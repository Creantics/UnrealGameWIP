#include "ftcore/FootHold.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

float flatDist(const FootPoint& a, const FootPoint& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

float smoothStep(float t) {
    const float s = std::clamp(t, 0.f, 1.f);
    return s * s * (3.f - 2.f * s);
}

} // namespace

void FootHold::reset() {
    feet_[0] = Foot{};
    feet_[1] = Foot{};
    pelvisDrop_ = 0.f;
}

void FootHold::update(const FootPoint ball[2], const bool lifted[2], bool lock, float dt, float bodyMoved, FootPoint by[2]) {
    by[0] = FootPoint{};
    by[1] = FootPoint{};
    if (!(bodyMoved <= tuning_.teleportJump)) reset(); // put somewhere else (or NaN): nothing held carries over
    const float step = dt > 0.f ? dt : 0.f;
    // Held, let go or lifted by the clip.
    for (int i = 0; i < 2; ++i) {
        Foot& f = feet_[i];
        if (f.stepping) continue;
        if (lifted[i]) {
            f.held = false;
        } else if (!f.held) {
            f.held = lock;
            f.anchor = ball[i];
        } else if (!lock && flatDist(f.anchor, ball[i]) <= tuning_.letGoWithin) {
            f.held = false;
        }
    }
    // One step at a time: the held foot farthest from its clip place, past the reach (let go: past letGoWithin).
    if (!feet_[0].stepping && !feet_[1].stepping) {
        const float reach = lock ? tuning_.holdReach : tuning_.letGoWithin;
        int farthest = -1;
        float farthestOff = 0.f;
        for (int i = 0; i < 2; ++i) {
            const float off = flatDist(feet_[i].anchor, ball[i]);
            if (feet_[i].held && off > reach && (farthest < 0 || off > farthestOff)) {
                farthest = i;
                farthestOff = off;
            }
        }
        if (farthest >= 0) {
            Foot& f = feet_[farthest];
            f.held = false;
            f.stepping = true;
            f.from = f.anchor;
            f.t = 0.f;
        }
    }
    for (int i = 0; i < 2; ++i) {
        Foot& f = feet_[i];
        if (f.stepping) {
            f.t = tuning_.stepTime > 0.f ? std::min(1.f, f.t + step / tuning_.stepTime) : 1.f;
            const float keep = 1.f - smoothStep(f.t);
            by[i].x = (f.from.x - ball[i].x) * keep;
            by[i].y = (f.from.y - ball[i].y) * keep;
            by[i].z = tuning_.stepLift * std::sin(3.14159265f * f.t);
            if (f.t >= 1.f) {
                f.stepping = false;
                f.held = lock && !lifted[i];
                f.anchor = ball[i];
                by[i].z = 0.f;
            }
        } else if (f.held) {
            by[i].x = f.anchor.x - ball[i].x;
            by[i].y = f.anchor.y - ball[i].y;
        }
    }
}

float FootHold::easePelvis(float want, float dt) {
    const float target = std::isfinite(want) ? std::clamp(want, 0.f, tuning_.maxPelvisDrop) : 0.f;
    const float rate = target > pelvisDrop_ ? tuning_.pelvisDropIn : tuning_.pelvisDropOut;
    const float step = dt > 0.f ? dt : 0.f;
    // FInterpTo's shape: a share of the gap each frame, the whole of it once the share reaches 1.
    const float share = std::min(1.f, rate * step);
    pelvisDrop_ += (target - pelvisDrop_) * share;
    if (target <= 0.f && pelvisDrop_ < tuning_.pelvisSnapBelow) pelvisDrop_ = 0.f;
    pelvisDrop_ = std::clamp(pelvisDrop_, 0.f, tuning_.maxPelvisDrop);
    return pelvisDrop_;
}

} // namespace ftcore
