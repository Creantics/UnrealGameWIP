#include "ftcore/UiEventMarks.h"

#include <algorithm>

namespace ftcore {

namespace {

float clamp01(float v) { return std::min(1.f, std::max(0.f, v)); }
float easeOut(float t) { t = clamp01(t); return 1.f - (1.f - t) * (1.f - t); }

} // namespace

UiEventMark uiEventMark(ArenaEventType type) {
    switch (type) {
    case ArenaEventType::Staggered:
    case ArenaEventType::CounterStagger: return UiEventMark::LineBreak;
    case ArenaEventType::TableShare:
    case ArenaEventType::TookHalf: return UiEventMark::Shared;
    case ArenaEventType::Parried:
    case ArenaEventType::Blocked:
    case ArenaEventType::Evaded:
    case ArenaEventType::LoadStored:
    case ArenaEventType::Read:
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
    case ArenaEventType::AllyDowned:
    case ArenaEventType::ReloadStarted:
    case ArenaEventType::Reloaded:
    case ArenaEventType::DryPress:
    case ArenaEventType::SteppedBack: // an enemy's step back: no HUD need
    case ArenaEventType::BreakTell: return UiEventMark::None;
    }
    return UiEventMark::None;
}

float uiEventMarkLife(UiEventMark mark) {
    switch (mark) {
    case UiEventMark::LineBreak: return 1.f;
    case UiEventMark::Shared: return 1.3f;
    case UiEventMark::None: break;
    }
    return 0.f;
}

UiMarkPose uiEventMarkPose(UiEventMark mark, float age) {
    UiMarkPose p;
    const float t = std::max(age, 0.f);
    if (t >= uiEventMarkLife(mark)) return p;
    switch (mark) {
    case UiEventMark::LineBreak:
        // The line breaks open in 0.1 s, holds, and closes as it fades by 1 s.
        p.tip = easeOut(t / 0.1f);
        p.alpha = t < 0.75f ? 1.f : 1.f - (t - 0.75f) / 0.25f;
        break;
    case UiEventMark::Shared:
        p.tip = 1.f;
        p.alpha = t < 1.f ? 1.f : 1.f - (t - 1.f) / 0.3f;
        break;
    case UiEventMark::None: break;
    }
    return p;
}

bool uiMarkPush(UiLiveMarks& marks, ArenaEventType type, std::uint8_t from, std::uint8_t to) {
    const UiEventMark mark = uiEventMark(type);
    if (mark == UiEventMark::None) return false;
    ++marks.serial;
    for (UiLiveMark& m : marks.m) {
        if (m.live && m.mark == mark && m.from == from && m.to == to) {
            m.age = 0.f;
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
    marks.m[slot] = UiLiveMark{true, mark, from, to, 0.f, marks.serial};
    return true;
}

void uiMarkTick(UiLiveMarks& marks, float dt) {
    const float step = std::max(dt, 0.f);
    for (UiLiveMark& m : marks.m) {
        if (!m.live) continue;
        m.age += step;
        if (m.age >= uiEventMarkLife(m.mark)) m.live = false;
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

} // namespace ftcore
