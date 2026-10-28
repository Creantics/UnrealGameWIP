#pragma once

// The exploration demo's player: a lead character (Devin) on a capsule with a third-person camera
// on a spring arm. Move with WASD or the left stick (camera-relative), look with the mouse or the
// right stick, hold Shift or the left trigger to run, Space or pad A to jump, Left Alt or pad B to
// roll, E or pad X to use what the prompt points at. Input objects are built here at run time, like
// AFTPlayerController's.
//
// The body is animated by UFTLocomotionAnimInstance: idle, walk, run, sprint, jump, fall and land
// clips (and the moving jump and landing taken at speed) blended from the capsule's speed and
// IsFalling, the gaits at the rate that keeps the planted feet still, the spine banking and leaning
// into turns and speed. Each gait's start and stops, where there are clips named after its loop
// (UFTLocomotionAnimInstance::SetMovesFromGait), play as the capsule sets off and brakes. Missing
// clips are skipped (with no run clip the walk speeds up; with no jump clips the gait carries on in
// the air).
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
// Jump in front of a low wall or a ledge (UFTLocomotionAnimInstance::SetTraversalFromRun's clips) goes over it or
// climbs onto it instead: forward and down traces find the near face, the top and its depth and the floor beyond,
// ftcore::TraversalSet picks the clip and where to enter it, and the capsule is carried along the plan's root path
// (no collision on the way) until the clip ends. -LocoVaultWall builds a test course ahead (a thin wall, a low
// block, a high block) and runs at it, traversing whatever it can.
//
// Roll goes the way the stick points (else the way the body moves, else the way it faces): one of the roll clips
// (SetTraversalFromRun's too) plays from its start, entered at the body's speed, the body turns to the roll's way and
// the capsule follows the clip's root path along the real floor (up and down steps; a wall ends the roll, and with no
// floor under it the body lets go and falls). A press the body can't act on yet (in the air, mid-traversal) is kept
// for a moment, and a moving landing from a real fall with crouch on rolls out of it. -LocoRollEvery=<s> rolls every
// s seconds.
//
// -ExplorePortrait puts the camera in front of the face and keeps it there, for checking a look;
// -ExplorePortraitYaw= (180 front, 90 side, 0 back), -ExplorePortraitArm=, -ExplorePortraitHeight= and
// -ExplorePortraitPitch= move it, e.g. Arm=330 Height=0 Pitch=0 frames the whole body.

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "InputActionValue.h"

#include "FTLocomotionAnimInstance.h"
#include "FTLook.h"

#include "FTExploreCharacter.generated.h"

class AFTExploreDirector;
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

	// The look worn when PartyLooks is empty, as Devin (FFTLeadLook): empty keeps the built-in Devin.
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Look")
	FSoftObjectPath BodyMeshPath;
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Look")
	FSoftObjectPath IdleClipPath;
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Look")
	FSoftObjectPath WalkClipPath;
	// The retargeted clips' folder (FFTLeadLook::ClipFolder).
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Look")
	FString ClipFolder;
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

	// The default gait is a run (the run clip's ground speed); held Run sprints. Crouch toggles a crouch walk
	// at the crouch walk clip's ground speed (CrouchSpeed until it is measured).
	UPROPERTY(EditAnywhere, Category = "Explore|Movement")
	float WalkSpeed = 450.f;
	UPROPERTY(EditAnywhere, Category = "Explore|Movement")
	float RunSpeed = 700.f;
	UPROPERTY(EditAnywhere, Category = "Explore|Movement")
	float CrouchSpeed = 150.f;

	virtual void Tick(float DeltaSeconds) override;
	virtual void AddMovementInput(FVector WorldDirection, float ScaleValue = 1.f, bool bForce = false) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	// Ground-plane facing, for the interact prompt's cone.
	FVector2D GetFacing2D() const;
	// For locomotion captures: the camera frames the body under Root (its feet) as it frames this one.
	void WatchFrom(USceneComponent* Root);

	// The PartyLooks entry the pawn wears (-1 when PartyLooks is empty), and whether the followers are hidden.
	int32 GetLeadIndex() const { return LeadIndex; }
	bool IsPartyHidden() const { return bPartyHidden; }
	// Whether a traversal (a vault, a climb or a roll) is carrying the body: a scene played in place waits for it to end.
	bool IsTraversing() const { return bTraversing; }
	// For a scene that turns input off and on again: a held Run's release that came while input was off never arrived,
	// so the body goes back to its default gait, and a roll press still kept is dropped. Crouch is a toggle, not held;
	// standing up from it is this call's choice, so a scene starts and ends with the lead on its feet.
	void ResetHeldInput();

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
	void HandleJump();
	void HandleRoll();
	void HandleCrouch();
	void HandleInteract();
	void HandleSwitchLead();
	void HandleToggleParty();
	// Puts on a look, replacing the body, clips, pieces and grooms worn (FTLook).
	void WearLook(const FFTLeadLook& Look);
	// Gives the body's locomotion its loaded clips (and those found beside them) and the crouch its speed.
	void SetupLocomotion(const FFTLoadedLook& Loaded);
	// Traversal: finds what is ahead (false: nothing to go over or onto), starts a traversal (false: none
	// fits; bLog says why), carries the body along one under way, and builds -LocoVaultWall's course.
	bool ProbeObstacle(const ftcore::TraversalSet& Set, ftcore::TraversalObstacle& Out) const;
	bool TryTraverse(bool bLog);
	// Starts a roll (false: not on the ground, mid-traversal, no room to stand up, or no roll clip; bLog says the
	// last), heading as a landing rolls when bLanding.
	bool TryRoll(bool bLanding, bool bLog);
	void TickTraversal(float DeltaSeconds);
	void SpawnTraversalCourse();
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
	TObjectPtr<UInputAction> RollAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> RunAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> CrouchAction;
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
	float CrouchEvery = 0.f; // -LocoCrouchEvery=<s>
	float CrouchTimer = 0.f;
	float RollAsked = 0.f; // a roll press still kept (s)
	float RollEvery = 0.f; // -LocoRollEvery=<s>
	float RollTimer = 0.f;
	float PulseEvery = 0.f; // -LocoPulse=<s>
	float PulseTimer = 0.f;
	float PulseRest = 2.f;  // -LocoRest=<s>
	float LookRest = 0.f; // seconds since the last look input
	float FaceBy = 0.f;   // -LocoFace=<deg>
	TWeakObjectPtr<AActor> Watched; // WatchFrom: the body the camera frames and recentres behind
	bool bFacing = false;
	// -LocoSideView: each turn of the framed body is watched from its side, the camera held where the turn began.
	bool bSideView = false;
	// -LocoSideAlways: the framed body is filmed from its side the whole time, for measuring its lean.
	// -LocoCamYaw=<deg> (implies it) films from that angle off the body's back instead (90 the side, 45 a rear
	// three-quarter with the body heading left), and -LocoCamPitch=<deg> looks down that much more.
	bool bSideAlways = false;
	float SideAlwaysYaw = 90.f;
	float SideAlwaysPitch = 0.f;
	// -LocoNoLook: look input is ignored, so a mouse passing over an unattended capture can't swing the camera.
	bool bNoLook = false;
	// The traversal under way: its plan, clip time, and the body's feet and facing as it began (a roll's facing is the
	// way it rolls).
	bool bTraversing = false;
	ftcore::TraversalPlan TravPlan;
	float TravTime = 0.f;
	FTransform TravFrame;
	// -LocoVaultWall: the course still to build, then its way, run along every frame, traversing whatever is ahead.
	bool bCoursePending = false;
	bool bVaultCourse = false;
	FVector CourseDir = FVector::ForwardVector;
	uint32 SideTurns = 0;
	float SideYaw = 0.f;
	float SideHold = 0.f;
	int32 LeadIndex = -1;
	bool bPartyHidden = false;
};
