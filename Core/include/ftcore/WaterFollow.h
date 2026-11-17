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
};

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
// else a jump from `edge` (the bank point nearest where it stands, PartyShore's), landing `edgeOut` (unit, toward the
// water) a metre out.
inline WaterFollowEntry waterFollowEntry(const WaterExit* exits, std::size_t count, Vec2 from, Vec2 lead, Vec2 edge,
                                         Vec2 edgeOut, float within = 8.f) {
    namespace d = water_follow_detail;
    WaterFollowEntry e;
    float best = within;
    for (std::size_t i = 0; i < count; ++i) {
        if (exits[i].kind != WaterExitKind::Steps) continue;
        const float dd = d::segDist(exits[i].top, from, lead);
        if (dd <= best) {
            best = dd;
            e.kind = WaterFollowEntryKind::Steps;
            e.point = exits[i].top;
            e.into = exits[i].foot;
            e.exitIndex = int(i);
        }
    }
    if (e.kind == WaterFollowEntryKind::None) {
        e.kind = WaterFollowEntryKind::Jump;
        e.point = edge;
        e.into = Vec2{edge.x + edgeOut.x, edge.y + edgeOut.y};
    }
    return e;
}

// The exit: the lead's own (leadExit, -1 when it left by none) when its foot is within `within` m of the follower;
// else the exit whose foot is nearest. -1: there are none.
inline int waterFollowExit(const WaterExit* exits, std::size_t count, int leadExit, Vec2 at, float within = 25.f) {
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
    return best;
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
    bool active = false;        // a party follower in this slot
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
        leadUnder_ = false;
        hasLastLead_ = false;
    }
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
        const bool rescued = lead.swim == SwimState::Rescued;
        // Lost: the lead jumped (a scene, a fight or a teleport moved it), or it is back from a parked planner on dry
        // ground. The old plan is not resumed against it: every follower in the water is placed with the lead.
        const bool leadDry = !lead.swimming && !leadUnder;
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
            if (lost && o.phase == WaterFollowPhase::ToEntry) go(o, WaterFollowPhase::Ashore);
            // Lost, or stuck past the watchdog (it never reached its exit, its slot or the end of its move): placed.
            if (inWater(o.phase) && (lost || s.watch >= tuning_.watchdog)) {
                go(o, WaterFollowPhase::Placed);
                o.place = true;
            }
            switch (o.phase) {
            case WaterFollowPhase::Ashore:
                if (!m.held && !lead.exiting && leadSwim_ >= tuning_.trigger) { // mid-traversal it picks its entry; ToEntry waits
                    const Vec2 here{m.position.x, m.position.y};
                    o.entry = waterFollowEntry(exits, exitCount, here, Vec2{lead.position.x, lead.position.y}, m.edge,
                                               m.edgeOut, tuning_.stepsWithin);
                    // A jump needs the water's edge; with none found toward the lead it stays ashore.
                    if (o.entry.kind == WaterFollowEntryKind::Steps || m.hasEdge)
                        go(o, WaterFollowPhase::ToEntry);
                    else
                        o.entry = WaterFollowEntry{};
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
                if (o.phase == WaterFollowPhase::ExitWait && !m.held && climbTurn(i, lead)) {
                    go(o, WaterFollowPhase::Climbing);
                    o.startClimb = true;
                    s.climbAt = clock_;
                    lastClimbAt_ = clock_;
                }
                break;
            case WaterFollowPhase::Climbing:
                if (m.climbed) go(o, WaterFollowPhase::Ashore);
                break;
            case WaterFollowPhase::Placed: // held at least the step it was placed in, so the host sees it
                if (was == WaterFollowPhase::Placed && !rescued && !lead.swimming && !leadUnder)
                    go(o, WaterFollowPhase::Ashore);
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

private:
    struct Slot {
        WaterFollowOrder order;
        float entryAt = -1.f;
        float climbAt = -1.f;
        bool held = false;        // its member is held by a scene: out of the climb queue
        float backFor = 0.f;      // s the lead has swum away from its exit (the queue's return)
        WaterFollowPhase watchPhase = WaterFollowPhase::Ashore;
        float watch = 0.f;        // s stuck in watchPhase
        float bestGap = 0.f;      // m: ToExit's nearest to its exit's foot so far
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
        if (o.phase != s.watchPhase) {
            s.watchPhase = o.phase;
            s.watch = 0.f;
            s.bestGap = gap;
            return;
        }
        if (!inWater(o.phase) || m.held || (swim && gap <= tuning_.stuckFar)) {
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
            s.watch = queueClear(i, lead) ? s.watch + dt : 0.f;
        } else {
            s.watch += dt;
        }
    }

    // The lead is out and no un-held member before slot i is still on its way to, or waiting at, its exit.
    bool queueClear(int i, const WaterFollowLead& lead) const {
        if (!lead.out) return false;
        for (int j = 0; j < i; ++j) {
            const WaterFollowPhase p = slots_[j].order.phase;
            if (!slots_[j].held && (p == WaterFollowPhase::ToExit || p == WaterFollowPhase::ExitWait)) return false;
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
    bool climbTurn(int i, const WaterFollowLead& lead) const {
        if (!queueClear(i, lead)) return false;
        return lastClimbAt_ < 0.f || clock_ - lastClimbAt_ >= tuning_.climbGap;
    }

    void beginExit(WaterFollowOrder& o, const WaterFollowLead& lead, const WaterFollowMember& m, const WaterExit* exits,
                   std::size_t exitCount) {
        o.exitIndex = waterFollowExit(exits, exitCount, lead.exitIndex, Vec2{m.position.x, m.position.y},
                                      tuning_.exitWithin);
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
        case WaterFollowPhase::Ashore:
        case WaterFollowPhase::Entering:
        case WaterFollowPhase::Climbing:
        case WaterFollowPhase::Placed:
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
                o.speed = o.phase == WaterFollowPhase::ToExit ? T.surfaceSpeed : 0.f;
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
    bool leadUnder_ = false;
    SwimVec lastLead_{};
    bool hasLastLead_ = false;
};

} // namespace ftcore
