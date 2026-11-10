#include "ftcore/StandIns.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
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

StandInPaint leafOf(float u) {
    return u < 0.45f ? StandInPaint::Leaf : (u < 0.75f ? StandInPaint::LeafDark : StandInPaint::LeafLight);
}

void tree(Emit& e, const Seed& r, bool park) {
    const double k = std::clamp(static_cast<double>(e.p.scale), 0.6, 1.6);
    const double crownW = e.p.canopy > 0.0 ? e.p.canopy : (park ? 7.0 + 3.0 * r.u(1) : 4.5 + 1.8 * r.u(1)) * k;
    double crownH = crownW * (0.72 + 0.16 * r.u(2));
    double clear = (park ? 2.6 + 1.0 * r.u(3) : 2.8 + 0.8 * r.u(3)) * k; // the trunk's clear height under the crown
    if (e.p.h > 0.0) {
        // A given height: the crown's top there, on a clear trunk of at least 1.8 m.
        crownH = std::min(crownH, 0.6 * e.p.h);
        clear = std::max(e.p.h - crownH, 1.8);
    }
    const double crownZ = clear + crownH * 0.5;
    const double trunk = clear + crownH * 0.35;
    const double girth = (park ? 0.45 : 0.3) * k;
    e.add(StandInShape::Cylinder, StandInPaint::Bark, StandInCollide::Solid, 0, 0, trunk * 0.5, girth, girth, trunk);
    const StandInPaint leaf = leafOf(r.u(4));
    e.add(StandInShape::Sphere, leaf, StandInCollide::None, 0, 0, crownZ, crownW, crownW * (0.85 + 0.1 * r.u(5)), crownH, 360.0 * r.u(6));
    const std::size_t extra = park ? 2 : 1;
    for (std::size_t i = 0; i < extra; ++i) {
        const std::uint32_t salt = 10u + 10u * static_cast<std::uint32_t>(i);
        const double a = 2.0 * kPi * r.u(salt);
        const double off = crownW * (0.25 + 0.12 * r.u(salt + 1));
        const double w = crownW * (0.55 + 0.15 * r.u(salt + 2));
        e.add(StandInShape::Sphere, i == 0 ? leaf : leafOf(r.u(salt + 3)), StandInCollide::None, off * std::cos(a), off * std::sin(a),
              crownZ + crownH * (0.05 + 0.15 * r.u(salt + 4)), w, w * 0.9, w * 0.75, 360.0 * r.u(salt + 5));
    }
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
    // A cube rolled 45 degrees whose top corner reaches the ridge: the gables' infill, its lower half inside the box.
    const double side45 = rise * std::sqrt(2.0);
    e.add(StandInShape::Cube, StandInPaint::Concrete, StandInCollide::None, 0, 0, 0.0, len, side45, side45, 0.0, 45.0);
}

// A back house: one cube rolled 45 degrees, its upper half a 45-degree gable over the box's full width.
void houseBack(Emit& e, const Seed& r) {
    const double len = e.p.len > 0.0 ? e.p.len : 9.0;
    const double wid = e.p.wid > 0.0 ? e.p.wid : 8.0;
    const double side45 = 0.5 * wid * std::sqrt(2.0);
    e.add(StandInShape::Cube, roofPaint(r.u(2)), StandInCollide::None, 0, 0, 0.0, len, side45, side45, 0.0, 45.0);
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
