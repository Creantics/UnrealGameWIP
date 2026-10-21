#include "FTAudioChecks.h"

#if WITH_EDITOR
#include "ForgottenThird.h"
#include "FTAudioSubsystem.h"
#include "FTText.h"
#include "Misc/FileHelper.h"
#include "ftcore/Face.h"
#include "ftcore/Loudness.h"
#include "ftcore/Wav.h"

#include <string>
#include <vector>

namespace
{
void LogIssues(const TCHAR* Tag, const FString& Id, const std::vector<ftcore::AssetIssue>& Issues)
{
	for (const ftcore::AssetIssue& Issue : Issues)
	{
		if (Issue.severity == ftcore::AssetSeverity::Error)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("%s: %s: %s: %s"), Tag, *Id, *FT::ToFString(Issue.code), *FT::ToFString(Issue.message));
		}
		else
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("%s: %s: %s: %s"), Tag, *Id, *FT::ToFString(Issue.code), *FT::ToFString(Issue.message));
		}
	}
}
} // namespace

namespace FT
{
bool CheckClipFile(const TCHAR* Tag, const FString& Path, const FString& Id, ftcore::AudioBus Bus, const ftcore::AudioCue* Cue,
	double& OutSeconds)
{
	OutSeconds = 0.0;
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %s: can't read %s."), Tag, *Id, *Path);
		return false;
	}
	ftcore::WavInfo Info;
	std::vector<float> Samples;
	std::string Error;
	if (!ftcore::readWav(FT::BytesAsText(Bytes), Info, &Samples, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %s: %s"), Tag, *Id, *FT::ToFString(Error));
		return false;
	}
	OutSeconds = Info.durationSeconds();
	const ftcore::Loudness Loud = ftcore::measureLoudness(Samples.data(), Info.frames, Info.channels, Info.sampleRate);
	UE_LOG(LogForgottenThird, Display, TEXT("%s: %s: %u Hz, %u ch, %u-bit%s, %.2f s, %s LUFS, peak %.1f dBFS, true peak %.1f dBTP%s."),
		Tag, *Id, Info.sampleRate, Info.channels, Info.bitsPerSample, Info.isFloat ? TEXT(" float") : TEXT(""),
		Info.durationSeconds(), Loud.hasIntegrated ? *FString::Printf(TEXT("%.1f"), Loud.integratedLufs) : TEXT("n/a"),
		Loud.samplePeakDb, Loud.truePeakDb, Info.hasLoop ? TEXT(", smpl loop") : TEXT(""));

	const std::vector<ftcore::AssetIssue> Issues = ftcore::checkClip(Info, Loud, Bus, Cue);
	LogIssues(Tag, Id, Issues);
	return ftcore::countErrors(Issues) == 0;
}

bool CheckLipSyncFile(const TCHAR* Tag, const FString& Id, double AudioSeconds, bool& bOutFound)
{
	const FString Path = UFTAudioSubsystem::VoiceTrackFile(Id);
	bOutFound = !Path.IsEmpty();
	if (!bOutFound)
	{
		return true;
	}
	TArray<uint8> Bytes;
	ftcore::FaceTrack Track;
	std::string Error;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		Error = "can't read the file";
	}
	if (!Error.empty() ||
		!ftcore::parseFaceTrack(std::span<const std::uint8_t>(Bytes.GetData(), static_cast<size_t>(Bytes.Num())), Track, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %s: bad lip-sync track: %s"), Tag, *Id, *FT::ToFString(Error));
		return false;
	}
	std::vector<ftcore::AssetIssue> Issues;
	ftcore::checkLipSyncLength(AudioSeconds, Track.duration(), Issues);
	LogIssues(Tag, Id, Issues);
	return true;
}
} // namespace FT
#endif
