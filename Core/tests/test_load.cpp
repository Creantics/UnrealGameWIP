#include "ftcore/Load.h"

#include <doctest/doctest.h>

using namespace ftcore;

TEST_CASE("load: stores up to the cap, never below zero") {
    Load l;
    CHECK(l.value == 0);
    CHECK(l.store(30) == 30);
    CHECK(l.store(0) == 0);
    CHECK(l.store(-5) == 0);
    CHECK(l.value == 30);
    CHECK_FALSE(l.overloaded());
    CHECK(l.store(90) == 70); // only the room left
    CHECK(l.value == kMaxLoad);
    CHECK(l.overloaded());
    CHECK(l.store(10) == 0);
}

TEST_CASE("load: release empties it and returns what it held") {
    Load l;
    l.store(64);
    CHECK(l.release() == 64);
    CHECK(l.value == 0);
    CHECK(l.release() == 0);
}

TEST_CASE("load: a Release's notes are 4 plus one per 20 Load") {
    Load l;
    CHECK(l.releaseNotes() == 4);
    l.store(19);
    CHECK(l.releaseNotes() == 4);
    l.store(1);
    CHECK(l.releaseNotes() == 5);
    l.store(40);
    CHECK(l.releaseNotes() == 7); // 60: the tutorial's Release
    l.store(40);
    CHECK(l.releaseNotes() == 9); // full
}

TEST_CASE("load: share moves a fraction to another, as much as it has room for") {
    Load a;
    Load b;
    a.store(80);
    CHECK(a.share(b, 0.5f) == 40);
    CHECK(a.value == 40);
    CHECK(b.value == 40);
    CHECK(a.share(b, 0.0f) == 0);
    CHECK(a.share(b, -1.0f) == 0);

    Load full;
    full.store(95);
    Load c;
    c.store(50);
    CHECK(c.share(full, 1.0f) == 5); // full has room for 5
    CHECK(c.value == 45);
    CHECK(full.overloaded());

    Load d;
    d.store(7);
    Load e;
    CHECK(d.share(e, 2.0f) == 7); // a fraction over 1 moves it all
    CHECK(d.value == 0);
}
