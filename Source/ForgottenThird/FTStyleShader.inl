// HLSL body of the Custom node in /Game/Rendering/PP_Style (FTBuildStyleMaterialsCommandlet).
// A port of newengine's tonemap STYLE stage (engine/src/render/PostProcess.cpp, applyStyle) plus a
// screen-space version of the lit shader's tonal-art-map hatching. Display-space sRGB in and out.
// The inputs are named after the material parameters (FTStyleParameters.cpp) plus SceneColor
// (PostProcessInput0), SceneDepthIn (SceneDepth, cm), WorldPos and TimeSec.
//
// Split into several literals: MSVC caps one string literal piece at 16 KB.

static const TCHAR* const FTStyleShaderCode =
TEXT(R"HLSL(
struct FTStyleFns
{
	// Integer hash, one value in [0, 1) per pixel and seed (same as newengine).
	float HashPx(uint2 p, uint frame)
	{
		uint h = p.x * 1597334677u ^ p.y * 3812015801u ^ frame * 2654435761u;
		h = (h ^ (h >> 16)) * 0x7feb352du;
		h = (h ^ (h >> 15)) * 0x846ca68bu;
		h ^= h >> 16;
		return float(h >> 8) * (1.0 / 16777216.0);
	}
	float HashLine(uint x)
	{
		x ^= x >> 16;
		x *= 0x7feb352du;
		x ^= x >> 15;
		x *= 0x846ca68bu;
		x ^= x >> 16;
		return float(x & 0xFFFFFFu) / 16777216.0;
	}
	// Smooth value noise on the integer lattice of p.
	float VNoise(float2 p, uint seed)
	{
		float2 i = floor(p);
		float2 f = frac(p);
		f = f * f * (3.0 - 2.0 * f);
		uint2 q = uint2(int2(i) + int2(65536, 65536));
		float a = HashPx(q, seed);
		float b = HashPx(q + uint2(1, 0), seed);
		float c = HashPx(q + uint2(0, 1), seed);
		float d = HashPx(q + uint2(1, 1), seed);
		return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
	}
	// Linear scene depth in cm at a viewport UV, clamped to [1, skyDepth] so the sky reads as one far plane.
	float DepthCm(float2 vuv, float skyDepth)
	{
		float2 uv = ClampSceneTextureUV(ViewportUVToSceneTextureUV(vuv, PPI_SceneDepth), PPI_SceneDepth);
		return clamp(SceneTextureLookup(uv, PPI_SceneDepth, false).r, 1.0, skyDepth);
	}
	// Reciprocal view depth: affine in screen space across any plane, so its second difference is zero on
	// flat surfaces and spikes at silhouettes and creases.
	float InvDist(float2 vuv, float skyDepth)
	{
		return 1.0 / DepthCm(vuv, skyDepth);
	}
	// Relative depth curvature over a 3x3 ring of radius o (viewport UV units).
	float EdgeMeasure(float2 vuv, float2 o, float c, float skyDepth)
	{
		float l = InvDist(vuv - float2(o.x, 0.0), skyDepth);
		float r = InvDist(vuv + float2(o.x, 0.0), skyDepth);
		float b = InvDist(vuv - float2(0.0, o.y), skyDepth);
		float t = InvDist(vuv + float2(0.0, o.y), skyDepth);
		float bl = InvDist(vuv - o, skyDepth);
		float tr = InvDist(vuv + o, skyDepth);
		float br = InvDist(vuv + float2(o.x, -o.y), skyDepth);
		float tl = InvDist(vuv + float2(-o.x, o.y), skyDepth);
		float k = max(abs(l + r - 2.0 * c), abs(b + t - 2.0 * c));
		k = max(k, 0.7 * max(abs(bl + tr - 2.0 * c), abs(br + tl - 2.0 * c)));
		return k / max(c, 1e-9);
	}
)HLSL")
TEXT(R"HLSL(
	// One stroke family of newengine's tonal art map (hatchTonalArtMap), evaluated analytically at tile
	// coordinate t (0..256). Lines are ax * x + ay * y = const (mod spacing), with a per-line offset, a slow
	// wobble and short breaks so they read as drawn.
	float Stroke(float2 t, float ax, float ay, float spacing, float offset, float halfWidth, uint seed)
	{
		const float n = 256.0;
		const float tau = 6.28318530718;
		float norm = sqrt(ax * ax + ay * ay);
		float u = ax * t.x + ay * t.y + n - offset;
		float v = ay * t.x - ax * t.y;
		float lines = floor(n / spacing + 0.5);
		float idx = floor(u / spacing + 0.5);
		float lineIdx = idx - lines * floor(idx / lines);
		uint ln = uint(lineIdx + 0.5);
		float h1 = HashLine(ln * 7919u + seed);
		float h2 = HashLine(ln * 104729u + seed * 31u);
		float wobble = 0.6 * sin(tau * 2.0 * v / n + h2 * tau);
		float centre = idx * spacing + (h1 - 0.5) * 0.25 * spacing + wobble;
		float dist = abs(u - centre) / norm;
		float gap = sin(tau * 3.0 * v / n + h1 * 11.0);
		float present = saturate((gap + 0.92) * 8.0);
		return saturate(halfWidth + 0.5 - dist) * present;
	}
};

FTStyleFns F;
const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

float2 pix = GetPixelPosition(Parameters);
// Post-process shaders have the output viewport; the other permutations UE compiles for a post-process
// material (a base-pass preview) only have the view.
#if POST_PROCESS_MATERIAL
float2 invView = PostProcessOutput_ViewportSizeInverse;
#else
float2 invView = View.ViewSizeAndInvSize.zw;
#endif
float2 viewSize = 1.0 / max(invView, float2(1e-6, 1e-6));
float2 vuv = pix * invView;
// newengine's patterns use gl_FragCoord (y up); flip so stroke directions match.
float2 pixGL = float2(pix.x, viewSize.y - pix.y);
float pxScale = max(viewSize.y, 1.0) / 1080.0;

float3 c = saturate(SceneColor.rgb);
float3 src = c; // the graded colour: what a colour island keeps
float rawDepth = SceneDepthIn.r;
bool sky = rawDepth >= SkyDepth;

// Split tint and saturation.
float l = dot(c, kLuma);
c = saturate(lerp(float3(l, l, l), c, Saturation));
l = dot(c, kLuma);
c *= lerp(ShadowTint.rgb, float3(1.0, 1.0, 1.0), smoothstep(0.0, 0.55, l));
c *= lerp(float3(1.0, 1.0, 1.0), HighlightTint.rgb, smoothstep(0.35, 1.0, l));
)HLSL")
TEXT(R"HLSL(
// Hatching (screen-space stand-in for the lit shader's tonal art map): darker tone -> denser strokes.
if (Hatch > 0.0 && !sky)
{
	float tone = saturate(dot(c, kLuma));
	float dark = pow(max(1.0 - tone, 0.0), max(HatchCurve, 1e-3));
	float d = min(saturate(dark * max(HatchDensity, 0.0)) * 5.0, max(HatchMaxLevel, 0.0));
	float4 w = max(0.0, 1.0 - abs(d - float4(1.0, 2.0, 3.0, 4.0)));
	float solid = max(0.0, 1.0 - abs(d - 5.0));
	float tile = max(HatchScale * pxScale, 8.0);
	float2 t = frac(pixGL / tile) * 256.0;
	const float s = 16.0;
	float f0 = F.Stroke(t, 1.0, 1.0, s, 0.0, 1.1, 11u);
	float f1 = F.Stroke(t, 1.0, 1.0, s, s * 0.5, 0.95, 12u);
	float f2 = F.Stroke(t, 1.0, -1.0, s, s * 0.25, 1.0, 13u);
	float f3 = F.Stroke(t, 1.0, -1.0, s, s * 0.75, 1.0, 14u);
	float f4 = F.Stroke(t, 0.0, 1.0, s * 0.5, s * 0.125, 0.9, 15u);
	float4 lv;
	lv.x = f0;
	lv.y = max(lv.x, f1);
	lv.z = max(lv.y, f2);
	lv.w = max(lv.z, max(f3, f4));
	float cov = saturate(dot(lv, w) + solid);
	// Shadow-only: post has no light buffer, so the lowest cel band is approximated by display tone.
	float bandEdge = 0.8 / max(CelSteps, 1.0);
	float shadowMask = 1.0 - smoothstep(bandEdge - 0.05, bandEdge + 0.05, tone);
	cov *= lerp(1.0, shadowMask, saturate(HatchShadowOnly));
	// Base: a paper-bright version of the colour (keeps the hue), or the colour itself with HatchOverlay.
	float paperL = dot(PaperColor.rgb, kLuma);
	float3 paperBase = saturate(c * (paperL / max(tone, 1e-3)));
	float3 hatchBase = lerp(paperBase, c, saturate(HatchOverlay));
	c = lerp(c, lerp(hatchBase, HatchInk.rgb, cov), saturate(Hatch));
}

// Monochrome page: luminance remapped onto ink -> paper, optional 45-degree screen-tone.
l = dot(c, kLuma);
if (Monochrome > 0.0)
{
	float tone = pow(saturate(l), max(MonoGamma, 1e-3));
	float3 page = lerp(InkColor.rgb, PaperColor.rgb, tone);
	if (Halftone > 0.0)
	{
		float p = max(HalftonePitch * pxScale, 2.0);
		float2 q = float2(0.70710678 * (pixGL.x - pixGL.y), 0.70710678 * (pixGL.x + pixGL.y)) / p;
		float dist = length(frac(q) - 0.5) * p;
		float r = sqrt(saturate(1.0 - tone)) * p * 0.74;
		float dotCov = 1.0 - smoothstep(r - 0.65, r + 0.65, dist);
		page = lerp(page, lerp(PaperColor.rgb, InkColor.rgb, dotCov), Halftone);
	}
	c = lerp(c, page, Monochrome);
}
if (sky)
{
	c = lerp(c, PaperColor.rgb, PaperSky);
}

// Ink void: distance falls away into ink, the sky with it. Depths in cm.
if (InkVoid > 0.0)
{
	float vd = sky ? 1.0 : smoothstep(InkVoidNear, max(InkVoidFar, InkVoidNear + 0.1), rawDepth);
	c = lerp(c, InkColor.rgb, vd * InkVoid);
}
)HLSL")
TEXT(R"HLSL(
// Colour island: a disc on the ground plane (XY, cm) that keeps the graded colour, with an inked edge.
float2 dxy = WorldPos.xy - IslandCenter.xy;
float islandDist = length(dxy);
float fw = max(fwidth(islandDist), 1e-3); // world units per pixel
if (Island > 0.0)
{
	float2 dir = dxy / max(islandDist, 1e-2);
	float rr = IslandRadius * (1.0 + 0.12 * (F.VNoise(dir * 2.2 + 7.0, 3u) - 0.5));
	float inside = sky ? 0.0 : 1.0 - smoothstep(rr - fw, rr + fw, islandDist);
	c = lerp(c, src, inside * Island);
	float halfW = 0.5 * IslandEdge * pxScale;
	float ring = sky ? 0.0 : 1.0 - smoothstep(halfW - 0.6, halfW + 0.6, abs(islandDist - rr) / fw);
	c = lerp(c, IslandInk.rgb, ring * Island);
}

// Ink outline from depth curvature.
if (Outline > 0.0)
{
	float w = OutlineWidth * pxScale;
	float2 suv = vuv;
	if (OutlineJitter > 0.0)
	{
		// Hand tremble: a low-frequency offset, re-drawn 8 times a second.
		float jitterStep = floor(TimeSec * 8.0);
		float2 np = pixGL / (34.0 * pxScale) + float2(jitterStep * 7.31, jitterStep * 3.17);
		float2 j = float2(F.VNoise(np, 21u), F.VNoise(np, 22u)) - 0.5;
		suv += j * 2.0 * OutlineJitter * pxScale * invView * float2(1.0, -1.0);
		w *= 0.8 + 0.4 * F.VNoise(np * 0.7, 23u);
	}
	float cInv = F.InvDist(suv, SkyDepth);
	if (OutlineTaper > 0.0)
	{
		// Brush: heavier near the camera, thinner far (newengine's law is in 1/metres), slow pressure wobble.
		float nearW = clamp(sqrt(7.0 * cInv * 100.0), 0.45, 1.6);
		float press = 0.65 + 0.7 * F.VNoise(pixGL / (70.0 * pxScale), 31u);
		w *= lerp(1.0, nearW * press, OutlineTaper);
	}
	float2 o = max(w, 1.0) * invView;
	float e = F.EdgeMeasure(suv, o, cInv, SkyDepth);
	float ink = smoothstep(OutlineThreshold, OutlineThreshold * 2.5, e);
	c = lerp(c, OutlineColor.rgb, ink * Outline);
}

// Paper tooth: static, screen-locked multiply at two scales (fine fibre + soft blotch).
if (PaperGrain > 0.0)
{
	float tooth = 0.6 * F.HashPx(uint2(pixGL), 977u) + 0.4 * F.VNoise(pixGL / (3.0 * pxScale), 41u);
	c *= 1.0 - PaperGrain * tooth;
}
return saturate(c);
)HLSL");
