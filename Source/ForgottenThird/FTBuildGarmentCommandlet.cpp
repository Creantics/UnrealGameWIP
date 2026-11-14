#include "FTBuildGarmentCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture2D.h"
#include "FTImportMaterials.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionPreSkinnedNormal.h"
#include "Materials/MaterialExpressionPreSkinnedPosition.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexInterpolator.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "ftcore/GarmentDetail.h"
#include "ftcore/SkinDetail.h"
#endif

#if WITH_EDITOR
// Named, not anonymous: in a unity build the other material commandlets' own AddExpression and PackageFile share this
// translation unit.
namespace FTGarmentBuild
{
const TCHAR* const kFolder = TEXT("/Game/Characters/Shared/Materials/Garment/");
const TCHAR* const kGroup = TEXT("Garment");

// The masters' scalar parameters and their defaults; a substance overrides some of them.
struct FScalarSpec
{
	const TCHAR* Name;
	float Default;
};
const FScalarSpec kScalars[] = {
	{TEXT("Gain"), 1.f},              // dark-albedo lift (rolls off to white); a piece sets it to bring its map into the physical range
	{TEXT("Specular"), 0.5f},         // F0 = 0.08 x Specular
	{TEXT("CavitySpecular"), 0.7f},   // specular left at the bottom of the detail
	{TEXT("RoughnessScale"), 1.f},    // roughness = map G x RoughnessScale + RoughnessOffset, before the detail
	{TEXT("RoughnessOffset"), 0.f},
	{TEXT("MicroRoughness"), 0.06f},  // +- from the detail mask's micro variation
	{TEXT("CavityRoughness"), 0.1f},  // added at the bottom of the detail
	{TEXT("CreaseRoughness"), 0.08f}, // added in concave creases (WearTexture R below 0.5)
	{TEXT("EdgeSmooth"), 0.1f},       // taken off on convex worn edges (WearTexture R above 0.5)
	{TEXT("EdgeLighten"), 0.4f},      // albedo x (1 + EdgeLighten) on convex worn edges
	{TEXT("CavityDarken"), 0.25f},    // albedo towards the detail cavity
	{TEXT("AOStrength"), 1.f},
	{TEXT("MetalScale"), 1.f},        // metal = max(smoothstep(MetalFloor, MetalFloor + 0.2, map B x MetalScale), ForceMetal)
	{TEXT("MetalFloor"), 0.25f},      // map B below this is bake noise on the cloth (p90 0.14), not metal; zips and studs sit above 0.5
	{TEXT("ForceMetal"), 0.f},
	{TEXT("MetalRoughness"), 0.55f},
	{TEXT("DetailSizeA"), 3.f},       // centimetres per repeat of the fine tile
	{TEXT("DetailSizeB"), 11.f},      // and of the coarse one
	{TEXT("DetailStrengthA"), 0.3f},  // normal tilt added by each scale
	{TEXT("DetailStrengthB"), 0.15f},
	{TEXT("FadeNearA"), 60.f},        // camera distance (cm) where scale A starts to fade, and where it is gone
	{TEXT("FadeFarA"), 250.f},
	{TEXT("FadeNearB"), 200.f},
	{TEXT("FadeFarB"), 1200.f},
	{TEXT("MottleSize"), 3.f},        // centimetres per feature of the broad normal tilt (the roughness mottle is twice that)
	{TEXT("MottleTilt"), 0.f},        // normal tilt of the mottle (0 = off; leather jackets set it per instance)
	{TEXT("MottleRoughness"), 0.f},   // +- roughness of the mottle (0 = off)
	{TEXT("MottleFadeNear"), 800.f},  // camera distance (cm) where the mottle starts to fade, and where it is gone
	{TEXT("MottleFadeFar"), 1500.f},
	{TEXT("Cloth"), 0.6f},            // cloth master only: fuzz amount
	{TEXT("DiffuseResponse"), 1.f},   // albedo ^ DiffuseResponse: above 1 a darker response for dark cloth, light stitching kept
	{TEXT("SkyFill"), 1.f},           // share of sky and bounce light the surface takes (multiplies the occlusion output)
	// Set at runtime by the water system (LEAD_WETNESS_INPUT_SPEC); names fixed by that spec, all 0 = dry.
	{TEXT("Wetness"), 0.f},           // 0..1, falls to 0 as the cloth dries
	{TEXT("WetLineZ"), 0.f},          // world Z (cm) of the highest waterline: wet at or below it
	{TEXT("WetLineSoft"), 8.f},       // cm of blend across the line
	{TEXT("WetSilt"), 0.f},           // how dirty the water was: tide line and silt film, which stay after drying
	{TEXT("WetDarken"), 0.85f},       // albedo x this when soaked (leather; cloth sets its own)
	{TEXT("WetRoughness"), 0.15f},    // roughness pulled toward this when soaked (metal goes to 0.1)
	{TEXT("TideStrength"), 0.7f},     // tide line weight = WetSilt x this (silt 0.6: about 15% darker on light cloth, gaps aside)
	{TEXT("TideRoughness"), 0.2f},    // roughness lift on the tide line: a matte break in the sheen (all that shows on black)
	{TEXT("SiltFilmMax"), 0.15f},     // silt film: at most this share toward SiltColor (spec: 0.15)
	{TEXT("SiltFilmRoughness"), 0.1f}, // and roughness up by at most this once dry (spec: 0.1)
	{TEXT("SiltLiftMax"), 100.f},     // the film lifts the albedo by at most this factor per channel (100: no cap; leather 1.15)
	{TEXT("SoleHeight"), -1000.f},    // pre-skinned height (cm) below which a shoe is rubber sole; off by default
	{TEXT("SoleBlend"), 0.6f},        // cm over which the sole fades into the upper
	{TEXT("SoleSpecular"), 0.45f},
	{TEXT("SoleRoughness"), 0.85f},
};

struct FVectorSpec
{
	const TCHAR* Name;
	FLinearColor Default;
};
const FVectorSpec kVectors[] = {
	{TEXT("Tint"), FLinearColor::White},
	{TEXT("MetalColor"), FLinearColor(0.3f, 0.3f, 0.31f)}, // worn steel, not chrome
	{TEXT("FuzzTint"), FLinearColor::White}, // cloth master only: fuzz colour = albedo x FuzzTint
	{TEXT("SoleTint"), FLinearColor::White},  // albedo x SoleTint on the sole
	{TEXT("SiltColor"), FLinearColor(0.085f, 0.08f, 0.062f)}, // silt film: a grey-green-brown dulling, not a paint
	{TEXT("TideTint"), FLinearColor(0.55f, 0.45f, 0.33f)}, // tide line multiplies the cloth: darker and browner on any colour
};

struct FValue
{
	const TCHAR* Name;
	float Value;
};

struct FSubstance
{
	const TCHAR* Name;
	bool bCloth;
	ftcore::GarmentTile Tile;
	TArray<FValue> Scalars;
	FLinearColor Tint;
	FLinearColor FuzzTint;
};

TArray<FSubstance> Substances()
{
	using ftcore::GarmentTile;
	TArray<FSubstance> Out;
	// Leather jacket: black, a tight highlight (not a broad grey sheen), pebbled grain, smoother on worn edges, rougher in
	// creases; less sky fill and a darker diffuse response so it reads black, not grey-green, under an open sky. Roughness
	// from a map median of 0.49 lands near 0.3 with the micro and cavity terms (was near 0.4: the broad lobe).
	Out.Add({TEXT("Leather"), false, GarmentTile::Leather,
		{{TEXT("Specular"), 0.28f}, {TEXT("CavitySpecular"), 0.6f}, {TEXT("RoughnessScale"), 0.2f}, {TEXT("RoughnessOffset"), 0.12f},
			{TEXT("MicroRoughness"), 0.1f}, {TEXT("CavityRoughness"), 0.18f}, {TEXT("CreaseRoughness"), 0.1f}, {TEXT("EdgeSmooth"), 0.1f},
			{TEXT("EdgeLighten"), 0.12f}, {TEXT("CavityDarken"), 0.35f}, {TEXT("DetailSizeA"), 2.6f}, {TEXT("DetailSizeB"), 9.f},
			{TEXT("DetailStrengthA"), 0.25f}, {TEXT("DetailStrengthB"), 0.12f}, {TEXT("DiffuseResponse"), 1.06f},
			{TEXT("SkyFill"), 0.7f}, {TEXT("SiltLiftMax"), 1.15f}, {TEXT("SiltFilmRoughness"), 0.25f}},
		FLinearColor::White, FLinearColor::White});
	// Woven trousers: cloth shading, matte; the twill only shows within about a metre.
	Out.Add({TEXT("Woven"), true, GarmentTile::Twill,
		{{TEXT("Specular"), 0.3f}, {TEXT("RoughnessScale"), 0.f}, {TEXT("RoughnessOffset"), 0.9f}, {TEXT("MicroRoughness"), 0.04f},
			{TEXT("CavityRoughness"), 0.04f}, {TEXT("CreaseRoughness"), 0.f}, {TEXT("EdgeSmooth"), 0.05f}, {TEXT("EdgeLighten"), 0.25f},
			{TEXT("CavityDarken"), 0.2f}, {TEXT("DetailSizeA"), 3.f}, {TEXT("DetailSizeB"), 13.f}, {TEXT("DetailStrengthA"), 0.15f},
			{TEXT("DetailStrengthB"), 0.04f}, {TEXT("FadeNearA"), 40.f}, {TEXT("FadeFarA"), 100.f}, {TEXT("FadeNearB"), 60.f},
			{TEXT("FadeFarB"), 150.f}, {TEXT("Cloth"), 0.2f}, {TEXT("DiffuseResponse"), 1.06f}, {TEXT("SkyFill"), 0.75f},
			{TEXT("WetDarken"), 0.6f}, {TEXT("WetRoughness"), 0.35f}},
		FLinearColor::White, FLinearColor(0.4f, 0.4f, 0.4f)});
	// Jersey tee: cloth shading, soft fuzz, fine knit.
	Out.Add({TEXT("Knit"), true, GarmentTile::Knit,
		{{TEXT("Specular"), 0.3f}, {TEXT("RoughnessScale"), 0.f}, {TEXT("RoughnessOffset"), 0.9f}, {TEXT("MicroRoughness"), 0.04f},
			{TEXT("CavityRoughness"), 0.f}, {TEXT("CreaseRoughness"), 0.f}, {TEXT("EdgeSmooth"), 0.f}, {TEXT("EdgeLighten"), 0.f},
			{TEXT("CavityDarken"), 0.3f}, {TEXT("DetailSizeA"), 2.4f}, {TEXT("DetailSizeB"), 10.f}, {TEXT("DetailStrengthA"), 0.45f},
			{TEXT("DetailStrengthB"), 0.1f}, {TEXT("FadeNearA"), 40.f}, {TEXT("FadeFarA"), 180.f}, {TEXT("Cloth"), 0.8f},
			{TEXT("WetDarken"), 0.6f}, {TEXT("WetRoughness"), 0.35f}},
		FLinearColor::White, FLinearColor::White});
	// Rubber sole: matte, a faint stipple, slightly dirty.
	Out.Add({TEXT("Rubber"), false, GarmentTile::Leather,
		{{TEXT("Specular"), 0.4f}, {TEXT("RoughnessScale"), 0.f}, {TEXT("RoughnessOffset"), 0.82f}, {TEXT("EdgeLighten"), 0.f},
			{TEXT("CavityDarken"), 0.2f}, {TEXT("DetailSizeA"), 0.8f}, {TEXT("DetailStrengthA"), 0.15f}, {TEXT("DetailStrengthB"), 0.05f}},
		FLinearColor(0.9f, 0.88f, 0.85f), FLinearColor::White});
	// Zips, snaps and badges.
	Out.Add({TEXT("Metal"), false, GarmentTile::Leather,
		{{TEXT("ForceMetal"), 1.f}, {TEXT("MetalRoughness"), 0.55f}, {TEXT("DetailStrengthA"), 0.05f}, {TEXT("DetailStrengthB"), 0.f},
			{TEXT("EdgeLighten"), 0.f}, {TEXT("CavityDarken"), 0.f}},
		FLinearColor::White, FLinearColor::White});
	return Out;
}

// The detail: the tile projected along the three pre-skinned axes and blended by the normal, at two scales that each
// fade with camera distance. Returns xy the tangent tilt to add, z cavity (1 open, lower in the grooves), w micro
// roughness variation in -1..1.
const TCHAR* const kDetailCode = TEXT(R"HLSL(float3 n = normalize(N);
float3 w = pow(abs(n), 4.0);
w /= max(w.x + w.y + w.z, 1e-4);
float fade[2] = { saturate((FadeFarA - Depth) / max(FadeFarA - FadeNearA, 1.0)), saturate((FadeFarB - Depth) / max(FadeFarB - FadeNearB, 1.0)) };
float size[2] = { max(SizeA, 0.01), max(SizeB, 0.01) };
float strength[2] = { StrengthA, StrengthB };
float2 tilt = 0;
float cavity = 0;
float micro = 0;
for (int i = 0; i < 2; ++i)
{
	float3 p = P / size[i] + i * 0.371;
	float2 nx = Texture2DSample(DetailN, DetailNSampler, p.yz).xy * 2.0 - 1.0;
	float2 ny = Texture2DSample(DetailN, DetailNSampler, p.xz).xy * 2.0 - 1.0;
	float2 nz = Texture2DSample(DetailN, DetailNSampler, p.xy).xy * 2.0 - 1.0;
	float2 mx = Texture2DSample(DetailM, DetailMSampler, p.yz).rg;
	float2 my = Texture2DSample(DetailM, DetailMSampler, p.xz).rg;
	float2 mz = Texture2DSample(DetailM, DetailMSampler, p.xy).rg;
	float2 m = mx * w.x + my * w.y + mz * w.z;
	tilt += (nx * w.x + ny * w.y + nz * w.z) * strength[i] * fade[i];
	float share = i == 0 ? 0.65 : 0.35;
	cavity += (m.x - 1.0) * fade[i] * share;
	micro += (m.y * 2.0 - 1.0) * fade[i] * share;
}
return float4(tilt, saturate(1.0 + cavity), micro);
)HLSL");

// Mottle: a broad, centimetre-scale breakup that cuts a long specular streak (a leather sleeve) into pieces, which the
// millimetre grain tiles above are too small to do. xy a normal tilt from two decorrelated gradient-noise fields at
// MottleSize cm (plus a half-weight octave at 0.43x), z a zero-mean roughness change at twice that size. Taken in
// pre-skinned space so it stays on the garment, faded out with camera distance (MottleFadeNear..Far) where it would only
// alias, no tilt on metal. Off by default (MottleTilt and MottleRoughness 0): the early out keeps other garments' cost.
const TCHAR* const kMottleCode = TEXT(R"HLSL(float fade = saturate((FadeFar - Depth) / max(FadeFar - FadeNear, 1.0));
if (fade <= 0.0 || (Tilt == 0.0 && Rough == 0.0))
	return float3(0.0, 0.0, 0.0);
float3 p = P / max(Size, 0.1);
float3 o = float3(17.3, 5.1, 9.7);
float2 t = float2(GradientNoise3D_ALU(p, false, 0.0), GradientNoise3D_ALU(p + o, false, 0.0));
float3 p2 = p * 2.31 + float3(3.7, 11.2, 7.9);
t += 0.5 * float2(GradientNoise3D_ALU(p2, false, 0.0), GradientNoise3D_ALU(p2 + o, false, 0.0));
float r = GradientNoise3D_ALU(p * 0.5 + float3(31.1, 2.9, 13.3), false, 0.0);
return float3(t * Tilt * (1.0 - Metal), r * Rough) * fade;
)HLSL");

const TCHAR* const kNormalCode = TEXT("return normalize(float3(Base.xy + D.xy + M.xy, Base.z));");

// A shoe's rubber sole, split from the upper by pre-skinned height (the rigid foot pieces stand on z = 0).
const TCHAR* const kSoleCode = TEXT("return saturate((SoleHeight - P.z) / max(SoleBlend, 0.01) + 0.5);");

// Metal is all or nothing: the Tripo bake leaves a low metal noise over the whole cloth (median 0.03 to 0.05, p90 0.14),
// which lerped the black leather and the navy twill toward the grey MetalColor (the grey-green lift on both jackets).
const TCHAR* const kMetalCode = TEXT("return saturate(max(smoothstep(MetalFloor, MetalFloor + 0.2, ORM.b * MetalScale), ForceMetal));");

// Gain lifts dark albedo (x Gain near black) but rolls off towards white, so a map whose cloth is crushed to 0.004
// comes up to about 0.03 while its bright zips and stitching stay below 1: x Gain / (1 + x (Gain - 1)).
const TCHAR* const kColorCode = TEXT(R"HLSL(float3 b = Base * Tint.rgb;
float3 c = b * Gain / (1.0 + b * (Gain - 1.0));
c = pow(max(c, 1e-5), DiffResp);
float edge = saturate((Wear.r - 0.5) * 2.0);
// Rubbed, not outlined: the edge wear comes and goes along an edge (a smooth noise of about 8 cm in pre-skinned space).
float3 q = P * 0.8;
float rub = sin(q.x + 1.7 * sin(q.y * 0.6)) * sin(q.y * 0.9 + 1.3 * sin(q.z * 0.7)) * sin(q.z * 0.8 + 1.1 * sin(q.x * 0.5));
edge *= saturate(0.35 + 1.6 * rub);
c *= 1.0 + EdgeLighten * edge;
c *= lerp(1.0, D.z, CavityDarken);
c *= lerp(1.0, SoleTint.rgb, Sole);
c *= lerp(1.0, WetDarken, Wet.x);
// silt film below the line; on leather a capped lift (still black). The 0.01 floor keeps the cap from vanishing where the
// albedo is 0, so at the default (100) the cap is at least 1 and never binds.
c = min(lerp(c, SiltColor.rgb, Wet.z), max(c, 0.01) * SiltLiftMax);
c *= lerp(float3(1.0, 1.0, 1.0), TideTint.rgb, Wet.y); // tide line: a stain that darkens, never a lighter paint
return lerp(c, MetalColor.rgb * lerp(0.55, 1.0, D.z), Metal); // grime in the metal's recesses
)HLSL");

const TCHAR* const kRoughnessCode = TEXT(R"HLSL(float edge = saturate((Wear.r - 0.5) * 2.0);
float crease = saturate((0.5 - Wear.r) * 2.0);
float r = ORM.g * RoughScale + RoughOffset;
r += D.w * MicroRough + (1.0 - D.z) * CavityRough;
r += crease * CreaseRough - edge * EdgeSmooth;
r += M.z; // centimetre mottle (0 unless MottleRoughness is set)
r = lerp(r, SoleRough + D.w * MicroRough, Sole);
r = lerp(r, MetalRough + D.w * 0.15, Metal); // scratched, not polished
r += Wet.w * (1.0 - Metal); // dried silt film dulls the cloth and leather
return saturate(lerp(r, lerp(WetRough, 0.1, Metal), Wet.x));
)HLSL");

// Wetness by world height: x soaked share (below the line; dries from open surfaces inward, creases and grooves last),
// y tide line weight (a flat band 2 to 4 cm wide whose height wanders by up to 1.5 cm over about 20 cm of cloth, broken
// into gaps: about three quarters of it drawn at full weight, the rest gone; weight WetSilt x TideStrength, shows as the
// cloth dries; the colour code multiplies by TideTint, darker and browner, so on black leather only its roughness lift
// shows), z silt film share (toward SiltColor, at most SiltFilmMax, heavier lower down and in creases), w the roughness
// lift: the film's (up to SiltFilmRoughness, once dry) plus the tide line's (TideRoughness).
// The line is tested on world height (P); the noise that breaks it is taken in pre-skinned space (PS), so it stays on the
// cloth when the lead moves instead of crawling over it.
const TCHAR* const kWetCode = TEXT(R"HLSL(float3 q = PS * 0.045;
float n = 0.5 + 0.5 * sin(q.x + 2.1 * sin(q.y * 1.3)) * sin(q.y * 1.1 + 1.7 * sin(q.z * 0.9)) * sin(q.z * 1.2 + 1.3 * sin(q.x * 0.7));
float below = saturate((LineZ - P.z) / max(Soft, 1.0) + 0.5 + (n - 0.5) * 0.6);
float fold = saturate(saturate((0.5 - Wear.r) * 2.0) + (1.0 - D.z) * 2.0);
float soaked = below * saturate(Wetness * 2.0 - n * (1.0 - fold));
float3 m = PS * 0.3;
float wob = sin(m.x + 1.3 * sin(m.y * 0.8)) * sin(m.y * 0.9 + 1.1 * sin(m.z * 0.7));
float hw = 1.5 + 0.5 * sin(m.z * 1.7 + m.x * 1.1);
float band = 1.0 - smoothstep(hw - 0.4, hw + 0.4, abs(P.z - LineZ - wob * 1.5));
float3 k = PS * 0.6;
float grain = 0.5 + 0.5 * sin(k.x + 1.9 * sin(k.z * 0.8)) * sin(k.y + 1.4 * sin(k.x * 0.9));
float tide = saturate(Silt * TideStrength) * (1.0 - Wetness) * band * smoothstep(0.28, 0.36, grain);
float low = lerp(0.45, 1.0, saturate((LineZ - P.z) / 60.0)) * (1.0 + 0.4 * fold);
float film = min(below * Silt * 0.25 * low, FilmMax);
return float4(soaked, tide, film, film * FilmRough / max(FilmMax, 1e-3) * (1.0 - soaked) + tide * TideRough);
)HLSL");

const TCHAR* const kSpecularCode = TEXT("return lerp(Spec, SoleSpec, Sole) * lerp(CavitySpec, 1.0, D.z);");

const TCHAR* const kOcclusionCode = TEXT("return lerp(1.0, ORM.r, AOStrength) * SkyFill;");

const TCHAR* const kFuzzCode = TEXT(R"HLSL(float3 b = Base * Tint.rgb;
return b * Gain / (1.0 + b * (Gain - 1.0)) * FuzzTint.rgb * (1.0 - 0.7 * Wet.x); // soaked fibres lie flat
)HLSL");

FString PackageFile(const FString& Package)
{
	return FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
}

// The old asset files, moved aside (to <file>.ftold, which the asset registry ignores) while the new ones are built.
// Unless Commit() is called, the destructor deletes whatever was written in their place and moves them back.
class FSetAside
{
public:
	~FSetAside()
	{
		IFileManager& Files = IFileManager::Get();
		for (const FString& File : Moved)
		{
			if (bCommitted)
			{
				Files.Delete(*(File + TEXT(".ftold")), false, true, true);
				continue;
			}
			Files.Delete(*File, false, true, true);
			if (!Files.Move(*File, *(File + TEXT(".ftold"))))
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildGarment: couldn't put back %s (left as %s.ftold)."), *File, *File);
			}
		}
		if (!bCommitted)
		{
			for (const FString& File : Created)
			{
				Files.Delete(*File, false, true, true); // saved by this run with nothing before it
			}
			if (!Moved.IsEmpty() || !Created.IsEmpty())
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildGarment: build failed; %d old assets put back, %d new ones removed."),
					Moved.Num(), Created.Num());
			}
		}
	}

	// Moves the file aside when it exists, else notes it as new (removed if the build fails); false (logged) when it
	// can't be. A .ftold left by a run that was killed is the last good copy: it is put back first.
	bool Add(const FString& File)
	{
		IFileManager& Files = IFileManager::Get();
		const FString Old = File + TEXT(".ftold");
		if (Files.FileExists(*Old))
		{
			Files.Delete(*File, false, true, true);
			if (Files.FileExists(*File) || !Files.Move(*File, *Old))
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildGarment: %s.ftold from an earlier run couldn't be put back; restore it by hand."), *File);
				return false;
			}
			UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildGarment: put back %s from an earlier run's .ftold."), *File);
		}
		if (!Files.FileExists(*File))
		{
			Created.Add(File);
			return true;
		}
		if (!Files.Move(*(File + TEXT(".ftold")), *File))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildGarment: couldn't set aside %s."), *File);
			return false;
		}
		Moved.Add(File);
		return true;
	}

	void Commit() { bCommitted = true; }

private:
	TArray<FString> Moved;
	TArray<FString> Created;
	bool bCommitted = false;
};

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

// One parameter node per name, made on first use.
struct FGraph
{
	UMaterial* Material = nullptr;
	TMap<FName, UMaterialExpression*> Params;
	int32 Row = 0;

	UMaterialExpression* Scalar(const TCHAR* Name)
	{
		if (UMaterialExpression** Found = Params.Find(Name))
		{
			return *Found;
		}
		float Default = 0.f;
		for (const FScalarSpec& Spec : kScalars)
		{
			if (FCString::Strcmp(Spec.Name, Name) == 0)
			{
				Default = Spec.Default;
			}
		}
		UMaterialExpressionScalarParameter* P = AddExpression<UMaterialExpressionScalarParameter>(Material, -1600, 60 * Row++);
		P->ParameterName = Name;
		P->DefaultValue = Default;
		P->Group = kGroup;
		P->UpdateParameterGuid(true, false);
		Params.Add(Name, P);
		return P;
	}

	UMaterialExpression* Vector(const TCHAR* Name)
	{
		if (UMaterialExpression** Found = Params.Find(Name))
		{
			return *Found;
		}
		FLinearColor Default = FLinearColor::White;
		for (const FVectorSpec& Spec : kVectors)
		{
			if (FCString::Strcmp(Spec.Name, Name) == 0)
			{
				Default = Spec.Default;
			}
		}
		UMaterialExpressionVectorParameter* P = AddExpression<UMaterialExpressionVectorParameter>(Material, -1600, 60 * Row++);
		P->ParameterName = Name;
		P->DefaultValue = Default;
		P->Group = kGroup;
		P->UpdateParameterGuid(true, false);
		Params.Add(Name, P);
		return P;
	}
};

UMaterialExpressionTextureSampleParameter2D* Sampler(UMaterial* Material, const TCHAR* Name, UTexture* Default,
	EMaterialSamplerType Type, int32 Y)
{
	UMaterialExpressionTextureSampleParameter2D* S = AddExpression<UMaterialExpressionTextureSampleParameter2D>(Material, -1200, Y);
	S->ParameterName = Name;
	S->Texture = Default;
	S->SamplerType = Type;
	S->Group = kGroup;
	S->UpdateParameterGuid(true, false);
	return S;
}

UMaterialExpressionTextureObjectParameter* TextureObject(UMaterial* Material, const TCHAR* Name, UTexture* Default,
	EMaterialSamplerType Type, int32 Y)
{
	UMaterialExpressionTextureObjectParameter* T = AddExpression<UMaterialExpressionTextureObjectParameter>(Material, -1200, Y);
	T->ParameterName = Name;
	T->Texture = Default;
	T->SamplerType = Type;
	T->Group = kGroup;
	T->UpdateParameterGuid(true, false);
	return T;
}

// A Custom HLSL node; each input is (name, expression, output index).
struct FIn
{
	const TCHAR* Name;
	UMaterialExpression* From;
	int32 Output;
};
UMaterialExpressionCustom* Custom(UMaterial* Material, const TCHAR* Description, ECustomMaterialOutputType Type,
	const TCHAR* Code, std::initializer_list<FIn> Inputs, int32 Y)
{
	UMaterialExpressionCustom* C = AddExpression<UMaterialExpressionCustom>(Material, -500, Y);
	C->Description = Description;
	C->OutputType = Type;
	C->Code = Code;
	C->Inputs.Reset();
	for (const FIn& In : Inputs)
	{
		FCustomInput& Slot = C->Inputs.AddDefaulted_GetRef();
		Slot.InputName = In.Name;
		Slot.Input.Connect(In.Output, In.From);
	}
	return C;
}

// A pre-skinned vector carried from the vertex shader to the pixel shader.
UMaterialExpressionVertexInterpolator* Interpolated(UMaterial* Material, UMaterialExpression* From, int32 Y)
{
	UMaterialExpressionVertexInterpolator* V = AddExpression<UMaterialExpressionVertexInterpolator>(Material, -1600, Y);
	V->Input.Connect(0, From);
	return V;
}

struct FDefaults
{
	UTexture* White = nullptr;
	UTexture* Orm = nullptr;
	UTexture* FlatNormal = nullptr;
	UTexture* Wear = nullptr;
	UTexture* DetailN = nullptr;
	UTexture* DetailM = nullptr;
};

UMaterial* BuildMaster(const FString& Path, bool bCloth, const FDefaults& Tex)
{
	UPackage* Package = CreatePackage(*Path);
	UMaterial* Material = NewObject<UMaterial>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
	Material->SetUsageByFlag(MATUSAGE_SkeletalMesh, true);
	Material->SetUsageByFlag(MATUSAGE_MorphTargets, true);
	Material->SetUsageByFlag(MATUSAGE_Clothing, true);
	Material->SetShadingModel(bCloth ? MSM_Cloth : MSM_DefaultLit);
	// Tripo garments are shells whose inside shows at the cuffs, collar and hem.
	Material->TwoSided = true;

	FGraph G;
	G.Material = Material;

	// Texture sample outputs: 0 RGB, 1 R, 2 G, 3 B, 4 A.
	UMaterialExpressionTextureSampleParameter2D* Base = Sampler(Material, TEXT("BaseColorTexture"), Tex.White, SAMPLERTYPE_Color, -900);
	UMaterialExpressionTextureSampleParameter2D* Orm = Sampler(Material, TEXT("MetallicRoughnessTexture"), Tex.Orm, SAMPLERTYPE_Masks, -600);
	UMaterialExpressionTextureSampleParameter2D* Normal = Sampler(Material, TEXT("NormalTexture"), Tex.FlatNormal, SAMPLERTYPE_Normal, -300);
	UMaterialExpressionTextureSampleParameter2D* Wear = Sampler(Material, TEXT("WearTexture"), Tex.Wear, SAMPLERTYPE_Masks, 0);
	UMaterialExpressionTextureObjectParameter* DetailN = TextureObject(Material, TEXT("DetailNormal"), Tex.DetailN, SAMPLERTYPE_Normal, 300);
	UMaterialExpressionTextureObjectParameter* DetailM = TextureObject(Material, TEXT("DetailMask"), Tex.DetailM, SAMPLERTYPE_Masks, 500);

	UMaterialExpressionVertexInterpolator* Position =
		Interpolated(Material, AddExpression<UMaterialExpressionPreSkinnedPosition>(Material, -1900, 700), 700);
	UMaterialExpressionVertexInterpolator* SurfaceNormal =
		Interpolated(Material, AddExpression<UMaterialExpressionPreSkinnedNormal>(Material, -1900, 800), 800);
	UMaterialExpressionPixelDepth* Depth = AddExpression<UMaterialExpressionPixelDepth>(Material, -1600, 900);

	UMaterialExpressionCustom* Detail = Custom(Material, TEXT("GarmentDetail"), CMOT_Float4, kDetailCode,
		{{TEXT("P"), Position, 0}, {TEXT("N"), SurfaceNormal, 0}, {TEXT("Depth"), Depth, 0}, {TEXT("DetailN"), DetailN, 0},
			{TEXT("DetailM"), DetailM, 0}, {TEXT("SizeA"), G.Scalar(TEXT("DetailSizeA")), 0}, {TEXT("SizeB"), G.Scalar(TEXT("DetailSizeB")), 0},
			{TEXT("StrengthA"), G.Scalar(TEXT("DetailStrengthA")), 0}, {TEXT("StrengthB"), G.Scalar(TEXT("DetailStrengthB")), 0},
			{TEXT("FadeNearA"), G.Scalar(TEXT("FadeNearA")), 0}, {TEXT("FadeFarA"), G.Scalar(TEXT("FadeFarA")), 0},
			{TEXT("FadeNearB"), G.Scalar(TEXT("FadeNearB")), 0}, {TEXT("FadeFarB"), G.Scalar(TEXT("FadeFarB")), 0}},
		600);
	UMaterialExpressionCustom* Metal = Custom(Material, TEXT("GarmentMetal"), CMOT_Float1, kMetalCode,
		{{TEXT("ORM"), Orm, 0}, {TEXT("MetalScale"), G.Scalar(TEXT("MetalScale")), 0}, {TEXT("MetalFloor"), G.Scalar(TEXT("MetalFloor")), 0},
			{TEXT("ForceMetal"), G.Scalar(TEXT("ForceMetal")), 0}}, -500);
	UMaterialExpressionCustom* Sole = Custom(Material, TEXT("GarmentSole"), CMOT_Float1, kSoleCode,
		{{TEXT("P"), Position, 0}, {TEXT("SoleHeight"), G.Scalar(TEXT("SoleHeight")), 0}, {TEXT("SoleBlend"), G.Scalar(TEXT("SoleBlend")), 0}}, -700);
	UMaterialExpressionWorldPosition* WorldPos = AddExpression<UMaterialExpressionWorldPosition>(Material, -1900, 1000);
	UMaterialExpressionCustom* WetMask = Custom(Material, TEXT("GarmentWet"), CMOT_Float4, kWetCode,
		{{TEXT("P"), WorldPos, 0}, {TEXT("PS"), Position, 0}, {TEXT("Wear"), Wear, 0}, {TEXT("D"), Detail, 0}, {TEXT("Wetness"), G.Scalar(TEXT("Wetness")), 0},
			{TEXT("LineZ"), G.Scalar(TEXT("WetLineZ")), 0}, {TEXT("Soft"), G.Scalar(TEXT("WetLineSoft")), 0},
			{TEXT("Silt"), G.Scalar(TEXT("WetSilt")), 0}, {TEXT("TideStrength"), G.Scalar(TEXT("TideStrength")), 0},
			{TEXT("TideRough"), G.Scalar(TEXT("TideRoughness")), 0}, {TEXT("FilmMax"), G.Scalar(TEXT("SiltFilmMax")), 0},
			{TEXT("FilmRough"), G.Scalar(TEXT("SiltFilmRoughness")), 0}},
		1000);
	UMaterialExpressionCustom* Color = Custom(Material, TEXT("GarmentColor"), CMOT_Float3, kColorCode,
		{{TEXT("Base"), Base, 0}, {TEXT("Tint"), G.Vector(TEXT("Tint")), 0}, {TEXT("Gain"), G.Scalar(TEXT("Gain")), 0}, {TEXT("Wear"), Wear, 0},
			{TEXT("D"), Detail, 0}, {TEXT("EdgeLighten"), G.Scalar(TEXT("EdgeLighten")), 0},
			{TEXT("CavityDarken"), G.Scalar(TEXT("CavityDarken")), 0}, {TEXT("MetalColor"), G.Vector(TEXT("MetalColor")), 0},
			{TEXT("Metal"), Metal, 0}, {TEXT("Sole"), Sole, 0}, {TEXT("SoleTint"), G.Vector(TEXT("SoleTint")), 0},
			{TEXT("P"), Position, 0}, {TEXT("DiffResp"), G.Scalar(TEXT("DiffuseResponse")), 0},
			{TEXT("Wet"), WetMask, 0}, {TEXT("WetDarken"), G.Scalar(TEXT("WetDarken")), 0}, {TEXT("SiltColor"), G.Vector(TEXT("SiltColor")), 0},
			{TEXT("TideTint"), G.Vector(TEXT("TideTint")), 0}, {TEXT("SiltLiftMax"), G.Scalar(TEXT("SiltLiftMax")), 0}},
		-900);
	UMaterialExpressionCustom* Mottle = Custom(Material, TEXT("GarmentMottle"), CMOT_Float3, kMottleCode,
		{{TEXT("P"), Position, 0}, {TEXT("Depth"), Depth, 0}, {TEXT("Metal"), Metal, 0}, {TEXT("Size"), G.Scalar(TEXT("MottleSize")), 0},
			{TEXT("Tilt"), G.Scalar(TEXT("MottleTilt")), 0}, {TEXT("Rough"), G.Scalar(TEXT("MottleRoughness")), 0},
			{TEXT("FadeNear"), G.Scalar(TEXT("MottleFadeNear")), 0}, {TEXT("FadeFar"), G.Scalar(TEXT("MottleFadeFar")), 0}},
		800);
	UMaterialExpressionCustom* NormalOut = Custom(Material, TEXT("GarmentNormal"), CMOT_Float3, kNormalCode,
		{{TEXT("Base"), Normal, 0}, {TEXT("D"), Detail, 0}, {TEXT("M"), Mottle, 0}}, -300);
	UMaterialExpressionCustom* Roughness = Custom(Material, TEXT("GarmentRoughness"), CMOT_Float1, kRoughnessCode,
		{{TEXT("ORM"), Orm, 0}, {TEXT("Wear"), Wear, 0}, {TEXT("D"), Detail, 0}, {TEXT("RoughScale"), G.Scalar(TEXT("RoughnessScale")), 0},
			{TEXT("RoughOffset"), G.Scalar(TEXT("RoughnessOffset")), 0}, {TEXT("MicroRough"), G.Scalar(TEXT("MicroRoughness")), 0},
			{TEXT("CavityRough"), G.Scalar(TEXT("CavityRoughness")), 0}, {TEXT("CreaseRough"), G.Scalar(TEXT("CreaseRoughness")), 0},
			{TEXT("EdgeSmooth"), G.Scalar(TEXT("EdgeSmooth")), 0}, {TEXT("MetalRough"), G.Scalar(TEXT("MetalRoughness")), 0},
			{TEXT("Metal"), Metal, 0}, {TEXT("Sole"), Sole, 0}, {TEXT("SoleRough"), G.Scalar(TEXT("SoleRoughness")), 0},
			{TEXT("Wet"), WetMask, 0}, {TEXT("WetRough"), G.Scalar(TEXT("WetRoughness")), 0}, {TEXT("M"), Mottle, 0}},
		0);
	UMaterialExpressionCustom* Specular = Custom(Material, TEXT("GarmentSpecular"), CMOT_Float1, kSpecularCode,
		{{TEXT("Spec"), G.Scalar(TEXT("Specular")), 0}, {TEXT("CavitySpec"), G.Scalar(TEXT("CavitySpecular")), 0}, {TEXT("D"), Detail, 0},
			{TEXT("Sole"), Sole, 0}, {TEXT("SoleSpec"), G.Scalar(TEXT("SoleSpecular")), 0}}, 200);
	UMaterialExpressionCustom* Occlusion = Custom(Material, TEXT("GarmentOcclusion"), CMOT_Float1, kOcclusionCode,
		{{TEXT("ORM"), Orm, 0}, {TEXT("AOStrength"), G.Scalar(TEXT("AOStrength")), 0}, {TEXT("SkyFill"), G.Scalar(TEXT("SkyFill")), 0}}, 400);

	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Color);
	Inputs->Normal.Connect(0, NormalOut);
	Inputs->Roughness.Connect(0, Roughness);
	Inputs->Metallic.Connect(0, Metal);
	Inputs->Specular.Connect(0, Specular);
	Inputs->AmbientOcclusion.Connect(0, Occlusion);
	if (bCloth)
	{
		// The cloth model's Fuzz Color is the subsurface colour pin and its Cloth amount CustomData0 (the ClearCoat field).
		UMaterialExpressionCustom* Fuzz = Custom(Material, TEXT("GarmentFuzz"), CMOT_Float3, kFuzzCode,
			{{TEXT("Base"), Base, 0}, {TEXT("Tint"), G.Vector(TEXT("Tint")), 0}, {TEXT("Gain"), G.Scalar(TEXT("Gain")), 0},
				{TEXT("FuzzTint"), G.Vector(TEXT("FuzzTint")), 0}, {TEXT("Wet"), WetMask, 0}},
			800);
		Inputs->SubsurfaceColor.Connect(0, Fuzz);
		Inputs->ClearCoat.Connect(0, G.Scalar(TEXT("Cloth")));
	}

	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	FAssetRegistryModule::AssetCreated(Material);
	return Material;
}

UTexture2D* NewTexture(const FString& Path, int32 Size, const TArray<uint8>& Bgra, TextureCompressionSettings Compression)
{
	UPackage* Package = CreatePackage(*Path);
	UTexture2D* Texture = NewObject<UTexture2D>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
	Texture->Source.Init(Size, Size, 1, 1, TSF_BGRA8, Bgra.GetData());
	Texture->SRGB = Compression == TC_Default;
	Texture->CompressionSettings = Compression;
	Texture->VirtualTextureStreaming = false;
	if (Size <= 4)
	{
		Texture->MipGenSettings = TMGS_NoMipmaps;
	}
	Texture->PostEditChange();
	FAssetRegistryModule::AssetCreated(Texture);
	return Texture;
}

// A 4 x 4 texture of one BGRA value.
UTexture2D* Solid(const FString& Path, uint8 B, uint8 Gr, uint8 R, TextureCompressionSettings Compression)
{
	TArray<uint8> Bytes;
	for (int32 i = 0; i < 16; ++i)
	{
		Bytes.Append({B, Gr, R, 255});
	}
	return NewTexture(Path, 4, Bytes, Compression);
}

UMaterialInstanceConstant* BuildInstance(const FString& Path, UMaterial* Parent, const FSubstance& Substance, UTexture* DetailN,
	UTexture* DetailM)
{
	UPackage* Package = CreatePackage(*Path);
	UMaterialInstanceConstant* Instance =
		NewObject<UMaterialInstanceConstant>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
	Instance->SetParentEditorOnly(Parent);
	for (const FValue& V : Substance.Scalars)
	{
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(V.Name), V.Value);
	}
	Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Tint")), Substance.Tint);
	if (Substance.bCloth)
	{
		Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("FuzzTint")), Substance.FuzzTint);
	}
	Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("DetailNormal")), DetailN);
	Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("DetailMask")), DetailM);
	Instance->PreEditChange(nullptr);
	Instance->PostEditChange();
	FAssetRegistryModule::AssetCreated(Instance);
	return Instance;
}

bool Save(UObject* Asset)
{
	UPackage* Package = Asset->GetPackage();
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	const FString File = PackageFile(Package->GetName());
	if (!UPackage::SavePackage(Package, Asset, *File, Args))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildGarment: couldn't save %s."), *File);
		return false;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildGarment: saved %s."), *File);
	return true;
}

const TCHAR* TileName(ftcore::GarmentTile Tile)
{
	switch (Tile)
	{
	case ftcore::GarmentTile::Leather:
		return TEXT("Leather");
	case ftcore::GarmentTile::Twill:
		return TEXT("Twill");
	case ftcore::GarmentTile::Knit:
		return TEXT("Knit");
	}
	return TEXT("Unknown");
}
} // namespace FTGarmentBuild
#endif

UFTBuildGarmentCommandlet::UFTBuildGarmentCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildGarmentCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	using namespace FTGarmentBuild;
	FString DumpDir;
	FParse::Value(*Params, TEXT("DumpDir="), DumpDir);
	if (!FApp::CanEverRender())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildGarment: run with -AllowCommandletRendering, so the masters compile and are checked."));
		return 1;
	}
	const FString Folder = kFolder;
	const ftcore::GarmentTile Tiles[] = {ftcore::GarmentTile::Leather, ftcore::GarmentTile::Twill, ftcore::GarmentTile::Knit};
	const TArray<FSubstance> All = Substances();

	// Start the shared assets from nothing, before anything loads the old ones. The old files are set aside, not
	// deleted, and come back unless every new asset compiles and saves (a failed build keeps the shared materials).
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().WaitForCompletion();
	TArray<FString> Packages = {Folder + TEXT("M_FTGarment"), Folder + TEXT("M_FTGarmentCloth"), Folder + TEXT("T_FTGarmentFlatNormal"),
		Folder + TEXT("T_FTGarmentORM"), Folder + TEXT("T_FTGarmentWear"), Folder + TEXT("T_FTGarmentWhite")};
	for (ftcore::GarmentTile Tile : Tiles)
	{
		Packages.Add(Folder + TEXT("T_FTGarment") + TileName(Tile) + TEXT("N"));
		Packages.Add(Folder + TEXT("T_FTGarment") + TileName(Tile) + TEXT("M"));
	}
	for (const FSubstance& S : All)
	{
		Packages.Add(Folder + TEXT("MI_FTGarment_") + S.Name);
	}
	FSetAside Old;
	for (const FString& Package : Packages)
	{
		if (!Old.Add(PackageFile(Package)))
		{
			return 1;
		}
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(PackageFile(Folder + TEXT("M_FTGarment"))), true);

	TArray<UObject*> ToSave;
	TMap<ftcore::GarmentTile, TPair<UTexture*, UTexture*>> TileTextures;
	for (ftcore::GarmentTile Tile : Tiles)
	{
		const ftcore::GarmentTileSettings Settings = ftcore::garmentTileDefaults(Tile);
		std::vector<float> Height;
		if (!ftcore::makeGarmentHeight(Tile, Settings, Height))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildGarment: bad %s tile settings."), TileName(Tile));
			return 1;
		}
		std::vector<uint8_t> NormalBytes, MaskBytes;
		ftcore::skinNormalBgra(Height, Settings.size, Settings.normalStrength, NormalBytes);
		ftcore::skinMaskBgra(Height, Settings.size, MaskBytes);
		const int32 Size = static_cast<int32>(Settings.size);
		const TArray<uint8> NormalArray(NormalBytes.data(), static_cast<int32>(NormalBytes.size()));
		const TArray<uint8> MaskArray(MaskBytes.data(), static_cast<int32>(MaskBytes.size()));
		if (!DumpDir.IsEmpty())
		{
			IFileManager::Get().MakeDirectory(*DumpDir, true);
			FImageUtils::SaveImageByExtension(*(DumpDir / FString::Printf(TEXT("garment_%s_normal.png"), TileName(Tile))),
				FImageView(reinterpret_cast<const FColor*>(NormalArray.GetData()), Size, Size, EGammaSpace::Linear));
			FImageUtils::SaveImageByExtension(*(DumpDir / FString::Printf(TEXT("garment_%s_mask.png"), TileName(Tile))),
				FImageView(reinterpret_cast<const FColor*>(MaskArray.GetData()), Size, Size, EGammaSpace::Linear));
		}
		UTexture2D* N = NewTexture(Folder + TEXT("T_FTGarment") + TileName(Tile) + TEXT("N"), Size, NormalArray, TC_Normalmap);
		UTexture2D* M = NewTexture(Folder + TEXT("T_FTGarment") + TileName(Tile) + TEXT("M"), Size, MaskArray, TC_Masks);
		ToSave.Add(N);
		ToSave.Add(M);
		TileTextures.Add(Tile, TPair<UTexture*, UTexture*>(N, M));
	}

	FDefaults Defaults;
	Defaults.White = Solid(Folder + TEXT("T_FTGarmentWhite"), 255, 255, 255, TC_Default);
	Defaults.Orm = Solid(Folder + TEXT("T_FTGarmentORM"), 0, 128, 255, TC_Masks);  // B metal 0, G roughness 0.5, R occlusion 1
	Defaults.FlatNormal = Solid(Folder + TEXT("T_FTGarmentFlatNormal"), 255, 128, 128, TC_Normalmap);
	Defaults.Wear = Solid(Folder + TEXT("T_FTGarmentWear"), 0, 128, 128, TC_Masks); // R curvature 0.5: flat
	Defaults.DetailN = TileTextures[ftcore::GarmentTile::Leather].Key;
	Defaults.DetailM = TileTextures[ftcore::GarmentTile::Leather].Value;
	ToSave.Add(Defaults.White);
	ToSave.Add(Defaults.Orm);
	ToSave.Add(Defaults.FlatNormal);
	ToSave.Add(Defaults.Wear);

	UMaterial* Lit = BuildMaster(Folder + TEXT("M_FTGarment"), false, Defaults);
	UMaterial* Cloth = BuildMaster(Folder + TEXT("M_FTGarmentCloth"), true, Defaults);
	ToSave.Add(Lit);
	ToSave.Add(Cloth);
	for (const FSubstance& S : All)
	{
		const TPair<UTexture*, UTexture*>& Tile = TileTextures[S.Tile];
		ToSave.Add(BuildInstance(Folder + TEXT("MI_FTGarment_") + S.Name, S.bCloth ? Cloth : Lit, S, Tile.Key, Tile.Value));
	}

	FAssetCompilingManager::Get().FinishAllCompilation();
	if (FT::CompileErrors(TEXT("FTBuildGarment"), Lit) + FT::CompileErrors(TEXT("FTBuildGarment"), Cloth) > 0)
	{
		return 1;
	}
	bool bOk = true;
	for (UObject* Asset : ToSave)
	{
		bOk &= Save(Asset);
	}
	if (!bOk)
	{
		return 1;
	}
	Old.Commit();
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildGarment: built 2 masters (%d expressions each), %d substances, %d detail tiles."),
		Lit->GetExpressionCollection().Expressions.Num(), All.Num(), static_cast<int32>(UE_ARRAY_COUNT(Tiles)));
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildGarment needs an editor build."));
	return 1;
#endif
}
