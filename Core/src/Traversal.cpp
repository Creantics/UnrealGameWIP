#include "ftcore/Traversal.h"

#include "FootRest.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ftcore {

namespace {

using detail::lastFootAtRest;

TravPoint lerp(const TravPoint& a, const TravPoint& b, float u) { return {a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u}; }

float smooth(float u) {
    u = std::clamp(u, 0.f, 1.f);
    return u * u * (3.f - 2.f * u);
}

// Eases from a at time t0 to b at t1.
TravPoint ease(const TravPoint& a, const TravPoint& b, float t, float t0, float t1) { return lerp(a, b, t1 > t0 ? smooth((t - t0) / (t1 - t0)) : 1.f); }

// The root's entry speed and its path, both from frame `from` on (count >= 3, from < count - 1).
void measureRootPath(const TraversalFrame* f, std::size_t count, float dt, TraversalClipInfo& info, std::size_t from = 0) {
    const TravPoint& root0 = f[from].root;
    const std::size_t k = std::clamp<std::size_t>(static_cast<std::size_t>(std::lround(kEntryTime / dt)), 1, count - 1 - from);
    info.entrySpeed = (f[from + k].root.x - root0.x) / (static_cast<float>(k) * dt);
    for (std::size_t s = 0; s < kTraversalSamples; ++s) {
        const float u = static_cast<float>(from) + static_cast<float>(s) / static_cast<float>(kTraversalSamples - 1) * static_cast<float>(count - 1 - from);
        const std::size_t i = std::min(static_cast<std::size_t>(u), count - 2);
        const TravPoint p = lerp(f[i].root, f[i + 1].root, u - static_cast<float>(i));
        info.path[s] = {p.x - root0.x, p.y - root0.y, p.z - root0.z};
    }
}

// What entering clip c costs a body at `speed` with `foot` planted: its speed against the clip's, and
// a take-off from the other foot.
float entryCost(const TraversalTuning& t, const TraversalClipInfo& c, float speed, int foot) {
    float cost = std::fabs(speed - c.entrySpeed) * t.speedCost;
    if (foot >= 0 && c.takeoffFoot >= 0 && foot != c.takeoffFoot) cost += t.footCost;
    return cost;
}

// The play rate at the entry: the body's speed over the clip's, within the tuning's limits.
float entryRate(const TraversalTuning& t, const TraversalClipInfo& c, float speed) {
    return c.entrySpeed > 0.f ? std::max(t.minEntryRate, std::min(speed / c.entrySpeed, t.maxEntryRate)) : 1.f;
}

// A catch's root before the catch: a cubic (Hermite) from where the body was, at its velocity, to the catch,
// at the velocity the clip's root has just after it.
TravPoint catchApproach(const TraversalClipInfo& c, const TraversalPlan& p, float t) {
    const float lead = c.contact - p.start;
    if (lead <= 0.f) return p.atContact;
    const float step = (c.length - c.touchdown) / static_cast<float>(kTraversalSamples - 1);
    const TravPoint& a = c.path[0];
    const TravPoint& b = c.path[1];
    const TravPoint out = step > 0.f ? TravPoint{(b.x - a.x) / step, (b.y - a.y) / step, (b.z - a.z) / step} : TravPoint{};
    const float s = std::clamp((t - p.start) / lead, 0.f, 1.f), s2 = s * s, s3 = s2 * s;
    const float h10 = s3 - 2.f * s2 + s, h01 = 3.f * s2 - 2.f * s3, h11 = s3 - s2;
    auto at = [&](float in, float to, float v) { return h10 * lead * in + h01 * to + h11 * lead * v; };
    const TravPoint& in = p.entryVelocity;
    return {at(in.x, p.atContact.x, out.x), at(in.y, p.atContact.y, out.y), at(in.z, p.atContact.z, out.z)};
}

// When a flight going up at vz (down below 0) under gravity g (pulling down) is first at height z, from `from`
// on; when it never is, when it comes nearest: at the top of its flight, or at `from` once past it.
float meetTime(float vz, float g, float z, float from) {
    if (g <= 0.f) return from;
    const float disc = vz * vz - 2.f * g * z;
    if (disc < 0.f) return std::max(vz / g, from);
    const float root = std::sqrt(disc);
    const float up = (vz - root) / g, down = (vz + root) / g;
    return up >= from ? up : down >= from ? down : from;
}

} // namespace

TraversalClipInfo measureTraversal(const TraversalFrame* frames, std::size_t count, float length) {
    TraversalClipInfo info;
    count = std::min(count, kMaxTraversalFrames);
    if (!frames || count < 3 || length <= 0.f) return info;
    const float dt = length / static_cast<float>(count - 1);
    const TravPoint& root0 = frames[0].root;
    auto up = [&](std::size_t i) { return frames[i].root.z - root0.z; };

    float peak = 0.f;
    for (std::size_t i = 0; i < count; ++i) peak = std::max(peak, up(i));
    const float rise = up(count - 1);
    if (peak < kMinObstacle || rise < -kMaxClipDrop) return info;

    std::size_t contact = 0;
    while (up(contact) <= kRiseShare * peak) ++contact;
    std::size_t top = contact;
    while (up(top) < kTopShare * peak) ++top;
    std::size_t leave = top;
    while (leave < count && up(leave) >= kTopShare * peak) ++leave;
    std::size_t land = top;
    while (land < count && std::fabs(up(land) - rise) >= kSettle) ++land;
    if (land == count) return info;

    info.takeoffFoot = lastFootAtRest(frames, count, 0, contact, dt).foot;
    info.length = length;
    info.contact = static_cast<float>(contact) * dt;
    info.land = static_cast<float>(land) * dt;
    info.ledgeAhead = frames[top].root.x - root0.x;
    info.ledgeHeight = peak;
    info.rise = rise;
    info.move = rise > 0.5f * peak ? TraversalMove::Up : TraversalMove::Over;
    if (info.move == TraversalMove::Over && leave < count) {
        info.leave = static_cast<float>(leave) * dt;
        info.topDepth = frames[leave].root.x - frames[top].root.x;
    } else {
        info.leave = info.land;
    }
    measureRootPath(frames, count, dt, info);
    return info;
}

TraversalClipInfo measureRoll(const TraversalFrame* frames, std::size_t count, float length) {
    TraversalClipInfo info;
    count = std::min(count, kMaxTraversalFrames);
    if (!frames || count < 3 || length <= 0.f) return info;
    const float dt = length / static_cast<float>(count - 1);
    // The touchdown: from here on the root stays on the floor it ends on (a land roll comes down onto it).
    const float floorZ = frames[count - 1].root.z;
    auto off = [&](std::size_t i) { return std::fabs(frames[i].root.z - floorZ); };
    std::size_t from = count - 1;
    while (from > 0 && off(from - 1) < kMinObstacle) --from;
    if (from > 0)
        while (off(from) >= kSettle) ++from;
    std::size_t bottom = from;
    for (std::size_t i = from; i < count; ++i)
        if (frames[i].pelvis.z < frames[bottom].pelvis.z) bottom = i;
    const float low = frames[bottom].pelvis.z;
    auto above = [&](std::size_t i) { return frames[i].pelvis.z - low; };
    const float startHeight = above(from), endHeight = above(count - 1);
    if (std::min(startHeight, endHeight) < kMinRollDip) return info;

    std::size_t contact = from; // the tuck
    while (above(contact) > kRiseShare * startHeight) ++contact;
    std::size_t land = bottom; // back up
    while (above(land) < kTopShare * endHeight) ++land;

    info.takeoffFoot = lastFootAtRest(frames, count, from, contact, dt).foot;
    info.length = length;
    info.move = TraversalMove::Roll;
    info.touchdown = static_cast<float>(from) * dt;
    info.contact = static_cast<float>(contact) * dt;
    info.leave = info.land = static_cast<float>(land) * dt;
    info.rise = floorZ - frames[from].root.z;
    info.dip = std::min(startHeight, endHeight);
    measureRootPath(frames, count, dt, info, from);
    return info;
}

TraversalClipInfo measureCatch(const TraversalFrame* frames, std::size_t count, float length) {
    TraversalClipInfo info;
    count = std::min(count, kMaxTraversalFrames);
    if (!frames || count < 3 || length <= 0.f) return info;
    const float dt = length / static_cast<float>(count - 1);
    std::size_t contact = 0;
    for (std::size_t i = 1; i < count; ++i)
        if (frames[i].root.z < frames[contact].root.z) contact = i;
    const TravPoint& caught = frames[contact].root;
    if (frames[0].root.z - caught.z < kMinObstacle || contact + 2 >= count) return info;
    const float top = frames[count - 1].root.z;
    std::size_t land = count - 1; // on the top from here on
    while (land > contact && std::fabs(frames[land - 1].root.z - top) < kSettle) --land;

    info.length = length;
    info.move = TraversalMove::Catch;
    info.contact = info.touchdown = static_cast<float>(contact) * dt;
    info.leave = info.land = static_cast<float>(land) * dt;
    info.ledgeAhead = -caught.x;
    info.ledgeHeight = info.rise = top - caught.z;
    if (land < count - 1) info.exitSpeed = (frames[count - 1].root.x - frames[land].root.x) / (static_cast<float>(count - 1 - land) * dt);
    measureRootPath(frames, count, dt, info, contact);
    return info;
}

TravPoint traversalPath(const TraversalClipInfo& info, float t) {
    if (info.length <= info.touchdown) return {}; // not measured
    const float u = std::clamp((t - info.touchdown) / (info.length - info.touchdown), 0.f, 1.f) * static_cast<float>(kTraversalSamples - 1);
    const std::size_t i = std::min(static_cast<std::size_t>(u), kTraversalSamples - 2);
    return lerp(info.path[i], info.path[i + 1], u - static_cast<float>(i));
}

bool TraversalSet::setClip(std::size_t i, const TraversalClipInfo& info) {
    if (i >= kMaxTraversalClips) return false;
    clips_[i] = info;
    return true;
}

TraversalPlan TraversalSet::plan(const TraversalObstacle& o, float speed, int foot) const {
    TraversalPlan p;
    const TraversalTuning& t = tuning_;
    const bool up = o.depth >= t.standDepth;
    if (!up && (!o.backFloor || o.backHeight < -t.maxDrop)) return p; // nowhere to land
    float best = std::numeric_limits<float>::infinity();
    bool tooFar = false;
    for (std::size_t i = 0; i < kMaxTraversalClips; ++i) {
        const TraversalClipInfo& c = clips_[i];
        if (c.length <= 0.f || c.move == TraversalMove::Roll || c.move == TraversalMove::Catch || (c.move == TraversalMove::Up) != up)
            continue;
        const float dh = std::fabs(o.height - c.ledgeHeight);
        if (dh > std::max(t.minHeightSlack, t.heightSlack * c.ledgeHeight)) continue;
        // The edge's distance ahead of the root from clip time s on.
        auto ahead = [&](float s) { return c.ledgeAhead - traversalPath(c, s).x; };
        const float latest = std::max(0.f, c.contact - t.minLead);
        float start = 0.f;
        if (o.distance > ahead(0.f) + t.maxPush) {
            tooFar = true;
            continue;
        }
        if (o.distance < ahead(0.f)) {
            if (ahead(latest) - o.distance > t.maxPull) continue; // too close for this clip
            if (o.distance <= ahead(latest)) {
                start = latest;
            } else {
                float lo = 0.f, hi = latest; // ahead(lo) > distance > ahead(hi)
                for (int k = 0; k < 24; ++k) {
                    const float mid = 0.5f * (lo + hi);
                    (ahead(mid) > o.distance ? lo : hi) = mid;
                }
                start = hi;
            }
        }
        const float cost = dh * t.heightCost + entryCost(t, c, speed, foot);
        if (cost < best) {
            best = cost;
            p.clip = static_cast<int>(i);
            p.start = start;
        }
    }
    if (!p.ok()) {
        p.tooFar = tooFar;
        return p;
    }
    const TraversalClipInfo& c = clips_[static_cast<std::size_t>(p.clip)];
    p.entryRate = entryRate(t, c, speed);
    const float push = o.distance - (c.ledgeAhead - traversalPath(c, p.start).x);
    p.atContact = {push, 0.f, o.height - c.ledgeHeight};
    if (up) {
        p.atLeave = p.atLand = p.atContact; // stands on top as far in as the clip does
    } else {
        const float across = push + std::max(0.f, o.depth - c.topDepth);
        p.atLeave = {across, 0.f, p.atContact.z};
        p.atLand = {across, 0.f, o.backHeight - c.rise};
    }
    return p;
}

TraversalPlan TraversalSet::planRoll(float speed, int foot) const {
    TraversalPlan p;
    float best = std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < kMaxTraversalClips; ++i) {
        const TraversalClipInfo& c = clips_[i];
        if (c.length <= 0.f || c.move != TraversalMove::Roll) continue;
        const float cost = entryCost(tuning_, c, speed, foot);
        if (cost < best) {
            best = cost;
            p.clip = static_cast<int>(i);
        }
    }
    if (p.ok()) {
        const TraversalClipInfo& c = clips_[static_cast<std::size_t>(p.clip)];
        p.start = c.touchdown;
        p.entryRate = entryRate(tuning_, c, speed);
    }
    return p;
}

TraversalPlan TraversalSet::planCatch(const TraversalObstacle& o, const TravPoint& velocity, float gravity, float speed) const {
    TraversalPlan p;
    const TraversalTuning& t = tuning_;
    if (o.depth < t.standDepth) return p; // nothing to climb onto
    float best = std::numeric_limits<float>::infinity();
    bool tooFar = false;
    for (std::size_t i = 0; i < kMaxTraversalClips; ++i) {
        const TraversalClipInfo& c = clips_[i];
        if (c.length <= 0.f || c.move != TraversalMove::Catch) continue;
        // Where the root must be at the catch for the clip's edge to be the real one.
        const float x = o.distance - c.ledgeAhead, z = o.height - c.ledgeHeight;
        // When: as the body gets there, if it closes in on it (covers maxCatchMiss within the longest lead);
        // else as its flight meets that height, or comes nearest it. One still to come past the longest
        // lead is too far yet.
        const float longest = std::min(t.maxCatchLead, c.contact);
        const float shortest = std::min(t.minCatchLead, longest);
        const bool closing = velocity.x * longest > t.maxCatchMiss;
        const float when = closing ? x / velocity.x : meetTime(velocity.z, gravity, z, shortest);
        if (when > longest) {
            tooFar = true;
            continue;
        }
        const float lead = std::max(when, shortest);
        // Its miss: how far the catch is from where the body's flight takes it by then. A flight that has
        // the feet over the top by then lands on it rather than catching it.
        const float flight = velocity.z * lead - 0.5f * gravity * lead * lead;
        const float missX = x - velocity.x * lead, missZ = z - flight;
        if (missX > t.maxCatchMiss) tooFar = true;
        if (flight >= o.height || std::fabs(missX) > t.maxCatchMiss || std::fabs(missZ) > t.maxCatchMiss) continue;
        const float cost = std::sqrt(missX * missX + missZ * missZ) * t.catchMissCost + std::fabs(speed - c.exitSpeed) * t.speedCost;
        if (cost < best) {
            best = cost;
            p.clip = static_cast<int>(i);
            p.start = c.contact - lead;
            p.atContact = {x, velocity.y * lead, z}; // across, it goes on as it was going
        }
    }
    if (!p.ok()) {
        p.tooFar = tooFar;
        return p;
    }
    p.atLeave = p.atLand = p.atContact;
    p.entryVelocity = velocity;
    return p;
}

float TraversalSet::exitTime(const TraversalPlan& p) const {
    if (!p.ok()) return 0.f;
    const TraversalClipInfo& c = clips_[static_cast<std::size_t>(p.clip)];
    return std::clamp(c.land + tuning_.exitAfter, p.start, c.length);
}

float TraversalSet::rate(const TraversalPlan& p, float t) const {
    if (!p.ok()) return 1.f;
    const TraversalClipInfo& c = clips_[static_cast<std::size_t>(p.clip)];
    if (t >= c.contact || c.contact <= p.start) return 1.f;
    return p.entryRate + (1.f - p.entryRate) * smooth((t - p.start) / (c.contact - p.start));
}

TravPoint TraversalSet::rootAt(const TraversalPlan& p, float t) const {
    if (!p.ok()) return {};
    const TraversalClipInfo& c = clips_[static_cast<std::size_t>(p.clip)];
    if (c.move == TraversalMove::Catch) {
        if (t < c.contact) return catchApproach(c, p, t);
        const TravPoint at = traversalPath(c, t);
        return {p.atContact.x + at.x, p.atContact.y + at.y, p.atContact.z + at.z};
    }
    const TravPoint from = traversalPath(c, p.start);
    const TravPoint at = traversalPath(c, t);
    TravPoint off;
    if (t <= c.contact) off = ease({}, p.atContact, t, p.start, c.contact);
    else if (t <= c.leave) off = ease(p.atContact, p.atLeave, t, c.contact, c.leave);
    else if (t <= c.land) off = ease(p.atLeave, p.atLand, t, c.leave, c.land);
    else off = p.atLand;
    return {at.x - from.x + off.x, at.y - from.y + off.y, at.z - from.z + off.z};
}

} // namespace ftcore
