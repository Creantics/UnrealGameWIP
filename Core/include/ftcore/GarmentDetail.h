#pragma once

// Tiling surface-detail tiles for garment materials: a height field per substance that the garment
// masters sample as a detail normal and a cavity / micro-roughness mask at two scales (see
// FTBuildGarmentCommandlet).
//
//   ftcore::GarmentTileSettings s = ftcore::garmentTileDefaults(ftcore::GarmentTile::Leather);
//   std::vector<float> height;
//   ftcore::makeGarmentHeight(ftcore::GarmentTile::Leather, s, height); // size * size, 1 = top, 0 = groove
//   // then ftcore::skinNormalBgra / skinMaskBgra (SkinDetail.h) turn it into the two textures.
//
// - Leather: pebbled grain, jittered cells with rounded tops and narrow grooves between them, plus a
//   fine crinkle. `repeats` is the number of cells along a side.
// - Twill: a 2/1 twill, diagonal wales of yarn floats; `repeats` is the number of yarns along a side.
// - Knit: single jersey, columns of V-shaped loops; `repeats` is the number of columns along a side
//   (rows are 1.3 times as many, so a loop is taller than wide).
// - Pore: a skin with dense pits pressed into it, so the skin between them reads as pebbles. The pits are a
//   Poisson-disk set (no two centres closer than 0.75 of the mean spacing, gaps filled), `repeats` mean
//   spacings along a side; radius 0.25 to 0.375 of the mean spacing, one pit in five 0.4 to 0.45, a slightly
//   wobbly outline, a rounded bowl, and any two rims kept apart so pits never merge. The skin domes a little
//   between them. Made for garmentTileCentimetres(Pore) = 1.6 cm: with the default 20, centres 0.6 to 1.0 mm
//   apart and pits of 0.4 to 0.9 mm across.
// - Crease: `repeats` short fold creases on a 6 cm tile, each 5 to 30 mm long, running along the tile's u axis
//   (across the image) and wandering within +-30 degrees of it, about a third with a branch at 25 to 40 degrees;
//   grooves 0.3 to 0.6 mm wide, tapered at both ends. Each crease has a rank in [0, 1) (garmentCreaseRank), so
//   a master can show only the creases whose rank is under a local fold density.
//
// Every tile wraps at its edges. The same settings always give the same tile. Building a tile
// allocates (load time only).

#include "ftcore/Api.h"

#include <cstdint>
#include <vector>

namespace ftcore {

enum class GarmentTile : uint8_t { Leather, Twill, Knit, Pore, Crease };

struct GarmentTileSettings {
    uint32_t size = 512;        // pixels per side; a power of two from 32 up
    uint32_t seed = 1;
    uint32_t repeats = 24;      // cells / yarns / columns along a side; at most size / 4
    float normalStrength = 4.f; // height slope (per pixel, in height units) to normal tilt
};

// The settings each tile is built with for the garment masters.
FTCORE_API GarmentTileSettings garmentTileDefaults(GarmentTile tile);

// Centimetres per tile the tile is drawn for (its features have their stated real sizes there): Pore 1.6,
// Crease 6; 0 for tiles drawn without a fixed size (Leather, Twill, Knit), which each substance scales.
FTCORE_API float garmentTileCentimetres(GarmentTile tile);

// Fills height with size * size values in [0, 1], row-major. Returns false (height empty) when the
// settings are out of range.
FTCORE_API bool makeGarmentHeight(GarmentTile tile, const GarmentTileSettings& settings, std::vector<float>& height);

// A pit of the Pore tile, in millimetres on the tile drawn at garmentTileCentimetres(Pore).
struct GarmentPore {
    float x = 0.f, y = 0.f;
    float radius = 0.f;  // before the outline wobble (at most 15 %)
    bool large = false;  // one of the larger one-in-five
};

// A crease of the Crease tile: its length (mm), its largest turn from the tile's u axis (degrees) and whether it
// is a branch off another crease.
struct GarmentCrease {
    float length = 0.f;
    float maxAngle = 0.f;
    bool branch = false;
};

// The Pore tile's pits and the Crease tile's creases, for checks. Return false (empty) when the settings are out
// of range.
FTCORE_API bool garmentPores(const GarmentTileSettings& settings, std::vector<GarmentPore>& pores);
FTCORE_API bool garmentCreases(const GarmentTileSettings& settings, std::vector<GarmentCrease>& creases);

// Per pixel of the Crease tile (size * size, row-major): the rank of the deepest crease there, 1 where there is
// none. The master stores it beside the height so it can fade creases in by local fold density.
FTCORE_API bool garmentCreaseRank(const GarmentTileSettings& settings, std::vector<float>& rank);

} // namespace ftcore
