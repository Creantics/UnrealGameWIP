#include "ftcore/WorldChecks.h"

#include "CsvRead.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_set>

namespace ftcore {

namespace {

using detail::fail;
using detail::isComment;
using detail::parseNumber;
using detail::Record;
using detail::splitCsv;
using detail::trim;
using detail::validId;

// Finds each of `names` in a header record: col[c] is its field. False on a column named twice or missing.
template <std::size_t N>
bool mapColumns(const Record& r, const char* const (&names)[N], std::size_t (&col)[N], std::string* error) {
    for (std::size_t c = 0; c < N; ++c) {
        col[c] = r.fields.size();
        for (std::size_t f = 0; f < r.fields.size(); ++f) {
            if (trim(r.fields[f]) != names[c]) continue;
            if (col[c] != r.fields.size()) return fail(error, r.line, std::string("column ") + names[c] + " twice");
            col[c] = f;
        }
        if (col[c] == r.fields.size()) return fail(error, r.line, std::string("no column ") + names[c]);
    }
    return true;
}

bool parseKind(std::string_view s, TourStopKind& out) {
    if (s == "reach") out = TourStopKind::Reach;
    else if (s == "edge") out = TourStopKind::Edge;
    else if (s == "push") out = TourStopKind::Push;
    else return false;
    return true;
}

double halton(std::size_t index, std::size_t base) {
    double f = 1.0, r = 0.0;
    while (index > 0) {
        f /= static_cast<double>(base);
        r += f * static_cast<double>(index % base);
        index /= base;
    }
    return r;
}

// The six cell fractions readBackSurfacePoints takes in turn: each at least 0.1 off both diagonals.
constexpr double kCellFractions[6][2] = {{0.2, 0.7}, {0.7, 0.2}, {0.35, 0.9}, {0.9, 0.35}, {0.6, 0.15}, {0.15, 0.6}};

} // namespace

// ---- Tour stops ------------------------------------------------------------------------------------------------

const char* tourStopKindName(TourStopKind kind) {
    switch (kind) {
    case TourStopKind::Reach: return "reach";
    case TourStopKind::Edge: return "edge";
    case TourStopKind::Push: return "push";
    }
    return "reach";
}

bool importTourStops(std::string_view csv, std::vector<TourStop>& out, std::string* error) {
    out.clear();
    std::vector<Record> records;
    if (!splitCsv(csv, records, error)) return false;

    enum Col { Id, Kind, X, Y, Z, StartX, StartY, StartZ, Edge, Still, Count };
    static constexpr const char* kNames[Count] = {"id",      "kind",    "x",       "y",    "z",
                                                  "start_x", "start_y", "start_z", "edge", "still"};
    std::size_t col[Count] = {};
    std::size_t width = 0;
    bool haveHeader = false;
    std::unordered_set<std::string> ids;

    for (const Record& r : records) {
        if (isComment(r)) continue;
        if (!haveHeader) {
            if (!mapColumns(r, kNames, col, error)) return false;
            width = r.fields.size();
            haveHeader = true;
            continue;
        }
        if (r.fields.size() != width)
            return fail(error, r.line,
                        std::to_string(r.fields.size()) + " fields where the header has " + std::to_string(width));
        TourStop s;
        s.id = std::string(trim(r.fields[col[Id]]));
        if (!validId(s.id)) return fail(error, r.line, "id must be A-Z, 0-9, '_' or '-'");
        if (!ids.insert(s.id).second) return fail(error, r.line, "id " + s.id + " twice");
        if (!parseKind(trim(r.fields[col[Kind]]), s.kind))
            return fail(error, r.line, s.id + ": kind must be reach, edge or push");
        const Col targets[] = {X, Y, Z};
        double* targetOut[] = {&s.x, &s.y, &s.z};
        for (std::size_t k = 0; k < 3; ++k)
            if (!parseNumber(r.fields[col[targets[k]]], *targetOut[k]))
                return fail(error, r.line, s.id + ": " + kNames[targets[k]] + " is not a number");
        const Col starts[] = {StartX, StartY, StartZ};
        double* startOut[] = {&s.startX, &s.startY, &s.startZ};
        int empty = 0;
        for (const Col c : starts)
            if (trim(r.fields[col[c]]).empty()) ++empty;
        if (empty != 0 && empty != 3)
            return fail(error, r.line, s.id + ": start_x, start_y and start_z must be all empty or all numbers");
        if (empty == 0) {
            for (std::size_t k = 0; k < 3; ++k)
                if (!parseNumber(r.fields[col[starts[k]]], *startOut[k]))
                    return fail(error, r.line, s.id + ": " + kNames[starts[k]] + " is not a number");
            s.hasStart = true;
        }
        if (s.kind != TourStopKind::Reach && !s.hasStart)
            return fail(error, r.line, s.id + (s.kind == TourStopKind::Edge ? ": an edge" : ": a push") + " stop needs a start");
        const std::string_view edge = trim(r.fields[col[Edge]]);
        if (s.kind == TourStopKind::Edge) {
            if (edge != "W" && edge != "S" && edge != "N" && edge != "E")
                return fail(error, r.line, s.id + ": edge must be W, S, N or E on an edge stop");
            s.edge = edge.front();
        } else if (!edge.empty()) {
            return fail(error, r.line, s.id + ": edge must be empty on a " + tourStopKindName(s.kind) + " stop");
        }
        const std::string_view still = trim(r.fields[col[Still]]);
        if (still == "1") s.still = true;
        else if (still == "0") s.still = false;
        else return fail(error, r.line, s.id + ": still must be 0 or 1");
        out.push_back(std::move(s));
    }
    if (!haveHeader) return fail(error, 1, "no header row");
    return true;
}

bool parseTourKinds(std::string_view list, std::uint8_t& mask) {
    std::uint8_t m = 0;
    std::size_t i = 0;
    while (i <= list.size()) {
        std::size_t j = list.find_first_of(",+", i);
        if (j == std::string_view::npos) j = list.size();
        const std::string_view name = trim(list.substr(i, j - i));
        if (!name.empty()) {
            TourStopKind kind = TourStopKind::Reach;
            if (!parseKind(name, kind)) return false;
            m = static_cast<std::uint8_t>(m | (1u << static_cast<unsigned>(kind)));
        }
        i = j + 1;
    }
    mask = m != 0 ? m : kTourKindsAll;
    return true;
}

// ---- Edge stops ------------------------------------------------------------------------------------------------

const char* edgeCauseName(EdgeCause cause) {
    switch (cause) {
    case EdgeCause::Cliff: return "cliff";
    case EdgeCause::Water: return "water";
    case EdgeCause::Drop: return "drop";
    case EdgeCause::BuiltWall: return "built wall";
    case EdgeCause::BareHiddenWall: return "BARE HIDDEN WALL";
    case EdgeCause::Unjudged: return "UNJUDGED";
    }
    return "UNJUDGED";
}

EdgeCause judgeEdge(const EdgeProbe& p) {
    switch (p.hit) {
    case EdgeHit::Built: return EdgeCause::BuiltWall;
    case EdgeHit::Landscape:
        if (p.hitNormalZ < p.walkableNormalZ) return EdgeCause::Cliff;
        break; // walkable ground ahead: judged by the ground along the probe, as with no hit
    case EdgeHit::HiddenWall:
    case EdgeHit::None: break;
    }
    if (!p.groundFound) return EdgeCause::Drop;
    if (p.hasSea && p.groundZ < p.seaLevel) return EdgeCause::Water;
    if (p.feetZ - p.groundZ > kEdgeDropMetres) return EdgeCause::Drop;
    if (p.hit == EdgeHit::HiddenWall && std::fabs(p.groundZ - p.feetZ) <= kEdgeDryBand) return EdgeCause::BareHiddenWall;
    return EdgeCause::Unjudged;
}

// ---- Coverage points and navmesh islands -----------------------------------------------------------------------

bool importCoveragePoints(std::string_view csv, std::vector<CoveragePoint>& out, std::string* error) {
    out.clear();
    std::vector<Record> records;
    if (!splitCsv(csv, records, error)) return false;

    enum Col { Id, Role, X, Y, Z, Count };
    static constexpr const char* kNames[Count] = {"id", "role", "x", "y", "z"};
    std::size_t col[Count] = {};
    std::size_t width = 0;
    bool haveHeader = false;
    std::unordered_set<std::string> ids;

    for (const Record& r : records) {
        if (isComment(r)) continue;
        if (!haveHeader) {
            if (!mapColumns(r, kNames, col, error)) return false;
            width = r.fields.size();
            haveHeader = true;
            continue;
        }
        if (r.fields.size() != width)
            return fail(error, r.line,
                        std::to_string(r.fields.size()) + " fields where the header has " + std::to_string(width));
        CoveragePoint p;
        p.id = std::string(trim(r.fields[col[Id]]));
        if (!validId(p.id)) return fail(error, r.line, "id must be A-Z, 0-9, '_' or '-'");
        if (!ids.insert(p.id).second) return fail(error, r.line, "id " + p.id + " twice");
        const std::string_view role = trim(r.fields[col[Role]]);
        if (role == "pocket") p.role = CoverageRole::Pocket;
        else if (role == "on") p.role = CoverageRole::On;
        else return fail(error, r.line, p.id + ": role must be pocket or on");
        const Col nums[] = {X, Y, Z};
        double* numOut[] = {&p.x, &p.y, &p.z};
        for (std::size_t k = 0; k < 3; ++k)
            if (!parseNumber(r.fields[col[nums[k]]], *numOut[k]))
                return fail(error, r.line, p.id + ": " + kNames[nums[k]] + " is not a number");
        out.push_back(std::move(p));
    }
    if (!haveHeader) return fail(error, 1, "no header row");
    return true;
}

void navPolyArea(const MassingVec* corners, std::size_t count, double& area, double& cx, double& cy) {
    area = cx = cy = 0.0;
    if (!corners || count == 0) return;
    // About the first corner, so a poly far from the origin keeps its precision.
    const double x0 = corners[0].x, y0 = corners[0].y;
    double twice = 0.0, sx = 0.0, sy = 0.0, mx = 0.0, my = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double px = corners[i].x - x0, py = corners[i].y - y0;
        const double qx = corners[(i + 1) % count].x - x0, qy = corners[(i + 1) % count].y - y0;
        const double cross = px * qy - qx * py;
        twice += cross;
        sx += (px + qx) * cross;
        sy += (py + qy) * cross;
        mx += px;
        my += py;
    }
    if (std::fabs(twice) <= 1e-12) {
        cx = x0 + mx / static_cast<double>(count);
        cy = y0 + my / static_cast<double>(count);
        return;
    }
    area = std::fabs(twice) * 0.5;
    cx = x0 + sx / (3.0 * twice);
    cy = y0 + sy / (3.0 * twice);
}

void navPolysAboveSea(const std::vector<MassingVec>& corners, const std::vector<NavPolyRef>& polys, double seaLevel,
                      double margin, std::vector<std::uint32_t>& keep, SeaLeftOut& out) {
    keep.clear();
    keep.reserve(polys.size());
    out = SeaLeftOut{};
    const double line = seaLevel + margin;
    for (std::size_t i = 0; i < polys.size(); ++i) {
        const NavPolyRef& p = polys[i];
        if (p.count > 0 && static_cast<std::uint64_t>(p.first) + p.count <= corners.size()) {
            const MassingVec* const at = corners.data() + p.first;
            double top = at[0].z;
            for (std::uint32_t k = 1; k < p.count; ++k) top = std::max(top, at[k].z);
            if (top <= line) {
                double area = 0.0, cx = 0.0, cy = 0.0;
                navPolyArea(at, p.count, area, cx, cy);
                ++out.polys;
                out.area += area;
                continue;
            }
        }
        keep.push_back(static_cast<std::uint32_t>(i));
    }
}

void navIslands(const std::vector<MassingVec>& corners, const std::vector<NavPolyRef>& polys,
                const std::vector<NavLink>& links, NavIslandSet& out) {
    out.islands.clear();
    out.islandOf.assign(polys.size(), 0);
    out.area = 0.0;
    const std::size_t n = polys.size();
    std::vector<std::uint32_t> parent(n);
    std::iota(parent.begin(), parent.end(), 0u);
    const auto find = [&parent](std::uint32_t i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    for (const NavLink& l : links) {
        if (l.a >= n || l.b >= n) continue;
        const std::uint32_t ra = find(l.a), rb = find(l.b);
        if (ra != rb) parent[std::max(ra, rb)] = std::min(ra, rb);
    }

    struct Acc {
        NavIsland island;
        double wx = 0.0, wy = 0.0;    // area-weighted centroid sums
        double mx = 0.0, my = 0.0;    // plain sums of the polys' centroids
        bool anyZ = false;
    };
    std::vector<Acc> acc;
    std::vector<std::int64_t> islandOfRoot(n, -1);
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t r = find(i);
        if (islandOfRoot[r] < 0) {
            islandOfRoot[r] = static_cast<std::int64_t>(acc.size());
            acc.emplace_back();
            acc.back().island.firstPoly = i;
        }
        const std::size_t k = static_cast<std::size_t>(islandOfRoot[r]);
        out.islandOf[i] = static_cast<std::uint32_t>(k);
        Acc& a = acc[k];
        ++a.island.polys;
        const NavPolyRef& p = polys[i];
        if (static_cast<std::uint64_t>(p.first) + p.count > corners.size()) continue;
        double area = 0.0, cx = 0.0, cy = 0.0;
        navPolyArea(corners.data() + p.first, p.count, area, cx, cy);
        a.island.area += area;
        a.wx += area * cx;
        a.wy += area * cy;
        a.mx += cx;
        a.my += cy;
        for (std::uint32_t c = 0; c < p.count; ++c) {
            const double z = corners[p.first + c].z;
            if (!a.anyZ) {
                a.island.zMin = a.island.zMax = z;
                a.anyZ = true;
            } else {
                a.island.zMin = std::min(a.island.zMin, z);
                a.island.zMax = std::max(a.island.zMax, z);
            }
        }
    }
    for (Acc& a : acc) {
        if (a.island.area > 0.0) {
            a.island.cx = a.wx / a.island.area;
            a.island.cy = a.wy / a.island.area;
        } else {
            a.island.cx = a.mx / static_cast<double>(a.island.polys);
            a.island.cy = a.my / static_cast<double>(a.island.polys);
        }
        out.area += a.island.area;
    }
    // Largest first; acc is already in firstPoly order, which the stable sort keeps for equal areas.
    std::vector<std::size_t> order(acc.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(),
                     [&acc](std::size_t a, std::size_t b) { return acc[a].island.area > acc[b].island.area; });
    std::vector<std::uint32_t> rank(acc.size());
    out.islands.reserve(acc.size());
    for (std::size_t k = 0; k < order.size(); ++k) {
        rank[order[k]] = static_cast<std::uint32_t>(k);
        out.islands.push_back(acc[order[k]].island);
    }
    for (std::uint32_t& k : out.islandOf) k = rank[k];
}

void classifyNavIslands(const NavIslandSet& set, std::int64_t homePoly, const std::vector<CoveragePoint>& points,
                        const std::vector<std::int64_t>& pointPolys, double bigArea, CoverageSummary& out) {
    out = CoverageSummary{};
    const std::size_t ni = set.islands.size();
    const auto islandAt = [&set, ni](std::int64_t poly) -> std::int64_t {
        if (poly < 0 || static_cast<std::uint64_t>(poly) >= set.islandOf.size()) return -1;
        const std::uint32_t k = set.islandOf[static_cast<std::size_t>(poly)];
        return k < ni ? static_cast<std::int64_t>(k) : -1;
    };
    out.home = islandAt(homePoly);
    const auto big = [&](std::int64_t k) {
        return k >= 0 && k != out.home && set.islands[static_cast<std::size_t>(k)].area > bigArea;
    };
    std::vector<bool> marked(ni, false);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const std::int64_t k = islandAt(i < pointPolys.size() ? pointPolys[i] : -1);
        if (points[i].role == CoverageRole::Pocket) {
            ++out.pockets;
            if (k < 0) {
                ++out.pocketsOffMesh;
            } else if (big(k)) {
                marked[static_cast<std::size_t>(k)] = true;
                ++out.pocketsMarking;
            }
        } else {
            ++out.onPoints;
            if (k >= 0 && k == out.home) ++out.onHome;
            else out.onMissed.push_back(i);
        }
    }
    out.classes.assign(ni, NavIslandClass::Small);
    for (std::size_t k = 0; k < ni; ++k) {
        const std::int64_t sk = static_cast<std::int64_t>(k);
        if (sk == out.home) {
            out.classes[k] = NavIslandClass::Home;
        } else if (big(sk)) {
            ++out.big;
            if (marked[k]) {
                out.classes[k] = NavIslandClass::Declared;
                ++out.bigDeclared;
            } else {
                out.classes[k] = NavIslandClass::Undeclared;
                ++out.bigUndeclared;
                out.undeclared.push_back(k);
            }
        }
    }
    if (out.home >= 0) {
        out.homeArea = set.islands[static_cast<std::size_t>(out.home)].area;
        out.homePercent = set.area > 0.0 ? 100.0 * out.homeArea / set.area : 0.0;
    }
}

// ---- Read-back of the built Landscape --------------------------------------------------------------------------

bool readBackWindow(std::size_t landSide, std::size_t builtX, std::size_t builtY, double builtSpacing,
                    double builtOriginX, double builtOriginY, std::size_t readX, std::size_t readY,
                    double readSpacing, double readOriginX, double readOriginY, ReadBackWindow& out,
                    std::string* error) {
    out = ReadBackWindow{};
    const auto no = [error](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (landSide == 0 || builtX == 0 || builtY == 0 || readX == 0 || readY == 0) return no("a size is zero");
    if (!(builtSpacing > 0.0) || !(readSpacing > 0.0) || !std::isfinite(builtSpacing) || !std::isfinite(readSpacing))
        return no("a spacing isn't a positive number");
    if (!std::isfinite(builtOriginX) || !std::isfinite(builtOriginY) || !std::isfinite(readOriginX) ||
        !std::isfinite(readOriginY))
        return no("an origin isn't a number");
    if (std::fabs(builtSpacing - readSpacing) > 1e-9 * builtSpacing)
        return no("the read-back field's spacing " + std::to_string(readSpacing) + " m isn't the built field's " +
                  std::to_string(builtSpacing) + " m");
    if (builtX > landSide || builtY > landSide) return no("the built field doesn't fit the Landscape's square");
    const double fx = (readOriginX - builtOriginX) / builtSpacing, fy = (readOriginY - builtOriginY) / builtSpacing;
    const double rx = std::round(fx), ry = std::round(fy);
    if (std::fabs(fx - rx) > 1e-6 || std::fabs(fy - ry) > 1e-6)
        return no("the read-back field's origin isn't on the built field's grid");
    if (rx < 0.0 || ry < 0.0 || rx + static_cast<double>(readX) > static_cast<double>(builtX) ||
        ry + static_cast<double>(readY) > static_cast<double>(builtY))
        return no("the read-back field (" + std::to_string(readX) + " by " + std::to_string(readY) +
                  " samples from the built field's sample (" + std::to_string(static_cast<long long>(rx)) + ", " +
                  std::to_string(static_cast<long long>(ry)) + ")) doesn't lie inside the built field (" +
                  std::to_string(builtX) + " by " + std::to_string(builtY) + ")");
    out.nx = readX;
    out.ny = readY;
    out.ix0 = static_cast<std::size_t>(rx);
    out.iy0 = static_cast<std::size_t>(ry);
    out.x1 = out.ix0;
    out.x2 = out.ix0 + readX - 1;
    // The built field's row iy is the Landscape's row landSide - 1 - iy (massingLandscapeHeights writes north first).
    out.y1 = landSide - 1 - (out.iy0 + readY - 1);
    out.y2 = landSide - 1 - out.iy0;
    return true;
}

void readBackSentinel(const std::uint16_t* field, std::size_t nx, std::size_t ny, std::uint16_t* land) {
    if (!field || !land) return;
    for (std::size_t k = 0; k < ny; ++k) {
        const std::uint16_t* src = field + (ny - 1 - k) * nx;
        std::uint16_t* dst = land + k * nx;
        for (std::size_t i = 0; i < nx; ++i) dst[i] = static_cast<std::uint16_t>(~src[i]);
    }
}

void compareHeightsExact(const std::uint16_t* field, std::size_t nx, std::size_t ny, double spacing, double originX,
                         double originY, const std::uint16_t* land, HeightCompare& out) {
    out = HeightCompare{};
    if (!field || !land) return;
    for (std::size_t j = 0; j < ny; ++j) {
        const std::uint16_t* want = field + j * nx;
        const std::uint16_t* got = land + (ny - 1 - j) * nx;
        for (std::size_t i = 0; i < nx; ++i) {
            ++out.compared;
            if (want[i] == got[i]) continue;
            ++out.mismatched;
            const double e = std::fabs(massingHeightFromValue(got[i]) - massingHeightFromValue(want[i]));
            if (e > out.worst) {
                out.worst = e;
                out.worstX = originX + static_cast<double>(i) * spacing;
                out.worstY = originY + static_cast<double>(j) * spacing;
            }
        }
    }
}

double heightOnTriangles(const std::uint16_t* grid, std::size_t samplesX, std::size_t samplesY, double spacing,
                         double originX, double originY, double x, double y, CellDiagonal diagonal) {
    if (!grid || samplesX == 0 || samplesY == 0 || !(spacing > 0.0) || !std::isfinite(x) || !std::isfinite(y))
        return 0.0;
    const double fx = std::clamp((x - originX) / spacing, 0.0, static_cast<double>(samplesX - 1));
    const double fy = std::clamp((y - originY) / spacing, 0.0, static_cast<double>(samplesY - 1));
    const std::size_t c0 = static_cast<std::size_t>(fx), r0 = static_cast<std::size_t>(fy);
    const std::size_t c1 = std::min(c0 + 1, samplesX - 1), r1 = std::min(r0 + 1, samplesY - 1);
    const double tx = fx - static_cast<double>(c0), ty = fy - static_cast<double>(r0);
    const auto h = [&](std::size_t c, std::size_t r) { return massingHeightFromValue(grid[r * samplesX + c]); };
    const double h00 = h(c0, r0), h10 = h(c1, r0), h01 = h(c0, r1), h11 = h(c1, r1);
    if (diagonal == CellDiagonal::SouthWestNorthEast) {
        if (tx >= ty) return h00 + tx * (h10 - h00) + ty * (h11 - h10); // (0,0) (1,0) (1,1)
        return h00 + ty * (h01 - h00) + tx * (h11 - h01);                // (0,0) (0,1) (1,1)
    }
    if (tx + ty <= 1.0) return h00 + tx * (h10 - h00) + ty * (h01 - h00); // (0,0) (1,0) (0,1)
    return h11 + (1.0 - tx) * (h01 - h11) + (1.0 - ty) * (h10 - h11);     // (1,1) (0,1) (1,0)
}

void readBackSurfacePoints(std::size_t samplesX, std::size_t samplesY, double spacing, double originX, double originY,
                           std::size_t count, std::vector<SurfacePoint>& out) {
    out.clear();
    if (samplesX < 2 || samplesY < 2 || !(spacing > 0.0)) return;
    out.reserve(count);
    const std::size_t cellsX = samplesX - 1, cellsY = samplesY - 1;
    for (std::size_t k = 0; k < count; ++k) {
        const std::size_t cx = std::min(static_cast<std::size_t>(halton(k + 1, 2) * static_cast<double>(cellsX)), cellsX - 1);
        const std::size_t cy = std::min(static_cast<std::size_t>(halton(k + 1, 3) * static_cast<double>(cellsY)), cellsY - 1);
        const double* f = kCellFractions[k % 6];
        out.push_back({originX + (static_cast<double>(cx) + f[0]) * spacing,
                       originY + (static_cast<double>(cy) + f[1]) * spacing});
    }
}

void compareSurface(const std::vector<SurfacePoint>& points, const std::vector<double>& measured,
                    const std::uint16_t* grid, std::size_t samplesX, std::size_t samplesY, double spacing,
                    double originX, double originY, CellDiagonal diagonal, double tolerance, SurfaceCompare& out) {
    out = SurfaceCompare{};
    for (std::size_t i = 0; i < points.size(); ++i) {
        ++out.compared;
        if (i >= measured.size() || !std::isfinite(measured[i])) {
            ++out.unread;
            ++out.over;
            continue;
        }
        const double want =
            heightOnTriangles(grid, samplesX, samplesY, spacing, originX, originY, points[i].x, points[i].y, diagonal);
        const double e = std::fabs(measured[i] - want);
        if (e > out.worst) {
            out.worst = e;
            out.worstX = points[i].x;
            out.worstY = points[i].y;
        }
        if (e > tolerance) ++out.over;
    }
}

} // namespace ftcore
