// The conversation cache's disk tier: a fixed set of files, one per volume, carved into page-aligned extents that a
// parked conversation's K/V payloads are written into.  See docs/CONVERSATION_CACHE_ARENA.md.
//
// Design notes:
//  * A parked image keeps its segmented K/V buffers on the heap while a layer is built (that is the existing
//    snapshot path, untouched).  Each layer is then written into the arena and the heap copy freed, so the parked
//    bytes live in files: they no longer charge commit, and the OS may keep the model's own pages in RAM.
//  * The files are written and read with unbuffered I/O (FILE_FLAG_NO_BUFFERING, pread/pwrite elsewhere).  A
//    memory-mapped arena was tried first: its freshly touched pages won the OS's LRU over the model's pages, and the
//    request after a park read at 218 tok/s from the page files.  Unbuffered I/O keeps the parked bytes out of RAM.
//  * Restoring reads the payloads back into heap buffers before the existing restore path runs, so nothing outside
//    this file needs to know where they were.
//  * The volume rotates with every extent, so a park spreads its writes over all five devices (measured: 806 MB/s
//    for five in parallel, 250-430 MB/s while the page files on the same devices are busy).
//  * The arena is off unless the engine is given the file list (--conversation-cache-dir).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace strata::core {

/// The background park writer waits until this steady-clock time before it writes: the engine pushes it forward on
/// every request, so a park never competes with a request that arrives right after the previous one.
std::atomic<int64_t>& arena_quiet_until_ms();

struct ArenaExtent {
    uint32_t volume = 0;
    uint64_t offset = 0;
    uint64_t length = 0;
};

class ConversationArena {
public:
    struct Stats {
        size_t volumes = 0;
        size_t used_bytes = 0;
        size_t free_bytes = 0;
        size_t live_extents = 0;
    };

    /// Opens (creating when needed) one file per path and checks its header against `model_id` / `config_id` - a
    /// mismatched arena is wiped, because a cache may be lost but must never be misread.  Returns nullptr and fills
    /// `err` when no volume can be opened.
    static std::unique_ptr<ConversationArena> open(const std::vector<std::string>& paths, size_t file_bytes,
                                                   uint64_t model_id, uint64_t config_id, std::string& err);
    ~ConversationArena();
    ConversationArena(const ConversationArena&) = delete;
    ConversationArena& operator=(const ConversationArena&) = delete;

    /// A page-aligned contiguous extent on the next volume with room; false when every volume is full (the cache
    /// treats that as a failed park, not as an engine error).
    bool allocate(size_t bytes, ArenaExtent& out);
    /// Frees an extent obtained from allocate().
    void release_extent(const ArenaExtent& extent);
    /// Writes `bytes` of `data` into `extent` and reads it back, unbuffered.
    bool write(const ArenaExtent& extent, const uint8_t* data, size_t bytes);
    bool read(const ArenaExtent& extent, uint8_t* data, size_t bytes);
    /// Reads part of an extent with positioned I/O into a caller-owned bounce buffer: several threads may read
    /// the same volume at once (the serial read() takes the arena's one bounce buffer, so it cannot).
    bool read_positioned(const ArenaExtent& extent, uint64_t offset_in_extent, uint8_t* dst, size_t bytes,
                         uint8_t* bounce);
    /// A 16 MiB page-aligned bounce buffer for read_positioned; nullptr on failure.  free_bounce() releases it.
    static void* alloc_bounce();
    static void free_bounce(void* bounce);
    Stats stats() const;

private:
    ConversationArena();
    struct File;
    struct FileState {
        bool fail = false;
    };
    bool add_volume(const std::string& path, size_t file_bytes, std::string& err);
    bool read_raw(size_t volume, uint64_t offset, size_t bytes);
    bool write_raw(size_t volume, uint64_t offset, size_t bytes);
    void persist_table();
    std::vector<std::unique_ptr<File>> files_;
    std::vector<FileState> file_state_;
    std::vector<std::vector<ArenaExtent>> free_;      // per volume, sorted by offset
    std::vector<ArenaExtent> live_;                    // the extent table, persisted in volume 0's header
    void* bounce_ = nullptr;                           // one 16 MiB aligned chunk for the unbuffered I/O
    uint64_t model_id_ = 0, config_id_ = 0;
    size_t file_bytes_ = 0, cursor_ = 0;
    mutable std::recursive_mutex mu_;
};

}  // namespace strata::core
