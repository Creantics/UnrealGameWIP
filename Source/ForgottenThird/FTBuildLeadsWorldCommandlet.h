#pragma once

// Builds the leads map (/Game/Maps/Leads) from code: the three lead characters side by side,
// facing a camera that frames all three, each looping its idle clip.
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildLeadsWorld -NullRHI -unattended -nopause -nosplash
//
// It needs the characters imported first (FTImportCharacter into /Game/Characters/{Devin,Duy,Third};
// see docs/LEADS.md). Each stands on an AFTCharacterPreview whose Body plays <name>idle in
// single-node mode (so the jaw and tongue bones stay at rest; the face morphs still work) and
// whose face holds one expression. The floor, sun, sky and fog are the same as the demo map's.
// It overwrites the map, and returns 1 when a mesh or clip is missing or the save fails.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildLeadsWorldCommandlet.generated.h"

UCLASS()
class UFTBuildLeadsWorldCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildLeadsWorldCommandlet();

	virtual int32 Main(const FString& Params) override;
};
