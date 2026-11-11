#include "ftcore/RoadDecals.h"

#include "ftcore/SurfaceWeather.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace ftcore {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kInset = 0.05;    // m: a decal keeps this far in from its slab's edges
constexpr double kSmallest = 0.3;  // m: a decal shrunk under this to fit is left out
constexpr double kSlabLeast = 1.0; // m: a slab narrower than this takes none

std::uint32_t mix(std::uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

// A slab's own random stream: a seed from its place, then one value in [0, 1) per draw.
struct Draws {
    std::uint32_t state = 0;
    double next() {
        state = mix(state + 0x9e3779b9u);
        return static_cast<double>(state >> 8) / 16777216.0;
    }
    double between(double a, double b) { return a + (b - a) * next(); }
    // A count whose mean is expect.
    std::size_t count(double expect) {
        if (!(expect > 0.0)) return 0;
        const double whole = std::floor(expect);
        return static_cast<std::size_t>(whole) + (next() < expect - whole ? 1u : 0u);
    }
};

std::uint32_t quantize(double v, double per) {
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(std::llround(v * per)));
}

std::uint32_t slabSeed(std::uint32_t seed, const MassingBox& b) {
    std::uint32_t h = mix(seed ^ 0x51ab5eedu);
    h = mix(h ^ quantize(b.cx, 100.0));
    h = mix(h ^ quantize(b.cy, 100.0) * 0x85ebca6bu);
    h = mix(h ^ quantize(b.rot, 10.0) * 0xc2b2ae35u);
    h = mix(h ^ quantize(b.z1, 100.0));
    h = mix(h ^ static_cast<std::uint32_t>(b.kind.size()) ^ (b.kind.empty() ? 0u : static_cast<std::uint32_t>(b.kind[0]) << 8));
    return h;
}

// Each kind's lift over its slab, under the markings' 6 mm and apart from each other so overlaps don't flicker.
double kindLift(RoadDecalKind k) {
    return kRoadDecalLift + 0.0005 * static_cast<double>(static_cast<int>(k) - 2);
}

struct Slab {
    const MassingBox* box = nullptr;
    double c = 1.0, s = 0.0; // its u axis
    double hu = 0.0, hw = 0.0; // half extents, inset
    bool pavement = false;
};

Slab makeSlab(const MassingBox& b) {
    Slab slab;
    slab.box = &b;
    const double r = b.rot * kPi / 180.0;
    slab.c = std::cos(r);
    slab.s = std::sin(r);
    slab.hu = 0.5 * b.sx - kInset;
    slab.hw = 0.5 * b.sy - kInset;
    slab.pavement = b.kind == "pavement";
    return slab;
}

// Whether a plan point lies on the slab's inset top.
bool onSlab(const Slab& slab, double x, double y) {
    const double dx = x - slab.box->cx, dy = y - slab.box->cy;
    return std::fabs(dx * slab.c + dy * slab.s) <= slab.hu && std::fabs(-dx * slab.s + dy * slab.c) <= slab.hw;
}

double wrapDegrees(double d) {
    d = std::fmod(d + 180.0, 360.0);
    if (d < 0.0) d += 360.0;
    return d - 180.0;
}

struct Emit {
    const RoadDecalSpec* spec = nullptr;
    std::vector<RoadDecal>* out = nullptr;
    RoadDecalCounts* counts = nullptr;

    // A decal of kind k, sx by sy, turned turn degrees from the slab, centred at (du, dw) in the slab's frame; shrunk to
    // fit the slab's inset top, or left out.
    void place(const Slab& slab, RoadDecalKind k, double du, double dw, double sx, double sy, double turn, float seed) {
        const double t = turn * kPi / 180.0, ct = std::cos(t), st = std::sin(t);
        // The turned footprint's half extents along the slab's u and w.
        const double eu = 0.5 * (std::fabs(ct) * sx + std::fabs(st) * sy);
        const double ew = 0.5 * (std::fabs(st) * sx + std::fabs(ct) * sy);
        double fit = 1.0;
        if (eu > slab.hu) fit = std::min(fit, slab.hu / eu);
        if (ew > slab.hw) fit = std::min(fit, slab.hw / ew);
        sx *= fit;
        sy *= fit;
        if (std::min(sx, sy) < kSmallest && k != RoadDecalKind::Crack) return;
        if (sx < kSmallest) return;
        // Keep the centre where the shrunk footprint fits.
        const double roomU = std::max(0.0, slab.hu - eu * fit), roomW = std::max(0.0, slab.hw - ew * fit);
        du = std::clamp(du, -roomU, roomU);
        dw = std::clamp(dw, -roomW, roomW);
        if (out->size() >= spec->maxDecals) {
            ++counts->capped;
            return;
        }
        const MassingBox& b = *slab.box;
        RoadDecal d;
        d.kind = k;
        d.cx = b.cx + du * slab.c - dw * slab.s;
        d.cy = b.cy + du * slab.s + dw * slab.c;
        d.sx = sx;
        d.sy = sy;
        d.rot = b.rot + turn;
        d.riseU = b.riseU * ct + b.riseW * st;
        d.riseW = -b.riseU * st + b.riseW * ct;
        d.top = b.z1 + b.riseU * du + b.riseW * dw + kindLift(k) + 0.0002 * static_cast<double>(seed);
        d.seed = seed;
        d.pavement = slab.pavement;
        out->push_back(d);
        ++counts->perKind[static_cast<std::size_t>(k)];
    }

    // The same, centred at the plan point (x, y) and turned to plan rot degrees.
    void placeAt(const Slab& slab, RoadDecalKind k, double x, double y, double sx, double sy, double rot, float seed) {
        const double dx = x - slab.box->cx, dy = y - slab.box->cy;
        place(slab, k, dx * slab.c + dy * slab.s, -dx * slab.s + dy * slab.c, sx, sy, wrapDegrees(rot - slab.box->rot), seed);
    }
};

} // namespace

bool roadDecalSlab(const RoadSolid& solid) {
    if (solid.shape != RoadShape::Cube) return false;
    const std::string& k = solid.box.kind;
    return k == "asphalt" || k == "fill" || k == "bay" || k == "pavement";
}

void roadDecals(const RoadSolid* solids, std::size_t count, const RoadDecalSpec& spec, std::vector<RoadDecal>& out,
                RoadDecalCounts& counts) {
    if (!solids || !(spec.density > 0.0)) return;
    Emit emit{&spec, &out, &counts};
    const double dens = spec.density;
    for (std::size_t i = 0; i < count; ++i) {
        const RoadSolid& solid = solids[i];
        if (!roadDecalSlab(solid)) continue;
        const MassingBox& b = solid.box;
        if (!(b.sx >= kSlabLeast) || !(b.sy >= kSlabLeast)) continue;
        ++counts.slabs;
        const Slab slab = makeSlab(b);
        const bool pavement = slab.pavement;
        const double area = b.sx * b.sy;
        (pavement ? counts.pavementM2 : counts.asphaltM2) += area;
        Draws draw{slabSeed(spec.seed, b)};
        const auto anywhere = [&](double& du, double& dw) {
            du = draw.between(-slab.hu, slab.hu);
            dw = draw.between(-slab.hw, slab.hw);
        };
        double du = 0.0, dw = 0.0;

        // Cracks: long and thin, any way round.
        const std::size_t cracks = draw.count(dens * area / 100.0 * (pavement ? spec.crackPavement : spec.crackAsphalt));
        for (std::size_t n = 0; n < cracks; ++n) {
            anywhere(du, dw);
            const double len = draw.between(0.8, 3.2), wide = draw.between(0.35, 1.0);
            const double turn = draw.between(-90.0, 90.0);
            emit.place(slab, RoadDecalKind::Crack, du, dw, len, wide, turn, static_cast<float>(draw.next()));
        }
        // Stains: roundish, oil on the carriageway mostly in the lanes' middles, dirt and spills on the pavement.
        const std::size_t stains = draw.count(dens * area / 100.0 * (pavement ? spec.stainPavement : spec.stainAsphalt));
        for (std::size_t n = 0; n < stains; ++n) {
            anywhere(du, dw);
            if (!pavement) dw *= 0.6;
            const double size = draw.between(0.5, 1.8);
            emit.place(slab, RoadDecalKind::Stain, du, dw, size * draw.between(0.8, 1.6), size, draw.between(-90.0, 90.0),
                       static_cast<float>(draw.next()));
        }
        if (pavement) continue;
        // A bay: a cluster of oil drips where an engine stands, and no patches or gutter.
        if (b.kind == "bay") {
            if (draw.next() < std::min(1.0, spec.bayOil * dens)) {
                emit.place(slab, RoadDecalKind::Oil, draw.between(-0.25, 0.25) * slab.hu, draw.between(-0.2, 0.2) * slab.hw,
                           draw.between(1.1, 1.6), draw.between(0.7, 1.0), draw.between(-10.0, 10.0), static_cast<float>(draw.next()));
            }
            continue;
        }
        // Patches: square to the road, a little askew.
        const std::size_t patches = draw.count(dens * area / 100.0 * spec.patchAsphalt);
        for (std::size_t n = 0; n < patches; ++n) {
            anywhere(du, dw);
            emit.place(slab, RoadDecalKind::Patch, du, dw, draw.between(1.0, 3.2), draw.between(0.8, 2.0), draw.between(-4.0, 4.0),
                       static_cast<float>(draw.next()));
        }
        // A crowned half (its top falls across it): manholes along its middle and grime along its low edge.
        if (b.riseW == 0.0) continue;
        const std::size_t holes = draw.count(dens * b.sx / 100.0 * spec.manholePer100m);
        for (std::size_t n = 0; n < holes; ++n) {
            const double size = draw.between(0.65, 0.8);
            emit.place(slab, RoadDecalKind::Manhole, draw.between(-slab.hu, slab.hu), draw.between(-0.2, 0.2) * slab.hw, size, size,
                       draw.between(-5.0, 5.0), static_cast<float>(draw.next()));
        }
        // The gutter: the edge the top falls to (+w when riseW < 0). Each piece is turned so its +w faces the kerb.
        const double gutter = b.riseW < 0.0 ? 1.0 : -1.0;
        const double wide = std::min(kRoadGrimeWide, slab.hw);
        const double centreW = gutter * (slab.hw - 0.5 * wide);
        const std::size_t runs = static_cast<std::size_t>(std::floor(b.sx / kRoadGrimeRun));
        for (std::size_t n = 0; n < runs; ++n) {
            const double p = draw.next();
            const double len = kRoadGrimeRun * draw.between(0.7, 1.0);
            if (p >= std::min(1.0, spec.grimeChance * dens)) continue;
            const double at = -0.5 * b.sx + (static_cast<double>(n) + 0.5) * kRoadGrimeRun;
            emit.place(slab, RoadDecalKind::KerbGrime, at, centreW, len, wide, gutter > 0.0 ? 0.0 : 180.0, static_cast<float>(draw.next()));
        }
        // Drain grates against the kerb, every drainFromM to drainToM.
        if (spec.drainFromM > 0.0) {
            const double grateDeep = std::min(0.4, slab.hw);
            const double grateW = gutter * (slab.hw - 0.5 * grateDeep);
            for (double at = -slab.hu + draw.between(0.3, 1.0) * spec.drainFromM; at < slab.hu;
                 at += draw.between(spec.drainFromM, std::max(spec.drainFromM, spec.drainToM))) {
                emit.place(slab, RoadDecalKind::Drain, at, grateW, 0.6, grateDeep, gutter > 0.0 ? 0.0 : 180.0,
                           static_cast<float>(draw.next()));
            }
        }
    }
}

void roadDecals(const RoadBuild& build, const RoadDecalSpec& spec, std::vector<RoadDecal>& out, RoadDecalCounts& counts) {
    roadDecals(build.solids.data(), build.solids.size(), spec, out, counts);
    if (!(spec.density > 0.0)) return;
    Emit emit{&spec, &out, &counts};
    // The decal slab of the given ground (pavement or not) under a plan point.
    const auto slabAt = [&](double x, double y, bool pavement, Slab& found) {
        for (const RoadSolid& solid : build.solids) {
            if (!roadDecalSlab(solid) || (solid.box.kind == "pavement") != pavement) continue;
            if (!(solid.box.sx >= kSlabLeast) || !(solid.box.sy >= kSlabLeast)) continue;
            const Slab slab = makeSlab(solid.box);
            if (onSlab(slab, x, y)) {
                found = slab;
                return true;
            }
        }
        return false;
    };
    // Oil before the stop lines, where the first car waits: on one side or the other (the line doesn't say which).
    for (const RoadSolid& solid : build.solids) {
        const MassingBox& m = solid.box;
        if (solid.shape != RoadShape::Cube || m.kind != "mark" || std::fabs(m.sx - kRoadStopLine) > 0.05 || m.sy < 1.5) continue;
        Draws draw{slabSeed(spec.seed ^ 0x0175a0f5u, m)};
        if (draw.next() >= std::min(1.0, spec.stopOil * spec.density)) continue;
        const double r = m.rot * kPi / 180.0, c = std::cos(r), sn = std::sin(r);
        const double back = (draw.next() < 0.5 ? -1.0 : 1.0) * (0.5 * m.sx + draw.between(1.5, 4.0));
        const double across = draw.between(-0.25, 0.25) * m.sy;
        const double x = m.cx + back * c - across * sn, y = m.cy + back * sn + across * c;
        Slab slab;
        if (slabAt(x, y, false, slab)) {
            emit.placeAt(slab, RoadDecalKind::Oil, x, y, draw.between(1.0, 1.5), draw.between(0.7, 1.0), m.rot + draw.between(-10.0, 10.0),
                         static_cast<float>(draw.next()));
        }
    }
    if (!spec.tactile) return;
    // Tactile strips: on the pavement past each end of each crossing, as wide as the crossing.
    for (const RoadQuad& q : build.crossings) {
        const double qx = 0.25 * (q.pts[0].x + q.pts[1].x + q.pts[2].x + q.pts[3].x);
        const double qy = 0.25 * (q.pts[0].y + q.pts[1].y + q.pts[2].y + q.pts[3].y);
        for (int end = 0; end < 2; ++end) {
            const RoadPoint& a = q.pts[end == 0 ? 0 : 2];
            const RoadPoint& b = q.pts[end == 0 ? 1 : 3];
            const double mx = 0.5 * (a.x + b.x), my = 0.5 * (a.y + b.y);
            const double ox = mx - qx, oy = my - qy, on = std::hypot(ox, oy);
            const double along = std::hypot(b.x - a.x, b.y - a.y);
            if (!(on > 1e-6) || !(along > 0.5)) continue;
            const double reach = kRoadKerbWide + 0.55;
            const double x = mx + ox / on * reach, y = my + oy / on * reach;
            Slab slab;
            if (slabAt(x, y, true, slab)) {
                const double rot = std::atan2(b.y - a.y, b.x - a.x) * 180.0 / kPi;
                emit.placeAt(slab, RoadDecalKind::Tactile, x, y, std::min(along, 4.0), 0.8, rot, 0.5f);
            }
        }
    }
}

int roadStreetKind(RoadTint tint) { return roadTintStreet(tint); }

MassingBox roadDecalBox(const RoadDecal& decal, double height) {
    MassingBox b;
    b.kind = "decal";
    b.cover = true;
    b.cx = decal.cx;
    b.cy = decal.cy;
    b.sx = decal.sx;
    b.sy = decal.sy;
    b.rot = decal.rot;
    b.z1 = decal.top;
    b.z0 = decal.top - height;
    b.riseU = decal.riseU;
    b.riseW = decal.riseW;
    return b;
}

void roadDecalData(const RoadDecal& decal, float out[kRoadDecalData]) {
    out[0] = static_cast<float>(static_cast<int>(decal.kind) + (decal.pavement ? 8 : 0));
    out[1] = decal.seed;
    out[2] = static_cast<float>(decal.sx);
    out[3] = static_cast<float>(decal.sy);
}

// M: metres from the plate's centre along its u and w (LocalPos is the 1 m plate's, in cm; its local y is -w). Every
// pattern is noise and distance from M, so nothing samples a texture. The return value is the colour, RoughOut the
// roughness and MaskOut the opacity mask (clipped at 0.5). The soft kinds converge on the ground's own colour and
// roughness over their outer band (Blend), so the clip lands where the decal already matches the ground. Wetness then
// darkens and smooths as the surface master does at the same place (surfaceWetHlsl).
const char* roadDecalHlsl() {
    static const std::string full = [] {
        std::string h = R"HLSL(
struct FTRoadDecalNoise
{
	float H(float2 p) { p = frac(p * float2(0.1031, 0.1030)); p += dot(p, p.yx + 33.33); return frac((p.x + p.y) * p.x); }
	float N(float2 p)
	{
		float2 i = floor(p);
		float2 f = frac(p);
		float2 u = f * f * (3.0 - 2.0 * f);
		return lerp(lerp(H(i), H(i + float2(1.0, 0.0)), u.x), lerp(H(i + float2(0.0, 1.0)), H(i + float2(1.0, 1.0)), u.x), u.y);
	}
	float F(float2 p) { return 0.5 * N(p) + 0.3 * N(p * 2.03 + 17.1) + 0.2 * N(p * 4.01 + 31.7); }
};
FTRoadDecalNoise D;
float2 Size = max(float2(SizeU, SizeW), float2(0.05, 0.05));
float2 M = float2(LocalPos.x, -LocalPos.y) / 100.0 * Size;
float2 E = 0.5 * Size - abs(M);
float Edge = min(E.x, E.y);
float2 O = float2(Seed * 91.7, Seed * 53.3) + 3.0;
int KindAll = (int)round(Kind);
bool Paved = KindAll >= 8;
int Kd = Paved ? KindAll - 8 : KindAll;
float3 Under = Paved ? float3(0.32, 0.31, 0.29) : float3(0.045, 0.045, 0.05);
float UnderR = Paved ? 0.8 : 0.85;
float Grain = D.N(M * 23.0 + O);
float3 C = Under;
float Mask = 0.0;
float R = UnderR;
float Blend = 1.0;
float GutIn = 0.0;
float FromKerb = 0.5 * Size.y - M.y + 0.05;
if (Kd == 0)
{
	// A sealed crack: a wandering tar line along u, 1.5 to 4 cm wide, a branch off it, tapering to its ends.
	float Y = M.y - (D.F(float2(M.x * 0.9, 0.0) + O) - 0.5) * Size.y * 0.8;
	float W = 0.0075 + 0.0125 * D.N(float2(M.x * 3.0, 5.0) + O);
	float Main = 1.0 - smoothstep(W * 0.5, W, abs(Y));
	float Slant = (D.H(O) - 0.5) * 1.6;
	float Y2 = M.y - (M.x - Size.x * (D.H(O + 1.7) - 0.5) * 0.5) * Slant - (D.F(float2(M.x * 2.1, 9.0) + O) - 0.5) * Size.y * 0.3;
	float Side = (D.H(O + 3.1) > 0.5) ? M.x : -M.x;
	float Branch = (1.0 - smoothstep(W * 0.35, W * 0.7, abs(Y2))) * step(0.0, Side) * step(0.35, D.H(O + 5.3));
	float Taper = smoothstep(0.0, 0.3 * Size.x, E.x) * smoothstep(0.0, 0.05, E.y);
	Mask = max(Main, Branch) * Taper * 1.4;
	C = float3(0.022, 0.021, 0.02) * (0.85 + 0.3 * Grain);
	R = 0.6;
}
else if (Kd == 1)
{
	// A stain: a ragged blob, oil (dark, glossy) or, for a third of them, dried dirt and spills (dull and brown-grey).
	float2 Q = M / (0.5 * Size);
	float Rad = length(Q) + (D.F(M * 1.7 + O) - 0.5) * 0.9;
	float In = (0.72 - Rad) * 0.5 * min(Size.x, Size.y);
	Mask = 0.5 + In * 40.0;
	Blend = smoothstep(0.0, 0.12, In);
	float Inner = D.F(M * 4.0 + O + 11.0);
	// On a pavement never oil, and never darker than a damp patch of the paving itself.
	bool Oil = !Paved && D.H(O + 7.7) < 0.66;
	C = Oil ? float3(0.018, 0.018, 0.02) * (0.7 + 0.6 * Inner) : float3(0.07, 0.062, 0.05) * (0.8 + 0.4 * Inner);
	if (Paved) { C = Under * float3(0.78, 0.75, 0.7) * (0.85 + 0.15 * Inner); }
	R = Oil ? lerp(0.25, 0.5, Inner) : 0.95;
}
else if (Kd == 2)
{
	// A cover: iron, round or (a quarter) square, a rim, a grid of raised studs worn bright in the middle.
	float Half = 0.5 * min(Size.x, Size.y);
	bool Square = D.H(O + 2.9) < 0.25;
	float Dist = Square ? max(abs(M.x), abs(M.y)) : length(M);
	float Out = Half * 0.96;
	Mask = 0.5 + (Out - Dist) * 200.0;
	float Rim = smoothstep(Out - 0.05, Out - 0.035, Dist);
	float2 Cell = abs(frac(M * 12.0) - 0.5);
	float Stud = 1.0 - smoothstep(0.18, 0.24, max(Cell.x, Cell.y));
	float Wear = 1.0 - saturate(Dist / Half);
	C = float3(0.055, 0.052, 0.05) * (0.8 + 0.25 * Grain) + Stud * float3(0.03, 0.028, 0.026) * (0.4 + Wear) + Rim * 0.03;
	C *= lerp(float3(1.0, 1.0, 1.0), float3(1.15, 0.85, 0.65), 0.35 * D.F(M * 6.0 + O));
	R = lerp(0.7, 0.35, saturate(Stud * Wear + Rim * 0.6));
}
else if (Kd == 3)
{
	// A patch of newer (a little darker) or older (a little paler) asphalt, within about 15% of the road's, in one of
	// four shapes: a rectangle, a rectangle with a corner cut out, a rounded blob or a trench strip along u. Its edge is
	// ragged at two scales and fades into the road, with a broken tar seal along part of it.
	float Pick = D.H(O + 6.1);
	float Dd = Edge;
	if (Pick >= 0.35 && Pick < 0.6)
	{
		float2 Sg = sign(float2(D.H(O + 2.2), D.H(O + 3.3)) - 0.5 + 1e-3);
		float2 Notch = (float2(D.H(O + 8.3), D.H(O + 9.1)) * 0.3 + 0.05) * Size * Sg;
		Dd = min(Edge, max(Sg.x * (Notch.x - M.x), Sg.y * (Notch.y - M.y)));
	}
	else if (Pick >= 0.6 && Pick < 0.8)
	{
		Dd = 0.5 * min(Size.x, Size.y) * (1.0 - length(M / (0.5 * Size)));
	}
	else if (Pick >= 0.8)
	{
		Dd = min(E.x, min(0.25, 0.5 * Size.y) - abs(M.y));
	}
	float In = Dd + (D.F(M * 1.1 + O) - 0.5) * 0.16 + (D.N(M * 7.0 + O) - 0.5) * 0.03 - 0.02;
	Mask = 0.5 + In * 60.0;
	Blend = smoothstep(0.0, 0.15, In);
	bool Newer = D.H(O + 4.4) < 0.6;
	float3 Body = Newer ? float3(0.038, 0.038, 0.041) : float3(0.052, 0.051, 0.054);
	C = Body * (0.9 + 0.2 * Grain) * (0.94 + 0.12 * D.F(M * 1.3 + O));
	float Seal = (1.0 - smoothstep(0.0, 0.035, In)) * step(0.55, D.N(M * 1.8 + O + 4.0)) * smoothstep(0.005, 0.02, In);
	C *= 1.0 - 0.3 * Seal;
	R = lerp(Newer ? 0.78 : 0.9, 0.6, Seal);
}
else if (Kd == 4)
{
	// Kerb grime: dark silt and leaf-stain, thickest against the kerb (+w), ragged towards the road, fading at its ends.
	float Off = saturate((0.5 * Size.y - M.y) / Size.y);
	float Streak = D.F(float2(M.x * 1.4, M.y * 6.0) + O);
	float Amount = (1.0 - Off) * 1.25 + (Streak - 0.5) * 0.8;
	Amount *= smoothstep(0.0, 0.6, E.x + (D.N(float2(M.x * 0.8, 1.0) + O) - 0.5) * 0.8);
	float In = (Amount - 0.55) / 2.8;
	Mask = 0.5 + In * 17.0;
	Blend = smoothstep(0.0, 0.15, In);
	C = lerp(float3(0.034, 0.031, 0.026), float3(0.02, 0.019, 0.016), saturate(1.0 - Off * 1.4)) * (0.85 + 0.3 * Grain);
	R = 0.92;
	GutIn = 1.0 - smoothstep(0.0, GUTTER_M, FromKerb);
}
else if (Kd == 5)
{
	// Oil drips: a few overlapping dark spots where an engine stood, glossy.
	float In = -1.0;
	for (int Sp = 0; Sp < 5; ++Sp)
	{
		float2 At = (float2(D.H(O + Sp * 1.3), D.H(O + Sp * 2.7)) - 0.5) * Size * 0.6;
		float Rad = 0.05 + 0.14 * D.H(O + Sp * 3.9);
		In = max(In, Rad - length(M - At) + (D.N(M * 9.0 + O + Sp) - 0.5) * 0.04);
	}
	Mask = 0.5 + In * 40.0;
	Blend = smoothstep(0.0, 0.08, In);
	C = float3(0.016, 0.016, 0.018) * (0.8 + 0.4 * Grain);
	R = 0.3;
}
else if (Kd == 6)
{
	// A tactile strip: yellow paving with rows of domes, worn on their tops.
	Mask = 0.5 + (Edge - 0.01) * 200.0;
	float2 Dm = frac(M / 0.06) - 0.5;
	float Dome = 1.0 - smoothstep(0.2, 0.32, length(Dm));
	C = float3(0.58, 0.43, 0.05) * (0.9 + 0.15 * Grain) * (0.85 + 0.15 * Dome) * (0.9 + 0.1 * D.F(M * 2.0 + O));
	R = lerp(0.7, 0.5, Dome);
}
else
{
	// A drain grate: an iron frame, slots across it, silt in the slots.
	Mask = 0.5 + (Edge - 0.005) * 200.0;
	float Frame = 1.0 - smoothstep(0.035, 0.045, Edge);
	float Slot = (1.0 - Frame) * step(0.45, frac(M.x / 0.04));
	C = lerp(float3(0.05, 0.048, 0.046) * (0.8 + 0.3 * Grain), float3(0.006, 0.006, 0.006), Slot);
	R = lerp(0.55, 0.9, Slot);
	GutIn = 1.0 - smoothstep(0.0, GUTTER_M, FromKerb);
}
C = lerp(Under, C, Blend);
R = lerp(UnderR, R, Blend);
float WetIn = saturate(Wetness);
)HLSL";
        const SurfaceWeatherSpec spec{};
        const std::string key = "GUTTER_M";
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.6f", static_cast<double>(spec.gutterM));
        for (std::size_t at = h.find(key); at != std::string::npos; at = h.find(key, at)) h.replace(at, key.size(), buf);
        h += surfaceWetHlsl(spec);
        h += "RoughOut = saturate(R);\nMaskOut = saturate(Mask);\nreturn C;\n";
        return h;
    }();
    return full.c_str();
}

} // namespace ftcore
