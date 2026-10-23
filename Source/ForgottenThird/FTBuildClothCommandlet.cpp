#include "FTBuildClothCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "ClothingAsset.h"
#include "ClothingAssetFactory.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PointWeightMap.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "SkeletalMeshClothingSystemUtilities.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#endif

#if WITH_EDITOR
namespace
{
void ClothReport(USkeletalMesh& Mesh)
{
	for (UClothingAssetBase* Base : Mesh.GetMeshClothingAssets())
	{
		const UClothingAssetCommon* Asset = Cast<UClothingAssetCommon>(Base);
		if (!Asset || Asset->LodData.Num() == 0)
		{
			continue;
		}
		const FClothPhysicalMeshData& Phys = Asset->LodData[0].PhysicalMeshData;
		const FPointWeightMap* Max = Phys.FindWeightMap(EWeightMapTargetCommon::MaxDistance);
		int32 Free = 0;
		float Top = 0.f;
		for (int32 i = 0; Max && i < Max->Num(); ++i)
		{
			Free += (*Max)[i] > 0.1f;
			Top = FMath::Max(Top, (*Max)[i]);
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildCloth: %s: cloth %s, %d sim vertices (%d free, max distance %.1f cm), physics %s."),
			*Mesh.GetName(), *Asset->GetName(), Phys.Vertices.Num(), Free, Top,
			Asset->PhysicsAsset ? *Asset->PhysicsAsset->GetName() : TEXT("none"));
	}
	if (Mesh.GetMeshClothingAssets().Num() == 0)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildCloth: %s has no cloth."), *Mesh.GetName());
	}
}

// Unbinds every clothed section of LOD 0 and drops the clothing assets, so a rerun starts clean.
void ClothStrip(USkeletalMesh& Mesh)
{
	FSkeletalMeshModel* Model = Mesh.GetImportedModel();
	if (Model && Model->LODModels.Num() > 0)
	{
		for (int32 Section = 0; Section < Model->LODModels[0].Sections.Num(); ++Section)
		{
			if (Model->LODModels[0].Sections[Section].HasClothingData())
			{
				FSkeletalMeshClothingSystemUtilities::RemoveClothingFromSection(&Mesh, 0, Section, nullptr);
			}
		}
	}
	for (UClothingAssetBase* Old : Mesh.GetMeshClothingAssets())
	{
		if (Old) // out of the mesh's package, so it isn't saved and its name is free for the new cloth
		{
			Old->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
		}
	}
	Mesh.GetMeshClothingAssets().Empty();
}

float ClothSmooth(float X)
{
	const float T = FMath::Clamp(X, 0.f, 1.f);
	return T * T * (3.f - 2.f * T);
}

// What the cloth can collide with: only spheres and capsules on bones the clothed mesh has are used.
void ClothCheckCollision(const USkeletalMesh& Mesh, const UPhysicsAsset& Physics)
{
	const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();
	int32 Round = 0, OnMesh = 0;
	TSet<FName> Covered;
	for (const USkeletalBodySetup* Setup : Physics.SkeletalBodySetups)
	{
		const int32 Shapes = Setup ? Setup->AggGeom.SphereElems.Num() + Setup->AggGeom.SphylElems.Num() : 0;
		Round += Shapes > 0;
		if (Shapes > 0 && Skeleton.FindBoneIndex(Setup->BoneName) != INDEX_NONE)
		{
			++OnMesh;
			Covered.Add(Setup->BoneName);
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildCloth: %s: %d bodies, %d with spheres or capsules, %d of those on bones %s has."),
		*Physics.GetName(), Physics.SkeletalBodySetups.Num(), Round, OnMesh, *Mesh.GetName());
	for (const TCHAR* Bone : {TEXT("pelvis"), TEXT("thigh_l"), TEXT("thigh_r"), TEXT("calf_l"), TEXT("calf_r")})
	{
		if (!Covered.Contains(Bone))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildCloth: no sphere or capsule on %s; the cloth will pass through it."), Bone);
		}
	}
	for (const USkeletalBodySetup* Setup : Physics.SkeletalBodySetups)
	{
		for (const FKSphylElem& Capsule : Setup ? Setup->AggGeom.SphylElems : TArray<FKSphylElem>())
		{
			UE_LOG(LogForgottenThird, Display, TEXT("FTBuildCloth: capsule on %s, radius %.1f cm, length %.1f cm."),
				*Setup->BoneName.ToString(), Capsule.Radius, Capsule.Length);
		}
	}
}

// Copies a physics asset and shrinks each capsule to the body it covers: each of the body's vertices (in the reference
// pose) goes to the capsule whose surface is nearest, the capsule's axis moves to the middle of its vertices and the
// radius becomes the Fit quantile of their distances from it, plus Pad cm, never larger than it was. Physics assets made
// for a ragdoll take every vertex a bone touches, so a thigh capsule can grow to hip width and push a skirt out in two
// lobes. Grouping by nearness rather than by skin weights keeps the left and right sides alike even when an
// auto-skinned body's weights aren't (one thigh taking the seat, a twist bone taking most of a limb).
UPhysicsAsset* ClothFitPhysics(const UPhysicsAsset& Source, const USkeletalMesh& Body, const FString& OutPath, float Fit, float Pad,
	const TArray<FString>& Drop)
{
	const FSkeletalMeshModel* Model = Body.GetImportedModel();
	if (!Model || Model->LODModels.Num() == 0)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildCloth: %s has no LOD 0 to fit capsules to."), *Body.GetName());
		return nullptr;
	}
	const FReferenceSkeleton& Skeleton = Body.GetRefSkeleton();
	TArray<FTransform> Pose = Skeleton.GetRefBonePose();
	for (int32 i = 0; i < Pose.Num(); ++i)
	{
		const int32 Parent = Skeleton.GetParentIndex(i);
		if (Parent != INDEX_NONE)
		{
			Pose[i] = Pose[i] * Pose[Parent];
		}
	}

	const FString PackageName = FPackageName::ObjectPathToPackageName(OutPath);
	UPackage* Package = CreatePackage(*PackageName);
	UPhysicsAsset* Out = DuplicateObject<UPhysicsAsset>(&Source, Package, *FPackageName::GetShortName(PackageName));
	Out->SetFlags(RF_Public | RF_Standalone);

	// Bodies whose bone name contains a -Drop word lose their shapes: hands hang against a skirt at rest and would
	// push it out in puffs.
	for (USkeletalBodySetup* Setup : Out->SkeletalBodySetups)
	{
		const FString Name = Setup ? Setup->BoneName.ToString() : FString();
		if (Drop.ContainsByPredicate([&Name](const FString& Word) { return Name.Contains(Word); }))
		{
			UE_LOG(LogForgottenThird, Display, TEXT("FTBuildCloth: dropped the shapes of %s."), *Name);
			Setup->AggGeom.EmptyElements();
			Setup->InvalidatePhysicsData();
		}
	}

	struct FFit
	{
		USkeletalBodySetup* Setup;
		FKSphylElem* Capsule;
		FTransform ToCapsule;
		TArray<FVector2D> Around;
	};
	TArray<FFit> Fits;
	for (USkeletalBodySetup* Setup : Out->SkeletalBodySetups)
	{
		const int32 Bone = Setup ? Skeleton.FindBoneIndex(Setup->BoneName) : INDEX_NONE;
		if (Bone == INDEX_NONE)
		{
			continue;
		}
		for (FKSphylElem& Capsule : Setup->AggGeom.SphylElems)
		{
			Fits.Add({Setup, &Capsule, Capsule.GetTransform() * Pose[Bone], {}});
		}
	}
	for (const FSkelMeshSection& Section : Model->LODModels[0].Sections)
	{
		for (const FSoftSkinVertex& Vertex : Section.SoftVertices)
		{
			FFit* Nearest = nullptr;
			float NearestGap = TNumericLimits<float>::Max();
			FVector2D NearestLocal(0, 0);
			bool bBesideAxis = false;
			for (FFit& Candidate : Fits)
			{
				const FVector Local = Candidate.ToCapsule.InverseTransformPosition(FVector(Vertex.Position));
				const float Along = FMath::Max(0.0f, float(FMath::Abs(Local.Z)) - Candidate.Capsule->Length / 2);
				const float Gap = FMath::Sqrt(float(Local.X * Local.X + Local.Y * Local.Y) + Along * Along) - Candidate.Capsule->Radius;
				if (Gap < NearestGap)
				{
					Nearest = &Candidate;
					NearestGap = Gap;
					NearestLocal = FVector2D(Local.X, Local.Y);
					bBesideAxis = Along == 0;
				}
			}
			if (Nearest && bBesideAxis)  // a vertex past a cap doesn't size the radius
			{
				Nearest->Around.Add(NearestLocal);
			}
		}
	}
	for (FFit& F : Fits)
	{
		if (F.Around.Num() < 8)
		{
			continue;
		}
		FVector2D Mid(0, 0);
		for (const FVector2D& P : F.Around)
		{
			Mid += P;
		}
		Mid /= F.Around.Num();
		TArray<float> Distances;
		for (const FVector2D& P : F.Around)
		{
			Distances.Add((P - Mid).Size());
		}
		Distances.Sort();
		const float Radius = FMath::Min(F.Capsule->Radius, Distances[FMath::Clamp(FMath::FloorToInt(Fit * Distances.Num()), 0, Distances.Num() - 1)] + Pad);
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildCloth: fitted capsule on %s from %d vertices, axis moved %.1f cm, radius %.1f -> %.1f cm."),
			*F.Setup->BoneName.ToString(), Distances.Num(), Mid.Size(), F.Capsule->Radius, Radius);
		F.Capsule->Center += F.Capsule->Rotation.RotateVector(FVector(Mid.X, Mid.Y, 0));
		F.Capsule->Radius = Radius;
		F.Setup->InvalidatePhysicsData();
	}
	// The body is symmetric, so each _l/_r pair takes the mean of its two radii.
	for (USkeletalBodySetup* Left : Out->SkeletalBodySetups)
	{
		const FString Name = Left ? Left->BoneName.ToString() : FString();
		const int32 Index = Name.EndsWith(TEXT("_l")) ? Out->FindBodyIndex(FName(Name.LeftChop(2) + TEXT("_r"))) : INDEX_NONE;
		USkeletalBodySetup* Right = Index != INDEX_NONE ? Out->SkeletalBodySetups[Index].Get() : nullptr;
		if (!Right || Left->AggGeom.SphylElems.Num() != 1 || Right->AggGeom.SphylElems.Num() != 1)
		{
			continue;
		}
		float& A = Left->AggGeom.SphylElems[0].Radius;
		float& B = Right->AggGeom.SphylElems[0].Radius;
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildCloth: %s pair radius %.1f / %.1f -> %.1f cm."), *Name.LeftChop(2), A, B, (A + B) / 2);
		A = B = (A + B) / 2;
	}
	return Out;
}

// Max distance by height: 0 in the top Pin cm, easing up to Max at the lowest vertex.
void ClothPaint(UClothingAssetCommon& Asset, float Max, float Pin)
{
	FClothLODDataCommon& Lod = Asset.LodData[0];
	const TArray<FVector3f>& Verts = Lod.PhysicalMeshData.Vertices;
	float Top = -UE_BIG_NUMBER, Bottom = UE_BIG_NUMBER;
	for (const FVector3f& V : Verts)
	{
		Top = FMath::Max(Top, V.Z);
		Bottom = FMath::Min(Bottom, V.Z);
	}
	const float Span = FMath::Max(Top - Bottom - Pin, 1.f);
	FPointWeightMap* Map = Lod.PointWeightMaps.FindByPredicate(
		[](const FPointWeightMap& M) { return M.CurrentTarget == static_cast<uint8>(EWeightMapTargetCommon::MaxDistance); });
	if (!Map)
	{
		Map = &Lod.PointWeightMaps.AddDefaulted_GetRef();
		Map->CurrentTarget = static_cast<uint8>(EWeightMapTargetCommon::MaxDistance);
	}
	Map->Name = TEXT("MaxDistance");
	Map->bEnabled = true;
	Map->Values.SetNum(Verts.Num());
	for (int32 i = 0; i < Verts.Num(); ++i)
	{
		Map->Values[i] = Max * ClothSmooth((Top - Verts[i].Z - Pin) / Span);
	}
	Asset.ApplyParameterMasks(false, true);
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildCloth: sim mesh %.1f cm tall, pinned %.1f cm, max distance %.1f cm at the hem."),
		Top - Bottom, Pin, Max);
}

bool SavePackageOf(UObject& Asset)
{
	UPackage* Package = Asset.GetOutermost();
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Package, &Asset, *File, Args))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildCloth: couldn't save %s."), *File);
		return false;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildCloth: saved %s."), *File);
	return true;
}

// A material drawn on a clothed section needs the clothing usage, or the engine swaps in its default (grey) material.
bool ClothMaterials(const USkeletalMesh& Mesh)
{
	bool bOk = true;
	for (const FSkeletalMaterial& Slot : Mesh.GetMaterials())
	{
		UMaterial* Base = Slot.MaterialInterface ? Slot.MaterialInterface->GetMaterial() : nullptr;
		if (!Base || Base->GetUsageByFlag(MATUSAGE_Clothing))
		{
			continue;
		}
		Base->PreEditChange(nullptr);
		Base->SetUsageByFlag(MATUSAGE_Clothing, true);
		Base->PostEditChange();
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildCloth: %s now allows clothing."), *Base->GetName());
		bOk &= SavePackageOf(*Base);
	}
	return bOk;
}

bool ClothSave(USkeletalMesh& Mesh)
{
	UPackage* Package = Mesh.GetOutermost();
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Package, &Mesh, *File, Args))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildCloth: couldn't save %s."), *File);
		return false;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildCloth: saved %s."), *File);
	return true;
}
}
#endif

UFTBuildClothCommandlet::UFTBuildClothCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildClothCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString MeshPath, PhysicsPath;
	float Max = 25.f, Pin = 3.f;
	FParse::Value(*Params, TEXT("Mesh="), MeshPath);
	FParse::Value(*Params, TEXT("Physics="), PhysicsPath);
	FParse::Value(*Params, TEXT("Max="), Max);
	FParse::Value(*Params, TEXT("Pin="), Pin);
	FString FitPath, BodyPath;
	if (FParse::Value(*Params, TEXT("FitPhysics="), FitPath))
	{
		float Fit = 0.7f, Pad = 1.f;
		FParse::Value(*Params, TEXT("Body="), BodyPath);
		FParse::Value(*Params, TEXT("Fit="), Fit);
		FParse::Value(*Params, TEXT("Pad="), Pad);
		FString DropList;
		TArray<FString> Drop;
		FParse::Value(*Params, TEXT("Drop="), DropList, false);  // the list itself has commas
		DropList.ParseIntoArray(Drop, TEXT(","));
		const UPhysicsAsset* Source = PhysicsPath.IsEmpty() ? nullptr : LoadObject<UPhysicsAsset>(nullptr, *PhysicsPath);
		const USkeletalMesh* Body = BodyPath.IsEmpty() ? nullptr : LoadObject<USkeletalMesh>(nullptr, *BodyPath);
		if (!Source || !Body)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildCloth: -FitPhysics needs -Physics=<physics asset> and -Body=<skeletal mesh>."));
			return 1;
		}
		UPhysicsAsset* Fitted = ClothFitPhysics(*Source, *Body, FitPath, Fit, Pad, Drop);
		return Fitted && SavePackageOf(*Fitted) ? 0 : 1;
	}
	USkeletalMesh* Mesh = MeshPath.IsEmpty() ? nullptr : LoadObject<USkeletalMesh>(nullptr, *MeshPath);
	if (!Mesh)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildCloth: no skeletal mesh at -Mesh=%s."), *MeshPath);
		return 1;
	}
	if (FParse::Param(*Params, TEXT("Verify")))
	{
		ClothReport(*Mesh);
		for (const FSkeletalMaterial& Slot : Mesh->GetMaterials())
		{
			const UMaterial* Base = Slot.MaterialInterface ? Slot.MaterialInterface->GetMaterial() : nullptr;
			UE_LOG(LogForgottenThird, Display, TEXT("FTBuildCloth: material %s, clothing usage %s."),
				Base ? *Base->GetName() : TEXT("none"), Base && Base->GetUsageByFlag(MATUSAGE_Clothing) ? TEXT("on") : TEXT("off"));
		}
		return 0;
	}
	UPhysicsAsset* Physics = nullptr;
	if (!PhysicsPath.IsEmpty() && !(Physics = LoadObject<UPhysicsAsset>(nullptr, *PhysicsPath)))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildCloth: no physics asset at -Physics=%s."), *PhysicsPath);
		return 1;
	}

	if (Physics)
	{
		ClothCheckCollision(*Mesh, *Physics);
	}
	ClothStrip(*Mesh);
	FSkeletalMeshClothBuildParams Build;
	Build.AssetName = Mesh->GetName() + TEXT("_Cloth");
	Build.LodIndex = 0;
	Build.SourceSection = 0;
	Build.bRemoveFromMesh = false;
	Build.PhysicsAsset = Physics;
	UClothingAssetFactory* Factory = NewObject<UClothingAssetFactory>();
	UClothingAssetCommon* Asset = Cast<UClothingAssetCommon>(Factory->CreateFromSkeletalMesh(Mesh, Build));
	if (!Asset || Asset->LodData.Num() == 0)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildCloth: couldn't make cloth from %s LOD 0 section 0."), *Mesh->GetName());
		return 1;
	}
	ClothPaint(*Asset, Max, Pin);
	Mesh->AddClothingAsset(Asset);
	FString Error;
	if (!FSkeletalMeshClothingSystemUtilities::AssignClothingToSection(Mesh, Asset, 0, 0, 0, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildCloth: couldn't bind the cloth to %s: %s"), *Mesh->GetName(), *Error);
		return 1;
	}
	ClothReport(*Mesh);
	const bool bMaterialsOk = ClothMaterials(*Mesh);
	return ClothSave(*Mesh) && bMaterialsOk ? 0 : 1;
#else
	return 1;
#endif
}
