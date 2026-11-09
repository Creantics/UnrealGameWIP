#include "ftcore/WindowInterior.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ftcore {

namespace {

constexpr std::uint32_t kBaySalt = 0x9e3779b9u;
constexpr float kNever = 1e30f;

// A float as an HLSL float literal with every digit kept.
std::string num(float f) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(f));
    std::string s(buf);
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

std::string unum(std::uint32_t u) { return std::to_string(u) + "u"; }

float toSide(float x, float d) {
    if (d > 0.0f) return (1.0f - x) / d;
    if (d < 0.0f) return -x / d;
    return kNever;
}

}  // namespace

std::uint32_t windowHash(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

std::uint32_t windowCellHash(std::uint32_t seed, std::uint32_t storey, std::uint32_t bay) {
    return windowHash(seed + windowHash(storey + windowHash(bay)));
}

float windowBayWidth(const WindowGridSpec& spec, std::uint32_t seed) {
    const std::uint32_t k = windowHash(seed ^ kBaySalt) % 7u;
    return spec.bayMin + (spec.bayMax - spec.bayMin) * static_cast<float>(k) / 6.0f;
}

WindowOpening windowOpening(const WindowGridSpec& spec, float /*bayWidth*/) {
    WindowOpening o;
    o.u0 = 0.5f - spec.fill * 0.5f;
    o.u1 = 0.5f + spec.fill * 0.5f;
    o.v0 = spec.sill / spec.storey;
    o.v1 = (spec.sill + spec.opening) / spec.storey;
    return o;
}

WindowCell windowCellAt(const WindowGridSpec& spec, std::uint32_t seed, float faceWidth, float faceHeight, float along,
                        float up) {
    WindowCell c;
    const float bw = windowBayWidth(spec, seed);
    c.bayWidth = bw;
    if (!(spec.storey > 0.0f) || !(bw > 0.0f)) return c;
    const float usable = faceWidth - 2.0f * spec.edge;
    if (usable < bw) return c;
    const auto n = static_cast<std::uint32_t>(std::floor(usable / bw));
    const float m0 = (faceWidth - static_cast<float>(n) * bw) * 0.5f;
    const float a = along - m0;
    if (a < 0.0f || a >= static_cast<float>(n) * bw || up < 0.0f) return c;
    const std::uint32_t col = std::min(static_cast<std::uint32_t>(std::floor(a / bw)), n - 1u);
    const auto st = static_cast<std::uint32_t>(std::floor(up / spec.storey));
    const float base = static_cast<float>(st) * spec.storey;
    if (base + spec.sill < spec.groundBand || base + spec.storey > faceHeight + 0.001f) return c;
    c.bay = true;
    c.storey = st;
    c.column = col;
    c.u = (a - static_cast<float>(col) * bw) / bw;
    c.v = (up - base) / spec.storey;
    const WindowOpening o = windowOpening(spec, bw);
    c.opening = c.u >= o.u0 && c.u <= o.u1 && c.v >= o.v0 && c.v <= o.v1;
    return c;
}

WindowLook windowLookFor(std::uint32_t seed, std::uint32_t storey, std::uint32_t column, float litShare,
                         float curtainShare) {
    const std::uint32_t h = windowCellHash(seed, storey, column);
    const std::uint32_t g = windowHash(h);
    const std::uint32_t k = windowHash(g);
    WindowLook w;
    w.room = h % kWindowRooms;
    w.mirror = ((h >> 3) & 1u) != 0u;
    w.tint = (h >> 4) % kWindowTints;
    w.lit = static_cast<float>((h >> 8) & 0xFFFFu) / 65536.0f < litShare;
    if (static_cast<float>(g & 0xFFFFu) / 65536.0f < curtainShare) {
        w.curtain = static_cast<int>((g >> 16) % kWindowCurtains);
        w.drawn = 0.25f + 0.75f * static_cast<float>(k & 0xFFu) / 255.0f;
    }
    return w;
}

WindowRoomHit windowRoomHit(const WindowGridSpec& spec, float bayWidth, float u, float v, float dAlong, float dUp,
                            float dIn) {
    const float dx = dAlong / bayWidth;
    const float dy = dUp / spec.storey;
    const float dz = dIn / spec.roomDepth;
    const float tx = toSide(u, dx);
    const float ty = toSide(v, dy);
    const float tz = 1.0f / dz;
    const float t = std::min(tz, std::min(tx, ty));
    WindowRoomHit r;
    if (tz <= tx && tz <= ty) r.face = 0;
    else if (ty < tx) r.face = dy < 0.0f ? 1 : 2;
    else r.face = dx < 0.0f ? 3 : 4;
    const float hx = u + t * dx;
    const float hy = v + t * dy;
    const float s = 1.0f / (1.0f + t * dz);
    r.x = 0.5f + (hx - 0.5f) * s;
    r.y = 0.5f + (hy - 0.5f) * s;
    return r;
}

WindowCurtainHit windowCurtainHit(const WindowGridSpec& spec, float bayWidth, float u, float v, float dAlong,
                                  float dUp, float dIn, float drawn) {
    const float dx = dAlong / bayWidth;
    const float dy = dUp / spec.storey;
    const float dz = dIn / spec.roomDepth;
    const float t = (spec.curtainDepth / spec.roomDepth) / dz;
    const WindowOpening o = windowOpening(spec, bayWidth);
    WindowCurtainHit c;
    c.x = std::clamp((u + t * dx - o.u0) / (o.u1 - o.u0), 0.0f, 1.0f);
    c.y = std::clamp((v + t * dy - o.v0) / (o.v1 - o.v0), 0.0f, 1.0f);
    c.covered = c.y >= 1.0f - drawn;
    return c;
}

std::string windowInteriorHlsl(const WindowGridSpec& s) {
    const WindowOpening o = windowOpening(s, 0.0f);
    std::string h;
    h.reserve(4096);
    h += "struct FTWin { uint H(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; "
         "x ^= x >> 16; return x; } };\n";
    h += "FTWin W;\n";
    h += "Glow = float3(0, 0, 0);\n";
    h += "uint seed = asuint(Seed);\n";
    h += "float storeyH = " + num(s.storey) + ";\n";
    h += "uint k = W.H(seed ^ " + unum(kBaySalt) + ") % 7u;\n";
    h += "float bw = " + num(s.bayMin) + " + (" + num(s.bayMax) + " - " + num(s.bayMin) + ") * float(k) / 6.0;\n";
    h += "float usable = FaceW - 2.0 * " + num(s.edge) + ";\n";
    h += "if (usable < bw) return float4(0, 0, 0, 0);\n";
    h += "uint n = (uint)floor(usable / bw);\n";
    h += "float m0 = (FaceW - float(n) * bw) * 0.5;\n";
    h += "float a = Along - m0;\n";
    h += "if (a < 0.0 || a >= float(n) * bw || Up < 0.0) return float4(0, 0, 0, 0);\n";
    h += "uint col = min((uint)floor(a / bw), n - 1u);\n";
    h += "uint st = (uint)floor(Up / storeyH);\n";
    h += "float base = float(st) * storeyH;\n";
    h += "if (base + " + num(s.sill) + " < " + num(s.groundBand) + " || base + storeyH > FaceH + 0.001) "
         "return float4(0, 0, 0, 0);\n";
    h += "float u = (a - float(col) * bw) / bw;\n";
    h += "float v = (Up - base) / storeyH;\n";
    h += "float u0 = " + num(o.u0) + ", u1 = " + num(o.u1) + ", v0 = " + num(o.v0) + ", v1 = " + num(o.v1) + ";\n";
    h += "if (u < u0 || u > u1 || v < v0 || v > v1) return float4(0, 0, 0, 0);\n";
    // The look (windowLookFor).
    h += "uint h = W.H(seed + W.H(st + W.H(col)));\n";
    h += "uint g = W.H(h);\n";
    h += "uint kk = W.H(g);\n";
    h += "uint room = h % " + unum(kWindowRooms) + ";\n";
    h += "bool mirror = ((h >> 3) & 1u) != 0u;\n";
    h += "uint tint = (h >> 4) % " + unum(kWindowTints) + ";\n";
    h += "bool lit = float((h >> 8) & 0xFFFFu) / 65536.0 < LitShare;\n";
    h += "bool hasCurtain = float(g & 0xFFFFu) / 65536.0 < CurtainShare;\n";
    h += "uint curtain = (g >> 16) % " + unum(kWindowCurtains) + ";\n";
    h += "float drawn = 0.25 + 0.75 * float(kk & 0xFFu) / 255.0;\n";
    // The room (windowRoomHit).
    h += "float3 d = normalize(ViewT);\n";
    h += "float dx = d.x / bw, dy = d.y / storeyH, dz = max(d.z, 1e-4) / " + num(s.roomDepth) + ";\n";
    h += "float tx = dx > 0.0 ? (1.0 - u) / dx : (dx < 0.0 ? -u / dx : 1e30);\n";
    h += "float ty = dy > 0.0 ? (1.0 - v) / dy : (dy < 0.0 ? -v / dy : 1e30);\n";
    h += "float t = min(1.0 / dz, min(tx, ty));\n";
    h += "float sc = 1.0 / (1.0 + t * dz);\n";
    h += "float px = 0.5 + (u + t * dx - 0.5) * sc;\n";
    h += "float py = 0.5 + (v + t * dy - 0.5) * sc;\n";
    h += "if (mirror) px = 1.0 - px;\n";
    h += "px = clamp(px, 0.002, 0.998); py = clamp(py, 0.002, 0.998);\n";
    h += "float2 ruv = float2((float(room % " + unum(kWindowAtlasCols) + ") + px) / " + num(kWindowAtlasCols) +
         ", (float(room / " + unum(kWindowAtlasCols) + ") + 1.0 - py) / " + num(kWindowAtlasRows) + ");\n";
    h += "float3 c = Texture2DSample(Rooms, RoomsSampler, ruv).rgb;\n";
    // The curtain (windowCurtainHit).
    h += "if (hasCurtain) {\n";
    h += "  float tc = " + num(s.curtainDepth / s.roomDepth) + " / dz;\n";
    h += "  float cx = saturate((u + tc * dx - u0) / (u1 - u0));\n";
    h += "  float cy = saturate((v + tc * dy - v0) / (v1 - v0));\n";
    h += "  if (cy >= 1.0 - drawn) {\n";
    h += "    float2 cuv = float2((float(curtain) + clamp(cx, 0.002, 0.998)) / " + num(kWindowCurtains) +
         ", 1.0 - clamp(cy, 0.002, 0.998));\n";
    h += "    float4 cc = Texture2DSample(Curtains, CurtainsSampler, cuv);\n";
    h += "    c = lerp(c, cc.rgb, cc.a);\n";
    h += "  }\n";
    h += "}\n";
    // Night: lit rooms glow in their tint, the rest go dark.
    h += "float3 tints[4] = { float3(1.0, 0.78, 0.52), float3(1.0, 0.92, 0.80), float3(0.82, 0.90, 1.0), "
         "float3(1.0, 0.66, 0.42) };\n";
    h += "Glow = lit ? c * tints[tint] * Night * GlowScale : float3(0, 0, 0);\n";
    h += "c *= lerp(1.0, lit ? 0.6 : 0.06, Night);\n";
    h += "return float4(c, 1.0);\n";
    return h;
}

}  // namespace ftcore
