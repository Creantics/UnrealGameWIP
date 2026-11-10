#include "ftcore/WindowInterior.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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

float share(std::uint32_t bits16) { return static_cast<float>(bits16 & 0xFFFFu) / 65536.0f; }

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

std::uint32_t windowSeed(std::uint32_t any) { return windowHash(any) & kWindowSeedMask; }

float windowBayWidth(const WindowGridSpec& spec, std::uint32_t seed) {
    const std::uint32_t k = windowHash(seed ^ kBaySalt) % 7u;
    return spec.bayMin + (spec.bayMax - spec.bayMin) * static_cast<float>(k) / 6.0f;
}

WindowOpening windowOpening(const WindowGridSpec& spec, bool shop) {
    WindowOpening o;
    const float fill = shop ? spec.shopFill : spec.fill;
    o.u0 = 0.5f - fill * 0.5f;
    o.u1 = 0.5f + fill * 0.5f;
    o.v0 = (shop ? spec.shopSill : spec.sill) / spec.storey;
    o.v1 = (shop ? spec.shopTop : spec.sill + spec.opening) / spec.storey;
    return o;
}

WindowCell windowCellAt(const WindowGridSpec& spec, std::uint32_t seed, float faceWidth, float faceHeight, float along,
                        float up, bool street) {
    WindowCell c;
    const float bw = windowBayWidth(spec, seed);
    c.bayWidth = bw;
    if (!(spec.storey > 0.0f) || !(bw > 0.0f)) return c;
    const float usable = faceWidth - 2.0f * spec.edge;
    if (usable < bw) return c;
    const float n = std::floor(usable / bw);
    const float m0 = (faceWidth - n * bw) * 0.5f;
    const float a = along - m0;
    if (a < 0.0f || a >= n * bw || up < 0.0f) return c;
    const float col = std::clamp(std::floor(a / bw), 0.0f, n - 1.0f);
    const float st = std::floor(up / spec.storey);
    if (st * spec.storey + spec.storey > faceHeight + 0.001f) return c;
    c.bay = true;
    c.shop = street && st < 0.5f;
    c.storey = static_cast<std::uint32_t>(st);
    c.column = static_cast<std::uint32_t>(col);
    c.u = (a - col * bw) / bw;
    c.v = (up - st * spec.storey) / spec.storey;
    const WindowOpening o = windowOpening(spec, c.shop);
    const bool open = c.u >= o.u0 && c.u <= o.u1 && c.v >= o.v0 && c.v <= o.v1;
    const float fu = spec.frame / bw, fv = spec.frame / spec.storey;
    const bool glass = open && c.u >= o.u0 + fu && c.u <= o.u1 - fu && c.v >= o.v0 + fv && c.v <= o.v1 - fv;
    c.part = glass ? WindowPart::Glass : (open ? WindowPart::Frame : WindowPart::Wall);
    return c;
}

WindowLook windowLookFor(std::uint32_t seed, std::uint32_t storey, std::uint32_t column, bool shop, float litShare,
                         float curtainShare, float shopLit) {
    const std::uint32_t h = windowCellHash(seed, storey, column);
    const std::uint32_t g = windowHash(h);
    const std::uint32_t k = windowHash(g);
    WindowLook w;
    w.room = shop ? kWindowRooms + h % kWindowShops : h % kWindowRooms;
    w.mirror = ((h >> 3) & 1u) != 0u;
    w.tint = (h >> 4) % kWindowTints;
    w.lit = share(h >> 8) < (shop ? shopLit : litShare);
    if (!shop && share(g) < curtainShare) {
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
    const float s = 1.0f / (1.0f + t * dz);
    r.x = 0.5f + (u + t * dx - 0.5f) * s;
    r.y = 0.5f + (v + t * dy - 0.5f) * s;
    return r;
}

WindowCurtainHit windowCurtainHit(const WindowGridSpec& spec, float bayWidth, float u, float v, float dAlong,
                                  float dUp, float dIn, float drawn) {
    const float dx = dAlong / bayWidth;
    const float dy = dUp / spec.storey;
    const float dz = dIn / spec.roomDepth;
    const float t = (spec.curtainDepth / spec.roomDepth) / dz;
    const WindowOpening o = windowOpening(spec, false);
    WindowCurtainHit c;
    c.x = std::clamp((u + t * dx - o.u0) / (o.u1 - o.u0), 0.0f, 1.0f);
    c.y = std::clamp((v + t * dy - o.v0) / (o.v1 - o.v0), 0.0f, 1.0f);
    c.covered = c.y >= 1.0f - drawn;
    return c;
}

std::string windowInteriorHlsl(const WindowGridSpec& s) {
    const WindowOpening w = windowOpening(s, false);
    const WindowOpening p = windowOpening(s, true);
    std::string h;
    h.reserve(6144);
    h += "struct FTWin { uint H(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; "
         "x ^= x >> 16; return x; } };\n";
    h += "FTWin W;\n";
    // The grid (windowCellAt), with no early return: the texture reads below run on every pixel.
    h += "uint seed = (uint)max(Seed, 0.0);\n";
    h += "float storeyH = " + num(s.storey) + ";\n";
    h += "uint k = W.H(seed ^ " + unum(kBaySalt) + ") % 7u;\n";
    h += "float bw = " + num(s.bayMin) + " + (" + num(s.bayMax) + " - " + num(s.bayMin) + ") * float(k) / 6.0;\n";
    h += "float usable = FaceW - 2.0 * " + num(s.edge) + ";\n";
    h += "float n = floor(usable / bw);\n";
    h += "float m0 = (FaceW - n * bw) * 0.5;\n";
    h += "float a = Along - m0;\n";
    h += "float colF = clamp(floor(a / bw), 0.0, max(n - 1.0, 0.0));\n";
    h += "float stF = floor(max(Up, 0.0) / storeyH);\n";
    h += "bool inBay = usable >= bw && a >= 0.0 && a < n * bw && Up >= 0.0 && stF * storeyH + storeyH <= FaceH + 0.001;\n";
    h += "bool shop = Street > 0.5 && stF < 0.5;\n";
    h += "uint col = (uint)colF;\n";
    h += "uint st = (uint)stF;\n";
    h += "float u = (a - colF * bw) / bw;\n";
    h += "float v = (Up - stF * storeyH) / storeyH;\n";
    h += "float u0 = shop ? " + num(p.u0) + " : " + num(w.u0) + ";\n";
    h += "float u1 = shop ? " + num(p.u1) + " : " + num(w.u1) + ";\n";
    h += "float v0 = shop ? " + num(p.v0) + " : " + num(w.v0) + ";\n";
    h += "float v1 = shop ? " + num(p.v1) + " : " + num(w.v1) + ";\n";
    h += "bool inOpen = inBay && u >= u0 && u <= u1 && v >= v0 && v <= v1;\n";
    h += "float fu = " + num(s.frame) + " / bw;\n";
    h += "float fv = " + num(s.frame) + " / storeyH;\n";
    h += "bool inGlass = inOpen && u >= u0 + fu && u <= u1 - fu && v >= v0 + fv && v <= v1 - fv;\n";
    // The look (windowLookFor).
    h += "uint hh = W.H(seed + W.H(st + W.H(col)));\n";
    h += "uint g = W.H(hh);\n";
    h += "uint kk = W.H(g);\n";
    h += "uint room = shop ? " + unum(kWindowRooms) + " + hh % " + unum(kWindowShops) + " : hh % " + unum(kWindowRooms) +
         ";\n";
    h += "bool mirror = ((hh >> 3) & 1u) != 0u;\n";
    h += "uint tint = (hh >> 4) % " + unum(kWindowTints) + ";\n";
    h += "bool lit = float((hh >> 8) & 0xFFFFu) / 65536.0 < (shop ? ShopLit : LitShare);\n";
    h += "bool hasCurtain = !shop && float(g & 0xFFFFu) / 65536.0 < CurtainShare;\n";
    h += "uint curtain = (g >> 16) % " + unum(kWindowCurtains) + ";\n";
    h += "float drawn = 0.25 + 0.75 * float(kk & 0xFFu) / 255.0;\n";
    // The room (windowRoomHit).
    h += "float3 d = normalize(ViewT);\n";
    h += "float dx = d.x / bw;\n";
    h += "float dy = d.y / storeyH;\n";
    h += "float dz = max(d.z, 1e-4) / " + num(s.roomDepth) + ";\n";
    h += "float tx = dx > 0.0 ? (1.0 - u) / dx : (dx < 0.0 ? -u / dx : 1e30);\n";
    h += "float ty = dy > 0.0 ? (1.0 - v) / dy : (dy < 0.0 ? -v / dy : 1e30);\n";
    h += "float t = min(1.0 / dz, min(tx, ty));\n";
    h += "float sc = 1.0 / (1.0 + t * dz);\n";
    h += "float px = 0.5 + (u + t * dx - 0.5) * sc;\n";
    h += "float py = 0.5 + (v + t * dy - 0.5) * sc;\n";
    h += "px = mirror ? 1.0 - px : px;\n";
    h += "px = clamp(px, 0.002, 0.998);\n";
    h += "py = clamp(py, 0.002, 0.998);\n";
    h += "float2 ruv = float2((float(room % " + unum(kWindowAtlasCols) + ") + px) / " + num(kWindowAtlasCols) +
         ", (float(room / " + unum(kWindowAtlasCols) + ") + 1.0 - py) / " + num(kWindowAtlasRows) + ");\n";
    h += "float3 c = Texture2DSample(Rooms, RoomsSampler, ruv).rgb;\n";
    // The curtain (windowCurtainHit), over the window's rectangle.
    h += "float tc = " + num(s.curtainDepth / s.roomDepth) + " / dz;\n";
    h += "float cx = saturate((u + tc * dx - " + num(w.u0) + ") / " + num(w.u1 - w.u0) + ");\n";
    h += "float cy = saturate((v + tc * dy - " + num(w.v0) + ") / " + num(w.v1 - w.v0) + ");\n";
    h += "float2 cuv = float2((float(curtain) + clamp(cx, 0.002, 0.998)) / " + num(kWindowCurtains) +
         ", 1.0 - clamp(cy, 0.002, 0.998));\n";
    h += "float4 cc = Texture2DSample(Curtains, CurtainsSampler, cuv);\n";
    h += "bool covered = hasCurtain && cy >= 1.0 - drawn;\n";
    h += "c = covered ? lerp(c, cc.rgb, cc.a) : c;\n";
    // Night: lit rooms glow in their tint, the rest go dark.
    h += "float3 tints[4] = { float3(1.0, 0.78, 0.52), float3(1.0, 0.92, 0.80), float3(0.82, 0.90, 1.0), "
         "float3(1.0, 0.66, 0.42) };\n";
    h += "Glow = (inGlass && lit) ? c * tints[tint] * Night * GlowScale : float3(0.0, 0.0, 0.0);\n";
    h += "c *= lerp(1.0, lit ? 0.6 : 0.06, Night);\n";
    h += "return float4(c, inGlass ? 1.0 : (inOpen ? 2.0 : 0.0));\n";
    return h;
}

}  // namespace ftcore
