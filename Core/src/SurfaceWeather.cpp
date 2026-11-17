#include "ftcore/SurfaceWeather.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace ftcore {

namespace {

constexpr float kSideFrom = 0.35f;  // |vertex normal z| under which a face is fully a side
constexpr float kSideTo = 0.6f;
constexpr float kPatchFrom = 0.4f;  // drying-patch noise: damp below, wet above
constexpr float kPatchTo = 0.6f;
constexpr float kWetRamp = 4.0f;    // puddles fade in over Wetness 0..1/kWetRamp
constexpr float kGrimeNoiseFloor = 0.6f;
constexpr float kTopNoiseFloor = 0.55f;
constexpr float kTopOcclusion = 0.3f;
constexpr float kFineFrom = 0.004f;  // m a pixel covers: fine detail full under this, gone over kFineTo
constexpr float kFineTo = 0.015f;
constexpr float kMidFrom = 0.02f;   // the same for the middle detail (5-50 cm features: blotches, chips, joints)
constexpr float kMidTo = 0.1f;
constexpr float kPaveAlbedo = 0.72f;  // a pavement tile's albedo against its tint (the tint stays for the other users)

float saturate(float x) { return std::clamp(x, 0.f, 1.f); }
float lerp(float a, float b, float t) { return a + (b - a) * t; }
// HLSL's smoothstep.
float smoothstep(float a, float b, float x) {
    const float t = saturate((x - a) / (b - a));
    return t * t * (3.f - 2.f * t);
}
float frac(float x) { return x - std::floor(x); }
// The joints' cell hash, as the HLSL writes it.
float jointHash(float cell, float lengthM) { return frac(std::sin(cell * 12.9898f + lengthM * 0.731f) * 43758.5453f); }

std::string num(float f) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(f));
    std::string s(buf);
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

// Value noise on the lattice hash the surface masters use: Noise[0] puddles and Noise[1] drying patches on the plan,
// Noise[2] grime in 3D (when channels is 3). Reads WorldPos (cm).
void noiseLoop(std::string& h, const SurfaceWeatherSpec& spec, int channels) {
    h += "float Noise[" + std::to_string(channels) + "];\n";
    h += "for (int Ch = 0; Ch < " + std::to_string(channels) + "; ++Ch)\n{\n";
    h += "\tfloat3 P = Ch == 0 ? float3(WorldPos.xy / " + num(spec.puddleScaleM * 100.f) + ", 0.0)\n";
    h += "\t\t: (Ch == 1 ? float3(WorldPos.xy / " + num(spec.patchScaleM * 100.f) + " + float2(17.31, -9.77), 0.0)\n";
    h += "\t\t: WorldPos / " + num(spec.grimeScaleM * 100.f) + " + float3(5.13, 11.9, -3.7));\n";
    h += "\tfloat Sum = 0.0;\n";
    h += "\tfloat Weight = 2.0 / 3.0;\n";
    h += "\tfor (int Octave = 0; Octave < 2; ++Octave)\n\t{\n";
    h += "\t\tfloat3 Cell = floor(P);\n";
    h += "\t\tfloat3 Frac = P - Cell;\n";
    h += "\t\tfloat3 Fade = Frac * Frac * (3.0 - 2.0 * Frac);\n";
    h += "\t\tint3 Corner = int3(Cell);\n";
    h += "\t\tfloat Value[8];\n";
    h += "\t\tfor (int K = 0; K < 8; ++K)\n\t\t{\n";
    h += "\t\t\tuint3 I = uint3(Corner + int3(K & 1, (K >> 1) & 1, K >> 2));\n";
    h += "\t\t\tuint H = (I.x * 73856093u) ^ (I.y * 19349663u) ^ (I.z * 83492791u);\n";
    h += "\t\t\tH ^= H >> 13;\n";
    h += "\t\t\tH *= 1274126177u;\n";
    h += "\t\t\tH ^= H >> 16;\n";
    h += "\t\t\tValue[K] = float(H & 0xFFFFu) / 65535.0;\n";
    h += "\t\t}\n";
    h += "\t\tfloat Lo = lerp(lerp(Value[0], Value[1], Fade.x), lerp(Value[2], Value[3], Fade.x), Fade.y);\n";
    h += "\t\tfloat Hi = lerp(lerp(Value[4], Value[5], Fade.x), lerp(Value[6], Value[7], Fade.x), Fade.y);\n";
    h += "\t\tSum += Weight * lerp(Lo, Hi, Fade.z);\n";
    h += "\t\tP = P * 2.03 + 3.7;\n";
    h += "\t\tWeight *= 0.5;\n";
    h += "\t}\n";
    h += "\tNoise[Ch] = Sum;\n";
    h += "}\n";
}

// The noise loop's 16-bit lattice hash (and the puddle cell's), as the HLSL writes it.
float weatherHash16(int x, int y, int z) {
    std::uint32_t v = (static_cast<std::uint32_t>(x) * 73856093u) ^ (static_cast<std::uint32_t>(y) * 19349663u) ^
                      (static_cast<std::uint32_t>(z) * 83492791u);
    v ^= v >> 13;
    v *= 1274126177u;
    v ^= v >> 16;
    return static_cast<float>(v & 0xFFFFu) / 65535.f;
}

constexpr int kPuddleSalt = 211;  // the puddle cell's hashes: z kPuddleSalt + 0..5 on the cell's plan corner
constexpr float kTwoPiW = 6.28318531f;
constexpr int kTrackSalt = 307;   // the worn track's 1D lattice along a slab: (i, 0, kTrackSalt) swing, + 1 width
constexpr float kTrackCore = 0.7f;  // the track is full out to this share of its half width, then fades to its edge
constexpr int kEdgePoolSalt = 401;  // an edge pool's hashes: (pool index, side 0 + / 1 -, kEdgePoolSalt + 0 jitter, 1 size)

// The sides an edge pool lies on: side 0 the slab's +across edge, 1 its - edge.
bool edgePoolOnSide(const SurfaceWeatherSpec& spec, int s) {
    return s == 0 ? spec.edgePoolSide >= 0.f : spec.edgePoolSide <= 0.f;
}

// The edge pools (weatherEdgePoolBowl) as HLSL statements after trackKerbHlsl (TrAcross, TrAlong, FlatSlab) and the
// puddle cell (Noise[0]): raises PdBowl to the pool's bowl on a flat slab. Nothing at pitch 0.
void edgePoolHlsl(std::string& h, const SurfaceWeatherSpec& spec) {
    if (spec.edgePoolPitchM <= 0.f) return;
    h += "float EpHalf = 0.5 * min(Sx, Sy);\n";
    h += "float EpIdx = floor((TrAlong - " + num(spec.edgePoolPhaseM) + ") / " + num(spec.edgePoolPitchM) + " + 0.5);\n";
    h += "float EpWob = 1.0 + " + num(spec.puddleWobble) + " * (2.0 * Noise[0] - 1.0);\n";
    for (int s = 0; s < 2; ++s) {
        if (!edgePoolOnSide(spec, s)) continue;
        const std::string S = std::to_string(s);
        h += "{\n\tfloat EpH[2];\n";
        h += "\tfor (int EpK = 0; EpK < 2; ++EpK)\n\t{\n";
        h += "\t\tuint3 EpI = uint3(int3(int(EpIdx), " + S + ", " + std::to_string(kEdgePoolSalt) + " + EpK));\n";
        h += "\t\tuint EpX = (EpI.x * 73856093u) ^ (EpI.y * 19349663u) ^ (EpI.z * 83492791u);\n";
        h += "\t\tEpX ^= EpX >> 13;\n";
        h += "\t\tEpX *= 1274126177u;\n";
        h += "\t\tEpX ^= EpX >> 16;\n";
        h += "\t\tEpH[EpK] = float(EpX & 0xFFFFu) / 65535.0;\n";
        h += "\t}\n";
        h += "\tfloat EpR = lerp(" + num(spec.edgePoolHalfLoM) + ", " + num(spec.edgePoolHalfHiM) + ", EpH[1]);\n";
        h += "\tfloat EpDa = TrAlong - (" + num(spec.edgePoolPhaseM) + " + EpIdx * " + num(spec.edgePoolPitchM) + " + " +
             num(spec.edgePoolJitterM) + " * (2.0 * EpH[0] - 1.0));\n";
        h += "\tfloat EpDc = EpHalf " + std::string(s == 0 ? "-" : "+") + " TrAcross - " + num(spec.edgePoolInsetM) + ";\n";
        h += "\tfloat EpRho = length(float2(EpDa / (EpR * " + num(spec.edgePoolStretch) + "), EpDc / EpR)) / EpWob;\n";
        h += "\tif (FlatSlab > 0.5) PdBowl = max(PdBowl, " + num(spec.edgePoolDeep) + " * (1.0 - EpRho * EpRho));\n";
        h += "}\n";
    }
}

// A flat ground slab's kerb weight and worn track (weatherKerb, weatherTrack) as HLSL statements after the box frame
// (Sx, Sy, Sz, Uy, Lm, Gut); slabCond an extra condition (the street kind) or empty. Declares Track.
void trackKerbHlsl(std::string& h, const SurfaceWeatherSpec& spec, const std::string& slabCond) {
    h += "float FlatSlab = (Sz <= " + num(spec.topSlabMaxZ) + " && min(Sx, Sy) >= " + num(spec.topSlabMinPlan) +
         " && abs(Uy.z) <= " + num(spec.tiltMin) + (slabCond.empty() ? "" : " && " + slabCond) + ") ? 1.0 : 0.0;\n";
    h += "float TrAcross = Sx >= Sy ? Lm.y : Lm.x;\n";
    h += "float TrAlong = Sx >= Sy ? Lm.x : Lm.y;\n";
    h += "Gut = max(Gut, FlatSlab * " + num(spec.kerbBias) + " * (1.0 - smoothstep(0.0, " + num(spec.gutterM) +
         ", 0.5 * min(Sx, Sy) - abs(TrAcross))));\n";
    h += "float TrA = TrAlong / " + num(spec.trackScaleM) + ";\n";
    h += "float TrC = floor(TrA);\n";
    h += "float TrF = TrA - TrC;\n";
    h += "float TrU = TrF * TrF * (3.0 - 2.0 * TrF);\n";
    h += "float TrN[4];\n";
    h += "for (int TrK = 0; TrK < 4; ++TrK)\n{\n";
    h += "\tuint3 TrI = uint3(int3(int(TrC) + (TrK & 1), 0, " + std::to_string(kTrackSalt) + " + (TrK >> 1)));\n";
    h += "\tuint TrX = (TrI.x * 73856093u) ^ (TrI.y * 19349663u) ^ (TrI.z * 83492791u);\n";
    h += "\tTrX ^= TrX >> 13;\n";
    h += "\tTrX *= 1274126177u;\n";
    h += "\tTrX ^= TrX >> 16;\n";
    h += "\tTrN[TrK] = float(TrX & 0xFFFFu) / 65535.0;\n";
    h += "}\n";
    h += "float TrCtr = " + num(spec.trackWanderM) + " * (2.0 * lerp(TrN[0], TrN[1], TrU) - 1.0);\n";
    h += "float TrHalf = lerp(" + num(spec.trackHalfLoM) + ", " + num(spec.trackHalfHiM) + ", lerp(TrN[2], TrN[3], TrU));\n";
    h += "float Track = (max(Sx, Sy) >= " + num(spec.trackAspect) + " * min(Sx, Sy)) ? FlatSlab * (1.0 - smoothstep(" +
         num(kTrackCore) + " * TrHalf, TrHalf, abs(TrAcross - TrCtr))) : 0.0;\n";
}

// The puddle cell (weatherPuddleBowl) as HLSL statements: reads WorldPos (cm) and Noise[0]; declares <P>Bowl and its
// locals, all prefixed P (so a decal node can hold it beside its own names).
void puddleCellHlsl(std::string& h, const SurfaceWeatherSpec& spec, const std::string& P) {
    const float cell = spec.puddleCellM;
    h += "float2 " + P + "B = floor(WorldPos.xy / " + num(cell * 100.f) + ");\n";
    h += "int2 " + P + "Ci = int2(" + P + "B);\n";
    h += "float " + P + "H[6];\n";
    h += "for (int " + P + "K = 0; " + P + "K < 6; ++" + P + "K)\n{\n";
    h += "\tuint3 " + P + "I = uint3(int3(" + P + "Ci, " + std::to_string(kPuddleSalt) + " + " + P + "K));\n";
    h += "\tuint " + P + "X = (" + P + "I.x * 73856093u) ^ (" + P + "I.y * 19349663u) ^ (" + P + "I.z * 83492791u);\n";
    h += "\t" + P + "X ^= " + P + "X >> 13;\n";
    h += "\t" + P + "X *= 1274126177u;\n";
    h += "\t" + P + "X ^= " + P + "X >> 16;\n";
    h += "\t" + P + "H[" + P + "K] = float(" + P + "X & 0xFFFFu) / 65535.0;\n";
    h += "}\n";
    h += "float " + P + "R = lerp(" + num(spec.puddleRadiusLoM) + ", " + num(spec.puddleRadiusHiM) + ", " + P + "H[1]);\n";
    h += "float " + P + "St = lerp(1.0, " + num(spec.puddleStretch) + ", " + P + "H[2]);\n";
    h += "float " + P + "M = " + P + "R * " + P + "St * " + num(1.f + spec.puddleWobble) + ";\n";
    h += "float2 " + P + "D = WorldPos.xy / 100.0 - ((" + P + "B + 0.5) * " + num(cell) + " + (float2(" + P + "H[4], " + P +
         "H[5]) - 0.5) * max(" + num(cell) + " - 2.0 * " + P + "M, 0.0));\n";
    h += "float " + P + "T = " + num(kTwoPiW) + " * " + P + "H[3];\n";
    h += "float " + P + "Co = cos(" + P + "T);\n";
    h += "float " + P + "Sn = sin(" + P + "T);\n";
    h += "float2 " + P + "E = float2(" + P + "Co * " + P + "D.x + " + P + "Sn * " + P + "D.y, " + P + "Co * " + P + "D.y - " + P +
         "Sn * " + P + "D.x) / float2(" + P + "R * " + P + "St, " + P + "R / " + P + "St);\n";
    h += "float " + P + "Rho = length(" + P + "E) / (1.0 + " + num(spec.puddleWobble) + " * (2.0 * Noise[0] - 1.0));\n";
    h += "float " + P + "Bowl = " + P + "H[0] < " + num(spec.puddleChance) + " ? 1.0 - " + P + "Rho * " + P + "Rho : -1.0;\n";
}

// The water's level over a bowl (weatherPuddle's argument to its edge ramp) as HLSL: bowl, gut and low (empty: none)
// are expressions, wet the wetness.
std::string puddleFillHlsl(const SurfaceWeatherSpec& spec, const std::string& bowl, const std::string& gut,
                           const std::string& low, const std::string& wet) {
    std::string deep = "1.0 + " + num(spec.gutterBias) + " * " + gut;
    if (!low.empty()) deep += " + " + num(spec.lowBias) + " * (2.0 * " + low + " - 1.0)";
    return bowl + " * max(" + deep + ", 0.0) - (1.0 - " + num(spec.puddleCover) + " * " + wet + ")";
}

constexpr int kReliefSteps = 3;      // the relief solve's fixed-point iterations
constexpr float kReliefRelax = 0.25f;  // steps after the first (a full one) move this share of the way: next to no
                                       // fold-back on the steeper convex walls
constexpr float kPaverJointM = 0.003f;  // a lane paver's half joint and bevel (the street layer's literals)
constexpr float kPaverBevelM = 0.006f;
constexpr float kKerbAcrossM = 10000.f;  // a kerb's grid is one cell across: no corners, no chips
constexpr float kTwoPi = 6.28318531f;
constexpr float kBevelFloor = 11.f / 35.f;  // the arris profile's mean floor share: a widened arris stays centred on it
constexpr float kAxisY[2] = {311.f, 173.f};  // the y joints' key offset (apart from the x joints')
constexpr float kNoiseStep[2] = {1.731f, 2.399f};  // the along-joint noise's lattice step in key space
// The factor hashes (seeds and multipliers): width by the joint, depth by the joint, width noise, depth noise.
constexpr float kFacSeed[4][3] = {
    {12.713f, 47.131f, 31871.31f}, {63.317f, 19.973f, 27143.93f}, {41.237f, 83.711f, 19391.77f}, {17.891f, 57.413f, 23417.37f}};

// FTRelief.Hs.
float reliefHash(float kx, float ky, float sx, float sy, float m) { return frac(std::sin(kx * sx + ky * sy) * m); }
float reliefHashS(float kx, float ky, int s) { return reliefHash(kx, ky, kFacSeed[s][0], kFacSeed[s][1], kFacSeed[s][2]); }
// FTRelief.N1: a value noise along a joint (a, in lattice steps) keyed by the joint.
float reliefNoise(float kx, float ky, float a, int s) {
    const float i = std::floor(a);
    float f = a - i;
    f = f * f * (3.f - 2.f * f);
    return lerp(reliefHashS(kx + i * kNoiseStep[0], ky + i * kNoiseStep[1], s),
                reliefHashS(kx + (i + 1.f) * kNoiseStep[0], ky + (i + 1.f) * kNoiseStep[1], s), f);
}
float sgn(float x) { return x > 0.f ? 1.f : (x < 0.f ? -1.f : 0.f); }

// FTRelief.Fac.
StreetJointFactors jointFactors(float x, float y, float keyX, float keyY, const StreetJointGrid& g,
                                const StreetSurfaceSpec& st) {
    float qx = x / g.cellX;
    const float qy = y / g.cellY;
    qx += g.bond * std::fmod(std::floor(qy), 2.f);
    const float xk[2] = {std::round(qx) + keyX, std::floor(qy) + keyY};
    const float yk[2] = {std::floor(x / g.cellX) + keyX + kAxisY[0], std::round(qy) + keyY + kAxisY[1]};
    const float ns = st.jointNoiseShare;
    const auto fac = [&](const float* k, float along, int hs, int nsd, float lo, float hi) {
        return lerp(lo, hi, lerp(reliefHashS(k[0], k[1], hs), reliefNoise(k[0], k[1], along * (1.f / g.noiseM), nsd), ns));
    };
    StreetJointFactors f;
    f.widthX = fac(xk, y, 0, 2, st.jointWidthMin, st.jointWidthMax);
    f.widthY = fac(yk, x, 0, 2, st.jointWidthMin, st.jointWidthMax);
    f.depthX = fac(xk, y, 1, 3, st.jointDepthMin, st.jointDepthMax);
    f.depthY = fac(yk, x, 1, 3, st.jointDepthMin, st.jointDepthMax);
    return f;
}

// FTRelief.Z.
StreetJointSample jointSample(float x, float y, float keyX, float keyY, const StreetJointFactors& fc, float fpX,
                              float fpY, const StreetJointGrid& g, const StreetSurfaceSpec& st) {
    float qx = x / g.cellX;
    const float qy = y / g.cellY;
    qx += g.bond * std::fmod(std::floor(qy), 2.f);
    const float id[2] = {std::floor(qx), std::floor(qy)};
    const float tf[2] = {frac(qx) - 0.5f, frac(qy) - 0.5f};
    const float cell[2] = {g.cellX, g.cellY}, fp[2] = {fpX, fpY};
    const float wf[2] = {fc.widthX, fc.widthY}, df[2] = {fc.depthX, fc.depthY};
    float hh[2], dh[2], wt[2], op[2];
    for (int a = 0; a < 2; ++a) {
        const float dd = (0.5f - std::fabs(tf[a])) * cell[a];
        const float ja = g.halfJointM * wf[a];
        const float bw = std::min(std::max(g.bevelM, fp[a]), std::max(0.4f * cell[a], g.bevelM));
        const float lo = std::max(ja + kBevelFloor * (g.bevelM - bw), 0.f);
        const float t = saturate((dd - lo) / bw);
        const float u = 1.f - t, c = 1.f - u * u;
        hh[a] = c * c * (3.f - 2.f * c);
        dh[a] = 12.f * c * (1.f - c) * u / bw * -sgn(tf[a]) *
                (1.f - smoothstep(st.jointNormalFrom, st.jointNormalTo, fp[a] / (2.f * (ja + g.bevelM))));
        wt[a] = 1.001f - hh[a];
        op[a] = 0.5f * (wf[a] - st.jointWidthMin) / (st.jointWidthMax - st.jointWidthMin) +
                0.5f * (st.jointDepthMax - df[a]) / (st.jointDepthMax - st.jointDepthMin);
    }
    const float dj = g.depthM * (wt[0] * df[0] + wt[1] * df[1]) / (wt[0] + wt[1]);
    const StreetTileFlaw f = streetTileFlaw(id[0] + keyX, id[1] + keyY, st, g.flawScale);
    const float fr = f.tiltX * tf[0] * g.cellX + f.tiltY * tf[1] * g.cellY - f.sinkM;
    float kc = 1.f, dkx = 0.f, dky = 0.f;
    if (f.chipR > 0.f) {
        const float fm = std::max(fp[0], fp[1]);
        const float rx = (tf[0] - 0.5f * f.chipX) * g.cellX, ry = (tf[1] - 0.5f * f.chipY) * g.cellY;
        const float rc = std::max(std::sqrt(rx * rx + ry * ry), 1e-5f);
        const float wc = std::max(0.45f * f.chipR, fm);
        const float s = saturate((rc - f.chipR + wc) / wc);
        kc = s * s * (3.f - 2.f * s);
        const float k = 6.f * s * (1.f - s) / wc / rc * (1.f - smoothstep(0.5f * f.chipR, f.chipR, fm));
        dkx = k * rx;
        dky = k * ry;
    }
    const float ht = hh[0] * hh[1], hk = ht * kc;
    StreetJointSample r;
    r.gx = (dh[0] * hh[1] * kc + ht * dkx) * (fr + dj) + hk * f.tiltX;
    r.gy = (hh[0] * dh[1] * kc + ht * dky) * (fr + dj) + hk * f.tiltY;
    const float gl = std::sqrt(r.gx * r.gx + r.gy * r.gy);
    const float cl = std::min(1.f, st.jointSlopeMax / std::max(gl, 1e-6f));
    r.gx *= cl;
    r.gy *= cl;
    r.z = hk * fr - (1.f - hk) * dj;
    r.floorShare = 1.f - hk;
    r.open = (wt[0] * op[0] + wt[1] * op[1]) / (wt[0] + wt[1]);
    return r;
}

float jointZ(float x, float y, float keyX, float keyY, const StreetJointFactors& fc, const StreetJointGrid& g,
             const StreetSurfaceSpec& st) {
    return jointSample(x, y, keyX, keyY, fc, 0.f, 0.f, g, st).z;
}

// FTRelief: Hs a hash, N1 a noise along a joint, Fac the factors of the joints nearest P (the joint across x: x and
// z, across y: y and w), and Z a jointed grid's height (m; 0 the face, -D times the depth factor a joint's floor) at P
// (m) with its gradient G (m per m) and FO, the floor share (1 in a joint or chip, 0 on a face) and the joint's
// openness. Each axis has its own profile (their product the height): a convex arris widened to the footprint Fp across
// it (centred on its mean floor share) and its slope faded by Fp against the groove. Sunk, tilted and chipped tiles by
// three hashes of the tile's key, none of them the tone's, their shares times Fs.
void reliefStructHlsl(std::string& h, const StreetSurfaceSpec& st) {
    const auto seed = [](int s) {
        return "float2(" + num(kFacSeed[s][0]) + ", " + num(kFacSeed[s][1]) + "), " + num(kFacSeed[s][2]);
    };
    const std::string step = "float2(" + num(kNoiseStep[0]) + ", " + num(kNoiseStep[1]) + ")";
    const std::string ns = num(st.jointNoiseShare);
    h += "struct FTRelief\n{\n";
    h += "\tfloat Hs(float2 k, float2 s, float m) { return frac(sin(dot(k, s)) * m); }\n";
    h += "\tfloat N1(float2 k, float a, float2 s, float m)\n\t{\n";
    h += "\t\tfloat i = floor(a);\n";
    h += "\t\tfloat f = a - i;\n";
    h += "\t\tf = f * f * (3.0 - 2.0 * f);\n";
    h += "\t\treturn lerp(Hs(k + i * " + step + ", s, m), Hs(k + (i + 1.0) * " + step + ", s, m), f);\n";
    h += "\t}\n";
    h += "\tfloat4 Fac(float2 P, float2 Cell, float Bond, float Ni, float2 Key)\n\t{\n";
    h += "\t\tfloat2 Q = P / Cell;\n";
    h += "\t\tQ.x += Bond * fmod(floor(Q.y), 2.0);\n";
    h += "\t\tfloat2 KX = float2(round(Q.x), floor(Q.y)) + Key;\n";
    h += "\t\tfloat2 KY = float2(floor(P.x / Cell.x), round(Q.y)) + Key + float2(" + num(kAxisY[0]) + ", " + num(kAxisY[1]) +
         ");\n";
    h += "\t\tfloat2 A = P.yx * Ni;\n";
    h += "\t\tfloat2 W = lerp(float2(Hs(KX, " + seed(0) + "), Hs(KY, " + seed(0) + ")), float2(N1(KX, A.x, " + seed(2) +
         "), N1(KY, A.y, " + seed(2) + ")), " + ns + ");\n";
    h += "\t\tfloat2 Dp = lerp(float2(Hs(KX, " + seed(1) + "), Hs(KY, " + seed(1) + ")), float2(N1(KX, A.x, " + seed(3) +
         "), N1(KY, A.y, " + seed(3) + ")), " + ns + ");\n";
    h += "\t\treturn float4(lerp(" + num(st.jointWidthMin) + ", " + num(st.jointWidthMax) + ", W), lerp(" +
         num(st.jointDepthMin) + ", " + num(st.jointDepthMax) + ", Dp));\n";
    h += "\t}\n";
    h += "\tfloat Z(float2 P, float2 Cell, float Bond, float J, float B, float D, float Fs, float2 Key, float4 Fc, float2 Fp,"
         " out float2 G, out float2 FO)\n\t{\n";
    h += "\t\tfloat2 Q = P / Cell;\n";
    h += "\t\tQ.x += Bond * fmod(floor(Q.y), 2.0);\n";
    h += "\t\tfloat2 Id = floor(Q);\n";
    h += "\t\tfloat2 TF = frac(Q) - 0.5;\n";
    h += "\t\tfloat2 Dd = (0.5 - abs(TF)) * Cell;\n";
    h += "\t\tfloat2 Ja = J * Fc.xy;\n";
    h += "\t\tfloat2 Bw = min(max(B, Fp), max(0.4 * Cell, B));\n";
    h += "\t\tfloat2 Lo = max(Ja + " + num(kBevelFloor) + " * (B - Bw), 0.0);\n";
    h += "\t\tfloat2 t = saturate((Dd - Lo) / Bw);\n";
    h += "\t\tfloat2 u = 1.0 - t;\n";
    h += "\t\tfloat2 c = 1.0 - u * u;\n";
    h += "\t\tfloat2 H = c * c * (3.0 - 2.0 * c);\n";
    h += "\t\tfloat2 DH = 12.0 * c * (1.0 - c) * u / Bw * -sign(TF) * (1.0 - smoothstep(" + num(st.jointNormalFrom) + ", " +
         num(st.jointNormalTo) + ", Fp / (2.0 * (Ja + B))));\n";
    h += "\t\tfloat2 Wt = 1.001 - H;\n";
    h += "\t\tfloat Dj = D * dot(Wt, Fc.zw) / (Wt.x + Wt.y);\n";
    h += "\t\tfloat2 Op = 0.5 * (Fc.xy - " + num(st.jointWidthMin) + ") / " + num(st.jointWidthMax - st.jointWidthMin) +
         " + 0.5 * (" + num(st.jointDepthMax) + " - Fc.zw) / " + num(st.jointDepthMax - st.jointDepthMin) + ";\n";
    h += "\t\tfloat2 K = Id + Key;\n";
    h += "\t\tfloat U = Hs(K, float2(39.3467, 11.1351), 24634.6345);\n";
    h += "\t\tfloat V = Hs(K, float2(73.156, 52.235), 35791.137);\n";
    h += "\t\tfloat C = Hs(K, float2(27.619, 91.427), 17369.917);\n";
    h += "\t\tfloat Sink = U < " + num(st.sunkShare) + " * Fs ? lerp(" + num(st.sinkMinM) + ", " + num(st.sinkMaxM) +
         ", V) : 0.0;\n";
    h += "\t\tfloat Ang = " + num(kTwoPi) + " * frac(V * 7.13);\n";
    h += "\t\tfloat2 Tg = (U >= " + num(st.sunkShare) + " * Fs && U < " + num(st.sunkShare + st.tiltShare) + " * Fs) ? lerp(" +
         num(st.tiltMinRad) + ", " + num(st.tiltMaxRad) + ", V) * float2(cos(Ang), sin(Ang)) : float2(0.0, 0.0);\n";
    h += "\t\tfloat Fr = dot(Tg, TF * Cell) - Sink;\n";
    h += "\t\tfloat Kc = 1.0;\n";
    h += "\t\tfloat2 DK = float2(0.0, 0.0);\n";
    h += "\t\tif (C < " + num(st.chipShare) + " * Fs)\n\t\t{\n";
    h += "\t\t\tfloat q = C / (" + num(st.chipShare) + " * Fs);\n";
    h += "\t\t\tfloat2 Cs = float2(q < 0.5 ? -1.0 : 1.0, frac(q * 2.0) < 0.5 ? -1.0 : 1.0);\n";
    h += "\t\t\tfloat R = lerp(" + num(st.chipMinM) + ", " + num(st.chipMaxM) + ", frac(q * 4.0));\n";
    h += "\t\t\tfloat Fm = max(Fp.x, Fp.y);\n";
    h += "\t\t\tfloat2 Rv = (TF - 0.5 * Cs) * Cell;\n";
    h += "\t\t\tfloat Rc = max(length(Rv), 1e-5);\n";
    h += "\t\t\tfloat Wc = max(0.45 * R, Fm);\n";
    h += "\t\t\tfloat s = saturate((Rc - R + Wc) / Wc);\n";
    h += "\t\t\tKc = s * s * (3.0 - 2.0 * s);\n";
    h += "\t\t\tDK = 6.0 * s * (1.0 - s) / Wc / Rc * (1.0 - smoothstep(0.5 * R, R, Fm)) * Rv;\n";
    h += "\t\t}\n";
    h += "\t\tfloat Ht = H.x * H.y;\n";
    h += "\t\tfloat HK = Ht * Kc;\n";
    h += "\t\tG = (float2(DH.x * H.y, H.x * DH.y) * Kc + Ht * DK) * (Fr + Dj) + HK * Tg;\n";
    h += "\t\tG *= min(1.0, " + num(st.jointSlopeMax) + " / max(length(G), 1e-6));\n";
    h += "\t\tFO = float2(1.0 - HK, dot(Wt, Op) / (Wt.x + Wt.y));\n";
    h += "\t\treturn HK * Fr - (1.0 - HK) * Dj;\n";
    h += "\t}\n};\n";
}

// A grid's cell and bond, as FTRelief.Fac and Z take them.
std::string cellArgs(const StreetJointGrid& g) {
    return "float2(" + num(g.cellX) + ", " + num(g.cellY) + "), " + num(g.bond);
}
// A grid's arguments to FTRelief.Z after P: Cell, Bond, J, B, D, Fs.
std::string reliefArgs(const StreetJointGrid& g) {
    return cellArgs(g) + ", " + num(g.halfJointM) + ", " + num(g.bevelM) + ", " + num(g.depthM) + ", " + num(g.flawScale);
}

// The relief solve on RlP (m, the grid's frame) by the view's tangent RlT in that frame, inside the near-only branch:
// each step moves the point to where the view ray is at the height under it, then the weight blends it in. The
// factors RlFc and footprints RlFp are the unshifted point's, the same in every step and the final sample.
void reliefSolveHlsl(std::string& h, const std::string& args, const std::string& key, const StreetSurfaceSpec& st) {
    h += "\t\tif (RlW > 0.001)\n\t\t{\n";
    h += "\t\t\tfloat2 RlS = RlT / max(RlN, " + num(st.parallaxSlopeClamp) + ");\n";
    h += "\t\t\tfloat2 RlO = RlP;\n";
    for (int i = 0; i < kReliefSteps; ++i) {
        const std::string to = "RlO + RlR.Z(RlP, " + args + ", " + key + ", RlFc, RlFp, RlG, RlF) * RlS";
        h += "\t\t\tRlP = " + (i == 0 ? to : "lerp(RlP, " + to + ", " + num(kReliefRelax) + ")") + ";\n";
    }
    h += "\t\t\tRlP = lerp(RlO, RlP, RlW);\n";
    h += "\t\t}\n";
}

// The relief's set-up in a jointed kind, after RlT and RlP: the footprint across each axis's joints and the factors.
void reliefSetupHlsl(std::string& h, const StreetJointGrid& g, const std::string& key, const std::string& facP) {
    h += "\t\tfloat2 RlFp = Foot * sqrt(saturate(RlN) * saturate(RlN) + RlT * RlT);\n";
    h += "\t\tfloat4 RlFc = RlR.Fac(" + facP + ", " + cellArgs(g) + ", " + num(1.f / g.noiseM) + ", " + key + ");\n";
}

// The relief's per-unit swings by the unit's tone hash: tone (faded with the middle detail like the plain swing) and
// roughness (its own hash from the same).
void unitSwingHlsl(std::string& h, const StreetSurfaceSpec& st, const std::string& hash) {
    h += "\t\tTone *= 1.0 + " + num(st.unitToneExtra) + " * RlD * (2.0 * " + hash + " - 1.0) * Mid;\n";
    h += "\t\tRoughMul *= 1.0 + " + num(st.unitRoughSwing) + " * RlD * (2.0 * frac(" + hash + " * 7.13) - 1.0);\n";
}

// The street layer's body (inside the node, after the box frame): reads Street, Pw, Foot, VN, Lm, Sx, Sy, Sz, Ux, Uy,
// Uy.z, GutD (with relief also ViewVec, JointDepth, Wet, Up); changes Alb, Rgh and Nrm (with relief declares RlCav and
// RlWet for the AO and the wet roughness after it).
void streetHlsl(std::string& h, const SurfaceWeatherSpec& spec, const StreetSurfaceSpec& st, bool relief, bool detail) {
    const StreetJointGrid paveG = streetJointGrid(StreetKind::Pavement, st);
    const StreetJointGrid paverG = streetJointGrid(StreetKind::Lane, st);
    const StreetJointGrid kerbG = streetJointGrid(StreetKind::Kerb, st);
    if (relief) {
        h += "float RlCav = 0.0;\n";  // the AO's cut
        h += "float RlWet = 0.0;\n";  // the floor's weight for the roughness floor after the weather
        h += "float RlRw = 0.0;\n";   // that floor
    }
    h += "int SK = (int)round(Street);\n";
    h += "bool Tilted = abs(Uy.z) > " + num(spec.tiltMin) + ";\n";
    h += "if (SK > 0)\n{\n";
    h += "\tFTStreetNoise SNz;\n";
    h += "\tfloat Fine = 1.0 - smoothstep(" + num(kFineFrom) + ", " + num(kFineTo) + ", Foot);\n";
    h += "\tfloat TopF = smoothstep(0.7, 0.9, VN.z);\n";
    h += "\tfloat Tone = 1.0;\n";
    h += "\tfloat RoughMul = 1.0;\n";
    h += "\tfloat2 SlopeW = float2(0.0, 0.0);\n";  // height gradient, world x and y, per m
    h += "\tfloat2 SlopeB = float2(0.0, 0.0);\n";  // the same in the box's own x and y
    h += "\tfloat FaceW = TopF;\n";
    h += "\tfloat Mid = 1.0 - smoothstep(" + num(kMidFrom) + ", " + num(kMidTo) + ", Foot);\n";
    h += "\tfloat PolishTop = 0.0;\n";             // share of the way to wheelTopRough (polished stone tops)
    h += "\tfloat AsWheel = 0.0;\n";               // asphalt: the wheel band's weight and the tar seal's, for its final range
    h += "\tfloat AsSeal = 0.0;\n";
    h += "\tfloat LitW = 0.0;\n";                  // kerb litter's cover and colour, over the tone
    h += "\tfloat3 LitC = float3(0.0, 0.0, 0.0);\n";
    if (relief) {
        // The joint relief: the switch, the view (metres, unit, its cosine to the face) and the parallax weight; RlG and
        // RlF take each FTRelief.Z's gradient and floor share, RlFl and RlMean the piece's floor share and its mean.
        h += "\tfloat RlD = saturate(JointDepth);\n";
        h += "\tfloat3 RlV = ViewVec / 100.0;\n";
        h += "\tfloat RlL = max(length(RlV), 1e-4);\n";
        h += "\tRlV = RlV / RlL;\n";
        h += "\tfloat RlN = dot(VN, RlV);\n";
        h += "\tfloat RlW = RlD * (1.0 - smoothstep(" + num(st.parallaxNearM) + ", " + num(st.parallaxFarM) +
             ", RlL)) * smoothstep(" + num(st.parallaxNdVNone) + ", " + num(st.parallaxNdVFull) + ", RlN) * Fine * TopF;\n";
        h += "\tFTRelief RlR;\n";
        h += "\tfloat2 RlG = float2(0.0, 0.0);\n";
        h += "\tfloat2 RlF = float2(1.0, 0.5);\n";
        h += "\tfloat RlFl = 0.0;\n";
        h += "\tfloat RlOp = 0.5;\n";
        h += "\tfloat RlMean = 0.0;\n";
    }
    // Asphalt: the tone layer, aggregate with light chips, ravelled blotches, wheel paths (on a flat slab across its
    // short side too), oil down a lane's centre, joints, sealed cracks and open hairlines.
    h += "\tif (SK == 1)\n\t{\n";
    h += "\t\tfloat Macro = SNz.F(Pw / " + num(st.macroScaleM) + " + 7.3);\n";
    h += "\t\tTone *= 1.0 + " + num(st.macroTone) + " * (2.0 * Macro - 1.0);\n";
    h += "\t\tRoughMul *= 1.0 - 0.06 * (2.0 * Macro - 1.0);\n";
    h += "\t\tfloat2 Ag = Pw / " + num(st.aggregateM) + ";\n";
    h += "\t\tfloat A0 = SNz.N(Ag);\n";
    h += "\t\tfloat Ax = SNz.N(Ag + float2(0.35, 0.0));\n";
    h += "\t\tfloat Ay = SNz.N(Ag + float2(0.0, 0.35));\n";
    // With the detail tile the aggregate's near terms (AgF) give way to the tile; its far tone lift stays.
    h += std::string("\t\tfloat AgF = ") + (detail ? "0.0" : "Fine") + ";\n";
    h += "\t\tSlopeW += float2(Ax - A0, Ay - A0) / 0.35 * " + num(st.aggregateSlope) + " * AgF;\n";
    if (!detail) {
        // Exposed stone chips: light flecks where the binder wore off (rougher), fading to their mean further out. With
        // the tile they are its own (and their far lift would only brighten the whole road).
        h += "\t\tfloat Chip = smoothstep(0.62, 0.8, 0.6 * A0 + 0.4 * SNz.N(Pw / " + num(st.aggregateM * 0.45f) + " + 5.0));\n";
        h += "\t\tTone *= 1.0 + 0.3 * (A0 - 0.5) * AgF + lerp(0.25, 1.4 * Chip, AgF) * 0.5;\n";
        h += "\t\tRoughMul *= 1.0 + 0.1 * Chip * AgF;\n";
    }
    h += "\t\tfloat DTop = 0.0;\n";  // the tile's exposed stone top
    if (detail && st.detailHexM > 0.0f) {
        // Anti-repetition: the tile is read three times on a triangular lattice of detailHexM, each lattice vertex
        // giving its own turn and offset into the tile, blended by sharpened barycentric weights; the blend's lost
        // contrast is put back about the means (variance-preserving, by 1/sqrt(sum w^2)). Each vertex goes to the
        // sample slot of its lattice colour ((a + 2b) mod 3), so a slot keeps its vertex across every triangle edge
        // and only a zero-weight one swaps: implicit mips stay seam-free with no screen derivatives (ray-tracing safe).
        // With the relief's camera distance (RlL, m) the three reads give way to one straight read far off.
        const bool hexFade = relief && st.detailHexFarM > 0.0f;
        h += streetHexUvHlsl(st, "SNz", "\t\t", hexFade ? "RlL" : "");
        h += "\t\tfloat DLr = 0.0;\n\t\tfloat2 DSg = float2(0.0, 0.0);\n\t\tfloat DRr = 0.0;\n\t\tfloat DAo = 0.0;\n";
        const std::string In = hexFade ? "\t\t\t" : "\t\t";
        if (hexFade) h += "\t\t[branch] if (HxNear > 0.0)\n\t\t{\n";
        for (int j = 0; j < 3; ++j) {
            const std::string J = std::to_string(j);
            h += In + "DLr += HxWt" + J + " * dot(Texture2DSample(DetAlb, DetAlbSampler, DUv" + J +
                 ").rgb, float3(0.2126, 0.7152, 0.0722));\n";
            h += In + "float3 DNm" + J + " = UnpackNormalMap(Texture2DSample(DetNrm, DetNrmSampler, DUv" + J + ")).xyz;\n";
            // The tile's slope (du, dv) turned back to the world: the transpose of the sample's turn.
            h += In + "float2 DGt" + J + " = float2(-DNm" + J + ".x, DNm" + J + ".y) / max(DNm" + J + ".z, 0.2);\n";
            h += In + "DSg += HxWt" + J + " * float2(HxC" + J + " * DGt" + J + ".x + HxSn" + J + " * DGt" + J + ".y, -HxSn" + J +
                 " * DGt" + J + ".x + HxC" + J + " * DGt" + J + ".y);\n";
            h += In + "float3 DRa" + J + " = Texture2DSample(DetRA, DetRASampler, DUv" + J + ").rgb;\n";
            h += In + "DRr += HxWt" + J + " * DRa" + J + ".r;\n";
            h += In + "DAo += HxWt" + J + " * DRa" + J + ".g;\n";
            h += In + "DTop += HxWt" + J + " * DRa" + J + ".b;\n";
        }
        if (hexFade) {
            h += "\t\t}\n";
            // The far read: straight on the plan, so its slope needs no turn back.
            h += "\t\t[branch] if (HxFar > 0.0)\n\t\t{\n";
            h += "\t\t\tDLr += HxFar * dot(Texture2DSample(DetAlb, DetAlbSampler, DUvF).rgb, float3(0.2126, 0.7152, 0.0722));\n";
            h += "\t\t\tfloat3 DNmF = UnpackNormalMap(Texture2DSample(DetNrm, DetNrmSampler, DUvF)).xyz;\n";
            h += "\t\t\tDSg += HxFar * float2(-DNmF.x, DNmF.y) / max(DNmF.z, 0.2);\n";
            h += "\t\t\tfloat3 DRaF = Texture2DSample(DetRA, DetRASampler, DUvF).rgb;\n";
            h += "\t\t\tDRr += HxFar * DRaF.r;\n";
            h += "\t\t\tDAo += HxFar * DRaF.g;\n";
            h += "\t\t\tDTop += HxFar * DRaF.b;\n";
            h += "\t\t}\n";
        }
        h += "\t\tDLr = max(1.0 + (DLr / " + num(st.detailAlbedoMean) + " - 1.0) * HxVar, 0.0);\n";
        h += "\t\tTone *= lerp(1.0, DLr, " + num(st.detailTone) + ");\n";
        h += "\t\tSlopeW += DSg * HxVar * " + num(st.detailSlope) + ";\n";
        h += "\t\tDRr = max(1.0 + (DRr / " + num(st.detailRoughMean) + " - 1.0) * HxVar, 0.0);\n";
        h += "\t\tRoughMul *= lerp(1.0, DRr, " + num(st.detailRough) + ");\n";
        h += "\t\tDetAO = lerp(1.0, DAo, " + num(st.detailAO) + ");\n";
    } else if (detail) {
        // The baked tile on the world's plan (image-down along world +y): albedo ratio to its mean, true slope from a
        // DirectX map (x = -dh/du, y = +dh/dv), roughness ratio and cavity AO. Texture2DSample, so a ray-tracing hit
        // takes its mip from the ray cone.
        h += "\t\tfloat2 DUv = Pw / " + num(st.detailTileM) + ";\n";
        h += "\t\tfloat3 DAl = Texture2DSample(DetAlb, DetAlbSampler, DUv).rgb;\n";
        h += "\t\tTone *= lerp(1.0, dot(DAl, float3(0.2126, 0.7152, 0.0722)) / " + num(st.detailAlbedoMean) + ", " +
             num(st.detailTone) + ");\n";
        h += "\t\tfloat3 DNm = UnpackNormalMap(Texture2DSample(DetNrm, DetNrmSampler, DUv)).xyz;\n";
        h += "\t\tSlopeW += float2(-DNm.x, DNm.y) / max(DNm.z, 0.2) * " + num(st.detailSlope) + ";\n";
        h += "\t\tfloat3 DRa = Texture2DSample(DetRA, DetRASampler, DUv).rgb;\n";
        h += "\t\tRoughMul *= lerp(1.0, DRa.r / " + num(st.detailRoughMean) + ", " + num(st.detailRough) + ");\n";
        h += "\t\tDetAO = lerp(1.0, DRa.g, " + num(st.detailAO) + ");\n";
        h += "\t\tDTop = DRa.b;\n";
    }
    // Ravelled blotches, 0.3-2 m: what is left of the grain beyond a few metres. (Repairs are the decals' cut-edge
    // patches, not blots in the layer.)
    h += "\t\tfloat Blot = SNz.N(Pw / 0.6 + 11.0);\n";
    h += "\t\tTone *= 1.0 + 0.22 * (Blot - 0.5) * Mid;\n";
    // Wheel paths: a cambered half from its gutter, a long flat slab across its short side (none on a squarish fill).
    h += "\t\tfloat Wd = Tilted ? Sy : (max(Sx, Sy) > 2.0 * min(Sx, Sy) ? min(Sx, Sy) : 0.0);\n";
    h += "\t\tfloat Across = Tilted ? GutD : (Sx >= Sy ? Lm.y : Lm.x) + 0.5 * Wd;\n";
    h += "\t\tfloat Lanes = max(1.0, round(Wd / " + num(st.laneM) + "));\n";
    h += "\t\tfloat Lw = Wd / Lanes;\n";
    h += "\t\tfloat Q = (frac(Across / max(Lw, 0.01)) - 0.5) * Lw;\n";
    // Two wheel paths a lane, darker and polished, their strength wandering along them; the lane's centre between paler.
    h += "\t\tfloat Wheel = Lw > 2.0 ? exp(-pow((abs(Q) - " + num(st.wheelOffM) + ") / " + num(st.wheelWideM) + ", 2.0)) : 0.0;\n";
    h += "\t\tWheel *= 0.75 + 0.25 * SNz.N(Pw / " + num(st.wheelRunM) + " + 13.0);\n";
    h += "\t\tfloat Centre = Lw > 2.0 ? exp(-pow(Q / 0.35, 2.0)) : 0.0;\n";
    h += "\t\tfloat Oil = Centre * smoothstep(0.5, 0.8, SNz.N(Pw * 0.9 + 23.0));\n";
    h += "\t\tTone *= (1.0 - " + num(st.wheelDarken) + " * 1.6 * Wheel) * (1.0 + " + num(st.wornCentre) + " * Centre) * (1.0 - 0.3 * Oil);\n";
    h += "\t\tRoughMul *= (1.0 - " + num(detail ? st.wheelPolishBinder : st.wheelPolish) + " * Wheel) * (1.0 - 0.3 * Oil);\n";
    h += "\t\tPolishTop = DTop * Wheel;\n";
    h += "\t\tif (Tilted)\n\t\t{\n";
    h += "\t\t\tfloat EdgeBand = smoothstep(0.3, 0.45, GutD) * (1.0 - smoothstep(0.8, 1.0, GutD)) + (1.0 - smoothstep(0.0, 0.4, Sy - GutD));\n";
    h += "\t\t\tTone *= 1.0 + " + num(st.wornEdge) + " * EdgeBand;\n";
    h += "\t\t\tfloat Damp = 1.0 - smoothstep(" + num(st.gutterDampM * 0.6f) + ", " + num(st.gutterDampM) + ", GutD);\n";
    h += "\t\t\tRoughMul *= 1.0 - " + num(st.gutterDampPolish) + " * Damp;\n";
    h += "\t\t\tAsWheel = Damp;\n";  // the damp strip keeps its sheen past the matt clamp, as a wheel band does
    // Kerb litter: pale silt drifted against the kerb, and blossom petals (one at most a cell, an ellipse turned at
    // random, fresh pink or aged brown); far off, their mean cover.
    h += "\t\t\tfloat LitB = 1.0 - smoothstep(0.0, " + num(st.litterM) + ", GutD);\n";
    h += "\t\t\tTone *= 1.0 + " + num(st.litterDust) + " * LitB * smoothstep(0.3, 0.7, SNz.N(Pw / 0.7 + 19.0));\n";
    h += "\t\t\tfloat2 PtC = Pw / " + num(st.petalCellM) + ";\n";
    h += "\t\t\tint2 PtI = int2(floor(PtC));\n";
    h += "\t\t\tfloat PtH = SNz.L(PtI);\n";
    h += "\t\t\tfloat PtH2 = SNz.L(PtI + int2(57, 113));\n";
    h += "\t\t\tfloat PtA = PtH2 * 6.2832;\n";
    h += "\t\t\tfloat2 PtD = frac(PtC) - (0.3 + 0.4 * float2(PtH, frac(PtH * 7.7)));\n";
    h += "\t\t\tPtD = float2(PtD.x * cos(PtA) + PtD.y * sin(PtA), -PtD.x * sin(PtA) + PtD.y * cos(PtA));\n";
    h += "\t\t\tfloat PtIn = 1.0 - smoothstep(0.8, 1.0, length(PtD / float2(0.2, 0.13)));\n";
    h += "\t\t\tfloat PtOn = step(SNz.L(PtI + int2(211, 7)), " + num(st.petalShare) + " * LitB * LitB);\n";
    h += "\t\t\tfloat PtFine = 1.0 - smoothstep(" + num(st.petalCellM * 0.08f) + ", " + num(st.petalCellM * 0.3f) + ", Foot);\n";
    h += "\t\t\tLitW = lerp(" + num(st.petalShare * 0.1f) + " * LitB * LitB, PtIn * PtOn, PtFine);\n";
    h += "\t\t\tLitC = lerp(float3(" + num(st.petalR) + ", " + num(st.petalG) + ", " + num(st.petalB) +
         "), float3(0.2, 0.15, 0.11), smoothstep(0.4, 0.9, frac(PtH * 13.1)));\n";
    h += "\t\t}\n";
    h += "\t\tfloat Run = Lm.x + 0.5 * Sx + (SNz.N(Pw * 0.8 + 3.0) - 0.5) * 0.12;\n";
    h += "\t\tfloat JCell = floor(Run / " + num(st.jointEveryM) + ");\n";
    h += "\t\tfloat JD = 1e4;\n";
    h += "\t\tfor (int Jk = -1; Jk <= 1; ++Jk)\n\t\t{\n";
    h += "\t\t\tfloat Kc = JCell + Jk;\n";
    h += "\t\t\tfloat Jh = frac(sin(Kc * 12.9898 + Sx * 0.731) * 43758.5453);\n";
    h += "\t\t\tJD = min(JD, abs(Run - ((Kc + 0.5) * " + num(st.jointEveryM) + " + (Jh - 0.5) * " + num(2.f * st.jointJitterM) +
         ")));\n";
    h += "\t\t}\n";
    h += "\t\tfloat JW = " + num(0.5f * st.jointWideM) + ";\n";
    h += "\t\tfloat Joint = (1.0 - smoothstep(JW, JW + Foot, JD)) * saturate(2.0 * JW / (2.0 * JW + Foot));\n";
    // Sealed cracks (tar, wide enough to hold from 20 m) and open hairlines (dark, near only).
    h += "\t\tfloat CrackV = SNz.N(Pw / " + num(st.crackScaleM) + " + 41.0);\n";
    // Street class: side lanes more cracked than a main road (the gate's threshold lowered by 0.08 per doubling).
    h += "\t\tfloat CrackCls = Wd <= 0.0 ? 1.0 : lerp(" + num(st.crackSideMul) + ", " + num(st.crackMainMul) + ", smoothstep(" +
         num(st.sideLaneM) + ", " + num(st.mainRoadM) + ", Wd));\n";
    h += "\t\tfloat CrackLo = 0.5 - 0.08 * log2(CrackCls);\n";
    h += "\t\tfloat CrackGate = smoothstep(CrackLo, CrackLo + 0.12, SNz.N(Pw / " + num(st.crackScaleM * 3.f) + " + 3.1));\n";
    h += "\t\tfloat CrackF = Foot / " + num(st.crackScaleM) + ";\n";
    h += "\t\tfloat Crack = (1.0 - smoothstep(0.006, 0.006 + CrackF, abs(CrackV - 0.5))) * saturate(0.012 / (0.012 + CrackF)) * CrackGate;\n";
    h += "\t\tfloat HairV = SNz.N(Pw / 0.9 + 77.0);\n";
    h += "\t\tfloat HairF = Foot / 0.9;\n";
    h += "\t\tfloat Hair = (1.0 - smoothstep(0.004, 0.004 + HairF, abs(HairV - 0.5))) * saturate(0.008 / (0.008 + HairF)) *\n";
    h += "\t\t\tsmoothstep(0.55, 0.7, SNz.N(Pw / 7.0 + 9.0));\n";
    h += "\t\tfloat Seal = max(Joint, Crack);\n";
    h += "\t\tTone *= lerp(1.0, " + num(st.sealDarken) + ", Seal) * (1.0 - 0.6 * Hair);\n";
    h += "\t\tRoughMul *= lerp(1.0, 0.75, Seal) * (1.0 + 0.2 * Hair);\n";
    h += "\t\tAsWheel = saturate(max(AsWheel, Wheel));\n";
    h += "\t\tAsSeal = Seal;\n";
    h += "\t}\n";
    // Footways: grey concrete pavers in running bond along the run, sanded joints, per-paver tone, grime and gum, and
    // pavers that soak unevenly (the wet gloss breaks up paver by paver).
    h += "\telse if (SK == 5)\n\t{\n";
    h += "\t\tbool LongX = Sx >= Sy;\n";
    h += "\t\tfloat2 PL = LongX ? Lm.xy + 0.5 * float2(Sx, Sy) : float2(Lm.y + 0.5 * Sy, Lm.x + 0.5 * Sx);\n";
    if (relief) {
        // The paver frame swaps the box's axes when the run is along y: the view's tangent swaps with it.
        h += "\t\tfloat2 RlKey = float2(round(Sx * 7.0), round(Sy * 13.0));\n";
        h += "\t\tfloat2 RlT = LongX ? float2(dot(RlV, Ux), dot(RlV, Uy)) : float2(dot(RlV, Uy), dot(RlV, Ux));\n";
        h += "\t\tfloat2 RlP = PL;\n";
        reliefSetupHlsl(h, paverG, "RlKey", "RlP");
        reliefSolveHlsl(h, reliefArgs(paverG), "RlKey", st);
        h += "\t\tPL = RlP;\n";
    }
    h += "\t\tfloat2 PP = PL / float2(0.2, 0.1);\n";
    h += "\t\tPP.x += 0.5 * fmod(floor(PP.y), 2.0);\n";
    h += "\t\tfloat2 PId = floor(PP);\n";
    h += "\t\tfloat2 PF = frac(PP) - 0.5;\n";
    h += "\t\tfloat2 PDd = (0.5 - abs(PF)) * float2(0.2, 0.1);\n";
    h += "\t\tbool PNearX = PDd.x < PDd.y;\n";
    h += "\t\tfloat PD = min(PDd.x, PDd.y);\n";
    h += "\t\tfloat Pt = saturate((PD - 0.003) / 0.006);\n";
    h += "\t\tfloat PH = Pt * Pt * (3.0 - 2.0 * Pt);\n";
    h += "\t\tfloat2 PSl = (PNearX ? float2(-sign(PF.x), 0.0) : float2(0.0, -sign(PF.y))) * 6.0 * Pt * (1.0 - Pt) / 0.006 * 0.003 * Fine;\n";
    if (relief) {
        // The relief's slope replaces the plain bevel's (its own footprint fade, not Fine), its floor the plain line.
        h += "\t\tRlR.Z(PL, " + reliefArgs(paverG) + ", RlKey, RlFc, RlFp, RlG, RlF);\n";
        h += "\t\tPSl = lerp(PSl, RlG, RlD);\n";
        h += "\t\tRlFl = RlF.x;\n";
        h += "\t\tRlOp = RlF.y;\n";
        h += "\t\tRlMean = 0.2;\n";
    }
    h += "\t\tSlopeB += LongX ? PSl : PSl.yx;\n";
    h += "\t\tfloat Ph = frac(sin(dot(PId + float2(round(Sx * 7.0), round(Sy * 13.0)), float2(12.9898, 78.233))) * 43758.5453);\n";
    h += "\t\tfloat PJ = lerp(0.2, 1.0 - PH, Mid);\n";  // joint share: 1 in a joint, its mean further out
    h += std::string("\t\tTone *= lerp(1.0, ") + (relief ? "lerp(0.5, 1.0, RlD)" : "0.5") +
         ", PJ) * (1.0 + 0.12 * (2.0 * Ph - 1.0) * Mid);\n";
    if (relief) unitSwingHlsl(h, st, "Ph");
    h += "\t\tTone *= 1.0 + 0.14 * (SNz.N(Pw / 0.006 + 3.0) - 0.5) * Fine;\n";
    h += "\t\tfloat PStain = smoothstep(0.55, 0.8, SNz.F(Pw / 1.8 + 13.0));\n";
    h += "\t\tfloat Gum = smoothstep(0.9, 0.93, SNz.N(Pw / 0.04 + 91.0)) * Fine;\n";
    h += "\t\tTone *= (1.0 - 0.3 * PStain) * (1.0 - 0.5 * Gum);\n";
    h += "\t\tRoughMul *= lerp(1.0, 1.15, PJ) * (1.0 + 0.08 * Ph);\n";
    h += "\t\tSoak *= lerp(lerp(0.45, 1.0, Ph), 1.0, PJ);\n";
    h += "\t}\n";
    // Pavements: tiles with sunk, grimy joints, a lower albedo, per-tile tone and porosity, stains and grit.
    h += "\telse if (SK == 2)\n\t{\n";
    // A junction corner's pieces (told by their depth, cornerSlabM) lay their tiles on the world's grid: the wedges of a
    // fan and the apron behind them share one square grid. The rest keep their own box's.
    h += "\t\tbool CornerP = abs(Sz - " + num(st.cornerSlabM) + ") < 0.004;\n";
    if (relief) {
        // A corner piece's tiles are on the world's plan: its view tangent is the world's x and y, its slope SlopeW's.
        h += "\t\tfloat2 IdB = CornerP ? float2(0.0, 0.0) : float2(round(Sx * 7.0), round(Sy * 13.0));\n";
        h += "\t\tfloat2 RlT = CornerP ? RlV.xy : float2(dot(RlV, Ux), dot(RlV, Uy));\n";
        h += "\t\tfloat2 RlP = CornerP ? Pw : Lm.xy + 0.5 * float2(Sx, Sy);\n";
        reliefSetupHlsl(h, paveG, "IdB", "RlP");
        reliefSolveHlsl(h, reliefArgs(paveG), "IdB", st);
        h += "\t\tfloat2 T = RlP / " + num(st.tileM) + ";\n";
    } else {
        h += "\t\tfloat2 T = (CornerP ? Pw : Lm.xy + 0.5 * float2(Sx, Sy)) / " + num(st.tileM) + ";\n";
    }
    h += "\t\tfloat2 Id = floor(T);\n";
    h += "\t\tfloat2 TF = frac(T) - 0.5;\n";
    h += "\t\tfloat2 Dd = (0.5 - abs(TF)) * " + num(st.tileM) + ";\n";
    h += "\t\tbool NearX = Dd.x < Dd.y;\n";
    h += "\t\tfloat TD = min(Dd.x, Dd.y);\n";
    h += "\t\tfloat Tt = saturate((TD - " + num(st.tileJointM) + ") / " + num(st.tileBevelM) + ");\n";
    h += "\t\tfloat Ht = Tt * Tt * (3.0 - 2.0 * Tt);\n";
    h += "\t\tfloat Dh = 6.0 * Tt * (1.0 - Tt) / " + num(st.tileBevelM) + ";\n";
    h += "\t\tfloat2 TSl = (NearX ? float2(-sign(TF.x), 0.0) : float2(0.0, -sign(TF.y))) * Dh * " + num(st.tileDepthM) + " * Fine;\n";
    if (relief) {
        h += "\t\tRlR.Z(RlP, " + reliefArgs(paveG) + ", IdB, RlFc, RlFp, RlG, RlF);\n";
        h += "\t\tTSl = lerp(TSl, RlG, RlD);\n";
        h += "\t\tRlFl = RlF.x;\n";
        h += "\t\tRlOp = RlF.y;\n";
        h += "\t\tRlMean = 0.12;\n";
    }
    h += "\t\tif (CornerP) { SlopeW += TSl; } else { SlopeB += TSl; }\n";
    h += "\t\tfloat TJ = lerp(0.12, 1.0 - Ht, Mid);\n";  // joint share, its mean further out
    h += "\t\tTone *= " + num(kPaveAlbedo) + " * lerp(1.0, " + (relief ? "lerp(0.45, 1.0, RlD)" : "0.45") + ", TJ);\n";
    if (!relief) h += "\t\tfloat2 IdB = CornerP ? float2(0.0, 0.0) : float2(round(Sx * 7.0), round(Sy * 13.0));\n";
    h += "\t\tfloat Th = frac(sin(dot(Id + IdB, float2(12.9898, 78.233))) * 43758.5453);\n";
    h += "\t\tTone *= 1.0 + " + num(st.tileTone) + " * 1.5 * (2.0 * Th - 1.0) * Mid;\n";
    if (relief) unitSwingHlsl(h, st, "Th");
    h += "\t\tTone *= 1.0 + 0.16 * (SNz.N(Pw / 0.006 + 8.0) - 0.5) * Fine;\n";
    h += "\t\tfloat Stain = smoothstep(0.5, 0.78, SNz.F(Pw / 2.5 + 13.0));\n";
    h += "\t\tfloat Gum = smoothstep(0.9, 0.93, SNz.N(Pw / 0.04 + 57.0)) * Fine;\n";
    h += "\t\tTone *= (1.0 - 0.3 * Stain) * (1.0 - 0.5 * Gum);\n";
    h += "\t\tRoughMul *= lerp(1.0, 1.2, TJ) * (1.0 + 0.1 * Th) * (1.0 - 0.1 * Stain);\n";
    h += "\t\tSoak *= lerp(lerp(0.4, 1.0, Th), 1.0, TJ);\n";
    h += "\t}\n";
    // Kerbs: stones along the run, jointed on top and sides.
    h += "\telse if (SK == 3)\n\t{\n";
    h += "\t\tbool LongX = Sx >= Sy;\n";
    if (relief) {
        // Along the run only (the grid is one cell across); the solve runs on the top face (RlW has TopF), the jointed
        // side faces take the height's normal and cavity without parallax.
        h += "\t\tfloat2 RlKey = float2(round(Sx * 7.0 + Sy * 3.0), 0.0);\n";
        h += "\t\tfloat2 RlT = float2(dot(RlV, LongX ? Ux : Uy), 0.0);\n";
        h += "\t\tfloat2 RlP = float2(LongX ? Lm.x + 0.5 * Sx : Lm.y + 0.5 * Sy, " + num(0.5f * kKerbAcrossM) + ");\n";
        // The factors' noise runs along a joint: across the top, down a side face (the grid's own y is constant).
        reliefSetupHlsl(h, kerbG, "RlKey", "RlP + float2(0.0, (LongX ? Lm.y : Lm.x) + Lm.z)");
        reliefSolveHlsl(h, reliefArgs(kerbG), "RlKey", st);
        h += "\t\tfloat KL = RlP.x / " + num(st.kerbStoneM) + ";\n";
    } else {
        h += "\t\tfloat KL = (LongX ? Lm.x + 0.5 * Sx : Lm.y + 0.5 * Sy) / " + num(st.kerbStoneM) + ";\n";
    }
    h += "\t\tfloat KF = frac(KL) - 0.5;\n";
    h += "\t\tfloat KD = (0.5 - abs(KF)) * " + num(st.kerbStoneM) + ";\n";
    h += "\t\tfloat Kt = saturate((KD - " + num(st.tileJointM) + ") / " + num(st.tileBevelM) + ");\n";
    h += "\t\tfloat KH = Kt * Kt * (3.0 - 2.0 * Kt);\n";
    h += "\t\tfloat KDh = 6.0 * Kt * (1.0 - Kt) / " + num(st.tileBevelM) + ";\n";
    if (relief) {
        h += "\t\tRlR.Z(RlP, " + reliefArgs(kerbG) + ", RlKey, RlFc, RlFp, RlG, RlF);\n";
        h += "\t\tSlopeB += lerp((LongX ? float2(-sign(KF), 0.0) : float2(0.0, -sign(KF))) * KDh * 0.006 * Fine, "
             "LongX ? float2(RlG.x, 0.0) : float2(0.0, RlG.x), RlD);\n";
        h += "\t\tRlFl = RlF.x;\n";
        h += "\t\tRlOp = RlF.y;\n";
        h += "\t\tRlMean = 0.03;\n";
    } else {
        h += "\t\tSlopeB += (LongX ? float2(-sign(KF), 0.0) : float2(0.0, -sign(KF))) * KDh * 0.006 * Fine;\n";
    }
    h += "\t\tFaceW = 1.0;\n";
    h += std::string("\t\tTone *= lerp(1.0, lerp(") + (relief ? "lerp(0.6, 1.0, RlD)" : "0.6") + ", 1.0, KH), Fine);\n";
    h += "\t\tfloat Kh = frac(sin(floor(KL) * 12.9898 + round(Sx * 7.0 + Sy * 3.0) * 4.1) * 43758.5453);\n";
    h += "\t\tTone *= 1.0 + 0.07 * (2.0 * Kh - 1.0);\n";
    if (relief) unitSwingHlsl(h, st, "Kh");
    h += "\t}\n";
    // Markings: paint at 0.75, faded, chipped, ragged at the edges, worn where wheels cross a line across the lane.
    h += "\telse if (SK == 4 || SK == 6)\n\t{\n";
    h += "\t\tfloat Mx = max(max(Alb.r, Alb.g), max(Alb.b, 1e-3));\n";
    h += "\t\tfloat Fade = SNz.F(Pw / 1.2 + 5.0);\n";
    h += "\t\tfloat3 Paint = Alb * (" + num(st.paintAlbedo) + " / Mx) * lerp(0.85, 1.0, Fade);\n";
    h += "\t\tfloat Chip = lerp(0.5, SNz.N(Pw / 0.05 + 9.0), Fine);\n";
    h += "\t\tfloat MW = Sy > 1.5 ? exp(-pow((abs(Lm.y) - " + num(st.wheelOffM) + ") / 0.3, 2.0)) : 0.0;\n";
    h += "\t\tfloat Left = smoothstep(-0.06, 0.06, 1.15 - 0.55 * Fade - 0.65 * MW - Chip);\n";
    h += "\t\tfloat ME = min(0.5 * Sx - abs(Lm.x), 0.5 * Sy - abs(Lm.y));\n";
    h += "\t\tLeft *= lerp(1.0, smoothstep(0.0, 0.012, ME - 0.02 * SNz.N(Pw / 0.03 + 2.0)), Fine);\n";
    // Tactile paving (the ramps' yellow strips, 0.6 m deep; no line is that wide): domes, no wear down to the road.
    h += "\t\tif (SK == 6 || (Alb.b < 0.3 * Alb.r && min(Sx, Sy) > 0.5 && min(Sx, Sy) < 0.7))\n\t\t{\n";
    h += "\t\t\tfloat2 Dm = frac(Lm.xy / 0.06) - 0.5;\n";
    h += "\t\t\tfloat DmR = length(Dm);\n";
    h += "\t\t\tSlopeB += Dm / max(DmR, 1e-3) * -0.5 * (1.0 - smoothstep(0.15, 0.3, DmR)) * smoothstep(0.0, 0.15, DmR) * Fine;\n";
    h += "\t\t\tLeft = 1.0;\n";
    h += "\t\t\tPaint = Alb * (0.6 / Mx) * lerp(0.9, 1.0, Fade);\n";
    h += "\t\t}\n";
    h += "\t\tAlb = lerp(float3(" + num(st.asphaltR) + ", " + num(st.asphaltG) + ", " + num(st.asphaltB) + "), Paint, Left);\n";
    h += "\t\tRgh = lerp(0.85, Rgh, Left);\n";
    h += "\t}\n";
    if (relief) {
        // The groove's floor (RlC its share, the floor share's mean far off; 0 off the jointed kinds): a gritty grey,
        // jointFloorToneLo..Hi of the unit by a fine grit noise, dirt patches darker, darker again wet (the weather's
        // wetness thrice as fast: grooves stay wet longest); rough, at least jointRoughDry (jointRoughWet wet) plus the
        // grit; its AO cut jointCavityAO, less where the joint is open (wide, shallow).
        h += "\tfloat RlC = lerp(RlMean, RlFl, Mid) * RlD;\n";
        h += "\tfloat RlGr = lerp(0.5, SNz.N(Pw / 0.005 + 29.0), Fine);\n";
        h += "\tfloat RlDt = lerp(0.2, smoothstep(0.55, 0.8, SNz.N(Pw / 0.12 + 47.0)), Mid);\n";
        h += "\tfloat RlWk = saturate(Wet * 3.0) * Up;\n";
        h += "\tTone *= lerp(1.0, lerp(" + num(st.jointFloorToneHi) + ", " + num(st.jointFloorToneLo) + ", RlGr) * lerp(1.0, " +
             num(st.jointDirtTone) + ", RlDt) * lerp(1.0, " + num(st.jointWetTone) + ", RlWk), RlC);\n";
        h += "\tRoughMul *= 1.0 + RlC * lerp(-0.05, 0.1, RlGr);\n";
        h += "\tRlCav = RlC * (1.0 - lerp(" + num(st.jointCavityAO) + ", " + num(st.jointOpenAO) + ", lerp(0.5, RlOp, Mid)));\n";
        h += "\tRlRw = lerp(" + num(st.jointRoughDry) + ", " + num(st.jointRoughWet) + ", RlWk) + " + num(st.jointRoughGrit) +
             " * RlGr;\n";
        h += "\tRlWet = RlC;\n";
    }
    h += "\tAlb *= Tone;\n";
    h += "\tRgh = saturate(Rgh * RoughMul);\n";
    h += "\tRgh = lerp(Rgh, " + num(st.wheelTopRough) + ", saturate(PolishTop));\n";
    // The asphalt's final range: one material in sun and shade, so no term may push the binder pale or the stone tips
    // cream (luminance clamped, hue kept); matt off the wheel paths, a little polished in the bands (more on the stone
    // peaks), tar seal glossy. Before the litter (petals keep their own colour) and the weather.
    h += "\tif (SK == 1)\n\t{\n";
    h += "\t\tfloat AsL = dot(Alb, float3(0.2126, 0.7152, 0.0722));\n";
    h += "\t\tAlb *= clamp(AsL, " + num(st.asphaltAlbedoLo) + ", " + num(st.asphaltAlbedoHi) + ") / max(AsL, 1e-4);\n";
    h += "\t\tfloat AsR = clamp(Rgh, " + num(st.asphaltRoughLo) + ", " + num(st.asphaltRoughHi) + ");\n";
    h += "\t\tAsR = lerp(AsR, lerp(" + num(st.wheelBandRough) + ", " + num(st.wheelBandRoughTop) + ", saturate(PolishTop / max(AsWheel, 1e-3))), AsWheel);\n";
    h += "\t\tRgh = lerp(AsR, " + num(st.sealRough) + ", saturate(AsSeal));\n";
    h += "\t}\n";
    h += "\tAlb = lerp(Alb, LitC, LitW);\n";
    h += "\tRgh = lerp(Rgh, 0.7, LitW);\n";
    if (relief) h += "\tRgh = lerp(Rgh, max(Rgh, RlRw), RlC);\n";
    h += "\tNrm = normalize(Nrm - FaceW * (SlopeB.x * Ux + SlopeB.y * Uy + float3(SlopeW, 0.0)));\n";
    h += "}\n";
}

}  // namespace

float weatherUp(float normalZ, const SurfaceWeatherSpec& spec) { return smoothstep(spec.upFrom, spec.upTo, normalZ); }

float weatherSide(float normalZ) { return 1.f - smoothstep(kSideFrom, kSideTo, std::fabs(normalZ)); }

float weatherSoak(float wetness, float up, float patchNoise, const SurfaceWeatherSpec& spec) {
    return saturate(wetness) * up * lerp(spec.dampShare, 1.f, smoothstep(kPatchFrom, kPatchTo, patchNoise));
}

float weatherGutter(float axisYz, float localY, float sizeY, const SurfaceWeatherSpec& spec) {
    if (std::fabs(axisYz) <= spec.tiltMin) return 0.f;
    const float d = 0.5f * sizeY + (axisYz > 0.f ? localY : -localY);
    return 1.f - smoothstep(0.f, spec.gutterM, d);
}

float weatherNoise(float px, float py, float pz) {
    float p[3] = {px, py, pz};
    float sum = 0.f;
    float weight = 2.f / 3.f;
    for (int octave = 0; octave < 2; ++octave) {
        int corner[3];
        float fade[3];
        for (int a = 0; a < 3; ++a) {
            const float cell = std::floor(p[a]);
            const float f = p[a] - cell;
            corner[a] = static_cast<int>(cell);
            fade[a] = f * f * (3.f - 2.f * f);
        }
        float value[8];
        for (int k = 0; k < 8; ++k)
            value[k] = weatherHash16(corner[0] + (k & 1), corner[1] + ((k >> 1) & 1), corner[2] + (k >> 2));
        const float lo = lerp(lerp(value[0], value[1], fade[0]), lerp(value[2], value[3], fade[0]), fade[1]);
        const float hi = lerp(lerp(value[4], value[5], fade[0]), lerp(value[6], value[7], fade[0]), fade[1]);
        sum += weight * lerp(lo, hi, fade[2]);
        for (float& c : p) c = c * 2.03f + 3.7f;
        weight *= 0.5f;
    }
    return sum;
}

float weatherRimNoise(float xM, float yM, const SurfaceWeatherSpec& spec) {
    return weatherNoise(xM / spec.puddleScaleM, yM / spec.puddleScaleM, 0.f);
}

float weatherPuddleBowl(float xM, float yM, float rimNoise, const SurfaceWeatherSpec& spec) {
    const float cell = spec.puddleCellM;
    const float bx = std::floor(xM / cell), by = std::floor(yM / cell);
    const int cx = static_cast<int>(bx), cy = static_cast<int>(by);
    float hv[6];
    for (int k = 0; k < 6; ++k) hv[k] = weatherHash16(cx, cy, kPuddleSalt + k);
    const float r = lerp(spec.puddleRadiusLoM, spec.puddleRadiusHiM, hv[1]);
    const float st = lerp(1.f, spec.puddleStretch, hv[2]);
    const float margin = r * st * (1.f + spec.puddleWobble);
    const float room = std::max(cell - 2.f * margin, 0.f);
    const float dx = xM - ((bx + 0.5f) * cell + (hv[4] - 0.5f) * room);
    const float dy = yM - ((by + 0.5f) * cell + (hv[5] - 0.5f) * room);
    const float t = kTwoPiW * hv[3];
    const float co = std::cos(t), sn = std::sin(t);
    const float ex = (co * dx + sn * dy) / (r * st), ey = (co * dy - sn * dx) / (r / st);
    const float rho = std::sqrt(ex * ex + ey * ey) / (1.f + spec.puddleWobble * (2.f * rimNoise - 1.f));
    return hv[0] < spec.puddleChance ? 1.f - rho * rho : -1.f;
}

float weatherPuddleCapShare(const SurfaceWeatherSpec& spec) {
    const float r = spec.puddleRadiusHiM * (1.f + spec.puddleWobble);
    return 3.14159265f * r * r / (spec.puddleCellM * spec.puddleCellM);
}

float weatherPuddle(float wetness, float up, float bowl, float gutter, float low, const SurfaceWeatherSpec& spec) {
    const float wet = saturate(wetness);
    const float deep = std::max(1.f + spec.gutterBias * gutter + spec.lowBias * (2.f * low - 1.f), 0.f);
    return up * saturate(wet * kWetRamp) * smoothstep(0.f, spec.puddleEdge, bowl * deep - (1.f - spec.puddleCover * wet));
}

float weatherJointRough(float rough, float joint, float wet, float up, float puddle, const SurfaceWeatherSpec& spec) {
    return lerp(rough, spec.jointWetRough, saturate(joint) * saturate(wet) * up * (1.f - puddle));
}

float weatherJointDarken(float joint, float wet, float up, float puddle, const SurfaceWeatherSpec& spec) {
    return lerp(1.f, spec.jointWetDarken, saturate(joint) * saturate(wet) * up * (1.f - puddle));
}

float weatherDirt(float side, float thinnest, float aboveM, float edgeM, float noise, const SurfaceWeatherSpec& spec) {
    const float thick = smoothstep(spec.grimeThinM, spec.grimeThinM * 1.5f, thinnest);
    const float low = 1.f - smoothstep(0.f, spec.grimeHeightM, aboveM);
    const float grime = spec.grimeStrength * std::pow(low, spec.grimeCurve) * lerp(kGrimeNoiseFloor, 1.f, noise);
    const float edge = spec.edgeStrength * (1.f - smoothstep(0.f, spec.edgeWidthM, edgeM));
    return saturate(side * thick * (grime + edge));
}

float weatherGroundSlab(float sz, float sx, float sy, const SurfaceWeatherSpec& spec) {
    return sz <= spec.topSlabMaxZ && std::min(sx, sy) >= spec.topSlabMinPlan ? 1.f : 0.f;
}

float weatherTopDirt(float edgeM, float noise, float slab, const SurfaceWeatherSpec& spec) {
    return saturate(slab * spec.topGrimeStrength * (1.f - smoothstep(0.f, spec.topGrimeM, edgeM)) *
                    lerp(kTopNoiseFloor, 1.f, noise));
}

float weatherRough(float rough, float soak, float puddle, const SurfaceWeatherSpec& spec) {
    return weatherRoughTrack(rough, soak, puddle, 0.f, spec);
}

float weatherRoughTrack(float rough, float soak, float puddle, float track, const SurfaceWeatherSpec& spec) {
    const float dry = rough * (1.f - spec.trackDryPolish * saturate(track));
    const float soaked = lerp(lerp(spec.soakRough, spec.trackRough, saturate(track)), dry, spec.soakKeep);
    const float r = std::min(dry, lerp(dry, soaked, std::pow(saturate(soak), spec.roughCurve)));
    return saturate(lerp(r, spec.puddleRough, smoothstep(spec.puddleCoreFrom, 1.f, puddle)));
}

float weatherFlatSlab(float sz, float sx, float sy, float axisYz, const SurfaceWeatherSpec& spec) {
    return std::fabs(axisYz) <= spec.tiltMin ? weatherGroundSlab(sz, sx, sy, spec) : 0.f;
}

float weatherTrack(float acrossM, float alongM, float longM, float shortM, float flat, const SurfaceWeatherSpec& spec) {
    if (flat <= 0.f || longM < spec.trackAspect * shortM) return 0.f;
    const float a = alongM / spec.trackScaleM;
    const float c = std::floor(a);
    const int i = static_cast<int>(c);
    const float f = a - c, u = f * f * (3.f - 2.f * f);
    const float swing = lerp(weatherHash16(i, 0, kTrackSalt), weatherHash16(i + 1, 0, kTrackSalt), u);
    const float width = lerp(weatherHash16(i, 0, kTrackSalt + 1), weatherHash16(i + 1, 0, kTrackSalt + 1), u);
    const float centre = spec.trackWanderM * (2.f * swing - 1.f);
    const float half = lerp(spec.trackHalfLoM, spec.trackHalfHiM, width);
    return flat * (1.f - smoothstep(kTrackCore * half, half, std::fabs(acrossM - centre)));
}

float weatherKerb(float edgeM, float flat, const SurfaceWeatherSpec& spec) {
    return flat * spec.kerbBias * (1.f - smoothstep(0.f, spec.gutterM, edgeM));
}

float weatherEdgePoolBowl(float acrossM, float alongM, float shortM, float flat, float rimNoise,
                          const SurfaceWeatherSpec& spec) {
    if (flat <= 0.5f || spec.edgePoolPitchM <= 0.f) return -1.f;
    const float half = 0.5f * shortM;
    const float idx = std::floor((alongM - spec.edgePoolPhaseM) / spec.edgePoolPitchM + 0.5f);
    const int i = static_cast<int>(idx);
    const float wob = 1.f + spec.puddleWobble * (2.f * rimNoise - 1.f);
    float best = -1.f;
    for (int s = 0; s < 2; ++s) {
        if (!edgePoolOnSide(spec, s)) continue;
        const float h0 = weatherHash16(i, s, kEdgePoolSalt), h1 = weatherHash16(i, s, kEdgePoolSalt + 1);
        const float r = lerp(spec.edgePoolHalfLoM, spec.edgePoolHalfHiM, h1);
        const float da = alongM - (spec.edgePoolPhaseM + idx * spec.edgePoolPitchM + spec.edgePoolJitterM * (2.f * h0 - 1.f));
        const float dc = half + (s == 0 ? -acrossM : acrossM) - spec.edgePoolInsetM;
        const float ea = da / (r * spec.edgePoolStretch), ec = dc / r;
        const float rho = std::sqrt(ea * ea + ec * ec) / wob;
        best = std::max(best, spec.edgePoolDeep * (1.f - rho * rho));
    }
    return best;
}

bool applySurfaceWeatherOverrides(SurfaceWeatherSpec& spec, std::string_view text, std::string* error) {
    struct Field {
        const char* name;
        float SurfaceWeatherSpec::* member;
    };
    static constexpr Field kFields[] = {
        {"upFrom", &SurfaceWeatherSpec::upFrom}, {"upTo", &SurfaceWeatherSpec::upTo},
        {"soakDarken", &SurfaceWeatherSpec::soakDarken}, {"soakRough", &SurfaceWeatherSpec::soakRough},
        {"soakKeep", &SurfaceWeatherSpec::soakKeep}, {"roughCurve", &SurfaceWeatherSpec::roughCurve},
        {"soakFlatten", &SurfaceWeatherSpec::soakFlatten}, {"dampShare", &SurfaceWeatherSpec::dampShare},
        {"patchScaleM", &SurfaceWeatherSpec::patchScaleM}, {"puddleScaleM", &SurfaceWeatherSpec::puddleScaleM},
        {"puddleCellM", &SurfaceWeatherSpec::puddleCellM}, {"puddleChance", &SurfaceWeatherSpec::puddleChance},
        {"puddleRadiusLoM", &SurfaceWeatherSpec::puddleRadiusLoM}, {"puddleRadiusHiM", &SurfaceWeatherSpec::puddleRadiusHiM},
        {"puddleStretch", &SurfaceWeatherSpec::puddleStretch}, {"puddleWobble", &SurfaceWeatherSpec::puddleWobble},
        {"puddleCover", &SurfaceWeatherSpec::puddleCover}, {"puddleEdge", &SurfaceWeatherSpec::puddleEdge},
        {"puddleCoreFrom", &SurfaceWeatherSpec::puddleCoreFrom}, {"puddleRough", &SurfaceWeatherSpec::puddleRough},
        {"puddleDarken", &SurfaceWeatherSpec::puddleDarken}, {"trackRough", &SurfaceWeatherSpec::trackRough},
        {"trackHalfLoM", &SurfaceWeatherSpec::trackHalfLoM}, {"trackHalfHiM", &SurfaceWeatherSpec::trackHalfHiM},
        {"trackWanderM", &SurfaceWeatherSpec::trackWanderM}, {"trackScaleM", &SurfaceWeatherSpec::trackScaleM},
        {"trackAspect", &SurfaceWeatherSpec::trackAspect}, {"trackDryPolish", &SurfaceWeatherSpec::trackDryPolish},
        {"kerbBias", &SurfaceWeatherSpec::kerbBias}, {"tiltMin", &SurfaceWeatherSpec::tiltMin},
        {"edgePoolPitchM", &SurfaceWeatherSpec::edgePoolPitchM}, {"edgePoolPhaseM", &SurfaceWeatherSpec::edgePoolPhaseM},
        {"edgePoolSide", &SurfaceWeatherSpec::edgePoolSide}, {"edgePoolJitterM", &SurfaceWeatherSpec::edgePoolJitterM},
        {"edgePoolInsetM", &SurfaceWeatherSpec::edgePoolInsetM}, {"edgePoolHalfLoM", &SurfaceWeatherSpec::edgePoolHalfLoM},
        {"edgePoolHalfHiM", &SurfaceWeatherSpec::edgePoolHalfHiM}, {"edgePoolStretch", &SurfaceWeatherSpec::edgePoolStretch},
        {"edgePoolDeep", &SurfaceWeatherSpec::edgePoolDeep},
        {"gutterM", &SurfaceWeatherSpec::gutterM}, {"gutterBias", &SurfaceWeatherSpec::gutterBias},
        {"lowBias", &SurfaceWeatherSpec::lowBias}, {"jointWetDarken", &SurfaceWeatherSpec::jointWetDarken},
        {"jointWetRough", &SurfaceWeatherSpec::jointWetRough}, {"grimeHeightM", &SurfaceWeatherSpec::grimeHeightM},
        {"grimeStrength", &SurfaceWeatherSpec::grimeStrength}, {"grimeScaleM", &SurfaceWeatherSpec::grimeScaleM},
        {"grimeThinM", &SurfaceWeatherSpec::grimeThinM}, {"edgeWidthM", &SurfaceWeatherSpec::edgeWidthM},
        {"edgeStrength", &SurfaceWeatherSpec::edgeStrength}, {"dirtR", &SurfaceWeatherSpec::dirtR},
        {"dirtG", &SurfaceWeatherSpec::dirtG}, {"dirtB", &SurfaceWeatherSpec::dirtB},
        {"dirtOcclusion", &SurfaceWeatherSpec::dirtOcclusion}, {"grimeCurve", &SurfaceWeatherSpec::grimeCurve},
        {"topGrimeM", &SurfaceWeatherSpec::topGrimeM}, {"topGrimeStrength", &SurfaceWeatherSpec::topGrimeStrength},
        {"topSlabMaxZ", &SurfaceWeatherSpec::topSlabMaxZ}, {"topSlabMinPlan", &SurfaceWeatherSpec::topSlabMinPlan},
    };
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find(',', at);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view entry = text.substr(at, end - at);
        at = end + 1;
        if (entry.empty()) continue;
        const std::size_t eq = entry.find('=');
        const std::string_view key = entry.substr(0, eq);
        const Field* hit = nullptr;
        for (const Field& f : kFields)
            if (eq != std::string_view::npos && key == f.name) hit = &f;
        const std::string value = eq == std::string_view::npos ? std::string() : std::string(entry.substr(eq + 1));
        char* stop = nullptr;
        const float v = value.empty() ? 0.f : std::strtof(value.c_str(), &stop);
        if (hit == nullptr || value.empty() || stop != value.c_str() + value.size() || !std::isfinite(v)) {
            if (error != nullptr) *error = std::string(entry);
            return false;
        }
        spec.*(hit->member) = v;
    }
    return true;
}

float weatherAlbedoScale(int channel, float soak, float puddle, float dirt, const SurfaceWeatherSpec& spec) {
    const float tint = channel == 0 ? spec.dirtR : (channel == 1 ? spec.dirtG : spec.dirtB);
    return lerp(1.f, spec.soakDarken, soak) * lerp(1.f, spec.puddleDarken, puddle) * lerp(1.f, tint, dirt);
}

float streetWheelPath(float fromLowM, float sizeY, const StreetSurfaceSpec& spec) {
    const float lanes = std::max(1.f, std::round(sizeY / spec.laneM));
    const float lw = sizeY / lanes;
    const float q = (frac(fromLowM / lw) - 0.5f) * lw;
    const float t = (std::fabs(q) - spec.wheelOffM) / spec.wheelWideM;
    return std::exp(-t * t);
}

float streetJointDistance(float posM, float lengthM, const StreetSurfaceSpec& spec) {
    const float cell = std::floor(posM / spec.jointEveryM);
    float best = 1e4f;
    for (int k = -1; k <= 1; ++k) {
        const float kc = cell + static_cast<float>(k);
        const float at = (kc + 0.5f) * spec.jointEveryM + (jointHash(kc, lengthM) - 0.5f) * 2.f * spec.jointJitterM;
        best = std::min(best, std::fabs(posM - at));
    }
    return best;
}

float streetPaintLeft(float fade, float wheel, float chip) {
    return smoothstep(-0.06f, 0.06f, 1.15f - 0.55f * fade - 0.65f * wheel - chip);
}

bool streetCornerSlab(float sz, const StreetSurfaceSpec& spec) { return std::fabs(sz - spec.cornerSlabM) < 0.004f; }

float streetTileHeight(float dM, const StreetSurfaceSpec& spec) {
    return smoothstep(spec.tileJointM, spec.tileJointM + spec.tileBevelM, dM);
}

StreetJointGrid streetJointGrid(StreetKind kind, const StreetSurfaceSpec& spec) {
    StreetJointGrid g;
    if (kind == StreetKind::Lane) {
        g.cellX = 0.2f;
        g.cellY = 0.1f;
        g.bond = 0.5f;
        g.halfJointM = kPaverJointM;
        g.bevelM = kPaverBevelM;
        g.depthM = spec.paverJointDepthM;
        g.flawScale = spec.paverFlawScale;
        g.noiseM = spec.paverNoiseM;
    } else if (kind == StreetKind::Kerb) {
        g.cellX = spec.kerbStoneM;
        g.cellY = kKerbAcrossM;
        g.halfJointM = spec.tileJointM;
        g.bevelM = spec.tileBevelM;
        g.depthM = spec.kerbJointDepthM;
        g.noiseM = spec.kerbNoiseM;
    } else {
        g.cellX = spec.tileM;
        g.cellY = spec.tileM;
        g.halfJointM = spec.tileJointM;
        g.bevelM = spec.tileBevelM;
        g.depthM = spec.paveJointDepthM;
        g.noiseM = spec.jointNoiseM;
    }
    g.bevelM *= spec.jointBevelScale;
    return g;
}

StreetTileFlaw streetTileFlaw(float keyX, float keyY, const StreetSurfaceSpec& spec, float shareScale) {
    const float u = reliefHash(keyX, keyY, 39.3467f, 11.1351f, 24634.6345f);
    const float v = reliefHash(keyX, keyY, 73.156f, 52.235f, 35791.137f);
    const float c = reliefHash(keyX, keyY, 27.619f, 91.427f, 17369.917f);
    const float sunk = spec.sunkShare * shareScale, chip = spec.chipShare * shareScale;
    StreetTileFlaw f;
    if (u < sunk) f.sinkM = lerp(spec.sinkMinM, spec.sinkMaxM, v);
    if (u >= sunk && u < (spec.sunkShare + spec.tiltShare) * shareScale) {
        const float ang = kTwoPi * frac(v * 7.13f);
        const float s = lerp(spec.tiltMinRad, spec.tiltMaxRad, v);
        f.tiltX = s * std::cos(ang);
        f.tiltY = s * std::sin(ang);
    }
    if (c < chip) {
        const float q = c / chip;
        f.chipX = q < 0.5f ? -1.f : 1.f;
        f.chipY = frac(q * 2.f) < 0.5f ? -1.f : 1.f;
        f.chipR = lerp(spec.chipMinM, spec.chipMaxM, frac(q * 4.f));
    }
    return f;
}

StreetJointFactors streetJointFactors(float x, float y, float keyX, float keyY, const StreetJointGrid& grid,
                                      const StreetSurfaceSpec& spec) {
    return jointFactors(x, y, keyX, keyY, grid, spec);
}

StreetJointSample streetJointSample(float x, float y, float keyX, float keyY, const StreetJointFactors& f, float footX,
                                    float footY, const StreetJointGrid& grid, const StreetSurfaceSpec& spec) {
    return jointSample(x, y, keyX, keyY, f, footX, footY, grid, spec);
}

float streetJointZ(float x, float y, float keyX, float keyY, const StreetJointGrid& grid, const StreetSurfaceSpec& spec) {
    return jointZ(x, y, keyX, keyY, jointFactors(x, y, keyX, keyY, grid, spec), grid, spec);
}

float streetJointFootprint(float foot, float nDotV, float viewAcross) {
    const float n = saturate(nDotV);
    return foot * std::sqrt(n * n + viewAcross * viewAcross);
}

float streetJointFloorTone(float grit, float dirt, float wet, const StreetSurfaceSpec& spec) {
    return lerp(spec.jointFloorToneHi, spec.jointFloorToneLo, saturate(grit)) * lerp(1.f, spec.jointDirtTone, saturate(dirt)) *
           lerp(1.f, spec.jointWetTone, saturate(wet));
}

float streetJointFloorRough(float rough, float grit, float wet, const StreetSurfaceSpec& spec) {
    return std::max(rough, lerp(spec.jointRoughDry, spec.jointRoughWet, saturate(wet)) + spec.jointRoughGrit * saturate(grit));
}

float streetParallaxWeight(float distM, float nDotV, float jointDepth, float fine, const StreetSurfaceSpec& spec) {
    return saturate(jointDepth) * (1.f - smoothstep(spec.parallaxNearM, spec.parallaxFarM, distM)) *
           smoothstep(spec.parallaxNdVNone, spec.parallaxNdVFull, nDotV) * fine;
}

StreetPoint streetReliefShift(float x, float y, float viewX, float viewY, float nDotV, float keyX, float keyY,
                              const StreetJointGrid& grid, const StreetSurfaceSpec& spec) {
    const float c = std::max(nDotV, spec.parallaxSlopeClamp);
    const float sx = viewX / c, sy = viewY / c;
    StreetPoint p{x, y};
    const StreetJointFactors f = jointFactors(x, y, keyX, keyY, grid, spec);  // the unshifted point's, as the HLSL
    for (int i = 0; i < kReliefSteps; ++i) {
        const float z = jointZ(p.x, p.y, keyX, keyY, f, grid, spec);
        p.x = lerp(p.x, x + z * sx, i == 0 ? 1.f : kReliefRelax);
        p.y = lerp(p.y, y + z * sy, i == 0 ? 1.f : kReliefRelax);
    }
    return p;
}

float streetLatticeValue(int x, int y) {
    std::uint32_t h = (static_cast<std::uint32_t>(x) * 73856093u) ^ (static_cast<std::uint32_t>(y) * 19349663u);
    h ^= h >> 13;
    h *= 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / 16777215.f;
}

float streetNoise(float x, float y) {
    const float fx = std::floor(x), fy = std::floor(y);
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
    const float tx = x - fx, ty = y - fy;
    const float ux = tx * tx * (3.f - 2.f * tx), uy = ty * ty * (3.f - 2.f * ty);
    return lerp(lerp(streetLatticeValue(ix, iy), streetLatticeValue(ix + 1, iy), ux),
                lerp(streetLatticeValue(ix, iy + 1), streetLatticeValue(ix + 1, iy + 1), ux), uy);
}

const char* streetLatticeHlsl() {
    return "float L(int2 c) { uint h = (uint(c.x) * 73856093u) ^ (uint(c.y) * 19349663u); h ^= h >> 13; h *= 1274126177u; "
           "h ^= h >> 16; return float(h & 0xFFFFFFu) / 16777215.0; }";
}

float streetCrackClass(float widthM, const StreetSurfaceSpec& st) {
    if (!(widthM > 0.0f)) return 1.0f;
    return lerp(st.crackSideMul, st.crackMainMul, smoothstep(st.sideLaneM, st.mainRoadM, widthM));
}

std::string streetHexUvHlsl(const StreetSurfaceSpec& st, const std::string& noise, const std::string& indent,
                            const std::string& distM) {
    // The tile read three times on a triangular lattice of detailHexM (see the street layer's tile branch).
    const std::string& I = indent;
    const bool fade = !distM.empty() && st.detailHexFarM > 0.0f;
    std::string h;
    h += I + "float2 DP = Pw / " + num(st.detailTileM) + ";\n";
    h += I + "float2 HxP = Pw / " + num(st.detailHexM) + ";\n";
    h += I + "float2 HxS = float2(HxP.x - 0.57735027 * HxP.y, 1.15470054 * HxP.y);\n";
    h += I + "float2 HxFl = floor(HxS);\n";
    h += I + "int2 HxB = int2(HxFl);\n";
    h += I + "float2 HxF = HxS - HxFl;\n";
    h += I + "float HxZ = 1.0 - HxF.x - HxF.y;\n";
    h += I + "float HxU = step(HxZ, 0.0);\n";  // 1 in the rhombus's upper triangle
    h += I + "int HxK = HxB.x + 2 * HxB.y;\n";
    h += I + "HxK -= 3 * int(floor(float(HxK) / 3.0));\n";
    // Weights of the triangle's vertices by their place: the corner (base or base + (1,1)), base + (1,0), base + (0,1).
    h += I + "float3 HxW = float3(abs(HxZ), lerp(HxF.x, 1.0 - HxF.y, HxU), lerp(HxF.y, 1.0 - HxF.x, HxU));\n";
    h += I + "HxW = pow(max(HxW, 0.0), " + num(st.detailHexSharp) + ");\n";
    h += I + "HxW /= max(HxW.x + HxW.y + HxW.z, 1e-5);\n";
    h += I + "float HxVar = clamp(rsqrt(max(dot(HxW, HxW), 1e-4)), 1.0, 1.7320508);\n";
    for (int j = 0; j < 3; ++j) {
        const std::string J = std::to_string(j);
        h += I + "int HxR" + J + " = (" + J + " - HxK + 3) % 3;\n";
        h += I + "int2 HxV" + J + " = HxB + (HxR" + J + " == 0 ? int2(int(HxU), int(HxU)) : (HxR" + J +
             " == 1 ? int2(1, 0) : int2(0, 1)));\n";
        h += I + "float HxWt" + J + " = HxR" + J + " == 0 ? HxW.x : (HxR" + J + " == 1 ? HxW.y : HxW.z);\n";
        h += I + "float HxA" + J + " = " + num(6.2831853f * st.detailHexTurn) + " * " + noise + ".L(HxV" + J + " + int2(13, 97));\n";
        h += I + "float HxC" + J + " = cos(HxA" + J + ");\n" + I + "float HxSn" + J + " = sin(HxA" + J + ");\n";
        h += I + "float2 DUv" + J + " = float2(HxC" + J + " * DP.x - HxSn" + J + " * DP.y, HxSn" + J + " * DP.x + HxC" + J +
             " * DP.y) + float2(" + noise + ".L(HxV" + J + "), " + noise + ".L(HxV" + J + " + int2(71, 29)));\n";
    }
    if (fade) {
        // Far off, one straight read (the header's note): the blend on the camera distance, the weights scaled by it.
        h += I + "float HxNear = 1.0 - smoothstep(" + num(st.detailHexFarM * 0.8f) + ", " + num(st.detailHexFarM) + ", " + distM +
             ");\n";
        h += I + "float HxFar = 1.0 - HxNear;\n";
        h += I + "float2 DUvF = DP;\n";
        h += I + "HxWt0 *= HxNear;\n" + I + "HxWt1 *= HxNear;\n" + I + "HxWt2 *= HxNear;\n";
        h += I + "HxVar = lerp(1.0, HxVar, HxNear);\n";
    }
    return h;
}

std::string surfaceWeatherHlsl(const SurfaceWeatherSpec& spec, const SurfaceWeatherOptions& options,
                               const StreetSurfaceSpec& street) {
    const bool box = options.boxFrame;
    const bool dirt = box && options.grime;
    const bool top = box && options.topGrime;
    const bool streets = box && options.street;
    const bool relief = streets && options.jointDepth;
    const bool detail = streets && options.streetDetail;
    std::string h;
    h.reserve(16384);
    if (relief) reliefStructHlsl(h, street);
    if (streets) {
        // A 2D value noise for the street layer, on the integer lattice hash the decals use (streetLatticeHlsl).
        h += "struct FTStreetNoise\n{\n";
        h += "\t";
        h += streetLatticeHlsl();
        h += "\n";
        h += "\tfloat N(float2 p)\n\t{\n";
        h += "\t\tfloat2 b = floor(p);\n";
        h += "\t\tint2 i = int2(b);\n";
        h += "\t\tfloat2 f = p - b;\n";
        h += "\t\tfloat2 u = f * f * (3.0 - 2.0 * f);\n";
        h += "\t\treturn lerp(lerp(L(i), L(i + int2(1, 0)), u.x), lerp(L(i + int2(0, 1)), L(i + int2(1, 1)), u.x), u.y);\n";
        h += "\t}\n";
        h += "\tfloat F(float2 p) { return 0.5 * N(p) + 0.3 * N(p * 2.03 + 17.1) + 0.2 * N(p * 4.01 + 31.7); }\n";
        h += "};\n";
    }
    h += "float3 VN = normalize(VertexNormal);\n";
    h += "float Up = smoothstep(" + num(spec.upFrom) + ", " + num(spec.upTo) + ", VN.z);\n";
    h += "float Wet = saturate(Wetness) * saturate(Amount);\n";
    h += "float3 Alb = Albedo;\n";
    h += "float Rgh = Rough;\n";
    h += "float3 Nrm = Normal;\n";
    if (streets) {
        // The pixel's footprint in metres, outside any branch (fine detail fades with it).
        h += "float Foot = max(length(fwidth(WorldPos.xyz)) / 100.0, 1e-4);\n";
        h += "float2 Pw = WorldPos.xy / 100.0;\n";
    }
    noiseLoop(h, spec, (dirt || top) ? 3 : 2);
    h += "float Soak = Wet * Up * lerp(" + num(spec.dampShare) + ", 1.0, smoothstep(" + num(kPatchFrom) + ", " +
         num(kPatchTo) + ", Noise[1]));\n";
    h += "float Gut = 0.0;\n";
    if (box) {
        // The box's size in metres (the cube is 100 cm a side, so an axis's length is its scale) and the pixel in its
        // frame, metres from the centre.
        h += "float Sx = max(length(AxisX), 1e-4);\n";
        h += "float Sy = max(length(AxisY), 1e-4);\n";
        h += "float Sz = max(length(AxisZ), 1e-4);\n";
        h += "float3 Ux = AxisX / Sx;\n";
        h += "float3 Uy = AxisY / Sy;\n";
        h += "float3 Lm = LocalPos / 100.0 * float3(Sx, Sy, Sz);\n";
        h += "float GutD = 0.5 * Sy + (Uy.z > 0.0 ? Lm.y : -Lm.y);\n";
        h += "Gut = abs(Uy.z) > " + num(spec.tiltMin) + " ? 1.0 - smoothstep(0.0, " + num(spec.gutterM) + ", GutD) : 0.0;\n";
    }
    if (detail) h += "float DetAO = 1.0;\n";
    if (streets) streetHlsl(h, spec, street, relief, detail);
    // A flat ground slab's kerb line fills its bowls; its worn track soaks fully, smoother (a street piece has its own
    // wheel paths, so only a non-street slab carries one).
    if (box) {
        trackKerbHlsl(h, spec, streets ? "SK == 0" : "");
        h += "Soak = max(Soak, Wet * Up * Track);\n";
        if (spec.trackDryPolish > 0.f) h += "Rgh *= 1.0 - " + num(spec.trackDryPolish) + " * Track;\n";
    } else {
        h += "float Track = 0.0;\n";
    }
    // Joints holding water soak fully (no drying patch in a joint).
    if (options.waterInput) {
        h += "float PdJw = saturate(Water.x) * Wet * Up;\n";
        h += "Soak = max(Soak, PdJw);\n";
    }
    // One puddle a cell at most, in its bowl (the header): no puddle outside one, whatever the gutter or low spot.
    puddleCellHlsl(h, spec, "Pd");
    if (box) edgePoolHlsl(h, spec);
    h += "float Pud = Up * saturate(Wet * " + num(kWetRamp) + ") * smoothstep(0.0, " + num(spec.puddleEdge) + ", " +
         puddleFillHlsl(spec, "PdBowl", "Gut", options.waterInput ? "saturate(Water.y)" : "", "Wet") + ");\n";
    h += "float3 C = Alb * (lerp(1.0, " + num(spec.soakDarken) + ", Soak) * lerp(1.0, " + num(spec.puddleDarken) +
         ", Pud));\n";
    h += "float R = min(Rgh, lerp(Rgh, lerp(lerp(" + num(spec.soakRough) + ", " + num(spec.trackRough) + ", Track), Rgh, " +
         num(spec.soakKeep) + "), pow(saturate(Soak), " + num(spec.roughCurve) + ")));\n";
    h += "float PudCore = smoothstep(" + num(spec.puddleCoreFrom) + ", 1.0, Pud);\n";
    h += "R = lerp(R, " + num(spec.puddleRough) + ", PudCore);\n";
    if (options.waterInput) {
        // A joint holding water: darker, and the wet grit's roughness, not a mirror (a puddle's own numbers win).
        h += "C *= lerp(1.0, " + num(spec.jointWetDarken) + ", PdJw * (1.0 - Pud));\n";
        h += "R = lerp(R, " + num(spec.jointWetRough) + ", PdJw * (1.0 - Pud));\n";
    }
    // The joint floor's roughness floor again after the soak (no glints off a wet joint), but not under a puddle.
    if (relief) h += "R = lerp(R, max(R, RlRw), RlWet * (1.0 - Pud));\n";
    h += "float A = AO;\n";
    if (relief) h += "A *= 1.0 - RlCav;\n";
    if (detail) h += "A *= DetAO;\n";
    const std::string tint = "float3(" + num(spec.dirtR) + ", " + num(spec.dirtG) + ", " + num(spec.dirtB) + ")";
    if (dirt) {
        h += "float Side = 1.0 - smoothstep(" + num(kSideFrom) + ", " + num(kSideTo) + ", abs(VN.z));\n";
        h += "float Thick = smoothstep(" + num(spec.grimeThinM) + ", " + num(spec.grimeThinM * 1.5f) + ", min(Sx, Sy));\n";
        h += std::string("float Height = ") + (options.aboveInput ? "Above" : "Lm.z + 0.5 * Sz") + ";\n";
        h += "float Low = 1.0 - smoothstep(0.0, " + num(spec.grimeHeightM) + ", Height);\n";
        h += "float Grime = " + num(spec.grimeStrength) + " * pow(Low, " + num(spec.grimeCurve) + ") * lerp(" +
             num(kGrimeNoiseFloor) + ", 1.0, Noise[2]);\n";
        // A side face's distance to its nearer vertical edge: across Y on an X face, across X on a Y face.
        h += "float EdgeD = abs(dot(VN, Ux)) >= abs(dot(VN, Uy)) ? 0.5 * Sy - abs(Lm.y) : 0.5 * Sx - abs(Lm.x);\n";
        h += "float Edge = " + num(spec.edgeStrength) + " * (1.0 - smoothstep(0.0, " + num(spec.edgeWidthM) + ", EdgeD));\n";
        h += "float Dirt = saturate(Side * Thick * (Grime + Edge));\n";
        h += "C *= lerp(float3(1.0, 1.0, 1.0), " + tint + ", Dirt);\n";
        h += "A *= 1.0 - " + num(spec.dirtOcclusion) + " * Dirt;\n";
    }
    if (top) {
        // A ground slab's top: dirt along its gutter when cambered, else along both long edges. With the street layer,
        // a pavement always, a carriageway half always (its gutter), other street kinds never; without, by its size.
        h += "bool SizeSlab = Sz <= " + num(spec.topSlabMaxZ) + " && min(Sx, Sy) >= " + num(spec.topSlabMinPlan) + ";\n";
        h += "bool Cambered = abs(Uy.z) > " + num(spec.tiltMin) + ";\n";
        if (streets)
            h += "float TopSlab = (SK == 2 || (SK == 1 && Cambered) || (SK == 0 && SizeSlab)) ? 1.0 : 0.0;\n";
        else
            h += "float TopSlab = SizeSlab ? 1.0 : 0.0;\n";
        h += "float TopEdge = Cambered ? GutD : 0.5 * min(Sx, Sy) - abs(Sx >= Sy ? Lm.y : Lm.x);\n";
        h += "float TopDirt = saturate(TopSlab * smoothstep(0.7, 0.9, VN.z) * " + num(spec.topGrimeStrength) +
             " * (1.0 - smoothstep(0.0, " + num(spec.topGrimeM) + ", TopEdge)) * lerp(" + num(kTopNoiseFloor) +
             ", 1.0, Noise[2]));\n";
        h += "C *= lerp(float3(1.0, 1.0, 1.0), " + tint + ", TopDirt);\n";
        h += "A *= 1.0 - " + num(kTopOcclusion) + " * TopDirt;\n";
    }
    h += "NormalOut = normalize(lerp(Nrm, VN, saturate(" + num(spec.soakFlatten) + " * Soak + PudCore)));\n";
    h += "RoughOut = saturate(R);\n";
    h += "AOOut = A;\n";
    // Dry asphalt's own specular (lower than the default 0.5); a wet road's sheen is its roughness, not this.
    if (options.specOut)
        h += streets ? "SpecOut = SK == 1 ? " + num(street.asphaltSpecular) + " : 0.5;\n" : std::string("SpecOut = 0.5;\n");
    h += "return max(C, 0.02);\n";
    return h;
}

std::string surfaceWetHlsl(const SurfaceWeatherSpec& spec) {
    std::string h;
    h.reserve(2048);
    noiseLoop(h, spec, 2);
    h += "float WSoak = WetIn * lerp(" + num(spec.dampShare) + ", 1.0, smoothstep(" + num(kPatchFrom) + ", " + num(kPatchTo) +
         ", Noise[1]));\n";
    puddleCellHlsl(h, spec, "WPd");
    h += "float WPud = saturate(WetIn * " + num(kWetRamp) + ") * smoothstep(0.0, " + num(spec.puddleEdge) + ", " +
         puddleFillHlsl(spec, "WPdBowl", "GutIn", "", "WetIn") + ");\n";
    h += "C *= lerp(1.0, " + num(spec.soakDarken) + ", WSoak) * lerp(1.0, " + num(spec.puddleDarken) + ", WPud);\n";
    h += "R = min(R, lerp(R, lerp(" + num(spec.soakRough) + ", R, " + num(spec.soakKeep) + "), pow(saturate(WSoak), " +
         num(spec.roughCurve) + ")));\n";
    h += "R = lerp(R, " + num(spec.puddleRough) + ", smoothstep(" + num(spec.puddleCoreFrom) + ", 1.0, WPud));\n";
    return h;
}

}  // namespace ftcore
