#include "ftcore/PathFollow.h"

#include <doctest/doctest.h>

#include <array>

using namespace ftcore;

TEST_CASE("an empty path has no waypoint") {
    CHECK(advanceWaypoint(nullptr, 0, 0, {0.f, 0.f}, 0.5f) == -1);
}

TEST_CASE("the waypoint moves on when reached and stops at the end") {
    const std::array<Vec2, 4> path{{{0.f, 0.f}, {5.f, 0.f}, {5.f, 5.f}, {0.f, 5.f}}};
    CHECK(advanceWaypoint(path.data(), 4, 1, {0.f, 0.f}, 0.5f) == 1);
    CHECK(advanceWaypoint(path.data(), 4, 1, {4.7f, 0.1f}, 0.5f) == 2);
    CHECK(advanceWaypoint(path.data(), 4, 2, {4.8f, 5.6f}, 0.5f) == 3); // just past the corner
    CHECK(advanceWaypoint(path.data(), 4, 3, {0.f, 5.f}, 0.5f) == 3); // the last point stays
}

TEST_CASE("a corner the walker cut past is skipped, and it never goes back") {
    const std::array<Vec2, 3> path{{{0.f, 0.f}, {5.f, 0.f}, {5.f, 5.f}}};
    // Overshot the corner along the leg into it, more than `reach` from it.
    CHECK(advanceWaypoint(path.data(), 3, 1, {5.8f, 0.4f}, 0.5f) == 2);
    // Still before the corner, off to either side: keep steering at it.
    CHECK(advanceWaypoint(path.data(), 3, 1, {3.f, -0.5f}, 0.5f) == 1);
    CHECK(advanceWaypoint(path.data(), 3, 1, {2.f, 0.4f}, 0.5f) == 1);
    // Back at the start: the index stays where it was.
    CHECK(advanceWaypoint(path.data(), 3, 2, {0.f, 0.f}, 0.5f) == 2);
    // Out of range input is clamped.
    CHECK(advanceWaypoint(path.data(), 3, 9, {0.f, 0.f}, 0.5f) == 2);
}
