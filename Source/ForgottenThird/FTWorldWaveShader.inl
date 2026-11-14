// HLSL bodies of the world wave's Custom nodes (FTBuildWaveMaterialsCommandlet): the ink post-process material
// (/Game/Look/Wave/PP_WorldWave_Ink) and the cut in MF_WorldWaveCut. Both start with the same helper struct, so the ragged
// edge a surface is clipped on is the edge the post-process inks to. Distances in cm, display-space sRGB colours.
//
// The ragged edge: the front's outer edge is Radius (the host writes the tongues' reach, ahead of the cues' radius); the
// band runs BandWidth inward of it, and the edge the second world reaches is e = Radius - BandWidth * Tongue(direction
// from the origin, time) plus a fixed world-space jitter of a few decimetres (FTWaveFns::Edge), so narrow tongues of the
// new world run out to the outer edge, the body of the front sits in the band's inner half and the tear is ragged along
// a wall. The second world is where
// d < e for a Forward wave and d >= e for a Reverse one.
//
// Split into several literals: MSVC caps one string literal piece at 16 KB. The commandlet puts FTWorldWaveFnsCode in
// front of each body (a raw string inside a macro is not portable, so the join is at run time).

static const TCHAR* const FTWorldWaveFnsCode =
TEXT(R"HLSL(
struct FTWaveFns
{
	float Hash3(int3 p, uint seed)
	{
		uint3 q = asuint(p);
		uint h = q.x * 1597334677u ^ q.y * 3812015801u ^ q.z * 2798796415u ^ seed * 2654435761u;
		h = (h ^ (h >> 16)) * 0x7feb352du;
		h = (h ^ (h >> 15)) * 0x846ca68bu;
		h ^= h >> 16;
		return float(h >> 8) * (1.0 / 16777216.0);
	}
	// Smooth value noise on the integer lattice of p, 0..1.
	float VNoise3(float3 p, uint seed)
	{
		float3 i = floor(p);
		float3 f = frac(p);
		f = f * f * (3.0 - 2.0 * f);
		int3 b = int3(i);
		float n000 = Hash3(b, seed);
		float n100 = Hash3(b + int3(1, 0, 0), seed);
		float n010 = Hash3(b + int3(0, 1, 0), seed);
		float n110 = Hash3(b + int3(1, 1, 0), seed);
		float n001 = Hash3(b + int3(0, 0, 1), seed);
		float n101 = Hash3(b + int3(1, 0, 1), seed);
		float n011 = Hash3(b + int3(0, 1, 1), seed);
		float n111 = Hash3(b + int3(1, 1, 1), seed);
		float x0 = lerp(lerp(n000, n100, f.x), lerp(n010, n110, f.x), f.y);
		float x1 = lerp(lerp(n001, n101, f.x), lerp(n011, n111, f.x), f.y);
		return lerp(x0, x1, f.z);
	}
	float VNoise2(float2 p, uint seed)
	{
		return VNoise3(float3(p, 0.5), seed);
	}
	float HashPx(float2 pix, uint seed)
	{
		return Hash3(int3(int2(floor(pix)), 0), seed);
	}
	// How far into the band the new world's edge stands in a direction: 0 at the outer edge (the tongues' reach), 1 at
	// the inner. The body of the front sits in the inner half (two octaves on the direction, drifting with the wave's
	// time); narrow spikes (about half a metre across at a 20 m front) run out from it to the outer edge.
	float Tongue(float3 dir, float t)
	{
		float a = VNoise3(dir * 7.0 + float3(0.0, 0.0, 0.30 * t), 7u);
		float b = VNoise3(dir * 19.0 + float3(3.1, 0.0, 0.55 * t), 8u);
		float body = saturate((0.55 * a + 0.3 * b - 0.15) / 0.6);
		float spike = smoothstep(0.62, 0.86, VNoise3(dir * 41.0 + float3(0.0, 0.0, 0.2 * t), 15u));
		return lerp(0.45, 1.0, body) * (1.0 - spike);
	}
	// The new world's edge at a world point, cm from the origin: the band's tongues, then a fixed jitter in world space
	// (80 and 25 cm cells, +-30 and +-10 cm) so the tear is ragged along a wall and not a clean line, kept inside the band.
	float Edge(float r, float w, float3 dir, float t, float3 wp)
	{
		float j = (VNoise3(wp / 80.0 + float3(0.0, 0.0, 0.4 * t), 13u) - 0.5) * 60.0 + (VNoise3(wp / 25.0, 14u) - 0.5) * 20.0;
		return clamp(r - w * Tongue(dir, t) + j, r - w, r);
	}
	// Thin streaks across the front, 0..1: the eaten world's smear (slivers that trail inward behind the edge).
	float Streak(float3 dir)
	{
		return VNoise3(dir * 160.0, 9u);
	}
};
)HLSL");

// The cut, in MF_WorldWaveCut (after FTWorldWaveFnsCode). Inputs: WorldPos (absolute world position), CamPos (camera
// position), WOrigin, WRadius, WBand, WDirection, WTime (the collection) and Tag, LocalR (custom primitive data,
// FTWorldWave::kTagSlot and kLocalSlot). Returns the clip (1 keep, 0 cut); InkOut is the arriving thing's leading strip
// (base colour to ink), CoreOut its paper-white core (emissive). No derivatives: ray-tracing hit shaders compile this too.
static const TCHAR* const FTWorldWaveCutBody =
TEXT(R"HLSL(
InkOut = 0.0;
CoreOut = float3(0.0, 0.0, 0.0);
float tag = Tag;
if (tag < 0.5)
{
	return 1.0;
}
FTWaveFns W;
float3 rel = WorldPos - WOrigin.xyz;
float dist = length(rel);
float3 dir = rel / max(dist, 0.01);
float r = LocalR > 0.0 ? LocalR : WRadius;
float w = max(WBand, 1.0);
bool forward = WDirection < 0.5;
bool oldOnly = tag < 1.5;
float edge = W.Edge(r, w, dir, WTime, WorldPos);
// The thing the wave takes away (OldOnly going forward, NewOnly in reverse) is eaten from the side the front reaches
// first, the eaten part running out in thin radial streaks across the rest of the band (the smear); the thing it
// brings is drawn in from the same side.
bool leaving = (oldOnly == forward);
if (leaving)
{
	float reach = smoothstep(0.62, 0.9, W.Streak(dir)) * max(r - edge, 0.0);
	return dist >= edge + reach ? 1.0 : 0.0;
}
if (dist >= edge)
{
	return 0.0;
}
// The leading strip: about 2.5 px wide at the camera's distance (no derivatives), at least 5 cm; ink black with a
// paper-white core.
float camDist = length(WorldPos - CamPos);
float stripW = max(5.0, 0.0025 * camDist);
float s = (edge - dist) / stripW;
InkOut = 1.0 - saturate(s);
float core = 1.0 - saturate(abs(s - 0.45) / 0.2);
CoreOut = float3(0.93, 0.91, 0.86) * (1.6 * core);
return 1.0;
)HLSL");

// The ink post-process material. Inputs: SceneColor (PostProcessInput0), SceneDepthIn (SceneDepth, cm), SceneNormalIn
// (GBuffer world normal), WorldPos, CamPos (camera position), SunDir (toward the sun), the collection's WOrigin, WOriginScreen, WFoot, WTip, WRadius, WBand, WPrelude,
// WDirection, WInside, WTime, and the material's own SkyDepth, SceneKeep, DisplacePx, SmearMinPx, SmearMaxPx, LinePx,
// HatchPx, Tooth, StreakPx, PaperColor, InkColor. After FTWorldWaveFnsCode.
static const TCHAR* const FTWorldWaveInkBody =
TEXT(R"HLSL(
struct FTInkFns
{
	float3 Color(float2 vuv)
	{
		float2 uv = ClampSceneTextureUV(ViewportUVToSceneTextureUV(vuv, PPI_PostProcessInput0), PPI_PostProcessInput0);
		return SceneTextureLookup(uv, PPI_PostProcessInput0, false).rgb;
	}
	float DepthCm(float2 vuv, float skyDepth)
	{
		float2 uv = ClampSceneTextureUV(ViewportUVToSceneTextureUV(vuv, PPI_SceneDepth), PPI_SceneDepth);
		return clamp(SceneTextureLookup(uv, PPI_SceneDepth, false).r, 1.0, skyDepth);
	}
	float3 Normal(float2 vuv)
	{
		float2 uv = ClampSceneTextureUV(ViewportUVToSceneTextureUV(vuv, PPI_WorldNormal), PPI_WorldNormal);
		float3 n = SceneTextureLookup(uv, PPI_WorldNormal, false).rgb;
		return n / max(length(n), 1e-3);
	}
	// Ink lines whose weight follows the form: the silhouettes (relative curvature of the reciprocal depth) over a ring
	// twice as wide, so an outline is drawn heavier than an inner crease (the normal's turn over the ring o, viewport
	// UV), and the creases a little lighter.
	float Lines(float2 vuv, float2 o, float skyDepth, float3 n)
	{
		float2 o2 = 2.0 * o;
		float c = 1.0 / DepthCm(vuv, skyDepth);
		float l = 1.0 / DepthCm(vuv - float2(o2.x, 0.0), skyDepth);
		float r = 1.0 / DepthCm(vuv + float2(o2.x, 0.0), skyDepth);
		float b = 1.0 / DepthCm(vuv - float2(0.0, o2.y), skyDepth);
		float t = 1.0 / DepthCm(vuv + float2(0.0, o2.y), skyDepth);
		float k = max(abs(l + r - 2.0 * c), abs(b + t - 2.0 * c)) / max(c, 1e-9);
		float depthInk = smoothstep(0.02, 0.05, k);
		float turn = max(max(1.0 - dot(n, Normal(vuv - float2(o.x, 0.0))), 1.0 - dot(n, Normal(vuv + float2(o.x, 0.0)))),
			max(1.0 - dot(n, Normal(vuv - float2(0.0, o.y))), 1.0 - dot(n, Normal(vuv + float2(0.0, o.y)))));
		float normalInk = 0.85 * smoothstep(0.15, 0.35, turn);
		return max(depthInk, normalInk);
	}
	// The drawn detail the depth and normals do not carry (window bars, a clock's face and hands, signs): the scene's
	// own luminance edges over the ring o, drawn as thin lines.
	float Detail(float2 vuv, float2 o, float3 kLuma)
	{
		float l = dot(saturate(Color(vuv - float2(o.x, 0.0))), kLuma);
		float r = dot(saturate(Color(vuv + float2(o.x, 0.0))), kLuma);
		float b = dot(saturate(Color(vuv - float2(0.0, o.y))), kLuma);
		float t = dot(saturate(Color(vuv + float2(0.0, o.y))), kLuma);
		float grad = max(abs(l - r), abs(b - t));
		return 0.75 * smoothstep(0.07, 0.2, grad);
	}
	// One hatch family on the plane coordinate q (cm) at spacing s (cm): lines along angle a, drawn about halfPx
	// pixels each side, a slow wobble and a per-line offset so they read as drawn. fq: the plane coordinate's change
	// per pixel (cm), taken once outside the branches, so no derivative is taken here.
	float Family(float2 q, float2 fq, float s, float2 a, float halfPx, uint seed, FTWaveFns W)
	{
		float u = dot(q, a) / s;
		float v = dot(q, float2(-a.y, a.x)) / s;
		float idx = floor(u + 0.5);
		float h = W.Hash3(int3(int(idx), 0, 0), seed);
		u += 0.18 * (h - 0.5) + 0.07 * sin(6.2831853 * (0.05 * v + h));
		float du = max(dot(abs(a), fq) / s, 1e-4);
		float distPx = abs(u - floor(u + 0.5)) / du;
		float mark = saturate(halfPx + 0.5 - distPx);
		// Lines closer than about three pixels merge to grey: let the coarser octave carry them.
		return mark * saturate((0.42 - du) / 0.14);
	}
	float HatchAt(float2 q, float2 fq, float s, float tone, float halfPx, FTWaveFns W)
	{
		float a = Family(q, fq, s, float2(0.70710678, 0.70710678), halfPx, 21u, W) * smoothstep(0.78, 0.62, tone);
		float b = Family(q, fq, s, float2(0.70710678, -0.70710678), halfPx, 22u, W) * smoothstep(0.50, 0.36, tone);
		float c = Family(q, fq, s * 0.75, float2(1.0, 0.0), halfPx, 23u, W) * smoothstep(0.26, 0.12, tone);
		return max(a, max(b, c));
	}
	// Crosshatch held to the surfaces (the plane the normal faces most), at a spacing near hatchPx pixels snapped to
	// an octave so it stays still as the camera moves; the two nearest octaves blend. fwp: fwidth of the world
	// position.
	float Hatch(float3 wp, float3 fwp, float3 n, float tone, float cmPerPx, float hatchPx, float halfPx, FTWaveFns W)
	{
		float3 an = abs(n);
		bool zMost = an.z >= max(an.x, an.y);
		bool xMost = an.x >= an.y;
		float2 q = zMost ? wp.xy : (xMost ? wp.yz : wp.xz);
		float2 fq = zMost ? fwp.xy : (xMost ? fwp.yz : fwp.xz);
		float want = max(hatchPx * cmPerPx, 0.25);
		float lv = log2(want);
		float l0 = floor(lv);
		float blend = lv - l0;
		return lerp(HatchAt(q, fq, exp2(l0), tone, halfPx, W), HatchAt(q, fq, exp2(l0 + 1.0), tone, halfPx, W), blend);
	}
};
)HLSL")
TEXT(R"HLSL(
FTWaveFns W;
FTInkFns I;
const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
float2 pix = GetPixelPosition(Parameters);
#if POST_PROCESS_MATERIAL
float2 invView = PostProcessOutput_ViewportSizeInverse;
#else
float2 invView = View.ViewSizeAndInvSize.zw;
#endif
float2 viewSize = 1.0 / max(invView, float2(1e-6, 1e-6));
float2 vuv = pix * invView;
float pxScale = max(viewSize.y, 1.0) / 1080.0;

float3 src = saturate(SceneColor.rgb);
// No wave set: the input, untouched.
if (WOriginScreen.w < 0.5)
{
	return src;
}
float rawDepth = SceneDepthIn.r;
bool sky = rawDepth >= SkyDepth;
bool forward = WDirection < 0.5;
float r = WRadius;
float w = max(WBand, 1.0);
float3 paper = PaperColor.rgb;
float3 ink = InkColor.rgb;
// The paper's tooth: fine grain, a softer mottle and long fibres laid one way.
float fibre = W.VNoise2(pix * float2(0.07, 0.55) / pxScale, 43u);
float tooth = 0.45 * W.HashPx(pix, 977u) + 0.3 * W.VNoise2(pix / (3.0 * pxScale), 41u) + 0.25 * smoothstep(0.55, 0.9, fibre);
float3 paperT = paper * (1.0 - Tooth * tooth);

float3 rel = WorldPos - WOrigin.xyz;
float dist = length(rel);
float3 dir = rel / max(dist, 0.01);
float edge = W.Edge(r, w, dir, WTime, WorldPos);
bool newSide = (dist < edge) == forward;
float3 c = src;
// World cm per pixel at this depth, facing the camera.
float cmPerPx = clamp(rawDepth, 1.0, SkyDepth) * 2.0 / (max(viewSize.y, 1.0) * max(View.ViewToClip[1][1], 1e-3));
// Derivatives up front, in uniform control flow: the rim's distance to the edge and the world position (the hatch).
// The rim's is capped so a silhouette (a depth jump between neighbours) draws no rim of its own.
float g = dist - edge;
float gPerPx = clamp(fwidth(g), 1e-3, 6.0 * cmPerPx);
float3 fwp = fwidth(WorldPos);

if (sky)
{
	// The sky is one far plane. Seen through the dome (the view ray crosses the sphere of the ragged edge) it is the
	// second world's paper, so the dome's outline stands on the sky as it grows, inked, with the leading glow outside
	// it; and the whole sky goes to paper as the camera goes inside (the host ramps CameraInside).
	float3 o3 = CamPos - WOrigin.xyz;
	// The view ray from the pixel at a 10 m depth (the sky's own world position is far enough out to lose precision).
	float4 rayAt = mul(float4(float2(2.0 * vuv.x - 1.0, 1.0 - 2.0 * vuv.y) * 1000.0, 1000.0, 1.0), View.ScreenToTranslatedWorld);
	float3 rd = normalize(rayAt.xyz / max(rayAt.w, 1e-6) - View.TranslatedWorldCameraOrigin);
	float bq = dot(o3, rd);
	float closeT = max(-bq, 0.0);
	float3 pc = o3 + rd * closeT;
	float m = length(pc);
	float3 dirc = pc / max(m, 0.01);
	float domeR = r > 0.0 ? W.Edge(r, w, dirc, WTime, WOrigin.xyz + pc) : 0.0;
	bool camIn = length(o3) < domeR;
	float hit = (domeR > 0.0 && (camIn || (bq < 0.0 && m < domeR))) ? 1.0 : 0.0;
	float skyNew = forward ? max(WInside, hit) : 1.0 - WInside;
	c = lerp(src, paperT, saturate(skyNew));
	if (forward && domeR > 0.0 && !camIn && bq < 0.0)
	{
		float cmAt = max(closeT, 1.0) * 2.0 / (max(viewSize.y, 1.0) * max(View.ViewToClip[1][1], 1e-3));
		float px = (m - domeR) / cmAt;
		float glow = px > 0.0 ? exp(-px / (10.0 * pxScale)) : 0.0;
		c = lerp(c, float3(1.0, 0.99, 0.96), 0.75 * glow);
		c = lerp(c, ink, 1.0 - smoothstep(1.5 * pxScale, 1.5 * pxScale + 1.0, abs(px)));
	}
}
else
{
	// The band between the ragged edge and the outer edge: the world being taken is displaced and smeared outward,
	// along the screen direction away from the origin's image.
	float3 tone = src;
	bool inBand = r > 0.0 && dist >= edge && dist <= r;
	if (inBand)
	{
		float2 away = (vuv - WOriginScreen.xy) * viewSize * (WOriginScreen.z < 0.0 ? -1.0 : 1.0);
		float awayLen = length(away);
		float2 sd = awayLen > 1e-3 ? away / awayLen : float2(0.0, -1.0);
		float bandPos = saturate((dist - edge) / max(r - edge, 1.0)); // 0 at the edge, 1 at the outer edge
		float shape = sin(3.14159265 * bandPos);
		// Noise moving outward with time.
		float n = W.VNoise3(float3(dir.xy * 13.0, dist / 90.0 - WTime * 3.5), 11u) * 2.0 - 1.0;
		float2 duv = sd * (DisplacePx * pxScale * n * shape) * invView;
		float smearPx = lerp(SmearMinPx, SmearMaxPx, W.VNoise3(dir * 23.0 + float3(0.0, 0.0, WTime), 12u)) * pxScale;
		float2 tapStep = sd * smearPx * (1.0 - 0.6 * bandPos) * invView / 6.0;
		float3 acc = float3(0.0, 0.0, 0.0);
		float wsum = 0.0;
		[unroll]
		for (int k = 0; k < 7; ++k)
		{
			float wk = 1.0 - float(k) / 8.0;
			acc += wk * saturate(I.Color(vuv + duv - tapStep * float(k)));
			wsum += wk;
		}
		tone = acc / wsum;
	}

	if (newSide)
	{
		// The second world: paper with tooth, ink lines from depth and normal edges, crosshatch by tone in three
		// directions, the scene's colour kept to SceneKeep.
		float3 n = SceneNormalIn.rgb;
		n = n / max(length(n), 1e-3);
		float2 o = max(LinePx * pxScale, 1.0) * invView;
		float lines = max(I.Lines(vuv, o, SkyDepth, n), I.Detail(vuv, o, kLuma));
		// The tone the hatch follows: the scene's light, and the face's turn from the sun, so a white wall's shadowed
		// faces are hatched (two directions; three on faces turned away from the sun, or where the light is low as well);
		// ground the sun stands more than about 40 degrees over stays clear.
		float lum = dot(tone, kLuma);
		float3 sun = SunDir.xyz;
		sun = dot(sun, sun) > 0.25 ? normalize(sun) : float3(0.42, 0.3, 0.86);
		float shade = saturate(0.45 + 0.55 * dot(n, sun));
		float toneH = min(lum / 0.85, shade);
		float hatch = I.Hatch(WorldPos, fwp, n, toneH, cmPerPx, max(HatchPx * pxScale, 3.0), 0.6 * pxScale, W);
		float3 page = lerp(paperT, ink, saturate(max(hatch * 0.85, lines)));
		c = lerp(page, tone, saturate(SceneKeep));
	}
	else if (inBand)
	{
		c = tone;
	}

	// The tear: an ink line on the ragged edge and, ahead of it, the front's bright leading rim fading out across the
	// band over the world being taken.
	if (r > 0.0 && edge > 0.0)
	{
		float px = g / gPerPx;
		float lead = px > 0.0 && dist <= r ? exp(-px / (9.0 * pxScale)) : 0.0;
		c = lerp(c, float3(1.0, 0.99, 0.96), 0.8 * lead);
		float rim = 1.0 - smoothstep(1.6 * pxScale, 1.6 * pxScale + 1.0, abs(px));
		c = lerp(c, ink, rim);
	}
}
)HLSL")
TEXT(R"HLSL(
// The prelude: one dark, slightly wavy stroke 2 m tall at the origin with a paper halo, drawn up in the first 0.12 s
// (0.4 of the prelude), hidden where the scene is in front of it, fading as the front spreads past 6 m.
float streakFade = 1.0 - saturate(r / 600.0);
if (WPrelude > 0.0 && streakFade > 0.0 && WFoot.z > 0.5 && WTip.z > 0.5)
{
	float grow = saturate(WPrelude / 0.4);
	float2 foot = WFoot.xy * viewSize;
	float2 tip = lerp(WFoot.xy, WTip.xy, grow) * viewSize;
	float2 seg = tip - foot;
	float segLen = max(length(seg), 1e-3);
	float2 along = seg / segLen;
	float2 across = float2(-along.y, along.x);
	float2 p = pix - foot;
	float t = saturate(dot(p, along) / segLen);
	float wave = 3.0 * pxScale * sin(6.2831853 * (t * 1.7 - WTime * 1.3)) * t;
	float off = dot(p, across) - wave;
	float onSeg = step(0.0, dot(p, along)) * step(dot(p, along), segLen);
	float halfW = StreakPx * pxScale * (1.0 - 0.4 * t);
	float body = (1.0 - smoothstep(halfW, halfW + 1.0, abs(off))) * onSeg;
	float haloW = halfW + 3.0 * pxScale;
	float halo = (1.0 - smoothstep(haloW, haloW + 1.5, abs(off))) * onSeg;
	float sDepth = lerp(WFoot.w, WTip.w, t * grow);
	float shown = rawDepth + 15.0 >= sDepth ? 1.0 : 0.0;
	c = lerp(c, paper, 0.7 * halo * shown * streakFade);
	c = lerp(c, ink, body * shown * streakFade);
}
return saturate(c);
)HLSL");
