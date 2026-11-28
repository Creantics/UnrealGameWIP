#include "ftcore/RainCover.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ftcore {

namespace {

bool finite(float v) { return std::isfinite(v); }

}  // namespace

bool rainCoverEnabled(bool rainCoverGiven, bool noRainCoverGiven) { return rainCoverGiven && !noRainCoverGiven; }

bool rainCoverEmitter(bool emitterCover, bool coverEnabled) { return emitterCover && coverEnabled; }

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

namespace {

// Plan points this far from the origin (m) or further are not tiled (keeps every index well inside int).
constexpr float kTileReachM = 1.0e6f;

bool tileable(float x, float y) { return finite(x) && finite(y) && std::fabs(x) < kTileReachM && std::fabs(y) < kTileReachM; }

int floorDiv(int a, int b) {
    const int q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

int posMod3(int a) {
    const int m = a % 3;
    return m < 0 ? m + 3 : m;
}

bool inRing(RainCoverTile centre, RainCoverTile t) { return std::abs(t.x - centre.x) <= 1 && std::abs(t.y - centre.y) <= 1; }

}  // namespace

RainCoverTile rainCoverTileAt(const RainCoverSpec& spec, float x, float y) {
    RainCoverTile t;
    if (!(spec.sizeM > 0.f) || !tileable(x, y)) return t;
    t.x = static_cast<int>(std::floor(static_cast<double>(x) / spec.sizeM));
    t.y = static_cast<int>(std::floor(static_cast<double>(y) / spec.sizeM));
    return t;
}

RainCoverFrame rainCoverTileFrame(const RainCoverSpec& spec, RainCoverTile tile, float topZ) {
    RainCoverFrame f;
    f.sizeM = std::max(spec.sizeM, 0.f);
    f.cx = (static_cast<float>(tile.x) + 0.5f) * f.sizeM;
    f.cy = (static_cast<float>(tile.y) + 0.5f) * f.sizeM;
    f.topZ = finite(topZ) ? topZ : 0.f;
    return f;
}

bool rainCoverTileTexel(const RainCoverSpec& spec, float x, float y, RainCoverTile& tile, int& px, int& py) {
    tile = RainCoverTile{};
    px = py = 0;
    const float t = rainCoverTexel(spec);
    if (!(t > 0.f) || !tileable(x, y)) return false;
    const int res = spec.resolution;
    const int gi = static_cast<int>(std::floor(static_cast<double>(x) / t));  // whole texels along +X
    const int gj = static_cast<int>(std::floor(static_cast<double>(y) / t));  // ... along +Y
    tile.x = floorDiv(gi, res);
    tile.y = floorDiv(gj, res);
    py = res - 1 - (gi - tile.x * res);  // the image's down runs along -X
    px = gj - tile.y * res;              // its right along +Y
    return true;
}

int rainCoverRingSlot(RainCoverTile tile) { return posMod3(tile.x) + 3 * posMod3(tile.y); }

int rainCoverRingEntered(RainCoverTile from, RainCoverTile to, RainCoverTile (&out)[9]) {
    int n = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const RainCoverTile t{to.x + dx, to.y + dy};
            if (!inRing(from, t)) out[n++] = t;
        }
    }
    return n;
}

float rainCoverR16Error(float rangeM) {
    if (!finite(rangeM) || !(rangeM > 0.f)) return 0.f;
    return rangeM / (2.f * static_cast<float>(kRainCoverR16Top - 1u));
}

bool rainCoverR16Pack(const float* depthM, int count, float topZ, float farM, unsigned short* out, RainCoverR16Range& range,
                      float toleranceM) {
    range = RainCoverR16Range{};
    if (!depthM || !out || count <= 0) return false;
    const bool okTop = finite(topZ) && finite(farM);
    const auto hit = [&](float d) { return okTop && finite(d) && d < farM; };
    float lo = 0.f, hi = 0.f;
    for (int i = 0; i < count; ++i) {
        if (!hit(depthM[i])) continue;
        const float z = topZ - depthM[i];
        if (range.hits == 0 || z < lo) lo = z;
        if (range.hits == 0 || z > hi) hi = z;
        ++range.hits;
    }
    range.minZ = lo;
    range.maxZ = hi;
    const double span = static_cast<double>(hi) - static_cast<double>(lo);
    const double steps = static_cast<double>(kRainCoverR16Top - 1u);
    for (int i = 0; i < count; ++i) {
        if (!hit(depthM[i])) {
            out[i] = static_cast<unsigned short>(kRainCoverR16None);
            continue;
        }
        double q = 0.0;
        if (span > 0.0) q = std::round((static_cast<double>(topZ - depthM[i]) - lo) / span * steps);
        q = std::clamp(q, 0.0, steps);
        out[i] = static_cast<unsigned short>(1u + static_cast<unsigned>(q));
    }
    return okTop && rainCoverR16Error(hi - lo) <= toleranceM;
}

bool rainCoverR16Unpack(unsigned code, const RainCoverR16Range& range, float& z) {
    z = 0.f;
    if (code == kRainCoverR16None || code > kRainCoverR16Top) return false;
    const double span = static_cast<double>(range.maxZ) - static_cast<double>(range.minZ);
    z = static_cast<float>(range.minZ + span * static_cast<double>(code - 1u) / static_cast<double>(kRainCoverR16Top - 1u));
    return true;
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

const char* rainCoverSlotParam(int slot) {
    static const char* const kNames[kRainCoverRingSlots] = {"CoverSlot0", "CoverSlot1", "CoverSlot2", "CoverSlot3", "CoverSlot4",
                                                            "CoverSlot5", "CoverSlot6", "CoverSlot7", "CoverSlot8"};
    return (slot >= 0 && slot < kRainCoverRingSlots) ? kNames[slot] : "";
}

RainCoverSlot rainCoverSlotFor(RainCoverTile tile, const RainCoverR16Range& range) {
    RainCoverSlot s;
    s.tx = static_cast<float>(tile.x);
    s.ty = static_cast<float>(tile.y);
    s.minZcm = range.minZ * 100.f;
    s.maxZcm = range.maxZ * 100.f;
    return s;
}

int rainCoverAtlasSide(const RainCoverSpec& spec) { return spec.resolution > 0 ? 3 * spec.resolution : 0; }

void rainCoverRingOrigin(const RainCoverSpec& spec, int slot, int& ax, int& ay) {
    const int res = std::max(spec.resolution, 0);
    const int s = (slot >= 0 && slot < kRainCoverRingSlots) ? slot : 0;
    ax = (s % 3) * res;
    ay = (s / 3) * res;
}

bool rainCoverRingTexel(const RainCoverSpec& spec, float x, float y, RainCoverTile& tile, int& slot, int& ax, int& ay) {
    int px = 0, py = 0;
    slot = ax = ay = 0;
    if (!rainCoverTileTexel(spec, x, y, tile, px, py)) return false;
    slot = rainCoverRingSlot(tile);
    rainCoverRingOrigin(spec, slot, ax, ay);
    ax += px;
    ay += py;
    return true;
}

float rainCoverRingOpen(const RainCoverSpec& spec, const unsigned short* atlas, const RainCoverSlot (&slots)[9], float x, float y,
                        float z, float bias) {
    RainCoverTile tile;
    int slot = 0, ax = 0, ay = 0;
    if (!atlas || !rainCoverRingTexel(spec, x, y, tile, slot, ax, ay)) return 1.f;
    const RainCoverSlot& v = slots[slot];
    if (std::fabs(v.tx - static_cast<float>(tile.x)) >= 0.5f || std::fabs(v.ty - static_cast<float>(tile.y)) >= 0.5f) return 1.f;
    const unsigned code = atlas[static_cast<std::size_t>(ay) * static_cast<std::size_t>(rainCoverAtlasSide(spec)) + static_cast<std::size_t>(ax)];
    float oz = 0.f;
    if (!rainCoverR16Unpack(code, RainCoverR16Range{v.minZcm * 0.01f, v.maxZcm * 0.01f, 1}, oz)) return 1.f;
    return rainCoverOpen(oz, z, bias);
}

std::string rainCoverRingHlsl(const RainCoverSpec& spec, bool sprite) {
    const auto lit = [](double v) {
        char b[40];
        std::snprintf(b, sizeof b, "%.6g", v);
        return std::string(b);
    };
    const double texelCm = static_cast<double>(rainCoverTexel(spec)) * 100.0;
    const int res = std::max(spec.resolution, 1);
    std::string s =
        "struct FTRainCoverRing {\n"
        "  float4 Sl[9];\n"
        // Floor division and a non-negative mod (HLSL's int / and % truncate toward zero).
        "  int FloorDiv(int a, int b) { return a >= 0 ? a / b : (a - b + 1) / b; }\n"
        "  int PosMod3(int a) { int m = a % 3; return m < 0 ? m + 3 : m; }\n"
        // Openness at a plan point (cm) for a point at Z (cm): 1 when the slot holds another tile or no occluder.
        "  float OpenAt(Texture2D T, float2 P, float Z, float B) {\n"
        "    int2 g = int2(floor(P / $Texel));\n"
        "    int2 t = int2(FloorDiv(g.x, $Res), FloorDiv(g.y, $Res));\n"
        "    int2 l = g - t * $Res;\n"
        "    int2 m = int2(PosMod3(t.x), PosMod3(t.y));\n"
        "    float4 V = Sl[m.x + 3 * m.y];\n"
        "    float o = 1.0;\n"
        "    if (abs(V.x - float(t.x)) < 0.5 && abs(V.y - float(t.y)) < 0.5) {\n"
        "      uint code = (uint)round(T.Load(int3(m.x * $Res + l.y, m.y * $Res + ($Res - 1) - l.x, 0)).r * 65535.0);\n"
        "      if (code > 0u) {\n"
        "        float z = V.z + (V.w - V.z) * float(code - 1u) / 65534.0;\n"
        "        o = z > Z + B ? 0.0 : 1.0;\n"
        "      }\n"
        "    }\n"
        "    return o;\n"
        "  }\n"
        "  float Weight(float r) { return saturate(1.25 - r / max($Fringe, 0.01)); }\n"
        "  float Wet(Texture2D T, float On, float3 W, float3 N) {\n"
        "    float r = 1.0;\n"
        "    [branch] if (On >= 0.5) {\n"
        "      float h = length(N.xy);\n"
        "      float2 P = W.xy + (h > 0.0001 ? N.xy / h * $Wall * min(h, 1.0) : float2(0.0, 0.0));\n"
        "      float here = OpenAt(T, P, W.z, $Bias);\n"
        "      if (here < 1.0) {\n"
        "        float best = 0.0;\n"
        "        const float2 dirs[4] = { float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1) };\n"
        "        [unroll] for (int k = 0; k < 12; ++k) {\n"
        "          float d = $Fringe * float(k / 4 + 1) / 3.0;\n"
        "          best = max(best, OpenAt(T, P + dirs[k % 4] * d, W.z, $Bias) * Weight(d));\n"
        "        }\n"
        "        r = best;\n"
        "      }\n"
        "    }\n"
        "    return r;\n"
        "  }\n"
        "  float Open(Texture2D T, float On, float3 W) {\n"
        "    float r = 1.0;\n"
        "    [branch] if (On >= 0.5) {\n"
        "      r = OpenAt(T, W.xy, W.z, $Bias);\n"
        "    }\n"
        "    return r;\n"
        "  }\n"
        "};\n";
    const auto put = [&s](const char* name, const std::string& value) {
        const std::string key = name;
        for (std::size_t at = s.find(key); at != std::string::npos; at = s.find(key, at + value.size())) s.replace(at, key.size(), value);
    };
    put("$Texel", lit(texelCm));
    put("$Res", std::to_string(res));
    put("$Bias", lit(static_cast<double>(sprite ? spec.streakBiasM : spec.biasM) * 100.0));
    put("$Wall", lit(static_cast<double>(spec.wallOffsetM) * 100.0));
    put("$Fringe", lit(static_cast<double>(spec.fringeM) * 100.0));
    s += "FTRainCoverRing C;\n";
    for (int i = 0; i < kRainCoverRingSlots; ++i) s += std::string("C.Sl[") + std::to_string(i) + "] = " + rainCoverSlotParam(i) + ";\n";
    s += sprite ? "return C.Open(CoverTex, CoverOn, WorldPos);\n" : "return C.Wet(CoverTex, CoverOn, WorldPos, Normal);\n";
    return s;
}

namespace {

constexpr unsigned char kMagic[4] = {'F', 'T', 'R', '2'};

void putU32(unsigned char* p, unsigned v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>((v >> (8 * i)) & 0xffu);
}
unsigned getU32(const unsigned char* p) {
    unsigned v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<unsigned>(p[i]) << (8 * i);
    return v;
}
void putF32(unsigned char* p, float f) {
    unsigned v = 0;
    std::memcpy(&v, &f, 4);
    putU32(p, v);
}
float getF32(const unsigned char* p) {
    const unsigned v = getU32(p);
    float f = 0.f;
    std::memcpy(&f, &v, 4);
    return f;
}

}  // namespace

bool rainCoverTileHeaderOnly(const RainCoverR16Range& range, int count) {
    return range.hits <= 0 || (range.hits == count && range.minZ == range.maxZ);
}

void rainCoverHeaderWrite(const RainCoverTileHeader& h, unsigned char* out) {
    if (!out) return;
    std::memset(out, 0, kRainCoverFileHeader);
    std::memcpy(out, kMagic, 4);
    putU32(out + 4, kRainCoverFileVersion);
    putU32(out + 8, static_cast<unsigned>(h.tile.x));
    putU32(out + 12, static_cast<unsigned>(h.tile.y));
    putU32(out + 16, static_cast<unsigned>(h.res));
    putU32(out + 20, static_cast<unsigned>(h.hits));
    putF32(out + 24, h.topZ);
    putF32(out + 28, h.minZ);
    putF32(out + 32, h.maxZ);
    putF32(out + 36, h.farM);  // bytes 36..39 were zero in every v2 file before 12825
    putU32(out + 40, static_cast<unsigned>(h.key & 0xffffffffull));
    putU32(out + 44, static_cast<unsigned>(h.key >> 32));
}

bool rainCoverHeaderRead(const unsigned char* in, int size, RainCoverTileHeader& h) {
    h = RainCoverTileHeader{};
    if (!in || size < kRainCoverFileHeader || std::memcmp(in, kMagic, 4) != 0 || getU32(in + 4) != kRainCoverFileVersion) return false;
    const int res = static_cast<int>(getU32(in + 16));
    if (res < 0 || res > 4096) return false;
    h.tile.x = static_cast<int>(getU32(in + 8));
    h.tile.y = static_cast<int>(getU32(in + 12));
    h.res = res;
    h.hits = static_cast<int>(getU32(in + 20));
    h.topZ = getF32(in + 24);
    h.minZ = getF32(in + 28);
    h.maxZ = getF32(in + 32);
    h.farM = getF32(in + 36);
    h.key = static_cast<unsigned long long>(getU32(in + 40)) | (static_cast<unsigned long long>(getU32(in + 44)) << 32);
    return true;
}

int rainCoverPayloadBytes(const RainCoverTileHeader& h) { return h.res > 0 ? h.res * h.res * 2 : 0; }

void rainCoverTileFill(const RainCoverTileHeader& h, int res, unsigned short* out) {
    if (!out || res <= 0) return;
    const unsigned short code = static_cast<unsigned short>(h.hits > 0 ? 1u : kRainCoverR16None);
    for (int i = 0; i < res * res; ++i) out[i] = code;
}

bool rainCoverTileLine(const char* line, RainCoverTile& tile) {
    tile = RainCoverTile{};
    if (!line) return false;
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    const char* p = line;
    while (space(*p)) ++p;
    if (*p != 't' || !space(p[1])) return false;
    ++p;
    long v[2] = {0, 0};
    for (long& out : v) {
        char* end = nullptr;
        out = std::strtol(p, &end, 10);
        if (end == p || out < -1000000L || out > 1000000L) return false;
        p = end;
    }
    while (space(*p)) ++p;
    if (*p != '\0') return false;
    tile.x = static_cast<int>(v[0]);
    tile.y = static_cast<int>(v[1]);
    return true;
}

}  // namespace ftcore
