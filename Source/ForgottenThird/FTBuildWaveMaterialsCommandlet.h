#pragma once

// Builds the world wave's assets from code (FTWorldWave.h has the names):
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildWaveMaterials -unattended -nopause -nosplash
//
// First adds the wave's parameters to the look's collection, /Game/Look/MPC_Look, where they are missing (saved in
// place, never deleted; FTBuildLookMaterials builds them in too). Then writes, under /Game/Look/Wave:
//   MF_WorldWaveCut          the cut a master wires in (FTWorldWave::AddCut): outputs Clip, Ink, Core
//   PP_WorldWave_Ink         the post-process (after tonemapping, priority 10, after the world style): the second world
//                            drawn as ink line, crosshatch and paper inside the front, the band's displace, smear and
//                            rim, and the prelude's streak; outside the front it passes the input through
//   M_FTCharacter_WaveCut    twins of the prop masters with the cut wired in, for the props the wave dressing tags
//   M_FTCharacterTex_WaveCut
// Files under /Game/Look/Wave are replaced. Then rebuild the masters that wire the cut in themselves:
//   -run=FTImportPlant -Masters                  (foliage and bark)
//   -run=FTImportVehicle -Source=<the car's export> (the kei car's body, clear coat and glass)

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildWaveMaterialsCommandlet.generated.h"

UCLASS()
class UFTBuildWaveMaterialsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildWaveMaterialsCommandlet();

	virtual int32 Main(const FString& Params) override;
};
