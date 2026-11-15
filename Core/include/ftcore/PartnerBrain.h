#pragma once

// The partner's brain: what the lead not under the player's hand does in an arena fight, one intent a step. It decides
// only; ArenaFight (through a hook the host adds) feeds it a view each step and applies the intent with the ally's
// actions. Plain data in, one intent out, no allocation per step.
//
// The order (the player's button): StayClose or Spread.
//   1. Target. Spread: the nearest standing enemy the lead is not attacking; if only one enemy stands, that one. When
//      the lead attacks every standing enemy, or none, the nearest standing one. StayClose: the lead's target (none:
//      no target), and never further than leashMeters from the lead: past it the intent is MoveTo back, until within
//      returnMeters (no flicker at the edge); closing on its target it never aims past returnMeters from the lead
//      (so a target in reach only from 3..4 m out is not attacked). A Down enemy is never a target and does not count as standing. The target
//      is chosen when a string starts and kept to its end. Spread keeps a chosen target at least targetHoldSeconds
//      (1.5) before another may take it, unless it falls (Down or absent) or, once it has been within reach, it
//      leaves reach. StayClose has no hold: the lead's target is its target, so the lead changing target (or having
//      none) is the target no longer being attackable, and it follows at once.
//   2. Rhythm. A light string of blowsMin..blowsMax blows, then a pause of pauseMin..pauseMax s, both drawn (seeded)
//      when the string starts. The host says how long each blow of a string lasts (blowSeconds, the i-th light) and
//      keeps to it: a blow once started always runs out (no host can cancel one). The string's clock is those
//      lengths: the next blow starts on the first step at or after the last one ends, and the string is over on the
//      first step after its last blow ends. It never starts a string while its target winds up a blow aimed at it.
//      Budget (a partner never out-fights the player, and a lead pinned down defending does not freeze it): over any
//      rolling budgetWindowSeconds (20) window, its attack time is at most budgetShare (0.60) x the lead's attack
//      time in that window plus budgetAllowanceSeconds (2.0). The host says each step whether the lead is attacking
//      (leadAttacking). The partner's attack time is the whole of every blow it began (whatever it does after: a
//      blow begun counts to its end), and any step the host reports Attacking outside them. Every blow is decided
//      before it starts, on its full length plus what is left of a blow still running, against every window that
//      would hold any of it with the lead attacking no more; so a blow it begins can never carry a window over (a
//      paired counter aside: see below). A
//      string starts only if its first blow fits. When it does not, the intent is Wait (why Budget) on its target,
//      in reach and facing it; it guards as rule 3 says; the string, already drawn, starts on the first step it fits
//      (no new draw). Mid-string, a next blow that would not fit ends the string there (Budget; its pause follows,
//      or none on a staggered target). A string ended mid-blow (a guard, the leash, cannot act, its target gone)
//      leaves that blow running: no new string starts before it ends (Wait, why Committed). The budget is kept in
//      0.05 s slots, counted against it (the partner's part slots in, the lead's out): with the lengths the host
//      keeps, the only slack left is that slot, and it errs on the side of holding back.
//      Attack time outside its strings (a paired counter: the ruled 1.0 s, reported on the step it lands) comes in as
//      spentSeconds. On that step, before it decides, the brain puts it on the partner's side of the ring in the slots
//      it ran in: back from now, each slot filled up to the time it held, less the partner's own attack there. What
//      finds no room within the window goes into the current slot. So the next string waits exactly as it would
//      after a string of that length ending now. The brain cannot refuse a counter (the player triggers it), so with
//      counters the bound is: over any window, its attack time is at most budgetShare x the lead's attack time plus
//      budgetAllowanceSeconds, plus at most 1.0 s for each paired counter whose time falls in that window (with the
//      3 s cooldown, at most 7 land in 20 s). Every blow it begins fits every window as counted when it begins, the
//      counters so far included; a counter that lands after a blow was decided can carry that blow's window over by
//      the counter's own seconds, and the brain pays it back by waiting before its next string. The host must not also report those seconds as doing Attacking (they
//      would count twice). It adds no state: save, load and the hash are as before.
//   3. Defence. When a windup aimed at the partner is first seen (a new one: the enemy entered WindingUp, or its
//      windupLeft rose) with at least guardWarning s left, one seeded draw: guard with guardChance, else take the blow.
//      It guards from then until guardHold s after the windup ends. With less warning it is hit. It never parries,
//      ripostes or dodges. A guard begun mid-string ends that string (its pause runs from there; the blow it was
//      in runs out).
//   4. It never uses Release, gifts or the count (no intent exists for them).
//   5. Down and up. At health 0 it is Down and waits. After downSeconds Down the intent carries rise (level-triggered:
//      every step until the host reports health above 0) at riseHealth.
//   6. On a staggered target it strings without pauses until the stagger ends (punish on the intent); its blows still
//      add no break value and do not extend the stagger (breakValue 0, extendsStagger false). It does not switch to a
//      staggered enemy: only its own target counts.
//   7. Every Attack intent carries partnerBlow: the fight applies partnerBlowDamage and zero break value.
//      partnerBlowDamage is the single home of the ruled damage rule (the part of a partner blow above 60% of the
//      lead's light is halved): the fight calls it and nothing else. blowCapShare (0.6) and overScale (0.5) are not
//      tuning of their own; they are that rule's two numbers, kept in the data only so the rule has one home.
// An enemy view whose pos or radius is not a finite number (or whose radius is under 0) is absent: never a target,
// never drawn for, and a target that turns absent is dropped as if Down. Views past kMaxPartnerEnemies are ignored.
// StepBack (this module's choice of trigger): during a pause, closer than standoffMeters to its target's edge, it
// steps back to standoffMeters. It is never a dodge.
// Stationing (rules 8-12): where the partner stands, so its blows are seen from the side and it never crowds the lead
// or blocks the camera. Opt-in: only while the host sets PartnerInput::station.on with a whole finite view (camera,
// lead facing, arena); off, every rule above is as it was (intent.stationed false). All on the ground plane.
// When the rules collide the winners are, in order: the arena ring (a goal is never outside it), then personal space
// (8), then the camera wedge (9), then the arc and StayClose's cap (10). So a wedge edge that lies inside personal
// space gives a goal outside personal space even if it is in the wedge; a wall (the ring) behind a station pulls it in
// whatever the arc says; a target beside the lead, where no station is both in the arc and outside personal space,
// gives a goal outside personal space and off the arc. Only when the ring leaves no room outside personal space is a
// goal inside it: then as far from the lead as the ring allows, out of the wedge where that ties.
//   8. Personal space: no goal is within personalSpaceMeters (1.6) of the lead, centre to centre, except while the
//      host says it revives the lead, or when the arena leaves no other room (then as far from the lead as it can get).
//      The cross-field check: leashMeters at least personalSpaceMeters + 1, so StayClose always has room for a goal.
//   9. Camera wedge: no goal is inside the wedge from the camera to wedgeBeyondMeters (1.0) past the lead,
//      wedgeHalfDegrees (25) either side of the camera-to-lead line (the camera on the lead: its forward; neither:
//      no wedge). A goal inside it moves to the nearer wedge edge plus wedgeClearMeters (0.5); every goal stationing
//      emits keeps that clearance unless the arena or personal space leaves none outside it (the order above). A goal outside the arena (arenaCentre, arenaRadius; 0: unbounded) is pulled in.
//  10. Stations, on the circle round the target at its radius + half the reach (in reach), by the angle at the
//      target between the lead and the station: StayClose closeArcMin..Max (60-90) and within closeLeadMeters (3.5,
//      at most leashMeters - 0.5) of the lead; Spread spreadArcMin..Max (120-180), any distance. Of the two sides, the one
//      with a station outside the wedge (and personal space and the arena); both: the one nearer the partner. A side
//      once chosen is kept sideHoldSeconds (2.0) unless the wedge (or 8, or the arena) leaves it no station in its
//      arc. A new target (or none, the follow point) keeps the side and its timer while that side's station for the
//      new target is in its arc, and re-picks only when it is not. The named exception: an order change (StayClose
//      to Spread or back) re-picks at once. No station in the arc: the nearest legal point of the circle on that side; none on the circle: the arc's
//      middle, resolved by 8 and 9 (and if that is out of reach, it waits there and never starts a string).
//      StayClose's leash still rules: no station goal (on the circle, resolved or a follow point) is past that cap,
//      so a lead drifting a little never trips the leash and its return; stationing replaces
//      the old returnMeters clamp on the approach; the leash's own goal is a resolved point on the partner's side
//      of the lead, within max(returnMeters, personal space + 0.5).
//  11. No target: stand followMin..followMax (2.0-2.5) behind the lead (by its facing; zero facing: the camera's
//      line) and to one side, outside the wedge, the side kept as in 10.
//  12. A string starts only within stationTolerance (0.5) of its station (blows are seen from the side, not from
//      behind the lead); out of it the intent is MoveTo the station (Approach). The fallback, so a station it cannot
//      reach (one that runs from it, a spot it is blocked from) never starves it: with its target within reach and not
//      stationed for stationGiveUpSeconds (3.0), it may start a string from where it stands if that spot is outside
//      the camera wedge itself (no clearance) and outside personal space; after stationAnySeconds (6.0), from any spot
//      in reach outside personal space. Out of reach (or inside personal space) once the first has run, it goes to the nearest in-reach point of
//      the target's ring in the arena, outside personal space (and within StayClose's cap). The time counts while it
//      tries to station (Ready or a pause, with a target); being within the tolerance and in reach resets it, as do
//      going Down and rolesSwapped(). During a pause it goes back to the
//      station (StepBack when nearer than standoffMeters to the target's edge, else MoveTo). Later blows of a string
//      are not held to it; guarding, the budget and the rest are unchanged.
// The play camera (rules 13-16): only while the host also gives PartnerStationView::camera (whole and finite, see
// PartnerCameraView) with stationing on. Not given, every rule above is as it was, to the bit: the intents, the save's
// hash and every golden. Once given in a fight the brain is "framed" (sticky until begin or load).
//  13. Frustum stationing. A station (rule 10's ring round the target, half the reach off its edge, and as a second
//      tier the ring reachMeters - stationTolerance off it; no target, rule 11's ring round the lead at
//      followMin..followMax) is valid only if the partner's body there, a capsule of feet (groundZ), middle and head
//      (groundZ + bodyHeight), is inside the camera's frustum shrunk by frustumMargin (kPartnerFrustumMargin, 0.10) of
//      the whole screen's width and height on every side (each point in front of the camera, |x|, |y| <= 1 - 2 x margin in the screen's -1..1
//      frame), and outside both keep-outs: rule 9's wedge round the camera-to-lead line, and the target's own: from
//      the camera along the line to the target, half-angle the target's silhouette (asin(radius / distance)), out to
//      wedgeBeyondMeters past its far edge, with wedgeClearMeters' clearance (so it never stands in front of the
//      target, nor hidden behind it). Rule 8 (personal space), the arena and StayClose's cap still rule. Of the valid
//      stations the nearest to the partner, those in rule 10's arc (rule 11's 110..160 degrees) first; once chosen it
//      is held, by its angle on its ring, while it stays valid and the target stays. None valid: it keeps going to
//      the one it last held (whatever the target, while that spot stays legal and out of the target's keep-out);
//      after frustumGiveUpSeconds (3.0) with none (or at once when it holds none) it falls back to rules 10-11's
//      station until a valid one is found, and says so on every step it stays fallen back, strings, guards and
//      repositions included (intent.frameFallback, and fallbackSeconds()). The 3 s run on every step: the station held
//      is checked on string and guard steps too, so the fallback comes at 3 s whatever it is doing, and never early
//      for a station that stayed valid. Sooner only when the target's keep-out comes over the spot held.
//      Standing (its last step no MoveTo or StepBack) within twice stationTolerance of its goal, it counts as there
//      while its own place is in frame and outside both keep-outs, so a drifting station does not start and stop it.
//  14. Engage. With its target within reach and its next blow within the budget, it starts an action within
//      engageSeconds (2.0): a string, a guard (rule 3) or a reposition (16). The clock counts the steps it stands so
//      (in reach, the budget fitting, not acting) since its last action began. At engageSeconds a pause ends, and a
//      string starts from where it stands (ahead of rule 12's 3 s and 6 s and rule 13's wait) when that spot is
//      outside personal space and outside both keep-outs, and in the frame unless no valid station was found that step
//      (this step's search alone, with no wait for one; then reported: intent.offFrame, offFrameStarts()); otherwise
//      it keeps moving. The leash and the budget win:
//      it never starts a blow that does not fit (then the idle reason says NoBudget).
//  15. StayClose without the lead's target (no lock): the enemy the lead is attacking, else the nearest standing
//      enemy within closeLeadMeters + reachMeters of the lead; so it never stands idle by a fighting lead. Spread,
//      framed: it chooses among the enemies standing in the frame when any does, and a held target that has left the
//      frame gives way to one in it.
//  16. Variety. A pause in which it has not guarded or repositioned for repositionEverySeconds (8.0), with a string
//      struck since, it repositions: to the nearest valid station of rule 13 at least repositionMeters (1.5) from
//      where it stands, else at least half that; fallen back (rule 13), to rule 10's station on the other side when
//      that is legal (in its arc or not: fallen back, the frame offers nothing better) and at least half
//      repositionMeters away (why Reposition). With the strings (about every 3-4 s) that is two kinds of action in
//      every 20 s with the budget free. No new moves: strings, guards and walking are the kit it has.
// Idle reasons (with or without a camera): every step's intent carries why it is not acting (PartnerIdle), and the
// brain counts the seconds of each over the fight (idleSeconds; None is the time it acts, so they sum to the time
// stepped since begin). They are a report: in save and load, never in the hash, and they steer nothing.
// Two seeded streams (rhythm, defence), drawn at events only: the same seed and script give the same draws at any step.
// A step that is negative or not a finite number counts as 0, and one step is at most kMaxPartnerStep (the fight's
// kMaxFightStep), as in FightSession.
// save() and load() (PartnerBrainSave, plain data) carry its whole state for a checkpoint; hash() is its play state.
//
// Data/Battle/partner_brain.json (PartnerTuning; every field but version may be left out):
//   { "version": 1, "leashMeters": 4, "returnMeters": 3, "reachMeters": 1.8, "standoffMeters": 0.8, "blowsMin": 2,
//     "blowsMax": 3, "blowSeconds": 0.65, "pauseMin": 1.2, "pauseMax": 2.0, "guardWarning": 0.25, "guardChance": 0.7,
//     "guardHold": 0.3, "downSeconds": 30, "riseHealth": 0.25, "blowCapShare": 0.6, "overScale": 0.5,
//     "budgetWindowSeconds": 20, "budgetShare": 0.6, "budgetAllowanceSeconds": 2.0, "personalSpaceMeters": 1.6,
//     "wedgeHalfDegrees": 25, "wedgeBeyondMeters": 1.0, "wedgeClearMeters": 0.5, "closeArcMin": 60, "closeArcMax": 90,
//     "closeLeadMeters": 3.5, "spreadArcMin": 120, "spreadArcMax": 180, "sideHoldSeconds": 2.0, "followMin": 2.0,
//     "followMax": 2.5, "stationTolerance": 0.5, "stationGiveUpSeconds": 3.0, "stationAnySeconds": 6.0,
//     "targetHoldSeconds": 1.5, "frustumMargin": 0.1, "frustumGiveUpSeconds": 3.0, "engageSeconds": 2.0,
//     "repositionEverySeconds": 8.0, "repositionMeters": 1.5 }

#include "ftcore/Api.h"
#include "ftcore/ArenaFight.h"
#include "ftcore/PartnerCamera.h" // PartnerVec3, PartnerCameraView
#include "ftcore/Explore.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

inline constexpr std::size_t kMaxPartnerEnemies = kMaxArenaEnemies; // views past this are ignored
inline constexpr std::size_t kMaxPartnerBlows = 6;                  // a string's blows at most (blowsMax's bound)
// One step's time at most (seconds): a longer step counts as this. FightSession's kMaxFightStep, the same number.
inline constexpr double kMaxPartnerStep = 0.25;

enum class PartnerOrder : std::uint8_t { StayClose, Spread };
// What the host's ally body is doing this step (its own state, not the brain's intent).
enum class PartnerDoing : std::uint8_t { Idle, Moving, Attacking, Guarding, Hurt };

struct PartnerEnemyView {
    enum class Kind : std::uint8_t { Regular, Boss };
    enum class State : std::uint8_t { Idle, WindingUp, Staggered, Down };
    enum class Aim : std::uint8_t { None, Lead, Partner };
    Vec2 pos{};
    float radius = 0.5f; // reach is measured to its edge
    Kind kind = Kind::Regular;
    State state = State::Idle;
    float windupLeft = 0.f; // WindingUp: seconds until the blow lands
    Aim aim = Aim::None;    // WindingUp: whom the blow is aimed at
    bool leadAttacking = false;
};

// Stationing's view (rules 8-12), on the ground plane, fed by the host each step. Off (the default), or any field not
// a finite number, or arenaRadius under 0, or the partner's or the lead's position not finite: no stationing.
// The target's position is its enemy view's.
struct PartnerStationView {
    bool on = false;
    Vec2 cameraPos{};
    Vec2 cameraForward{};    // need not be unit; the wedge's axis when the camera stands on the lead
    Vec2 leadFacing{};       // need not be unit; zero: the camera-to-lead line (the lead faces away from the camera)
    bool reviving = false;   // the partner is reviving the lead: personal space is off
    Vec2 arenaCentre{};
    float arenaRadius = 0.f; // where the partner may stand (its body's margin taken off); 0: unbounded
    PartnerCameraView camera{}; // rules 13-16; not given by default
};

// One step's view. Enemies by the fight's own index (keep the order stable through the fight).
struct PartnerInput {
    PartnerOrder order = PartnerOrder::Spread;
    Vec2 pos{};
    float health = 1.f; // fraction of max
    bool canAct = true;
    PartnerDoing doing = PartnerDoing::Idle;
    Vec2 leadPos{};
    int leadTarget = -1; // an index into enemies, or -1
    // The lead is attacking this step (rule 2's budget counts its time). A host that never says gets the budget of
    // a lead who never attacks: budgetAllowanceSeconds per window.
    bool leadAttacking = false;
    // Each blow of a string the host would run, by its place in the string: its whole length (seconds). The host
    // keeps to it: a blow it starts always runs out. 0, or a value out of 0.1..2: the tuning's blowSeconds. Read
    // when a string starts.
    std::array<float, kMaxPartnerBlows> blowSeconds{};
    std::span<const PartnerEnemyView> enemies;
    PartnerStationView station{}; // rules 8-12; off by default
    // Rule 2: seconds of partner attack time spent outside its own strings since the last step (a paired counter's
    // 1.0 s on the step it lands). Not finite, or under 0: 0. Above budgetWindowSeconds: that. 0: as before.
    float spentSeconds = 0.f;
};

enum class PartnerAct : std::uint8_t { Wait, MoveTo, Attack, Guard, StepBack };
// Why this intent (for logs and tests).
enum class PartnerWhy : std::uint8_t {
    NotBegun, Down, CannotAct, Guard, Leash, String, Pause, NoTarget, Approach, HeldOff, Start, Budget,
    Committed, // a blow it began before its string ended still runs out: no new string yet
    Reposition // rule 16: a pause's walk to another station
};
// Why it is not acting this step. None: it acts (a string, a blow running out, a guard, a reposition). NoBudget: its
// next blow would break rule 2's budget. NoTarget: no target, or its station is reached and the target is out of
// reach. Moving: walking to a station, back on the leash, or stepping back. Staggered: hitstun (canAct false,
// doing Hurt) or down. Pause: the rhythm's pause, standing at its station. Other: held off a windup aimed at it it
// did not guard, canAct false for any other reason, not begun. (No "waiting on a called action": ArenaFight never
// holds the partner's body for one; a paired counter is dealt by the fight and only spends its budget.)
enum class PartnerIdle : std::uint8_t { None, NoBudget, NoTarget, Moving, Staggered, Pause, Other };
inline constexpr std::size_t kPartnerIdleCount = 7;

struct PartnerIntent {
    PartnerAct act = PartnerAct::Wait;
    PartnerWhy why = PartnerWhy::NotBegun;
    Vec2 point{};        // MoveTo, StepBack: where to go
    int enemy = -1;      // Attack: the target; MoveTo (Approach): the enemy it closes on
    int blows = 0;       // Attack: the string's length
    bool newString = false; // Attack: the string's first step (the host starts the light chain here)
    // Attack: start a blow now (the string's blowIndex-th light, of the length the host gave): the only step a blow
    // starts on. The host runs it out whatever comes after; on steps without it, the host starts none.
    bool blowStart = false;
    int blowIndex = -1;
    bool partnerBlow = false; // Attack: always true (rule 7)
    float breakValue = 0.f;   // Attack: always 0 (rules 6, 7)
    bool extendsStagger = false; // Attack: always false (rule 6)
    bool punish = false;      // Attack on a staggered target: no pause after it (rule 6)
    bool rise = false;        // Wait while Down past downSeconds: stand now
    float riseHealth = 0.f;   // with rise: the health to stand at (fraction of max)
    bool stationed = false;   // stationing (rules 8-12) ran this step: point, when set, obeys them
    bool framed = false;      // rule 13 ran this step (the camera given): point, when set, is a valid station ...
    bool frameFallback = false; // ... unless this: no valid station for frustumGiveUpSeconds, rules 10-11's instead
    bool offFrame = false;      // a report: this string starts from off the frame, no valid station found (rule 14)
    PartnerIdle idle = PartnerIdle::Other; // a report: never in the hash
};

struct PartnerTuning {
    float leashMeters = 4.f;    // StayClose: never further from the lead (1..10)
    float returnMeters = 3.f;   // StayClose: a pull back ends within this (0.5..leash)
    float reachMeters = 1.8f;   // strings start within this of the target's edge (0.5..4)
    float standoffMeters = 0.8f; // pause: step back when closer than this to the target's edge (0..reach)
    int blowsMin = 2;           // a string's blows (1..6)
    int blowsMax = 3;           // (blowsMin..6)
    float blowSeconds = 0.65f;  // one light blow when the host gives no length (0.1..2); the default fighter's first light
    float pauseMin = 1.2f;      // seconds after a string (0..10)
    float pauseMax = 2.0f;      // (pauseMin..10)
    float guardWarning = 0.25f; // least windup left to guard (0..2)
    float guardChance = 0.7f;   // (0..1)
    float guardHold = 0.3f;     // guard kept this long after the windup ends (0..2)
    float downSeconds = 30.f;   // Down this long, then rise (1..600)
    float riseHealth = 0.25f;   // (0.01..1)
    float blowCapShare = 0.6f;  // rule 7's share (0..1): see partnerBlowDamage
    float overScale = 0.5f;     // rule 7's scale over it (0..1)
    float budgetWindowSeconds = 20.f;  // rule 2's budget: any rolling window this long (5..30)
    float budgetShare = 0.6f;          // its attack time at most this x the lead's in the window (0..1)
    float budgetAllowanceSeconds = 2.f; // plus this (0..10)
    // Stationing (rules 8-12). Cross-field limits are applied where used: closeLeadMeters (and, StayClose, followMax)
    // at most leashMeters - 0.5, the follow distances at least personal space. Checked: stationTolerance at most half
    // reachMeters (a station is half the reach off the target's edge, so arriving always means in reach).
    float personalSpaceMeters = 1.6f; // rule 8 (0.5..4)
    float wedgeHalfDegrees = 25.f;    // rule 9 (5..60)
    float wedgeBeyondMeters = 1.f;    // rule 9: the wedge runs this far past the lead (0..5)
    float wedgeClearMeters = 0.5f;    // rule 9: a goal moved out keeps this from the edge (0..2)
    float closeArcMin = 60.f;         // rule 10, StayClose, degrees at the target from the lead (0..180)
    float closeArcMax = 90.f;         // (closeArcMin..180)
    float closeLeadMeters = 3.5f;     // rule 10, StayClose: a station within this of the lead (1..10)
    float spreadArcMin = 120.f;       // rule 10, Spread (0..180)
    float spreadArcMax = 180.f;       // (spreadArcMin..180)
    float sideHoldSeconds = 2.f;      // rule 10: a side is kept at least this long (0..10)
    float followMin = 2.f;            // rule 11, no target: behind the lead (0.5..10)
    float followMax = 2.5f;           // (followMin..10)
    float stationTolerance = 0.5f;    // rule 12: a string starts within this of its station (0.05..2, <= reach/2)
    float stationGiveUpSeconds = 3.f; // rule 12's fallback: not stationed this long, from where it stands (0.5..30)
    float stationAnySeconds = 6.f;    // rule 12's fallback: and this long, from anywhere outside personal space
                                      // (stationGiveUpSeconds..60)
    float targetHoldSeconds = 1.5f;   // rule 1, Spread: a chosen target is kept at least this long (0..10)
    // The play camera (rules 13-16).
    float frustumMargin = kPartnerFrustumMargin;          // rule 13: of the screen's width and height kept clear each side (0..0.4)
    float frustumGiveUpSeconds = 3.f;    // rule 13: no valid station this long, rules 10-11's (0..30)
    float engageSeconds = 2.f;           // rule 14 (0.25..10)
    float repositionEverySeconds = 8.f;  // rule 16 (2..20)
    float repositionMeters = 1.5f;       // rule 16: at least this far (0.5..6)
};

FTCORE_API bool validPartnerTuning(const PartnerTuning& t);
// Rule 13's frame test alone: a partner standing at p (its capsule, the camera's groundZ and bodyHeight) is inside the
// camera's frustum shrunk by `margin` (0..0.4) of the screen on every side. A camera not given or not whole: false.
FTCORE_API bool partnerInFrame(const PartnerCameraView& camera, float margin, Vec2 p);
// Reads Data/Battle/partner_brain.json. A field out of its range, of the wrong type or unknown, a bad version or a
// file that does not parse: `out` is the defaults and false (`error` says why). A cross-field pair broken (returnMeters
// over leashMeters, leashMeters under personalSpaceMeters + 1, standoffMeters over reachMeters, stationTolerance over
// half reachMeters, each min over its max: blows, pause, both arcs, follow, stationGiveUpSeconds over
// stationAnySeconds): both fields of that pair take their defaults, every other field is kept, it is reported in
// `notes`, and the import succeeds.
FTCORE_API bool importPartnerBrain(std::string_view json, PartnerTuning& out, std::string* error = nullptr,
                                   std::vector<std::string>* notes = nullptr);

// Rule 7, and its only home: a partner blow's hp damage from its raw damage and the lead's light damage (the part
// over 60% of the lead's light is halved, with the default tuning).
inline float partnerBlowDamage(float raw, float leadLight, const PartnerTuning& t = PartnerTuning{}) {
    const float cap = t.blowCapShare * leadLight;
    return raw <= cap ? raw : cap + (raw - cap) * t.overScale;
}

// The brain's whole state, plain data (a checkpoint keeps it; PartnerBrain::save and load).
struct PartnerBrainSave {
    enum class Phase : std::uint8_t { Ready, String, Pause };
    struct Watch {
        bool winding = false; // was WindingUp at Partner last step
        float left = 0.f;
        bool guard = false;   // this windup's draw said guard
        double hold = 0.0;    // guard kept after it ends
    };
    static constexpr double kSlot = 0.05;          // the budget's slot (s)
    static constexpr std::size_t kSlots = 604;     // 30 s of slots, and room either side

    PartnerTuning t{};
    std::uint32_t rhythm = 1;
    std::uint32_t defence = 1;
    std::uint32_t rhythmDraws = 0;
    std::uint32_t defenceDraws = 0;
    Phase phase = Phase::Ready;
    int target = -1;
    int blows = 0;
    double pause = 0.0;
    double clock = 0.0;      // in the current pause
    bool returning = false;
    bool drawn = false;      // blows and pause drawn for a string the budget holds
    std::array<double, kMaxPartnerBlows> lengths{}; // the string's blows, as the host gave them when it started
    int begunBlows = 0;      // of the string's blows, those begun
    double committedEnd = 0.0; // on its own clock (now): when the last blow begun runs out
    // Rule 2's budget: attack seconds per slot (the slot `slot % kSlots` is the current one), both sides.
    std::array<float, kSlots> spentPartner{};
    std::array<float, kSlots> spentLead{};
    std::int64_t slot = 0;
    double now = 0.0;
    bool down = false;
    double downFor = 0.0;
    std::array<Watch, kMaxPartnerEnemies> watch{};
    PartnerIntent out{};
    bool begun = false;
    // Stationing's side (rule 10, 11): -1 or +1 (0: none chosen), what it was chosen for (the target and the order,
    // or no target), and how long it has been held.
    std::int8_t side = 0;
    int sideKey = 0;
    double sideFor = 0.0;
    std::uint32_t swaps = 0; // rolesSwapped() calls
    int sideTarget = -1;     // the target the side was last used for (-1: none, the follow point)
    double offStation = 0.0; // rule 12's fallback: seconds trying to station since it last was stationed
    double targetFor = 0.0;  // rule 1: how long the current target has been held
    bool targetReached = false; // the current target has been within reach since it was chosen
    // Rules 13-16 (the camera given).
    bool framed = false;           // a camera was given since begin (sticky)
    std::uint8_t frameRing = 0;    // the station held: 0 none, 1 on its target's ring, 2 on the lead's follow ring
    int frameTarget = -1;          // ring 1: the target it is held for
    float frameAngle = 0.f;        // its direction from the ring's centre (degrees from +x)
    float frameRadius = 0.f;       // its radius from the ring's centre
    Vec2 frameLast{};              // where the held station last was valid (the grace's goal)
    double noFrame = 0.0;          // seconds with no valid station
    bool fellBack = false;         // rule 13's fallback holds: on rules 10-11's station until a valid one is found
    double engageFor = 0.0;        // rule 14's clock
    double lastStrike = -1e9;      // rule 16: when the last string, guard and reposition began (on now's clock)
    double lastGuard = -1e9;
    double lastMove = -1e9;
    bool guarding = false;         // the last step's intent was Guard
    // The report (never hashed, steers nothing): seconds per PartnerIdle, and of rule 13's fallback.
    std::array<double, kPartnerIdleCount> idleSeconds{};
    double fallbackSeconds = 0.0;
    std::uint32_t offFrameStarts = 0; // strings begun from off the frame (rule 14's last resort)

    // FNV-1a over every field that steers play (StateHash.h): two brains with the same hash do the same from here.
    // The report is left out; rules 13-16's fields count only once framed or with one of their tunables off its
    // default, so a brain never given a camera hashes as it always did.
    FTCORE_API std::uint64_t hash() const;
};

class FTCORE_API PartnerBrain {
public:
    // False (and the defaults) for bad tuning. seed 0 is taken as 1.
    bool begin(const PartnerTuning& t, std::uint32_t seed);
    const PartnerIntent& step(double dt, const PartnerInput& in);
    // The body it drives changed (the host calls it on a party switch, tick.switched, and on a takeover). Everything
    // about the old body goes: the string (its blows, the one still running, a string drawn and held), the pause,
    // the target, the return, the chosen station side, the windups it watched and being down. The budget ring stays
    // (the budget is about the pair, not the body), and so do the clock and the seeds. The next step decides afresh
    // from the new body's position: with a target in reach it starts a string (or moves to its station) at once.
    void rolesSwapped();
    std::uint32_t swaps() const { return s_.swaps; }

    const PartnerIntent& intent() const { return s_.out; }
    bool down() const { return s_.down; }
    double downFor() const { return s_.downFor; }
    bool stringing() const { return s_.phase == Phase::String; }
    bool pausing() const { return s_.phase == Phase::Pause; }
    // Rule 2's budget: whether `seconds` more of its attack, from now, would keep every window within it.
    bool budgetFits(double seconds) const;
    // A string drawn and held for the budget, not yet started (its draws are made).
    bool drawnAhead() const { return s_.drawn; }
    int target() const { return s_.target; }
    // Draw counts (tests: draws happen at events only).
    std::uint32_t rhythmDraws() const { return s_.rhythmDraws; }
    std::uint32_t defenceDraws() const { return s_.defenceDraws; }
    int lastBlows() const { return s_.blows; }
    double lastPause() const { return s_.pause; }
    // Seconds left of the blows it has begun (0: none running).
    double committedLeft() const { return s_.committedEnd > s_.now ? s_.committedEnd - s_.now : 0.0; }
    // The idle report: seconds stepped with each reason since begin (None: acting); they sum to the time stepped.
    double idleSeconds(PartnerIdle why) const {
        const std::size_t i = static_cast<std::size_t>(why);
        return i < kPartnerIdleCount ? s_.idleSeconds[i] : 0.0;
    }
    double fallbackSeconds() const { return s_.fallbackSeconds; } // rule 13's fallback, seconds
    std::uint32_t offFrameStarts() const { return s_.offFrameStarts; } // rule 14's off-frame starts
    bool framed() const { return s_.framed; }

    // Its whole state, for a checkpoint: load(save()) gives a brain that does exactly the same from there.
    const PartnerBrainSave& save() const { return s_; }
    void load(const PartnerBrainSave& state) { s_ = state; }
    std::uint64_t hash() const { return s_.hash(); }

private:
    using Phase = PartnerBrainSave::Phase;
    using Watch = PartnerBrainSave::Watch;
    static constexpr double kSlot = PartnerBrainSave::kSlot;
    static constexpr std::size_t kSlots = PartnerBrainSave::kSlots;
    float draw(std::uint32_t& s, std::uint32_t& n);
    const PartnerIntent& decide(double dt, const PartnerInput& in);
    void account(double dt, double partner, bool lead);
    void spend(float seconds);
    void endString(bool staggered);
    // seen: bit i set when enemy i stands in the frame (framed only; 0 otherwise).
    int chooseTarget(const PartnerInput& in, bool framed, std::uint32_t seen) const;

    PartnerBrainSave s_{};
};

} // namespace ftcore
