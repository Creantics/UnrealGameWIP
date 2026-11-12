#pragma once

// Twist bones for a retargeted clip. A limb segment (upper arm, forearm, thigh, calf) has twist bones
// along it that carry no motion of their own in mocap; without a bake they sit at the bind pose and
// the skin they hold candy-wraps when the limb rolls. Each twist bone gets a roll about the segment's
// axis (the direction from the segment's head to its child's, in the segment's frame):
//
//   roll = f * childTwist - (1 - f) * ownTwist
//
//  - f is the twist bone's fraction along the segment (0 at the segment's head, 1 at its child).
//  - childTwist is the twist, about the axis, of the child's rotation away from its bind pose in the
//    segment's frame (a wrist roll): the forearm's skin follows it more toward the hand.
//  - ownTwist is the segment's own roll about its axis away from its bind pose: the skin near the
//    segment's head holds back from it (an upper arm rolling in the shoulder).
//
// The twist is the swing-twist split q = swing * twist (the twist applied first, about the axis). It
// is ill-conditioned when the swing nears 180 degrees (a hinge bent far, an arm raised overhead): a
// small tilt of the hinge then reads as a large twist, so each key's twist is faded to 0 between
// 100 and 160 degrees of swing (twistFade; a 150 degree hinge tilted 5 degrees keeps under a tenth
// of its 36 degrees of false twist). The price: a segment swung past 100 degrees from its bind pose
// (an arm raised overhead from an A-pose bind) keeps only part of its real twist. Angles are
// radians; a clip's twist is unwrapped key to key (unwrapFadedTwist) before it is faded and scaled,
// so a roll through 180 degrees does not flip the twist bones by 360 * f; where the fade is near 0
// the unwrap starts afresh, so noise there carries no turn into the rest of the clip. The first key
// picks its turn on its own, so a looping clip whose twist passes 180 degrees can end a turn away
// from its start (the caller compares the faded twist at both ends and warns).
//
// The child term belongs to the forearm and the shin only (lowerarm -> hand, calf -> foot): the
// forearm's own pronation, carried at the elbow, is not the upper arm's to follow (it would wrap the
// upper arm's skin at the elbow), so the upper arm's and thigh's twist bones take childTwist = 0.

#include "ftcore/Api.h"

namespace ftcore {

struct TwistVec {
    double x = 0.0, y = 0.0, z = 0.0;
};

// A rotation as a unit quaternion (x, y, z the vector part, w the scalar part).
struct TwistQuat {
    double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
};

// The angle in (-pi, pi] of q's twist about the axis (any non-zero length): the rotation about the
// axis left when the swing that tilts the axis is taken out. q and -q give the same angle; a swing
// of nearly 180 degrees, where the twist is undefined, gives a finite angle.
FTCORE_API double twistAngle(const TwistQuat& q, const TwistVec& axis);

// The swing's angle in [0, pi]: how far q tilts the axis (the angle between the axis and q applied to
// it). 0 for a zero axis.
FTCORE_API double swingAngle(const TwistQuat& q, const TwistVec& axis);

// The weight a key's twist keeps for its swing: 1 up to 100 degrees, 0 from 160 degrees, a smoothstep
// between.
FTCORE_API double twistFade(double swing);

// The angle plus the whole turns that bring it nearest previous (for a run of keys).
FTCORE_API double unwrapAngle(double angle, double previous);

// unwrapAngle for a key whose twist will be scaled by fade: below a fade of 0.05 the angle comes back
// wrapped to (-pi, pi] instead (the key shows next to none of it, and the unwrap restarts there).
FTCORE_API double unwrapFadedTwist(double angle, double previous, double fade);

// How far along the segment a twist bone sits: its offset from the segment's head projected on the
// child's offset, as a share of the child's offset, clamped to [0, 1]. 0 when the child's offset is zero.
FTCORE_API double segmentFraction(const TwistVec& boneOffset, const TwistVec& childOffset);

// The twist bone's roll (radians) for one key, from the formula above.
FTCORE_API double twistBoneRoll(double fraction, double childTwist, double ownTwist);

// The rotation of angle radians about the axis (any non-zero length).
FTCORE_API TwistQuat axisAngle(const TwistVec& axis, double angle);

// What a segment's pass over a clip found (radians).
struct TwistSegmentSummary {
    double ownPeak = 0.0;   // the largest faded own twist of any key
    double childPeak = 0.0; // the largest faded child twist of any key (0 without a child term)
    bool loops = false;     // the clip ends where it starts (segment and child within 0.02 rad of their first key)
    bool loopPops = false;  // ... and a faded twist ends more than half a turn from where it starts
};

// One segment's twists over a clip of `keys` keys, in the segment's bind frame:
//  - ownDelta[k]: the segment's rotation away from its bind, applied first (bind^-1 * key);
//  - childDelta[k]: the child's rotation away from its bind in the segment's frame (key * bind^-1), or null when the
//    segment takes no child term (the upper arm and thigh), so childOut is all 0;
//  - axis: the child's bind offset in the segment's frame.
// Writes each key's faded, unwrapped twist to ownOut[k] and childOut[k] (the inputs to twistBoneRoll) and returns
// the peaks and the loop check. No allocation; the caller owns every array (keys long; keys < 1 writes nothing).
FTCORE_API TwistSegmentSummary bakeSegmentTwist(const TwistQuat* ownDelta, const TwistQuat* childDelta, int keys,
                                                const TwistVec& axis, double* ownOut, double* childOut);

} // namespace ftcore
