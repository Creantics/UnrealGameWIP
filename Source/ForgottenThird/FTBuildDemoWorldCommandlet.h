#pragma once

// Builds the demo map (/Game/Maps/Demo) from code, so the map is never hand-made:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildDemoWorld -NullRHI -unattended -nopause -nosplash
//
// It writes a floor, a sun, a sky light, sky atmosphere, height fog, a player start, an
// AFTCharacterPreview at the origin, a few BasicShapes props, an unbound post-process volume and an
// AFTStyleController. Options:
//   -Mesh=<skeletal mesh object path>  the preview character, turned to face the player
//   -Style=<spec>                      none, solhaven (default), lattice, unwritten, restored, inksolhaven
//                                      or a dial such as S3,U0.5 (ftcore::parseStyleSpec)
// The style needs /Game/Rendering/PP_Style: run FTBuildStyleMaterials first. It overwrites the map, so
// run it again after changing it here.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildDemoWorldCommandlet.generated.h"

UCLASS()
class UFTBuildDemoWorldCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildDemoWorldCommandlet();

	virtual int32 Main(const FString& Params) override;
};
