#include "FTBuildVanMaterialsCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "FTImportMaterials.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionLocalPosition.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#endif

#if WITH_EDITOR
namespace FTVanMaterialsBuild
{
const TCHAR* const kBodyPackage = TEXT("/Game/Vehicles/PartyVan/M_VanBody");
const TCHAR* const kLampPackage = TEXT("/Game/Vehicles/PartyVan/M_VanLamp");
const TCHAR* const kBodyTexture = TEXT("/Game/Vehicles/PartyVan/T_party_van_model_basecolor.T_party_van_model_basecolor");

// The body in the van's mesh frame (cm, origin at the wheelbase middle on the ground, +x ahead, +y right). Glass is the
// window band (z 112 to 157) where it faces sideways over the three side windows, ahead over the windscreen, or back over
// the rear windows. Wear: per zone (front x > 120, rear x < -120, left y < -50, right y > 50) scuffs, and scratches
// through the paint to bare metal; then dust over everything with mud low down. The noise helpers sit in a struct,
// since a custom node's code is one function's body.
const TCHAR* const kBodyCode = TEXT(R"HLSL(struct FVanWear
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
	float Span(float X, float A, float B)
	{
		return smoothstep(A, A + 3.0, X) * (1.0 - smoothstep(B - 3.0, B, X));
	}
};
FVanWear W;
float3 C = Texture2DSample(Base, GetMaterialSharedSampler(BaseSampler, View.MaterialTextureBilinearWrapedSampler), Uv).rgb;
float3 P = LocalPos;
float3 N = normalize(LocalNormal);
float Band = W.Span(P.z, 112.0, 157.0);
float Side = smoothstep(0.55, 0.75, abs(N.y)) * saturate(W.Span(P.x, 57.0, 131.0) + W.Span(P.x, -51.0, 39.0) + W.Span(P.x, -133.0, -68.0));
float Ahead = smoothstep(0.35, 0.55, N.x) * W.Span(P.y, -62.0, 62.0);
float Back = smoothstep(0.55, 0.75, -N.x) * W.Span(P.y, -60.0, 60.0);
float Glass = Band * saturate(Side + Ahead + Back) * saturate(GlassMask);
float Rough = 0.42;
float Metal = 0.0;
float Spec = 0.5;
float Worn = max(max(saturate(WearFront) * smoothstep(120.0, 150.0, P.x), saturate(WearRear) * (1.0 - smoothstep(-150.0, -120.0, P.x))),
	max(saturate(WearLeft) * (1.0 - smoothstep(-70.0, -50.0, P.y)), saturate(WearRight) * smoothstep(50.0, 70.0, P.y)));
float2 Along = abs(N.x) > 0.6 ? P.yz : P.xz;
float Scuff = W.Noise(Along / 18.0 + 2.1) * 0.7 + W.Noise(Along / 5.0 + 7.3) * 0.3;
C *= 1.0 - 0.45 * Worn * smoothstep(0.35, 0.8, Scuff) * (1.0 - Glass);
float Streak = W.Noise(float2(Along.x / 9.0, Along.y * 1.3) + 11.0);
float Scratch = Worn > 0.0 ? smoothstep(1.0 - 0.4 * Worn, 1.04 - 0.4 * Worn, Streak) * (1.0 - Glass) : 0.0;
C = lerp(C, float3(0.52, 0.52, 0.54), Scratch);
Metal = lerp(Metal, 1.0, Scratch);
Rough = lerp(Rough, 0.32, Scratch) + 0.2 * Worn * (1.0 - Scratch);
float D = saturate(Dirt);
float Low = 1.0 - smoothstep(25.0, 95.0, P.z + 30.0 * W.Noise(P.xy / 40.0));
float Mud = saturate(D * (0.35 + 0.65 * Low) * (0.6 + 0.4 * W.Noise(Along / 12.0 + 3.0)));
C = lerp(C, float3(0.16, 0.13, 0.10), Mud * 0.85);
C = lerp(C, float3(0.42, 0.39, 0.34), 0.25 * D);
Rough = lerp(Rough, 0.85, max(Mud, 0.3 * D));
Metal *= 1.0 - Mud;
float Clear = Glass * (1.0 - 0.5 * Mud);
C = lerp(C, C * 0.15 + float3(0.012, 0.016, 0.02), Clear);
Rough = lerp(Rough, 0.05 + 0.3 * D, Clear);
Metal = lerp(Metal, 0.0, Clear);
Spec = lerp(Spec, 0.8, Clear);
RoughOut = saturate(Rough);
MetalOut = saturate(Metal);
SpecOut = Spec;
return max(C, 0.0);
)HLSL");

// A lens: its colour by day, and that colour times Glow as light when the lamp is on.
const TCHAR* const kLampCode = TEXT("return max(Color.rgb, 0.0) * max(Glow, 0.0);\n");

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

UMaterialExpressionCustom* AddCustom(UMaterial* Material, const TCHAR* Description, const TCHAR* Code)
{
	UMaterialExpressionCustom* Custom = AddExpression<UMaterialExpressionCustom>(Material, -450, 0);
	Custom->Description = Description;
	Custom->OutputType = CMOT_Float3;
	Custom->Code = Code;
	Custom->Inputs.Reset();
	return Custom;
}

UMaterialExpressionScalarParameter* AddScalar(UMaterial* Material, FName Name, float Value, int32 Y)
{
	UMaterialExpressionScalarParameter* P = AddExpression<UMaterialExpressionScalarParameter>(Material, -900, Y);
	P->ParameterName = Name;
	P->DefaultValue = Value;
	P->Group = TEXT("Van");
	P->UpdateParameterGuid(true, false);
	return P;
}

UMaterial* NewMaterial(const TCHAR* PackageName)
{
	UPackage* Package = CreatePackage(PackageName);
	return NewObject<UMaterial>(Package, FName(FPackageName::GetShortName(Package->GetName())), RF_Public | RF_Standalone | RF_Transactional);
}

void Finish(UMaterial* Material)
{
	// The van's meshes may be Nanite; a game run can't add the usage itself.
	Material->SetUsageByFlag(MATUSAGE_Nanite, true);
	Material->PreEditChange(nullptr);
	Material->PostEditChange();
}

UMaterial* BuildBody(UTexture* BaseColor)
{
	UMaterial* Material = NewMaterial(kBodyPackage);
	UMaterialExpressionCustom* Body = AddCustom(Material, TEXT("FTVanBody"), kBodyCode);
	UMaterialExpressionTextureObjectParameter* Base = AddExpression<UMaterialExpressionTextureObjectParameter>(Material, -900, -300);
	Base->ParameterName = TEXT("BaseColor");
	Base->Texture = BaseColor;
	Base->SamplerType = SAMPLERTYPE_Color;
	Base->Group = TEXT("Van");
	Base->UpdateParameterGuid(true, false);
	AddInput(Body, TEXT("Base"), Base);
	AddInput(Body, TEXT("Uv"), AddExpression<UMaterialExpressionTextureCoordinate>(Material, -900, -150));
	AddInput(Body, TEXT("LocalPos"), AddExpression<UMaterialExpressionLocalPosition>(Material, -900, -50));
	UMaterialExpressionTransform* ToLocal = AddExpression<UMaterialExpressionTransform>(Material, -700, 50);
	ToLocal->TransformSourceType = TRANSFORMSOURCE_World;
	ToLocal->TransformType = TRANSFORM_Local;
	ToLocal->Input.Connect(0, AddExpression<UMaterialExpressionVertexNormalWS>(Material, -900, 50));
	AddInput(Body, TEXT("LocalNormal"), ToLocal);
	AddInput(Body, TEXT("GlassMask"), AddScalar(Material, TEXT("GlassMask"), 1.f, 150));
	AddInput(Body, TEXT("Dirt"), AddScalar(Material, TEXT("Dirt"), 0.f, 250));
	AddInput(Body, TEXT("WearFront"), AddScalar(Material, TEXT("WearFront"), 0.f, 350));
	AddInput(Body, TEXT("WearRear"), AddScalar(Material, TEXT("WearRear"), 0.f, 450));
	AddInput(Body, TEXT("WearLeft"), AddScalar(Material, TEXT("WearLeft"), 0.f, 550));
	AddInput(Body, TEXT("WearRight"), AddScalar(Material, TEXT("WearRight"), 0.f, 650));
	AddOutput(Body, TEXT("RoughOut"), CMOT_Float1);
	AddOutput(Body, TEXT("MetalOut"), CMOT_Float1);
	AddOutput(Body, TEXT("SpecOut"), CMOT_Float1);
	Body->RebuildOutputs();
	// Output 0 is the code's return value (the colour), then its additional outputs in order.
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Body);
	Inputs->Roughness.Connect(1, Body);
	Inputs->Metallic.Connect(2, Body);
	Inputs->Specular.Connect(3, Body);
	Finish(Material);
	return Material;
}

UMaterial* BuildLamp()
{
	UMaterial* Material = NewMaterial(kLampPackage);
	UMaterialExpressionVectorParameter* Color = AddExpression<UMaterialExpressionVectorParameter>(Material, -900, 0);
	Color->ParameterName = TEXT("Color");
	Color->DefaultValue = FLinearColor(1.f, 0.95f, 0.85f);
	Color->Group = TEXT("Van");
	Color->UpdateParameterGuid(true, false);
	UMaterialExpressionCustom* Glow = AddCustom(Material, TEXT("FTVanLampGlow"), kLampCode);
	AddInput(Glow, TEXT("Color"), Color);
	AddInput(Glow, TEXT("Glow"), AddScalar(Material, TEXT("Glow"), 0.f, 100));
	Glow->RebuildOutputs();
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Color);
	Inputs->Roughness.Connect(0, AddScalar(Material, TEXT("Roughness"), 0.15f, 200));
	Inputs->Specular.Connect(0, AddScalar(Material, TEXT("Specular"), 0.8f, 300));
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
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVanMaterials: couldn't save %s."), *File);
		return false;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildVanMaterials: saved %s."), *File);
	return true;
}
} // namespace FTVanMaterialsBuild
#endif

UFTBuildVanMaterialsCommandlet::UFTBuildVanMaterialsCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildVanMaterialsCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	using namespace FTVanMaterialsBuild;
	UTexture* BaseColor = LoadObject<UTexture>(nullptr, kBodyTexture);
	if (!BaseColor)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVanMaterials: %s is missing; import the van first."), kBodyTexture);
		return 1;
	}
	const TCHAR* const Packages[] = {kBodyPackage, kLampPackage};
	// Start from nothing: replace the files up front, before anything loads the old assets.
	for (const TCHAR* Package : Packages)
	{
		const FString File = PackageFile(Package);
		if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVanMaterials: couldn't replace %s."), *File);
			return 1;
		}
	}
	UMaterial* const Built[] = {BuildBody(BaseColor), BuildLamp()};
	FAssetCompilingManager::Get().FinishAllCompilation();
	int32 Errors = 0;
	for (UMaterial* Each : Built)
	{
		Errors += Each ? FT::CompileErrors(TEXT("FTBuildVanMaterials"), Each) : 1;
	}
	if (Errors > 0)
	{
		return 1;
	}
	for (int32 I = 0; I < 2; ++I)
	{
		if (!Save(Built[I], Packages[I]))
		{
			return 1;
		}
	}
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVanMaterials needs an editor build."));
	return 1;
#endif
}
