#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

TEST_CASE("alloc counter: a thread's count moves with its own allocations only, not another thread's") {
    if (!ftcore_test::allocHookInstalled()) return;
    constexpr int kAllocs = 8;
    std::atomic<int> stage{0};
    std::uint64_t workerDelta = 0;
    // The thread starts (and allocates on this thread) before the window opens, and is joined after it closes.
    std::thread worker([&] {
        std::vector<int*> kept;
        kept.reserve(kAllocs);
        while (stage.load() != 1) std::this_thread::yield();
        const std::uint64_t from = ftcore_test::allocStats().count;
        for (int i = 0; i < kAllocs; ++i) kept.push_back(new int(i)); // kept, so no allocation is elided
        workerDelta = ftcore_test::allocStats().count - from;
        stage.store(2);
        for (int* p : kept) delete p;
    });
    const std::uint64_t before = ftcore_test::allocStats().count;
    stage.store(1);
    while (stage.load() != 2) std::this_thread::yield();
    const std::uint64_t after = ftcore_test::allocStats().count;
    worker.join();
    CHECK_MESSAGE(after == before, "this thread's count moved by " << after - before << " while another thread allocated");
    CHECK(workerDelta >= static_cast<std::uint64_t>(kAllocs));

    // And this thread's own allocations still count.
    const std::uint64_t own = ftcore_test::allocStats().count;
    int* volatile one = new int(1);
    const std::uint64_t ownAfter = ftcore_test::allocStats().count;
    delete one;
    CHECK(ownAfter == own + 1);
}
