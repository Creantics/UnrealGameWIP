#pragma once

// Builds the demo map (/Game/Maps/Demo) from code, so the map is never hand-made:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildDemoWorld -NullRHI -unattended -nopause -nosplash
//
// It writes a floor, the shared photoreal look (FTSceneLook.h: sun, sky light, atmosphere, fog, clouds and
// an unbound post-process volume), a player start, an AFTCharacterPreview at the origin and a few
// BasicShapes props, plus three AFTVfxActor effects beside the character (ember_swirl, heal_ring, smoke)
// and a physics corner of simulating props with the /Game/Physics materials, off to the side.
// Options:
//   -Mesh=<skeletal mesh object path>  the preview character, turned to face the player
//   -Style=<spec>                      adds an AFTStyleController over the look: solhaven, lattice,
//                                      unwritten, restored, inksolhaven or a dial such as S3,U0.5
//                                      (ftcore::parseStyleSpec); none (the default) leaves it out
//   -NoVfx                             leave the effects out
//   -NoPhysics                         leave the physics corner out
// A style needs /Game/Rendering/PP_Style: run FTBuildStyleMaterials first. The effects need
// /Game/VFX/NS_*: run FTBuildVfx first. The physics corner needs /Game/Physics/PM_*: run FTBuildPhysics
// first. It overwrites the map, so run it again after changing it here.

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
