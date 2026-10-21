// The conversation cache's disk tier behind --conversation-cache-dir.  See conversation_arena.hpp.  The files are
// written and read unbuffered: a parked payload must not sit in the file cache, where its freshly touched pages win
// the OS's LRU over the model's own pages.

#include "strata/core/conversation_arena.hpp"
#include "strata/core/conversation_cache.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <atomic>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace strata::core {
namespace {

constexpr uint64_t kMagic = 0x5354524154415241ull;   // "STRATAAR"
constexpr uint32_t kVersion = 2;                     // 2: unbuffered files, no mapping
constexpr size_t kPage = 4096;
constexpr size_t kReserveHeader = 1024 * 1024;
constexpr size_t kBounce = 16 * 1024 * 1024;

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

std::atomic<int64_t>& arena_quiet_until_ms() {
    static std::atomic<int64_t> until{0};
    return until;
}

struct ConversationArena::File {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
    size_t bytes = 0;
};

ConversationArena::ConversationArena() {
#ifdef _WIN32
    bounce_ = VirtualAlloc(nullptr, kBounce, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    bounce_ = std::aligned_alloc(kPage, kBounce);
#endif
}

ConversationArena::~ConversationArena() {
    for (auto& file : files_) {
        if (!file) continue;
#ifdef _WIN32
        if (file->handle != INVALID_HANDLE_VALUE) CloseHandle(file->handle);
#else
        if (file->fd >= 0) close(file->fd);
#endif
    }
#ifdef _WIN32
    if (bounce_) VirtualFree(bounce_, 0, MEM_RELEASE);
#else
    std::free(bounce_);
#endif
}

bool ConversationArena::add_volume(const std::string& path, size_t file_bytes, std::string& err) {
    auto file = std::make_unique<File>();
    file->bytes = file_bytes;
#ifdef _WIN32
    std::wstring wide(path.begin(), path.end());
    file->handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING, nullptr);
    if (file->handle == INVALID_HANDLE_VALUE) { err = "cannot open " + path; return false; }
    LARGE_INTEGER size;
    size.QuadPart = (LONGLONG) file_bytes;
    if (!SetFilePointerEx(file->handle, size, nullptr, FILE_BEGIN) || !SetEndOfFile(file->handle)) {
        err = "cannot size " + path;
        CloseHandle(file->handle);
        return false;
    }
#else
    file->fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (file->fd < 0 || ftruncate(file->fd, (off_t) file_bytes) != 0) {
        err = "cannot open/size " + path;
        if (file->fd >= 0) close(file->fd);
        return false;
    }
#endif
    files_.push_back(std::move(file));
    return true;
}

bool ConversationArena::read_raw(size_t volume, uint64_t offset, size_t bytes) {
    if (bounce_ == nullptr || volume >= files_.size() || bytes > kBounce || file_state_[volume].fail) return false;
#ifdef _WIN32
    LARGE_INTEGER at;
    at.QuadPart = (LONGLONG) offset;
    if (!SetFilePointerEx(files_[volume]->handle, at, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    if (!ReadFile(files_[volume]->handle, bounce_, (DWORD) bytes, &got, nullptr) || got != bytes) {
        file_state_[volume].fail = true;
        return false;
    }
#else
    if (pread(files_[volume]->fd, bounce_, bytes, (off_t) offset) != (ssize_t) bytes) return false;
#endif
    return true;
}

bool ConversationArena::write_raw(size_t volume, uint64_t offset, size_t bytes) {
    if (bounce_ == nullptr || volume >= files_.size() || bytes > kBounce || file_state_[volume].fail) return false;
#ifdef _WIN32
    LARGE_INTEGER at;
    at.QuadPart = (LONGLONG) offset;
    if (!SetFilePointerEx(files_[volume]->handle, at, nullptr, FILE_BEGIN)) return false;
    DWORD wrote = 0;
    if (!WriteFile(files_[volume]->handle, bounce_, (DWORD) bytes, &wrote, nullptr) || wrote != bytes) {
        file_state_[volume].fail = true;
        return false;
    }
#else
    if (pwrite(files_[volume]->fd, bounce_, bytes, (off_t) offset) != (ssize_t) bytes) return false;
#endif
    return true;
}

bool ConversationArena::write(const ArenaExtent& extent, const uint8_t* data, size_t bytes) {
    if (data == nullptr || extent.length == 0 || bytes > extent.length) return false;
    std::lock_guard<std::recursive_mutex> lock(mu_);
    // One bounce chunk at a time; a payload may be larger than the chunk.
    for (size_t done = 0; done < bytes; done += std::min(bytes - done, kBounce)) {
        const size_t n = std::min(bytes - done, kBounce);
        std::memcpy(bounce_, data + done, n);
        const size_t len = round_up(n, kPage);   // the device wants whole sectors; the tail is padding
        std::memset(static_cast<uint8_t*>(bounce_) + n, 0, len - n);
        if (!write_raw(extent.volume, extent.offset + done, len)) return false;
    }
    return true;
}

void* ConversationArena::alloc_bounce() {
#ifdef _WIN32
    return _aligned_malloc(kBounce, kPage);
#else
    return std::aligned_alloc(kPage, kBounce);
#endif
}

void ConversationArena::free_bounce(void* bounce) {
    if (bounce == nullptr) return;
#ifdef _WIN32
    _aligned_free(bounce);
#else
    std::free(bounce);
#endif
}

bool ConversationArena::write_positioned(const ArenaExtent& extent, uint64_t offset_in_extent, const uint8_t* src,
                                         size_t bytes, uint8_t* bounce) {
    if (bounce == nullptr || src == nullptr || bytes == 0 || offset_in_extent + bytes > extent.length) return false;
    if (extent.volume >= files_.size() || file_state_[extent.volume].fail) return false;
    const uint64_t at = extent.offset + offset_in_extent;
    for (size_t done = 0; done < bytes; done += std::min(bytes - done, kBounce)) {
        const size_t n = std::min(bytes - done, kBounce);
        const size_t len = std::min(round_up(n, kPage), (size_t) (extent.length - offset_in_extent - done));
        std::memcpy(bounce, src + done, n);
        std::memset(bounce + n, 0, len - n);
#ifdef _WIN32
        OVERLAPPED ov{};
        ov.Offset = (DWORD) ((at + done) & 0xFFFFFFFFull);
        ov.OffsetHigh = (DWORD) ((at + done) >> 32);
        DWORD wrote = 0;
        if (!WriteFile(files_[extent.volume]->handle, bounce, (DWORD) len, &wrote, &ov) || wrote != len) return false;
#else
        if (pwrite(files_[extent.volume]->fd, bounce, len, (off_t) (at + done)) != (ssize_t) len) return false;
#endif
    }
    return true;
}

bool ConversationArena::read_positioned(const ArenaExtent& extent, uint64_t offset_in_extent, uint8_t* dst,
                                        size_t bytes, uint8_t* bounce) {
    if (bounce == nullptr || dst == nullptr || bytes == 0 || offset_in_extent + bytes > extent.length) return false;
    if (extent.volume >= files_.size() || file_state_[extent.volume].fail) return false;
    const uint64_t at = extent.offset + offset_in_extent;
    for (size_t done = 0; done < bytes; done += std::min(bytes - done, kBounce)) {
        const size_t n = std::min(bytes - done, kBounce);
        const size_t len = round_up(n, kPage);
#ifdef _WIN32
        OVERLAPPED ov{};
        ov.Offset = (DWORD) ((at + done) & 0xFFFFFFFFull);
        ov.OffsetHigh = (DWORD) ((at + done) >> 32);
        DWORD got = 0;
        if (!ReadFile(files_[extent.volume]->handle, bounce, (DWORD) len, &got, &ov) || got != len) return false;
#else
        if (pread(files_[extent.volume]->fd, bounce, len, (off_t) (at + done)) != (ssize_t) len) return false;
#endif
        std::memcpy(dst + done, bounce, n);
    }
    return true;
}

bool ConversationArena::read(const ArenaExtent& extent, uint8_t* data, size_t bytes) {
    if (data == nullptr || extent.length == 0 || bytes > extent.length) return false;
    std::lock_guard<std::recursive_mutex> lock(mu_);
    for (size_t done = 0; done < bytes; done += std::min(bytes - done, kBounce)) {
        const size_t n = std::min(bytes - done, kBounce);
        if (!read_raw(extent.volume, extent.offset + done, round_up(n, kPage))) return false;
        std::memcpy(data + done, bounce_, n);
    }
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
        }
    }
    if (arena->files_.empty()) {
        err = first_err.empty() ? "no arena volume could be opened" : first_err;
        return nullptr;
    }
    arena->file_state_.resize(arena->files_.size());
    Header head;
    if (!arena->read_raw(0, 0, kReserveHeader)) { err = "cannot read the arena header"; return nullptr; }
    std::memcpy(&head, arena->bounce_, sizeof head);
    const bool fresh = head.magic != kMagic || head.version != kVersion || head.model_id != model_id ||
                       head.config_id != config_id || head.volumes != arena->files_.size();
    if (fresh) {
        head = Header{};
        head.volumes = (uint32_t) arena->files_.size();
        head.model_id = model_id;
        head.config_id = config_id;
        std::memset(arena->bounce_, 0, kReserveHeader);
        std::memcpy(arena->bounce_, &head, sizeof head);
        if (!arena->write_raw(0, 0, kReserveHeader)) { err = "cannot write the arena header"; return nullptr; }
        std::fprintf(stderr, "strata serve: conversation arena: new arena over %zu volume(s), %.1f GiB each\n",
                     arena->files_.size(), (double) file_bytes / (1024.0 * 1024.0 * 1024.0));
    } else {
        std::fprintf(stderr, "strata serve: conversation arena: reclaimed %llu extent(s) from a previous run\n",
                     (unsigned long long) head.extent_count);
        head.extent_count = 0;
        std::memcpy(arena->bounce_, &head, sizeof head);
        if (!arena->write_raw(0, 0, kReserveHeader)) { err = "cannot reset the arena header"; return nullptr; }
    }
    arena->free_.resize(arena->files_.size());
    for (size_t v = 0; v < arena->files_.size(); ++v)
        arena->free_[v].push_back(ArenaExtent{(uint32_t) v, (uint64_t) kReserveHeader, file_bytes - kReserveHeader});
    return arena;
}

bool ConversationArena::allocate(size_t bytes, ArenaExtent& out) {
    if (bytes == 0) return false;
    const size_t want = round_up(bytes, kPage);
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (files_.empty()) return false;
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
            live_.push_back(got);
            cursor_ = (v + 1) % volumes;
            persist_table();
            return true;
        }
    }
    return false;   // full: the caller's park is skipped, never a failure
}

void ConversationArena::release_extent(const ArenaExtent& extent) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    for (size_t i = 0; i < live_.size(); ++i) {
        if (live_[i].volume != extent.volume || live_[i].offset != extent.offset) continue;
        const ArenaExtent e = live_[i];
        live_.erase(live_.begin() + (std::ptrdiff_t) i);
        if (e.volume < free_.size()) {
            auto& list = free_[e.volume];
            list.push_back(e);
            std::sort(list.begin(), list.end(), [](const ArenaExtent& a, const ArenaExtent& b) { return a.offset < b.offset; });
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
    if (files_.empty() || bounce_ == nullptr) return;
    Header head;
    head.volumes = (uint32_t) files_.size();
    head.model_id = model_id_;
    head.config_id = config_id_;
    head.extent_count = (uint64_t) live_.size();
    std::memset(bounce_, 0, kReserveHeader);
    std::memcpy(bounce_, &head, sizeof head);
    const size_t capacity = (kReserveHeader - 4096) / sizeof(ArenaExtent);
    const size_t n = std::min(capacity, live_.size());
    std::memcpy(static_cast<uint8_t*>(bounce_) + 4096, live_.data(), n * sizeof(ArenaExtent));
    write_raw(0, 0, kReserveHeader);
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

namespace {

using FiveBuffers = std::array<ConversationBuffer*, 5>;

FiveBuffers five(ConversationKv& kv) { return {&kv.k, &kv.v, &kv.k_scale, &kv.v_scale, &kv.pooled}; }

constexpr size_t kChunk = 16 * 1024 * 1024;

void release_parts(ConversationKv& kv, ConversationArena& arena) {
    for (auto& part : kv.arena)
        for (const auto& extent : part.extents) arena.release_extent(extent);
    for (auto& part : kv.arena) { part.extents.clear(); part.size = 0; }
    kv.arena_bytes = 0;
}

}  // namespace

bool conversation_image_to_arena(SavedConversation& image, ConversationArena& arena, std::string& error, bool throttle) {
    for (auto& kv : image.kv)
        if (!conversation_kv_to_arena(kv, arena, error, throttle)) { conversation_image_release_arena(image, arena); return false; }
    for (auto& stage : image.stage_images)
        if (!conversation_image_to_arena(stage, arena, error, throttle)) { conversation_image_release_arena(image, arena); return false; }
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

bool conversation_kv_to_arena(ConversationKv& kv, ConversationArena& arena, std::string& error, bool throttle) {
    if (kv.arena_bytes != 0) return true;   // already there
    FiveBuffers buffers = five(kv);
    std::array<ConversationKv::ArenaPart, 5> parts;
    for (size_t i = 0; i < 5; ++i) {
        const size_t n = buffers[i]->size();
        parts[i].size = n;
        for (size_t done = 0; done < n; done += kChunk) {
            ArenaExtent extent;
            if (!arena.allocate(std::min(kChunk, n - done), extent)) {
                for (auto& part : parts)
                    for (const auto& e : part.extents) arena.release_extent(e);
                error = "the arena is full";
                return false;
            }
            parts[i].extents.push_back(extent);
        }
    }
    // One worker thread per volume, each using only that volume's handle: concurrent writes on one synchronous handle
    // serialise (measured 140-156 MB/s), while five streams reach the devices' own write rate.
    struct Task {
        const ArenaExtent* extent;
        uint64_t offset;
        const uint8_t* src;
        size_t bytes;
    };
    struct Span {
        const uint8_t* p;
        size_t len;
        size_t at;
    };
    std::vector<Task> tasks;
    std::vector<Span> spans;
    for (size_t i = 0; i < 5; ++i) {
        const size_t total = parts[i].size;
        if (total == 0) continue;
        spans.clear();
        buffers[i]->visit(0, total, [&](uint8_t* p, size_t len, size_t at) {
            spans.push_back({p, len, at});
            return true;
        });
        size_t done = 0;
        for (const auto& extent : parts[i].extents) {
            const size_t n = std::min((size_t) extent.length, total - done);
            const size_t begin = done, end = done + n;
            for (const auto& span : spans) {
                const size_t lo = std::max(begin, span.at), hi = std::min(end, span.at + span.len);
                if (lo >= hi) continue;
                tasks.push_back({&extent, (uint64_t) (lo - begin), span.p + (lo - span.at), hi - lo});
            }
            done += n;
        }
    }
    std::atomic<bool> failed{false};
    std::vector<std::thread> pool;
    pool.reserve(5);
    for (size_t v = 0; v < 5; ++v) {
        pool.emplace_back([&, v] {
            uint8_t* bounce = static_cast<uint8_t*>(ConversationArena::alloc_bounce());
            if (bounce == nullptr) { failed = true; return; }
            for (const auto& task : tasks) {
                if (task.extent->volume != v || failed.load(std::memory_order_relaxed)) continue;
                if (!arena.write_positioned(*task.extent, task.offset, task.src, task.bytes, bounce)) failed = true;
            }
            ConversationArena::free_bounce(bounce);
        });
    }
    for (auto& worker : pool) worker.join();
    if (failed.load()) {
        release_parts(kv, arena);
        error = "the snapshot buffer could not be written to the arena";
        return false;
    }
    for (auto& buffer : buffers) *buffer = ConversationBuffer();
    kv.arena = std::move(parts);
    kv.arena_bytes = 0;
    for (const auto& part : kv.arena) kv.arena_bytes += part.size;
    return true;
}
bool conversation_kv_from_arena(ConversationKv& kv, ConversationArena& arena, std::string& error) {
    if (kv.arena_bytes == 0) return true;
    FiveBuffers buffers = five(kv);
    // The restore is the slow side of the arena (a 330k-token conversation materialises ~6 GB), so its extents are
    // read in parallel: one task per (extent, destination span), each worker with its own bounce buffer.  The
    // destination ranges are disjoint, so no lock is needed on the image.
    struct Task {
        const ArenaExtent* extent;
        uint64_t offset;   // inside the extent
        uint8_t* dst;
        size_t bytes;
    };
    struct Span {
        uint8_t* p;
        size_t len;
        size_t at;
    };
    std::vector<Task> tasks;
    std::vector<Span> spans;
    for (size_t i = 0; i < 5; ++i) {
        const size_t total = kv.arena[i].size;
        if (total == 0) { buffers[i]->resize(0); continue; }
        buffers[i]->resize(total, 0);
        spans.clear();
        buffers[i]->visit(0, total, [&](uint8_t* p, size_t len, size_t at) {
            spans.push_back({p, len, at});
            return true;
        });
        size_t done = 0;
        for (const auto& extent : kv.arena[i].extents) {
            const size_t n = std::min((size_t) extent.length, total - done);
            const size_t begin = done, end = done + n;
            for (const auto& span : spans) {
                const size_t lo = std::max(begin, span.at), hi = std::min(end, span.at + span.len);
                if (lo >= hi) continue;
                tasks.push_back({&extent, (uint64_t) (lo - begin), span.p + (lo - span.at), hi - lo});
            }
            done += n;
        }
    }
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex error_mu;
    std::string first_error;
    const size_t workers = std::min<size_t>(8, std::max<size_t>(1, tasks.size()));
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (size_t w = 0; w < workers; ++w) {
        pool.emplace_back([&] {
            uint8_t* bounce = static_cast<uint8_t*>(ConversationArena::alloc_bounce());
            if (bounce == nullptr) { failed = true; return; }
            for (size_t k = next++; k < tasks.size() && !failed.load(std::memory_order_relaxed); k = next++) {
                const Task& task = tasks[k];
                if (!arena.read_positioned(*task.extent, task.offset, task.dst, task.bytes, bounce)) {
                    std::lock_guard<std::mutex> lock(error_mu);
                    if (first_error.empty()) first_error = "the arena could not be read";
                    failed = true;
                }
            }
            ConversationArena::free_bounce(bounce);
        });
    }
    for (auto& worker : pool) worker.join();
    if (failed.load()) {
        error = first_error.empty() ? "the arena could not be read" : first_error;
        return false;
    }
    release_parts(kv, arena);
    return true;
}

void conversation_kv_release_arena(ConversationKv& kv, ConversationArena& arena) { release_parts(kv, arena); }

}  // namespace strata::core
