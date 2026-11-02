// The Turns battle camera director (BattleCamera.h): which shot, who it is on, where the camera stands,
// how it blends, and that it never comes through a figure.
#include "AllocCounter.h"
#include "ftcore/Battle.h"
#include "ftcore/BattleCamera.h"
#include "ftcore/BattleDemo.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

using namespace ftcore;

namespace {

constexpr double kStep = 1.0 / 60.0;
constexpr double kPi = 3.14159265358979323846;

// The demo stage as the Unreal host builds it: two leads (190 tall) and `enemies` cubes, the first the big one.
std::vector<CameraFigure> stage(std::size_t party, std::size_t enemies) {
    std::vector<CameraFigure> figures;
    for (std::size_t i = 0; i < party; ++i) {
        CameraFigure f;
        f.pos = stageHome(true, i, enemies);
        f.party = true;
        figures.push_back(f);
    }
    for (std::size_t i = 0; i < enemies; ++i) {
        CameraFigure f;
        f.pos = stageHome(false, i, enemies);
        f.height = 100.0 * (i == 0 ? 1.5 : 1.1);
        f.radius = f.height * 0.55;
        f.party = false;
        figures.push_back(f);
    }
    return figures;
}

CameraCue choosing(int actor, int target) {
    CameraCue c;
    c.phase = Phase::AwaitingCommand;
    c.actor = actor;
    c.target = target;
    return c;
}

void run(BattleCamera& cam, const CameraCue& cue, const std::vector<CameraFigure>& figures, double seconds) {
    for (double t = 0.0; t < seconds; t += kStep) cam.update(kStep, cue, figures);
}

double dist2(const CamVec& a, const CamVec& b) { return std::hypot(a.x - b.x, a.y - b.y); }

double dist3(const CamVec& a, const CamVec& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }

// The horizontal angle (degrees) between where the camera looks and the direction to `p`.
double offAxis(const BattleCameraPose& pose, const CamVec& p) {
    const double vx = pose.look.x - pose.eye.x, vy = pose.look.y - pose.eye.y;
    const double px = p.x - pose.eye.x, py = p.y - pose.eye.y;
    const double c = (vx * px + vy * py) / (std::hypot(vx, vy) * std::hypot(px, py));
    return std::acos(std::clamp(c, -1.0, 1.0)) * 180.0 / kPi;
}

// Whether `p` is within the picture's height: the picture is 16:9 and `fov` is its horizontal angle.
bool inFrameVertically(const BattleCameraPose& pose, const CamVec& p) {
    const double halfV = std::atan(std::tan(pose.fov * 0.5 * kPi / 180.0) * 9.0 / 16.0);
    const double pitch = std::atan2(pose.look.z - pose.eye.z, std::hypot(pose.look.x - pose.eye.x, pose.look.y - pose.eye.y));
    const double to = std::atan2(p.z - pose.eye.z, std::hypot(p.x - pose.eye.x, p.y - pose.eye.y));
    return std::fabs(to - pitch) < halfV;
}

// The distance from `p` to the segment a-b on the ground plane.
double segmentDistance(const CamVec& a, const CamVec& b, const CamVec& p) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / (dx * dx + dy * dy), 0.0, 1.0);
    return std::hypot(p.x - (a.x + dx * t), p.y - (a.y + dy * t));
}

bool finite(const BattleCameraPose& p) {
    return std::isfinite(p.eye.x) && std::isfinite(p.eye.y) && std::isfinite(p.eye.z) && std::isfinite(p.look.x) &&
           std::isfinite(p.look.y) && std::isfinite(p.look.z) && std::isfinite(p.fov);
}

// Outside every figure's inflated shape, above the minimum height, and looking toward the enemy side.
void checkSafe(const BattleCamera& cam, const std::vector<CameraFigure>& figures) {
    const BattleCameraPose& p = cam.pose();
    const CameraTuning& t = cam.tuning();
    REQUIRE(finite(p));
    CHECK(p.eye.z >= t.minHeight - 1e-9);
    for (const CameraFigure& f : figures) {
        const double rxy = (f.alive ? f.radius : std::max(f.radius, f.height)) + t.minClear;
        const double rz = (f.alive ? f.height : f.radius * 2.0) + t.minAbove;
        const double dx = p.eye.x - f.pos.x, dy = p.eye.y - f.pos.y, dz = p.eye.z - f.pos.z;
        const double inflated = (dx * dx + dy * dy) / (rxy * rxy) + dz * dz / (rz * rz);
        CHECK(inflated >= 1.0 - 1e-9);
        // The plain version: never inside the body.
        CHECK(!(std::hypot(dx, dy) < f.radius && p.eye.z < f.pos.z + f.height));
    }
    const CamVec fwd = cam.forward();
    CHECK((p.look.x - p.eye.x) * fwd.x + (p.look.y - p.eye.y) * fwd.y > 0.0);
}

} // namespace

TEST_CASE("camera: the stage puts the party behind the enemies along +Y, first of each side on the left") {
    const auto figures = stage(2, 4);
    BattleCamera cam;
    cam.begin(figures);
    CHECK(cam.forward().y > 0.95);
    CHECK(cam.right().x < -0.95); // looking along +Y, screen-right is -X
    // From behind the party the first lead stands left of the second, and the rear lead is the first.
    CHECK(cam.lateralOf(figures[0].pos) < cam.lateralOf(figures[1].pos));
    CHECK(cam.lateralOf(figures[4].pos) < cam.lateralOf(figures[5].pos));
    CHECK(cam.depthOf(figures[0].pos) < cam.depthOf(figures[1].pos));
    CHECK(cam.depthOf(figures[2].pos) > cam.depthOf(figures[1].pos) + 500.0);
}

TEST_CASE("camera: choosing is over the shoulder of whoever acts, behind them, with the target past the shoulder") {
    for (std::size_t enemies = 1; enemies <= 5; ++enemies) {
        const auto figures = stage(2, enemies);
        for (int actor = 0; actor < 2; ++actor) {
            for (int target = 2; target < static_cast<int>(figures.size()); ++target) {
                CAPTURE(enemies);
                CAPTURE(actor);
                CAPTURE(target);
                BattleCamera cam;
                cam.begin(figures);
                run(cam, choosing(actor, target), figures, 2.0);
                const BattleCameraPose& p = cam.pose();
                const CameraFigure& a = figures[static_cast<std::size_t>(actor)];
                const CameraFigure& t = figures[static_cast<std::size_t>(target)];
                CHECK(cam.shot() == CameraShot::Choose);
                CHECK(cam.shotActor() == actor);
                CHECK(cam.blend() == doctest::Approx(1.0));
                // Close: behind the actor along the axis, a shoulder to one side, about head height.
                CHECK(cam.depthOf(p.eye) == doctest::Approx(cam.depthOf(a.pos) - cam.tuning().chooseBack));
                CHECK(std::fabs(cam.lateralOf(p.eye) - cam.lateralOf(a.pos)) == doctest::Approx(cam.tuning().chooseSide));
                CHECK(p.eye.z >= a.height * cam.tuning().chooseEye - 1e-9); // head height, or higher when an ally is in the line of sight
                CHECK(dist2(p.eye, a.pos) < 330.0);
                // On the target's side of the actor.
                const double across = cam.lateralOf(t.pos) - cam.lateralOf(a.pos);
                if (std::fabs(across) > cam.tuning().sideHysteresis) {
                    CHECK(((cam.lateralOf(p.eye) - cam.lateralOf(a.pos)) > 0.0) == (across > 0.0));
                }
                // Both in the frame, and the target not behind the actor's body.
                CHECK(offAxis(p, a.pos) < p.fov * 0.5);
                CHECK(offAxis(p, t.pos) < p.fov * 0.5 * 0.9);
                CHECK(inFrameVertically(p, {a.pos.x, a.pos.y, a.height})); // the actor's head
                CHECK(inFrameVertically(p, {t.pos.x, t.pos.y, 0.55 * t.height}));
                CHECK(segmentDistance(p.eye, t.pos, a.pos) > a.radius);
                // ...nor behind the other lead: the line of sight to the target's chest clears their body sideways or passes
                // over their head.
                for (int o = 0; o < 2; ++o) {
                    if (o == actor) continue;
                    const CameraFigure& other = figures[static_cast<std::size_t>(o)];
                    const double dx = t.pos.x - p.eye.x, dy = t.pos.y - p.eye.y;
                    const double s = std::clamp(((other.pos.x - p.eye.x) * dx + (other.pos.y - p.eye.y) * dy) / (dx * dx + dy * dy), 0.0, 1.0);
                    const double lineZ = p.eye.z + (0.55 * t.height - p.eye.z) * s;
                    CHECK((segmentDistance(p.eye, t.pos, other.pos) > other.radius || lineZ >= other.height));
                }
                checkSafe(cam, figures);
            }
        }
    }
}

TEST_CASE("camera: an ally on the line of sight is looked over, and the lift comes smoothly as the line nears them") {
    // Devin (slot 0) choosing against the far enemy of a four-line: the second lead stands in the way.
    const auto figures = stage(2, 4);
    BattleCamera cam;
    cam.begin(figures);
    run(cam, choosing(0, 5), figures, 2.0);
    const double blocked = cam.pose().eye.z;
    CHECK(blocked > figures[1].height);
    // The same lead against the near enemy, which nothing hides, stays at head height.
    BattleCamera clear;
    clear.begin(figures);
    run(clear, choosing(0, 2), figures, 2.0);
    CHECK(clear.pose().eye.z == doctest::Approx(figures[0].height * clear.tuning().chooseEye));
    // Cycling through every enemy moves the height by bounded steps (no pop when the ally enters the line).
    double last = clear.pose().eye.z, worst = 0.0;
    for (int frame = 0; frame < 600; ++frame) {
        clear.update(kStep, choosing(0, 2 + (frame / 60) % 4), figures);
        worst = std::max(worst, std::fabs(clear.pose().eye.z - last));
        last = clear.pose().eye.z;
    }
    CHECK(worst < 15.0);
}

TEST_CASE("camera: a skill that hits them all frames the enemy line; aim zooms on its target") {
    const auto figures = stage(2, 3);
    BattleCamera all;
    all.begin(figures);
    run(all, choosing(0, -1), figures, 2.0);
    CHECK(all.shot() == CameraShot::Choose);
    // The look is among the enemies' depths and across them.
    CHECK(all.depthOf(all.pose().look) > all.depthOf(figures[0].pos) + 300.0);
    checkSafe(all, figures);

    BattleCamera aim;
    aim.begin(figures);
    run(aim, choosing(1, 3), figures, 2.0);
    const BattleCameraPose choose = aim.pose();
    CameraCue c = choosing(1, 3);
    c.phase = Phase::PartyAim;
    run(aim, c, figures, 1.0);
    CHECK(aim.shot() == CameraShot::Aim);
    CHECK(aim.shotActor() == 1);
    CHECK(aim.pose().fov < choose.fov - 10.0);
    // The target is the middle of the picture.
    CHECK(offAxis(aim.pose(), figures[3].pos) < 8.0);
    CHECK(inFrameVertically(aim.pose(), {figures[1].pos.x, figures[1].pos.y, figures[1].height}));

    // Aiming past the other lead: the camera rises over them, and the aiming lead's head stays in the picture.
    const auto four = stage(2, 4);
    BattleCamera past;
    past.begin(four);
    CameraCue shot = choosing(0, 5);
    shot.phase = Phase::PartyAim;
    run(past, shot, four, 2.0);
    CHECK(past.pose().eye.z > four[0].height * past.tuning().chooseEye + 20.0);
    CHECK(inFrameVertically(past.pose(), {four[0].pos.x, four[0].pos.y, four[0].height}));
    CHECK(inFrameVertically(past.pose(), {four[5].pos.x, four[5].pos.y, 0.55 * four[5].height}));
    checkSafe(past, four);
    CHECK(aim.depthOf(aim.pose().eye) == doctest::Approx(aim.depthOf(figures[1].pos) - aim.tuning().chooseBack));
    checkSafe(aim, figures);
}

TEST_CASE("camera: actions are seen from behind the party, low and over their heads, looking at the enemies") {
    for (std::size_t enemies = 1; enemies <= 5; ++enemies) {
        const auto figures = stage(2, enemies);
        for (const Phase phase : {Phase::PartySkill, Phase::EnemySkill}) {
            for (int defender = 0; defender < 2; ++defender) {
                for (int actor = 2; actor < static_cast<int>(figures.size()); ++actor) {
                    CAPTURE(enemies);
                    CAPTURE(defender);
                    CAPTURE(actor);
                    BattleCamera cam;
                    cam.begin(figures);
                    CameraCue c;
                    c.phase = phase;
                    c.actor = phase == Phase::EnemySkill ? actor : 0;
                    c.defender = defender;
                    c.target = phase == Phase::PartySkill ? actor : -1;
                    run(cam, c, figures, 3.0);
                    const BattleCameraPose& p = cam.pose();
                    CHECK(cam.shot() == CameraShot::Behind);
                    CHECK(cam.shotActor() == -1);
                    // Behind every party member by the shot's distance, above all their heads, nowhere near ground level.
                    for (int i = 0; i < 2; ++i) {
                        CHECK(cam.depthOf(p.eye) <= cam.depthOf(figures[static_cast<std::size_t>(i)].pos) - cam.tuning().behindBack + 1e-6);
                        CHECK(p.eye.z > figures[static_cast<std::size_t>(i)].height + 60.0);
                    }
                    // It sees the enemies, and both leads' backs.
                    for (std::size_t i = 0; i < figures.size(); ++i) {
                        CHECK(offAxis(p, figures[i].pos) < p.fov * 0.5);
                    }
                    // The drift toward the defender is sideways and bounded.
                    const double middle = (cam.lateralOf(figures[0].pos) + cam.lateralOf(figures[1].pos)) * 0.5;
                    const double drift = cam.lateralOf(p.eye) - middle;
                    if (phase == Phase::EnemySkill) {
                        CHECK(std::fabs(drift) <= cam.tuning().driftMax + 1e-6);
                        CHECK((drift > 0.0) == (cam.lateralOf(figures[static_cast<std::size_t>(defender)].pos) > middle));
                    } else {
                        CHECK(std::fabs(drift) < 1e-6);
                    }
                    checkSafe(cam, figures);
                }
            }
        }
    }
}

TEST_CASE("camera: the Intercept's cue sends the drift to the interceptor; downed leads do not hold the camera back") {
    auto figures = stage(2, 2);
    BattleCamera cam;
    cam.begin(figures);
    CameraCue c;
    c.phase = Phase::EnemySkill;
    c.actor = 2;
    c.defender = 1;
    run(cam, c, figures, 2.0);
    const double toOne = cam.lateralOf(cam.pose().eye);
    c.defender = 0;
    run(cam, c, figures, 2.0);
    const double toZero = cam.lateralOf(cam.pose().eye);
    CHECK(toOne > toZero + 50.0);

    // The rear lead is down: the camera now stands behind the one who is left.
    figures[0].alive = false;
    c.defender = 1;
    run(cam, c, figures, 2.0);
    CHECK(cam.depthOf(cam.pose().eye) <= cam.depthOf(figures[1].pos) - cam.tuning().behindBack + 1e-6);
    checkSafe(cam, figures);
}

TEST_CASE("camera: the outcome is a wide shot from behind and above the party") {
    for (const Phase outcome : {Phase::Victory, Phase::Defeat}) {
        auto figures = stage(2, 3);
        BattleCamera cam;
        cam.begin(figures);
        run(cam, choosing(0, 2), figures, 1.0);
        CameraCue c;
        c.phase = outcome;
        c.actor = 0;
        if (outcome == Phase::Victory) {
            for (std::size_t i = 2; i < figures.size(); ++i) figures[i].alive = false;
        } else {
            figures[0].alive = figures[1].alive = false;
        }
        run(cam, c, figures, 2.0);
        CHECK(cam.shot() == CameraShot::Wide);
        const BattleCameraPose& p = cam.pose();
        CHECK(p.eye.z > 350.0);
        CHECK(cam.depthOf(p.eye) < cam.depthOf(figures[0].pos) - 500.0);
        for (const CameraFigure& f : figures) CHECK(offAxis(p, f.pos) < p.fov * 0.5);
        checkSafe(cam, figures);
    }
}

TEST_CASE("camera: every blend takes 0.35 to 0.6 s and is eased, from where the camera is") {
    const CameraTuning t;
    for (const double b : {t.blendChoose, t.blendAim, t.blendBehind, t.blendWide}) {
        CHECK(b >= 0.35);
        CHECK(b <= 0.6);
    }
    const auto figures = stage(2, 3);
    BattleCamera cam;
    cam.begin(figures);
    run(cam, choosing(0, 2), figures, 2.0);
    const BattleCameraPose from = cam.pose();
    // The choice passes to the other lead.
    const CameraCue next = choosing(1, 2);
    double elapsed = 0.0;
    std::vector<double> steps;
    CamVec last = from.eye;
    cam.update(kStep, next, figures);
    elapsed += kStep;
    CHECK(cam.shotActor() == 1);
    CHECK(cam.blend() < 0.5);
    steps.push_back(dist3(last, cam.pose().eye));
    last = cam.pose().eye;
    while (cam.blend() < 1.0 && elapsed < 2.0) {
        cam.update(kStep, next, figures);
        elapsed += kStep;
        steps.push_back(dist3(last, cam.pose().eye));
        last = cam.pose().eye;
    }
    CHECK(elapsed >= 0.35);
    CHECK(elapsed <= 0.6 + kStep);
    // No snap: the first step is a small share of the move, and the fastest is a bounded multiple of the average.
    const double total = dist3(from.eye, cam.pose().eye);
    CHECK(steps.front() < total * 0.05);
    CHECK(steps.back() < total * 0.05);
    const double fastest = *std::max_element(steps.begin(), steps.end());
    CHECK(fastest < total / elapsed * kStep * 2.2);
    // Settled, it is the pose a fresh camera takes for the same cue.
    BattleCamera fresh;
    fresh.begin(figures);
    run(fresh, next, figures, 2.0);
    run(cam, next, figures, 1.0);
    CHECK(dist3(cam.pose().eye, fresh.pose().eye) < 1e-6);
    CHECK(dist3(cam.pose().look, fresh.pose().look) < 1e-6);
}

TEST_CASE("camera: cycling the target glides the framing, and a blend interrupted midway does not jump") {
    const auto figures = stage(2, 5);
    BattleCamera cam;
    cam.begin(figures);
    run(cam, choosing(0, 2), figures, 2.0);
    BattleCameraPose last = cam.pose();
    double worstEye = 0.0, worstLook = 0.0;
    // A new target every 0.1 s, back and forth across the whole row, then the other lead steps in.
    for (int frame = 0; frame < 240; ++frame) {
        const int phase = frame / 6;
        const int target = 2 + (phase % 10 < 5 ? phase % 10 : 9 - phase % 10);
        const int actor = frame < 120 ? 0 : 1;
        cam.update(kStep, choosing(actor, target), figures);
        worstEye = std::max(worstEye, dist3(last.eye, cam.pose().eye));
        worstLook = std::max(worstLook, dist3(last.look, cam.pose().look));
        last = cam.pose();
        checkSafe(cam, figures);
    }
    CHECK(worstEye < 30.0);  // 1800 cm/s at most
    CHECK(worstLook < 45.0);
    // An action starts while the lead switch is still blending: the move carries on from the current pose.
    BattleCamera cam2;
    cam2.begin(figures);
    run(cam2, choosing(0, 2), figures, 2.0);
    run(cam2, choosing(1, 2), figures, 0.2);
    const BattleCameraPose mid = cam2.pose();
    CameraCue c;
    c.phase = Phase::PartySkill;
    c.actor = 1;
    c.target = 2;
    cam2.update(kStep, c, figures);
    CHECK(cam2.shot() == CameraShot::Behind);
    CHECK(dist3(mid.eye, cam2.pose().eye) < 15.0);
    CHECK(dist3(mid.look, cam2.pose().look) < 30.0);
}

TEST_CASE("camera: the Recovery pause keeps the shot it follows; a new round blends from where the camera was") {
    const auto figures = stage(2, 3);
    BattleCamera cam;
    cam.begin(figures);
    CameraCue c;
    c.phase = Phase::EnemySkill;
    c.actor = 2;
    c.defender = 1;
    run(cam, c, figures, 2.0);
    const BattleCameraPose attack = cam.pose();
    c.phase = Phase::Recovery;
    run(cam, c, figures, 0.7);
    CHECK(cam.shot() == CameraShot::Behind);
    CHECK(cam.blend() == doctest::Approx(1.0));
    // The drift held through the pause: it did not swing back to the middle and out again.
    CHECK(std::fabs(cam.lateralOf(cam.pose().eye) - cam.lateralOf(attack.eye)) < 5.0);

    // A party skill's Recovery after a choice stays on the actor.
    BattleCamera item;
    item.begin(figures);
    run(item, choosing(0, 2), figures, 1.0);
    c.phase = Phase::Recovery;
    c.actor = 0;
    run(item, c, figures, 0.7);
    CHECK(item.shot() == CameraShot::Choose);
    CHECK(item.shotActor() == 0);

    // A second round: begin() blends, it does not snap to the first frame's shot.
    cam.begin(figures);
    const BattleCameraPose before = cam.pose();
    cam.update(kStep, choosing(0, 2), figures);
    CHECK(dist3(before.eye, cam.pose().eye) < 10.0);
    CHECK(cam.blend() < 0.5);
}

TEST_CASE("camera: odd input does not break it") {
    BattleCamera cam;
    // Not begun, no figures: nothing happens.
    cam.update(kStep, choosing(0, 1), {});
    // One lead against one enemy; an actor or target that does not exist; a huge or a negative frame.
    const auto figures = stage(1, 1);
    cam.begin(figures);
    for (const CameraCue& c : {choosing(7, 9), choosing(-1, -1), choosing(0, 0), choosing(0, 1), choosing(0, -1)}) {
        for (const double dt : {0.0, 0.001, 0.5, 30.0, -1.0}) {
            cam.update(dt, c, figures);
            CHECK(finite(cam.pose()));
        }
    }
    CameraCue c;
    c.phase = Phase::EnemySkill;
    c.actor = 5;
    c.defender = 9;
    cam.update(kStep, c, figures);
    CHECK(finite(cam.pose()));
    c.phase = Phase::Recovery;
    cam.update(kStep, c, figures);
    CHECK(finite(cam.pose()));
    // No enemies at all.
    const auto lone = stage(2, 0);
    BattleCamera empty;
    empty.begin(lone);
    run(empty, choosing(0, -1), lone, 1.0);
    CHECK(finite(empty.pose()));
    // A camera that stands among figures lifts over them.
    CameraTuning near;
    near.chooseBack = 0.0;
    near.chooseSide = 0.0;
    near.chooseEye = 0.2;
    BattleCamera inside(near);
    const auto two = stage(2, 2);
    inside.begin(two);
    run(inside, choosing(0, 2), two, 1.0);
    checkSafe(inside, two);
    CHECK(inside.pose().eye.z > two[0].height);
}

TEST_CASE("camera: a whole Turns fight, with the player taking a second and a half over each choice") {
    std::vector<Combatant> party = demo::party();
    std::vector<Combatant> foes = demo::enemies();
    Battle b(party, foes, demo::tuning());
    const std::size_t n = b.combatants().size();
    std::vector<CameraFigure> figures = stage(party.size(), foes.size());
    REQUIRE(figures.size() == n);
    BattleCamera cam;
    cam.begin(figures);
    demo::PilotSkill skill;
    demo::PilotState state;

    double now = 0.0;
    b.start(now);
    double chooseStart = -1.0;
    int target = -1;
    std::uint64_t allocs = 0;
    BattleCameraPose last = cam.pose();
    double worst = 0.0;
    int chooseFrames = 0, behindFrames = 0, wideFrames = 0, driftFrames = 0;
    bool wasOver = false;
    for (int steps = 0; steps < 200000 && !wasOver; ++steps) {
        const Phase before = b.phase();
        if (b.phase() == Phase::AwaitingCommand) {
            if (chooseStart < 0.0) {
                chooseStart = now;
                target = -1;
            }
            // The player cycles through the living enemies, then picks.
            const int cycle = static_cast<int>((now - chooseStart) / 0.4);
            int seen = -1;
            for (int i = 0; i < static_cast<int>(n) && cycle >= 0; ++i) {
                if (b.combatant(static_cast<CombatantId>(i)).team == Team::Enemies && b.combatant(static_cast<CombatantId>(i)).alive()) {
                    if (++seen == cycle % static_cast<int>(b.livingCount(Team::Enemies))) target = i;
                }
            }
            if (now - chooseStart >= 1.5) {
                const demo::PilotChoice choice = demo::pilotChoice(b);
                const CommandError e = choice.aim ? b.aim(choice.target, now) : b.command(choice.skill, choice.target, now);
                REQUIRE(e == CommandError::None);
                if (choice.aim) demo::pilotShoot(b, choice.weakPoint, now);
                chooseStart = -1.0;
            }
        }
        demo::pilotPress(b, now, skill, state);
        demo::pilotDefend(b, now + kStep, DefenceInput::Parry, skill, state);
        now += kStep;
        b.update(now);
        b.clearEvents();

        for (std::size_t i = 0; i < n; ++i) figures[i].alive = b.combatant(static_cast<CombatantId>(i)).alive();
        CameraCue cue;
        cue.phase = b.phase();
        cue.actor = static_cast<int>(b.actor());
        cue.defender = static_cast<int>(b.defender());
        cue.target = b.phase() == Phase::AwaitingCommand || b.phase() == Phase::PartyAim ? target
                     : b.phase() == Phase::PartySkill && b.activeTargets().size() == 1 ? static_cast<int>(b.activeTargets().front())
                                                                                      : -1;
        const auto allocBefore = ftcore_test::allocStats().count;
        cam.update(kStep, cue, figures);
        if (now > 5.0) allocs += ftcore_test::allocStats().count - allocBefore;

        // The shot follows the phase.
        switch (b.phase()) {
        case Phase::AwaitingCommand:
            CHECK(cam.shot() == CameraShot::Choose);
            CHECK(cam.shotActor() == static_cast<int>(b.actor()));
            ++chooseFrames;
            break;
        case Phase::PartySkill:
        case Phase::EnemySkill:
            CHECK(cam.shot() == CameraShot::Behind);
            ++behindFrames;
            if (b.phase() == Phase::EnemySkill && cam.blend() >= 1.0) {
                // Settled on an enemy turn: behind every living lead, drifted toward the defender.
                for (std::size_t i = 0; i < n; ++i)
                    if (figures[i].party && figures[i].alive) CHECK(cam.depthOf(cam.pose().eye) < cam.depthOf(figures[i].pos) - 100.0);
                ++driftFrames;
            }
            break;
        case Phase::Victory:
        case Phase::Defeat:
            CHECK(cam.shot() == CameraShot::Wide);
            ++wideFrames;
            wasOver = true;
            break;
        default:
            break;
        }
        (void)before;
        checkSafe(cam, figures);
        if (steps > 0) worst = std::max(worst, dist3(last.eye, cam.pose().eye)); // the first frame takes its pose
        last = cam.pose();
    }
    CHECK(b.phase() == Phase::Victory);
    CHECK(chooseFrames > 0);
    CHECK(behindFrames > 0);
    CHECK(driftFrames > 0);
    CHECK(wideFrames > 0);
    CHECK(worst < 30.0); // never a snap
    if (ftcore_test::allocHookInstalled()) CHECK(allocs == 0);
}
