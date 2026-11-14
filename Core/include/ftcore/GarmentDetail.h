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
// - Pore: a near-flat skin with small pits pressed into it (grain read as dimples, not as cells with walls
//   between them). One pit per cell of a `repeats` x `repeats` grid at most: jittered, some cells empty, a
//   random radius and depth, a slightly elliptical, turned outline and a soft rounded bowl, the lookup
//   domain-warped so no two neighbours match; a radius never reaches past 0.38 of the way to the nearest
//   neighbour, so pits never merge. A very faint undulation keeps the skin between them from being dead
//   flat. Made for garmentTileCentimetres(Pore) = 1.6 cm per repeat: with the default 24 cells, pits of
//   0.3 to 0.6 mm across about 0.67 mm apart.
// - Crease: a sparse net of fine crease lines running along the tile's v axis (down the image), `repeats`
//   lines across a side, spaced unevenly (2.1 to 3.9 mm at the intended 2.4 cm), each wandering a little,
//   broken by gaps, varying in depth and width (about 0.15 to 0.25 mm), plus a few short connectors at a
//   shallow angle between neighbouring lines. `repeats` must be even.
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

// Centimetres per repeat the tile is drawn for (its features have their stated real sizes there): Pore 1.6,
// Crease 2.4; 0 for tiles drawn without a fixed size (Leather, Twill, Knit), which each substance scales.
FTCORE_API float garmentTileCentimetres(GarmentTile tile);

// Fills height with size * size values in [0, 1], row-major. Returns false (height empty) when the
// settings are out of range.
FTCORE_API bool makeGarmentHeight(GarmentTile tile, const GarmentTileSettings& settings, std::vector<float>& height);

} // namespace ftcore
