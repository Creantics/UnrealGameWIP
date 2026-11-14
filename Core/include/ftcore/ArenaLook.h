#pragma once

// How far the arena's camera may look up and down, and how a steep look is drawn without the boom swinging into the
// floor. Rules only; the host turns the boom and the lens by what these say.
//
// The view's pitch (degrees, + up) runs from viewMin to viewMax, free, aimed or locked alike: near straight up, so a
// hovering or very tall boss can be put under the crosshair from anywhere in the ring. The boom (the arm from the
// pivot over the lead to the lens) swings with the view only up to boomMax; past that the boom stays and the lens alone
// tilts up the rest (arenaSplitPitch). A boom swung further up would put the lens under the pivot, into the floor or
// the lead's legs; held at boomMax the lens stays above the floor (arenaEyeHeight at least minEye for the fight's rig
// and the aim's) and the look never changes the arm's length (only the arm's own wall probe pulls it in), so looking
// up never pulls the camera out.
//
// arenaLookUpPitch: the view pitch that sends the line of sight from the lens through a point `height` m over the
// floor, `away` m ahead of the pivot (over the lead's feet), with the boom where the split puts it at that pitch (solved, as the lens
// moves with the boom). The lock looks up at a tall or hovering boss by it, and the headless aim check aims by it.

#include "ftcore/Api.h"
#include "ftcore/ArenaFight.h"

namespace ftcore {

struct ArenaLookLimits {
    float viewMin = -50.f; // degrees, looking down
    float viewMax = 85.f;  // degrees, looking up: near straight up
    float boomMax = 15.f;  // degrees the boom itself swings up; the lens tilts the rest
    float minEye = 0.4f;   // m, the lens's least height over the floor the rigs must keep at boomMax
};

inline constexpr ArenaLookLimits kArenaLook{};

// The view pitch held to [viewMin, viewMax] (NaN: 0).
FTCORE_API float arenaClampViewPitch(const ArenaLookLimits& limits, float pitch);

// A view pitch as the boom's pitch and the lens's tilt on top of it (degrees; boom + tilt = the clamped view).
struct ArenaPitchSplit {
    float boom = 0.f;
    float tilt = 0.f;
};
FTCORE_API ArenaPitchSplit arenaSplitPitch(const ArenaLookLimits& limits, float viewPitch);

// The lens's height over the floor (m) and how far it sits behind the pivot along the view's heading (m), for a boom
// of `arm` m from a pivot `pivot` m up, the lens `up` m above the boom's end (in the boom's frame), at boomPitch
// degrees (+ up swings the lens down behind the lead).
FTCORE_API float arenaEyeHeight(float pivot, float arm, float up, float boomPitch);
FTCORE_API float arenaEyeBack(float arm, float up, float boomPitch);

// The view pitch (degrees) whose line of sight from the lens passes through the point `height` m up, `away` m ahead
// of the pivot on the floor, the boom split by `limits`; held to [floorPitch, viewMax] (floorPitch held to the limits).
FTCORE_API float arenaLookUpPitch(const ArenaLookLimits& limits, float pivot, float arm, float up, float away, float height,
                                  float floorPitch);

// The height (m) at which the line of sight at viewPitch (split by `limits`) crosses `away` m ahead of the pivot: the
// crosshair's height over a body standing there.
FTCORE_API float arenaSightHeightAt(const ArenaLookLimits& limits, float pivot, float arm, float up, float away,
                                    float viewPitch);

// An enemy's body as the crosshair meets it: an upright cylinder round `at`, from `base` to `top` m over the floor.
struct ArenaBodyCylinder {
    Vec2 at{};
    float base = 0.f;
    float top = 0.f;
    float radius = 0.f;
};

// The body of a kind standing at `at` on a floor `floorZ` m up: a skinned figure hangs `look.hover` m over the floor
// and is `look.scale` times its height and its bodyRadius; a plain figure stands on the floor at scale 1. The empty
// gap under a hovering body is not body.
FTCORE_API ArenaBodyCylinder arenaBodyCylinder(const ArenaEnemyKind& kind, Vec2 at, float floorZ);

// Where the segment from -> to first enters `body`, as the share t (0..1) along it. False when it never enters,
// enters past `to`, or starts inside the body (a lens inside a flanker's body sees through it, not into it).
FTCORE_API bool arenaRayEntersBody(ArenaPoint3 from, ArenaPoint3 to, const ArenaBodyCylinder& body, float& t);

// The locked camera's framing. The lock turns the view's heading onto the line from the lead to the target, and the
// rig's shoulder (its side offset, + right) puts the lead off the view's centre; a target close behind that line is
// hidden by the lead. The framing adds an orbit turn round the pivot over the lead (yaw, degrees, + turns the view
// right) that brings the target onto the view's centre line: sin(turn) = -side / distance. The arm, the pivot and the
// shoulder are never touched, so the lens stays exactly as far from the lead as the free camera's and the lead stays
// where the free camera puts it on screen. The turn is held to maxTurn, so a stick read along the lock's line is never
// more than maxTurn off the view; nearer than |side| / sin(maxTurn) the target sits between the lead and the centre.
struct ArenaFrameRules {
    float maxTurn = 30.f; // degrees
    float rate = 6.f;     // 1/s: the applied turn eases toward the wanted one at this rate,
    float minRate = 30.f; // deg/s: and never slower than this, so it arrives
};

inline constexpr ArenaFrameRules kArenaFrame{};

// The wanted turn for a target `distance` m from the pivot along the lock's line, the shoulder `side` m (0 or NaN, or a
// NaN distance: 0; a distance within |side|: the cap).
FTCORE_API float arenaFrameTurn(const ArenaFrameRules& rules, float side, float distance);

// One step of dt s of the applied turn toward `want`: eased, never past it, and unchanged when it is there already (an
// applied 0 toward a want of 0 stays exactly 0). dt <= 0 or a NaN want: unchanged; a NaN applied counts as 0.
FTCORE_API float arenaFrameStep(const ArenaFrameRules& rules, float applied, float want, float dt);

} // namespace ftcore
