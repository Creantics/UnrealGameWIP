#include "ftcore/Traversal.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ftcore {

namespace {

using Part = const TravPoint& (*)(const TraversalFrame&);
const TravPoint& ballLeft(const TraversalFrame& f) { return f.ballLeft; }
const TravPoint& ballRight(const TraversalFrame& f) { return f.ballRight; }

float dist(const TravPoint& a, const TravPoint& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }

TravPoint lerp(const TravPoint& a, const TravPoint& b, float u) { return {a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u}; }

float smooth(float u) {
    u = std::clamp(u, 0.f, 1.f);
    return u * u * (3.f - 2.f * u);
}

// Eases from a at time t0 to b at t1.
TravPoint ease(const TravPoint& a, const TravPoint& b, float t, float t0, float t1) { return lerp(a, b, t1 > t0 ? smooth((t - t0) / (t1 - t0)) : 1.f); }

// At rest from frame i: every step over the next n frames slower than kRestSpeed.
bool restsFrom(const TraversalFrame* f, std::size_t count, Part part, std::size_t i, std::size_t n, float dt) {
    if (i + n >= count) return false;
    for (std::size_t k = i; k < i + n; ++k)
        if (dist(part(f[k]), part(f[k + 1])) / dt >= kRestSpeed) return false;
    return true;
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

    // The take-off foot: the last at rest on the starting floor before the contact.
    const Part balls[2] = {ballLeft, ballRight};
    const float floor0 = std::min(frames[0].ballLeft.z, frames[0].ballRight.z);
    const std::size_t rest = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(kRestTime / dt - 1e-4f)));
    for (std::size_t i = contact; i-- > 0 && info.takeoffFoot < 0;)
        for (int b = 0; b < 2; ++b)
            if (balls[b](frames[i]).z - floor0 < kFootRest && restsFrom(frames, count, balls[b], i, rest, dt)) {
                info.takeoffFoot = b;
                break;
            }

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
    const std::size_t k = std::clamp<std::size_t>(static_cast<std::size_t>(std::lround(kEntryTime / dt)), 1, count - 1);
    info.entrySpeed = (frames[k].root.x - root0.x) / (static_cast<float>(k) * dt);
    for (std::size_t s = 0; s < kTraversalSamples; ++s) {
        const float u = static_cast<float>(s) / static_cast<float>(kTraversalSamples - 1) * static_cast<float>(count - 1);
        const std::size_t i = std::min(static_cast<std::size_t>(u), count - 2);
        const TravPoint p = lerp(frames[i].root, frames[i + 1].root, u - static_cast<float>(i));
        info.path[s] = {p.x - root0.x, p.y - root0.y, p.z - root0.z};
    }
    return info;
}

TravPoint traversalPath(const TraversalClipInfo& info, float t) {
    if (info.length <= 0.f) return {};
    const float u = std::clamp(t / info.length, 0.f, 1.f) * static_cast<float>(kTraversalSamples - 1);
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
        if (c.length <= 0.f || (c.move == TraversalMove::Up) != up) continue;
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
        float cost = dh * t.heightCost + std::fabs(speed - c.entrySpeed) * t.speedCost;
        if (foot >= 0 && c.takeoffFoot >= 0 && foot != c.takeoffFoot) cost += t.footCost;
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
    if (c.entrySpeed > 0.f) p.entryRate = std::max(t.minEntryRate, std::min(speed / c.entrySpeed, t.maxEntryRate));
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
