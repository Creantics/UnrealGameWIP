#pragma once

// The player's viewpoint: a camera, plus the rhythm conductor that tap input feeds. It doesn't move
// on its own; place it in the map or let the game mode spawn it at a PlayerStart.

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"

#include "FTPawn.generated.h"

class UCameraComponent;
class UFTConductorComponent;

UCLASS()
class FORGOTTENTHIRD_API AFTPawn : public APawn
{
	GENERATED_BODY()

public:
	AFTPawn();

	UCameraComponent* GetCamera() const { return Camera; }
	UFTConductorComponent* GetConductor() const { return Conductor; }

private:
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USceneComponent> Root;
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UCameraComponent> Camera;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UFTConductorComponent> Conductor;
};
