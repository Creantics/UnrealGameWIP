#include "ftcore/Crowd.h"

#include "SeedMix.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace ftcore {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = 180.f / kPi;

float dist(Vec2 a, Vec2 b) { return std::hypot(b.x - a.x, b.y - a.y); }
float dist2(Vec2 a, Vec2 b) { return (b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y); }

// Signed difference b - a in degrees, in (-180, 180].
float angleDiff(float a, float b) {
    float d = std::fmod(b - a, 360.f);
    if (d > 180.f) d -= 360.f;
    if (d <= -180.f) d += 360.f;
    return d;
}

// How far off a new edge's line a member may stand as it turns onto it (m).
constexpr float kCornerReach = 12.f;
// m: a threat this far to one side of a runner's street makes it veer across to the far pavement as it runs, at
// this share of its run speed sideways.
constexpr float kVeerOffset = 2.f;
constexpr float kVeerRate = 0.6f;
// s: for this long after it starts to run, a runner never steps sideways towards the threat.
constexpr float kFleeOpen = 1.f;
// m: how far inside the safe distance a recovering member may stand before it runs again.
constexpr float kSafeSlack = 6.f;
// m: the most a member runs past the safe distance (its own share, drawn at spawn).
constexpr float kSafeSpread = 15.f;
// S-286: how far short of a dead end's end node the first row of cornered members stands (m).
constexpr float kBraceEnd = 0.3f;
// m/s: how fast members standing too close step apart.
constexpr float kStandApart = 0.8f;
float yawTo(Vec2 from, Vec2 to) { return std::atan2(to.y - from.y, to.x - from.x) * kDeg; }

// Distance from p to the segment ab.
float segDist(Vec2 a, Vec2 b, Vec2 p) {
    const float ux = b.x - a.x, uy = b.y - a.y;
    const float l2 = ux * ux + uy * uy;
    const float t = l2 > 1e-12f ? std::clamp(((p.x - a.x) * ux + (p.y - a.y) * uy) / l2, 0.f, 1.f) : 0.f;
    return dist({a.x + ux * t, a.y + uy * t}, p);
}

// The kinds that make a member close by run whatever it sees or how bold it is.
bool fightClass(CrowdEventKind k) {
    return k == CrowdEventKind::FightStart || k == CrowdEventKind::Strike || k == CrowdEventKind::Gunshot ||
           k == CrowdEventKind::MagicBurst || k == CrowdEventKind::Death;
}

std::uint32_t floatBits(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    return u;
}

} // namespace

std::string_view crowdEventName(CrowdEventKind kind) {
    switch (kind) {
    case CrowdEventKind::FightStart: return "fight_start";
    case CrowdEventKind::Strike: return "strike";
    case CrowdEventKind::Gunshot: return "gunshot";
    case CrowdEventKind::MagicBurst: return "magic_burst";
    case CrowdEventKind::Death: return "death";
    case CrowdEventKind::LoudNoise: return "loud_noise";
    case CrowdEventKind::PlayerSprint: return "player_sprint";
    case CrowdEventKind::Panic: return "panic";
    case CrowdEventKind::Count: break;
    }
    return "";
}

std::string_view crowdStateName(CrowdState s) {
    switch (s) {
    case CrowdState::Free: return "free";
    case CrowdState::Walk: return "walk";
    case CrowdState::Idle: return "idle";
    case CrowdState::Notice: return "notice";
    case CrowdState::BackAway: return "back_away";
    case CrowdState::Flee: return "flee";
    case CrowdState::Cower: return "cower";
    case CrowdState::Recover: return "recover";
    }
    return "";
}

bool CrowdSim::init(const CrowdGraph& graph, const CrowdTuning& tuning, std::uint32_t seed) {
    graph_ = &graph;
    tuning_ = tuning;
    tuning_.step = std::max(tuning_.step, 1e-3f);
    tuning_.maxSteps = std::max<std::uint32_t>(tuning_.maxSteps, 1);
    tuning_.targetAgents = std::min(tuning_.targetAgents, tuning_.maxAgents);
    rng_ = detail::mixSeed(seed);
    agents_.assign(tuning_.maxAgents, Agent{});
    views_.assign(tuning_.maxAgents, CrowdAgentView{});
    freeList_.resize(tuning_.maxAgents);
    for (std::uint32_t i = 0; i < tuning_.maxAgents; ++i) freeList_[i] = tuning_.maxAgents - 1 - i;
    freeCount_ = tuning_.maxAgents;
    live_ = 0;
    eventCount_ = 0;
    gcell_ = std::max(2.f * tuning_.avoidRadius, std::max(tuning_.spreadRadius, 2.f));
    gdim_ = static_cast<std::uint32_t>(std::ceil(2.f * tuning_.despawnDist / gcell_)) + 1;
    gHead_.assign(static_cast<std::size_t>(gdim_) * gdim_, UINT32_MAX);
    gNext_.assign(tuning_.maxAgents, UINT32_MAX);
    sCost_.assign(graph.nodeCount(), 0.f);
    sPrev_.assign(graph.nodeCount(), UINT32_MAX);
    sStamp_.assign(graph.nodeCount(), 0);
    heap_.assign(static_cast<std::size_t>(graph.edgeCount()) + 8, 0);
    stamp_ = 0;
    order_.assign(tuning_.maxAgents, 0);
    counts_ = CrowdCounts{};
    time_ = 0.0;
    acc_ = 0.f;
    steps_ = 0;
    return graph.spawnableCount() > 0;
}

float CrowdSim::rand01() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<float>(rng_ >> 8) * (1.f / 16777216.f);
}

void CrowdSim::pushEvent(const CrowdEvent& e) {
    if (e.kind >= CrowdEventKind::Panic) return;
    if (eventCount_ >= kCrowdMaxEvents) {
        ++counts_.eventsDropped;
        return;
    }
    events_[eventCount_++] = e;
}

void CrowdSim::tick(float dt, Vec2 player, Vec2 playerVelocity) {
    if (!graph_ || agents_.empty()) return;
    if (std::isfinite(dt) && dt > 0.f) acc_ += dt;
    std::uint32_t n = 0;
    while (acc_ >= tuning_.step && n < tuning_.maxSteps) {
        step(player, playerVelocity);
        acc_ -= tuning_.step;
        ++n;
    }
    if (acc_ >= tuning_.step) acc_ = std::fmod(acc_, tuning_.step);
}

std::uint32_t CrowdSim::countIn(CrowdState s) const {
    std::uint32_t c = 0;
    for (const Agent& a : agents_) c += a.state == s ? 1u : 0u;
    return c;
}

std::uint64_t CrowdSim::digest() const {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](std::uint32_t v) {
        for (int k = 0; k < 4; ++k) {
            h ^= (v >> (8 * k)) & 0xffu;
            h *= 1099511628211ull;
        }
    };
    for (std::uint32_t i = 0; i < agents_.size(); ++i) {
        const Agent& a = agents_[i];
        if (a.state == CrowdState::Free) continue;
        mix(i);
        mix(static_cast<std::uint32_t>(a.state));
        mix(floatBits(a.pos.x));
        mix(floatBits(a.pos.y));
        mix(floatBits(a.fear));
    }
    return h;
}

float CrowdSim::scale(const Agent& a) const {
    // timidScale at boldness 0, 1 at 0.5, boldScale at 1.
    return a.boldness < 0.5f ? tuning_.timidScale + (1.f - tuning_.timidScale) * a.boldness * 2.f
                             : 1.f + (tuning_.boldScale - 1.f) * (a.boldness - 0.5f) * 2.f;
}

Vec2 CrowdSim::basePoint(const Agent& a) const {
    const Vec2 f = graph_->node(a.from), t = graph_->node(a.to);
    // along may start a little below 0 after a corner (enter keeps the member where it stands).
    const float u = std::min(a.along / a.len, 1.f);
    return {f.x + (t.x - f.x) * u, f.y + (t.y - f.y) * u};
}

void CrowdSim::setState(Agent& a, CrowdState s) {
    if (a.state == s) return;
    const bool wasCalm = a.state == CrowdState::Walk || a.state == CrowdState::Idle;
    const bool calm = s == CrowdState::Walk || s == CrowdState::Idle;
    if (wasCalm && !calm) a.walkHeading = a.heading;
    if (s != CrowdState::Walk) a.idleAlong = -1.f;
    if (s != CrowdState::Flee) a.stopAlong = -1.f;
    a.hold = false;
    a.brace = false;
    a.braceSlot = -1;
    switch (s) {
    case CrowdState::Notice: ++counts_.noticed; break;
    case CrowdState::BackAway: ++counts_.backedAway; break;
    case CrowdState::Flee:
        ++counts_.fled;
        if (a.fleeEntries++ == 0) ++counts_.fleers;
        counts_.maxFleeEntries = std::max<std::uint32_t>(counts_.maxFleeEntries, a.fleeEntries);
        break;
    case CrowdState::Cower: ++counts_.cowered; break;
    case CrowdState::Walk:
        if (a.state != CrowdState::Idle && a.state != CrowdState::Free) ++counts_.resumed;
        break;
    default: break;
    }
    a.state = s;
    a.stateTime = 0.f;
}

// Moves onto the edge from -> to, keeping the member where it stands: along and sideways are its own position in the
// new edge's frame (along may be a little below 0 at a corner), the side of the new street the nearer one.
void CrowdSim::enter(Agent& a, std::uint32_t from, std::uint32_t to) {
    const CrowdEdge* e = graph_->find(from, to);
    a.from = from;
    a.to = to;
    a.along = 0.f;
    a.len = e ? e->length : 1.f;
    a.lane = e ? e->lane : 0.f;
    a.laneHalf = e ? e->laneHalf : 0.f;
    const Vec2 f = graph_->node(from), t = graph_->node(to);
    const Vec2 dir{(t.x - f.x) / a.len, (t.y - f.y) / a.len};
    const Vec2 nrm{-dir.y, dir.x};
    const float lat = (a.pos.x - f.x) * nrm.x + (a.pos.y - f.y) * nrm.y;
    // Unclamped but for a sanity bound (the old street's half width at most, in practice): the lane smoothing brings
    // the member onto its pavement from wherever it stands, so the turn never jumps.
    a.along = std::clamp((a.pos.x - f.x) * dir.x + (a.pos.y - f.y) * dir.y, -kCornerReach, a.len);
    a.lat = std::clamp(lat, -kCornerReach, kCornerReach);
    a.side = lat >= 0.f ? 1.f : -1.f;
    a.latTarget = a.side * a.lane + (rand01() * 2.f - 1.f) * a.laneHalf * 0.8f;
}

bool CrowdSim::unCorner(Agent& a) {
    // Still rounding the corner before `from` (along < 0): it is on the street it came by. Put it back on that
    // street, heading for `from`, so turning round doesn't mirror it across the node. False (and left where it is) when
    // no other street at `from` runs its way.
    if (a.along >= 0.f) return true;
    const Vec2 F = graph_->node(a.from);
    const Vec2 off{a.pos.x - F.x, a.pos.y - F.y};
    std::uint32_t best = UINT32_MAX;
    float bestDot = 0.f;
    for (std::uint32_t k = 0; k < graph_->degree(a.from); ++k) {
        const CrowdEdge& e = graph_->edge(a.from, k);
        if (e.to == a.to) continue;
        const Vec2 w = graph_->node(e.to);
        const float d = ((w.x - F.x) * off.x + (w.y - F.y) * off.y) / std::max(e.length, 0.01f);
        if (d > bestDot) bestDot = d, best = e.to;
    }
    if (best == UINT32_MAX) return false; // S-286: never snapped onto the node (a 4 m jump, towards a fight there)
    const float latTarget = a.latTarget;
    enter(a, best, a.from);
    a.latTarget = a.side * std::fabs(latTarget);
    a.along = std::max(a.along, 0.f);
    return true;
}

void CrowdSim::reverse(Agent& a) {
    if (!unCorner(a)) {
        // Behind `from` with no street there running its way: out along another street at `from` (the one most its
        // way), heading off from `to`, kept exactly where it stands; at a dead end, onto the node as before.
        const Vec2 F = graph_->node(a.from);
        const Vec2 off{a.pos.x - F.x, a.pos.y - F.y};
        std::uint32_t best = UINT32_MAX;
        float bestDot = -1e30f;
        for (std::uint32_t k = 0; k < graph_->degree(a.from); ++k) {
            const CrowdEdge& e = graph_->edge(a.from, k);
            if (e.to == a.to) continue;
            const Vec2 w = graph_->node(e.to);
            const float d = ((w.x - F.x) * off.x + (w.y - F.y) * off.y) / std::max(e.length, 0.01f);
            if (d > bestDot) bestDot = d, best = e.to;
        }
        if (best != UINT32_MAX) {
            const float latTarget = a.latTarget;
            enter(a, a.from, best);
            a.latTarget = a.side * std::fabs(latTarget);
            return;
        }
        a.along = 0.f;
    }
    std::swap(a.from, a.to);
    a.along = std::max(0.f, a.len - a.along);
    a.lat = -a.lat;
    a.latTarget = -a.latTarget;
    a.side = -a.side;
}

std::uint32_t CrowdSim::awayFromThreat(const Agent& a) const {
    const std::uint32_t d = graph_->degree(a.to);
    std::uint32_t best = a.from;
    float bestD = -1.f;
    for (std::uint32_t k = 0; k < d; ++k) {
        const std::uint32_t w = graph_->edge(a.to, k).to;
        const float dw = dist2(graph_->node(w), a.threat);
        if (dw > bestD) bestD = dw, best = w;
    }
    return best;
}

std::uint32_t CrowdSim::chooseNext(Agent& a) {
    const std::uint32_t d = graph_->degree(a.to);
    if (d == 0) return a.from;
    if (d == 1) return graph_->edge(a.to, 0).to;
    if (rand01() < tuning_.headingChange) a.heading = rand01() * 360.f;
    const float hx = std::cos(a.heading / kDeg), hy = std::sin(a.heading / kDeg);
    const bool wary = time_ - a.threatAt < tuning_.threatMemory;
    const Vec2 here = graph_->node(a.to);
    const float hereD = dist(here, a.threat);
    std::uint32_t best = a.from;
    float bestS = -1e30f;
    for (std::uint32_t k = 0; k < d; ++k) {
        const CrowdEdge& e = graph_->edge(a.to, k);
        const Vec2 w = graph_->node(e.to);
        float s = ((w.x - here.x) * hx + (w.y - here.y) * hy) / e.length * tuning_.headingKeep +
                  rand01() * (1.f - tuning_.headingKeep);
        if (e.to == a.from) s -= 1.5f; // turning back only at a dead end, in effect
        if (wary) s += dist(w, a.threat) > hereD ? 1.f : -1.f;
        if (s > bestS) bestS = s, best = e.to;
    }
    return best;
}

std::uint32_t CrowdSim::spawnAt(std::uint32_t n, std::uint32_t k, float boldness, float along) {
    if (!graph_ || freeCount_ == 0 || n >= graph_->nodeCount() || k >= graph_->degree(n)) return UINT32_MAX;
    const std::uint32_t i = freeList_[--freeCount_];
    Agent& a = agents_[i];
    const std::uint16_t gen = static_cast<std::uint16_t>(a.gen + 1);
    a = Agent{};
    a.gen = gen;
    a.state = CrowdState::Walk;
    a.boldness = std::clamp(boldness, 0.f, 1.f);
    a.baseSpeed = std::max(tuning_.walkSpeed * 0.25f, tuning_.walkSpeed + (rand01() * 2.f - 1.f) * tuning_.walkSpeedJitter);
    a.heading = rand01() * 360.f;
    a.pos = graph_->node(n);
    enter(a, n, graph_->edge(n, k).to);
    a.lat = a.latTarget;
    a.along = std::clamp(along, 0.f, std::max(0.f, a.len - 0.01f));
    const Vec2 f = graph_->node(a.from), t = graph_->node(a.to);
    a.yaw = yawTo(f, t);
    const Vec2 dir{(t.x - f.x) / a.len, (t.y - f.y) / a.len};
    a.pos = {f.x + dir.x * a.along - dir.y * a.lat, f.y + dir.y * a.along + dir.x * a.lat};
    a.prev = a.pos;
    a.spreadLeft = rand01() * tuning_.spreadInterval;
    a.safeExtra = rand01() * kSafeSpread;
    if (rand01() < tuning_.idleChance) a.idleAlong = a.along + (a.len - a.along) * (0.2f + 0.6f * rand01());
    ++live_;
    ++counts_.spawned;
    writeView(i);
    return i;
}

void CrowdSim::free(std::uint32_t i) {
    Agent& a = agents_[i];
    a.state = CrowdState::Free;
    freeList_[freeCount_++] = i;
    --live_;
    ++counts_.despawned;
    views_[i].live = false;
    views_[i].drawn = false;
    views_[i].state = CrowdState::Free;
}

void CrowdSim::despawnFar(Vec2 player) {
    const float d2 = tuning_.despawnDist * tuning_.despawnDist;
    for (std::uint32_t i = 0; i < agents_.size(); ++i)
        if (agents_[i].state != CrowdState::Free && dist2(agents_[i].pos, player) > d2) free(i);
}

void CrowdSim::unspawn(std::uint32_t i) {
    agents_[i].state = CrowdState::Free;
    freeList_[freeCount_++] = i;
    --live_;
    --counts_.spawned;
    views_[i] = CrowdAgentView{};
}

bool CrowdSim::crowded(std::uint32_t self, Vec2 p, float r) const {
    bool hit = false;
    forEachNear(p, r, [&](std::uint32_t j) {
        if (j != self && agents_[j].state != CrowdState::Free) hit = true;
    });
    for (std::uint32_t b = 0; b < bornCount_ && !hit; ++b) hit = dist2(born_[b], p) < r * r;
    return hit;
}

void CrowdSim::spawnSome(Vec2 player) {
    bornCount_ = 0;
    const bool fill = live_ < tuning_.targetAgents / 2;
    const float rMin = fill ? tuning_.fillMin : tuning_.spawnMin;
    const float rMax = std::max(rMin + 1.f, tuning_.spawnMax);
    const std::uint32_t want = fill ? tuning_.spawnPerStep * 8 : tuning_.spawnPerStep;
    std::uint32_t made = 0;
    for (std::uint32_t tries = 0; tries < want * 4 && made < want && live_ < tuning_.targetAgents && freeCount_ > 0; ++tries) {
        const float ang = rand01() * 2.f * kPi;
        const float r = std::sqrt(rMin * rMin + rand01() * (rMax * rMax - rMin * rMin));
        const Vec2 p{player.x + std::cos(ang) * r, player.y + std::sin(ang) * r};
        const std::uint32_t n = graph_->nearest(p, 15.f);
        if (n == UINT32_MAX || !graph_->spawnable(n)) continue;
        const float d = dist(graph_->node(n), player);
        if (d < rMin || d > rMax) continue;
        const std::uint32_t deg = graph_->degree(n);
        const auto k = std::min(deg - 1, static_cast<std::uint32_t>(rand01() * static_cast<float>(deg)));
        const CrowdEdge& e = graph_->edge(n, k);
        // Somewhere along the street, not on the node: spawns that share a node would stand inside each other.
        const std::uint32_t i = spawnAt(n, k, rand01(), rand01() * e.length);
        if (i == UINT32_MAX) continue;
        if (crowded(i, agents_[i].pos, tuning_.avoidRadius * 1.5f)) {
            unspawn(i);
            continue;
        }
        if (bornCount_ < born_.size()) born_[bornCount_++] = agents_[i].pos;
        ++made;
    }
}

void CrowdSim::rebuildGrid(Vec2 player) {
    gorigin_ = {player.x - tuning_.despawnDist, player.y - tuning_.despawnDist};
    std::fill(gHead_.begin(), gHead_.end(), UINT32_MAX);
    for (std::uint32_t i = static_cast<std::uint32_t>(agents_.size()); i-- > 0;) {
        const Agent& a = agents_[i];
        if (a.state == CrowdState::Free) continue;
        const float fx = (a.pos.x - gorigin_.x) / gcell_, fy = (a.pos.y - gorigin_.y) / gcell_;
        if (fx < 0.f || fy < 0.f) continue;
        const auto x = static_cast<std::uint32_t>(fx), y = static_cast<std::uint32_t>(fy);
        if (x >= gdim_ || y >= gdim_) continue;
        const std::size_t c = static_cast<std::size_t>(y) * gdim_ + x;
        gNext_[i] = gHead_[c];
        gHead_[c] = i;
    }
}

template <class Fn> void CrowdSim::forEachNear(Vec2 p, float r, Fn&& fn) const {
    const float fx0 = (p.x - r - gorigin_.x) / gcell_, fy0 = (p.y - r - gorigin_.y) / gcell_;
    const float fx1 = (p.x + r - gorigin_.x) / gcell_, fy1 = (p.y + r - gorigin_.y) / gcell_;
    if (fx1 < 0.f || fy1 < 0.f) return;
    const auto lim = static_cast<float>(gdim_ - 1);
    const auto x0 = static_cast<std::uint32_t>(std::clamp(fx0, 0.f, lim)), x1 = static_cast<std::uint32_t>(std::clamp(fx1, 0.f, lim));
    const auto y0 = static_cast<std::uint32_t>(std::clamp(fy0, 0.f, lim)), y1 = static_cast<std::uint32_t>(std::clamp(fy1, 0.f, lim));
    const float r2 = r * r;
    for (std::uint32_t y = y0; y <= y1; ++y)
        for (std::uint32_t x = x0; x <= x1; ++x)
            for (std::uint32_t j = gHead_[static_cast<std::size_t>(y) * gdim_ + x]; j != UINT32_MAX; j = gNext_[j])
                if (dist2(agents_[j].pos, p) <= r2) fn(j);
}

void CrowdSim::perceive() {
    for (std::size_t e = 0; e < eventCount_; ++e) {
        const CrowdEvent& ev = events_[e];
        const CrowdEventRule& rule = tuning_.events[static_cast<std::size_t>(ev.kind)];
        const float radius = rule.radius * ev.radiusScale;
        const float intensity = rule.intensity * ev.intensityScale;
        if (radius <= 0.f || intensity <= 0.f) continue;
        for (std::uint32_t i = 0; i < agents_.size(); ++i) {
            Agent& a = agents_[i];
            if (a.state == CrowdState::Free) continue;
            const float d = dist(a.pos, ev.at);
            if (d > radius) continue;
            float fall = 1.f - d / radius;
            const bool close = d <= tuning_.reactRadius && fightClass(ev.kind);
            const bool felt = close || d <= tuning_.feelRadius;
            if (rule.sightOnly && !felt && std::fabs(angleDiff(a.yaw, yawTo(a.pos, ev.at))) > tuning_.fov * 0.5f) continue;
            if (occlude_ && !felt && occlusionLeft_ > 0) {
                --occlusionLeft_;
                ++counts_.occlusionQueries;
                if (occlude_(occludeCtx_, a.pos, ev.at)) {
                    if (rule.sightOnly) continue;
                    fall *= tuning_.occludedHearing;
                }
            }
            const float amount = intensity * fall;
            if (amount < 0.005f) continue;
            double delay = tuning_.reactBase + d * tuning_.reactPerMetre + rand01() * tuning_.reactJitter;
            if (close) delay = std::min<double>(delay, tuning_.reactCloseMax);
            const double at = time_ + delay;
            if (!a.pend) {
                a.pend = true;
                a.pendFear = amount;
                a.pendAt = at;
                a.pendFrom = ev.at;
                a.pendSpread = false;
                a.pendClose = close;
            } else {
                if (a.pendSpread || amount > a.pendFear) {
                    a.pendFear = a.pendSpread ? amount : std::max(a.pendFear, amount);
                    a.pendAt = std::min(a.pendAt, at);
                    a.pendFrom = ev.at;
                    if (a.pendSpread) a.pendClose = false;
                    a.pendSpread = false;
                }
                if (close) {
                    a.pendClose = true;
                    a.pendAt = std::min(a.pendAt, at);
                    a.pendFrom = ev.at;
                }
            }
        }
    }
    eventCount_ = 0;
}

void CrowdSim::deliver(Agent& a) {
    a.pend = false;
    if (a.pendSpread) {
        a.fear = std::max(a.fear, a.pendFear);
    } else {
        a.fear += a.pendFear;
        ++counts_.perceived;
    }
    // Close to a fight: run, whatever the boldness.
    if (a.pendClose) a.fear = std::max(a.fear, tuning_.fleeFear * scale(a) + 0.05f);
    a.pendClose = false;
    a.fear = std::min(a.fear, tuning_.fearMax);
    // A flee planned from an old threat replans when the threat moves.
    if (a.state == CrowdState::Flee && dist(a.pendFrom, a.planFrom) > 15.f) a.needPlan = true;
    a.threat = a.pendFrom;
    a.threatAt = time_;
}

// Dijkstra from the member's edge to the nearest node at least safeDist from the threat, nodes near the threat
// costing more; the first kCrowdPathMax nodes kept. With no such node within the search, the farthest node it settled,
// if that gains 10 m; otherwise false (cornered).
bool CrowdSim::plan(Agent& a) {
    ++counts_.plans;
    if (a.plans < UINT16_MAX) ++a.plans;
    counts_.maxPlans = std::max<std::uint32_t>(counts_.maxPlans, a.plans);
    unCorner(a);
    const float startD = dist(a.pos, a.threat);
    std::size_t hn = 0;
    std::uint32_t goal = UINT32_MAX, far = UINT32_MAX, settled = 0;
    const Vec2 base = basePoint(a);
    const Vec2 laneOff{a.pos.x - base.x, a.pos.y - base.y};
    {
        hn = 0;
        if (++stamp_ == 0) {
            std::fill(sStamp_.begin(), sStamp_.end(), 0);
            stamp_ = 1;
        }
        // Never through the fight: no step whose line passes within blockRadius of the threat, or, for a member already
        // inside it, closer than where it starts.
        auto push = [&](std::uint32_t node, float cost, std::uint32_t prev) {
            if (sStamp_[node] == stamp_ && sCost_[node] <= cost) return;
            // Never back along its own street: that end was its first step to take or turn down.
            if ((prev == a.from && node == a.to) || (prev == a.to && node == a.from)) return;
            // The member's own first step runs along its lane, beside the centre line, to the node.
            const Vec2 n0 = graph_->node(node);
            const Vec2 q = prev == UINT32_MAX ? Vec2{n0.x + laneOff.x, n0.y + laneOff.y} : n0;
            const Vec2 p0 = prev == UINT32_MAX ? a.pos : graph_->node(prev);
            // A step it takes in its first second never closer than where it starts, however far out it is (one way
            // along its street always opens the distance); a later one never within blockRadius, nor 0.5 m short of it.
            const bool first = prev == UINT32_MAX || dist(a.pos, graph_->node(prev)) < tuning_.runSpeed * kFleeOpen;
            const float clear = first ? startD - 0.05f : std::min(tuning_.blockRadius, startD - 0.5f);
            if (segDist(p0, q, a.threat) < clear) return;
            if (hn >= heap_.size()) return;
            sStamp_[node] = stamp_;
            sCost_[node] = cost;
            sPrev_[node] = prev;
            heap_[hn++] = (static_cast<std::uint64_t>(floatBits(cost)) << 32) | node;
            std::push_heap(heap_.begin(), heap_.begin() + static_cast<std::ptrdiff_t>(hn), std::greater<>());
        };
        const float penR = std::max(tuning_.threatPenaltyRadius, 0.01f);
        auto weight = [&](std::uint32_t node) {
            return 1.f + tuning_.threatPenalty * std::max(0.f, 1.f - dist(graph_->node(node), a.threat) / penR);
        };
        // The first step never heads closer to the threat when the other end leads away: a member near a fight turns
        // its back on it and runs, rather than passing it to reach a cheaper street.
        // Away: running along the lane towards that end opens the distance (not just an end farther off).
        const Vec2 F = graph_->node(a.from), T = graph_->node(a.to), off{a.pos.x - a.threat.x, a.pos.y - a.threat.y};
        const float run = (T.x - F.x) * off.x + (T.y - F.y) * off.y; // > 0: towards `to` opens it
        const bool fromAway = run < 0.f && dist(F, a.threat) > startD;
        const bool toAway = run > 0.f && dist(T, a.threat) > startD;
        if (fromAway || !toAway) push(a.from, std::max(a.along, 0.f) * weight(a.from), UINT32_MAX);
        if (toAway || !fromAway) push(a.to, (a.len - a.along) * weight(a.to), UINT32_MAX);
        goal = far = UINT32_MAX;
        settled = 0;
        float farD = startD + 10.f;
        while (hn > 0) {
            std::pop_heap(heap_.begin(), heap_.begin() + static_cast<std::ptrdiff_t>(hn), std::greater<>());
            const std::uint64_t top = heap_[--hn];
            const auto v = static_cast<std::uint32_t>(top & 0xffffffffu);
            float c;
            const auto cb = static_cast<std::uint32_t>(top >> 32);
            std::memcpy(&c, &cb, sizeof c);
            if (c > sCost_[v]) continue;
            const float dv = dist(graph_->node(v), a.threat);
            if (dv >= tuning_.safeDist + a.safeExtra) {
                goal = v;
                break;
            }
            if (dv > farD) farD = dv, far = v;
            if (++settled >= tuning_.searchLimit) break;
            for (std::uint32_t k = 0; k < graph_->degree(v); ++k) {
                const CrowdEdge& e = graph_->edge(v, k);
                push(e.to, c + e.length * weight(e.to), v);
            }
        }
    }
    bool partial = false;
    if (goal == UINT32_MAX) {
        if (far == UINT32_MAX) {
            ++counts_.planFails;
            return false;
        }
        goal = far;
        partial = true;
    }
    std::uint32_t len = 0;
    for (std::uint32_t v = goal; v != UINT32_MAX; v = sPrev_[v]) ++len;
    const std::uint32_t keep = std::min<std::uint32_t>(len, kCrowdPathMax);
    std::uint32_t idx = len;
    for (std::uint32_t v = goal; v != UINT32_MAX; v = sPrev_[v]) {
        --idx;
        if (idx < keep) a.path[idx] = v;
    }
    a.pathLen = static_cast<std::uint8_t>(keep);
    a.pathPos = 0;
    a.pathPartial = partial || len > keep;
    a.planFrom = a.threat;
    a.needPlan = false;
    a.stopAlong = -1.f;
    a.hold = false;
    if (a.path[0] == a.from) reverse(a);
    return true;
}

void CrowdSim::startFlee(Agent& a) {
    setState(a, CrowdState::Flee);
    a.pathLen = a.pathPos = 0;
    a.needPlan = true;
    if (plansLeft_ > 0) {
        --plansLeft_;
        if (!plan(a)) brace(a);
    }
}

void CrowdSim::brace(Agent& a) {
    // Cornered: no way out that keeps clear of the threat. Turn away, run to the far end of the dead end and cower
    // there (never past the threat).
    setState(a, CrowdState::Cower);
    a.brace = true;
    a.retryLeft = tuning_.cowerRetry;
}

// S-286: cornered members at the end of a dead end take spots of their own: a row across the width at the end (side
// walls included), then rows further back, each a minGap (or avoidRadius) apart; nobody stacks on the end node.
int CrowdSim::freeBraceSlot(std::uint32_t i) const {
    const Agent& a = agents_[i];
    std::uint64_t used = 0;
    for (std::uint32_t j = 0; j < agents_.size(); ++j) {
        const Agent& b = agents_[j];
        if (j != i && b.brace && b.braceSlot >= 0 && b.braceSlot < 64 && b.from == a.from && b.to == a.to) used |= 1ull << b.braceSlot;
    }
    for (int k = 0; k < 64; ++k)
        if (((used >> k) & 1ull) == 0) return k;
    return 63;
}

void CrowdSim::braceSlotAt(const Agent& a, float& along, float& lat) const {
    const CrowdEdge* e = graph_->find(a.from, a.to);
    const float w = std::max(0.f, (e ? e->half : a.lane + a.laneHalf) - tuning_.graph.wallInset);
    const float sp = std::max(tuning_.minGap, tuning_.avoidRadius);
    const int cols = 1 + static_cast<int>(2.f * w / sp);
    const int row = a.braceSlot / cols, col = a.braceSlot % cols;
    lat = -w + (2.f * w - static_cast<float>(cols - 1) * sp) * 0.5f + static_cast<float>(col) * sp;
    along = std::max(0.f, a.len - kBraceEnd - static_cast<float>(row) * sp);
}

void CrowdSim::decide(std::uint32_t i) {
    Agent& a = agents_[i];
    const float s = scale(a);
    const float notice = tuning_.noticeFear * s, back = tuning_.backFear * s, flee = tuning_.fleeFear * s;
    const float f = a.fear;
    a.stateTime += tuning_.step;
    switch (a.state) {
    case CrowdState::Walk:
    case CrowdState::Idle:
        if (f >= flee) startFlee(a);
        else if (f >= back) setState(a, CrowdState::BackAway);
        else if (f >= notice) setState(a, CrowdState::Notice);
        else if (time_ - a.threatAt < tuning_.threatMemory && time_ - a.threatAt > tuning_.quietAfter) {
            // It has gone quiet: one look back. Most walk back the way they came, through where it was (the street
            // refills); the rest walk on and stop steering clear of it.
            if (rand01() < tuning_.returnChance) {
                if (a.state == CrowdState::Idle) setState(a, CrowdState::Walk);
                a.heading = yawTo(a.pos, a.threat);
                if (dist2(graph_->node(a.to), a.threat) > dist2(graph_->node(a.from), a.threat)) reverse(a);
            }
            a.threatAt = time_ - tuning_.threatMemory;
        } else if (a.state == CrowdState::Idle && (a.idleLeft -= tuning_.step) <= 0.f) setState(a, CrowdState::Walk);
        break;
    case CrowdState::Notice:
        if (f >= flee) startFlee(a);
        else if (f >= back) setState(a, CrowdState::BackAway);
        else if (a.stateTime >= tuning_.noticeTime && f < notice) setState(a, CrowdState::Walk);
        break;
    case CrowdState::BackAway:
        if (f >= flee) startFlee(a);
        else if (a.stateTime >= tuning_.backTime && f < notice) setState(a, CrowdState::Recover);
        break;
    case CrowdState::Flee:
        if (a.needPlan && plansLeft_ > 0) {
            --plansLeft_;
            if (!plan(a)) brace(a);
        }
        break;
    case CrowdState::Cower:
        a.retryLeft -= tuning_.step;
        if (f >= flee && a.retryLeft <= 0.f && plansLeft_ > 0) {
            --plansLeft_;
            if (plan(a)) {
                a.cowerFails = 0;
                setState(a, CrowdState::Flee);
            } else {
                // Still cornered: back off (x2, x3, x4) so a cowering member doesn't search the streets every retry.
                if (a.cowerFails < 3) ++a.cowerFails;
                a.retryLeft = tuning_.cowerRetry * static_cast<float>(1 + a.cowerFails);
            }
        } else if (f < tuning_.calmFear && a.stateTime >= tuning_.calmTime) {
            setState(a, CrowdState::Recover);
        }
        break;
    case CrowdState::Recover:
        // Already at the safe distance from this threat: stay put rather than plan a run to where it stands.
        // The run ends at a node, the member a pavement's width off it: the slack keeps that from reading as inside.
        if (f >= flee && dist(a.pos, a.threat) < tuning_.safeDist - kSafeSlack) startFlee(a);
        else if (a.stateTime >= tuning_.recoverTime && f < std::max(tuning_.calmFear, notice)) {
            // Walk on. Once the threat has gone quiet, some walk back the way they ran, through where it was (the street
            // refills); of the rest about half carry on the way they were going, the others somewhere new.
            const float r = rand01();
            const bool quiet = time_ - a.threatAt > tuning_.quietAfter;
            const bool goBack = quiet && r < tuning_.returnChance;
            const float rest = quiet ? (r - tuning_.returnChance) / std::max(1e-3f, 1.f - tuning_.returnChance) : r;
            a.heading = goBack ? yawTo(a.pos, a.threat) : rest < tuning_.resumeHeading ? a.walkHeading : rand01() * 360.f;
            setState(a, CrowdState::Walk);
            if (goBack) {
                // Turned round on its street towards it, and no longer steering clear of it at the next corner.
                if (dist2(graph_->node(a.to), a.threat) > dist2(graph_->node(a.from), a.threat)) reverse(a);
                a.threatAt = std::min(a.threatAt, time_ - tuning_.threatMemory);
            }
        }
        break;
    case CrowdState::Free: break;
    }
}

void CrowdSim::move(std::uint32_t i, Vec2 player, Vec2 playerVel) {
    Agent& a = agents_[i];
    float speed = 0.f;
    float latRate = tuning_.laneSpeed;
    switch (a.state) {
    case CrowdState::Walk: speed = a.baseSpeed; break;
    case CrowdState::BackAway: speed = tuning_.backSpeed; break;
    case CrowdState::Flee:
        speed = tuning_.runSpeed * (a.baseSpeed / std::max(tuning_.walkSpeed, 0.1f));
        latRate *= 2.f;
        break;
    case CrowdState::Cower:
        if (a.brace) speed = tuning_.runSpeed * (a.baseSpeed / std::max(tuning_.walkSpeed, 0.1f));
        break;
    default: break;
    }
    if (a.hold) speed = 0.f;
    // A fight close by and about to react: it stops short where it is rather than walk on towards it.
    const bool startled = a.pend && a.pendClose && a.state == CrowdState::Walk;
    if (startled) speed = 0.f;
    // Moving away from the threat: keep the far end ahead while backing off or running before a plan.
    const bool away = a.state == CrowdState::BackAway || (a.state == CrowdState::Flee && a.pathLen == 0 && a.stopAlong < 0.f) ||
                      (a.state == CrowdState::Cower && a.brace);
    if (away && speed > 0.f) {
        // A cornered member runs the way that opens the distance at once (never round the threat to a farther end).
        const Vec2 f0 = graph_->node(a.from), t0 = graph_->node(a.to);
        const float run = (t0.x - f0.x) * (a.pos.x - a.threat.x) + (t0.y - f0.y) * (a.pos.y - a.threat.y);
        if (a.brace ? run < 0.f : dist2(t0, a.threat) < dist2(f0, a.threat)) reverse(a);
    }

    const float bandLo = a.lane > 0.f ? a.lane - a.laneHalf : -a.laneHalf, bandHi = a.lane + a.laneHalf;
    // S-286: running to the end of a dead end (its far node the last away from the threat): a spot of its own there.
    float slotAlong = -1.f, slotLat = 0.f;
    if (a.state == CrowdState::Cower && a.brace) {
        const bool end = dist2(graph_->node(awayFromThreat(a)), a.threat) < dist2(graph_->node(a.to), a.threat);
        if (!end) a.braceSlot = -1;
        else if (a.braceSlot < 0) a.braceSlot = freeBraceSlot(i);
        if (a.braceSlot >= 0) braceSlotAt(a, slotAlong, slotLat);
    }
    if (speed > 0.f && (a.state == CrowdState::Flee || a.brace)) {
        // Running from a threat off to one side of this street: veer across to the far pavement's outer edge as it
        // runs, so it opens the distance at once instead of running along the threat's flank.
        const Vec2 f = graph_->node(a.from), t = graph_->node(a.to);
        const float tl = ((a.threat.x - f.x) * -(t.y - f.y) + (a.threat.y - f.y) * (t.x - f.x)) / a.len;
        if (std::fabs(tl) > kVeerOffset) {
            // A spot of its own across the outer half of the far band (by slot), so runners that stop over there
            // don't stand in a file.
            const float u = static_cast<float>((i * 2654435761u >> 16) & 255u) / 255.f;
            a.side = tl > 0.f ? -1.f : 1.f;
            const float lo = std::max(bandLo, 0.f); // a street with no pavement: the outer half of its far side
            a.latTarget = a.side * (lo + (bandHi - lo) * (0.5f + 0.5f * u));
            latRate = std::max(latRate, speed * kVeerRate);
        }
    }
    if (speed > 0.f) {
        // Others and the player in the way or alongside: step aside, slow down.
        const Vec2 f = graph_->node(a.from), t = graph_->node(a.to);
        const Vec2 dir{(t.x - f.x) / a.len, (t.y - f.y) / a.len};
        const Vec2 nrm{-dir.y, dir.x};
        float push = 0.f, slow = 1.f;
        const float r = tuning_.avoidRadius;
        forEachNear(a.pos, r * 2.f, [&](std::uint32_t j) {
            if (j == i) return;
            const Vec2 d{agents_[j].pos.x - a.pos.x, agents_[j].pos.y - a.pos.y};
            const float ahead = d.x * dir.x + d.y * dir.y;
            const float side = d.x * nrm.x + d.y * nrm.y;
            if (std::fabs(side) > r) return;
            const float sgn = side > 0.f ? -1.f : side < 0.f ? 1.f : (j < i ? 1.f : -1.f);
            if (ahead > 0.f) {
                push += sgn;
                if (ahead < r && a.state == CrowdState::Walk) slow = 0.5f;
            } else if (ahead > -r) {
                push += sgn * 0.5f; // alongside: walkers don't walk inside each other either
            }
        });
        const Vec2 dp{player.x - a.pos.x, player.y - a.pos.y};
        const float reach = tuning_.avoidPlayer + std::hypot(playerVel.x, playerVel.y) * 0.5f;
        if (dp.x * dp.x + dp.y * dp.y < reach * reach) {
            const float side = dp.x * nrm.x + dp.y * nrm.y;
            push += side >= 0.f ? -2.f : 2.f;
        }
        if (push != 0.f) a.latTarget = a.side * std::clamp(a.side * a.latTarget + push * 0.25f * a.side, bandLo, bandHi);
        speed *= slow;
        // Off its band (just round a corner): back onto it at least as fast as it walks, so it doesn't cut the
        // corner through a wall.
        const float sl = a.side * a.lat;
        if (sl < bandLo - 0.05f || sl > bandHi + 0.05f) latRate = std::max(latRate, speed);
    } else if (a.state != CrowdState::Free && !startled) {
        // Standing (noticing, recovering, cowering, idling): members closer than avoidRadius step apart, along the
        // street and across the pavement band, so a crowd that stops in one place doesn't stand inside itself.
        const Vec2 f = graph_->node(a.from), t = graph_->node(a.to);
        const Vec2 dir{(t.x - f.x) / a.len, (t.y - f.y) / a.len};
        const Vec2 nrm{-dir.y, dir.x};
        float alongPush = 0.f, latPush = 0.f;
        const float r = tuning_.avoidRadius;
        forEachNear(a.pos, r, [&](std::uint32_t j) {
            if (j == i) return;
            Vec2 d{a.pos.x - agents_[j].pos.x, a.pos.y - agents_[j].pos.y};
            float d2 = d.x * d.x + d.y * d.y;
            if (d2 >= r * r) return;
            if (d2 < 1e-6f) { // exactly on top: split by slot order
                d = j < i ? dir : Vec2{-dir.x, -dir.y};
                d2 = 1.f;
            }
            const float w = (r - std::sqrt(d2)) / r;
            const float inv = 1.f / std::sqrt(d2);
            alongPush += (d.x * dir.x + d.y * dir.y) * inv * w;
            latPush += (d.x * nrm.x + d.y * nrm.y) * inv * w;
        });
        if (alongPush != 0.f || latPush != 0.f) {
            const float n = std::max(0.3f, std::hypot(alongPush, latPush)); // a slight overlap still clears quickly
            // (Still rounding a corner, along < 0: it stays where it is on that corner, not snapped onto the node.)
            a.along = std::clamp(a.along + alongPush / n * kStandApart * tuning_.step, std::min(a.along, 0.f),
                                 std::max(0.f, a.len - 0.01f));
            a.latTarget = a.side * std::clamp(a.side * (a.lat + latPush / n * kStandApart), bandLo, bandHi);
            latRate = std::max(latRate, kStandApart);
        }
    }

    if (slotAlong >= 0.f) {
        // Across to its spot as it runs, and stands there once it reaches its row.
        a.latTarget = slotLat;
        latRate = std::max(latRate, std::max(speed, tuning_.laneSpeed));
    }
    a.along += speed * tuning_.step;
    // A run that reached its goal goes on a little way down the next street and stops there; a calm walk that
    // means to idle stops partway along: either way, not on the node where the others stop too.
    bool stopped = false;
    if (slotAlong >= 0.f && a.along >= slotAlong) {
        if (a.along - speed * tuning_.step <= slotAlong) a.along = slotAlong; // reached its row this step
        a.hold = true;
        stopped = true;
    } else if (a.state == CrowdState::Flee && a.stopAlong >= 0.f && a.along >= a.stopAlong) {
        a.along = a.stopAlong;
        setState(a, CrowdState::Recover);
        stopped = true;
    } else if (a.state == CrowdState::Flee && speed > 0.f && a.stateTime >= kFleeOpen && a.along >= 0.f && a.along < a.len) {
        // S-286: far enough already, partway along a long street: it stops here rather than run on to the node at
        // its end (a 100 m street took runners a block past the safe distance, too far to walk back in half a minute).
        const Vec2 f0 = graph_->node(a.from), t0 = graph_->node(a.to);
        const Vec2 b0 = basePoint(a);
        const Vec2 at{b0.x - (t0.y - f0.y) / a.len * a.lat, b0.y + (t0.x - f0.x) / a.len * a.lat};
        if (dist(at, a.threat) >= tuning_.safeDist + a.safeExtra) {
            setState(a, CrowdState::Recover);
            a.pathLen = a.pathPos = 0;
            stopped = true;
        }
    } else if (a.state == CrowdState::Walk && a.idleAlong >= 0.f && a.along >= a.idleAlong) {
        a.along = a.idleAlong;
        setState(a, CrowdState::Idle);
        a.idleLeft = tuning_.idleMin + rand01() * (tuning_.idleMax - tuning_.idleMin);
        stopped = true;
    }
    for (int guard = 0; !stopped && a.along >= a.len && guard < 8; ++guard) {
        const float over = a.along - a.len;
        const std::uint32_t at = a.to;
        {
            const Vec2 f0 = graph_->node(a.from), t0 = graph_->node(at);
            a.pos = {t0.x - (t0.y - f0.y) / a.len * a.lat, t0.y + (t0.x - f0.x) / a.len * a.lat};
        }
        std::uint32_t next;
        bool settle = false;
        if (a.state == CrowdState::Flee && a.pathLen > 0) {
            ++a.pathPos;
            if (a.pathPos >= a.pathLen) {
                a.pathLen = a.pathPos = 0;
                if (a.pathPartial) a.needPlan = true;
                else settle = true;
                next = awayFromThreat(a);
            } else {
                next = a.path[a.pathPos];
            }
        } else if (away || a.state == CrowdState::Flee) {
            next = awayFromThreat(a);
        } else {
            next = chooseNext(a);
        }
        if ((away || settle) && dist2(graph_->node(next), a.threat) < dist2(graph_->node(at), a.threat)) {
            // Nowhere farther from the threat from here: stand at the node (a run asks for a plan, which may lead
            // back past it), rather than turn back and forth on the spot.
            a.along = a.len;
            if (settle) setState(a, CrowdState::Recover);
            else {
                a.hold = true;
                if (a.state == CrowdState::Flee) a.needPlan = true;
            }
            break;
        }
        enter(a, at, next);
        if (settle) a.stopAlong = std::min(a.len * (0.15f + 0.7f * rand01()), 2.f + rand01() * 10.f);
        else if (a.state == CrowdState::Walk && rand01() < tuning_.idleChance) a.idleAlong = a.len * (0.2f + 0.6f * rand01());
        a.along += over;
        if (settle && a.along >= a.stopAlong) {
            a.along = a.stopAlong;
            setState(a, CrowdState::Recover);
            break;
        }
    }

    // Sideways inside the lane, then the world position.
    float dl = a.latTarget - a.lat;
    if (startled || ((a.state == CrowdState::Flee || a.brace) && a.stateTime < kFleeOpen && (speed > 0.f || slotAlong >= 0.f))) {
        // About to react, or just started to run: never sideways towards the threat (back onto the band only once it
        // is clear).
        const Vec2 f0 = graph_->node(a.from), t0 = graph_->node(a.to);
        const Vec2 th = startled ? a.pendFrom : a.threat;
        const float tl = ((th.x - f0.x) * -(t0.y - f0.y) + (th.y - f0.y) * (t0.x - f0.x)) / a.len;
        if ((tl - a.lat) * dl > 0.f) dl = 0.f;
    }
    const float maxDl = latRate * tuning_.step;
    a.lat += std::clamp(dl, -maxDl, maxDl);
    const Vec2 f = graph_->node(a.from), t = graph_->node(a.to);
    const Vec2 dir{(t.x - f.x) / a.len, (t.y - f.y) / a.len};
    const Vec2 base = basePoint(a);
    a.pos = {base.x - dir.y * a.lat, base.y + dir.x * a.lat};
    a.speed = a.state == CrowdState::Recover || a.state == CrowdState::Idle ? 0.f : speed;

    // A moving member that hasn't got anywhere (a run check counts it): a runner in a whole second, anyone else
    // walking or backing off in two.
    const bool moving = (a.state == CrowdState::Flee || a.state == CrowdState::Walk || a.state == CrowdState::BackAway ||
                         (a.state == CrowdState::Cower && a.brace)) && !a.hold && !startled && speed > 0.f;
    if (moving) {
        a.stuckTime += tuning_.step;
        if (dist2(a.pos, a.stuckRef) > 0.01f) {
            a.stuckRef = a.pos;
            a.stuckTime = 0.f;
            a.stuckTurned = false;
        } else if (a.state == CrowdState::Flee && a.stuckTime >= 0.5f) {
            // Someone stands in its way at the end of its run: it stops behind them and asks for a new plan.
            a.hold = true;
            a.needPlan = true;
        } else if (a.state != CrowdState::Flee && a.stuckTime >= 1.f && !a.stuckTurned) {
            // Blocked (someone stood in its way at the pavement's edge): a walker turns back, a member backing off or
            // bracing stops where it is.
            a.stuckTurned = true;
            if (a.state == CrowdState::Walk) reverse(a);
            else a.hold = true;
        } else if (a.state == CrowdState::Walk && a.stuckTurned && a.stuckTime >= 0.5f) {
            // Boxed in both ways: it stands and waits for the way to clear.
            setState(a, CrowdState::Idle);
            a.idleLeft = tuning_.idleMin + rand01() * (tuning_.idleMax - tuning_.idleMin);
        } else if (a.stuckTime >= (a.state == CrowdState::Flee ? 1.f : 2.f)) {
            ++counts_.stuck;
            a.stuckTime = 0.f;
        }
    } else {
        a.stuckRef = a.pos;
        a.stuckTime = 0.f;
    }

    // Facing: the way it moves; towards the threat while noticing or backing off; held otherwise.
    float want = a.yaw;
    if (a.state == CrowdState::Notice || a.state == CrowdState::BackAway) want = yawTo(a.pos, a.threat);
    else if (a.state == CrowdState::Cower && a.brace && a.speed <= 0.f) want = yawTo(a.threat, a.pos); // back to it
    else if (a.speed > 0.f) want = std::atan2(dir.y, dir.x) * kDeg;
    const float turn = tuning_.turnRate * tuning_.step;
    a.yaw += std::clamp(angleDiff(a.yaw, want), -turn, turn);
    if (a.yaw > 180.f) a.yaw -= 360.f;
    if (a.yaw <= -180.f) a.yaw += 360.f;
}

void CrowdSim::spread(std::uint32_t i) {
    Agent& a = agents_[i];
    if (a.state != CrowdState::Flee) return;
    a.spreadLeft -= tuning_.step;
    if (a.spreadLeft > 0.f) return;
    a.spreadLeft = tuning_.spreadInterval;
    const float value = std::min(a.fear, tuning_.fearMax) * tuning_.spreadGain;
    if (value <= 0.f) return;
    forEachNear(a.pos, tuning_.spreadRadius, [&](std::uint32_t j) {
        if (j == i) return;
        Agent& b = agents_[j];
        if (b.state == CrowdState::Flee || b.state == CrowdState::Cower) return;
        if (value <= b.fear || (b.pend && value <= b.pendFear)) return;
        if (!b.pend) {
            b.pendAt = time_ + tuning_.reactBase + rand01() * tuning_.reactJitter;
            b.pendClose = false;
        }
        b.pend = true;
        b.pendFear = value;
        b.pendFrom = a.threat;
        b.pendSpread = true;
        ++counts_.spread;
    });
}

void CrowdSim::pickDrawn(Vec2 player) {
    if (focusOn_) player = focus_;
    std::uint32_t m = 0;
    for (std::uint32_t i = 0; i < agents_.size(); ++i) {
        views_[i].drawn = false;
        if (agents_[i].state == CrowdState::Free) continue;
        order_[m++] = (static_cast<std::uint64_t>(floatBits(dist2(agents_[i].pos, player))) << 32) | i;
    }
    const std::uint32_t k = std::min(m, tuning_.maxDrawn);
    if (k < m) std::nth_element(order_.begin(), order_.begin() + k, order_.begin() + m);
    for (std::uint32_t r = 0; r < k; ++r) views_[order_[r] & 0xffffffffu].drawn = true;
}

void CrowdSim::writeView(std::uint32_t i) {
    const Agent& a = agents_[i];
    CrowdAgentView& v = views_[i];
    v.id = i | (static_cast<std::uint32_t>(a.gen) << 16);
    v.live = a.state != CrowdState::Free;
    v.state = a.state;
    v.cornered = a.state == CrowdState::Cower && a.brace && a.hold;
    v.pos = a.pos;
    v.prevPos = a.prev;
    v.odo = a.odo;
    v.yaw = a.yaw;
    v.speed = a.speed;
    v.fear = a.fear;
    v.boldness = a.boldness;
    switch (a.state) {
    case CrowdState::Walk: v.anim = a.speed > 0.05f ? CrowdAnim::Walk : CrowdAnim::Idle; break;
    case CrowdState::Notice:
    case CrowdState::Recover: v.anim = CrowdAnim::Look; break;
    case CrowdState::BackAway: v.anim = a.speed > 0.05f ? CrowdAnim::BackStep : CrowdAnim::Look; break;
    case CrowdState::Flee: v.anim = a.speed > 0.05f ? CrowdAnim::Run : CrowdAnim::Look; break;
    case CrowdState::Cower: v.anim = a.speed > 0.05f ? CrowdAnim::Run : CrowdAnim::Cower; break;
    default: v.anim = CrowdAnim::Idle; break;
    }
}

void CrowdSim::step(Vec2 player, Vec2 playerVel) {
    time_ += tuning_.step;
    ++steps_;
    plansLeft_ = tuning_.plansPerStep;
    occlusionLeft_ = tuning_.occlusionBudget;
    for (Agent& a : agents_)
        if (a.state != CrowdState::Free) a.prev = a.pos;
    if (!frozen_) {
        despawnFar(player);
        spawnSome(player);
    }
    rebuildGrid(player);
    perceive();
    for (std::uint32_t i = 0; i < agents_.size(); ++i) {
        Agent& a = agents_[i];
        if (a.state == CrowdState::Free) continue;
        if (a.pend && time_ >= a.pendAt) deliver(a);
        float decay = a.state == CrowdState::Cower ? tuning_.cowerDecay : tuning_.fearDecay;
        // A quiet spell since the last stimulus: fear falls fast, so the street settles and refills.
        if (time_ - a.threatAt > tuning_.quietAfter) decay = std::max(decay, tuning_.quietDecay);
        a.fear = std::max(0.f, a.fear - decay * tuning_.step);
        decide(i);
        move(i, player, playerVel);
        spread(i);
    }
    separate();
    pickDrawn(player);
    for (std::uint32_t i = 0; i < agents_.size(); ++i)
        if (agents_[i].state != CrowdState::Free) {
            agents_[i].odo += dist(agents_[i].pos, agents_[i].prev);
            writeView(i);
        }
}

std::uint32_t CrowdSim::closePairs(float d) const {
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < agents_.size(); ++i) {
        if (agents_[i].state == CrowdState::Free) continue;
        forEachNear(agents_[i].pos, d, [&](std::uint32_t j) {
            if (j > i && agents_[j].state != CrowdState::Free && dist2(agents_[i].pos, agents_[j].pos) < d * d) ++n;
        });
    }
    return n;
}

// How far p stands inside the streets round a's edge (its own and those at both its nodes): the distance to the
// nearest outer edge. fixTo (optional): the nearest point wallInset inside the best of them.
float CrowdSim::clearance(const Agent& a, Vec2 p, Vec2* fixTo) const {
    float best = -1e30f;
    for (const std::uint32_t n : {a.from, a.to})
        for (std::uint32_t k = 0; k < graph_->degree(n); ++k) {
            const CrowdEdge& e = graph_->edge(n, k);
            const Vec2 s = graph_->node(n), q = graph_->node(e.to);
            const Vec2 d{q.x - s.x, q.y - s.y};
            const float l2 = std::max(d.x * d.x + d.y * d.y, 1e-6f);
            const float t = std::clamp(((p.x - s.x) * d.x + (p.y - s.y) * d.y) / l2, 0.f, 1.f);
            const Vec2 c{s.x + d.x * t, s.y + d.y * t};
            const float off = dist(p, c);
            const float cl = e.half - off;
            if (cl <= best) continue;
            best = cl;
            if (fixTo) {
                const float keep = std::max(0.f, e.half - tuning_.graph.wallInset);
                *fixTo = off > keep && off > 1e-6f ? Vec2{c.x + (p.x - c.x) * keep / off, c.y + (p.y - c.y) * keep / off} : p;
            }
        }
    return best;
}

// Moves a member to p, kept wallInset inside the streets: its along and sideways set from p in its edge's frame.
void CrowdSim::place(Agent& a, Vec2 p) {
    Vec2 q = p;
    if (clearance(a, p, &q) >= tuning_.graph.wallInset - 1e-3f) q = p;
    const Vec2 f = graph_->node(a.from), t = graph_->node(a.to);
    const Vec2 dir{(t.x - f.x) / a.len, (t.y - f.y) / a.len};
    a.along = std::clamp((q.x - f.x) * dir.x + (q.y - f.y) * dir.y, std::min(a.along, 0.f), a.len);
    a.lat = std::clamp((q.x - f.x) * -dir.y + (q.y - f.y) * dir.x, -kCornerReach, kCornerReach);
    const Vec2 base = basePoint(a);
    a.pos = {base.x - dir.y * a.lat, base.y + dir.x * a.lat};
    if (clearance(a, a.pos, nullptr) >= tuning_.graph.wallInset - 1e-3f) return;
    // Its own street's frame can't reach that point (it stands on a node): in across its own street until clear.
    float lo = 0.f, hi = 1.f;
    const float lat0 = a.lat;
    for (int k = 0; k < 10; ++k) {
        const float mid = 0.5f * (lo + hi);
        const Vec2 at{base.x - dir.y * lat0 * mid, base.y + dir.x * lat0 * mid};
        if (clearance(a, at, nullptr) >= tuning_.graph.wallInset - 1e-3f) lo = mid;
        else hi = mid;
    }
    a.lat = lat0 * lo;
    a.latTarget = a.lat;
    a.pos = {base.x - dir.y * a.lat, base.y + dir.x * a.lat};
}

// The part of a push a member takes: one reacting to a threat is never pushed towards it.
Vec2 CrowdSim::pushable(const Agent& a, Vec2 by) const {
    const bool wary = a.pend || (a.state != CrowdState::Walk && a.state != CrowdState::Idle);
    if (!wary) return by;
    const Vec2 th = a.pend ? a.pendFrom : a.threat;
    const float l = dist(a.pos, th);
    if (l <= 1e-4f) return by;
    const Vec2 u{(th.x - a.pos.x) / l, (th.y - a.pos.y) / l};
    const float c = by.x * u.x + by.y * u.y;
    return c > 0.f ? Vec2{by.x - u.x * c, by.y - u.y * c} : by;
}

// A push from a neighbour (unused: kept for the header).
void CrowdSim::nudge(Agent& a, Vec2 by) {
    const bool wary = a.pend || (a.state != CrowdState::Walk && a.state != CrowdState::Idle);
    if (wary) {
        const Vec2 th = a.pend ? a.pendFrom : a.threat;
        const float l = dist(a.pos, th);
        if (l > 1e-4f) {
            const Vec2 u{(th.x - a.pos.x) / l, (th.y - a.pos.y) / l};
            const float c = by.x * u.x + by.y * u.y;
            if (c > 0.f) by = {by.x - u.x * c, by.y - u.y * c};
        }
    }
    place(a, {a.pos.x + by.x, a.pos.y + by.y});
}

// S-271: no two members closer than minGap, and nobody nearer the frontage than wallInset, after every step.
void CrowdSim::separate() {
    const float g = tuning_.minGap;
    for (Agent& a : agents_)
        if (a.state != CrowdState::Free && clearance(a, a.pos, nullptr) < tuning_.graph.wallInset - 1e-3f) place(a, a.pos);
    for (int it = 0; it < 24 && g > 0.f; ++it) {
        bool any = false;
        for (std::uint32_t i = 0; i < agents_.size(); ++i) {
            if (agents_[i].state == CrowdState::Free) continue;
            // The grid is from the step's start: look a run step farther.
            forEachNear(agents_[i].pos, g + 1.f, [&](std::uint32_t j) {
                if (j == i || agents_[j].state == CrowdState::Free) return;
                Agent& a = agents_[i];
                Agent& b = agents_[j];
                Vec2 d{a.pos.x - b.pos.x, a.pos.y - b.pos.y};
                float l = std::sqrt(d.x * d.x + d.y * d.y);
                if (l >= g) return;
                any = true;
                if (l < 1e-4f) { // on top of each other: apart across the street, by slot order
                    const Vec2 f = graph_->node(a.from), t = graph_->node(a.to);
                    d = {-(t.y - f.y) / a.len, (t.x - f.x) / a.len};
                    if (i < j) d = {-d.x, -d.y};
                } else {
                    d = {d.x / l, d.y / l};
                }
                const float push = (g - l) * 0.5f + 1e-3f;
                const Vec2 wa{d.x * push, d.y * push};
                const Vec2 pa = pushable(a, wa);
                const Vec2 wb{-d.x * push - (wa.x - pa.x), -d.y * push - (wa.y - pa.y)};
                const Vec2 pb = pushable(b, wb);
                const Vec2 ra = pushable(a, {pa.x - (wb.x - pb.x), pa.y - (wb.y - pb.y)});
                place(a, {a.pos.x + ra.x, a.pos.y + ra.y});
                place(b, {b.pos.x + pb.x, b.pos.y + pb.y});
                const float l2 = dist(a.pos, b.pos);
                if (l2 >= g - 1e-3f) return;
                // Still too close (one stands at a dead end, the other may not step towards its threat): side by side
                // instead, whichever way round leaves them farther apart.
                const Vec2 n{-d.y, d.x};
                const float side = (g - l2) * 0.5f + 1e-3f;
                Agent best[2] = {a, b};
                float bestL = l2;
                for (int sgn = -1; sgn <= 1; sgn += 2) {
                    Agent ta = a, tb = b;
                    const float k = side * static_cast<float>(sgn);
                    const Vec2 sa = pushable(ta, {n.x * k, n.y * k}), sb = pushable(tb, {-n.x * k, -n.y * k});
                    place(ta, {ta.pos.x + sa.x, ta.pos.y + sa.y});
                    place(tb, {tb.pos.x + sb.x, tb.pos.y + sb.y});
                    const float lt = dist(ta.pos, tb.pos);
                    if (lt > bestL) { bestL = lt; best[0] = ta; best[1] = tb; }
                }
                a = best[0];
                b = best[1];
                if (bestL >= g - 1e-3f) return;
                // Last resort: apart, threat or not (two people never stand inside each other): straight apart or side by
                // side (straight apart may run into the end of a street), whichever leaves them farther apart. Not in the
                // first second of a reaction: nobody steps towards what they just heard.
                auto fresh = [&](const Agent& m) { return m.pend || (m.state != CrowdState::Walk && m.state != CrowdState::Idle && m.stateTime < 1.f); };
                if (fresh(a) || fresh(b)) return;
                const float rest = (g - bestL) * 0.5f + 1e-3f;
                Vec2 e{a.pos.x - b.pos.x, a.pos.y - b.pos.y};
                const float le = std::sqrt(e.x * e.x + e.y * e.y);
                e = le > 1e-4f ? Vec2{e.x / le, e.y / le} : d;
                const Vec2 ways[3] = {e, {-e.y, e.x}, {e.y, -e.x}};
                const Agent a0 = a, b0 = b;
                for (const Vec2& w : ways) {
                    Agent ta = a0, tb = b0;
                    place(ta, {ta.pos.x + w.x * rest, ta.pos.y + w.y * rest});
                    place(tb, {tb.pos.x - w.x * rest, tb.pos.y - w.y * rest});
                    float lt = dist(ta.pos, tb.pos);
                    if (lt < g - 1e-3f) {
                        // One stands at the end of its street: the other takes the rest.
                        const float more = g - lt + 1e-3f;
                        Agent ua = ta, ub = tb;
                        place(ua, {ua.pos.x + w.x * more, ua.pos.y + w.y * more});
                        place(ub, {ub.pos.x - w.x * more, ub.pos.y - w.y * more});
                        const float la = dist(ua.pos, tb.pos), lb = dist(ta.pos, ub.pos);
                        if (la >= lb && la > lt) { ta = ua; lt = la; }
                        else if (lb > lt) { tb = ub; lt = lb; }
                    }
                    if (lt > bestL) { bestL = lt; a = ta; b = tb; }
                }
            });
        }
        if (!any) break;
    }
}


float CrowdSim::minPairDistance(float within) const {
    float best2 = within * within;
    for (std::uint32_t i = 0; i < agents_.size(); ++i) {
        if (agents_[i].state == CrowdState::Free) continue;
        forEachNear(agents_[i].pos, within, [&](std::uint32_t j) {
            if (j > i && agents_[j].state != CrowdState::Free) best2 = std::min(best2, dist2(agents_[i].pos, agents_[j].pos));
        });
    }
    return std::sqrt(best2);
}

float CrowdSim::wallClearance(std::uint32_t i) const {
    const Agent& a = agents_[i];
    float best = -1e30f;
    auto segment = [&](std::uint32_t u, const CrowdEdge& e) {
        const Vec2 p = graph_->node(u), q = graph_->node(e.to);
        const Vec2 d{q.x - p.x, q.y - p.y};
        const float l2 = std::max(d.x * d.x + d.y * d.y, 1e-6f);
        const float t = std::clamp(((a.pos.x - p.x) * d.x + (a.pos.y - p.y) * d.y) / l2, 0.f, 1.f);
        best = std::max(best, e.half - dist(a.pos, {p.x + d.x * t, p.y + d.y * t}));
    };
    for (const std::uint32_t n : {a.from, a.to})
        for (std::uint32_t k = 0; k < graph_->degree(n); ++k) segment(n, graph_->edge(n, k));
    return best;
}

void CrowdReactionProbe::begin(const CrowdSim& sim, Vec2 at, float radius) {
    at_ = at;
    n_ = 0;
    for (std::uint32_t i = 0; i < sim.capacity() && n_ < kCrowdProbeMax; ++i) {
        const CrowdAgentView& v = sim.agent(i);
        if (!v.live) continue;
        const float d = dist(v.pos, at);
        if (d > radius) continue;
        id_[n_] = v.id;
        d0_[n_] = d;
        odo0_[n_] = v.odo;
        p0_[n_] = v.pos;
        ++n_;
    }
}

CrowdReactionResult CrowdReactionProbe::evaluate(const CrowdSim& sim, float minPath, float minRadial) const {
    CrowdReactionResult r;
    r.worstGain = r.worstPath = 1e30f;
    for (std::uint32_t k = 0; k < n_; ++k) {
        const CrowdAgentView& v = sim.agent(id_[k] & 0xffffu);
        if (!v.live || v.id != id_[k]) continue;
        ++r.inside;
        const float gain = dist(v.pos, at_) - d0_[k];
        const float path = v.odo - odo0_[k];
        r.worstGain = std::min(r.worstGain, gain);
        r.worstPath = std::min(r.worstPath, path);
        const bool facing = dist(v.pos, p0_[k]) > 0.01f && std::fabs(angleDiff(v.yaw, yawTo(p0_[k], v.pos))) <= 90.f;
        if (path >= minPath && facing && gain > minRadial) ++r.away;
        else if (v.cornered && gain >= 0.f) ++r.cornered;
        if (gain < 0.f) ++r.toward;
        if (std::fabs(angleDiff(v.yaw, yawTo(at_, v.pos))) <= 90.f) ++r.turned;
    }
    if (r.inside == 0) r.worstGain = r.worstPath = 0.f;
    return r;
}

} // namespace ftcore
