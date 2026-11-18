// The slice's hard rejects (ftcore/BlockCheck.h).

#include "ftcore/BlockCheck.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

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

// a road: kerbed and 10 m or wider (lanes carry kerbed footways too, so the width tells them apart)
bool isRoad(const MassingStreet& s) { return s.pavement > 0.0 && s.width >= 10.0 - 1e-9; }
bool counts(const MassingStreet& s) { return s.kind != "alley" && s.width > kBlockAlleyMax + 1e-9; }

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

// Samples along a street no more than step apart (its points included).
void samplesOf(const MassingStreet& s, double step, std::vector<MassingVec>& out) {
    out.clear();
    if (s.points.size() == 1) out.push_back(s.points[0]);
    for (std::size_t i = 1; i < s.points.size(); ++i) {
        const double ax = s.points[i - 1].x, ay = s.points[i - 1].y, bx = s.points[i].x, by = s.points[i].y;
        const int n = std::max(1, static_cast<int>(std::ceil(std::hypot(bx - ax, by - ay) / step)));
        for (int k = (i == 1 ? 0 : 1); k <= n; ++k) {
            const double f = static_cast<double>(k) / n;
            MassingVec v{};
            v.x = ax + f * (bx - ax);
            v.y = ay + f * (by - ay);
            out.push_back(v);
        }
    }
}

// Whether the segment p -> q (t in (0, 1)) passes through a box (axis-aligned bounds of its footprint, z0..z1).
bool rayHits(const BlockRect& f, double z0, double z1, const double p[3], const double q[3]) {
    const double lo[3] = {f.x0, f.y0, z0}, hi[3] = {f.x1, f.y1, z1};
    double t0 = 0.0, t1 = 1.0;
    for (int k = 0; k < 3; ++k) {
        const double d = q[k] - p[k];
        if (std::fabs(d) < 1e-12) {
            if (p[k] < lo[k] || p[k] > hi[k]) return false;
            continue;
        }
        double a = (lo[k] - p[k]) / d, b = (hi[k] - p[k]) / d;
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a), t1 = std::min(t1, b);
        if (t0 > t1) return false;
    }
    return t1 > 1e-6 && t0 < 1.0 - 1e-6;
}

void routeRules(const BlockCheckInput& in, const std::vector<MassingBox>& boxes, BlockCheckReport& out) {
    out.routes = true;
    const BlockRect& R = in.rect;
    const BlockRect R20{R.x0 - 20.0, R.y0 - 20.0, R.x1 + 20.0, R.y1 + 20.0};
    std::vector<const MassingStreet*> net;  // streets within 200 m (the walk graph)
    std::vector<const MassingStreet*> here; // streets through the slice (a point within 20 m)
    for (const MassingStreet& s : in.streets) {
        if (s.points.empty()) continue;
        if (blockStreetNearRect(s, R, 200.0)) net.push_back(&s);
        if (!blockStreetNearRect(s, R, 20.0)) continue;
        here.push_back(&s);
        const BlockRole role = blockStreetRole(s);
        if (role == BlockRole::Main) ++out.mains;
        else if (role == BlockRole::Lane) ++out.lanes;
        else ++out.alleys;
    }
    auto reason = [&](double x, double y) {
        for (const BlockRect& r : in.reasons)
            if (x >= r.x0 - 3.0 && x <= r.x1 + 3.0 && y >= r.y0 - 3.0 && y <= r.y1 + 3.0) return true;
        return false;
    };

    // section: each class's widths (a lane carries kerbed footways)
    for (const MassingStreet* s : here) {
        const BlockRole role = blockStreetRole(*s);
        const double w = s->width, p = s->pavement, e = 1e-9;
        const char* why = nullptr;
        if (role == BlockRole::Main) {
            if (w > 14.0 + e || p < 2.0 - e || p > 3.0 + e) why = "section_road";
        } else if (role == BlockRole::Lane) {
            if (p <= 0.0) why = "section_kerbless";
            else if (w < 4.0 - e || w > 6.0 + e || p < 1.0 - e || p > 1.5 + e) why = "section_lane";
        } else if (w > kBlockAlleyMax + e || p > 0.0) {
            why = "section_alley";
        }
        if (!why) continue;
        ++out.section;
        issue(out, why, s->id, s->points[0].x, s->points[0].y, w);
    }
    // main: straight for kBlockMainStraight m (a kerbed link under kBlockLinkMax m between two roads is no main street)
    for (const MassingStreet* s : here) {
        if (blockStreetRole(*s) != BlockRole::Main) continue;
        double len = 0.0;
        for (std::size_t i = 1; i < s->points.size(); ++i)
            len += std::hypot(s->points[i].x - s->points[i - 1].x, s->points[i].y - s->points[i - 1].y);
        if (len < kBlockLinkMax) {
            ++out.links;
            continue;
        }
        const double run = blockStraightRun(*s);
        if (run < kBlockMainStraight) {
            ++out.mainShort;
            issue(out, "main", s->id, s->points[0].x, s->points[0].y, run);
        }
    }
    // lanes: short cuts between main streets, no chains, no dead ends without a reason
    for (const MassingStreet* s : here) {
        if (blockStreetRole(*s) != BlockRole::Lane || s->points.size() < 2) continue;
        for (int e = 0; e < 2; ++e) {
            const MassingVec& p = e == 0 ? s->points.front() : s->points.back();
            if (!R20.contains(p.x, p.y)) continue;
            bool main = false, lane = false;
            for (const MassingStreet* o : net) {
                if (o == s || !blockOnStreet(*o, p.x, p.y, 2.0)) continue;
                const BlockRole r = blockStreetRole(*o);
                if (r == BlockRole::Main) main = true;
                else if (r == BlockRole::Lane) lane = true;
            }
            if (main) continue;
            if (lane) {
                ++out.chain;
                issue(out, "chain", s->id, p.x, p.y, 0.0);
            } else if (!reason(p.x, p.y)) {
                ++out.deadEnd;
                issue(out, "deadend", s->id, p.x, p.y, 0.0);
            }
        }
    }
    // lanes crossing lanes away from their ends
    for (std::size_t i = 0; i < here.size(); ++i)
        for (std::size_t j = i + 1; j < here.size(); ++j) {
            const MassingStreet& a = *here[i];
            const MassingStreet& b = *here[j];
            if (blockStreetRole(a) != BlockRole::Lane || blockStreetRole(b) != BlockRole::Lane) continue;
            bool hit = false;
            for (std::size_t si = 1; si < a.points.size() && !hit; ++si)
                for (std::size_t sj = 1; sj < b.points.size() && !hit; ++sj) {
                    const double ax = a.points[si - 1].x, ay = a.points[si - 1].y;
                    const double adx = a.points[si].x - ax, ady = a.points[si].y - ay;
                    const double bx = b.points[sj - 1].x, by = b.points[sj - 1].y;
                    const double bdx = b.points[sj].x - bx, bdy = b.points[sj].y - by;
                    const double den = adx * bdy - ady * bdx;
                    if (std::fabs(den) < 1e-9) continue;
                    const double ta = ((bx - ax) * bdy - (by - ay) * bdx) / den;
                    const double tb = ((bx - ax) * ady - (by - ay) * adx) / den;
                    if (ta < 0.0 || ta > 1.0 || tb < 0.0 || tb > 1.0) continue;
                    const double x = ax + ta * adx, y = ay + ta * ady;
                    if (!R20.contains(x, y)) continue;
                    const double g = std::max(a.width, b.width) * 0.5 + 2.0;
                    const auto nearEnd = [&](const MassingStreet& s) {
                        return std::hypot(x - s.points.front().x, y - s.points.front().y) <= g ||
                               std::hypot(x - s.points.back().x, y - s.points.back().y) <= g;
                    };
                    if (nearEnd(a) || nearEnd(b)) continue; // an end on a lane: counted above
                    hit = true;
                    ++out.chain;
                    issue(out, "chain", a.id + "/" + b.id, x, y, 0.0);
                }
        }

    // walk: multi-source shortest paths from the main streets over 2 m samples
    struct Node {
        double x, y;
        std::size_t street;
    };
    std::vector<Node> nodes;
    std::vector<std::size_t> first(net.size() + 1, 0);
    std::vector<MassingVec> tmp;
    for (std::size_t k = 0; k < net.size(); ++k) {
        first[k] = nodes.size();
        samplesOf(*net[k], 2.0, tmp);
        for (const MassingVec& v : tmp) nodes.push_back({v.x, v.y, k});
    }
    first[net.size()] = nodes.size();
    std::vector<std::vector<std::pair<std::size_t, double>>> adj(nodes.size());
    for (std::size_t k = 0; k < net.size(); ++k)
        for (std::size_t i = first[k] + 1; i < first[k + 1]; ++i) {
            const double d = std::hypot(nodes[i].x - nodes[i - 1].x, nodes[i].y - nodes[i - 1].y);
            adj[i].push_back({i - 1, d});
            adj[i - 1].push_back({i, d});
        }
    // junctions: a sample on another street joins that street's nearest sample
    for (std::size_t i = 0; i < nodes.size(); ++i)
        for (std::size_t k = 0; k < net.size(); ++k) {
            if (k == nodes[i].street || !blockOnStreet(*net[k], nodes[i].x, nodes[i].y, 2.0)) continue;
            std::size_t best = first[k];
            double bd = std::numeric_limits<double>::infinity();
            for (std::size_t j = first[k]; j < first[k + 1]; ++j) {
                const double d = std::hypot(nodes[j].x - nodes[i].x, nodes[j].y - nodes[i].y);
                if (d < bd) bd = d, best = j;
            }
            if (first[k] == first[k + 1]) continue;
            adj[i].push_back({best, bd});
            adj[best].push_back({i, bd});
        }
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<double> dist(nodes.size(), inf);
    using Item = std::pair<double, std::size_t>;
    std::vector<Item> heap;
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (blockStreetRole(*net[nodes[i].street]) == BlockRole::Main) {
            dist[i] = 0.0;
            heap.push_back({0.0, i});
        }
    const auto cmp = [](const Item& a, const Item& b) { return a.first > b.first; };
    std::make_heap(heap.begin(), heap.end(), cmp);
    while (!heap.empty()) {
        std::pop_heap(heap.begin(), heap.end(), cmp);
        const Item it = heap.back();
        heap.pop_back();
        if (it.first > dist[it.second]) continue;
        for (const auto& e : adj[it.second]) {
            const double d = it.first + e.second;
            if (d < dist[e.first]) {
                dist[e.first] = d;
                heap.push_back({d, e.first});
                std::push_heap(heap.begin(), heap.end(), cmp);
            }
        }
    }
    for (std::size_t k = 0; k < net.size(); ++k) {
        if (blockStreetRole(*net[k]) == BlockRole::Main) continue;
        double worst = -1.0, wx = 0.0, wy = 0.0;
        for (std::size_t i = first[k]; i < first[k + 1]; ++i) {
            if (!R.contains(nodes[i].x, nodes[i].y)) continue;
            ++out.walkSamples;
            const double d = std::isfinite(dist[i]) ? dist[i] : 9999.0;
            if (d > kBlockWalkMax) ++out.walkOver;
            if (d > worst) worst = d, wx = nodes[i].x, wy = nodes[i].y;
        }
        if (worst < 0.0) continue;
        out.maxWalk = std::max(out.maxWalk, worst);
        if (worst > kBlockWalkMax) {
            ++out.walk;
            issue(out, "walk", net[k]->id, wx, wy, worst);
        }
    }

    // sight: a landmark or the sea from some point of every main street through the slice
    if (in.sights.empty() && !in.sea) return;
    std::vector<std::size_t> bld;
    for (std::size_t i = 0; i < boxes.size(); ++i)
        if (blockIsBuilding(boxes[i])) bld.push_back(i);
    std::vector<BlockRect> foot(boxes.size());
    for (const std::size_t i : bld) foot[i] = blockFootprint(boxes[i]);
    const auto groundAt = [&](double x, double y) { return in.ground ? in.ground->at(x, y) : 0.0; };
    const auto clear = [&](const double p[3], const double q[3]) {
        const BlockRect span{std::min(p[0], q[0]), std::min(p[1], q[1]), std::max(p[0], q[0]), std::max(p[1], q[1])};
        const auto blocks = [&](const BlockRect& f, double z0, double z1) {
            if (f.x1 < span.x0 || f.x0 > span.x1 || f.y1 < span.y0 || f.y0 > span.y1) return false;
            if (q[0] >= f.x0 && q[0] <= f.x1 && q[1] >= f.y0 && q[1] <= f.y1) return false; // the target's own box
            return rayHits(f, z0, z1, p, q);
        };
        for (const std::size_t i : bld)
            if (blocks(foot[i], boxes[i].z0, boxes[i].z1)) return false;
        for (const MassingBox& o : in.occluders)
            if (blocks(blockFootprint(o), o.z0, o.z1)) return false;
        if (in.ground) {
            const double len = std::hypot(q[0] - p[0], q[1] - p[1]);
            const int n = static_cast<int>(len / 2.0);
            for (int k = 1; k < n; ++k) {
                const double f = static_cast<double>(k) / n;
                if (groundAt(p[0] + f * (q[0] - p[0]), p[1] + f * (q[1] - p[1])) > p[2] + f * (q[2] - p[2]) + 0.05) return false;
            }
        }
        return true;
    };
    for (const MassingStreet* s : here) {
        if (blockStreetRole(*s) != BlockRole::Main) continue;
        samplesOf(*s, 10.0, tmp);
        bool seen = false;
        for (const MassingVec& v : tmp) {
            if (seen) break;
            if (!R20.contains(v.x, v.y)) continue;
            const double eye[3] = {v.x, v.y, groundAt(v.x, v.y) + kBlockEyeZ};
            for (const BlockSightTarget& t : in.sights) {
                const double q[3] = {t.x, t.y, t.z};
                ++out.sightRays;
                if (clear(eye, q)) {
                    seen = true;
                    break;
                }
            }
            if (seen || !in.sea) continue;
            // the sea: the first water (ground under 0) due east of the sea line
            for (double x = std::max(in.seaX, v.x); x <= in.seaX + 400.0; x += 5.0)
                if (groundAt(x, v.y) < -0.2) {
                    const double q[3] = {x + 10.0, v.y, 0.2};
                    ++out.sightRays;
                    seen = clear(eye, q);
                    break;
                }
        }
        if (!seen) {
            ++out.sight;
            issue(out, "sight", s->id, s->points[0].x, s->points[0].y, 0.0);
        }
    }
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
    // walls kBlockWallClose m or higher (a yard's back wall) close a street's side for the paving rule
    std::vector<std::size_t> walls;
    for (std::size_t i = 0; i < boxes.size(); ++i)
        if (boxes[i].kind == "wall" && boxes[i].z1 - boxes[i].z0 >= kBlockWallClose && blockFootprint(boxes[i]).overlaps(wide, 0.0))
            walls.push_back(i);
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
            if (isRoad(s))
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
            const double limit = road ? kBlockOpenRoad : kBlockOpenLane;
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
                        bool wall = false;
                        for (const std::size_t i : walls) wall = wall || inBox(boxes[i], px, py, 0.0);
                        if (wall || onBuilding(px, py, 0.0)) {
                            d[side] = q;
                            break;
                        }
                    }
                }
                if (d[0] < 0.0 || d[1] < 0.0) continue;
                const double span = std::max(0.0, d[0] + d[1] - s.width); // open paving beyond the footway edges
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
    if (in.routes) routeRules(in, boxes, out);
    if (!std::isfinite(out.minDepth)) out.minDepth = 0.0;
    return out.ok();
}

std::string blockCheckText(const BlockCheckReport& r, std::size_t listMax) {
    std::string t;
    t += "buildings checked " + std::to_string(r.buildings) + ", frontage probes " + std::to_string(r.probes) +
         ", paving stations " + std::to_string(r.stations) + "\n";
    t += "lone " + std::to_string(r.lone) + ", depth " + std::to_string(r.depth) + " (min " + num(r.minDepth) +
         " m), frontage " + std::to_string(r.frontage) + ", paving " + std::to_string(r.paving) + " (open max road " +
         num(r.maxPaveRoad) + " m, lane " + num(r.maxPaveLane) + " m), sink " + std::to_string(r.sink) + " (max " +
         num(r.maxSink) + " m)\n";
    if (r.routes)
        t += "routes: mains " + std::to_string(r.mains) + " (links " + std::to_string(r.links) + "), lanes " + std::to_string(r.lanes) + ", alleys " +
             std::to_string(r.alleys) + "; main " + std::to_string(r.mainShort) + ", chain " + std::to_string(r.chain) +
             ", deadend " + std::to_string(r.deadEnd) + ", walk " + std::to_string(r.walk) + " (" +
             std::to_string(r.walkOver) + " of " + std::to_string(r.walkSamples) + " samples over, max " + num(r.maxWalk) +
             " m), sight " + std::to_string(r.sight) + " (" + std::to_string(r.sightRays) + " rays), section " +
             std::to_string(r.section) + "\n";
    std::size_t n = 0;
    for (const BlockCheckIssue& i : r.issues) {
        if (++n > listMax) break;
        t += "  " + i.rule + " " + i.id + " at " + num(i.x) + "," + num(i.y) + " value " + num(i.value) + "\n";
    }
    t += r.ok() ? "BLOCKCHECK OK\n" : "BLOCKCHECK FAIL\n";
    return t;
}

} // namespace ftcore
