#pragma once

// The first Arena boss (id `stone_bearer`): its own rules on top of the generic pieces (ArenaBoss.h).
// A heavy stone figure that carries a slab on its back and grows heavier each time the lead meets its
// blows head-on instead of turning them. The data is Data/Battle/arena_boss/stone_bearer.json.
//
// * Weight (WeightStacks). The boss carries 0..cap stacks (cap 5).
//     - It gains them when the lead meets its blows with a Guard: `guard` (1) for a blow the lead
//       blocks that has no called answer (onBlocked: ArenaFight's Blocked; a timed guard counts too),
//       a called blow's own `guardStacks` for each of its impacts Guarded, and its `missStacks` for each
//       impact Missed or WrongZone; `soloLift` (1) when the lead lifts a stone alone (onSoloLift).
//     - It sheds `interceptShed` (1) for each Intercepted impact flagged `sheds`, and `critShed` (1) for
//       the first crit of each weak-point opening (later crits in the same opening shed nothing).
//     - Dodges, jumps and parries change nothing (there is no entry for them).
//     - Each stack adds `damagePerStack` (0.10) to the boss's damage (damageScale(), on the lead's
//       damage taken from its blows) and takes `defencePerStack` (0.10) off the damage it takes
//       (takenScale(), never below 0; the loader keeps cap x defencePerStack below 1).
//     - At `shockRingAt` (3) or more its floor blows send a shock ring (shockRing(move)); at
//       `poiseFreezeAt` (5) or more its poise is frozen: hits do no poise damage and a Stagger the host
//       reports anyway is ignored, so the slab cannot break.
//   StacksChanged on every change (value = the stacks, amount = the change), ShockRing and PoiseFrozen
//   (value 1 / 0) as each threshold is crossed either way.
//
// * The nape: the data's one weak point, gated behind the boss (facing 180, a 60-degree half-arc), x2.0.
//   It opens for a move's `opens` seconds after every impact of an intercept move is Intercepted (3 s
//   after the called blows, 4 s after the marked throw).
//
// * The slab is the breakable: the first Stagger breaks it, and each phase plays its broken table from
//   then on (the doubled and halved blows).
//
// * The arch clamp: the data's keepOut is a disc on the guest's spot at the open arch. The host clamps
//   every arc, lane and area the boss swings with it (ArenaKeepOut), so nothing reaches the arch.
//
// * The pedestal (PedestalPhase). Once, when the boss's hp share first falls to `pedestal.at` (0.25),
//   it steps onto its pedestal: Invulnerable on, the live move cancelled, every move refused. From
//   `firstCallBeats` after that the guest calls every `callEveryBeats` (4) beats (Call events with move
//   kArenaBossNoMove; the miss shift moves them too). A Release (onRelease) within `releaseWindow`
//   (0.15 s) either side of the nearest call, with Load above kArenaReleaseOpenLoad (a third), lifts
//   it: Invulnerable off, Lifted, and the boss is down for `downTime` (3.0 s; every move refused) until
//   Recovered. Any other Release costs nothing (OffBeat, LowLoad).
//
// The host (the wiring session) drives one StoneBearer beside the boss's ArenaFight body:
//   begin(def, now)                   when the fight starts.
//   startMove(move, t, side)          on the boss's AttackStarted, with the move it chose from table()
//                                     and the side of it the lead stands on (arenaSideOf); refused while
//                                     invulnerable or down.
//   update(now, hpShare)              every frame, after ArenaFight's tick.
//   intercept/guard Down/Up(zone, t)  the lead's Intercept and Guard buttons, with the zone from
//                                     arenaZoneOf (the lane to the marked throw's point for Path).
//   onBlocked(move, t)                ArenaFight's Blocked on the lead against a boss blow; ignored for
//                                     intercept moves, whose Guard the called blow already counted.
//   onSoloLift(t)                     the lead lifted a stone alone.
//   onStagger(t)                      ArenaFight's Staggered on the boss.
//   onHit(from, pos, facing, t, at)   before ArenaFight applies a lead's blow: multiply its damage by
//                                     StoneBearerHit::damage and its poise damage by ::poise.
//   onRelease(t, load)                the lead's Release, with the Load before it is spent.
// and acts on what comes back: damageScale() on the boss's blows, shockRing(move) on its floor blows,
// guardBreak on a Guarded marked throw, the clamps, the calls (voice and HUD), LoadGained, and the
// pedestal's states. What is left for the wiring: an ArenaEnemyKind and moves for the boss in the
// arena data, choosing moves from table(), the zones from the lead's position, and the stone pickups
// behind onSoloLift.
//
// Data, after importArenaBoss's keys (ArenaBoss.h):
//   "weight": { "cap": 5, "damagePerStack": 0.1, "defencePerStack": 0.1, "shockRingAt": 3,
//               "poiseFreezeAt": 5, "guard": 1, "soloLift": 1, "interceptShed": 1, "critShed": 1 },
//   "pedestal": { "at": 0.25, "firstCallBeats": 4, "callEveryBeats": 4, "releaseWindow": 0.15,
//                 "downTime": 3.0 }
//   weight: cap 1..10; damagePerStack 0..0.5; defencePerStack 0..0.5 with cap x it below 1; shockRingAt
//   and poiseFreezeAt 1..cap; guard, soloLift, interceptShed and critShed 0..5. pedestal: at in (0, 1);
//   firstCallBeats 0..16; callEveryBeats 1..16; releaseWindow 0.02..0.5 s, below half the call period;
//   downTime 0..10 s. Both objects are required; errors and warnings as importArenaBoss.
//
// Nothing here allocates except importStoneBearer (load time).

#include "ftcore/Api.h"
#include "ftcore/ArenaBoss.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

struct WeightTuning {
    int cap = 5;
    float damagePerStack = 0.10f;
    float defencePerStack = 0.10f;
    int shockRingAt = 3;
    int poiseFreezeAt = 5;
    int guard = 1;
    int soloLift = 1;
    int interceptShed = 1;
    int critShed = 1;
};

class WeightStacks {
public:
    void reset(const WeightTuning& tuning) {
        tuning_ = tuning;
        stacks_ = 0;
    }
    // Adds n (negative sheds), kept in 0..cap. Returns the change made.
    int change(int n) {
        const int before = stacks_;
        stacks_ += n;
        if (stacks_ > tuning_.cap) stacks_ = tuning_.cap;
        if (stacks_ < 0) stacks_ = 0;
        return stacks_ - before;
    }
    int stacks() const { return stacks_; }
    float damageScale() const { return 1.0f + tuning_.damagePerStack * static_cast<float>(stacks_); }
    float takenScale() const {
        const float s = 1.0f - tuning_.defencePerStack * static_cast<float>(stacks_);
        return s > 0.0f ? s : 0.0f;
    }
    bool shockRing() const { return stacks_ >= tuning_.shockRingAt; }
    bool poiseFrozen() const { return stacks_ >= tuning_.poiseFreezeAt; }
    const WeightTuning& tuning() const { return tuning_; }

private:
    WeightTuning tuning_{};
    int stacks_ = 0;
};

struct PedestalTuning {
    float at = 0.25f;            // hp share that starts it (once)
    double firstCallBeats = 4.0; // the guest's first call, this long after it starts
    double callEveryBeats = 4.0; // then every this many beats
    double releaseWindow = 0.15; // a Release this close to a call (either side) lifts it
    double downTime = 3.0;       // the boss is down this long after
};

enum class PedestalRelease : std::uint8_t { Lifted, OffBeat, LowLoad, NotHolding }; // append only

class PedestalPhase {
public:
    void reset(const PedestalTuning& tuning) {
        tuning_ = tuning;
        trigger_ = ArenaHpTrigger{tuning.at};
        holding_ = false;
        downUntil_ = -1.0e300;
        lifted_ = false;
    }
    // True when the hp share starts it, at `now`.
    bool update(float hpShare, double now) {
        if (!trigger_.update(hpShare)) return false;
        holding_ = true;
        from_ = now;
        return true;
    }
    // The first call (with the guest's shift) and the call nearest to t.
    double firstCall(const GuestCaller& guest) const { return guest.cue(from_, tuning_.firstCallBeats); }
    double nearestCall(const GuestCaller& guest, double t) const {
        return guest.nearestRepeat(from_ + arenaBeats(tuning_.firstCallBeats, guest.bpm()), tuning_.callEveryBeats, t);
    }
    double nextCall(const GuestCaller& guest, double t) const {
        return guest.nextRepeat(from_ + arenaBeats(tuning_.firstCallBeats, guest.bpm()), tuning_.callEveryBeats, t);
    }
    // A Release at t with `load`.
    FTCORE_API PedestalRelease release(const GuestCaller& guest, double t, int load);

    bool holding() const { return holding_; }
    bool down(double t) const { return t < downUntil_; }
    bool lifted() const { return lifted_; }
    double from() const { return from_; }
    double downUntil() const { return downUntil_; }
    const PedestalTuning& tuning() const { return tuning_; }

private:
    PedestalTuning tuning_{};
    ArenaHpTrigger trigger_{};
    bool holding_ = false;
    bool lifted_ = false;
    double from_ = 0.0;
    double downUntil_ = -1.0e300;
};

struct StoneBearerDef {
    ArenaBossDef boss;
    WeightTuning weight;
    PedestalTuning pedestal;
};

// Reads Data/Battle/arena_boss/stone_bearer.json (the generic keys through importArenaBoss).
FTCORE_API bool importStoneBearer(std::string_view json, StoneBearerDef& out, std::string* error = nullptr,
                                  std::vector<std::string>* warnings = nullptr);

// What a lead's blow on the boss does, as multipliers on the blow's own damage and poise damage.
struct StoneBearerHit {
    float damage = 1.0f; // 0 while invulnerable; else the crit's multiplier x takenScale()
    float poise = 1.0f;  // 0 while invulnerable or poise-frozen; else the crit's multiplier
    CritResult crit;
};

class StoneBearer {
public:
    // Starts `def` (kept by reference: it must outlive the boss) at `now`.
    FTCORE_API void begin(const StoneBearerDef& def, double now);

    FTCORE_API const ArenaBossEvents& startMove(std::uint8_t move, double t, CalledZone side, bool* started = nullptr);
    FTCORE_API const ArenaBossEvents& update(double now, float hpShare);
    FTCORE_API const ArenaBossEvents& interceptDown(CalledZone zone, double t);
    FTCORE_API const ArenaBossEvents& interceptUp(double t);
    FTCORE_API const ArenaBossEvents& guardDown(CalledZone zone, double t);
    FTCORE_API const ArenaBossEvents& guardUp(double t);
    FTCORE_API const ArenaBossEvents& onBlocked(std::uint8_t move, double t);
    FTCORE_API const ArenaBossEvents& onSoloLift(double t);
    FTCORE_API const ArenaBossEvents& onStagger(double t);
    FTCORE_API const ArenaBossEvents& onHit(Vec2 from, Vec2 pos, Vec2 facing, double t, const AimPoint* at,
                                            StoneBearerHit* hit);
    FTCORE_API const ArenaBossEvents& onRelease(double t, int load, PedestalRelease* result = nullptr);

    int stacks() const { return weight_.stacks(); }
    float damageScale() const { return weight_.damageScale(); }
    float takenScale() const { return weight_.takenScale(); }
    bool poiseFrozen() const { return weight_.poiseFrozen(); }
    // Move `move` is a floor blow and the stacks send a ring with it.
    bool shockRing(std::uint8_t move) const {
        return move < def_->boss.moveCount && def_->boss.moves[move].floor && weight_.shockRing();
    }
    bool invulnerable() const { return pedestal_.holding(); }
    bool down() const { return pedestal_.down(run_.now()); }
    const ArenaBossMoveTable& table() const { return run_.table(); }
    const ArenaBossRun& run() const { return run_; }
    const PedestalPhase& pedestal() const { return pedestal_; }
    const StoneBearerDef& def() const { return *def_; }

private:
    void weigh(int n, double at);
    const ArenaBossEvents& finish();
    const ArenaBossEvents& tick(double t);

    const StoneBearerDef* def_ = nullptr;
    ArenaBossRun run_{};
    WeightStacks weight_{};
    PedestalPhase pedestal_{};
    float hpShare_ = 1.0f;
    double nextCall_ = 0.0;
    bool recoverDue_ = false;
};

} // namespace ftcore
