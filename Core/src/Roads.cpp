#include "ftcore/Roads.h"
#include "ftcore/RoadGraph.h"

#include "CsvRead.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace ftcore {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMarkLift = 0.006;  // m: a marking's top over its surface
constexpr double kMarkDeep = 0.012;  // m: a marking's thickness
constexpr double kStubPavement = 2.0; // m: a stub arm's pavements
constexpr double kShelterDeep = 1.4, kShelterLong = 4.2, kShelterHigh = 2.4;
constexpr double kPoleHigh = 10.0;   // m: a utility pole over the pavement
constexpr double kSignalHigh = 5.6;  // m: a signal pole
constexpr double kPedHigh = 3.2;     // m: a pedestrian signal's pole
constexpr int kCornerSteps = 12;

double deg(double rad) { return rad * 180.0 / kPi; }
double rad(double d) { return d * kPi / 180.0; }

std::uint32_t mix(std::uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352dU;
    h ^= h >> 15;
    h *= 0x846ca68bU;
    h ^= h >> 16;
    return h;
}
std::uint32_t hashText(std::uint32_t seed, const std::string& s) {
    std::uint32_t h = 2166136261U ^ seed;
    for (const char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619U;
    }
    return mix(h);
}

// A road's own axes: s along it from its first point, t to its left.
struct Frame {
    double ox = 0.0, oy = 0.0, dx = 1.0, dy = 0.0, rot = 0.0;
    double grade = 0.0; // a network piece's rise per m along s: its boxes' tops rise with it from s = 0
    RoadPoint at(double s, double t) const { return {ox + s * dx - t * dy, oy + s * dy + t * dx}; }
    // A plan direction from one in (s, t).
    RoadPoint dir(double ds, double dt) const { return {ds * dx - dt * dy, ds * dy + dt * dx}; }
};
Frame frameOf(double x0, double y0, double x1, double y1) {
    Frame f;
    const double len = std::hypot(x1 - x0, y1 - y0);
    f.ox = x0;
    f.oy = y0;
    f.dx = len > 0.0 ? (x1 - x0) / len : 1.0;
    f.dy = len > 0.0 ? (y1 - y0) / len : 0.0;
    f.rot = deg(std::atan2(f.dy, f.dx));
    return f;
}
double angleOf(RoadPoint d) { return deg(std::atan2(d.y, d.x)); }

MassingFoot footOf(const MassingBox& b) {
    MassingFoot f;
    f.x = b.cx;
    f.y = b.cy;
    f.hx = b.sx * 0.5;
    f.hy = b.sy * 0.5;
    f.rot = b.rot;
    return f;
}

// The builder of one road.
struct Ctx {
    const RoadSpec& r;
    RoadBuild& out;
    Frame f;
    double len = 0.0, hc = 0.0, w2 = 0.0, gutter = 0.0, crown = 0.0, pave = 0.0;
    double deep = 0.0; // m added to every slab's depth (a network piece over falling ground)
    int n = 0;
    bool inst = false; // what is added now is laid as instances (a network's poles, wires and signs)

    Ctx(const RoadSpec& road, RoadBuild& build) : r(road), out(build) {}

    std::string id(const char* part) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "-%s-%d", part, ++n);
        return r.id + buf;
    }
    // A box over s0..s1 by t0..t1 of a frame, its top `top` at its centre, rising riseU and riseW.
    MassingBox box(const Frame& fr, double s0, double s1, double t0, double t1, double top, double depth, double riseU,
                   double riseW, const char* kind) {
        MassingBox b;
        b.id = id(kind);
        b.row = r.id;
        b.kind = kind;
        b.source = MassingSource::Invented;
        const RoadPoint c = fr.at((s0 + s1) * 0.5, (t0 + t1) * 0.5);
        b.cx = c.x;
        b.cy = c.y;
        b.sx = std::fabs(s1 - s0);
        b.sy = std::fabs(t1 - t0);
        b.rot = fr.rot;
        top += fr.grade * (s0 + s1) * 0.5;
        if (depth >= kRoadSlabDepth - 1e-9) depth += deep;
        b.z1 = top;
        b.z0 = top - depth;
        b.riseU = riseU + fr.grade;
        b.riseW = riseW;
        return b;
    }
    RoadSolid& add(const MassingBox& b, RoadTint tint, bool collide, RoadShape shape = RoadShape::Cube,
                   RoadAxis axis = RoadAxis::Up, const std::string& tag = {}) {
        RoadSolid s;
        s.box = b;
        s.tint = tint;
        s.collide = collide;
        s.shape = shape;
        s.axis = axis;
        s.tag = tag;
        s.instanced = inst;
        out.solids.push_back(std::move(s));
        ++out.counts.solids;
        if (collide) ++out.counts.collide;
        return out.solids.back();
    }
    // A plan box centred on a point.
    MassingBox at(RoadPoint p, double rot, double sx, double sy, double z0, double z1, const char* kind) {
        MassingBox b;
        b.id = id(kind);
        b.row = r.id;
        b.kind = kind;
        b.source = MassingSource::Invented;
        b.cx = p.x;
        b.cy = p.y;
        b.sx = sx;
        b.sy = sy;
        b.rot = rot;
        b.z0 = z0;
        b.z1 = z1;
        return b;
    }
    // A marking on a carriageway half (the crossfall's tilt), or flat at `flatTop` when it is >= 0.
    void mark(const Frame& fr, double s0, double s1, double t0, double t1, RoadTint tint, double flatTop = -1.0) {
        double top = 0.0, riseW = 0.0;
        if (flatTop >= 0.0) {
            top = flatTop;
        } else {
            const double tm = (t0 + t1) * 0.5;
            top = crown - kRoadCrossfall * std::fabs(tm);
            riseW = tm >= 0.0 ? -kRoadCrossfall : kRoadCrossfall;
        }
        add(box(fr, s0, s1, std::min(t0, t1), std::max(t0, t1), top + kMarkLift, kMarkDeep, 0.0, riseW, "mark"), tint,
            false);
        ++out.counts.markings;
    }
    void furnitureFoot(RoadPoint p, double hx, double hy, double rot) {
        MassingFoot ft;
        ft.x = p.x;
        ft.y = p.y;
        ft.hx = hx;
        ft.hy = hy;
        ft.rot = rot;
        out.furniture.push_back(ft);
    }
    // A post (a cylinder) standing on `base` at a point, `high` tall.
    void post(RoadPoint p, double dia, double base, double high, RoadTint tint, const char* kind) {
        add(at(p, 0.0, dia, dia, base, base + high, kind), tint, true, RoadShape::Cylinder);
        furnitureFoot(p, dia * 0.5, dia * 0.5, 0.0);
    }
    // A road sign: a post and a face (a disc or a plate) at its top, facing `face` (a plan direction).
    void sign(RoadPoint p, RoadPoint face, double base, const char* what, RoadTint faceTint, bool disc) {
        post(p, 0.07, base, 2.5, RoadTint::Steel, "signpost");
        const double rot = angleOf(face);
        const RoadPoint c{p.x + face.x * 0.06, p.y + face.y * 0.06};
        const double z = base + 2.5 - 0.35;
        std::string tag = std::string("FTRoadSign_") + what;
        if (disc) {
            add(at(c, rot, 0.04, 0.6, z - 0.3, z + 0.3, "sign"), faceTint, false, RoadShape::Cylinder, RoadAxis::AlongU,
                tag);
        } else {
            add(at(c, rot, 0.04, 0.65, z - 0.33, z + 0.33, "sign"), faceTint, false, RoadShape::Cube, RoadAxis::Up, tag);
        }
        if (std::string(what) == "no_entry")
            add(at({c.x + face.x * 0.025, c.y + face.y * 0.025}, rot, 0.01, 0.42, z - 0.06, z + 0.06, "sign"),
                RoadTint::SignWhite, false, RoadShape::Cube, RoadAxis::Up, tag);
        ++out.counts.signs;
    }
};

enum class Span : std::uint8_t { Normal, Bay, Mouth, Corner, Ramp, Bus };
struct SideSpan {
    double s0 = 0.0, s1 = 0.0;
    Span type = Span::Normal;
};

// A signal head facing `face`, centred at c (z its centre), on a phase.
void signalHead(Ctx& x, RoadPoint c, RoadPoint face, double z, const std::string& junction, char phase) {
    const double rot = angleOf(face) + 90.0; // u across the face
    x.add(x.at(c, rot, 1.2, 0.35, z - 0.2, z + 0.2, "signal"), RoadTint::SignalBody, false);
    const RoadTint lens[3] = {RoadTint::LensGreen, RoadTint::LensAmber, RoadTint::LensRed};
    const char* name[3] = {"green", "amber", "red"};
    const RoadPoint u{std::cos(rad(rot)), std::sin(rad(rot))};
    for (int i = 0; i < 3; ++i) {
        const double off = (static_cast<double>(i) - 1.0) * 0.38;
        const RoadPoint p{c.x + u.x * off + face.x * 0.19, c.y + u.y * off + face.y * 0.19};
        x.add(x.at(p, rot, 0.28, 0.04, z - 0.14, z + 0.14, "lens"), lens[i], false, RoadShape::Cylinder,
              RoadAxis::AlongW, "FTSignal_" + junction + "_" + phase + "_" + name[i]);
    }
    ++x.out.counts.signals;
}

// A pedestrian signal: a pole at p and a head facing `face`.
void pedSignal(Ctx& x, RoadPoint p, RoadPoint face, double base, const std::string& junction, char phase) {
    x.post(p, 0.14, base, kPedHigh, RoadTint::Steel, "pedpole");
    const double rot = angleOf(face) + 90.0;
    const RoadPoint c{p.x + face.x * 0.2, p.y + face.y * 0.2};
    const double z = base + 2.65;
    x.add(x.at(c, rot, 0.4, 0.25, z - 0.4, z + 0.4, "pedsignal"), RoadTint::SignalBody, false);
    const RoadPoint l{c.x + face.x * 0.13, c.y + face.y * 0.13};
    x.add(x.at(l, rot, 0.3, 0.03, z + 0.05, z + 0.35, "lens"), RoadTint::LensRed, false, RoadShape::Cube, RoadAxis::Up,
          "FTPedSignal_" + junction + "_" + phase + "_red");
    x.add(x.at(l, rot, 0.3, 0.03, z - 0.35, z - 0.05, "lens"), RoadTint::LensGreen, false, RoadShape::Cube,
          RoadAxis::Up, "FTPedSignal_" + junction + "_" + phase + "_green");
    ++x.out.counts.signals;
}

// A vehicle signal: a pole at p, an arm out to `armTo` (when not p) and the head there facing `face`.
void vehicleSignal(Ctx& x, RoadPoint p, RoadPoint armTo, RoadPoint face, double base, const std::string& junction,
                   char phase) {
    x.post(p, 0.22, base, kSignalHigh, RoadTint::Steel, "signalpole");
    const double ax = armTo.x - p.x, ay = armTo.y - p.y;
    const double armLen = std::hypot(ax, ay);
    RoadPoint head{p.x + face.x * 0.35, p.y + face.y * 0.35};
    double z = base + kSignalHigh - 0.6;
    if (armLen > 0.5) {
        const RoadPoint mid{(p.x + armTo.x) * 0.5, (p.y + armTo.y) * 0.5};
        x.add(x.at(mid, angleOf({ax, ay}), armLen, 0.12, base + kSignalHigh - 0.35, base + kSignalHigh - 0.2, "arm"),
              RoadTint::Steel, false);
        head = armTo;
        z = base + kSignalHigh - 0.58;
    }
    signalHead(x, head, face, z, junction, phase);
}

// The pavement side of a frame's carriageway: spans along s (sorted, covering 0..len), the side `sigma` (+1 left).
void buildSide(Ctx& x, const Frame& fr, double sigma, double hc, double w2, double len, const std::vector<SideSpan>& spans,
               bool flatCarriage, bool rail) {
    const double g = x.gutter, pv = x.pave;
    auto tt = [&](double a) { return sigma * a; };
    auto lo = [&](double a, double b) { return std::min(tt(a), tt(b)); };
    auto hi = [&](double a, double b) { return std::max(tt(a), tt(b)); };
    for (const SideSpan& sp : spans) {
        const double s0 = sp.s0, s1 = sp.s1;
        if (s1 - s0 < 1e-6) continue;
        switch (sp.type) {
        case Span::Normal:
        case Span::Bus:
            x.add(x.box(fr, s0, s1, lo(hc, hc + kRoadKerbWide), hi(hc, hc + kRoadKerbWide), pv, kRoadSlabDepth + 0.15,
                        0.0, 0.0, "kerb"),
                  RoadTint::Kerb, true);
            x.add(x.box(fr, s0, s1, lo(hc + kRoadKerbWide, w2), hi(hc + kRoadKerbWide, w2), pv, kRoadSlabDepth, 0.0, 0.0,
                        "pavement"),
                  RoadTint::Pavement, true);
            break;
        case Span::Bay: {
            const double bk = hc + kRoadBayDeep;
            x.add(x.box(fr, s0, s1, lo(hc, bk), hi(hc, bk), g, kRoadSlabDepth, 0.0, 0.0, "bay"), RoadTint::Asphalt,
                  true);
            x.add(x.box(fr, s0, s1, lo(bk, bk + kRoadKerbWide), hi(bk, bk + kRoadKerbWide), pv, kRoadSlabDepth + 0.15,
                        0.0, 0.0, "kerb"),
                  RoadTint::Kerb, true);
            x.add(x.box(fr, s0, s1, lo(bk + kRoadKerbWide, w2), hi(bk + kRoadKerbWide, w2), pv, kRoadSlabDepth, 0.0,
                        0.0, "pavement"),
                  RoadTint::Pavement, true);
            break;
        }
        case Span::Mouth: {
            const double back = w2 + x.r.mouthBack;
            const double zb = x.r.mouthZ > 0.0 ? x.r.mouthZ : pv;
            const double rise = (zb - g) / (back - hc);
            x.add(x.box(fr, s0, s1, lo(hc, back), hi(hc, back), (g + zb) * 0.5, kRoadSlabDepth, 0.0, sigma * rise,
                        "crossover"),
                  RoadTint::Pavement, true);
            MassingFoot ft = footOf(x.box(fr, s0, s1, lo(hc, back), hi(hc, back), 0.0, 0.0, 0.0, 0.0, "foot"));
            x.out.band.push_back(ft);
            x.out.keepOut.push_back(ft);
            break;
        }
        case Span::Ramp: {
            const double rk = hc + kRoadRampDeep;
            const double rise = (pv - g) / kRoadRampDeep;
            x.add(x.box(fr, s0, s1, lo(hc, rk), hi(hc, rk), (g + pv) * 0.5, kRoadSlabDepth, 0.0, sigma * rise, "ramp"),
                  RoadTint::Pavement, true);
            // Tactile paving at the ramp's foot.
            x.add(x.box(fr, s0 + 0.3, s1 - 0.3, lo(hc + 0.15, hc + 0.75), hi(hc + 0.15, hc + 0.75),
                        g + (0.45) * rise + kMarkLift, kMarkDeep, 0.0, sigma * rise, "tactile"),
                  RoadTint::LineYellow, false);
            x.add(x.box(fr, s0, s1, lo(rk, w2), hi(rk, w2), pv, kRoadSlabDepth, 0.0, 0.0, "pavement"),
                  RoadTint::Pavement, true);
            break;
        }
        case Span::Corner:
            break;
        }
    }
    // Edge lines along the kerb, broken over mouths, corners and zebras; bay ends.
    double run0 = -1.0;
    for (std::size_t i = 0; i <= spans.size(); ++i) {
        const bool lined = i < spans.size() && (spans[i].type == Span::Normal || spans[i].type == Span::Bay ||
                                                spans[i].type == Span::Bus);
        if (lined && run0 < 0.0) run0 = spans[i].s0;
        if (!lined && run0 >= 0.0) {
            const double end = i < spans.size() ? spans[i].s0 : len;
            if (end - run0 > 1.0) {
                if (flatCarriage)
                    x.mark(fr, run0, end, tt(hc - 0.25), tt(hc - 0.25 - kRoadLine), RoadTint::LineWhite, g);
                else
                    x.mark(fr, run0, end, tt(hc - 0.25), tt(hc - 0.25 - kRoadLine), RoadTint::LineWhite);
            }
            run0 = -1.0;
        }
        if (i < spans.size() && spans[i].type == Span::Bay) {
            for (const double e : {spans[i].s0, spans[i].s1})
                x.mark(fr, e - kRoadLine * 0.5, e + kRoadLine * 0.5, tt(hc), tt(hc + kRoadBayDeep - 0.05),
                       RoadTint::LineWhite, g);
        }
    }
    // The guard rail along the back edge, over every span but the mouths and corners.
    if (rail) {
        double r0 = -1.0;
        for (std::size_t i = 0; i <= spans.size(); ++i) {
            const bool on = i < spans.size() && spans[i].type != Span::Mouth && spans[i].type != Span::Corner;
            if (on && r0 < 0.0) r0 = spans[i].s0;
            if (!on && r0 >= 0.0) {
                const double end = i < spans.size() ? spans[i].s0 : len;
                if (end - r0 > 2.0) {
                    const double tr = w2 - 0.2;
                    x.add(x.box(fr, r0, end, lo(tr - 0.06, tr + 0.06), hi(tr - 0.06, tr + 0.06), pv + 0.8, 0.3, 0.0,
                                0.0, "rail"),
                          RoadTint::Steel, true);
                    const int posts = static_cast<int>(std::floor((end - r0) / 2.0));
                    for (int k = 0; k <= posts; ++k) {
                        const double s = std::min(r0 + 0.1 + 2.0 * k, end - 0.1);
                        const RoadPoint p = fr.at(s, tt(tr + 0.12));
                        x.add(x.at(p, fr.rot, 0.1, 0.1, pv, pv + 0.85, "railpost"), RoadTint::Steel, true);
                    }
                    MassingFoot ft = footOf(x.box(fr, r0, end, lo(tr - 0.06, tr + 0.2), hi(tr - 0.06, tr + 0.2), 0.0, 0.0,
                                                  0.0, 0.0, "foot"));
                    x.out.furniture.push_back(ft);
                }
                r0 = -1.0;
            }
        }
    }
}

// Spans: the fixed ones sorted, the gaps Normal; bays carved into the gaps on a parking side.
std::vector<SideSpan> fillSpans(std::vector<SideSpan> fixed, double len, bool park, const std::vector<double>& busAt) {
    std::sort(fixed.begin(), fixed.end(), [](const SideSpan& a, const SideSpan& b) { return a.s0 < b.s0; });
    std::vector<SideSpan> out;
    double s = 0.0;
    auto gap = [&](double a, double b, Span before, Span after) {
        if (b - a <= 1e-6) return;
        if (!park) {
            out.push_back({a, b, Span::Normal});
            return;
        }
        auto keep = [](Span t) {
            return t == Span::Ramp ? kRoadBayKeepJunction + kRoadStopBack + kRoadStopLine
                                   : (t == Span::Bus ? 0.0 : kRoadBayKeepJunction);
        };
        double p0 = a + (before == Span::Normal ? kRoadBayKeepEnd : keep(before));
        const double p1 = b - (after == Span::Normal ? kRoadBayKeepEnd : keep(after));
        if (p1 - p0 < kRoadBayLong) {
            out.push_back({a, b, Span::Normal});
            return;
        }
        double cur = a;
        int inRun = 0;
        while (p0 + kRoadBayLong <= p1 + 1e-9) {
            bool nearBus = false;
            for (const double bus : busAt)
                if (p0 < bus + kRoadBayKeepBus + 2.0 && p0 + kRoadBayLong > bus - kRoadBayKeepBus) nearBus = true;
            if (nearBus) {
                p0 += 1.0;
                inRun = 0;
                continue;
            }
            if (p0 > cur) out.push_back({cur, p0, Span::Normal});
            out.push_back({p0, p0 + kRoadBayLong, Span::Bay});
            cur = p0 + kRoadBayLong;
            p0 = cur;
            if (++inRun == 3) {
                p0 += 3.0; // a gap in each run of three for the lamps and people
                inRun = 0;
            }
        }
        if (b > cur) out.push_back({cur, b, Span::Normal});
    };
    Span prev = Span::Normal;
    for (const SideSpan& sp : fixed) {
        const double a = std::max(sp.s0, 0.0), b = std::min(sp.s1, len);
        if (b <= s) continue;
        gap(s, std::max(a, s), prev, sp.type);
        out.push_back({std::max(a, s), b, sp.type});
        s = b;
        prev = sp.type;
    }
    gap(s, len, prev, Span::Normal);
    return out;
}

Span spanAt(const std::vector<SideSpan>& spans, double s) {
    for (const SideSpan& sp : spans)
        if (s >= sp.s0 && s < sp.s1) return sp.type;
    return Span::Corner;
}
// Whether a spot on a side's pavement at s stands clear of its mouths, corners and ramps by `margin`.
bool spotFree(const std::vector<SideSpan>& spans, double s, double margin) {
    for (const SideSpan& sp : spans) {
        if (sp.type != Span::Mouth && sp.type != Span::Corner && sp.type != Span::Ramp) continue;
        if (s > sp.s0 - margin && s < sp.s1 + margin) return false;
    }
    return true;
}

} // namespace

RoadColor roadTint(RoadTint tint) {
    switch (tint) {
    case RoadTint::Asphalt: return {0.045f, 0.045f, 0.05f, 0.85f};
    case RoadTint::AsphaltPatch: return {0.07f, 0.07f, 0.075f, 0.8f};
    case RoadTint::Pavement: return {0.32f, 0.31f, 0.29f, 0.8f};
    case RoadTint::Kerb: return {0.46f, 0.45f, 0.43f, 0.75f};
    case RoadTint::LineWhite: return {0.78f, 0.78f, 0.76f, 0.6f};
    case RoadTint::LineYellow: return {0.75f, 0.55f, 0.08f, 0.6f};
    case RoadTint::Steel: return {0.42f, 0.44f, 0.46f, 0.45f};
    case RoadTint::PoleConcrete: return {0.40f, 0.39f, 0.37f, 0.8f};
    case RoadTint::SignalBody: return {0.05f, 0.05f, 0.05f, 0.5f};
    case RoadTint::LensRed: return {0.35f, 0.02f, 0.02f, 0.2f};
    case RoadTint::LensAmber: return {0.35f, 0.18f, 0.01f, 0.2f};
    case RoadTint::LensGreen: return {0.02f, 0.25f, 0.18f, 0.2f};
    case RoadTint::SignRed: return {0.55f, 0.04f, 0.04f, 0.5f};
    case RoadTint::SignBlue: return {0.04f, 0.12f, 0.45f, 0.5f};
    case RoadTint::SignWhite: return {0.80f, 0.80f, 0.80f, 0.5f};
    case RoadTint::Glass: return {0.04f, 0.06f, 0.07f, 0.08f};
    case RoadTint::Wood: return {0.30f, 0.19f, 0.10f, 0.7f};
    case RoadTint::BarrierRed: return {0.60f, 0.06f, 0.04f, 0.6f};
    case RoadTint::Wire: return {0.03f, 0.03f, 0.03f, 0.6f};
    case RoadTint::Tyre: return {0.02f, 0.02f, 0.02f, 0.9f};
    case RoadTint::CarWhite: return {0.78f, 0.78f, 0.76f, 0.25f};
    case RoadTint::CarSilver: return {0.48f, 0.49f, 0.50f, 0.25f};
    case RoadTint::CarBlack: return {0.03f, 0.03f, 0.035f, 0.2f};
    case RoadTint::CarRed: return {0.45f, 0.04f, 0.04f, 0.25f};
    case RoadTint::CarBlue: return {0.06f, 0.12f, 0.30f, 0.25f};
    case RoadTint::CarGreen: return {0.12f, 0.22f, 0.15f, 0.3f};
    case RoadTint::CarCream: return {0.70f, 0.64f, 0.50f, 0.3f};
    case RoadTint::TaxiYellow: return {0.75f, 0.52f, 0.05f, 0.3f};
    case RoadTint::BusGreen: return {0.10f, 0.35f, 0.22f, 0.3f};
    case RoadTint::Count: break;
    }
    return {0.5f, 0.5f, 0.5f, -1.f};
}

const char* roadTintName(RoadTint tint) {
    static const char* const names[] = {"Asphalt",   "AsphaltPatch", "Pavement",  "Kerb",      "LineWhite",
                                        "LineYellow", "Steel",       "PoleConcrete", "SignalBody", "LensRed",
                                        "LensAmber", "LensGreen",    "SignRed",   "SignBlue",  "SignWhite",
                                        "Glass",     "Wood",         "BarrierRed", "Wire",     "Tyre",
                                        "CarWhite",  "CarSilver",    "CarBlack",  "CarRed",    "CarBlue",
                                        "CarGreen",  "CarCream",     "TaxiYellow", "BusGreen"};
    const auto i = static_cast<std::size_t>(tint);
    return i < sizeof names / sizeof names[0] ? names[i] : "Unknown";
}

const char* roadVehicleName(RoadVehicleKind kind) {
    switch (kind) {
    case RoadVehicleKind::Kei: return "kei";
    case RoadVehicleKind::Sedan: return "sedan";
    case RoadVehicleKind::Taxi: return "taxi";
    case RoadVehicleKind::Van: return "van";
    case RoadVehicleKind::KeiTruck: return "kei_truck";
    case RoadVehicleKind::Bus: return "bus";
    case RoadVehicleKind::Scooter: return "scooter";
    case RoadVehicleKind::Bicycle: return "bicycle";
    case RoadVehicleKind::Count: break;
    }
    return "unknown";
}

MassingVec roadVehicleSize(RoadVehicleKind kind) {
    switch (kind) {
    case RoadVehicleKind::Kei: return {3.4, 1.48, 1.65};
    case RoadVehicleKind::Sedan: return {4.7, 1.8, 1.45};
    case RoadVehicleKind::Taxi: return {4.7, 1.7, 1.5};
    case RoadVehicleKind::Van: return {4.7, 1.7, 1.9};
    case RoadVehicleKind::KeiTruck: return {3.4, 1.48, 1.8};
    case RoadVehicleKind::Bus: return {10.5, 2.5, 3.1};
    case RoadVehicleKind::Scooter: return {1.8, 0.7, 1.1};
    case RoadVehicleKind::Bicycle: return {1.7, 0.5, 1.0};
    case RoadVehicleKind::Count: break;
    }
    return {1.0, 1.0, 1.0};
}

MassingFoot roadVehicleFoot(const RoadVehicle& v) {
    const MassingVec size = roadVehicleSize(v.kind);
    MassingFoot f;
    f.x = v.x;
    f.y = v.y;
    f.hx = size.x * 0.5;
    f.hy = size.y * 0.5;
    f.rot = v.rot;
    return f;
}

void roadVehicleSolids(const RoadVehicle& v, std::vector<RoadSolid>& out) {
    const MassingVec size = roadVehicleSize(v.kind);
    const double L = size.x, W = size.y, H = size.z;
    const std::string tag = std::string("FTVehicle_") + roadVehicleName(v.kind);
    const double c = std::cos(rad(v.rot)), s = std::sin(rad(v.rot));
    int part = 0;
    auto put = [&](double u, double w, double sx, double sy, double z0, double z1, RoadTint tint, bool collide,
                   RoadShape shape, RoadAxis axis) {
        RoadSolid sd;
        char buf[16];
        std::snprintf(buf, sizeof buf, "-%d", ++part);
        sd.box.id = v.id + buf;
        sd.box.row = v.where;
        sd.box.kind = "vehicle";
        sd.box.source = MassingSource::Invented;
        sd.box.cx = v.x + u * c - w * s;
        sd.box.cy = v.y + u * s + w * c;
        sd.box.sx = sx;
        sd.box.sy = sy;
        sd.box.rot = v.rot;
        sd.box.z0 = v.z + z0;
        sd.box.z1 = v.z + z1;
        sd.tint = tint;
        sd.collide = collide;
        sd.shape = shape;
        sd.axis = axis;
        sd.tag = tag;
        out.push_back(std::move(sd));
    };
    const bool two = v.kind == RoadVehicleKind::Scooter || v.kind == RoadVehicleKind::Bicycle;
    if (two) {
        const double wd = v.kind == RoadVehicleKind::Scooter ? 0.42 : 0.66;
        const double wt = v.kind == RoadVehicleKind::Scooter ? 0.1 : 0.04;
        for (const double u : {-(L * 0.5 - wd * 0.5), L * 0.5 - wd * 0.5})
            put(u, 0.0, wd, wt, 0.0, wd, RoadTint::Tyre, false, RoadShape::Cylinder, RoadAxis::AlongW);
        if (v.kind == RoadVehicleKind::Scooter) {
            put(-0.1, 0.0, L * 0.62, 0.32, 0.22, 0.62, v.body, false, RoadShape::Cube, RoadAxis::Up);
            put(-0.25, 0.0, 0.62, 0.28, 0.62, 0.78, RoadTint::CarBlack, false, RoadShape::Cube, RoadAxis::Up);
            put(L * 0.5 - 0.42, 0.0, 0.12, 0.34, 0.3, 1.0, v.body, false, RoadShape::Cube, RoadAxis::Up);
            put(L * 0.5 - 0.42, 0.0, 0.05, W, 1.0, 1.05, RoadTint::Steel, false, RoadShape::Cube, RoadAxis::Up);
        } else {
            put(0.0, 0.0, L * 0.55, 0.04, 0.45, 0.5, v.body, false, RoadShape::Cube, RoadAxis::Up);
            put(-0.2, 0.0, 0.04, 0.04, 0.3, 0.85, v.body, false, RoadShape::Cube, RoadAxis::Up);
            put(-0.2, 0.0, 0.26, 0.12, 0.85, 0.9, RoadTint::CarBlack, false, RoadShape::Cube, RoadAxis::Up);
            put(L * 0.5 - 0.45, 0.0, 0.04, 0.04, 0.35, 0.95, RoadTint::Steel, false, RoadShape::Cube, RoadAxis::Up);
            put(L * 0.5 - 0.45, 0.0, 0.04, W, 0.95, 0.98, RoadTint::Steel, false, RoadShape::Cube, RoadAxis::Up);
        }
        return;
    }
    const double wd = v.kind == RoadVehicleKind::Bus ? 1.0 : (L < 4.0 ? 0.55 : 0.64);
    const double bottom = wd * 0.3;
    double belt = H * 0.55, cabFront = L * 0.18, cabBack = -L * 0.32;
    switch (v.kind) {
    case RoadVehicleKind::Van:
    case RoadVehicleKind::Bus:
        belt = H * 0.5;
        cabFront = L * 0.42;
        cabBack = -L * 0.48;
        break;
    case RoadVehicleKind::Kei:
        belt = H * 0.5;
        cabFront = L * 0.3;
        cabBack = -L * 0.45;
        break;
    case RoadVehicleKind::KeiTruck:
        belt = H * 0.5;
        cabFront = L * 0.47;
        cabBack = L * 0.12;
        break;
    default: break;
    }
    put(0.0, 0.0, L, W, bottom, belt, v.body, true, RoadShape::Cube, RoadAxis::Up);
    const double cu = (cabFront + cabBack) * 0.5, cl = cabFront - cabBack;
    put(cu, 0.0, cl, W - 0.12, belt, H - 0.07, RoadTint::Glass, true, RoadShape::Cube, RoadAxis::Up);
    put(cu, 0.0, cl - 0.06, W - 0.16, H - 0.07, H, v.body, false, RoadShape::Cube, RoadAxis::Up);
    if (v.kind == RoadVehicleKind::KeiTruck) // the bed's gates
        put((-L * 0.5 + cabBack) * 0.5, 0.0, cabBack + L * 0.5, W, belt, belt + 0.3, v.body, false, RoadShape::Cube,
            RoadAxis::Up);
    if (v.kind == RoadVehicleKind::Taxi)
        put(cu, 0.0, 0.35, 0.14, H, H + 0.16, RoadTint::SignWhite, false, RoadShape::Cube, RoadAxis::Up);
    const double wu = L * 0.5 - wd * (v.kind == RoadVehicleKind::Bus ? 1.6 : 0.85);
    for (const double u : {-wu, wu})
        for (const double w : {-(W * 0.5 - 0.12), W * 0.5 - 0.12})
            put(u, w, wd, 0.2, 0.0, wd, RoadTint::Tyre, false, RoadShape::Cylinder, RoadAxis::AlongW);
}

bool importRoads(std::string_view csv, std::vector<RoadSpec>& out, std::string* error) {
    using namespace detail;
    out.clear();
    std::vector<Record> recs;
    if (!splitCsv(csv, recs, error)) return false;
    std::vector<std::string> head;
    for (const Record& rec : recs) {
        if (isComment(rec)) continue;
        if (head.empty()) {
            for (const std::string& h : rec.fields) head.emplace_back(trim(h));
            continue;
        }
        auto col = [&](const char* name) -> std::string_view {
            for (std::size_t i = 0; i < head.size() && i < rec.fields.size(); ++i)
                if (head[i] == name) return trim(rec.fields[i]);
            return {};
        };
        auto num = [&](const char* name, double& v, bool need) {
            const std::string_view s = col(name);
            if (s.empty()) return !need;
            return parseNumber(s, v);
        };
        RoadSpec r;
        r.id = std::string(col("id"));
        r.kind = std::string(col("kind"));
        if (!validId(r.id)) return fail(error, rec.line, "a road needs an id (A-Z, 0-9, '_', '-')");
        if (!validKind(r.kind)) return fail(error, rec.line, r.id + ": a road needs a kind");
        if (!num("width_m", r.width, true) || !num("pavement_m", r.pavement, true) || !num("lane_m", r.lane, false) ||
            !num("speed_kmh", r.speedKmh, false) || !num("ground_z", r.ground, true) ||
            !num("mouth_back_m", r.mouthBack, false) || !num("mouth_z", r.mouthZ, false))
            return fail(error, rec.line, r.id + ": a number column is missing or not a number");
        const std::string_view ow = col("one_way");
        r.oneWay = ow == "1" || ow == "true" || ow == "True";
        // The two points.
        {
            const std::string pts(col("pts"));
            double v[4] = {0, 0, 0, 0};
            int k = 0;
            std::size_t p = 0;
            while (p <= pts.size() && k < 5) {
                const std::size_t q = pts.find_first_of(":;", p);
                const std::string_view part = std::string_view(pts).substr(p, q == std::string::npos ? std::string::npos : q - p);
                if (k >= 4 || !parseNumber(part, v[k])) return fail(error, rec.line, r.id + ": pts must be \"x:y;x:y\"");
                ++k;
                if (q == std::string::npos) break;
                p = q + 1;
            }
            if (k != 4) return fail(error, rec.line, r.id + ": pts must be \"x:y;x:y\"");
            r.x0 = v[0];
            r.y0 = v[1];
            r.x1 = v[2];
            r.y1 = v[3];
            if (std::hypot(r.x1 - r.x0, r.y1 - r.y0) < 10.0)
                return fail(error, rec.line, r.id + ": a road is at least 10 m long");
        }
        const double carriage = r.width - 2.0 * r.pavement;
        const double lanes = r.oneWay ? 1.0 : 2.0;
        if (r.pavement < 0.0 || r.lane < 3.0 - 1e-9 || r.lane > 3.5 + 1e-9 || carriage < lanes * r.lane - 1e-6)
            return fail(error, rec.line, r.id + ": the carriageway must hold its lanes (3.0 to 3.5 m each)");
        auto sides = [&](const char* name, bool& left, bool& right) {
            const std::string_view s = col(name);
            left = s.find('L') != std::string_view::npos;
            right = s.find('R') != std::string_view::npos;
        };
        sides("parking", r.parkLeft, r.parkRight);
        sides("rail", r.railLeft, r.railRight);
        sides("poles", r.poleLeft, r.poleRight);
        r.closedEnds = col("ends") == "closed";
        // "a;b;c" items of '@'-separated fields.
        auto items = [](std::string_view s, std::vector<std::vector<std::string>>& out2) {
            out2.clear();
            std::size_t p = 0;
            while (p < s.size()) {
                std::size_t q = s.find(';', p);
                if (q == std::string_view::npos) q = s.size();
                std::vector<std::string> f;
                std::size_t a = p;
                while (a <= q) {
                    std::size_t b = s.find('@', a);
                    if (b == std::string_view::npos || b > q) b = q;
                    f.emplace_back(trim(s.substr(a, b - a)));
                    a = b + 1;
                }
                if (!(f.size() == 1 && f[0].empty())) out2.push_back(std::move(f));
                p = q + 1;
            }
        };
        std::vector<std::vector<std::string>> it;
        items(col("mouths"), it);
        for (const std::vector<std::string>& f : it) {
            RoadMouth m;
            if (f.size() < 4 || f.size() > 5 || !validId(f[0]) || !parseNumber(f[1], m.at) || !parseNumber(f[2], m.width) ||
                (f[3] != "L" && f[3] != "R") || (f.size() == 5 && !parseNumber(f[4], m.stub)) || m.width <= 0.0 ||
                m.stub < 0.0)
                return fail(error, rec.line, r.id + ": a mouth is \"lane@at@width@L|R[@stub]\"");
            m.lane = f[0];
            m.left = f[3] == "L";
            r.mouths.push_back(std::move(m));
        }
        items(col("bus_stops"), it);
        for (const std::vector<std::string>& f : it) {
            RoadBusStop b;
            if (f.size() != 2 || !parseNumber(f[0], b.at) || (f[1] != "L" && f[1] != "R"))
                return fail(error, rec.line, r.id + ": a bus stop is \"at@L|R\"");
            b.left = f[1] == "L";
            r.busStops.push_back(b);
        }
        for (const RoadSpec& o : out)
            if (o.id == r.id) return fail(error, rec.line, r.id + ": the id is used twice");
        out.push_back(std::move(r));
    }
    if (head.empty()) return fail(error, 1, "the table has no header row");
    return true;
}

void roadBuild(const RoadSpec& road, RoadBuild& out) {
    Ctx x(road, out);
    x.f = frameOf(road.x0, road.y0, road.x1, road.y1);
    x.len = std::hypot(road.x1 - road.x0, road.y1 - road.y0);
    const double carriage = road.width - 2.0 * road.pavement;
    x.hc = carriage * 0.5;
    x.w2 = road.width * 0.5;
    x.gutter = road.ground + kRoadSurfaceLift;
    x.crown = x.gutter + kRoadCrossfall * x.hc;
    x.pave = x.gutter + kRoadKerbRise;
    const Frame& f = x.f;
    const double hc = x.hc, w2 = x.w2, len = x.len, R = kRoadCornerRadius;
    const double stubHalf = road.lane; // a stub: one lane each way

    // The carriageway: two slabs from the crown down to the gutters.
    x.add(x.box(f, 0.0, len, 0.0, hc, x.crown - kRoadCrossfall * hc * 0.5, kRoadSlabDepth, 0.0, -kRoadCrossfall, "asphalt"),
          RoadTint::Asphalt, true);
    x.add(x.box(f, 0.0, len, -hc, 0.0, x.crown - kRoadCrossfall * hc * 0.5, kRoadSlabDepth, 0.0, kRoadCrossfall, "asphalt"),
          RoadTint::Asphalt, true);
    out.carriageway.push_back(footOf(x.box(f, 0.0, len, -hc, hc, 0.0, 0.0, 0.0, 0.0, "foot")));
    out.band.push_back(footOf(x.box(f, 0.0, len, -w2, w2, 0.0, 0.0, 0.0, 0.0, "foot")));

    // The spans each side.
    std::vector<SideSpan> fixedL, fixedR;
    std::vector<double> busL, busR;
    struct Zebra {
        double s0, s1;
    };
    std::vector<Zebra> zebras;
    struct Junction {
        const RoadMouth* m;
        double sigmaStub; // the stub's side
        double zs0, zs1, zn0, zn1;
    };
    std::vector<Junction> juncs;
    for (const RoadMouth& m : road.mouths) {
        const double sl = m.left ? 1.0 : -1.0;
        (m.left ? fixedL : fixedR).push_back({m.at - m.width * 0.5 - kRoadMouthFlare, m.at + m.width * 0.5 + kRoadMouthFlare, Span::Mouth});
        ++out.counts.mouths;
        if (m.stub > 0.0) {
            const double J = stubHalf + R;
            (m.left ? fixedR : fixedL).push_back({m.at - J, m.at + J, Span::Corner});
            Junction j{&m, -sl, m.at - J - kRoadZebraOff - kRoadZebraWide, m.at - J - kRoadZebraOff, m.at + J + kRoadZebraOff,
                       m.at + J + kRoadZebraOff + kRoadZebraWide};
            juncs.push_back(j);
            zebras.push_back({j.zs0, j.zs1});
            zebras.push_back({j.zn0, j.zn1});
        }
    }
    for (const Zebra& z : zebras) {
        for (std::vector<SideSpan>* side : {&fixedL, &fixedR}) {
            bool clash = false;
            for (const SideSpan& sp : *side)
                if (z.s0 < sp.s1 && z.s1 > sp.s0) clash = true;
            if (!clash) side->push_back({z.s0, z.s1, Span::Ramp});
        }
    }
    for (const RoadBusStop& b : road.busStops) {
        (b.left ? fixedL : fixedR).push_back({b.at - 1.0, b.at + kShelterLong + 1.5, Span::Bus});
        (b.left ? busL : busR).push_back(b.at);
    }
    const std::vector<SideSpan> spansL = fillSpans(fixedL, len, road.parkLeft, busL);
    const std::vector<SideSpan> spansR = fillSpans(fixedR, len, road.parkRight, busR);
    buildSide(x, f, 1.0, hc, w2, len, spansL, false, road.railLeft);
    buildSide(x, f, -1.0, hc, w2, len, spansR, false, road.railRight);

    // Bays.
    for (int side = 0; side < 2; ++side) {
        const double sg = side == 0 ? 1.0 : -1.0;
        for (const SideSpan& sp : side == 0 ? spansL : spansR) {
            if (sp.type != Span::Bay) continue;
            RoadBay bay;
            bay.id = x.id("bay");
            bay.foot = footOf(x.box(f, sp.s0, sp.s1, std::min(sg * hc, sg * (hc + kRoadBayDeep)),
                                    std::max(sg * hc, sg * (hc + kRoadBayDeep)), 0.0, 0.0, 0.0, 0.0, "foot"));
            bay.at = (sp.s0 + sp.s1) * 0.5;
            bay.z = x.gutter;
            bay.heading = sg > 0.0 ? f.rot : f.rot + 180.0;
            bay.left = sg > 0.0;
            out.bays.push_back(bay);
            ++out.counts.bays;
        }
    }

    // The centre line: dashed, solid for 30 m before a stop line, none through a junction.
    if (!road.oneWay) {
        std::vector<std::pair<double, double>> gone, solid;
        for (const Junction& j : juncs) {
            gone.push_back({j.zs0 - kRoadStopBack - kRoadStopLine - 0.5, j.zn1 + kRoadStopBack + kRoadStopLine + 0.5});
            solid.push_back({j.zs0 - 30.0, j.zn1 + 30.0});
        }
        // Broken across each lane mouth, either side.
        for (const std::vector<SideSpan>* side : {&spansL, &spansR})
            for (const SideSpan& sp : *side)
                if (sp.type == Span::Mouth) gone.push_back({sp.s0, sp.s1});
        double s = 2.0;
        while (s < len - 2.0) {
            bool skip = false, sol = false;
            double next = s + kRoadCentreDash;
            for (const auto& g : gone)
                if (s >= g.first && s < g.second) {
                    skip = true;
                    next = g.second;
                } else if (s < g.first && next > g.first) {
                    next = g.first;
                }
            for (const auto& g : solid)
                if (s >= g.first && s < g.second) sol = true;
            next = std::min(next, len - 2.0);
            if (!skip) {
                if (sol) {
                    double e = next;
                    for (const auto& g : gone)
                        if (s < g.first) e = std::min(e, g.first);
                    x.mark(f, s, e, -kRoadLine * 0.5, kRoadLine * 0.5, RoadTint::LineYellow, x.crown);
                    s = e;
                    continue;
                }
                x.mark(f, s, next, -kRoadLine * 0.5, kRoadLine * 0.5, RoadTint::LineWhite, x.crown);
                s = next + kRoadCentreDash;
                continue;
            }
            s = next;
        }
    }

    // Zebras, stop lines and the junctions.
    auto zebraBars = [&](const Frame& fr, double s0, double s1, double half, bool flat) {
        for (double t = 0.225; t + kRoadZebraBar <= half - 0.1 + 1e-9; t += 2.0 * kRoadZebraBar) {
            if (flat) {
                x.mark(fr, s0, s1, t, t + kRoadZebraBar, RoadTint::LineWhite, x.gutter);
                x.mark(fr, s0, s1, -t, -t - kRoadZebraBar, RoadTint::LineWhite, x.gutter);
            } else {
                x.mark(fr, s0, s1, t, t + kRoadZebraBar, RoadTint::LineWhite);
                x.mark(fr, s0, s1, -t, -t - kRoadZebraBar, RoadTint::LineWhite);
            }
        }
        RoadQuad q;
        q.id = x.id("zebra");
        q.pts = {fr.at(s0, -half), fr.at(s1, -half), fr.at(s1, half), fr.at(s0, half)};
        out.crossings.push_back(q);
        out.keepOut.push_back(footOf(x.box(fr, s0, s1, -half, half, 0.0, 0.0, 0.0, 0.0, "foot")));
        ++out.counts.zebras;
    };
    int jn = 0;
    for (const Junction& j : juncs) {
        const RoadMouth& m = *j.m;
        const double ss = j.sigmaStub, sl = -ss;
        char jbuf[16];
        std::snprintf(jbuf, sizeof jbuf, "J%d", ++jn);
        const std::string jid = road.id + "-" + jbuf;
        const double J = stubHalf + R;
        zebraBars(f, j.zs0, j.zs1, hc, false);
        zebraBars(f, j.zn0, j.zn1, hc, false);
        // Stop lines: the +s way in the left lane before the south zebra, the -s way in the right lane past the north.
        x.mark(f, j.zs0 - kRoadStopBack - kRoadStopLine, j.zs0 - kRoadStopBack, 0.0, hc, RoadTint::LineWhite);
        x.mark(f, j.zn1 + kRoadStopBack, j.zn1 + kRoadStopBack + kRoadStopLine, -hc, 0.0, RoadTint::LineWhite);
        out.keepOut.push_back(footOf(x.box(f, j.zs0 - kRoadStopBack - kRoadStopLine, j.zs0, 0.0, hc, 0, 0, 0, 0, "foot")));
        out.keepOut.push_back(footOf(x.box(f, j.zn1, j.zn1 + kRoadStopBack + kRoadStopLine, -hc, 0.0, 0, 0, 0, 0, "foot")));
        RoadJunction rj;
        rj.id = jid;
        const RoadPoint c = f.at(m.at, 0.0);
        rj.x = c.x;
        rj.y = c.y;
        rj.road = road.id;
        rj.lane = m.lane;
        rj.box = footOf(x.box(f, m.at - J, m.at + J, -hc, hc, 0, 0, 0, 0, "foot"));
        out.keepOut.push_back(rj.box);
        out.junctions.push_back(rj);
        ++out.counts.junctions;

        // The stub: its own frame from the kerb line outward.
        const RoadPoint so = f.at(m.at, ss * hc);
        const RoadPoint sd = f.dir(0.0, ss);
        Frame sf = frameOf(so.x, so.y, so.x + sd.x, so.y + sd.y);
        const double sl2 = m.stub;
        const double sw2 = stubHalf + kStubPavement;
        x.add(x.box(sf, 0.0, sl2, -stubHalf, stubHalf, x.gutter, kRoadSlabDepth, 0.0, 0.0, "asphalt"), RoadTint::Asphalt, true);
        out.carriageway.push_back(footOf(x.box(sf, 0.0, sl2, -stubHalf, stubHalf, 0, 0, 0, 0, "foot")));
        out.band.push_back(footOf(x.box(sf, 0.0, sl2, -sw2, sw2, 0, 0, 0, 0, "foot")));
        const double sz0 = R + kRoadZebraOff, sz1 = sz0 + kRoadZebraWide;
        std::vector<SideSpan> sfix = {{0.0, R, Span::Corner}, {sz0, sz1, Span::Ramp}};
        const std::vector<SideSpan> sspans = fillSpans(sfix, sl2, false, {});
        buildSide(x, sf, 1.0, stubHalf, sw2, sl2, sspans, true, false);
        buildSide(x, sf, -1.0, stubHalf, sw2, sl2, sspans, true, false);
        zebraBars(sf, sz0, sz1, stubHalf, true);
        // Inbound (towards the road, -s of the stub) keeps to its left: the stub's t < 0.
        x.mark(sf, sz1 + kRoadStopBack, sz1 + kRoadStopBack + kRoadStopLine, -stubHalf, 0.0, RoadTint::LineWhite, x.gutter);
        x.mark(sf, sz1 + kRoadStopBack + kRoadStopLine + 1.0, sl2 - 2.0, -kRoadLine * 0.5, kRoadLine * 0.5,
               RoadTint::LineYellow, x.gutter);
        // Its end: a rail, a barrier and a no-entry sign facing the junction.
        x.add(x.box(sf, sl2 - 0.2, sl2 - 0.08, -sw2, sw2, x.pave + 0.8, 0.3, 0, 0, "rail"), RoadTint::Steel, true);
        x.add(x.box(sf, sl2 - 1.2, sl2 - 1.0, -stubHalf + 0.3, stubHalf - 0.3, x.gutter + 1.0, 0.25, 0, 0, "barrier"),
              RoadTint::BarrierRed, true, RoadShape::Cube, RoadAxis::Up, "FTRoadBarrier");
        for (const double t : {-stubHalf + 0.5, stubHalf - 0.5})
            x.add(x.box(sf, sl2 - 1.15, sl2 - 1.05, t - 0.05, t + 0.05, x.gutter + 0.8, 0.8, 0, 0, "barrierpost"),
                  RoadTint::SignWhite, true);
        out.furniture.push_back(footOf(x.box(sf, sl2 - 1.25, sl2 - 0.95, -stubHalf, stubHalf, 0, 0, 0, 0, "foot")));
        out.furniture.push_back(footOf(x.box(sf, sl2 - 0.2, sl2 - 0.08, -sw2, sw2, 0, 0, 0, 0, "foot")));
        x.sign(sf.at(sl2 - 1.6, stubHalf + 0.8), sf.dir(-1.0, 0.0), x.pave, "no_entry", RoadTint::SignRed, true);

        // The corners: asphalt over the square between the kerb lines and the arc's centre where it lies in the road's
        // or the stub's band (the rest is the lot behind), the pavement from the kerb on the arc back to where it leaves
        // both bands, the kerb on the arc; a signal pole and a lamp on each.
        RoadPoint cornerPole[2];
        for (int qi = 0; qi < 2; ++qi) {
            const double q = qi == 0 ? -1.0 : 1.0;
            const double cs = m.at + q * J, ct = ss * (hc + R);
            const double a0s = std::min(m.at + q * stubHalf, cs), a1s = std::max(m.at + q * stubHalf, cs);
            const double sb = m.at + q * sw2; // the stub band's edge on this corner's side
            const double b0s = std::min(m.at + q * stubHalf, sb), b1s = std::max(m.at + q * stubHalf, sb);
            // Across the road's band (to its back edge w2), then along the stub's band (out to the arc's centre).
            x.add(x.box(f, a0s, a1s, std::min(ss * hc, ss * w2), std::max(ss * hc, ss * w2), x.gutter, kRoadSlabDepth, 0, 0,
                        "asphalt"),
                  RoadTint::Asphalt, true);
            x.add(x.box(f, b0s, b1s, std::min(ss * w2, ct), std::max(ss * w2, ct), x.gutter, kRoadSlabDepth, 0, 0, "asphalt"),
                  RoadTint::Asphalt, true);
            out.band.push_back(footOf(x.box(f, a0s, a1s, std::min(ss * hc, ss * w2), std::max(ss * hc, ss * w2), 0, 0, 0, 0,
                                            "foot")));
            out.band.push_back(footOf(x.box(f, b0s, b1s, std::min(ss * w2, ct), std::max(ss * w2, ct), 0, 0, 0, 0, "foot")));
            const double thA = std::atan2(-ss, 0.0), thB = std::atan2(0.0, -q);
            double d = thB - thA;
            while (d > kPi) d -= 2.0 * kPi;
            while (d < -kPi) d += 2.0 * kPi;
            const double step = d / kCornerSteps;
            const RoadPoint C = f.at(cs, ct);
            for (int k = 0; k < kCornerSteps; ++k) {
                const double tm = thA + step * (k + 0.5);
                const RoadPoint u = f.dir(std::cos(tm), std::sin(tm));
                const double chord = 2.0 * R * std::sin(std::fabs(step) * 0.5);
                const double rin = R - kRoadKerbWide;
                // Where the ray from the centre enters the road's band (towards the road) or the stub's (towards it).
                const double toRoad = -ss * (u.x * -f.dy + u.y * f.dx), toStub = -q * (u.x * f.dx + u.y * f.dy);
                double rb = rin - 0.05;
                if (toRoad > 1e-6) rb = std::min(rb, (hc + R - w2) / toRoad);
                if (toStub > 1e-6) rb = std::min(rb, (J - sw2) / toStub);
                rb = std::max(0.0, rb);
                MassingBox pie = x.at({C.x + u.x * (rb + rin) * 0.5, C.y + u.y * (rb + rin) * 0.5}, angleOf(u), rin - rb,
                                      chord * rin / R + 0.04, x.pave - kRoadSlabDepth, x.pave, "pavement");
                x.add(pie, RoadTint::Pavement, true);
                const double rk = (R - kRoadKerbWide * 0.5) * std::cos(std::fabs(step) * 0.5);
                MassingBox kb = x.at({C.x + u.x * rk, C.y + u.y * rk}, angleOf(u) + 90.0, chord + 0.03, kRoadKerbWide,
                                     x.pave - kRoadSlabDepth - 0.15, x.pave, "kerb");
                x.add(kb, RoadTint::Kerb, true);
            }
            const double tb = thA + d * 0.5;
            const RoadPoint ub = f.dir(std::cos(tb), std::sin(tb));
            cornerPole[qi] = {C.x + ub.x * (R - kRoadSignalPoleBack), C.y + ub.y * (R - kRoadSignalPoleBack)};
            const double tl = thA + d * (q > 0.0 ? 0.2 : 0.8);
            const RoadPoint ul = f.dir(std::cos(tl), std::sin(tl));
            MassingProp lamp;
            lamp.x = C.x + ul.x * (R - kRoadLampBack - 0.3);
            lamp.y = C.y + ul.y * (R - kRoadLampBack - 0.3);
            lamp.z = x.pave;
            lamp.face = angleOf({-ul.x, -ul.y}) + 180.0; // its head out over the arc, towards the corner
            lamp.face = angleOf(ul);
            out.lamps.push_back(lamp);
            ++out.counts.lamps;
        }
        // Vehicle signals at each far-left corner.
        // The road's +s way (left lane): far side +s, left side.
        auto laneSidePole = [&](double sSide) {
            return f.at(m.at + sSide * (m.width * 0.5 + kRoadMouthFlare + 1.0), sl * (hc + kRoadSignalPoleBack));
        };
        auto poleAt = [&](double sSide, double tSide) {
            if (tSide == ss) return cornerPole[sSide > 0.0 ? 1 : 0];
            return laneSidePole(sSide);
        };
        {
            const RoadPoint p = poleAt(1.0, 1.0);
            vehicleSignal(x, p, f.at(m.at + 1.0 * ((p.x - f.ox) * f.dx + (p.y - f.oy) * f.dy - m.at), hc * 0.5),
                          f.dir(-1.0, 0.0), x.pave, jid, 'A');
        }
        {
            const RoadPoint p = poleAt(-1.0, -1.0);
            vehicleSignal(x, p, f.at(m.at - 1.0 * (m.at - ((p.x - f.ox) * f.dx + (p.y - f.oy) * f.dy)), -hc * 0.5),
                          f.dir(1.0, 0.0), x.pave, jid, 'A');
        }
        // The stub's inbound way: far-left corner on the lane side, s side ss.
        {
            const RoadPoint p = laneSidePole(ss);
            vehicleSignal(x, p, p, f.dir(0.0, ss), x.pave, jid, 'B');
        }
        // The lane's inbound way: far-left corner on the stub side, s side sl; the arm over its inbound half.
        {
            const RoadPoint p = cornerPole[sl > 0.0 ? 1 : 0];
            const double ps = (p.x - f.ox) * f.dx + (p.y - f.oy) * f.dy;
            const double pt = (p.x - f.ox) * -f.dy + (p.y - f.oy) * f.dx;
            vehicleSignal(x, p, f.at(m.at + sl * m.width * 0.25, pt), f.dir(0.0, sl), x.pave, jid, 'B');
            (void)ps;
        }
        // Pedestrian signals at each zebra's ends, on the side away from the junction; phases: crossing the road B,
        // the stub A.
        for (const double side : {1.0, -1.0}) {
            pedSignal(x, f.at(j.zn1 + 0.6, side * (hc + kRoadSignalPoleBack)), f.dir(0.0, -side), x.pave, jid, 'B');
            pedSignal(x, f.at(j.zs0 - 0.6, side * (hc + kRoadSignalPoleBack)), f.dir(0.0, -side), x.pave, jid, 'B');
            pedSignal(x, sf.at(sz1 + 0.6, side * (stubHalf + kRoadSignalPoleBack)), sf.dir(0.0, -side), x.pave, jid, 'A');
        }
        // No parking signs before the junction, each way.
        x.sign(f.at(j.zs0 - 12.0, hc + 1.2), f.dir(-1.0, 0.0), x.pave, "no_parking", RoadTint::SignBlue, true);
        x.sign(f.at(j.zn1 + 12.0, -hc - 1.2), f.dir(1.0, 0.0), x.pave, "no_parking", RoadTint::SignBlue, true);
        (void)sl2;
    }

    // Spots behind the kerb on a side: on the pavement, behind a bay's kerb in a bay.
    auto backOf = [&](const std::vector<SideSpan>& spans, double s, double back) {
        return spanAt(spans, s) == Span::Bay ? hc + kRoadBayDeep + back : hc + back;
    };
    // Stop signs at the plain lane mouths, on the lane traffic's left (the +s side for a left mouth).
    for (const RoadMouth& m : road.mouths) {
        if (m.stub > 0.0) continue;
        const double sl = m.left ? 1.0 : -1.0;
        const std::vector<SideSpan>& spans = m.left ? spansL : spansR;
        const double s = m.at + sl * (m.width * 0.5 + kRoadMouthFlare + 0.6);
        x.sign(f.at(s, sl * backOf(spans, s, 1.2)), f.dir(0.0, sl), x.pave, "stop", RoadTint::SignRed, false);
    }
    // Speed limits at each end, on each way's left; no-entry and a barrier at a closed end.
    {
        const double s0 = 15.0, s1 = len - 15.0;
        x.sign(f.at(s0, backOf(spansL, s0, 1.0)), f.dir(-1.0, 0.0), x.pave, "speed", RoadTint::SignWhite, true);
        if (!road.oneWay)
            x.sign(f.at(s1, -backOf(spansR, s1, 1.0)), f.dir(1.0, 0.0), x.pave, "speed", RoadTint::SignWhite, true);
    }
    if (road.closedEnds) {
        for (const double e : {0.0, len}) {
            const double dir = e == 0.0 ? 1.0 : -1.0;
            const double sb = e + dir * 0.6;
            x.add(x.box(f, sb - 0.1, sb + 0.1, -hc + 0.2, hc - 0.2, x.crown + 1.0, 0.25, 0, 0, "barrier"), RoadTint::BarrierRed,
                  true, RoadShape::Cube, RoadAxis::Up, "FTRoadBarrier");
            x.add(x.box(f, sb - 0.11, sb + 0.11, -hc + 0.2, hc - 0.2, x.crown + 0.72, 0.06, 0, 0, "barrier"),
                  RoadTint::SignWhite, false);
            for (const double t : {-hc + 0.5, 0.0, hc - 0.5})
                x.add(x.at(f.at(sb, t), f.rot, 0.1, 0.1, x.crown - kRoadCrossfall * std::fabs(t), x.crown + 1.0, "barrierpost"),
                      RoadTint::SignWhite, true);
            out.furniture.push_back(footOf(x.box(f, sb - 0.15, sb + 0.15, -hc, hc, 0, 0, 0, 0, "foot")));
            // Seen by the way running into the end: its left is -t at the first end, +t at the last.
            const double ts = e == 0.0 ? -1.0 : 1.0;
            const double ss2 = e + dir * 1.2;
            x.sign(f.at(ss2, ts * backOf(ts > 0 ? spansL : spansR, ss2, 0.8)), f.dir(dir, 0.0), x.pave, "no_entry",
                   RoadTint::SignRed, true);
        }
    }

    // Bus stops: the pole at the kerb, the shelter at the back of the pavement past it, a bench inside.
    for (const RoadBusStop& b : road.busStops) {
        const double sg = b.left ? 1.0 : -1.0;
        x.post(f.at(b.at, sg * (hc + 0.8)), 0.08, x.pave, 2.4, RoadTint::Steel, "busstop");
        x.add(x.at(f.at(b.at, sg * (hc + 0.8)), f.rot, 0.04, 0.5, x.pave + 1.95, x.pave + 2.45, "sign"), RoadTint::SignWhite,
              false, RoadShape::Cylinder, RoadAxis::AlongU, "FTRoadSign_bus_stop");
        ++out.counts.signs;
        const double s0 = b.at + 1.0, s1 = s0 + kShelterLong;
        const double t1 = w2 - 0.3, t0 = t1 - kShelterDeep;
        auto tl = [&](double a, double c2) { return std::min(sg * a, sg * c2); };
        auto th = [&](double a, double c2) { return std::max(sg * a, sg * c2); };
        for (const double s : {s0 + 0.05, s1 - 0.05})
            for (const double t : {t0 + 0.05, t1 - 0.05})
                x.add(x.at(f.at(s, sg * t), f.rot, 0.08, 0.08, x.pave, x.pave + kShelterHigh, "shelterpost"), RoadTint::Steel,
                      true);
        x.add(x.box(f, s0 - 0.1, s1 + 0.1, tl(t0 - 0.15, t1 + 0.05), th(t0 - 0.15, t1 + 0.05), x.pave + kShelterHigh + 0.08,
                    0.08, 0, 0, "shelterroof"),
              RoadTint::Steel, false);
        x.add(x.box(f, s0 + 0.1, s1 - 0.1, tl(t1 - 0.06, t1 - 0.03), th(t1 - 0.06, t1 - 0.03), x.pave + 2.1, 1.8, 0, 0,
                    "shelterglass"),
              RoadTint::Glass, true);
        x.add(x.box(f, s0 + 0.8, s1 - 0.8, tl(t1 - 0.5, t1 - 0.1), th(t1 - 0.5, t1 - 0.1), x.pave + 0.45, 0.05, 0, 0, "bench"),
              RoadTint::Wood, false);
        out.furniture.push_back(footOf(x.box(f, s0, s1, tl(t0, t1), th(t0, t1), 0, 0, 0, 0, "foot")));
        // The stop's box on the carriageway.
        x.mark(f, b.at - 8.0, b.at + 12.0, sg * (hc - 0.45), sg * (hc - 0.3), RoadTint::LineYellow);
        out.keepOut.push_back(footOf(x.box(f, b.at - kRoadBayKeepBus, b.at + kRoadBayKeepBus + 2.0, std::min(sg * hc, sg * (hc + kRoadBayDeep)),
                                           std::max(sg * hc, sg * (hc + kRoadBayDeep)), 0, 0, 0, 0, "foot")));
    }

    // Lamps each side, staggered; each moved along to a free spot.
    for (int side = 0; side < 2; ++side) {
        const double sg = side == 0 ? 1.0 : -1.0;
        const std::vector<SideSpan>& spans = side == 0 ? spansL : spansR;
        for (double s = side == 0 ? kRoadLampSpacing * 0.5 : kRoadLampSpacing; s < len - 3.0; s += kRoadLampSpacing) {
            for (const double nudge : {0.0, 2.0, -2.0, 4.0, -4.0, 6.0, -6.0}) {
                const double sp = s + nudge;
                if (sp < 3.0 || sp > len - 3.0 || !spotFree(spans, sp, 1.5)) continue;
                bool nearBus = false;
                for (const RoadBusStop& b : road.busStops)
                    if ((b.left ? 1.0 : -1.0) == sg && sp > b.at - 1.5 && sp < b.at + kShelterLong + 2.5) nearBus = true;
                if (nearBus) continue;
                // Off a bay's end lines and a bay run's join.
                const RoadPoint p = f.at(sp, sg * backOf(spans, sp, kRoadLampBack));
                MassingProp lamp;
                lamp.x = p.x;
                lamp.y = p.y;
                lamp.z = x.pave;
                const RoadPoint toward = f.dir(0.0, -sg);
                lamp.face = angleOf(toward);
                out.lamps.push_back(lamp);
                ++out.counts.lamps;
                MassingFoot ft;
                ft.x = p.x;
                ft.y = p.y;
                ft.hx = 0.3;
                ft.hy = 0.24;
                ft.rot = lamp.face;
                out.furniture.push_back(ft);
                break;
            }
        }
    }

    // Utility poles and their wires.
    for (int side = 0; side < 2; ++side) {
        if (!(side == 0 ? road.poleLeft : road.poleRight)) continue;
        const double sg = side == 0 ? 1.0 : -1.0;
        const std::vector<SideSpan>& spans = side == 0 ? spansL : spansR;
        std::vector<RoadPoint> poles;
        const double tp = w2 - 0.6;
        for (double s = kRoadPoleSpacing * 0.5; s < len - 3.0; s += kRoadPoleSpacing) {
            for (const double nudge : {0.0, 1.5, -1.5, 3.0, -3.0, 4.5, -4.5}) {
                const double sp = s + nudge;
                if (sp < 3.0 || sp > len - 3.0 || !spotFree(spans, sp, 1.0) || spanAt(spans, sp) == Span::Bus) continue;
                const RoadPoint p = f.at(sp, sg * tp);
                MassingFoot ft;
                ft.x = p.x;
                ft.y = p.y;
                ft.hx = ft.hy = 0.2;
                bool hit = false;
                for (const MassingFoot& o : out.furniture)
                    if (massingFeetOverlap(ft, o, kMassingPropGap)) hit = true;
                if (hit) continue;
                x.post(p, 0.3, x.pave, kPoleHigh, RoadTint::PoleConcrete, "utilitypole");
                x.add(x.at(p, f.rot + 90.0, 1.6, 0.1, x.pave + kPoleHigh - 0.75, x.pave + kPoleHigh - 0.65, "crossarm"),
                      RoadTint::Steel, false);
                poles.push_back(p);
                ++out.counts.poles;
                break;
            }
        }
        for (std::size_t i = 1; i < poles.size(); ++i) {
            const RoadPoint a = poles[i - 1], b = poles[i];
            const double l = std::hypot(b.x - a.x, b.y - a.y);
            for (const double off : {-0.6, 0.0, 0.6}) {
                const RoadPoint o = f.dir(0.0, off);
                const RoadPoint mid{(a.x + b.x) * 0.5 + o.x, (a.y + b.y) * 0.5 + o.y};
                const double z = x.pave + kPoleHigh - 0.62 - 0.35; // with a little sag
                x.add(x.at(mid, angleOf({b.x - a.x, b.y - a.y}), l, 0.025, z, z + 0.025, "wire"), RoadTint::Wire, false);
            }
        }
    }
}

bool roadInCarriageway(const RoadBuild& build, const MassingFoot& foot, double clear) {
    for (const MassingFoot& c : build.carriageway)
        if (massingFeetOverlap(foot, c, clear)) return true;
    return false;
}

bool roadFitsBay(const RoadBay& bay, const MassingFoot& v) {
    const double c = std::cos(rad(v.rot)), s = std::sin(rad(v.rot));
    const double bc = std::cos(rad(bay.foot.rot)), bs = std::sin(rad(bay.foot.rot));
    for (const double su : {-1.0, 1.0})
        for (const double sw : {-1.0, 1.0}) {
            const double px = v.x + su * v.hx * c - sw * v.hy * s - bay.foot.x;
            const double py = v.y + su * v.hx * s + sw * v.hy * c - bay.foot.y;
            const double u = px * bc + py * bs, w = -px * bs + py * bc;
            if (std::fabs(u) > bay.foot.hx + 1e-9 || std::fabs(w) > bay.foot.hy + 1e-9) return false;
        }
    return true;
}

bool importRoadDoors(std::string_view csv, double clear, std::vector<MassingFoot>& out, std::string* error) {
    using namespace detail;
    std::vector<Record> recs;
    if (!splitCsv(csv, recs, error)) return false;
    std::vector<std::string> head;
    for (const Record& rec : recs) {
        if (isComment(rec)) continue;
        if (head.empty()) {
            for (const std::string& h : rec.fields) head.emplace_back(trim(h));
            continue;
        }
        auto col = [&](const char* name) -> std::string_view {
            for (std::size_t i = 0; i < head.size() && i < rec.fields.size(); ++i)
                if (head[i] == name) return trim(rec.fields[i]);
            return {};
        };
        if (col("kind") != "door") continue;
        double width = 0.0;
        if (!parseNumber(col("width_m"), width) || width <= 0.0) return fail(error, rec.line, "a door needs a width");
        const std::string pts(col("pts"));
        double v[6] = {0, 0, 0, 0, 0, 0};
        int k = 0;
        std::size_t p = 0;
        while (k < 6) {
            const std::size_t q = pts.find_first_of(":;", p);
            if (!parseNumber(std::string_view(pts).substr(p, q == std::string::npos ? std::string::npos : q - p), v[k]))
                return fail(error, rec.line, "a door's pts are \"x:y:z;x:y:z\"");
            ++k;
            if (q == std::string::npos) break;
            p = q + 1;
        }
        if (k != 6) return fail(error, rec.line, "a door's pts are \"x:y:z;x:y:z\"");
        MassingFoot f;
        f.x = (v[0] + v[3]) * 0.5;
        f.y = (v[1] + v[4]) * 0.5;
        f.hx = std::hypot(v[3] - v[0], v[4] - v[1]) * 0.5 + clear;
        f.hy = width * 0.5 + clear;
        f.rot = deg(std::atan2(v[4] - v[1], v[3] - v[0]));
        out.push_back(f);
    }
    return true;
}

RoadParkCounts roadParkVehicles(const RoadBuild& build, const std::vector<MassingFoot>& doors,
                                const std::vector<MassingFoot>* taken, std::uint32_t seed, std::vector<RoadVehicle>& out) {
    RoadParkCounts counts;
    static const RoadVehicleKind kinds[10] = {RoadVehicleKind::Kei,  RoadVehicleKind::Kei,      RoadVehicleKind::Kei,
                                              RoadVehicleKind::Sedan, RoadVehicleKind::Sedan,   RoadVehicleKind::Sedan,
                                              RoadVehicleKind::Taxi,  RoadVehicleKind::Van,     RoadVehicleKind::Van,
                                              RoadVehicleKind::KeiTruck};
    static const RoadTint bodies[7] = {RoadTint::CarWhite, RoadTint::CarSilver, RoadTint::CarBlack, RoadTint::CarWhite,
                                       RoadTint::CarBlue,  RoadTint::CarRed,    RoadTint::CarCream};
    for (const RoadBay& bay : build.bays) {
        ++counts.bays;
        const std::uint32_t h = hashText(seed, bay.id);
        if (h % 4 == 0) {
            ++counts.empty;
            continue;
        }
        RoadVehicle v;
        v.id = bay.id + "-car";
        v.kind = kinds[(h >> 4) % 10];
        v.body = v.kind == RoadVehicleKind::Taxi ? RoadTint::TaxiYellow : bodies[(h >> 12) % 7];
        if (v.kind == RoadVehicleKind::KeiTruck) v.body = RoadTint::CarWhite;
        v.x = bay.foot.x;
        v.y = bay.foot.y;
        v.z = bay.z;
        v.rot = bay.heading;
        v.where = bay.id;
        const MassingFoot vf = roadVehicleFoot(v);
        bool ok = roadFitsBay(bay, vf) && !roadInCarriageway(build, vf, 0.0);
        for (const MassingFoot& k : build.keepOut)
            if (ok && massingFeetOverlap(vf, k, 0.0)) ok = false;
        for (const MassingFoot& d : doors)
            if (ok && massingFeetOverlap(vf, d, 0.0)) ok = false;
        if (taken)
            for (const MassingFoot& t : *taken)
                if (ok && massingFeetOverlap(vf, t, kMassingPropGap)) ok = false;
        if (!ok) {
            ++counts.blocked;
            continue;
        }
        out.push_back(std::move(v));
        ++counts.parked;
    }
    return counts;
}

namespace {

bool vehicleFree(const MassingFoot& vf, const RoadBuild* road, const std::vector<MassingFoot>& doors,
                 const std::vector<MassingFoot>* taken, const std::vector<RoadVehicle>& placed, std::size_t from) {
    if (road) {
        if (roadInCarriageway(*road, vf, kRoadKerbClear)) return false;
        for (const MassingFoot& k : road->keepOut)
            if (massingFeetOverlap(vf, k, 0.0)) return false;
        for (const MassingFoot& k : road->furniture)
            if (massingFeetOverlap(vf, k, kMassingPropGap)) return false;
    }
    for (const MassingFoot& d : doors)
        if (massingFeetOverlap(vf, d, 0.0)) return false;
    if (taken)
        for (const MassingFoot& t : *taken)
            if (massingFeetOverlap(vf, t, kMassingPropGap)) return false;
    for (std::size_t i = from; i < placed.size(); ++i)
        if (massingFeetOverlap(vf, roadVehicleFoot(placed[i]), 0.15)) return false;
    return true;
}

bool inBox(double x, double y, double x0, double y0, double x1, double y1) {
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

} // namespace

std::size_t roadTwoWheelers(const std::vector<MassingStreet>& lanes, const RoadTwoWheelOptions& opts, const RoadBuild* road,
                            const RoadSpec* spec, const MassingGround& ground, const std::vector<MassingFoot>& doors,
                            const std::vector<MassingFoot>* taken, std::vector<RoadVehicle>& out) {
    const std::size_t first = out.size();
    static const RoadTint bodies[5] = {RoadTint::CarWhite, RoadTint::CarBlack, RoadTint::CarRed, RoadTint::CarBlue,
                                       RoadTint::CarSilver};
    auto groundAt = [&](double px, double py) {
        return massingGroundAt(ground.grid, ground.samplesX, ground.samplesY, ground.spacing, ground.originX, ground.originY,
                               px, py);
    };
    for (const MassingStreet& st : lanes) {
        if (st.width > opts.laneUpTo + 1e-9 || st.points.size() != 2) continue;
        const MassingVec a = st.points[0], b = st.points[1];
        const double l = std::hypot(b.x - a.x, b.y - a.y);
        if (l < 8.0) continue;
        const Frame f = frameOf(a.x, a.y, b.x, b.y);
        for (const double side : {1.0, -1.0}) {
            int k = 0;
            for (double s = 4.0; s < l - 4.0; s += opts.spacing, ++k) {
                const std::uint32_t h = mix(hashText(opts.seed, st.id) ^ static_cast<std::uint32_t>(k * 2 + (side > 0 ? 1 : 0)));
                if (h % 3 != 0) continue;
                const int group = 1 + static_cast<int>((h >> 8) % 2);
                for (int gi = 0; gi < group; ++gi) {
                    RoadVehicle v;
                    v.kind = ((h >> (12 + gi)) & 1U) == 0 ? RoadVehicleKind::Scooter : RoadVehicleKind::Bicycle;
                    const MassingVec size = roadVehicleSize(v.kind);
                    const double t = side * (st.width * 0.5 - opts.edgeIn - size.y * 0.5);
                    const RoadPoint p = f.at(s + gi * 0.9, t);
                    if (!inBox(p.x, p.y, opts.x0, opts.y0, opts.x1, opts.y1)) continue;
                    char buf[48];
                    std::snprintf(buf, sizeof buf, "-tw%d-%d", k, gi);
                    v.id = st.id + buf;
                    v.x = p.x;
                    v.y = p.y;
                    v.z = groundAt(p.x, p.y);
                    v.rot = f.rot + (((h >> 20) & 1U) ? 180.0 : 0.0);
                    v.body = bodies[(h >> 24) % 5];
                    v.where = st.id;
                    if (!vehicleFree(roadVehicleFoot(v), road, doors, taken, out, first)) continue;
                    out.push_back(std::move(v));
                }
            }
        }
    }
    // Along a road's back of pavement, on the side its mouths are: bicycles nose to the kerb, in twos and threes.
    if (road && spec) {
        const Frame f = frameOf(spec->x0, spec->y0, spec->x1, spec->y1);
        const double len = std::hypot(spec->x1 - spec->x0, spec->y1 - spec->y0);
        double sideSign = 1.0;
        for (const RoadMouth& m : spec->mouths)
            if (m.stub <= 0.0) sideSign = m.left ? 1.0 : -1.0;
        const double gz = spec->ground + kRoadSurfaceLift + kRoadKerbRise;
        int k = 0;
        for (double s = 20.0; s < len - 20.0; s += 37.0, ++k) {
            const std::uint32_t h = mix(hashText(opts.seed, spec->id) ^ static_cast<std::uint32_t>(k));
            const int group = 2 + static_cast<int>(h % 2);
            for (int gi = 0; gi < group; ++gi) {
                RoadVehicle v;
                v.kind = ((h >> (4 + gi)) & 1U) == 0 ? RoadVehicleKind::Bicycle : RoadVehicleKind::Scooter;
                const MassingVec size = roadVehicleSize(v.kind);
                const RoadPoint p = f.at(s + gi * 0.75, sideSign * (spec->width * 0.5 - size.x * 0.5 - 0.25));
                char buf[48];
                std::snprintf(buf, sizeof buf, "-tw%d-%d", k, gi);
                v.id = spec->id + buf;
                v.x = p.x;
                v.y = p.y;
                v.z = gz;
                v.rot = angleOf(f.dir(0.0, -sideSign)); // nose to the kerb
                v.body = bodies[(h >> (8 + gi * 3)) % 5];
                v.where = spec->id;
                if (!vehicleFree(roadVehicleFoot(v), road, doors, taken, out, first)) continue;
                out.push_back(std::move(v));
            }
        }
    }
    return out.size() - first;
}

std::size_t roadLaneLamps(const std::vector<MassingStreet>& lanes, const RoadLaneLampOptions& opts,
                          const MassingGround& ground, const std::vector<RoadPoint>& existing,
                          const std::vector<MassingFoot>& doors, const std::vector<MassingFoot>* taken,
                          std::vector<MassingProp>& out) {
    const std::size_t first = out.size();
    for (const MassingStreet& st : lanes) {
        if (st.width > opts.laneUpTo + 1e-9 || st.points.size() < 2) continue;
        for (std::size_t seg = 0; seg + 1 < st.points.size(); ++seg) {
            const MassingVec a = st.points[seg], b = st.points[seg + 1];
            const double l = std::hypot(b.x - a.x, b.y - a.y);
            if (l < 4.0) continue;
            const Frame f = frameOf(a.x, a.y, b.x, b.y);
            for (const double side : {1.0, -1.0}) {
                for (double s = side > 0.0 ? opts.spacing * 0.5 : opts.spacing; s < l - 2.0; s += opts.spacing) {
                    const RoadPoint p = f.at(s, side * (st.width * 0.5 - opts.edgeIn));
                    if (!inBox(p.x, p.y, opts.x0, opts.y0, opts.x1, opts.y1)) continue;
                    bool near = false;
                    for (const RoadPoint& e : existing)
                        if (std::hypot(e.x - p.x, e.y - p.y) < opts.keep) near = true;
                    for (std::size_t i = first; i < out.size() && !near; ++i)
                        if (std::hypot(out[i].x - p.x, out[i].y - p.y) < opts.keep) near = true;
                    if (near) continue;
                    MassingProp lamp;
                    lamp.x = p.x;
                    lamp.y = p.y;
                    lamp.z = massingGroundAt(ground.grid, ground.samplesX, ground.samplesY, ground.spacing, ground.originX,
                                             ground.originY, p.x, p.y);
                    lamp.face = angleOf(f.dir(0.0, -side));
                    MassingFoot ft;
                    ft.x = p.x;
                    ft.y = p.y;
                    ft.hx = opts.footX * 0.5;
                    ft.hy = opts.footY * 0.5;
                    ft.rot = lamp.face;
                    bool hit = false;
                    for (const MassingFoot& d : doors)
                        if (massingFeetOverlap(ft, d, 0.0)) hit = true;
                    if (taken)
                        for (const MassingFoot& t : *taken)
                            if (!hit && massingFeetOverlap(ft, t, kMassingPropGap)) hit = true;
                    if (hit) continue;
                    out.push_back(lamp);
                }
            }
        }
    }
    return out.size() - first;
}

std::vector<std::size_t> roadClearedBoxes(const RoadBuild& build, const std::vector<MassingBox>& boxes, double margin) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        const MassingBox& b = boxes[i];
        if (b.kind != "deck" && b.kind != "pier") continue;
        if (!massingBuilt(massingRole(b))) continue;
        const MassingFoot f = massingBoxFoot(b);
        for (const MassingFoot& band : build.band)
            if (massingFeetOverlap(f, band, margin)) {
                out.push_back(i);
                break;
            }
    }
    return out;
}

// ---- Clashes: boxes and kerbs in the carriageway ----

namespace {

constexpr double kFaceDeep = 0.15;  // m: a kerb lying further than this (a kerb stone's width) into a slab is in the way
constexpr double kFaceLevels = 2.0; // m: a slab and a kerb further apart than this are on different levels (a deck)

// A point in a foot's own axes.
void localOf(const MassingFoot& f, double x, double y, double& u, double& w) {
    const double c = std::cos(rad(f.rot)), s = std::sin(rad(f.rot));
    const double dx = x - f.x, dy = y - f.y;
    u = dx * c + dy * s;
    w = -dx * s + dy * c;
}
RoadPoint planOf(const MassingFoot& f, double u, double w) {
    const double c = std::cos(rad(f.rot)), s = std::sin(rad(f.rot));
    return {f.x + u * c - w * s, f.y + u * s + w * c};
}
// A box's top over a plan point (its tilt from the centre; a sloped table row by its slope).
double topAt(const MassingBox& b, double x, double y) {
    double u = 0.0, w = 0.0;
    localOf(massingBoxFoot(b), x, y, u, w);
    if (b.sloped && b.sx > 0.0) return b.slopeFrom + (b.slopeTo - b.slopeFrom) * (u / b.sx + 0.5);
    return b.z1 + b.riseU * u + b.riseW * w;
}
double topMax(const MassingBox& b) {
    if (b.sloped) return std::max({b.z1, b.slopeFrom, b.slopeTo});
    return b.z1 + std::fabs(b.riseU) * b.sx * 0.5 + std::fabs(b.riseW) * b.sy * 0.5;
}
double topMin(const MassingBox& b) {
    if (b.sloped) return std::min({b.z1, b.slopeFrom, b.slopeTo});
    return b.z1 - std::fabs(b.riseU) * b.sx * 0.5 - std::fabs(b.riseW) * b.sy * 0.5;
}
double bottomMin(const MassingBox& b) {
    return b.z0 - std::fabs(b.riseU) * b.sx * 0.5 - std::fabs(b.riseW) * b.sy * 0.5;
}
bool slabSolid(const RoadSolid& s) { return s.collide && (s.box.kind == "asphalt" || s.box.kind == "fill"); }
bool faceSolid(const RoadSolid& s) { return s.box.kind == "kerb" || s.box.kind == "pavement"; }
double reach(const MassingFoot& f) { return std::hypot(f.hx, f.hy); }
// Whether a plan point lies in a slab's footprint (a cylinder's by its circle).
bool slabHas(const RoadSolid& s, double x, double y) {
    const MassingFoot f = massingBoxFoot(s.box);
    if (s.shape == RoadShape::Cylinder) return std::hypot(x - f.x, y - f.y) < std::min(f.hx, f.hy);
    double u = 0.0, w = 0.0;
    localOf(f, x, y, u, w);
    return std::fabs(u) < f.hx && std::fabs(w) < f.hy;
}
// The part of segment a-b inside the foot (Liang-Barsky in its axes): false when it misses.
bool clipSegment(const MassingFoot& f, RoadPoint a, RoadPoint b, double& t0, double& t1) {
    double ua = 0.0, wa = 0.0, ub = 0.0, wb = 0.0;
    localOf(f, a.x, a.y, ua, wa);
    localOf(f, b.x, b.y, ub, wb);
    const double du = ub - ua, dw = wb - wa;
    t0 = 0.0;
    t1 = 1.0;
    const double p[4] = {-du, du, -dw, dw};
    const double q[4] = {ua + f.hx, f.hx - ua, wa + f.hy, f.hy - wa};
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(p[i]) < 1e-12) {
            if (q[i] <= 0.0) return false;
            continue;
        }
        const double r = q[i] / p[i];
        if (p[i] < 0.0)
            t0 = std::max(t0, r);
        else
            t1 = std::min(t1, r);
        if (t0 >= t1) return false;
    }
    return t1 - t0 > 1e-9;
}
// The nearest point of a foot to a plan point.
RoadPoint nearestIn(const MassingFoot& f, double x, double y) {
    double u = 0.0, w = 0.0;
    localOf(f, x, y, u, w);
    return planOf(f, std::clamp(u, -f.hx, f.hx), std::clamp(w, -f.hy, f.hy));
}

// The faults of one kerb or pavement box: lane centre lines crossing it, and the first carriageway rectangle it lies in.
template <typename Emit>
void boxFaces(const MassingBox& piece, const std::vector<const RoadSolid*>& slabs,
              const std::vector<MassingFoot>& carriageway, const RoadGraph* graph, Emit&& emit) {
    const MassingFoot pf = massingBoxFoot(piece);
    const double pr = reach(pf);
    if (graph) {
        for (const RoadGraphLane& lane : graph->lanes) {
            bool hit = false;
            for (std::size_t i = 0; i + 1 < lane.pts.size() && !hit; ++i) {
                const RoadPoint a = lane.pts[i], b = lane.pts[i + 1];
                const double lx = b.x - a.x, ly = b.y - a.y, l2 = lx * lx + ly * ly;
                const double tt = l2 > 0.0 ? std::clamp(((pf.x - a.x) * lx + (pf.y - a.y) * ly) / l2, 0.0, 1.0) : 0.0;
                if (std::hypot(a.x + lx * tt - pf.x, a.y + ly * tt - pf.y) > pr) continue;
                double t0 = 0.0, t1 = 0.0;
                if (!clipSegment(pf, a, b, t0, t1)) continue;
                const double tm = (t0 + t1) * 0.5;
                const double mx = a.x + lx * tm, my = a.y + ly * tm;
                const double top = topAt(piece, mx, my);
                bool flush = false, apart = false;
                for (const RoadSolid* sl : slabs) {
                    if (!slabHas(*sl, mx, my)) continue;
                    const double d = std::fabs(topAt(sl->box, mx, my) - top);
                    if (d <= kRoadFaceMax) flush = true;
                    if (d > kFaceLevels) apart = true;
                }
                if (flush || apart) continue;
                hit = true;
                emit(mx, my, "lane " + lane.id);
            }
        }
    }
    // The carriageway rectangles it lies in, on its level (a slab under the spot within kFaceLevels, or none there).
    MassingFoot inner = pf;
    inner.hx = std::max(0.0, inner.hx - kFaceDeep * 0.5);
    inner.hy = std::max(0.0, inner.hy - kFaceDeep * 0.5);
    for (std::size_t c = 0; c < carriageway.size(); ++c) {
        const MassingFoot& cf = carriageway[c];
        if (std::hypot(cf.x - pf.x, cf.y - pf.y) > pr + reach(cf)) continue;
        MassingFoot ci = cf;
        ci.hx = std::max(0.0, ci.hx - kFaceDeep * 0.5);
        ci.hy = std::max(0.0, ci.hy - kFaceDeep * 0.5);
        if (!massingFeetOverlap(inner, ci, 0.0)) continue;
        // The spot: the piece's centre held inside the rectangle, then inside the piece.
        const RoadPoint q = nearestIn(cf, pf.x, pf.y);
        const RoadPoint at = nearestIn(pf, q.x, q.y);
        const double top = topAt(piece, at.x, at.y);
        bool level = false, other = false;
        for (const RoadSolid* sl : slabs) {
            if (!slabHas(*sl, at.x, at.y)) continue;
            if (std::fabs(topAt(sl->box, at.x, at.y) - top) <= kFaceLevels)
                level = true;
            else
                other = true;
        }
        if (other && !level) continue;
        char buf[24];
        std::snprintf(buf, sizeof buf, "carriageway %zu", c + 1);
        emit(at.x, at.y, std::string(buf));
        break;
    }
}

// Steps: two kerb or pavement pieces (solids indices) overlapping by more than kFaceDeep whose tops differ by more
// than kRoadFaceMax (but by no more than kFaceLevels) where they overlap; emit(a, b, point). Sorted by x for a sweep.
template <typename Emit>
void faceSteps(const RoadBuild& build, std::vector<std::size_t> pieces, Emit&& emit) {
    std::sort(pieces.begin(), pieces.end(),
              [&](std::size_t a, std::size_t b) { return build.solids[a].box.cx < build.solids[b].box.cx; });
    double longest = 0.0;
    for (const std::size_t i : pieces) longest = std::max(longest, reach(massingBoxFoot(build.solids[i].box)));
    for (std::size_t ia = 0; ia < pieces.size(); ++ia) {
        const MassingBox& a = build.solids[pieces[ia]].box;
        MassingFoot fa = massingBoxFoot(a);
        const double ra = reach(fa);
        fa.hx = std::max(0.0, fa.hx - kFaceDeep * 0.5);
        fa.hy = std::max(0.0, fa.hy - kFaceDeep * 0.5);
        for (std::size_t ib = ia + 1; ib < pieces.size(); ++ib) {
            const MassingBox& b = build.solids[pieces[ib]].box;
            if (b.cx - a.cx > ra + longest) break;
            MassingFoot fb = massingBoxFoot(b);
            if (std::hypot(fb.x - fa.x, fb.y - fa.y) > ra + reach(fb)) continue;
            fb.hx = std::max(0.0, fb.hx - kFaceDeep * 0.5);
            fb.hy = std::max(0.0, fb.hy - kFaceDeep * 0.5);
            if (!massingFeetOverlap(fa, fb, 0.0)) continue;
            const RoadPoint q = nearestIn(fb, fa.x, fa.y);
            const RoadPoint at = nearestIn(fa, q.x, q.y);
            const double d = std::fabs(topAt(a, at.x, at.y) - topAt(b, at.x, at.y));
            if (d <= kRoadFaceMax || d > kFaceLevels) continue;
            emit(pieces[ia], pieces[ib], at);
        }
    }
}

std::vector<const RoadSolid*> slabsOf(const RoadBuild& build) {
    std::vector<const RoadSolid*> out;
    for (const RoadSolid& s : build.solids)
        if (slabSolid(s)) out.push_back(&s);
    return out;
}

} // namespace

std::vector<std::size_t> roadBoxClashes(const RoadBuild& build, const std::vector<MassingBox>& boxes) {
    const std::vector<const RoadSolid*> slabs = slabsOf(build);
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        const MassingBox& b = boxes[i];
        if (!massingBuilt(massingRole(b))) continue;
        MassingFoot bf = massingBoxFoot(b);
        bf.hx = std::max(0.0, bf.hx - 0.01);
        bf.hy = std::max(0.0, bf.hy - 0.01);
        const double br = reach(bf), top = topMax(b), bottom = std::min(b.z0, bottomMin(b));
        for (const RoadSolid* sl : slabs) {
            const MassingFoot sf = massingBoxFoot(sl->box);
            if (std::hypot(sf.x - bf.x, sf.y - bf.y) > br + reach(sf)) continue;
            if (top <= topMin(sl->box) + kRoadFaceMax) continue;
            if (bottom >= topMax(sl->box) + kRoadBridgeClear) continue;
            if (sl->shape == RoadShape::Cylinder) {
                const RoadPoint q = nearestIn(bf, sf.x, sf.y);
                if (std::hypot(q.x - sf.x, q.y - sf.y) >= std::min(sf.hx, sf.hy)) continue;
            } else if (!massingFeetOverlap(bf, sf, 0.0)) {
                continue;
            }
            out.push_back(i);
            break;
        }
    }
    return out;
}

std::vector<RoadFace> roadKerbFaces(const RoadBuild& build, const RoadGraph* graph) {
    const std::vector<const RoadSolid*> slabs = slabsOf(build);
    std::vector<RoadFace> out;
    std::vector<std::size_t> pieces;
    for (std::size_t i = 0; i < build.solids.size(); ++i) {
        const RoadSolid& s = build.solids[i];
        if (!faceSolid(s)) continue;
        pieces.push_back(i);
        boxFaces(s.box, slabs, build.carriageway, graph, [&](double fx, double fy, std::string fault) {
            RoadFace f;
            f.piece = s.box.id;
            f.x = fx;
            f.y = fy;
            f.fault = std::move(fault);
            out.push_back(std::move(f));
        });
    }
    faceSteps(build, pieces, [&](std::size_t ia, std::size_t ib, RoadPoint at) {
        RoadFace f;
        f.piece = build.solids[ia].box.id;
        f.x = at.x;
        f.y = at.y;
        f.fault = "step " + build.solids[ib].box.id;
        out.push_back(std::move(f));
    });
    return out;
}

std::vector<std::size_t> roadKerbsInCarriageway(const RoadBuild& build, const RoadGraph* graph,
                                                const std::vector<MassingBox>& kerbs) {
    const std::vector<const RoadSolid*> slabs = slabsOf(build);
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < kerbs.size(); ++i) {
        bool hit = false;
        boxFaces(kerbs[i], slabs, build.carriageway, graph, [&](double, double, const std::string&) { hit = true; });
        if (hit) out.push_back(i);
    }
    return out;
}

// ---- Road networks ----

namespace {

constexpr double kNetPierSpacing = 30.0; // m
constexpr double kNetPierDia = 1.4;      // m
constexpr double kNetDeckDeep = 1.2;     // m: a deck's beam under its slabs
constexpr double kNetMaxDeep = 3.0;      // m: the deepest a slab grows over falling ground
constexpr double kNetSample = 5.0;       // m: the ground's samples along an edge
constexpr double kNetProfileTol = 0.1;   // m: a piece's line keeps this near the ground's samples
constexpr double kNetBandLift = 0.5;     // m: the most a cross-slope lifts the road over the ground at its crown
constexpr std::size_t kNetNotes = 12;    // notes kept of each kind

RoadPoint netSub(RoadPoint a, RoadPoint b) { return {a.x - b.x, a.y - b.y}; }
RoadPoint netAdd(RoadPoint a, RoadPoint b) { return {a.x + b.x, a.y + b.y}; }
RoadPoint netMul(RoadPoint a, double k) { return {a.x * k, a.y * k}; }
double netDot(RoadPoint a, RoadPoint b) { return a.x * b.x + a.y * b.y; }
double netCross(RoadPoint a, RoadPoint b) { return a.x * b.y - a.y * b.x; }
double netLen(RoadPoint a) { return std::hypot(a.x, a.y); }
RoadPoint netUnit(RoadPoint a) {
    const double l = netLen(a);
    return l > 0.0 ? RoadPoint{a.x / l, a.y / l} : RoadPoint{1.0, 0.0};
}
RoadPoint netLeft(RoadPoint d) { return {-d.y, d.x}; }
RoadPoint netDir(double a) { return {std::cos(a), std::sin(a)}; }
double netWrap(double a) {
    while (a > kPi) a -= 2.0 * kPi;
    while (a <= -kPi) a += 2.0 * kPi;
    return a;
}

// An edge leaving a node.
struct NetArm {
    std::size_t edge = 0;
    bool atA = true;       // the edge's a end is this node
    RoadPoint d{1.0, 0.0}; // out along the edge
    double ang = 0.0, hc = 0.0, w2 = 0.0, pav = 0.0;
    double trimL = 0.0, trimR = 0.0; // the arm's left and right kerbs start this far out
    double sc = 0.0;                 // its crowned carriageway starts here; the junction's fill covers it nearer
    char phase = 'A';
};
struct NetCorner {
    std::size_t i = 0, j = 0; // arms: the corner lies on i's left and j's right
    bool outer = false;       // the outside of a bend: a ring round the node
    RoadPoint c{}, t1{}, t2{};
};
struct NetNode {
    RoadPoint p{};
    double z = 0.0;
    std::vector<NetArm> arms;
    std::vector<NetCorner> corners;
    bool signal = false;
    std::string id;
};

void netNote(RoadNetReport& rep, std::size_t count, const char* line) {
    if (count <= kNetNotes) rep.notes.emplace_back(line);
}

// Douglas-Peucker over (s, z): marks the points the simplified line keeps.
void netSimplify(const std::vector<std::pair<double, double>>& p, std::size_t i0, std::size_t i1, double tol,
                 std::vector<char>& keep) {
    if (i1 <= i0 + 1) return;
    const double ds = p[i1].first - p[i0].first;
    double worst = 0.0;
    std::size_t at = i0;
    for (std::size_t k = i0 + 1; k < i1; ++k) {
        const double zl = ds > 0.0 ? p[i0].second + (p[i1].second - p[i0].second) * (p[k].first - p[i0].first) / ds
                                   : p[i0].second;
        const double e = std::fabs(p[k].second - zl);
        if (e > worst) {
            worst = e;
            at = k;
        }
    }
    if (worst > tol) {
        keep[at] = 1;
        netSimplify(p, i0, at, tol, keep);
        netSimplify(p, at, i1, tol, keep);
    }
}

// A ring piece round a centre: radius r0 to r1 over the angles a..a+step (radians), its top `top`, `depth` deep.
MassingBox netRing(Ctx& x, RoadPoint c, double a, double step, double r0, double r1, double top, double depth,
                   const char* kind) {
    const RoadPoint u = netDir(a + step * 0.5);
    const double cs = std::cos(std::fabs(step) * 0.5);
    const double rm = (r0 * cs + r1) * 0.5;
    const double radial = r1 - r0 * cs;
    const double chord = 2.0 * r1 * std::sin(std::fabs(step) * 0.5) + 0.04;
    return x.at(netAdd(c, netMul(u, rm)), angleOf(u), radial, chord, top - depth, top, kind);
}

} // namespace

bool roadNetTable(std::string_view csv) {
    using namespace detail;
    std::vector<Record> recs;
    if (!splitCsv(csv, recs, nullptr)) return false;
    for (const Record& rec : recs) {
        if (isComment(rec)) continue;
        bool pts = false, ground = false;
        for (const std::string& h : rec.fields) {
            const std::string_view t = trim(h);
            pts = pts || t == "pts";
            ground = ground || t == "ground_z";
        }
        return pts && !ground;
    }
    return false;
}

bool importRoadNet(std::string_view csv, std::vector<RoadNetStreet>& out, std::string* error) {
    using namespace detail;
    out.clear();
    std::vector<Record> recs;
    if (!splitCsv(csv, recs, error)) return false;
    std::vector<std::string> head;
    for (const Record& rec : recs) {
        if (isComment(rec)) continue;
        if (head.empty()) {
            for (const std::string& h : rec.fields) head.emplace_back(trim(h));
            continue;
        }
        auto col = [&](const char* name) -> std::string_view {
            for (std::size_t i = 0; i < head.size() && i < rec.fields.size(); ++i)
                if (head[i] == name) return trim(rec.fields[i]);
            return {};
        };
        auto num = [&](const char* name, double& v, bool need) {
            const std::string_view s = col(name);
            if (s.empty()) return !need;
            return parseNumber(s, v);
        };
        auto flag = [&](const char* name) {
            const std::string_view s = col(name);
            return s == "1" || s == "true" || s == "True";
        };
        RoadNetStreet r;
        r.id = std::string(col("id"));
        r.kind = std::string(col("kind"));
        if (!validId(r.id)) return fail(error, rec.line, "a street needs an id (A-Z, 0-9, '_', '-')");
        if (!validKind(r.kind)) return fail(error, rec.line, r.id + ": a street needs a kind");
        if (!num("width_m", r.width, true) || !num("pavement_m", r.pavement, true) || !num("speed_kmh", r.speedKmh, false))
            return fail(error, rec.line, r.id + ": a number column is missing or not a number");
        if (!col("deck_z").empty()) {
            if (!parseNumber(col("deck_z"), r.deckZ)) return fail(error, rec.line, r.id + ": deck_z is not a number");
            r.deck = true;
        }
        r.oneWay = flag("one_way");
        r.park = flag("parking");
        {
            const std::string_view ps = col("poles");
            if (ps != "" && ps != "L" && ps != "R" && ps != "LR" && ps != "RL")
                return fail(error, rec.line, r.id + ": poles is L, R or LR");
            r.poleLeft = ps.find('L') != std::string_view::npos;
            r.poleRight = ps.find('R') != std::string_view::npos;
        }
        if (!num("mouth_back_m", r.mouthBack, false) || !num("mouth_z", r.mouthZ, false))
            return fail(error, rec.line, r.id + ": mouth_back_m or mouth_z is not a number");
        {
            const std::string_view ms = col("mouths");
            std::size_t p0 = 0;
            while (p0 < ms.size()) {
                std::size_t q = ms.find(';', p0);
                if (q == std::string_view::npos) q = ms.size();
                std::vector<std::string_view> f;
                std::size_t a = p0;
                while (a <= q) {
                    std::size_t b = ms.find('@', a);
                    if (b == std::string_view::npos || b > q) b = q;
                    f.push_back(trim(ms.substr(a, b - a)));
                    a = b + 1;
                }
                RoadMouth m;
                if (f.size() != 4 || !validId(f[0]) || !parseNumber(f[1], m.at) || !parseNumber(f[2], m.width) ||
                    (f[3] != "L" && f[3] != "R") || m.width <= 0.0)
                    return fail(error, rec.line, r.id + ": a mouth is \"lane@at@width@L|R\"");
                m.lane = std::string(f[0]);
                m.left = f[3] == "L";
                r.mouths.push_back(std::move(m));
                p0 = q + 1;
            }
        }
        const std::string_view pts = col("pts");
        std::size_t p = 0;
        while (p < pts.size()) {
            std::size_t q = pts.find(';', p);
            if (q == std::string_view::npos) q = pts.size();
            const std::string_view item = pts.substr(p, q - p);
            const std::size_t c1 = item.find(':');
            if (c1 == std::string_view::npos) return fail(error, rec.line, r.id + ": pts must be \"x:y;x:y;...\"");
            const std::size_t c2 = item.find(':', c1 + 1);
            RoadPoint pt;
            const std::string_view ys =
                c2 == std::string_view::npos ? item.substr(c1 + 1) : item.substr(c1 + 1, c2 - c1 - 1);
            if (!parseNumber(item.substr(0, c1), pt.x) || !parseNumber(ys, pt.y))
                return fail(error, rec.line, r.id + ": pts must be \"x:y;x:y;...\"");
            r.pts.push_back(pt);
            p = q + 1;
        }
        if (r.pts.size() < 2) return fail(error, rec.line, r.id + ": a street needs two points or more");
        for (std::size_t i = 1; i < r.pts.size(); ++i)
            if (netLen(netSub(r.pts[i], r.pts[i - 1])) < 1.0)
                return fail(error, rec.line, r.id + ": two of its points are less than 1 m apart");
        const double carriage = r.width - 2.0 * r.pavement;
        if (r.pavement <= 0.0 || carriage < (r.oneWay ? 1.0 : 2.0) * 3.0 - 1e-6)
            return fail(error, rec.line, r.id + ": it needs pavements and a carriageway holding its lanes (3.0 m each)");
        for (const RoadNetStreet& o : out)
            if (o.id == r.id) return fail(error, rec.line, r.id + ": the id is used twice");
        out.push_back(std::move(r));
    }
    if (head.empty()) return fail(error, 1, "the table has no header row");
    return true;
}

MassingStreet roadNetStreet(const RoadNetStreet& s) {
    MassingStreet m;
    m.id = s.id;
    m.kind = s.kind;
    m.width = s.width;
    m.pavement = s.pavement;
    for (const RoadPoint& p : s.pts) m.points.push_back({p.x, p.y, 0.0});
    return m;
}

namespace {

// A network edge's utility poles (on its street's pole sides, wired pole to pole) and its signs: a speed limit past
// each junction on the way's left, no entry at a one-way street's exit, a stop sign at each lane mouth. All of it is
// laid as instances. `fwd`: the edge runs the way its street's points do. `pz(s)`: the pavement's top at s.
template <class PaveZ>
void netPolesAndSigns(Ctx& x, const Frame& f, const RoadNetStreet& st, bool fwd, double L, double sA, double sB,
                      bool juncA, bool juncB, const std::vector<SideSpan>& spansL, const std::vector<SideSpan>& spansR,
                      const std::vector<std::pair<double, bool>>& mouthSigns, const PaveZ& pz) {
    RoadBuild& out = x.out;
    const double hc = x.hc, w2 = x.w2;
    x.inst = true;
    auto clear = [&](RoadPoint p, double half) {
        MassingFoot ft;
        ft.x = p.x;
        ft.y = p.y;
        ft.hx = ft.hy = half;
        for (const MassingFoot& o : out.furniture) {
            if (std::fabs(o.x - p.x) > 4.0 || std::fabs(o.y - p.y) > 4.0) continue;
            if (massingFeetOverlap(ft, o, kMassingPropGap)) return false;
        }
        return true;
    };
    auto backOf = [&](const std::vector<SideSpan>& spans, double s, double back) {
        return spanAt(spans, s) == Span::Bay ? hc + kRoadBayDeep + back : hc + back;
    };
    // Utility poles at the back of the pavement, a crossarm each, three wires to the next.
    for (int side = 0; side < 2 && !st.deck; ++side) {
        const bool want = (side == 0) == fwd ? st.poleLeft : st.poleRight;
        if (!want) continue;
        const double sg = side == 0 ? 1.0 : -1.0;
        const std::vector<SideSpan>& spans = side == 0 ? spansL : spansR;
        const double tp = w2 - 0.6;
        if (tp - 0.3 < hc + 0.5) continue; // too narrow a pavement
        RoadPoint prev{};
        double prevS = -1e9, prevZ = 0.0;
        for (double s = kRoadPoleSpacing * 0.5; s < L - 3.0; s += kRoadPoleSpacing) {
            for (const double nudge : {0.0, 1.5, -1.5, 3.0, -3.0, 4.5, -4.5}) {
                const double sp = s + nudge;
                if (sp < 3.0 || sp > L - 3.0 || !spotFree(spans, sp, 1.0)) continue;
                if (spanAt(spans, sp) == Span::Bay && hc + kRoadBayDeep + 0.3 > tp - 0.15) continue;
                const RoadPoint p = f.at(sp, sg * tp);
                if (!clear(p, 0.2)) continue;
                const double z = pz(sp);
                x.post(p, 0.3, z, kPoleHigh, RoadTint::PoleConcrete, "utilitypole");
                x.add(x.at(p, f.rot + 90.0, 1.6, 0.1, z + kPoleHigh - 0.75, z + kPoleHigh - 0.65, "crossarm"),
                      RoadTint::Steel, false);
                ++out.counts.poles;
                if (sp - prevS < 2.5 * kRoadPoleSpacing) {
                    const double l = std::hypot(p.x - prev.x, p.y - prev.y);
                    const double za = prevZ + kPoleHigh - 0.97, zb = z + kPoleHigh - 0.97; // with a little sag
                    for (const double off : {-0.6, 0.0, 0.6}) {
                        const RoadPoint o = f.dir(0.0, off);
                        const RoadPoint mid{(prev.x + p.x) * 0.5 + o.x, (prev.y + p.y) * 0.5 + o.y};
                        const double zm = (za + zb) * 0.5;
                        MassingBox w = x.at(mid, f.rot, l, 0.025, zm, zm + 0.025, "wire");
                        w.riseU = (zb - za) / std::max(1e-6, l);
                        x.add(w, RoadTint::Wire, false);
                    }
                }
                prev = p;
                prevS = sp;
                prevZ = z;
                break;
            }
        }
    }
    // A sign on a side past s (searching away from the junction by `dir`), facing `face`.
    auto signNear = [&](double s, double dir, bool plusT, RoadPoint face, const char* what, RoadTint tint, bool disc) {
        const std::vector<SideSpan>& spans = plusT ? spansL : spansR;
        for (const double step : {0.0, 2.0, 4.0, 6.0, 8.0}) {
            const double ss = s + dir * step;
            if (ss < 3.0 || ss > L - 3.0 || !spotFree(spans, ss, 1.0)) continue;
            const double back = backOf(spans, ss, 1.0);
            if (back > w2 - 0.3) continue;
            const RoadPoint p = f.at(ss, plusT ? back : -back);
            if (!clear(p, 0.2)) continue;
            x.sign(p, face, pz(ss), what, tint, disc);
            return;
        }
    };
    const bool runs = !st.oneWay || fwd, against = !st.oneWay || !fwd; // traffic along +s, along -s
    if (sB - sA > 20.0) {
        // Left-hand traffic: the way along +s keeps to +t; its signs face -s.
        if (juncA && runs) signNear(sA + 6.0, 1.0, true, f.dir(-1.0, 0.0), "speed", RoadTint::SignWhite, true);
        if (juncB && against) signNear(sB - 6.0, -1.0, false, f.dir(1.0, 0.0), "speed", RoadTint::SignWhite, true);
        // A one-way street's exit, on the left of traffic turning in the wrong way, facing it.
        if (st.oneWay && fwd && juncB)
            signNear(sB - 4.0, -1.0, false, f.dir(1.0, 0.0), "no_entry", RoadTint::SignRed, true);
        if (st.oneWay && !fwd && juncA)
            signNear(sA + 4.0, 1.0, true, f.dir(-1.0, 0.0), "no_entry", RoadTint::SignRed, true);
    }
    // Stop signs at the lane mouths, on the lane traffic's left, facing it.
    for (const auto& [ms, plusT] : mouthSigns) {
        const std::vector<SideSpan>& spans = plusT ? spansL : spansR;
        const double back = backOf(spans, ms, 1.2);
        if (back > w2 - 0.3) continue;
        const RoadPoint p = f.at(ms, plusT ? back : -back);
        if (!clear(p, 0.2)) continue;
        x.sign(p, f.dir(0.0, plusT ? 1.0 : -1.0), pz(ms), "stop", RoadTint::SignRed, false);
    }
    x.inst = false;
}

} // namespace

void roadNetBuild(const std::vector<RoadNetStreet>& streets, const MassingGround& ground, RoadBuild& out,
                  RoadNetReport* report) {
    RoadNetReport rep;
    const std::size_t solids0 = out.solids.size();
    auto groundAt = [&](RoadPoint p) {
        return massingGroundAt(ground.grid, ground.samplesX, ground.samplesY, ground.spacing, ground.originX,
                               ground.originY, p.x, p.y);
    };
    std::vector<MassingStreet> ms;
    ms.reserve(streets.size());
    for (const RoadNetStreet& s : streets) ms.push_back(roadNetStreet(s));
    const RoadNetTopo net = roadGraphNet(ms, kRoadGraphSnap);
    rep.edges = net.edges.size();
    char note[192];

    // Nodes and their arms.
    std::vector<NetNode> nodes(net.nodes.size());
    int jcount = 0;
    for (std::size_t n = 0; n < nodes.size(); ++n) {
        NetNode& nd = nodes[n];
        nd.p = net.nodes[n];
        bool deck = false;
        for (const std::size_t s : net.streetsAt[n])
            if (streets[s].deck) {
                nd.z = deck ? std::max(nd.z, streets[s].deckZ) : streets[s].deckZ;
                deck = true;
            }
        if (!deck) {
            // The ground's highest over the node's band, so a level fill on a cross-slope stays on top of it.
            double rn = 0.0;
            for (const std::size_t s : net.streetsAt[n]) rn = std::max(rn, streets[s].width * 0.5 - 0.3);
            const double g0 = groundAt(nd.p);
            nd.z = g0;
            for (int i = 0; i < 8; ++i) {
                const double a = kPi * 0.25 * i;
                nd.z = std::max(nd.z, groundAt({nd.p.x + rn * std::cos(a), nd.p.y + rn * std::sin(a)}) - kRoadKerbRise);
            }
            nd.z = std::min(nd.z, g0 + kNetBandLift); // a cliff beside it is reported as pokes, not climbed
        }
        if (net.streetsAt[n].size() >= 2) {
            char buf[24];
            std::snprintf(buf, sizeof buf, "JN-%04d", ++jcount);
            nd.id = buf;
        }
    }
    for (std::size_t e = 0; e < net.edges.size(); ++e) {
        const RoadNetTopo::Edge& ed = net.edges[e];
        const RoadNetStreet& st = streets[ed.street];
        for (int end = 0; end < 2; ++end) {
            NetArm arm;
            arm.edge = e;
            arm.atA = end == 0;
            const std::size_t here = end == 0 ? ed.a : ed.b, there = end == 0 ? ed.b : ed.a;
            arm.d = netUnit(netSub(nodes[there].p, nodes[here].p));
            arm.ang = std::atan2(arm.d.y, arm.d.x);
            arm.hc = (st.width - 2.0 * st.pavement) * 0.5;
            arm.w2 = st.width * 0.5;
            arm.pav = st.pavement;
            nodes[here].arms.push_back(arm);
        }
    }
    const double R = kRoadCornerRadius;
    for (NetNode& nd : nodes) {
        std::vector<NetArm>& arms = nd.arms;
        if (arms.empty()) continue;
        ++rep.nodes;
        // Phases by the first arm's axis (the first edge at the node, as roadGraphJunctions takes it).
        const RoadPoint axis = arms.front().d;
        for (NetArm& a : arms) a.phase = std::fabs(netDot(a.d, axis)) >= std::cos(kPi / 4.0) ? 'A' : 'B';
        std::sort(arms.begin(), arms.end(), [](const NetArm& a, const NetArm& b) { return a.ang < b.ang; });
        const std::size_t k = arms.size();
        if (k == 1) {
            ++rep.ends;
            continue;
        }
        nd.signal = k >= 3;
        if (nd.signal) ++rep.junctions;
        bool bend = false;
        for (std::size_t i = 0; i < k; ++i) {
            const std::size_t j = (i + 1) % k;
            NetArm& ai = arms[i];
            NetArm& aj = arms[j];
            double th = aj.ang - ai.ang;
            while (th <= 1e-9) th += 2.0 * kPi;
            const RoadPoint ni = netLeft(ai.d), nj = netLeft(aj.d);
            if (th < kPi - 1e-3) {
                // The kerb circle tangent to i's left kerb and j's right kerb.
                const RoadPoint rhs = netAdd(netMul(nj, -(aj.hc + R)), netMul(ni, -(ai.hc + R)));
                const double det = -netCross(ai.d, aj.d);
                if (std::fabs(det) < 1e-9) continue;
                const double s1 = netCross(rhs, netMul(aj.d, -1.0)) / det;
                const double s2 = netCross(ai.d, rhs) / det;
                ai.trimL = std::max(ai.trimL, s1);
                aj.trimR = std::max(aj.trimR, s2);
                NetCorner c;
                c.i = i;
                c.j = j;
                c.c = netAdd(nd.p, netAdd(netMul(ai.d, s1), netMul(ni, ai.hc + R)));
                c.t1 = netAdd(nd.p, netAdd(netMul(ai.d, s1), netMul(ni, ai.hc)));
                c.t2 = netAdd(nd.p, netSub(netMul(aj.d, s2), netMul(nj, aj.hc)));
                nd.corners.push_back(c);
            } else if (th > kPi + 1e-3) {
                NetCorner c;
                c.i = i;
                c.j = j;
                c.outer = true;
                c.c = nd.p;
                nd.corners.push_back(c);
                bend = true;
            }
        }
        if (k == 2 && bend) ++rep.bends;
        for (NetArm& a : arms) a.sc = std::max(a.trimL, a.trimR);
    }

    // Each node of two arms or more: the level fill, its corners, a junction's record.
    for (std::size_t n = 0; n < nodes.size(); ++n) {
        const NetNode& nd = nodes[n];
        if (nd.arms.size() < 2) continue;
        RoadSpec spec;
        char buf[32];
        std::snprintf(buf, sizeof buf, "NODE-%04zu", n + 1);
        spec.id = nd.id.empty() ? std::string(buf) : nd.id;
        Ctx x(spec, out);
        const double g = nd.z + kRoadSurfaceLift, pv = g + kRoadKerbRise;
        double hcMax = 0.0, hcMin = 1e9, scMax = 0.0;
        for (const NetArm& a : nd.arms) {
            hcMax = std::max(hcMax, a.hc);
            hcMin = std::min(hcMin, a.hc);
            scMax = std::max(scMax, a.sc);
        }
        const double zf = g + kRoadCrossfall * hcMax * 0.5;
        for (const NetArm& a : nd.arms)
            rep.maxStep = std::max(rep.maxStep, std::max(zf - g, g + kRoadCrossfall * a.hc - zf));
        for (const NetArm& a : nd.arms) {
            if (a.sc <= 0.01) continue;
            const Frame fa = frameOf(nd.p.x, nd.p.y, nd.p.x + a.d.x, nd.p.y + a.d.y);
            x.add(x.box(fa, 0.0, a.sc, -a.hc, a.hc, zf, kRoadSlabDepth, 0.0, 0.0, "fill"), RoadTint::Asphalt, true);
            const MassingFoot ft = footOf(x.box(fa, 0.0, a.sc, -a.hc, a.hc, 0, 0, 0, 0, "foot"));
            out.carriageway.push_back(ft);
            out.band.push_back(ft);
        }
        // A disc over the node as wide as its narrowest arm (a bend's outer ring brings its own fill out to its kerb).
        const double disc = hcMin;
        x.add(x.at(nd.p, 0.0, 2.0 * disc, 2.0 * disc, zf - kRoadSlabDepth, zf, "fill"), RoadTint::Asphalt, true,
              RoadShape::Cylinder);
        for (const NetCorner& c : nd.corners) {
            const NetArm& ai = nd.arms[c.i];
            const NetArm& aj = nd.arms[c.j];
            const RoadPoint ni = netLeft(ai.d), nj = netLeft(aj.d);
            if (!c.outer) {
                const double a0 = std::atan2(-ni.y, -ni.x);
                const double sweep = netWrap(std::atan2(nj.y, nj.x) - a0);
                // The fill out to the line through the kerb arc's nearest point to the node (parallel to its chord).
                const RoadPoint ch = netSub(c.t2, c.t1);
                const double lc = netLen(ch);
                if (lc > 0.05) {
                    const RoadPoint ux = netUnit(ch);
                    RoadPoint pp = netLeft(ux);
                    const RoadPoint v = netSub(nd.p, c.t1);
                    double wd = netDot(v, pp);
                    if (wd < 0.0) {
                        pp = netMul(pp, -1.0);
                        wd = -wd;
                    }
                    const double sag = R * (1.0 - std::cos(std::fabs(sweep) * 0.5));
                    wd -= sag;
                    const double pn = netDot(v, ux);
                    const double u0 = std::min(0.0, pn), u1 = std::max(lc, pn);
                    const RoadPoint mid =
                        netAdd(c.t1, netAdd(netMul(ux, (u0 + u1) * 0.5), netMul(pp, sag + wd * 0.5)));
                    if (wd > 0.05) {
                        const MassingBox fb = x.at(mid, angleOf(ux), u1 - u0, wd, zf - kRoadSlabDepth, zf, "fill");
                        x.add(fb, RoadTint::Asphalt, true);
                        out.carriageway.push_back(footOf(fb));
                        out.band.push_back(footOf(fb));
                    }
                }
                // The kerb and the pavement round the corner.
                const int steps = std::max(2, static_cast<int>(std::ceil(std::fabs(sweep) / (kPi / 9.0))));
                const double step = sweep / steps;
                for (int s = 0; s < steps; ++s) {
                    const double a = a0 + step * s;
                    x.add(netRing(x, c.c, a, step, R - kRoadKerbWide, R, pv, kRoadSlabDepth + 0.15, "kerb"), RoadTint::Kerb,
                          true);
                    // The pavement from the kerb back to where the ray from the centre leaves the two streets' bands.
                    const RoadPoint u = netDir(a + step * 0.5);
                    const double ui = -netDot(ni, u), uj = netDot(nj, u);
                    double rb = R - kRoadKerbWide - 0.05;
                    if (ui > 1e-6) rb = std::min(rb, (R - ai.pav) / ui);
                    if (uj > 1e-6) rb = std::min(rb, (R - aj.pav) / uj);
                    const MassingBox pb =
                        netRing(x, c.c, a, step, std::max(0.0, rb), R - kRoadKerbWide, pv, kRoadSlabDepth, "pavement");
                    x.add(pb, RoadTint::Pavement, true);
                    out.band.push_back(footOf(pb));
                }
            } else {
                // The outside of a bend: a ring of kerb and pavement round the node, its kerb line going from arm i's
                // (at the start of the sweep) to arm j's (at its end) so neither arm's kerb jogs where it meets the ring,
                // and the level fill out to it.
                const double a0 = std::atan2(ni.y, ni.x);
                double sweep = std::atan2(-nj.y, -nj.x) - a0;
                while (sweep < 0.0) sweep += 2.0 * kPi;
                const int steps = std::max(1, static_cast<int>(std::ceil(sweep / (kPi / 9.0))));
                const double step = sweep / steps;
                for (int s = 0; s < steps; ++s) {
                    const double a = a0 + step * s;
                    const double k = steps > 1 ? static_cast<double>(s) / (steps - 1) : 0.5;
                    const double hc = ai.hc + (aj.hc - ai.hc) * k, w2 = ai.w2 + (aj.w2 - ai.w2) * k;
                    const MassingBox fb = netRing(x, nd.p, a, step, 0.0, hc, zf, kRoadSlabDepth, "fill");
                    x.add(fb, RoadTint::Asphalt, true);
                    x.add(netRing(x, nd.p, a, step, hc, hc + kRoadKerbWide, pv, kRoadSlabDepth + 0.15, "kerb"),
                          RoadTint::Kerb, true);
                    const MassingBox pb = netRing(x, nd.p, a, step, hc + kRoadKerbWide, w2, pv, kRoadSlabDepth, "pavement");
                    x.add(pb, RoadTint::Pavement, true);
                    out.band.push_back(footOf(pb));
                }
            }
        }
        if (nd.signal) {
            RoadJunction rj;
            rj.id = nd.id;
            rj.x = nd.p.x;
            rj.y = nd.p.y;
            const std::vector<std::size_t>& at = net.streetsAt[n];
            rj.road = streets[at[0]].id;
            rj.lane = at.size() > 1 ? streets[at[1]].id : std::string();
            rj.box.x = nd.p.x;
            rj.box.y = nd.p.y;
            rj.box.hx = rj.box.hy = scMax;
            out.keepOut.push_back(rj.box);
            out.junctions.push_back(rj);
            ++out.counts.junctions;
        }
    }

    // The edges.
    std::vector<int> edgeCount(streets.size(), 0);
    for (std::size_t e = 0; e < net.edges.size(); ++e) {
        const RoadNetTopo::Edge& ed = net.edges[e];
        const RoadNetStreet& st = streets[ed.street];
        const NetNode& na = nodes[ed.a];
        const NetNode& nb = nodes[ed.b];
        const NetArm* pa = nullptr;
        const NetArm* pb = nullptr;
        for (const NetArm& a : na.arms)
            if (a.edge == e && a.atA) pa = &a;
        for (const NetArm& a : nb.arms)
            if (a.edge == e && !a.atA) pb = &a;
        if (!pa || !pb) continue;
        const NetArm& A = *pa;
        const NetArm& B = *pb;
        RoadSpec spec;
        {
            char buf[16];
            std::snprintf(buf, sizeof buf, "-E%d", ++edgeCount[ed.street]);
            spec.id = st.id + buf;
        }
        spec.mouthBack = st.mouthBack;
        spec.mouthZ = st.mouthZ;
        Ctx x(spec, out);
        const Frame f = frameOf(na.p.x, na.p.y, nb.p.x, nb.p.y);
        const double L = netLen(netSub(nb.p, na.p));
        const double hc = A.hc, w2 = A.w2;
        x.hc = hc;
        x.w2 = w2;
        x.len = L;
        double scA = na.arms.size() >= 2 ? A.sc : 0.0, scB = nb.arms.size() >= 2 ? B.sc : 0.0;
        if (scA + scB > L - 0.5) {
            const double k = std::max(0.0, L - 0.5) / std::max(1e-6, scA + scB);
            scA *= k;
            scB *= k;
            ++rep.tight;
            std::snprintf(note, sizeof note, "%s: its junctions' corners overlap (%.0f m)", spec.id.c_str(), L);
            netNote(rep, rep.tight, note);
        }
        const double sA = scA, sB = L - scB;
        // Zebras where there is room for them.
        const double zebraRun = kRoadZebraOff + kRoadZebraWide + kRoadStopBack + kRoadStopLine + 1.0;
        bool zA = na.signal, zB = nb.signal;
        if ((zA && zB && sB - sA < 2.0 * zebraRun) || ((zA || zB) && sB - sA < zebraRun)) {
            zA = zB = false;
            ++rep.tight;
            std::snprintf(note, sizeof note, "%s: too short for its zebras (%.0f m)", spec.id.c_str(), L);
            netNote(rep, rep.tight, note);
        }
        const double zA0 = sA + kRoadZebraOff, zA1 = zA0 + kRoadZebraWide;
        const double zB1 = sB - kRoadZebraOff, zB0 = zB1 - kRoadZebraWide;
        std::vector<SideSpan> fixL, fixR;
        // The edge's left: A's left and B's right; its right: A's right and B's left.
        if (na.arms.size() >= 2) {
            if (A.trimL > 0.0) fixL.push_back({0.0, std::min(A.trimL, L), Span::Corner});
            if (A.trimR > 0.0) fixR.push_back({0.0, std::min(A.trimR, L), Span::Corner});
        }
        if (nb.arms.size() >= 2) {
            if (B.trimR > 0.0) fixL.push_back({std::max(0.0, L - B.trimR), L, Span::Corner});
            if (B.trimL > 0.0) fixR.push_back({std::max(0.0, L - B.trimL), L, Span::Corner});
        }
        // The street's mouths on this edge: where along it its ends lie, and which way it runs.
        bool fwd = true;
        std::vector<std::pair<double, bool>> mouthSigns; // a stop sign's s and side (true: +t) at each lane mouth
        if (!st.mouths.empty() || st.poleLeft != st.poleRight) {
            auto along = [&](RoadPoint q) {
                double best = 1e18, at = 0.0, run = 0.0;
                for (std::size_t i = 1; i < st.pts.size(); ++i) {
                    const RoadPoint d = netSub(st.pts[i], st.pts[i - 1]);
                    const double l = netLen(d);
                    const double u = std::clamp(netDot(netSub(q, st.pts[i - 1]), d) / std::max(1e-9, l * l), 0.0, 1.0);
                    const double dist = netLen(netSub(q, netAdd(st.pts[i - 1], netMul(d, u))));
                    if (dist < best) {
                        best = dist;
                        at = run + u * l;
                    }
                    run += l;
                }
                return at;
            };
            const double ta = along(na.p), tb = along(nb.p);
            fwd = tb >= ta;
            for (const RoadMouth& m : st.mouths) {
                const double s = fwd ? m.at - ta : ta - m.at;
                const double s0 = s - m.width * 0.5 - kRoadMouthFlare, s1 = s + m.width * 0.5 + kRoadMouthFlare;
                if (s1 <= 0.0 || s0 >= L) continue;
                ((m.left == fwd) ? fixL : fixR).push_back({std::max(0.0, s0), std::min(L, s1), Span::Mouth});
                // On the lane traffic's left as it comes out: past the mouth's +s end on the +t side, else its -s end.
                const double ss = (m.left == fwd) ? s1 + 0.6 : s0 - 0.6;
                if (ss > 3.0 && ss < L - 3.0) mouthSigns.push_back({ss, m.left == fwd});
            }
        }
        for (std::vector<SideSpan>* side : {&fixL, &fixR}) {
            if (zA) side->push_back({zA0, zA1, Span::Ramp});
            if (zB) side->push_back({zB0, zB1, Span::Ramp});
        }
        const bool park = st.park && !st.deck && st.pavement >= kRoadBayDeep + 1.0;
        const std::vector<SideSpan> spansL = fillSpans(fixL, L, park, {});
        const std::vector<SideSpan> spansR = fillSpans(fixR, L, park, {});

        // The profile: level over each junction's fill, then the ground's samples simplified (a deck: straight).
        // A sample is the highest of the ground under the crown and under each pavement (less the kerb), lifted by
        // half the tolerance, so neither a cross-slope nor the simplified line lets the ground through the top.
        auto bandAt = [&](double s) {
            const double g0 = groundAt(f.at(s, 0.0));
            double z = g0;
            for (const double tt : {-w2 + 0.3, w2 - 0.3}) z = std::max(z, groundAt(f.at(s, tt)) - kRoadKerbRise);
            return std::min(z, g0 + kNetBandLift) + kNetProfileTol * 0.5;
        };
        std::vector<std::pair<double, double>> prof;
        prof.push_back({sA, na.z});
        if (!st.deck) {
            const int ns = static_cast<int>(std::floor((sB - sA) / kNetSample));
            for (int i = 1; i < ns; ++i) {
                const double s = sA + (sB - sA) * i / ns;
                prof.push_back({s, bandAt(s)});
            }
        }
        prof.push_back({sB, nb.z});
        std::vector<char> keep(prof.size(), 0);
        keep.front() = keep.back() = 1;
        netSimplify(prof, 0, prof.size() - 1, kNetProfileTol, keep);
        std::vector<std::pair<double, double>> knots;
        knots.push_back({0.0, na.z});
        for (std::size_t i = 0; i < prof.size(); ++i)
            if (keep[i]) knots.push_back(prof[i]);
        knots.push_back({L, nb.z});
        // No knot inside a bay: it moves to the bay's nearer end.
        for (std::size_t i = 2; i + 2 < knots.size(); ++i)
            for (const std::vector<SideSpan>* sp : {&spansL, &spansR})
                for (const SideSpan& b : *sp)
                    if (b.type == Span::Bay && knots[i].first > b.s0 && knots[i].first < b.s1) {
                        knots[i].first = knots[i].first - b.s0 < b.s1 - knots[i].first ? b.s0 : b.s1;
                        knots[i].second = bandAt(knots[i].first);
                    }
        std::sort(knots.begin(), knots.end());
        std::vector<std::pair<double, double>> kn;
        for (std::size_t i = 0; i < knots.size(); ++i) {
            if (kn.empty() || knots[i].first > kn.back().first + 0.5)
                kn.push_back(knots[i]);
            else if (i + 1 == knots.size())
                kn.back() = knots[i];
        }
        auto zAt = [&](double s) {
            if (s <= kn.front().first) return kn.front().second;
            for (std::size_t i = 1; i < kn.size(); ++i)
                if (s <= kn[i].first) {
                    const double t = (s - kn[i - 1].first) / std::max(1e-9, kn[i].first - kn[i - 1].first);
                    return kn[i - 1].second + (kn[i].second - kn[i - 1].second) * t;
                }
            return kn.back().second;
        };
        // The pieces.
        struct Piece {
            double p0 = 0.0, p1 = 0.0, z0 = 0.0, deep = 0.0;
            Frame fr;
        };
        std::vector<Piece> pieces;
        for (std::size_t i = 1; i < kn.size(); ++i) {
            Piece pc;
            pc.p0 = kn[i - 1].first;
            pc.p1 = kn[i].first;
            if (pc.p1 - pc.p0 < 1e-6) continue;
            pc.z0 = kn[i - 1].second;
            const RoadPoint q0 = f.at(pc.p0, 0.0), q1 = f.at(pc.p1, 0.0);
            pc.fr = frameOf(q0.x, q0.y, q1.x, q1.y);
            pc.fr.grade = (kn[i].second - pc.z0) / (pc.p1 - pc.p0);
            if (std::fabs(pc.fr.grade) > kRoadMaxGrade + 1e-9) {
                ++rep.steep;
                std::snprintf(note, sizeof note, "%s: %.1f%% over %.0f m at (%.0f, %.0f)", spec.id.c_str(),
                              pc.fr.grade * 100.0, pc.p1 - pc.p0, q0.x, q0.y);
                netNote(rep, rep.steep, note);
            }
            if (std::fabs(pc.fr.grade) > rep.maxGrade) {
                rep.maxGrade = std::fabs(pc.fr.grade);
                std::snprintf(note, sizeof note, "%s (%.0f, %.0f)", spec.id.c_str(), q0.x, q0.y);
                rep.maxGradeAt = note;
            }
            // The ground under it: slabs deepen over a fall; ground over the road's top is a poke.
            const double ds = std::max(1.0, std::min(kNetSample, (pc.p1 - pc.p0) * 0.5));
            for (double s = pc.p0; s <= pc.p1 + 1e-9; s += ds) {
                const double gz = zAt(s) + kRoadSurfaceLift;
                for (const double t : {0.0, -w2 + 0.3, w2 - 0.3}) {
                    const RoadPoint q = f.at(s, t);
                    const double gr = groundAt(q);
                    const double top = std::fabs(t) < hc ? gz + kRoadCrossfall * (hc - std::fabs(t)) : gz + kRoadKerbRise;
                    if (gr > top + 0.05) {
                        ++rep.pokes;
                        rep.maxPoke = std::max(rep.maxPoke, gr - top);
                        if (gr - top > 0.3) {
                            ++rep.pokesOver;
                            std::snprintf(note, sizeof note, "%s: the ground %.2f m over its top at (%.0f, %.0f)",
                                          spec.id.c_str(), gr - top, q.x, q.y);
                            netNote(rep, rep.pokesOver, note);
                        }
                    }
                    if (st.deck) continue;
                    const double fall = gz - kRoadSlabDepth - gr;
                    if (fall > kNetMaxDeep) {
                        ++rep.floats;
                        std::snprintf(note, sizeof note, "%s: %.1f m over the ground at (%.0f, %.0f)", spec.id.c_str(),
                                      fall, q.x, q.y);
                        netNote(rep, rep.floats, note);
                    }
                    pc.deep = std::max(pc.deep, std::min(kNetMaxDeep, fall));
                }
            }
            pieces.push_back(pc);
        }
        rep.pieces += pieces.size();
        auto use = [&](const Piece& pc) {
            x.gutter = pc.z0 + kRoadSurfaceLift;
            x.crown = x.gutter + kRoadCrossfall * hc;
            x.pave = x.gutter + kRoadKerbRise;
            x.deep = pc.deep;
            x.f = pc.fr;
        };
        // A marking from s0 to s1 (edge frame), across t0..t1, split over the pieces.
        auto markAlong = [&](double s0, double s1, double t0, double t1) {
            for (const Piece& pc : pieces) {
                const double a = std::max(s0, pc.p0), b = std::min(s1, pc.p1);
                if (b - a < 1e-3) continue;
                use(pc);
                x.mark(pc.fr, a - pc.p0, b - pc.p0, t0, t1, RoadTint::LineWhite);
            }
        };
        for (const Piece& pc : pieces) {
            use(pc);
            const double lp = pc.p1 - pc.p0;
            const double c0 = std::max(pc.p0, sA) - pc.p0, c1 = std::min(pc.p1, sB) - pc.p0;
            if (c1 - c0 > 1e-3) {
                const double mid = x.crown - kRoadCrossfall * hc * 0.5;
                x.add(x.box(pc.fr, c0, c1, 0.0, hc, mid, kRoadSlabDepth, 0.0, -kRoadCrossfall, "asphalt"),
                      RoadTint::Asphalt, true);
                x.add(x.box(pc.fr, c0, c1, -hc, 0.0, mid, kRoadSlabDepth, 0.0, kRoadCrossfall, "asphalt"),
                      RoadTint::Asphalt, true);
                out.carriageway.push_back(footOf(x.box(pc.fr, c0, c1, -hc, hc, 0, 0, 0, 0, "foot")));
            }
            out.band.push_back(footOf(x.box(pc.fr, 0.0, lp, -w2, w2, 0, 0, 0, 0, "foot")));
            for (int side = 0; side < 2; ++side) {
                std::vector<SideSpan> clip;
                for (const SideSpan& sp : side == 0 ? spansL : spansR) {
                    const double a = std::max(sp.s0, pc.p0), b = std::min(sp.s1, pc.p1);
                    if (b - a > 1e-6) clip.push_back({a - pc.p0, b - pc.p0, sp.type});
                }
                buildSide(x, pc.fr, side == 0 ? 1.0 : -1.0, hc, w2, lp, clip, false, st.deck);
            }
            if (st.deck) {
                // The deck's beam under its slabs.
                x.deep = 0.0;
                x.add(x.box(pc.fr, 0.0, lp, -w2, w2, x.gutter - kRoadSlabDepth, kNetDeckDeep, 0.0, 0.0, "deck"),
                      RoadTint::PoleConcrete, true);
            }
        }
        // Bays.
        for (int side = 0; side < 2; ++side) {
            const double sg = side == 0 ? 1.0 : -1.0;
            for (const SideSpan& sp : side == 0 ? spansL : spansR) {
                if (sp.type != Span::Bay) continue;
                RoadBay bay;
                bay.id = x.id("bay");
                bay.foot = footOf(x.box(f, sp.s0, sp.s1, std::min(sg * hc, sg * (hc + kRoadBayDeep)),
                                        std::max(sg * hc, sg * (hc + kRoadBayDeep)), 0.0, 0.0, 0.0, 0.0, "foot"));
                bay.at = (sp.s0 + sp.s1) * 0.5;
                bay.z = zAt(bay.at) + kRoadSurfaceLift;
                // Left-hand traffic: the left side's lane runs along s, the right side's against it.
                bay.heading = sg > 0.0 ? f.rot : f.rot + 180.0;
                bay.left = sg > 0.0;
                bay.street = st.id;
                out.bays.push_back(bay);
                ++out.counts.bays;
            }
        }
        // The centre line, dashed, stop line to stop line.
        const double stopRun = kRoadStopBack + kRoadStopLine + 1.0;
        if (!st.oneWay) {
            const double c0 = zA ? zA1 + stopRun : sA + 2.0, c1 = zB ? zB0 - stopRun : sB - 2.0;
            // Broken across each lane mouth, either side.
            auto atMouth = [&](double a, double b) {
                for (const std::vector<SideSpan>* side : {&spansL, &spansR})
                    for (const SideSpan& sp : *side)
                        if (sp.type == Span::Mouth && a < sp.s1 && b > sp.s0) return true;
                return false;
            };
            for (double s = c0; s < c1 - 1.0; s += 2.0 * kRoadCentreDash) {
                const double de = std::min(s + kRoadCentreDash, c1);
                if (!atMouth(s, de)) markAlong(s, de, -kRoadLine * 0.5, kRoadLine * 0.5);
            }
        }
        // Zebras, stop lines and signals at each signalised end.
        auto pz = [&](double s) { return zAt(s) + kRoadSurfaceLift + kRoadKerbRise; };
        for (int end = 0; end < 2; ++end) {
            const NetNode& nd = end == 0 ? na : nb;
            if (!nd.signal) continue;
            const NetArm& arm = end == 0 ? A : B;
            const bool on = end == 0 ? zA : zB;
            const double z0 = end == 0 ? zA0 : zB0, z1 = end == 0 ? zA1 : zB1;
            // Inbound traffic keeps left: towards a it is the -t half, towards b the +t half.
            const double ti = end == 0 ? -1.0 : 1.0;
            const double away = end == 0 ? 1.0 : -1.0; // along s, away from the junction
            const char other = arm.phase == 'A' ? 'B' : 'A';
            double poleS = end == 0 ? sA + 1.0 : sB - 1.0;
            if (on) {
                for (double t = 0.225; t + kRoadZebraBar <= hc - 0.1 + 1e-9; t += 2.0 * kRoadZebraBar) {
                    markAlong(z0, z1, t, t + kRoadZebraBar);
                    markAlong(z0, z1, -t - kRoadZebraBar, -t);
                }
                RoadQuad q;
                q.id = nd.id + "-Z-" + spec.id;
                q.street = st.id;
                q.junction = nd.id;
                q.pts = {f.at(z0, -hc), f.at(z1, -hc), f.at(z1, hc), f.at(z0, hc)};
                out.crossings.push_back(q);
                out.keepOut.push_back(footOf(x.box(f, z0, z1, -hc, hc, 0, 0, 0, 0, "foot")));
                ++out.counts.zebras;
                const double nearS = end == 0 ? z1 : z0;
                const double l0 = nearS + away * kRoadStopBack, l1 = l0 + away * kRoadStopLine;
                markAlong(std::min(l0, l1), std::max(l0, l1), std::min(0.0, ti * hc), std::max(0.0, ti * hc));
                out.keepOut.push_back(footOf(x.box(f, std::min(nearS, l1), std::max(nearS, l1), std::min(0.0, ti * hc),
                                                   std::max(0.0, ti * hc), 0, 0, 0, 0, "foot")));
                poleS = l1 + away * 0.5;
                const double ps = nearS + away * 0.6;
                for (const double sd : {1.0, -1.0})
                    pedSignal(x, f.at(ps, sd * (hc + kRoadSignalPoleBack)), f.dir(0.0, -sd), pz(ps), nd.id, other);
            }
            // The vehicle signal on the inbound kerb, its arm over the inbound half, facing the traffic coming in.
            const RoadPoint p = f.at(poleS, ti * (hc + kRoadSignalPoleBack));
            vehicleSignal(x, p, f.at(poleS, ti * hc * 0.5), f.dir(away, 0.0), pz(poleS), nd.id, arm.phase);
        }
        // Lamps each side, staggered, on free spots clear of the furniture.
        const std::size_t furn0 = out.furniture.size();
        for (int side = 0; side < 2; ++side) {
            const double sg = side == 0 ? 1.0 : -1.0;
            const std::vector<SideSpan>& spans = side == 0 ? spansL : spansR;
            for (double s = side == 0 ? kRoadLampSpacing * 0.5 : kRoadLampSpacing; s < L - 3.0; s += kRoadLampSpacing) {
                for (const double nudge : {0.0, 2.0, -2.0, 4.0, -4.0, 6.0, -6.0}) {
                    const double sl = s + nudge;
                    if (sl < 3.0 || sl > L - 3.0 || !spotFree(spans, sl, 1.5)) continue;
                    const double back =
                        spanAt(spans, sl) == Span::Bay ? hc + kRoadBayDeep + kRoadLampBack : hc + kRoadLampBack;
                    if (back > w2 - 0.3) continue;
                    const RoadPoint lpt = f.at(sl, sg * back);
                    MassingFoot ft;
                    ft.x = lpt.x;
                    ft.y = lpt.y;
                    ft.hx = 0.3;
                    ft.hy = 0.24;
                    ft.rot = angleOf(f.dir(0.0, -sg));
                    bool hit = false;
                    for (std::size_t i = 0; i < furn0 && !hit; ++i) hit = massingFeetOverlap(ft, out.furniture[i], 0.5);
                    if (hit) continue;
                    MassingProp lamp;
                    lamp.x = lpt.x;
                    lamp.y = lpt.y;
                    lamp.z = pz(sl);
                    lamp.face = ft.rot;
                    out.lamps.push_back(lamp);
                    ++out.counts.lamps;
                    out.furniture.push_back(ft);
                    break;
                }
            }
        }
        netPolesAndSigns(x, f, st, fwd, L, sA, sB, na.arms.size() >= 3, nb.arms.size() >= 3, spansL, spansR,
                         mouthSigns, pz);
        // A deck's piers, two abreast, down into the ground.
        if (st.deck) {
            for (double s = sA + kNetPierSpacing * 0.5; s < sB - 5.0; s += kNetPierSpacing) {
                const double bottom = zAt(s) + kRoadSurfaceLift - kRoadSlabDepth - kNetDeckDeep;
                for (const double t : {-hc * 0.5, hc * 0.5}) {
                    const RoadPoint q = f.at(s, t);
                    const double gr = groundAt(q);
                    if (gr > bottom - 0.5) continue;
                    x.add(x.at(q, 0.0, kNetPierDia, kNetPierDia, gr - 1.0, bottom, "pier"), RoadTint::PoleConcrete, true,
                          RoadShape::Cylinder);
                    ++rep.piers;
                }
            }
        }
    }
    // Kerb and pavement pieces in the way: crossed by a lane of the net where no slab lies flush with them, or lying in
    // a carriageway rectangle on their level (junctions near each other, a corner of one in the other's arm). They go:
    // the carriageway or the other junction's fill is there.
    {
        RoadGraph lanes;
        for (std::size_t i = 0; i < streets.size(); ++i)
            roadGraphStreetLanes(ms[i], streets[i].speedKmh, streets[i].oneWay, 0.0, lanes);
        const std::vector<const RoadSolid*> slabs = slabsOf(out);
        std::vector<char> drop(out.solids.size(), 0);
        for (std::size_t i = solids0; i < out.solids.size(); ++i) {
            const RoadSolid& sd = out.solids[i];
            if (!faceSolid(sd)) continue;
            boxFaces(sd.box, slabs, out.carriageway, &lanes, [&](double fx, double fy, const std::string& why) {
                if (drop[i]) return;
                drop[i] = 1;
                ++rep.pruned;
                std::snprintf(note, sizeof note, "pruned %s at (%.1f, %.1f): %s", sd.box.id.c_str(), fx, fy, why.c_str());
                netNote(rep, rep.pruned, note);
            });
        }
        // Steps between the pieces left (an edge's tilted pavement into a junction's level one): the smaller piece goes,
        // a junction's before an edge's, so the longer run stays at one height.
        std::vector<std::size_t> kept;
        for (std::size_t i = solids0; i < out.solids.size(); ++i)
            if (!drop[i] && faceSolid(out.solids[i])) kept.push_back(i);
        faceSteps(out, kept, [&](std::size_t a, std::size_t b, RoadPoint at) {
            if (drop[a] || drop[b]) return;
            const MassingBox& ba = out.solids[a].box;
            const MassingBox& bb = out.solids[b].box;
            const bool ja = ba.id.rfind("JN-", 0) == 0 || ba.id.rfind("NODE-", 0) == 0;
            const bool jb = bb.id.rfind("JN-", 0) == 0 || bb.id.rfind("NODE-", 0) == 0;
            std::size_t g = ba.sx * ba.sy <= bb.sx * bb.sy ? a : b;
            if (ja != jb) g = ja ? a : b;
            drop[g] = 1;
            ++rep.pruned;
            std::snprintf(note, sizeof note, "pruned %s at (%.1f, %.1f): step %s", out.solids[g].box.id.c_str(), at.x,
                          at.y, out.solids[g == a ? b : a].box.id.c_str());
            netNote(rep, rep.pruned, note);
        });
        std::size_t w = solids0;
        for (std::size_t i = solids0; i < out.solids.size(); ++i) {
            if (drop[i]) {
                --out.counts.solids;
                if (out.solids[i].collide) --out.counts.collide;
                continue;
            }
            if (w != i) out.solids[w] = std::move(out.solids[i]);
            ++w;
        }
        out.solids.resize(w);
    }
    if (report) *report = std::move(rep);
}

} // namespace ftcore
