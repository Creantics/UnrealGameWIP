#include "ftcore/UiEventMarks.h"

#include <doctest/doctest.h>

using namespace ftcore;

namespace {

constexpr int kLastEvent = static_cast<int>(ArenaEventType::TableShare);

} // namespace

TEST_CASE("ui event marks: only a stagger and a shared blow draw a mark of their own") {
    int marked = 0;
    for (int e = 0; e <= kLastEvent; ++e) {
        const UiEventMark mark = uiEventMark(static_cast<ArenaEventType>(e));
        CHECK(static_cast<std::size_t>(mark) < kUiEventMarkCount);
        if (mark == UiEventMark::None) {
            CHECK(uiEventMarkLife(mark) == 0.f);
        } else {
            CHECK(uiEventMarkLife(mark) > 0.f);
            CHECK(uiEventMarkLife(mark) < 1.5f); // a mark is a moment, never a standing label
            ++marked;
        }
    }
    CHECK(marked == 4);
    CHECK(uiEventMark(ArenaEventType::Staggered) == UiEventMark::LineBreak);
    CHECK(uiEventMark(ArenaEventType::CounterStagger) == UiEventMark::LineBreak);
    CHECK(uiEventMark(ArenaEventType::TableShare) == UiEventMark::Shared);
    CHECK(uiEventMark(ArenaEventType::TookHalf) == UiEventMark::Shared);
    // Damage, heals, parries and the Load are the lines' and the name's own.
    CHECK(uiEventMark(ArenaEventType::Hit) == UiEventMark::None);
    CHECK(uiEventMark(ArenaEventType::Healed) == UiEventMark::None);
    CHECK(uiEventMark(ArenaEventType::Parried) == UiEventMark::None);
    CHECK(uiEventMark(ArenaEventType::LoadStored) == UiEventMark::None);
}

TEST_CASE("ui event marks: a stagger breaks the line open in 0.1 s, holds, and closes by 1 s") {
    CHECK(uiEventMarkPose(UiEventMark::LineBreak, 0.05f).tip < 1.f);
    CHECK(uiEventMarkPose(UiEventMark::LineBreak, 0.12f).tip == doctest::Approx(1.f));
    CHECK(uiEventMarkPose(UiEventMark::LineBreak, 0.7f).alpha == 1.f);
    CHECK(uiEventMarkPose(UiEventMark::LineBreak, 0.875f).alpha == doctest::Approx(0.5f));
    CHECK(uiEventMarkPose(UiEventMark::LineBreak, 1.f).alpha == 0.f);
}

TEST_CASE("ui event marks: SHARED holds 1 s, then fades over 0.3 s") {
    CHECK(uiEventMarkPose(UiEventMark::Shared, 0.f).alpha == 1.f);
    CHECK(uiEventMarkPose(UiEventMark::Shared, 0.99f).alpha == 1.f);
    CHECK(uiEventMarkPose(UiEventMark::Shared, 1.15f).alpha == doctest::Approx(0.5f));
    CHECK(uiEventMarkPose(UiEventMark::Shared, 1.3f).alpha == 0.f);
    CHECK(uiEventMarkPose(UiEventMark::None, 0.f).alpha == 0.f);
}

TEST_CASE("ui event marks: live marks restart on a repeat, never grow past the list, and clear") {
    UiLiveMarks marks;
    CHECK_FALSE(uiMarkPush(marks, ArenaEventType::Hit, 1, kArenaPlayer)); // no mark: nothing kept
    CHECK(uiMarkLiveCount(marks) == 0);
    CHECK(uiMarkPush(marks, ArenaEventType::Staggered, kArenaPlayer, 1));
    uiMarkTick(marks, 0.6f);
    CHECK(uiMarkPush(marks, ArenaEventType::CounterStagger, kArenaPlayer, 1)); // the same break again: restarts
    CHECK(uiMarkLiveCount(marks) == 1);
    uiMarkTick(marks, 0.6f);
    CHECK(uiMarkLiveCount(marks) == 1); // restarted, so still live at 0.6 s
    uiMarkTick(marks, 0.6f);
    CHECK(uiMarkLiveCount(marks) == 0);
    for (int i = 0; i < 40; ++i) {
        uiMarkPush(marks, ArenaEventType::Staggered, kArenaPlayer, static_cast<std::uint8_t>(i % 20));
        CHECK(uiMarkLiveCount(marks) <= kUiLiveMarkMax);
    }
    uiMarkClear(marks);
    CHECK(uiMarkLiveCount(marks) == 0);
}
