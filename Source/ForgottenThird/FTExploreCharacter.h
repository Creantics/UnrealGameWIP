#pragma once

// The exploration demo's player: a lead character (Devin) on a capsule with a third-person camera
// on a spring arm. Move with WASD or the left stick (camera-relative), look with the mouse or the
// right stick, hold Shift or the left trigger to run, Space or pad A to jump, E or pad X to use
// what the prompt points at. Input objects are built here at run time, like AFTPlayerController's.
//
// The body plays its idle or walk clip in single-node mode, switched on ground speed, with the
// walk's play rate following the speed. There is no blend, so the switch pops.
//
// The body, its clips and any extra pieces (clothes, hair) can be swapped from config without a
// rebuild, under [/Script/ForgottenThird.FTExploreCharacter] in Config/UserGame.ini (local, not
// committed) or DefaultGame.ini. Pieces follow the body's pose (leader pose) and face shapes.
//
// -ExplorePortrait puts the camera in front of the face and keeps it there, for checking a look;
// -ExplorePortraitYaw= (180 front, 90 side, 0 back), -ExplorePortraitArm=, -ExplorePortraitHeight= and
// -ExplorePortraitPitch= move it, e.g. Arm=330 Height=0 Pitch=0 frames the whole body.

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "InputActionValue.h"

#include "FTExploreCharacter.generated.h"

class AFTExploreDirector;
class UAnimSequence;
class UCameraComponent;
class UInputAction;
class UInputMappingContext;
class URectLightComponent;
class USpringArmComponent;

UCLASS(Config = Game)
class FORGOTTENTHIRD_API AFTExploreCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AFTExploreCharacter();

	UPROPERTY(EditAnywhere, Category = "Explore|Look")
	TObjectPtr<UAnimSequence> IdleClip;
	UPROPERTY(EditAnywhere, Category = "Explore|Look")
	TObjectPtr<UAnimSequence> WalkClip;
	// Config overrides, loaded at BeginPlay; empty keeps the built-in Devin.
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Look")
	FSoftObjectPath BodyMeshPath;
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Look")
	FSoftObjectPath IdleClipPath;
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Look")
	FSoftObjectPath WalkClipPath;
	// Skeletal meshes worn over the body, posed by it.
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Look")
	TArray<FSoftObjectPath> PiecePaths;
	// The ground speed (cm/s) at which the walk clip plays at rate 1.
	UPROPERTY(EditAnywhere, Category = "Explore|Look")
	float WalkClipSpeed = 140.f;

	UPROPERTY(EditAnywhere, Category = "Explore|Movement")
	float WalkSpeed = 330.f;
	UPROPERTY(EditAnywhere, Category = "Explore|Movement")
	float RunSpeed = 700.f;

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
	void ApplyLookOverrides();
	void AddPortraitLights();
	void StepPortraitShots();

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

	UPROPERTY(Transient)
	TArray<TObjectPtr<USkeletalMeshComponent>> Pieces;
	UPROPERTY(Transient)
	TArray<TObjectPtr<URectLightComponent>> PortraitLights;

	// -ExplorePortraitShot: the yaws still to shoot, the file prefix, and whether the boom already faces the next one.
	TArray<float> ShotYaws;
	FString ShotPrefix;
	int32 ShotIndex = 0;
	bool bShotAimed = false;
	FTimerHandle ShotTimer;

	TWeakObjectPtr<AFTExploreDirector> Director;
	bool bWalking = false;
	bool bClipStarted = false;
};
