#include "ftcore/SurfaceJoints.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ftcore {

namespace {

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

int clampedMax(const SurfaceJointSpec& spec) { return std::max(spec.maxSteps, 1); }
int clampedMin(const SurfaceJointSpec& spec) { return std::clamp(spec.minSteps, 1, clampedMax(spec)); }

// The HLSL for "Dst = height of the ARM sample Src" in two statements (a Custom node has no functions of its own).
std::string heightHlsl(const std::string& indent, const std::string& dst, const std::string& src) {
    return indent + "float " + dst + " = saturate((" + src + ".r - JointLo) / max(JointHi - JointLo, 1e-4));\n" +
           indent + dst + " = " + dst + " * " + dst + " * (3.0 - 2.0 * " + dst + ");\n";
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
        float depthCm;
    };
    static constexpr Entry kEntries[] = {
        {"soot_brick", 1.0f},     {"breeze_block", 0.8f},   {"ceramic_tile", 0.3f},
        {"paving_slab", 0.65f},   {"tactile_paving", 0.5f}, {"board_concrete", 0.2f},
    };
    for (const Entry& e : kEntries)
        if (surfaceId.find(e.key) != std::string_view::npos) return {e.depthCm, 0.6f, 0.92f};
    return {0.f, 0.6f, 0.92f};
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
    h += "\tfloat2 JtFloor = JtRun * (JointDepthCm / max(TileCm, 1.0) * JointPar);\n";
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
    const float eps = 1.5f / std::max(spec.texels, 1.f);
    std::string h;
    h += "// Joint depth, shading: the slope of the joint mask bends the normal; the joints darken, dirty and roughen.\n";
    h += "// Fades once a joint is well under a pixel (mip " + jointNum(spec.shadeLodFull) + " to " +
         jointNum(spec.shadeLodOff) + "), not at the parallax's distance.\n";
    h += "float JtShade = saturate(JointDepth) * saturate(JointDepthCm) * (1.0 - smoothstep(" +
         jointNum(spec.shadeLodFull) + ", " + jointNum(spec.shadeLodOff) + ", JtLod));\n";
    h += "if (JtShade > 0.0)\n{\n";
    h += "\t// The centre is sampled again on the dominant projection (Arm blends all three), the neighbours 1.5 texels on.\n";
    const std::string arm = "Texture2DSample(ArmTex, ArmTexSampler, JtUv";
    h += heightHlsl("\t", "JtH0", arm + ")");
    h += heightHlsl("\t", "JtHu", arm + " + float2(" + jointNum(eps) + ", 0.0))");
    h += heightHlsl("\t", "JtHv", arm + " + float2(0.0, " + jointNum(eps) + "))");
    h += "\t// Some courses sit deeper than others (the large-scale mottle).\n";
    h += "\tfloat JtDeep = lerp(" + jointNum(1.f - spec.uneven) + ", 1.0, Mottle[2]);\n";
    h += "\t// The height's rise per unit of u and v, both in tile units: a world slope along JtTu and JtTv.\n";
    h += "\tfloat2 JtSlope = (" + jointNum(spec.normalStrength / eps) +
         " * JointDepthCm / max(TileCm, 1.0) * JtDeep) * float2(JtHu - JtH0, JtHv - JtH0);\n";
    h += "\tNormalOut = normalize(NormalOut - JtShade * (JtSlope.x * JtTu + JtSlope.y * JtTv));\n";
    h += "\tfloat JtCav = (1.0 - JtH0) * JtShade * JtDeep;\n";
    h += "\tAOOut *= lerp(1.0, " + jointNum(spec.cavityAO) + ", JtCav);\n";
    h += "\tRoughOut = saturate(RoughOut + " + jointNum(spec.cavityRough) + " * JtCav);\n";
    h += "\tColor *= lerp(1.0, " + jointNum(spec.cavityAlbedo) + ", JtCav) * (1.0 - " + jointNum(spec.dirt) +
         " * JtCav * smoothstep(0.25, 0.75, Mottle[0]));\n";
    h += "}\n";
    return h;
}

} // namespace ftcore
