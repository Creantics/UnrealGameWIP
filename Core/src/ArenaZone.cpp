#include "ftcore/ArenaZone.h"
#include "ftcore/ArenaCamera.h"
#include "ftcore/ArenaLook.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <optional>

namespace ftcore {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = 180.f / kPi;

// ---- the size ----

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

// ---- the fit ----

constexpr float kBuildingMargin = 0.6f;   // a wall keeps this far from a building's face (m)
constexpr float kCentreClear = 1.f;       // the zone's centre stays this far on the open side of a cut (m)
constexpr float kMinAreaShare = 0.5f;     // of the circle the size asked for
constexpr float kMinInradiusPad = 3.f;    // inscribed radius: the biggest body radius + this
constexpr float kMinScale = 0.65f;        // of the size asked for
constexpr float kStartWall = 0.3f;        // a start keeps its body this far inside the walls (m)
constexpr float kStartObstacle = 0.5f;    // ... and this far from an obstacle's surface
constexpr float kLaneObstacle = 1.f;      // a lane keeps this far from an obstacle's surface
constexpr float kLaneWall = 0.4f;         // ... and from the walls
constexpr int kShrinkSteps = 4;
constexpr float kShrink[kShrinkSteps] = {1.f, 0.88f, 0.77f, 0.68f};

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float lengthOf(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
Vec2 minus(Vec2 a, Vec2 b) { return Vec2{a.x - b.x, a.y - b.y}; }
Vec2 plus(Vec2 a, Vec2 b) { return Vec2{a.x + b.x, a.y + b.y}; }
Vec2 scaled(Vec2 a, float s) { return Vec2{a.x * s, a.y * s}; }
float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
float cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
Vec2 rotated(Vec2 v, float a) {
    const float c = std::cos(a), s = std::sin(a);
    return Vec2{v.x * c - v.y * s, v.x * s + v.y * c};
}

// The outward unit normal of the counter-clockwise edge a -> b.
Vec2 edgeNormal(Vec2 a, Vec2 b) {
    const Vec2 d = minus(b, a);
    const float len = lengthOf(d);
    return len > 1e-6f ? Vec2{d.y / len, -d.x / len} : Vec2{0.f, 0.f};
}

float polygonArea(const Vec2* v, std::size_t n) {
    float a = 0.f;
    for (std::size_t i = 0; i < n; ++i) a += cross(v[i], v[(i + 1) % n]);
    return 0.5f * a;
}

Vec2 polygonCentroid(const Vec2* v, std::size_t n) {
    const float a = polygonArea(v, n);
    if (std::fabs(a) < 1e-4f) {
        Vec2 m;
        for (std::size_t i = 0; i < n; ++i) m = plus(m, v[i]);
        return n ? scaled(m, 1.f / static_cast<float>(n)) : m;
    }
    Vec2 c;
    for (std::size_t i = 0; i < n; ++i) {
        const float w = cross(v[i], v[(i + 1) % n]);
        c = plus(c, scaled(plus(v[i], v[(i + 1) % n]), w));
    }
    return scaled(c, 1.f / (6.f * a));
}

float segmentDistance(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = minus(b, a);
    const float len2 = dot(ab, ab);
    const float t = len2 > 0.f ? clampf(dot(minus(p, a), ab) / len2, 0.f, 1.f) : 0.f;
    return lengthOf(minus(p, plus(a, scaled(ab, t))));
}

// ---- convex polygons (the fit's working form) ----

using Poly = std::vector<Vec2>;

// Counter-clockwise; true when convex (collinear points allowed).
bool makeConvexCcw(Poly& p) {
    if (p.size() < 3) return false;
    if (polygonArea(p.data(), p.size()) < 0.f) std::reverse(p.begin(), p.end());
    for (std::size_t i = 0; i < p.size(); ++i) {
        const Vec2 a = p[i], b = p[(i + 1) % p.size()], c = p[(i + 2) % p.size()];
        if (cross(minus(b, a), minus(c, b)) < -1e-4f) return false;
    }
    return polygonArea(p.data(), p.size()) > 1e-3f;
}

// Keeps the part with n . x >= d (Sutherland-Hodgman on a convex polygon).
Poly clipKeep(const Poly& in, Vec2 n, float d) {
    Poly out;
    const std::size_t count = in.size();
    for (std::size_t i = 0; i < count; ++i) {
        const Vec2 a = in[i], b = in[(i + 1) % count];
        const float sa = dot(n, a) - d, sb = dot(n, b) - d;
        if (sa >= 0.f) out.push_back(a);
        if ((sa >= 0.f) != (sb >= 0.f)) {
            const float t = sa / (sa - sb);
            out.push_back(plus(a, scaled(minus(b, a), t)));
        }
    }
    return out;
}

// A box's corners, counter-clockwise, grown by `grow` on every side.
void boxCorners(Vec2 at, Vec2 half, float yaw, float grow, Vec2* out) {
    const Vec2 h{half.x + grow, half.y + grow};
    const Vec2 local[4] = {{-h.x, -h.y}, {h.x, -h.y}, {h.x, h.y}, {-h.x, h.y}};
    for (int i = 0; i < 4; ++i) out[i] = plus(at, rotated(local[i], yaw));
}

// Whether two convex polygons overlap (separating axis test).
bool convexOverlap(const Vec2* a, std::size_t na, const Vec2* b, std::size_t nb) {
    for (int pass = 0; pass < 2; ++pass) {
        const Vec2* p = pass == 0 ? a : b;
        const std::size_t n = pass == 0 ? na : nb;
        for (std::size_t i = 0; i < n; ++i) {
            const Vec2 axis = edgeNormal(p[i], p[(i + 1) % n]);
            float amin = 1e30f, amax = -1e30f, bmin = 1e30f, bmax = -1e30f;
            for (std::size_t k = 0; k < na; ++k) {
                const float d = dot(axis, a[k]);
                amin = std::min(amin, d);
                amax = std::max(amax, d);
            }
            for (std::size_t k = 0; k < nb; ++k) {
                const float d = dot(axis, b[k]);
                bmin = std::min(bmin, d);
                bmax = std::max(bmax, d);
            }
            if (amax < bmin || bmax < amin) return false;
        }
    }
    return true;
}

bool pointInConvex(const Poly& p, Vec2 q, float margin) {
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (dot(edgeNormal(p[i], p[(i + 1) % p.size()]), minus(q, p[i])) > -margin) return false;
    }
    return true;
}

// ---- the zone ----

void setPolygon(ArenaZone& z, const Poly& p) {
    z.shape = ZoneShape::Polygon;
    z.vertCount = static_cast<std::uint8_t>(std::min(p.size(), kMaxZoneVerts));
    for (std::size_t i = 0; i < z.vertCount; ++i) z.verts[i] = p[i];
    z.centre = polygonCentroid(z.verts.data(), z.vertCount);
    float r = 0.f;
    for (std::size_t i = 0; i < z.vertCount; ++i) r = std::max(r, lengthOf(minus(z.verts[i], z.centre)));
    z.radius = r;
}

// The distance from p to the surface of the obstacle (negative inside); the box's local frame also comes back.
float obstacleSurface(const ZoneObstacle& o, Vec2 p) {
    if (!o.box) return lengthOf(minus(p, o.at)) - o.radius;
    const Vec2 l = rotated(minus(p, o.at), -o.yaw);
    const float qx = std::fabs(l.x) - o.half.x, qy = std::fabs(l.y) - o.half.y;
    const float outside = std::sqrt(std::max(qx, 0.f) * std::max(qx, 0.f) + std::max(qy, 0.f) * std::max(qy, 0.f));
    return outside + std::min(std::max(qx, qy), 0.f);
}

Vec2 pushOutOf(const ZoneObstacle& o, Vec2 p, float margin) {
    if (obstacleSurface(o, p) >= margin) return p;
    if (!o.box) {
        Vec2 d = minus(p, o.at);
        const float len = lengthOf(d);
        d = len > 1e-5f ? scaled(d, 1.f / len) : Vec2{1.f, 0.f};
        return plus(o.at, scaled(d, o.radius + margin));
    }
    const Vec2 l = rotated(minus(p, o.at), -o.yaw);
    const float qx = std::fabs(l.x) - o.half.x, qy = std::fabs(l.y) - o.half.y;
    Vec2 out = l;
    if (qx > 0.f || qy > 0.f) {
        // Outside the box but within margin of it: out along the way from its nearest point.
        const Vec2 c{clampf(l.x, -o.half.x, o.half.x), clampf(l.y, -o.half.y, o.half.y)};
        Vec2 d = minus(l, c);
        const float len = lengthOf(d);
        d = len > 1e-5f ? scaled(d, 1.f / len) : Vec2{1.f, 0.f};
        out = plus(c, scaled(d, margin));
    } else if (qx > qy) {
        out.x = (l.x >= 0.f ? 1.f : -1.f) * (o.half.x + margin);
    } else {
        out.y = (l.y >= 0.f ? 1.f : -1.f) * (o.half.y + margin);
    }
    return plus(o.at, rotated(out, o.yaw));
}

} // namespace

bool ArenaZone::contains(Vec2 p, float margin) const {
    if (shape == ZoneShape::Circle) return lengthOf(minus(p, centre)) <= radius - margin;
    for (std::size_t i = 0; i < vertCount; ++i) {
        if (dot(edgeNormal(verts[i], verts[(i + 1) % vertCount]), minus(p, verts[i])) > -margin) return false;
    }
    return vertCount >= 3;
}

Vec2 ArenaZone::clamp(Vec2 p, float margin) const {
    if (shape == ZoneShape::Circle) {
        const float limit = radius - margin;
        if (limit <= 0.f) return centre;
        const Vec2 d = minus(p, centre);
        const float len = lengthOf(d);
        return len <= limit ? p : plus(centre, scaled(d, limit / len));
    }
    for (int sweep = 0; sweep < 4; ++sweep) {
        bool moved = false;
        for (std::size_t i = 0; i < vertCount; ++i) {
            const Vec2 n = edgeNormal(verts[i], verts[(i + 1) % vertCount]);
            const float s = dot(n, minus(p, verts[i])) + margin;
            if (s > 1e-5f) {
                p = minus(p, scaled(n, s));
                moved = true;
            }
        }
        if (!moved) return p;
    }
    return contains(p, margin - 1e-3f) ? p : centre;
}

float ArenaZone::obstacleDistance(Vec2 p) const {
    float least = 1e9f;
    for (std::size_t i = 0; i < obstacleCount; ++i) least = std::min(least, obstacleSurface(obstacles[i], p));
    return least;
}

Vec2 ArenaZone::pushOut(Vec2 p, float margin) const {
    for (std::size_t i = 0; i < obstacleCount; ++i) p = pushOutOf(obstacles[i], p, margin);
    return p;
}

Vec2 ArenaZone::constrain(Vec2 p, float margin) const {
    for (int pass = 0; pass < 4; ++pass) {
        const Vec2 q = clamp(pushOut(p, margin), margin);
        const bool settled = std::fabs(q.x - p.x) + std::fabs(q.y - p.y) < 1e-4f;
        p = q;
        if (settled) break;
    }
    return p;
}

float ArenaZone::wallDistance(Vec2 p) const {
    if (shape == ZoneShape::Circle) return radius - lengthOf(minus(p, centre));
    float least = 1e9f;
    for (std::size_t i = 0; i < vertCount; ++i)
        least = std::min(least, -dot(edgeNormal(verts[i], verts[(i + 1) % vertCount]), minus(p, verts[i])));
    return vertCount >= 3 ? least : 0.f;
}

float ArenaZone::area() const {
    return shape == ZoneShape::Circle ? kPi * radius * radius : polygonArea(verts.data(), vertCount);
}

float ArenaZone::inradius() const {
    if (shape == ZoneShape::Circle) return radius;
    float least = 1e9f;
    for (std::size_t i = 0; i < vertCount; ++i)
        least = std::min(least, -dot(edgeNormal(verts[i], verts[(i + 1) % vertCount]), minus(centre, verts[i])));
    return vertCount >= 3 ? least : 0.f;
}

const ArenaZoneSpec* ArenaZoneTable::find(std::string_view id) const {
    for (const ArenaZoneSpec& z : zones)
        if (z.id == id) return &z;
    return nullptr;
}

const ArenaZoneSpec* ArenaZoneTable::defaultZone() const {
    if (const ArenaZoneSpec* z = find(defaultId)) return z;
    return zones.empty() ? nullptr : &zones[0];
}

// ---- the step ----

namespace {
constexpr float kBrakeBand = 1.5f;  // the last metres before a wall
constexpr float kBrakeFloor = 0.6f; // a body at the wall moves at this share of its speed
} // namespace

void arenaStep(const ArenaFight& fight, ArenaBodies& bodies, float dt, const ArenaZone& zone) {
    const ArenaDef& def = fight.def();
    const std::size_t n = fight.enemyCount();
    const float playerRadius = def.fighter.bodyRadius;
    const ArenaBodies before = bodies;
    arenaStep(fight, bodies, dt);
    if (zone.softWall) {
        // Nearing a wall, a body slows (to 60% at the wall), so it meets it without a stop.
        const auto brake = [&](Vec2& now, Vec2 was, float r) {
            const float after = zone.wallDistance(now) - r;
            if (after >= kBrakeBand || after >= zone.wallDistance(was) - r) return;
            const float share = kBrakeFloor + (1.f - kBrakeFloor) * clampf(after / kBrakeBand, 0.f, 1.f);
            now = plus(was, scaled(minus(now, was), share));
        };
        brake(bodies.player, before.player, playerRadius);
        for (std::size_t i = 0; i < n; ++i) brake(bodies.enemies[i], before.enemies[i], def.enemies[i].bodyRadius);
    }
    const auto separate = [](Vec2& a, float ra, Vec2& b, float rb) {
        const Vec2 d = minus(b, a);
        const float distance = lengthOf(d);
        const float least = ra + rb;
        if (distance >= least) return;
        const Vec2 dir = distance > 1e-4f ? scaled(d, 1.f / distance) : Vec2{1.f, 0.f};
        const float push = 0.5f * (least - distance);
        a = minus(a, scaled(dir, push));
        b = plus(b, scaled(dir, push));
    };
    for (int pass = 0; pass < 2; ++pass) {
        bool moved = false;
        const auto hold = [&](Vec2& p, float r) {
            const Vec2 q = zone.constrain(p, r);
            moved = moved || q.x != p.x || q.y != p.y;
            p = q;
        };
        hold(bodies.player, playerRadius);
        for (std::size_t i = 0; i < n; ++i) hold(bodies.enemies[i], def.enemies[i].bodyRadius);
        // Nothing the walls or the obstacles had to move: the step is arenaStep's.
        if (!moved || pass == 1) break;
        // A wall can press two bodies together: push them apart once more, then hold them again.
        for (std::size_t i = 0; i < n; ++i)
            if (fight.enemyAlive(i)) separate(bodies.player, playerRadius, bodies.enemies[i], def.enemies[i].bodyRadius);
        for (std::size_t i = 0; i < n; ++i) {
            if (!fight.enemyAlive(i)) continue;
            for (std::size_t j = i + 1; j < n; ++j)
                if (fight.enemyAlive(j))
                    separate(bodies.enemies[i], def.enemies[i].bodyRadius, bodies.enemies[j], def.enemies[j].bodyRadius);
        }
    }
}

// ---- the size ----

ArenaZoneSizing arenaZoneSizing(const ArenaDef& def) {
    ArenaZoneSizing z;
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
    return z;
}

float arenaZoneAskedRing(const ArenaZoneSpec& spec, const ArenaDef& def) {
    if (spec.ring > 0.f) return clampf(spec.ring, kMinPinnedRing, kMaxPinnedRing);
    const ArenaZoneSizing z = arenaZoneSizing(def);
    const float base = kBaseRing + kRingPerEnemy * static_cast<float>(z.enemies - 1) +
                       kRingPerAttacker * static_cast<float>(z.attackers - 1);
    return clampf(base * (0.4f + 0.6f * z.k), kMinRing, kMaxRing);
}

ZoneCamera arenaZoneCamera(const ArenaZoneSizing& /*sizing*/) {
    return ZoneCamera{};
}

namespace {

// ---- the fit ----

struct Anchor {
    Vec2 at;       // world
    float frame = 0.f; // radians: the anchor frame's turn
    int district = -1;
};

Anchor anchorOf(const ArenaZoneSpec& spec, const WorldLayout* layout) {
    Anchor a;
    if (spec.district >= 0 && layout && static_cast<std::size_t>(spec.district) < layout->districtCount()) {
        const WorldDistrict& d = layout->district(static_cast<std::size_t>(spec.district));
        a.at = d.centre;
        a.frame = std::atan2(d.forward.y, d.forward.x);
        a.district = spec.district;
    } else {
        a.at = spec.at;
    }
    return a;
}

// The shape's polygon about `centre` at ring size R, turned `turn` radians; a circle at 40 sides.
Poly templatePolygon(const ArenaZoneSpec& spec, Vec2 centre, float R, float turn) {
    Poly p;
    switch (spec.kind) {
    case ZoneKind::Circle:
        for (int i = 0; i < 40; ++i) {
            const float a = 2.f * kPi * static_cast<float>(i) / 40.f;
            p.push_back(plus(centre, Vec2{R * std::cos(a), R * std::sin(a)}));
        }
        break;
    case ZoneKind::Rect: {
        // The area of the circle of radius R, long over short = aspect.
        const float aspect = std::max(spec.aspect, 1.f);
        const float w = R * std::sqrt(kPi / (4.f * aspect));
        const float l = aspect * w;
        const float c = clampf(spec.corner, 0.f, 1.f) * w * 0.95f;
        // Corners counter-clockwise from the bottom right; each an arc of `c` about its own centre.
        const Vec2 cornerAt[4] = {{l - c, -w + c}, {l - c, w - c}, {-l + c, w - c}, {-l + c, -w + c}};
        const bool round = c > 0.05f;
        for (int k = 0; k < 4; ++k) {
            for (int s = 0; s <= (round ? 5 : 0); ++s) {
                const float a = (static_cast<float>(k - 1) + static_cast<float>(s) / 5.f) * 0.5f * kPi;
                const Vec2 q = round ? plus(cornerAt[k], Vec2{c * std::cos(a), c * std::sin(a)}) : cornerAt[k];
                p.push_back(plus(rotated(q, turn), centre));
            }
        }
        return p;
    }
    case ZoneKind::Polygon:
        for (const Vec2& q : spec.points) p.push_back(plus(centre, rotated(scaled(q, R), turn)));
        return p;
    }
    return p;
}

// The zone at `centre`, size R, turned `turn`: cut by the buildings it crosses, with the rest of what stands in it
// as obstacles. False when it can't be made (its centre is inside a building, or too many obstacles).
bool buildZone(const ArenaZoneSpec& spec, const WorldLayout* layout, const Anchor& anchor, Vec2 centre, float R, float turn,
               ArenaZone& out) {
    out = ArenaZone{};
    Poly poly = templatePolygon(spec, centre, R, turn);
    if (!makeConvexCcw(poly)) return false;
    const float reach = R * 1.6f + 2.f;
    bool cut = false;
    std::vector<ZoneObstacle> obstacles;
    if (layout) {
        for (std::size_t i = 0; i < layout->blockCount(); ++i) {
            const WorldBlock& b = layout->block(i);
            const float diag = 0.5f * std::hypot(b.width, b.depth);
            if (lengthOf(minus(b.pos, centre)) > reach + diag) continue;
            Vec2 box[4];
            boxCorners(b.pos, Vec2{b.width * 0.5f, b.depth * 0.5f}, b.yaw, kBuildingMargin, box);
            if (!convexOverlap(poly.data(), poly.size(), box, 4)) continue;
            bool inside = true;
            for (const Vec2& c : box) inside = inside && pointInConvex(poly, c, 0.3f);
            if (inside) {
                ZoneObstacle o;
                o.box = true;
                o.at = b.pos;
                o.half = Vec2{b.width * 0.5f, b.depth * 0.5f};
                o.yaw = b.yaw;
                obstacles.push_back(o);
                continue;
            }
            // The zone crosses the building: cut along the face that costs the least, keeping its centre.
            const Vec2 g = polygonCentroid(poly.data(), poly.size());
            Poly best;
            float bestArea = -1.f;
            for (int f = 0; f < 4; ++f) {
                // Outward normal of the face between corners f and f + 1 (the box is counter-clockwise).
                const Vec2 n = edgeNormal(box[f], box[(f + 1) % 4]);
                const float d = dot(n, box[f]);
                if (dot(n, g) < d + kCentreClear) continue; // the centre must stand on the open side
                Poly cutPoly = clipKeep(poly, n, d);
                if (cutPoly.size() < 3) continue;
                const float a = polygonArea(cutPoly.data(), cutPoly.size());
                if (a > bestArea) {
                    bestArea = a;
                    best = std::move(cutPoly);
                }
            }
            if (bestArea < 0.f || !makeConvexCcw(best)) return false;
            poly = std::move(best);
            cut = true;
        }
    }
    if (poly.size() > kMaxZoneVerts) return false;
    if (cut || spec.kind != ZoneKind::Circle) {
        setPolygon(out, poly);
    } else {
        out.shape = ZoneShape::Circle;
        out.centre = centre;
        out.radius = R;
    }
    // How far a circle of radius r at `at` is from the zone: negative when it touches it.
    const auto touchDistance = [&](Vec2 at, float r) {
        float away = lengthOf(minus(at, out.centre)) - out.radius; // from the zone's edge, negative inside
        if (out.shape == ZoneShape::Polygon) {
            away = 0.f;
            if (!out.contains(at, 0.f)) {
                away = 1e9f;
                for (std::size_t i = 0; i < out.vertCount; ++i)
                    away = std::min(away, segmentDistance(at, out.verts[i], out.verts[(i + 1) % out.vertCount]));
            }
        }
        return away - r;
    };
    // The zone's own cover, in the anchor's frame: what touches the zone blocks it.
    for (const ArenaZoneSpec::Cover& c : spec.cover) {
        ZoneObstacle o;
        o.authored = true;
        o.box = c.box;
        o.at = plus(anchor.at, rotated(c.at, anchor.frame));
        o.radius = c.radius;
        o.half = c.half;
        o.yaw = anchor.frame + c.yaw / kDeg;
        const float reachOfIt = c.box ? lengthOf(c.half) : c.radius;
        if (touchDistance(o.at, reachOfIt) < 0.f) obstacles.push_back(o);
    }
    if (layout) {
        const auto addCircle = [&](Vec2 at, float r) {
            if (touchDistance(at, r) < 0.f) {
                ZoneObstacle o;
                o.at = at;
                o.radius = r;
                obstacles.push_back(o);
            }
        };
        for (std::size_t i = 0; i < layout->propCount(); ++i) {
            const WorldProp& p = layout->prop(i);
            if (lengthOf(minus(p.pos, centre)) > reach) continue;
            addCircle(p.pos, p.kind == 0 ? 0.2f * p.scale + 0.3f : 0.75f * p.scale);
        }
        if (anchor.district >= 0) {
            const WorldDistrict& d = layout->district(static_cast<std::size_t>(anchor.district));
            if (lengthOf(minus(d.landmark, centre)) <= reach) addCircle(d.landmark, WorldLayout::kLandmarkRadius);
        }
    }
    if (obstacles.size() > kMaxZoneObstacles) return false;
    for (const ZoneObstacle& o : obstacles) out.obstacles[out.obstacleCount++] = o;
    return true;
}

struct Starts {
    Vec2 player;
    std::array<Vec2, kMaxArenaEnemies> enemy{};
    std::size_t count = 0;
};

// Whether a body of `r` can start at p: inside the walls and clear of obstacles.
bool startOk(const ArenaZone& z, Vec2 p, float r) {
    return z.contains(p, r + kStartWall) && z.obstacleDistance(p) >= r + kStartObstacle;
}

// Whether the line from a to b is clear of obstacles and walls.
bool laneOk(const ArenaZone& z, Vec2 a, Vec2 b) {
    const Vec2 d = minus(b, a);
    const float len = lengthOf(d);
    const int steps = std::max(1, static_cast<int>(len / 0.5f));
    for (int k = 0; k <= steps; ++k) {
        const Vec2 q = plus(a, scaled(d, static_cast<float>(k) / static_cast<float>(steps)));
        if (!z.contains(q, kLaneWall) || z.obstacleDistance(q) < kLaneObstacle) return false;
    }
    return true;
}

// Where the player starts for the fight `def` in zone z: the first of a fan of lanes, the longest spawn the zone
// allows first. False when none works.
bool findLane(const ArenaZone& z, const ArenaDef& def, ArenaFight& fight, float preferred, ArenaZoneFit& out) {
    const float bound = z.radius;
    const ArenaRing ring{z.centre, bound};
    const ArenaZoneSizing sizing = arenaZoneSizing(def);
    const float s0 = def.spawnDistance;
    const float playerR = def.fighter.bodyRadius;
    for (float share : {1.f, 0.85f, 0.7f}) {
        const float spawn = std::max(s0 * share, std::min(s0, sizing.bodyRadius + 4.f));
        for (float behind : {0.35f, 0.25f, 0.15f}) {
            for (int k = 0; k < 16; ++k) {
                const float turn = preferred + static_cast<float>((k + 1) / 2) * (kPi / 8.f) * ((k % 2) ? 1.f : -1.f);
                const Vec2 u{std::cos(turn), std::sin(turn)};
                const Vec2 start = minus(z.centre, scaled(u, behind * bound));
                ArenaDef d = def;
                d.spawnDistance = spawn;
                fight.begin(d, ring, start, u, 1u);
                bool ok = startOk(z, fight.playerStart(), playerR);
                for (std::size_t i = 0; ok && i < fight.enemyCount(); ++i) {
                    const Vec2 e = fight.enemyStart(i);
                    const float r = def.enemies[i].bodyRadius;
                    ok = startOk(z, e, r) && laneOk(z, fight.playerStart(), e) &&
                         lengthOf(minus(e, fight.playerStart())) >= std::max(r + 3.f, 0.5f * spawn);
                }
                if (ok) {
                    out.playerStart = fight.playerStart();
                    out.playerFacing = fight.playerStartFacing();
                    out.spawnDistance = spawn;
                    out.ring = ring;
                    return true;
                }
            }
        }
    }
    return false;
}

bool onFlatPad(const ArenaZone& z, const WorldLayout* layout, int district) {
    if (!layout || district < 0) return true;
    const Vec2 c = layout->district(static_cast<std::size_t>(district)).centre;
    const float limit = WorldLayout::kDistrictRadius - 1.f;
    if (z.shape == ZoneShape::Circle) return lengthOf(minus(z.centre, c)) + z.radius <= limit;
    for (std::size_t i = 0; i < z.vertCount; ++i)
        if (lengthOf(minus(z.verts[i], c)) > limit) return false;
    return true;
}

std::string fmtNum(double v) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%g", v);
    return buffer;
}

// Points along the zone's wall, about `step` metres apart, counter-clockwise.
std::vector<Vec2> wallPoints(const ArenaZone& z, float step) {
    std::vector<Vec2> pts;
    if (z.shape == ZoneShape::Circle) {
        const int n = std::max(16, static_cast<int>(2.f * kPi * z.radius / step));
        for (int i = 0; i < n; ++i) {
            const float a = 2.f * kPi * static_cast<float>(i) / static_cast<float>(n);
            pts.push_back(plus(z.centre, Vec2{z.radius * std::cos(a), z.radius * std::sin(a)}));
        }
        return pts;
    }
    for (std::size_t i = 0; i < z.vertCount; ++i) {
        const Vec2 a = z.verts[i], b = z.verts[(i + 1) % z.vertCount];
        const int n = std::max(1, static_cast<int>(lengthOf(minus(b, a)) / step));
        for (int k = 0; k < n; ++k) pts.push_back(plus(a, scaled(minus(b, a), static_cast<float>(k) / static_cast<float>(n))));
    }
    return pts;
}

// Where the wall crosses a road: runs of wall within the road's half width of a road's centre line.
void findMouths(const ArenaZone& z, const WorldLayout& layout, ArenaZoneFit& out) {
    constexpr float kStep = 0.75f;
    const float halfRoad = WorldLayout::kRoadWidth * 0.5f + 0.5f;
    const std::vector<Vec2> pts = wallPoints(z, kStep);
    const std::size_t n = pts.size();
    if (n < 3) return;
    std::vector<char> inRoad(n);
    std::size_t start = n;
    for (std::size_t i = 0; i < n; ++i) {
        inRoad[i] = layout.roadDistance(pts[i]) <= halfRoad;
        if (!inRoad[i] && start == n) start = i; // begin the walk on a point that is off a road, so no run wraps
    }
    out.mouthCount = 0;
    if (start == n) return;
    std::size_t i = 0;
    while (i < n && out.mouthCount < kMaxZoneMouths) {
        const std::size_t at = (start + i) % n;
        if (!inRoad[at]) {
            ++i;
            continue;
        }
        Vec2 sum;
        std::size_t count = 0;
        while (i < n && inRoad[(start + i) % n]) {
            sum = plus(sum, pts[(start + i) % n]);
            ++count;
            ++i;
        }
        ZoneMouth m;
        m.at = scaled(sum, 1.f / static_cast<float>(count));
        const Vec2 d = minus(m.at, z.centre);
        const float len = lengthOf(d);
        m.out = len > 1e-4f ? scaled(d, 1.f / len) : Vec2{1.f, 0.f};
        m.width = std::max(WorldLayout::kRoadWidth, static_cast<float>(count) * kStep);
        out.mouths[out.mouthCount++] = m;
    }
}

// A lens closer than this to a building's footprint (or the landmark's circle) counts as inside it: the spring arm's
// probe and the lens's near plane would already clip the face there, and the blocks are the layout's footprints, not
// the drawn walls with their trim.
constexpr float kLensClearance = 0.5f;

// The share of the wall where a player standing on it, facing in, has the lens inside a building taller than the lens.
// The lens is the one the host draws, from Core's pose and lens (arenaCameraPose, arenaCameraLens) at the zone's lock
// pitch: on the lock's line, turned the zone's framing cap (ftcore::arenaFrameTurn at the target's nearest), and fully
// aimed (the aim's rig, where the framing holds the turn at 0); a sample is blocked when any of the three lenses is.
float cameraBlockedShare(const ArenaZone& z, const WorldLayout& layout, int district, const ZoneCamera& cam) {
    ArenaFrameRules frame = kArenaFrame;
    frame.maxTurn = cam.frameTurnCapDeg; // the zone's own cap
    AimStance aimed;
    aimed.tick(aimed.rules().blendIn + 1.f, true, FieldBody::Free); // fully aimed
    struct View {
        const AimStance* aim;
        float turn; // degrees
    };
    const AimStance off;
    const View views[3] = {{&off, 0.f}, {&off, arenaFrameTurn(frame, cam.side, 0.f)}, {&aimed, 0.f}};
    const auto lensBlocked = [&](Vec2 lens, float height) {
        for (std::size_t i = 0; i < layout.blockCount(); ++i) {
            const WorldBlock& b = layout.block(i);
            if (b.height <= height || lengthOf(minus(b.pos, lens)) > 20.f) continue;
            const Vec2 l = rotated(minus(lens, b.pos), -b.yaw);
            if (std::fabs(l.x) <= b.width * 0.5f + kLensClearance && std::fabs(l.y) <= b.depth * 0.5f + kLensClearance) return true;
        }
        if (district >= 0) {
            const WorldDistrict& d = layout.district(static_cast<std::size_t>(district));
            if (lengthOf(minus(lens, d.landmark)) <= WorldLayout::kLandmarkRadius + kLensClearance) return true;
        }
        return false;
    };
    constexpr int kSamples = 48;
    int blocked = 0;
    for (int k = 0; k < kSamples; ++k) {
        const float a = 2.f * kPi * static_cast<float>(k) / static_cast<float>(kSamples);
        const Vec2 dir{std::cos(a), std::sin(a)};
        float lo = 0.f, hi = z.radius * 1.05f;
        for (int it = 0; it < 24; ++it) {
            const float mid = 0.5f * (lo + hi);
            (z.contains(plus(z.centre, scaled(dir, mid)), 0.f) ? lo : hi) = mid;
        }
        const Vec2 at = plus(z.centre, scaled(dir, lo));
        bool hit = false;
        for (const View& v : views) {
            // Facing in (the lock's line is the yaw of -dir), plus the view's turn; the lens over the feet at `at`.
            const ArenaCameraPose pose = arenaCameraPose(cam, *v.aim, ArenaFrameYaw{(a + kPi) * kDeg, v.turn}, cam.lockPitch);
            const ArenaLensAt lens = arenaCameraLens(pose);
            hit = hit || lensBlocked(plus(at, Vec2{lens.x, lens.y}), pose.pivot + lens.z);
        }
        blocked += hit ? 1 : 0;
    }
    return static_cast<float>(blocked) / static_cast<float>(kSamples);
}

} // namespace

bool fitArenaZone(const ArenaZoneSpec& spec, const WorldLayout* layout, const ArenaDef& def, ArenaZoneFit& out, std::string* error,
                  std::vector<std::string>* warnings) {
    out = ArenaZoneFit{};
    out.id = spec.id;
    out.sizing = arenaZoneSizing(def);
    out.camera = arenaZoneCamera(out.sizing);
    if (spec.frameTurnCapDeg >= 0.f) out.camera.frameTurnCapDeg = spec.frameTurnCapDeg;
    const float asked = arenaZoneAskedRing(spec, def);
    out.askedRing = asked;
    const Anchor anchor = anchorOf(spec, layout);
    out.district = anchor.district;
    const float turn = anchor.frame + spec.yaw / kDeg;
    const Vec2 shift = rotated(spec.offset, anchor.frame);
    const float bossR = out.sizing.bodyRadius;

    ArenaFight fight;
    bool found = false;
    ArenaZoneFit best;
    float bestArea = -1.f;
    float bestScale = 1.f;
    bool bestMoved = false;
    for (int step = 0; step < kShrinkSteps && !found; ++step) {
        const float R = asked * kShrink[step];
        if (R < kMinScale * asked - 1e-3f || R < bossR + 5.f) break;
        // The spawn this size gives, as the old rings did.
        ArenaDef d = def;
        float spawn = spec.spawn > 0.f ? spec.spawn : std::max(kSpawnShare * R, bossR + 4.f);
        d.spawnDistance = std::min({spawn, kMaxSpawn, 0.9f * R});
        // Centres: the anchor's own, which wins when it has room, then two steps out in eight directions.
        for (int c = 0; c < 17; ++c) {
            Vec2 centre = plus(anchor.at, shift);
            if (c > 0) {
                const float a = static_cast<float>((c - 1) % 8) * (kPi / 4.f);
                const float r = ((c - 1) / 8 == 0 ? 0.12f : 0.25f) * R;
                centre = plus(centre, Vec2{r * std::cos(a), r * std::sin(a)});
            }
            ArenaZone z;
            if (!buildZone(spec, layout, anchor, centre, R, turn, z)) continue;
            if (z.area() < kMinAreaShare * kPi * R * R || z.inradius() < bossR + kMinInradiusPad) continue;
            if (!onFlatPad(z, layout, anchor.district)) continue;
            ArenaZoneFit trial;
            trial.zone = z;
            if (!findLane(z, d, fight, turn, trial)) continue;
            bestArea = z.area();
            best = trial;
            bestScale = kShrink[step];
            bestMoved = c > 0;
            break;
        }
        found = bestArea >= 0.f;
    }
    if (!found) {
        if (error) {
            *error = "zone " + spec.id + ": no room near its anchor for a fight asking a ring of " + fmtNum(asked) +
                     " m (a body radius of " + fmtNum(bossR) + " m): the free space there is too small, too crowded or off the district's flat pad";
        }
        return false;
    }
    best.id = out.id;
    best.sizing = out.sizing;
    best.camera = out.camera;
    best.askedRing = asked;
    best.district = out.district;
    best.scale = bestScale;
    best.zone.softWall = layout != nullptr;
    // The zone's own water, in the world.
    for (const ArenaZoneSpec::Water& w : spec.water) {
        if (best.waterCount >= kMaxZoneWater) break;
        ZoneWater sheet;
        sheet.at = plus(anchor.at, rotated(w.at, anchor.frame));
        sheet.radius = w.radius;
        if (lengthOf(minus(sheet.at, best.zone.centre)) <= best.zone.radius + w.radius) best.water[best.waterCount++] = sheet;
    }
    if (layout) {
        findMouths(best.zone, *layout, best);
        best.cameraBlocked = cameraBlockedShare(best.zone, *layout, anchor.district, best.camera);
    }
    out = best;
    if (warnings && bestScale < 1.f)
        warnings->push_back("zone " + spec.id + ": shrunk to " + fmtNum(std::round(bestScale * 100.0)) + "% of the size asked (a ring of " +
                            fmtNum(std::round(asked * bestScale * 10.0) / 10.0) + " m of " + fmtNum(std::round(asked * 10.0) / 10.0) + " m): buildings or props are in the way");
    if (warnings && out.cameraBlocked > 0.f)
        warnings->push_back("zone " + spec.id + ": the camera (an arm of " + fmtNum(std::round(out.camera.arm * 10.0) / 10.0) + " m) is inside a building along " +
                            fmtNum(std::round(out.cameraBlocked * 100.0)) + "% of the zone's edge; the camera collision pulls it in there");
    if (warnings && bestMoved)
        warnings->push_back("zone " + spec.id + ": moved off its anchor to fit");
    return true;
}

bool checkArenaZone(const ArenaZoneFit& fit, const ArenaDef& defIn, const WorldLayout* layout, std::string* error) {
    std::string problems;
    const auto fail = [&](const std::string& what) {
        if (!problems.empty()) problems += '\n';
        problems += (fit.id.empty() ? std::string("zone") : fit.id) + ": " + what;
    };
    ArenaDef def = defIn;
    fit.apply(def);
    const ArenaZone& z = fit.zone;
    if (lengthOf(minus(fit.ring.centre, z.centre)) > 1e-3f || std::fabs(fit.ring.radius - z.radius) > 1e-3f)
        fail("the ring is not the zone's bounding circle");
    if (!(z.radius > 0.f) || !(z.area() > 0.f)) fail("the zone has no size");
    if (z.shape == ZoneShape::Polygon && z.vertCount < 3) fail("the polygon has fewer than three vertices");
    if (!onFlatPad(z, layout, fit.district)) fail("the zone leaves the district's flat pad");

    const ZoneCamera& c = fit.camera;
    const bool cameraOk = std::isfinite(c.arm) && std::isfinite(c.pivot) && std::isfinite(c.side) && std::isfinite(c.up) &&
                          c.arm >= 3.f && c.arm <= 40.f && c.pivot >= 0.5f && c.pivot <= 10.f && c.lockPitch <= -5.f &&
                          c.lockPitch >= -40.f;
    if (!cameraOk) fail("the camera rig is out of range");

    ArenaFight fight;
    fight.begin(def, fit.ring, fit.playerStart, fit.playerFacing, 1u);
    if (!startOk(z, fight.playerStart(), def.fighter.bodyRadius)) fail("the player's start is outside the walls or in an obstacle");
    for (std::size_t i = 0; i < fight.enemyCount(); ++i) {
        const Vec2 e = fight.enemyStart(i);
        const float r = def.enemies[i].bodyRadius;
        if (!startOk(z, e, r)) fail("enemy " + std::to_string(i) + " starts outside the walls or in an obstacle");
        if (!laneOk(z, fight.playerStart(), e)) fail("the lane to enemy " + std::to_string(i) + " crosses a wall or an obstacle");
        if (lengthOf(minus(e, fight.playerStart())) < r + 3.f) fail("the player's start is too close to enemy " + std::to_string(i));
    }
    // The zone's sheets of water stay off the starts and the lanes between them (they never block, but the first beat
    // should read clean).
    for (std::size_t w = 0; w < fit.waterCount; ++w) {
        const ZoneWater& s = fit.water[w];
        bool clear = lengthOf(minus(s.at, fight.playerStart())) >= s.radius + 1.5f;
        for (std::size_t i = 0; i < fight.enemyCount(); ++i) {
            const Vec2 e = fight.enemyStart(i);
            clear = clear && lengthOf(minus(s.at, e)) >= s.radius + def.enemies[i].bodyRadius + 1.5f &&
                    segmentDistance(s.at, fight.playerStart(), e) >= s.radius + 1.5f;
        }
        if (!clear) fail("water sheet " + std::to_string(w) + " lies on a start or the lane between the starts");
    }
    if (problems.empty()) return true;
    if (error) *error = problems;
    return false;
}

// ---- the file ----

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

void readNum(JsonValue j, const char* key, float& out, double lo, double hi, const std::string& path, std::vector<std::string>& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent()) return;
    const double d = v.number(-1e30);
    if (!(v.isNumber() && std::isfinite(d) && d >= lo && d <= hi)) {
        errors.push_back(path + "." + key + ": expected a number in [" + fmtNum(lo) + ", " + fmtNum(hi) + "]");
        return;
    }
    out = static_cast<float>(d);
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

// [x, y] within +-limit.
bool readVec(JsonValue v, Vec2& out, float limit) {
    if (!v.isArray() || v.size() != 2 || !v[0].isNumber() || !v[1].isNumber()) return false;
    const double x = v[0].number(), y = v[1].number();
    if (!(std::isfinite(x) && std::isfinite(y) && std::fabs(x) <= limit && std::fabs(y) <= limit)) return false;
    out = Vec2{static_cast<float>(x), static_cast<float>(y)};
    return true;
}

} // namespace

bool importArenaZoneTable(std::string_view json, const ArenaTable& arena, const WorldTable& worlds, ArenaZoneTable& out,
                          std::string* error, std::vector<std::string>* warnings) {
    out = ArenaZoneTable{};
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
    unknownKeys(root, "", {"version", "default", "zones"}, warnings);

    ArenaZoneTable table;
    const JsonValue zones = root["zones"];
    if (!zones.isArray() || zones.size() == 0) errors.push_back("zones: expected a non-empty array");
    for (std::uint32_t i = 0; i < zones.size() && zones.isArray(); ++i) {
        const JsonValue j = zones[i];
        const std::string path = "zones[" + std::to_string(i) + "]";
        if (!j.isObject()) {
            errors.push_back(path + ": expected an object");
            continue;
        }
        unknownKeys(j, path,
                    {"id", "world", "scene", "district", "at", "offset", "yaw", "shape", "bosses", "ring", "spawn", "attackers", "seed",
                     "water", "cover", "frameTurnCapDeg"},
                    warnings);
        ArenaZoneSpec s;
        s.id = std::string(j["id"].string());
        if (s.id.empty()) errors.push_back(path + ".id: expected an id");
        for (const ArenaZoneSpec& known : table.zones)
            if (!s.id.empty() && known.id == s.id) errors.push_back(path + ".id: duplicate id: " + s.id);

        // Where: a world and a scene, a district or a position; or no world at all.
        const JsonValue w = j["world"], sc = j["scene"], di = j["district"], at = j["at"];
        int world = -1;
        if (!w.isAbsent()) {
            world = w.isString() ? worlds.findWorld(w.string()) : -1;
            if (world < 0) errors.push_back(path + ".world: expected a world id from worlds.json");
        }
        if (!sc.isAbsent()) {
            const WorldScene* scene = sc.isString() ? worlds.findScene(sc.string()) : nullptr;
            if (!scene) {
                errors.push_back(path + ".scene: expected a scene id from worlds.json");
            } else {
                if (world >= 0 && static_cast<std::uint32_t>(world) != scene->world)
                    errors.push_back(path + ".scene: that scene is in another world than .world");
                world = static_cast<int>(scene->world);
                s.district = static_cast<int>(scene->district);
                s.scene = std::string(sc.string());
            }
        }
        if (!di.isAbsent()) {
            if (!di.isInteger() || di.integer() < 0 || world < 0 ||
                di.integer() >= static_cast<std::int64_t>(worlds.worlds[static_cast<std::size_t>(world)].districts)) {
                errors.push_back(path + ".district: expected a district of the world");
            } else {
                s.district = static_cast<int>(di.integer());
            }
        }
        if (!at.isAbsent() && !readVec(at, s.at, 2000.f)) errors.push_back(path + ".at: expected [x, y] in metres");
        if (world >= 0) {
            s.worldIndex = static_cast<std::uint32_t>(world);
            s.world = worlds.worlds[static_cast<std::size_t>(world)].id;
            if (s.district < 0 && at.isAbsent()) errors.push_back(path + ": a world zone needs a scene, a district or at");
        }
        if (const JsonValue o = j["offset"]; !o.isAbsent() && !readVec(o, s.offset, 200.f))
            errors.push_back(path + ".offset: expected [x, y] in metres");
        readNum(j, "yaw", s.yaw, -360, 360, path, errors);

        if (const JsonValue sh = j["shape"]; !sh.isAbsent()) {
            if (!sh.isObject()) {
                errors.push_back(path + ".shape: expected an object");
            } else {
                unknownKeys(sh, path + ".shape", {"kind", "aspect", "corner", "points"}, warnings);
                const std::string_view kind = sh["kind"].string();
                if (kind == "circle") s.kind = ZoneKind::Circle;
                else if (kind == "rect") s.kind = ZoneKind::Rect;
                else if (kind == "polygon") s.kind = ZoneKind::Polygon;
                else errors.push_back(path + ".shape.kind: expected circle, rect or polygon");
                readNum(sh, "aspect", s.aspect, 1, 4, path + ".shape", errors);
                readNum(sh, "corner", s.corner, 0, 1, path + ".shape", errors);
                if (s.kind == ZoneKind::Polygon) {
                    const JsonValue pts = sh["points"];
                    if (!pts.isArray() || pts.size() < 3 || pts.size() > 24) {
                        errors.push_back(path + ".shape.points: expected 3..24 points [x, y] in multiples of the ring radius");
                    } else {
                        for (std::uint32_t p = 0; p < pts.size(); ++p) {
                            Vec2 v;
                            if (!readVec(pts[p], v, 3.f)) errors.push_back(path + ".shape.points[" + std::to_string(p) + "]: expected [x, y] within 3");
                            s.points.push_back(v);
                        }
                        Poly check(s.points.begin(), s.points.end());
                        if (!makeConvexCcw(check)) errors.push_back(path + ".shape.points: expected a convex polygon");
                    }
                }
            }
        }

        if (const JsonValue wl = j["water"]; !wl.isAbsent()) {
            if (!wl.isArray() || wl.size() > kMaxZoneWater) {
                errors.push_back(path + ".water: expected an array of at most " + std::to_string(kMaxZoneWater) + " sheets");
            } else {
                for (std::uint32_t k = 0; k < wl.size(); ++k) {
                    const std::string wp = path + ".water[" + std::to_string(k) + "]";
                    ArenaZoneSpec::Water sheet;
                    if (!readVec(wl[k]["at"], sheet.at, 200.f)) errors.push_back(wp + ".at: expected [x, y] in metres");
                    readNum(wl[k], "radius", sheet.radius, 0.5, 30, wp, errors);
                    unknownKeys(wl[k], wp, {"at", "radius"}, warnings);
                    s.water.push_back(sheet);
                }
            }
        }
        if (const JsonValue cl = j["cover"]; !cl.isAbsent()) {
            if (!cl.isArray() || cl.size() > 16) {
                errors.push_back(path + ".cover: expected an array of at most 16 pieces");
            } else {
                for (std::uint32_t k = 0; k < cl.size(); ++k) {
                    const std::string cp = path + ".cover[" + std::to_string(k) + "]";
                    ArenaZoneSpec::Cover piece;
                    const std::string_view kind = cl[k]["kind"].string();
                    if (kind == "box") piece.box = true;
                    else if (kind != "circle") errors.push_back(cp + ".kind: expected circle or box");
                    if (!readVec(cl[k]["at"], piece.at, 200.f)) errors.push_back(cp + ".at: expected [x, y] in metres");
                    readNum(cl[k], "radius", piece.radius, 0.2, 10, cp, errors);
                    if (const JsonValue h = cl[k]["half"]; !h.isAbsent() && !readVec(h, piece.half, 20.f))
                        errors.push_back(cp + ".half: expected [x, y] in metres");
                    readNum(cl[k], "yaw", piece.yaw, -360, 360, cp, errors);
                    unknownKeys(cl[k], cp, {"kind", "at", "radius", "half", "yaw"}, warnings);
                    s.cover.push_back(piece);
                }
            }
        }

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
        readNum(j, "ring", s.ring, 0, kMaxPinnedRing, path, errors);
        if (s.ring != 0.f && s.ring < kMinPinnedRing)
            errors.push_back(path + ".ring: expected 0 (from the enemies' size) or " + fmtNum(kMinPinnedRing) + ".." + fmtNum(kMaxPinnedRing));
        readNum(j, "spawn", s.spawn, 0, kMaxSpawn, path, errors);
        readNum(j, "frameTurnCapDeg", s.frameTurnCapDeg, 0, kArenaFrame.maxTurn, path, errors);
        readInt(j, "attackers", s.attackers, 0, static_cast<int>(kMaxArenaEnemies), path, errors);
        int seed = 1;
        readInt(j, "seed", seed, 0, 1000000, path, errors);
        s.seed = static_cast<std::uint32_t>(seed);
        table.zones.push_back(std::move(s));
    }

    if (const JsonValue d = root["default"]; !d.isAbsent()) {
        table.defaultId = std::string(d.string());
        if (!d.isString() || !table.find(table.defaultId)) errors.push_back("default: expected the id of a zone");
    } else if (!table.zones.empty()) {
        table.defaultId = table.zones[0].id;
    }

    if (!finish()) return false;
    out = std::move(table);
    return true;
}

} // namespace ftcore
