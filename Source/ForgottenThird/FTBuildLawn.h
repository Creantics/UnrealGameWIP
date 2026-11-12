#pragma once

// FTBuildMassingWorld's lawns: wraps ftcore/LawnSheet.h.
// -Lawn=<csv>: one sheet per region of the table, sampled from the height field on a 1 m grid a few cm over the ground,
// and its paths as paved ribbons over it with skirts, so a green reads as one continuous lawn that follows the ground.
// -LawnTex=<dir>: the lawn's maps (lawn_D.jpg, lawn_N.jpg as a DirectX normal, lawn_ARM.jpg; their low-frequency blotches
// flattened out first, as the master supplies the large-scale colour) imported once into
// FTLawn::TextureFolder (local only, never committed) and worn by the lawn surface; without it the lawn keeps the
// project's grass surface.

#include "CoreMinimal.h"

#include "ftcore/Backdrop.h"

class UMaterialInterface;
class UPackage;
class UWorld;
struct FActorSpawnParameters;

namespace FTLawn
{
inline constexpr const TCHAR* TextureFolder = TEXT("/Game/Fab/Lawn");

// The lawn surface: Parent with -LawnTex's maps, its tint and broad mottle. Parent itself (logged) when -LawnTex is not
// given; null (logged) when a map doesn't import.
UMaterialInterface* LawnMaterial(UPackage* Package, UMaterialInterface* Parent);

// Builds -Lawn's sheets, paths and the paths' edging (Edge; none when null) into World. True with nothing built without the switch; false (logged) on a table
// that doesn't read.
bool Build(UWorld* World, const FActorSpawnParameters& Spawn, UPackage* Package, const ftcore::HeightField& Ground, double Side,
	UMaterialInterface* Lawn, UMaterialInterface* Path, UMaterialInterface* Edge);
}
