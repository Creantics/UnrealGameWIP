#include "FTCutsceneGameMode.h"

#include "CineCameraActor.h"
#include "EngineUtils.h"
#include "FTCutsceneDirector.h"
#include "FTCutsceneHUD.h"
#include "FTGameInstance.h"
#include "FTText.h"
#include "ForgottenThird.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "ftcore/StubScreen.h"

#include <string>

namespace
{
const FString kNoShotCutscene;
} // namespace

AFTCutsceneGameMode::AFTCutsceneGameMode()
{
	HUDClass = AFTCutsceneHUD::StaticClass();
	PrimaryActorTick.bCanEverTick = true;
}

void AFTCutsceneGameMode::LoadPlan(const FString& Scene, int32 Beat, int32 Step, ftcore::CutscenePlan& Out) const
{
	Out.clear();
	const FString Path = FPaths::ProjectDir() / TEXT("Data/Cutscene/cutscenes.json");
	TArray<uint8> Bytes;
	ftcore::CutsceneTable Table;
	std::string Error;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Cutscene: %s not found; playing one wide shot."), *Path);
	}
	else if (!ftcore::importCutsceneTable(FT::BytesAsText(Bytes), Table, &Error))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Cutscene: cutscenes.json refused (%s); playing one wide shot."), UTF8_TO_TCHAR(Error.c_str()));
	}
	else
	{
		const uint32 BeatIndex = static_cast<uint32>(FMath::Max(Beat, 0));
		const uint32 StepIndex = static_cast<uint32>(FMath::Max(Step, 0));
		Out = Table.find(FT::ToUtf8(Scene), BeatIndex, StepIndex);
		return;
	}
	ftcore::CutsceneShot Shot;
	Shot.setSpot("spot_a");
	Shot.setSubject("subj_a");
	Shot.framing = ftcore::ShotFraming::Wide;
	Shot.duration = 3.f;
	Out.add(Shot);
}

void AFTCutsceneGameMode::StartPlay()
{
	// Skips AFTGameMode::StartPlay on purpose, as the stub, battle and exploration modes do: the story
	// must not start here. This also begins play for the map's actors (the director among them).
	AGameModeBase::StartPlay();

	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	const FFTStoryCarry* Carry = (GI && GI->StoryCarry.bValid && !GI->StoryCarry.bDone) ? &GI->StoryCarry : nullptr;
	bHasStep = Carry != nullptr;

	ftcore::CutscenePlan Plan;
	if (Carry)
	{
		// The title comes from Core, whatever the step's token is (CG: "Cutscene").
		const FString& Token = Carry->StepToken.IsEmpty() ? Carry->Kind : Carry->StepToken;
		TitleText = FT::ToFString(ftcore::stubTitle(FT::ToUtf8(Token)));
		StepText = FString::Printf(TEXT("Step %d of %d"), Carry->Step + 1, FMath::Max(Carry->StepCount, 1));
		LoadPlan(Carry->Scene, Carry->Beat, Carry->Step, Plan);
	}
	else
	{
		TitleText = FT::ToFString(ftcore::stubTitle("CG"));
		StepText = TEXT("No story step (preview of the default shots)");
		LoadPlan(FString(), -1, 0, Plan);
	}
	ShotTexts.Reset(static_cast<int32>(Plan.size()));
	for (uint32 I = 0; I < Plan.size(); ++I)
	{
		ShotTexts.Add(FString::Printf(TEXT("Shot %u of %u"), I + 1, Plan.size()));
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	for (TActorIterator<AFTCutsceneDirector> It(World); It; ++It)
	{
		Director = *It;
		break;
	}
	if (!Director)
	{
		FActorSpawnParameters Spawn;
		Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Director = World->SpawnActor<AFTCutsceneDirector>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
	}
	if (!Director)
	{
		return;
	}
	Director->Play(Plan, bHasStep && Carry->bAutoPlay);

	// Look through the director's camera rather than the pawn.
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (ACineCameraActor* Camera = Director->GetCamera())
		{
			PC->SetViewTarget(Camera);
		}
	}
}

const FString& AFTCutsceneGameMode::GetShotText() const
{
	if (!Director || !Director->IsStarted())
	{
		return kNoShotCutscene;
	}
	const int32 Index = static_cast<int32>(Director->GetPlayer().currentIndex());
	return ShotTexts.IsValidIndex(Index) ? ShotTexts[Index] : kNoShotCutscene;
}

void AFTCutsceneGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Director || !Director->IsStarted() || bReturning)
	{
		return;
	}
	const ftcore::CutscenePlayer& Player = Director->GetPlayer();
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (PC && Player.playing()
		&& (PC->WasInputKeyJustPressed(EKeys::SpaceBar) || PC->WasInputKeyJustPressed(EKeys::Enter)
			|| PC->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Bottom)))
	{
		Director->Press();
	}
	if (Player.finished())
	{
		Finish();
	}
}

void AFTCutsceneGameMode::Finish()
{
	if (bReturning)
	{
		return;
	}
	bReturning = true;
	if (!bHasStep)
	{
		return; // a preview: no carry to hand back, stay on the map
	}
	UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (!GI || !GI->StoryCarry.bValid)
	{
		return;
	}
	FFTStoryCarry& Carry = GI->StoryCarry;
	Carry.bDone = true;
	Carry.bWon = true;
	// Ids and counts only.
	const ftcore::CutscenePlayer& Player = Director->GetPlayer();
	UE_LOG(LogForgottenThird, Display, TEXT("Cutscene: done %s beat %d step %d shots %u skipped %d"), *Carry.Scene, Carry.Beat,
		Carry.Step, Player.shotCount(), Player.skipped() ? 1 : 0);
	OpenReturnMap();
}

void AFTCutsceneGameMode::OpenReturnMap()
{
	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	if (GI && GI->StoryCarry.bValid && !GI->StoryCarry.ReturnMap.IsEmpty())
	{
		UGameplayStatics::OpenLevel(this, FName(*GI->StoryCarry.ReturnMap));
	}
}
