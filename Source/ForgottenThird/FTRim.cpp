#include "FTRim.h"

#include "ForgottenThird.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "FTRimCollection.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"

#include "ftcore/LookReview.h"

namespace
{
TAutoConsoleVariable<float> CVarRim(TEXT("ft.Rim"), 0.f,
	TEXT("Character rim (a material term): 0 off (the default), above 0 on at that strength."), ECVF_Default);
}

UFTRimComponent::UFTRimComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork; // after the camera has moved this frame
}

void UFTRimComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	UWorld* World = GetWorld();
	AActor* Owner = GetOwner();
	if (!bAllowed || !World || !World->IsGameWorld() || !Owner)
	{
		return;
	}
	if (bRequirePlayerPawn && UGameplayStatics::GetPlayerPawn(this, 0) != Owner)
	{
		return;
	}
	UMaterialParameterCollectionInstance* Params = FTRim::Params(*World);
	if (!Params)
	{
		return; // never built: the masters carry no term either
	}
	const float Strength = ftcore::look::rimStrength(CVarRim.GetValueOnGameThread());
	if (Strength != WrittenStrength)
	{
		WrittenStrength = Strength;
		Params->SetScalarParameterValue(FTRim::Strength, Strength);
		LoggedAway = FVector::ZeroVector; // log the placement again at the new strength
	}
	if (Strength <= 0.f || FTRim::GetDirectionSource() != FTRim::EDirectionSource::Camera)
	{
		return; // off, or a world system owns RimDir (FTRim::SetDirectionSource)
	}
	const APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(this, 0); // player 0: one view
	if (!Cam)
	{
		return;
	}
	const FVector C = Cam->GetCameraLocation();
	const FVector T = GetComponentLocation();
	const ftcore::look::Dir3 L = ftcore::look::rimDirection({static_cast<float>(C.X), static_cast<float>(C.Y), static_cast<float>(C.Z)},
		{static_cast<float>(T.X), static_cast<float>(T.Y), static_cast<float>(T.Z)});
	Params->SetVectorParameterValue(FTRim::Dir, FLinearColor(L.x, L.y, L.z, 0.f));
	FVector Away = T - C;
	Away.Z = 0.0;
	if (Away.Normalize() && FVector::DotProduct(Away, LoggedAway) < 0.94)
	{
		// The direction against the view it was set for, when the strength changes or the camera bearing turns more than
		// about 20 degrees (a review log can check it points behind the figure as the camera sees it).
		LoggedAway = Away;
		UE_LOG(LogForgottenThird, Display, TEXT("FTRim: %s strength %.2f, camera %s, lead %s, direction (%.2f, %.2f, %.2f)."), *Owner->GetName(),
			Strength, *C.ToCompactString(), *T.ToCompactString(), L.x, L.y, L.z);
	}
}
