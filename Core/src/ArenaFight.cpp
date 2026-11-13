#include "ftcore/ArenaFight.h"
#include "ftcore/ArenaSoftTarget.h"
#include "ftcore/ArenaImpact.h"
#include "ftcore/ArenaLook.h"

#include "SeedMix.h"
#include "ftcore/Json.h"

#include <algorithm>
#include <cctype>
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
constexpr float kArenaDrawEaseAngle = 0.5f;  // rad: the drawn turn slows over its last this much
constexpr float kArenaDrawEaseFloor = 0.25f; // ... down to this share of its pace
// A locked dodge whose way is within this cosine of straight away from the target is a back dodge: drawn facing the
// target (no back-hop clip yet: the squash), not rolled the way it goes.
constexpr float kArenaDodgeBackCos = 0.7071f;
constexpr float kArenaDodgeTurnShare = 0.3f; // a roll's drawn body turns half a turn over this share of dodgeTime (then eases out)
constexpr float kArenaAttackTurnShare = 0.6f; // an attack's drawn body turns half a turn over this share of its startup
constexpr float kArenaLockNearWeight = 0.3f;
constexpr int kArenaAutoHealPercent = 40;  // the autopilot heals below this share of its hp ...
constexpr float kArenaAutoParryWindow = 0.12f; // ... and parries only with a window at least this long
constexpr float kArenaAutoStand = 0.85f;   // it closes in to this share of its reach before it swings
constexpr int kArenaAutoReleaseAt = 50;    // it releases its Load from this much on an upright enemy in reach
constexpr float kArenaAutoReadDelay = 0.2f; // it reads this long into a startup (half of a shorter one)
constexpr std::uint64_t kArenaReadSeedStep = 0xA24BAED4963EE407ull; // spreads the deals of one fight over the seeds
constexpr float kArenaMaxBlow = 1.0e6f;   // a host's scaled damage or poise damage stops here (an int, times any share)
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

// The drawn body's turn: at most maxAngle a frame, slowing over the last kArenaDrawEaseAngle to kArenaDrawEaseFloor of
// that pace (slow out), so it settles into the facing instead of stopping dead; it still arrives.
Vec2 arenaDrawTurn(Vec2 drawn, Vec2 dir, float maxAngle) {
    const float left = std::fabs(std::atan2(arenaCross(drawn, dir), arenaDot(drawn, dir)));
    return arenaTurn(drawn, dir, maxAngle * std::clamp(left / kArenaDrawEaseAngle, kArenaDrawEaseFloor, 1.f));
}

// An attack from `from` facing `facing` (unit) reaches a body of `radius` at `to` when that body's centre is within
// range + radius of the attacker's centre and inside the arc.
bool arenaReaches(Vec2 from, Vec2 facing, const ArenaAttack& a, Vec2 to, float radius) {
    const Vec2 d = arenaSub(to, from);
    const float distance = arenaLength(d);
    if (a.width > 0.f) {
        // A lane: ahead of the attacker along its facing, and off that line by at most half the width.
        const float along = arenaDot(facing, d);
        const float off = std::fabs(facing.x * d.y - facing.y * d.x);
        return along >= -radius && along <= a.range + radius && off <= a.width * 0.5f + radius;
    }
    if (distance > a.range + radius) return false;
    if (distance <= kArenaEps) return true;
    const float dot = std::clamp(arenaDot(facing, d) / distance, -1.f, 1.f);
    return dot >= a.arcCos;
}

// The first of `count` bodies (kinds[i] standing at at[i], skipped where `skip` has its bit) that a round from `from` at
// kArenaShotHeight along `dir` (unit) climbing at `pitch` enters within the shot's range: its index, and `distance`
// along the round's line; kArenaNobody for none. Each body is its arenaBodyCylinder on a floor at 0 with the radius
// widened by half the shot's width; the line runs range + that radius in space (so a steep round reaches less far over
// the ground, and the body's near side, not its middle, must be in reach, unlike the flat lane). A lead standing inside
// a body whose centre is ahead of it (along `dir`, or exactly on the lead) meets it at 0 (point-blank), where
// arenaRayEntersBody, a lens's rule, would see through it, but only when the round leaves the body through its side:
// one that climbs out through its top (or dives out through its base) first, as a steep round at a hovering boss over
// an enemy the lead stands in, passes it. A body beside or behind the lead is not met (the round leaves it).
std::uint8_t arenaShotFirst(const ArenaAttack& a, Vec2 from, Vec2 dir, float pitch, const ArenaEnemyKind* kinds,
                            const Vec2* at, const bool* standing, std::size_t count, std::uint32_t skip,
                            float& distance) {
    std::uint8_t best = kArenaNobody;
    float nearest = 0.f;
    const float climb = std::isfinite(pitch) ? std::clamp(pitch, -1.5f, 1.5f) : 0.f; // about 86 degrees either way
    const float c = std::cos(climb), s = std::sin(climb);
    const ArenaPoint3 o{from.x, from.y, kArenaShotHeight};
    for (std::size_t i = 0; i < count; ++i) {
        if (!standing[i] || (skip & (1u << i)) != 0u) continue;
        ArenaBodyCylinder b = arenaBodyCylinder(kinds[i], at[i], 0.f);
        b.radius += std::max(0.f, a.width) * 0.5f;
        const float length = std::max(0.f, a.range) + b.radius;
        float d = 0.f;
        const float ox = o.x - b.at.x, oy = o.y - b.at.y;
        const float off2 = ox * ox + oy * oy;
        if (off2 <= b.radius * b.radius && o.z >= b.base && o.z <= b.top) {
            // Inside: the centre ahead (or on the lead), and the line out through the side before the top or base.
            const float along = -(ox * dir.x + oy * dir.y);
            if (!(along > 0.f || off2 <= kArenaEps * kArenaEps)) continue;
            const float p = ox * dir.x + oy * dir.y;
            const float flat = -p + std::sqrt(std::max(0.f, p * p - (off2 - b.radius * b.radius))); // over the ground
            const float side = flat / c; // along the line (c > 0: the climb is clamped short of straight up)
            const float cap = s > 0.f ? (b.top - o.z) / s : (s < 0.f ? (b.base - o.z) / s : side + 1.f);
            if (cap < side) continue;
            d = 0.f;
        } else {
            const ArenaPoint3 end{o.x + dir.x * c * length, o.y + dir.y * c * length, o.z + s * length};
            float t = 0.f;
            if (!arenaRayEntersBody(o, end, b, t)) continue;
            d = t * length;
        }
        if (best == kArenaNobody || d < nearest) {
            best = static_cast<std::uint8_t>(i);
            nearest = d;
        }
    }
    distance = nearest;
    return best;
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
    // The aim this tick (ArenaInput::aim): the way the camera aims, the turn rate toward it and the move scale.
    bool aimOn = false;
    Vec2 aimWay{1.f, 0.f};
    float aimTurn = 0.f;
    float aimSpeed = 1.f;
    float attackSpeed = 0.f; // ArenaInput::attackSpeed / attackTurn this tick
    float attackTurn = 0.f;
    float t1 = 0.f; // ... and its end
    float playerOwn = 0.f; // the player's own time this tick: dt less any hit pause

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
        case ArenaAction::Light:
            return p.move == kArenaShotMove ? &f.shot : &f.light[std::min<std::size_t>(p.move, f.lightCount - 1u)];
        case ArenaAction::Heavy: return &f.heavy;
        case ArenaAction::Riposte: return &f.riposte;
        case ArenaAction::Release: return &f.release;
        default: return nullptr;
        }
    }

    // What wants to cut into the current action: an attack's own cancel window for it, if it names one.
    enum class Cut : std::uint8_t { Other, Guard, Roll, Light };
    static Cut cutOf(Press what) {
        switch (what) {
        case Press::Light:
        case Press::Shot: return Cut::Light;
        case Press::Dodge: return Cut::Roll;
        default: return Cut::Other;
        }
    }

    // A new action may start from Idle/Move, from an attack once `cancel` of its recovery has passed (or its own
    // window for that action, a share of the whole attack), from a dodge once `dodgeCancel` of it has passed, and
    // from Guard at any time.
    static bool canStart(const Player& p, const ArenaFighter& f, Cut cut = Cut::Other) {
        switch (p.action) {
        case ArenaAction::Idle:
        case ArenaAction::Move:
        case ArenaAction::Guard:
            return true;
        case ArenaAction::Light:
        case ArenaAction::Heavy:
        case ArenaAction::Riposte:
        case ArenaAction::Release: {
            const ArenaAttack& a = *attackOf(p, f);
            const float own = cut == Cut::Guard  ? a.cancelGuard
                              : cut == Cut::Roll  ? a.cancelRoll
                              : cut == Cut::Light ? a.cancelLight
                                                  : -1.f;
            if (own >= 0.f) return p.actionTime >= own * a.total();
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
        fight.heldTime_ = 0.f;
        pl = Player{};
        pl.pos = fight.playerStart_;
        pl.facing = fight.playerStartFacing_;
        pl.drawFacing = pl.facing;
        pl.hp = std::max(fi.hp, 1);
        pl.stamina = fi.stamina;
        pl.poise = fi.poise;
        pl.charges = static_cast<std::uint8_t>(std::min(255, static_cast<int>(fi.healCharges) + fight.chairs_.healCharges()));
        fight.healMax_ = pl.charges;
        fight.allyHp_ = fight.allyMaxHp_;
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
        fight.read_.clear();
        fight.readFoe_ = kArenaNobody;
        fight.readMove_ = 0;
        fight.readDeals_ = 0;
        out = ArenaOutput{};
        fillOutput();
    }

    // ---- the output ----

    void fillOutput() {
        ArenaPlayerOut& po = out.player;
        po.velocity = pl.velocity;
        po.facing = pl.facing;
        po.drawFacing = pl.drawFacing;
        po.action = pl.action;
        po.part = ArenaPart::None;
        po.move = 0;
        po.actionTime = (pl.action == ArenaAction::Idle || pl.action == ArenaAction::Move) ? 0.f : pl.actionTime;
        if (const ArenaAttack* a = attackOf(pl, fi)) {
            po.part = arenaPartOf(*a, pl.actionTime);
            po.move = pl.move;
        }
        po.invulnerable = pl.action == ArenaAction::Riposte || pl.action == ArenaAction::Release ||
                          (pl.action == ArenaAction::Dodge && pl.actionTime >= fi.dodgeInvulnStart &&
                           pl.actionTime < fi.dodgeInvulnEnd);
        po.parrying = pl.action == ArenaAction::Guard && pl.parryLeft > 0.f;
        po.lock = pl.lock;
        po.soft = attackOf(pl, fi) != nullptr && alive(pl.soft) ? pl.soft : kArenaNobody;
        po.dodgeDir = pl.dodgeDir;
        po.backstep = pl.backstep;
        po.dodgeBack = pl.dodgeBack;
        po.load = pl.gauge.value;
        po.overloaded = overloadedNow();
        po.loadCap = fight.loadCap_;
        // A fight that is over pauses nobody (its clock stands still), nor does a dead enemy: the last blow's
        // pause, if the host wants one, is its own to play from that Hit event's attack.
        const bool fighting = fight.phase_ == ArenaPhase::Fighting;
        po.stopLeft = fighting ? std::max(0.f, pl.stopUntil - fight.time_) : 0.f;
        po.wantSpeed = fighting && po.stopLeft <= 0.f ? pl.wantSpeed : -1.f;
        {
            const float v = arenaLength(pl.velocity);
            po.stopDistance = po.wantSpeed >= 0.f && fi.moveBrake > 0.f ? v * v / (2.f * fi.moveBrake) : -1.f;
        }
        for (std::size_t i = 0; i < kMaxArenaEnemies; ++i) {
            ArenaEnemyOut& eo = out.enemies[i];
            const Enemy& foe = foes[i];
            eo.velocity = foe.velocity;
            eo.facing = foe.facing;
            eo.brain = i < n ? foe.brain : ArenaBrain::Dead;
            eo.part = ArenaPart::None;
            eo.move = 0;
            eo.stateTime = foe.stateTime;
            eo.stateLength = i < n ? recoverLength(i) : 0.f;
            eo.token = foe.token;
            eo.stopLeft = fighting && i < n && foe.brain != ArenaBrain::Dead
                              ? std::max(0.f, foe.stopUntil - fight.time_)
                              : 0.f;
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
            closeRead();
            pl.velocity = Vec2{};
            for (Enemy& foe : foes) foe.velocity = Vec2{};
            fillOutput();
            return;
        }
        pl.pos = bodies.player;
        for (std::size_t i = 0; i < n; ++i) foes[i].pos = bodies.enemies[i];
        fight.allyPos_ = bodies.ally;
        if (pl.giftLoad > 0 || pl.giftFilled) { // the host's giveLoad since the last tick
            reportGift(pl.giftLoad, pl.giftFilled);
            pl.giftLoad = 0;
            pl.giftFilled = false;
        }
        if (pl.spentLoad > 0) { // the host's spendLoad since the last tick
            emit(ArenaEventType::LoadSpent, kArenaNobody, kArenaPlayer, 0, pl.spentLoad, pl.pos);
            pl.spentLoad = 0;
        }
        if (pl.capFilled) { // a lowered cap overloaded the gauge
            emit(ArenaEventType::Overloaded, kArenaPlayer, kArenaPlayer, 0, pl.gauge.value, pl.pos);
            pl.capFilled = false;
        }
        reportHostBlows(); // the host's hostBlow since the last tick
        if (!(step > 0.f)) { // only refresh
            fillOutput();
            return;
        }
        dt = step;
        t0 = fight.time_;
        t1 = t0 + dt;
        applyHolds();
        stepPlayer(in);
        stepEnemies();
        if (fight.read_.dealt() && !readStillOpen()) closeRead(); // the active part began, or the move is gone
        endTick();
    }

    void endTick() {
        maintainLock();
        pl.parryLeft = std::max(0.f, pl.parryLeft - playerOwn); // a hit pause holds it, as the buffer
        if (pl.buffered != Press::None) pl.bufferAge += playerOwn; // a hit pause holds it
        // Every velocity is cut so the body stays inside the ring.
        pl.velocity = pl.hp > 0 ? arenaCutVelocity(fight.ring_, pl.pos, pl.velocity, dt, fi.bodyRadius) : Vec2{};
        for (std::size_t i = 0; i < n; ++i) {
            Enemy& foe = foes[i];
            foe.velocity = foe.brain == ArenaBrain::Dead
                               ? Vec2{}
                               : arenaCutVelocity(fight.ring_, foe.pos, foe.velocity, dt, def.enemies[i].bodyRadius);
        }
        // Punish windows: a run of Staggered, Open and Down counts once, from the tick it starts in.
        for (std::size_t i = 0; i < n; ++i) {
            Enemy& foe = foes[i];
            const bool down = alive(i) && !upright(foe);
            if (down && !foe.punishOpen) {
                foe.punishHit = false;
                ++stats.punishWindows;
            }
            foe.punishOpen = down;
        }
        fight.time_ = t1;
        fight.heldTime_ += playerOwn;
        stats.time += dt;
        if (pl.hp <= 0 || allDead()) closeRead();
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
        pl.soft = kArenaNobody;
        pl.homeClose = 0.f;
    }

    void spend(float cost) {
        if (cost <= 0.f) return;
        const bool had = pl.stamina > 0.f;
        pl.stamina = std::max(0.f, pl.stamina - cost);
        pl.lastSpend = t0;
        if (had && pl.stamina <= 0.f) emit(ArenaEventType::StaminaEmpty, kArenaPlayer, kArenaNobody, 0, 0, pl.pos);
    }

    // An attack `a` starts facing the aim, the lock target, the soft target, or the stick when it is pushed
    // (arenaAimSource; a riposte faces its own enemy before this). On a soft target it also sets how far the
    // attack closes in.
    void faceAtStart(const ArenaInput& in, const ArenaAttack& a) {
        pl.soft = kArenaNobody;
        pl.homeClose = 0.f;
        const Vec2 stick = arenaClampLength(in.move, 1.f);
        const ArenaSoftAim softAim = arenaSoftAimOf(fi);
        std::uint8_t pick = kArenaNobody;
        std::array<ArenaSoftBody, kMaxArenaEnemies> bodies{};
        if (!aimOn && pl.lock == kArenaNobody && softAim.range > 0.f) {
            ++stats.softChances;
            for (std::size_t i = 0; i < n; ++i)
                bodies[i] = ArenaSoftBody{foes[i].pos, def.enemies[i].bodyRadius, alive(i)};
            pick = arenaSoftTarget(pl.pos, arenaSoftDir(stick, pl.facing), bodies.data(), n, softAim);
        }
        switch (arenaAimSource(false, aimOn, pl.lock != kArenaNobody, pick != kArenaNobody, stick)) {
        case ArenaAimSource::Aim: pl.facing = aimWay; break; // where the camera aims, lock or not
        case ArenaAimSource::Lock: pl.facing = arenaUnit(arenaSub(foes[pl.lock].pos, pl.pos), pl.facing); break;
        case ArenaAimSource::Soft: {
            const ArenaHoming h =
                arenaHoming(pl.pos, fi.bodyRadius, pl.facing, bodies[pick], a.range, a.lunge, softAim);
            pl.facing = h.facing;
            pl.soft = pick;
            pl.homeClose = h.close;
            ++stats.softTargets;
            break;
        }
        case ArenaAimSource::Stick: pl.facing = arenaUnit(stick, pl.facing); break;
        default: break;
        }
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
        pl.cancelChance = false;
        emit(ArenaEventType::AttackStarted, kArenaPlayer, kArenaNobody, move, 0, pl.pos);
    }

    // An attack that costs stamina needs some to start; a free one starts whatever the stamina.
    bool affords(const ArenaAttack& a) const { return a.stamina <= 0.f || pl.stamina > 0.f; }

    // An attack under way, short of its plain cancel point (startup + active + cancel of recovery): an action
    // started now comes through one of its own cancel windows. A chained light after that point is no cancel.
    bool earlyCut() const {
        const ArenaAttack* a = attackOf(pl, fi);
        return a && pl.actionTime < a->startup + a->active + a->cancel * a->recovery;
    }

    bool startLight(const ArenaInput& in) {
        const int riposteAt = riposteTarget();
        if (riposteAt >= 0) {
            if (!affords(fi.riposte)) return false;
            pl.chainLive = false;
            pl.soft = kArenaNobody;
            pl.homeClose = 0.f;
            pl.target = id(static_cast<std::size_t>(riposteAt));
            // Turned onto its enemy over startup (advanceAttack), not in one frame; with no startup, at once.
            if (fi.riposte.startup <= kArenaEps)
                pl.facing = arenaUnit(arenaSub(foes[static_cast<std::size_t>(riposteAt)].pos, pl.pos), pl.facing);
            beginAttack(ArenaAction::Riposte, kArenaRiposteMove, fi.riposte);
            return true;
        }
        // The chain's next attack when it starts during the previous light's recovery or within chainGap after it.
        const bool chained = pl.chainLive && (pl.action == ArenaAction::Light || t0 - pl.lightEnd <= fi.chainGap);
        const std::uint8_t index = chained ? pl.chainNext : static_cast<std::uint8_t>(0);
        if (!affords(fi.light[index])) return false;
        pl.chainNext = static_cast<std::uint8_t>((index + 1u) % fi.lightCount);
        pl.chainLive = true;
        faceAtStart(in, fi.light[index]);
        ++stats.lights;
        stats.lightStamina += std::max(0.f, fi.light[index].stamina);
        beginAttack(ArenaAction::Light, index, fi.light[index]);
        return true;
    }

    // A gun kit's aimed light: one round at the aim, no riposte (that stays the unaimed light's), and it ends a chain.
    bool startShot(const ArenaInput& in) {
        if (!affords(fi.shot)) return false;
        pl.chainLive = false;
        ++stats.shots;
        faceAtStart(in, fi.shot);
        beginAttack(ArenaAction::Light, kArenaShotMove, fi.shot);
        pl.shotPitched = in.aimPitched;
        pl.shotPitch = in.aimPitch;
        return true;
    }

    bool startHeavy(const ArenaInput& in) {
        if (!affords(fi.heavy)) return false;
        pl.chainLive = false;
        faceAtStart(in, fi.heavy);
        beginAttack(ArenaAction::Heavy, kArenaHeavyMove, fi.heavy);
        return true;
    }

    bool startDodge(const ArenaInput& in) {
        if (fi.dodgeStamina > 0.f && pl.stamina <= 0.f) return false;
        const Vec2 stick = arenaClampLength(in.move, 1.f);
        if (arenaLength(stick) > kArenaEps) {
            pl.dodgeDir = arenaUnit(stick, pl.facing);
            pl.facing = pl.dodgeDir;
            pl.backstep = false;
        } else {
            pl.dodgeDir = Vec2{-pl.facing.x, -pl.facing.y}; // straight back
            pl.backstep = true;
        }
        pl.dodgeBack = false;
        if (pl.lock != kArenaNobody) {
            const Vec2 to = arenaUnit(arenaSub(foes[pl.lock].pos, pl.pos), Vec2{-pl.dodgeDir.x, -pl.dodgeDir.y});
            pl.dodgeBack = arenaDot(pl.dodgeDir, to) <= -kArenaDodgeBackCos;
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

    // The Release: open only above a third of the cap (kArenaReleaseOpenLoad at the full cap), whatever the stamina
    // (the two gauges are apart). All the Load goes at once; the strike's hits turn it into poise damage.
    bool startRelease(const ArenaInput& in) {
        if (pl.gauge.value <= fight.loadCap_ / 3) return false;
        pl.chainLive = false;
        faceAtStart(in, fi.release);
        pl.releaseSpent = pl.gauge.release();
        ++stats.releases;
        beginAttack(ArenaAction::Release, kArenaReleaseMove, fi.release);
        emit(ArenaEventType::Released, kArenaPlayer, kArenaNobody, kArenaReleaseMove, pl.releaseSpent, pl.pos);
        return true;
    }

    // A refused attack press of a kit with no attacks: cleared from the buffer at once, so it can't wait there and
    // start nothing in place of a later press.
    bool dropPress() {
        pl.buffered = Press::None;
        return false;
    }

    bool startAction(Press what, const ArenaInput& in) {
        // Overloaded: light (and so the riposte), heavy and heal are refused until a Release.
        // A kit with no attacks of its own (ArenaFighter::attacks) starts none: the press is dropped.
        const bool locked = overloadedNow();
        switch (what) {
        case Press::Light: return fi.attacks ? !locked && startLight(in) : dropPress();
        case Press::Heavy: return fi.attacks ? !locked && startHeavy(in) : dropPress();
        case Press::Shot: return fi.attacks ? !locked && startShot(in) : dropPress();
        case Press::Dodge: return startDodge(in);
        case Press::Heal: return !locked && startHeal();
        case Press::Release: return fi.attacks ? startRelease(in) : dropPress();
        default: return false;
        }
    }

    // ---- the paired counter ----

    // A parry and the partner's press within pairWindow of each other (either first) pair once: the parried enemy,
    // if it is still open, takes pairHits of light[0]'s hits and is staggered.
    void tryPair() {
        if (pl.pairFoe == kArenaNobody || std::fabs(pl.pairParry - pl.pairPress) > fi.pairWindow + kArenaEps) return;
        const std::size_t i = pl.pairFoe;
        pl.pairFoe = kArenaNobody;
        pl.pairParry = -1.0e9f;
        pl.pairPress = -1.0e9f;
        if (!alive(i) || foes[i].brain != ArenaBrain::Open) return;
        Enemy& foe = foes[i];
        const int damage = static_cast<int>(fi.pairHits) * fi.light[0].damage;
        ArenaBlowRuling r;
        if (!rule(ArenaEventType::CounterStagger, kArenaPlayer, id(i), 0, damage, def.enemies[i].poise, pl.pos,
                  pl.facing, foe.pos, r))
            return;
        const int amount = lose(i, scaledDamage(damage, r.damage));
        stats.damageDealt += amount;
        ++stats.counterStaggers;
        notePlayerBlow(i, t1);
        emit(ArenaEventType::CounterStagger, kArenaPlayer, id(i), 0, amount, foe.pos);
        if (foe.hp <= 0) killEnemy(i);
        else if (r.poise >= 1.f) staggerEnemy(i); // it breaks the whole poise or nothing (NaN: nothing)
    }

    // ---- Read ----

    // The open deal's move is still in its startup: a pick counts.
    bool readStillOpen() const {
        const std::size_t i = fight.readFoe_;
        if (!fight.read_.dealt() || !alive(i)) return false;
        const Enemy& foe = foes[i];
        return foe.brain == ArenaBrain::Attack && foe.move == fight.readMove_ &&
               foe.stateTime < def.enemies[i].moves[foe.move].attack.startup;
    }

    void closeRead() {
        fight.read_.clear();
        fight.readFoe_ = kArenaNobody;
        fight.readMove_ = 0;
    }

    // Enemy i began `move`: deal its Read cards from the living enemies' moves, unless another deal is still open.
    void dealRead(std::size_t i, std::uint8_t move) {
        if (fight.read_.dealt() && !fight.read_.picked() && readStillOpen()) return;
        closeRead();
        std::size_t count = 0, right = fight.readPool_.size();
        for (std::size_t e = 0; e < n; ++e) {
            if (!alive(e)) continue;
            const ArenaEnemyKind& k = def.enemies[e];
            for (std::size_t m = 0; m < k.moveCount; ++m) {
                if (e == i && m == move) right = count;
                fight.readPool_[count++] = ReadCard{static_cast<std::uint32_t>(e), static_cast<std::uint32_t>(m),
                                                    arenaTellName(k.moves[m].tell)};
            }
        }
        const std::uint64_t seed = static_cast<std::uint64_t>(detail::mixSeed(fight.seed_)) ^
                                   (static_cast<std::uint64_t>(++fight.readDeals_) * kArenaReadSeedStep);
        const std::size_t dealt = fight.read_.deal({fight.readPool_.data(), count}, right, seed);
        if (dealt == 0) return;
        fight.readFoe_ = id(i);
        fight.readMove_ = move;
        ++stats.readsDealt;
        emit(ArenaEventType::CardsDealt, id(i), kArenaPlayer, move, static_cast<int>(dealt), foes[i].pos);
    }

    // The partner picks card `card` of the open deal.
    void pickRead(std::size_t card) {
        if (!readStillOpen() || fight.read_.picked()) return;
        const ReadPick result = fight.read_.pick(card);
        if (result == ReadPick::Refused) return;
        const std::size_t i = fight.readFoe_;
        Enemy& foe = foes[i];
        ++stats.reads;
        if (result == ReadPick::Correct) {
            ++stats.correctReads;
            pl.readStacks = std::min(pl.readStacks + 1, kMaxReadStacks);
            foe.readRight = true;
            emit(ArenaEventType::Read, id(i), kArenaPlayer, foe.move, pl.readStacks, foe.pos);
            return;
        }
        // A misread: the stacks are lost and the attacker steadies a little.
        pl.readStacks = 0;
        const float before = foe.poise;
        foe.poise = std::max(foe.poise, std::min(def.enemies[i].poise, foe.poise + std::max(0.f, fi.readWrongPoise)));
        emit(ArenaEventType::Misread, id(i), kArenaPlayer, foe.move, static_cast<int>(std::lround(foe.poise - before)),
             foe.pos);
    }

    // The partner's Read counter on enemy i, once its read move is spent or cut short.
    void readCounter(std::size_t i) {
        Enemy& foe = foes[i];
        if (!foe.readRight) return;
        foe.readRight = false;
        if (!alive(i)) return;
        const float stacks = static_cast<float>(std::max(0, pl.readStacks - 1));
        const float power = fi.readPower * (1.f + std::max(0.f, fi.readStackPower) * stacks);
        const int damage = std::max(0, static_cast<int>(std::lround(power * static_cast<float>(fi.light[0].damage))));
        const int dealt =
            hitEnemy(i, damage, fi.readPoise, ArenaEventType::ReadCounter, static_cast<std::uint8_t>(pl.readStacks));
        if (dealt < 0) return; // vetoed
        ++stats.readCounters;
        stats.readDamage += dealt;
    }

    void stepPlayer(const ArenaInput& in) {
        aimOn = in.aim;
        // A shot keeps the pitch it started with (pitched or the flat lane) and follows the crosshair's climb until it
        // fires (the tick its startup ends takes that tick's climb), while the aim still gives one; from then on the
        // climb stays, hit or miss, through the active part.
        if (pl.action == ArenaAction::Light && pl.move == kArenaShotMove && pl.shotPitched && in.aimPitched &&
            pl.actionTime < fi.shot.startup)
            pl.shotPitch = in.aimPitch;
        aimWay = arenaUnit(in.aimDir, pl.facing);
        aimTurn = std::isfinite(in.aimTurn) ? in.aimTurn : 0.f;
        aimSpeed = std::isfinite(in.aimSpeed) ? std::clamp(in.aimSpeed, 0.f, 1.f) : 1.f;
        attackSpeed = std::isfinite(in.attackSpeed) ? std::clamp(in.attackSpeed, 0.f, 1.f) : 0.f;
        attackTurn = std::isfinite(in.attackTurn) ? std::max(in.attackTurn, 0.f) : 0.f;
        regenerate();
        handleLock(in);
        if (in.pair && pl.hp > 0) { // the partner's press: it pairs with a parry just before, or waits for one
            pl.pairPress = t0;
            tryPair();
        }
        // Take half: a press opens a window for the next blow on the ally, unless the last press was less than
        // takeHalfRearm ago (every press counts, so mashing opens nothing).
        if (in.takeHalf && pl.hp > 0 && fight.allyMaxHp_ > 0) {
            if (t0 - pl.lastTakeHalfPress >= fi.takeHalfRearm - kArenaEps) pl.takeHalfPress = t0;
            pl.lastTakeHalfPress = t0;
        }
        if (in.read >= 0 && pl.hp > 0) pickRead(static_cast<std::size_t>(in.read));

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
        // A gun kit's light while aiming is its shot, decided at the press: a buffered shot stays one if aim lets go.
        if (in.light) press = in.aim && fi.weapon == ArenaWeapon::Guns ? Press::Shot : Press::Light;
        // Aiming a gun kit is the pistol only: a heavy press is a round too.
        if (in.heavy) press = in.aim && fi.weapon == ArenaWeapon::Guns ? Press::Shot : Press::Heavy;
        if (in.heal) press = Press::Heal;
        if (in.dodge) press = Press::Dodge;
        if (in.release) press = Press::Release;
        if (press != Press::None) {
            pl.buffered = press;
            pl.bufferAge = 0.f;
        } else if (pl.bufferAge > fi.bufferTime) {
            pl.buffered = Press::None;
        }

        // Paused for the whole tick, nothing starts (a press waits in the buffer, which doesn't age) and the
        // player stands still.
        playerOwn = ownTime(pl.stopUntil);
        pl.wantSpeed = -1.f;
        if (!(playerOwn > 0.f)) {
            pl.velocity = Vec2{};
            return;
        }
        bool began = false;
        const bool inAttack = earlyCut(); // starting anything now cuts the attack before its plain cancel point
        if (inAttack && !pl.cancelChance && (pl.buffered != Press::None || in.guard)) {
            pl.cancelChance = true;
            ++stats.cancelChances;
        }
        if (pl.buffered != Press::None && canStart(pl, fi, cutOf(pl.buffered)) && startAction(pl.buffered, in)) {
            pl.buffered = Press::None;
            began = true;
            if (inAttack) ++stats.cancels;
        }
        if (!began && in.guard && pl.action != ArenaAction::Guard && canStart(pl, fi, Cut::Guard)) {
            startGuard();
            if (inAttack) ++stats.cancels;
        }

        if (playerOwn >= dt) {
            advancePlayer(in);
            return;
        }
        // A pause ends inside the tick: the player moves on by the part after it.
        const float whole = dt;
        dt = playerOwn;
        advancePlayer(in);
        dt = whole;
        pl.velocity = arenaScaled(pl.velocity, playerOwn / whole);
    }

    // ---- the hit pause ----

    // A body's own time this tick: dt less the part a hit pause ending at `until` covers.
    float ownTime(float until) const { return until <= t0 ? dt : std::clamp(t1 - until, 0.f, dt); }

    // Pauses a body for `seconds` from fight time `from` (the tick's start for one that hasn't moved this tick yet, its
    // end for one that has); pauses that overlap end at the latest.
    static void pause(float& until, float from, float seconds) {
        if (seconds > 0.f) until = std::max(until, from + seconds);
    }

    // Outside attacks the player faces the lock target, else turns toward the stick at turnRate. (Locked, the facing
    // the rules use snaps; the drawn one turns: drawFacing.)
    void faceFree(Vec2 stick, bool pushed) {
        if (aimOn) pl.facing = arenaTurn(pl.facing, aimWay, (aimTurn > 0.f ? aimTurn : fi.turnRate) * dt);
        else if (pl.lock != kArenaNobody) pl.facing = arenaUnit(arenaSub(foes[pl.lock].pos, pl.pos), pl.facing);
        else if (pushed) pl.facing = arenaTurn(pl.facing, arenaUnit(stick, pl.facing), fi.turnRate * dt);
    }

    // The velocity of a state that ramps (Idle, Move, Guard, Heal): from where it is now toward `want`, by moveAccel
    // while the wanted speed is above the current one and by moveBrake otherwise.
    Vec2 ramped(Vec2 want) {
        pl.wantSpeed = arenaLength(want);
        const float rate = pl.wantSpeed > arenaLength(pl.velocity) ? fi.moveAccel : fi.moveBrake;
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
            vel = ramped(arenaScaled(stick, fi.moveSpeed * aimSpeed));
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
        case ArenaAction::Release:
            vel = advanceAttack(stick, pushed, a0, a1);
            break;
        case ArenaAction::Dodge: {
            // A roll of dodgeDistance over dodgeTime; invulnerable from dodgeInvulnStart to dodgeInvulnEnd.
            const float length = std::max(fi.dodgeTime, kArenaEps);
            pl.invulnerable = fi.dodgeInvulnEnd > fi.dodgeInvulnStart && a0 < fi.dodgeInvulnEnd && a1 > fi.dodgeInvulnStart;
            const float moving = std::max(0.f, std::min(a1, length) - a0);
            vel = arenaScaled(pl.dodgeDir, fi.dodgeDistance / length * moving / dt);
            pl.actionTime = a1;
            if (a1 >= length) {
                endAction();
                // Locked, the rules face the target again as the roll ends, so a guard, a backstep or a riposte on the
                // first free frame faces it (the drawn body turns round: drawFacing).
                if (!aimOn && pl.lock != kArenaNobody) pl.facing = arenaUnit(arenaSub(foes[pl.lock].pos, pl.pos), pl.facing);
            }
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
        // The drawn facing. An attack's startup turns the body toward the rules' facing at half a turn per
        // kArenaAttackTurnShare of the startup (at least turnRate), easing out, and never slower than reaches it as
        // startup ends; from its active part on (the blow) it is drawn as the rules face it. A roll turns the body into its way
        // (dodgeDir; a locked one back from the target, dodgeBack, toward the target instead) at half a turn per kArenaDodgeTurnShare of the roll (at least turnRate), easing out, so its
        // start snap is drawn with the roll; anything else turns toward the rules' facing at turnRate, so a snap of
        // theirs is drawn as a turn.
        const ArenaAttack* const attack = attackOf(pl, fi);
        if (!(fi.turnRate > 0.f) || (attack && a1 >= attack->startup)) pl.drawFacing = pl.facing;
        else if (attack) {
            const float pace = std::max(fi.turnRate, kArenaPi / (kArenaAttackTurnShare * attack->startup));
            const Vec2 eased = arenaDrawTurn(pl.drawFacing, pl.facing, pace * dt);
            // catch up: a share of what is left at least as big as this frame's share of the startup still to run
            const float left = std::fabs(std::atan2(arenaCross(pl.drawFacing, pl.facing), arenaDot(pl.drawFacing, pl.facing)));
            const float need = left * dt / std::max(attack->startup - a0, dt);
            const float took = std::fabs(std::atan2(arenaCross(pl.drawFacing, eased), arenaDot(pl.drawFacing, eased)));
            pl.drawFacing = took >= need ? eased : arenaTurn(pl.drawFacing, pl.facing, need);
        } else if (pl.action == ArenaAction::Dodge)
            pl.drawFacing = arenaDrawTurn(pl.drawFacing, pl.dodgeBack ? Vec2{-pl.dodgeDir.x, -pl.dodgeDir.y} : pl.dodgeDir,
                                      std::max(fi.turnRate, kArenaPi / (kArenaDodgeTurnShare * std::max(fi.dodgeTime, dt))) * dt);
        else pl.drawFacing = arenaDrawTurn(pl.drawFacing, pl.facing, fi.turnRate * dt);
    }

    Vec2 advanceAttack(Vec2 stick, bool pushed, float a0, float a1) {
        const ArenaAttack& a = *attackOf(pl, fi);
        // During startup the attacker may still turn (at `tracking` rad/s) toward its target; after it, only on the
        // move (attackTurn).
        if (a0 < a.startup || attackTurn > 0.f) {
            Vec2 want = pl.facing;
            bool have = false;
            if (pl.action == ArenaAction::Riposte && alive(pl.target)) {
                want = arenaUnit(arenaSub(foes[pl.target].pos, pl.pos), pl.facing);
                have = true;
            } else if (aimOn) {
                want = aimWay;
                have = true;
            } else if (pl.lock != kArenaNobody) {
                want = arenaUnit(arenaSub(foes[pl.lock].pos, pl.pos), pl.facing);
                have = true;
            } else if (alive(pl.soft)) {
                want = arenaUnit(arenaSub(foes[pl.soft].pos, pl.pos), pl.facing);
                have = true;
            } else if (pushed) {
                want = arenaUnit(stick, pl.facing);
                have = true;
            }
            float tracked = a0 < a.startup ? a.tracking * (std::min(a1, a.startup) - a0) : 0.f;
            if (pl.action == ArenaAction::Riposte && have && a0 < a.startup) {
                // A riposte faces its enemy when startup ends whatever its tracking: at least this tick's share of the
                // startup left, of the angle left.
                const float left = std::abs(std::atan2(arenaCross(pl.facing, want), arenaDot(pl.facing, want)));
                tracked = std::max(tracked, left * std::min(1.f, (std::min(a1, a.startup) - a0) / (a.startup - a0)));
            }
            if (have) pl.facing = arenaTurn(pl.facing, want, std::max(tracked, attackTurn * dt));
        }
        pl.invulnerable = pl.action == ArenaAction::Riposte || pl.action == ArenaAction::Release;
        // Only the active part connects; a tick whose interval overlaps it tests, so a long frame can't skip it.
        if (a1 > a.startup && a0 < a.startup + a.active) playerStrike(a);
        // The lunge: `lunge` metres forward, spread over startup and active. A riposte's goes at its enemy, not along
        // the facing still turning onto it, so it reaches where a riposte snapped onto its enemy would.
        Vec2 vel{};
        const float span = a.startup + a.active;
        if (span > kArenaEps && a.lunge != 0.f) {
            const float moving = std::max(0.f, std::min(a1, span) - a0);
            const Vec2 way = pl.action == ArenaAction::Riposte && alive(pl.target)
                ? arenaUnit(arenaSub(foes[pl.target].pos, pl.pos), pl.facing) : pl.facing;
            vel = arenaScaled(way, a.lunge / span * moving / dt);
        }
        // Homing on a soft target: its share this tick, toward it, never past touching (it may have moved).
        if (pl.homeClose > 0.f && alive(pl.soft) && span > kArenaEps) {
            const Vec2 to = arenaSub(foes[pl.soft].pos, pl.pos);
            const float gap = arenaLength(to) - fi.bodyRadius - def.enemies[pl.soft].bodyRadius;
            const Vec2 toward = arenaUnit(to, pl.facing);
            const float lunged = std::max(0.f, arenaDot(vel, toward)) * dt; // this tick's lunge toward it
            const float step = std::min(arenaHomingStep(pl.homeClose, span, a0, a1), std::max(0.f, gap - lunged));
            if (step > 0.f) {
                const Vec2 way = arenaUnit(to, pl.facing);
                vel = Vec2{vel.x + way.x * step / dt, vel.y + way.y * step / dt};
            }
        }
        // Walking on through the attack (attackSpeed of the pace, ramped like a walk), on top of the lunge.
        // The walk ramps on its own (from the pace the attack began at), so the lunge doesn't feed it. A host that
        // moves the attack (attackTurn above 0) gets the ramp even at attackSpeed 0: the pace slides to a stop at the
        // brake rather than stopping dead; one that moves nothing (both 0) roots at once.
        if (a0 <= 0.f) pl.attackWalk = pl.velocity;
        if (attackSpeed > 0.f || attackTurn > 0.f) {
            const Vec2 want = arenaScaled(stick, fi.moveSpeed * aimSpeed * attackSpeed);
            const float rate = arenaLength(want) > arenaLength(pl.attackWalk) ? fi.moveAccel : fi.moveBrake;
            pl.attackWalk = arenaRamp(pl.attackWalk, want, rate, dt);
            vel = Vec2{vel.x + pl.attackWalk.x, vel.y + pl.attackWalk.y};
        }
        pl.actionTime = a1;
        if (a1 >= a.total()) {
            if (pl.action == ArenaAction::Light) pl.lightEnd = t1;
            endAction();
        }
        return vel;
    }

    // ---- the player's attacks on the enemies ----

    // A blow of the player's side (an attack, a partner's counter, a host's bonus) landed on enemy i at fight time
    // `at`: the pace counters.
    void notePlayerBlow(std::size_t i, float at) {
        Enemy& foe = foes[i];
        if (foe.firstBlow < 0.f) foe.firstBlow = at;
        if (foe.punishOpen && !foe.punishHit) {
            foe.punishHit = true;
            ++stats.punishUsed;
        }
    }

    // `at`: the fight time it dies at (< 0: this tick's end).
    void killEnemy(std::size_t i, bool report = true, float at = -1.f) {
        Enemy& foe = foes[i];
        if (at < 0.f) at = t1;
        ++stats.kills;
        if (foe.firstBlow >= 0.f) {
            stats.killSeconds[i] += std::max(0.f, at - foe.firstBlow);
            ++stats.killCount[i];
        }
        foe.hp = 0;
        foe.brain = ArenaBrain::Dead;
        foe.stateTime = 0.f;
        foe.velocity = Vec2{};
        foe.readRight = false;
        foe.holdFor = -1.f;
        releaseToken(i);
        if (report) emit(ArenaEventType::Downed, kArenaPlayer, id(i), 0, 0, foe.pos);
    }

    bool hostBlow(std::size_t i, int damage, std::uint8_t move) {
        if (i >= n || !alive(i) || damage <= 0) return false;
        Enemy& foe = foes[i];
        const int amount = lose(i, damage);
        stats.damageDealt += amount;
        const bool killed = foe.hp <= 0;
        notePlayerBlow(i, fight.time_); // its own Impl, outside the tick's: the fight's clock
        if (fight.inHook_.on) {
            emit(ArenaEventType::HostHit, kArenaPlayer, id(i), move, amount, foe.pos);
            if (killed) killEnemy(i, true, fight.time_);
        } else {
            foe.hostOwed += amount;
            foe.hostMove = move;
            if (killed) {
                foe.hostKilled = true;
                killEnemy(i, false, fight.time_);
            }
        }
        return true;
    }

    void reportHostBlows() {
        for (std::size_t i = 0; i < n; ++i) {
            Enemy& foe = foes[i];
            if (foe.hostOwed <= 0 && !foe.hostKilled) continue;
            emit(ArenaEventType::HostHit, kArenaPlayer, id(i), foe.hostMove, foe.hostOwed, foe.pos);
            if (foe.hostKilled) emit(ArenaEventType::Downed, kArenaPlayer, id(i), 0, 0, foe.pos);
            foe.hostOwed = 0;
            foe.hostKilled = false;
        }
    }

    void staggerEnemy(std::size_t i) {
        Enemy& foe = foes[i];
        foe.poise = def.enemies[i].poise;
        foe.breakHits = 0;
        interrupt(i, ArenaBrain::Staggered);
        ++stats.staggers;
        emit(ArenaEventType::Staggered, kArenaPlayer, id(i), 0, 0, foe.pos);
    }

    // ---- host hooks ----

    // The host's ruling on a blow (none without a hook): false when it is vetoed.
    bool rule(ArenaEventType type, std::uint8_t source, std::uint8_t target, std::uint8_t move, int damage, float poise,
              Vec2 from, Vec2 facing, Vec2 at, ArenaBlowRuling& r) const {
        r = ArenaBlowRuling{};
        if (!fight.blowHook_) return true;
        ArenaBlow b;
        b.type = type;
        b.source = source;
        b.target = target;
        b.move = move;
        b.damage = damage;
        b.poise = poise;
        b.from = from;
        b.facing = facing;
        b.at = at;
        b.time = t1;
        fight.inHook_.on = true;
        r = fight.blowHook_(fight.blowUser_, b);
        fight.inHook_.on = false;
        return !r.veto;
    }

    static int scaledDamage(int damage, float scale) {
        const float v = static_cast<float>(damage) * (scale > 0.f ? scale : 0.f); // NaN: 0
        if (!(v > 0.f)) return 0;
        return v >= kArenaMaxBlow ? static_cast<int>(kArenaMaxBlow) : static_cast<int>(std::lround(v));
    }
    static float scaledPoise(float poise, float scale) {
        const float v = poise * (scale > 0.f ? scale : 0.f);
        return v > 0.f ? std::min(v, kArenaMaxBlow) : 0.f;
    }

    // Enemy i loses up to `damage` hp, never below its floor; returns what it lost.
    int lose(std::size_t i, int damage) {
        Enemy& foe = foes[i];
        const int amount = std::clamp(damage, 0, std::max(0, foe.hp - foe.hpFloor));
        foe.hp -= amount;
        return amount;
    }

    // The host's hold requests, at the start of a tick that advances (so its events land in this tick's output).
    void applyHolds() {
        // Every request is taken first: one a blow hook makes while these apply (a cut read move's counter) waits for
        // the next tick, whichever enemy it is for.
        struct Ask {
            ArenaFight::HoldAsk ask = ArenaFight::HoldAsk::None;
            ArenaBrain brain = ArenaBrain::Open;
            float seconds = 0.f;
        };
        std::array<Ask, kMaxArenaEnemies> asks{};
        for (std::size_t i = 0; i < n; ++i) {
            asks[i] = Ask{foes[i].holdAsk, foes[i].holdBrain, foes[i].holdSeconds};
            foes[i].holdAsk = ArenaFight::HoldAsk::None;
        }
        for (std::size_t i = 0; i < n; ++i) {
            Enemy& foe = foes[i];
            const ArenaFight::HoldAsk ask = asks[i].ask;
            if (ask == ArenaFight::HoldAsk::None || !alive(i)) continue;
            if (ask == ArenaFight::HoldAsk::Release) {
                if (foe.holdFor < 0.f) continue;
                foe.holdFor = -1.f;
                foe.brain = ArenaBrain::Approach;
                foe.stateTime = 0.f;
                continue;
            }
            interrupt(i, asks[i].brain); // a read move cut short brings the partner's counter, which may kill it
            if (alive(i)) foe.holdFor = asks[i].seconds;
        }
    }

    // Enemy i loses `damage` hp (Downed at 0; never below its floor), then `poise` poise (poiseHit), as the host
    // rules. Reports `type` with the hp it lost and returns that, or -1 when the host vetoes it.
    int hitEnemy(std::size_t i, int damage, float poise, ArenaEventType type, std::uint8_t move) {
        Enemy& foe = foes[i];
        ArenaBlowRuling r;
        if (!rule(type, kArenaPlayer, id(i), move, damage, poise, pl.pos, pl.facing, foe.pos, r)) return -1;
        poise = scaledPoise(poise, r.poise);
        const int amount = lose(i, scaledDamage(damage, r.damage));
        stats.damageDealt += amount;
        notePlayerBlow(i, t1);
        emit(type, kArenaPlayer, id(i), move, amount, foe.pos);
        if (foe.hp <= 0) killEnemy(i);
        else poiseHit(i, poise);
        return amount;
    }

    // Poise: only an enemy on its feet loses it; `armor` ignores a share of it during its attack's startup and active
    // parts; at 0 it staggers.
    void poiseHit(std::size_t i, float poise) {
        Enemy& foe = foes[i];
        if (poise <= 0.f || !upright(foe)) return;
        float share = 0.f;
        if (foe.brain == ArenaBrain::Attack) {
            const ArenaAttack& own = def.enemies[i].moves[foe.move].attack;
            if (foe.stateTime < own.startup + own.active) share = std::clamp(own.armor, 0.f, 1.f);
        }
        foe.poise -= poise * (1.f - share);
        foe.lastPoiseHit = t1;
        if (foe.poise <= 0.f) staggerEnemy(i);
    }

    void playerStrike(const ArenaAttack& a) {
        const bool riposte = pl.action == ArenaAction::Riposte;
        // The enemies this tick reaches that the attack hasn't hit yet.
        std::array<std::size_t, kMaxArenaEnemies> reached{};
        std::size_t count = 0;
        if (pl.move == kArenaShotMove && pl.shotPitched) {
            // A pitched round: the first body its line enters, the ones it passes over or under not counted.
            std::array<Vec2, kMaxArenaEnemies> at{};
            std::array<bool, kMaxArenaEnemies> standing{};
            for (std::size_t i = 0; i < n; ++i) {
                at[i] = foes[i].pos;
                standing[i] = foes[i].brain != ArenaBrain::Dead;
            }
            float distance = 0.f;
            const std::uint8_t first = arenaShotFirst(a, pl.pos, pl.facing, pl.shotPitch, def.enemies.data(), at.data(),
                                                      standing.data(), n, pl.hitMask, distance);
            if (first != kArenaNobody) reached[count++] = first;
        } else {
            for (std::size_t i = 0; i < n; ++i) {
                if (foes[i].brain == ArenaBrain::Dead || (pl.hitMask & (1u << i)) != 0u) continue;
                // A shot's flat lane takes the body as drawn (scaled, as the pitched round does); other attacks the
                // kind's bodyRadius.
                const float radius = pl.move == kArenaShotMove && pl.action == ArenaAction::Light
                                         ? arenaBodyCylinder(def.enemies[i], foes[i].pos, 0.f).radius
                                         : def.enemies[i].bodyRadius;
                if (arenaReaches(pl.pos, pl.facing, a, foes[i].pos, radius)) reached[count++] = i;
            }
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
            pl.hitMask = static_cast<std::uint8_t>(pl.hitMask | (1u << i)); // spent, even when vetoed
            // A Release adds the Load it spent, times releasePoise, to the poise damage.
            const float poise = a.poiseDamage + (pl.action == ArenaAction::Release
                                                     ? static_cast<float>(pl.releaseSpent) * fi.releasePoise
                                                     : 0.f);
            ArenaBlowRuling r;
            if (!rule(ArenaEventType::Hit, kArenaPlayer, id(i), pl.move, a.damage, poise, pl.pos, pl.facing, foe.pos,
                      r))
                continue;
            const int amount = lose(i, scaledDamage(a.damage, r.damage));
            ++stats.hits;
            if (pl.move == kArenaShotMove) ++stats.shotHits;
            notePlayerBlow(i, t1);
            stats.damageDealt += amount;
            emit(ArenaEventType::Hit, kArenaPlayer, id(i), pl.move, amount, foe.pos);
            // The pause: the player has moved this tick, the enemies not yet.
            pause(pl.stopUntil, t1, a.hitStop * std::clamp(a.hitStopSelf, 0.f, 1.f));
            pause(foe.stopUntil, t0, a.hitStop);
            if (foe.hp <= 0) {
                killEnemy(i);
                continue;
            }
            if (riposte && foe.brain == ArenaBrain::Open) { // the riposte punishes the open enemy (and ends a hold)
                foe.brain = ArenaBrain::Down;
                foe.stateTime = 0.f;
                foe.holdFor = -1.f;
                ++stats.ripostes;
                continue;
            }
            const float dealt = scaledPoise(poise, r.poise);
            countBlow(i);
            poiseHit(i, dealt);
            if (!alive(i) || !upright(foe)) continue; // staggered (or the read counter it brought ended it)
            if (breakOut(i)) continue;
            // Counted here, not inside flinch(): a flinch path that never fires then reads FAIL, not n/a.
            if (couldFlinch(i, dealt)) ++stats.flinchChances;
            flinch(i, dealt);
        }
    }

    // On its feet: the states poise damage, a flinch and a break-out count in.
    static bool upright(const Enemy& foe) {
        return foe.brain == ArenaBrain::Approach || foe.brain == ArenaBrain::Strafe || foe.brain == ArenaBrain::Attack ||
               foe.brain == ArenaBrain::Flinch;
    }

    // A player's blow landed on enemy i: one more toward its break-out, while it is upright and not in that move.
    void countBlow(std::size_t i) {
        Enemy& foe = foes[i];
        const ArenaEnemyKind& k = def.enemies[i];
        if (k.breakAt == 0 || !upright(foe)) return;
        if (foe.brain == ArenaBrain::Attack && static_cast<int>(foe.move) == k.breakMove) return;
        ++stats.breakUpright;
        if (!foe.flinchAllowed) { // the host's veto bars it as it bars a flinch
            ++stats.breakVetoed;
            return;
        }
        if (t1 - foe.lastBreakHit > k.breakDecay) foe.breakHits = 0;
        if (foe.breakHits < 255) ++foe.breakHits;
        foe.lastBreakHit = t1;
        // The best run so far, compared as a share of each kind's breakAt.
        if (stats.breakRunNeed == 0 || foe.breakHits * stats.breakRunNeed > stats.breakRunBest * k.breakAt) {
            stats.breakRunBest = foe.breakHits;
            stats.breakRunNeed = k.breakAt;
        }
    }

    // The count reached breakAt: the break move starts at once, whatever the tokens and its cooldown.
    bool breakOut(std::size_t i) {
        Enemy& foe = foes[i];
        const ArenaEnemyKind& k = def.enemies[i];
        if (k.breakAt == 0 || !foe.flinchAllowed || foe.breakHits < k.breakAt) return false;
        ++stats.breakDue;
        foe.breakHits = 0; // spent, even when the move is barred
        if (k.breakMove < 0 || k.breakMove >= static_cast<int>(k.moveCount) || ((foe.moveMask >> k.breakMove) & 1u) == 0u) {
            ++stats.breakBarred;
            return false;
        }
        const std::uint8_t move = static_cast<std::uint8_t>(k.breakMove);
        interrupt(i, ArenaBrain::Approach); // a move cut short gives its token back (and a read move brings its counter)
        if (!alive(i) || foe.brain != ArenaBrain::Approach) return true;
        foe.token = true;
        ++stats.breakOuts;
        emit(ArenaEventType::BrokeOut, id(i), kArenaPlayer, move, 0, foe.pos);
        beginMove(i, move);
        return true;
    }

    // A blow that could flinch enemy i: a kind that flinches, the host allows it, and the blow carries flinchPoise.
    bool couldFlinch(std::size_t i, float poise) const {
        const ArenaEnemyKind& k = def.enemies[i];
        return k.flinchTime > 0.f && foes[i].flinchAllowed && poise >= k.flinchPoise;
    }

    // A landed blow of at least flinchPoise flinches enemy i, unless its attack is active or armoured.
    void flinch(std::size_t i, float poise) {
        Enemy& foe = foes[i];
        const ArenaEnemyKind& k = def.enemies[i];
        if (!couldFlinch(i, poise)) return;
        // No stunlock: the run of flinches starts again only after a gap with no blow that could flinch it.
        if (t1 - foe.lastFlinch > kArenaFlinchChainGap) foe.flinchChain = 0;
        foe.lastFlinch = t1;
        if (foe.flinchChain >= kArenaMaxFlinchChain) return; // the blow lands, the enemy fights on
        if (foe.brain == ArenaBrain::Attack) {
            const ArenaAttack& own = k.moves[foe.move].attack;
            const ArenaPart part = arenaPartOf(own, foe.stateTime);
            if (part == ArenaPart::Active || (part == ArenaPart::Startup && own.armor > 0.f)) return;
        }
        interrupt(i, ArenaBrain::Flinch);
        if (!alive(i) || foe.brain != ArenaBrain::Flinch) return; // the read counter it brought ended it
        ++foe.flinchChain;
        ++stats.flinches;
        emit(ArenaEventType::Flinched, kArenaPlayer, id(i), 0, 0, foe.pos);
    }

    // ---- the enemies ----

    void releaseToken(std::size_t i) {
        foes[i].token = false;
        foes[i].asked = 0.f;
    }

    // Leaves the current state for `brain` (Staggered, Open): the attack stops and the token goes back.
    // A read move cut short brings the partner's Read counter at once.
    void interrupt(std::size_t i, ArenaBrain brain) {
        Enemy& foe = foes[i];
        foe.brain = brain;
        if (!upright(foe)) foe.breakHits = 0; // Open, Down, Staggered: the count starts again once it is up
        foe.stateTime = 0.f;
        foe.stringEnd = t1;
        foe.holdFor = -1.f;
        releaseToken(i);
        readCounter(i);
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
        if (!(ownTime(foe.stopUntil) > 0.f)) return; // paused the whole tick: its brain stands still
        const ArenaEnemyKind& k = def.enemies[i];
        const float distance = arenaDistance(pl.pos, foe.pos);
        if (foe.brain == ArenaBrain::Approach && distance <= k.preferredRange + kArenaEps) { // approach() settles within kArenaEps of it
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
            asking[i] = !def.enemies[i].passive && foe.brain == ArenaBrain::Strafe
                        && t0 - foe.stringEnd >= def.enemies[i].attackDelay && ownTime(foe.stopUntil) > 0.f;
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

    // With the token, a move whose [minRange, maxRange] holds the player's distance, whose cooldown is over and whose
    // bit the host's move mask sets is picked, weighted by `weight`; when none fits, the nearest such move's band is
    // where to walk to.
    Pick pickMove(std::size_t i, float distance) {
        const Enemy& foe = foes[i];
        const ArenaEnemyKind& k = def.enemies[i];
        const auto gapOf = [&](const ArenaEnemyMove& mv) {
            return distance > mv.maxRange ? distance - mv.maxRange : (distance < mv.minRange ? mv.minRange - distance : 0.f);
        };
        const auto ready = [&](std::size_t m) {
            return k.moves[m].weight > 0.f && foe.cooldown[m] <= 0.f && ((foe.moveMask >> m) & 1u) != 0u;
        };
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
        foe.hitAlly = false;
        foe.evaded = false;
        foe.cooldown[move] = def.enemies[i].moves[move].cooldown;
        foe.readRight = false;
        emit(ArenaEventType::AttackStarted, id(i), kArenaNobody, move, 0, foe.pos);
        dealRead(i, move);
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
        foe.hitPlayer = true; // connected: parried, blocked, landed or vetoed, it is spent
        ArenaBlowRuling r;
        if (!rule(ArenaEventType::Hit, source, kArenaPlayer, foe.move, a.damage, a.poiseDamage, foe.pos, foe.facing,
                  pl.pos, r))
            return;
        const int damage = scaledDamage(a.damage, r.damage); // the ruling's damage: landed, chip and Load alike
        const float poiseDamage = scaledPoise(a.poiseDamage, r.poise);
        const Vec2 toFoe = arenaUnit(arenaSub(foe.pos, pl.pos), pl.facing);
        const bool guarding = pl.action == ArenaAction::Guard && arenaDot(pl.facing, toFoe) >= fi.guardArcCos;
        if (guarding && pl.parryLeft > 0.f && mv.parryable) {
            // No damage, no stamina cost: the enemy is open. The parry is the perfect guard: it stores the blow as Load.
            ++stats.parries;
            emit(ArenaEventType::Parried, source, kArenaPlayer, foe.move, 0, pl.pos);
            const std::uint8_t move = foe.move;
            interrupt(i, ArenaBrain::Open);
            pause(pl.stopUntil, t1, fi.parryStop);
            pause(foe.stopUntil, t1, fi.parryStop);
            storeLoad(source, move, damage);
            pl.pairParry = t0;
            pl.pairFoe = source;
            tryPair();
            return;
        }
        if (guarding && !mv.unblockable) {
            const float before = pl.stamina;
            pl.stamina = std::max(0.f, before - a.guardDamage);
            pl.lastSpend = t0;
            ++stats.blocks;
            emit(ArenaEventType::Blocked, source, kArenaPlayer, foe.move,
                 static_cast<int>(std::lround(before - pl.stamina)), pl.pos);
            // Carrying, a block stores a share of the blocked damage (a guard break too).
            if (fight.carry_) storeCapped(source, foe.move, static_cast<float>(damage) * fi.blockLoadShare, stats.blockLoad);
            const int chip = std::max(0, static_cast<int>(std::lround(damage * (1.f - fi.guardAbsorb))));
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
        damagePlayer(source, damage, foe.move);
        pause(pl.stopUntil, t1, a.hitStop); // both have moved this tick
        pause(foe.stopUntil, t1, a.hitStop * std::clamp(a.hitStopSelf, 0.f, 1.f));
        if (pl.hp <= 0) return;
        // Poise: a player in hitstun or guard break is already down; `armor` ignores a share of it during the player's
        // own attack's startup and active parts.
        if (pl.action == ArenaAction::Hitstun || pl.action == ArenaAction::GuardBreak || poiseDamage <= 0.f) return;
        float share = 0.f;
        if (const ArenaAttack* own = attackOf(pl, fi)) {
            if (pl.actionTime < own->startup + own->active) share = std::clamp(own->armor, 0.f, 1.f);
        }
        pl.poise -= poiseDamage * (1.f - share);
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

    // ---- Load ----

    bool overloadedNow() const { return pl.gauge.value >= fight.loadCap_; }

    // The chairs' Load gain: 1 + their loadGain while the ally is alive within their radius of the player, else 1.
    float loadGain() const {
        const ArenaChairs& c = fight.chairs_;
        const float extra = c.loadGain();
        if (!(extra > 0.f) || fight.allyMaxHp_ <= 0 || fight.allyHp_ <= 0) return 1.f;
        return arenaDistance(pl.pos, fight.allyPos_) <= c.loadGainRadius() + kArenaEps ? 1.f + extra : 1.f;
    }

    // The one capped store: max(0, min(lround(raw x gain), cap - load)), counted in `stat` and reported as LoadStored
    // from `source`'s `move`; Overloaded comes once, as the gauge reaches the cap. A kit with no attacks stores none:
    // it has no Release to spend it, and a full gauge would lock its heal for good. Returns what was stored.
    int storeCapped(std::uint8_t source, std::uint8_t move, float raw, int& stat) {
        if (!fi.attacks || !(raw > 0.f)) return 0;
        const bool was = overloadedNow();
        const float gained = std::min(raw * loadGain(), static_cast<float>(kMaxLoad));
        const int room = std::max(0, fight.loadCap_ - pl.gauge.value);
        const int stored = pl.gauge.store(std::min(static_cast<int>(std::lround(gained)), room));
        if (stored > 0) {
            stat += stored;
            emit(ArenaEventType::LoadStored, source, kArenaPlayer, move, stored, pl.pos);
        }
        if (!was && overloadedNow()) {
            ++stats.overloads;
            emit(ArenaEventType::Overloaded, kArenaPlayer, kArenaPlayer, 0, pl.gauge.value, pl.pos);
        }
        return stored;
    }

    // A parry stores the parried attack's damage times loadShare.
    void storeLoad(std::uint8_t source, std::uint8_t move, int damage) {
        storeCapped(source, move, static_cast<float>(damage) * fi.loadShare, stats.loadStored);
    }

    // The host's Load (giveLoad): stored up to the cap with no gain, counted and shown now. Inside the blow hook its
    // events join this tick's; otherwise they wait for the next tick, which reports them first.
    int giveLoad(int amount) {
        if (!fi.attacks) return 0;
        const bool was = overloadedNow();
        const int room = std::max(0, fight.loadCap_ - pl.gauge.value);
        const int stored = pl.gauge.store(std::min(amount, room));
        const bool filled = !was && overloadedNow();
        if (filled) ++stats.overloads;
        out.player.load = pl.gauge.value;
        out.player.overloaded = overloadedNow();
        if (fight.inHook_.on) {
            reportGift(stored, filled);
        } else {
            pl.giftLoad += stored;
            pl.giftFilled = pl.giftFilled || filled;
        }
        return stored;
    }

    void reportGift(int stored, bool filled) {
        if (stored > 0) emit(ArenaEventType::LoadStored, kArenaNobody, kArenaPlayer, 0, stored, pl.pos);
        if (filled) emit(ArenaEventType::Overloaded, kArenaPlayer, kArenaPlayer, 0, pl.gauge.value, pl.pos);
    }

    // The host's spendLoad: removed now; LoadSpent at once inside the blow hook, else ahead of the next tick's events.
    int spendLoad(int amount) {
        const int take = std::min(amount, pl.gauge.value);
        if (take <= 0) return 0;
        pl.gauge.value -= take;
        out.player.load = pl.gauge.value;
        out.player.overloaded = overloadedNow();
        if (fight.inHook_.on) emit(ArenaEventType::LoadSpent, kArenaNobody, kArenaPlayer, 0, take, pl.pos);
        else pl.spentLoad += take;
        return take;
    }

    // The host's setLoadCap: the Load stays; reaching the new cap overloads once.
    void setLoadCap(int cap) {
        const bool was = overloadedNow();
        fight.loadCap_ = std::clamp(cap, 1, kMaxLoad);
        out.player.loadCap = fight.loadCap_;
        out.player.overloaded = overloadedNow();
        if (fight.phase_ != ArenaPhase::Fighting || was || !overloadedNow()) return;
        ++stats.overloads;
        if (fight.inHook_.on) emit(ArenaEventType::Overloaded, kArenaPlayer, kArenaPlayer, 0, pl.gauge.value, pl.pos);
        else pl.capFilled = true;
    }

    // ---- the ally ----

    bool allyUp() const { return fight.allyMaxHp_ > 0 && fight.allyHp_ > 0; }

    // One of enemy i's attacks reaches the ally: the ruling, then take half if a press is waiting for it.
    void enemyStrikeAlly(std::size_t i) {
        Enemy& foe = foes[i];
        const ArenaAttack& a = def.enemies[i].moves[foe.move].attack;
        const std::uint8_t source = id(i);
        foe.hitAlly = true; // connected (or vetoed): spent on the ally
        ArenaBlowRuling r;
        if (!rule(ArenaEventType::Hit, source, kArenaAlly, foe.move, a.damage, a.poiseDamage, foe.pos, foe.facing,
                  fight.allyPos_, r))
            return;
        const int damage = scaledDamage(a.damage, r.damage);
        if (damage <= 0) return;
        int allyPart = damage;
        // Take half: a press within the window before this blow, the player near enough and free to take it. Not
        // while invulnerable (a dodge's i-frames, a riposte, a Release: the blow would skip guard, poise and hurt),
        // nor while hurt (hitstun) or guard-broken.
        const float window = fi.takeHalfWindow + fight.chairs_.takeHalfWindow();
        const bool pressed = pl.takeHalfPress >= t1 - window - kArenaEps && pl.takeHalfPress <= t1;
        const bool canTake = pl.hp > 0 && !pl.invulnerable && pl.action != ArenaAction::Downed &&
                             pl.action != ArenaAction::Release && pl.action != ArenaAction::Hitstun &&
                             pl.action != ArenaAction::GuardBreak;
        if (pressed && canTake && arenaDistance(pl.pos, fight.allyPos_) <= fi.takeHalfReach + kArenaEps) {
            pl.takeHalfPress = -1.0e9f; // used
            const int lead = (damage + 1) / 2;
            allyPart = damage - lead;
            const int taken = std::min(lead, pl.hp);
            pl.hp -= taken;
            stats.damageTaken += taken;
            ++stats.takeHalves;
            emit(ArenaEventType::TookHalf, source, kArenaPlayer, foe.move, taken, pl.pos);
            if (pl.hp <= 0) {
                pl.hp = 0;
                pl.action = ArenaAction::Downed;
                pl.actionTime = 0.f;
                pl.velocity = Vec2{};
                pl.buffered = Press::None;
                pl.parryLeft = 0.f;
                emit(ArenaEventType::Downed, source, kArenaPlayer, foe.move, 0, pl.pos);
            } else {
                storeCapped(source, foe.move, static_cast<float>(lead) * fi.takeHalfLoadShare, stats.takeHalfLoad);
            }
        }
        const int lost = std::min(allyPart, fight.allyHp_);
        fight.allyHp_ -= lost;
        if (lost > 0) emit(ArenaEventType::AllyHit, source, kArenaAlly, foe.move, lost, fight.allyPos_);
        if (fight.allyHp_ <= 0) {
            fight.allyHp_ = 0;
            emit(ArenaEventType::AllyDowned, source, kArenaAlly, foe.move, 0, fight.allyPos_);
            return;
        }
        // A takeHalf chair heals the ally a share of its max hp each time the player takes half (never past it).
        if (allyPart < damage) {
            const int heal = static_cast<int>(std::lround(fight.chairs_.takeHalfHeal() * static_cast<float>(fight.allyMaxHp_)));
            fight.allyHp_ = std::min(fight.allyMaxHp_, fight.allyHp_ + std::max(heal, 0));
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
        if (a0 < a.startup && !foe.facingHeld)
            foe.facing = arenaTurn(foe.facing, dir, a.tracking * (std::min(a1, a.startup) - a0));
        if (!foe.hitPlayer && pl.hp > 0 && a1 > a.startup && a0 < a.startup + a.active &&
            arenaReaches(foe.pos, foe.facing, a, pl.pos, fi.bodyRadius)) {
            enemyStrike(i);
            if (foe.brain != ArenaBrain::Attack) return Vec2{}; // parried: the attack is over
        }
        // The ally, tested as the player is (it never guards or rolls).
        if (!foe.hitAlly && allyUp() && a1 > a.startup && a0 < a.startup + a.active &&
            arenaReaches(foe.pos, foe.facing, a, fight.allyPos_, fight.allyRadius_))
            enemyStrikeAlly(i);
        Vec2 vel{};
        const float span = a.startup + a.active;
        if (span > kArenaEps && a.lunge != 0.f) {
            const float moving = std::max(0.f, std::min(a1, span) - a0);
            vel = arenaScaled(foe.facing, a.lunge / span * moving / dt);
        }
        foe.stateTime = a1;
        if (foe.readRight && a1 >= span) { // the read move is spent: the partner's counter
            readCounter(i);
            if (foe.brain != ArenaBrain::Attack) return Vec2{}; // it staggered or died
        }
        if (a1 >= a.total()) endMove(i);
        return vel;
    }

    Vec2 approach(std::size_t i, Vec2 dir, float distance, const Pick& pick) {
        Enemy& foe = foes[i];
        const ArenaEnemyKind& k = def.enemies[i];
        foe.stateTime += dt;
        if (!foe.turnHeld) foe.facing = arenaTurn(foe.facing, dir, k.turnRate * dt);
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
        if (!foe.turnHeld) foe.facing = arenaTurn(foe.facing, dir, k.turnRate * dt);
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

    // How long enemy i's Staggered, Open or Down lasts in all: the host's hold while one runs, else the kind's time;
    // a Flinch, flinchTime. 0 in any other state.
    float recoverLength(std::size_t i) const {
        const Enemy& foe = foes[i];
        const ArenaEnemyKind& k = def.enemies[i];
        if (foe.brain == ArenaBrain::Flinch) return k.flinchTime;
        if (foe.brain != ArenaBrain::Staggered && foe.brain != ArenaBrain::Open && foe.brain != ArenaBrain::Down) return 0.f;
        return foe.holdFor >= 0.f                ? foe.holdFor // the host's hold
               : foe.brain == ArenaBrain::Staggered ? k.staggerTime
               : foe.brain == ArenaBrain::Open      ? k.openTime
                                                    : k.downTime;
    }

    void recover(std::size_t i) {
        Enemy& foe = foes[i];
        const float length = recoverLength(i);
        foe.stateTime += dt;
        if (foe.stateTime >= length) {
            foe.brain = ArenaBrain::Approach;
            foe.stateTime = 0.f;
            foe.holdFor = -1.f;
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
        case ArenaBrain::Flinch: {
            // Pushed straight away from the player over the flinch.
            const ArenaEnemyKind& k = def.enemies[i];
            const float length = std::max(k.flinchTime, kArenaEps);
            const float moving = std::max(0.f, std::min(foe.stateTime + dt, length) - foe.stateTime);
            vel = arenaScaled(Vec2{-dir.x, -dir.y}, k.flinchPush / length * moving / dt);
            recover(i);
            break;
        }
        case ArenaBrain::Dead: break;
        }
        foe.velocity = vel;
    }

    void stepEnemies() {
        for (std::size_t i = 0; i < n; ++i) enemyTimers(i);
        for (std::size_t i = 0; i < n; ++i) enemyStance(i);
        assignTokens();
        for (std::size_t i = 0; i < n; ++i) {
            // A hit pause stops its own time: paused for the whole tick it does nothing and stands still; one that
            // ends inside the tick moves it on by the part after.
            Enemy& foe = foes[i];
            const float own = ownTime(foe.stopUntil);
            if (own >= dt || foe.brain == ArenaBrain::Dead) {
                enemyAct(i);
            } else if (own > 0.f) {
                const float whole = dt;
                dt = own;
                enemyAct(i);
                dt = whole;
                foe.velocity = arenaScaled(foe.velocity, own / whole);
            } else {
                foe.velocity = Vec2{};
            }
        }
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
        // The partner reads the open deal: the right card, a moment into the startup.
        if (self.read_.dealt() && !self.read_.picked() && self.readFoe_ < count) {
            const ArenaFight::EnemyState& foe = self.enemy_[self.readFoe_];
            const float startup = self.def_.enemies[self.readFoe_].moves[self.readMove_].attack.startup;
            if (foe.brain == ArenaBrain::Attack && foe.move == self.readMove_ && foe.stateTime < startup &&
                foe.stateTime >= std::min(kArenaAutoReadDelay, 0.5f * startup))
                in.read = static_cast<std::int8_t>(self.read_.correct());
        }
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
            } else if (p.action != ArenaAction::Dodge && (f.dodgeStamina <= 0.f || p.stamina > 0.f)) {
                if (soonest <= 0.5f * (f.dodgeInvulnStart + f.dodgeInvulnEnd)) {
                    in.dodge = true;
                    in.move = sideStep(self, foe.pos);
                }
            }
            return in;
        }

        // Heal when low, and nothing is near or winding up.
        const bool locked = p.gauge.value >= self.loadCap_; // Overloaded: no heal, no attack but the Release
        if (!locked && p.charges > 0 && p.hp * 100 < kArenaAutoHealPercent * f.hp && !windingUp) {
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

        // Overloaded: close on the target (the locked enemy, else the nearest) and release on it.
        if (locked) {
            const std::size_t target = (p.lock != kArenaNobody && living(p.lock)) ? static_cast<std::size_t>(p.lock)
                                                                               : static_cast<std::size_t>(nearest);
            const Vec2 to = arenaSub(self.enemy_[target].pos, p.pos);
            if (arenaLength(to) > (f.release.range + self.def_.enemies[target].bodyRadius) * kArenaAutoStand)
                in.move = arenaUnit(to, p.facing);
            else
                in.release = true;
            return in;
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
        // Enough Load and an upright target in the Release's reach: spend it as poise damage.
        if (p.gauge.value >= std::min(kArenaAutoReleaseAt, self.loadCap_) && !down(target) &&
            d <= (f.release.range + self.def_.enemies[target].bodyRadius) * kArenaAutoStand) {
            in.release = true;
            return in;
        }
        const float reach = range + self.def_.enemies[target].bodyRadius;
        const bool farOff = d > reach * kArenaAutoStand;
        if (farOff) in.move = arenaUnit(to, p.facing);
        // Opted in, a guns kit shoots on the way in: aimed at the target, in the shot's range, a dodge kept in reserve.
        if (farOff && self.autoShoot_ && f.weapon == ArenaWeapon::Guns && f.attacks &&
            d <= f.shot.range + self.def_.enemies[target].bodyRadius && p.action != ArenaAction::Light &&
            (f.shot.stamina <= 0.f || p.stamina >= f.shot.stamina + f.dodgeStamina)) {
            in.aim = true;
            in.aimDir = arenaUnit(to, p.facing);
            in.light = true;
        }
        if (!farOff) {
            bool danger = false; // an enemy that can still act: keep a dodge in reserve
            for (std::size_t i = 0; i < count; ++i)
                if (living(i) && !down(i)) danger = true;
            // A free light is always afforded; one that costs keeps a dodge in reserve.
            if (cost <= 0.f || (p.stamina > 0.f && p.stamina >= cost + (danger ? f.dodgeStamina : 0.f))) in.light = true;
        }
        return in;
    }
};

// ---- ArenaFight ----

void ArenaFight::begin(const ArenaDef& d, const ArenaRing& where, Vec2 playerPos, Vec2 playerFacing, std::uint32_t seed) {
    if (inHook_.on) return;
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
    if (inHook_.on || phase_ == ArenaPhase::NotStarted) return;
    ++stats_.restarts;
    ArenaFightImpl(*this).reset();
}

const ArenaOutput& ArenaFight::tick(float dt, const ArenaInput& input, const ArenaBodies& bodies) {
    if (inHook_.on) return out_;
    ArenaFightImpl(*this).tick(dt, input, bodies);
    return out_;
}

ArenaInput ArenaFight::autopilot() const { return ArenaFightImpl::autopilot(*this); }
void ArenaFight::setAutopilotShoots(bool on) { autoShoot_ = on; }
bool ArenaFight::autopilotShoots() const { return autoShoot_; }

ArenaPhase ArenaFight::phase() const { return phase_; }
float ArenaFight::time() const { return time_; }
float ArenaFight::heldTime() const { return heldTime_; }
float ArenaFight::hitStopLeft() const { return output().player.stopLeft; }
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
int ArenaFight::load() const { return player_.gauge.value; }
bool ArenaFight::overloaded() const { return player_.gauge.value >= loadCap_; }
float ArenaFight::stamina() const { return player_.stamina; }
float ArenaFight::poise() const { return player_.poise; }
std::uint8_t ArenaFight::healCharges() const { return player_.charges; }
std::uint8_t ArenaFight::lock() const { return player_.lock; }
int ArenaFight::enemyHp(std::size_t i) const { return i < enemyCount_ ? enemy_[i].hp : 0; }
float ArenaFight::enemyPoise(std::size_t i) const { return i < enemyCount_ ? enemy_[i].poise : 0.f; }
bool ArenaFight::enemyAlive(std::size_t i) const { return i < enemyCount_ && enemy_[i].brain != ArenaBrain::Dead; }
std::uint8_t ArenaFight::shotMeets(Vec2 dir, float pitch, float* distance, std::uint32_t skip) const {
    float d = 0.f;
    std::uint8_t first = kArenaNobody;
    if (def_.fighter.weapon == ArenaWeapon::Guns && def_.fighter.attacks) {
        std::array<Vec2, kMaxArenaEnemies> at{};
        std::array<bool, kMaxArenaEnemies> standing{};
        for (std::size_t i = 0; i < enemyCount_; ++i) {
            at[i] = enemy_[i].pos;
            standing[i] = enemy_[i].brain != ArenaBrain::Dead;
        }
        first = arenaShotFirst(def_.fighter.shot, player_.pos, arenaUnit(dir, player_.facing), pitch,
                               def_.enemies.data(), at.data(), standing.data(), enemyCount_, skip, d);
    }
    if (distance) *distance = d;
    return first;
}
const ReadCards& ArenaFight::readCards() const { return read_; }
std::uint8_t ArenaFight::readFoe() const { return readFoe_; }
std::uint8_t ArenaFight::readMove() const { return readMove_; }
bool ArenaFight::readOpen() const {
    if (!read_.dealt() || read_.picked() || readFoe_ >= enemyCount_) return false;
    const EnemyState& foe = enemy_[readFoe_];
    return foe.brain == ArenaBrain::Attack && foe.move == readMove_ &&
           foe.stateTime < def_.enemies[readFoe_].moves[readMove_].attack.startup;
}
int ArenaFight::readStacks() const { return player_.readStacks; }

bool ArenaFight::holdEnemy(std::size_t i, ArenaBrain brain, float seconds) {
    if (phase_ != ArenaPhase::Fighting || !enemyAlive(i) ||
        (brain != ArenaBrain::Open && brain != ArenaBrain::Down && brain != ArenaBrain::Staggered) || !(seconds > 0.f))
        return false;
    EnemyState& foe = enemy_[i];
    foe.holdAsk = HoldAsk::Hold;
    foe.holdBrain = brain;
    foe.holdSeconds = seconds;
    return true;
}
void ArenaFight::releaseEnemy(std::size_t i) {
    if (i < enemyCount_) enemy_[i].holdAsk = HoldAsk::Release;
}
bool ArenaFight::enemyHeld(std::size_t i) const { return enemyAlive(i) && enemy_[i].holdFor >= 0.f; }
void ArenaFight::setEnemyHpFloor(std::size_t i, int hpFloor) {
    if (i < enemyCount_) enemy_[i].hpFloor = std::max(0, hpFloor);
}
int ArenaFight::enemyHpFloor(std::size_t i) const { return i < enemyCount_ ? enemy_[i].hpFloor : 0; }
static_assert(kMaxArenaMoves <= 32, "a move mask holds one bit per move");
void ArenaFight::setEnemyMoveMask(std::size_t i, std::uint32_t mask) {
    if (i < enemyCount_) enemy_[i].moveMask = mask;
}
std::uint32_t ArenaFight::enemyMoveMask(std::size_t i) const { return i < enemyCount_ ? enemy_[i].moveMask : 0u; }
void ArenaFight::setEnemyFacingHeld(std::size_t i, bool held) {
    if (i < enemyCount_) enemy_[i].facingHeld = held;
}
bool ArenaFight::enemyFacingHeld(std::size_t i) const { return i < enemyCount_ && enemy_[i].facingHeld; }
void ArenaFight::setEnemyTurnHeld(std::size_t i, bool held) {
    if (i < enemyCount_) enemy_[i].turnHeld = held;
}
bool ArenaFight::enemyTurnHeld(std::size_t i) const { return i < enemyCount_ && enemy_[i].turnHeld; }
void ArenaFight::setEnemyFlinch(std::size_t i, bool allowed) {
    if (i < enemyCount_) enemy_[i].flinchAllowed = allowed;
}
bool ArenaFight::enemyFlinch(std::size_t i) const { return i < enemyCount_ && enemy_[i].flinchAllowed; }
void ArenaFight::setEnemyFacing(std::size_t i, Vec2 facing) {
    if (i < enemyCount_ && arenaLength(facing) > kArenaEps) enemy_[i].facing = arenaUnit(facing, enemy_[i].facing);
}
void ArenaFight::setBlowHook(ArenaBlowHook hook, void* user) {
    blowHook_ = hook;
    blowUser_ = hook ? user : nullptr;
}
ArenaBlowHook ArenaFight::blowHook() const { return blowHook_; }
void* ArenaFight::blowHookUser() const { return blowUser_; }
bool ArenaFight::hostBlow(std::size_t enemy, int damage, std::uint8_t move) {
    if (phase_ != ArenaPhase::Fighting) return false;
    return ArenaFightImpl(*this).hostBlow(enemy, damage, move);
}
bool ArenaFight::ruledHostBlow(std::size_t enemy, int damage, std::uint8_t move, ArenaBlowHook hook, void* user) {
    if (phase_ != ArenaPhase::Fighting) return false;
    ArenaFightImpl impl(*this);
    if (!hook || !impl.alive(enemy) || damage <= 0) return impl.hostBlow(enemy, damage, move);
    ArenaBlow b;
    b.type = ArenaEventType::HostHit;
    b.source = kArenaPlayer;
    b.target = static_cast<std::uint8_t>(enemy);
    b.move = move;
    b.damage = damage;
    b.poise = 0.f;
    b.from = player_.pos;
    b.facing = player_.facing;
    b.at = enemy_[enemy].pos;
    b.time = time_;
    const bool wasIn = inHook_.on;
    inHook_.on = true;
    const ArenaBlowRuling r = hook(user, b);
    inHook_.on = wasIn;
    if (r.veto || phase_ != ArenaPhase::Fighting) return false;
    const int scaled = ArenaFightImpl::scaledDamage(damage, r.damage);
    return scaled > 0 && impl.hostBlow(enemy, scaled, move);
}
int ArenaFight::giveLoad(int amount) {
    if (phase_ != ArenaPhase::Fighting) return 0;
    return ArenaFightImpl(*this).giveLoad(amount);
}

void ArenaFight::setLoadCap(int cap) { ArenaFightImpl(*this).setLoadCap(cap); }
int ArenaFight::loadCap() const { return loadCap_; }
int ArenaFight::releaseOpenLoad() const { return loadCap_ / 3; }
int ArenaFight::spendLoad(int amount) {
    if (phase_ != ArenaPhase::Fighting || amount <= 0) return 0;
    return ArenaFightImpl(*this).spendLoad(amount);
}
void ArenaFight::setCarry(bool blocksStore) { carry_ = blocksStore; }
bool ArenaFight::carry() const { return carry_; }
void ArenaFight::setChairs(const ArenaChairs& chairs) {
    chairs_ = chairs;
    chairs_.count = static_cast<std::uint8_t>(std::min<std::size_t>(chairs_.count, chairs_.named.size()));
}
const ArenaChairs& ArenaFight::chairs() const { return chairs_; }
std::uint8_t ArenaFight::healChargesMax() const { return healMax_; }
void ArenaFight::setAlly(int maxHp, float radius) {
    allyMaxHp_ = std::max(maxHp, 0);
    allyHp_ = allyMaxHp_;
    allyRadius_ = std::isfinite(radius) && radius > 0.f ? radius : 0.4f;
}
int ArenaFight::allyHp() const { return allyHp_; }
int ArenaFight::allyMaxHp() const { return allyMaxHp_; }
bool ArenaFight::allyAlive() const { return allyMaxHp_ > 0 && allyHp_ > 0; }
Vec2 ArenaFight::allyPos() const { return allyPos_; }
bool ArenaFight::takeHalfDue(float lead) const {
    if (phase_ != ArenaPhase::Fighting || !allyAlive() || player_.hp <= 0) return false;
    if (arenaDistance(player_.pos, allyPos_) > def_.fighter.takeHalfReach) return false;
    for (std::size_t i = 0; i < enemyCount_; ++i) {
        const EnemyState& foe = enemy_[i];
        if (foe.brain != ArenaBrain::Attack) continue;
        const float left = def_.enemies[i].moves[foe.move].attack.startup - foe.stateTime;
        if (left >= 0.f && left <= lead) return true;
    }
    return false;
}

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
    case ArenaAction::Release: return "Release";
    }
    return "";
}

int arenaWholeBeats(float seconds, float bpm) {
    if (!(bpm > 0.f) || !(seconds > 0.f)) return -1;
    const float beat = 60.f / bpm;
    const long beats = std::lround(seconds / beat);
    if (beats < 1 || std::fabs(seconds - static_cast<float>(beats) * beat) > kArenaBeatTolerance) return -1;
    return static_cast<int>(beats);
}

const char* arenaWeaponName(ArenaWeapon w) {
    switch (w) {
    case ArenaWeapon::None: return "none";
    case ArenaWeapon::Gauntlets: return "gauntlets";
    case ArenaWeapon::Guns: return "guns";
    case ArenaWeapon::Magic: return "magic";
    }
    return "";
}

std::size_t arenaStrikePattern(const ArenaFighter& kit, FightClip* out, std::size_t cap) {
    if (!out) return 0;
    const std::size_t count = std::min<std::size_t>({static_cast<std::size_t>(kit.lightCount), kMaxLightChain, cap});
    std::size_t punches = 0, kicks = 0;
    for (std::size_t i = 0; i < count; ++i) {
        FightClip c = FightClip::Cast;
        if (kit.weapon == ArenaWeapon::Guns) {
            c = (kicks++ % 2u == 0u) ? FightClip::KickLeft : FightClip::KickRight;
        } else if (kit.weapon != ArenaWeapon::Magic) {
            c = (punches++ % 2u == 0u) ? FightClip::StrikeLeft : FightClip::StrikeRight;
        }
        out[i] = c;
    }
    return count;
}

FightClip arenaHeavyStrike(const ArenaFighter& kit) {
    switch (kit.weapon) {
    case ArenaWeapon::Magic: return FightClip::Cast;
    case ArenaWeapon::Guns: return FightClip::KickRight;
    case ArenaWeapon::None:
    case ArenaWeapon::Gauntlets: break;
    }
    return FightClip::StrikeRight;
}

const ArenaAttack* arenaPlayerAttack(const ArenaFighter& kit, std::uint8_t move) {
    switch (move) {
    case kArenaHeavyMove: return &kit.heavy;
    case kArenaRiposteMove: return &kit.riposte;
    case kArenaReleaseMove: return &kit.release;
    case kArenaShotMove: return kit.weapon == ArenaWeapon::Guns ? &kit.shot : nullptr;
    default: break;
    }
    if (move < kit.lightCount && static_cast<std::size_t>(move) < kMaxLightChain) return &kit.light[move];
    return nullptr;
}

FightClip arenaPlayerStrike(const ArenaFighter& kit, std::uint8_t move) {
    switch (move) {
    case kArenaHeavyMove: return arenaHeavyStrike(kit);
    case kArenaRiposteMove: return FightClip::Counter;
    case kArenaReleaseMove: return FightClip::Finisher;
    case kArenaShotMove: return kit.weapon == ArenaWeapon::Guns ? FightClip::WeaponRight : FightClip::Stance;
    default: break;
    }
    if (move >= kit.lightCount || static_cast<std::size_t>(move) >= kMaxLightChain) return FightClip::Stance;
    FightClip p[kMaxLightChain];
    const std::size_t n = arenaStrikePattern(kit, p, kMaxLightChain);
    return static_cast<std::size_t>(move) < n ? p[move] : FightClip::Stance;
}

const char* arenaLeadClipId(FightClip clip) {
    switch (clip) {
    case FightClip::StrikeLeft: return "lead_jab";
    case FightClip::StrikeRight: return "lead_cross";
    case FightClip::KickRight: return "lead_kick_r";
    case FightClip::Counter: return "lead_counter";
    case FightClip::Finisher: return "lead_finisher";
    case FightClip::WeaponRight: return "lead_fire";
    case FightClip::Cast: return "lead_cast";
    default: return nullptr;
    }
}

const char* arenaTellName(ArenaTell t) {
    switch (t) {
    case ArenaTell::Strike: return "strike";
    case ArenaTell::Overhead: return "overhead";
    case ArenaTell::Backhand: return "backhand";
    case ArenaTell::Charge: return "charge";
    case ArenaTell::Thrust: return "thrust";
    case ArenaTell::Slash: return "slash";
    case ArenaTell::Sweep: return "sweep";
    case ArenaTell::Slam: return "slam";
    case ArenaTell::Shot: return "shot";
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
    case ArenaBrain::Flinch: return "Flinch";
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
    ArenaAttack& rel = f.release; // the Release: shaped like the riposte; its poise damage is the Load it spends
    rel.startup = 0.2f; rel.active = 0.15f; rel.recovery = 0.5f; rel.range = 2.6f; rel.arcCos = 0.f;
    rel.damage = 30; rel.poiseDamage = 0.f; rel.lunge = 0.5f; rel.tracking = 10.f; rel.stamina = 0.f; rel.cancel = 0.5f;
    rel.maxTargets = 1;

    // Enemy startups are whole beats at 150 bpm (0.4 s each).
    def.bpm = 150.f;

    // One enemy with a quick strike (move 0) and a slow two-hit string (moves 1 and 2).
    ArenaEnemyKind& k = def.enemies[0];
    k.hp = 150;
    k.poise = 48.f;
    k.accel = 8.f;
    k.moveCount = 3;
    ArenaEnemyMove& strike = k.moves[0];
    strike.attack.startup = 0.4f; strike.attack.active = 0.15f; strike.attack.recovery = 0.6f; strike.attack.range = 2.2f;
    strike.attack.damage = 12; strike.attack.poiseDamage = 12.f; strike.attack.guardDamage = 16.f; strike.attack.lunge = 0.6f;
    strike.attack.tracking = 4.f;
    strike.minRange = 0.f; strike.maxRange = 2.4f; strike.weight = 1.f; strike.tell = ArenaTell::Thrust;
    ArenaEnemyMove& first = k.moves[1];
    first.attack.startup = 0.8f; first.attack.active = 0.2f; first.attack.recovery = 0.5f; first.attack.range = 2.4f;
    first.attack.damage = 16; first.attack.poiseDamage = 20.f; first.attack.guardDamage = 24.f; first.attack.lunge = 0.6f;
    first.attack.tracking = 3.f;
    first.minRange = 0.f; first.maxRange = 2.6f; first.weight = 0.8f; first.chain = 2; first.tell = ArenaTell::Overhead;
    ArenaEnemyMove& second = k.moves[2];
    second.attack.startup = 0.4f; second.attack.active = 0.2f; second.attack.recovery = 0.9f; second.attack.range = 2.6f;
    second.attack.damage = 12; second.attack.poiseDamage = 18.f; second.attack.guardDamage = 20.f; second.attack.lunge = 0.8f;
    second.attack.tracking = 2.5f;
    second.minRange = 0.f; second.maxRange = 3.f; second.weight = 0.f; second.tell = ArenaTell::Backhand;
    def.enemyCount = 1;
    return def;
}

ArenaFeatureChecks arenaFeatureChecks(const ArenaStats& stats) {
    ArenaFeatureChecks c;
    c.lightsFree = stats.lights > 0 && stats.lightStamina <= 0.f;
    c.cancels = stats.cancels > 0;
    c.flinch = stats.flinches > 0;
    c.breakOut = stats.breakOuts > 0;
    c.breakOutDue = stats.breakDue > 0;
    c.softTarget = stats.softTargets > 0;
    c.shots = stats.shotHits > 0;
    const auto verdict = [](bool can, bool shown) {
        return !can ? ArenaCheck::NotShown : (shown ? ArenaCheck::Pass : ArenaCheck::Fail);
    };
    c.lightsFreeCheck = verdict(stats.lights > 0, c.lightsFree);
    c.softTargetCheck = verdict(stats.softChances > 0 || stats.softTargets > 0, c.softTarget);
    c.cancelsCheck = verdict(stats.cancelChances > 0 || stats.cancels > 0, c.cancels);
    c.flinchCheck = verdict(stats.flinchChances > 0 || stats.flinches > 0, c.flinch);
    if (stats.breakOuts > 0)
        c.breakOutCheck = ArenaCheck::Pass;
    else if (stats.breakDue > stats.breakBarred)
        c.breakOutCheck = ArenaCheck::Fail; // due, the move free, and still none
    else if (stats.breakBarred > 0 || (stats.breakUpright > 0 && stats.breakVetoed == stats.breakUpright))
        c.breakOutCheck = ArenaCheck::Barred;
    else if (stats.breakUpright > 0 && stats.breakRunBest == 0)
        c.breakOutCheck = ArenaCheck::Fail; // blows landed upright and the run never moved
    return c;
}

const char* arenaCheckText(ArenaCheck c) {
    switch (c) {
    case ArenaCheck::Pass: return "PASS";
    case ArenaCheck::Fail: return "FAIL";
    case ArenaCheck::Barred: return "barred";
    case ArenaCheck::NotShown: break;
    }
    return "n/a";
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
//             lockRange (0, 1000]; lockBreak (0, 1000] and at least lockRange; moveAccel, moveBrake 0..1000;
//             loadShare 0..10; releasePoise 0..1000; pairWindow 0..1; pairHits 0..10; parryStop 0..0.3;
//             softRange 0..100; softArcCos -1..1; homingReach 0..20; hurtJolt 0..kArenaMaxJolt;
//             attacks a bool; release an attack;
//             readPower 0..100; readStackPower 0..10; readPoise, readWrongPoise 0..100000;
//             blockLoadShare 0..1; takeHalfWindow 0.05..0.6; takeHalfReach 1..10; takeHalfLoadShare 0..2;
//             takeHalfRearm 0..2
//   bpm       (0, 300], optional; with it every move's startup is a whole number of beats, within
//             kArenaBeatTolerance. An enemy may carry a bpm of its own, which its startups are checked against
//             in place of the file's
//   attack    startup, recovery 0..10; active (0, 10]; range (0, 100]; arcCos -1..1; damage, poiseDamage,
//             guardDamage, stamina 0..100000; lunge, tracking 0..100; armor, cancel 0..1; maxTargets 0..6; width 0..20;
//             cancelGuard, cancelRoll, cancelLight -1..1 (below 0: `cancel`; the player's, an enemy's move ignores
//             them, as it does stamina and cancel; none before the active part); hitStop 0..0.3, hitStopSelf 0..1;
//             jolt 0..kArenaMaxJolt degrees (ArenaImpact.h)
//   enemy     hp 1..100000; poise (0, 100000]; poiseDelay, staggerTime, openTime, downTime, attackDelay 0..10;
//             walkSpeed, runSpeed, strafeSpeed 0..50; turnRate (0, 100]; preferredRange (0, 100];
//             runRange 0..100 and at least preferredRange; bodyRadius (0, 5]; height (0, 30] metres, optional
//             (see arenaEnemyHeight); moves 1..kMaxArenaMoves; accel 0..1000; bpm (0, 300], optional (see above);
//             look, optional, the host's skinned figure for it: { "clips": a path in Data/ of 1..95 characters,
//             "hover" 0..30 m, "scale" (0, 100], "yawOffset" -360..360 degrees } (clips required)
//             flinchTime 0..10; flinchPoise 0..100000; flinchPush 0..20; breakAt 0..255; breakDecay 0..60;
//             breakMove -1 or one of its moves
//             at most 254 kinds in the file (ArenaDef::enemyKind holds an index, kArenaNoKind is 255)
//   move      the attack's keys; minRange 0..100; maxRange (0, 100] and at least minRange; cooldown 0..60;
//             weight 0..1000 (0: only ever reached by a chain; at least one move needs more);
//             chain -1 or another move of the same enemy, with no loop; unblockable, parryable booleans;
//             tell one of arenaTellName's words, no two named alike in one enemy (default "strike": moves left
//             at it read alike, so a lone enemy whose moves name no tell is dealt no Read cards);
//             clip an id of 1..31 letters, digits, _ and -, optional, only in an enemy with a look: the clip the
//             host plays for the move (the host's business; no two moves of one enemy share one)
//   fight     enemies 1..6 known enemy ids; ring (0, 1000]; spawn (0, 1000]; attackers 1..6;
//             scene a non-empty id; beat 0..1000000; step 0..1000

namespace {

using ArenaStrings = std::vector<std::string>;

constexpr std::string_view kArenaAttackKeys[] = {"startup", "active",      "recovery",    "range",  "arcCos",
                                                 "damage",  "poiseDamage", "guardDamage", "lunge",  "tracking",
                                                 "armor",   "stamina",     "cancel",      "maxTargets", "width",
                                                 "cancelGuard", "cancelRoll", "cancelLight", "hitStop", "hitStopSelf",
                                                 "jolt"};

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
    arenaNum(j, "width", a.width, 0, 20, false, path, errors);
    arenaInt(j, "damage", a.damage, 0, 100000, path, errors);
    arenaNum(j, "poiseDamage", a.poiseDamage, 0, 100000, false, path, errors);
    arenaNum(j, "guardDamage", a.guardDamage, 0, 100000, false, path, errors);
    arenaNum(j, "lunge", a.lunge, 0, 100, false, path, errors);
    arenaNum(j, "tracking", a.tracking, 0, 100, false, path, errors);
    arenaNum(j, "armor", a.armor, 0, 1, false, path, errors);
    arenaNum(j, "stamina", a.stamina, 0, 100000, false, path, errors);
    arenaNum(j, "cancel", a.cancel, 0, 1, false, path, errors);
    arenaByte(j, "maxTargets", a.maxTargets, 0, static_cast<int>(kMaxArenaEnemies), path, errors);
    arenaNum(j, "cancelGuard", a.cancelGuard, -1, 1, false, path, errors);
    arenaNum(j, "cancelRoll", a.cancelRoll, -1, 1, false, path, errors);
    arenaNum(j, "cancelLight", a.cancelLight, -1, 1, false, path, errors);
    arenaNum(j, "hitStop", a.hitStop, 0, kArenaMaxStop, false, path, errors);
    arenaNum(j, "hitStopSelf", a.hitStopSelf, 0, 1, false, path, errors);
    arenaNum(j, "jolt", a.jolt, 0, kArenaMaxJolt, false, path, errors);
    // A cancel window opens no sooner than the active part: an earlier one would let the attack be cut before it
    // could hit (a feint), which no attack offers.
    const float whole = a.startup + a.active + a.recovery;
    const auto early = [&](const char* key, float share) {
        if (share >= 0.f && share * whole < a.startup - kArenaEps)
            errors.push_back(path + "." + key + ": opens before the active part (at least " +
                             std::to_string(whole > 0.f ? a.startup / whole : 0.f) + " of the attack)");
    };
    early("cancelGuard", a.cancelGuard);
    early("cancelRoll", a.cancelRoll);
    early("cancelLight", a.cancelLight);
}

// An attack object that is a value of its own (light entries, heavy, riposte, release).
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
                     {"id", "weapon", "hp", "stamina", "staminaRegen", "staminaDelay", "poise", "poiseDelay", "hitstun",
                      "moveSpeed", "turnRate", "bodyRadius", "bufferTime", "light", "chainGap", "heavy", "riposte",
                      "dodgeTime", "dodgeDistance", "dodgeInvulnStart", "dodgeInvulnEnd", "dodgeStamina",
                      "dodgeCancel", "guardArcCos", "guardAbsorb", "guardMoveScale", "guardBreakTime", "parryWindow",
                      "parryRearm", "riposteReach", "healCharges", "healAmount", "healTime", "healRecovery",
                      "healMoveScale", "lockRange", "lockBreak", "moveAccel", "moveBrake", "loadShare", "release",
                      "releasePoise", "pairWindow", "pairHits", "readPower", "readStackPower", "readPoise",
                      "readWrongPoise", "attacks", "shot", "parryStop", "softRange", "softArcCos", "homingReach",
                      "hurtJolt", "blockLoadShare", "takeHalfWindow", "takeHalfReach", "takeHalfLoadShare",
                      "takeHalfRearm"},
                     false, warnings);
    if (const JsonValue wj = j["weapon"]; !wj.isAbsent()) {
        bool known = false;
        for (const ArenaWeapon w : {ArenaWeapon::None, ArenaWeapon::Gauntlets, ArenaWeapon::Guns, ArenaWeapon::Magic}) {
            if (!wj.isString() || wj.string() != arenaWeaponName(w)) continue;
            f.weapon = w;
            known = true;
        }
        if (!known) errors.push_back(path + ".weapon: expected one of none, gauntlets, guns, magic");
    }
    arenaBool(j, "attacks", f.attacks, path, errors);
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
    arenaNum(j, "loadShare", f.loadShare, 0, 10, false, path, errors);
    arenaNum(j, "releasePoise", f.releasePoise, 0, 1000, false, path, errors);
    arenaNum(j, "pairWindow", f.pairWindow, 0, 1, false, path, errors);
    arenaByte(j, "pairHits", f.pairHits, 0, 10, path, errors);
    arenaNum(j, "parryStop", f.parryStop, 0, kArenaMaxStop, false, path, errors);
    arenaNum(j, "hurtJolt", f.hurtJolt, 0, kArenaMaxJolt, false, path, errors);
    arenaNum(j, "softRange", f.softRange, 0, 100, false, path, errors);
    arenaNum(j, "softArcCos", f.softArcCos, -1, 1, false, path, errors);
    arenaNum(j, "homingReach", f.homingReach, 0, 20, false, path, errors);
    arenaNum(j, "readPower", f.readPower, 0, 100, false, path, errors);
    arenaNum(j, "readStackPower", f.readStackPower, 0, 10, false, path, errors);
    arenaNum(j, "readPoise", f.readPoise, 0, 100000, false, path, errors);
    arenaNum(j, "readWrongPoise", f.readWrongPoise, 0, 100000, false, path, errors);
    arenaNum(j, "blockLoadShare", f.blockLoadShare, 0, 1, false, path, errors);
    arenaNum(j, "takeHalfWindow", f.takeHalfWindow, 0.05f, 0.6f, false, path, errors);
    arenaNum(j, "takeHalfReach", f.takeHalfReach, 1, 10, false, path, errors);
    arenaNum(j, "takeHalfLoadShare", f.takeHalfLoadShare, 0, 2, false, path, errors);
    arenaNum(j, "takeHalfRearm", f.takeHalfRearm, 0, 2, false, path, errors);

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
    if (j.has("release")) arenaParseAttackObject(j["release"], path + ".release", f.release, errors, warnings);
    if (j.has("shot")) {
        if (f.weapon != ArenaWeapon::Guns) errors.push_back(path + ".shot: only a guns kit has a shot");
        else arenaParseAttackObject(j["shot"], path + ".shot", f.shot, errors, warnings);
    } else if (f.weapon == ArenaWeapon::Guns && j.has("weapon")) {
        errors.push_back(path + ".shot: a guns kit needs its shot");
    }

    if (f.dodgeInvulnStart > f.dodgeInvulnEnd)
        errors.push_back(path + ".dodgeInvulnStart: expected at most dodgeInvulnEnd");
    if (f.dodgeInvulnEnd > f.dodgeTime) errors.push_back(path + ".dodgeInvulnEnd: expected at most dodgeTime");
    if (f.lockBreak < f.lockRange) errors.push_back(path + ".lockBreak: expected at least lockRange");
}

// 1 or more letters, digits, _ and -.
bool arenaIdText(std::string_view id) {
    if (id.empty()) return false;
    for (const char c : id)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
    return true;
}

// An enemy's `look`: absent keeps the shapes.
void arenaParseLook(JsonValue j, const std::string& path, ArenaEnemyLook& look, ArenaStrings& errors, ArenaStrings* warnings) {
    if (j.isAbsent()) return;
    if (!j.isObject()) {
        errors.push_back(path + ": expected an object");
        return;
    }
    arenaUnknownKeys(j, path, {"clips", "hover", "scale", "yawOffset"}, false, warnings);
    const JsonValue c = j["clips"];
    const std::string_view clips = c.string();
    if (!c.isString() || clips.empty() || clips.size() > kMaxArenaLookPath || clips.rfind("Data/", 0) != 0 ||
        clips.find("..") != std::string_view::npos || clips.find('\\') != std::string_view::npos)
        errors.push_back(path + ".clips: expected a path in Data/ of 1.." + std::to_string(kMaxArenaLookPath) + " characters");
    else
        std::copy(clips.begin(), clips.end(), look.clipsText.begin());
    arenaNum(j, "hover", look.hover, 0, 30, false, path, errors);
    arenaNum(j, "scale", look.scale, 0, 100, true, path, errors);
    arenaNum(j, "yawOffset", look.yawOffset, -360, 360, false, path, errors);
}

void arenaParseEnemy(JsonValue j, const std::string& path, ArenaEnemyKind& k, ArenaStrings& errors,
                     ArenaStrings* warnings) {
    arenaUnknownKeys(j, path,
                     {"id", "hp", "poise", "poiseDelay", "staggerTime", "openTime", "downTime", "walkSpeed", "runSpeed",
                      "strafeSpeed", "turnRate", "preferredRange", "runRange", "attackDelay", "bodyRadius", "height", "moves",
                      "accel", "bpm", "look", "passive", "flinchTime", "flinchPoise", "flinchPush", "breakAt",
                      "breakDecay", "breakMove"},
                     false, warnings);
    arenaInt(j, "hp", k.hp, 1, 100000, path, errors);
    arenaBool(j, "passive", k.passive, path, errors);
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
    arenaNum(j, "height", k.height, 0, 30, true, path, errors);
    arenaNum(j, "bpm", k.bpm, 0, 300, true, path, errors);
    arenaNum(j, "flinchTime", k.flinchTime, 0, 10, false, path, errors);
    arenaNum(j, "flinchPoise", k.flinchPoise, 0, 100000, false, path, errors);
    arenaNum(j, "flinchPush", k.flinchPush, 0, 20, false, path, errors);
    arenaByte(j, "breakAt", k.breakAt, 0, 255, path, errors);
    arenaNum(j, "breakDecay", k.breakDecay, 0, 60, false, path, errors);
    arenaInt(j, "breakMove", k.breakMove, -1, static_cast<int>(kMaxArenaMoves) - 1, path, errors);
    arenaParseLook(j["look"], path + ".look", k.look, errors, warnings);
    if (k.runRange < k.preferredRange) errors.push_back(path + ".runRange: expected at least preferredRange");

    const JsonValue moves = j["moves"];
    if (moves.isAbsent()) return; // the one default move stays
    if (!moves.isArray() || moves.size() == 0 || moves.size() > kMaxArenaMoves) {
        errors.push_back(path + ".moves: expected an array of 1.." + std::to_string(kMaxArenaMoves) + " moves");
        return;
    }
    k.moveCount = static_cast<std::uint8_t>(moves.size());
    bool pickable = false;
    std::array<bool, kMaxArenaMoves> told{}; // the moves whose tell the file names
    for (std::uint32_t m = 0; m < moves.size(); ++m) {
        const JsonValue mj = moves[m];
        const std::string mp = path + ".moves[" + std::to_string(m) + "]";
        if (!mj.isObject()) {
            errors.push_back(mp + ": expected an object");
            continue;
        }
        ArenaEnemyMove& mv = k.moves[m];
        arenaUnknownKeys(mj, mp,
                         {"minRange", "maxRange", "cooldown", "weight", "chain", "unblockable", "parryable", "tell", "clip"},
                         true, warnings);
        arenaParseAttack(mj, mp, mv.attack, errors);
        arenaNum(mj, "minRange", mv.minRange, 0, 100, false, mp, errors);
        arenaNum(mj, "maxRange", mv.maxRange, 0, 100, true, mp, errors);
        arenaNum(mj, "cooldown", mv.cooldown, 0, 60, false, mp, errors);
        arenaNum(mj, "weight", mv.weight, 0, 1000, false, mp, errors);
        arenaBool(mj, "unblockable", mv.unblockable, mp, errors);
        arenaBool(mj, "parryable", mv.parryable, mp, errors);
        const JsonValue tj = mj["tell"];
        if (!tj.isAbsent()) {
            bool known = false;
            for (std::size_t t = 0; t < kArenaTellCount && tj.isString(); ++t) {
                if (tj.string() != arenaTellName(static_cast<ArenaTell>(t))) continue;
                mv.tell = static_cast<ArenaTell>(t);
                known = true;
                told[m] = true;
            }
            if (!known)
                errors.push_back(mp + ".tell: expected one of strike, overhead, backhand, charge, thrust, slash, sweep, "
                                      "slam, shot");
        }
        if (const JsonValue cj = mj["clip"]; !cj.isAbsent()) {
            const std::string_view id = cj.string();
            if (!cj.isString() || id.empty() || id.size() > kMaxArenaClipId || !arenaIdText(id))
                errors.push_back(mp + ".clip: expected 1.." + std::to_string(kMaxArenaClipId) + " letters, digits, _ and -");
            else if (!k.look.skinned())
                errors.push_back(mp + ".clip: expected the enemy to have a look");
            else
                std::copy(id.begin(), id.end(), mv.clipText.begin());
            for (std::uint32_t o = 0; o < m && !id.empty(); ++o)
                if (k.moves[o].clip() == id) errors.push_back(mp + ".clip: the same as moves[" + std::to_string(o) + "]'s");
        }
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
    // Each named tell must tell its move apart from the enemy's other named ones.
    for (std::uint32_t m = 1; m < k.moveCount; ++m) {
        for (std::uint32_t o = 0; o < m; ++o) {
            if (!told[m] || !told[o] || k.moves[m].tell != k.moves[o].tell) continue;
            errors.push_back(path + ".moves[" + std::to_string(m) + "].tell: the same as moves[" + std::to_string(o) +
                             "]'s");
            break;
        }
    }
    if (k.breakMove >= static_cast<int>(k.moveCount))
        errors.push_back(path + ".breakMove: expected -1 or one of its " + std::to_string(k.moveCount) + " moves");
    if (k.breakAt > 0 && k.breakMove < 0) errors.push_back(path + ".breakMove: breakAt needs a move to break out with");
    if (k.breakAt > 1 && !(k.breakDecay > 0.f))
        errors.push_back(path + ".breakDecay: breakAt above 1 needs a decay above 0 to count blows across ticks");
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
    arenaUnknownKeys(root, "", {"version", "bpm", "fighters", "enemies", "default", "fights"}, false, warnings);

    ArenaTable table;
    if (const JsonValue v = root["bpm"]; !v.isAbsent()) {
        const double bpm = v.number(-1.0);
        if (v.isNumber() && std::isfinite(bpm) && bpm > 0.0 && bpm <= 300.0) table.bpm = static_cast<float>(bpm);
        else errors.push_back("bpm: expected a number in (0, 300]");
    }

    // Fighters: ids unique, "default" among them (and moved to the front). The "default" is read first, from the
    // struct's defaults; a lead's kit starts from it, so a lead lists only what differs.
    const JsonValue fighters = root["fighters"];
    if (!fighters.isAbsent() && !fighters.isArray()) errors.push_back("fighters: expected an array");
    ArenaFighter base;
    for (std::uint32_t i = 0; i < fighters.size() && fighters.isArray(); ++i) {
        const JsonValue j = fighters[i];
        if (!j.isObject() || j["id"].string() != "default") continue;
        ArenaStrings ignored; // its errors and warnings are reported in order below
        arenaParseFighter(j, "", base, ignored, nullptr);
        break;
    }
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
        ArenaFighter f = fid == "default" ? ArenaFighter{} : base;
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

    // With a bpm (the file's, or the kind's own), every move's startup is a whole number of beats (within
    // kArenaBeatTolerance).
    for (std::size_t i = 0; i < table.enemies.size(); ++i) {
        const ArenaEnemyKind& k = table.enemies[i];
        const float bpm = k.bpm > 0.f ? k.bpm : table.bpm;
        if (!(bpm > 0.f)) continue;
        for (std::size_t m = 0; m < k.moveCount; ++m) {
            if (arenaWholeBeats(k.moves[m].attack.startup, bpm) > 0) continue;
            errors.push_back("enemies[" + std::to_string(i) + "].moves[" + std::to_string(m) +
                             "].startup: expected a whole number of beats at " + arenaFormat(bpm) + " bpm (" +
                             arenaFormat(60.0 / bpm) + " s each, within " + arenaFormat(kArenaBeatTolerance) + " s)");
        }
    }

    // The default fight, and the fights keyed by scene, beat and step.
    const JsonValue def = root["default"];
    if (!def.isObject()) {
        errors.push_back("default: expected an object");
    } else {
        arenaUnknownKeys(def, "default", {"enemies", "ring", "attackers", "spawn"}, false, warnings);
        table.fallback.fighter = defaultFighter;
        table.fallback.bpm = table.bpm;
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
        entry.def.bpm = table.bpm;
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

// ---- log ----

namespace {

// Appends to a fixed buffer; whatever does not fit is dropped.
struct LineWriter {
    char* out;
    std::size_t cap;
    std::size_t len = 0;

    void put(std::string_view text) {
        for (const char c : text) {
            if (len + 1 >= cap) return;
            out[len++] = c;
        }
    }
    template <typename... Args>
    void print(const char* format, Args... args) {
        if (len + 1 >= cap) return;
        const int n = std::snprintf(out + len, cap - len, format, args...);
        if (n > 0) len = std::min(len + static_cast<std::size_t>(n), cap - 1);
    }
    // An id: printable ASCII with no spaces or commas, cut to 48 characters, so a line always parses.
    void id(std::string_view text) {
        if (text.empty()) text = "-";
        std::size_t n = 0;
        for (const char c : text) {
            if (n++ == 48) break;
            put(std::string_view(c > ' ' && c < 127 && c != ',' ? &c : "_", 1));
        }
    }
};

} // namespace

std::size_t arenaResultLine(char* out, std::size_t cap, const ArenaFight& fight, const ArenaTable& table,
                            const ArenaLogIds& ids) {
    if (!out || cap == 0) return 0;
    LineWriter w{out, cap};
    const ArenaStats& s = fight.stats();
    const ArenaPhase phase = fight.phase();
    w.put(phase == ArenaPhase::Victory ? "Arena: VICTORY at " : phase == ArenaPhase::Defeat ? "Arena: DEFEAT at " : "Arena: TIMEOUT at ");
    w.id(ids.scene);
    w.print(" beat %u step %u after %.1f s, zone ", ids.beat, ids.step, static_cast<double>(fight.time()));
    w.id(ids.zone);
    w.put(", kit ");
    w.id(ids.kit);
    w.put(", enemies ");
    const ArenaDef& def = fight.def();
    for (std::size_t e = 0; e < def.enemyCount; ++e) {
        if (e > 0) w.put("+");
        const std::uint8_t kind = def.enemyKind[e];
        w.id(kind < table.enemyIds.size() ? std::string_view(table.enemyIds[kind]) : std::string_view("built-in"));
    }
    if (def.enemyCount == 0) w.put("none");
    w.put(", ");
    w.id(ids.pilot);
    w.print(" seed %u, hp %d/%d, %d hits, %d taken, %d blocks, %d parries, %d ripostes, %d evades, %d guard breaks, %d staggers, "
            "%d heals, %d releases, %d overloads, %d/%d reads right, dealt %d, took %d, %d restarts.",
            ids.seed, fight.hp(), def.fighter.hp, s.hits, s.hitsTaken, s.blocks, s.parries, s.ripostes, s.evades, s.guardBreaks, s.staggers, s.heals,
            s.releases, s.overloads, s.correctReads, s.readsDealt, s.damageDealt, s.damageTaken, s.restarts);
    out[w.len] = '\0';
    return w.len;
}

} // namespace ftcore
