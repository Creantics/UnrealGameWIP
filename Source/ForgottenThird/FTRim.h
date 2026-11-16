// The character rim's per-frame driver. The rim itself is a material term on the garment and skin masters (and on the
// leads' hair copy; FTRimCollection.h), not a light: no channel, no shadow, nothing that can reach a glossy interior.
// Once a frame this component writes the term's virtual direction (ftcore::look::rimDirection: from the lead away from
// player 0's camera, raised 30 degrees, turned 35 to the camera's right) and its strength into MPC_FTRim. The collection
// is shared, so only one figure writes: the pawn player 0 controls, or (bRequirePlayerPawn false) any figure allowed to
// carry it (the arena allows the player's figure alone).
// Strength from ft.Rim: 0 = off (the default, the masters add nothing); above 0 = that strength. When a world system
// owns the direction (FTRim::SetDirectionSource(World)), this component writes the strength only.
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

private:
	float WrittenStrength = -1.f;
	FVector LoggedAway = FVector::ZeroVector; // camera bearing at the last log
};
