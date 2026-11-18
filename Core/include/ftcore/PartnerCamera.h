#pragma once
// The play camera as the partner's brain reads it (PartnerBrain rules 13-16), in its own header so ArenaFight's input
// can carry it without PartnerBrain.h (which includes ArenaFight.h).

#include <cmath>

namespace ftcore {

// Rule 13's frame margin, the default of PartnerTuning::frustumMargin: the share of the WHOLE screen's width (and of
// its height) kept clear on the left, right and top. A point is in frame when its normalised device coordinates are
// within 1 - 2 x margin of the centre across and above it (0.1: ndc <= 0.8; 0: the screen's own edges), and within
// 1 - 2 x kPartnerFrustumMarginBottom below it. A host checking "on screen" calls partnerInFrame with this. The brain
// picks stations by the same test with a tall enemy in the fight or rule 17's circling on; otherwise its bottom keeps
// frustumMargin too (stricter than the host's measure, so a station it picks is always on screen by it).
inline constexpr float kPartnerFrustumMargin = 0.1f;
// The bottom edge's own margin: the low lock-on camera puts the feet of a partner beside the lead near the bottom of
// the frame, where 0.10 left out partners whose whole body is plainly on screen. The bottom keeps this clear, or the
// margin when that is smaller (a margin of 0 is the screen's own edges on all four sides). Feet and head must still
// both be inside. It must stay under 0.06: the lock holds the lead's feet at ArenaLockRules::feetMin (0.06) of the
// frame's height over its bottom edge, and on the tall lock's settled low lens (0.6 m up, horizontal field of view 70
// degrees, the tall-boss test's camera) they sit at 0.060, so a partner at the lead's depth has its feet there too.
// 0.05 clears them; a smaller margin raises StayClose's circling turns (six seeds, reversals per 10 s while circling:
// 5.97 at 0.10, 6.33 at 0.05, 6.62 at 0.04, 7.48 at 0.03) for no further gain on the tall boss.
inline constexpr float kPartnerFrustumMarginBottom = 0.05f;

struct PartnerVec3 {
    float x = 0.f, y = 0.f, z = 0.f; // the ground plane's x and y, z up
};

// The play camera (rules 13-16), in the ground plane's x and y with z up, the units of the rest. Not given (the
// default), or not whole (partnerCameraWhole: a field not finite, forward or up not unit within 0.01, up within
// 1 degree of forward, the field of view outside 1..170 degrees, the aspect outside 0.2..5 or bodyHeight outside
// 0.5..4): exactly as not given, in the brain and in ArenaFight alike.
// An Unreal camera's FOV is its horizontal angle: verticalFovDegrees = 2 atan(tan(fov / 2) / aspect).
struct PartnerCameraView {
    bool given = false;
    PartnerVec3 pos{};
    PartnerVec3 forward{};          // unit
    PartnerVec3 up{};               // unit, not along forward (need not be square to it)
    float verticalFovDegrees = 0.f; // the whole vertical angle
    float aspect = 0.f;             // width over height
    float groundZ = 0.f;            // the floor under the partner (its feet)
    float bodyHeight = 1.8f;        // feet to head
};

// Given and whole (see PartnerCameraView): the one test the brain's frame and ArenaFight both use.
inline bool partnerCameraWhole(const PartnerCameraView& v) {
    const auto fin = [](const PartnerVec3& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); };
    const auto within = [](float x, float lo, float hi) { return x >= lo && x <= hi; }; // false for NaN
    if (!v.given || !fin(v.pos) || !fin(v.forward) || !fin(v.up) || !std::isfinite(v.groundZ) ||
        !within(v.verticalFovDegrees, 1.f, 170.f) || !within(v.aspect, 0.2f, 5.f) || !within(v.bodyHeight, 0.5f, 4.f))
        return false;
    const auto dot = [](const PartnerVec3& a, const PartnerVec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    const float lf = std::sqrt(dot(v.forward, v.forward)), lu = std::sqrt(dot(v.up, v.up));
    if (!(std::fabs(lf - 1.f) <= 0.01f) || !(std::fabs(lu - 1.f) <= 0.01f)) return false;
    const float c = dot(v.forward, v.up) / (lf * lu); // the cosine between them
    return 1.f - c * c > std::pow(std::sin(1.f * 3.14159265358979f / 180.f), 2.f); // more than 1 degree apart
}

} // namespace ftcore
