#include "ftcore/BayCount.h"

#include "ftcore/WindowFacade.h"
#include "ftcore/WindowInterior.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace ftcore {

namespace {

constexpr double kPiB = 3.14159265358979323846;
constexpr double kTinyB = 1e-9;
constexpr double kCellB = 32.0;  // m: the street grid's cell side
constexpr std::size_t kNoStreet = static_cast<std::size_t>(-1);

double nearestOnSegment(double x, double y, const MassingVec& a, const MassingVec& b, double& px, double& py) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len2 = dx * dx + dy * dy;
    double t = 0.0;
    if (len2 > kTinyB) t = std::clamp(((x - a.x) * dx + (y - a.y) * dy) / len2, 0.0, 1.0);
    px = a.x + t * dx;
    py = a.y + t * dy;
    return std::hypot(x - px, y - py);
}

std::int64_t cellKey(std::int64_t ix, std::int64_t iy) { return (ix + 524288) * 1048576 + (iy + 524288); }
std::int64_t cellOf(double v, double size) { return static_cast<std::int64_t>(std::floor(v / size)); }

// The street segments gridded, to find the street a face fronts as massingFrontage's kindAt does.
class StreetIndex {
public:
    explicit StreetIndex(const std::vector<MassingStreet>& streets) : streets_(streets) {
        for (std::size_t j = 0; j < streets.size(); ++j) {
            const MassingStreet& s = streets[j];
            maxHalf_ = std::max(maxHalf_, 0.5 * s.width);
            const std::size_t pts = s.points.size();
            for (std::size_t k = 0; k < pts; ++k) {
                if (pts > 1 && k + 1 == pts) break;
                Segment g;
                g.street = j;
                g.a = s.points[k];
                g.b = pts > 1 ? s.points[k + 1] : s.points[k];
                const auto at = static_cast<std::uint32_t>(segs_.size());
                segs_.push_back(g);
                const std::int64_t i0 = cellOf(std::min(g.a.x, g.b.x), kCellB), i1 = cellOf(std::max(g.a.x, g.b.x), kCellB);
                const std::int64_t j0 = cellOf(std::min(g.a.y, g.b.y), kCellB), j1 = cellOf(std::max(g.a.y, g.b.y), kCellB);
                for (std::int64_t jy = j0; jy <= j1; ++jy)
                    for (std::int64_t ix = i0; ix <= i1; ++ix) cells_[cellKey(ix, jy)].push_back(at);
            }
        }
    }

    // The street (index) a face fronts at (x, y), 0.5 m out of it along (nx, ny); kNoStreet for none.
    std::size_t at(double x, double y, double nx, double ny) const {
        const double pad = maxHalf_ + kMassingFrontReach;
        double best = 1e300;
        std::size_t bestStreet = kNoStreet;
        const std::int64_t i0 = cellOf(x - pad, kCellB), i1 = cellOf(x + pad, kCellB);
        const std::int64_t j0 = cellOf(y - pad, kCellB), j1 = cellOf(y + pad, kCellB);
        for (std::int64_t jy = j0; jy <= j1; ++jy)
            for (std::int64_t ix = i0; ix <= i1; ++ix) {
                const auto it = cells_.find(cellKey(ix, jy));
                if (it == cells_.end()) continue;
                for (const std::uint32_t q : it->second) {
                    const Segment& g = segs_[q];
                    double px = 0.0, py = 0.0;
                    const double edge = nearestOnSegment(x, y, g.a, g.b, px, py) - 0.5 * streets_[g.street].width;
                    if (edge > kMassingFrontReach) continue;
                    if ((px - x) * nx + (py - y) * ny <= 0.0) continue;
                    if (edge < best || (edge == best && g.street < bestStreet)) {
                        best = edge;
                        bestStreet = g.street;
                    }
                }
            }
        return bestStreet;
    }

private:
    struct Segment {
        std::size_t street = 0;
        MassingVec a, b;
    };
    const std::vector<MassingStreet>& streets_;
    std::vector<Segment> segs_;
    std::unordered_map<std::int64_t, std::vector<std::uint32_t>> cells_;
    double maxHalf_ = 0.0;
};

double polylineLength(const MassingStreet& s) {
    double len = 0.0;
    for (std::size_t k = 0; k + 1 < s.points.size(); ++k)
        len += std::hypot(s.points[k + 1].x - s.points[k].x, s.points[k + 1].y - s.points[k].y);
    return len;
}

struct BayPoint {
    double x = 0.0, y = 0.0;
};

void addTotals(BayTotals& t, const BayStreet& s) {
    ++t.streets;
    t.withBays += s.bays > 0 ? 1 : 0;
    t.length += s.length;
    t.faces += s.faces;
    t.bays += s.bays;
    t.litBays += s.litBays;
    t.lights += s.lights;
}

}  // namespace

bool bayCount(const std::vector<MassingBox>& boxes, const std::vector<bool>& replaced,
              const std::vector<MassingStreet>& streets, const std::vector<MassingStreet>& frontStreets,
              const std::vector<MassingFrontRule>& rules, const MassingGround& ground, const BayCountConfig& config,
              BayCountResult& out, std::string* error) {
    out = BayCountResult{};
    out.reach = config.reach;
    out.boxes = boxes.size();
    if (rules.empty()) {
        if (error) *error = "no frontage rules: without them no face fronts a street";
        return false;
    }
    if (!(config.step > 0.0)) {
        if (error) *error = "the sweep step must be > 0";
        return false;
    }

    // The result rows: the table's, then any front street it lacks.
    std::unordered_map<std::string, std::size_t> rowOf;
    std::vector<const MassingStreet*> source;
    for (const MassingStreet& s : streets) {
        rowOf.emplace(s.id, out.streets.size());
        source.push_back(&s);
        BayStreet row;
        row.id = s.id;
        row.kind = s.kind;
        row.width = s.width;
        row.pavement = s.pavement;
        row.length = polylineLength(s);
        out.streets.push_back(std::move(row));
    }
    std::vector<std::size_t> frontRow(frontStreets.size(), kNoStreet);
    for (std::size_t j = 0; j < frontStreets.size(); ++j) {
        const MassingStreet& s = frontStreets[j];
        const auto it = rowOf.find(s.id);
        if (it != rowOf.end()) {
            frontRow[j] = it->second;
            continue;
        }
        rowOf.emplace(s.id, out.streets.size());
        frontRow[j] = out.streets.size();
        source.push_back(&s);
        BayStreet row;
        row.id = s.id;
        row.kind = s.kind;
        row.width = s.width;
        row.pavement = s.pavement;
        row.length = polylineLength(s);
        out.streets.push_back(std::move(row));
    }

    // The hosts and the fronting bits, as the build's frontage pass.
    std::vector<bool> visible(boxes.size(), false), hosts(boxes.size(), false);
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        const bool kit = i < replaced.size() && replaced[i];
        visible[i] = massingVisible(massingRole(boxes[i]));
        hosts[i] = visible[i] && !kit && !boxes[i].sloped && massingLookIsBuilding(massingLookSurface(boxes[i]));
        out.hosts += hosts[i] ? 1 : 0;
    }
    std::vector<MassingProp> pieces;
    std::vector<MassingFrontCounts> counts;
    MassingFrontTotals totals;
    std::vector<std::uint8_t> fronting;
    massingFrontage(rules, frontStreets, ground, boxes, visible, hosts, 0.5, nullptr, pieces, counts, totals, nullptr,
                    &fronting);

    const StreetIndex index(frontStreets);
    const WindowFacadeSpec spec;
    std::vector<MassingShopLight> lightScratch;
    std::vector<BayPoint> bayAt;

    for (std::size_t i = 0; i < boxes.size(); ++i) {
        const MassingBox& box = boxes[i];
        if (!hosts[i] || box.kind == "crane" || box.kind == "boat") continue;
        if (fronting.size() < (i + 1) * 4) break;
        ++out.windowed;
        const MassingWindowData data = massingWindowData(box, ground, &fronting[i * 4]);
        if (data.streetBits == 0) continue;
        const WindowType type = massingFacadeData(box, data).type;
        const WindowGridSpec grid = windowGridFor(spec, type);
        const float ww = windowWidthFor(spec, type, data.seed, spec.base.windowWidth);
        const double height = box.z1 - box.z0;
        const double a = box.rot * kPiB / 180.0;
        const double ux = std::cos(a), uy = std::sin(a);
        for (int f = 0; f < 4; ++f) {
            if (((data.streetBits >> f) & 1u) == 0u) continue;
            ++out.streetFaces;
            const double nx = f == 0 ? ux : (f == 1 ? -uy : (f == 2 ? -ux : uy));
            const double ny = f == 0 ? uy : (f == 1 ? ux : (f == 2 ? -uy : -ux));
            const bool alongY = f == 0 || f == 2;  // the face runs along the box's w axis
            const double depth = alongY ? box.sx : box.sy;
            const double length = alongY ? box.sy : box.sx;
            const double mx = box.cx + nx * 0.5 * depth, my = box.cy + ny * 0.5 * depth;
            const std::size_t front = index.at(mx + nx * 0.5, my + ny * 0.5, nx, ny);
            const std::size_t row = front == kNoStreet ? kNoStreet : frontRow[front];
            if (row == kNoStreet) {
                ++out.unattributed;
                continue;
            }
            const WindowFaceGrid g =
                windowFaceGrid(grid, static_cast<float>(length), static_cast<float>(height - data.base[f]), ww);
            const std::size_t bays = g.storeys > 0u ? g.bays : 0u;
            if (bays == 0) {
                ++out.emptyFaces;
                continue;
            }
            BayStreet& s = out.streets[row];
            ++s.faces;
            s.bays += bays;
            for (std::uint32_t c = 0; c < bays; ++c) s.litBays += windowShopLit(data.seed, c, config.shopLit) ? 1 : 0;
            // The night spill points of this face alone.
            MassingWindowData one = data;
            one.streetBits = static_cast<std::uint8_t>(1u << f);
            lightScratch.clear();
            s.lights += massingShopLights(box, one, config.lightSpacing, lightScratch);
            // The bays' centres, along the face from its start (the bays are symmetric about its middle).
            const double tx = -ny, ty = nx;
            for (std::uint32_t c = 0; c < bays; ++c) {
                const double along = grid.edge + (static_cast<double>(c) + 0.5) * g.bayWidth - 0.5 * length;
                bayAt.push_back({mx + tx * along, my + ty * along});
            }
        }
    }

    for (const BayStreet& s : out.streets) {
        addTotals(out.all, s);
        if (s.pavement > 0.0) addTotals(out.walked, s);
    }

    // The sweep: bays within reach of a point on a walked street's centreline.
    std::unordered_map<std::int64_t, std::vector<std::uint32_t>> cells;
    const double cell = std::max(config.reach, 1.0);
    for (std::size_t b = 0; b < bayAt.size(); ++b)
        cells[cellKey(cellOf(bayAt[b].x, cell), cellOf(bayAt[b].y, cell))].push_back(static_cast<std::uint32_t>(b));
    const double r2 = config.reach * config.reach;
    const auto bound = [&](double x, double y) {
        std::size_t n = 0;
        const std::int64_t cx = cellOf(x, cell), cy = cellOf(y, cell);
        for (std::int64_t jy = cy - 1; jy <= cy + 1; ++jy)
            for (std::int64_t ix = cx - 1; ix <= cx + 1; ++ix) {
                const auto it = cells.find(cellKey(ix, jy));
                if (it == cells.end()) continue;
                for (const std::uint32_t b : it->second) {
                    const double dx = bayAt[b].x - x, dy = bayAt[b].y - y;
                    n += dx * dx + dy * dy <= r2 ? 1 : 0;
                }
            }
        return n;
    };
    const auto sample = [&](const BayStreet& s, double x, double y) {
        const std::size_t n = bound(x, y);
        if (!out.worst.found || n > out.worst.bays) {
            out.worst.found = true;
            out.worst.street = s.id;
            out.worst.x = x;
            out.worst.y = y;
            out.worst.bays = n;
        }
    };
    for (std::size_t r = 0; r < out.streets.size(); ++r) {
        const BayStreet& s = out.streets[r];
        if (!(s.pavement > 0.0)) continue;
        const std::vector<MassingVec>& pts = source[r]->points;
        if (pts.empty()) continue;
        if (pts.size() == 1) {
            sample(s, pts[0].x, pts[0].y);
            continue;
        }
        double carry = 0.0;  // metres since the last sample
        sample(s, pts[0].x, pts[0].y);
        for (std::size_t k = 0; k + 1 < pts.size(); ++k) {
            const double len = std::hypot(pts[k + 1].x - pts[k].x, pts[k + 1].y - pts[k].y);
            if (len <= kTinyB) continue;
            double at = config.step - carry;
            for (; at < len - kTinyB; at += config.step) {
                const double t = at / len;
                sample(s, pts[k].x + t * (pts[k + 1].x - pts[k].x), pts[k].y + t * (pts[k + 1].y - pts[k].y));
            }
            carry = len - (at - config.step);
        }
        sample(s, pts.back().x, pts.back().y);
    }
    return true;
}

}  // namespace ftcore
