#include "ftcore/SurfaceJoints.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace ftcore {

namespace {

constexpr int kUnitSalt = 7;                 // the per-unit hash's third lattice coordinate
constexpr float kUnevenOffset[3] = {31.7f, -12.9f, 7.3f}; // keeps the uneven noise off Mottle's lattice
constexpr float kLum[3] = {0.2126f, 0.7152f, 0.0722f};
constexpr float kAboveTexels = 3.f, kRingTexels = 6.f, kSlopeTexels = 1.5f;

std::string jointNum(float f) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(f));
    std::string s(buf);
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

float jointSaturate(float x) { return std::clamp(x, 0.f, 1.f); }

float jointSmoothstep(float lo, float hi, float x) {
    const float t = jointSaturate((x - lo) / std::max(hi - lo, 1e-4f));
    return t * t * (3.f - 2.f * t);
}

float jointLerp(float a, float b, float t) { return a + (b - a) * t; }

int clampedMax(const SurfaceJointSpec& spec) { return std::max(spec.maxSteps, 1); }
int clampedMin(const SurfaceJointSpec& spec) { return std::clamp(spec.minSteps, 1, clampedMax(spec)); }

// The lattice hash Mottle uses in kSurfaceCode, on three integer coordinates; a value 0..1.
float latticeValue(int x, int y, int z) {
    std::uint32_t h = (static_cast<std::uint32_t>(x) * 73856093u) ^ (static_cast<std::uint32_t>(y) * 19349663u) ^
                      (static_cast<std::uint32_t>(z) * 83492791u);
    h ^= h >> 13;
    h *= 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFu) / 65535.f;
}

int floorInt(float x) { return static_cast<int>(std::floor(x)); }

// The same hash in HLSL: "float Dst = value at (X, Y, Z)", X, Y, Z int expressions.
std::string latticeHlsl(const std::string& indent, const std::string& dst, const std::string& x, const std::string& y,
                        const std::string& z) {
    const std::string hh = dst + "Hash";
    return indent + "uint " + hh + " = (uint(" + x + ") * 73856093u) ^ (uint(" + y + ") * 19349663u) ^ (uint(" + z +
           ") * 83492791u);\n" + indent + hh + " ^= " + hh + " >> 13;\n" + indent + hh + " *= 1274126177u;\n" +
           indent + hh + " ^= " + hh + " >> 16;\n" + indent + "float " + dst + " = float(" + hh +
           " & 0xFFFFu) / 65535.0;\n";
}

// The HLSL for "Dst = height of the ARM sample Src" in two statements (a Custom node has no functions of its own).
std::string heightHlsl(const std::string& indent, const std::string& dst, const std::string& src) {
    return indent + "float " + dst + " = saturate((" + src + ".r - JointLo) / max(JointHi - JointLo, 1e-4));\n" +
           indent + dst + " = " + dst + " * " + dst + " * (3.0 - 2.0 * " + dst + ");\n";
}

// One course's tone along u (cu in unit cells, less half a cell), with its half-unit bond offset on odd courses; the
// HLSL twin writes "float Dst" from the row expression.
float unitRow(const SurfaceJointSpec& spec, float cu, int row, bool bond) {
    const float shift = bond ? 0.5f * static_cast<float>(row & 1) : 0.f;
    const float x = cu - shift;
    const int i = floorInt(x);
    const float w = jointSmoothstep(0.5f - 0.5f * spec.unitEdge, 0.5f + 0.5f * spec.unitEdge, x - std::floor(x));
    return jointLerp(latticeValue(i, row, kUnitSalt), latticeValue(i + 1, row, kUnitSalt), w);
}

std::string unitRowHlsl(const SurfaceJointSpec& spec, const std::string& dst, const std::string& row) {
    const std::string in = "\t\t", lo = jointNum(0.5f - 0.5f * spec.unitEdge), hi = jointNum(0.5f + 0.5f * spec.unitEdge);
    std::string h;
    h += in + "float " + dst + "X = JtCell.x - JointBond * 0.5 * (" + row + " - 2.0 * floor(" + row + " * 0.5));\n";
    h += in + "float " + dst + "I = floor(" + dst + "X);\n";
    h += latticeHlsl(in, dst + "A", "int(" + dst + "I)", "int(" + row + ")", std::to_string(kUnitSalt));
    h += latticeHlsl(in, dst + "B", "int(" + dst + "I) + 1", "int(" + row + ")", std::to_string(kUnitSalt));
    h += in + "float " + dst + " = lerp(" + dst + "A, " + dst + "B, smoothstep(" + lo + ", " + hi + ", " + dst + "X - " +
         dst + "I));\n";
    return h;
}

} // namespace

float surfaceJointHeight(float ao, float lo, float hi) { return jointSmoothstep(lo, hi, ao); }

float surfaceJointParallaxWeight(const SurfaceJointSpec& spec, float distanceM, float nDotV, float jointDepthSwitch) {
    return jointSaturate(jointDepthSwitch) * (1.f - jointSmoothstep(spec.fadeNearM, spec.fadeFarM, distanceM)) *
           jointSmoothstep(spec.grazeOff, spec.grazeFull, nDotV);
}

int surfaceJointSteps(const SurfaceJointSpec& spec, float nDotV) {
    const float t = jointSaturate((nDotV - spec.grazeFull) / std::max(1.f - spec.grazeFull, 1e-4f));
    const float hi = static_cast<float>(clampedMax(spec)), lo = static_cast<float>(clampedMin(spec));
    return static_cast<int>(std::floor(hi + (lo - hi) * t + 0.5f));
}

float surfaceJointLod(const SurfaceJointSpec& spec, float distanceCm, float tileCm) {
    const float perPixel = distanceCm * spec.radPerPixel * spec.texels / std::max(tileCm, 1.f);
    return std::max(0.f, std::log2(std::max(perPixel, 1e-6f)));
}

float surfaceJointShadeFade(const SurfaceJointSpec& spec, float lod) {
    return 1.f - jointSmoothstep(spec.shadeLodFull, spec.shadeLodOff, lod);
}

float surfaceJointUneven(const SurfaceJointSpec& spec, float xCm, float yCm, float zCm) {
    const float scale = std::max(spec.unevenScaleCm, 1.f);
    const float p[3] = {xCm / scale + kUnevenOffset[0], yCm / scale + kUnevenOffset[1], zCm / scale + kUnevenOffset[2]};
    int c[3];
    float f[3];
    for (int k = 0; k < 3; ++k) {
        const float cell = std::floor(p[k]);
        c[k] = static_cast<int>(cell);
        const float t = p[k] - cell;
        f[k] = t * t * (3.f - 2.f * t);
    }
    float n[8];
    for (int k = 0; k < 8; ++k) n[k] = latticeValue(c[0] + (k & 1), c[1] + ((k >> 1) & 1), c[2] + (k >> 2));
    const float lo = jointLerp(jointLerp(n[0], n[1], f[0]), jointLerp(n[2], n[3], f[0]), f[1]);
    const float hi = jointLerp(jointLerp(n[4], n[5], f[0]), jointLerp(n[6], n[7], f[0]), f[1]);
    return jointLerp(1.f - spec.uneven, 1.f, jointLerp(lo, hi, f[2]));
}

float surfaceJointCavity(const SurfaceJointSpec& spec, float h0, float above, float below, float ring, float depthCm,
                         float nDotV) {
    const float floorK = 1.f - h0;
    const float upper = jointSaturate(above - below), lower = jointSaturate(below - above);
    const float wide = jointSaturate(ring - h0);
    const float deep = jointSaturate(depthCm / std::max(spec.cavityRefCm, 1e-3f));
    const float view = 1.f + spec.viewDark * (1.f - jointSaturate(nDotV));
    return jointSaturate(
        (floorK * (spec.cavityBase + spec.upperDark * upper - spec.skyLift * lower) + spec.wideAO * wide) * deep * view);
}

float surfaceJointUnitTone(const SurfaceJointSpec& spec, float u, float v, float unitsU, float unitsV, bool bond) {
    if (!(unitsU > 0.f) || !(unitsV > 0.f)) return 0.5f;
    const float cu = u * unitsU - 0.5f, cv = v * unitsV - 0.5f;
    const int row = floorInt(cv);
    const float w = jointSmoothstep(0.5f - 0.5f * spec.unitEdge, 0.5f + 0.5f * spec.unitEdge, cv - std::floor(cv));
    return jointLerp(unitRow(spec, cu, row, bond), unitRow(spec, cu, row + 1, bond), w);
}

SurfaceJointRgb surfaceJointMortar(const SurfaceJointSpec& spec, float tintR, float tintG, float tintB) {
    const float lum = kLum[0] * tintR + kLum[1] * tintG + kLum[2] * tintB;
    return {spec.mortarR * lum, spec.mortarG * lum, spec.mortarB * lum};
}

SurfaceJointShift surfaceJointMarch(const SurfaceJointSpec& spec, SurfaceJointHeightFn height, const void* user,
                                    float u, float v, float viewU, float viewV, float viewN, float depthTile,
                                    float weight) {
    SurfaceJointShift out;
    if (!(weight > 0.f) || height == nullptr) return out;
    // The run: how far the ray goes sideways (in UV) per unit of depth, held to maxSlope.
    float runU = -viewU / std::max(viewN, 1e-4f), runV = -viewV / std::max(viewN, 1e-4f);
    const float len = std::sqrt(runU * runU + runV * runV);
    const float hold = std::min(1.f, spec.maxSlope / std::max(len, 1e-6f));
    runU *= hold;
    runV *= hold;
    // The shift where the ray meets the floor (height 0).
    const float floorU = runU * depthTile * weight, floorV = runV * depthTile * weight;
    const int steps = surfaceJointSteps(spec, viewN);
    const float layer = 1.f / static_cast<float>(steps);
    float prevU = 0.f, prevV = 0.f, prevGap = -1.f;
    for (int i = 0; i <= steps; ++i) {
        const float ray = 1.f - static_cast<float>(i) * layer;
        const float atU = floorU * (1.f - ray), atV = floorV * (1.f - ray);
        const float gap = height(u + atU, v + atV, user) - ray;
        if (gap >= 0.f || i == steps) {
            if (i == 0) return out;
            const float t = gap >= 0.f ? prevGap / (prevGap - gap) : 1.f;
            out.du = prevU + (atU - prevU) * t;
            out.dv = prevV + (atV - prevV) * t;
            return out;
        }
        prevU = atU;
        prevV = atV;
        prevGap = gap;
    }
    return out;
}

SurfaceJointSurface surfaceJointFor(std::string_view surfaceId) noexcept {
    struct Entry {
        const char* key;
        float depthCm, unitsU, unitsV;
        bool bond;
    };
    static constexpr Entry kEntries[] = {
        {"soot_brick", 1.2f, 4.f, 13.f, true},      {"breeze_block", 0.8f, 3.f, 5.f, false},
        {"ceramic_tile", 0.3f, 8.f, 8.f, false},    {"paving_slab", 0.65f, 4.f, 4.f, false},
        {"tactile_paving", 0.5f, 4.f, 4.f, false},  {"board_concrete", 0.2f, 0.f, 0.f, false},
    };
    for (const Entry& e : kEntries)
        if (surfaceId.find(e.key) != std::string_view::npos) return {e.depthCm, 0.6f, 0.92f, e.unitsU, e.unitsV, e.bond};
    return {0.f, 0.6f, 0.92f, 0.f, 0.f, false};
}

std::string surfaceJointParallaxHlsl(const SurfaceJointSpec& spec) {
    const int maxSteps = clampedMax(spec), minSteps = clampedMin(spec);
    std::string h;
    h += "// Joint depth (ftcore/SurfaceJoints.h): the height is the AO channel's joint mask. The mip comes from the camera\n";
    h += "// distance, never from derivatives (the ray-tracing hit shader has none).\n";
    h += "float JtDistCm = length(ViewVec);\n";
    h += "float3 JtV = ViewVec / max(JtDistCm, 1e-4);\n";
    h += "float JtNdotV = dot(Up, JtV);\n";
    h += "float JtLod = max(0.0, log2(max(JtDistCm * " + jointNum(spec.radPerPixel * spec.texels) +
         " / max(TileCm, 1.0), 1e-6)));\n";
    h += "// Some courses sit shallower than others: a value noise on the world position, cell " +
         jointNum(spec.unevenScaleCm) + " cm.\n";
    h += "float JtDeep = 1.0;\n";
    h += "if (JointDepth > 0.0 && JointDepthCm > 0.0)\n{\n";
    h += "\tfloat3 JtP = WorldPos / " + jointNum(std::max(spec.unevenScaleCm, 1.f)) + " + float3(" +
         jointNum(kUnevenOffset[0]) + ", " + jointNum(kUnevenOffset[1]) + ", " + jointNum(kUnevenOffset[2]) + ");\n";
    h += "\tfloat3 JtCellP = floor(JtP);\n";
    h += "\tfloat3 JtF = JtP - JtCellP;\n";
    h += "\tJtF = JtF * JtF * (3.0 - 2.0 * JtF);\n";
    h += "\tint3 JtC = int3(JtCellP);\n";
    h += "\tfloat JtN[8];\n";
    h += "\tfor (int JtK = 0; JtK < 8; ++JtK)\n\t{\n";
    h += latticeHlsl("\t\t", "JtNk", "JtC.x + (JtK & 1)", "JtC.y + ((JtK >> 1) & 1)", "JtC.z + (JtK >> 2)");
    h += "\t\tJtN[JtK] = JtNk;\n\t}\n";
    h += "\tfloat JtNoise = lerp(lerp(lerp(JtN[0], JtN[1], JtF.x), lerp(JtN[2], JtN[3], JtF.x), JtF.y),\n";
    h += "\t\tlerp(lerp(JtN[4], JtN[5], JtF.x), lerp(JtN[6], JtN[7], JtF.x), JtF.y), JtF.z);\n";
    h += "\tJtDeep = lerp(" + jointNum(1.f - spec.uneven) + ", 1.0, JtNoise);\n";
    h += "}\n";
    h += "// The dominant projection and its frame: u runs along JtTu, v (image-down) along JtTv, the face normal is Up.\n";
    h += "int JtAxis = (Blend.x >= Blend.y && Blend.x >= Blend.z) ? 0 : (Blend.y >= Blend.z ? 1 : 2);\n";
    h += "float2 JtUv = UvZ;\n";
    h += "float3 JtTu = AxX;\n";
    h += "float3 JtTv = TopDown * AxY;\n";
    h += "if (JtAxis == 0) { JtUv = UvX; JtTu = AxY; JtTv = SideDown * AxZ; }\n";
    h += "else if (JtAxis == 1) { JtUv = UvY; JtTu = AxX; JtTv = SideDown * AxZ; }\n";
    h += "// Off far away, on glancing views, where the projections blend (box edges) and on surfaces without joints.\n";
    h += "float JointPar = saturate(JointDepth) * (1.0 - smoothstep(" + jointNum(spec.fadeNearM) + ", " +
         jointNum(spec.fadeFarM) + ", JtDistCm * 0.01)) * smoothstep(" + jointNum(spec.grazeOff) + ", " +
         jointNum(spec.grazeFull) + ", JtNdotV)\n";
    h += "\t* smoothstep(0.6, 0.9, max(Blend.x, max(Blend.y, Blend.z))) * (JointDepthCm > 0.0 ? 1.0 : 0.0);\n";
    h += "if (JointPar > 0.0)\n{\n";
    h += "\t// The sideways run per unit of depth, held to " + jointNum(spec.maxSlope) +
         ", and the shift where the ray meets the joint's floor.\n";
    h += "\tfloat2 JtRun = -float2(dot(JtV, JtTu), dot(JtV, JtTv)) / max(JtNdotV, 1e-4);\n";
    h += "\tJtRun *= min(1.0, " + jointNum(spec.maxSlope) + " / max(length(JtRun), 1e-6));\n";
    h += "\tfloat2 JtFloor = JtRun * (JointDepthCm / max(TileCm, 1.0) * JtDeep * JointPar);\n";
    h += "\tint JtSteps = (int)(lerp(" + jointNum(static_cast<float>(maxSteps)) + ", " +
         jointNum(static_cast<float>(minSteps)) + ", saturate((JtNdotV - " + jointNum(spec.grazeFull) + ") / " +
         jointNum(std::max(1.f - spec.grazeFull, 1e-4f)) + ")) + 0.5);\n";
    h += "\tfloat JtLayer = 1.0 / float(JtSteps);\n";
    h += "\tfloat2 JtHit = JtUv + JtFloor;\n";
    h += "\tfloat2 JtPrev = JtUv;\n";
    h += "\tfloat JtPrevGap = -1.0;\n";
    h += "\t[loop] for (int JtI = 0; JtI <= " + std::to_string(maxSteps) + "; ++JtI)\n\t{\n";
    h += "\t\tfloat JtRay = 1.0 - float(JtI) * JtLayer;\n";
    h += "\t\tfloat2 JtAt = JtUv + JtFloor * (1.0 - JtRay);\n";
    h += heightHlsl("\t\t", "JtH", "Texture2DSampleLevel(ArmTex, ArmTexSampler, JtAt, JtLod)");
    h += "\t\tfloat JtGap = JtH - JtRay;\n";
    h += "\t\tif (JtGap >= 0.0 || JtI >= JtSteps)\n\t\t{\n";
    h += "\t\t\tJtHit = JtI == 0 ? JtUv : lerp(JtPrev, JtAt, JtGap >= 0.0 ? JtPrevGap / (JtPrevGap - JtGap) : 1.0);\n";
    h += "\t\t\tbreak;\n\t\t}\n";
    h += "\t\tJtPrev = JtAt;\n";
    h += "\t\tJtPrevGap = JtGap;\n";
    h += "\t}\n";
    h += "\tJtUv = JtHit;\n";
    h += "\tif (JtAxis == 0) UvX = JtHit;\n";
    h += "\telse if (JtAxis == 1) UvY = JtHit;\n";
    h += "\telse UvZ = JtHit;\n";
    h += "}\n";
    return h;
}

std::string surfaceJointShadeHlsl(const SurfaceJointSpec& spec) {
    const float texel = 1.f / std::max(spec.texels, 1.f);
    const std::string slope = jointNum(kSlopeTexels * texel), above = jointNum(kAboveTexels * texel),
                      ring = jointNum(kRingTexels * texel);
    std::string h;
    h += "// Joint depth, shading: the slope of the joint mask bends the normal, and for walls in shade (no direct light\n";
    h += "// for the normal) the recess is carried by a directional cavity and a wider AO; the floor takes a mortar grey,\n";
    h += "// the faces a slight per-unit tone. Fades from mip " + jointNum(spec.shadeLodFull) + " to " +
         jointNum(spec.shadeLodOff) + ", not at the parallax's distance.\n";
    h += "float JtShade = saturate(JointDepth) * saturate(JointDepthCm) * (1.0 - smoothstep(" +
         jointNum(spec.shadeLodFull) + ", " + jointNum(spec.shadeLodOff) + ", JtLod));\n";
    h += "if (JtShade > 0.0)\n{\n";
    h += "\t// On the dominant projection (Arm blends all three): the pixel, 1.5 texels along u and v for the slope, 3 up\n";
    h += "\t// and down the image (under the unit above, over the one below) and 6 back along u for the wide ring.\n";
    const std::string arm = "Texture2DSample(ArmTex, ArmTexSampler, JtUv";
    h += heightHlsl("\t", "JtH0", arm + ")");
    h += heightHlsl("\t", "JtHu", arm + " + float2(" + slope + ", 0.0))");
    h += heightHlsl("\t", "JtHv", arm + " + float2(0.0, " + slope + "))");
    h += heightHlsl("\t", "JtHa", arm + " - float2(0.0, " + above + "))");
    h += heightHlsl("\t", "JtHb", arm + " + float2(0.0, " + above + "))");
    h += heightHlsl("\t", "JtHw", arm + " - float2(" + ring + ", 0.0))");
    h += "\t// The height's rise per unit of u and v, both in tile units: a world slope along JtTu and JtTv.\n";
    h += "\tfloat2 JtSlope = (" + jointNum(spec.normalStrength / (kSlopeTexels * texel)) +
         " * JointDepthCm / max(TileCm, 1.0) * JtDeep) * float2(JtHu - JtH0, JtHv - JtH0);\n";
    h += "\tNormalOut = normalize(NormalOut - JtShade * (JtSlope.x * JtTu + JtSlope.y * JtTv));\n";
    h += "\t// Per-unit tone on a grid of JointUnitsU x JointUnitsV per tile (every other course offset by half a unit when\n";
    h += "\t// JointBond is 1), each cell blending into the next over " + jointNum(spec.unitEdge) +
         " of a cell: the photo's units are not known.\n";
    h += "\tif (JointUnitsU > 0.0 && JointUnitsV > 0.0)\n\t{\n";
    h += "\t\tfloat2 JtCell = JtUv * float2(JointUnitsU, JointUnitsV) - 0.5;\n";
    h += "\t\tfloat JtRow = floor(JtCell.y);\n";
    h += unitRowHlsl(spec, "JtTone0", "JtRow");
    h += unitRowHlsl(spec, "JtTone1", "(JtRow + 1.0)");
    h += "\t\tfloat JtTone = lerp(JtTone0, JtTone1, smoothstep(" + jointNum(0.5f - 0.5f * spec.unitEdge) + ", " +
         jointNum(0.5f + 0.5f * spec.unitEdge) + ", JtCell.y - JtRow));\n";
    h += "\t\tfloat JtToneK = (2.0 * JtTone - 1.0) * JtShade * JtH0;\n";
    h += "\t\tColor *= 1.0 + " + jointNum(spec.toneVar) + " * JtToneK;\n";
    h += "\t\tRoughOut = saturate(RoughOut * (1.0 + " + jointNum(spec.roughVar) + " * JtToneK));\n";
    h += "\t}\n";
    h += "\t// Mortar on the joint's floor: a grey scaled by the tint's luminance, rough.\n";
    h += "\tfloat JtMortarK = " + jointNum(spec.mortarBlend) + " * (1.0 - JtH0) * JtShade;\n";
    h += "\tColor = lerp(Color, float3(" + jointNum(spec.mortarR) + ", " + jointNum(spec.mortarG) + ", " +
         jointNum(spec.mortarB) + ") * dot(Tint.rgb, float3(" + jointNum(kLum[0]) + ", " + jointNum(kLum[1]) + ", " +
         jointNum(kLum[2]) + ")), JtMortarK);\n";
    h += "\tRoughOut = lerp(RoughOut, " + jointNum(spec.mortarRough) + ", JtMortarK);\n";
    h += "\t// The cavity: the floor, darkest on the upper wall under the unit above, lighter on the lower edge open to the\n";
    h += "\t// sky, plus the ring standing higher than the pixel (a soft core); full from " +
         jointNum(spec.cavityRefCm) + " cm deep, stronger on glancing views.\n";
    h += "\tfloat JtCav = saturate(((1.0 - JtH0) * (" + jointNum(spec.cavityBase) + " + " + jointNum(spec.upperDark) +
         " * saturate(JtHa - JtHb) - " +
         jointNum(spec.skyLift) + " * saturate(JtHb - JtHa))\n";
    h += "\t\t+ " + jointNum(spec.wideAO) + " * saturate(0.25 * (JtHa + JtHb + JtHw + JtHu) - JtH0))\n";
    h += "\t\t* saturate(JointDepthCm * JtDeep / " + jointNum(std::max(spec.cavityRefCm, 1e-3f)) + ") * (1.0 + " +
         jointNum(spec.viewDark) + " * (1.0 - saturate(JtNdotV)))) * JtShade;\n";
    h += "\tAOOut *= lerp(1.0, " + jointNum(spec.cavityAO) + ", JtCav);\n";
    h += "\tRoughOut = saturate(RoughOut + " + jointNum(spec.cavityRough) + " * JtCav);\n";
    h += "\tColor *= lerp(1.0, " + jointNum(spec.cavityAlbedo) + ", JtCav) * (1.0 - " + jointNum(spec.dirt) +
         " * JtCav * smoothstep(0.25, 0.75, Mottle[0]));\n";
    h += "}\n";
    return h;
}

} // namespace ftcore
