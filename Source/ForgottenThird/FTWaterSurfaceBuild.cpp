#include "FTWaterSurfaceBuild.h"

// Run by commandlets only: editor builds only, so the game targets, Shipping included, carry none of it.
#if WITH_EDITOR

#include "ForgottenThird.h"
#include "FTWaterSubsystem.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCameraPositionWS.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace FTWaterSurfaceBuild
{
namespace
{
const TCHAR* const kInclude = TEXT("/Plugin/FTLook/Private/Water/Surface.ush");

// The vertex offset: the waves at the vertex's rest place (faded out far from the eye, where the grid is too coarse to
// carry them and nobody swims, and over a sea's last 150 m of tiles, where the far plane takes over with no step), the
// tide everywhere. WaveVerts 0 (the far plane): the tide alone. A skirt vertex (its drop in Grid.y) folds up to its rim
// while the eye is under the water line, so the skirts don't hang overhead as a grid.
const TCHAR* const kOffsetCode = TEXT(R"HLSL(if (SurfaceOn <= 0.0f)
{
	return float3(0.0f, 0.0f, 0.0f);
}
const float2 R = Rest.xy;
const float EdgeCm = FTWaterEdgeCm(R, EdgeA, EdgeB.xy);
float Near = WaveVerts * (1.0f - smoothstep(FarStartCm, FarEndCm, length(Eye.xy - R)));
if (ReachCm > 0.0f && EdgeB.y > 0.5f && EdgeB.y < 1.5f)
{
	Near *= 1.0f - smoothstep(ReachCm - 15000.0f, ReachCm, EdgeCm);
}
float3 O = float3(0.0f, 0.0f, 0.0f);
if (Near > 0.0f)
{
	float4 W[8] = {W0, W1, W2, W3, W4, W5, W6, W7};
	O = FTWaterOffset(R, Time, W, Q, max(Grid.x, 1.0f), FTWaterSway(EdgeCm, FadeCm), LapShare) * Near;
}
O.z += TideCm;
if (Grid.y > 0.0f && Eye.z < Rest.z + Grid.y + TideCm)
{
	O.z += Grid.y;
}
return O;)HLSL");

// The pixel's normal: the waves' slope added to the material's own ripples (both world space, z up).
const TCHAR* const kNormalCode = TEXT(R"HLSL(if (SurfaceOn <= 0.0f)
{
	return Ripple;
}
float4 W[8] = {W0, W1, W2, W3, W4, W5, W6, W7};
const float2 R = Rest.xy;
const float Sway = FTWaterSway(FTWaterEdgeCm(R, EdgeA, EdgeB.xy), FadeCm);
const float3 N = FTWaterNormal(R, Time, W, Q, Sway, LapShare);
const float2 S = N.xy / max(N.z, 0.05f) + Ripple.xy / max(Ripple.z, 0.05f);
return normalize(float3(S, 1.0f));)HLSL");

UMaterialExpression* AddExpressionOf(UMaterial* Material, UClass* Class, int32 X, int32 Y)
{
	UMaterialExpression* Expression = NewObject<UMaterialExpression>(Material, Class, NAME_None, RF_Transactional);
	Material->GetExpressionCollection().AddExpression(Expression);
	Expression->Material = Material;
	Expression->MaterialExpressionEditorX = X;
	Expression->MaterialExpressionEditorY = Y;
	Expression->UpdateMaterialExpressionGuid(true, false);
	return Expression;
}

template <typename T>
T* AddExpression(UMaterial* Material, int32 X, int32 Y)
{
	return CastChecked<T>(AddExpressionOf(Material, T::StaticClass(), X, Y));
}

UMaterialExpression* AddScalar(UMaterial* Material, const FName Name, float Value, int32 Y)
{
	UMaterialExpressionScalarParameter* P = AddExpression<UMaterialExpressionScalarParameter>(Material, -2400, Y);
	P->ParameterName = Name;
	P->DefaultValue = Value;
	P->Group = TEXT("WaterSurface");
	P->UpdateParameterGuid(true, false);
	return P;
}

UMaterialExpression* AddVector(UMaterial* Material, const FName Name, const FLinearColor& Value, int32 Y)
{
	UMaterialExpressionVectorParameter* P = AddExpression<UMaterialExpressionVectorParameter>(Material, -2400, Y);
	P->ParameterName = Name;
	P->DefaultValue = Value;
	P->Group = TEXT("WaterSurface");
	P->UpdateParameterGuid(true, false);
	return P;
}

UMaterialExpressionCustom* AddCustom(UMaterial* Material, const TCHAR* Description, const TCHAR* Code, int32 Y)
{
	UMaterialExpressionCustom* Custom = AddExpression<UMaterialExpressionCustom>(Material, -1900, Y);
	Custom->Description = Description;
	Custom->OutputType = CMOT_Float3;
	Custom->Code = Code;
	Custom->IncludeFilePaths.Add(kInclude);
	Custom->Inputs.Reset();
	return Custom;
}

void AddInput(UMaterialExpressionCustom* Custom, const FName Name, UMaterialExpression* From, int32 Output = 0)
{
	FCustomInput& In = Custom->Inputs.AddDefaulted_GetRef();
	In.InputName = Name;
	In.Input.Connect(Output, From);
}

// The parameters both nodes read, made once.
struct FSurfaceInputs
{
	UMaterialExpression* Waves[8] = {};
	UMaterialExpression* Q = nullptr;
	UMaterialExpression* FadeCm = nullptr;
	UMaterialExpression* LapShare = nullptr;
	UMaterialExpression* EdgeA = nullptr;
	UMaterialExpression* EdgeB = nullptr;
	UMaterialExpression* SurfaceOn = nullptr;
	UMaterialExpression* Rest = nullptr;
	UMaterialExpression* Time = nullptr;
};

void AddShared(UMaterialExpressionCustom* Custom, const FSurfaceInputs& In)
{
	AddInput(Custom, TEXT("SurfaceOn"), In.SurfaceOn);
	AddInput(Custom, TEXT("Rest"), In.Rest);
	AddInput(Custom, TEXT("Time"), In.Time);
	for (int32 I = 0; I < 8; ++I)
	{
		AddInput(Custom, *FString::Printf(TEXT("W%d"), I), In.Waves[I]);
	}
	AddInput(Custom, TEXT("Q"), In.Q);
	AddInput(Custom, TEXT("FadeCm"), In.FadeCm);
	AddInput(Custom, TEXT("LapShare"), In.LapShare);
	AddInput(Custom, TEXT("EdgeA"), In.EdgeA);
	AddInput(Custom, TEXT("EdgeB"), In.EdgeB);
}

// One LOD of the tile: Cells x Cells quads over kTileCm, centred, face up, and a skirt kSkirtCm down its rim.
void FillTile(FMeshDescription& Desc, int32 Cells)
{
	using namespace FTWaterSurfaceBuild;
	FStaticMeshAttributes Attributes(Desc);
	Attributes.Register();
	const FPolygonGroupID Group = Desc.CreatePolygonGroup();
	Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Water");
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector2f> Uvs = Attributes.GetVertexInstanceUVs();
	Uvs.SetNumChannels(2);
	const float Cell = kTileCm / static_cast<float>(Cells);
	const int32 Side = Cells + 1;
	TArray<FVertexID> Top;
	Top.Reserve(Side * Side);
	for (int32 J = 0; J < Side; ++J)
	{
		for (int32 I = 0; I < Side; ++I)
		{
			const FVertexID Id = Desc.CreateVertex();
			Positions[Id] = FVector3f(-0.5f * kTileCm + Cell * static_cast<float>(I), -0.5f * kTileCm + Cell * static_cast<float>(J), 0.f);
			Top.Add(Id);
		}
	}
	auto Corner = [&](FVertexID V)
	{
		const FVertexInstanceID Id = Desc.CreateVertexInstance(V);
		const FVector3f P = Positions[V];
		Uvs.Set(Id, 0, FVector2f(P.X, P.Y) / 100.f);
		// UV1: the grid's cell (cm), and on a skirt's lower rim its drop (cm), which the material folds up under water.
		Uvs.Set(Id, 1, FVector2f(Cell, P.Z < -1.f ? kSkirtCm : 0.f));
		return Id;
	};
	// The engine draws a face up when its first cross product points down (FTBuildLawn): a, c, b and a, d, c.
	auto Quad = [&](FVertexID A, FVertexID B, FVertexID C, FVertexID D)
	{
		FVertexInstanceID T0[3] = {Corner(A), Corner(C), Corner(B)};
		Desc.CreatePolygon(Group, TArrayView<const FVertexInstanceID>(T0, 3));
		FVertexInstanceID T1[3] = {Corner(A), Corner(D), Corner(C)};
		Desc.CreatePolygon(Group, TArrayView<const FVertexInstanceID>(T1, 3));
	};
	for (int32 J = 0; J < Cells; ++J)
	{
		for (int32 I = 0; I < Cells; ++I)
		{
			Quad(Top[J * Side + I], Top[J * Side + I + 1], Top[(J + 1) * Side + I + 1], Top[(J + 1) * Side + I]);
		}
	}
	// The skirt: the rim walked round, each step a wall down, both faces (it is seen from either side of a crack). Under
	// the water line the material folds it up flat (UV1.y), so a swimmer looking up sees no walls.
	TArray<FVertexID> Rim;
	for (int32 I = 0; I < Cells; ++I) Rim.Add(Top[I]);
	for (int32 J = 0; J < Cells; ++J) Rim.Add(Top[J * Side + Cells]);
	for (int32 I = Cells; I > 0; --I) Rim.Add(Top[Cells * Side + I]);
	for (int32 J = Cells; J > 0; --J) Rim.Add(Top[J * Side]);
	TArray<FVertexID> Low;
	for (const FVertexID V : Rim)
	{
		const FVertexID Id = Desc.CreateVertex();
		Positions[Id] = Positions[V] - FVector3f(0.f, 0.f, kSkirtCm);
		Low.Add(Id);
	}
	for (int32 K = 0; K < Rim.Num(); ++K)
	{
		const int32 N = (K + 1) % Rim.Num();
		Quad(Rim[K], Rim[N], Low[N], Low[K]);
		Quad(Rim[N], Rim[K], Low[K], Low[N]);
	}
}
} // namespace
} // namespace FTWaterSurfaceBuild

namespace FTWaterSurfaceBuild
{
void WireMaterial(UMaterial* Material)
{
	if (!Material)
	{
		return;
	}
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	UMaterialExpression* const Ripple = Inputs->Normal.Expression;
	const int32 RippleOut = Inputs->Normal.OutputIndex;
	if (!Ripple)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTWaterSurfaceBuild: %s has no normal to add the waves to."), *Material->GetName());
		return;
	}

	FSurfaceInputs In;
	int32 Y = 2000;
	for (int32 I = 0; I < 8; ++I, Y += 60)
	{
		In.Waves[I] = AddVector(Material, *FString::Printf(TEXT("W%d"), I), FLinearColor(0.f, 0.f, 0.f, 0.f), Y);
	}
	In.Q = AddScalar(Material, TEXT("Q"), 0.f, Y += 60);
	In.FadeCm = AddScalar(Material, TEXT("FadeCm"), 150.f, Y += 60);
	In.LapShare = AddScalar(Material, TEXT("LapShare"), 0.35f, Y += 60);
	In.EdgeA = AddVector(Material, TEXT("EdgeA"), FLinearColor(0.f, 0.f, 1.f, 0.f), Y += 60);
	In.EdgeB = AddVector(Material, TEXT("EdgeB"), FLinearColor(0.f, 2.f, 0.f, 0.f), Y += 60);
	In.SurfaceOn = AddScalar(Material, TEXT("SurfaceOn"), 0.f, Y += 60);
	UMaterialExpressionWorldPosition* Rest = AddExpression<UMaterialExpressionWorldPosition>(Material, -2400, Y += 60);
	Rest->WorldPositionShaderOffset = WPT_ExcludeAllShaderOffsets;
	In.Rest = Rest;
	In.Time = AddExpression<UMaterialExpressionTime>(Material, -2400, Y += 60);

	UMaterialExpressionCustom* Offset = AddCustom(Material, TEXT("FTWaterOffset"), kOffsetCode, 2000);
	AddShared(Offset, In);
	UMaterialExpressionTextureCoordinate* Grid = AddExpression<UMaterialExpressionTextureCoordinate>(Material, -2400, Y += 60);
	Grid->CoordinateIndex = 1;
	AddInput(Offset, TEXT("Grid"), Grid);
	AddInput(Offset, TEXT("Eye"), AddExpression<UMaterialExpressionCameraPositionWS>(Material, -2400, Y += 60));
	AddInput(Offset, TEXT("WaveVerts"), AddScalar(Material, TEXT("WaveVerts"), 1.f, Y += 60));
	AddInput(Offset, TEXT("FarStartCm"), AddScalar(Material, TEXT("FarStartCm"), 30000.f, Y += 60));
	AddInput(Offset, TEXT("FarEndCm"), AddScalar(Material, TEXT("FarEndCm"), 50000.f, Y += 60));
	AddInput(Offset, TEXT("TideCm"), AddScalar(Material, TEXT("TideCm"), 0.f, Y += 60));
	AddInput(Offset, TEXT("ReachCm"), AddScalar(Material, TEXT("ReachCm"), 0.f, Y += 60));

	UMaterialExpressionCustom* Normal = AddCustom(Material, TEXT("FTWaterNormal"), kNormalCode, 2600);
	AddShared(Normal, In);
	AddInput(Normal, TEXT("Ripple"), Ripple, RippleOut);

	Inputs->WorldPositionOffset.Connect(0, Offset);
	Inputs->Normal.Connect(0, Normal);
	// The tiles are instanced; a cooked game can't add the usage itself.
	Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
}

UStaticMesh* BuildTileMesh()
{
	const FString Path = UFTWaterSubsystem::TileMeshPath;
	UPackage* Package = CreatePackage(*Path);
	UStaticMesh* Mesh = NewObject<UStaticMesh>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone);
	UMaterialInterface* Water = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Look/M_Look_Water.M_Look_Water"));
	Mesh->GetStaticMaterials().Add(FStaticMaterial(Water, TEXT("Water"), TEXT("Water")));
	// Screen sizes for the three LODs: the 25 cm grid near the party, 1 m to a few tiles off, 4 m beyond.
	const float Screen[3] = {1.f, 0.5f, 0.12f};
	for (int32 L = 0; L < 3; ++L)
	{
		FStaticMeshSourceModel& Model = Mesh->AddSourceModel();
		Model.BuildSettings.bRecomputeNormals = true;
		Model.BuildSettings.bRecomputeTangents = true;
		Model.BuildSettings.bGenerateLightmapUVs = false;
		Model.ScreenSize.Default = Screen[L];
		FMeshDescription Desc;
		FillTile(Desc, kTileCells[L]);
		Mesh->CreateMeshDescription(L, MoveTemp(Desc));
		Mesh->CommitMeshDescription(L);
		Mesh->GetSectionInfoMap().Set(L, 0, FMeshSectionInfo(0));
	}
	Mesh->bAutoComputeLODScreenSize = false;
	// The waves lift and sway the vertices past the flat tile: grow the bounds, or a crest is culled with its tile.
	Mesh->SetPositiveBoundsExtension(FVector(400.0, 400.0, 600.0));
	Mesh->SetNegativeBoundsExtension(FVector(400.0, 400.0, 600.0));
	Mesh->Build(true);
	FAssetRegistryModule::AssetCreated(Mesh);
	FSavePackageArgs Save;
	Save.TopLevelFlags = RF_Public | RF_Standalone;
	const FString Out = FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Package, Mesh, *Out, Save))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTWaterSurfaceBuild: couldn't save %s."), *Out);
		return nullptr;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTWaterSurfaceBuild: saved %s (LODs of %d, %d and %d cells)."), *Out, kTileCells[0], kTileCells[1], kTileCells[2]);
	return Mesh;
}
} // namespace FTWaterSurfaceBuild

#endif // WITH_EDITOR
