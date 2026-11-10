#pragma once

// The massing build's backdrop (FTBuildMassingWorld calls these): the far hill, the horizon and the skyline past the
// field, all material-only or instances of the build's cells (FFTCellInstancer, no collision, no shadow), never actors.
//
//   -HillLook  the ground's instance breaks the hill's slopes up: a shape mask made from the height field
//              (ftcore::heightShapeMask) puts rock on its ridges and pales its crests, with more macro variation and hue
//              drift, a cooler rock and a less sandy ground.
//   -HillDress scrub on the hill as low irregular clumps (ftcore::backdropClump, never cones), thickest on its
//              crests, and a pylon line along its ridge (ftcore::hillDress),
//              kept off every box.
//   -Skyline   far blocks past the field's south and north edges on a dark plate each (ftcore::skylineBand), so a
//              view out of the strip ends on a city, not the sky.
//   -Haze[=<density>] a second height-fog layer low over the sea and the ground (default 0.05), so the horizon fades
//              into haze instead of banding, and the night's city glow (FTNightLook.h) reads near it.
//   -RockFaces the ground's faces among the built boxes (cliffs and terraces, ftcore::backdropRockFace) wear the ground's
//              rock (the ground master's Rock maps and tint, ftcore::RockFaceLook) instead of their plain concrete, so the
//              terrain walls read as the hill's stone. On with -HillLook too.
//   -Backdrop  all of them.

#include "CoreMinimal.h"

#include "ftcore/Backdrop.h"

#include <vector>

class FFTCellInstancer;
class UMaterialInterface;
class UPackage;
class UWorld;

namespace FTBackdrop
{
// Ground is the Landscape's instance of the ground master (MI_Massing_Look_Ground); with -HillLook it gains the hill's
// shape mask (saved in Package) and parameters. Returns Ground either way.
FORGOTTENTHIRD_API UMaterialInterface* HillGround(UPackage* Package, UMaterialInterface* Ground, const ftcore::HeightField& Field, double Side);

// -RockFaces (or -HillLook): for a box ftcore::backdropRockFace takes, an instance of Wear (the box's surface instance)
// wearing the rock maps, tint and roughness Ground (the Landscape's instance, HillGround's) holds, one per Wear, saved in
// Package. Null for any other box, without the switch, or when Ground holds no rock maps: the box keeps Wear.
FORGOTTENTHIRD_API UMaterialInterface* RockFace(UPackage* Package, UMaterialInterface* Wear, const ftcore::MassingBox& Box, UMaterialInterface* Ground);

// -HillDress and -Skyline: their instances into Cells (before its Flush). Base is the build's surface material.
FORGOTTENTHIRD_API void Dress(UPackage* Package, FFTCellInstancer& Cells, UMaterialInterface* Base, const ftcore::HeightField& Field,
	double Side, const std::vector<ftcore::MassingBox>& Boxes);

// -Haze: the second fog layer on every height fog in World (after FTScene::SpawnLook).
FORGOTTENTHIRD_API void Haze(UWorld* World);
} // namespace FTBackdrop
