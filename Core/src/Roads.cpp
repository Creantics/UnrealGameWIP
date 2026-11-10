#include "ftcore/Roads.h"

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
    int n = 0;

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
        b.z1 = top;
        b.z0 = top - depth;
        b.riseU = riseU;
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

        // The corners: an asphalt square, the pavement as a pie, the kerb on the arc; a signal pole and a lamp on each.
        RoadPoint cornerPole[2];
        for (int qi = 0; qi < 2; ++qi) {
            const double q = qi == 0 ? -1.0 : 1.0;
            const double cs = m.at + q * J, ct = ss * (hc + R);
            const double a0s = std::min(m.at + q * stubHalf, cs), a1s = std::max(m.at + q * stubHalf, cs);
            x.add(x.box(f, a0s, a1s, std::min(ss * hc, ct), std::max(ss * hc, ct), x.gutter, kRoadSlabDepth, 0, 0, "asphalt"),
                  RoadTint::Asphalt, true);
            out.band.push_back(footOf(x.box(f, a0s, a1s, std::min(ss * hc, ct), std::max(ss * hc, ct), 0, 0, 0, 0, "foot")));
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
                MassingBox pie = x.at({C.x + u.x * rin * 0.5, C.y + u.y * rin * 0.5}, angleOf(u), rin, chord + 0.04,
                                      x.pave - kRoadSlabDepth, x.pave, "pavement");
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

} // namespace ftcore
