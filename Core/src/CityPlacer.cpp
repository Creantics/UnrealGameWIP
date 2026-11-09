#include "ftcore/CityPlacer.h"

#include "ftcore/Json.h"

#include "SeedMix.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <unordered_set>
#include <utility>

namespace ftcore {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kEps = 1e-9;

// xorshift32 on a mixed seed: the walk's only source of chance.
struct Rng {
    std::uint32_t s;
    explicit Rng(std::uint32_t seed) : s(detail::mixSeed(seed)) {}
    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    // [0, 1)
    double uniform() { return static_cast<double>(next() >> 8) * (1.0 / 16777216.0); }
};

std::uint32_t hashText(std::string_view s) {
    std::uint32_t h = 2166136261u;
    for (const char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619u;
    }
    return h;
}

bool failWith(std::string* error, const std::string& what) {
    if (error) *error = what;
    return false;
}

std::string fmt(const char* format, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, format, v);
    return buf;
}

double normaliseDeg(double deg) {
    double r = std::fmod(deg, 360.0);
    if (r <= -180.0) r += 360.0;
    if (r > 180.0) r -= 360.0;
    return r;
}

bool validId(std::string_view id) {
    if (id.empty()) return false;
    for (const char c : id)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '.'))
            return false;
    return true;
}

bool validName(std::string_view name) {
    if (name.empty()) return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

// The no-build bit that names a reason: the lowest set; 64 and 128 read as Other.
PlaceReject noBuildReason(std::uint8_t bits) {
    if (bits & static_cast<std::uint8_t>(NoBuild::Water)) return PlaceReject::Water;
    if (bits & static_cast<std::uint8_t>(NoBuild::Band)) return PlaceReject::Band;
    if (bits & static_cast<std::uint8_t>(NoBuild::Shoulder)) return PlaceReject::Shoulder;
    if (bits & static_cast<std::uint8_t>(NoBuild::Carriageway)) return PlaceReject::Carriageway;
    if (bits & static_cast<std::uint8_t>(NoBuild::Hold)) return PlaceReject::Hold;
    return PlaceReject::OtherMask;
}

// A candidate's envelope: a rectangle along its facing.
struct Envelope {
    double cx = 0.0, cy = 0.0; // the envelope's centre
    double depth = 0.0;        // along the facing
    double width = 0.0;        // across it
    double rot = 0.0;          // the facing, degrees
};

Envelope envelopeOf(const PlacerClass& c, const PlacerVariant& v, double bodyX, double bodyY, double rotDeg) {
    const double a = rotDeg * kPi / 180.0;
    const double shift = (c.frontGrowth - c.rearGrowth) * 0.5;
    Envelope e;
    e.cx = bodyX + std::cos(a) * shift;
    e.cy = bodyY + std::sin(a) * shift;
    e.depth = v.depth + c.frontGrowth + c.rearGrowth;
    e.width = v.width;
    e.rot = rotDeg;
    return e;
}

MassingBox boxOf(const Envelope& e) {
    MassingBox b;
    b.cx = e.cx;
    b.cy = e.cy;
    b.sx = e.depth;
    b.sy = e.width;
    b.rot = e.rot;
    return b;
}

// What stands under an envelope.
struct Under {
    double groundMax = -kInf, groundMin = kInf;
    double slopeMax = 0.0;
    double capMin = kInf;
    std::uint8_t bits = 0;
};

// False when an envelope corner lies outside the grid's samples.
bool sampleUnder(const PlacerGrid& g, const Envelope& e, Under& u) {
    const double a = e.rot * kPi / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    const double hu = e.depth * 0.5, hw = e.width * 0.5;
    const double hx = std::fabs(c) * hu + std::fabs(s) * hw;
    const double hy = std::fabs(s) * hu + std::fabs(c) * hw;
    const double maxX = g.x0 + static_cast<double>(g.nx - 1) * g.cell;
    const double maxY = g.y0 + static_cast<double>(g.ny - 1) * g.cell;
    const double tol = 1e-6 * g.cell;
    if (e.cx - hx < g.x0 - tol || e.cx + hx > maxX + tol || e.cy - hy < g.y0 - tol || e.cy + hy > maxY + tol)
        return false;
    const auto lo = [&](double v, double origin, std::size_t n) {
        const double f = std::ceil((v - origin) / g.cell - 1e-9);
        return static_cast<std::size_t>(std::clamp(f, 0.0, static_cast<double>(n - 1)));
    };
    const auto hi = [&](double v, double origin, std::size_t n) {
        const double f = std::floor((v - origin) / g.cell + 1e-9);
        return static_cast<std::size_t>(std::clamp(f, 0.0, static_cast<double>(n - 1)));
    };
    const std::size_t i0 = lo(e.cx - hx, g.x0, g.nx), i1 = hi(e.cx + hx, g.x0, g.nx);
    const std::size_t j0 = lo(e.cy - hy, g.y0, g.ny), j1 = hi(e.cy + hy, g.y0, g.ny);
    bool any = false;
    const auto take = [&](std::size_t i, std::size_t j) {
        const std::size_t k = j * g.nx + i;
        const double h = g.height[k];
        u.groundMax = std::max(u.groundMax, h);
        u.groundMin = std::min(u.groundMin, h);
        u.slopeMax = std::max(u.slopeMax, g.slopeDegAt(i, j));
        if (!g.maxRoof.empty()) u.capMin = std::min(u.capMin, static_cast<double>(g.maxRoof[k]));
        if (!g.noBuild.empty()) u.bits = static_cast<std::uint8_t>(u.bits | g.noBuild[k]);
        any = true;
    };
    if (i0 <= i1 && j0 <= j1) {
        for (std::size_t j = j0; j <= j1; ++j) {
            const double py = g.y0 + static_cast<double>(j) * g.cell - e.cy;
            for (std::size_t i = i0; i <= i1; ++i) {
                const double px = g.x0 + static_cast<double>(i) * g.cell - e.cx;
                const double lu = px * c + py * s, lw = -px * s + py * c;
                if (std::fabs(lu) <= hu + kEps && std::fabs(lw) <= hw + kEps) take(i, j);
            }
        }
    }
    if (!any) {
        const double fi = std::round((e.cx - g.x0) / g.cell), fj = std::round((e.cy - g.y0) / g.cell);
        take(static_cast<std::size_t>(std::clamp(fi, 0.0, static_cast<double>(g.nx - 1))),
             static_cast<std::size_t>(std::clamp(fj, 0.0, static_cast<double>(g.ny - 1))));
    }
    return true;
}

// A building already placed, as the overlap test reads it.
struct Standing {
    MassingBox box;   // the envelope
    double gap = 0.0;
    double radius = 0.0;
};

// The standing buildings by square bucket: each is listed in every bucket its bounding circle touches.
struct Buckets {
    double x0 = 0.0, y0 = 0.0, size = 32.0;
    std::size_t nx = 1, ny = 1;
    std::vector<std::vector<std::uint32_t>> cells;

    void init(const PlacerGrid& g, double bucket) {
        x0 = g.x0;
        y0 = g.y0;
        const double w = static_cast<double>(g.nx - 1) * g.cell, h = static_cast<double>(g.ny - 1) * g.cell;
        size = std::max(bucket, std::max(w, h) / 2048.0); // at most 2048 buckets a side
        nx = static_cast<std::size_t>(std::floor(w / size)) + 1;
        ny = static_cast<std::size_t>(std::floor(h / size)) + 1;
        cells.assign(nx * ny, {});
    }
    void range(double cx, double cy, double r, std::size_t& i0, std::size_t& i1, std::size_t& j0, std::size_t& j1) const {
        const auto at = [&](double v, double origin, std::size_t n) {
            return static_cast<std::size_t>(std::clamp(std::floor((v - origin) / size), 0.0, static_cast<double>(n - 1)));
        };
        i0 = at(cx - r, x0, nx);
        i1 = at(cx + r, x0, nx);
        j0 = at(cy - r, y0, ny);
        j1 = at(cy + r, y0, ny);
    }
    void insert(std::uint32_t index, double cx, double cy, double r) {
        std::size_t i0, i1, j0, j1;
        range(cx, cy, r, i0, i1, j0, j1);
        for (std::size_t j = j0; j <= j1; ++j)
            for (std::size_t i = i0; i <= i1; ++i) cells[j * nx + i].push_back(index);
    }
};

// True when cand comes nearer than the larger of the two gaps to a standing envelope.
bool tooClose(const std::vector<Standing>& standing, const Buckets& buckets, const MassingBox& cand, double candGap,
              double maxGap) {
    const double candRadius = 0.5 * std::hypot(cand.sx, cand.sy);
    std::size_t i0, i1, j0, j1;
    buckets.range(cand.cx, cand.cy, candRadius + maxGap, i0, i1, j0, j1);
    MassingBox a = cand, b;
    for (std::size_t j = j0; j <= j1; ++j) {
        for (std::size_t i = i0; i <= i1; ++i) {
            for (const std::uint32_t k : buckets.cells[j * buckets.nx + i]) {
                const Standing& st = standing[k];
                const double g = std::max(candGap, st.gap);
                if (std::hypot(cand.cx - st.box.cx, cand.cy - st.box.cy) >= candRadius + st.radius + g + 1e-6) continue;
                a.sx = cand.sx + g;
                a.sy = cand.sy + g;
                b = st.box;
                b.sx += g;
                b.sy += g;
                if (massingFootprintsOverlap(a, b)) return true;
            }
        }
    }
    return false;
}

// The variety guard's running counts (PlacerVarietyGuard): per disc its total and its count per
// footprint-and-storey class (a flat index over every class's variants), the discs found through a
// bucket grid of their centres at least a radius wide.
struct GuardState {
    bool on = false;
    const std::vector<bool>* zones = nullptr;
    const std::vector<PlacerPoint>* centres = nullptr;
    double r2 = 0.0, maxShare = 0.0, maxSlopeDeg = 0.0, minBuildings = 0.0;
    std::size_t stride = 0;               // variants over all classes
    std::vector<std::size_t> firstFlat;   // per class: its first variant's flat index
    std::vector<std::uint32_t> total;     // per disc
    std::vector<std::uint32_t> count;     // disc * stride + flat
    double bx0 = 0.0, by0 = 0.0, size = 1.0;
    std::size_t bnx = 1, bny = 1;
    std::vector<std::vector<std::uint32_t>> cells; // disc indexes per bucket

    void prepare(const PlacerConfig& cfg) {
        firstFlat.clear();
        stride = 0;
        for (const PlacerClass& c : cfg.classes) {
            firstFlat.push_back(stride);
            stride += c.variants.size();
        }
    }
    std::size_t flat(std::size_t cls, std::size_t variant) const { return firstFlat[cls] + variant; }

    void init(const PlacerConfig& cfg, const PlacerVarietyGuard& g) {
        on = true;
        zones = &g.zones;
        centres = &g.centres;
        const VarietyParams& p = cfg.variety;
        r2 = p.radius * p.radius;
        maxShare = p.maxShare;
        maxSlopeDeg = p.maxSlopeDeg;
        minBuildings = static_cast<double>(p.minBuildings);
        total.assign(g.centres.size(), 0u);
        count.assign(g.centres.size() * stride, 0u);
        if (g.centres.empty()) {
            cells.assign(1, {});
            return;
        }
        double x1 = -kInf, y1 = -kInf;
        bx0 = kInf;
        by0 = kInf;
        for (const PlacerPoint& c : g.centres) {
            bx0 = std::min(bx0, c.x);
            by0 = std::min(by0, c.y);
            x1 = std::max(x1, c.x);
            y1 = std::max(y1, c.y);
        }
        size = std::max({p.radius, (x1 - bx0) / 1024.0, (y1 - by0) / 1024.0, 1e-6}); // at most 1025 buckets a side
        bnx = static_cast<std::size_t>(std::floor((x1 - bx0) / size)) + 1;
        bny = static_cast<std::size_t>(std::floor((y1 - by0) / size)) + 1;
        cells.assign(bnx * bny, {});
        for (std::size_t k = 0; k < g.centres.size(); ++k) {
            const std::size_t i = std::min(bnx - 1, static_cast<std::size_t>(std::floor((g.centres[k].x - bx0) / size)));
            const std::size_t j = std::min(bny - 1, static_cast<std::size_t>(std::floor((g.centres[k].y - by0) / size)));
            cells[j * bnx + i].push_back(static_cast<std::uint32_t>(k));
        }
    }
    bool held(std::uint32_t zi, double slopeDeg) const { return on && (*zones)[zi] && slopeDeg < maxSlopeDeg; }

    // Calls fn(disc) for each disc holding (x, y): checkVariety's own test, centre to point.
    template <class Fn>
    void eachDisc(double x, double y, Fn&& fn) const {
        const double fi = std::floor((x - bx0) / size), fj = std::floor((y - by0) / size);
        const double i0 = std::max(fi - 1.0, 0.0), i1 = std::min(fi + 1.0, static_cast<double>(bnx - 1));
        const double j0 = std::max(fj - 1.0, 0.0), j1 = std::min(fj + 1.0, static_cast<double>(bny - 1));
        if (i0 > i1 || j0 > j1) return;
        for (std::size_t j = static_cast<std::size_t>(j0); j <= static_cast<std::size_t>(j1); ++j) {
            for (std::size_t i = static_cast<std::size_t>(i0); i <= static_cast<std::size_t>(i1); ++i) {
                for (const std::uint32_t d : cells[j * bnx + i]) {
                    const PlacerPoint& c = (*centres)[d];
                    const double ddx = x - c.x, ddy = y - c.y;
                    if (ddx * ddx + ddy * ddy > r2) continue;
                    fn(static_cast<std::size_t>(d));
                }
            }
        }
    }
    bool allows(std::uint32_t zi, std::size_t flatIndex, double x, double y, double slopeDeg) const {
        if (!held(zi, slopeDeg)) return true;
        bool ok = true;
        eachDisc(x, y, [&](std::size_t d) {
            const double n = static_cast<double>(total[d]) + 1.0;
            const double c = static_cast<double>(count[d * stride + flatIndex]) + 1.0;
            if (c > std::floor(maxShare * std::max(n, minBuildings) + 1e-9)) ok = false;
        });
        return ok;
    }
    void add(std::uint32_t zi, std::size_t flatIndex, double x, double y, double slopeDeg) {
        if (!held(zi, slopeDeg)) return;
        eachDisc(x, y, [&](std::size_t d) {
            ++total[d];
            ++count[d * stride + flatIndex];
        });
    }
};

constexpr int kPlaced = -1;

struct Attempt {
    Envelope env;
    Under under;
    bool bench = false;
};

// Every test for one variant at one body position; kPlaced or the reason's index.
int attempt(const PlacerConfig& cfg, const PlacerGrid& grid, const PlacerZone& zone, const PlacerClass& pc,
            const PlacerVariant& pv, double bodyX, double bodyY, double rotDeg, const std::vector<Standing>& standing,
            const Buckets& buckets, double maxGap, const GuardState& guard, std::uint32_t zi, std::size_t flatIndex,
            Attempt& at) {
    at = Attempt{};
    at.env = envelopeOf(pc, pv, bodyX, bodyY, rotDeg);
    if (!sampleUnder(grid, at.env, at.under)) return static_cast<int>(PlaceReject::OffGrid);
    if (at.under.bits) return static_cast<int>(noBuildReason(at.under.bits));
    if (at.under.slopeMax > zone.slopeMaxDeg + kEps) {
        if (!(zone.bench && pc.benchOk && at.under.slopeMax <= cfg.benchMaxSlopeDeg + kEps))
            return static_cast<int>(PlaceReject::Slope);
        at.bench = true;
    }
    if (at.under.groundMax + pv.top > at.under.capMin + kPlacerRoofTolerance) return static_cast<int>(PlaceReject::RoofCap);
    if (tooClose(standing, buckets, boxOf(at.env), pc.gap, maxGap)) return static_cast<int>(PlaceReject::Overlap);
    if (!guard.allows(zi, flatIndex, bodyX, bodyY, at.under.slopeMax)) return static_cast<int>(PlaceReject::Variety);
    return kPlaced;
}

// Index by weight; npos when nothing weighs.
std::size_t pickWeighted(Rng& rng, const double* w, std::size_t n) {
    double total = 0.0;
    for (std::size_t i = 0; i < n; ++i) total += std::max(w[i], 0.0);
    const double u = rng.uniform();
    if (!(total > 0.0)) return static_cast<std::size_t>(-1);
    double at = u * total;
    std::size_t last = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < n; ++i) {
        if (!(w[i] > 0.0)) continue;
        last = i;
        if (at < w[i]) return i;
        at -= w[i];
    }
    return last;
}

std::size_t pickVariant(Rng& rng, const PlacerClass& c, std::vector<double>& scratch) {
    scratch.clear();
    for (const PlacerVariant& v : c.variants) scratch.push_back(v.weight);
    return pickWeighted(rng, scratch.data(), scratch.size());
}

bool validConfig(const PlacerConfig& cfg, std::string* error) {
    if (cfg.classes.empty()) return failWith(error, "config: no classes");
    if (cfg.classes.size() > 65535) return failWith(error, "config: too many classes");
    if (!(cfg.rejectStep > 0.0)) return failWith(error, "config: reject_step_m must be > 0");
    if (!(cfg.setbackMargin >= 0.0) || !(cfg.endMargin >= 0.0)) return failWith(error, "config: margins must be >= 0");
    for (const PlacerClass& c : cfg.classes) {
        if (c.variants.empty() || c.variants.size() > 65535) return failWith(error, "config: class " + c.name + " has no variants");
        if (!(c.frontGrowth >= 0.0) || !(c.rearGrowth >= 0.0) || !(c.gap >= 0.0) || !(c.jitter >= 0.0))
            return failWith(error, "config: class " + c.name + ": growths, gap and jitter must be >= 0");
        bool weighs = false;
        for (const PlacerVariant& v : c.variants) {
            if (!(v.width > 0.0) || !(v.depth > 0.0) || !(v.top >= v.height) || !(v.height > 0.0) || !(v.weight >= 0.0) ||
                !std::isfinite(v.top) || !std::isfinite(v.weight))
                return failWith(error, "config: class " + c.name + " variant " + v.name + " has a bad size or weight");
            weighs = weighs || v.weight > 0.0;
        }
        if (!weighs) return failWith(error, "config: class " + c.name + " has no variant with weight > 0");
    }
    return true;
}

} // namespace

std::string_view placeRejectName(PlaceReject reason) {
    switch (reason) {
    case PlaceReject::OffGrid: return "off_grid";
    case PlaceReject::Water: return "water";
    case PlaceReject::Band: return "band";
    case PlaceReject::Shoulder: return "shoulder";
    case PlaceReject::Carriageway: return "carriageway";
    case PlaceReject::Hold: return "hold";
    case PlaceReject::OtherMask: return "other";
    case PlaceReject::Slope: return "slope";
    case PlaceReject::RoofCap: return "roof_cap";
    case PlaceReject::Overlap: return "overlap";
    case PlaceReject::Variety: return "variety";
    }
    return "?";
}

bool PlacerGrid::valid() const {
    const std::size_t n = nx * ny;
    return nx >= 2 && ny >= 2 && cell > 0.0 && std::isfinite(cell) && std::isfinite(x0) && std::isfinite(y0) &&
           height.size() == n && (maxRoof.empty() || maxRoof.size() == n) && (noBuild.empty() || noBuild.size() == n);
}

double PlacerGrid::slopeDegAt(std::size_t i, std::size_t j) const {
    if (nx == 0 || ny == 0 || i >= nx || j >= ny || height.size() != nx * ny) return 0.0;
    const std::size_t i0 = i > 0 ? i - 1 : i, i1 = i + 1 < nx ? i + 1 : i;
    const std::size_t j0 = j > 0 ? j - 1 : j, j1 = j + 1 < ny ? j + 1 : j;
    const double gx = i1 > i0 ? (static_cast<double>(height[j * nx + i1]) - height[j * nx + i0]) /
                                    (static_cast<double>(i1 - i0) * cell)
                              : 0.0;
    const double gy = j1 > j0 ? (static_cast<double>(height[j1 * nx + i]) - height[j0 * nx + i]) /
                                    (static_cast<double>(j1 - j0) * cell)
                              : 0.0;
    return std::atan(std::hypot(gx, gy)) * 180.0 / kPi;
}

std::size_t PlacerConfig::variantCount() const {
    std::size_t n = 0;
    for (const PlacerClass& c : classes) n += c.variants.size();
    return n;
}

std::uint32_t PlacerZoneStats::rejected() const {
    std::uint32_t n = 0;
    for (const std::uint32_t r : rejects) n += r;
    return n;
}

MassingBox placerFootprint(const PlacerConfig& config, const PlacedBuilding& b, bool envelope) {
    MassingBox box;
    box.id = b.id;
    box.kind = "building";
    box.z0 = b.groundZ;
    box.z1 = b.roofZ;
    if (b.cls >= config.classes.size() || b.variant >= config.classes[b.cls].variants.size()) return box;
    const PlacerClass& c = config.classes[b.cls];
    const PlacerVariant& v = c.variants[b.variant];
    if (envelope) {
        const Envelope e = envelopeOf(c, v, b.cx, b.cy, b.rot);
        box.cx = e.cx;
        box.cy = e.cy;
        box.sx = e.depth;
        box.sy = e.width;
    } else {
        box.cx = b.cx;
        box.cy = b.cy;
        box.sx = v.depth;
        box.sy = v.width;
    }
    box.rot = b.rot;
    return box;
}

double placerCoverage(const PlacerZoneStats& stats, const PlacerZone& zone, bool envelope) {
    if (!(zone.buildableM2 > 0.0)) return 0.0;
    return (envelope ? stats.envelopeArea : stats.bodyArea) / zone.buildableM2;
}

bool placeCity(const PlacerConfig& config, const PlacerGrid& grid, const std::vector<PlacerZone>& zones,
               const std::vector<PlacerFrontage>& frontages, PlacerResult& out, std::string* error,
               const PlacerVarietyGuard* guard) {
    out.placed.clear();
    out.zones.clear();
    if (!grid.valid()) return failWith(error, "grid: sizes don't agree, or the cell or origin is bad");
    if (!validConfig(config, error)) return false;
    if (guard && guard->zones.size() != zones.size()) return failWith(error, "variety guard: one zone flag per zone is needed");
    std::unordered_set<std::string_view> zoneIds;
    for (const PlacerZone& z : zones) {
        if (!validId(z.id)) return failWith(error, "zone '" + z.id + "': id must be A-Z, a-z, 0-9, '_', '-' or '.'");
        if (!zoneIds.insert(z.id).second) return failWith(error, "zone " + z.id + " twice");
        if (z.classWeights.size() != config.classes.size())
            return failWith(error, "zone " + z.id + ": class weights don't match the config's classes");
        if (!(z.share >= 0.0 && z.share <= 1.0) || !(z.slopeMaxDeg >= 0.0) || !std::isfinite(z.axisDeg))
            return failWith(error, "zone " + z.id + ": share, slope limit or axis out of range");
    }
    std::vector<std::uint32_t> frontageZone(frontages.size());
    for (std::size_t f = 0; f < frontages.size(); ++f) {
        const PlacerFrontage& fr = frontages[f];
        std::size_t z = 0;
        while (z < zones.size() && zones[z].id != fr.zone) ++z;
        if (z == zones.size()) return failWith(error, "frontage " + fr.id + ": no zone " + fr.zone);
        frontageZone[f] = static_cast<std::uint32_t>(z);
        if (!(std::hypot(fr.x1 - fr.x0, fr.y1 - fr.y0) > 0.0) || !(fr.halfWidth >= 0.0) || !std::isfinite(fr.axisDeg))
            return failWith(error, "frontage " + fr.id + ": no length, or a bad half-width or axis");
    }

    out.zones.assign(zones.size(), PlacerZoneStats{});
    std::vector<std::uint32_t> numbers(zones.size(), 0);
    std::vector<Standing> standing;
    Buckets buckets;
    buckets.init(grid, 32.0);
    double maxGap = 0.0;
    for (const PlacerClass& c : config.classes) maxGap = std::max(maxGap, c.gap);
    std::vector<double> scratch;
    std::vector<std::pair<double, std::uint16_t>> lower;
    std::size_t maxVariants = 0;
    for (const PlacerClass& c : config.classes) maxVariants = std::max(maxVariants, c.variants.size());
    scratch.reserve(maxVariants);
    lower.reserve(maxVariants);
    GuardState gs;
    gs.prepare(config);
    if (guard) gs.init(config, *guard);
    // The variety guard's stand-ins: (key, class, variant), keyed by weight within the zone's mix.
    struct Sub {
        double key;
        std::uint16_t cls, variant;
    };
    std::vector<Sub> subs;
    subs.reserve(config.variantCount());
    std::vector<double> variantWeight(config.classes.size(), 0.0); // per class, its variants' weights summed
    for (std::size_t c = 0; c < config.classes.size(); ++c)
        for (const PlacerVariant& v : config.classes[c].variants) variantWeight[c] += v.weight;
    // A rough upper bound on what a walk can place, so the lists rarely grow.
    double frontageTotal = 0.0;
    for (const PlacerFrontage& fr : frontages) frontageTotal += std::hypot(fr.x1 - fr.x0, fr.y1 - fr.y0);
    double narrowest = kInf;
    for (const PlacerClass& c : config.classes)
        for (const PlacerVariant& v : c.variants) narrowest = std::min(narrowest, v.width);
    const std::size_t guess = static_cast<std::size_t>(std::min(2.0 * frontageTotal / narrowest, 1e6));
    out.placed.reserve(guess);
    standing.reserve(guess);

    for (std::size_t f = 0; f < frontages.size(); ++f) {
        const PlacerFrontage& fr = frontages[f];
        const std::uint32_t zi = frontageZone[f];
        const PlacerZone& zone = zones[zi];
        PlacerZoneStats& stats = out.zones[zi];
        const double length = std::hypot(fr.x1 - fr.x0, fr.y1 - fr.y0);
        const double dx = (fr.x1 - fr.x0) / length, dy = (fr.y1 - fr.y0) / length;
        for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
            const int side = sideIndex == 0 ? 1 : -1;
            if ((side > 0 && !fr.left) || (side < 0 && !fr.right)) continue;
            // The building side's normal (left is +90 degrees from the walk); the front faces back along it.
            const double nx = side > 0 ? -dy : dy, ny = side > 0 ? dx : -dx;
            const double rot = normaliseDeg(std::atan2(-ny, -nx) * 180.0 / kPi + zone.axisDeg + fr.axisDeg);
            Rng rng(config.seed ^ detail::mixSeed(hashText(fr.id) + static_cast<std::uint32_t>(f) * 0x9e3779b9u +
                                                  static_cast<std::uint32_t>(sideIndex) * 0x85ebca6bu));
            const double end = length - config.endMargin;
            double next = config.endMargin;
            bool afterPlaced = false;
            double prevGap = 0.0;
            for (;;) {
                const std::size_t ci = pickWeighted(rng, zone.classWeights.data(), zone.classWeights.size());
                if (ci == static_cast<std::size_t>(-1)) break; // nothing weighs in this zone
                const PlacerClass& pc = config.classes[ci];
                const std::size_t vi = pickVariant(rng, pc, scratch);
                const double lead = rng.uniform() * pc.jitter;
                const bool offered = rng.uniform() < zone.share;
                const double start = next + (afterPlaced ? std::max(prevGap, pc.gap) : 0.0) + lead;
                if (start + pc.variants[vi].width > end + kEps) break;
                if (!offered) {
                    ++stats.thinned;
                    next = start + pc.variants[vi].width;
                    afterPlaced = false;
                    continue;
                }
                ++stats.candidates;
                const auto bodyAt = [&](const PlacerClass& c, const PlacerVariant& v, double& bx, double& by) {
                    const double setback = fr.halfWidth + config.setbackMargin + c.frontGrowth;
                    const double along = start + v.width * 0.5, away = setback + v.depth * 0.5;
                    bx = fr.x0 + dx * along + nx * away;
                    by = fr.y0 + dy * along + ny * away;
                };
                Attempt at;
                double bx = 0.0, by = 0.0;
                bodyAt(pc, pc.variants[vi], bx, by);
                std::size_t usedCls = ci;
                std::size_t used = vi;
                int reason = attempt(config, grid, zone, pc, pc.variants[vi], bx, by, rot, standing, buckets, maxGap, gs, zi,
                                     gs.flat(ci, vi), at);
                bool lowered = false;
                bool substituted = false;
                if (reason == static_cast<int>(PlaceReject::RoofCap)) {
                    // The lower variants, in a seeded order by weight (weight 0 last, tallest first).
                    lower.clear();
                    const double failedTop = pc.variants[vi].top;
                    for (std::size_t k = 0; k < pc.variants.size(); ++k) {
                        if (!(pc.variants[k].top < failedTop - kEps)) continue;
                        const double u = rng.uniform();
                        const double w = pc.variants[k].weight;
                        lower.emplace_back(w > 0.0 ? -std::log(1.0 - u) / w : kInf, static_cast<std::uint16_t>(k));
                    }
                    std::stable_sort(lower.begin(), lower.end(), [&](const auto& a, const auto& b) {
                        if (a.first != b.first) return a.first < b.first;
                        return pc.variants[a.second].top > pc.variants[b.second].top;
                    });
                    for (const auto& entry : lower) {
                        const PlacerVariant& lv = pc.variants[entry.second];
                        if (start + lv.width > end + kEps) continue;
                        double lx = 0.0, ly = 0.0;
                        bodyAt(pc, lv, lx, ly);
                        Attempt la;
                        if (attempt(config, grid, zone, pc, lv, lx, ly, rot, standing, buckets, maxGap, gs, zi,
                                    gs.flat(ci, entry.second), la) != kPlaced)
                            continue;
                        at = la;
                        bx = lx;
                        by = ly;
                        used = entry.second;
                        reason = kPlaced;
                        lowered = true;
                        break;
                    }
                }
                if (reason == static_cast<int>(PlaceReject::Variety)) {
                    // The zone's other variants, every class it weighs, in a seeded order by weight.
                    subs.clear();
                    for (std::size_t c = 0; c < config.classes.size(); ++c) {
                        const double cw = zone.classWeights[c];
                        if (!(cw > 0.0) || !(variantWeight[c] > 0.0)) continue;
                        for (std::size_t k = 0; k < config.classes[c].variants.size(); ++k) {
                            if (c == ci && k == vi) continue;
                            const double w = cw * config.classes[c].variants[k].weight / variantWeight[c];
                            if (!(w > 0.0)) continue;
                            const double u = rng.uniform();
                            subs.push_back({-std::log(1.0 - u) / w, static_cast<std::uint16_t>(c), static_cast<std::uint16_t>(k)});
                        }
                    }
                    std::sort(subs.begin(), subs.end(), [](const Sub& a, const Sub& b) {
                        if (a.key != b.key) return a.key < b.key;
                        return a.cls != b.cls ? a.cls < b.cls : a.variant < b.variant;
                    });
                    for (const Sub& s : subs) {
                        const PlacerClass& sc = config.classes[s.cls];
                        const PlacerVariant& sv = sc.variants[s.variant];
                        if (start + sv.width > end + kEps) continue;
                        double sx = 0.0, sy = 0.0;
                        bodyAt(sc, sv, sx, sy);
                        Attempt sa;
                        if (attempt(config, grid, zone, sc, sv, sx, sy, rot, standing, buckets, maxGap, gs, zi,
                                    gs.flat(s.cls, s.variant), sa) != kPlaced)
                            continue;
                        at = sa;
                        bx = sx;
                        by = sy;
                        usedCls = s.cls;
                        used = s.variant;
                        reason = kPlaced;
                        substituted = true;
                        break;
                    }
                }
                if (reason != kPlaced) {
                    ++stats.rejects[static_cast<std::size_t>(reason)];
                    next = start + config.rejectStep;
                    afterPlaced = false;
                    continue;
                }
                const PlacerClass& uc = config.classes[usedCls];
                const PlacerVariant& pv = uc.variants[used];
                PlacedBuilding b;
                b.id = zone.id + "_" + std::to_string(++numbers[zi]);
                b.zone = zi;
                b.frontage = static_cast<std::uint32_t>(f);
                b.cls = static_cast<std::uint16_t>(usedCls);
                b.variant = static_cast<std::uint16_t>(used);
                b.side = static_cast<std::int8_t>(side);
                b.bench = at.bench;
                b.lowered = lowered;
                b.cx = bx;
                b.cy = by;
                b.rot = rot;
                b.groundZ = at.under.groundMax;
                b.groundMinZ = at.under.groundMin;
                b.roofZ = at.under.groundMax + pv.top;
                b.slopeDeg = at.under.slopeMax;
                out.placed.push_back(std::move(b));
                Standing st;
                st.box = boxOf(at.env);
                st.gap = uc.gap;
                st.radius = 0.5 * std::hypot(at.env.depth, at.env.width);
                buckets.insert(static_cast<std::uint32_t>(standing.size()), st.box.cx, st.box.cy, st.radius);
                standing.push_back(st);
                gs.add(zi, gs.flat(usedCls, used), bx, by, at.under.slopeMax);
                ++stats.placed;
                if (lowered) ++stats.lowered;
                if (substituted) ++stats.substituted;
                if (at.bench) ++stats.benched;
                stats.bodyArea += pv.width * pv.depth;
                stats.envelopeArea += at.env.width * at.env.depth;
                next = start + pv.width;
                prevGap = uc.gap;
                afterPlaced = true;
            }
        }
    }
    return true;
}

// --- The variety check ---------------------------------------------------------------------------------

void checkVariety(const std::vector<PlacedBuilding>& placed, const std::vector<PlacerPoint>& centres,
                  const VarietyParams& params, VarietyResult& out) {
    out = VarietyResult{};
    std::vector<std::size_t> counted;
    counted.reserve(placed.size());
    std::size_t maxCls = 0, maxVar = 0;
    for (std::size_t i = 0; i < placed.size(); ++i) {
        if (!(placed[i].slopeDeg < params.maxSlopeDeg)) continue;
        counted.push_back(i);
        maxCls = std::max<std::size_t>(maxCls, placed[i].cls);
        maxVar = std::max<std::size_t>(maxVar, placed[i].variant);
    }
    out.counted = static_cast<std::uint32_t>(counted.size());
    const std::size_t stride = maxVar + 1;
    std::vector<std::uint32_t> counts((maxCls + 1) * stride, 0);
    out.discs.reserve(centres.size());
    const double r2 = params.radius * params.radius;
    for (const PlacerPoint& c : centres) {
        std::fill(counts.begin(), counts.end(), 0u);
        VarietyDisc d;
        d.centre = c;
        for (const std::size_t i : counted) {
            const PlacedBuilding& b = placed[i];
            const double ddx = b.cx - c.x, ddy = b.cy - c.y;
            if (ddx * ddx + ddy * ddy > r2) continue;
            ++d.buildings;
            std::uint32_t& n = counts[static_cast<std::size_t>(b.cls) * stride + b.variant];
            if (n++ == 0) ++d.classes;
            if (n > d.topCount) {
                d.topCount = n;
                d.topCls = b.cls;
                d.topVariant = b.variant;
            }
        }
        d.judged = d.buildings >= params.minBuildings && d.buildings > 0;
        if (d.judged) {
            ++out.judged;
            const bool shareOk = static_cast<double>(d.topCount) <= params.maxShare * static_cast<double>(d.buildings) + kEps;
            const bool classesOk = d.classes >= params.minClasses;
            if (!shareOk) ++out.failedShare;
            if (!classesOk) ++out.failedClasses;
            d.pass = shareOk && classesOk;
        }
        out.discs.push_back(d);
    }
    std::uint32_t failed = 0;
    for (const VarietyDisc& d : out.discs) failed += d.pass ? 0u : 1u;
    out.pass = out.judged > 0 && failed == 0;
}

void checkLandmarkReach(const std::vector<PlacerPoint>& points, const std::vector<PlacerPoint>& landmarks, double reach,
                        LandmarkReach& out) {
    out = LandmarkReach{};
    out.points = static_cast<std::uint32_t>(points.size());
    for (const PlacerPoint& p : points) {
        double nearest = kInf;
        for (const PlacerPoint& l : landmarks) nearest = std::min(nearest, std::hypot(p.x - l.x, p.y - l.y));
        if (nearest > reach) ++out.uncovered;
        if (nearest > out.worst) {
            out.worst = nearest;
            out.worstAt = p;
        }
    }
    out.pass = !points.empty() && !landmarks.empty() && out.uncovered == 0;
}

void placerFlatPoints(const PlacerGrid& grid, double minX, double minY, double maxX, double maxY, double spacing,
                      double maxSlopeDeg, std::vector<PlacerPoint>& out) {
    out.clear();
    if (!grid.valid() || !(spacing > 0.0) || !(maxX >= minX) || !(maxY >= minY)) return;
    const double gx1 = grid.x0 + static_cast<double>(grid.nx - 1) * grid.cell;
    const double gy1 = grid.y0 + static_cast<double>(grid.ny - 1) * grid.cell;
    const std::size_t kx = static_cast<std::size_t>(std::floor((maxX - minX) / spacing + 1e-9)) + 1;
    const std::size_t ky = static_cast<std::size_t>(std::floor((maxY - minY) / spacing + 1e-9)) + 1;
    out.reserve(kx * ky);
    for (std::size_t m = 0; m < ky; ++m) {
        const double y = minY + static_cast<double>(m) * spacing;
        for (std::size_t k = 0; k < kx; ++k) {
            const double x = minX + static_cast<double>(k) * spacing;
            if (x < grid.x0 || x > gx1 || y < grid.y0 || y > gy1) continue;
            const std::size_t i = static_cast<std::size_t>(std::lround((x - grid.x0) / grid.cell));
            const std::size_t j = static_cast<std::size_t>(std::lround((y - grid.y0) / grid.cell));
            if (!grid.noBuild.empty() && (grid.noBuild[j * grid.nx + i] & static_cast<std::uint8_t>(NoBuild::Water)))
                continue;
            if (!(grid.slopeDegAt(i, j) < maxSlopeDeg)) continue;
            out.push_back({x, y});
        }
    }
}

// --- Reading -----------------------------------------------------------------------------------------------

namespace {

// One CSV record: its fields and the line it starts on.
struct Record {
    std::vector<std::string> fields;
    std::size_t line = 0;
};

bool failLine(std::string* error, std::size_t line, const std::string& what) {
    return failWith(error, "line " + std::to_string(line) + ": " + what);
}

// RFC 4180 CSV: a quoted field may hold commas, doubled quotes and line breaks. Blank lines skipped.
bool splitCsv(std::string_view text, std::vector<Record>& out, std::string* error) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);
    std::size_t i = 0, line = 1;
    const std::size_t n = text.size();
    while (i < n) {
        Record rec;
        rec.line = line;
        std::string field;
        bool any = false;
        for (;;) {
            if (i < n && text[i] == '"') {
                any = true;
                ++i;
                for (;;) {
                    if (i >= n) return failLine(error, rec.line, "a quoted field is never closed");
                    const char c = text[i++];
                    if (c == '"') {
                        if (i < n && text[i] == '"') {
                            field.push_back('"');
                            ++i;
                            continue;
                        }
                        break;
                    }
                    if (c == '\n') ++line;
                    field.push_back(c);
                }
                if (i < n && text[i] != ',' && text[i] != '\n' && text[i] != '\r')
                    return failLine(error, line, "text after a closing quote");
            } else {
                while (i < n && text[i] != ',' && text[i] != '\n' && text[i] != '\r') {
                    field.push_back(text[i++]);
                    any = true;
                }
            }
            if (i < n && text[i] == ',') {
                any = true;
                rec.fields.push_back(std::move(field));
                field.clear();
                ++i;
                continue;
            }
            rec.fields.push_back(std::move(field));
            break;
        }
        if (i < n && text[i] == '\r') ++i;
        if (i < n && text[i] == '\n') {
            ++i;
            ++line;
        }
        if (any) out.push_back(std::move(rec));
    }
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

bool parseNumber(std::string_view s, double& out) {
    s = trim(s);
    if (s.empty()) return false;
    if (s.front() == '+') s.remove_prefix(1);
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && end == s.data() + s.size() && std::isfinite(out);
}

bool parseFlag(std::string_view s, bool& out) {
    s = trim(s);
    if (s == "1" || s == "true" || s == "True" || s == "TRUE") out = true;
    else if (s == "0" || s == "false" || s == "False" || s == "FALSE") out = false;
    else return false;
    return true;
}

// A CSV table with a header naming its columns (any order; '#' rows are comments).
struct Table {
    std::vector<Record> records;
    std::vector<std::string> header;
    std::size_t headerLine = 0;
    std::size_t first = 0; // the first data record

    // The column's index, or npos.
    std::size_t column(std::string_view name) const {
        for (std::size_t c = 0; c < header.size(); ++c)
            if (header[c] == name) return c;
        return static_cast<std::size_t>(-1);
    }
};

bool isComment(const Record& r) {
    const std::string_view f = trim(r.fields.front());
    return !f.empty() && f.front() == '#';
}

bool readTable(std::string_view csv, Table& t, const std::vector<std::string_view>& required, std::string* error) {
    if (!splitCsv(csv, t.records, error)) return false;
    std::size_t h = 0;
    while (h < t.records.size() && isComment(t.records[h])) ++h;
    if (h == t.records.size()) return failLine(error, 1, "no header row");
    for (const std::string& f : t.records[h].fields) t.header.emplace_back(trim(f));
    t.headerLine = t.records[h].line;
    for (std::size_t c = 0; c < t.header.size(); ++c)
        for (std::size_t d = c + 1; d < t.header.size(); ++d)
            if (t.header[c] == t.header[d]) return failLine(error, t.headerLine, "column " + t.header[c] + " twice");
    for (const std::string_view name : required)
        if (t.column(name) == static_cast<std::size_t>(-1))
            return failLine(error, t.headerLine, "no column " + std::string(name));
    t.first = h + 1;
    return true;
}

struct Cells {
    const Table& t;
    const Record& r;
    std::string_view text(std::string_view name) const {
        const std::size_t c = t.column(name);
        return c < r.fields.size() ? trim(r.fields[c]) : std::string_view{};
    }
    bool has(std::string_view name) const { return t.column(name) != static_cast<std::size_t>(-1) && !text(name).empty(); }
    bool number(std::string_view name, double& v, std::string* error) const {
        if (parseNumber(text(name), v)) return true;
        return failLine(error, r.line, std::string(name) + " is not a number");
    }
};

// Each data row, with its field count checked against the header.
template <typename Fn>
bool eachRow(const Table& t, std::string* error, Fn&& fn) {
    for (std::size_t k = t.first; k < t.records.size(); ++k) {
        const Record& r = t.records[k];
        if (isComment(r)) continue;
        if (r.fields.size() != t.header.size())
            return failLine(error, r.line,
                            std::to_string(r.fields.size()) + " fields where the header has " + std::to_string(t.header.size()));
        if (!fn(Cells{t, r})) return false;
    }
    return true;
}

// JSON helpers: a member must be a finite number (or, when optional and absent, keeps its default).
bool jsonNumber(JsonValue obj, const char* key, double& out, bool optional, const std::string& where, std::string* error) {
    const JsonValue v = obj[key];
    if (v.isAbsent() && optional) return true;
    if (!v.isNumber() || !std::isfinite(v.number())) return failWith(error, where + key + ": a number is needed");
    out = v.number();
    return true;
}

bool onlyKeys(JsonValue obj, const std::vector<std::string_view>& keys, const std::string& where, std::string* error) {
    for (std::uint32_t i = 0; i < obj.size(); ++i) {
        const std::string_view k = obj.key(i);
        if (std::find(keys.begin(), keys.end(), k) == keys.end())
            return failWith(error, where + "unknown member \"" + std::string(k) + "\"");
    }
    return true;
}

bool readClass(JsonValue jc, std::size_t index, PlacerClass& c, std::string* error) {
    const std::string where = "classes[" + std::to_string(index) + "].";
    if (!jc.isObject()) return failWith(error, "classes[" + std::to_string(index) + "]: an object is needed");
    if (!onlyKeys(jc, {"name", "front_growth_m", "rear_growth_m", "gap_m", "jitter_m", "bench", "storey_m", "top_extra_m", "variants"},
                  where, error))
        return false;
    c.name = std::string(jc["name"].string());
    if (!validName(c.name)) return failWith(error, where + "name: lower-case a-z, 0-9 and '_' only");
    double storey = 3.0, topExtra = 0.0;
    if (!jsonNumber(jc, "front_growth_m", c.frontGrowth, false, where, error) ||
        !jsonNumber(jc, "rear_growth_m", c.rearGrowth, false, where, error) ||
        !jsonNumber(jc, "gap_m", c.gap, false, where, error) || !jsonNumber(jc, "jitter_m", c.jitter, true, where, error) ||
        !jsonNumber(jc, "storey_m", storey, false, where, error) ||
        !jsonNumber(jc, "top_extra_m", topExtra, true, where, error))
        return false;
    if (c.frontGrowth < 0.0 || c.rearGrowth < 0.0 || c.gap < 0.0 || c.jitter < 0.0 || storey <= 0.0 || topExtra < 0.0)
        return failWith(error, where + " growths, gap, jitter and top_extra_m must be >= 0, storey_m > 0");
    const JsonValue bench = jc["bench"];
    if (!bench.isAbsent() && !bench.isBool()) return failWith(error, where + "bench: true or false");
    c.benchOk = bench.boolean(false);
    const JsonValue vs = jc["variants"];
    if (!vs.isArray() || vs.size() == 0) return failWith(error, where + "variants: a non-empty array is needed");
    for (std::uint32_t k = 0; k < vs.size(); ++k) {
        const JsonValue jv = vs[k];
        const std::string vw = where + "variants[" + std::to_string(k) + "].";
        if (!jv.isObject()) return failWith(error, vw + ": an object is needed");
        if (!onlyKeys(jv, {"name", "width_m", "depth_m", "storeys", "rise_m", "top_extra_m", "weight"}, vw, error)) return false;
        PlacerVariant base;
        base.name = std::string(jv["name"].string());
        if (!validName(base.name)) return failWith(error, vw + "name: lower-case a-z, 0-9 and '_' only");
        double rise = 0.0, extra = topExtra;
        if (!jsonNumber(jv, "width_m", base.width, false, vw, error) || !jsonNumber(jv, "depth_m", base.depth, false, vw, error) ||
            !jsonNumber(jv, "rise_m", rise, true, vw, error) || !jsonNumber(jv, "top_extra_m", extra, true, vw, error) ||
            !jsonNumber(jv, "weight", base.weight, true, vw, error))
            return false;
        if (!(base.width > 0.0) || !(base.depth > 0.0) || rise < 0.0 || extra < 0.0 || base.weight < 0.0)
            return failWith(error, vw + " width and depth must be > 0; rise, top_extra and weight >= 0");
        const JsonValue st = jv["storeys"];
        std::int64_t lo = 0, hi = 0;
        bool range = false;
        if (st.isInteger()) {
            lo = hi = st.integer();
        } else if (st.isArray() && st.size() == 2 && st[0].isInteger() && st[1].isInteger()) {
            lo = st[0].integer();
            hi = st[1].integer();
            range = true;
        } else {
            return failWith(error, vw + "storeys: a count or [min, max] is needed");
        }
        if (lo < 1 || hi < lo || hi > 200) return failWith(error, vw + "storeys: 1 or more, min <= max");
        for (std::int64_t n = lo; n <= hi; ++n) {
            PlacerVariant v = base;
            if (range) v.name = base.name + "_s" + std::to_string(n);
            v.storeys = static_cast<std::uint16_t>(n);
            v.height = static_cast<double>(n) * storey + rise;
            v.top = v.height + extra;
            for (const PlacerVariant& other : c.variants)
                if (other.name == v.name) return failWith(error, vw + "name: " + v.name + " twice in its class");
            c.variants.push_back(std::move(v));
        }
    }
    return true;
}

} // namespace

bool importPlacerConfig(std::string_view json, PlacerConfig& out, std::string* error) {
    out = PlacerConfig{};
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(json, &parseError)) return failWith(error, parseError);
    const JsonValue root = doc.root();
    const auto bad = [&](const std::string& what) {
        out = PlacerConfig{};
        return failWith(error, what);
    };
    std::string why;
    if (!root.isObject()) return bad("the top level must be an object");
    if (!onlyKeys(root, {"version", "seed", "setback_margin_m", "end_margin_m", "reject_step_m", "bench_max_slope_deg", "variety", "classes"},
                  "", &why))
        return bad(why);
    if (!root["version"].isInteger() || root["version"].integer() != 1) return bad("version: 1 is needed");
    const JsonValue seed = root["seed"];
    if (!seed.isInteger() || seed.integer() < 0 || seed.integer() > 0xffffffffll) return bad("seed: an integer 0..4294967295 is needed");
    out.seed = static_cast<std::uint32_t>(seed.integer());
    if (!jsonNumber(root, "setback_margin_m", out.setbackMargin, false, "", &why) ||
        !jsonNumber(root, "end_margin_m", out.endMargin, false, "", &why) ||
        !jsonNumber(root, "reject_step_m", out.rejectStep, false, "", &why) ||
        !jsonNumber(root, "bench_max_slope_deg", out.benchMaxSlopeDeg, false, "", &why))
        return bad(why);
    if (out.setbackMargin < 0.0 || out.endMargin < 0.0 || !(out.rejectStep > 0.0) || !(out.benchMaxSlopeDeg >= 0.0 && out.benchMaxSlopeDeg < 90.0))
        return bad("margins must be >= 0, reject_step_m > 0, bench_max_slope_deg 0..90");
    const JsonValue jv = root["variety"];
    if (!jv.isObject()) return bad("variety: an object is needed");
    if (!onlyKeys(jv, {"radius_m", "spacing_m", "max_share", "min_classes", "min_buildings", "max_slope_deg", "landmark_m"},
                  "variety.", &why))
        return bad(why);
    VarietyParams& vp = out.variety;
    double minClasses = 0.0, minBuildings = 0.0;
    if (!jsonNumber(jv, "radius_m", vp.radius, false, "variety.", &why) ||
        !jsonNumber(jv, "spacing_m", vp.spacing, false, "variety.", &why) ||
        !jsonNumber(jv, "max_share", vp.maxShare, false, "variety.", &why) ||
        !jsonNumber(jv, "min_classes", minClasses, false, "variety.", &why) ||
        !jsonNumber(jv, "min_buildings", minBuildings, false, "variety.", &why) ||
        !jsonNumber(jv, "max_slope_deg", vp.maxSlopeDeg, false, "variety.", &why) ||
        !jsonNumber(jv, "landmark_m", vp.landmarkReach, false, "variety.", &why))
        return bad(why);
    if (!jv["min_classes"].isInteger() || !jv["min_buildings"].isInteger() || minClasses < 0.0 || minBuildings < 0.0 ||
        minClasses > 1e6 || minBuildings > 1e9)
        return bad("variety: min_classes and min_buildings must be whole numbers >= 0");
    vp.minClasses = static_cast<std::uint32_t>(minClasses);
    vp.minBuildings = static_cast<std::uint32_t>(minBuildings);
    if (!(vp.radius > 0.0) || !(vp.spacing > 0.0) || !(vp.maxShare > 0.0 && vp.maxShare <= 1.0) || !(vp.landmarkReach > 0.0))
        return bad("variety: radius, spacing and landmark reach must be > 0, max_share 0..1");
    const JsonValue jc = root["classes"];
    if (!jc.isArray() || jc.size() == 0) return bad("classes: a non-empty array is needed");
    for (std::uint32_t i = 0; i < jc.size(); ++i) {
        PlacerClass c;
        if (!readClass(jc[i], i, c, &why)) return bad(why);
        for (const PlacerClass& other : out.classes)
            if (other.name == c.name) return bad("classes[" + std::to_string(i) + "].name: " + c.name + " twice");
        out.classes.push_back(std::move(c));
    }
    if (!validConfig(out, &why)) return bad(why);
    return true;
}

bool importPlacerGridMeta(std::string_view json, PlacerGrid& out, std::string* error) {
    out = PlacerGrid{};
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(json, &parseError)) return failWith(error, parseError);
    const JsonValue root = doc.root();
    const JsonValue sx = root["samples_x"], sy = root["samples_y"], sp = root["spacing_m"], org = root["origin_of_sample_0_0_m"];
    if (!sx.isInteger() || !sy.isInteger() || sx.integer() < 2 || sy.integer() < 2 || sx.integer() > 100000 ||
        sy.integer() > 100000)
        return failWith(error, "samples_x and samples_y: whole numbers 2..100000 are needed");
    if (!sp.isNumber() || !(sp.number() > 0.0) || !std::isfinite(sp.number())) return failWith(error, "spacing_m: a number > 0 is needed");
    if (!org.isArray() || org.size() != 2 || !org[0].isNumber() || !org[1].isNumber())
        return failWith(error, "origin_of_sample_0_0_m: [x, y] is needed");
    out.nx = static_cast<std::size_t>(sx.integer());
    out.ny = static_cast<std::size_t>(sy.integer());
    out.cell = sp.number();
    out.x0 = org[0].number();
    out.y0 = org[1].number();
    return true;
}

namespace {

bool rasterSize(const PlacerGrid& grid, std::size_t size, std::size_t bytesPerSample, const char* what, std::string* error) {
    if (grid.nx == 0 || grid.ny == 0) return failWith(error, std::string(what) + ": the grid has no size");
    const std::size_t want = grid.nx * grid.ny * bytesPerSample;
    if (size != want)
        return failWith(error, std::string(what) + ": " + std::to_string(size) + " bytes where the grid needs " + std::to_string(want));
    return true;
}

std::uint16_t u16At(const std::uint8_t* bytes, std::size_t k) {
    return static_cast<std::uint16_t>(bytes[2 * k] | (static_cast<std::uint16_t>(bytes[2 * k + 1]) << 8));
}

} // namespace

bool placerDecodeHeights(const std::uint8_t* bytes, std::size_t size, PlacerGrid& grid, std::string* error) {
    if (!rasterSize(grid, size, 2, "height", error)) return false;
    const std::size_t n = grid.nx * grid.ny;
    grid.height.resize(n);
    for (std::size_t k = 0; k < n; ++k) grid.height[k] = static_cast<float>(massingHeightFromValue(u16At(bytes, k)));
    return true;
}

bool placerDecodeMaxRoof(const std::uint8_t* bytes, std::size_t size, PlacerGrid& grid, std::string* error) {
    if (!rasterSize(grid, size, 2, "max roof", error)) return false;
    const std::size_t n = grid.nx * grid.ny;
    grid.maxRoof.resize(n);
    for (std::size_t k = 0; k < n; ++k) {
        const std::uint16_t v = u16At(bytes, k);
        grid.maxRoof[k] = v == 65535 ? std::numeric_limits<float>::infinity() : static_cast<float>(massingHeightFromValue(v));
    }
    return true;
}

bool placerDecodeNoBuild(const std::uint8_t* bytes, std::size_t size, PlacerGrid& grid, std::string* error) {
    if (!rasterSize(grid, size, 1, "no-build", error)) return false;
    grid.noBuild.assign(bytes, bytes + size);
    return true;
}

bool importPlacerZones(std::string_view csv, const PlacerConfig& config, std::vector<PlacerZone>& out, std::string* error) {
    out.clear();
    Table t;
    if (!readTable(csv, t, {"id", "share", "slope_max_deg", "bench", "buildable_m2", "weights"}, error)) return false;
    const bool ok = eachRow(t, error, [&](const Cells& c) {
        PlacerZone z;
        z.id = std::string(c.text("id"));
        if (!validId(z.id)) return failLine(error, c.r.line, "id must be A-Z, a-z, 0-9, '_', '-' or '.'");
        for (const PlacerZone& other : out)
            if (other.id == z.id) return failLine(error, c.r.line, "zone " + z.id + " twice");
        if (!c.number("share", z.share, error) || !c.number("slope_max_deg", z.slopeMaxDeg, error) ||
            !c.number("buildable_m2", z.buildableM2, error))
            return false;
        if (c.has("axis_deg") && !c.number("axis_deg", z.axisDeg, error)) return false;
        if (c.has("target_per_km2") && !c.number("target_per_km2", z.targetPerKm2, error)) return false;
        if (z.share < 0.0 || z.share > 1.0) return failLine(error, c.r.line, "share must be 0..1");
        if (z.slopeMaxDeg < 0.0 || z.slopeMaxDeg >= 90.0) return failLine(error, c.r.line, "slope_max_deg must be 0..90");
        if (z.buildableM2 < 0.0 || z.targetPerKm2 < 0.0) return failLine(error, c.r.line, "buildable_m2 and target_per_km2 must be >= 0");
        if (!parseFlag(c.text("bench"), z.bench)) return failLine(error, c.r.line, "bench must be 0, 1, true or false");
        z.classWeights.assign(config.classes.size(), 0.0);
        std::vector<bool> seen(config.classes.size(), false);
        std::string_view rest = c.text("weights");
        while (!rest.empty()) {
            const std::size_t semi = rest.find(';');
            const std::string_view item = trim(rest.substr(0, semi));
            rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
            if (item.empty()) continue;
            const std::size_t colon = item.find(':');
            if (colon == std::string_view::npos) return failLine(error, c.r.line, "weights: each item is class:weight");
            const std::string_view name = trim(item.substr(0, colon));
            double w = 0.0;
            if (!parseNumber(item.substr(colon + 1), w) || w < 0.0)
                return failLine(error, c.r.line, "weights: " + std::string(name) + "'s weight must be a number >= 0");
            std::size_t k = 0;
            while (k < config.classes.size() && config.classes[k].name != name) ++k;
            if (k == config.classes.size()) return failLine(error, c.r.line, "weights: no class " + std::string(name));
            if (seen[k]) return failLine(error, c.r.line, "weights: " + std::string(name) + " twice");
            seen[k] = true;
            z.classWeights[k] = w;
        }
        out.push_back(std::move(z));
        return true;
    });
    if (!ok) out.clear();
    return ok;
}

bool importPlacerFrontages(std::string_view csv, std::vector<PlacerFrontage>& out, std::string* error) {
    out.clear();
    Table t;
    if (!readTable(csv, t, {"id", "zone", "x0", "y0", "x1", "y1", "half_width", "sides"}, error)) return false;
    const bool ok = eachRow(t, error, [&](const Cells& c) {
        PlacerFrontage f;
        f.id = std::string(c.text("id"));
        f.zone = std::string(c.text("zone"));
        if (!validId(f.id) || !validId(f.zone)) return failLine(error, c.r.line, "id and zone must be A-Z, a-z, 0-9, '_', '-' or '.'");
        if (!c.number("x0", f.x0, error) || !c.number("y0", f.y0, error) || !c.number("x1", f.x1, error) ||
            !c.number("y1", f.y1, error) || !c.number("half_width", f.halfWidth, error))
            return false;
        if (c.has("axis_deg") && !c.number("axis_deg", f.axisDeg, error)) return false;
        if (f.halfWidth < 0.0) return failLine(error, c.r.line, "half_width must be >= 0");
        if (!(std::hypot(f.x1 - f.x0, f.y1 - f.y0) > 0.0)) return failLine(error, c.r.line, "the segment has no length");
        const std::string_view sides = c.text("sides");
        if (sides == "left") f.left = true;
        else if (sides == "right") f.right = true;
        else if (sides == "both") f.left = f.right = true;
        else return failLine(error, c.r.line, "sides must be left, right or both");
        out.push_back(std::move(f));
        return true;
    });
    if (!ok) out.clear();
    return ok;
}

bool importPlacerPoints(std::string_view csv, std::vector<PlacerPoint>& out, std::string* error) {
    out.clear();
    Table t;
    if (!readTable(csv, t, {"id", "x", "y"}, error)) return false;
    std::unordered_set<std::string> ids;
    const bool ok = eachRow(t, error, [&](const Cells& c) {
        const std::string id(c.text("id"));
        if (!validId(id)) return failLine(error, c.r.line, "id must be A-Z, a-z, 0-9, '_', '-' or '.'");
        if (!ids.insert(id).second) return failLine(error, c.r.line, "id " + id + " twice");
        PlacerPoint p;
        if (!c.number("x", p.x, error) || !c.number("y", p.y, error)) return false;
        out.push_back(p);
        return true;
    });
    if (!ok) out.clear();
    return ok;
}

// --- Writing -----------------------------------------------------------------------------------------------

std::string placerCsv(const PlacerConfig& config, const std::vector<PlacerZone>& zones,
                      const std::vector<PlacerFrontage>& frontages, const PlacerResult& result) {
    std::string s;
    s.reserve(160 * (result.placed.size() + 1));
    s += "id,zone,class,variant,frontage,frontage_row,side,cx,cy,rot,width,depth,front_growth,rear_growth,storeys,height,top,"
         "ground_z,ground_min_z,roof_z,slope_deg,bench,lowered\n";
    char buf[512];
    for (const PlacedBuilding& b : result.placed) {
        if (b.cls >= config.classes.size() || b.variant >= config.classes[b.cls].variants.size() || b.zone >= zones.size() ||
            b.frontage >= frontages.size())
            continue;
        const PlacerClass& c = config.classes[b.cls];
        const PlacerVariant& v = c.variants[b.variant];
        std::snprintf(buf, sizeof buf,
                      "%s,%s,%s,%s,%s,%u,%s,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,%d,%d\n",
                      b.id.c_str(), zones[b.zone].id.c_str(), c.name.c_str(), v.name.c_str(), frontages[b.frontage].id.c_str(),
                      static_cast<unsigned>(b.frontage), b.side > 0 ? "left" : "right", b.cx, b.cy, b.rot, v.width, v.depth,
                      c.frontGrowth, c.rearGrowth, static_cast<unsigned>(v.storeys), v.height, v.top, b.groundZ, b.groundMinZ,
                      b.roofZ, b.slopeDeg, b.bench ? 1 : 0, b.lowered ? 1 : 0);
        s += buf;
    }
    return s;
}

std::string placerZoneLines(const std::vector<PlacerZone>& zones, const PlacerResult& result) {
    std::string s;
    char buf[256];
    for (std::size_t z = 0; z < zones.size() && z < result.zones.size(); ++z) {
        const PlacerZoneStats& st = result.zones[z];
        std::snprintf(buf, sizeof buf,
                      "zone id=%s candidates=%u placed=%u thinned=%u lowered=%u substituted=%u benched=%u rejected=%u",
                      zones[z].id.c_str(), st.candidates, st.placed, st.thinned, st.lowered, st.substituted, st.benched,
                      st.rejected());
        s += buf;
        for (std::size_t r = 0; r < kPlaceRejectCount; ++r) {
            s += ' ';
            s += placeRejectName(static_cast<PlaceReject>(r));
            s += '=';
            s += std::to_string(st.rejects[r]);
        }
        s += '\n';
        const double km2 = zones[z].buildableM2 / 1e6;
        std::snprintf(buf, sizeof buf,
                      "coverage id=%s buildable_m2=%.1f body_m2=%.1f envelope_m2=%.1f body=%.4f envelope=%.4f per_km2=%.1f "
                      "target_per_km2=%.1f\n",
                      zones[z].id.c_str(), zones[z].buildableM2, st.bodyArea, st.envelopeArea,
                      placerCoverage(st, zones[z], false), placerCoverage(st, zones[z], true),
                      km2 > 0.0 ? static_cast<double>(st.placed) / km2 : 0.0, zones[z].targetPerKm2);
        s += buf;
    }
    return s;
}

std::string placerVarietyLines(const PlacerConfig& config, const VarietyResult& variety) {
    std::string s;
    char buf[320];
    const VarietyParams& p = config.variety;
    std::snprintf(buf, sizeof buf,
                  "variety discs=%u judged=%u counted=%u failed_share=%u failed_classes=%u radius_m=%s max_share=%s "
                  "min_classes=%u min_buildings=%u max_slope_deg=%s result=%s\n",
                  static_cast<unsigned>(variety.discs.size()), variety.judged, variety.counted, variety.failedShare,
                  variety.failedClasses, fmt("%.1f", p.radius).c_str(), fmt("%.3f", p.maxShare).c_str(), p.minClasses,
                  p.minBuildings, fmt("%.2f", p.maxSlopeDeg).c_str(),
                  variety.pass ? "PASS" : (variety.judged == 0 ? "EMPTY" : "FAIL"));
    s += buf;
    for (const VarietyDisc& d : variety.discs) {
        std::string top = "-";
        if (d.topCount > 0 && d.topCls < config.classes.size() && d.topVariant < config.classes[d.topCls].variants.size())
            top = config.classes[d.topCls].name + "/" + config.classes[d.topCls].variants[d.topVariant].name;
        std::snprintf(buf, sizeof buf, "disc x=%.1f y=%.1f buildings=%u classes=%u top=%s top_count=%u top_share=%.3f judged=%d result=%s\n",
                      d.centre.x, d.centre.y, d.buildings, d.classes, top.c_str(), d.topCount,
                      d.buildings ? static_cast<double>(d.topCount) / static_cast<double>(d.buildings) : 0.0, d.judged ? 1 : 0,
                      d.pass ? "PASS" : "FAIL");
        s += buf;
    }
    return s;
}

} // namespace ftcore
