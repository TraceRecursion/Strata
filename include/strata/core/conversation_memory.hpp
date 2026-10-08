#pragma once

#include <cstdint>
#include <istream>
#include <optional>

namespace strata::core {

// Host physical memory, not swap/commit or a container/job memory reservation.
// Unknown telemetry is deliberately distinct from a measured zero.
std::optional<uint64_t> conversation_available_memory();
std::optional<uint64_t> conversation_mem_available(std::istream& meminfo);

// The commit headroom instead: how much more this machine will hand out before an allocation fails -
// Windows MEMORYSTATUSEX::ullAvailPageFile (RAM plus the page files), Linux MemAvailable plus SwapFree.
// A parked conversation is ordinary pageable memory and is cold by construction: nothing reads it until it
// is restored.  Everything the engine keeps hot is VirtualLock'ed or cudaHostRegister'ed and cannot be
// paged out in its place, so admitting against this budget is what lets the OS page parked conversations
// out and back instead of the engine refusing to park them at all.  The restore then pays page file reads,
// which for a long conversation still beats re-reading its prompt.
std::optional<uint64_t> conversation_available_commit();
std::optional<uint64_t> conversation_mem_commit(std::istream& meminfo);

inline bool conversation_memory_admit(std::optional<uint64_t> available,
                                      uint64_t allocation, uint64_t floor) {
    return available && *available >= floor && allocation <= *available - floor;
}

} // namespace strata::core
