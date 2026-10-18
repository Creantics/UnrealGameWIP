#include "ftcore/FaceImport.h"
#include "ftcore/Json.h"

#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <utility>

using namespace ftcore;

namespace {

constexpr const char* kMini = R"J({
  "jaw_max_deg": 20.0,
  "masks": [ { "name": "ignored" } ],
  "shapes": [ { "name": "jawOpen", "layer": "mouth_art" }, { "name": "mouthClose", "layer": "mouth_art" },
              { "name": "mouthSmileLeft", "layer": "mouth_tone" }, { "name": "mouthSmileRight", "layer": "mouth_tone" },
              { "name": "jawOpen_x_mouthSmile", "layer": "corrective" } ],
  "visemes": [ { "name": "PP", "shapes": { "mouthClose": 1.0 }, "tongue_deg": [1, -2.5, 3] },
               { "name": "aa", "shapes": { "jawOpen": 0.6 } } ],
  "expressions": [ { "name": "neutral", "shapes": {} }, { "name": "warm", "shapes": { "mouthSmileLeft": 1, "mouthSmileRight": 0.5 } } ]
})J";

} // namespace

TEST_CASE("face import reads shapes, presets, tongue and jaw angle") {
    FaceShapeSet set;
    std::string err;
    REQUIRE_MESSAGE(importFaceShapeSet(kMini, set, &err), err);
    CHECK(set.size() == 5);
    CHECK(set.layers[2] == FaceLayer::MouthTone);
    CHECK(set.jawMaxDeg == doctest::Approx(20.f));
    CHECK(set.jawOpen == 0);
    CHECK(set.mouthClose == 1);
    REQUIRE(set.correctives.size() == 1);
    CHECK(set.correctives[0].driverB == 2);
    CHECK(set.correctives[0].driverB2 == 3);
    REQUIRE(set.viseme("PP"));
    CHECK(set.viseme("PP")->weights[1] == doctest::Approx(1.f));
    CHECK(set.viseme("PP")->tongueDeg == FaceVec3{1.f, -2.5f, 3.f});
    CHECK(set.viseme("aa")->tongueDeg == FaceVec3{});
    REQUIRE(set.expression("warm"));
    CHECK(set.expression("warm")->weights[3] == doctest::Approx(0.5f));
    CHECK(set.expressions.size() == 2);
}

TEST_CASE("face import rejects bad presets") {
    const std::pair<const char*, const char*> bad[] = {
        {"{}", "shapes"},
        {"{\"shapes\": [", "line 1"},
        {R"({"shapes": [{"name": "a", "layer": "lips"}], "visemes": [], "expressions": []})", "unknown layer"},
        {R"({"shapes": [{"name": "a", "layer": "upper"}], "visemes": [{"name": "X", "shapes": {"b": 1}}], "expressions": []})",
         "'b'"},
        {R"({"shapes": [{"name": "a", "layer": "upper"}], "visemes": [{"name": "X", "shapes": {"a": "1"}}], "expressions": []})",
         "not a number"},
        {R"({"shapes": [{"name": "a", "layer": "upper"}], "visemes": [{"name": "X", "shapes": {}, "tongue_deg": [1, 2]}], "expressions": []})",
         "tongue_deg"},
        {R"({"shapes": [{"name": "a_x_b", "layer": "corrective"}], "visemes": [], "expressions": []})", "unknown drivers"},
    };
    for (const auto& [text, want] : bad) {
        CAPTURE(text);
        FaceShapeSet set;
        std::string err;
        CHECK_FALSE(importFaceShapeSet(text, set, &err));
        CHECK(err.find("face_presets.json: ") == 0);
        CHECK(err.find(want) != std::string::npos);
        CHECK(set.size() == 0);
    }
}

// ---- newengine assets/rig/face_presets.json and the face_spec.py golden data -------------------

TEST_CASE("face presets load: 56 shapes, 15 visemes, 14 expressions, one corrective") {
    std::string text;
    if (!ftcore_test::readAsset("rig/face_presets.json", text)) {
        MESSAGE("newengine assets/rig/face_presets.json not present; skipping");
        return;
    }
    FaceShapeSet set;
    std::string err;
    REQUIRE_MESSAGE(importFaceShapeSet(text, set, &err), err);
    CHECK(set.size() == 56);
    CHECK(set.visemes.size() == 15);
    CHECK(set.expressions.size() == 14);
    REQUIRE(set.correctives.size() == 1);
    CHECK(set.shapes[static_cast<std::size_t>(set.correctives[0].shape)] == "jawOpen_x_mouthSmile");
    CHECK(set.jawOpen >= 0);
    CHECK(set.blinkLeft >= 0);
    CHECK(set.blinkRight >= 0);
    CHECK(set.jawMaxDeg == doctest::Approx(25.f));
    REQUIRE(set.viseme("PP") != nullptr);
    CHECK(set.viseme("PP")->weights[static_cast<std::size_t>(set.mouthClose)] == doctest::Approx(1.f));
    CHECK(set.expression("warm") != nullptr);
    CHECK(set.expression("nope") == nullptr);
}

TEST_CASE("composeFace matches face_spec.layer_weights on the proof-head layer cases") {
    std::string presets, expected;
    if (!ftcore_test::readAsset("rig/face_presets.json", presets) ||
        !ftcore_test::readAsset("models/rig/proof_head.expected.json", expected)) {
        MESSAGE("newengine face presets or proof_head.expected.json not present; skipping");
        return;
    }
    FaceShapeSet set;
    std::string err;
    REQUIRE_MESSAGE(importFaceShapeSet(presets, set, &err), err);
    JsonDocument golden;
    REQUIRE_MESSAGE(golden.parse(expected, &err), err);
    const JsonValue cases = golden.root()["cases"];
    REQUIRE(cases.isArray());

    const std::pair<const char*, const char*> layered[] = {{"warm", "aa"}, {"warm", "PP"}, {"angry", "ou"}, {"sad", "E"}};
    for (const auto& [e, v] : layered) {
        const std::string name = std::string("layer_") + e + "_" + v;
        CAPTURE(name);
        REQUIRE(set.expression(e));
        REQUIRE(set.viseme(v));
        FaceWeights out{}, micro{};
        composeFace(set, set.expression(e)->weights, set.viseme(v)->weights, 1.f, micro, out);
        JsonValue c;
        for (std::uint32_t i = 0; i < cases.size(); ++i)
            if (cases[i]["name"].string() == name) c = cases[i];
        REQUIRE(c.isObject());
        const JsonValue w = c["weights"];
        REQUIRE(w.isObject());
        // Every golden shape is one of ours.
        for (std::uint32_t i = 0; i < w.size(); ++i) CHECK_MESSAGE(set.find(w.key(i)) >= 0, w.key(i));
        for (std::size_t s = 0; s < set.size(); ++s) {
            const float want = static_cast<float>(w[set.shapes[s]].number(0.0));
            CHECK_MESSAGE(out[s] == doctest::Approx(want).epsilon(1e-5), name << " " << set.shapes[s]);
        }
    }
}

TEST_CASE("face player drives the real shape set: visemes, jaw, tongue, crossfade") {
    std::string text;
    if (!ftcore_test::readAsset("rig/face_presets.json", text)) {
        MESSAGE("newengine assets/rig/face_presets.json not present; skipping");
        return;
    }
    FaceShapeSet set;
    std::string err;
    REQUIRE_MESSAGE(importFaceShapeSet(text, set, &err), err);
    FaceTrack t;
    t.rate = 60.f;
    t.frames = 61; // one second
    t.channels = {"jawOpen", "mouthSmileLeft", "@speech", "@tongue_01"};
    for (std::uint32_t f = 0; f < t.frames; ++f) {
        const auto jaw = static_cast<std::uint8_t>(f < 30 ? 255 : 0);
        t.data.insert(t.data.end(), {jaw, 0, 255, 255});
    }
    FacePlayer p(set, 1234u);
    p.setBlinking(false);
    p.setExpression(set.expression("warm"), 5.f, 0.f);
    REQUIRE(p.play(t));
    for (int i = 0; i < 12; ++i) p.update(1.f / 60.f); // speech ramps up over 120 ms
    CHECK(p.speech() == doctest::Approx(1.f));
    const auto js = static_cast<std::size_t>(set.jawOpen);
    CHECK(p.weights()[js] == doctest::Approx(1.f));
    CHECK(p.jawDeg() == doctest::Approx(25.f));
    CHECK(p.tongueDeg().x == doctest::Approx(40.f));
    // No rounding in the viseme, so the warm smile shows and the corrective follows jawOpen * smile.
    const auto smile = static_cast<std::size_t>(set.find("mouthSmileLeft"));
    CHECK(p.weights()[smile] == doctest::Approx(set.expression("warm")->weights[smile]));
    const auto corr = static_cast<std::size_t>(set.correctives[0].shape);
    CHECK(p.weights()[corr] == doctest::Approx(p.weights()[smile]));

    // The track ends -> speech falls, the expression's mouth comes back.
    for (int i = 0; i < 90; ++i) p.update(1.f / 60.f);
    CHECK_FALSE(p.playing());
    CHECK(p.speech() == doctest::Approx(0.f));
    CHECK(p.weights()[js] == doctest::Approx(0.f)); // warm has no jawOpen

    // Halfway through a 0.5 s fade the smile is between the two.
    p.setExpression(nullptr, 5.f, 0.5f);
    p.update(0.25f);
    CHECK(p.weights()[smile] == doctest::Approx(0.5f * set.expression("warm")->weights[smile]));
    p.update(0.5f);
    CHECK(p.weights()[smile] == doctest::Approx(0.f));

    // Blinks: same seed + same dt sequence -> same weights; a blink happens within 6 s.
    FacePlayer a(set, 99u), b(set, 99u);
    const auto bl = static_cast<std::size_t>(set.blinkLeft);
    float peak = 0.f;
    bool same = true;
    for (int i = 0; i < 6 * 60; ++i) {
        a.update(1.f / 60.f);
        b.update(1.f / 60.f);
        same = same && a.weights()[bl] == b.weights()[bl];
        peak = std::max(peak, a.weights()[bl]);
    }
    CHECK(same);
    CHECK(peak > 0.9f);
}
