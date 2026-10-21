#pragma once

// Shared by FTImportAudio and FTValidateAssets, editor builds only. Each logs under Tag and names
// files by cue or line id only.
//
// CheckClipFile decodes a .wav (ftcore::readWav), measures it (ftcore::measureLoudness) and holds it
// to its bus's rules (ftcore::checkClip; Cue is null for a voice line), logging the measurements and
// every issue. False when it can't be read or breaks a rule. OutSeconds gets the clip's length.
//
// CheckLipSyncFile checks a voice line's Data/Voice/<LineId>.nface when there is one: it must parse,
// and a length more than 0.25 s off the audio's is a warning. False when it doesn't parse.
// bOutFound says whether the file is there.

#include "CoreMinimal.h"

#if WITH_EDITOR
#include "ftcore/AudioCues.h"

namespace FT
{
bool CheckClipFile(const TCHAR* Tag, const FString& Path, const FString& Id, ftcore::AudioBus Bus, const ftcore::AudioCue* Cue,
	double& OutSeconds);
bool CheckLipSyncFile(const TCHAR* Tag, const FString& Id, double AudioSeconds, bool& bOutFound);
} // namespace FT
#endif
