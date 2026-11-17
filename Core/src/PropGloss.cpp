#include "ftcore/PropGloss.h"

#include "ftcore/SurfaceWeather.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ftcore {

namespace {

// Prefixed: in the Unreal module's unity build this file shares a translation unit with SurfaceWeather.cpp, whose
// anonymous helpers have the plain names.

// The z cell's fold into the 2D lattice (as the HLSL's L3): far apart, so a z step never lands on a nearby xy cell.
constexpr std::uint32_t kPgFoldX = 7919u;
constexpr std::uint32_t kPgFoldY = 6271u;
// The noises' offsets, in cells, so the five reads do not share a lattice.
constexpr float kPgFineOffset = 17.f;
constexpr float kPgDustOffset = 41.f;
constexpr float kPgPatchOffset = 73.f;
constexpr float kPgSoakOffset = 5.f;
// The dust patches' and the drying patches' thresholds on their noise.
constexpr float kPgDustFrom = 0.3f;
constexpr float kPgDustTo = 0.7f;
constexpr float kPgPatchFrom = 0.4f;
constexpr float kPgPatchTo = 0.6f;
// Luminance weights (linear Rec. 709) for the dust's desaturation.
constexpr float kPgLuma[3] = {0.2126f, 0.7152f, 0.0722f};

float pgSaturate(float x) { return std::clamp(x, 0.f, 1.f); }
float pgLerp(float a, float b, float t) { return a + (b - a) * t; }
// HLSL's smoothstep.
float pgSmoothstep(float a, float b, float x) {
    const float t = pgSaturate((x - a) / (b - a));
    return t * t * (3.f - 2.f * t);
}

std::string pgNum(float f) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(f));
    std::string s(buf);
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

char pgLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
bool pgIsUpper(char c) { return c >= 'A' && c <= 'Z'; }
bool pgIsLowerCh(char c) { return c >= 'a' && c <= 'z'; }
bool pgIsDigit(char c) { return c >= '0' && c <= '9'; }
bool pgIsAlnum(char c) { return pgIsUpper(c) || pgIsLowerCh(c) || pgIsDigit(c); }

// Whether a and b are the same letters, any case.
bool pgSameWord(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (pgLower(a[i]) != pgLower(b[i])) return false;
    return true;
}

constexpr std::string_view kPgSuffixes[] = {"s", "es", "d", "ed", "y", "en", "ware", "work"};

// Whether a token is the word, or the word plus one of the suffixes, any case.
bool pgTokenIs(std::string_view token, std::string_view word) {
    if (token.size() < word.size()) return false;
    if (!pgSameWord(token.substr(0, word.size()), word)) return false;
    const std::string_view rest = token.substr(word.size());
    if (rest.empty()) return true;
    for (std::string_view suffix : kPgSuffixes)
        if (pgSameWord(rest, suffix)) return true;
    return false;
}

constexpr std::string_view kPgSmoothWords[] = {"glaze", "glass", "water", "mirror", "polish", "chrome"};
constexpr std::string_view kPgMetalWords[] = {"iron",   "metal",  "metallic", "brass",     "bronze",   "steel",     "copper",
                                              "chrome", "zinc",   "silver",   "gold",      "gilt",     "pewter",    "nickel",
                                              "aluminium", "aluminum", "alu", "galv", "galvanised", "galvanized"};
constexpr std::string_view kPgLiquidWords[] = {"water", "broth", "soup", "liquid"};

// Whether any token of name matches any of the words. Tokens: runs of letters and digits, split at a lower-to-upper
// case step and between a letter and a digit.
template <std::size_t N>
bool pgNameHas(std::string_view name, const std::string_view (&words)[N]) {
    std::size_t i = 0;
    while (i < name.size()) {
        while (i < name.size() && !pgIsAlnum(name[i])) ++i;
        if (i >= name.size()) break;
        const std::size_t start = i;
        ++i;
        while (i < name.size() && pgIsAlnum(name[i])) {
            const char prev = name[i - 1], c = name[i];
            if (pgIsLowerCh(prev) && pgIsUpper(c)) break;
            if (pgIsDigit(prev) != pgIsDigit(c)) break;
            ++i;
        }
        const std::string_view token = name.substr(start, i - start);
        for (std::string_view word : words)
            if (pgTokenIs(token, word)) return true;
    }
    return false;
}

// The noise the layers read at a point in metres, cell size cellM, offset in cells.
float pgNoiseAt(const PropSurfaceIn& in, float cellM, float offset) {
    return propNoise(in.xM / cellM + offset, in.yM / cellM + offset, in.zM / cellM + offset);
}

}  // namespace

bool propNameSmooth(std::string_view name) { return pgNameHas(name, kPgSmoothWords); }

bool propNameMetal(std::string_view name) { return pgNameHas(name, kPgMetalWords); }

bool propNameLiquid(std::string_view name) { return pgNameHas(name, kPgLiquidWords); }

PropGlossResult propGlossCheck(const PropGlossInput& in) {
    PropGlossResult r;
    r.smooth = propNameSmooth(in.name);
    r.metal = propNameMetal(in.name);
    // Written so a NaN fails each rule.
    r.wrongMetal = !r.metal && !(in.metallic <= kPropMetalMax);
    r.glossy = !r.smooth && !(in.p50 >= kPropGlossMedianMin);
    r.spreadChecked = in.hasMap && !r.smooth;
    r.flatMap = r.spreadChecked && !((in.p90 - in.p10) >= kPropGlossSpreadMin);
    if (r.wrongMetal)
        r.failed = PropGlossRule::Metal;
    else if (r.glossy)
        r.failed = PropGlossRule::Glossy;
    else if (r.flatMap)
        r.failed = PropGlossRule::FlatMap;
    r.pass = r.failed == PropGlossRule::None;
    return r;
}

const char* propGlossReason(PropGlossRule rule) {
    switch (rule) {
        case PropGlossRule::None: return "";
        case PropGlossRule::Metal: return "metallic over 0.5 on a name that is not a metal (set metal 0, or name the metal)";
        case PropGlossRule::Glossy:
            return "median roughness under 0.40 on a name not on the smooth list (glaze, glass, water, mirror, polish, chrome)";
        case PropGlossRule::FlatMap: return "the roughness map's p90 - p10 is under 0.10 (a flat map: give it grain, wear and dirt)";
    }
    return "";
}

float propHistogramPercentile(const std::array<std::uint32_t, 256>& hist, float p) {
    std::uint64_t total = 0;
    for (std::uint32_t c : hist) total += c;
    if (total == 0) return 0.f;
    const double share = static_cast<double>(pgSaturate(p));
    const std::uint64_t need = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(share * static_cast<double>(total))));
    std::uint64_t running = 0;
    for (std::size_t i = 0; i < hist.size(); ++i) {
        running += hist[i];
        if (running >= need) return static_cast<float>(i) / 255.f;
    }
    return 1.f;
}

float propGrimeFor(AssetPack world) {
    switch (world) {
        case AssetPack::None:
        case AssetPack::Solhaven:
        case AssetPack::InkSolhaven: return 1.f;
        case AssetPack::Lattice:
        case AssetPack::Unwritten:
        case AssetPack::Restored: return 0.f;
    }
    return 0.f;
}

float propLatticeValue(int x, int y, int z) {
    const std::uint32_t uz = static_cast<std::uint32_t>(z);
    return streetLatticeValue(static_cast<int>(static_cast<std::uint32_t>(x) + uz * kPgFoldX),
                              static_cast<int>(static_cast<std::uint32_t>(y) + uz * kPgFoldY));
}

float propNoise(float x, float y, float z) {
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy), iz = static_cast<int>(fz);
    const float tx = x - fx, ty = y - fy, tz = z - fz;
    const float ux = tx * tx * (3.f - 2.f * tx), uy = ty * ty * (3.f - 2.f * ty), uz = tz * tz * (3.f - 2.f * tz);
    const float lo = pgLerp(pgLerp(propLatticeValue(ix, iy, iz), propLatticeValue(ix + 1, iy, iz), ux),
                            pgLerp(propLatticeValue(ix, iy + 1, iz), propLatticeValue(ix + 1, iy + 1, iz), ux), uy);
    const float hi = pgLerp(pgLerp(propLatticeValue(ix, iy, iz + 1), propLatticeValue(ix + 1, iy, iz + 1), ux),
                            pgLerp(propLatticeValue(ix, iy + 1, iz + 1), propLatticeValue(ix + 1, iy + 1, iz + 1), ux), uy);
    return pgLerp(lo, hi, uz);
}

PropSurfaceOut propSurface(const PropSurfaceIn& in, const PropSurfaceSpec& spec) {
    PropSurfaceOut out;
    std::array<float, 3> c = in.albedo;
    const float nz = std::clamp(in.normalZ, -1.f, 1.f);
    const float r0 = pgSaturate(in.rough);
    const float cav = 1.f - pgSaturate(in.ao);
    float r = r0;

    // Break-up: two scales of noise and the cavity, faded out on a smooth material.
    const float amp = spec.breakupAmp * pgSaturate(in.breakup) * pgSaturate(r0 / spec.smoothFrom);
    if (amp > 0.f) {
        const float nc = pgNoiseAt(in, spec.coarseM, 0.f);
        const float nf = pgNoiseAt(in, spec.fineM, kPgFineOffset);
        const float swing = spec.coarseShare * (2.f * nc - 1.f) + (1.f - spec.coarseShare) * (2.f * nf - 1.f);
        const float b = std::clamp(spec.breakupGain * swing + spec.cavityShare * cav, -1.f, 1.f);
        r = pgSaturate(r0 + amp * b);
    }
    out.breakup = r - r0;

    // Grime: dust on up faces (patchy) and in cavities.
    const float up = pgSmoothstep(spec.upFrom, spec.upTo, nz);
    float dust = 0.f;
    if (in.grime > 0.f) {
        const float dp = pgSmoothstep(kPgDustFrom, kPgDustTo, pgNoiseAt(in, spec.grimePatchM, kPgDustOffset));
        dust = pgSaturate(in.grime) * pgSaturate(std::max(up * pgLerp(spec.grimeFloor, 1.f, dp), spec.cavityDust * cav));
        const float grey = kPgLuma[0] * c[0] + kPgLuma[1] * c[1] + kPgLuma[2] * c[2];
        for (int k = 0; k < 3; ++k) {
            c[k] = pgLerp(c[k], grey, spec.grimeDesat * dust);
            c[k] = pgLerp(c[k], in.dustColor[k], spec.grimeTint * dust);
        }
        r = std::min(r + spec.grimeRough * dust, 1.f);
    }
    out.dust = dust;

    // Wet: up faces soak, sides a little, undersides not; darker, smoother, never under the floor.
    const float w = pgSaturate(in.wetness) * pgSaturate(in.weather);
    float soak = 0.f;
    if (w > 0.f) {
        const float down = pgSmoothstep(spec.downFrom, spec.downTo, -nz);
        const float expo = pgLerp(spec.sideShare, 1.f, up) * (1.f - down);
        const float wetPatch = pgSmoothstep(kPgPatchFrom, kPgPatchTo, pgNoiseAt(in, spec.patchM, kPgPatchOffset));
        const float dv = pgSmoothstep(kPgDustFrom, kPgDustTo, pgNoiseAt(in, spec.grimeSoakM, kPgSoakOffset));
        soak = w * expo * pgLerp(spec.dampShare, 1.f, wetPatch) * pgLerp(1.f, dv, spec.grimePatchy * dust);
        const float dark =
            pgLerp(1.f, spec.soakDarken, soak * pgLerp(1.f, spec.metalDarkenShare, pgSaturate(in.metallic)));
        for (int k = 0; k < 3; ++k) c[k] *= dark;
        const float rw = std::max(in.wetRough, spec.wetFloor);
        r = std::min(r, pgLerp(r, rw, std::pow(pgSaturate(soak), spec.roughCurve)));
    }
    out.soak = soak;

    out.albedo = c;
    out.rough = pgSaturate(r);
    return out;
}

std::string propSurfaceHlsl(const PropSurfaceSpec& spec) {
    const auto n = [](float f) { return pgNum(f); };
    std::string h;
    h.reserve(4096);
    // The 3D value noise: the street lattice (streetLatticeHlsl) with the z cell folded into x and y, as propNoise.
    h += "struct FTPropNoise\n{\n\t";
    h += streetLatticeHlsl();
    h += "\n";
    h += "\tfloat L3(int3 c) { return L(int2(uint(c.x) + uint(c.z) * " + std::to_string(kPgFoldX) + "u, uint(c.y) + uint(c.z) * " +
         std::to_string(kPgFoldY) + "u)); }\n";
    h += "\tfloat N(float3 p)\n\t{\n";
    h += "\t\tfloat3 b = floor(p);\n";
    h += "\t\tint3 i = int3(b);\n";
    h += "\t\tfloat3 f = p - b;\n";
    h += "\t\tfloat3 u = f * f * (3.0 - 2.0 * f);\n";
    h += "\t\tfloat lo = lerp(lerp(L3(i), L3(i + int3(1, 0, 0)), u.x), lerp(L3(i + int3(0, 1, 0)), L3(i + int3(1, 1, 0)), u.x), u.y);\n";
    h += "\t\tfloat hi = lerp(lerp(L3(i + int3(0, 0, 1)), L3(i + int3(1, 0, 1)), u.x), lerp(L3(i + int3(0, 1, 1)), L3(i + int3(1, 1, 1)), u.x), u.y);\n";
    h += "\t\treturn lerp(lo, hi, u.z);\n";
    h += "\t}\n";
    h += "};\n";
    h += "FTPropNoise PN;\n";
    h += "float3 Pm = WorldPos / 100.0;\n";
    h += "float Nz = clamp(normalize(VertexNormal).z, -1.0, 1.0);\n";
    h += "float3 C = Albedo;\n";
    h += "float R0 = saturate(Rough);\n";
    h += "float Cav = 1.0 - saturate(AO);\n";
    h += "float R = R0;\n";
    // Break-up.
    h += "float BkAmp = " + n(spec.breakupAmp) + " * saturate(Breakup) * saturate(R0 / " + n(spec.smoothFrom) + ");\n";
    h += "[branch] if (BkAmp > 0.0)\n{\n";
    h += "\tfloat Nc = PN.N(Pm / " + n(spec.coarseM) + ");\n";
    h += "\tfloat Nf = PN.N(Pm / " + n(spec.fineM) + " + " + n(kPgFineOffset) + ");\n";
    h += "\tfloat Swing = " + n(spec.coarseShare) + " * (2.0 * Nc - 1.0) + " + n(1.f - spec.coarseShare) + " * (2.0 * Nf - 1.0);\n";
    h += "\tfloat Bk = clamp(" + n(spec.breakupGain) + " * Swing + " + n(spec.cavityShare) + " * Cav, -1.0, 1.0);\n";
    h += "\tR = saturate(R0 + BkAmp * Bk);\n";
    h += "}\n";
    // Grime.
    h += "float Up = smoothstep(" + n(spec.upFrom) + ", " + n(spec.upTo) + ", Nz);\n";
    h += "float Dust = 0.0;\n";
    h += "[branch] if (Grime > 0.0)\n{\n";
    h += "\tfloat Dp = smoothstep(" + n(kPgDustFrom) + ", " + n(kPgDustTo) + ", PN.N(Pm / " + n(spec.grimePatchM) + " + " +
         n(kPgDustOffset) + "));\n";
    h += "\tDust = saturate(Grime) * saturate(max(Up * lerp(" + n(spec.grimeFloor) + ", 1.0, Dp), " + n(spec.cavityDust) +
         " * Cav));\n";
    h += "\tfloat Grey = dot(C, float3(" + n(kPgLuma[0]) + ", " + n(kPgLuma[1]) + ", " + n(kPgLuma[2]) + "));\n";
    h += "\tC = lerp(C, float3(Grey, Grey, Grey), " + n(spec.grimeDesat) + " * Dust);\n";
    h += "\tC = lerp(C, DustColor, " + n(spec.grimeTint) + " * Dust);\n";
    h += "\tR = min(R + " + n(spec.grimeRough) + " * Dust, 1.0);\n";
    h += "}\n";
    // Wet.
    h += "float W = saturate(Wetness) * saturate(Weather);\n";
    h += "[branch] if (W > 0.0)\n{\n";
    h += "\tfloat Down = smoothstep(" + n(spec.downFrom) + ", " + n(spec.downTo) + ", -Nz);\n";
    h += "\tfloat Expo = lerp(" + n(spec.sideShare) + ", 1.0, Up) * (1.0 - Down);\n";
    h += "\tfloat WetPatch = smoothstep(" + n(kPgPatchFrom) + ", " + n(kPgPatchTo) + ", PN.N(Pm / " + n(spec.patchM) + " + " +
         n(kPgPatchOffset) + "));\n";
    h += "\tfloat Dv = smoothstep(" + n(kPgDustFrom) + ", " + n(kPgDustTo) + ", PN.N(Pm / " + n(spec.grimeSoakM) + " + " +
         n(kPgSoakOffset) + "));\n";
    h += "\tfloat Soak = W * Expo * lerp(" + n(spec.dampShare) + ", 1.0, WetPatch) * lerp(1.0, Dv, " + n(spec.grimePatchy) +
         " * Dust);\n";
    h += "\tC *= lerp(1.0, " + n(spec.soakDarken) + ", Soak * lerp(1.0, " + n(spec.metalDarkenShare) + ", saturate(Metal)));\n";
    h += "\tfloat Rw = max(WetRough, " + n(spec.wetFloor) + ");\n";
    h += "\tR = min(R, lerp(R, Rw, pow(saturate(Soak), " + n(spec.roughCurve) + ")));\n";
    h += "}\n";
    h += "RoughOut = saturate(R);\n";
    h += "return C;\n";
    return h;
}

}  // namespace ftcore
