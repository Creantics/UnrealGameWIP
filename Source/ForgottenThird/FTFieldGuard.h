#pragma once

// A field guard on the exploration demo: a blockout figure (a tinted column with a visor showing
// its facing and a marker over its head while it has seen the player) that the explore director
// places every frame from ftcore::GuardSet. It has no controller, no movement component and no
// collision, so it can't block the player or the autopilot. It answers one question for Core: is
// the line from its eyes to the player clear (CanSee).

#include "CoreMinimal.h"
#include "CollisionQueryParams.h"
#include "GameFramework/Actor.h"

#include "ftcore/Guard.h"

#include "FTFieldGuard.generated.h"

class UMaterialInstanceDynamic;
class UStaticMeshComponent;

UCLASS(NotBlueprintable)
class FORGOTTENTHIRD_API AFTFieldGuard : public AActor
{
	GENERATED_BODY()

public:
	AFTFieldGuard();

	// Builds the figure. `Player` is ignored by the ground trace.
	void Setup(const AActor* Player);

	// Stands the guard at `Ground` (cm, any height; the ground below it is found), facing `Facing`,
	// tinted for `State`.
	void Place(const FVector2D& Ground, const FVector2D& Facing, ftcore::GuardState State, float FeetZ);

	// True when nothing blocks the line from the guard's eyes to the middle of `Player`.
	bool CanSee(const AActor& Player) const;

private:
	UPROPERTY(VisibleAnywhere, Category = "Guard")
	TObjectPtr<UStaticMeshComponent> Body;
	UPROPERTY(VisibleAnywhere, Category = "Guard")
	TObjectPtr<UStaticMeshComponent> Visor;
	UPROPERTY(VisibleAnywhere, Category = "Guard")
	TObjectPtr<UStaticMeshComponent> Alert;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BodyTint;

	FCollisionQueryParams GroundParams;
	FCollisionQueryParams SightParams;
	ftcore::GuardState Shown = ftcore::GuardState::Idle;
	bool bTinted = false;
};
