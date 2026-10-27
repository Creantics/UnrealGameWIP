#pragma once

// The exploration demo's player: a lead character (Devin) on a capsule with a third-person camera
// on a spring arm. Move with WASD or the left stick (camera-relative), look with the mouse or the
// right stick, hold Shift or the left trigger to run, Space or pad A to jump, E or pad X to use
// what the prompt points at. Input objects are built here at run time, like AFTPlayerController's.
//
// The body plays its idle or walk clip in single-node mode, switched on ground speed, with the
// walk's play rate following the speed. There is no blend, so the switch pops.
//
// The body, its clips, any extra pieces (clothes, hair, a face) and grooms (brows, lashes) can be
// swapped from config without a rebuild, under [/Script/ForgottenThird.FTExploreCharacter] in
// Config/UserGame.ini (local, not committed) or DefaultGame.ini. Pieces follow the body's pose
// (leader pose) and face shapes; grooms bind to the piece they were built for (see FTLook.h).
//
// With a party in config (PartyLooks) the pawn wears one member's look and the rest follow it
// (AFTExploreDirector): Tab or RB switches to the next member, P or LB hides or shows the followers.
// StartLead and bStartSolo set the start (-ExploreLead=<Name> and -ExploreSolo for -game runs); the
// player's last choice is kept across a fight (UFTGameInstance).
//
// -ExplorePortrait puts the camera in front of the face and keeps it there, for checking a look;
// -ExplorePortraitYaw= (180 front, 90 side, 0 back), -ExplorePortraitArm=, -ExplorePortraitHeight= and
// -ExplorePortraitPitch= move it, e.g. Arm=330 Height=0 Pitch=0 frames the whole body.

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "InputActionValue.h"

#include "FTLook.h"

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
	// Grooms bound to the body or a piece; skipped without the HairStrands plugin.
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Look")
	TArray<FFTGroomLook> Grooms;
	// The party in order, each member's look: the pawn wears one and the others follow it. Empty keeps
	// the look above (as the first of ftcore::demo::kPartyNames) and the default followers.
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Party")
	TArray<FFTLeadLook> PartyLooks;
	// The member to start as, by name; empty or unknown starts as the first.
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Party")
	FString StartLead;
	// Start with the followers hidden.
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Party")
	bool bStartSolo = false;
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

	// The PartyLooks entry the pawn wears (-1 when PartyLooks is empty), and whether the followers are hidden.
	int32 GetLeadIndex() const { return LeadIndex; }
	bool IsPartyHidden() const { return bPartyHidden; }

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
	void HandleSwitchLead();
	void HandleToggleParty();
	void UpdateClip();
	// Puts on a look, replacing the body, clips, pieces and grooms worn (FTLook).
	void WearLook(const FFTLeadLook& Look);
	void AddPortraitLights();

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
	TObjectPtr<UInputAction> SwitchLeadAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> PartyAction;

	UPROPERTY(Transient)
	TArray<TObjectPtr<USkeletalMeshComponent>> Pieces;
	UPROPERTY(Transient)
	TArray<TObjectPtr<USceneComponent>> GroomComponents;
	UPROPERTY(Transient)
	TArray<TObjectPtr<URectLightComponent>> PortraitLights;

	TWeakObjectPtr<AFTExploreDirector> Director;
	int32 LeadIndex = -1;
	bool bPartyHidden = false;
	bool bWalking = false;
	bool bClipStarted = false;
};
