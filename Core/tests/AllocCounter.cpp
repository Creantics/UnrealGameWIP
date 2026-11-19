#include "AllocCounter.h"

#include <atomic>

namespace ftcore_test {
namespace {
// Per thread, not per process: Windows may start a thread in this process at any time (a thread pool worker, say),
// and each start runs the test framework's thread_local constructors, which allocate (2 operator new calls a thread
// in a Debug build). A process-wide total counted those inside a test's window now and then.
// Constant-initialised, so safe to touch from operator new before main(), during static init and while a new
// thread's TLS callbacks run.
constinit thread_local std::uint64_t t_count = 0;
constinit thread_local std::uint64_t t_bytes = 0;
std::atomic<bool> g_installed{false};
} // namespace

AllocStats allocStats() { return {t_count, t_bytes}; }

bool allocHookInstalled() { return g_installed.load(std::memory_order_relaxed); }

namespace detail {
void recordAlloc(std::uint64_t bytes) noexcept {
    ++t_count;
    t_bytes += bytes;
}
void markHookInstalled() noexcept { g_installed.store(true, std::memory_order_relaxed); }
} // namespace detail

} // namespace ftcore_test
