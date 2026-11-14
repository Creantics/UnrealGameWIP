#include "ftcore/GlassSurface.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

float gsSaturate(float v) { return std::clamp(v, 0.f, 1.f); }
float gsFrac(float v) { return v - std::floor(v); }
float gsSmoothstep(float a, float b, float x) {
    const float t = gsSaturate((x - a) / (b - a));
    return t * t * (3.f - 2.f * t);
}
float gsHash1(float x) { return gsFrac(std::sin(x * 127.1f + 311.7f) * 43758.5453f); }
float gsHash2(float x, float y) { return gsFrac(std::sin(x * 127.1f + y * 311.7f) * 43758.5453f); }
// Value gsNoise, 0..1.
float gsNoise(float x, float y) {
    const float ix = std::floor(x), iy = std::floor(y);
    float fx = x - ix, fy = y - iy;
    fx = fx * fx * (3.f - 2.f * fx);
    fy = fy * fy * (3.f - 2.f * fy);
    const float a = gsHash2(ix, iy), b = gsHash2(ix + 1.f, iy), c = gsHash2(ix, iy + 1.f), d = gsHash2(ix + 1.f, iy + 1.f);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

// Soda-lime glass's absorption a metre (red, green, blue): a 6 mm pane passes about 0.95, 0.99, 0.97 square-on.
constexpr float kGlassAbsorb[3] = {9.f, 2.f, 5.f};

} // namespace

float glassFresnel(float cosView, float f0) {
    const float c = gsSaturate(cosView);
    const float k = 1.f - c;
    return f0 + (1.f - f0) * k * k * k * k * k;
}

float glassPaneMirror(float cosView, float f0) {
    const float f = glassFresnel(cosView, f0);
    return 2.f * f / (1.f + f);
}

std::array<float, 3> glassTint(float cosView, float thicknessM, float age) {
    const float c = gsSaturate(cosView);
    // The path inside the pane, along the refracted ray (index 1.5).
    const float inside = std::sqrt(std::max(1.f - (1.f - c * c) / 2.25f, 0.05f));
    const float path = std::max(thicknessM, 0.f) / inside * (1.f + 3.f * gsSaturate(age));
    return {std::exp(-kGlassAbsorb[0] * path), std::exp(-kGlassAbsorb[1] * path), std::exp(-kGlassAbsorb[2] * path)};
}

GlassSeen glassSeen(float streetLum, float interiorLum, float cosView, float tintLum) {
    GlassSeen s;
    const float r = glassPaneMirror(cosView);
    s.mirrored = std::max(streetLum, 0.f) * r;
    s.through = std::max(interiorLum, 0.f) * (1.f - r) * std::max(tintLum, 0.f);
    s.ratio = s.mirrored > 1e-9f ? s.through / s.mirrored : (s.through > 0.f ? 1e9f : 0.f);
    return s;
}

GlassDetail glassDetail(float u, float v, float width, float height, float seed, float age, float wet, float handleU,
                        float handleV) {
    GlassDetail d;
    const float a = gsSaturate(age), w = gsSaturate(wet);
    const float x = u * width, y = v * height;
    const float ex = std::min(u, 1.f - u) * width, ey = std::min(v, 1.f - v) * height;
    const float ed = std::min(ex, ey);
    d.seal = 1.f - gsSmoothstep(0.006f, 0.010f, ed);
    d.edge = (1.f - gsSmoothstep(0.010f, 0.030f, ed)) * (1.f - d.seal);
    const float n1 = gsNoise(x * 7.f + seed * 31.f, y * 7.f + seed * 17.f);
    const float n2 = gsNoise(x * 23.f + seed * 57.f, y * 5.f + seed * 9.f);
    // Dust only where it gathers: a band hugging the bottom edge and the corners, none across the open pane (round 3: a
    // dust floor over the whole pane, lit by the sun, was a milky film as bright as the shop seen through it).
    const float bottom = std::exp(-y / 0.12f);
    const float corner = std::exp(-std::sqrt(ex * ex + ey * ey) / 0.08f);
    d.dust = gsSaturate((0.55f * bottom + 0.45f * corner) * (0.55f + 0.9f * n1) * (0.45f + 0.75f * a));
    // Dried runs: about 6 cm wide with soft sides (2 cm columns read as hanging threads at 2 m); mostly a rougher
    // reflection, barely a film.
    const float cx = x * 16.f + seed * 13.f;
    const float col = std::floor(cx);
    const float ch = gsHash1(col), ch2 = gsHash1(col + 17.3f);
    const float len = (0.15f + 0.85f * ch * ch) * height * 0.7f;
    const float top = (1.f - v) * height;
    const float across = gsSmoothstep(0.f, 1.f, 1.f - std::abs(gsFrac(cx) - 0.5f) * 2.f);
    d.streak = (ch2 > 0.78f ? 0.6f : 0.f) * (1.f - gsSmoothstep(0.6f * len, len, top)) * across * (0.35f + 0.65f * n2) *
               (0.35f + 0.65f * a);
    if (handleU >= 0.f) {
        const float hx = (u - handleU) * width, hy = (v - handleV) * height;
        d.hand = std::exp(-(hx * hx + hy * hy) / 0.045f) *
                 gsSmoothstep(0.35f, 0.75f, gsNoise(x * 18.f + seed * 91.f, y * 18.f + seed * 3.f)) * 0.8f;
    }
    const float bx = x * 90.f, by = y * 90.f;
    const float br = gsHash2(std::floor(bx) + seed * 7.f, std::floor(by));
    const float bd = std::sqrt((gsFrac(bx) - 0.5f) * (gsFrac(bx) - 0.5f) + (gsFrac(by) - 0.5f) * (gsFrac(by) - 0.5f));
    d.bead = w * (br > 0.8f ? 1.f : 0.f) * (1.f - gsSmoothstep(0.15f, 0.35f, bd));
    d.rough = gsSaturate(0.02f + 0.30f * d.dust + 0.30f * d.streak + 0.35f * d.hand + 0.10f * d.bead);
    d.veil = gsSaturate(0.30f * d.dust + 0.05f * d.streak + 0.25f * d.hand);
    d.mask = gsSmoothstep(0.035f, 0.10f, 0.30f * d.dust + 0.25f * d.hand + 0.5f * d.streak);
    d.cover = kGlassDirtCover * d.mask;
    for (int c = 0; c < 3; ++c) d.dim[static_cast<size_t>(c)] = 1.f + (kGlassDirtDim[static_cast<size_t>(c)] - 1.f) * d.mask;
    return d;
}

float glassMaskShare(float width, float height, float seed, float age, float handleU, float handleV, int n) {
    if (n < 1) return 0.f;
    int in = 0;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
            const float v = (static_cast<float>(j) + 0.5f) / static_cast<float>(n);
            if (glassDetail(u, v, width, height, seed, age, 0.f, handleU, handleV).mask > 0.5f) ++in;
        }
    return static_cast<float>(in) / static_cast<float>(n * n);
}

std::string glassHlsl() {
    // The same maths as the functions above, written out in a line (a Custom node's body takes no function definitions).
    return R"HLSL(
float gC = saturate(gCos);
float gF = 0.04 + 0.96 * pow(1.0 - gC, 5.0);
float gMirror = 2.0 * gF / (1.0 + gF);
float gInside = sqrt(max(1.0 - (1.0 - gC * gC) / 2.25, 0.05));
float gPath = max(gThick, 0.0) / gInside * (1.0 + 3.0 * saturate(gAge));
float3 gTint = exp(-float3(9.0, 2.0, 5.0) * gPath);
float2 gP = gUV * gSize;
float2 gE2 = min(gUV, 1.0 - gUV) * gSize;
float gEd = min(gE2.x, gE2.y);
float gSeal = 1.0 - smoothstep(0.006, 0.010, gEd);
float gEdge = (1.0 - smoothstep(0.010, 0.030, gEd)) * (1.0 - gSeal);
float2 gQ1 = float2(gP.x * 7.0 + gSeed * 31.0, gP.y * 7.0 + gSeed * 17.0);
float2 gI1 = floor(gQ1);
float2 gF1 = frac(gQ1);
gF1 = gF1 * gF1 * (3.0 - 2.0 * gF1);
float gN1 = lerp(lerp(frac(sin(gI1.x * 127.1 + gI1.y * 311.7) * 43758.5453), frac(sin((gI1.x + 1.0) * 127.1 + gI1.y * 311.7) * 43758.5453), gF1.x),
	lerp(frac(sin(gI1.x * 127.1 + (gI1.y + 1.0) * 311.7) * 43758.5453), frac(sin((gI1.x + 1.0) * 127.1 + (gI1.y + 1.0) * 311.7) * 43758.5453), gF1.x), gF1.y);
float2 gQ2 = float2(gP.x * 23.0 + gSeed * 57.0, gP.y * 5.0 + gSeed * 9.0);
float2 gI2 = floor(gQ2);
float2 gF2 = frac(gQ2);
gF2 = gF2 * gF2 * (3.0 - 2.0 * gF2);
float gN2 = lerp(lerp(frac(sin(gI2.x * 127.1 + gI2.y * 311.7) * 43758.5453), frac(sin((gI2.x + 1.0) * 127.1 + gI2.y * 311.7) * 43758.5453), gF2.x),
	lerp(frac(sin(gI2.x * 127.1 + (gI2.y + 1.0) * 311.7) * 43758.5453), frac(sin((gI2.x + 1.0) * 127.1 + (gI2.y + 1.0) * 311.7) * 43758.5453), gF2.x), gF2.y);
float gBottom = exp(-gP.y / 0.12);
float gCorner = exp(-length(gE2) / 0.08);
float gA = saturate(gAge);
float gDust = saturate((0.55 * gBottom + 0.45 * gCorner) * (0.55 + 0.9 * gN1) * (0.45 + 0.75 * gA));
float gCx = gP.x * 16.0 + gSeed * 13.0;
float gCol = floor(gCx);
float gCh = frac(sin(gCol * 127.1 + 311.7) * 43758.5453);
float gCh2 = frac(sin((gCol + 17.3) * 127.1 + 311.7) * 43758.5453);
float gLen = (0.15 + 0.85 * gCh * gCh) * gSize.y * 0.7;
float gTop = (1.0 - gUV.y) * gSize.y;
float gAcross = smoothstep(0.0, 1.0, 1.0 - abs(frac(gCx) - 0.5) * 2.0);
float gStreak = 0.6 * step(0.78, gCh2) * (1.0 - smoothstep(0.6 * gLen, gLen, gTop)) * gAcross * (0.35 + 0.65 * gN2) * (0.35 + 0.65 * gA);
float2 gHd = (gUV - gHandle) * gSize;
float2 gQ3 = float2(gP.x * 18.0 + gSeed * 91.0, gP.y * 18.0 + gSeed * 3.0);
float2 gI3 = floor(gQ3);
float2 gF3 = frac(gQ3);
gF3 = gF3 * gF3 * (3.0 - 2.0 * gF3);
float gN3 = lerp(lerp(frac(sin(gI3.x * 127.1 + gI3.y * 311.7) * 43758.5453), frac(sin((gI3.x + 1.0) * 127.1 + gI3.y * 311.7) * 43758.5453), gF3.x),
	lerp(frac(sin(gI3.x * 127.1 + (gI3.y + 1.0) * 311.7) * 43758.5453), frac(sin((gI3.x + 1.0) * 127.1 + (gI3.y + 1.0) * 311.7) * 43758.5453), gF3.x), gF3.y);
float gHand = gHandle.x >= 0.0 ? exp(-dot(gHd, gHd) / 0.045) * smoothstep(0.35, 0.75, gN3) * 0.8 : 0.0;
float2 gB = gP * 90.0;
float gBr = frac(sin((floor(gB.x) + gSeed * 7.0) * 127.1 + floor(gB.y) * 311.7) * 43758.5453);
float gBead = saturate(gWet) * step(0.8, gBr) * (1.0 - smoothstep(0.15, 0.35, length(frac(gB) - 0.5)));
float gRough = saturate(0.02 + 0.30 * gDust + 0.30 * gStreak + 0.35 * gHand + 0.10 * gBead);
float gVeil = saturate(0.30 * gDust + 0.05 * gStreak + 0.25 * gHand);
float gMask = smoothstep(0.035, 0.10, 0.30 * gDust + 0.25 * gHand + 0.5 * gStreak);
float gCover = 0.35 * gMask;
float3 gDim = lerp(float3(1.0, 1.0, 1.0), float3(0.90, 0.85, 0.76), gMask);
float3 gFilm = float3(0.085, 0.080, 0.068);
float2 gWave = gA * 0.015 * float2(sin(gP.x * 5.1 + gSeed * 40.0), sin(gP.y * 4.3 + gSeed * 23.0));
)HLSL";
}

GlassPane glassPaneAt(float x, const float* cuts, int count) {
    GlassPane p;
    if (!cuts || count < 2) return p;
    float lo = cuts[0], hi = cuts[count - 1];
    for (int k = 1; k + 1 < count; ++k) {
        if (x >= cuts[k]) lo = std::max(lo, cuts[k]);
        else hi = std::min(hi, cuts[k]);
    }
    p.left = lo;
    p.width = std::max(hi - lo, 1e-3f);
    return p;
}

int glassBayStock(float bayW, float doorX0, float doorX1, GlassBayItem* out, int max) {
    // A small general store's window: a plant at each end, a tea table, a stack of crates under a basket, boxes under
    // a brass pot, a jerrycan by them. (share of the width, metres added, metres from the glass, turn, base index)
    struct Plan {
        const char* id;
        float share, dx, y, turn;
        int on;
    };
    static const Plan kPlan[] = {
        {"potted_plant_02", 0.f, 0.35f, 0.45f, 0.f, -1},     {"round_wooden_table_01", 0.17f, 0.f, 0.65f, 0.f, -1},
        {"tea_set_01", 0.17f, -0.10f, 0.62f, 20.f, 1},       {"ceramic_vase_01", 0.17f, 0.22f, 0.72f, 0.f, 1},
        {"plastic_crate_02", 0.40f, 0.f, 0.50f, 0.f, -1},    {"plastic_crate_02", 0.40f, 0.f, 0.50f, 6.f, 4},
        {"wicker_basket_01", 0.40f, 0.f, 0.50f, -10.f, 5},   {"plastic_crate_03", 0.40f, 0.58f, 0.52f, -4.f, -1},
        {"cardboard_box_01", 0.63f, 0.f, 0.50f, 12.f, -1},   {"cardboard_box_01", 0.63f, 0.f, 0.50f, -8.f, 8},
        {"brass_pot_01", 0.63f, 0.f, 0.50f, 0.f, 9},         {"metal_jerrycan", 0.63f, 0.55f, 0.45f, 75.f, -1},
        {"potted_plant_01", 1.f, -0.35f, 0.45f, 30.f, -1},
    };
    constexpr int kPlanCount = static_cast<int>(sizeof(kPlan) / sizeof(kPlan[0]));
    static_assert(kPlanCount <= kGlassBayStockMax, "the plan fits the stock array");
    int kept[kPlanCount];
    int n = 0;
    for (int i = 0; i < kPlanCount && n < max && out; ++i) {
        const Plan& p = kPlan[i];
        kept[i] = -1;
        if (p.on >= 0 && kept[p.on] < 0) continue;
        const float x = std::clamp(p.share * bayW + p.dx, 0.25f, std::max(bayW - 0.25f, 0.25f));
        if (doorX1 >= doorX0 && x > doorX0 - 0.3f && x < doorX1 + 0.3f) continue;
        GlassBayItem& item = out[n];
        item.id = p.id;
        item.x = x;
        item.y = std::max(p.y, 0.3f);
        item.turn = p.turn;
        item.on = p.on >= 0 ? kept[p.on] : -1;
        kept[i] = n++;
    }
    return n;
}

GlassGoodsRow glassGoodsRow(int row, float seed) {
    GlassGoodsRow r;
    const float h = std::sin(static_cast<float>(row) * 7.31f + seed * 13.f) * 43758.5453f;
    const float f = h - std::floor(h);
    r.band = std::clamp(static_cast<int>(f * static_cast<float>(kGlassGoodsBands)), 0, kGlassGoodsBands - 1);
    const float s = seed * 0.37f + static_cast<float>(row) * 0.29f;
    r.start = s - std::floor(s);
    return r;
}

float glassGoodsV(int band, float up) {
    const int b = std::clamp(band, 0, kGlassGoodsBands - 1);
    return (static_cast<float>(b) + 1.f - std::clamp(up, 0.f, 1.f)) / static_cast<float>(kGlassGoodsBands);
}

std::string glassPictureHlsl() {
    return R"HLSL(
float3 iD = float3(iV.x, iV.y, max(iV.z, 0.03));
bool iIsShop = iShop > 0.5;
float iDepth = iIsShop ? 3.4 : 3.0;
float iCeil = iFloor + (iIsShop ? 3.1 : 2.5);
float iCellW = iIsShop ? 4000.0 : 3.6;
float iX0 = floor(iA.x / iCellW) * iCellW;
float iX1 = iX0 + iCellW;
float iStorey = floor(iFloor / 2.9 + 0.5);
float iRoom = frac(sin(dot(float2(iX0 * 0.37 + iSeed * 91.0, iStorey), float2(12.9898, 78.233))) * 43758.5453);
float iRoom2 = frac(iRoom * 57.31 + 0.137);
float iTb = iDepth / iD.z;
float iTy = iD.y < -1e-4 ? (iFloor - iA.y) / iD.y : (iD.y > 1e-4 ? (iCeil - iA.y) / iD.y : 1e9);
float iTx = iD.x < -1e-4 ? (iX0 - iA.x) / iD.x : (iD.x > 1e-4 ? (iX1 - iA.x) / iD.x : 1e9);
float iT = min(iTb, min(iTy, iTx));
float3 iH = float3(iA.x, iA.y, 0.0) + iD * iT;
float iUp = iH.y - iFloor;
float3 iAlb = float3(0.36, 0.35, 0.32);
float iGlow = 0.0;
float iShade = 1.0;
// The goods: rows of the shelf-front picture (GoodsMap: four 2 m x 0.5 m shelf fronts stacked), a band per row by seed,
// a band spanning four times its row's height so the goods keep their shape.
float iMip = clamp(log2(max(iEyeM + iT, 0.1) * 0.66), 0.0, 8.0);
if (iIsShop)
{
	if (iT == iTb)
	{
		float iRow = floor(iUp / 0.42);
		float iRy = frac(iUp / 0.42);
		float iBand = floor(frac(sin(iRow * 7.31 + iSeed * 13.0 + iX0 * 0.11) * 43758.5453) * 3.99);
		float2 iGuv = float2(frac(-iH.x / 1.68 + iSeed * 0.37 + iRow * 0.29), (iBand + 1.0 - iRy) / 4.0);
		float3 iGoods = Texture2DSampleLevel(GoodsMap, GoodsMapSampler, iGuv, iMip).rgb;
		iAlb = iUp > 2.1 ? (iUp > 2.3 && iUp < 2.75 ? lerp(float3(0.5, 0.12, 0.1), float3(0.1, 0.22, 0.42), step(0.5, frac(iSeed * 9.7))) : float3(0.38, 0.37, 0.34))
			: iGoods;
	}
	else if (iT == iTy)
	{
		if (iD.y < 0.0)
		{
			bool iJoint = frac(iH.x / 0.6) < 0.03 || frac(iH.z / 0.6) < 0.03;
			iAlb = float3(0.32, 0.31, 0.29) * (iJoint ? 0.7 : 1.0);
		}
		else
		{
			float2 iPn = float2(frac(iH.x / 1.8), frac(iH.z / 1.6));
			bool iPanel = iPn.x > 0.2 && iPn.x < 0.8 && iPn.y > 0.35 && iPn.y < 0.65;
			iAlb = float3(0.4, 0.4, 0.39);
			iGlow = iPanel ? 1.5 : 0.0;
			iShade = 0.6;
		}
	}
	float iTg = 1.3 / iD.z;
	float2 iPg = iA + iD.xy * iTg;
	float iGy = iPg.y - iFloor;
	if (iTg < iT && iGy > 0.0 && iGy < 1.35 && frac(iPg.x / 3.4) < 0.72)
	{
		float iRow2 = floor(iGy / 0.34);
		float iRy2 = frac(iGy / 0.34);
		float iBand2 = floor(frac(sin(iRow2 * 3.7 + iSeed * 29.0 + floor(iPg.x / 3.4) * 1.3) * 43758.5453) * 3.99);
		float2 iGuv2 = float2(frac(-iPg.x / 1.36 + iSeed * 0.53 + iRow2 * 0.41), (iBand2 + 1.0 - iRy2) / 4.0);
		iAlb = Texture2DSampleLevel(GoodsMap, GoodsMapSampler, iGuv2, clamp(log2(max(iEyeM + iTg, 0.1) * 0.66), 0.0, 8.0)).rgb;
		iH = float3(iPg.x, iPg.y, 1.3);
		iGlow = 0.0;
		iShade = 1.0;
	}
	else if (iD.y < -1e-4)
	{
		float iTt = (iFloor + 1.35 - iA.y) / iD.y;
		float iZt = iD.z * iTt;
		float iXt = iA.x + iD.x * iTt;
		if (iTt > 0.0 && iTt < iT && iZt > 1.3 && iZt < 1.8 && frac(iXt / 3.4) < 0.72)
		{
			iAlb = float3(0.36, 0.35, 0.33);
			iH = float3(iXt, iFloor + 1.35, iZt);
			iGlow = 0.0;
		}
	}
}
else
{
	float3 iWall = lerp(float3(0.4, 0.38, 0.35), float3(0.28 + 0.2 * iRoom, 0.3 + 0.15 * iRoom2, 0.28 + 0.18 * frac(iRoom * 17.3)), 0.3);
	float iLx = iH.x - iX0;
	if (iT == iTb)
	{
		bool iDoor = iLx > 0.5 && iLx < 1.35 && iUp < 2.02;
		bool iPic = iLx > 2.2 && iLx < 2.95 && iUp > 1.15 && iUp < 1.7;
		iAlb = iDoor ? float3(0.3, 0.22, 0.16) : (iPic ? float3(frac(iRoom * 3.3), frac(iRoom * 7.7), frac(iRoom * 11.1)) * 0.4 + 0.1 : iWall);
	}
	else if (iT == iTy)
	{
		if (iD.y < 0.0)
		{
			iAlb = float3(0.36, 0.26, 0.18) * (frac(iH.x / 0.12) < 0.06 ? 0.75 : 1.0);
		}
		else
		{
			float2 iLp = float2(iLx - 0.5 * iCellW, iH.z - 1.5);
			iAlb = float3(0.42, 0.42, 0.41);
			iGlow = dot(iLp, iLp) < 0.035 ? 2.0 : 0.0;
			iShade = 0.6;
		}
	}
	else
	{
		iAlb = iWall * 0.9;
	}
}
float iFall = exp(-iH.z / 2.2);
float3 iLight = iDay * iDayLevel * iFall * iShade + iLit * iLamp * (0.55 + 0.45 * iShade) * (0.7 + 0.3 * iFall);
float3 iCol = iAlb * iLight + iGlow * iLit * iLamp;
if (!iIsShop)
{
	float iLxA = iA.x - iX0;
	float3 iCloth = float3(0.0, 0.0, 0.0);
	float iCover = 0.0;
	if (iRoom < 0.3)
	{
		float iBottom = iFloor + 0.9 + 1.1 * iRoom2;
		iCover = step(iBottom, iA.y);
		iCloth = float3(0.5, 0.49, 0.45) * (0.72 + 0.28 * step(0.5, frac(iA.y / 0.05)));
	}
	else if (iRoom < 0.55)
	{
		float iEdge = iCellW * (0.3 + 0.35 * iRoom2);
		iCover = iRoom2 < 0.5 ? step(iLxA, iEdge) : step(iCellW - iEdge, iLxA);
		iCloth = lerp(float3(0.45, 0.41, 0.35), float3(0.3 + 0.25 * iRoom2, 0.3, 0.28 + 0.25 * iRoom), 0.4) * (0.75 + 0.25 * sin(iA.x * 42.0));
	}
	float3 iClothLight = iDay * iDayLevel * 1.6 + iLit * iLamp * 0.45;
	iCol = lerp(iCol, iCloth * iClothLight, iCover);
}
)HLSL";
}

} // namespace ftcore
