#pragma once

// A placeholder for an important NPC (Data/Npc/npc_values.csv): a capsule, a plain body and, in development builds, its
// id over its head. It registers with UFTNpcSubsystem while in play, so deeds done in its sight reach its memory as a
// witness, and the subsystem's hooks take it as the one dealt with. The real character replaces the body later; the
// id is what links it to its memory.

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "FTNpcActor.generated.h"

class UCapsuleComponent;
class UStaticMeshComponent;
class UTextRenderComponent;

UCLASS()
class FORGOTTENTHIRD_API AFTNpcActor : public AActor
{
	GENERATED_BODY()

public:
	AFTNpcActor();

	// The npc_id in the NPC tables.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "FT|Npc")
	FString NpcId;

	// The NPC's index in the loaded tables, -1 when the id is unknown (or nothing is loaded).
	int32 GetNpcIndex() const { return NpcIndex; }
	void SetNpcIndex(int32 Index) { NpcIndex = Index; }
	// Where a witness looks from (cm above the actor's origin).
	FVector GetEyes() const;

	void SetLabel(const FString& Text);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	UPROPERTY(VisibleAnywhere, Category = "FT|Npc")
	TObjectPtr<UCapsuleComponent> Capsule;
	UPROPERTY(VisibleAnywhere, Category = "FT|Npc")
	TObjectPtr<UStaticMeshComponent> Body;
	UPROPERTY(VisibleAnywhere, Category = "FT|Npc")
	TObjectPtr<UTextRenderComponent> Label;

	int32 NpcIndex = -1;
};
