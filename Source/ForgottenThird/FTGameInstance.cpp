#include "FTGameInstance.h"

#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include "ForgottenThird.h"
#include "FTStorySubsystem.h"
#include "FTText.h"
#include "FTUiText.h"

void UFTGameInstance::Init()
{
	// Creates the subsystems.
	Super::Init();
	bAlphaFast = FParse::Param(FCommandLine::Get(), TEXT("AlphaFast"));
	ReloadStory();
	LoadWorlds();
	LoadEncounters();
	// Load the interface text now, so a broken table shows up at launch (and in headless runs, which never draw).
	FTUiText::Culture();
}

void UFTGameInstance::LoadWorlds()
{
	Worlds = ftcore::WorldTable{};
	const FString File = FPaths::ProjectDir() / TEXT("Data/Field/worlds.json");
	TArray<uint8> Bytes;
	if (!IFileManager::Get().FileExists(*File) || !FFileHelper::LoadFileToArray(Bytes, *File))
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Worlds: no Data/Field/worlds.json; field steps use the Explore map."));
		return;
	}
	std::string Error;
	std::vector<std::string> Warnings;
	if (!ftcore::importWorldTable(FT::BytesAsText(Bytes), Worlds, &Error, &Warnings))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Worlds: Data/Field/worlds.json refused; field steps use the Explore map: %s"),
			*FT::ToFString(Error).TrimEnd());
		return;
	}
	for (const std::string& Warning : Warnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Worlds: %s"), *FT::ToFString(Warning));
	}
	UE_LOG(LogForgottenThird, Log, TEXT("Worlds: %d worlds, %d scenes placed."), static_cast<int32>(Worlds.worlds.size()),
		static_cast<int32>(Worlds.scenes.size()));
}

void UFTGameInstance::LoadEncounters()
{
	Roster = ftcore::EnemyRoster{};
	Encounters = ftcore::EncounterTable{};
	const FString Dir = FPaths::ProjectDir() / TEXT("Data/Battle");
	TArray<uint8> EnemyBytes, EncounterBytes;
	if (!FFileHelper::LoadFileToArray(EnemyBytes, *(Dir / TEXT("enemies.json")))
		|| !FFileHelper::LoadFileToArray(EncounterBytes, *(Dir / TEXT("encounters.json"))))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Encounters: Data/Battle not found under %s; every fight is a Turns fight."), *Dir);
		return;
	}
	std::string Error;
	if (!ftcore::importEnemyRoster(FT::BytesAsText(EnemyBytes), Roster, &Error)
		|| !ftcore::importEncounterTable(FT::BytesAsText(EncounterBytes), Roster, Encounters, &Error))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Encounters: Data/Battle refused; every fight is a Turns fight: %s"), *FT::ToFString(Error).TrimEnd());
		return;
	}
	int32 Kinds[ftcore::kCombatKinds] = {};
	for (const ftcore::EncounterEntry& E : Encounters.entries)
	{
		++Kinds[static_cast<int32>(E.combat)];
	}
	UE_LOG(LogForgottenThird, Log, TEXT("Encounters: %d listed (%d turns, %d arena, %d beat)."), static_cast<int32>(Encounters.entries.size()),
		Kinds[static_cast<int32>(ftcore::CombatKind::Rhythm)], Kinds[static_cast<int32>(ftcore::CombatKind::Arena)],
		Kinds[static_cast<int32>(ftcore::CombatKind::Beat)]);
}

bool UFTGameInstance::ReloadStory()
{
	UFTStorySubsystem* Story = GetSubsystem<UFTStorySubsystem>();
	if (!Story)
	{
		return false;
	}

	FString Error;
	TArray<FString> Warnings;
	const bool bOk = Story->LoadStory(StoryDir, Error, Warnings);
	for (const FString& Warning : Warnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Story: %s"), *Warning);
	}
	if (bOk)
	{
		UE_LOG(LogForgottenThird, Log, TEXT("Story loaded from %s"), *StoryDir);
	}
	else
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Story not loaded from %s: %s"), *StoryDir, *Error);
	}
	return bOk;
}
