#include "ftcore/ArenaFight.h"
#include "ftcore/ArenaSpecial.h"
#include "ftcore/ArenaSoftTarget.h"
#include "ftcore/ArenaImpact.h"
#include "ftcore/ArenaLook.h"
#include "ftcore/PartnerBrain.h"
#include "ftcore/TableRing.h"

#include "SeedMix.h"
#include "ftcore/Json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <span>
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
// The partner's drawn heading (ArenaAllyOut::heading): turns at most this (rad/s, 540 deg/s) toward its walk when it
// walked over kAllyHeadingMoving m/s this tick, else toward its facing.
constexpr float kAllyHeadingTurn = 3.f * kArenaPi;
constexpr float kAllyHeadingMoving = 0.05f;
constexpr float kAllyHeadingLeadMeters = 1.f;   // ... and toward its target over a walk's last metre ...
constexpr float kAllyHeadingLeadSeconds = 0.3f; // ... or its last 0.3 s at its pace, the longer
constexpr float kAllyLinkTurn = 2.f * kArenaPi; // rad/s: the most the partner's facing turns in a blow's recovery (360 deg/s)
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
// The partner's walk (stepAlly's shapeAllyWalk).
constexpr float kAllyAroundMargin = 0.15f; // m: it passes an enemy body this far outside touching it (twice round it)
constexpr float kAllyAroundStep = 0.5f;    // m: the least look-ahead along a tangent round a body
constexpr float kAllyHeadingHold = 0.4f;   // s: no reversal over 90 degrees within this of its last one
constexpr float kAllyHeadingGap = 0.5f;    // s: a heading is kept this long (standing this long, the next walk is fresh)
constexpr float kAllyHeadingStep = 1.f / 120.f; // s: a heading is kept at most this often (the ring covers the gap)
constexpr float kAllyHeadingMargin = 0.05f; // radians inside the allowed arc's edges
constexpr float kAllyMovingSpeed = 0.5f;   // m/s: a goal nearer than this a step is standing (no heading)

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
// an enemy the lead stands in, passes it. A body beside or behind the lead is not met (the round leaves it). `prefer`
// (the crosshair's body), met within kArenaShotGraze of the first body met, is the one instead.
std::uint8_t arenaShotFirst(const ArenaAttack& a, Vec2 from, Vec2 dir, float pitch, const ArenaEnemyKind* kinds,
                            const Vec2* at, const bool* standing, std::size_t count, std::uint32_t skip,
                            std::uint8_t prefer, float& distance) {
    std::uint8_t best = kArenaNobody;
    float nearest = 0.f;
    bool preferMet = false;
    float preferAt = 0.f;
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
        if (i == static_cast<std::size_t>(prefer)) {
            preferMet = true;
            preferAt = d;
        }
        if (best == kArenaNobody || d < nearest) {
            best = static_cast<std::uint8_t>(i);
            nearest = d;
        }
    }
    if (preferMet && preferAt - nearest <= kArenaShotGraze) {
        best = prefer;
        nearest = preferAt;
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

// How far a body of `radius` at `pos` may go straight along `way` (unit) before the ring's cut stops it: the edge it
// may reach is the one arenaCutVelocity keeps it to (the ring less its radius, or where it stands if already beyond).
float arenaRoomAlong(const ArenaRing& ring, Vec2 pos, Vec2 way, float radius) {
    const Vec2 w = arenaSub(pos, ring.centre);
    const float limit = std::max(std::max(ring.radius - radius, 0.f), arenaLength(w));
    const float b = arenaDot(w, way);
    const float c = arenaDot(w, w) - limit * limit; // <= 0: inside or on the edge
    return std::max(0.f, -b + std::sqrt(std::max(0.f, b * b - c)));
}

// How far a body of `radius` at `pos` gets in all trying `distance` along `way` (unit) over kArenaStepBackTime at a
// steady pace, the ring cutting each 60th of a second as arenaCutVelocity does (so it slides along the edge).
float arenaRoomSliding(const ArenaRing& ring, Vec2 pos, Vec2 way, float distance, float radius) {
    constexpr int kSteps = 30; // kArenaStepBackTime at 60 ticks a second
    const float dt = kArenaStepBackTime / static_cast<float>(kSteps);
    const Vec2 v = arenaScaled(way, distance / kArenaStepBackTime);
    Vec2 at = pos;
    for (int s = 0; s < kSteps; ++s) at = arenaMadd(at, arenaCutVelocity(ring, at, v, dt, radius), dt);
    return arenaDistance(at, pos);
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

// A boss's break phase (0-based) at `hp`: each phase but the last ends where the hp share falls to its end.
int arenaBreakPhaseOf(const ArenaEnemyKind& k, int hp) {
    const float share = static_cast<float>(hp) / static_cast<float>(std::max(k.hp, 1));
    const int last = std::clamp(static_cast<int>(k.breakPhases), 1, static_cast<int>(kMaxArenaBreakPhases)) - 1;
    int p = 0;
    while (p < last && share <= k.breakPhaseEnds[static_cast<std::size_t>(p)]) ++p;
    return p;
}

// Its threshold at `hp`: the phase's breakValueAt, never below kArenaBossBreakFloor (NaN: the floor).
float arenaBreakAtOf(const ArenaEnemyKind& k, int hp) {
    const float at = k.breakValueAt[static_cast<std::size_t>(arenaBreakPhaseOf(k, hp))];
    return at >= kArenaBossBreakFloor ? at : kArenaBossBreakFloor;
}

// Enemy kind k's body radius as drawn (a skinned look's scale), the one a blow's reach and the riposte's take.
float arenaDrawnRadius(const ArenaEnemyKind& k) { return arenaBodyCylinder(k, Vec2{}, 0.f).radius; }

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
    const ArenaFighter& pk; // the partner's kit (ArenaDef::partnerKit)
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
    Vec2 camera{};         // ArenaInput::cameraFacing this tick (zero: none)

    explicit ArenaFightImpl(ArenaFight& f)
        : fight(f), pl(f.player_), foes(f.enemy_), def(f.def_), fi(f.def_.fighter), pk(f.def_.partnerKit), out(f.out_), stats(f.stats_),
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
            return p.move == kArenaShotMove ? &f.shot
                   : p.move == kArenaAirMove ? &f.jumpLight
                                             : &f.light[std::min<std::size_t>(p.move, f.lightCount - 1u)];
        case ArenaAction::Heavy: return &f.heavy;
        case ArenaAction::Riposte: return &f.riposte;
        case ArenaAction::Release: return &f.release;
        default: return nullptr;
        }
    }

    // What wants to cut into the current action: an attack's own cancel window for it, if it names one.
    enum class Cut : std::uint8_t { Other, Guard, Roll, Light, Jump };
    static Cut cutOf(Press what) {
        switch (what) {
        case Press::Light:
        case Press::Shot: return Cut::Light;
        case Press::Dodge: return Cut::Roll;
        case Press::Jump: return Cut::Jump;
        default: return Cut::Other;
        }
    }

    // A new action may start from Idle/Move, from an attack once `cancel` of its recovery has passed (or its own
    // window for that action, a share of the whole attack), from a dodge once `dodgeCancel` of it has passed, and
    // from Guard at any time. A jump cuts an attack through its roll window, never a Release, and a dodge only once
    // its i-frames are over (no chaining them into the air's clear window). Nothing cuts an air light.
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
            if (p.action == ArenaAction::Light && p.move == kArenaAirMove) return false;
            if (p.action == ArenaAction::Release && cut == Cut::Jump) return false;
            const ArenaAttack& a = *attackOf(p, f);
            const float own = cut == Cut::Guard                    ? a.cancelGuard
                              : cut == Cut::Roll || cut == Cut::Jump ? a.cancelRoll
                              : cut == Cut::Light                  ? a.cancelLight
                                                                   : -1.f;
            if (own >= 0.f) return p.actionTime >= own * a.total();
            return p.actionTime >= a.startup + a.active + a.cancel * a.recovery;
        }
        case ArenaAction::Dodge:
            if (cut == Cut::Jump && p.actionTime < f.dodgeInvulnEnd - kArenaEps) return false;
            return p.actionTime >= f.dodgeCancel * f.dodgeTime;
        case ArenaAction::Jump: {
            // Nothing in the take-off; in the air only the one air light; on the ground again once the landing's
            // first jumpLandLock has passed (its weight).
            const float down = f.jumpStartup + f.jumpAir - kArenaEps; // float clocks: within kArenaEps is there
            if (p.actionTime < f.jumpStartup) return false;
            if (p.actionTime < down) return cut == Cut::Light && !p.airLightUsed;
            return p.actionTime >= down + f.jumpLandLock;
        }
        default:
            return false;
        }
    }

    // ---- begin / restart ----

    void reset() {
        if (fight.kitsSwapped_) swapKits(); // each lead its own kit again
        fight.switchRefusal_ = ArenaSwitchRefusal::Ok;
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
        pl.charges = static_cast<std::uint8_t>(
            std::min(255, static_cast<int>(fi.healCharges) + (fight.lone_ ? 0 : fight.chairs_.healCharges())));
        fight.healMax_ = pl.charges;
        pl.rounds = fi.rounds;
        fight.otherRounds_ = pk.rounds; // the other kit's cylinder, full and not reloading
        fight.otherReload_ = 0.f;
        fight.allyHp_ = fight.allyMaxHp_;
        fight.tableSeats_ = ArenaTableSeats{};
        fight.ally_ = ArenaFight::AllyState{};
        fight.ally_.facing = fight.playerStartFacing_;
        fight.ally_.heading = fight.playerStartFacing_;
        fight.ally_.goal = fight.allyPos_;
        fight.swapped_ = false;
        fight.aimStrings_ = 0;
        fight.aimPartner_ = 0;
        if (fight.partner_ && fight.partnerTuning_) fight.partner_->begin(*fight.partnerTuning_, fight.seed_);
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
        ArenaFight::SpecialState& sp = fight.special_;
        sp.gauge = sp.gaugeAtBegin;
        sp.nextCue = 0;
        sp.view = ArenaSpecialView{};
        specialArm();
        specialFlags(true);
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
        po.dodgeSide = pl.dodgeSide;
        po.load = pl.gauge.value;
        po.overloaded = overloadedNow();
        po.loadCap = fight.capNow();
        po.jumpTime = pl.jumpTime;
        po.height = fight.phase_ == ArenaPhase::Fighting ? jumpHeightAt(pl.jumpTime) : 0.f;
        po.airClear = pl.airClear;
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
            eo.unseen = i < n && foe.brain == ArenaBrain::Attack && foe.unseen;
            eo.stopLeft = fighting && i < n && foe.brain != ArenaBrain::Dead
                              ? std::max(0.f, foe.stopUntil - fight.time_)
                              : 0.f;
            if (i < n && foe.brain == ArenaBrain::Attack) {
                eo.part = arenaPartOf(fight.enemyAttackNow(i), foe.stateTime);
                eo.move = foe.move;
            }
            eo.aim = i < n && foe.brain == ArenaBrain::Attack ? foe.aim : 0;
        }
        ArenaAllyOut& ao = out.ally;
        ao = ArenaAllyOut{};
        ao.goal = fight.allyPos_;
        if (partnerOn()) {
            const ArenaFight::AllyState& al = fight.ally_;
            if (fighting) ao.goal = al.goal;
            ao.facing = al.facing;
            ao.attacking = al.attacking;
            ao.blow = al.blow;
            ao.stateTime = al.attacking ? al.blowTime : 0.f;
            ao.part = al.attacking ? arenaPartOf(allyLight(al.blow), al.blowTime) : ArenaPart::None;
            ao.guarding = al.guarding;
            ao.target = al.target;
            ao.act = al.act;
            ao.partnerOn = true;
            ao.stationed = al.stationed;
            ao.pairSeconds = pl.pairSpent;
            ao.bodyTurn = al.bodyTurn;
            ao.intercept = al.intercept;
            ao.heading = al.heading;
        }
    }

    // ---- one tick ----

    void tick(float step, const ArenaInput& in, const ArenaBodies& bodies) {
        if (step > 0.f && step < 1.0e6f) fight.special_.clock += step; // the host's clock runs on every tick given
        if (fight.special_.view.running) { // frozen: only the special's own block moves
            specialTick(step, in);
            return;
        }
        if (step > 0.f) fight.special_.view.endedThisTick = false; // the cut back was the last tick
        out.events.count = 0;
        pl.pairSpent = 0.f;
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
        fight.ally_.moved = arenaDistance(fight.allyPos_, bodies.ally) > kArenaEps; // the host walked it
        {   // its drawn heading: toward its walk this tick, else its facing, at most kAllyHeadingTurn; over a walk's
            // last kAllyHeadingLeadMeters (or kAllyHeadingLeadSeconds at its pace), toward its target, so a string
            // starts on arrival with no turn pause (rule 19's 60 degree gate is then a standing start's safety)
            ArenaFight::AllyState& al = fight.ally_;
            const Vec2 walked = arenaSub(bodies.ally, fight.allyPos_);
            const float moved = arenaDistance(fight.allyPos_, bodies.ally);
            const bool walking = step > 0.f && moved > kAllyHeadingMoving * step;
            Vec2 want = walking ? arenaUnit(walked, al.heading) : al.facing;
            if (walking && al.act == static_cast<std::uint8_t>(PartnerAct::MoveTo) && al.target >= 0 &&
                static_cast<std::size_t>(al.target) < n && alive(static_cast<std::size_t>(al.target))) {
                const float left = arenaDistance(bodies.ally, al.goal);
                if (left <= std::max(kAllyHeadingLeadMeters, kAllyHeadingLeadSeconds * moved / step))
                    want = arenaUnit(arenaSub(foes[static_cast<std::size_t>(al.target)].pos, bodies.ally), want);
            }
            // In a string (startup through recovery) or a guard it turns toward its facing, never its walk: a push or
            // a recoil moves the body, never turns it.
            // What this turn leaves unused goes to the tick's end, where a string's new blow may have re-aimed the
            // facing: the heading catches up there in the same tick.
            if (al.attacking || al.guarding) want = al.facing;
            const Vec2 was = al.heading;
            al.heading = arenaTurn(al.heading, want, kAllyHeadingTurn * step);
            al.headingLeft = std::max(0.f, kAllyHeadingTurn * step -
                                               std::fabs(std::atan2(arenaCross(was, al.heading), arenaDot(was, al.heading))));
            al.headingWalked = walking;
        }
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
        if (pl.hostHealed > 0) { // the host's hostHeal and hostHurt since the last tick
            emit(ArenaEventType::Healed, kArenaNobody, kArenaPlayer, 0, pl.hostHealed, pl.pos);
            pl.hostHealed = 0;
        }
        if (pl.hostHurt > 0) {
            emit(ArenaEventType::HostHit, kArenaNobody, kArenaPlayer, 0, pl.hostHurt, pl.pos);
            pl.hostHurt = 0;
        }
        fight.takenScale_ = in.takenScale >= 0.f ? std::min(in.takenScale, 10.f) : 1.f; // NaN, below 0: 1
        if (pl.capFilled) { // a lowered cap overloaded the gauge
            emit(ArenaEventType::Overloaded, kArenaPlayer, kArenaPlayer, 0, pl.gauge.value, pl.pos);
            pl.capFilled = false;
        }
        reportHostBlows(); // the host's hostBlow since the last tick
        if (step > 0.f && in.special != kArenaNobody) {
            specialFill(); // a blow the host landed since the last tick fills first: the press may use it
            if (startSpecial(in)) return; // frozen from here (no Ready is said on the press tick)
        }
        if (!(step > 0.f)) { // only refresh
            fillOutput();
            specialAfterTick(in.specialSight);
            return;
        }
        dt = step;
        t0 = fight.time_;
        t1 = t0 + dt;
        camera = in.cameraFacing;
        applyHolds();
        stepPlayer(in);
        stepAlly(in);
        noteAllyHeading();
        stepEnemies();
        stepBossBreaks();
        if (fight.read_.dealt() && !readStillOpen()) closeRead(); // the active part began, or the move is gone
        endTick();
        specialAfterTick(in.specialSight);
    }

    // The fight's end, wherever a tick ends it (endTick, a special's cut back): Defeat or Victory with its event; the
    // read closes, a reload under way is dropped (it never completes: no Reloaded, reloadProgress reads 0) and a jump
    // ends.
    void settleOutcome() {
        if (pl.hp <= 0 || allDead()) closeRead();
        if (pl.hp <= 0) {
            fight.phase_ = ArenaPhase::Defeat;
            emit(ArenaEventType::Defeat, kArenaNobody, kArenaPlayer, 0, 0, pl.pos);
        } else if (allDead()) {
            fight.phase_ = ArenaPhase::Victory;
            emit(ArenaEventType::Victory, kArenaNobody, kArenaNobody, 0, 0, pl.pos);
        }
        if (fight.phase_ != ArenaPhase::Fighting) {
            pl.reloadLeft = 0.f;
            pl.jumpTime = -1.f;
            pl.airClear = false;
        }
    }

    void endTick() {
        // A fall goes to a standing playable lead (the partner, with its kit playable), else it is the Defeat below.
        if (pl.hp <= 0 && partnerOn() && allyUp() && pk.playable) takeOver();
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
            const bool down = alive(i) && !upright(foe) && foe.brain != ArenaBrain::StepBack;
            if (down && !foe.punishOpen) {
                foe.punishHit = false;
                ++stats.punishWindows;
            }
            foe.punishOpen = down;
        }
        fight.time_ = t1;
        fight.heldTime_ += playerOwn;
        stats.time += dt;
        settleOutcome();
        // A jump ends with any action that isn't its own (a blow, a stagger, a defeat), or with the fight.
        if (pl.action != ArenaAction::Jump && !(pl.action == ArenaAction::Light && pl.move == kArenaAirMove)) {
            pl.jumpTime = -1.f;
            pl.airClear = false;
        }
        { // the partner's heading, in a string or a guard, catches up with this tick's facing (what is left of its turn)
            ArenaFight::AllyState& al = fight.ally_;
            if (al.attacking || al.guarding) al.heading = arenaTurn(al.heading, al.facing, al.headingLeft);
            al.headingLeft = 0.f;
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
        // Stamina refills at staminaRegen per second once staminaDelay has passed since the last spend. A jump costs
        // in full: none comes back off the ground (the delay starts again at touch-down) nor through the landing lock,
        // whatever staminaDelay is.
        const float down = fi.jumpStartup + fi.jumpAir;
        const bool jumping = pl.jumpTime >= 0.f;
        if (jumping && pl.jumpTime < down) pl.lastSpend = std::max(pl.lastSpend, t1);
        float from = std::max(t0, pl.lastSpend + fi.staminaDelay);
        if (jumping && pl.jumpTime < down + fi.jumpLandLock) from = t1;
        if (t1 > from && pl.stamina < fi.stamina)
            pl.stamina = std::min(fi.stamina, pl.stamina + fi.staminaRegen * (t1 - from));
        // Poise refills after poiseDelay with no poise damage.
        if (t1 - pl.lastPoiseHit >= fi.poiseDelay) pl.poise = fi.poise;
    }

    // The jump's height (m) at `t` on its air clock: an arc over jumpAir, jumpHeight at the top; 0 on the ground.
    float jumpHeightAt(float t) const {
        const float up = fi.jumpStartup;
        if (!(t > up) || !(fi.jumpAir > kArenaEps) || t >= up + fi.jumpAir - kArenaEps) return 0.f;
        const float u = (t - up) / fi.jumpAir;
        return 4.f * std::max(0.f, fi.jumpHeight) * u * (1.f - u);
    }

    // Off the floor: the jump's air phase (past the take-off, short of the landing), whatever its height.
    bool airborne() const {
        return pl.jumpTime > fi.jumpStartup && pl.jumpTime < fi.jumpStartup + fi.jumpAir - kArenaEps;
    }

    void endAction() {
        if (pl.action == ArenaAction::Light && pl.move == kArenaAirMove && pl.jumpTime >= 0.f) {
            // An air light that ends in the air: the jump goes on from its clock.
            pl.action = ArenaAction::Jump;
            pl.actionTime = pl.jumpTime;
            pl.soft = kArenaNobody;
            pl.homeClose = 0.f;
            return;
        }
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
            ArenaSoftAim own = softAim;
            if (a.homing >= 0.f) own.homing = a.homing; // the attack's own cap on the kit's
            const ArenaHoming h = arenaHoming(pl.pos, fi.bodyRadius, pl.facing, bodies[pick], a.range, a.lunge, own);
            // A softTurn caps the start's turn; tracking turns the rest over startup (advanceAttack).
            pl.facing = a.softTurn >= 0.f ? arenaTurn(pl.facing, h.facing, a.softTurn * kArenaPi / 180.f) : h.facing;
            pl.soft = pick;
            pl.homeClose = h.close;
            ++stats.softTargets;
            break;
        }
        case ArenaAimSource::Stick: pl.facing = arenaUnit(stick, pl.facing); break;
        default: break;
        }
    }

    // The Open enemy within riposteReach of its body's edge and in front of the player that a light press performs the
    // riposte on, or -1 (the nearest edge wins).
    int riposteTarget() const {
        int best = -1;
        float bestDist = 0.f;
        for (std::size_t i = 0; i < n; ++i) {
            if (foes[i].brain != ArenaBrain::Open) continue;
            const Vec2 d = arenaSub(foes[i].pos, pl.pos);
            const float distance = arenaLength(d);
            const float edge = distance - arenaDrawnRadius(def.enemies[i]);
            if (edge > fi.riposteReach) continue;
            if (distance > kArenaEps && arenaDot(pl.facing, d) / distance < kArenaRiposteDot) continue;
            if (best < 0 || edge < bestDist) {
                best = static_cast<int>(i);
                bestDist = edge;
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

    // What a light press performs now: the riposte on riposteTarget() (riposteAt >= 0), else the chain's next light
    // (`index`) when it starts during the previous light's recovery or within chainGap after it, else the first.
    const ArenaAttack& lightPick(int& riposteAt, std::uint8_t& index) const {
        riposteAt = riposteTarget();
        index = 0;
        if (riposteAt >= 0) return fi.riposte;
        const bool chained = pl.chainLive && (pl.action == ArenaAction::Light || t0 - pl.lightEnd <= fi.chainGap);
        if (chained) index = pl.chainNext;
        return fi.light[index];
    }

    // In the air: a jump past its take-off and short of touch-down (canStart lets only the air light through).
    bool inAir() const {
        return pl.action == ArenaAction::Jump && pl.jumpTime >= 0.f && pl.actionTime < fi.jumpStartup + fi.jumpAir;
    }

    // A dodge or a jump needs some stamina when the dodge costs any.
    bool outOfStamina() const { return fi.dodgeStamina > 0.f && pl.stamina <= 0.f; }

    // Why startAction would not start `what` now, once canStart lets it (Ok: it starts it). The one rule the press runs
    // (startAction) and the HUD asks (ArenaFight::pressReason), so the two cannot drift: in the air only a light or
    // shot press (the air light); an attack press of a kit with no attacks is Absent; overloaded, light (and so the
    // riposte), heavy, shot and heal are refused until a Release; an attack that costs stamina needs some; a shot needs
    // a round; a dodge or a jump needs stamina when the dodge costs any; a heal needs a charge; a Release needs more
    // than a third of the cap's Load; a kit with jumpAir 0 has no jump.
    PressReason refusal(Press what) const {
        const bool light = what == Press::Light || what == Press::Shot;
        const bool locked = overloadedNow();
        if (inAir()) {
            if (!light) return PressReason::Busy;
            if (kitLacks(what)) return PressReason::Absent;
            if (locked) return PressReason::Overloaded;
            return affords(fi.jumpLight) ? PressReason::Ok : PressReason::Stamina;
        }
        switch (what) {
        case Press::Light:
        case Press::Heavy:
        case Press::Shot: {
            if (kitLacks(what)) return PressReason::Absent;
            if (locked) return PressReason::Overloaded;
            if (what == Press::Shot && fi.rounds > 0 && pl.rounds == 0) return PressReason::Empty;
            int riposteAt = -1;
            std::uint8_t index = 0;
            const ArenaAttack& a = what == Press::Heavy  ? fi.heavy
                                   : what == Press::Shot ? fi.shot
                                                         : lightPick(riposteAt, index);
            return affords(a) ? PressReason::Ok : PressReason::Stamina;
        }
        case Press::Dodge: return outOfStamina() ? PressReason::Stamina : PressReason::Ok;
        case Press::Jump:
            if (kitLacks(what)) return PressReason::Absent;
            return outOfStamina() ? PressReason::Stamina : PressReason::Ok;
        case Press::Heal:
            if (locked) return PressReason::Overloaded;
            return pl.charges == 0 ? PressReason::Empty : PressReason::Ok;
        case Press::Release:
            if (kitLacks(what)) return PressReason::Absent;
            return pl.gauge.value <= fight.capNow() / 3 ? PressReason::Empty : PressReason::Ok;
        case Press::None: break;
        }
        return PressReason::Absent;
    }

    // The kit has no such move at all: no attacks of its own (ArenaFighter::attacks: light, heavy, shot and Release),
    // or jumpAir 0 (no jump).
    bool kitLacks(Press what) const {
        switch (what) {
        case Press::Light:
        case Press::Heavy:
        case Press::Shot:
        case Press::Release: return !fi.attacks;
        case Press::Jump: return !(fi.jumpAir > kArenaEps);
        case Press::Dodge:
        case Press::Heal:
        case Press::None: break;
        }
        return false;
    }

    // ArenaFight::pressReason, at t0 = the fight's time (the next tick's start): the order stepPlayer takes a press in,
    // from the same rules, reading only.
    PressReason pressReason(ArenaPress p) const {
        if (fight.phase_ == ArenaPhase::Defeat) return PressReason::Down;
        if (fight.phase_ != ArenaPhase::Fighting) return PressReason::Absent;
        Press what = Press::None;
        switch (p) {
        case ArenaPress::Light: what = Press::Light; break;
        case ArenaPress::Heavy: what = Press::Heavy; break;
        case ArenaPress::Shot: what = Press::Shot; break;
        case ArenaPress::Dodge: what = Press::Dodge; break;
        case ArenaPress::Jump: what = Press::Jump; break;
        case ArenaPress::Heal: what = Press::Heal; break;
        case ArenaPress::Release: what = Press::Release; break;
        case ArenaPress::Guard:
        case ArenaPress::Pair: break;
        }
        // A move the kit lacks is absent whatever the state: no heal charges at the start, a shot without a gun (a light
        // press is only a shot aimed with one).
        if (what != Press::None && kitLacks(what)) return PressReason::Absent;
        if (p == ArenaPress::Heal && fight.healMax_ == 0) return PressReason::Absent;
        if (p == ArenaPress::Shot && fi.weapon != ArenaWeapon::Guns) return PressReason::Absent;
        if (pl.hp <= 0 || pl.action == ArenaAction::Downed) return PressReason::Down;
        if (p == ArenaPress::Pair) {
            if (pairCooling(t0)) return PressReason::Cooldown; // over Closed: a press now deals nothing
            return fight.pairOpen() ? PressReason::Ok : PressReason::Closed;
        }
        if (pl.action == ArenaAction::GuardBreak) return PressReason::Broken; // canStart starts nothing until it ends
        // A hit pause over the tick's start: the press waits in the buffer (it starts in the tick the pause ends).
        const bool paused = pl.stopUntil > t0;
        if (p == ArenaPress::Guard) {
            if (pl.action == ArenaAction::Guard) return PressReason::Ok;
            // A buffered press that starts first takes the tick (the guard goes up once its action lets it).
            const bool first = pl.buffered != Press::None && !(pl.bufferAge > fi.bufferTime) &&
                               canStart(pl, fi, cutOf(pl.buffered)) && refusal(pl.buffered) == PressReason::Ok;
            if (paused || first) return PressReason::Busy;
            return canStart(pl, fi, Cut::Guard) ? PressReason::Ok : PressReason::Busy;
        }
        const PressReason r = refusal(what);
        if (r != PressReason::Ok) return r;
        return paused || !canStart(pl, fi, cutOf(what)) ? PressReason::Busy : PressReason::Ok;
    }

    // The start functions run only once refusal() is Ok.
    void startLight(const ArenaInput& in) {
        int riposteAt = -1;
        std::uint8_t index = 0;
        lightPick(riposteAt, index);
        if (riposteAt >= 0) {
            pl.chainLive = false;
            pl.soft = kArenaNobody;
            pl.homeClose = 0.f;
            pl.target = id(static_cast<std::size_t>(riposteAt));
            // Turned onto its enemy over startup (advanceAttack), not in one frame; with no startup, at once.
            if (fi.riposte.startup <= kArenaEps)
                pl.facing = arenaUnit(arenaSub(foes[static_cast<std::size_t>(riposteAt)].pos, pl.pos), pl.facing);
            beginAttack(ArenaAction::Riposte, kArenaRiposteMove, fi.riposte);
            return;
        }
        pl.chainNext = static_cast<std::uint8_t>((index + 1u) % fi.lightCount);
        pl.chainLive = true;
        faceAtStart(in, fi.light[index]);
        ++stats.lights;
        if (stringEnd(index)) ++stats.stringEnds;
        stats.lightStamina += std::max(0.f, fi.light[index].stamina);
        beginAttack(ArenaAction::Light, index, fi.light[index]);
    }

    // A shot press with the cylinder empty: counted, and dropped from the buffer.
    bool dryPress() {
        ++stats.dryPresses;
        emit(ArenaEventType::DryPress, kArenaPlayer, kArenaPlayer, kArenaShotMove, stats.dryPresses, pl.pos);
        return dropPress();
    }

    // A gun kit's aimed light: one round at the aim, no riposte (that stays the unaimed light's), and it ends a chain.
    void startShot(const ArenaInput& in) {
        if (fi.rounds > 0) --pl.rounds;
        pl.chainLive = false;
        ++stats.shots;
        faceAtStart(in, fi.shot);
        beginAttack(ArenaAction::Light, kArenaShotMove, fi.shot);
        pl.shotPitched = in.aimPitched;
        pl.shotPitch = in.aimPitch;
        pl.shotBody = in.aimBody;
    }

    void startHeavy(const ArenaInput& in) {
        pl.chainLive = false;
        faceAtStart(in, fi.heavy);
        beginAttack(ArenaAction::Heavy, kArenaHeavyMove, fi.heavy);
    }

    void startDodge(const ArenaInput& in) {
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
        pl.dodgeSide = 0;
        if (pl.lock != kArenaNobody) {
            const Vec2 to = arenaUnit(arenaSub(foes[pl.lock].pos, pl.pos), Vec2{-pl.dodgeDir.x, -pl.dodgeDir.y});
            const float along = arenaDot(pl.dodgeDir, to);
            pl.dodgeBack = along <= -kArenaDodgeBackCos;
            pl.dodgeSide = 0;
            if (fight.sideDashes_ && !pl.dodgeBack && along < kArenaDodgeBackCos)
                pl.dodgeSide = arenaCross(to, pl.dodgeDir) >= 0.f ? std::int8_t{1} : std::int8_t{-1};
        }
        spend(fi.dodgeStamina);
        pl.chainLive = false;
        pl.action = ArenaAction::Dodge;
        pl.actionTime = 0.f;
    }

    // The jump, from the ground: kArenaJumpStaminaShare of a dodge's stamina, refused as a dodge is (with none left).
    // The velocity carries the take-off's (no faster than a run).
    void startJump() {
        spend(kArenaJumpStaminaShare * fi.dodgeStamina);
        pl.chainLive = false;
        pl.action = ArenaAction::Jump;
        pl.actionTime = 0.f;
        pl.jumpTime = 0.f;
        pl.jumpVel = arenaClampLength(pl.velocity, fi.moveSpeed);
        pl.airLightUsed = false;
        ++stats.jumps;
    }

    // The one light a jump may strike in the air (a shot press there is one too): no riposte, no chain.
    void startAirLight(const ArenaInput& in) {
        pl.chainLive = false;
        pl.airLightUsed = true;
        ++stats.airLights;
        faceAtStart(in, fi.jumpLight);
        beginAttack(ArenaAction::Light, kArenaAirMove, fi.jumpLight);
    }

    void startHeal() {
        pl.chainLive = false;
        pl.action = ArenaAction::Heal;
        pl.actionTime = 0.f;
        pl.healApplied = false;
    }

    void startGuard() {
        pl.chainLive = false;
        pl.action = ArenaAction::Guard;
        pl.actionTime = 0.f;
    }

    // The Release: open only above a third of the cap (kArenaReleaseOpenLoad at the full cap), whatever the stamina
    // (the two gauges are apart). All the Load goes at once; the strike's hits turn it into poise damage.
    void startRelease(const ArenaInput& in) {
        pl.chainLive = false;
        faceAtStart(in, fi.release);
        pl.releaseSpent = pl.gauge.release();
        pl.releaseLone = fight.lone_;
        ++stats.releases;
        beginAttack(ArenaAction::Release, kArenaReleaseMove, fi.release);
        emit(ArenaEventType::Released, kArenaPlayer, kArenaNobody, kArenaReleaseMove, pl.releaseSpent, pl.pos);
    }

    // A refused attack press of a kit with no attacks: cleared from the buffer at once, so it can't wait there and
    // start nothing in place of a later press.
    bool dropPress() {
        pl.buffered = Press::None;
        return false;
    }

    // Starts the buffered press when refusal() lets it. An Absent one (a kit without the move) is dropped from the
    // buffer at once, so it can't wait there and start nothing in place of a later press; so is a shot on an empty
    // cylinder (a dry press). Any other refusal leaves it in the buffer, where it may yet start before it expires.
    bool startAction(Press what, const ArenaInput& in) {
        switch (refusal(what)) {
        case PressReason::Ok: break;
        case PressReason::Absent: return dropPress();
        case PressReason::Empty: return what == Press::Shot ? dryPress() : false;
        default: return false;
        }
        if (inAir()) {
            startAirLight(in);
            return true;
        }
        switch (what) {
        case Press::Light: startLight(in); break;
        case Press::Heavy: startHeavy(in); break;
        case Press::Shot: startShot(in); break;
        case Press::Dodge: startDodge(in); break;
        case Press::Jump: startJump(); break;
        case Press::Heal: startHeal(); break;
        case Press::Release: startRelease(in); break;
        case Press::None: return false;
        }
        return true;
    }

    // ---- the paired counter ----

    // A counter dealt less than pairCooldown before `at` (fight time) is cooling: a pair now deals nothing.
    bool pairCooling(float at) const { return at < pl.pairCoolUntil - kArenaEps; }

    // A parry and the partner's press within pairWindow of each other (either first) pair once: the parried enemy,
    // if it is still open, takes pairHits of light[0]'s hits and is staggered. Inside pairCooldown of the last counter
    // dealt the pair is spent and deals nothing (both leads share the clock: it lives in the player state a switch
    // keeps). A counter dealt with a partner on spends pairPartnerSeconds of the partner's attack time.
    void tryPair() {
        if (pl.pairFoe == kArenaNobody || std::fabs(pl.pairParry - pl.pairPress) > fi.pairWindow + kArenaEps) return;
        const std::size_t i = pl.pairFoe;
        pl.pairFoe = kArenaNobody;
        pl.pairParry = -1.0e9f;
        pl.pairPress = -1.0e9f;
        if (pairCooling(t0)) {
            ++stats.pairsCooled;
            return;
        }
        if (!alive(i) || foes[i].brain != ArenaBrain::Open) return;
        Enemy& foe = foes[i];
        const ArenaEnemyKind& k = def.enemies[i];
        const int damage = static_cast<int>(fi.pairHits) * fi.light[0].damage;
        const float pairPoise = k.boss ? fi.pairBossPoise * k.poise : k.poise; // a boss: a share of its poise
        ArenaBlowRuling r;
        if (!rule(ArenaEventType::CounterStagger, kArenaPlayer, id(i), 0, damage, pairPoise, pl.pos, pl.facing, foe.pos,
                  r))
            return;
        pl.pairCoolUntil = t0 + fi.pairCooldown; // dealt (the ruling let it through): the clock starts
        // A partner whose kit has no attacks (PartnerIdle::NoKit) pays nothing: the lead's counter is as with no partner.
        if (partnerOn() && pk.attacks) {
            pl.pairSpent += fi.pairPartnerSeconds;
            pl.pairBrain += fi.pairPartnerSeconds; // the brain's rule 2 budget: its spentSeconds at its next step
        }
        const int amount = lose(i, scaledDamage(damage, r.damage));
        stats.damageDealt += amount;
        ++stats.counterStaggers;
        notePlayerBlow(i, t1);
        emit(ArenaEventType::CounterStagger, kArenaPlayer, id(i), 0, amount, foe.pos);
        if (foe.hp <= 0) {
            killEnemy(i);
        } else if (k.boss) { // its share of the poise, as ruled: staggered only when that empties it, else still Open
            accrueBreak(i, kArenaBreakPair, r.breakScale);
            foe.poise -= scaledPoise(pairPoise, r.poise);
            foe.lastPoiseHit = t1;
            if (foe.poise <= 0.f) staggerEnemy(i);
        } else if (r.poise >= 1.f) {
            staggerEnemy(i); // it breaks the whole poise or nothing (NaN: nothing)
        }
    }

    // ---- Read ----

    // The open deal's move is still in its startup: a pick counts.
    bool readStillOpen() const {
        const std::size_t i = fight.readFoe_;
        if (!fight.read_.dealt() || !alive(i)) return false;
        const Enemy& foe = foes[i];
        return foe.brain == ArenaBrain::Attack && foe.move == fight.readMove_ &&
               foe.stateTime < fight.enemyAttackNow(i).startup;
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
        const int dealt = hitEnemy(i, damage, fi.readPoise, ArenaEventType::ReadCounter,
                                   static_cast<std::uint8_t>(pl.readStacks), kArenaBreakRead);
        if (dealt < 0) return; // vetoed
        ++stats.readCounters;
        stats.readDamage += dealt;
    }

    void stepPlayer(const ArenaInput& in) {
        aimOn = in.aim;
        // A shot keeps the pitch it started with (pitched or the flat lane) and follows the crosshair's climb and body
        // until it fires (the tick its startup ends takes that tick's), while the aim still gives one; from then on
        // they stay, hit or miss, through the active part.
        if (pl.action == ArenaAction::Light && pl.move == kArenaShotMove && pl.shotPitched && in.aimPitched &&
            pl.actionTime < fi.shot.startup) {
            pl.shotPitch = in.aimPitch;
            pl.shotBody = in.aimBody;
        }
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
        if (in.jump) press = Press::Jump;
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
        reloadStep();
        pl.wantSpeed = -1.f;
        if (!(playerOwn > 0.f)) {
            pl.velocity = Vec2{};
            return;
        }
        bool began = false;
        const bool inAttack = earlyCut(); // starting anything now cuts the attack before its plain cancel point
        // A chance: before the plain cancel point, a press or the guard waits while its own cut window on this attack
        // is open (canStart lets it through). Counted before the press's refusal, so one refused there is a chance
        // with no cancel. An attack with no own window for the cut (the default kit's, a heavy press) opens none, and
        // a press that left the buffer before its window opened waited in none.
        const bool pressCut = pl.buffered != Press::None && canStart(pl, fi, cutOf(pl.buffered));
        const bool guardCut = in.guard && canStart(pl, fi, Cut::Guard);
        if (inAttack && !pl.cancelChance && (pressCut || guardCut)) {
            pl.cancelChance = true;
            ++stats.cancelChances;
        }
        if (pressCut && startAction(pl.buffered, in)) {
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

    // ---- the cylinder ----

    // An empty cylinder refills `reload` seconds of the player's own time after the last round's shot has ended.
    void reloadStep() {
        if (fi.rounds == 0 || pl.rounds > 0) return;
        if (pl.reloadLeft > 0.f) {
            pl.reloadLeft -= playerOwn;
            if (pl.reloadLeft > kArenaEps) return;
        } else if (pl.action == ArenaAction::Light && pl.move == kArenaShotMove) {
            return; // the last round's shot is still under way
        } else if (fi.reload > kArenaEps) {
            pl.reloadLeft = fi.reload;
            emit(ArenaEventType::ReloadStarted, kArenaPlayer, kArenaPlayer, kArenaShotMove,
                 static_cast<int>(fi.reload * 1000.f + 0.5f), pl.pos);
            return;
        } else {
            emit(ArenaEventType::ReloadStarted, kArenaPlayer, kArenaPlayer, kArenaShotMove, 0, pl.pos);
        }
        pl.reloadLeft = 0.f;
        pl.rounds = fi.rounds;
        ++stats.reloads;
        emit(ArenaEventType::Reloaded, kArenaPlayer, kArenaPlayer, kArenaShotMove, static_cast<int>(pl.rounds), pl.pos);
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

    // The air clock from j0 to j1 (the jump's own time): the clear window, the landing, and the velocity it gives.
    Vec2 airStep(Vec2 stick, float j0, float j1) {
        const float up = fi.jumpStartup, down = fi.jumpStartup + fi.jumpAir - kArenaEps;
        pl.airClear = fi.jumpClearEnd > fi.jumpClearStart && j0 < fi.jumpClearEnd && j1 > fi.jumpClearStart;
        if (j0 < down && j1 >= down) ++stats.landings;
        pl.jumpTime = j1;
        if (j0 < up) return pl.jumpVel; // the take-off carries the run
        if (j0 < down) {                // in the air: the take-off's velocity, steered by the stick
            pl.jumpVel = arenaRamp(pl.jumpVel, arenaScaled(stick, fi.moveSpeed * aimSpeed), fi.jumpAirControl, dt);
            return pl.jumpVel;
        }
        return arenaRamp(pl.velocity, Vec2{}, fi.moveBrake, dt); // the landing brakes
    }

    void advancePlayer(const ArenaInput& in) {
        // The landing cuts an air light short: the rest is the jump's.
        if (pl.action == ArenaAction::Light && pl.move == kArenaAirMove && pl.jumpTime >= 0.f &&
            pl.jumpTime + dt >= fi.jumpStartup + fi.jumpAir - kArenaEps) {
            pl.action = ArenaAction::Jump;
            pl.actionTime = pl.jumpTime;
            pl.soft = kArenaNobody;
            pl.homeClose = 0.f;
        }
        const float a0 = pl.actionTime;
        const float a1 = a0 + dt;
        const Vec2 stick = arenaClampLength(in.move, 1.f);
        const bool pushed = arenaLength(stick) > kArenaEps;
        Vec2 vel{};
        pl.invulnerable = false;
        pl.airClear = false;
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
            if (pl.action == ArenaAction::Light && pl.move == kArenaAirMove && pl.jumpTime >= 0.f) {
                // An air light: the air clock runs on and moves the body; the attack only strikes.
                vel = airStep(stick, pl.jumpTime, pl.jumpTime + dt);
                advanceAttack(stick, pushed, a0, a1);
                break;
            }
            vel = advanceAttack(stick, pushed, a0, a1);
            break;
        case ArenaAction::Jump:
            faceFree(stick, pushed);
            vel = airStep(stick, a0, a1);
            pl.actionTime = a1;
            if (a1 >= fi.jumpStartup + fi.jumpAir + fi.jumpLand - kArenaEps) {
                pl.jumpTime = -1.f;
                endAction();
            }
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
        } else if (pl.action == ArenaAction::Dodge) {
            // a side dash: side-on to its way, facing across it on the target's side (its way to the body's side)
            const float s = static_cast<float>(pl.dodgeSide);
            const Vec2 way = pl.dodgeBack ? Vec2{-pl.dodgeDir.x, -pl.dodgeDir.y}
                             : pl.dodgeSide != 0 ? Vec2{s * pl.dodgeDir.y, -s * pl.dodgeDir.x}
                                                 : pl.dodgeDir;
            pl.drawFacing = arenaDrawTurn(pl.drawFacing, way,
                                      std::max(fi.turnRate, kArenaPi / (kArenaDodgeTurnShare * std::max(fi.dodgeTime, dt))) * dt);
        }
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
    void killEnemy(std::size_t i, bool report = true, float at = -1.f, std::uint8_t by = kArenaPlayer) {
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
        foe.breakTimed = false;
        foe.unseen = false;
        releaseToken(i);
        if (report) emit(ArenaEventType::Downed, by, id(i), 0, 0, foe.pos);
    }

    bool hostBlow(std::size_t i, int damage, std::uint8_t move, float breakScale = 1.f) {
        if (i >= n || !alive(i) || damage <= 0 || unhurt(foes[i])) return false;
        Enemy& foe = foes[i];
        const int amount = lose(i, damage);
        stats.damageDealt += amount;
        const bool killed = foe.hp <= 0;
        // A break it brings waits for the next tick; it lands before that tick, so the tick's whole dt is quiet after it.
        if (!killed) accrueBreak(i, kArenaBreakHost, breakScale, false);
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

    // `by`: whose blow broke the poise (the player, or the partner's kArenaAlly).
    void staggerEnemy(std::size_t i, std::uint8_t by = kArenaPlayer) {
        Enemy& foe = foes[i];
        foe.poise = def.enemies[i].poise;
        foe.breakHits = 0;
        interrupt(i, ArenaBrain::Staggered, by);
        ++stats.staggers;
        emit(ArenaEventType::Staggered, by, id(i), 0, 0, foe.pos);
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

    // An enemy blow's hp on the player: x the Lone tuning's taken while Lone (Load stores read the damage itself).
    // Then x the host's takenScale (rounded; 1 leaves it as it is).
    int hpTaken(int damage) const {
        return scaledTaken(fight.lone_ ? loneScale(damage, fight.loneTuning_.taken) : damage);
    }
    int scaledTaken(int hp) const {
        if (fight.takenScale_ == 1.f || hp <= 0) return hp;
        return static_cast<int>(std::lround(static_cast<double>(hp) * static_cast<double>(fight.takenScale_)));
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
    // A boss also gains breakWeight of break value (Boss break; a break it brings waits for the tick's break step).
    int hitEnemy(std::size_t i, int damage, float poise, ArenaEventType type, std::uint8_t move, float breakWeight) {
        Enemy& foe = foes[i];
        ArenaBlowRuling r;
        if (unhurt(foe) || !rule(type, kArenaPlayer, id(i), move, damage, poise, pl.pos, pl.facing, foe.pos, r)) return -1;
        poise = scaledPoise(poise, r.poise);
        const int amount = lose(i, scaledDamage(damage, r.damage));
        stats.damageDealt += amount;
        notePlayerBlow(i, t1);
        emit(type, kArenaPlayer, id(i), move, amount, foe.pos);
        if (foe.hp <= 0) {
            killEnemy(i);
        } else {
            accrueBreak(i, breakWeight, r.breakScale);
            poiseHit(i, poise);
        }
        return amount;
    }

    // Poise: only an enemy on its feet loses it; `armor` ignores a share of it during its attack's startup and active
    // parts; at 0 it staggers.
    void poiseHit(std::size_t i, float poise, std::uint8_t by = kArenaPlayer) {
        Enemy& foe = foes[i];
        if (poise <= 0.f || !upright(foe)) return;
        float share = 0.f;
        if (foe.brain == ArenaBrain::Attack) {
            const ArenaAttack& own = fight.enemyAttackNow(i);
            if (foe.stateTime < own.startup + own.active) share = std::clamp(own.armor, 0.f, 1.f);
        }
        foe.poise -= poise * (1.f - share);
        foe.lastPoiseHit = t1;
        if (foe.poise <= 0.f) staggerEnemy(i, by);
    }

    // One blow of a lead's body reaching enemy i: the player's (kArenaPlayer) or the partner's (kArenaAlly). The
    // one path both go through: the ruling, the hp, the Hit event, then (if it lives) break value, the break-out
    // count, poise and armour, a boss's break, a break-out and a flinch. `poise` and `breakWeight` are the striker's
    // (the partner's already at the data's partner scales). Only the player's blow adds the player's stats (hits,
    // damageDealt, the pace and punish counters), the hit pause, a Lone Release's scale and the riposte's punish;
    // the partner's hp is partnerBlowDamage of the ruled damage, counted in partnerDamage. Neither stores Load here
    // (Load comes from parries).
    struct LeadBlow {
        std::uint8_t striker = kArenaPlayer;
        const ArenaAttack* attack = nullptr;
        std::uint8_t move = 0;
        Vec2 from{};
        Vec2 facing{};
        float poise = 0.f;
        float breakWeight = 0.f;
    };
    // The last light of a kit with two or more: the chain only moves forward, so starting it means the string was full.
    bool stringEnd(std::uint8_t index) const { return fi.lightCount >= 2 && index + 1u == fi.lightCount; }

    // A blow's rules (landBlowRules), then a player's string-end count and its push (ArenaAttack::push).
    void landBlow(std::size_t i, const LeadBlow& b) {
        const bool player = b.striker == kArenaPlayer;
        const int hits = stats.hits;
        const int flinches = stats.flinches;
        landBlowRules(i, b);
        if (!player || stats.hits == hits) return; // vetoed, or the ally's
        if (pl.action == ArenaAction::Light && stringEnd(pl.move)) ++stats.stringEndHits;
        shove(i, *b.attack, stats.flinches != flinches);
    }

    // The player's blow `a` landed on enemy i: its push, straight away from the lead over pushTime of the enemy's
    // own time (a paused enemy's stands still, so it starts after the hit pause). A flinch from the same blow
    // already moved it flinchPush: the shove adds the rest, so the total is the larger of the two.
    void shove(std::size_t i, const ArenaAttack& a, bool flinched) {
        if (!(a.push > 0.f) || !(a.pushTime > 0.f) || !alive(i) || def.enemies[i].boss) return;
        // A body that can't walk (a practice target, a planted kind) is not shoved: nothing brings it back, and each
        // string would carry it on, the lead after it, to the wall.
        if (!(def.enemies[i].walkSpeed > 0.f) && !(def.enemies[i].runSpeed > 0.f)) return;
        Enemy& foe = foes[i];
        if (breakGuarded(foe) || foe.brain == ArenaBrain::StepBack) return; // a step back is its own way out
        if (foe.brain == ArenaBrain::Attack) {
            const ArenaAttack& own = fight.enemyAttackNow(i);
            const ArenaPart part = arenaPartOf(own, foe.stateTime);
            if (part == ArenaPart::Active || (part == ArenaPart::Startup && own.armor > 0.f)) return;
        }
        const float metres = flinched ? a.push - def.enemies[i].flinchPush : a.push;
        if (!(metres > 0.f)) return;
        foe.shoveVel = arenaScaled(arenaUnit(arenaSub(foe.pos, pl.pos), pl.facing), metres / a.pushTime);
        foe.shoveLeft = a.pushTime; // a special's shove still running gives way to this one
        ++stats.pushes;
    }

    void landBlowRules(std::size_t i, const LeadBlow& b) {
        const ArenaAttack& a = *b.attack;
        const bool player = b.striker == kArenaPlayer;
        Enemy& foe = foes[i];
        ArenaBlowRuling r;
        if (!rule(ArenaEventType::Hit, b.striker, id(i), b.move, a.damage, b.poise, b.from, b.facing, foe.pos, r)) return;
        int amount = 0;
        if (player) {
            // A Release begun while Lone: x the tuning's release on the ruled scale, kept under the single-blow cap.
            const float scale = pl.action == ArenaAction::Release && pl.releaseLone
                                    ? loneReleaseScale(r.damage, fight.loneTuning_.release, fight.loneBlowCap_)
                                    : r.damage;
            amount = lose(i, scaledDamage(a.damage, scale));
            ++stats.hits;
            if (pl.move == kArenaShotMove) ++stats.shotHits;
            notePlayerBlow(i, t1);
            stats.damageDealt += amount;
        } else {
            if (!alive(i) || unhurt(foe)) return; // the hook's own doing
            const float raw = static_cast<float>(scaledDamage(a.damage, r.damage));
            // The cap is the striking lead's own light: the ally's kit (pk), so a switch never changes what it deals.
            const float lead = static_cast<float>(pk.light[0].damage);
            amount = lose(i, static_cast<int>(std::lround(partnerBlowDamage(raw, lead, *fight.partnerTuning_))));
            stats.partnerDamage += amount;
            ++stats.partnerBlows;
        }
        emit(ArenaEventType::Hit, b.striker, id(i), b.move, amount, foe.pos);
        if (player) { // the pause: the player has moved this tick, the enemies not yet
            pause(pl.stopUntil, t1, a.hitStop * std::clamp(a.hitStopSelf, 0.f, 1.f));
            pause(foe.stopUntil, t0, a.hitStop);
        }
        if (foe.hp <= 0) {
            killEnemy(i, true, -1.f, b.striker);
            return;
        }
        accrueBreak(i, b.breakWeight, r.breakScale); // a boss's (Boss break)
        if (player && pl.action == ArenaAction::Riposte && foe.brain == ArenaBrain::Open) {
            // The riposte punishes the open enemy (and ends a hold).
            foe.brain = ArenaBrain::Down;
            foe.stateTime = 0.f;
            foe.holdFor = -1.f;
            ++stats.ripostes;
            return;
        }
        const float dealt = scaledPoise(b.poise, r.poise);
        countBlow(i, dealt);
        poiseHit(i, dealt, b.striker);
        if (!alive(i) || !upright(foe)) return; // staggered (or the read counter it brought ended it)
        if (tryBossBreak(i, b.striker)) return;
        if (breakOut(i, b.striker)) return;
        // Informational: every blow heavy enough. flinch() counts the chances its own rules leave (flinchChances).
        if (couldFlinch(i, dealt)) ++stats.flinchBlows;
        flinch(i, dealt, b.striker);
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
                standing[i] = foes[i].brain != ArenaBrain::Dead && !unhurt(foes[i]);
            }
            float distance = 0.f;
            const std::uint8_t first = arenaShotFirst(a, pl.pos, pl.facing, pl.shotPitch, def.enemies.data(), at.data(),
                                                      standing.data(), n, pl.hitMask, pl.shotBody, distance);
            if (first != kArenaNobody) reached[count++] = first;
        } else {
            for (std::size_t i = 0; i < n; ++i) {
                if (foes[i].brain == ArenaBrain::Dead || unhurt(foes[i]) || (pl.hitMask & (1u << i)) != 0u) continue;
                // Every blow and the shot's flat lane take the body as drawn (bodyRadius times a skinned look's
                // scale), as the pitched round does.
                const float radius = arenaBodyCylinder(def.enemies[i], foes[i].pos, 0.f).radius;
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
            pl.hitMask = static_cast<std::uint8_t>(pl.hitMask | (1u << i)); // spent, even when vetoed
            LeadBlow b;
            b.attack = &a;
            b.move = pl.move;
            b.from = pl.pos;
            b.facing = pl.facing;
            // A Release adds the Load it spent, times releasePoise, to the poise damage.
            b.poise = a.poiseDamage +
                      (pl.action == ArenaAction::Release ? static_cast<float>(pl.releaseSpent) * fi.releasePoise : 0.f);
            b.breakWeight = strikeBreakWeight(pl.action);
            landBlow(i, b);
        }
    }

    // On its feet: the states poise damage, a flinch and a break-out count in.
    static bool upright(const Enemy& foe) {
        return foe.brain == ArenaBrain::Approach || foe.brain == ArenaBrain::Strafe || foe.brain == ArenaBrain::Attack ||
               foe.brain == ArenaBrain::Flinch;
    }

    // Stepping back from a break-out (StepBack): no blow lands on it.
    static bool unhurt(const Enemy& foe) { return foe.brain == ArenaBrain::StepBack; }

    // A player's blow of `poise` ruled poise damage landed on enemy i: its weight toward the break-out (poise over
    // kArenaBreakPoiseUnit; a boss's blow 1), while it is upright and not in that move.
    void countBlow(std::size_t i, float poise) {
        Enemy& foe = foes[i];
        const ArenaEnemyKind& k = def.enemies[i];
        if (k.breakAt == 0 || !upright(foe)) return;
        if (foe.brain == ArenaBrain::Attack && static_cast<int>(foe.move) == k.breakMove) return;
        ++stats.breakUpright;
        if (!foe.flinchAllowed) { // the host's veto bars it as it bars a flinch
            ++stats.breakVetoed;
            return;
        }
        if (t1 - foe.lastBreakHit > k.breakDecay) {
            foe.breakHits = 0;
            foe.breakWeight = 0.f;
        }
        if (foe.breakHits < 255) ++foe.breakHits;
        foe.breakWeight += k.boss ? 1.f : std::max(0.f, poise) / kArenaBreakPoiseUnit;
        foe.lastBreakHit = t1;
        // The best run so far, compared as a share of each kind's breakAt.
        if (stats.breakRunNeed == 0 ||
            foe.breakWeight * static_cast<float>(stats.breakRunNeed) > stats.breakRunWeight * static_cast<float>(k.breakAt)) {
            stats.breakRunBest = foe.breakHits;
            stats.breakRunNeed = k.breakAt;
            stats.breakRunWeight = foe.breakWeight;
        }
    }

    // The tokens enemies other than i hold.
    std::size_t tokensBesides(std::size_t i) const {
        std::size_t held = 0;
        for (std::size_t j = 0; j < n; ++j)
            if (j != i && foes[j].brain != ArenaBrain::Dead && foes[j].token) ++held;
        return held;
    }

    // The count reached breakAt: the break move starts at once, whatever its cooldown. A regular enemy needs a token
    // for it as for any attack, and the windup arc: without either it steps back instead (StepBack).
    // `by`: whose blow brought it (kArenaPlayer or the partner's kArenaAlly), for interrupt's Read counter rule.
    bool breakOut(std::size_t i, std::uint8_t by = kArenaPlayer) {
        Enemy& foe = foes[i];
        const ArenaEnemyKind& k = def.enemies[i];
        if (k.breakAt == 0 || !foe.flinchAllowed || foe.breakWeight + kArenaBreakWeightSlack < static_cast<float>(k.breakAt))
            return false;
        ++stats.breakDue;
        foe.breakHits = 0; // spent, even when the move is barred
        foe.breakWeight = 0.f;
        if (k.breakMove < 0 || k.breakMove >= static_cast<int>(k.moveCount) || ((foe.moveMask >> k.breakMove) & 1u) == 0u) {
            ++stats.breakBarred;
            return false;
        }
        if (!k.boss && ((!foe.token && tokensBesides(i) >= def.attackers) || !inWindupArc(i))) {
            const StepPlan plan = planStep(i);
            if (plan.moves < kArenaStepBackNone) {
                ++stats.stepBacksBlocked; // no room any way: no step, the blow lands
                return false;
            }
            stepBack(i, plan, by);
            return true;
        }
        const std::uint8_t move = static_cast<std::uint8_t>(k.breakMove);
        interrupt(i, ArenaBrain::Approach, by); // a move cut short gives its token back (and a read move brings its counter)
        if (!alive(i) || foe.brain != ArenaBrain::Approach) {
            --stats.breakDue; // the counter put it down first: not due after all
            return true;
        }
        takeToken(i);
        ++stats.breakOuts;
        emit(ArenaEventType::BrokeOut, id(i), kArenaPlayer, move, 0, foe.pos);
        beginMove(i, move);
        return true;
    }

    // The lead's reach: its longest light or heavy range plus lunge (homing aside).
    float leadReach() const {
        float reach = fi.heavy.range + std::max(0.f, fi.heavy.lunge);
        const std::size_t lights = std::min<std::size_t>(fi.lightCount, fi.light.size());
        for (std::size_t m = 0; m < lights; ++m) reach = std::max(reach, fi.light[m].range + std::max(0.f, fi.light[m].lunge));
        return reach;
    }

    // A break-out with no token free, or out of the windup arc: enemy i steps straight back, unhurt, to
    // kArenaStepBackClear beyond the lead's reach and at least kArenaStepBackMin over kArenaStepBackTime (SteppedBack,
    // once); its count is spent and it walks again after. Only as far as the ring lets it: with less than
    // kArenaStepBackRoom behind, it steps to the side with more room instead, if that has more (SteppedBack's move
    // says which: 0 back, 1 its left, 2 its right, as it faces the lead).
    // Where enemy i's step would go, from where it stands now: the unit way, the distance it tries over
    // kArenaStepBackTime, how far it gets (the ring's room) and SteppedBack's move.
    struct StepPlan {
        Vec2 way{-1.f, 0.f};
        float distance = 0.f;
        float moves = 0.f;
        std::uint8_t wayId = 0;
    };
    StepPlan planStep(std::size_t i) const {
        const Enemy& foe = foes[i];
        StepPlan p;
        const float radius = arenaBodyCylinder(def.enemies[i], foe.pos, 0.f).radius;
        const float clear = leadReach() + radius + kArenaStepBackClear;
        const float want = std::max(kArenaStepBackMin, clear - arenaDistance(pl.pos, foe.pos)); // the blow seen to miss
        // The ring keeps the body as the velocity cut does: its own bodyRadius in from the edge.
        const float body = def.enemies[i].bodyRadius;
        const Vec2 toLead = arenaUnit(arenaSub(pl.pos, foe.pos), foe.facing);
        const Vec2 back{-toLead.x, -toLead.y};
        p.way = back;
        p.distance = std::min(want, arenaRoomAlong(fight.ring_, foe.pos, back, body)); // it stops at the edge
        p.moves = p.distance;
        if (p.distance < kArenaStepBackRoom) {
            // Square to the way to the lead; y is to the right on the ground, so its right is (-y, x) of that way.
            const Vec2 left{toLead.y, -toLead.x};
            const Vec2 right{-toLead.y, toLead.x};
            const float leftRoom = arenaRoomSliding(fight.ring_, foe.pos, left, want, body);
            const float rightRoom = arenaRoomSliding(fight.ring_, foe.pos, right, want, body);
            const bool toRight = rightRoom > leftRoom; // a tie goes left
            const float side = std::min(want, toRight ? rightRoom : leftRoom);
            if (side > p.distance) {
                p.wayId = toRight ? 2 : 1;
                p.way = toRight ? right : left;
                // It tries the whole step at the same pace; the ring's slide leaves it `side` from where it was.
                p.distance = want;
                p.moves = side;
            }
        }
        return p;
    }

    void stepBack(std::size_t i, const StepPlan& plan, std::uint8_t by = kArenaPlayer) {
        Enemy& foe = foes[i];
        // A move cut short gives its token back, and a read move brings its counter while it can still be hurt.
        interrupt(i, ArenaBrain::Approach, by);
        if (!alive(i) || foe.brain != ArenaBrain::Approach) {
            --stats.breakDue; // the counter put it down first: not due after all
            return;
        }
        foe.brain = ArenaBrain::StepBack;
        foe.stepWay = plan.way;
        foe.stepDistance = plan.distance;
        ++stats.stepBacks;
        emit(ArenaEventType::SteppedBack, id(i), kArenaPlayer, plan.wayId, static_cast<int>(std::lround(plan.moves * 100.f)),
             foe.pos);
    }

    // Enemy i where the player can see a windup begin: within kArenaWindupArc of the lead's facing, or within the
    // camera's half-angle (setCameraHalfAngle) of the camera's facing (when the host gives one), seen from the lead.
    // On top of it: seen.
    bool inWindupArc(std::size_t i) const {
        const Vec2 to = arenaSub(foes[i].pos, pl.pos);
        const float distance = arenaLength(to);
        if (distance <= kArenaEps) return true;
        const Vec2 way = arenaScaled(to, 1.f / distance);
        constexpr float kDegrees = 3.14159265358979f / 180.f;
        if (arenaDot(way, pl.facing) >= std::cos(kArenaWindupArc * kDegrees)) return true;
        const float camLength = arenaLength(camera);
        return camLength > kArenaEps &&
               arenaDot(way, arenaScaled(camera, 1.f / camLength)) >= std::cos(fight.cameraHalfAngle_ * kDegrees);
    }

    // Where a token holder may begin its string: a boss anywhere, a regular enemy in the windup arc.
    bool inSight(std::size_t i) const { return def.enemies[i].boss || inWindupArc(i); }

    // ---- the boss break value (Boss break) ----

    // A player's strike's weight, by the action it is (a shot and an air light are lights).
    static float strikeBreakWeight(ArenaAction a) {
        switch (a) {
        case ArenaAction::Heavy: return kArenaBreakHeavy;
        case ArenaAction::Release: return kArenaBreakRelease;
        case ArenaAction::Riposte: return kArenaBreakRiposte;
        default: return kArenaBreakLight;
        }
    }

    static void clearBreak(Enemy& foe) {
        foe.breakValue = 0.f;
        foe.breakQuiet = 0.f;
        foe.breakFresh = false;
        foe.breakTell = false;
    }

    // In its break move (windup, active part and recovery): blows add no value.
    static bool inBreakMove(const Enemy& foe) { return foe.brain == ArenaBrain::Attack && foe.breakTimed; }

    // In its break move's windup or active part: blows add no value and never flinch it.
    static bool breakGuarded(const Enemy& foe) {
        return foe.brain == ArenaBrain::Attack && foe.breakTimed &&
               foe.stateTime < foe.breakAttack.startup + foe.breakAttack.active;
    }

    // A blow of the player's side landed on boss i (after its ruling): weight x scale, at most the blow cap. Not while
    // it is Staggered, nor in any part of its break move.
    void accrueBreak(std::size_t i, float weight, float scale, bool inTick = true) {
        Enemy& foe = foes[i];
        if (!def.enemies[i].boss || !alive(i) || foe.brain == ArenaBrain::Staggered || inBreakMove(foe)) return;
        const float add = std::min(weight * (scale > 0.f ? scale : 0.f), kArenaBossBreakBlowCap); // NaN: 0
        if (!(add > 0.f)) return;
        foe.breakValue += add;
        foe.breakQuiet = 0.f;
        foe.breakFresh = inTick; // a blow inside a tick lands at its end: that tick's dt is not quiet
        stats.bossBreakAdded += add;
    }

    // The tell, then the break once it is due and upright: its break move, with its own windup and
    // recovery. True when it began (the blow that brought it then neither flinches nor counts).
    // `by`: whose blow brought it (the decay's own call: kArenaPlayer); the partner's never brings the Read counter.
    bool tryBossBreak(std::size_t i, std::uint8_t by = kArenaPlayer) {
        Enemy& foe = foes[i];
        const ArenaEnemyKind& k = def.enemies[i];
        if (!k.boss || !alive(i)) return false;
        const float at = arenaBreakAtOf(k, foe.hp);
        const bool tell = foe.breakValue > 0.f && foe.breakValue >= kArenaBossBreakTell * at;
        if (tell && !foe.breakTell) {
            ++stats.bossBreakTells;
            emit(ArenaEventType::BreakTell, id(i), kArenaPlayer, 0, static_cast<int>(std::lround(foe.breakValue * 100.f)),
                 foe.pos);
        }
        foe.breakTell = tell;
        const bool full = foe.breakValue >= at;
        if (!full || !upright(foe)) return false;
        clearBreak(foe);
        if (k.breakMove < 0 || k.breakMove >= static_cast<int>(k.moveCount) || ((foe.moveMask >> k.breakMove) & 1u) == 0u) {
            ++stats.bossBreakBarred; // spent
            return false;
        }
        const std::uint8_t move = static_cast<std::uint8_t>(k.breakMove);
        // A read move cut short brings its Read counter: held back until the break move has begun, so it lands as a
        // Read counter in the windup (damage and poise, no value) and a stagger it brings cancels the break.
        const bool counter = foe.readRight && by == kArenaPlayer;
        foe.readRight = false;
        interrupt(i, ArenaBrain::Approach, by); // a move cut short gives its token back
        foe.token = true;
        ++stats.bossBreaks;
        emit(ArenaEventType::BrokeOut, id(i), kArenaPlayer, move, 0, foe.pos);
        beginMove(i, move, true);
        if (counter) {
            const bool own = foe.readRight; // a read on the break move itself, kept for when it is cut
            foe.readRight = true;
            readCounter(i);
            foe.readRight = own && alive(i) && inBreakMove(foe);
        }
        return true;
    }

    // Each tick, after the enemies: the decay, then tryBossBreak. While it is Staggered neither the value nor the
    // decay's delay clock (breakQuiet) moves, so a stagger never costs the player the count.
    void stepBossBreaks() {
        for (std::size_t i = 0; i < n; ++i) {
            if (!def.enemies[i].boss || !alive(i)) continue;
            Enemy& foe = foes[i];
            if (foe.breakValue > 0.f) {
                const bool fresh = foe.breakFresh; // a blow added this tick: its quiet starts at the next
                foe.breakFresh = false;
                if (!fresh && foe.brain != ArenaBrain::Staggered) {
                    // Only the part of this tick's quiet past the delay falls.
                    const float was = std::max(foe.breakQuiet - kArenaBossBreakDecayDelay, 0.f);
                    foe.breakQuiet += dt;
                    const float now = std::max(foe.breakQuiet - kArenaBossBreakDecayDelay, 0.f);
                    foe.breakValue -= (now - was) * kArenaBossBreakDecayRate;
                    if (!(foe.breakValue > 0.f)) clearBreak(foe); // never below 0
                }
            }
            tryBossBreak(i);
        }
    }

    // A blow that could flinch enemy i: a kind that flinches, the host allows it, and the blow carries flinchPoise.
    bool couldFlinch(std::size_t i, float poise) const {
        const ArenaEnemyKind& k = def.enemies[i];
        return k.flinchTime > 0.f && foes[i].flinchAllowed && poise >= k.flinchPoise;
    }

    // A landed blow of at least flinchPoise flinches enemy i, unless its attack is active or armoured.
    void flinch(std::size_t i, float poise, std::uint8_t by = kArenaPlayer) {
        Enemy& foe = foes[i];
        if (!couldFlinch(i, poise) || breakGuarded(foe)) return;
        // No stunlock: the run of flinches starts again only after a gap with no blow that could flinch it.
        if (t1 - foe.lastFlinch > kArenaFlinchChainGap) foe.flinchChain = 0;
        foe.lastFlinch = t1;
        if (foe.flinchChain >= kArenaMaxFlinchChain) return; // the blow lands, the enemy fights on
        if (foe.brain == ArenaBrain::Attack) {
            const ArenaAttack& own = fight.enemyAttackNow(i);
            const ArenaPart part = arenaPartOf(own, foe.stateTime);
            if (part == ArenaPart::Active || (part == ArenaPart::Startup && own.armor > 0.f)) return;
        }
        // A chance the rules above left: counted before interrupt, so a flinch path that never fires reads FAIL.
        ++stats.flinchChances;
        interrupt(i, ArenaBrain::Flinch, by);
        if (!alive(i) || foe.brain != ArenaBrain::Flinch) return; // the read counter it brought ended it
        ++foe.flinchChain;
        ++stats.flinches;
        emit(ArenaEventType::Flinched, by, id(i), 0, 0, foe.pos);
    }

    // ---- the enemies ----

    void releaseToken(std::size_t i) {
        foes[i].token = false;
        foes[i].asked = 0.f;
    }

    void takeToken(std::size_t i) {
        foes[i].token = true;
        foes[i].asked = 0.f;
        foes[i].tokenSince = t0;
    }

    // Leaves the current state for `brain` (Staggered, Open): the attack stops and the token goes back.
    // A read move cut short brings the Read counter at once, but only when the lead's own blow (or the fight, `by`
    // kArenaPlayer) cut it: the counter belongs to whoever read the move. Cut by the partner's body (kArenaAlly) the
    // read is spent with no counter, no read stack used and nothing credited to the lead.
    void interrupt(std::size_t i, ArenaBrain brain, std::uint8_t by = kArenaPlayer) {
        Enemy& foe = foes[i];
        foe.brain = brain;
        if (!upright(foe)) { // Open, Down, Staggered, StepBack: the count starts again once it is up
            foe.breakHits = 0;
            foe.breakWeight = 0.f;
        }
        foe.stateTime = 0.f;
        foe.stringEnd = t1;
        foe.holdFor = -1.f;
        foe.breakTimed = false;
        foe.unseen = false;
        releaseToken(i);
        if (by == kArenaPlayer)
            readCounter(i);
        else
            foe.readRight = false;
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
    // up to `attackers` held at once. Then a holder out of sight that has not begun its string hands its token to the
    // asker in sight waiting longest (then the nearest) and asks again from 0 itself; with none in sight it keeps it.
    // One handed a token keeps it kArenaTokenHandHold s, and one that handed its token over takes none by hand-over
    // for as long, so a body at the arc's edge doesn't pass it back and forth.
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
        // The asker waiting longest, then the nearest (in sight only, when `sighted`); -1 for none.
        const auto bestAsker = [&](bool sighted) {
            int best = -1;
            float bestDist = 0.f;
            for (std::size_t i = 0; i < n; ++i) {
                if (!asking[i] || (sighted && (!inSight(i) || t0 - foes[i].handedAwayAt < kArenaTokenHandHold))) continue;
                const float distance = arenaDistance(pl.pos, foes[i].pos);
                const bool better = best < 0 || foes[i].asked > foes[static_cast<std::size_t>(best)].asked ||
                                    (foes[i].asked == foes[static_cast<std::size_t>(best)].asked && distance < bestDist);
                if (better) {
                    best = static_cast<int>(i);
                    bestDist = distance;
                }
            }
            return best;
        };
        while (held < def.attackers) {
            const int best = bestAsker(false);
            if (best < 0) break;
            const std::size_t winner = static_cast<std::size_t>(best);
            asking[winner] = false;
            takeToken(winner);
            ++held;
        }
        for (std::size_t h = 0; h < n; ++h) {
            const Enemy& holder = foes[h];
            if (!holder.token || (holder.brain != ArenaBrain::Approach && holder.brain != ArenaBrain::Strafe) ||
                t0 - holder.handedAt < kArenaTokenHandHold || inSight(h))
                continue;
            const int best = bestAsker(true);
            if (best < 0) continue;
            const std::size_t winner = static_cast<std::size_t>(best);
            asking[winner] = false;
            releaseToken(h); // it asks again from the next tick, its wait from 0
            foes[h].handedAwayAt = t0;
            takeToken(winner);
            foes[winner].handedAt = t0;
            ++stats.tokenHandOvers;
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

    // timed: a boss's break (Boss break), with its own windup and recovery. unseen: a regular string begun outside the
    // windup arc after kArenaUnseenWait, its windup kArenaUnseenWindup longer.
    // chained: the next move of a string under way (endMove), which keeps the string's aim.
    void beginMove(std::size_t i, std::uint8_t move, bool timed = false, bool unseen = false, bool chained = false) {
        Enemy& foe = foes[i];
        // A string aimed at the partner, which is down now: the blow under way finished where it was aimed; from this
        // one the boss aims at the standing lead (the cap's counts stay as they were).
        if (chained && foe.aim == 2 && !allyUp()) foe.aim = 1;
        if (!chained) { // a string begins: whom it aims at, and a boss's count toward the partner's cap
            const bool boss = def.enemies[i].boss;
            const bool atPartner = boss && partnerAim(i);
            foe.aim = atPartner ? 2 : 1;
            if (boss) {
                ++fight.aimStrings_;
                ++stats.bossAttacks;
                if (atPartner) {
                    ++fight.aimPartner_;
                    ++stats.bossAimedPartner;
                }
                stats.bossAimShareMax = std::max(stats.bossAimShareMax, static_cast<float>(fight.aimPartner_) /
                                                                            static_cast<float>(fight.aimStrings_));
            }
        }
        foe.brain = ArenaBrain::Attack;
        foe.move = move;
        foe.stateTime = 0.f;
        foe.hitPlayer = false;
        foe.hitAlly = false;
        foe.evaded = false;
        foe.callFrom = t0;
        foe.cooldown[move] = def.enemies[i].moves[move].cooldown;
        foe.readRight = false;
        foe.breakTimed = timed;
        if (timed) {
            foe.breakAttack = def.enemies[i].moves[move].attack;
            foe.breakAttack.startup = kArenaBossBreakWindup;
            foe.breakAttack.recovery = kArenaBossBreakRecovery;
        }
        foe.unseen = unseen && !timed;
        if (foe.unseen) {
            foe.unseenAttack = def.enemies[i].moves[move].attack;
            foe.unseenAttack.startup += kArenaUnseenWindup;
            ++stats.unseenAttacks;
        }
        emit(ArenaEventType::AttackStarted, id(i), kArenaNobody, move, 0, foe.pos);
        dealRead(i, move);
    }

    // The move ended: its `chain` move follows at once, else the string is over and the token goes back.
    void endMove(std::size_t i) {
        Enemy& foe = foes[i];
        const ArenaEnemyKind& k = def.enemies[i];
        foe.breakTimed = false;
        foe.unseen = false;
        const int chain = k.moves[foe.move].chain;
        if (chain >= 0 && chain < static_cast<int>(k.moveCount) && chain != static_cast<int>(foe.move)) {
            beginMove(i, static_cast<std::uint8_t>(chain), false, false, true);
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
        const ArenaAttack& a = fight.enemyAttackNow(i);
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
        // In the air's clear window a low move (hitsAir false) passes under: told once per attack, like a roll's.
        if (pl.airClear && !mv.hitsAir) {
            if (!foe.evaded) {
                foe.evaded = true;
                ++stats.evades;
                ++stats.airEvades;
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
            // A BLOCK call (blockable, not parryable) met by a guard pressed since the move began: blockCallGuard of
            // the cost. A guard held from before it pays the whole guardDamage, as does a PARRY call merely blocked.
            const bool called = !mv.parryable && pl.lastGuardPress >= foe.callFrom - kArenaEps;
            const float cost = called ? a.guardDamage * std::clamp(fi.blockCallGuard, 0.f, 1.f) : a.guardDamage;
            if (called) ++stats.blockCalls;
            const float before = pl.stamina;
            pl.stamina = std::max(0.f, before - cost);
            pl.lastSpend = t0;
            ++stats.blocks;
            emit(ArenaEventType::Blocked, source, kArenaPlayer, foe.move,
                 static_cast<int>(std::lround(before - pl.stamina)), pl.pos);
            // Carrying, a block stores a share of the blocked damage (a guard break too).
            if (fight.carry_) storeCapped(source, foe.move, static_cast<float>(damage) * fi.blockLoadShare, stats.blockLoad);
            const int chip = std::max(0, static_cast<int>(std::lround(hpTaken(damage) * (1.f - fi.guardAbsorb))));
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
        damagePlayer(source, hpTaken(damage), foe.move);
        pause(pl.stopUntil, t1, a.hitStop); // both have moved this tick
        // Its own blow's table share may have killed it: then nothing more touches it this tick.
        if (alive(i)) pause(foe.stopUntil, t1, a.hitStop * std::clamp(a.hitStopSelf, 0.f, 1.f));
        if (pl.hp <= 0) return;
        if (airborne()) {
            // A blow that lands in the air ends the jump: knocked out of it, whatever the poise (or the height).
            pl.chainLive = false;
            pl.action = ArenaAction::Hitstun;
            pl.actionTime = 0.f;
            pl.velocity = Vec2{};
            pl.jumpTime = -1.f;
            pl.airClear = false;
            return;
        }
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

    bool overloadedNow() const { return pl.gauge.value >= fight.capNow(); }

    // The chairs' Load gain: 1 + their loadGain while the ally is alive within their radius of the player, else 1.
    // While Lone, the Lone tuning's gain instead.
    float loadGain() const {
        if (fight.lone_) return fight.loneTuning_.gain;
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
        const int room = std::max(0, fight.capNow() - pl.gauge.value);
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
        const int room = std::max(0, fight.capNow() - pl.gauge.value);
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

    int hostHeal(int amount) {
        const int room = std::max(std::max(fi.hp, 1) - pl.hp, 0);
        const int added = pl.hp > 0 ? std::min(amount, room) : 0;
        if (added <= 0) return 0;
        pl.hp += added;
        if (fight.inHook_.on) emit(ArenaEventType::Healed, kArenaNobody, kArenaPlayer, 0, added, pl.pos);
        else pl.hostHealed += added;
        return added;
    }
    int hostHurt(int amount) {
        const int taken = std::min(amount, pl.hp - 1);
        if (taken <= 0) return 0;
        pl.hp -= taken;
        stats.damageTaken += taken;
        if (fight.inHook_.on) emit(ArenaEventType::HostHit, kArenaNobody, kArenaPlayer, 0, taken, pl.pos);
        else pl.hostHurt += taken;
        return taken;
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
        capChanged(was);
    }

    // The host's setLoad: the lead's own Load, silent but for the overload it reaches.
    void setLoad(int value) {
        const bool was = overloadedNow();
        pl.gauge.value = fi.attacks ? std::clamp(value, 0, fight.capNow()) : 0;
        out.player.load = pl.gauge.value;
        if (!overloadedNow()) { // an Overloaded not reported yet was the old value's, not this lead's
            pl.capFilled = false;
            pl.giftFilled = false;
        }
        capChanged(was);
    }

    // The host's setLone: the Load stays; reaching the cap it now reads overloads once.
    void setLone(bool on, const LoneTuning& t, float blowCap) {
        const bool was = overloadedNow();
        fight.lone_ = on && validLoneTuning(t);
        fight.loneTuning_ = validLoneTuning(t) ? t : LoneTuning{};
        fight.loneBlowCap_ = validBlowCap(blowCap) ? blowCap : 0.f;
        capChanged(was);
    }

    void capChanged(bool was) {
        out.player.loadCap = fight.capNow();
        out.player.overloaded = overloadedNow();
        if (fight.phase_ != ArenaPhase::Fighting || was || !overloadedNow()) return;
        ++stats.overloads;
        if (fight.inHook_.on) emit(ArenaEventType::Overloaded, kArenaPlayer, kArenaPlayer, 0, pl.gauge.value, pl.pos);
        else pl.capFilled = true;
    }

    // ---- the ally ----

    bool allyUp() const { return fight.allyMaxHp_ > 0 && fight.allyHp_ > 0; }

    // ---- the partner (ArenaFight::setPartner) ----

    bool partnerOn() const {
        return fight.partner_ != nullptr && fight.partnerTuning_ != nullptr && fight.allyMaxHp_ > 0 && !fight.lone_;
    }

    // The partner's k-th blow: its own kit's light, wrapping at its chain's length.
    const ArenaAttack& allyLight(std::uint8_t k) const {
        return pk.light[static_cast<std::size_t>(k) % std::max<std::size_t>(pk.lightCount, 1u)];
    }

    // Per-lead kits: the bodies swapped (a switch, a takeover), so the kits do, in place (fi and pk stay bound).
    // Leads whose kits differ in max hp: the max goes with its lead too, the player's (the kit's hp) and the ally's
    // (setAlly's) trading places, so a switch clips neither; after it each kit's hp is its body's max (partnerKit.hp
    // the partner's, as allyMaxHp). Each body keeps its own hp under its own max, so a trade neither loses nor makes
    // health. Kits of one max hp (both leads on one kit, as every fight before per-lead kits): nothing trades, the
    // player's max stays the kit's and the ally's setAlly's, exactly as before. Twice is none. With no ally
    // (setAlly(0), as a host puts the partner out while swapped) the player's max is its kit's hp and the ally stays
    // none.
    void swapKits() {
        ArenaDef& d = fight.def_;
        const int playerMax = d.fighter.hp;
        const int allyMax = fight.allyMaxHp_;
        const bool trade = allyMax > 0 && d.fighter.hp != d.partnerKit.hp;
        std::swap(d.fighter, d.partnerKit);
        if (trade) {
            d.fighter.hp = allyMax;
            d.partnerKit.hp = playerMax;
            fight.allyMaxHp_ = playerMax;
        }
        fight.kitsSwapped_ = !fight.kitsSwapped_;
    }

    // The player now drives the other lead (after swapKits and standInAlly). The rounds and the reload travel with
    // their kit: the one now held gets back what it had when it was let go (a reload part done goes on from where it
    // stopped), the one let go keeps what the player left in it. Nothing is refilled. Its stamina is held to its kit's.
    void takeKit() {
        std::swap(pl.rounds, fight.otherRounds_);
        std::swap(pl.reloadLeft, fight.otherReload_);
        pl.stamina = std::min(pl.stamina, fi.stamina);
    }

    // A boss string begun now would aim at the partner: it is up and nearer the boss than the lead (centre to
    // centre), and one more partner-aimed string keeps the fight's share within kArenaBossAimShareTenths. No RNG.
    bool partnerAim(std::size_t i) const {
        if (!def.enemies[i].boss || !partnerOn() || !allyUp()) return false;
        const Vec2 at = foes[i].pos;
        if (!(arenaDistance(at, fight.allyPos_) < arenaDistance(at, pl.pos))) return false;
        return (fight.aimPartner_ + 1) * 10 <= kArenaBossAimShareTenths * (fight.aimStrings_ + 1);
    }

    // The enemy the lead's attack this step is aimed at: its lock, else its soft target, else the nearest living one
    // within the riposte's dot of its facing; -1 for none.
    int leadAimed() const {
        if (alive(pl.lock)) return pl.lock;
        if (alive(pl.soft)) return pl.soft;
        int best = -1;
        float bestDist = 0.f;
        for (std::size_t i = 0; i < n; ++i) {
            if (!alive(i)) continue;
            const Vec2 to = arenaSub(foes[i].pos, pl.pos);
            const float d = arenaLength(to);
            if (d > kArenaEps && arenaDot(pl.facing, arenaScaled(to, 1.f / d)) < kArenaRiposteDot) continue;
            if (best < 0 || d < bestDist) {
                best = static_cast<int>(i);
                bestDist = d;
            }
        }
        return best;
    }

    // The partner's string, guard and hitstun end; it stands where it is.
    void clearAlly() {
        ArenaFight::AllyState& al = fight.ally_;
        al.attacking = false;
        al.blowTime = 0.f;
        al.landed = false;
        al.target = -1;
        al.guarding = false;
        al.hurtLeft = 0.f;
        al.goal = fight.allyPos_;
        resetAllyWalk();
    }

    // The ally reached 0 hp.
    void downAlly(std::uint8_t source, std::uint8_t move) {
        fight.allyHp_ = 0;
        clearAlly();
        emit(ArenaEventType::AllyDowned, source, kArenaAlly, move, 0, fight.allyPos_);
    }

    // The brain's view of this step, its intent, and the ally body's blow. After the player's step, before the
    // enemies'.
    void stepAlly(const ArenaInput& in) {
        ArenaFight::AllyState& al = fight.ally_;
        if (!partnerOn()) {
            al.attacking = false;
            al.guarding = false;
            al.hurtLeft = 0.f;
            al.goal = fight.allyPos_;
            resetAllyWalk();
            return;
        }
        PartnerBrain& brain = *fight.partner_;
        al.hurtLeft = std::max(0.f, al.hurtLeft - dt);
        const bool leadOn = pl.action == ArenaAction::Light || pl.action == ArenaAction::Heavy;
        const int leadAim = leadOn ? leadAimed() : -1;
        std::array<PartnerEnemyView, kMaxArenaEnemies> views{};
        for (std::size_t i = 0; i < n; ++i) {
            const Enemy& foe = foes[i];
            PartnerEnemyView& v = views[i];
            v.pos = foe.pos;
            const ArenaBodyCylinder body = arenaBodyCylinder(def.enemies[i], foe.pos, 0.f);
            v.radius = body.radius;
            v.base = body.base; // rule 18: over the floor, as the camera's groundZ
            v.top = body.top;
            v.kind = def.enemies[i].boss ? PartnerEnemyView::Kind::Boss : PartnerEnemyView::Kind::Regular;
            if (foe.brain == ArenaBrain::Dead) {
                v.state = PartnerEnemyView::State::Down;
            } else if (foe.brain == ArenaBrain::Staggered) {
                v.state = PartnerEnemyView::State::Staggered;
            } else if (foe.brain == ArenaBrain::Attack) {
                const float startup = fight.enemyAttackNow(i).startup;
                if (foe.stateTime < startup) {
                    v.state = PartnerEnemyView::State::WindingUp;
                    v.windupLeft = startup - foe.stateTime;
                    v.aim = foe.aim == 2 ? PartnerEnemyView::Aim::Partner : PartnerEnemyView::Aim::Lead;
                }
            }
            v.leadAttacking = static_cast<int>(i) == leadAim;
        }
        const bool hurt = al.hurtLeft > 0.f;
        PartnerInput pin;
        pin.order = in.partnerOrder == 0 ? PartnerOrder::StayClose : PartnerOrder::Spread;
        pin.pos = fight.allyPos_;
        pin.health = static_cast<float>(fight.allyHp_) / static_cast<float>(fight.allyMaxHp_);
        pin.canAct = allyUp() && !hurt;
        pin.doing = al.attacking ? PartnerDoing::Attacking
                    : al.guarding ? PartnerDoing::Guarding
                    : hurt        ? PartnerDoing::Hurt
                    : al.moved    ? PartnerDoing::Moving
                                  : PartnerDoing::Idle;
        pin.leadPos = pl.pos;
        pin.leadTarget = alive(pl.lock) ? static_cast<int>(pl.lock) : -1;
        pin.leadAttacking = leadOn;
        for (std::size_t k = 0; k < kMaxPartnerBlows; ++k) pin.blowSeconds[k] = allyLight(static_cast<std::uint8_t>(k)).total();
        // A paired counter's partner time (outside its strings): this step's if the player's counter came first, else
        // the next step's (a counter dealt in the enemies' step lands after the brain stepped).
        pin.spentSeconds = pl.pairBrain;
        pl.pairBrain = 0.f;
        // No kit (a partner kit with no attacks): its budget reads spent every step, so the brain begins no string
        // (NoBudget) and still guards, moves and repositions; the time is counted as no kit's.
        const bool noKit = !pk.attacks;
        pin.kitAttacks = !noKit; // the brain's NoKit: no string, no next blow, no paired counter (tryPair)
        if (noKit) {
            pin.spentSeconds = fight.partnerTuning_->budgetWindowSeconds;
            if (allyUp()) stats.partnerNoKitSeconds += dt;
        }
        pin.enemies = std::span<const PartnerEnemyView>(views.data(), n);
        // Stationing (rules 8-12): only with the host's camera. The ring less the ally's body is where it may stand.
        // The full camera (partnerCamera given and whole) turns it on too, from its own place and forward, for rules
        // 13-16. Given but not whole is exactly not given.
        PartnerStationView& st = pin.station;
        const PartnerCameraView& cam = in.partnerCamera;
        const bool whole = partnerCameraWhole(cam);
        st.on = in.cameraOn || whole;
        if (st.on) {
            if (whole) {
                st.cameraPos = {cam.pos.x, cam.pos.y};
                st.cameraForward = {cam.forward.x, cam.forward.y};
                st.camera = cam;
            } else {
                st.cameraPos = in.cameraPos;
                st.cameraForward = (in.cameraFacing.x != 0.f || in.cameraFacing.y != 0.f) ? in.cameraFacing : in.view;
            }
            st.leadFacing = pl.facing;
            st.arenaCentre = fight.ring_.centre;
            st.arenaRadius = std::max(1e-3f, fight.ring_.radius - fight.allyRadius_); // never 0 (unbounded)
            st.close = in.partnerClose; // rule 19: the close camera (not given: the classic rules)
        }
        pin.range = {pk.partnerReach, pk.partnerStandoff}; // rule 19 reads it under the close camera alone
        pin.facing = al.facing; // rule 19: a string starts facing its target (read under the close camera alone)
        // Rule 19 (11-17): and, from a standstill, with its drawn heading within 60 degrees of it (likewise). Arriving
        // from a walk it is not asked: the heading turned to the target over the walk's end, so no pause.
        pin.heading = al.headingWalked ? Vec2{} : al.heading;
        const PartnerIntent& intent = brain.step(static_cast<double>(dt), pin);
        al.act = static_cast<std::uint8_t>(intent.act);
        al.stationed = intent.stationed;
        al.intercept = intent.intercept;
        if (!allyUp()) { // down: it lies where it fell until the brain stands it
            clearAlly();
            if (!intent.rise) return;
            const long want = std::lround(static_cast<double>(intent.riseHealth) * static_cast<double>(fight.allyMaxHp_));
            fight.allyHp_ = static_cast<int>(std::clamp<long>(want, 1, fight.allyMaxHp_));
            ++stats.partnerRises;
            emit(ArenaEventType::AllyRose, kArenaNobody, kArenaAlly, 0, fight.allyHp_, fight.allyPos_);
            return;
        }
        if (intent.act == PartnerAct::Attack && intent.newString && !noKit) ++stats.partnerStrings;
        if (intent.act == PartnerAct::Attack && intent.blowStart && !noKit) { // the only step a blow starts on
            al.attacking = true;
            al.blow = static_cast<std::uint8_t>(std::clamp(intent.blowIndex, 0, static_cast<int>(kMaxPartnerBlows) - 1));
            al.blows = static_cast<std::uint8_t>(std::clamp(intent.blows, 0, static_cast<int>(kMaxPartnerBlows)));
            al.blowTime = 0.f;
            al.landed = false;
            al.target = intent.enemy;
            const float total = allyLight(al.blow).total();
            al.blowLength = total >= 0.1f && total <= 2.f ? total : fight.partnerTuning_->blowSeconds;
        }
        al.guarding = intent.act == PartnerAct::Guard && !al.attacking;
        if (al.attacking) {
            al.bodyTurn = false;
            // Rule 19's kit range: under the close camera a kit with its own reach strikes from it (the caster's 5-8 m);
            // otherwise (classic, no camera) the light's own range, as ever.
            // Under the close camera its facing also follows the target in recovery (classic: startup only, as main).
            runAllyBlow(partnerRangeOn(pin, *fight.partnerTuning_) ? pin.range.reach : 0.f,
                        in.partnerClose.given ? pk.turnRate : 0.f);
            return;
        }
        const bool walk = intent.act == PartnerAct::MoveTo || intent.act == PartnerAct::StepBack;
        al.goal = shapeAllyWalk(in, intent, walk);
        al.target = intent.enemy;
        Vec2 look = al.facing;
        if (intent.enemy >= 0 && alive(static_cast<std::size_t>(intent.enemy)))
            look = arenaSub(foes[static_cast<std::size_t>(intent.enemy)].pos, fight.allyPos_);
        else if (walk)
            look = arenaSub(intent.point, fight.allyPos_);
        al.facing = arenaTurn(al.facing, arenaUnit(look, al.facing), pk.turnRate * dt);
    }

    // ---- the partner's walk: the brain's point as the body walks to it ----

    // Its walk forgets the old body (down, risen, swapped, the partner off).
    void resetAllyWalk() {
        ArenaFight::AllyState& al = fight.ally_;
        al.around = -1;
        al.headingCount = 0;
        al.headingSince = -1.f;
        al.bodyTurn = false;
    }

    // The goal this tick (the host walks straight to it, pushed out of the living enemies' bodies). Standing: its place.
    // Walking rule 17's circling (its never-still goal, or its budget spent): round a body in the way (skirmishing, not
    // its own target's: the never-still point circles that one, and going round it as well doubled its reversals);
    // and, unless a wind-up at it forces the step (StepBack), within the heading rule's arc. Every other walk is the
    // brain's point as before: on its rules' stations the heading rule cost the camera's frame (StayClose's group
    // fight 0.97 -> 0.75 on screen, its goal jumps 28 -> 36 per 10 s), and rounding there cost Spread's worst seed.
    Vec2 shapeAllyWalk(const ArenaInput& in, const PartnerIntent& intent, bool walk) {
        fight.ally_.bodyTurn = false;
        if (!walk) {
            fight.ally_.around = -1;
            return fight.allyPos_;
        }
        // Rule 17's walk shaping (round bodies, heading held) for its own goals and a wait on the budget; under the
        // close camera rule 19 replaces rule 17 and its NoBudget walk is the brain's as given (its near field, rule 1).
        const bool circling = intent.skirmish || (intent.idle == PartnerIdle::NoBudget && !in.partnerClose.given);
        const Vec2 goal = circling ? allyRound(in, intent.point, intent.skirmish ? intent.enemy : -1) : intent.point;
        return intent.act == PartnerAct::StepBack || !circling ? goal : allyHeadingHeld(goal);
    }

    // The headings its walk moved on since its last reversal and within kAllyHeadingGap, newest first, while `f`
    // returns true.
    template <class F> void forAllyHeadings(F&& f) const {
        const ArenaFight::AllyState& al = fight.ally_;
        const std::size_t slots = ArenaFight::AllyState::kHeadingSlots;
        for (std::size_t k = 0; k < al.headingCount; ++k) {
            const std::size_t i = (al.headingNext + slots - 1 - k) % slots;
            if (fight.time_ - al.headingAt[i] >= kAllyHeadingGap || !f(al.headings[i])) return;
        }
    }

    // The heading rule's arc now (radians from `ref`, its newest heading): within kAllyHeadingHold of its last
    // reversal (or its walk's start) a heading it walks on lies within 90 degrees of each since; false: no rule now.
    bool allyHeadingArc(Vec2& ref, float& lo, float& hi) const {
        const ArenaFight::AllyState& al = fight.ally_;
        if (al.headingSince < 0.f || fight.time_ - al.headingSince >= kAllyHeadingHold + 0.5f * dt) return false;
        bool any = false;
        float a0 = -kArenaPi, a1 = kArenaPi;
        forAllyHeadings([&](Vec2 h) {
            if (!any) ref = h;
            any = true;
            const float a = std::atan2(arenaCross(ref, h), arenaDot(ref, h));
            a0 = std::max(a0, a - 0.5f * kArenaPi);
            a1 = std::min(a1, a + 0.5f * kArenaPi);
            return true;
        });
        if (!any) return false;
        lo = a0 + kAllyHeadingMargin;
        hi = a1 - kAllyHeadingMargin;
        if (lo > hi) lo = hi = 0.5f * (a0 + a1);
        return true;
    }

    // `goal` held to the arc: the nearest allowed heading at the same distance (a goal far past the arc: it walks the
    // arc's edge, not standing, so the hold costs no time in one place). A held line into a living enemy's body is
    // not walked: the arc's other edge if that one is clear, else the body forces the turn, as a wind-up does.
    Vec2 allyHeadingHeld(Vec2 goal) {
        const Vec2 from = fight.allyPos_;
        const Vec2 want = arenaSub(goal, from);
        const float far = arenaLength(want);
        Vec2 ref{1.f, 0.f};
        float lo = 0.f, hi = 0.f;
        if (far <= kAllyMovingSpeed * dt || !allyHeadingArc(ref, lo, hi)) return goal;
        const float w = std::atan2(arenaCross(ref, want), arenaDot(ref, want));
        const float c = std::clamp(w, lo, hi);
        if (c == w) return goal;
        for (const float a : {c, c == lo ? hi : lo}) {
            const Vec2 held = arenaMadd(from, arenaRotate(ref, a), far);
            if (allyLineClear(from, held)) return held;
        }
        fight.ally_.bodyTurn = true;
        return goal;
    }

    // The straight walk from `from` to `to` keeps outside every living enemy's body (touching allowed).
    bool allyLineClear(Vec2 from, Vec2 to) const {
        const Vec2 line = arenaSub(to, from);
        const float len2 = arenaDot(line, line);
        for (std::size_t i = 0; i < n; ++i) {
            if (!alive(i)) continue;
            const float touch = fight.allyRadius_ + def.enemies[i].bodyRadius;
            const Vec2 e = arenaSub(foes[i].pos, from);
            const float t = len2 > kArenaEps ? std::clamp(arenaDot(e, line) / len2, 0.f, 1.f) : 0.f;
            if (arenaDistance(arenaMadd(from, line, t), foes[i].pos) < touch &&
                arenaDistance(from, foes[i].pos) >= touch - kArenaEps) // already inside: the host pushes it out
                return false;
        }
        return true;
    }

    // The heading this tick's goal walks it on, for the heading rule (after stepAlly; a blow's lunge included).
    // A heading over 90 degrees from one kept is a reversal: the ring empties and the hold starts again from it; so
    // does a walk begun with none kept.
    void noteAllyHeading() {
        ArenaFight::AllyState& al = fight.ally_;
        if (!partnerOn() || !allyUp()) {
            al.headingCount = 0;
            al.headingSince = -1.f;
            return;
        }
        const Vec2 v = arenaSub(al.goal, fight.allyPos_);
        const float far = arenaLength(v);
        if (!(dt > 0.f) || far <= kAllyMovingSpeed * dt) return;
        const Vec2 d = arenaScaled(v, 1.f / far);
        const float now = fight.time_;
        bool kept = false, reversed = false;
        forAllyHeadings([&](Vec2 h) {
            kept = true;
            reversed = arenaDot(d, h) < 0.f;
            return !reversed;
        });
        if (reversed || !kept) {
            al.headingCount = 0;
            al.headingSince = now;
        }
        const std::size_t slots = ArenaFight::AllyState::kHeadingSlots;
        const std::size_t last = (al.headingNext + slots - 1) % slots;
        if (al.headingCount == 0 || now - al.headingAt[last] >= kAllyHeadingStep) {
            al.headings[al.headingNext] = d;
            al.headingAt[al.headingNext] = now;
            al.headingNext = (al.headingNext + 1) % slots;
            al.headingCount = std::min(slots, al.headingCount + 1);
        }
    }

    // The walk to `to` round a living enemy's body in the way: on the side of the smaller detour (square on: the
    // camera's side; with the play camera whole, a side whose passing point is out of its frame loses to one in it),
    // that side held until the body is passed, the line clears it or the goal comes to it. Body `skip` is never gone
    // round (-1: none).
    Vec2 allyRound(const ArenaInput& in, Vec2 to, int skip) {
        ArenaFight::AllyState& al = fight.ally_;
        const Vec2 from = fight.allyPos_;
        const Vec2 line = arenaSub(to, from);
        const float len = arenaLength(line);
        if (len <= kArenaEps) {
            al.around = -1;
            return to;
        }
        const Vec2 u = arenaScaled(line, 1.f / len);
        const Vec2 left{-u.y, u.x};
        // Body i blocks the line: ahead, short of the goal, within its clearance of the line, the goal outside it. A
        // body held is kept on a wider band, so it is not let go and taken again tick by tick.
        const auto blocks = [&](std::size_t i, bool held, float& off) {
            if (!alive(i) || static_cast<int>(i) == skip) return false;
            const float clear = fight.allyRadius_ + def.enemies[i].bodyRadius + kAllyAroundMargin;
            const Vec2 e = arenaSub(foes[i].pos, from);
            const float along = arenaDot(e, u);
            off = arenaCross(u, e);
            const float goalOff = arenaDistance(to, foes[i].pos);
            if (held) return along > 0.f && std::fabs(off) < clear + kAllyAroundMargin && goalOff > clear;
            return along > 0.f && along < len && std::fabs(off) < clear && goalOff > clear + 2.f * kAllyAroundMargin;
        };
        float off = 0.f;
        if (al.around >= 0 && !blocks(static_cast<std::size_t>(al.around), true, off)) al.around = -1;
        if (al.around < 0) {
            float nearest = 1e9f;
            for (std::size_t i = 0; i < n; ++i) {
                float o = 0.f;
                if (!blocks(i, false, o)) continue;
                const float a = arenaDot(arenaSub(foes[i].pos, from), u);
                if (a >= nearest) continue;
                nearest = a;
                al.around = static_cast<int>(i);
                off = o;
            }
            if (al.around < 0) return to;
            const std::size_t i = static_cast<std::size_t>(al.around);
            const PartnerCameraView& cam = in.partnerCamera;
            const bool whole = partnerCameraWhole(cam);
            float side = off > 0.f ? -1.f : 1.f; // past the side the body is not on
            if (std::fabs(off) < 0.05f) {
                const Vec2 eye = whole ? Vec2{cam.pos.x, cam.pos.y} : in.cameraOn ? in.cameraPos : pl.pos;
                side = arenaCross(u, arenaSub(eye, from)) >= 0.f ? 1.f : -1.f;
            }
            if (whole) {
                const float pass = fight.allyRadius_ + def.enemies[i].bodyRadius + 2.f * kAllyAroundMargin;
                const bool here = partnerInFrame(cam, kPartnerFrustumMargin, arenaMadd(foes[i].pos, left, side * pass));
                const bool there = partnerInFrame(cam, kPartnerFrustumMargin, arenaMadd(foes[i].pos, left, -side * pass));
                if (!here && there) side = -side;
            }
            al.aroundSide = side;
        }
        // Along the tangent to the body's passing circle on the held side (straight out from inside it).
        const std::size_t i = static_cast<std::size_t>(al.around);
        const float pass = fight.allyRadius_ + def.enemies[i].bodyRadius + 2.f * kAllyAroundMargin;
        const Vec2 toBody = arenaSub(foes[i].pos, from);
        const float d = arenaLength(toBody);
        const float turn = d > pass ? std::asin(pass / d) : 0.5f * kArenaPi;
        const float ahead = d > pass ? std::sqrt(d * d - pass * pass) : 0.f;
        const Vec2 dir = arenaRotate(arenaUnit(toBody, u), al.aroundSide * turn);
        return arenaMadd(from, dir, std::min(len, std::max(kAllyAroundStep, ahead)));
    }

    // The partner's running blow: the lead's light's tracking and lunge, and its one landing in the active part, from
    // `closeReach` m when that is further than the light's range (the kit's own reach under the close camera; 0: none).
    // Past the active part (recovery, before the string's next blow) its facing follows its target at the kit's
    // `turnRate`, at most kAllyLinkTurn, so a target crossing mid-string does not leave it behind (0: never).
    void runAllyBlow(float closeReach, float turnRate) {
        ArenaFight::AllyState& al = fight.ally_;
        const ArenaAttack& a = allyLight(al.blow);
        const float a0 = al.blowTime;
        const float a1 = a0 + dt;
        const bool aimed = al.target >= 0 && alive(static_cast<std::size_t>(al.target));
        if (al.target >= 0 && !aimed && !al.landed && a0 < a.startup + a.active) { // its target died before the hit
            ++stats.partnerBlowsVoid;                                              // frame: it deals nothing and ends
            al.attacking = false;
            al.blowTime = 0.f;
            al.goal = fight.allyPos_;
            return;
        }
        if (aimed && a0 < a.startup) {
            const Vec2 to = arenaSub(foes[static_cast<std::size_t>(al.target)].pos, fight.allyPos_);
            al.facing = arenaTurn(al.facing, arenaUnit(to, al.facing), a.tracking * (std::min(a1, a.startup) - a0));
        }
        if (aimed && !al.landed && a1 > a.startup && a0 < a.startup + a.active)
            allyStrike(static_cast<std::size_t>(al.target), closeReach);
        const float hitEnd = a.startup + a.active;
        if (aimed && turnRate > 0.f && a1 > hitEnd) {
            const Vec2 to = arenaSub(foes[static_cast<std::size_t>(al.target)].pos, fight.allyPos_);
            const float rate = std::min(turnRate, kAllyLinkTurn);
            al.facing = arenaTurn(al.facing, arenaUnit(to, al.facing), rate * (a1 - std::max(a0, hitEnd)));
        }
        // The lunge, spread over startup and active as the lead's: this tick's part of it is in the goal.
        const float span = a.startup + a.active;
        float lunge = 0.f;
        if (span > kArenaEps && a.lunge != 0.f) lunge = a.lunge / span * std::max(0.f, std::min(a1, span) - a0);
        al.goal = arenaMadd(fight.allyPos_, al.facing, lunge);
        al.blowTime = a1;
        if (a1 + kArenaEps >= al.blowLength) { // it has run out
            al.attacking = false;
            al.blowTime = 0.f;
            al.landed = false;
        }
    }

    // The partner's blow reaches enemy i (its string's target): the player's blow path (landBlow) with the partner as
    // the striker, its poise and break weight at the data's partner scales; no hit pause, punish credit or riposte,
    // and nothing in the player's stats or Load. Its range is the light's, or `closeReach` when further.
    void allyStrike(std::size_t i, float closeReach) {
        ArenaFight::AllyState& al = fight.ally_;
        Enemy& foe = foes[i];
        if (unhurt(foe)) return;
        ArenaAttack a = allyLight(al.blow);
        if (closeReach > a.range) a.range = closeReach;
        const float radius = arenaBodyCylinder(def.enemies[i], foe.pos, 0.f).radius;
        if (!arenaReaches(fight.allyPos_, al.facing, a, foe.pos, radius)) return;
        al.landed = true; // connected (or vetoed): spent
        // The lead's own blow path, at the data's partner scales (ArenaDef::partner).
        LeadBlow b;
        b.striker = kArenaAlly;
        b.attack = &a;
        b.move = al.blow;
        b.from = fight.allyPos_;
        b.facing = al.facing;
        b.poise = a.poiseDamage * def.partner.poise;
        b.breakWeight = kArenaBreakLight * def.partner.breakWeight;
        landBlow(i, b);
    }

    // The player fell with the partner up: the bodies swap (TookOver), and no Defeat. The player stands in the
    // ally's place at its hp, Idle; the fallen body is the ally now, down where the player fell.
    void takeOver() {
        ArenaFight::AllyState& al = fight.ally_;
        const Vec2 fellAt = pl.pos;
        const Vec2 fellFacing = pl.facing;
        swapKits();
        standInAlly();
        takeKit();
        fight.allyPos_ = fellAt;
        fight.allyHp_ = 0;
        clearAlly();
        al.facing = fellFacing;
        fight.swapped_ = !fight.swapped_;
        ++stats.takeovers;
        partnerSwapped();
        emit(ArenaEventType::TookOver, kArenaAlly, kArenaPlayer, 0, pl.hp, pl.pos);
        emit(ArenaEventType::AllyDowned, kArenaNobody, kArenaAlly, 0, 0, fight.allyPos_);
    }

    // The host's switchLeads: both up, the player takes the partner's body and the partner the player's.
    bool switchLeads() {
        ArenaSwitchRefusal& why = fight.switchRefusal_;
        // The order is ArenaSwitchRefusal's: the permanent reason wins, the busy one is last.
        why = fight.phase_ == ArenaPhase::NotStarted                     ? ArenaSwitchRefusal::NotFighting
              : !partnerOn()                                             ? ArenaSwitchRefusal::NoPartner
              : !pk.playable                                             ? ArenaSwitchRefusal::NotPlayable
              : fight.inHook_.on || fight.phase_ != ArenaPhase::Fighting ? ArenaSwitchRefusal::NotFighting
              : pl.hp <= 0                                               ? ArenaSwitchRefusal::PlayerDown
              : !allyUp()                                                ? ArenaSwitchRefusal::PartnerDown
                                                                         : ArenaSwitchRefusal::Ok;
        if (why == ArenaSwitchRefusal::Ok) {
            const std::size_t outgoing = playerSlot();
            if (!doSwitch()) return false;
            specialSwitchLoss(outgoing);
            return true;
        }
        ++stats.leadSwitchesRefused;
        if (why == ArenaSwitchRefusal::NotPlayable) ++stats.leadSwitchesNotPlayable;
        return false;
    }

    bool doSwitch() {
        ArenaFight::AllyState& al = fight.ally_;
        const Vec2 wasAt = pl.pos;
        const Vec2 wasFacing = pl.facing;
        const int wasHp = pl.hp;
        swapKits();
        standInAlly();
        takeKit();
        fight.allyPos_ = wasAt;
        fight.allyHp_ = std::min(wasHp, fight.allyMaxHp_);
        clearAlly();
        al.facing = wasFacing;
        fight.swapped_ = !fight.swapped_;
        ++stats.leadSwitches;
        partnerSwapped();
        return true;
    }

    // The partner's brain drives the other body now (a switch, a takeover): what it knew of the old one goes.
    void partnerSwapped() {
        // Counted only when the brain itself took it (its swaps() moved): a brain not begun ignores it.
        if (!fight.partner_ || !partnerOn()) return;
        const std::uint32_t before = fight.partner_->swaps();
        fight.partner_->rolesSwapped();
        fight.ally_.stationed = false;
        if (fight.partner_->swaps() != before) ++stats.partnerSwaps;
    }

    // The player stands in the ally's place at the ally's hp, Idle, its action and string, buffer, lock and poise
    // as a fresh body's (a takeover's and a switch's).
    void standInAlly() {
        const ArenaFight::AllyState& al = fight.ally_;
        pl.pos = fight.allyPos_;
        pl.facing = al.facing;
        pl.drawFacing = al.facing;
        pl.hp = std::min(fight.allyHp_, std::max(fi.hp, 1));
        pl.action = ArenaAction::Idle;
        pl.actionTime = 0.f;
        pl.velocity = Vec2{};
        pl.attackWalk = Vec2{};
        pl.buffered = Press::None;
        pl.bufferAge = 0.f;
        pl.parryLeft = 0.f;
        pl.chainLive = false;
        pl.hitMask = 0;
        pl.soft = kArenaNobody;
        pl.target = kArenaNobody;
        pl.homeClose = 0.f;
        pl.invulnerable = false;
        pl.stopUntil = -1.0e9f;
        pl.jumpTime = -1.f;
        pl.airClear = false;
        pl.airLightUsed = false;
        pl.poise = fi.poise;
    }

    // One of enemy i's attacks reaches the ally: the ruling, then take half if a press is waiting for it.
    void enemyStrikeAlly(std::size_t i) {
        Enemy& foe = foes[i];
        const ArenaAttack& a = fight.enemyAttackNow(i);
        const std::uint8_t source = id(i);
        foe.hitAlly = true; // connected (or vetoed): spent on the ally
        ArenaBlowRuling r;
        if (!rule(ArenaEventType::Hit, source, kArenaAlly, foe.move, a.damage, a.poiseDamage, foe.pos, foe.facing,
                  fight.allyPos_, r))
            return;
        // The partner's guard: a blow that is not unblockable is spent on it, no hp and no take half.
        if (partnerOn() && fight.ally_.guarding && !def.enemies[i].moves[foe.move].unblockable) {
            ++stats.partnerGuards;
            emit(ArenaEventType::AllyBlocked, source, kArenaAlly, foe.move, 0, fight.allyPos_);
            return;
        }
        const int damage = scaledDamage(a.damage, r.damage);
        if (damage <= 0) return;
        if (partnerOn()) { // it lands: the partner is held in its own kit's hitstun, which cuts its running blow
            ArenaFight::AllyState& al = fight.ally_;
            al.hurtLeft = std::max(al.hurtLeft, pk.hitstun);
            al.guarding = false;
            if (al.attacking) { // as the lead's: no damage from it after this, and the time its brain committed stays spent
                ++stats.partnerBlowsCut;
                al.attacking = false;
                al.blowTime = 0.f;
                al.landed = false;
                al.goal = fight.allyPos_;
            }
        }
        int allyPart = damage;
        bool tookHalf = false; // the chair's heal follows take half, never a table share
        // Take half: a press within the window before this blow, the player near enough and free to take it. Not
        // while invulnerable (a dodge's i-frames, a riposte, a Release: the blow would skip guard, poise and hurt),
        // nor while hurt (hitstun) or guard-broken.
        const float window = fi.takeHalfWindow + fight.chairs_.takeHalfWindow();
        const bool pressed = pl.takeHalfPress >= t1 - window - kArenaEps && pl.takeHalfPress <= t1;
        const bool canTake = pl.hp > 0 && !pl.invulnerable && pl.action != ArenaAction::Downed &&
                             pl.action != ArenaAction::Release && pl.action != ArenaAction::Hitstun &&
                             pl.action != ArenaAction::GuardBreak && !airborne() && // not from the air
                             !fight.lone_; // never while Lone
        if (pressed && canTake && arenaDistance(pl.pos, fight.allyPos_) <= fi.takeHalfReach + kArenaEps) {
            pl.takeHalfPress = -1.0e9f; // used
            tookHalf = true;
            const int lead = (damage + 1) / 2;
            allyPart = damage - lead;
            const int taken = std::min(scaledTaken(lead), pl.hp); // the Load below reads the blow's own half
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
        allyPart = tableShare(source, foe.move, kArenaAlly, allyPart); // after take half: what is left is shared
        const int lost = std::min(allyPart, fight.allyHp_);
        fight.allyHp_ -= lost;
        if (lost > 0) emit(ArenaEventType::AllyHit, source, kArenaAlly, foe.move, lost, fight.allyPos_);
        if (fight.allyHp_ <= 0) {
            downAlly(source, foe.move);
            return;
        }
        // A takeHalf chair heals the ally a share of its max hp each time the player takes half (never past it).
        if (tookHalf) {
            const int heal = static_cast<int>(std::lround(fight.chairs_.takeHalfHeal() * static_cast<float>(fight.allyMaxHp_)));
            fight.allyHp_ = std::min(fight.allyMaxHp_, fight.allyHp_ + std::max(heal, 0));
        }
    }

    // A set table (ArenaFight::setTableSeats): the even split of `damage` from source's blow on a seated lead
    // (`struck`: kArenaPlayer or kArenaAlly). Passes the other parts on and returns the struck one's.
    int tableShare(std::uint8_t source, std::uint8_t move, std::uint8_t struck, int damage) {
        const ArenaTableSeats& s = fight.tableSeats_;
        if (damage <= 0 || !s.any()) return damage;
        if (!(struck == kArenaPlayer ? s.player : (struck == kArenaAlly && s.ally))) return damage;
        const bool player = s.player && pl.hp > 0;
        const bool ally = s.ally && allyUp();
        int bodies = (player ? 1 : 0) + (ally ? 1 : 0);
        for (std::size_t i = 0; i < n; ++i)
            if (((s.enemies >> i) & 1u) && alive(i)) ++bodies;
        const TableEvenSplit split = tableEvenSplit(damage, bodies);
        if (split.each <= 0) return split.struck;
        if (player && struck != kArenaPlayer) {
            const int lost = std::min(split.each, pl.hp);
            pl.hp -= lost;
            stats.damageTaken += lost;
            emit(ArenaEventType::TableShare, source, kArenaPlayer, move, lost, pl.pos);
            if (pl.hp <= 0) downPlayer(source, move);
        }
        if (ally && struck != kArenaAlly) {
            const int lost = std::min(split.each, fight.allyHp_);
            fight.allyHp_ -= lost;
            emit(ArenaEventType::TableShare, source, kArenaAlly, move, lost, fight.allyPos_);
            if (fight.allyHp_ <= 0) downAlly(source, move);
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (!((s.enemies >> i) & 1u) || !alive(i) || unhurt(foes[i])) continue;
            const int part = tableEnemyPart(split.each, s.enemyShare[i]);
            if (part <= 0) continue;
            const int amount = lose(i, part);
            if (amount <= 0) continue;
            emit(ArenaEventType::TableShare, source, id(i), move, amount, foes[i].pos);
            if (foes[i].hp <= 0) killEnemy(i);
            else accrueBreak(i, kArenaBreakTable, 1.f);
        }
        return split.struck;
    }

    void downPlayer(std::uint8_t source, std::uint8_t move) {
        pl.hp = 0;
        pl.action = ArenaAction::Downed;
        pl.actionTime = 0.f;
        pl.velocity = Vec2{};
        pl.buffered = Press::None;
        pl.parryLeft = 0.f;
        emit(ArenaEventType::Downed, source, kArenaPlayer, move, 0, pl.pos);
    }

    void damagePlayer(std::uint8_t source, int damage, std::uint8_t move) {
        damage = tableShare(source, move, kArenaPlayer, damage);
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
        const ArenaAttack& a = fight.enemyAttackNow(i);
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
            arenaReaches(foe.pos, foe.facing, a, fight.allyPos_, fight.allyRadius_)) {
            enemyStrikeAlly(i);
            if (!alive(i)) return Vec2{}; // its own blow's table share killed it
        }
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

    // Out of the windup arc with the token: it circles round the lead toward its facing (Strafe, at the faster of
    // walkSpeed and strafeSpeed), pulled to the preferred range as a strafe is. No RNG.
    Vec2 reposition(std::size_t i, Vec2 dir) {
        Enemy& foe = foes[i];
        const ArenaEnemyKind& k = def.enemies[i];
        foe.stateTime += dt;
        if (!foe.turnHeld) foe.facing = arenaTurn(foe.facing, dir, k.turnRate * dt);
        const Vec2 outward = arenaUnit(arenaSub(foe.pos, pl.pos), Vec2{-dir.x, -dir.y});
        // Counter-clockwise of the lead's facing: round clockwise toward it, else counter-clockwise.
        const float side = pl.facing.x * outward.y - pl.facing.y * outward.x;
        foe.strafeDir = side > 0.f ? static_cast<std::int8_t>(-1) : static_cast<std::int8_t>(1);
        const float speed = std::max(k.walkSpeed, k.strafeSpeed);
        const float radius = std::max(k.preferredRange, kArenaEps);
        const float step = static_cast<float>(foe.strafeDir) * speed * dt / radius;
        const Vec2 target = arenaMadd(pl.pos, arenaRotate(outward, step), radius);
        const Vec2 want = arenaScaled(arenaSub(target, foe.pos), 1.f / dt);
        return arenaClampLength(want, speed);
    }

    // How long enemy i's Staggered, Open or Down lasts in all: the host's hold while one runs, else the kind's time;
    // a Flinch, flinchTime; a StepBack, kArenaStepBackTime. 0 in any other state.
    float recoverLength(std::size_t i) const {
        const Enemy& foe = foes[i];
        const ArenaEnemyKind& k = def.enemies[i];
        if (foe.brain == ArenaBrain::Flinch) return k.flinchTime;
        if (foe.brain == ArenaBrain::StepBack) return kArenaStepBackTime;
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
        // A boss string aimed at the partner (aim 2) tracks the ally; one with the token about to begin a string that
        // would aim at it picks its move by the distance to the ally and closes on it.
        const bool towardAlly =
            partnerOn() && (foe.brain == ArenaBrain::Attack
                                ? foe.aim == 2
                                : foe.token && (foe.brain == ArenaBrain::Approach || foe.brain == ArenaBrain::Strafe) &&
                                      partnerAim(i));
        const Vec2 toPlayer = arenaSub(towardAlly ? fight.allyPos_ : pl.pos, foe.pos);
        const float distance = arenaLength(toPlayer);
        const Vec2 dir = arenaUnit(toPlayer, foe.facing);

        Pick pick;
        bool circling = false; // repositioning into the windup arc
        if (foe.token && (foe.brain == ArenaBrain::Approach || foe.brain == ArenaBrain::Strafe)) {
            // A regular enemy begins its string only where the player can see it; after kArenaUnseenWait it begins
            // anyway, unseen (a longer windup and the flag). The wait runs only while it holds the token out of sight.
            const bool seen = inSight(i);
            if (seen) foe.tokenSince = t0;
            const bool late = !seen && t0 - foe.tokenSince >= kArenaUnseenWait - kArenaEps;
            if (!seen && !late) {
                circling = true;
                if (foe.brain != ArenaBrain::Strafe) {
                    foe.brain = ArenaBrain::Strafe;
                    foe.stateTime = 0.f;
                }
            } else if (pick = pickMove(i, distance); pick.move >= 0) {
                beginMove(i, static_cast<std::uint8_t>(pick.move), false, late);
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
        case ArenaBrain::Strafe:
            vel = arenaRamp(foe.velocity, circling ? reposition(i, dir) : strafe(i, dir), def.enemies[i].accel, dt);
            break;
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
        case ArenaBrain::StepBack: {
            // Back from the player (or to the side it picked) over the step, unhurt, still facing it.
            if (!foe.turnHeld) foe.facing = arenaTurn(foe.facing, dir, def.enemies[i].turnRate * dt);
            const float moving = std::max(0.f, std::min(foe.stateTime + dt, kArenaStepBackTime) - foe.stateTime);
            vel = arenaScaled(foe.stepWay, foe.stepDistance / kArenaStepBackTime * moving / dt);
            recover(i);
            break;
        }
        case ArenaBrain::Dead: break;
        }
        if (foe.shoveLeft > 0.f && foe.brain != ArenaBrain::Dead) { // a special's knock-back over its own time
            const float moving = std::min(foe.shoveLeft, dt);
            vel = arenaMadd(vel, foe.shoveVel, moving / dt);
            foe.shoveLeft -= moving;
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

    // ---- the special (ArenaSpecial.h) ----

    static constexpr float kSpecialEps = 1.0e-4f; // s: a cue, the impact or the end due within it is due now
    static constexpr float kSpecialEndStep = 0.05f;  // m: the steps the end's clear way is tested in (as the host's)
    static constexpr float kSpecialEndShort = 0.01f; // of the way: how far short of a body the end stops

    // The kit slot the player holds (setSpecials' slot 0 is the def's fighter as begun), and the partner's.
    std::size_t playerSlot() const { return fight.kitsSwapped_ ? 1u : 0u; }
    std::uint8_t bodyOfSlot(std::size_t slot) const { return slot == playerSlot() ? kArenaPlayer : kArenaAlly; }
    bool slotHasLead(std::size_t slot) const { return fight.special_.lead[slot][0] != '\0'; }
    const ArenaSpecialTable* specialTable() const { return fight.special_.on ? fight.specialTable_ : nullptr; }
    // A move by its index in the table, if it is still there and still the lead's (nullptr: gone).
    static const ArenaSpecialRules* specialMoveAt(const ArenaSpecialTable* t, std::uint8_t index, std::string_view lead) {
        if (!t || lead.empty() || index >= std::min(t->count, t->moves.size())) return nullptr;
        const ArenaSpecialRules& m = t->moves[index];
        return m.leadView() == lead ? &m : nullptr;
    }
    // The lead's move as an index into the table (kArenaSpecialNone: none).
    static std::uint8_t specialIndexOf(const ArenaSpecialTable* t, std::string_view lead) {
        const ArenaSpecialRules* m = t ? t->forLead(lead) : nullptr;
        return m ? static_cast<std::uint8_t>(m - t->moves.data()) : kArenaSpecialNone;
    }
    std::string_view slotLead(std::size_t slot) const { return std::string_view(fight.special_.lead[slot].data()); }
    const ArenaSpecialRules* slotMove(std::size_t slot) const {
        return specialMoveAt(specialTable(), fight.special_.move[slot], slotLead(slot));
    }
    // The running move (nullptr: none, or the host's table lost it).
    const ArenaSpecialRules* runningMove() const {
        const ArenaSpecialView& v = fight.special_.view;
        return v.slot < 2 ? specialMoveAt(specialTable(), v.move, slotLead(v.slot)) : nullptr;
    }
    bool slotReady(std::size_t slot) const {
        const ArenaSpecialTable* t = specialTable();
        return t && slotHasLead(slot) && fight.special_.gauge[slot] >= t->gauge.full;
    }
    // Whether a table may be set: the loader's own rules (validArenaSpecialRules, validArenaSpecialGauge).
    static bool specialTableValid(const ArenaSpecialTable* t) {
        if (!t) return true;
        if (t->count > t->moves.size() || !validArenaSpecialGauge(t->gauge)) return false;
        for (std::size_t i = 0; i < t->count; ++i)
            if (!validArenaSpecialRules(t->moves[i])) return false;
        return true;
    }

    // The enemy a special aims at, whichever body performs it (only the player fires it): the player's lock at any
    // range (the cinematic covers the distance); with no lock its attack's soft target, else the nearest living enemy in
    // front of the player, within the move's range of the performer (body centres). kArenaNobody for none.
    std::uint8_t specialTarget(std::uint8_t lead, const ArenaSpecialRules& m) const {
        const Vec2 from = lead == kArenaPlayer ? pl.pos : fight.allyPos_;
        const auto withinRange = [&](std::size_t i) { return arenaDistance(from, foes[i].pos) <= m.range; };
        if (pl.lock < n && alive(pl.lock)) return id(pl.lock);
        if (pl.soft < n && alive(pl.soft)) return withinRange(pl.soft) ? id(pl.soft) : kArenaNobody;
        std::uint8_t best = kArenaNobody;
        float bestDist = 0.f;
        for (std::size_t i = 0; i < n; ++i) {
            if (!alive(i) || !withinRange(i)) continue;
            const Vec2 to = arenaSub(foes[i].pos, pl.pos);
            const float d = arenaLength(to);
            if (arenaDot(to, pl.facing) < 0.f) continue;
            if (best == kArenaNobody || d < bestDist) {
                best = id(i);
                bestDist = d;
            }
        }
        return best;
    }
    // Where a special by `lead` at `target` starts and which way it strikes.
    Vec2 specialOrigin(std::uint8_t lead) const { return lead == kArenaPlayer ? pl.pos : fight.allyPos_; }
    Vec2 specialDir(std::uint8_t lead, std::size_t target) const {
        return arenaUnit(arenaSub(foes[target].pos, specialOrigin(lead)), lead == kArenaPlayer ? pl.facing : fight.ally_.facing);
    }
    // Whether enemy i's body reaches into the move's lane from `origin` along `dir`.
    bool inLane(std::size_t i, Vec2 origin, Vec2 dir, const ArenaSpecialRules& m) const {
        const Vec2 side{-dir.y, dir.x};
        const float r = def.enemies[i].bodyRadius;
        const Vec2 to = arenaSub(foes[i].pos, origin);
        const float along = arenaDot(to, dir);
        const float across = std::fabs(arenaDot(to, side));
        return along >= -r && along <= m.laneLength + r && across <= 0.5f * m.laneWidth + r;
    }

    ArenaSpecialRefusal specialRefusal(std::uint8_t lead, bool sight, std::uint8_t* target = nullptr) const {
        const ArenaFight::SpecialState& sp = fight.special_;
        if (fight.phase_ != ArenaPhase::Fighting) return ArenaSpecialRefusal::Over;
        if (sp.view.running) return ArenaSpecialRefusal::Running;
        if (lead != kArenaPlayer && lead != kArenaAlly) return ArenaSpecialRefusal::NoMove;
        if (lead == kArenaAlly && fight.allyMaxHp_ <= 0) return ArenaSpecialRefusal::NoMove;
        const std::size_t slot = lead == kArenaPlayer ? playerSlot() : 1u - playerSlot();
        const ArenaSpecialRules* m = slotMove(slot);
        if (!m) return ArenaSpecialRefusal::NoMove;
        if (lead == kArenaAlly && pk.playable) return ArenaSpecialRefusal::NotControlled;
        if (!slotReady(slot)) return ArenaSpecialRefusal::NotReady;
        if (lead == kArenaPlayer) {
            const ArenaAction a = pl.action;
            if (pl.hp <= 0 || a == ArenaAction::Hitstun || a == ArenaAction::GuardBreak || a == ArenaAction::Downed ||
                a == ArenaAction::Jump)
                return ArenaSpecialRefusal::Down;
        } else if (!allyUp() || fight.ally_.hurtLeft > 0.f) {
            return ArenaSpecialRefusal::Down;
        }
        const std::uint8_t t = specialTarget(lead, *m);
        if (t == kArenaNobody) return ArenaSpecialRefusal::NoTarget;
        if (!sight) return ArenaSpecialRefusal::NoSight;
        if (unhurt(foes[t])) { // held unhurt: the press stands only if the lane holds an enemy it can hurt
            const Vec2 origin = specialOrigin(lead), dir = specialDir(lead, t);
            bool hurtable = false;
            for (std::size_t i = 0; i < n && !hurtable; ++i)
                hurtable = i != t && alive(i) && !unhurt(foes[i]) && inLane(i, origin, dir, *m);
            if (!hurtable) return ArenaSpecialRefusal::Unhurt;
        }
        if (target) *target = t;
        return ArenaSpecialRefusal::Ok;
    }

    // The ready flags and refusals the view shows.
    // A slot's full gauge shows (a prompt, a Ready): the controlled lead's, or a partner's whose kit is not playable (a
    // party member the player calls on); a partner lead's fills hidden.
    bool slotShown(std::size_t slot) const {
        if (!slotReady(slot)) return false;
        return bodyOfSlot(slot) == kArenaPlayer || (fight.allyMaxHp_ > 0 && !pk.playable);
    }
    // No prompt stands (setSpecials, begin, restart): a gauge that is full says Ready on the first tick of the fight, its
    // line per the gap on the host's clock.
    void specialArm() { fight.special_.shown = {false, false}; }
    // A lead switch (`outgoing` was the player's slot before it): switchLoss off the gauges the data names.
    void specialSwitchLoss(std::size_t outgoing) {
        ArenaFight::SpecialState& sp = fight.special_;
        const ArenaSpecialTable* t = specialTable();
        if (!t) return;
        const ArenaSpecialGaugeRules& g = t->gauge;
        for (std::size_t s = 0; s < 2; ++s) {
            const bool named = g.switchLossAppliesTo == ArenaSpecialSwitchLoss::Both ||
                               (g.switchLossAppliesTo == ArenaSpecialSwitchLoss::Outgoing) == (s == outgoing);
            if (named && slotHasLead(s)) sp.gauge[s] = std::max(0.f, sp.gauge[s] * (1.f - g.switchLoss));
        }
        specialFlags(true);
    }

    void specialFlags(bool sight) {
        ArenaSpecialView& v = fight.special_.view;
        const std::size_t ps = playerSlot();
        v.readyPlayer = slotShown(ps);
        v.readyPartner = slotShown(1u - ps);
        v.refusalPlayer = specialRefusal(kArenaPlayer, sight);
        v.refusalPartner = specialRefusal(kArenaAlly, sight);
    }

    // The gauges take this tick's events not yet taken (each event once: the press takes those before it).
    std::size_t specialFilled = 0;
    void specialFill() {
        ArenaFight::SpecialState& sp = fight.special_;
        const ArenaSpecialTable* t = specialTable();
        const std::size_t count = std::min<std::size_t>(out.events.count, kMaxArenaEvents);
        const std::size_t from = std::min(specialFilled, count);
        specialFilled = count;
        if (!t) return;
        const ArenaSpecialGaugeRules& g = t->gauge;
        const std::size_t ps = playerSlot(), as = 1u - ps;
        std::array<float, 2> add{};
        for (std::size_t e = from; e < count; ++e) {
            const ArenaEvent& ev = out.events.items[e];
            const bool onFoe = ev.target < n;
            const bool byLead = ev.source == kArenaPlayer || ev.source == kArenaAlly;
            const float amount = static_cast<float>(std::max(ev.amount, 0));
            switch (ev.type) {
            case ArenaEventType::Hit:
            case ArenaEventType::HostHit:
            case ArenaEventType::CounterStagger:
            case ArenaEventType::ReadCounter:
                if (onFoe && byLead && ev.move != kArenaSpecialMove)
                    add[ev.source == kArenaPlayer ? ps : as] += amount * g.perDamageDealt;
                else if (ev.target == kArenaPlayer)
                    add[ps] += amount * g.perDamageTaken;
                break;
            case ArenaEventType::AllyHit: add[as] += amount * g.perDamageTaken; break;
            case ArenaEventType::Parried: add[ps] += g.perParry; break;
            case ArenaEventType::Blocked: add[ps] += g.perBlock; break;
            case ArenaEventType::AllyBlocked: add[as] += g.perBlock; break;
            default: break;
            }
        }
        for (std::size_t s = 0; s < 2; ++s) {
            if (!slotHasLead(s) || !(add[s] > 0.f) || sp.gauge[s] >= g.full) continue;
            sp.gauge[s] = std::min(g.full, sp.gauge[s] + add[s]);
        }
    }

    // After a tick that ran (or refreshed): the gauges take this tick's events, then the prompts, then the flags.
    void specialAfterTick(bool sight) {
        ArenaFight::SpecialState& sp = fight.special_;
        specialFill();
        const ArenaSpecialTable* t = specialTable();
        // The prompts, while the fight runs (none on the tick it ends): a full gauge that shows says Ready once (its
        // line at most once per readyLineGap of the host's clock); one that stops showing (a switch lowered it, or its
        // lead is the partner now) says Unready.
        for (std::size_t s = 0; t && fight.phase_ == ArenaPhase::Fighting && s < 2; ++s) {
            const bool show = slotShown(s);
            if (show == sp.shown[s]) continue;
            sp.shown[s] = show;
            const std::uint8_t body = bodyOfSlot(s);
            const Vec2 at = body == kArenaPlayer ? pl.pos : fight.allyPos_;
            if (!show) {
                emit(ArenaEventType::SpecialUnready, body, kArenaNobody, 0, static_cast<int>(s), at);
                continue;
            }
            const bool line = sp.lineAt[s] <= kArenaSpecialNoLine || sp.clock - sp.lineAt[s] >= t->gauge.readyLineGap;
            if (line) sp.lineAt[s] = sp.clock;
            emit(ArenaEventType::SpecialReady, body, kArenaNobody, line ? 1 : 0, static_cast<int>(s), at);
        }
        specialFlags(sight);
    }

    // The press: the gauge spent, the performer's spot and way taken, the fight frozen from now.
    bool startSpecial(const ArenaInput& in) {
        std::uint8_t target = kArenaNobody;
        if (specialRefusal(in.special, in.specialSight, &target) != ArenaSpecialRefusal::Ok) return false;
        ArenaFight::SpecialState& sp = fight.special_;
        const bool player = in.special == kArenaPlayer;
        const std::size_t slot = player ? playerSlot() : 1u - playerSlot();
        const ArenaSpecialRules& m = *slotMove(slot); // the refusal found it
        ArenaSpecialView& v = sp.view;
        v.running = true;
        v.move = sp.move[slot];
        v.performer = in.special;
        v.slot = static_cast<std::uint8_t>(slot);
        v.target = target;
        v.dir = specialDir(in.special, target);
        {   // where it starts (the host's back off) and ends (the clip's travel, as far as the host has room)
            const float radius = player ? def.fighter.bodyRadius : def.partnerKit.bodyRadius;
            const float back = in.specialBack > 0.f ? std::min(in.specialBack, kArenaSpecialMaxBack) : 0.f; // NaN: 0
            v.origin = fight.ring_.clamp(arenaMadd(specialOrigin(in.special), v.dir, -back), radius);
            if (player) pl.pos = v.origin;
            else fight.allyPos_ = v.origin;
            v.travel = m.endTravel > 0.f && in.specialTravel > 0.f ? std::min(in.specialTravel, m.endTravel) : 0.f;
            const float side = m.endTravel > 0.f ? m.endSide * (v.travel / m.endTravel) : 0.f;
            const Vec2 right{-v.dir.y, v.dir.x};
            v.end = fight.ring_.clamp(arenaMadd(arenaMadd(v.origin, v.dir, v.travel), right, side), radius);
            // Clear of every living enemy's body, by the host's own rule (ClampSpecialTravel): 5 cm steps along the
            // way, stopped a hair short of the first step that would overlap one. What the host sends is clear
            // already; this bites when Core picked another target than the host's guess, or the host sent too much.
            const Vec2 path = arenaSub(v.end, v.origin);
            const float length = std::sqrt(arenaDot(path, path));
            if (length > 0.01f) {
                const int steps = std::max(1, static_cast<int>(std::ceil(length / kSpecialEndStep)));
                float share = 1.f;
                for (int k = 1; k <= steps && share >= 1.f; ++k) {
                    const Vec2 p = arenaMadd(v.origin, path, static_cast<float>(k) / static_cast<float>(steps));
                    for (std::size_t i = 0; i < n; ++i) {
                        if (!alive(i)) continue;
                        const Vec2 d = arenaSub(p, foes[i].pos);
                        const float need = radius + def.enemies[i].bodyRadius;
                        if (arenaDot(d, d) < need * need) {
                            share = std::max(0.f, static_cast<float>(k - 1) / static_cast<float>(steps) - kSpecialEndShort);
                            break;
                        }
                    }
                }
                if (share < 1.f) {
                    v.end = arenaMadd(v.origin, path, share);
                    v.travel *= share;
                }
            }
        }
        v.time = 0.f;
        v.duration = m.duration;
        {   // the wave's travel from the performer's spot at the strike to the target, fixed now: nothing moves while frozen
            const float share = m.duration > 0.f ? std::clamp(m.strikeAt / m.duration, 0.f, 1.f) : 1.f;
            const Vec2 at = arenaMadd(v.origin, arenaSub(v.end, v.origin), share);
            const Vec2 to = target < n ? arenaSub(foes[target].pos, at) : Vec2{};
            v.impactTime = std::min(arenaSpecialImpactAt(m, std::sqrt(arenaDot(to, to))), m.duration);
        }
        v.impactDone = false;
        v.skippable = m.skippable && sp.seen[slot];
        ++v.uses;
        v.cueCount = 0;
        v.resultCount = 0;
        v.endedThisTick = false;
        sp.nextCue = 0;
        sp.gauge[slot] = 0.f;
        sp.shown[slot] = false; // spent: the prompt goes with SpecialStarted
        ++stats.specials;
        emit(ArenaEventType::SpecialStarted, v.performer, target, kArenaSpecialMove, static_cast<int>(slot), v.origin);
        specialCues(0.f, m);
        specialFlags(in.specialSight);
        return true;
    }

    // The cues due by `now` (s since the press), in order, into this tick's list.
    void specialCues(float now, const ArenaSpecialRules& m) {
        ArenaFight::SpecialState& sp = fight.special_;
        ArenaSpecialView& v = sp.view;
        while (sp.nextCue < m.cueCount && m.cues[sp.nextCue].at <= now + kSpecialEps &&
               v.cueCount < kMaxArenaSpecialCues)
            v.cuesFired[v.cueCount++] = sp.nextCue++;
    }

    // The hp a boss loses: `share` of its current break phase's hp span; never all of it from full hp.
    int specialBossDamage(std::size_t i, float share) const {
        const ArenaEnemyKind& k = def.enemies[i];
        const int hp = foes[i].hp;
        const int p = arenaBreakPhaseOf(k, hp);
        const int last = std::clamp(static_cast<int>(k.breakPhases), 1, static_cast<int>(kMaxArenaBreakPhases)) - 1;
        const float hi = p == 0 ? 1.f : k.breakPhaseEnds[static_cast<std::size_t>(p - 1)];
        const float lo = p >= last ? 0.f : k.breakPhaseEnds[static_cast<std::size_t>(p)];
        const float span = std::max(0.f, hi - lo) * static_cast<float>(std::max(k.hp, 1));
        int amount = std::max(1, static_cast<int>(std::lround(span * share)));
        if (hp >= k.hp) amount = std::min(amount, std::max(hp - 1, 0));
        return amount;
    }

    // At impact: the target always (one gone by now passes it to the nearest living enemy in the lane, else nobody),
    // every other living enemy whose body reaches into the lane at otherShare. An enemy held unhurt is in the results
    // as the target only, at 0.
    void specialImpact(const ArenaSpecialRules& m) {
        ArenaSpecialView& v = fight.special_.view;
        v.resultCount = 0;
        if (v.target >= n || !alive(v.target)) { // nothing in the freeze can kill it: kept for safety
            std::uint8_t next = kArenaNobody;
            float bestAlong = 0.f;
            for (std::size_t i = 0; i < n; ++i) {
                if (!alive(i) || !inLane(i, v.origin, v.dir, m)) continue;
                const float along = arenaDot(arenaSub(foes[i].pos, v.origin), v.dir);
                if (next == kArenaNobody || along < bestAlong) {
                    next = id(i);
                    bestAlong = along;
                }
            }
            v.target = next;
        }
        for (std::size_t i = 0; i < n && v.resultCount < kMaxArenaSpecialTargets; ++i) {
            if (!alive(i)) continue;
            const bool isTarget = i == v.target;
            if (!isTarget && (unhurt(foes[i]) || !inLane(i, v.origin, v.dir, m))) continue;
            const bool boss = def.enemies[i].boss;
            const int damage =
                boss ? specialBossDamage(i, m.bossPhaseShare * (isTarget ? 1.f : m.otherShare))
                     : (isTarget ? m.damage
                                 : static_cast<int>(std::lround(static_cast<float>(m.damage) * m.otherShare)));
            if (damage <= 0 && !isTarget) continue;
            ArenaSpecialResult& res = v.results[v.resultCount++];
            res.foe = id(i);
            res.damage = unhurt(foes[i]) ? 0 : std::max(damage, 1);
            res.target = isTarget;
            res.boss = boss;
        }
        v.impactDone = true;
    }

    // The cut back: the results land (hp, then a stagger and the knock-back for all but a boss; nothing on an enemy
    // held unhurt), the performer stands Idle facing the way it struck, and the fight runs again from the next tick. A
    // move the host's table lost ends here with no results (the gauge stays spent).
    void specialEnd(bool skipped, bool sight) {
        ArenaFight::SpecialState& sp = fight.special_;
        ArenaSpecialView& v = sp.view;
        const ArenaSpecialRules* m = runningMove();
        if (!m) {
            v.resultCount = 0;
            v.impactDone = true;
        } else if (!v.impactDone) {
            specialImpact(*m);
        }
        const std::uint8_t by = v.performer;
        if (by == kArenaPlayer) pl.pos = v.end; // the performance travelled: the body ends where the clip does
        else fight.allyPos_ = v.end;
        for (std::size_t r = 0; m && r < v.resultCount; ++r) {
            const ArenaSpecialResult& res = v.results[r];
            const std::size_t i = res.foe;
            if (!alive(i) || res.damage <= 0 || unhurt(foes[i])) continue;
            Enemy& foe = foes[i];
            const int amount = lose(i, res.damage);
            stats.damageDealt += amount;
            stats.specialDamage += amount;
            notePlayerBlow(i, fight.time_);
            emit(ArenaEventType::Hit, by, id(i), kArenaSpecialMove, amount, foe.pos);
            if (foe.hp <= 0) {
                killEnemy(i, true, fight.time_, by);
            } else if (!res.boss) {
                staggerEnemy(i, by);
                if (m->knockBack > 0.f && m->knockTime > 0.f) {
                    foe.shoveVel = arenaScaled(arenaUnit(arenaSub(foe.pos, v.end), v.dir), m->knockBack / m->knockTime);
                    foe.shoveLeft = m->knockTime;
                }
            }
        }
        if (by == kArenaPlayer) {
            pl.action = ArenaAction::Idle;
            pl.actionTime = 0.f;
            pl.velocity = Vec2{};
            pl.attackWalk = Vec2{};
            pl.buffered = Press::None;
            pl.bufferAge = 0.f;
            pl.chainLive = false;
            pl.hitMask = 0;
        }
        // It faces the target from where it ended (it may have travelled past it), else the strike's way.
        const Vec2 face = v.target < n && alive(v.target) ? arenaUnit(arenaSub(foes[v.target].pos, v.end), v.dir) : v.dir;
        if (by == kArenaPlayer) {
            pl.invulnerable = false;
            pl.facing = face;
            pl.drawFacing = face;
        } else {
            clearAlly();
            fight.ally_.facing = face;
            fight.ally_.heading = face;
        }
        if (m) sp.seen[v.slot] = true;
        v.running = false;
        v.endedThisTick = true;
        v.skippable = false;
        emit(ArenaEventType::SpecialEnded, by, v.target, kArenaSpecialMove, skipped ? 1 : 0,
             by == kArenaPlayer ? pl.pos : fight.allyPos_);
        settleOutcome();
        fillOutput();
        specialFlags(sight);
    }

    // A tick while frozen: nothing but the special's clock runs (the fight's time, every body, brain, timer and
    // cooldown stand still); its cues fire, its results are ruled at impact and land at the end.
    void specialTick(float step, const ArenaInput& in) {
        out.events.count = 0;
        ArenaFight::SpecialState& sp = fight.special_;
        ArenaSpecialView& v = sp.view;
        v.cueCount = 0;
        v.endedThisTick = false;
        const ArenaSpecialRules* m = runningMove();
        const bool skip = in.specialSkip && v.skippable;
        if (!(step > 0.f) && !skip && m) return;
        dt = step > 0.f ? step : 0.f;
        t0 = t1 = fight.time_;
        if (!m) { // the host's table lost the move: it ends now
            specialEnd(false, in.specialSight);
            return;
        }
        const float now = skip ? std::max(v.time + dt, v.duration) : v.time + dt;
        specialCues(now, *m);
        if (!v.impactDone && now >= v.impactTime - kSpecialEps) specialImpact(*m);
        v.time = std::min(now, v.duration);
        if (now >= v.duration - kSpecialEps) specialEnd(skip, in.specialSight);
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

    // hostHeal: -1 the fight's own charges; 0 or 1 the host's heal, not ready or ready.
    static ArenaInput autopilot(const ArenaFight& self, int hostHeal) {
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
            const float startup = self.enemyAttackOf(self.readFoe_, self.readMove_).startup;
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
            const ArenaAttack& a = self.enemyAttackNow(i);
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
        const bool locked = p.gauge.value >= self.capNow(); // Overloaded: no heal, no attack but the Release
        const bool canHeal = hostHeal < 0 ? p.charges > 0 : hostHeal > 0;
        if (!locked && canHeal && p.hp * 100 < kArenaAutoHealPercent * f.hp && !windingUp) {
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
            if (d - arenaDrawnRadius(self.def_.enemies[static_cast<std::size_t>(opened)]) <= f.riposteReach && front) {
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
        if (p.gauge.value >= std::min(kArenaAutoReleaseAt, self.capNow()) && !down(target) &&
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
            (f.rounds == 0 || p.rounds > 0) &&
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
    if (kitsSwapped_) allyMaxHp_ = allyHpGiven_; // the last fight ended swapped: the host's ally max again
    kitsSwapped_ = false; // d's kits as given
    switchRefusal_ = ArenaSwitchRefusal::Ok;
    for (ArenaFighter* k : {&def_.fighter, &def_.partnerKit})
        k->lightCount = static_cast<std::uint8_t>(std::clamp<int>(k->lightCount, 1, static_cast<int>(kMaxLightChain)));
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
    special_ = SpecialState{}; // the host gives them again (setSpecials): its store carries the gauges
    specialTable_ = nullptr;
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

ArenaInput ArenaFight::autopilot() const { return ArenaFightImpl::autopilot(*this, -1); }
ArenaInput ArenaFight::autopilot(bool hostHealReady) const { return ArenaFightImpl::autopilot(*this, hostHealReady ? 1 : 0); }
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

bool ArenaFight::setSpecials(const ArenaSpecialTable* table, std::string_view fighterLead, std::string_view partnerLead,
                             const LeadSpecialGauges& gauges) {
    if (inHook_.on || special_.view.running || !ArenaFightImpl::specialTableValid(table)) return false;
    special_.on = table != nullptr;
    specialTable_ = table;
    special_.clock = gauges.clock >= 0.f && gauges.clock < 1.0e9f ? gauges.clock : 0.f; // NaN, below 0: 0
    const std::array<std::string_view, 2> leads{fighterLead, partnerLead};
    for (std::size_t s = 0; s < 2; ++s) {
        std::array<char, kMaxArenaSpecialId + 1>& id = special_.lead[s];
        id.fill('\0');
        const std::string_view lead = leads[s].size() <= kMaxArenaSpecialId ? leads[s] : std::string_view{};
        std::copy(lead.begin(), lead.end(), id.begin());
        special_.move[s] = ArenaFightImpl::specialIndexOf(table, lead);
        const LeadSpecialGauges::Lead* kept = lead.empty() ? nullptr : gauges.find(lead);
        const float full = table ? table->gauge.full : 0.f;
        const float value = kept && kept->value > 0.f ? std::min(kept->value, full) : 0.f; // NaN: 0
        special_.gauge[s] = special_.gaugeAtBegin[s] = value;
        special_.seen[s] = kept && kept->seen;
        special_.lineAt[s] = kept && kept->lineAt <= special_.clock ? kept->lineAt : kArenaSpecialNoLine; // NaN: none
    }
    ArenaFightImpl(*this).specialArm();
    ArenaFightImpl(*this).specialFlags(true);
    return true;
}

void ArenaFight::storeSpecialGauges(LeadSpecialGauges& gauges) const {
    for (std::size_t s = 0; s < 2; ++s) {
        const std::string_view lead(special_.lead[s].data());
        if (lead.empty()) continue;
        if (LeadSpecialGauges::Lead* e = gauges.entry(lead)) {
            e->value = special_.gauge[s];
            e->seen = special_.seen[s];
            e->lineAt = special_.lineAt[s];
        }
    }
    gauges.clock = std::max(gauges.clock, special_.clock); // the host's clock never runs back
}

bool ArenaFight::setSpecialTable(const ArenaSpecialTable* table) {
    if (inHook_.on || !ArenaFightImpl::specialTableValid(table)) return false;
    special_.on = table != nullptr;
    specialTable_ = table;
    for (std::size_t s = 0; s < 2; ++s)
        special_.move[s] = ArenaFightImpl::specialIndexOf(table, std::string_view(special_.lead[s].data()));
    ArenaSpecialView& v = special_.view;
    if (v.running && v.slot < 2) v.move = special_.move[v.slot]; // none: it ends at the next tick
    ArenaFightImpl(*this).specialFlags(true);
    return true;
}

const ArenaSpecialRules* ArenaFight::specialMove() const {
    const ArenaSpecialView& v = special_.view;
    return v.slot < 2 ? ArenaFightImpl::specialMoveAt(special_.on ? specialTable_ : nullptr, v.move,
                                                      std::string_view(special_.lead[v.slot].data()))
                      : nullptr;
}

const ArenaSpecialView& ArenaFight::special() const { return special_.view; }
bool ArenaFight::specialsOn() const { return special_.on; }
bool ArenaFight::frozen() const { return special_.view.running; }

ArenaSpecialRefusal ArenaFight::specialRefusal(std::uint8_t lead) const {
    if (lead == kArenaPlayer) return special_.view.refusalPlayer;
    if (lead == kArenaAlly) return special_.view.refusalPartner;
    return ArenaSpecialRefusal::NoMove;
}

float ArenaFight::specialGaugeForTest(std::size_t slot) const { return slot < 2 ? special_.gauge[slot] : 0.f; }

void ArenaFight::setSpecialGaugeForTest(std::size_t slot, float value) {
    if (slot >= 2 || inHook_.on) return;
    special_.gauge[slot] = value;
    ArenaFightImpl(*this).specialFlags(true);
}

void ArenaFight::killEnemyForTest(std::size_t i) {
    if (i >= enemyCount_ || inHook_.on) return;
    enemy_[i].hp = 0;
    enemy_[i].brain = ArenaBrain::Dead;
}

std::size_t ArenaFight::frozenBytesForTest() const {
    return static_cast<std::size_t>(reinterpret_cast<const char*>(&special_) - reinterpret_cast<const char*>(this));
}

Vec2 ArenaFight::playerStart() const { return playerStart_; }
Vec2 ArenaFight::playerStartFacing() const { return playerStartFacing_; }
std::size_t ArenaFight::enemyCount() const { return enemyCount_; }
Vec2 ArenaFight::enemyStart(std::size_t i) const { return i < kMaxArenaEnemies ? enemyStart_[i] : Vec2{}; }
Vec2 ArenaFight::enemyStartFacing(std::size_t i) const { return i < kMaxArenaEnemies ? enemyStartFacing_[i] : Vec2{1.f, 0.f}; }

int ArenaFight::hp() const { return player_.hp; }
int ArenaFight::load() const { return player_.gauge.value; }
bool ArenaFight::overloaded() const { return player_.gauge.value >= capNow(); }
float ArenaFight::stamina() const { return player_.stamina; }
float ArenaFight::poise() const { return player_.poise; }
std::uint8_t ArenaFight::healCharges() const { return player_.charges; }
std::uint8_t ArenaFight::lock() const { return player_.lock; }
int ArenaFight::enemyHp(std::size_t i) const { return i < enemyCount_ ? enemy_[i].hp : 0; }
float ArenaFight::enemyPoise(std::size_t i) const { return i < enemyCount_ ? enemy_[i].poise : 0.f; }
bool ArenaFight::enemyAlive(std::size_t i) const { return i < enemyCount_ && enemy_[i].brain != ArenaBrain::Dead; }
std::uint8_t ArenaFight::shotMeets(Vec2 dir, float pitch, float* distance, std::uint32_t skip,
                                   std::uint8_t prefer) const {
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
                               def_.enemies.data(), at.data(), standing.data(), enemyCount_, skip, prefer, d);
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
           foe.stateTime < enemyAttackOf(readFoe_, readMove_).startup;
}
int ArenaFight::readStacks() const { return player_.readStacks; }
bool ArenaFight::pairOpen() const {
    // time_ is the next tick's start, the time tryPair would stamp a press made now.
    const std::uint8_t i = player_.pairFoe;
    return i < enemyCount_ && enemy_[i].brain == ArenaBrain::Open &&
           time_ - player_.pairParry <= def_.fighter.pairWindow + kArenaEps &&
           time_ >= player_.pairCoolUntil - kArenaEps; // no counter cooling (tryPair's pairCooling)
}

PressReason ArenaFight::pressReason(ArenaPress p) const {
    // Reading only: the rules bind the state by reference, and pressReason calls nothing but their const members.
    ArenaFightImpl impl(const_cast<ArenaFight&>(*this));
    impl.t0 = time_; // the next tick's start, the time a press made now is taken at
    return impl.pressReason(p);
}

bool ArenaFight::holdEnemy(std::size_t i, ArenaBrain brain, float seconds) {
    if (phase_ != ArenaPhase::Fighting || special_.view.running || !enemyAlive(i) ||
        (brain != ArenaBrain::Open && brain != ArenaBrain::Down && brain != ArenaBrain::Staggered) || !(seconds > 0.f))
        return false;
    EnemyState& foe = enemy_[i];
    foe.holdAsk = HoldAsk::Hold;
    foe.holdBrain = brain;
    foe.holdSeconds = seconds;
    return true;
}
void ArenaFight::releaseEnemy(std::size_t i) {
    if (i < enemyCount_ && !special_.view.running) enemy_[i].holdAsk = HoldAsk::Release;
}
bool ArenaFight::enemyHeld(std::size_t i) const { return enemyAlive(i) && enemy_[i].holdFor >= 0.f; }
void ArenaFight::setEnemyHpFloor(std::size_t i, int hpFloor) {
    if (i < enemyCount_ && !special_.view.running) enemy_[i].hpFloor = std::max(0, hpFloor);
}
int ArenaFight::enemyHpFloor(std::size_t i) const { return i < enemyCount_ ? enemy_[i].hpFloor : 0; }
static_assert(kMaxArenaMoves <= 32, "a move mask holds one bit per move");
void ArenaFight::setEnemyMoveMask(std::size_t i, std::uint32_t mask) {
    if (i < enemyCount_ && !special_.view.running) enemy_[i].moveMask = mask;
}
std::uint32_t ArenaFight::enemyMoveMask(std::size_t i) const { return i < enemyCount_ ? enemy_[i].moveMask : 0u; }
void ArenaFight::setEnemyFacingHeld(std::size_t i, bool held) {
    if (i < enemyCount_ && !special_.view.running) enemy_[i].facingHeld = held;
}
bool ArenaFight::enemyFacingHeld(std::size_t i) const { return i < enemyCount_ && enemy_[i].facingHeld; }
void ArenaFight::setEnemyTurnHeld(std::size_t i, bool held) {
    if (i < enemyCount_ && !special_.view.running) enemy_[i].turnHeld = held;
}
bool ArenaFight::enemyTurnHeld(std::size_t i) const { return i < enemyCount_ && enemy_[i].turnHeld; }
void ArenaFight::setEnemyFlinch(std::size_t i, bool allowed) {
    if (i < enemyCount_ && !special_.view.running) enemy_[i].flinchAllowed = allowed;
}
bool ArenaFight::enemyFlinch(std::size_t i) const { return i < enemyCount_ && enemy_[i].flinchAllowed; }
float ArenaFight::enemyBreakValue(std::size_t i) const {
    return i < enemyCount_ && def_.enemies[i].boss ? enemy_[i].breakValue : 0.f;
}
int ArenaFight::enemyBreakPhase(std::size_t i) const {
    return i < enemyCount_ && def_.enemies[i].boss ? arenaBreakPhaseOf(def_.enemies[i], enemy_[i].hp) : 0;
}
float ArenaFight::enemyBreakAt(std::size_t i) const {
    return i < enemyCount_ && def_.enemies[i].boss ? arenaBreakAtOf(def_.enemies[i], enemy_[i].hp) : 0.f;
}
bool ArenaFight::enemyBreakTell(std::size_t i) const {
    return i < enemyCount_ && def_.enemies[i].boss && enemy_[i].breakTell;
}
bool ArenaFight::enemyBreakGuard(std::size_t i) const {
    return i < enemyCount_ && def_.enemies[i].boss && ArenaFightImpl::breakGuarded(enemy_[i]);
}
bool ArenaFight::enemyBreakMoveOn(std::size_t i) const {
    return i < enemyCount_ && def_.enemies[i].boss && ArenaFightImpl::inBreakMove(enemy_[i]);
}
const ArenaAttack& ArenaFight::enemyAttackOf(std::size_t i, std::uint8_t move) const {
    const EnemyState& foe = enemy_[i];
    if (foe.move != move) return def_.enemies[i].moves[move].attack;
    return foe.breakTimed ? foe.breakAttack : foe.unseen ? foe.unseenAttack : def_.enemies[i].moves[move].attack;
}
void ArenaFight::setEnemyFacing(std::size_t i, Vec2 facing) {
    if (i < enemyCount_ && !special_.view.running && arenaLength(facing) > kArenaEps) enemy_[i].facing = arenaUnit(facing, enemy_[i].facing);
}
void ArenaFight::setBlowHook(ArenaBlowHook hook, void* user) {
    blowHook_ = hook;
    blowUser_ = hook ? user : nullptr;
}
ArenaBlowHook ArenaFight::blowHook() const { return blowHook_; }
void ArenaFight::setSideDashes(bool on) { sideDashes_ = on; }
bool ArenaFight::sideDashes() const { return sideDashes_; }
void ArenaFight::setCameraHalfAngle(float degrees) {
    cameraHalfAngle_ = std::isnan(degrees) ? kArenaCameraHalfAngle
                                           : std::clamp(degrees, kArenaCameraHalfAngleMin, kArenaCameraHalfAngleMax);
}
float ArenaFight::cameraHalfAngle() const { return cameraHalfAngle_; }
void* ArenaFight::blowHookUser() const { return blowUser_; }
bool ArenaFight::hostBlow(std::size_t enemy, int damage, std::uint8_t move) {
    if (phase_ != ArenaPhase::Fighting || special_.view.running) return false;
    return ArenaFightImpl(*this).hostBlow(enemy, damage, move);
}
bool ArenaFight::ruledHostBlow(std::size_t enemy, int damage, std::uint8_t move, ArenaBlowHook hook, void* user) {
    if (phase_ != ArenaPhase::Fighting || special_.view.running) return false;
    ArenaFightImpl impl(*this);
    if (!hook || !impl.alive(enemy) || damage <= 0 || ArenaFightImpl::unhurt(enemy_[enemy]))
        return impl.hostBlow(enemy, damage, move);
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
    return scaled > 0 && impl.hostBlow(enemy, scaled, move, r.breakScale);
}
int ArenaFight::giveLoad(int amount) {
    if (phase_ != ArenaPhase::Fighting || special_.view.running) return 0;
    return ArenaFightImpl(*this).giveLoad(amount);
}

void ArenaFight::setLoadCap(int cap) {
    if (!special_.view.running) ArenaFightImpl(*this).setLoadCap(cap);
}
int ArenaFight::loadCap() const { return capNow(); }
int ArenaFight::releaseOpenLoad() const { return capNow() / 3; }
void ArenaFight::setLone(bool on, const LoneTuning& tuning, float blowCap) {
    if (!special_.view.running) ArenaFightImpl(*this).setLone(on, tuning, blowCap);
}
bool ArenaFight::lone() const { return lone_; }
int ArenaFight::spendLoad(int amount) {
    if (phase_ != ArenaPhase::Fighting || special_.view.running || amount <= 0) return 0;
    return ArenaFightImpl(*this).spendLoad(amount);
}
void ArenaFight::setLoad(int value) {
    if (phase_ == ArenaPhase::Fighting && !special_.view.running) ArenaFightImpl(*this).setLoad(value);
}
void ArenaFight::setHealCharges(int charges) {
    if (phase_ == ArenaPhase::Fighting && !special_.view.running)
        player_.charges = static_cast<std::uint8_t>(std::clamp(charges, 0, static_cast<int>(healMax_)));
}
int ArenaFight::hostHeal(int amount) {
    if (phase_ != ArenaPhase::Fighting || special_.view.running || amount <= 0) return 0;
    return ArenaFightImpl(*this).hostHeal(amount);
}
int ArenaFight::hostHurt(int amount) {
    if (phase_ != ArenaPhase::Fighting || special_.view.running || amount <= 0) return 0;
    return ArenaFightImpl(*this).hostHurt(amount);
}
void ArenaFight::setCarry(bool blocksStore) { carry_ = blocksStore; }
bool ArenaFight::carry() const { return carry_; }
void ArenaFight::setChairs(const ArenaChairs& chairs) {
    if (special_.view.running) return;
    chairs_ = chairs;
    chairs_.count = static_cast<std::uint8_t>(std::min<std::size_t>(chairs_.count, chairs_.named.size()));
}
const ArenaChairs& ArenaFight::chairs() const { return chairs_; }
std::uint8_t ArenaFight::healChargesMax() const { return healMax_; }
std::uint8_t ArenaFight::roundsMax() const { return def_.fighter.rounds; }
std::uint8_t ArenaFight::rounds() const { return def_.fighter.rounds > 0 ? player_.rounds : static_cast<std::uint8_t>(0); }
bool ArenaFight::reloading() const {
    return def_.fighter.rounds > 0 && player_.rounds == 0 && player_.reloadLeft > 0.f && def_.fighter.reload > 0.f;
}
float ArenaFight::reloadProgress() const {
    const float r = def_.fighter.reload;
    if (def_.fighter.rounds == 0 || player_.rounds > 0 || player_.reloadLeft <= 0.f || !(r > 0.f)) return 0.f;
    return std::clamp(1.f - player_.reloadLeft / r, 0.f, 1.f);
}
void ArenaFight::setAlly(int maxHp, float radius) {
    if (special_.view.running) return;
    allyMaxHp_ = std::max(maxHp, 0);
    allyHpGiven_ = allyMaxHp_;
    allyHp_ = allyMaxHp_;
    allyRadius_ = std::isfinite(radius) && radius > 0.f ? radius : 0.4f;
}
int ArenaFight::allyHp() const { return allyHp_; }
int ArenaFight::allyMaxHp() const { return allyMaxHp_; }
bool ArenaFight::allyAlive() const { return allyMaxHp_ > 0 && allyHp_ > 0; }

void ArenaFight::setPartner(PartnerBrain* brain, const PartnerTuning* tuning) {
    PartnerBrain* const b = brain && tuning ? brain : nullptr;
    const PartnerTuning* const t = b ? tuning : nullptr;
    if (inHook_.on || special_.view.running || (b == partner_ && t == partnerTuning_)) return;
    partner_ = b;
    partnerTuning_ = t;
    ally_ = AllyState{};
    ally_.facing = player_.facing;
    ally_.heading = player_.facing;
    ally_.goal = allyPos_;
    if (partner_ && phase_ != ArenaPhase::NotStarted) partner_->begin(*partnerTuning_, seed_);
}

bool ArenaFight::swapped() const { return swapped_; }
bool ArenaFight::switchLeads() { return !special_.view.running && ArenaFightImpl(*this).switchLeads(); }
ArenaSwitchRefusal ArenaFight::switchRefusal() const { return switchRefusal_; }
Vec2 ArenaFight::allyPos() const { return allyPos_; }
float ArenaFight::allyRadius() const { return allyRadius_; }
void ArenaFight::setTableSeats(const ArenaTableSeats& seats) {
    if (special_.view.running) return;
    tableSeats_ = seats;
    for (float& v : tableSeats_.enemyShare) v = std::isfinite(v) ? std::clamp(v, 0.f, 4.f) : 0.f;
}
const ArenaTableSeats& ArenaFight::tableSeats() const { return tableSeats_; }
bool ArenaFight::takeHalfDue(float lead) const {
    if (phase_ != ArenaPhase::Fighting || lone_ || !allyAlive() || player_.hp <= 0) return false;
    if (arenaDistance(player_.pos, allyPos_) > def_.fighter.takeHalfReach) return false;
    for (std::size_t i = 0; i < enemyCount_; ++i) {
        const EnemyState& foe = enemy_[i];
        if (foe.brain != ArenaBrain::Attack) continue;
        const float left = enemyAttackNow(i).startup - foe.stateTime;
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
    case ArenaAction::Jump: return "Jump";
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
    case kArenaAirMove: return &kit.jumpLight;
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
    case kArenaAirMove: return arenaHeavyStrike(kit); // the air light: the kit's committed strike, downward from the air
    default: break;
    }
    if (move >= kit.lightCount || static_cast<std::size_t>(move) >= kMaxLightChain) return FightClip::Stance;
    FightClip p[kMaxLightChain];
    const std::size_t n = arenaStrikePattern(kit, p, kMaxLightChain);
    return static_cast<std::size_t>(move) < n ? p[move] : FightClip::Stance;
}

float arenaBoltFlight(float startup) {
    const float s = std::isfinite(startup) ? std::max(startup, 0.f) : 0.f;
    return std::max(s, kArenaBoltFlightMin);
}

float arenaBoltLate(float startup) {
    const float s = std::isfinite(startup) ? std::max(startup, 0.f) : 0.f;
    return arenaBoltFlight(s) - s;
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
    case ArenaBrain::StepBack: return "StepBack";
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
    def.partnerKit = def.fighter;
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
    c.stringEnd = stats.stringEndHits > 0;
    const auto verdict = [](bool can, bool shown) {
        return !can ? ArenaCheck::NotShown : (shown ? ArenaCheck::Pass : ArenaCheck::Fail);
    };
    c.lightsFreeCheck = verdict(stats.lights > 0, c.lightsFree);
    c.softTargetCheck = verdict(stats.softChances > 0 || stats.softTargets > 0, c.softTarget);
    c.cancelsCheck = verdict(stats.cancelChances > 0 || stats.cancels > 0, c.cancels);
    c.flinchCheck = verdict(stats.flinchChances > 0 || stats.flinches > 0, c.flinch);
    c.jumpCheck = verdict(stats.jumps > 0, stats.landings > 0);
    c.stringEndCheck = verdict(stats.stringEnds > 0, c.stringEnd);
    if (stats.breakOuts > 0)
        c.breakOutCheck = ArenaCheck::Pass;
    else if (stats.breakDue > stats.breakBarred + stats.stepBacks + stats.stepBacksBlocked)
        c.breakOutCheck = ArenaCheck::Fail; // due, the move free, and still none
    else if (stats.breakBarred > 0 || stats.stepBacks > 0 || stats.stepBacksBlocked > 0 ||
             (stats.breakUpright > 0 && stats.breakVetoed == stats.breakUpright))
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

namespace {
bool arenaRingHold(const void* wall, Vec2& p, float reach) {
    const Vec2 q = static_cast<const ArenaRing*>(wall)->clamp(p, reach);
    const bool moved = q.x != p.x || q.y != p.y;
    p = q;
    return moved;
}
} // namespace

void arenaStep(const ArenaFight& fight, ArenaBodies& bodies, float dt, int settleCap) {
    const ArenaSpecialView& sv = fight.special();
    if (fight.frozen()) { // a special runs: the host poses the performer (from its start), nobody else moves
        (sv.performer == kArenaPlayer ? bodies.player : bodies.ally) = sv.origin;
        return;
    }
    const bool placed = sv.endedThisTick && sv.performer == kArenaPlayer; // the cut back: put where the clip ended
    if (sv.endedThisTick) (sv.performer == kArenaPlayer ? bodies.player : bodies.ally) = sv.end;
    const std::size_t n = fight.enemyCount();
    const ArenaOutput& out = fight.output();
    if (dt > 0.f) {
        bodies.player = arenaMadd(bodies.player, out.player.velocity, dt);
        for (std::size_t i = 0; i < n; ++i) bodies.enemies[i] = arenaMadd(bodies.enemies[i], out.enemies[i].velocity, dt);
    }
    // Overlapping bodies are pushed apart, half each; dead enemies don't block. No push apart with the player on the
    // frame the performer is placed.
    for (int pass = 0; pass < kArenaPushPasses; ++pass) arenaPushApart(fight, bodies, !placed);
    // Everything stays inside the ring; a body the ring moved can't take its share of a push, so the settle gives it
    // to the other one, and the ring holds them again, until no contact is deeper than 0.1 mm (arenaSettleBodies).
    const ArenaRing& ring = fight.ring();
    ArenaHeld held;
    arenaHoldBodies(fight, bodies, held, arenaRingHold, &ring);
    if (!arenaSettleBodies(fight, bodies, !placed, held, arenaRingHold, &ring, settleCap).settled) ++bodies.settleCaps;
}

bool arenaHoldBodies(const ArenaFight& fight, ArenaBodies& bodies, ArenaHeld& held, ArenaHoldFn hold, const void* wall) {
    const ArenaDef& def = fight.def();
    bool moved = false;
    if (hold(wall, bodies.player, def.fighter.bodyRadius)) moved = held.player = true;
    for (std::size_t i = 0; i < fight.enemyCount(); ++i)
        if (hold(wall, bodies.enemies[i], arenaBodyReach(def.enemies[i]))) moved = held.enemies[i] = true;
    return moved;
}

ArenaSettleRun arenaSettleBodies(const ArenaFight& fight, ArenaBodies& bodies, bool withPlayer, ArenaHeld& held,
                                 ArenaHoldFn hold, const void* wall, int maxPasses) {
    ArenaSettleRun run;
    const int cap = std::max(1, maxPasses);
    while (run.passes < cap) {
        ++run.passes;
        if (!arenaSettle(fight, bodies, withPlayer, held)) {
            run.settled = true;
            break;
        }
        arenaHoldBodies(fight, bodies, held, hold, wall);
    }
    return run;
}

namespace {

// A body's spine: its centre +- span along its facing (span 0: a point, the circle's centre).
struct ArenaSpine {
    Vec2 centre;
    Vec2 axis; // unit
    float span = 0.f;
};

// The closest points of two spines (segments; either may be a point).
void arenaClosest(const ArenaSpine& a, const ArenaSpine& b, Vec2& pa, Vec2& pb) {
    // A spine shorter than kArenaEps either side is its centre (testing the squared length against kArenaEps once
    // took spines up to 5 mm long for a point at one end).
    const float spanA = a.span > kArenaEps ? a.span : 0.f, spanB = b.span > kArenaEps ? b.span : 0.f;
    const Vec2 a0 = arenaMadd(a.centre, a.axis, -spanA);
    const Vec2 b0 = arenaMadd(b.centre, b.axis, -spanB);
    const Vec2 da = Vec2{a.axis.x * 2.f * spanA, a.axis.y * 2.f * spanA};
    const Vec2 db = Vec2{b.axis.x * 2.f * spanB, b.axis.y * 2.f * spanB};
    const Vec2 r = arenaSub(a0, b0);
    const float aa = arenaDot(da, da), ee = arenaDot(db, db), ff = arenaDot(db, r);
    float s = 0.f, t = 0.f;
    if (spanA <= 0.f && spanB <= 0.f) {
        // both points
    } else if (spanA <= 0.f) {
        t = std::clamp(ff / ee, 0.f, 1.f);
    } else {
        const float c = arenaDot(da, r);
        if (spanB <= 0.f) {
            s = std::clamp(-c / aa, 0.f, 1.f);
        } else {
            const float bb = arenaDot(da, db);
            // Parallel when the sine of the angle is under 0.001 (a relative test: two pups' 0.38 m spines a few
            // degrees apart are not parallel).
            const float denom = aa * ee - bb * bb;
            if (denom > 1.0e-6f * aa * ee) {
                s = std::clamp((bb * ff - c * ee) / denom, 0.f, 1.f);
                t = (bb * s + ff) / ee;
                if (t < 0.f) { // t clamped, s found again from it
                    t = 0.f;
                    s = std::clamp(-c / aa, 0.f, 1.f);
                } else if (t > 1.f) {
                    t = 1.f;
                    s = std::clamp((bb - c) / aa, 0.f, 1.f);
                }
            } else {
                // (Near) parallel: the closest pair has an end of one spine in it, so the best of the four ends
                // against the other spine (s = 0 alone was up to 0.5 mm off on 1.2 m spines 0.03 degrees apart).
                float best = -1.f;
                const auto keep = [&](float sc, float tc) {
                    const Vec2 d = arenaSub(arenaMadd(a0, da, sc), arenaMadd(b0, db, tc));
                    const float dd = arenaDot(d, d);
                    if (best < 0.f || dd < best) {
                        best = dd;
                        s = sc;
                        t = tc;
                    }
                };
                keep(0.f, std::clamp(ff / ee, 0.f, 1.f));        // a's first end against b
                keep(1.f, std::clamp((bb + ff) / ee, 0.f, 1.f)); // a's last end
                keep(std::clamp(-c / aa, 0.f, 1.f), 0.f);        // b's first end against a
                keep(std::clamp((bb - c) / aa, 0.f, 1.f), 1.f);  // b's last end
            }
        }
    }
    pa = arenaMadd(a0, da, s);
    pb = arenaMadd(b0, db, t);
}

// Pushes two bodies apart when they overlap by more than `floor`: a takes `shareA` of the overlap, b the rest.
// Two circles at half each step exactly as the push always did. True when it pushed.
bool arenaPushPair(Vec2& a, float ra, const ArenaSpine& sa, Vec2& b, float rb, const ArenaSpine& sb, float shareA,
                   float floor) {
    Vec2 pa = a, pb = b;
    if (sa.span > 0.f || sb.span > 0.f) arenaClosest(sa, sb, pa, pb);
    const Vec2 d = arenaSub(pb, pa);
    const float distance = arenaLength(d);
    const float least = ra + rb;
    if (distance >= least - floor) return false;
    // Crossed spines meet at one point: push along the centres instead (for two circles that is the same line).
    Vec2 dir{1.f, 0.f};
    if (distance > kArenaEps) {
        dir = Vec2{d.x / distance, d.y / distance};
    } else {
        const Vec2 c = arenaSub(b, a);
        const float apart = arenaLength(c);
        if (apart > kArenaEps) dir = Vec2{c.x / apart, c.y / apart};
    }
    const float overlap = least - distance;
    a = arenaMadd(a, dir, -(shareA * overlap));
    b = arenaMadd(b, dir, (1.f - shareA) * overlap);
    return true;
}

// The push passes' shared walk: the player against each living enemy (withPlayer), then each living pair. True
// when it pushed a pair.
bool arenaPushWalk(const ArenaFight& fight, ArenaBodies& bodies, bool withPlayer, const ArenaHeld* held, float floor) {
    const std::size_t n = fight.enemyCount();
    const ArenaDef& def = fight.def();
    const ArenaOutput& out = fight.output();
    const float playerRadius = def.fighter.bodyRadius;
    const auto share = [held](bool heldA, bool heldB) {
        if (held == nullptr || heldA == heldB) return 0.5f;
        return heldA ? 0.f : 1.f;
    };
    const auto spine = [&](std::size_t i) {
        return ArenaSpine{bodies.enemies[i], arenaUnit(out.enemies[i].facing, Vec2{1.f, 0.f}), arenaBodySpan(def.enemies[i])};
    };
    bool pushed = false;
    for (std::size_t i = 0; i < n && withPlayer; ++i)
        if (fight.enemyAlive(i))
            pushed |= arenaPushPair(bodies.player, playerRadius, ArenaSpine{bodies.player, Vec2{1.f, 0.f}, 0.f}, bodies.enemies[i],
                          def.enemies[i].bodyRadius, spine(i),
                          share(held && held->player, held && held->enemies[i]), floor);
    for (std::size_t i = 0; i < n; ++i) {
        if (!fight.enemyAlive(i)) continue;
        for (std::size_t j = i + 1; j < n; ++j)
            if (fight.enemyAlive(j))
                pushed |= arenaPushPair(bodies.enemies[i], def.enemies[i].bodyRadius, spine(i), bodies.enemies[j],
                              def.enemies[j].bodyRadius, spine(j), share(held && held->enemies[i], held && held->enemies[j]),
                              floor);
    }
    return pushed;
}
} // namespace

void arenaPushApart(const ArenaFight& fight, ArenaBodies& bodies, bool withPlayer) {
    arenaPushWalk(fight, bodies, withPlayer, nullptr, 0.f);
}

bool arenaSettle(const ArenaFight& fight, ArenaBodies& bodies, bool withPlayer, const ArenaHeld& held, float floor) {
    return arenaPushWalk(fight, bodies, withPlayer, &held, floor);
}

void arenaClosestPoints(Vec2 centreA, Vec2 axisA, float spanA, Vec2 centreB, Vec2 axisB, float spanB, Vec2& pa,
                        Vec2& pb) {
    arenaClosest(ArenaSpine{centreA, axisA, spanA}, ArenaSpine{centreB, axisB, spanB}, pa, pb);
}

namespace {
// One pass of the ally's placement: off each living enemy, in enemy order. Whether any overlap was deeper than `floor`
// (a shallower one is still closed, so the pass ends exact, but it does not keep the loop going).
bool arenaAllyPass(const ArenaFight& fight, ArenaBodies& bodies, float floor) {
    const ArenaDef& def = fight.def();
    const ArenaOutput& out = fight.output();
    const float radius = fight.allyRadius();
    bool deep = false;
    for (std::size_t i = 0; i < fight.enemyCount(); ++i) {
        if (!fight.enemyAlive(i)) continue;
        // A circle stands it off its centre; a stadium (arenaBodySpan) off the nearest point of its spine, along the
        // enemy's facing as the step's push. On the spine itself: off the centre (the step's rule for crossed spines).
        Vec2 from = bodies.enemies[i];
        const float span = arenaBodySpan(def.enemies[i]);
        if (span > 0.f) {
            Vec2 pa = bodies.ally;
            arenaClosest(ArenaSpine{bodies.ally, Vec2{1.f, 0.f}, 0.f},
                         ArenaSpine{bodies.enemies[i], arenaUnit(out.enemies[i].facing, Vec2{1.f, 0.f}), span}, pa, from);
            if (arenaDistance(bodies.ally, from) <= kArenaEps) from = bodies.enemies[i];
        }
        const float least = radius + def.enemies[i].bodyRadius;
        const Vec2 off = arenaSub(bodies.ally, from);
        const float apart = arenaLength(off);
        if (apart >= least) continue;
        deep = deep || apart < least - floor;
        const Vec2 dir = apart > 1e-4f ? Vec2{off.x / apart, off.y / apart} : Vec2{1.f, 0.f};
        bodies.ally = Vec2{from.x + dir.x * least, from.y + dir.y * least};
    }
    return deep;
}
} // namespace

ArenaSettleRun arenaPlaceAlly(const ArenaFight& fight, ArenaBodies& bodies, ArenaHoldFn hold, const void* wall,
                              int maxPasses) {
    const int cap = std::max(maxPasses, 1);
    const float radius = fight.allyRadius();
    for (int pass = 1; pass <= cap; ++pass) {
        const bool deep = arenaAllyPass(fight, bodies, kArenaSettleFloor);
        const bool held = hold(wall, bodies.ally, radius); // the wall last: it wins a wedge
        if (!deep && !held) return ArenaSettleRun{pass, true};
    }
    return ArenaSettleRun{cap, false};
}

ArenaSettleRun arenaPlaceAlly(const ArenaFight& fight, ArenaBodies& bodies, int maxPasses) {
    return arenaPlaceAlly(fight, bodies, arenaRingHold, &fight.ring(), maxPasses);
}

// ---- ArenaTable ----

const ArenaFighter& ArenaTable::fighter(std::string_view leadId) const {
    if (const ArenaFighter* own = findFighter(leadId)) return *own;
    return fighters.empty() ? fallback.fighter : fighters[0];
}

const ArenaFighter* ArenaTable::findFighter(std::string_view leadId) const {
    const std::size_t count = std::min(fighterIds.size(), fighters.size());
    for (std::size_t i = 0; i < count; ++i)
        if (std::string_view(fighterIds[i]) == leadId) return &fighters[i];
    return nullptr;
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
//             loadShare 0..10; releasePoise 0..1000; pairWindow 0..1; pairHits 0..10; pairBossPoise 0..1;
//             pairCooldown 0..30; pairPartnerSeconds 0..10; parryStop 0..0.3;
//             softRange 0..100; softArcCos -1..1; homingReach 0..20; hurtJolt 0..kArenaMaxJolt;
//             attacks a bool; release an attack;
//             readPower 0..100; readStackPower 0..10; readPoise, readWrongPoise 0..100000;
//             blockLoadShare 0..1; blockCallGuard 0..1; takeHalfWindow 0.05..0.6; takeHalfReach 1..10; takeHalfLoadShare 0..2;
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
                                                 "jolt",    "softTurn",    "homing",      "push",   "pushTime"};

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
    arenaNum(j, "softTurn", a.softTurn, -1, 180, false, path, errors);   // < 0: no limit
    arenaNum(j, "homing", a.homing, -1, 100, false, path, errors);       // < 0: the kit's homingReach
    arenaNum(j, "push", a.push, 0, 20, false, path, errors);
    arenaNum(j, "pushTime", a.pushTime, 0, 5, true, path, errors);       // > 0
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
                      "releasePoise", "pairWindow", "pairHits", "pairBossPoise", "pairCooldown", "pairPartnerSeconds",
                      "readPower", "readStackPower", "readPoise",
                      "readWrongPoise", "attacks", "playable", "shot", "parryStop", "softRange", "softArcCos", "homingReach",
                      "hurtJolt", "blockLoadShare", "blockCallGuard", "takeHalfWindow", "takeHalfReach", "takeHalfLoadShare",
                      "takeHalfRearm", "rounds", "reload", "jumpStartup", "jumpAir", "jumpLand", "jumpLandLock",
                      "jumpHeight", "jumpClearStart", "jumpClearEnd", "jumpAirControl", "jumpLight", "partnerReach",
                      "partnerStandoff"},
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
    arenaBool(j, "playable", f.playable, path, errors);
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
    arenaNum(j, "pairBossPoise", f.pairBossPoise, 0, 1, false, path, errors);
    arenaNum(j, "pairCooldown", f.pairCooldown, 0, 30, false, path, errors);
    arenaNum(j, "pairPartnerSeconds", f.pairPartnerSeconds, 0, 10, false, path, errors);
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
    arenaNum(j, "blockCallGuard", f.blockCallGuard, 0, 1, false, path, errors);
    arenaNum(j, "takeHalfWindow", f.takeHalfWindow, 0.05f, 0.6f, false, path, errors);
    arenaNum(j, "takeHalfReach", f.takeHalfReach, 1, 10, false, path, errors);
    arenaNum(j, "takeHalfLoadShare", f.takeHalfLoadShare, 0, 2, false, path, errors);
    arenaNum(j, "takeHalfRearm", f.takeHalfRearm, 0, 2, false, path, errors);
    arenaByte(j, "rounds", f.rounds, 0, 30, path, errors);
    arenaNum(j, "reload", f.reload, 0, 10, false, path, errors);
    arenaNum(j, "jumpStartup", f.jumpStartup, 0, 2, false, path, errors);
    arenaNum(j, "jumpAir", f.jumpAir, 0, 3, false, path, errors);
    arenaNum(j, "jumpLand", f.jumpLand, 0, 2, false, path, errors);
    arenaNum(j, "jumpLandLock", f.jumpLandLock, 0, 2, false, path, errors);
    arenaNum(j, "jumpHeight", f.jumpHeight, 0, 5, false, path, errors);
    arenaNum(j, "jumpClearStart", f.jumpClearStart, 0, 5, false, path, errors);
    arenaNum(j, "jumpClearEnd", f.jumpClearEnd, 0, 5, false, path, errors);
    arenaNum(j, "jumpAirControl", f.jumpAirControl, 0, 1000, false, path, errors);
    if (j.has("jumpLight")) arenaParseAttackObject(j["jumpLight"], path + ".jumpLight", f.jumpLight, errors, warnings);
    // Up to the brain's own bound (kPartnerRangeMax: the caster band's far end and the tolerance past it).
    arenaNum(j, "partnerReach", f.partnerReach, 0, kPartnerRangeMax, false, path, errors);
    arenaNum(j, "partnerStandoff", f.partnerStandoff, 0, kPartnerRangeMax, false, path, errors);
    if (f.partnerReach > 0.f && f.partnerReach < 0.5f)
        errors.push_back(path + ".partnerReach: expected 0 (unset) or 0.5..8.5");
    if (f.partnerStandoff > 0.f && f.partnerReach == 0.f) // the default reach may sit below the standoff
        errors.push_back(path + ".partnerStandoff: expected a partnerReach with it");
    else if (f.partnerStandoff > f.partnerReach)
        errors.push_back(path + ".partnerStandoff: expected at most partnerReach");

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
    if (f.jumpClearStart > f.jumpClearEnd) errors.push_back(path + ".jumpClearStart: expected at most jumpClearEnd");
    if (f.jumpClearEnd > f.jumpStartup + f.jumpAir + 1e-4f)
        errors.push_back(path + ".jumpClearEnd: expected at most jumpStartup + jumpAir");
    if (f.jumpLandLock > f.jumpLand) errors.push_back(path + ".jumpLandLock: expected at most jumpLand");
    if (f.rounds > 0 && f.weapon != ArenaWeapon::Guns) errors.push_back(path + ".rounds: only a guns kit has a cylinder");
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
                      "strafeSpeed", "turnRate", "preferredRange", "runRange", "attackDelay", "bodyRadius", "height", "bodyLength",
                      "moves",
                      "accel", "bpm", "look", "passive", "flinchTime", "flinchPoise", "flinchPush", "breakAt",
                      "breakDecay", "breakMove", "carry", "seatable", "boss", "breakValueAt", "breakPhaseEnds"},
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
    // 0 (absent): a circle. A length shorter than the circle (under 2 x bodyRadius) would silently be a circle too, so it
    // is refused: a kind is a circle (0) or a stadium at least as long as it is wide.
    if (arenaNum(j, "bodyLength", k.bodyLength, 0, 20, false, path, errors) && k.bodyLength > 0.f &&
        k.bodyLength < 2.f * k.bodyRadius)
        errors.push_back(path + ".bodyLength: expected 0 (a circle) or at least 2 x bodyRadius (" +
                         arenaFormat(2.0 * k.bodyRadius) + ")");
    arenaNum(j, "bpm", k.bpm, 0, 300, true, path, errors);
    arenaNum(j, "flinchTime", k.flinchTime, 0, 10, false, path, errors);
    arenaNum(j, "flinchPoise", k.flinchPoise, 0, 100000, false, path, errors);
    arenaNum(j, "flinchPush", k.flinchPush, 0, 20, false, path, errors);
    arenaByte(j, "breakAt", k.breakAt, 0, 255, path, errors);
    arenaNum(j, "breakDecay", k.breakDecay, 0, 60, false, path, errors);
    arenaInt(j, "breakMove", k.breakMove, -1, static_cast<int>(kMaxArenaMoves) - 1, path, errors);
    arenaBool(j, "boss", k.boss, path, errors);
    const JsonValue breakAtList = j["breakValueAt"];
    if (!breakAtList.isAbsent()) {
        if (!breakAtList.isArray() || breakAtList.size() == 0 || breakAtList.size() > kMaxArenaBreakPhases) {
            errors.push_back(path + ".breakValueAt: expected an array of 1.." + std::to_string(kMaxArenaBreakPhases) +
                             " numbers");
        } else {
            k.breakPhases = static_cast<std::uint8_t>(breakAtList.size());
            for (std::uint32_t p = 0; p < breakAtList.size(); ++p) {
                const double d = breakAtList[p].number(-1.0);
                if (!breakAtList[p].isNumber() || !std::isfinite(d) || d < 0.5 || d > 1000.0)
                    errors.push_back(path + ".breakValueAt[" + std::to_string(p) + "]: expected a number in [0.5, 1000]");
                else
                    k.breakValueAt[p] = static_cast<float>(d);
            }
            for (std::uint32_t p = 0; p + 1 < k.breakPhases; ++p) // even shares unless breakPhaseEnds says
                k.breakPhaseEnds[p] = 1.f - static_cast<float>(p + 1) / static_cast<float>(k.breakPhases);
        }
    }
    const JsonValue breakEndList = j["breakPhaseEnds"];
    if (!breakEndList.isAbsent()) {
        if (!breakEndList.isArray() || breakEndList.size() + 1 != k.breakPhases) {
            errors.push_back(path + ".breakPhaseEnds: expected one hp share per break phase but the last (" +
                             std::to_string(k.breakPhases - 1) + ")");
        } else {
            double before = 1.0;
            for (std::uint32_t p = 0; p < breakEndList.size(); ++p) {
                const double d = breakEndList[p].number(-1.0);
                if (!breakEndList[p].isNumber() || !std::isfinite(d) || !(d > 0.0) || !(d < before)) {
                    errors.push_back(path + ".breakPhaseEnds[" + std::to_string(p) +
                                     "]: expected a share in (0, 1) below the one before");
                    break;
                }
                k.breakPhaseEnds[p] = static_cast<float>(d);
                before = d;
            }
        }
    }
    arenaByte(j, "carry", k.carry, 0, 255, path, errors);
    arenaBool(j, "seatable", k.seatable, path, errors);
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
                         {"minRange", "maxRange", "cooldown", "weight", "chain", "unblockable", "parryable", "tell", "clip",
                          "hitsAir"},
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
        // A sweep or a slam runs along the floor: a jump clears it unless the file says otherwise.
        mv.hitsAir = mv.tell != ArenaTell::Sweep && mv.tell != ArenaTell::Slam;
        arenaBool(mj, "hitsAir", mv.hitsAir, mp, errors);
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
    if (k.boss) { // Boss break: its break move is what the value fills toward
        if (k.breakAt > 0) errors.push_back(path + ".breakAt: a boss breaks by its break value, not by a count");
        if (k.breakMove < 0)
            errors.push_back(path + ".breakMove: a boss needs the move it breaks out with");
        else if (k.breakMove < static_cast<int>(k.moveCount) && k.moves[static_cast<std::size_t>(k.breakMove)].chain >= 0)
            errors.push_back(path + ".breakMove: a boss's break move may not chain");
    } else if (!breakAtList.isAbsent() || !breakEndList.isAbsent()) {
        errors.push_back(path + ": breakValueAt and breakPhaseEnds are a boss's (\"boss\": true)");
    }
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

void arenaOverlayFighter(const JsonValue& j, const std::string& path, ArenaFighter& f, std::vector<std::string>& errors,
                         std::vector<std::string>* warnings) {
    if (!j.isObject()) {
        errors.push_back(path + ": expected an object");
        return;
    }
    arenaParseFighter(j, path, f, errors, warnings);
}

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
    arenaUnknownKeys(root, "", {"version", "bpm", "partner", "fighters", "enemies", "default", "fights"}, false,
                     warnings);

    ArenaTable table;
    if (const JsonValue p = root["partner"]; !p.isAbsent()) {
        if (!p.isObject()) {
            errors.push_back("partner: expected an object");
        } else {
            arenaUnknownKeys(p, "partner", {"poise", "breakWeight"}, false, warnings);
            arenaNum(p, "poise", table.partner.poise, 0, 1, false, "partner", errors);
            arenaNum(p, "breakWeight", table.partner.breakWeight, 0, 1, false, "partner", errors);
        }
    }
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
        table.fallback.partnerKit = defaultFighter;
        table.fallback.bpm = table.bpm;
        table.fallback.partner = table.partner;
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
        entry.def.partnerKit = defaultFighter;
        entry.def.bpm = table.bpm;
        entry.def.partner = table.partner;
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

// ---- ArenaSpecial.h: the special moves' data, the gauge store ----
namespace {

bool spId(std::string_view id) {
    if (id.empty() || id.size() > kMaxArenaSpecialId) return false;
    for (const char c : id)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
    return true;
}

bool spFxId(std::string_view id) {
    if (id.empty() || id.size() > kMaxArenaSpecialId) return false;
    for (const char c : id)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) return false;
    return true;
}

template <std::size_t N>
void spCopy(std::string_view s, std::array<char, N>& out) {
    out.fill('\0');
    std::copy_n(s.begin(), std::min(s.size(), N - 1), out.begin());
}

bool spInRange(float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; }

// Whether a row's start (and its end) land inside [0, duration] at the nearest and the farthest impact, end >= start.
bool spFxInMove(const ArenaSpecialRules& r, const ArenaSpecialFx& f) {
    for (const float impact : {r.strikeAt, r.strikeAt + r.maxTravel}) {
        const float a = arenaSpecialFxAt(r, f.start, impact);
        if (!spInRange(a, 0.f, r.duration)) return false;
        if (f.hasEnd) {
            const float b = arenaSpecialFxAt(r, f.end, impact);
            if (!spInRange(b, a, r.duration)) return false;
        }
    }
    return true;
}

void spUnknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys,
                   std::vector<std::string>* warnings) {
    if (!warnings) return;
    for (std::uint32_t k = 0; k < j.size(); ++k) {
        const std::string_view key = j.key(k);
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            warnings->push_back((path.empty() ? std::string() : path + ": ") + "unknown key: " + std::string(key));
    }
}

// An optional number in [lo, hi] into `out`.
void spNumber(JsonValue j, const char* key, const std::string& path, float lo, float hi, float& out,
              std::vector<std::string>& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent()) return;
    const double d = v.number(-1.0e30);
    if (!v.isNumber() || !std::isfinite(d) || d < static_cast<double>(lo) || d > static_cast<double>(hi)) {
        errors.push_back(path + "." + key + ": expected a number in " + std::to_string(lo) + ".." + std::to_string(hi));
        return;
    }
    out = static_cast<float>(d);
}

void spMove(JsonValue j, const std::string& path, ArenaSpecialRules& r, std::vector<std::string>& errors,
            std::vector<std::string>* warnings) {
    if (!j.isObject()) {
        errors.push_back(path + ": expected an object");
        return;
    }
    spUnknownKeys(j, path,
                  {"id", "lead", "duration", "strikeAt", "waveSpeed", "maxTravel", "range", "laneLength", "laneWidth", "damage", "otherShare",
                   "bossPhaseShare", "knockBack", "knockTime", "endTravel", "endSide", "skippable", "cues", "fx"},
                  warnings);
    for (const char* key : {"id", "lead"}) {
        const JsonValue v = j[key];
        if (!v.isString() || !spId(v.string())) {
            errors.push_back(path + "." + key + ": expected an id of 1.." + std::to_string(kMaxArenaSpecialId) +
                             " letters, digits, _ and -");
            continue;
        }
        spCopy(v.string(), key[0] == 'i' ? r.id : r.lead);
    }
    spNumber(j, "duration", path, 0.5f, 30.f, r.duration, errors);
    spNumber(j, "strikeAt", path, 0.f, 30.f, r.strikeAt, errors);
    spNumber(j, "waveSpeed", path, 1.f, 1000.f, r.waveSpeed, errors);
    spNumber(j, "maxTravel", path, 0.f, 5.f, r.maxTravel, errors);
    spNumber(j, "range", path, 1.f, 200.f, r.range, errors);
    spNumber(j, "laneLength", path, 0.5f, 100.f, r.laneLength, errors);
    spNumber(j, "laneWidth", path, 0.5f, 50.f, r.laneWidth, errors);
    if (const JsonValue v = j["damage"]; !v.isAbsent()) {
        if (!v.isInteger() || v.integer() < 1 || v.integer() > 100000)
            errors.push_back(path + ".damage: expected an integer in 1..100000");
        else
            r.damage = static_cast<int>(v.integer());
    }
    spNumber(j, "otherShare", path, 0.f, 1.f, r.otherShare, errors);
    spNumber(j, "bossPhaseShare", path, 0.f, 1.f, r.bossPhaseShare, errors);
    spNumber(j, "knockBack", path, 0.f, 20.f, r.knockBack, errors);
    spNumber(j, "knockTime", path, 0.05f, 5.f, r.knockTime, errors);
    spNumber(j, "endTravel", path, 0.f, 20.f, r.endTravel, errors);
    spNumber(j, "endSide", path, -5.f, 5.f, r.endSide, errors);
    if (const JsonValue v = j["skippable"]; !v.isAbsent()) {
        if (!v.isBool())
            errors.push_back(path + ".skippable: expected true or false");
        else
            r.skippable = v.boolean();
    }
    if (!(r.strikeAt > 0.f && r.strikeAt <= r.duration))
        errors.push_back(path + ".strikeAt: expected a time in (0, duration]");
    else if (!(r.strikeAt + r.maxTravel + r.knockTime <= r.duration))
        errors.push_back(path + ".maxTravel: strikeAt + maxTravel + knockTime must be within duration");

    const JsonValue cues = j["cues"];
    if (cues.isAbsent()) return;
    if (!cues.isArray() || cues.size() > kMaxArenaSpecialCues) {
        errors.push_back(path + ".cues: expected an array of 0.." + std::to_string(kMaxArenaSpecialCues) + " cues");
        return;
    }
    float last = 0.f;
    for (std::uint32_t i = 0; i < cues.size(); ++i) {
        const std::string cp = path + ".cues[" + std::to_string(i) + "]";
        const JsonValue c = cues[i];
        if (!c.isObject()) {
            errors.push_back(cp + ": expected an object");
            continue;
        }
        spUnknownKeys(c, cp, {"at", "cue"}, warnings);
        ArenaSpecialCue& out = r.cues[r.cueCount++];
        const JsonValue at = c["at"];
        const double t = at.number(-1.0);
        if (!at.isNumber() || !std::isfinite(t) || t < 0.0 || t > static_cast<double>(r.duration) ||
            static_cast<float>(t) < last)
            errors.push_back(cp + ".at: expected a time in [0, duration], not before the cue above");
        else
            out.at = last = static_cast<float>(t);
        const JsonValue name = c["cue"];
        if (!name.isString() || !spId(name.string()))
            errors.push_back(cp + ".cue: expected a name of 1.." + std::to_string(kMaxArenaSpecialId) +
                             " letters, digits, _ and -");
        else
            spCopy(name.string(), out.name);
    }

    const JsonValue fx = j["fx"];
    if (fx.isAbsent()) return;
    if (!fx.isArray() || fx.size() > kMaxArenaSpecialFx) {
        errors.push_back(path + ".fx: expected an array of 0.." + std::to_string(kMaxArenaSpecialFx) + " rows");
        return;
    }
    // A cue + offset: the cue by name (or "impact"), the offset -5..5 s.
    const auto readTime = [&](JsonValue t, const std::string& tp, ArenaSpecialFxTime& out) {
        const JsonValue name = t["cue"];
        bool found = false;
        if (name.isString() && name.string() == "impact") {
            out.cue = kArenaSpecialAtImpact;
            found = true;
        } else if (name.isString()) {
            for (std::uint8_t k = 0; k < r.cueCount && !found; ++k)
                if (r.cues[k].view() == name.string()) {
                    out.cue = k;
                    found = true;
                }
        }
        if (!found) errors.push_back(tp + ".cue: expected a cue of this move, or impact");
        const JsonValue at = t["at"];
        const double v = at.number(0.0);
        if (!at.isAbsent() && (!at.isNumber() || !std::isfinite(v) || v < -5.0 || v > 5.0))
            errors.push_back(tp + ".at: expected an offset in -5..5 s");
        else
            out.offset = static_cast<float>(v);
        return found;
    };
    for (std::uint32_t i = 0; i < fx.size(); ++i) {
        const std::string fp = path + ".fx[" + std::to_string(i) + "]";
        const JsonValue f = fx[i];
        if (!f.isObject()) {
            errors.push_back(fp + ": expected an object");
            continue;
        }
        spUnknownKeys(f, fp, {"id", "cue", "at", "to", "for", "low", "note"}, warnings);
        ArenaSpecialFx& out = r.fx[r.fxCount++];
        const JsonValue id = f["id"];
        if (!id.isString() || !spFxId(id.string()))
            errors.push_back(fp + ".id: expected a name of 1.." + std::to_string(kMaxArenaSpecialId) +
                             " letters, digits, _, - and .");
        else
            spCopy(id.string(), out.id);
        bool ok = readTime(f, fp, out.start);
        const JsonValue to = f["to"], len = f["for"];
        if (!to.isAbsent() && !len.isAbsent()) {
            errors.push_back(fp + ": \"to\" and \"for\" both given");
            ok = false;
        } else if (!to.isAbsent()) {
            if (!to.isObject()) {
                errors.push_back(fp + ".to: expected { \"cue\": ..., \"at\": s }");
                ok = false;
            } else {
                spUnknownKeys(to, fp + ".to", {"cue", "at"}, warnings);
                ok = readTime(to, fp + ".to", out.end) && ok;
                out.hasEnd = true;
            }
        } else if (!len.isAbsent()) {
            const double v = len.number(-1.0);
            if (!len.isNumber() || !std::isfinite(v) || v <= 0.0 || v > 10.0) {
                errors.push_back(fp + ".for: expected a length in (0, 10] s");
                ok = false;
            } else {
                out.length = static_cast<float>(v);
            }
        }
        if (const JsonValue low = f["low"]; !low.isAbsent()) {
            if (!low.isBool())
                errors.push_back(fp + ".low: expected true or false");
            else
                out.low = low.boolean();
        }
        if (ok && !spFxInMove(r, out))
            errors.push_back(fp + ": its start or end lands outside [0, duration] at the nearest or farthest impact, "
                                  "or ends before it starts");
    }
}

} // namespace

bool validArenaSpecialRules(const ArenaSpecialRules& r) {
    if (!spId(r.idView()) || !spId(r.leadView())) return false;
    if (!spInRange(r.duration, 0.5f, 30.f) || !(r.strikeAt > 0.f && r.strikeAt <= r.duration)) return false;
    if (!spInRange(r.waveSpeed, 1.f, 1000.f) || !spInRange(r.maxTravel, 0.f, 5.f)) return false;
    if (!(r.strikeAt + r.maxTravel + r.knockTime <= r.duration)) return false;
    if (!spInRange(r.range, 1.f, 200.f) || !spInRange(r.laneLength, 0.5f, 100.f) || !spInRange(r.laneWidth, 0.5f, 50.f))
        return false;
    if (r.damage < 1 || r.damage > 100000) return false;
    if (!spInRange(r.otherShare, 0.f, 1.f) || !spInRange(r.bossPhaseShare, 0.f, 1.f)) return false;
    if (!spInRange(r.knockBack, 0.f, 20.f) || !spInRange(r.knockTime, 0.05f, 5.f)) return false;
    if (!spInRange(r.endTravel, 0.f, 20.f) || !spInRange(r.endSide, -5.f, 5.f)) return false;
    if (r.cueCount > kMaxArenaSpecialCues) return false;
    float last = 0.f;
    for (std::size_t i = 0; i < r.cueCount; ++i) {
        const ArenaSpecialCue& c = r.cues[i];
        if (!spInRange(c.at, last, r.duration) || !spId(c.view())) return false;
        last = c.at;
    }
    if (r.fxCount > kMaxArenaSpecialFx) return false;
    for (std::size_t i = 0; i < r.fxCount; ++i) {
        const ArenaSpecialFx& f = r.fx[i];
        if (!spFxId(f.view()) || !spFxInMove(r, f)) return false;
        if (f.hasEnd ? f.length != 0.f : !spInRange(f.length, 0.f, 10.f)) return false;
    }
    return true;
}

float arenaSpecialFxAt(const ArenaSpecialRules& r, const ArenaSpecialFxTime& at, float impact) {
    if (at.cue == kArenaSpecialAtImpact) return impact + at.offset;
    if (at.cue >= r.cueCount || at.cue >= kMaxArenaSpecialCues) return std::numeric_limits<float>::quiet_NaN();
    return r.cues[at.cue].at + at.offset;
}

float arenaSpecialFxEnd(const ArenaSpecialRules& r, const ArenaSpecialFx& fx, float impact) {
    return fx.hasEnd ? arenaSpecialFxAt(r, fx.end, impact) : arenaSpecialFxAt(r, fx.start, impact) + fx.length;
}

float arenaSpecialImpactAt(const ArenaSpecialRules& r, float distance) {
    const float d = distance > 0.f ? distance : 0.f; // NaN: 0
    const float travel = r.waveSpeed > 0.f ? d / r.waveSpeed : 0.f;
    return r.strikeAt + std::min(travel, r.maxTravel);
}

bool validArenaSpecialGauge(const ArenaSpecialGaugeRules& g) {
    return spInRange(g.full, 1.f, 1.0e6f) && spInRange(g.perDamageDealt, 0.f, 1000.f) && spInRange(g.perParry, 0.f, 1.0e5f) &&
           spInRange(g.perBlock, 0.f, 1.0e5f) && spInRange(g.perDamageTaken, 0.f, 1000.f) &&
           spInRange(g.switchLoss, 0.f, 1.f) && spInRange(g.readyLineGap, 0.f, 3600.f);
}

const ArenaSpecialRules* ArenaSpecialTable::find(std::string_view id) const {
    for (std::size_t i = 0; i < count && i < moves.size(); ++i)
        if (moves[i].idView() == id) return &moves[i];
    return nullptr;
}

const ArenaSpecialRules* ArenaSpecialTable::forLead(std::string_view leadId) const {
    if (leadId.empty()) return nullptr;
    for (std::size_t i = 0; i < count && i < moves.size(); ++i)
        if (moves[i].leadView() == leadId) return &moves[i];
    return nullptr;
}

LeadSpecialGauges::Lead* LeadSpecialGauges::entry(std::string_view leadId) {
    if (leadId.empty() || leadId.size() > kMaxArenaSpecialId) return nullptr;
    for (std::size_t i = 0; i < count && i < leads.size(); ++i)
        if (std::string_view(leads[i].id.data()) == leadId) return &leads[i];
    if (count >= leads.size()) return nullptr;
    Lead& l = leads[count++];
    l = Lead{};
    spCopy(leadId, l.id);
    return &l;
}

const LeadSpecialGauges::Lead* LeadSpecialGauges::find(std::string_view leadId) const {
    if (leadId.empty()) return nullptr;
    for (std::size_t i = 0; i < count && i < leads.size(); ++i)
        if (std::string_view(leads[i].id.data()) == leadId) return &leads[i];
    return nullptr;
}

bool LeadSpecialGauges::carry(std::string_view from, std::string_view to) {
    if (from.empty() || to.empty() || from == to || from.size() > kMaxArenaSpecialId) return false;
    const Lead* had = find(from);
    const float value = had ? had->value : 0.f;
    Lead* dest = entry(to);
    if (!dest) return false;
    dest->value = value;
    if (Lead* src = entry(from)) src->value = 0.f;
    return true;
}

const char* arenaSpecialRefusalName(ArenaSpecialRefusal r) {
    switch (r) {
    case ArenaSpecialRefusal::Ok: return "ok";
    case ArenaSpecialRefusal::NoMove: return "no_move";
    case ArenaSpecialRefusal::NotControlled: return "not_controlled";
    case ArenaSpecialRefusal::NotReady: return "not_ready";
    case ArenaSpecialRefusal::NoTarget: return "no_target";
    case ArenaSpecialRefusal::NoSight: return "no_sight";
    case ArenaSpecialRefusal::Down: return "down";
    case ArenaSpecialRefusal::Running: return "running";
    case ArenaSpecialRefusal::Over: return "over";
    case ArenaSpecialRefusal::Unhurt: return "unhurt";
    }
    return "?";
}

bool importArenaSpecials(std::string_view json, ArenaSpecialTable& out, std::string* error,
                         std::vector<std::string>* warnings) {
    out = ArenaSpecialTable{};
    std::vector<std::string> errors;
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(json, &parseError)) {
        errors.push_back("arena special: " + parseError);
    } else if (!doc.root().isObject()) {
        errors.push_back("arena special: expected an object");
    } else {
        const JsonValue root = doc.root();
        if (const JsonValue v = root["version"]; !v.isAbsent() && !(v.isInteger() && v.integer() == 1))
            errors.push_back("version: expected 1");
        spUnknownKeys(root, "", {"version", "gauge", "moves"}, warnings);
        const JsonValue g = root["gauge"];
        if (g.isObject()) {
            spUnknownKeys(g, "gauge",
                          {"full", "perDamageDealt", "perParry", "perBlock", "perDamageTaken", "switchLoss",
                           "switchLossAppliesTo", "readyLineGap"},
                          warnings);
            spNumber(g, "full", "gauge", 1.f, 1.0e6f, out.gauge.full, errors);
            spNumber(g, "perDamageDealt", "gauge", 0.f, 1000.f, out.gauge.perDamageDealt, errors);
            spNumber(g, "perParry", "gauge", 0.f, 1.0e5f, out.gauge.perParry, errors);
            spNumber(g, "perBlock", "gauge", 0.f, 1.0e5f, out.gauge.perBlock, errors);
            spNumber(g, "perDamageTaken", "gauge", 0.f, 1000.f, out.gauge.perDamageTaken, errors);
            spNumber(g, "switchLoss", "gauge", 0.f, 1.f, out.gauge.switchLoss, errors);
            spNumber(g, "readyLineGap", "gauge", 0.f, 3600.f, out.gauge.readyLineGap, errors);
            if (const JsonValue a = g["switchLossAppliesTo"]; !a.isAbsent()) {
                const std::string_view w = a.isString() ? std::string_view(a.string()) : std::string_view{};
                if (w == "both")
                    out.gauge.switchLossAppliesTo = ArenaSpecialSwitchLoss::Both;
                else if (w == "outgoing")
                    out.gauge.switchLossAppliesTo = ArenaSpecialSwitchLoss::Outgoing;
                else if (w == "incoming")
                    out.gauge.switchLossAppliesTo = ArenaSpecialSwitchLoss::Incoming;
                else
                    errors.push_back("gauge.switchLossAppliesTo: expected both, outgoing or incoming");
            }
        } else if (!g.isAbsent()) {
            errors.push_back("gauge: expected an object");
        }
        const JsonValue moves = root["moves"];
        if (!moves.isArray() || moves.size() > kMaxArenaSpecials) {
            errors.push_back("moves: expected an array of 0.." + std::to_string(kMaxArenaSpecials) + " moves");
        } else {
            for (std::uint32_t i = 0; i < moves.size(); ++i) {
                const std::string path = "moves[" + std::to_string(i) + "]";
                ArenaSpecialRules r;
                spMove(moves[i], path, r, errors, warnings);
                if (!r.idView().empty() && out.find(r.idView())) errors.push_back(path + ".id: duplicate move id");
                if (!r.leadView().empty() && out.forLead(r.leadView()))
                    errors.push_back(path + ".lead: that lead already has a move");
                out.moves[out.count++] = r;
            }
        }
    }
    if (errors.empty()) return true;
    out = ArenaSpecialTable{};
    if (error) {
        error->clear();
        for (const std::string& e : errors) {
            if (!error->empty()) *error += '\n';
            *error += e;
        }
    }
    return false;
}

} // namespace ftcore
