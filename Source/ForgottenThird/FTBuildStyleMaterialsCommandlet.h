#pragma once

// Builds the world-style post-process material and its pack instances from code:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildStyleMaterials -NullRHI -unattended -nopause -nosplash
//
// Writes /Game/Rendering/PP_Style (post-process domain, after tonemapping: one Custom HLSL node fed by
// a parameter per FTStyleParameters entry) and MI_Style_Solhaven, _Lattice, _Unwritten, _Restored and
// _InkSolhaven, whose values come from ftcore::stylePack / inkSolhavenStyle. Existing files are
// replaced. See docs/STYLE_PACKS_UE.md.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildStyleMaterialsCommandlet.generated.h"

UCLASS()
class UFTBuildStyleMaterialsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildStyleMaterialsCommandlet();

	virtual int32 Main(const FString& Params) override;
};
