// The slice's hard rejects (ftcore/BlockCheck.h).

#include "ftcore/BlockCheck.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace ftcore {

namespace {

bool inRect(const BlockRect& r, double x, double y) { return r.contains(x, y); }

double rectGap(const BlockRect& a, const BlockRect& b) {
    const double dx = std::max({0.0, a.x0 - b.x1, b.x0 - a.x1});
    const double dy = std::max({0.0, a.y0 - b.y1, b.y0 - a.y1});
    return std::hypot(dx, dy);
}

// Whether a point lies inside a (possibly turned) box's footprint, grown by g.
bool inBox(const MassingBox& b, double x, double y, double g) {
    const double r = b.rot * 3.14159265358979323846 / 180.0;
    const double c = std::cos(r), s = std::sin(r);
    const double dx = x - b.cx, dy = y - b.cy;
    const double u = dx * c + dy * s, w = -dx * s + dy * c;
    return std::fabs(u) <= b.sx * 0.5 + g && std::fabs(w) <= b.sy * 0.5 + g;
}

bool isKerbed(const MassingStreet& s) { return s.pavement > 0.0 && s.width >= 10.0 - 1e-9; }
bool counts(const MassingStreet& s) { return s.kind != "alley" && s.width >= 3.5; }

void issue(BlockCheckReport& r, const char* rule, const std::string& id, double x, double y, double v) {
    BlockCheckIssue i;
    i.rule = rule;
    i.id = id;
    i.x = x, i.y = y, i.value = v;
    r.issues.push_back(i);
}

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

} // namespace

bool blockIsBuilding(const MassingBox& b) {
    if (b.kind != "kit" && b.kind != "box" && b.kind != "slab") return false;
    return b.z1 - b.z0 >= 2.5;
}

bool blockCheck(const BlockCheckInput& in, BlockCheckReport& out) {
    out = BlockCheckReport{};
    out.minDepth = std::numeric_limits<double>::infinity();
    if (!in.boxes) return true;
    const std::vector<MassingBox>& boxes = *in.boxes;
    const BlockRect wide{in.rect.x0 - 60.0, in.rect.y0 - 60.0, in.rect.x1 + 60.0, in.rect.y1 + 60.0};

    // buildings near the slice
    std::vector<std::size_t> near, mine;
    std::vector<BlockRect> foot(boxes.size());
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        if (!blockIsBuilding(boxes[i])) continue;
        foot[i] = blockFootprint(boxes[i]);
        if (!foot[i].overlaps(wide, 0.0)) continue;
        near.push_back(i);
        if (foot[i].overlaps(in.rect, 0.0)) mine.push_back(i);
    }
    out.buildings = mine.size();
    // streets within reach of the slice
    std::vector<const MassingStreet*> streets;
    const BlockRect reach{in.rect.x0 - 320.0, in.rect.y0 - 320.0, in.rect.x1 + 320.0, in.rect.y1 + 320.0};
    for (const MassingStreet& s : in.streets) {
        if (s.points.empty()) continue;
        BlockRect r{s.points[0].x, s.points[0].y, s.points[0].x, s.points[0].y};
        for (const MassingVec& p : s.points) {
            r.x0 = std::min(r.x0, p.x), r.y0 = std::min(r.y0, p.y), r.x1 = std::max(r.x1, p.x), r.y1 = std::max(r.y1, p.y);
        }
        r.x0 -= s.width, r.y0 -= s.width, r.x1 += s.width, r.y1 += s.width;
        if (r.overlaps(reach, 0.0)) streets.push_back(&s);
    }
    auto onBuilding = [&](double x, double y, double g) {
        for (const std::size_t i : near)
            if (inBox(boxes[i], x, y, g)) return true;
        return false;
    };
    auto onStreet = [&](double x, double y, const MassingStreet* skip, bool onlyCounting) {
        for (const MassingStreet* s : streets) {
            if (s == skip) continue;
            if (onlyCounting && !counts(*s)) continue;
            if (blockOnStreet(*s, x, y)) return true;
        }
        return false;
    };
    auto exempt = [&](double x, double y) {
        for (const BlockRect& r : in.exempt)
            if (inRect(r, x, y)) return true;
        return false;
    };

    for (const std::size_t i : mine) {
        const MassingBox& b = boxes[i];
        // lone
        bool friendNear = false;
        for (const std::size_t j : near)
            if (j != i && rectGap(foot[i], foot[j]) <= kBlockLoneGap && std::fabs(boxes[j].z0 - b.z0) <= kBlockLoneDz) {
                friendNear = true;
                break;
            }
        if (!friendNear) {
            ++out.lone;
            issue(out, "lone", b.id, b.cx, b.cy, 0.0);
        }
        // block depth: street edge to street edge through the centre
        double ext[2];
        for (int axis = 0; axis < 2; ++axis) {
            double sum = 0.0;
            for (int dir = -1; dir <= 1; dir += 2) {
                double d = std::numeric_limits<double>::infinity();
                for (double t = 0.0; t <= 300.0; t += 0.5) {
                    const double x = b.cx + (axis == 0 ? dir * t : 0.0), y = b.cy + (axis == 1 ? dir * t : 0.0);
                    if (onStreet(x, y, nullptr, true)) {
                        d = t;
                        break;
                    }
                }
                sum += d;
            }
            ext[axis] = sum;
        }
        const double depth = std::min(ext[0], ext[1]);
        if (std::isfinite(depth)) {
            out.minDepth = std::min(out.minDepth, depth);
            if (depth < kBlockMinDepth) {
                ++out.depth;
                issue(out, "depth", b.id, b.cx, b.cy, depth);
            }
        }
        // sink
        if (in.ground) {
            const BlockRect& f = foot[i];
            const int nx = std::max(2, std::min(16, static_cast<int>(std::ceil(f.w())) + 1));
            const int ny = std::max(2, std::min(16, static_cast<int>(std::ceil(f.h())) + 1));
            double hi = -1e18;
            for (int a = 0; a < nx; ++a)
                for (int c = 0; c < ny; ++c) {
                    const double x = f.x0 + 0.1 + (f.w() - 0.2) * a / (nx - 1), y = f.y0 + 0.1 + (f.h() - 0.2) * c / (ny - 1);
                    if (!inBox(b, x, y, 0.0)) continue;
                    hi = std::max(hi, in.ground->at(x, y));
                }
            const double sink = hi - b.z0;
            out.maxSink = std::max(out.maxSink, sink);
            if (sink > kBlockMaxSink) {
                ++out.sink;
                issue(out, "sink", b.id, b.cx, b.cy, sink);
            }
        }
    }

    for (const MassingStreet* sp : streets) {
        const MassingStreet& s = *sp;
        for (std::size_t k = 1; k < s.points.size(); ++k) {
            const double ax = s.points[k - 1].x, ay = s.points[k - 1].y, bx = s.points[k].x, by = s.points[k].y;
            const double len = std::hypot(bx - ax, by - ay);
            if (len <= 0.0) continue;
            const double tx = (bx - ax) / len, ty = (by - ay) / len, nx = -ty, ny = tx;
            // frontage on a kerbed side
            if (isKerbed(s))
                for (int side = -1; side <= 1; side += 2) {
                    double run = 0.0, rx = 0.0, ry = 0.0;
                    for (double t = 0.0; t <= len; t += 1.0) {
                        const double off = s.width * 0.5 + 1.0;
                        const double x = ax + tx * t + nx * side * off, y = ay + ty * t + ny * side * off;
                        if (!inRect(in.rect, x, y)) {
                            run = 0.0;
                            continue;
                        }
                        ++out.probes;
                        const bool ok = onBuilding(x, y, 0.5) || onStreet(x, y, &s, false) || exempt(x, y);
                        if (ok) {
                            run = 0.0;
                            continue;
                        }
                        if (run == 0.0) rx = x, ry = y;
                        run += 1.0;
                        if (run > kBlockFrontGap && run - 1.0 <= kBlockFrontGap) {
                            ++out.frontage;
                            issue(out, "frontage", s.id, rx, ry, run);
                        }
                    }
                }
            // paving
            const bool road = s.width >= 10.0 - 1e-9;
            const double limit = road ? kBlockPaveRoad : kBlockPaveLane;
            for (double t = 1.0; t < len; t += 2.0) {
                const double x = ax + tx * t, y = ay + ty * t;
                if (!inRect(in.rect, x, y) || onStreet(x, y, &s, false)) continue;
                ++out.stations;
                double d[2] = {-1.0, -1.0};
                bool ex = exempt(x, y);
                for (int side = 0; side < 2; ++side) {
                    const double sg = side == 0 ? -1.0 : 1.0;
                    for (double q = 0.0; q <= 30.0; q += 0.25) {
                        const double px = x + nx * sg * q, py = y + ny * sg * q;
                        if (exempt(px, py)) ex = true;
                        if (onBuilding(px, py, 0.0)) {
                            d[side] = q;
                            break;
                        }
                    }
                }
                if (d[0] < 0.0 || d[1] < 0.0) continue;
                const double span = d[0] + d[1];
                if (ex) continue;
                if (road) out.maxPaveRoad = std::max(out.maxPaveRoad, span);
                else out.maxPaveLane = std::max(out.maxPaveLane, span);
                if (span > limit) {
                    ++out.paving;
                    issue(out, "paving", s.id, x, y, span);
                }
            }
        }
    }
    if (!std::isfinite(out.minDepth)) out.minDepth = 0.0;
    return out.ok();
}

std::string blockCheckText(const BlockCheckReport& r, std::size_t listMax) {
    std::string t;
    t += "buildings checked " + std::to_string(r.buildings) + ", frontage probes " + std::to_string(r.probes) +
         ", paving stations " + std::to_string(r.stations) + "\n";
    t += "lone " + std::to_string(r.lone) + ", depth " + std::to_string(r.depth) + " (min " + num(r.minDepth) +
         " m), frontage " + std::to_string(r.frontage) + ", paving " + std::to_string(r.paving) + " (max road " +
         num(r.maxPaveRoad) + " m, lane " + num(r.maxPaveLane) + " m), sink " + std::to_string(r.sink) + " (max " +
         num(r.maxSink) + " m)\n";
    std::size_t n = 0;
    for (const BlockCheckIssue& i : r.issues) {
        if (++n > listMax) break;
        t += "  " + i.rule + " " + i.id + " at " + num(i.x) + "," + num(i.y) + " value " + num(i.value) + "\n";
    }
    t += r.ok() ? "BLOCKCHECK OK\n" : "BLOCKCHECK FAIL\n";
    return t;
}

} // namespace ftcore
