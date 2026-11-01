#pragma once

// Arena stages: the place a real-time arena fight (ArenaFight.h) is held in. A stage is data in
// Data/Battle/arenas.json (ids and numbers only): a world style, a theme, the enemy kinds it hosts, a floor
// shape, a bound and counts of features. A deterministic generator turns the stage and the fight it holds into
// a layout of plain primitives (boxes and cylinders), lights, a scene look and a camera rig, which the host
// builds. The fight itself never reads any of it: Core still knows only the ring.
//
//   ftcore::ArenaStageTable stages;
//   if (!ftcore::importArenaStageTable(text, arenaTable, stages, &error)) ...;  // allocates: at load
//   ftcore::ArenaDef def = ...;                       // the fight's enemies and fighter
//   const ftcore::ArenaStageSpec* spec = stages.find("hulk_canal");
//   ftcore::arenaStageFit(*spec, def);                // ring, spawn and attackers from the enemies' size
//   ftcore::ArenaStageLayout layout;
//   ftcore::generateArenaStage(*spec, def, layout);   // allocates: when the stage is built, never per frame
//
// Scale. The stage grows with its biggest enemy (ArenaStageSizing): k is the larger of body radius / 0.5 m and
// height / 2.2 m, held to 1..6, so a human or the shipped wave is k = 1 and a 7 m giant about 4.4. From k:
//   ring radius  (8.5 m + 1.5 m per enemy past one + 0.75 m per attacker past one) * (0.4 + 0.6 k), 7..60 m
//                (a stage may pin it with `ring`; the fit never leaves the range 3..60 m)
//   spawn        0.58 of the ring, at least the biggest body radius + 4 m, at most 12 m (the lock's range is 15)
//   props        propScale 1 + 0.55 (k - 1): every wall, pillar, cover and landmark size and gap is a multiple of
//                it, so a giant's pillars are as many times taller as it is
//   camera       arm 6.4 m * (0.35 + 0.65 k) at most 4.2x; pivot height, shoulder offset and the lock pitch follow
// A stage is held to the ring, never the other way: the layout puts everything solid outside the ring, so
// nothing blocks a body (the host moves bodies with arenaStep, which knows no walls), and only floor-level
// decals (pools, cracks, debris) lie inside it, kept out of the spawn discs and the corridor between them.
//
// Deterministic for the same spec, fight and seed. Loading and generating allocate; nothing here runs per frame.

#include "ftcore/Api.h"
#include "ftcore/ArenaFight.h"
#include "ftcore/SceneLook.h"
#include "ftcore/StylePack.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

enum class StageFloor : std::uint8_t {
    Circle,     // a round slab
    Square,     // a square slab with the ring inside it
    BrokenRing, // a disc under the ring, with broken slabs sinking away around it
    Island,     // a disc on a stalk over water
};
inline constexpr std::size_t kStageFloorCount = 4;

enum class StageBound : std::uint8_t {
    Wall,    // a knee-to-chest wall just outside the ring, ground beyond it
    Edge,    // the floor ends: a kerb, then a drop
    Water,   // a low kerb, then still water all round
    FogWall, // tall dark curtains far out in thick fog, ground beyond
};
inline constexpr std::size_t kStageBoundCount = 4;

// A theme picks the look of the cover, pillars and landmarks (the world picks the colours and the light).
enum class StageTheme : std::uint8_t {
    Plain,   // square pillars, no landmarks
    Street,  // crates, square pillars, stacked towers
    Canal,   // bollards, round pillars, arches
    Circuit, // plinths, round pillars, slim pylons
    Grid,    // plinths, round pillars, tilted monoliths
    Shore,   // leaning slabs, round pillars, broken columns
    Void,    // broken chunks, round pillars, leaning monoliths
};
inline constexpr std::size_t kStageThemeCount = 7;

enum class StageShape : std::uint8_t { Box, Cylinder };

// How a piece is painted (StageRoleLook).
enum class StageRole : std::uint8_t { Floor, Ground, Structure, Accent, Line, Dark, Water };
inline constexpr std::size_t kStageRoleCount = 7;

// What a piece is. Solid groups stand outside the ring; the flat ones lie on the floor and may be inside it.
enum class StageGroup : std::uint8_t {
    Floor,     // the slab under the ring, the ground beyond it, the water plane, the island's stalk (flat or below)
    Fragment,  // a broken slab beyond the ring
    Mark,      // the ring's painted line and the centre mark (flat)
    Rim,       // the wall, kerb or curtain at the bound
    Cover,     // beyond the ring
    Pillar,    // beyond the ring
    Landmark,  // the set pieces at the rim
    Debris,    // flat slabs on the floor
    Pool,      // flat water on the floor
    Crack,     // flat glowing cracks on the floor
};
inline constexpr std::size_t kStageGroupCount = 10;

// One primitive. A box is sx (along its yaw, radial for rim pieces), sy, sz metres; a cylinder is sx = sy =
// diameter and sz tall. `z` is the bottom (metres, the floor's top is 0), `at` the centre on the ground, `yaw` degrees
// about up (0 = +x) and `pitch` degrees of lean about the piece's own y axis (positive tips the top toward +x
// after the yaw).
struct StagePiece {
    StageGroup group = StageGroup::Floor;
    StageShape shape = StageShape::Box;
    StageRole role = StageRole::Structure;
    Vec2 at;
    float z = 0.f;
    float sx = 1.f, sy = 1.f, sz = 1.f;
    float yaw = 0.f;
    float pitch = 0.f;
};

struct StageLight {
    Vec2 at;
    float z = 3.f;         // height of the lamp (m)
    float r = 1.f, g = 1.f, b = 1.f; // linear colour
    float intensity = 1.f; // 1 = the old demo's lamp (the host maps it to its own unit)
    float radius = 18.f;   // reach (m)
};

// How a role is painted: base colour (linear), roughness, metallic and emissive strength (0: a plain surface).
struct StageRoleLook {
    float r = 0.5f, g = 0.5f, b = 0.5f;
    float roughness = 0.8f;
    float metallic = 0.f;
    float glow = 0.f;
};

// The camera rig of the player's figure, scaled with the stage (metres and degrees). The old fixed rig is
// arm 6.4, pivot 1.5, side 1.2, up 0.7, FOV 70, lock pitch -20.
struct StageCamera {
    float arm = 6.4f;
    float pivot = 1.5f;
    float side = 1.2f;
    float up = 0.7f;
    float fov = 70.f;
    float lockPitch = -20.f;
};

// The numbers the scale comes from.
struct ArenaStageSizing {
    float bodyRadius = 0.5f; // the biggest enemy's (m)
    float height = 2.2f;     // the tallest enemy's (m)
    int enemies = 1;
    int attackers = 1;
    float k = 1.f;           // the boss scale, 1..6
    float propScale = 1.f;   // 1 + 0.55 (k - 1)
};

struct ArenaStageLayout {
    std::string id;
    StylePack world = StylePack::None;
    StageTheme theme = StageTheme::Plain;
    StageFloor floor = StageFloor::Circle;
    StageBound bound = StageBound::Wall;

    ArenaStageSizing sizing;
    float ringRadius = 12.f;    // the fight's ring, centred on the origin (arenaStageRing)
    float spawnDistance = 7.f;
    float floorRadius = 14.f;   // where the floor under the ring ends
    float boundRadius = 12.7f;  // the bound's centre line
    float wallHeight = 0.9f;    // the bound's height (m)
    float waterLevel = 0.f;     // the water plane's top (m, below 0); only meaningful with hasWater

    std::vector<StagePiece> pieces;
    std::vector<StageLight> lights;
    std::array<StageRoleLook, kStageRoleCount> roles{};

    SceneLook look;             // sun, sky, fog and the camera finish
    float sunYaw = 35.f;        // degrees
    float sunPitch = -38.f;
    float fogR = 0.5f, fogG = 0.6f, fogB = 0.7f; // the fog's inscattering colour (linear)
    StageCamera camera;

    // Any water: a bound or island over it, a plane, or a pool on the floor.
    bool hasWater() const;
    std::size_t count(StageGroup group) const;
};

// A stage in the file.
struct ArenaStageSpec {
    std::string id;
    StylePack world = StylePack::None; // "none", "solhaven", "lattice", "unwritten" ("restored" looks like none)
    StageTheme theme = StageTheme::Plain;
    std::vector<std::string> bosses;   // the enemy kinds it hosts: the default line-up
    StageFloor floor = StageFloor::Circle;
    StageBound bound = StageBound::Wall;
    float ring = 0.f;                  // 0: from the enemies' size; else pinned
    float spawn = 0.f;                 // 0: from the ring
    int attackers = 0;                 // 0: leave the fight's own
    int cover = 0;                     // 0..16
    int pillars = 0;                   // 0..24
    int landmarks = 0;                 // 0..8
    int pools = 0;                     // 0..6
    int cracks = 0;                    // 0..12
    int lights = 0;                    // 0..24
    float lightIntensity = 1.f;        // (0, 10]
    float fog = 1.f;                   // [0, 4]: times the world's fog
    std::uint32_t seed = 1;
};

struct ArenaStageTable {
    std::vector<ArenaStageSpec> stages;
    std::string defaultId;             // the stage a host uses without a choice

    // No allocation. Null when no stage has this id.
    FTCORE_API const ArenaStageSpec* find(std::string_view id) const;
    // The default stage (null only when the table is empty).
    FTCORE_API const ArenaStageSpec* defaultStage() const;
};

// Names as arenas.json spells them ("circle", "broken_ring", ...). Static storage; never null.
FTCORE_API const char* stageFloorName(StageFloor floor);
FTCORE_API const char* stageBoundName(StageBound bound);
FTCORE_API const char* stageThemeName(StageTheme theme);

// Reads arenas.json. Every boss id must be an enemy kind of `arena`, a stage needs 1..6 bosses, ids are unique,
// `default` names a stage. Unknown keys are warnings; anything else wrong is an error. On failure `out` is
// empty and *error lists each problem with its path, one per line.
//   { "version": 1, "default": "sand_ring",
//     "stages": [ { "id": "sand_ring", "world": "none", "theme": "plain", "bosses": ["brute", "skirmisher"],
//                   "floor": "circle", "bound": "wall", "ring": 12, "spawn": 7, "attackers": 1, "seed": 1,
//                   "features": { "cover": 0, "pillars": 8, "landmarks": 0, "pools": 0, "cracks": 0,
//                                 "lights": 8, "lightIntensity": 1, "fog": 1 } } ] }
FTCORE_API bool importArenaStageTable(std::string_view json, const ArenaTable& arena, ArenaStageTable& out,
                                      std::string* error = nullptr, std::vector<std::string>* warnings = nullptr);

// The fight's scale numbers from its enemies (kinds in enemies[0..enemyCount)) and attackers.
FTCORE_API ArenaStageSizing arenaStageSizing(const ArenaDef& def);
// Sets def's ringRadius, spawnDistance and (when the spec names one) attackers for this stage and these enemies.
FTCORE_API void arenaStageFit(const ArenaStageSpec& spec, ArenaDef& def);
// Where the host starts the player for a ring of this radius: radius * 0.35 behind the origin, facing +x.
FTCORE_API Vec2 arenaStagePlayerStart(float ringRadius);
// The fight's ring: ringRadius around the origin (arenaRingAt with the player at arenaStagePlayerStart).
FTCORE_API ArenaRing arenaStageRing(float ringRadius);

// Builds the layout for `spec` around the fight `def` (its ring and spawn as set, normally by arenaStageFit). False,
// with the reason in *error, when the layout breaks a rule of checkArenaStage; `out` then holds what was built.
FTCORE_API bool generateArenaStage(const ArenaStageSpec& spec, const ArenaDef& def, ArenaStageLayout& out,
                                   std::string* error = nullptr);
// The rules the fight needs of a layout, one problem per line:
//   * ring and spawn are the fight's; the camera and every number are finite and in range
//   * every solid piece (rim, cover, pillar, landmark, fragment) stands outside the ring, so none can block a body
//   * flat decals (pool, crack, debris) keep out of every spawn disc and the corridor between the player's start
//     and the enemies'
//   * the player's start is apart from every enemy's
//   * the stage has water somewhere (unless `needWater` is false)
FTCORE_API bool checkArenaStage(const ArenaStageLayout& layout, const ArenaDef& def, std::string* error = nullptr,
                                bool needWater = true);

} // namespace ftcore
