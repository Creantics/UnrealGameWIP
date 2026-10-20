#pragma once

// The exploration demo's player: a lead character (Devin) on a capsule with a third-person camera
// on a spring arm. Move with WASD or the left stick (camera-relative), look with the mouse or the
// right stick, hold Shift or the left trigger to run, Space or pad A to jump, E or pad X to use
// what the prompt points at. Input objects are built here at run time, like AFTPlayerController's.
//
// The body plays its idle or walk clip in single-node mode, switched on ground speed, with the
// walk's play rate following the speed. There is no blend, so the switch pops.

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "InputActionValue.h"

#include "FTExploreCharacter.generated.h"

class AFTExploreDirector;
class UAnimSequence;
class UCameraComponent;
class UInputAction;
class UInputMappingContext;
class USpringArmComponent;

UCLASS()
class FORGOTTENTHIRD_API AFTExploreCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AFTExploreCharacter();

	UPROPERTY(EditAnywhere, Category = "Explore|Look")
	TObjectPtr<UAnimSequence> IdleClip;
	UPROPERTY(EditAnywhere, Category = "Explore|Look")
	TObjectPtr<UAnimSequence> WalkClip;
	// The ground speed (cm/s) at which the walk clip plays at rate 1.
	UPROPERTY(EditAnywhere, Category = "Explore|Look")
	float WalkClipSpeed = 140.f;

	UPROPERTY(EditAnywhere, Category = "Explore|Movement")
	float WalkSpeed = 330.f;
	UPROPERTY(EditAnywhere, Category = "Explore|Movement")
	float RunSpeed = 620.f;

	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	// Ground-plane facing, for the interact prompt's cone.
	FVector2D GetFacing2D() const;

protected:
	virtual void BeginPlay() override;
	virtual void PawnClientRestart() override;

private:
	void CreateInputObjects();
	UInputAction* MakeAction(const TCHAR* Name, bool bAxis2D);

	void HandleMove(const FInputActionValue& Value);
	void HandleLook(const FInputActionValue& Value);
	void HandleRunStart();
	void HandleRunStop();
	void HandleInteract();
	void UpdateClip();

	AFTExploreDirector* GetDirector();

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USpringArmComponent> Boom;
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> Context;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> MoveAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> LookAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> JumpAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> RunAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> InteractAction;

	TWeakObjectPtr<AFTExploreDirector> Director;
	bool bWalking = false;
	bool bClipStarted = false;
};
