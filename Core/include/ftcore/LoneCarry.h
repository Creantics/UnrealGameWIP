#pragma once

// The Lone Carry: a stance a lead takes before a fight (or is put in when the third slot is Vacant or Hostile). It is
// fixed for the fight, except that a gift actually given ends it for the rest of that fight.
//
// While Lone (FightSession::lone(), mirrored into ArenaFight::setLone):
//   - the Load cap is `cap` whatever the chairs give, and every store (a parry, a block, take half) gains x`gain`
//     instead of the chairs' gain (the host's giveLoad stays ungained, as with chairs);
//   - a Release begun while Lone deals x`release` on its hp damage, its whole swing (a gift ending Lone mid-swing
//     leaves it): one more factor on the blow hook's scale, kept under the host's single-blow cap like the others;
//   - an enemy blow's hp on the lead (a hit, a guard's chip) is rnd(D x `taken`), after the blow hook; a parry or a
//     block still stores from D itself;
//   - the chairs' effects are off (the PlaceTable is untouched: the Named chairs still count where a carry check
//     reads them), take half and the table are refused, and nobody joins the count: another striker's blow is
//     Alone, so the lead's own call, unanswered, lands at the count's callerAloneMultiplier.
// Each Release made while Lone adds the Load it spent to the pool (LoneSave::pooled, saturating).
//
// Data/Battle/lone_carry.json: { "version": 1, "cap": 100, "gain": 1.5, "release": 1.3, "taken": 1.15 }
// Every field but version may be left out (its default stays).

#include "ftcore/Api.h"
#include "ftcore/CombatInputs.h"
#include "ftcore/GiftSlot.h"
#include "ftcore/Load.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace ftcore {

struct LoneTuning {
    int cap = 100;        // the Load cap while Lone: 1..kMaxLoad
    float gain = 1.5f;    // a store's gain: 1..3
    float release = 1.3f; // the Release's hp damage scale: 1..3
    float taken = 1.15f;  // an enemy blow's hp scale on the lead: 1..2
};

inline bool validLoneTuning(const LoneTuning& t) {
    return t.cap >= 1 && t.cap <= kMaxLoad && t.gain >= 1.f && t.gain <= 3.f && t.release >= 1.f && t.release <= 3.f &&
           t.taken >= 1.f && t.taken <= 2.f;
}

// The most a Lone scale gives (2 x the blow hook's million, the largest `taken` x its largest input).
inline constexpr int kMaxLoneScaled = 3000000;

// rnd(d x s), halves away from zero; 0 for d <= 0; a scale below 1 or not a number leaves d as it is.
inline int loneScale(int d, float s) {
    if (d <= 0) return 0;
    if (!(s >= 1.f) || !std::isfinite(s)) return d;
    const double v = std::round(static_cast<double>(d) * static_cast<double>(s));
    return v >= static_cast<double>(kMaxLoneScaled) ? kMaxLoneScaled : static_cast<int>(v);
}

// The save-wide pool of Load spent by Releases made while Lone. Exposed for the save; nothing persists it yet.
struct LoneSave {
    std::uint32_t pooled = 0;
};

// Adds a Release's spent Load (nothing for spent <= 0), saturating at the uint32 maximum. Returns the pool.
inline std::uint32_t poolLoneRelease(LoneSave& s, int spent) {
    if (spent > 0) s.pooled = saturatingAdd(s.pooled, static_cast<std::uint32_t>(spent));
    return s.pooled;
}

// A Lone Release's scale: the hook's ruled scale x `release`, kept to the single-blow cap (0: none) but never below
// the ruled scale itself (a cap the host did not apply is not applied here either).
inline float loneReleaseScale(float ruled, float release, float cap) {
    if (!(ruled > 0.f) || !std::isfinite(ruled)) return ruled;
    if (!(release >= 1.f) || !std::isfinite(release)) return ruled;
    const float product = ruled * release;
    if (cap > 0.f && product > cap) return ruled > cap ? ruled : cap;
    return product;
}

// The stance a session accepts: a lead and a tuning in range (FightSession::begin refuses anything else; a host
// asks this before its fight begins so a refused stance changes nothing there either).
inline bool loneStanceAccepted(FighterId lead, const LoneTuning& t) { return isLead(lead) && validLoneTuning(t); }

// Whether a host's fight goes Lone before it begins: wanted, with a session, and a stance that session will accept.
// The fight leaves the chairs' heal charges out at begin, so a stance refused later would cost them.
inline bool loneAtBegin(bool wanted, bool session, FighterId lead, const LoneTuning& t) {
    return wanted && session && loneStanceAccepted(lead, t);
}

// A host's gift press while Lone waits for the end of the step: a Release resolved on that tick pools first (with its
// x`release`), then the gift ends the stance. Not Lone, the press goes through at once.
struct LoneGiftHold {
    bool pending = false;
    // A press before the step; true: press the gift now.
    bool press(bool lone) {
        pending = pending || lone;
        return !lone;
    }
    // After the step's events (its Released pooled); true once for a held press.
    bool take() {
        const bool held = pending;
        pending = false;
        return held;
    }
};

// Lone is forced when the third slot is Vacant or Hostile.
inline bool loneForced(SlotState third) { return third == SlotState::Vacant || third == SlotState::Hostile; }

// False on a refused file: `out` keeps the defaults and *error names each problem, one per line. Refused: a wrong
// version, a field of the wrong type or out of its range, a non-whole cap, an unknown field.
FTCORE_API bool importLoneCarry(std::string_view json, LoneTuning& out, std::string* error = nullptr);

} // namespace ftcore
