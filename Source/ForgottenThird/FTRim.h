// The character rim's per-frame driver. The rim itself is a material term on the garment and skin masters (and on the
// leads' hair copy; FTRimCollection.h), not a light: no channel, no shadow, nothing that can reach a glossy interior.
// Once a frame this component writes the term's virtual direction (ftcore::look::rimDirection: from the lead away from
// player 0's camera, raised 30 degrees, turned 35 to the camera's right, or to its left while the world lane's nearest
// lamp is on that side: FTRim::SetPreferredSide, a flip eased over 0.4 s) and its strength into MPC_FTRim. The collection
// is shared, so only one figure writes (ftcore::look::rimMayWrite): the pawn player 0 controls, or (bRequirePlayerPawn
// false) a figure the arena allows (the player's figure alone), which wins over an explore pawn in the same world. It also
// writes RimHead (the head bone raised to the skull's middle), the point the hair's rim normal is taken from.
// At ft.Rim 0, with 0 written, a tick is a branch: nothing is loaded, looked up or written.
// Strength from ft.Rim: 0 = off (the default, the masters add nothing); above 0 = that strength. RimDir is this
// component's alone; the world lane writes RimColour only.
#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "FTRim.generated.h"

UCLASS(ClassGroup = (ForgottenThird), meta = (BlueprintSpawnableComponent))
class FORGOTTENTHIRD_API UFTRimComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UFTRimComponent();

	// The lead's point the direction is taken from: attach at chest-to-head height on the character.
	UPROPERTY(EditAnywhere, Category = "Rim")
	bool bAllowed = true; // false on figures that never write (the arena sets it per figure)

	UPROPERTY(EditAnywhere, Category = "Rim")
	bool bRequirePlayerPawn = true; // write only while player 0 controls this figure

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	float WrittenStrength = 0.f; // the collection's default: a first tick at 0 writes nothing
	TWeakObjectPtr<class UMaterialParameterCollectionInstance> CachedParams;
	TWeakObjectPtr<class USkeletalMeshComponent> HeadMesh;
	bool bHeadSearched = false;
	FVector LoggedAway = FVector::ZeroVector; // camera bearing at the last log
	float LoggedSide = 0.f;                   // lamp side at the last log
	float Side = 1.f;                         // eased side: +1 camera-right, -1 camera-left
};
