// FTBuildVanMaterials: builds the party van's two masters from code.
//   M_VanBody: the body's texture with glass in the window band and the wear the van has picked up (dirt, and scuffed and
//   scratched panels per zone), each a scalar the van sets on its instance.
//   M_VanLamp: a lamp lens, its colour and glow set per lamp.
// Run: UnrealEditor-Cmd <project> -run=FTBuildVanMaterials -nullrhi -unattended
#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildVanMaterialsCommandlet.generated.h"

UCLASS()
class UFTBuildVanMaterialsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildVanMaterialsCommandlet();

	virtual int32 Main(const FString& Params) override;
};
