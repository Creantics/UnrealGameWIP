#pragma once

// The day's shadow floor: how far a world lifts its shade by full day, so a lane or a building's shadow reads at about
// a third of the sunlit value instead of going black beside a bright sky.
//
// Four levers, each blended in by the day amount (dayShadeAmount: 1 by full day, 0 at full night and through the dusk,
// so the night and the whole dusk keep exactly what they had):
//   skyScale       the sky light's intensity times this (the fill a shaded lane mostly sees)
//   shadowContrast the local exposure's shadow contrast (1 = off; lower lifts the shade against a bright frame)
//   diffuseBoost   Lumen's diffuse colour boost (1 = off, the engine's 1..4): more bounce from sunlit walls into shade
//   filmToe        the tonemapper's toe (the engine's 0.55; a higher toe crushes the darks)
// and, so a shaded lane's far end keeps its colour instead of washing out to white:
//   highlightContrast the local exposure's highlight contrast (1 = off; lower pulls the sky and sunlit walls down);
//                  only ever lowered from the world's own
//   minEV100       the auto exposure's floor: in a frame that is all shade the camera opens no further than this, so
//                  the shade stays under the sunlit value and the sky beyond it is not burnt out; only ever raised
//                  from the world's own (a dark world keeps its floor)
//   fogMaxOpacity  the height fog's cap: the far end of a street or the sea keeps this much less of the fog
//   fogSky         the sky's share of the fog's colour, a channel each: a dimmer grey-blue haze, not a white one
// dayShadeFrame gives the values to write: each the world's own (night) value blended toward the day's.
//
// Plain C++20; nothing allocates.

#include "ftcore/Api.h"

#include <array>

namespace ftcore {

struct DayShadeLook {
    float skyScale = 2.6f;
    float shadowContrast = 0.45f;
    float diffuseBoost = 2.0f;
    float filmToe = 0.55f;
    float highlightContrast = 0.7f;
    float minEV100 = -1.5f;
    float fogMaxOpacity = 0.6f;
    std::array<float, 3> fogSky{0.42f, 0.48f, 0.58f};
};

// The world's own values, as the look writes them (the night keeps these).
struct DayShadeBase {
    float shadowContrast = 1.f;
    float diffuseBoost = 1.f;
    float filmToe = 0.55f;
    float highlightContrast = 1.f;
    float minEV100 = -10.f;
};

struct DayShadeFrame {
    float amount = 0.f;
    float skyScale = 1.f;
    float shadowContrast = 1.f;
    float diffuseBoost = 1.f;
    float filmToe = 0.55f;
    float highlightContrast = 1.f;
    float minEV100 = -10.f;
    float fogMaxOpacity = 1.f;
    std::array<float, 3> fogSky{1.f, 1.f, 1.f};
};

// The dusk amount by which the day's lift is gone.
inline constexpr float kDayShadeDuskCut = 0.2f;

// 1 by full day; falls with the night (0 day .. 1 full night) and, squared, with the dusk (0 .. 1), gone from
// kDayShadeDuskCut of it on.
FTCORE_API float dayShadeAmount(float night, float dusk);

// Base blended toward Look by Amount (clamped); out-of-range look values are clamped to the engine's ranges
// (skyScale >= 0, shadowContrast 0..1, diffuseBoost 1..4, filmToe 0..1, highlightContrast 0..1 and never over the
// base's, minEV100 -15..25 and never under the base's, fogMaxOpacity and fogSky 0..1).
FTCORE_API DayShadeFrame dayShadeFrame(const DayShadeLook& look, const DayShadeBase& base, float amount);

} // namespace ftcore
