#include "ftcore/Face.h"

#include "AllocCounter.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace ftcore;

namespace {

// A small hand-built set: one shape per layer that the layering treats differently, plus a
// corrective with a side-less driver.
struct MiniFace {
    FaceShapeSet set;
    int brow = -1, blinkL = -1, jaw = -1, funnel = -1, smileL = -1, smileR = -1, fix = -1;

    MiniFace() {
        set.addShape("browInnerUp", FaceLayer::Upper);
        set.addShape("eyeBlinkLeft", FaceLayer::Eyes);
        set.addShape("eyeBlinkRight", FaceLayer::Eyes);
        set.addShape("jawOpen", FaceLayer::MouthArticulation);
        set.addShape("mouthFunnel", FaceLayer::MouthArticulation);
        set.addShape("mouthClose", FaceLayer::MouthArticulation);
        set.addShape("mouthSmileLeft", FaceLayer::MouthTone);
        set.addShape("mouthSmileRight", FaceLayer::MouthTone);
        set.addShape("jawOpen_x_mouthSmile", FaceLayer::Corrective);
        const std::array<FaceShapeWeight, 2> aa{{{"jawOpen", 0.8f}, {"mouthFunnel", 0.1f}}};
        set.addViseme("AA", aa, FaceVec3{10.f, 0.f, 0.f});
        const std::array<FaceShapeWeight, 3> smile{{{"mouthSmileLeft", 1.f}, {"mouthSmileRight", 0.8f}, {"browInnerUp", 0.4f}}};
        set.addExpression("smile", smile);
        REQUIRE(set.finish());
        brow = set.find("browInnerUp");
        blinkL = set.find("eyeBlinkLeft");
        jaw = set.find("jawOpen");
        funnel = set.find("mouthFunnel");
        smileL = set.find("mouthSmileLeft");
        smileR = set.find("mouthSmileRight");
        fix = set.find("jawOpen_x_mouthSmile");
    }
};

float at(const FaceWeights& w, int i) { return w[static_cast<std::size_t>(i)]; }
void put(FaceWeights& w, int i, float v) { w[static_cast<std::size_t>(i)] = v; }

FaceTrack makeTrack(std::vector<std::string> channels, std::uint32_t frames, std::vector<std::uint8_t> frame0) {
    FaceTrack t;
    t.rate = 60.f;
    t.frames = frames;
    t.channels = std::move(channels);
    for (std::uint32_t f = 0; f < frames; ++f) t.data.insert(t.data.end(), frame0.begin(), frame0.end());
    return t;
}

} // namespace

TEST_CASE("face shape set resolves presets, named shapes and correctives") {
    MiniFace m;
    CHECK(m.set.size() == 9);
    CHECK(m.set.jawOpen == m.jaw);
    CHECK(m.set.mouthFunnel == m.funnel);
    CHECK(m.set.mouthPucker == -1);
    CHECK(m.set.blinkLeft == m.blinkL);
    REQUIRE(m.set.correctives.size() == 1);
    const auto& c = m.set.correctives[0];
    CHECK(c.shape == m.fix);
    CHECK(c.driverA == m.jaw);
    CHECK(c.driverB == m.smileL); // side-less driver: Left, then Right
    CHECK(c.driverB2 == m.smileR);

    const FacePreset* aa = m.set.viseme("AA");
    REQUIRE(aa);
    CHECK(at(aa->weights, m.jaw) == doctest::Approx(0.8f));
    CHECK(aa->tongueDeg == FaceVec3{10.f, 0.f, 0.f});
    CHECK(m.set.expression("smile"));
    CHECK_FALSE(m.set.expression("frown"));

    FaceLayer layer{};
    CHECK(faceLayerFromName("mouth_tone", layer));
    CHECK(layer == FaceLayer::MouthTone);
    CHECK_FALSE(faceLayerFromName("lips", layer));
}

TEST_CASE("face shape set rejects bad entries") {
    std::string error;
    FaceShapeSet s;
    REQUIRE(s.addShape("jawOpen", FaceLayer::MouthArticulation));
    CHECK_FALSE(s.addShape("jawOpen", FaceLayer::MouthArticulation, &error));
    CHECK(error.find("twice") != std::string::npos);
    const std::array<FaceShapeWeight, 1> bad{{{"nope", 1.f}}};
    CHECK_FALSE(s.addViseme("X", bad, {}, &error));
    CHECK(error.find("nope") != std::string::npos);

    FaceShapeSet u = s;
    u.addShape("jawOpen_x_cheekPuff", FaceLayer::Corrective);
    CHECK_FALSE(u.finish(&error));
    CHECK(error.find("unknown drivers") != std::string::npos);

    FaceShapeSet v = s;
    v.addShape("jawFix", FaceLayer::Corrective);
    CHECK_FALSE(v.finish(&error));

    FaceShapeSet big;
    for (std::size_t i = 0; i < kMaxFaceShapes; ++i) REQUIRE(big.addShape("s" + std::to_string(i), FaceLayer::Upper));
    CHECK_FALSE(big.addShape("one_more", FaceLayer::Upper, &error));
    CHECK(big.finish());
}

TEST_CASE("composeFace layers expression, viseme, speech and micro weights") {
    MiniFace m;
    FaceWeights e{}, v{}, micro{}, out{};
    put(e, m.smileL, 1.f);
    put(e, m.brow, 0.5f);
    put(e, m.jaw, 0.8f);
    put(v, m.jaw, 0.6f);
    put(v, m.funnel, 0.5f);
    put(micro, m.brow, 0.2f);
    put(micro, m.blinkL, 0.9f);

    composeFace(m.set, e, v, 1.f, micro, out);
    CHECK(at(out, m.brow) == doctest::Approx(0.7f));        // upper: e + m
    CHECK(at(out, m.blinkL) == doctest::Approx(0.9f));      // eyes: max
    CHECK(at(out, m.jaw) == doctest::Approx(0.6f));         // speaking: the viseme wins
    CHECK(at(out, m.funnel) == doctest::Approx(0.5f));
    CHECK(at(out, m.smileL) == doctest::Approx(0.65f));     // 1 * (1 - 0.7 * 0.5)
    CHECK(at(out, m.fix) == doctest::Approx(0.6f * 0.65f)); // jaw * max(smileL, smileR)

    composeFace(m.set, e, v, 0.f, micro, out);
    CHECK(at(out, m.jaw) == doctest::Approx(0.8f)); // quiet: max(v, e)
    put(micro, m.brow, 0.9f);
    composeFace(m.set, e, v, 0.f, micro, out);
    CHECK(at(out, m.brow) == doctest::Approx(1.f)); // clamped
}

TEST_CASE(".nface tracks round-trip and reject bad data") {
    FaceTrack t = makeTrack({"jawOpen", "@speech", "@tongue_02"}, 4, {10, 200, 128});
    t.data[5] = 77;
    const std::vector<std::uint8_t> bytes = serializeFaceTrack(t);
    FaceTrack back;
    std::string error;
    REQUIRE(parseFaceTrack(bytes, back, &error));
    CHECK(back.rate == doctest::Approx(60.f));
    CHECK(back.frames == 4);
    CHECK(back.channels == t.channels);
    CHECK(back.data == t.data);
    CHECK(back.duration() == doctest::Approx(3.f / 60.f));

    std::vector<std::uint8_t> shortBytes(bytes.begin(), bytes.end() - 1);
    CHECK_FALSE(parseFaceTrack(shortBytes, back, &error));
    CHECK(back.frames == 0);
    std::vector<std::uint8_t> badMagic = bytes;
    badMagic[0] = 'X';
    CHECK_FALSE(parseFaceTrack(badMagic, back, &error));
    CHECK_FALSE(parseFaceTrack(std::span<const std::uint8_t>(bytes.data(), 10), back, &error));
}

TEST_CASE("face track drives jaw and tongue") {
    MiniFace m;
    FacePlayer p(m.set);
    p.setBlinking(false);
    const FaceTrack t = makeTrack({"jawOpen", "@tongue_01", "@tongue_03", "notAShape"}, 30, {255, 255, 0, 255});
    REQUIRE(p.play(t));
    p.update(0.f);
    CHECK(p.jawDeg() == doctest::Approx(25.f));
    CHECK(p.tongueDeg().x == doctest::Approx(40.f));
    CHECK(p.tongueDeg().y == doctest::Approx(0.f));
    CHECK(p.tongueDeg().z == doctest::Approx(-40.f));
    CHECK(p.playing());
    for (int i = 0; i < 60; ++i) p.update(1.f / 60.f);
    CHECK_FALSE(p.playing()); // past its end
    CHECK(p.tongueDeg() == FaceVec3{});
    CHECK(p.jawDeg() == doctest::Approx(0.f));
}

TEST_CASE("face speech ramps over 120 ms") {
    MiniFace m;
    FacePlayer p(m.set);
    p.setBlinking(false);
    const FaceTrack t = makeTrack({"@speech"}, 120, {255});
    REQUIRE(p.play(t));
    for (int i = 0; i < 6; ++i) p.update(0.01f);
    CHECK(p.speech() == doctest::Approx(0.5f).epsilon(0.01));
    for (int i = 0; i < 6; ++i) p.update(0.01f);
    CHECK(p.speech() == doctest::Approx(1.f));
    p.stop();
    for (int i = 0; i < 12; ++i) p.update(0.01f);
    CHECK(p.speech() == doctest::Approx(0.f));
}

TEST_CASE("face expression crossfades with smoothstep and scales by intensity") {
    MiniFace m;
    FacePlayer p(m.set);
    p.setBlinking(false);
    p.setExpression(m.set.expression("smile"), 5.f, 1.f);
    p.update(0.5f);
    CHECK(at(p.weights(), m.smileL) == doctest::Approx(0.5f));
    p.update(0.5f);
    CHECK(at(p.weights(), m.smileL) == doctest::Approx(1.f));
    CHECK(at(p.weights(), m.smileR) == doctest::Approx(0.8f));

    p.setExpression(m.set.expression("smile"), 2.5f, 0.f);
    p.update(0.f);
    CHECK(at(p.weights(), m.smileL) == doctest::Approx(0.5f));
    p.setExpression(nullptr, 5.f, 0.f);
    p.update(0.f);
    CHECK(at(p.weights(), m.smileL) == doctest::Approx(0.f));
}

TEST_CASE("face blinks are seeded and deterministic") {
    MiniFace m;
    FacePlayer a(m.set, 1234u), b(m.set, 1234u), c(m.set, 99u);
    float peak = 0.f;
    bool same = true, differs = false;
    for (int i = 0; i < 700; ++i) { // 7 s: the first blink starts within 6 s
        a.update(0.01f);
        b.update(0.01f);
        c.update(0.01f);
        same = same && a.weights() == b.weights();
        differs = differs || a.weights() != c.weights();
        peak = std::max(peak, at(a.weights(), m.blinkL));
    }
    CHECK(same);
    CHECK(differs);
    CHECK(peak > 0.9f);

    FacePlayer quiet(m.set, 1234u);
    quiet.setBlinking(false);
    for (int i = 0; i < 700; ++i) quiet.update(0.01f);
    CHECK(at(quiet.weights(), m.blinkL) == doctest::Approx(0.f));
}

TEST_CASE("face player binds mesh targets by name") {
    MiniFace m;
    const std::array<std::string_view, 4> targets{"mouthSmileLeft", "unused", "jawOpen", "browInnerUp"};
    std::vector<int> map;
    FacePlayer::bindMesh(m.set, targets, map);
    REQUIRE(map.size() == 4);
    CHECK(map[0] == m.smileL);
    CHECK(map[1] == -1);
    CHECK(map[2] == m.jaw);

    FacePlayer p(m.set);
    p.setBlinking(false);
    p.setMicro(m.brow, 0.3f);
    p.setMicro(-4, 1.f); // ignored
    p.update(0.f);
    std::array<float, 4> out{9.f, 9.f, 9.f, 9.f};
    p.writeMeshWeights(map, out);
    CHECK(out[0] == doctest::Approx(0.f));
    CHECK(out[1] == doctest::Approx(0.f));
    CHECK(out[3] == doctest::Approx(0.3f));
    p.clearMicro();
    p.update(0.f);
    p.writeMeshWeights(map, out);
    CHECK(out[3] == doctest::Approx(0.f));
}

TEST_CASE(".nface tracks cut short at any byte are rejected") {
    FaceTrack t = makeTrack({"jawOpen", "@speech", "@tongue_02", "mouthSmileLeft"}, 5, {10, 200, 128, 40});
    const std::vector<std::uint8_t> bytes = serializeFaceTrack(t);
    FaceTrack back;
    std::string error;
    REQUIRE(parseFaceTrack(bytes, back, &error));
    int accepted = 0, dirty = 0;
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        back = t; // a parse failure must leave `back` empty, whatever it held
        if (parseFaceTrack(std::span<const std::uint8_t>(bytes.data(), n), back, &error)) ++accepted;
        if (back.frames != 0 || !back.channels.empty() || !back.data.empty()) ++dirty;
    }
    CHECK(accepted == 0);
    CHECK(dirty == 0);
}

TEST_CASE("a face set at the 64-shape budget, correctives last, composes and binds every shape") {
    FaceShapeSet set;
    REQUIRE(set.addShape("jawOpen", FaceLayer::MouthArticulation));
    REQUIRE(set.addShape("mouthFunnel", FaceLayer::MouthArticulation));
    REQUIRE(set.addShape("mouthSmileLeft", FaceLayer::MouthTone));
    REQUIRE(set.addShape("mouthSmileRight", FaceLayer::MouthTone));
    while (set.size() < kMaxFaceShapes - 2) REQUIRE(set.addShape("s" + std::to_string(set.size()), FaceLayer::Upper));
    REQUIRE(set.addShape("jawOpen_x_mouthSmile", FaceLayer::Corrective));
    REQUIRE(set.addShape("jawOpen_x_mouthFunnel", FaceLayer::Corrective));
    std::string error;
    CHECK_FALSE(set.addShape("one_more", FaceLayer::Upper, &error));
    const std::array<FaceShapeWeight, 2> aa{{{"jawOpen", 0.6f}, {"mouthFunnel", 0.5f}}};
    REQUIRE(set.addViseme("AA", aa));
    const std::array<FaceShapeWeight, 2> smile{{{"mouthSmileLeft", 0.8f}, {"s61", 0.7f}}};
    REQUIRE(set.addExpression("smile", smile));
    REQUIRE_MESSAGE(set.finish(&error), error);
    REQUIRE(set.size() == kMaxFaceShapes);
    REQUIRE(set.correctives.size() == 2);
    const int last = static_cast<int>(kMaxFaceShapes) - 1;
    CHECK(set.correctives[1].shape == last);
    CHECK(set.find("jawOpen_x_mouthFunnel") == last);

    FaceWeights out{}, micro{};
    composeFace(set, set.expression("smile")->weights, set.viseme("AA")->weights, 1.f, micro, out);
    const auto w = [&](std::string_view name) { return out[static_cast<std::size_t>(set.find(name))]; };
    CHECK(w("s61") == doctest::Approx(0.7f));
    CHECK(w("jawOpen_x_mouthFunnel") == doctest::Approx(w("jawOpen") * w("mouthFunnel")));
    CHECK(w("jawOpen_x_mouthFunnel") > 0.f);
    CHECK(w("jawOpen_x_mouthSmile") == doctest::Approx(w("jawOpen") * w("mouthSmileLeft")));

    // Every shape binds to a mesh target, in reverse order, and reaches it.
    std::vector<std::string_view> targets;
    for (std::size_t i = kMaxFaceShapes; i-- > 0;) targets.push_back(set.shapes[i]);
    std::vector<int> map;
    FacePlayer::bindMesh(set, targets, map);
    REQUIRE(map.size() == kMaxFaceShapes);
    for (std::size_t t = 0; t < kMaxFaceShapes; ++t) CHECK(map[t] == static_cast<int>(kMaxFaceShapes - 1 - t));
    FacePlayer p(set);
    p.setBlinking(false);
    p.setMicro(set.find("s4"), 0.25f);
    p.update(0.f);
    std::vector<float> mesh(kMaxFaceShapes, 9.f);
    p.writeMeshWeights(map, mesh);
    CHECK(mesh[kMaxFaceShapes - 1 - 4] == doctest::Approx(0.25f));
    CHECK(mesh[0] == doctest::Approx(0.f)); // the corrective, with nothing driving it

    // A track may carry every shape plus the 8 special channels, and no more.
    std::vector<std::string> channels(set.shapes.begin(), set.shapes.end());
    for (const char* c : {"@speech", "@tongue_01", "@tongue_02", "@tongue_03", "@a", "@b", "@c", "@d"}) channels.push_back(c);
    REQUIRE(channels.size() == FacePlayer::kMaxTrackChannels);
    const FaceTrack full = makeTrack(channels, 2, std::vector<std::uint8_t>(channels.size(), 128));
    CHECK(p.play(full));
    channels.push_back("@e");
    const FaceTrack over = makeTrack(channels, 2, std::vector<std::uint8_t>(channels.size(), 128));
    CHECK_FALSE(p.play(over));
    CHECK_FALSE(p.playing());
}

TEST_CASE("face player updates without allocating") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    MiniFace m;
    const FaceTrack t = makeTrack({"jawOpen", "@speech", "@tongue_01"}, 300, {200, 255, 64});
    const std::array<std::string_view, 3> targets{"jawOpen", "mouthSmileLeft", "eyeBlinkLeft"};
    std::vector<int> map;
    FacePlayer::bindMesh(m.set, targets, map);
    std::array<float, 3> out{};
    FacePlayer p(m.set);

    const auto before = ftcore_test::allocStats().count;
    p.setExpression(m.set.expression("smile"), 4.f, 0.2f);
    REQUIRE(p.play(t));
    for (int i = 0; i < 600; ++i) {
        p.update(1.f / 60.f);
        p.setMicro(m.brow, static_cast<float>(i % 10) * 0.1f);
        p.writeMeshWeights(map, out);
        if (i == 400) p.play(t, 1.f);
    }
    const auto after = ftcore_test::allocStats().count;
    CHECK(after == before);
}
