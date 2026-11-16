#include "FTRimLight.h"

#include "ForgottenThird.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SpotLightComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Containers/StringConv.h"

#include <string_view>

#include "ftcore/LookReview.h"

namespace
{
TAutoConsoleVariable<float> CVarRimLight(TEXT("ft.RimLight"), 0.f,
	TEXT("Character rim light: 0 off (the old look), above 0 on at that intensity scale."), ECVF_Default);
}

UFTRimLightComponent::UFTRimLightComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork; // after the camera has moved this frame
}

// Made on the first tick that wants it, so with ft.RimLight 0 nothing is added (the old look exactly) and figures that
// never carry it hold no light.
void UFTRimLightComponent::MakeLight()
{
	UWorld* World = GetWorld();
	if (Light || !World || !World->IsGameWorld() || !GetOwner())
	{
		return;
	}
	AppliedScale = -1.f;
	Light = NewObject<USpotLightComponent>(GetOwner(), NAME_None, RF_Transient);
	Light->SetMobility(EComponentMobility::Movable);
	Light->SetUsingAbsoluteLocation(true);
	Light->SetUsingAbsoluteRotation(true);
	Light->SetIntensityUnits(ELightUnits::Candelas);
	Light->SetIntensity(0.f);
	Light->SetLightColor(Color);
	Light->SetAttenuationRadius(Back + 400.f);
	Light->SetInnerConeAngle(20.f);
	Light->SetOuterConeAngle(35.f);
	Light->SetCastShadows(false); // the body would shadow its own edge; and it stays cheap
	Light->SetAffectTranslucentLighting(false);
	Light->SetVolumetricScatteringIntensity(0.f); // no haze in the fog: height fog ignores lighting channels
	Light->SetAffectGlobalIllumination(false);     // no bounce off the character onto the ground and walls
	Light->SetLightingChannels(false, false, true);
	Light->SetVisibility(false);
	Light->RegisterComponent();
	ApplyChannels();
	ChannelTimer = 1.f;
}

void UFTRimLightComponent::OnUnregister()
{
	if (Light)
	{
		Light->DestroyComponent();
		Light = nullptr;
	}
	AppliedScale = -1.f;
	Super::OnUnregister();
}

void UFTRimLightComponent::ApplyChannels()
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}
	static const FName GroomClassName(TEXT("GroomComponent"));
	TInlineComponentArray<UPrimitiveComponent*> Prims(Owner);
	for (UPrimitiveComponent* P : Prims)
	{
		if (!P)
		{
			continue;
		}
		// The face piece and glasses stay off the channel (ftcore::look::rimLightSkipsPiece says why).
		const UObject* Mesh = nullptr;
		if (const USkeletalMeshComponent* Skel = Cast<USkeletalMeshComponent>(P))
		{
			Mesh = Skel->GetSkeletalMeshAsset();
		}
		else if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(P))
		{
			Mesh = Static->GetStaticMesh();
		}
		const FTCHARToUTF8 Name(Mesh ? *Mesh->GetName() : TEXT(""));
		const bool bGroom = P->GetClass()->GetFName() == GroomClassName;
		const bool bWant = !ftcore::look::rimLightSkipsPiece(std::string_view(Name.Get(), static_cast<size_t>(Name.Length())), bGroom);
		if (P->LightingChannels.bChannel2 != bWant)
		{
			P->SetLightingChannels(P->LightingChannels.bChannel0, P->LightingChannels.bChannel1, bWant);
		}
	}
}

void UFTRimLightComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	const float Scale = ftcore::look::rimLightScale(CVarRimLight.GetValueOnGameThread(), bAllowed);
	if (!Light)
	{
		if (Scale <= 0.f)
		{
			return;
		}
		MakeLight();
		if (!Light)
		{
			return;
		}
	}
	const bool bScaleChanged = Scale != AppliedScale;
	if (bScaleChanged)
	{
		AppliedScale = Scale;
		Light->SetVisibility(Scale > 0.f);
		Light->SetIntensity(Intensity * Scale);
		Light->SetLightColor(Color);
	}
	if (Scale <= 0.f)
	{
		return;
	}
	// Pieces swapped on at runtime (garments, hair) join the channel within a second.
	ChannelTimer -= DeltaTime;
	if (ChannelTimer <= 0.f)
	{
		ChannelTimer = 1.f;
		ApplyChannels();
	}
	const APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(this, 0); // player 0: one view
	if (!Cam)
	{
		return;
	}
	const FVector Target = GetComponentLocation();
	FVector Away = Target - Cam->GetCameraLocation();
	Away.Z = 0.0;
	if (!Away.Normalize())
	{
		Away = GetOwner()->GetActorForwardVector();
	}
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Away);
	const FVector Pos = Target + Away * Back + Right * Side + FVector::UpVector * Up;
	Light->SetWorldLocationAndRotation(Pos, (Target - Pos).Rotation());
	if (bScaleChanged || FVector::DotProduct(Away, LoggedAway) < 0.94)
	{
		// Where the light sits against the view it was placed for, on a scale change or when the camera bearing turns
		// more than about 20 degrees (a review log can check it is behind the figure as the camera sees it).
		LoggedAway = Away;
		UE_LOG(LogForgottenThird, Display, TEXT("FTRimLight: %s scale %.2f, camera %s, aim %s, light %s (%.0f deg up)."), *GetOwner()->GetName(), Scale,
			*Cam->GetCameraLocation().ToCompactString(), *Target.ToCompactString(), *Pos.ToCompactString(),
			FMath::RadiansToDegrees(FMath::Atan2(Up, FMath::Sqrt(Back * Back + Side * Side))));
	}
}
