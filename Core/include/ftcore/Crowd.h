#pragma once

// Civilians: a crowd that walks a city's streets and reacts to what happens near it (a fight, a shot, a death),
// noticing, backing off, fleeing along the streets to a safe distance, cowering when it can't get away, and calming
// down after a while. Deterministic (one seed, a fixed internal step) and allocation-free once initialised.
//
//   std::vector<ftcore::CrowdStreet> streets;           // load time: allocates
//   ftcore::importCrowdStreets(csvText, side, streets, &error);
//   ftcore::CrowdTuning tuning;
//   ftcore::importCrowdTuning(jsonText, tuning, &error); // Data/Crowd/crowd.json
//   ftcore::CrowdGraph graph;
//   graph.build(streets, tuning.graph, &error);
//   ftcore::CrowdSim sim;
//   sim.init(graph, tuning, seed);                       // allocates every pool, once
//   each frame:
//     sim.pushEvent({ftcore::CrowdEventKind::Strike, at});
//     sim.tick(dt, playerPos, playerSpeed);              // no allocation
//     for (i < sim.capacity()) if (sim.agent(i).live) ... draw sim.agent(i)
//
// Frame: the engine's, in metres (massingEnginePoint: X east, Y south, centred on the square), the frame an
// ArenaEvent's `at` is in. Yaw: degrees from +X towards +Y, as Unreal's.
//
// The walk graph is the street table's polylines: each point a node, points within `snap` merged, a street's end
// that stops on another street joined to it, and crossing streets joined where they cross. Only the kinds listed in
// CrowdGraphTuning::walkKinds are walked. A crowd member walks the pavement (or, on a street without one, anywhere
// inside its width), so a member fleeing along the graph never runs into a wall or the sea.

#include "ftcore/Api.h"
#include "ftcore/Explore.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

// ---- Streets and the walk graph (load time) ----

struct CrowdStreet {
    std::string kind;          // the table's kind column ("street", "stair", ...); empty without the column
    float width = 0.f;         // m
    float pavement = 0.f;      // m, each side; 0 when the table gives none
    std::vector<Vec2> points;  // engine frame, metres
};

// Reads a street table (as importMassingStreets: id, width_m, pts, plus the optional kind and pavement_m columns) and
// moves its points to the engine frame of a square `side` metres across. `out` is cleared first. Allocates.
FTCORE_API bool importCrowdStreets(std::string_view csv, double side, std::vector<CrowdStreet>& out,
                                   std::string* error = nullptr);

// A door of an enterable building (S-275): where it stands on the frontage and the middle of the room behind it.
struct CrowdDoor {
    std::string id;  // the table's id without its DOOR- prefix
    Vec2 at;         // engine frame, metres: the door's middle
    Vec2 inside;     // the room's middle (the door itself when the table has no room row for it)
};

// Reads a door table (id, kind, width_m, pts, pavement_m; DOOR-<id> rows of kind door with the door's two ends as
// pts x:y:z, ROOM-<id> rows with the room's outline) and moves the points to the engine frame of a square `side` metres
// across, as importCrowdStreets does. `out` is cleared first; false (with the line) on a malformed row or no doors.
// Allocates (load time).
FTCORE_API bool importCrowdDoors(std::string_view csv, double side, std::vector<CrowdDoor>& out, std::string* error = nullptr);

struct CrowdGraphTuning {
    float snap = 1.5f;                 // m: points closer than this are one node
    float junctionReach = 1.0f;        // m past the other street's half width an end may stop and still join it
    float edgeMargin = 0.6f;           // m kept clear of a street's edge when it has no pavement
    float wallInset = 0.8f;            // m kept clear of a street's outer edge (the frontage, building faces) always
    std::uint32_t minComponent = 12;   // nodes: smaller islands are never spawned on
    std::vector<std::string> walkKinds = {"street", "lane", "core_lane", "road", "terrace", "stair", "link_stair",
                                          "link_street", "link_road", "coast_road"};
    // A street with no kind (no kind column) is always walked.
};

struct CrowdEdge {
    std::uint32_t to = 0;
    float length = 0.f;
    float lane = 0.f;     // m from the centre line to the walking lane's middle (the pavement's), each side
    float laneHalf = 0.f; // m: how far either way of the lane a walker may stray
    float half = 0.f;     // m: the street's half width (its outer edge: the frontage, building faces)
};

class CrowdGraph {
public:
    // Builds from streets in the engine frame. False (with a reason) when no walkable edge results. Allocates.
    FTCORE_API bool build(const std::vector<CrowdStreet>& streets, const CrowdGraphTuning& tuning,
                          std::string* error = nullptr);

    std::uint32_t nodeCount() const { return static_cast<std::uint32_t>(pos_.size()); }
    std::uint32_t edgeCount() const { return static_cast<std::uint32_t>(edges_.size()); } // directed
    Vec2 node(std::uint32_t n) const { return pos_[n]; }
    std::uint32_t degree(std::uint32_t n) const { return first_[n + 1] - first_[n]; }
    const CrowdEdge& edge(std::uint32_t n, std::uint32_t k) const { return edges_[first_[n] + k]; }
    // The edge from a to b, or null.
    FTCORE_API const CrowdEdge* find(std::uint32_t a, std::uint32_t b) const;
    bool spawnable(std::uint32_t n) const { return spawnable_[n] != 0; }
    std::uint32_t spawnableCount() const { return spawnableCount_; }
    std::uint32_t componentCount() const { return components_; }
    // The nearest node within `within` m, or UINT32_MAX. No allocation.
    FTCORE_API std::uint32_t nearest(Vec2 p, float within) const;
    Vec2 boundsMin() const { return min_; }
    Vec2 boundsMax() const { return max_; }

private:
    std::vector<Vec2> pos_;
    std::vector<std::uint32_t> first_; // CSR: node n's edges are edges_[first_[n] .. first_[n+1])
    std::vector<CrowdEdge> edges_;
    std::vector<std::uint8_t> spawnable_;
    std::uint32_t spawnableCount_ = 0;
    std::uint32_t components_ = 0;
    // node grid for nearest()
    Vec2 min_{}, max_{};
    float cell_ = 20.f;
    std::uint32_t gw_ = 0, gh_ = 0;
    std::vector<std::uint32_t> cellFirst_;
    std::vector<std::uint32_t> cellNodes_;
};

// ---- Events ----

enum class CrowdEventKind : std::uint8_t { // append only; the tuning's events section uses the names below
    FightStart,   // "fight_start": a fight began here
    Strike,       // "strike": a blow landed
    Gunshot,      // "gunshot"
    MagicBurst,   // "magic_burst"
    Death,        // "death": someone fell (seen, not heard)
    LoudNoise,    // "loud_noise"
    PlayerSprint, // "player_sprint": the player ran past (seen or felt, close range)
    Panic,        // internal: a fleeing neighbour's panic (pushEvent ignores it)
    Count
};
FTCORE_API std::string_view crowdEventName(CrowdEventKind kind);

struct CrowdEventRule {
    float radius = 30.f;    // m: perceived within this
    float intensity = 0.5f; // fear added at the source; falls off linearly to 0 at the radius
    bool sightOnly = false; // perceived only when the agent faces it (or it is within feelRadius)
};

struct CrowdEvent {
    CrowdEventKind kind = CrowdEventKind::Strike;
    Vec2 at;
    float radiusScale = 1.f;    // scales the kind's radius
    float intensityScale = 1.f; // scales the kind's intensity
};

// ---- Tuning (Data/Crowd/crowd.json) ----

struct CrowdTuning {
    // population
    std::uint32_t maxAgents = 512;   // the pool (fixed at init)
    std::uint32_t targetAgents = 300;
    std::uint32_t maxDrawn = 48;     // the nearest this many are flagged drawn
    float spawnMin = 40.f;           // m from the player
    float spawnMax = 150.f;
    float despawnDist = 180.f;
    float fillMin = 8.f;             // m: while under half the target (the first ticks), members appear this close
    std::uint32_t spawnPerStep = 8;
    // movement
    float walkSpeed = 1.3f;          // m/s
    float walkSpeedJitter = 0.25f;   // +- m/s per member
    float runSpeed = 4.2f;
    float backSpeed = 0.7f;
    float turnRate = 360.f;          // deg/s
    float laneSpeed = 0.8f;          // m/s sideways inside the lane
    float idleChance = 0.15f;        // at each node reached on a calm walk
    float idleMin = 2.f, idleMax = 7.f;
    float headingKeep = 0.8f;        // weight of the member's own heading when choosing a turn (0 = random)
    float headingChange = 0.05f;     // chance per node of a new heading
    float avoidRadius = 0.9f;        // m between members
    float avoidPlayer = 1.6f;        // m kept from the player
    float minGap = 0.65f;            // m: two members never stand closer (pushed apart every step, S-271)
    // doors (S-275): calm walkers go in and come out again; a runner takes a door ahead of it, away from the threat
    float doorAttach = 12.f;         // m: a door this close to a street's centre line opens onto it
    float doorEnterChance = 0.08f;   // a calm walker passing a door goes in
    float doorEnterOver = 0.6f;      // ... while more are out on the streets than the density asks for
    float doorDwellMin = 20.f;       // s inside (calm visit), at least
    float doorDwellMax = 90.f;       // s, at most
    float doorFleeAhead = 8.f;       // m: a runner takes a door at most this far ahead along its street
    float doorFleeMin = 10.f;        // m: only a door at least this far from the threat
    std::uint32_t doorFleeCap = 6;
    // S-332: at the safe distance most runners walk on, away along their street and out of the area; a few stop and
    // look back, each with room of its own (none within watchSpace, at most watchCap within watchRoom).
    float watchChance = 0.3f;
    float watchSpace = 1.6f;         // m
    float watchRoom = 4.f;           // m
    std::uint32_t watchCap = 2;
    // Cornered members at a dead end's end: spots this far apart across and along (staggered), not a row.
    float braceSpace = 3.1f;         // m
    float braceRow = 3.2f;           // m   // runners one door takes in
    // perception
    float fov = 140.f;               // deg, for sight-only events
    float feelRadius = 3.f;          // m: sight-only events this close are felt whatever the facing
    float reactBase = 0.2f;          // s
    float reactPerMetre = 0.01f;     // s per m
    float reactJitter = 0.35f;       // s, up to
    float occludedHearing = 0.4f;    // intensity kept through an occluder (sight is lost)
    // Close: within this of a fight-class event (fight start, strike, gunshot, magic burst, death) a member feels it
    // whatever the occlusion or facing, takes it within reactCloseMax, and runs whatever its boldness.
    float reactRadius = 22.f;        // m
    float reactCloseMax = 0.05f;     // s
    std::uint32_t occlusionBudget = 24; // host occlusion queries per step
    std::array<CrowdEventRule, static_cast<std::size_t>(CrowdEventKind::Count)> events = {{
        {60.f, 1.2f, false},  // fight_start
        {30.f, 0.45f, false}, // strike
        {100.f, 1.4f, false}, // gunshot
        {50.f, 0.80f, false}, // magic_burst
        {30.f, 0.90f, true},  // death
        {40.f, 0.35f, false}, // loud_noise
        {4.f, 0.25f, true},   // player_sprint
        {0.f, 0.f, false},    // panic (spread section)
    }};
    // reaction (fear thresholds for a member of boldness 0.5; boldness scales them)
    float noticeFear = 0.12f;
    float backFear = 0.3f;
    float fleeFear = 0.55f;
    float fearMax = 1.5f;
    float fearDecay = 0.06f;         // per s
    float cowerDecay = 0.03f;        // per s while cowering
    float calmFear = 0.08f;          // below this a member can calm down
    float noticeTime = 1.5f;         // s a notice lasts at least
    float backTime = 2.0f;           // s a back-away lasts at least
    float calmTime = 4.f;            // s calm before resuming (after a flee or cower)
    float recoverTime = 2.5f;        // s looking around before walking on
    float threatMemory = 20.f;       // s a calm walk keeps away from the last threat
    float quietAfter = 3.f;          // s after the last stimulus: fear falls at quietDecay from then on
    float quietDecay = 0.3f;         // per s
    float returnChance = 0.6f;       // chance a calmed member walks back the way it ran (the street refills)
    float timidScale = 0.6f;         // threshold multiplier at boldness 0
    float boldScale = 1.6f;          // ... at boldness 1
    // flee
    float safeDist = 60.f;           // m from the threat a flee ends at
    float resumeHeading = 0.5f;      // chance a calmed member walks on the way it was going before (else a new way)
    float threatPenaltyRadius = 25.f;
    float threatPenalty = 12.f;      // cost multiplier for nodes at the threat, falling to 1 at the radius
    float blockRadius = 8.f;         // m: a flee never passes a node this close to the threat (cornered: cower)
    std::uint32_t searchLimit = 4000; // nodes a flee search may settle
    std::uint32_t plansPerStep = 32;
    float cowerRetry = 2.f;          // s between a cowering member's tries to flee
    // panic spread
    float spreadRadius = 6.f;
    float spreadGain = 0.6f;         // < 1: what a neighbour takes of the spreader's fear
    float spreadInterval = 0.5f;
    // step
    float step = 1.f / 15.f;
    std::uint32_t maxSteps = 4;      // per tick; the rest of a long frame is dropped
    CrowdGraphTuning graph;
};

// Reads Data/Crowd/crowd.json over `out`'s values (absent members keep theirs; unknown members are refused).
// Allocates (load time). On failure `out` is left as it was.
FTCORE_API bool importCrowdTuning(std::string_view json, CrowdTuning& out, std::string* error = nullptr);

// ---- The simulation ----

enum class CrowdState : std::uint8_t { Free, Walk, Idle, Notice, BackAway, Flee, Cower, Recover, Indoors }; // append only
enum class CrowdAnim : std::uint8_t { Idle, Walk, Run, BackStep, Cower, Look };
FTCORE_API std::string_view crowdStateName(CrowdState s);

struct CrowdAgentView {
    std::uint32_t id = 0;   // slot | generation << 16: changes when the slot is reused
    bool live = false;
    bool drawn = false;
    bool cornered = false;  // cowering at the far end of a dead end with no way out past the threat, facing away
    bool indoors = false;   // inside a building (state Indoors, past its door): not drawn, pos is the door
    CrowdState state = CrowdState::Free;
    CrowdAnim anim = CrowdAnim::Idle;
    Vec2 pos, prevPos;      // this step's and the last step's (lerp by stepAlpha())
    float odo = 0.f;        // m walked or run since the slot was first used (path length, not straight line)
    float yaw = 0.f;        // deg
    float speed = 0.f;      // m/s
    float fear = 0.f;
    float boldness = 0.5f;
};

// S-332: a camera the crowd must not pop in front of: from eye, yaw (deg), halfFov (deg; 180 or more: all round),
// out to range (m). Inside it nobody spawns, despawns, or is drawn or undrawn.
struct CrowdViewCone {
    Vec2 eye{0.f, 0.f};
    float yaw = 0.f;
    float halfFov = 35.f;
    float range = 40.f;
};

struct CrowdCounts {
    std::uint64_t spawned = 0, despawned = 0;
    std::uint64_t perceived = 0; // stimuli taken in (after their delay)
    std::uint64_t noticed = 0, backedAway = 0, fled = 0, cowered = 0, resumed = 0;
    std::uint64_t spread = 0;    // panic passed to a neighbour
    std::uint64_t plans = 0, planFails = 0;
    std::uint64_t eventsDropped = 0;
    std::uint64_t occlusionQueries = 0;
    std::uint64_t fleers = 0;            // members that fled at least once
    std::uint32_t maxFleeEntries = 0;    // the most times one member entered Flee
    std::uint32_t maxPlans = 0;          // the most flee plans one member asked for
    std::uint64_t stuck = 0;             // times a moving member stood still (< 0.1 m): a runner 1 s, anyone else 2 s
    std::uint64_t wentIn = 0, cameOut = 0, fledIn = 0; // through doors (fledIn: runners that took a door)
    std::uint64_t popsInView = 0;  // S-332: a member drawn or undrawn (spawned, despawned) inside a view's range
    std::uint64_t popsAppear = 0, popsLive = 0; // S-366: of them, drawn (vs undrawn); spawned or despawned (vs draw pick)
    std::uint64_t watched = 0, walkedOn = 0; // S-332: runners at the safe distance that stopped to look back / walked on
};

// A run check: which members stood within `radius` of a point when a fight started there, and how they moved by
// a later time (the host evaluates it 1 s in). Fixed size, no allocation.
inline constexpr std::size_t kCrowdProbeMax = 256;
struct CrowdReactionResult {
    std::uint32_t inside = 0;   // live members within the radius at the start (still live now)
    std::uint32_t away = 0;     // ... escaping: at least minPath along its route, facing its motion, minRadial farther
    std::uint32_t turned = 0;   // ... facing within 90 degrees of straight away from the point (reported, not required)
    std::uint32_t cornered = 0; // ... not escaping, but cornered: backed to a dead end's far end, cowering, turned away
    std::uint32_t toward = 0;   // ... closer to the point than at the start
    float worstGain = 0.f;      // m: the least growth in distance among them (negative: came closer)
    float worstPath = 0.f;      // m: the shortest route walked or run among them
    bool ok() const { return inside > 0 && away + cornered == inside && toward == 0; }
};
class CrowdSim;
class CrowdReactionProbe {
public:
    FTCORE_API void begin(const CrowdSim& sim, Vec2 at, float radius);
    // S-267: escaping = moved at least minPath along its route, facing its motion, and minRadial farther from the point.
    FTCORE_API CrowdReactionResult evaluate(const CrowdSim& sim, float minPath, float minRadial) const;
    std::uint32_t count() const { return n_; }

private:
    Vec2 at_{};
    std::array<std::uint32_t, kCrowdProbeMax> id_{};
    std::array<float, kCrowdProbeMax> d0_{};
    std::array<float, kCrowdProbeMax> odo0_{};
    std::array<Vec2, kCrowdProbeMax> p0_{};
    std::uint32_t n_ = 0;
};

// Host occlusion: true when something solid stands between `from` and `to` (engine frame, metres).
using CrowdOcclusionFn = bool (*)(void* context, Vec2 from, Vec2 to);

inline constexpr std::size_t kCrowdMaxEvents = 64;
inline constexpr std::size_t kCrowdPathMax = 24;

class CrowdSim {
public:
    // Sizes every pool. The graph must outlive the sim. False when the graph has no spawnable node. Allocates.
    FTCORE_API bool init(const CrowdGraph& graph, const CrowdTuning& tuning, std::uint32_t seed);
    void setOcclusion(CrowdOcclusionFn fn, void* context) { occlude_ = fn; occludeCtx_ = context; }

    // Queued for the next step; dropped (and counted) beyond kCrowdMaxEvents a step.
    FTCORE_API void pushEvent(const CrowdEvent& e);
    // Advances by `dt` in fixed steps. `playerSpeed` m/s (members step aside from a player coming at them).
    FTCORE_API void tick(float dt, Vec2 player, Vec2 playerVelocity);

    std::uint32_t capacity() const { return static_cast<std::uint32_t>(agents_.size()); }
    const CrowdAgentView& agent(std::uint32_t i) const { return views_[i]; }
    std::uint32_t liveCount() const { return live_; }
    FTCORE_API std::uint32_t countIn(CrowdState s) const;
    const CrowdCounts& counts() const { return counts_; }
    float stepAlpha() const { return acc_ / tuning_.step; }
    double time() const { return time_; }
    std::uint64_t steps() const { return steps_; }
    // A hash of every live member's state and position (determinism checks).
    FTCORE_API std::uint64_t digest() const;

    // Tests and tools: places a member at node `n` walking towards its neighbour `k`; its slot, or UINT32_MAX.
    // `along`: m along that edge (clamped to it).
    FTCORE_API std::uint32_t spawnAt(std::uint32_t n, std::uint32_t k, float boldness, float along = 0.f);
    // Tests: stops automatic spawning and despawning.
    void setPopulationFrozen(bool frozen) { frozen_ = frozen; }
    // Live members closer than `d` m to another (pairs counted once). O(n) on the neighbour grid; no allocation.
    FTCORE_API std::uint32_t closePairs(float d) const;
    // S-271 run checks. The least distance between two live members (`within` when none is closer). No allocation.
    FTCORE_API float minPairDistance(float within) const;
    // How far member i stands inside the street (the street rectangles round its edge and the edges at its nodes):
    // the distance to the nearest outer edge, the frontage. Negative: past it.
    FTCORE_API float wallClearance(std::uint32_t i) const;
    // Draw the members nearest this point instead of the player (a test watching a spot away from the player).
    void setDrawFocus(bool on, Vec2 at) { focusOn_ = on; focus_ = at; }

    // S-275 doors. Attaches each door to the nearest street within tuning.doorAttach of it (doors farther off are
    // left out) and returns how many it attached. Call after init; init forgets them. Allocates (load time).
    FTCORE_API std::uint32_t setDoors(const std::vector<CrowdDoor>& doors);
    std::uint32_t doorCount() const { return static_cast<std::uint32_t>(doors_.size()); }
    // S-283: the share of the daytime crowd out on the streets (0..1; the host reads it from the day's CrowdDensity
    // curve). The streets fill to targetAgents x density: above it walkers passing a door go in and those inside stay;
    // below it those inside come out sooner and new members arrive at the edges as before.
    // S-332: up to kMaxViews cameras (capture and player); n 0 clears them (then the drawn pick is by distance only).
    static constexpr std::uint32_t kMaxViews = 6;
    // S-366: a view that jumps (a new camera, a cut: its eye over 3 m or its yaw over 15 degrees from the last call) shows
    // a new picture for one step: members may be drawn in it then. In a steady view a member is never drawn or undrawn.
    FTCORE_API void setViews(const CrowdViewCone* views, std::uint32_t n);
    FTCORE_API bool inView(Vec2 p, float extraDeg = 0.f, float extraRange = 0.f) const;
    // S-366: in a view that did not just cut.
    FTCORE_API bool inSteadyView(Vec2 p, float extraDeg = 0.f, float extraRange = 0.f) const;
    // S-332: the most stopped members within r of one member (itself included); members behind doors left out.
    FTCORE_API std::uint32_t largestStoppedGroup(float r) const;
    // S-332: live members drawn in a run pose while moving slower than walking speed (gate 0).
    FTCORE_API std::uint32_t runPoseSlow() const;
    void setDensity(float d) { density_ = d > 1.f ? 1.f : d < 0.f ? 0.f : d; }
    float density() const { return density_; }
    // Members inside a building. They count in liveCount() (they hold a pool slot) but not towards the streets' target,
    // and are left out of the neighbour grid, the gap checks and the drawn pick.
    std::uint32_t indoorCount() const { return indoors_; }

private:
    struct Agent {
        std::uint16_t gen = 0;
        CrowdState state = CrowdState::Free;
        std::uint32_t from = 0, to = 0; // on the edge from -> to
        float along = 0.f, len = 1.f;
        float lane = 0.f, laneHalf = 0.f, side = 1.f; // side: +-1 along the edge's left normal
        float lat = 0.f, latTarget = 0.f;
        Vec2 pos, prev;
        float odo = 0.f;
        float yaw = 0.f, speed = 0.f, baseSpeed = 1.3f;
        float boldness = 0.5f, fear = 0.f, stateTime = 0.f, idleLeft = 0.f;
        float heading = 0.f; // deg: where a calm walk tends
        Vec2 threat;
        double threatAt = -1e9;
        float pendFear = 0.f;
        double pendAt = 0.0;
        Vec2 pendFrom;
        bool pend = false, pendSpread = false;
        bool needPlan = false;
        float spreadLeft = 0.f, retryLeft = 0.f;
        std::array<std::uint32_t, kCrowdPathMax> path{};
        std::uint8_t pathLen = 0, pathPos = 0;
        bool pathPartial = false;
        Vec2 planFrom;
        std::uint16_t fleeEntries = 0, plans = 0;
        std::uint8_t cowerFails = 0; // failed retries in a row: each waits longer
        float safeExtra = 0.f;       // m past the safe distance this member runs: fleers stop in different places
        bool pendClose = false;      // the pending stimulus is a close fight-class one (reactRadius)
        float stopAlong = -1.f;      // >= 0: a run that reached its goal stops this far along its last edge
        float idleAlong = -1.f;      // >= 0: a calm walk stops to idle this far along the edge
        float walkHeading = 0.f;     // deg: the calm heading before the last scare
        Vec2 stuckRef;               // where a running member stood when its stuck clock last restarted
        float stuckTime = 0.f;
        bool stuckTurned = false;    // blocked a second: already turned back or stopped
        bool hold = false;           // backing off or running with nowhere farther to go: stands until the next plan
        bool brace = false;          // cornered (no plan): runs to the far end of its dead end, then cowers there
        int braceSlot = -1;          // >= 0: its own spot at that end (rows back from the end, across the width)
        std::int32_t door = -1;      // Indoors: the door it uses
        bool walkedOn = false;       // S-332: ran to the safe distance and walked on
        std::uint8_t doorPhase = 0;  // Indoors: 1 walking in to the door, 2 inside, 3 coming out to its pavement
        bool doorFled = false;       // went in running from a threat
        float doorLeft = 0.f;        // s left inside
        Vec2 doorTo;                 // phase 3: the pavement spot it walks out to
        std::int32_t doorSeen = -1;  // the last door it walked past (one chance per door)
    };
    struct DoorSlot {
        Vec2 at, inside;
        std::uint32_t u = 0, v = 0;  // the street edge it opens onto
        float along = 0.f;           // m from u
        std::uint32_t fleeIn = 0;    // runners inside now
    };

    void step(Vec2 player, Vec2 playerVel);
    void spawnSome(Vec2 player);
    void despawnFar(Vec2 player);
    void perceive();
    void deliver(Agent& a);
    void decide(std::uint32_t i);
    void move(std::uint32_t i, Vec2 player, Vec2 playerVel);
    void spread(std::uint32_t i);
    void rebuildGrid(Vec2 player);
    void pickDrawn(Vec2 player);
    void writeView(std::uint32_t i);
    bool plan(Agent& a);
    void enter(Agent& a, std::uint32_t from, std::uint32_t to);
    void reverse(Agent& a);
    void startFlee(Agent& a);
    std::uint32_t awayFromThreat(const Agent& a) const;
    template <class Fn> void forEachNear(Vec2 p, float r, Fn&& fn) const;
    std::uint32_t chooseNext(Agent& a);
    void setState(Agent& a, CrowdState s);
    void free(std::uint32_t i);
    void unspawn(std::uint32_t i);
    void brace(Agent& a);
    int freeBraceSlot(std::uint32_t i) const;
    void braceSlotAt(const Agent& a, float& along, float& lat) const;
    bool unCorner(Agent& a);
    float doorAlong(const DoorSlot& d, const Agent& a) const;
    bool tryDoor(std::uint32_t i, bool fleeing);
    void goIn(Agent& a, std::uint32_t door, bool fled);
    void doorStep(Agent& a);
    std::uint32_t streetTarget() const;
    void arrive(Agent& a);
    bool roomToWatch(const Agent& a) const;
    // S-366: other members standing (not moving, not behind a door) within r of a.
    std::uint32_t standersNear(const Agent& a, float r) const;
    void settle(Agent& a);
    // Two members both at doors (going in or coming out) pass each other in the doorway: no gap kept or measured.
    bool bothAtDoors(std::uint32_t i, std::uint32_t j) const {
        return agents_[i].state == CrowdState::Indoors && agents_[j].state == CrowdState::Indoors;
    }
    bool crowded(std::uint32_t self, Vec2 p, float r) const;
    void separate();
    void nudge(Agent& a, Vec2 by);
    Vec2 pushable(const Agent& a, Vec2 by) const;
    void place(Agent& a, Vec2 p);
    float clearance(const Agent& a, Vec2 p, Vec2* fixTo) const;
    float scale(const Agent& a) const;
    Vec2 basePoint(const Agent& a) const;
    float rand01();

    const CrowdGraph* graph_ = nullptr;
    CrowdTuning tuning_;
    std::uint32_t rng_ = 1;
    std::vector<Agent> agents_;
    CrowdViewCone viewCones_[kMaxViews];
    std::uint32_t viewCount_ = 0;
    std::uint32_t cutMask_ = 0; // S-366: views that jumped since the last step
    std::vector<std::uint8_t> wasDrawn_;
    std::vector<CrowdAgentView> views_;
    std::vector<std::uint32_t> freeList_;
    std::uint32_t freeCount_ = 0, live_ = 0;
    std::array<CrowdEvent, kCrowdMaxEvents> events_{};
    std::size_t eventCount_ = 0;
    // neighbour grid around the player
    float gcell_ = 4.f;
    std::uint32_t gdim_ = 0;
    Vec2 gorigin_{};
    std::vector<std::uint32_t> gHead_, gNext_;
    // flee search scratch
    std::vector<float> sCost_;
    std::vector<std::uint32_t> sPrev_, sStamp_;
    std::vector<std::uint64_t> heap_; // cost bits << 32 | node
    std::uint32_t stamp_ = 0;
    std::uint32_t plansLeft_ = 0, occlusionLeft_ = 0;
    // this step's spawn spots (spawns aren't in the neighbour grid until the next step)
    std::array<Vec2, 128> born_{};
    std::uint32_t bornCount_ = 0;
    // drawn pick scratch
    std::vector<std::uint64_t> order_;
    bool focusOn_ = false;
    Vec2 focus_{};
    std::vector<DoorSlot> doors_;
    std::vector<std::uint32_t> doorFirst_, doorList_; // doors by street node (each door under both its edge's nodes)
    float density_ = 1.f;
    std::uint32_t indoors_ = 0;
    CrowdOcclusionFn occlude_ = nullptr;
    void* occludeCtx_ = nullptr;
    CrowdCounts counts_;
    double time_ = 0.0;
    float acc_ = 0.f;
    std::uint64_t steps_ = 0;
    bool frozen_ = false;
};

} // namespace ftcore
