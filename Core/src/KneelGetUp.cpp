#include "ftcore/KneelGetUp.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

void kneelStart(KneelTrack& k, double& kneelSince, double& upAt, double now) {
    kneelSince = now;
    upAt = 0.0;
    k.upStart = kKneelNone;
    k.stood = false;
}

void kneelEnd(KneelTrack& k) {
    k.upStart = kKneelNone;
    k.stood = false;
}

KneelFrame kneelShow(KneelTrack& k, double& kneelSince, double& upAt, double now, double left, const KneelClips& c) {
    KneelFrame f;
    const bool finite = std::isfinite(left);
    if (c.hasUp && (k.upStart > kKneelNone * 0.1 || k.stood)) {
        // A get-up under way plays out in real time and never sinks back into the hold.
        const double into = now - k.upStart;
        if (!k.stood && into < c.upLength) {
            f.clip = KneelClip::GetUp;
            f.time = into;
            f.timed = true;
            upAt = into;
            return f;
        }
        k.upStart = kKneelNone;
        if (k.stood || (finite && left < c.inLength + c.upLength)) {
            f.standsOut = !k.stood;
            k.stood = true;
            f.clip = KneelClip::Stand;
            upAt = c.upLength;
            return f;
        }
        f.kneelsAgain = true;
        kneelSince = now;
        upAt = 0.0;
    }
    const double spell = now - kneelSince;
    if (c.hasUp && finite && left < c.upLength) {
        f.clip = KneelClip::GetUp;
        f.time = c.upLength - std::max(left, 0.0);
        f.timed = true;
        if (c.goingDown && c.hasIn && c.inLength > 0.0 && spell < c.inLength) {
            // The go-down cut at progress p: the get-up starts from the same pose, (1 - p) of the way in.
            f.cut = true;
            f.cutAt = std::max(spell, 0.0) / c.inLength;
            f.time = (1.0 - f.cutAt) * c.upLength;
        }
        upAt = f.time;
        if (k.track || f.cut) {
            k.upStart = now - f.time;
        }
    } else if (c.hasIn && spell < c.inLength) {
        f.clip = KneelClip::GoDown;
        f.time = spell;
        f.timed = true;
    }
    return f;
}

} // namespace ftcore
