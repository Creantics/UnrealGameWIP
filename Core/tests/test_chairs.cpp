// The chair table (Data/Battle/chairs.json) and the effects of the named chairs on a fight.
#include "ftcore/ArenaFight.h"
#include "ftcore/Chairs.h"
#include "ftcore/Places.h"

#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <string>

using namespace ftcore;

namespace {

bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

ArenaChairs healChairs(int n) {
    ArenaChairs c;
    for (int i = 0; i < n; ++i) c.named[static_cast<std::size_t>(i)] = ChairEffect{ChairKind::HealCharges, 1.f, 0.f};
    c.count = static_cast<std::uint8_t>(n);
    return c;
}

std::uint8_t chargesAtBegin(int kitCharges, const ArenaChairs& chairs, std::uint8_t* maxOut) {
    ArenaDef def;
    def.fighter.healCharges = static_cast<std::uint8_t>(kitCharges);
    def.enemies[0].hp = 100;
    def.enemyCount = 1;
    ArenaFight fight;
    fight.setChairs(chairs);
    fight.begin(def, ArenaRing{Vec2{0.f, 0.f}, 30.f}, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 1);
    if (maxOut) *maxOut = fight.healChargesMax();
    return fight.healCharges();
}

std::string importError(const char* rows) {
    ChairTable t;
    t.rows.push_back(ChairRow{});
    std::string err;
    const std::string json = std::string(R"({ "version": 1, "chairs": [ )") + rows + " ] }";
    if (importChairTable(json, t, &err)) return std::string();
    CHECK(t.rows.empty());
    return err;
}

} // namespace

TEST_CASE("chairs: a healCharges chair adds to the kit's charges from begin (S15)") {
    std::uint8_t max = 0;
    CHECK(chargesAtBegin(3, healChairs(1), &max) == 4);
    CHECK(max == 4);
    CHECK(chargesAtBegin(3, ArenaChairs{}, &max) == 3);
    CHECK(max == 3);
}

TEST_CASE("chairs: heal charges from chairs add up to 3 at most (S16)") {
    CHECK(healChairs(4).healCharges() == kChairHealChargesCap);
    CHECK(chargesAtBegin(3, healChairs(4), nullptr) == 6);
}

TEST_CASE("chairs: the shipped table loads, and only named places' chairs count (S17)") {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/chairs.json", text)) FAIL("Data/Battle/chairs.json not found");
    ChairTable t;
    std::string err;
    REQUIRE_MESSAGE(importChairTable(text, t, &err), err);
    CHECK(t.rows.size() == 3);
    const ChairRow* c = t.findKey("chair_a03");
    REQUIRE(c);
    CHECK(c->person == 3);
    CHECK(c->effect.kind == ChairKind::TakeHalf);
    CHECK(c->effect.a == doctest::Approx(0.1f));
    CHECK(c->effect.b == doctest::Approx(0.05f));
    CHECK(t.findKey("chair_zz") == nullptr);
    CHECK(t.findPerson(kNoPerson) == nullptr);

    PlaceTable places;
    places.reset(4);
    places.layPlace(1);
    places.leaveOpen();
    places.layPlace(3);
    places.layPlace(9); // named, but no chair row: no slot
    const ArenaChairs chairs = chairsFor(places, t);
    CHECK(chairs.count == 2);
    CHECK(chairs.loadGain() == doctest::Approx(0.25f));
    CHECK(chairs.loadGainRadius() == doctest::Approx(4.f));
    CHECK(chairs.takeHalfWindow() == doctest::Approx(0.1f));
    CHECK(chairs.takeHalfHeal() == doctest::Approx(0.05f));
    CHECK(chairs.healCharges() == 0);
    CHECK(places.carryCap() == 100);
}

TEST_CASE("chairs: a bad row is refused with its path, and the table comes back empty (S18)") {
    CHECK(contains(importError(R"({ "person": 1, "key": "c1", "kind": "heal", "a": 1 })"), "chairs[0].kind"));
    CHECK(contains(importError(R"({ "person": 1, "key": "c1", "kind": "loadGain", "a": 0.6, "b": 4 })"), "chairs[0].a"));
    CHECK(contains(importError(R"({ "person": 1, "key": "c1", "kind": "healCharges", "a": 1.5 })"), "chairs[0].a"));
    CHECK(contains(importError(R"({ "person": 2, "key": "c1", "kind": "healCharges", "a": 1 },
                                  { "person": 2, "key": "c2", "kind": "healCharges", "a": 1 })"),
                   "chairs[1].person"));
    CHECK(contains(importError(R"({ "person": 0, "key": "c1", "kind": "healCharges", "a": 1 })"), "chairs[0].person"));
    CHECK(contains(importError(R"({ "person": 1, "key": "Bad Key", "kind": "healCharges", "a": 1 })"), "chairs[0].key"));
    CHECK(importError(R"({ "person": 1, "key": "c1", "kind": "healCharges", "a": 1 })").empty());
    ChairTable t;
    CHECK_FALSE(importChairTable(R"({ "version": 2, "chairs": [] })", t, nullptr));
}
