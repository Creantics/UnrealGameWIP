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
// head): at a full melee share the rule then allows +10 degrees on a normal enemy and about +28 on a tall one (the
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
    float pitchMelee = 10.f;     // degrees: the most the melee look-up takes on a normal enemy
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
    float wallRoom = 0.8f;       // share of the arm clear under which a side is blocked
    float roomFlip = 0.05f;      // share of the arm one side's room must beat the other's by to win on room alone
    // Partner.
    float partnerYawMax = 12.f;  // degrees
    float partnerRange = 8.f;    // m from the lead
    float targetMin = 0.55f;     // the target's share of the width kept while the partner yaw turns (right shoulder)
    float targetMax = 0.80f;
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

// The close partner yaw (degrees, + right): the smallest turn in 0.25 degree steps, up to partnerYawMax, that puts
// the partner's point (relative to the pivot, as arenaFramePoint) inside the frame's width less `margin` each side,
// while the lead keeps `leadRules` and the target's point stays in targetMin..targetMax of the width (mirrored when
// `shoulder` is under 0). 0 when the partner is in the frame already, further than partnerRange from the pivot on the
// floor, or no turn up to the most does it. The turn toward the partner's side is tried first at each size.
FTCORE_API float arenaClosePartnerYaw(const ArenaCloseRules& rules, const ArenaLeadFrameRules& leadRules, const ArenaCameraPose& pose,
                                      float aspect, const ArenaLeadBody& body, ArenaPoint3 partner, ArenaPoint3 target, float shoulder,
                                      float margin = kPartnerFrustumMargin);

// A point in the lens's own ground frame (the capture logs' partner position): `ahead` m along the view's yaw from
// the lens (under 0: behind the camera) and `right` m to its right, from the lens where arenaCameraLens puts it.
struct ArenaLensGround {
    float ahead = 0.f;
    float right = 0.f;
};
FTCORE_API ArenaLensGround arenaLensGround(const ArenaCameraPose& pose, ArenaPoint3 at);

} // namespace ftcore
