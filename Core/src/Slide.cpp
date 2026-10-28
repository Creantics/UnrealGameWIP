#include "ftcore/Slide.h"

#include "FootRest.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ftcore {

namespace {

float smooth(float u) {
    u = std::clamp(u, 0.f, 1.f);
    return u * u * (3.f - 2.f * u);
}

std::size_t slot(SlideOut o) { return static_cast<std::size_t>(o); }

// The get-ups to try for each one asked for, nearest first.
constexpr SlideOut kOutOrder[kSlideOuts][kSlideOuts] = {
    {SlideOut::IdleStand, SlideOut::IdleCrouch, SlideOut::Walk, SlideOut::Run, SlideOut::CrouchWalk, SlideOut::Sprint},
    {SlideOut::IdleCrouch, SlideOut::CrouchWalk, SlideOut::IdleStand, SlideOut::Walk, SlideOut::Run, SlideOut::Sprint},
    {SlideOut::Walk, SlideOut::Run, SlideOut::CrouchWalk, SlideOut::Sprint, SlideOut::IdleStand, SlideOut::IdleCrouch},
    {SlideOut::Run, SlideOut::Walk, SlideOut::Sprint, SlideOut::CrouchWalk, SlideOut::IdleStand, SlideOut::IdleCrouch},
    {SlideOut::Sprint, SlideOut::Run, SlideOut::Walk, SlideOut::CrouchWalk, SlideOut::IdleStand, SlideOut::IdleCrouch},
    {SlideOut::CrouchWalk, SlideOut::IdleCrouch, SlideOut::Walk, SlideOut::Run, SlideOut::Sprint, SlideOut::IdleStand},
};

// The root's ground speed at frame i, over two frames either side of it (fewer at the ends).
float groundSpeed(const TraversalFrame* f, std::size_t count, std::size_t i, float dt) {
    const std::size_t a = i >= 2 ? i - 2 : 0;
    const std::size_t b = std::min(i + 2, count - 1);
    return detail::groundDist(f[a].root, f[b].root) / (static_cast<float>(b - a) * dt);
}

} // namespace

SlideIntoInfo measureSlideInto(const TraversalFrame* frames, std::size_t count, float length) {
    SlideIntoInfo info;
    count = std::min(count, kMaxTraversalFrames);
    if (!frames || count < 3 || length <= 0.f) return info;
    const float dt = length / static_cast<float>(count - 1);
    const float stand = frames[0].pelvis.z, low = frames[count - 1].pelvis.z;
    if (stand - low < kMinSlideDrop) return info;

    std::size_t down = 0;
    while (frames[down].pelvis.z > low + kSlideDownShare * (stand - low)) ++down;
    const detail::FootRest rest = detail::lastFootAtRest(frames, count, 0, down, dt);
    const std::size_t from = rest.first; // frame 0 with no foot at rest before the down
    const std::size_t k = std::clamp<std::size_t>(static_cast<std::size_t>(std::lround(kEntryTime / dt)), 1, count - 1 - from);

    info.length = length;
    info.takeoff = static_cast<float>(from) * dt;
    info.down = static_cast<float>(down) * dt;
    info.entrySpeed = (frames[from + k].root.x - frames[from].root.x) / (static_cast<float>(k) * dt);
    if (down + 1 < count) info.slideSpeed = (frames[count - 1].root.x - frames[down].root.x) / (static_cast<float>(count - 1 - down) * dt);
    info.takeoffFoot = rest.foot;
    return info;
}

SlideOutInfo measureSlideOut(const TraversalFrame* frames, std::size_t count, float length) {
    SlideOutInfo info;
    count = std::min(count, kMaxTraversalFrames);
    if (!frames || count < 3 || length <= 0.f) return info;
    const float dt = length / static_cast<float>(count - 1);
    const float low = frames[0].pelvis.z, top = frames[count - 1].pelvis.z;
    if (top - low < kMinSlideDrop) return info;

    std::size_t up = 0;
    while (frames[up].pelvis.z < low + kTopShare * (top - low)) ++up;
    // The first foot to come to rest on the floor the clip ends on.
    const detail::Part balls[2] = {detail::ballLeft, detail::ballRight};
    const float floorZ = std::min(frames[count - 1].ballLeft.z, frames[count - 1].ballRight.z);
    const std::size_t rest = detail::restFrames(dt);
    std::size_t plant = count;
    for (std::size_t i = 0; i < count && plant == count; ++i)
        for (const detail::Part part : balls)
            if (part(frames[i]).z - floorZ < kFootRest && detail::restsFrom(frames, count, part, i, rest, dt)) plant = i;
    if (plant == count) plant = up;
    // Settled from the first frame, once up, from which the pelvis stays at its end height and the root still.
    std::size_t settle = count;
    while (settle > up && std::fabs(frames[settle - 1].pelvis.z - top) < kSettle && groundSpeed(frames, count, settle - 1, dt) < kSlideStill) --settle;
    const std::size_t k = std::clamp<std::size_t>(static_cast<std::size_t>(std::lround(kEntryTime / dt)), 1, count - 1);

    info.length = length;
    info.plant = static_cast<float>(plant) * dt;
    info.up = static_cast<float>(up) * dt;
    info.settle = settle < count ? static_cast<float>(settle) * dt : -1.f;
    info.endSpeed = detail::groundDist(frames[count - 1].root, frames[count - 1 - k].root) / (static_cast<float>(k) * dt);
    for (std::size_t s = 0; s < kSlideSpeedSamples; ++s) {
        const float u = static_cast<float>(s) / static_cast<float>(kSlideSpeedSamples - 1) * static_cast<float>(count - 1);
        const std::size_t i = std::min(static_cast<std::size_t>(u), count - 2);
        const float a = groundSpeed(frames, count, i, dt), b = groundSpeed(frames, count, i + 1, dt);
        info.speed[s] = a + (b - a) * (u - static_cast<float>(i));
    }
    return info;
}

float slideOutSpeed(const SlideOutInfo& info, float t) {
    if (info.length <= 0.f) return 0.f;
    const float u = std::clamp(t / info.length, 0.f, 1.f) * static_cast<float>(kSlideSpeedSamples - 1);
    const std::size_t i = std::min(static_cast<std::size_t>(u), kSlideSpeedSamples - 2);
    return info.speed[i] + (info.speed[i + 1] - info.speed[i]) * (u - static_cast<float>(i));
}

bool SlideSet::setInto(std::size_t i, const SlideIntoInfo& info) {
    if (i >= kSlideIntos) return false;
    intos_[i] = info;
    return true;
}

SlideState SlideSet::start(float speed, int foot) const {
    SlideState s;
    const SlideTuning& t = tuning_;
    if (speed < t.minEntrySpeed) return s;
    float best = std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < kSlideIntos; ++i) {
        const SlideIntoInfo& c = intos_[i];
        if (c.length <= 0.f) continue;
        float cost = std::fabs(speed - c.entrySpeed) * t.speedCost;
        if (foot >= 0 && c.takeoffFoot >= 0 && foot != c.takeoffFoot) cost += t.footCost;
        if (cost < best) {
            best = cost;
            s.into = static_cast<int>(i);
        }
    }
    if (s.into < 0) return s;
    const SlideIntoInfo& c = intos_[static_cast<std::size_t>(s.into)];
    s.phase = SlidePhase::Into;
    s.clip = s.into;
    s.clipTime = s.start = c.takeoff;
    s.entryRate = c.entrySpeed > 0.f ? std::max(t.minEntryRate, std::min(speed / c.entrySpeed, t.maxEntryRate)) : 1.f;
    s.speed = speed;
    return s;
}

float SlideSet::rate(const SlideState& s) const {
    if (!s.sliding() || s.into < 0 || s.clip != s.into) return 1.f;
    const SlideIntoInfo& c = intos_[static_cast<std::size_t>(s.into)];
    if (s.clipTime >= c.down || c.down <= s.start) return 1.f;
    return s.entryRate + (1.f - s.entryRate) * smooth((s.clipTime - s.start) / (c.down - s.start));
}

SlideOut SlideSet::chooseOut(const SlideInput& in) const {
    SlideOut want = SlideOut::Run;
    if (in.wantSpeed <= 0.f) want = in.crouch ? SlideOut::IdleCrouch : SlideOut::IdleStand;
    else if (in.sprint) want = SlideOut::Sprint;
    else if (in.crouch) want = SlideOut::CrouchWalk;
    else if (std::fabs(outs_[slot(SlideOut::Walk)].endSpeed - in.wantSpeed) <= std::fabs(outs_[slot(SlideOut::Run)].endSpeed - in.wantSpeed)) want = SlideOut::Walk;
    for (const SlideOut o : kOutOrder[slot(want)])
        if (outs_[slot(o)].length > 0.f) return o;
    return want;
}

void SlideSet::beginOut(SlideState& s, const SlideInput& in) const {
    s.out = chooseOut(in);
    s.outFrom = s.speed;
    if (outs_[slot(s.out)].length <= 0.f) { // no get-up at all: hand straight back
        s.phase = SlidePhase::Done;
        return;
    }
    s.phase = SlidePhase::Out;
    s.clip = slideOutClip(s.out);
    s.clipTime = 0.f;
    s.loop = false;
}

void SlideSet::step(SlideState& s, const SlideInput& in, float dt) const {
    if (!s.sliding()) return;
    const SlideTuning& t = tuning_;
    if (s.phase == SlidePhase::Out) {
        const SlideOutInfo& o = outs_[slot(s.out)];
        s.yawStep = 0.f;
        s.clipTime = std::min(s.clipTime + dt, o.length);
        s.speed = s.clipTime < o.plant ? s.outFrom + (slideOutSpeed(o, o.plant) - s.outFrom) * smooth(s.clipTime / o.plant) : slideOutSpeed(o, s.clipTime);
        float exit = o.settle >= 0.f ? o.settle : o.length;
        if (in.wantSpeed > 0.f || o.settle < 0.f) exit = std::min(exit, o.up + t.exitAfter);
        if (s.clipTime >= std::min(exit, o.length)) s.phase = SlidePhase::Done;
        return;
    }

    const float turnLimit = t.steerRate * dt;
    s.yawStep = std::clamp(in.turn, -turnLimit, turnLimit);
    s.getUp = s.getUp || !in.crouch || in.blocked;
    if (in.blocked) s.speed = 0.f;
    if (s.phase == SlidePhase::Down) s.speed = std::clamp(s.speed - dt * (t.friction + t.gravity * in.slope), 0.f, std::max(t.maxSpeed, s.speed));

    const SlideIntoInfo& c = intos_[static_cast<std::size_t>(s.into)];
    if (s.clip == s.into) {
        s.clipTime += dt * rate(s);
        if (s.clipTime >= c.down) s.phase = SlidePhase::Down;
        if (s.clipTime >= c.length) {
            if (loop_ > 0.f) {
                s.clip = kSlideLoop;
                s.loop = true;
                s.clipTime = std::fmod(s.clipTime - c.length, loop_);
            } else {
                s.clipTime = c.length; // its last pose holds
            }
        }
    } else if (loop_ > 0.f) {
        s.clipTime = std::fmod(s.clipTime + dt, loop_);
    }
    if (s.phase == SlidePhase::Down && (s.getUp || s.speed < t.outSpeed)) beginOut(s, in);
}

} // namespace ftcore
