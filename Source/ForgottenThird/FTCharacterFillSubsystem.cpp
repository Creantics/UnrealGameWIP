#include "FTCharacterFillSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FTNightLook.h"
#include "FTWorldClockSubsystem.h"
#include "ForgottenThird.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
// Characters are looked for this often (seconds); the light is rewritten when it has moved this much.
constexpr float kScanSeconds = 0.5f;
constexpr float kLuxStep = 0.01f;   // a share of the fill
constexpr float kTurnStepDeg = 0.5f;

// Channel 1 on top of what a primitive had; true when it was added.
bool AddChannel(UPrimitiveComponent* Prim)
{
	if (!Prim || Prim->LightingChannels.bChannel1)
	{
		return false;
	}
	Prim->SetLightingChannels(Prim->LightingChannels.bChannel0, true, Prim->LightingChannels.bChannel2);
	return true;
}
} // namespace

bool UFTCharacterFillSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE) &&
		FParse::Param(FTLookSwitches(), TEXT("NightCharFill")) && Super::ShouldCreateSubsystem(Outer);
}

void UFTCharacterFillSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	bStillNight = FParse::Param(FCommandLine::Get(), TEXT("WorldEyeNight"));
	bActive = true;
}

void UFTCharacterFillSubsystem::Spawn(UWorld& World)
{
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ADirectionalLight* Actor = World.SpawnActor<ADirectionalLight>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
	UDirectionalLightComponent* L = Actor ? Cast<UDirectionalLightComponent>(Actor->GetLightComponent()) : nullptr;
	if (!L)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("CharacterFill: no light spawned; off."));
		bActive = false;
		return;
	}
	Actor->Tags.Add(LightTag);
	L->SetMobility(EComponentMobility::Movable);
	// Characters only, no shadows, no part in the sky, the fog, the indirect light or the atmosphere's sun.
	L->SetLightingChannels(false, true, false);
	L->SetCastShadows(false);
	L->SetAtmosphereSunLight(false);
	L->SetVolumetricScatteringIntensity(0.f);
	L->bAffectGlobalIllumination = false;
	L->SetUseTemperature(true);
	L->SetTemperature(FTNight::Look().charFillKelvin);
	L->SetLightColor(FLinearColor::White, false);
	L->SetIntensity(0.f);
	L->MarkRenderStateDirty();
	Light = L;
}

int32 UFTCharacterFillSubsystem::MarkCharacters(UWorld& World)
{
	int32 Added = 0;
	for (TActorIterator<ACharacter> It(&World); It; ++It)
	{
		TInlineComponentArray<UPrimitiveComponent*, 64> Prims;
		It->GetComponents(Prims, true);
		for (UPrimitiveComponent* Prim : Prims)
		{
			Added += AddChannel(Prim) ? 1 : 0;
		}
		// Garments and hair worn as attached actors.
		It->ForEachAttachedActors([&Added](AActor* Worn)
		{
			TInlineComponentArray<UPrimitiveComponent*, 64> WornPrims;
			Worn->GetComponents(WornPrims, true);
			for (UPrimitiveComponent* Prim : WornPrims)
			{
				Added += AddChannel(Prim) ? 1 : 0;
			}
			return true;
		});
	}
	return Added;
}

void UFTCharacterFillSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	if (!Light.IsValid())
	{
		Spawn(*World);
		if (!Light.IsValid())
		{
			return;
		}
	}
	ScanIn -= DeltaTime;
	if (ScanIn <= 0.f)
	{
		ScanIn = kScanSeconds;
		Marked += MarkCharacters(*World);
	}
	const ftcore::NightLook& Spec = FTNight::Look();
	const UFTWorldClockSubsystem* Clock = World->GetSubsystem<UFTWorldClockSubsystem>();
	const float Night = Clock && Clock->IsRunning() ? Clock->GetNightAmount() : (bStillNight ? 1.f : 0.f);
	const float Lux = ftcore::nightCharacterFillLux(Spec, Night);
	const APlayerController* Player = World->GetFirstPlayerController();
	const APlayerCameraManager* Camera = Player ? Player->PlayerCameraManager.Get() : nullptr;
	FRotator Rotation = Camera ? Camera->GetCameraRotation() : FRotator::ZeroRotator;
	Rotation.Pitch = FMath::Clamp(Rotation.Pitch + Spec.charFillPitchDeg, -89.f, 89.f);
	Rotation.Roll = 0.f;
	UDirectionalLightComponent* L = Light.Get();
	// Compared with the light's own values, so another writer (a night still's pass over the directional lights) is undone.
	const float Full = FMath::Max(ftcore::nightCharacterFillLux(Spec, 1.f), 1e-4f);
	if (FMath::Abs(L->Intensity - Lux) >= kLuxStep * Full || (Lux == 0.f && L->Intensity != 0.f))
	{
		L->SetIntensity(Lux);
		L->SetVisibility(Lux > 0.f);
		L->SetLightColor(FLinearColor::White, false);
	}
	if (!L->GetComponentRotation().Equals(Rotation, kTurnStepDeg))
	{
		L->SetWorldRotation(Rotation);
	}
	if (!bLogged && Marked > 0)
	{
		bLogged = true;
		UE_LOG(LogForgottenThird, Display, TEXT("CharacterFill: %.3f lux at night %.2f (%.1f stops under the lamp key at %.1f m), %.0f K, %.0f degrees below the view; channel 1 on %d primitives."),
			Lux, Night, Spec.charFillStops, Spec.charFillRefM, Spec.charFillKelvin, -Spec.charFillPitchDeg, Marked);
	}
}

TStatId UFTCharacterFillSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFTCharacterFillSubsystem, STATGROUP_Tickables);
}
