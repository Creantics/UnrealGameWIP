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

// The character rim is a material term, not a light: the garment and skin masters add, after lighting (emissive),
//   rim = pow(1 - saturate(N.V), 4) * saturate(N.L * 0.5 + 0.5) * RimStrength * RimScale * RimColour
// with L a virtual direction set once a frame. Strength 0 (the default) adds nothing. Per instance RimScale: 1 on cloth,
// leather and skin, kRimFaceScale on the face, kRimHairScale on hair (whose rim is also capped at kRimHairCap times its
// base colour: a stand-in for "at most 1.6 times its lit value", which a surface material cannot read), none on glasses.
inline constexpr float kRimFaceScale = 0.5f;
inline constexpr float kRimHairScale = 0.4f;
inline constexpr float kRimHairCap = 0.6f;
inline constexpr float kRimElevationDeg = 30.f; // above the horizon, on the far side of the lead from the camera
inline constexpr float kRimYawDeg = 35.f;       // turned this far to one side of the camera (right by default)
inline constexpr float kRimSideFlipSeconds = 0.4f; // a full flip, camera-right to camera-left, eases over this long

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

// The rim term's weight before colour, as the masters compute it (n.v and n.l of the shaded normal).
inline float rimTerm(float nDotV, float nDotL, float strength) {
    const float facing = std::fmin(std::fmax(nDotV, 0.f), 1.f);
    const float side = std::fmin(std::fmax(nDotL * 0.5f + 0.5f, 0.f), 1.f);
    const float edge = 1.f - facing;
    return edge * edge * edge * edge * side * rimStrength(strength);
}

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
