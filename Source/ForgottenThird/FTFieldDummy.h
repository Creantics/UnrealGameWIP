#pragma once

// A target dummy for the field (the explore run): a post the lead's weapon can hit (ftcore::FieldStrike, fed by
// AFTExploreCharacter). It never fights back. Place one in a map, or start the explore run with -ExploreDummy to have
// one stood 3 m in front of the lead. Each hit logs "Field: dummy hit for <n> (<total> taken)" and makes the post
// jolt; it never falls.

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "FTFieldDummy.generated.h"

class UStaticMeshComponent;

UCLASS()
class FORGOTTENTHIRD_API AFTFieldDummy : public AActor
{
	GENERATED_BODY()

public:
	AFTFieldDummy();

	// The body's radius the strikes test against (cm).
	UPROPERTY(EditAnywhere, Category = "Dummy")
	float Radius = 40.f;

	virtual void Tick(float DeltaSeconds) override;

	// One blow of Amount hp.
	void TakeHit(int32 Amount);
	int32 GetHits() const { return Hits; }
	int32 GetDamageTaken() const { return DamageTaken; }

private:
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UStaticMeshComponent> Post;

	int32 Hits = 0;
	int32 DamageTaken = 0;
	float Jolt = 0.f; // seconds of jolt left
};
