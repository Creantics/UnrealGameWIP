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
// The twist is the swing-twist split of the rotation about the axis. Angles are radians; a clip's
// twist is unwrapped key to key (unwrapAngle) before it is scaled, so a roll through 180 degrees
// does not flip the twist bones by 360 * f.

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

// The angle plus the whole turns that bring it nearest previous (for a run of keys).
FTCORE_API double unwrapAngle(double angle, double previous);

// How far along the segment a twist bone sits: its offset from the segment's head projected on the
// child's offset, as a share of the child's offset, clamped to [0, 1]. 0 when the child's offset is zero.
FTCORE_API double segmentFraction(const TwistVec& boneOffset, const TwistVec& childOffset);

// The twist bone's roll (radians) for one key, from the formula above.
FTCORE_API double twistBoneRoll(double fraction, double childTwist, double ownTwist);

// The rotation of angle radians about the axis (any non-zero length).
FTCORE_API TwistQuat axisAngle(const TwistVec& axis, double angle);

} // namespace ftcore
