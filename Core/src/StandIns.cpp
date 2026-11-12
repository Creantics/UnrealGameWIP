#include "ftcore/StandIns.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <array>
#include <utility>

namespace ftcore {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

// FNV-1a over the point's kind and its plan position in decimetres, then a salt.
std::uint32_t fnv(std::uint32_t h, std::uint32_t v) {
    for (int b = 0; b < 4; ++b) {
        h ^= (v >> (8 * b)) & 0xffu;
        h *= 16777619u;
    }
    return h;
}

struct Seed {
    std::uint32_t base = 2166136261u;
    // A repeatable share in [0, 1) for this salt.
    float u(std::uint32_t salt) const {
        std::uint32_t h = fnv(base, salt);
        h ^= h >> 15;
        h *= 0x2c1b3c6du;
        h ^= h >> 12;
        return static_cast<float>(h >> 8) / 16777216.f;
    }
};

Seed seedOf(const StandInPoint& p) {
    Seed s;
    s.base = fnv(s.base, static_cast<std::uint32_t>(p.kind));
    s.base = fnv(s.base, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(p.x * 10.0))));
    s.base = fnv(s.base, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(p.y * 10.0))));
    return s;
}

// Builds parts in the point's own frame (x along rot) and writes them turned into the plan's.
struct Emit {
    const StandInPoint& p;
    StandInPart* out;
    std::size_t n = 0;
    double c = 1.0, s = 0.0;
    Emit(const StandInPoint& point, StandInPart* o) : p(point), out(o), c(std::cos(point.rot * kDeg)), s(std::sin(point.rot * kDeg)) {}
    void add(StandInShape shape, StandInPaint paint, StandInCollide collide, double lx, double ly, double z, double sx, double sy, double sz,
             double yaw = 0.0, double roll = 0.0, double pitch = 0.0) {
        if (n >= kStandInPartsMax) {
            return;
        }
        StandInPart& part = out[n++];
        part.shape = shape;
        part.paint = paint;
        part.collide = collide;
        part.dx = static_cast<float>(lx * c - ly * s);
        part.dy = static_cast<float>(lx * s + ly * c);
        part.dz = static_cast<float>(z);
        part.sx = static_cast<float>(std::max(sx, 0.01));
        part.sy = static_cast<float>(std::max(sy, 0.01));
        part.sz = static_cast<float>(std::max(sz, 0.01));
        part.yaw = static_cast<float>(p.rot + yaw);
        part.roll = static_cast<float>(roll);
        part.pitch = static_cast<float>(pitch);
    }
};

// The four leaf greens, by a seeded share: mid, fresh light, deep blue-green and yellow-olive.
StandInPaint leafOf(float u) {
    return u < 0.3f ? StandInPaint::Leaf : (u < 0.55f ? StandInPaint::LeafLight : (u < 0.8f ? StandInPaint::LeafDark : StandInPaint::LeafWarm));
}

// The canopies' fork shares, as standInCanopyMesh builds them (a test holds the two together).
constexpr double kCanopyForkShare[kStandInCanopyVariants] = {0.3145186, 0.3343122, 0.3640813};

// A tree: a clear trunk (it collides) up to the fork, and one canopy mesh over it, the fork at the canopy's foot. Its
// crown width and depth vary by seed around the table's (+-12%), the canopy turns by seed, a park tree takes one of two
// canopies. Two instances whatever the kind.
void tree(Emit& e, const Seed& r, bool park) {
    const double k = std::clamp(static_cast<double>(e.p.scale), 0.6, 1.6);
    const double jitter = 0.88 + 0.24 * r.u(7);
    const double crownW = (e.p.canopy > 0.0 ? e.p.canopy : (park ? 7.0 + 3.0 * r.u(1) : 4.5 + 1.8 * r.u(1)) * k) * jitter;
    double crownH = crownW * (0.72 + 0.16 * r.u(2));
    double clear = (park ? 2.6 + 1.0 * r.u(3) : 2.8 + 0.8 * r.u(3)) * k; // the trunk's clear height under the crown
    if (e.p.h > 0.0) {
        // A given height: the crown's top there, on a clear trunk of at least 1.8 m.
        crownH = std::min(crownH, 0.6 * e.p.h);
        clear = std::max(e.p.h - crownH, 1.8);
    }
    const std::size_t variant = park ? (r.u(8) < 0.5f ? 1u : 2u) : 0u;
    const double share = kCanopyForkShare[variant];
    const double top = clear + crownH;
    // The fork under the crown by the canopy's own share, but never below 1.5 m (or 60% of the clear height).
    const double fork = std::max(clear - share * crownH / (1.0 - share), std::min(1.5, 0.6 * clear));
    const double girth = (park ? 0.42 : 0.3) * k;
    const double trunk = fork + 0.15; // into the limbs' foot
    e.add(StandInShape::Cylinder, StandInPaint::Bark, StandInCollide::Solid, 0, 0, trunk * 0.5, girth, girth, trunk);
    e.add(StandInShape::Canopy, leafOf(r.u(4)), StandInCollide::None, 0, 0, 0.5 * (fork + top), crownW, crownW * (0.85 + 0.15 * r.u(5)),
          top - fork, 360.0 * r.u(6));
    e.out[e.n - 1].variant = static_cast<std::uint8_t>(variant);
}

void shrub(Emit& e, const Seed& r) {
    const double k = std::clamp(static_cast<double>(e.p.scale), 0.6, 1.6);
    for (std::uint32_t i = 0; i < 2; ++i) {
        const double w = (1.4 + 1.0 * r.u(1 + 10 * i)) * k * (i == 0 ? 1.0 : 0.7);
        const double h = std::min(w * (0.55 + 0.15 * r.u(2 + 10 * i)), 1.5);
        const double a = 2.0 * kPi * r.u(3 + 10 * i);
        const double off = i == 0 ? 0.0 : 0.45 * w;
        e.add(StandInShape::Sphere, leafOf(r.u(4 + 10 * i)), StandInCollide::None, off * std::cos(a), off * std::sin(a), h * 0.3, w, w * 0.8,
              h, 360.0 * r.u(5 + 10 * i));
    }
}

void railing(Emit& e) {
    const double len = e.p.len > 0.0 ? e.p.len : 2.0;
    // Along a stair the rails pitch with it; the span's z is its foot at its centre.
    const double pitch = std::atan(e.p.slope) / kDeg;
    const double along = len / std::cos(pitch * kDeg);
    // Top rail (collides: it stops the lead at the quay edge), mid rail and the span's post at its start (the neighbour
    // shares it as its end).
    e.add(StandInShape::Cube, StandInPaint::Steel, StandInCollide::Solid, 0, 0, 1.1 - 0.03, along, 0.06, 0.06, 0.0, 0.0, pitch);
    e.add(StandInShape::Cube, StandInPaint::Steel, StandInCollide::Solid, 0, 0, 0.55, along, 0.03, 0.04, 0.0, 0.0, pitch);
    e.add(StandInShape::Cylinder, StandInPaint::Steel, StandInCollide::None, -0.5 * len + 0.04, 0, 0.55 - 0.5 * len * e.p.slope, 0.07, 0.07, 1.1);
}

void lampMast(Emit& e) {
    const double h = e.p.hasTop && e.p.top > e.p.z ? e.p.top - e.p.z : (e.p.h > 0.0 ? e.p.h : 8.0);
    e.add(StandInShape::Cylinder, StandInPaint::Steel, StandInCollide::Solid, 0, 0, h * 0.5, 0.2, 0.2, h);
    e.add(StandInShape::Cube, StandInPaint::LampHead, StandInCollide::None, 0.65, 0, h - 0.12, 1.5, 0.35, 0.2);
}

void footbridge(Emit& e) {
    const double len = e.p.len > 0.0 ? e.p.len : 30.0;
    const double wid = e.p.wid > 0.0 ? e.p.wid : 3.0;
    e.add(StandInShape::Cube, StandInPaint::Deck, StandInCollide::Walk, 0, 0, -0.175, len, wid, 0.35);
    e.add(StandInShape::Cube, StandInPaint::Steel, StandInCollide::None, 0, 0, -0.35 - 0.4, len, wid * 0.7, 0.8);
    for (int side = -1; side <= 1; side += 2) {
        e.add(StandInShape::Cube, StandInPaint::Steel, StandInCollide::Solid, 0, side * (0.5 * wid - 0.04), 0.55, len, 0.08, 1.1);
    }
}

// A footbridge's slab (deck or landing; a stair's pitched to its slope) from z up to top, or its pier.
void bridgePiece(Emit& e) {
    const double len = e.p.len > 0.0 ? e.p.len : 3.0;
    const double wid = e.p.wid > 0.0 ? e.p.wid : 3.0;
    const bool pier = e.p.kind == StandInKind::BridgePier;
    const bool stair = e.p.kind == StandInKind::BridgeStair;
    const double tall = e.p.hasTop && e.p.top > e.p.z ? e.p.top - e.p.z : (pier ? 5.0 : (stair ? 0.3 : 0.35));
    if (pier) {
        e.add(StandInShape::Cube, StandInPaint::Concrete, StandInCollide::Solid, 0, 0, 0.5 * tall, len, wid, tall);
        return;
    }
    const double pitch = stair ? std::atan(e.p.slope) / kDeg : 0.0;
    e.add(StandInShape::Cube, StandInPaint::Deck, StandInCollide::Walk, 0, 0, 0.5 * tall, len / std::cos(pitch * kDeg), wid, tall, 0.0, 0.0, pitch);
}

StandInPaint roofPaint(float u) {
    return u < 0.5f ? StandInPaint::RoofTile : (u < 0.8f ? StandInPaint::RoofSlate : StandInPaint::RoofGlazed);
}

// Two pitched halves meeting at a ridge along x, overhanging eaves and gables, and a gable infill under them.
void houseRoof(Emit& e, const Seed& r) {
    const double len = e.p.len > 0.0 ? e.p.len : 9.0;
    const double wid = e.p.wid > 0.0 ? e.p.wid : 8.0;
    const double pitch = e.p.pitch > 0.0 ? std::clamp(e.p.pitch, 15.0, 45.0) : 25.0 + 10.0 * r.u(1);
    const double t = std::tan(pitch * kDeg);
    const double over = 0.4, thick = 0.18;
    const double rise = 0.5 * wid * t; // ridge over the eaves
    const double run = 0.5 * wid + over;
    const double slab = run / std::cos(pitch * kDeg);
    const StandInPaint paint = roofPaint(r.u(2));
    for (int side = -1; side <= 1; side += 2) {
        // Its mid-plane runs from the ridge (0, rise) down to (side * run, rise - run * t); the slab sits on that line.
        const double yc = side * 0.5 * run;
        const double zc = rise - 0.5 * run * t + 0.5 * thick / std::cos(pitch * kDeg);
        e.add(StandInShape::Cube, paint, StandInCollide::None, 0, yc, zc, len + 2.0 * over, slab, thick, 0.0, -side * pitch);
    }
    // The gables' infill: the prism under the halves, standing on the eaves (nothing of it under them, where a kit
    // mesh smaller than its box would show it).
    e.add(StandInShape::Gable, StandInPaint::Concrete, StandInCollide::None, 0, 0, 0.5 * rise, len, wid, rise);
}

// A back house: one gable prism standing on the eaves over the box's footprint. Its ridge runs along the longer side
// whatever the table says (so the gable spans the shorter one and stays low), pitched as the table gives (15-40 degrees)
// or 25-35 by seed.
void houseBack(Emit& e, const Seed& r) {
    const double len = e.p.len > 0.0 ? e.p.len : 9.0;
    const double wid = e.p.wid > 0.0 ? e.p.wid : 8.0;
    const double pitch = e.p.pitch > 0.0 ? std::clamp(e.p.pitch, 15.0, 40.0) : 25.0 + 10.0 * r.u(1);
    const bool turn = wid > len;
    const double ridge = turn ? wid : len, span = turn ? len : wid;
    const double rise = 0.5 * span * std::tan(pitch * kDeg);
    e.add(StandInShape::Gable, roofPaint(r.u(2)), StandInCollide::None, 0, 0, 0.5 * rise, ridge, span, rise, turn ? 90.0 : 0.0);
}

void glassTower(Emit& e, const Seed& r) {
    const double len = e.p.len > 0.0 ? e.p.len : 30.0 + 15.0 * r.u(1);
    const double wid = e.p.wid > 0.0 ? e.p.wid : 30.0 + 15.0 * r.u(2);
    const int floors = e.p.floors > 0 ? e.p.floors : 30 + static_cast<int>(std::floor(21.0 * r.u(3)));
    const double height = e.p.h > 0.0 ? e.p.h : 3.5 * floors;
    const double lobby = 7.0, crown = 4.0;
    const bool setback = r.u(4) >= 0.35;
    const double lower = setback ? (0.70 + 0.1 * r.u(5)) * height : height - crown;
    const StandInPaint glass = r.u(6) < 0.5 ? StandInPaint::Glass : StandInPaint::GlassDark;
    // A recessed lobby, the shaft, an upper setback, the crown band and the plant box on top.
    e.add(StandInShape::Cube, StandInPaint::GlassDark, StandInCollide::Solid, 0, 0, 0.5 * lobby, len - 2.4, wid - 2.4, lobby);
    e.add(StandInShape::Cube, glass, StandInCollide::Solid, 0, 0, 0.5 * (lobby + lower), len, wid, lower - lobby);
    double top = lower;
    double upLen = len, upWid = wid;
    if (setback) {
        upLen = 0.82 * len;
        upWid = 0.82 * wid;
        const double upper = height - crown - lower;
        e.add(StandInShape::Cube, glass, StandInCollide::None, 0, 0, lower + 0.5 * upper, upLen, upWid, upper);
        top = lower + upper;
    }
    e.add(StandInShape::Cube, StandInPaint::Spandrel, StandInCollide::None, 0, 0, top + 0.5 * crown, upLen + 0.4, upWid + 0.4, crown);
    e.add(StandInShape::Cube, StandInPaint::Concrete, StandInCollide::None, 0, 0, top + crown + 2.5, 0.4 * upLen, 0.4 * upWid, 5.0);
    // Spandrel bands proud of the shaft, so its floors read at a distance.
    for (int b = 1; b <= 3; ++b) {
        const double z = lobby + (lower - lobby) * b / 4.0;
        e.add(StandInShape::Cube, StandInPaint::Spandrel, StandInCollide::None, 0, 0, z, len + 0.3, wid + 0.3, 1.0);
    }
}

// The landmark: a slender square shaft twisting a little at each of its four stages, two blade fins up its sides, a
// drum lantern between rims high up, a short stem and a kite diamond standing edge-up as its crown.
void landmark(Emit& e, const Seed& r) {
    const double height = e.p.h > 0.0 ? e.p.h : 250.0 + 50.0 * r.u(1);
    const double plinth = 10.0;
    const double shaftTop = 0.70 * height;
    e.add(StandInShape::Cube, StandInPaint::Concrete, StandInCollide::Solid, 0, 0, 0.5 * plinth, 26.0, 26.0, plinth);
    const double widths[4] = {18.0, 15.0, 12.0, 9.5};
    const double stage = (shaftTop - plinth) / 4.0;
    for (int i = 0; i < 4; ++i) {
        e.add(StandInShape::Cube, StandInPaint::Shell, i == 0 ? StandInCollide::Solid : StandInCollide::None, 0, 0, plinth + stage * (i + 0.5),
              widths[i], widths[i], stage + 0.02, 11.25 * i);
    }
    const double finH = 0.55 * height;
    for (int side = -1; side <= 1; side += 2) {
        e.add(StandInShape::Cube, StandInPaint::Shell, StandInCollide::None, side * 9.6, 0, plinth + 0.5 * finH, 1.2, 5.0, finH);
    }
    const double rim = 1.5, drum = 22.0;
    e.add(StandInShape::Cylinder, StandInPaint::Steel, StandInCollide::None, 0, 0, shaftTop + 0.5 * rim, 18.0, 18.0, rim);
    e.add(StandInShape::Cylinder, StandInPaint::Lantern, StandInCollide::None, 0, 0, shaftTop + rim + 0.5 * drum, 14.0, 14.0, drum);
    const double drumTop = shaftTop + rim + drum;
    e.add(StandInShape::Cylinder, StandInPaint::Steel, StandInCollide::None, 0, 0, drumTop + 0.5 * rim, 17.0, 17.0, rim);
    const double kite = 16.0;                       // the diamond's side
    const double half = kite * std::sqrt(0.5);      // its half diagonal
    const double kiteZ = height - half;
    const double stemFrom = drumTop + rim;
    e.add(StandInShape::Cylinder, StandInPaint::Steel, StandInCollide::None, 0, 0, 0.5 * (stemFrom + kiteZ), 1.2, 1.2, kiteZ - stemFrom);
    e.add(StandInShape::Cube, StandInPaint::Shell, StandInCollide::None, 0, 0, kiteZ, 0.8, kite, kite, 0.0, 45.0);
}

std::string_view trimView(std::string_view v) {
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.remove_prefix(1);
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.remove_suffix(1);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        v = v.substr(1, v.size() - 2);
    }
    return v;
}

bool equalsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

bool parseNum(std::string_view v, double& out) {
    v = trimView(v);
    if (v.empty()) return false;
    if (v.front() == '+') v.remove_prefix(1);
    const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    return ec == std::errc() && ptr == v.data() + v.size() && std::isfinite(out);
}

// Splits one line on commas (no quoted commas in these tables).
void splitLine(std::string_view line, std::vector<std::string_view>& fields) {
    fields.clear();
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            fields.push_back(trimView(line.substr(start, i - start)));
            start = i + 1;
        }
    }
}

bool fail(std::string* error, std::size_t line, const std::string& what) {
    if (error) *error = "line " + std::to_string(line) + ": " + what;
    return false;
}

} // namespace

const char* standInKindName(StandInKind kind) {
    switch (kind) {
    case StandInKind::StreetTree: return "street_tree";
    case StandInKind::ParkTree: return "park_tree";
    case StandInKind::Shrub: return "shrub";
    case StandInKind::Railing: return "railing";
    case StandInKind::LampMast: return "lamp_mast";
    case StandInKind::Footbridge: return "footbridge";
    case StandInKind::BridgeDeck: return "bridge_deck";
    case StandInKind::BridgeLanding: return "bridge_landing";
    case StandInKind::BridgeStair: return "bridge_stair";
    case StandInKind::BridgePier: return "bridge_pier";
    case StandInKind::House: return "house";
    case StandInKind::HouseBack: return "house_back";
    case StandInKind::GlassTower: return "glass_tower";
    case StandInKind::Landmark: return "landmark";
    default: return "?";
    }
}

std::size_t standInParts(const StandInPoint& p, StandInPart* out) {
    if (!out) return 0;
    Emit e(p, out);
    const Seed r = seedOf(p);
    switch (p.kind) {
    case StandInKind::StreetTree: tree(e, r, false); break;
    case StandInKind::ParkTree: tree(e, r, true); break;
    case StandInKind::Shrub: shrub(e, r); break;
    case StandInKind::Railing: railing(e); break;
    case StandInKind::LampMast: lampMast(e); break;
    case StandInKind::Footbridge: footbridge(e); break;
    case StandInKind::BridgeDeck:
    case StandInKind::BridgeLanding:
    case StandInKind::BridgeStair:
    case StandInKind::BridgePier: bridgePiece(e); break;
    case StandInKind::House: houseRoof(e, r); break;
    case StandInKind::HouseBack: houseBack(e, r); break;
    case StandInKind::GlassTower: glassTower(e, r); break;
    case StandInKind::Landmark: landmark(e, r); break;
    default: break;
    }
    return e.n;
}

bool parseStandInKind(std::string_view text, StandInKind& out) {
    struct Name {
        const char* text;
        StandInKind kind;
    };
    static constexpr Name kNames[] = {
        {"tree", StandInKind::StreetTree},       {"street_tree", StandInKind::StreetTree}, {"park_tree", StandInKind::ParkTree},
        {"shrub", StandInKind::Shrub},           {"planting", StandInKind::Shrub},         {"railing", StandInKind::Railing},
        {"rail", StandInKind::Railing},          {"railing_span", StandInKind::Railing},   {"mast", StandInKind::LampMast},
        {"lamp", StandInKind::LampMast},         {"lamp_mast", StandInKind::LampMast},     {"footbridge", StandInKind::Footbridge},
        {"bridge", StandInKind::Footbridge},     {"house", StandInKind::House},            {"house_roof", StandInKind::House},
        {"house_back", StandInKind::HouseBack},  {"back_house", StandInKind::HouseBack},   {"tower", StandInKind::GlassTower},
        {"glass_tower", StandInKind::GlassTower}, {"landmark", StandInKind::Landmark},
        {"street", StandInKind::StreetTree},     {"park", StandInKind::ParkTree},          {"forecourt", StandInKind::ParkTree}, {"plaza", StandInKind::ParkTree},
        {"precinct", StandInKind::ParkTree},     {"glass", StandInKind::GlassTower},       {"bridge_deck", StandInKind::BridgeDeck},
        {"cherry", StandInKind::StreetTree},     {"cherry_old", StandInKind::ParkTree},
        {"bridge_landing", StandInKind::BridgeLanding}, {"bridge_stair", StandInKind::BridgeStair}, {"bridge_pier", StandInKind::BridgePier},
    };
    text = trimView(text);
    for (const Name& n : kNames) {
        if (equalsNoCase(text, n.text)) {
            out = n.kind;
            return true;
        }
    }
    return false;
}

bool importStandIns(std::string_view csv, StandInKind defaultKind, std::vector<StandInPoint>& out, std::string* error,
                    StandInImportStats* stats) {
    enum Col { Kind, X, Y, X0, Y0, X1, Y1, Z, Top, Rot, Len, Wid, H, Floors, Scale, Id, Back, Slope, Pitch, Canopy, Ridge, Count };
    // Each column's names, the first found wins.
    static constexpr const char* kAliases[Count][6] = {
        {"kind", "type"},
        {"x", "cx", "px"},
        {"y", "cy", "py"},
        {"x0", "ax"},
        {"y0", "ay"},
        {"x1", "bx"},
        {"y1", "by"},
        {"z", "z0", "ground", "ground_z"},
        {"z1", "eave", "eave_z", "top"},
        {"rot", "yaw", "rot_deg", "heading"},
        {"len", "length", "length_m", "sx", "span"},
        {"wid", "width", "width_m", "sy", "w"},
        {"h", "height", "height_m"},
        {"floors"},
        {"scale"},
        {"id"},
        {"back", "hidden"},
        {"slope"},
        {"pitch", "pitch_deg"},
        {"canopy", "canopy_m"},
        {"ridge"},
    };
    constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    std::size_t col[Count];
    std::fill(col, col + Count, kNone);
    std::size_t width = 0;
    bool haveHeader = false, spans = false;
    std::vector<std::string_view> f;
    StandInImportStats local;
    StandInImportStats& st = stats ? *stats : local;

    std::size_t lineNo = 0, pos = 0;
    while (pos <= csv.size()) {
        std::size_t end = csv.find('\n', pos);
        if (end == std::string_view::npos) end = csv.size();
        std::string_view line = csv.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const std::string_view t = trimView(line);
        if (t.empty() || t.front() == '#') {
            if (end == csv.size()) break;
            continue;
        }
        splitLine(line, f);
        if (!haveHeader) {
            for (std::size_t c = 0; c < Count; ++c) {
                for (const char* name : kAliases[c]) {
                    if (!name || col[c] != kNone) break;
                    for (std::size_t i = 0; i < f.size(); ++i) {
                        if (equalsNoCase(f[i], name)) {
                            col[c] = i;
                            break;
                        }
                    }
                }
            }
            const bool centres = col[X] != kNone && col[Y] != kNone;
            spans = !centres && col[X0] != kNone && col[Y0] != kNone && col[X1] != kNone && col[Y1] != kNone;
            if (!centres && !spans) return fail(error, lineNo, "no x, y (or x0, y0, x1, y1) columns");
            if (col[Kind] == kNone && defaultKind == StandInKind::Count) return fail(error, lineNo, "no kind column");
            width = f.size();
            haveHeader = true;
            if (end == csv.size()) break;
            continue;
        }
        ++st.rows;
        if (f.size() < width) return fail(error, lineNo, std::to_string(f.size()) + " fields where the header has " + std::to_string(width));
        StandInPoint p;
        if (col[Kind] != kNone) {
            if (!parseStandInKind(f[col[Kind]], p.kind)) {
                ++st.skipped;
                if (end == csv.size()) break;
                continue;
            }
            const std::string_view written = trimView(f[col[Kind]]);
            p.tag.reserve(written.size());
            for (const char ch : written) p.tag.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        } else {
            p.kind = defaultKind;
        }
        if (col[Id] != kNone) p.id = std::string(f[col[Id]]);
        const auto num = [&](Col c, double& v, const char* what) {
            if (!parseNum(f[col[c]], v)) return fail(error, lineNo, std::string(what) + " is not a number");
            return true;
        };
        if (spans) {
            double ax = 0, ay = 0, bx = 0, by = 0;
            if (!num(X0, ax, "x0") || !num(Y0, ay, "y0") || !num(X1, bx, "x1") || !num(Y1, by, "y1")) return false;
            p.x = 0.5 * (ax + bx);
            p.y = 0.5 * (ay + by);
            p.len = std::hypot(bx - ax, by - ay);
            p.rot = std::atan2(by - ay, bx - ax) / kDeg;
        } else if (!num(X, p.x, "x") || !num(Y, p.y, "y")) {
            return false;
        }
        // Optional numbers: an empty field keeps the default.
        const auto opt = [&](Col c, double& v, const char* what) {
            if (col[c] == kNone || f[col[c]].empty()) return true;
            return num(c, v, what);
        };
        double floors = 0.0, scale = 1.0;
        if (!opt(Rot, p.rot, "rot") || !opt(H, p.h, "h") || !opt(Floors, floors, "floors") || !opt(Scale, scale, "scale") ||
            !opt(Slope, p.slope, "slope") || !opt(Pitch, p.pitch, "pitch") || !opt(Canopy, p.canopy, "canopy"))
            return false;
        if (!spans && (!opt(Len, p.len, "len") || !opt(Wid, p.wid, "wid"))) return false;
        if (spans && !opt(Wid, p.wid, "wid")) return false;
        if (col[Z] != kNone && !f[col[Z]].empty()) {
            if (!num(Z, p.z, "z")) return false;
            p.hasZ = true;
        }
        if (col[Top] != kNone && !f[col[Top]].empty()) {
            if (!num(Top, p.top, "z1")) return false;
            p.hasTop = true;
        }
        const bool roof = p.kind == StandInKind::House || p.kind == StandInKind::HouseBack;
        if (roof && p.hasTop) {
            p.z = p.top; // a roof stands on its eaves
            p.hasZ = true;
        }
        if (roof && col[Ridge] != kNone && equalsNoCase(f[col[Ridge]], "w")) {
            std::swap(p.len, p.wid);
            p.rot += 90.0;
        }
        if (col[Back] != kNone && p.kind == StandInKind::House) {
            const std::string_view b = f[col[Back]];
            if (b == "1" || equalsNoCase(b, "true") || equalsNoCase(b, "yes")) p.kind = StandInKind::HouseBack;
        }
        if (p.len < 0.0 || p.wid < 0.0 || p.h < 0.0 || floors < 0.0 || p.canopy < 0.0) return fail(error, lineNo, "negative size");
        p.floors = static_cast<int>(std::lround(floors));
        p.scale = static_cast<float>(scale);
        out.push_back(std::move(p));
        ++st.kept;
        if (end == csv.size()) break;
    }
    if (!haveHeader) return fail(error, lineNo, "no header row");
    return true;
}

} // namespace ftcore

// ---- The tree canopies -------------------------------------------------------------------------------------------

namespace ftcore {

namespace {

// In the raw frame: the fork at the origin, the crown about a unit wide, z up.
struct Clump {
    double x = 0.0, y = 0.0, z = 0.0, r = 0.0;
};
struct Limb {
    double a[3] = {}, b[3] = {};
    double ra = 0.0, rb = 0.0;
};
struct CanopyLayout {
    Clump clumps[8];
    std::size_t clumpCount = 0;
    Limb limbs[6];
    std::size_t limbCount = 0;
};

// A clump's limb end: inside it, pulled towards the trunk's axis and down.
void limbEnd(const Clump& c, double* out) {
    out[0] = 0.75 * c.x;
    out[1] = 0.75 * c.y;
    out[2] = c.z - 0.3 * c.r;
}

// Street (0): four clumps in a ring with gaps between them and one over the axis; a single fork into three limbs.
// Park (1, 2): six smaller ring clumps and one on top; two forks (one limb splits again half way).
CanopyLayout canopyLayout(std::size_t variant) {
    CanopyLayout l;
    Seed r;
    r.base = fnv(r.base, 0x7ee5u + static_cast<std::uint32_t>(variant));
    const bool park = variant > 0;
    const std::size_t ring = park ? 6 : 4;
    const double step = 2.0 * kPi / static_cast<double>(ring);
    const double a0 = 2.0 * kPi * r.u(1);
    for (std::size_t i = 0; i < ring; ++i) {
        const std::uint32_t s = 10u + 10u * static_cast<std::uint32_t>(i);
        const double a = a0 + step * (static_cast<double>(i) + 0.4 * (r.u(s) - 0.5));
        const double d = (park ? 0.34 : 0.3) + 0.08 * r.u(s + 1);
        Clump& c = l.clumps[l.clumpCount++];
        c.r = (park ? 0.13 : 0.15) + (park ? 0.05 : 0.06) * r.u(s + 2);
        c.x = d * std::cos(a);
        c.y = d * std::sin(a);
        c.z = 0.42 + 0.2 * r.u(s + 3) + (i % 2 == 1 ? 0.09 : -0.03);
    }
    {
        const double a = 2.0 * kPi * r.u(90);
        Clump& c = l.clumps[l.clumpCount++];
        c.r = park ? 0.19 : 0.2;
        c.x = 0.08 * std::cos(a);
        c.y = 0.08 * std::sin(a);
        c.z = 0.78 + 0.06 * r.u(91);
    }
    const Clump top = l.clumps[l.clumpCount - 1];
    const double base = 0.024, tip = 0.011;
    const auto limb = [&l](const double* a, const double* b, double ra, double rb) {
        Limb& m = l.limbs[l.limbCount++];
        for (int k = 0; k < 3; ++k) {
            m.a[k] = a[k];
            m.b[k] = b[k];
        }
        m.ra = ra;
        m.rb = rb;
    };
    const double fork[3] = {0.0, 0.0, 0.0};
    double end[3];
    limbEnd(l.clumps[0], end);
    limb(fork, end, base, tip);
    limbEnd(top, end);
    limb(fork, end, base * 0.85, tip);
    if (!park) {
        limbEnd(l.clumps[2], end);
        limb(fork, end, base, tip);
    } else {
        // The second fork: a limb towards clumps 2 and 4, splitting a little under the crown.
        double e2[3], e4[3];
        limbEnd(l.clumps[2], e2);
        limbEnd(l.clumps[4], e4);
        const double split[3] = {0.3 * (e2[0] + e4[0]), 0.3 * (e2[1] + e4[1]), 0.24};
        limb(fork, split, base, 0.017);
        limb(split, e2, 0.016, tip);
        limb(split, e4, 0.016, tip);
    }
    return l;
}

// A unit icosphere subdivided once: 42 vertices, 80 triangles.
void icosphere(std::vector<double>& v, std::vector<std::uint32_t>& f) {
    const double t = (1.0 + std::sqrt(5.0)) / 2.0;
    const double base[12][3] = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                                {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    const std::uint32_t faces[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
                                        {11, 10, 2}, {10, 7, 6}, {7, 1, 8},  {3, 9, 4},  {3, 4, 2},   {3, 2, 6}, {3, 6, 8},
                                        {3, 8, 9},  {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},   {9, 8, 1}};
    v.clear();
    f.clear();
    const auto push = [&v](double x, double y, double z) {
        const double n = std::sqrt(x * x + y * y + z * z);
        v.push_back(x / n);
        v.push_back(y / n);
        v.push_back(z / n);
        return static_cast<std::uint32_t>(v.size() / 3 - 1);
    };
    for (const auto& p : base) push(p[0], p[1], p[2]);
    std::vector<std::pair<std::uint64_t, std::uint32_t>> mids;
    const auto mid = [&](std::uint32_t a, std::uint32_t b) {
        const std::uint64_t key = (static_cast<std::uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
        for (const auto& m : mids)
            if (m.first == key) return m.second;
        const std::uint32_t i = push(v[3 * a] + v[3 * b], v[3 * a + 1] + v[3 * b + 1], v[3 * a + 2] + v[3 * b + 2]);
        mids.emplace_back(key, i);
        return i;
    };
    for (const auto& tri : faces) {
        const std::uint32_t ab = mid(tri[0], tri[1]), bc = mid(tri[1], tri[2]), ca = mid(tri[2], tri[0]);
        const std::uint32_t quads[4][3] = {{tri[0], ab, ca}, {tri[1], bc, ab}, {tri[2], ca, bc}, {ab, bc, ca}};
        for (const auto& q : quads) f.insert(f.end(), {q[0], q[1], q[2]});
    }
}

} // namespace

bool standInCanopyMesh(std::size_t variant, StandInCanopyMesh& out) {
    out.positions.clear();
    out.triangles.clear();
    out.normals.clear();
    out.slots.clear();
    if (variant >= kStandInCanopyVariants) return false;
    const CanopyLayout l = canopyLayout(variant);
    Seed r;
    r.base = fnv(r.base, 0x10b5u + static_cast<std::uint32_t>(variant));

    // Raw vertices, and per triangle the point its normal turns away from.
    std::vector<double> pos;
    std::vector<double> centres; // per triangle (x, y, z): its clump's centre, or its limb's axis point at its height
    std::vector<std::uint32_t> tris;
    std::vector<std::uint8_t> slots;
    std::vector<double> sv;
    std::vector<std::uint32_t> sf;
    icosphere(sv, sf);
    for (std::size_t c = 0; c < l.clumpCount; ++c) {
        const Clump& k = l.clumps[c];
        const std::uint32_t first = static_cast<std::uint32_t>(pos.size() / 3);
        for (std::size_t i = 0; i < sv.size() / 3; ++i) {
            // Lumpy (+-18% on the radius), flattened a little.
            const double lump = 1.0 + 0.36 * (r.u(static_cast<std::uint32_t>(1000 * c + i)) - 0.5);
            pos.push_back(k.x + k.r * lump * sv[3 * i]);
            pos.push_back(k.y + k.r * lump * sv[3 * i + 1]);
            pos.push_back(k.z + 0.8 * k.r * lump * sv[3 * i + 2]);
        }
        for (std::size_t t = 0; t < sf.size(); t += 3) {
            tris.insert(tris.end(), {first + sf[t], first + sf[t + 1], first + sf[t + 2]});
            centres.insert(centres.end(), {k.x, k.y, k.z});
            slots.push_back(0);
        }
    }
    // The limbs: five-sided tapered prisms with level ends, open (their ends are inside the trunk or the leaves).
    constexpr int kSides = 5;
    for (std::size_t m = 0; m < l.limbCount; ++m) {
        const Limb& b = l.limbs[m];
        const std::uint32_t first = static_cast<std::uint32_t>(pos.size() / 3);
        for (int end = 0; end < 2; ++end) {
            const double* p = end == 0 ? b.a : b.b;
            const double rad = end == 0 ? b.ra : b.rb;
            for (int s = 0; s < kSides; ++s) {
                const double a = 2.0 * kPi * (s + 0.5 * end) / kSides;
                pos.insert(pos.end(), {p[0] + rad * std::cos(a), p[1] + rad * std::sin(a), p[2]});
            }
        }
        for (int s = 0; s < kSides; ++s) {
            const std::uint32_t a0 = first + static_cast<std::uint32_t>(s), a1 = first + static_cast<std::uint32_t>((s + 1) % kSides);
            const std::uint32_t b0 = a0 + kSides, b1 = a1 + kSides;
            const std::array<std::uint32_t, 3> quad[2] = {{a0, a1, b0}, {a1, b1, b0}};
            for (const auto& t : quad) {
                tris.insert(tris.end(), {t[0], t[1], t[2]});
                // The axis point at the triangle's centroid's height.
                double cz = 0.0;
                for (std::uint32_t v : t) cz += pos[3 * v + 2] / 3.0;
                const double dz = b.b[2] - b.a[2];
                const double s01 = std::clamp(dz != 0.0 ? (cz - b.a[2]) / dz : 0.5, 0.0, 1.0);
                centres.insert(centres.end(), {b.a[0] + s01 * (b.b[0] - b.a[0]), b.a[1] + s01 * (b.b[1] - b.a[1]), cz});
                slots.push_back(1);
            }
        }
    }

    // Into the unit box around the fork.
    double halfX = 0.0, halfY = 0.0, topZ = 0.0;
    for (std::size_t i = 0; i < pos.size(); i += 3) {
        halfX = std::max(halfX, std::abs(pos[i]));
        halfY = std::max(halfY, std::abs(pos[i + 1]));
        topZ = std::max(topZ, pos[i + 2]);
    }
    const double kx = 0.5 / halfX, ky = 0.5 / halfY, kz = 1.0 / topZ;
    const auto unit = [kx, ky, kz](double* p) {
        p[0] *= kx;
        p[1] *= ky;
        p[2] = p[2] * kz - 0.5;
    };
    for (std::size_t i = 0; i < pos.size(); i += 3) unit(&pos[i]);
    for (std::size_t i = 0; i < centres.size(); i += 3) unit(&centres[i]);

    out.positions.reserve(pos.size());
    for (double p : pos) out.positions.push_back(static_cast<float>(p));
    out.slots = slots;
    out.triangles.reserve(tris.size());
    out.normals.reserve(tris.size());
    std::vector<double> vn(pos.size(), 0.0); // per vertex: its triangles' outward normals, each weighted by its area
    for (std::size_t t = 0; t < tris.size(); t += 3) {
        const double* a = &pos[3 * tris[t]];
        const double* b = &pos[3 * tris[t + 1]];
        const double* c = &pos[3 * tris[t + 2]];
        const double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const double v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const double n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        const double* o = &centres[t];
        const double away[3] = {(a[0] + b[0] + c[0]) / 3.0 - o[0], (a[1] + b[1] + c[1]) / 3.0 - o[1], (a[2] + b[2] + c[2]) / 3.0 - o[2]};
        const bool flip = n[0] * away[0] + n[1] * away[1] + n[2] * away[2] < 0.0;
        // Counter-clockwise seen from outside in this right-handed frame (the normal is the cross product); the engine
        // side winds each triangle as it needs from the normal.
        out.triangles.push_back(tris[t]);
        out.triangles.push_back(flip ? tris[t + 2] : tris[t + 1]);
        out.triangles.push_back(flip ? tris[t + 1] : tris[t + 2]);
        const double sign = (flip ? -1.0 : 1.0) / (len > 0.0 ? len : 1.0);
        for (int k = 0; k < 3; ++k) out.normals.push_back(static_cast<float>(n[k] * sign));
        for (std::size_t corner = 0; corner < 3; ++corner) {
            for (std::size_t k = 0; k < 3; ++k) vn[3 * tris[t + corner] + k] += n[k] * (flip ? -1.0 : 1.0);
        }
    }
    out.vertexNormals.clear();
    out.vertexNormals.reserve(vn.size());
    for (std::size_t i = 0; i < vn.size(); i += 3) {
        const double len = std::sqrt(vn[i] * vn[i] + vn[i + 1] * vn[i + 1] + vn[i + 2] * vn[i + 2]);
        // A vertex no triangle uses (none here) points up.
        out.vertexNormals.push_back(static_cast<float>(len > 0.0 ? vn[i] / len : 0.0));
        out.vertexNormals.push_back(static_cast<float>(len > 0.0 ? vn[i + 1] / len : 0.0));
        out.vertexNormals.push_back(static_cast<float>(len > 0.0 ? vn[i + 2] / len : 1.0));
    }
    return true;
}

void standInGableMesh(StandInCanopyMesh& out) {
    out.positions = {-0.5f, -0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, -0.5f, -0.5f, 0.5f, -0.5f, // the floor: 0 1 2 3
                     -0.5f, 0.f,   0.5f,  0.5f, 0.f,   0.5f};                                          // the ridge: 4 5
    // Counter-clockwise seen from outside (right-handed).
    out.triangles = {0, 1, 5, 0, 5, 4,  // the y < 0 slope
                     2, 3, 4, 2, 4, 5,  // the y > 0 slope
                     1, 2, 5,           // the +x gable
                     3, 0, 4,           // the -x gable
                     0, 3, 2, 0, 2, 1}; // the floor
    out.slots.assign(out.triangles.size() / 3, 0);
    out.vertexNormals.clear();
    out.normals.clear();
    for (std::size_t t = 0; t < out.triangles.size(); t += 3) {
        const float* a = &out.positions[3 * out.triangles[t]];
        const float* b = &out.positions[3 * out.triangles[t + 1]];
        const float* q = &out.positions[3 * out.triangles[t + 2]];
        const double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const double v[3] = {q[0] - a[0], q[1] - a[1], q[2] - a[2]};
        const double n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (double k : n) out.normals.push_back(static_cast<float>(k / len));
    }
}

// ---- The corridor rule ----

bool importStandInCorridors(std::string_view csv, std::vector<StandInCorridor>& out, std::vector<std::string>* ids, std::string_view skipKind,
                            std::string* error) {
    constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    enum Col { Id, Kind, Width, Pts, Pavement, Count };
    const char* const names[Count] = {"id", "kind", "width_m", "pts", "pavement_m"};
    std::size_t col[Count];
    std::fill(col, col + Count, kNone);
    bool haveHeader = false;
    std::vector<std::string_view> f;
    std::uint32_t street = 0;
    std::size_t lineNo = 0, pos = 0;
    while (pos <= csv.size()) {
        std::size_t end = csv.find('\n', pos);
        if (end == std::string_view::npos) end = csv.size();
        std::string_view line = csv.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const std::string_view t = trimView(line);
        if (t.empty() || t.front() == '#') continue;
        splitLine(line, f);
        if (!haveHeader) {
            for (std::size_t k = 0; k < Count; ++k) {
                for (std::size_t i = 0; i < f.size() && col[k] == kNone; ++i) {
                    if (equalsNoCase(f[i], names[k])) col[k] = i;
                }
            }
            if (col[Width] == kNone || col[Pts] == kNone) return fail(error, lineNo, "no width_m or pts column");
            haveHeader = true;
            continue;
        }
        const auto field = [&f, &col](Col k) { return col[k] != kNone && col[k] < f.size() ? f[col[k]] : std::string_view(); };
        if (!skipKind.empty() && equalsNoCase(field(Kind), skipKind)) continue;
        if (field(Width).empty()) continue;
        double width = 0.0, pavement = 0.0;
        if (!parseNum(field(Width), width) || width <= 0.0) return fail(error, lineNo, "bad width_m");
        if (!field(Pavement).empty() && !parseNum(field(Pavement), pavement)) return fail(error, lineNo, "bad pavement_m");
        const double clear = pavement > 0.0 && 2.0 * pavement < width ? width - 2.0 * pavement : width;
        // The points: x:y[:z] joined by ';'.
        const std::string_view pts = field(Pts);
        double px = 0.0, py = 0.0;
        bool havePrev = false;
        for (std::size_t p = 0; p < pts.size();) {
            std::size_t q = pts.find(';', p);
            if (q == std::string_view::npos) q = pts.size();
            const std::string_view one = trimView(pts.substr(p, q - p));
            p = q + 1;
            if (one.empty()) continue;
            const std::size_t c1 = one.find(':');
            if (c1 == std::string_view::npos) return fail(error, lineNo, "a point is x:y");
            std::size_t c2 = one.find(':', c1 + 1);
            if (c2 == std::string_view::npos) c2 = one.size();
            double x = 0.0, y = 0.0;
            if (!parseNum(one.substr(0, c1), x) || !parseNum(one.substr(c1 + 1, c2 - c1 - 1), y)) return fail(error, lineNo, "a bad point");
            if (havePrev) {
                StandInCorridor cor;
                cor.ax = px;
                cor.ay = py;
                cor.bx = x;
                cor.by = y;
                cor.half = 0.5 * clear;
                cor.street = street;
                out.push_back(cor);
                if (ids) ids->emplace_back(field(Id));
            }
            px = x;
            py = y;
            havePrev = true;
        }
        ++street;
    }
    if (!haveHeader) return fail(error, lineNo, "no header row");
    return true;
}

StandInBounds standInBounds(const StandInPoint& p, const StandInPart& part) {
    // The engine turns a part by its roll (about x), then its pitch (about y), then its yaw: the box round each turn.
    const double cr = std::abs(std::cos(part.roll * kDeg)), sr = std::abs(std::sin(part.roll * kDeg));
    const double cp = std::abs(std::cos(part.pitch * kDeg)), sp = std::abs(std::sin(part.pitch * kDeg));
    const double hx0 = 0.5 * part.sx, hy0 = 0.5 * part.sy, hz0 = 0.5 * part.sz;
    const double hy1 = hy0 * cr + hz0 * sr;
    const double hz1 = hy0 * sr + hz0 * cr;
    const double hz2 = hx0 * sp + hz1 * cp;
    StandInBounds b;
    b.cx = p.x + part.dx;
    b.cy = p.y + part.dy;
    b.hx = hx0 * cp + hz1 * sp;
    b.hy = hy1;
    b.yaw = part.yaw;
    b.bottom = part.dz - hz2;
    b.top = part.dz + hz2;
    return b;
}

namespace {

double pointToRect(double x, double y, double hx, double hy) {
    const double dx = std::max(std::abs(x) - hx, 0.0), dy = std::max(std::abs(y) - hy, 0.0);
    return std::sqrt(dx * dx + dy * dy);
}

double pointToSegment(double x, double y, double ax, double ay, double bx, double by) {
    const double ux = bx - ax, uy = by - ay;
    const double l2 = ux * ux + uy * uy;
    const double t = l2 > 0.0 ? std::clamp(((x - ax) * ux + (y - ay) * uy) / l2, 0.0, 1.0) : 0.0;
    const double dx = x - (ax + t * ux), dy = y - (ay + t * uy);
    return std::sqrt(dx * dx + dy * dy);
}

// True when the segment a-b passes through the rectangle |x| <= hx, |y| <= hy (a slab clip on each axis).
bool segmentInRect(double ax, double ay, double bx, double by, double hx, double hy) {
    double t0 = 0.0, t1 = 1.0;
    const double a[2] = {ax, ay}, d[2] = {bx - ax, by - ay}, h[2] = {hx, hy};
    for (int k = 0; k < 2; ++k) {
        if (std::abs(d[k]) < 1e-12) {
            if (std::abs(a[k]) > h[k]) return false;
            continue;
        }
        double u0 = (-h[k] - a[k]) / d[k], u1 = (h[k] - a[k]) / d[k];
        if (u0 > u1) std::swap(u0, u1);
        t0 = std::max(t0, u0);
        t1 = std::min(t1, u1);
        if (t0 > t1) return false;
    }
    return true;
}

} // namespace

double standInCorridorDepth(const StandInBounds& b, const StandInCorridor& c) {
    // The corridor's centre line in the rectangle's own frame.
    const double co = std::cos(b.yaw * kDeg), si = std::sin(b.yaw * kDeg);
    const auto local = [&b, co, si](double x, double y, double& lx, double& ly) {
        const double dx = x - b.cx, dy = y - b.cy;
        lx = dx * co + dy * si;
        ly = -dx * si + dy * co;
    };
    double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0;
    local(c.ax, c.ay, ax, ay);
    local(c.bx, c.by, bx, by);
    if (segmentInRect(ax, ay, bx, by, b.hx, b.hy)) return c.half;
    double d = std::min(pointToRect(ax, ay, b.hx, b.hy), pointToRect(bx, by, b.hx, b.hy));
    for (int k = 0; k < 4; ++k) {
        d = std::min(d, pointToSegment((k & 1) ? b.hx : -b.hx, (k & 2) ? b.hy : -b.hy, ax, ay, bx, by));
    }
    return c.half - d;
}

std::size_t standInCorridorCheck(const StandInPoint* points, const double* footOverGround, std::size_t count, const StandInCorridor* corridors,
                                 std::size_t corridorCount, const StandInCorridorSpec& spec, StandInCorridorHit* hits, std::size_t hitCap) {
    if (!points || !corridors) return 0;
    std::size_t found = 0;
    StandInPart parts[kStandInPartsMax];
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t n = standInParts(points[i], parts);
        const double foot = footOverGround ? footOverGround[i] : 0.0;
        for (std::size_t k = 0; k < n; ++k) {
            if (parts[k].shape == StandInShape::Canopy) continue;
            const StandInBounds b = standInBounds(points[i], parts[k]);
            if (foot + b.bottom >= spec.clearance) continue;
            const double reach = std::sqrt(b.hx * b.hx + b.hy * b.hy);
            StandInCorridorHit hit;
            hit.depth = spec.slack;
            bool any = false;
            for (std::size_t c = 0; c < corridorCount; ++c) {
                const StandInCorridor& cor = corridors[c];
                // Far off: the bounds' circle against the corridor's own box.
                const double pad = reach + cor.half;
                if (b.cx < std::min(cor.ax, cor.bx) - pad || b.cx > std::max(cor.ax, cor.bx) + pad || b.cy < std::min(cor.ay, cor.by) - pad ||
                    b.cy > std::max(cor.ay, cor.by) + pad) {
                    continue;
                }
                const double depth = standInCorridorDepth(b, cor);
                if (depth > hit.depth) {
                    hit.depth = depth;
                    hit.corridor = c;
                    any = true;
                }
            }
            if (!any) continue;
            hit.point = i;
            hit.part = k;
            if (hits && found < hitCap) hits[found] = hit;
            ++found;
        }
    }
    return found;
}

double standInCanopyForkShare(std::size_t variant) { return variant < kStandInCanopyVariants ? kCanopyForkShare[variant] : 0.0; }

} // namespace ftcore
