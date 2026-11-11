#include "FTLookSubsystem.h"

#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FTLookGrade.h"
#include "FTSceneLook.h"
#include "FTText.h"
#include "FTWorldClockSubsystem.h"
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

// Data/Look/looks.json's render settings, once per run: each console variable set at the game-setting priority, under
// the project's ini and the command line (-dpcvars=), which keep theirs.
void ApplyRenderSettings()
{
	static bool bApplied = false;
	if (bApplied)
	{
		return;
	}
	bApplied = true;
	for (const ftcore::LookTable::RenderSetting& Setting : LookTable().render)
	{
		const FString Name = FT::ToFString(Setting.name);
		IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(*Name);
		if (!Variable)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Look: no console variable %s; its render setting is skipped."), *Name);
			continue;
		}
		const FString Before = Variable->GetString();
		Variable->Set(*FString::SanitizeFloat(Setting.value), ECVF_SetByGameSetting);
		UE_LOG(LogForgottenThird, Display, TEXT("Look: %s %s -> %s."), *Name, *Before, *Variable->GetString());
	}
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
	if (!FParse::Param(FCommandLine::Get(), TEXT("LookOff")) && CVarLook.GetValueOnGameThread() != 0)
	{
		ApplyRenderSettings();
	}

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

void UFTLookSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	// The world sets up its parameter collections after its subsystems, swapping a fresh instance (at the collection's
	// defaults) for the one Initialize wrote into: write the look's parameters again now that the world's are final.
	bParametersPending = true;
	bWaiting = true;
	Tick(0.f);
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
	bParametersPending = true;
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
	// The materials' parameters need only the world's collection instance, not the look actors.
	if (bParametersPending)
	{
		ParametersWritten = FTScene::ApplyLookParameters(GetWorld(), Wanted);
		bParametersPending = ParametersWritten < 0;
	}
	if (Wanted != Applied)
	{
		const int32 Written = FTScene::ApplyLook(GetWorld(), Wanted, Applied);
		if (Written >= 0)
		{
			Applied = Wanted;
			ApplyStillDayShade();
			UE_LOG(LogForgottenThird, Display, TEXT("Look: %s (%d settings written; grade %s; %s)."),
				WorldId.IsEmpty() ? TEXT("shared look") : *WorldId, Written, Grade.IsValid() && Grade->IsGrading() ? TEXT("on") : TEXT("off"),
				bParametersPending ? TEXT("material parameters waiting")
					: ParametersWritten == 0 ? TEXT("no material parameter collection built")
					: *FString::Printf(TEXT("%d material parameters written"), ParametersWritten));
		}
	}
	if (Wanted == Applied && !bParametersPending)
	{
		bWaiting = false;
		return;
	}
	Waited += DeltaTime;
	if (Waited > kLookWaitSeconds)
	{
		bWaiting = false;
		if (Wanted != Applied)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Look: no level post-process volume after %.0f s; %s's light and camera finish not applied (the grade still is)."),
				kLookWaitSeconds, *WorldId);
		}
		if (bParametersPending)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Look: the world has no instance of %s after %.0f s; the look's material parameters stay at their defaults."),
				FTScene::LookCollection, kLookWaitSeconds);
		}
		bParametersPending = false;
	}
}

void UFTLookSubsystem::ApplyStillDayShade()
{
	// A running clock writes the day's shadow floor itself; this is for a day world-eye still, which runs none.
	UWorld* World = GetWorld();
	const float Amount = WorldId.IsEmpty() ? -1.f : FTDayShade::StillAmount();
	if (!World || Amount < 0.f)
	{
		return;
	}
	const ftcore::DayShadeFrame Shade = ftcore::dayShadeFrame(FTDayShade::Look(), FTDayShade::BaseFor(Wanted), Amount);
	// The look's own sky light (the one ApplyLook writes), scaled.
	for (TActorIterator<ASkyLight> It(World); It; ++It)
	{
		if (USkyLightComponent* SkyLight = It->GetLightComponent())
		{
			SkyLight->SetIntensity(Wanted.skyLight * Shade.skyScale);
			break;
		}
	}
	int32 Fogs = 0;
	for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
	{
		if (UExponentialHeightFogComponent* Fog = It->GetComponent())
		{
			FTDayShade::WriteFog(*Fog, Shade);
			++Fogs;
		}
	}
	const bool bPost = FTDayShade::WritePost(*World, Shade);
	UE_LOG(LogForgottenThird, Display, TEXT("Look: day shade %.2f for the still (sky light %.2f, shadow contrast %.2f, highlight contrast %.2f, diffuse boost %.2f, toe %.2f, exposure floor EV %.2f%s; fog cap %.2f and sky share %.2f %.2f %.2f on %d fogs)."),
		Amount, Wanted.skyLight * Shade.skyScale, Shade.shadowContrast, Shade.highlightContrast, Shade.diffuseBoost, Shade.filmToe,
		Shade.minEV100, bPost ? TEXT("") : TEXT("; no look volume"), Shade.fogMaxOpacity, Shade.fogSky[0], Shade.fogSky[1], Shade.fogSky[2], Fogs);
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
