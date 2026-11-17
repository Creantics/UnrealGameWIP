#include "FTRainSubsystem.h"

#include "FTVfxComponent.h"
#include "ForgottenThird.h"

#include "Camera/PlayerCameraManager.h"
#include "CollisionQueryParams.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Parse.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"

#include "ftcore/LookTable.h"

#include <string_view>

namespace
{
TAutoConsoleVariable<float> CVarFTRainTarget(TEXT("r.FT.Rain"), -1.f,
	TEXT("Falling rain (FTRainSubsystem.h): 0..1 ramps the rain toward that intensity; below 0 keeps -Rain=. Only in a world started with -Rain > 0."),
	ECVF_Default);

constexpr float kRainTraceCm = 3000.f;   // the floor trace, straight down from the camera
constexpr float kRainSplashLiftCm = 3.f; // the splash disc just over the floor
constexpr float kRainTopFollow = 6.f;    // FInterpTo speed of the streak box's height (1/s)
constexpr float kRainWriteStep = 0.002f; // the intensity change that rewrites the rates

UNiagaraComponent* SpawnRainSystem(UWorld& World, UNiagaraSystem* System)
{
	// Not auto-destroyed, not auto-activated, not pooled; WriteParameters activates it.
	return UNiagaraFunctionLibrary::SpawnSystemAtLocation(&World, System, FVector::ZeroVector, FRotator::ZeroRotator, FVector::OneVector,
		false, false, ENCPoolMethod::None, false);
}

UNiagaraSystem* LoadRainSystem(const TCHAR* Effect)
{
	return LoadObject<UNiagaraSystem>(nullptr, *UFTVfxComponent::SystemPath(Effect), nullptr, LOAD_NoWarn | LOAD_Quiet);
}
} // namespace

bool UFTRainSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE) && Super::ShouldCreateSubsystem(Outer);
}

void UFTRainSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	float Want = 0.f;
	if (!FParse::Value(FTLookSwitches(), TEXT("Rain="), Want) || !(Want > 0.f))
	{
		return;
	}
	const FTCHARToUTF8 Map(*InWorld.GetMapName());
	if (ftcore::lookWorldForMap(std::string_view(Map.Get(), static_cast<size_t>(Map.Length()))) != "solhaven")
	{
		return;
	}
	Want = FMath::Min(Want, 1.f);
	SwitchTarget = Want;
	State = ftcore::rainSettled(Spec, Want);

	UNiagaraSystem* RainSystem = LoadRainSystem(TEXT("rain"));
	UE_LOG(LogForgottenThird, Display, TEXT("Rain: on at %.2f, NS_rain %s"), Want, RainSystem ? TEXT("loaded") : TEXT("missing"));
	if (!RainSystem)
	{
		UE_LOG(LogForgottenThird, Warning,
			TEXT("Rain: /Game/VFX/NS_rain is missing, so no rain falls. Build it with -run=FTBuildVfx -Only=rain (and -Only=rain_splash)."));
		return;
	}
	UNiagaraSystem* SplashSystem = LoadRainSystem(TEXT("rain_splash"));
	if (!SplashSystem)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Rain: /Game/VFX/NS_rain_splash is missing; the rain falls without splashes."));
	}

	Streaks = SpawnRainSystem(InWorld, RainSystem);
	if (!Streaks)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Rain: couldn't spawn NS_rain."));
		return;
	}
	Splash = SplashSystem ? SpawnRainSystem(InWorld, SplashSystem) : nullptr;
	bActive = true;
	WriteParameters();
}

void UFTRainSubsystem::WriteParameters()
{
	static const FName RateName(ftcore::kRainUserRate);
	static const FName RingRateName(ftcore::kRainUserRingRate);
	static const FName DropRateName(ftcore::kRainUserDropRate);
	static const FName ColorName(ftcore::kRainUserColor);

	const ftcore::RainFallFrame Frame = ftcore::rainFallFrame(Spec, Fall, State.intensity);
	const FLinearColor Color(1.f, 1.f, 1.f, Frame.streakAlpha);
	// Our own flag, not IsActive(): a deactivated system stays active while its last particles drain.
	const bool bOn = State.intensity > 0.f;
	const bool bSwitch = bOn != bSystemsOn;
	if (Streaks)
	{
		Streaks->SetVariableFloat(RateName, Frame.streakRate);
		Streaks->SetVariableLinearColor(ColorName, Color);
		if (bSwitch)
		{
			bOn ? Streaks->Activate(true) : Streaks->Deactivate();
		}
	}
	if (Splash)
	{
		Splash->SetVariableFloat(RingRateName, Frame.ringRate);
		Splash->SetVariableFloat(DropRateName, Frame.dropRate);
		Splash->SetVariableLinearColor(ColorName, Color);
		if (bSwitch)
		{
			bOn ? Splash->Activate(true) : Splash->Deactivate();
		}
	}
	bSystemsOn = bOn;
	WrittenIntensity = State.intensity;
}

void UFTRainSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	UWorld* World = GetWorld();
	if (!World || !Streaks)
	{
		return;
	}

	// The intensity: the switch, or r.FT.Rain when set, ramped.
	const float Override = CVarFTRainTarget.GetValueOnGameThread();
	State.target = Override >= 0.f ? FMath::Min(Override, 1.f) : SwitchTarget;
	State = ftcore::rainStep(Spec, State, DeltaTime);
	if (FMath::Abs(State.intensity - WrittenIntensity) > kRainWriteStep ||
		(State.intensity != WrittenIntensity && (State.intensity == 0.f || State.intensity == State.target)))
	{
		WriteParameters();
	}

	const APlayerController* PC = World->GetFirstPlayerController();
	if (!PC || !PC->PlayerCameraManager)
	{
		return;
	}
	const FVector Eye = PC->PlayerCameraManager->GetCameraLocation();

	// The floor under the camera; the last one stands when the trace finds none.
	FCollisionQueryParams Params(SCENE_QUERY_STAT(FTRainFloor), false);
	if (const APawn* Pawn = PC->GetPawn())
	{
		Params.AddIgnoredActor(Pawn);
	}
	if (const AActor* View = PC->GetViewTarget())
	{
		Params.AddIgnoredActor(View);
	}
	FHitResult Hit;
	if (World->LineTraceSingleByChannel(Hit, Eye, Eye - FVector(0.0, 0.0, kRainTraceCm), ECC_Visibility, Params))
	{
		FloorZ = static_cast<float>(Hit.ImpactPoint.Z);
		bHasFloor = true;
	}

	const float EyeZM = static_cast<float>(Eye.Z) * 0.01f;
	const float TopTarget = ftcore::rainColumnTop(Fall, EyeZM, bHasFloor, FloorZ * 0.01f) * 100.f;
	TopZ = bTopSet ? FMath::FInterpTo(TopZ, TopTarget, DeltaTime, kRainTopFollow) : TopTarget;
	bTopSet = true;
	Streaks->SetWorldLocation(FVector(Eye.X, Eye.Y, TopZ));

	if (Splash)
	{
		const bool bShow = ftcore::rainSplashOn(Fall, EyeZM, bHasFloor, FloorZ * 0.01f);
		if (bShow != bSplashShown)
		{
			Splash->SetVisibility(bShow);
			bSplashShown = bShow;
		}
		if (bShow)
		{
			Splash->SetWorldLocation(FVector(Eye.X, Eye.Y, FloorZ + kRainSplashLiftCm));
		}
	}
}

TStatId UFTRainSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFTRainSubsystem, STATGROUP_Tickables);
}
