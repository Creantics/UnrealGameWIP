#pragma once

// One body of the arena demo (docs/ARENA_DEMO.md): the player's pawn or an enemy, drawn rough from engine
// basic shapes and posed procedurally from what ftcore::ArenaFight says it is doing. AFTArenaGameMode spawns
// and places every one of them; the figure never moves itself and owns no rules.
//
// The player's figure is the test human (a skeletal mesh looping its idle or walk clip by speed) when the
// project has it, else a body of shapes like the enemies'. Every figure has a weapon arm (a pivot at the
// shoulder and a blade) that winds up through an attack's startup, sweeps through its active part and
// settles through its recovery: a light swings across (alternating sides along the chain), a heavy, a
// riposte and an enemy's unparryable move come over the top. A guard raises a shield (gold while the parry
// window is open); a dodge crouches and leans along the roll; hitstun, a broken guard, a stagger and an
// open enemy lean back; the downed and the dead lie flat. An enemy's tell also shows on the floor: an arc
// of the move's reach and width that grows through its startup and flashes as the move goes active.
//
// Posing writes component transforms and material parameters only: nothing allocates per frame.

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"

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
	float LeanBack = 0.f;  // degrees: hitstun, guard break, stagger, open
	float Wobble = 0.f;    // degrees of side-to-side sway (a stagger)
	bool bLying = false;   // downed, down or dead
	bool bHealing = false;

	FLinearColor Tint = FLinearColor::White; // the body's colour this frame

	// An enemy's floor tell: its reach (cm) and half-width (radians, pi for all round) and how far
	// through the startup (0..1); Flash 0..1 after it goes active. Reach 0: none.
	float TellReach = 0.f;
	float TellHalfAngle = 0.f;
	float TellFraction = 0.f;
	float TellFlash = 0.f;
	bool bTellUnblockable = false;
};

UCLASS()
class FORGOTTENTHIRD_API AFTArenaFighter : public APawn
{
	GENERATED_BODY()

public:
	static constexpr int32 TellSegments = 14; // the floor tell's bars

	AFTArenaFighter();

	// Sizes the body (radius in metres, as Core's bodyRadius) and tints it; bPlayer picks the test human and
	// the camera. Call once after spawning.
	void Setup(bool bPlayer, float BodyRadius, const FLinearColor& BaseColor);
	void ApplyPose(const FFTArenaPose& Pose);

	// Where the HUD hangs this figure's markers: above its head, and its chest.
	FVector GetAnchor() const;
	FVector GetChest() const;
	float GetHeight() const { return Height; }
	const FLinearColor& GetBaseColor() const { return BaseColor; }

	USpringArmComponent* GetBoom() const { return Boom; }

private:
	void SetTellVisible(bool bVisible);

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
	bool bHuman = false;
	bool bWalking = false;
	bool bTellShown = true;
	bool bShieldShown = true;
	FLinearColor LastTint = FLinearColor(-1.f, -1.f, -1.f);
	FLinearColor LastShield = FLinearColor(-1.f, -1.f, -1.f);
	FLinearColor LastTell = FLinearColor(-1.f, -1.f, -1.f);
};
