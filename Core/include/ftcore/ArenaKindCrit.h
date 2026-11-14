#pragma once

// A plain arena enemy kind's weak points (an arena.json kind, not an ArenaBoss.h boss). ArenaFight
// (ArenaFight.h) has no crits of its own; this sits beside it for one enemy slot, as ArenaBoss.h's
// pieces do for a story boss, and reuses its CritWindow: the host feeds it the slot's AttackStarted
// events and asks it, from the blow hook (ArenaFight::setBlowHook), what each lead's blow on the slot
// is worth. Generic per kind: the white_glove (the Dealer) is the first, others share it.
//
// The rule. A kind's entry names one of its moves by its arena.json clip id (`opensAfter`,
// ArenaEnemyMove::clip). When the slot begins that move at t, the window opens at the end of its
// active part, t + startup + active, for `opens` seconds (the move's follow-through: white_glove's
// Deal throw, whose recovery is 1.6 s, opens its glove's seam for exactly that). A new use replaces
// an open window. A crit needs, at the blow's time, the window open (from its start, included, to its
// end, excluded), and the blow from inside a point's gate (ArenaWeakGate, the boss's own frame). The
// blow is judged locked on (CritWindow::judgeLockOn: the centre of the best gated point): a crit is
// the point's multiplier on both its damage and its poise. Every other blow passes as it is, the
// slot's own blows on the lead included.
//
// Time is the fight's clock in seconds (ArenaFight::time; ArenaBlow::time). A start time that is not
// finite opens nothing. Positions and facings are ArenaFight's ground plane, as in ArenaBoss.h.
//
//   ftcore::ArenaKindCrit crit;                        // one per enemy slot whose kind has an entry
//   crit.reset(*table.find(kindId));                    // at the fight's begin and restart
//   after each tick, for e in out.events with e.type AttackStarted and e.source == slot:
//     crit.onAttackStarted(e.move, fight.time(), fight.def().enemies[slot]);
//   in the blow hook:
//     return crit.rule(blow, slot, blow.at, fight.output().enemies[slot].facing);
//   crits(), lastPoint() and lastDamage() say what landed; isOpen(t) and closesAt() what is open.
//
// The data (Data/Battle/arena_crit.json, keyed by arena.json enemy kind id; a sidecar, so arena.json
// and ArenaEnemyKind carry nothing of it):
//   { "version": 1, "kinds": {
//       "white_glove": { "opensAfter": "deal", "opens": 1.6,
//                        "weakPoints": { "halfWidth": 0.5, "points": [ { "id": "fingertip_seam",
//                            "x": -0.22, "y": 0.37, "radius": 0.08, "multiplier": 2.0,
//                            "facing": 0, "arc": 90 } ] },
//                        "effects": { "socket": "middle_03_r", "tell": "dealer_deal_tell", "open": "dealer_seam_open",
//                                     "crit": "dealer_seam_crit", "hit": "ink_hit" } } } }
//   kinds: 0..kMaxArenaKindCrits, each id 1..kMaxArenaKindCritId letters, digits, _ and - (as
//   arena.json's), unique. opensAfter: a clip id, 1..kMaxArenaClipId of the same. opens 0.1..10 s.
//   weakPoints: ArenaBoss.h's (importArenaWeakPoints), with 1 point or more. effects (optional, each key
//   optional): names of 1..kMaxArenaKindCritId of the same characters (ArenaKindCritEffects). Figure space is
//   WeakPoint.h's, seen from the gate: through a front gate (facing 0) the kind's own right hand is on
//   the viewer's left, at negative x. The loader doesn't read arena.json: a kind id it lacks, or a clip
//   none of the kind's moves plays, is the host's to check (opensOn) when it installs the entry.
//   An unknown key is a warning; anything else wrong is an error with its path, and `out` is left
//   default.
//
// Nothing here allocates except importArenaKindCrits (load time).

#include "ftcore/Api.h"
#include "ftcore/ArenaBoss.h"
#include "ftcore/ArenaFight.h"
#include "ftcore/Explore.h" // Vec2

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

inline constexpr std::size_t kMaxArenaKindCrits = 8;   // kinds in the file
inline constexpr std::size_t kMaxArenaKindCritId = 31; // characters of a kind id

// What the host shows for an entry, by name (all optional, empty when unset): the bone or socket of the kind's
// mesh the effects hang from, and the Data/Vfx effects played on the opening move's start (tell), while the
// window is open (open), on a crit (crit) and on any other blow of the lead's on the slot (hit).
struct ArenaKindCritEffects {
    std::array<char, kMaxArenaKindCritId + 1> socketChars{};
    std::array<char, kMaxArenaKindCritId + 1> tellChars{};
    std::array<char, kMaxArenaKindCritId + 1> openChars{};
    std::array<char, kMaxArenaKindCritId + 1> critChars{};
    std::array<char, kMaxArenaKindCritId + 1> hitChars{};

    std::string_view socket() const { return std::string_view(socketChars.data()); }
    std::string_view tell() const { return std::string_view(tellChars.data()); }
    std::string_view open() const { return std::string_view(openChars.data()); }
    std::string_view crit() const { return std::string_view(critChars.data()); }
    std::string_view hit() const { return std::string_view(hitChars.data()); }
};

// One kind's entry.
struct ArenaKindCritDef {
    std::array<char, kMaxArenaKindCritId + 1> kindChars{};
    std::array<char, kMaxArenaClipId + 1> opensAfterChars{}; // the clip of the move that opens the window
    double opens = 1.0;                                      // seconds the window stays open
    ArenaWeakPoints weakPoints{};
    ArenaKindCritEffects effects{};

    std::string_view kind() const { return std::string_view(kindChars.data()); }
    std::string_view opensAfter() const { return std::string_view(opensAfterChars.data()); }
};

struct ArenaKindCritTable {
    std::array<ArenaKindCritDef, kMaxArenaKindCrits> kinds{};
    std::uint8_t count = 0;

    std::size_t size() const { return count; }
    const ArenaKindCritDef* begin() const { return kinds.data(); }
    const ArenaKindCritDef* end() const { return kinds.data() + count; }
    // The entry of that kind id, or null.
    const ArenaKindCritDef* find(std::string_view kind) const {
        for (std::size_t i = 0; i < count; ++i)
            if (kinds[i].kind() == kind) return &kinds[i];
        return nullptr;
    }
};

// Reads Data/Battle/arena_crit.json into `out`. On failure `out` is default and *error lists each
// problem with its path, one per line.
FTCORE_API bool importArenaKindCrits(std::string_view json, ArenaKindCritTable& out, std::string* error = nullptr,
                                     std::vector<std::string>* warnings = nullptr);

class ArenaKindCrit {
public:
    // Ready for a fight with `def`'s entry (copied): closed, no openings, no crits.
    FTCORE_API void reset(const ArenaKindCritDef& def);
    // The slot began move `move` of `kind` at t. When the move's clip is opensAfter, the window opens
    // from t + startup + active for `opens` seconds: true. Anything else changes nothing: false.
    FTCORE_API bool onAttackStarted(std::uint8_t move, double t, const ArenaEnemyKind& kind);
    // A blow about to land, the slot's body at `bossPos` facing `bossFacing` (unit). Only a lead's
    // blow on `slot` (source kArenaPlayer, target slot) is judged; every other one, a bonus (HostHit) too, gets
    // {1, 1, false} and counts nothing.
    FTCORE_API ArenaBlowRuling rule(const ArenaBlow& blow, std::uint8_t slot, Vec2 bossPos, Vec2 bossFacing);
    // Some move of `kind` plays the opensAfter clip (the host's check when it installs the entry).
    FTCORE_API bool opensOn(const ArenaEnemyKind& kind) const;
    // The host's own opening (a refusal's): the window opens from t for `seconds` (one open at t that closes later is
    // kept): true. Nothing while shut, for seconds <= 0 or a time not finite: false.
    FTCORE_API bool openFor(double t, double seconds);
    // Shut: nothing opens the window (onAttackStarted and openFor open nothing and say false) and a window open or
    // due closes now. Unshut lets openings through again; nothing reopens. reset() unshuts.
    FTCORE_API void setShut(bool shut);
    bool shut() const { return shut_; }

    bool isOpen(double t) const { return window_.isOpen(t); }
    double opensAt() const { return opensAt_; }
    double closesAt() const { return window_.closesAt(); }
    int openings() const { return window_.openings(); }
    // Crits landed since reset (a serial: the host notes what it has seen), and the last one's point
    // (an index into points(); kMaxWeakPoints before the first), multiplier, hp (the blow's damage
    // times the multiplier, rounded, as ArenaFight lands it) and time.
    int crits() const { return crits_; }
    std::uint8_t lastPoint() const { return lastPoint_; }
    float lastMultiplier() const { return lastMultiplier_; }
    int lastDamage() const { return lastDamage_; }
    double lastCritAt() const { return lastCritAt_; }
    std::string_view opensAfter() const { return std::string_view(opensAfter_.data()); }
    double opens() const { return opens_; }
    const ArenaWeakPoints& points() const { return window_.points(); }

private:
    CritWindow window_{};
    std::array<char, kMaxArenaClipId + 1> opensAfter_{};
    double opens_ = 0.0;
    double opensAt_ = -1.0e300;
    int crits_ = 0;
    std::uint8_t lastPoint_ = kMaxWeakPoints;
    float lastMultiplier_ = 1.0f;
    int lastDamage_ = 0;
    double lastCritAt_ = -1.0e300;
    bool shut_ = false;
};

} // namespace ftcore
