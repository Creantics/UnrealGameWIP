#include "ftcore/ShopFront.h"

#include "ftcore/WindowInterior.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ftcore {

namespace {

constexpr std::uint32_t kKindSalt = 0xa54ff53au;      // as r5's, so the kinds stay where r5 put them
constexpr std::uint32_t kKindPickSalt = 0x510e527fu;  // as r5's
constexpr std::uint32_t kFasciaSalt = 0x1f83d9abu;    // as r5's
constexpr std::uint32_t kPictureSalt = 0x6a09e667u;
constexpr std::uint32_t kBaySalt = 0xbb67ae85u;
constexpr std::uint32_t kCropSalt = 0x3c6ef373u;
constexpr std::uint32_t kSignSalt = 0xa4093822u;
constexpr std::uint32_t kStrokeSalt = 0x299f31d0u;

// The sign words, generic shop words only, grouped by kind (kGroups).
constexpr const char* kWords[kShopWordCount] = {
    // glazed shops
    "BOOKS", "PHONES", "SHOES", "TOYS", "LAMPS", "PRINT", "KEYS", "CLOCKS", "PAPER", "BAGS",
    // cafes
    "TEA", "BAKERY", "CAKES", "BREAD",
    // shutter shops
    "REPAIR", "TOOLS", "LOCKS", "TAILOR", "HARDWARE",
    // eateries
    "NOODLES", "DUMPLING", "SOUP", "RICE", "GRILL",
    // clinics
    "CLINIC", "PHARMACY", "DENTAL",
    // stalls
    "FRUIT", "FISH", "HERBS",
};
constexpr ShopWordGroup kGroups[kShopFrontKinds] = {{0, 10}, {10, 4}, {14, 5}, {19, 5}, {24, 3}, {27, 3}};

// The 5 x 7 face, A..Z: seven rows from the top, each five columns from the left ('#' ink).
constexpr const char* kFace[26][7] = {
    {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"},  // A
    {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."},  // B
    {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."},  // C
    {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."},  // D
    {"#####", "#....", "#....", "####.", "#....", "#....", "#####"},  // E
    {"#####", "#....", "#....", "####.", "#....", "#....", "#...."},  // F
    {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"},  // G
    {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"},  // H
    {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."},  // I
    {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."},  // J
    {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"},  // K
    {"#....", "#....", "#....", "#....", "#....", "#....", "#####"},  // L
    {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"},  // M
    {"#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#"},  // N
    {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."},  // O
    {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."},  // P
    {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"},  // Q
    {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"},  // R
    {".####", "#....", "#....", ".###.", "....#", "....#", "####."},  // S
    {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."},  // T
    {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."},  // U
    {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."},  // V
    {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#."},  // W
    {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"},  // X
    {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."},  // Y
    {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"},  // Z
};

struct Rgb {
    float r, g, b;
};
// As r5's fascia boards.
constexpr Rgb kFascia[8] = {{0.03f, 0.05f, 0.12f}, {0.35f, 0.03f, 0.03f}, {0.02f, 0.02f, 0.02f},
                            {0.75f, 0.75f, 0.72f}, {0.03f, 0.20f, 0.08f}, {0.60f, 0.22f, 0.03f},
                            {0.02f, 0.22f, 0.24f}, {0.55f, 0.40f, 0.05f}};
// A sign's panel or letters.
constexpr Rgb kInk[kShopInkCount] = {{0.86f, 0.82f, 0.70f}, {0.90f, 0.90f, 0.88f}, {0.88f, 0.66f, 0.10f},
                                     {0.62f, 0.05f, 0.04f}, {0.45f, 0.66f, 0.82f}, {0.10f, 0.46f, 0.26f}};
// Riser finishes, four colours each: tile, stone, steel, timber.
constexpr Rgb kTile[4] = {{0.74f, 0.71f, 0.63f}, {0.14f, 0.34f, 0.27f}, {0.40f, 0.08f, 0.07f}, {0.20f, 0.30f, 0.45f}};
constexpr Rgb kStone[4] = {{0.45f, 0.44f, 0.42f}, {0.30f, 0.30f, 0.31f}, {0.55f, 0.50f, 0.45f}, {0.20f, 0.20f, 0.21f}};
constexpr Rgb kSteel[4] = {{0.12f, 0.13f, 0.14f}, {0.25f, 0.27f, 0.30f}, {0.30f, 0.12f, 0.08f}, {0.10f, 0.20f, 0.15f}};
constexpr Rgb kTimber[4] = {{0.25f, 0.14f, 0.07f}, {0.35f, 0.20f, 0.10f}, {0.18f, 0.10f, 0.05f}, {0.30f, 0.22f, 0.14f}};
constexpr Rgb kShutter[4] = {{0.42f, 0.42f, 0.43f}, {0.30f, 0.36f, 0.32f}, {0.32f, 0.35f, 0.42f}, {0.50f, 0.48f, 0.44f}};
constexpr Rgb kCloth[4] = {{0.04f, 0.06f, 0.20f}, {0.35f, 0.04f, 0.03f}, {0.15f, 0.08f, 0.04f}, {0.06f, 0.06f, 0.06f}};

constexpr std::uint32_t kStrokeCells = 5;  // a stroke cell's grid, either way

float share(std::uint32_t bits16) { return static_cast<float>(bits16 & 0xFFFFu) / 65536.0f; }

std::uint32_t letterCode(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<std::uint32_t>(c - 'A') + 1u : 0u; }

// A glyph's rows packed as the HLSL reads them: rows 0..3 in x (row r at bit 5r, its leftmost column bit 4), 4..6 in y.
struct Packed {
    std::uint32_t x = 0, y = 0;
};
Packed packGlyph(std::uint32_t g) {
    Packed p;
    for (std::uint32_t r = 0; r < 7; ++r) {
        std::uint32_t row = 0;
        for (std::uint32_t c = 0; c < 5; ++c) row |= (kFace[g][r][c] == '#' ? 1u : 0u) << (4u - c);
        if (r < 4) p.x |= row << (5u * r);
        else p.y |= row << (5u * (r - 4u));
    }
    return p;
}
Packed packWord(const char* w) {
    Packed p;
    for (std::uint32_t i = 0; i < kShopWordLetters && w[i] != '\0'; ++i) {
        const std::uint32_t code = letterCode(w[i]);
        if (i < 6) p.x |= code << (5u * i);
        else p.y |= code << (5u * (i - 6u));
    }
    return p;
}

std::uint32_t preferredPicture(ShopFrontKind kind) {
    switch (kind) {
    case ShopFrontKind::Cafe:
    case ShopFrontKind::Eatery: return 1u;
    case ShopFrontKind::Clinic: return 2u;
    case ShopFrontKind::Stall: return 0u;
    default: return kWindowShops;
    }
}

// Ink of a stroke mark at grid cell (gx, gy) of cell `cell`, as the HLSL.
bool strokeInk(std::uint32_t signHash, std::uint32_t cell, std::uint32_t gx, std::uint32_t gy) {
    const std::uint32_t ch = windowHash(signHash + cell * 7u + kStrokeSalt);
    const std::uint32_t strokes = 3u + ch % 3u;
    bool ink = false;
    for (std::uint32_t s = 0; s < 5u; ++s) {
        const std::uint32_t h = windowHash(ch + s * 131u);
        const std::uint32_t pos = (h >> 1) % kStrokeCells, from = (h >> 4) % 3u, len = 2u + (h >> 6) % 3u;
        const std::uint32_t along = (h & 1u) != 0u ? gy : gx, across = (h & 1u) != 0u ? gx : gy;
        ink = ink || (s < strokes && across == pos && along >= from && along < from + len);
    }
    return ink;
}

std::string num(float f) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(f));
    std::string s(buf);
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}
std::string unum(std::uint32_t u) { return std::to_string(u) + "u"; }
std::string rgb(const Rgb& c) { return "float3(" + num(c.r) + ", " + num(c.g) + ", " + num(c.b) + ")"; }
template <std::size_t N>
std::string rgbArray(const char* name, const Rgb (&items)[N]) {
    std::string s = std::string("float3 ") + name + "[" + std::to_string(N) + "] = { ";
    for (std::size_t i = 0; i < N; ++i) s += (i ? ", " : "") + rgb(items[i]);
    return s + " };\n";
}

}  // namespace

std::uint32_t shopFrontRawKind(std::uint32_t seed, std::uint32_t column) {
    return windowHash(windowHash(seed ^ kKindSalt) + windowHash(column)) % kShopFrontKinds;
}

ShopFrontKind shopFrontKind(std::uint32_t seed, std::uint32_t column) {
    std::uint32_t k = shopFrontRawKind(seed, column);
    if ((column & 1u) != 0u) {
        const std::uint32_t l = shopFrontRawKind(seed, column - 1u), r = shopFrontRawKind(seed, column + 1u);
        for (int i = 0; i < 2; ++i)
            if (k == l || k == r) k = (k + 1u) % kShopFrontKinds;
    }
    return static_cast<ShopFrontKind>(k);
}

std::uint32_t shopFrontRawPicture(std::uint32_t seed, std::uint32_t column, ShopFrontKind kind) {
    const std::uint32_t h = windowHash(windowHash(seed ^ kPictureSalt) + windowHash(column));
    const std::uint32_t pref = preferredPicture(kind);
    return (pref < kWindowShops && (h & 3u) != 0u) ? pref : (h >> 2) % kWindowShops;
}

std::uint32_t shopFrontPicture(std::uint32_t seed, std::uint32_t column) {
    std::uint32_t p = shopFrontRawPicture(seed, column, shopFrontKind(seed, column));
    if ((column & 1u) != 0u) {
        const std::uint32_t l = shopFrontRawPicture(seed, column - 1u, shopFrontKind(seed, column - 1u));
        const std::uint32_t r = shopFrontRawPicture(seed, column + 1u, shopFrontKind(seed, column + 1u));
        for (int i = 0; i < 2; ++i)
            if (p == l || p == r) p = (p + 1u) % kWindowShops;
    }
    return kWindowRooms + p;
}

ShopBay shopFrontBay(const ShopFrontSpec& spec, std::uint32_t seed, std::uint32_t column) {
    ShopBay b;
    b.kind = shopFrontKind(seed, column);
    const std::uint32_t k = static_cast<std::uint32_t>(b.kind);
    const std::uint32_t kh = windowHash(seed ^ windowHash(column + kKindPickSalt));
    const std::uint32_t bh = windowHash(kh ^ kBaySalt);
    const float s0 = share(kh);
    const std::uint32_t fin = (kh >> 18) & 3u;
    const bool alt = ((kh >> 18) & 1u) != 0u;
    switch (b.kind) {
    case ShopFrontKind::Glazed: b.riser = static_cast<ShopRiser>(1u + fin); break;
    case ShopFrontKind::Cafe: b.riser = ShopRiser::Timber; break;
    case ShopFrontKind::Shutter: b.riser = ShopRiser::None; break;
    case ShopFrontKind::Eatery: b.riser = alt ? ShopRiser::Tile : ShopRiser::Timber; break;
    case ShopFrontKind::Clinic: b.riser = ShopRiser::Stone; break;
    case ShopFrontKind::Stall: b.riser = alt ? ShopRiser::Steel : ShopRiser::Timber; break;
    }
    b.riserHeight = b.riser == ShopRiser::None
                        ? 0.0f
                        : (b.kind == ShopFrontKind::Stall ? spec.counter
                                                          : spec.riserMin + (spec.riserMax - spec.riserMin) * s0);
    if (k == 0u || k == 1u || k == 3u) b.door = ((kh >> 16) & 1u) != 0u ? ShopDoor::Right : ShopDoor::Left;
    else if (b.kind == ShopFrontKind::Clinic) b.door = ShopDoor::Middle;
    b.housing = k == 2u || k == 5u || (k != 1u && share(bh) < spec.housingShare);
    if (b.kind == ShopFrontKind::Shutter) {
        b.shutter = spec.shutterLeast + (1.0f - spec.shutterLeast) * s0;
        b.shutter = b.shutter > 0.9f ? 1.0f : b.shutter;
    }
    b.picture = shopFrontPicture(seed, column);
    const std::uint32_t ch = windowHash(bh ^ kCropSalt);
    b.zoom = spec.zoomLeast + (1.0f - spec.zoomLeast) * share(ch);
    b.shift = (1.0f - b.zoom) * share(ch >> 16);
    b.colour = (kh >> 20) & 3u;
    const std::uint32_t sh = windowHash(seed ^ windowHash(column + kSignSalt));
    b.style = static_cast<ShopSignStyle>(sh % kShopSignStyles);
    const ShopWordGroup g = kGroups[k];
    b.word = g.first + (sh >> 8) % g.count;
    b.ink = (sh >> 16) % kShopInkCount;
    return b;
}

ShopSpan shopFrontDoorSpan(const ShopFrontSpec& spec, const ShopBay& bay, float openingWidth) {
    ShopSpan s;
    if (bay.door == ShopDoor::None) return s;
    const float w = std::max(openingWidth, 0.1f);
    const float dw = std::min(spec.doorWidth, 0.45f * w) / w;
    s.a = bay.door == ShopDoor::Left ? 0.0f : (bay.door == ShopDoor::Right ? 1.0f - dw : 0.5f - 0.5f * dw);
    s.b = s.a + dw;
    return s;
}

float shopFrontSill(const ShopFrontSpec& spec, const ShopBay& bay, float openingWidth, float uOpen) {
    const ShopSpan d = shopFrontDoorSpan(spec, bay, openingWidth);
    const bool inDoor = bay.door != ShopDoor::None && uOpen >= d.a && uOpen <= d.b;
    return (inDoor || bay.kind == ShopFrontKind::Shutter) ? 0.02f : bay.riserHeight;
}

const char* shopSignWord(std::uint32_t index) { return kWords[index < kShopWordCount ? index : 0u]; }

ShopWordGroup shopSignGroup(ShopFrontKind kind) {
    const std::uint32_t k = static_cast<std::uint32_t>(kind);
    return kGroups[k < kShopFrontKinds ? k : 0u];
}

bool shopGlyphInk(char letter, std::uint32_t x, std::uint32_t y) {
    const std::uint32_t code = letterCode(letter);
    if (code == 0u || x > 4u || y > 6u) return false;
    return kFace[code - 1u][y][x] == '#';
}

bool shopSignInk(const ShopBay& bay, float width, float height, float x, float y) {
    const float pw = std::max(width, 0.05f), ph = std::max(height, 0.05f);
    const float X = x * pw, Y = y * ph;
    if (bay.style == ShopSignStyle::Strokes) {
        // The cell count and stroke hash come from the word and ink, as the HLSL.
        const std::uint32_t k = 2u + (bay.ink + bay.word) % 3u;
        const float span = static_cast<float>(k) + 0.3f * static_cast<float>(k - 1u);
        const float cs = std::min(ph * 0.9f, 0.92f * pw / span);
        const float x0 = 0.5f * (pw - cs * span), y0 = 0.5f * (ph - cs);
        const float lx = X - x0, ly = Y - y0;
        if (lx < 0.0f || ly < 0.0f || ly >= cs) return false;
        const float cell = std::floor(lx / (1.3f * cs));
        const float cx = lx - cell * 1.3f * cs;
        if (cell >= static_cast<float>(k) || cx >= cs) return false;
        const std::uint32_t gx = std::min(static_cast<std::uint32_t>(cx / cs * 5.0f), 4u);
        const std::uint32_t gy = std::min(static_cast<std::uint32_t>(ly / cs * 5.0f), 4u);
        return strokeInk(bay.word * 977u + bay.ink, static_cast<std::uint32_t>(cell), gx, gy);
    }
    const char* w = shopSignWord(bay.word);
    std::uint32_t n = 0;
    while (n < kShopWordLetters && w[n] != '\0') ++n;
    const float tw = 6.0f * static_cast<float>(n) - 1.0f;
    float p = ph / 9.0f;
    p = tw * p > 0.92f * pw ? 0.92f * pw / tw : p;
    const float x0 = 0.5f * (pw - tw * p), y0 = 0.5f * (ph - 7.0f * p);
    const float fx = std::floor((X - x0) / p), fy = 6.0f - std::floor((Y - y0) / p);
    if (fx < 0.0f || fx >= tw || fy < 0.0f || fy > 6.0f) return false;
    const std::uint32_t gx = static_cast<std::uint32_t>(fx), gy = static_cast<std::uint32_t>(fy);
    const std::uint32_t li = gx / 6u, cx = gx % 6u;
    return cx < 5u && shopGlyphInk(w[li], cx, gy);
}

std::string shopFrontPreHlsl(const ShopFrontSpec& spec) {
    std::string h;
    h.reserve(4096);
    h += "struct FTShop {\n";
    h += "uint H(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }\n";
    h += "uint KRaw(uint s, uint c) { return H(H(s ^ " + unum(kKindSalt) + ") + H(c)) % " + unum(kShopFrontKinds) +
         "; }\n";
    h += "uint PRaw(uint s, uint c, uint k) { uint p = H(H(s ^ " + unum(kPictureSalt) +
         ") + H(c)); uint pref = (k == 1u || k == 3u) ? 1u : (k == 4u ? 2u : (k == 5u ? 0u : " + unum(kWindowShops) +
         ")); return (pref < " + unum(kWindowShops) + " && (p & 3u) != 0u) ? pref : (p >> 2) % " +
         unum(kWindowShops) + "; }\n";
    h += "};\n";
    h += "FTShop SF;\n";
    // The kind (shopFrontKind), stepped on an odd column past its even neighbours'.
    h += "uint kind = SF.KRaw(seed, col);\n";
    h += "uint sfKL = SF.KRaw(seed, col - 1u);\n";
    h += "uint sfKR = SF.KRaw(seed, col + 1u);\n";
    h += "bool sfOdd = (col & 1u) != 0u;\n";
    for (int i = 0; i < 2; ++i)
        h += "kind = (sfOdd && (kind == sfKL || kind == sfKR)) ? (kind + 1u) % " + unum(kShopFrontKinds) + " : kind;\n";
    h += "uint kh = SF.H(seed ^ SF.H(col + " + unum(kKindPickSalt) + "));\n";
    h += "uint sfBh = SF.H(kh ^ " + unum(kBaySalt) + ");\n";
    h += "float sfS0 = float(kh & 0xFFFFu) / 65536.0;\n";
    // The picture (shopFrontPicture) and its crop.
    h += "uint shopPick = SF.PRaw(seed, col, kind);\n";
    h += "uint sfPL = SF.PRaw(seed, col - 1u, sfKL);\n";
    h += "uint sfPR = SF.PRaw(seed, col + 1u, sfKR);\n";
    for (int i = 0; i < 2; ++i)
        h += "shopPick = (sfOdd && (shopPick == sfPL || shopPick == sfPR)) ? (shopPick + 1u) % " + unum(kWindowShops) +
             " : shopPick;\n";
    h += "uint sfCh = SF.H(sfBh ^ " + unum(kCropSalt) + ");\n";
    h += "float shopZoom = " + num(spec.zoomLeast) + " + " + num(1.0f - spec.zoomLeast) +
         " * float(sfCh & 0xFFFFu) / 65536.0;\n";
    h += "float shopShift = (1.0 - shopZoom) * float((sfCh >> 16) & 0xFFFFu) / 65536.0;\n";
    // The riser (0 none, 1 tile, 2 stone, 3 steel, 4 timber), its height, the door and the shutter (shopFrontBay).
    h += "uint sfFin = (kh >> 18) & 3u;\n";
    h += "bool sfAlt = ((kh >> 18) & 1u) != 0u;\n";
    h += "uint sfRiser = kind == 0u ? 1u + sfFin : (kind == 1u ? 4u : (kind == 2u ? 0u : (kind == 3u ? (sfAlt ? 1u : "
         "4u) : (kind == 4u ? 2u : (sfAlt ? 3u : 4u)))));\n";
    h += "float sfRiserH = sfRiser == 0u ? 0.0 : (kind == 5u ? " + num(spec.counter) + " : " + num(spec.riserMin) +
         " + " + num(spec.riserMax - spec.riserMin) + " * sfS0);\n";
    h += "uint sfDoor = (kind == 0u || kind == 1u || kind == 3u) ? (((kh >> 16) & 1u) != 0u ? 2u : 1u) : (kind == 4u "
         "? 3u : 0u);\n";
    h += "bool sfHousing = kind == 2u || kind == 5u || (kind != 1u && float(sfBh & 0xFFFFu) / 65536.0 < " +
         num(spec.housingShare) + ");\n";
    h += "float sfShut = kind == 2u ? " + num(spec.shutterLeast) + " + " + num(1.0f - spec.shutterLeast) +
         " * sfS0 : 0.0;\n";
    h += "sfShut = sfShut > 0.9 ? 1.0 : sfShut;\n";
    h += "uint sfColour = (kh >> 20) & 3u;\n";
    // The door's span across the opening (shopFrontDoorSpan) and the opening's bottom here (shopFrontSill).
    h += "float sfW = max((u1 - u0) * bw, 0.1);\n";
    h += "float sfDw = min(" + num(spec.doorWidth) + ", 0.45 * sfW) / sfW;\n";
    h += "float sfDa = sfDoor == 1u ? 0.0 : (sfDoor == 2u ? 1.0 - sfDw : 0.5 - 0.5 * sfDw);\n";
    h += "float sfDb = sfDa + sfDw;\n";
    h += "float sfUo = (u - u0) / max(u1 - u0, 1e-4);\n";
    h += "bool sfInDoor = sfDoor != 0u && sfUo >= sfDa && sfUo <= sfDb;\n";
    h += "float shopSill = (sfInDoor || kind == 2u) ? 0.02 : sfRiserH;\n";
    return h;
}

std::string shopFrontHlsl(const ShopFrontSpec& spec, float fasciaHeight) {
    std::string h;
    h.reserve(16384);
    h += "float3 own = float3(0.0, 0.0, 0.0);\n";
    h += "float sp = 0.0;\n";
    h += "float sfTop = v1 * sh;\n";
    h += "float sfBot = v0 * sh;\n";
    h += "float sfGx = (gu - u0) / max(u1 - u0, 1e-4) * sfW;\n";
    h += "float sfGz = gv * sh;\n";
    h += rgbArray("kShutter", kShutter);
    h += rgbArray("kCloth", kCloth);
    // An eatery's cloth panels over the top of its opening.
    h += "float sfPw4 = sfW / 4.0;\n";
    h += "bool sfNoren = kind == 3u && sfGz > sfBot + 0.62 * (sfTop - sfBot) && frac(sfGx / sfPw4) * sfPw4 > 0.03;\n";
    h += "sp = sfNoren ? 11.0 : sp;\n";
    h += "own = sfNoren ? kCloth[(kh >> 22) % 4u] : own;\n";
    // A clinic's frosted band across its glass, the door's too.
    h += "bool sfBand = kind == 4u && sfGz > 1.0 && sfGz < 1.22;\n";
    h += "sp = sfBand ? 13.0 : sp;\n";
    h += "own = sfBand ? float3(0.74, 0.77, 0.77) : own;\n";
    // A cafe's striped valance under its fascia, scalloped.
    h += "float sfSt = frac(a / 0.3);\n";
    h += "bool sfVal = kind == 1u && sfGz > sfTop - " + num(spec.valance) + " - 0.05 * (1.0 - abs(2.0 * sfSt - 1.0));\n";
    h += "sp = sfVal ? 16.0 : sp;\n";
    // A shutter shop's roller shutter, from the head down its share, ribbed, with a darker bottom rail.
    h += "float sfShutBot = sfTop - sfShut * (sfTop - 0.02);\n";
    h += "bool sfShutter = kind == 2u && sfGz > sfShutBot;\n";
    h += "float sfRib = frac((sfTop - sfGz) / 0.08) < 0.5 ? 0.82 : 1.0;\n";
    h += "sp = sfShutter ? 10.0 : sp;\n";
    h += "own = sfShutter ? (sfGz < sfShutBot + 0.05 && sfShut < 1.0 ? float3(0.2, 0.2, 0.21) : kShutter[sfColour] * "
         "sfRib) : own;\n";
    // The shutter box at the opening's head.
    h += "bool sfBox = sfHousing && sfGz > sfTop - " + num(spec.housing) + ";\n";
    h += "sp = sfBox ? 10.0 : sp;\n";
    h += "own = sfBox ? float3(0.33, 0.34, 0.35) * (frac((sfTop - sfGz) / 0.05) < 0.15 ? 0.75 : 1.0) : own;\n";
    // The door: its frame and the mullion beside it, the transom bar across the opening at its head, a kick plate and a
    // handle; a clinic's is a pair with a stile in the middle.
    const std::string f = num(spec.doorFrame), dh = num(spec.doorHeight);
    h += "float sfX0 = sfDa * sfW;\n";
    h += "float sfX1 = sfDb * sfW;\n";
    h += "float sfXm = 0.5 * (sfX0 + sfX1);\n";
    h += "bool sfHasDoor = sfDoor != 0u;\n";
    h += "bool sfInDoorG = sfHasDoor && sfGx >= sfX0 && sfGx <= sfX1;\n";
    h += "bool sfFrame = sfHasDoor && sfGz < " + dh + " && (abs(sfGx - sfX0) < " + f + " || abs(sfGx - sfX1) < " + f +
         " || (sfDoor == 3u && abs(sfGx - sfXm) < 0.5 * " + f + "));\n";
    h += "sfFrame = sfFrame || (sfHasDoor && abs(sfGz - " + dh + ") < 0.6 * " + f + ");\n";
    h += "bool sfKick = sfInDoorG && sfGz < " + num(spec.kick) + ";\n";
    h += "float sfHx = sfDoor == 1u ? sfX1 - 0.12 : (sfDoor == 2u ? sfX0 + 0.12 : sfXm);\n";
    h += "float sfHd = sfDoor == 3u ? abs(abs(sfGx - sfXm) - 0.07) : abs(sfGx - sfHx);\n";
    h += "bool sfHandle = sfHasDoor && sfGz > 0.75 && sfGz < 1.35 && sfHd < 0.018;\n";
    h += "sp = sfFrame ? 2.0 : sp;\n";
    h += "sp = (sfKick || sfHandle) ? 10.0 : sp;\n";
    h += "own = sfHandle ? float3(0.6, 0.6, 0.62) : (sfKick ? float3(0.38, 0.38, 0.4) : own);\n";
    h += "bool shopGlass = shop && part == 1.0;\n";
    h += "part = shopGlass && sp > 0.0 ? sp : part;\n";
    // The stall riser under the opening: tile, stone, steel or timber, a dark plinth line at its foot; a stall's counter
    // has a pale top; a door's threshold is steel.
    h += rgbArray("kTile", kTile);
    h += rgbArray("kStone", kStone);
    h += rgbArray("kSteel", kSteel);
    h += rgbArray("kTimber", kTimber);
    h += "bool sfRiserPx = shop && inBay && sfUo >= 0.0 && sfUo <= 1.0 && Up >= 0.0 && Up < shopSill && part == 3.0;\n";
    h += "float sfRx = (u - u0) * bw;\n";
    h += "bool sfGrout = frac(sfRx / 0.1) < 0.08 || frac(Up / 0.1) < 0.08;\n";
    h += "uint sfCell = SF.H((uint)floor(sfRx / 0.3) * 7919u + (uint)floor(Up / 0.15) + kh);\n";
    h += "float3 sfRc = kSteel[sfColour] * (abs(Up - 0.5 * shopSill) < 0.01 ? 0.6 : 1.0);\n";
    h += "sfRc = sfRiser == 1u ? kTile[sfColour] * (sfGrout ? 0.55 : 1.0) : sfRc;\n";
    h += "sfRc = sfRiser == 2u ? kStone[sfColour] * (0.88 + 0.24 * float(sfCell & 0xFFFFu) / 65536.0) : sfRc;\n";
    h += "sfRc = sfRiser == 4u ? kTimber[sfColour] * (frac(sfRx / 0.12) < 0.06 ? 0.5 : 1.0) : sfRc;\n";
    h += "sfRc = (kind == 5u && Up > shopSill - 0.05) ? float3(0.6, 0.6, 0.58) : sfRc;\n";
    h += "sfRc = sfRc * (Up < 0.04 ? 0.5 : 1.0);\n";
    h += "bool sfThreshold = shopSill < 0.05;\n";
    h += "sfRc = sfThreshold ? float3(0.3, 0.3, 0.31) : sfRc;\n";
    h += "part = sfRiserPx ? ((sfRiser == 3u || sfThreshold) ? 10.0 : 11.0) : part;\n";
    h += "own = sfRiserPx ? sfRc : own;\n";
    // The fascia: a board per bay in its own colour, carrying a sign: a word on a pale panel (a lightbox at night), a
    // word straight on the board (lit letters), or a mark of strokes (lit).
    h += rgbArray("kFascia", kFascia);
    h += rgbArray("kInk", kInk);
    h += "float3 fc = inBay ? kFascia[SF.H((seed ^ " + unum(kFasciaSalt) + ") + col) % 8u] : float3(0.06, 0.065, 0.07);\n";
    {
        std::string glyphs = "uint2 kGlyph[26] = { ";
        for (std::uint32_t g = 0; g < 26; ++g) {
            const Packed p = packGlyph(g);
            glyphs += (g ? ", " : "") + std::string("uint2(") + unum(p.x) + ", " + unum(p.y) + ")";
        }
        h += glyphs + " };\n";
        std::string words = "uint2 kWord[" + std::to_string(kShopWordCount) + "] = { ";
        for (std::uint32_t w = 0; w < kShopWordCount; ++w) {
            const Packed p = packWord(kWords[w]);
            words += (w ? ", " : "") + std::string("uint2(") + unum(p.x) + ", " + unum(p.y) + ")";
        }
        h += words + " };\n";
        std::string first = "uint kGFirst[6] = { ", count = "uint kGCount[6] = { ";
        for (std::uint32_t k = 0; k < kShopFrontKinds; ++k) {
            first += (k ? ", " : "") + unum(kGroups[k].first);
            count += (k ? ", " : "") + unum(kGroups[k].count);
        }
        h += first + " };\n" + count + " };\n";
    }
    const float m = spec.signMargin;
    h += "uint sfSh = SF.H(seed ^ SF.H(col + " + unum(kSignSalt) + "));\n";
    h += "uint sfStyle = sfSh % " + unum(kShopSignStyles) + ";\n";
    h += "uint sfWord = kGFirst[kind] + (sfSh >> 8) % kGCount[kind];\n";
    h += "uint sfInkI = (sfSh >> 16) % " + unum(kShopInkCount) + ";\n";
    h += "float3 sfInkC = kInk[sfInkI];\n";
    h += "float sfPanW = 0.76 * bw;\n";
    h += "float sfPanH = max(" + num(fasciaHeight - 2.0f * m) + ", 0.05);\n";
    // The facade's across coordinate u runs to the viewer's left on every face (Unreal's frame is left-handed), so the
    // panel reads from 1 - u: its words start at the viewer's left and aren't mirrored.
    h += "float sfX = (1.0 - u) * bw - 0.12 * bw;\n";
    h += "float sfY = Up - shopTop - " + num(m) + ";\n";
    h += "bool panel = fascia && inBay && sfX >= 0.0 && sfX <= sfPanW && sfY >= 0.0 && sfY <= sfPanH;\n";
    // A word (shopSignInk): letters sfPanH / 9 a pixel, narrower to fit 92% of the panel, centred.
    h += "uint2 sfWd = kWord[sfWord];\n";
    h += "uint sfN = 0u;\n";
    h += "[unroll] for (uint sfI = 0u; sfI < " + unum(kShopWordLetters) +
         "; ++sfI) { uint c = sfI < 6u ? (sfWd.x >> (5u * sfI)) & 31u : (sfWd.y >> (5u * (sfI - 6u))) & 31u; sfN += c "
         "!= 0u ? 1u : 0u; }\n";
    h += "float sfTw = 6.0 * float(sfN) - 1.0;\n";
    h += "float sfP = sfPanH / 9.0;\n";
    h += "sfP = sfTw * sfP > 0.92 * sfPanW ? 0.92 * sfPanW / sfTw : sfP;\n";
    h += "float sfFx = floor((sfX - 0.5 * (sfPanW - sfTw * sfP)) / sfP);\n";
    h += "float sfFy = 6.0 - floor((sfY - 0.5 * (sfPanH - 7.0 * sfP)) / sfP);\n";
    h += "bool sfInk = false;\n";
    h += "if (sfFx >= 0.0 && sfFx < sfTw && sfFy >= 0.0 && sfFy <= 6.0) {\n";
    h += "  uint gx = (uint)sfFx; uint gy = (uint)sfFy; uint li = gx / 6u; uint cx = gx % 6u;\n";
    h += "  uint code = li < 6u ? (sfWd.x >> (5u * li)) & 31u : (sfWd.y >> (5u * (li - 6u))) & 31u;\n";
    h += "  uint2 g = kGlyph[max(code, 1u) - 1u];\n";
    h += "  uint row = gy < 4u ? (g.x >> (5u * gy)) & 31u : (g.y >> (5u * (gy - 4u))) & 31u;\n";
    h += "  sfInk = cx < 5u && code != 0u && ((row >> (4u - cx)) & 1u) != 0u;\n";
    h += "}\n";
    // A mark of strokes: 2..4 square cells, each 3..5 strokes on a 5 x 5 grid.
    h += "uint sfK = 2u + (sfInkI + sfWord) % 3u;\n";
    h += "float sfSpan = float(sfK) + 0.3 * float(sfK - 1u);\n";
    h += "float sfCs = min(sfPanH * 0.9, 0.92 * sfPanW / sfSpan);\n";
    h += "float sfLx = sfX - 0.5 * (sfPanW - sfCs * sfSpan);\n";
    h += "float sfLy = sfY - 0.5 * (sfPanH - sfCs);\n";
    h += "float sfCi = floor(sfLx / (1.3 * sfCs));\n";
    h += "float sfCx = sfLx - sfCi * 1.3 * sfCs;\n";
    h += "bool sfMark = false;\n";
    h += "if (sfLx >= 0.0 && sfLy >= 0.0 && sfLy < sfCs && sfCi < float(sfK) && sfCx < sfCs) {\n";
    h += "  uint mx = min((uint)(sfCx / sfCs * 5.0), 4u); uint my = min((uint)(sfLy / sfCs * 5.0), 4u);\n";
    h += "  uint ch = SF.H(sfWord * 977u + sfInkI + (uint)sfCi * 7u + " + unum(kStrokeSalt) + ");\n";
    h += "  uint ns = 3u + ch % 3u;\n";
    h += "  [unroll] for (uint s = 0u; s < 5u; ++s) {\n";
    h += "    uint hs = SF.H(ch + s * 131u); uint pos = (hs >> 1) % 5u; uint from = (hs >> 4) % 3u; uint len = 2u + (hs >> "
         "6) % 3u;\n";
    h += "    uint along = (hs & 1u) != 0u ? my : mx; uint across = (hs & 1u) != 0u ? mx : my;\n";
    h += "    sfMark = sfMark || (s < ns && across == pos && along >= from && along < from + len);\n";
    h += "  }\n";
    h += "}\n";
    h += "bool sfStrokes = sfStyle == 2u;\n";
    h += "bool sfInked = panel && (sfStrokes ? sfMark : sfInk);\n";
    h += "float sfInkLum = dot(sfInkC, float3(0.3, 0.59, 0.11));\n";
    h += "float sfFcLum = dot(fc, float3(0.3, 0.59, 0.11));\n";
    h += "float3 sfPanelLetters = sfInkLum > 0.35 ? fc * 0.5 + 0.02 : float3(0.9, 0.88, 0.8);\n";
    h += "float3 sfBoardLetters = sfFcLum > 0.4 ? float3(0.05, 0.05, 0.06) : (sfInkLum < 0.25 ? lerp(sfInkC, float3(1.0, "
         "1.0, 1.0), 0.55) : sfInkC);\n";
    h += "bool sfLightbox = sfStyle == 0u;\n";
    h += "float3 fasciaRgb = sfLightbox ? (panel ? (sfInked ? sfPanelLetters : sfInkC) : fc) : (sfInked ? sfBoardLetters "
         ": fc);\n";
    h += "float3 signLight = sfLightbox ? (panel && !sfInked ? sfInkC * 1.2 : float3(0.0, 0.0, 0.0)) : (sfInked ? (sfFcLum "
         "> 0.4 ? float3(1.0, 0.9, 0.7) : sfBoardLetters) : float3(0.0, 0.0, 0.0));\n";
    // The valance's stripes take the fascia's colour and cream.
    h += "fasciaRgb = (part == 16.0) ? (sfSt < 0.5 ? fc : float3(0.8, 0.76, 0.66)) : fasciaRgb;\n";
    // An eatery's lantern hangs at the fascia's foot on the bay's left.
    h += "float lxL = (u * bw - (u0 * bw + 0.35)) / 0.17;\n";
    h += "float lyL = (Up - shopTop) / 0.26;\n";
    h += "bool lantern = shop && inBay && kind == 3u && lxL * lxL + lyL * lyL < 1.0;\n";
    h += "part = lantern ? 12.0 : part;\n";
    return h;
}

}  // namespace ftcore
