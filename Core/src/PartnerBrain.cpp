#include "ftcore/PartnerBrain.h"

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
float closeCap(const PartnerTuning& t) { return std::min(t.closeLeadMeters, t.leashMeters - 0.5f); }

// Rule 10: the best station on side s (+1: counter-clockwise from the lead, seen at the target).
Spot stationOn(const Ground& g, const PartnerTuning& t, const PartnerEnemyView& e, PartnerOrder order, Vec2 pos,
               float s) {
    const bool close = order == PartnerOrder::StayClose;
    const float lo = close ? t.closeArcMin : t.spreadArcMin, hi = close ? t.closeArcMax : t.spreadArcMax;
    const float cap = closeCap(t);
    const float ring = e.radius + 0.5f * t.reachMeters;
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
    const float hi = std::max(order == PartnerOrder::StayClose ? std::min(t.followMax, closeCap(t)) : t.followMax, lo);
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
    const float ring = e.radius + 0.5f * t.reachMeters;
    const float cap = order == PartnerOrder::StayClose ? closeCap(t) : kStFar;
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
bool v3Finite(V3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

// Rule 13's frustum, shrunk by the margin: a capsule's three points each in front of the camera and within lim of
// the screen's centre in its -1..1 frame.
struct Frame {
    V3 c{}, f{}, u{}, r{};
    float tanV = 0.f, tanH = 0.f, lim = 0.8f, z0 = 0.f, h = 1.8f;
    bool point(float x, float y, float z) const {
        const V3 d{x - c.x, y - c.y, z - c.z};
        const float depth = v3Dot(d, f);
        if (!(depth > 0.05f)) return false;
        const float sx = v3Dot(d, r) / (depth * tanH), sy = v3Dot(d, u) / (depth * tanV);
        return std::fabs(sx) <= lim && std::fabs(sy) <= lim;
    }
    bool sees(Vec2 p) const { return point(p.x, p.y, z0) && point(p.x, p.y, z0 + 0.5f * h) && point(p.x, p.y, z0 + h); }
};

// The camera view, whole (PartnerCameraView's rules), or not given.
bool makeFrame(const PartnerCameraView& v, const PartnerTuning& t, Frame& fr) {
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
bool frameStation(const Ground& g, const Frame& fr, const PartnerTuning& t, PartnerOrder order, Vec2 pos,
                  const PartnerEnemyView* e, int tgt, float minFrom, PartnerBrainSave& s, Vec2& out) {
    const bool close = order == PartnerOrder::StayClose;
    const float cap = close ? closeCap(t) : kStFar;
    const Keep k = e ? makeKeep(g, *e) : Keep{};
    const auto valid = [&](Vec2 q) {
        return stFinite(q) && g.legal(q) && dist(q, g.lead) <= cap + 1e-4f && !k.in(q, t.wedgeClearMeters) && fr.sees(q);
    };
    const Vec2 c = e ? e->pos : g.lead;
    const std::uint8_t ring = e ? 1 : 2;
    std::array<float, 3> radii{};
    int nr = 1;
    float lo = 110.f, hi = 160.f; // rule 11's
    if (e) {
        // Half the reach off its edge, else (a second tier) as far out as still arrives in reach.
        radii[0] = e->radius + 0.5f * t.reachMeters;
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
    if (minFrom <= 0.f && s.frameRing == ring && (ring == 2 || s.frameTarget == tgt)) {
        const Vec2 q = stAlong(c, stTurn(Vec2{1.f, 0.f}, s.frameAngle), s.frameRadius);
        if (valid(q)) {
            s.frameLast = q;
            out = q;
            return true;
        }
    }
    Spot best;
    float bestAngle = 0.f, bestRadius = 0.f;
    for (int ri = 0; ri < nr; ++ri)
        for (int a = 0; a < 72; ++a) {
            const float ang = 5.f * static_cast<float>(a);
            const Vec2 q = stAlong(c, stTurn(Vec2{1.f, 0.f}, ang), radii[static_cast<std::size_t>(ri)]);
            if (!valid(q) || dist(q, pos) < minFrom) continue;
            const float deg = e ? stAngle(stSub(g.lead, c), stSub(q, c)) : stAngle(g.facing, stSub(q, c));
            const Spot cand{q, (deg >= lo - 1e-3f && deg <= hi + 1e-3f ? 0 : 1) + (e && ri > 0 ? 2 : 0), dist(q, pos)};
            if (better(cand, best)) {
                best = cand;
                bestAngle = ang;
                bestRadius = radii[static_cast<std::size_t>(ri)];
            }
        }
    if (best.tier >= 9) return false;
    s.frameRing = ring;
    s.frameTarget = e ? tgt : -1;
    s.frameAngle = bestAngle;
    s.frameRadius = bestRadius;
    s.frameLast = best.p;
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
    default: return PartnerIdle::Other; // NotBegun, HeldOff
    }
}

} // namespace

bool partnerInFrame(const PartnerCameraView& camera, float margin, Vec2 p) {
    PartnerTuning t;
    t.frustumMargin = margin;
    Frame fr;
    return pbWithin(margin, 0.f, 0.4f) && stFinite(p) && makeFrame(camera, t, fr) && fr.sees(p);
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
           pbWithin(t.repositionEverySeconds, 2.f, 20.f) && pbWithin(t.repositionMeters, 0.5f, 6.f);
}

static_assert(kMaxPartnerStep == kMaxFightStep, "the brain's step bound is the fight's");

bool PartnerBrain::begin(const PartnerTuning& t, std::uint32_t seed) {
    const bool ok = validPartnerTuning(t);
    s_ = PartnerBrainSave{};
    s_.t = ok ? t : PartnerTuning{};
    s_.rhythm = mix(seed);
    s_.defence = mix(seed ^ 0xA5C3F00Du);
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
                       t.repositionMeters != d.repositionMeters;
    if (framed || tuned) {
        h.add(std::uint8_t{13});
        h.add(t.frustumMargin).add(t.frustumGiveUpSeconds).add(t.engageSeconds).add(t.repositionEverySeconds);
        h.add(t.repositionMeters).add(framed).add(frameRing).add(frameTarget).add(frameAngle).add(frameRadius);
        h.add(frameLast.x).add(frameLast.y).add(noFrame).add(engageFor).add(lastStrike).add(lastGuard).add(lastMove);
        h.add(guarding).add(out.framed).add(out.frameFallback).add(fellBack);
    }
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
        const float nearLead = s_.t.closeLeadMeters + s_.t.reachMeters;
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
    s_.targetFor = 0.0;
    s_.targetReached = false;
    s_.frameRing = 0;
    s_.frameTarget = -1;
    s_.noFrame = 0.0;
    s_.fellBack = false;
    s_.engageFor = 0.0;
    s_.guarding = false;
    s_.out = PartnerIntent{};
    ++s_.swaps;
}

const PartnerIntent& PartnerBrain::step(double dt, const PartnerInput& in) {
    // As FightSession's tick: not a finite number or under 0 is 0; a long step counts as kMaxPartnerStep.
    dt = std::isfinite(dt) && dt > 0.0 ? std::min(dt, kMaxPartnerStep) : 0.0;
    if (s_.begun) spend(in.spentSeconds); // counted before this step decides
    decide(dt, in);
    PartnerIntent& o = s_.out;
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
        if (guardNow && !s_.guarding) s_.lastGuard = s_.now;
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
            eligible = standing(e) && edge(in.pos, e) <= s_.t.reachMeters && budgetFits(committedLeft() + next) &&
                       makeGround(in, s_.t, g) && g.roomOk(in.pos) && !g.inWedge(in.pos, 0.f) &&
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

const PartnerIntent& PartnerBrain::decide(double dt, const PartnerInput& in) {
    PartnerIntent& out = s_.out;
    const bool wasStill = out.act != PartnerAct::MoveTo && out.act != PartnerAct::StepBack; // the last step's
    out = PartnerIntent{};
    if (!s_.begun) return out;
    const PartnerTuning& t = s_.t;
    const std::size_t n = std::min(in.enemies.size(), kMaxPartnerEnemies);

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
    const bool fm = st && makeFrame(in.station.camera, t, fr);
    if (fm) {
        s_.framed = true;
        s_.noFrame += dt; // since a valid station was last found
    }
    out.framed = fm;
    // Rule 13's goal for target tgt (-1: none): a valid station, else the one last held while noFrame is under
    // frustumGiveUpSeconds (whatever the target), else rules 10-11's (reported, and counted in step()).
    bool stationNow = false; // a valid station was found this step
    const auto goalFor = [&](int tgt) -> Vec2 {
        if (!fm) return tgt >= 0 ? stationFor(tgt) : followFor();
        const View* e = tgt >= 0 ? &in.enemies[static_cast<std::size_t>(tgt)] : nullptr;
        Vec2 p{};
        if (frameStation(g, fr, t, in.order, in.pos, e, tgt, 0.f, s_, p)) {
            s_.noFrame = 0.0;
            s_.fellBack = false;
            stationNow = true;
            return p;
        }
        // The grace holds the last station while it stays legal and out of this target's keep-out (it may have
        // left the frame: the camera can come back to it).
        if (!s_.fellBack && s_.frameRing != 0 && s_.noFrame + 1e-9 < static_cast<double>(t.frustumGiveUpSeconds) &&
            g.legal(s_.frameLast) && !(e && makeKeep(g, *e).in(s_.frameLast, t.wedgeClearMeters)))
            return s_.frameLast;
        out.frameFallback = true;
        s_.fellBack = true;
        s_.frameRing = 0;
        return tgt >= 0 ? stationFor(tgt) : followFor();
    };
    const bool engageDue = fm && s_.engageFor + 1e-9 >= static_cast<double>(t.engageSeconds);
    // Rule 13's hold: framed, a partner standing within twice stationTolerance of its goal stays put while its own
    // place is in frame and outside both keep-outs, so a drifting station does not start and stop it every few ticks.
    const auto near = [&](Vec2 p, const View* e) {
        const float d = dist(in.pos, p);
        if (d <= t.stationTolerance) return true;
        if (!fm || !wasStill || d > 2.f * t.stationTolerance) return false;
        return fr.sees(in.pos) && !g.inWedge(in.pos, 0.f) && !(e && makeKeep(g, *e).in(in.pos, t.wedgeClearMeters));
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
        const float back = st ? std::max(t.returnMeters, t.personalSpaceMeters + 0.5f) : t.returnMeters;
        if (d > t.leashMeters) s_.returning = true;
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

    if (s_.phase == Phase::String) {
        if (blowOver) { // the next blow, decided before it starts on its whole length
            const double len = s_.lengths[static_cast<std::size_t>(s_.begunBlows)];
            if (!budgetFits(running + len)) { // rule 2's budget: it would break it
                endString(staggered);
                out.why = PartnerWhy::Budget;
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
        if (fm) { // rule 16: a reposition, when the last guard or move is old and a string was struck since
            const double other = std::max(s_.lastGuard, s_.lastMove);
            if (s_.now - other + 1e-9 >= static_cast<double>(t.repositionEverySeconds) && s_.lastStrike > other) {
                Vec2 q{};
                bool moved = frameStation(g, fr, t, in.order, in.pos, &e, s_.target, t.repositionMeters, s_, q) ||
                             frameStation(g, fr, t, in.order, in.pos, &e, s_.target, 0.5f * t.repositionMeters, s_, q);
                if (moved) {
                    s_.noFrame = 0.0;
                    s_.fellBack = false;
                } else if (s_.frameRing == 0 && s_.side != 0) {
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
        if (st) { // rule 12: back to the station
            const Vec2 p = goalFor(s_.target);
            const bool there = near(p, &e);
            if (there && edge(in.pos, e) <= t.reachMeters) s_.offStation = 0.0;
            else s_.offStation += dt;
            if (!there) {
                out.act = edge(in.pos, e) < t.standoffMeters ? PartnerAct::StepBack : PartnerAct::MoveTo;
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
            if (fr.sees(in.enemies[i].pos)) seen |= 1u << i;
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
    }
    s_.target = tgt;
    if (tgt >= 0 && edge(in.pos, in.enemies[static_cast<std::size_t>(tgt)]) <= t.reachMeters) s_.targetReached = true;
    if (tgt < 0) {
        out.why = PartnerWhy::NoTarget;
        if (st) { // rule 11
            const Vec2 p = goalFor(-1);
            if (!near(p, nullptr)) {
                out.act = PartnerAct::MoveTo;
                out.point = p;
            }
        }
        return out;
    }
    const View& e = in.enemies[static_cast<std::size_t>(tgt)];
    if (st) { // rule 12: a string starts only from its station
        const Vec2 p = goalFor(tgt);
        const bool inReach = edge(in.pos, e) <= t.reachMeters;
        const bool there = near(p, &e);
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
            const bool fromHere =
                inReach && g.roomOk(in.pos) && (!fm || !stationNow || fr.sees(in.pos)) &&
                ((giveUp && !g.inWedge(in.pos, 0.f)) || any ||
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
    } else if (edge(in.pos, e) > t.reachMeters) {
        Vec2 p = toward(e.pos, in.pos, e.radius + 0.5f * t.reachMeters);
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
    if (fm && !fr.sees(in.pos)) { // rule 14's last resort: no valid station found, it strikes from off the frame
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
                                            "repositionMeters"};
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
