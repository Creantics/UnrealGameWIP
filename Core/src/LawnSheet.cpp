#include "ftcore/LawnSheet.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace ftcore {

namespace {

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '"')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '"')) s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> cells(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t at = 0;
    while (true) {
        const std::size_t comma = line.find(',', at);
        out.push_back(trimmed(line.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at)));
        if (comma == std::string_view::npos) break;
        at = comma + 1;
    }
    return out;
}

bool number(std::string_view s, double& out) {
    if (s.empty()) return false;
    const std::string text(s);
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    return end == text.c_str() + text.size() && std::isfinite(out);
}

bool fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

} // namespace

bool parseLawnTable(std::string_view csv, LawnTable& out, std::string* error) {
    out = {};
    int col[6] = {-1, -1, -1, -1, -1, -1};
    static constexpr std::string_view names[6] = {"kind", "x0", "y0", "x1", "y1", "width"};
    bool header = false;
    std::size_t lineNo = 0;
    std::size_t at = 0;
    while (at <= csv.size()) {
        const std::size_t nl = csv.find('\n', at);
        const std::string_view raw = csv.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
        at = nl == std::string_view::npos ? csv.size() + 1 : nl + 1;
        ++lineNo;
        const std::string_view line = trimmed(raw);
        if (line.empty() || line.front() == '#') continue;
        const std::vector<std::string_view> c = cells(line);
        if (!header) {
            for (std::size_t i = 0; i < c.size(); ++i)
                for (int k = 0; k < 6; ++k)
                    if (c[i] == names[k]) col[k] = static_cast<int>(i);
            for (int k = 0; k < 5; ++k)
                if (col[k] < 0) return fail(error, "the header has no column " + std::string(names[k]));
            header = true;
            continue;
        }
        double v[6] = {0, 0, 0, 0, 0, 2.0};
        for (int k = 1; k < 6; ++k) {
            if (col[k] < 0 || static_cast<std::size_t>(col[k]) >= c.size() || c[static_cast<std::size_t>(col[k])].empty()) {
                if (k == 5) continue;
                return fail(error, "line " + std::to_string(lineNo) + " has no " + std::string(names[k]));
            }
            if (!number(c[static_cast<std::size_t>(col[k])], v[k]))
                return fail(error, "line " + std::to_string(lineNo) + ": bad " + std::string(names[k]));
        }
        const std::string_view kind = static_cast<std::size_t>(col[0]) < c.size() ? c[static_cast<std::size_t>(col[0])] : "";
        if (kind == "region") {
            LawnRegion r{std::min(v[1], v[3]), std::min(v[2], v[4]), std::max(v[1], v[3]), std::max(v[2], v[4])};
            if (r.x1 - r.x0 < 1e-3 || r.y1 - r.y0 < 1e-3) return fail(error, "line " + std::to_string(lineNo) + ": an empty region");
            out.regions.push_back(r);
        } else if (kind == "path") {
            LawnPath p{v[1], v[2], v[3], v[4], v[5]};
            if (std::hypot(p.x1 - p.x0, p.y1 - p.y0) < 1e-3 || p.width <= 0.0)
                return fail(error, "line " + std::to_string(lineNo) + ": a path with no length or width");
            out.paths.push_back(p);
        } else {
            return fail(error, "line " + std::to_string(lineNo) + ": unknown kind '" + std::string(kind) + "'");
        }
    }
    if (!header) return fail(error, "no header row");
    return true;
}

bool parsePlantPoints(std::string_view csv, std::vector<PlantPoint>& out, std::string* error) {
    out.clear();
    static constexpr std::string_view names[7] = {"asset", "x", "y", "z", "yaw", "scale", "kind"};
    int col[7] = {-1, -1, -1, -1, -1, -1, -1};
    bool header = false;
    std::size_t lineNo = 0, at = 0;
    while (at <= csv.size()) {
        const std::size_t nl = csv.find('\n', at);
        const std::string_view line = trimmed(csv.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at));
        at = nl == std::string_view::npos ? csv.size() + 1 : nl + 1;
        ++lineNo;
        if (line.empty() || line.front() == '#') continue;
        const std::vector<std::string_view> c = cells(line);
        if (!header) {
            for (std::size_t i = 0; i < c.size(); ++i)
                for (int k = 0; k < 7; ++k)
                    if (c[i] == names[k]) col[k] = static_cast<int>(i);
            for (int k = 0; k < 7; ++k)
                if (col[k] < 0) return fail(error, "the header has no column " + std::string(names[k]));
            header = true;
            continue;
        }
        const auto cell = [&](int k) -> std::string_view {
            const auto i = static_cast<std::size_t>(col[k]);
            return i < c.size() ? c[i] : std::string_view{};
        };
        const std::string where = "line " + std::to_string(lineNo);
        PlantPoint p;
        p.asset = std::string(cell(0));
        if (p.asset.empty()) return fail(error, where + ": no asset");
        if (!number(cell(1), p.x) || !number(cell(2), p.y) || !number(cell(4), p.yaw) || !number(cell(5), p.scale))
            return fail(error, where + ": bad x, y, yaw or scale");
        if (!(p.scale > 0.0 && p.scale <= 10.0)) return fail(error, where + ": scale out of range");
        if (!cell(3).empty()) {
            if (!number(cell(3), p.z)) return fail(error, where + ": bad z");
            p.hasZ = true;
        }
        if (cell(6) == "shrub") p.kind = PlantPointKind::Shrub;
        else if (cell(6) == "cover") p.kind = PlantPointKind::Cover;
        else if (cell(6) == "blades") p.kind = PlantPointKind::Blades;
        else return fail(error, where + ": unknown kind '" + std::string(cell(6)) + "'");
        out.push_back(std::move(p));
    }
    if (!header) return fail(error, "no header row");
    return true;
}

void lawnSheetMesh(const HeightField& ground, const LawnRegion& region, double step, double lift, LawnMesh& out) {
    const double w = region.x1 - region.x0, h = region.y1 - region.y0;
    const int nx = std::max(1, static_cast<int>(std::ceil(w / std::max(step, 0.05) - 1e-9)));
    const int ny = std::max(1, static_cast<int>(std::ceil(h / std::max(step, 0.05) - 1e-9)));
    const auto base = static_cast<std::uint32_t>(out.v.size());
    for (int j = 0; j <= ny; ++j)
        for (int i = 0; i <= nx; ++i) {
            const double x = region.x0 + w * i / nx, y = region.y0 + h * j / ny;
            out.v.push_back({x, y, ground.at(x, y) + lift});
        }
    const auto at = [&](int i, int j) { return base + static_cast<std::uint32_t>(j * (nx + 1) + i); };
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            // Alternate the diagonal so the sheet has no grain on a slope.
            if (((i + j) & 1) == 0) {
                out.tris.push_back({at(i, j), at(i + 1, j), at(i + 1, j + 1)});
                out.tris.push_back({at(i, j), at(i + 1, j + 1), at(i, j + 1)});
            } else {
                out.tris.push_back({at(i, j), at(i + 1, j), at(i, j + 1)});
                out.tris.push_back({at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)});
            }
        }
}

void lawnPathMesh(const HeightField& ground, const LawnPath& path, double step, double lift, double skirt, LawnMesh& out) {
    const double dx = path.x1 - path.x0, dy = path.y1 - path.y0;
    const double len = std::hypot(dx, dy);
    if (len < 1e-6) return;
    const double ux = dx / len, uy = dy / len;
    const double lx = -uy, ly = ux; // left of the path's direction
    const int n = std::max(1, static_cast<int>(std::ceil(len / std::max(step, 0.05) - 1e-9)));
    const double hw = path.width * 0.5;
    // Across each station: left skirt foot, left edge, centre, right edge, right skirt foot.
    static constexpr double across[5] = {1.0, 1.0, 0.0, -1.0, -1.0};
    const auto base = static_cast<std::uint32_t>(out.v.size());
    for (int s = 0; s <= n; ++s) {
        const double cx = path.x0 + dx * s / n, cy = path.y0 + dy * s / n;
        for (int k = 0; k < 5; ++k) {
            const double x = cx + lx * hw * across[k], y = cy + ly * hw * across[k];
            const double g = ground.at(x, y);
            out.v.push_back({x, y, (k == 0 || k == 4) ? g - skirt : g + lift});
        }
    }
    const auto at = [&](int s, int k) { return base + static_cast<std::uint32_t>(s * 5 + k); };
    for (int s = 0; s < n; ++s)
        for (int k = 0; k < 4; ++k) {
            // Counter-clockwise seen from above (and from outside on the skirts): along runs +u, k runs right.
            out.tris.push_back({at(s, k), at(s, k + 1), at(s + 1, k + 1)});
            out.tris.push_back({at(s, k), at(s + 1, k + 1), at(s + 1, k)});
        }
}

namespace {
// The part [t0, t1] of the segment a + t*(b - a), t in [0, 1], inside the rectangle around path p grown by `grow` m
// on every side; false when the segment misses it.
bool insideRect(double ax, double ay, double bx, double by, const LawnPath& p, double grow, double& t0, double& t1) {
    const double dx = p.x1 - p.x0, dy = p.y1 - p.y0;
    const double len = std::hypot(dx, dy);
    if (len < 1e-9) return false;
    const double ux = dx / len, uy = dy / len;
    // The segment in the path's frame: u along it from its start, v across it.
    const double au = (ax - p.x0) * ux + (ay - p.y0) * uy, av = -(ax - p.x0) * uy + (ay - p.y0) * ux;
    const double du = (bx - ax) * ux + (by - ay) * uy, dv = -(bx - ax) * uy + (by - ay) * ux;
    t0 = 0.0;
    t1 = 1.0;
    const auto slab = [&](double a, double d, double lo, double hi) {
        if (std::abs(d) < 1e-12) return a >= lo && a <= hi;
        double s0 = (lo - a) / d, s1 = (hi - a) / d;
        if (s0 > s1) std::swap(s0, s1);
        t0 = std::max(t0, s0);
        t1 = std::min(t1, s1);
        return t0 < t1;
    };
    const double hw = p.width * 0.5 + grow;
    return slab(au, du, -grow, len + grow) && slab(av, dv, -hw, hw);
}
} // namespace

void lawnPathEdges(const LawnTable& table, double edge, std::vector<LawnPath>& out) {
    out.clear();
    if (!(edge > 0.0)) return;
    std::vector<std::pair<double, double>> cut;
    for (std::size_t i = 0; i < table.paths.size(); ++i) {
        const LawnPath& p = table.paths[i];
        const double dx = p.x1 - p.x0, dy = p.y1 - p.y0;
        const double len = std::hypot(dx, dy);
        if (len < 1e-9) continue;
        const double lx = -dy / len, ly = dx / len;
        const double off = p.width * 0.5 + edge * 0.5;
        for (const double side : {1.0, -1.0}) {
            const double ax = p.x0 + lx * off * side, ay = p.y0 + ly * off * side;
            const double bx = p.x1 + lx * off * side, by = p.y1 + ly * off * side;
            // Off where another path or its own edging runs: the strip stops at the other path's edging line.
            cut.clear();
            for (std::size_t j = 0; j < table.paths.size(); ++j) {
                double t0 = 0.0, t1 = 0.0;
                if (j != i && insideRect(ax, ay, bx, by, table.paths[j], edge, t0, t1)) cut.emplace_back(t0, t1);
            }
            std::sort(cut.begin(), cut.end());
            double from = 0.0;
            const auto keep = [&](double t0, double t1) {
                if ((t1 - t0) * len < kLawnEdgeMinM) return;
                out.push_back({ax + (bx - ax) * t0, ay + (by - ay) * t0, ax + (bx - ax) * t1, ay + (by - ay) * t1, edge});
            };
            for (const auto& [c0, c1] : cut) {
                if (c0 > from) keep(from, c0);
                from = std::max(from, c1);
            }
            if (from < 1.0) keep(from, 1.0);
        }
    }
}

} // namespace ftcore
