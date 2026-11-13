#pragma once

// Builds the look's materials from code:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildLookMaterials -AllowCommandletRendering -unattended -nopause
//       -nosplash
//
// -AllowCommandletRendering compiles each material's shaders, so a broken custom node fails the build; under -NullRHI
// the materials are saved unchecked.
// Writes, under /Game/Look (FTScene::LookCollection and the FTScene::Look*Material packages):
// - MPC_Look, the parameter collection a world's look fills at run time (FTScene::ApplyLookParameters): its wetness, its
//   water (absorption and scattering per metre, phase, roughness, wave size, speed and strength) and its glow. The
//   defaults are ftcore::SceneLook{}'s, and each parameter's id is made from its name, so a rebuild keeps every material
//   that reads it.
// - M_Look_Water: the engine's single layer water (Substrate converts it), its normal from two layers of noise panning
//   across each other on the world position, calmer and rougher where the water is shallow against what it lies on.
// - M_Look_Glass: clear glass, translucent and lit per pixel, refracting at an index of 1.5, with front-layer
//   reflections allowed.
// - M_Look_Chrome: a polished metal mirror.
// - M_Look_WetStone: a photographed stone (Data/Field/surfaces.json's cobblestone) that darkens, deepens, turns glossy
//   and fills its hollows with standing water as the look's wetness rises.
// - M_Look_Glow: a dark panel that glows in the look's glow colour.
// - M_Look_CanalWater: the look's water with a flow, for a canal's surface: ripples and streaks of scum carried along
//   the channel (fastest in its middle, slack at the walls) by two copies half a period apart, so nothing resets in
//   sight. The map's builder gives each canal an instance with its flow and channel (ftcore::RoadWater).
// - M_Look_CanalBed: a canal's walls and bed: silt, stones and weed on the faces looking up; concrete with a dark wet
//   band and a line of growth at the waterline on the others.
// Every file is replaced, and nothing is saved if a material doesn't compile. See docs/PHOTOREAL_LOOK.md.
// - M_Look_Underwater: the view from under a water surface, a post-process the water subsystem switches on while the
//   view's eye is in a body of water: it cuts at the waterline per pixel with a meniscus, takes what is seen through
//   the water toward the water's own colour by its kind (darker and bluer with depth and distance), dims with the
//   eye's depth, wobbles a little, lays the sun's caustics on what is under water, stands shafts of light and drifting
//   specks in it, and shows the surface from below as a bright window overhead and, beyond the critical angle, a
//   mirror of what is under water.
// - MPC_Water (FTScene::LookWaterCollection): what stirs the water, written each frame by the water subsystem: eight
//   rings, the lead's place and wake, the sun's way under water; and the strengths of the rings, the wake and the
//   waterline. M_Look_Water, M_Look_CanalWater and M_Look_Underwater read it, so every water plane shows the lead's
//   rings, wake and waterline with nothing set per plane.
// -Canal builds the two canal materials alone on the collections as they stand, and saves nothing else; -Underwater
// likewise the underwater view; -Water the water collection and the three materials that read it.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildLookMaterialsCommandlet.generated.h"

UCLASS()
class UFTBuildLookMaterialsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildLookMaterialsCommandlet();

	virtual int32 Main(const FString& Params) override;
};
