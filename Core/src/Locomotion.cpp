#include "ftcore/Locomotion.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

constexpr float kDegToRad = 3.14159265f / 180.f;
constexpr LocoClip kGaits[] = {LocoClip::Walk, LocoClip::Run, LocoClip::Sprint, LocoClip::CrouchWalk};
constexpr LocoClip kStandGaits[] = {LocoClip::Walk, LocoClip::Run, LocoClip::Sprint};

float frac(float x) { return x - std::floor(x); }

float clampf(float x, float lo, float hi) { return std::min(std::max(x, lo), hi); }

// An angle (degrees) brought within -180..180.
float wrap180(float a) { return a - 360.f * std::floor((a + 180.f) / 360.f); }

// The share of the remaining distance an exponential follow covers in dt.
float follow(float dt, float timeConstant) { return timeConstant > 0.f ? 1.f - std::exp(-dt / timeConstant) : 1.f; }

struct FootTrack {
    float speed = 0.f; // median planted speed relative to the root, 0 if never planted
    float side = 0.f;  // median planted speed across the way the foot swings
    int plants = 0;
    int firstPlant = -1; // frame of the first plant onset
};

// Turned back by the root's yaw since frame 0, so the feet of a turning clip compare with a gait's.
LocoPoint footRel(const GaitFrame* frames, std::size_t k, bool left) {
    const LocoPoint& f = left ? frames[k].left : frames[k].right;
    const LocoPoint& r = frames[k].root;
    const float x = f.x - r.x, y = f.y - r.y;
    const float a = (frames[k].yaw - frames[0].yaw) * kDegToRad;
    if (a == 0.f) return LocoPoint{x, y, f.z - r.z};
    const float c = std::cos(a), sn = std::sin(a);
    return LocoPoint{x * c + y * sn, y * c - x * sn, f.z - r.z};
}

float footMinZ(const GaitFrame* frames, std::size_t n, bool left) {
    float minZ = footRel(frames, 0, left).z;
    for (std::size_t k = 1; k < n; ++k) minZ = std::min(minZ, footRel(frames, k, left).z);
    return minZ;
}

// The way a foot travels while it swings (unnormalized): the way the body walks.
LocoPoint swingTravel(const GaitFrame* frames, std::size_t n, bool left) {
    const float minZ = footMinZ(frames, n, left);
    LocoPoint d;
    for (std::size_t k = 0; k + 1 < n; ++k) {
        const LocoPoint a = footRel(frames, k, left), b = footRel(frames, k + 1, left);
        if (a.z - minZ > kSwingHeight) {
            d.x += b.x - a.x;
            d.y += b.y - a.y;
        }
    }
    return d;
}

// dirX, dirY: the way the body walks (unit), from both feet's swings.
FootTrack measureFoot(const GaitFrame* frames, std::size_t n, float dt, bool left, float dirX, float dirY) {
    FootTrack out;
    const auto rel = [&](std::size_t k) { return footRel(frames, k, left); };
    const float minZ = footMinZ(frames, n, left);
    // A planted foot moves back under the root. A low foot still moving forward (a toe skimming the
    // ground mid-swing, or a heel coming down) isn't planted.
    const LocoPoint swing = swingTravel(frames, n, left);
    if (swing.x == 0.f && swing.y == 0.f) return out;
    const auto planted = [&](std::size_t k) {
        const std::size_t next = k + 1 < n ? k + 1 : 1; // the last frame is the first again
        const LocoPoint a = rel(k), b = rel(next);
        return a.z - minZ < kPlantHeight && (b.x - a.x) * dirX + (b.y - a.y) * dirY < 0.f;
    };

    std::array<float, kMaxGaitFrames> speeds{};
    std::array<float, kMaxGaitFrames> sides{};
    std::size_t count = 0;
    // Start from the state the loop ends in, so a plant spanning the wrap isn't counted twice. The
    // last frame is the first again: a plant starting there starts at frame 0, so read from the one before.
    bool down = false;
    for (std::size_t k = n - 1; k-- > 0;) {
        if (planted(k)) { down = true; break; }
        if (rel(k).z - minZ > kSwingHeight) break;
    }
    for (std::size_t k = 0; k + 1 < n; ++k) {
        const LocoPoint a = rel(k), b = rel(k + 1);
        const bool plant = planted(k);
        if (!down && plant) {
            down = true;
            ++out.plants;
            if (out.firstPlant < 0) out.firstPlant = static_cast<int>(k);
        } else if (down && a.z - minZ > kSwingHeight) {
            down = false;
        }
        if (plant) {
            sides[count] = std::fabs((b.y - a.y) * dirX - (b.x - a.x) * dirY) / dt;
            speeds[count++] = std::hypot(b.x - a.x, b.y - a.y) / dt;
        }
    }
    if (out.plants > 0 && count > 0) {
        float* mid = speeds.data() + count / 2;
        std::nth_element(speeds.data(), mid, speeds.data() + count);
        out.speed = *mid;
        mid = sides.data() + count / 2;
        std::nth_element(sides.data(), mid, sides.data() + count);
        out.side = *mid;
    }
    return out;
}

LocoPoint lerp(const LocoPoint& a, const LocoPoint& b, float u) {
    return {a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u};
}

LocoPoint sub(const LocoPoint& a, const LocoPoint& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

float dist2(const LocoPoint& a, const LocoPoint& b) {
    const LocoPoint d = sub(a, b);
    return d.x * d.x + d.y * d.y + d.z * d.z;
}

constexpr float kLastSample = static_cast<float>(kMoveSamples - 1);

// Where time t falls in a clip's move track: the sample before it and the share of the way to the next.
std::size_t trackAt(const LocoClipInfo& i, float t, float& u) {
    const float f = i.moveTime > 0.f ? clampf(t / i.moveTime, 0.f, 1.f) * kLastSample : 0.f;
    const std::size_t k = std::min(static_cast<std::size_t>(f), kMoveSamples - 2);
    u = f - static_cast<float>(k);
    return k;
}

float pathAt(const LocoClipInfo& i, float t) {
    float u = 0.f;
    const std::size_t k = trackAt(i, t, u);
    return i.path[k] + (i.path[k + 1] - i.path[k]) * u;
}

// The first time the root has come `d` along its path.
float timeAtPath(const LocoClipInfo& i, float d) {
    if (d <= i.path[0]) return 0.f;
    for (std::size_t k = 0; k + 1 < kMoveSamples; ++k) {
        if (i.path[k + 1] < d) continue;
        const float span = i.path[k + 1] - i.path[k];
        const float u = span > 0.f ? (d - i.path[k]) / span : 0.f;
        return (static_cast<float>(k) + u) / kLastSample * i.moveTime;
    }
    return i.moveTime;
}

// The root's speed along its path at t (rate 1).
float pathSpeed(const LocoClipInfo& i, float t) {
    const float h = i.moveTime / kLastSample;
    const float a = std::max(t - h, 0.f), b = std::min(t + h, i.moveTime);
    return b > a ? (pathAt(i, b) - pathAt(i, a)) / (b - a) : 0.f;
}

// The root's yaw at t (from the clip's start); the yaw track spans the whole clip.
float yawAt(const LocoClipInfo& i, float t) {
    const float f = i.length > 0.f ? clampf(t / i.length, 0.f, 1.f) * kLastSample : 0.f;
    const std::size_t k = std::min(static_cast<std::size_t>(f), kMoveSamples - 2);
    return i.yaw[k] + (i.yaw[k + 1] - i.yaw[k]) * (f - static_cast<float>(k));
}

LocoFeet feetAt(const LocoClipInfo& i, float t) {
    float u = 0.f;
    const std::size_t k = trackAt(i, t, u);
    return {lerp(i.feet[k].left, i.feet[k + 1].left, u), lerp(i.feet[k].right, i.feet[k + 1].right, u)};
}

// The feet's velocity relative to the root at t, played at `rate`.
LocoFeet feetVelocity(const LocoClipInfo& i, float t, float rate) {
    const float h = i.moveTime / kLastSample;
    const float a = std::max(t - h, 0.f), b = std::min(t + h, i.moveTime);
    if (b <= a) return {};
    const LocoFeet fa = feetAt(i, a), fb = feetAt(i, b);
    const float s = rate / (b - a);
    const LocoPoint l = sub(fb.left, fa.left), r = sub(fb.right, fa.right);
    return {{l.x * s, l.y * s, l.z * s}, {r.x * s, r.y * s, r.z * s}};
}

// How far apart two poses of the feet are: their positions, and their velocities by `velocityWeight`.
float poseCost(const LocoFeet& a, const LocoFeet& av, const LocoFeet& b, const LocoFeet& bv, float velocityWeight) {
    return dist2(a.left, b.left) + dist2(a.right, b.right) + velocityWeight * (dist2(av.left, bv.left) + dist2(av.right, bv.right));
}

} // namespace

LocoClipInfo measureMove(const GaitFrame* frames, std::size_t count, float length) {
    LocoClipInfo info;
    info.length = std::max(length, 0.f);
    const std::size_t n = std::min(count, kMaxGaitFrames);
    if (!frames || n < 2 || length <= 0.f) return info;
    const float dt = length / static_cast<float>(n - 1);
    std::array<float, kMaxGaitFrames> path{};
    for (std::size_t k = 1; k < n; ++k) {
        path[k] = path[k - 1] + std::hypot(frames[k].root.x - frames[k - 1].root.x, frames[k].root.y - frames[k - 1].root.y);
    }
    // The root moves until the frame after the last one away from where it ends; a root that never
    // moves has the whole clip as its track.
    const LocoPoint& end = frames[n - 1].root;
    std::size_t last = n - 1;
    for (std::size_t k = n; k-- > 0;) {
        if (std::hypot(frames[k].root.x - end.x, frames[k].root.y - end.y) > kMoveStill) {
            last = std::min(k + 1, n - 1);
            break;
        }
    }
    info.moveTime = path[n - 1] > kMoveStill ? static_cast<float>(last) * dt : info.length;
    for (std::size_t s = 0; s < kMoveSamples; ++s) {
        const float f = info.moveTime * static_cast<float>(s) / kLastSample / dt;
        const std::size_t k = std::min(static_cast<std::size_t>(f), n - 2);
        const float u = std::min(f - static_cast<float>(k), 1.f);
        info.path[s] = path[k] + (path[k + 1] - path[k]) * u;
        info.feet[s] = {lerp(footRel(frames, k, true), footRel(frames, k + 1, true), u),
                        lerp(footRel(frames, k, false), footRel(frames, k + 1, false), u)};
    }
    info.travel = info.path[kMoveSamples - 1];

    // The root's yaw, unwrapped, over the whole clip (a turn on the spot barely moves).
    std::array<float, kMaxGaitFrames> yaw{};
    for (std::size_t k = 1; k < n; ++k) yaw[k] = yaw[k - 1] + wrap180(frames[k].yaw - frames[k - 1].yaw);
    for (std::size_t s = 0; s < kMoveSamples; ++s) {
        const float f = static_cast<float>(s) / kLastSample * static_cast<float>(n - 1);
        const std::size_t k = std::min(static_cast<std::size_t>(f), n - 2);
        info.yaw[s] = yaw[k] + (yaw[k + 1] - yaw[k]) * std::min(f - static_cast<float>(k), 1.f);
    }
    info.turn = yaw[n - 1];
    if (std::fabs(info.turn) < kTurnStill) {
        info.turn = 0.f;
        info.yaw.fill(0.f);
        return info;
    }
    // When it has done 5% and 95% of its turn.
    const float sign = info.turn > 0.f ? 1.f : -1.f, whole = std::fabs(info.turn);
    bool started = false;
    for (std::size_t k = 0; k < n; ++k) {
        const float done = yaw[k] * sign;
        if (!started && done >= 0.05f * whole) {
            info.turnStart = static_cast<float>(k) * dt;
            started = true;
        }
        if (done >= 0.95f * whole) {
            info.turnEnd = static_cast<float>(k) * dt;
            break;
        }
    }
    return info;
}

LocoClipInfo measureGait(const GaitFrame* frames, std::size_t count, float length) {
    LocoClipInfo info = measureMove(frames, count, length);
    const std::size_t n = std::min(count, kMaxGaitFrames);
    if (!frames || n < 3 || length <= 0.f) return info;
    const float dt = length / static_cast<float>(n - 1);
    // Each leg may swing a little across the body; together they swing the way it walks.
    const LocoPoint ls = swingTravel(frames, n, true), rs = swingTravel(frames, n, false);
    const float dirLen = std::hypot(ls.x + rs.x, ls.y + rs.y);
    if (dirLen <= 0.f) return info;
    const float dirX = (ls.x + rs.x) / dirLen, dirY = (ls.y + rs.y) / dirLen;
    const FootTrack l = measureFoot(frames, n, dt, true, dirX, dirY);
    const FootTrack r = measureFoot(frames, n, dt, false, dirX, dirY);
    const int feet = (l.speed > 0.f) + (r.speed > 0.f);
    info.groundSpeed = info.footSpeed = feet ? (l.speed + r.speed) / static_cast<float>(feet) : 0.f;
    info.footSide = feet ? (l.side + r.side) / static_cast<float>(feet) : 0.f;
    // Not the root's travel, even in a clip with root motion: mocap feet often slip a few percent
    // against it, and matching the capsule to the root would carry that slip into the game.
    info.cycles = std::max(l.plants, 1);
    info.leftPlant = l.firstPlant >= 0 ? static_cast<float>(l.firstPlant) * dt / length : 0.f;
    return info;
}

void Locomotion::setClip(LocoClip clip, const LocoClipInfo& info) {
    if (clip == LocoClip::Count) return;
    LocoClipInfo& slot = info_[index(clip)];
    slot = info;
    slot.length = std::max(slot.length, 0.f);
    slot.groundSpeed = std::max(slot.groundSpeed, 0.f);
    slot.cycles = std::max(slot.cycles, 1);
    slot.leftPlant = frac(slot.leftPlant);
    slot.moveTime = clampf(slot.moveTime, 0.f, slot.length);
}

void Locomotion::reset() {
    weight_.fill(0.f);
    weight_[index(LocoClip::Idle)] = 1.f;
    time_.fill(0.f);
    rate_.fill(1.f);
    jump_ = LocoClip::JumpStart;
    land_ = LocoClip::Land;
    move_ = LocoClip::RunStart;
    moved_ = braking_ = 0.f;
    turnEnter_ = turnFrom_ = turnTo_ = yawStep_ = 0.f;
    warp_ = 1.f;
    rootDriven_ = pivot_ = false;
    limbs_ = {};
    layers_[0] = {LocoClip::Idle, 0.f, 1.f};
    layerCount_ = 1;
    state_ = LocoState::Ground;
    target_ = gait_ = LocoClip::Idle;
    share_.fill(0.f);
    stride_.fill(0);
    fade_ = phase_ = airTime_ = groundTime_ = 0.f;
    landTime_ = -1.f;
    lean_ = twist_ = pitch_ = 0.f;
    headPitch_ = headRoll_ = headYaw_ = 0.f;
    elbowHold_ = decel_ = lastSpeed_ = 0.f;
    crouched_ = false;
    crouch_ = 0.f;
    landRoll_ = false;
}

float Locomotion::footPhase(LocoClip c, float t) const {
    const LocoClipInfo& i = info_[index(c)];
    if (i.length <= 0.f) return 0.f;
    return frac((t / i.length - i.leftPlant) * static_cast<float>(i.cycles));
}

float Locomotion::timeAtPhase(LocoClip c, float phase) const {
    const LocoClipInfo& i = info_[index(c)];
    if (i.length <= 0.f) return 0.f;
    return frac(i.leftPlant + frac(phase) / static_cast<float>(i.cycles)) * i.length;
}

void Locomotion::splitGaits(float speed) {
    // The crouch walk takes its share (all of it once crouched), the standing gaits split the rest.
    share_.fill(0.f);
    const float crouch = has(LocoClip::CrouchWalk) ? crouch_ : 0.f;
    share_[index(LocoClip::CrouchWalk)] = crouch;
    // The standing gaits this body has, slowest first.
    std::array<LocoClip, 3> order{};
    std::size_t n = 0;
    for (const LocoClip c : kStandGaits) {
        if (!has(c)) continue;
        std::size_t k = n++;
        for (; k > 0 && clip(order[k - 1]).groundSpeed > clip(c).groundSpeed; --k) order[k] = order[k - 1];
        order[k] = c;
    }
    if (n == 0) {
        if (crouch > 0.f) share_[index(LocoClip::CrouchWalk)] = 1.f;
        return;
    }
    const float stand = 1.f - crouch;
    std::size_t k = 0;
    while (k + 1 < n && speed > clip(order[k + 1]).groundSpeed) ++k;
    const float lo = clip(order[k]).groundSpeed;
    if (k + 1 == n || speed <= lo) {
        share_[index(order[k])] = stand;
        return;
    }
    // The faster gait plays alone from gaitBand short of its own speed; the two mix from the slower one's up
    // to there. A gait with only a few percent of the mix plays at a rate far from its own and slides the
    // planted foot; near either end the nearer gait plays alone.
    const float hi = std::max((1.f - tuning_.gaitBand) * clip(order[k + 1]).groundSpeed, lo);
    float t = speed >= hi ? 1.f : (speed - lo) / (hi - lo);
    t = t < tuning_.gaitDeadBand ? 0.f : t > 1.f - tuning_.gaitDeadBand ? 1.f : t;
    share_[index(order[k])] = (1.f - t) * stand;
    share_[index(order[k + 1])] = t * stand;
}

LocoClip Locomotion::groundTarget(float speed) {
    const bool moving = isLocoGait(gait_);
    if (speed < (moving ? tuning_.moveStop : tuning_.moveStart) ||
        !(crouched_ || has(LocoClip::Walk) || has(LocoClip::Run) || has(LocoClip::Sprint))) {
        gait_ = idleClip();
        return gait_;
    }
    gait_ = heaviestGait();
    return gait_;
}

LocoClip Locomotion::heaviestGait() const {
    LocoClip g = LocoClip::Walk;
    for (const LocoClip c : kGaits) {
        if (share_[index(c)] > share_[index(g)]) g = c;
    }
    return g;
}

bool Locomotion::beginStop(const LocoInput& in, float speed) {
    if (braking_ < tuning_.stopDelay || in.stopDistance < 0.f || !isLocoGait(gait_) || speed < tuning_.moveStop) return false;
    if (crouched_ || gait_ == LocoClip::CrouchWalk) return false; // the crouch walk has no stops
    // The stop on the foot that best continues what the feet are doing now.
    const LocoClip from = state_ == LocoState::Starting || state_ == LocoState::Turning ? move_ : gait_;
    const bool known = hasMove(from);
    const LocoClipInfo& f = clip(from);
    const float fromTime = time_[index(from)];
    const LocoFeet a = known ? feetAt(f, fromTime) : LocoFeet{};
    const LocoFeet av = known ? feetVelocity(f, fromTime, rate_[index(from)]) : LocoFeet{};
    float best = 0.f, bestTime = 0.f;
    LocoClip pick = LocoClip::Count;
    for (const bool right : {false, true}) {
        const LocoClip c = locoStop(gait_, right);
        if (!hasMove(c)) continue;
        const LocoClipInfo& s = clip(c);
        // The point whose remaining travel is the capsule's.
        const float t = timeAtPath(s, s.travel - std::min(in.stopDistance, s.travel));
        const float cost = known ? poseCost(a, av, feetAt(s, t), feetVelocity(s, t, 1.f), tuning_.poseVelocity) : 0.f;
        if (pick == LocoClip::Count || cost < best) {
            pick = c;
            best = cost;
            bestTime = t;
        }
    }
    if (pick == LocoClip::Count) return false;
    state_ = LocoState::Stopping;
    move_ = pick;
    rate_[index(pick)] = 0.f;
    enter_ = bestTime;
    return true;
}

bool Locomotion::beginTurn(const LocoInput& in, LocoClip first, LocoClip last, float angle, bool matchFeet, bool pivot) {
    // Short of a pivot, a clip turns the body a degree short of one at most: the steering takes it on round, and a
    // follower replaying the turn sees it is no pivot.
    if (!pivot) angle = clampf(angle, 1.f - tuning_.pivotMin, tuning_.pivotMin - 1.f);
    // The turn nearest the angle asked for, the same way; its other foot turns about as far.
    float nearest = -1.f;
    for (int c = static_cast<int>(first); c <= static_cast<int>(last); ++c) {
        const LocoClipInfo& i = info_[static_cast<std::size_t>(c)];
        if (i.length <= 0.f || i.moveTime <= 0.f || i.turn == 0.f || (i.turn > 0.f) != (angle > 0.f)) continue;
        if (nearest < 0.f || std::fabs(std::fabs(i.turn) - std::fabs(angle)) < std::fabs(nearest - std::fabs(angle))) nearest = std::fabs(i.turn);
    }
    if (nearest < 0.f) return false;
    const bool known = matchFeet && hasMove(gait_);
    const LocoClipInfo& g = clip(gait_);
    const LocoFeet a = known ? feetAt(g, time_[index(gait_)]) : LocoFeet{};
    const LocoFeet av = known ? feetVelocity(g, time_[index(gait_)], rate_[index(gait_)]) : LocoFeet{};
    constexpr int kEntries = 8;
    float best = 0.f, bestTime = 0.f;
    LocoClip pick = LocoClip::Count;
    for (int c = static_cast<int>(first); c <= static_cast<int>(last); ++c) {
        const LocoClipInfo& i = info_[static_cast<std::size_t>(c)];
        if (i.length <= 0.f || i.moveTime <= 0.f || i.turn == 0.f || (i.turn > 0.f) != (angle > 0.f)) continue;
        if (std::fabs(std::fabs(i.turn) - nearest) > 10.f) continue;
        // Entering before the turn (a pivot a little before it: its root brings the body into it), at the point whose
        // feet best continue the gait's.
        const float early = pivot ? tuning_.turnEarly : tuning_.steerEarly, late = pivot ? tuning_.turnLate : tuning_.steerLate;
        const float lo = std::max(i.turnStart - early, 0.f), hi = std::max(i.turnStart - late, 0.f);
        for (int k = 0; k <= (known ? kEntries : 0); ++k) {
            const float t = known ? lo + (hi - lo) * static_cast<float>(k) / kEntries : 0.f;
            float cost = 0.f;
            if (known) {
                const float p = pathSpeed(i, t);
                const float rate = (in.rootMotion && pivot) || p <= 0.f ? 1.f : std::fabs(in.groundSpeed) / p;
                cost = poseCost(a, av, feetAt(i, t), feetVelocity(i, t, rate), tuning_.poseVelocity);
            }
            if (pick == LocoClip::Count || cost < best) {
                pick = static_cast<LocoClip>(c);
                best = cost;
                bestTime = t;
            }
        }
    }
    if (pick == LocoClip::Count) return false;
    state_ = LocoState::Turning;
    move_ = pick;
    // Its yaw from where it enters scaled to the angle asked for.
    warp_ = clampf(angle / (clip(pick).turn - yawAt(clip(pick), bestTime)), tuning_.minWarp, tuning_.maxWarp);
    // A pivot moves the body by its root motion when the caller lets it. A turn on the spot has no distance to match:
    // it plays at its own pace, as if its root moved the body.
    pivot_ = pivot;
    rootDriven_ = (in.rootMotion && pivot) || isLocoStandTurn(pick) || isLocoCrouchTurn(pick);
    rate_[index(pick)] = rootDriven_ ? 1.f : 0.f;
    moved_ = 0.f;
    turnEnter_ = enter_ = bestTime;
    ++turnsBegun_;
    return true;
}

float Locomotion::turnYaw() const {
    if (state_ != LocoState::Turning) return 0.f;
    const LocoClipInfo& m = clip(move_);
    return warp_ * (yawAt(m, time_[index(move_)]) - yawAt(m, turnEnter_));
}

float Locomotion::turnAngle() const {
    if (state_ != LocoState::Turning) return 0.f;
    const LocoClipInfo& m = clip(move_);
    return warp_ * (m.turn - yawAt(m, turnEnter_));
}

void Locomotion::syncGaits(LocoClip from, float t, float speed) {
    const LocoClip g = gait_;
    if (!isLocoGait(g) || !hasMove(from) || !hasMove(g)) return;
    float gaits = 0.f;
    for (const LocoClip c : kGaits) gaits += weight_[index(c)];
    if (gaits >= 0.5f) return; // the gaits still show: keep them going as they are
    // The time in the gait whose feet best continue the clip's.
    const LocoClipInfo& src = clip(from);
    const LocoClipInfo& dst = clip(g);
    const LocoFeet a = feetAt(src, t), av = feetVelocity(src, t, rate_[index(from)]);
    const float rate = dst.groundSpeed > 0.f ? speed / dst.groundSpeed : 1.f;
    float best = 0.f, bestTime = 0.f;
    for (std::size_t k = 0; k < kMoveSamples; ++k) {
        const float tt = dst.moveTime * static_cast<float>(k) / kLastSample;
        const float cost = poseCost(a, av, feetAt(dst, tt), feetVelocity(dst, tt, rate), tuning_.poseVelocity);
        if (k == 0 || cost < best) {
            best = cost;
            bestTime = tt;
        }
    }
    // That time as the shared phase and the gait's stride within its loop (the others start at their first).
    const float u = (bestTime / dst.length - dst.leftPlant) * static_cast<float>(dst.cycles);
    const float whole = std::floor(u);
    phase_ = u - whole;
    stride_.fill(0);
    stride_[index(g)] = ((static_cast<int>(whole) % dst.cycles) + dst.cycles) % dst.cycles;
}

LocoClip Locomotion::moveTarget(const LocoInput& in, float speed, LocoClip& syncFrom, float& syncTime) {
    const float dt = std::max(in.dt, 0.f);
    const bool heading = in.wantSpeed >= tuning_.moveStart;
    const LocoClipInfo& m = clip(move_);
    const float t = time_[index(move_)];
    const auto play = [&](float to, float maxStep) {
        const float next = clampf(to, t, t + maxStep);
        rate_[index(move_)] = dt > 0.f ? (next - t) / dt : 0.f;
        return move_;
    };
    const auto toGaits = [&]() {
        state_ = LocoState::Ground;
        const LocoClip next = groundTarget(speed);
        if (isLocoGait(next)) {
            syncFrom = move_;
            syncTime = t;
        }
        return next;
    };

    switch (state_) {
    case LocoState::Starting: {
        // Braking only just begun may be a dropped frame of input: the start plays on until it lasts.
        const bool brief = braking_ > 0.f && braking_ < tuning_.stopDelay;
        if ((!heading || speed < tuning_.moveStop) && !(brief && speed >= tuning_.moveStop)) {
            if (beginStop(in, speed)) return move_;
            return toGaits();
        }
        moved_ += speed * dt;
        if (pathSpeed(m, t) >= tuning_.startHandoff * clip(gait_).groundSpeed || t >= tuning_.startEnd * m.length) return toGaits();
        return play(timeAtPath(m, moved_), dt * tuning_.moveRate);
    }
    case LocoState::Stopping: {
        if (in.wantSpeed < 0.f || heading) return toGaits();
        if (t >= m.length - tuning_.stopSettle) {
            state_ = LocoState::Ground;
            gait_ = LocoClip::Idle;
            return LocoClip::Idle;
        }
        if (t >= m.moveTime) return play(t + dt, dt); // the settle, in place
        // Once the capsule stands, the clip gets to its stand quickly rather than walking on under it.
        const float step = dt * tuning_.moveRate;
        if (speed < tuning_.moveStop) return play(m.moveTime, std::max(step, (m.moveTime - t) * follow(dt, tuning_.landFade)));
        // At the point whose remaining travel is the capsule's.
        return play(timeAtPath(m, m.travel - std::min(std::max(in.stopDistance, 0.f), m.travel)), step);
    }
    case LocoState::StanceChange: {
        // Getting down or up: moving off cuts it short.
        if (heading || speed >= tuning_.moveStart || t >= m.length - tuning_.crouchSettle) {
            state_ = LocoState::Ground;
            return groundTarget(speed);
        }
        return play(t + dt, dt);
    }
    case LocoState::Turning: {
        if (isLocoStandTurn(move_) || isLocoCrouchTurn(move_)) {
            if (heading || t >= m.turnEnd + tuning_.turnTail || t >= m.length) return toGaits();
            return play(t + dt, dt);
        }
        const bool brief = braking_ > 0.f && braking_ < tuning_.stopDelay;
        if ((!heading || speed < tuning_.moveStop) && !(brief && speed >= tuning_.moveStop)) {
            if (beginStop(in, speed)) return move_;
            return toGaits();
        }
        // A turning start hands over like a start once it has turned; a running turn once it has
        // turned and run on a little.
        const bool done = isLocoReface(move_)
            ? (t >= m.turnEnd && pathSpeed(m, t) >= tuning_.startHandoff * clip(gait_).groundSpeed) || t >= tuning_.startEnd * m.length
            : t >= std::min(m.turnEnd + tuning_.turnTail, m.length);
        if (done) return toGaits();
        if (rootDriven_) return play(t + dt, dt);
        // At the point whose root has come as far as the body since the turn began, but never far from
        // the clip's own pace: a follower keeps the trail's speed where the clip slows into its turn,
        // and racing through it would snap the body round.
        moved_ += speed * dt;
        const float to = timeAtPath(m, pathAt(m, turnEnter_) + moved_);
        return play(std::max(to, t + dt * tuning_.turnRateMin), dt * tuning_.turnRateMax);
    }
    default: break;
    }

    // On the ground: heading off from standing starts the gait it heads for (turning as it sets off
    // when it heads well away from its facing), heading well away from its facing and the way it moves
    // at a run turns, braking stops the gait, and asked to face elsewhere when standing it turns on the
    // spot.
    float gaits = 0.f;
    for (const LocoClip c : kGaits) gaits += weight_[index(c)];
    const float want = wrap180(in.wantYaw - in.yaw);
    if (heading && !crouched_ && !isLocoGait(gait_) && gaits < 0.5f && speed >= tuning_.moveStop) {
        splitGaits(in.wantSpeed);
        const LocoClip g = heaviestGait();
        splitGaits(speed);
        if (in.steer && g != LocoClip::Walk && std::fabs(want) >= tuning_.refaceMin) {
            const LocoClip held = gait_;
            gait_ = g;
            if (beginTurn(in, LocoClip::RefaceLeft90, LocoClip::RefaceRight180, want, false, std::fabs(want) >= tuning_.pivotMin)) return move_;
            gait_ = held;
        }
        const LocoClip s = locoStart(g);
        if (hasMove(s)) {
            state_ = LocoState::Starting;
            move_ = s;
            gait_ = g;
            moved_ = speed * dt;
            rate_[index(s)] = 0.f;
            enter_ = 0.f;
            return s;
        }
    }
    if (beginStop(in, speed)) return move_;
    const float runSpeed = clip(has(LocoClip::Run) ? LocoClip::Run : LocoClip::Walk).groundSpeed;
    // A replayed turn needs a run the body has settled into: fired from a walk start or a half-blended
    // gait its clip's feet and stoop don't match the body's, so the follower rounds the corner instead.
    const bool settled = groundTime_ >= tuning_.replaySettle &&
        weight_[index(LocoClip::Run)] + weight_[index(LocoClip::Sprint)] >= tuning_.replayWeight;
    const float turn = in.turnBy != 0.f ? (settled ? wrap180(in.turnBy) : 0.f) : in.steer ? want : 0.f;
    // How far off its run it is asked to turn: steering, the lesser of how far its heading is from its facing and from
    // the way it moves. A heading swept round smoothly keeps close to the way the body moves, so the facing eases round
    // after it rather than turning in steps; one swung round sharply is far from both.
    const float off = in.turnBy != 0.f || !in.steer ? std::fabs(turn) : std::fmin(std::fabs(want), std::fabs(wrap180(in.wantYaw - in.moveYaw)));
    if (heading && !crouched_ && gait_ != LocoClip::CrouchWalk && isLocoGait(gait_) && gaits >= 0.5f &&
        speed >= tuning_.turnSpeed * runSpeed &&
        ((in.turnBy != 0.f && settled) || off >= tuning_.turnMin) &&
        beginTurn(in, LocoClip::RunTurnLeft45L, LocoClip::RunTurnRight180R, turn, true, off >= tuning_.pivotMin)) {
        return move_;
    }
    const float face = wrap180(in.faceYaw - in.yaw);
    if (in.face && !heading && !isLocoGait(gait_) && speed < tuning_.moveStop && std::fabs(face) >= tuning_.faceMin &&
        (crouched_ ? beginTurn(in, LocoClip::CrouchTurnLeft45, LocoClip::CrouchTurnRight180, face, false, true)
                   : beginTurn(in, LocoClip::StandTurnLeft45, LocoClip::StandTurnRight180, face, false, true))) {
        return move_;
    }
    return groundTarget(speed);
}

void Locomotion::changeStance(bool crouch, float speed) {
    crouch = crouch && has(LocoClip::CrouchIdle) && has(LocoClip::CrouchWalk);
    if (crouch == crouched_) return;
    crouched_ = crouch;
    const LocoClip c = crouch ? LocoClip::CrouchDown : LocoClip::CrouchUp;
    // Partway down and asked back up (or the other way): from the point as far along in the other clip.
    float enter = 0.f;
    if (state_ == LocoState::StanceChange) {
        const float from = clip(move_).length;
        enter = from > 0.f ? (1.f - time_[index(move_)] / from) * clip(c).length : 0.f;
    }
    // A start, stop or turn of the other stance gives way to the gaits.
    if (state_ == LocoState::Starting || state_ == LocoState::Stopping || state_ == LocoState::Turning ||
        state_ == LocoState::StanceChange) {
        state_ = LocoState::Ground;
    }
    float gaits = 0.f;
    for (const LocoClip g : kGaits) gaits += weight_[index(g)];
    if (state_ != LocoState::Ground || isLocoGait(gait_) || gaits >= 0.5f || speed >= tuning_.moveStop || !has(c)) return;
    state_ = LocoState::StanceChange;
    move_ = c;
    rate_[index(c)] = 1.f;
    // Now, for this frame's step, and again once it is the target (a clip fading in starts from 0).
    time_[index(c)] = enter_ = enter;
}

void Locomotion::retarget(LocoClip next) {
    const bool air = state_ == LocoState::Rising || state_ == LocoState::Falling;
    fade_ = air ? tuning_.airFade : state_ == LocoState::Landing ? tuning_.landFade : tuning_.gaitFade;
    if (next == target_) return;
    target_ = next;
    if (isLocoGait(next)) {
        // The gaits fade in together; from nothing, they start at a left plant.
        float gaits = 0.f;
        for (const LocoClip c : kGaits) gaits += weight_[index(c)];
        if (gaits <= 0.f) {
            phase_ = 0.f;
            stride_.fill(0);
        }
        return;
    }
    if (weight_[index(next)] > kMinWeight) return; // still playing: carry on from where it is
    if (next != LocoClip::Idle) time_[index(next)] = 0.f;
}

void Locomotion::advance(float dt, float speed, float leadPhase, float leadStride) {
    const float a = follow(dt, fade_);
    // The gaits fade in and out as one layer, split between them by the speed.
    float gaits = 0.f;
    for (const LocoClip c : kGaits) gaits += weight_[index(c)];
    gaits = gaits * (1.f - a) + (isLocoGait(target_) ? a : 0.f);
    float sum = 0.f;
    for (std::size_t i = 0; i < kLocoClipCount; ++i) {
        const LocoClip c = static_cast<LocoClip>(i);
        float& w = weight_[i];
        w = isLocoGait(c) ? gaits * share_[i] : w * (1.f - a) + (c == target_ ? a : 0.f);
        if (w < kMinWeight) w = 0.f;
        sum += w;
    }
    if (sum <= 0.f) {
        weight_[index(target_)] = sum = 1.f;
    }
    for (float& w : weight_) w /= sum;

    // The gaits step together at the stride rate that keeps the blended planted foot still on the
    // ground: a gait's stride is the distance its foot travels from one left plant to the next.
    gaits = 0.f;
    for (const LocoClip c : kGaits) gaits += weight_[index(c)];
    float stride = 0.f, natural = 0.f;
    for (const LocoClip c : kGaits) {
        const float s = gaits > 0.f ? weight_[index(c)] / gaits : share_[index(c)];
        const LocoClipInfo& i = clip(c);
        if (s <= 0.f || i.length <= 0.f) continue;
        stride += s * i.groundSpeed * i.length / static_cast<float>(i.cycles);
        natural += s * static_cast<float>(i.cycles) / i.length;
    }
    strideLength_ = stride;
    float strideRate = stride > 0.f ? std::min(speed / stride, tuning_.maxRate * natural) : natural;
    // A follower on the ground loop steps at the lead's rate (a longer stride than the lead's steps faster),
    // and eases its phase toward one of the lead's +-1/4 stride (feet half a step apart) by stepping a little
    // faster or slower: faster unless that one is much further. The rate alone would need a standing phase
    // error to hold.
    trim_ = 0.f;
    if (leadPhase >= 0.f && state_ == LocoState::Ground && speed >= tuning_.moveStop) {
        const float feed = leadStride > 0.f && stride > 0.f ? stride / leadStride - 1.f : 0.f;
        const float up = 0.5f * frac(2.f * (leadPhase + 0.25f - phase_)); // ahead to the next one, 0..0.5
        const float err = up <= tuning_.phaseUpBias * (0.5f - up) ? up : up - 0.5f;
        trim_ = clampf(feed + tuning_.phaseGain * err, -tuning_.phaseTrim, tuning_.phaseTrim);
        strideRate *= 1.f + trim_;
    }
    for (const LocoClip c : kGaits) {
        const LocoClipInfo& i = clip(c);
        rate_[index(c)] = i.length > 0.f ? strideRate * i.length / static_cast<float>(i.cycles) : 1.f;
    }

    // Every gait sits at the shared foot phase, each in its own stride of its loop (a loop can hold
    // several strides); the other clips run on their own clocks.
    phase_ += dt * strideRate;
    const int wraps = static_cast<int>(std::floor(phase_));
    phase_ -= static_cast<float>(wraps);
    for (std::size_t i = 0; i < kLocoClipCount; ++i) {
        const LocoClip c = static_cast<LocoClip>(i);
        const float len = info_[i].length;
        if (len <= 0.f) continue;
        float& t = time_[i];
        if (isLocoGait(c)) {
            const int cycles = info_[i].cycles;
            stride_[i] = (stride_[i] + wraps) % cycles;
            t = frac(info_[i].leftPlant + (static_cast<float>(stride_[i]) + phase_) / static_cast<float>(cycles)) * len;
        } else if (weight_[i] > 0.f) {
            t += dt * rate_[i];
            t = isLocoLoop(c) ? frac(t / len) * len : std::min(t, len);
        }
    }

    layerCount_ = 0;
    for (std::size_t i = 0; i < kLocoClipCount; ++i) {
        if (weight_[i] > 0.f) layers_[layerCount_++] = {static_cast<LocoClip>(i), time_[i], weight_[i]};
    }
}

void Locomotion::update(const LocoInput& in) {
    const float dt = std::max(in.dt, 0.f);
    const float speed = std::max(in.groundSpeed, 0.f);

    // Air and landing. A landing roll is asked for on the touchdown alone; a pose with no time passed keeps it.
    if (dt > 0.f) landRoll_ = false;
    if (in.falling) {
        airTime_ += dt;
    }
    if (landTime_ >= 0.f) landTime_ += dt;
    const bool jumped = in.falling && in.verticalSpeed > 0.f;
    const bool offGround = in.falling && (jumped || airTime_ > tuning_.airGrace);
    const bool moving = speed >= tuning_.movingAir;
    switch (state_) {
    case LocoState::Ground:
    case LocoState::Starting:
    case LocoState::Stopping:
    case LocoState::Turning:
    case LocoState::StanceChange:
    case LocoState::Landing:
        if (offGround) {
            jump_ = moving && has(LocoClip::JumpMoving) ? LocoClip::JumpMoving : LocoClip::JumpStart;
            state_ = jumped && has(jump_) ? LocoState::Rising : LocoState::Falling;
        } else if (state_ == LocoState::Landing) {
            if ((speed >= tuning_.moveStart && landTime_ >= tuning_.landHoldMoving) || landTime_ >= 0.9f * clip(land_).length) {
                state_ = LocoState::Ground;
            }
        }
        break;
    case LocoState::Rising:
    case LocoState::Falling:
        if (!in.falling) {
            // A standing landing at speed would slide its feet: without a moving one, run on.
            land_ = moving ? LocoClip::LandMoving : LocoClip::Land;
            const bool hard = airTime_ >= tuning_.landMinAir && has(land_);
            state_ = hard ? LocoState::Landing : LocoState::Ground;
            landTime_ = hard ? 0.f : -1.f;
            landRoll_ = moving && (in.crouch || in.roll) && airTime_ >= tuning_.rollMinAir;
        } else if (state_ == LocoState::Rising) {
            if (time(jump_) >= 0.85f * clip(jump_).length) state_ = LocoState::Falling;
        }
        break;
    }
    if (!in.falling) {
        airTime_ = 0.f;
    }

    const bool braking = !in.falling && in.wantSpeed >= 0.f && in.wantSpeed < tuning_.moveStop;
    braking_ = braking ? braking_ + dt : 0.f;
    groundTime_ = state_ == LocoState::Ground ? groundTime_ + dt : 0.f;
    changeStance(in.crouch, speed);
    const float crouchTo = crouched_ ? 1.f : 0.f;
    crouch_ += (crouchTo - crouch_) * follow(dt, tuning_.crouchFade);
    if (std::fabs(crouchTo - crouch_) < 0.01f) crouch_ = crouchTo;
    splitGaits(speed);
    LocoClip next = target_;
    LocoClip syncFrom = LocoClip::Count;
    float syncTime = 0.f;
    switch (state_) {
    case LocoState::Ground:
    case LocoState::Starting:
    case LocoState::Stopping:
    case LocoState::Turning:
    case LocoState::StanceChange: next = moveTarget(in, speed, syncFrom, syncTime); break;
    case LocoState::Rising: next = jump_; break;
    case LocoState::Falling: next = has(LocoClip::Fall) ? LocoClip::Fall : has(jump_) ? jump_ : gait_; break;
    case LocoState::Landing: next = land_; break;
    }
    retarget(next);
    if (enter_ >= 0.f) {
        time_[index(next)] = enter_;
        enter_ = -1.f;
    }
    if (syncFrom != LocoClip::Count) syncGaits(syncFrom, syncTime, speed);
    turnFrom_ = turnTo_ = state_ == LocoState::Turning ? time_[index(move_)] : 0.f;

    // The moving jump and landing play at the rate their root travel matches the capsule's, within
    // limits: past them it's no longer the same move.
    for (const LocoClip c : {LocoClip::JumpMoving, LocoClip::LandMoving}) {
        const float clipSpeed = clip(c).groundSpeed;
        rate_[index(c)] = clipSpeed > 0.f ? clampf(speed / clipSpeed, tuning_.minLandRate, tuning_.maxLandRate) : 1.f;
    }
    advance(dt, speed, in.leadPhase, in.leadStride);
    if (state_ == LocoState::Turning) turnTo_ = time_[index(move_)];

    // The air pose over the limbs: in the air, and for the arms into the landing (on past a moving body's
    // hand-back to its gait).
    const bool air = state_ == LocoState::Rising || state_ == LocoState::Falling;
    const bool arms = air || (landTime_ >= 0.f && landTime_ < tuning_.landArms);
    const bool posed = has(LocoClip::AirPose);
    const float limbFollow = posed ? follow(dt, tuning_.limbFade) : 1.f;
    limbs_.arms += ((posed && arms ? 1.f : 0.f) - limbs_.arms) * limbFollow;
    limbs_.legs += ((posed && air ? 1.f : 0.f) - limbs_.legs) * limbFollow;
    if (limbs_.arms < kMinWeight) limbs_.arms = 0.f;
    if (limbs_.legs < kMinWeight) limbs_.legs = 0.f;
    limbs_.time = clip(LocoClip::AirPose).length;

    // Facing: outside a pivot, ease toward the way the body moves.
    // The step is only measured over time that passes: a character with root motion poses twice a frame, the
    // second time with no time passed, before its owner takes the step, which that pose would leave at 0. A
    // pivot begun there, or a body no longer steering, still stops it at once.
    const bool steering = in.steer && !(state_ == LocoState::Turning && pivot_) && speed >= tuning_.moveStop;
    if (!steering) yawStep_ = 0.f;
    else if (dt > 0.f) yawStep_ = wrap180(in.moveYaw - in.yaw) * follow(dt, tuning_.faceTime);

    // The body: bank into turns by how fast the body turns and how fast it goes, lead into them with
    // the upper body, and lean forward with speed. In a turn clip the upper body leads by the turn
    // still to come, so it is ahead early and unwinds as the hips come round: the clip's, or a steered
    // body's heading from its facing when the clip only rides its motion.
    const float walkSpeed = clip(LocoClip::Walk).groundSpeed;
    const float runSpeed = has(LocoClip::Run) ? clip(LocoClip::Run).groundSpeed : walkSpeed;
    const float topSpeed = has(LocoClip::Sprint) ? clip(LocoClip::Sprint).groundSpeed : runSpeed;
    const float speedShare = runSpeed > 0.f ? clampf(speed / runSpeed, 0.f, 1.f) : 0.f;
    const bool turningNow = state_ == LocoState::Turning;
    const bool ground = turningNow || state_ == LocoState::Ground || state_ == LocoState::Starting || state_ == LocoState::Stopping ||
        state_ == LocoState::StanceChange;
    const float toCome = !turningNow ? 0.f : in.steer && !pivot_ ? wrap180(in.wantYaw - in.yaw) : turnAngle() - turnYaw();
    const float leanTo = ground ? clampf(in.turnRate * tuning_.leanPerTurn * speedShare, -tuning_.maxLean, tuning_.maxLean) : 0.f;
    const float twistTo = !ground ? 0.f
        : clampf(turningNow ? toCome * tuning_.twistLead : in.turnRate * tuning_.twistPerTurn, -tuning_.maxTwist, tuning_.maxTwist);
    // Crouched, the run's lean and arms go with the standing gaits' share.
    const float stand = 1.f - crouch_;
    const float runShare = runSpeed > walkSpeed ? stand * clampf((speed - walkSpeed) / (runSpeed - walkSpeed), 0.f, 1.f) : 0.f;
    const float sprintShare = topSpeed > runSpeed ? stand * clampf((speed - runSpeed) / (topSpeed - runSpeed), 0.f, 1.f) : 0.f;
    // The gait's lean at the speed, in the air and landing too: the tuck and the dip add to it, so a running
    // jump keeps the run's lean under them and the dip eases out into it.
    float pitchTo = tuning_.runPitch * runShare + tuning_.sprintPitch * sprintShare;
    // Starts lean into the speed still to gain, and stops lean back by how hard they slow, on top of
    // the gait's lean.
    // Only over time that passes: a character with root motion poses twice a frame, the second time with no
    // time passed but its new speed, which would leave the next frame nothing to measure.
    if (dt > 0.f) {
        decel_ += ((lastSpeed_ - speed) / dt - decel_) * (1.f - std::exp(-dt / std::max(tuning_.brakeSmooth, 1e-3f)));
        lastSpeed_ = speed;
    }
    if (ground && runSpeed > 0.f && in.wantSpeed >= 0.f) {
        if (braking) pitchTo += tuning_.brakePitch * clampf(decel_ / std::max(tuning_.brakeDecel, 1.f), 0.f, 1.f);
        else pitchTo += tuning_.startPitch * clampf((in.wantSpeed - speed) / runSpeed, 0.f, 1.f);
    }
    if (air) pitchTo += tuning_.airPitch;
    else if (landTime_ >= 0.f && landTime_ < tuning_.landDip) pitchTo += tuning_.landPitch * (1.f - landTime_ / tuning_.landDip);
    const float b = 1.f - std::exp(-dt * tuning_.bodyResponse);
    lean_ += (leanTo - lean_) * b;
    twist_ += (twistTo - twist_) * b;
    pitch_ += (pitchTo - pitch_) * b;
    // The elbows: held bent at a run, free at the walk and off the ground (the air pose has the arms).
    elbowHold_ += ((ground ? tuning_.elbowHold * runShare : 0.f) - elbowHold_) * b;

    // The head: level against the lean and the bank, and a look into the turn ahead of the chest.
    const float h = 1.f - std::exp(-dt * tuning_.headResponse);
    const float headYawTo = clampf(toCome * tuning_.headLead, -tuning_.maxHeadYaw, tuning_.maxHeadYaw);
    headPitch_ = -pitch_ * tuning_.headLevel;
    headRoll_ = -lean_ * tuning_.headLevelRoll;
    headYaw_ += (headYawTo - headYaw_) * h;
}

} // namespace ftcore
