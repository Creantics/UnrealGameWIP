#pragma once

// The Lone Carry: a stance a lead takes before a fight (or is put in when the third slot is Vacant or Hostile). It is
// fixed for the fight, except that a gift received (given) ends it for the rest of that fight; a press refused for
// any reason did nothing and leaves it. The host may also end it for the partner's call (FightSession::endLone),
// and the fight's end ends it (finish).
//
// What the host shows (FightSession):
//   - the bargain: loneBargain(), the gains and losses as data for a card, and FightTick::loneCarryBegan on the
//     first tick after a Lone begin;
//   - the end: FightTick::loneCarryEndedBy on the first tick after it ends (LoneEnd), and loneEndedBy() at any time
//     (read it at the fight's end: finish ends a Lone still on, and the host may not tick after it);
//   - the roll: loneRoll(save, poolTuning), the save's lone Releases and their Load plus this fight's until
//     foldLone puts the fight in the save, with the effect the pool will have on the last boss (lonePoolEffect).
//     Core keeps counts only, no names.
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
// Data/Battle/lone_pool.json (the pool's effect on the last boss, LonePoolTuning):
//   { "version": 1, "maxBonus": 400, "halfPool": 2000, "figurePermille": 150, "maxFigures": 2 }

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

// The save-wide pool of Load spent by Releases made while Lone, and how many Releases put it there. Exposed for the
// save; nothing persists it yet.
struct LoneSave {
    std::uint32_t pooled = 0;
    std::uint32_t releases = 0;
};

// Adds a fight's lone Releases to the save (both saturating). The host does not call it itself: FightSession::foldLone
// calls it once after finish(true) and stops the roll counting that fight twice.
inline void addLoneFight(LoneSave& s, std::uint32_t releases, std::uint32_t pooled) {
    s.releases = saturatingAdd(s.releases, releases);
    s.pooled = saturatingAdd(s.pooled, pooled);
}

// What the Lone Carry gives and takes, as data for a card the host shows when it begins (no text here).
struct LoneBargain {
    float release = 1.f;     // x on a Release's hp damage
    float loadGain = 1.f;    // x on every store
    int loadCap = 0;         // the Load cap, whatever the chairs give
    float damageTaken = 1.f; // x on an enemy blow's hp on the lead
    bool noCount = true;     // nobody joins the count (called alone, it lands light)
    bool noTable = true;     // the table is refused
    bool noTakeHalf = true;  // take half is refused
    bool giftReceivedEnds = true; // a gift received ends it for the fight (a refused press does not)
};
inline LoneBargain loneBargain(const LoneTuning& t) {
    LoneBargain b;
    b.release = t.release;
    b.loadGain = t.gain;
    b.loadCap = t.cap;
    b.damageTaken = t.taken;
    return b;
}

// Why the Lone Carry ended this fight.
enum class LoneEnd : std::uint8_t { None, Gift, PartnerCalled, FightEnded }; // append only

// The pool's effect on the last boss (per boss). B = maxBonus x P / (P + halfPool) permille, in [0, maxBonus) for
// every P; figures = min(maxFigures, B / figurePermille), each an extra figure of figurePermille of the boss's hp;
// what is left of B adds to the boss's own hp. lonePoolEffect is the one formula: the last boss must call it (with
// the tuning from Data/Battle/lone_pool.json, importLonePool) and no other, so what the roll shows is what it does.
struct LonePoolTuning {
    int maxBonus = 400;              // permille, the ceiling (never reached): 0..400
    std::uint32_t halfPool = 2000;   // the pool that gives half the ceiling: 1..1e6
    int figurePermille = 150;        // 50..400
    int maxFigures = 2;              // 0..2
};
inline bool validLonePoolTuning(const LonePoolTuning& t) {
    return t.maxBonus >= 0 && t.maxBonus <= 400 && t.halfPool >= 1u && t.halfPool <= 1000000u &&
           t.figurePermille >= 50 && t.figurePermille <= 400 && t.maxFigures >= 0 && t.maxFigures <= 2;
}
struct LonePoolEffect {
    int totalPermille = 0;   // B: the extra the pool adds, figures included
    int ceilingPermille = 0; // maxBonus: B stays under it
    int figures = 0;         // extra figures
    int figurePermille = 0;  // each figure's share of the boss's hp
    int hpPermille = 0;      // B less the figures: added to the boss's own hp
};
// Out-of-range tuning gives the defaults' effect.
inline LonePoolEffect lonePoolEffect(std::uint32_t pooled, const LonePoolTuning& tuning) {
    const LonePoolTuning t = validLonePoolTuning(tuning) ? tuning : LonePoolTuning{};
    LonePoolEffect e;
    const std::uint64_t p = pooled;
    e.totalPermille = static_cast<int>(static_cast<std::uint64_t>(t.maxBonus) * p / (p + t.halfPool));
    e.ceilingPermille = t.maxBonus;
    const int f = e.totalPermille / t.figurePermille;
    e.figures = f < t.maxFigures ? f : t.maxFigures;
    e.figurePermille = t.figurePermille;
    e.hpPermille = e.totalPermille - e.figures * t.figurePermille;
    return e;
}

// The roll: the lone Releases and the Load they pooled, the save's plus a fight's, and the pool's effect. Empty
// (shown() false) until the first lone Release.
struct LoneRoll {
    std::uint32_t releases = 0;
    std::uint32_t pooled = 0;
    LonePoolEffect effect{};
    bool shown() const { return releases > 0; }
};
inline LoneRoll loneRoll(const LoneSave& save, std::uint32_t fightReleases, std::uint32_t fightPooled,
                         const LonePoolTuning& tuning) {
    LoneRoll r;
    r.releases = saturatingAdd(save.releases, fightReleases);
    r.pooled = saturatingAdd(save.pooled, fightPooled);
    r.effect = lonePoolEffect(r.pooled, tuning);
    return r;
}

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
// x`release`), then the gift ends the stance. Not Lone, the press goes through at once. The host ends each step with
// stepEnd(), which keeps that order.
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
    // The end of a step: `events()` (the host's handling of the step's events, a Released pooled among them), then
    // `gift()` once if a press was held. The other way round the gift would end Lone first and the pool refuse the
    // Release.
    template <class Events, class Gift>
    void stepEnd(Events&& events, Gift&& gift) {
        events();
        if (take()) gift();
    }
};

// Lone is forced when the third slot is Vacant or Hostile.
inline bool loneForced(SlotState third) { return third == SlotState::Vacant || third == SlotState::Hostile; }

// False on a refused file: `out` keeps the defaults and *error names each problem, one per line. Refused: a wrong
// version, a field of the wrong type or out of its range, a non-whole cap, an unknown field.
FTCORE_API bool importLoneCarry(std::string_view json, LoneTuning& out, std::string* error = nullptr);
// Reads Data/Battle/lone_pool.json the same way (whole numbers only, each in its LonePoolTuning range).
FTCORE_API bool importLonePool(std::string_view json, LonePoolTuning& out, std::string* error = nullptr);

} // namespace ftcore
