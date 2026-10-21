#include "FTValidateAssetsCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "FTAudioChecks.h"
#include "FTAudioSubsystem.h"
#include "FTImportMaterials.h"
#include "FTText.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ftcore/AssetManifest.h"
#include "ftcore/StoryImport.h"

#include <string>
#include <string_view>
#include <vector>

namespace
{
const TCHAR* const GTag = TEXT("FTValidateAssets");

struct FValidator
{
	IAssetRegistry* Registry = nullptr;
	TArray<FString> CsvRows;
	int32 Failed = 0;
	bool bManifestError = false;

	bool HasAssets(const FString& Folder) const
	{
		return Registry->HasAssets(FName(*Folder), true);
	}

	void Record(const TCHAR* Category, const FString& Id, ftcore::AssetState State, ftcore::AssetTally& Tally)
	{
		Tally.add(State);
		Failed += State == ftcore::AssetState::Failed ? 1 : 0;
		CsvRows.Add(FString::Printf(TEXT("%s,%s,%s"), Category, *Id, ANSI_TO_TCHAR(ftcore::assetStateName(State))));
	}

	void Summary(const TCHAR* Category, const ftcore::AssetTally& Tally) const
	{
		UE_LOG(LogForgottenThird, Display, TEXT("%s: %s: %s."), GTag, Category, *FT::ToFString(Tally.summary()));
	}

	void Audio()
	{
		ftcore::AudioCueTable Cues;
		FString Error;
		if (!UFTAudioSubsystem::LoadCueTable(Cues, Error))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("%s: audio: %s"), GTag, *Error);
			bManifestError = true;
			return;
		}
		const FString Root = FPaths::ProjectDir() / TEXT("Data/Audio");
		ftcore::AssetTally Tally;
		for (const ftcore::AudioCue& Cue : Cues.cues)
		{
			const FString Id = FT::ToFString(Cue.id);
			const FString Path = FPaths::ConvertRelativePathToFull(Root / FT::ToFString(Cue.file));
			const bool bPresent = FPaths::FileExists(Path);
			double Seconds = 0.0;
			const bool bOk = !bPresent || FT::CheckClipFile(GTag, Path, Id, Cue.bus, &Cue, Seconds);
			const bool bImported = FPackageName::DoesPackageExist(UFTAudioSubsystem::AssetFolderFor(Cue) / Id);
			Record(TEXT("audio"), Id, ftcore::assetState(bPresent, bOk ? 0 : 1, bImported), Tally);
		}
		Summary(TEXT("audio"), Tally);
	}

	void Voice()
	{
		const FString Dir = FPaths::ProjectDir() / TEXT("Data/Story");
		FString StoryText;
		if (!FFileHelper::LoadFileToString(StoryText, *(Dir / TEXT("story.json")), FFileHelper::EHashOptions::None, FILEREAD_Silent))
		{
			UE_LOG(LogForgottenThird, Display, TEXT("%s: voice: no Data/Story here; skipped."), GTag);
			return;
		}
		const std::string Story = FT::ToUtf8(StoryText);
		std::vector<std::string> SceneIds;
		std::string Error;
		if (!ftcore::storySceneIds(Story, SceneIds, &Error))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("%s: voice: %s"), GTag, *FT::ToFString(Error));
			bManifestError = true;
			return;
		}
		std::vector<std::string> Scenes(SceneIds.size());
		std::vector<std::string_view> Views(SceneIds.size());
		for (size_t I = 0; I < SceneIds.size(); ++I)
		{
			FString Text;
			const FString Path = Dir / TEXT("scenes") / FT::ToFString(SceneIds[I]) + TEXT(".json");
			if (!FFileHelper::LoadFileToString(Text, *Path))
			{
				UE_LOG(LogForgottenThird, Error, TEXT("%s: voice: can't read %s."), GTag, *Path);
				bManifestError = true;
				return;
			}
			Scenes[I] = FT::ToUtf8(Text);
			Views[I] = Scenes[I];
		}
		ftcore::StoryDatabase Db;
		if (!ftcore::importStory(Story, Views, Db, &Error))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("%s: voice: the story doesn't import."), GTag);
			bManifestError = true;
			return;
		}

		const FString VoiceDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Data/Voice"));
		ftcore::AssetTally Tally;
		int32 NoTrack = 0;
		for (const std::string& LineId : ftcore::voiceLineIds(Db))
		{
			const FString Id = FT::ToFString(LineId);
			const FString Path = VoiceDir / Id + TEXT(".wav");
			const bool bPresent = FPaths::FileExists(Path);
			bool bOk = true;
			if (bPresent)
			{
				double Seconds = 0.0;
				bool bTrack = false;
				bOk = FT::CheckClipFile(GTag, Path, Id, ftcore::AudioBus::Voice, nullptr, Seconds);
				bOk = FT::CheckLipSyncFile(GTag, Id, Seconds, bTrack) && bOk;
				NoTrack += bTrack ? 0 : 1;
			}
			const bool bImported = FPackageName::DoesPackageExist(TEXT("/Game/Voice/") + Id);
			Record(TEXT("voice"), Id, ftcore::assetState(bPresent, bOk ? 0 : 1, bImported), Tally);
		}
		Summary(TEXT("voice"), Tally);
		if (NoTrack > 0)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("%s: voice: %d voice files have no lip-sync track yet."), GTag, NoTrack);
		}
	}

	void Models()
	{
		const FString ManifestPath = FPaths::ProjectDir() / TEXT("Data/Assets/models.json");
		TArray<uint8> Bytes;
		ftcore::ModelTable Models;
		std::string Error;
		if (!FFileHelper::LoadFileToArray(Bytes, *ManifestPath) || !ftcore::importModelManifest(FT::BytesAsText(Bytes), Models, &Error))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("%s: models: %s"), GTag,
				Error.empty() ? *FString::Printf(TEXT("can't read %s"), *ManifestPath) : *FT::ToFString(Error).TrimEnd());
			bManifestError = true;
			return;
		}
		const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Data/Models"));
		ftcore::AssetTally Tally;
		for (const ftcore::ModelEntry& Model : Models.models)
		{
			const FString Id = FT::ToFString(Model.id);
			const FString Path = Root / FT::ToFString(Model.file);
			const bool bPresent = FPaths::FileExists(Path);
			const bool bOk = !bPresent || FT::CheckGlbFile(GTag, Path, ftcore::modelRules(Model));
			const FString Folder = (Model.kind == ftcore::ModelKind::Character ? TEXT("/Game/Characters/") : TEXT("/Game/Props/")) + Id;
			Record(TEXT("models"), Id, ftcore::assetState(bPresent, bOk ? 0 : 1, HasAssets(Folder)), Tally);
		}
		Summary(TEXT("models"), Tally);
	}
};
} // namespace
#endif

UFTValidateAssetsCommandlet::UFTValidateAssetsCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTValidateAssetsCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString Category, Csv;
	FParse::Value(*Params, TEXT("Category="), Category);
	FParse::Value(*Params, TEXT("Csv="), Csv);
	if (!Category.IsEmpty() && Category != TEXT("audio") && Category != TEXT("voice") && Category != TEXT("models"))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: -Category is audio, voice or models."), GTag);
		return 1;
	}

	FValidator V;
	V.Registry = &FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	V.Registry->SearchAllAssets(true);
	if (Category.IsEmpty() || Category == TEXT("audio"))
	{
		V.Audio();
	}
	if (Category.IsEmpty() || Category == TEXT("voice"))
	{
		V.Voice();
	}
	if (Category.IsEmpty() || Category == TEXT("models"))
	{
		V.Models();
	}

	if (!Csv.IsEmpty())
	{
		const FString Path = FPaths::IsRelative(Csv) ? FPaths::ProjectDir() / Csv : Csv;
		V.CsvRows.Insert(TEXT("category,id,state"), 0);
		if (!FFileHelper::SaveStringArrayToFile(V.CsvRows, *Path))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("%s: couldn't write %s."), GTag, *Path);
			return 1;
		}
		UE_LOG(LogForgottenThird, Display, TEXT("%s: wrote %d rows to %s."), GTag, V.CsvRows.Num() - 1, *FPaths::ConvertRelativePathToFull(Path));
	}
	UE_LOG(LogForgottenThird, Display, TEXT("%s: %d failed%s."), GTag, V.Failed, V.bManifestError ? TEXT(", a manifest is bad") : TEXT(""));
	return V.Failed == 0 && !V.bManifestError ? 0 : 1;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTValidateAssets needs an editor build."));
	return 1;
#endif
}
