// The lead hit reaction (ArenaHitReact.h) and the blow records Core stamps (ArenaEvents::hits).
#include <doctest/doctest.h>

#include "AllocCounter.h"
#include "TestAssets.h"
#include "ftcore/ArenaFight.h"
#include "ftcore/ArenaHitReact.h"
#include "ftcore/PartnerBrain.h"
#include "ftcore/Physics.h"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 60.f;

ArenaTable loadTable() {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/arena.json", text)) FAIL("Data/Battle/arena.json not found");
    ArenaTable t;
    std::string err;
    REQUIRE_MESSAGE(importArenaTable(text, t, &err), err);
    return t;
}

ArenaHitSetup loadSetup() {
    std::string mapText, profileText;
    if (!ftcore_test::readProjectData("Physics/ragdoll/humanoid_map.json", mapText))
        FAIL("Data/Physics/ragdoll/humanoid_map.json not found");
    if (!ftcore_test::readProjectData("Physics/ragdoll/default.json", profileText))
        FAIL("Data/Physics/ragdoll/default.json not found");
    RagdollMap map;
    RagdollProfile profile;
    std::string err;
    REQUIRE_MESSAGE(importRagdollMap(mapText, map, &err), err);
    REQUIRE_MESSAGE(importRagdollProfile(profileText, profile, &err), err);
    ArenaHitSetup s;
    REQUIRE(arenaHitSetup(map, profile, s));
    return s;
}

const ArenaEnemyKind& kindOf(const ArenaTable& t, const char* id) {
    for (std::size_t e = 0; e < t.enemyIds.size(); ++e)
        if (t.enemyIds[e] == id) return t.enemies[e];
    FAIL("no enemy kind " << id);
    return t.enemies[0];
}

// A fight with the shipped kinds pinned where the test puts them; the player at the origin facing +x.
struct HRig {
    ArenaDef def;
    ArenaFight fight;
    ArenaBodies bodies;
    ArenaInput in;

    HRig(const ArenaFighter& fighter, std::initializer_list<ArenaEnemyKind> kinds, std::uint8_t attackers = 1,
         std::uint32_t seed = 1) {
        def.fighter = fighter;
        std::size_t n = 0;
        for (const ArenaEnemyKind& k : kinds) def.enemies[n++] = k;
        def.enemyCount = static_cast<std::uint8_t>(n);
        def.attackers = attackers;
        fight.begin(def, ArenaRing{Vec2{0.f, 0.f}, 30.f}, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, seed);
        for (std::size_t i = 0; i < n; ++i) bodies.enemies[i] = Vec2{1.2f, 0.f};
    }
    const ArenaOutput& tick() {
        const ArenaOutput& out = fight.tick(kDt, in, bodies);
        in.light = in.heavy = in.dodge = in.jump = in.heal = in.lock = in.release = in.pair = false;
        return out;
    }
    // Ticks until a record is stamped (or `seconds` pass); returns the tick's output, or nullptr.
    const ArenaOutput* untilRecord(float seconds) {
        const int n = static_cast<int>(std::lround(seconds / kDt));
        for (int k = 0; k < n && !fight.over(); ++k) {
            const ArenaOutput& out = tick();
            if (out.events.hitCount > 0) return &out;
        }
        return nullptr;
    }
};

int indexOf(const ArenaEvents& ev, ArenaEventType type, std::uint8_t target) {
    for (std::size_t i = 0; i < ev.count; ++i)
        if (ev.items[i].type == type && ev.items[i].target == target) return static_cast<int>(i);
    return -1;
}

bool has(const ArenaEvents& ev, ArenaEventType type) { return indexOf(ev, type, kArenaPlayer) >= 0; }

ArenaLeadHit blow(ArenaHitTier tier, Vec2 from, Vec2 at, float weight, std::uint8_t target = kArenaPlayer) {
    ArenaLeadHit h;
    h.tier = tier;
    h.event = 0;
    h.source = 0;
    h.target = target;
    h.fromKnown = true;
    h.from = from;
    h.fromFacing = Vec2{1.f, 0.f};
    h.at = at;
    h.facing = Vec2{-1.f, 0.f};
    h.weight = weight;
    return h;
}

void push(ArenaEvents& ev, const ArenaLeadHit& h) { ev.hits[ev.hitCount++] = h; }

} // namespace

TEST_CASE("arena hit react: setup reads the humanoid map's masses and parts") {
    const ArenaHitSetup s = loadSetup();
    REQUIRE(s.chest >= 0);
    REQUIRE(s.head >= 0);
    REQUIRE(s.spine >= 0);
    REQUIRE(s.shinL >= 0);
    REQUIRE(s.shinR >= 0);
    CHECK(s.totalMass == doctest::Approx(70.f));
    CHECK(s.upperMass == doctest::Approx(35.f));
    CHECK(s.partMass[static_cast<std::size_t>(s.chest)] == doctest::Approx(11.2f));
}

TEST_CASE("arena hit react: the maw pup's nip is a Light on its hitPart thigh, the Light default hit-stop") {
    const ArenaTable t = loadTable();
    const ArenaHitSetup s = loadSetup();
    ArenaEnemyKind pup = kindOf(t, "maw_pup");
    for (std::size_t m = 1; m < pup.moveCount; ++m) pup.moves[m].weight = 0.f; // the nip only
    HRig r(t.fighter("default"), {pup});
    const ArenaOutput* out = r.untilRecord(6.f);
    REQUIRE(out != nullptr);
    const ArenaEvents& ev = out->events;
    REQUIRE(ev.hitCount == 1);
    const ArenaLeadHit& h = ev.hits[0];
    CHECK(h.move == 0);
    CHECK(h.tell == ArenaTell::Thrust);
    CHECK(h.part == ArenaHitPart::Thigh); // arena.json: the nip's "hitPart": "thigh"
    CHECK(h.tier == ArenaHitTier::Light);
    REQUIRE(h.event != kArenaNoEvent);
    CHECK(ev.items[h.event].type == ArenaEventType::Hit);
    CHECK(arenaHitTierOf(ev, h.event) == ArenaHitTier::Light);
    CHECK_FALSE(has(ev, ArenaEventType::Staggered));
    const ArenaHitReaction re = arenaHitReaction(h, s);
    CHECK(re.tier == ArenaHitTier::Light);
    // The pup dead ahead (neither side): the left thigh by convention, not the thrust's spine.
    CHECK(re.part == s.thighL);
    CHECK(h.hitStop == 0.f); // a shipped move sets no hitStop of its own
    CHECK(re.hitStopFrames == s.tierFrames[static_cast<std::size_t>(ArenaHitTier::Light)]);
    CHECK(re.hitStopFrames == 0); // default.json: hitStopLightFrames 0
    CHECK(re.dir.x < -0.99f); // the pup at +x pushes the player toward -x
    CHECK(re.strength > 0.f);
    CHECK(re.strength <= arenaHitCap(ArenaHitTier::Light, re.part, s) + 1e-4f);
}

TEST_CASE("arena hit react: the brute's third move (the charge) breaks poise: Stagger") {
    const ArenaTable t = loadTable();
    const ArenaHitSetup s = loadSetup();
    ArenaEnemyKind brute = kindOf(t, "brute");
    REQUIRE(brute.moveCount >= 3);
    brute.moves[0].weight = 0.f;
    brute.moves[1].weight = 0.f;
    HRig r(t.fighter("default"), {brute});
    r.bodies.enemies[0] = Vec2{2.f, 0.f};
    const ArenaOutput* out = r.untilRecord(10.f);
    REQUIRE(out != nullptr);
    const ArenaLeadHit& h = out->events.hits[0];
    CHECK(h.move == 2);
    CHECK(h.tier == ArenaHitTier::Stagger);
    CHECK(has(out->events, ArenaEventType::Staggered));
    CHECK(out->player.action == ArenaAction::Hitstun);
    const ArenaHitReaction re = arenaHitReaction(h, s);
    CHECK(re.part == s.chest); // a charge
    CHECK(h.hitStop == 0.f);
    CHECK(re.hitStopFrames == 4); // default.json: hitStopStaggerFrames 4
    CHECK(re.strength <= arenaHitCap(ArenaHitTier::Stagger, re.part, s) + 1e-4f);
    CHECK(re.strength > arenaHitCap(ArenaHitTier::Light, s.chest, s)); // heavier than any Light could be
}

TEST_CASE("arena hit react: a blow that downs the lead is a Knockout") {
    const ArenaTable t = loadTable();
    const ArenaHitSetup s = loadSetup();
    ArenaEnemyKind brute = kindOf(t, "brute");
    brute.moves[0].weight = 0.f;
    brute.moves[1].weight = 0.f;
    ArenaFighter low = t.fighter("default");
    low.hp = 10;
    HRig r(low, {brute});
    r.bodies.enemies[0] = Vec2{2.f, 0.f};
    const ArenaOutput* out = r.untilRecord(10.f);
    REQUIRE(out != nullptr);
    const ArenaLeadHit& h = out->events.hits[0];
    CHECK(h.tier == ArenaHitTier::Knockout);
    CHECK(h.damage > 10); // before the hp clamp
    CHECK(has(out->events, ArenaEventType::Downed));
    const ArenaHitReaction re = arenaHitReaction(h, s);
    CHECK(re.strength <= s.totalMass * s.maxLinearVelocity + 1e-3f);
    CHECK(re.strength >= s.totalMass * s.maxLinearVelocity * s.minShare - 1e-3f);
    CHECK(re.hitStopFrames == 8); // default.json: hitStopKoFrames 8
}

TEST_CASE("arena hit react: a guard break is a Stagger; the block and its chip are None") {
    const ArenaTable t = loadTable();
    ArenaEnemyKind brute = kindOf(t, "brute");
    brute.moves[0].weight = 0.f;
    brute.moves[1].weight = 0.f;
    REQUIRE_FALSE(brute.moves[2].unblockable);
    ArenaFighter weak = t.fighter("default");
    weak.stamina = 5.f;
    REQUIRE(weak.guardAbsorb == 1.f); // the shipped guard takes the whole blow: no chip, so no Hit at all
    HRig r(weak, {brute});
    r.bodies.enemies[0] = Vec2{2.f, 0.f};
    r.in.guard = true;
    const ArenaOutput* out = r.untilRecord(10.f);
    REQUIRE(out != nullptr);
    const ArenaEvents& ev = out->events;
    const int blocked = indexOf(ev, ArenaEventType::Blocked, kArenaPlayer);
    const int broken = indexOf(ev, ArenaEventType::GuardBroken, kArenaPlayer);
    REQUIRE(blocked >= 0);
    REQUIRE(broken >= 0);
    CHECK(arenaHitTierOf(ev, static_cast<std::size_t>(blocked)) == ArenaHitTier::None);
    CHECK(arenaHitTierOf(ev, static_cast<std::size_t>(broken)) == ArenaHitTier::Stagger);
    CHECK(indexOf(ev, ArenaEventType::Hit, kArenaPlayer) < 0);
    REQUIRE(ev.hitCount == 1);
    const ArenaLeadHit& h = ev.hits[0];
    CHECK(h.event == broken);
    CHECK(h.damage == 0);
    CHECK(h.poise == doctest::Approx(brute.moves[2].attack.poiseDamage));
    CHECK(h.weight == doctest::Approx(brute.moves[2].attack.poiseDamage / weak.poise)); // no damage: poise alone
}

TEST_CASE("arena hit react: a chip that downs the guarding lead is a Knockout on its Hit; a chip alone has none") {
    const ArenaTable t = loadTable();
    ArenaEnemyKind brute = kindOf(t, "brute");
    brute.moves[0].weight = 0.f;
    brute.moves[1].weight = 0.f;
    for (const int hp : {1, 140}) {
        CAPTURE(hp);
        ArenaFighter f = t.fighter("default");
        f.guardAbsorb = 0.5f; // a guard that lets half through
        f.stamina = 1000.f;   // never breaks
        f.hp = hp;
        HRig r(f, {brute});
        r.bodies.enemies[0] = Vec2{2.f, 0.f};
        r.in.guard = true;
        bool blocked = false;
        for (int k = 0; k < 60 * 10 && !blocked; ++k) {
            const ArenaOutput& out = r.tick();
            const ArenaEvents& ev = out.events;
            if (indexOf(ev, ArenaEventType::Blocked, kArenaPlayer) < 0) continue;
            blocked = true;
            const int hit = indexOf(ev, ArenaEventType::Hit, kArenaPlayer);
            REQUIRE(hit >= 0); // the chip
            CHECK(ev.items[static_cast<std::size_t>(hit)].amount > 0);
            CHECK(indexOf(ev, ArenaEventType::GuardBroken, kArenaPlayer) < 0);
            if (hp == 1) {
                REQUIRE(ev.hitCount == 1);
                CHECK(ev.hits[0].tier == ArenaHitTier::Knockout);
                CHECK(ev.hits[0].event == hit);
                CHECK(ev.hits[0].damage == static_cast<int>(std::lround(brute.moves[2].attack.damage * 0.5f)));
                CHECK(has(ev, ArenaEventType::Downed));
            } else {
                CHECK(ev.hitCount == 0);
                CHECK(arenaHitTierOf(ev, static_cast<std::size_t>(hit)) == ArenaHitTier::None);
            }
        }
        REQUIRE(blocked);
    }
}

TEST_CASE("arena hit react: a blow that lands in the air is a Stagger whatever the poise") {
    const ArenaTable t = loadTable();
    ArenaEnemyKind brute = kindOf(t, "brute");
    brute.moves[1].weight = 0.f;
    REQUIRE(brute.moves[0].hitsAir);
    REQUIRE(brute.moves[2].hitsAir);
    ArenaFighter f = t.fighter("default");
    f.poise = 1.0e6f; // on the ground nothing breaks it: a Stagger can only be the air's
    HRig r(f, {brute});
    r.fight.setNoDefeatForTest(true);
    r.bodies.enemies[0] = Vec2{2.f, 0.f};
    int air = 0;
    for (int k = 0; k < 60 * 40 && air == 0; ++k) {
        const ArenaPlayerOut before = r.fight.output().player;
        const bool up = before.jumpTime >= 0.f;
        if (!up && before.action != ArenaAction::Hitstun) r.in.jump = true;
        const ArenaOutput& out = r.tick();
        CHECK_FALSE(has(out.events, ArenaEventType::Staggered));
        for (std::size_t j = 0; j < out.events.hitCount; ++j) {
            const ArenaLeadHit& h = out.events.hits[j];
            if (h.tier == ArenaHitTier::Stagger) {
                CHECK(up);                        // in the air when it landed
                CHECK(out.player.jumpTime < 0.f); // knocked out of the jump
                CHECK(out.player.action == ArenaAction::Hitstun);
                ++air;
            } else {
                CHECK(h.tier == ArenaHitTier::Light);
            }
        }
    }
    REQUIRE(air > 0);
}

TEST_CASE("arena hit react: a blow during hitstun is Light, never a second Stagger") {
    const ArenaTable t = loadTable();
    ArenaEnemyKind brute = kindOf(t, "brute");
    brute.moves[0].weight = 0.f;
    brute.moves[1].weight = 0.f;
    const ArenaEnemyKind pup = kindOf(t, "maw_pup");
    ArenaFighter f = t.fighter("default");
    f.hitstun = 2.f; // a long stun so the pups' nips land inside it
    HRig r(f, {brute, pup, pup}, 3);
    r.fight.setNoDefeatForTest(true);
    r.bodies.enemies[0] = Vec2{2.f, 0.f};
    r.bodies.enemies[1] = Vec2{0.f, 1.2f};
    r.bodies.enemies[2] = Vec2{0.f, -1.2f};
    int duringStun = 0;
    for (int k = 0; k < 60 * 30; ++k) {
        const bool stunned = r.fight.output().player.action == ArenaAction::Hitstun;
        const ArenaOutput& out = r.tick();
        const bool still = out.player.action == ArenaAction::Hitstun;
        if (!stunned || !still || has(out.events, ArenaEventType::Staggered)) continue;
        for (std::size_t j = 0; j < out.events.hitCount; ++j) {
            const ArenaLeadHit& h = out.events.hits[j];
            if (h.target != kArenaPlayer || h.event == kArenaNoEvent) continue;
            if (out.events.items[h.event].type != ArenaEventType::Hit) continue;
            CHECK(h.tier == ArenaHitTier::Light);
            ++duringStun;
        }
    }
    REQUIRE(duringStun > 0);
}

TEST_CASE("arena hit react: evades, parries, blocks and staggers carry no record; records name only blow events") {
    const ArenaTable t = loadTable();
    int evaded = 0, parried = 0, records = 0, downs = 0;
    for (std::uint32_t seed = 1; seed <= 12; ++seed) {
        ArenaFighter f = t.fighter("default");
        f.hp = 60; // some seeds end in a fall: the Knockout invariant below sees them
        HRig r(f, {kindOf(t, "brute"), kindOf(t, "maw_pup")}, 2, seed);
        r.bodies.enemies[1] = Vec2{0.f, 1.4f};
        for (int k = 0; k < 60 * 20 && !r.fight.over(); ++k) {
            r.in = r.fight.autopilot();
            const ArenaOutput& out = r.tick();
            const ArenaEvents& ev = out.events;
            REQUIRE(ev.hitCount <= kMaxArenaLeadHits);
            for (std::size_t i = 0; i < ev.count; ++i) {
                const ArenaEventType type = ev.items[i].type;
                if (type == ArenaEventType::Evaded) ++evaded;
                if (type == ArenaEventType::Parried) ++parried;
                if (type == ArenaEventType::Evaded || type == ArenaEventType::Parried ||
                    type == ArenaEventType::Blocked || type == ArenaEventType::Staggered ||
                    type == ArenaEventType::HostHit)
                    CHECK(arenaHitTierOf(ev, i) == ArenaHitTier::None);
            }
            // Every fall has its Knockout record (the takeover's AllyDowned, source kArenaNobody, is the same fall).
            for (std::size_t i = 0; i < ev.count; ++i) {
                const ArenaEvent& e = ev.items[i];
                const bool fall = (e.type == ArenaEventType::Downed && e.target == kArenaPlayer) ||
                                  (e.type == ArenaEventType::AllyDowned && e.source != kArenaNobody);
                if (!fall) continue;
                ++downs;
                // The record of this fall: its body, its attacker, and stamped on the fall itself (a table share) or
                // on that attacker's blow earlier in the tick.
                bool ko = false;
                for (std::size_t j = 0; j < ev.hitCount; ++j) {
                    const ArenaLeadHit& h = ev.hits[j];
                    if (h.target != e.target || h.tier != ArenaHitTier::Knockout || h.source != e.source) continue;
                    const std::size_t at = h.event;
                    if (at == i || (at < i && ev.items[at].source == e.source &&
                                    ev.items[at].target == e.target))
                        ko = true;
                }
                CHECK(ko);
            }
            for (std::size_t j = 0; j < ev.hitCount; ++j) {
                const ArenaLeadHit& h = ev.hits[j];
                ++records;
                CHECK(h.tier != ArenaHitTier::None);
                CHECK((h.target == kArenaPlayer || h.target == kArenaAlly));
                if (h.event == kArenaNoEvent) continue;
                const ArenaEventType type = ev.items[h.event].type;
                CHECK((type == ArenaEventType::Hit || type == ArenaEventType::GuardBroken ||
                       type == ArenaEventType::TookHalf || type == ArenaEventType::AllyHit ||
                       type == ArenaEventType::Downed || type == ArenaEventType::AllyDowned));
            }
        }
    }
    REQUIRE(evaded > 0);
    REQUIRE(parried > 0);
    REQUIRE(records > 0);
    REQUIRE(downs > 0);
}

TEST_CASE("arena hit react: direction is attacker -> target; a shared place falls back to facings, always unit") {
    const ArenaHitSetup s = loadSetup();
    const ArenaHitReaction fromRight = arenaHitReaction(blow(ArenaHitTier::Light, Vec2{3.f, 0.f}, Vec2{1.f, 0.f}, 0.5f), s);
    CHECK(fromRight.dir.x == doctest::Approx(-1.f));
    CHECK(fromRight.dir.y == doctest::Approx(0.f));
    const ArenaHitReaction fromBelow = arenaHitReaction(blow(ArenaHitTier::Light, Vec2{0.f, -2.f}, Vec2{0.f, 0.f}, 0.5f), s);
    CHECK(fromBelow.dir.y == doctest::Approx(1.f));

    ArenaLeadHit same = blow(ArenaHitTier::Stagger, Vec2{1.f, 1.f}, Vec2{1.f, 1.f}, 0.5f);
    same.fromFacing = Vec2{0.f, 2.f}; // not unit on purpose
    const ArenaHitReaction a = arenaHitReaction(same, s);
    CHECK(a.dir.x == doctest::Approx(0.f));
    CHECK(a.dir.y == doctest::Approx(1.f)); // the attacker's facing

    same.fromKnown = false;
    same.facing = Vec2{1.f, 0.f};
    const ArenaHitReaction b = arenaHitReaction(same, s);
    CHECK(b.dir.x == doctest::Approx(-1.f)); // against the target's facing

    same.facing = Vec2{0.f, 0.f};
    const ArenaHitReaction c = arenaHitReaction(same, s);
    CHECK(c.dir.x == doctest::Approx(1.f)); // the last resort
    CHECK(std::sqrt(c.dir.x * c.dir.x + c.dir.y * c.dir.y) == doctest::Approx(1.f));
}

TEST_CASE("arena hit react: each tier's cap, the least share, and None gives nothing") {
    const ArenaHitSetup s = loadSetup();
    const float chest = s.partMass[static_cast<std::size_t>(s.chest)];
    CHECK(arenaHitCap(ArenaHitTier::Light, s.chest, s) == doctest::Approx(chest * s.koMaxLocalDeltaV));
    CHECK(arenaHitCap(ArenaHitTier::Stagger, s.chest, s) == doctest::Approx(s.upperMass * s.koMaxLocalDeltaV));
    CHECK(arenaHitCap(ArenaHitTier::Knockout, s.chest, s) == doctest::Approx(s.totalMass * s.maxLinearVelocity));
    CHECK(arenaHitCap(ArenaHitTier::Light, s.chest, s) < arenaHitCap(ArenaHitTier::Stagger, s.chest, s));
    CHECK(arenaHitCap(ArenaHitTier::Stagger, s.chest, s) < arenaHitCap(ArenaHitTier::Knockout, s.chest, s));
    for (const ArenaHitTier tier : {ArenaHitTier::Light, ArenaHitTier::Stagger, ArenaHitTier::Knockout}) {
        const ArenaHitReaction huge = arenaHitReaction(blow(tier, Vec2{2.f, 0.f}, Vec2{}, 9.f), s);
        CHECK(huge.strength == doctest::Approx(arenaHitCap(tier, huge.part, s)));
        const ArenaHitReaction tiny = arenaHitReaction(blow(tier, Vec2{2.f, 0.f}, Vec2{}, 0.f), s);
        CHECK(tiny.strength == doctest::Approx(arenaHitCap(tier, tiny.part, s) * s.minShare));
        const ArenaHitReaction mid = arenaHitReaction(blow(tier, Vec2{2.f, 0.f}, Vec2{}, 0.5f), s);
        CHECK(mid.strength == doctest::Approx(arenaHitCap(tier, mid.part, s) * 0.5f));
    }
    const ArenaHitReaction none = arenaHitReaction(blow(ArenaHitTier::None, Vec2{2.f, 0.f}, Vec2{}, 1.f), s);
    CHECK(none.strength == 0.f);
    CHECK(none.part == -1);
}

TEST_CASE("arena hit react: parts by height and tell; a low blow takes the leg on the attacker's side") {
    const ArenaHitSetup s = loadSetup();
    ArenaLeadHit h = blow(ArenaHitTier::Light, Vec2{2.f, 0.f}, Vec2{}, 0.5f);
    h.facing = Vec2{1.f, 0.f}; // right is +y
    for (const ArenaTell tell : {ArenaTell::Overhead, ArenaTell::Backhand, ArenaTell::Slam}) {
        h.tell = tell;
        CHECK(arenaHitReaction(h, s).part == s.head);
    }
    h.tell = ArenaTell::Thrust;
    CHECK(arenaHitReaction(h, s).part == s.spine);
    for (const ArenaTell tell : {ArenaTell::Strike, ArenaTell::Slash, ArenaTell::Shot, ArenaTell::Charge, ArenaTell::Sweep}) {
        h.tell = tell;
        CHECK(arenaHitReaction(h, s).part == s.chest);
    }
    h.low = true; // the height wins over the tell
    h.tell = ArenaTell::Overhead;
    h.from = Vec2{0.f, 2.f};
    CHECK(arenaHitReaction(h, s).part == s.shinR);
    h.from = Vec2{0.f, -2.f};
    CHECK(arenaHitReaction(h, s).part == s.shinL);
    h.fromKnown = false; // no attacker: the left by convention
    CHECK(arenaHitReaction(h, s).part == s.shinL);
    h.fromKnown = true;
    ArenaHitSetup noShins = s;
    noShins.shinL = noShins.shinR = -1;
    CHECK(arenaHitReaction(h, noShins).part == s.thighL);
    ArenaHitSetup noLegs = noShins;
    noLegs.thighL = noLegs.thighR = -1;
    CHECK(arenaHitReaction(h, noLegs).part == s.chest);
    noLegs.chest = -1;
    CHECK(arenaHitReaction(h, noLegs).part == noLegs.root);
}

TEST_CASE("arena hit react: the nip's thigh is on the side the pup stands") {
    const ArenaTable t = loadTable();
    const ArenaHitSetup s = loadSetup();
    ArenaEnemyKind pup = kindOf(t, "maw_pup");
    for (std::size_t m = 1; m < pup.moveCount; ++m) pup.moves[m].weight = 0.f; // the nip only
    // The player faces +x, so +y is its right (the host's y right).
    for (const auto& [side, thigh] : {std::pair{0.6f, s.thighR}, std::pair{-0.6f, s.thighL}}) {
        CAPTURE(side);
        HRig r(t.fighter("default"), {pup});
        r.bodies.enemies[0] = Vec2{1.f, side};
        const ArenaOutput* out = r.untilRecord(6.f);
        REQUIRE(out != nullptr);
        const ArenaLeadHit& h = out->events.hits[0];
        CHECK(h.part == ArenaHitPart::Thigh);
        CHECK(arenaHitReaction(h, s).part == thigh);
    }
}

TEST_CASE("arena hit react: hitPart parses by name; absent is auto; an unknown name is an error") {
    const ArenaTable t = loadTable();
    const ArenaEnemyKind& pup = kindOf(t, "maw_pup");
    REQUIRE(pup.moveCount >= 2);
    CHECK(pup.moves[0].hitPart == ArenaHitPart::Thigh); // set in arena.json
    CHECK(pup.moves[1].hitPart == ArenaHitPart::Auto);  // the pounce sets none
    for (std::size_t p = 0; p < kArenaHitPartCount; ++p) {
        const ArenaHitPart part = static_cast<ArenaHitPart>(p);
        CAPTURE(p);
        CHECK(std::string(arenaHitPartName(part)) != "");
        std::string text;
        REQUIRE(ftcore_test::readProjectData("Battle/arena.json", text));
        const std::string from = "\"hitPart\": \"thigh\"";
        const std::size_t at = text.find(from);
        REQUIRE(at != std::string::npos);
        text.replace(at, from.size(), std::string("\"hitPart\": \"") + arenaHitPartName(part) + "\"");
        ArenaTable u;
        std::string err;
        REQUIRE_MESSAGE(importArenaTable(text, u, &err), err);
        CHECK(kindOf(u, "maw_pup").moves[0].hitPart == part);
    }
    for (const char* bad : {"\"hitPart\": \"knee\"", "\"hitPart\": 3", "\"hitPart\": \"Thigh\""}) {
        CAPTURE(bad);
        std::string text;
        REQUIRE(ftcore_test::readProjectData("Battle/arena.json", text));
        const std::string from = "\"hitPart\": \"thigh\"";
        const std::size_t at = text.find(from);
        REQUIRE(at != std::string::npos);
        text.replace(at, from.size(), bad);
        ArenaTable u;
        std::string err;
        CHECK_FALSE(importArenaTable(text, u, &err));
        CHECK(err.find(".hitPart: expected one of auto, head, chest, spine, pelvis, thigh, shin") != std::string::npos);
    }
}

TEST_CASE("arena hit react: a move's hitPart wins over height and tell; missing parts fall back in order") {
    const ArenaHitSetup s = loadSetup();
    REQUIRE(s.pelvis >= 0);
    REQUIRE(s.head >= 0);
    ArenaLeadHit h = blow(ArenaHitTier::Light, Vec2{0.f, 2.f}, Vec2{}, 0.5f);
    h.facing = Vec2{1.f, 0.f}; // right is +y: the attacker stands on the right
    h.tell = ArenaTell::Overhead;
    h.low = true; // an override wins over the height too
    h.part = ArenaHitPart::Head;
    CHECK(arenaHitReaction(h, s).part == s.head);
    h.part = ArenaHitPart::Chest;
    CHECK(arenaHitReaction(h, s).part == s.chest);
    h.part = ArenaHitPart::Spine;
    CHECK(arenaHitReaction(h, s).part == s.spine);
    h.part = ArenaHitPart::Pelvis;
    CHECK(arenaHitReaction(h, s).part == s.pelvis);
    h.part = ArenaHitPart::Thigh;
    CHECK(arenaHitReaction(h, s).part == s.thighR);
    h.part = ArenaHitPart::Shin;
    CHECK(arenaHitReaction(h, s).part == s.shinR);
    h.from = Vec2{0.f, -2.f}; // now on the left
    CHECK(arenaHitReaction(h, s).part == s.shinL);
    h.part = ArenaHitPart::Thigh;
    CHECK(arenaHitReaction(h, s).part == s.thighL);
    h.part = ArenaHitPart::Auto; // the low rule again
    CHECK(arenaHitReaction(h, s).part == s.shinL);

    // Fallbacks: shin -> that thigh -> pelvis -> chest -> root; thigh -> pelvis -> chest; head/spine/pelvis -> chest.
    ArenaHitSetup m = s;
    m.shinL = m.shinR = -1;
    h.part = ArenaHitPart::Shin;
    CHECK(arenaHitReaction(h, m).part == s.thighL);
    m.thighL = m.thighR = -1;
    CHECK(arenaHitReaction(h, m).part == s.pelvis);
    h.part = ArenaHitPart::Thigh;
    CHECK(arenaHitReaction(h, m).part == s.pelvis);
    m.pelvis = -1;
    CHECK(arenaHitReaction(h, m).part == s.chest);
    h.part = ArenaHitPart::Shin;
    CHECK(arenaHitReaction(h, m).part == s.chest);
    h.part = ArenaHitPart::Pelvis;
    CHECK(arenaHitReaction(h, m).part == s.chest);
    m.head = m.spine = -1;
    h.part = ArenaHitPart::Head;
    CHECK(arenaHitReaction(h, m).part == s.chest);
    h.part = ArenaHitPart::Spine;
    CHECK(arenaHitReaction(h, m).part == s.chest);
    m.chest = -1;
    for (const ArenaHitPart p : {ArenaHitPart::Head, ArenaHitPart::Chest, ArenaHitPart::Spine, ArenaHitPart::Pelvis,
                                 ArenaHitPart::Thigh, ArenaHitPart::Shin}) {
        h.part = p;
        CHECK(arenaHitReaction(h, m).part == m.root);
    }
}

TEST_CASE("arena hit react: a full record list loses its lowest tier, on any body; a Knockout only to Knockouts") {
    ArenaEvents ev;
    for (std::size_t k = 0; k < kMaxArenaLeadHits; ++k) {
        ArenaLeadHit h = blow(k == 3 ? ArenaHitTier::Light : ArenaHitTier::Stagger, Vec2{2.f, 0.f}, Vec2{}, 0.1f);
        h.move = static_cast<std::uint8_t>(k);
        arenaPushLeadHit(ev, h);
    }
    REQUIRE(ev.hitCount == kMaxArenaLeadHits);
    CHECK(ev.hitsDropped == 0);
    ArenaLeadHit ko = blow(ArenaHitTier::Knockout, Vec2{2.f, 0.f}, Vec2{}, 0.9f);
    ko.move = 99;
    arenaPushLeadHit(ev, ko);
    CHECK(ev.hitCount == kMaxArenaLeadHits);
    CHECK(ev.hitsDropped == 1);
    CHECK(ev.hits[3].move == 99); // the one Light gave way
    CHECK(ev.hits[3].tier == ArenaHitTier::Knockout);
    // A Light now finds nothing lower: it is the one lost.
    arenaPushLeadHit(ev, blow(ArenaHitTier::Light, Vec2{2.f, 0.f}, Vec2{}, 0.1f));
    CHECK(ev.hitsDropped == 2);
    for (std::size_t k = 0; k < kMaxArenaLeadHits; ++k) CHECK(ev.hits[k].tier != ArenaHitTier::Light);
    // Another body's Knockout takes the earliest of the lowest tier (the player's Stagger in slot 0).
    ArenaLeadHit allyKo = blow(ArenaHitTier::Knockout, Vec2{2.f, 0.f}, Vec2{}, 0.9f, kArenaAlly);
    allyKo.move = 77;
    arenaPushLeadHit(ev, allyKo);
    CHECK(ev.hitsDropped == 3);
    CHECK(ev.hits[0].target == kArenaAlly);
    CHECK(ev.hits[0].move == 77);
    // Among equal lowest tiers the same body's goes first: an ally Stagger, then the ally's Knockout takes its place.
    ArenaLeadHit allySt = blow(ArenaHitTier::Stagger, Vec2{2.f, 0.f}, Vec2{}, 0.1f, kArenaAlly);
    allySt.move = 55;
    ev.hits[5] = allySt;
    ArenaLeadHit allyKo2 = blow(ArenaHitTier::Knockout, Vec2{2.f, 0.f}, Vec2{}, 0.9f, kArenaAlly);
    allyKo2.move = 78;
    arenaPushLeadHit(ev, allyKo2);
    CHECK(ev.hits[5].move == 78);
    CHECK(ev.hits[1].tier == ArenaHitTier::Stagger); // the player's earlier Stagger stands
    // Every record a Knockout: the new Knockout is the one lost.
    for (std::size_t k = 0; k < kMaxArenaLeadHits; ++k) ev.hits[k].tier = ArenaHitTier::Knockout;
    ArenaLeadHit last = blow(ArenaHitTier::Knockout, Vec2{2.f, 0.f}, Vec2{}, 0.9f);
    last.move = 200;
    const std::uint32_t before = ev.hitsDropped;
    arenaPushLeadHit(ev, last);
    CHECK(ev.hitsDropped == before + 1);
    for (std::size_t k = 0; k < kMaxArenaLeadHits; ++k) CHECK(ev.hits[k].move != 200);
}

TEST_CASE("arena hit react: hit-stop is the tier default from the profile; a move's own hitStop wins") {
    const ArenaHitSetup s = loadSetup();
    CHECK(s.hitStopFps == doctest::Approx(60.f));
    CHECK(s.tierFrames[1] == 0);
    CHECK(s.tierFrames[2] == 4);
    CHECK(s.tierFrames[3] == 8);
    ArenaLeadHit h = blow(ArenaHitTier::Light, Vec2{2.f, 0.f}, Vec2{}, 0.5f);
    CHECK(arenaHitReaction(h, s).hitStopFrames == 0);
    h.tier = ArenaHitTier::Stagger;
    CHECK(arenaHitReaction(h, s).hitStopFrames == 4);
    h.tier = ArenaHitTier::Knockout;
    CHECK(arenaHitReaction(h, s).hitStopFrames == 8);
    h.tier = ArenaHitTier::None;
    CHECK(arenaHitReaction(h, s).hitStopFrames == 0);
    // A move with hitStop set overrides the tier, up or down: 0.1 s at 60 fps is 6 frames, 0.05 s is 3.
    h.tier = ArenaHitTier::Light;
    h.hitStop = 0.1f;
    CHECK(arenaHitReaction(h, s).hitStopFrames == 6);
    h.tier = ArenaHitTier::Knockout;
    h.hitStop = 0.05f;
    CHECK(arenaHitReaction(h, s).hitStopFrames == 3);
    // A set hitStop under half a frame still shows one: never rounded to nothing (nor back to the tier's 8).
    h.hitStop = 0.004f;
    CHECK(arenaHitReaction(h, s).hitStopFrames == 1);
    h.hitStop = 0.05f;
    // The defaults come from the data: another profile's frames and rate are used as given.
    ArenaHitSetup other = s;
    other.tierFrames = {0, 1, 5, 9};
    other.hitStopFps = 30.f;
    CHECK(arenaHitReaction(h, other).hitStopFrames == 2); // round(0.05 * 30)
    h.hitStop = 0.f;
    CHECK(arenaHitReaction(h, other).hitStopFrames == 9);
}

TEST_CASE("arena hit react: a move with hitStop set gives its own frames in a scripted fight") {
    const ArenaTable t = loadTable();
    const ArenaHitSetup s = loadSetup();
    ArenaEnemyKind brute = kindOf(t, "brute");
    brute.moves[0].weight = 0.f;
    brute.moves[1].weight = 0.f;
    brute.moves[2].attack.hitStop = 0.2f; // 12 frames at 60 fps, over the Stagger default of 4
    HRig r(t.fighter("default"), {brute});
    r.bodies.enemies[0] = Vec2{2.f, 0.f};
    const ArenaOutput* out = r.untilRecord(10.f);
    REQUIRE(out != nullptr);
    const ArenaLeadHit& h = out->events.hits[0];
    CHECK(h.tier == ArenaHitTier::Stagger);
    CHECK(h.hitStop == doctest::Approx(0.2f));
    CHECK(arenaHitReaction(h, s).hitStopFrames == 12);
}

TEST_CASE("arena hit react: building reactions feeds nothing back; the fight runs the same with any frames") {
    // Three identical fights in lockstep: one whose host never builds reactions, one with the shipped frames, one with
    // very long frames at another rate. The reaction API takes the events by const and the setup is the host's, so
    // this pins that nothing it builds reaches the tick. That the stamp itself changes no outcome is pinned by every
    // older arena test, unchanged and green with the records written.
    const ArenaTable t = loadTable();
    const ArenaHitSetup shipped = loadSetup();
    ArenaHitSetup longer = shipped;
    longer.tierFrames = {0, 30, 60, 90};
    longer.hitStopFps = 30.f;
    const std::initializer_list<ArenaEnemyKind> kinds{kindOf(t, "brute"), kindOf(t, "maw_pup")};
    HRig plain(t.fighter("default"), kinds, 2);
    HRig a(t.fighter("default"), kinds, 2);
    HRig b(t.fighter("default"), kinds, 2);
    for (HRig* r : {&plain, &a, &b}) {
        r->fight.setNoDefeatForTest(true);
        r->bodies.enemies[1] = Vec2{0.f, 1.2f};
    }
    ArenaHitReactions ra, rb;
    int shown = 0;
    for (int k = 0; k < 60 * 15; ++k) {
        const ArenaOutput& o0 = plain.tick();
        const ArenaOutput& o1 = a.tick();
        const ArenaOutput& o2 = b.tick();
        arenaHitReactions(o1.events, shipped, ra);
        arenaHitReactions(o2.events, longer, rb);
        for (const ArenaHitReaction& e : rb) shown += e.hitStopFrames;
        for (const ArenaOutput* o : {&o1, &o2}) {
            REQUIRE(o->events.count == o0.events.count);
            CHECK(std::memcmp(o->events.items.data(), o0.events.items.data(),
                              o0.events.count * sizeof(ArenaEvent)) == 0);
            CHECK(o->events.hitCount == o0.events.hitCount);
            CHECK(o->player.stopLeft == o0.player.stopLeft);
            CHECK(o->player.action == o0.player.action);
            CHECK(o->player.velocity.x == o0.player.velocity.x);
            CHECK(o->player.velocity.y == o0.player.velocity.y);
        }
        CHECK(a.fight.hp() == plain.fight.hp());
        CHECK(b.fight.hp() == plain.fight.hp());
        CHECK(a.fight.hitStopLeft() == plain.fight.hitStopLeft());
        CHECK(b.fight.hitStopLeft() == plain.fight.hitStopLeft());
    }
    REQUIRE(shown > 0); // the long frames were shown, and changed nothing
}

TEST_CASE("arena hit react: two blows on one body in a tick merge into one reaction") {
    const ArenaHitSetup s = loadSetup();
    ArenaEvents ev;
    ArenaLeadHit a = blow(ArenaHitTier::Light, Vec2{2.f, 0.f}, Vec2{}, 0.3f); // pushes -x
    a.hitStop = 0.05f;
    ArenaLeadHit b = blow(ArenaHitTier::Stagger, Vec2{0.f, 2.f}, Vec2{}, 0.4f); // pushes -y
    b.source = 1;
    b.tell = ArenaTell::Overhead;
    b.hitStop = 0.1f;
    push(ev, a);
    push(ev, b);
    ArenaHitReactions r;
    arenaHitReactions(ev, s, r);
    REQUIRE(r.size() == 1);
    const ArenaHitReaction& m = r[0];
    CHECK(m.target == kArenaPlayer);
    CHECK(m.blows == 2);
    CHECK(m.tier == ArenaHitTier::Stagger);
    CHECK(m.source == 1);
    CHECK(m.part == s.head); // the strongest blow's
    CHECK(m.hitStopFrames == 6);
    CHECK(m.dir.x < 0.f);
    CHECK(m.dir.y < 0.f);
    CHECK(std::sqrt(m.dir.x * m.dir.x + m.dir.y * m.dir.y) == doctest::Approx(1.f));
    CHECK(m.strength == doctest::Approx(arenaHitCap(ArenaHitTier::Stagger, m.part, s) * 0.7f));

    // Opposed equal blows cancel: the strongest (the earliest of equals) gives the direction; the sum is capped.
    ArenaEvents opposed;
    push(opposed, blow(ArenaHitTier::Light, Vec2{2.f, 0.f}, Vec2{}, 0.8f));
    push(opposed, blow(ArenaHitTier::Light, Vec2{-2.f, 0.f}, Vec2{}, 0.8f));
    arenaHitReactions(opposed, s, r);
    REQUIRE(r.size() == 1);
    CHECK(r[0].dir.x == doctest::Approx(-1.f));
    CHECK(r[0].strength == doctest::Approx(arenaHitCap(ArenaHitTier::Light, r[0].part, s)));
}

TEST_CASE("arena hit react: an ally's blow takes the same path; player first, ally second") {
    const ArenaHitSetup s = loadSetup();
    ArenaEvents ev;
    push(ev, blow(ArenaHitTier::Light, Vec2{0.f, 3.f}, Vec2{0.f, 1.f}, 0.2f, kArenaAlly));
    push(ev, blow(ArenaHitTier::Knockout, Vec2{2.f, 0.f}, Vec2{}, 0.6f));
    ArenaHitReactions r;
    arenaHitReactions(ev, s, r);
    REQUIRE(r.size() == 2);
    CHECK(r[0].target == kArenaPlayer);
    CHECK(r[0].tier == ArenaHitTier::Knockout);
    CHECK_FALSE(r[0].tookOver); // no TookOver this tick
    CHECK(r[1].target == kArenaAlly);
    CHECK(r[1].tier == ArenaHitTier::Light);
    CHECK(r[1].dir.y == doctest::Approx(-1.f));
    CHECK(r[1].strength == doctest::Approx(arenaHitCap(ArenaHitTier::Light, r[1].part, s) * s.minShare));
}

TEST_CASE("arena hit react: a TookOver tick flags every reaction; targets name the bodies before the swap") {
    const ArenaHitSetup s = loadSetup();
    ArenaEvents ev;
    ev.items[0].type = ArenaEventType::Hit;
    ev.items[0].target = kArenaPlayer;
    ev.items[1].type = ArenaEventType::Downed;
    ev.items[1].target = kArenaPlayer;
    ev.items[2].type = ArenaEventType::TookOver;
    ev.items[3].type = ArenaEventType::AllyDowned;
    ev.items[3].target = kArenaAlly;
    ev.count = 4;
    push(ev, blow(ArenaHitTier::Knockout, Vec2{2.f, 0.f}, Vec2{}, 0.6f));
    ArenaHitReactions r;
    arenaHitReactions(ev, s, r);
    REQUIRE(r.size() == 1); // the takeover's AllyDowned is the same fall: no ally reaction
    CHECK(r[0].tookOver);
    CHECK(arenaHitTierOf(ev, 3) == ArenaHitTier::None);
    CHECK(arenaHitTierOf(ev, 9) == ArenaHitTier::None); // past count
    // A blow on the ally body earlier in the same tick: its reaction is flagged too (the host swaps it with the
    // player's).
    ArenaLeadHit allyBlow = blow(ArenaHitTier::Light, Vec2{2.f, 0.f}, Vec2{}, 0.2f, kArenaAlly);
    allyBlow.event = 0;
    push(ev, allyBlow);
    arenaHitReactions(ev, s, r);
    REQUIRE(r.size() == 2);
    CHECK(r[0].target == kArenaPlayer);
    CHECK(r[0].tookOver);
    CHECK(r[1].target == kArenaAlly);
    CHECK(r[1].tier == ArenaHitTier::Light);
    CHECK(r[1].tookOver);
    // No TookOver in the tick: nothing is flagged.
    ev.items[2].type = ArenaEventType::Hit;
    arenaHitReactions(ev, s, r);
    REQUIRE(r.size() == 2);
    CHECK_FALSE(r[0].tookOver);
    CHECK_FALSE(r[1].tookOver);
}

TEST_CASE("arena hit react: no records, no reactions; None records are skipped") {
    const ArenaHitSetup s = loadSetup();
    ArenaEvents ev;
    ArenaHitReactions r;
    arenaHitReactions(ev, s, r);
    CHECK(r.empty());
    push(ev, blow(ArenaHitTier::None, Vec2{2.f, 0.f}, Vec2{}, 1.f));
    arenaHitReactions(ev, s, r);
    CHECK(r.empty());
}

TEST_CASE("arena hit react: a fight with records and the reactions allocate nothing") {
    REQUIRE(ftcore_test::allocHookInstalled());
    const ArenaTable t = loadTable();
    const ArenaHitSetup s = loadSetup();
    HRig r(t.fighter("default"), {kindOf(t, "brute"), kindOf(t, "maw_pup")}, 2);
    r.fight.setNoDefeatForTest(true);
    r.bodies.enemies[1] = Vec2{0.f, 1.2f};
    ArenaHitReactions out;
    int reactions = 0;
    const auto before = ftcore_test::allocStats();
    for (int k = 0; k < 60 * 15; ++k) {
        const ArenaOutput& o = r.tick();
        arenaHitReactions(o.events, s, out);
        reactions += static_cast<int>(out.size());
    }
    const auto after = ftcore_test::allocStats();
    CHECK(after.count == before.count);
    REQUIRE(reactions > 0);
}

namespace {

// The partner's ally body beside the player (test_arena_partner.cpp's PRig, cut down): bodies pinned, the ally swapped
// with the player on TookOver as the host does.
struct PartRig {
    ArenaDef def;
    ArenaFight fight;
    ArenaBodies bodies;
    ArenaInput in;
    PartnerBrain brain;
    PartnerTuning tuning;

    PartRig(const ArenaFighter& fighter, const ArenaEnemyKind& kind, Vec2 enemy, Vec2 player, Vec2 ally, int allyHp,
            Vec2 facing) {
        def.fighter = fighter;
        def.partnerKit = fighter;
        def.enemies[0] = kind;
        def.enemyCount = 1;
        def.attackers = 1;
        bodies.enemies[0] = enemy;
        bodies.player = player;
        bodies.ally = ally;
        fight.setAlly(allyHp, 0.4f);
        fight.setPartner(&brain, &tuning);
        fight.begin(def, ArenaRing{Vec2{0.f, 0.f}, 30.f}, player, facing, 1);
    }
    const ArenaOutput& tick() {
        const ArenaOutput& out = fight.tick(kDt, in, bodies);
        for (const ArenaEvent& e : out.events)
            if (e.type == ArenaEventType::TookOver) std::swap(bodies.player, bodies.ally);
        return out;
    }
};

} // namespace

TEST_CASE("arena hit react: an enemy's blow on the partner's body is stamped on AllyHit, target the ally") {
    const ArenaTable t = loadTable();
    const ArenaHitSetup s = loadSetup();
    ArenaEnemyKind pup = kindOf(t, "maw_pup");
    for (std::size_t m = 1; m < pup.moveCount; ++m) pup.moves[m].weight = 0.f;
    // The rig never moves bodies: the pup swings at the player from where it stands (a long maxRange, no strafing
    // walk-in), and the nip's 1.6 m reaches the ally in between, never the player 3.5 m off.
    pup.attackDelay = 0.f;
    pup.preferredRange = 5.f;
    pup.runRange = 10.f;
    pup.moves[0].maxRange = 5.f;
    PartRig r(t.fighter("default"), pup, Vec2{0.f, 0.f}, Vec2{3.5f, 0.f}, Vec2{1.2f, 0.f}, 200, Vec2{-1.f, 0.f});
    bool seen = false;
    for (int k = 0; k < 60 * 20 && !seen && !r.fight.over(); ++k) {
        const ArenaOutput& out = r.tick();
        const int at = indexOf(out.events, ArenaEventType::AllyHit, kArenaAlly);
        if (at < 0) continue;
        seen = true;
        CHECK(arenaHitTierOf(out.events, static_cast<std::size_t>(at)) != ArenaHitTier::None);
        ArenaHitReactions re;
        arenaHitReactions(out.events, s, re);
        REQUIRE(re.size() >= 1);
        const ArenaHitReaction& a = re[re.size() - 1];
        CHECK(a.target == kArenaAlly);
        CHECK(a.tier == ArenaHitTier::Light); // the nip's poise 5 is under the kit's
        CHECK(a.dir.x > 0.99f);               // the pup at the origin pushes the ally at +x further out
    }
    REQUIRE(seen);
}

TEST_CASE("arena hit react: a fall with the partner up is the player's Knockout, flagged tookOver, no ally reaction") {
    const ArenaTable t = loadTable();
    const ArenaHitSetup s = loadSetup();
    ArenaEnemyKind pup = kindOf(t, "maw_pup");
    for (std::size_t m = 1; m < pup.moveCount; ++m) pup.moves[m].weight = 0.f;
    ArenaFighter frail = t.fighter("default");
    frail.hp = 3; // one nip
    PartRig r(frail, pup, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, Vec2{0.f, 3.5f}, 80, Vec2{-1.f, 0.f});
    bool seen = false;
    for (int k = 0; k < 60 * 10 && !seen && !r.fight.over(); ++k) {
        const ArenaOutput& out = r.tick();
        if (indexOf(out.events, ArenaEventType::TookOver, kArenaPlayer) < 0) continue;
        seen = true;
        ArenaHitReactions re;
        arenaHitReactions(out.events, s, re);
        REQUIRE(re.size() == 1);
        CHECK(re[0].target == kArenaPlayer);
        CHECK(re[0].tier == ArenaHitTier::Knockout);
        CHECK(re[0].tookOver);
        const int allyDowned = indexOf(out.events, ArenaEventType::AllyDowned, kArenaAlly);
        REQUIRE(allyDowned >= 0);
        CHECK(arenaHitTierOf(out.events, static_cast<std::size_t>(allyDowned)) == ArenaHitTier::None);
    }
    REQUIRE(seen);
}
