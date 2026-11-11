#include "FTBuildWorldMaterialsCommandlet.h"

#include "ForgottenThird.h"
#include "FTSceneLook.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "FTImportMaterials.h"
#include "FTJointCollection.h"
#include "FTText.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCameraPositionWS.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionLocalPosition.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "ftcore/SurfaceWeather.h"
#endif

#if WITH_EDITOR
// Named, not anonymous: in a unity build the other material commandlets' own AddExpression, AddInput and PackageFile
// share this translation unit.
namespace FTWorldMaterialsBuild
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

// One of the shape's axes in world space, scale included: the instance's for an instanced shape, else the actor's.
UMaterialExpressionTransform* AddWeatherAxis(UMaterial* Material, const FLinearColor& Axis, int32 Y)
{
	UMaterialExpressionConstant3Vector* Local = AddExpression<UMaterialExpressionConstant3Vector>(Material, -1200, Y);
	Local->Constant = Axis;
	UMaterialExpressionTransform* World = AddExpression<UMaterialExpressionTransform>(Material, -900, Y);
	World->Input.Connect(0, Local);
	World->TransformSourceType = TRANSFORMSOURCE_Instance;
	World->TransformType = TRANSFORM_World;
	return World;
}

void AddWeatherInput(UMaterialExpressionCustom* Custom, FName Name, UMaterialExpression* From, int32 Output)
{
	FCustomInput& In = Custom->Inputs.AddDefaulted_GetRef();
	In.InputName = Name;
	In.Input.Connect(Output, From);
}

// -Weather: ftcore::surfaceWeatherHlsl's node (wet up-facing ground, puddles favouring a cambered box's low edge; no
// dirt, since this master also dresses sign plates, poles and markings) over the surface node's albedo (output 0) and
// roughness (output 1), reading the look collection's Wetness; "Weather" (group Weather, 1) scales it per instance.
// "Street" (group Weather, 0; the road tints set it, StreetKind) turns on the node's street layer: aggregate, wheel paths,
// joints, tiles, kerb stones, worn paint. The node's normal (world space) is the master's: the mesh's vertex normal
// wherever Street is 0 and the ground is dry, so everything else looks as before. Its AO is the node's.
void AddWeather(UMaterial* Material, UMaterialExpressionCustom* Surface, UMaterialExpressionWorldPosition* WorldPos,
	UMaterialParameterCollection* Look)
{
	ftcore::SurfaceWeatherOptions Options;
	Options.grime = false;
	Options.street = true;
	Options.jointDepth = true;
	UMaterialExpressionCollectionParameter* Wetness = AddExpression<UMaterialExpressionCollectionParameter>(Material, -900, 600);
	Wetness->Collection = Look;
	Wetness->ParameterName = TEXT("Wetness");
	Wetness->ParameterId = Look->GetParameterId(TEXT("Wetness"));
	UMaterialExpressionScalarParameter* Amount = AddScalar(Material, TEXT("Weather"), 1.f, 700);
	Amount->Group = TEXT("Weather");
	UMaterialExpressionScalarParameter* Street = AddScalar(Material, TEXT("Street"), 0.f, 1400);
	Street->Group = TEXT("Weather");
	UMaterialExpressionVertexNormalWS* VertexNormal = AddExpression<UMaterialExpressionVertexNormalWS>(Material, -900, 800);
	UMaterialExpressionConstant* Occlusion = AddExpression<UMaterialExpressionConstant>(Material, -900, 900);
	Occlusion->R = 1.f;
	UMaterialExpressionLocalPosition* LocalPos = AddExpression<UMaterialExpressionLocalPosition>(Material, -900, 1000);

	UMaterialExpressionCustom* Weather = AddExpression<UMaterialExpressionCustom>(Material, -150, 0);
	Weather->Description = TEXT("FTSurfaceWeather");
	Weather->OutputType = CMOT_Float3;
	Weather->Code = FT::ToFString(ftcore::surfaceWeatherHlsl(ftcore::SurfaceWeatherSpec{}, Options));
	Weather->Inputs.Reset();
	AddWeatherInput(Weather, TEXT("Albedo"), Surface, 0);
	AddWeatherInput(Weather, TEXT("Normal"), VertexNormal, 0);
	AddWeatherInput(Weather, TEXT("Rough"), Surface, 1);
	AddWeatherInput(Weather, TEXT("AO"), Occlusion, 0);
	AddWeatherInput(Weather, TEXT("WorldPos"), WorldPos, 0);
	AddWeatherInput(Weather, TEXT("VertexNormal"), VertexNormal, 0);
	AddWeatherInput(Weather, TEXT("Wetness"), Wetness, 0);
	AddWeatherInput(Weather, TEXT("Amount"), Amount, 0);
	AddWeatherInput(Weather, TEXT("LocalPos"), LocalPos, 0);
	AddWeatherInput(Weather, TEXT("AxisX"), AddWeatherAxis(Material, FLinearColor(1.f, 0.f, 0.f), 1100), 0);
	AddWeatherInput(Weather, TEXT("AxisY"), AddWeatherAxis(Material, FLinearColor(0.f, 1.f, 0.f), 1200), 0);
	AddWeatherInput(Weather, TEXT("AxisZ"), AddWeatherAxis(Material, FLinearColor(0.f, 0.f, 1.f), 1300), 0);
	AddWeatherInput(Weather, TEXT("Street"), Street, 0);
	// Joint depth: the pixel-to-camera vector (cm) and the joint collection's JointDepth (the run-time switch; a constant
	// 1 where the collection is not built yet).
	UMaterialExpressionCameraPositionWS* CameraPos = AddExpression<UMaterialExpressionCameraPositionWS>(Material, -900, 1500);
	UMaterialExpressionSubtract* ViewVec = AddExpression<UMaterialExpressionSubtract>(Material, -700, 1500);
	ViewVec->A.Connect(0, CameraPos);
	ViewVec->B.Connect(0, WorldPos);
	AddWeatherInput(Weather, TEXT("ViewVec"), ViewVec, 0);
	UMaterialParameterCollection* JointCollection = FTJoint::LoadOrBuild();
	if (JointCollection && JointCollection->GetParameterId(FTJoint::Depth).IsValid())
	{
		UMaterialExpressionCollectionParameter* Switch = AddExpression<UMaterialExpressionCollectionParameter>(Material, -900, 1600);
		Switch->Collection = JointCollection;
		Switch->ParameterName = TEXT("JointDepth");
		Switch->ParameterId = JointCollection->GetParameterId(TEXT("JointDepth"));
		AddWeatherInput(Weather, TEXT("JointDepth"), Switch, 0);
	}
	else
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildWorldMaterials: the joint collection could not be made; joint depth is always on."));
		UMaterialExpressionConstant* One = AddExpression<UMaterialExpressionConstant>(Material, -900, 1600);
		One->R = 1.f;
		AddWeatherInput(Weather, TEXT("JointDepth"), One, 0);
	}
	const TCHAR* const Outputs[] = {TEXT("NormalOut"), TEXT("RoughOut"), TEXT("AOOut")};
	const ECustomMaterialOutputType Types[] = {CMOT_Float3, CMOT_Float1, CMOT_Float1};
	for (int32 O = 0; O < 3; ++O)
	{
		FCustomOutput& Out = Weather->AdditionalOutputs.AddDefaulted_GetRef();
		Out.OutputName = Outputs[O];
		Out.OutputType = Types[O];
	}
	Weather->RebuildOutputs();

	// Output 0 is the albedo, then NormalOut, RoughOut and AOOut.
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Weather);
	Material->bTangentSpaceNormal = false;
	Inputs->Normal.Connect(1, Weather);
	Inputs->Roughness.Connect(2, Weather);
	Inputs->AmbientOcclusion.Connect(3, Weather);
}

// Look (-Weather): the weather node over the surface.
UMaterial* BuildSurfaceMaterial(UPackage* Package, UMaterialParameterCollection* Look)
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
	if (Look)
	{
		AddWeather(Material, Custom, WorldPos, Look);
	}

	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	return Material;
}
} // namespace FTWorldMaterialsBuild
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
	// -Weather: the master reads the look's Wetness; its collection is found before anything is replaced.
	UMaterialParameterCollection* Look = nullptr;
	if (FParse::Param(*Params, TEXT("Weather")))
	{
		const FString LookPath = FString(FTScene::LookCollection) + TEXT(".") + FPackageName::GetShortName(FTScene::LookCollection);
		Look = LoadObject<UMaterialParameterCollection>(nullptr, *LookPath);
		if (!Look)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildWorldMaterials -Weather: %s is missing; build it first with FTBuildLookMaterials."),
				FTScene::LookCollection);
			return 1;
		}
	}
	// Start from nothing: replace the file up front, before anything loads the old asset.
	const FString File = FTWorldMaterialsBuild::PackageFile(FTScene::SurfaceMaterial);
	if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildWorldMaterials: couldn't replace %s."), *File);
		return 1;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);

	UPackage* Package = CreatePackage(FTScene::SurfaceMaterial);
	UMaterial* Material = FTWorldMaterialsBuild::BuildSurfaceMaterial(Package, Look);
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
