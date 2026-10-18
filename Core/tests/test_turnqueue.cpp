#include "ftcore/TurnQueue.h"

#include <doctest/doctest.h>

#include <map>

using namespace ftcore;

TEST_CASE("faster combatants act proportionally more often") {
    TurnQueue q;
    q.add(0, 10);
    q.add(1, 20);
    std::map<CombatantId, int> turns;
    for (int i = 0; i < 300; ++i) ++turns[q.next()];
    CHECK(turns[1] == doctest::Approx(2 * turns[0]).epsilon(0.02));
}

TEST_CASE("ties go to higher speed then lower id") {
    TurnQueue q;
    q.add(3, 10);
    q.add(1, 10);
    q.add(2, 10);
    CHECK(q.next() == 1);
    CHECK(q.next() == 2);
    CHECK(q.next() == 3);
}

TEST_CASE("preview matches actual order and does not mutate") {
    TurnQueue q;
    q.add(0, 7);
    q.add(1, 11);
    q.add(2, 13);
    auto preview = q.preview(10);
    auto again = q.preview(10);
    CHECK(preview == again);
    for (auto id : preview) CHECK(q.next() == id);
}

TEST_CASE("removed combatants never act") {
    TurnQueue q;
    q.add(0, 10);
    q.add(1, 30);
    q.remove(1);
    for (int i = 0; i < 5; ++i) CHECK(q.next() == 0);
    CHECK_FALSE(q.contains(1));
}
