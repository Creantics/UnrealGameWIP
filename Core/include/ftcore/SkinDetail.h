#pragma once

// A tiling skin detail tile for character skin materials: a height field of pores and fine surface
// grooves, and the two textures the skin master samples from it (see FTBuildSkinCommandlet).
//
//   ftcore::SkinDetailSettings settings;          // 1024 px, a 28 x 28 jittered pore grid
//   std::vector<float> height;
//   ftcore::makeSkinHeight(settings, height);     // size * size, 1 = open skin, lower in pores and grooves
//   std::vector<uint8_t> normal, mask;
//   ftcore::skinNormalBgra(height, settings.size, settings.normalStrength, normal); // tangent-space, BGRA8
//   ftcore::skinMaskBgra(height, settings.size, mask); // R cavity, G micro roughness, B 0, A 255
//
// Everything wraps at the tile edges, so the tile repeats without a seam. The same seed always gives
// the same tile. Building a tile allocates (load time only).

#include "ftcore/Api.h"

#include <cstdint>
#include <vector>

namespace ftcore {

struct SkinDetailSettings {
    uint32_t size = 1024;       // pixels per side; a power of two from 16 up
    uint32_t seed = 1;
    uint32_t poreCells = 28;    // one pore per cell of a poreCells x poreCells grid, jittered
    float poreRadius = 0.16f;   // pore radius as a fraction of the cell, before the per-pore jitter
    float poreDepth = 0.55f;    // depth of an average pore, in height units
    float grooveDepth = 0.12f;  // depth of the fine groove network
    float normalStrength = 6.f; // height slope (per pixel, in height units) to normal tilt
};

// Fills height with size * size values in [0, 1], row-major. Returns false (height empty) when the
// settings are out of range.
FTCORE_API bool makeSkinHeight(const SkinDetailSettings& settings, std::vector<float>& height);

// Tangent-space normals from the height field (wrapping central differences), packed as BGRA8 with
// X in R, Y in G and Z in B, each mapped from [-1, 1] to [0, 255]; A is 255.
FTCORE_API void skinNormalBgra(const std::vector<float>& height, uint32_t size, float strength,
                               std::vector<uint8_t>& out);

// BGRA8 masks: R cavity (255 on open skin, darker in pores and grooves), G micro roughness
// variation centred on 128, B 0, A 255.
FTCORE_API void skinMaskBgra(const std::vector<float>& height, uint32_t size, std::vector<uint8_t>& out);

} // namespace ftcore
