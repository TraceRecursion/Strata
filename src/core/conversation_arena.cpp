// The mapped-file arena behind --conversation-cache-dir.  See include/strata/core/conversation_arena.hpp and
// docs/CONVERSATION_CACHE_ARENA.md.

#include "strata/core/conversation_arena.hpp"
#include "strata/core/conversation_cache.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace strata::core {
namespace {

constexpr uint64_t kMagic = 0x5354524154415241ull;   // "STRATAAR"
constexpr uint32_t kVersion = 1;
constexpr size_t kPage = 4096;
constexpr size_t kReserveHeader = 1024 * 1024;        // header + extent table, per volume

struct Header {
    uint64_t magic = kMagic;
    uint32_t version = kVersion;
    uint32_t volumes = 0;
    uint64_t model_id = 0;
    uint64_t config_id = 0;
    uint64_t extent_count = 0;
    uint64_t table_offset = 4096;
};

size_t round_up(size_t n, size_t unit) { return (n + unit - 1) / unit * unit; }

}  // namespace

struct ConversationArena::File {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
#else
    int fd = -1;
#endif
    void* base = nullptr;
    size_t bytes = 0;
};

ConversationArena::ConversationArena() = default;

ConversationArena::~ConversationArena() {
    for (auto& file : files_) {
        if (!file) continue;
#ifdef _WIN32
        if (file->base) UnmapViewOfFile(file->base);
        if (file->mapping) CloseHandle(file->mapping);
        if (file->handle != INVALID_HANDLE_VALUE) CloseHandle(file->handle);
#else
        if (file->base) munmap(file->base, file->bytes);
        if (file->fd >= 0) close(file->fd);
#endif
    }
}

bool ConversationArena::add_volume(const std::string& path, size_t file_bytes, std::string& err) {
    auto file = std::make_unique<File>();
    file->bytes = file_bytes;
#ifdef _WIN32
    std::wstring wide(path.begin(), path.end());
    file->handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file->handle == INVALID_HANDLE_VALUE) {
        err = "cannot open " + path;
        return false;
    }
    LARGE_INTEGER size;
    size.QuadPart = (LONGLONG) file_bytes;
    if (!SetFilePointerEx(file->handle, size, nullptr, FILE_BEGIN) || !SetEndOfFile(file->handle)) {
        err = "cannot size " + path;
        CloseHandle(file->handle);
        return false;
    }
    file->mapping = CreateFileMappingW(file->handle, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    if (!file->mapping) {
        err = "cannot map " + path;
        CloseHandle(file->handle);
        return false;
    }
    file->base = MapViewOfFile(file->mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!file->base) {
        err = "cannot view " + path;
        CloseHandle(file->mapping);
        CloseHandle(file->handle);
        return false;
    }
#else
    file->fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (file->fd < 0) {
        err = "cannot open " + path;
        return false;
    }
    if (ftruncate(file->fd, (off_t) file_bytes) != 0) {
        err = "cannot size " + path;
        close(file->fd);
        return false;
    }
    void* p = mmap(nullptr, file_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, file->fd, 0);
    if (p == MAP_FAILED) {
        err = "cannot map " + path;
        close(file->fd);
        return false;
    }
    file->base = p;
#endif
    files_.push_back(std::move(file));
    return true;
}

std::unique_ptr<ConversationArena> ConversationArena::open(const std::vector<std::string>& paths, size_t file_bytes,
                                                           uint64_t model_id, uint64_t config_id, std::string& err) {
    if (paths.empty() || file_bytes <= 2 * kReserveHeader) {
        err = "no arena volumes (or a file smaller than its header)";
        return nullptr;
    }
    std::unique_ptr<ConversationArena> arena(new ConversationArena());
    arena->file_bytes_ = file_bytes;
    arena->model_id_ = model_id;
    arena->config_id_ = config_id;
    std::string first_err;
    for (const auto& path : paths) {
        std::string why;
        if (!arena->add_volume(path, file_bytes, why)) {
            if (first_err.empty()) first_err = why;
            std::fprintf(stderr, "strata serve: conversation arena: %s (that volume is left out)\n", why.c_str());
            continue;
        }
    }
    if (arena->files_.empty()) {
        err = first_err.empty() ? "no arena volume could be mapped" : first_err;
        return nullptr;
    }
    // Identity: a header from another model/config (or another generation) is wiped, not read.
    Header* head = static_cast<Header*>(arena->files_[0]->base);
    const bool fresh = head->magic != kMagic || head->version != kVersion || head->model_id != model_id ||
                       head->config_id != config_id || head->volumes != arena->files_.size();
    if (fresh) {
        std::memset(head, 0, sizeof *head);
        head->magic = kMagic;
        head->version = kVersion;
        head->volumes = (uint32_t) arena->files_.size();
        head->model_id = model_id;
        head->config_id = config_id;
        head->table_offset = 4096;
        head->extent_count = 0;
        std::fprintf(stderr, "strata serve: conversation arena: new arena over %zu volume(s), %.1f GiB each\n",
                     arena->files_.size(), (double) file_bytes / (1024.0 * 1024.0 * 1024.0));
    } else {
        // A previous run's table.  There are no entry descriptors yet (M3), so that data cannot be used: reclaim
        // the extents instead of adopting them, which would leak a little of the arena on every restart.
        std::fprintf(stderr, "strata serve: conversation arena: reclaimed %llu extent(s) from a previous run\n",
                     (unsigned long long) head->extent_count);
        head->extent_count = 0;
    }
    // Every volume: the header at the front, the rest free.
    arena->free_.resize(arena->files_.size());
    for (size_t v = 0; v < arena->files_.size(); ++v)
        arena->free_[v].push_back(ArenaExtent{(uint32_t) v, (uint64_t) kReserveHeader, file_bytes - kReserveHeader});
    // Carve the adopted extents out of the free lists.
    for (const auto& e : arena->live_) {
        if (e.volume >= arena->free_.size()) continue;
        auto& list = arena->free_[e.volume];
        for (size_t i = 0; i < list.size(); ++i) {
            auto& f = list[i];
            if (e.offset < f.offset || e.offset + e.length > f.offset + f.length) continue;
            const uint64_t end = f.offset + f.length;
            const uint64_t tail_at = e.offset + e.length;
            ArenaExtent tail{ f.volume, tail_at, end - tail_at };
            f.length = e.offset - f.offset;
            std::vector<ArenaExtent> keep;
            if (f.length) keep.push_back(f);
            if (tail.length) keep.push_back(tail);
            list.erase(list.begin() + (std::ptrdiff_t) i);
            list.insert(list.begin() + (std::ptrdiff_t) i, keep.begin(), keep.end());
            break;
        }
    }
    for (auto& list : arena->free_)
        std::sort(list.begin(), list.end(), [](const ArenaExtent& a, const ArenaExtent& b) { return a.offset < b.offset; });
    return arena;
}

void* ConversationArena::allocate(size_t bytes, ArenaExtent& out) {
    if (bytes == 0) return nullptr;
    const size_t want = round_up(bytes, kPage);
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (files_.empty()) return nullptr;
    const size_t volumes = files_.size();
    for (size_t step = 0; step < volumes; ++step) {
        const size_t v = (cursor_ + step) % volumes;
        auto& list = free_[v];
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i].length < want) continue;
            ArenaExtent got = list[i];
            got.length = want;
            if (got.length < list[i].length) {
                list[i].offset += want;
                list[i].length -= want;
            } else {
                list.erase(list.begin() + (std::ptrdiff_t) i);
            }
            out = got;
            void* p = static_cast<uint8_t*>(files_[v]->base) + got.offset;
            live_.push_back(got);
            live_ptrs_.push_back(p);
            cursor_ = (v + 1) % volumes;
            persist_table();
            return p;
        }
    }
    return nullptr;   // full: the caller's park is skipped, never a failure
}

void ConversationArena::release(void* p) {
    if (p == nullptr) return;
    std::lock_guard<std::recursive_mutex> lock(mu_);
    for (size_t i = 0; i < live_ptrs_.size(); ++i) {
        if (live_ptrs_[i] != p) continue;
        ArenaExtent e = live_[i];
        live_.erase(live_.begin() + (std::ptrdiff_t) i);
        live_ptrs_.erase(live_ptrs_.begin() + (std::ptrdiff_t) i);
        if (e.volume < free_.size()) {
            auto& list = free_[e.volume];
            list.push_back(e);
            std::sort(list.begin(), list.end(), [](const ArenaExtent& a, const ArenaExtent& b) { return a.offset < b.offset; });
            // Coalesce neighbours so long runs come back for the next park.
            for (size_t k = 1; k < list.size();) {
                if (list[k - 1].offset + list[k - 1].length == list[k].offset) {
                    list[k - 1].length += list[k].length;
                    list.erase(list.begin() + (std::ptrdiff_t) k);
                } else {
                    ++k;
                }
            }
        }
        persist_table();
        return;
    }
}

void ConversationArena::persist_table() {
    if (files_.empty() || files_[0]->base == nullptr) return;
    auto* head = static_cast<Header*>(files_[0]->base);
    head->extent_count = (uint64_t) live_.size();
    const size_t capacity = (kReserveHeader - 4096) / sizeof(ArenaExtent);   // the table lives from 4096; the extents from kReserveHeader
    const size_t n = std::min(capacity, live_.size());
    auto* table = reinterpret_cast<ArenaExtent*>(static_cast<uint8_t*>(files_[0]->base) + head->table_offset);
    std::memcpy(table, live_.data(), n * sizeof(ArenaExtent));
}

ConversationArena::Stats ConversationArena::stats() const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    Stats out;
    out.volumes = files_.size();
    out.live_extents = live_.size();
    for (const auto& e : live_) out.used_bytes += e.length;
    for (const auto& list : free_)
        for (const auto& e : list) out.free_bytes += e.length;
    return out;
}


uint8_t* ConversationArena::pointer(const ArenaExtent& extent) const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (extent.volume >= files_.size() || files_[extent.volume]->base == nullptr) return nullptr;
    return static_cast<uint8_t*>(files_[extent.volume]->base) + extent.offset;
}

namespace {

using FiveBuffers = std::array<ConversationBuffer*, 5>;

FiveBuffers five(ConversationKv& kv) { return {&kv.k, &kv.v, &kv.k_scale, &kv.v_scale, &kv.pooled}; }

// One payload is copied between its buffer and its extents; the buffer spans at most 16 MiB segments, so an extent
// never straddles two of them.
constexpr size_t kChunk = 16 * 1024 * 1024;

void release_parts(ConversationKv& kv, ConversationArena& arena) {
    for (auto& part : kv.arena)
        for (const auto& extent : part.extents) arena.release(arena.pointer(extent));
    for (auto& part : kv.arena) { part.extents.clear(); part.size = 0; }
    kv.arena_bytes = 0;
}

}  // namespace

bool conversation_image_to_arena(SavedConversation& image, ConversationArena& arena, std::string& error) {
    for (auto& kv : image.kv)
        if (!conversation_kv_to_arena(kv, arena, error)) return false;
    for (auto& stage : image.stage_images)
        if (!conversation_image_to_arena(stage, arena, error)) return false;
    return true;
}

bool conversation_image_from_arena(SavedConversation& image, ConversationArena& arena, std::string& error) {
    for (auto& kv : image.kv)
        if (!conversation_kv_from_arena(kv, arena, error)) return false;
    for (auto& stage : image.stage_images)
        if (!conversation_image_from_arena(stage, arena, error)) return false;
    return true;
}

void conversation_image_release_arena(SavedConversation& image, ConversationArena& arena) {
    for (auto& kv : image.kv) release_parts(kv, arena);
    for (auto& stage : image.stage_images) conversation_image_release_arena(stage, arena);
}

bool conversation_kv_to_arena(ConversationKv& kv, ConversationArena& arena, std::string& error) {
    if (kv.arena_bytes != 0) return true;   // already there
    FiveBuffers buffers = five(kv);
    // All the extents first: a full arena leaves the heap image untouched (the park is then simply refused).
    std::array<ConversationKv::ArenaPart, 5> parts;
    for (size_t i = 0; i < 5; ++i) {
        const size_t n = buffers[i]->size();
        parts[i].size = n;
        for (size_t done = 0; done < n; done += kChunk) {
            ArenaExtent extent;
            if (arena.allocate(std::min(kChunk, n - done), extent) == nullptr) {
                for (auto& part : parts)
                    for (const auto& e : part.extents) arena.release(arena.pointer(e));
                error = "the arena is full";
                return false;
            }
            parts[i].extents.push_back(extent);
        }
    }
    // Then copy each payload in and drop the heap copy.
    for (size_t i = 0; i < 5; ++i) {
        size_t done = 0;
        for (const auto& extent : parts[i].extents) {
            const size_t n = std::min((size_t) extent.length, parts[i].size - done);   // the last extent rounds up to a page
            if (!buffers[i]->read(arena.pointer(extent), done, n)) {
                release_parts(kv, arena);
                error = "the snapshot buffer is shorter than its layout";
                return false;
            }
            done += n;
        }
        *buffers[i] = ConversationBuffer();
    }
    kv.arena = std::move(parts);
    kv.arena_bytes = 0;
    for (const auto& part : kv.arena) kv.arena_bytes += part.size;
    return true;
}

bool conversation_kv_from_arena(ConversationKv& kv, ConversationArena& arena, std::string& error) {
    if (kv.arena_bytes == 0) return true;
    FiveBuffers buffers = five(kv);
    for (size_t i = 0; i < 5; ++i) {
        const size_t total = kv.arena[i].size;
        if (total == 0) { buffers[i]->resize(0); continue; }
        buffers[i]->resize(total, 0);
        size_t done = 0;
        for (const auto& extent : kv.arena[i].extents) {
            const uint8_t* src = arena.pointer(extent);
            const size_t n = std::min((size_t) extent.length, total - done);
            buffers[i]->visit(done, n, [&](uint8_t* p, size_t len, size_t at) {
                std::memcpy(p, src + (at - done), len);
                return true;
            });
            done += n;
        }
    }
    release_parts(kv, arena);
    return true;
}

void conversation_kv_release_arena(ConversationKv& kv, ConversationArena& arena) { release_parts(kv, arena); }}  // namespace strata::core
