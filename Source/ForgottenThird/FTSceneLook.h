#pragma once

// The scene's photographic base, spawned from one ftcore::SceneLook: a movable sun, a sky light that
// captures the atmosphere in real time, the sky atmosphere, exponential height fog (volumetric when the
// look has it), the engine's volumetric cloud layer, and an unbound post-process volume at priority 0
// that carries the camera finish (exposure range and bias, white balance, saturation, contrast, bloom,
// vignette, grain). Every map builder lights its scene through SpawnLook and keeps only its own sun angle
// and local lamps, so all the worlds share one look until each gets its own. The builders' blocks and props
// are tints of one surface master, loaded with LoadSurface; a world map's shapes wear the photographed
// surfaces Data/Field/surfaces.json gives their role instead (LoadSurfaceTable, LoadSurfaceFor). See
// docs/SCENE_LOOK.md.

#include "CoreMinimal.h"

#include "ftcore/SceneLook.h"
#include "ftcore/SurfaceTable.h"

class UMaterialInterface;
class UWorld;
struct FActorSpawnParameters;

namespace FTScene
{
// The world surface master's package, built by FTBuildWorldMaterials.
inline constexpr const TCHAR* SurfaceMaterial = TEXT("/Game/Rendering/M_WorldSurface");
// The photographed surfaces' master and the folder of their textures and instances, built by FTBuildSurfaces.
inline constexpr const TCHAR* SurfaceTexMaterial = TEXT("/Game/Rendering/M_WorldSurfaceTex");
inline constexpr const TCHAR* SurfaceFolder = TEXT("/Game/Rendering/Surfaces");

// The package of surface Id's instance of the photographed master: SurfaceFolder/<Id>/MI_Surface_<Id>.
FORGOTTENTHIRD_API FString SurfaceInstancePackage(const FString& Id);

// Spawns the sun (pointing along SunRotation), sky light, atmosphere, fog, clouds and post-process volume
// into World, labelled Sun, SkyLight, SkyAtmosphere, HeightFog, VolumetricCloud and PostProcessVolume.
FORGOTTENTHIRD_API void SpawnLook(UWorld* World, const FActorSpawnParameters& Spawn, const ftcore::SceneLook& Look,
	const FRotator& SunRotation);

// Loads the world surface master: a "Color" vector held to a plausible albedo, "Roughness", "Metallic" and
// a low-frequency mottle on the world position ("Variation", "VariationScale"). Null, with an error logged,
// when it hasn't been built.
FORGOTTENTHIRD_API UMaterialInterface* LoadSurface();

// Reads Data/Field/surfaces.json into Table. True with an empty table when the file is missing; false, with
// the problems logged, when it's refused.
FORGOTTENTHIRD_API bool LoadSurfaceTable(ftcore::SurfaceTable& Table);

// The photographed surface Kind's Role wears, with the tint it wears it in: an instance of the photographed
// master taking a "Color" tint like the surface master's. Null when the table gives the role no surface, or,
// with a warning logged, when the surface hasn't been built.
FORGOTTENTHIRD_API UMaterialInterface* LoadSurfaceFor(const ftcore::SurfaceTable& Table, ftcore::WorldKind Kind,
	ftcore::SurfaceRole Role, FLinearColor& OutTint);
}
