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
// * Player actions. Idle/Move, Light, Heavy, Riposte, Dodge, Guard, Heal; and the ones that happen to
//   the player: Hitstun, GuardBreak, Downed.
//   - A new action may start from Idle/Move, from an attack once `cancel` of its recovery has passed,
//     from a dodge once `dodgeCancel` of it has passed, and from Guard at any time. An action pressed
//     while it can't start is buffered for `bufferTime` (only the latest press is kept) and starts as
//     soon as it can.
//   - Light: the chain's next attack when it starts during the previous light's recovery or within
//     `chainGap` after it, else the first. The chain wraps after lightCount.
//   - Every attack and the dodge cost stamina; an action needs stamina > 0 to start and may take it
//     to 0 (StaminaEmpty). Stamina refills at staminaRegen per second once staminaDelay has passed
//     since the last spend.
//   - Dodge: a roll of dodgeDistance over dodgeTime along the stick (straight back, away from the
//     facing, when the stick is idle). Invulnerable from dodgeInvulnStart to dodgeInvulnEnd: an enemy
//     attack whose hit test passes then does nothing and reports Evaded (once per attack).
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
//   - Heal: needs a charge; heals healAmount after healTime, then healRecovery; movement scaled by
//     healMoveScale. Hitstun before the heal applies cancels it and keeps the charge.
//   - Being hit: hp loses the damage; poise loses the poise damage, and at 0 the player goes into
//     Hitstun (cancelling the current action) and poise refills. Poise also refills after poiseDelay
//     with no poise damage. hp 0: Downed, then Defeat.
//   - Facing: locked, the player faces the lock target (and strafes); otherwise it turns toward the
//     stick at turnRate. An attack starts facing the lock target, or the stick when it is pushed,
//     then turns only at its `tracking`.
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
// * Lock-on: `lock` locks the living enemy within lockRange with the best score of how close it is to
//   the view direction and how near it is (the worker picks the weighting; nearest wins ties), or
//   lets go when already locked. lockSwitch moves the lock to the next living enemy in range to the
//   left (-1) or right (+1) of the current one, as seen from the player. The lock moves to the nearest
//   other living enemy in range when its target dies, and lets go beyond lockBreak.
// * The ring: every velocity Core outputs is cut so the body stays inside the ring (minus its body
//   radius); a dodge toward the edge slides along it.
// * Victory: every enemy Dead. Defeat: the player Downed. Then tick() does nothing but report the
//   phase. restart() puts everything back as begin() left it (the same seed, so the same fight).
//
// Data: Data/Battle/arena.json (ids and numbers only, no story text). Like encounters.json and
// qte.json it is keyed by scene id + gameplay beat index + plan step index, with a `default` fight:
//   { "version": 1,
//     "fighters": [ { "id": "default", "hp": 100, ...,
//                     "light": [ { "startup": 0.18, "active": 0.12, "recovery": 0.35, "range": 2.1,
//                                  "damage": 12, "stamina": 14 }, ... ],
//                     "heavy": { ... }, "riposte": { ... } } ],
//     "enemies": [ { "id": "brute", "hp": 90, "poise": 40, ...,
//                    "moves": [ { "startup": 0.7, "active": 0.2, "recovery": 0.8, "range": 2.4,
//                                 "damage": 18, "minRange": 0, "maxRange": 2.8, "chain": 1 }, ... ] } ],
//     "default": { "enemies": ["brute"], "ring": 12, "attackers": 1, "spawn": 7 },
//     "fights": [ { "scene": "SC-X", "beat": 4, "step": 0, "enemies": ["brute", "brute"], "attackers": 2 } ] }
//   Fighter, enemy, move and attack keys are the struct fields below, spelled the same; a missing
//   key keeps the struct's default, so entries list only what differs. A fighter's id is a lead id
//   or "default" (required); ArenaTable::fighter falls back to it. A move's attack keys sit in the
//   move object itself. `ring`, `attackers` and `spawn` are ArenaDef's ringRadius, attackers and
//   spawnDistance. 1..kMaxArenaEnemies enemies per fight, 1..kMaxArenaMoves moves per enemy,
//   1..kMaxLightChain lights; `chain` must name another move of the same enemy and no string may
//   loop. An unknown enemy id, a duplicate id or a duplicate (scene, beat, step) is an error; unknown
//   keys are warnings; numbers outside their ranges (the loader documents them) are errors.
//
// Deterministic for the same seed, inputs and dts. Loading allocates; nothing else does (fixed
// capacity, results by value or by reference to the fight's own storage).

#include "ftcore/Api.h"
#include "ftcore/Explore.h" // Vec2

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

inline constexpr std::size_t kMaxArenaEnemies = 6;
inline constexpr std::size_t kMaxArenaMoves = 6;   // per enemy
inline constexpr std::size_t kMaxLightChain = 4;
inline constexpr std::size_t kMaxArenaEvents = 32; // per tick; more are dropped (ArenaFight::droppedEvents)

// Actor ids in events: enemies are 0..kMaxArenaEnemies-1.
inline constexpr std::uint8_t kArenaPlayer = 0xFE;
inline constexpr std::uint8_t kArenaNobody = 0xFF;
// The player's move ids in events and ArenaPlayerOut::move: a light is its chain index.
inline constexpr std::uint8_t kArenaHeavyMove = 8;
inline constexpr std::uint8_t kArenaRiposteMove = 9;

// One attack, the player's or an enemy's.
struct ArenaAttack {
    float startup = 0.35f;    // before the active part (an enemy's telegraph)
    float active = 0.15f;     // the only part that can connect
    float recovery = 0.45f;
    float range = 2.0f;       // metres from the attacker's centre; the target's body radius is added
    float arcCos = 0.5f;      // cos of half the arc it sweeps: 0.5 = 120 degrees wide, -1 = all round
    int damage = 10;
    float poiseDamage = 10.f;
    float guardDamage = 15.f; // stamina a guard that blocks it loses
    float lunge = 0.f;        // metres the attacker moves forward over startup + active
    float tracking = 3.f;     // rad/s the attacker may still turn during startup
    float armor = 0.f;        // 0..1: share of incoming poise damage ignored during startup + active
    float stamina = 0.f;      // the player's cost; enemies have no stamina
    float cancel = 0.6f;      // the player's: share of recovery after which another action may start

    float total() const { return startup + active + recovery; }
};

// The player's kit. arena.json has one per lead id and a "default".
struct ArenaFighter {
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
    float riposteReach = 2.2f;

    std::uint8_t healCharges = 3;
    int healAmount = 45;
    float healTime = 0.9f;
    float healRecovery = 0.4f;
    float healMoveScale = 0.35f;

    float lockRange = 15.f;
    float lockBreak = 20.f;
};

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
    std::array<ArenaEnemyMove, kMaxArenaMoves> moves{};
    std::uint8_t moveCount = 1;
};

// One fight: the player's kit and the enemies (one kind per enemy; kinds may repeat).
struct ArenaDef {
    ArenaFighter fighter;
    std::array<ArenaEnemyKind, kMaxArenaEnemies> enemies{};
    std::uint8_t enemyCount = 1;
    float ringRadius = 12.f;   // the ring the host should build (arenaRingAt)
    float spawnDistance = 7.f; // enemies start this far in front of the player
    std::uint8_t attackers = 1;
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

// What the player pressed this frame. Edges (true on the frame of the press) except `guard`.
struct ArenaInput {
    Vec2 move;                  // the stick on the ground, already turned by the camera; length 0..1
    Vec2 view{1.f, 0.f};        // the camera's forward on the ground (unit): lock-on picks from it
    bool light = false;
    bool heavy = false;
    bool dodge = false;
    bool heal = false;
    bool guard = false;         // held
    bool lock = false;          // lock on, or let go
    std::int8_t lockSwitch = 0; // -1 / +1: the next enemy to the left / right
};

// Where the bodies stand, as the host moved them (enemies beyond enemyCount() are ignored).
struct ArenaBodies {
    Vec2 player;
    std::array<Vec2, kMaxArenaEnemies> enemies{};
};

// The player's action (append only: the Unreal side maps it to animations).
enum class ArenaAction : std::uint8_t {
    Idle, Move, Light, Heavy, Riposte, Dodge, Guard, Heal, Hitstun, GuardBreak, Downed
};
// An enemy's brain state (append only).
enum class ArenaBrain : std::uint8_t { Approach, Strafe, Attack, Staggered, Open, Down, Dead };
// Where an attack is.
enum class ArenaPart : std::uint8_t { None, Startup, Active, Recovery };

FTCORE_API const char* arenaActionName(ArenaAction a); // "Idle", "Move", ... Literals.
FTCORE_API const char* arenaBrainName(ArenaBrain b);

struct ArenaPlayerOut {
    Vec2 velocity;                    // m/s this frame
    Vec2 facing{1.f, 0.f};            // unit
    ArenaAction action = ArenaAction::Idle;
    ArenaPart part = ArenaPart::None; // Light, Heavy, Riposte
    std::uint8_t move = 0;            // Light: the chain index; kArenaHeavyMove; kArenaRiposteMove
    float actionTime = 0.f;           // seconds since the action started
    bool invulnerable = false;        // dodge i-frames or a riposte
    bool parrying = false;            // inside a parry window
    std::uint8_t lock = kArenaNobody; // the locked enemy
};

struct ArenaEnemyOut {
    Vec2 velocity;
    Vec2 facing{1.f, 0.f};
    ArenaBrain brain = ArenaBrain::Approach;
    ArenaPart part = ArenaPart::None; // Attack
    std::uint8_t move = 0;            // Attack: the move index
    float stateTime = 0.f;            // seconds in this brain state (Attack: in this move)
    bool token = false;               // holds an attack token
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
    int guardBreaks = 0;
    int restarts = 0;
};

enum class ArenaPhase : std::uint8_t { NotStarted, Fighting, Victory, Defeat };

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
    // enemy, and heals below 40% hp when nothing is near or winding up. It must win every fight in
    // arena.json within 120 s of fight time, and with no input every fight must be lost within 120 s
    // (the tests tick at a fixed 1/60 s and move the bodies with arenaStep).
    FTCORE_API ArenaInput autopilot() const;

    FTCORE_API ArenaPhase phase() const;
    bool over() const {
        const ArenaPhase p = phase();
        return p == ArenaPhase::Victory || p == ArenaPhase::Defeat;
    }
    FTCORE_API float time() const; // seconds since begin() / restart()
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
    FTCORE_API std::uint8_t lock() const; // kArenaNobody when nothing is locked
    FTCORE_API int enemyHp(std::size_t i) const;
    FTCORE_API float enemyPoise(std::size_t i) const;
    FTCORE_API bool enemyAlive(std::size_t i) const;

private:
    // The fight's state: the implementer's, fixed capacity only (copying a fight never allocates).
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

    // The lead's kit, else the "default" one (no allocation).
    FTCORE_API const ArenaFighter& fighter(std::string_view leadId) const;
    // The entry for (scene, beat, step), or nullptr. No allocation.
    FTCORE_API const ArenaFightEntry* findEntry(std::string_view scene, std::uint32_t beat, std::uint32_t step) const;
    // The fight for (scene, beat, step): the entry's, else the default. No allocation.
    FTCORE_API const ArenaDef& find(std::string_view scene, std::uint32_t beat, std::uint32_t step) const;
};

// On failure `out` is empty and *error lists each problem with its path, one per line.
FTCORE_API bool importArenaTable(std::string_view json, ArenaTable& out, std::string* error = nullptr,
                                 std::vector<std::string>* warnings = nullptr);

} // namespace ftcore
