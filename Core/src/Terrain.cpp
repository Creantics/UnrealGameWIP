#include "ftcore/Terrain.h"

#include "SeedMix.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace ftcore {

namespace {

constexpr std::size_t kN = WorldTerrain::kSamples;
constexpr std::size_t kMaxPads = WorldLayout::kMaxDistricts;
constexpr float kR = WorldTerrain::kFlatRadius;
constexpr float kLevel = WorldTerrain::kRoadHalfWidth + WorldTerrain::kShoulder; // ground level with a road out to here
constexpr float kReach = 90.f;    // m from a road's centre line that its banks still shape the ground
constexpr float kFillReach = 60.f; // m from it that ground under a road is still lifted toward it
constexpr float kCarveFade = 4.f; // m along a road past a pad's edge before the carve is at full depth
constexpr float kMaxPad = 60.f;   // m: no pad sits further from the base
constexpr float kTan30 = 0.57735f;
constexpr float kTan32 = 0.62487f;
constexpr float kCrest = 4.f;     // m: the rim around a bowl
constexpr float kButteSlope = 1.3f; // a butte's blend at its steepest (52 degrees)
constexpr float kFootWander = 60.f;    // m past the playable edge that the range's foot may start, by noise along it
constexpr float kFootBand = 12.f;      // m: the broken band of rock at the range's foot, at its tallest
constexpr float kFootRun = 12.f;       // m it rises over
constexpr float kRangeClimb = 170.f;   // m past its foot that the range takes to reach full height
constexpr float kRangeFloor = 60.f;    // m above the base that the range's valleys climb to
constexpr float kRangeFloorMin = 20.f; // m: and at least this above the ground beneath
constexpr float kRangeRidge = 80.f;    // m: the most its ridges rise over its valleys
constexpr float kSwell = 35.f;         // m: the ground's broad rise and fall, before the sites bend it
constexpr float kSwellWave = 850.f;    // m across one rise and fall, about
constexpr float kSwellReach = 450.f;   // m around the entry and the climax over which the swell bends to meet them
constexpr float kClimaxWant = 36.f;    // m: the climax's height, where the roads up to it allow
constexpr float kMesaHeight = 20.f;    // m: a ruin mesa's height once clear of its edge
constexpr float kMesaRise = 10.f;      // m its top rises further toward its middle
constexpr float kGullyDepth = 8.f;     // m: a ruin gully at its deepest
constexpr float kGullyRamp = 30.f;     // m outside a pad's blend over which a gully climbs out
constexpr float kMesaFrom = 0.3f;      // a ruin mesa grows where the mesa noise is over this
constexpr float kMesaBar = 0.7f;       // and over this much more where none may grow
constexpr float kMesaClimaxClear = 120.f; // m round the climax's centre where no mesa grows; they return over 60 m more
constexpr float kMesaRoadClear = 20.f;    // m either side of a road's centre line, returning over 20 m more
constexpr float kMesaRoom = 6.f;       // m: nor where its ceiling is less than this over the ground, growing freely from 14 m more
constexpr float kKnee = 8.f;           // m under a ceiling where the ground starts to ease toward it
constexpr float kClimaxClear = 2.f;    // m the ground between the pads keeps under the climax
constexpr float kEyeHeight = 1.7f;     // m: the sightline's eye over the entry pad
constexpr float kEyeInset = 5.f;       // m inside the entry pad's edge, toward the climax
constexpr float kAimHeight = 15.f;     // m over the climax's centre that the sightline aims at
constexpr float kSightClear = 1.5f;    // m the ground keeps under the sightline
constexpr float kCorridorHalf = 15.f;  // m either side of it that this holds in full
constexpr float kCorridorSlope = 0.5f; // how fast the ceiling rises beyond (27 degrees)
constexpr float kCoastBand = 60.f;     // m inside each edge: the town's coast is the edge whose band lies lowest on average
constexpr float kSeaUnder = 6.f;       // m the sea lies under the lowest ground in that band, at least
constexpr float kQuayRun = 6.f;        // m past the coast edge over which the quay drops
constexpr float kQuayFoot = 1.f;       // m under the sea at the quay's foot
constexpr float kSeaFall = 114.f;      // m beyond the quay over which the sea floor falls
constexpr float kSeaDeep = 7.f;        // m it falls by
constexpr float kHeadland = 40.f;      // m past the coast edge that the ranges either side run on, at least
constexpr float kHeadlandSpread = 120.f; // m more, by noise along the coast
constexpr float kHeadlandFade = 50.f;  // m over which a headland then falls into the sea
constexpr float kAbyssLip = 20.f;      // m past the edge that the ruins' ground runs on, at least
constexpr float kAbyssSpread = 90.f;   // m more, in bays and headlands along the edge
constexpr float kAbyssJag = 4.f;       // m the lip juts out or bites in by, more finely still
constexpr float kAbyssProbe = 24.f;    // m out from the edge that the lip takes its noise, so it changes round a corner too
constexpr float kAbyssCrumble = 10.f;  // m the ground crumbles away by past the lip
constexpr float kAbyssCrumbleRun = 6.f; // m it crumbles over
constexpr float kAbyssJitter = 1.5f;   // m the sheer fall then wavers by, nearer or further
constexpr float kAbyssBreak = 6.f;     // m over which it falls
constexpr float kAbyssFloor = -180.f;  // m: the abyss's floor, give or take kAbyssRoll
constexpr float kAbyssRoll = 10.f;
constexpr float kFeatureClear = 25.f;  // m a feature keeps from the roads' edges, the pads' blends and the other features
constexpr float kWallClear = 30.f;     // m it keeps inside the walls
constexpr float kFeatureGrid = 3.f;    // m: features are placed and sized on this grid
constexpr float kTrenchWidth = 15.f;   // m
constexpr float kTrenchMin = 60.f;     // m long, at least
constexpr float kTrenchMax = 200.f;    // m long, at most
constexpr int kMaxTrenches = 2;
constexpr float kTrenchApart = 120.f;  // m between two trenches' footprints, at least
constexpr float kCutBand = 6.f;        // m of walkable ground all round a trench or a pit, so every walk past one goes round
constexpr float kWalkSlope = 0.99183f; // tan 44.765 degrees: the steepest ground a player walks up
constexpr float kTrenchDepth = 18.f;   // m a trench's floor lies under the lowest ground round it,
constexpr float kTrenchDeeper = 6.f;   // and this or twice this more
constexpr float kTrenchRelief = 8.f;   // m the ground round a trench rises and falls by, at most
constexpr float kTrenchWall = 1.6f;    // the pitch of a trench's walls and a pit's funnel (58 degrees)
constexpr float kTrenchWobble = 2.f;   // m a trench's walls stand out past its floor's edge, on average: twice this at most
constexpr Vec2 kPoolSizes[4] = {{36.f, 18.f}, {30.f, 15.f}, {24.f, 12.f}, {18.f, 9.f}}; // m long by wide, the largest tried first
constexpr int kMaxPools = 3;
constexpr float kPoolMargin = 4.f;     // m of platform round a pool, on its level
constexpr float kPoolRelief = 3.f;     // m the ground round a pool rises and falls by, at most
constexpr float kPoolBankSlope = 0.5f; // the bank from a pool's platform up to the ground round it (27 degrees)
constexpr float kPoolBank = kPoolRelief / kPoolBankSlope; // m: the bank's width, at most
constexpr float kPoolApart = 80.f;     // m between two pools, at least
constexpr float kPoolBed = 0.75f;      // m under its platform that a pool's bed lies
constexpr float kPoolSurface = 0.15f;  // m under its platform that its water lies
constexpr int kMaxPits = 2;
constexpr float kPitRadius = 22.f;     // m: a pit's radius, on average
constexpr float kPitRagged = 0.3f;     // its rays run up to this share of kPitRadius further or shorter
constexpr float kPitWobble = 1.2f;     // noise cells across the circle that ruffles them
constexpr float kPitFunnel = 10.f;     // m in from a pit's edge that its funnel runs before the shaft drops sheer,
constexpr float kPitJitter = 1.f;      // give or take this,
constexpr float kPitJitterWobble = 3.f; // by noise this many cells across the circle
constexpr float kPitFloor = -145.f;    // m: a pit's floor, give or take kPitRoll
constexpr float kPitRoll = 25.f;
constexpr float kPitApart = 120.f;     // m between two pits, at least
// The rain (wear()), after Lague's droplet model, its heights in metres where his run 0 to 1 over a
// field kWearScale metres high.
constexpr int kWearDrops = 200000;       // droplets over the square and kWearMargin round it at kTunedSide,
                                         // and as many to each sample there at other sides
constexpr int kWearLife = 80;            // steps a droplet runs at most, a cell each
constexpr int kWearBrush = 4;            // samples: the radius a droplet cuts over
constexpr float kWearMargin = 60.f;      // m past the square's edge over which the wear fades out
constexpr float kWearRoadRamp = 15.f;    // m past a road's shoulder, and two samples, over which it fades in
constexpr float kWearSoft = 0.7f;        // ground steeper than this wears less, as rock,
constexpr float kWearSoftRun = 0.5f;     // and from this much steeper not at all
constexpr int kWearSoftBlur = 2;         // samples either side over which that share is eased, twice over, and
                                         // over which the ground round a mesa or a gully is kept
constexpr float kWearKeep = 2.f;         // m: ground the kind raised or cut this far (a mesa, a gully) doesn't wear
constexpr float kRangeSoft = 1.2f;       // the mountains' ground steeper than this wears less,
constexpr float kRangeSoftRun = 0.6f;    // and from this much steeper not at all
constexpr std::size_t kRangeCoarse = 4;  // samples a cell of the field the mountains' gullies are worn on
constexpr int kRangeDrops = 200000;      // droplets on it,
constexpr float kRangeRain = 0.1f;       // each wearing this share as hard as one on the full field
constexpr int kRangeBrush = 2;           // coarse samples: the radius a droplet cuts over there
constexpr float kRangeBury = 2.f;        // m: where the valleys lay down this much, it buries the finer relief
constexpr float kRangeRim = 30.f;        // m along the field's edge that the mountains keep as raised, the horizon,
constexpr float kRangeRimRun = 40.f;     // their wear fading out over this much before it
constexpr int kMaxWearBrush = 4;         // the widest brush wear() takes
constexpr float kWearScale = 50.f;
constexpr float kWearInertia = 0.1f;     // the share of its way a droplet keeps each step
constexpr float kWearCapacity = 4.f;     // what a droplet may carry per metre it falls, at unit speed and water
constexpr float kWearLeast = 0.01f * kWearScale; // m: what it may carry on the flat
constexpr float kWearCut = 0.3f;         // the share of what it may still carry that it cuts each step
constexpr float kWearLay = 0.3f;         // the share of what it carries past that which it lays each step
constexpr float kWearDry = 0.01f;        // the share of its water it loses each step
constexpr float kWearGravity = 4.f / kWearScale; // its speed squared gained per metre it falls
constexpr int kRelaxSweeps = 400;
constexpr int kSlopeSweeps = 64;       // forward-and-back sweeps the slope limit may take to settle
constexpr float kDiag = 0.70710678f;
constexpr Vec2 kCompass[8] = {{1.f, 0.f},  {kDiag, kDiag},   {0.f, 1.f},  {-kDiag, kDiag},
                              {-1.f, 0.f}, {-kDiag, -kDiag}, {0.f, -1.f}, {kDiag, -kDiag}};

float smooth01(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

float lerpf(float a, float b, float t) { return a + (b - a) * t; }

float terrainDist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

// The point of segment a-b nearest p: its parameter (0 at a, 1 at b), and its distance from p.
float nearestOnSegment(Vec2 p, Vec2 a, Vec2 b, float& distance) {
    const float abx = b.x - a.x, aby = b.y - a.y;
    const float len2 = abx * abx + aby * aby;
    const float t = len2 > 0.f ? std::clamp(((p.x - a.x) * abx + (p.y - a.y) * aby) / len2, 0.f, 1.f) : 0.f;
    distance = std::hypot(p.x - (a.x + abx * t), p.y - (a.y + aby * t));
    return t;
}

// A road's height s metres along it from district a: flat inside the two pads, straight between
// their edges.
float roadProfile(float length, float from, float to, float s) {
    const float ramp = length - 2.f * kR;
    if (ramp <= 0.f) return s < length * 0.5f ? from : to;
    return lerpf(from, to, std::clamp((s - kR) / ramp, 0.f, 1.f));
}

// Gradient noise on the integer lattice: one of eight directions per lattice point, picked by a
// hash, with a quintic fade. About -1..1.
float gradientNoise(float x, float y, std::uint32_t seed) {
    const float x0 = std::floor(x), y0 = std::floor(y);
    const std::int32_t ix = static_cast<std::int32_t>(x0), iy = static_cast<std::int32_t>(y0);
    const float fx = x - x0, fy = y - y0;
    const auto corner = [seed](std::int32_t cx, std::int32_t cy, float dx, float dy) {
        const std::uint32_t h = detail::mixSeed((static_cast<std::uint32_t>(cx) * 0x8da6b343u) ^
                                                (static_cast<std::uint32_t>(cy) * 0xd8163841u) ^ seed);
        switch (h & 7u) {
        case 0: return dx;
        case 1: return -dx;
        case 2: return dy;
        case 3: return -dy;
        case 4: return kDiag * (dx + dy);
        case 5: return kDiag * (dy - dx);
        case 6: return kDiag * (dx - dy);
        default: return -kDiag * (dx + dy);
        }
    };
    const auto fade = [](float t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); };
    const float u = fade(fx), v = fade(fy);
    const float bottom = lerpf(corner(ix, iy, fx, fy), corner(ix + 1, iy, fx - 1.f, fy), u);
    const float top = lerpf(corner(ix, iy + 1, fx, fy - 1.f), corner(ix + 1, iy + 1, fx - 1.f, fy - 1.f), u);
    return 1.41421356f * lerpf(bottom, top, v);
}

// Octaves of gradient noise, each twice as fine as the last and turned 37 degrees, scaled so the sum
// stays about -1..1. p is in units of the coarsest wavelength.
float fbm(Vec2 p, int octaves, float gain, std::uint32_t seed) {
    float sum = 0.f, amp = 1.f, norm = 0.f, x = p.x, y = p.y;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * gradientNoise(x, y, seed + static_cast<std::uint32_t>(o) * 0x9e3779b9u);
        norm += amp;
        amp *= gain;
        const float nx = 1.6f * x - 1.2f * y + 17.3f, ny = 1.2f * x + 1.6f * y - 9.9f;
        x = nx;
        y = ny;
    }
    return sum / norm;
}

// Ridged noise: sharp crests where gradient noise crosses zero, each octave weighted by the crest
// below it, so the detail gathers on the ridges and the valleys stay smooth. 0..1. p is in units of
// the coarsest wavelength.
float ridged(Vec2 p, int octaves, std::uint32_t seed) {
    float sum = 0.f, amp = 1.f, norm = 0.f, weight = 1.f, x = p.x, y = p.y;
    for (int o = 0; o < octaves; ++o) {
        float r = std::clamp(1.f - std::fabs(gradientNoise(x, y, seed + static_cast<std::uint32_t>(o) * 0x9e3779b9u)), 0.f, 1.f);
        r *= r * weight;
        weight = std::clamp(2.f * r, 0.f, 1.f);
        sum += amp * r;
        norm += amp;
        amp *= 0.5f;
        const float nx = 1.6f * x - 1.2f * y + 17.3f, ny = 1.2f * x + 1.6f * y - 9.9f;
        x = nx;
        y = ny;
    }
    return sum / norm;
}

// p moved by up to about `amount` metres, along a smooth field `wavelength` metres across.
Vec2 warp(Vec2 p, float wavelength, float amount, std::uint32_t seed) {
    const Vec2 q{p.x / wavelength, p.y / wavelength};
    return {p.x + amount * fbm(q, 2, 0.5f, seed ^ 0x2545f491u),
            p.y + amount * fbm({q.x + 5.2f, q.y + 1.3f}, 2, 0.5f, seed ^ 0x9e3779b9u)};
}

// A smooth bump: 1 at x = 0, 0 from |x| = 1 out.
float bump(float x) {
    const float k = 1.f - x * x;
    return k > 0.f ? k * k : 0.f;
}

// h held under `ceiling`: as it is up to kKnee below, then easing toward the ceiling without reaching
// it, with no crease at the knee.
float underCeiling(float h, float ceiling) {
    const float knee = ceiling - kKnee;
    return h <= knee ? h : ceiling - kKnee * std::exp((knee - h) / kKnee);
}

// What each kind of world does with the ground.
struct KindGround {
    WorldKind kind = WorldKind::Town;
    float half = 500.f; // the playable square's
    std::uint32_t seed = 1;
    std::uint32_t rangeSeed = 1; // the mountains': `seed` mixed with the kind, so that two kinds on one seed differ
    std::array<Vec2, 2> bendAt{};
    std::array<float, 2> bendBy{};
    std::size_t bends = 0;
    int coast = -1;  // the town's sea lies past this edge (past()), once chosen
    float sea = 0.f; // m: its level

    // How far p lies past the playable square's edge on `side` (0 +x, 1 +y, 2 -x, 3 -y): negative
    // inside.
    float past(Vec2 p, int side) const {
        const float along = side % 2 == 0 ? p.x : p.y;
        return (side < 2 ? along : -along) - half;
    }

    // The broad swell under everything else: slow noise, bent around the entry and the climax
    // (bend()).
    float swell(Vec2 p) const {
        float h = kSwell * fbm({p.x / kSwellWave + 7.7f, p.y / kSwellWave - 3.1f}, 2, 0.5f, seed ^ 0x27d4eb2fu);
        for (std::size_t k = 0; k < bends; ++k) h += bendBy[k] * (1.f - smooth01(terrainDist(p, bendAt[k]) / kSwellReach));
        return h;
    }

    // Bends the swell around up to two points so that it meets each one's `want` there exactly.
    void bend(std::size_t count, const Vec2* at, const float* want) {
        bends = 0;
        for (std::size_t k = 0; k < count; ++k) {
            bendAt[k] = at[k];
            bendBy[k] = want[k] - swell(at[k]);
        }
        if (count == 2) {
            const float c = 1.f - smooth01(terrainDist(at[0], at[1]) / kSwellReach), det = 1.f - c * c;
            const float miss0 = bendBy[0], miss1 = bendBy[1];
            bendBy[0] = (miss0 - c * miss1) / det;
            bendBy[1] = (miss1 - c * miss0) / det;
        }
        bends = count;
    }

    // The ground before the pads: the swell, and the kind's hills on it.
    float base(Vec2 p) const {
        const float s = swell(p);
        switch (kind) {
        case WorldKind::Town: {
            const Vec2 w = warp(p, 260.f, 36.f, seed);
            return s + 20.f * fbm({w.x / 300.f, w.y / 300.f}, 4, 0.45f, seed ^ 0x51ed270bu);
        }
        case WorldKind::Towers: {
            const Vec2 w = warp(p, 240.f, 30.f, seed);
            return s + 24.f * fbm({w.x / 240.f, w.y / 240.f}, 4, 0.45f, seed ^ 0x51ed270bu);
        }
        case WorldKind::Ruins: break;
        }
        const Vec2 w = warp(p, 380.f, 40.f, seed);
        return s + 6.f * fbm({w.x / 380.f, w.y / 380.f}, 3, 0.5f, seed ^ 0x51ed270bu);
    }

    // The mountains around the playable square, in metres over `ground`, the ground beneath them:
    // nothing inside the square, a broken band of rock at their foot, then ridges climbing toward the
    // field's edge over valleys that rise to kRangeFloor. Their foot wanders from the edge to
    // kFootWander out, by noise taken from the edge's point nearest p so that it never folds back, and
    // the band breaks off in stretches, so that the foot never traces the square. In town they stand
    // off the dry edges only, so past the coast edge they rise only beside the sea (apron()).
    float range(Vec2 p, float ground) const {
        float ex = std::max(past(p, 0), past(p, 2)), ey = std::max(past(p, 1), past(p, 3));
        if (coast >= 0) {
            const float dry = past(p, (coast + 2) % 4); // on the coast's axis only the far edge counts
            if (coast % 2 == 0) ex = dry;
            else ey = dry;
        }
        if (ex <= 0.f && ey <= 0.f) return 0.f;
        const Vec2 edge{std::clamp(p.x, -half, half), std::clamp(p.y, -half, half)};
        const float wander =
            std::clamp(0.5f + 0.9f * fbm({edge.x / 180.f + 2.3f, edge.y / 180.f - 6.7f}, 3, 0.5f, rangeSeed ^ 0x85ebca6bu), 0.f, 1.f);
        const float foot = std::hypot(std::max(ex, 0.f), std::max(ey, 0.f)) - kFootWander * wander;
        if (foot <= 0.f) return 0.f;
        const float floor = std::max(kRangeFloor - ground, kRangeFloorMin);
        const float crest = ridged({p.x / 230.f, p.y / 230.f}, 5, rangeSeed ^ 0xc2b2ae35u);
        const float band =
            kFootBand * smooth01((fbm({p.x / 60.f - 5.3f, p.y / 60.f + 7.1f}, 3, 0.5f, rangeSeed ^ 0xd3a2646cu) + 0.05f) / 0.35f);
        return band * smooth01(foot / kFootRun) + smooth01(foot / kRangeClimb) * (floor + kRangeRidge * crest);
    }

    // How far p lies past the ruins' lip: their ground runs on past the edge kAbyssLip to kAbyssLip +
    // kAbyssSpread, in bays and headlands a few hundred metres long, jagged by kAbyssJag more finely.
    // Each point takes its noise kAbyssProbe out along the way to it from the edge's point nearest it,
    // so every point on that way takes the same and the lip never folds back, and round a corner the
    // noise still changes, so that no corner is a turned arc. Negative short of it: inside the square,
    // by kAbyssLip - kAbyssJag at least.
    float pastLip(Vec2 p) const {
        const float ex = std::fabs(p.x) - half, ey = std::fabs(p.y) - half;
        const float beyond = std::hypot(std::max(ex, 0.f), std::max(ey, 0.f));
        const Vec2 edge{std::clamp(p.x, -half, half), std::clamp(p.y, -half, half)};
        const float probe = beyond > 0.f ? kAbyssProbe / beyond : 0.f;
        const Vec2 q{edge.x + (p.x - edge.x) * probe, edge.y + (p.y - edge.y) * probe};
        const float spread = smooth01(0.5f + fbm({q.x / 200.f, q.y / 200.f}, 3, 0.5f, seed ^ 0x27220a95u));
        const float jag = kAbyssJag * std::clamp(fbm({q.x / 12.f + 6.1f, q.y / 12.f - 2.7f}, 2, 0.5f, seed ^ 0x7f4a7c15u),
                                                 -1.f, 1.f);
        return beyond - (kAbyssLip + kAbyssSpread * spread + jag);
    }

    // Past their lip the ruins' ground crumbles kAbyssCrumble over kAbyssCrumbleRun, then falls sheer
    // over kAbyssBreak into the abyss, the fall wavering kAbyssJitter nearer or further. In metres
    // over `ground`.
    float abyss(Vec2 p, float ground) const {
        const float d = pastLip(p);
        if (d <= 0.f) return 0.f;
        const float crumble = kAbyssCrumble * smooth01(d / kAbyssCrumbleRun);
        const float jitter = kAbyssJitter * std::clamp(fbm({p.x / 8.f + 1.7f, p.y / 8.f - 3.9f}, 2, 0.5f, seed ^ 0x3243f6a9u),
                                                       -1.f, 1.f);
        const float fall = smooth01((d - kAbyssCrumbleRun - jitter) / kAbyssBreak);
        const float floor = std::clamp(kAbyssFloor + kAbyssRoll * fbm({p.x / 200.f - 3.3f, p.y / 200.f + 8.8f}, 3, 0.5f, seed ^ 0x4cf5ad43u),
                                       kAbyssFloor - kAbyssRoll, kAbyssFloor + kAbyssRoll);
        return lerpf(-crumble, floor - ground, fall);
    }

    // What becomes of the ground past the playable square's edge, in metres over `ground`: the ruins
    // break off into the abyss, the towers stand in mountains, and so does the town, but past its
    // coast edge a quay drops kQuayRun into the sea and the sea's floor falls away beyond, while the
    // ranges either side run on into it as headlands, a ragged way out.
    float apron(Vec2 p, float ground) const {
        if (kind == WorldKind::Ruins) return abyss(p, ground);
        const float c = coast >= 0 ? past(p, coast) : 0.f;
        if (c <= 0.f) return range(p, ground);
        const float bed = sea - kQuayFoot - kSeaDeep * smooth01((c - kQuayRun) / kSeaFall);
        const float shore = (bed - ground) * smooth01(c / kQuayRun);
        const float along = coast % 2 == 0 ? p.y : p.x;
        const float reach = kHeadland + kHeadlandSpread * std::clamp(0.5f + 0.5f * fbm({along / 110.f + 4.1f, 2.7f}, 2, 0.5f,
                                                                                        seed ^ 0x165667b1u),
                                                                     0.f, 1.f);
        return shore + (1.f - smooth01((c - reach) / kHeadlandFade)) * range(p, ground + shore);
    }

    // What the kind makes of the ground once the pads are blended in. `alpha` is how far into a
    // pad's blend p lies (0 outside them all), `open` how far outside the nearest blend. The ruins
    // raise mesas and cut gullies outside the blends only: a mesa rises from nothing at its edge,
    // and a gully climbs out as it nears a blend or a mesa, and in broken stretches along its
    // length, so that no gully is a pit. `clear` (0..1) is how freely a mesa may grow at p, and its
    // top eases under `ceiling`. Where either is steeper than kRockSlope, the slope limit cuts it back.
    float finish(Vec2 p, float h, float alpha, float open, float clear, float ceiling) const {
        if (kind != WorldKind::Ruins || alpha > 0.f) return h;
        const float mesa = fbm({p.x / 210.f - 4.4f, p.y / 210.f + 2.9f}, 3, 0.5f, seed ^ 0x1b873593u) - kMesaFrom -
                           kMesaBar * (1.f - clear);
        if (mesa > 0.f)
            return underCeiling(h + (kMesaHeight + kMesaRise * smooth01(mesa / 0.25f)) * smooth01(mesa / 0.15f), ceiling);
        const float gully = fbm({p.x / 260.f + 9.1f, p.y / 260.f + 6.3f}, 2, 0.5f, seed ^ 0xcc9e2d51u);
        const float broken = fbm({p.x / 90.f + 1.9f, p.y / 90.f - 4.2f}, 2, 0.5f, seed ^ 0x3c6ef372u);
        return h - kGullyDepth * (1.f - smooth01((std::fabs(gully) - 0.02f) / 0.04f)) * smooth01((broken + 0.25f) / 0.3f) *
                       smooth01(open / kGullyRamp) * smooth01(-mesa / 0.1f);
    }
};

// The line from an eye over the entry pad to a point over the climax's centre, and the ceiling it
// sets the ground: kSightClear under the line within kCorridorHalf of it, rising at kCorridorSlope
// beyond. Past either end the line keeps that end's height, so the ceiling has no seam anywhere.
struct Sightline {
    Vec2 from, to;
    float zFrom = 0.f, zTo = 0.f;
    bool on = false;

    float ceiling(Vec2 p) const {
        if (!on) return 1e30f;
        float d = 0.f;
        const float t = nearestOnSegment(p, from, to, d);
        return lerpf(zFrom, zTo, t) - kSightClear + kCorridorSlope * std::max(d - kCorridorHalf, 0.f);
    }
};

enum class PadShape : std::uint8_t {
    Plain, // blends into the ground around it
    Hill,  // the same, over a wider blend
    Bowl,  // sunk, with a crest around it
    Butte, // raised, over a tight, steep blend
};

struct Pad {
    Vec2 centre;
    float height = 0.f;
    float blend = 0.f; // m of blend outside the flat radius
    PadShape shape = PadShape::Plain;
    bool site = false;
    bool settled = false;
};

// A site's shape, and how far it stands above the swell around it (for the climax: above the
// swell bent to meet it, as the climax itself stands kClimaxWant high where it can). By kind: town
// hills and bowls, tower rises, a ruin butte over sunken bowls.
void siteDesign(WorldKind kind, SiteTier tier, std::size_t district, float& rise, PadShape& shape) {
    rise = 0.f;
    shape = PadShape::Plain;
    if (tier == SiteTier::Entry) return;
    const bool climax = tier == SiteTier::Climax;
    switch (kind) {
    case WorldKind::Town:
        if (climax || district % 2 == 0) {
            rise = climax ? 12.f : 10.f;
            shape = PadShape::Hill;
        } else {
            rise = -6.f;
            shape = PadShape::Bowl;
        }
        return;
    case WorldKind::Towers: rise = climax ? 6.f : 12.f; return;
    case WorldKind::Ruins:
        rise = climax ? 15.f : -6.f;
        shape = climax ? PadShape::Butte : PadShape::Bowl;
        return;
    }
}

// sum(coef[k] * height[pad[k]]) lies in lo..hi.
struct Constraint {
    std::array<std::uint8_t, 4> pad{};
    std::array<float, 4> coef{};
    std::uint8_t count = 0;
    float lo = 0.f, hi = 0.f;
};

void addConstraint(std::vector<Constraint>& out, std::initializer_list<std::pair<std::size_t, float>> terms, float lo,
                   float hi) {
    Constraint c;
    for (const auto& [pad, coef] : terms) {
        c.pad[c.count] = static_cast<std::uint8_t>(pad);
        c.coef[c.count] = coef;
        ++c.count;
    }
    c.lo = lo;
    c.hi = hi;
    out.push_back(c);
}

// The rain's dice: xorshift32, as the layout's.
struct WearRng {
    std::uint32_t s;
    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float range(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(next() >> 8) * (1.f / 16777216.f); }
};

// Rain on a field of n x n heights: `drops` droplets, each set down at random in the box from `from`
// to `to` (in samples, along both axes), runs downhill a cell a step for kWearLife steps at most.
// Where it can carry more than it holds it cuts the ground over a brush `brush` samples round it
// (kMaxWearBrush at most), and where it can't, or the ground rises ahead, it lays some of what it holds on the
// cell it leaves; what it may carry grows with its fall, its speed and its water, times the
// `weight` (0 to 1) where it fell, so that rain falling by ground that doesn't wear mostly drains
// away, and none runs from weight 0. Each sample takes only its `weight` of every cut or deposit,
// so a sample of weight 0 never changes and a droplet only carries what it took. Deterministic in
// the seed; no allocation.
void wear(float* heights, const float* weight, std::size_t n, int drops, int brush, float from, float to, std::uint32_t seed) {
    constexpr int kSpan = 2 * kMaxWearBrush + 1;
    std::array<std::ptrdiff_t, kSpan * kSpan> brushAt{};
    std::array<float, kSpan * kSpan> brushBy{};
    std::size_t brushSize = 0;
    float brushSum = 0.f;
    brush = std::clamp(brush, 1, kMaxWearBrush);
    for (int dy = -brush; dy <= brush; ++dy)
        for (int dx = -brush; dx <= brush; ++dx) {
            const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
            if (d >= static_cast<float>(brush)) continue;
            brushAt[brushSize] = static_cast<std::ptrdiff_t>(dy) * static_cast<std::ptrdiff_t>(n) + dx;
            brushBy[brushSize] = 1.f - d / static_cast<float>(brush);
            brushSum += brushBy[brushSize++];
        }
    for (std::size_t k = 0; k < brushSize; ++k) brushBy[k] /= brushSum;

    // A droplet stays where its cell and its brush lie inside the field.
    const float edgeLo = static_cast<float>(brush), edgeHi = static_cast<float>(n - 2) - static_cast<float>(brush);
    WearRng dice{seed};
    for (int drop = 0; drop < drops; ++drop) {
        float x = dice.range(from, to), y = dice.range(from, to);
        // Rain that falls where the ground wears little, by a pad or a road, mostly drains away.
        const float rain = weight[static_cast<std::size_t>(y) * n + static_cast<std::size_t>(x)];
        if (!(rain > 0.f)) continue;
        float wayX = 0.f, wayY = 0.f, speed = 1.f, water = 1.f, load = 0.f;
        for (int step = 0; step < kWearLife; ++step) {
            // The ground under the droplet and its slope, between the four samples round it.
            const std::size_t cx = static_cast<std::size_t>(x), cy = static_cast<std::size_t>(y), at = cy * n + cx;
            const float u = x - static_cast<float>(cx), v = y - static_cast<float>(cy);
            const float* corner = heights + at;
            const float gx = (corner[1] - corner[0]) * (1.f - v) + (corner[n + 1] - corner[n]) * v;
            const float gy = (corner[n] - corner[0]) * (1.f - u) + (corner[n + 1] - corner[1]) * u;
            const float here = (corner[0] * (1.f - u) + corner[1] * u) * (1.f - v) + (corner[n] * (1.f - u) + corner[n + 1] * u) * v;

            wayX = wayX * kWearInertia - gx * (1.f - kWearInertia);
            wayY = wayY * kWearInertia - gy * (1.f - kWearInertia);
            const float len = std::sqrt(wayX * wayX + wayY * wayY);
            if (!(len > 1e-12f)) break; // on the flat with nowhere to go
            wayX /= len;
            wayY /= len;
            x += wayX;
            y += wayY;
            if (!(x >= edgeLo && x < edgeHi && y >= edgeLo && y < edgeHi)) break;

            const std::size_t nx = static_cast<std::size_t>(x), ny = static_cast<std::size_t>(y);
            const float nu = x - static_cast<float>(nx), nv = y - static_cast<float>(ny);
            const float* next = heights + ny * n + nx;
            const float there = (next[0] * (1.f - nu) + next[1] * nu) * (1.f - nv) + (next[n] * (1.f - nu) + next[n + 1] * nu) * nv;
            const float fall = here - there;
            const float capacity = std::max(fall * speed * water * kWearCapacity, kWearLeast) * rain;
            if (load > capacity || fall < 0.f) {
                // Fill the hollow it climbs out of, or lay down what it can no longer carry.
                const float laid = fall < 0.f ? std::min(-fall, load) : (load - capacity) * kWearLay;
                const float w00 = (1.f - u) * (1.f - v) * weight[at], w10 = u * (1.f - v) * weight[at + 1];
                const float w01 = (1.f - u) * v * weight[at + n], w11 = u * v * weight[at + n + 1];
                heights[at] += laid * w00;
                heights[at + 1] += laid * w10;
                heights[at + n] += laid * w01;
                heights[at + n + 1] += laid * w11;
                load -= laid * (w00 + w10 + w01 + w11);
            } else {
                // Cut, never deeper than its fall, so that it digs no pit.
                const float cut = std::min((capacity - load) * kWearCut, fall);
                for (std::size_t k = 0; k < brushSize; ++k) {
                    const std::size_t j = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(at) + brushAt[k]);
                    const float taken = cut * brushBy[k] * weight[j];
                    heights[j] -= taken;
                    load += taken;
                }
            }
            speed = std::sqrt(std::max(speed * speed + fall * kWearGravity, 0.f));
            water *= 1.f - kWearDry;
        }
    }
}

} // namespace

std::uint16_t WorldTerrain::heightmapValue(std::size_t ix, std::size_t iy) const {
    const long v = 32768L + std::lround(sample(ix, iy) / kHeightStep);
    return static_cast<std::uint16_t>(std::clamp(v, 0L, 65535L));
}

float WorldTerrain::height(Vec2 p) const {
    if (heights_.empty()) return 0.f;
    const float cell = spacing(), last = static_cast<float>(kSamples - 1);
    const float gx = std::clamp((p.x + extent() * 0.5f) / cell, 0.f, last);
    const float gy = std::clamp((p.y + extent() * 0.5f) / cell, 0.f, last);
    const std::size_t ix = std::min(static_cast<std::size_t>(gx), kSamples - 2);
    const std::size_t iy = std::min(static_cast<std::size_t>(gy), kSamples - 2);
    const float fx = gx - static_cast<float>(ix), fy = gy - static_cast<float>(iy);
    const float h00 = sample(ix, iy), h10 = sample(ix + 1, iy), h01 = sample(ix, iy + 1), h11 = sample(ix + 1, iy + 1);
    if (fx >= fy) return h00 + (h10 - h00) * fx + (h11 - h10) * fy;
    return h00 + (h11 - h01) * fx + (h01 - h00) * fy;
}

Vec2 WorldTerrain::gradient(Vec2 p) const {
    if (heights_.empty()) return {};
    const float cell = spacing(), last = static_cast<float>(kSamples - 1);
    const float gx = std::clamp((p.x + extent() * 0.5f) / cell, 0.f, last);
    const float gy = std::clamp((p.y + extent() * 0.5f) / cell, 0.f, last);
    const std::size_t ix = std::min(static_cast<std::size_t>(gx), kSamples - 2);
    const std::size_t iy = std::min(static_cast<std::size_t>(gy), kSamples - 2);
    const float fx = gx - static_cast<float>(ix), fy = gy - static_cast<float>(iy);
    const float h00 = sample(ix, iy), h10 = sample(ix + 1, iy), h01 = sample(ix, iy + 1), h11 = sample(ix + 1, iy + 1);
    if (fx >= fy) return {(h10 - h00) / cell, (h11 - h10) / cell};
    return {(h11 - h01) / cell, (h01 - h00) / cell};
}

float WorldTerrain::roadHeight(std::size_t road, float t) const {
    const Road& r = roads_[road];
    return roadProfile(r.length, r.from, r.to, std::clamp(t, 0.f, 1.f) * r.length);
}

float WorldTerrain::waterDepth(Vec2 p) const {
    const float ground = height(p);
    float depth = 0.f;
    for (std::size_t i = 0; i < featureCount_; ++i) {
        const TerrainFeature& f = features_[i];
        const bool water = f.kind == TerrainFeature::Kind::Sea || f.kind == TerrainFeature::Kind::Lake ||
                           f.kind == TerrainFeature::Kind::Pool;
        if (water && f.covers(p)) depth = std::max(depth, f.level - ground);
    }
    return depth;
}

float TerrainFeature::radius(float angle) const {
    constexpr float kTau = 6.28318531f;
    float u = angle / kTau * static_cast<float>(kRays);
    u -= std::floor(u / static_cast<float>(kRays)) * static_cast<float>(kRays);
    const std::size_t k = static_cast<std::size_t>(u) % kRays;
    return lerpf(ray[k], ray[(k + 1) % kRays], u - std::floor(u));
}

bool TerrainFeature::covers(Vec2 p, float inset) const {
    switch (kind) {
    case Kind::Sea:
        switch (side) {
        case 0: return p.x > centre.x + inset;
        case 1: return p.y > centre.y + inset;
        case 2: return p.x < centre.x - inset;
        default: return p.y < centre.y - inset;
        }
    case Kind::Pool:
    case Kind::Trench: return std::fabs(p.x - centre.x) < half.x - inset && std::fabs(p.y - centre.y) < half.y - inset;
    case Kind::Lake:
    case Kind::Pit: break;
    }
    const float dx = p.x - centre.x, dy = p.y - centre.y;
    return std::hypot(dx, dy) < radius(std::atan2(dy, dx)) - inset;
}

bool generateWorldTerrain(const WorldSpec& spec, const WorldLayout& layout, WorldTerrain& out, std::string* error,
                          const TerrainOptions& options) {
    const auto fail = [&](const std::string& why) {
        out.side_ = 0.f;
        out.districtCount_ = out.roadCount_ = out.featureCount_ = 0;
        out.heights_.clear();
        out.cliffs_.clear();
        out.flow_.clear();
        if (error) *error = "terrain " + spec.id + ": " + why;
        return false;
    };
    const std::size_t n = layout.districtCount();
    if (n == 0) return fail("the layout has no districts");
    if (layout.side() != spec.side || layout.kind() != spec.kind) return fail("the layout isn't this world's");

    // The pads, the sites first.
    std::array<Pad, kMaxPads> pads{};
    for (std::size_t i = 0; i < n; ++i) pads[i].centre = layout.district(i).centre;
    int entry = -1, climax = -1;
    std::array<float, kMaxPads> design{};
    for (const WorldSite& s : spec.sites) {
        if (s.district >= n)
            return fail("site district " + std::to_string(s.district) + " is past the layout's " + std::to_string(n));
        Pad& pad = pads[s.district];
        if (pad.site) return fail("district " + std::to_string(s.district) + " is a site twice");
        if ((s.tier == SiteTier::Entry && entry >= 0) || (s.tier == SiteTier::Climax && climax >= 0))
            return fail("more than one entry or climax");
        if (s.tier == SiteTier::Entry) entry = static_cast<int>(s.district);
        if (s.tier == SiteTier::Climax) climax = static_cast<int>(s.district);
        pad.site = true;
        siteDesign(spec.kind, s.tier, s.district, design[s.district], pad.shape);
    }

    // Roads, and how far apart two pads' heights may be for every road between them to keep to
    // kMaxRoadGrade (shortest ways over the roads, Floyd-Warshall). Pad heights come in whole
    // kHeightStep, and so does each road's rise, so the intervals these set always end on whole steps.
    const bool towers = spec.kind == WorldKind::Towers;
    const float unit = WorldTerrain::kHeightStep;
    const std::size_t roads = layout.roadCount();
    std::array<float, WorldLayout::kMaxRoads> length{}, ramp{}, rise{};
    std::array<std::array<float, kMaxPads>, kMaxPads> apart{};
    for (std::size_t a = 0; a < n; ++a)
        for (std::size_t b = 0; b < n; ++b) apart[a][b] = a == b ? 0.f : 1e30f;
    for (std::size_t r = 0; r < roads; ++r) {
        const WorldRoad& road = layout.road(r);
        length[r] = terrainDist(pads[road.a].centre, pads[road.b].centre);
        ramp[r] = std::max(length[r] - 2.f * kR, 1.f);
        rise[r] = std::floor(WorldTerrain::kMaxRoadGrade * ramp[r] / unit) * unit;
        apart[road.a][road.b] = apart[road.b][road.a] = std::min(apart[road.a][road.b], rise[r]);
    }
    for (std::size_t k = 0; k < n; ++k)
        for (std::size_t a = 0; a < n; ++a)
            for (std::size_t b = 0; b < n; ++b) apart[a][b] = std::min(apart[a][b], apart[a][k] + apart[k][b]);

    // What else the pads' heights must keep to so that the ground between them stays walkable: the
    // roads' grades, pads close together, each road against the pads it passes and against the
    // roads near it (its banks have to meet theirs at kBankSlope).
    std::vector<Constraint> rules;
    rules.reserve(1024);
    for (std::size_t r = 0; r < roads; ++r)
        addConstraint(rules, {{layout.road(r).a, 1.f}, {layout.road(r).b, -1.f}}, -rise[r], rise[r]);
    const std::size_t nearFirst = rules.size();
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = i + 1; j < n; ++j) {
            const float gap = terrainDist(pads[i].centre, pads[j].centre) - 2.f * kR;
            if (layout.joined(i, j) || gap >= 112.f) continue;
            const float most = kTan32 * gap * 0.5f;
            addConstraint(rules, {{i, 1.f}, {j, -1.f}}, -most, most);
        }
    const std::size_t nearEnd = rules.size();
    constexpr float kSampleStep = 10.f; // m between the points of a road checked against its neighbours
    for (std::size_t r = 0; r < roads; ++r) {
        const WorldRoad& road = layout.road(r);
        const Vec2 a = pads[road.a].centre, b = pads[road.b].centre;
        for (float s = kR; s <= length[r] - kR; s += kSampleStep) {
            const float t = s / length[r];
            const Vec2 p{lerpf(a.x, b.x, t), lerpf(a.y, b.y, t)};
            const float u = std::clamp((s - kR) / ramp[r], 0.f, 1.f);
            for (std::size_t k = 0; k < n; ++k) {
                if (k == road.a || k == road.b) continue;
                const float gap = terrainDist(p, pads[k].centre) - kR - kLevel;
                if (gap > 50.f) continue;
                const float most = WorldTerrain::kBankSlope * std::max(gap, 0.f) + 0.1f;
                addConstraint(rules, {{road.a, 1.f - u}, {road.b, u}, {k, -1.f}}, -most, most);
            }
            for (std::size_t q = r + 1; q < roads; ++q) {
                const WorldRoad& other = layout.road(q);
                if (other.a == road.a || other.a == road.b || other.b == road.a || other.b == road.b) continue;
                float d = 0.f;
                const float tq = nearestOnSegment(p, pads[other.a].centre, pads[other.b].centre, d);
                const float sq = tq * length[q];
                const float gap = d - 2.f * kLevel;
                if (sq < kR || sq > length[q] - kR || gap > 50.f) continue;
                const float uq = std::clamp((sq - kR) / ramp[q], 0.f, 1.f);
                const float most = WorldTerrain::kBankSlope * std::max(gap, 0.f) + 0.02f;
                addConstraint(rules, {{road.a, 1.f - u}, {road.b, u}, {other.a, uq - 1.f}, {other.b, -uq}}, -most, most);
            }
        }
    }

    // The sightline from an eye over the entry pad to a point over the climax's centre (Sightline).
    // Its ends rise and fall with those two pads, so the rules that keep each other pad under its
    // ceiling (at the pad's edge nearest the entry) and each road near it tie them to those two.
    Sightline sight;
    const std::size_t sightFirst = rules.size();
    if (entry >= 0 && climax >= 0) {
        const std::size_t e = static_cast<std::size_t>(entry), c = static_cast<std::size_t>(climax);
        const Vec2 from = pads[e].centre, to = pads[c].centre;
        const float inset = (kR - kEyeInset) / terrainDist(from, to);
        sight.from = {lerpf(from.x, to.x, inset), lerpf(from.y, to.y, inset)};
        sight.to = to;
        const float reach = terrainDist(sight.from, sight.to);
        // The ceiling at t along the line and d from it, less the two pads' heights' share of it.
        const auto spare = [](float t, float d) {
            return lerpf(kEyeHeight, kAimHeight, t) - kSightClear + kCorridorSlope * std::max(d - kCorridorHalf, 0.f);
        };
        // sum(coef * height) over `terms` stays under the ceiling at t, whose spare there is `hi`.
        const auto addUnder = [&](std::initializer_list<std::pair<std::size_t, float>> terms, float t, float hi) {
            Constraint rule;
            const auto put = [&rule](std::size_t pad, float coef) {
                std::uint8_t m = 0;
                while (m < rule.count && rule.pad[m] != pad) ++m;
                if (m == rule.count) rule.pad[rule.count++] = static_cast<std::uint8_t>(pad);
                rule.coef[m] += coef;
            };
            for (const auto& [pad, coef] : terms) put(pad, coef);
            put(e, t - 1.f);
            put(c, -t);
            rule.lo = -1e30f;
            rule.hi = hi;
            rules.push_back(rule);
        };
        for (std::size_t k = 0; k < n; ++k) {
            if (k == e || k == c) continue;
            float d = 0.f;
            const float t = std::max(nearestOnSegment(pads[k].centre, sight.from, sight.to, d) - kR / reach, 0.f);
            addUnder({{k, 1.f}}, t, spare(t, std::max(d - kR, 0.f)));
        }
        for (std::size_t r = 0; r < roads; ++r) {
            const WorldRoad& road = layout.road(r);
            const Vec2 a = pads[road.a].centre, b = pads[road.b].centre;
            for (float s = kR;; s = std::min(s + kSampleStep, length[r] - kR)) {
                const float along = s / length[r], u = std::clamp((s - kR) / ramp[r], 0.f, 1.f);
                float d = 0.f;
                const float t = nearestOnSegment({lerpf(a.x, b.x, along), lerpf(a.y, b.y, along)}, sight.from, sight.to, d);
                if (d < kCorridorHalf + 40.f) addUnder({{road.a, 1.f - u}, {road.b, u}}, t, spare(t, d));
                if (s >= length[r] - kR) break;
            }
        }
    }
    const std::size_t sightEnd = rules.size();

    // Settles one pad on the whole unit nearest `want` that the pads settled so far allow: inside
    // every road-grade interval they set (always possible, as they keep to them among themselves),
    // then inside each rule that this pad alone still moves, while that leaves a whole unit, and
    // where it doesn't, on the whole unit nearest the rule that the rest allow.
    const auto settle = [&](std::size_t k, float want) {
        float lo = -kMaxPad, hi = kMaxPad;
        for (std::size_t j = 0; j < n; ++j)
            if (pads[j].settled) {
                lo = std::max(lo, pads[j].height - apart[k][j]);
                hi = std::min(hi, pads[j].height + apart[k][j]);
            }
        for (const Constraint& c : rules) {
            float rest = 0.f, mine = 0.f;
            bool involved = false, open = false;
            for (std::uint8_t m = 0; m < c.count; ++m) {
                if (c.pad[m] == k) {
                    involved = true;
                    mine += c.coef[m];
                } else if (pads[c.pad[m]].settled) {
                    rest += c.coef[m] * pads[c.pad[m]].height;
                } else {
                    open = true;
                }
            }
            if (!involved || open || std::fabs(mine) < 1e-4f) continue;
            float clo = (c.lo - rest) / mine, chi = (c.hi - rest) / mine;
            if (mine < 0.f) std::swap(clo, chi);
            const float qlo = std::ceil(std::max(lo, clo) / unit) * unit, qhi = std::floor(std::min(hi, chi) / unit) * unit;
            if (qlo <= qhi) {
                lo = qlo;
                hi = qhi;
            } else {
                lo = hi = chi < lo ? lo : clo > hi ? hi : std::clamp(std::round(0.5f * (clo + chi) / unit) * unit, lo, hi);
            }
        }
        pads[k].height = std::clamp(std::round(std::clamp(want, lo, hi) / unit) * unit, lo, hi);
        pads[k].settled = true;
    };
    // The entry at the base and the climax kClimaxWant high, where the roads allow; then the swell
    // bends to meet them both, and the majors stand above or below the swell where they are.
    KindGround ground{spec.kind, spec.side * 0.5f, detail::mixSeed(spec.seed ^ 0x7e5a1c3du)};
    ground.rangeSeed = detail::mixSeed(ground.seed ^ (0x632be5abu * (static_cast<std::uint32_t>(spec.kind) + 1u)));
    if (entry >= 0) settle(static_cast<std::size_t>(entry), 0.f);
    // The climax stands no higher than the sightline can still clear the pads and roads between.
    // With the near pairs' limits joined to the roads' (`differ`), each other pad's lowest height
    // follows from the entry's and the climax's, and every rule under the sightline has to hold with
    // the pads that low: the highest whole unit up to kClimaxWant where they all do, or kClimaxWant if
    // none does. (Settling the climax first and the rest after can leave a pad no room under the line.)
    float climaxWant = kClimaxWant;
    if (entry >= 0 && climax >= 0) {
        const std::size_t e = static_cast<std::size_t>(entry), c = static_cast<std::size_t>(climax);
        auto differ = apart;
        for (std::size_t ri = nearFirst; ri < nearEnd; ++ri) {
            const std::size_t i = rules[ri].pad[0], j = rules[ri].pad[1];
            differ[i][j] = differ[j][i] = std::min(differ[i][j], rules[ri].hi);
        }
        for (std::size_t k = 0; k < n; ++k)
            for (std::size_t a = 0; a < n; ++a)
                for (std::size_t b = 0; b < n; ++b) differ[a][b] = std::min(differ[a][b], differ[a][k] + differ[k][b]);
        const float he = pads[e].height;
        const auto clears = [&](float hc) {
            for (std::size_t ri = sightFirst; ri < sightEnd; ++ri) {
                const Constraint& rule = rules[ri];
                float sum = 0.f;
                // Only the entry's and the climax's terms fall, so each other pad counts at its lowest.
                for (std::uint8_t m = 0; m < rule.count; ++m) {
                    const std::size_t x = rule.pad[m];
                    sum += rule.coef[m] * (x == e ? he : x == c ? hc : std::max(hc - differ[x][c], he - differ[x][e]));
                }
                if (sum > rule.hi + 1e-4f) return false;
            }
            return true;
        };
        for (float hc = kClimaxWant; hc >= he; hc -= unit)
            if (clears(hc)) {
                climaxWant = hc;
                break;
            }
    }
    if (climax >= 0) settle(static_cast<std::size_t>(climax), climaxWant);
    const float top = climax >= 0 ? pads[static_cast<std::size_t>(climax)].height : kMaxPad;
    if (entry >= 0 && climax >= 0) {
        sight.zFrom = pads[static_cast<std::size_t>(entry)].height + kEyeHeight;
        sight.zTo = top + kAimHeight;
        sight.on = true;
    }
    {
        std::array<Vec2, 2> at{};
        std::array<float, 2> want{};
        std::size_t count = 0;
        for (const int k : {entry, climax}) {
            if (k < 0) continue;
            const Pad& pad = pads[static_cast<std::size_t>(k)];
            at[count] = pad.centre;
            want[count++] = pad.height - design[static_cast<std::size_t>(k)];
        }
        ground.bend(count, at.data(), want.data());
    }
    // A major stays kClimaxClear under the climax.
    for (std::size_t i = 0; i < n; ++i)
        if (pads[i].site && !pads[i].settled) settle(i, std::min(ground.swell(pads[i].centre) + design[i], top - kClimaxClear));

    // The ground between the pads eases toward a ceiling it never meets: kClimaxClear under the
    // climax, and under the sightline.
    const float climaxCeiling = climax >= 0 ? top - kClimaxClear : 1e30f;
    const auto cappedBase = [&](Vec2 p) { return underCeiling(ground.base(p), std::min(climaxCeiling, sight.ceiling(p))); };

    // The other pads start level with the ground they stand on, stay under the climax, and relax
    // against the rules (Kaczmarz: project onto each in turn, moving only these pads).
    for (std::size_t i = 0; i < n; ++i) {
        if (pads[i].settled) continue;
        float sum = cappedBase(pads[i].centre);
        for (const Vec2 d : kCompass) sum += cappedBase({pads[i].centre.x + 30.f * d.x, pads[i].centre.y + 30.f * d.y});
        pads[i].height = std::clamp(sum / 9.f, -kMaxPad, kMaxPad);
        if (climax >= 0) addConstraint(rules, {{i, 1.f}}, -kMaxPad, top - 1.f);
    }
    for (int sweep = 0; sweep < kRelaxSweeps; ++sweep) {
        float worst = 0.f;
        for (const Constraint& c : rules) {
            float v = 0.f, den = 0.f;
            for (std::uint8_t m = 0; m < c.count; ++m) {
                v += c.coef[m] * pads[c.pad[m]].height;
                if (!pads[c.pad[m]].settled) den += c.coef[m] * c.coef[m];
            }
            const float over = v > c.hi ? v - c.hi : v < c.lo ? v - c.lo : 0.f;
            if (over == 0.f || den < 1e-6f) continue;
            worst = std::max(worst, std::fabs(over));
            for (std::uint8_t m = 0; m < c.count; ++m)
                if (!pads[c.pad[m]].settled) pads[c.pad[m]].height -= over * c.coef[m] / den;
        }
        if (worst < 1e-3f) break;
    }
    for (std::size_t i = 0; i < n; ++i)
        if (!pads[i].settled) settle(i, pads[i].height);

    // How wide each pad's blend is: wide enough that the blend's steepest part stays near 30
    // degrees for the deepest drop to the ground around it (a butte's near kButteSlope), within the
    // shape's range.
    for (std::size_t i = 0; i < n; ++i) {
        Pad& pad = pads[i];
        float drop = 0.f;
        for (const Vec2 d : kCompass)
            drop = std::max(drop, std::fabs(pad.height - cappedBase({pad.centre.x + (kR + 30.f) * d.x,
                                                                     pad.centre.y + (kR + 30.f) * d.y})));
        const float wide = 1.5f * drop / kTan30;
        if (pad.shape == PadShape::Butte) pad.blend = std::clamp(1.5f * drop / kButteSlope, 20.f, 48.f);
        else if (pad.shape == PadShape::Bowl) pad.blend = 40.f;
        else if (pad.shape == PadShape::Hill) pad.blend = std::clamp(wide, 40.f, 60.f);
        else if (static_cast<int>(i) == entry) pad.blend = 44.f;
        else pad.blend = std::clamp(wide, 34.f, 56.f);
    }

    // The ground, sample by sample.
    struct RoadSpan {
        Vec2 a, b;
        float minX, maxX, minY, maxY;
        float from, to;
    };
    std::array<RoadSpan, WorldLayout::kMaxRoads> spans{};
    for (std::size_t r = 0; r < roads; ++r) {
        RoadSpan& sp = spans[r];
        sp.a = pads[layout.road(r).a].centre;
        sp.b = pads[layout.road(r).b].centre;
        sp.minX = std::min(sp.a.x, sp.b.x) - kReach;
        sp.maxX = std::max(sp.a.x, sp.b.x) + kReach;
        sp.minY = std::min(sp.a.y, sp.b.y) - kReach;
        sp.maxY = std::max(sp.a.y, sp.b.y) + kReach;
        sp.from = pads[layout.road(r).a].height;
        sp.to = pads[layout.road(r).b].height;
    }
    const float extent = spec.side + 2.f * WorldTerrain::kApron, cell = extent / static_cast<float>(kN - 1);
    // The spacing at kTunedSide, worked out the same way, that the rain's numbers follow cell from: at
    // that side the two are the same and each number stands exactly as tuned.
    const float tunedCell = (WorldTerrain::kTunedSide + 2.f * WorldTerrain::kApron) / static_cast<float>(kN - 1);
    const bool ruins = spec.kind == WorldKind::Ruins;
    // Also says how far p lies from the nearest road's centre line within kReach (1e30 beyond), the
    // lowest and the highest the roads' banks let the ground there lie (-1e30 and 1e30 where none
    // reaches), and how far the kind's finish moved it (KindGround::finish(): a ruin mesa or gully).
    const auto groundAt = [&](Vec2 p, float& roadGapOut, float& bankLowOut, float& bankHighOut, float& shapedOut) {
        roadGapOut = 1e30f;
        bankLowOut = -1e30f;
        bankHighOut = 1e30f;
        shapedOut = 0.f;
        for (std::size_t i = 0; i < n; ++i) {
            const float dx = p.x - pads[i].centre.x, dy = p.y - pads[i].centre.y;
            if (dx * dx + dy * dy <= kR * kR) return pads[i].height;
        }
        const float sightCeiling = sight.ceiling(p), ceiling = std::min(climaxCeiling, sightCeiling);
        float h = underCeiling(ground.base(p), ceiling);
        float alpha = 0.f, sumW = 0.f, sumWH = 0.f, crest = 0.f, open = 1e30f;
        for (std::size_t i = 0; i < n; ++i) {
            const Pad& pad = pads[i];
            const float s = terrainDist(p, pad.centre) - kR;
            open = std::min(open, s - pad.blend);
            if (pad.blend <= 0.f || s >= pad.blend) continue;
            const float u = s / pad.blend, a = 1.f - smooth01(u), w = a / std::max(u * u, 1e-6f);
            alpha = std::max(alpha, a);
            sumW += w;
            sumWH += w * pad.height;
            if (pad.shape == PadShape::Bowl) crest += kCrest * bump((u - 0.5f) / 0.35f);
        }
        if (alpha > 0.f) h = lerpf(h, sumWH / sumW, alpha);
        // A bowl's crest stands a fifth of kCrest to all of it by noise, so that its rim reads broken
        // rather than turned, and rises only as far as the ceiling allows.
        if (crest > 0.f)
            crest *= 0.2f + 0.8f * smooth01(0.5f + 1.5f * fbm({p.x / 45.f + 3.7f, p.y / 45.f - 8.2f}, 2, 0.5f,
                                                              ground.seed ^ 0x6c8e9cf5u));
        h += std::min(crest, std::max(ceiling - h, 0.f));

        // The roads: the bounds their banks set, the strip under the nearest, and how far that is.
        // Ground under a road is lifted toward it, less with distance, so that a fill eases out into
        // the ground around it instead of standing as a causeway; ground over a road keeps its cut.
        float lo = -1e30f, hi = 1e30f, strip = 1e30f, stripHeight = 0.f, carve = 0.f, roadGap = 1e30f, lift = 0.f;
        for (std::size_t r = 0; r < roads; ++r) {
            const RoadSpan& sp = spans[r];
            if (p.x < sp.minX || p.x > sp.maxX || p.y < sp.minY || p.y > sp.maxY) continue;
            float d = 0.f;
            const float s = nearestOnSegment(p, sp.a, sp.b, d) * length[r];
            if (d >= kReach) continue;
            roadGap = std::min(roadGap, d);
            const float hr = roadProfile(length[r], sp.from, sp.to, s);
            lift = std::max(lift, (hr - h) * (1.f - smooth01((d - kLevel) / (kFillReach - kLevel))));
            const float bank = WorldTerrain::kBankSlope * std::max(d - kLevel, 0.f);
            lo = std::max(lo, hr - bank);
            hi = std::min(hi, hr + bank);
            if (d < WorldTerrain::kRoadHalfWidth && d < strip) {
                strip = d;
                stripHeight = hr;
                carve = WorldTerrain::kRoadCarve * smooth01(std::min(s - kR, length[r] - kR - s) / kCarveFade);
            }
        }
        h += lift;
        roadGapOut = roadGap;
        bankLowOut = lo;
        bankHighOut = hi;

        // What the kind makes of it. A ruin mesa keeps clear of the climax and the roads, and of
        // ground too near its ceiling.
        float clear = 1.f;
        if (ruins && alpha <= 0.f) {
            const float climaxGap = climax >= 0 ? terrainDist(p, pads[static_cast<std::size_t>(climax)].centre) : 1e30f;
            clear = std::min({smooth01((climaxGap - kMesaClimaxClear) / 60.f), smooth01((roadGap - kMesaRoadClear) / 20.f),
                              smooth01((ceiling - h - kMesaRoom) / 14.f)});
        }
        const float unshaped = h;
        h = ground.finish(p, h, alpha, std::max(open, 0.f), clear, ceiling);
        shapedOut = std::fabs(h - unshaped);

        // Nothing rises into the sightline's corridor; then the roads' banks, and the strip under them.
        h = std::min(h, sightCeiling);
        if (lo <= hi) h = std::clamp(h, lo, hi);
        else h = 0.5f * (lo + hi);
        if (strip < 1e30f) h = stripHeight - carve;
        return h;
    };

    // Pass 1: the ground as the pads, the roads and the kind make it, everywhere.
    const auto sampleAt = [&](std::size_t ix, std::size_t iy) {
        return Vec2{-0.5f * extent + static_cast<float>(ix) * cell, -0.5f * extent + static_cast<float>(iy) * cell};
    };
    // How freely rain falls on the ground at p, gap metres from the nearest road's centre line, 0 to
    // 1: not at all on a pad or within two samples of it, nor on a road's shoulders or within two
    // samples of them, fading in over each pad's blend and over kWearRoadRamp past the shoulders.
    const auto rainWeight = [&](Vec2 p, float gap) {
        float w = smooth01((gap - kLevel - 2.f * cell) / kWearRoadRamp);
        for (std::size_t i = 0; i < n; ++i)
            w = std::min(w, smooth01((terrainDist(p, pads[i].centre) - kR - 2.f * cell) / std::max(pads[i].blend, cell)));
        return w;
    };
    // How freely the rain may wear the ground at p, where it falls `rain` freely (rainWeight()): as
    // freely, fading out over kWearMargin past the square's edge, where the mountains and the abyss
    // (apron()) take the ground over.
    const auto wearWeight = [&](Vec2 p, float rain) {
        return std::min(1.f - smooth01((std::max(std::fabs(p.x), std::fabs(p.y)) - ground.half) / kWearMargin), rain);
    };
    out.heights_.assign(kN * kN, 0.f);
    out.cliffs_.assign(kN * kN, 0);
    // Each sample's rain (rainWeight()), for the wear and for the rain's way over the finished ground.
    // With the wear, each sample's weight (wearWeight()) and the highest the rain may raise it: under
    // the ceilings and the roads' banks as Pass 1 held it, or no higher than Pass 1 left it where
    // that is over them. Where two roads' banks cross, the rain leaves the ground alone.
    // And how freely the kind's finish lets it wear (`keep`): not where that raised or cut the ground
    // kWearKeep or more, so that the ruins keep their mesas and gullies.
    std::vector<float> roadGapAt(kN * kN), bankLowAt(kN * kN), rainAt(kN * kN), wearAt, wearTopAt, keep;
    if (options.erode) {
        wearAt.assign(kN * kN, 0.f);
        wearTopAt.assign(kN * kN, 0.f);
        keep.assign(kN * kN, 1.f);
    }
    for (std::size_t iy = 0; iy < kN; ++iy)
        for (std::size_t ix = 0; ix < kN; ++ix) {
            const std::size_t i = iy * kN + ix;
            const Vec2 p = sampleAt(ix, iy);
            float bankHigh = 1e30f, shaped = 0.f;
            const float h0 = groundAt(p, roadGapAt[i], bankLowAt[i], bankHigh, shaped);
            out.heights_[i] = h0;
            rainAt[i] = rainWeight(p, roadGapAt[i]);
            if (!options.erode) continue;
            keep[i] = 1.f - smooth01(shaped / kWearKeep);
            if (bankLowAt[i] > bankHigh) continue;
            wearAt[i] = wearWeight(p, rainAt[i]);
            wearTopAt[i] = std::max(std::min({climaxCeiling, sight.ceiling(p), bankHigh}), h0);
        }

    // On a field n samples a side, along one axis (x where `alongX`), each sample becomes the least
    // (`least`) or the mean of the 2 kWearSoftBlur + 1 round it, stopping at the field's edge.
    std::vector<float> eased;
    const auto ease = [&](std::vector<float>& field, std::size_t n, bool alongX, bool least) {
        const std::size_t along = alongX ? 1 : n, across = alongX ? n : 1;
        eased.resize(n * n);
        for (std::size_t a = 0; a < n; ++a)
            for (std::size_t b = 0; b < n; ++b) {
                float sum = 0.f, count = 0.f, low = 1.f;
                for (int k = -kWearSoftBlur; k <= kWearSoftBlur; ++k) {
                    const std::ptrdiff_t c = static_cast<std::ptrdiff_t>(b) + k;
                    if (c < 0 || c >= static_cast<std::ptrdiff_t>(n)) continue;
                    const float v = field[a * across + static_cast<std::size_t>(c) * along];
                    sum += v;
                    count += 1.f;
                    low = std::min(low, v);
                }
                eased[a * across + b * along] = least ? low : sum / count;
            }
        field.swap(eased);
    };
    // How freely a field of n x n heights `spacing` metres apart wears by its slope: fully up to
    // `from`, less beyond, not at all from `from` + `run`; that share eased over kWearSoftBlur
    // samples, twice over, so that no sample wears much more than the next.
    const auto softness = [&](const float* heights, std::size_t n, float spacing, float from, float run) {
        std::vector<float> share(n * n, 1.f);
        for (std::size_t iy = 1; iy + 1 < n; ++iy)
            for (std::size_t ix = 1; ix + 1 < n; ++ix) {
                const float* z = heights + iy * n + ix;
                const float slope = std::hypot(z[1] - z[-1], z[n] - z[-static_cast<std::ptrdiff_t>(n)]) / (2.f * spacing);
                share[iy * n + ix] = 1.f - smooth01((slope - from) / run);
            }
        for (int round = 0; round < 2; ++round) {
            ease(share, n, true, false);
            ease(share, n, false, false);
        }
        return share;
    };
    // The rain wears the ground inside the square and kWearMargin round it (wear()): steep ground less
    // (kWearSoft) as rock, and the kind's mesas and gullies not at all, nor the ground for
    // kWearSoftBlur samples round them, from where it eases in, so that the rain cuts no notches at
    // their rims. Then each worn sample is held within the roads' banks and under its top.
    if (options.erode) {
        const std::vector<float> soft = softness(out.heights_.data(), kN, cell, kWearSoft, kWearSoftRun);
        std::vector<float> spread = keep;
        ease(spread, kN, true, true);
        ease(spread, kN, false, true);
        ease(spread, kN, true, false);
        ease(spread, kN, false, false);
        for (std::size_t i = 0; i < kN * kN; ++i) wearAt[i] *= soft[i] * std::min(spread[i], keep[i]);
        const float from = (WorldTerrain::kApron - kWearMargin) / cell, to = static_cast<float>(kN - 1) - from;
        // As many droplets to each sample of that box as at kTunedSide.
        const float tunedFrom = (WorldTerrain::kApron - kWearMargin) / tunedCell;
        const float box = to - from, tunedBox = (static_cast<float>(kN - 1) - tunedFrom) - tunedFrom;
        const int drops = static_cast<int>(std::lround(static_cast<float>(kWearDrops) * ((box * box) / (tunedBox * tunedBox))));
        wear(out.heights_.data(), wearAt.data(), kN, drops, kWearBrush, from, to, detail::mixSeed(spec.seed ^ 0x2f6b3d1au));
        for (std::size_t i = 0; i < kN * kN; ++i)
            if (wearAt[i] > 0.f) out.heights_[i] = std::clamp(out.heights_[i], bankLowAt[i], wearTopAt[i]);
    }

    // The slope limit: off the sheer walls no two neighbouring samples climb more than kRockSlope, in
    // whole steps axisMost along an axis and diagMost along a diagonal. Steeper ground is cut back,
    // never raised, sweeping forward and back over the field until a sweep each way moves nothing,
    // so it stands as rock at that pitch rather than as a step. A sample in a sheer wall (a cliff)
    // neither moves nor holds its neighbours. It runs once here, so that the features are placed on
    // the ground as it will stand, and again once the walls are marked. Every height comes out on a
    // whole step. Says whether the ground settled within kSlopeSweeps.
    const std::int32_t axisMost = static_cast<std::int32_t>(std::floor(WorldTerrain::kRockSlope * cell / WorldTerrain::kHeightStep));
    const std::int32_t diagMost =
        static_cast<std::int32_t>(std::floor(WorldTerrain::kRockSlope * cell * 1.41421356f / WorldTerrain::kHeightStep));
    std::vector<std::int32_t> units(kN * kN);
    const auto limitSlopes = [&]() {
        for (std::size_t i = 0; i < kN * kN; ++i)
            units[i] = static_cast<std::int32_t>(std::lround(out.heights_[i] / WorldTerrain::kHeightStep));
        // Lowers sample i to `most` over sample j, unless either is a cliff; says whether it moved.
        const auto hold = [&](std::size_t i, std::size_t j, std::int32_t most) {
            if (out.cliffs_[i] || out.cliffs_[j] || units[i] <= units[j] + most) return false;
            units[i] = units[j] + most;
            return true;
        };
        bool settled = false;
        for (int sweep = 0; sweep < kSlopeSweeps && !settled; ++sweep) {
            bool moved = false;
            for (std::size_t iy = 0; iy < kN; ++iy)
                for (std::size_t ix = 0; ix < kN; ++ix) {
                    const std::size_t i = iy * kN + ix;
                    if (ix > 0 && hold(i, i - 1, axisMost)) moved = true;
                    if (iy == 0) continue;
                    if (hold(i, i - kN, axisMost)) moved = true;
                    if (ix > 0 && hold(i, i - kN - 1, diagMost)) moved = true;
                    if (ix + 1 < kN && hold(i, i - kN + 1, diagMost)) moved = true;
                }
            for (std::size_t iy = kN; iy-- > 0;)
                for (std::size_t ix = kN; ix-- > 0;) {
                    const std::size_t i = iy * kN + ix;
                    if (ix + 1 < kN && hold(i, i + 1, axisMost)) moved = true;
                    if (iy + 1 == kN) continue;
                    if (hold(i, i + kN, axisMost)) moved = true;
                    if (ix + 1 < kN && hold(i, i + kN + 1, diagMost)) moved = true;
                    if (ix > 0 && hold(i, i + kN - 1, diagMost)) moved = true;
                }
            settled = !moved;
        }
        for (std::size_t i = 0; i < kN * kN; ++i) out.heights_[i] = static_cast<float>(units[i]) * WorldTerrain::kHeightStep;
        return settled;
    };
    if (!limitSlopes()) return fail("the ground's slopes don't settle");

    // The town's sea, past the edge whose band of ground inside lies lowest on average: kSeaUnder
    // under that band's lowest ground at least, and a metre under the lowest pad, on a whole metre.
    out.featureCount_ = 0;
    if (spec.kind == WorldKind::Town) {
        std::array<double, 4> sum{};
        std::array<std::size_t, 4> count{};
        std::array<float, 4> low{1e30f, 1e30f, 1e30f, 1e30f};
        for (std::size_t iy = 0; iy < kN; ++iy)
            for (std::size_t ix = 0; ix < kN; ++ix) {
                const Vec2 p = sampleAt(ix, iy);
                if (std::fabs(p.x) > ground.half || std::fabs(p.y) > ground.half) continue;
                for (int s = 0; s < 4; ++s) {
                    if (ground.past(p, s) < -kCoastBand) continue;
                    const float g = out.heights_[iy * kN + ix];
                    sum[s] += g;
                    ++count[s];
                    low[s] = std::min(low[s], g);
                }
            }
        int coast = 0;
        for (int s = 1; s < 4; ++s)
            if (sum[s] / static_cast<double>(count[s]) < sum[coast] / static_cast<double>(count[coast])) coast = s;
        float lowestPad = 1e30f;
        for (std::size_t i = 0; i < n; ++i) lowestPad = std::min(lowestPad, pads[i].height);
        ground.coast = coast;
        ground.sea = std::floor(std::min(low[coast] - kSeaUnder, lowestPad - 1.f));
        TerrainFeature& sea = out.features_[out.featureCount_++];
        sea = TerrainFeature{};
        sea.kind = TerrainFeature::Kind::Sea;
        sea.blocked = true;
        sea.side = static_cast<std::uint8_t>(coast);
        sea.level = ground.sea;
        sea.centre = {kCompass[2 * coast].x * ground.half, kCompass[2 * coast].y * ground.half};
    }

    // Where the other features may go: kFeatureClear from the roads' edges and the pads' blends,
    // kWallClear inside the walls, and out of the sightline's corridor. Each feature placed closes the
    // ground round it to the next. A summed-area table over the closed samples tests a rectangle at
    // once.
    const float half = ground.half;
    std::vector<std::uint8_t> closed(kN * kN, 0);
    std::vector<std::uint32_t> sat((kN + 1) * (kN + 1), 0);
    // A summed-area table over a mask of the samples.
    const auto tabulate = [](const std::vector<std::uint8_t>& mask, std::vector<std::uint32_t>& table) {
        for (std::size_t iy = 0; iy < kN; ++iy) {
            std::uint32_t row = 0;
            for (std::size_t ix = 0; ix < kN; ++ix) {
                row += mask[iy * kN + ix];
                table[(iy + 1) * (kN + 1) + ix + 1] = table[iy * (kN + 1) + ix + 1] + row;
            }
        }
    };
    // How many of the samples x0..x1 by y0..y1 are set in a table's mask.
    const auto countIn = [](const std::vector<std::uint32_t>& table, std::size_t x0, std::size_t y0, std::size_t x1,
                            std::size_t y1) {
        const std::size_t w = kN + 1;
        return table[(y1 + 1) * w + x1 + 1] - table[y0 * w + x1 + 1] - table[(y1 + 1) * w + x0] + table[y0 * w + x0];
    };
    // A height as pass 2 will store it. Inside the square the towers have no apron to add.
    const auto stored = [&](std::size_t ix, std::size_t iy) {
        return std::round(out.heights_[iy * kN + ix] / WorldTerrain::kHeightStep) * WorldTerrain::kHeightStep;
    };
    // The samples strictly between lo and hi along an axis, and `ring` more either way, within the field.
    const auto span = [&](float lo, float hi, float ring, std::size_t& i0, std::size_t& i1) {
        const float last = static_cast<float>(kN - 1);
        i0 = static_cast<std::size_t>(std::clamp(std::floor((lo + 0.5f * extent) / cell) + 1.f - ring, 0.f, last));
        i1 = static_cast<std::size_t>(std::clamp(std::ceil((hi + 0.5f * extent) / cell) - 1.f + ring, 0.f, last));
    };
    for (std::size_t iy = 0; iy < kN; ++iy)
        for (std::size_t ix = 0; ix < kN; ++ix) {
            const std::size_t i = iy * kN + ix;
            const Vec2 p = sampleAt(ix, iy);
            bool shut = std::fabs(p.x) > half - kWallClear || std::fabs(p.y) > half - kWallClear ||
                        roadGapAt[i] < kLevel + kFeatureClear;
            for (std::size_t k = 0; k < n && !shut; ++k) shut = terrainDist(p, pads[k].centre) - kR < pads[k].blend + kFeatureClear;
            if (!shut && sight.on) {
                float d = 0.f;
                nearestOnSegment(p, sight.from, sight.to, d);
                shut = d < kCorridorHalf;
            }
            closed[i] = shut;
        }
    tabulate(closed, sat);
    // Whether a rectangle's samples, and one more all round, are all open.
    const auto clearRect = [&](Vec2 c, Vec2 halves) {
        std::size_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
        span(c.x - halves.x, c.x + halves.x, 1.f, x0, x1);
        span(c.y - halves.y, c.y + halves.y, 1.f, y0, y1);
        return countIn(sat, x0, y0, x1, y1) == 0;
    };
    // Closes the samples a feature covers once grown by `by` metres.
    const auto closeAround = [&](const TerrainFeature& f, float by) {
        Vec2 reach = f.half;
        if (f.kind == TerrainFeature::Kind::Lake || f.kind == TerrainFeature::Kind::Pit) {
            const float most = *std::max_element(f.ray.begin(), f.ray.end());
            reach = {most, most};
        }
        std::size_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
        span(f.centre.x - reach.x - by, f.centre.x + reach.x + by, 1.f, x0, x1);
        span(f.centre.y - reach.y - by, f.centre.y + reach.y + by, 1.f, y0, y1);
        for (std::size_t iy = y0; iy <= y1; ++iy)
            for (std::size_t ix = x0; ix <= x1; ++ix)
                if (f.covers(sampleAt(ix, iy), -by)) closed[iy * kN + ix] = 1;
        tabulate(closed, sat);
    };
    // How far p lies from the nearest road's edge, over all of them.
    const auto roadEdgeGap = [&](Vec2 p) {
        float gap = 1e30f;
        for (std::size_t r = 0; r < roads; ++r) {
            float d = 0.f;
            nearestOnSegment(p, spans[r].a, spans[r].b, d);
            gap = std::min(gap, d - kLevel);
        }
        return gap;
    };
    // A trench or a pit only cuts ground a player could walk round: the ground under it and kCutBand
    // round it is all walkable, so a walk that crossed it goes round it instead and no ground is cut
    // off. The samples at a corner of a cell too steep to walk on, as the ground will be stored, and a
    // table over them. It is built once, before the cuts: each cut lies kTrenchApart or kPitApart from
    // the walls of the ones before, well past the band it tests.
    std::vector<std::uint8_t> steep;
    std::vector<std::uint32_t> steepSat;
    if (towers || ruins) {
        steep.assign(kN * kN, 0);
        steepSat.assign((kN + 1) * (kN + 1), 0);
        for (std::size_t iy = 0; iy + 1 < kN; ++iy)
            for (std::size_t ix = 0; ix + 1 < kN; ++ix) {
                const float h00 = stored(ix, iy), h10 = stored(ix + 1, iy), h01 = stored(ix, iy + 1), h11 = stored(ix + 1, iy + 1);
                const float g1 = std::hypot(h10 - h00, h11 - h10) / cell, g2 = std::hypot(h11 - h01, h01 - h00) / cell;
                if (std::max(g1, g2) < kWalkSlope) continue;
                steep[iy * kN + ix] = steep[iy * kN + ix + 1] = steep[(iy + 1) * kN + ix] = steep[(iy + 1) * kN + ix + 1] = 1;
            }
        tabulate(steep, steepSat);
    }
    // Whether the cells over a rectangle grown by kCutBand, and one more all round, are all walkable.
    const auto walkableRect = [&](Vec2 c, Vec2 halves) {
        std::size_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
        span(c.x - halves.x - kCutBand, c.x + halves.x + kCutBand, 1.f, x0, x1);
        span(c.y - halves.y - kCutBand, c.y + halves.y + kCutBand, 1.f, y0, y1);
        return countIn(steepSat, x0, y0, x1, y1) == 0;
    };

    // Among the towers, up to kMaxTrenches trenches kTrenchWidth wide and kTrenchMin to kTrenchMax
    // long, along x or y, kTrenchApart or more from the one before. A trench's floor lies kTrenchDepth,
    // or kTrenchDeeper or twice that more, under the lowest ground round it, and its walls climb from
    // the floor's edge at kTrenchWall until they meet the ground, standing out past the edge by up to
    // twice kTrenchWobble and wavering along it. Each goes where the ground round it rises and falls by
    // kTrenchRelief at most, so that its walls stand about as tall all round: the longest run on the
    // kFeatureGrid whose walls' furthest reach is open and walkable, and of those the one whose middle
    // lies nearest a road. Players are kept out from a sample outside its edge.
    if (towers) {
        const int steps = static_cast<int>(std::floor((half - kWallClear) / kFeatureGrid));
        const int longest = static_cast<int>(std::floor((kTrenchMax - kTrenchMin) / kFeatureGrid));
        std::vector<float> lineLow(kN), lineHigh(kN); // the ground across a window, at each sample along it
        for (int t = 0; t < kMaxTrenches && out.featureCount_ < WorldTerrain::kMaxFeatures; ++t) {
            const std::uint32_t roll = detail::mixSeed(ground.seed ^ (0x9e3779b9u * static_cast<std::uint32_t>(t + 1)));
            const float depth = kTrenchDepth + kTrenchDeeper * static_cast<float>(roll % 3u);
            // How far past the floor's edge the walls may reach (the floor rounds down by up to a step),
            // and the window round the floor whose ground sets the floor: a sample past the ring round that.
            const float foot = 2.f * kTrenchWobble + (depth + kTrenchRelief + WorldTerrain::kHeightStep) / kTrenchWall + cell;
            const float margin = foot + 2.f * cell;
            float bestRun = 0.f, bestGap = 1e30f;
            Vec2 bestCentre{}, bestHalves{};
            for (int axis = 0; axis < 2; ++axis) {
                const auto halves = [axis](float run) { // the floor's
                    return axis == 0 ? Vec2{0.5f * run, 0.5f * kTrenchWidth} : Vec2{0.5f * kTrenchWidth, 0.5f * run};
                };
                for (int ga = -steps; ga <= steps; ++ga) {
                    const float across = static_cast<float>(ga) * kFeatureGrid;
                    std::size_t a0 = 0, a1 = 0;
                    span(across - 0.5f * kTrenchWidth - margin, across + 0.5f * kTrenchWidth + margin, 1.f, a0, a1);
                    for (std::size_t s = 0; s < kN; ++s) {
                        float lo = 1e30f, hi = -1e30f;
                        for (std::size_t a = a0; a <= a1; ++a) {
                            const float h = out.heights_[axis == 0 ? a * kN + s : s * kN + a];
                            lo = std::min(lo, h);
                            hi = std::max(hi, h);
                        }
                        lineLow[s] = lo;
                        lineHigh[s] = hi;
                    }
                    for (int gb = -steps; gb <= steps; ++gb) {
                        const float along = static_cast<float>(gb) * kFeatureGrid;
                        const Vec2 c = axis == 0 ? Vec2{along, across} : Vec2{across, along};
                        // Clear and walkable out to the walls' furthest reach, kTrenchApart from the trenches cut.
                        const auto open = [&](float run) {
                            const Vec2 hv = halves(run), reach{hv.x + foot, hv.y + foot};
                            if (!clearRect(c, reach) || !walkableRect(c, reach)) return false;
                            for (std::size_t f = 0; f < out.featureCount_; ++f) {
                                const TerrainFeature& cut = out.features_[f];
                                if (cut.kind != TerrainFeature::Kind::Trench) continue;
                                const float apartX = std::max(std::fabs(c.x - cut.centre.x) - reach.x - cut.half.x, 0.f);
                                const float apartY = std::max(std::fabs(c.y - cut.centre.y) - reach.y - cut.half.y, 0.f);
                                if (std::hypot(apartX, apartY) < kTrenchApart) return false;
                            }
                            return true;
                        };
                        if (!open(kTrenchMin)) continue;
                        int fits = 0, cap = longest; // open rectangles nest, so the longest is a bisection away
                        while (fits < cap) {
                            const int mid = (fits + cap + 1) / 2;
                            if (open(kTrenchMin + static_cast<float>(mid) * kFeatureGrid)) fits = mid;
                            else cap = mid - 1;
                        }
                        // Of those, the longest whose window's ground rises and falls by kTrenchRelief at
                        // most. The windows nest too, so each takes in only the samples along it the last
                        // one didn't.
                        float lo = 1e30f, hi = -1e30f;
                        std::size_t s0 = 0, s1 = 0;
                        const auto take = [&](std::size_t first, std::size_t last) {
                            for (std::size_t s = first; s <= last; ++s) {
                                lo = std::min(lo, lineLow[s]);
                                hi = std::max(hi, lineHigh[s]);
                            }
                        };
                        int runs = -1;
                        for (int m = 0; m <= fits; ++m) {
                            const float tried = kTrenchMin + static_cast<float>(m) * kFeatureGrid;
                            std::size_t i0 = 0, i1 = 0;
                            span(along - 0.5f * tried - margin, along + 0.5f * tried + margin, 1.f, i0, i1);
                            if (m == 0) {
                                take(i0, i1);
                                s0 = i0;
                                s1 = i1;
                            } else {
                                if (i0 < s0) take(i0, s0 - 1);
                                if (i1 > s1) take(s1 + 1, i1);
                                s0 = std::min(s0, i0);
                                s1 = std::max(s1, i1);
                            }
                            if (hi - lo > kTrenchRelief) break;
                            runs = m;
                        }
                        if (runs < 0) continue;
                        const float run = kTrenchMin + static_cast<float>(runs) * kFeatureGrid;
                        if (run < bestRun) continue;
                        const float gap = roadEdgeGap(c);
                        if (run > bestRun || gap < bestGap) {
                            bestRun = run;
                            bestGap = gap;
                            bestCentre = c;
                            bestHalves = halves(run);
                        }
                    }
                }
            }
            if (bestRun <= 0.f) break;
            TerrainFeature& trench = out.features_[out.featureCount_++];
            trench = TerrainFeature{};
            trench.kind = TerrainFeature::Kind::Trench;
            trench.blocked = true;
            trench.rim = -cell;
            trench.centre = bestCentre;
            const Vec2 hv = bestHalves;
            std::size_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
            span(bestCentre.x - hv.x - margin, bestCentre.x + hv.x + margin, 1.f, x0, x1);
            span(bestCentre.y - hv.y - margin, bestCentre.y + hv.y + margin, 1.f, y0, y1);
            float low = 1e30f, high = -1e30f;
            for (std::size_t iy = y0; iy <= y1; ++iy)
                for (std::size_t ix = x0; ix <= x1; ++ix) {
                    low = std::min(low, out.heights_[iy * kN + ix]);
                    high = std::max(high, out.heights_[iy * kN + ix]);
                }
            trench.level = std::floor((low - depth) / WorldTerrain::kHeightStep) * WorldTerrain::kHeightStep;
            trench.wall = 2.f * kTrenchWobble + (high - trench.level) / kTrenchWall + cell;
            trench.half = {hv.x + trench.wall, hv.y + trench.wall};
            // The floor's edge stands out past hv by two waves running round it, 40 m and 26 m long at most
            // (a whole number of each to the way round), and the walls climb from it by their distance
            // out, so their corners round off and every side wanders, the ends too. Along a side they
            // lean by 0.38 at most, and round a corner not at all, so they stay under kRockSlope.
            const float perimeter = 4.f * (hv.x + hv.y);
            const float slowWaves = std::max(std::floor(perimeter / 40.f), 1.f);
            const float quickWaves = std::max(std::floor(perimeter / 26.f), 1.f);
            const std::uint32_t phases = detail::mixSeed(roll ^ 0x27d4eb2fu);
            const float slow = 6.28318531f * static_cast<float>(phases & 0xffffu) / 65536.f;
            const float quick = 6.28318531f * static_cast<float>(phases >> 16) / 65536.f;
            const auto stand = [&](float s) {
                return kTrenchWobble * (1.f + 0.6f * std::sin(6.28318531f * slowWaves * s / perimeter + slow) +
                                        0.4f * std::sin(6.28318531f * quickWaves * s / perimeter + quick));
            };
            // How far round the floor's edge q, a point on it, lies from its corner at (+x, -y), toward +y.
            const auto onEdge = [&](float qx, float qy) {
                if (qx >= hv.x) return qy + hv.y;
                if (qy >= hv.y) return 2.f * hv.y + hv.x - qx;
                if (qx <= -hv.x) return 3.f * hv.y + 2.f * hv.x - qy;
                return 4.f * hv.y + 3.f * hv.x + qx;
            };
            for (std::size_t iy = y0; iy <= y1; ++iy)
                for (std::size_t ix = x0; ix <= x1; ++ix) {
                    const Vec2 p = sampleAt(ix, iy);
                    const float u = p.x - bestCentre.x, v = p.y - bestCentre.y;
                    const float off = std::hypot(std::max(std::fabs(u) - hv.x, 0.f), std::max(std::fabs(v) - hv.y, 0.f));
                    const float climb =
                        off > 0.f ? off - stand(onEdge(std::clamp(u, -hv.x, hv.x), std::clamp(v, -hv.y, hv.y))) : 0.f;
                    float& h = out.heights_[iy * kN + ix];
                    h = std::min(h, trench.level + kTrenchWall * std::max(climb, 0.f));
                }
            closeAround(trench, kFeatureClear);
        }
    }

    // Among the towers, up to kMaxPools reflecting pools, kPoolApart or more from each other: each the
    // largest of kPoolSizes, along x or y on the kFeatureGrid, where the ground over the pool, a
    // platform kPoolMargin round it and a bank kPoolBank round that rises and falls by kPoolRelief at
    // most and is all open, and of those the one whose middle lies nearest a road. The platform is cut
    // level at the lowest ground there and banks up at kPoolBankSlope to meet the ground. The pool's bed
    // lies kPoolBed under the platform from a sample in from its edge, so the ground at the edge stays
    // on the platform and slopes gently in, and its water lies kPoolSurface under the platform.
    if (towers) {
        constexpr std::size_t kWays = 2 * (sizeof(kPoolSizes) / sizeof(kPoolSizes[0])); // each size along x, then along y
        const float grow = kPoolMargin + kPoolBank;
        const int steps = static_cast<int>(std::floor((half - kWallClear) / kFeatureGrid));
        const std::size_t spots = static_cast<std::size_t>(2 * steps + 1); // along each axis
        const auto spotAt = [steps](std::size_t k) { return static_cast<float>(static_cast<int>(k) - steps) * kFeatureGrid; };
        const auto halvesOf = [](std::size_t way) {
            const Vec2 dims = kPoolSizes[way / 2];
            return way % 2 == 0 ? Vec2{0.5f * dims.x, 0.5f * dims.y} : Vec2{0.5f * dims.y, 0.5f * dims.x};
        };
        // The lowest and highest ground over each spot's window (the pool, its platform and bank, and a
        // sample more all round), each way: along the rows first, then down the columns. A pool cuts
        // only its window, which is open, and closes kPoolApart round it, so no later window takes in
        // ground a pool cut and these hold for every pool.
        std::vector<float> lowest(kWays * spots * spots), highest(kWays * spots * spots);
        std::vector<float> rowLow(kN * spots), rowHigh(kN * spots);
        std::vector<std::size_t> firstX(spots), lastX(spots), firstY(spots), lastY(spots);
        for (std::size_t way = 0; way < kWays; ++way) {
            const Vec2 hv = halvesOf(way);
            for (std::size_t k = 0; k < spots; ++k) {
                span(spotAt(k) - hv.x - grow, spotAt(k) + hv.x + grow, 1.f, firstX[k], lastX[k]);
                span(spotAt(k) - hv.y - grow, spotAt(k) + hv.y + grow, 1.f, firstY[k], lastY[k]);
            }
            for (std::size_t iy = firstY.front(); iy <= lastY.back(); ++iy)
                for (std::size_t k = 0; k < spots; ++k) {
                    float lo = 1e30f, hi = -1e30f;
                    for (std::size_t ix = firstX[k]; ix <= lastX[k]; ++ix) {
                        lo = std::min(lo, out.heights_[iy * kN + ix]);
                        hi = std::max(hi, out.heights_[iy * kN + ix]);
                    }
                    rowLow[iy * spots + k] = lo;
                    rowHigh[iy * spots + k] = hi;
                }
            for (std::size_t ky = 0; ky < spots; ++ky)
                for (std::size_t kx = 0; kx < spots; ++kx) {
                    float lo = 1e30f, hi = -1e30f;
                    for (std::size_t iy = firstY[ky]; iy <= lastY[ky]; ++iy) {
                        lo = std::min(lo, rowLow[iy * spots + kx]);
                        hi = std::max(hi, rowHigh[iy * spots + kx]);
                    }
                    lowest[(way * spots + ky) * spots + kx] = lo;
                    highest[(way * spots + ky) * spots + kx] = hi;
                }
        }
        for (int t = 0; t < kMaxPools && out.featureCount_ < WorldTerrain::kMaxFeatures; ++t) {
            bool placed = false;
            for (std::size_t size = 0; size < kWays / 2 && !placed; ++size) {
                bool found = false;
                float bestGap = 1e30f, bestLevel = 0.f;
                Vec2 bestCentre{}, bestHalves{};
                for (std::size_t ky = 0; ky < spots; ++ky)
                    for (std::size_t kx = 0; kx < spots; ++kx) {
                        const Vec2 c{spotAt(kx), spotAt(ky)};
                        for (std::size_t way = 2 * size; way < 2 * size + 2; ++way) {
                            const std::size_t k = (way * spots + ky) * spots + kx;
                            const float level = std::floor(lowest[k] / WorldTerrain::kHeightStep) * WorldTerrain::kHeightStep;
                            const Vec2 hv = halvesOf(way);
                            if (highest[k] - level > kPoolRelief || !clearRect(c, {hv.x + grow, hv.y + grow})) continue;
                            const float gap = roadEdgeGap(c);
                            if (!found || gap < bestGap) {
                                found = true;
                                bestGap = gap;
                                bestLevel = level;
                                bestCentre = c;
                                bestHalves = hv;
                            }
                        }
                    }
                if (!found) continue;
                TerrainFeature& pool = out.features_[out.featureCount_++];
                pool = TerrainFeature{};
                pool.kind = TerrainFeature::Kind::Pool;
                pool.centre = bestCentre;
                pool.half = bestHalves;
                pool.level = bestLevel - kPoolSurface;
                std::size_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
                span(bestCentre.x - bestHalves.x - grow, bestCentre.x + bestHalves.x + grow, 1.f, x0, x1);
                span(bestCentre.y - bestHalves.y - grow, bestCentre.y + bestHalves.y + grow, 1.f, y0, y1);
                for (std::size_t iy = y0; iy <= y1; ++iy)
                    for (std::size_t ix = x0; ix <= x1; ++ix) {
                        const Vec2 p = sampleAt(ix, iy);
                        float& h = out.heights_[iy * kN + ix];
                        if (pool.covers(p, cell)) {
                            h = bestLevel - kPoolBed;
                            continue;
                        }
                        const float ox = std::max(std::fabs(p.x - bestCentre.x) - bestHalves.x, 0.f);
                        const float oy = std::max(std::fabs(p.y - bestCentre.y) - bestHalves.y, 0.f);
                        h = std::min(h, bestLevel + kPoolBankSlope * std::max(std::hypot(ox, oy) - kPoolMargin, 0.f));
                    }
                closeAround(pool, kPoolApart);
                placed = true;
            }
            if (!placed) break;
        }
    }

    // In the ruins, up to kMaxPits pits, kPitApart or more from each other: each a ragged star about
    // kPitRadius across its rays, on the kFeatureGrid where the square round it is open and walkable,
    // and of those the spot nearest a road. Its floor lies about kPitFloor, far under the plain. From
    // its edge a funnel falls toward its middle at kTrenchWall from the highest ground round it, so it
    // meets the ground wherever that lies, until kPitFunnel in from the edge, give or take kPitJitter,
    // the shaft drops sheer to the floor. Players are kept out from a sample outside its edge.
    if (ruins) {
        const int steps = static_cast<int>(std::floor((half - kWallClear) / kFeatureGrid));
        for (int t = 0; t < kMaxPits && out.featureCount_ < WorldTerrain::kMaxFeatures; ++t) {
            const std::uint32_t seed = detail::mixSeed(ground.seed ^ (0x85ebca6bu * static_cast<std::uint32_t>(t + 1)));
            TerrainFeature pit{};
            pit.kind = TerrainFeature::Kind::Pit;
            pit.blocked = true;
            pit.rim = -cell;
            pit.wall = kPitFunnel + kPitJitter;
            for (std::size_t k = 0; k < TerrainFeature::kRays; ++k) {
                const float a = 6.28318531f * static_cast<float>(k) / static_cast<float>(TerrainFeature::kRays);
                const float wobble = gradientNoise(kPitWobble * std::cos(a) + 17.3f, kPitWobble * std::sin(a) - 9.9f, seed);
                pit.ray[k] = kPitRadius * (1.f + kPitRagged * std::clamp(wobble, -1.f, 1.f));
            }
            const float reach = *std::max_element(pit.ray.begin(), pit.ray.end());
            bool found = false;
            float bestGap = 1e30f;
            for (int gy = -steps; gy <= steps; ++gy)
                for (int gx = -steps; gx <= steps; ++gx) {
                    const Vec2 c{static_cast<float>(gx) * kFeatureGrid, static_cast<float>(gy) * kFeatureGrid};
                    if (!clearRect(c, {reach, reach}) || !walkableRect(c, {reach, reach})) continue;
                    const float gap = roadEdgeGap(c);
                    if (!found || gap < bestGap) {
                        found = true;
                        bestGap = gap;
                        pit.centre = c;
                    }
                }
            if (!found) break;
            const float roll = static_cast<float>(detail::mixSeed(seed ^ 0xc2b2ae35u) >> 8) * (1.f / 16777216.f);
            pit.level = std::round(kPitFloor + kPitRoll * (2.f * roll - 1.f));
            std::size_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
            span(pit.centre.x - reach, pit.centre.x + reach, 1.f, x0, x1);
            span(pit.centre.y - reach, pit.centre.y + reach, 1.f, y0, y1);
            float brim = -1e30f; // the highest ground over it and a sample past its edge
            for (std::size_t iy = y0; iy <= y1; ++iy)
                for (std::size_t ix = x0; ix <= x1; ++ix)
                    if (pit.covers(sampleAt(ix, iy), -cell)) brim = std::max(brim, out.heights_[iy * kN + ix]);
            for (std::size_t iy = y0; iy <= y1; ++iy)
                for (std::size_t ix = x0; ix <= x1; ++ix) {
                    const Vec2 p = sampleAt(ix, iy);
                    if (!pit.covers(p)) continue;
                    const float dx = p.x - pit.centre.x, dy = p.y - pit.centre.y;
                    const float angle = std::atan2(dy, dx), edge = pit.radius(angle), r = std::hypot(dx, dy);
                    const float jitter = kPitJitter * std::clamp(gradientNoise(kPitJitterWobble * std::cos(angle) + 41.7f,
                                                                               kPitJitterWobble * std::sin(angle) - 23.1f,
                                                                               seed ^ 0x27d4eb2fu),
                                                                 -1.f, 1.f);
                    // In as far as covers(p, pit.wall) at least, whatever the rounding.
                    float& h = out.heights_[iy * kN + ix];
                    h = r < edge - (kPitFunnel - jitter) ? pit.level : std::min(h, brim - kTrenchWall * (edge - r));
                }
            out.features_[out.featureCount_++] = pit;
            closeAround(pit, kPitApart);
        }
    }

    // Pass 2: past the edge the mountains, the shore or the abyss; and every height on a whole step.
    for (std::size_t iy = 0; iy < kN; ++iy)
        for (std::size_t ix = 0; ix < kN; ++ix) {
            float& h = out.heights_[iy * kN + ix];
            h = std::round((h + ground.apron(sampleAt(ix, iy), h)) / WorldTerrain::kHeightStep) * WorldTerrain::kHeightStep;
            if (!(std::fabs(h) <= WorldTerrain::kMaxAbsHeight)) return fail("the ground leaves the height range");
        }

    // Then the rain wears the mountains (TerrainOptions::erodeMountains), over the whole field but
    // not inside the square, which it leaves as it stands: its wear fades in over kWearMargin past the
    // edge as the open ground's fades out, and out again over kRangeRimRun before the field's outer
    // kRangeRim, where the mountains stay as raised, the horizon. Nor past the town's coast edge, its
    // wear fading out over kWearMargin before it, so that the quay and the sea's floor keep their
    // depths, nor in the ruins, which have no mountains. It wears twice. First on a field
    // kRangeCoarse times coarser, its heights shrunk as much, so that it wears as the full field would
    // were the mountains that much smaller every way: kRangeDrops droplets, each wearing kRangeRain as
    // hard as one on the full field, cut gullies down the slopes and lay fans at their feet over the
    // broken band of rock. The difference, grown back, reaches the full field between the coarse
    // samples, and where the fans lie kRangeBury deep they bury the finer relief. Then on the full
    // field, so that rills run down the gullies' sides. Each time ground steeper than kRangeSoft wears
    // less and from kRangeSoft + kRangeSoftRun not at all, as bare rock. The slope limit below rounds
    // each worn height to a whole step, and where the rim meets the worn ground it may trim the rim's
    // first samples by a few centimetres.
    if (options.erodeMountains && !ruins) {
        const auto rangeWeight = [&](Vec2 p) {
            const float beyond = std::max(std::fabs(p.x), std::fabs(p.y)) - ground.half;
            const float shore = ground.coast >= 0 ? smooth01(-ground.past(p, ground.coast) / kWearMargin) : 1.f;
            return shore * smooth01(beyond / kWearMargin) *
                   smooth01((WorldTerrain::kApron - kRangeRim - beyond) / kRangeRimRun);
        };
        constexpr std::size_t kNc = (kN - 1) / kRangeCoarse + 1;
        static_assert((kN - 1) % kRangeCoarse == 0, "the coarse field's samples lie on the full field's");
        constexpr std::ptrdiff_t kBox = static_cast<std::ptrdiff_t>(kRangeCoarse / 2);
        std::vector<float> coarse(kNc * kNc);
        for (std::size_t jy = 0; jy < kNc; ++jy)
            for (std::size_t jx = 0; jx < kNc; ++jx) {
                float sum = 0.f, count = 0.f;
                for (std::ptrdiff_t dy = -kBox; dy <= kBox; ++dy)
                    for (std::ptrdiff_t dx = -kBox; dx <= kBox; ++dx) {
                        const std::ptrdiff_t ix = static_cast<std::ptrdiff_t>(jx * kRangeCoarse) + dx;
                        const std::ptrdiff_t iy = static_cast<std::ptrdiff_t>(jy * kRangeCoarse) + dy;
                        if (ix < 0 || iy < 0 || ix >= static_cast<std::ptrdiff_t>(kN) || iy >= static_cast<std::ptrdiff_t>(kN)) continue;
                        sum += out.heights_[static_cast<std::size_t>(iy) * kN + static_cast<std::size_t>(ix)];
                        count += 1.f;
                    }
                coarse[jy * kNc + jx] = sum / count / static_cast<float>(kRangeCoarse);
            }
        const std::vector<float> before = coarse;
        std::vector<float> coarseAt = softness(coarse.data(), kNc, cell, kRangeSoft, kRangeSoftRun);
        for (std::size_t jy = 0; jy < kNc; ++jy)
            for (std::size_t jx = 0; jx < kNc; ++jx)
                coarseAt[jy * kNc + jx] *= kRangeRain * rangeWeight(sampleAt(jx * kRangeCoarse, jy * kRangeCoarse));
        wear(coarse.data(), coarseAt.data(), kNc, kRangeDrops, kRangeBrush, static_cast<float>(kRangeBrush),
             static_cast<float>(kNc - 2 - kRangeBrush), detail::mixSeed(spec.seed ^ 0x6a09e667u));
        for (std::size_t iy = 0; iy < kN; ++iy)
            for (std::size_t ix = 0; ix < kN; ++ix) {
                const float w = rangeWeight(sampleAt(ix, iy));
                if (!(w > 0.f)) continue;
                const std::size_t jx = std::min(ix / kRangeCoarse, kNc - 2), jy = std::min(iy / kRangeCoarse, kNc - 2);
                const float u = static_cast<float>(ix) / static_cast<float>(kRangeCoarse) - static_cast<float>(jx);
                const float v = static_cast<float>(iy) / static_cast<float>(kRangeCoarse) - static_cast<float>(jy);
                const std::size_t j = jy * kNc + jx;
                const auto between = [&](const std::vector<float>& f) {
                    return lerpf(lerpf(f[j], f[j + 1], u), lerpf(f[j + kNc], f[j + kNc + 1], u), v);
                };
                const float scale = static_cast<float>(kRangeCoarse);
                const float worn = between(coarse) * scale, d = worn - between(before) * scale;
                float h = out.heights_[iy * kN + ix] + d;
                if (d > 0.f) h = lerpf(h, worn, smooth01(d / kRangeBury));
                out.heights_[iy * kN + ix] = lerpf(out.heights_[iy * kN + ix], h, w);
            }
        std::vector<float> rangeAt = softness(out.heights_.data(), kN, cell, kRangeSoft, kRangeSoftRun);
        for (std::size_t iy = 0; iy < kN; ++iy)
            for (std::size_t ix = 0; ix < kN; ++ix) rangeAt[iy * kN + ix] *= rangeWeight(sampleAt(ix, iy));
        wear(out.heights_.data(), rangeAt.data(), kN, kWearDrops, kWearBrush, static_cast<float>(kWearBrush),
             static_cast<float>(kN - 2 - kWearBrush), detail::mixSeed(spec.seed ^ 0xbb67ae85u));
        for (const float level : out.heights_)
            if (!(std::fabs(level) <= WorldTerrain::kMaxAbsHeight)) return fail("the ground leaves the height range");
    }

    // The sheer walls: the town's quay, and in the ruins the fall into the abyss and the pits. Of two
    // neighbouring samples that climb more than kRockSlope, each one in a wall is marked a cliff; then
    // the slope limit cuts back the rest, the mountains' foot, the headlands and all.
    if (!towers) {
        const auto wall = [&](std::size_t i) {
            const Vec2 p = sampleAt(i % kN, i / kN);
            if (!ruins) {
                const float c = ground.past(p, ground.coast);
                return c > 0.f && c <= kQuayRun + cell;
            }
            if (ground.pastLip(p) > 0.f) return true;
            for (std::size_t f = 0; f < out.featureCount_; ++f)
                if (out.features_[f].kind == TerrainFeature::Kind::Pit && out.features_[f].covers(p, -cell)) return true;
            return false;
        };
        const auto mark = [&](std::size_t i, std::size_t j, std::int32_t most) {
            if (std::fabs(out.heights_[i] - out.heights_[j]) <= static_cast<float>(most) * WorldTerrain::kHeightStep) return;
            if (wall(i)) out.cliffs_[i] = 1;
            if (wall(j)) out.cliffs_[j] = 1;
        };
        for (std::size_t iy = 0; iy < kN; ++iy)
            for (std::size_t ix = 0; ix < kN; ++ix) {
                const std::size_t i = iy * kN + ix;
                if (ix + 1 < kN) mark(i, i + 1, axisMost);
                if (iy + 1 == kN) continue;
                mark(i, i + kN, axisMost);
                if (ix + 1 < kN) mark(i, i + kN + 1, diagMost);
                if (ix > 0) mark(i, i + kN - 1, diagMost);
            }
    }
    if (!limitSlopes()) return fail("the ground's slopes don't settle");

    // The rain's way over the finished ground (flow()). Each sample passes the rain that falls on its
    // cell (rainAt), and all that reaches it, to every neighbour lower than it, in shares as the fourth
    // power of each drop over its length (after Holmgren's multiple flow directions), so that most goes
    // the steepest way and some spreads; upstream first, each sample once all that passes to it has.
    // What reaches a sample with no lower neighbour, a closed hollow's floor or a flat such as a pad,
    // stays there, as does what reaches an outlet: the field's edge, water over the ground, a trench's
    // floor or a pit's shaft. Nothing fills the hollows, so no levelled ground draws lines across them.
    // flow() is that log-scaled and faded by rainAt, so that it is 0 on the pads and the roads.
    {
        constexpr std::size_t kTotal = kN * kN;
        static constexpr std::array<std::ptrdiff_t, 8> kStepX{1, 1, 0, -1, -1, -1, 0, 1}; // kCompass's order
        static constexpr std::array<std::ptrdiff_t, 8> kStepY{0, 1, 1, 1, 0, -1, -1, -1};
        // Neighbour k of sample c, if the field has it.
        const auto neighbour = [](std::size_t c, std::size_t k, std::size_t& j) {
            const std::ptrdiff_t x = static_cast<std::ptrdiff_t>(c % kN) + kStepX[k];
            const std::ptrdiff_t y = static_cast<std::ptrdiff_t>(c / kN) + kStepY[k];
            if (x < 0 || y < 0 || x >= static_cast<std::ptrdiff_t>(kN) || y >= static_cast<std::ptrdiff_t>(kN)) return false;
            j = static_cast<std::size_t>(y) * kN + static_cast<std::size_t>(x);
            return true;
        };
        // The outlets, which pass nothing on, and how many samples pass theirs to each: its higher
        // neighbours but the outlets.
        std::vector<std::uint8_t> outlet(kTotal, 0), donors(kTotal, 0);
        for (std::size_t iy = 0; iy < kN; ++iy)
            for (std::size_t ix = 0; ix < kN; ++ix) {
                const std::size_t i = iy * kN + ix;
                bool sink = ix == 0 || iy == 0 || ix + 1 == kN || iy + 1 == kN;
                for (std::size_t f = 0; f < out.featureCount_ && !sink; ++f) {
                    const TerrainFeature& feature = out.features_[f];
                    const bool water = feature.kind == TerrainFeature::Kind::Sea || feature.kind == TerrainFeature::Kind::Lake ||
                                       feature.kind == TerrainFeature::Kind::Pool;
                    const bool under = water ? out.heights_[i] < feature.level
                                             : units[i] <= std::lround(feature.level / WorldTerrain::kHeightStep);
                    sink = under && feature.covers(sampleAt(ix, iy));
                }
                outlet[i] = sink ? 1 : 0;
            }
        for (std::size_t c = 0; c < kTotal; ++c) {
            if (outlet[c] != 0) continue;
            for (std::size_t k = 0; k < 8; ++k) {
                std::size_t j = 0;
                if (neighbour(c, k, j) && units[j] < units[c]) ++donors[j];
            }
        }
        std::vector<double> area(kTotal);
        const double cellArea = static_cast<double>(cell) * static_cast<double>(cell);
        for (std::size_t i = 0; i < kTotal; ++i) area[i] = static_cast<double>(rainAt[i]) * cellArea;
        std::vector<std::size_t> queue;
        queue.reserve(kTotal);
        for (std::size_t i = 0; i < kTotal; ++i)
            if (donors[i] == 0) queue.push_back(i);
        for (std::size_t head = 0; head < queue.size(); ++head) {
            const std::size_t c = queue[head];
            if (outlet[c] != 0) continue;
            std::array<std::size_t, 8> to{};
            std::array<double, 8> share{};
            double sum = 0.0;
            for (std::size_t k = 0; k < 8; ++k) {
                if (!neighbour(c, k, to[k]) || units[to[k]] >= units[c]) continue;
                const double drop = static_cast<double>(units[c] - units[to[k]]) * (k % 2 == 1 ? static_cast<double>(kDiag) : 1.0);
                share[k] = (drop * drop) * (drop * drop);
                sum += share[k];
            }
            for (std::size_t k = 0; k < 8; ++k) {
                if (!(share[k] > 0.0)) continue;
                area[to[k]] += area[c] * (share[k] / sum);
                if (--donors[to[k]] == 0) queue.push_back(to[k]);
            }
        }
        out.flow_.assign(kTotal, 0.f);
        // Counted in kFlowUnit grown with a sample's area from its area at kTunedSide.
        const float grown = (cell * cell) / (tunedCell * tunedCell);
        const float flowUnit = WorldTerrain::kFlowUnit * grown;
        const double full = std::log1p(static_cast<double>(WorldTerrain::kFlowFull / flowUnit));
        for (std::size_t i = 0; i < kTotal; ++i)
            out.flow_[i] = rainAt[i] * static_cast<float>(std::min(std::log1p(area[i] / flowUnit) / full, 1.0));
    }

    out.side_ = spec.side;
    out.districtCount_ = n;
    out.roadCount_ = roads;
    for (std::size_t i = 0; i < kMaxPads; ++i) out.pads_[i] = i < n ? pads[i].height : 0.f;
    for (std::size_t r = 0; r < WorldLayout::kMaxRoads; ++r)
        out.roads_[r] = r < roads ? WorldTerrain::Road{length[r], spans[r].from, spans[r].to} : WorldTerrain::Road{};
    return true;
}

void roadMask(const WorldLayout& layout, std::size_t size, std::uint8_t* out) {
    if (out == nullptr || size == 0) return;
    std::fill(out, out + size * size, std::uint8_t{0});
    if (size < 3) return;
    const float half = layout.side() * 0.5f;
    const float pixel = layout.side() / static_cast<float>(size);
    const float clear = WorldTerrain::kRoadHalfWidth + WorldTerrain::kRoadMaskFeather; // 0 from here
    const float last = static_cast<float>(size - 2);
    // The pixels whose centres span metres lo..hi along an axis, kept off the outermost ring.
    const auto lowPixel = [&](float lo) {
        return static_cast<std::size_t>(std::clamp(std::floor((lo + half) / pixel - 0.5f), 1.f, last));
    };
    const auto highPixel = [&](float hi) {
        return static_cast<std::size_t>(std::clamp(std::ceil((hi + half) / pixel - 0.5f), 1.f, last));
    };
    for (std::size_t r = 0; r < layout.roadCount(); ++r) {
        const Vec2 a = layout.district(layout.road(r).a).centre, b = layout.district(layout.road(r).b).centre;
        const std::size_t x0 = lowPixel(std::min(a.x, b.x) - clear), x1 = highPixel(std::max(a.x, b.x) + clear);
        const std::size_t y0 = lowPixel(std::min(a.y, b.y) - clear), y1 = highPixel(std::max(a.y, b.y) + clear);
        for (std::size_t iy = y0; iy <= y1; ++iy)
            for (std::size_t ix = x0; ix <= x1; ++ix) {
                const Vec2 p{-half + (static_cast<float>(ix) + 0.5f) * pixel, -half + (static_cast<float>(iy) + 0.5f) * pixel};
                float d = 0.f;
                nearestOnSegment(p, a, b, d);
                const float v = 255.f * std::clamp(0.5f + (WorldTerrain::kRoadHalfWidth - d) /
                                                              (2.f * WorldTerrain::kRoadMaskFeather),
                                                   0.f, 1.f);
                std::uint8_t& o = out[iy * size + ix];
                o = std::max(o, static_cast<std::uint8_t>(v + 0.5f)); // the nearest road's, where two meet
            }
    }
}

void shapeMask(const WorldTerrain& terrain, std::size_t size, std::uint8_t* out) {
    if (out == nullptr || size == 0) return;
    const float half = terrain.extent() * 0.5f;
    const float pixel = terrain.extent() / static_cast<float>(size);
    constexpr float kDiagonal = 0.70710678f;
    constexpr std::array<Vec2, 8> kRing{{{1.f, 0.f}, {kDiagonal, kDiagonal}, {0.f, 1.f}, {-kDiagonal, kDiagonal},
                                         {-1.f, 0.f}, {-kDiagonal, -kDiagonal}, {0.f, -1.f}, {kDiagonal, -kDiagonal}}};
    const std::array<float, 2> radii{static_cast<float>(WorldTerrain::kShapeNear) * terrain.spacing(),
                                     static_cast<float>(WorldTerrain::kShapeFar) * terrain.spacing()};
    for (std::size_t iy = 0; iy < size; ++iy)
        for (std::size_t ix = 0; ix < size; ++ix) {
            const Vec2 p{-half + (static_cast<float>(ix) + 0.5f) * pixel, -half + (static_cast<float>(iy) + 0.5f) * pixel};
            const float h = terrain.height(p);
            float t = 0.f;
            for (const float r : radii) {
                // Heights lie on whole steps, so on flat ground the ring's sum is exact and t exactly 0.
                float ring = 0.f;
                for (const Vec2& d : kRing) ring += terrain.height({p.x + r * d.x, p.y + r * d.y});
                t += (h - ring / static_cast<float>(kRing.size())) / r;
            }
            t /= static_cast<float>(radii.size());
            const float v = std::clamp(128.f + 127.f * t / WorldTerrain::kShapeFull, 0.f, 255.f);
            out[iy * size + ix] = static_cast<std::uint8_t>(v + 0.5f);
        }
}

void flowMask(const WorldTerrain& terrain, std::size_t size, std::uint8_t* out) {
    if (out == nullptr || size == 0) return;
    if (!(terrain.side() > 0.f)) {
        std::fill(out, out + size * size, std::uint8_t{0});
        return;
    }
    constexpr std::size_t kLast = WorldTerrain::kSamples - 1;
    const float half = terrain.extent() * 0.5f;
    const float pixel = terrain.extent() / static_cast<float>(size);
    const float cell = terrain.spacing();
    for (std::size_t iy = 0; iy < size; ++iy)
        for (std::size_t ix = 0; ix < size; ++ix) {
            const Vec2 p{-half + (static_cast<float>(ix) + 0.5f) * pixel, -half + (static_cast<float>(iy) + 0.5f) * pixel};
            const float gx = std::clamp((p.x + half) / cell, 0.f, static_cast<float>(kLast));
            const float gy = std::clamp((p.y + half) / cell, 0.f, static_cast<float>(kLast));
            const std::size_t x0 = std::min(static_cast<std::size_t>(gx), kLast - 1);
            const std::size_t y0 = std::min(static_cast<std::size_t>(gy), kLast - 1);
            const float fx = gx - static_cast<float>(x0), fy = gy - static_cast<float>(y0);
            const float f = lerpf(lerpf(terrain.flow(x0, y0), terrain.flow(x0 + 1, y0), fx),
                                  lerpf(terrain.flow(x0, y0 + 1), terrain.flow(x0 + 1, y0 + 1), fx), fy);
            out[iy * size + ix] = static_cast<std::uint8_t>(std::clamp(255.f * f, 0.f, 255.f) + 0.5f);
        }
}

} // namespace ftcore
