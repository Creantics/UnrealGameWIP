#pragma once

// Real-time arena melee (CombatKind::Arena; docs/COMBAT_SYSTEMS.md). The lead fights one or more
// enemies inside a ring: free movement, a chain of light attacks and a heavy one, a dodge roll with
// invulnerable frames, a guard whose first moments parry, stamina, poise and healing charges, and a
// lock-on. Enemies telegraph every attack (its startup), take turns to attack (attack tokens), and
// break into a stagger when their poise runs out.
//
// Core judges, the host moves. Each frame the host reports where the bodies stand (it moves them
// itself, with its own collision) and what the player pressed; tick() advances the rules by dt and
// returns what happened (events) and what each body wants next: a velocity and a facing. Core owns
// every facing and every timer (the host sets each pawn's rotation to Core's facing, so the hit
// arcs match what the player sees); it never knows walls or the navmesh, only the ring, a circle
// the host supplies that nothing leaves.
//
//   ftcore::ArenaFight fight;
//   fight.begin(def, ring, playerPos, playerFacing, seed);
//   place enemy i at fight.enemyStart(i) facing fight.enemyStartFacing(i); followers at arenaParkSpot()
//   every frame:
//     bodies.player = <the player pawn's position>; bodies.enemies[i] = <enemy i's position>;
//     const ftcore::ArenaOutput& out = fight.tick(dt, input, bodies);
//     move each pawn at its out velocity, set its rotation to its out facing, play its out action;
//     for (const ftcore::ArenaEvent& e : out.events) ...;   // sparks, sounds, the HUD
//     fight.phase() == Victory: the step is done. Defeat: retry (restart(), then put every pawn back
//     at its start) or quit.
//   Tests, the balance check and headless runs move the bodies with arenaStep() instead of an engine.
//
// Rules (times in seconds of fight time, the sum of the dts; distances in metres on the ground plane):
//
// * Attacks. Every attack has three parts back to back: startup (for an enemy, its telegraph),
//   active (the only part that can connect) and recovery. During startup the attacker may still turn
//   toward its target at `tracking` rad/s and moves forward `lunge` metres spread over startup and
//   active. A hit test runs on every tick whose interval [t, t + dt) overlaps the active part (so a
//   long frame can't skip a short active part), on the bodies as reported for that tick: the target
//   is hit when its centre is within range + its body radius of the attacker's centre and inside the
//   arc (dot(attacker facing, direction to target) >= arcCos). An attack connects at most once per
//   target. The player's attacks test every living enemy; an enemy's attacks test only the player.
//   With maxTargets > 0 an attack connects with at most that many distinct targets over its whole
//   active part: when a tick has more targets that qualify than slots left, the nearest win (centre
//   distance, ties by lower index; a riposte's own enemy first, as it is aimed at it). So a long,
//   narrow shot doesn't pierce the whole line.
// * Player actions. Idle/Move, Light, Heavy, Riposte, Release, Dodge, Guard, Heal; and the ones that happen to
//   the player: Hitstun, GuardBreak, Downed.
//   - A new action may start from Idle/Move, from an attack once `cancel` of its recovery has passed,
//     from a dodge once `dodgeCancel` of it has passed, and from Guard at any time. An attack may name its own
//     cancel windows instead, each a share of the whole attack (startup + active + recovery) after which that
//     action may cut in: cancelGuard (Guard, and a guard press there opens the parry window as anywhere),
//     cancelRoll (Dodge) and cancelLight (the next light, the riposte and the aimed shot); below 0, the
//     default, the `cancel` rule. Heavy, heal and Release always keep the `cancel` rule. An action pressed
//     while it can't start is buffered for `bufferTime` (only the latest press is kept) and starts as
//     soon as it can. Pressed in one tick, Release beats dodge, which beats jump, which beats heal, heavy and light.
//   - Light: the chain's next attack when it starts during the previous light's recovery or within
//     `chainGap` after it, else the first. The chain wraps after lightCount.
//   - Every attack and the dodge cost their `stamina` (dodgeStamina); one that costs some needs stamina > 0
//     to start and may take it to 0 (StaminaEmpty). One that costs 0 (a free light) starts whatever the
//     stamina and never holds its refill back. Stamina refills at staminaRegen per second once staminaDelay has passed
//     since the last spend.
//   - Dodge: a roll of dodgeDistance over dodgeTime along the stick (straight back, away from the
//     facing, when the stick is idle). Invulnerable from dodgeInvulnStart to dodgeInvulnEnd: an enemy
//     attack whose hit test passes then does nothing and reports Evaded (once per attack).
//     ArenaPlayerOut::dodgeDir (unit) and backstep are set once when a dodge starts, from the stick
//     or the backstep direction before any ring cut, and stay until the next dodge: the host picks
//     the clip once from them, since a direction read from the velocity turns when a roll slides
//     along the ring's edge.
//   - Jump (ArenaInput::jump): kArenaJumpStaminaShare of dodgeStamina, refused with none left (as a dodge);
//     no cooldown, the landing is its weight. jumpStartup on the
//     ground (the take-off), jumpAir in the air, jumpLand on the ground again (the landing). The air clock
//     (ArenaPlayerOut::jumpTime) runs beside the action, in the player's own time (a hit pause holds it), and
//     the height is a closed-form arc of it (jumpHeight at the top), drawn only. In the air the velocity keeps
//     the take-off's and steers toward the stick at jumpAirControl m/s^2; the landing brakes at moveBrake.
//     A dodge, guard or buffered press cuts in only once jumpLandLock of the landing has passed; in the air
//     only one light may start, the air light (kArenaAirMove, ArenaFighter::jumpLight), and the landing cuts
//     it short. From jumpClearStart to jumpClearEnd of the air clock an enemy move with hitsAir false passes
//     under the player and reports Evaded (once per attack); one with hitsAir true lands as on the ground,
//     and a blow that lands in the air ends the jump (Hitstun), whatever jumpHeight. No take half from the
//     air. It cuts into an attack through cancelRoll (never a Release), into a dodge only once its i-frames
//     are over, and never out of Hitstun, GuardBreak or Downed. Nothing cuts the air light.
//   - Guard (held): blocks attacks from within guardArcCos of the facing. A blocked hit deals
//     damage * (1 - guardAbsorb) and costs the guard guardDamage stamina; if that empties stamina
//     the guard breaks (GuardBroken, GuardBreak for guardBreakTime). Unblockable attacks ignore it.
//     Moving while guarding is scaled by guardMoveScale.
//   - Parry: pressing guard (the held flag going from false to true) opens a parry window of
//     parryWindow, unless the previous press was less than parryRearm ago (mashing guard opens no
//     window). A parryable enemy attack that connects inside the window, from inside the guard arc,
//     is parried: no damage, no stamina cost, and the enemy is Open for openTime.
//   - Riposte: a light press with an Open enemy within riposteReach and in front (dot >= 0.5)
//     performs the riposte attack instead of a light, aimed at that enemy and invulnerable throughout.
//     When it connects, the enemy goes Down for downTime.
//   - Load (Load.h): a second gauge beside stamina, 0 to kMaxLoad, never a replacement for it. A parry
//     (the perfect guard: the parry window, never wider) stores Load: the parried attack's damage times
//     loadShare (LoadStored; a block stores nothing). The host may give Load too (giveLoad); nothing
//     else stores it. It never refills or drains on its own, and no attack or roll spends it. At kMaxLoad the player is
//     overloaded (Overloaded, once as it fills): light, heavy and heal are refused until a Release
//     (dodge, guard, parry, movement and the lock stay open, as Turns refuses commands but never the
//     defence).
//   - Release: a strike shaped like the riposte (the `release` attack: invulnerable throughout, facing
//     the lock target or the stick at its start), open only while Load is above kArenaReleaseOpenLoad
//     (a third), from any state another action may start from, whatever the stamina. It spends all the
//     Load at once (Released, amount = the Load spent): each enemy it connects with loses the attack's
//     poiseDamage plus the Load spent times releasePoise (the usual armour share and upright rule).
//   - Paired counter: the partner's input (`pair`, a second lead's press the host feeds) within
//     pairWindow of a parry, before or after it, turns that parry into a counter-stagger: the parried
//     enemy, if still open, takes pairHits times light[0]'s damage and is staggered (CounterStagger,
//     then Staggered). A press is used once; a parry pairs with one press.
//   - Read (ReadCards.h, the partner's second input, as in Turns): as an enemy move starts (AttackStarted,
//     each move of a string too) the partner is dealt up to kReadCards cards (CardsDealt): that move and
//     decoys from the living enemies' other moves, each naming a move and its `tell`, no two tells alike.
//     Nothing pauses. A pick (`read`, the card's index) counts only while that move is still in its
//     startup; the cards go when its active part begins, or it ends, is cut short or the attacker dies,
//     and only one deal is open at a time (a move starting while another is open deals nothing).
//     Correct (Read): one Read stack more (up to kMaxReadStacks), and when that move's active part ends
//     or a parry or stagger cuts it short, the partner strikes the attacker once (ReadCounter): readPower
//     times light[0]'s damage, times 1 + readStackPower per stack past the first, plus readPoise poise
//     damage (an upright enemy only). Wrong (Misread): the stacks go back to 0 and the attacker regains
//     readWrongPoise poise, up to its kind's. A pick with nothing open, or a second one, does nothing.
//   - Heal: needs a charge; heals healAmount after healTime, then healRecovery; movement scaled by
//     healMoveScale. Hitstun before the heal applies cancels it and keeps the charge.
//   - Being hit: hp loses the damage; poise loses the poise damage, and at 0 the player goes into
//     Hitstun (cancelling the current action) and poise refills. Poise also refills after poiseDelay
//     with no poise damage. hp 0: Downed, then Defeat.
//   - Facing: locked, the player faces the lock target (and strafes); otherwise it turns toward the
//     stick at turnRate. The drawn facing (drawFacing) follows at turnRate outside attacks and dodges,
//     so the body is drawn turning where the rules snap; a dodge and an attack's startup turn it faster
//     (an attack's reaches the rules' facing by its active part, so a blow lands from where the body looks). An attack starts facing, in this order: the aim, the lock target, the soft
//     target (softRange, softArcCos: ArenaSoftTarget.h), the stick when it is pushed; then turns only
//     at its `tracking` toward the same. A riposte turns onto its enemy over its startup instead, at
//     its tracking or faster, so it faces the enemy as the blow lands (at once with no startup). On a soft target the attack also closes in by
//     up to homingReach on top of its lunge, never past touching.
// * Enemies. Each enemy's brain is Approach (run when farther than runRange, else walk, toward its
//   preferred range), Strafe (circle the player at preferredRange, changing direction now and then
//   from the seeded RNG), Attack (one move's startup, active and recovery; then its `chain` move at
//   once, if any), Staggered (poise broken: staggerTime, attack interrupted), Open (parried:
//   openTime), Down (after a riposte: downTime) and Dead. Only `attackers` enemies hold an attack
//   token at once: a strafing enemy whose attackDelay has passed asks for one, the longest waiting
//   (then the nearest) gets it, and it keeps it through its whole string, giving it back when the
//   string ends or it is staggered, parried or killed. Holding a token, it picks a move whose
//   [minRange, maxRange] holds the player's distance and whose cooldown is over (weighted by
//   `weight`, from the seeded RNG), or closes in to the nearest move's maxRange when none fits.
//   Enemy poise works like the player's: poise damage beyond poise staggers, `armor` ignores a share
//   of it during an attack's startup and active parts, and poise refills after poiseDelay.
// * Hit pause. A blow that lands (a Hit on either side, not one the host vetoes) with hitStop > 0 pauses the
//   body it lands on for hitStop seconds and its attacker for hitStop * hitStopSelf (0 for a shot: the
//   shooter goes on); a parry pauses the player and the parried enemy for the fighter's parryStop. A paused
//   body's own time stands still: its action or brain time, its move, its stance and token asking (an
//   enemy), its velocity (0) and, for the player, the age of a buffered press and the parry window, so a
//   press made in a pause still starts, and a guard still parries, when it ends. Pauses that overlap
//   end at the latest, never added up. A tick that a pause ends inside advances that body by the part of
//   the tick after it, so the result doesn't hang on the step. Everything else runs on fight time: the
//   fight's clock (time(), stats.time, every event's time), stamina and poise refill, cooldowns, attack
//   tokens and attackDelay, the pair window and the Read deal. Beside it runs the held
//   clock (heldTime()): the fight clock less the player's hit pauses, so it stands still while the player
//   is paused and, in a tick a pause ends inside, advances by the part after it. The shared combat inputs
//   the host ticks beside the fight are ticked every frame with the held clock, never with time(); the host
//   reads hitStopLeft() for the player's pause, and ArenaPlayerOut::stopLeft / ArenaEnemyOut::stopLeft to
//   stop each figure. Both read 0 once the fight is over and on a dead enemy, so the blow that ends a fight
//   reports no pause (the host may play one from its Hit event).
// * Flinch. An enemy kind with flinchTime > 0 flinches (Flinch, the Flinched event) when a player's blow
//   of at least flinchPoise poise damage lands and its poise holds: in Approach, Strafe, Flinch (it
//   starts again) or an attack's startup or recovery, never in its active part or while its `armor` is on.
//   A flinch cuts the move short and gives the token back, as a stagger does; it is pushed flinchPush
//   metres straight away from the player over the flinch, then walks again. A broken poise staggers
//   instead. The host may forbid it for one enemy (setEnemyFlinch: a boss whose script allows it only in
//   some phases; a restart allows it again, so the host sets it after restart()); bosses leave flinchTime at 0.
//   No stunlock: after kArenaMaxFlinchChain flinches in a run, blows land on it without a flinch until
//   kArenaFlinchChainGap passes with no blow of flinchPoise or more (every kind's poise still breaks).
// * Break-out. An enemy kind with breakAt > 0 counts the player's blows that land on it while it is
//   upright (Approach, Strafe, Attack, Flinch), the count going back to 0 after breakDecay with none, when
//   it goes down (Staggered, Open, Down) and on a break-out. The blow that reaches breakAt, if it
//   doesn't stagger it, starts its breakMove at once (BrokeOut, then AttackStarted): whatever the token count, its cooldown and the
//   flinch, though never a move the host's move mask clears. That move should carry `armor`. A stagger,
//   Open or Down never counts and never breaks out. The host's flinch veto (setEnemyFlinch false) bars the
//   count and the break-out too.
// * Lock-on: `lock` locks the living enemy within lockRange with the best score of how close it is to
//   the view direction and how near it is (the worker picks the weighting; nearest wins ties), or
//   lets go when already locked. lockSwitch moves the lock to the next living enemy in range to the
//   left (-1) or right (+1) of the current one, as seen from the player. The lock moves to the nearest
//   other living enemy in range when its target dies, and lets go beyond lockBreak.
// * Acceleration: each body keeps its velocity from tick to tick. Only the player in Idle, Move,
//   Guard and Heal and an enemy in Approach and Strafe ramp: their velocity moves toward the one
//   they want by at most rate * dt (as a vector), from wherever it is now. The player's rate is
//   moveAccel while the wanted speed is above the current one and moveBrake otherwise; an enemy's
//   is its kind's `accel` both ways; a rate <= 0 snaps (0 is instant). Lunges, dodges and every
//   velocity an attack or a reaction sets (hitstun, stagger, ...) are written directly and never
//   ramped; the ramp resumes from that velocity when the body is back in a ramped state.
//   arenaStep integrates the output velocity, so it follows the ramp.
// * The ring: every velocity Core outputs is cut so the body stays inside the ring (minus its body
//   radius); a dodge toward the edge slides along it.
// * Victory: every enemy Dead. Defeat: the player Downed. Then tick() does nothing but report the
//   phase. restart() puts everything back as begin() left it (the same seed, so the same fight).
// * Host hooks (generic: a boss's own rules live in its own Core class and the host, never here). The host
//   may hold an enemy Open or Down from outside (holdEnemy, releaseEnemy), keep an enemy's hp above a floor
//   (setEnemyHpFloor), rule on every blow before it applies (setBlowHook): scale its damage and poise
//   damage, or veto it, and give the player Load (giveLoad). Holds and floors are fight state (begin() and
//   restart() clear them); the blow hook is the host's and stays until it sets another.
//
// Data: Data/Battle/arena.json (ids and numbers only, no story text). Like encounters.json and
// qte.json it is keyed by scene id + gameplay beat index + plan step index, with a `default` fight:
//   { "version": 1, "bpm": 150,
//     "fighters": [ { "id": "default", "hp": 100, ...,
//                     "light": [ { "startup": 0.18, "active": 0.12, "recovery": 0.35, "range": 2.1,
//                                  "damage": 12, "stamina": 14 }, ... ],
//                     "heavy": { ... }, "riposte": { ... }, "release": { ... } } ],
//     "enemies": [ { "id": "brute", "hp": 90, "poise": 40, ...,
//                    "moves": [ { "startup": 0.8, "active": 0.2, "recovery": 0.8, "range": 2.4,
//                                 "damage": 18, "minRange": 0, "maxRange": 2.8, "chain": 1 }, ... ] } ],
//     "default": { "enemies": ["brute"], "ring": 12, "attackers": 1, "spawn": 7 },
//     "fights": [ { "scene": "SC-X", "beat": 4, "step": 0, "enemies": ["brute", "brute"], "attackers": 2 } ] }
//   A top-level "bpm" (ArenaTable::bpm, copied to every fight's ArenaDef::bpm) is the arena's beat:
//   every enemy move's startup must then be a whole number of its beats (1 or more, within
//   kArenaBeatTolerance), so a count-in can land on the impact. Without it (0) no startup is checked. An
//   enemy kind with a "bpm" of its own (ArenaEnemyKind::bpm) is checked against that one instead. An enemy
//   kind may also carry a "look" (ArenaEnemyLook: a skinned figure's clip table, hover, scale, yawOffset) and its
//   moves a "clip" (the clip id the host plays); both are the host's alone (docs/ARENA_DEMO.md).
//   Fighter, enemy, move and attack keys are the struct fields below, spelled the same; a missing
//   key keeps the struct's default, so entries list only what differs. A fighter's id is a lead id
//   (lower case: "devin", "duy", "third") or "default" (required); ArenaTable::fighter falls back to it.
//   A lead's kit starts from the "default" fighter, not the struct: a key it leaves out keeps the
//   default's value, and its light[i] starts from the default's light[i] (the struct's past the
//   default's chain), so a kit lists only what makes it that lead's. A move's attack keys sit in the
//   move object itself. `ring`, `attackers` and `spawn` are ArenaDef's ringRadius, attackers and
//   spawnDistance. 1..kMaxArenaEnemies enemies per fight, 1..kMaxArenaMoves moves per enemy,
//   1..kMaxLightChain lights; `chain` must name another move of the same enemy and no string may
//   loop. An unknown enemy id, a duplicate id or a duplicate (scene, beat, step) is an error; unknown
//   keys are warnings; numbers outside their ranges (the loader documents them) are errors.
//   `moveAccel` and `moveBrake` (a fighter's), `accel` (an enemy's) and `maxTargets` (an attack's)
//   are keys like the rest. The loader fills ArenaDef::enemyKind of every fight from its line-up:
//   slot i holds the index of enemies[i]'s kind in ArenaTable::enemyIds.
//
// Deterministic for the same seed, inputs and dts. Loading allocates; nothing else does (fixed
// capacity, results by value or by reference to the fight's own storage).

#include "ftcore/Api.h"
#include "ftcore/Chairs.h"
#include "ftcore/Explore.h" // Vec2
#include "ftcore/FightAnimator.h" // FightClip
#include "ftcore/Load.h"
#include "ftcore/ReadCards.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

class JsonValue; // ftcore/Json.h

inline constexpr std::size_t kMaxArenaEnemies = 6;
// Per enemy (a boss move of n impacts is n chained moves). Fixed storage: at 24 an ArenaDef is about 17.4 KiB and an
// ArenaFight about 23 KiB (MSVC x64; 7 and 10 KiB at 8); test_arena.cpp holds them under 20 and 28 KiB.
inline constexpr std::size_t kMaxArenaMoves = 24;
inline constexpr std::size_t kMaxLightChain = 4;
inline constexpr std::size_t kMaxArenaEvents = 32; // per tick; more are dropped (ArenaFight::droppedEvents)

// Actor ids in events: enemies are 0..kMaxArenaEnemies-1.
inline constexpr std::uint8_t kArenaPlayer = 0xFE;
inline constexpr std::uint8_t kArenaNobody = 0xFF;
// The ally body (ArenaFight::setAlly) as an event's or a blow's target.
inline constexpr std::uint8_t kArenaAlly = 0xFD;
// The player's move ids in events and ArenaPlayerOut::move: a light is its chain index.
inline constexpr std::uint8_t kArenaHeavyMove = 8;
inline constexpr std::uint8_t kArenaRiposteMove = 9;
inline constexpr std::uint8_t kArenaReleaseMove = 10;
// A gun kit's aimed light: one round (ArenaAction::Light with this move, never part of the chain).
inline constexpr std::uint8_t kArenaShotMove = 11;
// The light struck in the air (ArenaAction::Light with this move during a jump, never part of the chain).
inline constexpr std::uint8_t kArenaAirMove = 12;
// A jump's stamina: this share of the kit's dodgeStamina.
inline constexpr float kArenaJumpStaminaShare = 0.6f;
// A Release is open only with more Load than this: a third of kMaxLoad.
inline constexpr int kArenaReleaseOpenLoad = kMaxLoad / 3;
// An enemy move's startup is on the beat when it is within this many seconds of a whole number of beats.
inline constexpr float kArenaBeatTolerance = 0.002f;
// The longest hit pause or parry pause the table takes (hitStop, parryStop), in seconds.
inline constexpr float kArenaMaxStop = 0.3f;
// No flinch stunlock: at most this many flinches in a run; the run ends after kArenaFlinchChainGap s with no blow.
inline constexpr std::uint8_t kArenaMaxFlinchChain = 3;
inline constexpr float kArenaFlinchChainGap = 1.f;

// How many whole beats of `bpm` `seconds` lasts (1 or more, within kArenaBeatTolerance), else -1 (also
// for bpm <= 0). A host counts an attack in from it: the impact is that many beats after AttackStarted.
FTCORE_API int arenaWholeBeats(float seconds, float bpm);
// Longest clip id of a move and longest clip table path of a kind's look, in characters.
inline constexpr std::size_t kMaxArenaClipId = 31;
inline constexpr std::size_t kMaxArenaLookPath = 95;
// ArenaDef::enemyKind of a slot that is not a table kind (a def built in code).
inline constexpr std::uint8_t kArenaNoKind = 0xFF;

// One attack, the player's or an enemy's.
struct ArenaAttack {
    float startup = 0.35f;    // before the active part (an enemy's telegraph)
    float active = 0.15f;     // the only part that can connect
    float recovery = 0.45f;
    float range = 2.0f;       // metres from the attacker's centre; the target's body radius is added
    float arcCos = 0.5f;      // cos of half the arc it sweeps: 0.5 = 120 degrees wide, -1 = all round
    float width = 0.f;        // > 0: a straight lane this many metres wide instead of the arc (a shot): it reaches
                              // a body whose centre lies ahead within range + radius and off the line by at most
                              // width / 2 + radius; arcCos is then unused
    int damage = 10;
    float poiseDamage = 10.f;
    float guardDamage = 15.f; // stamina a guard that blocks it loses
    float lunge = 0.f;        // metres the attacker moves forward over startup + active
    float tracking = 3.f;     // rad/s the attacker may still turn during startup
    float armor = 0.f;        // 0..1: share of incoming poise damage ignored during startup + active
    float stamina = 0.f;      // the player's cost; enemies have no stamina
    float cancel = 0.6f;      // the player's: share of recovery after which another action may start
    float cancelGuard = -1.f; // the player's: share of the whole attack after which Guard may cut in; < 0: `cancel`
    float cancelRoll = -1.f;  // ... a dodge; < 0: `cancel`
    float cancelLight = -1.f; // ... the next light (a riposte, a shot); < 0: `cancel`
    float hitStop = 0.f;      // s the body it lands on pauses (see Hit pause); 0: none
    float hitStopSelf = 1.f;  // share of hitStop the attacker pauses too (0: a shot, the shooter goes on)
    float jolt = 0.f;         // degrees the host's camera kicks when it lands (ArenaImpact.h); 0: none
    std::uint8_t maxTargets = 0; // 0: connects with every target in range and arc; N: with at most N distinct
                                 // targets over its whole active part, the nearest first (the player's matter)

    float total() const { return startup + active + recovery; }
};

// What a lead fights with (arena.json "weapon": "none", "gauntlets", "guns" or "magic"). The rules never read it:
// the numbers of the kit are the weapon. The host picks its attack and hit clips and its props by it, and
// arenaStrikePattern turns it into a FightAnimator strike pattern.
enum class ArenaWeapon : std::uint8_t { None, Gauntlets, Guns, Magic };
FTCORE_API const char* arenaWeaponName(ArenaWeapon w); // "none", "gauntlets", "guns", "magic". Literals.

// The player's kit. arena.json has one per lead id and a "default"; each lead's own blocks are its guard and
// parry numbers (guardArcCos, guardAbsorb, guardMoveScale, guardBreakTime, parryWindow, parryRearm).
struct ArenaFighter {
    ArenaWeapon weapon = ArenaWeapon::None; // a lead's kit inherits the default's like every other key
    // Whether the lead has attacks of its own (arena.json "attacks", default true). False: light, heavy, the riposte a
    // light would start and the Release are refused (the press is dropped); guard, parry, dodge, heal, the partner's
    // pair and Read still work. Such a kit stores no Load (a parry stores none, giveLoad gives 0), so it never
    // overloads. A lead whose part in a fight is helping the others has none.
    bool attacks = true;
    int hp = 100;
    float stamina = 100.f;
    float staminaRegen = 45.f;  // per second, once staminaDelay has passed since the last spend
    float staminaDelay = 0.6f;
    float poise = 25.f;
    float poiseDelay = 1.5f;    // seconds without poise damage before poise refills
    float hitstun = 0.5f;
    float moveSpeed = 4.5f;     // m/s at full stick
    float turnRate = 14.f;      // rad/s outside attacks
    float bodyRadius = 0.4f;
    float bufferTime = 0.2f;

    std::array<ArenaAttack, kMaxLightChain> light{};

    std::uint8_t lightCount = 3;
    float chainGap = 0.3f;
    ArenaAttack heavy;
    ArenaAttack riposte;
    // Guns: the one round a light fires while aiming (ArenaInput::aim); unaimed, a light is the kit's own chain
    // (kicks). arena.json "shot", required on a guns kit and refused on any other.
    ArenaAttack shot;

    // The jump (see Jump above). jumpAir 0: the kit can't jump (the press is dropped).
    float jumpStartup = 0.08f;    // s of take-off on the ground
    float jumpAir = 0.6f;         // s in the air
    float jumpLand = 0.3f;        // s of landing on the ground
    float jumpLandLock = 0.15f;   // s of the landing nothing cuts into (the weight)
    float jumpHeight = 0.55f;     // m at the top (drawn only)
    float jumpClearStart = 0.2f;  // s of the air clock (from take-off) after which low moves pass under ...
    float jumpClearEnd = 0.5f;    // ... and before which (at most jumpStartup + jumpAir)
    float jumpAirControl = 4.f;   // m/s^2 the stick steers the velocity in the air
    ArenaAttack jumpLight;        // the air light (kArenaAirMove)

    float dodgeTime = 0.6f;
    float dodgeDistance = 3.5f;
    float dodgeInvulnStart = 0.05f;
    float dodgeInvulnEnd = 0.4f;
    float dodgeStamina = 20.f;
    float dodgeCancel = 0.75f;

    float guardArcCos = 0.f;    // 0: the front half
    float guardAbsorb = 1.f;
    float guardMoveScale = 0.5f;
    float guardBreakTime = 1.2f;
    float parryWindow = 0.15f;
    float parryRearm = 0.5f;
    float parryStop = 0.f;      // s a parry pauses the player and the parried enemy (see Hit pause)
    float hurtJolt = 0.f;       // degrees the camera kicks when an enemy move with no jolt of its own hits the player
    float riposteReach = 2.2f;

    std::uint8_t healCharges = 3;
    int healAmount = 45;
    float healTime = 0.9f;
    float healRecovery = 0.4f;
    float healMoveScale = 0.35f;

    float lockRange = 15.f;
    float lockBreak = 20.f;
    // Soft target and homing for an attack started with no lock (ArenaSoftTarget.h; the host asks for them).
    float softRange = 0.f;      // metres to an enemy's near edge; 0: no soft target
    float softArcCos = 0.5f;    // the arc either side of the stick or the facing (dot)
    float homingReach = 0.f;    // most metres an attack closes in on top of its lunge

    float moveAccel = 0.f;      // m/s^2: how fast the velocity gains speed toward the one the stick asks for ...
    float moveBrake = 0.f;      // ... and loses it (a slower one asked for); 0: instant. Idle, Move, Guard, Heal only

    float loadShare = 1.f;      // a parry stores the parried attack's damage times this as Load
    ArenaAttack release;        // the Release strike (riposte-shaped)
    float releasePoise = 1.f;   // poise each enemy the Release connects with loses per Load spent
    float pairWindow = 0.12f;   // seconds between a parry and the partner's press that still pair
    std::uint8_t pairHits = 3;  // a counter-stagger is worth this many of light[0]'s hits
    float readPower = 0.5f;     // a Read counter at one stack: this many of light[0]'s damage ...
    float readStackPower = 0.2f; // ... plus this share for each stack past the first
    float readPoise = 5.f;      // poise a Read counter costs the attacker (upright only)
    float readWrongPoise = 10.f; // poise a misread gives the attacker back (up to its kind's poise)
    // Carrying (ArenaFight::setCarry, setAlly): Load from blocks and from taking half a blow aimed at the ally.
    float blockLoadShare = 0.25f;   // a block stores the blocked damage times this as Load (0..1)
    float takeHalfWindow = 0.25f;   // s before a blow lands on the ally that a take half press still counts (0.05..0.6)
    float takeHalfReach = 4.f;      // m, centre to centre, the player may stand from the ally (1..10)
    float takeHalfLoadShare = 1.f;  // the hp the player takes, times this, is stored as Load (0..2)
    float takeHalfRearm = 0.5f;     // s after a take half press before the next one opens a window (0..2)
    // The cylinder (a guns kit only; arena.json "rounds" 0..30, "reload" 0..10 s). 0 rounds: no cylinder, the shot
    // is limited by stamina alone. Otherwise each shot spends a round; with none left a shot press is dropped (a
    // kick still starts), and once the last round's shot has ended the cylinder refills after `reload` seconds of
    // the player's own time (a hit pause holds it). Full at begin() and restart().
    std::uint8_t rounds = 0;
    float reload = 1.2f;
};

// Whether the player may take this kit as the lead: only one with attacks of its own. A kit without is a party
// slot, never played alone: the explore start, the explore lead switch and the arena's lead pick pass over it.
inline bool arenaPlayable(const ArenaFighter& kit) { return kit.attacks; }

// A gun kit's shot reaches at least this far (the shipped data holds to it); its kicks are close-in moves.
inline constexpr float kArenaShotRange = 5.f;
// The strike pattern of a lead's kit for a FightAnimator::setPattern, in chain order (one entry per light, so at most
// kMaxLightChain): None and Gauntlets punch (StrikeLeft, StrikeRight, ...), Magic casts (Cast), Guns kicks (KickLeft,
// KickRight in turn; its shot is the aimed light, kArenaShotMove, never in the chain). Writes up to `cap` entries to
// `out` and returns how many (no allocation).
FTCORE_API std::size_t arenaStrikePattern(const ArenaFighter& kit, FightClip* out, std::size_t cap);
// The clip of the kit's heavy: Guns kicks (KickRight), Magic casts, the rest punch (StrikeRight).
FTCORE_API FightClip arenaHeavyStrike(const ArenaFighter& kit);
// The kit's attack for the player's move id (an event's or ArenaPlayerOut::move: a light's chain index below
// lightCount, kArenaHeavyMove, kArenaRiposteMove, kArenaReleaseMove, or kArenaShotMove on a guns kit); nullptr for any
// other id.
FTCORE_API const ArenaAttack* arenaPlayerAttack(const ArenaFighter& kit, std::uint8_t move);
// The clip that shows the player's move: a light its pattern strike (arenaStrikePattern), the heavy
// arenaHeavyStrike's, a riposte Counter, a Release Finisher, a guns kit's shot WeaponRight; Stance for any other id. A host plays it with
// FightAnimator::playStrike (a riposte or Release with play) keyed to the attack's active part, startup after
// AttackStarted.
FTCORE_API FightClip arenaPlayerStrike(const ArenaFighter& kit, std::uint8_t move);
// The leads' strike clips: the id each FightClip a lead plays has in Data/Anim/beat_clips.json and in the look's clip
// folder ("lead_jab", "lead_cross", "lead_kick_r", "lead_counter", "lead_finisher", "lead_fire" for the shot,
// "lead_cast" for the spell); nullptr for a clip no lead has. A literal: valid forever.
FTCORE_API const char* arenaLeadClipId(FightClip clip);

// What an enemy move shows during its startup: the word on its Read card (append only; generic shapes,
// the host shows each through its own ui string). arena.json spells it as arenaTellName gives it.
enum class ArenaTell : std::uint8_t { Strike, Overhead, Backhand, Charge, Thrust, Slash, Sweep, Slam, Shot };
inline constexpr std::size_t kArenaTellCount = 9;
FTCORE_API const char* arenaTellName(ArenaTell t); // "strike", "overhead", ... Literals.

// One of an enemy's moves: an attack plus when the enemy picks it.
struct ArenaEnemyMove {
    ArenaAttack attack;
    float minRange = 0.f;     // picked only with the player at least this far ...
    float maxRange = 2.5f;    // ... and at most this far
    float cooldown = 0.f;     // seconds before it can be picked again
    float weight = 1.f;       // relative chance among the moves that fit
    std::int8_t chain = -1;   // the move that follows at once (a string), or -1
    bool unblockable = false; // a guard doesn't stop it: dodge it
    bool parryable = true;
    // Whether it reaches a jumping player (ArenaFighter::jumpClearStart..End). arena.json "hitsAir"; when the file
    // leaves it out, false for a sweep or a slam (low, along the floor) and true for every other tell.
    bool hitsAir = true;
    ArenaTell tell = ArenaTell::Strike; // its Read card's word; arena.json names a different one per move
    // The id of the animation clip the host plays for this move (a skinned enemy's look.clips table), "" for none.
    // Only the host reads it: the fight's rules never do.
    std::array<char, kMaxArenaClipId + 1> clipText{};

    std::string_view clip() const { return std::string_view(clipText.data()); }
};

// How the host draws an enemy kind as a skinned mesh instead of its placeholder shapes (arena.json's `look`). All
// host-side, except that a pitched shot (ArenaInput::aimPitched) meets the body where `hover` and `scale` put it. An
// empty `clipsText` is no look (the shapes).
struct ArenaEnemyLook {
    std::array<char, kMaxArenaLookPath + 1> clipsText{}; // project-relative path of the clip table (Data/Anim/*.json)
    float hover = 0.f;     // m: the mesh is lifted this far above the enemy's position on the ground
    float scale = 1.f;     // the mesh's scale
    float yawOffset = 0.f; // degrees added to the mesh's own facing

    std::string_view clips() const { return std::string_view(clipsText.data()); }
    bool skinned() const { return clipsText[0] != 0; }
};

struct ArenaEnemyKind {
    int hp = 80;
    float poise = 30.f;
    float poiseDelay = 3.f;
    float staggerTime = 1.0f; // poise broken
    float openTime = 1.5f;    // parried: open to a riposte this long
    float downTime = 1.2f;    // after a riposte
    float walkSpeed = 1.8f;
    float runSpeed = 4.f;
    float strafeSpeed = 1.4f;
    float turnRate = 6.f;     // rad/s outside attacks
    float preferredRange = 3.5f;
    float runRange = 8.f;
    float attackDelay = 1.2f; // after a string ends, before it asks for a token again
    float bodyRadius = 0.45f;
    float height = 0.f;       // m, standing; 0: derived from bodyRadius (arenaEnemyHeight). Only the host's figure
                              // and the arena stage's scale read it: the fight's rules never do
    std::array<ArenaEnemyMove, kMaxArenaMoves> moves{};
    std::uint8_t moveCount = 1;
    float accel = 0.f;        // m/s^2: how fast its velocity moves toward the one it wants, both ways; 0: instant.
                              // Approach and Strafe only
    float bpm = 0.f;          // the beat its startups are whole numbers of, when the file's own doesn't fit it; 0: the file's
    bool passive = false;     // never asks for an attack token: a target dummy (arena.json's `dummy`); with its
                              // speeds at 0 it stands where it spawned and only takes blows
    ArenaEnemyLook look;      // the host's skinned figure for it, if any
    float flinchTime = 0.f;   // > 0: a landed blow flinches it this long (see Flinch); 0: it never flinches
    float flinchPoise = 0.f;  // the least poise damage a blow needs to flinch it
    float flinchPush = 0.f;   // metres a flinch pushes it back
    std::uint8_t breakAt = 0; // > 0: the blows in a row that make it break out (see Break-out); 0: never
    float breakDecay = 1.5f;  // s without a blow after which the count goes back to 0
    int breakMove = -1;       // the move it breaks out with; -1: none
    // A set table (TableRing.h). Only a set table reads them: the fight's own rules never do.
    std::uint8_t carry = 0;   // carry grade: a seated one's share of a blow halves for each grade past the table's seats
    bool seatable = true;     // whether it may be seated at a table at all
};

// An enemy kind's standing height in metres: its `height` when set, else 1.75 m at the default body radius
// 0.4 m, scaled with the radius between 0.8x and 1.6x of that (the look the host's figure always had).
inline float arenaEnemyHeight(const ArenaEnemyKind& kind) {
    if (kind.height > 0.f) return kind.height;
    const float scale = kind.bodyRadius / 0.4f;
    return 1.75f * (scale < 0.8f ? 0.8f : (scale > 1.6f ? 1.6f : scale));
}

// kArenaNoKind in every slot (the default of ArenaDef::enemyKind).
constexpr std::array<std::uint8_t, kMaxArenaEnemies> arenaNoKinds() {
    std::array<std::uint8_t, kMaxArenaEnemies> kinds{};
    for (std::size_t i = 0; i < kinds.size(); ++i) kinds[i] = kArenaNoKind;
    return kinds;
}

// One fight: the player's kit and the enemies (one kind per enemy; kinds may repeat).
struct ArenaDef {
    ArenaFighter fighter;
    std::array<ArenaEnemyKind, kMaxArenaEnemies> enemies{};
    std::uint8_t enemyCount = 1;
    float ringRadius = 12.f;   // the ring the host should build (arenaRingAt)
    float spawnDistance = 7.f; // enemies start this far in front of the player
    std::uint8_t attackers = 1;
    // Slot i's index into ArenaTable::enemyIds: the kind of enemies[i], from which the host picks that enemy's
    // mesh and clips. kArenaNoKind (the default, and what defaultArenaDef() leaves) is a def built in code.
    std::array<std::uint8_t, kMaxArenaEnemies> enemyKind = arenaNoKinds();
    float bpm = 0.f;           // the arena's beat (arena.json's "bpm"); every enemy startup is whole beats of it. 0: none
};

// The built-in fight used when no data file is there: one enemy with a quick strike and a slow
// two-hit string, against the default fighter.
FTCORE_API ArenaDef defaultArenaDef();

// The circle nothing leaves.
struct ArenaRing {
    Vec2 centre;
    float radius = 12.f;

    // p pulled inside the ring, at least `margin` from its edge (onto the centre when margin >= radius).
    Vec2 clamp(Vec2 p, float margin = 0.f) const {
        const float limit = radius > margin ? radius - margin : 0.f;
        const float dx = p.x - centre.x, dy = p.y - centre.y;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d <= limit) return p;
        if (d <= 0.f) return centre;
        const float s = limit / d;
        return Vec2{centre.x + dx * s, centre.y + dy * s};
    }
    bool contains(Vec2 p, float margin = 0.f) const {
        const float dx = p.x - centre.x, dy = p.y - centre.y;
        const float limit = radius - margin;
        return limit >= 0.f && dx * dx + dy * dy <= limit * limit;
    }
};

// The ring for a fight that starts with the player at `playerPos` facing `facing` (unit): `radius`
// around a centre radius * 0.35 ahead of the player, so the enemies' side has room, then moved (and
// shrunk if it must be) to fit inside `bounds` (a plaza; bounds.radius <= 0: no bounds).
FTCORE_API ArenaRing arenaRingAt(Vec2 playerPos, Vec2 facing, float radius, const ArenaRing& bounds);
// Where follower i waits while the lead fights: just outside the ring, behind the player's start,
// spread around the edge (i = 0, 1, ...).
FTCORE_API Vec2 arenaParkSpot(const ArenaRing& ring, Vec2 playerStart, std::size_t i);

// A point in metres: x, y on the floor plane, z up.
struct ArenaPoint3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

// A gun kit's round leaves the lead this high over the floor (m): a pitched shot's line starts here.
inline constexpr float kArenaShotHeight = 1.4f;

// What the player pressed this frame. Edges (true on the frame of the press) except `guard`.
struct ArenaInput {
    Vec2 move;                  // the stick on the ground, already turned by the camera; length 0..1
    Vec2 view{1.f, 0.f};        // the camera's forward on the ground (unit): lock-on picks from it
    bool light = false;
    bool heavy = false;
    bool dodge = false;
    bool jump = false;
    bool heal = false;
    bool guard = false;         // held
    bool lock = false;          // lock on, or let go
    std::int8_t lockSwitch = 0; // -1 / +1: the next enemy to the left / right
    bool release = false;       // the Release (open above kArenaReleaseOpenLoad)
    bool pair = false;          // the partner's press: a second lead's input, paired with a parry within pairWindow
    std::int8_t read = -1;      // the partner's Read: the index of the card picked, or -1
    // The aim (AimStance, right mouse held): while on, the player turns toward aimDir (the camera's aim on the ground,
    // unit) at aimTurn rad/s (0: the kit's turnRate) instead of the stick or the lock, attacks start and track toward
    // it, and walking is scaled by aimSpeed (0..1). A guns kit's light pressed while aiming is its shot.
    bool aim = false;
    Vec2 aimDir{1.f, 0.f};
    float aimTurn = 0.f;
    float aimSpeed = 1.f;
    // The round's climb (rad, + up) from kArenaShotHeight toward the crosshair. Pitched, a guns kit's shot is a line in
    // space along the lead's facing: it meets the first standing body (ArenaLook's arenaBodyCylinder, its radius
    // widened by half the shot's width) it enters within the shot's range, so a round over a short enemy's head goes
    // on to the body behind it. Unpitched (the autopilot, a host without a camera), it is the flat lane. A shot keeps
    // the choice it started with; while pitched it follows aimPitch until it fires.
    bool aimPitched = false;
    float aimPitch = 0.f;
    // Attacking on the move (AttackMove, the host's attackMove for the attack under way): walking during an attack at
    // attackSpeed (0..1) of the pace (ramped from the pace the attack began at, at the brake or the accel, so a 0
    // slides to a stop), and turning at up to attackTurn rad/s all through it toward the aim, the lock or the stick
    // (during startup the attack's own tracking if faster). Both 0 (the defaults, for a host that moves nothing): the
    // attack roots at once and holds its facing after startup. The game's hosts always fill both from attackMove,
    // whose turn is never 0, so a lead's facing is never held.
    float attackSpeed = 0.f;
    float attackTurn = 0.f;
    bool takeHalf = false;      // take half (ArenaFight::setAlly): the next blow to land on the ally within the window
};

// Where the bodies stand, as the host moved them (enemies beyond enemyCount() are ignored).
struct ArenaBodies {
    Vec2 player;
    std::array<Vec2, kMaxArenaEnemies> enemies{};
    Vec2 ally; // the ally body (ArenaFight::setAlly), moved by the host as it moves the player
};

// The player's action (append only: the Unreal side maps it to animations).
enum class ArenaAction : std::uint8_t {
    Idle, Move, Light, Heavy, Riposte, Dodge, Guard, Heal, Hitstun, GuardBreak, Downed,
    Release, // appended
    Jump,    // appended: take-off, air and landing (an air light is Light with kArenaAirMove)
};
// An enemy's brain state (append only).
enum class ArenaBrain : std::uint8_t { Approach, Strafe, Attack, Staggered, Open, Down, Dead, Flinch /* appended */ };
// Where an attack is.
enum class ArenaPart : std::uint8_t { None, Startup, Active, Recovery };

FTCORE_API const char* arenaActionName(ArenaAction a); // "Idle", "Move", ... Literals.
FTCORE_API const char* arenaBrainName(ArenaBrain b);

struct ArenaPlayerOut {
    Vec2 velocity;                    // m/s this frame
    Vec2 facing{1.f, 0.f};            // unit: the facing the rules use (guard, parry, backstep, riposte, attacks)
    Vec2 drawFacing{1.f, 0.f};        // unit: the facing to draw the body at. Outside attacks and dodges it turns toward
                                      // `facing` at turnRate, so a lock that snaps the facing back after a dodge turns
                                      // the drawn body round over a few frames (turnRate 0: the same as `facing`); an
                                      // attack's startup turns it faster and it is `facing` from the active part on
    ArenaAction action = ArenaAction::Idle;
    ArenaPart part = ArenaPart::None; // Light, Heavy, Riposte, Release
    std::uint8_t move = 0;            // Light: the chain index or kArenaShotMove; kArenaHeavyMove; kArenaRiposteMove; kArenaReleaseMove
    float actionTime = 0.f;           // seconds since the action started
    bool invulnerable = false;        // dodge i-frames, a riposte or a Release
    bool parrying = false;            // inside a parry window
    std::uint8_t lock = kArenaNobody; // the locked enemy
    std::uint8_t soft = kArenaNobody; // the current attack's soft target (living), else kArenaNobody
    Vec2 dodgeDir{1.f, 0.f};          // unit: set once when a dodge starts (the stick, or the backstep's direction,
                                      // before any ring cut) and kept until the next dodge
    bool backstep = false;            // whether the current or last dodge is a backstep
    bool dodgeBack = false;           // ... and whether it goes back from the locked target (within kArenaDodgeBackCos of
                                      // straight away from it): drawn facing the target, not as a roll the way it goes
    std::int8_t dodgeSide = 0;        // ... or a side dash across it (ArenaFight::setSideDashes): +1 to the target's
                                      // right (the ground plane has y to the right), -1 to its left; drawn side-on to
                                      // its way on the target's side, so the dash goes to the drawn body's right or left
    int load = 0;                     // the Load gauge, 0..kMaxLoad (beside stamina)
    bool overloaded = false;          // Load at the cap (loadCap): light, heavy and heal are refused until a Release
    float stopLeft = 0.f;             // s left in a hit pause (0: none); its action time and velocity stand still
    float wantSpeed = -1.f;           // m/s the stick asks of a walk that ramps (Idle, Move, Guard, Heal) this frame, the
                                      // pace the velocity heads for; -1 otherwise (an attack, a dodge, a pause, a stun)
    float stopDistance = -1.f;        // m the body would slide at moveBrake from this velocity to a stand, while wantSpeed
                                      // is known; -1 otherwise
    int loadCap = kMaxLoad;           // the most Load the gauge holds now (ArenaFight::setLoadCap)
    float jumpTime = -1.f;            // s on the air clock since take-off (Jump, or an air light), -1: not jumping
    float height = 0.f;               // m over the floor: the jump's arc (0 on the ground); the host draws it
    bool airClear = false;            // inside jumpClearStart..End this tick: low moves pass under
};

struct ArenaEnemyOut {
    Vec2 velocity;
    Vec2 facing{1.f, 0.f};
    ArenaBrain brain = ArenaBrain::Approach;
    ArenaPart part = ArenaPart::None; // Attack
    std::uint8_t move = 0;            // Attack: the move index
    float stateTime = 0.f;            // seconds in this brain state (Attack: in this move)
    float stateLength = 0.f;          // Staggered, Open, Down: the state's whole length, s (the host's hold while one
                                      // runs; +infinity until a release); Flinch: flinchTime; 0 in any other state
    bool token = false;               // holds an attack token
    float stopLeft = 0.f;             // s left in a hit pause (0: none); its state time and velocity stand still
};

enum class ArenaEventType : std::uint8_t { // append only
    AttackStarted, // source began attack `move`
    Hit,           // source's attack hit target for `amount` hp (after any guard)
    Blocked,       // the player's guard stopped source's attack; amount = stamina lost
    GuardBroken,   // that block emptied the player's stamina
    Parried,       // the player parried source, which is now Open
    Evaded,        // source's attack passed through the rolling player
    Staggered,     // target's poise broke
    Healed,        // amount = hp restored
    Downed,        // target reached 0 hp (an enemy dies; the player: Defeat follows)
    LockChanged,   // target = the newly locked enemy, or kArenaNobody
    StaminaEmpty,  // the player spent the last stamina
    Victory,
    Defeat,
    LoadStored,     // `amount` Load stored: by a parry of source's attack `move`, or the host's giveLoad
                    // (source kArenaNobody, move 0) (appended)
    Overloaded,     // the player's Load reached kMaxLoad: only a Release lifts it (appended)
    Released,       // the player began a Release, spending `amount` Load (appended)
    CounterStagger, // a parry paired with the partner's press: target took `amount` hp and is staggered (appended)
    CardsDealt,     // Read cards dealt for source's move `move`: amount = how many (ArenaFight::readCards) (appended)
    Read,           // the partner picked the right card for source's `move`: amount = the stacks now (appended)
    Misread,        // ... a decoy: the stacks are 0 and source regained `amount` poise (appended)
    ReadCounter,    // the partner's Read counter hit target for `amount` hp; move = the stacks (appended)
    Flinched,       // the player's blow made target flinch (appended)
    BrokeOut,       // source broke out of the player's blows with its move `move` (appended)
    HostHit,        // the host's hostBlow took `amount` hp from target (a held blow's bonus); move = the blow's (appended)
    LoadSpent,      // the host's spendLoad removed `amount` Load (appended)
    AllyHit,        // source's attack `move` hit the ally (target kArenaAlly) for `amount` hp (appended)
    AllyDowned,     // the ally reached 0 hp: down for the rest of the fight, never a Defeat (appended)
    TookHalf,       // the player took half of source's blow on the ally: `amount` hp (target kArenaPlayer) (appended)
    TableShare,     // a set table passed `amount` hp of source's blow on a seated lead to target: the player, the ally
                    // or a seated enemy (ArenaFight::setTableSeats) (appended)
};

struct ArenaEvent {
    ArenaEventType type = ArenaEventType::Hit;
    std::uint8_t source = kArenaNobody;
    std::uint8_t target = kArenaNobody;
    std::uint8_t move = 0;
    int amount = 0;
    Vec2 at; // the target's position (the source's when there is no target)
};

// One tick's events, in the order they happened. Fixed capacity: no allocation.
struct ArenaEvents {
    std::array<ArenaEvent, kMaxArenaEvents> items{};
    std::size_t count = 0;

    const ArenaEvent* begin() const { return items.data(); }
    const ArenaEvent* end() const { return items.data() + count; }
    std::size_t size() const { return count; }
    bool empty() const { return count == 0; }
    const ArenaEvent& operator[](std::size_t i) const { return items[i]; }
};

struct ArenaOutput {
    ArenaPlayerOut player;
    std::array<ArenaEnemyOut, kMaxArenaEnemies> enemies{};
    ArenaEvents events;
};

struct ArenaStats {
    float time = 0.f;
    int damageDealt = 0;
    int damageTaken = 0;
    int hits = 0;      // the player's attacks that connected (per target)
    int hitsTaken = 0;
    int blocks = 0;
    int parries = 0;
    int ripostes = 0;
    int evades = 0;
    int heals = 0;
    int staggers = 0;  // enemy poise breaks
    int flinches = 0;  // enemy flinches
    int flinchChances = 0; // blows of flinchPoise or more on an upright kind that flinches, host-allowed (a flinch
                           // could show; the chain cap and an active or armoured attack may still stop it)
    int breakOuts = 0; // enemy break-outs
    int breakDue = 0;      // blows that brought an upright enemy's count to its breakAt without staggering it
    int breakRunBest = 0;  // the longest run toward a break-out (relative to its breakAt), counted as each blow lands
    int breakRunNeed = 0;  // that run's breakAt (0: no blow counted)
    int breakUpright = 0;  // the player's blows landed on an upright enemy whose kind has a breakAt
    int breakVetoed = 0;   // ... of them the host barred (as it bars a flinch), so none counted
    int breakBarred = 0;   // breakDue whose break move the enemy's move mask barred
    int cancels = 0;          // actions the player started inside an attack (its recovery or cancel window)
    int cancelChances = 0;    // attacks in whose cut window a press or a guard waited (a cancel could show)
    int lights = 0;           // light attacks started (not ripostes or shots)
    float lightStamina = 0.f; // stamina those lights cost
    int softTargets = 0;      // attacks started on a soft target
    int softChances = 0;      // attacks started with nothing locked, no aim and a kit with a softRange (one could)
    int shots = 0;            // a guns kit's shots started
    int shotHits = 0;         // of them, the ones that connected (per target; also in hits)
    int reloads = 0;          // the cylinder refilled (ArenaFighter::rounds)
    int dryPresses = 0;       // shot presses dropped with the cylinder empty
    int guardBreaks = 0;
    int restarts = 0;
    int loadStored = 0;      // Load stored by parries
    int releases = 0;
    int overloads = 0;
    int counterStaggers = 0; // parries paired with the partner's press
    int readsDealt = 0;      // deals of Read cards
    int reads = 0;           // cards picked
    int correctReads = 0;
    int readCounters = 0;    // Read counters that landed
    int readDamage = 0;      // their damage (also in damageDealt)
    // Pace (ArenaImpact.h's arenaPaceText), summed over restarts:
    int kills = 0;                                      // enemies killed
    std::array<float, kMaxArenaEnemies> killSeconds{};  // per slot: seconds from the player's first blow on it to its
                                                        // death, summed over its kills (a kill with no blow adds none)
    std::array<int, kMaxArenaEnemies> killCount{};      // per slot: the kills those seconds cover
    int punishWindows = 0; // times an enemy went Staggered, Open or Down (Open turning Down is the same window)
    int punishUsed = 0;    // ... of them, the windows the player's side landed a blow in
    int blockLoad = 0;     // Load stored by blocks (ArenaFight::setCarry)
    int takeHalfLoad = 0;  // Load stored by take half
    int takeHalves = 0;    // take halves that landed
    int jumps = 0;         // jumps started
    int landings = 0;      // ... that came down on their feet (not cut by a blow, a defeat or the fight's end)
    int airLights = 0;     // air lights started
    int airEvades = 0;     // enemy moves that passed under a jump (also in evades)
};

// Per-feature verdicts over a fight's stats, for the headless demo's check lines: each is true when the fight
// showed the feature at least once.
// A check's verdict: n/a (the run gave it no chance), PASS, FAIL, or barred (it was due and a mask barred it).
enum class ArenaCheck : std::uint8_t { NotShown, Pass, Fail, Barred };
FTCORE_API const char* arenaCheckText(ArenaCheck c); // "n/a", "PASS", "FAIL", "barred"

struct ArenaFeatureChecks {
    bool lightsFree = false; // lights were started and cost no stamina
    bool cancels = false;    // an action cut into an attack
    bool flinch = false;     // an enemy flinched
    bool breakOut = false;   // an enemy broke out
    bool breakOutDue = false; // a blow reached an enemy's breakAt without staggering it (else break-out can't show)
    bool softTarget = false; // an attack started on a soft target
    bool shots = false;      // a guns kit fired a shot that connected
    // Verdicts with their own can-show rule (the host adds its own for shots):
    ArenaCheck lightsFreeCheck = ArenaCheck::NotShown; // n/a with no light started
    ArenaCheck cancelsCheck = ArenaCheck::NotShown;    // n/a with no cancelChances
    ArenaCheck flinchCheck = ArenaCheck::NotShown;     // n/a with no flinchChances (a kit too light, or poise first)
    // n/a with no softChances: every attack started locked on or aimed (the autopilot locks unless -ArenaNoLock; a
    // guns kit's aimed shot always takes the aim), or the kit has no softRange.
    ArenaCheck softTargetCheck = ArenaCheck::NotShown;
    // PASS on a break-out; barred when it was due (or every upright blow vetoed) and barred; FAIL when it was due and
    // unbarred, or upright blows landed and the run never moved; n/a with no upright blow, or poise always first.
    ArenaCheck breakOutCheck = ArenaCheck::NotShown;
    // PASS when a jump landed; FAIL when jumps started and none landed; n/a with no jump.
    ArenaCheck jumpCheck = ArenaCheck::NotShown;
};

FTCORE_API ArenaFeatureChecks arenaFeatureChecks(const ArenaStats& stats);

enum class ArenaPhase : std::uint8_t { NotStarted, Fighting, Victory, Defeat };

// A blow about to land, as the blow hook sees it (ArenaFight::setBlowHook): one of the player's attacks or the
// partner's counters on an enemy, or an enemy's attack on the player.
struct ArenaBlow {
    ArenaEventType type = ArenaEventType::Hit; // the event it reports with: Hit (an attack, either way),
                                               // CounterStagger or ReadCounter (the partner's, on an enemy), or
                                               // HostHit (a bonus, ArenaFight::ruledHostBlow; never from tick)
    std::uint8_t source = kArenaNobody;        // kArenaPlayer (the partner's counters too) or the enemy
    std::uint8_t target = kArenaNobody;        // the enemy, or kArenaPlayer
    std::uint8_t move = 0;                     // as the event's
    int damage = 0;                            // hp, before the ruling (and before a guard)
    float poise = 0.f;                         // poise damage, before the ruling (and before armor). A counter-stagger:
                                               // the enemy kind's whole poise, as it breaks it
    Vec2 from;                                 // the source's position (the partner's counters: the player's) ...
    Vec2 facing{1.f, 0.f};                     // ... and facing
    Vec2 at;                                   // the target's position
    float time = 0.f;                          // fight time at the end of the tick it lands in
};

// The host's ruling on a blow. A vetoed blow is spent (that attack never connects with that target again, and it
// uses up the target slot maxTargets counts) and does nothing else: no hp, poise, Load, stamina, stats or events.
// Scales: NaN or below 0 count as 0, and a scaled blow stops at a million.
// * damage: the damage becomes round(damage * this), wherever it goes: hp landed, a guard's chip, a parry's Load.
// * poise: the poise damage is multiplied by this where the blow deals poise damage: an enemy's landed blow, a
//   player's blow on an enemy on its feet. A counter-stagger breaks the whole poise or nothing: it staggers only with
//   a poise scale of 1 or more. A guarded blow deals none (its stamina cost, guardDamage, is the attack's own), and
//   a riposte on an open enemy downs it whatever the scale.
struct ArenaBlowRuling {
    float damage = 1.f;
    float poise = 1.f;
    bool veto = false;
};

// Who sits at a set table (TableRing.h). All off by default: nothing is shared.
struct ArenaTableSeats {
    bool player = false;
    bool ally = false;
    std::uint8_t enemies = 0;                          // bit i: enemy i
    std::array<float, kMaxArenaEnemies> enemyShare{}; // a seated enemy's part of the even split, times this (0..4)

    bool any() const { return player || ally || enemies != 0; }
};

// Called inside tick(), once per blow, in the order the blows land (a player's attack reaching several enemies on
// one tick: by enemy index). It may read the fight's getters: mid-tick, time() and output() are still the last
// tick's (ArenaBlow::time is this one's end), and enemyHp and hp don't count the blow being ruled yet. It may call
// holdEnemy and releaseEnemy (they count from the next tick), setEnemyHpFloor (it counts for this blow already),
// setEnemyMoveMask (from the next pick) and setBlowHook (from the next blow). tick, begin and restart called from
// inside it do nothing (tick only returns the last output). A copy of a fight copies the hook and its user pointer
// (a copy made inside the hook ticks as any other).
using ArenaBlowHook = ArenaBlowRuling (*)(void* user, const ArenaBlow& blow);

class ArenaFight {
public:
    // Starts `def`: the player at playerPos facing playerFacing (unit), the enemies spread on an arc
    // spawnDistance in front of it, inside `ring`, facing the player. Full hp, stamina, poise and
    // charges; nothing locked.
    FTCORE_API void begin(const ArenaDef& def, const ArenaRing& ring, Vec2 playerPos, Vec2 playerFacing,
                          std::uint32_t seed);
    // Back to the state begin() left (the same seed); stats keep counting, plus one restart.
    FTCORE_API void restart();

    // Advances the fight by dt seconds (dt <= 0 only refreshes the output). `bodies` are where the
    // host put everyone after the last output. Returns the fight's own output storage.
    FTCORE_API const ArenaOutput& tick(float dt, const ArenaInput& input, const ArenaBodies& bodies);

    // What a steady player would press now, from the state after the last tick (pure; reads nothing
    // else). It locks the nearest enemy, closes in and attacks with the light chain when its target
    // isn't winding up, parries or dodges sideways out of an enemy attack about to become active
    // (parry only for parryable moves and a fighter with parryWindow >= 0.12 s), ripostes an open
    // enemy, releases at 50 Load on an enemy on its feet in reach (and at once when overloaded), and
    // heals below 40% hp when nothing is near or winding up. It never presses the partner's `pair`,
    // but reads for the partner: the right card, 0.2 s into the startup (or half of a
    // shorter one). It must win every fight in
    // arena.json within 120 s of fight time, and with no input every fight must be lost within 120 s
    // (the tests tick at a fixed 1/60 s and move the bodies with arenaStep).
    FTCORE_API ArenaInput autopilot() const;

    // The standing enemy a pitched shot from the player's place (as of the last tick) along `dir` (unit, on the ground)
    // at `pitch` would meet first, by the rule ArenaInput::aimPitched gives; kArenaNobody for none or a kit without a
    // shot. `distance`, when given, gets how far along the round's line it enters (0 for a lead inside the body). A
    // host shows the crosshair as out of reach when this is not the enemy under it. Enemies with their bit in `skip`
    // are left out (to ask how far along the round the one under the crosshair would be, the others aside).
    FTCORE_API std::uint8_t shotMeets(Vec2 dir, float pitch, float* distance = nullptr, std::uint32_t skip = 0u) const;
    // Opt in (off by default; begin and restart keep it): with a guns kit the autopilot also aims and shoots while it
    // closes in on its target, when the target stands within the shot's range and outside the light chain's reach and
    // the shot leaves a dodge's stamina in reserve. Off, the autopilot never aims.
    FTCORE_API void setAutopilotShoots(bool on);
    FTCORE_API bool autopilotShoots() const;

    FTCORE_API ArenaPhase phase() const;
    bool over() const {
        const ArenaPhase p = phase();
        return p == ArenaPhase::Victory || p == ArenaPhase::Defeat;
    }
    FTCORE_API float time() const; // seconds since begin() / restart()
    // The held clock (see Hit pause): time() less the player's hit pauses; 0 at begin() / restart(). The clock
    // the shared combat inputs are ticked with. hitStopLeft: seconds left in the player's hit pause (0: none).
    FTCORE_API float heldTime() const;
    FTCORE_API float hitStopLeft() const;
    FTCORE_API const ArenaDef& def() const;
    FTCORE_API const ArenaRing& ring() const;
    FTCORE_API const ArenaOutput& output() const; // the last tick's
    FTCORE_API const ArenaStats& stats() const;
    FTCORE_API std::uint32_t droppedEvents() const;

    FTCORE_API Vec2 playerStart() const;
    FTCORE_API Vec2 playerStartFacing() const;
    FTCORE_API std::size_t enemyCount() const;
    FTCORE_API Vec2 enemyStart(std::size_t i) const;
    FTCORE_API Vec2 enemyStartFacing(std::size_t i) const;

    FTCORE_API int hp() const;
    FTCORE_API float stamina() const;
    FTCORE_API float poise() const;
    FTCORE_API std::uint8_t healCharges() const;
    FTCORE_API int load() const;          // 0..kMaxLoad
    FTCORE_API bool overloaded() const;   // load() >= loadCap()
    FTCORE_API std::uint8_t lock() const; // kArenaNobody when nothing is locked
    FTCORE_API int enemyHp(std::size_t i) const;
    FTCORE_API float enemyPoise(std::size_t i) const;
    FTCORE_API bool enemyAlive(std::size_t i) const;

    // The Read deal open now (empty when none): its cards, the enemy and move it is for, and whether a pick
    // still counts (dealt, not picked, the move still in its startup). The Read stacks, 0..kMaxReadStacks.
    FTCORE_API const ReadCards& readCards() const;
    FTCORE_API std::uint8_t readFoe() const; // kArenaNobody when nothing is dealt
    FTCORE_API std::uint8_t readMove() const;
    FTCORE_API bool readOpen() const;
    FTCORE_API int readStacks() const;

    // ---- host hooks ----
    // Holds enemy i in `brain` (Open, Down or Staggered) for `seconds` (> 0; +infinity: until releaseEnemy), from the
    // start of the next tick with dt > 0: a live move is cut short and its token goes back, as a parry does (a read move
    // brings the partner's counter), and `seconds` stands in for the kind's openTime, downTime or staggerTime. Holding
    // again starts it over. Open invites a riposte, which ends the hold (Down for the kind's downTime), and a
    // counter-stagger ends it too (Staggered); Down invites neither. A held Staggered is not a stagger: no Staggered
    // event, no stats.staggers and the poise is left as it is (a poise break in it staggers anew, ending the hold). When a hold ends (its time, or a release) the enemy walks again, and the
    // kind's attackDelay counts from the hold's start, as after a parry. False, and nothing asked, for an enemy out of
    // range or dead, a fight not running (before begin(), after Victory or Defeat), another brain, or seconds NaN or
    // <= 0.
    FTCORE_API bool holdEnemy(std::size_t i, ArenaBrain brain, float seconds);
    // Ends enemy i's hold at the start of the next tick with dt > 0 (it walks again: Approach). Nothing for an enemy
    // that is not held then. Before a tick, the latest of holdEnemy and releaseEnemy for an enemy wins.
    FTCORE_API void releaseEnemy(std::size_t i);
    // A hold is running: not one still waiting for its tick, and true until a pending release applies.
    FTCORE_API bool enemyHeld(std::size_t i) const;
    // Enemy i's hp never falls below hpFloor (0, the default: none; below 0 counts as 0), from now: a blow that would
    // take it lower takes it to hpFloor (the event's amount is what it lost), so the enemy stays alive. A floor never
    // raises hp: at or below it, a blow takes nothing.
    FTCORE_API void setEnemyHpFloor(std::size_t i, int hpFloor);
    FTCORE_API int enemyHpFloor(std::size_t i) const;
    // Enemy i's picks may begin only the moves whose bit is set in `mask` (bit m: move m; every bit set, the
    // default). A move with its bit clear counts as one with weight 0 for the pick: never begun by it, and no band to
    // walk to, while a chain still runs into it. With no bit set, a pick holds (the enemy strafes). It applies from
    // the next pick and never cuts a move already begun. begin() and restart() set every bit again. Nothing for an
    // enemy out of range; enemyMoveMask is then 0.
    FTCORE_API void setEnemyMoveMask(std::size_t i, std::uint32_t mask);
    FTCORE_API std::uint32_t enemyMoveMask(std::size_t i) const;
    // While set, enemy i's moves keep the facing they began with: no tracking turn in startup, so the blow lands
    // along the line it was told on (a host that draws the strike point as the move begins). It applies from the next
    // tick, a move already in startup included. begin() and restart() clear it. Nothing for an enemy out of range;
    // enemyFacingHeld is then false.
    FTCORE_API void setEnemyFacingHeld(std::size_t i, bool held);
    FTCORE_API bool enemyFacingHeld(std::size_t i) const;
    // While set, enemy i keeps its facing between moves: Approach and Strafe turn it no more, and the host turns it
    // with setEnemyFacing (a host whose figure turns on stepping clips). It applies from the next tick. begin() and
    // restart() clear it. Nothing for an enemy out of range; enemyTurnHeld is then false.
    FTCORE_API void setEnemyTurnHeld(std::size_t i, bool held);
    FTCORE_API bool enemyTurnHeld(std::size_t i) const;
    // While false, enemy i never flinches (a boss whose script allows it only in some phases); true, the default,
    // leaves it to its kind's flinchTime. It applies from the next blow and never ends a flinch already running.
    // begin() and restart() set it again. Nothing for an enemy out of range; enemyFlinch is then false.
    FTCORE_API void setEnemyFlinch(std::size_t i, bool allowed);
    FTCORE_API bool enemyFlinch(std::size_t i) const;
    // Turns enemy i to `facing` (made unit) now; the next tick works from it and reports it. Nothing for a zero
    // vector or an enemy out of range.
    FTCORE_API void setEnemyFacing(std::size_t i, Vec2 facing);
    // Every blow goes to `hook` with `user` before it applies (ArenaBlowHook); nullptr: none (every blow as it is).
    FTCORE_API void setBlowHook(ArenaBlowHook hook, void* user);
    // The hook set now and its user pointer (nullptr, nullptr: none), for a host that chains its own ruling after it.
    FTCORE_API ArenaBlowHook blowHook() const;
    FTCORE_API void* blowHookUser() const;
    // While on, a locked dodge across the target (more than 45 degrees off both straight at it and straight away from
    // it) is a side dash: ArenaPlayerOut::dodgeSide gives its side, and the body is drawn side-on to the dash's way,
    // facing across it on the target's side, instead of rolling its way. The rules (facing, path, frames) are the same
    // either way. Off, the default, for a body with no side-dash clips. begin() and restart() keep it.
    FTCORE_API void setSideDashes(bool on);
    FTCORE_API bool sideDashes() const;
    // Stores `amount` Load in the player's gauge now, as a parry would (never past kMaxLoad; amount <= 0: nothing),
    // and returns what was stored. The same event types a parry raises report it: LoadStored (source kArenaNobody,
    // move 0) and, if the gauge filled, Overloaded. Inside the blow hook they join this tick's events; otherwise they
    // open the next tick's, ahead of anything that tick does (one LoadStored for all the Load given since). load(),
    // overloaded() and output().player show it at once; stats().overloads counts the fill at once, and
    // stats().loadStored stays the parries'. begin() and restart() empty the gauge and drop events not yet reported;
    // a fill already counted stays in the stats until begin() clears them. 0, and nothing stored, when the fight is
    // not running (before begin(), after Victory or Defeat), or for a kit with no attacks. kMaxLoad (Load.h) is the gauge's cap.
    FTCORE_API int giveLoad(int amount);
    // Extra damage from outside a blow (a held blow's settle, the fight session's bonuses): honours the hp floor,
    // counts in damageDealt, kills at 0 like a landed blow, and reports a HostHit (amount, move) for the enemy, then
    // its Downed when it died. No poise, no flinch, no hit pause, no hook call. The hp changes at once (enemyHp,
    // enemyAlive); the events come in the next tick's output (at once when called from a blow hook), and a kill's
    // Victory with that tick. False (nothing) on a dead or out-of-range enemy, damage <= 0, or outside Fighting.
    FTCORE_API bool hostBlow(std::size_t enemy, int damage, std::uint8_t move);
    // hostBlow, ruled first as a blow would be: `hook` (with `user`) sees an ArenaBlow (type HostHit, from kArenaPlayer
    // on the enemy, `damage`, no poise, the player's position and facing, the enemy's position, time()) and its veto
    // deals nothing, its damage scale scales the bonus. A HostHit blow is a bonus on a blow already ruled: a hook rules
    // it without counting a hit (no crit judged, no hit noted), vetoing it where the target takes no damage now. Pass the rulings that bar blows (a boss's invulnerable phase, a
    // weak point) and not the caller's own judge, so the bonus is not judged twice; nullptr: hostBlow as it is. The
    // fight's own hook is not called. Inside the call, tick, begin and restart do nothing, as inside the blow hook.
    // False (nothing) when vetoed, scaled to 0, or as hostBlow.
    FTCORE_API bool ruledHostBlow(std::size_t enemy, int damage, std::uint8_t move, ArenaBlowHook hook, void* user);

    // ---- carrying (Load by chairs) ----
    // Every setter here is kept across begin() and restart(), as the blow hook is; a fight that never calls them
    // behaves exactly as one before them (cap kMaxLoad, no block Load, no ally, no chairs).
    //
    // The Load cap, clamped to 1..kMaxLoad (kMaxLoad by default; PlaceTable::carryCap gives it from the chairs). It
    // never moves the Load. Every store stops at it (a parry, a block, take half, giveLoad), overloaded() is
    // load() >= loadCap(), and the Release opens above releaseOpenLoad(). If the Load is at or over the new cap and
    // was not overloaded before, it reports Overloaded (counted at once, reported as giveLoad's are).
    FTCORE_API void setLoadCap(int cap);
    FTCORE_API int loadCap() const;
    FTCORE_API int releaseOpenLoad() const; // loadCap() / 3 (integer): the Release is open while load() > this
    // Removes min(amount, load()) now and returns it; reports LoadSpent (amount = what was removed), at once inside
    // the blow hook, else ahead of the next tick's events. 0, and nothing, for amount <= 0 or outside Fighting.
    FTCORE_API int spendLoad(int amount);
    // Blocks store Load: the fighter's blockLoadShare of the blocked damage (after the hook's scale, before
    // guardAbsorb); a guard break still stores; a parry stores its own and no block Load. Off by default.
    FTCORE_API void setCarry(bool blocksStore);
    FTCORE_API bool carry() const;
    // The Named chairs' effects (Chairs.h). The heal charges apply from the next begin() or restart(); the rest at
    // once. A loadGain chair's gain counts on a store (not giveLoad's) while the ally is alive within its radius.
    FTCORE_API void setChairs(const ArenaChairs& chairs);
    FTCORE_API const ArenaChairs& chairs() const;
    FTCORE_API std::uint8_t roundsMax() const;    // the kit's cylinder (ArenaFighter::rounds); 0: none
    FTCORE_API std::uint8_t rounds() const;       // rounds still loaded, 0..roundsMax()
    FTCORE_API float reloadProgress() const;      // 0..1 while a reload runs, else 0
    FTCORE_API std::uint8_t healChargesMax() const; // the kit's charges plus the chairs' at the last begin or restart
    // One ally body (ArenaBodies::ally): maxHp > 0 turns it on with full hp, 0 or less turns it off (the default).
    // restart() fills its hp again. Every enemy attack's active part tests it as it tests the player (the move's
    // range and arc, `radius`); it never guards or rolls, and enemies still pick the player. A hit takes the move's
    // damage after the blow hook (an ArenaBlow with target kArenaAlly) and reports AllyHit; at 0 hp it is down
    // (AllyDowned) for the rest of the fight, never a Defeat. ArenaInput::takeHalf pressed within the fighter's
    // takeHalfWindow (plus the chairs') before such a hit lands, with the player within takeHalfReach of the ally,
    // not downed and not in a Release, and at least takeHalfRearm after the previous press, splits it: the player
    // loses (D + 1) / 2 hp (no guard, poise, hitstun or pause), the ally the rest, and the player's half times
    // takeHalfLoadShare is stored as Load (TookHalf, then LoadStored). A takeHalf chair heals the ally its share of
    // maxHp each time. A press no hit takes stays unused until its window passes.
    FTCORE_API void setAlly(int maxHp, float radius);
    FTCORE_API int allyHp() const;
    FTCORE_API int allyMaxHp() const;
    FTCORE_API bool allyAlive() const;
    FTCORE_API Vec2 allyPos() const; // as the last tick's bodies placed it
    FTCORE_API float allyRadius() const;
    // ---- a set table ----
    // The host's seats at a set table (TableRing.h; begin and restart clear them). While the player or the ally is
    // seated and two or more living bodies sit, the hp an enemy's blow takes from a seated lead (a hit, a guard's
    // chip, the ally's part after take half) is split evenly among them: each loses amount / bodies (rounded down)
    // and the one struck the rest. A seated enemy loses its part times its enemyShare (rounded, halves up; the hp
    // floor holds). Each part passed on reports TableShare; a part that empties the player downs it, the ally's
    // reports AllyDowned, an enemy's kills it. The struck lead's own part reports Hit or AllyHit as before.
    // The table's price: a seat means carrying a share whatever that body is doing. The parts passed on ignore guard,
    // parry, a dodge's i-frames, a Release and the air; only the struck body's defence counts (a parried, evaded or
    // dodged blow shares nothing; a blocked one shares its chip). A share can down or kill. Only enemy blows are
    // shared. Every part passed on is a TableShare event (amount, from whom), so no share is silent. A body leaves
    // its seat by walking out (TableRing.h: the setter too), from the next blow on.
    FTCORE_API void setTableSeats(const ArenaTableSeats& seats);
    FTCORE_API const ArenaTableSeats& tableSeats() const;
    // An enemy is in its attack's startup with `lead` s of it left or less, the ally is alive and the player within
    // takeHalfReach of it: when an autopilot would press take half.
    FTCORE_API bool takeHalfDue(float lead) const;

private:
    // The fight's state: the implementer's, fixed capacity only (copying a fight never allocates).
    // The rules are in ArenaFightImpl (ArenaFight.cpp), which reads and writes it directly.
    friend struct ArenaFightImpl;

    enum class Press : std::uint8_t { None, Light, Heavy, Dodge, Heal, Release, Shot, Jump }; // an action waiting in the input buffer
    enum class HoldAsk : std::uint8_t { None, Hold, Release };                     // a host's hold request

    struct PlayerState {
        Vec2 pos;
        Vec2 facing{1.f, 0.f};
        Vec2 drawFacing{1.f, 0.f};          // ArenaPlayerOut::drawFacing
        Vec2 velocity;                      // last output, after the ring cut
        int hp = 0;
        float stamina = 0.f;
        float lastSpend = -1.0e9f;          // fight time stamina was last spent
        float poise = 0.f;
        float lastPoiseHit = -1.0e9f;       // fight time of the last poise damage
        std::uint8_t charges = 0;
        ArenaAction action = ArenaAction::Idle;
        float actionTime = 0.f;             // seconds since the action started
        std::uint8_t move = 0;              // Light: the chain index; kArenaHeavyMove; kArenaRiposteMove
        std::uint8_t hitMask = 0;           // enemies the current attack has hit (one bit each)
        bool cancelChance = false;          // the current attack's cut window has had a press or guard waiting
        std::uint8_t target = kArenaNobody; // the enemy a riposte is aimed at
        std::uint8_t soft = kArenaNobody;   // the current attack's soft target ...
        float homeClose = 0.f;              // ... and the metres it closes in on it over startup and active
        bool healApplied = false;
        bool invulnerable = false;          // this tick: dodge frames or a riposte
        Vec2 dodgeDir{1.f, 0.f};            // the current or last dodge's direction (unit)
        bool backstep = false;              // ... and whether it is a backstep
        bool dodgeBack = false;             // ... and whether it goes back from the locked target (drawn facing it)
        std::int8_t dodgeSide = 0;          // ArenaPlayerOut::dodgeSide
        Press buffered = Press::None;
        float bufferAge = 0.f;
        bool chainLive = false;             // the last action was a light: the chain may go on
        std::uint8_t chainNext = 0;         // the light a chained press starts
        float lightEnd = 0.f;               // fight time that light ended
        bool shotPitched = false;           // the shot under way is a pitched round (ArenaInput::aimPitched at its start) ...
        float shotPitch = 0.f;              // ... climbing at this (rad)
        Vec2 attackWalk{};                  // the walking part of the velocity during an attack (the lunge apart)
        float wantSpeed = -1.f;             // ArenaPlayerOut::wantSpeed, this step
        bool guardHeld = false;             // last tick's guard flag
        float lastGuardPress = -1.0e9f;     // fight time of the last guard press (every press counts)
        float parryLeft = 0.f;              // seconds left in the parry window
        std::uint8_t lock = kArenaNobody;
        Load gauge;                         // the Load, beside stamina
        int releaseSpent = 0;               // the Load the Release in progress spent
        int giftLoad = 0;                   // Load giveLoad stored since the last tick, not yet reported ...
        bool giftFilled = false;            // ... and whether it filled the gauge
        float pairPress = -1.0e9f;          // fight time of the partner's last unused press
        float pairParry = -1.0e9f;          // fight time of the last unpaired parry ...
        std::uint8_t pairFoe = kArenaNobody; // ... and the enemy it opened
        int readStacks = 0;                 // correct Reads in a row, 0..kMaxReadStacks
        float stopUntil = -1.0e9f;          // fight time a hit pause ends
        float takeHalfPress = -1.0e9f;      // fight time of the last unused take half press that opened a window
        float lastTakeHalfPress = -1.0e9f;  // fight time of the last take half press (every press counts)
        int spentLoad = 0;                  // Load spendLoad removed since the last tick, not yet reported
        bool capFilled = false;             // setLoadCap overloaded the gauge since the last tick, not yet reported
        std::uint8_t rounds = 0;            // the cylinder's loaded rounds
        float reloadLeft = 0.f;             // > 0: a reload is running, this many seconds left
        float jumpTime = -1.f;              // ArenaPlayerOut::jumpTime
        Vec2 jumpVel{};                     // the velocity in the air (the take-off's, steered)
        bool airLightUsed = false;          // this jump's air light has started
        bool airClear = false;              // ArenaPlayerOut::airClear, this tick
    };

    struct EnemyState {
        Vec2 pos;
        Vec2 facing{1.f, 0.f};
        Vec2 velocity;                      // last output, after the ring cut
        int hp = 0;
        float poise = 0.f;
        float lastPoiseHit = -1.0e9f;
        ArenaBrain brain = ArenaBrain::Approach;
        float stateTime = 0.f;              // seconds in this brain state (Attack: in this move)
        std::uint8_t move = 0;
        bool token = false;
        bool hitPlayer = false;             // this attack has connected: landed, blocked or parried
        bool evaded = false;                // this attack's Evaded has been reported
        float stringEnd = 0.f;              // fight time the last string ended (0: the fight's start)
        float asked = 0.f;                  // seconds spent asking for a token
        std::int8_t strafeDir = 1;
        float strafeLeft = 0.f;             // seconds until the strafe may change direction
        std::array<float, kMaxArenaMoves> cooldown{};
        bool readRight = false;             // this move was read right: the partner's counter is due
        float holdFor = -1.f;               // a host's hold: seconds Open or Down lasts (not the kind's); < 0: none
        HoldAsk holdAsk = HoldAsk::None;    // the host's request waiting for the next tick ...
        ArenaBrain holdBrain = ArenaBrain::Open; // ... and the hold it asks for
        float holdSeconds = 0.f;
        int hpFloor = 0;                    // the host's hp floor (0: none)
        std::uint32_t moveMask = 0xFFFFFFFFu; // the moves a pick may begin (bit m: move m)
        bool facingHeld = false;            // the host's hold: a move keeps the facing it began with
        bool turnHeld = false;              // the host's hold: Approach and Strafe keep the facing (the host turns it)
        bool flinchAllowed = true;          // the host's: false, it never flinches
        float stopUntil = -1.0e9f;          // fight time a hit pause ends
        std::uint8_t breakHits = 0;         // the player's blows in a row toward a break-out
        std::uint8_t flinchChain = 0;       // flinches in a row (see Flinch)
        float lastFlinch = -1.0e9f;         // fight time of the last blow that could flinch it
        float lastBreakHit = -1.0e9f;       // fight time of the last of them
        int hostOwed = 0;                   // hostBlow damage since the last tick, not yet reported
        std::uint8_t hostMove = 0;          // the last of those blows' move
        bool hostKilled = false;            // one of them took the last hp (its Downed not yet reported)
        float firstBlow = -1.f;             // fight time of the player's side's first blow on it (< 0: none yet)
        bool punishOpen = false;            // in a punish window (Staggered, Open or Down) ...
        bool punishHit = false;             // ... that a blow of the player's side has landed in
        bool hitAlly = false;               // this attack has connected with the ally
    };

    ArenaDef def_;
    ArenaRing ring_;
    std::uint32_t seed_ = 1;
    std::uint32_t rng_ = 1;                 // xorshift32 state: a copy or a restart replays the same numbers
    ArenaPhase phase_ = ArenaPhase::NotStarted;
    float time_ = 0.f;
    float heldTime_ = 0.f;                  // time_ less the player's hit pauses
    std::size_t enemyCount_ = 0;
    Vec2 playerStart_;
    Vec2 playerStartFacing_{1.f, 0.f};
    std::array<Vec2, kMaxArenaEnemies> enemyStart_{};
    std::array<Vec2, kMaxArenaEnemies> enemyStartFacing_{};
    PlayerState player_;
    std::array<EnemyState, kMaxArenaEnemies> enemy_{};
    ArenaOutput out_;
    ArenaStats stats_;
    std::uint32_t dropped_ = 0;
    bool autoShoot_ = false; // setAutopilotShoots
    ReadCards read_;                        // the open Read deal
    std::array<ReadCard, kMaxArenaEnemies * kMaxArenaMoves> readPool_{}; // the deal's pool (tells are literals)
    std::uint8_t readFoe_ = kArenaNobody;
    std::uint8_t readMove_ = 0;
    std::uint32_t readDeals_ = 0;           // deals since begin() / restart(): seeds the next deal
    ArenaBlowHook blowHook_ = nullptr;      // the host's (begin() and restart() keep it)
    bool sideDashes_ = false;               // setSideDashes (begin() and restart() keep it)
    // The blow hook is running: tick, begin and restart do nothing. A copy starts clear (its hook isn't running),
    // and assigning to a fight keeps its own.
    struct HookFlag {
        bool on = false;
        HookFlag() = default;
        HookFlag(const HookFlag&) {}
        HookFlag& operator=(const HookFlag&) { return *this; }
    };
    HookFlag inHook_;
    void* blowUser_ = nullptr;
    // Carrying (kept across begin and restart):
    int loadCap_ = kMaxLoad;
    bool carry_ = false;
    ArenaChairs chairs_;
    std::uint8_t healMax_ = 0;              // the charges the last begin or restart gave
    int allyMaxHp_ = 0;                     // 0: no ally
    float allyRadius_ = 0.4f;
    int allyHp_ = 0;
    Vec2 allyPos_;
    ArenaTableSeats tableSeats_;            // setTableSeats (begin and restart clear it)
};

// Moves `bodies` the way a host with no walls would: each body at its output velocity for dt, then
// overlapping bodies pushed apart (circles of their body radii; dead enemies don't block), then
// everything kept inside the ring. Tests, the balance check and headless runs use it in place of an
// engine. Deterministic; no allocation.
FTCORE_API void arenaStep(const ArenaFight& fight, ArenaBodies& bodies, float dt);

// ---- data ----

struct ArenaFightEntry {
    std::string scene;
    std::uint32_t beat = 0, step = 0;
    ArenaDef def; // its fighter is the "default" one; the host swaps in the lead's (ArenaTable::fighter)
};

struct ArenaTable {
    std::vector<std::string> fighterIds; // [0] is "default"
    std::vector<ArenaFighter> fighters;
    std::vector<std::string> enemyIds;
    std::vector<ArenaEnemyKind> enemies;
    ArenaDef fallback; // the "default" fight
    std::vector<ArenaFightEntry> entries;
    float bpm = 0.f;   // the file's "bpm" (0: none), also in every fight's def

    // The lead's kit, else the "default" one (no allocation).
    FTCORE_API const ArenaFighter& fighter(std::string_view leadId) const;
    // The entry for (scene, beat, step), or nullptr. No allocation.
    FTCORE_API const ArenaFightEntry* findEntry(std::string_view scene, std::uint32_t beat, std::uint32_t step) const;
    // The fight for (scene, beat, step): the entry's, else the default. No allocation.
    FTCORE_API const ArenaDef& find(std::string_view scene, std::uint32_t beat, std::uint32_t step) const;
};

// ---- log ----

// What the result line names besides the fight's own counts. Ids only, never text: the story step's scene id
// ("standalone" for a fight on its own), the zone and the fighter kit, and who played ("autoplay", "idle", "player").
struct ArenaLogIds {
    std::string_view scene = "standalone";
    std::uint32_t beat = 0, step = 0;
    std::string_view zone;
    std::string_view kit;
    std::string_view pilot;
    std::uint32_t seed = 0;
};

constexpr std::size_t kArenaResultLineMax = 512; // a buffer this size always holds the line (ids are cut to fit)

// The arena's one line per fight, as the beat screen logs its own (docs/BEAT_SCREEN.md, "Log lines"):
//   Arena: VICTORY at <scene> beat <b> step <s> after <t> s, zone <z>, kit <k>, enemies <id>+<id>, <pilot> seed <n>,
//   hp <h>/<max>, <x> hits, <y> taken, <b> blocks, <p> parries, <r> ripostes, <e> evades, <g> guard breaks,
//   <s> staggers, <h> heals, <l> releases, <o> overloads, <c>/<d> reads right, dealt <n>, took <n>, <r> restarts.
// (<s> staggers: the enemies' poise breaks, every enemy's.)
// DEFEAT for a lost fight, TIMEOUT for one still running. The enemies are named by their table kind (the fight's
// enemyKind), "built-in" for a slot with none. Writes at most cap-1 characters and a terminating zero into out and
// returns the length written; no allocation.
FTCORE_API std::size_t arenaResultLine(char* out, std::size_t cap, const ArenaFight& fight, const ArenaTable& table,
                                       const ArenaLogIds& ids);

// Lays a fighter object of arena.json's form (only the keys it lists) over `f`, the way a lead's row lies over the
// "default" one. Problems go to `errors` with `path` in front; unknown keys go to `warnings` (none if null). The
// Brief table (Brief.h) lays its kits over a lead's row with it.
FTCORE_API void arenaOverlayFighter(const JsonValue& j, const std::string& path, ArenaFighter& f,
                                    std::vector<std::string>& errors, std::vector<std::string>* warnings);
// On failure `out` is empty and *error lists each problem with its path, one per line.
FTCORE_API bool importArenaTable(std::string_view json, ArenaTable& out, std::string* error = nullptr,
                                 std::vector<std::string>* warnings = nullptr);

} // namespace ftcore
