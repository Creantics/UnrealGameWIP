#pragma once

// The close lens's framing (CameraPreset close only; classic never reads any of this): the lead over a shoulder at a
// third of the width, the target beside it, the partner in the frame, all without pulling the camera out.
//
// Range: the melee share (0 far, 1 in melee) comes from the distance to the followed target (arenaCloseMeleeWant:
// 1 at meleeTo or nearer, 0 at farFrom or further, a smoothstep between), eased over smoothSeconds and held still
// during a dodge's first dodgeHold s (arenaCloseMeleeStep; the host's hit pause holds the whole camera already).
// The share shapes the rig (arenaCloseShape): the pivot up by pivotUp, the field of view to fovMelee, the target's
// share of the width to targetXMelee, and the lead's frame rule relaxed so the look-up can happen. From a lens 2.6 m
// behind a pivot raised 0.25 m the lead sinks in the frame, so the rule's cut (the point on the lead kept on screen)
// may sink to cutMinMelee and, on a tall target, rise toward the shoulders (cutUpTall, held shoulderRoom under the
// head): at a full melee share the rule then allows +6 degrees on a normal enemy and about +28 on a tall one (the
// tests pin what it allows), where the far rule held a raised pivot at -4.4. The arm never changes.
// The pitch at melee looks toward the target's head (arenaCloseMeleePitch), capped by the target's height between
// pitchMelee (normal) and pitchMeleeTall (tallTo m and over), blended from the far pitch by the share. The lead's rule
// (arenaLeadPitch) still holds the result.
//
// Shoulder: the lens sits `side` m to one side (+1 right, -1 left). The host picks the side only when the lock is
// acquired or changes, or when a wall leaves the current side under wallRoom of its arm, and never mid-attack
// (arenaCloseShoulderPick): a blocked side loses, then the side that has the partner in the frame wins, then the
// one with clearly more room, else the side stays. The swap blends over swapSeconds (arenaCloseShoulderStep). The
// target's share of the width mirrors with the side (0.6 right of centre on the right shoulder, 0.4 on the left),
// moving with the blend, so the lead and the target never cross.
//
// Partner: a yaw toward the partner of at most partnerYawMax degrees (arenaClosePartnerYaw), when it is within
// partnerRange m of the lead and out of the frame, given only when the lead keeps its rule and the target stays in
// targetMin..targetMax of the width (mirrored on the left shoulder).
//
// Nothing here allocates.

#include "ftcore/Api.h"
#include "ftcore/ArenaCamera.h"
#include "ftcore/ArenaLockFrame.h"
#include "ftcore/ArenaLook.h"
#include "ftcore/CameraPreset.h"
#include "ftcore/PartnerCamera.h"

namespace ftcore {

struct ArenaCloseRules {
    // Range.
    float meleeTo = 2.5f;        // m: the melee share is 1 at this distance or nearer
    float farFrom = 5.f;         // m: and 0 at this or further (today's close lens)
    float pivotUp = 0.25f;       // m the pivot rises at a full melee share
    float fovMelee = 68.f;       // degrees, horizontal, at a full melee share
    float targetXMelee = 0.64f;  // share of the width the target is turned to at a full melee share (right shoulder)
    float pitchMelee = 6.f;      // degrees: the most the melee look-up takes on a normal enemy (the enemy, not the sky, holds the upper frame)
    float pitchMeleeTall = 28.f; // degrees: on a target tallTo m or taller
    float tallFrom = 2.5f;       // m: a target this tall or shorter is normal
    float tallTo = 7.f;          // m: this tall or taller is fully tall
    float cutMinMelee = -0.10f;  // share of the frame's height the cut may sink to at a full melee share
    float cutUpTall = 0.7f;      // m the cut rises at a full melee share on a fully tall target
    float shoulderRoom = 0.35f;  // m: the raised cut stays at least this far under the lead's head (the shoulders)
    float smoothSeconds = 0.3f;  // s: the melee share's time constant
    float dodgeHold = 0.15f;     // s: the share is held still for a dodge's first this long
    // Shoulder.
    float swapSeconds = 0.4f;    // s: a full swap from one shoulder to the other
    float stickEase = 0.25f;     // s: a stick held past a swap's end eases its basis onto the view over this long
    float wallRoom = 0.8f;       // share of the arm clear under which a side is blocked
    float roomFlip = 0.05f;      // share of the arm one side's room must beat the other's by to win on room alone
    // Partner.
    float partnerYawMax = 12.f;  // degrees
    float partnerRange = 8.f;    // m from the lead
    float targetMin = 0.55f;     // the target's share of the width kept while the partner yaw turns (right shoulder)
    float targetMax = 0.80f;
    // Contact against a tall or wide target: the view swings round the lead to run along the target's flank.
    float contactTo = 2.5f;      // m from the lead to the target's body (its centre less its radius): share 1 at this or nearer
    float contactFrom = 3.5f;    // m: and 0 at this or further (the stand-off framing beyond)
    float contactTall = 3.f;     // m: a target taller than this takes the contact framing
    float contactWide = 1.f;     // m of body radius: or one at least this wide
    float contactSeconds = 0.5f; // s: a full blend in or out, at a steady rate
    float swingMin = 50.f;       // degrees: the swing round the lead at a full contact share, held to this band
    float swingMax = 70.f;
    float contactX = 0.88f;      // share of the width the swing puts the target's centre at (right shoulder), at least
    float contactEdgeX = 0.55f;  // and its body's near outline (the lead's side) at least this far across
    float contactPitchMin = 15.f; // degrees: the look-up at a full contact share, held to this band
    float contactPitchMax = 25.f;
};
inline constexpr ArenaCloseRules kArenaClose{};

// The melee share the distance asks for: 1 at meleeTo or nearer, 0 at farFrom or further, a smoothstep between. A
// NaN or negative distance gives 0 (nothing to frame).
FTCORE_API float arenaCloseMeleeWant(const ArenaCloseRules& rules, float distance);

// One step of dt s of the melee share toward `want`: a first-order ease with time constant smoothSeconds (never past
// it), unchanged while `hold`. NaN inputs give `now` (a NaN now: 0).
FTCORE_API float arenaCloseMeleeStep(const ArenaCloseRules& rules, float now, float want, float dt, bool hold);

// True during a dodge's first dodgeHold s (0 <= dodgeAge < dodgeHold): the share holds still.
FTCORE_API bool arenaCloseDodgeHolds(const ArenaCloseRules& rules, float dodgeAge);

// How tall the target is for the close lens: 0 at tallFrom m or shorter, 1 at tallTo or taller, linear between.
FTCORE_API float arenaCloseTallness(const ArenaCloseRules& rules, float targetHeight);

// The close rig and its rule at a melee share, a tallness and a shoulder (-1 left .. +1 right, the blend between):
// the pivot, the signed side, the fov, the target's share of the width (mirrored with the shoulder), the cut height on
// a lead `leadHead` m tall (the preset's cut held to half the lead, raised on a tall target toward the shoulders) and
// the cut's least share of the frame's height. Share 0 and shoulder +1 give the preset's own values.
struct ArenaCloseShape {
    float pivot = 0.f;
    float side = 0.f;
    float fov = 0.f;
    float screenX = 0.5f;
    float cutAt = 0.f;
    float cutMin = 0.f;
};
FTCORE_API ArenaCloseShape arenaCloseShape(const ArenaCloseRules& rules, const CameraPreset& base, float share, float tallness,
                                           float shoulder, float leadHead);

// The view pitch at a melee share: `farPitch` (the pitch the lock wants today) blended by `share` toward the look-up
// at the target's head (arenaLookUpPitch from this pivot, arm and height, `distance` m ahead, the head `targetHeight`
// m up), that look-up capped between pitchMelee and pitchMeleeTall by the target's tallness and never under farPitch.
// Share 0 (or a NaN input): farPitch to the bit.
FTCORE_API float arenaCloseMeleePitch(const ArenaCloseRules& rules, const ArenaLookLimits& limits, float pivot, float arm, float up,
                                      float distance, float targetHeight, float share, float farPitch);

// The shoulder to take (+1 right, -1 left) from each side's clear share of the arm (1: nothing in the way) and
// whether the partner falls inside the frame from that side: a blocked side (room under wallRoom) loses to an open
// one; then the side with the partner in frame when only one has it; then the side with more room by roomFlip or
// more; else `current` (its sign; 0 counts as right).
FTCORE_API float arenaCloseShoulderPick(const ArenaCloseRules& rules, float roomRight, float roomLeft, bool partnerRight,
                                        bool partnerLeft, float current);

// One step of dt s of the shoulder (-1..1) toward `want` (its sign) at a full swap per swapSeconds, never past it.
FTCORE_API float arenaCloseShoulderStep(const ArenaCloseRules& rules, float now, float want, float dt);

// True while the shoulder (-1..1) has not reached `want`'s side (its sign; 0 counts as right): a swap is blending.
FTCORE_API bool arenaCloseSwapping(float now, float want);

// The stick's basis (the yaw it is read along, degrees) through a shoulder swap. While the swap blends and the stick
// is held off neutral the basis stays where it was, so a held direction never turns under the player; let go, it is
// the view's yaw at once (nothing is felt with no stick); held past the blend's end, it eases onto the view at a
// steady rate that closes the gap in stickEase s (never slower, so a view that keeps turning is still caught). With
// no hold behind it the basis is the view's yaw exactly, held stick or not (classic never swaps, so never lags).
struct ArenaStickBasis {
    float yaw = 0.f;
    float rate = 0.f;  // degrees/s of the ease onto the view; 0 while holding or following
    bool set = false;  // false: the next step takes the view's yaw
    bool lag = false;  // true while held through a swap or easing back after one
};
FTCORE_API ArenaStickBasis arenaCloseStickBasis(const ArenaCloseRules& rules, ArenaStickBasis now, float viewYaw, bool swapping,
                                                bool stickHeld, float dt);

// The close partner yaw (degrees, + right): the smallest turn in 0.25 degree steps, up to partnerYawMax, that puts
// the partner's point (relative to the pivot, as arenaFramePoint) inside the frame's width less `margin` each side,
// while the lead keeps `leadRules` and the target's point stays in targetMin..targetMax of the width (mirrored when
// `shoulder` is under 0). 0 when the partner is in the frame already, further than partnerRange from the pivot on the
// floor, or no turn up to the most does it. The turn toward the partner's side is tried first at each size.
FTCORE_API float arenaClosePartnerYaw(const ArenaCloseRules& rules, const ArenaLeadFrameRules& leadRules, const ArenaCameraPose& pose,
                                      float aspect, const ArenaLeadBody& body, ArenaPoint3 partner, ArenaPoint3 target, float shoulder,
                                      float margin = kPartnerFrustumMargin);

// Contact framing. Within about 3 m of the body of a target taller than contactTall m (or with a body radius of
// contactWide m or more) the camera does not sit behind the lead: it swings round the lead toward the open side (the shoulder's side)
// so the view runs along the target's flank, the lead on one side of the frame and the target's edge on the other,
// looking up. The arm never changes (no pull-out).
//
// The contact share the range and size ask for: 0 for a target neither tall nor wide, else, by the distance to its body
// (`distance` to its centre less `targetRadius`), 1 at contactTo m or nearer, 0 at contactFrom or further, a smoothstep
// between. NaN or a negative distance: 0.
FTCORE_API float arenaCloseContactWant(const ArenaCloseRules& rules, float distance, float targetHeight, float targetRadius);

// One step of dt s of the contact share toward `want` at a full blend per contactSeconds, never past it. A NaN now: 0;
// a NaN want or dt <= 0: unchanged.
FTCORE_API float arenaCloseContactStep(const ArenaCloseRules& rules, float now, float want, float dt);

// True while the contact share has not reached what the range asks for: the swing is blending (the stick keeps its
// basis through it, as through a shoulder swap).
FTCORE_API bool arenaCloseContactBlending(float now, float want);

// The swing at a full contact share (degrees from the lock's line, + turns the view right). On the right shoulder the
// view turns left round the lead, so the lens swings out to the lead's right and the view runs along the target's
// flank: the smallest swing that puts the target's centre (`distance` m ahead on the line) at contactX of the width or
// further right and its body's near outline (a circle `radius` m round the centre, seen from a lens `back` m behind the
// pivot and |side| m out, horizontal field of view `fov`) at contactEdgeX or further right, held to swingMin..swingMax.
// Turned the other way on the left shoulder. `shoulder` is the blend (-1..1); the swing scales with it, so a swap passes
// through the line rather than jumping. NaN inputs: swingMin's size.
FTCORE_API float arenaCloseContactSwing(const ArenaCloseRules& rules, float shoulder, float side, float distance, float radius, float fov,
                                        float back);

// A target's body seen from a pose (the line from the pivot at yaw 0 is not assumed: `centre` is relative to the pivot
// on the floor, as arenaFramePoint): the shares of the frame's width its circle of `radius` m spans, left and right
// (each may lie outside 0..1), from the lens on the floor plane. `ahead` false: the centre is behind the lens.
struct ArenaBodySpan {
    float left = 0.f;
    float right = 0.f;
    bool ahead = false;
};
FTCORE_API ArenaBodySpan arenaBodySpan(const ArenaCameraPose& pose, float aspect, ArenaPoint3 centre, float radius);

// The tallness the close cut is raised by: the target's own (arenaCloseTallness), or the contact share if more, so
// the contact look-up is allowed by the lead's rule on any target that takes it.
FTCORE_API float arenaCloseCutTallness(const ArenaCloseRules& rules, float targetHeight, float contact);

// The yaw added to the framed view (line + frameTurn) at a contact share: the drawn turn goes from frameTurn (share 0)
// to `swing` (share 1), eased by a smoothstep of the share. Share 0 or a NaN input: exactly 0.
FTCORE_API float arenaCloseContactYaw(float frameTurn, float swing, float share);

// The pitch at a contact share: `pitch` (the melee pitch) held into contactPitchMin..contactPitchMax, blended in by
// the share. Share 0 or a NaN input: `pitch` to the bit.
FTCORE_API float arenaCloseContactPitch(const ArenaCloseRules& rules, float pitch, float share);

// A point in the lens's own ground frame (the capture logs' partner position): `ahead` m along the view's yaw from
// the lens (under 0: behind the camera) and `right` m to its right, from the lens where arenaCameraLens puts it.
struct ArenaLensGround {
    float ahead = 0.f;
    float right = 0.f;
};
FTCORE_API ArenaLensGround arenaLensGround(const ArenaCameraPose& pose, ArenaPoint3 at);

} // namespace ftcore
