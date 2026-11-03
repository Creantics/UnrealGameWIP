#include "FTLookSubsystem.h"

#include "Engine/World.h"
#include "FTLookGrade.h"
#include "FTSceneLook.h"
#include "FTText.h"
#include "ForgottenThird.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "SceneViewExtension.h"

#include "ftcore/LookTable.h"

#include <string>
#include <vector>

namespace
{
// How long to wait for the level's look actors before giving up (the arena spawns its own a moment after the world
// begins).
constexpr float kLookWaitSeconds = 10.f;

TAutoConsoleVariable<int32> CVarLook(TEXT("r.FT.Look"), 1,
	TEXT("The per-world photoreal look: 1 applies the world's look from Data/Look/looks.json when a map starts, 0 keeps the shared look (as -LookOff)."),
	ECVF_Default);

// Data/Look/looks.json, read once. Empty when it's missing or refused (the problems logged).
const ftcore::LookTable& LookTable()
{
	static ftcore::LookTable Table;
	static bool bLoaded = false;
	if (bLoaded)
	{
		return Table;
	}
	bLoaded = true;
	const FString File = FPaths::ProjectDir() / TEXT("Data/Look/looks.json");
	if (!IFileManager::Get().FileExists(*File))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Look: Data/Look/looks.json is missing; every world keeps the shared look."));
		return Table;
	}
	TArray<uint8> Bytes;
	std::string Error;
	std::vector<std::string> Warnings;
	if (!FFileHelper::LoadFileToArray(Bytes, *File) || !ftcore::importLookTable(FT::BytesAsText(Bytes), Table, &Error, &Warnings))
	{
		Table = ftcore::LookTable{};
		UE_LOG(LogForgottenThird, Error, TEXT("Data/Look/looks.json refused: %s"), *FT::ToFString(Error).TrimEnd());
		return Table;
	}
	for (const std::string& Warning : Warnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Data/Look/looks.json: %s"), *FT::ToFString(Warning));
	}
	return Table;
}

// ft.Look <world|off>: switches the look of the world the console is in.
void LookCommand(const TArray<FString>& Args, UWorld* World)
{
	UFTLookSubsystem* Look = World ? World->GetSubsystem<UFTLookSubsystem>() : nullptr;
	if (!Look)
	{
		return;
	}
	if (Args.Num() == 0)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("ft.Look: %s"), Look->GetWorldLook().IsEmpty() ? TEXT("off") : *Look->GetWorldLook());
		return;
	}
	if (!Look->SetWorldLook(Args[0]))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("ft.Look: no world '%s' in Data/Look/looks.json (solhaven|lattice|unwritten|off)."), *Args[0]);
	}
}

FAutoConsoleCommandWithWorldAndArgs GLookCommand(TEXT("ft.Look"),
	TEXT("Sets the photoreal look: ft.Look solhaven|lattice|unwritten|off. With no argument, prints the one on."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LookCommand));

}

bool UFTLookSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE) && Super::ShouldCreateSubsystem(Outer);
}

void UFTLookSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UWorld* World = GetWorld();
	Grade = FSceneViewExtensions::NewExtension<FFTLookGradeExtension>(World);

	// -Look=<world> picks it; otherwise the map's name does. -LookOff (or r.FT.Look 0) keeps the shared look.
	FString Want;
	if (FParse::Param(FCommandLine::Get(), TEXT("LookOff")) || CVarLook.GetValueOnGameThread() == 0)
	{
		Want = TEXT("off");
	}
	else if (!FParse::Value(FCommandLine::Get(), TEXT("Look="), Want))
	{
		const FTCHARToUTF8 Map(*World->GetMapName());
		const std::string_view Found = ftcore::lookWorldForMap(std::string_view(Map.Get(), static_cast<size_t>(Map.Length())));
		Want = Found.empty() ? TEXT("off") : FT::ToFString(std::string(Found));
	}
	if (!SetWorldLook(Want))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Look: -Look=%s is not a world in Data/Look/looks.json; keeping the shared look."), *Want);
	}
}

void UFTLookSubsystem::Deinitialize()
{
	// The registry holds extensions weakly: dropping ours unregisters it.
	Grade.Reset();
	Super::Deinitialize();
}

bool UFTLookSubsystem::SetWorldLook(const FString& World)
{
	ftcore::SceneLook Look;
	FString Id;
	if (!World.Equals(TEXT("off"), ESearchCase::IgnoreCase) && !World.Equals(TEXT("none"), ESearchCase::IgnoreCase) && !World.IsEmpty())
	{
		const FTCHARToUTF8 Utf8(*World);
		const ftcore::SceneLook* Found = LookTable().find(std::string_view(Utf8.Get(), static_cast<size_t>(Utf8.Length())));
		if (!Found)
		{
			return false;
		}
		Look = *Found;
		Id = World.ToLower();
	}
	WorldId = Id;
	Wanted = Look;
	PushGrade();
	// The scene's actors may not be there yet; Tick keeps trying.
	bWaiting = true;
	Waited = 0.f;
	Tick(0.f);
	return true;
}

void UFTLookSubsystem::Tick(float DeltaTime)
{
	if (!bWaiting)
	{
		return;
	}
	if (Wanted == Applied)
	{
		bWaiting = false;
		return;
	}
	const int32 Written = FTScene::ApplyLook(GetWorld(), Wanted, Applied);
	if (Written >= 0)
	{
		Applied = Wanted;
		bWaiting = false;
		UE_LOG(LogForgottenThird, Display, TEXT("Look: %s (%d settings written; grade %s)."), WorldId.IsEmpty() ? TEXT("shared look") : *WorldId,
			Written, Grade.IsValid() && Grade->IsGrading() ? TEXT("on") : TEXT("off"));
		return;
	}
	Waited += DeltaTime;
	if (Waited > kLookWaitSeconds)
	{
		bWaiting = false;
		UE_LOG(LogForgottenThird, Warning, TEXT("Look: no level post-process volume after %.0f s; %s's light and camera finish not applied (the grade still is)."),
			kLookWaitSeconds, *WorldId);
	}
}

TStatId UFTLookSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFTLookSubsystem, STATGROUP_Tickables);
}

void UFTLookSubsystem::SetFightAccent(float HueDegrees, float WidthDegrees, float Chroma)
{
	Accent.hueDeg = HueDegrees;
	Accent.widthDeg = WidthDegrees;
	Accent.chroma = Chroma;
	PushGrade();
}

void UFTLookSubsystem::ClearFightAccent()
{
	Accent = ftcore::FightAccent{};
	PushGrade();
}

void UFTLookSubsystem::PushGrade()
{
	if (!Grade.IsValid())
	{
		return;
	}
	const ftcore::GradeConstants Packed = ftcore::gradeConstants(Wanted, Accent);
	FFTLookGradeConstants Constants;
	for (int32 Index = 0; Index < 5; ++Index)
	{
		for (int32 Lane = 0; Lane < 4; ++Lane)
		{
			Constants.C[Index][Lane] = Packed.c[Index][Lane];
		}
	}
	Constants.bActive = !ftcore::gradeIsIdentity(Packed);
	Grade->SetConstants(Constants);
}
