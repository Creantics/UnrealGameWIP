#pragma once

// Party followers in the water (the user's ruling of 2026-11-17: partners swim with the lead when it jumps in). One
// WaterFollowParty decides, for up to kWaterFollowMax followers, when each goes in (by steps or a jump from the edge,
// staggered), where it swims (a loose slot behind the lead at the surface, a 4-6 m band under water out of the shot),
// when it dives and surfaces (with the lead, never on its own), where and in which order it climbs out, and where it
// is placed when the lead is rescued. It moves nothing: the host moves each follower to the order's target at its
// speed and feeds the follower's own WaterLoco for the animation. Followers have no breath or rescue rule.
//
// Metres, seconds, Z up. step() never allocates. The choices that read the exits are free functions
// (waterFollowEntry, waterFollowExit), so the host can feed them from WaterBodies and tests from a plain array.

#include "ftcore/Swim.h"
#include "ftcore/WaterBodies.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ftcore {

inline constexpr int kWaterFollowMax = 4;
inline constexpr int kWaterEntryAlts = 4; // other bank points a member may jump from (WaterFollowMember::altEdges)

struct WaterFollowTuning {
    float trigger = 0.8f;          // s the lead swims before the followers start in
    float staggerMin = 0.4f;       // s between one follower's entry and the next's: staggerMin + staggerSpan x hash
    float staggerSpan = 0.4f;
    float stepsWithin = 8.f;       // m: a Steps exit this near the follower's path to the lead is walked in by
    float slotBack = 2.5f;         // m behind the lead at the surface
    float slotSide = 1.5f;         // m to the side (the sides alternate by order)
    float catchUp = 0.2f;          // share over the lead's speed cap when off the slot
    float slotSlack = 1.f;         // m off the slot before it catches up; within it a treading lead is matched
    float treadSpeed = 0.4f;       // m/s: a lead slower than this treads, and so do the followers at their slots
    float diveDelay = 0.3f;        // s x (order + 1) after the lead's dive
    float underNear = 4.f;         // m: the under-water band around the lead
    float underFar = 6.f;
    float underSlotAngle = 35.f;   // deg off straight behind, toward the follower's side
    float keepOut = 0.8f;          // m around the camera-to-lead segment
    float underTop = 0.6f;         // m: an under-water slot stays this far under the surface
    float bedClearance = 0.9f;     // m: and this far over the bed (the body's centre)
    float exitWithin = 25.f;       // m: the lead's own exit is used when its foot is this near; else the nearest
    float climbGap = 0.6f;         // s between one follower's climb and the next's
    float surfaceSpeed = 1.5f;     // m/s: the lead's caps at the surface (WaterLoco's swimSpeed, swimSprintSpeed)
    float surfaceSprint = 2.4f;
    float underSpeed = 2.2f;       // m/s: under water (Swim's swimSpeed, sprintSpeed)
    float underSprint = 3.2f;
    float exitReturn = 3.f;        // m: a lead swimming again (not exiting) this far from a follower's exit foot...
    float returnHold = 1.f;        // s: ...this long sends the follower back from its exit queue to the surface
    float teleport = 10.f;         // m: the lead moved this far in one step (a scene, a fight, a load): lost
    float watchdog = 20.f;         // s: a follower stuck this long in a water phase is placed with the lead
    float stuckFar = 12.f;         // m: at the surface or under, further than this from the lead counts as stuck
    float entryLimit = 15.f;       // s: a walk to its entry not over by then is given up (ashore; it picks again)
    float rejoinNear = 1.5f;       // m: a rejoined follower this near its rejoin target is at the surface (else asked again)
    float startHold = 1.f;         // s: a start in the water (startInWater) waits this long for the lead's swim to reach the
                                   // party; a lead still out of the water then sends the members ashore
    // The host's climb out (FTPartyFollower), here so a test can budget it: up a ladder and over its top, a mantle onto
    // the bank (no exit), a walk up a flight of steps (at least 0.5 s), and how near its place in the queue is "at" it.
    float ladderSeconds = 2.2f;
    float mantleSeconds = 1.6f;
    float stepsSpeed = 1.2f;       // m/s
    float atExit = 0.6f;           // m
    // -PartyExitFast (item 2 of 11-18; all off until its proof passes):
    bool exitCatchUp = false;      // ToExit off its place by over slotSlack swims at the surface speed plus catchUp
    bool climbPath = false;        // a ladder's climb or a mantle as one WaterClimbPath at the speeds below, never a jump
    float footSwimSpeed = 2.f;     // m/s: from its place in the queue to the ladder's foot
    float ladderSpeed = 2.5f;      // m/s up the ladder, or up the bank in a mantle (the lead's: 6 m in its 2.4 s at most)
    float acrossSpeed = 2.f;       // m/s over the top onto the bank
    float climbBehindLead = -1.f;  // s into the lead's ladder climb the first may start up behind it (< 0: once it is out)
    bool exitPickNear = false;     // the lead's way out (its ladder, or the flight it walked out by); else the nearest
                                   // within exitWithin; else none (a mantle by the lead), never one in other water
    bool exitReadyFirst = false;   // a member at its place in the queue climbs ahead of one before it still swimming to
                                   // its own (11-19: the second waited 1.75 s there); the ladder's foot then spaced too:
                                   // each reaches it climbGap after the last (with climbPath, at footSwimSpeed)
};

// -PartyExitFast: the party out of the water within 4 s of the lead (11-18 item 2). A flight of steps is as before.
inline void waterFollowFastExit(WaterFollowTuning& t) {
    t.exitCatchUp = true;
    t.climbPath = true;
    t.climbBehindLead = 1.f;
    t.exitPickNear = true;
    t.exitReadyFirst = true;
}

// What a follower is doing in the water.
enum class WaterFollowPhase : std::uint8_t {
    Ashore,   // not in the water: the party's own follow (PartyShore keeps it at the bank)
    ToEntry,  // walking to its entry point (the head of the steps, or the edge it jumps from)
    Entering, // the host's entry move (walking down the steps, or the jump) is running
    Surface,  // at the surface, keeping its slot
    DiveWait, // the lead dived; it dives after its delay
    Under,    // under water, keeping the band
    ToExit,   // swimming to its exit's foot
    ExitWait, // at the foot, waiting its turn
    Climbing, // the host's climb (or the walk up the steps) is running
    Placed,   // the lead was rescued: put with the lead by the host
};

enum class WaterFollowEntryKind : std::uint8_t { None, Steps, Jump };

struct WaterFollowEntry {
    WaterFollowEntryKind kind = WaterFollowEntryKind::None;
    Vec2 point{};      // where it starts: the head of the steps (on the bank), or the edge point it jumps from
    Vec2 into{};       // where the entry move ends in the water: the steps' foot, or a metre out from the edge
    int exitIndex = -1; // Steps: the exit used
    bool otherWater = false; // Jump, given the bodies: its landing is not in the lead's body (another body, or none)
    bool cutOff = false;     // Jump: every bank point offered was cut off from the lead (WaterEntryEdge::cutOff)
};

// A bank point a follower may jump in from, and the way into the water there. `cutOff`: the host found the jump or the
// swim from its landing to the lead blocked by a solid (11-21 replay 6: a block in the sea standing between the quay and
// the lead left a pocket no slide gets out of); such a point is never jumped from.
struct WaterEntryEdge {
    Vec2 point{};
    Vec2 out{1.f, 0.f};
    bool cutOff = false;
};

namespace water_follow_detail {
inline float len(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
inline float dist(Vec2 a, Vec2 b) { return len(Vec2{a.x - b.x, a.y - b.y}); }
// Distance from p to the segment a-b.
inline float segDist(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab{b.x - a.x, b.y - a.y};
    const float l2 = ab.x * ab.x + ab.y * ab.y;
    float t = l2 > 0.f ? ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / l2 : 0.f;
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    return dist(p, Vec2{a.x + ab.x * t, a.y + ab.y * t});
}
// A fixed hash of the member's order and a seed, 0..1 (the stagger's spread).
inline float hash01(int order, std::uint32_t seed) {
    std::uint32_t h = seed ^ (std::uint32_t(order + 1) * 0x9E3779B9u);
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return float(h & 0xFFFFu) / 65535.f;
}
} // namespace water_follow_detail

// The entry: the Steps exit whose head is nearest the follower's path to the lead, within `within` m of it, walked in;
// else a jump from the first of `edges` (the bank point toward the lead first, PartyShore's, then the host's others along
// the bank) that is not cut off from the lead and lands in the lead's body, landing a metre out along its `out` (unit).
// With none such, the jump from edges[0], marked otherWater and cutOff as found: the plan then places the follower with
// the lead (it never jumps into a pocket or other water). None only with no edges at all.
// With `water` (and the lead in a body), only steps whose foot is in the lead's body count: a flight into other water
// (a canal by the sea, 11-20 run 5) has no swim to the lead. A jump landing out of the lead's body is marked otherWater.
inline WaterFollowEntry waterFollowEntry(const WaterExit* exits, std::size_t count, Vec2 from, Vec2 lead,
                                         const WaterEntryEdge* edges, std::size_t edgeCount, float within = 8.f,
                                         const WaterBodies* water = nullptr) {
    namespace d = water_follow_detail;
    WaterFollowEntry e;
    const int leadBody = water ? water->bodyAt(lead) : -1;
    float best = within;
    for (std::size_t i = 0; i < count; ++i) {
        if (exits[i].kind != WaterExitKind::Steps) continue;
        // A foot in another body is no way to the lead; a foot in none (a polygon a few cm short) is still a flight.
        if (leadBody >= 0) {
            const int footBody = water->bodyAt(exits[i].foot);
            if (footBody >= 0 && footBody != leadBody) continue;
        }
        const float dd = d::segDist(exits[i].top, from, lead);
        if (dd <= best) {
            best = dd;
            e.kind = WaterFollowEntryKind::Steps;
            e.point = exits[i].top;
            e.into = exits[i].foot;
            e.exitIndex = int(i);
        }
    }
    if (e.kind != WaterFollowEntryKind::None || edgeCount == 0) return e;
    auto jumpFrom = [&](const WaterEntryEdge& g) {
        e.kind = WaterFollowEntryKind::Jump;
        e.point = g.point;
        e.into = Vec2{g.point.x + g.out.x, g.point.y + g.out.y};
        e.otherWater = leadBody >= 0 && water->bodyAt(e.into) != leadBody;
        e.cutOff = g.cutOff;
    };
    for (std::size_t i = 0; i < edgeCount; ++i) {
        if (edges[i].cutOff) continue;
        jumpFrom(edges[i]);
        if (!e.otherWater) return e;
    }
    jumpFrom(edges[0]);
    return e;
}

// The same with one bank point, never cut off (the tests' and the old hosts' form).
inline WaterFollowEntry waterFollowEntry(const WaterExit* exits, std::size_t count, Vec2 from, Vec2 lead, Vec2 edge,
                                         Vec2 edgeOut, float within = 8.f, const WaterBodies* water = nullptr) {
    const WaterEntryEdge one{edge, edgeOut, false};
    return waterFollowEntry(exits, count, from, lead, &one, 1, within, water);
}

// The surface swimmer's step at the water's edge (11-20: followers swam on over banks and an in-sea block, and dried
// there). `want` is the step its swim would take (m, planar); `sample(Vec2 step)` is the host's WaterState where that step
// would put it (its feet by the surface rule over the bed looked at there; a place inside a solid read as Dry). The
// step is taken when it stays in the water (waterIn); else it slides: turned 30, 60, 90, then 120 degrees off to `side`
// (kept by the host from the last slide, so it follows one way round an edge), then the same to the other side, at the
// cosine's share of its length (half at least); else it holds where it is (the plan's watchdog then rejoins it with the
// lead: WaterFollowMember::blocked). A zero step (treading) is taken without a sample. Allocation-free: `sample` is any
// callable, called at most nine times.
enum class WaterStepKind : std::uint8_t { Take, Slide, Hold };
struct WaterSurfaceStep {
    WaterStepKind kind = WaterStepKind::Take;
    Vec2 step{};      // m: the step to make (zero when held)
    float side = 1.f; // the side slid to (+1 counter-clockwise in the frame's x-y), else the side given
};
template <class Sample>
WaterSurfaceStep waterSurfaceStep(Vec2 want, float side, const Sample& sample) {
    WaterSurfaceStep r;
    r.side = side < 0.f ? -1.f : 1.f;
    const float l2 = want.x * want.x + want.y * want.y;
    if (!(l2 > 1e-12f) || waterIn(sample(want))) {
        r.step = l2 > 1e-12f ? want : Vec2{};
        return r;
    }
    // cos and sin of 30, 60, 90 and 120 degrees; the share of the step's length.
    constexpr float kCos[4] = {0.8660254f, 0.5f, 0.f, -0.5f};
    constexpr float kSin[4] = {0.5f, 0.8660254f, 1.f, 0.8660254f};
    for (int k = 0; k < 2; ++k) {
        const float sd = k == 0 ? r.side : -r.side;
        for (int a = 0; a < 4; ++a) {
            const float share = kCos[a] > 0.5f ? kCos[a] : 0.5f;
            const float c = kCos[a], s = kSin[a] * sd;
            const Vec2 turned{(want.x * c - want.y * s) * share, (want.x * s + want.y * c) * share};
            if (waterIn(sample(turned))) {
                r.kind = WaterStepKind::Slide;
                r.step = turned;
                r.side = sd;
                return r;
            }
        }
    }
    r.kind = WaterStepKind::Hold;
    return r;
}

// The exit: the lead's own (leadExit, -1 when it left by none) when its foot is within `within` m of the follower;
// else the exit whose foot is nearest. -1: there are none.
// With `nearOnly`, none (-1) when the nearest is further than `within` too.
inline int waterFollowExit(const WaterExit* exits, std::size_t count, int leadExit, Vec2 at, float within = 25.f,
                           bool nearOnly = false) {
    namespace d = water_follow_detail;
    if (leadExit >= 0 && std::size_t(leadExit) < count && d::dist(exits[leadExit].foot, at) <= within) return leadExit;
    int best = -1;
    float bestD = 0.f;
    for (std::size_t i = 0; i < count; ++i) {
        const float dd = d::dist(exits[i].foot, at);
        if (best < 0 || dd < bestD) {
            best = int(i);
            bestD = dd;
        }
    }
    return nearOnly && best >= 0 && bestD > within ? -1 : best;
}

// The flight of steps a lead out of the water with no climb walked out by: the one whose foot-to-top line passes within
// `within` m of it (the nearest such); -1 if none.
inline int waterFollowWalkedOut(const WaterExit* exits, std::size_t count, Vec2 lead, float within = 1.5f) {
    int best = -1;
    float bestD = within;
    for (std::size_t i = 0; i < count; ++i) {
        if (exits[i].kind != WaterExitKind::Steps) continue;
        const Vec2 a = exits[i].foot, b = exits[i].top;
        const float abx = b.x - a.x, aby = b.y - a.y, ll = abx * abx + aby * aby;
        float f = ll > 1e-6f ? ((lead.x - a.x) * abx + (lead.y - a.y) * aby) / ll : 0.f;
        f = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
        const float dd = water_follow_detail::dist(lead, Vec2{a.x + abx * f, a.y + aby * f});
        if (dd <= bestD) {
            best = int(i);
            bestD = dd;
        }
    }
    return best;
}

// The host's climb out (s) without climbPath: a ladder's, a flight of steps' (`metres` from where the climb starts to the
// top), or a mantle's with no exit.
inline float waterFollowClimbSeconds(const WaterFollowTuning& t, const WaterExit* exit, float metres) {
    if (!exit) return t.mantleSeconds;
    if (exit->kind != WaterExitKind::Steps) return t.ladderSeconds;
    const float s = t.stepsSpeed > 0.f ? metres / t.stepsSpeed : 0.5f;
    return s > 0.5f ? s : 0.5f;
}

// climbPath's climb out, sampled by the host each frame (feet positions, m): a swim from where it waits to the ladder's
// foot, up it to the top's height, then across onto the top; with no exit the same up and across to the host's landing;
// a flight of steps one walk from where it waits to the top, as without climbPath. Each leg runs at a set speed, so two
// frames are never more than that speed x dt apart.
struct WaterClimbPath {
    SwimVec from, foot, up, to;
    float swim = 0.f, climb = 0.f, across = 0.f; // each leg's seconds
    float seconds() const { return swim + climb + across; }
    int leg(float t) const { return t < swim ? 0 : t < swim + climb ? 1 : 2; } // 0 the swim, 1 up, 2 across (or the walk)
    float legProgress(float t) const {
        const int l = leg(t);
        const float l0 = l == 0 ? 0.f : l == 1 ? swim : swim + climb;
        const float len = l == 0 ? swim : l == 1 ? climb : across;
        const float f = len > 0.f ? (t - l0) / len : 1.f;
        return f < 0.f ? 0.f : f > 1.f ? 1.f : f;
    }
    SwimVec at(float t) const {
        if (t <= 0.f) return from;
        if (t < swim) return mix(from, foot, t / swim);
        t -= swim;
        if (t < climb) return mix(foot, up, t / climb);
        t -= climb;
        if (t < across) return mix(up, to, t / across);
        return to;
    }
    static SwimVec mix(const SwimVec& a, const SwimVec& b, float f) {
        return SwimVec{a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f};
    }
};

inline WaterClimbPath waterClimbPath(const WaterFollowTuning& t, const WaterExit* exit, SwimVec from, SwimVec landing) {
    const auto secs = [](float metres, float speed) { return speed > 0.f && metres > 0.f ? metres / speed : 0.f; };
    const auto flat = [](const SwimVec& a, const SwimVec& b) {
        return std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
    };
    WaterClimbPath p;
    p.from = from;
    if (exit && exit->kind == WaterExitKind::Steps) {
        p.foot = p.up = from;
        p.to = SwimVec{exit->top.x, exit->top.y, exit->topZ};
        const float dx = p.to.x - from.x, dy = p.to.y - from.y, dz = p.to.z - from.z;
        const float walk = secs(std::sqrt(dx * dx + dy * dy + dz * dz), t.stepsSpeed);
        p.across = walk > 0.5f ? walk : 0.5f;
        return p;
    }
    p.foot = exit ? SwimVec{exit->foot.x, exit->foot.y, from.z} : from;
    p.to = exit ? SwimVec{exit->top.x, exit->top.y, exit->topZ} : landing;
    p.up = SwimVec{p.foot.x, p.foot.y, p.to.z > from.z ? p.to.z : from.z};
    p.swim = secs(flat(from, p.foot), t.footSwimSpeed);
    p.climb = secs(p.up.z - p.foot.z, t.ladderSpeed);
    p.across = secs(flat(p.up, p.to), t.acrossSpeed);
    return p;
}

// The lead this step.
struct WaterFollowLead {
    bool swimming = false;      // the lead's WaterLoco is in a water state at swimming depth (not Dry or Wade)
    SwimState swim = SwimState::Surface; // the lead's Swim
    SwimVec position;           // its body's reference point
    Vec2 velocity;              // planar m/s
    float yaw = 0.f;            // deg (the slot's frame when it is still)
    bool sprint = false;
    bool exiting = false;       // the lead is climbing out or walking out (or has)
    int exitIndex = -1;         // the exit it uses (-1: none)
    bool out = false;           // the lead's climb is over: it is out of the water
    SwimVec camera;             // the camera, for the keep-out
    bool resumed = false;       // the first step after the planner was parked (a scene, a hidden or uncontrolled lead)
};

// A follower this step (the host's).
struct WaterFollowMember {
    bool active = false;        // a party follower in this slot (the party hidden, P or LB, is not: its slot clears)
    bool lensHidden = false;    // hidden from the camera's lens only (it came between the camera and the lead): visual,
                                // the plan ignores it, so a swimmer keeps its phase and place
    SwimVec position;
    bool held = false;          // SetHeld: a scene holds it; no entry starts and it is not moved
    bool traversing = false;    // mid-traversal: no entry starts
    bool atEntry = false;       // it reached its entry point (ToEntry)
    bool entered = false;       // its entry move is over: it is in the water (Entering)
    bool atExit = false;        // it reached its exit's foot (ToExit)
    bool climbed = false;       // its climb is over: it is out (Climbing)
    float surfaceZ = 0.f;       // the water at it
    float bedZ = -10.f;
    Vec2 edge{};                // ashore: the bank point nearest it (PartyShore) and the way into the water there
    Vec2 edgeOut{1.f, 0.f};
    bool hasEdge = true;        // ashore: the host found water toward the lead (else no jump entry: it stays ashore)
    bool edgeCutOff = false;    // ashore: the jump from edge, or the swim from its landing to the lead, meets a solid
    WaterEntryEdge altEdges[kWaterEntryAlts]{}; // ashore: other bank points along the bank, nearest first (the host's)
    int altEdgeCount = 0;
    bool blocked = false;       // at the surface: its last move was held at the water's edge (waterSurfaceStep's Hold).
                                // The watchdog counts it whatever the gap, and rejoins it with the lead
    bool turned = false;        // ... or slid along it (Slide): counted the same while it has not got 0.5 m from where
                                // the count began (a slide that gets on is a swim, counted as one)
    bool hasPlacedAt = false;   // Placed: the host put it on the rejoin step at placedAt, not at the slot (the slot was
    Vec2 placedAt{};            // on a bank or a block, so it went to the first water toward the lead); accepted there
};

// What a follower is to do.
// Host contract: while a member is traversing (a mantle, a vault, a scene move) the host ignores its order's target and
// speed; ToEntry may be picked mid-traversal, and the entry starts only once the traversal ends.
struct WaterFollowOrder {
    WaterFollowPhase phase = WaterFollowPhase::Ashore;
    bool changed = false;
    SwimVec target;             // where to go (planar at the surface and ashore; 3D under water)
    float speed = 0.f;          // m/s to move at (0: hold, tread at the surface)
    bool sprint = false;        // the speed is over the walk-cap of its mode
    WaterFollowEntry entry;     // ToEntry, Entering: the entry chosen
    bool startEntry = false;    // this step: start the entry move (the jump, or the walk down the steps)
    int exitIndex = -1;         // ToExit, ExitWait, Climbing: its exit
    bool startClimb = false;    // this step: start the climb
    bool place = false;         // this step: place it with the lead (the lead's rescue)
    bool noEntry = false;       // this step: placed at plan time because no entry reaches the lead (every jump lands in
                                // other water or is cut off from it by a solid); the host logs it
    bool rejoin = false;        // Placed with the lead swimming: put it at target (its surface slot, in the water),
                                // swimming; the next step it is at the surface and follows from there
    // For the follower's WaterLoco: Swim's state, as if it had one, and the step's events.
    SwimState swim = SwimState::Surface;
    bool dived = false;
    bool surfaced = false;
};

class WaterFollowParty {
public:
    explicit WaterFollowParty(const WaterFollowTuning& t = WaterFollowTuning{}, std::uint32_t seed = 0x5717u)
        : tuning_(t), seed_(seed) {
        reset();
    }
    void reset() {
        for (Slot& s : slots_) s = Slot{};
        clock_ = 0.f;
        leadSwim_ = 0.f;
        leadDiveAt_ = -1.f;
        lastEntryAt_ = -1.f;
        lastClimbAt_ = -1.f;
        lastFootAt_ = -1.f;
        leadExitAt_ = -1.f;
        leadUnder_ = false;
        hasLastLead_ = false;
        startLeft_ = 0.f;
        starting_ = false;
    }
    // A start in the water (a scene or hold that begins in the sea, a lead switch while swimming): the plan starts with
    // every member Placed, so the first step asks the host to put each at its surface slot (a rejoin) instead of starting
    // ashore and walking to an entry. While the lead's swim has not reached the party yet (its flag lags the placement by
    // a frame or so) that holds for at most startHold: the rejoin is asked each step and nobody is sent ashore, to an
    // entry or to an exit; a lead climbing out meanwhile takes them out with it. After the hold, a lead still out of the
    // water sends them ashore as after any placement.
    void startInWater() {
        reset();
        for (Slot& s : slots_) {
            s.order.phase = WaterFollowPhase::Placed;
            s.watchPhase = WaterFollowPhase::Placed;
        }
        startLeft_ = tuning_.startHold > 0.f ? tuning_.startHold : 0.f;
    }
    // In a start's hold (startInWater, the lead's swim not seen yet).
    bool starting() const { return starting_; }
    const WaterFollowTuning& tuning() const { return tuning_; }
    // The stagger after the member before (s).
    float stagger(int order) const {
        return tuning_.staggerMin + tuning_.staggerSpan * water_follow_detail::hash01(order, seed_);
    }
    const WaterFollowOrder& order(int i) const { return slots_[i].order; }

    // One step. `exits` are the water's ways out (WaterBodies::exit(i)). With `water`, every slot is kept inside the
    // water's footprint at swimming depth (pulled in toward the lead) and an under-water slot over the bed there.
    void step(const WaterFollowLead& lead, const WaterFollowMember* members, int count, const WaterExit* exits,
              std::size_t exitCount, float dt, const WaterBodies* water = nullptr) {
        if (!std::isfinite(dt) || dt < 0.f) dt = 0.f;
        if (dt > 0.25f) dt = 0.25f;
        clock_ += dt;
        if (count > kWaterFollowMax) count = kWaterFollowMax;
        if (count < 0) count = 0;
        const bool leadUnder = lead.swim == SwimState::Diving || lead.swim == SwimState::Under ||
                               lead.swim == SwimState::Surfacing;
        leadSwim_ = lead.swimming || leadUnder ? leadSwim_ + dt : 0.f;
        if (leadUnder && !leadUnder_) leadDiveAt_ = clock_;
        if (!leadUnder) leadDiveAt_ = -1.f;
        leadUnder_ = leadUnder;
        if (!lead.exiting)
            leadExitAt_ = -1.f;
        else if (leadExitAt_ < 0.f)
            leadExitAt_ = clock_;
        const bool rescued = lead.swim == SwimState::Rescued;
        // Lost: the lead jumped (a scene, a fight or a teleport moved it), or it is back from a parked planner on dry
        // ground. The old plan is not resumed against it: every follower in the water is placed with the lead.
        const bool leadDry = !lead.swimming && !leadUnder;
        // A start in the water holds until the lead's swim is seen, or startHold has run.
        if (!leadDry) startLeft_ = 0.f;
        starting_ = startLeft_ > 0.f;
        startLeft_ = startLeft_ > dt ? startLeft_ - dt : 0.f;
        const bool jumped = hasLastLead_ && dist3(lead.position, lastLead_) > tuning_.teleport;
        const bool lost = jumped || (lead.resumed && leadDry);
        if (jumped) leadSwim_ = leadDry ? 0.f : dt; // a new swim: the entry's wait starts again
        lastLead_ = lead.position;
        hasLastLead_ = true;

        for (int i = 0; i < count; ++i) {
            Slot& s = slots_[i];
            WaterFollowOrder& o = s.order;
            const WaterFollowMember& m = members[i];
            o.changed = false;
            o.startEntry = false;
            o.startClimb = false;
            o.place = false;
            o.noEntry = false;
            const bool rejoined = o.rejoin; // last step asked the host to put it at o.target, in the water
            const Vec2 rejoinTo{o.target.x, o.target.y};
            o.rejoin = false;
            o.dived = false;
            o.surfaced = false;
            if (!m.active) {
                s = Slot{};
                continue;
            }
            s.held = m.held;
            const WaterFollowPhase was = o.phase;
            if (rescued && inWater(was)) {
                go(o, WaterFollowPhase::Placed);
                o.place = true;
            }
            if (leadSwim_ <= 0.f) s.entryFails = 0;
            if (o.phase == WaterFollowPhase::ToEntry && (lost || s.watch >= tuning_.entryLimit)) {
                // A second walk given up while the lead still swims: it is placed with the lead, in the water.
                const bool again = !lost && ++s.entryFails >= 2 && (lead.swimming || leadUnder);
                go(o, again ? WaterFollowPhase::Placed : WaterFollowPhase::Ashore);
                o.entry = WaterFollowEntry{};
                o.place = again;
            }
            // Lost, or stuck past the watchdog (it never reached its exit, its slot or the end of its move): placed.
            if (inWater(o.phase) && (lost || s.watch >= tuning_.watchdog)) {
                go(o, WaterFollowPhase::Placed);
                o.place = true;
            }
            switch (o.phase) {
            case WaterFollowPhase::Ashore:
                // Mid-traversal it picks its entry (ToEntry waits); a walk just given up picks again next step.
                if (was != WaterFollowPhase::ToEntry && !m.held && !lead.exiting && leadSwim_ >= tuning_.trigger) {
                    const Vec2 here{m.position.x, m.position.y};
                    WaterEntryEdge edges[1 + kWaterEntryAlts];
                    edges[0] = WaterEntryEdge{m.edge, m.edgeOut, m.edgeCutOff};
                    const int alts = m.altEdgeCount < 0 ? 0 : (m.altEdgeCount > kWaterEntryAlts ? kWaterEntryAlts : m.altEdgeCount);
                    for (int k = 0; k < alts; ++k) edges[1 + k] = m.altEdges[k];
                    o.entry = waterFollowEntry(exits, exitCount, here, Vec2{lead.position.x, lead.position.y}, edges,
                                               std::size_t(1 + alts), tuning_.stepsWithin, water);
                    // A jump needs the water's edge; with none found toward the lead it stays ashore. A jump into other
                    // water than the lead's (no steps into its body near the path, 11-20), or one cut off from the lead
                    // by a solid from every bank point offered (11-21), is no way to it: the follower is placed with the
                    // lead in the water now (a rejoin, as after a second walk given up), never left to slide along the
                    // solid until the watchdog.
                    if (o.entry.kind == WaterFollowEntryKind::Jump && m.hasEdge && (o.entry.otherWater || o.entry.cutOff)) {
                        go(o, WaterFollowPhase::Placed);
                        o.entry = WaterFollowEntry{};
                        o.place = true;
                        o.noEntry = true;
                    } else if (o.entry.kind == WaterFollowEntryKind::Steps || m.hasEdge) {
                        go(o, WaterFollowPhase::ToEntry);
                    } else {
                        o.entry = WaterFollowEntry{};
                    }
                }
                break;
            case WaterFollowPhase::ToEntry:
                // The lead left the water (or a scene took the follower) before it went in: it stays ashore.
                if (m.held || leadSwim_ <= 0.f || lead.exiting) {
                    go(o, WaterFollowPhase::Ashore);
                    break;
                }
                if (m.atEntry && !m.traversing && entryTurn(i)) {
                    go(o, WaterFollowPhase::Entering);
                    o.startEntry = true;
                    s.entryAt = clock_;
                    lastEntryAt_ = clock_;
                }
                break;
            case WaterFollowPhase::Entering:
                if (m.entered) go(o, leadUnder ? WaterFollowPhase::DiveWait : WaterFollowPhase::Surface);
                break;
            case WaterFollowPhase::Surface:
                if (lead.exiting) {
                    beginExit(o, lead, m, exits, exitCount);
                } else if (leadUnder) {
                    go(o, WaterFollowPhase::DiveWait);
                }
                break;
            case WaterFollowPhase::DiveWait:
                if (!leadUnder) {
                    go(o, WaterFollowPhase::Surface);
                } else if (leadDiveAt_ >= 0.f && clock_ - leadDiveAt_ >= tuning_.diveDelay * float(i + 1)) {
                    go(o, WaterFollowPhase::Under);
                    o.dived = true;
                }
                break;
            case WaterFollowPhase::Under:
                if (!leadUnder) {
                    go(o, WaterFollowPhase::Surface);
                    o.surfaced = true;
                }
                break;
            case WaterFollowPhase::ToExit:
            case WaterFollowPhase::ExitWait:
                // The lead turned back from the bank: swimming again, not exiting, away from this exit for returnHold.
                if ((lead.swimming || leadUnder) && !lead.exiting) {
                    const bool hasExit = o.exitIndex >= 0 && std::size_t(o.exitIndex) < exitCount;
                    const float away = hasExit ? water_follow_detail::dist(exits[o.exitIndex].foot,
                                                                          Vec2{lead.position.x, lead.position.y})
                                               : tuning_.exitReturn + 1.f;
                    s.backFor = away > tuning_.exitReturn ? s.backFor + dt : 0.f;
                    if (s.backFor >= tuning_.returnHold) {
                        s.backFor = 0.f;
                        go(o, WaterFollowPhase::Surface);
                        o.exitIndex = -1;
                        break;
                    }
                } else {
                    s.backFor = 0.f;
                }
                if (o.phase == WaterFollowPhase::ToExit && m.atExit) go(o, WaterFollowPhase::ExitWait);
                if (o.phase == WaterFollowPhase::ExitWait && !m.held) {
                    const float swim = footSwim(o, m, exits, exitCount);
                    if (climbTurn(i, lead, swim)) {
                        go(o, WaterFollowPhase::Climbing);
                        o.startClimb = true;
                        s.climbAt = clock_;
                        lastClimbAt_ = clock_;
                        lastFootAt_ = clock_ + swim;
                    }
                }
                break;
            case WaterFollowPhase::Climbing:
                if (m.climbed) go(o, WaterFollowPhase::Ashore);
                break;
            case WaterFollowPhase::Placed: // held at least the step it was placed in, so the host sees it
                // Then: ashore with the lead out of the water; with it swimming, at its surface slot (the host put it
                // there on the rejoin step), so it never stays frozen while the lead swims on.
                // Only once the host has put it there (within rejoinNear of the rejoin's target): a missed rejoin step
                // asks again. Rejoined and the lead out since: Placed one more step, so the host lands it beside the lead.
                // In a start's hold (startInWater) it is asked to rejoin each step; a lead climbing out meanwhile counts as in
                // the water, so the rejoined member goes to the surface and from there follows it out.
                if (was == WaterFollowPhase::Placed && !rescued) {
                    if (lead.swimming || leadUnder || (starting_ && lead.exiting)) {
                        const Vec2 at = m.hasPlacedAt ? m.placedAt : rejoinTo;
                        const float dx = m.position.x - at.x;
                        const float dy = m.position.y - at.y;
                        if (rejoined && dx * dx + dy * dy <= tuning_.rejoinNear * tuning_.rejoinNear) {
                            go(o, WaterFollowPhase::Surface);
                            s.entryFails = 0;
                        }
                    } else if (!rejoined && !starting_) {
                        go(o, WaterFollowPhase::Ashore);
                    }
                }
                break;
            }
            // A follower left in the water when the lead is out by no exit move (a scene, a load): its own way out.
            if (inWater(o.phase) && o.phase != WaterFollowPhase::ToExit && o.phase != WaterFollowPhase::ExitWait &&
                o.phase != WaterFollowPhase::Climbing && o.phase != WaterFollowPhase::Placed && !lead.swimming &&
                !leadUnder && !lead.exiting) {
                beginExit(o, lead, m, exits, exitCount);
            }
            target(o, lead, m, i, exits, exitCount, water);
            watch(i, s, o, lead, m, exits, exitCount, dt);
        }
        for (int i = count; i < kWaterFollowMax; ++i) slots_[i] = Slot{};
    }

    // The watchdog's running time for slot i (s): how long it has been stuck in its water phase.
    float stuckFor(int i) const { return i >= 0 && i < kWaterFollowMax ? slots_[i].watch : 0.f; }
    // Walks to an entry given up this swim (the second places it with the lead).
    int entryFailsFor(int i) const { return i >= 0 && i < kWaterFollowMax ? slots_[i].entryFails : 0; }

private:
    struct Slot {
        WaterFollowOrder order;
        float entryAt = -1.f;
        float climbAt = -1.f;
        bool held = false;        // its member is held by a scene: out of the climb queue
        int entryFails = 0;       // walks to an entry given up this swim
        float backFor = 0.f;      // s the lead has swum away from its exit (the queue's return)
        WaterFollowPhase watchPhase = WaterFollowPhase::Ashore;
        float watch = 0.f;        // s stuck in watchPhase
        float bestGap = 0.f;      // m: ToExit's nearest to its exit's foot so far
        bool edge = false;        // the last step counted it at the water's edge (blocked, or turned without getting on)
        Vec2 edgeFrom{};          // m: where that count began
    };

    static bool swimPhase(WaterFollowPhase p) {
        return p == WaterFollowPhase::Surface || p == WaterFollowPhase::DiveWait || p == WaterFollowPhase::Under;
    }

    // The watchdog: no progress for watchdog seconds places it. At the surface or under: over stuckFar from the lead and
    // not 0.5 m nearer it than its best; on its way to an exit: not 0.5 m nearer the foot; waiting at the foot: only
    // while nothing explains the wait (the lead is out, no un-held member before it is still queued); in an entry or a
    // climb: the phase's time.
    void watch(int i, Slot& s, const WaterFollowOrder& o, const WaterFollowLead& lead, const WaterFollowMember& m,
               const WaterExit* exits, std::size_t exitCount, float dt) const {
        namespace d = water_follow_detail;
        const bool hasExit = o.exitIndex >= 0 && std::size_t(o.exitIndex) < exitCount;
        const bool swim = swimPhase(o.phase);
        const float gap = swim ? dist3(m.position, lead.position)
                               : hasExit ? d::dist(exits[o.exitIndex].foot, Vec2{m.position.x, m.position.y}) : 0.f;
        const Vec2 at{m.position.x, m.position.y};
        const bool wasEdge = s.edge;
        s.edge = false;
        if (o.phase != s.watchPhase) {
            s.watchPhase = o.phase;
            s.watch = 0.f;
            s.bestGap = gap;
            return;
        }
        const bool gotOn = m.turned && !m.blocked && wasEdge && d::dist(at, s.edgeFrom) > 0.5f;
        if (o.phase == WaterFollowPhase::ToEntry) {
            s.watch = m.held ? 0.f : s.watch + dt; // the entry's time limit (a traversal on the way counts too)
        } else if (swim && !m.held && (m.blocked || m.turned) && !gotOn) {
            // Held at the water's edge, or sliding along it without getting 0.5 m on (waterSurfaceStep): it counts however
            // near the lead is or comes (11-20 run 5: a lead swimming back to it kept the gap shrinking, and the count
            // never ran). A full step, or a slide that gets on, is counted as any swim below.
            if (!wasEdge) s.edgeFrom = at;
            s.edge = true;
            s.watch += dt;
        } else if (!inWater(o.phase) || m.held || (swim && gap <= tuning_.stuckFar)) {
            s.watch = 0.f;
            s.bestGap = gap;
        } else if (swim || (o.phase == WaterFollowPhase::ToExit && hasExit)) {
            if (s.watch == 0.f) s.bestGap = gap; // a new count: its best is where it starts
            if (gap < s.bestGap - 0.5f) {
                s.bestGap = gap;
                s.watch = 0.f;
            } else {
                s.watch += dt;
            }
        } else if (o.phase == WaterFollowPhase::ExitWait) {
            // Counts only with the queue clear. The one wait left then is climbTurn's spacing (climbGap since the last
            // climb, and with exitReadyFirst the last climber's swim to the foot). It is bounded: while this one waits
            // no member after it may start a climb, and each member climbs once, so the wait ends within the sum of
            // climbGap + foot swim over the members that climb before it (with exitReadyFirst a later slot may be one).
            // Nothing here caps a foot swim: the count reaches the watchdog only when that sum passes it, e.g. 39 m of
            // foot swim at footSwimSpeed with one member before it, about 12 m each with three; the worst case then is
            // the ordinary placing with the lead. The 11-19 replay test meets one such wait (0.17 s).
            s.watch = queueClear(i, lead) ? s.watch + dt : 0.f;
        } else {
            s.watch += dt;
        }
    }

    // The lead is out (with climbBehindLead: or that far into its ladder climb) and no un-held member before slot i is
    // still on its way to, or waiting at, its exit.
    bool queueClear(int i, const WaterFollowLead& lead) const {
        const bool behind =
            tuning_.climbBehindLead >= 0.f && leadExitAt_ >= 0.f && clock_ - leadExitAt_ >= tuning_.climbBehindLead;
        if (!lead.out && !behind) return false;
        for (int j = 0; j < i; ++j) {
            const WaterFollowPhase p = slots_[j].order.phase;
            // exitReadyFirst: one still swimming to its place does not hold up one already at its own.
            const bool waits = p == WaterFollowPhase::ExitWait || (p == WaterFollowPhase::ToExit && !tuning_.exitReadyFirst);
            if (!slots_[j].held && waits) return false;
        }
        return true;
    }

    static float dist3(const SwimVec& a, const SwimVec& b) {
        const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
        return std::sqrt(x * x + y * y + z * z);
    }

    static bool inWater(WaterFollowPhase p) {
        return p == WaterFollowPhase::Entering || p == WaterFollowPhase::Surface || p == WaterFollowPhase::DiveWait ||
               p == WaterFollowPhase::Under || p == WaterFollowPhase::ToExit || p == WaterFollowPhase::ExitWait ||
               p == WaterFollowPhase::Climbing;
    }

    static void go(WaterFollowOrder& o, WaterFollowPhase p) {
        if (o.phase == p) return;
        o.phase = p;
        o.changed = true;
        if (p == WaterFollowPhase::Ashore) {
            o.exitIndex = -1;
            o.entry = WaterFollowEntry{};
        }
        o.swim = p == WaterFollowPhase::Under ? SwimState::Under : SwimState::Surface;
    }

    // Its entry may start: the members before it have started theirs (or are not entering), and the stagger since the
    // last entry has run.
    bool entryTurn(int i) const {
        for (int j = 0; j < i; ++j) {
            const WaterFollowPhase p = slots_[j].order.phase;
            if (p == WaterFollowPhase::ToEntry) return false;
        }
        return lastEntryAt_ < 0.f || clock_ - lastEntryAt_ >= stagger(i);
    }

    // Its climb may start: the lead is out, every member before it has started its climb (or is not waiting to, or is
    // held by a scene and so out of the queue), and climbGap has run since the last one started.
    // With exitReadyFirst the order can change, so one nearer the ladder could reach its foot first: it also waits until
    // its swim there (`swim` s) lands it climbGap after the last climber's.
    bool climbTurn(int i, const WaterFollowLead& lead, float swim) const {
        if (!queueClear(i, lead)) return false;
        if (lastClimbAt_ >= 0.f && clock_ - lastClimbAt_ < tuning_.climbGap) return false;
        return !tuning_.exitReadyFirst || lastFootAt_ < 0.f || clock_ + swim >= lastFootAt_ + tuning_.climbGap;
    }

    // Its swim from where it is to its exit's foot on the climb path (0 for a flight, a mantle, or without climbPath).
    float footSwim(const WaterFollowOrder& o, const WaterFollowMember& m, const WaterExit* exits,
                   std::size_t exitCount) const {
        if (!tuning_.climbPath || tuning_.footSwimSpeed <= 0.f || o.exitIndex < 0 || std::size_t(o.exitIndex) >= exitCount)
            return 0.f;
        const WaterExit& e = exits[o.exitIndex];
        if (e.kind == WaterExitKind::Steps) return 0.f;
        return water_follow_detail::dist(Vec2{m.position.x, m.position.y}, e.foot) / tuning_.footSwimSpeed;
    }

    void beginExit(WaterFollowOrder& o, const WaterFollowLead& lead, const WaterFollowMember& m, const WaterExit* exits,
                   std::size_t exitCount) {
        int leadExit = lead.exitIndex;
        if (leadExit < 0 && tuning_.exitPickNear && lead.out)
            leadExit = waterFollowWalkedOut(exits, exitCount, Vec2{lead.position.x, lead.position.y});
        o.exitIndex = waterFollowExit(exits, exitCount, leadExit, Vec2{m.position.x, m.position.y}, tuning_.exitWithin,
                                      tuning_.exitPickNear);
        go(o, WaterFollowPhase::ToExit);
    }

    // The order's target and speed for its phase.
    void target(WaterFollowOrder& o, const WaterFollowLead& lead, const WaterFollowMember& m, int i,
                const WaterExit* exits, std::size_t exitCount, const WaterBodies* water) const {
        namespace d = water_follow_detail;
        const WaterFollowTuning& T = tuning_;
        const Vec2 here{m.position.x, m.position.y};
        const Vec2 leadP{lead.position.x, lead.position.y};
        const float leadSpeed = d::len(lead.velocity);
        // The lead's frame: its travel, or its facing when still.
        const Vec2 fwd = leadSpeed > 0.2f ? Vec2{lead.velocity.x / leadSpeed, lead.velocity.y / leadSpeed}
                                    : Vec2{std::cos(lead.yaw * 0.0174532925f), std::sin(lead.yaw * 0.0174532925f)};
        const Vec2 right{-fwd.y, fwd.x}; // the engine's frame: +Y is to the right of +X
        const float side = (i % 2 == 0) ? 1.f : -1.f; // even orders to the right, odd to the left
        o.speed = 0.f;
        o.sprint = false;
        o.target = m.position;
        switch (o.phase) {
        case WaterFollowPhase::Placed:
            // Placed while the lead swims (a teleport, the watchdog, a second failed entry), or in a start's hold: rejoin
            // at the surface slot.
            if (lead.swim != SwimState::Rescued && (lead.swimming || leadUnder_ || starting_)) {
                const Vec2 slot = inWaterToward(water, leadP,
                                                Vec2{leadP.x - fwd.x * T.slotBack + right.x * T.slotSide * side,
                                                     leadP.y - fwd.y * T.slotBack + right.y * T.slotSide * side});
                o.target = SwimVec{slot.x, slot.y, m.surfaceZ};
                o.rejoin = true;
            }
            break;
        case WaterFollowPhase::Ashore:
        case WaterFollowPhase::Entering:
        case WaterFollowPhase::Climbing:
            break; // the party's own follow, or the host's placed move
        case WaterFollowPhase::ToEntry:
            o.target = SwimVec{o.entry.point.x, o.entry.point.y, m.position.z};
            o.speed = -1.f; // the party's own walk speed
            break;
        case WaterFollowPhase::Surface:
        case WaterFollowPhase::DiveWait: {
            const Vec2 slot = inWaterToward(water, leadP,
                                            Vec2{leadP.x - fwd.x * T.slotBack + right.x * T.slotSide * side,
                                                 leadP.y - fwd.y * T.slotBack + right.y * T.slotSide * side});
            o.target = SwimVec{slot.x, slot.y, m.surfaceZ};
            const float off = d::dist(here, slot);
            const float cap = lead.sprint ? T.surfaceSprint : T.surfaceSpeed;
            if (off > T.slotSlack) {
                o.speed = cap * (1.f + T.catchUp);
            } else if (leadSpeed >= T.treadSpeed) {
                o.speed = leadSpeed < cap ? leadSpeed : cap;
            } else {
                o.speed = off > 0.1f ? T.treadSpeed : 0.f; // the lead treads: settle at the slot and tread
            }
            o.sprint = o.speed > T.surfaceSpeed;
            break;
        }
        case WaterFollowPhase::Under: {
            const float a = T.underSlotAngle * 0.0174532925f * side;
            // Back and to its side, at the band's middle, at the lead's depth.
            const Vec2 back{-fwd.x * std::cos(a) + right.x * std::sin(a), -fwd.y * std::cos(a) + right.y * std::sin(a)};
            const float mid = 0.5f * (T.underNear + T.underFar);
            SwimVec slot{lead.position.x + back.x * mid, lead.position.y + back.y * mid, lead.position.z};
            slot = keepOutOfShot(slot, lead.position, lead.camera, T.keepOut);
            const Vec2 in = inWaterToward(water, leadP, Vec2{slot.x, slot.y});
            slot.x = in.x;
            slot.y = in.y;
            // Under the surface and over the bed (the slot's, with the water; else the follower's).
            const float top = m.surfaceZ - T.underTop;
            const float bottom = (water ? m.surfaceZ - water->depth(in) : m.bedZ) + T.bedClearance;
            if (slot.z > top) slot.z = top;
            if (slot.z < bottom) slot.z = bottom < top ? bottom : top;
            o.target = slot;
            const float dx = m.position.x - lead.position.x, dy = m.position.y - lead.position.y,
                        dz = m.position.z - lead.position.z;
            const float r = std::sqrt(dx * dx + dy * dy + dz * dz);
            const bool inShot = segDist3(m.position, lead.camera, lead.position) < T.keepOut;
            const float cap = lead.sprint ? T.underSprint : T.underSpeed;
            if (r < T.underNear || r > T.underFar || inShot)
                o.speed = cap * (1.f + T.catchUp);
            else
                o.speed = 0.f; // inside the band and out of the shot: drift with the lead
            o.sprint = o.speed > T.underSpeed;
            break;
        }
        case WaterFollowPhase::ToExit:
        case WaterFollowPhase::ExitWait:
            if (o.exitIndex >= 0 && std::size_t(o.exitIndex) < exitCount) {
                const Vec2 f = exits[o.exitIndex].foot;
                // Wait a little out from the foot by order, so the queue does not stack on one point.
                const Vec2 out = exits[o.exitIndex].out;
                const float back = 1.2f * float(i);
                o.target = SwimVec{f.x - out.x * back, f.y - out.y * back, m.surfaceZ};
                const bool behind = d::dist(here, Vec2{o.target.x, o.target.y}) > T.slotSlack;
                o.speed = o.phase != WaterFollowPhase::ToExit ? 0.f
                          : T.exitCatchUp && behind ? T.surfaceSpeed * (1.f + T.catchUp) : T.surfaceSpeed;
                o.sprint = o.speed > T.surfaceSpeed;
            }
            break;
        }
    }

    // `want`, or the furthest point toward it from `from` with a swimming column (8 halvings); `from` itself if even
    // the first step out is too shallow. Without `water`: `want`.
    static Vec2 inWaterToward(const WaterBodies* water, Vec2 from, Vec2 want) {
        if (!water || water->depth(want) > kWaterSwimDepth) return want;
        float lo = 0.f, hi = 1.f;
        for (int k = 0; k < 8; ++k) {
            const float mid = 0.5f * (lo + hi);
            const Vec2 p{from.x + (want.x - from.x) * mid, from.y + (want.y - from.y) * mid};
            if (water->depth(p) > kWaterSwimDepth)
                lo = mid;
            else
                hi = mid;
        }
        return Vec2{from.x + (want.x - from.x) * lo, from.y + (want.y - from.y) * lo};
    }

    static float segDist3(const SwimVec& p, const SwimVec& a, const SwimVec& b) {
        const float abx = b.x - a.x, aby = b.y - a.y, abz = b.z - a.z;
        const float l2 = abx * abx + aby * aby + abz * abz;
        float t = l2 > 0.f ? ((p.x - a.x) * abx + (p.y - a.y) * aby + (p.z - a.z) * abz) / l2 : 0.f;
        t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
        const float x = p.x - (a.x + abx * t), y = p.y - (a.y + aby * t), z = p.z - (a.z + abz * t);
        return std::sqrt(x * x + y * y + z * z);
    }

    // A slot nearer the camera-to-lead segment than keepOut is pushed straight out from it to keepOut (plus a margin).
    static SwimVec keepOutOfShot(SwimVec slot, const SwimVec& lead, const SwimVec& cam, float keepOut) {
        const float abx = lead.x - cam.x, aby = lead.y - cam.y, abz = lead.z - cam.z;
        const float l2 = abx * abx + aby * aby + abz * abz;
        if (!(l2 > 0.f)) return slot;
        float t = ((slot.x - cam.x) * abx + (slot.y - cam.y) * aby + (slot.z - cam.z) * abz) / l2;
        t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
        const SwimVec c{cam.x + abx * t, cam.y + aby * t, cam.z + abz * t};
        float nx = slot.x - c.x, ny = slot.y - c.y, nz = slot.z - c.z;
        const float dd = std::sqrt(nx * nx + ny * ny + nz * nz);
        const float want = keepOut + 0.2f;
        if (dd >= keepOut) return slot;
        if (dd < 1e-4f) {
            // On the line: out sideways (horizontal, perpendicular to the view).
            const float h = std::sqrt(abx * abx + aby * aby);
            nx = h > 0.f ? aby / h : 1.f;
            ny = h > 0.f ? -abx / h : 0.f;
            nz = 0.f;
        } else {
            nx /= dd;
            ny /= dd;
            nz /= dd;
        }
        return SwimVec{c.x + nx * want, c.y + ny * want, c.z + nz * want};
    }

    WaterFollowTuning tuning_;
    std::uint32_t seed_;
    Slot slots_[kWaterFollowMax];
    float clock_ = 0.f;
    float leadSwim_ = 0.f;
    float leadDiveAt_ = -1.f;
    float lastEntryAt_ = -1.f;
    float lastClimbAt_ = -1.f;
    float lastFootAt_ = -1.f; // when the last climber reaches its exit's foot (exitReadyFirst's spacing)
    float leadExitAt_ = -1.f; // when the lead's ladder climb began (-1: not climbing)
    bool leadUnder_ = false;
    SwimVec lastLead_{};
    bool hasLastLead_ = false;
    float startLeft_ = 0.f; // s left of a start's hold (startInWater)
    bool starting_ = false; // this step is in it
};

// A lead switch (the party menu) while the lead is in the water (11-18 item 3): what the new party starts as. A lead
// swimming or under water starts the new party in the water (WaterFollowParty::startInWater, the new lead put at the
// surface); a lead on its way out of the water (a climb) or being rescued refuses the switch, as both move the lead by
// a path the new lead would not take over; anywhere else (wading, on land) the party starts ashore as before.
enum class WaterSwitchStart : std::uint8_t { Ashore, InWater, Refused };

inline WaterSwitchStart waterSwitchStart(bool swimming, bool under, bool climbingOut, bool rescued) {
    if (climbingOut || rescued) return WaterSwitchStart::Refused;
    return swimming || under ? WaterSwitchStart::InWater : WaterSwitchStart::Ashore;
}

} // namespace ftcore
