#include "FTLookLabGameMode.h"

#include "Camera/CameraActor.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "ForgottenThird.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "ShaderCompiler.h"

namespace FTLookLab
{
// The settling step; the loop's step between frames; and the step while a shot is taken, so short that the scene holds.
constexpr double kStep = 1.0 / 48.0;
constexpr double kLoopStep = 1.0 / 24.0;
constexpr double kHoldStep = 1.0 / 100000.0;
// Ticks to settle a view before its still (3 s: exposure, Lumen, temporal AA), and after the map starts.
constexpr int32 kSettle = 144;
constexpr int32 kFirstSettle = 288;
constexpr int32 kLoopFrames = 192;
constexpr int32 kLoopView = 2;
const TCHAR* const kStillNames[] = {TEXT("1_wide"), TEXT("2_water"), TEXT("3_chrome")};
} // namespace FTLookLab

AFTLookLabGameMode::AFTLookLabGameMode()
{
	HUDClass = nullptr;
	bHostsStory = false;
	PrimaryActorTick.bCanEverTick = true;
}

FName AFTLookLabGameMode::ViewTag(int32 View)
{
	return FName(*FString::Printf(TEXT("FTLookLab%d"), View));
}

void AFTLookLabGameMode::StartPlay()
{
	// Skips AFTGameMode::StartPlay on purpose: the story must not start here.
	AGameModeBase::StartPlay();
	ShowView(1);
	if (FParse::Value(FCommandLine::Get(), TEXT("LookLabShot="), ShotPrefix) && !ShotPrefix.IsEmpty())
	{
		// A fixed step: the waves and the loop's frames land at the same times on every run, however slowly they render.
		FApp::SetUseFixedTimeStep(true);
		FApp::SetFixedDeltaTime(FTLookLab::kStep);
		Wait = FTLookLab::kFirstSettle;
		UE_LOG(LogForgottenThird, Display, TEXT("LookLab: shooting to %s."), *ShotPrefix);
	}
}

void AFTLookLabGameMode::ShowView(int32 View)
{
	APlayerController* Player = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!Player)
	{
		return;
	}
	for (TActorIterator<ACameraActor> It(GetWorld()); It; ++It)
	{
		if (It->ActorHasTag(ViewTag(View)))
		{
			Player->SetViewTarget(*It);
			return;
		}
	}
	UE_LOG(LogForgottenThird, Warning, TEXT("LookLab: the map has no camera tagged %s."), *ViewTag(View).ToString());
}

void AFTLookLabGameMode::Shoot(const FString& File, const TCHAR* Size)
{
	// The game viewport handles HighResShot; the engine's own Exec doesn't.
	if (UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr)
	{
		Viewport->Exec(GetWorld(), *FString::Printf(TEXT("HighResShot %s filename=\"%s\""), Size, *File), *GLog);
	}
	Pending = File;
	PendingTicks = 0;
}

void AFTLookLabGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (ShotPrefix.IsEmpty())
	{
		return;
	}
	// A material still compiling draws as the engine's default: nothing counts until every shader is in.
	if (GShaderCompilingManager && GShaderCompilingManager->GetNumRemainingJobs() > 0)
	{
		Wait = FMath::Max(Wait, FTLookLab::kSettle);
		return;
	}
	if (!Pending.IsEmpty())
	{
		// Nothing moves on until the last shot is on disk (a minute at most).
		if (!IFileManager::Get().FileExists(*Pending) && ++PendingTicks < 6000)
		{
			return;
		}
		if (PendingTicks >= 6000)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("LookLab: %s never arrived."), *Pending);
		}
		Pending.Reset();
		// A still is taken frames after it's asked for, so the next view comes in only once it's saved.
		if (NextView > 0)
		{
			ShowView(NextView);
			NextView = 0;
			Wait = FTLookLab::kSettle;
			return;
		}
	}
	if (Wait > 0)
	{
		--Wait;
		return;
	}
	constexpr int32 Stills = UE_ARRAY_COUNT(FTLookLab::kStillNames);
	if (Stage < Stills)
	{
		Shoot(FString::Printf(TEXT("%s_%s.png"), *ShotPrefix, FTLookLab::kStillNames[Stage]), TEXT("1920x1080"));
		UE_LOG(LogForgottenThird, Display, TEXT("LookLab: still %s."), FTLookLab::kStillNames[Stage]);
		++Stage;
		NextView = Stage < Stills ? Stage + 1 : FTLookLab::kLoopView;
		return;
	}
	if (Frame < FTLookLab::kLoopFrames)
	{
		if (!bAdvanced)
		{
			// The next frame moves the scene on by exactly a 24th of a second.
			FApp::SetFixedDeltaTime(FTLookLab::kLoopStep);
			bAdvanced = true;
			return;
		}
		// Then time all but stops while the shot is taken: a high-res shot waits frames of its own before it saves.
		FApp::SetFixedDeltaTime(FTLookLab::kHoldStep);
		Shoot(FString::Printf(TEXT("%s_loop_%04d.png"), *ShotPrefix, Frame), TEXT("1280x720"));
		++Frame;
		bAdvanced = false;
		return;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("LookLab: %d stills and %d loop frames taken; quitting."), Stills, Frame);
	ShotPrefix.Reset();
	FTRequestExit(0, TEXT("LookLab"));
}
