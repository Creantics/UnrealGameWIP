#include "FTBuildVfxCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionDepthFade.h"
#include "Materials/MaterialExpressionParticleColor.h"
#include "Materials/MaterialExpressionParticleRandom.h"
#include "Materials/MaterialExpressionParticleSize.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemEmitterState.h"
#include "NiagaraTypes.h"
#include "Stateless/Modules/NiagaraStatelessModule_AccelerationForce.h"
#include "Stateless/Modules/NiagaraStatelessModule_AddVelocity.h"
#include "Stateless/Modules/NiagaraStatelessModule_CurlNoiseForce.h"
#include "Stateless/Modules/NiagaraStatelessModule_Drag.h"
#include "Stateless/Modules/NiagaraStatelessModule_GravityForce.h"
#include "Stateless/Modules/NiagaraStatelessModule_InitializeParticle.h"
#include "Stateless/Modules/NiagaraStatelessModule_ScaleColor.h"
#include "Stateless/Modules/NiagaraStatelessModule_ScaleSpriteSize.h"
#include "Stateless/Modules/NiagaraStatelessModule_ScaleSpriteSizeBySpeed.h"
#include "Stateless/Modules/NiagaraStatelessModule_ShapeLocation.h"
#include "Stateless/Modules/NiagaraStatelessModule_SolveVelocitiesAndForces.h"
#include "Stateless/Modules/NiagaraStatelessModule_SpriteFacingAndAlignment.h"
#include "Stateless/Modules/NiagaraStatelessModule_SpriteRotationRate.h"
#include "Stateless/NiagaraStatelessEmitter.h"
#include "Stateless/NiagaraStatelessModule.h"
#include "Stateless/NiagaraStatelessSpawnInfo.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "ftcore/Vfx.h"

#include <string>
#endif

#if WITH_EDITOR
namespace
{
const TCHAR* const TemplatePath = TEXT("/Niagara/DefaultAssets/Templates/Systems/FountainLightweight.FountainLightweight");
const TCHAR* const VfxRoot = TEXT("/Game/VFX");
const TCHAR* const MaterialRoot = TEXT("/Game/VFX/Materials");

FString PackageFile(const FString& Package)
{
	return FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
}

bool SaveVfxAsset(UPackage* Package, UObject* Asset)
{
	FAssetRegistryModule::AssetCreated(Asset);
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	const FString File = PackageFile(Package->GetName());
	if (!UPackage::SavePackage(Package, Asset, *File, Args))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: couldn't save %s."), *File);
		return false;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildVfx: saved %s."), *File);
	return true;
}

bool RemoveFile(const FString& File)
{
	if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: couldn't delete %s."), *File);
		return false;
	}
	return true;
}

// ---- sprite materials ------------------------------------------------------------------------------------

// The procedural sprites newengine draws when an emitter has no texture: a soft round glow, a spark with a hot core, a
// hard ring (a flat mark on the ground, faintly filled), and an angular chip (an uneven six-sided flake, shaded in three
// facets). Puff (an eroded cloud) uses the soft one.
const TCHAR* const SoftMaskCode = TEXT(
	"float2 d = UV * 2.0 - 1.0;\n"
	"float m = 1.0 - saturate(dot(d, d));\n"
	"return m * m;\n");
const TCHAR* const SparkMaskCode = TEXT(
	"float2 d = UV * 2.0 - 1.0;\n"
	"float r2 = saturate(dot(d, d));\n"
	"return saturate(exp(-r2 * 18.0) + 0.45 * pow(1.0 - r2, 3.0));\n");
const TCHAR* const RingMaskCode = TEXT(
	"float r = length(UV * 2.0 - 1.0);\n"
	"float band = 1.0 - smoothstep(0.0, 0.09, abs(r - 0.84));\n"
	"float fill = 0.15 * (1.0 - smoothstep(0.70, 0.80, r));\n"
	"return saturate(max(band, fill)) * (1.0 - smoothstep(0.97, 1.0, r));\n");
const TCHAR* const ChipMaskCode = TEXT(
	"float2 d = UV * 2.0 - 1.0;\n"
	"float k = dot(d, float2(0.97, 0.24)) / 0.80;\n"
	"k = max(k, dot(d, float2(0.31, 0.95)) / 0.62);\n"
	"k = max(k, dot(d, float2(-0.62, 0.78)) / 0.85);\n"
	"k = max(k, dot(d, float2(-0.99, -0.12)) / 0.70);\n"
	"k = max(k, dot(d, float2(-0.40, -0.92)) / 0.88);\n"
	"k = max(k, dot(d, float2(0.60, -0.80)) / 0.66);\n"
	"return 1.0 - smoothstep(0.97, 1.0, k);\n");
// A chip's three facets about an off-centre ridge point: one lit, one mid, one in shade.
const TCHAR* const ChipShadeCode = TEXT(
	"float2 p = UV * 2.0 - 1.0 - float2(0.12, 0.18);\n"
	"float a = atan2(p.y, p.x);\n"
	"float s = a < -1.2 ? 0.65 : (a < 1.6 ? 1.15 : 0.85);\n");

// The liquid sprites. Each particle's random number (R) picks its silhouette, and its alpha (Color.a) erodes it: the
// thin parts go first, so a fading blot or splash breaks up and shrinks instead of turning to a translucent ghost. Both
// set mask (the eroded shape, with a soft edge) and gloss (a wet sheen added to the colour). sd is a signed
// distance in the sprite's -1..1 square, positive inside.
// Blot: six wet silhouettes (a splat with satellite drops, a falling drop, a torn shard, a crescent smear, a three-lobed
// blob, a flick), a rim light on the upper-left edge and a small specular spot.
const TCHAR* const BlotFieldCode = TEXT(
	"float A = Color.a;\n"
	"float2 d = UV * 2.0 - 1.0;\n"
	"float r = length(d);\n"
	"float a = atan2(d.y, d.x);\n"
	"float v = floor(frac(R) * 5.999);\n"
	"float sd = 0.0;\n"
	"float mx = 0.5;\n"
	"if (v < 0.5)\n"
	"{\n"
	"	float rs = 0.5 + 0.08 * cos(5.0 * a + 0.7) + 0.05 * cos(8.0 * a + 2.1) + 0.16 * pow(saturate(cos(7.0 * a + 1.3)), 12.0);\n"
	"	sd = (rs - r) * 0.9;\n"
	"	sd = max(sd, 0.11 - length(d - float2(0.74, 0.40)));\n"
	"	sd = max(sd, 0.07 - length(d - float2(-0.40, 0.80)));\n"
	"	sd = max(sd, 0.08 - length(d - float2(-0.82, -0.28)));\n"
	"	sd = max(sd, 0.06 - length(d - float2(0.22, -0.84)));\n"
	"	mx = 0.5;\n"
	"}\n"
	"else if (v < 1.5)\n"
	"{\n"
	"	float2 p = d - float2(0.0, -0.32);\n"
	"	float2 b = float2(0.06, 1.18);\n"
	"	float t = saturate(dot(p, b) / dot(b, b));\n"
	"	float head = 0.38 - length(p);\n"
	"	float tail = (0.30 - 0.26 * t) - length(p - b * t);\n"
	"	float h = max(0.12 - abs(head - tail), 0.0) / 0.12;\n"
	"	sd = max(head, tail) + h * h * 0.03;\n"
	"	sd = max(sd, 0.07 - length(d - float2(-0.30, 0.62)));\n"
	"	mx = 0.4;\n"
	"}\n"
	"else if (v < 2.5)\n"
	"{\n"
	"	float rs = 0.62 + 0.07 * cos(4.0 * a + 1.0) + 0.05 * cos(7.0 * a + 2.0) + 0.025 * cos(13.0 * a + 0.5);\n"
	"	sd = (rs - r) * 0.9;\n"
	"	sd = min(sd, 0.30 - dot(d, float2(0.80, 0.60)));\n"
	"	sd = min(sd, 0.42 - dot(d, float2(-0.90, 0.44)));\n"
	"	sd += 0.014 * sin(37.0 * d.x + 11.0 * d.y) * sin(29.0 * d.y - 17.0 * d.x);\n"
	"	mx = 0.42;\n"
	"}\n"
	"else if (v < 3.5)\n"
	"{\n"
	"	sd = min(0.72 - r + 0.04 * cos(6.0 * a + 0.8), length(d - float2(0.30, -0.26)) - 0.62);\n"
	"	float c = 0.16 - length(d - float2(-0.52, 0.30));\n"
	"	float h = max(0.1 - abs(sd - c), 0.0) / 0.1;\n"
	"	sd = max(sd, c) + h * h * 0.025;\n"
	"	sd = max(sd, 0.06 - length(d - float2(0.62, 0.52)));\n"
	"	mx = 0.22;\n"
	"}\n"
	"else if (v < 4.5)\n"
	"{\n"
	"	float c0 = 0.42 - length(d - float2(-0.22, -0.12));\n"
	"	float c1 = 0.34 - length(d - float2(0.30, 0.10));\n"
	"	float h = max(0.25 - abs(c0 - c1), 0.0) / 0.25;\n"
	"	sd = max(c0, c1) + h * h * 0.0625;\n"
	"	float c2 = 0.26 - length(d - float2(-0.05, 0.48));\n"
	"	h = max(0.2 - abs(sd - c2), 0.0) / 0.2;\n"
	"	sd = max(sd, c2) + h * h * 0.05;\n"
	"	sd = max(sd, 0.08 - length(d - float2(0.70, -0.52)));\n"
	"	mx = 0.42;\n"
	"}\n"
	"else\n"
	"{\n"
	"	float2 p = d + float2(0.62, -0.10);\n"
	"	float2 b = float2(1.02, -0.22);\n"
	"	float t = saturate(dot(p, b) / dot(b, b));\n"
	"	sd = (0.20 - 0.15 * t) - length(p - b * t) + 0.02 * cos(19.0 * t);\n"
	"	sd = max(sd, 0.07 - length(d - float2(0.55, -0.20)));\n"
	"	sd = max(sd, 0.05 - length(d - float2(0.72, -0.27)));\n"
	"	sd = max(sd, 0.035 - length(d - float2(0.85, -0.31)));\n"
	"	sd = max(sd, 0.05 - length(d - float2(-0.30, 0.40)));\n"
	"	mx = 0.2;\n"
	"}\n"
	"float aa = 0.035;\n"
	"float e = (1.0 - A) * 1.05 * mx;\n"
	"float mask = saturate((sd - e) / aa + 0.5) * saturate(A * 10.0);\n"
	"float rim = (1.0 - smoothstep(0.0, 0.06, sd - e)) * saturate(dot(d, float2(-0.6, -0.8)) / max(r, 0.05));\n"
	"float spec = (1.0 - smoothstep(0.03, 0.08, length(d + float2(0.14, 0.18)))) * smoothstep(0.05, 0.12, sd - e);\n"
	"float gloss = rim * 0.12 + spec * 0.22;\n");
// Splash: a wet body with six uneven tendrils, a drop on each tendril's tip, three loose drops, noise that breaks the
// eroding edge into veins, and thinner (more see-through, lighter) edges than its core.
const TCHAR* const SplashFieldCode = TEXT(
	"float A = Color.a;\n"
	"float2 d = UV * 2.0 - 1.0;\n"
	"float r = length(d);\n"
	"float a = atan2(d.y, d.x);\n"
	"float p = frac(R) * 6.283;\n"
	"float L = 0.22 + 0.12 * cos(3.0 * a + p * 2.0) + 0.06 * cos(2.0 * a + p * 3.0);\n"
	"float fing = pow(saturate(cos(6.0 * a + p)), 3.0) * L;\n"
	"float rb = 0.44 + 0.05 * cos(3.0 * a + p) + 0.03 * cos(7.0 * a + p * 4.0) + 0.015 * cos(13.0 * a + p * 5.0);\n"
	"float sd = (rb + fing - r) * (0.6 + 0.4 * saturate((0.6 - r) * 3.0));\n"
	"[unroll] for (int k = 0; k < 6; ++k)\n"
	"{\n"
	"	float ak = (6.283 * k - p) / 6.0;\n"
	"	float lk = 0.22 + 0.12 * cos(3.0 * ak + p * 2.0) + 0.06 * cos(2.0 * ak + p * 3.0);\n"
	"	float rk = 0.44 + lk;\n"
	"	sd = max(sd, 0.04 + 0.12 * lk - length(d - rk * float2(cos(ak), sin(ak))));\n"
	"}\n"
	"float q = p + 0.4;\n"
	"sd = max(sd, 0.05 - length(d - 0.86 * float2(cos(q), sin(q))));\n"
	"sd = max(sd, 0.04 - length(d - 0.80 * float2(cos(q + 2.3), sin(q + 2.3))));\n"
	"sd = max(sd, 0.03 - length(d - 0.90 * float2(cos(q + 3.9), sin(q + 3.9))));\n"
	"float n = 0.06 * sin(17.0 * d.x + p * 3.0) * sin(15.0 * d.y - p * 2.0) + 0.04 * sin(29.0 * d.y + p) * sin(23.0 * d.x);\n"
	"float aa = 0.03;\n"
	"float e = (1.0 - A) * 0.30;\n"
	"float thick = saturate(sd / 0.2);\n"
	"float mask = saturate((sd + n * (1.0 - A) * 2.0 - e) / aa + 0.5) * (0.55 + 0.45 * thick) * saturate(A * 4.0);\n"
	"float gloss = (1.0 - thick) * 0.08;\n");

enum class ESpriteMask : uint8 { Soft, Spark, Ring, Chip, Blot, Splash };

struct FSpriteMaterialSpec
{
	const TCHAR* Name;
	ESpriteMask Kind;
	bool bAdditive;
	bool bDepthFade; // soft against the scene: it fades where it nears what is behind it
};

// Indexed kind * 4 + additive + depth fade * 2 (SpriteMaterialIndex). The first two of each kind are the unfaded ones.
const FSpriteMaterialSpec SpriteMaterials[] = {
	{TEXT("M_VfxSoft"), ESpriteMask::Soft, false, false},
	{TEXT("M_VfxSoftAdditive"), ESpriteMask::Soft, true, false},
	{TEXT("M_VfxSoftFade"), ESpriteMask::Soft, false, true},
	{TEXT("M_VfxSoftAdditiveFade"), ESpriteMask::Soft, true, true},
	{TEXT("M_VfxSpark"), ESpriteMask::Spark, false, false},
	{TEXT("M_VfxSparkAdditive"), ESpriteMask::Spark, true, false},
	{TEXT("M_VfxSparkFade"), ESpriteMask::Spark, false, true},
	{TEXT("M_VfxSparkAdditiveFade"), ESpriteMask::Spark, true, true},
	{TEXT("M_VfxRing"), ESpriteMask::Ring, false, false},
	{TEXT("M_VfxRingAdditive"), ESpriteMask::Ring, true, false},
	{TEXT("M_VfxRingFade"), ESpriteMask::Ring, false, true},
	{TEXT("M_VfxRingAdditiveFade"), ESpriteMask::Ring, true, true},
	{TEXT("M_VfxChip"), ESpriteMask::Chip, false, false},
	{TEXT("M_VfxChipAdditive"), ESpriteMask::Chip, true, false},
	{TEXT("M_VfxChipFade"), ESpriteMask::Chip, false, true},
	{TEXT("M_VfxChipAdditiveFade"), ESpriteMask::Chip, true, true},
	{TEXT("M_VfxBlot"), ESpriteMask::Blot, false, false},
	{TEXT("M_VfxBlotAdditive"), ESpriteMask::Blot, true, false},
	{TEXT("M_VfxBlotFade"), ESpriteMask::Blot, false, true},
	{TEXT("M_VfxBlotAdditiveFade"), ESpriteMask::Blot, true, true},
	{TEXT("M_VfxSplash"), ESpriteMask::Splash, false, false},
	{TEXT("M_VfxSplashAdditive"), ESpriteMask::Splash, true, false},
	{TEXT("M_VfxSplashFade"), ESpriteMask::Splash, false, true},
	{TEXT("M_VfxSplashAdditiveFade"), ESpriteMask::Splash, true, true},
};
constexpr int32 SpriteMaterialCount = UE_ARRAY_COUNT(SpriteMaterials);
static_assert(SpriteMaterialCount == 24, "six kinds, additive or not, faded or not");

int32 SpriteMaterialIndex(const ftcore::VfxNiagaraEmitter& P)
{
	using ftcore::VfxSprite;
	const ESpriteMask Kind = P.sprite == VfxSprite::Spark ? ESpriteMask::Spark
		: P.sprite == VfxSprite::Ring ? ESpriteMask::Ring
		: P.sprite == VfxSprite::Chip ? ESpriteMask::Chip
		: P.sprite == VfxSprite::Blot ? ESpriteMask::Blot
		: P.sprite == VfxSprite::Splash ? ESpriteMask::Splash
		: ESpriteMask::Soft;
	return static_cast<int32>(Kind) * 4 + (P.additive ? 1 : 0) + (P.depthFade ? 2 : 0);
}

// The liquid kinds' field code (sets mask and gloss), or null for the plain masks.
const TCHAR* LiquidFieldCode(ESpriteMask Kind)
{
	return Kind == ESpriteMask::Blot ? BlotFieldCode : Kind == ESpriteMask::Splash ? SplashFieldCode : nullptr;
}

FString MaskCode(ESpriteMask Kind)
{
	switch (Kind)
	{
	case ESpriteMask::Spark:
		return SparkMaskCode;
	case ESpriteMask::Ring:
		return RingMaskCode;
	case ESpriteMask::Chip:
		return ChipMaskCode;
	case ESpriteMask::Blot:
	case ESpriteMask::Splash:
		return FString(LiquidFieldCode(Kind)) + TEXT("return mask;\n");
	case ESpriteMask::Soft:
		break;
	}
	return SoftMaskCode;
}

const TCHAR* MaskName(ESpriteMask Kind)
{
	switch (Kind)
	{
	case ESpriteMask::Spark:
		return TEXT("SparkMask");
	case ESpriteMask::Ring:
		return TEXT("RingMask");
	case ESpriteMask::Chip:
		return TEXT("ChipMask");
	case ESpriteMask::Blot:
		return TEXT("BlotMask");
	case ESpriteMask::Splash:
		return TEXT("SplashMask");
	case ESpriteMask::Soft:
		break;
	}
	return TEXT("SoftMask");
}

FString MaterialPackage(const FSpriteMaterialSpec& Spec)
{
	return FString(MaterialRoot) / Spec.Name;
}

template <typename T>
T* AddVfxExpression(UMaterial* Material, int32 X, int32 Y)
{
	T* Expression = NewObject<T>(Material, NAME_None, RF_Transactional);
	Material->GetExpressionCollection().AddExpression(Expression);
	Expression->Material = Material;
	Expression->MaterialExpressionEditorX = X;
	Expression->MaterialExpressionEditorY = Y;
	Expression->UpdateMaterialExpressionGuid(true, false);
	return Expression;
}

void AddCustomInput(UMaterialExpressionCustom* Custom, const TCHAR* Name, UMaterialExpression* From, int32 Output = 0)
{
	FCustomInput& In = Custom->Inputs.AddDefaulted_GetRef();
	In.InputName = Name;
	In.Input.Connect(Output, From);
}

UMaterialExpressionCustom* AddCustom(UMaterial* Material, int32 Y, const TCHAR* Description, ECustomMaterialOutputType Type,
	const FString& Code, UMaterialExpression* Color, UMaterialExpression* Mask, UMaterialExpression* UV = nullptr,
	UMaterialExpression* Random = nullptr)
{
	UMaterialExpressionCustom* Custom = AddVfxExpression<UMaterialExpressionCustom>(Material, -300, Y);
	Custom->Description = Description;
	Custom->OutputType = Type;
	Custom->Code = Code;
	Custom->Inputs.Reset();
	FCustomInput& InColor = Custom->Inputs.AddDefaulted_GetRef();
	InColor.InputName = TEXT("Color");
	InColor.Input.Connect(5, Color); // RGBA
	FCustomInput& InMask = Custom->Inputs.AddDefaulted_GetRef();
	InMask.InputName = TEXT("Mask");
	InMask.Input.Connect(0, Mask);
	if (UV)
	{
		FCustomInput& InUV = Custom->Inputs.AddDefaulted_GetRef();
		InUV.InputName = TEXT("UV");
		InUV.Input.Connect(0, UV);
	}
	if (Random)
	{
		AddCustomInput(Custom, TEXT("R"), Random);
	}
	return Custom;
}

UMaterial* BuildSpriteMaterial(UPackage* Package, const FSpriteMaterialSpec& Spec)
{
	UMaterial* Material = NewObject<UMaterial>(Package, Spec.Name, RF_Public | RF_Standalone | RF_Transactional);
	Material->MaterialDomain = MD_Surface;
	Material->BlendMode = Spec.bAdditive ? BLEND_Additive : BLEND_Translucent;
	Material->SetShadingModel(MSM_Unlit);
	Material->SetUsageByFlag(MATUSAGE_NiagaraSprites, true);

	UMaterialExpressionTextureCoordinate* UV = AddVfxExpression<UMaterialExpressionTextureCoordinate>(Material, -900, 0);
	UMaterialExpressionParticleColor* Color = AddVfxExpression<UMaterialExpressionParticleColor>(Material, -900, 150);
	// The liquid kinds pick a silhouette by the particle's random number and erode it by its alpha.
	const TCHAR* const LiquidCode = LiquidFieldCode(Spec.Kind);
	UMaterialExpressionParticleRandom* Random =
		LiquidCode ? AddVfxExpression<UMaterialExpressionParticleRandom>(Material, -900, 450) : nullptr;

	UMaterialExpressionCustom* Shape = AddVfxExpression<UMaterialExpressionCustom>(Material, -600, 0);
	Shape->Description = MaskName(Spec.Kind);
	Shape->OutputType = CMOT_Float1;
	Shape->Code = MaskCode(Spec.Kind);
	Shape->Inputs.Reset();
	AddCustomInput(Shape, TEXT("UV"), UV);
	if (LiquidCode)
	{
		AddCustomInput(Shape, TEXT("R"), Random);
		AddCustomInput(Shape, TEXT("Color"), Color, 5); // RGBA
	}
	UMaterialExpression* Mask = Shape;

	// Soft against the scene: the mask fades out over a quarter of the sprite's size (5 cm at least) as it nears what is
	// behind it (a cloud's lower edge on the floor, a chip's edge against the body).
	if (Spec.bDepthFade)
	{
		UMaterialExpressionParticleSize* Size = AddVfxExpression<UMaterialExpressionParticleSize>(Material, -1200, 300);
		UMaterialExpressionCustom* Distance = AddVfxExpression<UMaterialExpressionCustom>(Material, -900, 300);
		Distance->Description = TEXT("FadeDistance");
		Distance->OutputType = CMOT_Float1;
		Distance->Code = TEXT("return max(Size.x * 0.25, 5.0);");
		Distance->Inputs.Reset();
		FCustomInput& InSize = Distance->Inputs.AddDefaulted_GetRef();
		InSize.InputName = TEXT("Size");
		InSize.Input.Connect(0, Size);
		UMaterialExpressionDepthFade* Fade = AddVfxExpression<UMaterialExpressionDepthFade>(Material, -600, 300);
		Fade->OpacityDefault = 1.0f;
		Fade->FadeDistance.Connect(0, Distance);
		UMaterialExpressionCustom* Faded = AddVfxExpression<UMaterialExpressionCustom>(Material, -450, 150);
		Faded->Description = TEXT("FadedMask");
		Faded->OutputType = CMOT_Float1;
		Faded->Code = TEXT("return Mask * Fade;");
		Faded->Inputs.Reset();
		FCustomInput& InMask = Faded->Inputs.AddDefaulted_GetRef();
		InMask.InputName = TEXT("Mask");
		InMask.Input.Connect(0, Shape);
		FCustomInput& InFade = Faded->Inputs.AddDefaulted_GetRef();
		InFade.InputName = TEXT("Fade");
		InFade.Input.Connect(0, Fade);
		Mask = Faded;
	}

	// Additive: light adds in proportion to alpha. Translucent: the colour over a masked alpha. A chip's colour is shaded
	// by its facets. A liquid sprite's alpha is already in its mask (it eroded the shape), and its colour takes the wet
	// sheen.
	const bool bChip = Spec.Kind == ESpriteMask::Chip;
	FString EmissiveCode;
	if (LiquidCode)
	{
		EmissiveCode = Spec.bAdditive ? FString(TEXT("return Color.rgb * Mask;")) : FString(LiquidCode) + TEXT("return Color.rgb + gloss;\n");
	}
	else if (bChip)
	{
		EmissiveCode = FString(ChipShadeCode) + (Spec.bAdditive ? TEXT("return Color.rgb * Color.a * Mask * s;\n") : TEXT("return Color.rgb * s;\n"));
	}
	else
	{
		EmissiveCode = Spec.bAdditive ? TEXT("return Color.rgb * Color.a * Mask;") : TEXT("return Color.rgb;");
	}
	const bool bLiquidSheen = LiquidCode && !Spec.bAdditive;
	UMaterialExpressionCustom* Emissive = AddCustom(Material, 0, TEXT("Emissive"), CMOT_Float3, EmissiveCode, Color, Mask,
		bChip || bLiquidSheen ? UV : nullptr, bLiquidSheen ? Random : nullptr);
	Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Emissive);
	if (!Spec.bAdditive)
	{
		UMaterialExpressionCustom* Opacity = AddCustom(Material, 150, TEXT("Opacity"), CMOT_Float1,
			LiquidCode ? TEXT("return saturate(Mask);") : TEXT("return saturate(Color.a * Mask);"), Color, Mask);
		Material->GetEditorOnlyData()->Opacity.Connect(0, Opacity);
	}

	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	return Material;
}

// ---- Niagara distributions -------------------------------------------------------------------------------

void SetValues(FNiagaraDistributionBase& D, ENiagaraDistributionMode Mode, std::initializer_list<float> Values)
{
	D.Mode = Mode;
	D.ChannelConstantsAndRanges = TArray<float>(Values);
	D.ChannelCurves.Reset();
	D.UpdateValuesFromDistribution();
}

void SetFloat(FNiagaraDistributionBase& D, float V)
{
	SetValues(D, ENiagaraDistributionMode::UniformConstant, {V});
}

// Uniform range (one random number for every channel), or a constant when lo == hi.
void SetRange(FNiagaraDistributionBase& D, const ftcore::VfxRange& R)
{
	if (R.lo == R.hi)
	{
		SetFloat(D, R.lo);
	}
	else
	{
		SetValues(D, ENiagaraDistributionMode::UniformRange, {R.lo, R.hi});
	}
}

void SetVector(FNiagaraDistributionBase& D, const ftcore::VfxVec3& V)
{
	SetValues(D, ENiagaraDistributionMode::NonUniformConstant, {V.x, V.y, V.z});
}

void SetVectorRange(FNiagaraDistributionBase& D, const ftcore::VfxVec3& Lo, const ftcore::VfxVec3& Hi)
{
	if (Lo == Hi)
	{
		SetVector(D, Lo);
	}
	else
	{
		SetValues(D, ENiagaraDistributionMode::NonUniformRange, {Lo.x, Lo.y, Lo.z, Hi.x, Hi.y, Hi.z});
	}
}

void SetVector2(FNiagaraDistributionBase& D, const ftcore::VfxVec2& V)
{
	SetValues(D, ENiagaraDistributionMode::NonUniformConstant, {V.x, V.y});
}

FRichCurve LinearCurve(const TArray<TPair<float, float>>& Keys)
{
	FRichCurve Curve;
	for (const TPair<float, float>& K : Keys)
	{
		Curve.SetKeyInterpMode(Curve.AddKey(K.Key, K.Value), RCIM_Linear);
	}
	return Curve;
}

// Every channel follows the same keys.
void SetCurve(FNiagaraDistributionBase& D, int32 Channels, const std::vector<ftcore::VfxKey>& Keys)
{
	TArray<TPair<float, float>> K;
	for (const ftcore::VfxKey& Key : Keys)
	{
		K.Emplace(Key.t, Key.v);
	}
	D.Mode = ENiagaraDistributionMode::NonUniformCurve;
	D.ChannelConstantsAndRanges.Reset();
	D.ChannelCurves.Reset();
	for (int32 C = 0; C < Channels; ++C)
	{
		D.ChannelCurves.Add(LinearCurve(K));
	}
	D.UpdateValuesFromDistribution();
}

FLinearColor ToLinear(const ftcore::VfxColor& C)
{
	return FLinearColor(C.r, C.g, C.b, C.a);
}

// A colour distribution from stops: one stop is a constant; several are a gradient sampled at `Lookup`.
void SetColorStops(FNiagaraDistributionBase& D, const std::vector<ftcore::VfxColorStop>& Stops, uint8 Lookup)
{
	if (Stops.size() == 1)
	{
		const FLinearColor C = ToLinear(Stops[0].color);
		SetValues(D, ENiagaraDistributionMode::NonUniformConstant, {C.R, C.G, C.B, C.A});
		return;
	}
	D.Mode = ENiagaraDistributionMode::ColorGradient;
	D.ChannelConstantsAndRanges.Reset();
	D.ChannelCurves.Reset();
	for (int32 C = 0; C < 4; ++C)
	{
		TArray<TPair<float, float>> K;
		for (const ftcore::VfxColorStop& S : Stops)
		{
			const float V[4] = {S.color.r, S.color.g, S.color.b, S.color.a};
			K.Emplace(S.t, V[C]);
		}
		D.ChannelCurves.Add(LinearCurve(K));
	}
	D.SetLookupValueMode(Lookup);
	D.UpdateValuesFromDistribution();
}

void Bind(FNiagaraDistributionBase& D, const FNiagaraVariableBase& Variable)
{
	D.Mode = ENiagaraDistributionMode::Binding;
	D.ParameterBinding = Variable;
}

FNiagaraVariable SpeedScaleVariable()
{
	return FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), FName(FString(TEXT("User.")) + UTF8_TO_TCHAR(ftcore::kVfxUserSpeedScale)));
}

FNiagaraVariable ShapeScaleVariable()
{
	return FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), FName(FString(TEXT("User.")) + UTF8_TO_TCHAR(ftcore::kVfxUserShapeScale)));
}

FVector3f ToVector(const ftcore::VfxVec3& V)
{
	return FVector3f(V.x, V.y, V.z);
}

// ---- emitters --------------------------------------------------------------------------------------------

template <typename T>
T* Module(UNiagaraStatelessEmitter* Emitter)
{
	T* M = Cast<T>(Emitter->GetModule(T::StaticClass()));
	if (!M)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: the template emitter has no %s."), *T::StaticClass()->GetName());
	}
	return M;
}

template <typename TValue>
TValue* EmitterProperty(UNiagaraStatelessEmitter* Emitter, const TCHAR* Name)
{
	const FProperty* Property = UNiagaraStatelessEmitter::StaticClass()->FindPropertyByName(Name);
	check(Property);
	return Property->ContainerPtrToValuePtr<TValue>(Emitter);
}

bool BuildEmitter(UNiagaraStatelessEmitter* Emitter, const ftcore::VfxNiagaraEmitter& P, UMaterialInterface* const Materials[SpriteMaterialCount],
	int32 DrawOrder)
{
	using namespace ftcore;

	UNiagaraStatelessModule_InitializeParticle* Init = Module<UNiagaraStatelessModule_InitializeParticle>(Emitter);
	UNiagaraStatelessModule_ShapeLocation* Shape = Module<UNiagaraStatelessModule_ShapeLocation>(Emitter);
	UNiagaraStatelessModule_AddVelocity* Velocity = Module<UNiagaraStatelessModule_AddVelocity>(Emitter);
	UNiagaraStatelessModule_GravityForce* Gravity = Module<UNiagaraStatelessModule_GravityForce>(Emitter);
	UNiagaraStatelessModule_Drag* Drag = Module<UNiagaraStatelessModule_Drag>(Emitter);
	UNiagaraStatelessModule_AccelerationForce* Accel = Module<UNiagaraStatelessModule_AccelerationForce>(Emitter);
	UNiagaraStatelessModule_CurlNoiseForce* Curl = Module<UNiagaraStatelessModule_CurlNoiseForce>(Emitter);
	UNiagaraStatelessModule_ScaleColor* ScaleColor = Module<UNiagaraStatelessModule_ScaleColor>(Emitter);
	UNiagaraStatelessModule_ScaleSpriteSize* ScaleSize = Module<UNiagaraStatelessModule_ScaleSpriteSize>(Emitter);
	UNiagaraStatelessModule_ScaleSpriteSizeBySpeed* BySpeed = Module<UNiagaraStatelessModule_ScaleSpriteSizeBySpeed>(Emitter);
	UNiagaraStatelessModule_SpriteFacingAndAlignment* Facing = Module<UNiagaraStatelessModule_SpriteFacingAndAlignment>(Emitter);
	UNiagaraStatelessModule_SpriteRotationRate* Spin = Module<UNiagaraStatelessModule_SpriteRotationRate>(Emitter);
	UNiagaraStatelessModule_SolveVelocitiesAndForces* Solve = Module<UNiagaraStatelessModule_SolveVelocitiesAndForces>(Emitter);
	if (!Init || !Shape || !Velocity || !Gravity || !Drag || !Accel || !Curl || !ScaleColor || !ScaleSize || !BySpeed ||
		!Facing || !Spin || !Solve)
	{
		return false;
	}
	// Start from nothing but the particle setup and the solver.
	for (UNiagaraStatelessModule* M : Emitter->GetModules())
	{
		if (M)
		{
			M->SetIsModuleEnabled(M == Init || M == Solve);
		}
	}

	// Emitter state, spawning and bounds.
	FNiagaraEmitterStateData* State = EmitterProperty<FNiagaraEmitterStateData>(Emitter, TEXT("EmitterState"));
	State->LoopBehavior = P.loop ? ENiagaraLoopBehavior::Infinite : ENiagaraLoopBehavior::Once;
	State->LoopDuration.InitConstant(P.loopDuration);
	State->bLoopDelayEnabled = false;

	TArray<FNiagaraStatelessSpawnInfo>* SpawnInfos = EmitterProperty<TArray<FNiagaraStatelessSpawnInfo>>(Emitter, TEXT("SpawnInfos"));
	SpawnInfos->Reset();
	if (P.rate > 0.f)
	{
		FNiagaraStatelessSpawnInfo& Info = Emitter->AddSpawnInfo();
		Info.Type = ENiagaraStatelessSpawnInfoType::Rate;
		Info.Rate.InitConstant(P.rate);
	}
	for (const VfxNiagaraBurst& B : P.bursts)
	{
		FNiagaraStatelessSpawnInfo& Info = Emitter->AddSpawnInfo();
		Info.Type = ENiagaraStatelessSpawnInfoType::Burst;
		Info.SpawnTime = B.time;
		Info.Amount.InitConstant(int32(B.countLo));
		if (B.countHi != B.countLo)
		{
			Info.Amount.Mode = ENiagaraDistributionMode::UniformRange;
			Info.Amount.Max = int32(B.countHi);
		}
	}
	*EmitterProperty<FBox>(Emitter, TEXT("FixedBounds")) =
		FBox(FVector(ToVector(P.boundsMin)), FVector(ToVector(P.boundsMax)));

	// InitializeParticle.
	SetRange(Init->LifetimeDistribution, P.lifetime);
	SetRange(Init->SpriteSizeDistribution, P.spriteSize);
	SetRange(Init->SpriteRotationDistribution, P.spriteRotation);
	SetColorStops(Init->ColorDistribution, P.startColor, P.startColorRandom ? 0xff : 0);

	// ShapeLocation.
	if (P.shape != VfxNiagaraShape::None)
	{
		Shape->SetIsModuleEnabled(true);
		switch (P.shape)
		{
		case VfxNiagaraShape::Sphere:
			Shape->ShapePrimitive = ENSM_ShapePrimitive::Sphere;
			SetFloat(Shape->SphereRadius, P.sphereRadius);
			break;
		case VfxNiagaraShape::Box:
			Shape->ShapePrimitive = ENSM_ShapePrimitive::Box;
			SetVector(Shape->BoxSize, P.boxSize);
			break;
		case VfxNiagaraShape::Ring:
			Shape->ShapePrimitive = ENSM_ShapePrimitive::Ring;
			SetFloat(Shape->RingRadius, P.ringRadius);
			SetFloat(Shape->DiscCoverage, P.discCoverage);
			break;
		case VfxNiagaraShape::None:
			break;
		}
		SetVector(Shape->TransformOffset, P.shapeOffset);
		Bind(Shape->ShapeScale, ShapeScaleVariable());
	}

	// AddVelocity, scaled by User.SpeedScale.
	if (P.velocity != VfxNiagaraVelocity::None)
	{
		Velocity->SetIsModuleEnabled(true);
		switch (P.velocity)
		{
		case VfxNiagaraVelocity::Linear:
			Velocity->VelocityType = ENSM_VelocityType::Linear;
			SetVectorRange(Velocity->LinearVelocityDistribution, P.linearLo, P.linearHi);
			Bind(Velocity->LinearVelocityScale, SpeedScaleVariable());
			break;
		case VfxNiagaraVelocity::FromPoint:
			Velocity->VelocityType = ENSM_VelocityType::FromPoint;
			SetRange(Velocity->PointVelocityDistribution, P.speed);
			Velocity->PointOrigin = ToVector(P.pointOrigin);
			Bind(Velocity->PointVelocityScale, SpeedScaleVariable());
			break;
		case VfxNiagaraVelocity::Cone:
			Velocity->VelocityType = ENSM_VelocityType::InCone;
			SetRange(Velocity->ConeVelocityDistribution, P.speed);
			Velocity->ConeRotationType = ENSM_ConeRotationType::Direction;
			SetVector(Velocity->ConeDirection, P.coneDirection);
			Velocity->ConeAngle = P.coneAngle;
			Velocity->InnerCone = 0.f;
			Bind(Velocity->ConeVelocityScale, SpeedScaleVariable());
			break;
		case VfxNiagaraVelocity::None:
			break;
		}
	}

	// Forces.
	if (!(P.gravity == VfxVec3{}))
	{
		Gravity->SetIsModuleEnabled(true);
		SetVector(Gravity->GravityDistribution, P.gravity);
	}
	if (P.drag > 0.f)
	{
		Drag->SetIsModuleEnabled(true);
		SetFloat(Drag->DragDistribution, P.drag);
	}
	if (!(P.acceleration == VfxVec3{}))
	{
		Accel->SetIsModuleEnabled(true);
		SetVector(Accel->AccelerationDistribution, P.acceleration);
	}
	if (P.curlNoise)
	{
		Curl->SetIsModuleEnabled(true);
		Curl->NoiseStrength = P.noiseStrength;
		Curl->NoiseFrequency = P.noiseFrequency;
	}

	// Over life.
	if (!P.sizeOverLife.empty())
	{
		ScaleSize->SetIsModuleEnabled(true);
		SetCurve(ScaleSize->ScaleDistribution, 2, P.sizeOverLife);
	}
	if (!P.colorOverLife.empty())
	{
		ScaleColor->SetIsModuleEnabled(true);
		SetColorStops(ScaleColor->ScaleDistribution, P.colorOverLife, 0); // Particles.NormalizedAge
	}
	if (P.rotationRate.lo != 0.f || P.rotationRate.hi != 0.f)
	{
		Spin->SetIsModuleEnabled(true);
		SetRange(Spin->RotationRateDistribution, P.rotationRate);
	}
	if (P.scaleBySpeed)
	{
		BySpeed->SetIsModuleEnabled(true);
		SetFloat(BySpeed->VelocityThreshold, P.speedThreshold);
		SetVector2(BySpeed->MinScaleFactor, P.minScale);
		SetVector2(BySpeed->MaxScaleFactor, P.maxScale);
	}

	// Renderer.
	UNiagaraSpriteRendererProperties* Sprite = nullptr;
	for (UNiagaraRendererProperties* R : Emitter->GetRenderers())
	{
		Sprite = Sprite ? Sprite : Cast<UNiagaraSpriteRendererProperties>(R);
	}
	if (!Sprite)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: the template emitter has no sprite renderer."));
		return false;
	}
	Sprite->Material = Materials[SpriteMaterialIndex(P)];
	Sprite->SortMode = P.additive ? ENiagaraSortMode::None : ENiagaraSortMode::ViewDistance;
	// The emitters draw in the data's order, later on top: Niagara sorts its renderers by this hint with an unstable
	// sort, so equal hints draw in any order (the crit's red flash covered its ink cloud).
	Sprite->SortOrderHint = DrawOrder;
	Sprite->FacingMode = ENiagaraSpriteFacingMode::FaceCamera;
	Sprite->Alignment = ENiagaraSpriteAlignment::Unaligned;
	switch (P.facing)
	{
	case VfxNiagaraFacing::Camera:
		break;
	case VfxNiagaraFacing::VelocityAligned:
		Sprite->Alignment = ENiagaraSpriteAlignment::VelocityAligned;
		break;
	case VfxNiagaraFacing::Horizontal:
		Sprite->FacingMode = ENiagaraSpriteFacingMode::CustomFacingVector;
		Facing->SetIsModuleEnabled(true);
		Facing->bSpriteFacingEnabled = true;
		Facing->bSpriteAlignmentEnabled = false;
		SetVector(Facing->SpriteFacing, VfxVec3{0.f, 0.f, 1.f});
		break;
	case VfxNiagaraFacing::Vertical:
		Sprite->Alignment = ENiagaraSpriteAlignment::CustomAlignment;
		Facing->SetIsModuleEnabled(true);
		Facing->bSpriteFacingEnabled = false;
		Facing->bSpriteAlignmentEnabled = true;
		SetVector(Facing->SpriteAlignment, VfxVec3{0.f, 0.f, 1.f});
		break;
	}
	return true;
}

UNiagaraSystem* BuildSystem(UPackage* Package, UNiagaraSystem* Template, const ftcore::VfxNiagaraPlan& Plan,
	UMaterialInterface* const Materials[SpriteMaterialCount])
{
	const FString Name = FPackageName::GetShortName(Package->GetName());
	UNiagaraSystem* System = CastChecked<UNiagaraSystem>(StaticDuplicateObject(Template, Package, *Name,
		RF_Public | RF_Standalone | RF_Transactional, UNiagaraSystem::StaticClass()));
	System->TemplateAssetDescription = FText();
	System->Category = FText();

	// One emitter handle per plan emitter, all copies of the template's.
	const FNiagaraEmitterHandle First = System->GetEmitterHandles()[0];
	for (size_t I = 1; I < Plan.emitters.size(); ++I)
	{
		System->DuplicateEmitterHandle(First, FName(UTF8_TO_TCHAR(Plan.emitters[I].name.c_str())));
	}
	System->GetEmitterHandles()[0].SetName(FName(UTF8_TO_TCHAR(Plan.emitters[0].name.c_str())), *System);
	if (System->GetEmitterHandles().Num() != int32(Plan.emitters.size()))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: %s has %d emitter handles, expected %d."), *Name,
			System->GetEmitterHandles().Num(), int32(Plan.emitters.size()));
		return nullptr;
	}

	// The runtime knobs, at their identity values.
	FNiagaraUserRedirectionParameterStore& User = System->GetExposedParameters();
	const FNiagaraVariable Speed = SpeedScaleVariable();
	const FNiagaraVariable ShapeScale = ShapeScaleVariable();
	User.AddParameter(Speed, true, false);
	User.SetParameterValue(1.0f, Speed);
	User.AddParameter(ShapeScale, true, false);
	User.SetParameterValue(FVector3f(1.0f), ShapeScale);

	for (int32 I = 0; I < System->GetEmitterHandles().Num(); ++I)
	{
		UNiagaraStatelessEmitter* Emitter = System->GetEmitterHandles()[I].GetStatelessEmitter();
		if (!Emitter || !BuildEmitter(Emitter, Plan.emitters[size_t(I)], Materials, I))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: couldn't build emitter %d of %s."), I, *Name);
			return nullptr;
		}
		Emitter->PostEditChange();
	}

	System->RequestCompile(false);
	System->WaitForCompilationComplete();
	return System;
}

// Logs what a saved system holds (for -Verify, after a reload from disk).
void Describe(const UNiagaraSystem* System)
{
	TArray<FNiagaraVariable> Params;
	System->GetExposedParameters().GetParameters(Params);
	FString Names;
	for (const FNiagaraVariable& V : Params)
	{
		Names += V.GetName().ToString() + TEXT(" ");
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildVfx: %s user parameters: %s"), *System->GetName(), *Names);
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		UNiagaraStatelessEmitter* Emitter = Handle.GetStatelessEmitter();
		if (!Emitter)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: %s/%s is not a Lightweight emitter."), *System->GetName(),
				*Handle.GetName().ToString());
			continue;
		}
		FString Modules;
		for (const UNiagaraStatelessModule* M : Emitter->GetModules())
		{
			if (M && M->IsModuleEnabled())
			{
				Modules += M->GetClass()->GetName().Replace(TEXT("NiagaraStatelessModule_"), TEXT("")) + TEXT(" ");
			}
		}
		FString Bindings;
		for (const UNiagaraStatelessModule* M : Emitter->GetModules())
		{
			if (M && M->IsModuleEnabled())
			{
				const FString ModuleName = M->GetClass()->GetName().Replace(TEXT("NiagaraStatelessModule_"), TEXT(""));
				FNiagaraDistributionBase::ForEachParameterBinding(M,
					[&Bindings, &ModuleName](const FNiagaraVariableBase& V)
					{ Bindings += ModuleName + TEXT(":") + V.GetName().ToString() + TEXT(" "); });
			}
		}
		FString DrawOrders;
		for (const UNiagaraRendererProperties* R : Emitter->GetRenderers())
		{
			DrawOrders += R ? FString::FromInt(R->SortOrderHint) + TEXT(" ") : FString();
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildVfx:   %s: %d spawn infos; draw order %s; modules %s; bindings %s"),
			*Handle.GetName().ToString(), Emitter->GetNumSpawnInfos(), *DrawOrders, *Modules, *Bindings);
	}
}

bool LoadEffect(const FString& File, ftcore::VfxEffect& Effect)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *File))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: couldn't read %s."), *File);
		return false;
	}
	const FTCHARToUTF8 Utf8(*Text);
	std::string Error;
	std::vector<std::string> Warnings;
	const bool bOk = ftcore::importVfxEffect(std::string_view(Utf8.Get(), Utf8.Length()), Effect, &Error, &Warnings);
	for (const std::string& W : Warnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildVfx: %s: %s"), *File, UTF8_TO_TCHAR(W.c_str()));
	}
	if (!bOk)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: %s: %s"), *File, UTF8_TO_TCHAR(Error.c_str()));
	}
	return bOk;
}
} // namespace
#endif

UFTBuildVfxCommandlet::UFTBuildVfxCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildVfxCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	TArray<FString> Files;
	const FString DataDir = FPaths::ProjectDir() / TEXT("Data/Vfx");
	IFileManager::Get().FindFiles(Files, *(DataDir / TEXT("*.json")), true, false);
	Files.Sort();
	if (Files.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: no effects in %s."), *DataDir);
		return 1;
	}
	// -Only=<name>: build that one effect and leave every other asset as it is (a sprite material it uses that isn't built
	// yet is built and saved; the built ones are used as they are).
	FString Only;
	FParse::Value(*Params, TEXT("Only="), Only);
	if (!Only.IsEmpty())
	{
		const FString Wanted = Only + TEXT(".json");
		Files.RemoveAll([&Wanted](const FString& File) { return File != Wanted; });
		if (Files.IsEmpty())
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: no effect called %s in %s."), *Only, *DataDir);
			return 1;
		}
	}

	if (FParse::Param(*Params, TEXT("Verify")))
	{
		int32 Missing = 0;
		for (const FString& File : Files)
		{
			const FString Name = TEXT("NS_") + FPaths::GetBaseFilename(File);
			const FString Path = FString(VfxRoot) / Name + TEXT(".") + Name;
			if (const UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *Path))
			{
				Describe(System);
			}
			else
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: couldn't load %s."), *Path);
				++Missing;
			}
		}
		return Missing == 0 ? 0 : 1;
	}

	UNiagaraSystem* Template = LoadObject<UNiagaraSystem>(nullptr, TemplatePath);
	if (!Template || Template->GetEmitterHandles().Num() < 1 || !Template->GetEmitterHandles()[0].GetStatelessEmitter())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: the Lightweight template %s is missing."), TemplatePath);
		return 1;
	}

	// Read and plan every effect before touching any file.
	TArray<ftcore::VfxNiagaraPlan> Plans;
	for (const FString& File : Files)
	{
		ftcore::VfxEffect Effect;
		if (!LoadEffect(DataDir / File, Effect))
		{
			return 1;
		}
		ftcore::VfxNiagaraPlan& Plan = Plans.AddDefaulted_GetRef();
		ftcore::planVfxNiagara(Effect, Plan);
		Plan.name = TCHAR_TO_UTF8(*FPaths::GetBaseFilename(File)); // the asset is named after the file
		for (const ftcore::VfxNiagaraEmitter& E : Plan.emitters)
		{
			for (const std::string& Gap : E.gaps)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("FTBuildVfx: gap %s/%s: %s"), UTF8_TO_TCHAR(Plan.name.c_str()),
					UTF8_TO_TCHAR(E.name.c_str()), UTF8_TO_TCHAR(Gap.c_str()));
			}
		}
	}

	// Start from nothing: delete every NS_* and the sprite materials up front, before anything loads them, then build the
	// materials the plans use. With -Only just that effect's asset goes, and the materials already built are used (one not
	// built yet is built).
	bool bOk = true;
	bool Used[SpriteMaterialCount] = {};
	for (const ftcore::VfxNiagaraPlan& Plan : Plans)
	{
		for (const ftcore::VfxNiagaraEmitter& E : Plan.emitters)
		{
			Used[SpriteMaterialIndex(E)] = true;
		}
	}
	UMaterialInterface* Materials[SpriteMaterialCount] = {};
	if (Only.IsEmpty())
	{
		TArray<FString> Old;
		IFileManager::Get().FindFiles(Old, *(FPaths::ProjectContentDir() / TEXT("VFX/NS_*.uasset")), true, false);
		for (const FString& File : Old)
		{
			if (!RemoveFile(FPaths::ProjectContentDir() / TEXT("VFX") / File))
			{
				return 1;
			}
		}
		for (const FSpriteMaterialSpec& Spec : SpriteMaterials)
		{
			if (!RemoveFile(PackageFile(MaterialPackage(Spec))))
			{
				return 1;
			}
		}
	}
	else if (!RemoveFile(FPaths::ProjectContentDir() / TEXT("VFX") / (TEXT("NS_") + Only + TEXT(".uasset"))))
	{
		return 1;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(PackageFile(MaterialPackage(SpriteMaterials[0]))), true);
	int32 MaterialsBuilt = 0;
	for (int32 I = 0; I < SpriteMaterialCount; ++I)
	{
		if (!Used[I])
		{
			continue;
		}
		const FString PackageName = MaterialPackage(SpriteMaterials[I]);
		if (!Only.IsEmpty() && IFileManager::Get().FileExists(*PackageFile(PackageName)))
		{
			Materials[I] = LoadObject<UMaterialInterface>(nullptr, *(PackageName + TEXT(".") + FPackageName::GetShortName(PackageName)));
			if (!Materials[I])
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: sprite material %s doesn't load."), *PackageName);
				return 1;
			}
			continue;
		}
		UPackage* Package = CreatePackage(*PackageName);
		UMaterial* Material = BuildSpriteMaterial(Package, SpriteMaterials[I]);
		Materials[I] = Material;
		FAssetCompilingManager::Get().FinishAllCompilation();
		bOk &= SaveVfxAsset(Package, Material);
		++MaterialsBuilt;
	}
	if (!bOk)
	{
		return 1;
	}

	int32 Emitters = 0;
	for (const ftcore::VfxNiagaraPlan& Plan : Plans)
	{
		const FString PackageName = FString(VfxRoot) / TEXT("NS_") + UTF8_TO_TCHAR(Plan.name.c_str());
		UPackage* Package = CreatePackage(*PackageName);
		UNiagaraSystem* System = BuildSystem(Package, Template, Plan, Materials);
		if (!System)
		{
			return 1;
		}
		FAssetCompilingManager::Get().FinishAllCompilation();
		if (!SaveVfxAsset(Package, System))
		{
			return 1;
		}
		Emitters += System->GetEmitterHandles().Num();
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildVfx: built %d systems (%d emitters) and %d sprite materials."), Plans.Num(),
		Emitters, MaterialsBuilt);
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx needs an editor build."));
	return 1;
#endif
}
