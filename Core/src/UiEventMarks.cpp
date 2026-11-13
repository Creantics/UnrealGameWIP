#include "ftcore/UiEventMarks.h"

#include <algorithm>

namespace ftcore {

namespace {

float clamp01(float v) { return std::min(1.f, std::max(0.f, v)); }
float lerp(float a, float b, float t) { return a + (b - a) * t; }
float easeOut(float t) { t = clamp01(t); return 1.f - (1.f - t) * (1.f - t); }
float easeIn(float t) { t = clamp01(t); return t * t; }
float smooth(float t) { t = clamp01(t); return t * t * (3.f - 2.f * t); }

constexpr std::array<UiEventMarkSpec, kUiEventMarkCount> kSpecs = {{
    {UiEventMark::None, UiMarkPlace::None, 0.f, 0},
    {UiEventMark::RingSnap, UiMarkPlace::Reticle, 0.55f, 0},
    {UiEventMark::GuardTick, UiMarkPlace::LeadRing, 0.4f, 0},
    {UiEventMark::RingOpen, UiMarkPlace::Reticle, 0.45f, 0},
    {UiEventMark::LineBreak, UiMarkPlace::TopLine, 1.0f, 0},
    {UiEventMark::ShareThread, UiMarkPlace::Between, 0.75f, 0},
    {UiEventMark::LoadTip, UiMarkPlace::LoadArc, 0.6f, 0},
    {UiEventMark::ReadTick, UiMarkPlace::Reticle, 0.35f, 0},
}};

} // namespace

UiEventMark uiEventMark(ArenaEventType type) {
    switch (type) {
    case ArenaEventType::Parried: return UiEventMark::RingSnap;
    case ArenaEventType::Blocked: return UiEventMark::GuardTick;
    case ArenaEventType::Evaded: return UiEventMark::RingOpen;
    case ArenaEventType::Staggered:
    case ArenaEventType::CounterStagger: return UiEventMark::LineBreak;
    case ArenaEventType::TableShare:
    case ArenaEventType::TookHalf: return UiEventMark::ShareThread;
    case ArenaEventType::LoadStored: return UiEventMark::LoadTip;
    case ArenaEventType::Read: return UiEventMark::ReadTick;
    // Carried by the lines and states themselves: the hp lines' rub-out (hits, heals, downs), the stamina arc and the
    // verbs (guard broken, stamina empty), the Load arc (overload, release, spent), the tells, the lock, the result card.
    case ArenaEventType::AttackStarted:
    case ArenaEventType::Hit:
    case ArenaEventType::GuardBroken:
    case ArenaEventType::Healed:
    case ArenaEventType::Downed:
    case ArenaEventType::LockChanged:
    case ArenaEventType::StaminaEmpty:
    case ArenaEventType::Victory:
    case ArenaEventType::Defeat:
    case ArenaEventType::Overloaded:
    case ArenaEventType::Released:
    case ArenaEventType::CardsDealt:
    case ArenaEventType::Misread:
    case ArenaEventType::ReadCounter:
    case ArenaEventType::Flinched:
    case ArenaEventType::BrokeOut:
    case ArenaEventType::HostHit:
    case ArenaEventType::LoadSpent:
    case ArenaEventType::AllyHit:
    case ArenaEventType::AllyDowned: return UiEventMark::None;
    }
    return UiEventMark::None;
}

const UiEventMarkSpec& uiEventMarkSpec(UiEventMark mark) {
    const std::size_t i = static_cast<std::size_t>(mark);
    return kSpecs[i < kUiEventMarkCount ? i : 0];
}

UiMarkPose uiEventMarkPose(UiEventMark mark, float age) {
    UiMarkPose p;
    const float life = uiEventMarkSpec(mark).life;
    const float t = std::max(age, 0.f);
    if (mark == UiEventMark::None || t >= life) {
        p.alpha = 0.f;
        p.span = kUiLockSpan;
        return p;
    }
    switch (mark) {
    case UiEventMark::RingSnap:
        // Snaps shut in 0.08 s (closing in a little as it does), holds, rings once outward, then gives the lock back.
        p.base = smooth((t - 0.3f) / 0.25f);
        p.span = lerp(kUiLockSpan, 1.f, easeIn(t / 0.08f));
        p.radius = lerp(1.15f, 1.f, clamp01(t / 0.08f));
        p.alpha = t < 0.3f ? 1.f : 1.f - smooth((t - 0.3f) / 0.25f);
        if (t >= 0.08f) {
            const float k = clamp01((t - 0.08f) / 0.42f);
            p.ripple = 1.f + 0.8f * easeOut(k);
            p.rippleAlpha = 0.9f * (1.f - k);
        }
        break;
    case UiEventMark::RingOpen: {
        // Opens and lets go: the arcs shrink apart and drift out as they fade; the lock comes back behind them.
        const float k = clamp01(t / 0.35f);
        p.span = lerp(kUiLockSpan, 0.12f, easeOut(k));
        p.radius = 1.f + 0.6f * easeOut(k);
        p.alpha = 1.f - k;
        p.base = smooth((t - 0.2f) / 0.25f);
        break;
    }
    case UiEventMark::GuardTick:
        // Strikes long, settles to its length, fades.
        p.tip = 1.f + 0.6f * (1.f - easeOut(t / 0.12f));
        p.alpha = t < 0.2f ? 1.f : 1.f - (t - 0.2f) / 0.2f;
        break;
    case UiEventMark::LineBreak: {
        // The line breaks open in 0.1 s and the ring springs open with it, trembling hard, then settles to the hold.
        const UiMarkPose hold = uiStaggerHoldPose();
        const float k = easeOut(t / 0.1f);
        p.tip = k;
        p.base = 0.f;
        p.span = lerp(kUiLockSpan, hold.span, k);
        p.radius = lerp(1.f, hold.radius, k);
        p.jitter = lerp(1.f, hold.jitter, clamp01((t - 0.1f) / 0.2f));
        p.alpha = t < 0.75f ? 1.f : 1.f - (t - 0.75f) / 0.25f;
        break;
    }
    case UiEventMark::ShareThread:
        // Drawn from the struck body to the sharer, pulses once as it arrives, fades.
        p.tip = easeOut(t / 0.18f);
        p.alpha = t < 0.18f ? 0.7f : (t < 0.36f ? 1.f : 1.f - (t - 0.36f) / 0.39f);
        p.rippleAlpha = t < 0.18f ? 0.f : 1.f - clamp01((t - 0.18f) / 0.18f);
        break;
    case UiEventMark::LoadTip:
        // The leading tip flares and shrinks back as it fades.
        p.tip = lerp(1.6f, 1.f, easeOut(t / life));
        p.alpha = 1.f - easeIn(t / life);
        break;
    case UiEventMark::ReadTick:
        // The new tick grows out of the ring.
        p.tip = easeOut(t / 0.2f);
        p.alpha = 1.f;
        break;
    case UiEventMark::None: break;
    }
    return p;
}

UiMarkPose uiStaggerHoldPose() {
    UiMarkPose p;
    p.alpha = 1.f;
    p.base = 0.f;
    p.span = 0.30f;
    p.radius = 1.18f;
    p.jitter = 0.35f;
    p.tip = 1.f;
    return p;
}

bool uiMarkPush(UiLiveMarks& marks, ArenaEventType type, std::uint8_t from, std::uint8_t to, int amount) {
    const UiEventMark mark = uiEventMark(type);
    if (mark == UiEventMark::None) return false;
    ++marks.serial;
    for (UiLiveMark& m : marks.m) {
        if (m.live && m.mark == mark && m.from == from && m.to == to) {
            m.age = 0.f;
            m.amount = amount;
            m.serial = marks.serial;
            return true;
        }
    }
    std::size_t slot = kUiLiveMarkMax;
    for (std::size_t i = 0; i < kUiLiveMarkMax; ++i) {
        if (!marks.m[i].live) {
            slot = i;
            break;
        }
        if (slot == kUiLiveMarkMax || marks.m[i].serial < marks.m[slot].serial) slot = i;
    }
    marks.m[slot] = UiLiveMark{true, mark, from, to, amount, 0.f, marks.serial};
    return true;
}

void uiMarkTick(UiLiveMarks& marks, float dt) {
    const float step = std::max(dt, 0.f);
    for (UiLiveMark& m : marks.m) {
        if (!m.live) continue;
        m.age += step;
        if (m.age >= uiEventMarkSpec(m.mark).life) m.live = false;
    }
}

void uiMarkClear(UiLiveMarks& marks) {
    for (UiLiveMark& m : marks.m) m.live = false;
}

std::uint32_t uiMarkLiveCount(const UiLiveMarks& marks) {
    std::uint32_t n = 0;
    for (const UiLiveMark& m : marks.m) n += m.live ? 1u : 0u;
    return n;
}

std::uint8_t uiShareStruck(const ArenaTableSeats& seats, bool sharedToPlayer, bool sharedToAlly) {
    if (sharedToAlly && !sharedToPlayer) return kArenaPlayer;
    if (sharedToPlayer && !sharedToAlly) return kArenaAlly;
    if (sharedToPlayer && sharedToAlly) return kArenaNobody; // never: the struck lead takes no share of its own blow
    if (seats.player && !seats.ally) return kArenaPlayer;
    if (seats.ally && !seats.player) return kArenaAlly;
    return kArenaNobody;
}

} // namespace ftcore
