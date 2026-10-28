#include "FTBuildSurfacesCommandlet.h"

#include "ForgottenThird.h"
#include "FTSceneLook.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture2D.h"
#include "FTImportMaterials.h"
#include "FTText.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "ftcore/SurfaceTable.h"
#endif

#if WITH_EDITOR
// Named, not anonymous: in a unity build the other material commandlets' own AddExpression and PackageFile share this
// translation unit.
namespace FTSurfaceBuild
{
// The three maps of a surface: the file name's part, the texture's suffix, and how it's stored and sampled.
struct FMapSpec
{
	const TCHAR* File;
	const TCHAR* Suffix;
	bool bSRGB;
	TextureCompressionSettings Compression;
	TextureGroup Group;
};
const FMapSpec kMaps[] = {
	{TEXT("diff"), TEXT("D"), true, TC_Default, TEXTUREGROUP_World},
	{TEXT("nor_dx"), TEXT("N"), false, TC_Normalmap, TEXTUREGROUP_WorldNormalMap},
	{TEXT("arm"), TEXT("ARM"), false, TC_Masks, TEXTUREGROUP_World},
};
constexpr int32 kMapCount = UE_ARRAY_COUNT(kMaps);
constexpr int32 kArmMap = 2;
// An AO/roughness/metal map whose roughness averages less than this has an empty channel: none of the world's surfaces
// is that smooth all over, and the surface would render as a mirror.
constexpr double kEmptyRoughness = 0.05;

// Three projections along the shape's own axes, blended by how squarely the face meets each (the albedo, a DirectX
// normal map and AO/roughness/metal maps sampled once per projection), then M_WorldSurface's mottle: two octaves of
// value noise on the world position, hashed on the integer lattice, one channel for the albedo and one for the
// roughness at Scale, a third for the albedo at MacroScale.
const TCHAR* const kSurfaceCode = TEXT(R"HLSL(float3 AxX = normalize(AxisX);
float3 AxY = normalize(AxisY);
float3 AxZ = normalize(AxisZ);
float3 Q = float3(dot(WorldPos, AxX), dot(WorldPos, AxY), dot(WorldPos, AxZ)) / max(TileCm, 1.0);
float3 Up = normalize(VertexNormal);
float3 Blend = pow(abs(float3(dot(Up, AxX), dot(Up, AxY), dot(Up, AxZ))), max(Sharpness, 1.0));
Blend /= max(Blend.x + Blend.y + Blend.z, 1e-5);
// Image-down runs down the world on a side face and down the slope on a top tilted about the shape's X.
float SideDown = AxZ.z > 0.0 ? -1.0 : 1.0;
float TopDown = AxY.z > 0.0 ? -1.0 : 1.0;
float2 UvX = float2(Q.y, SideDown * Q.z);
float2 UvY = float2(Q.x, SideDown * Q.z);
float2 UvZ = float2(Q.x, TopDown * Q.y);
// The engine's Texture2DSample, never Tex.Sample: a ray-tracing hit shader has no derivatives, and there it takes the
// mip from the ray cone. A plain Sample fails that shader, and the whole material falls back to the default one.
float3 Albedo = Blend.x * Texture2DSample(AlbedoTex, AlbedoTexSampler, UvX).rgb
	+ Blend.y * Texture2DSample(AlbedoTex, AlbedoTexSampler, UvY).rgb + Blend.z * Texture2DSample(AlbedoTex, AlbedoTexSampler, UvZ).rgb;
float3 Arm = Blend.x * Texture2DSample(ArmTex, ArmTexSampler, UvX).rgb + Blend.y * Texture2DSample(ArmTex, ArmTexSampler, UvY).rgb
	+ Blend.z * Texture2DSample(ArmTex, ArmTexSampler, UvZ).rgb;
// A DirectX normal map leans along +u with x and along +v (image-down) with y.
float3 NX = UnpackNormalMap(Texture2DSample(NormalTex, NormalTexSampler, UvX)).xyz;
float3 NY = UnpackNormalMap(Texture2DSample(NormalTex, NormalTexSampler, UvY)).xyz;
float3 NZ = UnpackNormalMap(Texture2DSample(NormalTex, NormalTexSampler, UvZ)).xyz;
float3 Lean = Blend.x * (NX.x * AxY + NX.y * SideDown * AxZ) + Blend.y * (NY.x * AxX + NY.y * SideDown * AxZ)
	+ Blend.z * (NZ.x * AxX + NZ.y * TopDown * AxY);
NormalOut = normalize(NormalStrength * Lean + (Blend.x * NX.z + Blend.y * NY.z + Blend.z * NZ.z) * Up);

float Mottle[3];
for (int Channel = 0; Channel < 3; ++Channel)
{
	float Sum = 0.0;
	float Weight = 2.0 / 3.0;
	float3 P = WorldPos / max(Channel == 2 ? MacroScale : Scale, 1.0) + Channel * float3(17.31, -9.77, 5.13);
	for (int Octave = 0; Octave < 2; ++Octave)
	{
		float3 Cell = floor(P);
		float3 Frac = P - Cell;
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
		P = P * 2.03 + 3.7;
		Weight *= 0.5;
	}
	Mottle[Channel] = Sum;
}
// A surface whose packed map has an empty roughness channel gets a constant from surfaces.json instead.
float Rough = RoughnessOverride >= 0.0 ? RoughnessOverride : Arm.g * RoughnessScale;
RoughOut = saturate(Rough * (1.0 + 0.5 * Amount * (2.0 * Mottle[1] - 1.0)));
MetalOut = Arm.b;
AOOut = Arm.r;
float3 Color = Albedo * Tint.rgb * (1.0 + Amount * (2.0 * Mottle[0] - 1.0)) * (1.0 + Macro * (2.0 * Mottle[2] - 1.0));
Color *= min(1.0, 0.8 / max(max(Color.r, Color.g), max(Color.b, 1e-4)));
return max(Color, 0.03);
)HLSL");

FString PackageFile(const FString& Package)
{
	return FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
}

FString TexturePackage(const FString& Id, const FMapSpec& Map)
{
	return FString::Printf(TEXT("%s/%s/T_%s_%s"), FTScene::SurfaceFolder, *Id, *Id, Map.Suffix);
}

FString SourceFile(const FString& Dir, const FString& Id, const FMapSpec& Map, uint32 ResK)
{
	return Dir / Id / FString::Printf(TEXT("%s_%s_%uk.jpg"), *Id, Map.File, ResK);
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

UMaterialExpressionScalarParameter* AddScalar(UMaterial* Material, FName Name, float Value, int32 Y)
{
	UMaterialExpressionScalarParameter* P = AddExpression<UMaterialExpressionScalarParameter>(Material, -900, Y);
	P->ParameterName = Name;
	P->DefaultValue = Value;
	P->Group = TEXT("Surface");
	P->UpdateParameterGuid(true, false);
	return P;
}

UMaterialExpressionTextureObjectParameter* AddTexture(UMaterial* Material, FName Name, UTexture* Default,
	EMaterialSamplerType Type, int32 Y)
{
	UMaterialExpressionTextureObjectParameter* P = AddExpression<UMaterialExpressionTextureObjectParameter>(Material, -900, Y);
	P->ParameterName = Name;
	P->Texture = Default;
	P->SamplerType = Type;
	P->Group = TEXT("Surface");
	P->UpdateParameterGuid(true, false);
	return P;
}

// One of the shape's axes in world space, scale included: the instance's for an instanced shape, else the actor's.
UMaterialExpressionTransform* AddAxis(UMaterial* Material, const FLinearColor& Axis, int32 Y)
{
	UMaterialExpressionConstant3Vector* Local = AddExpression<UMaterialExpressionConstant3Vector>(Material, -1200, Y);
	Local->Constant = Axis;
	UMaterialExpressionTransform* World = AddExpression<UMaterialExpressionTransform>(Material, -900, Y);
	World->Input.Connect(0, Local);
	World->TransformSourceType = TRANSFORMSOURCE_Instance;
	World->TransformType = TRANSFORM_World;
	return World;
}

// Imports a JPEG into a new texture at Path, keeping the JPEG as its source the way the editor's importer does with
// RetainJpegFormat: the asset costs about the file's size, and the JPEG is decoded when the texture is built.
UTexture2D* ImportJpeg(const FString& File, const FString& Path, const FMapSpec& Map)
{
	TArray<uint8> Bytes;
	FImage Image;
	if (!FFileHelper::LoadFileToArray(Bytes, *File) || !FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), Image))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSurfaces: couldn't read %s as a JPEG."), *File);
		return nullptr;
	}
	ETextureSourceFormat Format = TSF_Invalid;
	if (Image.Format == ERawImageFormat::BGRA8)
	{
		Format = TSF_BGRA8;
	}
	else if (Image.Format == ERawImageFormat::G8)
	{
		Format = TSF_G8;
	}
	else
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSurfaces: %s isn't an 8-bit JPEG."), *File);
		return nullptr;
	}

	UPackage* Package = CreatePackage(*Path);
	UTexture2D* Texture =
		NewObject<UTexture2D>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
	Texture->PreEditChange(nullptr);
	Texture->Source.InitWithCompressedSourceData(Image.SizeX, Image.SizeY, 1, Format,
		TArrayView64<uint8>(Bytes.GetData(), Bytes.Num()), ETextureSourceCompressionFormat::TSCF_JPEG);
	Texture->SRGB = Map.bSRGB;
	Texture->CompressionSettings = Map.Compression;
	Texture->LODGroup = Map.Group;
	// A projection doesn't use the mesh's UVs, which the streamer judges a texture's need by: load it whole.
	Texture->NeverStream = true;
	Texture->VirtualTextureStreaming = false;
	Texture->PostEditChange();
	FAssetRegistryModule::AssetCreated(Texture);
	return Texture;
}

// The mean of an AO/roughness/metal JPEG's roughness (green) channel in 0..1, read from every 8th pixel each way.
bool RoughnessMean(const FString& File, double& Mean)
{
	TArray<uint8> Bytes;
	FImage Image;
	if (!FFileHelper::LoadFileToArray(Bytes, *File) || !FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), Image))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSurfaces: couldn't read %s as a JPEG."), *File);
		return false;
	}
	// BGRA8 keeps green in byte 1 of each pixel; a grey JPEG has one byte.
	const int32 Stride = Image.Format == ERawImageFormat::BGRA8 ? 4 : Image.Format == ERawImageFormat::G8 ? 1 : 0;
	if (Stride == 0 || Image.SizeX <= 0 || Image.SizeY <= 0)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSurfaces: %s isn't an 8-bit JPEG."), *File);
		return false;
	}
	const int32 Channel = Stride == 4 ? 1 : 0;
	uint64 Sum = 0;
	uint64 Count = 0;
	for (int32 Y = 0; Y < Image.SizeY; Y += 8)
	{
		for (int32 X = 0; X < Image.SizeX; X += 8)
		{
			Sum += Image.RawData[(static_cast<int64>(Y) * Image.SizeX + X) * Stride + Channel];
			++Count;
		}
	}
	Mean = Sum / (255.0 * Count);
	return true;
}

// The master, with the first surface's textures as its defaults.
UMaterial* BuildMaster(TArrayView<UTexture* const> Defaults)
{
	UPackage* Package = CreatePackage(FTScene::SurfaceTexMaterial);
	UMaterial* Material = NewObject<UMaterial>(Package, FName(FPackageName::GetShortName(Package->GetName())),
		RF_Public | RF_Standalone | RF_Transactional);
	// The world maps draw their roads, buildings and trees as instanced static meshes.
	Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
	// The code bends the normal in world space.
	Material->bTangentSpaceNormal = false;

	UMaterialExpressionVectorParameter* Color = AddExpression<UMaterialExpressionVectorParameter>(Material, -900, -800);
	Color->ParameterName = TEXT("Color");
	Color->DefaultValue = FLinearColor(1.f, 1.f, 1.f);
	Color->Group = TEXT("Surface");
	Color->UpdateParameterGuid(true, false);
	UMaterialExpressionTextureObjectParameter* Albedo =
		AddTexture(Material, TEXT("AlbedoTex"), Defaults[0], SAMPLERTYPE_Color, -700);
	UMaterialExpressionTextureObjectParameter* Normal =
		AddTexture(Material, TEXT("NormalTex"), Defaults[1], SAMPLERTYPE_Normal, -550);
	UMaterialExpressionTextureObjectParameter* Arm = AddTexture(Material, TEXT("ArmTex"), Defaults[2], SAMPLERTYPE_Masks, -400);
	UMaterialExpressionScalarParameter* TileCm = AddScalar(Material, TEXT("TileCm"), 200.f, -250);
	UMaterialExpressionScalarParameter* Strength = AddScalar(Material, TEXT("NormalStrength"), 1.f, -150);
	UMaterialExpressionScalarParameter* Roughness = AddScalar(Material, TEXT("RoughnessScale"), 1.f, -50);
	UMaterialExpressionScalarParameter* RoughnessOverride = AddScalar(Material, TEXT("RoughnessOverride"), -1.f, 0);
	UMaterialExpressionScalarParameter* Variation = AddScalar(Material, TEXT("Variation"), 0.12f, 50);
	UMaterialExpressionScalarParameter* Scale = AddScalar(Material, TEXT("VariationScale"), 350.f, 150);
	UMaterialExpressionScalarParameter* Macro = AddScalar(Material, TEXT("MacroVariation"), 0.12f, 250);
	UMaterialExpressionScalarParameter* MacroScale = AddScalar(Material, TEXT("MacroScale"), 2500.f, 350);
	UMaterialExpressionScalarParameter* Sharpness = AddScalar(Material, TEXT("BlendSharpness"), 4.f, 450);
	UMaterialExpressionWorldPosition* WorldPos = AddExpression<UMaterialExpressionWorldPosition>(Material, -900, 550);
	UMaterialExpressionVertexNormalWS* VertexNormal = AddExpression<UMaterialExpressionVertexNormalWS>(Material, -900, 650);
	UMaterialExpressionTransform* AxisX = AddAxis(Material, FLinearColor(1.f, 0.f, 0.f), 750);
	UMaterialExpressionTransform* AxisY = AddAxis(Material, FLinearColor(0.f, 1.f, 0.f), 850);
	UMaterialExpressionTransform* AxisZ = AddAxis(Material, FLinearColor(0.f, 0.f, 1.f), 950);

	UMaterialExpressionCustom* Custom = AddExpression<UMaterialExpressionCustom>(Material, -450, 0);
	Custom->Description = TEXT("FTWorldSurfaceTex");
	Custom->OutputType = CMOT_Float3;
	Custom->Code = kSurfaceCode;
	Custom->Inputs.Reset();
	AddInput(Custom, TEXT("Tint"), Color);
	AddInput(Custom, TEXT("AlbedoTex"), Albedo);
	AddInput(Custom, TEXT("NormalTex"), Normal);
	AddInput(Custom, TEXT("ArmTex"), Arm);
	AddInput(Custom, TEXT("TileCm"), TileCm);
	AddInput(Custom, TEXT("NormalStrength"), Strength);
	AddInput(Custom, TEXT("RoughnessScale"), Roughness);
	AddInput(Custom, TEXT("RoughnessOverride"), RoughnessOverride);
	AddInput(Custom, TEXT("Amount"), Variation);
	AddInput(Custom, TEXT("Scale"), Scale);
	AddInput(Custom, TEXT("Macro"), Macro);
	AddInput(Custom, TEXT("MacroScale"), MacroScale);
	AddInput(Custom, TEXT("Sharpness"), Sharpness);
	AddInput(Custom, TEXT("WorldPos"), WorldPos);
	AddInput(Custom, TEXT("VertexNormal"), VertexNormal);
	AddInput(Custom, TEXT("AxisX"), AxisX);
	AddInput(Custom, TEXT("AxisY"), AxisY);
	AddInput(Custom, TEXT("AxisZ"), AxisZ);
	AddOutput(Custom, TEXT("NormalOut"), CMOT_Float3);
	AddOutput(Custom, TEXT("RoughOut"), CMOT_Float1);
	AddOutput(Custom, TEXT("MetalOut"), CMOT_Float1);
	AddOutput(Custom, TEXT("AOOut"), CMOT_Float1);
	Custom->RebuildOutputs();

	// Output 0 is the code's return value (the albedo), then its additional outputs in order.
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Custom);
	Inputs->Normal.Connect(1, Custom);
	Inputs->Roughness.Connect(2, Custom);
	Inputs->Metallic.Connect(3, Custom);
	Inputs->AmbientOcclusion.Connect(4, Custom);

	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	return Material;
}

UMaterialInstanceConstant* BuildInstance(const FString& Path, UMaterial* Master, TArrayView<UTexture* const> Maps, float TileCm,
	float Roughness)
{
	UPackage* Package = CreatePackage(*Path);
	UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, *FPackageName::GetShortName(Path),
		RF_Public | RF_Standalone | RF_Transactional);
	Mic->SetParentEditorOnly(Master);
	Mic->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("AlbedoTex")), Maps[0]);
	Mic->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("NormalTex")), Maps[1]);
	Mic->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("ArmTex")), Maps[2]);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("TileCm")), TileCm);
	if (Roughness >= 0.f)
	{
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("RoughnessOverride")), Roughness);
	}
	Mic->PostEditChange();
	FAssetRegistryModule::AssetCreated(Mic);
	return Mic;
}

// Saves Asset's package, adding the file's size to Bytes.
bool SaveAsset(UObject* Asset, int64& Bytes)
{
	UPackage* Package = Asset->GetPackage();
	const FString File = PackageFile(Package->GetName());
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Asset, *File, Args))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSurfaces: couldn't save %s."), *File);
		return false;
	}
	Bytes += IFileManager::Get().FileSize(*File);
	return true;
}
} // namespace FTSurfaceBuild
#endif

UFTBuildSurfacesCommandlet::UFTBuildSurfacesCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildSurfacesCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString Dir;
	if (!FParse::Value(*Params, TEXT("Source="), Dir, false) || Dir.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSurfaces: pass -Source=<dir>, the folder tools/fetch_surfaces.py fetched into."));
		return 1;
	}
	ftcore::SurfaceTable Table;
	if (!FTScene::LoadSurfaceTable(Table))
	{
		return 1;
	}
	if (Table.surfaces.empty())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSurfaces: Data/Field/surfaces.json is missing."));
		return 1;
	}

	// Every file first: nothing changes if one is missing.
	int32 Missing = 0;
	for (const ftcore::SurfaceSpec& Spec : Table.surfaces)
	{
		const FString Id = FT::ToFString(Spec.id);
		for (const FTSurfaceBuild::FMapSpec& Map : FTSurfaceBuild::kMaps)
		{
			const FString File = FTSurfaceBuild::SourceFile(Dir, Id, Map, Spec.resK);
			if (!IFileManager::Get().FileExists(*File))
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSurfaces: %s is missing."), *File);
				++Missing;
			}
		}
	}
	if (Missing > 0)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSurfaces: %d files missing; run tools/fetch_surfaces.py --out %s first."),
			Missing, *Dir);
		return 1;
	}

	// A packed map with an empty roughness channel renders its surface as a mirror, so the surface needs a constant
	// roughness from the table. An override on a map that has its own would hide the map once its file is fixed.
	int32 Unfit = 0;
	for (const ftcore::SurfaceSpec& Spec : Table.surfaces)
	{
		const FString Id = FT::ToFString(Spec.id);
		double Mean = 0.0;
		if (!FTSurfaceBuild::RoughnessMean(
				FTSurfaceBuild::SourceFile(Dir, Id, FTSurfaceBuild::kMaps[FTSurfaceBuild::kArmMap], Spec.resK), Mean))
		{
			return 1;
		}
		const bool bEmpty = Mean < FTSurfaceBuild::kEmptyRoughness;
		if (bEmpty && Spec.roughness < 0.f)
		{
			UE_LOG(LogForgottenThird, Error,
				TEXT("FTBuildSurfaces: %s's arm map has an empty roughness channel (it averages %.3f): give the surface a roughness in Data/Field/surfaces.json, or pick another surface."),
				*Id, Mean);
			++Unfit;
		}
		else if (!bEmpty && Spec.roughness >= 0.f)
		{
			UE_LOG(LogForgottenThird, Error,
				TEXT("FTBuildSurfaces: %s has a roughness in Data/Field/surfaces.json, but its arm map has its own (it averages %.2f): remove the override."),
				*Id, Mean);
			++Unfit;
		}
		else if (bEmpty)
		{
			UE_LOG(LogForgottenThird, Display,
				TEXT("FTBuildSurfaces: %s's arm map has an empty roughness channel; the surface takes %.2f from Data/Field/surfaces.json."),
				*Id, Spec.roughness);
		}
	}
	if (Unfit > 0)
	{
		return 1;
	}

	// Start from nothing: replace the files up front, before anything loads the old assets.
	TArray<FString> Packages;
	Packages.Add(FTScene::SurfaceTexMaterial);
	for (const ftcore::SurfaceSpec& Spec : Table.surfaces)
	{
		const FString Id = FT::ToFString(Spec.id);
		for (const FTSurfaceBuild::FMapSpec& Map : FTSurfaceBuild::kMaps)
		{
			Packages.Add(FTSurfaceBuild::TexturePackage(Id, Map));
		}
		Packages.Add(FTScene::SurfaceInstancePackage(Id));
	}
	for (const FString& Package : Packages)
	{
		const FString File = FTSurfaceBuild::PackageFile(Package);
		if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSurfaces: couldn't replace %s."), *File);
			return 1;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
	}

	TArray<UTexture*> Textures;
	for (const ftcore::SurfaceSpec& Spec : Table.surfaces)
	{
		const FString Id = FT::ToFString(Spec.id);
		for (const FTSurfaceBuild::FMapSpec& Map : FTSurfaceBuild::kMaps)
		{
			UTexture2D* Texture = FTSurfaceBuild::ImportJpeg(FTSurfaceBuild::SourceFile(Dir, Id, Map, Spec.resK), FTSurfaceBuild::TexturePackage(Id, Map), Map);
			if (!Texture)
			{
				return 1;
			}
			Textures.Add(Texture);
		}
	}
	UMaterial* Master = FTSurfaceBuild::BuildMaster(MakeArrayView(Textures.GetData(), FTSurfaceBuild::kMapCount));
	TArray<UMaterialInstanceConstant*> Instances;
	for (int32 S = 0; S < static_cast<int32>(Table.surfaces.size()); ++S)
	{
		const ftcore::SurfaceSpec& Spec = Table.surfaces[S];
		Instances.Add(FTSurfaceBuild::BuildInstance(FTScene::SurfaceInstancePackage(FT::ToFString(Spec.id)), Master,
			MakeArrayView(Textures.GetData() + S * FTSurfaceBuild::kMapCount, FTSurfaceBuild::kMapCount), Spec.tileCm,
			Spec.roughness));
	}

	FAssetCompilingManager::Get().FinishAllCompilation();
	if (FT::CompileErrors(TEXT("FTBuildSurfaces"), Master) > 0)
	{
		return 1;
	}
	FAssetRegistryModule::AssetCreated(Master);

	int64 Bytes = 0;
	bool bSaved = FTSurfaceBuild::SaveAsset(Master, Bytes);
	for (UTexture* Texture : Textures)
	{
		bSaved = FTSurfaceBuild::SaveAsset(Texture, Bytes) && bSaved;
	}
	for (UMaterialInstanceConstant* Instance : Instances)
	{
		bSaved = FTSurfaceBuild::SaveAsset(Instance, Bytes) && bSaved;
	}
	if (!bSaved)
	{
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildSurfaces: saved %d surfaces (%d textures, %d instances and %s) in %.1f MB."),
		Instances.Num(), Textures.Num(), Instances.Num(), FTScene::SurfaceTexMaterial, Bytes / (1024.0 * 1024.0));
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSurfaces needs an editor build."));
	return 1;
#endif
}
