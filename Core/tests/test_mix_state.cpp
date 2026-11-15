#include "ftcore/MixState.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

using namespace ftcore;

namespace {

constexpr MixRequests kExplore = 0;
const MixRequests kDialogue = mixRequest(MixMode::Dialogue);
const MixRequests kCombat = mixRequest(MixMode::Combat);
const MixRequests kMenu = mixRequest(MixMode::Menu);

int bus(MixBus b) { return static_cast<int>(b); }

// Steps `mix` with `req` for `seconds` at `hz` steps per second.
void run(MixState& mix, MixRequests req, double seconds, int hz) {
    const int steps = static_cast<int>(std::lround(seconds * hz));
    for (int i = 0; i < steps; ++i) mix.step(req, 1.f / static_cast<float>(hz));
}

// Seconds until the bus is within 0.01 dB of its target, stepping at 1000 Hz.
double timeToSettle(MixState& mix, MixRequests req, MixBus b) {
    for (int i = 1; i < 10000; ++i) {
        mix.step(req, 0.001f);
        if (std::fabs(mix.gainDb(b) - mix.targetDb(b)) < 0.01f) return i * 0.001;
    }
    return -1.0;
}

} // namespace

TEST_CASE("activeMixMode: Menu > Cutscene > Dialogue > Combat > Explore") {
    CHECK(activeMixMode(0) == MixMode::Explore);
    CHECK(activeMixMode(mixRequest(MixMode::Explore)) == MixMode::Explore);
    CHECK(activeMixMode(kCombat) == MixMode::Combat);
    CHECK(activeMixMode(kCombat | kDialogue) == MixMode::Dialogue);
    CHECK(activeMixMode(kCombat | kDialogue | mixRequest(MixMode::Cutscene)) == MixMode::Cutscene);
    CHECK(activeMixMode(0x1F) == MixMode::Menu);
    CHECK(activeMixMode(kMenu | kCombat) == MixMode::Menu);
    CHECK(activeMixMode(0xE0) == MixMode::Explore); // bits past Menu mean nothing
}

TEST_CASE("defaults: the table") {
    const MixTable t = defaultMixTable();
    const auto g = [&](MixMode m, MixBus b) { return t.gainDb[static_cast<int>(m)][bus(b)]; };
    for (int b = 0; b < kMixBuses; ++b) CHECK(t.gainDb[0][b] == 0.f);
    CHECK(g(MixMode::Dialogue, MixBus::Music) == -6.f);
    CHECK(g(MixMode::Dialogue, MixBus::Ambience) == -4.f);
    CHECK(g(MixMode::Dialogue, MixBus::Voice) == 0.f);
    CHECK(g(MixMode::Combat, MixBus::Ambience) == -6.f);
    for (int b = 0; b < kMixBuses; ++b)
        CHECK(t.gainDb[static_cast<int>(MixMode::Menu)][b] == (b == bus(MixBus::Ui) ? 0.f : -12.f));
    CHECK(t.attackSeconds == doctest::Approx(0.15));
    CHECK(t.releaseSeconds == doctest::Approx(0.6));
    CHECK(t.voiceCap[0] == 24);
    CHECK(t.voiceCap[static_cast<int>(MixMode::Combat)] == 16);
    CHECK(mixBusFor(AudioBus::Music) == MixBus::Music);
    CHECK(mixBusFor(AudioBus::Voice) == MixBus::Voice);
    CHECK(mixBusFor(AudioBus::Sfx) == MixBus::Sfx);
    CHECK(mixBusFor(AudioBus::Ambience) == MixBus::Ambience);
}

TEST_CASE("MixState: starts at Explore, the highest request wins, gains are linear") {
    MixState mix;
    for (int b = 0; b < kMixBuses; ++b) CHECK(mix.gains().linear[b] == 1.f);
    run(mix, kCombat | kDialogue, 2.0, 60);
    CHECK(mix.active() == MixMode::Dialogue);
    CHECK(mix.gainDb(MixBus::Music) == doctest::Approx(-6.0));
    CHECK(mix.gainDb(MixBus::Ambience) == doctest::Approx(-4.0));
    CHECK(mix.gainDb(MixBus::Voice) == 0.f);
    CHECK(mix.gains()[MixBus::Music] == doctest::Approx(std::pow(10.0, -6.0 / 20.0)));
    CHECK(mix.voiceCap() == 16);
    run(mix, kMenu | kDialogue, 2.0, 60);
    CHECK(mix.active() == MixMode::Menu);
    CHECK(mix.gainDb(MixBus::Voice) == doctest::Approx(-12.0));
    CHECK(mix.gainDb(MixBus::Ui) == 0.f);
    CHECK(mix.voiceCapFor(MixMode::Explore) == 24);
}

TEST_CASE("MixState: attack and release times, measured") {
    MixState mix;
    CHECK(timeToSettle(mix, kDialogue, MixBus::Music) == doctest::Approx(0.15).epsilon(0.02));
    CHECK(timeToSettle(mix, kExplore, MixBus::Music) == doctest::Approx(0.6).epsilon(0.02));
    // Halfway through the attack the bus is halfway down (linear in dB).
    MixState half;
    run(half, kMenu, 0.075, 1000);
    CHECK(half.gainDb(MixBus::Music) == doctest::Approx(-6.0).epsilon(0.02));
    // The time doesn't depend on the distance: Menu's 12 dB also takes 0.15 s.
    MixState deep;
    CHECK(timeToSettle(deep, kMenu, MixBus::Sfx) == doctest::Approx(0.15).epsilon(0.02));

    MixTable slow = defaultMixTable();
    slow.attackSeconds = 1.0f;
    slow.releaseSeconds = 2.0f;
    MixState custom(slow);
    CHECK(timeToSettle(custom, kCombat, MixBus::Ambience) == doctest::Approx(1.0).epsilon(0.01));
    CHECK(timeToSettle(custom, kExplore, MixBus::Ambience) == doctest::Approx(2.0).epsilon(0.01));

    MixTable instant = defaultMixTable();
    instant.attackSeconds = 0.f;
    MixState jump(instant);
    jump.step(kMenu, 0.f);
    CHECK(jump.gainDb(MixBus::Music) == -12.f);
}

TEST_CASE("MixState: never overshoots, even with huge steps or a target change mid-ramp") {
    MixState mix;
    float lowest = 0.f;
    for (int i = 0; i < 200; ++i) {
        mix.step(i < 10 ? kMenu : kDialogue, 0.013f);
        lowest = std::fmin(lowest, mix.gainDb(MixBus::Music));
        CHECK(mix.gainDb(MixBus::Music) >= -12.f);
        if (i >= 10) {
            // Rising from the Menu dip toward Dialogue's -6 dB: never above -6 dB.
            CHECK(mix.gainDb(MixBus::Music) <= -6.f + 1e-5f);
        }
    }
    CHECK(mix.gainDb(MixBus::Music) == -6.f);
    CHECK(lowest >= -12.f);
    CHECK(lowest < -9.f);
    mix.step(kExplore, 1000.f);
    CHECK(mix.gainDb(MixBus::Music) == 0.f);
    mix.step(kMenu, 1000.f);
    CHECK(mix.gainDb(MixBus::Music) == -12.f);
    CHECK(mix.gainDb(MixBus::Ui) == 0.f);
}

TEST_CASE("MixState: non-finite or negative dt counts as 0") {
    MixState mix;
    mix.step(kMenu, std::numeric_limits<float>::quiet_NaN());
    mix.step(kMenu, std::numeric_limits<float>::infinity());
    mix.step(kMenu, -1.f);
    CHECK(mix.gainDb(MixBus::Music) == 0.f);
    CHECK(mix.active() == MixMode::Menu);
    mix.step(kMenu, 0.075f);
    CHECK(mix.gainDb(MixBus::Music) == doctest::Approx(-6.0));
}

TEST_CASE("MixState: the same gains at 30, 60 and 120 steps per second") {
    // Requests change only at times all three rates share (multiples of 1/30 s).
    struct Phase {
        MixRequests req;
        double seconds;
    };
    const Phase phases[] = {{kCombat, 0.1}, {kDialogue | kCombat, 0.2}, {kMenu, 0.1}, {kExplore, 0.3}, {kCombat, 1.0}};
    MixState at[3];
    const int rates[3] = {30, 60, 120};
    for (const Phase& p : phases) {
        for (int k = 0; k < 3; ++k) run(at[k], p.req, p.seconds, rates[k]);
        for (int b = 0; b < kMixBuses; ++b) {
            CAPTURE(b);
            CHECK(at[1].gainDb(static_cast<MixBus>(b)) == doctest::Approx(at[0].gainDb(static_cast<MixBus>(b))).epsilon(1e-4));
            CHECK(at[2].gainDb(static_cast<MixBus>(b)) == doctest::Approx(at[0].gainDb(static_cast<MixBus>(b))).epsilon(1e-4));
        }
    }
}

TEST_CASE("mixVoicesToStop: lowest priority first, then the oldest, then the lower index") {
    MixVoice v[6] = {{50, 1.0f}, {10, 0.2f}, {10, 3.0f}, {90, 9.0f}, {50, 4.0f}, {10, 3.0f}};
    MixVoiceStops s = mixVoicesToStop(v, 6);
    CHECK(s.count == 0);
    s = mixVoicesToStop(v, 2);
    REQUIRE(s.count == 4);
    CHECK(s.index[0] == 2); // prio 10, age 3, lower index of the tie
    CHECK(s.index[1] == 5);
    CHECK(s.index[2] == 1); // prio 10, youngest
    CHECK(s.index[3] == 4); // prio 50, older than index 0
    s = mixVoicesToStop(v, -4);
    CHECK(s.count == 6);
    s = mixVoicesToStop(std::span<const MixVoice>(), 0);
    CHECK(s.count == 0);

    // 40 voices: only the first 32 count. Combat's cap of 16 stops the 16 lowest.
    MixVoice many[40];
    for (int i = 0; i < 40; ++i) many[i] = {i % 8, static_cast<float>(i)};
    MixState mix;
    mix.step(kCombat, 0.f);
    s = mixVoicesToStop(many, mix.voiceCap());
    REQUIRE(s.count == 16);
    for (int i = 0; i < s.count; ++i) {
        CHECK(s.index[i] < 32);
        CHECK(many[s.index[i]].priority <= 3); // 4 voices per priority: priorities 0..3
    }
    CHECK(s.index[0] == 24); // prio 0, the oldest of 0, 8, 16, 24
    CHECK(s.index[3] == 0);
}

TEST_CASE("MixState and the voice cap: no allocation") {
    if (!ftcore_test::allocHookInstalled()) return;
    MixVoice v[32];
    for (int i = 0; i < 32; ++i) v[i] = {i % 5, static_cast<float>(i) * 0.1f};
    const auto before = ftcore_test::allocStats().count;
    MixState mix;
    float sum = 0.f;
    for (int i = 0; i < 1000; ++i) sum += mix.step(i % 3 ? kCombat : kDialogue, 1.f / 60.f)[MixBus::Music];
    const MixVoiceStops s = mixVoicesToStop(v, mix.voiceCap());
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(sum > 0.f);
    CHECK(s.count == 16);
}
