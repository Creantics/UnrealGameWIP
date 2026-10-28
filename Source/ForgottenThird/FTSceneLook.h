#pragma once

// The scene's photographic base, spawned from one ftcore::SceneLook: a movable sun, a sky light that
// captures the atmosphere in real time, the sky atmosphere, exponential height fog (volumetric when the
// look has it), the engine's volumetric cloud layer, and an unbound post-process volume at priority 0
// that carries the camera finish (exposure range and bias, white balance, saturation, contrast, bloom,
// vignette, grain). Every map builder lights its scene through SpawnLook and keeps only its own sun angle
// and local lamps, so all the worlds share one look until each gets its own. The builders' blocks and props
// are tints of one surface master, loaded with LoadSurface. See docs/SCENE_LOOK.md.

#include "CoreMinimal.h"

#include "ftcore/SceneLook.h"

class UMaterialInterface;
class UWorld;
struct FActorSpawnParameters;

namespace FTScene
{
// The world surface master's package, built by FTBuildWorldMaterials.
inline constexpr const TCHAR* SurfaceMaterial = TEXT("/Game/Rendering/M_WorldSurface");

// Spawns the sun (pointing along SunRotation), sky light, atmosphere, fog, clouds and post-process volume
// into World, labelled Sun, SkyLight, SkyAtmosphere, HeightFog, VolumetricCloud and PostProcessVolume.
FORGOTTENTHIRD_API void SpawnLook(UWorld* World, const FActorSpawnParameters& Spawn, const ftcore::SceneLook& Look,
	const FRotator& SunRotation);

// Loads the world surface master: a "Color" vector held to a plausible albedo, "Roughness", "Metallic" and
// a low-frequency mottle on the world position ("Variation", "VariationScale"). Null, with an error logged,
// when it hasn't been built.
FORGOTTENTHIRD_API UMaterialInterface* LoadSurface();
}
