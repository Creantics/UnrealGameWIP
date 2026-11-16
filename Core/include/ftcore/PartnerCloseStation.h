#pragma once
// Rule 19: the partner's station under the close arena camera (CameraPresetId::Close), on the ground plane.
//
// The close lens sits 2.6 m behind the lead over its shoulder with a 60-68 degree field of view, so the classic
// stations beside or behind the lead fall outside it. Under it the partner stands AHEAD of the lead on a flank of its
// target, where the lens sees it. One struct in, one station out (partnerCloseStation), plus a small hold the caller
// keeps. Pure: no allocation, no state but the hold. PartnerBrain uses it only while the host gives
// PartnerStationView::close (given, whole); not given, the brain is exactly as before (the classic camera, to the bit).
//
// Candidates: on both flanks of the target (flank +1: the lead's bearing turned from +x toward +y, as stTurn),
// at kPartnerCloseAngleMin..Max (40-80) degrees round the target from the target-to-lead line, in steps of
// kPartnerCloseAngleStep (5), and at standMin..standMax metres off the target's edge (the host's kit band: a melee
// partner 1.5-2.5, a caster 5-8): for each angle the band's two ends, its middle, and the point of the band nearest
// the partner. A candidate is legal when, in order:
//   - it is in the lens's view cone with the margin: in front of the lens (depth over kPartnerCloseNear) and its
//     horizontal screen coordinate within 1 - 2 x kPartnerCloseConeMargin of the centre (0.1: ndc 0.8; the share of
//     the whole width kept clear each side, as rule 13's frustumMargin);
//   - its silhouette (bodyRadius) does not overlap the lead's (bodyRadius) from the lens, either way round (never
//     hidden behind the lead, never covering it);
//   - it is not hidden behind the target (its centre inside the target's silhouette and deeper than the target);
//   - it is outside personal space (personalSpace, centre to centre from the lead) and, when leashMeters > 0, within
//     it of the lead; inside the arena (arenaRadius 0: unbounded); not inside another body (bodies: centre within
//     its radius + bodyRadius) nor a wall (a segment, within halfThickness + bodyRadius), and no wall crosses the
//     line from the lens to it.
// Score (lower is better): the walk from the partner, minus kPartnerCloseClearWeight x the lead clearance (the angle
// between partner and lead from the lens over the sum of their silhouettes' half-angles, at most 3), plus
// kPartnerCloseSidePenalty when leadSide is set and it does not stand between the screen's edge and the lead on the
// lead's side (the owner's reference: the ally ahead of the lead, between the edge and the lead), minus
// kPartnerCloseKeepBonus for the angle held.
// Hold: a flank once chosen is kept at least kPartnerCloseHoldSeconds and never changed while attacking; then only
// when the other flank scores better. On the held flank the best legal candidate wins. With none legal on the flank
// it must keep (held, or attacking): its held angle at the band's nearest point, reported not valid (the caller keeps
// its own station). None legal at all: not valid, and the hold is left as it was.

#include "ftcore/Api.h"
#include "ftcore/Explore.h" // Vec2

#include <cstdint>
#include <span>

namespace ftcore {

inline constexpr float kPartnerCloseAngleMin = 40.f;   // degrees round the target from the lead's bearing
inline constexpr float kPartnerCloseAngleMax = 80.f;
inline constexpr float kPartnerCloseAngleStep = 5.f;
inline constexpr float kPartnerCloseConeMargin = 0.1f; // share of the whole width kept clear each side
inline constexpr float kPartnerCloseNear = 0.3f;       // m: a point nearer the lens's plane than this is not seen
inline constexpr double kPartnerCloseHoldSeconds = 1.5; // a flank is kept at least this long
inline constexpr float kPartnerCloseClearWeight = 4.f;  // m of walk per unit of lead clearance (capped at 3)
inline constexpr float kPartnerCloseSidePenalty = 2.f;  // m of walk: not between the screen's edge and the lead
inline constexpr float kPartnerCloseKeepBonus = 1.f;    // m of walk: the held angle
// The kit bands (m off the target's edge) a host gives by its partner's kit (WEAPONS owns which kit is which).
inline constexpr float kPartnerCloseMeleeMin = 1.5f;
inline constexpr float kPartnerCloseMeleeMax = 2.5f;
inline constexpr float kPartnerCloseCasterMin = 5.f;
inline constexpr float kPartnerCloseCasterMax = 8.f;

// A wall as the host knows it: a segment and its half-thickness (m).
struct PartnerWall {
    Vec2 a{};
    Vec2 b{};
    float halfThickness = 0.1f;
};
// Another body the partner must not stand in (an enemy that is not the target, a prop).
struct PartnerBody {
    Vec2 pos{};
    float radius = 0.5f;
};

struct PartnerCloseInput {
    Vec2 lead{};
    Vec2 target{};
    float targetRadius = 0.5f;
    Vec2 cameraPos{};
    Vec2 cameraForward{};          // need not be unit; zero: nothing is in the cone
    float halfAngleDegrees = 30.f; // the lens's horizontal half-angle (1..85)
    // The lead's side of the screen's centre: -1 left, +1 right, 0 no preference. Right is the lens's level right,
    // (-f.y, f.x) for forward f: x forward and y right at yaw 0, as arenaInFrustum.
    std::int8_t leadSide = 0;
    Vec2 partner{};
    float standMin = kPartnerCloseMeleeMin; // m off the target's edge (0..30, standMin <= standMax)
    float standMax = kPartnerCloseMeleeMax;
    float bodyRadius = 0.4f;     // the partner's and the lead's (0.1..1.5)
    float personalSpace = 1.6f;  // centre to centre from the lead (0 none)
    float leashMeters = 0.f;     // at most this from the lead (0: none)
    Vec2 arenaCentre{};
    float arenaRadius = 0.f;     // where the partner may stand (its body's margin taken off); 0: unbounded
    std::span<const PartnerWall> walls{};
    std::span<const PartnerBody> bodies{};
    bool attacking = false;      // the partner is in a string or a blow runs: its flank never changes
};

// The caller's hold (keep it per partner; zero it on a new fight, a party switch or going down).
struct PartnerCloseHold {
    std::int8_t flank = 0; // 0: none held
    float angle = 0.f;     // degrees, the held candidate's
    double heldFor = 0.0;  // since the flank last changed (the caller adds dt each step before the call)
};

struct PartnerCloseStation {
    bool valid = false;    // a legal station: point
    Vec2 point{};
    std::int8_t flank = 0;
    float angle = 0.f;     // degrees round the target from the lead's bearing
    float offEdge = 0.f;   // m off the target's edge
    bool held = false;     // the flank was kept by the hold (the 1.5 s, or attacking)
};

// What the host gives the brain under the close camera (PartnerStationView::close); the lens's place and forward are
// the station view's cameraPos and cameraForward, the other bodies the fight's standing enemies but the target, the
// arena the station view's. Not given (the default): the brain's classic rules, to the bit.
struct PartnerCloseView {
    bool given = false;             // the close preset is active
    float halfAngleDegrees = 30.f;  // the lens's horizontal half-angle: atan(tan(vertical fov / 2) x aspect)
    std::int8_t leadSide = 0;       // as PartnerCloseInput::leadSide
    float standMin = kPartnerCloseMeleeMin; // the partner's kit band, m off the target's edge
    float standMax = kPartnerCloseMeleeMax;
    float bodyRadius = 0.4f;        // the partner's (and the lead's) body
    std::span<const PartnerWall> walls{}; // the host's walls near the fight (the brain never knows the navmesh)
};

// A whole input: every number finite, the ranges above. Not whole: no station (not valid), the hold untouched.
FTCORE_API bool partnerCloseWhole(const PartnerCloseInput& in);
// The station (see above). Updates `hold` when it returns a valid one.
FTCORE_API PartnerCloseStation partnerCloseStation(const PartnerCloseInput& in, PartnerCloseHold& hold);
// The measure: p's horizontal screen coordinate (-1..1 at the lens's edges) and whether it is in front of the lens.
FTCORE_API bool partnerCloseNdc(Vec2 cameraPos, Vec2 cameraForward, float halfAngleDegrees, Vec2 p, float& ndcX);
// p in the cone with `margin` (share of the width each side; 0: the lens's own edges).
FTCORE_API bool partnerCloseInCone(Vec2 cameraPos, Vec2 cameraForward, float halfAngleDegrees, float margin, Vec2 p);
// From the lens, a body of radius pr at p and one of radius br at b overlap (either nearer).
FTCORE_API bool partnerCloseOverlaps(Vec2 cameraPos, Vec2 p, float pr, Vec2 b, float br);
// From the lens, p's centre is inside the silhouette of the body (radius br) at b and deeper than it.
FTCORE_API bool partnerCloseHiddenBehind(Vec2 cameraPos, Vec2 p, Vec2 b, float br);

} // namespace ftcore
