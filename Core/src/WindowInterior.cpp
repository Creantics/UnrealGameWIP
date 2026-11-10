#include "ftcore/WindowInterior.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ftcore {

namespace {

// Each of a window's looks reads its own salted hash of the cell, so no two of them move together.
constexpr std::uint32_t kRoomSalt = 0x68bc21ebu;
constexpr std::uint32_t kMirrorSalt = 0x02e5be93u;
constexpr std::uint32_t kTintSalt = 0x967a889bu;
constexpr std::uint32_t kLitSalt = 0x5bd1e995u;
constexpr std::uint32_t kCurtainSalt = 0x1b873593u;
constexpr std::uint32_t kPickSalt = 0xcc9e2d51u;
constexpr std::uint32_t kDrawSalt = 0x85ebca6bu;
constexpr float kNever = 1e30f;
constexpr float kLeastWindow = 0.3f;  // a window width under this is taken as this

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

std::uint32_t nextRoom(std::uint32_t room, bool shop) {
    return shop ? kWindowRooms + (room - kWindowRooms + 1u) % kWindowShops : (room + 1u) % kWindowRooms;
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

std::uint32_t windowSeed(std::uint32_t any) { return windowHash(any) & kWindowSeedMask; }

WindowFaceGrid windowFaceGrid(const WindowGridSpec& spec, float faceWidth, float faceHeight, float windowWidth) {
    WindowFaceGrid g;
    if (!(spec.storey > 0.0f) || !(spec.bay > 0.0f) || !(spec.storeyMin > 0.0f) || !(faceHeight >= spec.storeyMin))
        return g;
    float nst = std::max(std::floor(faceHeight / spec.storey + 0.5f), 1.0f);
    if (faceHeight / nst < spec.storeyMin) nst = std::floor(faceHeight / spec.storeyMin);
    g.storeys = static_cast<std::uint32_t>(nst);
    g.storeyHeight = std::min(faceHeight / nst, spec.storeyMax);
    const float ww = std::max(windowWidth, kLeastWindow);
    const float usable = faceWidth - 2.0f * spec.edge;
    const float nb =
        std::min(std::max(std::floor(usable / spec.bay + 0.5f), 1.0f), std::floor(usable / (ww + 2.0f * spec.pier)));
    if (nb >= 1.0f) {
        g.bays = static_cast<std::uint32_t>(nb);
        g.bayWidth = usable / nb;
    }
    return g;
}

WindowOpening windowOpening(const WindowGridSpec& spec, bool shop, float bayWidth, float storeyHeight,
                            float windowWidth) {
    WindowOpening o;
    if (!(bayWidth > 0.0f) || !(storeyHeight > 0.0f)) return o;
    const float ww = std::max(windowWidth, kLeastWindow);
    const float half = shop ? spec.shopFill * 0.5f : std::min(ww, bayWidth - 2.0f * spec.pier) / (2.0f * bayWidth);
    o.u0 = 0.5f - half;
    o.u1 = 0.5f + half;
    o.v0 = (shop ? spec.shopSill : spec.sill) / storeyHeight;
    o.v1 = (shop ? std::min(spec.shopTop, storeyHeight - spec.fascia) : spec.sill + spec.opening) / storeyHeight;
    return o;
}

WindowCell windowCellAt(const WindowGridSpec& spec, float faceWidth, float faceHeight, float along, float up,
                        bool street, float windowWidth) {
    WindowCell c;
    const WindowFaceGrid g = windowFaceGrid(spec, faceWidth, faceHeight, windowWidth);
    c.bayWidth = g.bayWidth;
    c.storeyHeight = g.storeyHeight;
    if (g.storeys == 0u || up < 0.0f) return c;
    const float sh = g.storeyHeight;
    const float st = std::floor(up / sh);
    if (st >= static_cast<float>(g.storeys)) return c;  // the parapet over the top storey
    c.storey = static_cast<std::uint32_t>(st);
    c.v = (up - st * sh) / sh;
    c.band = street && st < 0.5f;
    if (c.band) {
        const float top = std::min(spec.shopTop, sh - spec.fascia);
        c.part = up >= top && up < top + spec.fascia ? WindowPart::Fascia : WindowPart::Band;
    }
    if (g.bays == 0u) return c;
    const float bw = g.bayWidth;
    const float nb = static_cast<float>(g.bays);
    const float a = along - spec.edge;
    if (a < 0.0f || a >= nb * bw) return c;
    const float col = std::clamp(std::floor(a / bw), 0.0f, nb - 1.0f);
    c.bay = true;
    c.shop = c.band;
    c.column = static_cast<std::uint32_t>(col);
    c.u = (a - col * bw) / bw;
    const WindowOpening o = windowOpening(spec, c.shop, bw, sh, windowWidth);
    const bool open = c.u >= o.u0 && c.u <= o.u1 && c.v >= o.v0 && c.v <= o.v1;
    const float fu = spec.frame / bw, fv = spec.frame / sh;
    const bool glass = open && c.u >= o.u0 + fu && c.u <= o.u1 - fu && c.v >= o.v0 + fv && c.v <= o.v1 - fv;
    if (open) c.part = glass ? WindowPart::Glass : WindowPart::Frame;
    return c;
}

std::uint32_t windowRawRoom(std::uint32_t seed, std::uint32_t storey, std::uint32_t column, bool shop) {
    const std::uint32_t r = windowHash(windowCellHash(seed, storey, column) ^ kRoomSalt);
    return shop ? kWindowRooms + r % kWindowShops : r % kWindowRooms;
}

std::uint32_t windowRoomFor(std::uint32_t seed, std::uint32_t storey, std::uint32_t column, bool shop) {
    std::uint32_t room = windowRawRoom(seed, storey, column, shop);
    if (((storey + column) & 1u) == 0u) return room;
    const std::uint32_t above = windowRawRoom(seed, storey + 1u, column, false);
    const std::uint32_t below = windowRawRoom(seed, storey - 1u, column, false);
    const std::uint32_t left = windowRawRoom(seed, storey, column - 1u, shop);
    const std::uint32_t right = windowRawRoom(seed, storey, column + 1u, shop);
    for (std::uint32_t i = 0; i < kWindowAvoidSteps; ++i)
        if (room == above || room == below || room == left || room == right) room = nextRoom(room, shop);
    return room;
}

WindowLook windowLookFor(const WindowGridSpec& spec, std::uint32_t seed, std::uint32_t storey, std::uint32_t column,
                         bool shop, float litShare, float curtainShare, float shopLit) {
    const std::uint32_t h = windowCellHash(seed, storey, column);
    WindowLook w;
    w.room = windowRoomFor(seed, storey, column, shop);
    w.mirror = (windowHash(h ^ kMirrorSalt) & 1u) != 0u;
    w.tint = windowHash(h ^ kTintSalt) % kWindowTints;
    w.lit = share(windowHash(h ^ kLitSalt)) < (shop ? shopLit : litShare);
    if (!shop && share(windowHash(h ^ kCurtainSalt)) < curtainShare) {
        w.curtain = static_cast<int>(windowHash(h ^ kPickSalt) % kWindowCurtains);
        const std::uint32_t d = windowHash(h ^ kDrawSalt);
        const float r = share(d);
        w.drawn = share(d >> 16) < spec.curtainFull ? 1.0f : spec.drawMin + spec.drawSpan * r * r;
    }
    return w;
}

WindowRay windowRay(const WindowGridSpec& spec, float bayWidth, float storeyHeight, float dAlong, float dUp,
                    float dIn) {
    const float din = std::max(dIn, 1e-4f);
    WindowRay r;
    r.x = std::clamp(dAlong / din * (spec.roomDepth / bayWidth) * spec.viewSquashX, -spec.viewMaxX, spec.viewMaxX);
    r.y = std::clamp(dUp / din * (spec.roomDepth / storeyHeight) * spec.viewSquashY, -spec.viewMaxY, spec.viewMaxY) +
          spec.viewBiasY;
    return r;
}

WindowRoomHit windowRoomHit(const WindowGridSpec& spec, float bayWidth, float storeyHeight, float u, float v,
                            float dAlong, float dUp, float dIn) {
    const WindowRay ray = windowRay(spec, bayWidth, storeyHeight, dAlong, dUp, dIn);
    const float tx = toSide(u, ray.x);
    const float ty = toSide(v, ray.y);
    const float t = std::min(1.0f, std::min(tx, ty));
    WindowRoomHit r;
    if (1.0f <= tx && 1.0f <= ty) r.face = 0;
    else if (ty < tx) r.face = ray.y < 0.0f ? 1 : 2;
    else r.face = ray.x < 0.0f ? 3 : 4;
    const float s = 1.0f / (1.0f + t);
    r.x = 0.5f + (u + t * ray.x - 0.5f) * s;
    r.y = 0.5f + (v + t * ray.y - 0.5f) * s;
    return r;
}

WindowCurtainHit windowCurtainHit(const WindowGridSpec& spec, float bayWidth, float storeyHeight, float windowWidth,
                                  float u, float v, float dAlong, float dUp, float dIn, float drawn) {
    const WindowRay ray = windowRay(spec, bayWidth, storeyHeight, dAlong, dUp, dIn);
    const float t = spec.curtainDepth / spec.roomDepth;
    const WindowOpening o = windowOpening(spec, false, bayWidth, storeyHeight, windowWidth);
    WindowCurtainHit c;
    c.x = std::clamp((u + t * ray.x - o.u0) / std::max(o.u1 - o.u0, 1e-4f), 0.0f, 1.0f);
    c.y = std::clamp((v + t * ray.y - o.v0) / std::max(o.v1 - o.v0, 1e-4f), 0.0f, 1.0f);
    c.covered = c.y >= 1.0f - drawn;
    return c;
}

std::string windowInteriorHlsl(const WindowGridSpec& s) {
    const std::string rooms = unum(kWindowRooms), shops = unum(kWindowShops);
    std::string h;
    h.reserve(10240);
    h += "struct FTWin {\n";
    h += "uint H(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }\n";
    // windowRawRoom and the step past a neighbour's room.
    h += "uint Raw(uint s, uint st, uint c, bool isShop) { uint r = H(H(s + H(st + H(c))) ^ " + unum(kRoomSalt) +
         "); return isShop ? " + rooms + " + r % " + shops + " : r % " + rooms + "; }\n";
    h += "uint Next(uint r, bool isShop) { return isShop ? " + rooms + " + (r - " + rooms + " + 1u) % " + shops +
         " : (r + 1u) % " + rooms + "; }\n";
    h += "};\n";
    h += "FTWin W;\n";
    // The grid (windowFaceGrid, windowCellAt, windowOpening), with no early return: the texture reads below run on every
    // pixel.
    h += "uint seed = (uint)max(Seed, 0.0);\n";
    h += "float ww = max(WinW, " + num(kLeastWindow) + ");\n";
    h += "float nst = max(floor(FaceH / " + num(s.storey) + " + 0.5), 1.0);\n";
    h += "nst = FaceH / nst < " + num(s.storeyMin) + " ? floor(FaceH / " + num(s.storeyMin) + ") : nst;\n";
    h += "float sh = max(min(FaceH / max(nst, 1.0), " + num(s.storeyMax) + "), 0.5);\n";
    h += "float usable = FaceW - " + num(2.0f * s.edge) + ";\n";
    h += "float nb = min(max(floor(usable / " + num(s.bay) + " + 0.5), 1.0), floor(usable / (ww + " +
         num(2.0f * s.pier) + ")));\n";
    h += "float bw = max(usable / max(nb, 1.0), 0.5);\n";
    h += "float stF = floor(max(Up, 0.0) / sh);\n";
    h += "bool inFace = FaceH >= " + num(s.storeyMin) + " && Up >= 0.0 && stF < nst;\n";
    h += "bool band = inFace && Street > 0.5 && stF < 0.5;\n";
    h += "float v = (Up - stF * sh) / sh;\n";
    h += "float shopTop = min(" + num(s.shopTop) + ", sh - " + num(s.fascia) + ");\n";
    h += "bool fascia = band && Up >= shopTop && Up < shopTop + " + num(s.fascia) + ";\n";
    h += "float a = Along - " + num(s.edge) + ";\n";
    h += "float colF = clamp(floor(a / bw), 0.0, max(nb - 1.0, 0.0));\n";
    h += "bool inBay = inFace && nb >= 1.0 && a >= 0.0 && a < nb * bw;\n";
    h += "bool shop = band;\n";
    h += "uint col = (uint)colF;\n";
    h += "uint st = (uint)stF;\n";
    h += "float u = (a - colF * bw) / bw;\n";
    h += "float hw = shop ? " + num(s.shopFill * 0.5f) + " : min(ww, bw - " + num(2.0f * s.pier) + ") / (2.0 * bw);\n";
    h += "float u0 = 0.5 - hw;\n";
    h += "float u1 = 0.5 + hw;\n";
    h += "float v0 = (shop ? " + num(s.shopSill) + " : " + num(s.sill) + ") / sh;\n";
    h += "float v1 = (shop ? shopTop : " + num(s.sill + s.opening) + ") / sh;\n";
    h += "bool inOpen = inBay && u >= u0 && u <= u1 && v >= v0 && v <= v1;\n";
    h += "float fu = " + num(s.frame) + " / bw;\n";
    h += "float fv = " + num(s.frame) + " / sh;\n";
    h += "bool inGlass = inOpen && u >= u0 + fu && u <= u1 - fu && v >= v0 + fv && v <= v1 - fv;\n";
    h += "float part = inGlass ? 1.0 : (inOpen ? 2.0 : (fascia ? 4.0 : (band ? 3.0 : 0.0)));\n";
    // The look (windowRoomFor, windowLookFor).
    h += "uint hh = W.H(seed + W.H(st + W.H(col)));\n";
    h += "uint room = W.Raw(seed, st, col, shop);\n";
    h += "uint nU = W.Raw(seed, st + 1u, col, false);\n";
    h += "uint nD = W.Raw(seed, st - 1u, col, false);\n";
    h += "uint nL = W.Raw(seed, st, col - 1u, shop);\n";
    h += "uint nR = W.Raw(seed, st, col + 1u, shop);\n";
    h += "bool odd = ((st + col) & 1u) != 0u;\n";
    for (std::uint32_t i = 0; i < kWindowAvoidSteps; ++i)
        h += "room = (odd && (room == nU || room == nD || room == nL || room == nR)) ? W.Next(room, shop) : room;\n";
    h += "bool mirror = (W.H(hh ^ " + unum(kMirrorSalt) + ") & 1u) != 0u;\n";
    h += "uint tint = W.H(hh ^ " + unum(kTintSalt) + ") % " + unum(kWindowTints) + ";\n";
    h += "bool lit = float(W.H(hh ^ " + unum(kLitSalt) + ") & 0xFFFFu) / 65536.0 < (shop ? ShopLit : LitShare);\n";
    h += "bool hasCurtain = !shop && float(W.H(hh ^ " + unum(kCurtainSalt) +
         ") & 0xFFFFu) / 65536.0 < CurtainShare;\n";
    h += "uint curtain = W.H(hh ^ " + unum(kPickSalt) + ") % " + unum(kWindowCurtains) + ";\n";
    h += "uint dh = W.H(hh ^ " + unum(kDrawSalt) + ");\n";
    h += "float dr = float(dh & 0xFFFFu) / 65536.0;\n";
    h += "float drawn = float((dh >> 16) & 0xFFFFu) / 65536.0 < " + num(s.curtainFull) + " ? 1.0 : " +
         num(s.drawMin) + " + " + num(s.drawSpan) + " * dr * dr;\n";
    // The eased ray (windowRay) and the room (windowRoomHit).
    h += "float dzv = max(ViewT.z, 1e-4);\n";
    h += "float rx = clamp(ViewT.x / dzv * (" + num(s.roomDepth) + " / bw) * " + num(s.viewSquashX) + ", " +
         num(-s.viewMaxX) + ", " + num(s.viewMaxX) + ");\n";
    h += "float ry = clamp(ViewT.y / dzv * (" + num(s.roomDepth) + " / sh) * " + num(s.viewSquashY) + ", " +
         num(-s.viewMaxY) + ", " + num(s.viewMaxY) + ") + (" + num(s.viewBiasY) + ");\n";
    h += "float tx = rx > 0.0 ? (1.0 - u) / rx : (rx < 0.0 ? -u / rx : 1e30);\n";
    h += "float ty = ry > 0.0 ? (1.0 - v) / ry : (ry < 0.0 ? -v / ry : 1e30);\n";
    h += "float t = min(1.0, min(tx, ty));\n";
    h += "float sc = 1.0 / (1.0 + t);\n";
    h += "float px = 0.5 + (u + t * rx - 0.5) * sc;\n";
    h += "float py = 0.5 + (v + t * ry - 0.5) * sc;\n";
    h += "px = mirror ? 1.0 - px : px;\n";
    h += "px = clamp(px, 0.002, 0.998);\n";
    h += "py = clamp(py, 0.002, 0.998);\n";
    h += "float2 ruv = float2((float(room % " + unum(kWindowAtlasCols) + ") + px) / " + num(kWindowAtlasCols) +
         ", (float(room / " + unum(kWindowAtlasCols) + ") + 1.0 - py) / " + num(kWindowAtlasRows) + ");\n";
    h += "float3 c = Texture2DSample(Rooms, RoomsSampler, ruv).rgb;\n";
    // The curtain (windowCurtainHit), over the window's rectangle; it hides at most CurtainAlpha of the room.
    h += "float tc = " + num(s.curtainDepth / s.roomDepth) + ";\n";
    h += "float cx = saturate((u + tc * rx - u0) / max(u1 - u0, 1e-4));\n";
    h += "float cy = saturate((v + tc * ry - v0) / max(v1 - v0, 1e-4));\n";
    h += "float2 cuv = float2((float(curtain) + clamp(cx, 0.002, 0.998)) / " + num(kWindowCurtains) +
         ", 1.0 - clamp(cy, 0.002, 0.998));\n";
    h += "float4 cc = Texture2DSample(Curtains, CurtainsSampler, cuv);\n";
    h += "bool covered = hasCurtain && cy >= 1.0 - drawn;\n";
    h += "float ca = covered ? cc.a * CurtainAlpha : 0.0;\n";
    h += "Curtain = float4(cc.rgb, ca);\n";
    h += "float3 seen = c * (1.0 - ca);\n";
    // The opening's own light: the room's by day, fading with Night; by night a lit room's in its tint, and its
    // curtain's, weaker. A curtain is never lit on its own.
    h += "float3 tints[" + std::to_string(kWindowTints) +
         "] = { float3(1.0, 0.78, 0.52), float3(1.0, 0.92, 0.80), float3(0.82, 0.90, 1.0), float3(1.0, 0.66, 0.42) };\n";
    h += "float on = (inGlass && lit) ? Night : 0.0;\n";
    h += "float3 day = seen * (shop ? ShopDay : RoomDay) * (1.0 - Night);\n";
    h += "float3 night = on * (seen + cc.rgb * ca * CurtainGlow) * tints[tint] * (shop ? ShopGlow : GlowScale);\n";
    h += "Emit = inGlass ? day + night : float3(0.0, 0.0, 0.0);\n";
    h += "return float4(seen, part);\n";
    return h;
}

}  // namespace ftcore
