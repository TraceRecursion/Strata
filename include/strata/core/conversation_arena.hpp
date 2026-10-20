// The conversation cache's disk tier: a fixed set of mapped files, one per volume, carved into page-aligned
// extents that a parked conversation's K/V payloads are copied into.  See docs/CONVERSATION_CACHE_ARENA.md.
//
// Design notes:
//  * A parked image keeps its segmented K/V buffers on the heap while it is built (that is the existing snapshot
//    path, untouched).  Park then copies each payload into the arena and frees the heap copy, so the parked bytes
//    become file-backed pages: they no longer charge commit - the pressure behind two engine hangs - and the OS
//    may write them back and drop them without evicting the application.
//  * Restoring copies the payloads back into heap buffers before the existing restore path runs, so nothing
//    outside this file needs to know where they were.
//  * The volume rotates with every extent, so a park spreads its write-back over all five devices (measured:
//    806 MB/s write and 2.08 GB/s read with five volumes in parallel).
//  * The arena is off unless the engine is given the file list (--conversation-cache-dir); without it the cache
//    behaves exactly as before.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace strata::core {

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

    /// Opens (creating when needed) one file per path, maps it, and checks its header against `model_id` /
    /// `config_id` - a mismatched arena is wiped, because a cache may be lost but must never be misread.
    /// Returns nullptr and fills `err` when no volume can be mapped.
    static std::unique_ptr<ConversationArena> open(const std::vector<std::string>& paths, size_t file_bytes,
                                                   uint64_t model_id, uint64_t config_id, std::string& err);
    ~ConversationArena();
    ConversationArena(const ConversationArena&) = delete;
    ConversationArena& operator=(const ConversationArena&) = delete;

    /// A page-aligned contiguous extent on the next volume with room; nullptr when every volume is full
    /// (the cache treats that as a failed park, not as an engine error).  `out` describes the extent.
    void* allocate(size_t bytes, ArenaExtent& out);
    void* allocate(size_t bytes) { ArenaExtent unused; return allocate(bytes, unused); }
    /// The mapped address `extent` starts at (valid while the arena is open).
    uint8_t* pointer(const ArenaExtent& extent) const;
    /// Writes `extent` back to its volume now.  A park flushes each 16 MiB extent, so the dirty-page backlog
    /// stays bounded instead of flooding RAM and evicting the engine's own working set.
    void flush(const ArenaExtent& extent);
    /// Returns an extent obtained from allocate(); a pointer the arena does not know is ignored.
    void release(void* p);
    Stats stats() const;

private:
    ConversationArena();
    struct File;
    bool add_volume(const std::string& path, size_t file_bytes, std::string& err);
    void persist_table();
    std::vector<std::unique_ptr<File>> files_;
    std::vector<std::vector<ArenaExtent>> free_;      // per volume, sorted by offset
    std::vector<ArenaExtent> live_;                    // the extent table, persisted in volume 0's header
    std::vector<void*> live_ptrs_;                     // parallel to live_
    uint64_t model_id_ = 0, config_id_ = 0;
    size_t file_bytes_ = 0, cursor_ = 0;
    mutable std::recursive_mutex mu_;
};

}  // namespace strata::core
