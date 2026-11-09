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

float yawTo(Vec2 from, Vec2 to) { return std::atan2(to.y - from.y, to.x - from.x) * kDeg; }

// Distance from p to the segment ab.
float segDist(Vec2 a, Vec2 b, Vec2 p) {
    const float ux = b.x - a.x, uy = b.y - a.y;
    const float l2 = ux * ux + uy * uy;
    const float t = l2 > 1e-12f ? std::clamp(((p.x - a.x) * ux + (p.y - a.y) * uy) / l2, 0.f, 1.f) : 0.f;
    return dist({a.x + ux * t, a.y + uy * t}, p);
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
    acc_ += std::max(dt, 0.f);
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
    const float u = std::clamp(a.along / a.len, 0.f, 1.f);
    return {f.x + (t.x - f.x) * u, f.y + (t.y - f.y) * u};
}

void CrowdSim::setState(Agent& a, CrowdState s) {
    if (a.state == s) return;
    switch (s) {
    case CrowdState::Notice: ++counts_.noticed; break;
    case CrowdState::BackAway: ++counts_.backedAway; break;
    case CrowdState::Flee: ++counts_.fled; break;
    case CrowdState::Cower: ++counts_.cowered; break;
    case CrowdState::Walk:
        if (a.state != CrowdState::Idle && a.state != CrowdState::Free) ++counts_.resumed;
        break;
    default: break;
    }
    a.state = s;
    a.stateTime = 0.f;
}

// Moves onto the edge from -> to (along 0), keeping the member where it stands sideways: the side of the new street
// nearer to it, the lateral offset continuous.
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
    const float reach = a.lane + a.laneHalf;
    a.lat = std::clamp(lat, -reach, reach);
    a.side = lat >= 0.f ? 1.f : -1.f;
    a.latTarget = a.side * a.lane + (rand01() * 2.f - 1.f) * a.laneHalf * 0.8f;
}

void CrowdSim::reverse(Agent& a) {
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

std::uint32_t CrowdSim::spawnAt(std::uint32_t n, std::uint32_t k, float boldness) {
    if (!graph_ || freeCount_ == 0 || n >= graph_->nodeCount() || k >= graph_->degree(n)) return UINT32_MAX;
    const std::uint32_t i = freeList_[--freeCount_];
    Agent& a = agents_[i];
    const std::uint16_t gen = static_cast<std::uint16_t>(a.gen + 1);
    a = Agent{};
    a.gen = gen;
    a.state = CrowdState::Walk;
    a.boldness = std::clamp(boldness, 0.f, 1.f);
    a.baseSpeed = std::max(0.3f, tuning_.walkSpeed + (rand01() * 2.f - 1.f) * tuning_.walkSpeedJitter);
    a.heading = rand01() * 360.f;
    a.pos = graph_->node(n);
    enter(a, n, graph_->edge(n, k).to);
    a.lat = a.latTarget;
    const Vec2 t = graph_->node(a.to);
    a.yaw = yawTo(a.pos, t);
    const Vec2 nrm{-(t.y - a.pos.y) / a.len, (t.x - a.pos.x) / a.len};
    a.pos = {a.pos.x + nrm.x * a.lat, a.pos.y + nrm.y * a.lat};
    a.prev = a.pos;
    a.spreadLeft = rand01() * tuning_.spreadInterval;
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

void CrowdSim::spawnSome(Vec2 player) {
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
        if (spawnAt(n, k, rand01()) != UINT32_MAX) ++made;
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
            const bool felt = d <= tuning_.feelRadius;
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
            const double at = time_ + tuning_.reactBase + d * tuning_.reactPerMetre + rand01() * tuning_.reactJitter;
            if (!a.pend) {
                a.pend = true;
                a.pendFear = amount;
                a.pendAt = at;
                a.pendFrom = ev.at;
                a.pendSpread = false;
            } else if (a.pendSpread || amount > a.pendFear) {
                a.pendFear = a.pendSpread ? amount : std::max(a.pendFear, amount);
                a.pendAt = std::min(a.pendAt, at);
                a.pendFrom = ev.at;
                a.pendSpread = false;
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
    if (++stamp_ == 0) {
        std::fill(sStamp_.begin(), sStamp_.end(), 0);
        stamp_ = 1;
    }
    std::size_t hn = 0;
    const float startD = dist(a.pos, a.threat);
    // Never through the fight: no step whose line passes within blockRadius of the threat. A member already inside
    // it may still take a first step that never brings it closer.
    const float startLimit = std::min(tuning_.blockRadius, startD - 0.5f);
    auto push = [&](std::uint32_t node, float cost, std::uint32_t prev) {
        if (sStamp_[node] == stamp_ && sCost_[node] <= cost) return;
        const Vec2 q = graph_->node(node);
        if (prev == UINT32_MAX ? segDist(a.pos, q, a.threat) < startLimit
                               : segDist(graph_->node(prev), q, a.threat) < tuning_.blockRadius)
            return;
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
    push(a.from, a.along * weight(a.from), UINT32_MAX);
    push(a.to, (a.len - a.along) * weight(a.to), UINT32_MAX);
    std::uint32_t goal = UINT32_MAX, far = UINT32_MAX, settled = 0;
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
        if (dv >= tuning_.safeDist) {
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
    if (a.path[0] == a.from) reverse(a);
    return true;
}

void CrowdSim::startFlee(Agent& a) {
    setState(a, CrowdState::Flee);
    a.pathLen = a.pathPos = 0;
    a.needPlan = true;
    if (plansLeft_ > 0) {
        --plansLeft_;
        if (!plan(a)) {
            setState(a, CrowdState::Cower);
            a.retryLeft = tuning_.cowerRetry;
        }
    }
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
        else if (a.state == CrowdState::Idle && (a.idleLeft -= tuning_.step) <= 0.f) setState(a, CrowdState::Walk);
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
            if (!plan(a)) {
                setState(a, CrowdState::Cower);
                a.retryLeft = tuning_.cowerRetry;
            }
        }
        break;
    case CrowdState::Cower:
        a.retryLeft -= tuning_.step;
        if (f >= flee && a.retryLeft <= 0.f && plansLeft_ > 0) {
            --plansLeft_;
            a.retryLeft = tuning_.cowerRetry;
            if (plan(a)) setState(a, CrowdState::Flee);
        } else if (f < tuning_.calmFear && a.stateTime >= tuning_.calmTime) {
            setState(a, CrowdState::Recover);
        }
        break;
    case CrowdState::Recover:
        if (f >= flee) startFlee(a);
        else if (a.stateTime >= tuning_.recoverTime && f < std::max(tuning_.calmFear, notice)) setState(a, CrowdState::Walk);
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
    default: break;
    }
    // Moving away from the threat: keep the far end ahead while backing off or running before a plan.
    const bool away = a.state == CrowdState::BackAway || (a.state == CrowdState::Flee && a.pathLen == 0);
    if (away && speed > 0.f && dist2(graph_->node(a.to), a.threat) < dist2(graph_->node(a.from), a.threat)) reverse(a);

    // Others and the player in the way: step aside, slow down.
    if (speed > 0.f) {
        const Vec2 f = graph_->node(a.from), t = graph_->node(a.to);
        const Vec2 dir{(t.x - f.x) / a.len, (t.y - f.y) / a.len};
        const Vec2 nrm{-dir.y, dir.x};
        float push = 0.f, slow = 1.f;
        const float look = tuning_.avoidRadius * 2.f;
        forEachNear(a.pos, look, [&](std::uint32_t j) {
            if (j == i) return;
            const Vec2 d{agents_[j].pos.x - a.pos.x, agents_[j].pos.y - a.pos.y};
            const float ahead = d.x * dir.x + d.y * dir.y;
            const float side = d.x * nrm.x + d.y * nrm.y;
            if (ahead <= 0.f || std::fabs(side) > tuning_.avoidRadius) return;
            push += side >= 0.f ? -1.f : 1.f;
            if (ahead < tuning_.avoidRadius && a.state == CrowdState::Walk) slow = 0.5f;
        });
        const Vec2 dp{player.x - a.pos.x, player.y - a.pos.y};
        const float reach = tuning_.avoidPlayer + std::hypot(playerVel.x, playerVel.y) * 0.5f;
        if (dp.x * dp.x + dp.y * dp.y < reach * reach) {
            const float side = dp.x * nrm.x + dp.y * nrm.y;
            push += side >= 0.f ? -2.f : 2.f;
        }
        if (push != 0.f) {
            const float lim = a.lane + a.laneHalf;
            a.latTarget = std::clamp(a.latTarget + push * 0.25f, -lim, lim);
        }
        speed *= slow;
    }

    a.along += speed * tuning_.step;
    for (int guard = 0; a.along >= a.len && guard < 8; ++guard) {
        const float over = a.along - a.len;
        const std::uint32_t at = a.to;
        {
            const Vec2 f0 = graph_->node(a.from), t0 = graph_->node(at);
            a.pos = {t0.x - (t0.y - f0.y) / a.len * a.lat, t0.y + (t0.x - f0.x) / a.len * a.lat};
        }
        std::uint32_t next;
        if (a.state == CrowdState::Flee && a.pathLen > 0) {
            ++a.pathPos;
            if (a.pathPos >= a.pathLen) {
                a.pathLen = a.pathPos = 0;
                if (a.pathPartial) a.needPlan = true;
                else {
                    setState(a, CrowdState::Recover);
                    a.along = a.len;
                    break;
                }
                next = awayFromThreat(a);
            } else {
                next = a.path[a.pathPos];
            }
        } else if (away) {
            next = awayFromThreat(a);
        } else {
            next = chooseNext(a);
            if (a.state == CrowdState::Walk && rand01() < tuning_.idleChance) {
                setState(a, CrowdState::Idle);
                a.idleLeft = tuning_.idleMin + rand01() * (tuning_.idleMax - tuning_.idleMin);
            }
        }
        enter(a, at, next);
        a.along = a.state == CrowdState::Idle ? 0.f : over;
        if (a.state == CrowdState::Idle) break;
    }

    // Sideways inside the lane, then the world position.
    const float dl = a.latTarget - a.lat;
    const float maxDl = latRate * tuning_.step;
    a.lat += std::clamp(dl, -maxDl, maxDl);
    const Vec2 f = graph_->node(a.from), t = graph_->node(a.to);
    const Vec2 dir{(t.x - f.x) / a.len, (t.y - f.y) / a.len};
    const Vec2 base = basePoint(a);
    a.pos = {base.x - dir.y * a.lat, base.y + dir.x * a.lat};
    a.speed = speed;

    // Facing: the way it moves; towards the threat while noticing or backing off; held otherwise.
    float want = a.yaw;
    if (a.state == CrowdState::Notice || a.state == CrowdState::BackAway) want = yawTo(a.pos, a.threat);
    else if (speed > 0.f) want = std::atan2(dir.y, dir.x) * kDeg;
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
        if (!b.pend) b.pendAt = time_ + tuning_.reactBase + rand01() * tuning_.reactJitter;
        b.pend = true;
        b.pendFear = value;
        b.pendFrom = a.threat;
        b.pendSpread = true;
        ++counts_.spread;
    });
}

void CrowdSim::pickDrawn(Vec2 player) {
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
    v.pos = a.pos;
    v.prevPos = a.prev;
    v.yaw = a.yaw;
    v.speed = a.speed;
    v.fear = a.fear;
    v.boldness = a.boldness;
    switch (a.state) {
    case CrowdState::Walk: v.anim = a.speed > 0.05f ? CrowdAnim::Walk : CrowdAnim::Idle; break;
    case CrowdState::Notice:
    case CrowdState::Recover: v.anim = CrowdAnim::Look; break;
    case CrowdState::BackAway: v.anim = CrowdAnim::BackStep; break;
    case CrowdState::Flee: v.anim = CrowdAnim::Run; break;
    case CrowdState::Cower: v.anim = CrowdAnim::Cower; break;
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
        const float decay = a.state == CrowdState::Cower ? tuning_.cowerDecay : tuning_.fearDecay;
        a.fear = std::max(0.f, a.fear - decay * tuning_.step);
        decide(i);
        move(i, player, playerVel);
        spread(i);
    }
    pickDrawn(player);
    for (std::uint32_t i = 0; i < agents_.size(); ++i)
        if (agents_[i].state != CrowdState::Free) writeView(i);
}

} // namespace ftcore
