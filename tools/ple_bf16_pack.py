"""tools/ple_bf16_pack.py - the PLE n-gram table exactly as Qwen's checkpoint ships it (BF16), as a GGUF the engine reads.

    python tools/ple_bf16_pack.py --model <checkpoint dir> --out <ple-bf16.gguf>
    python tools/ple_bf16_pack.py --model <checkpoint dir> --out x --dry-run     # geometry only, writes nothing

Qwen3.8-Flash-Next's checkpoint stores the table as 128 shards `...ngram_embedding.shard_{k}.weight` of
[2500012, 160] **BF16**: 320,001,536 rows of 160 values, 102.4 GB, no scale tensor. ISTA-DASLab's GGUF ships the
same table as IQ4_NL (90 B/row, 28.8 GB), which is 8% off these values per row (tools/ple_fp8_pack.py, which packs
the FP8 E4M3 release of the same table); this keeps the checkpoint's own values bit for bit.

The engine resolves a PLE table's format from the TENSOR's GGUF type (src/kernels/ngram.cpp, `kPleFormats`):
BF16 is an entry of its own, 320-byte rows, `needs_scale` false, so a BF16 table needs no `strata.ple.format` or
`strata.ple.scale` of its own - only the type and `general.architecture` = "strata-ple", which is how the engine
tells a PLE-only table from a model shard (src/program/generate.cpp: the PLE-only file is not added to the dense
shards).

The output is a one-tensor GGUF: `per_layer_token_embd.weight`, ne = [160, 320001536], type BF16. The bytes are
copied straight out of the safetensors files in shard order - nothing is decoded or rounded - so memory stays flat
(64 MB chunks).
"""
import argparse
import json
import pathlib
import struct
import sys

GGUF_TYPE_STRING = 8
GGML_TYPE_BF16 = 30
ALIGN = 32
CHUNK = 64 << 20
BYTES_PER_VALUE = 2                                       # BF16


def safetensors_header(path):
    with open(path, "rb") as f:
        n = struct.unpack("<Q", f.read(8))[0]
        return json.loads(f.read(n)), 8 + n


def gguf_string(s):
    b = s.encode("utf-8")
    return struct.pack("<Q", len(b)) + b


def kv_string(key, val):
    return gguf_string(key) + struct.pack("<I", GGUF_TYPE_STRING) + gguf_string(val)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--model", required=True, help="the checkpoint directory (model.safetensors.index.json)")
    ap.add_argument("--out", required=True, help="the one-tensor GGUF to write")
    ap.add_argument("--dry-run", action="store_true", help="print the geometry and exit; writes nothing")
    a = ap.parse_args()
    model, out = pathlib.Path(a.model), pathlib.Path(a.out)
    wmap = json.loads((model / "model.safetensors.index.json").read_text(encoding="utf-8"))["weight_map"]

    shards = {}
    for name in wmap:
        if ".ngram_embedding.shard_" in name and name.endswith(".weight"):
            shards[int(name.rsplit(".shard_", 1)[1].split(".")[0])] = name
    if not shards:
        sys.exit("no ngram_embedding shards in " + str(model))
    if sorted(shards) != list(range(len(shards))):
        sys.exit("the n-gram shards are not numbered 0..%d without gaps" % (len(shards) - 1))

    parts, rows, dim = [], 0, None
    for k in range(len(shards)):
        name = shards[k]
        path = model / wmap[name]
        if not path.exists():
            sys.exit("%s is missing (%s)" % (path, name))
        hdr, base = safetensors_header(path)
        t = hdr[name]
        span = t["data_offsets"][1] - t["data_offsets"][0]
        if t["dtype"] != "BF16" or len(t["shape"]) != 2:
            sys.exit("%s is %s %s, not a 2-D BF16 tensor" % (name, t["dtype"], t["shape"]))
        if dim is None:
            dim = t["shape"][1]
        if t["shape"][1] != dim or span != t["shape"][0] * dim * BYTES_PER_VALUE:
            sys.exit("%s: inconsistent shape %s (span %d)" % (name, t["shape"], span))
        parts.append((path, base + t["data_offsets"][0], span))
        rows += t["shape"][0]
    payload = rows * dim * BYTES_PER_VALUE
    print("%d shards, %d rows x %d BF16, %.2f GB, in %d safetensors files"
          % (len(parts), rows, dim, payload / 1e9, len({p for p, _, _ in parts})), flush=True)
    if a.dry_run:
        for k in range(0, len(parts), max(1, len(parts) // 8)):
            print("  shard %3d: %s +%d" % (k, parts[k][0].name, parts[k][1]))
        print("dry run: nothing written")
        return

    kvs = [kv_string("general.architecture", "strata-ple"),
           kv_string("general.name", "PLE n-gram table, BF16 as the checkpoint ships it"),
           kv_string("strata.ple.format", "BF16"),
           kv_string("strata.ple.source", model.name)]
    head = b"GGUF" + struct.pack("<IQQ", 3, 1, len(kvs)) + b"".join(kvs)
    head += gguf_string("per_layer_token_embd.weight") + struct.pack("<I", 2) + struct.pack("<QQ", dim, rows)
    head += struct.pack("<I", GGML_TYPE_BF16) + struct.pack("<Q", 0)
    head += b"\0" * ((-len(head)) % ALIGN)

    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(out.suffix + ".part")
    written = 0
    with open(tmp, "wb") as w:
        w.write(head)
        for i, (path, off, n) in enumerate(parts):
            with open(path, "rb") as f:
                f.seek(off)
                left = n
                while left:
                    b = f.read(min(CHUNK, left))
                    if not b:
                        sys.exit("short read in " + str(path))
                    w.write(b)
                    left -= len(b)
            written += n
            if i % 16 == 15 or i == len(parts) - 1:
                print("  %3d/%d shards, %.1f GB" % (i + 1, len(parts), written / 1e9), flush=True)
    if tmp.stat().st_size != len(head) + payload:
        sys.exit("size check failed: %d != %d" % (tmp.stat().st_size, len(head) + payload))
    tmp.replace(out)
    print("wrote %s (%d rows x %d BF16, header %d B, %.2f GB)"
          % (out, rows, dim, len(head), (len(head) + payload) / 1e9))


if __name__ == "__main__":
    main()
