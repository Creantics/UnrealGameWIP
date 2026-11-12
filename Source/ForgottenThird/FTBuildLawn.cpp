#include "FTBuildLawn.h"

#include "ForgottenThird.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "ImageUtils.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "ftcore/LawnSheet.h"
#include "ftcore/MassingPlan.h"

#include <string>

namespace FTLawn::Impl
{
constexpr double kStepM = 1.0;        // the sheet's grid and the paths' stations, as the height field's own spacing
constexpr double kLawnLiftM = 0.05;   // the sheet over the ground: clear of the ground's own triangulation
constexpr double kPathLiftM = 0.09;   // a path over the ground (4 cm proud of the lawn)
constexpr double kPathStaggerM = 0.004; // each later path this much higher, so crossing paths never fight
constexpr double kSkirtM = 0.10;      // a path's skirt reaches this far under the ground
const FName kSlot(TEXT("Lawn"));

struct FMap
{
	const TCHAR* File;
	const TCHAR* Suffix;
	bool bSRGB;
	TextureCompressionSettings Compression;
};
constexpr FMap kMaps[] = {
	{TEXT("lawn_D.jpg"), TEXT("D"), true, TC_Default},
	{TEXT("lawn_N.jpg"), TEXT("N"), false, TC_Normalmap},
	{TEXT("lawn_ARM.jpg"), TEXT("ARM"), false, TC_Masks},
};

UTexture2D* ImportMap(const FString& Dir, const FMap& Map)
{
	const FString Path = FString::Printf(TEXT("%s/T_Lawn_%s"), TextureFolder, Map.Suffix);
	if (UTexture2D* Have = LoadObject<UTexture2D>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)), nullptr, LOAD_NoWarn | LOAD_Quiet))
	{
		return Have;
	}
	const FString File = Dir / Map.File;
	TArray<uint8> Bytes;
	FImage Image;
	if (!FFileHelper::LoadFileToArray(Bytes, *File) || !FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), Image) ||
		(Image.Format != ERawImageFormat::BGRA8 && Image.Format != ERawImageFormat::G8))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -LawnTex: couldn't read %s as an 8-bit JPEG."), *File);
		return nullptr;
	}
	UPackage* Package = CreatePackage(*Path);
	UTexture2D* Texture = NewObject<UTexture2D>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone);
	Texture->PreEditChange(nullptr);
	Texture->Source.InitWithCompressedSourceData(Image.SizeX, Image.SizeY, 1, Image.Format == ERawImageFormat::G8 ? TSF_G8 : TSF_BGRA8,
		TArrayView64<uint8>(Bytes.GetData(), Bytes.Num()), ETextureSourceCompressionFormat::TSCF_JPEG);
	Texture->SRGB = Map.bSRGB;
	Texture->CompressionSettings = Map.Compression;
	Texture->LODGroup = TEXTUREGROUP_World;
	// A world projection doesn't use the mesh's UVs, which the streamer judges a texture's need by: load it whole.
	Texture->NeverStream = true;
	Texture->PostEditChange();
	FAssetRegistryModule::AssetCreated(Texture);
	FSavePackageArgs Save;
	Save.TopLevelFlags = RF_Public | RF_Standalone;
	const FString Out = FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Package, Texture, *Out, Save))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -LawnTex: couldn't save %s."), *Out);
		return nullptr;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: -LawnTex: imported %s (%d x %d)."), *Path, Image.SizeX, Image.SizeY);
	return Texture;
}

UStaticMesh* SheetMesh(UPackage* Package, const FString& Name, const ftcore::LawnMesh& Source, double Side, const FVector& Pivot,
	UMaterialInterface* Material)
{
	UStaticMesh* Mesh = NewObject<UStaticMesh>(Package, *Name, RF_Public);
	FMeshDescription Desc;
	FStaticMeshAttributes Attributes(Desc);
	Attributes.Register();
	const FPolygonGroupID Group = Desc.CreatePolygonGroup();
	Attributes.GetPolygonGroupMaterialSlotNames()[Group] = kSlot;
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector2f> Uvs = Attributes.GetVertexInstanceUVs();
	TArray<FVertexID> Verts;
	TArray<FVector> Points;
	Verts.Reserve(static_cast<int32>(Source.v.size()));
	Points.Reserve(static_cast<int32>(Source.v.size()));
	for (const ftcore::MassingVec& V : Source.v)
	{
		const ftcore::MassingVec E = ftcore::massingEnginePoint(Side, V.x, V.y, V.z);
		Points.Add(FVector(E.x, E.y, E.z) * 100.0);
		const FVertexID Id = Desc.CreateVertex();
		Positions[Id] = FVector3f(Points.Last() - Pivot);
		Verts.Add(Id);
	}
	// The plan frame's counter-clockwise turns clockwise in the engine's (its y runs the other way), the winding the
	// engine draws a face up by; check it on the first upward triangle all the same and turn every one if it's wrong.
	bool bFlip = false;
	for (const auto& T : Source.tris)
	{
		const FVector N = FVector::CrossProduct(Points[static_cast<int32>(T[1])] - Points[static_cast<int32>(T[0])], Points[static_cast<int32>(T[2])] - Points[static_cast<int32>(T[0])]);
		if (FMath::Abs(N.Z) > 0.5 * N.Size())
		{
			bFlip = N.Z > 0.0; // the engine wants an up face's first cross product pointing down
			break;
		}
	}
	for (const auto& T : Source.tris)
	{
		FVertexInstanceID Corners[3];
		for (int32 K = 0; K < 3; ++K)
		{
			const int32 Index = static_cast<int32>(T[static_cast<size_t>(bFlip ? 2 - K : K)]);
			Corners[K] = Desc.CreateVertexInstance(Verts[Index]);
			Uvs[Corners[K]] = FVector2f(FVector2D(Points[Index].X, Points[Index].Y) / 100.0);
		}
		Desc.CreatePolygon(Group, TArrayView<const FVertexInstanceID>(Corners, 3));
	}
	Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, kSlot, kSlot));
	FStaticMeshSourceModel& Model = Mesh->AddSourceModel();
	Model.BuildSettings.bRecomputeNormals = true;
	Model.BuildSettings.bRecomputeTangents = true;
	Model.BuildSettings.bGenerateLightmapUVs = false;
	Mesh->CreateMeshDescription(0, MoveTemp(Desc));
	Mesh->CommitMeshDescription(0);
	Mesh->CreateBodySetup();
	Mesh->GetBodySetup()->CollisionTraceFlag = CTF_UseComplexAsSimple;
	Mesh->Build(true);
	Mesh->PostEditChange();
	return Mesh;
}

void SpawnSheet(UWorld* World, const FActorSpawnParameters& Params, UStaticMesh* Mesh, const FVector& Pivot, const FString& Label)
{
	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Pivot, FRotator::ZeroRotator, Params);
	Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	Actor->SetActorLabel(Label);
	Actor->Tags.Add(TEXT("FTLawn"));
}

FVector PivotOf(const ftcore::LawnMesh& M, double Side)
{
	FBox Box(ForceInit);
	for (const ftcore::MassingVec& V : M.v)
	{
		const ftcore::MassingVec E = ftcore::massingEnginePoint(Side, V.x, V.y, V.z);
		Box += FVector(E.x, E.y, E.z) * 100.0;
	}
	return FVector(Box.GetCenter().X, Box.GetCenter().Y, Box.Min.Z);
}
} // namespace FTLawn::Impl

UMaterialInterface* FTLawn::LawnMaterial(UPackage* Package, UMaterialInterface* Parent)
{
	using namespace FTLawn::Impl;
	FString Dir;
	UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, TEXT("MI_Massing_Lawn"), RF_Public);
	Mic->SetParentEditorOnly(Parent);
	if (FParse::Value(FCommandLine::Get(), TEXT("LawnTex="), Dir, false) && !Dir.IsEmpty())
	{
		static const TCHAR* const Params[] = {TEXT("AlbedoTex"), TEXT("NormalTex"), TEXT("ArmTex")};
		for (int32 M = 0; M < UE_ARRAY_COUNT(kMaps); ++M)
		{
			UTexture2D* Texture = ImportMap(Dir, kMaps[M]);
			if (!Texture)
			{
				return nullptr;
			}
			Mic->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Params[M]), Texture);
		}
		// The scan is 2 m a side and an olive green: pulled to a mid lawn green, with a broad mottle in brightness so a
		// green of 80 m reads as patches, not one tile repeated.
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("TileCm")), 200.f);
		Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Color")), FLinearColor(0.74f, 1.45f, 0.78f));
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("NormalStrength")), 1.f);
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Variation")), 0.14f);
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("VariationScale")), 420.f);
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("MacroVariation")), 0.22f);
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("MacroScale")), 1700.f);
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: the lawn wears the scanned set from -LawnTex (%s)."), TextureFolder);
	}
	else
	{
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("MacroVariation")), 0.32f);
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("MacroScale")), 1400.f);
		Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Color")), FLinearColor(0.8f, 1.2f, 0.7f));
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: the lawn wears the project's grass surface (no -LawnTex)."));
	}
	// Grass soaks rain up: no standing puddles or wet sheen on the lawn (the surface master's per-instance Weather).
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Weather")), 0.f);
	Mic->PostEditChange();
	return Mic;
}

bool FTLawn::Build(UWorld* World, const FActorSpawnParameters& Spawn, UPackage* Package, const ftcore::HeightField& Ground, double Side,
	UMaterialInterface* Lawn, UMaterialInterface* Path)
{
	using namespace FTLawn::Impl;
	FString File;
	if (!FParse::Value(FCommandLine::Get(), TEXT("Lawn="), File, false) || File.IsEmpty())
	{
		return true;
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *File))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -Lawn couldn't read '%s'."), *File);
		return false;
	}
	ftcore::LawnTable Table;
	std::string Error;
	if (!ftcore::parseLawnTable(std::string_view(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num())), Table, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -Lawn %s: %s"), *File, UTF8_TO_TCHAR(Error.c_str()));
		return false;
	}
	if (!Lawn || !Path || !Ground.grid)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -Lawn needs -Surfaces (the lawn and paving surfaces) and the height field."));
		return false;
	}
	int64 Triangles = 0;
	for (int32 R = 0; R < static_cast<int32>(Table.regions.size()); ++R)
	{
		ftcore::LawnMesh M;
		ftcore::lawnSheetMesh(Ground, Table.regions[static_cast<size_t>(R)], kStepM, kLawnLiftM, M);
		const FVector Pivot = PivotOf(M, Side);
		SpawnSheet(World, Spawn, SheetMesh(Package, FString::Printf(TEXT("SM_Lawn_%d"), R), M, Side, Pivot, Lawn), Pivot, FString::Printf(TEXT("Lawn_%d"), R));
		Triangles += static_cast<int64>(M.tris.size());
	}
	if (!Table.paths.empty())
	{
		ftcore::LawnMesh M;
		for (size_t P = 0; P < Table.paths.size(); ++P)
		{
			ftcore::lawnPathMesh(Ground, Table.paths[P], kStepM, kPathLiftM + kPathStaggerM * static_cast<double>(P), kSkirtM, M);
		}
		const FVector Pivot = PivotOf(M, Side);
		SpawnSheet(World, Spawn, SheetMesh(Package, TEXT("SM_LawnPaths"), M, Side, Pivot, Path), Pivot, TEXT("LawnPaths"));
		Triangles += static_cast<int64>(M.tris.size());
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: -Lawn: %d lawn sheets and %d paths, %lld triangles, from %s."),
		static_cast<int32>(Table.regions.size()), static_cast<int32>(Table.paths.size()), Triangles, *File);
	return true;
}
