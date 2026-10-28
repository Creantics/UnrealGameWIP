#include "ftcore/ArenaFight.h"

#include "SeedMix.h"
#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>

namespace ftcore {

namespace {

// Numbers that are not balance (Data/Battle/arena.json holds every balance number). The riposte's dot and the
// autopilot's 40%, 0.12 s are the header's; the rest are details of the implementation with no data to take them from.
constexpr float kArenaPi = 3.14159265358979f;
constexpr float kArenaEps = 1.0e-4f;       // lengths and times below this are zero: a stick at rest, two bodies in one place
constexpr float kArenaTie = 1.0e-5f;       // scores closer than this tie (the nearer enemy wins)
constexpr float kArenaRiposteDot = 0.5f;   // the riposte's enemy must be this far inside the facing
constexpr float kArenaLockViewWeight = 0.7f; // lock-on score = 0.7 * alignment with the view + 0.3 * nearness (1 - d / lockRange)
constexpr float kArenaLockNearWeight = 0.3f;
constexpr int kArenaAutoHealPercent = 40;  // the autopilot heals below this share of its hp ...
constexpr float kArenaAutoParryWindow = 0.12f; // ... and parries only with a window at least this long
constexpr float kArenaAutoStand = 0.85f;   // it closes in to this share of its reach before it swings
constexpr float kArenaParkOffset = 1.5f;   // follower spots: metres outside the ring ...
constexpr float kArenaParkSpacing = 2.0f;  // ... and metres between neighbours along its edge
constexpr float kArenaSpawnGapRadii = 4.0f; // enemy start points: neighbours this many body radii apart along the arc
constexpr int kArenaPushPasses = 3;        // arenaStep: push-apart passes

float arenaLength(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
Vec2 arenaSub(Vec2 a, Vec2 b) { return Vec2{a.x - b.x, a.y - b.y}; }
float arenaDistance(Vec2 a, Vec2 b) { return arenaLength(arenaSub(a, b)); }
float arenaDot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
float arenaCross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
// a + dir * s
Vec2 arenaMadd(Vec2 a, Vec2 dir, float s) { return Vec2{a.x + dir.x * s, a.y + dir.y * s}; }
Vec2 arenaScaled(Vec2 v, float s) { return Vec2{v.x * s, v.y * s}; }

// v at length 1, or `fallback` when v is (nearly) zero.
Vec2 arenaUnit(Vec2 v, Vec2 fallback) {
    const float length = arenaLength(v);
    if (length <= kArenaEps) return fallback;
    return Vec2{v.x / length, v.y / length};
}

Vec2 arenaClampLength(Vec2 v, float most) {
    const float length = arenaLength(v);
    if (length <= most || length <= 0.f) return v;
    return arenaScaled(v, most / length);
}

Vec2 arenaRotate(Vec2 v, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    return Vec2{v.x * c - v.y * s, v.x * s + v.y * c};
}

// `facing` turned toward `dir` (both unit) by at most maxAngle radians.
Vec2 arenaTurn(Vec2 facing, Vec2 dir, float maxAngle) {
    if (!(maxAngle > 0.f)) return facing;
    const float angle = std::atan2(arenaCross(facing, dir), arenaDot(facing, dir));
    if (std::fabs(angle) <= maxAngle) return dir;
    return arenaUnit(arenaRotate(facing, angle > 0.f ? maxAngle : -maxAngle), facing);
}

// An attack from `from` facing `facing` (unit) reaches a body of `radius` at `to` when that body's centre is within
// range + radius of the attacker's centre and inside the arc.
bool arenaReaches(Vec2 from, Vec2 facing, const ArenaAttack& a, Vec2 to, float radius) {
    const Vec2 d = arenaSub(to, from);
    const float distance = arenaLength(d);
    if (distance > a.range + radius) return false;
    if (distance <= kArenaEps) return true;
    const float dot = std::clamp(arenaDot(facing, d) / distance, -1.f, 1.f);
    return dot >= a.arcCos;
}

// The velocity (m/s, for dt seconds) that keeps a body of `radius` at `pos` inside the ring: what the body wanted,
// cut where it would cross the edge (the radial overshoot goes, so a move along the edge slides). A body already
// out of the ring may not go further out. With dt <= 0 nothing moves and the velocity is left as it was.
Vec2 arenaCutVelocity(const ArenaRing& ring, Vec2 pos, Vec2 v, float dt, float radius) {
    if (!(dt > 0.f)) return v;
    ArenaRing reach;
    reach.centre = ring.centre;
    reach.radius = std::max(std::max(ring.radius - radius, 0.f), arenaDistance(pos, ring.centre));
    const Vec2 want = arenaMadd(pos, v, dt);
    if (reach.contains(want)) return v;
    const Vec2 got = reach.clamp(want);
    return Vec2{(got.x - pos.x) / dt, (got.y - pos.y) / dt};
}

// `current` moved toward `want` by at most rate * dt, as a vector, so a turn is braked and accelerated as one. A rate
// of 0 or less snaps.
Vec2 arenaRamp(Vec2 current, Vec2 want, float rate, float dt) {
    if (!(rate > 0.f)) return want;
    const Vec2 delta = arenaSub(want, current);
    const float length = arenaLength(delta);
    const float most = rate * dt;
    if (length <= most) return want;
    return arenaMadd(current, delta, most / length);
}

std::uint32_t arenaNextBits(std::uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
float arenaRandom(std::uint32_t& s) { return static_cast<float>(arenaNextBits(s) >> 8) * (1.f / 16777216.f); } // [0, 1)

ArenaPart arenaPartOf(const ArenaAttack& a, float time) {
    if (time < a.startup) return ArenaPart::Startup;
    if (time < a.startup + a.active) return ArenaPart::Active;
    return ArenaPart::Recovery;
}

} // namespace

// The fight's rules. ArenaFight's state is private; this is its friend, so the rules read and write it directly.
struct ArenaFightImpl {
    using Player = ArenaFight::PlayerState;
    using Enemy = ArenaFight::EnemyState;
    using Press = ArenaFight::Press;

    struct Pick {
        int move = -1;     // the move to start, or -1
        float goal = 0.f;  // none fits: the distance to walk to
        bool hold = false; // no move is available at all (every one cooling down): wait where it stands
    };

    ArenaFight& fight;
    Player& pl;
    std::array<Enemy, kMaxArenaEnemies>& foes;
    const ArenaDef& def;
    const ArenaFighter& fi;
    ArenaOutput& out;
    ArenaStats& stats;
    const std::size_t n;
    float dt = 0.f;
    float t0 = 0.f; // the tick's start, in fight time
    float t1 = 0.f; // ... and its end

    explicit ArenaFightImpl(ArenaFight& f)
        : fight(f), pl(f.player_), foes(f.enemy_), def(f.def_), fi(f.def_.fighter), out(f.out_), stats(f.stats_),
          n(f.enemyCount_) {}

    // ---- small things ----

    static std::uint8_t id(std::size_t i) { return static_cast<std::uint8_t>(i); }
    bool alive(std::size_t i) const { return i < n && foes[i].brain != ArenaBrain::Dead; }

    bool allDead() const {
        for (std::size_t i = 0; i < n; ++i)
            if (foes[i].brain != ArenaBrain::Dead) return false;
        return true;
    }

    void emit(ArenaEventType type, std::uint8_t source, std::uint8_t target, std::uint8_t move, int amount, Vec2 at) {
        if (out.events.count >= kMaxArenaEvents) {
            ++fight.dropped_;
            return;
        }
        ArenaEvent& ev = out.events.items[out.events.count++];
        ev.type = type;
        ev.source = source;
        ev.target = target;
        ev.move = move;
        ev.amount = amount;
        ev.at = at;
    }

    // The attack the player's current action performs, or nullptr.
    static const ArenaAttack* attackOf(const Player& p, const ArenaFighter& f) {
        switch (p.action) {
        case ArenaAction::Light: return &f.light[std::min<std::size_t>(p.move, f.lightCount - 1u)];
        case ArenaAction::Heavy: return &f.heavy;
        case ArenaAction::Riposte: return &f.riposte;
        default: return nullptr;
        }
    }

    // A new action may start from Idle/Move, from an attack once `cancel` of its recovery has passed, from a dodge once
    // `dodgeCancel` of it has passed, and from Guard at any time.
    static bool canStart(const Player& p, const ArenaFighter& f) {
        switch (p.action) {
        case ArenaAction::Idle:
        case ArenaAction::Move:
        case ArenaAction::Guard:
            return true;
        case ArenaAction::Light:
        case ArenaAction::Heavy:
        case ArenaAction::Riposte: {
            const ArenaAttack& a = *attackOf(p, f);
            return p.actionTime >= a.startup + a.active + a.cancel * a.recovery;
        }
        case ArenaAction::Dodge:
            return p.actionTime >= f.dodgeCancel * f.dodgeTime;
        default:
            return false;
        }
    }

    // ---- begin / restart ----

    void reset() {
        fight.rng_ = detail::mixSeed(fight.seed_);
        fight.phase_ = ArenaPhase::Fighting;
        fight.time_ = 0.f;
        pl = Player{};
        pl.pos = fight.playerStart_;
        pl.facing = fight.playerStartFacing_;
        pl.hp = std::max(fi.hp, 1);
        pl.stamina = fi.stamina;
        pl.poise = fi.poise;
        pl.charges = fi.healCharges;
        for (std::size_t i = 0; i < kMaxArenaEnemies; ++i) {
            Enemy& foe = foes[i];
            foe = Enemy{};
            if (i >= n) {
                foe.brain = ArenaBrain::Dead;
                continue;
            }
            foe.pos = fight.enemyStart_[i];
            foe.facing = fight.enemyStartFacing_[i];
            foe.hp = std::max(def.enemies[i].hp, 1);
            foe.poise = def.enemies[i].poise;
        }
        out = ArenaOutput{};
        fillOutput();
    }

    // ---- the output ----

    void fillOutput() {
        ArenaPlayerOut& po = out.player;
        po.velocity = pl.velocity;
        po.facing = pl.facing;
        po.action = pl.action;
        po.part = ArenaPart::None;
        po.move = 0;
        po.actionTime = (pl.action == ArenaAction::Idle || pl.action == ArenaAction::Move) ? 0.f : pl.actionTime;
        if (const ArenaAttack* a = attackOf(pl, fi)) {
            po.part = arenaPartOf(*a, pl.actionTime);
            po.move = pl.move;
        }
        po.invulnerable = pl.action == ArenaAction::Riposte ||
                          (pl.action == ArenaAction::Dodge && pl.actionTime >= fi.dodgeInvulnStart &&
                           pl.actionTime < fi.dodgeInvulnEnd);
        po.parrying = pl.action == ArenaAction::Guard && pl.parryLeft > 0.f;
        po.lock = pl.lock;
        po.dodgeDir = pl.dodgeDir;
        po.backstep = pl.backstep;
        for (std::size_t i = 0; i < kMaxArenaEnemies; ++i) {
            ArenaEnemyOut& eo = out.enemies[i];
            const Enemy& foe = foes[i];
            eo.velocity = foe.velocity;
            eo.facing = foe.facing;
            eo.brain = i < n ? foe.brain : ArenaBrain::Dead;
            eo.part = ArenaPart::None;
            eo.move = 0;
            eo.stateTime = foe.stateTime;
            eo.token = foe.token;
            if (i < n && foe.brain == ArenaBrain::Attack) {
                eo.part = arenaPartOf(def.enemies[i].moves[foe.move].attack, foe.stateTime);
                eo.move = foe.move;
            }
        }
    }

    // ---- one tick ----

    void tick(float step, const ArenaInput& in, const ArenaBodies& bodies) {
        out.events.count = 0;
        if (fight.phase_ == ArenaPhase::NotStarted) return;
        if (fight.phase_ != ArenaPhase::Fighting) { // over: nothing moves and nothing happens
            pl.velocity = Vec2{};
            for (Enemy& foe : foes) foe.velocity = Vec2{};
            fillOutput();
            return;
        }
        pl.pos = bodies.player;
        for (std::size_t i = 0; i < n; ++i) foes[i].pos = bodies.enemies[i];
        if (!(step > 0.f)) { // only refresh
            fillOutput();
            return;
        }
        dt = step;
        t0 = fight.time_;
        t1 = t0 + dt;
        stepPlayer(in);
        stepEnemies();
        endTick();
    }

    void endTick() {
        maintainLock();
        pl.parryLeft = std::max(0.f, pl.parryLeft - dt);
        if (pl.buffered != Press::None) pl.bufferAge += dt;
        // Every velocity is cut so the body stays inside the ring.
        pl.velocity = pl.hp > 0 ? arenaCutVelocity(fight.ring_, pl.pos, pl.velocity, dt, fi.bodyRadius) : Vec2{};
        for (std::size_t i = 0; i < n; ++i) {
            Enemy& foe = foes[i];
            foe.velocity = foe.brain == ArenaBrain::Dead
                               ? Vec2{}
                               : arenaCutVelocity(fight.ring_, foe.pos, foe.velocity, dt, def.enemies[i].bodyRadius);
        }
        fight.time_ = t1;
        stats.time += dt;
        if (pl.hp <= 0) {
            fight.phase_ = ArenaPhase::Defeat;
            emit(ArenaEventType::Defeat, kArenaNobody, kArenaPlayer, 0, 0, pl.pos);
        } else if (allDead()) {
            fight.phase_ = ArenaPhase::Victory;
            emit(ArenaEventType::Victory, kArenaNobody, kArenaNobody, 0, 0, pl.pos);
        }
        fillOutput();
    }

    // ---- lock-on ----

    void setLock(std::uint8_t target) {
        if (pl.lock == target) return;
        pl.lock = target;
        emit(ArenaEventType::LockChanged, kArenaPlayer, target, 0, 0, target == kArenaNobody ? pl.pos : foes[target].pos);
    }

    // The living enemy within lockRange with the best score for `view`; nearest wins ties.
    std::uint8_t bestLock(Vec2 view) const {
        std::uint8_t best = kArenaNobody;
        float bestScore = 0.f, bestDist = 0.f;
        const float range = std::max(fi.lockRange, kArenaEps);
        for (std::size_t i = 0; i < n; ++i) {
            if (!alive(i)) continue;
            const Vec2 d = arenaSub(foes[i].pos, pl.pos);
            const float distance = arenaLength(d);
            if (distance > fi.lockRange) continue;
            const float score = kArenaLockViewWeight * arenaDot(view, arenaUnit(d, view)) +
                                kArenaLockNearWeight * (1.f - distance / range);
            const bool better = best == kArenaNobody || score > bestScore + kArenaTie ||
                                (score >= bestScore - kArenaTie && distance < bestDist);
            if (better) {
                best = id(i);
                bestScore = score;
                bestDist = distance;
            }
        }
        return best;
    }

    // The next living enemy in range round the player from the locked one, to the right (or the left): the
    // smallest turn that way, wrapping round when nothing lies that way. Unreal's ground plane has x forward and
    // y right, so a positive cross(from, to) is a turn to the right.
    std::uint8_t switchLock(bool right) const {
        const std::size_t current = pl.lock;
        const Vec2 base = arenaUnit(arenaSub(foes[current].pos, pl.pos), pl.facing);
        std::uint8_t best = pl.lock;
        float bestTurn = 0.f, bestDist = 0.f;
        bool have = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (!alive(i) || i == current) continue;
            const Vec2 d = arenaSub(foes[i].pos, pl.pos);
            const float distance = arenaLength(d);
            if (distance > fi.lockRange) continue;
            const Vec2 dir = arenaUnit(d, base);
            float turn = std::atan2(arenaCross(base, dir), arenaDot(base, dir));
            if (!right) turn = -turn;
            if (turn <= 0.f) turn += 2.f * kArenaPi;
            if (!have || turn < bestTurn - kArenaTie || (turn <= bestTurn + kArenaTie && distance < bestDist)) {
                have = true;
                best = id(i);
                bestTurn = turn;
                bestDist = distance;
            }
        }
        return best;
    }

    void handleLock(const ArenaInput& in) {
        if (in.lock) {
            if (pl.lock != kArenaNobody) setLock(kArenaNobody);
            else setLock(bestLock(arenaUnit(in.view, pl.facing)));
        }
        if (in.lockSwitch != 0 && pl.lock != kArenaNobody) setLock(switchLock(in.lockSwitch > 0));
    }

    // The target died: the lock moves to the nearest other living enemy in range, else lets go. Beyond lockBreak it lets go.
    void maintainLock() {
        if (pl.lock == kArenaNobody) return;
        const std::size_t current = pl.lock;
        if (!alive(current)) {
            std::uint8_t best = kArenaNobody;
            float bestDist = 0.f;
            for (std::size_t i = 0; i < n; ++i) {
                if (!alive(i)) continue;
                const float distance = arenaDistance(pl.pos, foes[i].pos);
                if (distance > fi.lockRange) continue;
                if (best == kArenaNobody || distance < bestDist) {
                    best = id(i);
                    bestDist = distance;
                }
            }
            setLock(best);
        } else if (arenaDistance(pl.pos, foes[current].pos) > fi.lockBreak) {
            setLock(kArenaNobody);
        }
    }

    // ---- the player ----

    void regenerate() {
        // Stamina refills at staminaRegen per second once staminaDelay has passed since the last spend.
        const float from = std::max(t0, pl.lastSpend + fi.staminaDelay);
        if (t1 > from && pl.stamina < fi.stamina)
            pl.stamina = std::min(fi.stamina, pl.stamina + fi.staminaRegen * (t1 - from));
        // Poise refills after poiseDelay with no poise damage.
        if (t1 - pl.lastPoiseHit >= fi.poiseDelay) pl.poise = fi.poise;
    }

    void endAction() {
        pl.action = ArenaAction::Idle;
        pl.actionTime = 0.f;
    }

    void spend(float cost) {
        if (cost <= 0.f) return;
        const bool had = pl.stamina > 0.f;
        pl.stamina = std::max(0.f, pl.stamina - cost);
        pl.lastSpend = t0;
        if (had && pl.stamina <= 0.f) emit(ArenaEventType::StaminaEmpty, kArenaPlayer, kArenaNobody, 0, 0, pl.pos);
    }

    // An attack starts facing the lock target, or the stick when it is pushed.
    void faceAtStart(const ArenaInput& in) {
        if (pl.lock != kArenaNobody) {
            pl.facing = arenaUnit(arenaSub(foes[pl.lock].pos, pl.pos), pl.facing);
            return;
        }
        const Vec2 stick = arenaClampLength(in.move, 1.f);
        if (arenaLength(stick) > kArenaEps) pl.facing = arenaUnit(stick, pl.facing);
    }

    // The Open enemy within riposteReach and in front of the player that a light press performs the riposte on, or -1.
    int riposteTarget() const {
        int best = -1;
        float bestDist = 0.f;
        for (std::size_t i = 0; i < n; ++i) {
            if (foes[i].brain != ArenaBrain::Open) continue;
            const Vec2 d = arenaSub(foes[i].pos, pl.pos);
            const float distance = arenaLength(d);
            if (distance > fi.riposteReach) continue;
            if (distance > kArenaEps && arenaDot(pl.facing, d) / distance < kArenaRiposteDot) continue;
            if (best < 0 || distance < bestDist) {
                best = static_cast<int>(i);
                bestDist = distance;
            }
        }
        return best;
    }

    void beginAttack(ArenaAction kind, std::uint8_t move, const ArenaAttack& a) {
        spend(a.stamina);
        pl.action = kind;
        pl.move = move;
        pl.actionTime = 0.f;
        pl.hitMask = 0;
        emit(ArenaEventType::AttackStarted, kArenaPlayer, kArenaNobody, move, 0, pl.pos);
    }

    bool startLight(const ArenaInput& in) {
        if (pl.stamina <= 0.f) return false;
        const int riposteAt = riposteTarget();
        if (riposteAt >= 0) {
            pl.chainLive = false;
            pl.target = id(static_cast<std::size_t>(riposteAt));
            pl.facing = arenaUnit(arenaSub(foes[static_cast<std::size_t>(riposteAt)].pos, pl.pos), pl.facing);
            beginAttack(ArenaAction::Riposte, kArenaRiposteMove, fi.riposte);
            return true;
        }
        // The chain's next attack when it starts during the previous light's recovery or within chainGap after it.
        const bool chained = pl.chainLive && (pl.action == ArenaAction::Light || t0 - pl.lightEnd <= fi.chainGap);
        const std::uint8_t index = chained ? pl.chainNext : static_cast<std::uint8_t>(0);
        pl.chainNext = static_cast<std::uint8_t>((index + 1u) % fi.lightCount);
        pl.chainLive = true;
        faceAtStart(in);
        beginAttack(ArenaAction::Light, index, fi.light[index]);
        return true;
    }

    bool startHeavy(const ArenaInput& in) {
        if (pl.stamina <= 0.f) return false;
        pl.chainLive = false;
        faceAtStart(in);
        beginAttack(ArenaAction::Heavy, kArenaHeavyMove, fi.heavy);
        return true;
    }

    bool startDodge(const ArenaInput& in) {
        if (pl.stamina <= 0.f) return false;
        const Vec2 stick = arenaClampLength(in.move, 1.f);
        if (arenaLength(stick) > kArenaEps) {
            pl.dodgeDir = arenaUnit(stick, pl.facing);
            pl.facing = pl.dodgeDir;
            pl.backstep = false;
        } else {
            pl.dodgeDir = Vec2{-pl.facing.x, -pl.facing.y}; // straight back
            pl.backstep = true;
        }
        spend(fi.dodgeStamina);
        pl.chainLive = false;
        pl.action = ArenaAction::Dodge;
        pl.actionTime = 0.f;
        return true;
    }

    bool startHeal() {
        if (pl.charges == 0) return false;
        pl.chainLive = false;
        pl.action = ArenaAction::Heal;
        pl.actionTime = 0.f;
        pl.healApplied = false;
        return true;
    }

    void startGuard() {
        pl.chainLive = false;
        pl.action = ArenaAction::Guard;
        pl.actionTime = 0.f;
    }

    bool startAction(Press what, const ArenaInput& in) {
        switch (what) {
        case Press::Light: return startLight(in);
        case Press::Heavy: return startHeavy(in);
        case Press::Dodge: return startDodge(in);
        case Press::Heal: return startHeal();
        default: return false;
        }
    }

    void stepPlayer(const ArenaInput& in) {
        regenerate();
        handleLock(in);

        // The guard flag going up is a press; it opens the parry window unless the last press was less than
        // parryRearm ago. Every press counts, so mashing guard opens nothing.
        if (in.guard && !pl.guardHeld) {
            if (t0 - pl.lastGuardPress >= fi.parryRearm) pl.parryLeft = fi.parryWindow;
            pl.lastGuardPress = t0;
        }
        if (!in.guard) pl.parryLeft = 0.f;
        pl.guardHeld = in.guard;
        if (pl.action == ArenaAction::Guard && !in.guard) endAction();

        // The input buffer: only the latest press is kept, for bufferTime. Pressed in one tick, a defensive action
        // beats an attack.
        Press press = Press::None;
        if (in.light) press = Press::Light;
        if (in.heavy) press = Press::Heavy;
        if (in.heal) press = Press::Heal;
        if (in.dodge) press = Press::Dodge;
        if (press != Press::None) {
            pl.buffered = press;
            pl.bufferAge = 0.f;
        } else if (pl.bufferAge > fi.bufferTime) {
            pl.buffered = Press::None;
        }

        bool began = false;
        if (pl.buffered != Press::None && canStart(pl, fi) && startAction(pl.buffered, in)) {
            pl.buffered = Press::None;
            began = true;
        }
        if (!began && in.guard && pl.action != ArenaAction::Guard && canStart(pl, fi)) startGuard();

        advancePlayer(in);
    }

    // Outside attacks the player faces the lock target, else turns toward the stick at turnRate.
    void faceFree(Vec2 stick, bool pushed) {
        if (pl.lock != kArenaNobody) pl.facing = arenaUnit(arenaSub(foes[pl.lock].pos, pl.pos), pl.facing);
        else if (pushed) pl.facing = arenaTurn(pl.facing, arenaUnit(stick, pl.facing), fi.turnRate * dt);
    }

    // The velocity of a state that ramps (Idle, Move, Guard, Heal): from where it is now toward `want`, by moveAccel
    // while the wanted speed is above the current one and by moveBrake otherwise.
    Vec2 ramped(Vec2 want) const {
        const float rate = arenaLength(want) > arenaLength(pl.velocity) ? fi.moveAccel : fi.moveBrake;
        return arenaRamp(pl.velocity, want, rate, dt);
    }

    void advancePlayer(const ArenaInput& in) {
        const float a0 = pl.actionTime;
        const float a1 = a0 + dt;
        const Vec2 stick = arenaClampLength(in.move, 1.f);
        const bool pushed = arenaLength(stick) > kArenaEps;
        Vec2 vel{};
        pl.invulnerable = false;
        switch (pl.action) {
        case ArenaAction::Idle:
        case ArenaAction::Move:
            faceFree(stick, pushed);
            vel = ramped(arenaScaled(stick, fi.moveSpeed));
            pl.action = pushed ? ArenaAction::Move : ArenaAction::Idle;
            break;
        case ArenaAction::Guard:
            faceFree(stick, pushed);
            vel = ramped(arenaScaled(stick, fi.moveSpeed * fi.guardMoveScale));
            pl.actionTime = a1;
            break;
        case ArenaAction::Light:
        case ArenaAction::Heavy:
        case ArenaAction::Riposte:
            vel = advanceAttack(stick, pushed, a0, a1);
            break;
        case ArenaAction::Dodge: {
            // A roll of dodgeDistance over dodgeTime; invulnerable from dodgeInvulnStart to dodgeInvulnEnd.
            const float length = std::max(fi.dodgeTime, kArenaEps);
            pl.invulnerable = fi.dodgeInvulnEnd > fi.dodgeInvulnStart && a0 < fi.dodgeInvulnEnd && a1 > fi.dodgeInvulnStart;
            const float moving = std::max(0.f, std::min(a1, length) - a0);
            vel = arenaScaled(pl.dodgeDir, fi.dodgeDistance / length * moving / dt);
            pl.actionTime = a1;
            if (a1 >= length) endAction();
            break;
        }
        case ArenaAction::Heal:
            faceFree(stick, pushed);
            vel = ramped(arenaScaled(stick, fi.moveSpeed * fi.healMoveScale));
            if (!pl.healApplied && a1 >= fi.healTime) {
                // The charge goes when the heal applies: a hitstun before that keeps it.
                pl.healApplied = true;
                if (pl.charges > 0) --pl.charges;
                const int before = pl.hp;
                pl.hp = std::min(std::max(fi.hp, 1), pl.hp + fi.healAmount);
                ++stats.heals;
                emit(ArenaEventType::Healed, kArenaPlayer, kArenaPlayer, 0, pl.hp - before, pl.pos);
            }
            pl.actionTime = a1;
            if (a1 >= fi.healTime + fi.healRecovery) endAction();
            break;
        case ArenaAction::Hitstun:
            pl.actionTime = a1;
            if (a1 >= fi.hitstun) endAction();
            break;
        case ArenaAction::GuardBreak:
            pl.actionTime = a1;
            if (a1 >= fi.guardBreakTime) endAction();
            break;
        case ArenaAction::Downed:
            break;
        }
        pl.velocity = vel;
    }

    Vec2 advanceAttack(Vec2 stick, bool pushed, float a0, float a1) {
        const ArenaAttack& a = *attackOf(pl, fi);
        // During startup the attacker may still turn (at `tracking` rad/s) toward its target; after it, never.
        if (a0 < a.startup) {
            Vec2 want = pl.facing;
            bool have = false;
            if (pl.action == ArenaAction::Riposte && pl.target < n) {
                want = arenaUnit(arenaSub(foes[pl.target].pos, pl.pos), pl.facing);
                have = true;
            } else if (pl.lock != kArenaNobody) {
                want = arenaUnit(arenaSub(foes[pl.lock].pos, pl.pos), pl.facing);
                have = true;
            } else if (pushed) {
                want = arenaUnit(stick, pl.facing);
                have = true;
            }
            if (have) pl.facing = arenaTurn(pl.facing, want, a.tracking * (std::min(a1, a.startup) - a0));
        }
        pl.invulnerable = pl.action == ArenaAction::Riposte;
        // Only the active part connects; a tick whose interval overlaps it tests, so a long frame can't skip it.
        if (a1 > a.startup && a0 < a.startup + a.active) playerStrike(a);
        // The lunge: `lunge` metres forward, spread over startup and active.
        Vec2 vel{};
        const float span = a.startup + a.active;
        if (span > kArenaEps && a.lunge != 0.f) {
            const float moving = std::max(0.f, std::min(a1, span) - a0);
            vel = arenaScaled(pl.facing, a.lunge / span * moving / dt);
        }
        pl.actionTime = a1;
        if (a1 >= a.total()) {
            if (pl.action == ArenaAction::Light) pl.lightEnd = t1;
            endAction();
        }
        return vel;
    }

    // ---- the player's attacks on the enemies ----

    void killEnemy(std::size_t i) {
        Enemy& foe = foes[i];
        foe.hp = 0;
        foe.brain = ArenaBrain::Dead;
        foe.stateTime = 0.f;
        foe.velocity = Vec2{};
        releaseToken(i);
        emit(ArenaEventType::Downed, kArenaPlayer, id(i), 0, 0, foe.pos);
    }

    void staggerEnemy(std::size_t i) {
        Enemy& foe = foes[i];
        foe.poise = def.enemies[i].poise;
        interrupt(i, ArenaBrain::Staggered);
        ++stats.staggers;
        emit(ArenaEventType::Staggered, kArenaPlayer, id(i), 0, 0, foe.pos);
    }

    void playerStrike(const ArenaAttack& a) {
        const bool riposte = pl.action == ArenaAction::Riposte;
        // The enemies this tick reaches that the attack hasn't hit yet.
        std::array<std::size_t, kMaxArenaEnemies> reached{};
        std::size_t count = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (foes[i].brain == ArenaBrain::Dead || (pl.hitMask & (1u << i)) != 0u) continue;
            if (arenaReaches(pl.pos, pl.facing, a, foes[i].pos, def.enemies[i].bodyRadius)) reached[count++] = i;
        }
        // maxTargets: at most that many distinct targets over the whole active part; with more than the slots left on
        // one tick, the nearest win (ties by lower index). A riposte is aimed at its enemy, so that one comes first.
        if (a.maxTargets > 0) {
            const std::size_t most = a.maxTargets;
            std::size_t used = 0;
            for (std::size_t i = 0; i < n; ++i)
                if ((pl.hitMask & (1u << i)) != 0u) ++used;
            const std::size_t slots = used < most ? most - used : 0;
            if (count > slots) {
                const auto first = reached.begin();
                const std::size_t aimed = riposte ? static_cast<std::size_t>(pl.target) : kMaxArenaEnemies;
                std::sort(first, first + static_cast<std::ptrdiff_t>(count), [&](std::size_t x, std::size_t y) {
                    if ((x == aimed) != (y == aimed)) return x == aimed;
                    const float dx = arenaDistance(pl.pos, foes[x].pos), dy = arenaDistance(pl.pos, foes[y].pos);
                    return dx < dy || (dx == dy && x < y);
                });
                count = slots;
                std::sort(first, first + static_cast<std::ptrdiff_t>(count)); // the hits go in index order
            }
        }
        for (std::size_t k = 0; k < count; ++k) {
            const std::size_t i = reached[k];
            Enemy& foe = foes[i];
            pl.hitMask = static_cast<std::uint8_t>(pl.hitMask | (1u << i));
            const int amount = std::min(a.damage, foe.hp);
            foe.hp -= amount;
            ++stats.hits;
            stats.damageDealt += amount;
            emit(ArenaEventType::Hit, kArenaPlayer, id(i), pl.move, amount, foe.pos);
            if (foe.hp <= 0) {
                killEnemy(i);
                continue;
            }
            if (riposte && foe.brain == ArenaBrain::Open) { // the riposte punishes the open enemy
                foe.brain = ArenaBrain::Down;
                foe.stateTime = 0.f;
                ++stats.ripostes;
                continue;
            }
            // Poise: only an enemy on its feet loses it; `armor` ignores a share of it during an attack's startup
            // and active parts.
            const bool upright = foe.brain == ArenaBrain::Approach || foe.brain == ArenaBrain::Strafe ||
                                 foe.brain == ArenaBrain::Attack;
            if (a.poiseDamage <= 0.f || !upright) continue;
            float share = 0.f;
            if (foe.brain == ArenaBrain::Attack) {
                const ArenaAttack& own = def.enemies[i].moves[foe.move].attack;
                if (foe.stateTime < own.startup + own.active) share = std::clamp(own.armor, 0.f, 1.f);
            }
            foe.poise -= a.poiseDamage * (1.f - share);
            foe.lastPoiseHit = t1;
            if (foe.poise <= 0.f) staggerEnemy(i);
        }
    }

    // ---- the enemies ----

    void releaseToken(std::size_t i) {
        foes[i].token = false;
        foes[i].asked = 0.f;
    }

    // Leaves the current state for `brain` (Staggered, Open): the attack stops and the token goes back.
    void interrupt(std::size_t i, ArenaBrain brain) {
        Enemy& foe = foes[i];
        foe.brain = brain;
        foe.stateTime = 0.f;
        foe.stringEnd = t1;
        releaseToken(i);
    }

    void flipStrafe(std::size_t i) {
        Enemy& foe = foes[i];
        foe.strafeDir = arenaRandom(fight.rng_) < 0.5f ? static_cast<std::int8_t>(-1) : static_cast<std::int8_t>(1);
        // It circles for about an attackDelay before it may turn round.
        foe.strafeLeft = def.enemies[i].attackDelay * (0.5f + arenaRandom(fight.rng_));
    }

    void enterStrafe(std::size_t i) {
        foes[i].brain = ArenaBrain::Strafe;
        foes[i].stateTime = 0.f;
        flipStrafe(i);
    }

    void enemyTimers(std::size_t i) {
        Enemy& foe = foes[i];
        if (foe.brain == ArenaBrain::Dead) return;
        const ArenaEnemyKind& k = def.enemies[i];
        for (float& c : foe.cooldown) c = std::max(0.f, c - dt);
        if (t1 - foe.lastPoiseHit >= k.poiseDelay) foe.poise = k.poise;
    }

    // Approach (no token) ends at the preferred range; Strafe gives way to Approach when the player is beyond runRange.
    void enemyStance(std::size_t i) {
        Enemy& foe = foes[i];
        if (foe.token || (foe.brain != ArenaBrain::Approach && foe.brain != ArenaBrain::Strafe)) return;
        const ArenaEnemyKind& k = def.enemies[i];
        const float distance = arenaDistance(pl.pos, foe.pos);
        if (foe.brain == ArenaBrain::Approach && distance <= k.preferredRange) {
            enterStrafe(i);
        } else if (foe.brain == ArenaBrain::Strafe && distance > k.runRange) {
            foe.brain = ArenaBrain::Approach;
            foe.stateTime = 0.f;
        }
    }

    // A strafing enemy whose attackDelay has passed asks for a token; the longest waiting (then the nearest) gets it,
    // up to `attackers` held at once.
    void assignTokens() {
        std::size_t held = 0;
        std::array<bool, kMaxArenaEnemies> asking{};
        for (std::size_t i = 0; i < n; ++i) {
            Enemy& foe = foes[i];
            if (foe.brain == ArenaBrain::Dead) continue;
            if (foe.token) {
                ++held;
                continue;
            }
            asking[i] = foe.brain == ArenaBrain::Strafe && t0 - foe.stringEnd >= def.enemies[i].attackDelay;
            foe.asked = asking[i] ? foe.asked + dt : 0.f;
        }
        while (held < def.attackers) {
            int best = -1;
            float bestDist = 0.f;
            for (std::size_t i = 0; i < n; ++i) {
                if (!asking[i]) continue;
                const float distance = arenaDistance(pl.pos, foes[i].pos);
                const bool better = best < 0 || foes[i].asked > foes[static_cast<std::size_t>(best)].asked ||
                                    (foes[i].asked == foes[static_cast<std::size_t>(best)].asked && distance < bestDist);
                if (better) {
                    best = static_cast<int>(i);
                    bestDist = distance;
                }
            }
            if (best < 0) break;
            const std::size_t winner = static_cast<std::size_t>(best);
            asking[winner] = false;
            foes[winner].token = true;
            foes[winner].asked = 0.f;
            ++held;
        }
    }

    // With the token, a move whose [minRange, maxRange] holds the player's distance and whose cooldown is over is
    // picked, weighted by `weight`; when none fits, the nearest move's band is where to walk to.
    Pick pickMove(std::size_t i, float distance) {
        const Enemy& foe = foes[i];
        const ArenaEnemyKind& k = def.enemies[i];
        const auto gapOf = [&](const ArenaEnemyMove& mv) {
            return distance > mv.maxRange ? distance - mv.maxRange : (distance < mv.minRange ? mv.minRange - distance : 0.f);
        };
        const auto ready = [&](std::size_t m) { return k.moves[m].weight > 0.f && foe.cooldown[m] <= 0.f; };
        Pick pick;
        float total = 0.f;
        int nearest = -1;
        float nearestGap = 0.f;
        for (std::size_t m = 0; m < k.moveCount; ++m) {
            if (!ready(m)) continue;
            const float gap = gapOf(k.moves[m]);
            if (gap <= kArenaEps) total += k.moves[m].weight;
            if (nearest < 0 || gap < nearestGap) {
                nearest = static_cast<int>(m);
                nearestGap = gap;
            }
        }
        if (total > 0.f) {
            float roll = arenaRandom(fight.rng_) * total;
            for (std::size_t m = 0; m < k.moveCount; ++m) {
                if (!ready(m) || gapOf(k.moves[m]) > kArenaEps) continue;
                pick.move = static_cast<int>(m);
                if (roll < k.moves[m].weight) break;
                roll -= k.moves[m].weight;
            }
            return pick;
        }
        if (nearest < 0) {
            pick.hold = true;
            return pick;
        }
        const ArenaEnemyMove& mv = k.moves[static_cast<std::size_t>(nearest)];
        pick.goal = distance > mv.maxRange ? mv.maxRange : mv.minRange;
        return pick;
    }

    void beginMove(std::size_t i, std::uint8_t move) {
        Enemy& foe = foes[i];
        foe.brain = ArenaBrain::Attack;
        foe.move = move;
        foe.stateTime = 0.f;
        foe.hitPlayer = false;
        foe.evaded = false;
        foe.cooldown[move] = def.enemies[i].moves[move].cooldown;
        emit(ArenaEventType::AttackStarted, id(i), kArenaNobody, move, 0, foe.pos);
    }

    // The move ended: its `chain` move follows at once, else the string is over and the token goes back.
    void endMove(std::size_t i) {
        Enemy& foe = foes[i];
        const ArenaEnemyKind& k = def.enemies[i];
        const int chain = k.moves[foe.move].chain;
        if (chain >= 0 && chain < static_cast<int>(k.moveCount) && chain != static_cast<int>(foe.move)) {
            beginMove(i, static_cast<std::uint8_t>(chain));
            return;
        }
        releaseToken(i);
        foe.stringEnd = t1;
        foe.brain = ArenaBrain::Approach;
        foe.stateTime = 0.f;
    }

    // One of the enemy's attacks reaches the player.
    void enemyStrike(std::size_t i) {
        Enemy& foe = foes[i];
        const ArenaEnemyMove& mv = def.enemies[i].moves[foe.move];
        const ArenaAttack& a = mv.attack;
        const std::uint8_t source = id(i);
        // Invulnerable (dodge frames, a riposte): it passes through, and that is told once per attack.
        if (pl.invulnerable) {
            if (!foe.evaded) {
                foe.evaded = true;
                ++stats.evades;
                emit(ArenaEventType::Evaded, source, kArenaPlayer, foe.move, 0, pl.pos);
            }
            return;
        }
        const Vec2 toFoe = arenaUnit(arenaSub(foe.pos, pl.pos), pl.facing);
        const bool guarding = pl.action == ArenaAction::Guard && arenaDot(pl.facing, toFoe) >= fi.guardArcCos;
        foe.hitPlayer = true; // connected: parried, blocked or landed, it is spent
        if (guarding && pl.parryLeft > 0.f && mv.parryable) {
            // No damage, no stamina cost: the enemy is open.
            ++stats.parries;
            emit(ArenaEventType::Parried, source, kArenaPlayer, foe.move, 0, pl.pos);
            interrupt(i, ArenaBrain::Open);
            return;
        }
        if (guarding && !mv.unblockable) {
            const float before = pl.stamina;
            pl.stamina = std::max(0.f, before - a.guardDamage);
            pl.lastSpend = t0;
            ++stats.blocks;
            emit(ArenaEventType::Blocked, source, kArenaPlayer, foe.move,
                 static_cast<int>(std::lround(before - pl.stamina)), pl.pos);
            const int chip = std::max(0, static_cast<int>(std::lround(a.damage * (1.f - fi.guardAbsorb))));
            if (chip > 0) damagePlayer(source, chip, foe.move);
            if (pl.hp > 0 && pl.stamina <= 0.f) { // the block emptied the stamina: the guard breaks
                ++stats.guardBreaks;
                emit(ArenaEventType::GuardBroken, source, kArenaPlayer, foe.move, 0, pl.pos);
                pl.action = ArenaAction::GuardBreak;
                pl.actionTime = 0.f;
                pl.velocity = Vec2{};
                pl.parryLeft = 0.f;
            }
            return;
        }
        ++stats.hitsTaken;
        damagePlayer(source, a.damage, foe.move);
        if (pl.hp <= 0) return;
        // Poise: a player in hitstun or guard break is already down; `armor` ignores a share of it during the player's
        // own attack's startup and active parts.
        if (pl.action == ArenaAction::Hitstun || pl.action == ArenaAction::GuardBreak || a.poiseDamage <= 0.f) return;
        float share = 0.f;
        if (const ArenaAttack* own = attackOf(pl, fi)) {
            if (pl.actionTime < own->startup + own->active) share = std::clamp(own->armor, 0.f, 1.f);
        }
        pl.poise -= a.poiseDamage * (1.f - share);
        pl.lastPoiseHit = t1;
        if (pl.poise <= 0.f) { // the poise broke: hitstun cancels the current action (a heal keeps its charge)
            pl.poise = fi.poise;
            pl.chainLive = false;
            pl.action = ArenaAction::Hitstun;
            pl.actionTime = 0.f;
            pl.velocity = Vec2{};
            emit(ArenaEventType::Staggered, source, kArenaPlayer, foe.move, 0, pl.pos);
        }
    }

    void damagePlayer(std::uint8_t source, int damage, std::uint8_t move) {
        const int amount = std::min(damage, pl.hp);
        pl.hp -= amount;
        stats.damageTaken += amount;
        emit(ArenaEventType::Hit, source, kArenaPlayer, move, amount, pl.pos);
        if (pl.hp <= 0) {
            pl.hp = 0;
            pl.action = ArenaAction::Downed;
            pl.actionTime = 0.f;
            pl.velocity = Vec2{};
            pl.buffered = Press::None;
            pl.parryLeft = 0.f;
            emit(ArenaEventType::Downed, source, kArenaPlayer, move, 0, pl.pos);
        }
    }

    Vec2 enemyAttack(std::size_t i, Vec2 dir) {
        Enemy& foe = foes[i];
        const ArenaAttack& a = def.enemies[i].moves[foe.move].attack;
        const float a0 = foe.stateTime;
        const float a1 = a0 + dt;
        if (a0 < a.startup) foe.facing = arenaTurn(foe.facing, dir, a.tracking * (std::min(a1, a.startup) - a0));
        if (!foe.hitPlayer && pl.hp > 0 && a1 > a.startup && a0 < a.startup + a.active &&
            arenaReaches(foe.pos, foe.facing, a, pl.pos, fi.bodyRadius)) {
            enemyStrike(i);
            if (foe.brain != ArenaBrain::Attack) return Vec2{}; // parried: the attack is over
        }
        Vec2 vel{};
        const float span = a.startup + a.active;
        if (span > kArenaEps && a.lunge != 0.f) {
            const float moving = std::max(0.f, std::min(a1, span) - a0);
            vel = arenaScaled(foe.facing, a.lunge / span * moving / dt);
        }
        foe.stateTime = a1;
        if (a1 >= a.total()) endMove(i);
        return vel;
    }

    Vec2 approach(std::size_t i, Vec2 dir, float distance, const Pick& pick) {
        Enemy& foe = foes[i];
        const ArenaEnemyKind& k = def.enemies[i];
        foe.stateTime += dt;
        foe.facing = arenaTurn(foe.facing, dir, k.turnRate * dt);
        const float goal = foe.token ? pick.goal : k.preferredRange;
        const float error = distance - goal;
        if (std::fabs(error) <= kArenaEps) return Vec2{};
        const float speed = std::min(distance > k.runRange ? k.runSpeed : k.walkSpeed, std::fabs(error) / dt);
        return arenaScaled(dir, error > 0.f ? speed : -speed);
    }

    Vec2 strafe(std::size_t i, Vec2 dir) {
        Enemy& foe = foes[i];
        const ArenaEnemyKind& k = def.enemies[i];
        foe.stateTime += dt;
        foe.facing = arenaTurn(foe.facing, dir, k.turnRate * dt);
        foe.strafeLeft -= dt;
        if (foe.strafeLeft <= 0.f) flipStrafe(i);
        // Toward the point on the circle at the preferred range one step further round: the circling and a soft pull
        // back to the range in one, never faster than strafeSpeed.
        const float radius = std::max(k.preferredRange, kArenaEps);
        const Vec2 outward = arenaUnit(arenaSub(foe.pos, pl.pos), Vec2{-dir.x, -dir.y});
        const float step = static_cast<float>(foe.strafeDir) * k.strafeSpeed * dt / radius;
        const Vec2 target = arenaMadd(pl.pos, arenaRotate(outward, step), radius);
        const Vec2 want = arenaScaled(arenaSub(target, foe.pos), 1.f / dt);
        return arenaClampLength(want, k.strafeSpeed);
    }

    void recover(std::size_t i) {
        Enemy& foe = foes[i];
        const ArenaEnemyKind& k = def.enemies[i];
        const float length = foe.brain == ArenaBrain::Staggered ? k.staggerTime
                             : foe.brain == ArenaBrain::Open    ? k.openTime
                                                                : k.downTime;
        foe.stateTime += dt;
        if (foe.stateTime >= length) {
            foe.brain = ArenaBrain::Approach;
            foe.stateTime = 0.f;
        }
    }

    void enemyAct(std::size_t i) {
        Enemy& foe = foes[i];
        if (foe.brain == ArenaBrain::Dead) {
            foe.velocity = Vec2{};
            return;
        }
        const Vec2 toPlayer = arenaSub(pl.pos, foe.pos);
        const float distance = arenaLength(toPlayer);
        const Vec2 dir = arenaUnit(toPlayer, foe.facing);

        Pick pick;
        if (foe.token && (foe.brain == ArenaBrain::Approach || foe.brain == ArenaBrain::Strafe)) {
            pick = pickMove(i, distance);
            if (pick.move >= 0) {
                beginMove(i, static_cast<std::uint8_t>(pick.move));
            } else if (pick.hold) {
                if (foe.brain != ArenaBrain::Strafe) enterStrafe(i);
            } else if (foe.brain != ArenaBrain::Approach) {
                foe.brain = ArenaBrain::Approach;
                foe.stateTime = 0.f;
            }
        }
        // Approach and Strafe ramp (the kind's accel, both ways); a lunge and a reaction are written directly.
        Vec2 vel{};
        switch (foe.brain) {
        case ArenaBrain::Attack: vel = enemyAttack(i, dir); break;
        case ArenaBrain::Approach:
            vel = arenaRamp(foe.velocity, approach(i, dir, distance, pick), def.enemies[i].accel, dt);
            break;
        case ArenaBrain::Strafe: vel = arenaRamp(foe.velocity, strafe(i, dir), def.enemies[i].accel, dt); break;
        case ArenaBrain::Staggered:
        case ArenaBrain::Open:
        case ArenaBrain::Down: recover(i); break;
        case ArenaBrain::Dead: break;
        }
        foe.velocity = vel;
    }

    void stepEnemies() {
        for (std::size_t i = 0; i < n; ++i) enemyTimers(i);
        for (std::size_t i = 0; i < n; ++i) enemyStance(i);
        assignTokens();
        for (std::size_t i = 0; i < n; ++i) enemyAct(i);
    }

    // ---- the autopilot ----

    // The way to roll sideways out of an attack from `from`: the side with more room inside the ring.
    static Vec2 sideStep(const ArenaFight& self, Vec2 from) {
        const Player& p = self.player_;
        const Vec2 away = arenaUnit(arenaSub(p.pos, from), p.facing);
        const Vec2 side{-away.y, away.x};
        const float step = self.def_.fighter.dodgeDistance;
        const bool first = arenaDistance(arenaMadd(p.pos, side, step), self.ring_.centre) <=
                           arenaDistance(arenaMadd(p.pos, side, -step), self.ring_.centre);
        return first ? side : Vec2{-side.x, -side.y};
    }

    static ArenaInput autopilot(const ArenaFight& self) {
        ArenaInput in;
        if (self.phase_ != ArenaPhase::Fighting) return in;
        const Player& p = self.player_;
        const ArenaFighter& f = self.def_.fighter;
        const std::size_t count = self.enemyCount_;
        const auto living = [&](std::size_t i) { return i < count && self.enemy_[i].brain != ArenaBrain::Dead; };
        const auto down = [&](std::size_t i) {
            const ArenaBrain b = self.enemy_[i].brain;
            return b == ArenaBrain::Staggered || b == ArenaBrain::Open || b == ArenaBrain::Down;
        };

        // The nearest living enemy, and the nearest one a parry left open.
        int nearest = -1, opened = -1;
        float nearestDist = 0.f, openedDist = 0.f;
        for (std::size_t i = 0; i < count; ++i) {
            if (!living(i)) continue;
            const float d = arenaDistance(p.pos, self.enemy_[i].pos);
            if (nearest < 0 || d < nearestDist) {
                nearest = static_cast<int>(i);
                nearestDist = d;
            }
            if (self.enemy_[i].brain == ArenaBrain::Open && (opened < 0 || d < openedDist)) {
                opened = static_cast<int>(i);
                openedDist = d;
            }
        }
        if (nearest < 0) return in;
        // The view points at the nearest enemy, so a lock press picks it.
        in.view = arenaUnit(arenaSub(self.enemy_[static_cast<std::size_t>(nearest)].pos, p.pos), p.facing);
        if (p.action == ArenaAction::Hitstun || p.action == ArenaAction::GuardBreak || p.action == ArenaAction::Downed ||
            p.action == ArenaAction::Heal) // nothing to press; a heal is waited out standing still
            return in;

        // Attacks that could still land: the nearest in time that can reach the player.
        bool windingUp = false;
        int threat = -1;
        float soonest = 0.f;
        for (std::size_t i = 0; i < count; ++i) {
            if (!living(i)) continue;
            const ArenaFight::EnemyState& foe = self.enemy_[i];
            if (foe.brain != ArenaBrain::Attack || foe.hitPlayer) continue;
            const ArenaAttack& a = self.def_.enemies[i].moves[foe.move].attack;
            if (foe.stateTime >= a.startup + a.active) continue; // already recovering
            windingUp = true;
            if (arenaDistance(p.pos, foe.pos) > a.range + a.lunge + f.bodyRadius) continue; // can't reach
            const float untilActive = a.startup - foe.stateTime;
            if (threat < 0 || untilActive < soonest) {
                threat = static_cast<int>(i);
                soonest = untilActive;
            }
        }
        if (threat >= 0) {
            // Parry it (a parryable move, a window worth the trouble, the attacker inside the guard arc), else roll
            // sideways so its active part lands in the i-frames. Nothing else while it comes.
            const ArenaFight::EnemyState& foe = self.enemy_[static_cast<std::size_t>(threat)];
            const ArenaEnemyMove& mv = self.def_.enemies[static_cast<std::size_t>(threat)].moves[foe.move];
            const Vec2 toFoe = arenaUnit(arenaSub(foe.pos, p.pos), p.facing);
            const bool inArc = arenaDot(p.facing, toFoe) >= f.guardArcCos;
            const bool rearmed = p.guardHeld || self.time_ - p.lastGuardPress >= f.parryRearm;
            if (mv.parryable && f.parryWindow >= kArenaAutoParryWindow && inArc && rearmed) {
                // Guard goes up when the active part is half a window away, and stays up till it is spent.
                if (soonest <= f.parryWindow * 0.5f && canStart(p, f)) in.guard = true;
            } else if (p.action != ArenaAction::Dodge && p.stamina > 0.f) {
                if (soonest <= 0.5f * (f.dodgeInvulnStart + f.dodgeInvulnEnd)) {
                    in.dodge = true;
                    in.move = sideStep(self, foe.pos);
                }
            }
            return in;
        }

        // Heal when low, and nothing is near or winding up.
        if (p.charges > 0 && p.hp * 100 < kArenaAutoHealPercent * f.hp && !windingUp) {
            bool crowded = false;
            for (std::size_t i = 0; i < count; ++i) {
                if (!living(i) || down(i)) continue;
                const ArenaEnemyKind& k = self.def_.enemies[i];
                float reach = 0.f;
                for (std::size_t m = 0; m < k.moveCount; ++m) reach = std::max(reach, k.moves[m].maxRange);
                if (arenaDistance(p.pos, self.enemy_[i].pos) <= reach) crowded = true;
            }
            if (!crowded) {
                in.heal = true;
                return in;
            }
        }

        // An open enemy: riposte it (in reach and in front), else turn to it and close in.
        if (opened >= 0) {
            const ArenaFight::EnemyState& foe = self.enemy_[static_cast<std::size_t>(opened)];
            const Vec2 to = arenaSub(foe.pos, p.pos);
            const float d = arenaLength(to);
            const Vec2 dir = arenaUnit(to, p.facing);
            const bool front = d <= kArenaEps || arenaDot(p.facing, dir) >= kArenaRiposteDot;
            if (d <= f.riposteReach && front) {
                in.light = true;
            } else {
                in.move = dir;
                if (!front && p.lock != kArenaNobody) in.lock = true; // let go: the stick turns us to it
            }
            return in;
        }

        if (p.lock == kArenaNobody && nearestDist <= f.lockRange) in.lock = true;
        if (windingUp) return in; // an attack is coming at someone out of reach: hold

        // Close in on the target (the locked enemy, else the nearest) and swing the light chain.
        const std::size_t target = (p.lock != kArenaNobody && living(p.lock)) ? static_cast<std::size_t>(p.lock)
                                                                           : static_cast<std::size_t>(nearest);
        const Vec2 to = arenaSub(self.enemy_[target].pos, p.pos);
        const float d = arenaLength(to);
        float range = f.light[0].range, cost = 0.f;
        for (std::size_t l = 0; l < f.lightCount; ++l) {
            range = std::min(range, f.light[l].range);
            cost = std::max(cost, f.light[l].stamina);
        }
        const float reach = range + self.def_.enemies[target].bodyRadius;
        if (d > reach * kArenaAutoStand) {
            in.move = arenaUnit(to, p.facing);
        } else {
            bool danger = false; // an enemy that can still act: keep a dodge in reserve
            for (std::size_t i = 0; i < count; ++i)
                if (living(i) && !down(i)) danger = true;
            if (p.stamina > 0.f && p.stamina >= cost + (danger ? f.dodgeStamina : 0.f)) in.light = true;
        }
        return in;
    }
};

// ---- ArenaFight ----

void ArenaFight::begin(const ArenaDef& d, const ArenaRing& where, Vec2 playerPos, Vec2 playerFacing, std::uint32_t seed) {
    def_ = d;
    // A def built in code may say anything; hold it to the capacities.
    def_.enemyCount = static_cast<std::uint8_t>(std::clamp<int>(def_.enemyCount, 1, static_cast<int>(kMaxArenaEnemies)));
    def_.attackers = static_cast<std::uint8_t>(std::clamp<int>(def_.attackers, 1, static_cast<int>(kMaxArenaEnemies)));
    def_.fighter.lightCount =
        static_cast<std::uint8_t>(std::clamp<int>(def_.fighter.lightCount, 1, static_cast<int>(kMaxLightChain)));
    for (ArenaEnemyKind& k : def_.enemies)
        k.moveCount = static_cast<std::uint8_t>(std::clamp<int>(k.moveCount, 1, static_cast<int>(kMaxArenaMoves)));
    ring_ = where;
    seed_ = seed;
    enemyCount_ = def_.enemyCount;
    playerStart_ = playerPos;
    playerStartFacing_ = arenaUnit(playerFacing, Vec2{1.f, 0.f});
    // The enemies spread on an arc spawnDistance in front of the player, facing it, inside the ring.
    const float spawn = std::max(def_.spawnDistance, 0.f);
    float gap = 0.f;
    for (std::size_t i = 0; i < enemyCount_; ++i) gap = std::max(gap, kArenaSpawnGapRadii * def_.enemies[i].bodyRadius);
    float step = spawn > kArenaEps ? gap / spawn : 0.f;
    if (enemyCount_ > 1) step = std::min(step, kArenaPi / static_cast<float>(enemyCount_ - 1)); // never round behind it
    for (std::size_t i = 0; i < kMaxArenaEnemies; ++i) {
        enemyStart_[i] = Vec2{};
        enemyStartFacing_[i] = Vec2{-playerStartFacing_.x, -playerStartFacing_.y};
        if (i >= enemyCount_) continue;
        const float angle = (static_cast<float>(i) - 0.5f * static_cast<float>(enemyCount_ - 1)) * step;
        const Vec2 at = ring_.clamp(arenaMadd(playerStart_, arenaRotate(playerStartFacing_, angle), spawn),
                                    def_.enemies[i].bodyRadius);
        enemyStart_[i] = at;
        enemyStartFacing_[i] = arenaUnit(arenaSub(playerStart_, at), Vec2{-playerStartFacing_.x, -playerStartFacing_.y});
    }
    stats_ = ArenaStats{};
    dropped_ = 0;
    ArenaFightImpl(*this).reset();
}

void ArenaFight::restart() {
    if (phase_ == ArenaPhase::NotStarted) return;
    ++stats_.restarts;
    ArenaFightImpl(*this).reset();
}

const ArenaOutput& ArenaFight::tick(float dt, const ArenaInput& input, const ArenaBodies& bodies) {
    ArenaFightImpl(*this).tick(dt, input, bodies);
    return out_;
}

ArenaInput ArenaFight::autopilot() const { return ArenaFightImpl::autopilot(*this); }

ArenaPhase ArenaFight::phase() const { return phase_; }
float ArenaFight::time() const { return time_; }
const ArenaDef& ArenaFight::def() const { return def_; }
const ArenaRing& ArenaFight::ring() const { return ring_; }
const ArenaOutput& ArenaFight::output() const { return out_; }
const ArenaStats& ArenaFight::stats() const { return stats_; }
std::uint32_t ArenaFight::droppedEvents() const { return dropped_; }

Vec2 ArenaFight::playerStart() const { return playerStart_; }
Vec2 ArenaFight::playerStartFacing() const { return playerStartFacing_; }
std::size_t ArenaFight::enemyCount() const { return enemyCount_; }
Vec2 ArenaFight::enemyStart(std::size_t i) const { return i < kMaxArenaEnemies ? enemyStart_[i] : Vec2{}; }
Vec2 ArenaFight::enemyStartFacing(std::size_t i) const { return i < kMaxArenaEnemies ? enemyStartFacing_[i] : Vec2{1.f, 0.f}; }

int ArenaFight::hp() const { return player_.hp; }
float ArenaFight::stamina() const { return player_.stamina; }
float ArenaFight::poise() const { return player_.poise; }
std::uint8_t ArenaFight::healCharges() const { return player_.charges; }
std::uint8_t ArenaFight::lock() const { return player_.lock; }
int ArenaFight::enemyHp(std::size_t i) const { return i < enemyCount_ ? enemy_[i].hp : 0; }
float ArenaFight::enemyPoise(std::size_t i) const { return i < enemyCount_ ? enemy_[i].poise : 0.f; }
bool ArenaFight::enemyAlive(std::size_t i) const { return i < enemyCount_ && enemy_[i].brain != ArenaBrain::Dead; }

// ---- free functions ----

const char* arenaActionName(ArenaAction a) {
    switch (a) {
    case ArenaAction::Idle: return "Idle";
    case ArenaAction::Move: return "Move";
    case ArenaAction::Light: return "Light";
    case ArenaAction::Heavy: return "Heavy";
    case ArenaAction::Riposte: return "Riposte";
    case ArenaAction::Dodge: return "Dodge";
    case ArenaAction::Guard: return "Guard";
    case ArenaAction::Heal: return "Heal";
    case ArenaAction::Hitstun: return "Hitstun";
    case ArenaAction::GuardBreak: return "GuardBreak";
    case ArenaAction::Downed: return "Downed";
    }
    return "";
}

const char* arenaBrainName(ArenaBrain b) {
    switch (b) {
    case ArenaBrain::Approach: return "Approach";
    case ArenaBrain::Strafe: return "Strafe";
    case ArenaBrain::Attack: return "Attack";
    case ArenaBrain::Staggered: return "Staggered";
    case ArenaBrain::Open: return "Open";
    case ArenaBrain::Down: return "Down";
    case ArenaBrain::Dead: return "Dead";
    }
    return "";
}

ArenaDef defaultArenaDef() {
    ArenaDef def;
    ArenaFighter& f = def.fighter; // the same kit as the "default" fighter in Data/Battle/arena.json
    f.hp = 140;
    f.stamina = 100.f;
    f.staminaRegen = 45.f;
    f.staminaDelay = 0.6f;
    f.poise = 25.f;
    f.poiseDelay = 1.5f;
    f.hitstun = 0.5f;
    f.moveSpeed = 4.5f;
    f.moveAccel = 25.f;
    f.moveBrake = 40.f;
    f.turnRate = 14.f;
    f.bodyRadius = 0.4f;
    f.bufferTime = 0.2f;
    f.lightCount = 3;
    f.chainGap = 0.3f;
    ArenaAttack& l0 = f.light[0];
    l0.startup = 0.18f; l0.active = 0.12f; l0.recovery = 0.35f; l0.range = 2.1f; l0.arcCos = 0.3f;
    l0.damage = 12; l0.poiseDamage = 10.f; l0.lunge = 0.4f; l0.tracking = 8.f; l0.stamina = 14.f; l0.cancel = 0.5f;
    ArenaAttack& l1 = f.light[1];
    l1 = l0;
    l1.startup = 0.16f;
    ArenaAttack& l2 = f.light[2];
    l2.startup = 0.22f; l2.active = 0.14f; l2.recovery = 0.55f; l2.range = 2.3f; l2.arcCos = 0.f;
    l2.damage = 18; l2.poiseDamage = 22.f; l2.lunge = 0.6f; l2.tracking = 8.f; l2.stamina = 18.f; l2.cancel = 0.5f;
    ArenaAttack& h = f.heavy;
    h.startup = 0.55f; h.active = 0.18f; h.recovery = 0.65f; h.range = 2.4f; h.arcCos = 0.2f;
    h.damage = 38; h.poiseDamage = 45.f; h.lunge = 0.9f; h.tracking = 4.f; h.armor = 0.6f; h.stamina = 30.f; h.cancel = 0.7f;
    ArenaAttack& r = f.riposte;
    r.startup = 0.12f; r.active = 0.15f; r.recovery = 0.5f; r.range = 2.6f; r.arcCos = 0.f;
    r.damage = 48; r.poiseDamage = 0.f; r.lunge = 0.5f; r.tracking = 12.f; r.stamina = 0.f; r.cancel = 0.5f; r.maxTargets = 1;

    // One enemy with a quick strike (move 0) and a slow two-hit string (moves 1 and 2).
    ArenaEnemyKind& k = def.enemies[0];
    k.hp = 150;
    k.poise = 48.f;
    k.accel = 8.f;
    k.moveCount = 3;
    ArenaEnemyMove& strike = k.moves[0];
    strike.attack.startup = 0.5f; strike.attack.active = 0.15f; strike.attack.recovery = 0.6f; strike.attack.range = 2.2f;
    strike.attack.damage = 12; strike.attack.poiseDamage = 12.f; strike.attack.guardDamage = 16.f; strike.attack.lunge = 0.6f;
    strike.attack.tracking = 4.f;
    strike.minRange = 0.f; strike.maxRange = 2.4f; strike.weight = 1.f;
    ArenaEnemyMove& first = k.moves[1];
    first.attack.startup = 0.8f; first.attack.active = 0.2f; first.attack.recovery = 0.5f; first.attack.range = 2.4f;
    first.attack.damage = 16; first.attack.poiseDamage = 20.f; first.attack.guardDamage = 24.f; first.attack.lunge = 0.6f;
    first.attack.tracking = 3.f;
    first.minRange = 0.f; first.maxRange = 2.6f; first.weight = 0.8f; first.chain = 2;
    ArenaEnemyMove& second = k.moves[2];
    second.attack.startup = 0.6f; second.attack.active = 0.2f; second.attack.recovery = 0.9f; second.attack.range = 2.6f;
    second.attack.damage = 14; second.attack.poiseDamage = 18.f; second.attack.guardDamage = 20.f; second.attack.lunge = 0.8f;
    second.attack.tracking = 2.5f;
    second.minRange = 0.f; second.maxRange = 3.f; second.weight = 0.f;
    def.enemyCount = 1;
    return def;
}

ArenaRing arenaRingAt(Vec2 playerPos, Vec2 facing, float radius, const ArenaRing& bounds) {
    ArenaRing ring;
    ring.radius = std::max(radius, 0.f);
    const Vec2 ahead = arenaUnit(facing, Vec2{1.f, 0.f});
    ring.centre = arenaMadd(playerPos, ahead, ring.radius * 0.35f);
    if (!(bounds.radius > 0.f)) return ring;
    // Shrunk when it can't fit at all, then moved the least that puts it inside.
    ring.radius = std::min(ring.radius, bounds.radius);
    const float room = bounds.radius - ring.radius;
    const Vec2 off = arenaSub(ring.centre, bounds.centre);
    const float d = arenaLength(off);
    if (d > room) ring.centre = d > 0.f ? arenaMadd(bounds.centre, off, room / d) : bounds.centre;
    return ring;
}

Vec2 arenaParkSpot(const ArenaRing& ring, Vec2 playerStart, std::size_t i) {
    // Behind the player's start: on the side of the ring it stands on, then spread either way round the edge.
    const Vec2 base = arenaUnit(arenaSub(playerStart, ring.centre), Vec2{-1.f, 0.f});
    const float radius = ring.radius + kArenaParkOffset;
    const float step = kArenaParkSpacing / std::max(radius, kArenaEps);
    const float rank = static_cast<float>((i + 1u) / 2u);
    const float angle = (i % 2u == 1u ? 1.f : -1.f) * rank * step;
    return arenaMadd(ring.centre, arenaRotate(base, angle), radius);
}

void arenaStep(const ArenaFight& fight, ArenaBodies& bodies, float dt) {
    const std::size_t n = fight.enemyCount();
    const ArenaOutput& out = fight.output();
    const ArenaDef& def = fight.def();
    const float playerRadius = def.fighter.bodyRadius;
    if (dt > 0.f) {
        bodies.player = arenaMadd(bodies.player, out.player.velocity, dt);
        for (std::size_t i = 0; i < n; ++i) bodies.enemies[i] = arenaMadd(bodies.enemies[i], out.enemies[i].velocity, dt);
    }
    // Overlapping bodies are pushed apart, half each; dead enemies don't block.
    const auto separate = [](Vec2& a, float ra, Vec2& b, float rb) {
        const Vec2 d = arenaSub(b, a);
        const float distance = arenaLength(d);
        const float least = ra + rb;
        if (distance >= least) return;
        const Vec2 dir = distance > kArenaEps ? Vec2{d.x / distance, d.y / distance} : Vec2{1.f, 0.f};
        const float push = 0.5f * (least - distance);
        a = arenaMadd(a, dir, -push);
        b = arenaMadd(b, dir, push);
    };
    for (int pass = 0; pass < kArenaPushPasses; ++pass) {
        for (std::size_t i = 0; i < n; ++i)
            if (fight.enemyAlive(i)) separate(bodies.player, playerRadius, bodies.enemies[i], def.enemies[i].bodyRadius);
        for (std::size_t i = 0; i < n; ++i) {
            if (!fight.enemyAlive(i)) continue;
            for (std::size_t j = i + 1; j < n; ++j)
                if (fight.enemyAlive(j))
                    separate(bodies.enemies[i], def.enemies[i].bodyRadius, bodies.enemies[j], def.enemies[j].bodyRadius);
        }
    }
    // Everything stays inside the ring.
    const ArenaRing& ring = fight.ring();
    bodies.player = ring.clamp(bodies.player, playerRadius);
    for (std::size_t i = 0; i < n; ++i) bodies.enemies[i] = ring.clamp(bodies.enemies[i], def.enemies[i].bodyRadius);
}

// ---- ArenaTable ----

const ArenaFighter& ArenaTable::fighter(std::string_view leadId) const {
    const std::size_t count = std::min(fighterIds.size(), fighters.size());
    for (std::size_t i = 0; i < count; ++i)
        if (std::string_view(fighterIds[i]) == leadId) return fighters[i];
    return fighters.empty() ? fallback.fighter : fighters[0];
}

const ArenaFightEntry* ArenaTable::findEntry(std::string_view scene, std::uint32_t beat, std::uint32_t step) const {
    for (const ArenaFightEntry& e : entries)
        if (e.beat == beat && e.step == step && e.scene == scene) return &e;
    return nullptr;
}

const ArenaDef& ArenaTable::find(std::string_view scene, std::uint32_t beat, std::uint32_t step) const {
    const ArenaFightEntry* e = findEntry(scene, beat, step);
    return e ? e->def : fallback;
}

// ---- data ----
//
// Number ranges the loader checks (an integer where the struct field is one; the ends are included unless a "(" says
// otherwise). Anything outside is an error with its path.
//
//   fighter   hp 1..100000; stamina (0, 100000]; staminaRegen 0..100000; staminaDelay, poiseDelay, hitstun,
//             bufferTime, chainGap, guardBreakTime, parryWindow, parryRearm, healTime, healRecovery 0..10;
//             poise (0, 100000]; moveSpeed (0, 50]; turnRate (0, 100]; bodyRadius (0, 5]; light 1..4 attacks;
//             dodgeTime (0, 10]; dodgeDistance 0..100; dodgeInvulnStart <= dodgeInvulnEnd <= dodgeTime;
//             dodgeStamina 0..100000; dodgeCancel, guardAbsorb, guardMoveScale, healMoveScale 0..1;
//             guardArcCos -1..1; riposteReach (0, 100]; healCharges 0..99; healAmount 0..100000;
//             lockRange (0, 1000]; lockBreak (0, 1000] and at least lockRange; moveAccel, moveBrake 0..1000
//   attack    startup, recovery 0..10; active (0, 10]; range (0, 100]; arcCos -1..1; damage, poiseDamage,
//             guardDamage, stamina 0..100000; lunge, tracking 0..100; armor, cancel 0..1; maxTargets 0..6
//   enemy     hp 1..100000; poise (0, 100000]; poiseDelay, staggerTime, openTime, downTime, attackDelay 0..10;
//             walkSpeed, runSpeed, strafeSpeed 0..50; turnRate (0, 100]; preferredRange (0, 100];
//             runRange 0..100 and at least preferredRange; bodyRadius (0, 5]; moves 1..6; accel 0..1000;
//             at most 254 kinds in the file (ArenaDef::enemyKind holds an index, kArenaNoKind is 255)
//   move      the attack's keys; minRange 0..100; maxRange (0, 100] and at least minRange; cooldown 0..60;
//             weight 0..1000 (0: only ever reached by a chain; at least one move needs more);
//             chain -1 or another move of the same enemy, with no loop; unblockable, parryable booleans
//   fight     enemies 1..6 known enemy ids; ring (0, 1000]; spawn (0, 1000]; attackers 1..6;
//             scene a non-empty id; beat 0..1000000; step 0..1000

namespace {

using ArenaStrings = std::vector<std::string>;

constexpr std::string_view kArenaAttackKeys[] = {"startup", "active",      "recovery",    "range",  "arcCos",
                                                 "damage",  "poiseDamage", "guardDamage", "lunge",  "tracking",
                                                 "armor",   "stamina",     "cancel",      "maxTargets"};

std::string arenaFormat(double v) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%g", v);
    return buffer;
}

void arenaUnknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys,
                      bool attackKeys, ArenaStrings* warnings) {
    if (!warnings) return;
    for (std::uint32_t k = 0; k < j.size(); ++k) {
        const std::string_view key = j.key(k);
        bool known = std::find(keys.begin(), keys.end(), key) != keys.end();
        if (!known && attackKeys) known = std::find(std::begin(kArenaAttackKeys), std::end(kArenaAttackKeys), key) != std::end(kArenaAttackKeys);
        if (!known) warnings->push_back((path.empty() ? std::string() : path + ": ") + "unknown key: " + std::string(key));
    }
}

// A number in range; absent is fine (the field keeps its value). False, with an error, when present and bad.
bool arenaNum(JsonValue j, const char* key, float& out, double lo, double hi, bool loOpen, const std::string& path,
              ArenaStrings& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent()) return true;
    const double d = v.number(-1e30);
    const bool good = v.isNumber() && std::isfinite(d) && (loOpen ? d > lo : d >= lo) && d <= hi;
    if (!good) {
        errors.push_back(path + "." + key + ": expected a number in " + (loOpen ? "(" : "[") + arenaFormat(lo) + ", " +
                         arenaFormat(hi) + "]");
        return false;
    }
    out = static_cast<float>(d);
    return true;
}

bool arenaInt(JsonValue j, const char* key, int& out, int lo, int hi, const std::string& path, ArenaStrings& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent()) return true;
    if (!v.isInteger() || v.integer() < lo || v.integer() > hi) {
        errors.push_back(path + "." + key + ": expected an integer in " + std::to_string(lo) + ".." + std::to_string(hi));
        return false;
    }
    out = static_cast<int>(v.integer());
    return true;
}

bool arenaByte(JsonValue j, const char* key, std::uint8_t& out, int lo, int hi, const std::string& path,
               ArenaStrings& errors) {
    int value = out;
    const bool ok = arenaInt(j, key, value, lo, hi, path, errors);
    if (ok) out = static_cast<std::uint8_t>(value);
    return ok;
}

bool arenaBool(JsonValue j, const char* key, bool& out, const std::string& path, ArenaStrings& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent()) return true;
    if (!v.isBool()) {
        errors.push_back(path + "." + key + ": expected true or false");
        return false;
    }
    out = v.boolean();
    return true;
}

void arenaParseAttack(JsonValue j, const std::string& path, ArenaAttack& a, ArenaStrings& errors) {
    arenaNum(j, "startup", a.startup, 0, 10, false, path, errors);
    arenaNum(j, "active", a.active, 0, 10, true, path, errors);
    arenaNum(j, "recovery", a.recovery, 0, 10, false, path, errors);
    arenaNum(j, "range", a.range, 0, 100, true, path, errors);
    arenaNum(j, "arcCos", a.arcCos, -1, 1, false, path, errors);
    arenaInt(j, "damage", a.damage, 0, 100000, path, errors);
    arenaNum(j, "poiseDamage", a.poiseDamage, 0, 100000, false, path, errors);
    arenaNum(j, "guardDamage", a.guardDamage, 0, 100000, false, path, errors);
    arenaNum(j, "lunge", a.lunge, 0, 100, false, path, errors);
    arenaNum(j, "tracking", a.tracking, 0, 100, false, path, errors);
    arenaNum(j, "armor", a.armor, 0, 1, false, path, errors);
    arenaNum(j, "stamina", a.stamina, 0, 100000, false, path, errors);
    arenaNum(j, "cancel", a.cancel, 0, 1, false, path, errors);
    arenaByte(j, "maxTargets", a.maxTargets, 0, static_cast<int>(kMaxArenaEnemies), path, errors);
}

// An attack object that is a value of its own (light entries, heavy, riposte).
void arenaParseAttackObject(JsonValue j, const std::string& path, ArenaAttack& a, ArenaStrings& errors,
                            ArenaStrings* warnings) {
    if (!j.isObject()) {
        errors.push_back(path + ": expected an object");
        return;
    }
    arenaUnknownKeys(j, path, {}, true, warnings);
    arenaParseAttack(j, path, a, errors);
}

void arenaParseFighter(JsonValue j, const std::string& path, ArenaFighter& f, ArenaStrings& errors,
                       ArenaStrings* warnings) {
    arenaUnknownKeys(j, path,
                     {"id", "hp", "stamina", "staminaRegen", "staminaDelay", "poise", "poiseDelay", "hitstun",
                      "moveSpeed", "turnRate", "bodyRadius", "bufferTime", "light", "chainGap", "heavy", "riposte",
                      "dodgeTime", "dodgeDistance", "dodgeInvulnStart", "dodgeInvulnEnd", "dodgeStamina",
                      "dodgeCancel", "guardArcCos", "guardAbsorb", "guardMoveScale", "guardBreakTime", "parryWindow",
                      "parryRearm", "riposteReach", "healCharges", "healAmount", "healTime", "healRecovery",
                      "healMoveScale", "lockRange", "lockBreak", "moveAccel", "moveBrake"},
                     false, warnings);
    arenaInt(j, "hp", f.hp, 1, 100000, path, errors);
    arenaNum(j, "stamina", f.stamina, 0, 100000, true, path, errors);
    arenaNum(j, "staminaRegen", f.staminaRegen, 0, 100000, false, path, errors);
    arenaNum(j, "staminaDelay", f.staminaDelay, 0, 10, false, path, errors);
    arenaNum(j, "poise", f.poise, 0, 100000, true, path, errors);
    arenaNum(j, "poiseDelay", f.poiseDelay, 0, 10, false, path, errors);
    arenaNum(j, "hitstun", f.hitstun, 0, 10, false, path, errors);
    arenaNum(j, "moveSpeed", f.moveSpeed, 0, 50, true, path, errors);
    arenaNum(j, "turnRate", f.turnRate, 0, 100, true, path, errors);
    arenaNum(j, "bodyRadius", f.bodyRadius, 0, 5, true, path, errors);
    arenaNum(j, "bufferTime", f.bufferTime, 0, 10, false, path, errors);
    arenaNum(j, "chainGap", f.chainGap, 0, 10, false, path, errors);
    arenaNum(j, "dodgeTime", f.dodgeTime, 0, 10, true, path, errors);
    arenaNum(j, "dodgeDistance", f.dodgeDistance, 0, 100, false, path, errors);
    arenaNum(j, "dodgeInvulnStart", f.dodgeInvulnStart, 0, 10, false, path, errors);
    arenaNum(j, "dodgeInvulnEnd", f.dodgeInvulnEnd, 0, 10, false, path, errors);
    arenaNum(j, "dodgeStamina", f.dodgeStamina, 0, 100000, false, path, errors);
    arenaNum(j, "dodgeCancel", f.dodgeCancel, 0, 1, false, path, errors);
    arenaNum(j, "guardArcCos", f.guardArcCos, -1, 1, false, path, errors);
    arenaNum(j, "guardAbsorb", f.guardAbsorb, 0, 1, false, path, errors);
    arenaNum(j, "guardMoveScale", f.guardMoveScale, 0, 1, false, path, errors);
    arenaNum(j, "guardBreakTime", f.guardBreakTime, 0, 10, false, path, errors);
    arenaNum(j, "parryWindow", f.parryWindow, 0, 10, false, path, errors);
    arenaNum(j, "parryRearm", f.parryRearm, 0, 10, false, path, errors);
    arenaNum(j, "riposteReach", f.riposteReach, 0, 100, true, path, errors);
    arenaByte(j, "healCharges", f.healCharges, 0, 99, path, errors);
    arenaInt(j, "healAmount", f.healAmount, 0, 100000, path, errors);
    arenaNum(j, "healTime", f.healTime, 0, 10, false, path, errors);
    arenaNum(j, "healRecovery", f.healRecovery, 0, 10, false, path, errors);
    arenaNum(j, "healMoveScale", f.healMoveScale, 0, 1, false, path, errors);
    arenaNum(j, "lockRange", f.lockRange, 0, 1000, true, path, errors);
    arenaNum(j, "lockBreak", f.lockBreak, 0, 1000, true, path, errors);
    arenaNum(j, "moveAccel", f.moveAccel, 0, 1000, false, path, errors);
    arenaNum(j, "moveBrake", f.moveBrake, 0, 1000, false, path, errors);

    const JsonValue light = j["light"];
    if (!light.isAbsent()) {
        if (!light.isArray() || light.size() == 0 || light.size() > kMaxLightChain) {
            errors.push_back(path + ".light: expected an array of 1.." + std::to_string(kMaxLightChain) + " attacks");
        } else {
            f.lightCount = static_cast<std::uint8_t>(light.size());
            for (std::uint32_t i = 0; i < light.size(); ++i)
                arenaParseAttackObject(light[i], path + ".light[" + std::to_string(i) + "]", f.light[i], errors, warnings);
        }
    }
    if (j.has("heavy")) arenaParseAttackObject(j["heavy"], path + ".heavy", f.heavy, errors, warnings);
    if (j.has("riposte")) arenaParseAttackObject(j["riposte"], path + ".riposte", f.riposte, errors, warnings);

    if (f.dodgeInvulnStart > f.dodgeInvulnEnd)
        errors.push_back(path + ".dodgeInvulnStart: expected at most dodgeInvulnEnd");
    if (f.dodgeInvulnEnd > f.dodgeTime) errors.push_back(path + ".dodgeInvulnEnd: expected at most dodgeTime");
    if (f.lockBreak < f.lockRange) errors.push_back(path + ".lockBreak: expected at least lockRange");
}

void arenaParseEnemy(JsonValue j, const std::string& path, ArenaEnemyKind& k, ArenaStrings& errors,
                     ArenaStrings* warnings) {
    arenaUnknownKeys(j, path,
                     {"id", "hp", "poise", "poiseDelay", "staggerTime", "openTime", "downTime", "walkSpeed", "runSpeed",
                      "strafeSpeed", "turnRate", "preferredRange", "runRange", "attackDelay", "bodyRadius", "moves",
                      "accel"},
                     false, warnings);
    arenaInt(j, "hp", k.hp, 1, 100000, path, errors);
    arenaNum(j, "poise", k.poise, 0, 100000, true, path, errors);
    arenaNum(j, "poiseDelay", k.poiseDelay, 0, 10, false, path, errors);
    arenaNum(j, "staggerTime", k.staggerTime, 0, 10, false, path, errors);
    arenaNum(j, "openTime", k.openTime, 0, 10, false, path, errors);
    arenaNum(j, "downTime", k.downTime, 0, 10, false, path, errors);
    arenaNum(j, "walkSpeed", k.walkSpeed, 0, 50, false, path, errors);
    arenaNum(j, "runSpeed", k.runSpeed, 0, 50, false, path, errors);
    arenaNum(j, "strafeSpeed", k.strafeSpeed, 0, 50, false, path, errors);
    arenaNum(j, "turnRate", k.turnRate, 0, 100, true, path, errors);
    arenaNum(j, "preferredRange", k.preferredRange, 0, 100, true, path, errors);
    arenaNum(j, "runRange", k.runRange, 0, 100, false, path, errors);
    arenaNum(j, "attackDelay", k.attackDelay, 0, 10, false, path, errors);
    arenaNum(j, "accel", k.accel, 0, 1000, false, path, errors);
    arenaNum(j, "bodyRadius", k.bodyRadius, 0, 5, true, path, errors);
    if (k.runRange < k.preferredRange) errors.push_back(path + ".runRange: expected at least preferredRange");

    const JsonValue moves = j["moves"];
    if (moves.isAbsent()) return; // the one default move stays
    if (!moves.isArray() || moves.size() == 0 || moves.size() > kMaxArenaMoves) {
        errors.push_back(path + ".moves: expected an array of 1.." + std::to_string(kMaxArenaMoves) + " moves");
        return;
    }
    k.moveCount = static_cast<std::uint8_t>(moves.size());
    bool pickable = false;
    for (std::uint32_t m = 0; m < moves.size(); ++m) {
        const JsonValue mj = moves[m];
        const std::string mp = path + ".moves[" + std::to_string(m) + "]";
        if (!mj.isObject()) {
            errors.push_back(mp + ": expected an object");
            continue;
        }
        ArenaEnemyMove& mv = k.moves[m];
        arenaUnknownKeys(mj, mp, {"minRange", "maxRange", "cooldown", "weight", "chain", "unblockable", "parryable"}, true,
                         warnings);
        arenaParseAttack(mj, mp, mv.attack, errors);
        arenaNum(mj, "minRange", mv.minRange, 0, 100, false, mp, errors);
        arenaNum(mj, "maxRange", mv.maxRange, 0, 100, true, mp, errors);
        arenaNum(mj, "cooldown", mv.cooldown, 0, 60, false, mp, errors);
        arenaNum(mj, "weight", mv.weight, 0, 1000, false, mp, errors);
        arenaBool(mj, "unblockable", mv.unblockable, mp, errors);
        arenaBool(mj, "parryable", mv.parryable, mp, errors);
        if (mv.minRange > mv.maxRange) errors.push_back(mp + ".minRange: expected at most maxRange");
        if (mv.weight > 0.f) pickable = true;
        const JsonValue c = mj["chain"];
        if (!c.isAbsent()) {
            if (!c.isInteger() || c.integer() < -1 || c.integer() >= static_cast<std::int64_t>(moves.size()))
                errors.push_back(mp + ".chain: expected -1 or the index of a move of this enemy (0.." +
                                 std::to_string(moves.size() - 1) + ")");
            else if (c.integer() == static_cast<std::int64_t>(m))
                errors.push_back(mp + ".chain: a move cannot chain to itself");
            else
                mv.chain = static_cast<std::int8_t>(c.integer());
        }
    }
    if (!pickable) errors.push_back(path + ".moves: expected at least one move with a weight above 0");
    // A string may not loop: following the chain from any move must end within moveCount steps.
    for (std::uint32_t m = 0; m < k.moveCount; ++m) {
        const std::uint32_t most = k.moveCount;
        std::uint32_t at = m, steps = 0;
        while (k.moves[at].chain >= 0 && k.moves[at].chain < static_cast<int>(k.moveCount) && steps <= most) {
            at = static_cast<std::uint32_t>(k.moves[at].chain);
            ++steps;
        }
        if (steps > most) {
            errors.push_back(path + ".moves[" + std::to_string(m) + "].chain: the string loops");
            break;
        }
    }
}

// The enemies, ring, attackers and spawn that a fight (the default one or an entry) holds.
void arenaParseFightBody(JsonValue j, const std::string& path, const ArenaTable& table, ArenaDef& def,
                         ArenaStrings& errors) {
    const JsonValue list = j["enemies"];
    if (!list.isArray() || list.size() == 0 || list.size() > kMaxArenaEnemies) {
        errors.push_back(path + ".enemies: expected an array of 1.." + std::to_string(kMaxArenaEnemies) + " enemy ids");
    } else {
        def.enemyCount = static_cast<std::uint8_t>(list.size());
        for (std::uint32_t i = 0; i < list.size(); ++i) {
            const std::string_view want = list[i].string();
            std::size_t found = table.enemyIds.size();
            for (std::size_t e = 0; e < table.enemyIds.size(); ++e)
                if (std::string_view(table.enemyIds[e]) == want) found = e;
            if (found == table.enemyIds.size()) {
                errors.push_back(path + ".enemies[" + std::to_string(i) + "]: unknown enemy id: " + std::string(want));
            } else {
                def.enemies[i] = table.enemies[found];
                def.enemyKind[i] = found < kArenaNoKind ? static_cast<std::uint8_t>(found) : kArenaNoKind; // the table is capped below
            }
        }
    }
    arenaNum(j, "ring", def.ringRadius, 0, 1000, true, path, errors);
    arenaNum(j, "spawn", def.spawnDistance, 0, 1000, true, path, errors);
    arenaByte(j, "attackers", def.attackers, 1, static_cast<int>(kMaxArenaEnemies), path, errors);
}

} // namespace

bool importArenaTable(std::string_view json, ArenaTable& out, std::string* error, std::vector<std::string>* warnings) {
    out = ArenaTable{};
    ArenaStrings errors;
    const auto finish = [&]() {
        if (errors.empty()) return true;
        if (error) {
            error->clear();
            for (const std::string& e : errors) {
                if (!error->empty()) *error += '\n';
                *error += e;
            }
        }
        return false;
    };

    JsonDocument doc;
    std::string err;
    if (!doc.parse(json, &err)) {
        errors.push_back("arena file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("arena file: expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !v.isAbsent() && !(v.isInteger() && v.integer() == 1))
        errors.push_back("version: expected 1");
    arenaUnknownKeys(root, "", {"version", "fighters", "enemies", "default", "fights"}, false, warnings);

    ArenaTable table;

    // Fighters: ids unique, "default" among them (and moved to the front).
    const JsonValue fighters = root["fighters"];
    if (!fighters.isAbsent() && !fighters.isArray()) errors.push_back("fighters: expected an array");
    for (std::uint32_t i = 0; i < fighters.size() && fighters.isArray(); ++i) {
        const JsonValue j = fighters[i];
        const std::string path = "fighters[" + std::to_string(i) + "]";
        if (!j.isObject()) {
            errors.push_back(path + ": expected an object");
            continue;
        }
        const std::string fid(j["id"].string());
        if (fid.empty()) errors.push_back(path + ".id: expected an id");
        for (const std::string& known : table.fighterIds)
            if (!fid.empty() && known == fid) errors.push_back(path + ".id: duplicate id: " + fid);
        ArenaFighter f;
        arenaParseFighter(j, path, f, errors, warnings);
        table.fighterIds.push_back(fid);
        table.fighters.push_back(f);
    }
    std::size_t defaultAt = table.fighterIds.size();
    for (std::size_t i = 0; i < table.fighterIds.size(); ++i)
        if (table.fighterIds[i] == "default") defaultAt = i;
    if (defaultAt == table.fighterIds.size()) {
        errors.push_back("fighters: expected a fighter with id \"default\"");
    } else if (defaultAt != 0) {
        std::rotate(table.fighterIds.begin(), table.fighterIds.begin() + static_cast<std::ptrdiff_t>(defaultAt),
                    table.fighterIds.begin() + static_cast<std::ptrdiff_t>(defaultAt) + 1);
        std::rotate(table.fighters.begin(), table.fighters.begin() + static_cast<std::ptrdiff_t>(defaultAt),
                    table.fighters.begin() + static_cast<std::ptrdiff_t>(defaultAt) + 1);
    }
    const ArenaFighter defaultFighter = table.fighters.empty() ? ArenaFighter{} : table.fighters[0];

    // Enemy kinds: ids unique.
    const JsonValue enemies = root["enemies"];
    if (!enemies.isAbsent() && !enemies.isArray()) errors.push_back("enemies: expected an array");
    for (std::uint32_t i = 0; i < enemies.size() && enemies.isArray(); ++i) {
        const JsonValue j = enemies[i];
        const std::string path = "enemies[" + std::to_string(i) + "]";
        if (!j.isObject()) {
            errors.push_back(path + ": expected an object");
            continue;
        }
        const std::string eid(j["id"].string());
        if (eid.empty()) errors.push_back(path + ".id: expected an id");
        for (const std::string& known : table.enemyIds)
            if (!eid.empty() && known == eid) errors.push_back(path + ".id: duplicate id: " + eid);
        ArenaEnemyKind k;
        arenaParseEnemy(j, path, k, errors, warnings);
        table.enemyIds.push_back(eid);
        table.enemies.push_back(k);
    }
    // ArenaDef::enemyKind holds an index in a byte, and kArenaNoKind is its last value.
    if (table.enemyIds.size() >= kArenaNoKind) errors.push_back("enemies: expected at most 254 kinds");

    // The default fight, and the fights keyed by scene, beat and step.
    const JsonValue def = root["default"];
    if (!def.isObject()) {
        errors.push_back("default: expected an object");
    } else {
        arenaUnknownKeys(def, "default", {"enemies", "ring", "attackers", "spawn"}, false, warnings);
        table.fallback.fighter = defaultFighter;
        arenaParseFightBody(def, "default", table, table.fallback, errors);
    }

    const JsonValue list = root["fights"];
    if (!list.isAbsent() && !list.isArray()) errors.push_back("fights: expected an array");
    for (std::uint32_t i = 0; i < list.size() && list.isArray(); ++i) {
        const JsonValue j = list[i];
        const std::string path = "fights[" + std::to_string(i) + "]";
        if (!j.isObject()) {
            errors.push_back(path + ": expected an object");
            continue;
        }
        arenaUnknownKeys(j, path, {"scene", "beat", "step", "enemies", "ring", "attackers", "spawn"}, false, warnings);
        ArenaFightEntry entry;
        entry.scene = std::string(j["scene"].string());
        bool good = !entry.scene.empty();
        if (entry.scene.empty()) errors.push_back(path + ".scene: expected a scene id");
        const JsonValue b = j["beat"];
        const JsonValue s = j["step"];
        if (!b.isInteger() || b.integer() < 0 || b.integer() > 1000000) {
            errors.push_back(path + ".beat: expected an integer in 0..1000000");
            good = false;
        } else {
            entry.beat = static_cast<std::uint32_t>(b.integer());
        }
        if (!s.isAbsent() && (!s.isInteger() || s.integer() < 0 || s.integer() > 1000)) {
            errors.push_back(path + ".step: expected an integer in 0..1000");
            good = false;
        } else {
            entry.step = static_cast<std::uint32_t>(s.integer(0));
        }
        entry.def.fighter = defaultFighter;
        arenaParseFightBody(j, path, table, entry.def, errors);
        if (good && table.findEntry(entry.scene, entry.beat, entry.step))
            errors.push_back(path + ": duplicate entry for " + entry.scene + " beat " + std::to_string(entry.beat) +
                             " step " + std::to_string(entry.step));
        table.entries.push_back(std::move(entry));
    }

    if (!finish()) return false;
    out = std::move(table);
    return true;
}

} // namespace ftcore
