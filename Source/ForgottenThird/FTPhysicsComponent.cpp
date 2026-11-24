#include "FTPhysicsComponent.h"

#include "ForgottenThird.h"

#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicalAnimationComponent.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Chaos/ChaosConstraintSettings.h"

#include <string>

namespace
{
// The most bodies a hit reaction or a frozen frame tracks without growing a buffer (the humanoid map has 12).
constexpr int32 kPhysReserveBodies = 64;

FVector PhysToUnrealImpulse(const float Ns[3])
{
	return FVector(ftcore::physImpulseToUnreal(Ns[0]), ftcore::physImpulseToUnreal(Ns[1]), ftcore::physImpulseToUnreal(Ns[2]));
}

// N*m to Unreal's kg*cm^2/s^2.
constexpr float kPhysTorqueToUnreal = 10000.f;

bool PartValid(const ftcore::ArenaHitSetup& Setup, int32 Part)
{
	return Part >= 0 && static_cast<std::size_t>(Part) < Setup.partCount;
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
	if (!bLeadSetup)
	{
		Skel->SetCollisionProfileName(TEXT("Ragdoll"));
	}
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

bool UFTPhysicsComponent::SetupForLead(USkeletalMeshComponent* Skin, UPhysicsAsset* Ragdoll)
{
	ClearLeadSetup();
	USkeletalMesh* Mesh = Skin ? Skin->GetSkeletalMeshAsset() : nullptr;
	if (!Mesh || !Ragdoll)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTPhysics: no lead set-up on %s: %s."), *GetNameSafe(GetOwner()),
			Mesh ? TEXT("no ragdoll asset") : TEXT("no skeletal mesh"));
		return false;
	}
	if (!bProfileLoaded)
	{
		bProfileLoaded = LoadProfile(Profile.ToString(), RagdollProfile);
		if (!bProfileLoaded)
		{
			return false;
		}
	}

	const FString MapFile = DataDir() / TEXT("ragdoll") / TEXT("humanoid_map.json");
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *MapFile))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTPhysics: couldn't read %s."), *MapFile);
		return false;
	}
	ftcore::RagdollMap Map;
	std::string Error;
	{
		const FTCHARToUTF8 Utf8(*Text);
		if (!ftcore::importRagdollMap(std::string_view(Utf8.Get(), static_cast<size_t>(Utf8.Length())), Map, &Error))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTPhysics: %s: %s"), *MapFile, UTF8_TO_TCHAR(Error.c_str()));
			return false;
		}
	}

	const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
	std::vector<std::string> BoneNames;
	BoneNames.reserve(static_cast<size_t>(Ref.GetNum()));
	for (int32 i = 0; i < Ref.GetNum(); ++i)
	{
		const FTCHARToUTF8 Bone(*Ref.GetBoneName(i).ToString());
		BoneNames.emplace_back(Bone.Get(), static_cast<size_t>(Bone.Length()));
	}
	ftcore::RagdollBinding Binding;
	if (!ftcore::bindRagdoll(Map, BoneNames, Binding, &Error))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTPhysics: %s does not fit the humanoid map: %s"), *Mesh->GetName(),
			UTF8_TO_TCHAR(Error.c_str()));
		return false;
	}
	ftcore::ArenaHitSetup Setup;
	if (!ftcore::arenaHitSetup(Map, RagdollProfile, Setup))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTPhysics: the humanoid map gives no hit setup."));
		return false;
	}

	// The bodies at or under each part's bone, found once: a blow then only reads them. Body indices are the asset's
	// (the skin's Bodies follow its SkeletalBodySetups).
	const int32 PartCount = static_cast<int32>(Setup.partCount);
	PartBones.Init(NAME_None, PartCount);
	PartBodyStart.Init(0, PartCount);
	PartBodyCount.Init(0, PartCount);
	PartBodies.Reset();
	TArray<int32> Below;
	for (const ftcore::RagdollBoundPart& Bound : Binding.parts)
	{
		if (Bound.part < 0 || Bound.part >= PartCount || Bound.bone < 0 || Bound.bone >= Ref.GetNum())
		{
			continue;
		}
		const FName Bone = Ref.GetBoneName(Bound.bone);
		Below.Reset();
		Ragdoll->GetBodyIndicesBelow(Below, Bone, Mesh, true);
		PartBones[Bound.part] = Bone;
		PartBodyStart[Bound.part] = PartBodies.Num();
		PartBodyCount[Bound.part] = Below.Num();
		PartBodies.Append(Below);
	}
	if (!PartValid(Setup, Setup.root) || PartBodyCount[Setup.root] == 0)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTPhysics: %s has no body under the root part's bone."), *Ragdoll->GetName());
		ClearLeadSetup();
		return false;
	}

	// The skin's collision as the look left it, given back by ClearLeadSetup. The bodies need a physics state: physics
	// only (no queries, no overlaps) and blocking the static world alone, so a simulated part lands on the floor and
	// meets walls but never pushes a figure. The simulation never moves the skin component: the figure stays where Core
	// puts it, and a ragdoll is drawn by its bones alone.
	Target = Skin;
	RagdollAsset = Ragdoll;
	SavedCollision = Skin->GetCollisionEnabled();
	SavedObjectType = Skin->GetCollisionObjectType();
	SavedResponses = Skin->GetCollisionResponseToChannels();
	SavedUpdateMode = static_cast<uint8>(Skin->PhysicsTransformUpdateMode);
	bSkinSaved = true;
	Skin->PhysicsTransformUpdateMode = EPhysicsTransformUpdateMode::ComponentTransformIsKinematic;
	Skin->SetCollisionObjectType(ECC_PhysicsBody);
	Skin->SetCollisionResponseToAllChannels(ECR_Ignore);
	Skin->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Skin->SetCollisionEnabled(ECollisionEnabled::PhysicsOnly);
	if (Skin->GetPhysicsAsset() != Ragdoll)
	{
		Skin->SetPhysicsAsset(Ragdoll, true);
	}
	if (!Skin->IsPhysicsStateCreated())
	{
		Skin->RecreatePhysicsState();
	}
	const int32 BodyCount = Ragdoll->SkeletalBodySetups.Num();
	if (!Skin->IsPhysicsStateCreated() || Skin->Bodies.Num() != BodyCount)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTPhysics: %s: the skin made %d of %s's %d bodies; no lead set-up."),
			*GetNameSafe(GetOwner()), Skin->Bodies.Num(), *Ragdoll->GetName(), BodyCount);
		ClearLeadSetup();
		return false;
	}

	for (const int32 Index : PartBodies)
	{
		if (!Skin->Bodies.IsValidIndex(Index) || !Skin->Bodies[Index] || !Skin->Bodies[Index]->IsValidBodyInstance())
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTPhysics: %s: body %d of %s was not made on the skin; no lead set-up."),
				*GetNameSafe(GetOwner()), Index, *Ragdoll->GetName());
			ClearLeadSetup();
			return false;
		}
	}

	// Sized now so a blow, a hit-stop or a knockout never grows them.
	const int32 Reserve = FMath::Max(kPhysReserveBodies, BodyCount);
	HitBodies.Reserve(Reserve);
	FrozenLinear.Reserve(Reserve);
	FrozenAngular.Reserve(Reserve);
	FrozenGravity.Reserve(Reserve);

	// The motor pulls a simulated part toward the clip (ftcore::ragdollDriveStrengths, a mass-free spring). Chaos
	// scales the physical-animation drive by its angular drive scales, so they are divided out here. The drive acts
	// only on simulated bodies: the kinematic rest of the body is untouched.
	DriveStrengths = ftcore::ragdollDriveStrengths(RagdollProfile.motor);
	const float StiffnessScale = static_cast<float>(Chaos::ConstraintSettings::AngularDriveStiffnessScale());
	const float DampingScale = static_cast<float>(Chaos::ConstraintSettings::AngularDriveDampingScale());
	if (!Drive)
	{
		Drive = NewObject<UPhysicalAnimationComponent>(GetOwner(), TEXT("FTPhysicsDrive"));
		Drive->RegisterComponent();
	}
	Drive->SetSkeletalMeshComponent(Skin);
	FPhysicalAnimationData Data;
	Data.bIsLocalSimulation = true;
	Data.OrientationStrength = StiffnessScale > 0.f ? DriveStrengths.orientation / StiffnessScale : DriveStrengths.orientation;
	Data.AngularVelocityStrength = DampingScale > 0.f ? DriveStrengths.angularVelocity / DampingScale : DriveStrengths.angularVelocity;
	Data.PositionStrength = 0.f;
	Data.VelocityStrength = 0.f;
	Data.MaxLinearForce = 0.f;
	Data.MaxAngularForce = DriveStrengths.maxTorque * kPhysTorqueToUnreal;
	Drive->ApplyPhysicalAnimationSettingsBelow(PartBones[Setup.root], Data, true);
	SetDrive(1.f);
	SetDriveTicking(false); // on only while a part simulates: idle leads cost nothing

	HitSetup = Setup;
	bLeadSetup = true;
	UE_LOG(LogForgottenThird, Log, TEXT("FTPhysics: %s set up with %s: %d parts, %d bodies, drive %.0f / %.1f."),
		*GetNameSafe(GetOwner()), *Ragdoll->GetName(), PartCount, BodyCount, DriveStrengths.orientation,
		DriveStrengths.angularVelocity);
	return true;
}

void UFTPhysicsComponent::ClearLeadSetup()
{
	if (bFrozen)
	{
		Thaw();
	}
	Stop = ftcore::HitStop();
	USkeletalMeshComponent* Skel = SkeletalTarget();
	if (Drive)
	{
		Drive->SetSkeletalMeshComponent(nullptr); // its constraints go before the bodies they hold
		SetDriveTicking(false);
	}
	if (Skel && (bHitActive || bKnockedOut || bRecovering))
	{
		Skel->SetAllBodiesPhysicsBlendWeight(0.f);
		Skel->SetSimulatePhysics(false);
	}
	if (Skel && bSkinSaved)
	{
		Skel->SetCollisionEnabled(SavedCollision);
		Skel->SetCollisionObjectType(SavedObjectType);
		Skel->SetCollisionResponseToChannels(SavedResponses);
		Skel->PhysicsTransformUpdateMode = static_cast<EPhysicsTransformUpdateMode::Type>(SavedUpdateMode);
		if (RagdollAsset && Skel->GetPhysicsAsset() == RagdollAsset)
		{
			Skel->SetPhysicsAsset(nullptr, false);
		}
	}
	if (bSkinSaved)
	{
		RagdollAsset = nullptr; // the lead's, set by SetupForLead
	}
	bSkinSaved = false;
	bHitActive = false;
	bKnockedOut = false;
	bRecovering = false;
	bLeadSetup = false;
	HitBodies.Reset();
	PartBones.Reset();
	PartBodyStart.Reset();
	PartBodyCount.Reset();
	PartBodies.Reset();
	HitSetup = ftcore::ArenaHitSetup();
}

FName UFTPhysicsComponent::PartBone(int32 Part) const
{
	return PartBones.IsValidIndex(Part) ? PartBones[Part] : NAME_None;
}

void UFTPhysicsComponent::SetDriveTicking(bool bTick)
{
	// The drive builds its constraints and moves its targets in its own tick, so it runs only while a body simulates.
	if (Drive && Drive->IsComponentTickEnabled() != bTick)
	{
		Drive->SetComponentTickEnabled(bTick);
	}
}

void UFTPhysicsComponent::SetDrive(float Strength)
{
	if (Drive)
	{
		Drive->SetStrengthMultiplyer(Strength);
	}
}

bool UFTPhysicsComponent::React(const ftcore::ArenaHitReaction& Reaction, const FVector& WorldDir)
{
	if (!bLeadSetup || Reaction.tier == ftcore::ArenaHitTier::None || bKnockedOut)
	{
		return false;
	}
	// A blow inside a hit-stop: the held velocities go back first, so the new push isn't zeroed by the hold; the new
	// stop holds them again from the next frame.
	if (bFrozen)
	{
		Thaw();
	}
	FVector Dir(WorldDir.X, WorldDir.Y, 0.f);
	if (!Dir.Normalize())
	{
		Dir = FVector::ForwardVector;
	}
	const FVector ImpulseNs = Dir * FMath::Max(Reaction.strength, 0.f);
	const int32 Root = HitSetup.root;
	const int32 Spine = PartValid(HitSetup, HitSetup.spine) && PartBodyCount[HitSetup.spine] > 0 ? HitSetup.spine : Root;
	const int32 Chest = PartValid(HitSetup, HitSetup.chest) && PartBodyCount[HitSetup.chest] > 0 ? HitSetup.chest : Spine;

	bool bDone = false;
	switch (Reaction.tier)
	{
	case ftcore::ArenaHitTier::Light:
	{
		// The struck part gives. A blow on the root (the pelvis) moves the upper body: the pelvis and the legs under
		// it never simulate together while the lead can be controlled.
		int32 Part = PartValid(HitSetup, Reaction.part) && PartBodyCount[Reaction.part] > 0 ? Reaction.part : Chest;
		if (Part == Root)
		{
			Part = Spine;
		}
		bDone = Part != Root && StartHit(Part, PartBones[Part], ImpulseNs);
		break;
	}
	case ftcore::ArenaHitTier::Stagger:
		// The body's weight thrown: the upper body from the spine, pushed at the chest.
		bDone = Spine != Root && StartHit(Spine, PartBones[Chest], ImpulseNs);
		break;
	case ftcore::ArenaHitTier::Knockout:
	{
		EndHit();
		const bool bWasRecovering = bRecovering;
		bRecovering = false;
		const int32 Part = PartValid(HitSetup, Reaction.part) && PartBodyCount[Reaction.part] > 0 ? Reaction.part : Chest;
		SetDrive(0.f);
		bDone = Knockout(ImpulseNs, PartBones[Part]);
		if (bDone)
		{
			if (USkeletalMeshComponent* Skel = SkeletalTarget())
			{
				Skel->SetAllBodiesPhysicsBlendWeight(1.f); // drawn wholly by the ragdoll
			}
			SetDriveTicking(true); // its targets keep up through the knockout, ready for the recovery
		}
		else
		{
			bRecovering = bWasRecovering;
			SetDrive(bWasRecovering ? 0.f : 1.f);
		}
		break;
	}
	default:
		break;
	}
	if (bDone && Reaction.hitStopFrames > 0)
	{
		HitStop(Reaction.hitStopFrames);
	}
	return bDone;
}

void UFTPhysicsComponent::SetHitBodiesSimulating(bool bSimulate)
{
	USkeletalMeshComponent* Skel = SkeletalTarget();
	if (!Skel)
	{
		return;
	}
	for (const int32 Index : HitBodies)
	{
		if (Skel->Bodies.IsValidIndex(Index) && Skel->Bodies[Index])
		{
			Skel->Bodies[Index]->SetInstanceSimulatePhysics(bSimulate, false, true);
		}
	}
	// Once more on the set's top body through the component, which registers (or drops) its end-of-physics tick.
	Skel->SetBodySimulatePhysics(HitBone, bSimulate);
}

bool UFTPhysicsComponent::StartHit(int32 Part, FName PushBone, const FVector& ImpulseNs)
{
	USkeletalMeshComponent* Skel = SkeletalTarget();
	if (!Skel || bRecovering || !PartBodyCount.IsValidIndex(Part) || PartBodyCount[Part] == 0 || PartBones[Part].IsNone())
	{
		return false;
	}
	const int32* PartFirst = &PartBodies[PartBodyStart[Part]];
	const int32 PartCount = PartBodyCount[Part];
	for (int32 i = 0; i < PartCount; ++i)
	{
		if (!Skel->Bodies.IsValidIndex(PartFirst[i]) || !Skel->Bodies[PartFirst[i]] ||
			!Skel->Bodies[PartFirst[i]]->IsValidBodyInstance())
		{
			return false; // the skin's bodies aren't the set-up's (its physics state was remade)
		}
	}
	// A reaction under way takes the new part's bodies in with its own (the union: no part snaps back to the clip), and
	// the whole set holds and fades again from this blow. HitBodies was sized to the body count, so it never grows.
	if (!bHitActive)
	{
		HitBodies.Reset();
		HitBone = PartBones[Part];
	}
	const int32 Before = HitBodies.Num();
	for (int32 i = 0; i < PartCount; ++i)
	{
		if (!HitBodies.Contains(PartFirst[i]))
		{
			HitBodies.Add(PartFirst[i]);
			Skel->Bodies[PartFirst[i]]->SetInstanceSimulatePhysics(true, false, true);
		}
	}
	if (Before == 0)
	{
		Skel->SetBodySimulatePhysics(HitBone, true); // registers the skin's end-of-physics tick
	}
	if (Drive && !Drive->IsComponentTickEnabled())
	{
		// The drive's targets last moved when it last ticked (an earlier reaction): set them to the last evaluated pose now,
		// before the first physics step, so no part is pulled toward an old pose. Its tick then keeps them up.
		SetDriveTicking(true);
		Drive->TickComponent(0.f, LEVELTICK_All, nullptr);
	}
	for (const int32 Index : HitBodies)
	{
		Skel->Bodies[Index]->PhysicsBlendWeight = 1.f;
	}
	const FName At = PushBone.IsNone() ? PartBones[Part] : PushBone;
	Skel->AddImpulse(ImpulseNs * (RagdollProfile.hitImpulseScale * ftcore::kPhysCmPerMetre), At);
	HitTime = 0.f;
	bHitActive = true;
	return true;
}

void UFTPhysicsComponent::EndHit()
{
	if (!bHitActive)
	{
		return;
	}
	if (USkeletalMeshComponent* Skel = SkeletalTarget())
	{
		for (const int32 Index : HitBodies)
		{
			if (Skel->Bodies.IsValidIndex(Index) && Skel->Bodies[Index])
			{
				Skel->Bodies[Index]->PhysicsBlendWeight = 0.f;
			}
		}
		SetHitBodiesSimulating(false);
	}
	bHitActive = false;
	if (!bKnockedOut && !bRecovering)
	{
		SetDriveTicking(false);
	}
}

void UFTPhysicsComponent::Recover(bool bInstant)
{
	if (bFrozen)
	{
		Thaw();
	}
	Stop = ftcore::HitStop();
	EndHit();
	USkeletalMeshComponent* Skel = SkeletalTarget();
	if (bRecovering && bInstant)
	{
		FinishRecover();
		return;
	}
	if (!bKnockedOut || !Skel)
	{
		return;
	}
	const FName Root = bLeadSetup ? PartBone(HitSetup.root) : NAME_None;
	const FVector Place = Skel->GetComponentLocation();
	const FVector Pelvis = Root.IsNone() ? Place : Skel->GetBoneLocation(Root);
	const FVector Gap = Pelvis - Place;
	UE_LOG(LogForgottenThird, Display,
		TEXT("FTPhysics: %s back from the knockout %s (no get-up clip yet; the pelvis lay %.0f cm from the skin's place, "
			 "%.0f cm across)."),
		*GetNameSafe(GetOwner()), bInstant ? TEXT("at once") : TEXT("by a timed blend"), Gap.Size(),
		FVector(Gap.X, Gap.Y, 0.f).Size());
	bKnockedOut = false;
	if (bInstant)
	{
		FinishRecover();
		return;
	}
	bRecovering = true;
	RecoverTime = 0.f;
	Skel->WakeAllRigidBodies();
}

void UFTPhysicsComponent::FinishRecover()
{
	if (USkeletalMeshComponent* Skel = SkeletalTarget())
	{
		Skel->SetAllBodiesPhysicsBlendWeight(0.f);
		Skel->SetSimulatePhysics(false);
		if (!bLeadSetup)
		{
			Skel->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		}
	}
	SetDrive(1.f);
	bRecovering = false;
	if (!bHitActive)
	{
		SetDriveTicking(false);
	}
}

void UFTPhysicsComponent::UpdateRecover(float DeltaTime)
{
	USkeletalMeshComponent* Skel = SkeletalTarget();
	RecoverTime += DeltaTime;
	const float T = RecoverSeconds > 0.f ? FMath::Clamp(RecoverTime / RecoverSeconds, 0.f, 1.f) : 1.f;
	if (Skel && T < 1.f)
	{
		Skel->SetAllBodiesPhysicsBlendWeight(1.f - FMath::SmoothStep(0.f, 1.f, T));
		return;
	}
	FinishRecover();
}

void UFTPhysicsComponent::Freeze()
{
	// Every body's gravity is kept, simulated or not, so Thaw can give each back whatever started or stopped simulating
	// meanwhile.
	const int32 Count = FMath::Min(NumBodies(), FrozenLinear.Max());
	FrozenLinear.SetNumUninitialized(Count, EAllowShrinking::No);
	FrozenAngular.SetNumUninitialized(Count, EAllowShrinking::No);
	FrozenGravity.SetNumUninitialized(Count, EAllowShrinking::No);
	for (int32 i = 0; i < Count; ++i)
	{
		FBodyInstance* B = Body(i);
		FrozenLinear[i] = FVector::ZeroVector;
		FrozenAngular[i] = FVector::ZeroVector;
		FrozenGravity[i] = B ? B->bEnableGravity : false;
		if (!B || !B->IsInstanceSimulatingPhysics())
		{
			continue;
		}
		FrozenLinear[i] = B->GetUnrealWorldVelocity();
		FrozenAngular[i] = B->GetUnrealWorldAngularVelocityInRadians();
		B->SetEnableGravity(false);
	}
	bFrozen = true;
}

void UFTPhysicsComponent::Thaw()
{
	for (int32 i = 0; i < FrozenLinear.Num(); ++i)
	{
		FBodyInstance* B = Body(i);
		if (!B)
		{
			continue;
		}
		if (B->bEnableGravity != FrozenGravity[i])
		{
			B->SetEnableGravity(FrozenGravity[i]);
		}
		if (B->IsInstanceSimulatingPhysics())
		{
			B->SetLinearVelocity(FrozenLinear[i], false);
			B->SetAngularVelocityInRadians(FrozenAngular[i], false);
		}
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
		if (bLeadSetup)
		{
			EndHit(); // the kept indices, no lookup
		}
		else
		{
			Skel->SetAllBodiesBelowSimulatePhysics(HitBone, false, true);
			bHitActive = false;
		}
	}
}

void UFTPhysicsComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!Target)
	{
		return;
	}
	// A lead standing untouched: nothing simulates, so nothing to hold, cap or fade.
	if (bLeadSetup && !bHitActive && !bKnockedOut && !bRecovering && !bFrozen && !Stop.frozen())
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
	if (bRecovering)
	{
		UpdateRecover(DeltaTime);
	}
	if (bHitActive)
	{
		UpdateHitReaction(DeltaTime);
	}
}
