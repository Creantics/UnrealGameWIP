#pragma once

// A parked car's look when it is drawn as the imported kei car mesh: its paint (one of eight linear presets), how dirty
// it is, and whether its lamps glow. Chosen by a seeded hash of the car's id, so a rebuild with the same seed gives the
// same street. The engine side writes it as per-instance custom data (kParkedCarCustomFloats floats):
//   0..2 PaintColor (linear rgb), 3 DirtAmount (0 clean .. 1 as worn as the sheet), 4 LampOn (0 off, 1 on).
// The car material multiplies LampOn by the night amount, so a lit car stays dark by day.
//
// Plain C++20; nothing allocates.

#include "ftcore/Api.h"

#include <cstdint>
#include <string_view>

namespace ftcore {

inline constexpr int kParkedCarPaints = 8;
inline constexpr int kParkedCarCustomFloats = 5;
inline constexpr float kParkedCarDirtMin = 0.15f;
inline constexpr float kParkedCarDirtMax = 1.0f;

struct ParkedCarColor {
    float r = 0.f, g = 0.f, b = 0.f;
};

// The eight presets in order: pale blue (the default), off-white, silver, cream, mint, brick red, charcoal, mustard.
// An index out of range gives the default.
FTCORE_API ParkedCarColor parkedCarPaint(int index);

struct ParkedCarLook {
    int paint = 0;              // 0..kParkedCarPaints-1
    ParkedCarColor color;       // parkedCarPaint(paint)
    float dirt = 1.f;           // uniform in [kParkedCarDirtMin, kParkedCarDirtMax]
    float lamp = 0.f;           // 0 or 1
};

// The look of the car with this id. lampShare (0..1) is the share of cars whose lamps are on (0: none).
FTCORE_API ParkedCarLook parkedCarLook(std::string_view id, std::uint32_t seed, float lampShare = 0.f);

// The look as custom data floats, in the order above. out must hold kParkedCarCustomFloats.
FTCORE_API void parkedCarCustomData(const ParkedCarLook& look, float* out);

} // namespace ftcore
