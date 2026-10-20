#include "FTCutsceneDirector.h"

#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "ForgottenThird.h"
#include "FTText.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "ftcore/StubScreen.h"

namespace
{
constexpr float kCutsceneCmPerMetre = 100.f;

ftcore::CutsceneVec3 CutsceneToCore(const FVector& Cm)
{
	return {static_cast<float>(Cm.X) / kCutsceneCmPerMetre, static_cast<float>(Cm.Y) / kCutsceneCmPerMetre, static_cast<float>(Cm.Z) / kCutsceneCmPerMetre};
}

FVector CutsceneToUnreal(const ftcore::CutsceneVec3& M)
{
	return FVector(M.x * kCutsceneCmPerMetre, M.y * kCutsceneCmPerMetre, M.z * kCutsceneCmPerMetre);
}

// The lens of each framing, in millimetres on the cine camera's default filmback.
float CutsceneFocalLength(ftcore::ShotFraming Framing)
{
	switch (Framing)
	{
	case ftcore::ShotFraming::Wide: return 24.f;
	case ftcore::ShotFraming::Medium: return 35.f;
	case ftcore::ShotFraming::Close: return 50.f;
	case ftcore::ShotFraming::OverShoulder: return 40.f;
	case ftcore::ShotFraming::TwoShot: return 30.f;
	case ftcore::ShotFraming::Insert: return 50.f;
	}
	return 35.f;
}
} // namespace

AFTCutsceneDirector::AFTCutsceneDirector()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AFTCutsceneDirector::EnsureCamera()
{
	if (Camera || !GetWorld())
	{
		return;
	}
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Camera = GetWorld()->SpawnActor<ACineCameraActor>(GetActorLocation(), FRotator::ZeroRotator, Spawn);
}

void AFTCutsceneDirector::Play(const ftcore::CutscenePlan& Plan, bool bAutoPlay)
{
	EnsureCamera();
	if (Camera)
	{
		if (UCineCameraComponent* Cine = Camera->GetCineCameraComponent())
		{
			// Blockouts stay sharp: no depth of field.
			Cine->FocusSettings.FocusMethod = ECameraFocusMethod::Disable;
		}
	}
	bFast = FParse::Param(FCommandLine::Get(), TEXT("AlphaFast"));
	Player.begin(Plan, bAutoPlay);
	ResolveMarkers();
	LastRealTime = FPlatformTime::Seconds();
	AppliedShot = -1;
	bStarted = true;
	ApplyCamera();
}

bool AFTCutsceneDirector::Press()
{
	return Player.press();
}

bool AFTCutsceneDirector::FindMarker(std::string_view Id, bool bSubject, FVector& Out) const
{
	if (Id.empty() || !GetWorld())
	{
		return false;
	}
	const FName Tag(*FT::ToFString(Id));
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (!It->ActorHasTag(Tag))
		{
			continue;
		}
		if (!bSubject)
		{
			Out = It->GetActorLocation();
			return true;
		}
		FVector Origin, Extent;
		It->GetActorBounds(false, Origin, Extent);
		Out = Extent.IsNearlyZero() ? It->GetActorLocation() : FVector(Origin.X, Origin.Y, Origin.Z - Extent.Z);
		return true;
	}
	return false;
}

void AFTCutsceneDirector::ResolveMarkers()
{
	// Each missing id is logged once.
	TArray<FString, TInlineAllocator<8>> Missing;
	const auto Lookup = [&](std::string_view Id, bool bSubject, FVector& Out)
	{
		if (FindMarker(Id, bSubject, Out))
		{
			return true;
		}
		const FString Name = FT::ToFString(Id);
		if (!Missing.Contains(Name))
		{
			Missing.Add(Name);
			UE_LOG(LogForgottenThird, Warning, TEXT("Cutscene: no actor tagged %s in the map."), *Name);
		}
		return false;
	};

	const ftcore::CutscenePlan& Plan = Player.plan();
	for (uint32 I = 0; I < Plan.size(); ++I)
	{
		const ftcore::CutsceneShot& Shot = Plan.shot(I);
		FVector Subject = FVector::ZeroVector;
		Lookup(Shot.subject(), true, Subject);
		FVector Spot;
		if (!Lookup(Shot.spot(), false, Spot))
		{
			Spot = Subject + FVector(400.f, 0.f, 170.f);
		}
		FVector Subject2 = FVector::ZeroVector;
		bShotHasSubject2[I] = Shot.hasSubject2() && Lookup(Shot.subject2(), true, Subject2);
		ShotSpot[I] = CutsceneToCore(Spot);
		ShotSubject[I] = CutsceneToCore(Subject);
		ShotSubject2[I] = CutsceneToCore(Subject2);
	}
}

void AFTCutsceneDirector::ApplyCamera()
{
	if (!Camera || Player.shotCount() == 0)
	{
		return;
	}
	const uint32 Index = Player.currentIndex();
	const ftcore::CutsceneShot& Shot = Player.currentShot();
	ftcore::CameraPose Pose = ftcore::cutsceneCamera(Shot, Player.shotProgress(), ShotSpot[Index], ShotSubject[Index],
		bShotHasSubject2[Index] ? &ShotSubject2[Index] : nullptr);
	float Focal = CutsceneFocalLength(Shot.framing);
	if (Player.blending() && Index > 0)
	{
		const uint32 Prev = Index - 1;
		const ftcore::CameraPose From = ftcore::cutsceneCamera(Player.previousShot(), 1.f, ShotSpot[Prev], ShotSubject[Prev],
			bShotHasSubject2[Prev] ? &ShotSubject2[Prev] : nullptr);
		const float W = Player.blendWeight();
		Pose = ftcore::lerpPose(From, Pose, W);
		Focal = FMath::Lerp(CutsceneFocalLength(Player.previousShot().framing), Focal, W);
		AppliedShot = -1; // set the lens again once the blend is over
	}
	else if (AppliedShot == static_cast<int32>(Index))
	{
		Focal = -1.f; // already set
	}

	const FVector Position = CutsceneToUnreal(Pose.position);
	const FVector Look = CutsceneToUnreal(Pose.lookAt);
	const FVector Forward = Look - Position;
	Camera->SetActorLocationAndRotation(Position, Forward.IsNearlyZero() ? Camera->GetActorRotation() : Forward.Rotation());
	if (Focal > 0.f)
	{
		if (UCineCameraComponent* Cine = Camera->GetCineCameraComponent())
		{
			Cine->SetCurrentFocalLength(Focal);
		}
		if (!Player.blending())
		{
			AppliedShot = static_cast<int32>(Index);
		}
	}
}

void AFTCutsceneDirector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bStarted || !Player.playing())
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	const double Real = Now - LastRealTime;
	LastRealTime = Now;

	const AWorldSettings* Settings = GetWorld() ? GetWorld()->GetWorldSettings() : nullptr;
	const double Dilation = Settings ? Settings->GetEffectiveTimeDilation() : 1.0;
	Player.update(static_cast<double>(DeltaSeconds) * ftcore::StubScreen::speedFactor(bFast, Dilation));
	Player.tickReal(Real);
	ApplyCamera();
}
