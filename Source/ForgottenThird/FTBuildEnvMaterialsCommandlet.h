#pragma once

// Builds the environment's materials from code:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildEnvMaterials -AllowCommandletRendering -unattended -nopause
//       -nosplash
//
// -AllowCommandletRendering compiles each material's shaders, so a broken custom node or include fails the build; under
// -NullRHI the materials are saved unchecked. Each material's shading code is an include in the FTLook plugin
// (Plugins/FTLook/Shaders/Private/Env, the virtual path /Plugin/FTLook/Private/Env) that its custom node calls into.
// Writes, under /Game/Look (FTScene::EnvCollection and the FTScene::Env*Material packages):
// - MPC_Env, the environment's parameter collection, filled at run time beside MPC_Look (FTScene::ApplyLookParameters):
//   the sky's stars, skyglow and flat dome, the wind, the clouds' cover, kind, density, drift and underglow, and the
//   sea's foam and its eight waves (ftcore::seaWaves, one vector each, and their shared steepness). The defaults are
//   ftcore::SceneLook{}'s, and each parameter's id is made from its name, so a rebuild keeps every material that reads
//   it.
// - Env/M_Env_Sky: an unlit sky material for a sky dome (Env/Sky.ush): the atmosphere's own sky and sun disc through
//   the engine's sky atmosphere nodes, with stars, a city's skyglow and a featureless flat dome added over them. With
//   the defaults it draws the atmosphere alone.
// - Env/M_Env_Clouds: a volume material for the volumetric cloud layer (Env/Clouds.ush): a weather map of cover that
//   drifts with the wind, the engine's tiling noise volumes for the shape and its worn edges, a cheap bound the engine
//   marches by, and a warm underglow a lit city puts on the cloud bases. FTScene::ApplyLook puts it on the layer when a
//   world has cloud cover and gives the engine's own back when the cover goes.
// - Env/M_Env_Sea: single-layer water for open water on a grid mesh (Env/Waves.ush): the swell and the wind's sea as
//   eight Gerstner waves that move the vertices and give each pixel its normal and whitecaps, the look water's ripples
//   over them, surf where it runs shallow, and the look water's colour and medium. Nothing places it yet.
// The sea reads MPC_Look too, so FTBuildLookMaterials runs first, once (its rebuilds keep the ids the sea reads); this
// build stops, replacing nothing, when MPC_Look is missing. Every file is replaced, and nothing is saved if a material
// doesn't compile. See docs/PHOTOREAL_LOOK.md.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildEnvMaterialsCommandlet.generated.h"

UCLASS()
class UFTBuildEnvMaterialsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildEnvMaterialsCommandlet();

	virtual int32 Main(const FString& Params) override;
};
