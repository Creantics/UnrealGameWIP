#include "ftcore/ArenaStage.h"

#include "SeedMix.h"
#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace ftcore {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = 180.f / kPi;

// ---- scale ----

constexpr float kBaseRing = 8.5f;
constexpr float kRingPerEnemy = 1.5f;
constexpr float kRingPerAttacker = 0.75f;
constexpr float kMinRing = 7.f;
constexpr float kMaxRing = 60.f;
constexpr float kMinPinnedRing = 6.f;
constexpr float kMaxPinnedRing = 80.f;
constexpr float kMaxSpawn = 12.f;
constexpr float kSpawnShare = 0.58f;
// The reference body the scale is 1 at: the shipped wave's biggest (radius 0.5 m, 2.2 m tall).
constexpr float kRefRadius = 0.5f;
constexpr float kRefHeight = 2.2f;
constexpr float kMaxScale = 6.f;
// Flat decals keep this far from the player's start, an enemy's body edge and the lines between them (m).
constexpr float kKeepPlayer = 3.f;
constexpr float kKeepEnemy = 2.5f;
constexpr float kKeepLane = 2.f;
// A solid piece's nearest edge keeps this far outside the ring (m).
constexpr float kRingClear = 0.1f;

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float lengthOf(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
Vec2 minus(Vec2 a, Vec2 b) { return Vec2{a.x - b.x, a.y - b.y}; }
Vec2 plus(Vec2 a, Vec2 b) { return Vec2{a.x + b.x, a.y + b.y}; }
Vec2 scaled(Vec2 a, float s) { return Vec2{a.x * s, a.y * s}; }

// xorshift32 on a mixed seed.
class Rng {
public:
    explicit Rng(std::uint32_t seed) : s_(detail::mixSeed(seed)) {}
    std::uint32_t next() {
        s_ ^= s_ << 13;
        s_ ^= s_ >> 17;
        s_ ^= s_ << 5;
        return s_;
    }
    float uni() { return static_cast<float>(next() >> 8) * (1.f / 16777216.f); }       // [0, 1)
    float range(float lo, float hi) { return lo + (hi - lo) * uni(); }
    float sign() { return (next() & 1u) ? 1.f : -1.f; }

private:
    std::uint32_t s_;
};

// ---- names ----

constexpr const char* kFloorNames[kStageFloorCount] = {"circle", "square", "broken_ring", "island"};
constexpr const char* kBoundNames[kStageBoundCount] = {"wall", "edge", "water", "fog_wall"};
constexpr const char* kThemeNames[kStageThemeCount] = {"plain", "street", "canal", "circuit", "grid", "shore", "void"};

template <std::size_t N, typename E>
bool parseName(const char* const (&names)[N], std::string_view text, E& out) {
    for (std::size_t i = 0; i < N; ++i) {
        if (text == names[i]) {
            out = static_cast<E>(i);
            return true;
        }
    }
    return false;
}

// ---- the worlds' looks ----

struct WorldLook {
    std::array<StageRoleLook, kStageRoleCount> roles;
    SceneLook look;
    float sunYaw, sunPitch;
    float fogR, fogG, fogB;
    float lightA[3], lightB[3]; // two lamp colours, alternating
    float lightHeight;          // lamp height as a share of the pillar's
    float lightScale;           // multiplies a stage's lamp intensity
};

// roles: Floor, Ground, Structure, Accent, Line, Dark, Water. {r, g, b, roughness, metallic, glow}
WorldLook worldLook(StylePack world) {
    WorldLook w{};
    switch (world) {
    case StylePack::Solhaven: // a contemporary night city: dark asphalt, sodium and cursed purple
        w.roles = {StageRoleLook{0.05f, 0.05f, 0.06f, 0.45f, 0.f, 0.f},   StageRoleLook{0.03f, 0.03f, 0.04f, 0.8f, 0.f, 0.f},
                   StageRoleLook{0.10f, 0.09f, 0.12f, 0.7f, 0.f, 0.f},    StageRoleLook{0.55f, 0.12f, 1.f, 0.4f, 0.f, 6.f},
                   StageRoleLook{0.75f, 0.35f, 0.12f, 0.6f, 0.f, 0.8f},   StageRoleLook{0.02f, 0.015f, 0.03f, 0.9f, 0.f, 0.f},
                   StageRoleLook{0.15f, 0.4f, 0.9f, 0.04f, 0.f, 2.5f}};
        w.look.sunLux = 0.8f;
        w.look.sunKelvin = 9000.f;
        w.look.skyLight = 0.5f;
        w.look.exposureMinEV100 = 6.5f; // held: a night street stays dark between its lamps
        w.look.exposureMaxEV100 = 6.5f;
        w.look.fogDensity = 0.045f;
        w.look.fogFalloff = 0.25f;
        w.look.volumetricFog = 1.f;
        w.look.fogForward = 0.5f;
        w.look.clouds = 0.f;
        w.look.exposureBias = 0.5f;
        w.look.saturation = 1.08f;
        w.look.contrast = 1.1f;
        w.look.bloom = 1.1f;
        w.look.vignette = 0.5f;
        w.sunYaw = 215.f;
        w.sunPitch = 8.f; // below the horizon: night
        w.fogR = 0.10f; w.fogG = 0.06f; w.fogB = 0.18f;
        w.lightA[0] = 1.f; w.lightA[1] = 0.55f; w.lightA[2] = 0.2f;   // sodium
        w.lightB[0] = 0.6f; w.lightB[1] = 0.2f; w.lightB[2] = 1.f;    // cursed purple
        w.lightHeight = 0.95f;
        w.lightScale = 1.1f;
        break;
    case StylePack::Lattice: // a game-themed world: mid blue ground, pale structure, blue data light
        w.roles = {StageRoleLook{0.16f, 0.24f, 0.38f, 0.3f, 0.2f, 0.f},   StageRoleLook{0.08f, 0.12f, 0.2f, 0.5f, 0.f, 0.f},
                   StageRoleLook{0.55f, 0.65f, 0.8f, 0.35f, 0.3f, 0.f},   StageRoleLook{0.15f, 0.55f, 1.f, 0.3f, 0.f, 14.f},
                   StageRoleLook{0.3f, 0.7f, 1.f, 0.3f, 0.f, 8.f},        StageRoleLook{0.05f, 0.07f, 0.12f, 0.6f, 0.f, 0.f},
                   StageRoleLook{0.1f, 0.5f, 1.f, 0.03f, 0.f, 30.f}};
        w.look.sunLux = 300.f;
        w.look.sunKelvin = 10000.f;
        w.look.skyLight = 1.f;
        w.look.exposureMinEV100 = 8.f; // held: the sky is a bright blue, the glow reads against it
        w.look.exposureMaxEV100 = 8.f;
        w.look.fogDensity = 0.02f;
        w.look.fogFalloff = 0.2f;
        w.look.volumetricFog = 1.f;
        w.look.fogForward = 0.4f;
        w.look.clouds = 0.f;
        w.look.exposureBias = 1.f;
        w.look.saturation = 1.12f;
        w.look.bloom = 1.3f;
        w.look.vignette = 0.35f;
        w.sunYaw = 40.f;
        w.sunPitch = -35.f;
        w.fogR = 0.22f; w.fogG = 0.42f; w.fogB = 0.8f;
        w.lightA[0] = 0.3f; w.lightA[1] = 0.6f; w.lightA[2] = 1.f;
        w.lightB[0] = 0.5f; w.lightB[1] = 0.85f; w.lightB[2] = 1.f;
        w.lightHeight = 0.9f;
        w.lightScale = 1.f;
        break;
    case StylePack::Unwritten: // a horror void: grey half-formed ground, black water, thick fog, sparse cold light
        w.roles = {StageRoleLook{0.10f, 0.11f, 0.12f, 0.85f, 0.f, 0.f},   StageRoleLook{0.05f, 0.055f, 0.06f, 0.9f, 0.f, 0.f},
                   StageRoleLook{0.17f, 0.18f, 0.19f, 0.8f, 0.f, 0.f},    StageRoleLook{0.5f, 0.65f, 0.8f, 0.5f, 0.f, 1.2f},
                   StageRoleLook{0.4f, 0.45f, 0.5f, 0.6f, 0.f, 0.3f},     StageRoleLook{0.02f, 0.02f, 0.025f, 0.9f, 0.f, 0.f},
                   StageRoleLook{0.5f, 0.6f, 0.7f, 0.02f, 0.f, 1.2f}};
        w.look.sunLux = 0.1f;
        w.look.sunKelvin = 10000.f;
        w.look.skyLight = 0.3f;
        w.look.exposureMinEV100 = 5.5f;
        w.look.exposureMaxEV100 = 5.5f;
        w.look.fogDensity = 0.14f;
        w.look.fogFalloff = 0.15f;
        w.look.volumetricFog = 1.5f;
        w.look.fogForward = 0.2f;
        w.look.clouds = 0.f;
        w.look.exposureBias = 0.5f;
        w.look.saturation = 0.6f;
        w.look.bloom = 0.5f;
        w.look.vignette = 0.7f;
        w.sunYaw = 120.f;
        w.sunPitch = 12.f;
        w.fogR = 0.07f; w.fogG = 0.08f; w.fogB = 0.1f;
        w.lightA[0] = 0.7f; w.lightA[1] = 0.8f; w.lightA[2] = 1.f;
        w.lightB[0] = 0.55f; w.lightB[1] = 0.65f; w.lightB[2] = 0.8f;
        w.lightHeight = 0.7f;
        w.lightScale = 0.8f;
        break;
    default: // the generic sand ring: the look every map shared before the worlds had their own
        w.roles = {StageRoleLook{0.42f, 0.4f, 0.36f, 0.85f, 0.f, 0.f},    StageRoleLook{0.16f, 0.15f, 0.13f, 0.9f, 0.f, 0.f},
                   StageRoleLook{0.27f, 0.25f, 0.22f, 0.85f, 0.f, 0.f},   StageRoleLook{0.85f, 0.7f, 0.4f, 0.6f, 0.f, 0.f},
                   StageRoleLook{0.85f, 0.82f, 0.7f, 0.85f, 0.f, 0.f},    StageRoleLook{0.1f, 0.1f, 0.1f, 0.9f, 0.f, 0.f},
                   StageRoleLook{0.02f, 0.04f, 0.06f, 0.05f, 0.7f, 0.f}};
        w.sunYaw = 35.f;
        w.sunPitch = -38.f;
        w.fogR = 0.5f; w.fogG = 0.6f; w.fogB = 0.7f;
        w.lightA[0] = 1.f; w.lightA[1] = 0.82f; w.lightA[2] = 0.6f;
        w.lightB[0] = 1.f; w.lightB[1] = 0.82f; w.lightB[2] = 0.6f;
        w.lightHeight = 1.f;
        w.lightScale = 0.05f; // the old demo's lamps were in the engine's legacy unit: next to a few lux of sun, a glow
        break;
    }
    return w;
}

// ---- pieces ----

// A frame at a point on the ground turned `angle` radians: u points away from the origin, v along the circle.
struct Frame {
    Vec2 c;
    float a = 0.f;
    Vec2 at(float u, float v) const {
        const float ca = std::cos(a), sa = std::sin(a);
        return Vec2{c.x + u * ca - v * sa, c.y + u * sa + v * ca};
    }
    float yaw() const { return a * kDeg; }
};

Frame frameAt(float radius, float angle) {
    return Frame{Vec2{radius * std::cos(angle), radius * std::sin(angle)}, angle};
}

StagePiece box(StageGroup group, StageRole role, Vec2 at, float z, float sx, float sy, float sz, float yaw, float pitch = 0.f) {
    StagePiece p;
    p.group = group;
    p.shape = StageShape::Box;
    p.role = role;
    p.at = at;
    p.z = z;
    p.sx = sx;
    p.sy = sy;
    p.sz = sz;
    p.yaw = yaw;
    p.pitch = pitch;
    return p;
}

StagePiece cylinder(StageGroup group, StageRole role, Vec2 at, float z, float diameter, float sz) {
    StagePiece p;
    p.group = group;
    p.shape = StageShape::Cylinder;
    p.role = role;
    p.at = at;
    p.z = z;
    p.sx = diameter;
    p.sy = diameter;
    p.sz = sz;
    return p;
}

// The nearest the piece's footprint comes to `point` on the ground (0 inside it), a lean's reach included.
float footprintDistance(const StagePiece& p, Vec2 point) {
    const float lean = std::fabs(p.sz * std::sin(p.pitch / kDeg));
    if (p.shape == StageShape::Cylinder) {
        return std::max(0.f, lengthOf(minus(point, p.at)) - 0.5f * p.sx - lean);
    }
    const float ca = std::cos(p.yaw / kDeg), sa = std::sin(p.yaw / kDeg);
    const Vec2 d = minus(point, p.at);
    const float lx = d.x * ca + d.y * sa;  // into the piece's frame
    const float ly = -d.x * sa + d.y * ca;
    const float ex = std::max(std::fabs(lx) - (0.5f * p.sx + lean), 0.f);
    const float ey = std::max(std::fabs(ly) - 0.5f * p.sy, 0.f);
    return std::sqrt(ex * ex + ey * ey);
}

bool isSolid(StageGroup g) {
    return g == StageGroup::Fragment || g == StageGroup::Rim || g == StageGroup::Cover || g == StageGroup::Pillar ||
           g == StageGroup::Landmark;
}
bool isDecal(StageGroup g) { return g == StageGroup::Debris || g == StageGroup::Pool || g == StageGroup::Crack; }

// ---- where the bodies start ----

struct Starts {
    Vec2 player;
    std::array<Vec2, kMaxArenaEnemies> enemy{};
    std::array<float, kMaxArenaEnemies> radius{};
    std::size_t count = 0;
};

Starts startsOf(const ArenaDef& def) {
    Starts s;
    ArenaFight fight;
    fight.begin(def, arenaStageRing(def.ringRadius), arenaStagePlayerStart(def.ringRadius), Vec2{1.f, 0.f}, 1u);
    s.player = fight.playerStart();
    s.count = fight.enemyCount();
    for (std::size_t i = 0; i < s.count; ++i) {
        s.enemy[i] = fight.enemyStart(i);
        s.radius[i] = def.enemies[i].bodyRadius;
    }
    return s;
}

// The distance from `p` to the segment a-b.
float segmentDistance(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = minus(b, a);
    const float len2 = ab.x * ab.x + ab.y * ab.y;
    const float t = len2 > 0.f ? clampf(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len2, 0.f, 1.f) : 0.f;
    return lengthOf(minus(p, plus(a, scaled(ab, t))));
}

// How far a decal footprint (a circle of `radius` at `p`) is from breaking a keep-out rule; negative when it breaks one.
float decalClearance(const Starts& s, Vec2 p, float radius) {
    float least = lengthOf(minus(p, s.player)) - kKeepPlayer - radius;
    for (std::size_t i = 0; i < s.count; ++i) {
        least = std::min(least, lengthOf(minus(p, s.enemy[i])) - (s.radius[i] + kKeepEnemy) - radius);
        least = std::min(least, segmentDistance(p, s.player, s.enemy[i]) - (kKeepLane + s.radius[i]) - radius);
    }
    return least;
}

// A piece's own clearance, exact: the nearest its footprint comes to each rule's centre, less the rule's reach.
float pieceDecalClearance(const Starts& s, const StagePiece& p) {
    float least = footprintDistance(p, s.player) - kKeepPlayer;
    for (std::size_t i = 0; i < s.count; ++i) {
        least = std::min(least, footprintDistance(p, s.enemy[i]) - (s.radius[i] + kKeepEnemy));
        // The lane: sample the segment every half metre.
        const Vec2 d = minus(s.enemy[i], s.player);
        const float len = lengthOf(d);
        const int steps = std::max(1, static_cast<int>(len / 0.5f));
        for (int k = 0; k <= steps; ++k) {
            const Vec2 q = plus(s.player, scaled(d, static_cast<float>(k) / static_cast<float>(steps)));
            least = std::min(least, footprintDistance(p, q) - (kKeepLane + s.radius[i]));
        }
    }
    return least;
}

// ---- landmarks ----

void addLandmark(ArenaStageLayout& out, Rng& rng, StageTheme theme, const Frame& f, float s) {
    const float h = rng.range(8.f, 14.f) * s;
    const float yaw = f.yaw();
    const auto add = [&](StagePiece p) { out.pieces.push_back(p); };
    switch (theme) {
    case StageTheme::Street: // a stacked tower with a lit band
    case StageTheme::Plain: {
        const float w = rng.range(3.5f, 5.f) * s;
        add(box(StageGroup::Landmark, StageRole::Structure, f.at(0, 0), 0.f, w, w, h * 0.6f, yaw));
        add(box(StageGroup::Landmark, StageRole::Structure, f.at(0, 0), h * 0.6f, w * 0.78f, w * 0.78f, h * 0.3f, yaw));
        add(box(StageGroup::Landmark, StageRole::Structure, f.at(0, 0), h * 0.9f, w * 0.5f, w * 0.5f, h * 0.1f, yaw));
        add(box(StageGroup::Landmark, StageRole::Accent, f.at(-w * 0.5f + 0.05f * s, 0), h * 0.25f, 0.12f * s, w * 0.6f, h * 0.5f, yaw));
        add(box(StageGroup::Landmark, StageRole::Accent, f.at(0, 0), h * 0.9f - 0.15f * s, w * 0.8f, w * 0.8f, 0.3f * s, yaw));
        break;
    }
    case StageTheme::Canal: // an arch: two piers and a lintel with a lit underside
    case StageTheme::Void: {
        if (theme == StageTheme::Void) {
            // A leaning monolith with a floating chip beside it.
            const float w = rng.range(2.5f, 3.5f) * s;
            add(box(StageGroup::Landmark, StageRole::Structure, f.at(0, 0), 0.f, w, 0.9f * s, h, yaw, rng.range(-10.f, 10.f)));
            add(box(StageGroup::Landmark, StageRole::Accent, f.at(0, 0), h * 0.35f, w * 0.7f, 0.95f * s, 0.12f * s, yaw, 0.f));
            add(box(StageGroup::Landmark, StageRole::Structure, f.at(0, rng.sign() * 4.f * s), h * 0.5f, 1.4f * s, 1.2f * s, 1.8f * s, yaw + 20.f, 8.f));
            break;
        }
        const float gap = 7.f * s;
        const float pier = 1.6f * s;
        add(box(StageGroup::Landmark, StageRole::Structure, f.at(0, -gap * 0.5f), 0.f, pier, pier, h * 0.75f, yaw));
        add(box(StageGroup::Landmark, StageRole::Structure, f.at(0, gap * 0.5f), 0.f, pier, pier, h * 0.75f, yaw));
        add(box(StageGroup::Landmark, StageRole::Structure, f.at(0, 0), h * 0.75f, pier * 1.2f, gap + pier * 2.f, h * 0.18f, yaw));
        add(box(StageGroup::Landmark, StageRole::Accent, f.at(0, 0), h * 0.75f - 0.15f * s, pier * 0.8f, gap, 0.2f * s, yaw));
        break;
    }
    case StageTheme::Circuit: { // a slim pylon with lit bands
        const float w = 1.6f * s;
        const float tall = h * 1.3f;
        add(box(StageGroup::Landmark, StageRole::Structure, f.at(0, 0), 0.f, w * 2.2f, w * 2.2f, 0.6f * s, yaw));
        add(box(StageGroup::Landmark, StageRole::Structure, f.at(0, 0), 0.6f * s, w, w, tall, yaw));
        for (int i = 0; i < 3; ++i) {
            add(box(StageGroup::Landmark, StageRole::Accent, f.at(0, 0), tall * (0.25f + 0.25f * static_cast<float>(i)), w * 1.12f,
                    w * 1.12f, 0.3f * s, yaw));
        }
        break;
    }
    case StageTheme::Grid: { // a tilted monolith with a lit edge
        const float w = rng.range(3.f, 4.f) * s;
        add(box(StageGroup::Landmark, StageRole::Structure, f.at(0, 0), 0.f, 0.9f * s, w, h * 1.1f, yaw, rng.range(-7.f, 7.f)));
        add(box(StageGroup::Landmark, StageRole::Accent, f.at(0.5f * s, 0), 0.5f * s, 0.1f * s, w * 0.8f, h * 1.05f, yaw, 0.f));
        break;
    }
    case StageTheme::Shore: { // broken columns of different heights and a fallen slab
        for (int i = 0; i < 3; ++i) {
            const float ch = h * rng.range(0.35f, 0.9f);
            out.pieces.push_back(cylinder(StageGroup::Landmark, StageRole::Structure, f.at(rng.range(-1.f, 1.f) * s, (static_cast<float>(i) - 1.f) * 3.2f * s), 0.f, 2.1f * s, ch));
        }
        add(box(StageGroup::Landmark, StageRole::Structure, f.at(-2.f * s, 0), 0.f, 5.f * s, 1.6f * s, 0.7f * s, yaw + rng.range(-25.f, 25.f), 4.f));
        break;
    }
    }
}

// ---- cover ----

void addCover(ArenaStageLayout& out, Rng& rng, StageTheme theme, const Frame& f, float s) {
    // Every random draw is its own statement: the order of draws inside one call's arguments is unspecified.
    const float d0 = rng.uni(), d1 = rng.uni(), d2 = rng.uni(), d3 = rng.uni();
    switch (theme) {
    case StageTheme::Street: // a crate, sometimes stacked
        out.pieces.push_back(box(StageGroup::Cover, StageRole::Structure, f.at(0, 0), 0.f, (1.6f + 0.8f * d0) * s, (1.2f + 0.6f * d1) * s, 1.2f * s, f.yaw() + (d2 - 0.5f) * 40.f));
        if (d3 < 0.4f) {
            out.pieces.push_back(box(StageGroup::Cover, StageRole::Structure, f.at(0, 0), 1.2f * s, 1.1f * s, 1.1f * s, 0.9f * s, f.yaw() + d0 * 60.f));
        }
        break;
    case StageTheme::Canal: // a bollard with a lit cap
        out.pieces.push_back(cylinder(StageGroup::Cover, StageRole::Structure, f.at(0, 0), 0.f, 0.9f * s, 1.1f * s));
        out.pieces.push_back(cylinder(StageGroup::Cover, StageRole::Accent, f.at(0, 0), 1.1f * s, 0.95f * s, 0.12f * s));
        break;
    case StageTheme::Circuit:
    case StageTheme::Grid: // a low plinth with a lit top
        out.pieces.push_back(cylinder(StageGroup::Cover, StageRole::Structure, f.at(0, 0), 0.f, 1.7f * s, 0.9f * s));
        out.pieces.push_back(cylinder(StageGroup::Cover, StageRole::Accent, f.at(0, 0), 0.9f * s, 1.5f * s, 0.08f * s));
        break;
    case StageTheme::Shore: // a leaning slab
        out.pieces.push_back(box(StageGroup::Cover, StageRole::Structure, f.at(0, 0), 0.f, 0.7f * s, (2.f + d0) * s, 1.7f * s, f.yaw() + (d1 - 0.5f) * 60.f, 8.f + 12.f * d2));
        break;
    case StageTheme::Void: // a broken chunk
        out.pieces.push_back(box(StageGroup::Cover, StageRole::Structure, f.at(0, 0), 0.f, (1.2f + d0) * s, (1.2f + d1) * s, (0.8f + d2) * s, f.yaw() + d3 * 90.f, (d0 - 0.5f) * 24.f));
        break;
    default: // plain: a low block
        out.pieces.push_back(box(StageGroup::Cover, StageRole::Structure, f.at(0, 0), 0.f, 1.6f * s, 1.6f * s, 1.0f * s, f.yaw()));
        break;
    }
}

// ---- the rules of the file ----

std::string fmt(double v) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%g", v);
    return buffer;
}

bool pinnedOk(float v) { return v == 0.f || (v >= kMinPinnedRing && v <= kMaxPinnedRing); }

} // namespace

// ---- table ----

const ArenaStageSpec* ArenaStageTable::find(std::string_view id) const {
    for (const ArenaStageSpec& s : stages)
        if (s.id == id) return &s;
    return nullptr;
}

const ArenaStageSpec* ArenaStageTable::defaultStage() const {
    if (const ArenaStageSpec* s = find(defaultId)) return s;
    return stages.empty() ? nullptr : &stages[0];
}

const char* stageFloorName(StageFloor floor) { return kFloorNames[std::min<std::size_t>(static_cast<std::size_t>(floor), kStageFloorCount - 1)]; }
const char* stageBoundName(StageBound bound) { return kBoundNames[std::min<std::size_t>(static_cast<std::size_t>(bound), kStageBoundCount - 1)]; }
const char* stageThemeName(StageTheme theme) { return kThemeNames[std::min<std::size_t>(static_cast<std::size_t>(theme), kStageThemeCount - 1)]; }

bool ArenaStageLayout::hasWater() const {
    if (bound == StageBound::Water || floor == StageFloor::Island) return true;
    for (const StagePiece& p : pieces)
        if (p.role == StageRole::Water) return true;
    return false;
}

std::size_t ArenaStageLayout::count(StageGroup group) const {
    std::size_t n = 0;
    for (const StagePiece& p : pieces)
        if (p.group == group) ++n;
    return n;
}

namespace {

void unknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys,
                 std::vector<std::string>* warnings) {
    if (!warnings) return;
    for (std::uint32_t k = 0; k < j.size(); ++k) {
        const std::string_view key = j.key(k);
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            warnings->push_back((path.empty() ? std::string() : path + ": ") + "unknown key: " + std::string(key));
    }
}

void readInt(JsonValue j, const char* key, int& out, int lo, int hi, const std::string& path, std::vector<std::string>& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent()) return;
    if (!v.isInteger() || v.integer() < lo || v.integer() > hi) {
        errors.push_back(path + "." + key + ": expected an integer in " + std::to_string(lo) + ".." + std::to_string(hi));
        return;
    }
    out = static_cast<int>(v.integer());
}

void readNum(JsonValue j, const char* key, float& out, double lo, double hi, bool loOpen, const std::string& path,
             std::vector<std::string>& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent()) return;
    const double d = v.number(-1e30);
    if (!(v.isNumber() && std::isfinite(d) && (loOpen ? d > lo : d >= lo) && d <= hi)) {
        errors.push_back(path + "." + key + ": expected a number in " + (loOpen ? "(" : "[") + fmt(lo) + ", " + fmt(hi) + "]");
        return;
    }
    out = static_cast<float>(d);
}

template <std::size_t N, typename E>
void readName(JsonValue j, const char* key, const char* const (&names)[N], E& out, const std::string& path,
              std::vector<std::string>& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent()) return;
    if (!v.isString() || !parseName(names, v.string(), out)) {
        std::string list;
        for (std::size_t i = 0; i < N; ++i) list += (i ? ", " : "") + std::string(names[i]);
        errors.push_back(path + "." + key + ": expected one of " + list);
    }
}

} // namespace

bool importArenaStageTable(std::string_view json, const ArenaTable& arena, ArenaStageTable& out, std::string* error,
                           std::vector<std::string>* warnings) {
    out = ArenaStageTable{};
    std::vector<std::string> errors;
    const auto finish = [&]() {
        if (errors.empty()) return true;
        if (error) {
            error->clear();
            for (const std::string& e : errors) {
                if (!error->empty()) *error += '\n';
                *error += e;
            }
        }
        return false;
    };

    JsonDocument doc;
    std::string err;
    if (!doc.parse(json, &err)) {
        errors.push_back("arenas file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("arenas file: expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !v.isAbsent() && !(v.isInteger() && v.integer() == 1))
        errors.push_back("version: expected 1");
    unknownKeys(root, "", {"version", "default", "stages"}, warnings);

    ArenaStageTable table;
    const JsonValue stages = root["stages"];
    if (!stages.isArray() || stages.size() == 0) errors.push_back("stages: expected a non-empty array");
    for (std::uint32_t i = 0; i < stages.size() && stages.isArray(); ++i) {
        const JsonValue j = stages[i];
        const std::string path = "stages[" + std::to_string(i) + "]";
        if (!j.isObject()) {
            errors.push_back(path + ": expected an object");
            continue;
        }
        unknownKeys(j, path,
                    {"id", "world", "theme", "bosses", "floor", "bound", "ring", "spawn", "attackers", "seed", "features"},
                    warnings);
        ArenaStageSpec s;
        s.id = std::string(j["id"].string());
        if (s.id.empty()) errors.push_back(path + ".id: expected an id");
        for (const ArenaStageSpec& known : table.stages)
            if (!s.id.empty() && known.id == s.id) errors.push_back(path + ".id: duplicate id: " + s.id);

        if (const JsonValue w = j["world"]; !w.isAbsent()) {
            const std::optional<StylePack> pack = w.isString() ? parseStylePack(w.string()) : std::nullopt;
            if (!pack) errors.push_back(path + ".world: expected none, solhaven, lattice, unwritten or restored");
            else s.world = *pack;
        }
        readName(j, "theme", kThemeNames, s.theme, path, errors);
        readName(j, "floor", kFloorNames, s.floor, path, errors);
        readName(j, "bound", kBoundNames, s.bound, path, errors);

        const JsonValue bosses = j["bosses"];
        if (!bosses.isArray() || bosses.size() == 0 || bosses.size() > kMaxArenaEnemies) {
            errors.push_back(path + ".bosses: expected an array of 1.." + std::to_string(kMaxArenaEnemies) + " enemy kind ids");
        } else {
            for (std::uint32_t b = 0; b < bosses.size(); ++b) {
                const std::string id(bosses[b].string());
                if (std::find(arena.enemyIds.begin(), arena.enemyIds.end(), id) == arena.enemyIds.end())
                    errors.push_back(path + ".bosses[" + std::to_string(b) + "]: unknown enemy id: " + id);
                s.bosses.push_back(id);
            }
        }

        readNum(j, "ring", s.ring, 0, kMaxPinnedRing, false, path, errors);
        if (!pinnedOk(s.ring)) errors.push_back(path + ".ring: expected 0 (from the enemies' size) or " + fmt(kMinPinnedRing) + ".." + fmt(kMaxPinnedRing));
        readNum(j, "spawn", s.spawn, 0, kMaxSpawn, false, path, errors);
        readInt(j, "attackers", s.attackers, 0, static_cast<int>(kMaxArenaEnemies), path, errors);
        int seed = 1;
        readInt(j, "seed", seed, 0, 1000000, path, errors);
        s.seed = static_cast<std::uint32_t>(seed);

        if (const JsonValue f = j["features"]; !f.isAbsent()) {
            if (!f.isObject()) {
                errors.push_back(path + ".features: expected an object");
            } else {
                const std::string fp = path + ".features";
                unknownKeys(f, fp, {"cover", "pillars", "landmarks", "pools", "cracks", "lights", "lightIntensity", "fog"}, warnings);
                readInt(f, "cover", s.cover, 0, 16, fp, errors);
                readInt(f, "pillars", s.pillars, 0, 24, fp, errors);
                readInt(f, "landmarks", s.landmarks, 0, 8, fp, errors);
                readInt(f, "pools", s.pools, 0, 6, fp, errors);
                readInt(f, "cracks", s.cracks, 0, 12, fp, errors);
                readInt(f, "lights", s.lights, 0, 24, fp, errors);
                readNum(f, "lightIntensity", s.lightIntensity, 0, 10, true, fp, errors);
                readNum(f, "fog", s.fog, 0, 4, false, fp, errors);
            }
        }
        table.stages.push_back(std::move(s));
    }

    if (const JsonValue d = root["default"]; !d.isAbsent()) {
        table.defaultId = std::string(d.string());
        if (!d.isString() || !table.find(table.defaultId)) errors.push_back("default: expected the id of a stage");
    } else if (!table.stages.empty()) {
        table.defaultId = table.stages[0].id;
    }

    if (!finish()) return false;
    out = std::move(table);
    return true;
}

// ---- scale ----

ArenaStageSizing arenaStageSizing(const ArenaDef& def) {
    ArenaStageSizing z;
    z.enemies = std::clamp<int>(def.enemyCount, 1, static_cast<int>(kMaxArenaEnemies));
    z.attackers = std::clamp<int>(def.attackers, 1, z.enemies);
    z.bodyRadius = 0.f;
    z.height = 0.f;
    for (int i = 0; i < z.enemies; ++i) {
        const ArenaEnemyKind& kind = def.enemies[static_cast<std::size_t>(i)];
        z.bodyRadius = std::max(z.bodyRadius, kind.bodyRadius);
        z.height = std::max(z.height, arenaEnemyHeight(kind));
    }
    z.k = clampf(std::max(z.bodyRadius / kRefRadius, z.height / kRefHeight), 1.f, kMaxScale);
    z.propScale = 1.f + 0.55f * (z.k - 1.f);
    return z;
}

void arenaStageFit(const ArenaStageSpec& spec, ArenaDef& def) {
    if (spec.attackers > 0) {
        def.attackers = static_cast<std::uint8_t>(std::clamp<int>(spec.attackers, 1, std::max<int>(def.enemyCount, 1)));
    }
    const ArenaStageSizing z = arenaStageSizing(def);
    float ring = spec.ring;
    if (!(ring > 0.f)) {
        const float base = kBaseRing + kRingPerEnemy * static_cast<float>(z.enemies - 1) +
                           kRingPerAttacker * static_cast<float>(z.attackers - 1);
        ring = clampf(base * (0.4f + 0.6f * z.k), kMinRing, kMaxRing);
    }
    ring = clampf(ring, 3.f, kMaxPinnedRing);
    float spawn = spec.spawn;
    if (!(spawn > 0.f)) spawn = std::max(kSpawnShare * ring, z.bodyRadius + 4.f);
    def.ringRadius = ring;
    def.spawnDistance = std::min({spawn, kMaxSpawn, 0.9f * ring});
}

Vec2 arenaStagePlayerStart(float ringRadius) { return Vec2{-0.35f * ringRadius, 0.f}; }

ArenaRing arenaStageRing(float ringRadius) {
    return arenaRingAt(arenaStagePlayerStart(ringRadius), Vec2{1.f, 0.f}, ringRadius, ArenaRing{Vec2{0.f, 0.f}, ringRadius});
}

// ---- generator ----

bool generateArenaStage(const ArenaStageSpec& spec, const ArenaDef& def, ArenaStageLayout& out, std::string* error) {
    out = ArenaStageLayout{};
    out.id = spec.id;
    out.world = spec.world;
    out.theme = spec.theme;
    out.floor = spec.floor;
    out.bound = spec.bound;
    out.sizing = arenaStageSizing(def);
    const float R = def.ringRadius;
    const float s = out.sizing.propScale;
    const float k = out.sizing.k;
    out.ringRadius = R;
    out.spawnDistance = def.spawnDistance;
    Rng rng(spec.seed * 2654435761u + 17u);

    const WorldLook w = worldLook(spec.world);
    out.roles = w.roles;
    out.look = w.look;
    out.sunYaw = w.sunYaw;
    out.sunPitch = w.sunPitch;
    out.fogR = w.fogR;
    out.fogG = w.fogG;
    out.fogB = w.fogB;
    // Fog thins as the stage grows, so a big boss stays in view; a stage's own share scales it.
    out.look.fogDensity = w.look.fogDensity * spec.fog / (0.5f + 0.5f * s);
    if (spec.bound == StageBound::FogWall) out.look.fogDensity *= 1.6f;

    const Starts starts = startsOf(def);
    auto& pieces = out.pieces;
    // Ground beyond the bound only for a slab on the earth; an island or a broken ring has the void or the water beyond.
    const bool groundBeyond = (spec.bound == StageBound::Wall || spec.bound == StageBound::FogWall) &&
                              spec.floor != StageFloor::Island && spec.floor != StageFloor::BrokenRing;
    const float rim = 2.4f * s;
    const float floorR = R + rim;
    out.floorRadius = floorR;
    const float boundR = spec.bound == StageBound::Edge ? floorR - 0.25f * s : R + 0.7f * s;
    out.boundRadius = boundR;
    // Pillars and cover stand in this band beyond the ring: on the ground beyond a wall, else on the rim of the floor.
    const float bandR = groundBeyond ? R + 3.f * s : R + 1.7f * s;

    // ---- the floor ----
    const StageRole floorRole = StageRole::Floor;
    switch (spec.floor) {
    case StageFloor::Circle:
        pieces.push_back(cylinder(StageGroup::Floor, floorRole, Vec2{}, -0.5f, 2.f * floorR, 0.5f));
        break;
    case StageFloor::Square:
        pieces.push_back(box(StageGroup::Floor, floorRole, Vec2{}, -0.5f, 2.f * floorR, 2.f * floorR, 0.5f, 0.f));
        break;
    case StageFloor::BrokenRing: {
        const float midR = floorR - 0.3f * s;
        pieces.push_back(cylinder(StageGroup::Floor, floorRole, Vec2{}, -0.5f, 2.f * midR, 0.5f));
        // Slabs sinking away around the ring, with gaps between them; a few adrift further out.
        const int slabs = std::clamp(static_cast<int>(2.f * kPi * (midR + 2.f * s) / (4.f * s)), 10, 44);
        for (int i = 0; i < slabs; ++i) {
            if (rng.uni() < 0.22f) continue;
            const float angle = (static_cast<float>(i) + rng.range(0.f, 0.4f)) / static_cast<float>(slabs) * 2.f * kPi;
            const float depth = rng.range(1.6f, 3.4f) * s;
            const Frame f = frameAt(midR + depth * 0.5f + rng.range(0.f, 0.6f) * s, angle);
            const float drop = rng.range(0.f, 1.3f) * s;
            const float width = rng.range(2.4f, 4.f) * s;
            const float lean = rng.range(-9.f, 9.f);
            pieces.push_back(box(StageGroup::Fragment, floorRole, f.c, -0.5f - drop, depth, width, 0.5f, f.yaw(), lean));
        }
        const int adrift = 4 + static_cast<int>(rng.next() % 4u);
        for (int i = 0; i < adrift; ++i) {
            const float far = floorR + rng.range(3.f, 9.f) * s;
            const float angle = rng.range(0.f, 2.f * kPi);
            const Frame f = frameAt(far, angle);
            const float z = rng.range(-3.f, -0.5f) * s;
            const float a = rng.range(1.5f, 3.5f) * s;
            const float b = rng.range(1.5f, 3.5f) * s;
            const float turn = rng.range(0.f, 360.f);
            const float lean = rng.range(-14.f, 14.f);
            pieces.push_back(box(StageGroup::Fragment, floorRole, f.c, z, a, b, 0.6f * s, turn, lean));
        }
        break;
    }
    case StageFloor::Island:
        pieces.push_back(cylinder(StageGroup::Floor, floorRole, Vec2{}, -0.6f * s, 2.f * floorR, 0.6f * s));
        pieces.push_back(cylinder(StageGroup::Floor, StageRole::Structure, Vec2{}, -3.6f * s, 1.5f * floorR, 3.f * s));
        pieces.push_back(cylinder(StageGroup::Floor, StageRole::Dark, Vec2{}, -7.6f * s, 0.9f * floorR, 4.f * s));
        break;
    }
    if (groundBeyond) {
        pieces.push_back(cylinder(StageGroup::Floor, StageRole::Ground, Vec2{}, -0.62f, 2.f * (R + 80.f * s), 0.1f));
    }

    // ---- the ring's painted line and the centre mark ----
    {
        const int marks = std::clamp(static_cast<int>(2.f * kPi * R / 1.2f), 32, 160);
        const float step = 2.f * kPi / static_cast<float>(marks);
        const float chord = 2.f * R * std::sin(step * 0.5f) + 0.1f;
        for (int i = 0; i < marks; ++i) {
            const Frame f = frameAt(R, step * static_cast<float>(i));
            pieces.push_back(box(StageGroup::Mark, StageRole::Line, f.c, 0.f, 0.14f * std::sqrt(s), chord, 0.01f, f.yaw()));
        }
        pieces.push_back(cylinder(StageGroup::Mark, StageRole::Line, Vec2{}, 0.f, 0.6f * std::sqrt(s), 0.01f));
    }

    // ---- the bound ----
    {
        float height = 0.f, depth = 0.f;
        StageRole role = StageRole::Structure;
        float z = 0.f;
        switch (spec.bound) {
        case StageBound::Wall: height = 0.9f * s; depth = 0.4f * s; break;
        case StageBound::Edge: height = 0.18f * s; depth = 0.3f * s; break;
        case StageBound::Water: height = 0.25f * s; depth = 0.4f * s; break;
        case StageBound::FogWall:
            height = 16.f * s;
            depth = 1.f * s;
            role = StageRole::Dark;
            break;
        }
        float ringAt = boundR;
        if (spec.bound == StageBound::FogWall) ringAt = R + 9.f * s;
        out.wallHeight = height;
        const int segs = std::clamp(static_cast<int>(2.f * kPi * ringAt / (2.5f * s)), 24, 120);
        const float step = 2.f * kPi / static_cast<float>(segs);
        const float chord = 2.f * ringAt * std::sin(step * 0.5f) + 0.06f * s;
        for (int i = 0; i < segs; ++i) {
            const Frame f = frameAt(ringAt, step * static_cast<float>(i));
            pieces.push_back(box(StageGroup::Rim, role, f.c, z, depth, chord, height, f.yaw()));
        }
        if (spec.bound == StageBound::FogWall) out.boundRadius = ringAt;
    }

    // ---- water ----
    if (spec.bound == StageBound::Water || spec.floor == StageFloor::Island) {
        out.waterLevel = -0.4f * s;
        const float reach = 2.f * (floorR + 70.f * s);
        pieces.push_back(cylinder(StageGroup::Floor, StageRole::Water, Vec2{}, out.waterLevel - 0.3f, reach, 0.3f));
    }

    // ---- pillars ----
    const bool roundPillars = spec.theme != StageTheme::Plain && spec.theme != StageTheme::Street;
    const float pillarW = 0.8f * s;
    const float pillarH = 4.5f * s;
    std::vector<float> pillarAngles;
    {
        const int fit = std::max(1, static_cast<int>(2.f * kPi * bandR / (3.5f * pillarW)));
        const int n = std::min(spec.pillars, fit);
        for (int i = 0; i < n; ++i) {
            const float angle = 2.f * kPi * (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
            pillarAngles.push_back(angle);
            const Frame f = frameAt(bandR, angle);
            if (roundPillars) {
                pieces.push_back(cylinder(StageGroup::Pillar, StageRole::Structure, f.c, 0.f, pillarW * 1.15f, pillarH));
            } else {
                pieces.push_back(box(StageGroup::Pillar, StageRole::Structure, f.c, 0.f, pillarW, pillarW, pillarH, f.yaw()));
            }
            if (spec.theme != StageTheme::Plain) {
                pieces.push_back(cylinder(StageGroup::Pillar, StageRole::Accent, f.c, pillarH, pillarW * 0.9f, 0.18f * s));
            }
        }
    }

    // ---- cover: between the pillars where it fits, then anywhere in the band ----
    {
        std::vector<float> taken = pillarAngles;
        const float coverGap = 3.f * s / bandR; // radians
        for (int i = 0; i < spec.cover; ++i) {
            for (int attempt = 0; attempt < 24; ++attempt) {
                const float angle = rng.range(0.f, 2.f * kPi);
                bool clear = true;
                for (float t : taken) {
                    float d = std::fabs(angle - t);
                    d = std::min(d, 2.f * kPi - d);
                    if (d < coverGap) clear = false;
                }
                if (!clear) continue;
                taken.push_back(angle);
                const Frame f = frameAt(bandR + rng.range(-0.25f, 0.25f) * s, angle);
                addCover(out, rng, spec.theme, f, s);
                break;
            }
        }
    }

    // ---- landmarks beyond the bound ----
    {
        const float firstAngle = rng.range(0.f, 2.f * kPi);
        for (int i = 0; i < spec.landmarks; ++i) {
            const float angle = firstAngle + 2.f * kPi * (static_cast<float>(i) + rng.range(-0.15f, 0.15f)) / static_cast<float>(spec.landmarks);
            const float far = (groundBeyond ? R + 10.f * s : floorR + 5.f * s) + rng.range(0.f, 3.f) * s;
            addLandmark(out, rng, spec.theme, frameAt(far, angle), s);
        }
    }

    // ---- decals on the floor: pools, cracks, debris ----
    const auto place = [&](float footprint, float rMin, float rMax, Vec2& at) {
        for (int attempt = 0; attempt < 60; ++attempt) {
            const float angle = rng.range(0.f, 2.f * kPi);
            const float r = rng.range(rMin, rMax) * R;
            const Vec2 p{r * std::cos(angle), r * std::sin(angle)};
            if (lengthOf(p) + footprint > R - 0.4f) continue;
            if (decalClearance(starts, p, footprint) < 0.2f) continue;
            at = p;
            return true;
        }
        return false;
    };
    for (int i = 0; i < spec.pools; ++i) {
        const float dia = rng.range(2.4f, 4.6f) * std::sqrt(s);
        Vec2 at;
        if (place(dia * 0.5f, 0.3f, 0.85f, at)) pieces.push_back(cylinder(StageGroup::Pool, StageRole::Water, at, 0.f, dia, 0.04f));
    }
    for (int i = 0; i < spec.cracks; ++i) {
        const float len = rng.range(2.5f, 6.f) * std::sqrt(s);
        Vec2 at;
        if (place(len * 0.5f, 0.25f, 0.9f, at)) {
            pieces.push_back(box(StageGroup::Crack, StageRole::Accent, at, 0.f, len, 0.14f * std::sqrt(s), 0.03f, rng.range(0.f, 180.f)));
        }
    }
    {
        const int debris = spec.cover * 2 + (spec.pools + spec.cracks);
        for (int i = 0; i < debris; ++i) {
            const float a = rng.range(0.6f, 1.8f) * std::sqrt(s), b = rng.range(0.5f, 1.4f) * std::sqrt(s);
            Vec2 at;
            if (place(0.5f * std::max(a, b) * 1.42f, 0.2f, 0.92f, at)) {
                pieces.push_back(box(StageGroup::Debris, StageRole::Structure, at, 0.f, a, b, 0.07f, rng.range(0.f, 180.f)));
            }
        }
    }

    // ---- lights: at the pillars when there are some, else evenly round the band ----
    {
        const int n = spec.lights;
        for (int i = 0; i < n; ++i) {
            float angle = 2.f * kPi * (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
            if (!pillarAngles.empty()) angle = pillarAngles[static_cast<std::size_t>(i) % pillarAngles.size()];
            const float radius = pillarAngles.empty() ? bandR : bandR * 0.92f;
            const Frame f = frameAt(radius, angle);
            StageLight l;
            l.at = f.c;
            l.z = (pillarAngles.empty() ? 4.5f : 5.f) * s * w.lightHeight;
            const float* c = (i % 2 == 0) ? w.lightA : w.lightB;
            l.r = c[0];
            l.g = c[1];
            l.b = c[2];
            l.intensity = spec.lightIntensity * w.lightScale * s * s;
            l.radius = 18.f * s;
            out.lights.push_back(l);
        }
    }

    // ---- camera ----
    {
        const float arm = clampf(0.35f + 0.65f * k, 1.f, 4.2f);
        out.camera.arm = 6.4f * arm;
        out.camera.pivot = 1.5f * (1.f + 0.5f * (arm - 1.f));
        out.camera.side = 1.2f * arm;
        out.camera.up = 0.7f * arm;
        out.camera.lockPitch = clampf(-20.f + 4.f * (arm - 1.f), -20.f, -8.f);
    }

    const bool watery = spec.world == StylePack::Solhaven || spec.world == StylePack::Lattice || spec.world == StylePack::Unwritten;
    return checkArenaStage(out, def, error, watery);
}

// ---- checks ----

bool checkArenaStage(const ArenaStageLayout& layout, const ArenaDef& def, std::string* error, bool needWater) {
    std::string problems;
    const auto fail = [&](const std::string& what) {
        if (!problems.empty()) problems += '\n';
        problems += (layout.id.empty() ? std::string("stage") : layout.id) + ": " + what;
    };
    const float R = def.ringRadius;
    if (std::fabs(layout.ringRadius - R) > 1e-4f) fail("ring is not the fight's");
    if (std::fabs(layout.spawnDistance - def.spawnDistance) > 1e-4f) fail("spawn is not the fight's");
    if (!(R > 0.f && R <= kMaxPinnedRing)) fail("ring out of range");

    const StageCamera& c = layout.camera;
    const bool cameraOk = std::isfinite(c.arm) && std::isfinite(c.pivot) && std::isfinite(c.side) && std::isfinite(c.up) &&
                          std::isfinite(c.lockPitch) && c.arm >= 3.f && c.arm <= 40.f && c.pivot >= 0.5f && c.pivot <= 10.f &&
                          c.lockPitch <= -5.f && c.lockPitch >= -40.f;
    if (!cameraOk) fail("camera rig out of range");

    const Starts starts = startsOf(def);
    for (std::size_t i = 0; i < starts.count; ++i) {
        const float apart = lengthOf(minus(starts.enemy[i], starts.player));
        if (apart < starts.radius[i] + 3.f) fail("the player's start is too close to enemy " + std::to_string(i));
        if (apart < 0.5f * def.spawnDistance) fail("the player's start is too close to enemy " + std::to_string(i));
    }

    std::size_t bad = 0;
    for (std::size_t n = 0; n < layout.pieces.size(); ++n) {
        const StagePiece& p = layout.pieces[n];
        const bool finite = std::isfinite(p.at.x) && std::isfinite(p.at.y) && std::isfinite(p.z) && std::isfinite(p.sx) &&
                            std::isfinite(p.sy) && std::isfinite(p.sz) && std::isfinite(p.yaw) && std::isfinite(p.pitch);
        if (!finite || p.sx <= 0.f || p.sy <= 0.f || p.sz <= 0.f || lengthOf(p.at) > 1500.f) {
            if (bad++ < 3) fail("piece " + std::to_string(n) + " has a bad number");
            continue;
        }
        if (isSolid(p.group) && footprintDistance(p, Vec2{}) < R + kRingClear) {
            if (bad++ < 3) fail("solid piece " + std::to_string(n) + " stands inside the ring");
        }
        if (isDecal(p.group) && pieceDecalClearance(starts, p) < 0.f) {
            if (bad++ < 3) fail("decal " + std::to_string(n) + " lies in a spawn disc or the lane between the starts");
        }
        if (isDecal(p.group) && lengthOf(p.at) > R) {
            if (bad++ < 3) fail("decal " + std::to_string(n) + " lies outside the ring");
        }
    }
    for (const StageLight& l : layout.lights) {
        if (!(std::isfinite(l.intensity) && l.intensity > 0.f && l.radius > 0.f && std::isfinite(l.z))) {
            fail("a light has a bad number");
            break;
        }
    }
    if (needWater && !layout.hasWater()) fail("no water anywhere");

    if (problems.empty()) return true;
    if (error) *error = problems;
    return false;
}

} // namespace ftcore
