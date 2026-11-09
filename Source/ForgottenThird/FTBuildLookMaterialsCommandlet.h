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
// Every file is replaced, and nothing is saved if a material doesn't compile. See docs/PHOTOREAL_LOOK.md.

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
