#include "ftcore/ArenaImpact.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ftcore {

namespace {

constexpr float kPi = 3.14159265358979f;

float clampOr0(float v, float lo, float hi) {
    if (!(v >= lo)) return lo; // NaN too
    return v > hi ? hi : v;
}

// 1 at the kick, easing to 0 at kArenaJoltLength (squared, so it settles softly); 0 after.
float envelope(float age) {
    if (!(age >= 0.f) || age >= kArenaJoltLength) return 0.f;
    const float k = 1.f - age / kArenaJoltLength;
    return k * k;
}

const ArenaAttack* enemyAttack(const ArenaDef& def, std::uint8_t enemy, std::uint8_t move) {
    if (enemy >= def.enemyCount || enemy >= kMaxArenaEnemies) return nullptr;
    const ArenaEnemyKind& kind = def.enemies[enemy];
    if (move >= kind.moveCount || move >= kMaxArenaMoves) return nullptr;
    return &kind.moves[move].attack;
}

} // namespace

float arenaJoltOf(const ArenaDef& def, const ArenaEvent& e) {
    if (e.type == ArenaEventType::Hit && e.source == kArenaPlayer) {
        const ArenaAttack* a = arenaPlayerAttack(def.fighter, e.move);
        return a ? a->jolt : 0.f;
    }
    if ((e.type == ArenaEventType::Hit || e.type == ArenaEventType::Blocked) && e.target == kArenaPlayer) {
        const ArenaAttack* a = enemyAttack(def, e.source, e.move);
        if (!a) return 0.f;
        const float own = a->jolt > 0.f ? a->jolt : def.fighter.hurtJolt;
        return e.type == ArenaEventType::Blocked ? own * 0.5f : own;
    }
    return 0.f;
}

void ArenaJolt::kick(float degrees, float scale, float sideSign) {
    const float want = clampOr0(degrees, 0.f, kArenaMaxJolt) * clampOr0(scale, 0.f, 1.f);
    if (!(want > left())) return;
    amount = want;
    age = 0.f;
    side = sideSign < 0.f ? -1.f : 1.f;
}

void ArenaJolt::step(float dt) {
    if (!(dt > 0.f) || amount <= 0.f) return;
    age += dt;
    if (age >= kArenaJoltLength) {
        amount = 0.f;
        age = 0.f;
    }
}

float ArenaJolt::left() const { return amount > 0.f ? amount * envelope(age) : 0.f; }

// The view dips at the kick and swings back through its rest point, kArenaJoltCycles times.
float ArenaJolt::pitch() const {
    const float l = left();
    if (l <= 0.f) return 0.f;
    return -l * std::cos(2.f * kPi * kArenaJoltCycles * age / kArenaJoltLength);
}

float ArenaJolt::yaw() const {
    const float l = left();
    if (l <= 0.f) return 0.f;
    return side * kArenaJoltYawShare * l * std::sin(2.f * kPi * kArenaJoltCycles * age / kArenaJoltLength);
}

float arenaBearing(Vec2 eye, Vec2 forward, Vec2 to) {
    const float dx = to.x - eye.x, dy = to.y - eye.y;
    if (!(dx * dx + dy * dy > 1e-12f)) return 0.f;
    const float cross = forward.x * dy - forward.y * dx; // + : to the right (y right of x forward)
    const float dot = forward.x * dx + forward.y * dy;
    if (!(cross * cross + dot * dot > 0.f)) return 0.f; // a zero forward
    return std::atan2(cross, dot) * (180.f / kPi);
}

namespace {

// -1: the point (bearing degrees off the view's heading, `ahead` m away on the ground, `up` m over the lens) is under
// the view's vertical edge (or behind the lens and under it), +1 over it, 0 inside. `tanEdge` is tan of the edge.
int verticalSide(float pitchDeg, float bearingDeg, float ahead, float up, float tanEdge) {
    const float p = pitchDeg * (kPi / 180.f);
    const float f = ahead * std::cos(bearingDeg * (kPi / 180.f)); // along the view's heading, on the ground
    const float x = f * std::cos(p) + up * std::sin(p);         // along the view
    const float z = -f * std::sin(p) + up * std::cos(p);        // the view's up
    if (!(x > 1e-4f)) return z < 0.f ? -1 : 1;
    const float slope = z / x;
    return slope < -tanEdge ? -1 : (slope > tanEdge ? 1 : 0);
}

} // namespace

ArenaOffFrameTells arenaOffFrameTells(const ArenaView& view, const ArenaTellBody* bodies, std::size_t count) {
    ArenaOffFrameTells out;
    if (!bodies) return out;
    const float edge = std::max(0.f, clampOr0(view.halfAngle, 0.f, 180.f) - clampOr0(view.margin, 0.f, 180.f));
    const bool vertical = view.vHalfAngle > 0.f && view.pitch == view.pitch && view.eyeZ == view.eyeZ;
    const float vEdge = std::max(0.f, clampOr0(view.vHalfAngle, 0.f, 89.f) - clampOr0(view.margin, 0.f, 89.f));
    const float tanVEdge = std::tan(vEdge * (kPi / 180.f));
    count = std::min(count, kMaxArenaEnemies);
    for (std::size_t i = 0; i < count; ++i) {
        const ArenaTellBody& b = bodies[i];
        if (!b.winding) continue;
        const float bearing = arenaBearing(view.eye, view.forward, b.pos);
        bool out_ = std::fabs(bearing) > edge;
        if (!out_ && vertical && b.high > b.low) {
            // Out when its base and top are both under the frame, or both over it.
            const float dx = b.pos.x - view.eye.x, dy = b.pos.y - view.eye.y;
            const float ahead = std::sqrt(dx * dx + dy * dy);
            const int lo = verticalSide(view.pitch, bearing, ahead, b.low - view.eyeZ, tanVEdge);
            const int hi = verticalSide(view.pitch, bearing, ahead, b.high - view.eyeZ, tanVEdge);
            out_ = lo != 0 && lo == hi;
        }
        if (!out_) continue;
        ArenaOffFrameTell& t = out.items[out.count++];
        t.enemy = static_cast<std::uint8_t>(i);
        t.bearing = bearing;
        t.fill = clampOr0(b.fill, 0.f, 1.f);
    }
    return out;
}

ArenaTellBody arenaTellBody(const ArenaFight& fight, std::size_t i, Vec2 pos) {
    ArenaTellBody b;
    b.pos = pos;
    if (i >= fight.enemyCount() || i >= kMaxArenaEnemies || !fight.enemyAlive(i)) return b;
    const ArenaEnemyOut& e = fight.output().enemies[i];
    if (e.brain != ArenaBrain::Attack || e.part != ArenaPart::Startup) return b;
    b.winding = true;
    const ArenaAttack* a = enemyAttack(fight.def(), static_cast<std::uint8_t>(i), e.move);
    const float startup = a ? a->startup : 0.f;
    b.fill = startup > 0.f ? clampOr0(e.stateTime / startup, 0.f, 1.f) : 1.f;
    return b;
}

std::size_t arenaPaceText(const ArenaStats& s, std::size_t enemyCount, char* buf, std::size_t cap) {
    if (!buf || cap == 0) return 0;
    std::size_t len = 0;
    auto put = [&](const char* fmt, auto... args) {
        if (len + 1 >= cap) return;
        const int n = std::snprintf(buf + len, cap - len, fmt, args...);
        if (n > 0) len = std::min(len + static_cast<std::size_t>(n), cap - 1);
    };
    buf[0] = '\0';
    if (s.kills == 0 && s.punishWindows == 0) {
        put("%s", "pace: n/a");
        return len;
    }
    put("pace: kills %d, ttk", s.kills);
    enemyCount = std::min(enemyCount, kMaxArenaEnemies);
    for (std::size_t i = 0; i < enemyCount; ++i) {
        put(i == 0 ? " %d " : ", %d ", static_cast<int>(i));
        if (s.killCount[i] > 0) put("%.1f s", static_cast<double>(s.killSeconds[i] / static_cast<float>(s.killCount[i])));
        else put("%s", "n/a");
    }
    if (enemyCount == 0) put("%s", " n/a");
    put("; punish %d/%d windows used", s.punishUsed, s.punishWindows);
    return len;
}

} // namespace ftcore
