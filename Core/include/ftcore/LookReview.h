#pragma once

// Look-dev switches that must be inert unless asked for: the garment master's fold-layer defaults, the character rim
// term and the -ArenaWet review flag. Header only; nothing here allocates.
//
//   float s = ftcore::look::rimStrength(cvarValue);                  // 0: the masters add no rim
//   side = ftcore::look::rimSideStep(side, ftcore::look::rimSideTarget(preferred), dt); // eased lamp side
//   ftcore::look::Dir3 l = ftcore::look::rimDirection(camera, lead, side); // the virtual rim direction for this frame
//   if (auto w = ftcore::look::findWetReview(commandLine, "ArenaWet")) { /* hold the garments at w->wetness */ }

#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string_view>
#include <system_error>

namespace ftcore::look {

// Fold-layer parameters on the garment masters (FTBuildGarmentCommandlet). They are read only with the master's static
// switch FoldLayers on; off (the default) the master compiles as it did before they existed. With the switch on, these
// defaults still change nothing: no dirt, no ridge sheen, no crease darkening, no tint drift.
inline constexpr float kFoldDirtStrength = 0.f;
inline constexpr float kFoldWetRidgeSheen = 0.f;
inline constexpr float kFoldWetCreaseDarken = 0.f;
inline constexpr float kFoldTintDrift = 0.f;
inline constexpr float kFoldDirtRoughness = 0.15f; // scaled by DirtStrength, so inert at its default

// Dry fold ridges: roughness on convex ridges (WearTexture R above 0.5) is held at most this, so dry leather keeps a soft
// sheen on its folds and seams. 1 (the default) never binds: roughness is at most 1 anyway.
inline constexpr float kFoldDryRidgeRoughness = 1.f;

// Dry matte floor (on by default, with or without the fold layer): a dry garment's roughness is held at least
// lerp(lo, hi, ORM G) for its family, so the dry state never reads as vinyl gloss and the map's own variation stays;
// wet lowers it as before (wet is the only glossy state). Metal hardware takes its own band by the metal mask. Dry fold
// ridges (DryRidgeRoughness) are held at least the family's lo.
struct DryBand {
    float lo;
    float hi;
};
inline constexpr DryBand kDryLeather{0.45f, 0.60f};
inline constexpr DryBand kDryWaxed{0.55f, 0.70f};
inline constexpr DryBand kDryCloth{0.80f, 0.95f}; // cotton and wool; the master's default
inline constexpr DryBand kDryRubber{0.75f, 0.90f};
inline constexpr DryBand kDryMetal{0.45f, 0.60f};

constexpr float dryFloor(DryBand band, float ormG) {
    const float g = ormG > 0.f ? (ormG < 1.f ? ormG : 1.f) : 0.f;
    return band.lo + (band.hi - band.lo) * g;
}
// Mirrors the garment roughness node's dry floor: r held at least the band's floor (metal's by the metal mask).
constexpr float dryRoughness(float r, DryBand fabric, DryBand metal, float ormG, float metalMask) {
    const float m = metalMask > 0.f ? (metalMask < 1.f ? metalMask : 1.f) : 0.f;
    const float f = dryFloor(fabric, ormG) + (dryFloor(metal, ormG) - dryFloor(fabric, ormG)) * m;
    return r > f ? r : f;
}

// The character rim is a material term, not a light: the garment and skin masters add, after lighting (emissive),
//   rim = pow(1 - saturate(N.V), kRimExponent) * smoothstep(kRimSideLow, kRimSideHigh, saturate(N.L))
//         * RimStrength * RimScale * RimColour
// with N the vertex (interpolated geometric) normal turned to the face being drawn (TwoSidedSign: a two-sided garment's
// back faces, such as a collar's inside or a flipped face, would otherwise read as grazing and glow whole), never the
// normal-mapped one, so fold ridges, seams and stitch lines inside the silhouette catch nothing; and L a virtual direction set once a frame. The side term has no bias: the
// silhouette edge facing away from L gets nothing, the edge toward it gets the full rim (rimWeight is the reference). Strength 0 (the default) adds nothing. Per instance RimScale: 1 on cloth,
// leather and skin, 0 on under-layers (a tee or shirt worn beneath an open jacket), kRimFaceScale on the face,
// kRimHairScale on hair, none on glasses. A strand has no surface normal (its vertex normal is the strand's tangent), so
// hair takes N from the head instead: from RimHead (the head bone raised kRimHairCentreUpCm, written with RimDir) to the
// strand; its rim is also capped at kRimHairCap times its base colour (a stand-in for "at most 1.6 times its lit
// value", which a surface material cannot read).
inline constexpr float kRimExponent = 5.f;   // edge falloff, pow(1 - N.V, this)
inline constexpr float kRimSideLow = 0.1f;   // N.L at or below this: no rim on that side
inline constexpr float kRimSideHigh = 0.6f;  // N.L at or above this: the full rim
inline constexpr float kRimFaceScale = 0.5f;
inline constexpr float kRimHairScale = 0.4f;
inline constexpr float kRimHairCap = 0.6f;
inline constexpr float kRimHairCentreUpCm = 7.f; // head bone to the middle of the skull, for the hair's head normal
inline constexpr float kRimElevationDeg = 30.f; // above the horizon, on the far side of the lead from the camera
inline constexpr float kRimYawDeg = 35.f;       // turned this far to one side of the camera (right by default)
inline constexpr float kRimSideFlipSeconds = 0.4f; // a full flip, camera-right to camera-left, eases over this long

// The rim's weight before strength, scale and colour, as the masters compute it: nDotV and nDotL from the vertex normal.
// NaN gives 0.
inline float rimWeight(float nDotV, float nDotL) {
    const float v = nDotV > 0.f ? std::fmin(nDotV, 1.f) : 0.f;
    const float l = nDotL > 0.f ? std::fmin(nDotL, 1.f) : 0.f;
    if (!(nDotV == nDotV) || !(nDotL == nDotL)) {
        return 0.f;
    }
    const float t = std::fmin(std::fmax((l - kRimSideLow) / (kRimSideHigh - kRimSideLow), 0.f), 1.f);
    return std::pow(1.f - v, kRimExponent) * t * t * (3.f - 2.f * t);
}

// The ft.Rim value as a strength: 0 for 0, negative or NaN.
constexpr float rimStrength(float cvarValue) { return cvarValue > 0.f ? cvarValue : 0.f; }

struct Dir3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

// The side the rim leans to, from a preferred side's sign (the world lane passes the side its nearest lamp is on):
// +1 camera-right, -1 camera-left; 0 or NaN gives the default, camera-right.
constexpr float rimSideTarget(float preferredSign) { return preferredSign < 0.f ? -1.f : 1.f; }

// One frame of the side's ease toward its target: linear, a full flip (-1 to +1) in kRimSideFlipSeconds. A NaN or
// out-of-range side restarts at the target; a NaN or negative dt holds.
inline float rimSideStep(float side, float target, float dt) {
    if (!(side >= -1.f && side <= 1.f)) {
        return target;
    }
    if (!(dt > 0.f)) {
        return side;
    }
    const float step = 2.f * dt / kRimSideFlipSeconds;
    return side < target ? std::fmin(side + step, target) : std::fmax(side - step, target);
}

// The rim direction (unit, z up, the engine's axes: x forward, y right) for a camera and a lead: from the lead away from
// the camera on the ground plane, turned side x kRimYawDeg toward the camera's right (up x away; side -1 turns it to the
// left, 0 straight behind), raised kRimElevationDeg. With the camera straight above the lead (no ground bearing) the
// bearing is +x. side is clamped to [-1, 1].
inline Dir3 rimDirection(Dir3 camera, Dir3 lead, float side = 1.f) {
    float ax = lead.x - camera.x;
    float ay = lead.y - camera.y;
    const float len = std::sqrt(ax * ax + ay * ay);
    if (!(len > 1e-3f)) {
        ax = 1.f;
        ay = 0.f;
    } else {
        ax /= len;
        ay /= len;
    }
    const float rx = -ay; // up x away
    const float ry = ax;
    constexpr float kDeg = 3.14159265358979f / 180.f;
    const float k = side >= -1.f ? (side <= 1.f ? side : 1.f) : (side < -1.f ? -1.f : 1.f); // NaN: the default side
    const float cy = std::cos(k * kRimYawDeg * kDeg), sy = std::sin(k * kRimYawDeg * kDeg);
    const float ce = std::cos(kRimElevationDeg * kDeg), se = std::sin(kRimElevationDeg * kDeg);
    return {ce * (cy * ax + sy * rx), ce * (cy * ay + sy * ry), se};
}

// The rim's per-frame driver, as rules it can be checked by. The collection is shared, so one component writes it: the
// holder keeps it while it writes every frame; a figure the arena placed (priority 1) takes it from an explore pawn
// (priority 0) alive in the same world, since the arena's camera frames that figure; a slot last written before the
// previous frame is free. Frames are the engine's frame counter.
constexpr bool rimMayWrite(bool isHolder, bool held, unsigned long long slotFrame, unsigned long long frame, int priority,
                           int slotPriority) {
    return isHolder || !held || slotFrame + 1 < frame || priority > slotPriority;
}

// True when a tick has nothing to do: strength 0 now and 0 last written (the collection's default is 0). It then
// returns before it touches the collection (a branch), only snapping its side to the target so a switch-on starts there.
constexpr bool rimIdle(float strength, float writtenStrength) { return !(strength > 0.f) && !(writtenStrength > 0.f); }

// A fixed wet state held on a figure's garments for review: Wetness and the wet line in cm above the feet.
struct WetReview {
    float wetness = 1.f;
    float lineCm = 130.f;
};

namespace detail {
inline float parseOr(std::string_view text, float fallback) {
    float value = fallback;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc() && end == text.data() + text.size() ? value : fallback;
}
} // namespace detail

// "-<flag>=<wetness>,<line cm>" in a command line, flag matched whole and case sensitive (at the start or after a
// space). A part left out or unreadable takes the WetReview default. No flag: nullopt, and no review runs.
inline std::optional<WetReview> findWetReview(std::string_view commandLine, std::string_view flag) {
    if (flag.empty()) {
        return std::nullopt;
    }
    for (std::size_t at = commandLine.find('-'); at != std::string_view::npos; at = commandLine.find('-', at + 1)) {
        if (at > 0 && commandLine[at - 1] != ' ' && commandLine[at - 1] != '\t') {
            continue;
        }
        const std::string_view rest = commandLine.substr(at + 1);
        if (rest.substr(0, flag.size()) != flag || rest.size() <= flag.size() || rest[flag.size()] != '=') {
            continue;
        }
        std::string_view value = rest.substr(flag.size() + 1);
        value = value.substr(0, value.find_first_of(" \t"));
        WetReview review;
        const std::size_t comma = value.find(',');
        review.wetness = detail::parseOr(value.substr(0, comma), review.wetness);
        if (comma != std::string_view::npos) {
            review.lineCm = detail::parseOr(value.substr(comma + 1), review.lineCm);
        }
        return review;
    }
    return std::nullopt;
}

} // namespace ftcore::look
