#pragma once

// Applies the Core physics feel (ftcore/Physics.h) to the owner's physics bodies under Chaos. See
// docs/PHYSICS_UE.md.
//
//   Knockback   F6: one sweep along the slide, then ftcore::planKnockback cuts the impulse so a body stops short
//               of a wall instead of slamming into it.
//   KO          F8: swaps in the ragdoll asset built by FTBuildPhysics (never the mesh's own physics asset),
//               simulates every body, and applies ftcore::splitKoImpulse (whole-body velocity change plus a
//               capped local push). A profile with useAuthoredKo refuses (the authored animation plays instead).
//   Hit react   F9: simulates the bodies below a bone and blends them out with ftcore::hitReactionWeight.
//   Going heavy F8/F10: after koHeavyAfter seconds, slow bodies are damped by ftcore::heavyDampingFactor.
//   Settle      ftcore::SettleTracker puts the ragdoll to sleep once it has been quiet for settleTime.
//   Speed cap   Chaos has no per-body linear cap, so maxLinearVelocity is enforced here each tick; the angular
//               cap is also written into the asset's bodies.
//   Hit-stop    F7: an approximation. Chaos can't pause one body bit-exactly, so frozen frames zero the bodies'
//               velocities and gravity and restore both afterwards.
//
// A lead (SetupForLead, at wear time): the skin's bodies are made (physics only, blocking the static world alone;
// the simulation never moves the skin component), the ragdoll asset is set on the skin once, the humanoid map is bound to the
// skin's bones, and the bodies under each ragdoll part are found and kept, so a blow looks nothing up. React plays
// one ftcore::ArenaHitReaction (ftcore/ArenaHitReact.h): Light simulates the struck part's bodies, Stagger the upper
// body from the spine with the push at the chest, Knockout the whole body (Knockout). The pelvis and the legs under
// it never simulate together while the lead can be controlled: a blow on the pelvis moves the upper body. While a
// part simulates, a physical-animation drive (the profile's motor, ftcore::ragdollDriveStrengths) pulls it toward the
// clip, so the part gives and comes back rather than hanging limp; the drive is off through a knockout. Recover
// brings a knocked-out body back to the clip.
//
// The profile is read once in BeginPlay; buffers are sized there and in SetupForLead. Nothing here allocates per
// frame.

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "CollisionQueryParams.h"
#include "Engine/EngineTypes.h"

#include "ftcore/ArenaHitReact.h"
#include "ftcore/Physics.h"

#include "FTPhysicsComponent.generated.h"

class UPhysicalAnimationComponent;
class UPhysicsAsset;
class UPrimitiveComponent;
class USkeletalMeshComponent;
struct FBodyInstance;

UCLASS(ClassGroup = (ForgottenThird), meta = (BlueprintSpawnableComponent))
class FORGOTTENTHIRD_API UFTPhysicsComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFTPhysicsComponent();

	// The ragdoll profile, as its Data/Physics/ragdoll file is called ("gentle").
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Physics")
	FName Profile = TEXT("gentle");

	// The ragdoll asset FTBuildPhysics wrote (/Game/Physics/PHYS_<mesh>_Ragdoll). Swapped in on KO.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Physics")
	TObjectPtr<UPhysicsAsset> RagdollAsset;

	// The body to drive. Empty: the owner's first skeletal mesh, else its root primitive.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics")
	TObjectPtr<UPrimitiveComponent> Target;

	// Knockback: a horizontal impulse in N*s (the Core units) against ground with this friction.
	// Returns the planned slide in metres (after the wall cut).
	UFUNCTION(BlueprintCallable, Category = "Physics")
	float Knockback(FVector ImpulseNs, float GroundFriction = 0.8f);

	// KO: ragdolls the target and applies the split impulse (N*s) at StruckBone. False when the profile uses an
	// authored KO, or when there is no skeletal target or ragdoll asset.
	UFUNCTION(BlueprintCallable, Category = "Physics")
	bool Knockout(FVector ImpulseNs, FName StruckBone);

	// A hit reaction: simulates the bodies below Bone for hitHold + hitBlendOut seconds, pushed by ImpulseNs.
	UFUNCTION(BlueprintCallable, Category = "Physics")
	bool HitReact(FVector ImpulseNs, FName Bone);

	// Freezes the target's bodies for this many frames (F7).
	UFUNCTION(BlueprintCallable, Category = "Physics")
	void HitStop(int32 Frames);

	// A lead's set-up, at wear time (never per frame): Skin becomes the target and gets Ragdoll as its physics asset,
	// the profile and Data/Physics/ragdoll/humanoid_map.json are read, the map is bound to the skin's bones, the
	// bodies under each part are kept, and the drive is applied. False (and logged) when anything is missing: the
	// component then reacts to nothing. A second call (a new look) sets up again.
	bool SetupForLead(USkeletalMeshComponent* Skin, UPhysicsAsset* Ragdoll);
	// Forgets the lead set-up (the skin's mesh changed): any reaction or knockout ends at once.
	void ClearLeadSetup();
	bool HasLeadSetup() const { return bLeadSetup; }
	// The hit setup bound with the lead's map and profile, for ftcore::arenaHitReactions.
	const ftcore::ArenaHitSetup& GetHitSetup() const { return HitSetup; }
	// The bone a ragdoll part follows on this skin, or NAME_None.
	FName PartBone(int32 Part) const;

	// Plays one reaction (ftcore::arenaHitReaction or arenaHitReactions). WorldDir: the push on the ground plane
	// (attacker to target); the strength is the reaction's raw N*s. The reaction's hit-stop frames freeze the
	// simulated bodies (display only). False when it did nothing (no set-up, None, already down, no bodies).
	bool React(const ftcore::ArenaHitReaction& Reaction, const FVector& WorldDir);

	// Back from a knockout (the partner rises, a restart): the bodies blend from the ragdoll to the clip over
	// RecoverSeconds (at once when bInstant), then stop simulating, and the drive comes back. A hit reaction or a
	// hit-stop under way ends at once. No get-up clip is played yet: the blend is timed, and the log says so and gives
	// how far the pelvis lay from the skin's place, in cm.
	void Recover(bool bInstant = false);
	bool IsRecovering() const { return bRecovering; }

	// Seconds Recover takes to blend back to the clip.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics")
	float RecoverSeconds = 0.35f;

	UFUNCTION(BlueprintPure, Category = "Physics")
	bool IsKnockedOut() const { return bKnockedOut; }

	UFUNCTION(BlueprintPure, Category = "Physics")
	bool IsFrozen() const { return Stop.frozen(); }

	const ftcore::RagdollProfile& GetProfile() const { return RagdollProfile; }
	bool HasProfile() const { return bProfileLoaded; }

	// Times the settle assist put the body to sleep, for checks.
	int32 GetSettleCount() const { return SettleCount; }

	// <ProjectDir>/Data/Physics, where FTBuildPhysics and this component read their data.
	static FString DataDir();
	// "/Game/Physics/PM_<Name>.PM_<Name>" for a materials.json entry.
	static FString MaterialPath(const FString& Name);
	// "/Game/Physics/PHYS_<Mesh>_Ragdoll" (a package name).
	static FString RagdollPackage(const FString& MeshName);
	// Reads Data/Physics/ragdoll/<Name>.json; logs and returns false on failure.
	static bool LoadProfile(const FString& Name, ftcore::RagdollProfile& Out);

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;

private:
	USkeletalMeshComponent* SkeletalTarget() const;
	int32 NumBodies() const;
	FBodyInstance* Body(int32 Index) const;
	void CapSpeeds();
	void UpdateKo(float DeltaTime);
	void UpdateHitReaction(float DeltaTime);
	void Freeze();
	void Thaw();
	bool StartHit(int32 Part, FName PushBone, const FVector& ImpulseNs);
	void EndHit();
	void UpdateRecover(float DeltaTime);
	void FinishRecover();
	void SetDrive(float Strength);
	// Starts or stops simulating the kept HitBodies (no lookup, no allocation).
	void SetHitBodiesSimulating(bool bSimulate);
	void SetDriveTicking(bool bTick);

	ftcore::RagdollProfile RagdollProfile;
	ftcore::SettleTracker Settle;
	ftcore::HitStop Stop;
	bool bProfileLoaded = false;
	bool bKnockedOut = false;
	bool bFrozen = false;
	bool bHitActive = false;
	float KoTime = 0.f;
	float HitTime = 0.f;
	FName HitBone;
	int32 SettleCount = 0;

	// Saved per body while frozen; sized in BeginPlay and on KO (never in a tick).
	TArray<FVector> FrozenLinear;
	TArray<FVector> FrozenAngular;
	TArray<bool> FrozenGravity;
	// Body indices a hit reaction blends; found once per reaction (a lead's: copied from PartBodies).
	TArray<int32> HitBodies;

	// The lead set-up (SetupForLead).
	bool bLeadSetup = false;
	bool bRecovering = false;
	float RecoverTime = 0.f;
	ftcore::ArenaHitSetup HitSetup;
	TArray<FName> PartBones;      // by ragdoll part: the bone it follows on the skin, or NAME_None
	TArray<int32> PartBodyStart;  // by part: first index in PartBodies
	TArray<int32> PartBodyCount;  // by part: how many bodies are at or under its bone
	TArray<int32> PartBodies;     // the skin's body indices, part by part
	// The skin's collision and transform mode before SetupForLead, given back by ClearLeadSetup.
	bool bSkinSaved = false;
	TEnumAsByte<ECollisionEnabled::Type> SavedCollision = ECollisionEnabled::NoCollision;
	TEnumAsByte<ECollisionChannel> SavedObjectType = ECC_WorldDynamic;
	FCollisionResponseContainer SavedResponses;
	uint8 SavedUpdateMode = 0;
	UPROPERTY(Transient)
	TObjectPtr<UPhysicalAnimationComponent> Drive;
	ftcore::RagdollDriveStrengths DriveStrengths;

	FCollisionQueryParams SweepParams;
};
