#pragma once

// The world wave's light and air: how far the second world's sky, sun and fog have come in, and how the
// post-process keeps the change to the pixels the front has reached.
//
// The front reaches the ground point by point, but the sun, the sky light, the fog and the cloud layer are one each for
// the whole view. So they cross over with the share of the view the front has covered, never ahead of it:
//   worldWaveViewShare  the share of a set of view samples that show the second world. A sample is a ray from the eye:
//                       where it hit something, the hit point is in the second world when it is inside the front's middle
//                       (d < radius - band / 2) for a Forward wave, outside it for Reverse; a ray that hit nothing (the
//                       sky) is in the second world when it passes through the sphere of that radius (the eye inside it
//                       counts) for Forward, when it misses it for Reverse. Before the front moves (radius 0) nothing is
//                       in the second world going forward and everything is in reverse.
//   stepWaveCover       the cover the host blends by: it follows the share at no more than `rate` a second either way,
//                       so a rising share is lagged, never led.
//   waveSkyStart        the look a wave starts from: the first world's, but when the second world has cloud cover and
//                       the first has none, with kWaveFairCover cover and the cloud layer on, so the layer's material
//                       swap (no cover: the engine's clouds; any cover: the world's) happens once at the start and the
//                       cover then grows smoothly.
//   waveSkyLook         from blended toward to by t in the light, air, cloud and wind fields only; every camera, grade,
//                       surface, water, practical, glow, sky-dome and sea field stays as from has it, since the per-pixel
//                       post-process makes those changes where the front is (a whole-screen change would pop). The
//                       four-argument form blends the cloud fields by their own cloudT instead (stepWaveCloud).
//   stepWaveCloud       the share the cloud layer's shape fields (cover, type, base, depth, albedo, density, drift,
//                       underglow) blend by: it moves at `rate` a second toward 1 only once the cover is at least
//                       kWaveCloudFull, toward 0 only at kWaveCloudNone or less, and holds between. The layer changes
//                       where no pixel shows it (the post-process paints the second world's sky by the front), because a
//                       volumetric layer whose shape changes every frame never settles its temporal reconstruction and
//                       shows a dot pattern; a layer that holds still settles.
//   waveKeyLight       a look's key light, sunLux + kWaveSkyWeight * skyLight (the scenes' lux scale).
//   waveCaptureSkyLight the sky light's intensity to write when it captures the sky in real time: that capture already
//                       dims with the sun, so a look's skyLight (the fill as a share of the first world's sky) is lifted
//                       by the sun the first world has lost (from.sunLux / now.sunLux, 1 to kWaveCaptureLiftMax). Without
//                       it the dimmer sun takes the fill down twice and the settled second world goes black.
//   waveGains           the post-process gains for a blended look: the side the front has not reached is lifted by the
//                       key light the first world has lost (it keeps its own light); the second world's side is never
//                       brought back up (inside 1): its exposure stays the first world's, so less light reads darker, and
//                       its mood comes from the post-process grade. Both 1 when nothing is lost.
//
// Plain value maths, no allocation; bad (non-finite) input never moves a value.

#include "ftcore/Api.h"
#include "ftcore/SceneLook.h"
#include "ftcore/WorldWave.h"

namespace ftcore {

struct WaveViewSample {
    WaveVec3 point;   // metres: where the ray hit, when hit
    WaveVec3 dir;     // the ray's direction (need not be unit length): used when it hit nothing
    bool hit = false;
};

FTCORE_API float worldWaveViewShare(const WorldWaveFrame& frame, const WaveVec3& origin, const WaveVec3& eye,
                                    const WaveViewSample* samples, int count);

FTCORE_API float stepWaveCover(float cover, float share, float dt, float rate);

inline constexpr float kWaveFairCover = 0.35f;
inline constexpr float kWaveSkyWeight = 4.f;

FTCORE_API SceneLook waveSkyStart(const SceneLook& from, const SceneLook& to);
FTCORE_API SceneLook waveSkyLook(const SceneLook& from, const SceneLook& to, float t);
FTCORE_API SceneLook waveSkyLook(const SceneLook& from, const SceneLook& to, float t, float cloudT);

inline constexpr float kWaveCloudFull = 0.99f;
inline constexpr float kWaveCloudNone = 0.01f;
FTCORE_API float stepWaveCloud(float cloudT, float cover, float dt, float rate);
FTCORE_API float waveKeyLight(const SceneLook& look);

inline constexpr float kWaveCaptureLiftMax = 32.f;
FTCORE_API float waveCaptureSkyLight(const SceneLook& from, const SceneLook& now);

struct WaveGains {
    float outside = 1.f;  // the first world's side
    float inside = 1.f;   // the second world's side
};
FTCORE_API WaveGains waveGains(const SceneLook& from, const SceneLook& now);

} // namespace ftcore
