#include "FTBuildLookMaterialsCommandlet.h"

#include "FTJointCollection.h"
#include "FTSeason.h"

#include "ForgottenThird.h"
#include "FTSceneLook.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "FTImportMaterials.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSingleLayerWaterMaterialOutput.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#endif

#if WITH_EDITOR
// Named, not anonymous: in a unity build the other material commandlets' own AddExpression, AddInput and PackageFile
// share this translation unit.
namespace FTLookMaterialsBuild
{
// The photographed surface the wet stone wears (Data/Field/surfaces.json's plaza stone) and its tile.
const TCHAR* const kStoneSurface = TEXT("cobblestone_color");
constexpr float kStoneTileCm = 240.f;

// Value noise on the integer lattice, hashed as the world surfaces' mottle is: 0..1, smooth between lattice points. A
// struct, since its methods are the only functions a custom node's code, one function's body, can hold.
#define FT_LOOK_NOISE_STRUCT(Name) TEXT("struct ") TEXT(#Name) TEXT(R"HLSL(
{
	float Noise(float2 P)
	{
		float2 Cell = floor(P);
		float2 Frac = P - Cell;
		float2 Fade = Frac * Frac * (3.0 - 2.0 * Frac);
		int2 Corner = int2(Cell);
		float Value[4];
		for (int K = 0; K < 4; ++K)
		{
			uint2 I = uint2(Corner + int2(K & 1, K >> 1));
			uint H = (I.x * 73856093u) ^ (I.y * 19349663u);
			H ^= H >> 13;
			H *= 1274126177u;
			H ^= H >> 16;
			Value[K] = float(H & 0xFFFFu) / 65535.0;
		}
		return lerp(lerp(Value[0], Value[1], Fade.x), lerp(Value[2], Value[3], Fade.x), Fade.y);
	}
	// Two octaves of it, one cell a unit.
	float Height(float2 P)
	{
		return (2.0 / 3.0) * Noise(P) + (1.0 / 3.0) * Noise(P * 2.03 + 3.7);
	}
	// Its slope along x and y, per unit, by central differences.
	float2 Slope(float2 P)
	{
		const float E = 0.04;
		return float2(Height(P + float2(E, 0.0)) - Height(P - float2(E, 0.0)), Height(P + float2(0.0, E)) - Height(P - float2(0.0, E)))
			/ (2.0 * E);
	}
};
)HLSL")

// The water's surface: two layers of waves, heights in centimetres, whose slopes bend the normal (in world space). The
// broad one's cells are WaveCm across and stand 5% of that high, drifting 0.05 of a cell a second; the fine one is 0.37 of
// the size, turned 1.1 radians, 3.5% high, drifting 0.09 of its cell a second across the first. WaveSpeed scales the
// drift and WaveStrength the slopes. Where the water lies less than 40 cm deep over what's behind it (along the view) the
// waves calm to a third and the surface roughens toward 0.18, so its edge meets the shore without a hard line.
const TCHAR* const kWaterCode = FT_LOOK_NOISE_STRUCT(FLookWaves) TEXT(R"HLSL(FLookWaves Waves;
float2 P = WorldPos.xy;
float Cell = max(WaveCm, 1.0);
float T = Time * WaveSpeed;
const float CosB = 0.4535961;
const float SinB = 0.8912074;
float2 GradA = 0.05 * Waves.Slope(P / Cell + T * float2(0.047, 0.017));
float2 Q = float2(CosB * P.x + SinB * P.y, CosB * P.y - SinB * P.x) / (0.37 * Cell);
float2 G = Waves.Slope(Q + T * float2(-0.035, 0.083));
float2 GradB = 0.035 * float2(CosB * G.x - SinB * G.y, SinB * G.x + CosB * G.y);
float Deep = saturate(max(BehindDepth - EyeDepth, 0.0) / 40.0);
float2 Grad = (GradA + GradB) * max(WaveStrength, 0.0) * lerp(0.35, 1.0, Deep);
RoughOut = saturate(lerp(max(Rough, 0.18), Rough, Deep));
return normalize(float3(-Grad.x, -Grad.y, 1.0));
)HLSL");

// The water's medium: the look's coefficients are per metre, the engine's per centimetre.
const TCHAR* const kWaterMediumCode = TEXT(R"HLSL(ScatterOut = max(Scatter.rgb, 0.0) * 0.01;
return max(Absorb.rgb, 0.0) * 0.01;
)HLSL");

// The wet stone: a photographed surface laid on the world position along each face's main axis (the floor's from above,
// a wall's from the side). As Wetness rises the stone soaks, quickly at first (all by the square root of Wetness): it
// darkens by WetDarken, its colour stands WetRich further from its grey, and a film of water glosses it, its roughness
// falling toward 0.06 by WetSmooth. Past PuddleStart, water stands on the floor, first where the broad dips (noise 150 cm
// across, its middle stretched to the full range) and the occluded hollows meet, spreading as Wetness reaches 1: near
// black, mirror smooth and flat.
const TCHAR* const kStoneCode = FT_LOOK_NOISE_STRUCT(FLookStone) TEXT(R"HLSL(FLookStone Stone;
float3 Up = normalize(VertexNormal);
float3 Axis = abs(Up);
float3 AxisU = Axis.z >= max(Axis.x, Axis.y) ? float3(1.0, 0.0, 0.0) : (Axis.x >= Axis.y ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0));
float3 AxisV = Axis.z >= max(Axis.x, Axis.y) ? float3(0.0, 1.0, 0.0) : float3(0.0, 0.0, 1.0);
float2 Uv = float2(dot(WorldPos, AxisU), dot(WorldPos, AxisV)) / max(TileCm, 1.0);
float3 Color =
	Texture2DSample(StoneAlbedo, GetMaterialSharedSampler(StoneAlbedoSampler, View.MaterialTextureBilinearWrapedSampler), Uv).rgb;
float3 Pack = Texture2DSample(StoneArm, GetMaterialSharedSampler(StoneArmSampler, View.MaterialTextureBilinearWrapedSampler), Uv).rgb;
float3 NM = UnpackNormalMap(
	Texture2DSample(StoneNormal, GetMaterialSharedSampler(StoneNormalSampler, View.MaterialTextureBilinearWrapedSampler), Uv)).xyz;
float Wet = saturate(Wetness);
float Grey = dot(Color, float3(0.2126, 0.7152, 0.0722));
float Soak = sqrt(Wet);
float3 Soaked = max(Grey + (Color - Grey) * (1.0 + WetRich * Soak), 0.0) * (1.0 - saturate(WetDarken) * Soak);
float Fill = saturate((Wet - PuddleStart) / max(1.0 - PuddleStart, 0.01));
float Low = 0.8 * smoothstep(0.3, 0.7, Stone.Height(WorldPos.xy / 150.0 + 5.3)) + 0.2 * saturate(1.0 - Pack.r);
float Floor = smoothstep(0.7, 0.9, Up.z);
float Puddle = Fill > 0.0 ? Floor * smoothstep(1.0 - 0.75 * Fill, 1.0 - 0.75 * Fill + 0.05, Low) : 0.0;
NormalOut = normalize((1.0 - Puddle) * (NM.x * AxisU + NM.y * AxisV) + NM.z * Up);
float Rough = lerp(Roughness >= 0.0 ? Roughness : Pack.g, 0.06, saturate(WetSmooth) * Soak);
RoughOut = saturate(lerp(Rough, 0.02, Puddle));
AOOut = lerp(Pack.r, 1.0, Puddle);
return max(lerp(Soaked, 0.35 * Soaked, Puddle), 0.02);
)HLSL");
#undef FT_LOOK_NOISE_STRUCT

FString PackageFile(const TCHAR* Package)
{
	return FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
}

// Adds an expression to the material the way the material editor does.
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

void AddInput(UMaterialExpressionCustom* Custom, FName Name, UMaterialExpression* From)
{
	FCustomInput& In = Custom->Inputs.AddDefaulted_GetRef();
	In.InputName = Name;
	In.Input.Connect(0, From);
}

void AddOutput(UMaterialExpressionCustom* Custom, FName Name, ECustomMaterialOutputType Type)
{
	FCustomOutput& Out = Custom->AdditionalOutputs.AddDefaulted_GetRef();
	Out.OutputName = Name;
	Out.OutputType = Type;
}

UMaterialExpressionCustom* AddCustom(UMaterial* Material, const TCHAR* Description, const TCHAR* Code, ECustomMaterialOutputType Type, int32 Y)
{
	UMaterialExpressionCustom* Custom = AddExpression<UMaterialExpressionCustom>(Material, -450, Y);
	Custom->Description = Description;
	Custom->OutputType = Type;
	Custom->Code = Code;
	Custom->Inputs.Reset();
	return Custom;
}

UMaterialExpressionScalarParameter* AddScalar(UMaterial* Material, FName Name, float Value, int32 Y)
{
	UMaterialExpressionScalarParameter* P = AddExpression<UMaterialExpressionScalarParameter>(Material, -900, Y);
	P->ParameterName = Name;
	P->DefaultValue = Value;
	P->Group = TEXT("Look");
	P->UpdateParameterGuid(true, false);
	return P;
}

UMaterialExpressionVectorParameter* AddVector(UMaterial* Material, FName Name, const FLinearColor& Value, int32 Y)
{
	UMaterialExpressionVectorParameter* P = AddExpression<UMaterialExpressionVectorParameter>(Material, -900, Y);
	P->ParameterName = Name;
	P->DefaultValue = Value;
	P->Group = TEXT("Look");
	P->UpdateParameterGuid(true, false);
	return P;
}

UMaterialExpressionTextureObjectParameter* AddTexture(UMaterial* Material, FName Name, UTexture* Default, EMaterialSamplerType Type, int32 Y)
{
	UMaterialExpressionTextureObjectParameter* P = AddExpression<UMaterialExpressionTextureObjectParameter>(Material, -900, Y);
	P->ParameterName = Name;
	P->Texture = Default;
	P->SamplerType = Type;
	P->Group = TEXT("Surface");
	P->UpdateParameterGuid(true, false);
	return P;
}

// One of the look collection's parameters.
UMaterialExpressionCollectionParameter* AddLook(UMaterial* Material, UMaterialParameterCollection* Collection, FName Name, int32 Y)
{
	UMaterialExpressionCollectionParameter* P = AddExpression<UMaterialExpressionCollectionParameter>(Material, -900, Y);
	P->Collection = Collection;
	P->ParameterName = Name;
	P->ParameterId = Collection->GetParameterId(Name);
	return P;
}

UMaterial* NewMaterial(const TCHAR* PackageName)
{
	UPackage* Package = CreatePackage(PackageName);
	return NewObject<UMaterial>(Package, FName(FPackageName::GetShortName(Package->GetName())), RF_Public | RF_Standalone | RF_Transactional);
}

void Finish(UMaterial* Material)
{
	// The lab's opaque surfaces stand on Nanite meshes; a game run can't add the usage itself.
	if (Material->BlendMode == BLEND_Opaque && !Material->GetShadingModels().HasShadingModel(MSM_SingleLayerWater))
	{
		Material->SetUsageByFlag(MATUSAGE_Nanite, true);
	}
	Material->PreEditChange(nullptr);
	Material->PostEditChange();
}

// The collection, its defaults SceneLook{}'s. Each parameter's id comes from its name, so a rebuild changes no id.
UMaterialParameterCollection* BuildCollection()
{
	UPackage* Package = CreatePackage(FTScene::LookCollection);
	UMaterialParameterCollection* Collection = NewObject<UMaterialParameterCollection>(Package,
		FName(FPackageName::GetShortName(Package->GetName())), RF_Public | RF_Standalone | RF_Transactional);
	const ftcore::SceneLook Default;
	for (const FTScene::FLookScalarParameter& Parameter : FTScene::LookScalarParameters)
	{
		FCollectionScalarParameter& Scalar = Collection->ScalarParameters.AddDefaulted_GetRef();
		Scalar.ParameterName = Parameter.Name;
		Scalar.DefaultValue = Default.*(Parameter.Field);
		Scalar.Id = FGuid::NewDeterministicGuid(FString(TEXT("FTLook.")) + Parameter.Name);
	}
	for (const FTScene::FLookVectorParameter& Parameter : FTScene::LookVectorParameters)
	{
		FCollectionVectorParameter& Vector = Collection->VectorParameters.AddDefaulted_GetRef();
		Vector.ParameterName = Parameter.Name;
		Vector.DefaultValue = FLinearColor(Default.*(Parameter.R), Default.*(Parameter.G), Default.*(Parameter.B), 1.f);
		Vector.Id = FGuid::NewDeterministicGuid(FString(TEXT("FTLook.")) + Parameter.Name);
	}
	// The surfaces' joint depth switch lives here too (FTJointCollection.h).
	FTJoint::AddScalarTo(*Collection);
	// And the plants' season (FTSeason.h).
	FTSeason::AddScalarTo(*Collection);
	Collection->PostEditChange();
	return Collection;
}

UMaterial* BuildWater(UMaterialParameterCollection* Look)
{
	UMaterial* Material = NewMaterial(FTScene::LookWaterMaterial);
	Material->SetShadingModel(MSM_SingleLayerWater);
	Material->BlendMode = BLEND_Opaque;
	Material->bTangentSpaceNormal = false;

	// The engine's own depth behind the water; its class isn't exported, so it's made by name.
	UClass* BehindClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.MaterialExpressionSceneDepthWithoutWater"));
	if (!BehindClass)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLookMaterials: the engine has no MaterialExpressionSceneDepthWithoutWater."));
		return nullptr;
	}

	UMaterialExpressionCustom* Waves = AddCustom(Material, TEXT("FTLookWaves"), FTLookMaterialsBuild::kWaterCode, CMOT_Float3, 0);
	AddInput(Waves, TEXT("WorldPos"), AddExpression<UMaterialExpressionWorldPosition>(Material, -900, 0));
	AddInput(Waves, TEXT("Time"), AddExpression<UMaterialExpressionTime>(Material, -900, 100));
	AddInput(Waves, TEXT("WaveCm"), AddLook(Material, Look, TEXT("WaterWaveCm"), 200));
	AddInput(Waves, TEXT("WaveSpeed"), AddLook(Material, Look, TEXT("WaterWaveSpeed"), 300));
	AddInput(Waves, TEXT("WaveStrength"), AddLook(Material, Look, TEXT("WaterWaveStrength"), 400));
	AddInput(Waves, TEXT("Rough"), AddLook(Material, Look, TEXT("WaterRoughness"), 500));
	AddInput(Waves, TEXT("EyeDepth"), AddExpression<UMaterialExpressionPixelDepth>(Material, -900, 600));
	AddInput(Waves, TEXT("BehindDepth"), AddExpressionOf(Material, BehindClass, -900, 700));
	AddOutput(Waves, TEXT("RoughOut"), CMOT_Float1);
	Waves->RebuildOutputs();

	UMaterialExpressionCustom* Medium = AddCustom(Material, TEXT("FTLookWaterMedium"), FTLookMaterialsBuild::kWaterMediumCode, CMOT_Float3, 900);
	AddInput(Medium, TEXT("Absorb"), AddLook(Material, Look, TEXT("WaterAbsorb"), 900));
	AddInput(Medium, TEXT("Scatter"), AddLook(Material, Look, TEXT("WaterScatter"), 1000));
	AddOutput(Medium, TEXT("ScatterOut"), CMOT_Float3);
	Medium->RebuildOutputs();

	UMaterialExpressionSingleLayerWaterMaterialOutput* Water = AddExpression<UMaterialExpressionSingleLayerWaterMaterialOutput>(Material, 0, 900);
	Water->AbsorptionCoefficients.Connect(0, Medium);
	Water->ScatteringCoefficients.Connect(1, Medium);
	Water->PhaseG.Connect(0, AddLook(Material, Look, TEXT("WaterPhaseG"), 1100));

	// Clear water over whatever lies under it: no top layer, water's own reflectance (F0 0.02) and index.
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, AddVector(Material, TEXT("TopColor"), FLinearColor(0.8f, 0.8f, 0.8f), -300));
	Inputs->Opacity.Connect(0, AddScalar(Material, TEXT("TopOpacity"), 0.f, -200));
	Inputs->Specular.Connect(0, AddScalar(Material, TEXT("Specular"), 0.25f, -100));
	Inputs->Refraction.Connect(0, AddScalar(Material, TEXT("IOR"), 1.33f, -400));
	Inputs->Normal.Connect(0, Waves);
	Inputs->Roughness.Connect(1, Waves);
	Finish(Material);
	return Material;
}

UMaterial* BuildGlass()
{
	UMaterial* Material = NewMaterial(FTScene::LookGlassMaterial);
	Material->BlendMode = BLEND_Translucent;
	Material->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
	Material->RefractionMethod = RM_IndexOfRefraction;
	Material->bAllowFrontLayerTranslucency = true;
	Material->bScreenSpaceReflections = true;
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, AddVector(Material, TEXT("Color"), FLinearColor(0.01f, 0.012f, 0.012f), 0));
	Inputs->Opacity.Connect(0, AddScalar(Material, TEXT("Opacity"), 0.08f, 100));
	Inputs->Roughness.Connect(0, AddScalar(Material, TEXT("Roughness"), 0.03f, 200));
	Inputs->Specular.Connect(0, AddScalar(Material, TEXT("Specular"), 0.5f, 300));
	Inputs->Refraction.Connect(0, AddScalar(Material, TEXT("IOR"), 1.5f, 400));
	Finish(Material);
	return Material;
}

UMaterial* BuildChrome()
{
	UMaterial* Material = NewMaterial(FTScene::LookChromeMaterial);
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, AddVector(Material, TEXT("Color"), FLinearColor(0.92f, 0.92f, 0.92f), 0));
	Inputs->Metallic.Connect(0, AddScalar(Material, TEXT("Metallic"), 1.f, 100));
	Inputs->Roughness.Connect(0, AddScalar(Material, TEXT("Roughness"), 0.06f, 200));
	Finish(Material);
	return Material;
}

UMaterial* BuildWetStone(UMaterialParameterCollection* Look, UTexture* const (&Maps)[3])
{
	UMaterial* Material = NewMaterial(FTScene::LookWetStoneMaterial);
	// The code bends the normal in world space.
	Material->bTangentSpaceNormal = false;
	UMaterialExpressionCustom* Stone = AddCustom(Material, TEXT("FTLookWetStone"), FTLookMaterialsBuild::kStoneCode, CMOT_Float3, 0);
	const EMaterialSamplerType Samplers[3] = {SAMPLERTYPE_Color, SAMPLERTYPE_Normal, SAMPLERTYPE_Masks};
	for (int32 M = 0; M < 3; ++M)
	{
		const FName Name(*(FString(TEXT("Stone")) + FTScene::SurfaceMaps[M].Name));
		AddInput(Stone, Name, AddTexture(Material, Name, Maps[M], Samplers[M], -600 + 150 * M));
	}
	AddInput(Stone, TEXT("TileCm"), AddScalar(Material, TEXT("TileCm"), kStoneTileCm, -100));
	AddInput(Stone, TEXT("Roughness"), AddScalar(Material, TEXT("Roughness"), -1.f, 0));
	AddInput(Stone, TEXT("Wetness"), AddLook(Material, Look, TEXT("Wetness"), 100));
	AddInput(Stone, TEXT("WetDarken"), AddScalar(Material, TEXT("WetDarken"), 0.5f, 200));
	AddInput(Stone, TEXT("WetRich"), AddScalar(Material, TEXT("WetRich"), 0.35f, 300));
	AddInput(Stone, TEXT("WetSmooth"), AddScalar(Material, TEXT("WetSmooth"), 0.85f, 400));
	AddInput(Stone, TEXT("PuddleStart"), AddScalar(Material, TEXT("PuddleStart"), 0.25f, 500));
	AddInput(Stone, TEXT("WorldPos"), AddExpression<UMaterialExpressionWorldPosition>(Material, -900, 600));
	AddInput(Stone, TEXT("VertexNormal"), AddExpression<UMaterialExpressionVertexNormalWS>(Material, -900, 700));
	AddOutput(Stone, TEXT("NormalOut"), CMOT_Float3);
	AddOutput(Stone, TEXT("RoughOut"), CMOT_Float1);
	AddOutput(Stone, TEXT("AOOut"), CMOT_Float1);
	Stone->RebuildOutputs();
	// Output 0 is the code's return value (the albedo), then its additional outputs in order.
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Stone);
	Inputs->Normal.Connect(1, Stone);
	Inputs->Roughness.Connect(2, Stone);
	Inputs->AmbientOcclusion.Connect(3, Stone);
	Finish(Material);
	return Material;
}

const TCHAR* const kGlowCode = TEXT("return max(Glow.rgb, 0.0) * max(Strength, 0.0);\n");

UMaterial* BuildGlow(UMaterialParameterCollection* Look)
{
	UMaterial* Material = NewMaterial(FTScene::LookGlowMaterial);
	UMaterialExpressionCustom* Glow = AddCustom(Material, TEXT("FTLookGlow"), kGlowCode, CMOT_Float3, 0);
	AddInput(Glow, TEXT("Glow"), AddLook(Material, Look, TEXT("GlowColor"), 0));
	AddInput(Glow, TEXT("Strength"), AddLook(Material, Look, TEXT("GlowStrength"), 100));
	Glow->RebuildOutputs();
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, AddVector(Material, TEXT("Color"), FLinearColor(0.03f, 0.03f, 0.035f), 200));
	Inputs->Roughness.Connect(0, AddScalar(Material, TEXT("Roughness"), 0.35f, 300));
	Inputs->EmissiveColor.Connect(0, Glow);
	Finish(Material);
	return Material;
}

bool Save(UObject* Asset, const TCHAR* PackageName)
{
	const FString File = PackageFile(PackageName);
	FAssetRegistryModule::AssetCreated(Asset);
	UPackage* Package = Asset->GetPackage();
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Asset, *File, Args))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLookMaterials: couldn't save %s."), *File);
		return false;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildLookMaterials: saved %s."), *File);
	return true;
}
} // namespace FTLookMaterialsBuild
#endif

UFTBuildLookMaterialsCommandlet::UFTBuildLookMaterialsCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildLookMaterialsCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	using namespace FTLookMaterialsBuild;
	const TCHAR* const Packages[] = {FTScene::LookCollection, FTScene::LookWaterMaterial, FTScene::LookGlassMaterial,
		FTScene::LookChromeMaterial, FTScene::LookWetStoneMaterial, FTScene::LookGlowMaterial};
	// Start from nothing: replace the files up front, before anything loads the old assets.
	for (const TCHAR* Package : Packages)
	{
		const FString File = PackageFile(Package);
		if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLookMaterials: couldn't replace %s."), *File);
			return 1;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
	}

	UTexture* Stone[3] = {};
	for (int32 M = 0; M < 3; ++M)
	{
		Stone[M] = FTScene::LoadSurfaceTexture(kStoneSurface, FTScene::SurfaceMaps[M].Suffix);
		if (!Stone[M])
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLookMaterials: %s is missing; run FTBuildSurfaces first."),
				*FTScene::SurfaceTexturePackage(kStoneSurface, FTScene::SurfaceMaps[M].Suffix));
			return 1;
		}
	}

	UMaterialParameterCollection* Look = BuildCollection();
	if (!Save(Look, FTScene::LookCollection))
	{
		return 1;
	}
	struct FBuilt
	{
		UMaterial* Material;
		const TCHAR* Package;
	};
	const FBuilt Built[] = {
		{BuildWater(Look), FTScene::LookWaterMaterial},
		{BuildGlass(), FTScene::LookGlassMaterial},
		{BuildChrome(), FTScene::LookChromeMaterial},
		{BuildWetStone(Look, Stone), FTScene::LookWetStoneMaterial},
		{BuildGlow(Look), FTScene::LookGlowMaterial},
	};
	FAssetCompilingManager::Get().FinishAllCompilation();
	int32 Errors = 0;
	for (const FBuilt& Each : Built)
	{
		Errors += Each.Material ? FT::CompileErrors(TEXT("FTBuildLookMaterials"), Each.Material) : 1;
	}
	if (Errors > 0)
	{
		return 1;
	}
	for (const FBuilt& Each : Built)
	{
		if (!Save(Each.Material, Each.Package))
		{
			return 1;
		}
	}
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLookMaterials needs an editor build."));
	return 1;
#endif
}
