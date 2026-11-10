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

// The wall, or an opening over it: glass shows the room (a share of it as albedo under the light, a share as its own
// light, plus the night glow), the frame its own finish.
const TCHAR* const kWindowComposeCode = TEXT(R"HLSL(bool Glass = Win.a > 0.5 && Win.a < 1.5;
bool Frame = Win.a > 1.5;
NormalOut = (Glass || Frame) ? normalize(VertexNormal) : WallNormal;
RoughOut = Glass ? GlassRough : (Frame ? FrameRough : WallRough);
MetalOut = Glass ? 0.0 : (Frame ? FrameMetal : WallMetal);
AOOut = (Glass || Frame) ? 1.0 : WallAO;
EmissiveOut = Glass ? Win.rgb * RoomDay + Glow : float3(0.0, 0.0, 0.0);
return Glass ? Win.rgb * GlassBase : (Frame ? FrameColor.rgb : WallAlbedo);)HLSL");

// The box data the window master reads, in custom primitive data order (FTScene::WindowData*).
const TCHAR* const kWindowDataNames[FTScene::WindowDataCount] = {TEXT("Seed"), TEXT("StreetBits"), TEXT("Base0"), TEXT("Base1"),
	TEXT("Base2"), TEXT("Base3")};
constexpr const TCHAR* kWindowNight = TEXT("Night");

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

// The windows' collection: Night, 0 by day to 1 by night. Its id comes from its name, so a rebuild changes no id.
UMaterialParameterCollection* BuildWindowCollection()
{
	UPackage* Package = CreatePackage(FTScene::WindowCollection);
	UMaterialParameterCollection* Collection = NewObject<UMaterialParameterCollection>(Package,
		FName(FPackageName::GetShortName(Package->GetName())), RF_Public | RF_Standalone | RF_Transactional);
	FCollectionScalarParameter& Night = Collection->ScalarParameters.AddDefaulted_GetRef();
	Night.ParameterName = kWindowNight;
	Night.DefaultValue = 0.f;
	Night.Id = FGuid::NewDeterministicGuid(FString(TEXT("FTWindows.")) + kWindowNight);
	Collection->PostEditChange();
	FAssetRegistryModule::AssetCreated(Collection);
	return Collection;
}

// The window master: the photographed surface (its parameters as M_WorldSurfaceTex's, so an instance copies a surface's
// values across) with WindowInterior's openings cut into its walls.
UMaterial* BuildWindowMaster(TArrayView<UTexture* const> Defaults, UTexture* Rooms, UTexture* Curtains, UMaterialParameterCollection* Collection)
{
	UMaterial* Material = NewMaster(FTScene::WindowMaterial);
	const FSurfaceNode Wall = AddSurfaceNode(Material, Defaults);

	UMaterialExpressionScalarParameter* Data[FTScene::WindowDataCount] = {};
	for (int32 D = 0; D < FTScene::WindowDataCount; ++D)
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
	Face->Code = kWindowFaceCode;
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
	Face->RebuildOutputs();

	UMaterialExpressionScalarParameter* LitShare = AddWindowScalar(Material, TEXT("LitShare"), 0.35f, 1900);
	UMaterialExpressionScalarParameter* CurtainShare = AddWindowScalar(Material, TEXT("CurtainShare"), 0.5f, 2000);
	UMaterialExpressionScalarParameter* ShopLit = AddWindowScalar(Material, TEXT("ShopLit"), 0.8f, 2100);
	UMaterialExpressionScalarParameter* GlowScale = AddWindowScalar(Material, TEXT("GlowScale"), 1.f, 2200);
	UMaterialExpressionCollectionParameter* Night = AddExpression<UMaterialExpressionCollectionParameter>(Material, -900, 2300);
	Night->Collection = Collection;
	Night->ParameterName = kWindowNight;
	Night->ParameterId = Collection->GetParameterId(kWindowNight);
	UMaterialExpressionTextureObjectParameter* RoomsTex = AddTexture(Material, TEXT("Rooms"), Rooms, SAMPLERTYPE_Color, 2400);
	UMaterialExpressionTextureObjectParameter* CurtainsTex = AddTexture(Material, TEXT("Curtains"), Curtains, SAMPLERTYPE_Color, 2550);
	RoomsTex->Group = TEXT("Window");
	CurtainsTex->Group = TEXT("Window");

	UMaterialExpressionCustom* Win = AddExpression<UMaterialExpressionCustom>(Material, -300, 1600);
	Win->Description = TEXT("FTWindowInterior");
	Win->OutputType = CMOT_Float4;
	Win->Code = FT::ToFString(ftcore::windowInteriorHlsl(ftcore::WindowGridSpec{}));
	Win->Inputs.Reset();
	AddInputFrom(Win, TEXT("Along"), Face, 0);
	AddInputFrom(Win, TEXT("Up"), Face, 2);
	AddInputFrom(Win, TEXT("FaceW"), Face, 4);
	AddInputFrom(Win, TEXT("FaceH"), Face, 3);
	AddInput(Win, TEXT("Seed"), Data[FTScene::WindowDataSeed]);
	AddInputFrom(Win, TEXT("Street"), Face, 1);
	AddInputFrom(Win, TEXT("ViewT"), Face, 5);
	AddInput(Win, TEXT("LitShare"), LitShare);
	AddInput(Win, TEXT("CurtainShare"), CurtainShare);
	AddInput(Win, TEXT("ShopLit"), ShopLit);
	AddInput(Win, TEXT("Night"), Night);
	AddInput(Win, TEXT("GlowScale"), GlowScale);
	AddInput(Win, TEXT("Rooms"), RoomsTex);
	AddInput(Win, TEXT("Curtains"), CurtainsTex);
	AddOutput(Win, TEXT("Glow"), CMOT_Float3);
	Win->RebuildOutputs();

	UMaterialExpressionScalarParameter* GlassBase = AddWindowScalar(Material, TEXT("GlassBase"), 0.35f, 2700);
	UMaterialExpressionScalarParameter* GlassRough = AddWindowScalar(Material, TEXT("GlassRough"), 0.08f, 2800);
	UMaterialExpressionScalarParameter* RoomDay = AddWindowScalar(Material, TEXT("RoomDay"), 0.4f, 2900);
	UMaterialExpressionVectorParameter* FrameColor = AddExpression<UMaterialExpressionVectorParameter>(Material, -900, 3000);
	FrameColor->ParameterName = TEXT("FrameColor");
	FrameColor->DefaultValue = FLinearColor(0.12f, 0.12f, 0.13f);
	FrameColor->Group = TEXT("Window");
	FrameColor->UpdateParameterGuid(true, false);
	UMaterialExpressionScalarParameter* FrameRough = AddWindowScalar(Material, TEXT("FrameRough"), 0.4f, 3100);
	UMaterialExpressionScalarParameter* FrameMetal = AddWindowScalar(Material, TEXT("FrameMetal"), 0.6f, 3200);

	UMaterialExpressionCustom* Compose = AddExpression<UMaterialExpressionCustom>(Material, 0, 0);
	Compose->Description = TEXT("FTWindowCompose");
	Compose->OutputType = CMOT_Float3;
	Compose->Code = kWindowComposeCode;
	Compose->Inputs.Reset();
	AddInputFrom(Compose, TEXT("Win"), Win, 0);
	AddInputFrom(Compose, TEXT("Glow"), Win, 1);
	AddInputFrom(Compose, TEXT("WallAlbedo"), Wall.Custom, 0);
	AddInputFrom(Compose, TEXT("WallNormal"), Wall.Custom, 1);
	AddInputFrom(Compose, TEXT("WallRough"), Wall.Custom, 2);
	AddInputFrom(Compose, TEXT("WallMetal"), Wall.Custom, 3);
	AddInputFrom(Compose, TEXT("WallAO"), Wall.Custom, 4);
	AddInput(Compose, TEXT("VertexNormal"), Wall.VertexNormal);
	AddInput(Compose, TEXT("GlassBase"), GlassBase);
	AddInput(Compose, TEXT("GlassRough"), GlassRough);
	AddInput(Compose, TEXT("RoomDay"), RoomDay);
	AddInput(Compose, TEXT("FrameColor"), FrameColor);
	AddInput(Compose, TEXT("FrameRough"), FrameRough);
	AddInput(Compose, TEXT("FrameMetal"), FrameMetal);
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

// An instance of the window master with the values Source (a surface's instance of M_WorldSurfaceTex) sets.
UMaterialInstanceConstant* BuildWindowInstance(const FString& Path, UMaterial* Master, const UMaterialInstanceConstant* Source)
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
	Mic->PostEditChange();
	FAssetRegistryModule::AssetCreated(Mic);
	return Mic;
}

// -Windows -Atlas=<dir> [-Prefix=<world>_]: the window atlases from <dir>/rooms_atlas.png and curtains_atlas.png, the
// collection, the master and an instance per building surface of the world (FTScene::WindowInstancePackage(prefix + id)),
// each copying that surface's instance. Only the windows' packages are replaced.
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
	TArray<FString> Packages = {FTScene::WindowMaterial, FTScene::WindowCollection, FTScene::WindowRoomsTexture, FTScene::WindowCurtainsTexture};
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
	UMaterial* Master = BuildWindowMaster(MakeArrayView(Defaults), Rooms, Curtains, Collection);
	TArray<UMaterialInstanceConstant*> Instances;
	for (int32 I = 0; I < Ids.Num(); ++I)
	{
		Instances.Add(BuildWindowInstance(FTScene::WindowInstancePackage(Ids[I]), Master, Sources[I]));
	}

	FAssetCompilingManager::Get().FinishAllCompilation();
	if (FT::CompileErrors(TEXT("FTBuildSurfaces -Windows"), Master) > 0)
	{
		return 1;
	}
	FAssetRegistryModule::AssetCreated(Master);
	int64 Bytes = 0;
	bool bSaved = SaveAsset(Rooms, Bytes);
	bSaved = SaveAsset(Curtains, Bytes) && bSaved;
	bSaved = SaveAsset(Collection, Bytes) && bSaved;
	bSaved = SaveAsset(Master, Bytes) && bSaved;
	for (UMaterialInstanceConstant* Instance : Instances)
	{
		bSaved = SaveAsset(Instance, Bytes) && bSaved;
	}
	if (!bSaved)
	{
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildSurfaces -Windows: saved the atlases, %s, %s and %d instances (%s) in %.1f MB."),
		FTScene::WindowCollection, FTScene::WindowMaterial, Instances.Num(), *FString::Join(Ids, TEXT(", ")), Bytes / (1024.0 * 1024.0));
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
