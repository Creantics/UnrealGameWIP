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
    const float bottom = std::exp(-y / 0.30f);
    const float corner = std::exp(-std::sqrt(ex * ex + ey * ey) / 0.12f);
    d.dust = gsSaturate((0.06f + 0.55f * bottom + 0.45f * corner) * (0.55f + 0.9f * n1) * (0.45f + 0.75f * a));
    const float cx = x * 45.f + seed * 13.f;
    const float col = std::floor(cx);
    const float ch = gsHash1(col), ch2 = gsHash1(col + 17.3f);
    const float len = (0.15f + 0.85f * ch * ch) * height * 0.7f;
    const float top = (1.f - v) * height;
    const float across = 1.f - std::abs(gsFrac(cx) - 0.5f) * 2.f;
    d.streak = (ch2 > 0.78f ? 0.6f : 0.f) * (1.f - gsSmoothstep(0.6f * len, len, top)) * across * across * (0.35f + 0.65f * n2) *
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
    d.rough = gsSaturate(0.02f + 0.30f * d.dust + 0.20f * d.streak + 0.35f * d.hand + 0.10f * d.bead);
    d.veil = gsSaturate(0.55f * d.dust + 0.30f * d.streak + 0.25f * d.hand);
    return d;
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
float gBottom = exp(-gP.y / 0.30);
float gCorner = exp(-length(gE2) / 0.12);
float gA = saturate(gAge);
float gDust = saturate((0.06 + 0.55 * gBottom + 0.45 * gCorner) * (0.55 + 0.9 * gN1) * (0.45 + 0.75 * gA));
float gCx = gP.x * 45.0 + gSeed * 13.0;
float gCol = floor(gCx);
float gCh = frac(sin(gCol * 127.1 + 311.7) * 43758.5453);
float gCh2 = frac(sin((gCol + 17.3) * 127.1 + 311.7) * 43758.5453);
float gLen = (0.15 + 0.85 * gCh * gCh) * gSize.y * 0.7;
float gTop = (1.0 - gUV.y) * gSize.y;
float gAcross = 1.0 - abs(frac(gCx) - 0.5) * 2.0;
float gStreak = 0.6 * step(0.78, gCh2) * (1.0 - smoothstep(0.6 * gLen, gLen, gTop)) * gAcross * gAcross * (0.35 + 0.65 * gN2) * (0.35 + 0.65 * gA);
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
float gRough = saturate(0.02 + 0.30 * gDust + 0.20 * gStreak + 0.35 * gHand + 0.10 * gBead);
float gVeil = saturate(0.55 * gDust + 0.30 * gStreak + 0.25 * gHand);
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
float3 iAlb = float3(0.7, 0.68, 0.63);
float iGlow = 0.0;
float iShade = 1.0;
if (iIsShop)
{
	if (iT == iTb)
	{
		float iRow = floor(iUp / 0.42);
		float iRy = frac(iUp / 0.42);
		float iColumn = floor(iH.x / 0.24);
		float iCx = frac(iH.x / 0.24);
		float iG = frac(sin(dot(float2(iColumn, iRow + iSeed * 13.0), float2(12.9898, 78.233))) * 43758.5453);
		float3 iGoods = lerp(float3(0.82, 0.8, 0.74), float3(frac(iG * 7.1), frac(iG * 13.3), frac(iG * 29.7)), 0.65);
		bool iBoard = iRy > 0.9;
		bool iGap = iCx < 0.07 || iRy > 0.5 + 0.35 * frac(iG * 3.7);
		iAlb = iUp > 2.1 ? (iUp > 2.3 && iUp < 2.75 ? lerp(float3(0.74, 0.2, 0.16), float3(0.16, 0.34, 0.6), step(0.5, frac(iSeed * 9.7))) : float3(0.8, 0.78, 0.72))
			: (iBoard ? float3(0.5, 0.48, 0.44) : (iGap ? float3(0.1, 0.095, 0.09) : iGoods));
	}
	else if (iT == iTy)
	{
		if (iD.y < 0.0)
		{
			bool iJoint = frac(iH.x / 0.6) < 0.03 || frac(iH.z / 0.6) < 0.03;
			iAlb = float3(0.56, 0.55, 0.51) * (iJoint ? 0.7 : 1.0);
		}
		else
		{
			float2 iPn = float2(frac(iH.x / 1.8), frac(iH.z / 1.6));
			bool iPanel = iPn.x > 0.2 && iPn.x < 0.8 && iPn.y > 0.35 && iPn.y < 0.65;
			iAlb = float3(0.84, 0.84, 0.82);
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
		float iColumn2 = floor(iPg.x / 0.2);
		float iG2 = frac(sin(dot(float2(iColumn2, iRow2 + iSeed * 29.0), float2(12.9898, 78.233))) * 43758.5453);
		float3 iGoods2 = lerp(float3(0.8, 0.78, 0.72), float3(frac(iG2 * 5.3), frac(iG2 * 17.9), frac(iG2 * 23.1)), 0.7);
		iAlb = iRy2 > 0.88 ? float3(0.55, 0.53, 0.5) : (frac(iPg.x / 0.2) < 0.08 || iRy2 > 0.55 + 0.3 * frac(iG2 * 3.1) ? float3(0.1, 0.095, 0.09) : iGoods2);
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
			iAlb = float3(0.44, 0.42, 0.39);
			iH = float3(iXt, iFloor + 1.35, iZt);
			iGlow = 0.0;
		}
	}
}
else
{
	float3 iWall = lerp(float3(0.76, 0.73, 0.67), float3(0.55 + 0.4 * iRoom, 0.6 + 0.3 * iRoom2, 0.55 + 0.35 * frac(iRoom * 17.3)), 0.3);
	float iLx = iH.x - iX0;
	if (iT == iTb)
	{
		bool iDoor = iLx > 0.5 && iLx < 1.35 && iUp < 2.02;
		bool iPic = iLx > 2.2 && iLx < 2.95 && iUp > 1.15 && iUp < 1.7;
		iAlb = iDoor ? float3(0.3, 0.22, 0.16) : (iPic ? float3(frac(iRoom * 3.3), frac(iRoom * 7.7), frac(iRoom * 11.1)) * 0.7 + 0.15 : iWall);
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
			iAlb = float3(0.82, 0.82, 0.8);
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
		iCloth = float3(0.8, 0.78, 0.72) * (0.72 + 0.28 * step(0.5, frac(iA.y / 0.05)));
	}
	else if (iRoom < 0.55)
	{
		float iEdge = iCellW * (0.3 + 0.35 * iRoom2);
		iCover = iRoom2 < 0.5 ? step(iLxA, iEdge) : step(iCellW - iEdge, iLxA);
		iCloth = lerp(float3(0.72, 0.66, 0.56), float3(0.5 + 0.4 * iRoom2, 0.5, 0.45 + 0.4 * iRoom), 0.4) * (0.75 + 0.25 * sin(iA.x * 42.0));
	}
	float3 iClothLight = iDay * iDayLevel * 1.6 + iLit * iLamp * 0.45;
	iCol = lerp(iCol, iCloth * iClothLight, iCover);
}
)HLSL";
}

} // namespace ftcore
