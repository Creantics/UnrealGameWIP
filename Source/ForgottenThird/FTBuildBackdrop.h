#pragma once

// The massing build's backdrop (FTBuildMassingWorld calls these): the far hill, the horizon and the skyline past the
// field, all material-only or instances of the build's cells (FFTCellInstancer, no collision, no shadow), never actors.
//
//   -HillLook  the ground's instance breaks the hill's slopes up: a shape mask made from the height field
//              (ftcore::heightShapeMask) puts rock on its ridges and pales its crests, with more macro variation and hue
//              drift and a cooler rock; the hill is planted green (ftcore::hillPlantMask), the low ground keeps its grey.
//              By night (the window collection's Night, the lamps' share) the ground over the planting's foot goes dark
//              and grey (ftcore::HillNightLook through the ground master's HillNight*); the road layer keeps its colour.
//   -HillDress planting on the hill as irregular green clumps (ftcore::backdropClump, never cones), canopies thickest
//              on its crests, and a pylon line along its ridge (ftcore::hillDress), kept off every box. Its materials
//              darken with the hill at night (the surface master's NightDim). -NoHillNight keeps the day albedo at night,
//              for the ground and the dress both.
//   -HillWindows hamlets of small dark houses low on the hill (the band ftcore::hillWindows takes: its foot up a quarter
//              of its height), fronts to the town, kept off every box and the planting off them, 150 lit windows at most:
//              warm (2700 K) heads lit at night through the lamp head master (needs FTBuildSurfaces -Windows), glow
//              -HillWindowGlow=<strength> (default ftcore::HillWindowSpec::glow). On with -HillLook or -HillDress too;
//              -NoHillWindows drops them.
//   -AviationLamps red obstruction lamps on the tallest roofs and far blocks (ftcore::aviationLamps), instanced
//              emissive heads lit at night (needs FTBuildSurfaces -Windows' lamp head master).
//   -Skyline   far blocks past the field's south and north edges (ftcore::skylineBand), so a view out of the strip
//              ends on a city, not the sky. They stand on land plates that carry on from the field's edge at its own
//              height, only along the reach of the edge that is land (ftcore::skylineSeat): none in open water. They
//              wear the build's window instance when Dress is given one; -SkylinePlain keeps the plain dark mass.
//   -Haze[=<density>] a second height-fog layer low over the sea and the ground (default 0.02), so the horizon fades
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

// -HillDress, -HillWindows, -Skyline and -AviationLamps: their instances into Cells (before its Flush). Base is the build's surface
// material. SkylineFacade: a window instance of the build (the r5 window master's, or null) for the far blocks.
FORGOTTENTHIRD_API void Dress(UPackage* Package, FFTCellInstancer& Cells, UMaterialInterface* Base, const ftcore::HeightField& Field,
	double Side, const std::vector<ftcore::MassingBox>& Boxes, UMaterialInterface* SkylineFacade = nullptr);

// -Haze: the second fog layer on every height fog in World (after FTScene::SpawnLook).
FORGOTTENTHIRD_API void Haze(UWorld* World);
} // namespace FTBackdrop
