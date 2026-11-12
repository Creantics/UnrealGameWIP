#include "ftcore/StoryFaces.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cstring>
#include <string_view>

using namespace ftcore;

TEST_CASE("pickFaceRoute: uncast, own faces first, then a stand-in, else no face") {
    CHECK(pickFaceRoute(false, 3, 1) == FaceRoute::Uncast);
    CHECK(pickFaceRoute(true, 1, 1) == FaceRoute::Own);
    CHECK(pickFaceRoute(true, 0, 1) == FaceRoute::StandIn);
    CHECK(pickFaceRoute(true, 0, 0) == FaceRoute::NoFace);
}

TEST_CASE("FaceLineTally counts each voiced line once and plays only with a track on a bound face") {
    FaceLineTally t;
    t.add(FaceRoute::Own, true, true);
    t.add(FaceRoute::StandIn, true, true);
    t.add(FaceRoute::StandIn, true, false); // the face has no shapes bound
    t.add(FaceRoute::Own, false, false);    // no track file
    t.add(FaceRoute::Uncast, true, true);   // played is ignored without a face
    t.add(FaceRoute::NoFace, true, true);
    CHECK(t.voiced == 6);
    CHECK(t.own == 2);
    CHECK(t.standIn == 2);
    CHECK(t.uncast == 1);
    CHECK(t.noFace == 1);
    CHECK(t.noTrack == 1);
    CHECK(t.played == 2);
    CHECK(t.unseen == 1);
    CHECK(t.played + t.uncast + t.noFace + t.noTrack + t.unseen == t.voiced);

    char line[200];
    const std::size_t n = t.format(line, sizeof line);
    CHECK(std::string_view(line, n)
          == "Story: FACES 2 of 6 voiced lines played on a face (2 own, 2 stand-in, 1 uncast, 1 no face, 1 no track, 1 unseen)");
    char small[8];
    CHECK(t.format(small, sizeof small) == 7);
    CHECK(std::strlen(small) == 7);
    CHECK(t.format(nullptr, 4) == 0);
}

TEST_CASE("storyShotNameOk takes plain names only") {
    CHECK(storyShotNameOk("p01"));
    CHECK(storyShotNameOk("D97-P-01-L001"));
    CHECK(storyShotNameOk("shot_a"));
    CHECK_FALSE(storyShotNameOk(""));
    CHECK_FALSE(storyShotNameOk(".."));
    CHECK_FALSE(storyShotNameOk("a/b"));
    CHECK_FALSE(storyShotNameOk("a\\b"));
    CHECK_FALSE(storyShotNameOk(std::string_view("a\0b", 3)));
    CHECK_FALSE(storyShotNameOk("a\xC3\xA9"));
    CHECK(storyShotNameOk("-p01"));
    CHECK_FALSE(storyShotNameOk("C:shot"));
    CHECK_FALSE(storyShotNameOk("/abs"));
    CHECK_FALSE(storyShotNameOk("a b"));
    CHECK_FALSE(storyShotNameOk("a.b"));
    CHECK(storyShotNameOk(std::string_view("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")));        // 64
    CHECK_FALSE(storyShotNameOk(std::string_view("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"))); // 65
}

TEST_CASE("clampStoryShotMax keeps 0..64") {
    CHECK(clampStoryShotMax(-5) == 0);
    CHECK(clampStoryShotMax(0) == 0);
    CHECK(clampStoryShotMax(kStoryShotDefault) == 4);
    CHECK(clampStoryShotMax(64) == 64);
    CHECK(clampStoryShotMax(100000) == 64);
}

TEST_CASE("remainingStoryShots counts the whole run") {
    CHECK(remainingStoryShots(4, 0) == 4);
    CHECK(remainingStoryShots(4, 3) == 1);
    CHECK(remainingStoryShots(4, 4) == 0);
    CHECK(remainingStoryShots(4, 9) == 0);
    CHECK(remainingStoryShots(0, 0) == 0);
    CHECK(remainingStoryShots(500, 10) == 54);
    CHECK(remainingStoryShots(-1, 0) == 0);
    CHECK(remainingStoryShots(4, -2) == 4);
}
