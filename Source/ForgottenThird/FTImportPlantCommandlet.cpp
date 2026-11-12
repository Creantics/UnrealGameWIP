#include "FTImportPlantCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "FTImportMaterials.h"
#include "FTSceneLook.h"
#include "FTSeason.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionLocalPosition.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialParameterCollection.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace FTPlantImport
{
const TCHAR* const kTag = TEXT("FTImportPlant");
const TCHAR* const kFolder = TEXT("/Game/Rendering/Foliage");
const TCHAR* const kFoliagePath = TEXT("/Game/Rendering/Foliage/M_FTFoliage");
const TCHAR* const kBarkPath = TEXT("/Game/Rendering/Foliage/M_FTBark");
const TCHAR* const kScratch = TEXT("/Game/Props/_PlantImport");
constexpr float kLodScreenSize[] = {1.f, 0.45f, 0.20f, 0.06f};
constexpr int32 kMaxLods = UE_ARRAY_COUNT(kLodScreenSize);
constexpr float kClip = 0.5f;

// The maps: the file's suffix, the texture's, how it is stored and sampled; Optional ones may be missing.
enum EMap
{
	MapLeaf,
	MapLeafNormal,
	MapLeafMask,
	MapBark,
	MapBarkNormal,
	MapBillboard,
	MapBloom,
	MapCount
};
struct FMapSpec
{
	const TCHAR* Suffix;
	bool bSRGB;
	bool bAlpha; // keep the alpha's coverage through the mips
	bool bOptional;
	TextureCompressionSettings Compression;
	TextureGroup Group;
	EMaterialSamplerType Sampler;
};
const FMapSpec kMaps[MapCount] = {
	{TEXT("leaf"), true, true, false, TC_Default, TEXTUREGROUP_World, SAMPLERTYPE_Color},
	{TEXT("leaf_n"), false, false, false, TC_Normalmap, TEXTUREGROUP_WorldNormalMap, SAMPLERTYPE_Normal},
	{TEXT("leaf_m"), false, false, false, TC_Masks, TEXTUREGROUP_World, SAMPLERTYPE_Masks},
	{TEXT("bark"), true, false, true, TC_Default, TEXTUREGROUP_World, SAMPLERTYPE_Color},
	{TEXT("bark_n"), false, false, true, TC_Normalmap, TEXTUREGROUP_WorldNormalMap, SAMPLERTYPE_Normal},
	{TEXT("bb"), true, true, true, TC_Default, TEXTUREGROUP_World, SAMPLERTYPE_Color},
	{TEXT("bloom"), true, true, true, TC_Default, TEXTUREGROUP_World, SAMPLERTYPE_Color},
};

// The leaf cards, the billboard (BbMode 1: one picture, 2: the 2 x 2 atlas of leafy, bare and blossom) and the season.
// Texture2DSample, never Tex.Sample: a ray-tracing hit shader has no derivatives.
const TCHAR* const kLeafCode = TEXT(R"HLSL(float s = Season;
float r = Rand;
float v = 0.86 + 0.28 * frac(r * 7.13);
float w = frac(r * 3.71) - 0.5;
float k = HasBloom * saturate((0.6 + 0.3 * r - s) / 0.2);
float keep = 1.0 - Evergreen;
float4 c;
float3 m = float3(1.0, 0.0, r);
float4 bare = float4(0.0, 0.0, 0.0, 0.0);
float3 nt = float3(0.0, 0.0, 1.0);
if (BbMode < 0.5)
{
	c = Texture2DSample(LeafTex, LeafTexSampler, UV);
	m = Texture2DSample(LeafMaskTex, LeafMaskTexSampler, UV).rgb;
	if (HasBloom > 0.5)
	{
		c = lerp(c, Texture2DSample(BloomTex, BloomTexSampler, UV), k);
	}
	nt = UnpackNormalMap(Texture2DSample(LeafNormalTex, LeafNormalTexSampler, UV)).xyz;
}
else if (BbMode < 1.5)
{
	c = Texture2DSample(LeafTex, LeafTexSampler, UV);
}
else
{
	float2 q = UV * 0.5;
	c = Texture2DSample(LeafTex, LeafTexSampler, q);
	bare = Texture2DSample(LeafTex, LeafTexSampler, q + float2(0.5, 0.0));
	c = lerp(c, Texture2DSample(LeafTex, LeafTexSampler, q + float2(0.0, 0.5)), k);
}
float idv = frac(m.b + 0.37 * r);
float t = saturate((s - 1.2 - 0.6 * idv) / 0.3) * (1.0 - k) * keep;
float drop = saturate((s - 2.3 - 0.5 * idv) / 0.15) * (1.0 - k) * keep;
float3 col = c.rgb;
float lum = dot(col, float3(0.30, 0.59, 0.11));
float3 tint = float3(1.0 + 0.16 * w, 1.0, 1.0 - 0.20 * w) * v;
col *= lerp(tint, 1.0 + 0.4 * (tint - 1.0), k);
col = lerp(col, col * SpringTint.rgb, saturate(1.0 - s) * (1.0 - k));
float3 au = lerp(AutumnA.rgb, AutumnB.rgb, frac(idv * 5.3 + r * 0.61)) * (lum / max(MeanLum, 0.02)) * v;
float leafy = saturate(m.r * 2.0);
col = lerp(col, au, t * leafy);
float op = c.a;
if (BbMode < 0.5)
{
	col = lerp(col, TwigColor.rgb * v, drop * leafy);
	op = lerp(c.a, m.g, drop);
}
else if (BbMode < 1.5)
{
	op = c.a * (1.0 - drop);
}
else
{
	col = lerp(col, bare.rgb, drop);
	op = lerp(c.a, bare.a, drop);
}
OpacityOut = op;
nt.z *= Sign;
NormalOut = nt;
SubsurfaceOut = col * Subsurface;
return col;)HLSL");

const TCHAR* const kBarkCode = TEXT(R"HLSL(float v = 0.90 + 0.20 * frac(Rand * 7.13);
NormalOut = UnpackNormalMap(Texture2DSample(BarkNormalTex, BarkNormalTexSampler, UV)).xyz;
return Texture2DSample(BarkTex, BarkTexSampler, UV).rgb * v;)HLSL");

// The wind, the same for bark and leaves (so the twigs stay on their limbs): nothing below BendStart (metres), the sway
// growing with the square of the height over BendSpan and with the reach from the trunk; a phase per instance; the
// leaves flutter on top (Flutter 1). Centimetres, world space; WindDeg is a yaw (0 = toward +X).
const TCHAR* const kWindCode = TEXT(R"HLSL(float h = max(Local.z, 0.0) * 0.01;
float rad = length(Local.xy) * 0.01;
float bend = saturate((h - BendStart) / max(BendSpan, 0.5));
bend *= bend * (0.35 + 0.65 * saturate(rad / 2.5));
float sp = max(WindMps, 1.5);
float a = WindDeg * 0.0174533;
float2 d = float2(cos(a), sin(a));
float ph = Rand * 6.2832;
float gust = 0.55 + 0.30 * sin(Time * 0.63 + ph) + 0.15 * sin(Time * 1.71 + ph * 1.3);
float2 o = d * (sp * 1.1 * bend * gust) + float2(-d.y, d.x) * (sp * 0.25 * bend * sin(Time * 1.13 + ph * 2.1));
float3 off = float3(o, -0.004 * dot(o, o));
float fl = Flutter * saturate(h - 1.0) * sp * 0.12 * sin(Time * (6.0 + 2.0 * frac(Rand * 13.1)) + dot(Local, float3(0.031, 0.027, 0.043)));
return off + float3(0.3, 0.3, 1.0) * fl;)HLSL");

FString PackageFile(const FString& Package)
{
	return FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
}

FString ObjectPath(const FString& Package)
{
	return Package + TEXT(".") + FPackageName::GetShortName(Package);
}

bool SaveAsset(UObject* Asset)
{
	UPackage* Package = Asset->GetPackage();
	const FString File = PackageFile(Package->GetName());
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Asset, *File, Args))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: couldn't save %s."), kTag, *File);
		return false;
	}
	return true;
}

bool ClearPackage(const FString& Package)
{
	const FString File = PackageFile(Package);
	if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: couldn't replace %s."), kTag, *File);
		return false;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
	return true;
}

// The number after "key": in the json, or Default.
double JsonNumber(const FString& Json, const TCHAR* Key, double Default)
{
	const int32 At = Json.Find(FString::Printf(TEXT("\"%s\""), Key));
	if (At == INDEX_NONE)
	{
		return Default;
	}
	const int32 Colon = Json.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At);
	return Colon == INDEX_NONE ? Default : FCString::Atod(*Json + Colon + 1);
}

// The items of the list after "key": [a, b, ...] in the json; false if there is none.
bool JsonList(const FString& Json, const TCHAR* Key, TArray<FString>& Parts)
{
	const int32 At = Json.Find(FString::Printf(TEXT("\"%s\""), Key));
	const int32 Open = At == INDEX_NONE ? INDEX_NONE : Json.Find(TEXT("["), ESearchCase::CaseSensitive, ESearchDir::FromStart, At);
	const int32 Close = Open == INDEX_NONE ? INDEX_NONE : Json.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open);
	if (Close == INDEX_NONE)
	{
		return false;
	}
	Json.Mid(Open + 1, Close - Open - 1).ParseIntoArray(Parts, TEXT(","));
	for (FString& Part : Parts)
	{
		Part.TrimStartAndEndInline();
	}
	return true;
}

// An sRGB 0..255 triple after "key": [r, g, b] in the json as a linear colour, or Default.
FLinearColor JsonColor(const FString& Json, const TCHAR* Key, const FLinearColor& Default)
{
	TArray<FString> Parts;
	if (!JsonList(Json, Key, Parts) || Parts.Num() < 3)
	{
		return Default;
	}
	const auto Channel = [&Parts](int32 I) { return static_cast<uint8>(FMath::Clamp(FCString::Atoi(*Parts[I].TrimStartAndEnd()), 0, 255)); };
	return FLinearColor::FromSRGBColor(FColor(Channel(0), Channel(1), Channel(2)));
}

UTexture2D* ImportMap(const FString& File, const FString& Path, const FMapSpec& Map)
{
	TArray<uint8> Bytes;
	FImage Image;
	if (!FFileHelper::LoadFileToArray(Bytes, *File) || !FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), Image))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: couldn't read %s as an image."), kTag, *File);
		return nullptr;
	}
	if (Image.Format != ERawImageFormat::BGRA8 && Image.Format != ERawImageFormat::G8)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %s isn't an 8-bit image."), kTag, *File);
		return nullptr;
	}
	if (Image.Format != ERawImageFormat::BGRA8)
	{
		Image.ChangeFormat(ERawImageFormat::BGRA8, Image.GammaSpace);
	}
	UPackage* Package = CreatePackage(*Path);
	UTexture2D* Texture = NewObject<UTexture2D>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
	Texture->PreEditChange(nullptr);
	Texture->Source.Init(Image.SizeX, Image.SizeY, 1, 1, TSF_BGRA8, Image.RawData.GetData());
	Texture->SRGB = Map.bSRGB;
	Texture->CompressionSettings = Map.Compression;
	Texture->LODGroup = Map.Group;
	Texture->VirtualTextureStreaming = false;
	if (Map.bAlpha)
	{
		// The cut-out keeps its coverage in the small mips: far crowns stay as dense as near ones.
		Texture->bDoScaleMipsForAlphaCoverage = true;
		Texture->AlphaCoverageThresholds = FVector4(0.0, 0.0, 0.0, kClip);
	}
	Texture->PostEditChange();
	FAssetRegistryModule::AssetCreated(Texture);
	UE_LOG(LogForgottenThird, Display, TEXT("%s: %s: %d x %d, %s."), kTag, *FPaths::GetCleanFilename(File), Image.SizeX, Image.SizeY,
		Map.bSRGB ? TEXT("sRGB") : TEXT("linear"));
	return Texture;
}

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

// A node whose class the engine doesn't export (found by name).
UMaterialExpression* AddByClass(UMaterial* Material, const TCHAR* ClassName, int32 Y)
{
	UClass* const Class = FindObject<UClass>(FTopLevelAssetPath(TEXT("/Script/Engine"), ClassName));
	if (!Class)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: no %s node class."), kTag, ClassName);
		return nullptr;
	}
	UMaterialExpression* Node = NewObject<UMaterialExpression>(Material, Class, NAME_None, RF_Transactional);
	Material->GetExpressionCollection().AddExpression(Node);
	Node->Material = Material;
	Node->MaterialExpressionEditorX = -1100;
	Node->MaterialExpressionEditorY = Y;
	Node->UpdateMaterialExpressionGuid(true, false);
	return Node;
}

UMaterialExpressionScalarParameter* AddScalar(UMaterial* Material, FName Name, float Value, int32 Y)
{
	UMaterialExpressionScalarParameter* P = AddExpression<UMaterialExpressionScalarParameter>(Material, -1100, Y);
	P->ParameterName = Name;
	P->DefaultValue = Value;
	P->Group = TEXT("Plant");
	P->UpdateParameterGuid(true, false);
	return P;
}

UMaterialExpressionVectorParameter* AddVector(UMaterial* Material, FName Name, const FLinearColor& Value, int32 Y)
{
	UMaterialExpressionVectorParameter* P = AddExpression<UMaterialExpressionVectorParameter>(Material, -1100, Y);
	P->ParameterName = Name;
	P->DefaultValue = Value;
	P->Group = TEXT("Plant");
	P->UpdateParameterGuid(true, false);
	return P;
}

UMaterialExpressionTextureObjectParameter* AddTexture(UMaterial* Material, FName Name, UTexture2D* Texture, EMaterialSamplerType Sampler, int32 Y)
{
	UMaterialExpressionTextureObjectParameter* P = AddExpression<UMaterialExpressionTextureObjectParameter>(Material, -1100, Y);
	P->ParameterName = Name;
	P->Texture = Texture;
	P->SamplerType = Sampler;
	P->Group = TEXT("Plant");
	P->UpdateParameterGuid(true, false);
	return P;
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

// A collection's scalar, or a constant when the collection or the scalar is missing.
UMaterialExpression* AddCollectionScalar(UMaterial* Material, UMaterialParameterCollection* Collection, FName Name, float Fallback, int32 Y)
{
	if (Collection && Collection->GetParameterId(Name).IsValid())
	{
		UMaterialExpressionCollectionParameter* P = AddExpression<UMaterialExpressionCollectionParameter>(Material, -1100, Y);
		P->Collection = Collection;
		P->ParameterName = Name;
		P->ParameterId = Collection->GetParameterId(Name);
		return P;
	}
	UE_LOG(LogForgottenThird, Warning, TEXT("%s: %s has no %s; the masters use %.1f."), kTag,
		Collection ? *Collection->GetPathName() : TEXT("(a missing collection)"), *Name.ToString(), Fallback);
	UMaterialExpressionConstant* C = AddExpression<UMaterialExpressionConstant>(Material, -1100, Y);
	C->R = Fallback;
	return C;
}

UMaterial* NewMaster(const FString& Path, EMaterialShadingModel Shading, EBlendMode Blend, bool bTwoSided)
{
	UPackage* Package = CreatePackage(*Path);
	UMaterial* Material = NewObject<UMaterial>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
	Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
	Material->bTangentSpaceNormal = true;
	Material->SetShadingModel(Shading);
	Material->BlendMode = Blend;
	Material->TwoSided = bTwoSided;
	Material->OpacityMaskClipValue = kClip;
	Material->DitheredLODTransition = true;
	return Material;
}

// The wind's offset node; Flutter 0 for the bark, 1 for the leaves.
UMaterialExpressionCustom* AddWind(UMaterial* Material, UMaterialParameterCollection* Env, UMaterialExpression* Rand, float Flutter)
{
	UMaterialExpressionCustom* Wind = AddExpression<UMaterialExpressionCustom>(Material, -400, 900);
	Wind->Description = TEXT("FTPlantWind");
	Wind->OutputType = CMOT_Float3;
	Wind->Code = kWindCode;
	Wind->Inputs.Reset();
	UMaterialExpressionLocalPosition* Local = AddExpression<UMaterialExpressionLocalPosition>(Material, -1100, 900);
	Local->IncludedOffsets = EPositionIncludedOffsets::ExcludeOffsets;
	Local->LocalOrigin = ELocalPositionOrigin::Instance;
	AddInput(Wind, TEXT("Local"), Local);
	AddInput(Wind, TEXT("Rand"), Rand);
	UMaterialExpression* Time = AddByClass(Material, TEXT("MaterialExpressionTime"), 1000);
	AddInput(Wind, TEXT("Time"), Time);
	AddInput(Wind, TEXT("WindDeg"), AddCollectionScalar(Material, Env, TEXT("WindDeg"), 0.f, 1100));
	AddInput(Wind, TEXT("WindMps"), AddCollectionScalar(Material, Env, TEXT("WindMps"), 3.f, 1200));
	AddInput(Wind, TEXT("BendStart"), AddScalar(Material, TEXT("BendStart"), 2.f, 1300));
	AddInput(Wind, TEXT("BendSpan"), AddScalar(Material, TEXT("BendSpan"), 8.f, 1400));
	UMaterialExpressionConstant* F = AddExpression<UMaterialExpressionConstant>(Material, -1100, 1500);
	F->R = Flutter;
	AddInput(Wind, TEXT("Flutter"), F);
	return Time ? Wind : nullptr;
}

UMaterial* BuildFoliage(UTexture2D* const* Maps, UMaterialParameterCollection* Look, UMaterialParameterCollection* Env)
{
	UMaterial* Material = NewMaster(kFoliagePath, MSM_TwoSidedFoliage, BLEND_Masked, true);
	UMaterialExpressionCustom* Leaf = AddExpression<UMaterialExpressionCustom>(Material, -400, 0);
	Leaf->Description = TEXT("FTPlantLeaf");
	Leaf->OutputType = CMOT_Float3;
	Leaf->Code = kLeafCode;
	Leaf->Inputs.Reset();
	UMaterialExpression* Rand = AddByClass(Material, TEXT("MaterialExpressionPerInstanceRandom"), 800);
	UMaterialExpression* Sign = AddByClass(Material, TEXT("MaterialExpressionTwoSidedSign"), 700);
	if (!Rand || !Sign)
	{
		return nullptr;
	}
	UMaterialExpressionTextureCoordinate* Uv = AddExpression<UMaterialExpressionTextureCoordinate>(Material, -1100, -1000);
	AddInput(Leaf, TEXT("UV"), Uv);
	AddInput(Leaf, TEXT("LeafTex"), AddTexture(Material, TEXT("LeafTex"), Maps[MapLeaf], SAMPLERTYPE_Color, -900));
	AddInput(Leaf, TEXT("LeafNormalTex"), AddTexture(Material, TEXT("LeafNormalTex"), Maps[MapLeafNormal], SAMPLERTYPE_Normal, -800));
	AddInput(Leaf, TEXT("LeafMaskTex"), AddTexture(Material, TEXT("LeafMaskTex"), Maps[MapLeafMask], SAMPLERTYPE_Masks, -700));
	AddInput(Leaf, TEXT("BloomTex"), AddTexture(Material, TEXT("BloomTex"), Maps[MapBloom] ? Maps[MapBloom] : Maps[MapLeaf], SAMPLERTYPE_Color, -600));
	AddInput(Leaf, TEXT("HasBloom"), AddScalar(Material, TEXT("HasBloom"), 0.f, -500));
	AddInput(Leaf, TEXT("BbMode"), AddScalar(Material, TEXT("BbMode"), 0.f, -400));
	AddInput(Leaf, TEXT("Evergreen"), AddScalar(Material, TEXT("Evergreen"), 0.f, -450));
	AddInput(Leaf, TEXT("MeanLum"), AddScalar(Material, TEXT("MeanLum"), 0.09f, -300));
	AddInput(Leaf, TEXT("Subsurface"), AddScalar(Material, TEXT("Subsurface"), 0.55f, -200));
	AddInput(Leaf, TEXT("SpringTint"), AddVector(Material, TEXT("SpringTint"), FLinearColor(1.10f, 1.16f, 0.80f), -100));
	AddInput(Leaf, TEXT("AutumnA"), AddVector(Material, TEXT("AutumnA"), FLinearColor::FromSRGBColor(FColor(186, 112, 30)), 0));
	AddInput(Leaf, TEXT("AutumnB"), AddVector(Material, TEXT("AutumnB"), FLinearColor::FromSRGBColor(FColor(150, 48, 26)), 100));
	AddInput(Leaf, TEXT("TwigColor"), AddVector(Material, TEXT("TwigColor"), FLinearColor::FromSRGBColor(FColor(78, 64, 52)), 200));
	AddInput(Leaf, TEXT("Season"), AddCollectionScalar(Material, Look, FTSeason::Name, FTSeason::Default, 300));
	AddInput(Leaf, TEXT("Rand"), Rand);
	AddInput(Leaf, TEXT("Sign"), Sign);
	AddOutput(Leaf, TEXT("OpacityOut"), CMOT_Float1);
	AddOutput(Leaf, TEXT("NormalOut"), CMOT_Float3);
	AddOutput(Leaf, TEXT("SubsurfaceOut"), CMOT_Float3);
	Leaf->RebuildOutputs();
	UMaterialExpressionCustom* Wind = AddWind(Material, Env, Rand, 1.f);
	if (!Wind)
	{
		return nullptr;
	}
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Leaf);
	Inputs->OpacityMask.Connect(1, Leaf);
	Inputs->Normal.Connect(2, Leaf);
	Inputs->SubsurfaceColor.Connect(3, Leaf);
	Inputs->Roughness.Connect(0, AddScalar(Material, TEXT("Roughness"), 0.62f, 400));
	Inputs->Specular.Connect(0, AddScalar(Material, TEXT("Specular"), 0.35f, 500));
	Inputs->WorldPositionOffset.Connect(0, Wind);
	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	FAssetRegistryModule::AssetCreated(Material);
	return Material;
}

UMaterial* BuildBark(UTexture2D* Color, UTexture2D* Normal, UMaterialParameterCollection* Env)
{
	UMaterial* Material = NewMaster(kBarkPath, MSM_DefaultLit, BLEND_Opaque, false);
	UMaterialExpressionCustom* Bark = AddExpression<UMaterialExpressionCustom>(Material, -400, 0);
	Bark->Description = TEXT("FTPlantBark");
	Bark->OutputType = CMOT_Float3;
	Bark->Code = kBarkCode;
	Bark->Inputs.Reset();
	UMaterialExpression* Rand = AddByClass(Material, TEXT("MaterialExpressionPerInstanceRandom"), 800);
	if (!Rand)
	{
		return nullptr;
	}
	AddInput(Bark, TEXT("UV"), AddExpression<UMaterialExpressionTextureCoordinate>(Material, -1100, -1000));
	AddInput(Bark, TEXT("BarkTex"), AddTexture(Material, TEXT("BarkTex"), Color, SAMPLERTYPE_Color, -900));
	AddInput(Bark, TEXT("BarkNormalTex"), AddTexture(Material, TEXT("BarkNormalTex"), Normal, SAMPLERTYPE_Normal, -800));
	AddInput(Bark, TEXT("Rand"), Rand);
	AddOutput(Bark, TEXT("NormalOut"), CMOT_Float3);
	Bark->RebuildOutputs();
	UMaterialExpressionCustom* Wind = AddWind(Material, Env, Rand, 0.f);
	if (!Wind)
	{
		return nullptr;
	}
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Bark);
	Inputs->Normal.Connect(1, Bark);
	Inputs->Roughness.Connect(0, AddScalar(Material, TEXT("Roughness"), 0.86f, 400));
	Inputs->WorldPositionOffset.Connect(0, Wind);
	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	FAssetRegistryModule::AssetCreated(Material);
	return Material;
}

struct FPlantLook
{
	float BendStart = 2.f;
	float BendSpan = 8.f;
	float MeanLum = 0.09f;
	float BbMode = 1.f;
	float Evergreen = 0.f;
	FLinearColor AutumnA = FLinearColor::FromSRGBColor(FColor(186, 112, 30));
	FLinearColor AutumnB = FLinearColor::FromSRGBColor(FColor(150, 48, 26));
	FLinearColor Twig = FLinearColor::FromSRGBColor(FColor(78, 64, 52));
	FLinearColor Spring = FLinearColor(1.10f, 1.16f, 0.80f);
};

UMaterialInstanceConstant* BuildInstance(const FString& Path, UMaterial* Master, const FPlantLook& Look)
{
	UPackage* Package = CreatePackage(*Path);
	UMaterialInstanceConstant* Mic =
		NewObject<UMaterialInstanceConstant>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
	Mic->SetParentEditorOnly(Master);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("BendStart")), Look.BendStart);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("BendSpan")), Look.BendSpan);
	return Mic;
}

void SetTexture(UMaterialInstanceConstant* Mic, const TCHAR* Name, UTexture2D* Texture)
{
	if (Texture)
	{
		Mic->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Name), Texture);
	}
}

void SetLeafLook(UMaterialInstanceConstant* Mic, const FPlantLook& Look)
{
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("MeanLum")), Look.MeanLum);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Evergreen")), Look.Evergreen);
	Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("AutumnA")), Look.AutumnA);
	Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("AutumnB")), Look.AutumnB);
	Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("TwigColor")), Look.Twig);
	Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("SpringTint")), Look.Spring);
}

// Which of Leaf, Bark, Billboard (0..2) a slot is by its imported name, or INDEX_NONE.
int32 SlotKind(FName Slot)
{
	const FString Name = Slot.ToString();
	if (Name.Contains(TEXT("Billboard"), ESearchCase::IgnoreCase))
	{
		return 2;
	}
	if (Name.Contains(TEXT("Bark"), ESearchCase::IgnoreCase))
	{
		return 1;
	}
	if (Name.Contains(TEXT("Leaf"), ESearchCase::IgnoreCase))
	{
		return 0;
	}
	return INDEX_NONE;
}

int32 Run(const FString& Params)
{
	FString Source, Dest = TEXT("/Game/Props");
	if (!FParse::Value(*Params, TEXT("Source="), Source) || Source.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: pass -Source=<dir>/<NAME>.glb."), kTag);
		return 1;
	}
	FParse::Value(*Params, TEXT("Dest="), Dest);
	Source.TrimQuotesInline();
	if (FPaths::IsRelative(Source))
	{
		Source = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Source);
	}
	if (!FPaths::FileExists(Source))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %s doesn't exist."), kTag, *Source);
		return 1;
	}
	const FString Name = FPaths::GetBaseFilename(Source);
	const FString Dir = FPaths::GetPath(Source);
	const FString Folder = Dest / Name;

	// The maps.
	FString Files[MapCount];
	for (int32 I = 0; I < MapCount; ++I)
	{
		Files[I] = Dir / FString::Printf(TEXT("%s_%s.png"), *Name, kMaps[I].Suffix);
		if (!kMaps[I].bOptional && !FPaths::FileExists(Files[I]))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("%s: %s doesn't exist; nothing imported."), kTag, *Files[I]);
			return 1;
		}
	}
	FString Json;
	FFileHelper::LoadFileToString(Json, *(Dir / Name + TEXT(".json")));
	const double Height = JsonNumber(Json, TEXT("height_m"), 0.0);
	const double CrownBase = JsonNumber(Json, TEXT("crown_base_m"), 0.0);
	const double TrunkRadius = JsonNumber(Json, TEXT("trunk_radius_m"), 0.0);
	FPlantLook Look;
	if (Height > 0.0)
	{
		Look.BendStart = static_cast<float>(FMath::Max(0.0, CrownBase * 0.7));
		Look.BendSpan = static_cast<float>(FMath::Max(1.0, Height - Look.BendStart));
	}
	const FLinearColor Mean = JsonColor(Json, TEXT("leaf_mean_srgb"), FLinearColor(0.08f, 0.11f, 0.03f));
	Look.MeanLum = 0.30f * Mean.R + 0.59f * Mean.G + 0.11f * Mean.B;
	Look.AutumnA = JsonColor(Json, TEXT("autumn_a"), Look.AutumnA);
	Look.AutumnB = JsonColor(Json, TEXT("autumn_b"), Look.AutumnB);
	Look.Twig = JsonColor(Json, TEXT("twig"), Look.Twig);
	// The spring tint multiplies the summer colour, three plain factors (not sRGB).
	TArray<FString> Spring;
	if (JsonList(Json, TEXT("spring"), Spring) && Spring.Num() >= 3)
	{
		Look.Spring = FLinearColor(FCString::Atof(*Spring[0]), FCString::Atof(*Spring[1]), FCString::Atof(*Spring[2]));
	}
	Look.BbMode = JsonNumber(Json, TEXT("billboard_atlas"), 0.0) > 0.5 ? 2.f : 1.f;
	Look.Evergreen = JsonNumber(Json, TEXT("evergreen"), 0.0) > 0.5 ? 1.f : 0.f;

	// Replace this plant's files (and the masters when asked or missing).
	TArray<FString> TexturePaths;
	for (int32 I = 0; I < MapCount; ++I)
	{
		TexturePaths.Add(Folder / FString::Printf(TEXT("T_%s_%s"), *Name, kMaps[I].Suffix));
	}
	const FString LeafMi = Folder / FString::Printf(TEXT("MI_%s_Leaf"), *Name);
	const FString BarkMi = Folder / FString::Printf(TEXT("MI_%s_Bark"), *Name);
	const FString BbMi = Folder / FString::Printf(TEXT("MI_%s_Billboard"), *Name);
	const FString MeshPath = Folder / Name;
	TArray<FString> Packages = TexturePaths;
	Packages.Append({LeafMi, BarkMi, BbMi, MeshPath});
	UMaterial* Foliage = nullptr;
	UMaterial* BarkMaster = nullptr;
	const bool bMasters = FParse::Param(*Params, TEXT("Masters")) || !FPaths::FileExists(PackageFile(kFoliagePath)) ||
		!FPaths::FileExists(PackageFile(kBarkPath));
	if (bMasters)
	{
		Packages.Append({kFoliagePath, kBarkPath});
	}
	for (const FString& Package : Packages)
	{
		if (!ClearPackage(Package))
		{
			return 1;
		}
	}

	UTexture2D* Maps[MapCount] = {};
	for (int32 I = 0; I < MapCount; ++I)
	{
		if (FPaths::FileExists(Files[I]))
		{
			Maps[I] = ImportMap(Files[I], TexturePaths[I], kMaps[I]);
			if (!Maps[I])
			{
				return 1;
			}
		}
	}

	if (bMasters)
	{
		UMaterialParameterCollection* LookCollection = FTSeason::LoadOrBuild();
		UMaterialParameterCollection* Env = LoadObject<UMaterialParameterCollection>(nullptr, *ObjectPath(FTScene::EnvCollection), nullptr,
			LOAD_NoWarn | LOAD_Quiet);
		Foliage = BuildFoliage(Maps, LookCollection, Env);
		BarkMaster = BuildBark(Maps[MapBark] ? Maps[MapBark] : Maps[MapLeaf], Maps[MapBarkNormal] ? Maps[MapBarkNormal] : Maps[MapLeafNormal], Env);
		if (!Foliage || !BarkMaster)
		{
			return 1;
		}
		FAssetCompilingManager::Get().FinishAllCompilation();
		if (FT::CompileErrors(kTag, Foliage) > 0 || FT::CompileErrors(kTag, BarkMaster) > 0)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("%s: a plant master doesn't compile; nothing saved."), kTag);
			return 1;
		}
		if (!SaveAsset(Foliage) || !SaveAsset(BarkMaster))
		{
			return 1;
		}
	}
	else
	{
		Foliage = LoadObject<UMaterial>(nullptr, *ObjectPath(kFoliagePath));
		BarkMaster = LoadObject<UMaterial>(nullptr, *ObjectPath(kBarkPath));
		if (!Foliage || !BarkMaster)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("%s: couldn't load the plant masters (run with -Masters)."), kTag);
			return 1;
		}
	}

	UMaterialInstanceConstant* LeafInstance = BuildInstance(LeafMi, Foliage, Look);
	SetTexture(LeafInstance, TEXT("LeafTex"), Maps[MapLeaf]);
	SetTexture(LeafInstance, TEXT("LeafNormalTex"), Maps[MapLeafNormal]);
	SetTexture(LeafInstance, TEXT("LeafMaskTex"), Maps[MapLeafMask]);
	SetTexture(LeafInstance, TEXT("BloomTex"), Maps[MapBloom] ? Maps[MapBloom] : Maps[MapLeaf]);
	LeafInstance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("HasBloom")), Maps[MapBloom] ? 1.f : 0.f);
	SetLeafLook(LeafInstance, Look);
	UMaterialInstanceConstant* BbInstance = BuildInstance(BbMi, Foliage, Look);
	SetTexture(BbInstance, TEXT("LeafTex"), Maps[MapBillboard] ? Maps[MapBillboard] : Maps[MapLeaf]);
	SetTexture(BbInstance, TEXT("LeafNormalTex"), Maps[MapLeafNormal]);
	SetTexture(BbInstance, TEXT("LeafMaskTex"), Maps[MapLeafMask]);
	SetTexture(BbInstance, TEXT("BloomTex"), Maps[MapBillboard] ? Maps[MapBillboard] : Maps[MapLeaf]);
	BbInstance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("BbMode")), Look.BbMode);
	BbInstance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("HasBloom")), Maps[MapBloom] ? 1.f : 0.f);
	SetLeafLook(BbInstance, Look);
	UMaterialInstanceConstant* BarkInstance = BuildInstance(BarkMi, BarkMaster, Look);
	SetTexture(BarkInstance, TEXT("BarkTex"), Maps[MapBark]);
	SetTexture(BarkInstance, TEXT("BarkNormalTex"), Maps[MapBarkNormal]);
	UMaterialInstanceConstant* const Instances[3] = {LeafInstance, BarkInstance, BbInstance};
	for (UMaterialInstanceConstant* Mic : Instances)
	{
		Mic->PostEditChange();
		FAssetRegistryModule::AssetCreated(Mic);
	}

	// The file's meshes, imported unsaved into a scratch folder, one static mesh per LOD node.
	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = Source;
	Task->DestinationPath = FString(kScratch) / Name;
	Task->bAutomated = true;
	Task->bReplaceExisting = true;
	Task->bReplaceExistingSettings = true;
	Task->bSave = false;
	Task->bAsync = false;
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	AssetTools.ImportAssetTasks({Task});
	FAssetCompilingManager::Get().FinishAllCompilation();
	UStaticMesh* Lods[kMaxLods] = {};
	for (UObject* Object : Task->GetObjects())
	{
		UStaticMesh* Found = Cast<UStaticMesh>(Object);
		if (!Found)
		{
			continue;
		}
		const FString MeshName = Found->GetName();
		int32 Lod = INDEX_NONE;
		for (int32 L = 0; L < kMaxLods; ++L)
		{
			if (MeshName.EndsWith(FString::Printf(TEXT("_LOD%d"), L)) && !MeshName.StartsWith(TEXT("UCX_")))
			{
				Lod = L;
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("%s: the file's %s -> %s."), kTag, *MeshName,
			Lod == INDEX_NONE ? TEXT("not a LOD, left out") : *FString::Printf(TEXT("LOD%d"), Lod));
		if (Lod != INDEX_NONE)
		{
			Lods[Lod] = Found;
		}
	}
	int32 LodCount = 0;
	while (LodCount < kMaxLods && Lods[LodCount] && Lods[LodCount]->GetMeshDescription(0))
	{
		++LodCount;
	}
	if (LodCount == 0)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: no <NAME>_LOD0 mesh came out of %s."), kTag, *Source);
		return 1;
	}

	// One mesh with the LODs, its slots by name.
	UPackage* MeshPackage = CreatePackage(*MeshPath);
	UStaticMesh* Mesh = NewObject<UStaticMesh>(MeshPackage, *Name, RF_Public | RF_Standalone | RF_Transactional);
	Mesh->PreEditChange(nullptr);
	// The slots first: the build gives each polygon group the material whose imported slot name matches its own, and
	// falls back to the group's index when there is none (a billboard LOD's only group would take slot 0, the bark).
	TArray<FName> Slots;
	for (int32 L = 0; L < LodCount; ++L)
	{
		const FMeshDescription& Desc = *Lods[L]->GetMeshDescription(0);
		const FStaticMeshConstAttributes Attributes(Desc);
		for (const FPolygonGroupID Group : Desc.PolygonGroups().GetElementIDs())
		{
			Slots.AddUnique(Attributes.GetPolygonGroupMaterialSlotNames()[Group]);
		}
	}
	bool bOk = true;
	for (const FName Slot : Slots)
	{
		const int32 Kind = SlotKind(Slot);
		if (Kind == INDEX_NONE)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("%s: slot %s is none of Leaf, Bark, Billboard."), kTag, *Slot.ToString());
			bOk = false;
		}
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Kind == INDEX_NONE ? nullptr : Instances[Kind], Slot, Slot));
	}
	for (int32 L = 0; L < LodCount; ++L)
	{
		FMeshDescription Copy = *Lods[L]->GetMeshDescription(0);
		FStaticMeshSourceModel& Model = Mesh->AddSourceModel();
		Model.BuildSettings.bRecomputeNormals = false;
		Model.BuildSettings.bRecomputeTangents = true;
		Model.BuildSettings.bUseMikkTSpace = true;
		Model.BuildSettings.bGenerateLightmapUVs = false;
		Model.BuildSettings.bRemoveDegenerates = false;
		Model.BuildSettings.bUseFullPrecisionUVs = false;
		Model.ScreenSize = FPerPlatformFloat(kLodScreenSize[L]);
		Mesh->CreateMeshDescription(L, MoveTemp(Copy));
		Mesh->CommitMeshDescription(L);
		// each section's material by its group's slot name (the section info map otherwise keeps the group index)
		const FMeshDescription& Built = *Mesh->GetMeshDescription(L);
		const FStaticMeshConstAttributes BuiltAttributes(Built);
		int32 SectionIndex = 0;
		for (const FPolygonGroupID Group : Built.PolygonGroups().GetElementIDs())
		{
			FMeshSectionInfo Info = Mesh->GetSectionInfoMap().Get(L, SectionIndex);
			Info.MaterialIndex = FMath::Max(0, Slots.IndexOfByKey(BuiltAttributes.GetPolygonGroupMaterialSlotNames()[Group]));
			Mesh->GetSectionInfoMap().Set(L, SectionIndex, Info);
			++SectionIndex;
		}
	}
	Mesh->GetNaniteSettings().bEnabled = false;
	Mesh->SetAutoComputeLODScreenSize(false);
	Mesh->SetLightingGuid();
	Mesh->PostEditChange();
	FAssetCompilingManager::Get().FinishAllCompilation();

	// Simple collision only: the file's trunk convex, else a capsule round the trunk up to the crown's foot.
	Mesh->CreateBodySetup();
	UBodySetup* Body = Mesh->GetBodySetup();
	Body->Modify();
	Body->AggGeom.EmptyElements();
	const UBodySetup* FileBody = Lods[0]->GetBodySetup();
	const bool bFileHull = FileBody && FileBody->AggGeom.ConvexElems.Num() >= 1 && FileBody->AggGeom.GetElementCount() == FileBody->AggGeom.ConvexElems.Num();
	if (bFileHull)
	{
		Body->AggGeom.ConvexElems = FileBody->AggGeom.ConvexElems;
	}
	else if (TrunkRadius > 0.0 && CrownBase > 0.0)
	{
		const float Radius = static_cast<float>(TrunkRadius * 100.0);
		const float Top = static_cast<float>(CrownBase * 100.0);
		FKSphylElem Capsule(Radius, FMath::Max(1.f, Top - 2.f * Radius));
		Capsule.Center = FVector(0.0, 0.0, Top * 0.5);
		Body->AggGeom.SphylElems.Add(Capsule);
	}
	Body->CollisionTraceFlag = CTF_UseSimpleAsComplex;
	Body->InvalidatePhysicsData();
	Body->CreatePhysicsMeshes();
	Mesh->SetCustomizedCollision(true);
	const FBox HullBox = Body->AggGeom.CalcAABB(FTransform::Identity);
	UE_LOG(LogForgottenThird, Display, TEXT("%s: collision: %s, %d shapes, z %.0f..%.0f cm, %.0f cm across."), kTag,
		bFileHull ? TEXT("the file's trunk convex") : TEXT("a trunk capsule"), Body->AggGeom.GetElementCount(), HullBox.Min.Z,
		HullBox.Max.Z, HullBox.GetSize().X);
	if (Body->AggGeom.GetElementCount() == 0)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("%s: no collision (no UCX in the file and no trunk in the json)."), kTag);
	}

	if (!Mesh->GetRenderData() || Mesh->GetNumLODs() != LodCount)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: %s built %d LODs, not %d."), kTag, *Name, Mesh->GetNumLODs(), LodCount);
		return 1;
	}
	for (int32 L = 0; L < LodCount; ++L)
	{
		FString Sections;
		for (const FStaticMeshSection& Section : Mesh->GetRenderData()->LODResources[L].Sections)
		{
			const FStaticMaterial& Slot = Mesh->GetStaticMaterials()[Section.MaterialIndex];
			Sections += FString::Printf(TEXT(" %s %d;"), *Slot.MaterialSlotName.ToString(), Section.NumTriangles);
		}
		UE_LOG(LogForgottenThird, Display, TEXT("%s: LOD%d: %d triangles, screen size %.2f:%s"), kTag, L, Mesh->GetNumTriangles(L),
			kLodScreenSize[L], *Sections);
	}
	const FBox Bounds = Mesh->GetBoundingBox();
	UE_LOG(LogForgottenThird, Display, TEXT("%s: bounds x %.0f..%.0f, y %.0f..%.0f, z %.0f..%.0f cm; the json's height %.1f m."), kTag,
		Bounds.Min.X, Bounds.Max.X, Bounds.Min.Y, Bounds.Max.Y, Bounds.Min.Z, Bounds.Max.Z, Height);
	if (Height > 0.0 && FMath::Abs(Bounds.Max.Z - Height * 100.0) > Height * 100.0 * 0.15)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("%s: the mesh's top isn't at the json's height (units or axes?)."), kTag);
		bOk = false;
	}

	bool bSaved = true;
	for (UTexture2D* Texture : Maps)
	{
		if (Texture)
		{
			bSaved = SaveAsset(Texture) && bSaved;
		}
	}
	for (UMaterialInstanceConstant* Mic : Instances)
	{
		bSaved = SaveAsset(Mic) && bSaved;
	}
	bSaved = SaveAsset(Mesh) && bSaved;
	UE_LOG(LogForgottenThird, Display, TEXT("%s: %s, %s."), kTag, *Mesh->GetPathName(), bOk && bSaved ? TEXT("ok") : TEXT("failed"));
	return bOk && bSaved ? 0 : 1;
}
} // namespace FTPlantImport
#endif

UFTImportPlantCommandlet::UFTImportPlantCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTImportPlantCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	return FTPlantImport::Run(Params);
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTImportPlant needs an editor build."));
	return 1;
#endif
}
