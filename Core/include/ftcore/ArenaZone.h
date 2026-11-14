#pragma once

// Arena zones: where an arena fight is held. A zone is a portion of a world's own scene (a plaza, a street
// between buildings) that the player and the enemies are held inside by invisible walls. Nothing is built for it:
// the fight happens in the world map with its art and lighting, and the zone is only numbers.
//
//   ftcore::ArenaZoneTable zones;
//   if (!ftcore::importArenaZoneTable(text, arenaTable, worldTable, zones, &error)) ...;  // allocates: at load
//   ftcore::ArenaDef def = ...;                        // the fight's enemies and fighter
//   const ftcore::ArenaZoneSpec* spec = zones.find("solhaven_plaza");
//   ftcore::ArenaZoneFit fit;                          // allocates: when the zone is fitted, never per frame
//   if (!ftcore::fitArenaZone(*spec, layout, def, fit, &error, &warnings)) ...;
//   fit.apply(def);                                    // ring, spawn from the fitted zone
//   fight.begin(def, fit.ring, fit.playerStart, fit.playerFacing, seed);
//   ...each step: fight.tick(dt, input, bodies); ftcore::arenaStep(fight, bodies, dt, fit.zone);
//
// The walls. A zone is a circle or a convex polygon (a rounded rectangle, a polygon of the data's, or a circle
// that buildings have cut). arenaStep(fight, bodies, dt, zone) is arenaStep plus the zone: every body (the
// player, the boss and the minions) is held inside the shape, shrunk by its own body radius, and pushed out of
// every obstacle. Nothing in Unreal blocks them, and Core's fight rules never see the zone: it judges the same
// fight inside the zone's bounding circle (ArenaZoneFit::ring), and the host reports where the bodies stand.
// A body that walks against a wall slides along it; velocity isn't cut, so one pressing into a pillar stays put.
//
// Obstacles. Scene geometry inside the zone is real: a building that stands wholly inside is a box, the district's
// landmark and every tree and rock inside are circles. A building the zone's shape merely crosses is not an
// obstacle: it cuts the zone, by a half-plane along its nearest face (the zone's centre staying on the open
// side), so the walls never run through a building. Obstacles block both sides. The fit keeps every obstacle and
// every wall out of the spawn discs and the lane between the player's start and each enemy's.
//
// The fit. The zone starts as the shape at the size the fight needs (ArenaZoneSizing: the boss's size and the
// number of enemies and attackers, the rule of the old arena rings), is moved up to two steps off its anchor and
// shrunk up to three times (to 0.88, 0.77, 0.68) until what remains of it is room enough: at least half the area
// of its circle, an inscribed radius of the biggest body radius + 3 m, a size of at least 0.65 of what was
// asked, and wholly on the district's flat pad. Where none of that works the fit fails (a spot too crowded or
// too small for this boss); where it works at a smaller size it says so in `warnings`. All of it is
// deterministic.
//
// Everything is fixed-capacity once fitted; fitting allocates (strings, the table), arenaStep never does.

#include "ftcore/Api.h"
#include "ftcore/ArenaFight.h"
#include "ftcore/ArenaLook.h"
#include "ftcore/StylePack.h"
#include "ftcore/WorldLayout.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

inline constexpr std::size_t kMaxZoneVerts = 64;
inline constexpr std::size_t kMaxZoneObstacles = 48;

// A thing in the zone that blocks movement: a circle (a tree, a rock, the landmark) or an oriented box (a building).
struct ZoneObstacle {
    bool box = false;
    Vec2 at;
    float radius = 0.f; // a circle's
    Vec2 half;          // a box's half sizes along its own axes
    float yaw = 0.f;    // a box's, radians (the world's WorldBlock::yaw)
    bool authored = false; // from the zone's own `cover` list: the host dresses it (a world's buildings are already there)
};

enum class ZoneShape : std::uint8_t {
    Circle,  // centre, radius: exact
    Polygon, // convex, counter-clockwise
};

class ArenaZone {
public:
    ZoneShape shape = ZoneShape::Circle;
    Vec2 centre;          // the circle's centre; the polygon's centroid
    float radius = 12.f;  // the circle's radius; the polygon's bounding radius about `centre`
    std::array<Vec2, kMaxZoneVerts> verts{};
    std::uint8_t vertCount = 0;
    std::array<ZoneObstacle, kMaxZoneObstacles> obstacles{};
    std::uint8_t obstacleCount = 0;
    // A zone in a world brakes a body that nears a wall: arenaStep slows it to 60% over the last 1.5 m (the plain
    // zone does not: it is the old ring).
    bool softWall = false;

    // Whether a disc of `margin` around p is inside the zone's shape (obstacles ignored). No allocation.
    FTCORE_API bool contains(Vec2 p, float margin = 0.f) const;
    // p pulled inside the shape shrunk by `margin` (the zone's centre when the shape is thinner than that). No
    // allocation.
    FTCORE_API Vec2 clamp(Vec2 p, float margin = 0.f) const;
    // The distance from p to the nearest obstacle's surface, negative inside one (a large number with none).
    FTCORE_API float obstacleDistance(Vec2 p) const;
    // p pushed out of every obstacle to `margin` from its surface.
    FTCORE_API Vec2 pushOut(Vec2 p, float margin) const;
    // clamp and pushOut together until neither moves it (a few passes): where a body of `margin` can stand.
    FTCORE_API Vec2 constrain(Vec2 p, float margin) const;
    // The distance from p to the nearest wall: positive inside the shape, negative outside it. No allocation.
    FTCORE_API float wallDistance(Vec2 p) const;
    // Polygon area (a circle's, for a circle).
    FTCORE_API float area() const;
    // The radius of the largest circle about `centre` that fits inside the shape.
    FTCORE_API float inradius() const;
};

// How a zone's shape is asked for.
enum class ZoneKind : std::uint8_t {
    Circle,
    Rect,    // rounded rectangle: `aspect` long over short, corners `corner` of the short half
    Polygon, // `points`, in multiples of the ring radius about the anchor, convex
};

// A zone in the file.
struct ArenaZoneSpec {
    std::string id;
    std::string world;                 // a worlds.json id; empty: no world, a plain zone about the origin
    std::uint32_t worldIndex = 0;      // into the WorldTable
    int district = -1;                 // the anchor's district (from `scene` or `district`); -1: `at`
    std::string scene;                 // the scene it was named by, when it was
    Vec2 at;                           // a world position (metres), when no district
    Vec2 offset;                       // metres from the anchor, in its frame (a district's: origin its plaza centre, +x the way its spawn faces)
    float yaw = 0.f;                   // degrees, the shape's turn in the anchor's frame
    ZoneKind kind = ZoneKind::Circle;
    float aspect = 1.6f;
    float corner = 0.3f;
    std::vector<Vec2> points;
    // Scenery and cover the zone names itself, in the anchor's frame. `water`: shallow sheets, drawn and never
    // blocking (the plazas are dry paving; a world needs some water). `cover`: things that block both sides.
    struct Water {
        Vec2 at;
        float radius = 3.f;
    };
    struct Cover {
        bool box = false;
        Vec2 at;
        float radius = 1.f; // a circle's
        Vec2 half{1.f, 1.f}; // a box's half sizes
        float yaw = 0.f;     // a box's, degrees
    };
    std::vector<Water> water;
    std::vector<Cover> cover;
    std::vector<std::string> bosses;   // the enemy kinds it hosts: the default line-up
    float ring = 0.f;                  // 0: from the enemies' size; else the size asked for (6..80 m)
    float spawn = 0.f;                 // 0: from the ring
    float frameTurnCapDeg = -1.f;      // below 0: the global lock framing cap; else this zone's (0..the global), degrees
    int attackers = 0;                 // 0: leave the fight's own
    std::uint32_t seed = 1;
};

struct ArenaZoneTable {
    std::vector<ArenaZoneSpec> zones;
    std::string defaultId; // the zone a host uses without a choice

    // No allocation. Null when no zone has this id.
    FTCORE_API const ArenaZoneSpec* find(std::string_view id) const;
    FTCORE_API const ArenaZoneSpec* defaultZone() const;
};

// Reads arenas.json. Boss ids must be enemy kinds of `arena`, a zone needs 1..6 bosses, ids are unique, a world
// names a worlds.json world, a scene one of its scenes, a district one of its districts; `default` names a zone.
// Unknown keys are warnings; anything else wrong is an error. On failure `out` is empty and *error lists each
// problem with its path, one per line.
//   { "version": 1, "default": "sand_ring",
//     "zones": [ { "id": "sand_ring", "bosses": ["brute", "skirmisher"], "ring": 12, "spawn": 7, "attackers": 1 },
//                { "id": "solhaven_square", "world": "solhaven", "scene": "SC-03-D1", "offset": [30, 0], "yaw": 20,
//                  "shape": { "kind": "rect", "aspect": 1.6, "corner": 0.3 }, "bosses": ["duelist"], "seed": 3 } ] }
FTCORE_API bool importArenaZoneTable(std::string_view json, const ArenaTable& arena, const WorldTable& worlds,
                                     ArenaZoneTable& out, std::string* error = nullptr,
                                     std::vector<std::string>* warnings = nullptr);

// The numbers the size comes from.
struct ArenaZoneSizing {
    float bodyRadius = 0.5f; // the biggest enemy's (m)
    float height = 2.2f;     // the tallest enemy's (m)
    int enemies = 1;
    int attackers = 1;
    float k = 1.f;           // the boss scale, 1..6: the larger of bodyRadius / 0.5 and height / 2.2
};

// The camera rig of the player's figure (metres and degrees): arm 6.4, pivot 1.5, side 1.2, up 0.7, FOV 70, lock
// pitch -20, the same for every fight. A big boss does not pull the camera out: the player locks on or looks up.
struct ZoneCamera {
    float arm = 6.4f;
    float pivot = 1.5f;
    float side = 1.2f;
    float up = 0.7f;
    float fov = 70.f;
    float lockPitch = -20.f;
    // The lock framing's most turn in this zone (degrees, the cap ftcore::arenaFrameTurn holds the orbit to): the
    // global one unless the zone asks less, as one whose lens would sit in a building turned that far does.
    float frameTurnCapDeg = kArenaFrame.maxTurn;
};

FTCORE_API ArenaZoneSizing arenaZoneSizing(const ArenaDef& def);
// The ring radius the fight asks for: (8.5 m + 1.5 per enemy past one + 0.75 per attacker past one) x (0.4 + 0.6 k),
// held to 7..60 m, unless the spec pins one.
FTCORE_API float arenaZoneAskedRing(const ArenaZoneSpec& spec, const ArenaDef& def);
FTCORE_API ZoneCamera arenaZoneCamera(const ArenaZoneSizing& sizing);

// The fitted zone: the walls, the obstacles, where everyone starts and the fight's ring.
// A shallow sheet of water on the floor, in world metres.
struct ZoneWater {
    Vec2 at;
    float radius = 3.f;
};

// Where the zone's wall crosses a road: the point on the wall, the way out of the zone and the road's width there.
struct ZoneMouth {
    Vec2 at;
    Vec2 out{1.f, 0.f};
    float width = 6.f;
};

inline constexpr std::size_t kMaxZoneWater = 8;
inline constexpr std::size_t kMaxZoneMouths = 8;

struct ArenaZoneFit {
    std::string id;
    ArenaZone zone;
    ArenaRing ring;            // the fight's: the zone's bounding circle
    Vec2 playerStart;
    Vec2 playerFacing{1.f, 0.f};
    float spawnDistance = 7.f;
    float askedRing = 12.f;    // the size asked for
    float scale = 1.f;         // the size got, over the size asked for (1: all of it)
    int district = -1;         // the district whose flat pad it stands on, -1 without a world
    ArenaZoneSizing sizing;
    ZoneCamera camera;
    std::array<ZoneWater, kMaxZoneWater> water{};
    std::uint8_t waterCount = 0;
    std::array<ZoneMouth, kMaxZoneMouths> mouths{};
    std::uint8_t mouthCount = 0;
    // The share of the zone's edge (0..1) where a player standing on it would have the camera (its arm behind
    // him, along the line from the zone's centre) inside a building taller than the camera: the camera
    // collision pulls it in there. 0 without a world.
    float cameraBlocked = 0.f;

    // Sets def's ringRadius and spawnDistance to the fitted ones.
    void apply(ArenaDef& def) const {
        def.ringRadius = ring.radius;
        def.spawnDistance = spawnDistance;
    }
};

// Fits `spec` to the world's real layout (`layout` null: a plain zone with nothing to clip or block) for the fight
// `def`. False, with the reason in *error, when no size and place near the anchor gives room enough; a shrink
// pushes a message to *warnings. `def` is not changed.
FTCORE_API bool fitArenaZone(const ArenaZoneSpec& spec, const WorldLayout* layout, const ArenaDef& def, ArenaZoneFit& out,
                             std::string* error = nullptr, std::vector<std::string>* warnings = nullptr);

// The rules the fight needs of a fitted zone, one problem per line: the ring is the bounding circle; the player and
// every enemy start inside the zone and clear of obstacles; the lanes between the starts are clear of obstacles and
// walls; the starts are apart; every vertex stands on the district's flat pad; the camera is in range.
FTCORE_API bool checkArenaZone(const ArenaZoneFit& fit, const ArenaDef& def, const WorldLayout* layout,
                               std::string* error = nullptr);

// arenaStep, then the zone: bodies held inside the walls and out of the obstacles. No allocation.
FTCORE_API void arenaStep(const ArenaFight& fight, ArenaBodies& bodies, float dt, const ArenaZone& zone);

} // namespace ftcore
