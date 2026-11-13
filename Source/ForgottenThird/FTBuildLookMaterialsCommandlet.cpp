#include "FTBuildLookMaterialsCommandlet.h"

#include "FTJointCollection.h"
#include "FTSeason.h"

#include "ForgottenThird.h"
#include "FTSceneLook.h"
#include "FTText.h"

#include "ftcore/GlassSurface.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "FTImportMaterials.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionCameraPositionWS.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionLocalPosition.h"
#include "Materials/MaterialExpressionThinTranslucentMaterialOutput.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionSceneTexture.h"
#include "Materials/MaterialExpressionScreenPosition.h"
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
	// The sun's light gathered by the ripples into a net of bright lines (the underwater view's own, so the two lie on
	// each other): about 0.3 where the surface is flat, 1 and more on a line.
	float Net(float2 P, float T)
	{
		// The surface as six trains of waves, each bent a little by another. Its curvature turns the rays that come down
		// through it: a patch of them lands on an area (1 + Hxx)(1 + Hyy) - Hxy^2 of its own, and the light there is one
		// over that, so where the curvature brings them together the bed shows a bright line.
		const float2 Way[6] = {float2(1.0, 0.0), float2(0.5, 0.866), float2(-0.5, 0.866), float2(0.94, -0.342), float2(0.174, 0.985),
			float2(-0.766, 0.643)};
		const float Fine[6] = {1.0, 1.31, 1.73, 2.3, 2.9, 3.7};
		const float Pace[6] = {0.9, -1.1, 1.3, -0.8, 1.6, -1.4};
		float Hxx = 0.0;
		float Hyy = 0.0;
		float Hxy = 0.0;
		for (int K = 0; K < 6; ++K)
		{
			float2 Bent = Way[(K + 2) % 6];
			float Phase = dot(P, Way[K]) * Fine[K] * 2.2 + T * Pace[K] + K * 1.7 + 0.7 * sin(dot(P, Bent) * Fine[K] * 0.9 + T * 0.3);
			float Curve = -sin(Phase) * Fine[K] * 0.34 / (1.0 + 0.35 * K);
			Hxx += Curve * Way[K].x * Way[K].x;
			Hyy += Curve * Way[K].y * Way[K].y;
			Hxy += Curve * Way[K].x * Way[K].y;
		}
		float Spread = (1.0 + Hxx) * (1.0 + Hyy) - Hxy * Hxy;
		// (Eased where the rays cross, so a line is bright at its middle and soft at its edges.)
		return 0.3 / sqrt(Spread * Spread + 0.02);
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
FLookWaves W;
//REACT
Grad += ReactGrad;
float Foam = saturate(ReactFoam);
RoughOut = saturate(lerp(lerp(max(Rough, 0.18), Rough, Deep), 0.55, Foam));
FoamOut = Foam;
TopOut = FoamColor.rgb;
DarkOut = ReactDark * (1.0 - Foam);
return normalize(float3(-Grad.x, -Grad.y, 1.0));
)HLSL");

// What the lead does to a water surface, put into a surface's code where it says //REACT (P the world place in cm, W
// the noise, Time, EyeDepth and BehindDepth as the surface has them). Out of it: ReactGrad, a slope to add; ReactFoam,
// foam to add; ReactDark, how much the water is darkened. Its inputs are the water collection's (BuildWaterCollection):
//  - Ring0..Ring7: a ring each (place xy in cm, radius in cm, amplitude; ftcore::WaterRipples). A ring is a short train
//    of ripples on its front, the front widening as it travels, a fainter train behind it, and flecks of foam on it.
//  - Lead: where the lead stands in the water (xy in cm; z its half width at the surface in cm, 0 out of the water).
//    The waterline on the body: foam where the surface meets what stands in it (the depth behind it less than a
//    hand's width along the view), a churned ring close around it, and the water darker just beyond.
//  - Wake: the way the lead moves (xy), its speed (z, cm a second) and how much wake it draws (w, 0..1): two arms
//    behind it at the angle a hull's make (about 20 degrees each side), each a crest and a fainter one inside it,
//    short waves across between them, and a trail of foam down the middle, for as far as 2.8 s of travel.
const TCHAR* const kWaterReactCode = TEXT(R"HLSL(float2 ReactGrad = 0.0;
float ReactFoam = 0.0;
float ReactDark = 0.0;
{
	float4 ReactRings[8] = {Ring0, Ring1, Ring2, Ring3, Ring4, Ring5, Ring6, Ring7};
	for (int RingI = 0; RingI < 8; ++RingI)
	{
		if (ReactRings[RingI].w <= 0.0)
		{
			continue;
		}
		float2 RingRel = P - ReactRings[RingI].xy;
		float RingDst = max(length(RingRel), 1.0);
		float RingWide = 7.0 + 0.045 * ReactRings[RingI].z;
		float RingX = RingDst - ReactRings[RingI].z;
		float RingLag = RingX + 2.6 * RingWide;
		float RingEnv = exp(-RingX * RingX / (RingWide * RingWide)) + 0.4 * step(RingX, 0.0) * exp(-RingLag * RingLag / (5.0 * RingWide * RingWide));
		float RingAmp = ReactRings[RingI].w / (1.0 + ReactRings[RingI].z / 180.0);
		ReactGrad += (RingRel / RingDst) * (RingAmp * RingEnv * sin(RingX * 2.4 / RingWide));
		float RingFront = RingX - 0.3 * RingWide;
		ReactFoam += RingAmp * exp(-RingFront * RingFront / (0.4 * RingWide * RingWide));
	}
	ReactGrad *= RingSlope;
	float Fleck = W.Noise(P / 2.6 + Time * float2(0.6, -0.4));
	float Clump = W.Noise(P / 9.0 - Time * float2(0.2, 0.3));
	// (The foam is thin: scattered flecks on the fronts, never a sheet.)
	ReactFoam *= RingFoam * smoothstep(0.55, 0.95, Fleck);
	float Wading = step(1.0, Lead.z);
	float2 LeadRel = P - Lead.xy;
	float LeadDst = length(LeadRel);
	float LeadClose = Wading * (1.0 - smoothstep(Lead.z * 2.0, Lead.z * 4.5, LeadDst));
	float LeadGap = max(BehindDepth - EyeDepth, 0.0);
	// The line where the surface meets the body: a thin line of foam (the water no more than a finger deep over the
	// body along the view), in short lengths with gaps between them, thick and thin, never still; and just outside it
	// a dark band a hand wide where the water is drawn up against the body.
	float LeadBreak = smoothstep(0.4, 0.62, W.Noise(P / 3.1 + Time * float2(0.9, -0.7))) * lerp(0.55, 1.0, W.Noise(P / 11.0 - Time * float2(0.3, 0.2)));
	float LeadTouch = LeadClose * (1.0 - smoothstep(0.6, 3.0, LeadGap)) * LeadBreak;
	float LeadEdge = LeadClose * smoothstep(1.0, 4.0, LeadGap) * (1.0 - smoothstep(10.0, 24.0, LeadGap));
	float LeadOff = LeadDst - Lead.z;
	float LeadRing = Wading * exp(-LeadOff * LeadOff / max(0.18 * Lead.z * Lead.z, 1.0));
	ReactFoam += ContactFoam * (LeadTouch + 0.15 * LeadRing * smoothstep(0.5, 0.85, Clump));
	// (The dark is the water's own there: the surface's code hands it on as more absorption (DarkOut), so what stands
	// in the water goes dark just under the line. Laid on the water as a film it mirrored the sky and the walls, and
	// lay on the body as a pale veil.)
	ReactDark = ContactDark * LeadEdge;
	if (Wake.w > 0.002 && Wading > 0.0)
	{
		float2 WakeDir = normalize(Wake.xy + float2(1e-5, 0.0));
		float2 WakeSide = float2(-WakeDir.y, WakeDir.x);
		float WakeBack = -dot(LeadRel, WakeDir);
		float WakeOff = dot(LeadRel, WakeSide);
		float WakeLen = max(2.8 * Wake.z, 90.0);
		float WakeTail = saturate(1.0 - WakeBack / WakeLen) * smoothstep(-30.0, 10.0, WakeBack);
		const float TanK = 0.36;
		float Arm = abs(WakeOff) - (WakeBack + 25.0) * TanK;
		float ArmW = 6.0 + 0.045 * max(WakeBack, 0.0);
		float ArmEnv = exp(-Arm * Arm / (ArmW * ArmW));
		float2 ArmN = normalize((WakeOff >= 0.0 ? WakeSide : -WakeSide) + TanK * WakeDir);
		float Feather = lerp(0.55, 1.0, W.Noise(float2(WakeBack / 14.0, WakeOff / 30.0) + Time * 0.3));
		float Push = WakeSlope * Wake.w * WakeTail;
		ReactGrad += ArmN * (Push * Feather * ArmEnv * sin(Arm * 2.6 / ArmW));
		float Arm2 = Arm + 3.2 * ArmW;
		ReactGrad += ArmN * (0.5 * Push * exp(-Arm2 * Arm2 / (ArmW * ArmW)) * sin(Arm2 * 2.6 / ArmW));
		float Between = 1.0 - smoothstep(-ArmW, ArmW, Arm);
		float Cross = max(0.42 * Wake.z, 16.0);
		ReactGrad -= WakeDir * (0.4 * Push * Between * sin(WakeBack * 6.2832 / Cross) * smoothstep(0.0, 40.0, WakeBack));
		float TrailW = 12.0 + 0.1 * max(WakeBack, 0.0);
		float Trail = exp(-WakeOff * WakeOff / (TrailW * TrailW)) * WakeTail * smoothstep(0.0, 25.0, WakeBack);
		ReactFoam += WakeFoam * Wake.w * (Trail * smoothstep(0.6, 0.97, Fleck) + 0.7 * ArmEnv * WakeTail * smoothstep(0.6, 0.95, Fleck));
	}
}
)HLSL");

// The water's medium: the look's coefficients are per metre, the engine's per centimetre. Dark (the surface's DarkOut,
// 0..1) is the dark band where the water meets a body: up to 0.14 more absorption a centimetre, so what stands in the
// water a hand's width behind the surface is seen darkened.
const TCHAR* const kWaterMediumCode = TEXT(R"HLSL(ScatterOut = max(Scatter.rgb, 0.0) * 0.01 * max(Clear, 0.0);
return max(Absorb.rgb, 0.0) * 0.01 * max(Clear, 0.0) + 0.14 * saturate(Dark);
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

// A canal's surface: water that runs. Flow is the way it runs (xy, world) and its speed mid-channel (z, cm a second);
// Channel is the channel's middle (xy, world cm) and its half width across the flow (z, cm). The speed falls from the
// middle to an eighth at the walls. The ripples (two sizes, RippleCm and 0.37 of it) are carried along it by two
// copies half a period apart, each fading out before it jumps back and coming in on a new patch, so the surface
// travels without a visible reset however unevenly it is sheared. A ragged line of scum lies along each wall; the open
// water carries none. Where the water lies less than 25 cm deep over what's behind it the ripples calm, as the look's
// water's do.
const TCHAR* const kCanalWaterCode = FT_LOOK_NOISE_STRUCT(FLookCanal) TEXT(R"HLSL(FLookCanal W;
float2 P = WorldPos.xy;
float2 Dir = normalize(Flow.xy + float2(1e-5, 0.0));
float2 Perp = float2(-Dir.y, Dir.x);
float2 Rel = P - Channel.xy;
float Across = dot(Rel, Perp) / max(Channel.z, 1.0);
float Profile = saturate(1.0 - Across * Across);
float Speed = max(Flow.z, 0.0) * lerp(0.125, 1.0, Profile) * max(WaveSpeed, 0.0);
const float Period = 4.0;
float Cycle = Time / Period;
float Cell = max(RippleCm, 1.0);
float2 Grad = 0.0;
float Foam = 0.0;
float Norm = 0.0;
for (int K = 0; K < 2; ++K)
{
	float C = Cycle + 0.5 * K;
	float Phase = frac(C);
	float Weight = 1.0 - abs(1.0 - 2.0 * Phase);
	float2 Jump = float2(37.3, 91.7) * (floor(C) + 0.5 * K);
	float2 U = P - Dir * Speed * Period * (Phase - 0.5);
	float2 G = 0.05 * W.Slope(U / Cell + Jump);
	float2 Q = float2(0.4535961 * U.x + 0.8912074 * U.y, 0.4535961 * U.y - 0.8912074 * U.x) / (0.37 * Cell);
	// The fine layer: one octave, its slope by central differences.
	float2 QJ = Q - Jump;
	const float E = 0.04;
	float2 F = float2(W.Noise(QJ + float2(E, 0.0)) - W.Noise(QJ - float2(E, 0.0)), W.Noise(QJ + float2(0.0, E)) - W.Noise(QJ - float2(0.0, E))) / (2.0 * E);
	G += 0.035 * float2(0.4535961 * F.x - 0.8912074 * F.y, 0.8912074 * F.x + 0.4535961 * F.y);
	Grad += Weight * G;
	// (Nothing is drawn floating on it: specks out of a noise read as pale dashes from above.)
	Norm += Weight * Weight;
}
Grad /= sqrt(max(Norm, 1e-4));
float Edge = smoothstep(0.975, 0.998, abs(Across)) * smoothstep(0.45, 0.75, W.Noise(float2(dot(Rel, Dir) / 40.0, Across * 3.0) + Time * 0.02));
// Too fine to draw far off: the flecks fade out between 25 and 60 m.
Foam = saturate(max(Foam, 0.7 * Edge) * FoamAmount * saturate(1.0 - (EyeDepth - 2500.0) / 3500.0));
float Deep = saturate(max(BehindDepth - EyeDepth, 0.0) / 25.0);
Grad *= max(WaveStrength, 0.0) * RippleStrength * lerp(0.4, 1.0, Deep) * lerp(0.6, 1.0, Profile);
// A slow broad swell under the ripples, drifting: the surface is never a flat sheet.
float2 SwellAt = P / 380.0 + Time * float2(0.05, -0.03);
Grad += 0.02 * max(WaveStrength, 0.0) * float2(cos(SwellAt.x + 1.7 * sin(SwellAt.y)), 0.8 * cos(1.3 * SwellAt.y + 1.1 * sin(0.7 * SwellAt.x)));
// Cat's paws: patches of very fine ripples that the air lays on it and takes away again, drifting; the sun breaks
// into glints on them. Too fine to draw far off.
float Gust = smoothstep(0.42, 0.7, W.Noise(P / 520.0 + Time * float2(0.021, 0.013))) * saturate(1.0 - (EyeDepth - 1800.0) / 2500.0);
if (Gust * FineStrength > 0.001)
{
	float2 Mq = P / 5.5 + Time * float2(0.7, 0.45);
	const float Me = 0.08;
	float2 Mg = float2(W.Noise(Mq + float2(Me, 0.0)) - W.Noise(Mq - float2(Me, 0.0)), W.Noise(Mq + float2(0.0, Me)) - W.Noise(Mq - float2(0.0, Me))) / (2.0 * Me);
	Grad += 0.03 * FineStrength * Gust * Mg;
}
// What the lead does to it: rings, the waterline on the body, the wake (kWaterReactCode). Its foam is paler than the scum.
//REACT
Grad += ReactGrad;
float Froth = saturate(ReactFoam);
Foam = saturate(Foam + Froth);
RoughOut = saturate(lerp(max(Rough, 0.02), 0.55, Foam));
FoamOut = Foam;
TopOut = lerp(FoamColor.rgb, float3(0.5, 0.52, 0.5), saturate(Froth / max(Foam, 1e-3)));
DarkOut = ReactDark * (1.0 - Foam);
return normalize(float3(-Grad.x, -Grad.y, 1.0));
)HLSL");

// A canal's walls and bed, one material on the channel's boxes, laid on the world position. Faces looking up under
// the water are the bed: silt mottled light and dark, drifts of gravel (pebbles a few centimetres across), a stone
// here and there, dark drifts of weed in fine strands, and the sun's net of light moving over it all (the water
// collection's Sun and BedCaustic). The others, and faces looking up over the water (steps, a slip), are the walls:
// concrete, stained down its height, with a dark wet band from the waterline (WaterZ, world cm) up to a ragged edge
// 10 to 30 cm over it, a green line of growth just above the water and dark slime under it.
const TCHAR* const kCanalBedCode = FT_LOOK_NOISE_STRUCT(FLookBed) TEXT(R"HLSL(FLookBed B;
float3 Up = normalize(VertexNormal);
float H = WorldPos.z - WaterZ;
// Faces that look up are the bed where the water lies deep on them (a slope the lead walks too): silt settles from
// some 50 cm under the surface and covers them from 1.2 m down. Over the water and just under it they are the
// wall's concrete (a flight of steps, a slip), so a step or a ramp is seen going on down into the water.
float Level = smoothstep(0.5, 0.75, Up.z);
float Floor = Level * (1.0 - smoothstep(-120.0, -50.0, H));
float2 P = WorldPos.xy;
float Silt = B.Height(P / 170.0 + 2.3);
float Grit = B.Height(P / 9.0 + 7.9);
float Fine = B.Noise(P / 1.9 + 3.3);
float3 Bed = lerp(float3(0.11, 0.10, 0.075), float3(0.25, 0.225, 0.17), Silt) * lerp(0.85, 1.12, Grit) * lerp(0.9, 1.08, Fine);
// Gravel: pebbles a few centimetres across, thick in drifts and thin between them, each its own grey.
float Drift = smoothstep(0.3, 0.7, B.Height(P / 240.0 + 13.0));
// (Their field is turned off the lattice's axes and pushed about a little by a second, broader one, so no pebble
// shows a square side; pushed far by a fine one, every outline broke into dark squiggles seen from above.)
float2 Warp = float2(B.Noise(P / 13.0 + 5.0), B.Noise(P / 13.0 + 19.0)) - 0.5;
float2 Pq = float2(0.799 * P.x - 0.602 * P.y, 0.602 * P.x + 0.799 * P.y) / 4.9 + 0.7 * Warp + 11.0;
float Pebble = smoothstep(0.56, 0.86, B.Height(Pq)) * lerp(0.05, 0.6, Drift);
float3 PebbleColor = Bed * lerp(0.7, 1.45, B.Noise(Pq * 0.71 + 40.0));
Bed = lerp(Bed, PebbleColor, Pebble);
// A stone here and there, a hand or two across, half sunk in the silt, mottled as the silt is.
float2 StoneAt = float2(0.799 * P.x + 0.602 * P.y, 0.799 * P.y - 0.602 * P.x) / 24.0 + 0.3 * Warp + 11.0;
float StoneField = B.Height(StoneAt);
float Stone = smoothstep(0.70, 0.80, StoneField);
float3 StoneColor = lerp(float3(0.12, 0.12, 0.11), float3(0.25, 0.24, 0.21), B.Noise(P / 37.0 + 5.1)) * lerp(0.7, 1.15, Grit) * lerp(0.85, 1.1, Fine);
Bed = lerp(Bed, StoneColor, Stone);
// Weed: dark soft drifts a few metres across, thicker and thinner within over a hand's width or more; no fine marks.
float Weed = smoothstep(0.5, 0.8, B.Height(P / 330.0 + 21.0)) * lerp(0.45, 1.0, B.Height(float2(0.602 * P.x + 0.799 * P.y, 0.799 * P.x - 0.602 * P.y) / 46.0 + 1.7)) * (1.0 - Stone);
Bed = lerp(Bed, float3(0.05, 0.08, 0.035), 0.55 * Weed);
float2 TurnedSlope = B.Slope(StoneAt) * Stone * (1.0 - smoothstep(0.80, 0.9, StoneField)) * 0.3;
float2 BedSlope = float2(0.799 * TurnedSlope.x - 0.602 * TurnedSlope.y, 0.602 * TurnedSlope.x + 0.799 * TurnedSlope.y);
float Along = WorldPos.x + WorldPos.y;
float3 Wall = float3(0.36, 0.35, 0.33) * lerp(0.75, 1.1, B.Height(float2(Along, WorldPos.z) / 95.0));
Wall *= lerp(0.7, 1.0, smoothstep(0.25, 0.6, B.Noise(float2(Along / 17.0, WorldPos.z / 210.0))));
float Ragged = B.Height(float2(Along / 41.0, 3.3));
float Wet = 1.0 - smoothstep(4.0, 10.0 + 20.0 * Ragged, H);
float Growth = (1.0 - smoothstep(2.0, 9.0 + 8.0 * Ragged, H)) * smoothstep(-6.0, 0.0, H);
// (The slime under the line is thick on a wall's face; on a tread or a ramp, where feet and the wash keep it down, it
// is thin at the line and thickens over the first metre down.)
float Under = (1.0 - smoothstep(-8.0, 0.0, H)) * lerp(1.0, 0.05 + 0.3 * saturate(-H / 100.0), Level);
// A tread or a ramp under the water keeps most of its concrete's own lightness (the dark of a wet face in the air is
// its sheen's doing), so from above it is seen going on down through the first metre, the water's own colour and the
// silt taking it over with depth.
float Sunken = Level * (1.0 - smoothstep(-6.0, 0.0, H));
Wall = lerp(Wall, 0.42 * Wall, Wet * (1.0 - 0.9 * Sunken));
Wall = lerp(Wall, float3(0.07, 0.11, 0.035), 0.7 * Growth);
Wall = lerp(Wall, float3(0.05, 0.065, 0.035), 0.8 * Under);
NormalOut = normalize(Up + Floor * float3(-BedSlope.x, -BedSlope.y, 0.0) * 0.9);
RoughOut = lerp(lerp(0.85, 0.22, Wet), 0.7, Floor);
// The sun's net on what lies under the water, seen from above too: where the underwater view lays it (cast along the
// sun's way under water), strongest a little under the surface, fading with depth; faint on the walls.
float Sunk = max(-H, 0.0);
float3 SunW = normalize(Sun.xyz + float3(0.0, 0.0, 1e-3));
float2 Lit = (WorldPos.xy + SunW.xy * (Sunk / max(SunW.z, 0.3))) * 0.01;
float Focus = saturate(Sunk / 12.0) * exp(-0.3 * Sunk * 0.01) * saturate(Sun.w) * lerp(0.35, 1.0, Level);
float3 Color = lerp(Wall, Bed, Floor);
Color *= max(1.0 + BedCaustic * Focus * (B.Net(Lit * 4.0, Time) - 0.3), 0.5);
return clamp(Color, 0.01, 1.0);
)HLSL");

// The sea's bed near the shore, on a skin over the ground there: rippled sand, pale where the ripples crest, scattered
// stones, weed in dark drifts, and the sun's net as on the canal's bed. WaterZ is the sea's level (world cm).
const TCHAR* const kSeaBedCode = FT_LOOK_NOISE_STRUCT(FLookSeaBed) TEXT(R"HLSL(FLookSeaBed B;
float3 Up = normalize(VertexNormal);
float2 P = WorldPos.xy;
float Broad = B.Height(P / 420.0 + 4.1);
// Ripple marks: bands some 14 cm apart, bent and broken by the noise.
float Bend = B.Height(P / 95.0 + 9.3) * 6.0 + B.Noise(P / 31.0) * 1.4;
float Phase = (P.x * 0.82 + P.y * 0.57) / 14.0 + Bend;
float Mark = 0.5 + 0.5 * sin(Phase * 6.2832 / 3.0);
float MarkOn = smoothstep(0.25, 0.6, B.Noise(P / 260.0 + 17.0));
float3 Sand = lerp(float3(0.34, 0.29, 0.20), float3(0.50, 0.44, 0.31), Broad) * lerp(1.0, lerp(0.82, 1.1, Mark), MarkOn);
Sand *= lerp(0.9, 1.08, B.Noise(P / 2.3 + 3.0));
float StoneField = B.Height(P / 21.0 + 11.0);
float Stone = smoothstep(0.63, 0.68, StoneField);
float Pebble = smoothstep(0.72, 0.78, B.Noise(P / 5.0 + 23.0)) * smoothstep(0.4, 0.7, B.Noise(P / 140.0 + 31.0));
float3 StoneColor = lerp(float3(0.17, 0.17, 0.16), float3(0.40, 0.38, 0.34), B.Noise(P / 27.0 + 5.1));
float3 Bed = lerp(Sand, StoneColor, max(Stone, Pebble));
float Weed = smoothstep(0.55, 0.72, B.Height(P / 380.0 + 21.0)) * smoothstep(0.25, 0.6, B.Noise(P / 9.0 + 1.7)) * (1.0 - Stone);
Bed = lerp(Bed, float3(0.04, 0.075, 0.03), 0.85 * Weed);
float2 MarkSlope = float2(0.82, 0.57) * cos(Phase * 6.2832 / 3.0) * 0.22 * MarkOn;
float2 StoneSlope = B.Slope(P / 21.0 + 11.0) * Stone * (1.0 - smoothstep(0.68, 0.82, StoneField));
NormalOut = normalize(Up + float3(-MarkSlope.x - StoneSlope.x, -MarkSlope.y - StoneSlope.y, 0.0) * (1.0 - Weed));
RoughOut = lerp(0.8, 0.6, Stone);
float Sunk = max(WaterZ - WorldPos.z, 0.0);
float3 SunW = normalize(Sun.xyz + float3(0.0, 0.0, 1e-3));
float2 Lit = (WorldPos.xy + SunW.xy * (Sunk / max(SunW.z, 0.3))) * 0.01;
float Focus = saturate(Sunk / 12.0) * exp(-0.3 * Sunk * 0.01) * saturate(Sun.w);
Bed *= max(1.0 + BedCaustic * Focus * (B.Net(Lit * 4.0, Time) - 0.3), 0.5);
return clamp(Bed, 0.01, 1.0);
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

// The same from one of `From`'s further outputs (a custom node's additional outputs follow its return value).
void AddInputAt(UMaterialExpressionCustom* Custom, FName Name, UMaterialExpression* From, int32 Output)
{
	FCustomInput& In = Custom->Inputs.AddDefaulted_GetRef();
	In.InputName = Name;
	In.Input.Connect(Output, From);
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

// The view from under a water surface (BuildUnderwater), a post-process. Per pixel: the point where its ray crosses
// the near plane is under the surface (WaterZ, world cm) or not, so a half-sunk view cuts at the waterline: there a
// thin band of the view is smeared (the meniscus), with a bright lip over the line and a dark one under it. Under it,
// the scene is seen through the water the ray crosses (to what it meets, or to the surface):
//  - The water has its own colour by its kind (Tint near the surface, DeepTint far down) and takes Extinct of red,
//    green and blue a metre, so what is far goes to the water's colour and what is deep goes darker and bluer. Its
//    light is what the frame has (four taps: their mean and their brightest, half each).
//  - What is under water carries the sun's net of bright lines (caustics), cast along the sun's way under water
//    (the water collection's Sun), strongest a little under the surface and fading with depth.
//  - Shafts of that light stand in the water along the same way, drifting slowly (twelve steps along the ray).
//  - Specks drift in it: one in some cells of a 14 cm grid fixed in the world, so they slide past as the eye moves.
//  - A ray that leaves through the surface sees the world above through a bright window (bent by the ripples and the
//    lead's rings) inside the critical angle. Past it the surface is a mirror of what is under water: the ray is
//    turned back down at the surface and followed in the frame's own depth until it meets the bed, a wall or a body
//    (kUnderMirrorUvCode); where the frame has nothing for it, the water's own colour. What stands in the surface
//    close behind it (a body, a wall) is not mirrored away: it is seen through, bent at the line (kUnderPierceUvCode).
const TCHAR* const kUnderRippleCode = TEXT(R"HLSL(float3 V = WorldPos - CamPos;
float Dist = max(length(V), 1.0);
float3 D = V / Dist;
float T = (D.z > 1e-4 && CamPos.z < WaterZ) ? (WaterZ - CamPos.z) / D.z : 0.0;
float2 Hc = (CamPos + D * T).xy;
float2 H = Hc * 0.01;
float2 G = float2(cos(H.x * 9.1 + H.y * 2.3 + Time * 1.6), cos(H.y * 7.7 - H.x * 1.9 - Time * 1.4)) * 0.35;
G += float2(cos(H.x * 21.7 - H.y * 12.1 + Time * 2.6), cos(H.y * 24.3 + H.x * 9.7 + Time * 2.2)) * 0.35;
G += float2(cos(H.x * 47.0 + H.y * 35.0 - Time * 3.6), cos(H.y * 53.0 - H.x * 29.0 + Time * 3.1)) * 0.3;
G *= Strength / (1.0 + T * 0.0015);
float4 Rings[8] = {Ring0, Ring1, Ring2, Ring3, Ring4, Ring5, Ring6, Ring7};
float2 RingG = 0.0;
for (int R = 0; R < 8; ++R)
{
	float2 Rel = Hc - Rings[R].xy;
	float Dst = max(length(Rel), 1.0);
	float Wide = 7.0 + 0.045 * Rings[R].z;
	float X = Dst - Rings[R].z;
	RingG += (Rel / Dst) * (Rings[R].w / (1.0 + Rings[R].z / 180.0) * exp(-X * X / (Wide * Wide)) * sin(X * 2.4 / Wide));
}
G += RingG * RingUnder * step(1e-3, T);
return float3(G, T);)HLSL");

// Where the window's sample is taken (viewport UV), and PierceOut: how much what the ray meets stands in the surface:
// close behind it along the ray (1 within PierceNear cm of the surface, 0 from PierceFar), or the lead's own body where
// it stands in the water (the water collection's Lead), whole. Such a thing is not mirrored away; it is seen through
// the surface with its first hand's breadths over the line drawn out (by Bend at the line, easing to nothing over
// BendFade cm), so its outline breaks at the line as a straw's does in a glass.
const TCHAR* const kUnderPierceUvCode = TEXT(R"HLSL(float3 V = WorldPos - CamPos;
float Dist = max(length(V), 1.0);
float T = Ripple.z;
float2 Out = UV;
PierceOut = 0.0;
if (T > 0.0 && Dist > T)
{
	PierceOut = 1.0 - smoothstep(PierceNear, max(PierceFar, PierceNear + 1.0), Dist - T);
	PierceOut = max(PierceOut, step(1.0, Lead.z) * (1.0 - smoothstep(60.0, 110.0, length(WorldPos.xy - Lead.xy))));
	float Over = max(WorldPos.z - WaterZ, 0.0);
	float3 S = WorldPos;
	S.z = WaterZ + Over * (1.0 - PierceOut * (1.0 - 1.0 / max(Bend, 1.0)) * exp(-Over / max(BendFade, 1.0)));
	float4 Clip = mul(float4(S - CamPos + ResolvedView.TranslatedWorldCameraOrigin, 1.0), ResolvedView.TranslatedWorldToClip);
	// The ripples bend the window's view; a body that stands in the surface is kept whole, so they move it far less.
	float2 Plain = Clip.xy / max(Clip.w, 1.0) * float2(0.5, -0.5) + 0.5;
	Out = Plain + Ripple.xy * (0.22 * lerp(1.0, PierceRipple, PierceOut));
	// A bent sample that leaves the body (onto the sky beside it) is given up for the plain one.
	if (PierceOut > 0.5)
	{
		float There = CalcSceneDepth(ViewportUVToBufferUV(saturate(Out)));
		float Here = CalcSceneDepth(ViewportUVToBufferUV(saturate(Plain)));
		Out = abs(There - Here) > 40.0 ? Plain : Out;
	}
}
return clamp(Out, 0.002, 0.998);)HLSL");

// The mirror past the critical angle: the ray turned back down at the surface (its normal bent by the ripples) and
// followed through the frame's own depth, fourteen steps to a little past the bed (BedZ, world cm; no further than
// MirrorReach cm), then five halvings onto what it went behind. Out: the viewport UV of that point; HitOut, 1 where something
// was met (fading at the frame's edge); PathOut, the metres of water from the surface to it.
const TCHAR* const kUnderMirrorUvCode = TEXT(R"HLSL(float3 V = WorldPos - CamPos;
float Dist = max(length(V), 1.0);
float3 D = V / Dist;
float T = Ripple.z;
HitOut = 0.0;
PathOut = 0.0;
float2 Out = UV;
if (T > 0.0 && Dist > T)
{
	float3 H = CamPos + D * T;
	// (The mirror takes a small share of the ripples' slope, less the further off the surface is met: bent by more,
	// what it shows broke into a busy net of blobs overhead.)
	float3 N = normalize(float3(-Ripple.xy * (0.4 * MirrorRipple / (1.0 + T / 500.0)), 1.0));
	float3 R = D - 2.0 * dot(D, N) * N;
	R.z = min(R.z, -0.03);
	R = normalize(R);
	float Reach = min(max(H.z - BedZ, 10.0) / (-R.z) * 1.08, MirrorReach);
	float3 Org = ResolvedView.TranslatedWorldCameraOrigin - CamPos;
	float Found = -1.0;
	float Before = 0.0;
	float Prev = 0.0;
	for (int I = 1; I <= 14; ++I)
	{
		float S = Reach * I / 14.0;
		float4 Clip = mul(float4(H + R * S + Org, 1.0), ResolvedView.TranslatedWorldToClip);
		float2 Uv = Clip.xy / max(Clip.w, 1.0) * float2(0.5, -0.5) + 0.5;
		float Scene = CalcSceneDepth(ViewportUVToBufferUV(saturate(Uv)));
		bool On = Uv.x > 0.003 && Uv.x < 0.997 && Uv.y > 0.003 && Uv.y < 0.997 && Clip.w > 1.0;
		if (Found < 0.0 && On && Scene < Clip.w - 3.0)
		{
			Found = S;
			Before = Prev;
		}
		Prev = S;
	}
	if (Found > 0.0)
	{
		float A = Before;
		float B = Found;
		for (int J = 0; J < 5; ++J)
		{
			float M = 0.5 * (A + B);
			float4 ClipM = mul(float4(H + R * M + Org, 1.0), ResolvedView.TranslatedWorldToClip);
			float2 UvM = ClipM.xy / max(ClipM.w, 1.0) * float2(0.5, -0.5) + 0.5;
			float SceneM = CalcSceneDepth(ViewportUVToBufferUV(saturate(UvM)));
			if (SceneM < ClipM.w - 3.0)
			{
				B = M;
			}
			else
			{
				A = M;
			}
		}
		float4 ClipB = mul(float4(H + R * B + Org, 1.0), ResolvedView.TranslatedWorldToClip);
		Out = ClipB.xy / max(ClipB.w, 1.0) * float2(0.5, -0.5) + 0.5;
		// Met, if the frame's depth there is the ray's own (within MirrorThick cm and 3% of it); else the ray only went
		// behind something nearer, and the frame does not hold what it meets.
		float Miss = abs(ClipB.w - CalcSceneDepth(ViewportUVToBufferUV(saturate(Out))));
		float Slack = MirrorThick + 0.03 * ClipB.w;
		float2 Edge = min(Out, 1.0 - Out);
		HitOut = saturate(min(Edge.x, Edge.y) / 0.04) * (1.0 - smoothstep(Slack, 2.0 * Slack, Miss));
		PathOut = B * 0.01;
	}
}
return clamp(Out, 0.002, 0.998);)HLSL");

const TCHAR* const kUnderWobbleUvCode = TEXT(R"HLSL(float2 W = float2(sin(UV.y * 38.0 + Time * 1.9), cos(UV.x * 31.0 + Time * 1.5));
return clamp(UV + W * Wobble, 0.002, 0.998);)HLSL");

const TCHAR* const kUnderwaterCode = TEXT(R"HLSL(struct FLookUnder
{
	float Hash(float3 P)
	{
		int3 I = int3(floor(P));
		uint H = (uint(I.x) * 73856093u) ^ (uint(I.y) * 19349663u) ^ (uint(I.z) * 83492791u);
		H ^= H >> 13;
		H *= 1274126177u;
		H ^= H >> 16;
		return float(H & 0xFFFFu) / 65535.0;
	}
	// The sun's light gathered by the ripples into a net of bright lines (the same in the beds' materials, so the two
	// lie on each other). About 0.3 where the surface is flat, 1 and more on a line.
	float Net(float2 P, float T)
	{
		// The surface as six trains of waves, each bent a little by another. Its curvature turns the rays that come down
		// through it: a patch of them lands on an area (1 + Hxx)(1 + Hyy) - Hxy^2 of its own, and the light there is one
		// over that, so where the curvature brings them together the bed shows a bright line.
		const float2 Way[6] = {float2(1.0, 0.0), float2(0.5, 0.866), float2(-0.5, 0.866), float2(0.94, -0.342), float2(0.174, 0.985),
			float2(-0.766, 0.643)};
		const float Fine[6] = {1.0, 1.31, 1.73, 2.3, 2.9, 3.7};
		const float Pace[6] = {0.9, -1.1, 1.3, -0.8, 1.6, -1.4};
		float Hxx = 0.0;
		float Hyy = 0.0;
		float Hxy = 0.0;
		for (int K = 0; K < 6; ++K)
		{
			float2 Bent = Way[(K + 2) % 6];
			float Phase = dot(P, Way[K]) * Fine[K] * 2.2 + T * Pace[K] + K * 1.7 + 0.7 * sin(dot(P, Bent) * Fine[K] * 0.9 + T * 0.3);
			float Curve = -sin(Phase) * Fine[K] * 0.34 / (1.0 + 0.35 * K);
			Hxx += Curve * Way[K].x * Way[K].x;
			Hyy += Curve * Way[K].y * Way[K].y;
			Hxy += Curve * Way[K].x * Way[K].y;
		}
		float Spread = (1.0 + Hxx) * (1.0 + Hyy) - Hxy * Hxy;
		// (Eased where the rays cross, so a line is bright at its middle and soft at its edges.)
		return 0.3 / sqrt(Spread * Spread + 0.02);
	}
	// Where the surface lets more light down and where less: broad soft patches, drifting slowly.
	float Beam(float2 P, float T)
	{
		float A = sin(P.x * 1.9 + 0.31 * T + 1.7 * sin(P.y * 1.1 - 0.23 * T));
		float B = sin(P.y * 2.3 - 0.27 * T + 1.5 * sin(P.x * 1.3 + 0.19 * T));
		float C = sin((P.x + P.y) * 4.3 + 0.4 * T + 1.2 * sin(P.x * 2.7 - P.y * 1.9));
		float S = saturate(0.5 + 0.24 * (A + B) + 0.13 * C);
		S *= S;
		return S * S * 2.5;
	}
};
FLookUnder U;
float3 V = WorldPos - CamPos;
float Dist = max(length(V), 1.0);
float3 D = V / Dist;
float CosF = max(dot(D, ResolvedView.ViewForward), 0.05);
float3 P0 = CamPos + D * (ResolvedView.NearPlane / CosF);
float Below = WaterZ - P0.z + 0.1 * sin(dot(P0.xy, float2(0.83, 0.61)) + Time * 2.3) + 0.05 * sin(dot(P0.xy, float2(-1.9, 2.3)) - Time * 3.1);
float3 Smear = 0.2 * (Scene.rgb + Up1.rgb + Up2.rgb + Dn1.rgb + Dn2.rgb);
float Band = (1.0 - smoothstep(0.0, max(MeniscusCm, 0.01), abs(Below))) * saturate(Meniscus);
if (Below <= 0.0)
{
	float Lip = exp(-(Below + 0.045) * (Below + 0.045) / 0.0009);
	return lerp(Scene.rgb, Smear, Band) * (1.0 + 0.35 * Lip * saturate(Meniscus));
}
float3 Col = lerp(Scene.rgb, Wobbled.rgb, saturate(Below / 1.5));
float TExit = (D.z > 1e-4 && CamPos.z < WaterZ) ? (WaterZ - CamPos.z) / D.z : 1e12;
float Far = min(Dist, TExit);
float Len = Far * 0.01;
float CamDepth = max(WaterZ - CamPos.z, 0.0) * 0.01;
float3 Lw = float3(0.3, 0.59, 0.11);
float4 Taps = float4(dot(Tap0.rgb, Lw), dot(Tap1.rgb, Lw), dot(Tap2.rgb, Lw), dot(Tap3.rgb, Lw));
float Lum = 0.5 * dot(Taps, 0.25) + 0.5 * max(max(Taps.x, Taps.y), max(Taps.z, Taps.w));
float3 SunW = normalize(Sun.xyz + float3(0.0, 0.0, 1e-3));
float SunOn = saturate(Sun.w);
float SunZ = max(SunW.z, 0.3);
float3 Ext = max(Extinct.rgb, 1e-3);
if (Dist <= TExit)
{
	float Sunk = max(WaterZ - WorldPos.z, 0.0);
	float2 Lit = (WorldPos.xy + SunW.xy * (Sunk / SunZ)) * 0.01;
	float Focus = saturate(Sunk / 12.0) * exp(-CausticFade * Sunk * 0.01);
	Col *= max(1.0 + CausticGain * SunOn * Focus * (U.Net(Lit * CausticScale, Time) - 0.3), 0.4);
}
float ExtMean = dot(Ext, 0.3333);
// The water's own light comes from about as far along the ray as the water lets the eye see, at the depth it has there.
float SeenFar = (1.0 - exp(-ExtMean * Len)) / ExtMean;
float Mid = clamp(CamDepth - D.z * SeenFar * 0.6, 0.0, 60.0);
float3 Water = lerp(Tint.rgb, DeepTint.rgb, 1.0 - exp(-Mid * DeepRate)) * exp(-Mid * DeepDark);
float3 Fog = Water * Lum * FogGain * lerp(0.75, 1.25, saturate(D.z * 0.5 + 0.5));
if (Dist > TExit)
{
	float3 N = normalize(float3(-Ripple.xy, 1.0));
	float CosI = saturate(dot(D, N));
	float Sin2T = 1.769 * (1.0 - CosI * CosI);
	float Fres = 1.0;
	if (Sin2T < 1.0)
	{
		float CosT = sqrt(1.0 - Sin2T);
		Fres = max(0.02 + 0.98 * pow(1.0 - CosT, 5.0), smoothstep(0.8, 1.0, Sin2T));
	}
	Fres *= 1.0 - saturate(Pierce);
	float Glint = Ripple.x + Ripple.y;
	// (The mirror's own shading by the slope is a share of the window's and eases off with distance, as its bending does.)
	float Sheen = Glint * MirrorRipple / (1.0 + TExit / 500.0);
	// Where the frame does not hold what the mirror shows, a bed is supposed at BedZ: its own dull colour in the frame's
	// light, seen through the water the turned ray crosses and shaded softly by the surface's slope, so the mirror is
	// neither one flat colour nor a pattern of its own.
	float3 Turned = D - 2.0 * dot(D, N) * N;
	float Down = max(WaterZ - BedZ, 20.0) / max(-Turned.z, 0.05);
	float3 BedKept = exp(-Ext * Down * 0.01);
	float3 BedCol = float3(0.20, 0.18, 0.13) * Lum * 1.6;
	float3 Shine = (BedCol * BedKept + Fog * (1.0 - BedKept)) * clamp(1.0 + 0.5 * Sheen, 0.75, 1.3);
	float3 Kept = exp(-Ext * MirrorPath);
	// What the mirror shows is the mean of five taps, spread wider the further off the surface is met (MirrorSoft).
	float3 Seen = 0.36 * Mirrored.rgb + 0.16 * (Mir1.rgb + Mir2.rgb + Mir3.rgb + Mir4.rgb);
	float3 Image = (Seen * Kept + Fog * (1.0 - Kept)) * MirrorGain * (0.88 + 0.6 * max(Sheen, -0.2));
	float3 Mirror = lerp(Shine, Image, saturate(MirrorHit));
	float3 Window = Refracted.rgb * WindowGain * max(1.0 + 1.4 * Glint, 0.45);
	// What stands in the surface is the same lit body above it as below: no window's glare, a little dimmer.
	Window = lerp(Window, Refracted.rgb * PierceGain * clamp(1.0 + 0.35 * Glint, 0.85, 1.15), saturate(Pierce));
	Col = lerp(Window, Mirror, Fres);
}
)HLSL") TEXT(R"HLSL(// (The code is two literals: one would pass the compiler's length for a string.)
// The shafts: the light's patches on the surface are drawn out three times along the sun's own heading, as the swell
// that makes them is, so they stand as curtains seen along it and as slanted beams seen across it.
float Shaft = 0.0;
float Reach = min(Len, ShaftReach);
float2 Heading = normalize(SunW.xy + float2(1e-4, 0.0));
for (int I = 0; I < 12; ++I)
{
	float S = (I + 0.5) / 12.0 * Reach;
	float3 Ps = CamPos + D * (S * 100.0);
	float Sunk = WaterZ - Ps.z;
	float2 Lit = (Ps.xy + SunW.xy * (max(Sunk, 0.0) / SunZ)) * 0.01;
	float2 Drawn = float2(dot(Lit, Heading) * 0.33, Lit.x * Heading.y - Lit.y * Heading.x);
	Shaft += step(0.0, Sunk) * U.Beam(Drawn * ShaftScale, Time) * exp(-ShaftFade * Sunk * 0.01 - ExtMean * S);
}
Shaft *= Reach / 12.0;
float Toward = saturate(dot(D, SunW) * 0.5 + 0.5);
float3 Shafts = lerp(Water, 1.0, 0.5) * (Lum * ShaftGain * SunOn * Shaft * lerp(0.4, 1.7, Toward * Toward));
float3 Through = exp(-Ext * Len);
Col = Col * Through + Fog * (1.0 - Through) + Shafts;
float Motes = 0.0;
float3 Drift = Time * float3(1.3, 0.8, -0.5);
float3 Last = -1e9;
for (int M = 0; M < 16; ++M)
{
	float Tm = 16.0 + 12.5 * M;
	float3 Id = floor((CamPos + D * Tm + Drift) / 14.0);
	if (Tm < Far && any(Id != Last))
	{
		float3 Rnd = float3(U.Hash(Id), U.Hash(Id + 17.0), U.Hash(Id + 41.0));
		float3 Q = (Id + 0.15 + 0.7 * Rnd) * 14.0 - Drift + 0.7 * sin(Time * (0.4 + Rnd.yzx) + 6.2832 * Rnd.zxy);
		float3 Tq = Q - CamPos;
		float Along = dot(Tq, D);
		float Off = length(Tq - D * Along);
		float Rad = 0.07 + 0.12 * U.Hash(Id + 7.0);
		float Blur = Rad + 0.0011 * Along + 0.45 * saturate(1.0 - Along / 45.0);
		float Seen = step(0.45, U.Hash(Id + 29.0)) * step(8.0, Along) * step(Along, Far);
		Motes += Seen * (1.0 - smoothstep(0.0, Blur, Off)) * (Rad * Rad / (Blur * Blur)) * exp(-Along / 170.0);
	}
	Last = Id;
}
Col += lerp(Water, 1.0, 0.6) * (Lum * MoteGain * Motes);
Col *= lerp(1.0, 0.55, saturate(CamDepth / 8.0));
Col *= 1.0 - 0.45 * (1.0 - saturate(Below / 0.3));
Col = lerp(Col, 0.7 * Smear, 0.7 * Band);
Col *= 1.0 - 0.55 * saturate(Meniscus) * exp(-Below * Below / 0.0006);
return Col;)HLSL");
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

// The water collection (FTScene::LookWaterCollection): what the water subsystem writes each frame for every water
// surface and the underwater view to read, and the surfaces' own strengths (set by name with -WaterTune in a game run).
// Each parameter's id comes from its name, so a rebuild changes no id.
UMaterialParameterCollection* BuildWaterCollection()
{
	UPackage* Package = CreatePackage(FTScene::LookWaterCollection);
	UMaterialParameterCollection* Collection = NewObject<UMaterialParameterCollection>(Package,
		FName(FPackageName::GetShortName(Package->GetName())), RF_Public | RF_Standalone | RF_Transactional);
	auto PutVector = [Collection](const FString& Name, const FLinearColor& Value)
	{
		FCollectionVectorParameter& P = Collection->VectorParameters.AddDefaulted_GetRef();
		P.ParameterName = FName(*Name);
		P.DefaultValue = Value;
		P.Id = FGuid::NewDeterministicGuid(FString(TEXT("FTWater.")) + Name);
	};
	auto PutScalar = [Collection](const TCHAR* Name, float Value)
	{
		FCollectionScalarParameter& P = Collection->ScalarParameters.AddDefaulted_GetRef();
		P.ParameterName = Name;
		P.DefaultValue = Value;
		P.Id = FGuid::NewDeterministicGuid(FString(TEXT("FTWater.")) + Name);
	};
	// A ring: place (xy, cm), radius (cm), amplitude. Lead: place (xy, cm), half width at the surface (cm; 0 out of the
	// water). Wake: the way (xy), speed (cm a second), amount. Sun: the way to it under water (xyz), how much of it (w).
	for (int32 I = 0; I < 8; ++I)
	{
		PutVector(FString::Printf(TEXT("Ring%d"), I), FLinearColor(0.f, 0.f, 0.f, 0.f));
	}
	PutVector(TEXT("Lead"), FLinearColor(0.f, 0.f, 0.f, 0.f));
	PutVector(TEXT("Wake"), FLinearColor(1.f, 0.f, 0.f, 0.f));
	PutVector(TEXT("Sun"), FLinearColor(0.f, 0.f, 1.f, 0.f));
	PutScalar(TEXT("RingSlope"), 0.5f);
	PutScalar(TEXT("RingFoam"), 0.35f);
	PutScalar(TEXT("ContactFoam"), 0.75f);
	PutScalar(TEXT("ContactDark"), 0.85f);
	// A canal's own: how much of the look's absorption and scattering its water keeps (so its bed shows through a
	// metre of it and the deep middle goes dark), its specks, its ripples and cat's paws; the sea's share likewise;
	// the sun's net on a bed seen from above.
	PutScalar(TEXT("CanalClear"), 0.5f);
	PutScalar(TEXT("CanalScum"), 0.1f);
	PutScalar(TEXT("CanalRipple"), 0.4f);
	PutScalar(TEXT("CanalFine"), 1.f);
	PutScalar(TEXT("SeaClear"), 0.45f);
	PutScalar(TEXT("BedCaustic"), 0.9f);
	PutScalar(TEXT("WakeSlope"), 0.45f);
	PutScalar(TEXT("WakeFoam"), 0.22f);
	Collection->PostEditChange();
	return Collection;
}

// The inputs kWaterReactCode reads, from the water collection.
void AddReactInputs(UMaterial* Material, UMaterialExpressionCustom* Custom, UMaterialParameterCollection* Water, int32 Y)
{
	for (int32 I = 0; I < 8; ++I)
	{
		const FName Name(*FString::Printf(TEXT("Ring%d"), I));
		AddInput(Custom, Name, AddLook(Material, Water, Name, Y + 60 * I));
	}
	for (const TCHAR* Name : {TEXT("Lead"), TEXT("Wake"), TEXT("RingSlope"), TEXT("RingFoam"), TEXT("ContactFoam"), TEXT("ContactDark"), TEXT("WakeSlope"), TEXT("WakeFoam")})
	{
		Y += 60;
		AddInput(Custom, Name, AddLook(Material, Water, Name, Y + 480));
	}
}

// A surface's code with the lead's part put in.
FString WithReact(const TCHAR* Code)
{
	return FString(Code).Replace(TEXT("//REACT"), kWaterReactCode, ESearchCase::CaseSensitive);
}

UMaterial* BuildWater(UMaterialParameterCollection* Look, UMaterialParameterCollection* WaterParams)
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

	UMaterialExpressionCustom* Waves = AddCustom(Material, TEXT("FTLookWaves"), *WithReact(FTLookMaterialsBuild::kWaterCode), CMOT_Float3, 0);
	AddInput(Waves, TEXT("WorldPos"), AddExpression<UMaterialExpressionWorldPosition>(Material, -900, 0));
	AddInput(Waves, TEXT("Time"), AddExpression<UMaterialExpressionTime>(Material, -900, 100));
	AddInput(Waves, TEXT("WaveCm"), AddLook(Material, Look, TEXT("WaterWaveCm"), 200));
	AddInput(Waves, TEXT("WaveSpeed"), AddLook(Material, Look, TEXT("WaterWaveSpeed"), 300));
	AddInput(Waves, TEXT("WaveStrength"), AddLook(Material, Look, TEXT("WaterWaveStrength"), 400));
	AddInput(Waves, TEXT("Rough"), AddLook(Material, Look, TEXT("WaterRoughness"), 500));
	AddInput(Waves, TEXT("EyeDepth"), AddExpression<UMaterialExpressionPixelDepth>(Material, -900, 600));
	AddInput(Waves, TEXT("BehindDepth"), AddExpressionOf(Material, BehindClass, -900, 700));
	AddInput(Waves, TEXT("FoamColor"), AddVector(Material, TEXT("FoamColor"), FLinearColor(0.62f, 0.66f, 0.66f), 760));
	AddReactInputs(Material, Waves, WaterParams, 1300);
	AddOutput(Waves, TEXT("RoughOut"), CMOT_Float1);
	AddOutput(Waves, TEXT("FoamOut"), CMOT_Float1);
	AddOutput(Waves, TEXT("TopOut"), CMOT_Float3);
	AddOutput(Waves, TEXT("DarkOut"), CMOT_Float1);
	Waves->RebuildOutputs();

	UMaterialExpressionCustom* Medium = AddCustom(Material, TEXT("FTLookWaterMedium"), FTLookMaterialsBuild::kWaterMediumCode, CMOT_Float3, 900);
	AddInput(Medium, TEXT("Absorb"), AddLook(Material, Look, TEXT("WaterAbsorb"), 900));
	AddInput(Medium, TEXT("Scatter"), AddLook(Material, Look, TEXT("WaterScatter"), 1000));
	AddInput(Medium, TEXT("Clear"), AddLook(Material, WaterParams, TEXT("SeaClear"), 1050));
	AddInputAt(Medium, TEXT("Dark"), Waves, 4);
	AddOutput(Medium, TEXT("ScatterOut"), CMOT_Float3);
	Medium->RebuildOutputs();

	UMaterialExpressionSingleLayerWaterMaterialOutput* Water = AddExpression<UMaterialExpressionSingleLayerWaterMaterialOutput>(Material, 0, 900);
	Water->AbsorptionCoefficients.Connect(0, Medium);
	Water->ScatteringCoefficients.Connect(1, Medium);
	Water->PhaseG.Connect(0, AddLook(Material, Look, TEXT("WaterPhaseG"), 1100));

	// Clear water over whatever lies under it, water's own reflectance (F0 0.02) and index; the only top layer is what
	// the lead stirs up (foam on its rings and wake, the line where it stands in the water).
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(3, Waves);
	Inputs->Opacity.Connect(2, Waves);
	Inputs->Specular.Connect(0, AddScalar(Material, TEXT("Specular"), 0.25f, -100));
	Inputs->Refraction.Connect(0, AddScalar(Material, TEXT("IOR"), 1.33f, -400));
	Inputs->Normal.Connect(0, Waves);
	Inputs->Roughness.Connect(1, Waves);
	Finish(Material);
	return Material;
}

// A canal's water (kCanalWaterCode): the look's water with a flow. Its Flow and Channel are set per canal on an
// instance by the map's builder; the medium, roughness and wave strength are the look's.
UMaterial* BuildCanalWater(UMaterialParameterCollection* Look, UMaterialParameterCollection* WaterParams)
{
	UMaterial* Material = NewMaterial(FTScene::LookCanalWaterMaterial);
	Material->SetShadingModel(MSM_SingleLayerWater);
	Material->BlendMode = BLEND_Opaque;
	Material->bTangentSpaceNormal = false;
	UClass* BehindClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.MaterialExpressionSceneDepthWithoutWater"));
	if (!BehindClass)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLookMaterials: the engine has no MaterialExpressionSceneDepthWithoutWater."));
		return nullptr;
	}
	UMaterialExpressionCustom* Waves = AddCustom(Material, TEXT("FTLookCanalWater"), *WithReact(FTLookMaterialsBuild::kCanalWaterCode), CMOT_Float3, 0);
	AddInput(Waves, TEXT("WorldPos"), AddExpression<UMaterialExpressionWorldPosition>(Material, -900, 0));
	AddInput(Waves, TEXT("Time"), AddExpression<UMaterialExpressionTime>(Material, -900, 100));
	AddInput(Waves, TEXT("Flow"), AddVector(Material, TEXT("Flow"), FLinearColor(1.f, 0.f, 25.f, 0.f), 150));
	AddInput(Waves, TEXT("Channel"), AddVector(Material, TEXT("Channel"), FLinearColor(0.f, 0.f, 350.f, 0.f), 200));
	AddInput(Waves, TEXT("RippleCm"), AddScalar(Material, TEXT("RippleCm"), 26.f, 250));
	AddInput(Waves, TEXT("RippleStrength"), AddLook(Material, WaterParams, TEXT("CanalRipple"), 300));
	AddInput(Waves, TEXT("FoamAmount"), AddLook(Material, WaterParams, TEXT("CanalScum"), 350));
	AddInput(Waves, TEXT("FineStrength"), AddLook(Material, WaterParams, TEXT("CanalFine"), 380));
	AddInput(Waves, TEXT("WaveSpeed"), AddLook(Material, Look, TEXT("WaterWaveSpeed"), 400));
	AddInput(Waves, TEXT("WaveStrength"), AddLook(Material, Look, TEXT("WaterWaveStrength"), 450));
	AddInput(Waves, TEXT("Rough"), AddLook(Material, Look, TEXT("WaterRoughness"), 500));
	AddInput(Waves, TEXT("EyeDepth"), AddExpression<UMaterialExpressionPixelDepth>(Material, -900, 600));
	AddInput(Waves, TEXT("BehindDepth"), AddExpressionOf(Material, BehindClass, -900, 700));
	AddInput(Waves, TEXT("FoamColor"), AddVector(Material, TEXT("FoamColor"), FLinearColor(0.30f, 0.29f, 0.20f), 760));
	AddReactInputs(Material, Waves, WaterParams, 1300);
	AddOutput(Waves, TEXT("RoughOut"), CMOT_Float1);
	AddOutput(Waves, TEXT("FoamOut"), CMOT_Float1);
	AddOutput(Waves, TEXT("TopOut"), CMOT_Float3);
	AddOutput(Waves, TEXT("DarkOut"), CMOT_Float1);
	Waves->RebuildOutputs();

	UMaterialExpressionCustom* Medium = AddCustom(Material, TEXT("FTLookWaterMedium"), FTLookMaterialsBuild::kWaterMediumCode, CMOT_Float3, 900);
	AddInput(Medium, TEXT("Absorb"), AddLook(Material, Look, TEXT("WaterAbsorb"), 900));
	AddInput(Medium, TEXT("Scatter"), AddLook(Material, Look, TEXT("WaterScatter"), 1000));
	AddInput(Medium, TEXT("Clear"), AddLook(Material, WaterParams, TEXT("CanalClear"), 1050));
	AddInputAt(Medium, TEXT("Dark"), Waves, 4);
	AddOutput(Medium, TEXT("ScatterOut"), CMOT_Float3);
	Medium->RebuildOutputs();

	UMaterialExpressionSingleLayerWaterMaterialOutput* Water = AddExpression<UMaterialExpressionSingleLayerWaterMaterialOutput>(Material, 0, 900);
	Water->AbsorptionCoefficients.Connect(0, Medium);
	Water->ScatteringCoefficients.Connect(1, Medium);
	Water->PhaseG.Connect(0, AddLook(Material, Look, TEXT("WaterPhaseG"), 1100));

	// The top layer is the scum (pale, as much of it as the code's foam) and the froth the lead stirs up; the dark
	// line where it stands in the water is the medium's (DarkOut).
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(3, Waves);
	Inputs->Opacity.Connect(2, Waves);
	Inputs->Specular.Connect(0, AddScalar(Material, TEXT("Specular"), 0.25f, -100));
	Inputs->Refraction.Connect(0, AddScalar(Material, TEXT("IOR"), 1.33f, -400));
	Inputs->Normal.Connect(0, Waves);
	Inputs->Roughness.Connect(1, Waves);
	Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
	Finish(Material);
	return Material;
}

// The view from under a water surface (kUnderwaterCode): a post-process before the tone curve. WaterZ and BedZ (world
// cm) and the water's colours by its kind (Tint, DeepTint, Extinct) are set each frame by the water subsystem on its
// instance; the rings and the sun under water come from the water collection. The other scalars are its strengths
// (-WaterTune sets them by name in a game run).
// Its pixel shader is compiled on first use, so a mistake in the custom node's own code passes this build and shows in
// a game run's log instead ("Failed to compile Material", and the engine's default post-process is drawn).
UMaterial* BuildUnderwater(UMaterialParameterCollection* WaterParams)
{
	UMaterial* Material = NewMaterial(FTScene::LookUnderwaterMaterial);
	Material->MaterialDomain = MD_PostProcess;
	Material->BlendableLocation = BL_SceneColorAfterDOF;
	UMaterialExpression* const WorldPos = AddExpression<UMaterialExpressionWorldPosition>(Material, -1400, 0);
	UMaterialExpression* const CamPos = AddExpression<UMaterialExpressionCameraPositionWS>(Material, -1400, 100);
	UMaterialExpression* const Time = AddExpression<UMaterialExpressionTime>(Material, -1400, 200);
	UMaterialExpression* const WaterZ = AddScalar(Material, TEXT("WaterZ"), 0.f, 300);
	UMaterialExpression* const BedZ = AddScalar(Material, TEXT("BedZ"), -300.f, 350);
	UMaterialExpression* const Screen = AddExpression<UMaterialExpressionScreenPosition>(Material, -1400, 400);
	auto SceneAt = [Material](UMaterialExpression* Uv, int32 Y)
	{
		UMaterialExpressionSceneTexture* Scene = AddExpression<UMaterialExpressionSceneTexture>(Material, -700, Y);
		Scene->SceneTextureId = PPI_PostProcessInput0;
		if (Uv)
		{
			Scene->Coordinates.Connect(0, Uv);
		}
		return Scene;
	};
	auto TapAt = [Material, &SceneAt](float U, float V, int32 Y)
	{
		UMaterialExpressionConstant2Vector* At = AddExpression<UMaterialExpressionConstant2Vector>(Material, -1000, Y);
		At->R = U;
		At->G = V;
		return SceneAt(At, Y);
	};
	// The view shifted up or down a little, for the meniscus's smear.
	auto ShiftAt = [Material, &SceneAt, Screen](float Shift, int32 Y)
	{
		UMaterialExpressionCustom* Uv = AddCustom(Material, TEXT("FTLookUnderShiftUv"),
			*FString::Printf(TEXT("return clamp(UV + float2(0.0, %.4f), 0.002, 0.998);"), Shift), CMOT_Float2, Y);
		AddInput(Uv, TEXT("UV"), Screen);
		return SceneAt(Uv, Y);
	};

	UMaterialExpressionCustom* Ripple = AddCustom(Material, TEXT("FTLookUnderRipple"), FTLookMaterialsBuild::kUnderRippleCode, CMOT_Float3, 500);
	AddInput(Ripple, TEXT("WorldPos"), WorldPos);
	AddInput(Ripple, TEXT("CamPos"), CamPos);
	AddInput(Ripple, TEXT("WaterZ"), WaterZ);
	AddInput(Ripple, TEXT("Time"), Time);
	AddInput(Ripple, TEXT("Strength"), AddScalar(Material, TEXT("RippleStrength"), 0.2f, 600));
	AddInput(Ripple, TEXT("RingUnder"), AddScalar(Material, TEXT("RingUnder"), 1.2f, 650));
	for (int32 I = 0; I < 8; ++I)
	{
		const FName Name(*FString::Printf(TEXT("Ring%d"), I));
		AddInput(Ripple, Name, AddLook(Material, WaterParams, Name, 3000 + 60 * I));
	}

	UMaterialExpressionCustom* PierceUv = AddCustom(Material, TEXT("FTLookUnderPierceUv"), FTLookMaterialsBuild::kUnderPierceUvCode, CMOT_Float2, 700);
	AddInput(PierceUv, TEXT("WorldPos"), WorldPos);
	AddInput(PierceUv, TEXT("CamPos"), CamPos);
	AddInput(PierceUv, TEXT("WaterZ"), WaterZ);
	AddInput(PierceUv, TEXT("UV"), Screen);
	AddInput(PierceUv, TEXT("Ripple"), Ripple);
	AddInput(PierceUv, TEXT("PierceNear"), AddScalar(Material, TEXT("PierceNear"), 70.f, 720));
	AddInput(PierceUv, TEXT("PierceFar"), AddScalar(Material, TEXT("PierceFar"), 240.f, 740));
	AddInput(PierceUv, TEXT("Bend"), AddScalar(Material, TEXT("Bend"), 1.33f, 760));
	AddInput(PierceUv, TEXT("BendFade"), AddScalar(Material, TEXT("BendFade"), 35.f, 765));
	AddInput(PierceUv, TEXT("PierceRipple"), AddScalar(Material, TEXT("PierceRipple"), 0.2f, 767));
	AddInput(PierceUv, TEXT("Lead"), AddLook(Material, WaterParams, TEXT("Lead"), 770));
	AddOutput(PierceUv, TEXT("PierceOut"), CMOT_Float1);
	PierceUv->RebuildOutputs();

	UMaterialExpressionCustom* MirrorUv = AddCustom(Material, TEXT("FTLookUnderMirrorUv"), FTLookMaterialsBuild::kUnderMirrorUvCode, CMOT_Float2, 780);
	AddInput(MirrorUv, TEXT("WorldPos"), WorldPos);
	AddInput(MirrorUv, TEXT("CamPos"), CamPos);
	AddInput(MirrorUv, TEXT("UV"), Screen);
	AddInput(MirrorUv, TEXT("Ripple"), Ripple);
	AddInput(MirrorUv, TEXT("BedZ"), BedZ);
	AddInput(MirrorUv, TEXT("MirrorReach"), AddScalar(Material, TEXT("MirrorReach"), 3000.f, 790));
	AddInput(MirrorUv, TEXT("MirrorThick"), AddScalar(Material, TEXT("MirrorThick"), 40.f, 795));
	UMaterialExpression* const MirrorRipple = AddScalar(Material, TEXT("MirrorRipple"), 0.3f, 797);
	AddInput(MirrorUv, TEXT("MirrorRipple"), MirrorRipple);
	AddOutput(MirrorUv, TEXT("HitOut"), CMOT_Float1);
	AddOutput(MirrorUv, TEXT("PathOut"), CMOT_Float1);
	MirrorUv->RebuildOutputs();
	// The mirror's four taps about its own (kUnderwaterCode's Seen): out by MirrorSoft of the view at 9 m and beyond,
	// by less nearer, so the mirror softens with distance.
	UMaterialExpression* const MirrorSoft = AddScalar(Material, TEXT("MirrorSoft"), 0.006f, 799);
	auto MirrorTap = [Material, &SceneAt, MirrorUv, Ripple, MirrorSoft](float U, float V, int32 Y)
	{
		UMaterialExpressionCustom* Uv = AddCustom(Material, TEXT("FTLookUnderMirrorTapUv"),
			*FString::Printf(TEXT("return clamp(At + float2(%.3f, %.3f) * (Soft * saturate(Ripple.z / 900.0)), 0.002, 0.998);"), U, V), CMOT_Float2, Y);
		AddInput(Uv, TEXT("At"), MirrorUv);
		AddInput(Uv, TEXT("Ripple"), Ripple);
		AddInput(Uv, TEXT("Soft"), MirrorSoft);
		return SceneAt(Uv, Y);
	};

	UMaterialExpressionCustom* WobbleUv = AddCustom(Material, TEXT("FTLookUnderWobbleUv"), FTLookMaterialsBuild::kUnderWobbleUvCode, CMOT_Float2, 800);
	AddInput(WobbleUv, TEXT("UV"), Screen);
	AddInput(WobbleUv, TEXT("Time"), Time);
	AddInput(WobbleUv, TEXT("Wobble"), AddScalar(Material, TEXT("Wobble"), 0.0022f, 900));

	UMaterialExpressionCustom* Under = AddCustom(Material, TEXT("FTLookUnderwater"), FTLookMaterialsBuild::kUnderwaterCode, CMOT_Float3, 0);
	Under->MaterialExpressionEditorX = -300;
	AddInput(Under, TEXT("WorldPos"), WorldPos);
	AddInput(Under, TEXT("CamPos"), CamPos);
	AddInput(Under, TEXT("WaterZ"), WaterZ);
	AddInput(Under, TEXT("BedZ"), BedZ);
	AddInput(Under, TEXT("Time"), Time);
	AddInput(Under, TEXT("UV"), Screen);
	AddInput(Under, TEXT("Scene"), SceneAt(nullptr, 1000));
	AddInput(Under, TEXT("Wobbled"), SceneAt(WobbleUv, 1100));
	AddInput(Under, TEXT("Refracted"), SceneAt(PierceUv, 1200));
	AddInputAt(Under, TEXT("Pierce"), PierceUv, 1);
	AddInput(Under, TEXT("Mirrored"), SceneAt(MirrorUv, 1250));
	AddInputAt(Under, TEXT("MirrorHit"), MirrorUv, 1);
	AddInputAt(Under, TEXT("MirrorPath"), MirrorUv, 2);
	AddInput(Under, TEXT("Mir1"), MirrorTap(1.f, 0.5f, 2640));
	AddInput(Under, TEXT("Mir2"), MirrorTap(-0.5f, 1.f, 2700));
	AddInput(Under, TEXT("Mir3"), MirrorTap(-1.f, -0.5f, 2760));
	AddInput(Under, TEXT("Mir4"), MirrorTap(0.5f, -1.f, 2820));
	AddInput(Under, TEXT("MirrorRipple"), MirrorRipple);
	AddInput(Under, TEXT("Up1"), ShiftAt(-0.009f, 2400));
	AddInput(Under, TEXT("Up2"), ShiftAt(-0.022f, 2460));
	AddInput(Under, TEXT("Dn1"), ShiftAt(0.009f, 2520));
	AddInput(Under, TEXT("Dn2"), ShiftAt(0.022f, 2580));
	AddInput(Under, TEXT("Tap0"), TapAt(0.5f, 0.2f, 1300));
	AddInput(Under, TEXT("Tap1"), TapAt(0.2f, 0.5f, 1400));
	AddInput(Under, TEXT("Tap2"), TapAt(0.8f, 0.5f, 1500));
	AddInput(Under, TEXT("Tap3"), TapAt(0.5f, 0.8f, 1600));
	AddInput(Under, TEXT("Ripple"), Ripple);
	AddInput(Under, TEXT("Sun"), AddLook(Material, WaterParams, TEXT("Sun"), 1650));
	AddInput(Under, TEXT("Tint"), AddVector(Material, TEXT("Tint"), FLinearColor(0.24f, 0.38f, 0.27f), 1700));
	AddInput(Under, TEXT("DeepTint"), AddVector(Material, TEXT("DeepTint"), FLinearColor(0.035f, 0.11f, 0.11f), 1750));
	AddInput(Under, TEXT("Extinct"), AddVector(Material, TEXT("Extinct"), FLinearColor(0.30f, 0.13f, 0.17f), 1800));
	struct FStrength
	{
		const TCHAR* Name;
		float Value;
	};
	const FStrength Strengths[] = {{TEXT("DeepRate"), 0.35f}, {TEXT("DeepDark"), 0.1f}, {TEXT("FogGain"), 1.f}, {TEXT("WindowGain"), 1.1f},
		{TEXT("MirrorGain"), 0.9f}, {TEXT("PierceGain"), 0.82f}, {TEXT("CausticGain"), 0.45f}, {TEXT("CausticScale"), 4.f}, {TEXT("CausticFade"), 0.3f}, {TEXT("ShaftGain"), 0.2f},
		{TEXT("ShaftScale"), 2.5f}, {TEXT("ShaftFade"), 0.25f}, {TEXT("ShaftReach"), 12.f}, {TEXT("MoteGain"), 2.2f}, {TEXT("Meniscus"), 1.f},
		{TEXT("MeniscusCm"), 0.22f}};
	int32 Y = 1850;
	for (const FStrength& Each : Strengths)
	{
		AddInput(Under, Each.Name, AddScalar(Material, Each.Name, Each.Value, Y));
		Y += 50;
	}
	Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Under);
	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	return Material;
}
// A canal's walls and bed (kCanalBedCode). WaterZ (world cm) is set per canal on an instance by the map's builder.
// The sea's bed (kSeaBedCode) is built the same way with bSea, its WaterZ the sea's level.
UMaterial* BuildCanalBed(UMaterialParameterCollection* WaterParams, bool bSea = false)
{
	UMaterial* Material = NewMaterial(bSea ? FTScene::LookSeaBedMaterial : FTScene::LookCanalBedMaterial);
	Material->bTangentSpaceNormal = false;
	UMaterialExpressionCustom* Bed = AddCustom(Material, bSea ? TEXT("FTLookSeaBed") : TEXT("FTLookCanalBed"),
		bSea ? FTLookMaterialsBuild::kSeaBedCode : FTLookMaterialsBuild::kCanalBedCode, CMOT_Float3, 0);
	AddInput(Bed, TEXT("WorldPos"), AddExpression<UMaterialExpressionWorldPosition>(Material, -900, 0));
	AddInput(Bed, TEXT("VertexNormal"), AddExpression<UMaterialExpressionVertexNormalWS>(Material, -900, 100));
	AddInput(Bed, TEXT("WaterZ"), AddScalar(Material, TEXT("WaterZ"), bSea ? 0.f : 250.f, 200));
	AddInput(Bed, TEXT("Time"), AddExpression<UMaterialExpressionTime>(Material, -900, 300));
	AddInput(Bed, TEXT("Sun"), AddLook(Material, WaterParams, TEXT("Sun"), 400));
	AddInput(Bed, TEXT("BedCaustic"), AddLook(Material, WaterParams, TEXT("BedCaustic"), 500));
	AddOutput(Bed, TEXT("NormalOut"), CMOT_Float3);
	AddOutput(Bed, TEXT("RoughOut"), CMOT_Float1);
	Bed->RebuildOutputs();
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Bed);
	Inputs->Normal.Connect(1, Bed);
	Inputs->Roughness.Connect(2, Bed);
	Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
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

// ---- The glass system (-Glass; ftcore/GlassSurface.h) ----

// The true pane's code: the shared glass function over the pane's own coordinates (the pane is a cube thin along its
// x: its local y and z run across and up it).
// Mode 0: thin translucent (TintOut is the light it passes, the opacity only its dirt and seal); mode 1: a mirror layer
// whose opacity is the pane's mirror share (the surface a full mirror), for a renderer without thin translucency.
FString GlassPaneCode()
{
	FString Code = TEXT("float2 gUV = saturate(LocalPos.yz / 100.0 + 0.5);\n"
		"float2 gSize = max(PaneSize.xy, 0.05);\n"
		"float gSeed = Seed;\nfloat gAge = Age;\nfloat gWet = Wet;\nfloat gThick = Thick;\n"
		"float gCos = abs(dot(normalize(VNormal), normalize(CamV)));\n"
		"float2 gHandle = Handle.xy;\n");
	Code += FT::ToFString(ftcore::glassHlsl());
	Code += TEXT("float3 dirt = float3(0.60, 0.58, 0.53);\n"
		"TintOut = gTint * lerp(float3(1.0, 1.0, 1.0), float3(0.25, 0.45, 0.35), gEdge) * (1.0 - gSeal);\n"
		"RoughOut = lerp(gRough, 0.6, gSeal);\n"
		"NormalOut = normalize(float3(gWave.x, gWave.y, 1.0));\n"
		"OpacityOut = Mode > 0.5 ? saturate(gMirror + gVeil + gSeal) : saturate(gVeil + gSeal);\n"
		"MetalOut = Mode > 0.5 ? (1.0 - saturate(gVeil + gSeal)) : 0.0;\n"
		"SpecOut = lerp(0.96, 0.3, gSeal);\n"
		"return Mode > 0.5 ? lerp(float3(1.0, 1.0, 1.0), lerp(dirt, float3(0.02, 0.02, 0.02), gSeal), saturate(gVeil + gSeal)) : lerp(dirt, float3(0.02, 0.02, 0.02), gSeal);\n");
	return Code;
}

UMaterial* BuildGlassPane(UMaterialParameterCollection* Look, bool bMirror)
{
	UMaterial* Material = NewMaterial(bMirror ? FTScene::LookGlassMirrorMaterial : FTScene::LookGlassPaneMaterial);
	Material->BlendMode = BLEND_Translucent;
	Material->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
	Material->bAllowFrontLayerTranslucency = true;
	Material->bScreenSpaceReflections = true;
	Material->bTangentSpaceNormal = true;
	if (!bMirror)
	{
		Material->SetShadingModel(MSM_ThinTranslucent);
	}
	const FString Code = GlassPaneCode();
	UMaterialExpressionCustom* Pane = AddCustom(Material, TEXT("FTGlassPane"), *Code, CMOT_Float3, 0);
	AddInput(Pane, TEXT("LocalPos"), AddExpression<UMaterialExpressionLocalPosition>(Material, -900, -300));
	AddInput(Pane, TEXT("VNormal"), AddExpression<UMaterialExpressionVertexNormalWS>(Material, -900, -200));
	AddInput(Pane, TEXT("CamV"), AddExpression<UMaterialExpressionCameraVectorWS>(Material, -900, -100));
	AddInput(Pane, TEXT("PaneSize"), AddVector(Material, TEXT("PaneSize"), FLinearColor(1.5f, 2.5f, 0.f, 0.f), 0));
	AddInput(Pane, TEXT("Handle"), AddVector(Material, TEXT("Handle"), FLinearColor(-1.f, 0.f, 0.f, 0.f), 100));
	AddInput(Pane, TEXT("Seed"), AddScalar(Material, TEXT("Seed"), 0.37f, 200));
	AddInput(Pane, TEXT("Age"), AddScalar(Material, TEXT("Age"), 0.15f, 300));
	AddInput(Pane, TEXT("Thick"), AddScalar(Material, TEXT("ThicknessM"), 0.008f, 400));
	AddInput(Pane, TEXT("Wet"), AddLook(Material, Look, TEXT("Wetness"), 500));
	AddInput(Pane, TEXT("Mode"), AddScalar(Material, TEXT("MirrorLayer"), bMirror ? 1.f : 0.f, 600));
	AddOutput(Pane, TEXT("TintOut"), CMOT_Float3);
	AddOutput(Pane, TEXT("RoughOut"), CMOT_Float1);
	AddOutput(Pane, TEXT("NormalOut"), CMOT_Float3);
	AddOutput(Pane, TEXT("OpacityOut"), CMOT_Float1);
	AddOutput(Pane, TEXT("MetalOut"), CMOT_Float1);
	AddOutput(Pane, TEXT("SpecOut"), CMOT_Float1);
	Pane->RebuildOutputs();
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Pane);
	Inputs->Roughness.Connect(2, Pane);
	Inputs->Normal.Connect(3, Pane);
	Inputs->Opacity.Connect(4, Pane);
	Inputs->Metallic.Connect(5, Pane);
	Inputs->Specular.Connect(6, Pane);
	if (!bMirror)
	{
		UMaterialExpressionThinTranslucentMaterialOutput* Thin = AddExpression<UMaterialExpressionThinTranslucentMaterialOutput>(Material, 200, 300);
		Thin->TransmittanceColor.Connect(1, Pane);
	}
	Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
	Finish(Material);
	return Material;
}

// Glass over a picture: an opaque smooth surface (the engine mirrors the scene in it, with its own Fresnel), the room
// behind it (ftcore::glassPictureHlsl, met by the true view ray) added as emitted light times what the pane passes.
// The room's light is the daylight share (Daylight) and its lamps (Lights): no value here reads the hour.
UMaterial* BuildGlassOver(UMaterialParameterCollection* Look)
{
	UMaterial* Material = NewMaterial(FTScene::LookGlassOverMaterial);
	Material->bTangentSpaceNormal = false;
	FString Code = TEXT("float3 n = normalize(VNormal);\n"
		"float3 hx = normalize(cross(float3(0.0, 0.0, 1.0), n) + float3(1e-4, 0.0, 0.0));\n"
		"float3 V = -normalize(CamV);\n"
		"float2 iA = float2(dot(WorldPos, hx), WorldPos.z) / 100.0;\n"
		"float iFloor = FloorCm / 100.0;\n"
		"float3 iV = float3(dot(V, hx), V.z, dot(V, -n));\n"
		"float iShop = Shop;\nfloat iSeed = Seed;\nfloat iLit = Lights;\nfloat iDay = Daylight;\nfloat iDayLevel = DayLevel;\n"
		"float3 iLamp = Lamp.rgb;\n");
	Code += FT::ToFString(ftcore::glassPictureHlsl());
	Code += TEXT("float2 gUV = saturate(LocalPos.yz / 100.0 + 0.5);\n"
		"float2 gSize = max(PaneSize.xy, 0.05);\n"
		"float gSeed = Seed;\nfloat gAge = Age;\nfloat gWet = Wet;\nfloat gThick = Thick;\n"
		"float gCos = abs(dot(n, V));\n"
		"float2 gHandle = Handle.xy;\n");
	Code += FT::ToFString(ftcore::glassHlsl());
	Code += TEXT("EmissiveOut = iCol * (1.0 - gMirror) * gTint * (1.0 - gVeil) * (1.0 - gSeal) * lerp(float3(1.0, 1.0, 1.0), float3(0.25, 0.45, 0.35), gEdge);\n"
		"RoughOut = lerp(gRough, 0.6, gSeal);\n"
		"SpecOut = lerp(0.96, 0.3, gSeal);\n"
		"NormalOut = normalize(n + hx * gWave.x + float3(0.0, 0.0, 1.0) * gWave.y);\n"
		"return lerp(float3(0.60, 0.58, 0.53) * gVeil, float3(0.02, 0.02, 0.02), gSeal);\n");
	UMaterialExpressionCustom* Over = AddCustom(Material, TEXT("FTGlassOver"), *Code, CMOT_Float3, 0);
	AddInput(Over, TEXT("LocalPos"), AddExpression<UMaterialExpressionLocalPosition>(Material, -900, -400));
	AddInput(Over, TEXT("WorldPos"), AddExpression<UMaterialExpressionWorldPosition>(Material, -900, -300));
	AddInput(Over, TEXT("VNormal"), AddExpression<UMaterialExpressionVertexNormalWS>(Material, -900, -200));
	AddInput(Over, TEXT("CamV"), AddExpression<UMaterialExpressionCameraVectorWS>(Material, -900, -100));
	AddInput(Over, TEXT("PaneSize"), AddVector(Material, TEXT("PaneSize"), FLinearColor(1.5f, 2.5f, 0.f, 0.f), 0));
	AddInput(Over, TEXT("Handle"), AddVector(Material, TEXT("Handle"), FLinearColor(-1.f, 0.f, 0.f, 0.f), 100));
	AddInput(Over, TEXT("Lamp"), AddVector(Material, TEXT("Lamp"), FLinearColor(1.6f, 1.3f, 0.95f, 0.f), 150));
	AddInput(Over, TEXT("Seed"), AddScalar(Material, TEXT("Seed"), 0.37f, 200));
	AddInput(Over, TEXT("Age"), AddScalar(Material, TEXT("Age"), 0.15f, 300));
	AddInput(Over, TEXT("Thick"), AddScalar(Material, TEXT("ThicknessM"), 0.008f, 400));
	AddInput(Over, TEXT("Wet"), AddLook(Material, Look, TEXT("Wetness"), 500));
	AddInput(Over, TEXT("FloorCm"), AddScalar(Material, TEXT("FloorCm"), 0.f, 600));
	AddInput(Over, TEXT("Shop"), AddScalar(Material, TEXT("Shop"), 1.f, 700));
	AddInput(Over, TEXT("Lights"), AddScalar(Material, TEXT("Lights"), 0.f, 800));
	AddInput(Over, TEXT("Daylight"), AddScalar(Material, TEXT("Daylight"), 1.f, 900));
	AddInput(Over, TEXT("DayLevel"), AddScalar(Material, TEXT("DayLevel"), 0.35f, 1000));
	AddOutput(Over, TEXT("EmissiveOut"), CMOT_Float3);
	AddOutput(Over, TEXT("RoughOut"), CMOT_Float1);
	AddOutput(Over, TEXT("SpecOut"), CMOT_Float1);
	AddOutput(Over, TEXT("NormalOut"), CMOT_Float3);
	Over->RebuildOutputs();
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Over);
	Inputs->EmissiveColor.Connect(1, Over);
	Inputs->Roughness.Connect(2, Over);
	Inputs->Specular.Connect(3, Over);
	Inputs->Normal.Connect(4, Over);
	Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
	Finish(Material);
	return Material;
}

// A lit sign: a dark face that gives SignColor x SignStrength of its own light.
UMaterial* BuildSign()
{
	UMaterial* Material = NewMaterial(FTScene::LookSignMaterial);
	UMaterialExpressionCustom* Glow = AddCustom(Material, TEXT("FTSignGlow"), TEXT("return max(SignColor.rgb, 0.0) * max(Strength, 0.0);\n"), CMOT_Float3, 0);
	UMaterialExpressionVectorParameter* Color = AddVector(Material, TEXT("SignColor"), FLinearColor(1.f, 0.25f, 0.2f), 0);
	AddInput(Glow, TEXT("SignColor"), Color);
	AddInput(Glow, TEXT("Strength"), AddScalar(Material, TEXT("SignStrength"), 6.f, 100));
	Glow->RebuildOutputs();
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Color);
	Inputs->Roughness.Connect(0, AddScalar(Material, TEXT("Roughness"), 0.4f, 200));
	Inputs->EmissiveColor.Connect(0, Glow);
	Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
	Finish(Material);
	return Material;
}

// Nothing: every pixel cut out. A kit piece's glass slot wears it where a true pane stands in the slot's place.
UMaterial* BuildGlassHole()
{
	UMaterial* Material = NewMaterial(FTScene::LookGlassHoleMaterial);
	Material->BlendMode = BLEND_Masked;
	Material->OpacityMaskClipValue = 0.5f;
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, AddVector(Material, TEXT("Color"), FLinearColor(0.f, 0.f, 0.f), 0));
	Inputs->OpacityMask.Connect(0, AddScalar(Material, TEXT("Open"), 0.f, 100));
	Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
	Material->SetUsageByFlag(MATUSAGE_Nanite, true);
	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	return Material;
}

// A real bay's surfaces behind a true pane: Color under the scene's light, plus the bay's own: a little by day (the
// shop's lamps are on, the daylight comes in through the pane by itself) and, with the lights on (the window
// collection's Night), Color x ProofShopNight; LampPanel 1 is a ceiling light panel. A collection without those values
// (built before the glass system) gives plain parameters instead.
UMaterial* BuildGlassBay()
{
	UMaterial* Material = NewMaterial(FTScene::LookGlassBayMaterial);
	UMaterialParameterCollection* Windows = LoadObject<UMaterialParameterCollection>(nullptr,
		*(FString(FTScene::WindowCollection) + TEXT(".") + FPackageName::GetShortName(FTScene::WindowCollection)), nullptr, LOAD_NoWarn | LOAD_Quiet);
	const auto Light = [&](const TCHAR* Name, float Fallback, int32 Y) -> UMaterialExpression*
	{
		if (Windows && Windows->GetParameterId(Name).IsValid())
		{
			return AddLook(Material, Windows, Name, Y);
		}
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildLookMaterials: the window collection has no %s; %s reads a plain value."), Name, FTScene::LookGlassBayMaterial);
		return AddScalar(Material, Name, Fallback, Y);
	};
	UMaterialExpressionVectorParameter* Color = AddVector(Material, TEXT("Color"), FLinearColor(0.7f, 0.68f, 0.63f), 0);
	UMaterialExpressionCustom* Glow = AddCustom(Material, TEXT("FTGlassBay"),
		TEXT("float3 lamp = float3(1.0, 0.9, 0.76) * ProofNight * Night;\n"
			 "float3 own = Color.rgb * (DayLamp * Daylight + lamp * 1.3);\n"
			 "return own + LampPanel * (DayLamp * Daylight * 6.0 + lamp * 4.0);\n"),
		CMOT_Float3, 100);
	AddInput(Glow, TEXT("Color"), Color);
	AddInput(Glow, TEXT("Night"), Light(TEXT("Night"), 0.f, 200));
	AddInput(Glow, TEXT("Daylight"), Light(TEXT("Daylight"), 1.f, 300));
	AddInput(Glow, TEXT("ProofNight"), Light(TEXT("ProofShopNight"), 0.3f, 400));
	AddInput(Glow, TEXT("DayLamp"), AddScalar(Material, TEXT("DayLamp"), 0.12f, 500));
	AddInput(Glow, TEXT("LampPanel"), AddScalar(Material, TEXT("LampPanel"), 0.f, 600));
	Glow->RebuildOutputs();
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	Inputs->BaseColor.Connect(0, Color);
	Inputs->Roughness.Connect(0, AddScalar(Material, TEXT("Roughness"), 0.6f, 700));
	Inputs->EmissiveColor.Connect(0, Glow);
	Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
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
	struct FBuilt
	{
		UMaterial* Material;
		const TCHAR* Package;
	};
	if (FParse::Param(*Params, TEXT("Glass")))
	{
		// The glass system's materials alone, on the look collection as it stands: nothing else is rebuilt or resaved.
		UMaterialParameterCollection* Look = LoadObject<UMaterialParameterCollection>(nullptr, FTScene::LookCollection);
		if (!Look)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLookMaterials: %s is missing; run without -Glass first."), FTScene::LookCollection);
			return 1;
		}
		const TCHAR* const Parts[] = {FTScene::LookGlassPaneMaterial, FTScene::LookGlassMirrorMaterial, FTScene::LookGlassOverMaterial,
			FTScene::LookSignMaterial, FTScene::LookGlassHoleMaterial, FTScene::LookGlassBayMaterial};
		for (const TCHAR* Package : Parts)
		{
			const FString File = PackageFile(Package);
			if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLookMaterials: couldn't replace %s."), *File);
				return 1;
			}
		}
		const FBuilt Built[] = {{BuildGlassPane(Look, false), Parts[0]}, {BuildGlassPane(Look, true), Parts[1]}, {BuildGlassOver(Look), Parts[2]},
			{BuildSign(), Parts[3]}, {BuildGlassHole(), Parts[4]}, {BuildGlassBay(), Parts[5]}};
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
	}
	const bool bCanal = FParse::Param(*Params, TEXT("Canal"));
	const bool bUnderwater = FParse::Param(*Params, TEXT("Underwater"));
	const bool bWater = FParse::Param(*Params, TEXT("Water"));
	if (bCanal || bUnderwater || bWater)
	{
		// A part alone, on the look collection as it stands (nothing else is rebuilt or resaved):
		// -Water: the water collection and the three materials that read it (the sea, the canal's water, the underwater view).
		// -Canal: the canal's water and its walls and bed. -Underwater: the underwater view.
		UMaterialParameterCollection* Look = LoadObject<UMaterialParameterCollection>(nullptr, FTScene::LookCollection);
		if (!Look)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLookMaterials: %s is missing; run without -Water, -Canal or -Underwater first."), FTScene::LookCollection);
			return 1;
		}
		TArray<const TCHAR*, TInlineAllocator<8>> Parts;
		if (bWater)
		{
			Parts.Add(FTScene::LookWaterCollection);
			Parts.Add(FTScene::LookWaterMaterial);
		}
		if (bWater || bCanal)
		{
			Parts.Add(FTScene::LookCanalWaterMaterial);
		}
		if (bCanal)
		{
			Parts.Add(FTScene::LookCanalBedMaterial);
			Parts.Add(FTScene::LookSeaBedMaterial);
		}
		if (bWater || bUnderwater)
		{
			Parts.Add(FTScene::LookUnderwaterMaterial);
		}
		for (const TCHAR* Package : Parts)
		{
			const FString File = PackageFile(Package);
			if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLookMaterials: couldn't replace %s."), *File);
				return 1;
			}
		}
		UMaterialParameterCollection* WaterParams = bWater ? BuildWaterCollection() : LoadObject<UMaterialParameterCollection>(nullptr, FTScene::LookWaterCollection);
		if (!WaterParams || (bWater && !Save(WaterParams, FTScene::LookWaterCollection)))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildLookMaterials: %s is missing; run with -Water first."), FTScene::LookWaterCollection);
			return 1;
		}
		TArray<FBuilt, TInlineAllocator<8>> Built;
		for (const TCHAR* Package : Parts)
		{
			if (Package == FTScene::LookWaterMaterial)
			{
				Built.Add(FBuilt{BuildWater(Look, WaterParams), Package});
			}
			else if (Package == FTScene::LookCanalWaterMaterial)
			{
				Built.Add(FBuilt{BuildCanalWater(Look, WaterParams), Package});
			}
			else if (Package == FTScene::LookCanalBedMaterial)
			{
				Built.Add(FBuilt{BuildCanalBed(WaterParams), Package});
			}
			else if (Package == FTScene::LookSeaBedMaterial)
			{
				Built.Add(FBuilt{BuildCanalBed(WaterParams, true), Package});
			}
			else if (Package == FTScene::LookUnderwaterMaterial)
			{
				Built.Add(FBuilt{BuildUnderwater(WaterParams), Package});
			}
		}
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
	}
	const TCHAR* const Packages[] = {FTScene::LookUnderwaterMaterial, FTScene::LookWaterCollection, FTScene::LookCollection, FTScene::LookWaterMaterial, FTScene::LookGlassMaterial,
		FTScene::LookChromeMaterial, FTScene::LookWetStoneMaterial, FTScene::LookGlowMaterial, FTScene::LookCanalWaterMaterial,
		FTScene::LookCanalBedMaterial, FTScene::LookSeaBedMaterial};
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
	UMaterialParameterCollection* WaterParams = BuildWaterCollection();
	if (!Save(WaterParams, FTScene::LookWaterCollection))
	{
		return 1;
	}
	const FBuilt Built[] = {
		{BuildWater(Look, WaterParams), FTScene::LookWaterMaterial},
		{BuildGlass(), FTScene::LookGlassMaterial},
		{BuildChrome(), FTScene::LookChromeMaterial},
		{BuildWetStone(Look, Stone), FTScene::LookWetStoneMaterial},
		{BuildGlow(Look), FTScene::LookGlowMaterial},
		{BuildCanalWater(Look, WaterParams), FTScene::LookCanalWaterMaterial},
		{BuildCanalBed(WaterParams), FTScene::LookCanalBedMaterial},
		{BuildCanalBed(WaterParams, true), FTScene::LookSeaBedMaterial},
		{BuildUnderwater(WaterParams), FTScene::LookUnderwaterMaterial},
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
