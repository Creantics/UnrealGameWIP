#pragma once
// Whether a press would be taken now, and if not why: the rules' own answer (ArenaFight::pressReason,
// FightSession::giftReason), read from the same predicate the press runs, so a key prompt asks the rules instead of
// copying them. The HUD draws a refused prompt dimmed with its reason's word, and hides only an Absent one.
#include <cstddef>
#include <cstdint>

namespace ftcore {

enum class PressReason : std::uint8_t {
    Ok,         // a press now is taken
    Busy,       // a press now waits (the buffer, a hit pause, the current action) and is taken when that ends: not refused
    Absent,     // the action does not exist in this fight (the kit has no such move, no heals, no partner, no gift slot)
    Down,       // the one who would act is down, or the fight is lost
    Broken,     // the guard is broken: nothing starts until the break ends
    Overloaded, // the Load is at its cap: light, heavy, shot and heal are refused until a Release
    Stamina,    // no stamina for it
    Empty,      // nothing left to spend: no heal charges, an empty cylinder, too little Load, no gift heals left
    Cooldown,   // the gift is cooling down
    Full,       // a heal with nothing to heal (the gift Heal at a lead at full health)
    Closed,     // the paired counter's window is not open
    Refused,    // the slot will not give (hostile, or aimed at no lead)
};
inline constexpr std::size_t kPressReasonCount = static_cast<std::size_t>(PressReason::Refused) + 1;

// A press refused now: drawn dimmed with its reason. Ok and Busy are taken (now or once the wait ends); Absent is not
// drawn at all.
constexpr bool pressRefused(PressReason r) {
    return r != PressReason::Ok && r != PressReason::Busy && r != PressReason::Absent;
}

} // namespace ftcore
