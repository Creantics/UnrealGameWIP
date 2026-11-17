#include "FTCharacterFillSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/LocalLightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FTNightLook.h"
#include "FTWorldClockSubsystem.h"
#include "ForgottenThird.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
// Characters are looked for this often (seconds); the light is rewritten when it has moved this much.
constexpr float kScanSeconds = 0.5f;
constexpr float kLuxStep = 0.01f;   // a share of the fill
constexpr float kTurnStepDeg = 0.5f;
constexpr int32 kBayLooks = 20;     // scans spent looking for lit bay fronts before giving up (until a level streams in)

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
	FTDemoBootNoteWorld(Outer); // this world's boot row, not the last one's (see ForgottenThird.h)
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE) &&
		FParse::Param(FTLookSwitches(), TEXT("NightCharFill")) && Super::ShouldCreateSubsystem(Outer);
}

void UFTCharacterFillSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	bStillNight = FParse::Param(FCommandLine::Get(), TEXT("WorldEyeNight"));
	bActive = true;
	// The hero corner arrives as a streamed sub-level: its front keys are looked for again once a level comes in.
	LevelAddedHandle = FWorldDelegates::LevelAddedToWorld.AddWeakLambda(this, [this](ULevel*, UWorld* Into)
	{
		if (Into == GetWorld())
		{
			bBaysFound = false;
			BayLooksLeft = kBayLooks;
		}
	});
}

void UFTCharacterFillSubsystem::Deinitialize()
{
	FWorldDelegates::LevelAddedToWorld.Remove(LevelAddedHandle);
	LevelAddedHandle.Reset();
	Super::Deinitialize();
}

void UFTCharacterFillSubsystem::FindBays(UWorld& World)
{
	BayFronts.Reset();
	LocalLights.Reset();
	FollowedLight = INDEX_NONE;
	for (TActorIterator<AActor> It(&World); It; ++It)
	{
		if (It->ActorHasTag(FTNight::HeroFrontTag))
		{
			BayFronts.Add(It->GetActorLocation());
		}
		TInlineComponentArray<ULocalLightComponent*, 8> Lights;
		It->GetComponents(Lights);
		for (const ULocalLightComponent* Local : Lights)
		{
			if (Local && Local->IsVisible() && Local->Intensity > 0.f)
			{
				LocalLights.Add(Local->GetComponentLocation());
			}
		}
	}
	bBaysFound = !BayFronts.IsEmpty();
	if (bBaysFound)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("CharacterFill: %d lit bay fronts; the fill is gone within %.1f m of one and whole from %.1f m. Key %.0f%% at %.0f degrees off the view toward the nearest of %d local lights."),
			BayFronts.Num(), FTNight::Look().charFillBayNearM, FTNight::Look().charFillBayFarM,
			100.f * ftcore::nightCharacterFillKeyShare(FTNight::Look()), FTNight::Look().charFillYawDeg, LocalLights.Num());
	}
}

float UFTCharacterFillSubsystem::BayDistanceM(const UWorld& World) const
{
	const APlayerController* Player = World.GetFirstPlayerController();
	const APawn* Pawn = Player ? Player->GetPawn() : nullptr;
	if (!Pawn || BayFronts.IsEmpty())
	{
		return -1.f;
	}
	const FVector At = Pawn->GetActorLocation();
	double Best = TNumericLimits<double>::Max();
	for (const FVector& Front : BayFronts)
	{
		Best = FMath::Min(Best, FVector::DistSquared2D(At, Front));
	}
	return static_cast<float>(FMath::Sqrt(Best) / 100.0);
}

FVector2D UFTCharacterFillSubsystem::ToFollowedLight(const UWorld& World)
{
	const APlayerController* Player = World.GetFirstPlayerController();
	const APawn* Pawn = Player ? Player->GetPawn() : nullptr;
	if (!Pawn || LocalLights.IsEmpty())
	{
		return FVector2D::ZeroVector;
	}
	const FVector At = Pawn->GetActorLocation();
	double Best = TNumericLimits<double>::Max();
	int32 Nearest = 0;
	for (int32 I = 0; I < LocalLights.Num(); ++I)
	{
		const double D = FVector::DistSquared2D(At, LocalLights[I]);
		if (D < Best)
		{
			Best = D;
			Nearest = I;
		}
	}
	const float FollowedM = LocalLights.IsValidIndex(FollowedLight)
		? static_cast<float>(FVector::Dist2D(At, LocalLights[FollowedLight]) / 100.0) : -1.f;
	if (Nearest != FollowedLight
		&& ftcore::nightCharacterFillSwitchLight(FTNight::Look(), FollowedM, static_cast<float>(FMath::Sqrt(Best) / 100.0)))
	{
		FollowedLight = Nearest;
	}
	const FVector& Local = LocalLights[FollowedLight];
	return FVector2D(Local.X - At.X, Local.Y - At.Y);
}

void UFTCharacterFillSubsystem::Spawn(UWorld& World)
{
	Light = SpawnOne(World);
	Ambient = SpawnOne(World);
	if (!Light.IsValid() || !Ambient.IsValid())
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("CharacterFill: no light spawned; off."));
		bActive = false;
	}
}

UDirectionalLightComponent* UFTCharacterFillSubsystem::SpawnOne(UWorld& World)
{
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ADirectionalLight* Actor = World.SpawnActor<ADirectionalLight>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
	UDirectionalLightComponent* L = Actor ? Cast<UDirectionalLightComponent>(Actor->GetLightComponent()) : nullptr;
	if (!L)
	{
		return nullptr;
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
	return L;
}

void UFTCharacterFillSubsystem::Write(UDirectionalLightComponent& L, float Lux, const FRotator& Rotation, float Full)
{
	// Compared with the light's own values, so another writer (a night still's pass over the directional lights) is undone.
	if (FMath::Abs(L.Intensity - Lux) >= kLuxStep * Full || (Lux == 0.f && L.Intensity != 0.f))
	{
		L.SetIntensity(Lux);
		L.SetVisibility(Lux > 0.f);
		L.SetLightColor(FLinearColor::White, false);
	}
	if (!L.GetComponentRotation().Equals(Rotation, kTurnStepDeg))
	{
		L.SetWorldRotation(Rotation);
	}
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
	if (!Light.IsValid() || !Ambient.IsValid())
	{
		Spawn(*World);
		if (!Light.IsValid() || !Ambient.IsValid())
		{
			return;
		}
	}
	ScanIn -= DeltaTime;
	if (ScanIn <= 0.f)
	{
		ScanIn = kScanSeconds;
		Marked += MarkCharacters(*World);
		// A map without a hero corner: a few looks (10 s), then none until a level streams in.
		if (!bBaysFound && BayLooksLeft > 0)
		{
			--BayLooksLeft;
			FindBays(*World);
		}
	}
	const ftcore::NightLook& Spec = FTNight::Look();
	const UFTWorldClockSubsystem* Clock = World->GetSubsystem<UFTWorldClockSubsystem>();
	const float Night = Clock && Clock->IsRunning() ? Clock->GetNightAmount() : (bStillNight ? 1.f : 0.f);
	const float Lux = ftcore::nightCharacterFillLux(Spec, Night) * ftcore::nightCharacterFillShare(Spec, BayDistanceM(*World));
	const APlayerController* Player = World->GetFirstPlayerController();
	const APlayerCameraManager* Camera = Player ? Player->PlayerCameraManager.Get() : nullptr;
	FRotator Rotation = Camera ? Camera->GetCameraRotation() : FRotator::ZeroRotator;
	Rotation.Pitch = FMath::Clamp(Rotation.Pitch + Spec.charFillPitchDeg, -89.f, 89.f);
	Rotation.Roll = 0.f;
	const float Full = FMath::Max(ftcore::nightCharacterFillLux(Spec, 1.f), 1e-4f);
	const float KeyShare = ftcore::nightCharacterFillKeyShare(Spec);
	const FVector View = Rotation.Vector();
	const FVector2D To = ToFollowedLight(*World);
	const float TargetYaw = ftcore::nightCharacterFillYaw(Spec, static_cast<float>(View.X), static_cast<float>(View.Y),
		static_cast<float>(To.X), static_cast<float>(To.Y));
	// Eased, so the key doesn't jump sides in one frame when the followed light changes or crosses the view line.
	KeyYawOffset = bKeyYawSet ? ftcore::nightCharacterFillEaseYaw(Spec, KeyYawOffset, TargetYaw, DeltaTime) : TargetYaw;
	bKeyYawSet = true;
	FRotator KeyRotation = Rotation;
	KeyRotation.Yaw += KeyYawOffset;
	Write(*Light.Get(), Lux * KeyShare, KeyRotation, Full);
	Write(*Ambient.Get(), Lux * (1.f - KeyShare), Rotation, Full);
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
