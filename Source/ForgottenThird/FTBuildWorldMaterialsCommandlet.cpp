#include "FTBuildWorldMaterialsCommandlet.h"

#include "ForgottenThird.h"
#include "FTSceneLook.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "FTImportMaterials.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#endif

#if WITH_EDITOR
namespace
{
// Two octaves of value noise on the world position, one channel for the albedo and one for the roughness, each
// 0..1, hashed on the integer lattice. Scale is the first octave's cell in centimetres.
const TCHAR* const kSurfaceCode = TEXT(R"HLSL(float3 P = WorldPos / max(Scale, 1.0);
float Mottle[2];
for (int Channel = 0; Channel < 2; ++Channel)
{
	float Sum = 0.0;
	float Weight = 2.0 / 3.0;
	float3 Q = P + Channel * float3(17.31, -9.77, 5.13);
	for (int Octave = 0; Octave < 2; ++Octave)
	{
		float3 Cell = floor(Q);
		float3 Frac = Q - Cell;
		float3 Fade = Frac * Frac * (3.0 - 2.0 * Frac);
		int3 Corner = int3(Cell);
		float Value[8];
		for (int K = 0; K < 8; ++K)
		{
			uint3 I = uint3(Corner + int3(K & 1, (K >> 1) & 1, K >> 2));
			uint H = (I.x * 73856093u) ^ (I.y * 19349663u) ^ (I.z * 83492791u);
			H ^= H >> 13;
			H *= 1274126177u;
			H ^= H >> 16;
			Value[K] = float(H & 0xFFFFu) / 65535.0;
		}
		float Lo = lerp(lerp(Value[0], Value[1], Fade.x), lerp(Value[2], Value[3], Fade.x), Fade.y);
		float Hi = lerp(lerp(Value[4], Value[5], Fade.x), lerp(Value[6], Value[7], Fade.x), Fade.y);
		Sum += Weight * lerp(Lo, Hi, Fade.z);
		Q = Q * 2.03 + 3.7;
		Weight *= 0.5;
	}
	Mottle[Channel] = Sum;
}
RoughOut = saturate(Rough * (1.0 + 0.5 * Amount * (2.0 * Mottle[1] - 1.0)));
float3 Albedo = Tint.rgb * (1.0 + Amount * (2.0 * Mottle[0] - 1.0));
Albedo *= min(1.0, 0.8 / max(max(Albedo.r, Albedo.g), max(Albedo.b, 1e-4)));
return max(Albedo, 0.04);
)HLSL");

FString PackageFile(const TCHAR* Package)
{
	return FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
}

// Adds an expression to the material the way the material editor does.
template <typename T>
T* AddExpression(UMaterial* Material, int32 X, int32 Y)
{
	T* Expression = NewObject<T>(Material, NAME_None, RF_Transactional);
	Material->GetExpressionCollection().AddExpression(Expression);
	Expression->Material = Material;
	Expression->MaterialExpressionEditorX = X;
	Expression->MaterialExpressionEditorY = Y;
	Expression->UpdateMaterialExpressionGuid(true, false);
	return Expression;
}

void AddInput(UMaterialExpressionCustom* Custom, FName Name, UMaterialExpression* From)
{
	FCustomInput& In = Custom->Inputs.AddDefaulted_GetRef();
	In.InputName = Name;
	In.Input.Connect(0, From);
}

UMaterialExpressionScalarParameter* AddScalar(UMaterial* Material, FName Name, float Value, int32 Y)
{
	UMaterialExpressionScalarParameter* P = AddExpression<UMaterialExpressionScalarParameter>(Material, -900, Y);
	P->ParameterName = Name;
	P->DefaultValue = Value;
	P->Group = TEXT("Surface");
	P->UpdateParameterGuid(true, false);
	return P;
}

UMaterial* BuildSurfaceMaterial(UPackage* Package)
{
	UMaterial* Material = NewObject<UMaterial>(Package, FName(FPackageName::GetShortName(Package->GetName())),
		RF_Public | RF_Standalone | RF_Transactional);
	// The world maps draw their roads, buildings and trees as instanced static meshes.
	Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);

	UMaterialExpressionVectorParameter* Color = AddExpression<UMaterialExpressionVectorParameter>(Material, -900, -200);
	Color->ParameterName = TEXT("Color");
	Color->DefaultValue = FLinearColor(0.36f, 0.36f, 0.36f);
	Color->Group = TEXT("Surface");
	Color->UpdateParameterGuid(true, false);
	UMaterialExpressionScalarParameter* Roughness = AddScalar(Material, TEXT("Roughness"), 0.8f, 0);
	UMaterialExpressionScalarParameter* Metallic = AddScalar(Material, TEXT("Metallic"), 0.f, 100);
	UMaterialExpressionScalarParameter* Variation = AddScalar(Material, TEXT("Variation"), 0.12f, 200);
	UMaterialExpressionScalarParameter* Scale = AddScalar(Material, TEXT("VariationScale"), 350.f, 300);
	UMaterialExpressionWorldPosition* WorldPos = AddExpression<UMaterialExpressionWorldPosition>(Material, -900, 400);

	UMaterialExpressionCustom* Custom = AddExpression<UMaterialExpressionCustom>(Material, -450, 0);
	Custom->Description = TEXT("FTWorldSurface");
	Custom->OutputType = CMOT_Float3;
	Custom->Code = kSurfaceCode;
	Custom->Inputs.Reset();
	AddInput(Custom, TEXT("Tint"), Color);
	AddInput(Custom, TEXT("Rough"), Roughness);
	AddInput(Custom, TEXT("Amount"), Variation);
	AddInput(Custom, TEXT("Scale"), Scale);
	AddInput(Custom, TEXT("WorldPos"), WorldPos);
	FCustomOutput& RoughOut = Custom->AdditionalOutputs.AddDefaulted_GetRef();
	RoughOut.OutputName = TEXT("RoughOut");
	RoughOut.OutputType = CMOT_Float1;
	Custom->RebuildOutputs();

	// Output 0 is the code's return value (the albedo), output 1 its RoughOut.
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Custom);
	Inputs->Roughness.Connect(1, Custom);
	Inputs->Metallic.Connect(0, Metallic);

	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	return Material;
}
}
#endif

UFTBuildWorldMaterialsCommandlet::UFTBuildWorldMaterialsCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildWorldMaterialsCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	// Start from nothing: replace the file up front, before anything loads the old asset.
	const FString File = PackageFile(FTScene::SurfaceMaterial);
	if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildWorldMaterials: couldn't replace %s."), *File);
		return 1;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);

	UPackage* Package = CreatePackage(FTScene::SurfaceMaterial);
	UMaterial* Material = BuildSurfaceMaterial(Package);
	FAssetCompilingManager::Get().FinishAllCompilation();
	if (FT::CompileErrors(TEXT("FTBuildWorldMaterials"), Material) > 0)
	{
		return 1;
	}

	FAssetRegistryModule::AssetCreated(Material);
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Material, *File, Args))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildWorldMaterials: couldn't save %s."), *File);
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildWorldMaterials: saved %s (%d expressions)."), *File,
		Material->GetExpressionCollection().Expressions.Num());
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildWorldMaterials needs an editor build."));
	return 1;
#endif
}
