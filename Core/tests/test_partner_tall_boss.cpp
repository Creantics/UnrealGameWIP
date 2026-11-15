// The partner against a tall boss, seen through the REAL lock camera: the host's tall-lock solve (FTArenaRunner's
// camera step: arenaFrameYawStep, arenaLowSide, arenaLowFit, arenaLockAimPitch, arenaLeadPitch, arenaPoseAtPitch)
// stepped here tick by tick and turned into the fight's partnerCamera with arenaPoseCamera, as the host feeds it.
// "In play" is measured as the host measures it ("Arena check: partner in play"): partnerInFrame at the brain's
// margin, and hidden only when the lines from the lens to its middle and to its head both pass through the target's
// cylinder (arenaHiddenBehind).
#include "ftcore/ArenaCamera.h"
#include "ftcore/ArenaFight.h"
#include "ftcore/ArenaLockFrame.h"
#include "ftcore/ArenaLook.h"
#include "ftcore/ArenaZone.h"
#include "ftcore/PartnerBrain.h"
#include "TestAssets.h"
#include <ostream>
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <span>
#include <sstream>
#include <vector>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 60.f;
constexpr float kAspect = 16.f / 9.f;
constexpr float kBossRadius = 2.5f;
constexpr float kBossHeight = 12.f;
constexpr float kAllyRadius = 0.4f;

float tbDist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

PartnerTuning tbTuning() {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/partner_brain.json", text)) FAIL("Data/Battle/partner_brain.json not found");
    PartnerTuning t;
    std::string err;
    REQUIRE_MESSAGE(importPartnerBrain(text, t, &err), err);
    return t;
}

ArenaAttack tbSwing(float startup, float active, float recovery, float range, int damage) {
    ArenaAttack a;
    a.startup = startup;
    a.active = active;
    a.recovery = recovery;
    a.range = range;
    a.arcCos = -1.f;
    a.damage = damage;
    a.poiseDamage = 0.f;
    a.guardDamage = 0.f;
    a.lunge = 0.f;
    a.tracking = 0.f;
    a.armor = 0.f;
    a.stamina = 0.f;
    a.cancel = 0.5f;
    return a;
}

// test_arena_partner.cpp's rule kit: lights of 10-12 hp, 0.6 s each.
ArenaFighter tbFighter() {
    ArenaFighter f;
    f.hp = 1000;
    f.stamina = 100.f;
    f.staminaRegen = 50.f;
    f.staminaDelay = 0.5f;
    f.poise = 30.f;
    f.poiseDelay = 1.f;
    f.hitstun = 0.4f;
    f.moveSpeed = 4.f;
    f.turnRate = 20.f;
    f.bodyRadius = kAllyRadius;
    f.bufferTime = 0.2f;
    f.lightCount = 3;
    f.light[0] = tbSwing(0.2f, 0.1f, 0.3f, 2.f, 10);
    f.light[1] = tbSwing(0.2f, 0.1f, 0.3f, 2.f, 11);
    f.light[2] = tbSwing(0.2f, 0.1f, 0.3f, 2.f, 12);
    f.chainGap = 0.3f;
    f.heavy = tbSwing(0.4f, 0.2f, 0.5f, 2.f, 30);
    f.riposte = tbSwing(0.1f, 0.1f, 0.4f, 2.5f, 50);
    f.release = tbSwing(0.1f, 0.1f, 0.4f, 2.5f, 20);
    f.dodgeTime = 0.6f;
    f.dodgeDistance = 3.f;
    f.dodgeInvulnStart = 0.1f;
    f.dodgeInvulnEnd = 0.4f;
    f.dodgeStamina = 20.f;
    f.dodgeCancel = 0.75f;
    f.guardArcCos = 0.f;
    f.guardAbsorb = 0.75f;
    f.guardMoveScale = 0.5f;
    f.guardBreakTime = 1.f;
    f.parryWindow = 0.2f;
    f.parryRearm = 0.5f;
    f.riposteReach = 2.2f;
    f.healCharges = 2;
    f.healAmount = 40;
    f.healTime = 0.5f;
    f.healRecovery = 0.3f;
    f.healMoveScale = 0.35f;
    f.lockRange = 30.f;
    f.lockBreak = 40.f;
    return f;
}

// A tall boss's body: the shipped test's 2.5 m round and 12 m high, or another.
struct TallShape {
    float radius = kBossRadius, height = kBossHeight;
};

// The tall boss: 2.5 m round, 12 m high (or `shape`), plain (not skinned: no hover, scale 1), a harmless bite about every 2 s.
ArenaEnemyKind tallBoss(TallShape shape = {}) {
    ArenaEnemyKind k;
    k.hp = 1000000;
    k.poise = 1000.f;
    k.poiseDelay = 1.f;
    k.staggerTime = 1.f;
    k.openTime = 1.5f;
    k.downTime = 1.f;
    k.attackDelay = 0.f;
    k.bodyRadius = shape.radius;
    k.height = shape.height;
    k.boss = true;
    k.breakMove = -1;
    k.runRange = 40.f;
    k.moveCount = 1;
    ArenaEnemyMove& m = k.moves[0];
    m.attack = tbSwing(0.6f, 0.15f, 0.6f, 3.f, 0);
    m.minRange = 0.f;
    m.maxRange = 4.f;
    m.cooldown = 2.f;
    m.weight = 1.f;
    m.chain = -1;
    m.unblockable = false;
    m.parryable = true;
    return k;
}

// The host's tall-lock camera, one step per fight tick (FTArenaRunner's camera step, "the lock on a tall target"):
// the zone's default rig (arenaZoneCamera gives ZoneCamera{}), no wall (share 1), not aiming, framing on, a 16:9
// frame, the lead's default body. Left out: the partner's soft yaw (arenaPartnerYaw, at most 5 degrees toward the
// partner), so the test is the stricter for it.
struct TallCamera {
    ZoneCamera zone{};
    AimStance aim{};
    float yaw = 0.f, turn = 0.f, pitch = -16.f;
    float low = 0.f, lowSide = 1.f, lowSideNow = kArenaLow.side, lowFit = -1.f;
    ArenaCameraPose lens{};
    bool tall = false;

    static float interp(float now, float want, float dt, float rate) { // FMath::FInterpTo
        if (rate <= 0.f) return want;
        const float d = want - now;
        if (d * d < 1e-8f) return want;
        return now + d * std::clamp(dt * rate, 0.f, 1.f);
    }
    static float interpConst(float now, float want, float dt, float rate) { // FMath::FInterpConstantTo
        const float d = want - now, step = rate * dt;
        return std::fabs(d) <= step ? want : now + (d > 0.f ? step : -step);
    }
    void begin(Vec2 lead, Vec2 boss) {
        const Vec2 to{boss.x - lead.x, boss.y - lead.y};
        yaw = std::atan2(to.y, to.x) / 3.14159265f * 180.f;
        lowSide = zone.side < 0.f ? -1.f : 1.f;
    }
    // One step over the lead at `lead`, the followed target at `boss` with its body `height` m tall.
    PartnerCameraView step(Vec2 lead, Vec2 boss, float height, float dt) {
        ArenaFrameRules rules = kArenaFrame;
        rules.maxTurn = zone.frameTurnCapDeg;
        ArenaFrameScene scene;
        scene.framing = true;
        scene.aiming = false;
        scene.side = zone.side;
        scene.lineRate = 7.f; // the host's kArenaLockTurn
        const Vec2 to{boss.x - lead.x, boss.y - lead.y};
        const float far = std::hypot(to.x, to.y);
        scene.followed = true;
        scene.hasLine = far > 0.3f;
        scene.lineWant = std::atan2(to.y, to.x) / 3.14159265f * 180.f;
        scene.distance = far;
        const ArenaFrameYaw y = arenaFrameYawStep(rules, ArenaFrameYaw{yaw, turn}, scene, dt);
        yaw = y.line;
        turn = y.turn;
        const ArenaLeadBody body{};
        const ArenaCameraPose rig = arenaCameraPose(zone, aim, ArenaFrameYaw{yaw, turn}, 0.f);
        const ArenaLowRules sideRules = kArenaLow;
        tall = height >= kArenaLookTallBoss;
        float want = arenaFrameLockPitch(kArenaLook, zone.pivot, zone.arm, zone.up, zone.lockPitch, far, turn, height);
        if (tall) {
            const float aimHeight = arenaLockAimHeight(0.f, height, -1.f, -1.f); // the chest: no telegraph or weak point read
            const ArenaPoint3 at{to.x, to.y, aimHeight};
            lowSide = arenaLowSide(kArenaLook, rig, pitch, kAspect, body, at, lowSide, kArenaLowSide, sideRules);
            if (lowFit < 0.f) lowFit = arenaLowFit(kArenaLook, kArenaLeadFrame, rig, kAspect, body, sideRules);
            ArenaLowRules wantRules = sideRules;
            wantRules.side = lowSideNow;
            want = arenaLockAimPitch(kArenaLook, rig, kAspect, at, zone.lockPitch, low, wantRules);
        }
        pitch = interp(pitch, want, dt, 4.f);
        yaw = std::remainder(yaw, 360.f);
        pitch = arenaClampViewPitch(kArenaLook, pitch);
        const float fit = lowFit < 0.f ? 0.f : lowFit;
        low = std::min(low, fit);
        low = interpConst(low, tall ? fit : 0.f, dt, kArenaLow.rate);
        lowSideNow = low > 0.f ? interpConst(lowSideNow, lowSide * kArenaLow.side, dt, 2.4f) : lowSide * kArenaLow.side;
        ArenaLowRules lowRules = sideRules;
        lowRules.side = lowSideNow;
        pitch = arenaLeadPitch(kArenaLook, kArenaLeadFrame, rig, kAspect, body, pitch, low, lowRules).pitch;
        lens = arenaPoseAtPitch(kArenaLook, rig, pitch, low, lowRules);
        return arenaPoseCamera(lens, lead, kAspect, 1.8f);
    }
};

// The partner hidden behind the boss from the lens, as the host counts it: its middle and its head both behind.
bool tbHidden(const PartnerCameraView& cam, Vec2 ally, Vec2 boss, TallShape shape = {}) {
    const ArenaPoint3 eye{cam.pos.x, cam.pos.y, cam.pos.z};
    const auto behind = [&](float z) {
        return arenaHiddenBehind(eye, ArenaPoint3{ally.x, ally.y, cam.groundZ + z}, boss, shape.radius, cam.groundZ,
                                 cam.groundZ + shape.height);
    };
    return behind(cam.bodyHeight * 0.55f) && behind(cam.bodyHeight * 0.95f);
}

struct TallRun {
    float seconds = 0.f;     // partner on and up
    float clear = 0.f;       // on screen and not hidden
    float seen = 0.f;        // on screen
    float hidden = 0.f;      // on screen but hidden
    float longestOut = 0.f;  // the longest run not clear (s)
    int outRuns = 0;         // runs not clear
    int blows = 0;           // blows begun
    int seenBlows = 0;       // ... from where the camera sees it, unhidden
    int insideSteps = 0;     // walk steps that would end inside the boss's body (before the push-out)
    int goalInside = 0;      // steps whose goal stands inside the boss's body
    float outMoving = 0.f;   // not clear while walking (MoveTo or StepBack)
    float outAttacking = 0.f; // not clear while in a string
    float lowMax = 0.f;      // the low share reached
    bool tallLock = false;
    int switched = -1;       // the switch run: the tick of the switch
    float reachSeen = -1.f;  // ... seconds from it until the new partner stood clear for 0.5 s on end (the run's start)
    std::vector<float> fromLead; // the partner's distance to the lead, each step it is on and up
    float leadP50() const {
        if (fromLead.empty()) return 0.f;
        std::vector<float> v = fromLead;
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
        return v[v.size() / 2];
    }
    float leadMax() const { return fromLead.empty() ? 0.f : *std::max_element(fromLead.begin(), fromLead.end()); }
    float meanOut() const { return outRuns > 0 ? (seconds - clear) / static_cast<float>(outRuns) : 0.f; }
};

// A 20 s fight: the boss at the origin, the lead at its foot (3.4 m out) on autopilot (it locks and strikes), the
// partner 2 m behind the lead to its side. The ally body walks to its goal at the lead's speed and is pushed out of the
// boss's body. switchAt >= 0: the host's switchLeads at that time (s), the bodies swapped as the host does; the run
// measures until 3 s after it.
TallRun tallRun(PartnerOrder order, float switchAt = -1.f, float seconds = 20.f, const PartnerTuning* tune = nullptr,
                TallShape shape = {}) {
    ArenaDef def;
    def.fighter = tbFighter();
    def.enemyCount = 1;
    def.attackers = 1;
    def.enemies[0] = tallBoss(shape);
    const ArenaRing ring{Vec2{0.f, 0.f}, 30.f};
    ArenaBodies bodies;
    bodies.enemies[0] = Vec2{0.f, 0.f};
    bodies.player = Vec2{-(shape.radius + 0.9f), 0.f};
    bodies.ally = Vec2{bodies.player.x - 2.f, -1.5f};
    PartnerBrain brain;
    const PartnerTuning tuning = tune ? *tune : tbTuning();
    ArenaFight fight;
    fight.setAlly(def.fighter.hp, kAllyRadius);
    fight.setPartner(&brain, &tuning);
    fight.begin(def, ring, bodies.player, Vec2{1.f, 0.f}, 7);
    TallCamera cam;
    cam.begin(bodies.player, bodies.enemies[0]);
    TallRun r;
    float outFor = 0.f, clearFor = 0.f;
    const int n = static_cast<int>(std::lround(seconds / kDt));
    for (int i = 0; i < n && !fight.over(); ++i) {
        if (switchAt >= 0.f && r.switched < 0 && fight.time() >= switchAt) {
            if (fight.switchLeads()) {
                std::swap(bodies.player, bodies.ally);
                r.switched = i;
                r.seconds = r.clear = r.seen = r.hidden = r.longestOut = r.outMoving = r.outAttacking = 0.f;
                r.outRuns = r.blows = r.seenBlows = r.insideSteps = r.goalInside = 0;
                r.fromLead.clear();
                outFor = clearFor = 0.f;
            }
        }
        ArenaInput in = fight.autopilot();
        in.partnerOrder = order == PartnerOrder::Spread ? 1 : 0;
        const PartnerCameraView view = cam.step(bodies.player, bodies.enemies[0], shape.height, kDt);
        const ArenaLensAt lensAt = arenaCameraLens(cam.lens);
        in.cameraOn = true;
        in.cameraPos = Vec2{bodies.player.x + lensAt.x, bodies.player.y + lensAt.y};
        in.partnerCamera = view;
        r.lowMax = std::max(r.lowMax, cam.low);
        r.tallLock = r.tallLock || cam.tall;
        const ArenaOutput& out = fight.tick(kDt, in, bodies);
        for (const ArenaEvent& e : out.events)
            if (e.type == ArenaEventType::TookOver) std::swap(bodies.player, bodies.ally);
        arenaStep(fight, bodies, kDt);
        if (out.ally.partnerOn && fight.allyHp() > 0) {
            const bool seen = partnerInFrame(view, kPartnerFrustumMargin, bodies.ally);
            const bool clear = seen && !tbHidden(view, bodies.ally, bodies.enemies[0], shape);
            r.seconds += kDt;
            r.fromLead.push_back(tbDist(bodies.ally, bodies.player));
            r.seen += seen ? kDt : 0.f;
            r.clear += clear ? kDt : 0.f;
            r.hidden += seen && !clear ? kDt : 0.f;
            const PartnerIntent& pi = brain.intent();
            const bool walking = pi.act == PartnerAct::MoveTo || pi.act == PartnerAct::StepBack;
            if (!clear) {
                r.outMoving += walking ? kDt : 0.f;
                r.outAttacking += out.ally.attacking ? kDt : 0.f;
                if (outFor <= 0.f) ++r.outRuns;
                outFor += kDt;
                r.longestOut = std::max(r.longestOut, outFor);
            } else {
                outFor = 0.f;
            }
            clearFor = clear ? clearFor + kDt : 0.f;
            if (r.switched >= 0 && r.reachSeen < 0.f && clearFor >= 0.5f - 1e-4f)
                r.reachSeen = static_cast<float>(i - r.switched) * kDt - clearFor + kDt;
            if (pi.blowStart) {
                ++r.blows;
                r.seenBlows += clear ? 1 : 0;
            }
            if (tbDist(out.ally.goal, bodies.enemies[0]) < shape.radius + kAllyRadius - 1e-3f) ++r.goalInside;
            // The host's walk: toward the goal at the lead's speed, then out of the boss's body.
            const Vec2 d{out.ally.goal.x - bodies.ally.x, out.ally.goal.y - bodies.ally.y};
            const float far = std::hypot(d.x, d.y), most = def.fighter.moveSpeed * kDt;
            Vec2 next = far > most ? Vec2{bodies.ally.x + d.x * most / far, bodies.ally.y + d.y * most / far} : out.ally.goal;
            const Vec2 e{next.x - bodies.enemies[0].x, next.y - bodies.enemies[0].y};
            const float apart = std::hypot(e.x, e.y), least = shape.radius + kAllyRadius;
            if (apart < least - 1e-3f) {
                ++r.insideSteps;
                const Vec2 dir = apart > 1e-4f ? Vec2{e.x / apart, e.y / apart} : Vec2{1.f, 0.f};
                next = Vec2{bodies.enemies[0].x + dir.x * least, bodies.enemies[0].y + dir.y * least};
            }
            bodies.ally = fight.ring().clamp(next, kAllyRadius);
        }
        if (r.switched >= 0 && static_cast<float>(i - r.switched) * kDt >= 3.5f) break;
    }
    return r;
}

void tbReport(const char* name, const TallRun& r) {
    MESSAGE(std::string(name) << ": " << r.seconds << " s on; clear " << (r.seconds > 0.f ? r.clear / r.seconds : 0.f) << " (seen "
                 << (r.seconds > 0.f ? r.seen / r.seconds : 0.f) << ", hidden " << r.hidden << " s); out runs " << r.outRuns
                 << ", longest " << r.longestOut << " s, mean " << r.meanOut() << " s (walking " << r.outMoving << ", in strings "
                 << r.outAttacking << "); blows " << r.blows << ", from clear "
                 << r.seenBlows << "; inside steps " << r.insideSteps << ", goals inside " << r.goalInside << "; low "
                 << r.lowMax << "; from the lead p50 " << r.leadP50() << " m, max " << r.leadMax() << " m"
                 << (r.switched >= 0 ? "; switch: clear after " + std::to_string(r.reachSeen) + " s" : std::string()));
}

} // namespace

TEST_CASE("partner tall boss: the camera harness is the host's tall lock (the low lens comes on)") {
    TallCamera cam;
    cam.begin(Vec2{-3.4f, 0.f}, Vec2{0.f, 0.f});
    PartnerCameraView v{};
    for (int i = 0; i < 180; ++i) v = cam.step(Vec2{-3.4f, 0.f}, Vec2{0.f, 0.f}, kBossHeight, kDt);
    CHECK(cam.tall);
    CHECK(cam.lowFit > 0.5f);
    CHECK(cam.low == doctest::Approx(cam.lowFit));
    REQUIRE(partnerCameraWhole(v));
    CHECK(v.pos.z < 1.5f);                 // the low lens: under the zone's pivot height
    CHECK(v.forward.z > 0.2f);             // looking up at the chest
    const ArenaLeadShares lead = arenaLeadShares(cam.lens, kAspect, ArenaLeadBody{});
    CHECK(arenaLeadInFrame(kArenaLeadFrame, lead));
    // The lead's feet on the settled low lens, as a share of the frame's height over its bottom edge, against the
    // partner's bottom margin: a partner standing at the lead's depth must fit under it (kPartnerFrustumMarginBottom).
    MESSAGE("low lens: " << v.pos.z << " m up, horizontal fov " << cam.lens.fov << ", the lead's feet at " << lead.feet
                         << " of the frame's height");
    CHECK(lead.feet > kPartnerFrustumMarginBottom);
}

// Two bodies: the shipped test's (2.5 m round, 12 m high) and a narrower, shorter one (1.5 m round, 6 m high, still
// over kPartnerTallMeters). Rule 18 says never hidden; clear (on screen and not hidden) at least 0.9 of the run.
TEST_CASE("partner tall boss: in play through the real camera, 20 s, StayClose and Spread, two boss shapes") {
    for (const TallShape shape : {TallShape{}, TallShape{1.5f, 6.f}})
    for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread}) {
        CAPTURE(shape.radius);
        const TallRun r = tallRun(order, -1.f, 20.f, nullptr, shape);
        tbReport(order == PartnerOrder::StayClose ? "StayClose" : "Spread", r);
        CHECK(r.tallLock);
        CHECK(r.lowMax > 0.5f);
        CHECK(r.seconds > 19.9f);
        CHECK(r.clear >= 0.9f * r.seconds); // on screen and not hidden behind the boss
        CHECK(r.seenBlows >= 4);            // blows begun where the camera sees it
        CHECK(r.insideSteps == 0);          // never a step into the boss's body
        CHECK(r.goalInside == 0);           // nor a goal in it
    }
}

TEST_CASE("partner tall boss: after a lead switch, the new partner reaches a seen station within 3 s") {
    for (const TallShape shape : {TallShape{}, TallShape{1.5f, 6.f}})
    for (const float at : {6.f, 13.5f})
    for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread}) {
        CAPTURE(shape.radius);
        CAPTURE(at);
        const TallRun r = tallRun(order, at, 20.f, nullptr, shape);
        tbReport(order == PartnerOrder::StayClose ? "StayClose switch" : "Spread switch", r);
        REQUIRE(r.switched >= 0);
        CHECK(r.reachSeen >= 0.f);
        CHECK(r.reachSeen <= 3.f);
        CHECK(r.insideSteps == 0);
        CHECK(r.goalInside == 0);
    }
}

// Why StayClose's cap grows on a tall target: through the settled low lens, with the lead at the boss's foot, the
// grown cap (closeLeadMeters plus the radius, at most tallGrowMaxMeters, under the grown leash less 0.5) holds more of
// the station cells the camera sees unhidden than the old cap does. Measured (seen, within the old cap, within the
// grown cap): 30, 0, 21 at a bottom margin of 0.10; 89, 41, 80 at the bottom's own 0.05 (kPartnerFrustumMarginBottom)
// (96, 48, 87 at 0.04).
TEST_CASE("partner tall boss: the grown cap holds more of the seen station cells than StayClose's old cap") {
    TallCamera cam;
    const Vec2 lead{-(kBossRadius + 0.9f), 0.f}, boss{0.f, 0.f};
    cam.begin(lead, boss);
    PartnerCameraView v{};
    for (int i = 0; i < 300; ++i) v = cam.step(lead, boss, kBossHeight, kDt);
    const PartnerTuning t = tbTuning();
    int old = 0, grown = 0, all = 0;
    for (const float rad : {kBossRadius + 0.5f * t.reachMeters, kBossRadius + t.reachMeters - t.stationTolerance})
        for (int a = 0; a < 72; ++a) {
            const float ang = 5.f * static_cast<float>(a) * 3.14159265f / 180.f;
            const Vec2 q{boss.x + rad * std::cos(ang), boss.y + rad * std::sin(ang)};
            if (!partnerInFrame(v, t.frustumMargin, q) || tbHidden(v, q, boss)) continue;
            ++all;
            const float dl = tbDist(q, lead);
            const float g = std::min(kBossRadius, t.tallGrowMaxMeters);
            old += dl <= std::min(t.closeLeadMeters, t.leashMeters - 0.5f) ? 1 : 0;
            grown += dl <= std::min(t.closeLeadMeters + g, t.leashMeters + g - 0.5f) ? 1 : 0;
        }
    MESSAGE("seen station cells: " << all << ", within the old cap " << old << ", within the grown cap " << grown);
    CHECK(all >= 30);
    CHECK(grown > old);
}

// Rule 18's growth is the tall target's radius and no more, clamped to tallGrowMaxMeters (6.0 shipped, over this
// boss's 2.5 m radius, so it does not bind here). Clamped to 1.0 m, its leash (4 m) and return grow by 1 m, not 2.5:
// it keeps within that grown leash and nearer the lead than shipped (measured max 3.55 m against 4.42 m).
TEST_CASE("partner tall boss: StayClose's growth is clamped to tallGrowMaxMeters") {
    PartnerTuning t = tbTuning();
    CHECK(t.tallGrowMaxMeters == 6.f);
    const TallRun shipped = tallRun(PartnerOrder::StayClose);
    t.tallGrowMaxMeters = 1.f;
    const TallRun clamped = tallRun(PartnerOrder::StayClose, -1.f, 20.f, &t);
    tbReport("StayClose, growth clamped to 1 m", clamped);
    CHECK(shipped.leadMax() > t.leashMeters);                // the grown leash used
    CHECK(clamped.leadMax() <= t.leashMeters + 1.f + 0.5f); // a little over the grown leash: the step it turns back
    CHECK(clamped.leadMax() < shipped.leadMax());
    CHECK(clamped.leadP50() <= shipped.leadP50() + 1e-3f);
}

namespace {

// A brain-level scene: the lead at `lead` facing +x, a camera `back` m behind it and 1.7 m up looking along +x
// (pitched `pitch` degrees up), a `vfov` degree lens at 16:9; one boss at the origin.
struct TbScene {
    PartnerBrain b;
    PartnerInput in;
    std::array<PartnerEnemyView, 1> e{};
    PartnerCameraView v{};
    TbScene(const PartnerTuning& t, PartnerOrder order, Vec2 lead, float radius, float height, float back = 4.f,
            float pitch = 0.f, float vfov = 50.f) {
        REQUIRE(b.begin(t, 1));
        e[0].pos = {0.f, 0.f};
        e[0].radius = radius;
        e[0].kind = PartnerEnemyView::Kind::Boss;
        e[0].base = 0.f;
        e[0].top = height;
        in.order = order;
        in.leadPos = lead;
        in.leadTarget = 0;
        in.pos = {lead.x, lead.y + 2.f};
        in.station.on = true;
        in.station.cameraPos = {lead.x - back, lead.y};
        in.station.cameraForward = {1.f, 0.f};
        in.station.leadFacing = {1.f, 0.f};
        const float p = pitch * 3.14159265f / 180.f;
        v.given = true;
        v.pos = {lead.x - back, lead.y, 1.7f};
        v.forward = {std::cos(p), 0.f, std::sin(p)};
        v.up = {-std::sin(p), 0.f, std::cos(p)};
        v.aspect = kAspect;
        v.verticalFovDegrees = vfov;
        in.station.camera = v;
        in.enemies = std::span<const PartnerEnemyView>(e.data(), e.size());
    }
    // One step; the partner walks toward the intent's point at 4 m/s.
    const PartnerIntent& step() {
        const PartnerIntent& o = b.step(kDt, in);
        if (o.act == PartnerAct::MoveTo || o.act == PartnerAct::StepBack) {
            const Vec2 d{o.point.x - in.pos.x, o.point.y - in.pos.y};
            const float l = std::hypot(d.x, d.y), s = std::min(l, 4.f * kDt);
            if (l > 1e-4f) in.pos = {in.pos.x + d.x / l * s, in.pos.y + d.y / l * s};
        }
        return o;
    }
};

} // namespace

// Rule 18's fallback with no valid station: the camera 4 m behind the lead, a 40 degree lens and a wide tall boss
// (4 m round, 12 m high) 1.2 m past the lead filling the frame, so every station ring point is out of the frame, hidden
// behind the body, in the lead's wedge or in the target's keep-out (the only seen, unhidden ones: its near flanks, by
// the keep-out's margin). Rule 13 falls back; the fallback is a ring point the camera sees unhidden, not rules 10-11's
// station, which may stand behind the body; and it strikes from there.
TEST_CASE("partner tall boss: no valid station (the boss fills the frame): the fallback is a spot the camera sees unhidden") {
    const PartnerTuning t = tbTuning();
    for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread}) {
        CAPTURE(int(order));
        TbScene s(t, order, Vec2{-5.2f, 0.f}, 4.f, 12.f, 4.f, -10.f, 40.f);
        s.in.pos = {3.f, 4.5f}; // behind the boss's flank, out of the frame
        int fallback = 0, valid = 0, hidden = 0, strikes = 0;
        for (int i = 0; i < 300; ++i) {
            const PartnerIntent& o = s.step();
            fallback += o.frameFallback ? 1 : 0;
            valid += o.framed && !o.frameFallback ? 1 : 0;
            if (i >= 240) hidden += tbHidden(s.v, s.in.pos, s.e[0].pos, TallShape{4.f, 12.f}) ? 1 : 0;
            strikes += o.act == PartnerAct::Attack ? 1 : 0;
        }
        MESSAGE("fallback steps " << fallback << ", framed steps on a station " << valid << ", hidden in the last second "
                                  << hidden << ", strike steps " << strikes << ", at " << s.in.pos.x << ", " << s.in.pos.y);
        CHECK(fallback > 200);  // the station set is empty (rule 13's fallback holds)
        CHECK(hidden == 0);     // standing where the camera sees it unhidden
        CHECK(partnerInFrame(s.v, t.frustumMargin, s.in.pos));
        CHECK(strikes > 0);     // and it fights from there
    }
}

// Rule 18's fallback, a string from where the camera does not see it unhidden. The boss fills the frame as above and
// the lead is 0.6 m from it: every seen, unhidden ring point is in the lead's wedge, so the fallback's best is an
// unhidden spot out of the frame. A string starts there or at any other unhidden spot, never from a hidden one: walking,
// it strikes from the unhidden spot; held where it stands (a hidden spot in reach, its host not moving it), rule 14's
// timers run out and it still starts none there.
TEST_CASE("partner tall boss: fallen back with an unhidden spot only out of the frame: no string from a hidden spot") {
    const PartnerTuning t = tbTuning();
    for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread})
        for (const bool walks : {true, false}) {
            CAPTURE(int(order));
            CAPTURE(walks);
            TbScene s(t, order, Vec2{-4.6f, 0.f}, 4.f, 12.f, 4.f, -10.f, 40.f);
            s.in.pos = {1.f, 4.6f}; // behind the boss's flank, hidden, in reach, within the leash
            REQUIRE(tbHidden(s.v, s.in.pos, s.e[0].pos, TallShape{4.f, 12.f}));
            int fallback = 0, strikes = 0, hiddenStrikes = 0;
            for (int i = 0; i < 300; ++i) {
                const PartnerIntent& o = walks ? s.step() : s.b.step(kDt, s.in);
                fallback += o.frameFallback ? 1 : 0;
                if (o.act == PartnerAct::Attack) {
                    ++strikes;
                    hiddenStrikes += tbHidden(s.v, s.in.pos, s.e[0].pos, TallShape{4.f, 12.f}) ? 1 : 0;
                }
            }
            MESSAGE("fallback steps " << fallback << ", strike steps " << strikes << " (hidden " << hiddenStrikes
                                      << "), at " << s.in.pos.x << ", " << s.in.pos.y);
            CHECK(fallback > 200);
            CHECK(hiddenStrikes == 0);
            if (walks) CHECK(strikes > 0); // never idle where it can walk to the unhidden spot
        }
}

// And with no unhidden spot at all: the camera 0.5 m behind the lead and the arena a half-plane on the boss's far side
// (x >= -0.2) leave only ring points behind its body. Fallen back, it strikes from a hidden spot: never idle for it.
TEST_CASE("partner tall boss: fallen back with every legal ring point hidden: it still strikes") {
    const PartnerTuning t = tbTuning();
    for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread}) {
        CAPTURE(int(order));
        TbScene s(t, order, Vec2{-5.2f, 0.f}, 4.f, 12.f, 0.5f, -10.f, 40.f);
        s.in.station.arenaCentre = {100.f, 0.f};
        s.in.station.arenaRadius = 100.2f;
        s.in.pos = {3.f, 4.5f};
        int fallback = 0, strikes = 0, hiddenStrikes = 0;
        for (int i = 0; i < 300; ++i) {
            const PartnerIntent& o = s.step();
            fallback += o.frameFallback ? 1 : 0;
            if (o.act == PartnerAct::Attack) {
                ++strikes;
                hiddenStrikes += tbHidden(s.v, s.in.pos, s.e[0].pos, TallShape{4.f, 12.f}) ? 1 : 0;
            }
        }
        MESSAGE("fallback steps " << fallback << ", strike steps " << strikes << " (hidden " << hiddenStrikes << "), at "
                                  << s.in.pos.x << ", " << s.in.pos.y);
        CHECK(fallback > 200);
        CHECK(hiddenStrikes > 0); // from behind the body: no unhidden spot is legal
        CHECK(strikes > 0);       // never idle
    }
}

// Rule 18's growth (StayClose): it starts when the tall target's edge is within closeLeadMeters of the lead and ends
// only kPartnerTallGrowHysteresis past it; never on a target under kPartnerTallMeters, nor for Spread.
TEST_CASE("partner tall boss: StayClose's growth starts at closeLeadMeters and ends 0.5 m past it; none by a short target or for Spread") {
    const PartnerTuning t = tbTuning();
    const float r = 2.5f;
    const auto leadAt = [&](float edge) { return Vec2{-(r + edge), 0.f}; };
    {
        TbScene s(t, PartnerOrder::StayClose, leadAt(t.closeLeadMeters + 0.1f), r, 12.f);
        s.step();
        CHECK(s.b.save().tallGrowOn == 0); // past the bar: off
        s.in.leadPos = leadAt(t.closeLeadMeters - 0.1f);
        s.step();
        CHECK(s.b.save().tallGrowOn == 1); // within it: on
        s.in.leadPos = leadAt(t.closeLeadMeters + 0.3f);
        s.step();
        CHECK(s.b.save().tallGrowOn == 1); // within the hysteresis: still on
        s.in.leadPos = leadAt(t.closeLeadMeters + kPartnerTallGrowHysteresis + 0.1f);
        s.step();
        CHECK(s.b.save().tallGrowOn == 0); // past it: off
        s.in.leadPos = leadAt(t.closeLeadMeters + 0.3f);
        s.step();
        CHECK(s.b.save().tallGrowOn == 0); // off, the plain bar again
    }
    {
        TbScene s(t, PartnerOrder::StayClose, leadAt(0.5f), r, kPartnerTallMeters - 0.5f); // not tall
        for (int i = 0; i < 30; ++i) s.step();
        CHECK(s.b.save().tallGrowOn == 0);
    }
    {
        TbScene s(t, PartnerOrder::Spread, leadAt(0.5f), r, 12.f);
        for (int i = 0; i < 30; ++i) s.step();
        CHECK(s.b.save().tallGrowOn == 0);
    }
}

