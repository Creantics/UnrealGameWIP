#pragma once

// One body of the arena demo (docs/ARENA_DEMO.md): the player's pawn or an enemy, drawn rough from engine
// basic shapes and posed procedurally from what ftcore::ArenaFight says it is doing. AFTArenaGameMode spawns
// and places every one of them; the figure never moves itself and owns no rules.
//
// The player's figure wears a lead's look from config (a MetaHuman body with its face, outfit and grooms, walking
// and running by speed: FTLook.h, UFTLocomotionAnimInstance) when it loads, else the test human (a skeletal mesh
// looping its idle or walk clip by speed) when the project has it, else a body of shapes like the enemies'. An
// enemy kind with a skinned look (arena.json) is drawn as a skeletal mesh lifted off the ground whose clips the game
// mode picks and times from Core's state (PlayClip, UFTBeatFighterAnimInstance), the shapes hidden. Every figure has a weapon arm (a pivot at the
// shoulder and a blade) that winds up through an attack's startup, sweeps through its active part and
// settles through its recovery: a light swings across (alternating sides along the chain), a heavy, a
// riposte and an enemy's unparryable move come over the top. A guard raises a shield (gold while the parry
// window is open); a dodge crouches and leans along the roll; hitstun, a broken guard, a stagger and an
// open enemy lean back; the downed and the dead lie flat (a skinned enemy's clips do that themselves). An enemy's tell also shows on the floor: an arc
// of the move's reach and width that grows through its startup and flashes as the move goes active.
//
// Posing writes component transforms and material parameters only: nothing allocates per frame.

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"

#include "FTLook.h"

#include "FTArenaFighter.generated.h"

class UAnimSequence;
class UCameraComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class USkeletalMesh;
class USkeletalMeshComponent;
class USpringArmComponent;
class UStaticMesh;
class UStaticMeshComponent;

// How the weapon arm moves through an attack.
enum class EFTArenaSwing : uint8
{
	Across,   // a horizontal sweep from the right
	Backhand, // the same from the left
	Overhead, // raised over the head and brought down
	Thrust,   // straight ahead
};

// What a figure shows this frame. The game mode fills it from Core's output.
struct FFTArenaPose
{
	FVector Feet = FVector::ZeroVector; // world, centimetres
	float Yaw = 0.f;                    // degrees
	float Speed = 0.f;                  // m/s on the ground: idle or walk
	float LegYaw = 0.f;                 // degrees the legs turn from Yaw toward the way the body moves (+ right)
	bool bFootLock = false;             // the feet held where they stand (a strike whose clip has the legs)
	float WantSpeed = -1.f;             // m/s the walk heads for (0: braking to a stand; below 0 unknown: no starts or stops)
	float StopDistance = -1.f;          // m left to slide while braking to a stand (below 0 unknown)

	// The attack under way: its part, how far through that part (0..1) and how the arm moves.
	bool bAttacking = false;
	uint8 Part = 0; // ftcore::ArenaPart
	float PartFraction = 0.f;
	EFTArenaSwing Swing = EFTArenaSwing::Across;

	bool bGuard = false;
	bool bParrying = false;
	bool bDodging = false;
	FVector2D DodgeLocal = FVector2D(-1.0, 0.0); // the roll's way in the figure's own frame (x forward)
	float DodgeFraction = 0.f;
	float DodgeTime = 0.f; // s: the whole roll (the land-roll clip is fitted to it)
	bool bDodgeBack = false; // a locked dodge back from the target: drawn facing it, a hop back on the dash clip
	int8 DodgeSide = 0;      // a locked side dash across it (ftcore dodgeSide): +1 to the body's right, -1 to its left
	float LeanBack = 0.f;  // degrees: hitstun, guard break, stagger, open
	float Wobble = 0.f;    // degrees of side-to-side sway (a stagger)
	bool bLying = false;   // downed, down or dead
	bool bHealing = false;
	bool bAir = false;      // a jump from take-off to touch-down: the clips' jump, fall and land (UFTLocomotionAnimInstance::SetAir)
	float AirHeight = 0.f;  // cm the body is drawn over the floor (the jump's arc)
	float AirRise = 0.f;    // cm/s the arc climbs (below 0: coming down)
	float JumpClock = -1.f; // s on the jump's clock (Core's jumpTime; -1: not jumping), for -ArenaJumpFloor
	float AirFraction = -1.f; // 0..1 through the arc from leaving the ground to touching down (-1: on the ground)

	FLinearColor Tint = FLinearColor::White; // the body's colour this frame

	// An enemy's floor tell: its reach (cm) and half-width (radians, pi for all round) and how far
	// through the startup (0..1); Flash 0..1 after it goes active. Reach 0: none. Width > 0 (cm): a
	// straight lane that wide out to the reach (a shot) instead of the arc.
	float TellReach = 0.f;
	float TellHalfAngle = 0.f;
	float TellWidth = 0.f;
	float TellFraction = 0.f;
	float TellFlash = 0.f;
	bool bTellUnblockable = false;
};

// One part of a skinned figure's carried piece after it splits (AFTArenaFighter::SplitCarried).
struct FFTCarriedPiece
{
	FString Mesh;                           // a static mesh's object path, modelled where it sits in the hand at rest
	FName Bone;                             // the hand it goes into
	FVector OffsetMeters = FVector::ZeroVector; // a tweak, along the mesh's axes
	FRotator Turn = FRotator::ZeroRotator;  // a tweak about the part's centre, in the mesh's axes
};

UCLASS()
class FORGOTTENTHIRD_API AFTArenaFighter : public APawn
{
	GENERATED_BODY()

public:
	static constexpr int32 TellSegments = 14; // the floor tell's bars

	AFTArenaFighter();

	// Sizes the body (radius in metres, as Core's bodyRadius, and standing height in metres; 0 derives the
	// height from the radius as the figure always did) and tints it; bPlayer picks the lead's body and the
	// camera. Call once after spawning. For the player, a Look is worn when it loads (its pieces and grooms too, and
	// its locomotion clips); with none, or when its body doesn't load (one warning), the test human is.
	void Setup(bool bPlayer, float BodyRadius, const FLinearColor& BaseColor, float HeightMeters = 0.f,
		const FFTLeadLook* Look = nullptr);
	// Whether the lead's body has both side-dash clips (left and right) to draw a locked side dash with, and the dashes
	// are not swapped for the squash (-ArenaNoDash). Call after Setup.
	bool CanSideDash() const;
	// Draws an enemy as Mesh instead of the shapes: lifted HoverMeters off the ground, scaled, turned YawOffset degrees
	// from the mesh's own facing. Its height (for the HUD's markers) is HeightMeters x Scale over the hover. Call after
	// Setup. False when the mesh is null.
	bool SetupSkinned(USkeletalMesh* Mesh, float HoverMeters, float Scale, float YawOffset, float HeightMeters);
	// A skinned figure shows Clip at Time (seconds into it; looping wraps) over a short crossfade from the clip before it.
	// bHoldRoot false lets the clip's root travel through (a fall). Call every frame; a null clip changes nothing.
	// Blend: the crossfade into a new clip, seconds; 0 the anim instance's own. bAdditiveIn: a new clip fades in additively
	// (its motion lands at full size on the pose it cuts in from; UFTBeatFighterAnimInstance).
	void PlayClip(const UAnimSequence* Clip, float Time, bool bLoop, float DeltaSeconds, bool bHoldRoot = true, float Blend = 0.f,
		bool bAdditiveIn = false);
	bool IsSkinned() const { return SkinKind == ESkin::Creature; }
	bool WearsLook() const { return SkinKind == ESkin::Lead; }
	// The camera's rig in metres (ftcore::StageCamera, scaled with the arena): the boom's length, the pivot's
	// height over the feet, the shoulder offset sideways and up, and the field of view in degrees.
	void SetCameraRig(float ArmMeters, float PivotMeters, float SideMeters, float UpMeters, float FovDegrees);
	// The lens tilted up on the boom's end by this much (degrees; ftcore::arenaSplitPitch's tilt): a look steeper than
	// the boom may swing.
	void SetCameraTilt(float Degrees);
	void ApplyPose(const FFTArenaPose& Pose);
	// The jump's clock from the fighter's kit (s), so a lead's take-off and landing clips fit Core's arc and landing.
	void SetJumpTiming(float Startup, float Air, float Land) { JumpStartup = Startup; JumpAirTime = Air; JumpLandTime = Land; }
	// How far the body is drawn lifted off its feet' floor for the jump (cm), for a view that follows it.
	float GetJumpLift() const { return AirLift; }
	// Drops the lead's held feet and sunk pelvis (UFTLocomotionAnimInstance::ResetFeet): the body was put somewhere
	// else. ApplyPose does it itself when the body goes down.
	void ResetFeet();
	// Every skeletal mesh on the figure poses after Host ticks (it places the figure), now and for any mesh the figure
	// makes later (a look worn or a skin set again). Followers and grooms tick after the mesh they follow already.
	void PoseAfter(AActor* Host);
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

	// Where the HUD hangs this figure's markers: above its head, and its chest.
	FVector GetAnchor() const;
	FVector GetChest() const;
	// Where a skinned figure's bone or socket Name is now, in the world (effects hang there). False when the figure is
	// drawn as shapes or its mesh has no such name.
	bool GetSkinPoint(FName Name, FVector& Out) const;
	// A skinned figure's carried piece splits: the mesh's sections drawn with material slot HideSlot go, and each piece
	// (a static mesh modelled where it sits in its hand at rest, in the mesh's own space) is held by the bone from there,
	// after its Turn and OffsetMeters tweak. Once a figure; false when it isn't skinned or already split. A piece that
	// doesn't load is skipped with a warning.
	bool SplitCarried(FName HideSlot, TConstArrayView<FFTCarriedPiece> Split);
	// Makes the split's parts ahead of it, on their bones and hidden, so SplitCarried only swaps what shows. Once a
	// figure; false when it isn't skinned, has them already, or none loaded.
	bool PrepareCarried(TConstArrayView<FFTCarriedPiece> Split);
	// Undoes SplitCarried (a fight begun again): the piece whole on the figure, the parts hidden (kept for the next).
	void MendCarried();
	bool IsSplit() const { return bSplit; }
	float GetHeight() const { return Height; }
	const FLinearColor& GetBaseColor() const { return BaseColor; }

	USpringArmComponent* GetBoom() const { return Boom; }
	// The skeletal mesh a skinned figure is drawn with (null for the shapes), for hanging effects off its bones.
	USkeletalMeshComponent* GetSkin() const { return IsSkinned() ? Skin.Get() : nullptr; }
	// The lead's body when the figure wears a look (null otherwise), for the lead's weapon (FTLeadWeapons.h).
	USkeletalMeshComponent* GetLeadSkin() const { return WearsLook() ? Skin.Get() : nullptr; }
	// Shows or hides the shapes' blade (a lead whose own clips strike hides it).
	void SetBladeShown(bool bShown);

private:
	void SetTellVisible(bool bVisible);
	void ShowSlot(USkeletalMesh* Mesh, FName HideSlot, bool bShow);
	void MakeCarried(USkeletalMesh* Mesh, TConstArrayView<FFTCarriedPiece> Split, bool bVisible);
	// Puts Look on the player's skin: false (nothing changed) when its body doesn't load.
	bool WearLook(const FFTLeadLook& Look);

	// What the Skin component shows: nothing (the shapes), the test human, a lead's look or a creature's mesh.
	enum class ESkin : uint8
	{
		Shapes,
		TestHuman,
		Lead,
		Creature,
	};

	UPROPERTY(VisibleAnywhere, Category = "Arena")
	TObjectPtr<USceneComponent> Root;
	// Leans, crouches and lies down; everything visible hangs off it.
	UPROPERTY(VisibleAnywhere, Category = "Arena")
	TObjectPtr<USceneComponent> Visual;
	UPROPERTY(VisibleAnywhere, Category = "Arena")
	TObjectPtr<UStaticMeshComponent> Body;
	UPROPERTY(VisibleAnywhere, Category = "Arena")
	TObjectPtr<UStaticMeshComponent> Head;
	UPROPERTY(VisibleAnywhere, Category = "Arena")
	TObjectPtr<UStaticMeshComponent> Visor;
	UPROPERTY(VisibleAnywhere, Category = "Arena")
	TObjectPtr<USkeletalMeshComponent> Skin;
	// The lead's look: pieces worn on the body (clothes, a face) and the grooms bound to them.
	UPROPERTY()
	TArray<TObjectPtr<USkeletalMeshComponent>> Pieces;
	// The look's garment pairs, both halves held (FTLook::PrepareFronts), for switching open or closed.
	UPROPERTY(Transient)
	FFTWornFronts WornFronts;
	UPROPERTY()
	TArray<TObjectPtr<USceneComponent>> Grooms;
	// The carried piece's parts in the hands, after SplitCarried.
	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> Carried;
	bool bSplit = false;
	UPROPERTY(VisibleAnywhere, Category = "Arena")
	TObjectPtr<USceneComponent> Arm;
	UPROPERTY(VisibleAnywhere, Category = "Arena")
	TObjectPtr<UStaticMeshComponent> Blade;
	UPROPERTY(VisibleAnywhere, Category = "Arena")
	TObjectPtr<UStaticMeshComponent> Shield;
	UPROPERTY(VisibleAnywhere, Category = "Arena")
	TObjectPtr<UStaticMeshComponent> Tell[14]; // TellSegments (a literal for UHT)
	UPROPERTY(VisibleAnywhere, Category = "Arena")
	TObjectPtr<USpringArmComponent> Boom;
	UPROPERTY(VisibleAnywhere, Category = "Arena")
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> ShapeMaterial;
	UPROPERTY()
	TObjectPtr<USkeletalMesh> HumanMesh;
	UPROPERTY()
	TObjectPtr<UAnimSequence> HumanIdle;
	UPROPERTY()
	TObjectPtr<UAnimSequence> HumanWalk;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> BodyMaterial;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> BladeMaterial;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> ShieldMaterial;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> TellMaterial;

	FLinearColor BaseColor = FLinearColor::White;
	float Height = 180.f; // cm
	float Radius = 40.f;  // cm
	ESkin SkinKind = ESkin::Shapes;
	float SkinHover = 0.f; // cm: a creature's lift
	bool bWalking = false;
	bool bTellShown = true;
	bool bShieldShown = true;
	bool bWasLying = false;
	// A lead's roll: the land-roll traversal clip playing for it (-1: none), from its touchdown to back up.
	int32 RollClip = -1;
	float RollFrom = 0.f;
	float RollTo = 0.f;
	float RollRate = 1.f;
	float RollFraction = 0.f;
	bool bRollPlanFailed = false;
	// The roll's lift off the floor (cm), and the balls' height standing (read once; 1e9: not yet).
	float RollLift = 0.f;
	float RollLiftFall = 0.f; // cm/s the lift comes down at as the clip fades out
	float AirLift = 0.f;      // cm: the jump's lift as drawn (floor hold, the arc and the head kept on it; eased)
	// The jump's clock (Core's jumpStartup, jumpAir, jumpLand; SetJumpTiming), handed to the locomotion so its take-off
	// and landing clips fit it. 0: not set, the clips keep their own timing.
	float JumpStartup = 0.f;
	float JumpAirTime = 0.f;
	float JumpLandTime = 0.f;
	float StandHead = 0.f; // the head bone's height in the reference pose (cm, unscaled; 0: not read yet)
	void ReadFloorBones();
	float JumpFloorAfter = 0.f; // s left to log after a jump (-ArenaJumpFloor)
	// -ArenaRollFloor: how long the clip's fade-out after a roll or dash is still logged.
	float RollFadeLog = 0.f;
	// The dashes tightened (not -ArenaDashTall): the trunk pitched back under the dash clip, held through its fade.
	bool bDashPitched = false;
	float RollFloorBall = 1e9f;
	// Whether the land-roll clip plays for this roll (picked by the planted foot), or the squash stands in for it.
	bool RollWithClip(const FFTArenaPose& Pose);
	TWeakObjectPtr<AActor> PoseHost;
	void TieMeshTicks();
	void UntieMeshTicks();
	bool bPlayerFigure = false; // the player's figure never shows the guard plate
	FLinearColor LastTint = FLinearColor(-1.f, -1.f, -1.f);
	FLinearColor LastShield = FLinearColor(-1.f, -1.f, -1.f);
	FLinearColor LastTell = FLinearColor(-1.f, -1.f, -1.f);
};
