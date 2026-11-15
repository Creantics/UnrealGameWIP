#pragma once

// The lock's framing on a large enemy: looking up never costs the lead. A hard rule on the view pitch, solved from the
// lens itself (arenaCameraLens: the arm as long as it really is, the shoulder, the height, the boom split), so the
// controlled lead's whole body stays in the frame at any pitch the camera is given:
//   - the feet at least feetMin of the frame's height over its bottom edge,
//   - the head no higher than headMax of the frame's height,
//   - the body (its radius each side) within the middle share of the frame's width.
// The framing turn orbits the pivot over the lead and the lead is fixed to the pivot, so where the lead sits on the
// screen depends on the pitch (and the rig), never on the yaw: the rule is a range of view pitch, re-solved every frame
// as the arm and the aim stance change.
//
// What the lock looks at inside that range: the part the player must read, the active telegraph, else the open weak
// point, else the chest (arenaLockAimHeight), by arenaLookUpPitch. A point the range cannot bring into the frame is
// marked at the frame's edge instead of the camera tilting further (ArenaLockFrame::aimAbove).
//
// To show more of a tall enemy the lens goes low and to the side, not back (ArenaLowRules): the boom swings down until
// the lens is `eye` m over the floor, the shoulder held at the zone's own width on the side the host picks
// (arenaLowSide: the one that keeps the aim point clear of the lead's body), and the lens tilts up from there. The arm
// and the field of view stay as they are. `low` (0..1, eased by the host) blends the rig from the zone's toward that.
//
// The partner counts only as a soft term (arenaPartnerYaw): a turn of at most maxYaw degrees that brings it into the
// frame, given only when the lead keeps its rule at the turned yaw.

#include "ftcore/Api.h"
#include "ftcore/ArenaCamera.h"
#include "ftcore/PartnerCamera.h"

namespace ftcore {

struct ArenaLeadFrameRules {
    float feetMin = 0.06f; // share of the frame's height: the feet at least this far over the bottom edge
    float headMax = 0.55f; // share of the frame's height: the head no higher than this over the bottom edge
    float widthMid = 0.70f; // the body within the middle share of the frame's width
};

inline constexpr ArenaLeadFrameRules kArenaLeadFrame{};

// The controlled lead's body at the pivot (on the floor under it), metres.
struct ArenaLeadBody {
    float head = 1.8f;
    float radius = 0.35f;
};

// Where a point lands in a view: x and y as shares of the frame (0 left / bottom, 1 right / top); `ahead` false when
// it is behind the lens (x and y then 0).
struct ArenaFramePoint {
    float x = 0.f;
    float y = 0.f;
    bool ahead = false;
};

// A point `at` (m: x forward and y right of the pivot at yaw 0, z up from the floor under it) seen by `pose` with a
// frame `aspect` wide per unit of height. pose.fov is the horizontal field of view (the engine keeps the width's).
FTCORE_API ArenaFramePoint arenaFramePoint(const ArenaCameraPose& pose, float aspect, ArenaPoint3 at);

// The lead's extent in the frame: its feet's and head's heights, and its leftmost and rightmost share, as seen by
// `pose` (its yaw included: the lead stays where the pose puts it, the body's sides along the view's right).
struct ArenaLeadShares {
    float feet = 0.f;
    float head = 0.f;
    float left = 0.f;
    float right = 0.f;
    bool ahead = false;
};
FTCORE_API ArenaLeadShares arenaLeadShares(const ArenaCameraPose& pose, float aspect, const ArenaLeadBody& body);

// True when the shares keep the rule.
FTCORE_API bool arenaLeadInFrame(const ArenaLeadFrameRules& rules, const ArenaLeadShares& s);
// True when the whole body is in the frame at all (the weaker rule a pulled-in arm may leave).
FTCORE_API bool arenaLeadVisible(const ArenaLeadShares& s);

struct ArenaLowRules {
    float eye = 0.6f;  // m: the lens's height over the floor, low
    float side = 1.2f; // m: the shoulder, low (+ right; the zone's own width: the host passes it signed by arenaLowSide)
    float rate = 2.5f; // 1/s: the host eases `low` toward its want at this rate
};
inline constexpr ArenaLowRules kArenaLow{};

// The boom pitch (degrees, 0..60) that puts the lens `eye` m over the floor for this arm; 0 when the lens is that low
// at a level boom already.
FTCORE_API float arenaLowBoom(float pivot, float arm, float up, float eye);

// The rig of one frame without its pitch: arenaCameraPose at a pitch of 0 (its arm the one the lens really has).
// The pose at view pitch p: the boom and the tilt of arenaSplitPitch(limits, p), blended by `low` toward the low boom
// (arenaLowBoom) and the low shoulder, the tilt the rest of p.
FTCORE_API ArenaCameraPose arenaPoseAtPitch(const ArenaLookLimits& limits, const ArenaCameraPose& rig, float viewPitch, float low = 0.f,
                                            const ArenaLowRules& lowRules = kArenaLow);

// The view pitch nearest `want` that keeps the rule for this rig: `want` itself when it keeps it (one test: an ordinary
// lock is never moved), else the edge of the band of pitches that keep it nearest `want`, by bisection (the lead's
// shares fall as the view pitches up, so the band is one range; the brute sweep test holds the solve to that). No
// pitch keeps it (a lens pulled in close by a wall): the nearest that keeps the whole body visible; none of those
// either: the pitch where the body passes the frame's edges least (never `want` as it is). `kept` says which held: 2
// the rule, 1 visible only, 0 neither; `evals` how many poses were tested (the cost).
// In that last case the feet give way first: a share they pass the bottom edge by counts this much against one of the
// head's or the sides' (the head and the chest are what the player reads the lead by).
inline constexpr float kArenaFeetGive = 0.25f;
// The limit of that last case: with the arm shorter than this (measured: 0.5 and 0.6 m on the shipped rig) the lens is
// so near that the head's top passes the frame too, whatever the pitch. The host logs once per fight when it is.
inline constexpr float kArenaHeadKeptArm = 0.7f;
struct ArenaLeadPitch {
    float pitch = 0.f;
    int kept = 0;
    int evals = 0;
};
FTCORE_API ArenaLeadPitch arenaLeadPitch(const ArenaLookLimits& limits, const ArenaLeadFrameRules& rules, const ArenaCameraPose& rig,
                                         float aspect, const ArenaLeadBody& body, float want, float low = 0.f,
                                         const ArenaLowRules& lowRules = kArenaLow);

// The largest low share (0..1) at which some pitch still keeps the rule for this rig: the low lens yields as a wall
// shortens the arm (on the shipped rig it is whole down to an arm of about 5 m and gone by about 2.4 m; under that
// even the zone's height keeps the rule only down to 2.4 m). About 150 pose tests: the host works it out again only
// when the arm it really has or the aspect changes.
FTCORE_API float arenaLowFit(const ArenaLookLimits& limits, const ArenaLeadFrameRules& rules, const ArenaCameraPose& rig, float aspect,
                             const ArenaLeadBody& body, const ArenaLowRules& lowRules = kArenaLow);

// The height (m over the floor) the lock aims at on a body from `base` to `top`: the telegraph's when one is active
// (telegraph >= 0), else the open weak point's (weakPoint >= 0), else the chest (chestShare of the way up).
inline constexpr float kArenaChestShare = 0.6f;
FTCORE_API float arenaLockAimHeight(float base, float top, float telegraph, float weakPoint);

// One lock frame on a followed target: the pitch the camera wants (the aim point `aimHeight` m up, `aimAway` m ahead of
// the pivot along the view, on the frame's centre line; no lower than floorPitch), held to the lead's rule, and whether
// the aim point is then above the frame's top (the host draws its marker at the top edge).
struct ArenaLockFrame {
    float want = 0.f;   // degrees, the look through the aim point (not held)
    float pitch = 0.f;  // degrees, held to the lead's rule
    int kept = 0;       // ArenaLeadPitch::kept
    bool aimAbove = false;
    float aimX = 0.5f;  // the aim point's share of the frame's width at `pitch` (clamped 0..1): where the edge marker sits
};
FTCORE_API ArenaLockFrame arenaLockFrame(const ArenaLookLimits& limits, const ArenaLeadFrameRules& rules, const ArenaCameraPose& rig,
                                         float aspect, const ArenaLeadBody& body, float aimAway, float aimHeight, float floorPitch,
                                         float low = 0.f, const ArenaLowRules& lowRules = kArenaLow);

// The view pitch (no lower than floorPitch) that puts the point `aim` (relative to the pivot, as arenaFramePoint) on the
// frame's centre line for this rig and low share: the want arenaLockFrame holds to the rule, alone (the host solves the
// want and the hold once a frame each).
FTCORE_API float arenaLockAimPitch(const ArenaLookLimits& limits, const ArenaCameraPose& rig, float aspect, ArenaPoint3 aim, float floorPitch,
                                   float low = 0.f, const ArenaLowRules& lowRules = kArenaLow);

// The low lens's shoulder side (+1 right, -1 left): the one where the aim point `aim` (the telegraph or weak point,
// relative to the pivot) stands clear of the lead's body in the frame at the full low lens. `current` stays unless its
// clearance is under `clear` (shares of the frame's width) and the other side's is at least `flip` more (the
// hysteresis: it never flips on a tie or a near one).
struct ArenaLowSideRules {
    float clear = 0.05f;
    float flip = 0.05f;
};
inline constexpr ArenaLowSideRules kArenaLowSide{};
FTCORE_API float arenaLowSide(const ArenaLookLimits& limits, const ArenaCameraPose& rig, float viewPitch, float aspect,
                              const ArenaLeadBody& body, ArenaPoint3 aim, float current, const ArenaLowSideRules& sideRules = kArenaLowSide,
                              const ArenaLowRules& lowRules = kArenaLow);

// The partner's soft term: a yaw (degrees, + right) of at most maxYaw that brings the partner's point `partner`
// (relative to the pivot, as arenaFramePoint) inside the frame's width less `margin` each side, from the pose as it is;
// 0 when it is in already, when more than maxYaw would be needed, or when the lead would lose its rule at that yaw.
struct ArenaPartnerYawRules {
    float maxYaw = 5.f;    // degrees
    float margin = kPartnerFrustumMargin; // share of the width kept clear each side: the brain's frame margin
};
inline constexpr ArenaPartnerYawRules kArenaPartnerYaw{};
FTCORE_API float arenaPartnerYaw(const ArenaPartnerYawRules& yawRules, const ArenaLeadFrameRules& rules, const ArenaCameraPose& pose,
                                 float aspect, const ArenaLeadBody& body, ArenaPoint3 partner);

// The frustum of `pose` with its pivot over the floor point `at` (m) and a frame `aspect` wide per unit of height
// (pose.fov horizontal, as arenaFramePoint): the lens where arenaCameraLens puts it, looking along the boom's pitch
// plus the tilt at the pose's yaw. Off when the aspect or the lens is not usable.
FTCORE_API ArenaFrustum arenaPoseFrustum(const ArenaCameraPose& pose, Vec2 at, float aspect);

// True when the point `p` (world m, as the frustum) is ahead of the lens and inside the frame less `margin` (a share
// of the frame's width and height kept clear each side, 0..0.49). False for a frustum that is off.
FTCORE_API bool arenaInFrustum(const ArenaFrustum& f, ArenaPoint3 p, float margin = 0.f);

// The same camera as the partner's brain reads it (ArenaInput::partnerCamera; ftcore/PartnerCamera.h): the frustum
// of arenaPoseFrustum over a floor at z 0, with the partner `bodyHeight` tall. Not given when that frustum is off.
FTCORE_API PartnerCameraView arenaPoseCamera(const ArenaCameraPose& pose, Vec2 at, float aspect, float bodyHeight);

// A point hidden behind a body from the lens ("partner in play" counts the partner hidden when its middle and its head
// both are): the straight line from `eye` to `point` (m) passes through the body's upright cylinder (centre `at`,
// `radius`, from baseZ up to topZ) on its way. A point inside the cylinder is hidden; an eye inside it hides nothing
// (it is not looking past the body). False for a bad cylinder (radius not above 0, topZ not above baseZ) or input.
FTCORE_API bool arenaHiddenBehind(ArenaPoint3 eye, ArenaPoint3 point, Vec2 at, float radius, float baseZ, float topZ);


} // namespace ftcore
