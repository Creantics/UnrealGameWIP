#include "ftcore/PartnerBrain.h"

#include "ftcore/ArenaLockFrame.h"
#include "ftcore/FightSession.h"
#include "ftcore/Json.h"
#include "ftcore/StateHash.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ftcore {
namespace {

using View = PartnerEnemyView;

float dist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }
// From p to the enemy's edge (0 inside it).
float edge(Vec2 p, const View& e) { return std::max(0.f, dist(p, e.pos) - e.radius); }
// A view with a position and radius that are numbers (NaN or an infinity: absent, as if not there).
bool present(const View& e) {
    return std::isfinite(e.pos.x) && std::isfinite(e.pos.y) && std::isfinite(e.radius) && e.radius >= 0.f;
}
bool standing(const View& e) { return present(e) && e.state != View::State::Down; }
static_assert(kPartnerTallMeters == kArenaLookTallBoss, "rule 18's bar is the lock's tall-boss bar");
// Rule 18: a body this tall.
bool tallView(const View& e) {
    return present(e) && std::isfinite(e.base) && std::isfinite(e.top) && e.top - e.base >= kPartnerTallMeters;
}
bool windingAt(const View& e) {
    return present(e) && e.state == View::State::WindingUp && e.aim == View::Aim::Partner;
}
// The point `d` from c toward p (along +x when p is on c).
Vec2 toward(Vec2 c, Vec2 p, float d) {
    const float l = dist(c, p);
    if (l < 1e-4f) return {c.x + d, c.y};
    return {c.x + (p.x - c.x) / l * d, c.y + (p.y - c.y) / l * d};
}
std::uint32_t mix(std::uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h != 0 ? h : 1u;
}
// In float, as the tuning is held (so 0.01 in the data is at its 0.01 bound). NaN is out.
bool pbWithin(float v, float lo, float hi) { return v >= lo && v <= hi; }

// --- Stationing (rules 8-12), on the ground plane. No allocation: every search is a fixed set of samples. ---
constexpr float kStDeg = 3.14159265358979f / 180.f;
constexpr float kStFar = 1e30f; // no limit
Vec2 stSub(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
Vec2 stAlong(Vec2 p, Vec2 d, float k) { return {p.x + d.x * k, p.y + d.y * k}; }
float stDot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
float stCross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
bool stFinite(Vec2 a) { return std::isfinite(a.x) && std::isfinite(a.y); }
Vec2 stUnit(Vec2 a, Vec2 fallback) {
    const float l = std::hypot(a.x, a.y);
    return l > 1e-4f ? Vec2{a.x / l, a.y / l} : fallback;
}
// v turned `degrees` counter-clockwise.
Vec2 stTurn(Vec2 v, float degrees) {
    const float r = degrees * kStDeg, c = std::cos(r), s = std::sin(r);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}

// One step's ground: the wedge, the lead, the arena, and rules 8 and 9's tests and moves.
struct Ground {
    const PartnerTuning* t = nullptr;
    Vec2 cam{}, axis{}; // the wedge: from cam along axis (unit) to `length` (the lead + wedgeBeyondMeters)
    float length = 0.f;
    bool wedge = false;
    float sinH = 0.f, cosH = 1.f;
    Vec2 lead{}, facing{1.f, 0.f};
    bool reviving = false;
    Vec2 centre{};
    float radius = 0.f; // 0: unbounded
    float grow = 0.f;   // rule 18: StayClose's cap, leash and return grow by this (a tall target's radius)
    bool rangeBand = false; // rule 19: the kit's range on this step (partnerRangeOn)

    // Inside the wedge grown by `margin` (on its sides and far end; not behind the camera).
    bool inWedge(Vec2 p, float margin) const {
        if (!wedge) return false;
        const Vec2 d = stSub(p, cam);
        const float along = stDot(d, axis);
        if (along <= 0.f || along >= length + margin) return false;
        return std::fabs(stCross(axis, d)) * cosH - along * sinH < margin;
    }
    // How far inside the grown wedge p is (0 on its edge or outside it): the side distance or the far end's.
    float wedgeDepth(Vec2 p) const {
        if (!inWedge(p, t->wedgeClearMeters)) return 0.f;
        const Vec2 d = stSub(p, cam);
        const float along = stDot(d, axis);
        const float side = t->wedgeClearMeters - (std::fabs(stCross(axis, d)) * cosH - along * sinH);
        return std::max(0.f, std::min(side, length + t->wedgeClearMeters - along));
    }
    bool arenaOk(Vec2 p) const { return radius <= 0.f || dist(p, centre) <= radius + 1e-4f; }
    bool roomOk(Vec2 p) const { return reviving || dist(p, lead) >= t->personalSpaceMeters - 1e-4f; }
    bool legal(Vec2 p) const { return arenaOk(p) && roomOk(p) && !inWedge(p, t->wedgeClearMeters); }
    Vec2 intoArena(Vec2 p) const { return radius > 0.f && dist(p, centre) > radius ? toward(centre, p, radius) : p; }
    Vec2 outOfRoom(Vec2 p) const {
        return reviving || dist(p, lead) >= t->personalSpaceMeters ? p : toward(lead, p, t->personalSpaceMeters + 1e-3f);
    }
    // Rule 9: onto the nearer wedge edge, then the clearance out from it.
    Vec2 outToEdge(Vec2 p) const {
        const Vec2 d = stSub(p, cam);
        const float side = stCross(axis, d) >= 0.f ? 1.f : -1.f;
        const Vec2 e{axis.x * cosH - side * axis.y * sinH, side * axis.x * sinH + axis.y * cosH};
        const Vec2 n{-e.y * side, e.x * side}; // the edge's normal, away from the axis
        return stAlong(stAlong(cam, e, std::max(0.f, stDot(d, e))), n, t->wedgeClearMeters + 1e-3f);
    }
    // The wedge's other way out: past its far end.
    Vec2 outPastEnd(Vec2 p) const {
        return stAlong(p, axis, length + t->wedgeClearMeters + 1e-3f - stDot(stSub(p, cam), axis));
    }
    // p when it is legal and within maxLead of the lead; else the best of a fixed set of candidates (p moved out
    // of the wedge, out of personal space and into the arena, and rings round the lead), in the ruled order of the
    // winners: the arena first (never outside), then personal space, then the wedge, then the cap. Legal within
    // maxLead, the nearest p; legal, the least past maxLead; outside personal space but in the wedge (nothing
    // outside both is left), the least far into it; in neither (the arena leaves no room outside personal space),
    // the furthest from the lead.
    Vec2 resolve(Vec2 p, float maxLead) const {
        if (legal(p) && dist(p, lead) <= maxLead + 1e-4f) return p;
        Vec2 best = intoArena(p);
        int bt = 9;
        float ba = kStFar, bb = kStFar;
        const auto consider = [&](Vec2 q) {
            if (!stFinite(q) || !arenaOk(q)) return;
            const bool clear = !inWedge(q, t->wedgeClearMeters), room = roomOk(q);
            const float toLead = dist(q, lead), toP = dist(q, p);
            // Crowding the lead: the furthest from it (to 2 cm), then the least far into the wedge.
            int tier = 3;
            float a = -0.02f * std::floor(toLead / 0.02f), b = 100.f * wedgeDepth(q) + toP;
            if (clear && room && toLead <= maxLead + 1e-4f) {
                tier = 0;
                a = b = toP;
            } else if (clear && room) {
                tier = 1;
                a = toLead - maxLead;
                b = toP;
            } else if (room) {
                tier = 2;
                a = wedgeDepth(q);
                b = toP;
            }
            if (tier < bt || (tier == bt && (a < ba - 1e-5f || (a <= ba + 1e-5f && b < bb)))) {
                best = q;
                bt = tier;
                ba = a;
                bb = b;
            }
        };
        consider(p);
        Vec2 q = p;
        for (int i = 0; i < 3; ++i) {
            if (inWedge(q, t->wedgeClearMeters)) q = outToEdge(q);
            q = intoArena(outOfRoom(q));
            consider(q);
        }
        consider(intoArena(outOfRoom(outPastEnd(p))));
        const float ps = t->personalSpaceMeters;
        const float radii[] = {ps + 0.05f, ps + 0.5f, ps + 1.f, ps + 1.6f, ps + 2.4f};
        for (const float r : radii)
            for (int k = 0; k < 32; ++k)
                consider(intoArena(stAlong(lead, stTurn(Vec2{1.f, 0.f}, 11.25f * static_cast<float>(k)), r)));
        return best;
    }
};

// The station view, whole and finite, or no stationing.
bool makeGround(const PartnerInput& in, const PartnerTuning& t, Ground& g) {
    const PartnerStationView& v = in.station;
    if (!v.on || !stFinite(v.cameraPos) || !stFinite(v.cameraForward) || !stFinite(v.leadFacing) ||
        !stFinite(v.arenaCentre) || !std::isfinite(v.arenaRadius) || v.arenaRadius < 0.f || !stFinite(in.pos) ||
        !stFinite(in.leadPos))
        return false;
    const Vec2 toLead = stSub(in.leadPos, v.cameraPos);
    const float l = std::hypot(toLead.x, toLead.y);
    if (!std::isfinite(l) || !std::isfinite(dist(in.pos, in.leadPos)) || !std::isfinite(dist(in.pos, v.arenaCentre)))
        return false;
    g.t = &t;
    g.cam = v.cameraPos;
    g.lead = in.leadPos;
    g.reviving = v.reviving;
    g.centre = v.arenaCentre;
    g.radius = v.arenaRadius;
    g.axis = stUnit(toLead, stUnit(v.cameraForward, Vec2{}));
    g.wedge = g.axis.x != 0.f || g.axis.y != 0.f;
    g.length = (l > 1e-4f ? l : 0.f) + t.wedgeBeyondMeters;
    g.sinH = std::sin(t.wedgeHalfDegrees * kStDeg);
    g.cosH = std::cos(t.wedgeHalfDegrees * kStDeg);
    g.facing = stUnit(v.leadFacing, g.wedge ? g.axis : Vec2{1.f, 0.f});
    g.rangeBand = partnerRangeOn(in, t);
    return true;
}

// A candidate goal: tier 0 in the arc, 2 elsewhere on the circle, 3 resolved off it; lower first, then score.
struct Spot {
    Vec2 p{};
    int tier = 9;
    float score = kStFar;
};
bool better(const Spot& a, const Spot& b) { return a.tier < b.tier || (a.tier == b.tier && a.score < b.score); }

// StayClose: every station goal within this of the lead, half a metre inside the leash so a drifting lead does not
// trip it (the return would end at back, the partner walk out again, and so on).
float closeCap(const PartnerTuning& t, const Ground& g) {
    // Rule 19, the kit's range on: room for a caster at its standoff on the far side of a target its lead is in melee
    // with (the standoff, the target's width and the lead's gap: kPartnerRangeLeadMelee).
    const float lead =
        g.rangeBand ? std::max(t.closeLeadMeters, t.standoffMeters + kPartnerRangeLeadMelee) : t.closeLeadMeters;
    return std::min(lead + g.grow, t.leashMeters + g.grow - 0.5f);
}

// A ring's metres off a target's edge where the rules place a partner to start a string: half the reach (classic), or,
// the kit's range on (rule 19), the middle of its band [min(standoff, reach - tolerance), reach - tolerance].
float innerRing(const PartnerTuning& t, bool rangeBand) {
    if (!rangeBand) return 0.5f * t.reachMeters;
    const float hi = t.reachMeters - t.stationTolerance;
    return 0.5f * (std::min(t.standoffMeters, hi) + hi);
}

// A string may start at this edge distance: within the reach. The kit's range on (rule 19, ruling of 11-16), its
// standoff holds no string back: the partner steps out to it between strings (kPartnerRangeStepOut).
bool inStartBand(const PartnerTuning& t, float edgeDist) { return edgeDist <= t.reachMeters; }

// Rule 19, the kit's range on: from the lens, a partner at p is at least partly behind the lead at b (both of radius
// bodyRadius): its centre within the lead's silhouette widened by its own width, and further off.
bool rangePartlyHidden(Vec2 lens, Vec2 p, Vec2 b, float bodyRadius) {
    return partnerCloseHiddenBehind(lens, p, b, 2.f * bodyRadius);
}

// The distance from p to the segment a-b.
float segmentDist(Vec2 a, Vec2 b, Vec2 p) {
    const Vec2 ab = stSub(b, a);
    const float l2 = stDot(ab, ab);
    const float k = l2 > 1e-8f ? std::clamp(stDot(stSub(p, a), ab) / l2, 0.f, 1.f) : 0.f;
    return dist(p, stAlong(a, ab, k));
}

// Rule 19, the kit's range on (ruling of 11-16): a partner more than kPartnerRangeStepOut inside its standoff steps out
// between strings to the nearest point of the band's near end (the target's edge plus the standoff) on its own flank
// (the side of the lead-target line it stands on): straight out from the target first, then turned up to 120 degrees
// either way, 10 at a time. The point is in the arena, outside personal space and, under StayClose, inside the cap;
// the walk to it passes the lead no closer than personal space (or than the partner stands now). Under the close lens
// (lens given), the first such point not even partly behind the lead (rangePartlyHidden) wins, else the first. False:
// no such point (the partner casts from where it stands).
bool rangeStepOut(const Ground& g, const PartnerTuning& t, const View& e, PartnerOrder order, Vec2 from,
                  const Vec2* lens, float bodyRadius, Vec2& q) {
    const Vec2 bear = stSub(g.lead, e.pos);
    const Vec2 out = stUnit(stSub(from, e.pos), stUnit(Vec2{-bear.x, -bear.y}, Vec2{1.f, 0.f}));
    const float own = stCross(bear, stSub(from, e.pos));
    const float ring = e.radius + t.standoffMeters;
    const float cap = order == PartnerOrder::StayClose ? closeCap(t, g) : kStFar;
    const float clear = std::min(t.personalSpaceMeters, dist(from, g.lead)) - 1e-4f;
    const float body = std::min(e.radius + bodyRadius, dist(from, e.pos)) - 1e-4f;
    bool found = false;
    for (int k = 0; k <= 12; ++k)
        for (int w = 0; w < (k == 0 ? 1 : 2); ++w) {
            const float a = (w == 0 ? 10.f : -10.f) * static_cast<float>(k) * kStDeg;
            const float c = std::cos(a), s = std::sin(a);
            const Vec2 p = stAlong(e.pos, Vec2{out.x * c - out.y * s, out.x * s + out.y * c}, ring);
            if (!g.arenaOk(p) || !g.roomOk(p) || dist(p, g.lead) > cap + 1e-4f) continue;
            if (std::fabs(own) > 1e-3f && stCross(bear, stSub(p, e.pos)) * own < 0.f) continue; // the other flank
            if (!g.reviving && segmentDist(from, p, g.lead) < clear) continue;
            if (segmentDist(from, p, e.pos) < body) continue; // nor through the target
            if (!lens || !rangePartlyHidden(*lens, p, g.lead, bodyRadius)) {
                q = p;
                return true;
            }
            if (!found) q = p;
            found = true;
        }
    return found;
}

// Rule 10: the best station on side s (+1: counter-clockwise from the lead, seen at the target).
Spot stationOn(const Ground& g, const PartnerTuning& t, const PartnerEnemyView& e, PartnerOrder order, Vec2 pos,
               float s) {
    const bool close = order == PartnerOrder::StayClose;
    const float lo = close ? t.closeArcMin : t.spreadArcMin, hi = close ? t.closeArcMax : t.spreadArcMax;
    const float cap = closeCap(t, g);
    const float ring = e.radius + innerRing(t, g.rangeBand);
    const Vec2 u = stUnit(stSub(g.lead, e.pos), stUnit(stSub(g.cam, e.pos), Vec2{1.f, 0.f}));
    Spot best;
    const auto at = [&](float deg) {
        const Vec2 q = stAlong(e.pos, stTurn(u, s * deg), ring);
        if (!g.legal(q)) return;
        if (close && dist(q, g.lead) > cap) return; // the leash still rules, with room to spare
        Spot c{q, 2, 0.f};
        if (deg >= lo - 1e-3f && deg <= hi + 1e-3f) {
            c.tier = 0;
            c.score = dist(q, pos);
        } else {
            c.score = std::max(lo - deg, deg - hi) + 1e-3f * dist(q, pos);
        }
        if (better(c, best)) best = c;
    };
    for (int k = 0; k <= 36; ++k) at(5.f * static_cast<float>(k));
    at(lo);
    at(hi);
    at(0.5f * (lo + hi));
    if (best.tier < 9) return best;
    best.p = g.resolve(stAlong(e.pos, stTurn(u, s * 0.5f * (lo + hi)), ring), close ? cap : kStFar);
    best.tier = 3;
    best.score = dist(best.p, pos);
    return best;
}

// Rule 11: behind the lead and to a side (110..160 degrees from its facing) on side s, followMin..followMax out
// (StayClose: at most the close cap).
Spot followOn(const Ground& g, const PartnerTuning& t, PartnerOrder order, Vec2 pos, float s) {
    const float lo = std::max(t.followMin, t.personalSpaceMeters + 0.05f);
    const float hi = std::max(order == PartnerOrder::StayClose ? std::min(t.followMax, closeCap(t, g)) : t.followMax, lo);
    const float radii[] = {lo, 0.5f * (lo + hi), hi};
    Spot best;
    for (const float r : radii)
        for (int k = 0; k < 6; ++k) {
            const Vec2 q = stAlong(g.lead, stTurn(g.facing, s * (110.f + 10.f * static_cast<float>(k))), r);
            if (!g.legal(q)) continue;
            const Spot c{q, 0, dist(q, pos)};
            if (better(c, best)) best = c;
        }
    if (best.tier < 9) return best;
    best.p = g.resolve(stAlong(g.lead, stTurn(g.facing, s * 135.f), 0.5f * (lo + hi)), hi);
    best.tier = 3;
    best.score = dist(best.p, pos);
    return best;
}

// Rule 10's side (and 11's): the side with a station in its arc; both, the nearer the partner. Chosen afresh when the
// order changes (key: the order; the named exception, a re-pick at once). A new target (or none) keeps the side, and
// its timer, while that side's station for it is in its arc; else it re-picks. Otherwise held sideHoldSeconds unless
// its own side has none in its arc and the other has.
Vec2 pickSide(PartnerBrainSave& s, int key, int target, const Spot& minus, const Spot& plus) {
    const auto ok = [](const Spot& x) { return x.tier == 0; };
    const auto prefer = [&]() -> std::int8_t {
        if (ok(minus) != ok(plus)) return ok(minus) ? std::int8_t{-1} : std::int8_t{1};
        return better(minus, plus) ? std::int8_t{-1} : std::int8_t{1};
    };
    if (s.side == 0 || s.sideKey != key) {
        s.side = prefer();
        s.sideKey = key;
        s.sideTarget = target;
        s.sideFor = 0.0;
    } else if (s.sideTarget != target) {
        s.sideTarget = target;
        if (!ok(s.side < 0 ? minus : plus)) {
            const std::int8_t p = prefer();
            if (p != s.side) {
                s.side = p;
                s.sideFor = 0.0;
            }
        }
    } else {
        const bool forced = !ok(s.side < 0 ? minus : plus) && ok(s.side < 0 ? plus : minus);
        if (forced || s.sideFor + 1e-9 >= static_cast<double>(s.t.sideHoldSeconds)) {
            const std::int8_t p = prefer();
            if (p != s.side) {
                s.side = p;
                s.sideFor = 0.0;
            }
        }
    }
    return (s.side < 0 ? minus : plus).p;
}

int orderKey(PartnerOrder o) { return o == PartnerOrder::Spread ? 2 : 1; }

// The starvation fallback's goal (rule 12 given up, out of reach): the nearest point of the target's ring in the
// arena, outside personal space and (StayClose) within the cap, or false.
bool inReachSpot(const Ground& g, const PartnerTuning& t, const PartnerEnemyView& e, PartnerOrder order, Vec2 pos,
                 Vec2& out) {
    const float ring = e.radius + innerRing(t, g.rangeBand);
    const float cap = order == PartnerOrder::StayClose ? closeCap(t, g) : kStFar;
    bool found = false;
    float best = kStFar;
    const auto consider = [&](Vec2 q) {
        if (!stFinite(q) || !g.arenaOk(q) || !g.roomOk(q) || dist(q, g.lead) > cap) return;
        const float d = dist(q, pos);
        if (d < best) {
            best = d;
            out = q;
            found = true;
        }
    };
    consider(toward(e.pos, pos, ring));
    for (int k = 0; k < 36; ++k) consider(stAlong(e.pos, stTurn(Vec2{1.f, 0.f}, 10.f * static_cast<float>(k)), ring));
    return found;
}

// --- Rules 13-16: the play camera. ---
using V3 = PartnerVec3;
float v3Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Rule 13's frustum, shrunk by the margin: a capsule's three points each in front of the camera and within lim of
// the screen's centre in its -1..1 frame across and above it, within limBelow below it (the bottom's own
// kPartnerFrustumMarginBottom in the brain only with its framing holds on, framingHolds; else lim, as main).
// Under rule 19 (upper) the partner is in frame by its middle and head alone; its feet are not tested.
struct Frame {
    V3 c{}, f{}, u{}, r{};
    float tanV = 0.f, tanH = 0.f, lim = 0.8f, limBelow = 0.8f, z0 = 0.f, h = 1.8f;
    bool holds = false; // framingHolds this step: the brain's bottom margin and side hold are on
    bool upper = false; // rule 19 (the close camera given): the feet are not tested
    bool point(float x, float y, float z) const {
        const V3 d{x - c.x, y - c.y, z - c.z};
        const float depth = v3Dot(d, f);
        if (!(depth > 0.05f)) return false;
        const float sx = v3Dot(d, r) / (depth * tanH), sy = v3Dot(d, u) / (depth * tanV);
        return std::fabs(sx) <= lim && sy <= lim && sy >= -limBelow;
    }
    bool sees(Vec2 p) const { return (upper || point(p.x, p.y, z0)) && point(p.x, p.y, z0 + 0.5f * h) && point(p.x, p.y, z0 + h); }
    bool seesWhole(Vec2 p) const { return point(p.x, p.y, z0) && point(p.x, p.y, z0 + 0.5f * h) && point(p.x, p.y, z0 + h); }
    // Rule 18: the partner at p hidden behind a tall e from the lens (its middle and its head both behind its body).
    bool hides(const PartnerEnemyView* e, Vec2 p) const {
        if (!e || !tallView(*e)) return false;
        const ArenaPoint3 eye{c.x, c.y, c.z};
        const auto behind = [&](float z) {
            return arenaHiddenBehind(eye, ArenaPoint3{p.x, p.y, z0 + z}, e->pos, e->radius, z0 + e->base, z0 + e->top);
        };
        return behind(0.55f * h) && behind(0.95f * h);
    }
    bool clear(const PartnerEnemyView* e, Vec2 p) const { return sees(p) && !hides(e, p); }
};

// The camera view, whole (PartnerCameraView's rules), or not given.
// The brain's framing holds (rule 13's bottom margin, frustumMarginBottom, and side hold, frameSideHoldSeconds) act
// only with a tall enemy in the fight (any of the input's: rule 18) or rule 17's circling on. Without either the
// framing is main's, to the bit (measured: kept general they lowered an ordinary fight's on-screen share).
bool framingHolds(const PartnerInput& in, const PartnerTuning& t) {
    if (t.circling) return true;
    const std::size_t n = std::min(in.enemies.size(), kMaxPartnerEnemies);
    for (std::size_t i = 0; i < n; ++i)
        if (tallView(in.enemies[i])) return true;
    return false;
}

// Rule 13's side hold: frameSideHoldSeconds with the framing holds on, else 0 (main's: none).
double sideHoldOf(const Frame& fr, const PartnerTuning& t) {
    return fr.holds ? static_cast<double>(t.frameSideHoldSeconds) : 0.0;
}

// `holds`: framingHolds (the brain), or partnerInFrame's measure, which keeps the bottom's own margin always.
bool makeFrame(const PartnerCameraView& v, const PartnerTuning& t, Frame& fr, bool holds) {
    if (!partnerCameraWhole(v)) return false; // the same test ArenaFight makes
    const float lf = std::sqrt(v3Dot(v.forward, v.forward)), lu = std::sqrt(v3Dot(v.up, v.up));
    const V3 f{v.forward.x / lf, v.forward.y / lf, v.forward.z / lf};
    const float k = v3Dot(v.up, f) / lu;
    const V3 u0{v.up.x / lu - k * f.x, v.up.y / lu - k * f.y, v.up.z / lu - k * f.z}; // up made square to forward
    const float l0 = std::sqrt(v3Dot(u0, u0));
    if (!(l0 > 0.f)) return false; // never with a whole camera (up more than a degree off forward)
    fr.c = v.pos;
    fr.f = f;
    fr.u = V3{u0.x / l0, u0.y / l0, u0.z / l0};
    fr.r = V3{f.y * fr.u.z - f.z * fr.u.y, f.z * fr.u.x - f.x * fr.u.z, f.x * fr.u.y - f.y * fr.u.x};
    fr.tanV = std::tan(0.5f * v.verticalFovDegrees * kStDeg);
    fr.tanH = fr.tanV * v.aspect;
    fr.lim = 1.f - 2.f * t.frustumMargin;
    fr.holds = holds;
    fr.limBelow = holds ? 1.f - 2.f * std::min(t.frustumMargin, t.frustumMarginBottom) : fr.lim; // else main's
    fr.z0 = v.groundZ;
    fr.h = v.bodyHeight;
    return true;
}

// Rule 13's target keep-out: from the camera along the line to the target, the target's silhouette's half-angle, to
// wedgeBeyondMeters past its far edge (rule 9's wedge's form). Off with the camera on the target.
struct Keep {
    bool on = false;
    Vec2 cam{}, axis{};
    float length = 0.f, sinH = 0.f, cosH = 1.f;
    bool in(Vec2 p, float margin) const {
        if (!on) return false;
        const Vec2 d = stSub(p, cam);
        const float along = stDot(d, axis);
        if (along <= 0.f || along >= length + margin) return false;
        return std::fabs(stCross(axis, d)) * cosH - along * sinH < margin;
    }
};
Keep makeKeep(const Ground& g, const PartnerEnemyView& e) {
    Keep k;
    const Vec2 d = stSub(e.pos, g.cam);
    const float far = std::hypot(d.x, d.y);
    if (!(far > 1e-3f)) return k;
    k.on = true;
    k.cam = g.cam;
    k.axis = Vec2{d.x / far, d.y / far};
    k.sinH = std::min(1.f, e.radius / far);
    k.cosH = std::sqrt(std::max(0.f, 1.f - k.sinH * k.sinH));
    k.length = far + e.radius + g.t->wedgeBeyondMeters;
    return k;
}

// The angle (degrees) between a and b.
float stAngle(Vec2 a, Vec2 b) {
    const Vec2 ua = stUnit(a, Vec2{1.f, 0.f}), ub = stUnit(b, Vec2{1.f, 0.f});
    return std::acos(std::clamp(stDot(ua, ub), -1.f, 1.f)) / kStDeg;
}

// Rule 13: a valid station for target e (none: the follow ring round the lead) into `out`, or false. The one held,
// while it is valid (minFrom 0); else the nearest valid one, the arc's first, at least minFrom from pos (rule 16),
// which becomes the one held.
// Rule 18: the straight walk from `from` to `to` passes within kPartnerSteerClear of the target's body (it must go
// round it).
bool walksRound(Vec2 from, Vec2 to, const PartnerEnemyView& e) {
    const Vec2 a = stSub(from, e.pos), d = stSub(to, from);
    const float dd = stDot(d, d);
    const float u = dd > 1e-6f ? std::clamp(-stDot(a, d) / dd, 0.f, 1.f) : 0.f;
    return std::hypot(a.x + d.x * u, a.y + d.y * u) < e.radius + kPartnerSteerClear;
}

// Rule 18: rule 13's fallback by a tall target e (no valid station): the nearest point of its two station rings that
// is legal and (StayClose) within the cap; seen and unhidden first, then unhidden, then seen, then neither, a walk
// round its body after each. False: none (rules 10-11's then). `cls`: the spot's class, 0 seen and unhidden, 1
// unhidden, 2 seen (hidden), 3 neither; the best class of any legal ring point.
bool tallFallback(const Ground& g, const Frame& fr, const PartnerTuning& t, PartnerOrder order, Vec2 pos,
                  const PartnerEnemyView& e, Vec2& out, int& cls) {
    const float cap = order == PartnerOrder::StayClose ? closeCap(t, g) : kStFar;
    const std::array<float, 2> radii{e.radius + innerRing(t, g.rangeBand), e.radius + t.reachMeters - t.stationTolerance};
    Spot best;
    for (const float r : radii)
        for (int a = 0; a < 72; ++a) {
            const Vec2 q = stAlong(e.pos, stTurn(Vec2{1.f, 0.f}, 5.f * static_cast<float>(a)), r);
            if (!stFinite(q) || !g.legal(q) || dist(q, g.lead) > cap + 1e-4f) continue;
            const bool hid = fr.hides(&e, q), seen = fr.sees(q);
            const int tier = 2 * (!hid ? (seen ? 0 : 1) : (seen ? 2 : 3)) + (walksRound(pos, q, e) ? 1 : 0);
            const Spot cand{q, tier, dist(q, pos)};
            if (better(cand, best)) best = cand;
        }
    if (best.tier >= 9) return false;
    out = best.p;
    cls = best.tier / 2;
    return true;
}

// Rule 18: StayClose's growth by the tall targets marked in `on` (tallGrowOn): the largest radius, at most
// tallGrowMaxMeters. The one source for the brain's step and its circling walk.
float tallGrowOf(const PartnerInput& in, const PartnerTuning& t, std::uint8_t on) {
    float grow = 0.f;
    const std::size_t n = std::min(in.enemies.size(), kMaxPartnerEnemies);
    for (std::size_t i = 0; i < n; ++i)
        if (on & (1u << i)) grow = std::max(grow, std::min(in.enemies[i].radius, t.tallGrowMaxMeters));
    return grow;
}

// Rule 13's grace: whether the last station (s.frameLast) is held this step with no valid one found. It holds while it
// stays legal, unhidden and out of this target's keep-out (it may have left the frame: the camera can come back to it);
// within frameSideHoldSeconds of the last valid station it holds in the keep-out too (a short gap is not worth a jump
// to rules 10-11's and back). The one clock for that hold is noFrame, here and in frameStation's side hold.
bool graceHolds(const Ground& g, const Frame& fr, const PartnerTuning& t, const PartnerEnemyView* e,
                const PartnerBrainSave& s) {
    const bool sideHeld = s.noFrame + 1e-9 < sideHoldOf(fr, t);
    return !s.fellBack && s.frameRing != 0 && s.noFrame + 1e-9 < static_cast<double>(t.frustumGiveUpSeconds) &&
           g.legal(s.frameLast) && (sideHeld || !(e && makeKeep(g, *e).in(s.frameLast, t.wedgeClearMeters))) &&
           !fr.hides(e, s.frameLast);
}

bool frameStation(const Ground& g, const Frame& fr, const PartnerTuning& t, PartnerOrder order, Vec2 pos,
                  const PartnerEnemyView* e, int tgt, float minFrom, PartnerBrainSave& s, Vec2& out) {
    const bool close = order == PartnerOrder::StayClose;
    const float cap = close ? closeCap(t, g) : kStFar;
    const Keep k = e ? makeKeep(g, *e) : Keep{};
    const bool tall = e && tallView(*e); // rule 18
    const auto valid = [&](Vec2 q) {
        return stFinite(q) && g.legal(q) && dist(q, g.lead) <= cap + 1e-4f && !k.in(q, t.wedgeClearMeters) && fr.clear(e, q);
    };
    const Vec2 c = e ? e->pos : g.lead;
    const std::uint8_t ring = e ? 1 : 2;
    std::array<float, 3> radii{};
    int nr = 1;
    float lo = 110.f, hi = 160.f; // rule 11's
    if (e) {
        // Half the reach off its edge, else (a second tier) as far out as still arrives in reach.
        radii[0] = e->radius + innerRing(t, g.rangeBand);
        radii[1] = e->radius + t.reachMeters - t.stationTolerance;
        nr = radii[1] > radii[0] + 0.1f ? 2 : 1;
        lo = close ? t.closeArcMin : t.spreadArcMin;
        hi = close ? t.closeArcMax : t.spreadArcMax;
    } else {
        const float a = std::max(t.followMin, t.personalSpaceMeters + 0.05f);
        const float b = std::max(close ? std::min(t.followMax, cap) : t.followMax, a);
        radii = {a, 0.5f * (a + b), b};
        nr = 3;
    }
    // The held station's side (of the line from the centre to the lead, or of the lead's facing): 0 none held.
    const Vec2 sideRef = e ? stSub(g.lead, c) : g.facing;
    const auto sideOf = [&](Vec2 p) { return stCross(sideRef, stSub(p, c)) >= 0.f ? 1.f : -1.f; };
    const bool held = s.frameRing == ring && (ring == 2 || s.frameTarget == tgt);
    const Vec2 was = stAlong(c, stTurn(Vec2{1.f, 0.f}, s.frameAngle), s.frameRadius);
    const float side = held ? sideOf(was) : 0.f;
    // Rule 18: a station still round a tall target's body is not held (a nearer one may be valid again).
    if (minFrom <= 0.f && held && valid(was) && !(tall && walksRound(pos, was, *e))) {
        s.frameLast = was;
        out = was;
        return true;
    }
    // Within kPartnerSideHoldSeconds of its last change of side, a new pick keeps to its side; none valid there, the
    // grace holds the last point while the camera sees it unhidden (the other side once the hold is out).
    const bool keepSide = side != 0.f && s.now - s.frameSideAt < sideHoldOf(fr, t);
    Spot best;
    float bestAngle = 0.f, bestRadius = 0.f;
    for (int pass = keepSide ? 0 : 1; pass < 2 && best.tier >= 9; ++pass) {
        // The other side is refused only while the grace will hold the last point (in frame and unhidden): else
        // the other side's valid station, not rules 10-11's fallback.
        if (pass == 1 && keepSide && fr.clear(e, s.frameLast) && graceHolds(g, fr, t, e, s)) return false;
        for (int ri = 0; ri < nr; ++ri)
            for (int a = 0; a < 72; ++a) {
                const float ang = 5.f * static_cast<float>(a);
                const Vec2 q = stAlong(c, stTurn(Vec2{1.f, 0.f}, ang), radii[static_cast<std::size_t>(ri)]);
                if (!valid(q) || dist(q, pos) < minFrom || (pass == 0 && sideOf(q) != side)) continue;
                const float deg = e ? stAngle(stSub(g.lead, c), stSub(q, c)) : stAngle(g.facing, stSub(q, c));
                const bool second = tall ? ri == 0 && nr > 1 : ri > 0; // rule 18: on a tall target the outer ring first
                // Rule 18: on a tall target a station reached only round its body (out of sight behind it) comes last.
                const int round = tall && walksRound(pos, q, *e) ? 4 : 0;
                const Spot cand{q, (deg >= lo - 1e-3f && deg <= hi + 1e-3f ? 0 : 1) + (e && second ? 2 : 0) + round, dist(q, pos)};
                if (better(cand, best)) {
                    best = cand;
                    bestAngle = ang;
                    bestRadius = radii[static_cast<std::size_t>(ri)];
                }
            }
    }
    if (best.tier >= 9) return false;
    s.frameRing = ring;
    s.frameTarget = e ? tgt : -1;
    s.frameAngle = bestAngle;
    s.frameRadius = bestRadius;
    s.frameLast = best.p;
    if (fr.holds && side != 0.f && sideOf(best.p) != side) s.frameSideAt = s.now; // the side hold's clock
    out = best.p;
    return true;
}

// Why it is not acting (PartnerIdle's rules), from the intent it gives.
PartnerIdle idleOf(const PartnerIntent& o, const PartnerInput& in) {
    switch (o.why) {
    case PartnerWhy::Guard:
    case PartnerWhy::String:
    case PartnerWhy::Start:
    case PartnerWhy::Committed:
    case PartnerWhy::Reposition: return PartnerIdle::None;
    case PartnerWhy::Down: return PartnerIdle::Staggered;
    case PartnerWhy::CannotAct:
        return in.doing == PartnerDoing::Hurt ? PartnerIdle::Staggered : PartnerIdle::Other;
    case PartnerWhy::Leash: return PartnerIdle::Moving;
    case PartnerWhy::Pause: return o.act == PartnerAct::Wait ? PartnerIdle::Pause : PartnerIdle::Moving;
    case PartnerWhy::NoTarget: return PartnerIdle::NoTarget;
    case PartnerWhy::Approach: return o.act == PartnerAct::MoveTo ? PartnerIdle::Moving : PartnerIdle::NoTarget;
    case PartnerWhy::Budget: return PartnerIdle::NoBudget;
    case PartnerWhy::NoKit: return PartnerIdle::NoKit;
    default: return PartnerIdle::Other; // NotBegun, HeldOff
    }
}

} // namespace

bool partnerInFrame(const PartnerCameraView& camera, float margin, Vec2 p) {
    PartnerTuning t;
    t.frustumMargin = margin;
    Frame fr;
    return pbWithin(margin, 0.f, 0.4f) && stFinite(p) && makeFrame(camera, t, fr, true) && fr.sees(p);
}

bool partnerInFrameClose(const PartnerCameraView& camera, float margin, Vec2 p) {
    PartnerTuning t;
    t.frustumMargin = margin;
    Frame fr;
    fr.upper = true;
    return pbWithin(margin, 0.f, 0.4f) && stFinite(p) && makeFrame(camera, t, fr, true) && fr.sees(p);
}

bool validPartnerTuning(const PartnerTuning& t) {
    return pbWithin(t.leashMeters, 1.f, 10.f) && pbWithin(t.returnMeters, 0.5f, t.leashMeters) &&
           pbWithin(t.reachMeters, 0.5f, 4.f) && pbWithin(t.standoffMeters, 0.f, t.reachMeters) && t.blowsMin >= 1 &&
           t.blowsMin <= 6 && t.blowsMax >= t.blowsMin && t.blowsMax <= 6 && pbWithin(t.blowSeconds, 0.1f, 2.f) &&
           pbWithin(t.pauseMin, 0.f, 10.f) && pbWithin(t.pauseMax, t.pauseMin, 10.f) &&
           pbWithin(t.guardWarning, 0.f, 2.f) && pbWithin(t.guardChance, 0.f, 1.f) && pbWithin(t.guardHold, 0.f, 2.f) &&
           pbWithin(t.downSeconds, 1.f, 600.f) && pbWithin(t.riseHealth, 0.01f, 1.f) &&
           pbWithin(t.blowCapShare, 0.f, 1.f) && pbWithin(t.overScale, 0.f, 1.f) &&
           pbWithin(t.budgetWindowSeconds, 5.f, 30.f) && pbWithin(t.budgetShare, 0.f, 1.f) &&
           pbWithin(t.budgetAllowanceSeconds, 0.f, 10.f) && pbWithin(t.personalSpaceMeters, 0.5f, 4.f) &&
           pbWithin(t.wedgeHalfDegrees, 5.f, 60.f) && pbWithin(t.wedgeBeyondMeters, 0.f, 5.f) &&
           pbWithin(t.wedgeClearMeters, 0.f, 2.f) && pbWithin(t.closeArcMin, 0.f, 180.f) &&
           pbWithin(t.closeArcMax, t.closeArcMin, 180.f) && pbWithin(t.closeLeadMeters, 1.f, 10.f) &&
           pbWithin(t.spreadArcMin, 0.f, 180.f) && pbWithin(t.spreadArcMax, t.spreadArcMin, 180.f) &&
           pbWithin(t.sideHoldSeconds, 0.f, 10.f) && pbWithin(t.followMin, 0.5f, 10.f) &&
           pbWithin(t.followMax, t.followMin, 10.f) && pbWithin(t.stationTolerance, 0.05f, 2.f) &&
           t.stationTolerance <= 0.5f * t.reachMeters && t.leashMeters >= t.personalSpaceMeters + 1.f &&
           pbWithin(t.stationGiveUpSeconds, 0.5f, 30.f) && pbWithin(t.stationAnySeconds, t.stationGiveUpSeconds, 60.f) &&
           pbWithin(t.targetHoldSeconds, 0.f, 10.f) && pbWithin(t.frustumMargin, 0.f, 0.4f) &&
           pbWithin(t.frustumGiveUpSeconds, 0.f, 30.f) && pbWithin(t.engageSeconds, 0.25f, 10.f) &&
           pbWithin(t.repositionEverySeconds, 2.f, 20.f) && pbWithin(t.repositionMeters, 0.5f, 6.f) &&
           pbWithin(t.tallGrowMaxMeters, 0.f, 20.f) && pbWithin(t.frustumMarginBottom, 0.f, 0.4f) &&
           pbWithin(t.frameSideHoldSeconds, 0.f, 5.f) && pbWithin(t.choiceHoldSeconds, 0.f, 5.f) &&
           pbWithin(t.turnHoldSeconds, 0.f, 5.f);
}

PartnerTuning mainEquivalentTuning(PartnerTuning t) {
    t.frustumMarginBottom = t.frustumMargin;
    t.frameSideHoldSeconds = 0.f;
    t.choiceHoldSeconds = 0.f;
    t.turnHoldSeconds = 0.f;
    return t;
}

static_assert(kMaxPartnerStep == kMaxFightStep, "the brain's step bound is the fight's");

bool partnerRangeOn(const PartnerInput& in, const PartnerTuning& t) {
    const PartnerRange& r = in.range;
    return in.station.close.given && std::isfinite(r.reach) && std::isfinite(r.standoff) &&
           r.reach >= std::max(0.5f, 2.f * t.stationTolerance) && r.reach <= kPartnerRangeMax && r.standoff >= 0.f &&
           r.standoff <= r.reach;
}

PartnerTuning partnerEffectiveTuning(const PartnerTuning& t, const PartnerInput& in) {
    PartnerTuning e = t;
    if (partnerRangeOn(in, t)) {
        e.reachMeters = in.range.reach;
        e.standoffMeters = in.range.standoff;
        // The leash holds the band: a caster at its reach on the far side of its lead's melee target, at most 10 m
        // (a melee kit keeps its own); the pull back ends as far inside it as the tuning's did.
        e.leashMeters = std::min(10.f, std::max(t.leashMeters, in.range.reach + kPartnerRangeLeadMelee));
        e.returnMeters = std::max(t.returnMeters, e.leashMeters - (t.leashMeters - t.returnMeters));
    }
    return e;
}

bool PartnerBrain::begin(const PartnerTuning& t, std::uint32_t seed) {
    const bool ok = validPartnerTuning(t);
    s_ = PartnerBrainSave{};
    s_.t = ok ? t : PartnerTuning{};
    tStep_ = s_.t;
    s_.rhythm = mix(seed);
    s_.defence = mix(seed ^ 0xA5C3F00Du);
    s_.skirmish = mix(seed ^ 0x5C1A7E3Bu);
    s_.begun = true;
    return ok;
}

std::uint64_t PartnerBrainSave::hash() const {
    StateHash h;
    h.add(t.leashMeters).add(t.returnMeters).add(t.reachMeters).add(t.standoffMeters).add(t.blowsMin).add(t.blowsMax);
    h.add(t.blowSeconds).add(t.pauseMin).add(t.pauseMax).add(t.guardWarning).add(t.guardChance).add(t.guardHold);
    h.add(t.downSeconds).add(t.riseHealth).add(t.blowCapShare).add(t.overScale).add(t.budgetWindowSeconds);
    h.add(t.budgetShare).add(t.budgetAllowanceSeconds);
    h.add(t.personalSpaceMeters).add(t.wedgeHalfDegrees).add(t.wedgeBeyondMeters).add(t.wedgeClearMeters);
    h.add(t.closeArcMin).add(t.closeArcMax).add(t.closeLeadMeters).add(t.spreadArcMin).add(t.spreadArcMax);
    h.add(t.sideHoldSeconds).add(t.followMin).add(t.followMax).add(t.stationTolerance);
    h.add(t.stationGiveUpSeconds).add(t.stationAnySeconds).add(t.targetHoldSeconds);
    h.add(rhythm).add(defence).add(rhythmDraws).add(defenceDraws).add(phase).add(target).add(blows).add(pause);
    h.add(clock).add(returning).add(drawn).add(lengths).add(begunBlows).add(committedEnd);
    h.add(spentPartner).add(spentLead).add(slot).add(now).add(down).add(downFor);
    for (const Watch& w : watch) h.add(w.winding).add(w.left).add(w.guard).add(w.hold);
    h.add(out.act).add(out.why).add(out.point.x).add(out.point.y).add(out.enemy).add(out.blows).add(out.newString).add(out.blowStart).add(out.blowIndex);
    h.add(out.partnerBlow).add(out.breakValue).add(out.extendsStagger).add(out.punish).add(out.rise).add(out.riseHealth);
    h.add(out.stationed).add(begun).add(side).add(sideKey).add(sideFor).add(swaps);
    h.add(sideTarget).add(offStation).add(targetFor).add(targetReached);
    // Rules 13-16: only once framed or tuned off their defaults (a brain never given a camera hashes as before).
    const PartnerTuning d{};
    const bool tuned = t.frustumMargin != d.frustumMargin || t.frustumGiveUpSeconds != d.frustumGiveUpSeconds ||
                       t.engageSeconds != d.engageSeconds || t.repositionEverySeconds != d.repositionEverySeconds ||
                       t.repositionMeters != d.repositionMeters || t.circling != d.circling;
    if (framed || tuned) {
        h.add(std::uint8_t{13});
        h.add(t.frustumMargin).add(t.frustumGiveUpSeconds).add(t.engageSeconds).add(t.repositionEverySeconds);
        h.add(t.repositionMeters).add(framed).add(frameRing).add(frameTarget).add(frameAngle).add(frameRadius);
        h.add(frameLast.x).add(frameLast.y).add(noFrame).add(engageFor).add(lastStrike).add(lastGuard).add(lastMove);
        h.add(guarding).add(out.framed).add(out.frameFallback).add(fellBack);
        if (tallGrowOn != 0) h.add(std::uint8_t{21}).add(tallGrowOn); // rule 18 (none by a target not tall: as before)
        if (t.circling) { // rule 17's state: only with it on (off, the hash is main's, to the bit)
            h.add(std::uint8_t{17});
            h.add(skirmish).add(skirmishDraws).add(skirmishOn).add(skirmishGoal.x).add(skirmishGoal.y).add(stillFor);
            h.add(placeAt.x).add(placeAt.y).add(placeFor);
            h.add(skirmishFor).add(skirmishEvery).add(skirmishDir).add(skirmishKind).add(skirmishOf);
            h.add(choice).add(choiceAt).add(walkDir.x).add(walkDir.y).add(walkAt);
            if (t.choiceHoldSeconds != d.choiceHoldSeconds || t.turnHoldSeconds != d.turnHoldSeconds)
                h.add(t.choiceHoldSeconds).add(t.turnHoldSeconds);
        }
        // The framing holds, hashed only once framed with them on (holdsFramed), so a fight with no tall enemy and
        // circling off hashes as main whatever the tuning. The bottom margin: below the sides'. The side hold: on,
        // and its clock's age, capped at the hold (a side change older than that steers nothing).
        if (holdsFramed) {
            h.add(std::uint8_t{19}).add(holdsFramed);
            if (t.frustumMarginBottom < t.frustumMargin) h.add(t.frustumMarginBottom);
            if (t.frameSideHoldSeconds > 0.f)
                h.add(std::uint8_t{20}).add(t.frameSideHoldSeconds)
                    .add(std::min(now - frameSideAt, static_cast<double>(t.frameSideHoldSeconds)));
        }
    }
    if (t.tallGrowMaxMeters != d.tallGrowMaxMeters) h.add(std::uint8_t{18}).add(t.tallGrowMaxMeters); // default: as before
    // Rule 19: only once the close camera was given (closeUsed), so a brain never given it hashes as before.
    if (closeUsed)
        h.add(std::uint8_t{22}).add(closeHold.flank).add(closeHold.angle).add(closeHold.heldFor).add(closeHold.ring);
    if (steppedOut) h.add(std::uint8_t{23}).add(steppedOut); // rule 19's step out (never set by the classic camera)
    return h.value;
}

// xorshift32, one value in [0, 1).
float PartnerBrain::draw(std::uint32_t& s, std::uint32_t& n) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    ++n;
    return static_cast<float>(s >> 8) * (1.f / 16777216.f);
}

void PartnerBrain::endString(bool staggered) {
    s_.clock = 0.0;
    s_.begunBlows = s_.blows; // no more blows: one still running runs out (committedEnd)
    s_.phase = staggered ? Phase::Ready : Phase::Pause; // rule 6: no pause on a staggered target
}

int PartnerBrain::chooseTarget(const PartnerInput& in, bool framed, std::uint32_t seen) const {
    const int n = static_cast<int>(std::min(in.enemies.size(), kMaxPartnerEnemies));
    if (in.order == PartnerOrder::StayClose) {
        const int t = in.leadTarget;
        if (t >= 0 && t < n && standing(in.enemies[static_cast<std::size_t>(t)])) return t;
        if (!framed) return -1;
        // Rule 15: no lock; the enemy the lead attacks, else the nearest standing one near the lead.
        const float nearLead = tStep_.closeLeadMeters + tStep_.reachMeters;
        int hit = -1, close = -1;
        float dHit = 0.f, dClose = 0.f;
        for (int i = 0; i < n; ++i) {
            const View& e = in.enemies[static_cast<std::size_t>(i)];
            if (!standing(e)) continue;
            const float d = edge(in.pos, e);
            if (e.leadAttacking && (hit < 0 || d < dHit)) {
                hit = i;
                dHit = d;
            }
            if (edge(in.leadPos, e) <= nearLead && (close < 0 || d < dClose)) {
                close = i;
                dClose = d;
            }
        }
        return hit >= 0 ? hit : close;
    }
    // Spread, framed: among the enemies in the frame when any stands there (rule 15), so it fights on screen.
    bool anySeen = false;
    for (int i = 0; i < n; ++i)
        anySeen = anySeen || (standing(in.enemies[static_cast<std::size_t>(i)]) && ((seen >> i) & 1u) != 0);
    int count = 0, all = -1, free = -1;
    float dAll = 0.f, dFree = 0.f;
    for (int i = 0; i < n; ++i) {
        const View& e = in.enemies[static_cast<std::size_t>(i)];
        if (!standing(e) || (anySeen && ((seen >> i) & 1u) == 0)) continue;
        ++count;
        const float d = edge(in.pos, e);
        if (all < 0 || d < dAll) {
            all = i;
            dAll = d;
        }
        if (!e.leadAttacking && (free < 0 || d < dFree)) {
            free = i;
            dFree = d;
        }
    }
    if (count <= 1) return all;
    return free >= 0 ? free : all; // the lead on every standing enemy: the nearest
}

// Every window [T - W, T] that would hold any of `seconds` more attack from now, the lead attacking no more. For the
// windows whose past part starts in slot b_i (the current slot is b_0): the partner's time is at most its sum over
// b_0..b_i, the lead's at least its sum over b_0..b_(i-1), and the new attack in the window at most
// min(seconds, W - s), s the length of b_0..b_(i-1). Slots before the fight hold nothing.
bool PartnerBrain::budgetFits(double seconds) const {
    const double w = static_cast<double>(s_.t.budgetWindowSeconds);
    const double share = static_cast<double>(s_.t.budgetShare);
    const double allow = static_cast<double>(s_.t.budgetAllowanceSeconds);
    double partner = 0.0, lead = 0.0, past = 0.0;
    for (std::size_t i = 0; i < kSlots && past < w; ++i) {
        const std::int64_t b = s_.slot - static_cast<std::int64_t>(i);
        if (b < 0) break;
        const std::size_t k = static_cast<std::size_t>(b % static_cast<std::int64_t>(kSlots));
        partner += static_cast<double>(s_.spentPartner[k]);
        if (partner + std::min(seconds, w - past) > share * lead + allow + 1e-9) return false;
        lead += static_cast<double>(s_.spentLead[k]);
        past = s_.now - static_cast<double>(b) * kSlot;
    }
    return true;
}

// `dt` from now: the partner's `partner` seconds of it are its first stretch (a blow begun runs from the step's start).
void PartnerBrain::account(double dt, double partner, bool lead) {
    double left = dt;
    double mine = partner;
    while (left > 1e-12) {
        const double end = static_cast<double>(s_.slot + 1) * kSlot;
        const double part = std::min(left, std::max(0.0, end - s_.now));
        const std::size_t k = static_cast<std::size_t>(s_.slot % static_cast<std::int64_t>(kSlots));
        const double p = std::min(part, mine);
        if (p > 0.0) s_.spentPartner[k] += static_cast<float>(p);
        if (lead) s_.spentLead[k] += static_cast<float>(part);
        mine -= p;
        s_.now += part;
        left -= part;
        if (s_.now >= end - 1e-12) {
            ++s_.slot;
            const std::size_t next = static_cast<std::size_t>(s_.slot % static_cast<std::int64_t>(kSlots));
            s_.spentPartner[next] = 0.f;
            s_.spentLead[next] = 0.f;
        }
    }
}

// Rule 2: `seconds` of attack outside its strings, ending now, into the slots they ran in (the latest first), each
// filled up to the time it held less the partner's own attack there; what finds no room in the window (or would fall
// before the fight) goes into the current slot.
void PartnerBrain::spend(float seconds) {
    if (!std::isfinite(seconds) || !(seconds > 0.f)) return;
    const double w = static_cast<double>(s_.t.budgetWindowSeconds);
    double left = std::min(static_cast<double>(seconds), w);
    const std::int64_t back = static_cast<std::int64_t>(std::ceil(w / kSlot)) + 1; // the slots a window can touch
    double end = s_.now; // where the slot's time held so far ends
    for (std::int64_t i = 0; i < back && left > 1e-12; ++i) {
        const std::int64_t b = s_.slot - i;
        if (b < 0) break;
        const double start = static_cast<double>(b) * kSlot;
        const std::size_t k = static_cast<std::size_t>(b % static_cast<std::int64_t>(kSlots));
        const double room = std::max(0.0, (end - start) - static_cast<double>(s_.spentPartner[k]));
        const double p = std::min(room, left);
        if (p > 0.0) s_.spentPartner[k] += static_cast<float>(p);
        left -= p;
        end = start;
    }
    if (left > 1e-12) {
        const std::size_t k = static_cast<std::size_t>(s_.slot % static_cast<std::int64_t>(kSlots));
        s_.spentPartner[k] += static_cast<float>(left);
    }
}

void PartnerBrain::rolesSwapped() {
    if (!s_.begun) return;
    s_.phase = Phase::Ready;
    s_.clock = 0.0;
    s_.target = -1;
    s_.returning = false;
    s_.drawn = false;
    s_.begunBlows = s_.blows;
    s_.committedEnd = s_.now; // the old body's blow is not this one's
    s_.down = false;
    s_.downFor = 0.0;
    s_.watch = {};
    s_.side = 0;
    s_.sideKey = 0;
    s_.sideTarget = -1;
    s_.sideFor = 0.0;
    s_.offStation = 0.0;
    s_.steppedOut = false;
    s_.targetFor = 0.0;
    s_.targetReached = false;
    s_.frameRing = 0;
    s_.frameTarget = -1;
    s_.noFrame = 0.0;
    s_.fellBack = false;
    s_.engageFor = 0.0;
    s_.guarding = false;
    s_.skirmishOn = false;
    s_.stillFor = 0.0;
    s_.placeFor = 0.0;
    s_.closeHold = {};
    s_.out = PartnerIntent{};
    ++s_.swaps;
}

const PartnerIntent& PartnerBrain::step(double dt, const PartnerInput& in) {
    // As FightSession's tick: not a finite number or under 0 is 0; a long step counts as kMaxPartnerStep.
    dt = std::isfinite(dt) && dt > 0.0 ? std::min(dt, kMaxPartnerStep) : 0.0;
    tStep_ = partnerEffectiveTuning(s_.t, in); // rule 19: the kit's range under the close camera; else s_.t
    if (s_.begun) spend(in.spentSeconds); // counted before this step decides
    decide(dt, in);
    PartnerIntent& o = s_.out;
    // Rule 17 (off: none of it runs); the close camera's rule 19 replaces it.
    if (s_.begun && o.framed && s_.t.circling && !in.station.close.given) skirmishStep(dt, in);
    if (s_.begun && o.framed) steerRound(in); // rule 18: a walk round a tall target, not through it
    o.idle = idleOf(o, in);
    if (s_.begun) s_.idleSeconds[static_cast<std::size_t>(o.idle)] += dt;
    if (s_.begun && o.framed && s_.fellBack) { // rule 13's fallback: every step spent on it, whatever it does
        o.frameFallback = true;
        s_.fallbackSeconds += dt;
    }
    if (s_.begun && o.framed) {
        // Rule 16's kinds, and rule 14's clock: reset by an action's start, counted while it stands in reach with
        // its next blow fitting the budget.
        const bool guardNow = o.act == PartnerAct::Guard;
        if (o.why == PartnerWhy::Start) s_.lastStrike = s_.now;
        if (guardNow && !s_.guarding) {
            s_.lastGuard = s_.now;
            ++s_.guardsBegun;
        }
        if (o.why == PartnerWhy::Reposition) s_.lastMove = s_.now;
        const bool began = o.why == PartnerWhy::Start || o.why == PartnerWhy::Reposition || (guardNow && !s_.guarding);
        s_.guarding = guardNow;
        const std::size_t n = std::min(in.enemies.size(), kMaxPartnerEnemies);
        bool eligible = false;
        if ((o.idle == PartnerIdle::Moving || o.idle == PartnerIdle::Pause) && s_.target >= 0 &&
            static_cast<std::size_t>(s_.target) < n) {
            const View& e = in.enemies[static_cast<std::size_t>(s_.target)];
            const float l = in.blowSeconds[0];
            const double next = s_.drawn ? s_.lengths[0] : static_cast<double>(l >= 0.1f && l <= 2.f ? l : s_.t.blowSeconds);
            Ground g;
            eligible = in.kitAttacks && standing(e) && inStartBand(tStep_, edge(in.pos, e)) &&
                       budgetFits(committedLeft() + next) &&
                       makeGround(in, tStep_, g) && g.roomOk(in.pos) && !g.inWedge(in.pos, 0.f) &&
                       !makeKeep(g, e).in(in.pos, 0.f);
        }
        if (began || o.idle == PartnerIdle::None) s_.engageFor = 0.0;
        else if (eligible) s_.engageFor += dt;
    }
    if (s_.begun) {
        // Its attack this step: what runs of the blows it began, or, with none running, the whole step when the host
        // says it attacks anyway.
        const double running = std::clamp(s_.committedEnd - s_.now, 0.0, dt);
        const double mine = running > 0.0 ? running : (in.doing == PartnerDoing::Attacking ? dt : 0.0);
        account(dt, mine, in.leadAttacking);
    }
    return s_.out;
}

// Rule 18: a walk (MoveTo, StepBack) whose straight line would cross a tall target's body (its radius +
// kPartnerSteerClear) goes by a point on that circle, the shorter way round: the tangent from where it stands, at least
// 20 degrees on (so a partner on the circle walks along it), never past its goal's angle.
void PartnerBrain::steerRound(const PartnerInput& in) {
    PartnerIntent& o = s_.out;
    if (o.act != PartnerAct::MoveTo && o.act != PartnerAct::StepBack) return;
    if (!stFinite(in.pos) || !stFinite(o.point)) return;
    const std::size_t n = std::min(in.enemies.size(), kMaxPartnerEnemies);
    // Every standing tall body, held or not: a goal inside one (a leash's line through it) is put out on its edge.
    for (std::size_t i = 0; i < n; ++i) {
        const View& e = in.enemies[i];
        if (!standing(e) || !tallView(e)) continue;
        const Vec2 to = stSub(o.point, e.pos);
        const float rr = e.radius + kPartnerSteerClear;
        if (std::hypot(to.x, to.y) < rr)
            o.point = stAlong(e.pos, stUnit(to, stUnit(stSub(in.pos, e.pos), Vec2{1.f, 0.f})), rr + 0.1f);
    }
    // Then round the nearest one the straight walk crosses.
    int at = -1;
    float best = kStFar;
    for (std::size_t i = 0; i < n; ++i) {
        const View& e = in.enemies[i];
        if (!standing(e) || !tallView(e) || !walksRound(in.pos, o.point, e)) continue;
        const float d = dist(in.pos, e.pos) - e.radius;
        if (d < best) {
            best = d;
            at = static_cast<int>(i);
        }
    }
    if (at < 0) return;
    const View& e = in.enemies[static_cast<std::size_t>(at)];
    const float rr = e.radius + kPartnerSteerClear;
    const Vec2 from = stSub(in.pos, e.pos), to = stSub(o.point, e.pos), d = stSub(o.point, in.pos);
    const float far = std::hypot(from.x, from.y), dd = stDot(d, d);
    if (far < 1e-3f || dd < 1e-6f) return; // on its centre (the host pushes it out), or not walking
    const float u = std::clamp(-stDot(from, d) / dd, 0.f, 1.f);
    if (std::hypot(from.x + d.x * u, from.y + d.y * u) >= rr) return; // the line keeps clear
    // Inside the circle already (pushed against the body): the point is out on it and on round (the tangent is 0).
    const float left = std::atan2(stCross(from, to), stDot(from, to)) / kStDeg; // signed, the shorter way
    const float tangent = std::acos(std::clamp(rr / far, -1.f, 1.f)) / kStDeg;
    const float turn = std::min(std::max(tangent, 20.f), std::fabs(left));
    o.point = stAlong(e.pos, stUnit(stTurn(from, left < 0.f ? -turn : turn), Vec2{1.f, 0.f}), rr + 0.1f);
}

// Rule 17 (framed): with its attack held (the budget out, a pause, a windup at it it did not guard, no target), it
// keeps moving round its target, facing it: a new valid station of rule 13 every 2-4 s, and never standing more than
// kSkirmishStill. It deals nothing and spends no budget (MoveTo only).
void PartnerBrain::skirmishStep(double dt, const PartnerInput& in) {
    skirmishWalk(dt, in);
    // Its choice of walk, for kPartnerChoiceHoldSeconds: its own goal (1) or its rules' walk (-1); a stand is neither.
    const PartnerIntent& o = s_.out;
    const std::int8_t now = o.skirmish ? std::int8_t{1}
                            : (o.act == PartnerAct::MoveTo || o.act == PartnerAct::StepBack) ? std::int8_t{-1}
                                                                                              : std::int8_t{0};
    if (now != 0 && now != s_.choice) {
        s_.choice = now;
        s_.choiceAt = s_.now;
    }
    if (now != 0) { // the way it last walked, for its next goal's turn
        const Vec2 d = stSub(o.point, in.pos);
        const float l = std::sqrt(d.x * d.x + d.y * d.y);
        if (l > 0.05f) {
            s_.walkDir = Vec2{d.x / l, d.y / l};
            s_.walkAt = s_.now;
        }
    }
}

void PartnerBrain::skirmishWalk(double dt, const PartnerInput& in) {
    constexpr double kSkirmishStill = 1.0;                   // standing this long (held), it moves: under the ruled 1.5 s
    constexpr double kSkirmishMin = 2.0, kSkirmishMax = 4.0; // a new goal this often, standing, the budget out
    constexpr float kSideStep = 40.f;                        // degrees round its target when no valid station is found
    constexpr float kShortWalk = 1.f;   // with the budget out, its rules' walk shorter than this is a step: it moves on
    constexpr float kReached = 0.2f;    // its goal is reached this near (so its last stride is not a goal jump)
    constexpr float kPlace = 0.5f;      // within this of where it stood kPlaceStill, held, it is in one place: it moves on
    constexpr double kPlaceStill = 0.8; // so that, with the time to leave the place, it stays under the ruled 1.5 s
    constexpr float kMinStride = 1.f;   // its own goal is at least this far: it leaves the place it stood in
    constexpr float kLeave = 1.5f * kPlace; // and at least this far from where that place began
    PartnerIntent& o = s_.out;
    const PartnerTuning& t = tStep_; // this step's, as every rule reads (s_.t here: it runs off the close camera alone)
    const std::size_t n = std::min(in.enemies.size(), kMaxPartnerEnemies);
    const bool haveTarget = s_.target >= 0 && static_cast<std::size_t>(s_.target) < n &&
                            standing(in.enemies[static_cast<std::size_t>(s_.target)]);
    int face = haveTarget ? s_.target : -1;
    if (face < 0) {
        float best = kStFar;
        for (std::size_t i = 0; i < n; ++i)
            if (standing(in.enemies[i]) && dist(in.pos, in.enemies[i].pos) < best) {
                best = dist(in.pos, in.enemies[i].pos);
                face = static_cast<int>(i);
            }
    }
    s_.skirmishFor += dt;
    const auto stop = [&]() {
        s_.skirmishOn = false;
        s_.stillFor = 0.0;
    };
    // Its clock in one place, run on every framed step whatever it does: shuffles under kPlace count, as standing
    // does; a blow running, a guard or no enemy standing starts it again.
    if (face < 0 || o.act == PartnerAct::Attack || o.act == PartnerAct::Guard || s_.committedEnd > s_.now ||
        dist(in.pos, s_.placeAt) > kPlace) {
        s_.placeAt = in.pos;
        s_.placeFor = 0.0;
    } else {
        s_.placeFor += dt;
    }
    // Faces its target (else the nearest enemy standing) at all times, staggered too: the host turns to intent.enemy.
    if (face >= 0 && o.act != PartnerAct::Attack && o.enemy < 0) o.enemy = face;
    if (face < 0) return stop(); // no enemy standing: nothing to keep moving for
    if (!in.canAct) { // staggered: it stands, and that counts toward its stand-still (it moves once it can)
        s_.skirmishOn = false;
        s_.stillFor += dt;
        return;
    }
    const float l = in.blowSeconds[0];
    const double next = s_.drawn ? s_.lengths[0] : static_cast<double>(l >= 0.1f && l <= 2.f ? l : t.blowSeconds);
    const bool budgetOut = !in.kitAttacks || !budgetFits(committedLeft() + next); // no kit: held as the budget out
    bool held = false;
    switch (o.why) {
    case PartnerWhy::NoKit:
    case PartnerWhy::Budget:
    case PartnerWhy::Pause:
    case PartnerWhy::HeldOff:
    case PartnerWhy::NoTarget: held = true; break;
    case PartnerWhy::Approach: held = budgetOut; break; // closing to strike: its own goal
    default: break; // acting (a string, a guard, a blow running out, a reposition) or on the leash
    }
    if (!held) return stop();
    Ground g;
    Frame fr;
    if (!makeGround(in, t, g) || !makeFrame(in.station.camera, t, fr, framingHolds(in, t))) return stop();
    fr.upper = in.station.close.given; // rule 19: middle and head only
    g.grow = tallGrowOf(in, t, s_.tallGrowOn); // rule 18: the step's own growth (one source)
    const View* e = haveTarget ? &in.enemies[static_cast<std::size_t>(s_.target)] : nullptr;
    const bool rooted = s_.placeFor + 1e-9 >= kPlaceStill; // in one place this long: a new goal now, whatever it walks
    // kind 0: its own never-still goal; 1, 2: the walk (MoveTo, StepBack) its rules gave, held with the budget out.
    const auto walk = [&](Vec2 q, std::int8_t kind) {
        o.act = kind == 2 ? PartnerAct::StepBack : PartnerAct::MoveTo;
        o.point = q;
        o.enemy = face;
        o.skirmish = kind == 0;
        s_.stillFor = 0.0;
        if (kind == 0) s_.skirmishSeconds += dt;
    };
    bool lost = false;
    if (s_.skirmishOn) { // its goal is kept until reached, while legal, out of the target's keep-out and in frame
        const Vec2 q = s_.skirmishGoal;
        if (dist(in.pos, q) > kReached && g.legal(q) && !(e && makeKeep(g, *e).in(q, t.wedgeClearMeters)) &&
            (fr.sees(q) || (s_.skirmishKind == 0 && !fr.sees(in.pos))) && s_.skirmishOf == s_.target &&
            (s_.skirmishKind != 1 || budgetOut) && !rooted)
            return walk(q, s_.skirmishKind);
        lost = s_.skirmishKind == 0 && dist(in.pos, q) > kReached; // its own goal gone (not reached): a new one now
        s_.skirmishOn = false;
    }
    bool step = false; // its rules' short step, the budget out: a goal of its own replaces it
    // Out of the frame, held: back into it now, to a goal in it (its rules' walk stands if none is found).
    const bool outside = !fr.sees(in.pos);
    // Within kPartnerChoiceHoldSeconds of its last change between its own goal and its rules' walk (in the frame),
    // it does not change back: from its own goal it stands rather than take its rules' walk (a step back still goes);
    // on its rules' walk a short step stays its rules' (out of the frame, or in one place too long: it moves at once).
    const bool choiceHeld = !outside && s_.now - s_.choiceAt < static_cast<double>(t.choiceHoldSeconds);
    if (choiceHeld && s_.choice == 1 && o.act == PartnerAct::MoveTo && !rooted) {
        o.act = PartnerAct::Wait;
        o.point = Vec2{};
    }
    if (budgetOut && o.act == PartnerAct::MoveTo && e && edge(in.pos, *e) <= t.reachMeters && !outside &&
        (dist(in.pos, o.point) < kShortWalk || (s_.skirmishKind == 0 && s_.skirmishOf == s_.target &&
                                                 dist(in.pos, s_.skirmishGoal) <= t.stationTolerance))) {
        // In reach and in the frame with the budget out, near its station or at its own last goal, it does not walk
        // to its station (it could strike nothing there): it holds, and moves on its own clock below.
        o.act = PartnerAct::Wait;
        o.why = in.kitAttacks ? PartnerWhy::Budget : PartnerWhy::NoKit;
    }
    if (rooted || lost) {
        step = true; // its rules' walk, or its kept goal, is not moving it: a goal of its own replaces it
    } else if (o.act != PartnerAct::Wait) { // walking its own rule's goal
        if (o.act != PartnerAct::MoveTo && o.act != PartnerAct::StepBack) return;
        s_.stillFor = 0.0;
        if (o.act == PartnerAct::StepBack) { // its rules' step back, as one whole stride (not a step, a stand, a step)
            const Vec2 d = stSub(o.point, in.pos);
            const float dl = std::sqrt(d.x * d.x + d.y * d.y);
            const float len = std::max(dl, kShortWalk);
            const Vec2 p{in.pos.x + d.x * len / std::max(dl, 1e-4f), in.pos.y + d.y * len / std::max(dl, 1e-4f)};
            if (dl > 1e-4f && stFinite(p) && g.legal(p) && fr.sees(p) && !(e && makeKeep(g, *e).in(p, t.wedgeClearMeters))) {
                s_.skirmishOn = true;
                s_.skirmishGoal = p;
                s_.skirmishKind = 2;
                s_.skirmishOf = s_.target;
                return walk(p, 2);
            }
        }
        if (!budgetOut && (!outside || fr.sees(o.point))) return;
        if (choiceHeld && s_.choice == -1) return; // its rules' walk, held: no goal of its own replaces it yet
        // With the budget out, that goal is kept until reached too, else its station's edge (rule 12) or the standoff
        // flips it between walking and standing on alternate steps; a step shorter than kShortWalk, or one out of
        // the frame, is replaced by a goal of its own.
        if (budgetOut && dist(in.pos, o.point) >= kShortWalk && fr.sees(o.point)) {
            s_.skirmishOn = true;
            s_.skirmishGoal = o.point;
            s_.skirmishKind = o.act == PartnerAct::StepBack ? std::int8_t{2} : std::int8_t{1};
            s_.skirmishOf = s_.target;
            return walk(o.point, s_.skirmishKind);
        }
        step = true;
    }
    const bool due = step || outside || s_.stillFor + dt + 1e-9 >= kSkirmishStill ||
                     (budgetOut && s_.skirmishFor + 1e-9 >= s_.skirmishEvery);
    if (!due) {
        s_.stillFor += dt;
        return;
    }
    Vec2 q{};
    PartnerBrainSave probe = s_; // rule 13's held station and grace are its own: rule 17 only reads them
    bool found = frameStation(g, fr, t, in.order, in.pos, e, e ? s_.target : -1, t.repositionMeters, probe, q) ||
                 frameStation(g, fr, t, in.order, in.pos, e, e ? s_.target : -1,
                              std::max(kMinStride, 0.5f * t.repositionMeters), probe, q);
    // Within kPartnerChoiceHoldSeconds of its last walk, a goal back the way it came (over 90 degrees) is taken only
    // when no side-step round its target keeps its way.
    const bool turnHeld = s_.now - s_.walkAt < static_cast<double>(t.turnHoldSeconds);
    const auto reverses = [&](Vec2 p) {
        const Vec2 d = stSub(p, in.pos);
        return turnHeld && d.x * s_.walkDir.x + d.y * s_.walkDir.y < 0.f;
    };
    // Its own goal leaves the place it stood in (else, reached, it is still in one place): kLeave from there.
    const auto leaves = [&](Vec2 p) { return dist(p, s_.placeAt) >= kLeave; };
    if (found && !leaves(q)) found = false;
    const Vec2 back = q;
    const bool backFound = found && reverses(q);
    if (backFound) found = false;
    if (!found) { // no valid station: a side-step round its target (or the lead), either way, legal and out of its keep-out
        const Vec2 c = e ? e->pos : in.leadPos;
        const Vec2 from = stSub(in.pos, c);
        // In the frame only; turned far enough that the stride is at least kMinStride.
        const float rad = std::sqrt(from.x * from.x + from.y * from.y);
        const float half = rad > 0.5f * kMinStride ? std::asin(std::min(1.f, 0.5f * kMinStride / rad)) : 0.f;
        const float turn = std::max(kSideStep, 2.f * half * 57.2957795f);
        const int tries = (rooted || lost) ? 6 : 2; // rooted, or its goal lost: wider, up to three turns each way
        for (int k = 0; k < tries && !found; ++k) {
            const float sgn = static_cast<float>(k % 2 == 0 ? s_.skirmishDir : -s_.skirmishDir);
            const Vec2 r = stTurn(from, sgn * turn * static_cast<float>(k / 2 + 1));
            const Vec2 p{c.x + r.x, c.y + r.y};
            if (stFinite(p) && !reverses(p) && leaves(p) && g.legal(p) && !(e && makeKeep(g, *e).in(p, t.wedgeClearMeters)) &&
                (!e || (edge(p, *e) <= t.reachMeters && edge(p, *e) >= t.standoffMeters)) && fr.sees(p) &&
                (in.order != PartnerOrder::StayClose || dist(p, in.leadPos) <= closeCap(t, g) + 1e-4f)) {
                q = p;
                found = true;
                if (k % 2 == 1) s_.skirmishDir = static_cast<std::int8_t>(-s_.skirmishDir);
            }
        }
    }
    // No side-step keeps its way: the station back the way it came (not to replace a goal it lost: it would undo it).
    if (!found && backFound && !(lost && !rooted)) {
        q = back;
        found = true;
    }
    // Out of the frame with no in-frame place round its target, or (StayClose) boxed in between its target's keep-out
    // and its cap, rooted or its goal lost: a station on the lead's ring (leaving its place, when in the frame).
    const bool boxed = in.order == PartnerOrder::StayClose && (rooted || lost);
    if (!found && ((outside && budgetOut) || boxed)) {
        PartnerBrainSave lead = s_;
        const View& fe = in.enemies[static_cast<std::size_t>(face)]; // out of the keep-out of the enemy it faces
        found = frameStation(g, fr, t, in.order, in.pos, nullptr, -1, outside ? 0.f : kMinStride, lead, q) &&
                !makeKeep(g, fe).in(q, t.wedgeClearMeters) && (outside || leaves(q));
    }
    if (!found) { // nowhere to go: its rules' step stands, else it stands (the stand-still report shows it)
        if (!step) s_.stillFor += dt;
        return;
    }
    s_.skirmishOn = true;
    s_.skirmishGoal = q;
    s_.skirmishKind = 0;
    s_.skirmishOf = s_.target;
    s_.skirmishFor = 0.0;
    s_.placeFor = 0.0; // a fresh kPlaceStill to leave the place
    s_.skirmishEvery = kSkirmishMin + (kSkirmishMax - kSkirmishMin) * static_cast<double>(draw(s_.skirmish, s_.skirmishDraws));
    s_.skirmishDir = static_cast<std::int8_t>(-s_.skirmishDir);
    ++s_.skirmishMoves;
    walk(q, 0);
}

const PartnerIntent& PartnerBrain::decide(double dt, const PartnerInput& in) {
    PartnerIntent& out = s_.out;
    const bool wasStill = out.act != PartnerAct::MoveTo && out.act != PartnerAct::StepBack; // the last step's
    out = PartnerIntent{};
    if (!s_.begun) return out;
    const PartnerTuning& t = tStep_; // the kit's reach and standoff under the close camera (rule 19), else s_.t
    const bool rangeOn = partnerRangeOn(in, s_.t);
    const std::size_t n = std::min(in.enemies.size(), kMaxPartnerEnemies);
    // Rule 19: the last step stepped out to the band (or held there). Set again only by the pause's step out; any
    // other step (a leash, a guard, down, a new target) drops it.
    bool wasOut = s_.steppedOut;
    s_.steppedOut = false;

    // Rule 5: down at zero, waits, rises after downSeconds (every step until the host stands it).
    if (!(in.health > 0.f)) {
        if (!s_.down) {
            s_.down = true;
            s_.downFor = 0.0;
            s_.phase = Phase::Ready;
            s_.target = -1;
            s_.returning = false;
            s_.watch = {};
            s_.side = 0;
            s_.sideFor = 0.0;
            s_.offStation = 0.0;
            s_.targetFor = 0.0;
            s_.targetReached = false;
            s_.frameRing = 0;
            s_.frameTarget = -1;
            s_.noFrame = 0.0;
            s_.fellBack = false;
            s_.engageFor = 0.0;
            s_.skirmishOn = false;
            s_.stillFor = 0.0;
            s_.placeFor = 0.0;
            s_.closeHold = {};
        } else {
            s_.downFor += dt;
        }
        out.why = PartnerWhy::Down;
        if (s_.downFor + 1e-9 >= static_cast<double>(t.downSeconds)) {
            out.rise = true;
            out.riseHealth = t.riseHealth;
        }
        return out;
    }
    s_.down = false;
    s_.downFor = 0.0;

    // Rules 8-12: the ground for this step, when the host feeds a whole station view.
    if (s_.side != 0) s_.sideFor += dt;
    if (s_.target >= 0) s_.targetFor += dt;
    Ground g;
    const bool st = makeGround(in, t, g);
    out.stationed = st;
    const auto stationFor = [&](int tgt) {
        const View& v = in.enemies[static_cast<std::size_t>(tgt)];
        return pickSide(s_, orderKey(in.order), tgt, stationOn(g, t, v, in.order, in.pos, -1.f),
                        stationOn(g, t, v, in.order, in.pos, 1.f));
    };
    const auto followFor = [&]() {
        return pickSide(s_, orderKey(in.order), -1, followOn(g, t, in.order, in.pos, -1.f),
                        followOn(g, t, in.order, in.pos, 1.f));
    };
    // Rules 13-16: the play camera, when the host gives a whole one with stationing on.
    Frame fr;
    const bool fm = st && makeFrame(in.station.camera, t, fr, framingHolds(in, t));
    fr.upper = st && in.station.close.given; // rule 19: the partner in frame by its middle and head alone
    if (fm && fr.holds) s_.holdsFramed = true; // the framing holds steer from here (sticky; hashed from here)
    // Rule 18: StayClose by a tall target (framed; one standing whose edge is within closeLeadMeters of the lead, held
    // or not, so a lead switch does not shrink them): its cap, leash and return grow by the target's radius, at most
    // tallGrowMaxMeters.
    // It starts at that edge and ends kPartnerTallGrowHysteresis past it.
    for (std::size_t i = 0; i < kMaxPartnerEnemies; ++i) {
        const std::uint8_t bit = static_cast<std::uint8_t>(1u << i);
        bool on = false;
        if (fm && in.order == PartnerOrder::StayClose && i < n) {
            const View& e = in.enemies[i];
            const float edgeFromLead = dist(e.pos, in.leadPos) - e.radius;
            const float bar = t.closeLeadMeters + ((s_.tallGrowOn & bit) ? kPartnerTallGrowHysteresis : 0.f);
            on = standing(e) && tallView(e) && edgeFromLead <= bar;
        }
        s_.tallGrowOn = static_cast<std::uint8_t>(on ? s_.tallGrowOn | bit : s_.tallGrowOn & ~bit);
    }
    g.grow = tallGrowOf(in, t, s_.tallGrowOn); // the radius, no more than tallGrowMaxMeters
    // Rule 19: the close camera (PartnerCloseStation.h), given with stationing on: between actions the partner stands
    // ahead of the lead on a flank of its target, where the close lens sees it. Its flank hold's clock runs here.
    const bool closeOn = st && in.station.close.given;
    if (closeOn) {
        s_.closeUsed = true;
        s_.closeHold.heldFor += dt;
    }
    if (fm) {
        s_.framed = true;
        s_.noFrame += dt; // since a valid station was last found
        // Rule 13's clock counts real time on every step, whatever it does: the station held is checked now too, so
        // a string or guard neither ages a station still valid nor holds a dead one past frustumGiveUpSeconds.
        if (!s_.fellBack && s_.frameRing != 0 && !closeOn) { // rule 19 keeps its own station
            const bool onTarget = s_.frameRing == 1 && s_.target >= 0 && static_cast<std::size_t>(s_.target) < n;
            const View* e = onTarget ? &in.enemies[static_cast<std::size_t>(s_.target)] : nullptr;
            Vec2 p{};
            if (frameStation(g, fr, t, in.order, in.pos, e, onTarget ? s_.target : -1, 0.f, s_, p)) s_.noFrame = 0.0;
            else if (s_.noFrame + 1e-9 >= static_cast<double>(t.frustumGiveUpSeconds)) {
                s_.fellBack = true; // reported by step() from this step on
                s_.frameRing = 0;
            }
        }
    }
    out.framed = fm;
    // Rule 13's goal for target tgt (-1: none): a valid station, else the one last held while noFrame is under
    // frustumGiveUpSeconds (whatever the target), else rules 10-11's (reported, and counted in step()).
    bool stationNow = false;    // a valid station was found this step
    bool closeNow = false;      // rule 19's station was found this step
    int fallbackClass = 3; // rule 18: the class of the fallback by a tall target (tallFallback; 3 without one)
    // Rule 19's station for target tgt; forString: one a string can start from: the band pulled in to
    // [reach / 2, reach - stationTolerance] where it is deeper (as the classic ring at reach / 2), since a partner
    // stops within stationTolerance of its station and a string needs reach. Framed, the 3D frame must see it too.
    // Single-sourced: computed once a step (the first call's target and band), later calls this step reuse it.
    int closeDoneFor = -1;
    bool closeDoneOk = false;
    Vec2 closeDonePoint{};
    const auto closeGoal = [&](int tgt, bool forString, Vec2& p) -> bool {
        if (closeDoneFor == tgt) {
            if (closeDoneOk) p = closeDonePoint;
            return closeDoneOk;
        }
        closeDoneFor = tgt;
        closeDoneOk = false;
        const PartnerCloseView& c = in.station.close;
        const View& v = in.enemies[static_cast<std::size_t>(tgt)];
        std::array<PartnerBody, kMaxPartnerEnemies> bodies{};
        std::size_t nb = 0;
        for (std::size_t i = 0; i < n; ++i)
            if (static_cast<int>(i) != tgt && standing(in.enemies[i])) bodies[nb++] = {in.enemies[i].pos, in.enemies[i].radius};
        PartnerCloseInput ci;
        ci.lead = in.leadPos;
        ci.target = v.pos;
        ci.targetRadius = v.radius;
        ci.cameraPos = in.station.cameraPos;
        ci.cameraForward = in.station.cameraForward;
        ci.halfAngleDegrees = c.halfAngleDegrees;
        ci.aimConeDegrees = c.aimConeDegrees;
        ci.minAhead = c.minAhead;
        ci.pitchDegrees = c.pitchDegrees;
        ci.verticalHalfDegrees = c.verticalHalfDegrees;
        ci.lensHeight = c.lensHeight;
        ci.aimHeight = c.aimHeight;
        if (fm) { // ruling: the 3D frame is the measure whenever the camera is given
            ci.sees = [](const void* ctx, Vec2 at) { return static_cast<const Frame*>(ctx)->sees(at); };
            ci.seesContext = &fr;
            if (!(c.verticalHalfDegrees > 0.f)) { // the vertical score from the given camera itself
                const PartnerCameraView& lensView = in.station.camera;
                const float fwdLen = std::sqrt(lensView.forward.x * lensView.forward.x + lensView.forward.y * lensView.forward.y +
                                           lensView.forward.z * lensView.forward.z);
                ci.pitchDegrees = std::clamp(std::asin(std::clamp(lensView.forward.z / fwdLen, -1.f, 1.f)) / kStDeg, -85.f, 85.f);
                ci.verticalHalfDegrees = std::clamp(0.5f * lensView.verticalFovDegrees, 1.f, 85.f);
                ci.lensHeight = std::clamp(lensView.pos.z - lensView.groundZ, -20.f, 50.f);
                ci.aimHeight = std::clamp(0.5f * lensView.bodyHeight, 0.f, 5.f);
            }
        }
        // Ruling: under StayClose the leash holds here too (rule 18's cap, grown by a tall target).
        ci.leashMeters = in.order == PartnerOrder::StayClose ? closeCap(t, g) : 0.f;
        ci.leadSide = c.leadSide;
        ci.partner = in.pos;
        if (rangeOn) {
            // The kit's range: one band for Pause and Ready, [standoff, reach - tolerance] (a standoff past its far end:
            // that end), the host's band within it where they overlap, so a string starts where it waits.
            const float rangeHi = t.reachMeters - t.stationTolerance;
            const float rangeLo = std::min(t.standoffMeters, rangeHi);
            const float bothLo = std::max(rangeLo, c.standMin), bothHi = std::min(rangeHi, c.standMax);
            const bool overlap = bothLo <= bothHi;
            ci.standMin = overlap ? bothLo : rangeLo;
            ci.standMax = overlap ? bothHi : rangeHi;
        } else {
            ci.standMax = forString ? std::min(c.standMax, t.reachMeters - t.stationTolerance) : c.standMax;
            ci.standMin = std::min(forString ? std::min(c.standMin, 0.5f * t.reachMeters) : c.standMin, ci.standMax);
        }
        ci.bodyRadius = c.bodyRadius;
        ci.personalSpace = in.station.reviving ? 0.f : t.personalSpaceMeters;
        ci.arenaCentre = in.station.arenaCentre;
        ci.arenaRadius = in.station.arenaRadius;
        ci.walls = c.walls;
        ci.bodies = std::span<const PartnerBody>(bodies.data(), nb);
        ci.attacking = s_.phase == Phase::String || committedLeft() > 0.0 || in.doing == PartnerDoing::Attacking;
        const PartnerCloseStation r = partnerCloseStation(ci, s_.closeHold);
        if (!r.valid || (fm && !fr.sees(r.point))) return false;
        out.closeTier = r.tier;
        p = r.point;
        closeDoneOk = true;
        closeDonePoint = p;
        return true;
    };
    const auto goalFor = [&](int tgt, bool forString) -> Vec2 {
        if (closeOn && tgt >= 0) {
            Vec2 q{};
            if (closeGoal(tgt, forString, q)) { // rule 19; none legal: the rules below, as before
                s_.noFrame = 0.0;
                s_.fellBack = false;
                stationNow = true;
                closeNow = true;
                return q;
            }
        }
        if (!fm) return tgt >= 0 ? stationFor(tgt) : followFor();
        const View* e = tgt >= 0 ? &in.enemies[static_cast<std::size_t>(tgt)] : nullptr;
        Vec2 p{};
        if (frameStation(g, fr, t, in.order, in.pos, e, tgt, 0.f, s_, p)) {
            s_.noFrame = 0.0;
            s_.fellBack = false;
            stationNow = true;
            return p;
        }
        if (graceHolds(g, fr, t, e, s_)) return s_.frameLast; // rule 13's grace
        out.frameFallback = true;
        s_.fellBack = true;
        s_.frameRing = 0;
        Vec2 q{};
        if (e && tallView(*e) && tallFallback(g, fr, t, in.order, in.pos, *e, q, fallbackClass)) return q; // rule 18
        return tgt >= 0 ? stationFor(tgt) : followFor();
    };
    const bool engageDue = fm && s_.engageFor + 1e-9 >= static_cast<double>(t.engageSeconds);
    // Rule 13's hold: framed, a partner standing within twice stationTolerance of its goal stays put while its own
    // place is in frame and outside both keep-outs, so a drifting station does not start and stop it every few ticks.
    const auto near = [&](Vec2 p, const View* e) {
        const float d = dist(in.pos, p);
        // Rule 18: on a tall target the tolerance holds only a spot the camera sees unhidden (the station may stand
        // at the frame's edge, its tolerance past it).
        if (d <= t.stationTolerance) return !(fm && e && tallView(*e)) || fr.clear(e, in.pos);
        if (!fm || !wasStill || d > 2.f * t.stationTolerance) return false;
        return fr.clear(e, in.pos) && !g.inWedge(in.pos, 0.f) && !(e && makeKeep(g, *e).in(in.pos, t.wedgeClearMeters));
    };

    // Rule 3: one defence draw per windup aimed at it, when first seen with enough warning.
    bool guard = false;
    for (std::size_t i = 0; i < kMaxPartnerEnemies; ++i) {
        Watch& w = s_.watch[i];
        if (i < n && windingAt(in.enemies[i])) {
            const float left = in.enemies[i].windupLeft;
            if (!w.winding || left > w.left + 1e-4f) { // a new windup
                w.guard = left >= t.guardWarning && draw(s_.defence, s_.defenceDraws) < t.guardChance;
                w.hold = t.guardHold;
            }
            w.winding = true;
            w.left = left;
            if (w.guard) guard = true;
        } else {
            w.winding = false;
            w.left = 0.f;
            if (w.guard) {
                if (w.hold > 1e-6) {
                    guard = true;
                    w.hold -= dt;
                } else {
                    w.guard = false;
                }
            }
        }
    }

    const bool haveTarget = s_.target >= 0 && static_cast<std::size_t>(s_.target) < n &&
                            standing(in.enemies[static_cast<std::size_t>(s_.target)]);
    const bool staggered =
        haveTarget && in.enemies[static_cast<std::size_t>(s_.target)].state == View::State::Staggered;
    // The blows begun have run out (a blow begun counts to its end, whatever follows).
    const bool blowOver = s_.now + 1e-9 >= s_.committedEnd;

    // The string's clock is its blows' lengths; the pause's is the brain's.
    if (s_.phase == Phase::String) {
        if (!haveTarget) { // gone (down, or past the span): pick again, no pause on nothing
            s_.phase = Phase::Ready;
            s_.clock = 0.0;
            s_.begunBlows = s_.blows;
        } else if (blowOver && s_.begunBlows >= s_.blows) {
            endString(staggered);
        }
    } else if (s_.phase == Phase::Pause) {
        s_.clock += dt;
        if (s_.clock + 1e-9 >= s_.pause || staggered || !haveTarget || engageDue) { // rule 14: a pause ends
            s_.phase = Phase::Ready;
            s_.clock = 0.0;
        }
    }

    if (!in.canAct) {
        if (s_.phase == Phase::String) endString(staggered);
        out.why = PartnerWhy::CannotAct;
        return out;
    }
    if (guard) {
        if (s_.phase == Phase::String) endString(staggered);
        out.act = PartnerAct::Guard;
        out.why = PartnerWhy::Guard;
        return out;
    }

    // Rule 1, StayClose: past the leash, back to the lead until within returnMeters.
    if (in.order == PartnerOrder::StayClose) {
        const float d = dist(in.pos, in.leadPos);
        // Stationing: the return ends within max(returnMeters, personal space + 0.5), at a resolved point.
        const float back = st ? std::max(t.returnMeters + g.grow, t.personalSpaceMeters + 0.5f) : t.returnMeters;
        if (d > t.leashMeters + g.grow) s_.returning = true;
        else if (d <= back) s_.returning = false;
        if (s_.returning) {
            if (s_.phase == Phase::String) endString(staggered);
            out.act = PartnerAct::MoveTo;
            out.why = PartnerWhy::Leash;
            out.point = st ? g.resolve(toward(in.leadPos, in.pos, std::max(t.personalSpaceMeters + 0.05f, back - 0.5f)),
                                       back - 0.05f)
                           : in.leadPos;
            return out;
        }
    } else {
        s_.returning = false;
    }

    const auto attack = [&](int enemy, bool fresh) {
        out.act = PartnerAct::Attack;
        out.enemy = enemy;
        out.blows = s_.blows;
        out.newString = fresh;
        out.partnerBlow = true;
        out.breakValue = 0.f;
        out.extendsStagger = false;
        out.punish = in.enemies[static_cast<std::size_t>(enemy)].state == View::State::Staggered;
    };
    // What is left of the blows begun: every budget check counts it with the blow it decides.
    const double running = blowOver ? 0.0 : s_.committedEnd - s_.now;

    // Rule 19: under the close lens, a string the lead has come to hide from the lens (the partner's centre inside its
    // silhouette) ends between blows, the one running run out: the pause walks it to a station clear of the lead.
    if (s_.phase == Phase::String && blowOver && closeOn &&
        partnerCloseHiddenBehind(in.station.cameraPos, in.pos, in.leadPos, in.station.close.bodyRadius))
        endString(staggered);
    if (s_.phase == Phase::String) {
        if (blowOver) { // the next blow, decided before it starts on its whole length
            const double len = s_.lengths[static_cast<std::size_t>(s_.begunBlows)];
            if (!in.kitAttacks || !budgetFits(running + len)) { // no kit, or rule 2's budget: it would break it
                endString(staggered);
                out.why = in.kitAttacks ? PartnerWhy::Budget : PartnerWhy::NoKit;
                out.enemy = s_.target;
                return out;
            }
            out.blowStart = true;
            out.blowIndex = s_.begunBlows;
            ++s_.begunBlows;
            s_.committedEnd = s_.now + len;
        }
        attack(s_.target, false);
        out.why = PartnerWhy::String;
        return out;
    }
    if (s_.phase == Phase::Pause) {
        out.why = PartnerWhy::Pause;
        const View& e = in.enemies[static_cast<std::size_t>(s_.target)];
        if (fm && !closeOn) { // rule 16: a reposition, when the last guard or move is old and a string was struck since
            const double other = std::max(s_.lastGuard, s_.lastMove);
            if (s_.now - other + 1e-9 >= static_cast<double>(t.repositionEverySeconds) && s_.lastStrike > other) {
                Vec2 q{};
                bool moved = frameStation(g, fr, t, in.order, in.pos, &e, s_.target, t.repositionMeters, s_, q) ||
                             frameStation(g, fr, t, in.order, in.pos, &e, s_.target, 0.5f * t.repositionMeters, s_, q);
                if (moved) {
                    s_.noFrame = 0.0;
                    s_.fellBack = false;
                } else if (s_.frameRing == 0 && s_.side != 0 && !tallView(e)) { // rule 18: its own fallback holds
                    // Fallen back (rule 13): to rule 10's station on the other side, when it is legal (in its arc
                    // or not: fallen back, the frame gives no better).
                    const Spot o = stationOn(g, t, e, in.order, in.pos, s_.side < 0 ? 1.f : -1.f);
                    if (o.tier <= 2 && dist(o.p, in.pos) >= 0.5f * t.repositionMeters) {
                        s_.side = static_cast<std::int8_t>(-s_.side);
                        s_.sideFor = 0.0;
                        q = o.p;
                        moved = true;
                        out.frameFallback = true;
                        s_.fellBack = true;
                    }
                }
                if (moved) {
                    out.act = PartnerAct::MoveTo;
                    out.why = PartnerWhy::Reposition;
                    out.enemy = s_.target;
                    out.point = q;
                    return out;
                }
            }
        }
        // Rule 19 (ruling of 11-16): more than kPartnerRangeStepOut inside the kit's standoff, out to its band first.
        // Once out (within a few centimetres of the standoff), it holds there till the pause is over.
        Vec2 outTo{};
        const float at = edge(in.pos, e);
        if (st && rangeOn &&
            (at < t.standoffMeters - kPartnerRangeStepOut || (wasOut && at < t.standoffMeters - 0.05f)) &&
            rangeStepOut(g, t, e, in.order, in.pos, closeOn ? &in.station.cameraPos : nullptr,
                         in.station.close.bodyRadius, outTo)) {
            s_.offStation += dt;
            s_.steppedOut = true;
            out.act = PartnerAct::StepBack;
            out.enemy = s_.target;
            out.point = outTo;
            return out;
        }
        if (st && rangeOn && wasOut && at <= t.reachMeters) {
            s_.steppedOut = true;
            out.enemy = s_.target;
            return out;
        }
        if (st) { // rule 12: back to the station
            const Vec2 p = goalFor(s_.target, false);
            const bool there = near(p, &e);
            if (there && inStartBand(t, edge(in.pos, e))) s_.offStation = 0.0;
            else s_.offStation += dt;
            if (!there) {
                // With the kit's range, a StepBack is a step out to the standoff only: a walk to a station inside it
                // (rule 19's short of its band or on the lead ring, or a fallback's ring) is a MoveTo.
                const bool outward = !rangeOn || edge(p, e) + 1e-4f >= t.standoffMeters;
                out.act = edge(in.pos, e) < t.standoffMeters && outward ? PartnerAct::StepBack : PartnerAct::MoveTo;
                out.enemy = s_.target;
                out.point = p;
            }
        } else if (edge(in.pos, e) < t.standoffMeters) {
            out.act = PartnerAct::StepBack;
            out.enemy = s_.target;
            out.point = toward(e.pos, in.pos, e.radius + t.standoffMeters);
        }
        return out;
    }

    // Ready: pick (a Spread target kept targetHoldSeconds unless it falls, leaves reach once reached, or stops being
    // standing), close in, or start a string.
    static_assert(kMaxPartnerEnemies <= 32, "the seen mask");
    std::uint32_t seen = 0;
    if (fm)
        for (std::size_t i = 0; i < n; ++i)
            if (fr.seesWhole(in.enemies[i].pos)) seen |= 1u << i;
    int tgt = chooseTarget(in, fm, seen);
    if (in.order == PartnerOrder::Spread && s_.target >= 0 && tgt != s_.target &&
        static_cast<std::size_t>(s_.target) < n && s_.targetFor + 1e-9 < static_cast<double>(t.targetHoldSeconds)) {
        const View& was = in.enemies[static_cast<std::size_t>(s_.target)];
        // Framed, a target gone from the frame is not held over one in it.
        const bool lost = fm && tgt >= 0 && ((seen >> s_.target) & 1u) == 0 && ((seen >> tgt) & 1u) != 0;
        if (standing(was) && !lost && !(s_.targetReached && edge(in.pos, was) > t.reachMeters)) tgt = s_.target;
    }
    if (tgt != s_.target) {
        s_.targetFor = 0.0;
        s_.targetReached = false;
        s_.closeHold = {}; // rule 19: one target's flank is not carried to the next
        wasOut = false;
    }
    s_.target = tgt;
    if (tgt >= 0 && edge(in.pos, in.enemies[static_cast<std::size_t>(tgt)]) <= t.reachMeters) s_.targetReached = true;
    if (tgt < 0) {
        out.why = PartnerWhy::NoTarget;
        if (st) { // rule 11
            const Vec2 p = goalFor(-1, false);
            if (!near(p, nullptr)) {
                out.act = PartnerAct::MoveTo;
                out.point = p;
            }
        }
        return out;
    }
    const View& e = in.enemies[static_cast<std::size_t>(tgt)];
    if (st) { // rule 12: a string starts only from its station
        const Vec2 p = goalFor(tgt, true);
        const bool inReach = inStartBand(t, edge(in.pos, e));
        // Rule 18: on a tall target a string starts at its station only where the camera sees it unhidden (the
        // grace's spot may have left the frame), unless fallen back. Fallen back, it starts from a spot the camera
        // does not see when no legal ring point is seen and unhidden (class 1: from an unhidden spot only), and from a
        // hidden one only when no legal ring point is unhidden (classes 2-3): never idle for it.
        const bool hiddenOk =
            s_.fellBack && (fallbackClass >= 2 || (fallbackClass == 1 && !fr.hides(&e, in.pos)));
        const bool there = near(p, &e) && (closeNow || !fm || !tallView(e) || hiddenOk || fr.clear(&e, in.pos));
        if (there && inReach) {
            s_.offStation = 0.0; // stationed: the fallback's timers start again
        } else {
            s_.offStation += dt;
            // The fallback (no station it can reach, or one that runs from it): in reach, after stationGiveUpSeconds
            // from where it stands when that is outside the wedge and personal space; after stationAnySeconds from
            // anywhere outside personal space. Out of reach (or inside personal space) after the first: to the
            // nearest in-reach point outside personal space.
            const double off = s_.offStation;
            const bool giveUp = off + 1e-9 >= static_cast<double>(t.stationGiveUpSeconds);
            const bool any = off + 1e-9 >= static_cast<double>(t.stationAnySeconds);
            // Rule 14: when due, from where it stands outside personal space and both keep-outs. Framed, only from in
            // the frame, unless no valid station was found this step (then reported: intent.offFrame).
            // Rule 18: on a tall target, only from a spot the camera sees unhidden, unless fallen back.
            const bool seenHere = tallView(e) ? hiddenOk || fr.clear(&e, in.pos) : !stationNow || fr.sees(in.pos);
            // Rule 19 (ruling of 11-16): more than kPartnerRangeStepOut inside the kit's standoff, or stepped out to
            // its band in the pause just over, it casts from where it stands (on arrival, or on its way out, or with
            // no way out), unless it is even partly behind the lead.
            const bool inside =
                rangeOn && (wasOut || edge(in.pos, e) < t.standoffMeters - kPartnerRangeStepOut) &&
                !(closeOn && rangePartlyHidden(in.station.cameraPos, in.pos, in.leadPos, in.station.close.bodyRadius));
            const bool fromHere =
                inReach && g.roomOk(in.pos) && (!fm || seenHere) &&
                ((giveUp && !g.inWedge(in.pos, 0.f)) || any || inside ||
                 (engageDue && !g.inWedge(in.pos, 0.f) && !makeKeep(g, e).in(in.pos, 0.f)));
            if (!fromHere) {
                Vec2 goal = p;
                if (giveUp && (!inReach || !g.roomOk(in.pos))) inReachSpot(g, t, e, in.order, in.pos, goal);
                out.why = PartnerWhy::Approach;
                out.enemy = tgt;
                out.point = goal;
                if (!near(goal, &e)) out.act = PartnerAct::MoveTo; // else wait there
                return out;
            }
        }
    } else if (!inStartBand(t, edge(in.pos, e))) { // out of reach: to the ring
        Vec2 p = toward(e.pos, in.pos, e.radius + innerRing(t, rangeOn));
        if (in.order == PartnerOrder::StayClose && dist(in.leadPos, p) > t.returnMeters)
            p = toward(in.leadPos, p, t.returnMeters);
        out.act = PartnerAct::MoveTo;
        out.why = PartnerWhy::Approach;
        out.enemy = tgt;
        out.point = p;
        return out;
    }
    if (windingAt(e)) { // rule 2: never into a windup aimed at it
        out.why = PartnerWhy::HeldOff;
        out.enemy = tgt;
        return out;
    }
    if (!blowOver) { // a blow begun before its string ended still runs out
        out.why = PartnerWhy::Committed;
        out.enemy = tgt;
        return out;
    }
    if (!s_.drawn) {
        const int span = t.blowsMax - t.blowsMin + 1;
        s_.blows = std::min(t.blowsMax,
                            t.blowsMin + static_cast<int>(draw(s_.rhythm, s_.rhythmDraws) * static_cast<float>(span)));
        s_.pause = static_cast<double>(t.pauseMin) +
                   static_cast<double>(draw(s_.rhythm, s_.rhythmDraws)) * static_cast<double>(t.pauseMax - t.pauseMin);
        s_.drawn = true;
        // The lengths the host will run (read once, so a held string keeps them).
        for (std::size_t i = 0; i < kMaxPartnerBlows; ++i) {
            const float l = in.blowSeconds[i];
            s_.lengths[i] = static_cast<double>(l >= 0.1f && l <= 2.f ? l : t.blowSeconds);
        }
    }
    if (!in.kitAttacks) { // its kit has no attacks: it never strikes (no other lead's kit stands in)
        out.why = PartnerWhy::NoKit;
        out.enemy = tgt;
        return out;
    }
    if (!budgetFits(running + s_.lengths[0])) { // rule 2's budget: hold (each later blow is decided as it starts)
        out.why = PartnerWhy::Budget;
        out.enemy = tgt;
        return out;
    }
    s_.drawn = false;
    s_.phase = Phase::String;
    s_.clock = 0.0;
    s_.begunBlows = 1;
    s_.committedEnd = s_.now + s_.lengths[0];
    attack(tgt, true);
    if (fm && !fr.clear(&e, in.pos)) { // rule 14's last resort: no valid station found, it strikes from off the frame
        out.offFrame = true;
        ++s_.offFrameStarts;
    }
    out.blowStart = true;
    out.blowIndex = 0;
    out.why = PartnerWhy::Start;
    return out;
}

bool importPartnerBrain(std::string_view json, PartnerTuning& out, std::string* error, std::vector<std::string>* notes) {
    out = PartnerTuning{};
    if (notes) notes->clear();
    std::vector<std::string> errors;
    const auto finish = [&]() {
        if (errors.empty()) return true;
        out = PartnerTuning{};
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
        errors.push_back("partner brain file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("partner brain file: expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !(v.isInteger() && v.integer() == 1)) errors.push_back("version: expected 1");

    PartnerTuning t;
    const auto real = [&](const char* key, float& to, double lo, double hi) {
        const JsonValue v = root[key];
        if (v.isAbsent()) return;
        if (!v.isNumber() || !(v.number() >= lo && v.number() <= hi)) {
            errors.push_back(std::string(key) + ": expected a number " + std::to_string(lo) + ".." + std::to_string(hi));
            return;
        }
        to = static_cast<float>(v.number());
    };
    const auto whole = [&](const char* key, int& to, int lo, int hi) {
        const JsonValue v = root[key];
        if (v.isAbsent()) return;
        if (!v.isInteger() || v.integer() < lo || v.integer() > hi) {
            errors.push_back(std::string(key) + ": expected a whole " + std::to_string(lo) + ".." + std::to_string(hi));
            return;
        }
        to = static_cast<int>(v.integer());
    };
    static constexpr const char* kKeys[] = {"version",     "leashMeters", "returnMeters", "reachMeters",  "standoffMeters",
                                            "blowsMin",    "blowsMax",    "blowSeconds",  "pauseMin",     "pauseMax",
                                            "guardWarning", "guardChance", "guardHold",   "downSeconds",  "riseHealth",
                                            "blowCapShare", "overScale",  "budgetWindowSeconds", "budgetShare",
                                            "budgetAllowanceSeconds", "personalSpaceMeters", "wedgeHalfDegrees",
                                            "wedgeBeyondMeters", "wedgeClearMeters", "closeArcMin", "closeArcMax",
                                            "closeLeadMeters", "spreadArcMin", "spreadArcMax", "sideHoldSeconds",
                                            "followMin", "followMax", "stationTolerance", "stationGiveUpSeconds",
                                            "stationAnySeconds", "targetHoldSeconds", "frustumMargin",
                                            "frustumGiveUpSeconds", "engageSeconds", "repositionEverySeconds",
                                            "repositionMeters", "tallGrowMaxMeters", "frustumMarginBottom",
                                            "frameSideHoldSeconds", "choiceHoldSeconds", "turnHoldSeconds",
                                            "circling"};
    real("leashMeters", t.leashMeters, 1, 10);
    real("returnMeters", t.returnMeters, 0.5, 10);
    real("reachMeters", t.reachMeters, 0.5, 4);
    real("standoffMeters", t.standoffMeters, 0, 4);
    whole("blowsMin", t.blowsMin, 1, 6);
    whole("blowsMax", t.blowsMax, 1, 6);
    real("blowSeconds", t.blowSeconds, 0.1, 2);
    real("pauseMin", t.pauseMin, 0, 10);
    real("pauseMax", t.pauseMax, 0, 10);
    real("guardWarning", t.guardWarning, 0, 2);
    real("guardChance", t.guardChance, 0, 1);
    real("guardHold", t.guardHold, 0, 2);
    real("downSeconds", t.downSeconds, 1, 600);
    real("riseHealth", t.riseHealth, 0.01, 1);
    real("blowCapShare", t.blowCapShare, 0, 1);
    real("overScale", t.overScale, 0, 1);
    real("budgetWindowSeconds", t.budgetWindowSeconds, 5, 30);
    real("budgetShare", t.budgetShare, 0, 1);
    real("budgetAllowanceSeconds", t.budgetAllowanceSeconds, 0, 10);
    real("personalSpaceMeters", t.personalSpaceMeters, 0.5, 4);
    real("wedgeHalfDegrees", t.wedgeHalfDegrees, 5, 60);
    real("wedgeBeyondMeters", t.wedgeBeyondMeters, 0, 5);
    real("wedgeClearMeters", t.wedgeClearMeters, 0, 2);
    real("closeArcMin", t.closeArcMin, 0, 180);
    real("closeArcMax", t.closeArcMax, 0, 180);
    real("closeLeadMeters", t.closeLeadMeters, 1, 10);
    real("spreadArcMin", t.spreadArcMin, 0, 180);
    real("spreadArcMax", t.spreadArcMax, 0, 180);
    real("sideHoldSeconds", t.sideHoldSeconds, 0, 10);
    real("followMin", t.followMin, 0.5, 10);
    real("followMax", t.followMax, 0.5, 10);
    real("stationTolerance", t.stationTolerance, 0.05, 2);
    real("stationGiveUpSeconds", t.stationGiveUpSeconds, 0.5, 30);
    real("stationAnySeconds", t.stationAnySeconds, 0.5, 60);
    real("targetHoldSeconds", t.targetHoldSeconds, 0, 10);
    real("frustumMargin", t.frustumMargin, 0, 0.4);
    real("frustumGiveUpSeconds", t.frustumGiveUpSeconds, 0, 30);
    real("engageSeconds", t.engageSeconds, 0.25, 10);
    real("repositionEverySeconds", t.repositionEverySeconds, 2, 20);
    real("repositionMeters", t.repositionMeters, 0.5, 6);
    real("tallGrowMaxMeters", t.tallGrowMaxMeters, 0, 20);
    real("frustumMarginBottom", t.frustumMarginBottom, 0, 0.4);
    real("frameSideHoldSeconds", t.frameSideHoldSeconds, 0, 5);
    real("choiceHoldSeconds", t.choiceHoldSeconds, 0, 5);
    real("turnHoldSeconds", t.turnHoldSeconds, 0, 5);
    if (const JsonValue v = root["circling"]; !v.isAbsent()) {
        if (v.isBool()) t.circling = v.boolean();
        else errors.push_back("circling: expected true or false");
    }

    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view k = root.key(i);
        if (std::find(std::begin(kKeys), std::end(kKeys), k) == std::end(kKeys))
            errors.push_back(std::string(k) + ": unknown field");
    }
    if (!errors.empty()) return finish();
    // Cross-field pairs: a broken pair puts both its fields back to their defaults (noted) and keeps every other
    // field; repeated until none is broken (a reset can break another pair: leashMeters is in two).
    const PartnerTuning d;
    struct Pair {
        const char* a;
        const char* b;
        const char* rule;
    };
    const auto broken = [](const PartnerTuning& x, int i) {
        switch (i) {
        case 0: return x.returnMeters > x.leashMeters;
        case 1: return x.leashMeters < x.personalSpaceMeters + 1.f;
        case 2: return x.standoffMeters > x.reachMeters;
        case 3: return x.stationTolerance > 0.5f * x.reachMeters;
        case 4: return x.blowsMax < x.blowsMin;
        case 5: return x.pauseMax < x.pauseMin;
        case 6: return x.closeArcMax < x.closeArcMin;
        case 7: return x.spreadArcMax < x.spreadArcMin;
        case 8: return x.followMax < x.followMin;
        default: return x.stationAnySeconds < x.stationGiveUpSeconds;
        }
    };
    const auto reset = [&](PartnerTuning& x, int i) {
        switch (i) {
        case 0: x.returnMeters = d.returnMeters; x.leashMeters = d.leashMeters; break;
        case 1: x.leashMeters = d.leashMeters; x.personalSpaceMeters = d.personalSpaceMeters; break;
        case 2: x.standoffMeters = d.standoffMeters; x.reachMeters = d.reachMeters; break;
        case 3: x.stationTolerance = d.stationTolerance; x.reachMeters = d.reachMeters; break;
        case 4: x.blowsMin = d.blowsMin; x.blowsMax = d.blowsMax; break;
        case 5: x.pauseMin = d.pauseMin; x.pauseMax = d.pauseMax; break;
        case 6: x.closeArcMin = d.closeArcMin; x.closeArcMax = d.closeArcMax; break;
        case 7: x.spreadArcMin = d.spreadArcMin; x.spreadArcMax = d.spreadArcMax; break;
        case 8: x.followMin = d.followMin; x.followMax = d.followMax; break;
        default: x.stationGiveUpSeconds = d.stationGiveUpSeconds; x.stationAnySeconds = d.stationAnySeconds; break;
        }
    };
    static constexpr Pair kPairs[] = {
        {"returnMeters", "leashMeters", "returnMeters at most leashMeters"},
        {"leashMeters", "personalSpaceMeters", "leashMeters at least personalSpaceMeters + 1"},
        {"standoffMeters", "reachMeters", "standoffMeters at most reachMeters"},
        {"stationTolerance", "reachMeters", "stationTolerance at most half reachMeters"},
        {"blowsMin", "blowsMax", "blowsMin at most blowsMax"},
        {"pauseMin", "pauseMax", "pauseMin at most pauseMax"},
        {"closeArcMin", "closeArcMax", "closeArcMin at most closeArcMax"},
        {"spreadArcMin", "spreadArcMax", "spreadArcMin at most spreadArcMax"},
        {"followMin", "followMax", "followMin at most followMax"},
        {"stationGiveUpSeconds", "stationAnySeconds", "stationGiveUpSeconds at most stationAnySeconds"},
    };
    constexpr int kPairCount = static_cast<int>(sizeof kPairs / sizeof kPairs[0]);
    for (int pass = 0; pass < kPairCount + 1; ++pass) {
        bool any = false;
        for (int i = 0; i < kPairCount; ++i) {
            if (!broken(t, i)) continue;
            any = true;
            reset(t, i);
            if (notes)
                notes->push_back(std::string(kPairs[i].a) + ", " + kPairs[i].b + ": expected " + kPairs[i].rule +
                                 ", both defaults used");
        }
        if (!any) break;
    }
    if (!validPartnerTuning(t)) { // never: the defaults hold every pair
        errors.push_back("partner brain: the fields still disagree");
        return finish();
    }
    out = t;
    return true;
}

} // namespace ftcore
