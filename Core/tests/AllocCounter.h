#pragma once

#include <cstdint>

namespace ftcore_test {

// Process-wide heap allocation totals since start. Only meaningful when the replacement global
// operator new from AllocHook.cpp is linked into the executable (allocHookInstalled() tells);
// otherwise the totals stay at zero.
struct AllocStats {
    std::uint64_t count = 0; // calls to operator new (all overloads)
    std::uint64_t bytes = 0; // requested bytes
};

AllocStats allocStats();
bool allocHookInstalled();

namespace detail {
// Called by the operator new replacement; not for general use.
void recordAlloc(std::uint64_t bytes) noexcept;
void markHookInstalled() noexcept;
} // namespace detail

} // namespace ftcore_test
