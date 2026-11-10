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

float saturate(float x) { return std::clamp(x, 0.f, 1.f); }
float lerp(float a, float b, float t) { return a + (b - a) * t; }
// HLSL's smoothstep.
float smoothstep(float a, float b, float x) {
    const float t = saturate((x - a) / (b - a));
    return t * t * (3.f - 2.f * t);
}

std::string num(float f) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(f));
    std::string s(buf);
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
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
    const float grime = spec.grimeStrength * low * low * lerp(kGrimeNoiseFloor, 1.f, noise);
    const float edge = spec.edgeStrength * (1.f - smoothstep(0.f, spec.edgeWidthM, edgeM));
    return saturate(side * thick * (grime + edge));
}

float weatherRough(float rough, float soak, float puddle, const SurfaceWeatherSpec& spec) {
    const float r = std::min(rough, lerp(rough, spec.soakRough, std::pow(saturate(soak), spec.roughCurve)));
    return saturate(lerp(r, spec.puddleRough, puddle));
}

float weatherAlbedoScale(int channel, float soak, float puddle, float dirt, const SurfaceWeatherSpec& spec) {
    const float tint = channel == 0 ? spec.dirtR : (channel == 1 ? spec.dirtG : spec.dirtB);
    return lerp(1.f, spec.soakDarken, soak) * lerp(1.f, spec.puddleDarken, puddle) * lerp(1.f, tint, dirt);
}

std::string surfaceWeatherHlsl(const SurfaceWeatherSpec& spec, const SurfaceWeatherOptions& options) {
    const bool box = options.boxFrame;
    const bool dirt = box && options.grime;
    std::string h;
    h.reserve(4096);
    h += "float3 VN = normalize(VertexNormal);\n";
    h += "float Up = smoothstep(" + num(spec.upFrom) + ", " + num(spec.upTo) + ", VN.z);\n";
    h += "float Wet = saturate(Wetness) * saturate(Amount);\n";
    // Value noise on the lattice hash the surface masters use: 0 puddles and 1 drying patches on the plan, 2 grime in 3D.
    const int channels = dirt ? 3 : 2;
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
    h += "float Pud = Up * saturate(Wet * " + num(kWetRamp) + ") * smoothstep(0.0, " + num(spec.puddleEdge) + ", Noise[0] + " +
         num(spec.gutterBias) + " * Gut - (1.0 - " + num(spec.puddleCover) + " * Wet));\n";
    h += "float3 C = Albedo * (lerp(1.0, " + num(spec.soakDarken) + ", Soak) * lerp(1.0, " + num(spec.puddleDarken) +
         ", Pud));\n";
    h += "float R = min(Rough, lerp(Rough, " + num(spec.soakRough) + ", pow(saturate(Soak), " + num(spec.roughCurve) +
         ")));\n";
    h += "R = lerp(R, " + num(spec.puddleRough) + ", Pud);\n";
    h += "float A = AO;\n";
    if (dirt) {
        h += "float Side = 1.0 - smoothstep(" + num(kSideFrom) + ", " + num(kSideTo) + ", abs(VN.z));\n";
        h += "float Thick = smoothstep(" + num(spec.grimeThinM) + ", " + num(spec.grimeThinM * 1.5f) + ", min(Sx, Sy));\n";
        h += std::string("float Height = ") + (options.aboveInput ? "Above" : "Lm.z + 0.5 * Sz") + ";\n";
        h += "float Low = 1.0 - smoothstep(0.0, " + num(spec.grimeHeightM) + ", Height);\n";
        h += "float Grime = " + num(spec.grimeStrength) + " * Low * Low * lerp(" + num(kGrimeNoiseFloor) + ", 1.0, Noise[2]);\n";
        // A side face's distance to its nearer vertical edge: across Y on an X face, across X on a Y face.
        h += "float EdgeD = abs(dot(VN, Ux)) >= abs(dot(VN, Uy)) ? 0.5 * Sy - abs(Lm.y) : 0.5 * Sx - abs(Lm.x);\n";
        h += "float Edge = " + num(spec.edgeStrength) + " * (1.0 - smoothstep(0.0, " + num(spec.edgeWidthM) + ", EdgeD));\n";
        h += "float Dirt = saturate(Side * Thick * (Grime + Edge));\n";
        h += "C *= lerp(float3(1.0, 1.0, 1.0), float3(" + num(spec.dirtR) + ", " + num(spec.dirtG) + ", " + num(spec.dirtB) +
             "), Dirt);\n";
        h += "A *= 1.0 - " + num(spec.dirtOcclusion) + " * Dirt;\n";
    }
    h += "NormalOut = normalize(lerp(Normal, VN, saturate(" + num(spec.soakFlatten) + " * Soak + Pud)));\n";
    h += "RoughOut = saturate(R);\n";
    h += "AOOut = A;\n";
    h += "return max(C, 0.02);\n";
    return h;
}

}  // namespace ftcore
