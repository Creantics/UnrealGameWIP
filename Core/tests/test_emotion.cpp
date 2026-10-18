#include "ftcore/Emotion.h"
#include "ftcore/FaceImport.h"
#include "AllocCounter.h"

#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <string>
#include <utility>

using namespace ftcore;

namespace {

// browInnerUp (upper), eyeBlinkLeft (eyes), jawOpen (mouth_art), mouthSmileLeft (mouth_tone).
constexpr const char* kMini = R"J({
  "shapes": [ { "name": "browInnerUp", "layer": "upper" }, { "name": "eyeBlinkLeft", "layer": "eyes" },
              { "name": "jawOpen", "layer": "mouth_art" }, { "name": "mouthSmileLeft", "layer": "mouth_tone" } ],
  "visemes": [],
  "expressions": [ { "name": "neutral", "shapes": {} },
                   { "name": "warm", "shapes": { "mouthSmileLeft": 1.0, "browInnerUp": 0.2 } },
                   { "name": "sad", "shapes": { "browInnerUp": 1.0, "eyeBlinkLeft": 0.5, "jawOpen": 0.2 } } ]
})J";

constexpr std::size_t kBrow = 0, kBlink = 1, kJaw = 2, kSmile = 3;

FaceShapeSet miniSet() {
    FaceShapeSet set;
    std::string err;
    REQUIRE_MESSAGE(importFaceShapeSet(kMini, set, &err), err);
    return set;
}

EmotionMap miniMap(const FaceShapeSet& set) {
    EmotionMap map;
    std::string err;
    const FaceShapeWeight tender[] = {{"browInnerUp", 0.3f}};
    REQUIRE_MESSAGE(map.add(set, "warm", "warm", 1.f, {}, &err), err);
    REQUIRE_MESSAGE(map.add(set, "tender", "warm", 0.7f, tender, &err), err);
    REQUIRE_MESSAGE(map.add(set, "sad", "sad", 1.f, {}, &err), err);
    return map;
}

// The VO direction vocabulary (newengine tools/vo_direction/vo_common.py EMOTIONS).
constexpr const char* kVocabulary[] = {
    "warm", "tender", "affectionate", "reassuring", "hospitable", "playful", "teasing", "amused", "wry", "proud",
    "hopeful", "relieved", "eager", "curious", "excited", "earnest", "dry", "deadpan", "formal", "commanding",
    "determined", "confident", "guarded", "wary", "evasive", "solemn", "reverent", "nostalgic", "anxious", "afraid",
    "embarrassed", "ashamed", "hurt", "sad", "grieving", "longing", "resigned", "weary", "numb", "stunned",
    "confused", "pleading", "urgent", "frustrated", "irritated", "angry", "defiant", "contemptuous", "cold",
    "menacing", "neutral", "hollow", "uncanny"};

} // namespace

TEST_CASE("emotion intensity: 0.2 per step, clamped to 1..5, then capped") {
    CHECK(emotionIntensityWeight(1) == doctest::Approx(0.2f));
    CHECK(emotionIntensityWeight(3) == doctest::Approx(0.6f));
    CHECK(emotionIntensityWeight(5) == doctest::Approx(1.f));
    CHECK(emotionIntensityWeight(0) == doctest::Approx(0.2f));
    CHECK(emotionIntensityWeight(9) == doctest::Approx(1.f));
    CHECK(emotionIntensityWeight(5, 0.6f) == doctest::Approx(0.6f));
    CHECK(emotionIntensityWeight(2, 0.6f) == doctest::Approx(0.4f));
    CHECK(emotionIntensityWeight(4, -1.f) == doctest::Approx(0.f));
}

TEST_CASE("emotion word = weight * base + modifiers, scaled by intensity") {
    const FaceShapeSet set = miniSet();
    const EmotionMap map = miniMap(set);
    FacePreset out;
    REQUIRE(resolveEmotion(set, map, "tender", "", 5, 1.f, out));
    CHECK(out.weights[kSmile] == doctest::Approx(0.7f));
    CHECK(out.weights[kBrow] == doctest::Approx(0.7f * 0.2f + 0.3f));
    CHECK(out.weights[kJaw] == doctest::Approx(0.f));

    REQUIRE(resolveEmotion(set, map, "tender", "", 3, 1.f, out));
    CHECK(out.weights[kSmile] == doctest::Approx(0.6f * 0.7f));
    CHECK(out.weights[kBrow] == doctest::Approx(0.6f * 0.44f));

    // The containment cap limits the primary.
    REQUIRE(resolveEmotion(set, map, "warm", "", 5, 0.5f, out));
    CHECK(out.weights[kSmile] == doctest::Approx(0.5f));
}

TEST_CASE("the secondary emotion shows only in the upper face and eyes, at a fixed 0.35") {
    const FaceShapeSet set = miniSet();
    const EmotionMap map = miniMap(set);
    FacePreset out;
    REQUIRE(resolveEmotion(set, map, "", "sad", 1, 1.f, out));
    CHECK(out.weights[kBrow] == doctest::Approx(0.35f));
    CHECK(out.weights[kBlink] == doctest::Approx(0.35f * 0.5f));
    CHECK(out.weights[kJaw] == doctest::Approx(0.f)); // mouth_art: not touched

    // warm at 5 under sad: the brows add up and clamp at 1, the mouth is warm's alone.
    REQUIRE(resolveEmotion(set, map, "warm", "sad", 5, 1.f, out));
    CHECK(out.weights[kSmile] == doctest::Approx(1.f));
    CHECK(out.weights[kBrow] == doctest::Approx(0.2f + 0.35f));
    CHECK(out.weights[kJaw] == doctest::Approx(0.f));

    // sad under sad: 1 + 0.35 on the brow clamps to 1; the jaw is the primary's alone.
    REQUIRE(resolveEmotion(set, map, "sad", "sad", 5, 1.f, out));
    CHECK(out.weights[kBrow] == doctest::Approx(1.f));
    CHECK(out.weights[kJaw] == doctest::Approx(0.2f));
}

TEST_CASE("unknown emotion words are reported and left out") {
    const FaceShapeSet set = miniSet();
    const EmotionMap map = miniMap(set);
    FacePreset out;
    CHECK_FALSE(resolveEmotion(set, map, "smug", "", 5, 1.f, out));
    CHECK(out.weights[kSmile] == doctest::Approx(0.f));
    CHECK_FALSE(resolveEmotion(set, map, "warm", "smug", 5, 1.f, out));
    CHECK(out.weights[kSmile] == doctest::Approx(1.f));
    CHECK(resolveEmotion(set, map, "", "", 5, 1.f, out));
    CHECK(out.weights[kSmile] == doctest::Approx(0.f));
}

TEST_CASE("emotion map rejects unknown bases and shapes and duplicates") {
    const FaceShapeSet set = miniSet();
    EmotionMap map;
    std::string err;
    CHECK_FALSE(map.add(set, "smug", "pleased", 1.f, {}, &err));
    CHECK(err.find("'pleased'") != std::string::npos);
    const FaceShapeWeight bad[] = {{"noseSneerLeft", 0.3f}};
    CHECK_FALSE(map.add(set, "smug", "warm", 1.f, bad, &err));
    CHECK(err.find("'noseSneerLeft'") != std::string::npos);
    CHECK_FALSE(map.add(set, "", "warm", 1.f, {}, &err));
    CHECK(map.add(set, "warm", "warm", 1.f, {}, &err));
    CHECK_FALSE(map.add(set, "warm", "warm", 1.f, {}, &err));
    CHECK(err.find("twice") != std::string::npos);
    CHECK(map.emotions.size() == 1);
}

TEST_CASE("emotion map import reads entries and rejects bad ones") {
    const FaceShapeSet set = miniSet();
    EmotionMap map;
    std::string err;
    REQUIRE_MESSAGE(importEmotionMap(R"({"emotions": [{"name": "warm", "base": "warm"},
        {"name": "tender", "base": "warm", "weight": 0.7, "add": {"browInnerUp": 0.3}}]})", set, map, &err), err);
    REQUIRE(map.find("tender"));
    CHECK(map.find("tender")->weight == doctest::Approx(0.7f));
    CHECK(map.find("tender")->modifiers[kBrow] == doctest::Approx(0.3f));
    CHECK(map.find("warm")->weight == doctest::Approx(1.f));

    const std::pair<const char*, const char*> badCases[] = {
        {"{}", "emotions"},
        {"{\"emotions\": [", "line 1"},
        {R"({"emotions": [{"name": "a", "base": "pleased"}]})", "'pleased'"},
        {R"({"emotions": [{"name": "a", "base": "warm", "weight": "1"}]})", "weight is not a number"},
        {R"({"emotions": [{"name": "a", "base": "warm", "add": [1]}]})", "add must be an object"},
        {R"({"emotions": [{"name": "a", "base": "warm", "add": {"jawOpen": "x"}}]})", "'jawOpen'"},
        {R"({"emotions": [{"name": "a", "base": "warm", "add": {"tongueOut": 1}}]})", "'tongueOut'"},
    };
    for (const auto& [text, want] : badCases) {
        CAPTURE(text);
        CHECK_FALSE(importEmotionMap(text, set, map, &err));
        CHECK(err.find("emotion_map.json: ") == 0);
        CHECK(err.find(want) != std::string::npos);
        CHECK(map.emotions.empty());
    }
}

TEST_CASE("resolving an emotion does not allocate") {
    const FaceShapeSet set = miniSet();
    const EmotionMap map = miniMap(set);
    FacePreset out;
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("allocation hook not installed; skipping");
        return;
    }
    const auto before = ftcore_test::allocStats().count;
    for (int i = 1; i <= 5; ++i) resolveEmotion(set, map, "tender", "sad", i, 0.8f, out);
    CHECK(ftcore_test::allocStats().count == before);
}

// ---- the project's Data/Rig/emotion_map.json against Data/Rig/face_presets.json ----------------

TEST_CASE("the draft emotion map covers the whole VO vocabulary and resolves on the real face") {
    std::string presets, mapText;
    if (!ftcore_test::readProjectData("Rig/face_presets.json", presets) ||
        !ftcore_test::readProjectData("Rig/emotion_map.json", mapText)) {
        MESSAGE("Data/Rig not present; skipping");
        return;
    }
    FaceShapeSet set;
    std::string err;
    REQUIRE_MESSAGE(importFaceShapeSet(presets, set, &err), err);
    EmotionMap map;
    REQUIRE_MESSAGE(importEmotionMap(mapText, set, map, &err), err);
    CHECK(map.emotions.size() == std::size(kVocabulary));
    FacePreset out;
    for (const char* word : kVocabulary) {
        CAPTURE(word);
        CHECK(resolveEmotion(set, map, word, "sad", 5, 1.f, out));
        for (const float w : out.weights) {
            CHECK(w >= 0.f);
            CHECK(w <= 1.f);
        }
    }
    // The documented examples (rigging_plan §7.1, face_spec EXPRESSION_VARIANTS).
    const int smileL = set.find("mouthSmileLeft"), brow = set.find("browInnerUp"), noCheek = set.find("mouthSmileNoCheek");
    REQUIRE(smileL >= 0);
    REQUIRE(brow >= 0);
    REQUIRE(noCheek >= 0);
    REQUIRE(resolveEmotion(set, map, "tender", "", 5, 1.f, out));
    CHECK(out.weights[static_cast<std::size_t>(smileL)] == doctest::Approx(0.7f));
    CHECK(out.weights[static_cast<std::size_t>(brow)] == doctest::Approx(0.7f * 0.2f + 0.3f));
    REQUIRE(resolveEmotion(set, map, "uncanny", "", 5, 1.f, out));
    CHECK(out.weights[static_cast<std::size_t>(noCheek)] == doctest::Approx(0.45f));
    FacePreset cold;
    REQUIRE(resolveEmotion(set, map, "cold", "", 5, 1.f, cold));
    REQUIRE(resolveEmotion(set, map, "hollow", "", 5, 1.f, out));
    CHECK(out.weights == cold.weights);
}
