#include "ftcore/FaceDirection.h"
#include "ftcore/FaceImport.h"
#include "AllocCounter.h"

#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 60.f;

// One shape per layer the direction treats differently, plus the brow shapes a flick drives and
// the shapes the delivery (contour, volume) drives.
struct MiniRig {
    FaceShapeSet set;
    EmotionMap map;
    int browIn = -1, browOutL = -1, browOutR = -1, cheek = -1, blinkL = -1, jaw = -1, pressL = -1, smileL = -1;
    int browDownL = -1, pressR = -1, stretchL = -1;

    MiniRig() {
        set.addShape("browInnerUp", FaceLayer::Upper);
        set.addShape("browOuterUpLeft", FaceLayer::Upper);
        set.addShape("browOuterUpRight", FaceLayer::Upper);
        set.addShape("cheekSquintLeft", FaceLayer::Upper);
        set.addShape("eyeBlinkLeft", FaceLayer::Eyes);
        set.addShape("eyeBlinkRight", FaceLayer::Eyes);
        set.addShape("jawOpen", FaceLayer::MouthArticulation);
        set.addShape("mouthPressLeft", FaceLayer::MouthArticulation);
        set.addShape("mouthSmileLeft", FaceLayer::MouthTone);
        set.addShape("browDownLeft", FaceLayer::Upper);
        set.addShape("browDownRight", FaceLayer::Upper);
        set.addShape("mouthPressRight", FaceLayer::MouthArticulation);
        set.addShape("mouthStretchLeft", FaceLayer::MouthTone);
        set.addShape("mouthStretchRight", FaceLayer::MouthTone);
        const std::array<FaceShapeWeight, 2> warm{{{"mouthSmileLeft", 1.f}, {"cheekSquintLeft", 0.5f}}};
        const std::array<FaceShapeWeight, 2> sad{{{"browInnerUp", 1.f}, {"jawOpen", 0.2f}}};
        const std::array<FaceShapeWeight, 2> contempt{{{"mouthSmileLeft", 0.6f}, {"browOuterUpLeft", 0.4f}}};
        REQUIRE(set.addExpression("neutral", {}));
        REQUIRE(set.addExpression("warm", warm));
        REQUIRE(set.addExpression("sad", sad));
        REQUIRE(set.addExpression("contempt", contempt));
        REQUIRE(set.finish());
        REQUIRE(map.add(set, "warm", "warm", 1.f));
        REQUIRE(map.add(set, "sad", "sad", 1.f));
        REQUIRE(map.add(set, "contemptuous", "contempt", 1.f));
        map.emotions.back().mirrors = false;
        browIn = set.find("browInnerUp");
        browOutL = set.find("browOuterUpLeft");
        browOutR = set.find("browOuterUpRight");
        cheek = set.find("cheekSquintLeft");
        blinkL = set.find("eyeBlinkLeft");
        jaw = set.find("jawOpen");
        pressL = set.find("mouthPressLeft");
        smileL = set.find("mouthSmileLeft");
        browDownL = set.find("browDownLeft");
        pressR = set.find("mouthPressRight");
        stretchL = set.find("mouthStretchLeft");
    }
};

float at(const FaceWeights& w, int i) { return w[static_cast<std::size_t>(i)]; }

ContainmentProfile devinLike(const FaceShapeSet& set) {
    ContainmentProfile p;
    p.name = "devin";
    p.cap = 0.7f;
    p.added[static_cast<std::size_t>(set.find("mouthPressLeft"))] = 0.3f;
    return p;
}

void run(FaceDirector& dir, FacePlayer& player, float seconds) {
    for (float t = 0.f; t < seconds; t += kDt) {
        dir.tick(kDt);
        player.update(kDt);
    }
}

} // namespace

TEST_CASE("containment: cap, holding-back shapes and the excess") {
    MiniRig r;
    const ContainmentProfile devin = devinLike(r.set);
    FacePreset out;
    float excess = -1.f;

    REQUIRE(resolveContainedEmotion(r.set, r.map, nullptr, "warm", "", 5, out, &excess));
    CHECK(at(out.weights, r.smileL) == doctest::Approx(1.f));
    CHECK(excess == doctest::Approx(0.f));

    REQUIRE(resolveContainedEmotion(r.set, r.map, &devin, "warm", "", 5, out, &excess));
    CHECK(at(out.weights, r.smileL) == doctest::Approx(0.7f));
    CHECK(at(out.weights, r.pressL) == doctest::Approx(0.3f)); // 0.3 * felt 1.0
    CHECK(excess == doctest::Approx(0.3f));

    REQUIRE(resolveContainedEmotion(r.set, r.map, &devin, "warm", "", 2, out, &excess));
    CHECK(at(out.weights, r.smileL) == doctest::Approx(0.4f));
    CHECK(at(out.weights, r.pressL) == doctest::Approx(0.12f));
    CHECK(excess == doctest::Approx(0.f));

    ContainmentProfile minor;
    minor.maxIntensity = 4;
    REQUIRE(resolveContainedEmotion(r.set, r.map, &minor, "warm", "", 5, out, &excess));
    CHECK(at(out.weights, r.smileL) == doctest::Approx(0.8f));
    CHECK(excess == doctest::Approx(0.f));

    CHECK_FALSE(resolveContainedEmotion(r.set, r.map, &devin, "nope", "", 3, out, &excess));
}

TEST_CASE("containment profiles import, and bad entries are refused") {
    MiniRig r;
    ContainmentProfiles profiles;
    std::string err;
    constexpr const char* kGood = R"J({"profiles": [
        {"name": "a", "cap": 0.6, "blink_rate": 0.5, "add": {"mouthPressLeft": 0.25}},
        {"name": "b", "minor": true, "head_motion": 0, "micro_per_excess": 3}]})J";
    REQUIRE_MESSAGE(importContainmentProfiles(kGood, r.set, profiles, &err), err);
    REQUIRE(profiles.profiles.size() == 2);
    const ContainmentProfile* a = profiles.find("a");
    REQUIRE(a);
    CHECK(a->cap == doctest::Approx(0.6f));
    CHECK(a->blinkRate == doctest::Approx(0.5f));
    CHECK(at(a->added, r.pressL) == doctest::Approx(0.25f));
    CHECK(a->maxIntensity == 5);
    const ContainmentProfile* b = profiles.find("b");
    REQUIRE(b);
    CHECK(b->maxIntensity == 4);
    CHECK(b->headMotion == doctest::Approx(0.f));
    CHECK(b->microPerExcess == doctest::Approx(3.f));
    CHECK(b->cap == doctest::Approx(1.f));

    for (const char* bad : {R"({"profiles": [{"name": "a"}, {"name": "a"}]})", R"({"profiles": [{"cap": 1}]})",
                            R"({"profiles": [{"name": "a", "cap": -1}]})", R"({"profiles": [{"name": "a", "minor": 1}]})",
                            R"({"profiles": [{"name": "a", "add": {"nope": 1}}]})", R"({"profiles": {}})", "{"}) {
        CAPTURE(bad);
        CHECK_FALSE(importContainmentProfiles(bad, r.set, profiles, &err));
        CHECK(profiles.profiles.empty());
    }
}

TEST_CASE("micro-expression rate: secondary sets the base, excess raises it") {
    MiniRig r;
    FacePlayer player(r.set);
    FaceDirector dir(r.set, r.map, player);
    const ContainmentProfile devin = devinLike(r.set);
    const FaceDirectionTuning tuning;

    dir.speak("warm", "", 5);
    CHECK(dir.microRate() == doctest::Approx(0.f)); // nothing held back, nothing leaking
    dir.speak("warm", "sad", 5);
    CHECK(dir.microRate() == doctest::Approx(tuning.microBaseRate));
    dir.setProfile(&devin);
    dir.speak("warm", "", 5);
    CHECK(dir.microRate() == doctest::Approx(devin.microPerExcess * 0.3f)); // the held-back warmth leaks
    dir.speak("warm", "sad", 5);
    CHECK(dir.microRate() == doctest::Approx(tuning.microBaseRate + devin.microPerExcess * 0.3f));
    dir.speak("warm", "sad", 2);
    CHECK(dir.microRate() == doctest::Approx(tuning.microBaseRate));
}

TEST_CASE("micro-expression flashes come at the rate and only in the upper face") {
    MiniRig r;
    FacePlayer player(r.set);
    FaceDirector dir(r.set, r.map, player, 1234u);
    const ContainmentProfile devin = devinLike(r.set);
    dir.setProfile(&devin);
    dir.speak("warm", "sad", 5);
    const float rate = dir.microRate();
    REQUIRE(rate > 0.f);

    int flashes = 0;
    bool was = false;
    float peak = 0.f;
    const float seconds = 300.f;
    for (float t = 0.f; t < seconds; t += kDt) {
        dir.tick(kDt);
        player.update(kDt);
        const FaceWeights& m = dir.micro();
        CHECK(at(m, r.jaw) == 0.f);
        CHECK(at(m, r.pressL) == 0.f);
        CHECK(at(m, r.smileL) == 0.f);
        const bool on = at(m, r.browIn) > 0.f; // sad's brow is the flashed shape
        if (on && !was) ++flashes;
        was = on;
        peak = std::max(peak, at(m, r.browIn));
    }
    CHECK(peak == doctest::Approx(FaceDirectionTuning{}.microWeight));
    const float expected = rate * seconds;
    CHECK(static_cast<float>(flashes) > expected * 0.8f);
    CHECK(static_cast<float>(flashes) < expected * 1.2f);
}

TEST_CASE("emphasis: a 120 ms brow flick and a head nod") {
    MiniRig r;
    FacePlayer player(r.set);
    FaceDirector dir(r.set, r.map, player);
    const FaceCue cues[] = {{0.5f, 0.3f, FaceCueKind::Emphasis}};
    dir.speak("", "", 3, cues);

    float peak = 0.f, peakAt = -1.f, lastOn = -1.f, nodPeak = 0.f;
    for (int f = 1; f <= 90; ++f) {
        dir.tick(kDt);
        player.update(kDt);
        const float t = dir.lineTime();
        const float w = at(dir.micro(), r.browIn);
        CHECK(at(dir.micro(), r.browOutL) == doctest::Approx(w));
        CHECK(at(dir.micro(), r.browOutR) == doctest::Approx(w));
        CHECK(at(dir.micro(), r.cheek) == 0.f);
        CHECK(at(dir.micro(), r.blinkL) == 0.f);
        if (t < 0.5f - 1e-4f) CHECK(w == 0.f);
        if (w > peak) {
            peak = w;
            peakAt = t;
        }
        if (w > 0.f) lastOn = t;
        nodPeak = std::max(nodPeak, dir.headNod());
    }
    CHECK(peak == doctest::Approx(0.3f).epsilon(0.05));
    CHECK(peakAt == doctest::Approx(0.56f).epsilon(0.05));
    CHECK(lastOn < 0.5f + 0.12f + 1e-3f);
    CHECK(nodPeak > 0.9f);
    CHECK(dir.headNod() == 0.f);
    // The brow flick reaches the face through the directed micro layer.
    dir.speak("", "", 3, cues);
    run(dir, player, 0.56f);
    CHECK(at(player.weights(), r.browIn) > 0.25f);

    // A still head (Odette's profile) shows no nod.
    ContainmentProfile still;
    still.headMotion = 0.f;
    dir.setProfile(&still);
    dir.speak("", "", 3, cues);
    for (int f = 0; f < 60; ++f) {
        dir.tick(kDt);
        CHECK(dir.headNod() == 0.f);
    }
}

TEST_CASE("no blink lands on an emphasis word") {
    MiniRig r;
    std::array<FaceCue, 40> cues{};
    for (std::size_t i = 0; i < cues.size(); ++i) cues[i] = {0.4f + 0.7f * static_cast<float>(i), 0.3f, FaceCueKind::Emphasis};
    int blinksSeen = 0;
    for (std::uint32_t seed = 1; seed <= 60; ++seed) {
        FacePlayer player(r.set, seed);
        FaceDirector dir(r.set, r.map, player, seed);
        dir.speak("", "", 3, cues);
        bool was = false;
        for (int f = 0; f < 60 * 28; ++f) {
            dir.tick(kDt);
            player.update(kDt);
            const float t = dir.lineTime();
            const float blink = at(player.weights(), r.blinkL);
            if (blink > 0.f && !was) ++blinksSeen;
            was = blink > 0.f;
            for (const FaceCue& c : cues)
                if (t >= c.time && t <= c.time + c.duration) {
                    CAPTURE(seed);
                    CAPTURE(t);
                    CHECK(blink == 0.f);
                }
        }
    }
    CHECK(blinksSeen > 60 * 3); // the blinks moved into the gaps; they did not stop
}

TEST_CASE("pause blink slots: // forces a blink at least half the time, / less often") {
    MiniRig r;
    int longBlinks = 0, shortBlinks = 0;
    const int trials = 400;
    for (int k = 0; k < trials; ++k) {
        const auto seed = static_cast<std::uint32_t>(k + 1);
        for (const FaceCueKind kind : {FaceCueKind::LongPause, FaceCueKind::Pause}) {
            FacePlayer player(r.set, seed);
            FaceDirector dir(r.set, r.map, player, seed * 7u);
            const FaceCue cues[] = {{0.1f, 0.f, kind}};
            dir.speak("", "", 3, cues);
            bool blinked = false;
            for (int f = 0; f < 12; ++f) { // blinks never come on their own before 2 s
                dir.tick(kDt);
                player.update(kDt);
                blinked = blinked || player.blinkActive();
            }
            (kind == FaceCueKind::LongPause ? longBlinks : shortBlinks) += blinked ? 1 : 0;
        }
    }
    const float longRate = static_cast<float>(longBlinks) / trials, shortRate = static_cast<float>(shortBlinks) / trials;
    CHECK(longRate >= 0.5f);
    CHECK(longRate == doctest::Approx(FaceDirectionTuning{}.longPauseBlinkChance).epsilon(0.15));
    CHECK(shortRate == doctest::Approx(FaceDirectionTuning{}.shortPauseBlinkChance).epsilon(0.25));

    // A `/` pulls forward a blink that is due soon anyway.
    for (std::uint32_t seed = 1; seed <= 50; ++seed) {
        FacePlayer player(r.set, seed);
        FaceDirector dir(r.set, r.map, player, seed);
        const float due = player.timeToNextBlink();
        const FaceCue cues[] = {{due - 0.5f, 0.f, FaceCueKind::Pause}};
        dir.speak("", "", 3, cues);
        run(dir, player, due - 0.5f);
        dir.tick(kDt);
        player.update(kDt);
        CHECK(player.blinkActive());
    }

    // Inside an emphasis word's guard, the pause blink is refused.
    for (std::uint32_t seed = 1; seed <= 50; ++seed) {
        FacePlayer player(r.set, seed);
        FaceDirector dir(r.set, r.map, player, seed);
        const FaceCue cues[] = {{0.1f, 0.f, FaceCueKind::LongPause}, {0.2f, 0.2f, FaceCueKind::Emphasis}};
        dir.speak("", "", 3, cues);
        for (int f = 0; f < 30; ++f) {
            dir.tick(kDt);
            player.update(kDt);
            CHECK_FALSE(player.blinkActive());
        }
    }
}

TEST_CASE("a profile's blink rate scales the blinks") {
    MiniRig r;
    const auto countBlinks = [&](float rate) {
        FacePlayer player(r.set, 99u);
        FaceDirector dir(r.set, r.map, player, 99u);
        ContainmentProfile p;
        p.blinkRate = rate;
        dir.setProfile(&p);
        int n = 0;
        bool was = false;
        for (int f = 0; f < 60 * 900; ++f) {
            dir.tick(kDt);
            player.update(kDt);
            if (player.blinkActive() && !was) ++n;
            was = player.blinkActive();
        }
        return n;
    };
    const int normal = countBlinks(1.f), duy = countBlinks(0.5f);
    CHECK(normal > 100);
    CHECK(static_cast<float>(duy) / static_cast<float>(normal) == doctest::Approx(0.5f).epsilon(0.15));
}

TEST_CASE("listening: the last emotion halves every 4 s, under the mirrored speaker") {
    MiniRig r;
    FacePlayer player(r.set);
    FaceDirector dir(r.set, r.map, player);
    dir.speak("sad", "", 5, {}, 0.f);
    CHECK(at(dir.pose().weights, r.browIn) == doctest::Approx(1.f));

    dir.listen("", 3);
    CHECK(dir.listening());
    run(dir, player, 4.f);
    CHECK(at(dir.pose().weights, r.browIn) == doctest::Approx(0.5f).epsilon(0.02));
    run(dir, player, 4.f);
    CHECK(at(dir.pose().weights, r.browIn) == doctest::Approx(0.25f).epsilon(0.02));
    CHECK(at(player.weights(), r.browIn) == doctest::Approx(0.25f).epsilon(0.02)); // through setExpression

    // Warmth mirrors at 0.3x the speaker's weight (intensity 4 -> 0.8).
    dir.listen("warm", 4);
    float prev = at(dir.pose().weights, r.smileL);
    float maxStep = 0.f;
    for (int f = 0; f < 60 * 30; ++f) {
        dir.tick(kDt);
        const float w = at(dir.pose().weights, r.smileL);
        maxStep = std::max(maxStep, std::abs(w - prev));
        prev = w;
    }
    CHECK(prev == doctest::Approx(0.3f * 0.8f).epsilon(0.01));
    CHECK(at(dir.pose().weights, r.browIn) < 0.002f);
    CHECK(maxStep < 0.02f); // eased in, no pop

    // Contempt does not mirror: the warm mirror eases out to nothing.
    dir.listen("contemptuous", 5);
    run(dir, player, 2.f);
    CHECK(at(dir.pose().weights, r.smileL) == doctest::Approx(0.f));
    CHECK(at(dir.pose().weights, r.browOutL) == doctest::Approx(0.f));

    // Speaking again leaves the listening state.
    dir.speak("warm", "", 3);
    CHECK_FALSE(dir.listening());
}

TEST_CASE("listening: glance-aways per phrase, nods on // when warm") {
    MiniRig r;
    FacePlayer player(r.set);
    FaceDirector dir(r.set, r.map, player, 77u);
    CHECK_FALSE(dir.speakerPhrase()); // not listening yet

    dir.speak("sad", "", 3);
    dir.listen("sad", 3);
    int glances = 0;
    for (int i = 0; i < 2000; ++i) glances += dir.speakerPhrase() ? 1 : 0;
    CHECK(static_cast<float>(glances) / 2000.f == doctest::Approx(0.2f).epsilon(0.15));

    dir.speakerPause(FaceCueKind::LongPause); // sad, not warm: no nod
    dir.tick(0.1f);
    CHECK(dir.headNod() == 0.f);

    dir.listen("warm", 3); // mirroring warmth makes the listener warm
    dir.speakerPause(FaceCueKind::Pause);
    dir.tick(0.1f);
    CHECK(dir.headNod() == 0.f);
    dir.speakerPause(FaceCueKind::LongPause);
    dir.tick(0.1f);
    CHECK(dir.headNod() > 0.5f);
}

FaceDelivery delivery(float wpm, FaceVolume volume = FaceVolume::Conversational, std::string_view contour = "flat",
                      float duration = 0.f) {
    FaceDelivery d;
    d.paceWpm = wpm;
    d.volume = volume;
    d.duration = duration;
    REQUIRE(parsePitchContourChain(contour, d));
    return d;
}

TEST_CASE("delivery vocabulary parses") {
    FaceVolume v{};
    CHECK(parseFaceVolume("whisper", v));
    CHECK(v == FaceVolume::Whisper);
    CHECK(parseFaceVolume("shout", v));
    CHECK(v == FaceVolume::Shout);
    CHECK_FALSE(parseFaceVolume("loud", v));
    CHECK(volumeJawScale(FaceVolume::Whisper) == doctest::Approx(0.5f));
    CHECK(volumeJawScale(FaceVolume::Soft) == doctest::Approx(0.8f));
    CHECK(volumeJawScale(FaceVolume::Conversational) == doctest::Approx(1.f));
    CHECK(volumeJawScale(FaceVolume::Raised) == doctest::Approx(1.15f));
    CHECK(volumeJawScale(FaceVolume::Shout) == doctest::Approx(1.3f));

    CHECK(paceBandWpm("very slow") == doctest::Approx(92.5f));
    CHECK(paceBandWpm("normal") == doctest::Approx(140.f));
    CHECK(paceBandWpm("rushed") == doctest::Approx(205.f));
    CHECK(paceBandWpm("brisk") == 0.f);

    PitchContour c{};
    for (const std::string_view word : {"falling", "rising", "flat", "arc", "dipping", "breaking", "climbing", "stepped",
                                        "trailing", "clipped", "lilting"})
        CHECK(parsePitchContour(word, c));
    CHECK_FALSE(parsePitchContour("wobbly", c));

    FaceDelivery d;
    REQUIRE(parsePitchContourChain("stepped>climbing > falling", d));
    REQUIRE(d.contourCount == 3);
    CHECK(d.contours[0] == PitchContour::Stepped);
    CHECK(d.contours[1] == PitchContour::Climbing);
    CHECK(d.contours[2] == PitchContour::Falling);
    CHECK_FALSE(parsePitchContourChain("rising>wobbly", d));
    CHECK(d.contourCount == 0);
    CHECK_FALSE(parsePitchContourChain("rising>", d));
    CHECK_FALSE(parsePitchContourChain("", d));
    CHECK_FALSE(parsePitchContourChain("flat>flat>flat>flat>flat", d));
    CHECK(parsePitchContourChain("flat>flat>flat>flat", d));
}

TEST_CASE("pace: blink rate x0.8-1.2, blend time 400-150 ms, head motion") {
    MiniRig r;
    {
        FacePlayer player(r.set);
        FaceDirector dir(r.set, r.map, player);
        dir.speak("warm", "", 3, delivery(80.f));
        CHECK(dir.pace() == doctest::Approx(0.f));
        CHECK(dir.paceBlinkScale() == doctest::Approx(0.8f));
        CHECK(dir.blendTime() == doctest::Approx(0.4f));
        CHECK(dir.headMotion() == doctest::Approx(0.7f));
        dir.speak("warm", "", 3, delivery(300.f)); // past the fast end: clamped
        CHECK(dir.pace() == doctest::Approx(1.f));
        CHECK(dir.paceBlinkScale() == doctest::Approx(1.2f));
        CHECK(dir.blendTime() == doctest::Approx(0.15f));
        CHECK(dir.headMotion() == doctest::Approx(1.3f));
        dir.speak("warm", "", 3, FaceDelivery{}); // unknown pace: neutral
        CHECK(dir.paceBlinkScale() == 1.f);
        CHECK(dir.headMotion() == 1.f);
        dir.speak("warm", "", 3, delivery(230.f));
        dir.listen("", 3); // listening drops the line's pace
        CHECK(dir.paceBlinkScale() == 1.f);
        CHECK(dir.headMotion() == 1.f);
    }
    // A still profile stays still at any pace; the pace scales the profile's head motion.
    {
        FacePlayer player(r.set);
        FaceDirector dir(r.set, r.map, player);
        ContainmentProfile still;
        still.headMotion = 0.f;
        dir.setProfile(&still);
        dir.speak("warm", "", 3, delivery(230.f));
        CHECK(dir.headMotion() == 0.f);
        ContainmentProfile half;
        half.headMotion = 0.5f;
        dir.setProfile(&half);
        dir.speak("warm", "", 3, delivery(230.f));
        CHECK(dir.headMotion() == doctest::Approx(0.65f));
        const FaceCue cues[] = {{0.f, 0.2f, FaceCueKind::Emphasis}};
        dir.speak("warm", "", 3, delivery(230.f), cues);
        float peak = 0.f;
        for (int f = 0; f < 30; ++f) {
            dir.tick(kDt);
            peak = std::max(peak, dir.headNod());
        }
        CHECK(peak == doctest::Approx(0.65f).epsilon(0.02)); // the nod follows the paced head motion
    }
    // The blend time is the expression crossfade: a slow line is still fading in when a fast one is done.
    const auto smileAfter = [&](float wpm, float seconds) {
        FacePlayer player(r.set);
        FaceDirector dir(r.set, r.map, player);
        dir.speak("warm", "", 5, delivery(wpm));
        run(dir, player, seconds);
        return at(player.weights(), r.smileL);
    };
    CHECK(smileAfter(230.f, 0.2f) == doctest::Approx(1.f));
    CHECK(smileAfter(80.f, 0.2f) < 0.8f);
    CHECK(smileAfter(80.f, 0.45f) == doctest::Approx(1.f));
}

TEST_CASE("pace scales the blink rate on top of the profile's, and listening restores it") {
    MiniRig r;
    const auto countBlinks = [&](float wpm, bool listenFirst) {
        FacePlayer player(r.set, 99u);
        FaceDirector dir(r.set, r.map, player, 99u);
        ContainmentProfile p;
        p.blinkRate = 0.5f;
        dir.setProfile(&p);
        dir.speak("warm", "", 3, delivery(wpm));
        if (listenFirst) dir.listen("", 3);
        int n = 0;
        bool was = false;
        for (int f = 0; f < 60 * 900; ++f) {
            dir.tick(kDt);
            player.update(kDt);
            if (player.blinkActive() && !was) ++n;
            was = player.blinkActive();
        }
        return n;
    };
    const int slow = countBlinks(80.f, false), fast = countBlinks(230.f, false), listening = countBlinks(230.f, true);
    CHECK(slow > 50);
    CHECK(static_cast<float>(fast) / static_cast<float>(slow) == doctest::Approx(1.5f).epsilon(0.15));
    CHECK(static_cast<float>(listening) / static_cast<float>(fast) == doctest::Approx(1.f / 1.2f).epsilon(0.15));
}

TEST_CASE("pitch contour: rising lifts the brows, falling settles them, clipped presses") {
    MiniRig r;
    FacePlayer player(r.set);
    FaceDirector dir(r.set, r.map, player);

    dir.speak("warm", "", 3, delivery(140.f, FaceVolume::Conversational, "rising"));
    CHECK(dir.contour() == PitchContour::Rising);
    run(dir, player, 0.05f);
    CHECK(at(dir.micro(), r.browIn) < 0.1f); // eases in, no pop
    run(dir, player, 2.f);
    CHECK(at(dir.micro(), r.browIn) == doctest::Approx(0.25f).epsilon(0.01));
    CHECK(at(dir.micro(), r.browOutL) == doctest::Approx(0.25f).epsilon(0.01));
    CHECK(at(dir.micro(), r.browDownL) == 0.f);
    CHECK(at(player.weights(), r.browIn) == doctest::Approx(0.25f).epsilon(0.01)); // shows through the upper layer

    dir.speak("warm", "", 3, delivery(140.f, FaceVolume::Conversational, "falling"));
    run(dir, player, 3.f);
    CHECK(at(dir.micro(), r.browIn) < 0.01f);
    CHECK(at(dir.micro(), r.browDownL) == doctest::Approx(0.15f).epsilon(0.01));

    dir.listen("", 3); // the contour brows ease out
    run(dir, player, 3.f);
    CHECK(at(dir.micro(), r.browDownL) < 0.01f);
    CHECK(dir.contour() == PitchContour::Flat);

    // Contours without a spec'd face effect do nothing.
    for (const std::string_view word : {"flat", "stepped", "lilting", "arc", "trailing"}) {
        dir.speak("warm", "", 3, delivery(140.f, FaceVolume::Conversational, word));
        run(dir, player, 1.f);
        for (const int s : {r.browIn, r.browDownL, r.blinkL}) CHECK(at(dir.micro(), s) == 0.f);
    }

    FaceDirector plain(r.set, r.map, player);
    plain.speak("warm", "", 3, delivery(140.f));
    const float press = at(plain.pose().weights, r.pressL);
    dir.speak("warm", "", 3, delivery(140.f, FaceVolume::Conversational, "stepped>clipped"));
    CHECK(at(dir.pose().weights, r.pressL) == doctest::Approx(press + 0.3f));
    CHECK(at(dir.pose().weights, r.pressR) == doctest::Approx(0.3f));
}

TEST_CASE("pitch contour: breaking flutters the lids at its start and at each pause") {
    MiniRig r;
    FacePlayer player(r.set);
    player.setBlinking(false);
    FaceDirector dir(r.set, r.map, player, 3u);
    const FaceCue cues[] = {{1.f, 0.f, FaceCueKind::Pause}};
    dir.speak("sad", "", 3, delivery(140.f, FaceVolume::Conversational, "breaking"), cues);
    const auto bursts = [&](float from, float to, float& peak) {
        int closes = 0;
        bool up = false;
        float prev = 0.f;
        peak = 0.f;
        for (float t = from; t < to; t += kDt) {
            dir.tick(kDt);
            player.update(kDt);
            const float w = at(dir.micro(), r.blinkL);
            peak = std::max(peak, w);
            if (w > prev && !up && w > 0.2f) {
                ++closes;
                up = true;
            }
            if (w < 0.05f) up = false;
            prev = w;
        }
        return closes;
    };
    float peak = 0.f;
    const int first = bursts(0.f, 0.6f, peak);
    CHECK(first >= 2);
    CHECK(first <= 3);
    CHECK(peak == doctest::Approx(0.35f).epsilon(0.05));
    CHECK(at(player.weights(), r.blinkL) < 0.05f); // over by now
    CHECK(bursts(0.6f, 0.95f, peak) == 0);
    const int second = bursts(0.95f, 1.6f, peak);
    CHECK(second >= 2);
    CHECK(second <= 3);
}

TEST_CASE("pitch contour: a chain plays in equal parts of the line") {
    MiniRig r;
    FacePlayer player(r.set);
    FaceDirector dir(r.set, r.map, player);
    dir.speak("warm", "", 3, delivery(140.f, FaceVolume::Conversational, "rising>falling", 2.f));
    run(dir, player, 0.9f);
    CHECK(dir.contour() == PitchContour::Rising);
    run(dir, player, 0.3f);
    CHECK(dir.contour() == PitchContour::Falling);
    run(dir, player, 3.f); // past the end: the last part holds
    CHECK(dir.contour() == PitchContour::Falling);
    CHECK(at(dir.micro(), r.browDownL) == doctest::Approx(0.15f).epsilon(0.02));

    // Without a duration the first part plays throughout.
    dir.speak("warm", "", 3, delivery(140.f, FaceVolume::Conversational, "rising>falling"));
    run(dir, player, 3.f);
    CHECK(dir.contour() == PitchContour::Rising);
}

TEST_CASE("volume: mouthStretch at raised and shout; the jaw scale is an output only") {
    MiniRig r;
    FacePlayer player(r.set);
    FaceDirector dir(r.set, r.map, player);
    const auto line = [&](FaceVolume v) {
        dir.speak("sad", "", 3, delivery(140.f, v));
        return dir.pose();
    };
    const FacePreset conversational = line(FaceVolume::Conversational);
    CHECK(dir.jawScale() == doctest::Approx(1.f));
    CHECK(at(conversational.weights, r.stretchL) == 0.f);
    CHECK(at(line(FaceVolume::Whisper).weights, r.stretchL) == 0.f);
    CHECK(dir.jawScale() == doctest::Approx(0.5f));
    CHECK(at(line(FaceVolume::Soft).weights, r.stretchL) == 0.f);
    CHECK(at(line(FaceVolume::Raised).weights, r.stretchL) == doctest::Approx(0.15f));
    const FacePreset shout = line(FaceVolume::Shout);
    CHECK(at(shout.weights, r.stretchL) == doctest::Approx(0.3f));
    CHECK(dir.jawScale() == doctest::Approx(1.3f));
    CHECK(at(shout.weights, r.jaw) == at(conversational.weights, r.jaw)); // baked tracks carry the jaw scale
    dir.listen("", 3);
    CHECK(dir.jawScale() == 1.f);
}

TEST_CASE("face direction is deterministic per seed") {
    MiniRig r;
    const ContainmentProfile devin = devinLike(r.set);
    const FaceCue cues[] = {{0.3f, 0.2f, FaceCueKind::Emphasis}, {1.f, 0.f, FaceCueKind::Pause},
                            {2.f, 0.f, FaceCueKind::LongPause}, {2.5f, 0.3f, FaceCueKind::Emphasis}};
    const auto record = [&](std::uint32_t seed, std::array<FaceWeights, 8>& snaps) {
        FacePlayer player(r.set, seed);
        FaceDirector dir(r.set, r.map, player, seed);
        dir.setProfile(&devin);
        std::size_t k = 0;
        for (int line = 0; line < 4; ++line) {
            if (line % 2) dir.speak("warm", "sad", 5, cues);
            else dir.speak("warm", "sad", 5, delivery(170.f, FaceVolume::Raised, "rising>breaking>clipped", 3.f), cues);
            run(dir, player, 3.f);
            snaps[k++] = player.weights();
            dir.listen("warm", 3);
            dir.speakerPause(FaceCueKind::LongPause);
            run(dir, player, 1.5f);
            snaps[k++] = player.weights();
        }
    };
    std::array<FaceWeights, 8> a{}, b{}, c{};
    record(5u, a);
    record(5u, b);
    record(6u, c);
    CHECK(a == b);
    CHECK(a != c);
}

TEST_CASE("face direction does not allocate per frame") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    MiniRig r;
    const ContainmentProfile devin = devinLike(r.set);
    FacePlayer player(r.set);
    FaceDirector dir(r.set, r.map, player);
    dir.setProfile(&devin);
    const FaceCue cues[] = {{0.3f, 0.2f, FaceCueKind::Emphasis}, {1.f, 0.f, FaceCueKind::Pause},
                            {2.f, 0.f, FaceCueKind::LongPause}};
    FacePreset out;
    float excess = 0.f;
    const auto before = ftcore_test::allocStats().count;
    for (int line = 0; line < 3; ++line) {
        resolveContainedEmotion(r.set, r.map, &devin, "warm", "sad", 4, out, &excess);
        dir.speak("warm", "sad", 5, delivery(170.f, FaceVolume::Shout, "breaking>rising", 3.f), cues);
        run(dir, player, 3.f);
        dir.listen("sad", 3);
        dir.speakerPhrase();
        dir.speakerPause(FaceCueKind::LongPause);
        run(dir, player, 2.f);
    }
    CHECK(ftcore_test::allocStats().count == before);
}

TEST_CASE("real data: the draft profiles and mirror flags load and direct a face") {
    std::string presets, mapText, profileText;
    if (!ftcore_test::readProjectData("Rig/face_presets.json", presets) ||
        !ftcore_test::readProjectData("Rig/emotion_map.json", mapText) ||
        !ftcore_test::readProjectData("Rig/containment_profiles.json", profileText)) {
        MESSAGE("Data/Rig not present; skipping");
        return;
    }
    FaceShapeSet set;
    std::string err;
    REQUIRE_MESSAGE(importFaceShapeSet(presets, set, &err), err);
    EmotionMap map;
    REQUIRE_MESSAGE(importEmotionMap(mapText, set, map, &err), err);
    ContainmentProfiles profiles;
    REQUIRE_MESSAGE(importContainmentProfiles(profileText, set, profiles, &err), err);

    CHECK(map.find("warm")->mirrors);
    CHECK(map.find("sad")->mirrors);
    CHECK_FALSE(map.find("contemptuous")->mirrors);
    const ContainmentProfile* devin = profiles.find("devin");
    const ContainmentProfile* duy = profiles.find("duy");
    const ContainmentProfile* odette = profiles.find("odette");
    REQUIRE(devin);
    REQUIRE(duy);
    REQUIRE(odette);
    CHECK(devin->cap == doctest::Approx(0.7f));
    CHECK(at(devin->added, set.find("mouthPressLeft")) > 0.f);
    CHECK(duy->cap == doctest::Approx(0.6f));
    CHECK(duy->blinkRate == doctest::Approx(0.5f));
    CHECK(odette->cap == doctest::Approx(0.5f));
    CHECK(odette->headMotion == doctest::Approx(0.f));

    const FaceCue cues[] = {{0.2f, 0.25f, FaceCueKind::Emphasis}, {0.8f, 0.f, FaceCueKind::LongPause}};
    for (const ContainmentProfile& p : profiles.profiles) {
        FacePlayer player(set);
        FaceDirector dir(set, map, player);
        dir.setProfile(&p);
        for (const EmotionDef& e : map.emotions) {
            CAPTURE(p.name);
            CAPTURE(e.name);
            dir.speak(e.name, "anxious", 5, cues);
            run(dir, player, 0.5f);
            for (const float w : player.weights()) {
                CHECK(w >= 0.f);
                CHECK(w <= 1.f);
            }
        }
    }
}
