// Tests for the mapped-file arena behind --conversation-cache-dir (docs/CONVERSATION_CACHE_ARENA.md).
// Five small files in a temporary directory stand in for the five Optane volumes.

#include "strata/core/conversation_arena.hpp"
#include "strata/core/conversation_buffer.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace strata::core;

namespace {
int checks = 0;
void check(bool value, const char* what) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
constexpr size_t kEach = 32ull * 1024 * 1024;   // one volume in the test
}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "strata-arena-test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    std::vector<std::string> paths;
    for (int i = 1; i <= 5; ++i) paths.push_back((dir / ("strata-arena." + std::to_string(i) + ".bin")).string());
    std::string err;

    // 1. Open, allocate, write, release: extents come back to the free lists.
    {
        auto arena = ConversationArena::open(paths, kEach, 11, 22, err);
        check(arena != nullptr, "the arena opens over five files");
        const auto start = arena->stats();
        check(start.volumes == 5, "five volumes are mapped");
        const size_t want = 20 * 1024 * 1024 + 123;
        void* p = arena->allocate(want);
        check(p != nullptr, "a 20 MiB extent is served");
        std::memset(p, 0x5a, want);
        check(static_cast<uint8_t*>(p)[want - 1] == 0x5a, "the extent is writable to its end");
        check(arena->stats().used_bytes >= want, "the used bytes account it");
        void* q = arena->allocate(8 * 1024 * 1024);
        check(q != nullptr && q != p, "a second extent is a different range");
        arena->release(p);
        arena->release(q);
        const auto after = arena->stats();
        check(after.used_bytes == 0, "released extents are free again");
        check(after.free_bytes == start.free_bytes, "the free bytes are back to the start");
    }

    // 2. Exhaustion: a request bigger than one volume's free space is refused, not fatal.
    {
        auto arena = ConversationArena::open(paths, kEach, 11, 22, err);
        check(arena != nullptr, "the arena reopens");
        check(arena->allocate(kEach) == nullptr, "a request larger than a volume is refused");
        check(arena->allocate(4 * 1024 * 1024) != nullptr, "a fitting request still works after a refusal");
    }

    // 3. Identity: another config wipes the arena; the same identity reclaims the old extents (until M3).
    {
        auto arena = ConversationArena::open(paths, kEach, 11, 22, err);
        check(arena->allocate(1024 * 1024) != nullptr, "an extent before the identity change");
        arena.reset();
        auto other = ConversationArena::open(paths, kEach, 11, 99, err);
        check(other != nullptr, "the arena reopens with another config");
        check(other->stats().used_bytes == 0, "the mismatched arena starts empty");
        void* keep = other->allocate(2 * 1024 * 1024);
        check(keep != nullptr, "an extent in the new arena");
        other.reset();
        auto same = ConversationArena::open(paths, kEach, 11, 99, err);
        check(same->stats().used_bytes == 0, "a matching arena reclaims a previous run's extents (no entry descriptors yet)");
    }

    // 4. ConversationBuffer: its segments come from the arena and go back when it is destroyed.
    {
        auto arena = ConversationArena::open(paths, kEach, 11, 22, err);
        {
            ArenaAllocatorScope scope(arena.get());
            ConversationBuffer bytes;
            bytes.resize(40 * 1024 * 1024 + 7, 0x33);
            check(bytes.size() == 40 * 1024 * 1024 + 7, "the buffer holds its payload");
            check(arena->stats().used_bytes >= 40 * 1024 * 1024, "the segments come from the arena");
            bool same = true;
            bytes.visit(0, bytes.size(), [&](const uint8_t* p, size_t n, size_t) {
                for (size_t i = 0; i < n; ++i)
                    if (p[i] != 0x33) { same = false; break; }
                return true;
            });
            check(same, "the payload reads back");
            ConversationBuffer copy = bytes;
            check(copy.size() == bytes.size() && copy == bytes, "a copy inside the arena round-trips");
        }
        check(arena->stats().used_bytes == 0, "the buffer's extents are released with it");
    }

    // 5. Without an arena the buffer is the heap, exactly as before the feature.
    {
        ConversationBuffer heap;
        heap.resize(1024, 1);
        check(heap.bytes() >= 1024, "a heap buffer needs no arena");
        check(ConversationArena::current() == nullptr, "no arena is active by default");
    }

    fs::remove_all(dir, ec);
    std::printf("conversation_arena_test: %d checks\n", checks);
    return 0;
}
