#include "strata/core/conversation_memory.hpp"

#include <charconv>
#include <cstddef>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace strata::core {

namespace {

/// One `<Key>: <number> kB` line wanted from /proc/meminfo.
struct Wanted {
    const char* key;
    std::optional<uint64_t>* value;
};

/// One pass over meminfo, filling every wanted key.  False when a wanted key repeats with another value or
/// its line is malformed - the caller then has no measurement, which is not the same as a measured zero.
bool read_meminfo(std::istream& meminfo, const Wanted* wanted, size_t count) {
    std::string line;
    while (std::getline(meminfo, line)) {
        std::istringstream fields(line);
        std::string key;
        if (!(fields >> key)) continue;
        for (size_t i = 0; i < count; ++i) {
            if (key != wanted[i].key) continue;
            std::string value, unit, extra;
            if (*wanted[i].value || !(fields >> value >> unit) || unit != "kB" || (fields >> extra)) return false;
            uint64_t kb = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), kb);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                kb > std::numeric_limits<uint64_t>::max() / 1024) return false;
            *wanted[i].value = kb * 1024;
        }
    }
    if (meminfo.bad() || (meminfo.fail() && !meminfo.eof())) return false;
    return true;
}

} // namespace

std::optional<uint64_t> conversation_mem_available(std::istream& meminfo) {
    std::optional<uint64_t> available;
    const Wanted wanted[] = {{"MemAvailable:", &available}};
    if (!read_meminfo(meminfo, wanted, 1)) return {};
    return available;
}

std::optional<uint64_t> conversation_mem_commit(std::istream& meminfo) {
    std::optional<uint64_t> available, swap;
    const Wanted wanted[] = {{"MemAvailable:", &available}, {"SwapFree:", &swap}};
    if (!read_meminfo(meminfo, wanted, 2)) return {};
    // A machine with no swap reports `SwapFree: 0 kB`, which is a measurement and not a missing one.
    if (!available || !swap || *available > std::numeric_limits<uint64_t>::max() - *swap) return {};
    return *available + *swap;
}

std::optional<uint64_t> conversation_available_memory() {
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    if (GlobalMemoryStatusEx(&status)) return status.ullAvailPhys;
    return {};
#elif defined(__linux__)
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo) return {};
    return conversation_mem_available(meminfo);
#else
    return {};
#endif
}

std::optional<uint64_t> conversation_available_commit() {
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    if (GlobalMemoryStatusEx(&status)) return status.ullAvailPageFile;
    return {};
#elif defined(__linux__)
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo) return {};
    return conversation_mem_commit(meminfo);
#else
    return {};
#endif
}

} // namespace strata::core
