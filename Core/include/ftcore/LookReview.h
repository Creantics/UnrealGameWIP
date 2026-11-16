#pragma once

// Look-dev switches that must be inert unless asked for: the garment master's fold-layer defaults, the character rim
// light's scale and the -ArenaWet review flag. Header only; nothing here allocates.
//
//   float s = ftcore::look::rimLightScale(cvarValue, figureAllowed); // 0: no light component is made
//   if (auto w = ftcore::look::findWetReview(commandLine, "ArenaWet")) { /* hold the garments at w->wetness */ }

#include <charconv>
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

// The intensity scale a rim light runs at: the ft.RimLight value when the figure carries the light, else 0. 0 (or a
// negative or NaN value) means off, and an off rim light never makes its light component.
constexpr float rimLightScale(float cvarValue, bool allowed) { return allowed && cvarValue > 0.f ? cvarValue : 0.f; }
constexpr bool rimLightWanted(float cvarValue, bool allowed) { return rimLightScale(cvarValue, allowed) > 0.f; }

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
