#include "ftcore/RoadGraph.h"

#include "CsvRead.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace ftcore {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    std::string s(buf);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}
std::string ptsText(const RoadPoint* p, std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        if (i) s += ';';
        s += num(p[i].x) + ":" + num(p[i].y);
    }
    return s;
}
bool readPts(std::string_view s, std::vector<RoadPoint>& out) {
    out.clear();
    std::size_t p = 0;
    while (p <= s.size()) {
        std::size_t q = s.find(';', p);
        if (q == std::string_view::npos) q = s.size();
        const std::string_view item = s.substr(p, q - p);
        const std::size_t c = item.find(':');
        RoadPoint pt;
        if (c == std::string_view::npos || !detail::parseNumber(item.substr(0, c), pt.x) ||
            !detail::parseNumber(item.substr(c + 1), pt.y))
            return false;
        out.push_back(pt);
        p = q + 1;
    }
    return !out.empty();
}

RoadPoint sub(RoadPoint a, RoadPoint b) { return {a.x - b.x, a.y - b.y}; }
double len(RoadPoint a) { return std::hypot(a.x, a.y); }
RoadPoint unit(RoadPoint a) {
    const double l = len(a);
    return l > 0.0 ? RoadPoint{a.x / l, a.y / l} : RoadPoint{1.0, 0.0};
}
RoadPoint left(RoadPoint d) { return {-d.y, d.x}; }
double cross(RoadPoint a, RoadPoint b) { return a.x * b.y - a.y * b.x; }

double carriageOf(const MassingStreet& s, double share) {
    return s.pavement > 0.0 ? massingCarriageway(s) : s.width * (1.0 - 2.0 * share);
}

// A polyline offset by `t` to its left, mitred.
std::vector<RoadPoint> offsetLine(const std::vector<RoadPoint>& p, double t) {
    std::vector<RoadPoint> out;
    const std::size_t n = p.size();
    for (std::size_t i = 0; i < n; ++i) {
        RoadPoint nrm;
        if (i == 0) {
            nrm = left(unit(sub(p[1], p[0])));
        } else if (i + 1 == n) {
            nrm = left(unit(sub(p[i], p[i - 1])));
        } else {
            const RoadPoint a = left(unit(sub(p[i], p[i - 1]))), b = left(unit(sub(p[i + 1], p[i])));
            RoadPoint m = unit({a.x + b.x, a.y + b.y});
            const double c = std::max(0.25, m.x * a.x + m.y * a.y);
            nrm = {m.x / c, m.y / c};
        }
        out.push_back({p[i].x + nrm.x * t, p[i].y + nrm.y * t});
    }
    return out;
}

// The net as nodes and straight edges, split at every junction.
struct Net {
    std::vector<RoadPoint> nodes;
    struct Edge {
        std::size_t a, b;
        double len;
        std::size_t street;
    };
    std::vector<Edge> edges;
    std::vector<std::vector<std::size_t>> streetsAt; // per node: the streets through it
    double snap = kRoadGraphSnap;

    std::size_t node(RoadPoint p) {
        for (std::size_t i = 0; i < nodes.size(); ++i)
            if (len(sub(nodes[i], p)) <= snap) return i;
        nodes.push_back(p);
        streetsAt.emplace_back();
        return nodes.size() - 1;
    }
};

// Where segment p0-p1 crosses q0-q1 strictly inside both (t along p).
bool crossAt(RoadPoint p0, RoadPoint p1, RoadPoint q0, RoadPoint q1, double& t) {
    const RoadPoint r = sub(p1, p0), s = sub(q1, q0);
    const double d = cross(r, s);
    if (std::fabs(d) < 1e-12) return false;
    const RoadPoint qp = sub(q0, p0);
    t = cross(qp, s) / d;
    const double u = cross(qp, r) / d;
    return t > 1e-6 && t < 1.0 - 1e-6 && u > 1e-6 && u < 1.0 - 1e-6;
}

std::vector<RoadPoint> pointsOf(const MassingStreet& s) {
    std::vector<RoadPoint> p;
    for (const MassingVec& v : s.points) p.push_back({v.x, v.y});
    return p;
}

Net buildNet(const std::vector<MassingStreet>& streets, double snap) {
    Net net;
    net.snap = snap;
    std::vector<std::vector<RoadPoint>> lines;
    for (const MassingStreet& s : streets) lines.push_back(pointsOf(s));
    for (std::size_t si = 0; si < lines.size(); ++si) {
        const std::vector<RoadPoint>& L = lines[si];
        std::size_t prev = static_cast<std::size_t>(-1);
        for (std::size_t k = 0; k + 1 < L.size(); ++k) {
            const RoadPoint a = L[k], b = L[k + 1];
            const double segLen = len(sub(b, a));
            std::vector<double> ts = {0.0};
            for (std::size_t oj = 0; oj < lines.size(); ++oj) {
                const std::vector<RoadPoint>& O = lines[oj];
                for (std::size_t m = 0; m + 1 < O.size(); ++m) {
                    if (oj == si && (m + 1 >= k && m <= k + 1)) continue;
                    double t = 0.0;
                    if (crossAt(a, b, O[m], O[m + 1], t)) ts.push_back(t);
                }
                if (oj == si || segLen <= 0.0) continue;
                // Another street's end near this segment's inside: a T.
                for (const RoadPoint e : {O.front(), O.back()}) {
                    const RoadPoint d = sub(b, a);
                    const double t = ((e.x - a.x) * d.x + (e.y - a.y) * d.y) / (segLen * segLen);
                    if (t <= 0.0 || t >= 1.0) continue;
                    const RoadPoint pr{a.x + d.x * t, a.y + d.y * t};
                    if (len(sub(pr, e)) <= snap) ts.push_back(t);
                }
            }
            ts.push_back(1.0);
            std::sort(ts.begin(), ts.end());
            for (const double t : ts) {
                const std::size_t n = net.node({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t});
                std::vector<std::size_t>& at = net.streetsAt[n];
                if (std::find(at.begin(), at.end(), si) == at.end()) at.push_back(si);
                if (prev != static_cast<std::size_t>(-1) && prev != n) {
                    bool dup = false;
                    for (const Net::Edge& e : net.edges)
                        if ((e.a == prev && e.b == n) || (e.a == n && e.b == prev)) dup = true;
                    if (!dup) net.edges.push_back({prev, n, len(sub(net.nodes[n], net.nodes[prev])), si});
                }
                prev = n;
            }
        }
    }
    return net;
}

} // namespace

const char* roadGraphHeader() {
    return "# Road graph (ftcore/RoadGraph.h). Plan frame, metres; left-hand traffic. One row per item, by type:\n"
           "#   lane      id, street, dir (+1 along the street's points, -1 against), width_m (the lane), speed_kmh,\n"
           "#             one_way (1/0), pts (its centre line in the way of travel, \"x:y;x:y;...\"), refs (empty)\n"
           "#   junction  id, pts (its centre, \"x:y\"), refs (the streets meeting there, '+'-joined), phases\n"
           "#             (\"A:ST-1+ST-2;B:ST-3\": two-phase, each phase the streets it lets go; empty: unsignalised)\n"
           "#   crossing  id, street (the one it crosses), pts (its four corners, counter-clockwise), refs (its junction)\n"
           "#   bay       id, street, dir (the way a car parked in it faces, as a lane's), width_m (across the street),\n"
           "#             pts (its four corners)\n"
           "# Columns a type does not use are empty.\n";
}

std::string roadGraphWrite(const RoadGraph& g) {
    std::string s = roadGraphHeader();
    s += "type,id,street,dir,width_m,speed_kmh,one_way,pts,refs,phases\n";
    for (const RoadGraphLane& l : g.lanes)
        s += "lane," + l.id + "," + l.street + "," + (l.dir > 0 ? "1" : "-1") + "," + num(l.width) + "," +
             num(l.speedKmh) + "," + (l.oneWay ? "1" : "0") + "," + ptsText(l.pts.data(), l.pts.size()) + ",,\n";
    for (const RoadGraphJunction& j : g.junctions) {
        std::string refs;
        for (std::size_t i = 0; i < j.streets.size(); ++i) refs += (i ? "+" : "") + j.streets[i];
        s += "junction," + j.id + ",,,,,," + ptsText(&j.at, 1) + "," + refs + "," + j.phases + "\n";
    }
    for (const RoadGraphCrossing& c : g.crossings)
        s += "crossing," + c.id + "," + c.street + ",,,,," + ptsText(c.pts.data(), 4) + "," + c.junction + ",\n";
    for (const RoadGraphBay& b : g.bays)
        s += "bay," + b.id + "," + b.street + "," + (b.dir > 0 ? "1" : "-1") + "," + num(b.width) + ",,," +
             ptsText(b.pts.data(), 4) + ",,\n";
    return s;
}

bool importRoadGraph(std::string_view csv, RoadGraph& out, std::string* error) {
    using namespace detail;
    out = RoadGraph{};
    std::vector<Record> recs;
    if (!splitCsv(csv, recs, error)) return false;
    bool head = false;
    for (const Record& rec : recs) {
        if (isComment(rec)) continue;
        if (!head) {
            if (rec.fields.size() != 10 || trim(rec.fields[0]) != "type")
                return fail(error, rec.line, "the header row is type,id,street,dir,width_m,speed_kmh,one_way,pts,refs,phases");
            head = true;
            continue;
        }
        if (rec.fields.size() != 10) return fail(error, rec.line, "a row has 10 columns");
        auto f = [&](std::size_t i) { return trim(rec.fields[i]); };
        const std::string_view type = f(0);
        const std::string id(f(1));
        // Any case: the build's bay and zebra ids carry lower-case kinds ("ST-1-E1-bay-3").
        if (!validKind(id)) return fail(error, rec.line, "a row needs an id");
        std::vector<RoadPoint> pts;
        if (!readPts(f(7), pts)) return fail(error, rec.line, id + ": pts must be \"x:y;...\"");
        auto dirOf = [&](int& d) {
            if (f(3) == "1") d = 1;
            else if (f(3) == "-1") d = -1;
            else return false;
            return true;
        };
        if (type == "lane") {
            RoadGraphLane l;
            l.id = id;
            l.street = std::string(f(2));
            if (!dirOf(l.dir) || !parseNumber(f(4), l.width) || !parseNumber(f(5), l.speedKmh) || pts.size() < 2 ||
                l.width <= 0.0)
                return fail(error, rec.line, id + ": a lane needs dir, width, speed and two points");
            l.oneWay = f(6) == "1";
            l.pts = std::move(pts);
            out.lanes.push_back(std::move(l));
        } else if (type == "junction") {
            RoadGraphJunction j;
            j.id = id;
            if (pts.size() != 1) return fail(error, rec.line, id + ": a junction has one point");
            j.at = pts[0];
            const std::string_view refs = f(8);
            std::size_t p = 0;
            while (p < refs.size()) {
                std::size_t q = refs.find('+', p);
                if (q == std::string_view::npos) q = refs.size();
                j.streets.emplace_back(refs.substr(p, q - p));
                p = q + 1;
            }
            if (j.streets.empty()) return fail(error, rec.line, id + ": a junction names its streets");
            j.phases = std::string(f(9));
            out.junctions.push_back(std::move(j));
        } else if (type == "crossing" || type == "bay") {
            if (pts.size() != 4) return fail(error, rec.line, id + ": four corners");
            if (type == "crossing") {
                RoadGraphCrossing c;
                c.id = id;
                c.street = std::string(f(2));
                c.junction = std::string(f(8));
                std::copy(pts.begin(), pts.end(), c.pts.begin());
                out.crossings.push_back(std::move(c));
            } else {
                RoadGraphBay b;
                b.id = id;
                b.street = std::string(f(2));
                if (!dirOf(b.dir) || !parseNumber(f(4), b.width)) return fail(error, rec.line, id + ": a bay needs dir and width");
                std::copy(pts.begin(), pts.end(), b.pts.begin());
                out.bays.push_back(std::move(b));
            }
        } else {
            return fail(error, rec.line, id + ": type is lane, junction, crossing or bay");
        }
    }
    if (!head) return fail(error, 1, "the table has no header row");
    return true;
}

void roadGraphStreetLanes(const MassingStreet& street, double speedKmh, bool oneWay, double pavementShare,
                          RoadGraph& out) {
    const std::vector<RoadPoint> p = pointsOf(street);
    if (p.size() < 2) return;
    const double cw = carriageOf(street, pavementShare);
    const int ways = oneWay ? 1 : 2;
    const int perWay = std::max(1, static_cast<int>(std::floor(cw / ways / kRoadLaneWidth + 1e-9)));
    const double lw = std::min(kRoadLaneWidth, cw / ways / perWay);
    std::vector<RoadPoint> back(p.rbegin(), p.rend());
    for (int way = 0; way < ways; ++way) {
        const int dir = way == 0 ? 1 : -1;
        const std::vector<RoadPoint>& base = dir > 0 ? p : back;
        for (int k = 0; k < perWay; ++k) {
            // Left-hand traffic: a two-way street's lanes lie left of each way's centre line, the kerb lane outermost.
            const double t = oneWay ? (-lw * perWay * 0.5 + lw * (k + 0.5)) : lw * (k + 0.5);
            RoadGraphLane l;
            char buf[16];
            std::snprintf(buf, sizeof buf, "-%c%d", dir > 0 ? 'F' : 'B', k + 1);
            l.id = street.id + buf;
            l.street = street.id;
            l.dir = dir;
            l.width = lw;
            l.speedKmh = speedKmh;
            l.oneWay = oneWay;
            l.pts = offsetLine(base, t);
            out.lanes.push_back(std::move(l));
        }
    }
}

void roadGraphJunctions(const std::vector<MassingStreet>& streets, double snap, double pavementShare, RoadGraph& out) {
    const Net net = buildNet(streets, snap);
    int count = 0;
    for (std::size_t n = 0; n < net.nodes.size(); ++n) {
        const std::vector<std::size_t>& at = net.streetsAt[n];
        if (at.size() < 2) continue;
        const RoadPoint c = net.nodes[n];
        // Its arms: the directions out along each edge at the node.
        struct Arm {
            RoadPoint d;
            std::size_t street;
        };
        std::vector<Arm> arms;
        for (const Net::Edge& e : net.edges) {
            if (e.a != n && e.b != n) continue;
            const std::size_t o = e.a == n ? e.b : e.a;
            arms.push_back({unit(sub(net.nodes[o], c)), e.street});
        }
        RoadGraphJunction j;
        char buf[24];
        std::snprintf(buf, sizeof buf, "JN-%04d", ++count);
        j.id = buf;
        j.at = c;
        for (const std::size_t s : at) j.streets.push_back(streets[s].id);
        if (arms.size() >= 3) {
            // Phase A: the arms on the first arm's axis (within 45 degrees, either way); B: the rest.
            const RoadPoint ax = arms[0].d;
            std::string a, b;
            std::vector<std::size_t> inA, inB;
            for (const Arm& arm : arms) {
                const double cosv = std::fabs(arm.d.x * ax.x + arm.d.y * ax.y);
                std::vector<std::size_t>& to = cosv >= std::cos(kPi / 4.0) ? inA : inB;
                if (std::find(to.begin(), to.end(), arm.street) == to.end()) to.push_back(arm.street);
            }
            for (const std::size_t s : inA) a += (a.empty() ? "" : "+") + streets[s].id;
            for (const std::size_t s : inB) b += (b.empty() ? "" : "+") + streets[s].id;
            if (!a.empty() && !b.empty()) j.phases = "A:" + a + ";B:" + b;
            // A zebra across each arm, past the widest other street's kerb.
            for (std::size_t ai = 0; ai < arms.size(); ++ai) {
                double off = 0.0;
                for (const Arm& other : arms)
                    if (other.street != arms[ai].street)
                        off = std::max(off, carriageOf(streets[other.street], pavementShare) * 0.5);
                const double half = carriageOf(streets[arms[ai].street], pavementShare) * 0.5;
                const RoadPoint d = arms[ai].d, l = left(d);
                const double s0 = off + kRoadZebraOff, s1 = s0 + kRoadZebraWide;
                RoadGraphCrossing z;
                std::snprintf(buf, sizeof buf, "%s-Z%zu", j.id.c_str(), ai + 1);
                z.id = buf;
                z.street = streets[arms[ai].street].id;
                z.junction = j.id;
                z.pts = {RoadPoint{c.x + d.x * s0 - l.x * half, c.y + d.y * s0 - l.y * half},
                         RoadPoint{c.x + d.x * s1 - l.x * half, c.y + d.y * s1 - l.y * half},
                         RoadPoint{c.x + d.x * s1 + l.x * half, c.y + d.y * s1 + l.y * half},
                         RoadPoint{c.x + d.x * s0 + l.x * half, c.y + d.y * s0 + l.y * half}};
                out.crossings.push_back(z);
            }
        }
        out.junctions.push_back(std::move(j));
    }
}

void roadGraphFromRoad(const RoadSpec& road, const RoadBuild& build, RoadGraph& out) {
    MassingStreet s;
    s.id = road.id;
    s.width = road.width;
    s.pavement = road.pavement;
    s.points = {{road.x0, road.y0, 0.0}, {road.x1, road.y1, 0.0}};
    const std::size_t first = out.lanes.size();
    roadGraphStreetLanes(s, road.speedKmh, road.oneWay, 0.0, out);
    for (std::size_t i = first; i < out.lanes.size(); ++i) out.lanes[i].width = road.lane;
    const RoadPoint d = unit({road.x1 - road.x0, road.y1 - road.y0});
    for (const RoadMouth& m : road.mouths) {
        if (m.stub <= 0.0) continue;
        // The stub's two lanes, both ways.
        const double sg = m.left ? -1.0 : 1.0; // the stub's side
        const RoadPoint c{road.x0 + d.x * m.at, road.y0 + d.y * m.at};
        const RoadPoint n = left(d);
        const double hc = (road.width - 2.0 * road.pavement) * 0.5;
        const RoadPoint a{c.x + n.x * sg * hc, c.y + n.y * sg * hc};
        const RoadPoint b{a.x + n.x * sg * m.stub, a.y + n.y * sg * m.stub};
        MassingStreet st;
        st.id = road.id + "-S1";
        st.width = 2.0 * road.lane;
        st.points = {{a.x, a.y, 0.0}, {b.x, b.y, 0.0}};
        const std::size_t f2 = out.lanes.size();
        roadGraphStreetLanes(st, road.speedKmh, false, 0.0, out);
        for (std::size_t i = f2; i < out.lanes.size(); ++i) out.lanes[i].width = road.lane;
    }
    for (const RoadJunction& rj : build.junctions) {
        RoadGraphJunction j;
        j.id = rj.id;
        j.at = {rj.x, rj.y};
        j.streets = {rj.road, rj.lane, rj.road + "-S1"};
        j.phases = "A:" + rj.road + ";B:" + rj.lane + "+" + rj.road + "-S1";
        out.junctions.push_back(std::move(j));
    }
    for (const RoadQuad& q : build.crossings) {
        RoadGraphCrossing c;
        c.id = q.id;
        c.street = road.id;
        c.pts = q.pts;
        // The stub's zebra crosses the stub; the others the road. Both serve the junction.
        if (!build.junctions.empty()) c.junction = build.junctions.front().id;
        const RoadPoint mid{(q.pts[0].x + q.pts[2].x) * 0.5, (q.pts[0].y + q.pts[2].y) * 0.5};
        const RoadPoint rel{mid.x - road.x0, mid.y - road.y0};
        if (std::fabs(cross(d, rel)) > (road.width - 2.0 * road.pavement) * 0.5) c.street = road.id + "-S1";
        out.crossings.push_back(c);
    }
    for (const RoadBay& bay : build.bays) {
        RoadGraphBay b;
        b.id = bay.id;
        b.street = road.id;
        b.dir = bay.left ? 1 : -1;
        b.width = bay.foot.hy * 2.0;
        const double r = bay.foot.rot * kPi / 180.0;
        const RoadPoint u{std::cos(r), std::sin(r)}, w{-std::sin(r), std::cos(r)};
        const double hx = bay.foot.hx, hy = bay.foot.hy;
        b.pts = {RoadPoint{bay.foot.x - u.x * hx - w.x * hy, bay.foot.y - u.y * hx - w.y * hy},
                 RoadPoint{bay.foot.x + u.x * hx - w.x * hy, bay.foot.y + u.y * hx - w.y * hy},
                 RoadPoint{bay.foot.x + u.x * hx + w.x * hy, bay.foot.y + u.y * hx + w.y * hy},
                 RoadPoint{bay.foot.x - u.x * hx + w.x * hy, bay.foot.y - u.y * hx + w.y * hy}};
        out.bays.push_back(b);
    }
}

RoadNetTopo roadGraphNet(const std::vector<MassingStreet>& streets, double snap) {
    const Net net = buildNet(streets, snap);
    RoadNetTopo t;
    t.nodes = net.nodes;
    t.streetsAt = net.streetsAt;
    for (const Net::Edge& e : net.edges) t.edges.push_back({e.a, e.b, e.len, e.street});
    return t;
}

void roadGraphFromNet(const std::vector<RoadNetStreet>& streets, const RoadBuild& build, RoadGraph& out) {
    std::vector<MassingStreet> ms;
    for (const RoadNetStreet& s : streets) {
        ms.push_back(roadNetStreet(s));
        roadGraphStreetLanes(ms.back(), s.speedKmh, s.oneWay, 0.0, out);
    }
    // The junctions and their phases; the zebras are the build's.
    RoadGraph jg;
    roadGraphJunctions(ms, kRoadGraphSnap, 0.0, jg);
    for (RoadGraphJunction& j : jg.junctions) {
        // A scramble (as roadNetBuild finds it): an all-walk phase after the vehicle phases.
        bool scramble = false;
        for (const RoadNetStreet& s : streets)
            for (const RoadPoint& q : s.scramble)
                scramble = scramble || std::hypot(q.x - j.at.x, q.y - j.at.y) <= kRoadGraphSnap;
        if (scramble && !j.phases.empty()) j.phases += ";W:all-walk";
        out.junctions.push_back(std::move(j));
    }
    for (const RoadQuad& q : build.crossings) {
        RoadGraphCrossing c;
        c.id = q.id;
        c.street = q.street;
        c.junction = q.junction;
        c.pts = q.pts;
        out.crossings.push_back(c);
    }
    for (const RoadBay& bay : build.bays) {
        RoadGraphBay b;
        b.id = bay.id;
        b.street = bay.street;
        b.dir = bay.left ? 1 : -1;
        b.width = bay.foot.hy * 2.0;
        const double r = bay.foot.rot * kPi / 180.0;
        const RoadPoint u{std::cos(r), std::sin(r)}, w{-std::sin(r), std::cos(r)};
        const double hx = bay.foot.hx, hy = bay.foot.hy;
        b.pts = {RoadPoint{bay.foot.x - u.x * hx - w.x * hy, bay.foot.y - u.y * hx - w.y * hy},
                 RoadPoint{bay.foot.x + u.x * hx - w.x * hy, bay.foot.y + u.y * hx - w.y * hy},
                 RoadPoint{bay.foot.x + u.x * hx + w.x * hy, bay.foot.y + u.y * hx + w.y * hy},
                 RoadPoint{bay.foot.x - u.x * hx + w.x * hy, bay.foot.y - u.y * hx + w.y * hy}};
        out.bays.push_back(b);
    }
}

RoadLoop roadGraphLoop(const std::vector<MassingStreet>& streets, double snap) {
    RoadLoop loop;
    const Net net = buildNet(streets, snap);
    const std::size_t N = net.nodes.size(), E = net.edges.size();
    std::vector<std::vector<std::size_t>> adj(N);
    for (std::size_t e = 0; e < E; ++e) {
        adj[net.edges[e].a].push_back(e);
        adj[net.edges[e].b].push_back(e);
    }
    // Bridges (Tarjan, iterative).
    std::vector<int> disc(N, -1), low(N, 0);
    std::vector<bool> bridge(E, false);
    int timer = 0;
    for (std::size_t root = 0; root < N; ++root) {
        if (disc[root] >= 0) continue;
        struct Frame {
            std::size_t v, parentEdge, next;
        };
        std::vector<Frame> st = {{root, static_cast<std::size_t>(-1), 0}};
        disc[root] = low[root] = timer++;
        while (!st.empty()) {
            Frame& fr = st.back();
            if (fr.next < adj[fr.v].size()) {
                const std::size_t e = adj[fr.v][fr.next++];
                if (e == fr.parentEdge) continue;
                const std::size_t o = net.edges[e].a == fr.v ? net.edges[e].b : net.edges[e].a;
                if (disc[o] < 0) {
                    disc[o] = low[o] = timer++;
                    st.push_back({o, e, 0});
                } else {
                    low[fr.v] = std::min(low[fr.v], disc[o]);
                }
            } else {
                const Frame done = fr;
                st.pop_back();
                if (!st.empty()) {
                    Frame& up = st.back();
                    low[up.v] = std::min(low[up.v], low[done.v]);
                    if (low[done.v] > disc[up.v]) bridge[done.parentEdge] = true;
                }
            }
        }
    }
    for (std::size_t e = 0; e < E; ++e)
        if (bridge[e]) ++loop.bridges;
    // The pieces left, by length; keep the longest.
    std::vector<int> comp(N, -1);
    std::vector<double> compLen;
    for (std::size_t s = 0; s < N; ++s) {
        if (comp[s] >= 0) continue;
        const int c = static_cast<int>(compLen.size());
        compLen.push_back(0.0);
        std::vector<std::size_t> todo = {s};
        comp[s] = c;
        while (!todo.empty()) {
            const std::size_t v = todo.back();
            todo.pop_back();
            for (const std::size_t e : adj[v]) {
                if (bridge[e]) continue;
                const std::size_t o = net.edges[e].a == v ? net.edges[e].b : net.edges[e].a;
                if (comp[o] < 0) {
                    comp[o] = c;
                    todo.push_back(o);
                }
            }
        }
    }
    for (std::size_t e = 0; e < E; ++e)
        if (!bridge[e]) compLen[static_cast<std::size_t>(comp[net.edges[e].a])] += net.edges[e].len;
    if (compLen.empty()) return loop;
    const int best = static_cast<int>(std::max_element(compLen.begin(), compLen.end()) - compLen.begin());
    loop.netLength = compLen[static_cast<std::size_t>(best)];
    if (loop.netLength <= 0.0) return loop;
    // Its half-edges sorted by angle at each node; trace every face, keep the largest by area.
    std::vector<std::vector<std::size_t>> out(N); // per node: edges, sorted counter-clockwise
    for (std::size_t v = 0; v < N; ++v) {
        if (comp[v] != best) continue;
        ++loop.nodes;
        for (const std::size_t e : adj[v])
            if (!bridge[e]) out[v].push_back(e);
        auto angle = [&](std::size_t e) {
            const std::size_t o = net.edges[e].a == v ? net.edges[e].b : net.edges[e].a;
            return std::atan2(net.nodes[o].y - net.nodes[v].y, net.nodes[o].x - net.nodes[v].x);
        };
        std::sort(out[v].begin(), out[v].end(), [&](std::size_t x, std::size_t y) { return angle(x) < angle(y); });
    }
    std::vector<std::array<bool, 2>> used(E, {false, false}); // [0] a->b, [1] b->a
    double bestArea = 0.0;
    for (std::size_t e0 = 0; e0 < E; ++e0) {
        if (bridge[e0] || comp[net.edges[e0].a] != best) continue;
        ++loop.edges;
        for (int side = 0; side < 2; ++side) {
            if (used[e0][static_cast<std::size_t>(side)]) continue;
            std::vector<RoadPoint> ring;
            double area = 0.0, perim = 0.0;
            std::size_t e = e0;
            int sd = side;
            for (std::size_t guard = 0; guard < 4 * E + 4; ++guard) {
                if (used[e][static_cast<std::size_t>(sd)]) break;
                used[e][static_cast<std::size_t>(sd)] = true;
                const std::size_t u = sd == 0 ? net.edges[e].a : net.edges[e].b;
                const std::size_t v = sd == 0 ? net.edges[e].b : net.edges[e].a;
                ring.push_back(net.nodes[u]);
                area += cross(net.nodes[u], net.nodes[v]);
                perim += net.edges[e].len;
                // At v: the edge just clockwise of the way back (the face on the right... any consistent turn).
                const std::vector<std::size_t>& ring2 = out[v];
                std::size_t idx = 0;
                while (idx < ring2.size() && ring2[idx] != e) ++idx;
                const std::size_t nextE = ring2[(idx + ring2.size() - 1) % ring2.size()];
                e = nextE;
                sd = net.edges[e].a == v ? 0 : 1;
            }
            if (std::fabs(area) * 0.5 > bestArea) {
                bestArea = std::fabs(area) * 0.5;
                loop.length = perim;
                loop.ring = std::move(ring);
            }
        }
    }
    return loop;
}

} // namespace ftcore
