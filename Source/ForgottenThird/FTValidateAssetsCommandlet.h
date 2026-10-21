#pragma once

// Reports where every expected asset stands, by id, so the user can see what's left to deliver:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTValidateAssets [-Category=audio|voice|models]
//       [-Csv=Saved/AssetStatus.csv] -NullRHI -unattended -nopause -nosplash
//
// What's expected (ftcore/AssetManifest.h):
//   audio   every cue in Data/Audio/cues.json: Data/Audio/<file>, imported at /Game/Audio/<Bus>/<CueId>
//   voice   every line id in the local Data/Story: Data/Voice/<LineId>.wav (+ .nface), imported at
//           /Game/Voice/<LineId>. Skipped when there's no story here; nothing about it is committed.
//   models  every entry in Data/Assets/models.json: Data/Models/<file>, imported under
//           /Game/Characters/<ModelId> or /Game/Props/<ModelId> (FTImportCharacter / FTImportProp -Dest)
//
// Each delivered file goes through its importer's checks (FT::CheckClipFile, FT::CheckLipSyncFile,
// FT::CheckGlbFile) and each asset ends up missing, failed, delivered (passes, not imported yet) or
// imported. One summary line per category; -Csv also writes category,id,state per asset (keep it
// under Saved/: voice rows carry line ids). Returns 1 when a manifest is bad or a file breaks a rule;
// missing files are the normal state until assets arrive.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTValidateAssetsCommandlet.generated.h"

UCLASS()
class UFTValidateAssetsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTValidateAssetsCommandlet();

	virtual int32 Main(const FString& Params) override;
};
