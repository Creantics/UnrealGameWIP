#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include "AllocCounter.h"
#include "ftcore/StoryCast.h"
#include "ftcore/StoryDatabase.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

using namespace ftcore;

namespace {

FaceShapeSet threeShapes() {
    FaceShapeSet set;
    REQUIRE(set.addShape("jawOpen", FaceLayer::MouthArticulation));
    REQUIRE(set.addShape("eyeBlinkLeft", FaceLayer::Eyes));
    REQUIRE(set.addShape("cheekPuff", FaceLayer::MouthTone));
    REQUIRE(set.finish());
    return set;
}

} // namespace

TEST_CASE("face curve map: poses match shapes ignoring case; unmatched shapes are reported") {
    const FaceShapeSet set = threeShapes();
    const std::array<std::string_view, 3> poses{"EyeBlinkLeft", "JawOpen", "Unused"};
    const std::array<std::string_view, 3> curves{"CTRL_a", "CTRL_b", "CTRL_c"};
    // rows: pose, columns: curve
    const std::array<float, 9> values{0.f, 0.f, 1.f, //
                                      0.5f, 1.f, 0.f, //
                                      9.f, 9.f, 9.f};
    FaceCurveMap map;
    std::vector<std::string> unmatched;
    REQUIRE(buildFaceCurveMap(set, poses, curves, values, map, &unmatched));
    CHECK(map.curves == std::vector<std::string>{"CTRL_a", "CTRL_b", "CTRL_c"});
    CHECK(map.mappedShapes == 2);
    CHECK(unmatched == std::vector<std::string>{"cheekPuff"});
    REQUIRE(map.terms.size() == 3);
    for (std::size_t i = 1; i < map.terms.size(); ++i) CHECK(map.terms[i - 1].curve <= map.terms[i].curve);

    FaceWeights w{};
    w[0] = 0.8f; // jawOpen
    w[1] = 0.6f; // eyeBlinkLeft
    std::array<float, 4> out{7.f, 7.f, 7.f, 7.f};
    const std::uint64_t before = ftcore_test::allocStats().count;
    writeFaceCurves(map, w, out);
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(out[0] == doctest::Approx(0.4f));
    CHECK(out[1] == doctest::Approx(0.8f));
    CHECK(out[2] == doctest::Approx(0.6f));
    CHECK(out[3] == 0.f);

    w[0] = 1.f;
    w[1] = 1.f;
    const std::array<float, 9> big{0.f, 0.f, 0.9f, 0.9f, 0.f, 0.f, 0.f, 0.f, 0.f};
    const std::array<std::string_view, 3> two{"eyeblinkleft", "JAWOPEN", "x"};
    REQUIRE(buildFaceCurveMap(set, two, curves, big, map));
    writeFaceCurves(map, w, out);
    CHECK(out[0] == doctest::Approx(0.9f));
    CHECK(out[2] == doctest::Approx(0.9f));
}

TEST_CASE("face curve map: sums are clamped to [0, 1]; a size mismatch fails empty") {
    const FaceShapeSet set = threeShapes();
    const std::array<std::string_view, 2> poses{"jawOpen", "eyeBlinkLeft"};
    const std::array<std::string_view, 1> curves{"CTRL_x"};
    const std::array<float, 2> values{0.9f, 0.9f};
    FaceCurveMap map;
    REQUIRE(buildFaceCurveMap(set, poses, curves, values, map));
    FaceWeights w{};
    w[0] = 1.f;
    w[1] = 1.f;
    std::array<float, 1> out{};
    writeFaceCurves(map, w, out);
    CHECK(out[0] == 1.f);

    const std::array<float, 3> wrong{1.f, 1.f, 1.f};
    CHECK_FALSE(buildFaceCurveMap(set, poses, curves, wrong, map));
    CHECK(map.empty());
    CHECK(map.curves.empty());
}

TEST_CASE("scene cast: cast speakers in order of first line, each once") {
    StoryBuilder b;
    b.setStart("SC-A");
    b.beginChapter("01", "One");
    b.beginScene("SC-A", "A");
    b.line("L-1", "SPK-B", "One.");
    b.direction("Stage.");
    b.line("L-2", "SPK-X", "Not cast.");
    b.line("L-3", "SPK-A", "Two.");
    b.line("L-4", "SPK-B", "Three.");
    b.end();
    b.beginScene("SC-B", "B");
    b.line("L-5", "SPK-X", "Only an uncast one.");
    b.end();
    StoryDatabase db;
    std::string error;
    REQUIRE_MESSAGE(b.finish(db, &error), error);
    SpeakerCast cast;
    cast.entries = {{"SPK-A", "MDL-1"}, {"SPK-B", "MDL-2"}};

    std::array<std::string_view, kMaxSceneCast> out{};
    std::uint32_t dropped = 99;
    const std::uint64_t before = ftcore_test::allocStats().count;
    CHECK(sceneCastSpeakers(db, 0, cast, out, &dropped) == 2);
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(out[0] == "SPK-B");
    CHECK(out[1] == "SPK-A");
    CHECK(out[2].empty());
    CHECK(dropped == 0);
    CHECK(sceneCastSpeakers(db, 1, cast, out, &dropped) == 0);
    CHECK(out[0].empty());
    CHECK(sceneCastSpeakers(db, 99, cast, out) == 0);
}

TEST_CASE("scene cast: more than the cap are dropped and counted once each") {
    StoryBuilder b;
    b.setStart("SC-A");
    b.beginChapter("01", "One");
    b.beginScene("SC-A", "A");
    SpeakerCast cast;
    for (int i = 0; i < 10; ++i) {
        const std::string s = "SPK-" + std::to_string(i);
        cast.entries.push_back({s, "MDL-" + std::to_string(i)});
        b.line("L-" + std::to_string(i), s, "Line.");
    }
    b.line("L-x", "SPK-9", "Again.");
    b.end();
    StoryDatabase db;
    std::string error;
    REQUIRE_MESSAGE(b.finish(db, &error), error);
    std::array<std::string_view, kMaxSceneCast> out{};
    std::uint32_t dropped = 0;
    CHECK(sceneCastSpeakers(db, 0, cast, out, &dropped) == kMaxSceneCast);
    CHECK(dropped == 2);
    CHECK(out[7] == "SPK-7");
}

TEST_CASE("cast marks: import, find, and errors that never quote a label") {
    CastMarks marks;
    std::string error;
    REQUIRE_MESSAGE(importCastMarks(R"({"version":1,"scenes":[{"scene":"s1","marks":[
        {"speaker":"SPK-A","at":[1.5,-2],"yaw":30},{"speaker":"SPK-B","at":[0,1]}]}]})",
                                    marks, &error),
                    error);
    const CastMark* a = marks.find("s1", "SPK-A");
    REQUIRE(a);
    CHECK(a->x == doctest::Approx(1.5f));
    CHECK(a->y == doctest::Approx(-2.f));
    CHECK(a->yawDeg == doctest::Approx(30.f));
    REQUIRE(marks.find("s1", "SPK-B"));
    CHECK(marks.find("s1", "SPK-B")->yawDeg == 0.f);
    CHECK_FALSE(marks.find("s2", "SPK-A"));

    CHECK_FALSE(importCastMarks(R"({"version":2,"scenes":[]})", marks, &error));
    CHECK(error.find("version") != std::string::npos);
    CHECK(marks.entries.empty());
    CHECK_FALSE(importCastMarks(R"({"version":1,"scenes":[{"scene":"s1","marks":[
        {"speaker":"SPK-SECRET","at":[1]},{"speaker":"SPK-SECRET","at":[60,0]},{"speaker":"SPK-C","at":[0,0],"yaw":"x"}]}]})",
                                marks, &error));
    CHECK(error.find("SPK-SECRET") == std::string::npos);
    CHECK(error.find("marks[0].at") != std::string::npos);
    CHECK(error.find("marked twice") != std::string::npos);
    CHECK(error.find("50 m") != std::string::npos);
    CHECK(error.find("marks[2].yaw") != std::string::npos);
    CHECK_FALSE(importCastMarks("[", marks, &error));
}

TEST_CASE("cast marks: the default arc") {
    const CastMark one = castArcMark(0, 1);
    CHECK(one.x == 0.f);
    CHECK(one.y == 0.f);
    CHECK(one.yawDeg == 0.f);
    const CastMark l = castArcMark(0, 3), m = castArcMark(1, 3), r = castArcMark(2, 3);
    CHECK(m.y == doctest::Approx(0.f));
    CHECK(l.y == doctest::Approx(-1.1f));
    CHECK(r.y == doctest::Approx(1.1f));
    CHECK(l.x == doctest::Approx(r.x));
    CHECK(l.x < m.x);
    CHECK(l.yawDeg == doctest::Approx(-r.yawDeg));
    CHECK(castArcMark(5, 3).y == 0.f);
}

TEST_CASE("face curve map: the hand list maps mouthClose only where the asset has no pose") {
    FaceShapeSet set;
    REQUIRE(set.addShape("jawOpen", FaceLayer::MouthArticulation));
    REQUIRE(set.addShape("mouthClose", FaceLayer::MouthArticulation));
    REQUIRE(set.addShape("cheekPuff", FaceLayer::MouthTone));
    REQUIRE(set.finish());
    const std::array<std::string_view, 1> poses{"JawOpen"};
    const std::array<std::string_view, 3> curves{"CTRL_expressions_jawOpen", "CTRL_expressions_mouthLipsTogetherUL",
                                                 "CTRL_expressions_mouthLipsTogetherDR"};
    const std::array<float, 3> values{1.f, 0.f, 0.f};
    FaceCurveMap map;
    std::vector<std::string> unmatched;
    REQUIRE(buildFaceCurveMap(set, poses, curves, values, map, &unmatched, faceCurveHandMap()));
    CHECK(map.mappedShapes == 2);
    CHECK(map.handShapes == 1);
    CHECK(unmatched == std::vector<std::string>{"cheekPuff"}); // hand curves the asset lacks are skipped

    FaceWeights w{};
    w[0] = 0.6f; // jaw open on a vowel
    std::array<float, 3> out{};
    writeFaceCurves(map, w, out);
    CHECK(out[0] == doctest::Approx(0.6f));
    CHECK(out[1] == 0.f);
    w[1] = 0.4f; // closing for an m
    writeFaceCurves(map, w, out);
    CHECK(out[0] == doctest::Approx(0.2f)); // the jaw pulled toward shut
    CHECK(out[1] == doctest::Approx(0.4f)); // lips pressed together
    CHECK(out[2] == doctest::Approx(0.4f));

    // Without the hand list the shape stays unmatched.
    unmatched.clear();
    REQUIRE(buildFaceCurveMap(set, poses, curves, values, map, &unmatched));
    CHECK(map.handShapes == 0);
    CHECK(unmatched == std::vector<std::string>{"mouthClose", "cheekPuff"});
}
