#include "ftcore/WindowFacade.h"

#include "ftcore/ShopFront.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ftcore {

namespace {

constexpr std::uint32_t kJitterSalt = 0x3c6ef372u;
constexpr std::uint32_t kKindSalt = 0xa54ff53au;
constexpr std::uint32_t kKindPickSalt = 0x510e527fu;
constexpr std::uint32_t kBandSalt = 0x9b05688cu;
constexpr std::uint32_t kWallTintSalt = 0x5be0cd19u;
constexpr std::uint32_t kStyleSalt = 0x6a09e667u;
constexpr std::uint32_t kParapetSalt = 0xbb67ae85u;
constexpr float kLeast = 0.3f;  // as WindowInterior's least window width
constexpr float kNever = 1e30f;
constexpr float kLoggiaFloor = 0.05f;  // m: a loggia's opening starts this far over the storey line (the slab's edge)

// Apartment, walkway, shophouse, office; Legacy is WindowGridSpec's own numbers.
constexpr WindowTypeSpec kTypes[kWindowTypes] = {
    {3.0f, 2.5f, 1.35f, 0.85f, 1.45f, 0.25f, 0.0f, false, false, 0.06f},
    {2.9f, 2.6f, 1.5f, 0.9f, 1.35f, 0.25f, 0.20f, false, false, 0.09f},
    {2.8f, 3.2f, 2.4f, 0.95f, 1.0f, 0.2f, 0.15f, false, true, 0.09f},
    {3.2f, 2.2f, 0.9f, 0.5f, 1.9f, 0.3f, 0.24f, false, false, 0.09f},
    {3.6f, 1.8f, 1.72f, 0.8f, 1.9f, 0.04f, 0.12f, true, true, 0.08f},
};

// Style weights per type (plain, mullion, grille, loggia, small), out of kWindowStyleWeightTotal.
constexpr std::uint32_t kStyleWeights[kWindowTypes][kWindowStyles] = {
    {16, 0, 0, 0, 0}, {4, 5, 4, 2, 1}, {6, 4, 4, 0, 2}, {3, 4, 4, 3, 2}, {7, 6, 0, 0, 3},
};
constexpr std::uint32_t weightRow(std::uint32_t t) {
    std::uint32_t c = 0;
    for (std::uint32_t k = 0; k < kWindowStyles; ++k) c += kStyleWeights[t][k];
    return c;
}
static_assert(weightRow(0) == kWindowStyleWeightTotal && weightRow(1) == kWindowStyleWeightTotal &&
              weightRow(2) == kWindowStyleWeightTotal && weightRow(3) == kWindowStyleWeightTotal &&
              weightRow(4) == kWindowStyleWeightTotal);
// The cumulative weights a column's hash (mod the total) is counted against: its style is how many of the first four
// it reaches.
constexpr std::uint32_t styleEdge(std::uint32_t t, std::uint32_t i) {
    std::uint32_t c = 0;
    for (std::uint32_t k = 0; k <= i; ++k) c += kStyleWeights[t][k];
    return c;
}
std::uint32_t styleRoll(std::uint32_t seed, std::uint32_t column) {
    return windowHash(windowHash(seed ^ kStyleSalt) + windowHash(column)) % kWindowStyleWeightTotal;
}
std::uint32_t typeIndex(WindowType type) {
    const std::uint32_t t = static_cast<std::uint32_t>(type);
    return t < kWindowTypes ? t : 0u;
}

constexpr WindowTint kWallTints[kWindowWallTintCount] = {
    {1.0f, 1.0f, 1.0f, 0.0f, false},     // as photographed
    {0.80f, 0.78f, 0.73f, 0.65f, false}, // off-white
    {0.66f, 0.58f, 0.46f, 0.6f, false},  // tile beige
    {0.50f, 0.47f, 0.43f, 0.5f, false},  // warm grey
    {0.56f, 0.68f, 0.58f, 0.6f, true},   // pastel green
    {0.55f, 0.64f, 0.74f, 0.6f, true},   // pastel blue
    {0.52f, 0.27f, 0.17f, 0.55f, true},  // rust
    {0.30f, 0.30f, 0.28f, 0.5f, false},  // stained concrete
    {0.78f, 0.56f, 0.48f, 0.6f, true},   // salmon
    {0.80f, 0.74f, 0.52f, 0.6f, true},   // pale yellow
};
constexpr std::uint32_t kNeutralTints[] = {0, 1, 2, 3, 7};

constexpr WindowTint kRoofFinishes[kWindowRoofFinishCount] = {
    {0.55f, 0.54f, 0.51f, 0.8f, false},  // light concrete
    {0.07f, 0.07f, 0.075f, 0.9f, false}, // dark tar
    {0.18f, 0.33f, 0.24f, 0.85f, false}, // green paint
    {0.36f, 0.12f, 0.08f, 0.85f, false}, // red-oxide paint
    {0.33f, 0.33f, 0.32f, 0.7f, false},  // weathered grey
    {0.45f, 0.50f, 0.55f, 0.8f, false},  // pale blue-grey
};

struct Rgb {
    float r, g, b;
};
constexpr Rgb kBand[kWindowBandColorCount] = {{0.30f, 0.30f, 0.29f}, {0.26f, 0.15f, 0.09f}, {0.09f, 0.18f, 0.13f},
                                              {0.10f, 0.10f, 0.11f}, {0.52f, 0.47f, 0.39f}, {0.42f, 0.20f, 0.12f}};

float toSide(float x, float d) {
    if (d > 0.0f) return (1.0f - x) / d;
    if (d < 0.0f) return -x / d;
    return kNever;
}

float share(std::uint32_t bits16) { return static_cast<float>(bits16 & 0xFFFFu) / 65536.0f; }

std::string num(float f) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(f));
    std::string s(buf);
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}
std::string unum(std::uint32_t u) { return std::to_string(u) + "u"; }
std::string rgb(float r, float g, float b) { return "float3(" + num(r) + ", " + num(g) + ", " + num(b) + ")"; }

template <class T, std::size_t N, class F>
std::string array(const char* type, const char* name, const T (&items)[N], F each) {
    std::string s = std::string(type) + " " + name + "[" + std::to_string(N) + "] = { ";
    for (std::size_t i = 0; i < N; ++i) s += (i ? ", " : "") + each(items[i]);
    return s + " };\n";
}

}  // namespace

WindowTypeSpec windowTypeSpec(WindowType type) {
    const std::uint32_t t = static_cast<std::uint32_t>(type);
    return kTypes[t < kWindowTypes ? t : 0u];
}

WindowGridSpec windowGridFor(const WindowFacadeSpec& spec, WindowType type) {
    WindowGridSpec g = spec.base;
    if (type == WindowType::Legacy) return g;
    const WindowTypeSpec t = windowTypeSpec(type);
    g.storey = t.storey;
    g.bay = t.bay;
    g.windowWidth = t.width;
    g.sill = t.sill;
    g.opening = t.opening;
    g.pier = t.pier;
    return g;
}

float windowWidthFor(const WindowFacadeSpec& spec, WindowType type, std::uint32_t seed, float materialWidth) {
    if (type == WindowType::Legacy) return std::max(materialWidth, kLeast);
    const WindowTypeSpec t = windowTypeSpec(type);
    if (t.ribbon) return std::max(t.bay - 2.0f * t.pier, kLeast);
    const float jit = 1.0f - spec.widthJitter + 2.0f * spec.widthJitter * share(windowHash(seed ^ kJitterSalt));
    return std::max(t.width * jit, kLeast);
}

std::uint32_t windowStyleWeight(WindowType type, WindowStyle style) {
    const std::uint32_t s = static_cast<std::uint32_t>(style);
    return s < kWindowStyles ? kStyleWeights[typeIndex(type)][s] : 0u;
}

WindowStyle windowColumnStyle(WindowType type, std::uint32_t seed, std::uint32_t column) {
    const std::uint32_t t = typeIndex(type), r = styleRoll(seed, column);
    std::uint32_t s = 0;
    for (std::uint32_t i = 0; i + 1 < kWindowStyles; ++i) s += r >= styleEdge(t, i) ? 1u : 0u;
    return static_cast<WindowStyle>(s);
}

WindowStyle windowStyleFor(WindowType type, std::uint32_t seed, std::uint32_t column, std::uint32_t storey, bool shop) {
    if (shop) return WindowStyle::Plain;
    const WindowStyle s = windowColumnStyle(type, seed, column);
    if (s == WindowStyle::Loggia && (storey < 1u || type == WindowType::Office)) return WindowStyle::Plain;
    return s;
}

WindowOpening windowStyleOpening(const WindowFacadeSpec& spec, WindowType type, WindowStyle style, float bayWidth,
                                 float storeyHeight, float windowWidth) {
    WindowOpening o;
    if (!(bayWidth > 0.0f) || !(storeyHeight > 0.0f)) return o;
    const WindowGridSpec g = windowGridFor(spec, type);
    const bool ribbon = windowTypeSpec(type).ribbon && type != WindowType::Legacy;
    const bool loggia = style == WindowStyle::Loggia, smallWin = style == WindowStyle::Small && !ribbon;
    const float full = bayWidth - 2.0f * g.pier;
    float hw = ((ribbon || loggia) ? full : std::min(std::max(windowWidth, kLeast), full)) / (2.0f * bayWidth);
    if (smallWin) hw = std::min(hw, spec.smallWidth / (2.0f * bayWidth));
    o.u0 = 0.5f - hw;
    o.u1 = 0.5f + hw;
    o.v1 = (g.sill + g.opening) / storeyHeight;
    o.v0 = (loggia ? kLoggiaFloor : g.sill) / storeyHeight;
    if (smallWin) o.v0 = std::max(o.v0, o.v1 - spec.smallHeight / storeyHeight);
    return o;
}

float windowStyleReveal(const WindowFacadeSpec& spec, WindowType type, WindowStyle style) {
    if (type == WindowType::Legacy) return 0.0f;
    return style == WindowStyle::Loggia ? spec.loggiaDepth : windowTypeSpec(type).reveal;
}

float windowLitShare(const WindowFacadeSpec& spec, WindowType type, float litShare, float upperLit,
                     std::uint32_t storey, std::uint32_t storeys) {
    if (type == WindowType::Legacy) return litShare;
    const float f = std::clamp((static_cast<float>(storey) - 1.0f) / std::max(static_cast<float>(storeys) - 2.0f, 1.0f),
                               0.0f, 1.0f);
    return std::min(litShare * (1.0f + upperLit * f), spec.litMax);
}

WindowRay windowFacadeRay(const WindowFacadeSpec& spec, const WindowOpening& o, float bayWidth, float storeyHeight,
                          float dAlong, float dUp, float dIn) {
    WindowRay r = windowRay(spec.base, bayWidth, storeyHeight, dAlong, dUp, dIn);
    const float side = std::min(spec.base.viewMaxX, std::min(o.u0, 1.0f - o.u1) + spec.sideShare * (o.u1 - o.u0));
    r.x = std::clamp(r.x, -side, side);
    return r;
}

WindowRoomHit windowFacadeRoomHit(const WindowFacadeSpec& spec, const WindowOpening& o, float bayWidth,
                                  float storeyHeight, float u, float v, float dAlong, float dUp, float dIn) {
    const WindowRay ray = windowFacadeRay(spec, o, bayWidth, storeyHeight, dAlong, dUp, dIn);
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

WindowRevealHit windowReveal(float frame, float reveal, const WindowOpening& o, float bayWidth, float storeyHeight,
                             float u, float v, float dAlong, float dUp, float dIn) {
    WindowRevealHit h;
    h.gu = u;
    h.gv = v;
    if (u < o.u0 || u > o.u1 || v < o.v0 || v > o.v1 || !(bayWidth > 0.0f) || !(storeyHeight > 0.0f)) return h;
    const float din = std::max(dIn, 1e-4f);
    const float ow = (o.u1 - o.u0) * bayWidth, oh = (o.v1 - o.v0) * storeyHeight;
    const float sx = std::clamp(dAlong / din * reveal, -ow, ow);
    const float sy = std::clamp(dUp / din * reveal, -oh, oh);
    h.gu = u + sx / bayWidth;
    h.gv = v + sy / storeyHeight;
    const float fx = sx > 0.0f ? (o.u1 - u) * bayWidth / sx : (sx < 0.0f ? (o.u0 - u) * bayWidth / sx : kNever);
    const float fy = sy > 0.0f ? (o.v1 - v) * storeyHeight / sy : (sy < 0.0f ? (o.v0 - v) * storeyHeight / sy : kNever);
    const bool out = h.gu < o.u0 || h.gu > o.u1 || h.gv < o.v0 || h.gv > o.v1;
    if (out) {
        if (fx <= fy) h.part = sx > 0.0f ? WindowFacadePart::JambMinus : WindowFacadePart::JambPlus;
        else h.part = sy < 0.0f ? WindowFacadePart::RevealBottom : WindowFacadePart::RevealHead;
        return h;
    }
    const float fu = frame / bayWidth, fv = frame / storeyHeight;
    const bool glass = h.gu >= o.u0 + fu && h.gu <= o.u1 - fu && h.gv >= o.v0 + fv && h.gv <= o.v1 - fv;
    h.part = glass ? WindowFacadePart::Glass : WindowFacadePart::Frame;
    return h;
}

std::uint32_t windowShopRawKind(std::uint32_t seed, std::uint32_t column) {
    return windowHash(windowHash(seed ^ kKindSalt) + windowHash(column)) % kWindowShopKinds;
}

WindowShopKind windowShopKind(std::uint32_t seed, std::uint32_t column) {
    std::uint32_t k = windowShopRawKind(seed, column);
    if ((column & 1u) != 0u) {
        const std::uint32_t l = windowShopRawKind(seed, column - 1u), r = windowShopRawKind(seed, column + 1u);
        for (int i = 0; i < 2; ++i)
            if (k == l || k == r) k = (k + 1u) % kWindowShopKinds;
    }
    return static_cast<WindowShopKind>(k);
}

std::uint32_t windowShopPicture(WindowShopKind kind, std::uint32_t seed, std::uint32_t column) {
    switch (kind) {
    case WindowShopKind::Glazed:
        return kWindowRooms + ((windowHash(seed ^ windowHash(column + kKindPickSalt)) & 1u) != 0u ? 3u : 0u);
    case WindowShopKind::Cafe:
    case WindowShopKind::Eatery: return kWindowRooms + 1u;
    case WindowShopKind::Clinic: return kWindowRooms + 2u;
    case WindowShopKind::Laundromat: return kWindowRooms + 3u;
    case WindowShopKind::Shutter: return kWindowRooms;
    }
    return kWindowRooms;
}

const WindowTint& windowWallTint(std::uint32_t index) { return kWallTints[index < kWindowWallTintCount ? index : 0u]; }
const WindowTint& windowRoofFinish(std::uint32_t index) {
    return kRoofFinishes[index < kWindowRoofFinishCount ? index : 0u];
}

std::uint32_t windowWallTintFor(std::uint32_t seed, bool plainFinish) {
    const std::uint32_t h = windowHash(seed ^ kWallTintSalt);
    if (plainFinish) return h % kWindowWallTintCount;
    return kNeutralTints[h % (sizeof kNeutralTints / sizeof kNeutralTints[0])];
}

std::string windowFacadeFaceHlsl() {
    return R"HLSL(float Sx = length(AxisX);
float Sy = length(AxisY);
float Sz = length(AxisZ);
float3 Nx = AxisX / max(Sx, 1e-6);
float3 Ny = AxisY / max(Sy, 1e-6);
float3 Nz = AxisZ / max(Sz, 1e-6);
float3 L = LocalPos / 100.0 * float3(Sx, Sy, Sz);
float3 N = normalize(VertexNormal);
float nx = dot(N, Nx);
float ny = dot(N, Ny);
float nz = dot(N, Nz);
int F = abs(nx) >= abs(ny) ? (nx > 0.0 ? 0 : 2) : (ny < 0.0 ? 1 : 3);
float3 Out = F == 0 ? Nx : (F == 1 ? -Ny : (F == 2 ? -Nx : Ny));
float3 T = F == 0 ? Ny : (F == 1 ? Nx : (F == 2 ? -Ny : -Nx));
float Across = F == 0 ? L.y : (F == 1 ? L.x : (F == 2 ? -L.y : -L.x));
float Width = (F == 0 || F == 2) ? Sy : Sx;
float Base = F == 0 ? Base0 : (F == 1 ? Base1 : (F == 2 ? Base2 : Base3));
StreetOut = fmod(floor(floor(StreetBits + 0.5) / exp2((float)F)), 2.0);
UpOut = L.z + 0.5 * Sz - Base;
FaceHOut = abs(nz) < 0.5 ? Sz - Base : 0.0;
FaceWOut = Width;
float3 V = -CameraVector;
ViewOut = float3(dot(V, T), dot(V, Nz), -dot(V, Out));
TOut = T;
NzOut = Nz;
TopOut = nz > 0.5 ? 1.0 : 0.0;
return Across + 0.5 * Width;)HLSL";
}

std::string windowFacadeHlsl(const WindowFacadeSpec& spec) {
    const WindowGridSpec& s = spec.base;
    const std::string rooms = unum(kWindowRooms), shops = unum(kWindowShops), kinds = unum(kWindowShopKinds);
    std::string h;
    h.reserve(16384);
    h += "struct FTWin {\n";
    h += "uint H(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }\n";
    h += "uint Raw(uint s, uint st, uint c, bool isShop) { uint r = H(H(s + H(st + H(c))) ^ " +
         unum(0x68bc21ebu) + "); return isShop ? " + rooms + " + r % " + shops + " : r % " + rooms + "; }\n";
    h += "uint Next(uint r, bool isShop) { return isShop ? " + rooms + " + (r - " + rooms + " + 1u) % " + shops +
         " : (r + 1u) % " + rooms + "; }\n";
    h += "uint KRaw(uint s, uint c) { return H(H(s ^ " + unum(kKindSalt) + ") + H(c)) % " + kinds + "; }\n";
    h += "};\n";
    h += "FTWin W;\n";
    h += "uint seed = (uint)max(Seed, 0.0);\n";
    h += "uint ty = (uint)clamp(floor(Type + 0.5), 0.0, " + num(static_cast<float>(kWindowTypes - 1)) + ");\n";
    // The type table (windowTypeSpec); Legacy's row is the base spec's, its width the material's.
    WindowTypeSpec rows[kWindowTypes];
    for (std::uint32_t t = 0; t < kWindowTypes; ++t) rows[t] = windowTypeSpec(static_cast<WindowType>(t));
    rows[0].storey = s.storey;
    rows[0].bay = s.bay;
    rows[0].sill = s.sill;
    rows[0].opening = s.opening;
    rows[0].pier = s.pier;
    rows[0].frame = s.frame;
    h += array("float", "kSt", rows, [](const WindowTypeSpec& r) { return num(r.storey); });
    h += array("float", "kBay", rows, [](const WindowTypeSpec& r) { return num(r.bay); });
    h += array("float", "kW", rows, [](const WindowTypeSpec& r) { return num(r.width); });
    h += array("float", "kSill", rows, [](const WindowTypeSpec& r) { return num(r.sill); });
    h += array("float", "kOpen", rows, [](const WindowTypeSpec& r) { return num(r.opening); });
    h += array("float", "kPier", rows, [](const WindowTypeSpec& r) { return num(r.pier); });
    h += array("float", "kRev", rows, [](const WindowTypeSpec& r) { return num(r.reveal); });
    h += array("float", "kRib", rows, [](const WindowTypeSpec& r) { return num(r.ribbon ? 1.0f : 0.0f); });
    h += array("float", "kPl", rows, [](const WindowTypeSpec& r) { return num(r.plinth ? 1.0f : 0.0f); });
    h += array("float", "kFr", rows, [](const WindowTypeSpec& r) { return num(r.frame); });
    std::uint32_t edges[kWindowTypes * (kWindowStyles - 1)];
    for (std::uint32_t t = 0; t < kWindowTypes; ++t)
        for (std::uint32_t i = 0; i + 1 < kWindowStyles; ++i) edges[t * (kWindowStyles - 1) + i] = styleEdge(t, i);
    h += array("uint", "kStyE", edges, [](std::uint32_t e) { return unum(e); });
    h += "float pSt = kSt[ty];\n";
    h += "float pBay = kBay[ty];\n";
    h += "float pSill = kSill[ty];\n";
    h += "float pOpen = kOpen[ty];\n";
    h += "float pPier = kPier[ty];\n";
    h += "bool ribbon = kRib[ty] > 0.5;\n";
    h += "float jit = 1.0 - " + num(spec.widthJitter) + " + " + num(2.0f * spec.widthJitter) + " * float(W.H(seed ^ " +
         unum(kJitterSalt) + ") & 0xFFFFu) / 65536.0;\n";
    h += "float ww = ty == 0u ? max(WinW, " + num(kLeast) + ") : max(ribbon ? pBay - 2.0 * pPier : kW[ty] * jit, " +
         num(kLeast) + ");\n";
    // The grid (windowFaceGrid with the type's numbers), with no early return.
    h += "float nst = max(floor(FaceH / pSt + 0.5), 1.0);\n";
    h += "nst = FaceH / nst < " + num(s.storeyMin) + " ? floor(FaceH / " + num(s.storeyMin) + ") : nst;\n";
    h += "float sh = max(min(FaceH / max(nst, 1.0), " + num(s.storeyMax) + "), 0.5);\n";
    h += "float usable = FaceW - " + num(2.0f * s.edge) + ";\n";
    h += "float nb = min(max(floor(usable / pBay + 0.5), 1.0), floor(usable / (ww + 2.0 * pPier)));\n";
    h += "float bw = max(usable / max(nb, 1.0), 0.5);\n";
    h += "float stF = floor(max(Up, 0.0) / sh);\n";
    h += "bool inFace = FaceH >= " + num(s.storeyMin) + " && Up >= 0.0 && stF < nst;\n";
    h += "bool plinth = ty > 0u && (kPl[ty] > 0.5 || FaceH >= " + num(spec.plinthFrom) + ");\n";
    h += "bool shop = inFace && Street > 0.5 && stF < 0.5;\n";
    h += "bool band = inFace && stF < 0.5 && (Street > 0.5 || plinth);\n";
    h += "float v = (Up - stF * sh) / sh;\n";
    h += "float shopTop = min(" + num(s.shopTop) + ", sh - " + num(s.fascia) + ");\n";
    h += "bool fascia = shop && Up >= shopTop && Up < shopTop + " + num(s.fascia) + ";\n";
    h += "bool course = band && plinth && Up >= sh - " + num(spec.course) + " && Up < sh;\n";
    h += "float a = Along - " + num(s.edge) + ";\n";
    h += "float colF = clamp(floor(a / bw), 0.0, max(nb - 1.0, 0.0));\n";
    h += "bool inBay = inFace && nb >= 1.0 && a >= 0.0 && a < nb * bw;\n";
    h += "uint col = (uint)colF;\n";
    h += "uint st = (uint)stF;\n";
    h += "float u = (a - colF * bw) / bw;\n";
    // The column's style (windowStyleFor) and the opening it cuts (windowStyleOpening).
    h += "uint sr = W.H(W.H(seed ^ " + unum(kStyleSalt) + ") + W.H(col)) % " + unum(kWindowStyleWeightTotal) + ";\n";
    h += "uint sty = 0u;\n";
    for (std::uint32_t i = 0; i + 1 < kWindowStyles; ++i)
        h += "sty += sr >= kStyE[ty * " + unum(kWindowStyles - 1) + " + " + unum(i) + "] ? 1u : 0u;\n";
    h += "sty = shop || (sty == 3u && (st < 1u || ty == 4u)) ? 0u : sty;\n";
    h += "bool loggia = sty == 3u;\n";
    h += "bool smallWin = sty == 4u && !ribbon;\n";
    h += "bool frostLow = sty == 4u && ribbon;\n";
    h += "float hw = shop ? " + num(s.shopFill * 0.5f) +
         " : ((ribbon || loggia) ? bw - 2.0 * pPier : min(ww, bw - 2.0 * pPier)) / (2.0 * bw);\n";
    h += "hw = smallWin ? min(hw, " + num(spec.smallWidth * 0.5f) + " / bw) : hw;\n";
    h += "float u0 = 0.5 - hw;\n";
    h += "float u1 = 0.5 + hw;\n";
    // The shop front's kind, door and sill (ShopFront.h).
    h += shopFrontPreHlsl(ShopFrontSpec{});
    h += "float v0 = (shop ? shopSill : (loggia ? " + num(kLoggiaFloor) + " : pSill)) / sh;\n";
    h += "float v1 = (shop ? shopTop : pSill + pOpen) / sh;\n";
    h += "v0 = smallWin ? max(v0, v1 - " + num(spec.smallHeight) + " / sh) : v0;\n";
    h += "bool inOpen = inBay && u >= u0 && u <= u1 && v >= v0 && v <= v1;\n";
    // The reveal (windowReveal): the glass a reveal deep, the jambs, bottom and head between.
    h += "float dzv = max(ViewT.z, 1e-4);\n";
    h += "float rev = shop ? " + num(spec.shopReveal) + " : (loggia ? " + num(spec.loggiaDepth) + " : kRev[ty]);\n";
    h += "float sxm = clamp(ViewT.x / dzv * rev, -(u1 - u0) * bw, (u1 - u0) * bw);\n";
    h += "float sym = clamp(ViewT.y / dzv * rev, -(v1 - v0) * sh, (v1 - v0) * sh);\n";
    h += "float gu = u + sxm / bw;\n";
    h += "float gv = v + sym / sh;\n";
    h += "float fxr = sxm > 0.0 ? (u1 - u) * bw / sxm : (sxm < 0.0 ? (u0 - u) * bw / sxm : 1e30);\n";
    h += "float fyr = sym > 0.0 ? (v1 - v) * sh / sym : (sym < 0.0 ? (v0 - v) * sh / sym : 1e30);\n";
    h += "bool outG = gu < u0 || gu > u1 || gv < v0 || gv > v1;\n";
    h += "float revPart = fxr <= fyr ? (sxm > 0.0 ? 6.0 : 5.0) : (sym < 0.0 ? 7.0 : 8.0);\n";
    h += "float fr = kFr[ty];\n";
    h += "float fu = fr / bw;\n";
    h += "float fv = fr / sh;\n";
    h += "bool glassG = inOpen && !outG && gu >= u0 + fu && gu <= u1 - fu && gv >= v0 + fv && gv <= v1 - fv;\n";
    // A mullion (where a loggia's doors meet too) and a transom.
    h += "bool mull = !shop && (sty == 1u || loggia) && abs(gu - 0.5) * bw < 0.5 * fr;\n";
    h += "mull = mull || (sty == 1u && abs(gv - (v1 - 0.3 * (v1 - v0))) * sh < 0.5 * fr);\n";
    h += "glassG = glassG && !mull;\n";
    h += "bool inLip = inBay && u >= u0 - " + num(spec.lipOut) + " / bw && u <= u1 + " + num(spec.lipOut) + " / bw;\n";
    h += "bool lip = !shop && !loggia && inLip && rev > 0.0 && v < v0 && v >= v0 - " + num(spec.lipHeight) + " / sh;\n";
    h += "bool sillSh = !shop && !loggia && inLip && rev > 0.0 && v < v0 - " + num(spec.lipHeight) +
         " / sh && v >= v0 - " + num(spec.lipHeight + spec.sillShadow) + " / sh;\n";
    h += "float part = inOpen ? (outG ? revPart : (glassG ? 1.0 : 2.0)) : (lip ? 9.0 : (sillSh ? 20.0 : (course ? 15.0 : "
         "(fascia ? 4.0 : (band ? 3.0 : 0.0)))));\n";
    // A grille's bars at the wall's face over the opening; a loggia's rail (or, one building in four, a parapet with a
    // concrete cap) at the face over its floor; a small window's glass frosted, a ribbon's lower part.
    h += "bool grille = sty == 2u && inLip && v >= v0 && v <= v1 && (frac((u - u0) * bw / " + num(spec.grillePitch) +
         ") < 0.25 || frac((v - v0) * sh / " + num(2.0f * spec.grillePitch) + ") < 0.12);\n";
    h += "part = grille ? 17.0 : part;\n";
    h += "float ry0 = (v - v0) * sh;\n";
    h += "bool railZ = loggia && inOpen && ry0 < " + num(spec.railHeight) + ";\n";
    h += "bool railTop = ry0 > " + num(spec.railHeight - 0.06f) + ";\n";
    h += "bool parapet = (W.H(seed ^ " + unum(kParapetSalt) + ") & 3u) == 0u;\n";
    h += "bool bal = frac(u * bw / " + num(spec.baluster) + ") < 0.3;\n";
    h += "part = railZ ? (parapet ? (railTop ? 9.0 : 0.0) : (railTop || bal ? 18.0 : part)) : part;\n";
    h += "part = part == 1.0 && !shop && (smallWin || (frostLow && gv < v0 + 0.4 * (v1 - v0))) ? 13.0 : part;\n";
    // The shop's front at the glass plane, its fascia, sign and lantern (ShopFront.h).
    h += shopFrontHlsl(ShopFrontSpec{}, s.fascia);
    // The band: one colour a building, with tile courses; the string course and the lip, concrete.
    h += array("float3", "kBand", kBand, [](const Rgb& c) { return rgb(c.r, c.g, c.b); });
    h += "float3 bandRgb = kBand[W.H(seed ^ " + unum(kBandSalt) + ") % " + unum(kWindowBandColorCount) + "] * (frac(Up / " +
         num(spec.tileCourse) + ") < 0.06 ? 0.75 : 1.0);\n";
    // The look (windowRoomFor, windowLookFor); a shop shows its kind's picture.
    h += "uint hh = W.H(seed + W.H(st + W.H(col)));\n";
    h += "uint room = W.Raw(seed, st, col, shop);\n";
    h += "uint nU = W.Raw(seed, st + 1u, col, false);\n";
    h += "uint nD = W.Raw(seed, st - 1u, col, false);\n";
    h += "uint nL = W.Raw(seed, st, col - 1u, shop);\n";
    h += "uint nR = W.Raw(seed, st, col + 1u, shop);\n";
    h += "bool odd = ((st + col) & 1u) != 0u;\n";
    for (std::uint32_t i = 0; i < kWindowAvoidSteps; ++i)
        h += "room = (odd && (room == nU || room == nD || room == nL || room == nR)) ? W.Next(room, shop) : room;\n";
    h += "uint pick = shopPick;\n";
    h += "room = shop ? " + rooms + " + pick : room;\n";
    h += "bool mirror = (W.H(hh ^ " + unum(0x02e5be93u) + ") & 1u) != 0u;\n";
    h += "uint tint = W.H(hh ^ " + unum(0x967a889bu) + ") % " + unum(kWindowTints) + ";\n";
    // Upper storeys lit more often (windowLitShare).
    h += "float litP = shop ? ShopLit : (ty == 0u ? LitShare : min(LitShare * (1.0 + UpperLit * saturate((stF - 1.0) / "
         "max(nst - 2.0, 1.0))), " + num(spec.litMax) + "));\n";
    h += "bool lit = float(W.H(hh ^ " + unum(0x5bd1e995u) + ") & 0xFFFFu) / 65536.0 < litP;\n";
    h += "bool hasCurtain = !shop && float(W.H(hh ^ " + unum(0x1b873593u) + ") & 0xFFFFu) / 65536.0 < CurtainShare;\n";
    h += "uint curtain = W.H(hh ^ " + unum(0xcc9e2d51u) + ") % " + unum(kWindowCurtains) + ";\n";
    h += "uint dh = W.H(hh ^ " + unum(0x85ebca6bu) + ");\n";
    h += "float dr = float(dh & 0xFFFFu) / 65536.0;\n";
    h += "float drawn = float((dh >> 16) & 0xFFFFu) / 65536.0 < " + num(s.curtainFull) + " ? 1.0 : " + num(s.drawMin) +
         " + " + num(s.drawSpan) + " * dr * dr;\n";
    // The eased ray with the side clamp (windowFacadeRay) and the room (windowFacadeRoomHit), from the glass point.
    h += "float rxMax = min(" + num(s.viewMaxX) + ", min(u0, 1.0 - u1) + " + num(spec.sideShare) + " * (u1 - u0));\n";
    h += "float rx = clamp(ViewT.x / dzv * (" + num(s.roomDepth) + " / bw) * " + num(s.viewSquashX) +
         ", -rxMax, rxMax);\n";
    h += "float ry = clamp(ViewT.y / dzv * (" + num(s.roomDepth) + " / sh) * " + num(s.viewSquashY) + ", " +
         num(-s.viewMaxY) + ", " + num(s.viewMaxY) + ") + (" + num(s.viewBiasY) + ");\n";
    h += "float tx = rx > 0.0 ? (1.0 - gu) / rx : (rx < 0.0 ? -gu / rx : 1e30);\n";
    h += "float ty2 = ry > 0.0 ? (1.0 - gv) / ry : (ry < 0.0 ? -gv / ry : 1e30);\n";
    h += "float t = min(1.0, min(tx, ty2));\n";
    h += "float sc = 1.0 / (1.0 + t);\n";
    h += "float px = 0.5 + (gu + t * rx - 0.5) * sc;\n";
    h += "float py = 0.5 + (gv + t * ry - 0.5) * sc;\n";
    h += "px = mirror ? 1.0 - px : px;\n";
    h += "px = shop ? shopShift + px * shopZoom : px;\n";
    h += "px = clamp(px, 0.002, 0.998);\n";
    h += "py = clamp(py, 0.002, 0.998);\n";
    h += "float2 ruv = float2((float(room % " + unum(kWindowAtlasCols) + ") + px) / " + num(kWindowAtlasCols) +
         ", (float(room / " + unum(kWindowAtlasCols) + ") + 1.0 - py) / " + num(kWindowAtlasRows) + ");\n";
    h += "float3 c = Texture2DSample(Rooms, RoomsSampler, ruv).rgb;\n";
    h += "float tc = " + num(s.curtainDepth / s.roomDepth) + ";\n";
    h += "float cx = saturate((gu + tc * rx - u0) / max(u1 - u0, 1e-4));\n";
    h += "float cy = saturate((gv + tc * ry - v0) / max(v1 - v0, 1e-4));\n";
    h += "float2 cuv = float2((float(curtain) + clamp(cx, 0.002, 0.998)) / " + num(kWindowCurtains) +
         ", 1.0 - clamp(cy, 0.002, 0.998));\n";
    h += "float4 cc = Texture2DSample(Curtains, CurtainsSampler, cuv);\n";
    h += "bool glass = part == 1.0;\n";
    h += "bool covered = glass && hasCurtain && cy >= 1.0 - drawn;\n";
    h += "float ca = covered ? cc.a * CurtainAlpha : 0.0;\n";
    h += "Curtain = float4(cc.rgb, ca);\n";
    h += "float3 seen = c * (1.0 - ca);\n";
    h += windowTintTableHlsl();
    h += "float on = (glass && lit) ? Night : 0.0;\n";
    // By day the glass darkens toward its head, in the reveal's shade.
    h += "float hs = (ty > 0u && !shop) ? lerp(0.45, 1.0, saturate((v1 - gv) * sh / " + num(spec.headShadow) +
         ")) : 1.0;\n";
    h += "float3 day = seen * hs * (shop ? ShopDay : RoomDay) * (1.0 - Night);\n";
    h += "float3 night = on * (seen + cc.rgb * ca * CurtainGlow) * tints[tint] * (shop ? ShopGlow : GlowScale);\n";
    // A lit shop's sign panel glows at night (SignGlow, strong enough to show in a wet road); a lantern glows by day
    // and much more by night.
    h += "float3 signGlow = (lit && part == 4.0) ? signLight * Night * SignGlow : float3(0.0, 0.0, 0.0);\n";
    h += "float3 lanternGlow = float3(0.9, 0.25, 0.08) * (0.4 + 6.0 * Night);\n";
    h += "float3 frostGlow = (!shop && part == 13.0 && lit) ? float3(0.9, 0.85, 0.72) * Night * GlowScale * 0.8 : "
         "float3(0.0, 0.0, 0.0);\n";
    h += "Emit = glass ? day + night : (part == 12.0 ? lanternGlow : (part == 13.0 && !shop ? frostGlow : signGlow));\n";
    h += "own = (!shop && part == 13.0) ? float3(0.7, 0.73, 0.73) : own;\n";
    h += "own = part == 17.0 ? float3(0.05, 0.05, 0.055) : (part == 18.0 ? float3(0.14, 0.14, 0.15) : own);\n";
    h += "float3 rgbOut = glass ? seen * hs : (part == 3.0 ? bandRgb : (part == 4.0 || part == 16.0 ? fasciaRgb : (part == "
         "12.0 ? float3(0.75, 0.12, 0.06) : (part == 9.0 || part == 15.0 ? float3(0.62, 0.6, 0.56) : own))));\n";
    h += "return float4(rgbOut, part);\n";
    return h;
}

std::string windowFacadeComposeHlsl() {
    std::string h;
    h.reserve(4096);
    h += "float p = floor(Win.a + 0.5);\n";
    h += "float lum = dot(WallAlbedo, float3(0.3, 0.59, 0.11));\n";
    h += "float grain = clamp(lum / 0.35, 0.6, 1.4);\n";
    h += array("float3", "kTint", kWallTints, [](const WindowTint& t) { return rgb(t.r, t.g, t.b); });
    h += array("float", "kTintS", kWallTints, [](const WindowTint& t) { return num(t.strength); });
    h += array("float3", "kRoof", kRoofFinishes, [](const WindowTint& t) { return rgb(t.r, t.g, t.b); });
    h += array("float", "kRoofS", kRoofFinishes, [](const WindowTint& t) { return num(t.strength); });
    h += "uint ti = (uint)clamp(floor(Tint + 0.5), 0.0, " + num(static_cast<float>(kWindowWallTintCount - 1)) + ");\n";
    h += "uint ri = (uint)clamp(floor(RoofFin + 0.5), 0.0, " + num(static_cast<float>(kWindowRoofFinishCount - 1)) +
         ");\n";
    // Capped, so a pale tint times a bright grain never glares under the night lights.
    const std::string cap = num(WindowFacadeSpec{}.wallMax);
    h += "float3 wall = min(lerp(WallAlbedo, kTint[ti] * grain, kTintS[ti]), " + cap + ");\n";
    h += "float3 roof = min(lerp(WallAlbedo, kRoof[ri] * grain, kRoofS[ri]), " + cap + ");\n";
    h += "bool top = Top > 0.5;\n";
    h += "bool Glass = p == 1.0;\n";
    h += "bool Frame = p == 2.0;\n";
    h += "bool Band = p == 3.0;\n";
    h += "bool Fascia = p == 4.0;\n";
    h += "bool Reveal = p >= 5.0 && p <= 8.0;\n";
    h += "bool Shade = p == 20.0;\n";
    h += "bool Metal = p == 17.0 || p == 18.0;\n";
    h += "bool Own = p >= 9.0 && !Shade;\n";
    h += "bool Flat = Glass || Frame || Fascia || Own;\n";
    h += "float3 rn = p == 5.0 ? T : (p == 6.0 ? -T : (p == 7.0 ? Nz : -Nz));\n";
    h += "NormalOut = Reveal ? normalize(rn) : (Flat ? normalize(VertexNormal) : WallNormal);\n";
    h += "RoughOut = Glass ? GlassRough : (Frame || Metal ? FrameRough : (Fascia ? FasciaRough : (p == 13.0 || p == 14.0 ? 0.2 : "
         "(p == 10.0 ? 0.45 : (Own ? 0.8 : WallRough)))));\n";
    h += "MetalOut = Glass ? 0.0 : (Frame || Metal ? FrameMetal : (p == 10.0 ? 0.5 : (Fascia || Own ? 0.0 : WallMetal)));\n";
    h += "AOOut = Reveal ? 0.6 * WallAO : (Flat ? 1.0 : (Shade ? 0.7 * WallAO : WallAO));\n";
    h += "EmissiveOut = Emit;\n";
    h += "float3 GlassAlbedo = Win.rgb * GlassBase + Curtain.rgb * Curtain.a * CurtainAlbedo;\n";
    h += "return top ? roof : (Glass ? GlassAlbedo : (Frame ? FrameColor.rgb : (Band ? Win.rgb * grain : (Reveal ? wall "
         "* 0.8 : (Fascia || Own ? Win.rgb : (Shade ? wall * 0.5 : wall))))));\n";
    return h;
}

}  // namespace ftcore
