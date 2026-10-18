#include "AllocCounter.h"

#include <atomic>

namespace ftcore_test {
namespace {
// Constant-initialised, so safe to touch from operator new before main() and during static init.
std::atomic<std::uint64_t> g_count{0};
std::atomic<std::uint64_t> g_bytes{0};
std::atomic<bool> g_installed{false};
} // namespace

AllocStats allocStats() {
    return {g_count.load(std::memory_order_relaxed), g_bytes.load(std::memory_order_relaxed)};
}

bool allocHookInstalled() { return g_installed.load(std::memory_order_relaxed); }

namespace detail {
void recordAlloc(std::uint64_t bytes) noexcept {
    g_count.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(bytes, std::memory_order_relaxed);
}
void markHookInstalled() noexcept { g_installed.store(true, std::memory_order_relaxed); }
} // namespace detail

} // namespace ftcore_test
