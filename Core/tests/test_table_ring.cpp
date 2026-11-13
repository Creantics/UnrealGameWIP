// Setting the table in the arena: the ring and who sits in it, the even split of a blow, one table per place, the
// cooldown by Load cap and the carry grade.
#include "ftcore/ArenaFight.h"
#include "ftcore/Places.h"
#include "ftcore/TableRing.h"
#include "ftcore/TableSet.h"

#include "TestAssets.h"

#include <ostream>
#include <doctest/doctest.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 60.f;

ArenaAttack swing(float startup, float active, float recovery, float range, int damage) {
    ArenaAttack a;
    a.startup = startup;
    a.active = active;
    a.recovery = recovery;
    a.range = range;
    a.arcCos = -1.f;
    a.damage = damage;
    a.poiseDamage = 0.f;
    a.guardDamage = 0.f;
    a.lunge = 0.f;
    a.tracking = 0.f;
    a.armor = 0.f;
    a.stamina = 0.f;
    a.cancel = 0.5f;
    return a;
}

ArenaFighter plainFighter() {
    ArenaFighter f;
    f.hp = 100;
    f.poise = 1000.f; // no stagger: the tests read hp only
    f.bodyRadius = 0.4f;
    f.lightCount = 1;
    f.light[0] = swing(0.2f, 0.1f, 0.3f, 2.f, 10);
    f.heavy = swing(0.4f, 0.2f, 0.5f, 2.f, 30);
    f.riposte = swing(0.1f, 0.1f, 0.4f, 2.5f, 50);
    f.release = swing(0.1f, 0.1f, 0.4f, 2.5f, 20);
    return f;
}

// Stands where it is placed and never attacks.
ArenaEnemyKind standerKind(float radius = 0.45f) {
    ArenaEnemyKind k;
    k.hp = 100;
    k.poise = 1000.f;
    k.attackDelay = 1000.f;
    k.passive = true;
    k.walkSpeed = k.runSpeed = k.strafeSpeed = 0.f;
    k.bodyRadius = radius;
    return k;
}

// Bites the player once (it never moves: the rig doesn't move the bodies).
ArenaEnemyKind biterKind(int damage, float armor = 0.f) {
    ArenaEnemyKind k;
    k.hp = 100;
    k.poise = 1000.f;
    k.attackDelay = 0.f;
    k.preferredRange = 3.f;
    k.runRange = 8.f;
    k.bodyRadius = 0.45f;
    k.moveCount = 1;
    ArenaEnemyMove& m = k.moves[0];
    m.attack = swing(0.4f, 0.15f, 0.5f, 2.f, damage);
    m.attack.armor = armor;
    m.minRange = 0.f;
    m.maxRange = 3.f;
    m.cooldown = 60.f;
    return k;
}

struct Rig {
    ArenaDef def;
    ArenaFight fight;
    ArenaBodies bodies;
    ArenaInput in;
    TableRing ring;
    std::vector<ArenaEvent> log;

    explicit Rig(std::initializer_list<ArenaEnemyKind> kinds) {
        def.fighter = plainFighter();
        std::size_t n = 0;
        for (const ArenaEnemyKind& k : kinds) def.enemies[n++] = k;
        def.enemyCount = static_cast<std::uint8_t>(n);
        def.attackers = 1;
        fight.begin(def, ArenaRing{Vec2{0.f, 0.f}, 30.f}, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 1);
        for (std::size_t i = 0; i < n; ++i) bodies.enemies[i] = Vec2{1.5f, 0.f};
        fight.setAlly(100, 0.4f);
        bodies.ally = Vec2{-3.f, 0.f}; // inside the ring, out of a bite's reach
        fight.tick(0.f, in, bodies); // the output and the ally's place
    }
    void tick() {
        const ArenaOutput& out = fight.tick(kDt, in, bodies);
        for (const ArenaEvent& e : out.events) log.push_back(e);
    }
    int sum(ArenaEventType type, std::uint8_t target) const {
        int s = 0;
        for (const ArenaEvent& e : log)
            if (e.type == type && e.target == target) s += e.amount;
        return s;
    }
    int count(ArenaEventType type) const {
        int c = 0;
        for (const ArenaEvent& e : log)
            if (e.type == type) ++c;
        return c;
    }
    // Seats the table round the player and hands the fight its seats.
    int sit(float enemyShare = 1.f, int seats = 3) {
        ring.place(Vec2{0.f, 0.f});
        const int b = ring.sit(fight, bodies);
        fight.setTableSeats(ring.seats(fight, enemyShare, seats));
        return b;
    }
    // Ticks until the first enemy bite has landed (or 2 s).
    void untilBite() {
        for (int k = 0; k < 120 && count(ArenaEventType::Hit) == 0; ++k) tick();
    }
};

Load fullLoad() {
    Load l;
    l.value = kMaxLoad;
    return l;
}

PlaceTable threeSeats() {
    PlaceTable p;
    p.reset(3);
    p.leaveOpen();
    p.leaveOpen();
    p.leaveOpen();
    return p;
}

} // namespace

TEST_CASE("table: the even split and the carry grade") {
    CHECK(tableEvenSplit(22, 3).each == 7);
    CHECK(tableEvenSplit(22, 3).struck == 8); // the rest goes to the one struck
    CHECK(tableEvenSplit(21, 3).each == 7);
    CHECK(tableEvenSplit(21, 3).struck == 7);
    CHECK(tableEvenSplit(5, 1).struck == 5);  // alone: no share
    CHECK(tableEvenSplit(0, 3).struck == 0);
    // A blow of 30 among 3, an enemy of carry 6 at a table of 4 seats: 10 x 0.5^2 = 2.5, rounded up to 3.
    const float share = tableEnemyShare(1.f, 6, 4, 0.5f);
    CHECK(share == doctest::Approx(0.25f));
    CHECK(tableEnemyPart(tableEvenSplit(30, 3).each, share) == 3);
    CHECK(tableEnemyShare(1.f, 0, 3, 0.5f) == 1.f);   // no carry: the whole part
    CHECK(tableEnemyShare(1.f, 3, 3, 0.5f) == 1.f);   // carry within the seats: the whole part
    CHECK(tableEnemyShare(1.f, 255, 0, 0.5f) == 0.f); // the heaviest carry: nothing
}

TEST_CASE("table: the cooldown grows as the Load cap shrinks") {
    CHECK(tableCooldownForCap(20.0, 100) == doctest::Approx(20.0));
    CHECK(tableCooldownForCap(20.0, 40) == doctest::Approx(50.0));
    CHECK(tableCooldownForCap(20.0, 1) == doctest::Approx(300.0));
    CHECK(tableCooldownForCap(20.0, 0) == doctest::Approx(300.0));

    // Cap 40: set at 0.8 s, cleared at 6.8 s, ready again 50 s later.
    TableTuning tuning;
    tuning.cooldown = tableCooldownForCap(20.0, 40);
    TableSet t;
    REQUIRE(t.reset(tuning, 0.0));
    Load load = fullLoad();
    REQUIRE(t.begin(kLeadA, load, threeSeats(), 0, 0.0) == TableError::None);
    t.update(56.79);
    CHECK(t.state() == TableState::Cooling);
    t.update(56.8);
    CHECK(t.state() == TableState::Ready);
}

TEST_CASE("table: one table per place") {
    const PlaceTable seats = threeSeats();
    TableSet a;
    TableSet b;
    Load la = fullLoad();
    REQUIRE(a.beginAt(kLeadA, la, seats, 0, TableSpot{0.f, 0.f, 6.f}, nullptr, 0, 0.0) == TableError::None);
    const TableSet* others[] = {&a};

    // While a is only setting, it blocks nothing.
    {
        TableSet c;
        Load lc = fullLoad();
        CHECK(c.beginAt(kLeadB, lc, seats, 0, TableSpot{1.f, 0.f, 6.f}, others, 1, 0.1) == TableError::None);
    }
    a.update(1.0);
    REQUIRE(a.set());

    // 11.9 m apart overlaps two 6 m rings: refused, nothing spent, no cooldown.
    Load lb = fullLoad();
    CHECK(b.beginAt(kLeadB, lb, seats, 0, TableSpot{11.9f, 0.f, 6.f}, others, 1, 1.0) == TableError::Occupied);
    CHECK(lb.value == kMaxLoad);
    CHECK(b.state() == TableState::Ready);
    // 12 m apart only touches: allowed.
    CHECK(b.beginAt(kLeadB, lb, seats, 0, TableSpot{12.f, 0.f, 6.f}, others, 1, 1.0) == TableError::None);
    CHECK(lb.value == 0);
    CHECK(b.spot().x == 12.f);
}

TEST_CASE("table ring: who sits, and when they stand") {
    SUBCASE("an edge exactly at the radius sits; one metre past still sits; past that it stands") {
        Rig rig({standerKind(0.45f)});
        rig.bodies.enemies[0] = Vec2{6.45f, 0.f}; // edge 6.0
        CHECK(rig.sit() == 3);                     // the player, the ally, the enemy
        CHECK((rig.ring.seated() & tableEnemyBit(0)) != 0);
        rig.bodies.enemies[0] = Vec2{7.45f, 0.f}; // edge 7.0: radius + standMargin
        rig.tick();
        CHECK_FALSE(rig.ring.update(rig.fight, rig.bodies).any());
        rig.bodies.enemies[0] = Vec2{7.46f, 0.f}; // edge 7.01
        rig.tick();
        const TableRingChange c = rig.ring.update(rig.fight, rig.bodies);
        CHECK(c.stood == 1);
        CHECK((rig.ring.stood() & tableEnemyBit(0)) != 0);
        // It never sits again at this table.
        rig.bodies.enemies[0] = Vec2{2.f, 0.f};
        rig.tick();
        rig.ring.update(rig.fight, rig.bodies);
        CHECK((rig.ring.seated() & tableEnemyBit(0)) == 0);
        CHECK(rig.ring.bodies() == 2);
    }
    SUBCASE("a pup (radius 0.35) at 7.35 m stays seated; at 7.36 m it stands") {
        Rig rig({standerKind(0.35f)});
        rig.bodies.enemies[0] = Vec2{3.f, 0.f};
        REQUIRE(rig.sit() == 3);
        rig.bodies.enemies[0] = Vec2{7.35f, 0.f};
        rig.tick();
        CHECK(rig.ring.update(rig.fight, rig.bodies).stood == 0);
        rig.bodies.enemies[0] = Vec2{7.36f, 0.f};
        rig.tick();
        CHECK(rig.ring.update(rig.fight, rig.bodies).stood == 1);
    }
    SUBCASE("outside the ring at the set, it never sits") {
        Rig rig({standerKind(0.45f)});
        rig.bodies.enemies[0] = Vec2{6.5f, 0.f}; // edge 6.05
        CHECK(rig.sit() == 2);
        rig.bodies.enemies[0] = Vec2{2.f, 0.f};
        rig.tick();
        rig.ring.update(rig.fight, rig.bodies);
        CHECK(rig.ring.bodies() == 2);
    }
    SUBCASE("a kind that may not be seated never is") {
        ArenaEnemyKind k = standerKind();
        k.seatable = false;
        Rig rig({k});
        CHECK(rig.sit() == 2);
    }
    SUBCASE("the ally stands past the margin, and so does the setter") {
        Rig rig({standerKind()});
        REQUIRE(rig.sit() == 3);
        rig.bodies.ally = Vec2{0.f, 7.5f}; // edge 7.1
        rig.tick();
        CHECK(rig.ring.update(rig.fight, rig.bodies).stood == 1);
        CHECK((rig.ring.seated() & kTablePlayerBit) != 0);
        rig.bodies.player = Vec2{-7.35f, 0.f}; // edge 6.95: still seated
        rig.tick();
        CHECK(rig.ring.update(rig.fight, rig.bodies).stood == 0);
        rig.bodies.player = Vec2{-7.45f, 0.f}; // edge 7.05: past radius + margin
        rig.tick();
        CHECK(rig.ring.update(rig.fight, rig.bodies).stood == 1);
        CHECK((rig.ring.seated() & kTablePlayerBit) == 0);
        CHECK((rig.ring.stood() & kTablePlayerBit) != 0);
    }
}

TEST_CASE("table ring: an armoured enemy mid-attack sits on its first recovery tick") {
    Rig rig({biterKind(10, 0.5f)});
    rig.fight.setAlly(0, 0.4f); // no ally: the bite has only the player to reach
    int guard = 0;
    while (rig.fight.output().enemies[0].part != ArenaPart::Active && guard++ < 120) rig.tick();
    REQUIRE(rig.fight.output().enemies[0].part == ArenaPart::Active);
    CHECK(rig.sit() == 1); // only the setter
    CHECK((rig.ring.waiting() & tableEnemyBit(0)) != 0);
    guard = 0;
    while (rig.fight.output().enemies[0].part == ArenaPart::Active && guard++ < 120) {
        CHECK_FALSE(rig.ring.update(rig.fight, rig.bodies).any());
        rig.tick();
    }
    REQUIRE(rig.fight.output().enemies[0].part == ArenaPart::Recovery);
    const TableRingChange c = rig.ring.update(rig.fight, rig.bodies);
    CHECK(c.sat == 1);
    CHECK(rig.ring.bodies() == 2);
}

TEST_CASE("table: a setter and one ally are too few; the table gutters at the set") {
    TableTuning tuning;
    tuning.minBodies = 3;
    TableSet t;
    REQUIRE(t.reset(tuning, 0.0));
    Load load = fullLoad();
    REQUIRE(t.begin(kLeadA, load, threeSeats(), 0, 0.0) == TableError::None);
    t.update(0.8);
    REQUIRE(t.set());
    Rig rig({standerKind()});
    rig.bodies.enemies[0] = Vec2{20.f, 0.f}; // far outside the ring
    const int bodies = rig.sit();
    CHECK(bodies == 2);
    CHECK(t.checkBodies(bodies, 0.8));
    const TableEvents& ev = t.update(0.8);
    bool guttered = false;
    for (const TableEvent& e : ev) guttered = guttered || e.type == TableEventType::Guttered;
    CHECK(guttered);
    CHECK(t.state() == TableState::Cooling);
    CHECK_FALSE(t.checkBodies(0, 0.9)); // not set any more
}

TEST_CASE("table: a blow on a seated lead is shared evenly") {
    for (const int damage : {22, 21}) {
        CAPTURE(damage);
        Rig rig({biterKind(damage)});
        REQUIRE(rig.sit() == 3); // the player, the ally (3 m away) and the biter (1.5 m away)
        rig.untilBite();
        const int each = damage / 3;
        CHECK(rig.sum(ArenaEventType::Hit, kArenaPlayer) == damage - 2 * each); // 8 of 22, 7 of 21
        CHECK(rig.sum(ArenaEventType::TableShare, kArenaAlly) == each);
        CHECK(rig.sum(ArenaEventType::TableShare, 0) == each); // the biter shares its own blow
        CHECK(rig.fight.hp() == 100 - (damage - 2 * each));
        CHECK(rig.fight.allyHp() == 100 - each);
        CHECK(rig.fight.output().enemies[0].brain != ArenaBrain::Dead);
    }
}

TEST_CASE("table: a seated enemy's share falls with its carry grade") {
    ArenaEnemyKind k = biterKind(30);
    k.carry = 6;
    Rig rig({k});
    REQUIRE(rig.sit(1.f, 4) == 3);
    rig.untilBite();
    CHECK(rig.sum(ArenaEventType::Hit, kArenaPlayer) == 10);
    CHECK(rig.sum(ArenaEventType::TableShare, 0) == 3); // 10 x 0.5^2 = 2.5, halves up
}

TEST_CASE("table: no seats, no share (the default)") {
    Rig rig({biterKind(22)});
    rig.untilBite();
    CHECK(rig.sum(ArenaEventType::Hit, kArenaPlayer) == 22);
    CHECK(rig.count(ArenaEventType::TableShare) == 0);
    CHECK(rig.fight.allyHp() == 100);
}

TEST_CASE("table: a seated body downed by its share leaves the table at once") {
    Rig rig({biterKind(22)});
    rig.fight.setAlly(5, 0.4f);
    REQUIRE(rig.sit() == 3);
    rig.untilBite();
    CHECK(rig.sum(ArenaEventType::TableShare, kArenaAlly) == 5);
    CHECK(rig.count(ArenaEventType::AllyDowned) == 1);
    const TableRingChange c = rig.ring.update(rig.fight, rig.bodies);
    CHECK(c.left == 1);
    CHECK(rig.ring.bodies() == 2);
}

TEST_CASE("table: arena.json gives the carry grades and reads seatable") {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/arena.json", text)) FAIL("Data/Battle/arena.json not found");
    ArenaTable table;
    std::string error;
    REQUIRE_MESSAGE(importArenaTable(text, table, &error), error);
    int found = 0;
    for (std::size_t i = 0; i < table.enemyIds.size(); ++i) {
        if (table.enemyIds[i] == "kite_boss") {
            CHECK(table.enemies[i].carry == 3);
            ++found;
        } else if (table.enemyIds[i] == "maw_pup") {
            CHECK(table.enemies[i].carry == 1);
            ++found;
        } else {
            CHECK(table.enemies[i].carry == 0);
        }
        CHECK(table.enemies[i].seatable);
    }
    CHECK(found == 2);

    ArenaTable bad;
    CHECK_FALSE(importArenaTable(R"({"enemies":[{"id":"x","carry":300}]})", bad, &error));
    CHECK(error.find("carry") != std::string::npos);
}

TEST_CASE("table: the Release hold sets the table, a tap stays a Release") {
    TableHold h(0.5);
    // Not possible now: the press is the fight's.
    CHECK(h.step(true, true, false, 1.0) == TableHoldResult::None);
    CHECK_FALSE(h.holding());
    // Held the whole time: holding, then Begin at 0.5 s.
    CHECK(h.step(true, true, true, 2.0) == TableHoldResult::Holding);
    CHECK(h.holding());
    CHECK(h.step(false, true, true, 2.25) == TableHoldResult::Holding);
    CHECK(h.share() == doctest::Approx(0.5f));
    CHECK(h.step(false, true, true, 2.5) == TableHoldResult::Begin);
    CHECK_FALSE(h.holding());
    CHECK(h.share() == 0.f);
    // A tap: passed on as a Release at the let-go.
    CHECK(h.step(true, true, true, 3.0) == TableHoldResult::Holding);
    CHECK(h.step(false, false, true, 3.1) == TableHoldResult::PassRelease);
    // Pressed and let go inside one step: a Release at once.
    CHECK(h.step(true, false, true, 4.0) == TableHoldResult::PassRelease);
    CHECK_FALSE(h.holding());
    // The table stops being possible mid-hold: cancelled, nothing passed on; the next step is the fight's.
    CHECK(h.step(true, true, true, 5.0) == TableHoldResult::Holding);
    CHECK(h.step(false, true, false, 5.2) == TableHoldResult::Cancelled);
    CHECK(h.step(false, true, true, 5.3) == TableHoldResult::None);
    // A hold time of 0: the press sets it at once.
    TableHold now(0.0);
    CHECK(now.step(true, true, true, 1.0) == TableHoldResult::Begin);
}

TEST_CASE("table: the view the HUD reads") {
    TableTuning tt;
    tt.minSeats = 3;
    tt.setTime = 0.8;
    tt.setDuration = 6.0;
    tt.cooldown = 20.0;
    TableSet table;
    REQUIRE(table.reset(tt, 0.0));
    TableRing ring;
    TableHold hold;
    ArenaTableView v = tableView(table, ring, hold, 0.0);
    CHECK(v.state == static_cast<std::uint8_t>(TableState::Ready));
    CHECK(v.radius == 0.f);
    CHECK(v.duration == 0.f);
    Load load = fullLoad();
    REQUIRE(table.begin(kLeadA, load, threeSeats(), 0, 1.0) == TableError::None);
    ring.place(Vec2{2.f, -3.f});
    v = tableView(table, ring, hold, 1.2);
    CHECK(v.state == static_cast<std::uint8_t>(TableState::Setting));
    CHECK(v.duration == doctest::Approx(0.8f));
    CHECK(v.timeLeft == doctest::Approx(0.6f));
    CHECK(v.centreX == 2.f);
    CHECK(v.centreY == -3.f);
    CHECK(v.radius == 6.f);
    table.update(2.0);
    v = tableView(table, ring, hold, 2.0);
    CHECK(v.state == static_cast<std::uint8_t>(TableState::Set));
    CHECK(v.duration == doctest::Approx(6.f));
    CHECK(v.timeLeft == doctest::Approx(5.8f));
    table.update(8.0);
    ring.clear();
    v = tableView(table, ring, hold, 10.0);
    CHECK(v.state == static_cast<std::uint8_t>(TableState::Cooling));
    CHECK(v.cooldownLeft > 0.f);
    CHECK(v.timeLeft == 0.f);
    CHECK(v.radius == 0.f);
}

namespace {

// Bites from up to 10 m: placed 2 m past the ally, it reaches the ally and never the player at the origin.
ArenaEnemyKind farBiter(int damage, int hp = 100) {
    ArenaEnemyKind k = biterKind(damage);
    k.hp = hp;
    k.preferredRange = 10.f;
    k.runRange = 15.f;
    k.moves[0].maxRange = 10.f;
    return k;
}

// The ally 3 m along +x and the biter at 5 m: all three inside the ring round the origin.
void allySide(Rig& rig) {
    rig.bodies.ally = Vec2{3.f, 0.f};
    rig.bodies.enemies[0] = Vec2{5.f, 0.f};
}

// Starts the fight again with the player's hp and guard absorb changed (the ally given the same hp).
void rebegin(Rig& rig, int hp, float guardAbsorb = 1.f) {
    rig.def.fighter.hp = hp;
    rig.def.fighter.guardAbsorb = guardAbsorb;
    rig.fight.begin(rig.def, ArenaRing{Vec2{0.f, 0.f}, 30.f}, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 1);
    rig.fight.setAlly(hp, 0.4f);
    rig.fight.tick(0.f, rig.in, rig.bodies);
}

int allyLost(const Rig& rig) {
    return rig.sum(ArenaEventType::AllyHit, kArenaAlly) + rig.sum(ArenaEventType::TableShare, kArenaAlly);
}

// The tick (from 1) the bite lands on the ally with nothing pressed (the fight is deterministic), or -1.
int allyLandTick(int damage) {
    Rig rig({farBiter(damage)});
    allySide(rig);
    rig.sit();
    for (int k = 1; k <= 120; ++k) {
        rig.tick();
        if (rig.count(ArenaEventType::AllyHit) > 0) return k;
    }
    return -1;
}

ArenaChairs takeHalfChair() {
    ArenaChairs c;
    c.named[0] = ChairEffect{ChairKind::TakeHalf, 0.1f, 0.05f};
    c.count = 1;
    return c;
}

} // namespace

TEST_CASE("table: a shared blow with the take-half chair heals nothing") {
    Rig rig({farBiter(30)});
    allySide(rig);
    rig.fight.setChairs(takeHalfChair());
    REQUIRE(rig.sit() == 3);
    for (int k = 0; k < 120 && rig.count(ArenaEventType::AllyHit) == 0; ++k) rig.tick();
    REQUIRE(rig.count(ArenaEventType::AllyHit) == 1);
    CHECK(rig.count(ArenaEventType::TookHalf) == 0);
    CHECK(rig.sum(ArenaEventType::AllyHit, kArenaAlly) == 10);
    CHECK(rig.sum(ArenaEventType::TableShare, kArenaPlayer) == 10);
    CHECK(rig.fight.allyHp() == 100 - allyLost(rig)); // no heal
    CHECK(rig.fight.allyHp() == 90);
}

TEST_CASE("table: take half at a set table: the lead's half first, the rest shared, the chair heals once") {
    const int land = allyLandTick(30);
    REQUIRE(land > 6);
    Rig rig({farBiter(30)});
    allySide(rig);
    rig.fight.setChairs(takeHalfChair());
    REQUIRE(rig.sit() == 3);
    for (int k = 1; k <= land + 2; ++k) {
        rig.in.takeHalf = k == land - 5;
        rig.tick();
    }
    rig.in.takeHalf = false;
    CHECK(rig.sum(ArenaEventType::TookHalf, kArenaPlayer) == 15);
    // The other 15 split three ways: 5 each.
    CHECK(rig.sum(ArenaEventType::AllyHit, kArenaAlly) == 5);
    CHECK(rig.sum(ArenaEventType::TableShare, kArenaPlayer) == 5);
    CHECK(rig.sum(ArenaEventType::TableShare, 0) == 5);
    CHECK(rig.fight.hp() == 100 - 15 - 5);
    CHECK(rig.fight.allyHp() == 100); // 95, and the chair's 5 back
}

TEST_CASE("table: a blocked blow shares its chip; a parried one shares nothing") {
    SUBCASE("block") {
        Rig rig({biterKind(30)});
        rebegin(rig, 100, 0.5f);
        REQUIRE(rig.sit() == 3);
        rig.in.guard = true;
        for (int k = 0; k < 120 && rig.count(ArenaEventType::Blocked) == 0; ++k) rig.tick();
        REQUIRE(rig.count(ArenaEventType::Blocked) == 1);
        CHECK(rig.sum(ArenaEventType::Hit, kArenaPlayer) == 5); // chip 15, split three ways
        CHECK(rig.sum(ArenaEventType::TableShare, kArenaAlly) == 5);
        CHECK(rig.sum(ArenaEventType::TableShare, 0) == 5);
    }
    SUBCASE("parry") {
        Rig rig({biterKind(30)});
        REQUIRE(rig.sit() == 3);
        for (int k = 1; k <= 40; ++k) {
            if (k == 19) rig.in.guard = true;
            rig.tick();
        }
        REQUIRE(rig.count(ArenaEventType::Parried) == 1);
        CHECK(rig.count(ArenaEventType::TableShare) == 0);
        CHECK(rig.fight.hp() == 100);
        CHECK(rig.fight.allyHp() == 100);
    }
}

TEST_CASE("table: a seated body carries its share whatever it is doing") {
    const int land = allyLandTick(30);
    REQUIRE(land > 8);
    enum class Doing { Guard, Parry, Dodge, Release, Air };
    for (const Doing doing : {Doing::Guard, Doing::Parry, Doing::Dodge, Doing::Release, Doing::Air}) {
        CAPTURE(static_cast<int>(doing));
        Rig rig({farBiter(30)});
        allySide(rig);
        if (doing == Doing::Release) {
            rig.fight.setCarry(true);
            rig.fight.setLoadCap(100);
            rig.tick();
            rig.fight.giveLoad(100);
        }
        REQUIRE(rig.sit() == 3);
        ArenaAction at = ArenaAction::Idle;
        bool invulnerable = false;
        float height = 0.f;
        for (int k = 1; k <= land + 2 && rig.count(ArenaEventType::AllyHit) == 0; ++k) {
            rig.in.guard = doing == Doing::Guard || (doing == Doing::Parry && k >= land - 3);
            rig.in.dodge = doing == Doing::Dodge && k == land - 2;
            rig.in.release = doing == Doing::Release && k == land - 2;
            rig.in.jump = doing == Doing::Air && k == land - 12;
            rig.tick();
            at = rig.fight.output().player.action;
            invulnerable = rig.fight.output().player.invulnerable;
            height = rig.fight.output().player.height;
        }
        REQUIRE(rig.count(ArenaEventType::AllyHit) == 1);
        switch (doing) {
        case Doing::Guard: CHECK(at == ArenaAction::Guard); break;
        case Doing::Parry: CHECK(at == ArenaAction::Guard); break;
        case Doing::Dodge: CHECK(invulnerable); break;
        case Doing::Release: CHECK(at == ArenaAction::Release); CHECK(invulnerable); break;
        case Doing::Air: CHECK(height > 0.f); break;
        }
        CHECK(rig.sum(ArenaEventType::TableShare, kArenaPlayer) == 10);
        CHECK(rig.fight.hp() == 100 - 10);
    }
}

TEST_CASE("table: the player walks out during the wind-up and carries nothing") {
    Rig rig({farBiter(30)});
    allySide(rig);
    REQUIRE(rig.sit() == 3);
    int guard = 0;
    while (rig.fight.output().enemies[0].part != ArenaPart::Startup && guard++ < 120) rig.tick();
    REQUIRE(rig.fight.output().enemies[0].part == ArenaPart::Startup);
    rig.bodies.player = Vec2{-7.5f, 0.f}; // edge 7.1
    CHECK(rig.ring.update(rig.fight, rig.bodies).stood == 1);
    rig.fight.setTableSeats(rig.ring.seats(rig.fight, 1.f, 3));
    for (int k = 0; k < 120 && rig.count(ArenaEventType::AllyHit) == 0; ++k) rig.tick();
    REQUIRE(rig.count(ArenaEventType::AllyHit) == 1);
    CHECK(rig.sum(ArenaEventType::TableShare, kArenaPlayer) == 0);
    CHECK(rig.sum(ArenaEventType::AllyHit, kArenaAlly) == 15); // two at the table now
    CHECK(rig.sum(ArenaEventType::TableShare, 0) == 15);
    CHECK(rig.fight.hp() == 100);
}

TEST_CASE("table: an enemy killed by its own blow's share is left alone after") {
    for (const bool onAlly : {false, true}) {
        CAPTURE(onAlly);
        Rig rig({onAlly ? farBiter(30, 4) : [] { ArenaEnemyKind k = biterKind(30); k.hp = 4; return k; }()});
        if (onAlly) allySide(rig);
        REQUIRE(rig.sit() == 3);
        int k = 0;
        for (; k < 120 && rig.count(ArenaEventType::TableShare) == 0; ++k) rig.tick();
        REQUIRE(rig.count(ArenaEventType::TableShare) > 0);
        CHECK(rig.sum(ArenaEventType::TableShare, 0) == 4); // the hp floor: 4 of its 10
        const ArenaEnemyOut& foe = rig.fight.output().enemies[0];
        CHECK(foe.brain == ArenaBrain::Dead);
        CHECK(foe.velocity.x == 0.f);
        CHECK(foe.velocity.y == 0.f);
        CHECK(rig.fight.stats().kills == 1);
        const std::size_t events = rig.log.size();
        for (int j = 0; j < 120; ++j) rig.tick();
        for (std::size_t e = events; e < rig.log.size(); ++e) CHECK(rig.log[e].source != 0);
        CHECK(rig.fight.output().enemies[0].brain == ArenaBrain::Dead);
        CHECK(rig.fight.stats().kills == 1);
    }
}

TEST_CASE("table: a shared blow sums to the blow exactly, the remainder to the one struck") {
    for (int extra = 0; extra <= 3; ++extra) { // bodies: the player, the ally, the biter and `extra` standers
        for (int damage = 1; damage <= 200; ++damage) {
            CAPTURE(extra);
            CAPTURE(damage);
            ArenaEnemyKind biter = biterKind(damage);
            biter.hp = 1000;
            ArenaEnemyKind stander = standerKind();
            stander.hp = 1000;
            Rig rig = extra == 0 ? Rig({biter})
                    : extra == 1 ? Rig({biter, stander})
                    : extra == 2 ? Rig({biter, stander, stander})
                                 : Rig({biter, stander, stander, stander});
            const Vec2 spots[3] = {Vec2{0.f, 3.f}, Vec2{0.f, -3.f}, Vec2{-3.f, 3.f}};
            for (int i = 0; i < extra; ++i) rig.bodies.enemies[static_cast<std::size_t>(i) + 1] = spots[i];
            rebegin(rig, 1000);
            const int bodies = 3 + extra;
            REQUIRE(rig.sit() == bodies);
            rig.untilBite();
            REQUIRE(rig.count(ArenaEventType::Hit) == 1);
            const int each = damage / bodies;
            int total = rig.sum(ArenaEventType::Hit, kArenaPlayer) + rig.sum(ArenaEventType::TableShare, kArenaAlly);
            for (int i = 0; i < bodies - 2; ++i) total += rig.sum(ArenaEventType::TableShare, static_cast<std::uint8_t>(i));
            CHECK(total == damage);
            CHECK(rig.sum(ArenaEventType::Hit, kArenaPlayer) == damage - each * (bodies - 1));
        }
    }
}
