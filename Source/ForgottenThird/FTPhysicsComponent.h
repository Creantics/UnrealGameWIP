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
// The profile is read once in BeginPlay; buffers are sized there. Nothing here allocates per frame.

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "CollisionQueryParams.h"

#include "ftcore/Physics.h"

#include "FTPhysicsComponent.generated.h"

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
	// Body indices a hit reaction blends; found once per reaction.
	TArray<int32> HitBodies;

	FCollisionQueryParams SweepParams;
};
