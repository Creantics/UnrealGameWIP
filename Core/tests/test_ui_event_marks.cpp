#include "ftcore/UiEventMarks.h"

#include <doctest/doctest.h>

using namespace ftcore;

namespace {

constexpr int kLastEvent = static_cast<int>(ArenaEventType::TableShare);

} // namespace

TEST_CASE("ui event marks: every fight event maps to exactly one mark or to nothing, and none to text") {
    int marked = 0;
    for (int e = 0; e <= kLastEvent; ++e) {
        const ArenaEventType type = static_cast<ArenaEventType>(e);
        const UiEventMark mark = uiEventMark(type);
        CHECK(static_cast<std::size_t>(mark) < kUiEventMarkCount);
        const UiEventMarkSpec& spec = uiEventMarkSpec(mark);
        CHECK(spec.mark == mark);
        CHECK(spec.words == 0);
        if (mark == UiEventMark::None) {
            CHECK(spec.place == UiMarkPlace::None);
            CHECK(spec.life == 0.f);
        } else {
            CHECK(spec.place != UiMarkPlace::None);
            CHECK(spec.life > 0.f);
            CHECK(spec.life < 1.5f); // a mark is a moment, never a standing label
            ++marked;
        }
    }
    CHECK(marked == 9);
    for (std::size_t m = 0; m < kUiEventMarkCount; ++m) CHECK(uiEventMarkSpec(static_cast<UiEventMark>(m)).words == 0);
}

TEST_CASE("ui event marks: the table says what the ruling says") {
    CHECK(uiEventMark(ArenaEventType::Parried) == UiEventMark::RingSnap);
    CHECK(uiEventMark(ArenaEventType::Blocked) == UiEventMark::GuardTick);
    CHECK(uiEventMark(ArenaEventType::Evaded) == UiEventMark::RingOpen);
    CHECK(uiEventMark(ArenaEventType::Staggered) == UiEventMark::LineBreak);
    CHECK(uiEventMark(ArenaEventType::TableShare) == UiEventMark::ShareThread);
    CHECK(uiEventMark(ArenaEventType::LoadStored) == UiEventMark::LoadTip);
    CHECK(uiEventMark(ArenaEventType::Read) == UiEventMark::ReadTick);
    // Damage and heals are the lines' own: no numbers, no mark.
    CHECK(uiEventMark(ArenaEventType::Hit) == UiEventMark::None);
    CHECK(uiEventMark(ArenaEventType::Healed) == UiEventMark::None);
    CHECK(uiEventMark(ArenaEventType::Released) == UiEventMark::None);
    CHECK(uiEventMarkSpec(UiEventMark::RingSnap).place == UiMarkPlace::Reticle);
    CHECK(uiEventMarkSpec(UiEventMark::GuardTick).place == UiMarkPlace::LeadRing);
}

TEST_CASE("ui event marks: a parry snaps the ring shut, rings once, and gives the lock back") {
    const UiMarkPose a = uiEventMarkPose(UiEventMark::RingSnap, 0.f);
    CHECK(a.span == doctest::Approx(kUiLockSpan));
    CHECK(a.base == 0.f);
    const UiMarkPose shut = uiEventMarkPose(UiEventMark::RingSnap, 0.1f);
    CHECK(shut.span == doctest::Approx(1.f));
    CHECK(shut.rippleAlpha > 0.f);
    // The ripple only grows and only fades: it rings once.
    float r = 0.f;
    float ra = 1.f;
    for (float t = 0.1f; t < 0.5f; t += 0.02f) {
        const UiMarkPose p = uiEventMarkPose(UiEventMark::RingSnap, t);
        CHECK(p.ripple >= r);
        CHECK(p.rippleAlpha <= ra);
        r = p.ripple;
        ra = p.rippleAlpha;
    }
    const UiMarkPose late = uiEventMarkPose(UiEventMark::RingSnap, 0.54f);
    CHECK(late.base > 0.9f);
    CHECK(uiEventMarkPose(UiEventMark::RingSnap, 0.6f).alpha == 0.f);
}

TEST_CASE("ui event marks: an evade opens the ring and lets it go") {
    float span = 1.f;
    float rad = 0.f;
    for (float t = 0.f; t < 0.35f; t += 0.05f) {
        const UiMarkPose p = uiEventMarkPose(UiEventMark::RingOpen, t);
        CHECK(p.span <= span);
        CHECK(p.radius >= rad);
        span = p.span;
        rad = p.radius;
    }
    CHECK(span < 0.2f);
    CHECK(rad > 1.4f);
    CHECK(uiEventMarkPose(UiEventMark::RingOpen, 0.4f).alpha < 0.05f);
}

TEST_CASE("ui event marks: a block tick holds full for 150 ms, then fades out by 300 ms") {
    CHECK(uiEventMarkPose(UiEventMark::GuardTick, 0.f).tip > 1.5f); // strikes long
    CHECK(uiEventMarkPose(UiEventMark::GuardTick, 0.14f).alpha == doctest::Approx(1.f));
    CHECK(uiEventMarkPose(UiEventMark::GuardTick, 0.225f).alpha == doctest::Approx(0.5f));
    CHECK(uiEventMarkPose(UiEventMark::GuardTick, 0.3f).alpha == 0.f);
}

TEST_CASE("ui event marks: a stagger springs out to a clearly larger ring and holds still") {
    const UiMarkPose open = uiEventMarkPose(UiEventMark::LineBreak, 0.12f);
    CHECK(open.tip == doctest::Approx(1.f));
    CHECK(open.jitter > uiStaggerHoldPose().jitter);
    CHECK(open.radius > uiStaggerHoldPose().radius); // the spring overshoots once
    const UiMarkPose settled = uiEventMarkPose(UiEventMark::LineBreak, 0.5f);
    CHECK(settled.span == doctest::Approx(uiStaggerHoldPose().span));
    CHECK(settled.radius == doctest::Approx(uiStaggerHoldPose().radius));
    CHECK(settled.jitter == 0.f);
    CHECK(uiStaggerHoldPose().radius >= 1.7f); // clearly larger than the resting ring
    CHECK(uiStaggerHoldPose().span > kUiLockSpan);
    CHECK(uiStaggerHoldPose().base == 0.f);
}

TEST_CASE("ui event marks: a shared blow's thread draws in, a pulse travels it and rings once, fades") {
    CHECK(uiEventMarkPose(UiEventMark::ShareThread, 0.05f).tip < 1.f);
    CHECK(uiEventMarkPose(UiEventMark::ShareThread, 0.12f).tip == doctest::Approx(1.f));
    CHECK(uiEventMarkPose(UiEventMark::ShareThread, 0.04f).ripple == 0.f);
    const float mid = uiEventMarkPose(UiEventMark::ShareThread, 0.18f).ripple;
    CHECK(mid > 0.2f);
    CHECK(mid < 0.8f);
    CHECK(uiEventMarkPose(UiEventMark::ShareThread, 0.3f).ripple == doctest::Approx(1.f));
    CHECK(uiEventMarkPose(UiEventMark::ShareThread, 0.31f).rippleAlpha > 0.9f);
    CHECK(uiEventMarkPose(UiEventMark::ShareThread, 0.55f).rippleAlpha == 0.f);
    CHECK(uiEventMarkPose(UiEventMark::ShareThread, 0.74f).alpha < 0.05f);
}

TEST_CASE("ui event marks: live marks restart on a repeat, never grow past the list, and clear") {
    UiLiveMarks marks;
    CHECK_FALSE(uiMarkPush(marks, ArenaEventType::Hit, 1, kArenaPlayer, 12)); // no mark: nothing kept
    CHECK(uiMarkLiveCount(marks) == 0);
    CHECK(uiMarkPush(marks, ArenaEventType::Parried, 1, kArenaPlayer, 0));
    uiMarkTick(marks, 0.3f);
    CHECK(uiMarkPush(marks, ArenaEventType::Parried, 1, kArenaPlayer, 0)); // the same parry again: restarts
    CHECK(uiMarkLiveCount(marks) == 1);
    uiMarkTick(marks, 0.3f);
    CHECK(uiMarkLiveCount(marks) == 1); // restarted, so still live at 0.3 s
    uiMarkTick(marks, 0.3f);
    CHECK(uiMarkLiveCount(marks) == 0);
    for (int i = 0; i < 40; ++i) {
        uiMarkPush(marks, ArenaEventType::TableShare, kArenaPlayer, static_cast<std::uint8_t>(i % 20), 3);
        CHECK(uiMarkLiveCount(marks) <= kUiLiveMarkMax);
    }
    uiMarkClear(marks);
    CHECK(uiMarkLiveCount(marks) == 0);
}

TEST_CASE("ui event marks: the struck lead of a shared blow comes from who shared it, or is not drawn") {
    ArenaTableSeats seats;
    seats.player = seats.ally = true;
    CHECK(uiShareStruck(seats, false, true) == kArenaPlayer);
    CHECK(uiShareStruck(seats, true, false) == kArenaAlly);
    CHECK(uiShareStruck(seats, false, false) == kArenaNobody); // both seated, only enemies shared: cannot tell
    seats.ally = false;
    CHECK(uiShareStruck(seats, false, false) == kArenaPlayer);
    seats = ArenaTableSeats{};
    CHECK(uiShareStruck(seats, false, false) == kArenaNobody);
}
