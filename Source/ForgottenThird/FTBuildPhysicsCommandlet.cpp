#include "FTBuildPhysicsCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "FTPhysicsComponent.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "ftcore/Physics.h"

#include <string>
#include <vector>
#endif

#if WITH_EDITOR
namespace
{
const TCHAR* const kPhysDefaultMesh = TEXT("/Game/Characters/ProofHead/proof_head/SkeletalMeshes/proof_head.proof_head");

FString PhysPackageFile(const FString& Package)
{
	return FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
}

FString PhysPackageOf(const FString& ObjectPath)
{
	FString Package, Object;
	return ObjectPath.Split(TEXT("."), &Package, &Object) ? Package : ObjectPath;
}

bool PhysRemoveFile(const FString& File)
{
	if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildPhysics: couldn't delete %s."), *File);
		return false;
	}
	return true;
}

bool PhysSave(UPackage* Package, UObject* Asset)
{
	FAssetRegistryModule::AssetCreated(Asset);
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	const FString File = PhysPackageFile(Package->GetName());
	if (!UPackage::SavePackage(Package, Asset, *File, Args))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildPhysics: couldn't save %s."), *File);
		return false;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildPhysics: saved %s."), *File);
	return true;
}

bool PhysReadData(const FString& File, std::string& Out)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *File))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildPhysics: couldn't read %s."), *File);
		return false;
	}
	const FTCHARToUTF8 Utf8(*Text);
	Out.assign(Utf8.Get(), static_cast<size_t>(Utf8.Length()));
	return true;
}

FString PhysStr(const std::string& Text)
{
	return UTF8_TO_TCHAR(Text.c_str());
}

// ---- physical materials ------------------------------------------------------------------------------------

EFrictionCombineMode::Type PhysCombineMode(ftcore::PhysCombine Combine)
{
	switch (Combine)
	{
	case ftcore::PhysCombine::Min: return EFrictionCombineMode::Min;
	case ftcore::PhysCombine::Multiply: return EFrictionCombineMode::Multiply;
	case ftcore::PhysCombine::Max: return EFrictionCombineMode::Max;
	default: return EFrictionCombineMode::Average;
	}
}

void PhysFillMaterial(UPhysicalMaterial& Out, const ftcore::PhysMaterial& In)
{
	Out.Friction = In.friction;
	Out.StaticFriction = In.effectiveStaticFriction();
	Out.Restitution = In.restitution;
	Out.Density = ftcore::physDensityToUnreal(In.density);
	Out.bOverrideFrictionCombineMode = In.frictionCombine != ftcore::PhysCombine::Default;
	Out.FrictionCombineMode = PhysCombineMode(In.frictionCombine);
	Out.bOverrideRestitutionCombineMode = In.restitutionCombine != ftcore::PhysCombine::Default;
	Out.RestitutionCombineMode = PhysCombineMode(In.restitutionCombine);
	// ftcore::PhysSurface's order is the SurfaceType index (Config/DefaultEngine.ini names them).
	Out.SurfaceType = static_cast<EPhysicalSurface>(static_cast<int32>(In.surface));
}

// Mismatches between an asset and its data, as text; empty when they agree.
FString PhysCompareMaterial(const UPhysicalMaterial& Asset, const ftcore::PhysMaterial& In)
{
	FString Out;
	auto Near = [&Out](const TCHAR* What, float Have, float Want) {
		if (!FMath::IsNearlyEqual(Have, Want, 1e-4f))
		{
			Out += FString::Printf(TEXT(" %s %g (data %g)"), What, Have, Want);
		}
	};
	Near(TEXT("Friction"), Asset.Friction, In.friction);
	Near(TEXT("StaticFriction"), Asset.StaticFriction, In.effectiveStaticFriction());
	Near(TEXT("Restitution"), Asset.Restitution, In.restitution);
	Near(TEXT("Density"), Asset.Density, ftcore::physDensityToUnreal(In.density));
	if (Asset.SurfaceType != static_cast<EPhysicalSurface>(static_cast<int32>(In.surface)))
	{
		Out += FString::Printf(TEXT(" SurfaceType %d (data %d)"), static_cast<int32>(Asset.SurfaceType), static_cast<int32>(In.surface));
	}
	return Out;
}

// ---- ragdoll -----------------------------------------------------------------------------------------------

// Component-space reference pose of every bone.
void PhysRefPose(const FReferenceSkeleton& Skeleton, TArray<FTransform>& Out)
{
	const TArray<FTransform>& Local = Skeleton.GetRefBonePose();
	Out.SetNum(Local.Num());
	for (int32 Bone = 0; Bone < Local.Num(); ++Bone)
	{
		const int32 Parent = Skeleton.GetParentIndex(Bone);
		Out[Bone] = Parent == INDEX_NONE ? Local[Bone] : Local[Bone] * Out[Parent];
	}
}

// The character's left, from the right thigh to the left one (then the upper arms); +Y when neither is there.
FVector PhysLateral(const FReferenceSkeleton& Skeleton, const TArray<FTransform>& Pose)
{
	const TCHAR* const Pairs[][2] = {{TEXT("thigh_r"), TEXT("thigh_l")}, {TEXT("upperarm_r"), TEXT("upperarm_l")}};
	for (const auto& Pair : Pairs)
	{
		const int32 R = Skeleton.FindBoneIndex(Pair[0]);
		const int32 L = Skeleton.FindBoneIndex(Pair[1]);
		if (R != INDEX_NONE && L != INDEX_NONE)
		{
			const FVector Side = Pose[L].GetLocation() - Pose[R].GetLocation();
			if (!Side.IsNearlyZero())
			{
				return Side.GetSafeNormal();
			}
		}
	}
	return FVector::YAxisVector;
}

// The joint frame in component space: X along the bone, Z as close to the lateral axis as X allows (the
// newengine boneFrame, with Unreal's twist axis X in place of newengine's Y).
FQuat PhysJointFrame(const FVector& Dir, const FVector& Lateral)
{
	const FVector X = Dir.GetSafeNormal();
	FVector Z = Lateral - FVector::DotProduct(Lateral, X) * X;
	if (Z.SizeSquared() < 1e-6)
	{
		const FVector Alt = FMath::Abs(X.Z) < 0.9 ? FVector::ZAxisVector : FVector::XAxisVector;
		Z = Alt - FVector::DotProduct(Alt, X) * X;
	}
	return FRotationMatrix::MakeFromXZ(X, Z.GetSafeNormal()).ToQuat();
}

struct FPhysPartGeometry
{
	FVector Joint = FVector::ZeroVector; // component space, cm
	FVector Tip = FVector::ZeroVector;
};

void PhysApplyLimits(FConstraintInstance& Joint, const ftcore::RagdollChaosLimits& Limits)
{
	auto Motion = [](float HalfRange) { return HalfRange > 0.01f ? ACM_Limited : ACM_Locked; };
	Joint.SetAngularSwing1Limit(Motion(Limits.swing1Deg), Limits.swing1Deg);
	if (Limits.hinge)
	{
		Joint.SetAngularSwing2Limit(ACM_Locked, 0.f);
		Joint.SetAngularTwistLimit(ACM_Locked, 0.f);
	}
	else
	{
		Joint.SetAngularSwing2Limit(Motion(Limits.swing2Deg), Limits.swing2Deg);
		Joint.SetAngularTwistLimit(Motion(Limits.twistDeg), Limits.twistDeg);
	}
	Joint.SetLinearLimits(LCM_Locked, LCM_Locked, LCM_Locked, 0.f);
	Joint.SetDisableCollision(true);
}

UPhysicsAsset* PhysBuildRagdoll(UPackage* Package, const FString& AssetName, USkeletalMesh& Mesh, const ftcore::RagdollMap& Map,
	const ftcore::RagdollProfile& Profile, UPhysicalMaterial* Material, int32& OutConstraints)
{
	const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();
	std::vector<std::string> BoneNames;
	BoneNames.reserve(static_cast<size_t>(Skeleton.GetNum()));
	for (int32 Bone = 0; Bone < Skeleton.GetNum(); ++Bone)
	{
		BoneNames.emplace_back(TCHAR_TO_UTF8(*Skeleton.GetBoneName(Bone).ToString()));
	}
	ftcore::RagdollBinding Binding;
	std::string Error;
	if (!ftcore::bindRagdoll(Map, BoneNames, Binding, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildPhysics: can't bind the %s map to %s: %s"), *PhysStr(Map.name), *Mesh.GetName(),
			*PhysStr(Error));
		return nullptr;
	}
	for (const std::string& Dropped : Binding.dropped)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildPhysics: optional part %s isn't on %s; dropped."), *PhysStr(Dropped), *Mesh.GetName());
	}

	TArray<FTransform> Pose;
	PhysRefPose(Skeleton, Pose);
	const FVector Lateral = PhysLateral(Skeleton, Pose);

	// Capsule ends, as newengine places them: joint to tip bone, or `length` along the parent-to-joint line.
	TArray<FPhysPartGeometry> Geometry;
	Geometry.SetNum(static_cast<int32>(Binding.parts.size()));
	for (int32 i = 0; i < Geometry.Num(); ++i)
	{
		const ftcore::RagdollBoundPart& Part = Binding.parts[static_cast<size_t>(i)];
		const ftcore::RagdollPartDef& Def = Map.parts[static_cast<size_t>(Part.part)];
		FPhysPartGeometry& G = Geometry[i];
		G.Joint = Pose[Part.bone].GetLocation();
		bool bHaveTip = false;
		if (Part.tipBone >= 0)
		{
			G.Tip = Pose[Part.tipBone].GetLocation();
			bHaveTip = FVector::Dist(G.Tip, G.Joint) > 0.01;
		}
		if (!bHaveTip)
		{
			FVector Dir = FVector::ZAxisVector;
			if (Part.parent >= 0)
			{
				const FVector Up = G.Joint - Geometry[Part.parent].Joint;
				if (!Up.IsNearlyZero())
				{
					Dir = Up.GetSafeNormal();
				}
			}
			G.Tip = G.Joint + Dir * ftcore::physMetresToCm(Def.length);
		}
	}

	UPhysicsAsset* Asset = NewObject<UPhysicsAsset>(Package, *AssetName, RF_Public | RF_Standalone);
#if WITH_EDITORONLY_DATA
	Asset->PreviewSkeletalMesh = &Mesh;
#endif

	const uint8 PositionIterations = static_cast<uint8>(FMath::Clamp(Profile.positionSteps, 1, 255));
	const uint8 VelocityIterations = static_cast<uint8>(FMath::Clamp(Profile.velocitySteps, 1, 255));
	for (int32 i = 0; i < Geometry.Num(); ++i)
	{
		const ftcore::RagdollBoundPart& Part = Binding.parts[static_cast<size_t>(i)];
		const ftcore::RagdollPartDef& Def = Map.parts[static_cast<size_t>(Part.part)];
		const FTransform& BonePose = Pose[Part.bone];
		const FPhysPartGeometry& G = Geometry[i];

		USkeletalBodySetup* Setup = NewObject<USkeletalBodySetup>(Asset, NAME_None, RF_Transactional);
		Setup->BoneName = Skeleton.GetBoneName(Part.bone);
		Setup->PhysicsType = PhysType_Default;
		Setup->CollisionTraceFlag = CTF_UseSimpleAsComplex;
		Setup->PhysMaterial = Material;

		// FKSphylElem runs along its local Z; Length is the cylinder between the caps.
		const float Radius = ftcore::physMetresToCm(Def.radius);
		const FVector Axis = BonePose.InverseTransformVectorNoScale(G.Tip - G.Joint);
		const float Span = static_cast<float>(Axis.Size());
		FKSphylElem Capsule(Radius, FMath::Max(Span - 2.f * Radius, 2.f));
		Capsule.Center = BonePose.InverseTransformPosition(0.5 * (G.Joint + G.Tip));
		Capsule.Rotation = FRotationMatrix::MakeFromZ(Axis.GetSafeNormal()).Rotator();
		Capsule.SetName(Setup->BoneName);
		Setup->AggGeom.SphylElems.Add(Capsule);

		FBodyInstance& Body = Setup->DefaultInstance;
		Body.SetMassOverride(Part.mass, true);
		Body.LinearDamping = Profile.linearDamping;
		Body.AngularDamping = Profile.angularDamping;
		Body.InertiaTensorScale = FVector(Profile.inertiaScale);
		Body.bOverrideMaxAngularVelocity = true;
		Body.MaxAngularVelocity = ftcore::physRadToDeg(Profile.maxAngularVelocity);
		Body.SetOverrideIterationCounts(true);
		Body.PositionSolverIterationCount = PositionIterations;
		Body.VelocitySolverIterationCount = VelocityIterations;

		Asset->SkeletalBodySetups.Add(Setup);
	}
	Asset->UpdateBodySetupIndexMap();
	Asset->UpdateBoundsBodiesArray();

	OutConstraints = 0;
	for (int32 i = 0; i < Geometry.Num(); ++i)
	{
		const ftcore::RagdollBoundPart& Part = Binding.parts[static_cast<size_t>(i)];
		if (Part.parent < 0)
		{
			continue;
		}
		const ftcore::RagdollBoundPart& ParentPart = Binding.parts[static_cast<size_t>(Part.parent)];
		const ftcore::RagdollPartDef& Def = Map.parts[static_cast<size_t>(Part.part)];
		const FName Child = Skeleton.GetBoneName(Part.bone);
		const FName Parent = Skeleton.GetBoneName(ParentPart.bone);

		UPhysicsConstraintTemplate* Template = NewObject<UPhysicsConstraintTemplate>(Asset, NAME_None, RF_Transactional);
		FConstraintInstance& Joint = Template->DefaultInstance;
		Joint.JointName = Child;
		Joint.ConstraintBone1 = Child;
		Joint.ConstraintBone2 = Parent;

		const ftcore::RagdollChaosLimits Limits = ftcore::ragdollChaosLimits(Profile.limitsFor(Def.name));
		PhysApplyLimits(Joint, Limits);

		// Frames: the child's sits on the joint, the parent's is the same frame turned by the range's centre, so
		// Chaos' symmetric limits cover the data's asymmetric range (AngularRotationOffset isn't used by Chaos).
		const FTransform JointPose(PhysJointFrame(Geometry[i].Tip - Geometry[i].Joint, Lateral), Geometry[i].Joint);
		const FQuat Centre = FQuat(FVector::ZAxisVector, FMath::DegreesToRadians(Limits.swing1OffsetDeg))
			* FQuat(FVector::XAxisVector, FMath::DegreesToRadians(Limits.twistOffsetDeg));
		const FTransform ParentJointPose = FTransform(Centre) * JointPose;
		Joint.SetRefFrame(EConstraintFrame::Frame1, JointPose.GetRelativeTransform(Pose[Part.bone]));
		Joint.SetRefFrame(EConstraintFrame::Frame2, ParentJointPose.GetRelativeTransform(Pose[ParentPart.bone]));

		Template->SetDefaultProfile(Joint);
		Asset->ConstraintSetup.Add(Template);
		++OutConstraints;
	}

	for (const std::pair<int, int>& Pair : Binding.noCollide)
	{
		const FName A = Skeleton.GetBoneName(Binding.parts[static_cast<size_t>(Pair.first)].bone);
		const FName B = Skeleton.GetBoneName(Binding.parts[static_cast<size_t>(Pair.second)].bone);
		Asset->DisableCollision(Asset->FindBodyIndex(A), Asset->FindBodyIndex(B));
	}

	for (USkeletalBodySetup* Setup : Asset->SkeletalBodySetups)
	{
		Setup->InvalidatePhysicsData();
		Setup->CreatePhysicsMeshes();
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildPhysics: %s: %d bodies, %d joints, %d no-collide pairs, %.1f kg, lateral (%.2f %.2f %.2f)."),
		*AssetName, Asset->SkeletalBodySetups.Num(), OutConstraints, static_cast<int32>(Binding.noCollide.size()), Map.totalMass,
		Lateral.X, Lateral.Y, Lateral.Z);
	return Asset;
}
}
#endif

UFTBuildPhysicsCommandlet::UFTBuildPhysicsCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildPhysicsCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	const bool bVerify = FParse::Param(*Params, TEXT("Verify"));
	const FString DataDir = UFTPhysicsComponent::DataDir();
	int32 Errors = 0;

	// Materials.
	std::string Text;
	if (!PhysReadData(DataDir / TEXT("materials.json"), Text))
	{
		return 1;
	}
	ftcore::PhysMaterialSet Materials;
	std::string Error;
	std::vector<std::string> Warnings;
	if (!ftcore::importPhysMaterials(Text, Materials, &Error, &Warnings))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildPhysics: materials.json:\n%s"), *PhysStr(Error));
		return 1;
	}
	for (const std::string& Warning : Warnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildPhysics: materials.json: %s"), *PhysStr(Warning));
	}

	UPhysicalMaterial* RagdollMaterial = nullptr;
	for (const ftcore::PhysMaterial& Data : Materials.materials)
	{
		const FString ObjectPath = UFTPhysicsComponent::MaterialPath(PhysStr(Data.name));
		const FString PackageName = PhysPackageOf(ObjectPath);
		const FString AssetName = FPackageName::GetShortName(PackageName);
		UPhysicalMaterial* Asset = nullptr;
		if (bVerify)
		{
			Asset = LoadObject<UPhysicalMaterial>(nullptr, *ObjectPath);
			if (!Asset)
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildPhysics: -Verify: %s is missing."), *ObjectPath);
				++Errors;
				continue;
			}
			const FString Diff = PhysCompareMaterial(*Asset, Data);
			if (!Diff.IsEmpty())
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildPhysics: -Verify: %s differs:%s"), *ObjectPath, *Diff);
				++Errors;
			}
		}
		else
		{
			if (!PhysRemoveFile(PhysPackageFile(PackageName)))
			{
				return 1;
			}
			UPackage* Package = CreatePackage(*PackageName);
			Asset = NewObject<UPhysicalMaterial>(Package, *AssetName, RF_Public | RF_Standalone);
			PhysFillMaterial(*Asset, Data);
			if (!PhysSave(Package, Asset))
			{
				return 1;
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildPhysics: %s: friction %.2f/%.2f, restitution %.2f, density %.2f g/cm3, surface %s."),
			*AssetName, Asset->Friction, Asset->StaticFriction, Asset->Restitution, Asset->Density,
			*PhysStr(std::string(ftcore::physSurfaceName(Data.surface))));
		if (Data.name == "ragdoll")
		{
			RagdollMaterial = Asset;
		}
	}

	if (FParse::Param(*Params, TEXT("NoRagdoll")))
	{
		return Errors == 0 ? 0 : 1;
	}

	// Ragdoll.
	FString ProfileName = TEXT("gentle");
	FParse::Value(*Params, TEXT("Profile="), ProfileName);
	FString MeshPath = kPhysDefaultMesh;
	FParse::Value(*Params, TEXT("Mesh="), MeshPath);

	ftcore::RagdollProfile Profile;
	if (!UFTPhysicsComponent::LoadProfile(ProfileName, Profile))
	{
		return 1;
	}
	if (!PhysReadData(DataDir / TEXT("ragdoll/humanoid_map.json"), Text))
	{
		return 1;
	}
	ftcore::RagdollMap Map;
	if (!ftcore::importRagdollMap(Text, Map, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildPhysics: humanoid_map.json: %s"), *PhysStr(Error));
		return 1;
	}
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
	if (!Mesh)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildPhysics: couldn't load skeletal mesh %s."), *MeshPath);
		return 1;
	}
	if (Profile.useAuthoredKo)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildPhysics: profile %s uses an authored KO; the asset is built but KO won't ragdoll."),
			*ProfileName);
	}
	if (!FMath::IsNearlyEqual(Profile.friction, RagdollMaterial ? RagdollMaterial->Friction : Profile.friction, 1e-3f))
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildPhysics: the bodies use PM_Ragdoll (friction %.2f); profile %s asks for %.2f."),
			RagdollMaterial->Friction, *ProfileName, Profile.friction);
	}

	const FString PackageName = UFTPhysicsComponent::RagdollPackage(Mesh->GetName());
	const FString AssetName = FPackageName::GetShortName(PackageName);
	if (bVerify)
	{
		const UPhysicsAsset* Asset = LoadObject<UPhysicsAsset>(nullptr, *(PackageName + TEXT(".") + AssetName));
		if (!Asset)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildPhysics: -Verify: %s is missing."), *PackageName);
			return 1;
		}
		float Mass = 0.f;
		for (const USkeletalBodySetup* Setup : Asset->SkeletalBodySetups)
		{
			Mass += Setup->DefaultInstance.GetMassOverride();
			if (Mesh->GetRefSkeleton().FindBoneIndex(Setup->BoneName) == INDEX_NONE)
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildPhysics: -Verify: body %s isn't a bone of %s."), *Setup->BoneName.ToString(),
					*Mesh->GetName());
				++Errors;
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildPhysics: -Verify: %s has %d bodies (%.1f kg) and %d joints; the mesh keeps %s."),
			*AssetName, Asset->SkeletalBodySetups.Num(), Mass, Asset->ConstraintSetup.Num(),
			Mesh->GetPhysicsAsset() ? *Mesh->GetPhysicsAsset()->GetName() : TEXT("no physics asset"));
		if (Asset->SkeletalBodySetups.Num() < 2 || Asset->ConstraintSetup.Num() != Asset->SkeletalBodySetups.Num() - 1)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildPhysics: -Verify: expected one joint per non-root body."));
			++Errors;
		}
		return Errors == 0 ? 0 : 1;
	}

	if (!PhysRemoveFile(PhysPackageFile(PackageName)))
	{
		return 1;
	}
	UPackage* Package = CreatePackage(*PackageName);
	int32 Constraints = 0;
	UPhysicsAsset* Asset = PhysBuildRagdoll(Package, AssetName, *Mesh, Map, Profile, RagdollMaterial, Constraints);
	if (!Asset || !PhysSave(Package, Asset))
	{
		return 1;
	}
	return Errors == 0 ? 0 : 1;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildPhysics needs an editor build."));
	return 1;
#endif
}
