#include "ftcore/AudioCues.h"
#include "ftcore/Loudness.h"
#include "ftcore/Wav.h"

#include "AllocCounter.h"
#include "RealStory.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr double kPi = 3.14159265358979323846;

void put16(std::string& s, std::uint32_t v) {
    s += static_cast<char>(v & 0xFF);
    s += static_cast<char>((v >> 8) & 0xFF);
}

void put32(std::string& s, std::uint32_t v) {
    put16(s, v & 0xFFFF);
    put16(s, v >> 16);
}

// A WAVE file around `data`: format tag 1 (PCM), 3 (float), or 0xFFFE with `subFormat`.
std::string wavFile(std::uint32_t format, std::uint32_t channels, std::uint32_t rate, std::uint32_t bits,
                    const std::string& data, std::uint32_t subFormat = 1, const std::string& extra = {}) {
    std::string fmt;
    put16(fmt, format);
    put16(fmt, channels);
    put32(fmt, rate);
    put32(fmt, rate * channels * bits / 8);
    put16(fmt, channels * bits / 8);
    put16(fmt, bits);
    if (format == 0xFFFE) {
        put16(fmt, 22);
        put16(fmt, bits);
        put32(fmt, 0);
        put16(fmt, subFormat);
        fmt += std::string("\x00\x00\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71", 14);
    }
    std::string body = "WAVE";
    body += "fmt ";
    put32(body, static_cast<std::uint32_t>(fmt.size()));
    body += fmt;
    body += extra;
    body += "data";
    put32(body, static_cast<std::uint32_t>(data.size()));
    body += data;
    if (data.size() & 1) body += '\0';
    std::string file = "RIFF";
    put32(file, static_cast<std::uint32_t>(body.size()));
    return file + body;
}

std::string pcm16(const std::vector<int>& values) {
    std::string s;
    for (int v : values) put16(s, static_cast<std::uint32_t>(v) & 0xFFFF);
    return s;
}

// A sine on every channel (the same signal), `amplitude` linear, `phase` radians.
std::vector<float> sine(double freq, double amplitude, double seconds, std::uint32_t rate, std::uint32_t channels,
                        double phase = 0.0) {
    const std::size_t frames = static_cast<std::size_t>(seconds * rate);
    std::vector<float> out(frames * channels);
    for (std::size_t i = 0; i < frames; ++i) {
        const float v = static_cast<float>(amplitude * std::sin(2.0 * kPi * freq * static_cast<double>(i) / rate + phase));
        for (std::uint32_t c = 0; c < channels; ++c) out[i * channels + c] = v;
    }
    return out;
}

double dbToLinear(double db) { return std::pow(10.0, db / 20.0); }

constexpr const char* kCues = R"J({
  "version": 1,
  "cues": [
    { "id": "MUS-T1", "bus": "music", "playback": "loop", "bpm": 54, "fadeIn": 2, "fadeOut": 3,
      "loopStart": 1.5, "loopEnd": 11.5 },
    { "id": "MUS-T2", "bus": "music", "playback": "stinger", "offset": 0.25 },
    { "id": "SFX-T1", "bus": "sfx", "playback": "oneshot", "gainDb": -3, "file": "Sfx/custom.wav" }
  ]
})J";

} // namespace

TEST_CASE("readWav reads 16-bit PCM stereo") {
    const std::string file = wavFile(1, 2, 48000, 16, pcm16({0, 16384, -32768, 32767, 100, -100}));
    WavInfo info;
    std::vector<float> samples;
    std::string error;
    REQUIRE(readWav(file, info, &samples, &error));
    CHECK(info.channels == 2);
    CHECK(info.sampleRate == 48000);
    CHECK(info.bitsPerSample == 16);
    CHECK_FALSE(info.isFloat);
    CHECK(info.frames == 3);
    CHECK_FALSE(info.hasLoop);
    REQUIRE(samples.size() == 6);
    CHECK(samples[0] == 0.f);
    CHECK(samples[1] == doctest::Approx(0.5));
    CHECK(samples[2] == -1.f);
    CHECK(samples[3] == doctest::Approx(1.0).epsilon(0.001));
    CHECK(info.durationSeconds() == doctest::Approx(3.0 / 48000));

    WavInfo headerOnly;
    REQUIRE(readWav(file, headerOnly));
    CHECK(headerOnly.frames == 3);
}

TEST_CASE("readWav reads 24-bit, 32-bit, float and extensible files") {
    std::string d24;
    for (std::uint32_t v : {0x400000u, 0xC00000u}) { // +0.5, -0.5
        d24 += static_cast<char>(v & 0xFF);
        d24 += static_cast<char>((v >> 8) & 0xFF);
        d24 += static_cast<char>((v >> 16) & 0xFF);
    }
    WavInfo info;
    std::vector<float> samples;
    REQUIRE(readWav(wavFile(1, 1, 44100, 24, d24), info, &samples));
    REQUIRE(samples.size() == 2);
    CHECK(samples[0] == doctest::Approx(0.5));
    CHECK(samples[1] == doctest::Approx(-0.5));

    std::string d32;
    put32(d32, 0xC0000000u); // -0.5
    REQUIRE(readWav(wavFile(1, 1, 44100, 32, d32), info, &samples));
    CHECK(samples[0] == doctest::Approx(-0.5));

    std::string df;
    for (float f : {0.25f, -0.75f}) {
        std::uint32_t u;
        std::memcpy(&u, &f, 4);
        put32(df, u);
    }
    REQUIRE(readWav(wavFile(3, 2, 48000, 32, df), info, &samples));
    CHECK(info.isFloat);
    CHECK(info.frames == 1);
    CHECK(samples[1] == -0.75f);

    REQUIRE(readWav(wavFile(0xFFFE, 2, 48000, 32, df, 3), info, &samples));
    CHECK(info.isFloat);
    CHECK(samples[0] == 0.25f);
    REQUIRE(readWav(wavFile(0xFFFE, 1, 48000, 16, pcm16({16384}), 1), info, &samples));
    CHECK(samples[0] == doctest::Approx(0.5));
}

TEST_CASE("readWav reports the first smpl loop") {
    std::string smpl = "smpl";
    std::string body(36, '\0');
    body[28] = 1; // one loop
    std::string loop(24, '\0');
    loop[8] = 10;  // start frame 10
    loop[12] = 90; // end frame 90
    body += loop;
    put32(smpl, static_cast<std::uint32_t>(body.size()));
    smpl += body;
    WavInfo info;
    REQUIRE(readWav(wavFile(1, 1, 48000, 16, pcm16(std::vector<int>(100, 0)), 1, smpl), info));
    CHECK(info.hasLoop);
    CHECK(info.loopStart == 10);
    CHECK(info.loopEnd == 90);
}

TEST_CASE("readWav rejects what it can't read") {
    WavInfo info;
    std::string error;
    CHECK_FALSE(readWav("not a wave file at all", info, nullptr, &error));
    CHECK(error == "not a RIFF WAVE file");
    CHECK_FALSE(readWav(wavFile(1, 1, 48000, 8, std::string(4, '\x80')), info, nullptr, &error));
    CHECK(error == "PCM must be 16, 24 or 32 bit");
    CHECK_FALSE(readWav(wavFile(2, 1, 48000, 4, std::string(4, '\0')), info, nullptr, &error));
    CHECK(error.find("compressed") != std::string::npos);
    std::string noData = wavFile(1, 1, 48000, 16, pcm16({1}));
    noData.resize(noData.size() - 10); // cut into the data chunk header: no data chunk left
    CHECK_FALSE(readWav(noData, info, nullptr, &error));
}

TEST_CASE("measureLoudness matches EBU 3341 on a 1 kHz sine") {
    const double amp = dbToLinear(-23.0);
    const std::vector<float> stereo = sine(1000.0, amp, 20.0, 48000, 2);
    const Loudness l = measureLoudness(stereo.data(), stereo.size() / 2, 2, 48000);
    REQUIRE(l.hasIntegrated);
    CHECK(l.integratedLufs == doctest::Approx(-23.0).epsilon(0.1 / 23.0));
    CHECK(l.samplePeakDb == doctest::Approx(-23.0).epsilon(0.01));

    const std::vector<float> mono = sine(1000.0, amp, 20.0, 48000, 1);
    const Loudness m = measureLoudness(mono.data(), mono.size(), 1, 48000);
    REQUIRE(m.hasIntegrated);
    CHECK(m.integratedLufs == doctest::Approx(-26.01).epsilon(0.1 / 26.0));

    // The same at 44.1 kHz: the filters are derived per rate.
    const std::vector<float> cd = sine(1000.0, amp, 20.0, 44100, 2);
    const Loudness c = measureLoudness(cd.data(), cd.size() / 2, 2, 44100);
    CHECK(c.integratedLufs == doctest::Approx(-23.0).epsilon(0.1 / 23.0));
}

TEST_CASE("measureLoudness gates silence and short clips") {
    std::vector<float> clip = sine(1000.0, dbToLinear(-23.0), 10.0, 48000, 2);
    clip.resize(clip.size() * 2, 0.f); // 10 s more of silence
    const Loudness gated = measureLoudness(clip.data(), clip.size() / 2, 2, 48000);
    REQUIRE(gated.hasIntegrated);
    CHECK(gated.integratedLufs == doctest::Approx(-23.0).epsilon(0.1 / 23.0));

    const std::vector<float> silence(48000 * 2 * 2, 0.f);
    const Loudness quiet = measureLoudness(silence.data(), 48000 * 2, 2, 48000);
    CHECK_FALSE(quiet.hasIntegrated);
    CHECK(quiet.samplePeakDb == Loudness::kSilence);

    const std::vector<float> blip = sine(1000.0, 0.5, 0.3, 48000, 1);
    const Loudness shortClip = measureLoudness(blip.data(), blip.size(), 1, 48000);
    CHECK_FALSE(shortClip.hasIntegrated);
    CHECK(shortClip.samplePeakDb == doctest::Approx(-6.02).epsilon(0.01));
}

TEST_CASE("measureLoudness finds the true peak between samples") {
    // A quarter-rate sine sampled 45 degrees off its crests: every sample is at 0.707, the wave peaks at 1.
    const std::vector<float> s = sine(12000.0, 1.0, 1.0, 48000, 1, kPi / 4);
    const Loudness l = measureLoudness(s.data(), s.size(), 1, 48000);
    CHECK(l.samplePeakDb == doctest::Approx(-3.01).epsilon(0.01));
    CHECK(l.truePeakDb > -0.3f);
    CHECK(l.truePeakDb < 0.3f);
}

TEST_CASE("importAudioCues reads the manifest") {
    AudioCueTable table;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importAudioCues(kCues, table, &error, &warnings), error);
    CHECK(warnings.empty());
    REQUIRE(table.cues.size() == 3);

    const AudioCue* loop = table.find("MUS-T1");
    REQUIRE(loop);
    CHECK(loop->bus == AudioBus::Music);
    CHECK(loop->playback == CuePlayback::Loop);
    CHECK(loop->file == "Music/MUS-T1.wav");
    CHECK(loop->bpm == 54.f);
    CHECK(loop->fadeInSeconds == 2.f);
    CHECK(loop->fadeOutSeconds == 3.f);
    CHECK(loop->hasLoopPoints());
    CHECK(loop->loopStartSeconds == 1.5f);

    const AudioCue* stinger = table.find("MUS-T2");
    REQUIRE(stinger);
    CHECK(stinger->playback == CuePlayback::Stinger);
    CHECK(stinger->offsetSeconds == 0.25f);
    CHECK_FALSE(stinger->hasLoopPoints());

    const AudioCue* sfx = table.find("SFX-T1");
    REQUIRE(sfx);
    CHECK(sfx->bus == AudioBus::Sfx);
    CHECK(sfx->file == "Sfx/custom.wav");
    CHECK(sfx->gainDb == -3.f);
    CHECK(table.find("MUS-NONE") == nullptr);

    if (ftcore_test::allocHookInstalled()) {
        const auto before = ftcore_test::allocStats().count;
        CHECK(table.find("SFX-T1") == sfx);
        CHECK(table.find("MUS-NONE") == nullptr);
        CHECK(ftcore_test::allocStats().count == before);
    }
}

TEST_CASE("importAudioCues reports bad entries") {
    AudioCueTable table;
    std::string error;
    std::vector<std::string> warnings;
    CHECK_FALSE(importAudioCues(R"J({ "version": 1, "cues": [
        { "id": "A", "bus": "music", "playback": "loop" },
        { "id": "A", "bus": "music", "playback": "loop" },
        { "id": "B", "bus": "drums", "playback": "loop" },
        { "id": "C", "bus": "sfx", "playback": "sometimes" },
        { "id": "D", "bus": "sfx", "playback": "oneshot", "loopStart": 1, "loopEnd": 2 },
        { "id": "E", "bus": "music", "playback": "loop", "loopStart": 1 },
        { "id": "F", "bus": "music", "playback": "loop", "loopStart": 3, "loopEnd": 2 },
        { "id": "G", "bus": "music", "playback": "loop", "bpm": -4, "colour": "blue" },
        { "bus": "music", "playback": "loop" } ] })J",
                                table, &error, &warnings));
    CHECK(table.cues.empty());
    CHECK(error.find("cues[1].id: duplicate cue A") != std::string::npos);
    CHECK(error.find("cues[2].bus") != std::string::npos);
    CHECK(error.find("cues[3].playback") != std::string::npos);
    CHECK(error.find("cues[4]: loop points on a cue that doesn't loop") != std::string::npos);
    CHECK(error.find("cues[5]: give both") != std::string::npos);
    CHECK(error.find("cues[6]: loopEnd must be after loopStart") != std::string::npos);
    CHECK(error.find("cues[7].bpm") != std::string::npos);
    CHECK(error.find("cues[8].id") != std::string::npos);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0] == "cues[7]: unknown key: colour");

    CHECK_FALSE(importAudioCues("{ \"version\": 2, \"cues\": [] }", table, &error));
    CHECK(error.find("version") != std::string::npos);
    CHECK_FALSE(importAudioCues("[", table, &error));
}

TEST_CASE("checkClip holds clips to the bus rules") {
    WavInfo info;
    info.channels = 2;
    info.sampleRate = 48000;
    info.bitsPerSample = 16;
    info.frames = 48000 * 10;
    Loudness good;
    good.hasIntegrated = true;
    good.integratedLufs = -18.5f;
    good.truePeakDb = -1.5f;
    CHECK(checkClip(info, good, AudioBus::Music, nullptr).empty());

    const auto codes = [](const std::vector<AssetIssue>& issues) {
        std::string s;
        for (const AssetIssue& i : issues) s += i.code + (i.severity == AssetSeverity::Error ? "! " : "? ");
        return s;
    };

    // Voice is mono and quieter: a stereo -18 LUFS clip breaks both rules.
    CHECK(codes(checkClip(info, good, AudioBus::Voice, nullptr)) == "voice-not-mono! loudness! ");
    // Sfx has no loudness target, only the peak ceiling.
    Loudness hot = good;
    hot.integratedLufs = -6.f;
    hot.truePeakDb = 0.2f;
    CHECK(codes(checkClip(info, hot, AudioBus::Sfx, nullptr)) == "true-peak! ");
    CHECK(codes(checkClip(info, hot, AudioBus::Music, nullptr)) == "loudness! true-peak! ");

    Loudness unmeasured = good;
    unmeasured.hasIntegrated = false;
    CHECK(codes(checkClip(info, unmeasured, AudioBus::Music, nullptr)) == "loudness-unmeasured? ");

    WavInfo odd = info;
    odd.sampleRate = 22050;
    odd.channels = 6;
    CHECK(codes(checkClip(odd, good, AudioBus::Music, nullptr)) == "sample-rate! channels! ");
    WavInfo empty = info;
    empty.frames = 0;
    CHECK(codes(checkClip(empty, good, AudioBus::Music, nullptr)) == "empty! ");

    AudioCue cue;
    cue.playback = CuePlayback::Loop;
    cue.loopStartSeconds = 2.f;
    cue.loopEndSeconds = 12.f; // the clip is 10 s
    CHECK(codes(checkClip(info, good, AudioBus::Music, &cue)) == "loop-range! ");
    cue.loopEndSeconds = 10.f;
    CHECK(checkClip(info, good, AudioBus::Music, &cue).empty());
}

TEST_CASE("checkLipSyncLength allows a little slack") {
    std::vector<AssetIssue> issues;
    checkLipSyncLength(4.77, 4.60, issues);
    CHECK(issues.empty());
    checkLipSyncLength(4.77, 3.0, issues);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "nface-length");
    CHECK(issues[0].severity == AssetSeverity::Warning);
}

TEST_CASE("decoding a written wav and measuring it agree with measuring the samples") {
    const std::vector<float> s = sine(440.0, dbToLinear(-20.0), 5.0, 48000, 1);
    std::string data;
    for (float v : s) put16(data, static_cast<std::uint32_t>(static_cast<int>(std::lround(v * 32767.0))) & 0xFFFF);
    WavInfo info;
    std::vector<float> decoded;
    REQUIRE(readWav(wavFile(1, 1, 48000, 16, data), info, &decoded));
    const Loudness a = measureLoudness(s.data(), s.size(), 1, 48000);
    const Loudness b = measureLoudness(decoded.data(), info.frames, info.channels, info.sampleRate);
    CHECK(b.integratedLufs == doctest::Approx(a.integratedLufs).epsilon(0.001));
}

TEST_CASE("Data/Audio/cues.json loads and covers every cue the real story fires") {
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Audio/cues.json", text));
    AudioCueTable table;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importAudioCues(text, table, &error, &warnings), error);
    CHECK(warnings.empty());
    CHECK(table.cues.size() > 0);

    FTCORE_REQUIRE_REAL_STORY(s);
    int missing = 0;
    for (std::uint32_t i = 0; i < s.db.cueCount(); ++i) {
        const std::string_view id = s.db.view(s.db.cue(i).id);
        const bool silence = id.size() >= 5 && id.substr(id.size() - 5) == "-NONE";
        if (!silence && !table.find(id)) {
            MESSAGE("no manifest entry for " << id); // a cue id, never story text
            ++missing;
        }
    }
    CHECK(missing == 0);
}
