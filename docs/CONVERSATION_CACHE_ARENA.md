# The conversation cache on disk: a mapped arena on the page-file volumes

Status: **design only**, nothing of it is implemented. The measurements below were taken on this PC (Windows 11,
RTX 5080, 62.6 GiB RAM, five Intel Optane M10 16 GB modules mounted at `C:\OptanePagefiles\{1..5}`, 12.4 GB
usable each), with the page files shrunk to 4 GiB each so 9.4 GB per volume was free. The tool was a ctypes
harness using `CreateFileW` with `FILE_FLAG_NO_BUFFERING` (and `FILE_FLAG_WRITE_THROUGH` for writes), 16 MiB
sequential blocks and 4 KiB random blocks; every temporary file was deleted afterwards.

## The problem it solves

Parked conversations (`--conversation-cache-mib`, the `SavedConversation` images in
`include/strata/core/conversation_cache.hpp`) are anonymous host memory. When RAM is short the OS pages them to
the page files (4 KiB random). That has three costs, all seen on this PC:

1. **They are charged to commit.** 9 parked conversations were 24 GB of commit against a 127.6 GiB limit; with the
   engine's own working set the box ran at 1.6 GiB free and 106.8 GiB committed. Two engine hangs followed an
   allocation failure under that pressure (`ExpertCache::fill_slot_queued: out of memory`,
   `prefill gr_write_norm_rs: out of memory`); each ended only by ending the engine.
2. **The engine cannot choose what stays in RAM.** The OS decides, competing with the model's pageable pack.
3. **They die with the engine.** A fatal error or a restart loses every parked conversation.

A mapped file owned by the engine fixes all three without changing the park cost: parking is already a copy
(`parked 342761 tokens in 1741 ms`), only its destination changes from anonymous pages to file-backed pages.

## Why not the session file (SAVE/RESTORE)

`docs/DETAILS.md` ("Session files") and `src/core/conversation_file.cpp` already save one conversation to a file
in 16 MiB aligned blocks, identity-checked (model + config), and can restore it after a restart of the same engine
version. But it is a **snapshot**: written whole, sequentially, on demand, one slot at a time. For a cache that
would mean

- 4-5 s of foreground time per 3.5 GB save on the five modules (806 MB/s aggregate) and 7.4 s for a 6 GB 342k
  conversation, on every park - the 342k/92k churn parks every 15-40 s;
- no way to save from a wedged engine (the SAVE command travels the same control pipe that is stuck).

So the session file stays what it is (an explicit save), and the cache gets its own storage.

## Why not RAID0

The five volumes are independently mounted (`C:\OptanePagefiles\{1..5}`). Striping in the engine gives the same
aggregate bandwidth (measured: 5 threads reach 806 MB/s write and 2.08 GB/s read, five times one volume, so the
devices are not the limit), keeps a single dead module from losing the whole cache, needs no disk management and
lets each file be sized and rolled back separately.

## Measured

| | one M10 | five in parallel | D: (2 TB NVMe, for reference) |
| --- | ---: | ---: | ---: |
| sequential write (16 MiB, write-through) | 169 MB/s | **806 MB/s** | 4,826 MB/s |
| sequential read (16 MiB) | 884 MB/s | **2,080 MB/s** | 6,175 MB/s |
| 4 KiB random read, QD1 | 29,866 IOPS (33.0 us) | - | 26,341 IOPS (37.5 us) |
| 4 KiB random read, QD8 | 152,749 IOPS | - | 155,704 IOPS |
| 4 KiB random write, QD1 | 19,665 IOPS (49.6 us) | - | 46,693 IOPS (20.4 us) |
| 4 KiB random write, QD4 | 39,511 IOPS | - | 116,428 IOPS |
| buffered write / fsync | 286 / 353 MB/s | - | 4,471 / 6,752 MB/s |

Two conclusions that shaped the design:

- **Five modules in parallel are linear** (806 / 2.08 GB/s), so striping is worth it and one volume would be the
  bottleneck.
- **The M10's 4 KiB advantage does not show here**: QD1 read matches the NVMe, QD8 matches it, and 4 KiB random
  write is 2.4x slower. The case for the Optanes is therefore **isolation, capacity and deliberate wear** (they
  are sacrificial), not latency. It also means the design should keep the traffic **coarse** (16 MiB segments,
  as `ConversationBuffer` already uses) rather than fine-grained random.

## Layout

One mapping file per volume, the segment size of `ConversationBuffer` (16 MiB) as the stripe unit:

```
C:\OptanePagefiles\1\strata-arena.bin  8.0 GB
C:\OptanePagefiles\2\strata-arena.bin  8.0 GB
C:\OptanePagefiles\3\strata-arena.bin  8.0 GB
C:\OptanePagefiles\4\strata-arena.bin  8.0 GB
C:\OptanePagefiles\5\strata-arena.bin  8.0 GB
```

- each file starts with a 1 MiB header: format version, `SessionFileIdentity`-style model + config hashes, an
  arena UUID and generation, the extent table (segment -> volume, offset, length, owner entry, checksum);
- segment `i` of a buffer lands on volume `i % 5`, so a park spreads over all five and a restore reads from all
  five; 16 MiB is large enough for NTFS read-ahead and small enough to balance;
- a volume that cannot be opened drops out of the stripe (the rest keep working); if none can be opened the cache
  falls back to today's anonymous-memory behaviour, with no change in semantics.

## Allocation, park, restore, evict

- **Allocator**: fixed 16 MiB extents, one free list per volume, first fit; `alloc(bytes)` returns a list of
  `{volume, offset, length}`. A conversation's `SavedConversation` is (live checkpoint + checkpoints + one
  `ConversationKv` per layer and the draft layer + `stage_images` for a layer split), each part holding five
  `ConversationBuffer`s, so the extent table is tens of entries, not thousands.
- **Park**: the existing copy is unchanged in size, only the destination changes -
  `ConversationBuffer::resize` allocates segments from the arena and the copy faults the pages in, dirtying 16 MiB
  ranges spread over the five volumes. The lazy writer drains them at up to 806 MB/s in the background (3.5 GB in
  about 4.5 s), so the request sees the same park cost as today.
- **Restore**: `visit()` reads the mapping; 16 MiB extents with NTFS read-ahead across five volumes reach about
  2 GB/s, so 3.5 GB is about 1.7 s - inside the 18-19 s the engine already spends rebuilding the pools.
- **Evict**: the LRU entry returns its extents to the free lists and the extent table is rewritten (a few KiB,
  sequential). Freed pages become clean and the OS drops them; nothing is charged to commit.

## Identity and adoption after a restart

The header carries the same identity the session files use (model + config hashes, `conversation_file.hpp`), plus
an arena UUID and generation. On start:

- identity matches -> **adopt**: map the files, rebuild each `SavedConversation` from its extent table and put it
  back in the cache (a new request matches it by its prompt-prefix hash, as today);
- identity differs (another PLE table, another `--kv`, another engine version) -> **wipe** the arena and start
  empty, which is the rule the session files already follow;
- a corrupt header or a failed mapping -> wipe that volume only.

A fatal error therefore no longer loses the parked conversations: the file is on disk and the next engine adopts
it. That is the same problem the session-file path solves, but without the foreground save and without needing a
live engine.

## Space budget

Per volume (12.4 GB usable, 13.0 GB reported):

| | size |
| --- | ---: |
| page file (fixed) | 4 GiB |
| arena file | 8 GB |
| NTFS slack | about 0.9 GB |
| total | about 13.0 GB |

Twenty GiB of page files now, 40 GB of arena. Measured commit on this PC: 16.9 GiB idle (service stopped),
58.0 GiB with the engine and its 24 GB parked set. With the parked set in the arena the 58 GiB no longer includes
it, so a 20 GiB page-file budget leaves about 12 GiB of headroom for the engine and the desktop at 82.6 GiB of
commit. To grow the arena to 47.5 GB the page files would go to 3 GiB each (77.6 GiB of commit, about 7 GiB of
headroom) - possible, but not the default. The arena holds 24 GB of parked conversations today; 40 GB leaves room
for 11-15 conversations of 2.5-3.5 GB.

## Code

| file | change |
| --- | --- |
| `include/strata/core/conversation_buffer.hpp` | a segment that can point into a mapping (`{uint8_t* p, size_t cap}`) instead of owning memory; `bytes()` and `allocation_peak()` account through the arena |
| `include/strata/core/conversation_arena.hpp`, `src/core/conversation_arena.cpp` (new) | the five mappings, the extent allocator, the header/table, identity, adoption, statistics |
| `include/strata/core/conversation_cache.hpp` | park/evict hooks: an entry's extents are taken at park and returned at evict |
| `src/program/generate.cpp` | `--conversation-cache-dir DIR` (arena on, default off) and `--conversation-cache-arena-gib N`; the park path hands the buffers the arena |
| `serve/server.py` | pass the two settings through; `/status` reports adopted entries, arena bytes free and evictions; one line at start |
| `CMakeLists.txt` | the new source file |
| tests | `conversation_arena_test.cpp` (alloc/free/stripe/identity/adopt/corruption) plus the existing `conversation_cache_test` |

The flag is off by default: without `--conversation-cache-dir` nothing changes, which is what the project's
rules ask of a new feature.

## Milestones

1. **M1 (1 day)** - `ConversationArena` alone: mappings, extents, header, identity, unit tests against five
   directories.
2. **M2 (1.5 days)** - `ConversationBuffer` allocation through the arena; park/restore behind the flag; the
   342k/92k churn for 20 minutes with no regression; `/status` shows the arena.
3. **M3 (1 day)** - adoption, wipe on identity change, eviction, statistics; kill the engine and restart it, the
   log says how many entries were adopted.
4. **M4 (1 day)** - the full test suite plus a measured comparison (park/restore time, throughput, commit with
   the parked set) at the same load.

## Failure modes

| event | behaviour |
| --- | --- |
| a volume missing or its file cannot be created | that volume leaves the stripe; all of them failing falls back to anonymous memory |
| corrupt header, wrong identity | wipe and start empty (a cache may be lost, it must never fail a request) |
| a fault fails while a request reads an entry | drop that entry and treat the request as a cache miss |
| the engine dies | the file stays, the next engine adopts it |
| memory pressure | the pages are file-backed, so the OS writes them back and drops them; commit does not grow |

## Open questions

- Whether a request may hold a pointer into the mapping while an eviction frees that extent (needs a reference
  count, or eviction only from the cache's own thread).
- Dirty-page throttling under a burst of parks: 3.5 GB of dirty 16 MiB ranges on a 20 GiB page-file budget
  should drain in about 4.5 s, but it needs measuring under the real churn.
- Whether `--conversation-cache-mib` becomes a soft RAM hint (the OS page cache decides) or stays the eviction
  budget; the design above keeps it as the eviction budget and lets the OS cache the clean pages.
- Layer splits: `SavedConversation::stage_images` are nested conversations and must go through the arena too.
