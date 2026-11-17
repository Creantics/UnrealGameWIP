#include "FTRim.h"

#include "ForgottenThird.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/SkeletalMeshComponent.h"
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

namespace
{
// The one component writing the shared collection (ftcore::look::rimMayWrite says who may take it).
struct FRimWriterSlot
{
	TWeakObjectPtr<const UFTRimComponent> Holder;
	int32 Priority = 0;
	uint64 Frame = 0;
};

FRimWriterSlot& WriterSlot()
{
	static FRimWriterSlot Slot;
	return Slot;
}
} // namespace

void UFTRimComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	const float Strength = ftcore::look::rimStrength(CVarRim.GetValueOnGameThread());
	const float Target = ftcore::look::rimSideTarget(FTRim::GetPreferredSide());
	if (ftcore::look::rimIdle(Strength, WrittenStrength))
	{
		Side = Target; // off: nothing touched, and a switch-on starts on the lamp's side instead of sweeping to it
		return;
	}
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
	// One writer: a figure the arena placed (bRequirePlayerPawn false, priority 1) wins over an explore pawn alive in the
	// same world, since the arena's camera frames that figure; the holder keeps the slot while it writes every frame.
	FRimWriterSlot& Slot = WriterSlot();
	const int32 Priority = bRequirePlayerPawn ? 0 : 1;
	const bool bHolder = Slot.Holder.Get() == this;
	if (!ftcore::look::rimMayWrite(bHolder, Slot.Holder.IsValid(), Slot.Frame, GFrameCounter, Priority, Slot.Priority))
	{
		return;
	}
	Slot.Holder = this;
	Slot.Priority = Priority;
	Slot.Frame = GFrameCounter;
	UMaterialParameterCollectionInstance* Params = CachedParams.Get();
	if (!Params)
	{
		Params = FTRim::Params(*World);
		CachedParams = Params;
	}
	if (!Params)
	{
		return; // never built: the masters carry no term either
	}
	if (Strength != WrittenStrength)
	{
		WrittenStrength = Strength;
		Params->SetScalarParameterValue(FTRim::Strength, Strength);
		LoggedAway = FVector::ZeroVector; // log the placement again at the new strength
	}
	if (Strength <= 0.f)
	{
		Side = Target;
		return;
	}
	Side = ftcore::look::rimSideStep(Side, Target, DeltaTime); // the lamp's side, a flip eased over 0.4 s
	const APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(this, 0); // player 0: one view
	if (!Cam)
	{
		return;
	}
	const FVector C = Cam->GetCameraLocation();
	const FVector T = GetComponentLocation();
	const ftcore::look::Dir3 L = ftcore::look::rimDirection({static_cast<float>(C.X), static_cast<float>(C.Y), static_cast<float>(C.Z)},
		{static_cast<float>(T.X), static_cast<float>(T.Y), static_cast<float>(T.Z)}, Side);
	Params->SetVectorParameterValue(FTRim::Dir, FLinearColor(L.x, L.y, L.z, 0.f));
	// The hair's head normal comes from this point: the head bone raised to the middle of the skull, or this component
	// on a figure without one.
	static const FName HeadBoneName(TEXT("head"));
	if (!bHeadSearched)
	{
		bHeadSearched = true;
		TInlineComponentArray<USkeletalMeshComponent*> Meshes(Owner);
		for (USkeletalMeshComponent* Mesh : Meshes)
		{
			if (Mesh && Mesh->GetBoneIndex(HeadBoneName) != INDEX_NONE)
			{
				HeadMesh = Mesh;
				break;
			}
		}
	}
	const USkeletalMeshComponent* Mesh = HeadMesh.Get();
	const FVector H = Mesh ? Mesh->GetSocketLocation(HeadBoneName) + FVector(0.0, 0.0, ftcore::look::kRimHairCentreUpCm) : T;
	Params->SetVectorParameterValue(FTRim::Head, FLinearColor(static_cast<float>(H.X), static_cast<float>(H.Y), static_cast<float>(H.Z), 0.f));
	FVector Away = T - C;
	Away.Z = 0.0;
	if (Away.Normalize() && (FVector::DotProduct(Away, LoggedAway) < 0.94 || Target != LoggedSide))
	{
		// The direction against the view it was set for, when the strength or the lamp side changes or the camera bearing
		// turns more than about 20 degrees (a review log can check it points behind the figure as the camera sees it).
		LoggedAway = Away;
		LoggedSide = Target;
		UE_LOG(LogForgottenThird, Display, TEXT("FTRim: %s strength %.2f, side %+.0f, camera %s, lead %s, direction (%.2f, %.2f, %.2f), head %s."),
			*Owner->GetName(), Strength, Target, *C.ToCompactString(), *T.ToCompactString(), L.x, L.y, L.z, *H.ToCompactString());
	}
}

void UFTRimComponent::EndPlay(const EEndPlayReason::Type Reason)
{
	// A writer leaving with the rim on puts the strength back to 0, so a figure that takes over writes it again.
	FRimWriterSlot& Slot = WriterSlot();
	if (Slot.Holder.Get() == this)
	{
		if (UMaterialParameterCollectionInstance* Params = CachedParams.Get(); Params && WrittenStrength > 0.f)
		{
			Params->SetScalarParameterValue(FTRim::Strength, 0.f);
		}
		Slot.Holder.Reset();
	}
	Super::EndPlay(Reason);
}
