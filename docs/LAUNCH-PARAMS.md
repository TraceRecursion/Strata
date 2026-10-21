# The live launch parameters (512k, BF16 PLE table, Optane arena)

The configuration `strata-qwen3.8-flash-next-512k-int8-plebf16-arena.json` in the repository root, launched by
`run-qwen3.8-flash-next-...-arena‑solo.bat`.  Every value below was measured on this PC; the numbers are from this
session's logs unless a note says otherwise.

## Model and pack

| argument | value | why |
| --- | --- | --- |
| `--pack` | `Strata-data\packs\iq3_s` | the experts pack (IQ3_S, about 46.84 GiB loaded into RAM) |
| `--native` | `IQ3_S\...-00001-of-00002.gguf` | the dense/attention shards |
| `--ple-gguf` | `models\ple-bf16.gguf` | the checkpoint's own PLE table, bit for bit (95.37 GiB; IQ4_NL's is 8% off per row) |
| `--ple-io mmap` | | the rows come from the OS file cache instead of unbuffered reads: 122614 rows/s against 4523 rows/s for the BF16 table read `direct` |
| `--tokenizer`, `--mtp`, `--spec 4`, `--spec-min-p 0.5` | | the drafter and its speculative window |
| `--control-vector-scaled`, `--control-vector-layer-range 4 44`, `--cvec-mode project`, `--cvec-dir per-layer` | | the control vector, as tuned earlier |

## Context and KV

| argument | value | why |
| --- | --- | --- |
| `--max-context` | **524288** | 512k.  1M loads (KV doubles to about 12.4 GiB) but measures **3,326 -> 1,085 prompt tok/s and 103.8 -> 49.2 decode tok/s**, i.e. a third of the speed: the memory pressure, not the compute, is the limit |
| `--rope-scaling yarn`, `--rope-scale 2`, `--yarn-orig-ctx 262144` | | the extension the 512k point was tuned with |
| `--kv int8`, `--kv-resident 65536` | | int8 K/V (1,056 B/cell against 528 for q4_0; the quality is the reason int8 stays); 65536 of 524288 cells per layer in VRAM, the K/V in 6.19 GiB of pinned RAM |
| `--vision` | | remote vision (the G15's 3060): 2.8 s for a 1024-token picture against 15.3 s on this processor |

## Prefill and VRAM

| argument | value | why |
| --- | --- | --- |
| `--prefill auto` | 8192-token chunks | 3,300 tok/s prompt on a 257k read; a smaller chunk borrows fewer cache slots, a larger one stalls under WDDM |
| `--vram-reserve-mib 700` | | leaves the verify windows their buffers (325 MiB free at load; 279-325 MiB measured during window capture) |
| `--expert-cache auto`, `--expert-profile` | | the resident expert set (3,629-3,630 slots, 6.94 GiB of VRAM) |

## Conversation cache on the Optane arena

| argument | value | why |
| --- | --- | --- |
| `--conversation-cache-mib` | **8192** | the RAM budget: the checkpoint metadata plus the materialized transient of a restore.  It must cover the biggest conversation's restore (`take()` copies its payload back into RAM), 4096 refused a 257k-token image next to a taken one |
| `--conversation-cache-slots` | 1000 | |
| `--conversation-cache-admit commit` | | the admission gate reads the commit headroom (RAM plus page files), not `MemAvailable`: the engine's hot host memory is locked |
| `--conversation-cache-min-free-mib` | 2560 | the floor the gate keeps |
| `--conversation-cache-min-tokens` | absent (0) | park every conversation |
| `--conversation-cache-park-gain` | **2048** | park only after the conversation advanced this far past the parked copy: the eager park (after `DONE`) does not rebuild the snapshot on every turn |
| `--conversation-cache-dir` | `C:\OptanePagefiles\{2..5}\strata-arena.bin` | one file per Optane volume: the parked K/V and checkpoint arrays live there, unbuffered, so they charge no commit and stay out of the file cache |
| `--conversation-cache-arena-gib` | **7** | 7 GiB per file, 28 GiB in total (the volumes also hold a 4.5 GiB page file each and keep about 1 GiB free) |

## Why the disks are split this way

Disk 1 has its own PCIe 3.0 link (873 MB/s sequential read, 30k IOPS at QD1); disks 2-5 share one adapter card and
measure **1,588 MB/s together** (the card is the limit; five together are 1,983 MB/s).  The page files therefore live
mostly on disk 1 (12 GiB there, 4.5 GiB on each of 2-5, 30 GiB in total, commit limit 92.6 GiB) and the arena on 2-5:
the paging's 4K random traffic keeps the dedicated link, and the arena's bulk writes and parallel restores use the
shared card, where they are the card's limit anyway (a 2.85 GB restore reads at 1.67-1.98 GB/s).

## Measured on this configuration

| | |
| --- | --- |
| prompt processing | 3,326 tok/s at 260k tokens, 3,047 at 463k |
| decode | 103.8 tok/s at 260k, 101.2 at 463k |
| a 257k conversation back from the cache | restore 1.7-1.9 s (2.85 GB), 7.3 s for the request against 78 s for a re-read |
| a short conversation back from the cache | restore 87-124 ms, about 0.6 s for the request |
| the park behind a 257k conversation | build 3.2-4.3 s (in the client's idle window, after `DONE`), arena write 9.6 s in the background |
