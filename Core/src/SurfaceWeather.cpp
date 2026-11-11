#include "ftcore/SurfaceWeather.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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

// The street layer's body (inside the node, after the box frame): reads Street, Pw, Foot, VN, Lm, Sx, Sy, Sz, Ux, Uy,
// Uy.z, GutD; changes Alb, Rgh and Nrm.
void streetHlsl(std::string& h, const SurfaceWeatherSpec& spec, const StreetSurfaceSpec& st) {
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
    h += "\t\tSlopeW += float2(Ax - A0, Ay - A0) / 0.35 * " + num(st.aggregateSlope) + " * Fine;\n";
    // Exposed stone chips: light flecks where the binder wore off (rougher), fading to their mean further out.
    h += "\t\tfloat Chip = smoothstep(0.62, 0.8, 0.6 * A0 + 0.4 * SNz.N(Pw / " + num(st.aggregateM * 0.45f) + " + 5.0));\n";
    h += "\t\tTone *= 1.0 + 0.3 * (A0 - 0.5) * Fine + lerp(0.25, 1.4 * Chip, Fine) * 0.5;\n";
    h += "\t\tRoughMul *= 1.0 + 0.1 * Chip * Fine;\n";
    // Ravelled blotches and old patches, 0.3-2 m: what is left of the grain beyond a few metres.
    h += "\t\tfloat Blot = SNz.N(Pw / 0.6 + 11.0);\n";
    h += "\t\tTone *= 1.0 + 0.22 * (Blot - 0.5) * Mid;\n";
    h += "\t\tfloat Fix = smoothstep(0.7, 0.72, SNz.N(Pw / 5.0 + 61.0));\n";
    h += "\t\tTone *= lerp(1.0, 0.72, Fix);\n";
    h += "\t\tRoughMul *= lerp(1.0, 0.92, Fix);\n";
    // Wheel paths: a cambered half from its gutter, a long flat slab across its short side (none on a squarish fill).
    h += "\t\tfloat Wd = Tilted ? Sy : (max(Sx, Sy) > 2.0 * min(Sx, Sy) ? min(Sx, Sy) : 0.0);\n";
    h += "\t\tfloat Across = Tilted ? GutD : (Sx >= Sy ? Lm.y : Lm.x) + 0.5 * Wd;\n";
    h += "\t\tfloat Lanes = max(1.0, round(Wd / " + num(st.laneM) + "));\n";
    h += "\t\tfloat Lw = Wd / Lanes;\n";
    h += "\t\tfloat Q = (frac(Across / max(Lw, 0.01)) - 0.5) * Lw;\n";
    h += "\t\tfloat Wheel = Lw > 2.0 ? exp(-pow((abs(Q) - " + num(st.wheelOffM) + ") / " + num(st.wheelWideM) + ", 2.0)) : 0.0;\n";
    h += "\t\tfloat Centre = Lw > 2.0 ? exp(-pow(Q / 0.35, 2.0)) : 0.0;\n";
    h += "\t\tfloat Oil = Centre * smoothstep(0.5, 0.8, SNz.N(Pw * 0.9 + 23.0));\n";
    h += "\t\tTone *= (1.0 - " + num(st.wheelDarken) + " * 1.6 * Wheel) * (1.0 + " + num(st.wornCentre) + " * Centre) * (1.0 - 0.3 * Oil);\n";
    h += "\t\tRoughMul *= (1.0 - " + num(st.wheelPolish) + " * Wheel) * (1.0 - 0.3 * Oil);\n";
    h += "\t\tif (Tilted)\n\t\t{\n";
    h += "\t\t\tfloat EdgeBand = smoothstep(0.3, 0.45, GutD) * (1.0 - smoothstep(0.8, 1.0, GutD)) + (1.0 - smoothstep(0.0, 0.4, Sy - GutD));\n";
    h += "\t\t\tTone *= 1.0 + " + num(st.wornEdge) + " * EdgeBand;\n";
    h += "\t\t\tRoughMul *= 1.0 - " + num(st.gutterDampPolish) + " * (1.0 - smoothstep(" + num(st.gutterDampM * 0.6f) + ", " +
         num(st.gutterDampM) + ", GutD));\n";
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
    h += "\t\tfloat CrackGate = smoothstep(0.5, 0.62, SNz.N(Pw / " + num(st.crackScaleM * 3.f) + " + 3.1));\n";
    h += "\t\tfloat CrackF = Foot / " + num(st.crackScaleM) + ";\n";
    h += "\t\tfloat Crack = (1.0 - smoothstep(0.006, 0.006 + CrackF, abs(CrackV - 0.5))) * saturate(0.012 / (0.012 + CrackF)) * CrackGate;\n";
    h += "\t\tfloat HairV = SNz.N(Pw / 0.9 + 77.0);\n";
    h += "\t\tfloat HairF = Foot / 0.9;\n";
    h += "\t\tfloat Hair = (1.0 - smoothstep(0.004, 0.004 + HairF, abs(HairV - 0.5))) * saturate(0.008 / (0.008 + HairF)) *\n";
    h += "\t\t\tsmoothstep(0.55, 0.7, SNz.N(Pw / 7.0 + 9.0));\n";
    h += "\t\tfloat Seal = max(Joint, Crack);\n";
    h += "\t\tTone *= lerp(1.0, " + num(st.sealDarken) + ", Seal) * (1.0 - 0.6 * Hair);\n";
    h += "\t\tRoughMul *= lerp(1.0, 0.75, Seal) * (1.0 + 0.2 * Hair);\n";
    h += "\t}\n";
    // Footways: grey concrete pavers in running bond along the run, sanded joints, per-paver tone, grime and gum, and
    // pavers that soak unevenly (the wet gloss breaks up paver by paver).
    h += "\telse if (SK == 5)\n\t{\n";
    h += "\t\tbool LongX = Sx >= Sy;\n";
    h += "\t\tfloat2 PL = LongX ? Lm.xy + 0.5 * float2(Sx, Sy) : float2(Lm.y + 0.5 * Sy, Lm.x + 0.5 * Sx);\n";
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
    h += "\t\tSlopeB += LongX ? PSl : PSl.yx;\n";
    h += "\t\tfloat Ph = frac(sin(dot(PId + float2(round(Sx * 7.0), round(Sy * 13.0)), float2(12.9898, 78.233))) * 43758.5453);\n";
    h += "\t\tfloat PJ = lerp(0.2, 1.0 - PH, Mid);\n";  // joint share: 1 in a joint, its mean further out
    h += "\t\tTone *= lerp(1.0, 0.5, PJ) * (1.0 + 0.12 * (2.0 * Ph - 1.0) * Mid);\n";
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
    h += "\t\tfloat2 T = (CornerP ? Pw : Lm.xy + 0.5 * float2(Sx, Sy)) / " + num(st.tileM) + ";\n";
    h += "\t\tfloat2 Id = floor(T);\n";
    h += "\t\tfloat2 TF = frac(T) - 0.5;\n";
    h += "\t\tfloat2 Dd = (0.5 - abs(TF)) * " + num(st.tileM) + ";\n";
    h += "\t\tbool NearX = Dd.x < Dd.y;\n";
    h += "\t\tfloat TD = min(Dd.x, Dd.y);\n";
    h += "\t\tfloat Tt = saturate((TD - " + num(st.tileJointM) + ") / " + num(st.tileBevelM) + ");\n";
    h += "\t\tfloat Ht = Tt * Tt * (3.0 - 2.0 * Tt);\n";
    h += "\t\tfloat Dh = 6.0 * Tt * (1.0 - Tt) / " + num(st.tileBevelM) + ";\n";
    h += "\t\tfloat2 TSl = (NearX ? float2(-sign(TF.x), 0.0) : float2(0.0, -sign(TF.y))) * Dh * " + num(st.tileDepthM) + " * Fine;\n";
    h += "\t\tif (CornerP) { SlopeW += TSl; } else { SlopeB += TSl; }\n";
    h += "\t\tfloat TJ = lerp(0.12, 1.0 - Ht, Mid);\n";  // joint share, its mean further out
    h += "\t\tTone *= " + num(kPaveAlbedo) + " * lerp(1.0, 0.45, TJ);\n";
    h += "\t\tfloat2 IdB = CornerP ? float2(0.0, 0.0) : float2(round(Sx * 7.0), round(Sy * 13.0));\n";
    h += "\t\tfloat Th = frac(sin(dot(Id + IdB, float2(12.9898, 78.233))) * 43758.5453);\n";
    h += "\t\tTone *= 1.0 + " + num(st.tileTone) + " * 1.5 * (2.0 * Th - 1.0) * Mid;\n";
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
    h += "\t\tfloat KL = (LongX ? Lm.x + 0.5 * Sx : Lm.y + 0.5 * Sy) / " + num(st.kerbStoneM) + ";\n";
    h += "\t\tfloat KF = frac(KL) - 0.5;\n";
    h += "\t\tfloat KD = (0.5 - abs(KF)) * " + num(st.kerbStoneM) + ";\n";
    h += "\t\tfloat Kt = saturate((KD - " + num(st.tileJointM) + ") / " + num(st.tileBevelM) + ");\n";
    h += "\t\tfloat KH = Kt * Kt * (3.0 - 2.0 * Kt);\n";
    h += "\t\tfloat KDh = 6.0 * Kt * (1.0 - Kt) / " + num(st.tileBevelM) + ";\n";
    h += "\t\tSlopeB += (LongX ? float2(-sign(KF), 0.0) : float2(0.0, -sign(KF))) * KDh * 0.006 * Fine;\n";
    h += "\t\tFaceW = 1.0;\n";
    h += "\t\tTone *= lerp(1.0, lerp(0.6, 1.0, KH), Fine);\n";
    h += "\t\tfloat Kh = frac(sin(floor(KL) * 12.9898 + round(Sx * 7.0 + Sy * 3.0) * 4.1) * 43758.5453);\n";
    h += "\t\tTone *= 1.0 + 0.07 * (2.0 * Kh - 1.0);\n";
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
    h += "\t\tAlb = lerp(float3(0.045, 0.045, 0.05), Paint, Left);\n";
    h += "\t\tRgh = lerp(0.85, Rgh, Left);\n";
    h += "\t}\n";
    h += "\tAlb *= Tone;\n";
    h += "\tRgh = saturate(Rgh * RoughMul);\n";
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

float weatherPuddle(float wetness, float up, float noise, float gutter, const SurfaceWeatherSpec& spec) {
    const float wet = saturate(wetness);
    return up * saturate(wet * kWetRamp) *
           smoothstep(0.f, spec.puddleEdge, noise + spec.gutterBias * gutter - (1.f - spec.puddleCover * wet));
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
    const float r = std::min(rough, lerp(rough, spec.soakRough, std::pow(saturate(soak), spec.roughCurve)));
    return saturate(lerp(r, spec.puddleRough, puddle));
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

std::string surfaceWeatherHlsl(const SurfaceWeatherSpec& spec, const SurfaceWeatherOptions& options,
                               const StreetSurfaceSpec& street) {
    const bool box = options.boxFrame;
    const bool dirt = box && options.grime;
    const bool top = box && options.topGrime;
    const bool streets = box && options.street;
    std::string h;
    h.reserve(16384);
    if (streets) {
        // A 2D value noise for the street layer (the lattice hash the decals use).
        h += "struct FTStreetNoise\n{\n";
        h += "\tfloat H(float2 p) { p = frac(p * float2(0.1031, 0.1030)); p += dot(p, p.yx + 33.33); return frac((p.x + p.y) * p.x); }\n";
        h += "\tfloat N(float2 p)\n\t{\n";
        h += "\t\tfloat2 i = floor(p);\n";
        h += "\t\tfloat2 f = frac(p);\n";
        h += "\t\tfloat2 u = f * f * (3.0 - 2.0 * f);\n";
        h += "\t\treturn lerp(lerp(H(i), H(i + float2(1.0, 0.0)), u.x), lerp(H(i + float2(0.0, 1.0)), H(i + float2(1.0, 1.0)), u.x), u.y);\n";
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
    if (streets) streetHlsl(h, spec, street);
    h += "float Pud = Up * saturate(Wet * " + num(kWetRamp) + ") * smoothstep(0.0, " + num(spec.puddleEdge) + ", Noise[0] + " +
         num(spec.gutterBias) + " * Gut - (1.0 - " + num(spec.puddleCover) + " * Wet));\n";
    h += "float3 C = Alb * (lerp(1.0, " + num(spec.soakDarken) + ", Soak) * lerp(1.0, " + num(spec.puddleDarken) +
         ", Pud));\n";
    h += "float R = min(Rgh, lerp(Rgh, " + num(spec.soakRough) + ", pow(saturate(Soak), " + num(spec.roughCurve) + ")));\n";
    h += "R = lerp(R, " + num(spec.puddleRough) + ", Pud);\n";
    h += "float A = AO;\n";
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
    h += "NormalOut = normalize(lerp(Nrm, VN, saturate(" + num(spec.soakFlatten) + " * Soak + Pud)));\n";
    h += "RoughOut = saturate(R);\n";
    h += "AOOut = A;\n";
    h += "return max(C, 0.02);\n";
    return h;
}

std::string surfaceWetHlsl(const SurfaceWeatherSpec& spec) {
    std::string h;
    h.reserve(2048);
    noiseLoop(h, spec, 2);
    h += "float WSoak = WetIn * lerp(" + num(spec.dampShare) + ", 1.0, smoothstep(" + num(kPatchFrom) + ", " + num(kPatchTo) +
         ", Noise[1]));\n";
    h += "float WPud = saturate(WetIn * " + num(kWetRamp) + ") * smoothstep(0.0, " + num(spec.puddleEdge) + ", Noise[0] + " +
         num(spec.gutterBias) + " * GutIn - (1.0 - " + num(spec.puddleCover) + " * WetIn));\n";
    h += "C *= lerp(1.0, " + num(spec.soakDarken) + ", WSoak) * lerp(1.0, " + num(spec.puddleDarken) + ", WPud);\n";
    h += "R = min(R, lerp(R, " + num(spec.soakRough) + ", pow(saturate(WSoak), " + num(spec.roughCurve) + ")));\n";
    h += "R = lerp(R, " + num(spec.puddleRough) + ", WPud);\n";
    return h;
}

}  // namespace ftcore
