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
#include <cmath>
#include <string>
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

// The tall boss: 2.5 m round, 12 m high, plain (not skinned: no hover, scale 1), a harmless bite about every 2 s.
ArenaEnemyKind tallBoss() {
    ArenaEnemyKind k;
    k.hp = 1000000;
    k.poise = 1000.f;
    k.poiseDelay = 1.f;
    k.staggerTime = 1.f;
    k.openTime = 1.5f;
    k.downTime = 1.f;
    k.attackDelay = 0.f;
    k.bodyRadius = kBossRadius;
    k.height = kBossHeight;
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
bool tbHidden(const PartnerCameraView& cam, Vec2 ally, Vec2 boss) {
    const ArenaPoint3 eye{cam.pos.x, cam.pos.y, cam.pos.z};
    const auto behind = [&](float z) {
        return arenaHiddenBehind(eye, ArenaPoint3{ally.x, ally.y, cam.groundZ + z}, boss, kBossRadius, cam.groundZ,
                                 cam.groundZ + kBossHeight);
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
TallRun tallRun(PartnerOrder order, float switchAt = -1.f, float seconds = 20.f, const PartnerTuning* tune = nullptr) {
    ArenaDef def;
    def.fighter = tbFighter();
    def.enemyCount = 1;
    def.attackers = 1;
    def.enemies[0] = tallBoss();
    const ArenaRing ring{Vec2{0.f, 0.f}, 30.f};
    ArenaBodies bodies;
    bodies.enemies[0] = Vec2{0.f, 0.f};
    bodies.player = Vec2{-(kBossRadius + 0.9f), 0.f};
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
        const PartnerCameraView view = cam.step(bodies.player, bodies.enemies[0], kBossHeight, kDt);
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
            const bool clear = seen && !tbHidden(view, bodies.ally, bodies.enemies[0]);
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
            if (tbDist(out.ally.goal, bodies.enemies[0]) < kBossRadius + kAllyRadius - 1e-3f) ++r.goalInside;
            // The host's walk: toward the goal at the lead's speed, then out of the boss's body.
            const Vec2 d{out.ally.goal.x - bodies.ally.x, out.ally.goal.y - bodies.ally.y};
            const float far = std::hypot(d.x, d.y), most = def.fighter.moveSpeed * kDt;
            Vec2 next = far > most ? Vec2{bodies.ally.x + d.x * most / far, bodies.ally.y + d.y * most / far} : out.ally.goal;
            const Vec2 e{next.x - bodies.enemies[0].x, next.y - bodies.enemies[0].y};
            const float apart = std::hypot(e.x, e.y), least = kBossRadius + kAllyRadius;
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

TEST_CASE("partner tall boss: in play through the real camera, 20 s, StayClose and Spread") {
    for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread}) {
        const TallRun r = tallRun(order);
        tbReport(order == PartnerOrder::StayClose ? "StayClose" : "Spread", r);
        CHECK(r.tallLock);
        CHECK(r.lowMax > 0.5f);
        CHECK(r.seconds > 19.9f);
        CHECK(r.clear >= 0.7f * r.seconds); // on screen and not hidden behind the boss
        CHECK(r.seenBlows >= 4);            // blows begun where the camera sees it
        CHECK(r.insideSteps == 0);          // never a step into the boss's body
        CHECK(r.goalInside == 0);           // nor a goal in it
    }
}

TEST_CASE("partner tall boss: after a lead switch, the new partner reaches a seen station within 3 s") {
    for (const PartnerOrder order : {PartnerOrder::StayClose, PartnerOrder::Spread}) {
        const TallRun r = tallRun(order, 6.f, 20.f);
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
