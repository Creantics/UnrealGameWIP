#include "FTBuildGroundCommandlet.h"

#include "ForgottenThird.h"
#include "FTSceneLook.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture2D.h"
#include "FTImportMaterials.h"
#include "FTText.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "ftcore/SurfaceTable.h"
#endif

#if WITH_EDITOR
// Named, not anonymous: in a unity build the other material commandlets' own AddExpression and PackageFile share this
// translation unit.
namespace FTGroundBuild
{
// The empty road mask the master takes by default, so that the master alone draws no road.
const TCHAR* const kNoRoads = TEXT("/Game/Rendering/T_GroundNoRoads");
constexpr int32 kLayerCount = UE_ARRAY_COUNT(FTScene::GroundLayers);
constexpr int32 kMapCount = UE_ARRAY_COUNT(FTScene::SurfaceMaps);
// How the master samples each of FTScene::SurfaceMaps.
const EMaterialSamplerType kMapSamplers[kMapCount] = {SAMPLERTYPE_Color, SAMPLERTYPE_Normal, SAMPLERTYPE_Masks};

// The ground, rock and road layers (see the header), then their blend. Every texture is read with the engine's
// Texture2DSample, never Tex.Sample (see M_WorldSurfaceTex), through one of the view's shared samplers, which filter as
// the world's textures do: ten textures with their own samplers, and the Landscape's, would pass the sixteen a shader
// may bind. Helpers are a struct's methods, the only functions a custom node's code, one function's body, can hold. The
// code is three literals, each well under the compiler's limit for one.
const TCHAR* const kGroundCode = TEXT(R"HLSL(struct FGroundLayers
{
	// Value noise on the integer lattice, hashed as M_WorldSurfaceTex's mottle is: 0..1, smooth between lattice points.
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
	// Two octaves of it over the ground, Scale centimetres across, Seed giving each use a pattern of its own.
	float Mottle(float2 P, float Scale, float Seed)
	{
		float2 Q = P / max(Scale, 1.0) + Seed * float2(17.31, -9.77);
		return (2.0 / 3.0) * Noise(Q) + (1.0 / 3.0) * Noise(Q * 2.03 + 3.7);
	}
	// A plausible albedo: no channel over 0.8 and none under 0.03.
	float3 Hold(float3 Color)
	{
		Color *= min(1.0, 0.8 / max(max(Color.r, Color.g), max(Color.b, 1e-4)));
		return max(Color, 0.03);
	}
	// How much of the top layer shows where the weights alone would show Weight of it. Each layer's height stands on its
	// own share of the weight, and wherever one stands clear of the other by Depth it alone shows, so the top layer
	// breaks through first at its high points and fills its low ones last. With both heights under 1 - Depth, Weight 0
	// shows none of it and 1 all of it.
	float HeightBlend(float Weight, float LowHeight, float TopHeight, float Depth)
	{
		float Low = LowHeight + (1.0 - Weight);
		float Top = TopHeight + Weight;
		float Cut = max(Low, Top) - Depth;
		float LowShare = max(Low - Cut, 0.0);
		float TopShare = max(Top - Cut, 0.0);
		return TopShare / max(LowShare + TopShare, 1e-5);
	}
};
FGroundLayers Layers;
float2 P = WorldPos.xy;
float3 Up = normalize(VertexNormal);
// Far: 0 out to FarStartCm from the eye and 1 from FarEndCm. Over it the detail that would tile or glitter far off gives
// way: the bumps ease to FarNormalStrength of their strength.
float Far = smoothstep(FarStartCm, max(FarEndCm, FarStartCm + 1.0), EyeDepth);
float Strength = NormalStrength * lerp(1.0, FarNormalStrength, Far);

// Ground: laid flat and drawn twice, the second copy turned 0.6 radians and 1.31 times the size, a broad mottle
// choosing between them.
const float Cos06 = 0.8253356;
const float Sin06 = 0.5646425;
float2 UvA = P / max(GroundTileCm, 1.0);
float2 UvB = float2(Cos06 * P.x + Sin06 * P.y, Cos06 * P.y - Sin06 * P.x) / max(GroundTileCm * 1.31, 1.0) + float2(0.37, 0.71);
float Turned = smoothstep(0.35, 0.65, Layers.Mottle(P, 1800.0, 1.0));
float3 GroundColor = lerp(
	Texture2DSample(GroundAlbedo, GetMaterialSharedSampler(GroundAlbedoSampler, View.MaterialTextureBilinearWrapedSampler), UvA).rgb,
	Texture2DSample(GroundAlbedo, GetMaterialSharedSampler(GroundAlbedoSampler, View.MaterialTextureBilinearWrapedSampler), UvB).rgb,
	Turned);
float3 GroundPack = lerp(
	Texture2DSample(GroundArm, GetMaterialSharedSampler(GroundArmSampler, View.MaterialTextureBilinearWrapedSampler), UvA).rgb,
	Texture2DSample(GroundArm, GetMaterialSharedSampler(GroundArmSampler, View.MaterialTextureBilinearWrapedSampler), UvB).rgb,
	Turned);
// A DirectX normal map leans along +u with x and along +v (image-down) with y; the turned copy's u and v run along
// (cos, sin) and (-sin, cos).
float3 NA = UnpackNormalMap(
	Texture2DSample(GroundNormal, GetMaterialSharedSampler(GroundNormalSampler, View.MaterialTextureBilinearWrapedSampler), UvA)).xyz;
float3 NB = UnpackNormalMap(
	Texture2DSample(GroundNormal, GetMaterialSharedSampler(GroundNormalSampler, View.MaterialTextureBilinearWrapedSampler), UvB)).xyz;
float3 GroundLean =
	lerp(float3(NA.x, NA.y, 0.0), float3(Cos06 * NB.x - Sin06 * NB.y, Sin06 * NB.x + Cos06 * NB.y, 0.0), Turned);
float3 GroundN = normalize(Strength * GroundLean + lerp(NA.z, NB.z, Turned) * Up);
// Far off, a third copy, FarScale times the size and turned 1.9 radians, takes FarBlend of the colour and the packed
// maps, so that the near copies' repeats don't line up into a pattern seen from afar. Its bumps are left out: far off
// they have eased anyway.
const float Cos19 = -0.3232896;
const float Sin19 = 0.9463001;
float2 UvF = float2(Cos19 * P.x + Sin19 * P.y, Cos19 * P.y - Sin19 * P.x) / max(GroundTileCm * FarScale, 1.0) + float2(0.13, 0.52);
float FarW = Far * saturate(FarBlend);
GroundColor = lerp(GroundColor,
	Texture2DSample(GroundAlbedo, GetMaterialSharedSampler(GroundAlbedoSampler, View.MaterialTextureBilinearWrapedSampler), UvF).rgb, FarW);
GroundPack = lerp(GroundPack,
	Texture2DSample(GroundArm, GetMaterialSharedSampler(GroundArmSampler, View.MaterialTextureBilinearWrapedSampler), UvF).rgb, FarW);
)HLSL") TEXT(R"HLSL(
// Rock: projected along the world's axes, blended by how squarely the ground meets each, image-down pointing down the
// world on a side face.
float3 Q = WorldPos / max(RockTileCm, 1.0);
float3 Blend = pow(abs(Up), 4.0);
Blend /= max(Blend.x + Blend.y + Blend.z, 1e-5);
float2 UvX = float2(Q.y, -Q.z);
float2 UvY = float2(Q.x, -Q.z);
float2 UvZ = float2(Q.x, Q.y);
float3 RockColor =
	Blend.x * Texture2DSample(RockAlbedo, GetMaterialSharedSampler(RockAlbedoSampler, View.MaterialTextureBilinearWrapedSampler), UvX).rgb
	+ Blend.y * Texture2DSample(RockAlbedo, GetMaterialSharedSampler(RockAlbedoSampler, View.MaterialTextureBilinearWrapedSampler), UvY).rgb
	+ Blend.z * Texture2DSample(RockAlbedo, GetMaterialSharedSampler(RockAlbedoSampler, View.MaterialTextureBilinearWrapedSampler), UvZ).rgb;
float3 RockPack =
	Blend.x * Texture2DSample(RockArm, GetMaterialSharedSampler(RockArmSampler, View.MaterialTextureBilinearWrapedSampler), UvX).rgb
	+ Blend.y * Texture2DSample(RockArm, GetMaterialSharedSampler(RockArmSampler, View.MaterialTextureBilinearWrapedSampler), UvY).rgb
	+ Blend.z * Texture2DSample(RockArm, GetMaterialSharedSampler(RockArmSampler, View.MaterialTextureBilinearWrapedSampler), UvZ).rgb;
float3 RX = UnpackNormalMap(
	Texture2DSample(RockNormal, GetMaterialSharedSampler(RockNormalSampler, View.MaterialTextureBilinearWrapedSampler), UvX)).xyz;
float3 RY = UnpackNormalMap(
	Texture2DSample(RockNormal, GetMaterialSharedSampler(RockNormalSampler, View.MaterialTextureBilinearWrapedSampler), UvY)).xyz;
float3 RZ = UnpackNormalMap(
	Texture2DSample(RockNormal, GetMaterialSharedSampler(RockNormalSampler, View.MaterialTextureBilinearWrapedSampler), UvZ)).xyz;
float3 RockLean =
	float3(Blend.y * RY.x + Blend.z * RZ.x, Blend.x * RX.x + Blend.z * RZ.y, -(Blend.x * RX.y + Blend.y * RY.y));
float3 RockN = normalize(Strength * RockLean + (Blend.x * RX.z + Blend.y * RY.z + Blend.z * RZ.z) * Up);
// The heights the rock and the ground meet by: each surface's occlusion about its own mean (its last mip), so that a
// bright gravel and a dark rock meet as evenly as two of a kind. They reach RockBlendHeight, short enough that the
// blend's ends stay pure.
float Span = clamp(RockBlendHeight, 0.0, 0.95 - saturate(RockBlendDepth));
float GroundMean =
	Texture2DSampleLevel(GroundArm, GetMaterialSharedSampler(GroundArmSampler, View.MaterialTextureBilinearWrapedSampler), float2(0.5, 0.5), 15.0).r;
float RockMean =
	Texture2DSampleLevel(RockArm, GetMaterialSharedSampler(RockArmSampler, View.MaterialTextureBilinearWrapedSampler), float2(0.5, 0.5), 15.0).r;
float GroundH = Span * saturate(0.5 + 2.0 * (GroundPack.r - GroundMean));
float RockH = Span * saturate(0.5 + 2.0 * (RockPack.r - RockMean));
// Rock takes the ground over between slopes of about 0.75 and 0.95 (37 to 44 degrees), the line wandering by noise.
// From RockAltitudeCm up, over RockAltitudeRunCm and up to half as much again by noise, ground gentler by 0.4 joins it,
// so that the high mountainsides turn to rock and only their gentle shoulders keep the ground; nothing under
// RockAltitudeCm does. Within the band the rock breaks through at its high points and the ground keeps to its low ones.
float Nz = saturate(Up.z);
float Slope = sqrt(saturate(1.0 - Nz * Nz)) / max(Nz, 1e-4);
float Run = max(RockAltitudeRunCm, 1.0);
float Lift = smoothstep(RockAltitudeCm, RockAltitudeCm + Run, WorldPos.z - 0.5 * Run * Layers.Mottle(P, 4000.0, 8.0));
float SlopeW = smoothstep(0.55, 1.15, Slope + 0.4 * Lift + (Layers.Mottle(P, 600.0, 5.0) - 0.5) * 0.2);
float RockW = Layers.HeightBlend(SlopeW, GroundH, RockH, max(RockBlendDepth, 0.01));
)HLSL") TEXT(R"HLSL(
// Road: laid flat inside each road's edge. The mask is 255 within a metre of a road's centre line and falls in a
// straight line to 0 five metres out, so it reads back as the distance; the edge lies 3 m out, give or take 40 cm.
float2 UvR = P / max(RoadTileCm, 1.0);
float3 RoadColor =
	Texture2DSample(RoadAlbedo, GetMaterialSharedSampler(RoadAlbedoSampler, View.MaterialTextureBilinearWrapedSampler), UvR).rgb;
float3 RoadPack =
	Texture2DSample(RoadArm, GetMaterialSharedSampler(RoadArmSampler, View.MaterialTextureBilinearWrapedSampler), UvR).rgb;
float3 NR = UnpackNormalMap(
	Texture2DSample(RoadNormal, GetMaterialSharedSampler(RoadNormalSampler, View.MaterialTextureBilinearWrapedSampler), UvR)).xyz;
float3 RoadN = normalize(Strength * float3(NR.x, NR.y, 0.0) + NR.z * Up);
float MaskV = Texture2DSample(RoadMask, GetMaterialSharedSampler(RoadMaskSampler, View.MaterialTextureBilinearClampedSampler),
	P / max(MaskSideCm, 1.0) + 0.5).r;
float DistCm = 500.0 - 400.0 * MaskV;
float EdgeCm = 300.0 + (Layers.Mottle(P, 250.0, 6.0) - 0.5) * 80.0;
float Soft = 0.5 * max(RoadEdgeCm, 1.0);
float RoadW = 1.0 - smoothstep(-Soft, Soft, DistCm - EdgeCm);

// Rock over the ground and the road over both, each layer in its tint, then the mottles and the warm and cool drift. The
// fine mottle, which far off would only speckle, fades with the detail.
float Fine = Layers.Mottle(P, VariationScale, 2.0);
float Broad = Layers.Mottle(P, MacroScale, 3.0);
float Hue = Layers.Mottle(P, MacroScale * 1.7, 4.0);
float FineVariation = Variation * (1.0 - Far);
float3 Shade = (1.0 + FineVariation * (2.0 * Fine - 1.0)) * (1.0 + MacroVariation * (2.0 * Broad - 1.0))
	* (1.0 + HueDrift * (2.0 * Hue - 1.0) * float3(1.0, 0.25, -0.75));
float3 Color = lerp(lerp(GroundColor * GroundTint.rgb, RockColor * RockTint.rgb, RockW), RoadColor * RoadTint.rgb, RoadW) * Shade;
NormalOut = normalize(lerp(lerp(GroundN, RockN, RockW), RoadN, RoadW));
// A surface whose packed map has an empty roughness channel gets a constant from surfaces.json instead.
float GroundRough = GroundRoughness >= 0.0 ? GroundRoughness : GroundPack.g;
float RockRough = RockRoughness >= 0.0 ? RockRoughness : RockPack.g;
float RoadRough = RoadRoughness >= 0.0 ? RoadRoughness : RoadPack.g;
float RoughVar = 1.0 + 0.5 * FineVariation * (2.0 * Layers.Mottle(P, VariationScale, 7.0) - 1.0);
RoughOut = saturate(lerp(lerp(GroundRough, RockRough, RockW), RoadRough, RoadW) * RoughVar);
float3 Pack = lerp(lerp(GroundPack, RockPack, RockW), RoadPack, RoadW);
MetalOut = Pack.b;
AOOut = Pack.r;
return Layers.Hold(Color);
)HLSL");

FString PackageFile(const FString& Package)
{
	return FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
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

UMaterialExpressionScalarParameter* AddScalar(UMaterial* Material, FName Name, float Value, FName Group, int32 Y)
{
	UMaterialExpressionScalarParameter* P = AddExpression<UMaterialExpressionScalarParameter>(Material, -900, Y);
	P->ParameterName = Name;
	P->DefaultValue = Value;
	P->Group = Group;
	P->UpdateParameterGuid(true, false);
	return P;
}

UMaterialExpressionVectorParameter* AddVector(UMaterial* Material, FName Name, const FLinearColor& Value, FName Group, int32 Y)
{
	UMaterialExpressionVectorParameter* P = AddExpression<UMaterialExpressionVectorParameter>(Material, -900, Y);
	P->ParameterName = Name;
	P->DefaultValue = Value;
	P->Group = Group;
	P->UpdateParameterGuid(true, false);
	return P;
}

UMaterialExpressionTextureObjectParameter* AddTexture(UMaterial* Material, FName Name, UTexture* Default,
	EMaterialSamplerType Type, FName Group, int32 Y)
{
	UMaterialExpressionTextureObjectParameter* P = AddExpression<UMaterialExpressionTextureObjectParameter>(Material, -900, Y);
	P->ParameterName = Name;
	P->Texture = Default;
	P->SamplerType = Type;
	P->Group = Group;
	P->UpdateParameterGuid(true, false);
	return P;
}

// The master, with Defaults (each layer's maps in turn) as its textures and NoRoads as its road mask.
UMaterial* BuildMaster(TConstArrayView<UTexture*> Defaults, UTexture* NoRoads)
{
	UPackage* Package = CreatePackage(FTScene::GroundMaterial);
	UMaterial* Material = NewObject<UMaterial>(Package, FName(FPackageName::GetShortName(Package->GetName())),
		RF_Public | RF_Standalone | RF_Transactional);
	// The code bends the normal in world space. A Landscape needs no usage flag: its own instances of the map's instance
	// say they draw one.
	Material->bTangentSpaceNormal = false;

	UMaterialExpressionCustom* Custom = AddExpression<UMaterialExpressionCustom>(Material, -450, 0);
	Custom->Description = TEXT("FTWorldGround");
	Custom->OutputType = CMOT_Float3;
	Custom->Code = kGroundCode;
	Custom->Inputs.Reset();
	int32 Y = -1600;
	for (int32 L = 0; L < kLayerCount; ++L)
	{
		const FString Layer = FTScene::GroundLayers[L].Name;
		for (int32 M = 0; M < kMapCount; ++M)
		{
			const FName Name(*(Layer + FTScene::SurfaceMaps[M].Name));
			AddInput(Custom, Name, AddTexture(Material, Name, Defaults[L * kMapCount + M], kMapSamplers[M], *Layer, Y));
			Y += 150;
		}
		const FName Tint(*(Layer + TEXT("Tint")));
		AddInput(Custom, Tint, AddVector(Material, Tint, FLinearColor(1.f, 1.f, 1.f), *Layer, Y));
		const FName TileCm(*(Layer + TEXT("TileCm")));
		AddInput(Custom, TileCm, AddScalar(Material, TileCm, 200.f, *Layer, Y + 150));
		const FName Roughness(*(Layer + TEXT("Roughness")));
		AddInput(Custom, Roughness, AddScalar(Material, Roughness, -1.f, *Layer, Y + 250));
		Y += 350;
	}
	AddInput(Custom, TEXT("RoadMask"),
		AddTexture(Material, TEXT("RoadMask"), NoRoads, SAMPLERTYPE_LinearGrayscale, TEXT("Road"), Y));
	Y += 150;
	struct FLookScalar
	{
		const TCHAR* Name;
		float Value;
		const TCHAR* Group;
	};
	// RockAltitudeCm's default lies far over any ground, so the master alone turns no high ground to rock.
	const FLookScalar Look[] = {
		{TEXT("NormalStrength"), 1.f, TEXT("Look")},
		{TEXT("Variation"), 0.12f, TEXT("Look")},
		{TEXT("VariationScale"), 400.f, TEXT("Look")},
		{TEXT("MacroVariation"), 0.14f, TEXT("Look")},
		{TEXT("MacroScale"), 3000.f, TEXT("Look")},
		{TEXT("HueDrift"), 0.05f, TEXT("Look")},
		{TEXT("MaskSideCm"), 100000.f, TEXT("Road")},
		{TEXT("RoadEdgeCm"), 35.f, TEXT("Road")},
		{TEXT("RockBlendHeight"), 0.5f, TEXT("Rock")},
		{TEXT("RockBlendDepth"), 0.2f, TEXT("Rock")},
		{TEXT("RockAltitudeCm"), 100000.f, TEXT("Rock")},
		{TEXT("RockAltitudeRunCm"), 4000.f, TEXT("Rock")},
		{TEXT("FarStartCm"), 3000.f, TEXT("Far")},
		{TEXT("FarEndCm"), 12000.f, TEXT("Far")},
		{TEXT("FarScale"), 5.f, TEXT("Far")},
		{TEXT("FarBlend"), 0.6f, TEXT("Far")},
		{TEXT("FarNormalStrength"), 0.4f, TEXT("Far")},
	};
	for (const FLookScalar& Scalar : Look)
	{
		AddInput(Custom, Scalar.Name, AddScalar(Material, Scalar.Name, Scalar.Value, Scalar.Group, Y));
		Y += 100;
	}
	AddInput(Custom, TEXT("WorldPos"), AddExpression<UMaterialExpressionWorldPosition>(Material, -900, Y));
	AddInput(Custom, TEXT("VertexNormal"), AddExpression<UMaterialExpressionVertexNormalWS>(Material, -900, Y + 100));
	AddInput(Custom, TEXT("EyeDepth"), AddExpression<UMaterialExpressionPixelDepth>(Material, -900, Y + 200));
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
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildGround: couldn't save %s."), *File);
		return false;
	}
	Bytes += IFileManager::Get().FileSize(*File);
	return true;
}
} // namespace FTGroundBuild
#endif

UFTBuildGroundCommandlet::UFTBuildGroundCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildGroundCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	ftcore::SurfaceTable Table;
	if (!FTScene::LoadSurfaceTable(Table))
	{
		return 1;
	}
	// The master's defaults are the town's surfaces.
	FString Ids[FTGroundBuild::kLayerCount];
	for (int32 L = 0; L < FTGroundBuild::kLayerCount; ++L)
	{
		const ftcore::SurfaceRole Role = FTScene::GroundLayers[L].Role;
		const ftcore::SurfaceSpec* Spec = Table.surfaceFor(ftcore::WorldKind::Town, Role);
		if (!Spec)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildGround: Data/Field/surfaces.json gives the town's %s no surface."),
				*FT::ToFString(ftcore::surfaceRoleName(Role)));
			return 1;
		}
		Ids[L] = FT::ToFString(Spec->id);
	}

	// Start from nothing: replace the files up front, before anything loads the old assets.
	for (const TCHAR* Package : {FTScene::GroundMaterial, FTGroundBuild::kNoRoads})
	{
		const FString File = FTGroundBuild::PackageFile(Package);
		if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildGround: couldn't replace %s."), *File);
			return 1;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
	}

	TArray<UTexture*> Defaults;
	for (int32 L = 0; L < FTGroundBuild::kLayerCount; ++L)
	{
		for (const FTScene::FSurfaceMap& Map : FTScene::SurfaceMaps)
		{
			UTexture* Texture = FTScene::LoadSurfaceTexture(Ids[L], Map.Suffix);
			if (!Texture)
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildGround: %s is missing; run FTBuildSurfaces first."),
					*FTScene::SurfaceTexturePackage(Ids[L], Map.Suffix));
				return 1;
			}
			Defaults.Add(Texture);
		}
	}
	const uint8 Empty[4 * 4] = {};
	UTexture2D* NoRoads = FTScene::NewRoadMask(CreatePackage(FTGroundBuild::kNoRoads),
		*FPackageName::GetShortName(FTGroundBuild::kNoRoads), 4, Empty,
		RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(NoRoads);
	UMaterial* Master = FTGroundBuild::BuildMaster(Defaults, NoRoads);

	FAssetCompilingManager::Get().FinishAllCompilation();
	if (FT::CompileErrors(TEXT("FTBuildGround"), Master) > 0)
	{
		return 1;
	}
	FAssetRegistryModule::AssetCreated(Master);

	int64 Bytes = 0;
	bool bSaved = FTGroundBuild::SaveAsset(NoRoads, Bytes);
	bSaved = FTGroundBuild::SaveAsset(Master, Bytes) && bSaved;
	if (!bSaved)
	{
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildGround: saved %s and %s (%.1f KB), wearing %s, %s and %s until a map's instance says otherwise."),
		FTScene::GroundMaterial, FTGroundBuild::kNoRoads, Bytes / 1024.0, *Ids[0], *Ids[1], *Ids[2]);
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildGround needs an editor build."));
	return 1;
#endif
}
