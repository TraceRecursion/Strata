// The conversation cache's disk tier: a fixed set of mapped files, one per volume, carved into page-aligned
// extents that parked conversations' segment buffers are allocated from.  See docs/CONVERSATION_CACHE_ARENA.md.
//
// Design notes:
//  * A segment buffer (ConversationBuffer) is a vector of up to 16 MiB segments.  An arena-backed segment is one
//    contiguous extent on one of the volumes, and the volume rotates with each allocation, so a parked image
//    spreads its write-back over every device (measured 806 MB/s write and 2.08 GB/s read with five volumes).
//  * The pages are file-backed, so a parked conversation no longer charges commit - that was the pressure behind
//    two engine hangs - and the OS may write them back and drop them without evicting the application.
//  * The arena is off unless the engine is given a directory (--conversation-cache-dir); without it every
//    allocation goes to the heap exactly as before.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
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
    /// (the caller's caller treats that as a failed park, not as an engine error).
    void* allocate(size_t bytes);
    /// Returns an extent obtained from allocate(); a pointer the arena does not know is ignored.
    void release(void* p);
    Stats stats() const;

    /// The arena segment allocations go to, as a thread-local: the park path sets it for the duration of one
    /// snapshot, so the parked image's buffers come from the arena while everything else keeps the heap.
    static ConversationArena* current();
    static ConversationArena* set_current(ConversationArena* arena);

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

public:
    // The allocator std::vector uses for a ConversationBuffer segment.
    template <class T> class Allocator;
};

/// RAII: sets the arena segment allocations go to for a scope (the park path), restoring the previous one.
class ArenaAllocatorScope {
public:
    explicit ArenaAllocatorScope(ConversationArena* arena) : previous_(ConversationArena::set_current(arena)) {}
    ~ArenaAllocatorScope() { ConversationArena::set_current(previous_); }
    ArenaAllocatorScope(const ArenaAllocatorScope&) = delete;
    ArenaAllocatorScope& operator=(const ArenaAllocatorScope&) = delete;
private:
    ConversationArena* previous_;
};

template <class T>
class ConversationArena::Allocator {
public:
    using value_type = T;
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;

    Allocator() noexcept : arena_(ConversationArena::current()) {}
    template <class U> Allocator(const Allocator<U>& other) noexcept : arena_(other.arena_) {}

    T* allocate(std::size_t n) {
        if (n > SIZE_MAX / sizeof(T)) throw std::bad_alloc();
        if (arena_ == nullptr) return static_cast<T*>(::operator new(n * sizeof(T)));
        void* p = arena_->allocate(n * sizeof(T));
        if (p == nullptr) throw std::bad_alloc();   // the park path catches it and skips parking
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept {
        if (arena_ == nullptr) ::operator delete(p);
        else arena_->release(p);
    }
    ConversationArena* arena() const noexcept { return arena_; }
    bool operator==(const Allocator& other) const noexcept { return arena_ == other.arena_; }
    bool operator!=(const Allocator& other) const noexcept { return !(*this == other); }

private:
    template <class U> friend class Allocator;
    ConversationArena* arena_ = nullptr;
};

}  // namespace strata::core
