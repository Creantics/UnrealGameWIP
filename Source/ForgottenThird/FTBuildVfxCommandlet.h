#pragma once

// Builds the Niagara systems for the VFX effect specs from code:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildVfx -NullRHI -unattended -nopause -nosplash
//
// Reads every Data/Vfx/*.json through ftcore::importVfxEffect and writes /Game/VFX/NS_<name>, one
// Niagara Lightweight (stateless) emitter per spec emitter, starting from the engine's lightweight
// system template. Existing files are replaced. See docs/VFX_UE.md.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildVfxCommandlet.generated.h"

UCLASS()
class UFTBuildVfxCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildVfxCommandlet();

	virtual int32 Main(const FString& Params) override;
};
