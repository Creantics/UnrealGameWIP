#pragma once

// One spot of the exploration demo: the visible side of an entry in ftcore::demo::kExploreSpots,
// chosen by SpotId. Its kind decides the look:
// - Talk: Duy, looping his idle clip, with a blocking capsule.
// - Pickup: a glowing shard that bobs and spins; it vanishes when picked up.
// - Gate: two pillars and a bar that blocks the gap; the bar sinks into the ground when opened.
// - Encounter: two placeholder enemies (the battle demo's) waiting beyond the gate.
// - SpotId == ftcore::demo::kFieldGoalSpotId: the story field step's goal marker (glowing column).
// The look is set from OnConstruction (editor viewport) and BeginPlay (play).

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "FTExploreSpot.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class UPointLightComponent;
class USkeletalMeshComponent;
class UStaticMeshComponent;

UCLASS()
class FORGOTTENTHIRD_API AFTExploreSpot : public AActor
{
	GENERATED_BODY()

public:
	AFTExploreSpot();

	// Index into ftcore::demo::kExploreSpots.
	UPROPERTY(EditAnywhere, Category = "Explore")
	int32 SpotId = 0;

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void Tick(float DeltaSeconds) override;

	// The spot was used (or, for an encounter, won). bInstant skips the animation (a restored save).
	void MarkUsed(bool bInstant);
	// Brief highlight while the prompt points at it.
	void SetFocused(bool bFocused);

protected:
	virtual void BeginPlay() override;

private:
	void Configure();
	UMaterialInstanceDynamic* Tint(UStaticMeshComponent* Part, const FLinearColor& Color);

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USceneComponent> Root;
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UStaticMeshComponent> Shape;
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UStaticMeshComponent> PartA;
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UStaticMeshComponent> PartB;
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USkeletalMeshComponent> Skin;
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UPointLightComponent> Glow;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> ShapeMaterial;

	FLinearColor BaseColor = FLinearColor::White;
	double UsedTime = -1.0;
	bool bUsed = false;
	bool bFocused = false;
	float Age = 0.f;
};
