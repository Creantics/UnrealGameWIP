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
// (AFTExploreDirector). Under the new UI (FT::NewUiEnabled, the default) the party screen takes P and the map key's
// hold (its last row shows or hides the followers), and Tab alone switches. Under -FTOldUi, as before: Tab or RB
// switches to the next member, P or LB (or the map key held) hides or shows the followers.
// StartLead and bStartSolo set the start (-ExploreLead=<Name> and -ExploreSolo for -game runs); the
// player's last choice is kept across a fight (UFTGameInstance).
//
// Jump in front of a low wall or a ledge (UFTLocomotionAnimInstance::SetTraversalFromRun's clips) goes over it or
// climbs onto it instead: forward and down traces find the near face, the top and its depth and the floor beyond,
// ftcore::TraversalSet picks the clip and where to enter it, and the capsule is carried along the plan's root path
// (no collision on the way) until the clip ends. -LocoVaultWall builds a test course ahead (a thin wall, a low
// block, a high block) and runs at it, traversing whatever it can. Each vault or climb begun is marked (where and which
// way) for the followers, who go over or onto the same face where they meet it (AFTPartyFollower).
//
// In the air, a ledge within reach (a deep top, from under the feet to past the hands) is caught when the jump is held
// or the stick points into it: one of the catch clips (SetTraversalFromRun's too) plays from where its catch is timed to
// meet the body's flight (ftcore::TraversalSet::planCatch), the body turns square into the face, and the capsule is
// carried from its flight into the catch and up onto the top as a climb is. Each catch begun is marked for the followers
// too, who catch the same ledge where they meet it in the air. At the catch the body hangs (ftcore::LedgeHang: the
// catch clip held at its catch), unless jump is still held with the stick into the face, which climbs straight up.
// Hanging, jump or the stick into the face climbs up (the clip plays on); crouch or the stick away from the face lets
// go, and the body falls pushed off the face, catching no ledge for a moment. The tunables are the "ledge" section of
// Data/Movement/movement.json. -LocoCatch builds deep blocks 150, 300 and 260 cm high ahead, jumps at each from about a
// metre out, hangs half a second and climbs.
//
// Roll goes the way the stick points (else the way the body moves, else the way it faces): one of the roll clips
// (SetTraversalFromRun's too) plays from its start, the body turns to the roll's way and the capsule follows the
// clip's root path along the real floor (up and down steps; a wall ends the roll, and with no floor under it the body
// lets go and falls). The roll keeps a speed of its own the whole way (the body's, but no slower than a run's, or a
// landing's than its clip's: ftcore::TraversalPlan::speed), the clip playing faster where its root slows in the tuck.
// The body is handed back once it is up and a moment on, or as soon as it is up when the stick points well off the
// roll's way. A press the body can't act on yet (mid-traversal) is kept for a moment. An asked roll is a dodge
// (ftcore::DodgeRoll, the dodge section of Data/Movement/movement.json): it waits out the last one's cooldown, its
// kind sets its speed (a sprint's Long roll keeps its momentum, a walk's Short one its own pace, a Tired one with too
// little stamina goes slow), and IsInvulnerable() holds through its cover window. Landings: touching down after
// landing.rollMinAir in the air, a moving body with crouch on, or with a roll pressed in the air within
// landing.pressWindow of the touchdown, rolls out of it, and any body does from a fall of landing.rollDrop or more
// (Data/Movement/movement.json), keeping its speed rather than landing hard. -LocoRollEvery=<s> rolls every s seconds; -LocoRollTurn=<deg>
// turns the stick that far off each roll's way until a second after it hands back.
//
// Crouch in a sprint slides (ftcore::SlideSet, from SetTraversalFromRun's slide clips): the drop-in nearest the body's
// speed plays from its take-off, and the body slides on the way it was going along the real floor, steered a little by
// the stick, slowing by friction and faster downhill. A wall met at a glance turns it along the wall; one met square on
// stops it. Letting go of the crouch (sprint pressed stands it up too) or slowing down gets it up into what the stick,
// sprint and crouch ask then: a sprint, a run, a walk, a crouch walk or a stand. Jump mid-slide (once it has gone
// ftcore::SlideTuning::jumpFrom; a press before is kept a moment) cancels it into a jump that takes off along the slide
// at the slide's speed. The tunables are in Data/Movement/movement.json. With -LocoRun, -LocoCrouchEvery=<s> slides
// every s seconds.
//
// -ExplorePortrait puts the camera in front of the face and keeps it there, for checking a look;
// -ExplorePortraitYaw= (180 front, 90 side, 0 back), -ExplorePortraitArm=, -ExplorePortraitHeight= and
// -ExplorePortraitPitch= move it, e.g. Arm=330 Height=0 Pitch=0 frames the whole body.
//
// The lead's weapon (FTLeadWeapons.h, as in the arena): left mouse (a tap), J or pad RT throws the kit's light attack (its
// chain), left mouse held ftcore::AimRules::heavyHold, K or pad Y its heavy. Right mouse held aims (ftcore::AimStance,
// Data/Battle/aim.json): the camera pulls in over the right shoulder, the HUD shows a centre reticle, the lead turns to
// the camera's aim and slows to a strafe, and an attack goes at the reticle point (a traced line from the camera): the
// pistol fires at it, the bolt flies at it, the gauntlets' light lunges toward it. Not in the van, a menu or a pause.
// -ExploreAim=<s> holds aim from s seconds of play on (for captures); ftcore::FieldStrike runs the attack from Data/Battle/arena.json's kit for the lead
// worn (its name, lower case; else the default) and hits any AFTFieldDummy in reach. The props, clips and effects are
// the arena's. -ExploreDummy stands a dummy ahead, within the first light attack's reach (at most 3 m; -ExploreDummyAt=<cm> sets it); -ExploreAttackEvery=<s> presses light every s seconds (every
// fourth press a heavy), for captures.

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "InputActionValue.h"

#include "FTLeadWeapons.h"
#include "FTLocomotionAnimInstance.h"
#include "ftcore/AimStance.h"
#include "ftcore/InputBindings.h"
#include "ftcore/AttackMove.h"
#include "ftcore/FieldUnstick.h"
#include "ftcore/FieldStrike.h"
#include "ftcore/DodgeRoll.h"
#include "ftcore/HeldUpJump.h"
#include "ftcore/LedgeHang.h"
#include "ftcore/StillCam.h"
#include "ftcore/Swim.h"
#include "ftcore/UiPartyScreen.h"
#include "ftcore/WaterBodies.h"
#include "ftcore/WaterFollow.h"
#include "ftcore/WaterLoco.h"
#include "ftcore/WetPrints.h"
#include "FTLook.h"
#include "FTTraversalProbe.h"

#include "FTExploreCharacter.generated.h"

class AFTExploreDirector;
class AFTPartyFollower;
class UAnimSequence;
class AFTPartyVan;
class APlayerController;
class UCameraComponent;
class UDecalComponent;
class UInputAction;
class UInputMappingContext;
class URectLightComponent;
class USpringArmComponent;
class UFTWaterSubsystem;
class UFTRimComponent;

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
	// Looks the story's cast wears that are not party members' (a lead's other outfit, keyed by ModelId).
	// Dialogue scenes search PartyLooks first, then these. Not part of the party.
	UPROPERTY(Config, EditAnywhere, Category = "Explore|Party")
	TArray<FFTLeadLook> CastLooks;
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
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void AddMovementInput(FVector WorldDirection, float ScaleValue = 1.f, bool bForce = false) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	// Ground-plane facing, for the interact prompt's cone.
	FVector2D GetFacing2D() const;

	// The party van (AFTPartyVan): boarding hides the lead and the party and hands the van this pawn's input and the
	// view; leaving puts the lead down at `At`, facing `Yaw`, as the party was.
	bool CanBoardVan() const;
	void BoardVan(AFTPartyVan* InVan);
	void LeaveVan(const FVector& At, double Yaw);
	// For locomotion captures: the camera frames the body under Root (its feet) as it frames this one.
	void WatchFrom(USceneComponent* Root);

	// The PartyLooks entry the pawn wears (-1 when PartyLooks is empty), and whether the followers are hidden.
	int32 GetLeadIndex() const { return LeadIndex; }
	bool IsPartyHidden() const { return bPartyHidden; }
	// The party screen under the new UI (FT::NewUiEnabled; its explore layer draws it while open) and its rows'
	// PartyLooks entries (-1: none), found at begin play and after every lead change: the partner is the first other
	// playable look, the Third the first look the player may not play.
	const ftcore::UiPartyScreen& GetPartyScreen() const { return PartyScreen; }
	int32 GetPartnerIndex() const { return PartnerIndex; }
	int32 GetThirdIndex() const { return ThirdIndex; }
	// Whether the partner's row has a lead to switch to (else it reads NOT HERE and never takes the cursor): the one
	// truth the screen's step (UiPartyInput::partnerHere) and its drawing read.
	bool IsPartnerHere() const { return PartyLooks.IsValidIndex(PartnerIndex) && PartnerIndex != LeadIndex; }
	// Whether a traversal (a vault, a climb, a catch, a roll or a slide) is carrying the body: a scene played in place waits
	// for it to end.
	bool IsTraversing() const { return bTraversing; }
	// The aim stance (right mouse held): whether the lead aims now and the stance's blend (0..1), for the HUD's reticle.
	bool IsAiming() const { return Aim.aiming(); }
	float GetAimAlpha() const { return Aim.alpha(); }
	// The dodge roll's state (stamina, cooldown, the one going on), and whether a hit now does nothing (its cover).
	const ftcore::DodgeState& GetDodge() const { return DodgeNow; }
	bool IsInvulnerable() const { return Dodge.invulnerable(DodgeNow); }
	// For the followers: how many vaults, climbs and catches the body has begun (not rolls or slides), and where and which
	// way the last one began, and whether it was a catch.
	uint32 GetTraversalsBegun() const { return TraversalsBegun; }
	const FFTTraversalMark& GetLastTraversal() const { return LastTraversal; }
	// The capsule (cm), which the followers probe ahead with too.
	static constexpr float CapsuleRadius = 34.f;
	static constexpr float CapsuleHalfHeight = 88.f;
	// CharacterMovement keeps a walking capsule hovering this far over the floor (cm, its CurrentFloor.FloorDist, held
	// between 1.9 and 2.4; measured 2.15 at rest, 2.04-2.15 walking): the mesh hangs this much lower than the capsule's
	// bottom, so the soles meet the floor.
	static constexpr float FloorHover = 2.15f;
	// For a scene that turns input off and on again: a held Run's release that came while input was off never arrived,
	// so the body goes back to its default gait, a held jump is let go and a roll press still kept is dropped. Crouch is
	// a toggle, not held; standing up from it is this call's choice, so a scene starts and ends with the lead on its feet.
	void ResetHeldInput();

protected:
	virtual void BeginPlay() override;
	virtual void PawnClientRestart() override;

private:
	void CreateInputObjects();
	// Maps the context's keys from FTInputBindings (unmapping the old ones first) and notes their generation; Tick calls
	// it again when the bindings' generation moves (a reload).
	void MapBoundKeys();
	UInputAction* MakeAction(const TCHAR* Name, bool bAxis2D);

	void HandleMove(const FInputActionValue& Value);
	void HandleLook(const FInputActionValue& Value);
	void HandleRunStart();
	void HandleRunStop();
	void HandleJump();
	void HandleJumpRelease();
	void HandleRoll();
	void HandleCrouch();
	void HandleInteract();
	void HandleInteractRelease();
	void HandleSwitchLead();
	void HandleToggleParty();
	// Run, crouch, and the map button held for the party toggle: polled from the binding table each frame, so a held
	// action can be held or toggled (FTInputBindings).
	void TickHeldKeys(float DeltaSeconds);
	// Whether the game takes the player's keys this frame: on foot, no menu, pause or scene.
	bool TakesInput();
	void HandleToggleMap();
	// The party screen (ftcore::UiPartyScreen, under the new UI only): Party's own key down, or the map key held
	// kUiPartyHold, opens it (a shorter map press is the map); a flick moves the cursor; letting go picks. The partner's
	// row switches to that lead, FOLLOW / WAIT HERE toggles the followers. PC null: input is taken away, which shuts it.
	void TickPartyScreen(const APlayerController* PC, float DeltaSeconds);
	// Esc (the controller's OnBack) while the party screen stands: its cancel, claimed so no pause menu opens.
	UFUNCTION()
	void HandlePartyBack();
	bool bPartyBack = false;     // Esc came since the last party step
	bool bPartyKeysHeld = false; // the party screen stands, or its confirm key is still held after it shut
	TWeakObjectPtr<class AFTPlayerController> BackBoundTo;
	void FindPartyRows();
	// Wears PartyLooks[Index] as the lead (not from the van or mid-traversal; a crouch ends).
	void SwitchLeadTo(int32 Index);
	// The worn look's weapon: its kit from Data/Battle/arena.json and the props, clips and effects on the body.
	void BeginWeapon(const FFTLeadLook& Look);
	// Reads Data/Battle/arena.json into WeaponTable once (logged, built-in kits when missing or refused).
	void LoadWeaponTable();
	// Whether the player may play this look as the lead: its kit has attacks (ftcore::arenaPlayable).
	bool IsPlayableLead(const FFTLeadLook& Look);
	void TickWeapon(float DeltaSeconds);
	// The aim and the attack button for this frame (TickWeapon, before the strike's step): the presses, the stance, the
	// camera, the body's turn and speed, the reticle point.
	void TickAim(float DeltaSeconds, ftcore::FieldBody Body);
	// What the attack under way allows (Data/Battle/attack_move.json): walking pace share and turn rate; off attack
	// full pace and no limit.
	ftcore::AttackMoveOut AttackMoveNow() const;
	float BaseWalkSpeed() const;
	bool ScenePlaying();
	// The way out of a too-steep slope or a wedge (ftcore::FieldUnstick, Data/Movement/unstick.json): the slide
	// downhill, and the step back to safe ground with a short fade.
	void TickUnstick(float DeltaSeconds);
	// The camera at the aim stance's alpha (0: the usual follow camera).
	void ApplyAimCamera();
	void TickWeaponShots();
	void SpawnFieldDummy();
	// Puts on a look, replacing the body, clips, pieces and grooms worn (FTLook).
	void WearLook(const FFTLeadLook& Look);
	// Gives the body's locomotion its loaded clips (and those found beside them) and the crouch its speed.
	void SetupLocomotion(const FFTLoadedLook& Loaded);
	// Traversal: finds what is ahead (FTTraversalProbe::Probe from the feet along the facing; false: nothing to go over or
	// onto; bAir: a deep top to catch, In the way into its face), starts a traversal (false: none fits; bLog says why),
	// carries the body along one under way, and builds -LocoVaultWall's (or -LocoCatch's) course.
	bool ProbeObstacle(const ftcore::TraversalSet& Set, bool bAir, ftcore::TraversalObstacle& Out, FVector& In) const;
	bool TryTraverse(bool bLog);
	// Starts a catch (false: not in the air, mid-traversal, crouched, not asked for, or none fits or is in reach yet; bLog
	// says the last two, once a flight each).
	bool TryCatch(bool bLog);
	// Starts a roll (false: not on the ground, mid-traversal, no room to stand up, or no roll clip; bLog says the
	// last), heading as a landing rolls when bLanding.
	bool TryRoll(bool bLanding, bool bLog);
	void TickTraversal(float DeltaSeconds);
	// Starts a slide (false: not sprinting on the ground, mid-traversal, too slow or no slide clip) and carries the
	// body along one under way.
	bool TrySlide();
	void TickSlide(float DeltaSeconds);
	// Moves the capsule to To's ground position over the real floor: up a step, across (stopping at a wall, which
	// Wall then holds) and down onto what is there (Floor, no blocking hit when nothing is); with no floor in reach it
	// is left at the height it started from.
	void MoveOverFloor(const FVector& To, FHitResult& Wall, FHitResult& Floor);
	// Where the soles are: the capsule's bottom less the floor hover, the frame traversals and slides are planned in.
	FVector FeetLocation() const;
	void SpawnTraversalCourse();
	// -ExploreWedge: a V trench of two 60-degree slabs ahead, the lead dropped into it (the unstick's test).
	void SpawnWedge();
	void TickWedge();
	void AddPortraitLights();
	// -ExploreLookDev: a neutral set for judging materials (see the .cpp).
	void EnterLookDev();
	void FixExposure(float Bias);
	void GreyGround();
#if !UE_BUILD_SHIPPING
	void Nightfall(); // -ExploreNight
	void SoakForReview(); // -ExploreWet
#endif
	// -ExploreWet=<Wetness>,<wet line cm above the feet>,<WetSilt>: review only (see the .cpp); unused in shipping builds.
	FVector WetReview = FVector::ZeroVector;
	FTimerHandle WetTimer;
	int32 WetSlots = -1;
#if !UE_BUILD_SHIPPING
	// -ExploreSoakSwap=<wetness>,<word>@<off s>[+<on s>]: review only (see the .cpp).
	void StepSoakSwap();
	void LogSoakSwap(const TCHAR* Stage);
	FString SoakSwapWord;
	float SoakSwapWet = -1.f;
	float SoakSwapOff = -1.f;
	float SoakSwapOn = -1.f;
	double SoakSwapStart = 0.0;
	int32 SoakSwapStage = 0;
	bool bSoakSwapBefore = false;
	FTimerHandle SoakSwapTimer;
	// -StillCam=x,y,z,yaw,pitch,fov: review only (see the .cpp).
	void HoldStillCam();
	ftcore::StillCam StillCamView;
	FTimerHandle StillCamTimer;
	bool bStillCamHeld = false;
#endif
	void StepPortraitShots();
	void HoldFaceAt(float Time);
	void HoldBodyAt(float Time);

	AFTExploreDirector* GetDirector();

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USpringArmComponent> Boom;
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY(VisibleAnywhere, Category = "Explore|Look")
	TObjectPtr<UFTRimComponent> Rim; // writes the rim term's direction while player 0 controls this lead (ft.Rim)

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
	TObjectPtr<UInputAction> InteractAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> SwitchLeadAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> PartyAction;
	uint32 KeyGeneration = 0; // the bindings' generation Context's keys were mapped from (FTInputBindings::Generation)

	UPROPERTY(Transient)
	TArray<TObjectPtr<USkeletalMeshComponent>> Pieces;
	// The look's garment pairs, both halves held (FTLook::PrepareFronts), for switching open or closed.
	UPROPERTY(Transient)
	FFTWornFronts WornFronts;
	// The look worn (WearLook), for taking a piece off or putting it on later (FTLook::SetPieceWorn).
	UPROPERTY(Transient)
	FFTLeadLook WornLook;
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
	// -ExplorePortraitShotEvery=<s>: the step between aiming and shooting (a shot every two steps).
	float ShotEvery = 2.5f;
	// -ExploreLookDevSpin=<deg/s>: the look-dev key circles the pawn at this rate (0 holds it still).
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> LookDevPivot;
	float LookDevSpin = 0.f;
	// -ExplorePortraitFaceClip=<anim> with -ExplorePortraitFaceTimes=<s,s,..>: each shot holds the face piece on that
	// clip at its time (one shot per time, yaws repeat), to check expressions in the game.
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> FaceClip;
	TArray<float> FaceTimes;
	// -ExplorePortraitBodyClip=<anim> with -ExplorePortraitBodyTimes=<s,s,..>: each shot holds the whole body (and every
	// leader-posed piece with it) on that clip at its time, so garments are judged posed in the game (a crouch, an arm
	// raise), not in a Blender render. Not with a face clip; the walk circle is off while a pose is held.
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> BodyClip;
	TArray<float> BodyTimes;

	TWeakObjectPtr<AFTExploreDirector> Director;
	// When the last search for a director found none (real seconds): a world without one is searched once a second, not per call (ScenePlaying asks every tick).
	double DirectorMissAt = -1.0e9;
	TWeakObjectPtr<AFTPartyVan> Van;
	bool bPartyHiddenBeforeVan = false;
	FFTFootSlideProbe SlideProbe;
	float JumpEvery = 0.f; // -LocoJump=<s>
	float JumpTimer = 0.f;
	float CrouchEvery = 0.f; // -LocoCrouchEvery=<s>
	float CrouchTimer = 0.f;
	bool bJumpHeld = false; // the jump pressed and not yet let go
	int32 CatchMissSaid = -1; // the last "none" TryCatch logged this flight: 0 none fits, 1 out of reach
	float RollAsked = 0.f; // a roll press still kept (s)
	bool bRollAskedAir = false; // ...pressed in the air: the touchdown rolls out of the landing
	float RollEvery = 0.f; // -LocoRollEvery=<s>
	float RollTimer = 0.f;
	float RollTurn = 0.f;     // -LocoRollTurn=<deg>
	float RollTurnLeft = 0.f; // how much longer the stick stays turned by RollTurn once the roll hands back (s)
	float PulseEvery = 0.f; // -LocoPulse=<s>
	float PulseTimer = 0.f;
	float PulseRest = 2.f;  // -LocoRest=<s>
	float LookRest = 0.f; // seconds since the last look input
	float FaceBy = 0.f;   // -LocoFace=<deg>
	bool bPortraitWalk = false;   // -ExplorePortraitWalk
	float PortraitMoveScale = 0.5f; // -ExplorePortraitMoveScale=<0..1> (1 runs)
	bool bPortraitJump = false;   // -ExplorePortraitJump
	double PortraitJumpAt = -10.0;
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
	// way it rolls); for its log, how long it has gone (s) and its slowest speed (cm/s).
	bool bTraversing = false;
	ftcore::TraversalPlan TravPlan;
	float TravTime = 0.f;
	FTransform TravFrame;
	float TravClock = 0.f;
	float TravSlowest = 0.f;
	// For the hand-back check: the soles' height over the floor through a roll, a slide or a clip's tail (min and max,
	// cm), and the capsule's height as it handed back, logged with what CharacterMovement makes of it a frame on.
	float SoleGapMin = 0.f;
	float SoleGapMax = 0.f;
	double HandBackZ = 0.0;
	double HandBackMeshZ = 0.0;
	bool bHandBackPending = false;
	void ResetSoleGap();
	void NoteSoleGap();
	void LogSoleGap(const TCHAR* What) const;
	// The vaults, climbs and catches begun, and the last one's mark (GetLastTraversal).
	uint32 TraversalsBegun = 0;
	FFTTraversalMark LastTraversal;
	// The slide under way (sliding() while it is), the way it goes, the floor's normal last met, whether a wall has
	// stopped it since its last step, and how long it has gone.
	ftcore::SlideState SlideNow;
	float SlideHeading = 0.f;
	FVector SlideFloor = FVector::UpVector;
	bool bSlideBlocked = false;
	float SlideTime = 0.f;
	float SlideJumpAsked = 0.f; // a jump pressed mid-slide, still kept (s)
	// The dodge roll: an asked roll is one (its rules from Data/Movement/movement.json).
	ftcore::DodgeRoll Dodge;
	ftcore::DodgeState DodgeNow;
	// The ledge hang (a catch's), and a jump or crouch pressed while hanging, still kept (s).
	ftcore::LedgeHang Ledge;
	ftcore::LedgeState LedgeNow;
	float LedgeJumpAsked = 0.f;
	float LedgeDropAsked = 0.f;
	// What the player asks of a hang now: jump and crouch, and the stick against the face caught.
	ftcore::LedgeInput MakeLedgeInput() const;
	// -LocoVaultWall: the course still to build, then (a course built and run) its way, run along every frame, traversing
	// whatever is ahead; -LocoCatch's course jumps at its walls instead.
	bool bCoursePending = false;
	bool bVaultCourse = false;
	bool bCatchCourse = false;
	FVector CourseDir = FVector::ForwardVector;
	uint32 SideTurns = 0;
	float SideYaw = 0.f;
	float SideHold = 0.f;
	int32 LeadIndex = -1;
	bool bPartyHidden = false;

	// The weapon: Data/Battle/arena.json (loaded once), the kit worn, its attack rules and its props, clips and effects;
	// the presses since the last tick, and -ExploreAttackEvery's timer.
	ftcore::ArenaTable WeaponTable;
	bool bWeaponTableTried = false;
	bool bWeaponTable = false;
	ftcore::FieldStrike Strike;
	FFTLeadWeapons Weapons;
	bool bPressLight = false;
	bool bPressHeavy = false;
	// The aim (right mouse held) and the attack button's tap and hold (left mouse), their rules from Data/Battle/aim.json
	// (loaded once), what an aimed attack does for the kit worn, the reticle point (world, cm), the walk speed the
	// movement handlers last set (the aim scales it) and what the aim set it to, -ExploreAim's start (below 0: off).
	ftcore::AimStance Aim;
	ftcore::PressHold AttackHold;
	// The held actions' hold or toggle latches, the crouch key's last state, whether the run is on, how long the map
	// button has been held (the party toggle) and whether that hold already toggled.
	ftcore::HeldInput AimInput;
	ftcore::HeldInput RunInput;
	bool bCrouchKeyWas = true;
	bool bRunOn = false;
	float MapHeld = 0.f;
	bool bMapHoldUsed = false;
	// The new UI's party screen, read once with the input objects: it takes the Party action and the map key's hold, and
	// SwitchLead stays on the keyboard alone.
	bool bNewUiParty = false;
	ftcore::UiPartyScreen PartyScreen;
	int32 PartnerIndex = -1;
	int32 ThirdIndex = -1;
	bool bAimRulesTried = false;
	ftcore::AttackMoveRules AttackMoveTable;
	ftcore::FieldUnstick Unstick;
	bool bUnstickTried = false;
	bool bWedgePending = false;  // -ExploreWedge
	FString WedgeShotPrefix;     // -ExploreWedgeShot=<path prefix>
	double WedgeDropAt = -1.0;
	double WedgeRescueAt = -1.0;
	int32 WedgeShotsTaken = 0;
	ftcore::AimKind AimKindNow = ftcore::AimKind::Focus;
	FVector ReticlePoint = FVector::ZeroVector;
	float AimSpeedBase = 0.f;
	float AimSpeedSet = -1.f;
	bool bAimCamera = false;
	float AimFrom = -1.f;
	float AttackEvery = 0.f;
	float AttackTimer = 0.f;
	int32 AttackPresses = 0;
	bool bDummyPending = false;
	float DummyDistance = 300.f; // cm
	// The body's chest bone for an aim with no target (found once per body; INDEX_NONE: none), and whether the body
	// was busy with a move of its own (vault, climb, hang, roll, slide) last frame.
	int32 ChestBone = INDEX_NONE;
	bool bWeaponBodyBusy = false;
	// -ExploreWeaponShot's prefix, shots left and taken, the settle time, the active attacks seen, the wait to quit.
	FString WeaponShotPrefix;
	int32 WeaponShotsLeft = 3;
	int32 WeaponShotIndex = 0;
	float WeaponShotDelay = 12.f;
	int32 WeaponShotSeen = 0;
	double WeaponShotDue = -1.0; // Strike time of the next shot; below 0: none
	float WeaponQuitIn = 0.f;
	// -ExploreWeaponStrip=<n>: in place of one shot per attack, n shots StripStep seconds apart from the first walking
	// light attack after the delay (StripAt: its Strike time; below 0: not begun), each frame's speed and upper share
	// logged while it runs.
	int32 StripShots = 0;
	float StripStep = 0.1f;
	double StripAt = -1.0;

	// Water (ftcore::WaterBodies through UFTWaterSubsystem), read each tick: a wade and a deep wade slow the walk by
	// the state's factor (AddMovementInput); over the swim depth the body is held at the surface (the movement's
	// flying mode at the swim speed, the feet eased to the swimmer's draft with its bob, the flow pushing it) until
	// the column is a wade's again. Jump in the water, facing a wall whose top is within reach, climbs out onto it (a
	// stand-in for a ladder or steps; TryWaterClimb).
	// In a swimDeep body's dive area (ftcore::WaterBodies::dive) a crouch press at the surface dives: from then on
	// ftcore::Swim (Data/Swim/swim.json) moves the body in 3D along the camera's look (TickSwimDeep) until it is back at
	// the surface, with its breath, the outer edge's push and, at no vitality left, a rescue to the nearest way out
	// (TickSwimRescue). The Third never dives (only a playable lead does), and the followers hold at the bank while the
	// lead is under. While swimming, at the surface too, the swim's flags hold: no attack, no dodge, weapons stowed.
	void TickWater(float DeltaSeconds);
	bool TryWaterClimb();
	ftcore::WaterState WaterNow;
	bool bSwimming = false;
	bool bSwimSaid = false;
	float SwimSavedFly = 0.f;
	float SwimSavedBrake = 0.f;
	// The free swim. False: at the surface (the surface hold moves the body).
	bool TickSwimDeep(const UFTWaterSubsystem& System, float DeltaSeconds);
	void LoadSwimTuning();
	void BeginSwimRescue(const UFTWaterSubsystem& System);
	void TickSwimRescue(float DeltaSeconds);
	// -SwimProof=land|watchdog [-SwimProofShots=<dir>] (dev, not shipped): the swim follow's capture run. It finds water
	// near the start, walks the lead from its dry edge in and out to a far point (16 m or more for watchdog), and holds
	// follower slot 0 stuck (its walk to an entry for land, its swim for watchdog) so it rejoins the lead; a still every
	// 0.5 s, a camera sweep after the rejoin; exit 0 only on the kind's own pass line, at 80-110 s.
	void TickSwimProof(float DeltaSeconds);
	int32 SwimProofStage = -1; // -1 off; 0 settle, 1 at the dry edge, 2 walking in, 3 out to the far point, 4 holding
	float SwimProofTime = 0.f;
	float SwimProofShotAt = 0.f;
	int32 SwimProofShotCount = 0;
	FString SwimProofShots;
	FString SwimProofKind;
	FVector SwimProofDry = FVector::ZeroVector;
	FVector SwimProofWet = FVector::ZeroVector;
	FVector SwimProofFar = FVector::ZeroVector;
	int32 SwimProofSlotPhase = -1;     // slot 0's last phase (its changes are logged with the watchdog's and the entry's counts)
	float SwimProofSlotStuck = 0.f;
	int32 SwimProofSlotFails = 0;
	float SwimProofRejoinAt = -1.f;    // SwimProofTime when slot 0 was rejoined
	float SwimProofSweep = -1.f;       // degrees the camera has turned round the lead (-1: not yet)
	int32 SwimProofGaveUp = 0;         // slot 0's ToEntry -> Ashore changes (a given-up entry walk)
	int32 SwimProofPath = 0;           // 0 not placed yet, 1 placed by this kind's own path, -1 placed some other way
	bool bSwimProofPass = false;       // placed by its own path, then Placed -> Surface (the rejoin swims on)
	bool bSwimProofLeadPlaced = false; // the harness put the lead in the water after 3 held-up jumps that didn't clear
	float SwimProofPlaceDue = -1.f;    // SwimProofTime the placement became due (it waits for the body to be down)
	float SwimProofPlacedAt = -1.f;    // SwimProofTime of the last placement (checked: swimming within 3 s)
	int32 SwimProofPlaceCount = 0;
	bool bSwimProofLeadSwims = false;  // the placed lead was seen swimming
	bool bSwimProofSpotFallback = false; // no water spot passed the check; the watchdog took one blocked on the way in
	// -SwimProof=boom (item 5 forced): 3 s after the lead holds at the far point the sweep starts, and slot 0 is put on the
	// sweep circle 90 deg ahead of the camera, slot 1 at 270 deg; both tread there
	// for the sweep. The run ends after the sweep; exit 0 when a partner was held and the sweep measured.
	float SwimProofHoldAt = -1.f;      // SwimProofTime the lead reached the far point
	int32 SwimProofBoomHeld = 0;       // partners held for the sweep
	int32 SwimProofBoomPulled = 0;     // sweep ticks the spring arm's probe pulled the camera in (a capsule or the world in the way)
	FString SwimProofBoomWhere;
	// Item 5 (the boom through a partner): during the sweep, the camera's distance to the nearest partner's capsule; under
	// 50 cm with that partner's mesh drawn is the clip. Counted per tick and per still (the still numbers kept, first 32).
	int32 SwimProofClipTicks = 0;
	int32 SwimProofSweepTicks = 0;
	float SwimProofSweepNearest = -1.f;
	int32 SwimProofClipShots[32] = {};
	int32 SwimProofClipShotCount = 0;
	// -SwimProof=exit (item 2 of 11-18): 3 s after the lead holds at the far point (its partners treading, 25 s at the most)
	// it swims to the nearest ladder in its water and climbs it. Each partner is timed from the lead out (its climb over,
	// standing on the quay) to its own Climbing -> Ashore (the strict line) and to its feet at the exit's top (dry feet);
	// the most it moves between two frames from the lead's climb on is kept with that frame's time and its phase.
	void TickSwimProofExit(const UFTWaterSubsystem& System, float DeltaSeconds);
	int32 SwimProofExitStage = 0;      // 0 not begun, 1 to the ladder, 2 the lead climbing, 3 the lead out
	int32 SwimProofExitLadder = -1;
	float SwimProofExitAt = -1.f;      // SwimProofTime the route to the ladder began
	float SwimProofLeadClimbAt = -1.f;
	float SwimProofLeadOutAt = -1.f;
	float SwimProofExitClimbAt[2] = {-1.f, -1.f};
	float SwimProofExitAshoreAt[2] = {-1.f, -1.f};
	float SwimProofExitDryAt[2] = {-1.f, -1.f};
	int32 SwimProofExitIndex[2] = {-2, -2};
	float SwimProofExitWorst[2] = {0.f, 0.f}; // cm
	float SwimProofExitWorstDt[2] = {0.f, 0.f};
	int32 SwimProofExitWorstPhase[2] = {-1, -1};
	int32 SwimProofExitPhase[2] = {-1, -1};
	FVector SwimProofExitLast[2] = {FVector::ZeroVector, FVector::ZeroVector};
	bool bSwimProofExitHasLast[2] = {false, false};
	// The body pitched along the swim (the mesh about the capsule's centre), eased; 0 puts it upright again.
	void PoseSwim(float TargetPitch, float DeltaSeconds);
	ftcore::Swim SwimBody;
	ftcore::SwimFlags SwimFlagsNow;
	bool bSwimTuningTried = false;
	bool bSwimUnder = false;       // Diving, Under or Surfacing last step
	bool bSwimWarned = false;      // the breath warning is logged once a dive
	bool bSwimHolstered = false;   // the swim stowed the weapons
	bool bSwimLeadDives = false;   // the lead is a playable lead (the Third never dives)
	int32 SwimLeadChecked = -2;    // the LeadIndex bSwimLeadDives was found for
	FVector2D SwimMoveAxis = FVector2D::ZeroVector; // the move stick under water (HandleMove), read by TickSwimDeep
	float SwimPitch = 0.f;         // the mesh's pitch shown (degrees, nose down negative)
	FVector SwimMeshLoc = FVector::ZeroVector;
	FRotator SwimMeshRot = FRotator::ZeroRotator;
	bool bSwimMeshSaved = false;
	// ftcore::WaterLoco (Data/Swim/water_loco.json): the water's own locomotion over Swim, its tread, surface stroke,
	// entries, dive and breach clips and the exits. Stepped once a tick on every TickWater path the body is in play on
	// (StepWaterLoco); at the surface its planar velocity, facing and sink move the body (the surface hold), and its
	// clip layers play over the locomotion (UFTLocomotionAnimInstance::SetWater). With no swim clip loaded for the
	// lead's body, its layers carry no weight and the stand-in plays as before.
	void StepWaterLoco(float DeltaSeconds);
	void LoadWaterLoco(UFTLocomotionAnimInstance* Anim);
	ftcore::WaterLoco WaterLocoBody;
	ftcore::WaterLocoPose WaterPose;
	FVector2D WaterMoveWanted = FVector2D::ZeroVector; // the surface move asked this tick (world planar), AddMovementInput
	uint64 SwimSteppedFrame = 0;   // the frame TickSwimDeep stepped Swim (its dived and surfaced are that frame's)
	uint64 WaterLocoFrame = 0;     // the frame WaterLoco was stepped
	bool bWaterLocoTried = false;
	int32 WaterClipsFor = -2;      // the LeadIndex the swim clips were looked up for, and the anim instance given them
	TWeakObjectPtr<UAnimInstance> WaterClipsAnim;
	bool bWaterUnderClips = false; // the prone under-water clips are loaded: the body is not pitched for them
	bool bWaterTopFalling = false; // at the top of TickWater: airborne, and its velocity (the entry reads them)
	FVector WaterTopVelocity = FVector::ZeroVector;
	bool bWaterClimbLadder = false; // the climb out is a ladder's (TryLadderClimb), else the mantle (TryWaterClimb)
	int32 WaterLeadExit = -1;       // the ladder's exit index the last climb out used (-1: the mantle, none)
	// The partners in the water (ftcore::WaterFollowParty): one plan for the party, stepped after WaterLoco, its orders
	// read by each follower's own Place (AFTPartyFollower::TickWater). The followers are the ones set up with this lead,
	// looked for again every half second while all are ashore; a slot keeps its member while it is in the water.
	// -LocoNoFollowerSwim: none of it; the followers hold at the bank as before.
	void StepWaterParty(float DeltaSeconds);
	ftcore::WaterFollowParty WaterParty;
	TWeakObjectPtr<AFTPartyFollower> WaterFollowers[ftcore::kWaterFollowMax];
	int32 NumWaterFollowers = 0;
	bool bWaterPartyParked = false;              // the lead was taken out of play since the plan last stepped
	bool bWaterPartyLeadSwam = false;
	bool bNoFollowerSwim = false;
	bool bNoFollowerSwimRead = false;
	bool bSwimRescue = false;
	float SwimRescueTime = 0.f;
	FVector SwimRescueFrom = FVector::ZeroVector;
	FVector SwimRescueUp = FVector::ZeroVector;  // over the start at the surface
	FVector SwimRescueTo = FVector::ZeroVector;
	bool bLeadGoDive = false;      // -LeadGoDive: a crouch press once in the dive area, a jump press when the route ends
	bool bLeadGoDived = false;
	bool bLeadGoRose = false;
	bool bWaterClimbing = false;
	float WaterClimbTime = 0.f;
	float WaterClimbSeconds = 0.9f; // the placed move's length: longer up a ladder
	// At a ladder (a way out of the map's water) within reach whose wall the body faces or pushes at: up it.
	bool TryLadderClimb(bool bPressed);
	FVector WaterClimbFrom = FVector::ZeroVector;
	FVector WaterClimbTo = FVector::ZeroVector;
	// What the lead does to the water and the water to it (UFTWaterSubsystem::LeadWater, each tick): splash, rings,
	// wake, and its wetness on its materials. The four scalar parameters Wetness (0..1), WetLineZ (world cm),
	// WetLineSoft (cm) and WetSilt (0..1, the water's stain; it stays when dry, with its line) are set on dynamic
	// instances of the materials of its skeletal meshes, made the first time it is wet and again after a look or a mesh
	// swap; a material that has no
	// such parameter ignores them. WetDrip is never set (the drips are off), so it stays at its default 0.
	void StirWater(float DeltaSeconds);
	void ShowWetness(const ftcore::WaterWetness& Wet);
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> WetMaterials;
	bool bWetMaterialsMade = false;
	// FTLook::WornMeshSerial() when WetMaterials were gathered: a piece's mesh swapped since (an under layer's cut, a
	// front) has new slots, so they are gathered again and every value written anew.
	uint32 WetMaterialsSerial = 0;
	float WetShown = 0.f;
	float WetLineShown = 0.f;
	float WetSiltShown = 0.f;
	// The wet switch (ftcore/LeadWet.h): one weather value (ft.LeadWet; 0, the default, is no rain; below 0 follows the
	// rain) soaks the cloth as the water does (through ShowWetness); the lead's surface wetness (that or its own water,
	// whichever is wetter) wears the look's wet grooms and writes SkinWet (0..1) to the same dynamic materials, where the
	// skin master smooths its up- and forward-facing open skin. The leads' MetaHuman skin takes it through its post-bake
	// roughness and specular multipliers (their dry values kept in WetSkinDry, one per WetMaterials entry, -1 where the
	// material has none); hair without a wet groom (a beard, brows) darkens and smooths through its own instances. The
	// shown amount eases over 2 s. No drips.
	float LeadWeather() const;
	void ShowLeadWet(float Weather, float DeltaSeconds);
	void ShowHairWet(float Wet);
	bool bGroomWet = false;
	float SkinWetShown = 0.f;
	float SkinWetEased = 0.f;
	TArray<FVector2f> WetSkinDry;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> WetHairMaterials;
	// Dry Roughness, HairRoughness and hairMelanin per WetHairMaterials entry (-1 where absent).
	TArray<FVector3f> WetHairDry;
	bool bWetHairMade = false;
	// The look's wet grooms and bindings, loaded with the look (FTLook::PreloadWetGrooms) so the swap loads nothing.
	UPROPERTY(Transient)
	TArray<TObjectPtr<UObject>> WetGroomHold;
	// Wet prints (ftcore::WetPrints): on foot on dry ground the lead leaves a print at each plant of its foot phase,
	// as wet as it is, fading as it dries. A fixed pool of decals, one per Core slot, made once the first time the lead
	// is wet (in the water, so the hitch is out of sight of any print); a laid
	// print's decal is placed by a short trace down at the foot (dry walkable ground only, never over water), its colour
	// carries the print and the engine's fade its age, so what shows is the Core's strength. The party's meshes take no
	// decals. While the lead is out of the water and wet, or prints show, the live count is logged every 2 s.
	void TickWetPrints(float DeltaSeconds);
	void PlaceWetPrint(int32 Index, float DryScale);
	ftcore::WetPrints WetPrints;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UDecalComponent>> WetPrintDecals;
	float WetPrintLook = 0.f;  // to the next sweep for faded decals and decal-taking party meshes
	float WetPrintOut = -1.f;  // seconds out of the water (-1 in it, or before it was ever in)
	float WetPrintLogAt = 0.f; // the out-of-water time of the next log line
	bool bWetPrintLoggedDry = true;
	// A scripted walk for captures and checks (BeginLeadGo, or -LeadGo=<x,y> in the plan frame of -WorldEyeSide after
	// -LeadGoAfter=<s>): the lead walks at the point; held up for a second it holds jump for 0.25 s with its push kept on
	// (with -LeadGoJump; ftcore::HeldUpJump counts and judges the tries); the run quits -LeadGoQuit=<s> after it began.
	// -LeadGoPace=<0.1..1> walks it slower (the stick's push).
	float LeadGoPace = 1.f;
	bool bLeadGo = false;
	bool bLeadGoJump = false;
	FVector LeadGoTo = FVector::ZeroVector;
	float LeadGoAfter = -1.f;
	float LeadGoQuit = 0.f;
	float LeadGoRan = 0.f;
	ftcore::HeldUpJump LeadGoTry;
	TArray<FVector> LeadGoNext; // the scripted walk's further points, taken in turn as each is reached
	// The held-up jump's events this frame (the release and the verdict, logged).
	void LeadGoTryEvents(float Distance);
	// What held the walk up, measured at a press (dev, not shipped): the top of what stands in the way ahead, over the
	// feet (cm; < 0 when nothing was found within reach), and whether the lead's own jump rises over it.
	void LogLeadGoHold();
	// How each held-up jump went (11-18 ruling, item 4; dev): from its press until the body swims, the next press or 3 s,
	// whether a traversal began (a vault from the ground or a catch in the air, its clip), the ledge phases and movement
	// modes seen, the feet's rise over the press every 0.1 s and its peak, and where the body first stood again 40 cm or
	// more up (what it stood on); logged in one line at the end.
	void TraceLeadGoJump(float DeltaSeconds, bool bPressed);
	void LogLeadGoTrace(const TCHAR* Why);
	bool bLeadGoTrace = false;
	float LeadGoTraceTime = 0.f;
	double LeadGoTraceFeet0 = 0.0;
	uint32 LeadGoTraceTraversals0 = 0;
	int32 LeadGoTraceTry = 0;
	float LeadGoTraceTravAt = -1.f;      // s after the press a traversal began (-1: none)
	bool bLeadGoTraceCatch = false;      // that traversal was a catch in the air (else a vault from the ground)
	int32 LeadGoTraceClip = -1;
	uint8 LeadGoTraceLedge = 0;          // bit per ftcore::LedgePhase seen
	uint32 LeadGoTraceModes = 0;         // bit per EMovementMode seen
	double LeadGoTracePeak = 0.0;        // cm: the feet's highest rise over the press
	float LeadGoTracePeakAt = 0.f;
	int16 LeadGoTraceRise[30] = {};      // cm over the press, each 0.1 s
	int32 LeadGoTraceSamples = 0;
	float LeadGoTraceStoodAt = -1.f;     // s after the press the body first stood 40 cm or more up (-1: never)
	double LeadGoTraceStoodRise = 0.0;
	FString LeadGoTraceStoodOn;          // what it stood on then (actor/component, and the floor's normal z)
	float LeadGoTraceVerdictAt = -1.f;   // s after the press the try was judged

public:
	// The lead walks at a point (cm) from now on.
	void BeginLeadGo(const FVector& To, bool bJumpWhenHeld);
	void EndLeadGo()
	{
		if (LeadGoTry.holding())
		{
			HandleJumpRelease();
		}
		LeadGoTry.reset();
		bLeadGo = false;
	}
	// One more point (cm) for the scripted walk, taken once the points before it are reached.
	void AddLeadGo(const FVector& To) { LeadGoNext.Add(To); }
	const ftcore::WaterState& GetWaterState() const { return WaterNow; }
	bool IsSwimming() const { return bSwimming; }
	// Under water (diving, under, surfacing) or being carried out by a rescue: the followers hold at the bank.
	bool IsUnderWater() const { return bSwimUnder || bSwimRescue; }
	// The water follow's order for the follower in Slot (AFTPartyFollower::SetWaterSlot); null with none (no water on the
	// map, -LocoNoFollowerSwim, or no such slot).
	const ftcore::WaterFollowOrder* GetWaterOrder(int32 Slot) const;
	// The water follow's tuning: the climb out's times and speeds, and -PartyExitFast's switches.
	const ftcore::WaterFollowTuning& GetWaterFollowTuning() const { return WaterParty.tuning(); }
	// The water's locomotion tuning (Data/Swim/water_loco.json, else the built-in), read once for every body.
	static const ftcore::WaterLocoTuning& SharedWaterLocoTuning();
	// Gives Anim the swim clips of the body called Name (/Game/Characters/<Name>/Swim/A_<clip>), Devin's where Name has
	// none; a clip that does not fit BodyAsset is left out. The count found (0: the stand-in plays).
	static int32 LoadWaterClips(UFTLocomotionAnimInstance& Anim, const USkeletalMesh* BodyAsset, const FString& Name, const TCHAR* LogName);
	float GetBreathShare() const { return SwimBody.last().breathShare; }
	bool IsBreathWarning() const { return SwimBody.last().warning; }
	// For the HUD's breath ring: the swim's last step (breath, breathShare, warning, state, and the canDive and guard
	// flags the core lane is adding) and the tuning it ran on (breathSeconds, warningSeconds). Both are the lead's own
	// Swim object, read by const reference; the HUD never steps it.
	const ftcore::SwimStep& GetSwimStep() const { return SwimBody.last(); }
	const ftcore::SwimTuning& GetSwimTuning() const { return SwimBody.tuning(); }
};
