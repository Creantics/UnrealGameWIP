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
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionLocalPosition.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "ftcore/MassingPlan.h"
#include "ftcore/SurfaceTable.h"
#include "ftcore/WindowFacade.h"
#include "ftcore/WindowInterior.h"
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
	return FTScene::SurfaceTexturePackage(Id, Map.Suffix);
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

// What the surface node reads that a window master reads too.
struct FSurfaceNode
{
	UMaterialExpressionCustom* Custom = nullptr;
	UMaterialExpressionVertexNormalWS* VertexNormal = nullptr;
	UMaterialExpressionTransform* Axis[3] = {};
};

// A new master at Path that draws as the world's boxes do: instanced, its normal in world space.
UMaterial* NewMaster(const TCHAR* Path)
{
	UPackage* Package = CreatePackage(Path);
	UMaterial* Material = NewObject<UMaterial>(Package, FName(FPackageName::GetShortName(Package->GetName())),
		RF_Public | RF_Standalone | RF_Transactional);
	// The world maps draw their roads, buildings and trees as instanced static meshes.
	Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
	// The code bends the normal in world space.
	Material->bTangentSpaceNormal = false;
	return Material;
}

// The photographed surface's parameters and its Custom node (kSurfaceCode), the textures' defaults Defaults. Output 0 is
// the code's return value (the albedo), then NormalOut, RoughOut, MetalOut and AOOut.
FSurfaceNode AddSurfaceNode(UMaterial* Material, TArrayView<UTexture* const> Defaults)
{
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
	FSurfaceNode Node;
	Node.Custom = Custom;
	Node.VertexNormal = VertexNormal;
	Node.Axis[0] = AxisX;
	Node.Axis[1] = AxisY;
	Node.Axis[2] = AxisZ;
	return Node;
}

// The master, with the first surface's textures as its defaults.
UMaterial* BuildMaster(TArrayView<UTexture* const> Defaults)
{
	UMaterial* Material = NewMaster(FTScene::SurfaceTexMaterial);
	UMaterialExpressionCustom* Custom = AddSurfaceNode(Material, Defaults).Custom;

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

// Points a built master's texture parameters at Defaults, as BuildMaster would have, setting bChanged if any moved. False
// if the master lacks one of them.
bool SetDefaults(UMaterial* Master, TArrayView<UTexture* const> Defaults, bool& bChanged)
{
	const FName Names[kMapCount] = {TEXT("AlbedoTex"), TEXT("NormalTex"), TEXT("ArmTex")};
	int32 Found = 0;
	bChanged = false;
	for (UMaterialExpression* Expression : Master->GetExpressions())
	{
		UMaterialExpressionTextureObjectParameter* P = Cast<UMaterialExpressionTextureObjectParameter>(Expression);
		for (int32 M = 0; P && M < kMapCount; ++M)
		{
			if (P->ParameterName != Names[M])
			{
				continue;
			}
			++Found;
			if (P->Texture != Defaults[M])
			{
				if (!bChanged)
				{
					Master->PreEditChange(nullptr);
				}
				P->Texture = Defaults[M];
				bChanged = true;
			}
		}
	}
	if (bChanged)
	{
		Master->PostEditChange();
	}
	return Found == kMapCount;
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

// ---- The windows (-Windows) ----

// The face a box's pixel lies on, in ftcore's boxFace order (0 +X, 1 -Y, 2 -X, 3 +Y of the engine's cube, which is 100 cm a
// side and centred, so an instance axis's length is the box's size along it in metres), and where on that face: Along
// across it from its left end and Up from its street floor (Base<f>), in metres, the face's width and height over the
// street floor, its street flag (bit f of StreetBits) and the view ray in its frame (across, up, into the wall). A top or
// bottom pixel gets no height, so no openings.
const TCHAR* const kWindowFaceCode = TEXT(R"HLSL(float Sx = length(AxisX);
float Sy = length(AxisY);
float Sz = length(AxisZ);
float3 Nx = AxisX / max(Sx, 1e-6);
float3 Ny = AxisY / max(Sy, 1e-6);
float3 Nz = AxisZ / max(Sz, 1e-6);
float3 L = LocalPos / 100.0 * float3(Sx, Sy, Sz);
float3 N = normalize(VertexNormal);
float nx = dot(N, Nx);
float ny = dot(N, Ny);
float nz = dot(N, Nz);
int F = abs(nx) >= abs(ny) ? (nx > 0.0 ? 0 : 2) : (ny < 0.0 ? 1 : 3);
float3 Out = F == 0 ? Nx : (F == 1 ? -Ny : (F == 2 ? -Nx : Ny));
float3 T = F == 0 ? Ny : (F == 1 ? Nx : (F == 2 ? -Ny : -Nx));
float Across = F == 0 ? L.y : (F == 1 ? L.x : (F == 2 ? -L.y : -L.x));
float Width = (F == 0 || F == 2) ? Sy : Sx;
float Base = F == 0 ? Base0 : (F == 1 ? Base1 : (F == 2 ? Base2 : Base3));
StreetOut = fmod(floor(floor(StreetBits + 0.5) / exp2((float)F)), 2.0);
UpOut = L.z + 0.5 * Sz - Base;
FaceHOut = abs(nz) < 0.5 ? Sz - Base : 0.0;
FaceWOut = Width;
float3 V = -CameraVector;
ViewOut = float3(dot(V, T), dot(V, Nz), -dot(V, Out));
return Across + 0.5 * Width;)HLSL");

// The wall, or what WindowInterior puts over it (Win.a, ftcore::WindowPart): glass is dark (the room past the curtain as
// a little albedo, the curtain as cloth under the light) and its own light is WindowInterior's Emit; the frame and the
// fascia have their own finish; the street band is the wall, darker.
const TCHAR* const kWindowComposeCode = TEXT(R"HLSL(bool Glass = Win.a > 0.5 && Win.a < 1.5;
bool Frame = Win.a > 1.5 && Win.a < 2.5;
bool Band = Win.a > 2.5 && Win.a < 3.5;
bool Fascia = Win.a > 3.5;
bool Flat = Glass || Frame || Fascia;
NormalOut = Flat ? normalize(VertexNormal) : WallNormal;
RoughOut = Glass ? GlassRough : (Frame ? FrameRough : (Fascia ? FasciaRough : WallRough));
MetalOut = Glass ? 0.0 : (Frame ? FrameMetal : (Fascia ? 0.0 : WallMetal));
AOOut = Flat ? 1.0 : WallAO;
EmissiveOut = Glass ? Emit : float3(0.0, 0.0, 0.0);
float3 GlassAlbedo = Win.rgb * GlassBase + Curtain.rgb * Curtain.a * CurtainAlbedo;
return Glass ? GlassAlbedo : (Frame ? FrameColor.rgb : (Fascia ? FasciaColor.rgb : (Band ? WallAlbedo * BandShade : WallAlbedo)));)HLSL");

// The box data the window master reads, in custom primitive data order (FTScene::WindowData*).
const TCHAR* const kWindowDataNames[FTScene::WindowDataCountR5] = {TEXT("Seed"), TEXT("StreetBits"), TEXT("Base0"), TEXT("Base1"),
	TEXT("Base2"), TEXT("Base3"), TEXT("Type"), TEXT("WallTint"), TEXT("RoofFinish")};
constexpr const TCHAR* kWindowNight = TEXT("Night");
// The collection's night glow: a multiplier on the windows' GlowScale and ShopGlow, 1 unless a look or the world eye
// (-WorldEyeNightGlow) sets it.
constexpr const TCHAR* kWindowNightGlow = TEXT("NightGlow");
// The window widths a building finish may take (metres): each finish keeps one, so a building never mixes sizes.
constexpr float kWindowWidths[] = {1.2f, 1.35f, 1.5f};

void AddInputFrom(UMaterialExpressionCustom* Custom, FName Name, UMaterialExpression* From, int32 Output)
{
	FCustomInput& In = Custom->Inputs.AddDefaulted_GetRef();
	In.InputName = Name;
	In.Input.Connect(Output, From);
}

UMaterialExpressionScalarParameter* AddWindowScalar(UMaterial* Material, FName Name, float Value, int32 Y)
{
	UMaterialExpressionScalarParameter* P = AddScalar(Material, Name, Value, Y);
	P->Group = TEXT("Window");
	return P;
}

// Imports a PNG into a new sRGB texture at Path, stretched to SizeX x SizeY (powers of two, so it mips; the material
// reads the atlas in fractions of its size, so the stretch changes nothing it reads). bAlpha keeps the alpha channel.
UTexture2D* ImportPng(const FString& File, const FString& Path, int32 SizeX, int32 SizeY, bool bAlpha)
{
	TArray<uint8> Bytes;
	FImage Image;
	if (!FFileHelper::LoadFileToArray(Bytes, *File) || !FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), Image))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSurfaces: couldn't read %s as an image."), *File);
		return nullptr;
	}
	FImage Sized;
	Image.ResizeTo(Sized, SizeX, SizeY, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	UPackage* Package = CreatePackage(*Path);
	UTexture2D* Texture =
		NewObject<UTexture2D>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
	Texture->PreEditChange(nullptr);
	Texture->Source.Init(SizeX, SizeY, 1, 1, TSF_BGRA8, Sized.RawData.GetData());
	Texture->SRGB = true;
	Texture->CompressionSettings = TC_Default;
	Texture->CompressionNoAlpha = !bAlpha;
	Texture->LODGroup = TEXTUREGROUP_World;
	// The rooms are read by a hash of the opening, not the mesh's UVs, which the streamer judges a texture's need by.
	Texture->NeverStream = true;
	Texture->VirtualTextureStreaming = false;
	Texture->PostEditChange();
	FAssetRegistryModule::AssetCreated(Texture);
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildSurfaces: %s: %d x %d, stretched to %d x %d."), *FPaths::GetCleanFilename(File),
		Image.SizeX, Image.SizeY, SizeX, SizeY);
	return Texture;
}

// The windows' collection: Night, 0 by day to 1 by night, and NightGlow, 1. Their ids come from their names, so a rebuild
// changes no id.
UMaterialParameterCollection* BuildWindowCollection()
{
	UPackage* Package = CreatePackage(FTScene::WindowCollection);
	UMaterialParameterCollection* Collection = NewObject<UMaterialParameterCollection>(Package,
		FName(FPackageName::GetShortName(Package->GetName())), RF_Public | RF_Standalone | RF_Transactional);
	const TCHAR* const Names[] = {kWindowNight, kWindowNightGlow};
	const float Defaults[] = {0.f, 1.f};
	for (int32 P = 0; P < static_cast<int32>(UE_ARRAY_COUNT(Names)); ++P)
	{
		FCollectionScalarParameter& Scalar = Collection->ScalarParameters.AddDefaulted_GetRef();
		Scalar.ParameterName = Names[P];
		Scalar.DefaultValue = Defaults[P];
		Scalar.Id = FGuid::NewDeterministicGuid(FString(TEXT("FTWindows.")) + Names[P]);
	}
	Collection->PostEditChange();
	FAssetRegistryModule::AssetCreated(Collection);
	return Collection;
}

// Round 5: the face node's three more outputs (6 TOut, 7 NzOut, 8 TopOut), after r4's.
void AddR5FaceOutputs(UMaterialExpressionCustom* Face)
{
	AddOutput(Face, TEXT("TOut"), CMOT_Float3);
	AddOutput(Face, TEXT("NzOut"), CMOT_Float3);
	AddOutput(Face, TEXT("TopOut"), CMOT_Float1);
}

// Round 5: the compose node's inputs beyond r4's: the face's along and up axes, its top flag, the wall tint and the roof
// finish.
void AddR5ComposeInputs(UMaterialExpressionCustom* Compose, UMaterialExpressionCustom* Face, UMaterialExpression* Tint,
	UMaterialExpression* RoofFinish)
{
	AddInputFrom(Compose, TEXT("T"), Face, 6);
	AddInputFrom(Compose, TEXT("Nz"), Face, 7);
	AddInputFrom(Compose, TEXT("Top"), Face, 8);
	AddInput(Compose, TEXT("Tint"), Tint);
	AddInput(Compose, TEXT("RoofFin"), RoofFinish);
}

// The window master: the photographed surface (its parameters as M_WorldSurfaceTex's, so an instance copies a surface's
// values across) with WindowInterior's openings cut into its walls. bR5 (-FacadeR5): WindowFacade's nodes instead, which
// read three more box values (type, wall tint, roof finish) and add the reveals, bands, shop kinds, tints and roof colour.
UMaterial* BuildWindowMaster(TArrayView<UTexture* const> Defaults, UTexture* Rooms, UTexture* Curtains, UMaterialParameterCollection* Collection,
	bool bR5)
{
	UMaterial* Material = NewMaster(FTScene::WindowMaterial);
	const FSurfaceNode Wall = AddSurfaceNode(Material, Defaults);

	const int32 DataCount = bR5 ? FTScene::WindowDataCountR5 : FTScene::WindowDataCount;
	UMaterialExpressionScalarParameter* Data[FTScene::WindowDataCountR5] = {};
	for (int32 D = 0; D < DataCount; ++D)
	{
		Data[D] = AddWindowScalar(Material, kWindowDataNames[D], 0.f, 1100 + D * 100);
		Data[D]->bUseCustomPrimitiveData = true;
		Data[D]->PrimitiveDataIndex = static_cast<uint8>(D);
	}
	UMaterialExpressionLocalPosition* LocalPos = AddExpression<UMaterialExpressionLocalPosition>(Material, -900, 1700);
	UMaterialExpressionCameraVectorWS* Camera = AddExpression<UMaterialExpressionCameraVectorWS>(Material, -900, 1800);

	UMaterialExpressionCustom* Face = AddExpression<UMaterialExpressionCustom>(Material, -600, 1300);
	Face->Description = TEXT("FTWindowFace");
	Face->OutputType = CMOT_Float1;
	Face->Code = bR5 ? FT::ToFString(ftcore::windowFacadeFaceHlsl()) : FString(kWindowFaceCode);
	Face->Inputs.Reset();
	AddInput(Face, TEXT("LocalPos"), LocalPos);
	AddInput(Face, TEXT("AxisX"), Wall.Axis[0]);
	AddInput(Face, TEXT("AxisY"), Wall.Axis[1]);
	AddInput(Face, TEXT("AxisZ"), Wall.Axis[2]);
	AddInput(Face, TEXT("VertexNormal"), Wall.VertexNormal);
	AddInput(Face, TEXT("CameraVector"), Camera);
	AddInput(Face, TEXT("StreetBits"), Data[FTScene::WindowDataStreet]);
	for (int32 F = 0; F < 4; ++F)
	{
		AddInput(Face, *FString::Printf(TEXT("Base%d"), F), Data[FTScene::WindowDataBase + F]);
	}
	// Output 0 is Along, then these in order.
	AddOutput(Face, TEXT("StreetOut"), CMOT_Float1);
	AddOutput(Face, TEXT("UpOut"), CMOT_Float1);
	AddOutput(Face, TEXT("FaceHOut"), CMOT_Float1);
	AddOutput(Face, TEXT("FaceWOut"), CMOT_Float1);
	AddOutput(Face, TEXT("ViewOut"), CMOT_Float3);
	if (bR5)
	{
		AddR5FaceOutputs(Face);
	}
	Face->RebuildOutputs();

	// The windows' numbers (Group "Window"): the shares lit, curtained and shop-lit; a building finish's window width; a
	// room's and a shop's own light by day and by night (the night ones times the collection's NightGlow); how opaque a
	// curtain is, how much of the room's light it passes at night and how much it shows as cloth by day.
	UMaterialExpressionScalarParameter* LitShare = AddWindowScalar(Material, TEXT("LitShare"), 0.35f, 1900);
	UMaterialExpressionScalarParameter* CurtainShare = AddWindowScalar(Material, TEXT("CurtainShare"), 0.35f, 2000);
	UMaterialExpressionScalarParameter* ShopLit = AddWindowScalar(Material, TEXT("ShopLit"), 0.8f, 2100);
	UMaterialExpressionScalarParameter* WindowWidth = AddWindowScalar(Material, TEXT("WindowWidth"), ftcore::WindowGridSpec{}.windowWidth, 2150);
	UMaterialExpressionScalarParameter* RoomDay = AddWindowScalar(Material, TEXT("RoomDay"), 0.5f, 2200);
	UMaterialExpressionScalarParameter* ShopDay = AddWindowScalar(Material, TEXT("ShopDay"), 0.9f, 2250);
	UMaterialExpressionScalarParameter* GlowScale = AddWindowScalar(Material, TEXT("GlowScale"), 0.35f, 2300);
	UMaterialExpressionScalarParameter* ShopGlow = AddWindowScalar(Material, TEXT("ShopGlow"), 0.55f, 2350);
	UMaterialExpressionScalarParameter* CurtainAlpha = AddWindowScalar(Material, TEXT("CurtainAlpha"), 0.85f, 2400);
	UMaterialExpressionScalarParameter* CurtainGlow = AddWindowScalar(Material, TEXT("CurtainGlow"), 0.35f, 2450);
	UMaterialExpressionCollectionParameter* Night = AddExpression<UMaterialExpressionCollectionParameter>(Material, -900, 2500);
	Night->Collection = Collection;
	Night->ParameterName = kWindowNight;
	Night->ParameterId = Collection->GetParameterId(kWindowNight);
	UMaterialExpressionCollectionParameter* NightGlow = AddExpression<UMaterialExpressionCollectionParameter>(Material, -900, 2550);
	NightGlow->Collection = Collection;
	NightGlow->ParameterName = kWindowNightGlow;
	NightGlow->ParameterId = Collection->GetParameterId(kWindowNightGlow);
	UMaterialExpressionMultiply* RoomGlow = AddExpression<UMaterialExpressionMultiply>(Material, -600, 2300);
	RoomGlow->A.Connect(0, GlowScale);
	RoomGlow->B.Connect(0, NightGlow);
	UMaterialExpressionMultiply* ShopNightGlow = AddExpression<UMaterialExpressionMultiply>(Material, -600, 2400);
	ShopNightGlow->A.Connect(0, ShopGlow);
	ShopNightGlow->B.Connect(0, NightGlow);
	UMaterialExpressionTextureObjectParameter* RoomsTex = AddTexture(Material, TEXT("Rooms"), Rooms, SAMPLERTYPE_Color, 2600);
	UMaterialExpressionTextureObjectParameter* CurtainsTex = AddTexture(Material, TEXT("Curtains"), Curtains, SAMPLERTYPE_Color, 2750);
	RoomsTex->Group = TEXT("Window");
	CurtainsTex->Group = TEXT("Window");

	// Output 0 is the room seen past the curtain (rgb) and the part (a), then the curtain (rgb, opacity) and the light.
	UMaterialExpressionCustom* Win = AddExpression<UMaterialExpressionCustom>(Material, -300, 1600);
	Win->Description = TEXT("FTWindowInterior");
	Win->OutputType = CMOT_Float4;
	Win->Code = FT::ToFString(bR5 ? ftcore::windowFacadeHlsl(ftcore::WindowFacadeSpec{}) : ftcore::windowInteriorHlsl(ftcore::WindowGridSpec{}));
	Win->Inputs.Reset();
	AddInputFrom(Win, TEXT("Along"), Face, 0);
	AddInputFrom(Win, TEXT("Up"), Face, 2);
	AddInputFrom(Win, TEXT("FaceW"), Face, 4);
	AddInputFrom(Win, TEXT("FaceH"), Face, 3);
	AddInput(Win, TEXT("Seed"), Data[FTScene::WindowDataSeed]);
	AddInputFrom(Win, TEXT("Street"), Face, 1);
	AddInputFrom(Win, TEXT("ViewT"), Face, 5);
	AddInput(Win, TEXT("WinW"), WindowWidth);
	AddInput(Win, TEXT("LitShare"), LitShare);
	AddInput(Win, TEXT("CurtainShare"), CurtainShare);
	AddInput(Win, TEXT("ShopLit"), ShopLit);
	AddInput(Win, TEXT("Night"), Night);
	AddInput(Win, TEXT("RoomDay"), RoomDay);
	AddInput(Win, TEXT("ShopDay"), ShopDay);
	AddInput(Win, TEXT("GlowScale"), RoomGlow);
	AddInput(Win, TEXT("ShopGlow"), ShopNightGlow);
	AddInput(Win, TEXT("CurtainAlpha"), CurtainAlpha);
	AddInput(Win, TEXT("CurtainGlow"), CurtainGlow);
	AddInput(Win, TEXT("Rooms"), RoomsTex);
	AddInput(Win, TEXT("Curtains"), CurtainsTex);
	if (bR5)
	{
		AddInput(Win, TEXT("Type"), Data[FTScene::WindowDataType]);
	}
	AddOutput(Win, TEXT("Curtain"), CMOT_Float4);
	AddOutput(Win, TEXT("Emit"), CMOT_Float3);
	Win->RebuildOutputs();

	// The glass is dark (GlassBase of the room as albedo, so the light is Emit's) and the curtain shows as cloth under the
	// day's light; the street band is the wall, darker; the fascia has its own dark finish.
	UMaterialExpressionScalarParameter* GlassBase = AddWindowScalar(Material, TEXT("GlassBase"), 0.06f, 2900);
	UMaterialExpressionScalarParameter* GlassRough = AddWindowScalar(Material, TEXT("GlassRough"), 0.08f, 2950);
	UMaterialExpressionScalarParameter* CurtainAlbedo = AddWindowScalar(Material, TEXT("CurtainAlbedo"), 0.6f, 3000);
	UMaterialExpressionVectorParameter* FrameColor = AddExpression<UMaterialExpressionVectorParameter>(Material, -900, 3050);
	FrameColor->ParameterName = TEXT("FrameColor");
	FrameColor->DefaultValue = FLinearColor(0.12f, 0.12f, 0.13f);
	FrameColor->Group = TEXT("Window");
	FrameColor->UpdateParameterGuid(true, false);
	UMaterialExpressionScalarParameter* FrameRough = AddWindowScalar(Material, TEXT("FrameRough"), 0.4f, 3100);
	UMaterialExpressionScalarParameter* FrameMetal = AddWindowScalar(Material, TEXT("FrameMetal"), 0.6f, 3150);
	UMaterialExpressionScalarParameter* BandShade = AddWindowScalar(Material, TEXT("BandShade"), 0.55f, 3200);
	UMaterialExpressionVectorParameter* FasciaColor = AddExpression<UMaterialExpressionVectorParameter>(Material, -900, 3250);
	FasciaColor->ParameterName = TEXT("FasciaColor");
	FasciaColor->DefaultValue = FLinearColor(0.06f, 0.065f, 0.07f);
	FasciaColor->Group = TEXT("Window");
	FasciaColor->UpdateParameterGuid(true, false);
	UMaterialExpressionScalarParameter* FasciaRough = AddWindowScalar(Material, TEXT("FasciaRough"), 0.45f, 3300);

	UMaterialExpressionCustom* Compose = AddExpression<UMaterialExpressionCustom>(Material, 0, 0);
	Compose->Description = TEXT("FTWindowCompose");
	Compose->OutputType = CMOT_Float3;
	Compose->Code = bR5 ? FT::ToFString(ftcore::windowFacadeComposeHlsl()) : FString(kWindowComposeCode);
	Compose->Inputs.Reset();
	AddInputFrom(Compose, TEXT("Win"), Win, 0);
	AddInputFrom(Compose, TEXT("Curtain"), Win, 1);
	AddInputFrom(Compose, TEXT("Emit"), Win, 2);
	AddInputFrom(Compose, TEXT("WallAlbedo"), Wall.Custom, 0);
	AddInputFrom(Compose, TEXT("WallNormal"), Wall.Custom, 1);
	AddInputFrom(Compose, TEXT("WallRough"), Wall.Custom, 2);
	AddInputFrom(Compose, TEXT("WallMetal"), Wall.Custom, 3);
	AddInputFrom(Compose, TEXT("WallAO"), Wall.Custom, 4);
	AddInput(Compose, TEXT("VertexNormal"), Wall.VertexNormal);
	AddInput(Compose, TEXT("GlassBase"), GlassBase);
	AddInput(Compose, TEXT("GlassRough"), GlassRough);
	AddInput(Compose, TEXT("CurtainAlbedo"), CurtainAlbedo);
	AddInput(Compose, TEXT("FrameColor"), FrameColor);
	AddInput(Compose, TEXT("FrameRough"), FrameRough);
	AddInput(Compose, TEXT("FrameMetal"), FrameMetal);
	AddInput(Compose, TEXT("BandShade"), BandShade);
	AddInput(Compose, TEXT("FasciaColor"), FasciaColor);
	AddInput(Compose, TEXT("FasciaRough"), FasciaRough);
	if (bR5)
	{
		AddR5ComposeInputs(Compose, Face, Data[FTScene::WindowDataTint], Data[FTScene::WindowDataRoof]);
	}
	AddOutput(Compose, TEXT("NormalOut"), CMOT_Float3);
	AddOutput(Compose, TEXT("RoughOut"), CMOT_Float1);
	AddOutput(Compose, TEXT("MetalOut"), CMOT_Float1);
	AddOutput(Compose, TEXT("AOOut"), CMOT_Float1);
	AddOutput(Compose, TEXT("EmissiveOut"), CMOT_Float3);
	Compose->RebuildOutputs();

	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Compose);
	Inputs->Normal.Connect(1, Compose);
	Inputs->Roughness.Connect(2, Compose);
	Inputs->Metallic.Connect(3, Compose);
	Inputs->AmbientOcclusion.Connect(4, Compose);
	Inputs->EmissiveColor.Connect(5, Compose);

	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	return Material;
}

// An instance of the window master with the values Source (a surface's instance of M_WorldSurfaceTex) sets, and its
// window width in metres (one a finish, so a building never mixes sizes).
UMaterialInstanceConstant* BuildWindowInstance(const FString& Path, UMaterial* Master, const UMaterialInstanceConstant* Source,
	float WindowWidth)
{
	UPackage* Package = CreatePackage(*Path);
	UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, *FPackageName::GetShortName(Path),
		RF_Public | RF_Standalone | RF_Transactional);
	Mic->SetParentEditorOnly(Master);
	for (const FTextureParameterValue& V : Source->TextureParameterValues)
	{
		Mic->SetTextureParameterValueEditorOnly(V.ParameterInfo, V.ParameterValue);
	}
	for (const FScalarParameterValue& V : Source->ScalarParameterValues)
	{
		Mic->SetScalarParameterValueEditorOnly(V.ParameterInfo, V.ParameterValue);
	}
	for (const FVectorParameterValue& V : Source->VectorParameterValues)
	{
		Mic->SetVectorParameterValueEditorOnly(V.ParameterInfo, V.ParameterValue);
	}
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("WindowWidth")), WindowWidth);
	Mic->PostEditChange();
	FAssetRegistryModule::AssetCreated(Mic);
	return Mic;
}

// The street lamps' heads (FTScene::NightLampMaterial): a pale enamel shade by day; by night (the window collection's Night)
// a warm glow of LampColor x LampStrength, so a lamp reads lit from the street even where its light falls on nothing.
UMaterial* BuildNightLamp(UMaterialParameterCollection* Collection)
{
	UMaterial* Material = NewMaster(FTScene::NightLampMaterial);
	UMaterialExpressionVectorParameter* Base = AddExpression<UMaterialExpressionVectorParameter>(Material, -900, 0);
	Base->ParameterName = TEXT("LampBase");
	Base->DefaultValue = FLinearColor(0.8f, 0.78f, 0.72f);
	Base->Group = TEXT("Lamp");
	Base->UpdateParameterGuid(true, false);
	UMaterialExpressionScalarParameter* Rough = AddScalar(Material, TEXT("LampRough"), 0.3f, 100);
	Rough->Group = TEXT("Lamp");
	UMaterialExpressionVectorParameter* Color = AddExpression<UMaterialExpressionVectorParameter>(Material, -900, 200);
	Color->ParameterName = TEXT("LampColor");
	Color->DefaultValue = FLinearColor(1.f, 0.55f, 0.2f);
	Color->Group = TEXT("Lamp");
	Color->UpdateParameterGuid(true, false);
	UMaterialExpressionScalarParameter* Strength = AddScalar(Material, TEXT("LampStrength"), 8.f, 300);
	Strength->Group = TEXT("Lamp");
	UMaterialExpressionCollectionParameter* Night = AddExpression<UMaterialExpressionCollectionParameter>(Material, -900, 400);
	Night->Collection = Collection;
	Night->ParameterName = kWindowNight;
	Night->ParameterId = Collection->GetParameterId(kWindowNight);

	UMaterialExpressionCustom* Glow = AddExpression<UMaterialExpressionCustom>(Material, -500, 200);
	Glow->Description = TEXT("FTNightLampGlow");
	Glow->OutputType = CMOT_Float3;
	Glow->Code = TEXT("return LampColor.rgb * LampStrength * Night;");
	Glow->Inputs.Reset();
	AddInput(Glow, TEXT("LampColor"), Color);
	AddInput(Glow, TEXT("LampStrength"), Strength);
	AddInput(Glow, TEXT("Night"), Night);

	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Base);
	Inputs->Roughness.Connect(0, Rough);
	Inputs->EmissiveColor.Connect(0, Glow);
	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	return Material;
}

// -Windows -Atlas=<dir> [-Prefix=<world>_] [-FacadeR5]: the window atlases from <dir>/rooms_atlas.png and curtains_atlas.png, the
// collection, the master, an instance per building surface of the world (FTScene::WindowInstancePackage(prefix + id)),
// each copying that surface's instance with one of kWindowWidths, and the street lamps' heads (FTScene::NightLampMaterial).
// Only those packages are replaced. -FacadeR5 writes the round 5 master (ftcore/WindowFacade.h) at the same path; without
// it the master is r4's, byte for byte.
int32 BuildWindows(const FString& Params)
{
	FString Atlas;
	if (!FParse::Value(*Params, TEXT("Atlas="), Atlas, false) || Atlas.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSurfaces -Windows: pass -Atlas=<dir>, the folder with rooms_atlas.png and curtains_atlas.png."));
		return 1;
	}
	FString Prefix = TEXT("solhaven_");
	FParse::Value(*Params, TEXT("Prefix="), Prefix, false);
	const FString RoomsFile = Atlas / TEXT("rooms_atlas.png");
	const FString CurtainsFile = Atlas / TEXT("curtains_atlas.png");
	for (const FString& File : {RoomsFile, CurtainsFile})
	{
		if (!IFileManager::Get().FileExists(*File))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSurfaces -Windows: %s is missing."), *File);
			return 1;
		}
	}

	// The building surfaces (two finishes may share one), each with its surface's instance.
	TArray<FString> Ids;
	for (size_t S = 0; S < ftcore::kMassingLookSurfaceCount; ++S)
	{
		const ftcore::MassingLookSurface Surface = static_cast<ftcore::MassingLookSurface>(S);
		if (ftcore::massingLookIsBuilding(Surface))
		{
			Ids.AddUnique(Prefix + FT::ToFString(ftcore::massingLookSurfaceId(Surface)));
		}
	}
	TArray<UMaterialInstanceConstant*> Sources;
	for (const FString& Id : Ids)
	{
		const FString Path = FTScene::SurfaceInstancePackage(Id);
		UMaterialInstanceConstant* Source = LoadObject<UMaterialInstanceConstant>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)));
		if (!Source)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSurfaces -Windows: %s is missing; build the surfaces first."), *Path);
			return 1;
		}
		Sources.Add(Source);
	}
	UTexture* Defaults[kMapCount] = {};
	const FName MapNames[kMapCount] = {TEXT("AlbedoTex"), TEXT("NormalTex"), TEXT("ArmTex")};
	for (int32 M = 0; M < kMapCount; ++M)
	{
		if (!Sources[0]->GetTextureParameterValue(FMaterialParameterInfo(MapNames[M]), Defaults[M]) || !Defaults[M])
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSurfaces -Windows: %s has no %s."), *Ids[0], *MapNames[M].ToString());
			return 1;
		}
	}

	// Start from nothing: replace the windows' files up front, before anything loads the old assets.
	TArray<FString> Packages = {FTScene::WindowMaterial, FTScene::WindowCollection, FTScene::WindowRoomsTexture, FTScene::WindowCurtainsTexture,
		FTScene::NightLampMaterial};
	for (const FString& Id : Ids)
	{
		Packages.Add(FTScene::WindowInstancePackage(Id));
	}
	for (const FString& Package : Packages)
	{
		const FString File = PackageFile(Package);
		if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSurfaces -Windows: couldn't replace %s."), *File);
			return 1;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
	}

	UTexture2D* Rooms = ImportPng(RoomsFile, FTScene::WindowRoomsTexture, 4096, 4096, false);
	UTexture2D* Curtains = ImportPng(CurtainsFile, FTScene::WindowCurtainsTexture, 4096, 512, true);
	if (!Rooms || !Curtains)
	{
		return 1;
	}
	UMaterialParameterCollection* Collection = BuildWindowCollection();
	const bool bR5 = FParse::Param(*Params, TEXT("FacadeR5"));
	UMaterial* Master = BuildWindowMaster(MakeArrayView(Defaults), Rooms, Curtains, Collection, bR5);
	UMaterial* Lamp = BuildNightLamp(Collection);
	// The finishes take the widths in turn, so neighbouring finishes differ and every width is used.
	TArray<UMaterialInstanceConstant*> Instances;
	TArray<FString> Widths;
	for (int32 I = 0; I < Ids.Num(); ++I)
	{
		const float Width = kWindowWidths[I % static_cast<int32>(UE_ARRAY_COUNT(kWindowWidths))];
		Instances.Add(BuildWindowInstance(FTScene::WindowInstancePackage(Ids[I]), Master, Sources[I], Width));
		Widths.Add(FString::Printf(TEXT("%s %.2f m"), *Ids[I], Width));
	}

	FAssetCompilingManager::Get().FinishAllCompilation();
	if (FT::CompileErrors(TEXT("FTBuildSurfaces -Windows"), Master) > 0 || FT::CompileErrors(TEXT("FTBuildSurfaces -Windows"), Lamp) > 0)
	{
		return 1;
	}
	FAssetRegistryModule::AssetCreated(Master);
	FAssetRegistryModule::AssetCreated(Lamp);
	int64 Bytes = 0;
	bool bSaved = SaveAsset(Rooms, Bytes);
	bSaved = SaveAsset(Curtains, Bytes) && bSaved;
	bSaved = SaveAsset(Collection, Bytes) && bSaved;
	bSaved = SaveAsset(Master, Bytes) && bSaved;
	bSaved = SaveAsset(Lamp, Bytes) && bSaved;
	for (UMaterialInstanceConstant* Instance : Instances)
	{
		bSaved = SaveAsset(Instance, Bytes) && bSaved;
	}
	if (!bSaved)
	{
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildSurfaces -Windows: saved the atlases, %s, %s, %s and %d instances (%s) in %.1f MB."),
		FTScene::WindowCollection, FTScene::WindowMaterial, FTScene::NightLampMaterial, Instances.Num(), *FString::Join(Widths, TEXT(", ")),
		Bytes / (1024.0 * 1024.0));
	return 0;
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
	if (FParse::Param(*Params, TEXT("Windows")))
	{
		return FTSurfaceBuild::BuildWindows(Params);
	}
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

	// -Only=<id,...> rebuilds those surfaces and keeps the rest, the master included but for its default textures, which
	// follow the table's first surface.
	const int32 SurfaceCount = static_cast<int32>(Table.surfaces.size());
	TArray<int32> Chosen;
	FString OnlyList;
	const bool bOnly = FParse::Value(*Params, TEXT("Only="), OnlyList, false) && !OnlyList.IsEmpty();
	if (bOnly)
	{
		TArray<FString> Ids;
		OnlyList.ParseIntoArray(Ids, TEXT(","), true);
		for (const FString& Id : Ids)
		{
			int32 Found = INDEX_NONE;
			for (int32 S = 0; S < SurfaceCount && Found == INDEX_NONE; ++S)
			{
				Found = FT::ToFString(Table.surfaces[S].id) == Id ? S : INDEX_NONE;
			}
			if (Found == INDEX_NONE)
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSurfaces: -Only names %s, which Data/Field/surfaces.json doesn't list."),
					*Id);
				return 1;
			}
			Chosen.AddUnique(Found);
		}
	}
	else
	{
		for (int32 S = 0; S < SurfaceCount; ++S)
		{
			Chosen.Add(S);
		}
	}

	// Every file first: nothing changes if one is missing.
	int32 Missing = 0;
	for (const int32 S : Chosen)
	{
		const ftcore::SurfaceSpec& Spec = Table.surfaces[S];
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
	for (const int32 S : Chosen)
	{
		const ftcore::SurfaceSpec& Spec = Table.surfaces[S];
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
	if (!bOnly)
	{
		Packages.Add(FTScene::SurfaceTexMaterial);
	}
	for (const int32 S : Chosen)
	{
		const FString Id = FT::ToFString(Table.surfaces[S].id);
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

	// Textures holds each chosen surface's maps in turn.
	TArray<UTexture*> Textures;
	for (const int32 S : Chosen)
	{
		const ftcore::SurfaceSpec& Spec = Table.surfaces[S];
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
	UMaterial* Master = nullptr;
	bool bMasterChanged = true;
	if (!bOnly)
	{
		Master = FTSurfaceBuild::BuildMaster(MakeArrayView(Textures.GetData(), FTSurfaceBuild::kMapCount));
	}
	else
	{
		// Loaded only now: loaded before the import, it would pull in old textures the new ones collide with by path.
		Master = LoadObject<UMaterial>(nullptr,
			*(FString(FTScene::SurfaceTexMaterial) + TEXT(".") + FPackageName::GetShortName(FTScene::SurfaceTexMaterial)));
		if (!Master)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSurfaces: %s is missing; build every surface (no -Only) first."),
				FTScene::SurfaceTexMaterial);
			return 1;
		}
		UTexture* Defaults[FTSurfaceBuild::kMapCount] = {};
		const int32 First = Chosen.Find(0);
		const FString FirstId = FT::ToFString(Table.surfaces[0].id);
		for (int32 M = 0; M < FTSurfaceBuild::kMapCount; ++M)
		{
			const FString Path = FTSurfaceBuild::TexturePackage(FirstId, FTSurfaceBuild::kMaps[M]);
			Defaults[M] = First != INDEX_NONE ? Textures[First * FTSurfaceBuild::kMapCount + M]
											  : LoadObject<UTexture>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)));
			if (!Defaults[M])
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSurfaces: %s is missing; add %s to -Only."), *Path, *FirstId);
				return 1;
			}
		}
		if (!FTSurfaceBuild::SetDefaults(Master, MakeArrayView(Defaults), bMasterChanged))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSurfaces: %s lacks a texture parameter; build every surface (no -Only)."),
				FTScene::SurfaceTexMaterial);
			return 1;
		}
	}
	TArray<UMaterialInstanceConstant*> Instances;
	for (int32 C = 0; C < Chosen.Num(); ++C)
	{
		const ftcore::SurfaceSpec& Spec = Table.surfaces[Chosen[C]];
		Instances.Add(FTSurfaceBuild::BuildInstance(FTScene::SurfaceInstancePackage(FT::ToFString(Spec.id)), Master,
			MakeArrayView(Textures.GetData() + C * FTSurfaceBuild::kMapCount, FTSurfaceBuild::kMapCount), Spec.tileCm,
			Spec.roughness));
	}

	FAssetCompilingManager::Get().FinishAllCompilation();
	if (FT::CompileErrors(TEXT("FTBuildSurfaces"), Master) > 0)
	{
		return 1;
	}
	if (!bOnly)
	{
		FAssetRegistryModule::AssetCreated(Master);
	}

	// The master is saved only when it changed, so a rebuild of a few surfaces leaves it untouched.
	int64 Bytes = 0;
	bool bSaved = !bMasterChanged || FTSurfaceBuild::SaveAsset(Master, Bytes);
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
	const FString MasterNote = bMasterChanged ? FString::Printf(TEXT(" and %s"), FTScene::SurfaceTexMaterial) : FString();
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildSurfaces: saved %d surfaces (%d textures, %d instances%s) in %.1f MB%s."),
		Instances.Num(), Textures.Num(), Instances.Num(), *MasterNote, Bytes / (1024.0 * 1024.0),
		bOnly ? TEXT("; the others are as they were") : TEXT(""));
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildSurfaces needs an editor build."));
	return 1;
#endif
}
