#include "ftcore/ArenaHitReact.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

constexpr float kZeroLength = 1.0e-4f; // m (or a weighted sum's length): treated as no direction

float lengthOf(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

// v made unit, or `fallback` (assumed unit) when v is about zero.
Vec2 unitOr(Vec2 v, Vec2 fallback) {
    const float len = lengthOf(v);
    if (len <= kZeroLength) return fallback;
    return Vec2{v.x / len, v.y / len};
}

// The push direction: attacker -> target; the attacker's facing when they share a place; with no attacker known,
// against the target's facing (a blow from the front); last {1, 0}.
Vec2 hitDir(const ArenaLeadHit& h) {
    const Vec2 last{1.f, 0.f};
    const Vec2 back = unitOr(Vec2{-h.facing.x, -h.facing.y}, last);
    if (!h.fromKnown) return back;
    const Vec2 facing = unitOr(h.fromFacing, back);
    return unitOr(Vec2{h.at.x - h.from.x, h.at.y - h.from.y}, facing);
}

int firstOf(int a, int b) { return a >= 0 ? a : b; }

// Whether the attacker stands on the target's right. Right of the target's facing f is (-f.y, f.x): facing +x, right
// is +y (the host's x forward, y right). No attacker known: the left.
bool attackerOnRight(const ArenaLeadHit& h) {
    const Vec2 f = unitOr(h.facing, Vec2{1.f, 0.f});
    const Vec2 right{-f.y, f.x};
    const Vec2 toAttacker{h.from.x - h.at.x, h.from.y - h.at.y};
    return h.fromKnown && (toAttacker.x * right.x + toAttacker.y * right.y) > 0.f;
}

// The struck part. A move's own hitPart wins: head -> chest; chest; spine -> chest; pelvis -> chest; thigh (the
// attacker's side) -> pelvis -> chest; shin (the attacker's side) -> that thigh -> pelvis -> chest. Auto: a low move
// (hitsAir false) takes the shin on the attacker's side, then that thigh, then the chest; otherwise by the move's
// tell (Overhead, Backhand, Slam: the head; Thrust: the spine; the rest: the chest). The chest falls back to the root.
int hitPart(const ArenaLeadHit& h, const ArenaHitSetup& s) {
    const int chest = firstOf(s.chest, s.root);
    const bool right = attackerOnRight(h);
    const int shin = right ? s.shinR : s.shinL;
    const int thigh = right ? s.thighR : s.thighL;
    switch (h.part) {
    case ArenaHitPart::Head: return firstOf(s.head, chest);
    case ArenaHitPart::Chest: return chest;
    case ArenaHitPart::Spine: return firstOf(s.spine, chest);
    case ArenaHitPart::Pelvis: return firstOf(s.pelvis, chest);
    case ArenaHitPart::Thigh: return firstOf(thigh, firstOf(s.pelvis, chest));
    case ArenaHitPart::Shin: return firstOf(shin, firstOf(thigh, firstOf(s.pelvis, chest)));
    case ArenaHitPart::Auto: break;
    }
    if (h.low) return firstOf(shin, firstOf(thigh, chest));
    switch (h.tell) {
    case ArenaTell::Overhead:
    case ArenaTell::Backhand:
    case ArenaTell::Slam:
        return firstOf(s.head, chest);
    case ArenaTell::Thrust:
        return firstOf(s.spine, chest);
    case ArenaTell::Strike:
    case ArenaTell::Charge:
    case ArenaTell::Slash:
    case ArenaTell::Sweep:
    case ArenaTell::Shot:
        break;
    }
    return chest;
}

float partMass(int part, const ArenaHitSetup& s) {
    if (part < 0 || static_cast<std::size_t>(part) >= s.partCount) return 0.f;
    return s.partMass[static_cast<std::size_t>(part)];
}

int tierRank(ArenaHitTier t) { return static_cast<int>(t); }

// Display only. A move's own hitStop (s), when set, wins; otherwise the tier's frames from the ragdoll profile.
int hitStopFrames(ArenaHitTier tier, float hitStop, const ArenaHitSetup& s) {
    if (tier == ArenaHitTier::None) return 0;
    if (hitStop > 0.f && s.hitStopFps > 0.f) return static_cast<int>(std::lround(hitStop * s.hitStopFps));
    return std::max(0, s.tierFrames[static_cast<std::size_t>(tier)]);
}

} // namespace

bool arenaHitSetup(const RagdollMap& map, const RagdollProfile& profile, ArenaHitSetup& out) {
    out = ArenaHitSetup{};
    if (map.parts.empty() || map.parts.size() > kMaxArenaHitParts) return false;
    out.partCount = map.parts.size();
    out.totalMass = std::max(0.f, map.totalMass);
    for (std::size_t i = 0; i < out.partCount; ++i) {
        const RagdollPartDef& p = map.parts[i];
        const float mass = std::max(0.f, p.massFraction) * out.totalMass;
        out.partMass[i] = mass;
        if (p.upper) out.upperMass += mass;
    }
    out.chest = map.find("chest");
    out.spine = map.find("spine");
    out.pelvis = map.find("pelvis");
    out.head = map.find("head");
    out.shinL = map.find("shin.L");
    out.shinR = map.find("shin.R");
    out.thighL = map.find("thigh.L");
    out.thighR = map.find("thigh.R");
    out.root = 0;
    out.maxLinearVelocity = std::max(0.f, profile.maxLinearVelocity);
    out.koMaxLocalDeltaV = std::max(0.f, profile.koMaxLocalDeltaV);
    out.tierFrames = {0, std::max(0, profile.hitStopLightFrames), std::max(0, profile.hitStopStaggerFrames),
                      std::max(0, profile.hitStopKoFrames)};
    out.hitStopFps = profile.hitStopFps > 0.f ? profile.hitStopFps : 60.f;
    return true;
}

float arenaHitCap(ArenaHitTier tier, int part, const ArenaHitSetup& s) {
    switch (tier) {
    case ArenaHitTier::None:
        return 0.f;
    case ArenaHitTier::Light:
        return partMass(part, s) * s.koMaxLocalDeltaV;
    case ArenaHitTier::Stagger:
        return s.upperMass * s.koMaxLocalDeltaV;
    case ArenaHitTier::Knockout:
        return s.totalMass * s.maxLinearVelocity;
    }
    return 0.f;
}

ArenaHitReaction arenaHitReaction(const ArenaLeadHit& h, const ArenaHitSetup& s) {
    ArenaHitReaction r;
    r.target = h.target;
    r.source = h.source;
    r.tier = h.tier;
    if (h.tier == ArenaHitTier::None) return r;
    r.dir = hitDir(h);
    r.part = hitPart(h, s);
    const float share = std::clamp(h.weight, std::clamp(s.minShare, 0.f, 1.f), 1.f);
    r.strength = arenaHitCap(h.tier, r.part, s) * share;
    r.hitStopFrames = hitStopFrames(h.tier, h.hitStop, s);
    r.blows = 1;
    return r;
}

void arenaHitReactions(const ArenaEvents& events, const ArenaHitSetup& s, ArenaHitReactions& out) {
    out.count = 0;
    bool tookOver = false;
    for (std::size_t i = 0; i < events.count && i < events.items.size(); ++i)
        if (events.items[i].type == ArenaEventType::TookOver) tookOver = true;

    const std::uint8_t bodies[2] = {kArenaPlayer, kArenaAlly};
    const std::size_t records = std::min(events.hitCount, events.hits.size());
    for (const std::uint8_t body : bodies) {
        ArenaHitTier top = ArenaHitTier::None;
        int best = -1;           // index of the strongest blow
        float weightSum = 0.f;
        Vec2 dirSum{};
        int frames = 0;
        std::uint8_t blows = 0;
        for (std::size_t k = 0; k < records; ++k) {
            const ArenaLeadHit& h = events.hits[k];
            if (h.target != body || h.tier == ArenaHitTier::None) continue;
            const ArenaHitReaction one = arenaHitReaction(h, s);
            const float w = std::max(0.f, h.weight);
            weightSum += w;
            dirSum.x += one.dir.x * std::max(w, 1.0e-3f);
            dirSum.y += one.dir.y * std::max(w, 1.0e-3f);
            frames = std::max(frames, one.hitStopFrames);
            if (blows < 0xFF) ++blows;
            if (best < 0) {
                best = static_cast<int>(k);
            } else {
                const ArenaLeadHit& b = events.hits[static_cast<std::size_t>(best)];
                if (tierRank(h.tier) > tierRank(b.tier) || (h.tier == b.tier && w > b.weight)) best = static_cast<int>(k);
            }
            if (tierRank(h.tier) > tierRank(top)) top = h.tier;
        }
        if (best < 0) continue;
        const ArenaHitReaction strongest = arenaHitReaction(events.hits[static_cast<std::size_t>(best)], s);
        ArenaHitReaction r;
        r.tier = top;
        r.target = body;
        r.source = strongest.source;
        r.part = strongest.part;
        r.dir = unitOr(dirSum, strongest.dir);
        const float share = std::clamp(weightSum, std::clamp(s.minShare, 0.f, 1.f), 1.f);
        r.strength = arenaHitCap(top, r.part, s) * share;
        r.hitStopFrames = frames;
        r.blows = blows;
        r.tookOver = body == kArenaPlayer && top == ArenaHitTier::Knockout && tookOver;
        out.items[out.count++] = r;
    }
}

ArenaHitTier arenaHitTierOf(const ArenaEvents& events, std::size_t eventIndex) {
    if (eventIndex >= events.count) return ArenaHitTier::None;
    const std::size_t records = std::min(events.hitCount, events.hits.size());
    for (std::size_t k = 0; k < records; ++k)
        if (events.hits[k].event == eventIndex) return events.hits[k].tier;
    return ArenaHitTier::None;
}

} // namespace ftcore
