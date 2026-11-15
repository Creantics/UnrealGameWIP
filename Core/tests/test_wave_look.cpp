#include <doctest/doctest.h>

#include "ftcore/WaveLook.h"

#include "AllocCounter.h"

#include <cmath>
#include <limits>

using namespace ftcore;

namespace {

WorldWaveFrame frameAt(float radius, float band, WaveDirection direction = WaveDirection::Forward) {
    WorldWaveFrame f;
    f.radius = radius;
    f.bandWidth = band;
    f.direction = direction;
    f.active = true;
    return f;
}

WaveViewSample hitAt(float x, float y, float z) {
    WaveViewSample s;
    s.point = {x, y, z};
    s.hit = true;
    return s;
}

WaveViewSample skyAlong(float x, float y, float z) {
    WaveViewSample s;
    s.dir = {x, y, z};
    return s;
}

} // namespace

TEST_CASE("wave look: the view share counts hits inside the front's middle, and sky rays through its sphere") {
    const WaveVec3 origin{0.f, 0.f, 0.f};
    const WaveVec3 eye{-30.f, 0.f, 2.f};
    // Radius 12, band 4: the middle is at 10 m.
    const WorldWaveFrame f = frameAt(12.f, 4.f);
    const WaveViewSample samples[] = {
        hitAt(5.f, 0.f, 0.f),    // inside
        hitAt(11.f, 0.f, 0.f),   // in the band's outer half: not yet
        hitAt(40.f, 0.f, 0.f),   // beyond
        skyAlong(1.f, 0.f, 0.f), // through the sphere
        skyAlong(-1.f, 0.f, 0.f) // away from it
    };
    CHECK(worldWaveViewShare(f, origin, eye, samples, 5) == doctest::Approx(2.f / 5.f));

    // The eye inside: every sky ray is in the second world.
    const WaveVec3 inEye{1.f, 0.f, 1.f};
    const WaveViewSample sky[] = {skyAlong(-1.f, 0.f, 0.f), skyAlong(0.f, 0.f, 1.f)};
    CHECK(worldWaveViewShare(f, origin, inEye, sky, 2) == 1.f);

    // Reverse: the second world is outside.
    const WorldWaveFrame rev = frameAt(12.f, 4.f, WaveDirection::Reverse);
    CHECK(worldWaveViewShare(rev, origin, eye, samples, 5) == doctest::Approx(3.f / 5.f));

    // Before the front moves: nothing going forward, everything in reverse.
    CHECK(worldWaveViewShare(frameAt(0.f, 0.f), origin, eye, samples, 5) == 0.f);
    CHECK(worldWaveViewShare(frameAt(0.f, 0.f, WaveDirection::Reverse), origin, eye, samples, 5) == 1.f);
}

TEST_CASE("wave look: bad samples are skipped, and no samples share nothing") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const WorldWaveFrame f = frameAt(12.f, 4.f);
    const WaveViewSample samples[] = {hitAt(nan, 0.f, 0.f), skyAlong(0.f, 0.f, 0.f), hitAt(1.f, 0.f, 0.f)};
    // The NaN hit is skipped; the zero-length sky ray counts but misses (the eye is outside).
    CHECK(worldWaveViewShare(f, {0.f, 0.f, 0.f}, {-30.f, 0.f, 0.f}, samples, 3) == doctest::Approx(0.5f));
    CHECK(worldWaveViewShare(f, {0.f, 0.f, 0.f}, {-30.f, 0.f, 0.f}, nullptr, 3) == 0.f);
    CHECK(worldWaveViewShare(f, {0.f, 0.f, 0.f}, {-30.f, 0.f, 0.f}, samples, 0) == 0.f);
    CHECK(worldWaveViewShare(f, {nan, 0.f, 0.f}, {-30.f, 0.f, 0.f}, samples, 3) == 0.f);
}

TEST_CASE("wave look: the cover follows the share at a limited rate, never ahead of it") {
    float cover = 0.f;
    float share = 0.f;
    for (int i = 0; i < 100; ++i) {
        share = std::fmin(1.f, share + 0.03f);  // a share rising 0.03 a frame
        cover = stepWaveCover(cover, share, 1.f / 24.f, 0.5f);
        CHECK(cover <= share + 1e-6f);
    }
    CHECK(cover == doctest::Approx(1.f));
    // A falling share is followed down at the same rate.
    CHECK(stepWaveCover(1.f, 0.f, 0.1f, 2.f) == doctest::Approx(0.8f));
    // Bad steps hold the cover.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(stepWaveCover(0.4f, nan, 0.1f, 1.f) == 0.4f);
    CHECK(stepWaveCover(0.4f, 1.f, -0.1f, 1.f) == 0.4f);
    CHECK(stepWaveCover(0.4f, 1.f, 0.1f, 0.f) == 0.4f);
    CHECK(stepWaveCover(nan, 1.f, 0.1f, 1.f) == doctest::Approx(0.1f));
}

TEST_CASE("wave look: the sky blend moves the light, air and clouds only") {
    SceneLook from;
    from.sunLux = 6.f;
    from.skyLight = 0.7f;
    from.fogDensity = 0.011f;
    from.cloudCover = 0.f;
    from.gradeChroma = 0.82f;
    from.exposureBias = 0.3f;
    from.wetness = 0.6f;
    SceneLook to;
    to.sunLux = 1.5f;
    to.skyLight = 0.6f;
    to.fogDensity = 0.03f;
    to.cloudCover = 1.f;
    to.cloudBaseKm = 1.2f;
    to.gradeChroma = 0.12f;
    to.exposureBias = -0.3f;
    to.wetness = 0.f;

    const SceneLook start = waveSkyStart(from, to);
    CHECK(start.cloudCover == kWaveFairCover);
    CHECK(start.hasClouds());
    CHECK(start.sunLux == from.sunLux);

    const SceneLook half = waveSkyLook(start, to, 0.5f);
    CHECK(half.sunLux == doctest::Approx(3.75f));
    CHECK(half.fogDensity == doctest::Approx(0.0205f));
    CHECK(half.cloudCover == doctest::Approx(0.5f * (kWaveFairCover + 1.f)));
    CHECK(half.cloudBaseKm == doctest::Approx(0.5f * (from.cloudBaseKm + 1.2f)));
    // The grade, camera and surfaces stay the first world's: the post-process changes them where the front is.
    CHECK(half.gradeChroma == from.gradeChroma);
    CHECK(half.exposureBias == from.exposureBias);
    CHECK(half.wetness == from.wetness);

    CHECK(waveSkyLook(start, to, 0.f) == start);
    CHECK(waveSkyLook(start, to, 1.f).sunLux == to.sunLux);
    CHECK(waveSkyLook(start, to, std::numeric_limits<float>::quiet_NaN()) == start);
    // A second world with no cover leaves the first's sky as it was.
    SceneLook clear = to;
    clear.cloudCover = 0.f;
    CHECK(waveSkyStart(from, clear).cloudCover == 0.f);
}

TEST_CASE("wave look: the cloud layer changes only once the view is all one world, and holds still between") {
    // Between the ends the layer holds, whatever the cover does.
    CHECK(stepWaveCloud(0.f, 0.5f, 0.1f, 1.5f) == 0.f);
    CHECK(stepWaveCloud(0.4f, 0.98f, 0.1f, 1.5f) == 0.4f);
    // At full cover it moves toward 1 at its rate; at none, back toward 0.
    CHECK(stepWaveCloud(0.f, 1.f, 0.1f, 1.5f) == doctest::Approx(0.15f));
    CHECK(stepWaveCloud(0.95f, kWaveCloudFull, 0.1f, 1.5f) == 1.f);
    CHECK(stepWaveCloud(0.5f, 0.f, 0.1f, 1.5f) == doctest::Approx(0.35f));
    CHECK(stepWaveCloud(std::nanf(""), 1.f, 0.1f, 1.5f) == doctest::Approx(0.15f));
    CHECK(stepWaveCloud(0.3f, std::nanf(""), 0.1f, 1.5f) == 0.3f);

    SceneLook from;
    from.sunLux = 6.f;
    from.cloudCover = kWaveFairCover;
    SceneLook to = from;
    to.sunLux = 0.75f;
    to.cloudCover = 1.f;
    to.cloudDensity = 0.04f;
    const SceneLook light = waveSkyLook(from, to, 1.f, 0.f);
    CHECK(light.sunLux == to.sunLux);           // the light follows the cover
    CHECK(light.cloudCover == from.cloudCover); // the layer's shape does not
    CHECK(light.cloudDensity == from.cloudDensity);
    const SceneLook both = waveSkyLook(from, to, 0.5f, 1.f);
    CHECK(both.sunLux == doctest::Approx(3.375f));
    CHECK(both.cloudCover == 1.f);
    CHECK(waveSkyLook(from, to, 0.3f) == waveSkyLook(from, to, 0.3f, 0.3f));
}

TEST_CASE("wave look: the gains lift the unreached side by the light lost, and never bring the second world back up") {
    SceneLook from;
    from.sunLux = 6.f;
    from.skyLight = 0.5f;  // key 8
    SceneLook now = from;
    now.sunLux = 0.f;      // key 2: two stops lost
    const WaveGains g = waveGains(from, now);
    CHECK(g.outside == doctest::Approx(4.f));
    CHECK(g.inside == 1.f);
    const WaveGains same = waveGains(from, from);
    CHECK(same.outside == 1.f);
    CHECK(same.inside == 1.f);
    SceneLook dark = from;
    dark.sunLux = 0.f;
    dark.skyLight = 0.f;
    CHECK(waveGains(from, dark).outside == 1.f);  // no light to scale by: unchanged
}

TEST_CASE("wave look: a real-time sky capture's fill is lifted by the sun lost, within its cap") {
    SceneLook from;
    from.sunLux = 6.f;
    from.skyLight = 0.7f;
    SceneLook now = from;
    now.sunLux = 0.375f;  // 16 x less sun
    now.skyLight = 0.6f;
    CHECK(waveCaptureSkyLight(from, now) == doctest::Approx(0.6f * 16.f));
    CHECK(waveCaptureSkyLight(from, from) == doctest::Approx(0.7f));  // nothing lost: as the look has it
    SceneLook brighter = now;
    brighter.sunLux = 12.f;
    CHECK(waveCaptureSkyLight(from, brighter) == doctest::Approx(0.6f));  // never taken down
    SceneLook tiny = now;
    tiny.sunLux = 0.01f;
    CHECK(waveCaptureSkyLight(from, tiny) == doctest::Approx(0.6f * kWaveCaptureLiftMax));
    SceneLook dark = now;
    dark.sunLux = 0.f;
    CHECK(waveCaptureSkyLight(from, dark) == doctest::Approx(0.6f));  // no sun to scale by: unchanged
    SceneLook bad = now;
    bad.skyLight = std::nanf("");
    CHECK(waveCaptureSkyLight(from, bad) == doctest::Approx(0.7f));
}

TEST_CASE("wave look: no allocation") {
    const WaveViewSample samples[] = {hitAt(1.f, 0.f, 0.f), skyAlong(1.f, 0.f, 0.f)};
    SceneLook a;
    SceneLook b;
    b.cloudCover = 1.f;
    const auto before = ftcore_test::allocStats().count;
    float sum = 0.f;
    for (int i = 0; i < 50; ++i) {
        sum += worldWaveViewShare(frameAt(10.f, 2.f), {0.f, 0.f, 0.f}, {-20.f, 0.f, 0.f}, samples, 2);
        sum += stepWaveCover(0.2f, 0.5f, 0.04f, 1.f);
        sum += waveSkyLook(waveSkyStart(a, b), b, 0.3f).cloudCover;
        sum += waveSkyLook(a, b, 0.3f, 0.6f).cloudCover + stepWaveCloud(0.2f, 1.f, 0.04f, 1.5f);
        sum += waveGains(a, b).outside;
    }
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(sum > 0.f);
}
