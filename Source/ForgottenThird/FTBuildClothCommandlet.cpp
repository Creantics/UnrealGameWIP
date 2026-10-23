#include "FTBuildClothCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "ClothingAsset.h"
#include "ClothingAssetFactory.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PointWeightMap.h"
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
	USkeletalMesh* Mesh = MeshPath.IsEmpty() ? nullptr : LoadObject<USkeletalMesh>(nullptr, *MeshPath);
	if (!Mesh)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildCloth: no skeletal mesh at -Mesh=%s."), *MeshPath);
		return 1;
	}
	if (FParse::Param(*Params, TEXT("Verify")))
	{
		ClothReport(*Mesh);
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
	return ClothSave(*Mesh) ? 0 : 1;
#else
	return 1;
#endif
}
