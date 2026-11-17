#include "ftcore/RainCover.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ftcore {

namespace {

bool finite(float v) { return std::isfinite(v); }

}  // namespace

float rainCoverTexel(const RainCoverSpec& spec) {
    if (!(spec.sizeM > 0.f) || spec.resolution <= 0) return 0.f;
    return spec.sizeM / static_cast<float>(spec.resolution);
}

void rainCoverSnap(const RainCoverSpec& spec, float camX, float camY, float& cx, float& cy) {
    const float t = rainCoverTexel(spec);
    if (!(t > 0.f) || !finite(camX) || !finite(camY)) {
        cx = finite(camX) ? camX : 0.f;
        cy = finite(camY) ? camY : 0.f;
        return;
    }
    cx = std::round(camX / t) * t;
    cy = std::round(camY / t) * t;
}

bool rainCoverRecapture(const RainCoverSpec& spec, bool haveCapture, float cx, float cy, float camX, float camY) {
    if (!haveCapture) return true;
    const float dx = camX - cx, dy = camY - cy;
    const float d2 = dx * dx + dy * dy;
    if (!finite(d2)) return true;
    const float r = std::max(spec.recaptureM, 0.f);
    return d2 >= r * r;
}

RainCoverFrame rainCoverFrameAt(const RainCoverSpec& spec, float camX, float camY, float camZ) {
    RainCoverFrame f;
    rainCoverSnap(spec, camX, camY, f.cx, f.cy);
    f.sizeM = std::max(spec.sizeM, 0.f);
    f.topZ = (finite(camZ) ? camZ : 0.f) + spec.aboveM;
    return f;
}

bool rainCoverUV(const RainCoverFrame& frame, float x, float y, float& u, float& v) {
    u = v = 0.f;
    if (!(frame.sizeM > 0.f)) return false;
    u = (y - frame.cy) / frame.sizeM + 0.5f;
    v = (frame.cx - x) / frame.sizeM + 0.5f;
    return u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f;
}

float rainCoverOpen(float occluderZ, float z, float bias) {
    if (!finite(occluderZ) || !finite(z)) return 1.f;
    return occluderZ > z + bias ? 0.f : 1.f;
}

float rainCoverFringeWeight(float r, float fringeM) {
    if (!(fringeM > 0.f)) return r <= 0.f ? 1.f : 0.f;
    return std::clamp(1.25f - r / fringeM, 0.f, 1.f);
}

float rainCoverWet(float openHere, const float (&ring)[12], float fringeM) {
    const float here = std::clamp(openHere, 0.f, 1.f);
    if (here >= 1.f) return 1.f;
    float best = here;
    for (int k = 0; k < 12; ++k) {
        const float r = fringeM * static_cast<float>(k / 4 + 1) / 3.f;
        best = std::max(best, std::clamp(ring[k], 0.f, 1.f) * rainCoverFringeWeight(r, fringeM));
    }
    return best;
}

void rainCoverReadPoint(const RainCoverSpec& spec, float x, float y, float nx, float ny, float nz, float& rx, float& ry) {
    rx = x;
    ry = y;
    const float h = std::sqrt(nx * nx + ny * ny);
    if (!(h > 1e-4f) || !finite(nz)) return;
    // A floor (nz 1) not at all, a wall (nz 0) fully: the horizontal share is h itself for a unit normal.
    const float out = spec.wallOffsetM * std::min(h, 1.f);
    rx = x + nx / h * out;
    ry = y + ny / h * out;
}

std::string rainCoverHlsl(const RainCoverSpec& spec, bool sprite) {
    // Struct members can't be static consts in HLSL: the numbers (cm) go in as literals.
    const auto lit = [](float m) {
        char b[32];
        std::snprintf(b, sizeof b, "%.1f", static_cast<double>(m * 100.f));
        return std::string(b);
    };
    std::string s =
        "struct FTRainCover {\n"
        // Openness at a plan point (cm) for a point at z (cm): 1 outside the square or with no capture.
        "  float OpenAt(Texture2D T, SamplerState S, float4 F, float2 P, float Z, float B) {\n"
        "    float2 uv = float2((P.y - F.y) / F.z + 0.5, (F.x - P.x) / F.z + 0.5);\n"
        "    if (any(uv < 0.0) || any(uv > 1.0)) return 1.0;\n"
        "    float d = T.SampleLevel(S, uv, 0).r;\n"
        "    if (d >= $Depth) return 1.0;\n"
        "    return (F.w - d) > Z + B ? 0.0 : 1.0;\n"
        "  }\n"
        "  float Weight(float r) { return saturate(1.25 - r / max($Fringe, 0.01)); }\n"
        "  float Wet(Texture2D T, SamplerState S, float4 F, float On, float3 W, float3 N) {\n"
        "    if (On < 0.5) return 1.0;\n"
        "    float h = length(N.xy);\n"
        "    float2 P = W.xy + (h > 0.0001 ? N.xy / h * $Wall * min(h, 1.0) : float2(0.0, 0.0));\n"
        "    float here = OpenAt(T, S, F, P, W.z, $Bias);\n"
        "    if (here >= 1.0) return 1.0;\n"
        "    float best = 0.0;\n"
        "    const float2 dirs[4] = { float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1) };\n"
        "    [unroll] for (int k = 0; k < 12; ++k) {\n"
        "      float r = $Fringe * float(k / 4 + 1) / 3.0;\n"
        "      best = max(best, OpenAt(T, S, F, P + dirs[k % 4] * r, W.z, $Bias) * Weight(r));\n"
        "    }\n"
        "    return best;\n"
        "  }\n"
        "  float Open(Texture2D T, SamplerState S, float4 F, float On, float3 W) {\n"
        "    if (On < 0.5) return 1.0;\n"
        "    return OpenAt(T, S, F, W.xy, W.z, $Bias);\n"
        "  }\n"
        "};\n";
    const auto put = [&s](const char* name, const std::string& value) {
        const std::string key = name;
        for (std::size_t at = s.find(key); at != std::string::npos; at = s.find(key, at + value.size())) s.replace(at, key.size(), value);
    };
    put("$Depth", lit(spec.depthM));
    put("$Bias", lit(sprite ? spec.streakBiasM : spec.biasM));
    put("$Wall", lit(spec.wallOffsetM));
    put("$Fringe", lit(spec.fringeM));
    // A local struct's methods are called through an instance (static members of one don't compile everywhere).
    s += sprite ? "FTRainCover C;\nreturn C.Open(CoverTex, CoverTexSampler, CoverFrame, CoverOn, WorldPos);\n"
                : "FTRainCover C;\nreturn C.Wet(CoverTex, CoverTexSampler, CoverFrame, CoverOn, WorldPos, Normal);\n";
    return s;
}

}  // namespace ftcore
