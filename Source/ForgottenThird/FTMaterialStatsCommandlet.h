#pragma once

// What the world wave's surface state costs each master that wears it (MF_WorldWaveState, wired in by
// FTWorldWave::AddState):
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTMaterialStats -unattended -nopause -nosplash [-Masters=<path>;<path>]
//
// For each master (by default the surface, textured surface, ground, foliage and bark masters) it compiles the master
// as saved and a transient copy with the state call bypassed (each input the call fed takes what fed the call), and
// logs the pixel and vertex shader instruction counts, samplers and texture samples of both, for the editor's shader
// platform. Nothing is saved.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTMaterialStatsCommandlet.generated.h"

UCLASS()
class UFTMaterialStatsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTMaterialStatsCommandlet();

	virtual int32 Main(const FString& Params) override;
};
