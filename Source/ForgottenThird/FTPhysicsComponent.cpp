#include "FTPhysicsComponent.h"

#include "ForgottenThird.h"

#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicsAsset.h"

#include <string>

namespace
{
// The most bodies a hit reaction or a frozen frame tracks without growing a buffer (the humanoid map has 12).
constexpr int32 kPhysReserveBodies = 64;

FVector PhysToUnrealImpulse(const float Ns[3])
{
	return FVector(ftcore::physImpulseToUnreal(Ns[0]), ftcore::physImpulseToUnreal(Ns[1]), ftcore::physImpulseToUnreal(Ns[2]));
}
}

UFTPhysicsComponent::UFTPhysicsComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	// After physics, so the cap, damping and settle see this frame's velocities.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

FString UFTPhysicsComponent::DataDir()
{
	return FPaths::ProjectDir() / TEXT("Data/Physics");
}

FString UFTPhysicsComponent::MaterialPath(const FString& Name)
{
	const FTCHARToUTF8 Utf8(*Name);
	const std::string Asset = ftcore::physMaterialAssetName(std::string_view(Utf8.Get(), static_cast<size_t>(Utf8.Length())));
	const FString AssetName = UTF8_TO_TCHAR(Asset.c_str());
	return FString::Printf(TEXT("/Game/Physics/%s.%s"), *AssetName, *AssetName);
}

FString UFTPhysicsComponent::RagdollPackage(const FString& MeshName)
{
	return FString::Printf(TEXT("/Game/Physics/PHYS_%s_Ragdoll"), *MeshName);
}

bool UFTPhysicsComponent::LoadProfile(const FString& Name, ftcore::RagdollProfile& Out)
{
	const FString File = DataDir() / TEXT("ragdoll") / (Name + TEXT(".json"));
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *File))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTPhysics: couldn't read %s."), *File);
		return false;
	}
	const FTCHARToUTF8 Utf8(*Text);
	std::string Error;
	if (!ftcore::importRagdollProfile(std::string_view(Utf8.Get(), static_cast<size_t>(Utf8.Length())), Out, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTPhysics: %s: %s"), *File, UTF8_TO_TCHAR(Error.c_str()));
		return false;
	}
	return true;
}

void UFTPhysicsComponent::BeginPlay()
{
	Super::BeginPlay();

	bProfileLoaded = LoadProfile(Profile.ToString(), RagdollProfile);

	if (!Target)
	{
		if (AActor* Owner = GetOwner())
		{
			if (USkeletalMeshComponent* Skel = Owner->FindComponentByClass<USkeletalMeshComponent>())
			{
				Target = Skel;
			}
			else
			{
				Target = Cast<UPrimitiveComponent>(Owner->GetRootComponent());
			}
		}
	}

	FrozenLinear.Reserve(kPhysReserveBodies);
	FrozenAngular.Reserve(kPhysReserveBodies);
	FrozenGravity.Reserve(kPhysReserveBodies);
	HitBodies.Reserve(kPhysReserveBodies);

	SweepParams = FCollisionQueryParams(SCENE_QUERY_STAT(FTPhysicsKnockback), false, GetOwner());
}

USkeletalMeshComponent* UFTPhysicsComponent::SkeletalTarget() const
{
	return Cast<USkeletalMeshComponent>(Target);
}

int32 UFTPhysicsComponent::NumBodies() const
{
	if (const USkeletalMeshComponent* Skel = SkeletalTarget())
	{
		return Skel->Bodies.Num();
	}
	return Target ? 1 : 0;
}

FBodyInstance* UFTPhysicsComponent::Body(int32 Index) const
{
	if (const USkeletalMeshComponent* Skel = SkeletalTarget())
	{
		return Skel->Bodies.IsValidIndex(Index) ? Skel->Bodies[Index] : nullptr;
	}
	return Target && Index == 0 ? Target->GetBodyInstance() : nullptr;
}

float UFTPhysicsComponent::Knockback(FVector ImpulseNs, float GroundFriction)
{
	if (!Target || !Target->IsSimulatingPhysics())
	{
		return 0.f;
	}
	const float Mass = Target->GetMass();
	const FVector Velocity = Target->GetPhysicsLinearVelocity() / ftcore::kPhysCmPerMetre;

	// Plan without a wall first to learn the direction and length of the slide, then sweep once along it.
	ftcore::KnockbackPlan Plan = ftcore::planKnockback(Mass, Velocity.X, Velocity.Y, ImpulseNs.X, ImpulseNs.Y, GroundFriction, -1.f);
	if (Plan.requestedDistance > 0.f && GetWorld())
	{
		// A sphere inside the body, so the floor it rests on isn't a hit; the wall distance is then corrected
		// for the body's half-extent along the slide.
		const FBoxSphereBounds Bounds = Target->Bounds;
		const FVector Extent = Bounds.BoxExtent;
		const float Radius = 0.5f * FMath::Min3(Extent.X, Extent.Y, Extent.Z);
		const float HalfAlong = FMath::Abs(Plan.dirX) * Extent.X + FMath::Abs(Plan.dirY) * Extent.Y;
		const FVector Dir(Plan.dirX, Plan.dirY, 0.f);
		const FVector Start = Bounds.Origin;
		const FVector End = Start + Dir * (ftcore::physMetresToCm(Plan.requestedDistance) + HalfAlong);
		FHitResult Hit;
		if (GetWorld()->SweepSingleByObjectType(Hit, Start, End, FQuat::Identity, FCollisionObjectQueryParams(ECC_WorldStatic),
				FCollisionShape::MakeSphere(Radius), SweepParams)
			&& !Hit.bStartPenetrating)
		{
			const float WallCm = FMath::Max(0.f, Hit.Distance + Radius - HalfAlong);
			Plan = ftcore::planKnockback(Mass, Velocity.X, Velocity.Y, ImpulseNs.X, ImpulseNs.Y, GroundFriction,
				WallCm / ftcore::kPhysCmPerMetre);
		}
	}
	Target->AddImpulse(FVector(ftcore::physImpulseToUnreal(Plan.impulseX), ftcore::physImpulseToUnreal(Plan.impulseY),
		ftcore::physImpulseToUnreal(ImpulseNs.Z)));
	return Plan.distance;
}

bool UFTPhysicsComponent::Knockout(FVector ImpulseNs, FName StruckBone)
{
	USkeletalMeshComponent* Skel = SkeletalTarget();
	if (!bProfileLoaded || RagdollProfile.useAuthoredKo || !Skel || !RagdollAsset)
	{
		return false;
	}
	if (Skel->GetPhysicsAsset() != RagdollAsset)
	{
		Skel->SetPhysicsAsset(RagdollAsset, true);
	}
	Skel->SetCollisionProfileName(TEXT("Ragdoll"));
	Skel->SetSimulatePhysics(true);
	Skel->WakeAllRigidBodies();
	bHitActive = false;

	// Event time, not a tick: size the frozen-frame buffers to this body count.
	const int32 Count = Skel->Bodies.Num();
	FrozenLinear.SetNumZeroed(Count);
	FrozenAngular.SetNumZeroed(Count);
	FrozenGravity.SetNumZeroed(Count);

	const float Impulse[3] = {static_cast<float>(ImpulseNs.X), static_cast<float>(ImpulseNs.Y), static_cast<float>(ImpulseNs.Z)};
	const float TotalMass = Skel->GetMass();
	const FBodyInstance* Struck = Skel->GetBodyInstance(StruckBone);
	const float PartMass = Struck ? Struck->GetBodyMass() : (Count > 0 ? TotalMass / Count : TotalMass);
	const ftcore::KoImpulseSplit Split = ftcore::splitKoImpulse(RagdollProfile, Impulse, TotalMass, PartMass);
	Skel->SetAllPhysicsLinearVelocity(FVector(ftcore::physMetresToCm(Split.wholeDeltaV[0]), ftcore::physMetresToCm(Split.wholeDeltaV[1]),
		ftcore::physMetresToCm(Split.wholeDeltaV[2])), true);
	if (Struck)
	{
		Skel->AddImpulse(PhysToUnrealImpulse(Split.localImpulse), StruckBone);
	}

	bKnockedOut = true;
	KoTime = 0.f;
	Settle = ftcore::SettleTracker();
	return true;
}

bool UFTPhysicsComponent::HitReact(FVector ImpulseNs, FName Bone)
{
	USkeletalMeshComponent* Skel = SkeletalTarget();
	if (!bProfileLoaded || bKnockedOut || !Skel || !RagdollAsset)
	{
		return false;
	}
	if (Skel->GetPhysicsAsset() != RagdollAsset)
	{
		Skel->SetPhysicsAsset(RagdollAsset, true);
	}
	// Which bodies blend is fixed for the reaction; find them once so the tick only writes weights.
	HitBodies.Reset();
	RagdollAsset->GetBodyIndicesBelow(HitBodies, Bone, Skel->GetSkeletalMeshAsset(), true);
	if (HitBodies.IsEmpty())
	{
		return false;
	}
	Skel->SetAllBodiesBelowSimulatePhysics(Bone, true, true);
	Skel->SetAllBodiesBelowPhysicsBlendWeight(Bone, 1.f, false, true);
	Skel->AddImpulse(ImpulseNs * (RagdollProfile.hitImpulseScale * ftcore::kPhysCmPerMetre), Bone);
	HitBone = Bone;
	HitTime = 0.f;
	bHitActive = true;
	return true;
}

void UFTPhysicsComponent::HitStop(int32 Frames)
{
	Stop.trigger(Frames);
}

void UFTPhysicsComponent::Freeze()
{
	const int32 Count = FMath::Min(NumBodies(), FrozenLinear.Max());
	FrozenLinear.SetNumUninitialized(Count, EAllowShrinking::No);
	FrozenAngular.SetNumUninitialized(Count, EAllowShrinking::No);
	FrozenGravity.SetNumUninitialized(Count, EAllowShrinking::No);
	for (int32 i = 0; i < Count; ++i)
	{
		FBodyInstance* B = Body(i);
		if (!B || !B->IsInstanceSimulatingPhysics())
		{
			FrozenGravity[i] = false;
			FrozenLinear[i] = FVector::ZeroVector;
			FrozenAngular[i] = FVector::ZeroVector;
			continue;
		}
		FrozenLinear[i] = B->GetUnrealWorldVelocity();
		FrozenAngular[i] = B->GetUnrealWorldAngularVelocityInRadians();
		FrozenGravity[i] = B->bEnableGravity;
		B->SetEnableGravity(false);
	}
	bFrozen = true;
}

void UFTPhysicsComponent::Thaw()
{
	for (int32 i = 0; i < FrozenLinear.Num(); ++i)
	{
		FBodyInstance* B = Body(i);
		if (!B || !B->IsInstanceSimulatingPhysics())
		{
			continue;
		}
		B->SetEnableGravity(FrozenGravity[i]);
		B->SetLinearVelocity(FrozenLinear[i], false);
		B->SetAngularVelocityInRadians(FrozenAngular[i], false);
	}
	bFrozen = false;
}

void UFTPhysicsComponent::CapSpeeds()
{
	if (!bProfileLoaded)
	{
		return;
	}
	const float Cap = ftcore::physMetresToCm(RagdollProfile.maxLinearVelocity);
	const int32 Count = NumBodies();
	for (int32 i = 0; i < Count; ++i)
	{
		FBodyInstance* B = Body(i);
		if (!B || !B->IsInstanceSimulatingPhysics() || !B->IsInstanceAwake())
		{
			continue;
		}
		const FVector V = B->GetUnrealWorldVelocity();
		const double Speed = V.Size();
		if (Speed > Cap)
		{
			B->SetLinearVelocity(V * (Cap / Speed), false);
		}
	}
}

void UFTPhysicsComponent::UpdateKo(float DeltaTime)
{
	KoTime += DeltaTime;
	float MaxLinear = 0.f;
	float MaxAngular = 0.f;
	bool bAnyAwake = false;
	const int32 Count = NumBodies();
	for (int32 i = 0; i < Count; ++i)
	{
		const FBodyInstance* B = Body(i);
		if (!B || !B->IsInstanceSimulatingPhysics() || !B->IsInstanceAwake())
		{
			continue;
		}
		bAnyAwake = true;
		MaxLinear = FMath::Max(MaxLinear, static_cast<float>(B->GetUnrealWorldVelocity().Size()) / ftcore::kPhysCmPerMetre);
		MaxAngular = FMath::Max(MaxAngular, static_cast<float>(B->GetUnrealWorldAngularVelocityInRadians().Size()));
	}

	// Going heavy: once the body has had time to fall and is slow, bleed off what's left.
	if (bAnyAwake && KoTime > RagdollProfile.koHeavyAfter && MaxLinear < RagdollProfile.koHeavyBelow)
	{
		const float Factor = ftcore::heavyDampingFactor(RagdollProfile.koHeavyRate, DeltaTime);
		for (int32 i = 0; i < Count; ++i)
		{
			FBodyInstance* B = Body(i);
			if (B && B->IsInstanceSimulatingPhysics() && B->IsInstanceAwake())
			{
				B->SetLinearVelocity(B->GetUnrealWorldVelocity() * Factor, false);
				B->SetAngularVelocityInRadians(B->GetUnrealWorldAngularVelocityInRadians() * Factor, false);
			}
		}
	}

	if (Settle.update(MaxLinear, MaxAngular, bAnyAwake, RagdollProfile.settleSpeed, RagdollProfile.settleAngularSpeed,
			RagdollProfile.settleTime, DeltaTime))
	{
		if (Target)
		{
			Target->PutAllRigidBodiesToSleep();
		}
		++SettleCount;
	}
}

void UFTPhysicsComponent::UpdateHitReaction(float DeltaTime)
{
	USkeletalMeshComponent* Skel = SkeletalTarget();
	if (!Skel)
	{
		bHitActive = false;
		return;
	}
	HitTime += DeltaTime;
	const float Weight = ftcore::hitReactionWeight(RagdollProfile, HitTime);
	for (const int32 Index : HitBodies)
	{
		if (Skel->Bodies.IsValidIndex(Index))
		{
			Skel->Bodies[Index]->PhysicsBlendWeight = Weight;
		}
	}
	if (Weight <= 0.f)
	{
		Skel->SetAllBodiesBelowSimulatePhysics(HitBone, false, true);
		bHitActive = false;
	}
}

void UFTPhysicsComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!Target)
	{
		return;
	}

	// Hit-stop: hold the bodies while frames are left, and let them go on the first free frame.
	if (Stop.consume())
	{
		if (!bFrozen)
		{
			Freeze();
		}
		for (int32 i = 0; i < FrozenLinear.Num(); ++i)
		{
			FBodyInstance* B = Body(i);
			if (B && B->IsInstanceSimulatingPhysics())
			{
				B->SetLinearVelocity(FVector::ZeroVector, false, false);
				B->SetAngularVelocityInRadians(FVector::ZeroVector, false, false);
			}
		}
		return;
	}
	if (bFrozen)
	{
		Thaw();
	}

	CapSpeeds();
	if (bKnockedOut)
	{
		UpdateKo(DeltaTime);
	}
	if (bHitActive)
	{
		UpdateHitReaction(DeltaTime);
	}
}
