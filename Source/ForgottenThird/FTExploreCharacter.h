#pragma once

// The exploration demo's player: a lead character (Devin) on a capsule with a third-person camera
// on a spring arm. Move with WASD or the left stick (camera-relative), look with the mouse or the
// right stick, hold Shift or the left trigger to run, Space or pad A to jump, E or pad X to use
// what the prompt points at. Input objects are built here at run time, like AFTPlayerController's.
//
// The body is animated by UFTLocomotionAnimInstance: idle, walk, run, sprint, jump, fall and land
// clips (and the moving jump and landing taken at speed) blended from the capsule's speed and
// IsFalling, the gaits at the rate that keeps the planted feet still, the spine banking and leaning
// into turns and speed. Missing clips are skipped (with no run clip the walk speeds up; with no jump
// clips the gait carries on in the air).
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

#include "FTLocomotionAnimInstance.h"

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
	UPROPERTY(EditAnywhere, Category = "Explore|Look")
	TObjectPtr<UAnimSequence> RunClip;
	UPROPERTY(EditAnywhere, Category = "Explore|Look")
	TObjectPtr<UAnimSequence> SprintClip;
	UPROPERTY(EditAnywhere, Category = "Explore|Look")
	TObjectPtr<UAnimSequence> JumpClip;
	UPROPERTY(EditAnywhere, Category = "Explore|Look")
	TObjectPtr<UAnimSequence> FallClip;
	UPROPERTY(EditAnywhere, Category = "Explore|Look")
	TObjectPtr<UAnimSequence> LandClip;
	UPROPERTY(EditAnywhere, Category = "Explore|Look")
	TObjectPtr<UAnimSequence> JumpMovingClip;
	UPROPERTY(EditAnywhere, Category = "Explore|Look")
	TObjectPtr<UAnimSequence> LandMovingClip;
	// Config overrides, loaded at BeginPlay; empty keeps the built-in Devin.
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Look")
	FSoftObjectPath BodyMeshPath;
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Look")
	FSoftObjectPath IdleClipPath;
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Look")
	FSoftObjectPath WalkClipPath;
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Look")
	FSoftObjectPath RunClipPath;
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Look")
	FSoftObjectPath SprintClipPath;
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Look")
	FSoftObjectPath JumpClipPath;
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Look")
	FSoftObjectPath FallClipPath;
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Look")
	FSoftObjectPath LandClipPath;
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Look")
	FSoftObjectPath JumpMovingClipPath;
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Look")
	FSoftObjectPath LandMovingClipPath;
	// Skeletal meshes worn over the body, posed by it.
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Look")
	TArray<FSoftObjectPath> PiecePaths;
	// The default gait is a run (the run clip's ground speed); held Run sprints.
	UPROPERTY(EditAnywhere, Category = "Explore|Movement")
	float WalkSpeed = 450.f;
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
	void SetupLocomotion();
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
	FFTFootSlideProbe SlideProbe;
	float JumpEvery = 0.f; // -LocoJump=<s>
	float JumpTimer = 0.f;
};
