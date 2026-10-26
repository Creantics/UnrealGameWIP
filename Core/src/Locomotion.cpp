#include "ftcore/Locomotion.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

constexpr LocoClip kGaits[] = {LocoClip::Walk, LocoClip::Run, LocoClip::Sprint};

float frac(float x) { return x - std::floor(x); }

float clampf(float x, float lo, float hi) { return std::min(std::max(x, lo), hi); }

// The share of the remaining distance an exponential follow covers in dt.
float follow(float dt, float timeConstant) { return timeConstant > 0.f ? 1.f - std::exp(-dt / timeConstant) : 1.f; }

struct FootTrack {
    float speed = 0.f; // median planted speed relative to the root, 0 if never planted
    float side = 0.f;  // median planted speed across the way the foot swings
    int plants = 0;
    int firstPlant = -1; // frame of the first plant onset
};

LocoPoint footRel(const GaitFrame* frames, std::size_t k, bool left) {
    const LocoPoint& f = left ? frames[k].left : frames[k].right;
    const LocoPoint& r = frames[k].root;
    return LocoPoint{f.x - r.x, f.y - r.y, f.z - r.z};
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

} // namespace

LocoClipInfo measureGait(const GaitFrame* frames, std::size_t count, float length) {
    LocoClipInfo info;
    info.length = std::max(length, 0.f);
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
}

void Locomotion::reset() {
    weight_.fill(0.f);
    weight_[index(LocoClip::Idle)] = 1.f;
    time_.fill(0.f);
    rate_.fill(1.f);
    jump_ = LocoClip::JumpStart;
    land_ = LocoClip::Land;
    layers_[0] = {LocoClip::Idle, 0.f, 1.f};
    layerCount_ = 1;
    state_ = LocoState::Ground;
    target_ = gait_ = LocoClip::Idle;
    share_.fill(0.f);
    stride_.fill(0);
    fade_ = phase_ = airTime_ = landTime_ = 0.f;
    lean_ = twist_ = pitch_ = 0.f;
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
    // The gaits this body has, slowest first.
    std::array<LocoClip, 3> order{};
    std::size_t n = 0;
    for (const LocoClip c : kGaits) {
        if (!has(c)) continue;
        std::size_t k = n++;
        for (; k > 0 && clip(order[k - 1]).groundSpeed > clip(c).groundSpeed; --k) order[k] = order[k - 1];
        order[k] = c;
    }
    share_.fill(0.f);
    if (n == 0) return;
    std::size_t k = 0;
    while (k + 1 < n && speed > clip(order[k + 1]).groundSpeed) ++k;
    const float lo = clip(order[k]).groundSpeed;
    if (k + 1 == n || speed <= lo) {
        share_[index(order[k])] = 1.f;
        return;
    }
    const float t = (speed - lo) / (clip(order[k + 1]).groundSpeed - lo);
    share_[index(order[k])] = 1.f - t;
    share_[index(order[k + 1])] = t;
}

LocoClip Locomotion::groundTarget(float speed) {
    const bool moving = isLocoGait(gait_);
    if (speed < (moving ? tuning_.moveStop : tuning_.moveStart) || !(has(LocoClip::Walk) || has(LocoClip::Run) || has(LocoClip::Sprint))) {
        gait_ = LocoClip::Idle;
        return gait_;
    }
    gait_ = LocoClip::Walk;
    for (const LocoClip c : kGaits) {
        if (share_[index(c)] > share_[index(gait_)]) gait_ = c;
    }
    return gait_;
}

void Locomotion::retarget(LocoClip next) {
    fade_ = state_ == LocoState::Ground ? tuning_.gaitFade : state_ == LocoState::Landing ? tuning_.landFade : tuning_.airFade;
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

void Locomotion::advance(float dt, float speed) {
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
    const float strideRate = stride > 0.f ? std::min(speed / stride, tuning_.maxRate * natural) : natural;
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

    // Air and landing.
    if (in.falling) {
        airTime_ += dt;
    }
    const bool jumped = in.falling && in.verticalSpeed > 0.f;
    const bool offGround = in.falling && (jumped || airTime_ > tuning_.airGrace);
    const bool moving = speed >= tuning_.movingAir;
    switch (state_) {
    case LocoState::Ground:
    case LocoState::Landing:
        if (offGround) {
            jump_ = moving && has(LocoClip::JumpMoving) ? LocoClip::JumpMoving : LocoClip::JumpStart;
            state_ = jumped && has(jump_) ? LocoState::Rising : LocoState::Falling;
        } else if (state_ == LocoState::Landing) {
            landTime_ += dt;
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
            landTime_ = 0.f;
        } else if (state_ == LocoState::Rising) {
            if (time(jump_) >= 0.85f * clip(jump_).length) state_ = LocoState::Falling;
        }
        break;
    }
    if (!in.falling) {
        airTime_ = 0.f;
    }

    splitGaits(speed);
    LocoClip next = target_;
    switch (state_) {
    case LocoState::Ground: next = groundTarget(speed); break;
    case LocoState::Rising: next = jump_; break;
    case LocoState::Falling: next = has(LocoClip::Fall) ? LocoClip::Fall : has(jump_) ? jump_ : gait_; break;
    case LocoState::Landing: next = land_; break;
    }
    retarget(next);

    // The moving jump and landing play at the rate their root travel matches the capsule's, within
    // limits: past them it's no longer the same move.
    for (const LocoClip c : {LocoClip::JumpMoving, LocoClip::LandMoving}) {
        const float clipSpeed = clip(c).groundSpeed;
        rate_[index(c)] = clipSpeed > 0.f ? clampf(speed / clipSpeed, tuning_.minLandRate, tuning_.maxLandRate) : 1.f;
    }
    advance(dt, speed);

    // The body: bank and lead with the upper body into turns, lean forward with speed.
    const float walkSpeed = clip(LocoClip::Walk).groundSpeed;
    const float runSpeed = has(LocoClip::Run) ? clip(LocoClip::Run).groundSpeed : walkSpeed;
    const float topSpeed = has(LocoClip::Sprint) ? clip(LocoClip::Sprint).groundSpeed : runSpeed;
    const float speedShare = runSpeed > 0.f ? clampf(speed / runSpeed, 0.f, 1.f) : 0.f;
    const bool ground = state_ == LocoState::Ground;
    const float leanTo = ground ? clampf(in.turnRate * tuning_.leanPerTurn * speedShare, -tuning_.maxLean, tuning_.maxLean) : 0.f;
    const float twistTo = ground ? clampf(in.turnRate * tuning_.twistPerTurn, -tuning_.maxTwist, tuning_.maxTwist) : 0.f;
    const float runShare = topSpeed > walkSpeed ? clampf((speed - walkSpeed) / (topSpeed - walkSpeed), 0.f, 1.f) : 0.f;
    const float pitchTo = ground ? tuning_.runPitch * runShare : 0.f;
    const float b = 1.f - std::exp(-dt * tuning_.bodyResponse);
    lean_ += (leanTo - lean_) * b;
    twist_ += (twistTo - twist_) * b;
    pitch_ += (pitchTo - pitch_) * b;
}

} // namespace ftcore
