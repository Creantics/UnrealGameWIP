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
//   the sky's stars, skyglow and flat dome. The defaults are ftcore::SceneLook{}'s, and each parameter's id is made
//   from its name, so a rebuild keeps every material that reads it.
// - Env/M_Env_Sky: an unlit sky material for a sky dome (Env/Sky.ush): the atmosphere's own sky and sun disc through
//   the engine's sky atmosphere nodes, with stars, a city's skyglow and a featureless flat dome added over them. With
//   the defaults it draws the atmosphere alone.
// FTBuildLookMaterials and its collection are separate; the two builds can run in either order. Every file is replaced,
// and nothing is saved if a material doesn't compile. See docs/PHOTOREAL_LOOK.md.

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
