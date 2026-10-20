// Tests for the arena behind --conversation-cache-dir (docs/CONVERSATION_CACHE_ARENA.md).
// Five small files in a temporary directory stand in for the five Optane volumes.

#include "strata/core/conversation_arena.hpp"
#include "strata/core/conversation_cache.hpp"

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
bool holds(const ConversationBuffer& b, uint8_t value) {
    bool same = true;
    b.visit(0, b.size(), [&](const uint8_t* p, size_t n, size_t) {
        for (size_t i = 0; i < n; ++i)
            if (p[i] != value) { same = false; break; }
        return true;
    });
    return same;
}
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

    // 1. Open, allocate, write, read, release: extents come back to the free lists.
    {
        auto arena = ConversationArena::open(paths, kEach, 11, 22, err);
        check(arena != nullptr, "the arena opens over five files");
        const auto start = arena->stats();
        check(start.volumes == 5, "five volumes are opened");
        const size_t want = 20 * 1024 * 1024 + 123;
        ArenaExtent extent;
        check(arena->allocate(want, extent), "a 20 MiB extent is served");
        std::vector<uint8_t> buf((size_t) extent.length, 0x5a), back((size_t) extent.length, 0);
        check(arena->write(extent, buf.data(), buf.size()), "the extent takes a write");
        check(arena->read(extent, back.data(), back.size()) && back.back() == 0x5a, "and reads back");
        check(arena->stats().used_bytes >= want, "the used bytes account it");
        ArenaExtent second;
        check(arena->allocate(8 * 1024 * 1024, second) &&
                  (second.volume != extent.volume || second.offset != extent.offset),
              "a second extent is a different range");
        arena->release_extent(extent);
        arena->release_extent(second);
        const auto after = arena->stats();
        check(after.used_bytes == 0, "released extents are free again");
        check(after.free_bytes == start.free_bytes, "the free bytes are back to the start");
    }

    // 2. Exhaustion: a request bigger than one volume's free space is refused, not fatal.
    {
        auto arena = ConversationArena::open(paths, kEach, 11, 22, err);
        check(arena != nullptr, "the arena reopens");
        ArenaExtent extent;
        check(!arena->allocate(kEach, extent), "a request larger than a volume is refused");
        check(arena->allocate(4 * 1024 * 1024, extent), "a fitting request still works after a refusal");
    }

    // 3. Identity: another config wipes the arena; the same identity reclaims the old extents (until M3).
    {
        auto arena = ConversationArena::open(paths, kEach, 11, 22, err);
        ArenaExtent extent;
        check(arena->allocate(1024 * 1024, extent), "an extent before the identity change");
        arena.reset();
        auto other = ConversationArena::open(paths, kEach, 11, 99, err);
        check(other != nullptr, "the arena reopens with another config");
        check(other->stats().used_bytes == 0, "the mismatched arena starts empty");
        ArenaExtent kept;
        check(other->allocate(2 * 1024 * 1024, kept), "an extent in the new arena");
        other.reset();
        auto same = ConversationArena::open(paths, kEach, 11, 99, err);
        check(same->stats().used_bytes == 0, "a matching arena reclaims a previous run's extents (no entry descriptors yet)");
    }

    // 4. One K/V image: into the arena (the heap copies go), back out identical, and released without copying.
    {
        auto arena = ConversationArena::open(paths, kEach, 11, 22, err);
        ConversationKv kv;
        const size_t big = 40 * 1024 * 1024 + 7, small = 1024 * 1024 + 17;
        kv.k.resize(big, 0x01);
        kv.v.resize(small, 0x02);
        kv.k_scale.resize(0);
        kv.v_scale.resize(4096, 0x03);
        kv.pooled.resize(65536, 0x04);
        check(conversation_kv_to_arena(kv, *arena, err), "the image moves into the arena");
        check(kv.k.size() == 0 && kv.v.size() == 0 && kv.pooled.size() == 0, "the heap copies are freed");
        check(kv.bytes() == 0 && kv.arena_bytes >= big + small + 4096 + 65536,
              "the RAM accounting is zero (the payloads are in the files) and arena_bytes counts them");
        check(arena->stats().used_bytes >= big + small + 4096 + 65536, "the payloads are in the arena");
        check(conversation_kv_from_arena(kv, *arena, err), "the image comes back");
        check(kv.k.size() == big && kv.v.size() == small && kv.v_scale.size() == 4096 && kv.pooled.size() == 65536,
              "the buffer sizes are restored");
        check(holds(kv.k, 0x01) && holds(kv.v, 0x02) && holds(kv.v_scale, 0x03) && holds(kv.pooled, 0x04),
              "the payload bytes survive the round trip");
        check(arena->stats().used_bytes == 0, "the extents are released on the way back");
        check(conversation_kv_to_arena(kv, *arena, err), "the image moves in again");
        conversation_kv_release_arena(kv, *arena);
        check(arena->stats().used_bytes == 0, "release frees them without a copy");
    }

    // 5. Through the cache: put copies into the arena, take copies back, clear frees, the budget still bounds.
    {
        auto arena = ConversationArena::open(paths, kEach, 11, 22, err);
        ConversationCache cache(64ull * 1024 * 1024, 3);
        cache.set_arena(arena.get());
        SavedConversation image;
        image.live.ids = {1, 2, 3};
        image.kv.resize(2);
        image.kv[0].k.resize(8 * 1024 * 1024, 0x11);
        image.kv[1].v.resize(1024 * 1024, 0x22);
        const size_t parked = image.bytes();
        check(cache.put(std::move(image)), "the cache takes the image");
        check(arena->stats().used_bytes >= 9 * 1024 * 1024, "its payload is in the arena, not the heap");
        check(cache.bytes() >= parked, "the cache still counts the parked bytes against its budget");
        SavedConversation out = cache.take(0);
        check(out.kv.size() == 2 && out.kv[0].k.size() == 8 * 1024 * 1024 && holds(out.kv[0].k, 0x11),
              "the taken image is materialized with its bytes");
        check(arena->stats().used_bytes == 0, "and its extents are back");
        SavedConversation again;
        again.live.ids = {4};
        again.kv.resize(1);
        again.kv[0].pooled.resize(2 * 1024 * 1024, 0x33);
        check(cache.put(std::move(again)), "a second image is parked");
        check(cache.clear() == 1 && cache.size() == 0, "clear drops it");
        check(arena->stats().used_bytes == 0, "and frees its extents");
    }

    // 5b. A small arena: a second park evicts the first instead of failing (the failure that reached a client once).
    {
        ConversationCache cache(64ull * 1024 * 1024, 2);
        check(!cache.arena_backed(), "no arena by default");
        SavedConversation image;
        image.live.ids = {9};
        image.kv.resize(1);
        image.kv[0].k.resize(1024, 0x44);
        check(cache.put(std::move(image)), "a heap cache parks as before");
        SavedConversation out = cache.take(0);
        check(out.kv[0].k.size() == 1024 && holds(out.kv[0].k, 0x44), "and takes it back unchanged");
    }

    fs::remove_all(dir, ec);
    std::printf("conversation_arena_test: %d checks\n", checks);
    return 0;
}
