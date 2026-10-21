#pragma once

// Imports the delivered music and sound files by cue id, checked against the mix rules, so audio
// needs no editor clicks:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTImportAudio [-Cue=<id>] [-Check]
//       -NullRHI -unattended -nopause -nosplash
//
// Every cue in Data/Audio/cues.json (or just -Cue) is looked for at Data/Audio/<file> (default
// <Bus>/<CueId>.wav). Each file there is decoded (ftcore::readWav), measured (ftcore::measureLoudness:
// integrated LUFS and true peak) and held to its bus's rules (ftcore::checkClip, docs/ASSET_INTAKE.md);
// one that passes is imported as /Game/Audio/<Bus>/<CueId> with looping set from the cue's playback.
// -Check measures and reports without importing. A cue whose file isn't there yet is listed, not an
// error. Returns 1 when the manifest is bad, a file breaks a rule, or an import fails.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTImportAudioCommandlet.generated.h"

UCLASS()
class UFTImportAudioCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTImportAudioCommandlet();

	virtual int32 Main(const FString& Params) override;
};
