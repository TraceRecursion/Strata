# tools/remote-vision/update-remote-vision.ps1
#
# One command on the 5080 that brings BOTH machines up to date:
#   1. git pull the Strata checkout (unless -SkipPull)
#   2. rebuild the wrapper exe and regenerate the vocabulary stub (unless -SkipBuild)
#   3. push tools/vision, third_party/llama.cpp, the stub and the watchdog to the vision host
#   4. build strata-vision there (CUDA) and smoke test the READY handshake
#
# The llama.cpp revision is never chosen here: third_party/llama.cpp lives inside the Strata
# checkout, so what gets pushed is by construction the revision the engine was built against.
#
#   .\update-remote-vision.ps1                      full update
#   .\update-remote-vision.ps1 -SkipPull            only re-push/build (sources already current)
#   .\update-remote-vision.ps1 -Host dell-g15-5511  another vision host
#   .\update-remote-vision.ps1 -Arch 86             CUDA arch of the vision host's GPU
param(
    [string]$VisionHost = 'dell-g15-5511',
    [string]$RemoteRoot = '~/Developer/strata-vision',
    [string]$Arch       = '86',
    [switch]$SkipPull,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Resolve-Path (Join-Path $Here '..\..')
$Py   = Join-Path $Root '.venv\Scripts\python.exe'
if (-not (Test-Path $Py)) { $Py = 'python' }

$Model  = 'D:\TraceRecursion\Developer\Strata-data\models\IQ3_S\Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf'
$Mmproj  = 'D:\TraceRecursion\Developer\Strata-data\models\mmproj-Qwen3.8-Flash-Next-BF16.gguf'

function Step($n, $text) { Write-Host ""; Write-Host "=== $n. $text ===" -ForegroundColor Cyan }
function Ok($text)       { Write-Host "    $text" -ForegroundColor Green }

function Invoke-Remote([string]$Command) {
    $out = ssh -o BatchMode=yes $VisionHost $Command 2>&1
    if ($LASTEXITCODE -ne 0) { throw "remote command failed ($LASTEXITCODE): $Command`n$out" }
    return $out
}

function Send-File([string]$Local, [string]$RemotePath) {
    # cmd.exe owns the redirect: PowerShell's `<` does not exist
    cmd /c "ssh -o BatchMode=yes $VisionHost `"cat > $RemotePath`" < `"$Local`"" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "sending $Local failed" }
}

function Send-Tar([string]$LocalDir, [string]$Member, [string]$RemoteDir) {
    $tgz = Join-Path $env:TEMP "strata-$Member.tgz"
    & tar.exe -czf $tgz -C $LocalDir $Member
    $mb = [math]::Round((Get-Item $tgz).Length / 1MB, 1)
    cmd /c "ssh -o BatchMode=yes $VisionHost `"tar -xzf - -C $RemoteDir`" < `"$tgz`"" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "pushing $Member failed" }
    Remove-Item $tgz -ErrorAction SilentlyContinue
    Ok "$Member  ($mb MB)"
}

# ---------------------------------------------------------------------------
Step 0 'Reachability'
Invoke-Remote 'echo ok' | Out-Null
$gpu = Invoke-Remote "nvidia-smi --query-gpu=name,memory.total,compute_cap --format=csv,noheader"
Ok "vision host: $VisionHost -> $gpu"

# ---------------------------------------------------------------------------
Step 1 'Update the Strata checkout'
if ($SkipPull) { Ok 'skipped (-SkipPull)' }
else {
    Push-Location $Root
    try {
        $before = (git rev-parse HEAD).Trim()
        git pull --ff-only
        $after = (git rev-parse HEAD).Trim()
        if ($before -eq $after) { Ok "already at $($after.Substring(0,7))" }
        else { Ok "moved $($before.Substring(0,7)) -> $($after.Substring(0,7))" }
        Ok "engine binary: $((Get-Content (Join-Path $Root 'engine\BUILD.json') -Raw | ConvertFrom-Json).version)"
    } finally { Pop-Location }
}

# ---------------------------------------------------------------------------
Step 2 'The vocabulary stub'
# The wrapper itself needs no build: the server runs strata_vision_remote.py through
# strata-vision-remote.cmd with the venv's Python.  Only the stub is generated here.
if ($SkipBuild) { Ok 'skipped (-SkipBuild)' }
else {
    & $Py (Join-Path $Here 'make_vocab_stub.py') $Model (Join-Path $Here 'text-vocab-stub.gguf') |
        Select-Object -Last 1 | ForEach-Object { Ok $_ }
}

# ---------------------------------------------------------------------------
Step 3 'Push sources to the vision host'
Invoke-Remote "mkdir -p $RemoteRoot/tools $RemoteRoot/third_party/llama.cpp $RemoteRoot/models" | Out-Null
Send-Tar (Join-Path $Root 'tools') 'vision' "$RemoteRoot/tools"
Send-Tar (Join-Path $Root 'third_party') 'llama.cpp' "$RemoteRoot/third_party"
Send-File (Join-Path $Here 'text-vocab-stub.gguf') "$RemoteRoot/models/text-vocab-stub.gguf"
Ok 'vocab stub (10.5 MB)'

$sh = (Get-Content (Join-Path $Here 'remote-vision-watchdog.sh') -Raw) -replace "`r`n", "`n"
[System.IO.File]::WriteAllText("$env:TEMP\strata-watchdog.sh", $sh)
Send-File "$env:TEMP\strata-watchdog.sh" "$RemoteRoot/remote-vision-watchdog.sh"
Invoke-Remote "chmod +x $RemoteRoot/remote-vision-watchdog.sh" | Out-Null
Ok 'watchdog'

$sh = (Get-Content (Join-Path $Here 'remote-build.sh') -Raw) -replace "`r`n", "`n"
[System.IO.File]::WriteAllText("$env:TEMP\strata-remote-build.sh", $sh)
Send-File "$env:TEMP\strata-remote-build.sh" "$RemoteRoot/remote-build.sh"
Invoke-Remote "chmod +x $RemoteRoot/remote-build.sh" | Out-Null
Ok 'build script'

# ---------------------------------------------------------------------------
Step 4 'The mmproj file and the build'
$have = (Invoke-Remote "ls $RemoteRoot/mmproj-*.gguf 2>/dev/null | head -1").Trim()
if ($have) {
    Ok "mmproj already on the vision host: $(Split-Path $have -Leaf)"
} else {
    Write-Host "    sending mmproj (0.85 GB, one time) ..."
    Send-Tar (Split-Path $Mmproj) (Split-Path $Mmproj -Leaf) $RemoteRoot
}

$log = Invoke-Remote "cd $RemoteRoot && STRATA_VISION_ARCH=$Arch ./remote-build.sh 2>&1 | tail -25"
$log | ForEach-Object { Write-Host "    $_" }
# $log is a string array; -notmatch on an array returns the matching lines, not a boolean
if (([string]::Join("`n", $log)) -notmatch 'READY') {
    throw "the remote build did not report READY - see the output above"
}
Ok 'vision host rebuilt and smoking-tested'

# ---------------------------------------------------------------------------
Step 5 'Done'
$ver = (Get-Content (Join-Path $Root 'engine\BUILD.json') -Raw | ConvertFrom-Json).version
Write-Host ""
Write-Host "Both machines are up to date:" -ForegroundColor Green
Write-Host ("  server : engine {0} + wrapper tools\remote-vision\strata-vision-remote.cmd" -f $ver)
Write-Host ("  vision : {0}/build/bin/strata-vision  (CUDA arch {1})" -f $RemoteRoot, $Arch)
Write-Host ""
Write-Host "If the model server is running, restart it to pick up the new encoder:"
Write-Host "  run-<model>-512k-int8-yarn-vision-remote-esp-greedy.bat"
