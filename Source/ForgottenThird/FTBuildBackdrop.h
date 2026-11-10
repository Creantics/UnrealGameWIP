#pragma once

// The massing build's backdrop (FTBuildMassingWorld calls these): the far hill, the horizon and the skyline past the
// field, all material-only or instances of the build's cells (FFTCellInstancer, no collision, no shadow), never actors.
//
//   -HillLook  the ground's instance breaks the hill's slopes up: a shape mask made from the height field
//              (ftcore::heightShapeMask) puts rock on its ridges and pales its crests, with more macro variation and hue
//              drift, a cooler rock and a less sandy ground.
//   -HillDress trees and scrub on the hill, thickest on its crests, and a pylon line along its ridge (ftcore::hillDress),
//              kept off every box.
//   -Skyline   far blocks past the field's south and north edges on a dark plate each (ftcore::skylineBand), so a
//              view out of the strip ends on a city, not the sky.
//   -Haze[=<density>] a second height-fog layer low over the sea and the ground (default 0.05), so the horizon fades
//              into haze instead of banding, and the night's city glow (FTNightLook.h) reads near it.
//   -Backdrop  all four.

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

// -HillDress and -Skyline: their instances into Cells (before its Flush). Base is the build's surface material.
FORGOTTENTHIRD_API void Dress(UPackage* Package, FFTCellInstancer& Cells, UMaterialInterface* Base, const ftcore::HeightField& Field,
	double Side, const std::vector<ftcore::MassingBox>& Boxes);

// -Haze: the second fog layer on every height fog in World (after FTScene::SpawnLook).
FORGOTTENTHIRD_API void Haze(UWorld* World);
} // namespace FTBackdrop
