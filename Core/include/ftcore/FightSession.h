#pragma once

// The fight session: one object that owns every shared combat input of a fight (the count, the gift slot, each
// fighter's places, the table, the held refusal, the weak-point lock and an optional boss script) and moves them all
// on one fight clock. See docs/SHARED_COMBAT_INPUTS.md section 7.
//
// * One clock. tick(fightDt) is the only time input: the session adds the step to its clock and updates every piece
//   with it, each frame. No call takes a time; presses and blows are stamped with the session's now(). The host
//   passes 0 while its hit-stop holds, so the shared inputs see fight time that does not advance during hit-stop.
// * A step that is negative or not finite counts as 0, and one step is at most kMaxFightStep, so the clock can never
//   jump far enough for a piece to refuse it (kMaxTimeJump).
// * Events: tick() returns what each piece emitted since the last tick (calls between ticks buffer theirs).
// * Glue: a blow judged on the count (a shared outcome, at once or when a held blow settles) is a shared blow on the
//   boss script, or an offer of the shared-blow key to the session's own lock when there is no boss script. A settle
//   the count's full event buffer lost (droppedSettles) is not a shared blow. A change to
//   the table setter's places re-checks the table's seats.
// * The count's Load and the Lift (def.lift; off by default, and off nothing below happens). The host feeds each
//   lead's Load (setLoad; a lead never fed has no gauge) before it judges blows. When a window becomes joined, each
//   lead joiner with a gauge stores countLoad, at most once per countLoadEvery per lead; a Lift stores none and each
//   lead joiner spends min(liftCost, its Load) instead. These come as the next tick's `loads`; the host applies them
//   to its gauges (its own capped store and spend). A kThirdSlot caller joins without a gauge (StrikeCount.h).
// * The kept gauge (def.lift with def.keptLead a lead): the session keeps that lead's Load itself (the partner lead,
//   whose gauge no host keeps). Its cap is the chairs' formula, baseCarry + perPlace x keptNamed (at most kMaxLoad).
//   It fills from its joined counts, capped at the cap (no chair gain), and a Lift spends from it. keptParried and
//   keptBlocked (parried damage x keptParryShare, blocked damage x keptBlockShare) store as the host's do, for a
//   partner that defends: the arena's scripted partner does not yet, so no host calls them. setKeptDown(true) (the
//   partner is down): it stores and spends nothing and counts 0 toward a Lift, until the next begin. The session
//   feeds it to the count itself; its changes still come in `loads`, marked kept (already applied).
// * The Lone Carry (def.loneLead a lead; LoneCarry.h): that lead is Lone from begin until a gift is given (giveGift's
//   given; a press refused for any reason leaves it Lone), endLone(PartnerCalled) or finish, then not for the rest
//   of the fight; begin makes it Lone again. While Lone: setTable is refused (TableError::Lone, for any setter),
//   nobody joins the count (another striker's blow is judged Alone at x1 without touching the count, so the lead's
//   own call, unanswered, lands at callerAloneMultiplier), and poolRelease adds a Release's spent Load to the
//   fight's lone pool (saturating). The host mirrors lone() into its fight (ArenaFight::setLone) and refuses take
//   half itself; after finish(true) it calls foldLone(save) to put the fight's lone Releases in the save's
//   LoneSave. The tick carries loneCarryBegan and loneCarryEndedBy once each; at the fight's end the host reads
//   loneEndedBy(), since no tick may follow finish.
// * Marks: markEnemy(enemy, bonus, seconds) marks an enemy slot (a Release's chair effect). The joined blows of the
//   next joined window judged on that enemy (judgeHeldBlow, held ones at their settle) take x (1 + bonus); the mark
//   is used up once that window is over, or ends unused at its time. begin clears every mark.
// * Story: with a story state bound, every press of the gift button aimed at a lead is one press in it
//   (storyCountPress), given or refused (cooling, a refused count, a hostile or vacant slot): the side is the
//   target lead (kLeadA 0, kLeadB 1), the kind is the selected gift (Heal 0, Guard 1, Count 2). A press at a
//   non-lead has no side and writes nothing.
//   The per-kind press counters (counter wides) roll back with the fight step (storyEndStep: the story marks the
//   step as it opens and goes back to the mark on a retry or a quit), so a retried fight counts afresh. The
//   binding enforces it: it lasts one fight (begin unbinds) and is refused unless the state is still at the
//   step's mark, so a screen that retries a fight itself must roll the last attempt back (storyEndStep with
//   Retry), mark the step again and bind anew, or the new attempt writes nothing. Each attempt thrown away counts
//   one story retry, whoever retries it: a screen's own "fight again" adds 1 (its Retry), and a lost fight the
//   story retries adds 1 more (the director's Retry verdict). A phase retry is not one: the story stays tied to
//   the fight-start mark, so the binding stays and nothing rolls back or forward (see Checkpoints).
//   A press refused Down (below) is no press anywhere, the same as NotHurt.
// * Down, takeover and defeat (def.inFight: the leads in this fight, both by default). setLeadHealth is also the
//   fall feed: a lead in the fight at 0 or below (a finite value: an overkill under 0 counts, though it is stored
//   as unknown for the gift Heal) is down, and above 0 it stands again; NaN or an infinity changes nothing. A lead
//   at 0 is down, not dead: while the other lead in the fight stands, the fight goes on. When the controlled lead
//   (activeLead) falls and the other stands, control passes to it at once (tick.tookOver). It never passes back
//   by itself: only setActiveLead (the player's switch) moves it, and that refuses a lead that is down or not in
//   the fight. The fallen lead stands again by itself after the partner's downSeconds (30 s) at its riseHealth
//   (25%): the partner's brain drives the uncontrolled body, which a fallen lead always is, so it times the fall
//   and asks for the rise (PartnerBrain rule 5) and the host stands the body and feeds its health. Every lead in
//   the fight down at once is a defeat (tick.defeated, once); after it nothing falls or stands until a retry or a
//   begin. Each fall and rise comes once, on the next tick (leadsFell, leadsRose).
//   A lead the def marks cannotFall (per fight, per lead; false by default) never goes down in that fight: a
//   blow that would take it to 0 or below leaves it at 1 hp (blowOnLead does the sum for the host) and raises
//   tick.heldUp once, on the next tick. With every lead in the fight marked, health never loses the fight: what
//   ends it then (a round timer, say) is the host's.
//   A gift pressed at a lead who is down is refused Down before the slot sees it: no charge, no cooldown, no
//   thread, no tally, no story press. A down lead stands only by time (the brain's rise) or at a retry.
// * Checkpoints (FightCheckpoint lists every field). The fight-start checkpoint is the state once the host has
//   set the fight up: the first setHostCheckpoint or the first tick after a begin takes it, whichever comes
//   first. With a boss script whose def says checkpointAtPhase, the end of a tick that changed its phase takes a
//   phase checkpoint; checkpointPhase() takes one for a boss the host runs itself (its def's checkpointAtPhase).
//   The host writes its own part (setHostCheckpoint) right after the call or tick that reports one
//   (tick.checkpoint). retry() goes back to the latest: every piece starts clean, as at a begin, at the
//   checkpoint's clock, and then the checkpoint's fields are put back exactly. A defeat costs nothing but the
//   attempt: the Load, the currency and the gift tally come back as the checkpoint has them. A checkpoint holds
//   the fight's state only. A retry to the fight start unbinds the story, as a begin does (the host rolls the
//   step back and binds anew); a phase retry leaves the binding and the story state exactly as they are.
// * The session keeps its own copy of the boss def; it cannot be copied or moved (the script points into it).
// * No allocation after construction (the UE wrapper allocates its session once, when it is made).

#include "ftcore/BossScript.h"
#include "ftcore/CombatKind.h"
#include "ftcore/CombatInputs.h"
#include "ftcore/GiftSlot.h"
#include "ftcore/HeldRefusal.h"
#include "ftcore/Load.h"
#include "ftcore/LoneCarry.h"
#include "ftcore/PartnerBrain.h"
#include "ftcore/Places.h"
#include "ftcore/StoryState.h"
#include "ftcore/StrikeCount.h"
#include "ftcore/TableSet.h"
#include "ftcore/WeakPointLock.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <span>

namespace ftcore {

// One tick's fight time at most (seconds). A host frame longer than this loses the rest, as a paused game would.
inline constexpr double kMaxFightStep = 0.25;
// The furthest a fight may start from 0 on the fight clock (about 31 years in seconds). Further out a double's
// spacing grows past a frame's step, so `now += step` would stop moving the clock; begin clamps to it.
inline constexpr double kMaxFightStart = 1.0e9;

// The shared inputs a press can make, for a host's binding table to map its actions onto. Append only.
enum class SharedAction : std::uint8_t {
    Count,      // call the count
    Gift,       // give the selected gift to `target`
    RefuseDown, // the refusal button went down (the host maps its held refusal button here)
    RefuseUp,   // and came up
    SetTable,   // set the table (needs the setter's Load: use setTable)
    LeaveOpen,  // leave a place open
};

struct FightSessionDef {
    CountTuning count{};
    bool lift = false; // the count's Load and the Lift (see above)
    LiftTuning liftTuning{};
    GiftTuning gift{};
    PlaceTuning places{};
    int seats = 3;
    TableTuning table{};
    // The session's own lock, used when there is no boss script (a boss script has its own, from its def).
    bool locked = false;
    LockTuning lock{};
    bool hasBoss = false;
    BossScriptDef boss{};
    // The kept gauge (see above; only with lift): the lead whose Load the session keeps (kNoFighter: none), its
    // Named chairs (0..kMaxPlaces) and the shares of a parried (0..2) and a blocked (0..1) blow it stores.
    FighterId keptLead = kNoFighter;
    int keptNamed = 1;
    float keptParryShare = 1.0f;
    float keptBlockShare = 0.25f;
    // One blow's multiplier from all sources kept to this (StrikeCount.h capBlowScale; 0: no cap, else 1..20). With a
    // cap, judgeHeldBlow takes the base before crits and the scale the blow landed at, and a settle stays under it.
    float blowCap = 0.f;
    // The Lone Carry's lead (kNoFighter: nobody Lone) and its tuning (LoneCarry.h).
    FighterId loneLead = kNoFighter;
    LoneTuning loneTuning{};
    // The leads in this fight, by lead (kLeadA, kLeadB): a fight with one lead has only that one. At least one.
    std::array<bool, 2> inFight{true, true};
    // The leads that cannot fall in this fight, by lead: a blow leaves them at 1 hp and raises tick.heldUp.
    std::array<bool, 2> cannotFall{false, false};
};

inline bool validKeptGauge(const FightSessionDef& d) {
    if (d.keptLead == kNoFighter) return true;
    return isLead(d.keptLead) && d.keptNamed >= 0 && d.keptNamed <= static_cast<int>(kMaxPlaces) &&
           d.keptParryShare >= 0.f && d.keptParryShare <= 2.f && d.keptBlockShare >= 0.f && d.keptBlockShare <= 1.f;
}

// What begin() refused: each refused piece keeps its defaults, except a refused boss def, which leaves no boss script.
struct FightSessionCheck {
    bool count = true;
    bool gift = true;
    bool places = true;
    bool table = true;
    bool lock = true;
    bool boss = true;
    bool lift = true;  // false: the lift tuning was refused (the Lift stays off)
    bool kept = true;  // false: the kept gauge was refused (no gauge kept)
    bool cap = true;   // false: the blow cap was refused (no cap)
    bool lone = true;  // false: the Lone Carry was refused (not a lead, or its tuning out of range): nobody Lone
    bool start = true; // false: the start was moved: past kMaxFightStart or infinite (clamped to it), or NaN (0)
    bool party = true; // false: no lead in the fight (both are then)
    // Every piece took its tuning. A moved start is reported in `start` but is not a refusal: the fight began.
    bool ok() const { return count && gift && places && table && lock && boss && lift && kept && cap && lone && party; }
};

// A Load change the count asks of a lead's gauge (def.lift): the host applies it, store or spend. Append only.
// ParryStore and BlockStore come only from the kept gauge.
enum class CountLoadKind : std::uint8_t { CountStore, LiftSpend, ParryStore, BlockStore };
struct CountLoadChange {
    FighterId lead = kNoFighter;
    int amount = 0; // CountStore: the Load to store (above 0); LiftSpend: the Load to spend (above 0)
    CountLoadKind kind = CountLoadKind::CountStore;
    bool kept = false; // the session's kept gauge: already applied (kept stores are what it took, after the cap)
};
// Two joiners per window: a tick between joins holds at most a few.
inline constexpr std::size_t kMaxCountLoadChanges = 8;

// A Release's mark on an enemy slot (markEnemy).
struct EnemyMark {
    float bonus = 0.f;
    double until = -1.0e300;
    bool used = false; // a joined window's blow on the enemy took it; it ends with that window
};
inline constexpr std::size_t kMaxMarkedEnemies = 8;

// A held blow's settle above x1: the extra damage its enemy takes now (the blow itself landed at base).
struct HeldBonus {
    std::uint8_t enemy = 0;
    std::uint8_t move = 0;
    int extraDamage = 0; // round(baseDamage * (multiplier - 1)), above 0
};

// Held blows the session can wait on at once (judgeHeldBlow); one more lands at base with no bonus.
inline constexpr std::size_t kMaxHeldBlows = 16;

// Which checkpoint a tick or call took (None: none).
enum class CheckpointAt : std::uint8_t { None, FightStart, Phase };

// The host's part of a checkpoint: what a retry must put back that the session does not own. The host writes it
// (setHostCheckpoint) when a checkpoint is taken and applies it from retry()'s checkpoint.
struct FightHostCheckpoint {
    std::array<int, 2> hp{};         // each lead's hp, by lead (the host's own scale)
    std::array<int, 2> load{};       // each lead's Load gauge (the kept lead's is the session's: keptLoad)
    std::uint64_t draws = 0;         // the fight's draw state (at the fight's start: its seed)
    PartnerBrain partner{};          // the partner's brain as it stood (a fallen lead's down time is in it)
    PartnerOrder order = PartnerOrder::StayClose; // the order button
    int bossHp = 0;                  // a boss the host runs itself (not a boss script): its hp; 0 when none
    std::uint8_t bossPhase = 0;      // and its phase
    std::int64_t currency = 0;       // what the party carries (a defeat costs none of it)
};

// A checkpoint: everything a retry puts back. The rest of the session (the count and its windows, the table, the
// held refusal, the session's own lock, marks, held blows, the story binding) starts clean, as at a begin.
struct FightCheckpoint {
    CheckpointAt at = CheckpointAt::None; // None: not taken yet (a retry then is a begin again)
    std::uint8_t phase = 0;               // the boss's phase (0 at the fight's start)
    double now = 0.0;                     // the fight clock
    FighterId activeLead = kLeadA;        // the controlled lead
    std::array<float, 2> leadHealth{kHealthUnknown, kHealthUnknown}; // setLeadHealth's stored values
    std::array<bool, 2> down{};           // which leads were down
    GiftSlot gift{};                      // the whole slot: tally, threads, cooldowns, heals given, its state
    std::array<PlaceTable, kMaxFighters> places{}; // each fighter's places
    int keptLoad = 0;                     // the kept gauge's Load
    bool keptDown = false;
    bool lone = false;                    // the Lone Carry: on, ended, why, and this fight's lone Releases
    bool loneEnded = false;
    LoneEnd loneEndedBy = LoneEnd::None;
    std::uint32_t loneReleases = 0;
    std::uint32_t lonePooled = 0;
    BossScript boss{};                    // with a boss script: as it stood (phase, hp, rotation, its lock)
    FightHostCheckpoint host{};           // the host's part
};

// One tick's results. The event buffers are the pieces' own (no copy): read them before the next session call,
// which may refill them; the pointers stay valid until the next tick or begin.
struct FightTick {
    double now = 0.0;
    double step = 0.0; // the fight time this tick added (0 during hit-stop)
    const CountEvents* count = nullptr;
    const TableEvents* table = nullptr;
    const BossEvents* boss = nullptr; // empty when there is no boss script
    int sharedBlows = 0;              // shared blows the settled count blows dealt the boss or the lock
    std::span<const HeldBonus> bonuses; // this tick's held-blow bonuses (the session's own buffer)
    int heldDropped = 0;  // held blows judged since the last tick with no room to wait (they stay at base)
    int heldExpired = 0;  // held blows whose settle never came (a dropped settle): they stay at base
    std::span<const CountLoadChange> loads; // def.lift: the gauges' changes since the last tick (the session's own buffer)
    int loadsDropped = 0;                   // changes with no room in the buffer (never with two joiners a tick)
    bool loneCarryBegan = false;            // the first tick after a begin with a lead Lone (show the bargain)
    LoneEnd loneCarryEndedBy = LoneEnd::None; // the first tick after the Lone Carry ended, and why
    std::uint8_t leadsFell = 0;             // since the last tick: bit (1 << lead) for each lead that fell
    std::uint8_t leadsRose = 0;             // and for each that stood again
    std::uint8_t heldUp = 0;                // and for each cannotFall lead a blow left at 1 hp
    FighterId tookOver = kNoFighter;        // the lead that took control when the controlled lead fell
    bool defeated = false;                  // once: every lead in the fight down at once
    CheckpointAt checkpoint = CheckpointAt::None; // taken since the last tick: write the host's part now
};

// What a press did, for the generic press(); the typed calls return the pieces' own results.
struct SharedReply {
    bool accepted = false;
    CountError count = CountError::None;
    GiftResult gift{};
    RefusalInput refusal = RefusalInput::Ignored;
    PlaceError place = PlaceError::None;
};

// The story's hand-off to a fight, as the host sees it when the fight begins or restarts: whether the story handed
// this fight over (a valid carry), whether that carry is already settled (won or lost), and the combat kind it named.
struct FightHandOff {
    bool fromStory = false;
    bool settled = false;
    CombatKind kind = CombatKind::Arena;
};

// Whether a fight's session takes the story's writes: only when the story handed off this kind of fight and it is not
// settled yet. A fight with no hand-off (standalone, demo, a practice run) or a stale one for another kind is unbound.
constexpr bool fightTakesStory(const FightHandOff& h, CombatKind fight) {
    return h.fromStory && !h.settled && h.kind == fight;
}

class FightSession {
public:
    FightSession() { begin(FightSessionDef{}, 0.0); }
    FightSession(const FightSession&) = delete;
    FightSession& operator=(const FightSession&) = delete;

    // Starts a fight at `start` on the fight clock (not bound by kMaxTimeJump). A start past +-kMaxFightStart, or
    // an infinite one, is clamped to +-kMaxFightStart; a NaN start is 0. Either way the fight begins (check.start).
    FightSessionCheck begin(const FightSessionDef& def, double start) {
        def_ = def;
        const FightSessionCheck c = restart(start);
        start_ = now_;
        startCp_.at = CheckpointAt::None;
        phaseCp_.at = CheckpointAt::None;
        startDue_ = true;
        return c;
    }

    // ---- Down, takeover, defeat and checkpoints (see above) ----
    bool inFight(FighterId lead) const { return isLead(lead) && inFight_[lead]; }
    bool down(FighterId lead) const { return isLead(lead) && down_[lead]; }
    bool cannotFall(FighterId lead) const { return inFight(lead) && def_.cannotFall[lead]; }
    // The host's hp sum for a blow of `damage` on a lead at `hp` of `maxHp`: hp - damage, but at least 1 for a lead
    // that cannot fall (raising heldUp when the blow would have taken it to 0 or below). It feeds setLeadHealth
    // with the result over maxHp (when maxHp > 0), so the host needs no second call.
    int blowOnLead(FighterId lead, int hp, int damage, int maxHp) {
        if (!isLead(lead)) return hp - damage;
        int left = hp - damage;
        if (left < 1 && cannotFall(lead)) {
            left = 1;
            if (!defeated_) heldUpDue_ = static_cast<std::uint8_t>(heldUpDue_ | (1u << lead));
        }
        if (maxHp > 0) setLeadHealth(lead, static_cast<float>(left) / static_cast<float>(maxHp));
        return left;
    }
    bool defeated() const { return defeated_; }
    // The checkpoint a retry would go back to now.
    const FightCheckpoint& checkpoint() const { return phaseCp_.at == CheckpointAt::Phase ? phaseCp_ : startCp_; }
    // A phase checkpoint for a boss the host runs itself, when its def says checkpointAtPhase: call it on the
    // step its phase changes (`phase`, the new one), then setHostCheckpoint. False (none taken) after a defeat.
    bool checkpointPhase(std::uint8_t phase) {
        if (defeated_) return false;
        takeStartIfDue();
        takeCheckpoint(phaseCp_, CheckpointAt::Phase, phase);
        return true;
    }
    // The host's part of the latest checkpoint (taking the fight-start one first if it is due: call it once the
    // fight is set up, before the first tick, or right after a tick or call that reports one).
    void setHostCheckpoint(const FightHostCheckpoint& host) {
        takeStartIfDue();
        (phaseCp_.at == CheckpointAt::Phase ? phaseCp_ : startCp_).host = host;
        cpDue_ = CheckpointAt::None; // written: the next tick does not ask for it again
    }
    // Back to the latest checkpoint (see above); the host applies its part from the one returned. Before any
    // checkpoint was taken it is a begin again at the fight's start (the returned one is at None).
    const FightCheckpoint& retry() {
        won_ = false; // an attempt thrown away: the gift tally goes back
        const bool atPhase = phaseCp_.at == CheckpointAt::Phase;
        const FightCheckpoint& cp = atPhase ? phaseCp_ : startCp_;
        StoryState* const story = story_; // a phase retry keeps the story as it is: bound, never rolled back
        const FactRegistry* const reg = reg_;
        restart(cp.at == CheckpointAt::None ? start_ : cp.now);
        if (atPhase) {
            story_ = story;
            reg_ = reg;
        }
        if (cp.at == CheckpointAt::None) {
            startDue_ = true;
            return cp;
        }
        activeLead_ = cp.activeLead;
        leadHealth_ = cp.leadHealth;
        down_ = cp.down;
        gift_ = cp.gift;
        places_ = cp.places;
        kept_ = cp.keptLoad;
        keptDown_ = cp.keptDown;
        if (keptOn_) count_.setLoad(def_.keptLead, keptDown_ ? 0 : kept_);
        lone_ = cp.lone;
        loneEnded_ = cp.loneEnded;
        loneEndedBy_ = cp.loneEndedBy;
        loneReleases_ = cp.loneReleases;
        lonePooled_ = cp.lonePooled;
        loneBeganDue_ = !atPhase && lone_;
        if (hasBoss_) boss_ = cp.boss;
        return cp;
    }

private:
    FightSessionCheck restart(double start) {
        FightSessionCheck c;
        c.start = std::isfinite(start) && start >= -kMaxFightStart && start <= kMaxFightStart;
        now_ = startTime(start < -kMaxFightStart ? -kMaxFightStart : (start > kMaxFightStart ? kMaxFightStart : start));
        c.count = count_.reset(def_.count, now_);
        if (!c.count) count_.reset(CountTuning{}, now_);
        c.lift = count_.setLift(def_.lift, def_.liftTuning);
        lastStore_.fill(-1.0e300);
        pendingLoads_ = 0;
        pendingLoadsDropped_ = 0;
        marks_.fill(EnemyMark{});
        c.kept = validKeptGauge(def_);
        c.cap = validBlowCap(def_.blowCap);
        blowCap_ = c.cap ? def_.blowCap : 0.f;
        keptOn_ = count_.liftOn() && c.kept && def_.keptLead != kNoFighter;
        kept_ = 0;
        keptDown_ = false;
        c.lone = def_.loneLead == kNoFighter || loneStanceAccepted(def_.loneLead, def_.loneTuning);
        lone_ = c.lone && def_.loneLead != kNoFighter;
        leadHealth_ = {kHealthUnknown, kHealthUnknown};
        loneEnded_ = false;
        loneEndedBy_ = LoneEnd::None;
        loneBeganDue_ = lone_;
        loneEndDue_ = false;
        lonePooled_ = 0;
        loneReleases_ = 0;
        loneFolded_ = false;
        {
            const PlaceTuning pt = validPlaceTuning(def_.places) ? def_.places : PlaceTuning{};
            const int cap = pt.baseCarry + pt.perPlace * (c.kept ? def_.keptNamed : 0);
            keptCap_ = cap < kMaxLoad ? cap : kMaxLoad;
        }
        if (keptOn_) count_.setLoad(def_.keptLead, 0);
        // The save-wide gift tally and threads carry over from a won fight; after one not finished as won (a
        // lost or abandoned attempt, retried) they go back to how that fight began.
        if (!won_) gift_.restoreFightStart();
        won_ = false;
        c.gift = gift_.beginFight(def_.gift, now_);
        for (PlaceTable& p : places_) {
            if (!p.reset(def_.seats, def_.places)) {
                c.places = false;
                p.reset();
            }
        }
        c.table = table_.reset(def_.table, now_);
        if (!c.table) table_.reset(TableTuning{}, now_);
        c.lock = lock_.reset(def_.lock, now_);
        if (!c.lock) lock_.reset(LockTuning{}, now_);
        refusal_ = HeldRefusal{};
        // A refused boss def leaves no boss script: the weak point follows the session's lock, as without one.
        c.boss = !def_.hasBoss || boss_.reset(def_.boss, now_);
        hasBoss_ = def_.hasBoss && c.boss;
        lastStory_ = SetResult::Unchanged;
        story_ = nullptr; // a binding lasts one fight: a restart binds again, at a fresh mark
        reg_ = nullptr;
        tick_ = FightTick{};
        heldCount_ = 0; // a held blow from the last fight never pays out
        heldDropped_ = 0;
        tick_.now = now_;
        tick_.count = &kNoCount;
        tick_.table = &kNoTable;
        tick_.boss = &kNoBoss;
        c.party = def_.inFight[kLeadA] || def_.inFight[kLeadB];
        inFight_ = c.party ? def_.inFight : std::array<bool, 2>{true, true};
        // A lead not in this fight is never the controlled one.
        if (!inFight_[activeLead_]) activeLead_ = inFight_[kLeadA] ? kLeadA : kLeadB;
        down_ = {false, false};
        defeated_ = false;
        fellDue_ = 0;
        roseDue_ = 0;
        heldUpDue_ = 0;
        tookOverDue_ = kNoFighter;
        defeatedDue_ = false;
        cpDue_ = CheckpointAt::None;
        startDue_ = false;
        return c;
    }

public:

    // The story state presses are written to, from now until the next begin. Bind only a fight the story handed off
    // (fightTakesStory); a standalone, demo or practice fight stays unbound and writes nothing. False (and unbound)
    // unless the state is at the step's mark (storyAtMark): an attempt whose writes were not rolled back never
    // lets the next one count on top of it.
    bool bindStory(StoryState& story, const FactRegistry& reg, const StoryStepMark& mark) {
        const bool ok = storyAtMark(story, reg, mark);
        story_ = ok ? &story : nullptr;
        reg_ = ok ? &reg : nullptr;
        return ok;
    }
    void unbindStory() {
        story_ = nullptr;
        reg_ = nullptr;
    }
    bool storyBound() const { return story_ != nullptr; }
    // What the last gift's story write returned (Unchanged when none was made).
    SetResult lastStoryWrite() const { return lastStory_; }

    // Moves the fight clock by `fightDt` (0 during hit-stop) and every piece with it.
    const FightTick& tick(double fightDt) {
        takeStartIfDue();
        const double step = std::isfinite(fightDt) && fightDt > 0.0 ? (fightDt < kMaxFightStep ? fightDt : kMaxFightStep) : 0.0;
        now_ += step;
        tick_.now = now_;
        tick_.step = step;
        tick_.sharedBlows = 0;
        tick_.count = &count_.update(now_);
        tick_.heldDropped = heldDropped_;
        heldDropped_ = 0;
        tick_.heldExpired = 0;
        std::size_t bonuses = 0;
        for (const CountEvent& e : *tick_.count) {
            if (sharedBlow(e) && shareBlow()) ++tick_.sharedBlows;
            if (e.type == CountEventType::BlowSettled) settleHeld(e, bonuses);
        }
        // Every held blow has settled or its settle was dropped: what is still waiting never will.
        if (count_.heldBlows() == 0) {
            tick_.heldExpired = static_cast<int>(heldCount_);
            heldCount_ = 0;
        }
        tick_.bonuses = std::span<const HeldBonus>(bonuses_.data(), bonuses);
        // The gauges' changes since the last tick, then the marks: a used one ends with its window, an unused one at its time.
        tickLoads_ = pendingLoads_;
        for (std::size_t i = 0; i < pendingLoads_; ++i) loadsOut_[i] = loadsIn_[i];
        tick_.loads = std::span<const CountLoadChange>(loadsOut_.data(), tickLoads_);
        tick_.loadsDropped = pendingLoadsDropped_;
        pendingLoads_ = 0;
        pendingLoadsDropped_ = 0;
        for (EnemyMark& m : marks_) {
            if ((m.used && !count_.live()) || (!m.used && now_ > m.until)) m = EnemyMark{};
        }
        gift_.update(now_);
        tick_.table = &table_.update(now_);
        lock_.update(now_);
        tick_.boss = hasBoss_ ? &boss_.update(now_) : &kNoBoss;
        tick_.loneCarryBegan = loneBeganDue_;
        tick_.loneCarryEndedBy = loneEndDue_ ? loneEndedBy_ : LoneEnd::None;
        loneBeganDue_ = false;
        loneEndDue_ = false;
        if (hasBoss_ && def_.boss.checkpointAtPhase && !defeated_) {
            for (const BossEvent& e : *tick_.boss) {
                if (e.type == BossEventType::PhaseChanged) {
                    takeCheckpoint(phaseCp_, CheckpointAt::Phase, static_cast<std::uint8_t>(boss_.phase()));
                    break;
                }
            }
        }
        tick_.leadsFell = fellDue_;
        tick_.leadsRose = roseDue_;
        tick_.heldUp = heldUpDue_;
        tick_.tookOver = tookOverDue_;
        tick_.defeated = defeatedDue_;
        tick_.checkpoint = cpDue_;
        fellDue_ = 0;
        roseDue_ = 0;
        heldUpDue_ = 0;
        tookOverDue_ = kNoFighter;
        defeatedDue_ = false;
        cpDue_ = CheckpointAt::None;
        return tick_;
    }

    double now() const { return now_; }

    // The lead the player's figure fills (kLeadA until set): the host aims its gift presses at it and reports its
    // blows and calls for it. The host sets it when the fight begins and whenever the player switches lead; it
    // stays across begin (a party fact, not a fight's), except that a begin moves it off a lead not in the fight.
    // A fall of the controlled lead moves it too (see above). False (unchanged) for a non-lead, a lead not in this
    // fight or a lead that is down.
    bool setActiveLead(FighterId lead) {
        if (!isLead(lead) || !inFight_[lead] || down_[lead]) return false;
        activeLead_ = lead;
        return true;
    }
    FighterId activeLead() const { return activeLead_; }

    // ---- the Lone Carry ----
    bool lone() const { return lone_; }
    FighterId loneLead() const { return lone_ ? def_.loneLead : kNoFighter; }
    bool loneEnded() const { return loneEnded_; } // it ended this fight (loneEndedBy says why)
    LoneEnd loneEndedBy() const { return loneEndedBy_; } // None while Lone or never Lone this fight
    const LoneTuning& loneTuning() const { return def_.loneTuning; }
    // The bargain for the card, from this fight's tuning (the host shows it on loneCarryBegan).
    LoneBargain loneBargain() const { return ftcore::loneBargain(def_.loneTuning); }
    // The host ends the Lone Carry for the partner's call (or any reason but None and Gift, which giveGift owns).
    // False when nobody is Lone.
    bool endLone(LoneEnd why) {
        if (!lone_ || why == LoneEnd::None || why == LoneEnd::Gift) return false;
        endLoneFor(why);
        return true;
    }
    // A Release by `who` that spent `spent` Load: pooled while `who` is the Lone lead (true), else nothing.
    bool poolRelease(FighterId who, int spent) {
        if (!lone_ || who != def_.loneLead || spent <= 0) return false;
        lonePooled_ = saturatingAdd(lonePooled_, static_cast<std::uint32_t>(spent));
        loneReleases_ = saturatingAdd(loneReleases_, 1u);
        return true;
    }
    std::uint32_t lonePooled() const { return lonePooled_; }     // this fight's
    std::uint32_t loneReleases() const { return loneReleases_; } // this fight's lone Releases
    // The roll as it stands: the save's lone Releases (as the host keeps them) plus this fight's, until foldLone
    // has put this fight in the save (then the save's alone), so it never counts a fight twice.
    LoneRoll loneRoll(const LoneSave& saved, const LonePoolTuning& pool = LonePoolTuning{}) const {
        return loneFolded_ ? ftcore::loneRoll(saved, 0u, 0u, pool)
                           : ftcore::loneRoll(saved, loneReleases_, lonePooled_, pool);
    }
    // After finish(true) (the story step kept), once: adds this fight's lone Releases to the save. False (nothing
    // added) before finish(true), after finish(false) or a second time. loneReleases() and lonePooled() still read
    // the fight's own until the next begin.
    bool foldLone(LoneSave& save) {
        if (!won_ || loneFolded_) return false;
        addLoneFight(save, loneReleases_, lonePooled_);
        loneFolded_ = true;
        return true;
    }

    // ---- Presses (stamped with now()) ----

    SharedReply press(SharedAction action, FighterId who, FighterId target = kNoFighter) {
        SharedReply r;
        switch (action) {
        case SharedAction::Count:
            r.count = callCount(who);
            r.accepted = r.count == CountError::None;
            break;
        case SharedAction::Gift:
            r.gift = giveGift(target);
            r.accepted = r.gift.given;
            break;
        case SharedAction::RefuseDown:
            r.refusal = refusal_.holdDown();
            r.accepted = r.refusal != RefusalInput::Ignored;
            break;
        case SharedAction::RefuseUp:
            r.refusal = refusal_.holdUp();
            r.accepted = r.refusal != RefusalInput::Ignored;
            break;
        case SharedAction::LeaveOpen:
            r.place = leaveOpen(who);
            r.accepted = r.place == PlaceError::None;
            break;
        case SharedAction::SetTable: // needs a Load: setTable()
            break;
        }
        return r;
    }

    CountError callCount(FighterId caller, CountVoice voice = CountVoice::Voice) { return count_.call(caller, now_, voice); }
    void falseCount(double shift) { count_.falseCall(now_, shift); }
    void cancelCount() { count_.cancel(now_); }
    void cancelFalseCount() { count_.cancelFalse(now_); }

    bool selectGift(Gift g) { return gift_.select(g); }
    // The host's word on a lead's health (a fraction of its max), for the gift Heal: at exactly 1 a Heal is
    // refused NotHurt. Anything that is not a number in 0..1 (NaN, an infinity, under 0, over 1) is stored as
    // unknown (kHealthUnknown, also the default from begin), and unknown reads as hurt. It is also the fall feed
    // (see above): feed it whenever a lead's hp changes.
    void setLeadHealth(FighterId lead, float fraction) {
        if (!isLead(lead)) return;
        leadHealth_[lead] = std::isfinite(fraction) && fraction >= 0.f && fraction <= 1.f ? fraction : kHealthUnknown;
        fall(lead, fraction);
    }
    float leadHealth(FighterId lead) const { return isLead(lead) ? leadHealth_[lead] : kHealthUnknown; }
    GiftResult giveGift(FighterId target) {
        lastStory_ = SetResult::Unchanged;
        if (down(target) && inFight_[target]) { // a gift never stands a lead up: no press anywhere
            GiftResult refused;
            refused.gift = gift_.selected();
            refused.target = target;
            refused.error = GiftError::Down;
            return refused;
        }
        const GiftResult r = gift_.press(target, now_, &count_, leadHealth(target));
        // A gift given ends the Lone Carry for the rest of the fight; a press refused for any reason did nothing
        // and leaves it.
        if (lone_ && r.given) endLoneFor(LoneEnd::Gift);
        // A NotHurt press is no press anywhere (the slot does not tally it), so the story does not count it either.
        if (story_ && isLead(r.target) && r.gift != Gift::None && r.error != GiftError::NotHurt)
            lastStory_ = storyCountPress(*story_, *reg_, r.target, static_cast<std::uint8_t>(r.gift));
        return r;
    }

    PlaceError layPlace(FighterId who, PersonId person) { return placeChanged(who, isFighter(who) ? places_[who].layPlace(person) : PlaceError::BadPerson); }
    PlaceError leaveOpen(FighterId who) { return placeChanged(who, isFighter(who) ? places_[who].leaveOpen() : PlaceError::BadPerson); }
    PlaceError clearPlace(FighterId who, int chair) { return placeChanged(who, isFighter(who) ? places_[who].clearPlace(chair) : PlaceError::BadPerson); }
    PlaceEmptied emptyPlace(FighterId who, PersonId person, Load* load = nullptr) {
        if (!isFighter(who)) return PlaceEmptied{};
        const PlaceEmptied r = places_[who].emptyPlace(person, load);
        placeChanged(who, PlaceError::None);
        return r;
    }

    TableError setTable(FighterId setter, Load& load, std::uint8_t seated) {
        if (!isFighter(setter)) return TableError::BadSetter;
        if (lone_) return TableError::Lone;
        return table_.begin(setter, load, places_[setter], seated, now_);
    }
    bool interruptTable() { return table_.interrupt(now_); }
    bool cancelTable() { return table_.cancel(now_); }
    bool seatEnemy() { return table_.seatEnemy(now_); }
    TableSplit split(FighterId target, float amount) { return table_.split(target, amount, now_); }

    void offerRefusal() { refusal_.offer(); }
    RefusalInput acceptRefusal() { return refusal_.accept(); }
    // "Not yet" (a yes): the deferral relocks an unlocked point, the boss script's or the session's own lock, when
    // that lock's relockOnDefer says so. An offer that ends unheld (endRefusal) is a deferral too.
    RefusalInput deferRefusal() {
        const RefusalInput r = refusal_.defer();
        if (r == RefusalInput::Resolved) deferLocks();
        return r;
    }
    // Ends the host's prompt; a refusal kept to the end offers the refusal key: to the boss script when there is one
    // (it opens the point only when refusal is its key kind and a counter has the point exposed), else to the
    // session's own lock. Only the call that resolves the refusal offers it: ending an ended prompt again offers nothing.
    RefusalInput endRefusal() {
        const RefusalInput r = refusal_.offerEnded();
        if (r == RefusalInput::Resolved && refusal_.refused()) {
            if (hasBoss_) boss_.offerKey(kUnlockRefusal, now_);
            else if (def_.locked) lock_.offer(kUnlockRefusal, now_);
        } else if (r == RefusalInput::Resolved && refusal_.deferred()) {
            deferLocks();
        }
        return r;
    }

    // A blow by `striker`, judged on the count before its damage; an OnCount (or Lift) blow is a shared blow at once.
    CountHit judgeBlow(FighterId striker) {
        if (lone_ && striker != def_.loneLead) return CountHit{}; // nobody joins a Lone lead: Alone, x1
        if (keptOn_) count_.setLoad(def_.keptLead, keptDown_ ? 0 : kept_);
        const CountHit h = count_.judgeBlow(striker, now_);
        takeJoin();
        if (sharedBlow(h)) shareBlow();
        return h;
    }
    // A fighter's Load now, for the Lift (def.lift); negative: no gauge. Feed it before judging its blows.
    // For the kept lead it sets the kept gauge (0..its cap).
    void setLoad(FighterId f, int load) {
        if (keptOn_ && f == def_.keptLead) {
            kept_ = load < 0 ? 0 : (load > keptCap_ ? keptCap_ : load);
            count_.setLoad(f, keptDown_ ? 0 : kept_);
            return;
        }
        count_.setLoad(f, load);
    }
    // The blow cap (0: none) and a blow's scale kept under it.
    float blowCap() const { return blowCap_; }
    float capBlow(float scale) const { return capBlowScale(scale, blowCap_); }
    // The kept gauge: whether the session keeps `f`'s Load, that Load, its cap.
    bool keepsLoad(FighterId f) const { return keptOn_ && f == def_.keptLead; }
    FighterId keptLead() const { return keptOn_ ? def_.keptLead : kNoFighter; }
    int keptLoad() const { return keptOn_ ? kept_ : 0; }
    int keptCap() const { return keptOn_ ? keptCap_ : 0; }
    // The kept lead parried (blocked) a blow of `damage`: it stores damage x keptParryShare (keptBlockShare), rounded,
    // up to its cap. Returns what it stored (0 with no kept gauge).
    // The kept lead is down (true) or up again: down, its Load stays where it was but is neither stored, spent nor
    // counted toward a Lift.
    void setKeptDown(bool down) {
        keptDown_ = down;
        if (keptOn_) count_.setLoad(def_.keptLead, down ? 0 : kept_);
    }
    bool keptDown() const { return keptDown_; }
    int keptParried(int damage) { return keptStore(damage, def_.keptParryShare, CountLoadKind::ParryStore); }
    int keptBlocked(int damage) { return keptStore(damage, def_.keptBlockShare, CountLoadKind::BlockStore); }
    // Marks enemy slot `enemy`: its next joined window's blows take x (1 + bonus), bonus clamped to
    // 0..kMaxReleaseMark, for `seconds` (0..15) from now. A new mark replaces the old. False (nothing) for a slot
    // past kMaxMarkedEnemies, a bonus of 0 or less, or seconds of 0 or less (or not finite).
    bool markEnemy(std::uint8_t enemy, float bonus, double seconds) {
        if (enemy >= kMaxMarkedEnemies || !(bonus > 0.f) || !(seconds > 0.0) || !std::isfinite(seconds)) return false;
        EnemyMark m;
        m.bonus = bonus < kMaxReleaseMark ? bonus : kMaxReleaseMark;
        m.until = now_ + (seconds < 15.0 ? seconds : 15.0);
        marks_[enemy] = m;
        return true;
    }
    EnemyMark mark(std::uint8_t enemy) const { return enemy < kMaxMarkedEnemies ? marks_[enemy] : EnemyMark{}; }
    // judgeBlow for a blow the host lands at `baseDamage` on `enemy` now. A Held blow lands at base and the session
    // waits on it: when it settles above x1, a later tick's `bonuses` lists the extra damage to add (the host deals
    // it outside a blow). A settle at x1 or below, a dropped settle (heldExpired) or no room to wait (heldDropped)
    // adds nothing. Other outcomes are final: the host multiplies as with judgeBlow.
    // With a blow cap, baseDamage is the blow's damage before crits and `landed` the scale it lands at (its crit).
    CountHit judgeHeldBlow(FighterId striker, std::uint8_t enemy, int baseDamage, std::uint8_t move, float landed = 1.0f) {
        CountHit h = judgeBlow(striker);
        if (sharedOutcome(h.outcome)) h.multiplier *= markFactor(enemy);
        if (h.outcome != CountOutcome::Held || baseDamage <= 0) return h;
        if (heldCount_ == kMaxHeldBlows) {
            ++heldDropped_;
            return h;
        }
        held_[heldCount_++] = HeldRecord{h.blow, enemy, move, baseDamage, landed};
        return h;
    }
    // An unlock key (a read clause with its id, a refusal, a shared blow) offered to the boss script's point when
    // there is one (BossScript::offerKey: only its key kind, only while exposed), else to the session's own lock. A shared
    // blow offered here is the host's own: it skips the count (tick().sharedBlows does not see it).
    UnlockResult offerKey(UnlockKey key, std::uint16_t clause = 0) {
        if (hasBoss_) return boss_.offerKey(key, now_, clause);
        if (!def_.locked) return UnlockResult::BadKey;
        return lock_.offer(key, now_, clause);
    }
    // The weak point is open: always without a lock, else while the lock or the boss script says so.
    bool weakPointOpen() const {
        if (hasBoss_) return boss_.unlocked();
        return !def_.locked || lock_.open(now_);
    }

    // The boss script's inputs (Ignored, or nothing, without one). `shared`: the counter carries the shared input, a
    // blow judged a shared outcome (sharedBlow(judgeBlow(...))) or a hit the lead took half of; a shared counter
    // kind without it is refused (NeedsShared).
    BossInput counter(BossCounter kind, FighterId who, bool shared = false) {
        if (!hasBoss_) return BossInput::Ignored;
        return boss_.counter(kind, who, now_, shared);
    }
    ScriptHit bossHit(int amount, bool onWeakPoint) { return hasBoss_ ? boss_.hit(amount, onWeakPoint, now_) : ScriptHit{}; }
    bool answer() { return hasBoss_ && boss_.answer(now_); }
    void setWatched(bool watched) {
        if (hasBoss_) boss_.setWatched(watched, now_);
    }
    int addResource(int n) { return hasBoss_ ? boss_.addResource(n, now_) : 0; }

    // ---- Reads ----

    const StrikeCount& strikeCount() const { return count_; }
    // For the HUD: Heals still to give this fight, and seconds until the Heal and the Guard are ready (0: ready).
    int giftHealsLeft() const { return gift_.healsLeft(); }
    // The host calls this when the fight ends: won, the next begin keeps this fight's gift presses; not won
    // (or never called), the next begin puts the tally and threads back to this fight's start. To agree with the
    // story's press counters: true wherever the story step ends Keep (a win, or a loss branch it keeps), never
    // when it ends Retry or Drop.
    // A Lone Carry still on ends here (LoneEnd::FightEnded); the host reads loneEndedBy() then, since no tick
    // may follow to carry loneCarryEndedBy.
    void finish(bool won) {
        won_ = won;
        if (lone_) endLoneFor(LoneEnd::FightEnded);
    }
    bool finishedWon() const { return won_; }
    double giftHealReadyIn() const { return gift_.readyIn(Gift::Heal, now_); }
    double giftGuardReadyIn() const { return gift_.readyIn(Gift::Guard, now_); }
    const GiftSlot& giftSlot() const { return gift_; }
    GiftSlot& giftSlot() { return gift_; } // setState, save and load
    // A fighter's places (kLeadA's for an id that is not a fighter). The mutable one is for loads: call recheckTable after.
    const PlaceTable& places(FighterId who) const { return places_[isFighter(who) ? who : 0]; }
    PlaceTable& places(FighterId who) { return places_[isFighter(who) ? who : 0]; }
    const TableSet& table() const { return table_; }
    const HeldRefusal& refusal() const { return refusal_; }
    const WeakPointLock& lock() const { return lock_; }
    bool hasBoss() const { return hasBoss_; }
    const BossScript& boss() const { return boss_; } // meaningful only while hasBoss()
    const FightSessionDef& def() const { return def_; }

    // Re-checks the table's seats against its setter's places (after a host changed them directly).
    bool recheckTable() {
        const TableState s = table_.state();
        if ((s != TableState::Setting && s != TableState::Set) || !isFighter(table_.setter())) return false;
        return table_.checkSeats(places_[table_.setter()], now_);
    }

private:
    PlaceError placeChanged(FighterId who, PlaceError e) {
        if (e == PlaceError::None && who == table_.setter()) recheckTable();
        return e;
    }
    bool shareBlow() {
        if (hasBoss_) return boss_.sharedBlow(now_);
        if (!def_.locked) return false;
        const UnlockResult u = lock_.offer(kUnlockSharedBlow, now_);
        return u == UnlockResult::Unlocked || u == UnlockResult::Refreshed;
    }
    struct HeldRecord {
        std::uint16_t blow = 0;
        std::uint8_t enemy = 0;
        std::uint8_t move = 0;
        int baseDamage = 0;
        float landed = 1.0f;
    };
    // Frees the settled blow's record; above x1 it adds a bonus (at most one per record, so the buffer fits).
    void settleHeld(const CountEvent& e, std::size_t& bonuses) {
        for (std::size_t i = 0; i < heldCount_; ++i) {
            if (held_[i].blow != e.blow) continue;
            const HeldRecord r = held_[i];
            held_[i] = held_[--heldCount_];
            const float mult = sharedOutcome(e.outcome) ? e.multiplier * markFactor(r.enemy) : e.multiplier;
            const int add = heldSettleBonus(r.baseDamage, r.landed, mult, blowCap_);
            if (add < 1) return;
            bonuses_[bonuses++] = HeldBonus{r.enemy, r.move, add};
            return;
        }
    }
    // A joined blow on `enemy`: its mark's factor (1 + bonus), using the mark; 1 with no live mark.
    float markFactor(std::uint8_t enemy) {
        if (enemy >= kMaxMarkedEnemies) return 1.0f;
        EnemyMark& m = marks_[enemy];
        if (!(m.bonus > 0.f) || (!m.used && now_ > m.until)) return 1.0f;
        m.used = true;
        return 1.0f + m.bonus;
    }
    void pushLoad(FighterId lead, int amount, CountLoadKind kind, bool kept = false) {
        if (pendingLoads_ == loadsIn_.size()) {
            ++pendingLoadsDropped_;
            return;
        }
        loadsIn_[pendingLoads_++] = CountLoadChange{lead, amount, kind, kept};
    }
    // A window just joined (def.lift): each lead joiner with a gauge stores countLoad (rate-limited) or, on a Lift,
    // spends min(liftCost, its Load).
    void takeJoin() {
        CountJoin j;
        if (!count_.takeJoin(j)) return;
        const LiftTuning& l = count_.liftTuning();
        // A window joined with the third slot's caller (the lead counting alone) stores the smaller solo amount.
        const int store = (j.first == kThirdSlot || j.second == kThirdSlot) ? l.soloCountLoad : l.countLoad;
        for (const FighterId f : {j.first, j.second}) {
            if (!isLead(f) || !count_.hasGauge(f)) continue;
            const bool kept = keepsLoad(f);
            if (kept && keptDown_) continue;
            if (j.lift) {
                const int held = count_.loadOf(f);
                const int spend = l.liftCost < held ? l.liftCost : held;
                if (spend > 0) {
                    pushLoad(f, spend, CountLoadKind::LiftSpend, kept);
                    count_.setLoad(f, held - spend);
                    if (kept) kept_ = held - spend;
                }
            } else if (store > 0 && (l.countLoadEvery <= 0.0 || now_ - lastStore_[f] >= l.countLoadEvery - 1e-9)) {
                lastStore_[f] = now_;
                if (!kept) {
                    pushLoad(f, store, CountLoadKind::CountStore);
                } else {
                    const int room = keptCap_ - kept_;
                    const int stored = store < room ? store : (room > 0 ? room : 0);
                    kept_ += stored;
                    count_.setLoad(f, kept_);
                    if (stored > 0) pushLoad(f, stored, CountLoadKind::CountStore, true);
                }
            }
        }
    }
    int keptStore(int damage, float share, CountLoadKind kind) {
        if (!keptOn_ || keptDown_ || damage <= 0) return 0;
        const double raw = std::round(static_cast<double>(damage) * static_cast<double>(share));
        const int room = keptCap_ - kept_;
        if (room <= 0 || !(raw > 0.0)) return 0;
        const int stored = raw < static_cast<double>(room) ? static_cast<int>(raw) : room;
        kept_ += stored;
        count_.setLoad(def_.keptLead, kept_);
        pushLoad(def_.keptLead, stored, kind, true);
        return stored;
    }
    void deferLocks() {
        if (hasBoss_) boss_.onDeferred(now_);
        else if (def_.locked) lock_.onDeferred(now_);
    }
    void fall(FighterId lead, float f) {
        if (!inFight_[lead] || defeated_ || !std::isfinite(f)) return;
        const std::uint8_t bit = static_cast<std::uint8_t>(1u << lead);
        if (f > 0.f) {
            if (down_[lead]) {
                down_[lead] = false;
                roseDue_ = static_cast<std::uint8_t>(roseDue_ | bit);
            }
            return;
        }
        if (down_[lead]) return;
        if (def_.cannotFall[lead]) { // held up at 1 hp: never down in this fight
            heldUpDue_ = static_cast<std::uint8_t>(heldUpDue_ | bit);
            return;
        }
        down_[lead] = true;
        fellDue_ = static_cast<std::uint8_t>(fellDue_ | bit);
        const FighterId other = lead == kLeadA ? kLeadB : kLeadA;
        if (!inFight_[other] || down_[other]) {
            defeated_ = true;
            defeatedDue_ = true;
            return;
        }
        if (lead == activeLead_) {
            activeLead_ = other;
            tookOverDue_ = other;
        }
    }
    void takeStartIfDue() {
        if (!startDue_) return;
        startDue_ = false;
        takeCheckpoint(startCp_, CheckpointAt::FightStart, 0);
    }
    void takeCheckpoint(FightCheckpoint& cp, CheckpointAt at, std::uint8_t phase) {
        cp.at = at;
        cp.phase = phase;
        cp.now = now_;
        cp.activeLead = activeLead_;
        cp.leadHealth = leadHealth_;
        cp.down = down_;
        cp.gift = gift_;
        cp.places = places_;
        cp.keptLoad = kept_;
        cp.keptDown = keptDown_;
        cp.lone = lone_;
        cp.loneEnded = loneEnded_;
        cp.loneEndedBy = loneEndedBy_;
        cp.loneReleases = loneReleases_;
        cp.lonePooled = lonePooled_;
        if (hasBoss_) cp.boss = boss_;
        cp.host = FightHostCheckpoint{};
        cpDue_ = at;
    }

    double now_ = 0.0;
    FightSessionDef def_{};
    StrikeCount count_{};
    GiftSlot gift_{};
    std::array<PlaceTable, kMaxFighters> places_{};
    TableSet table_{};
    HeldRefusal refusal_{};
    WeakPointLock lock_{};
    bool hasBoss_ = false;
    BossScript boss_{};
    FighterId activeLead_ = kLeadA;
    StoryState* story_ = nullptr;
    const FactRegistry* reg_ = nullptr;
    SetResult lastStory_ = SetResult::Unchanged;
    FightTick tick_{};
    std::array<HeldRecord, kMaxHeldBlows> held_{};
    std::size_t heldCount_ = 0;
    int heldDropped_ = 0;
    std::array<HeldBonus, kMaxHeldBlows> bonuses_{};
    std::array<double, kMaxFighters> lastStore_{};
    std::array<CountLoadChange, kMaxCountLoadChanges> loadsIn_{};
    std::array<CountLoadChange, kMaxCountLoadChanges> loadsOut_{};
    std::size_t pendingLoads_ = 0;
    std::size_t tickLoads_ = 0;
    int pendingLoadsDropped_ = 0;
    std::array<EnemyMark, kMaxMarkedEnemies> marks_{};
    bool keptOn_ = false;
    int kept_ = 0;
    bool keptDown_ = false;
    int keptCap_ = 0;
    float blowCap_ = 0.f;
    void endLoneFor(LoneEnd why) {
        lone_ = false;
        loneEnded_ = true;
        loneEndedBy_ = why;
        loneEndDue_ = true;
    }
    bool lone_ = false;
    bool loneEnded_ = false;
    LoneEnd loneEndedBy_ = LoneEnd::None;
    bool loneBeganDue_ = false;
    bool loneEndDue_ = false;
    std::uint32_t loneReleases_ = 0;
    bool loneFolded_ = false; // foldLone put this fight in the save
    std::array<float, 2> leadHealth_{kHealthUnknown, kHealthUnknown}; // setLeadHealth, per lead
    bool won_ = false; // finish(true) since the last begin
    std::uint32_t lonePooled_ = 0;
    double start_ = 0.0; // the fight's start (begin's, clamped)
    std::array<bool, 2> inFight_{true, true};
    std::array<bool, 2> down_{};
    bool defeated_ = false;
    std::uint8_t fellDue_ = 0;
    std::uint8_t roseDue_ = 0;
    std::uint8_t heldUpDue_ = 0;
    FighterId tookOverDue_ = kNoFighter;
    bool defeatedDue_ = false;
    bool startDue_ = false;
    CheckpointAt cpDue_ = CheckpointAt::None;
    FightCheckpoint startCp_{};
    FightCheckpoint phaseCp_{};
    // The empty buffers a tick points at before the first tick and when there is no boss script.
    static inline const CountEvents kNoCount{};
    static inline const TableEvents kNoTable{};
    static inline const BossEvents kNoBoss{};
};

} // namespace ftcore
