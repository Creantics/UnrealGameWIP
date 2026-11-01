#include "ftcore/BattleCamera.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

CamVec operator+(const CamVec& a, const CamVec& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
CamVec operator-(const CamVec& a, const CamVec& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
CamVec operator*(const CamVec& a, double s) { return {a.x * s, a.y * s, a.z * s}; }

CamVec lerp(const CamVec& a, const CamVec& b, double t) { return a + (b - a) * t; }

// Smootherstep: zero slope and curvature at both ends.
double ease(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

bool valid(int id, std::span<const CameraFigure> figures) {
    return id >= 0 && static_cast<std::size_t>(id) < figures.size();
}

bool isParty(int id, std::span<const CameraFigure> figures) { return valid(id, figures) && figures[static_cast<std::size_t>(id)].party; }

bool isEnemy(int id, std::span<const CameraFigure> figures) {
    return valid(id, figures) && !figures[static_cast<std::size_t>(id)].party;
}

// The middle of one side's feet: its living figures, else all of them; false when the side is empty.
bool centreOf(std::span<const CameraFigure> figures, bool party, CamVec& out, double* meanHeight = nullptr) {
    CamVec sum;
    double heights = 0.0;
    int n = 0;
    for (int pass = 0; pass < 2 && n == 0; ++pass) {
        for (const CameraFigure& f : figures) {
            if (f.party != party || (pass == 0 && !f.alive)) continue;
            sum = sum + f.pos;
            heights += f.height;
            ++n;
        }
    }
    if (n == 0) return false;
    out = sum * (1.0 / n);
    if (meanHeight) *meanHeight = heights / n;
    return true;
}

} // namespace

CamVec stageHome(bool party, std::size_t slot, std::size_t enemyCount) {
    const double s = static_cast<double>(slot);
    if (party) return {80.0 - 180.0 * s, -400.0 + 130.0 * s, 0.0};
    // A third enemy gets a tighter stagger so the row stays in frame, and a fourth and fifth tighter still.
    const double stepX = enemyCount > 3 ? 105.0 : enemyCount > 2 ? 140.0 : 180.0;
    const double stepY = enemyCount > 3 ? 70.0 : enemyCount > 2 ? 95.0 : 130.0;
    return {80.0 - stepX * s, 360.0 + stepY * s, 0.0};
}

BattleCamera::BattleCamera(const CameraTuning& tuning) : tuning_(tuning) {
    // Before the first begin(): the party's side of the demo stage, looking at the enemies.
    partyCentre_ = {-10.0, -335.0, 0.0};
    pose_.eye = {-10.0, -900.0, 320.0};
    pose_.look = {-10.0, 450.0, 80.0};
    pose_.fov = tuning_.behindFov;
    from_ = pose_;
}

double BattleCamera::depthOf(const CamVec& p) const {
    return (p.x - partyCentre_.x) * forward_.x + (p.y - partyCentre_.y) * forward_.y;
}

double BattleCamera::lateralOf(const CamVec& p) const {
    const CamVec r = right();
    return (p.x - partyCentre_.x) * r.x + (p.y - partyCentre_.y) * r.y;
}

void BattleCamera::begin(std::span<const CameraFigure> figures) {
    CamVec party, enemies;
    const bool haveParty = centreOf(figures, true, party);
    const bool haveEnemies = centreOf(figures, false, enemies);
    if (haveParty) partyCentre_ = {party.x, party.y, 0.0};
    forward_ = {0.0, 1.0, 0.0};
    if (haveParty && haveEnemies) {
        const double dx = enemies.x - party.x;
        const double dy = enemies.y - party.y;
        const double len = std::sqrt(dx * dx + dy * dy);
        if (len > 1e-6) forward_ = {dx / len, dy / len, 0.0};
    }
    shot_ = CameraShot::Behind;
    actor_ = -1;
    sideSign_ = 1.0;
    snapFollow_ = true;
    restart_ = placed_; // a later round glides from where the camera is
}

double BattleCamera::blendTime(CameraShot shot) const {
    switch (shot) {
    case CameraShot::Choose: return tuning_.blendChoose;
    case CameraShot::Aim: return tuning_.blendAim;
    case CameraShot::Behind: return tuning_.blendBehind;
    case CameraShot::Wide: return tuning_.blendWide;
    }
    return tuning_.blendBehind;
}

CameraShot BattleCamera::shotFor(const CameraCue& cue, std::span<const CameraFigure> figures) const {
    switch (cue.phase) {
    case Phase::AwaitingCommand:
        return isParty(cue.actor, figures) && figures[static_cast<std::size_t>(cue.actor)].alive ? CameraShot::Choose
                                                                                                  : CameraShot::Behind;
    case Phase::PartyAim: return isParty(cue.actor, figures) ? CameraShot::Aim : CameraShot::Behind;
    case Phase::Victory:
    case Phase::Defeat: return CameraShot::Wide;
    case Phase::Recovery: return shot_; // the pause after an action: stay where the action was
    case Phase::NotStarted:
    case Phase::PartySkill:
    case Phase::EnemySkill: return CameraShot::Behind;
    }
    return CameraShot::Behind;
}

void BattleCamera::follow(double dt, const CameraCue& cue, std::span<const CameraFigure> figures, bool snap) {
    const double k = snap ? 1.0 : 1.0 - std::exp(-dt / std::max(tuning_.follow, 1e-6));
    const double kSlow = snap ? 1.0 : 1.0 - std::exp(-dt / std::max(tuning_.follow * 2.0, 1e-6));

    CamVec enemyC;
    double enemyH = 110.0;
    if (!centreOf(figures, false, enemyC, &enemyH)) enemyC = partyCentre_ + forward_ * 800.0;
    CamVec partyC = partyCentre_;
    centreOf(figures, true, partyC);

    CamVec goal;
    switch (shot_) {
    case CameraShot::Choose:
    case CameraShot::Aim: {
        const CameraFigure& a = figures[static_cast<std::size_t>(actor_)];
        const bool picked = isEnemy(cue.target, figures);
        const CameraFigure* t = picked ? &figures[static_cast<std::size_t>(cue.target)] : nullptr;
        const CamVec at = t ? t->pos : enemyC;
        const double th = t ? t->height : enemyH;
        // The shoulder: the side the target lies on, kept until it is clearly across.
        const double across = lateralOf(at) - lateralOf(a.pos);
        if (across > tuning_.sideHysteresis) sideSign_ = 1.0;
        else if (across < -tuning_.sideHysteresis) sideSign_ = -1.0;
        side_ += (sideSign_ - side_) * (snap ? 1.0 : k);
        const double ahead = shot_ == CameraShot::Aim ? tuning_.aimLookAhead : tuning_.chooseLookAhead;
        goal = lerp(a.pos, at, ahead);
        goal.z = 0.5 * (0.8 * a.height + 0.55 * th);
        break;
    }
    case CameraShot::Behind: {
        // Lean toward whoever is acting against the party, or the enemy being hit.
        const bool lean = (cue.phase == Phase::EnemySkill && isEnemy(cue.actor, figures)) ||
                          (cue.phase == Phase::PartySkill && isEnemy(cue.target, figures));
        const int who = cue.phase == Phase::EnemySkill ? cue.actor : cue.target;
        goal = enemyC;
        if (lean) goal = lerp(enemyC, figures[static_cast<std::size_t>(who)].pos, tuning_.leanShare);
        goal.z = 0.55 * enemyH;
        double driftGoal = 0.0;
        if (cue.phase == Phase::EnemySkill && isParty(cue.defender, figures)) {
            driftGoal = (lateralOf(figures[static_cast<std::size_t>(cue.defender)].pos) - lateralOf(partyC)) *
                        tuning_.driftShare;
            driftGoal = std::clamp(driftGoal, -tuning_.driftMax, tuning_.driftMax);
        } else if (cue.phase == Phase::Recovery) {
            driftGoal = drift_; // hold the drift through the pause
        }
        drift_ += (driftGoal - drift_) * (snap ? 1.0 : kSlow);
        break;
    }
    case CameraShot::Wide: {
        goal = lerp(partyC, enemyC, 0.5);
        goal.z = 70.0;
        break;
    }
    }
    look_ = snap ? goal : lerp(look_, goal, k);
}

BattleCameraPose BattleCamera::solve(const CameraCue& /*cue*/, std::span<const CameraFigure> figures) const {
    BattleCameraPose out;
    out.look = look_;
    const CamVec r = right();
    switch (shot_) {
    case CameraShot::Choose:
    case CameraShot::Aim: {
        const CameraFigure& a = figures[static_cast<std::size_t>(actor_)];
        out.eye = a.pos - forward_ * tuning_.chooseBack + r * (side_ * tuning_.chooseSide);
        out.eye.z = a.height * tuning_.chooseEye;
        out.fov = shot_ == CameraShot::Aim ? tuning_.aimFov : tuning_.chooseFov;
        // The actor stays in the frame's lower corner: the look may turn from the actor by no more than a share of the
        // half field of view, whatever the target's bearing (so a target across the stage does not push the actor off
        // the edge of the picture).
        {
            constexpr double kPi = 3.14159265358979323846;
            const double toActor = std::atan2(a.pos.y - out.eye.y, a.pos.x - out.eye.x);
            const double toLook = std::atan2(out.look.y - out.eye.y, out.look.x - out.eye.x);
            double turn = toLook - toActor;
            while (turn > kPi) turn -= 2.0 * kPi;
            while (turn < -kPi) turn += 2.0 * kPi;
            const double limit = tuning_.actorFrame * 0.5 * out.fov * kPi / 180.0;
            if (std::fabs(turn) > limit) {
                const double yaw = toActor + (turn > 0.0 ? limit : -limit);
                const double reach = std::hypot(out.look.x - out.eye.x, out.look.y - out.eye.y);
                out.look.x = out.eye.x + std::cos(yaw) * reach;
                out.look.y = out.eye.y + std::sin(yaw) * reach;
            }
        }
        break;
    }
    case CameraShot::Behind: {
        // Behind the rearmost living party member (all of them when none lives), in their middle.
        double rear = 0.0;
        double lat = 0.0;
        double tall = 190.0;
        int n = 0;
        for (int pass = 0; pass < 2 && n == 0; ++pass) {
            for (const CameraFigure& f : figures) {
                if (!f.party || (pass == 0 && !f.alive)) continue;
                const double d = depthOf(f.pos);
                rear = n == 0 ? d : std::min(rear, d);
                lat += lateralOf(f.pos);
                tall = n == 0 ? f.height : std::max(tall, f.height);
                ++n;
            }
        }
        if (n > 0) lat /= n;
        out.eye = partyCentre_ + forward_ * (rear - tuning_.behindBack) + r * (lat + drift_);
        out.eye.z = tall * tuning_.behindEye;
        out.fov = tuning_.behindFov;
        break;
    }
    case CameraShot::Wide: {
        out.eye = partyCentre_ - forward_ * tuning_.wideBack + r * tuning_.wideSide;
        out.eye.z = tuning_.wideEye;
        out.fov = tuning_.wideFov;
        break;
    }
    }
    return out;
}

void BattleCamera::clear(BattleCameraPose& pose, std::span<const CameraFigure> figures) const {
    // Outside an ellipsoid around every figure: wide by the figure's radius plus minClear, tall by its height plus
    // minAbove. Inside one, the camera rises to its surface, so a pass close by lifts it smoothly instead of
    // shoving it sideways or letting it through. A fallen figure is a long, low shape.
    for (int pass = 0; pass < 2; ++pass) {
        for (const CameraFigure& f : figures) {
            const double rxy = (f.alive ? f.radius : std::max(f.radius, f.height)) + tuning_.minClear;
            const double rz = (f.alive ? f.height : f.radius * 2.0) + tuning_.minAbove;
            const double dx = pose.eye.x - f.pos.x;
            const double dy = pose.eye.y - f.pos.y;
            const double q = (dx * dx + dy * dy) / (rxy * rxy);
            if (q >= 1.0) continue;
            pose.eye.z = std::max(pose.eye.z, f.pos.z + rz * std::sqrt(1.0 - q));
        }
    }
    pose.eye.z = std::max(pose.eye.z, tuning_.minHeight);
}

void BattleCamera::update(double dt, const CameraCue& cue, std::span<const CameraFigure> figures) {
    if (figures.empty()) return;
    dt = std::clamp(dt, 0.0, tuning_.maxStep);

    CameraShot want = shotFor(cue, figures);
    if ((want == CameraShot::Choose || want == CameraShot::Aim) && cue.phase == Phase::Recovery && !valid(actor_, figures))
        want = CameraShot::Behind;
    const bool actorShot = want == CameraShot::Choose || want == CameraShot::Aim;
    int actor = actor_;
    if (actorShot && cue.phase != Phase::Recovery) actor = cue.actor;
    const bool changed = want != shot_ || (actorShot && actor != actor_) || restart_;
    if (changed) {
        if (placed_) {
            from_ = pose_;
            elapsed_ = 0.0;
            duration_ = blendTime(want);
        }
        shot_ = want;
        actor_ = actorShot ? actor : -1;
        snapFollow_ = true;
        restart_ = false;
    } else {
        elapsed_ = std::min(elapsed_ + dt, duration_);
    }

    follow(dt, cue, figures, snapFollow_);
    snapFollow_ = false;

    const BattleCameraPose goal = solve(cue, figures);
    if (!placed_) {
        pose_ = goal;
        from_ = goal;
        elapsed_ = 0.0;
        duration_ = 0.0;
        placed_ = true;
    } else {
        const double e = duration_ > 0.0 ? ease(elapsed_ / duration_) : 1.0;
        pose_.eye = lerp(from_.eye, goal.eye, e);
        pose_.look = lerp(from_.look, goal.look, e);
        pose_.fov = from_.fov + (goal.fov - from_.fov) * e;
    }
    clear(pose_, figures);
}

} // namespace ftcore
