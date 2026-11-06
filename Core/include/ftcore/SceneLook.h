#pragma once

// The scene's photographic base: the sun, sky, fog and clouds and the camera finish (exposure, grade, lens)
// that every map builder sets up through FTScene::SpawnLook (Source/ForgottenThird/FTSceneLook.h), and the
// settings of the after-tonemap grade (LookGrade.h). A look is plain data: each world has its own in
// Data/Look/looks.json (LookTable.h), and two blend field by field. See docs/SCENE_LOOK.md.
//
// Units are the engine's where it has one: lux and Kelvin for the sun, EV for exposure, and the height fog's
// own density and falloff scales. The scenes run auto exposure, so the sun's lux sets only its ratio to the
// local lights and emissive effects, which are tuned at this scale (a few lux, not daylight's tens of
// thousands).
//
// Plain value maths, no allocation: safe to call every frame.

#include "ftcore/Api.h"

#include <cstddef>

namespace ftcore {

// X-macro of every field: (name, default, lo, hi). Keeps ==, lerp, the table importer and the tests in sync.
// Every field is a float. lo..hi is the range the look table accepts (Data/Look/looks.json); the engine's own
// range is wider for some. Every default is the engine's or the project config's value, so a default look
// renders as before. Hue fields are OKLab degrees (red ~30, gold ~85, green ~145, blue ~265, violet ~305) and
// lerp linearly like the rest, so a blend between worlds should keep their hues close. The surface, water, fog
// albedo, practical light, glow, sky, wind, cloud and sea fields drive materials and lights the maps carry; their
// defaults add no wetness, glow, stars, skyglow, wind or swell, keep the engine's own cloud layer, and leave the water,
// fog and lights as they were.
#define FTCORE_LOOK_FIELDS(X)                                                                                  \
    /* Light. */                                                                                               \
    X(sunLux, 6.f, 0.f, 200.f)    /* directional light illuminance */                                          \
    X(sunKelvin, 5800.f, 1500.f, 15000.f) /* the sun's colour before the atmosphere reddens it by elevation; */ \
                                  /* the look's one warm cast, so whiteBalanceKelvin stays neutral beside it */ \
    X(skyLight, 1.f, 0.f, 10.f)   /* sky light intensity (a real-time capture of the atmosphere) */            \
    /* Air. */                                                                                                 \
    X(fogDensity, 0.03f, 0.f, 1.f)   /* exponential height fog density (the engine's scale; its default 0.02) */ \
    X(fogFalloff, 0.2f, 0.001f, 2.f) /* height falloff (the engine's default 0.2): larger thins it faster */ \
    X(volumetricFog, 1.f, 0.f, 10.f) /* volumetric fog extinction scale; 0 = off, the plain height fog only */ \
    X(fogForward, 0.6f, -0.9f, 0.9f) /* volumetric fog phase g: > 0 makes it glow toward the sun */          \
    X(clouds, 1.f, 0.f, 1.f)      /* 0.5 or more: the volumetric cloud layer (see cloudCover); less: clear */  \
    /* Camera. */                                                                                              \
    X(exposureBias, 1.f, -15.f, 15.f)       /* exposure compensation, EV (the project default is 1) */        \
    X(exposureMinEV100, -10.f, -15.f, 25.f) /* auto exposure range, EV100 (the engine's extended range) */    \
    X(exposureMaxEV100, 20.f, -15.f, 25.f)                                                                     \
    X(exposureSpeedUp, 3.f, 0.02f, 20.f)    /* auto exposure adaption speed, toward brighter (engine default) */ \
    X(exposureSpeedDown, 1.f, 0.02f, 20.f)  /* ... toward darker (engine default) */                          \
    X(whiteBalanceKelvin, 6500.f, 1500.f, 15000.f) /* 6500 = neutral */                                       \
    X(saturation, 1.f, 0.f, 2.f)  /* 1 = unchanged */                                                          \
    X(contrast, 1.f, 0.f, 2.f)    /* 1 = unchanged */                                                          \
    X(bloom, 0.675f, 0.f, 8.f)    /* bloom intensity (the engine's default 0.675) */                           \
    X(vignette, 0.4f, 0.f, 1.f)   /* vignette intensity (the engine's default 0.4) */                          \
    X(filmGrain, 0.f, 0.f, 1.f)   /* film grain intensity */                                                   \
    X(filmGrainShadows, 1.f, 0.f, 4.f)    /* grain scale in the shadows (engine default) */                   \
    X(filmGrainHighlights, 1.f, 0.f, 4.f) /* ... in the highlights (engine default) */                        \
    /* The filmic tonemapper's curve (the engine's defaults) and the local exposure's contrasts (the */        \
    /* project's own 0.8, where the engine's is 1). */                                                         \
    X(filmSlope, 0.88f, 0.f, 1.f)                                                                              \
    X(filmToe, 0.55f, 0.f, 1.f)                                                                                \
    X(filmShoulder, 0.26f, 0.f, 1.f)                                                                           \
    X(filmBlackClip, 0.f, 0.f, 1.f)                                                                            \
    X(filmWhiteClip, 0.04f, 0.f, 1.f)                                                                          \
    X(localHighlightContrast, 0.8f, 0.f, 1.f)                                                                  \
    X(localShadowContrast, 0.8f, 0.f, 1.f)                                                                     \
    X(fringe, 0.f, 0.f, 5.f)      /* chromatic aberration intensity */                                         \
    /* The grade after the tonemap (LookGrade.h), in OKLab. A window whose width is 0 is off. */               \
    X(gradeChroma, 1.f, 0.f, 2.f)  /* global chroma scale; 1 = unchanged */                                    \
    X(skinHue, 55.f, 0.f, 360.f)   /* the skin protection window's centre... */                                \
    X(skinWidth, 25.f, 0.f, 180.f) /* ... and half-width */                                                   \
    X(skinFloor, 0.f, 0.f, 1.5f)   /* the least chroma scale skin tones keep; 0 = no protection */             \
    X(keepAHue, 0.f, 0.f, 360.f)   /* a hue window that takes its own chroma scale instead (width 0 = off) */  \
    X(keepAWidth, 0.f, 0.f, 180.f)                                                                             \
    X(keepAChroma, 1.f, 0.f, 2.f)                                                                              \
    X(keepBHue, 0.f, 0.f, 360.f)                                                                               \
    X(keepBWidth, 0.f, 0.f, 180.f)                                                                             \
    X(keepBChroma, 1.f, 0.f, 2.f)                                                                              \
    X(shadowTintHue, 0.f, 0.f, 360.f)    /* split tone: the shadows lean toward this hue... */                 \
    X(shadowTintChroma, 0.f, 0.f, 0.05f) /* ... by this much OKLab chroma at black */                         \
    X(highlightTintHue, 0.f, 0.f, 360.f)                                                                       \
    X(highlightTintChroma, 0.f, 0.f, 0.05f)                                                                    \
    /* Surfaces. */                                                                                            \
    X(wetness, 0.f, 0.f, 1.f) /* ground and stone wetness: darker, smoother, puddles past 0.5 */               \
    /* Water (Single Layer Water). Coefficients are per metre here; the material converts to per cm. */        \
    X(waterAbsorbR, 0.45f, 0.f, 50.f) /* absorption per metre: how fast each colour dies with depth */         \
    X(waterAbsorbG, 0.065f, 0.f, 50.f)                                                                         \
    X(waterAbsorbB, 0.025f, 0.f, 50.f)                                                                         \
    X(waterScatterR, 0.02f, 0.f, 50.f) /* scattering per metre: the colour the water gives back */             \
    X(waterScatterG, 0.03f, 0.f, 50.f)                                                                         \
    X(waterScatterB, 0.04f, 0.f, 50.f)                                                                         \
    X(waterPhaseG, 0.f, -0.9f, 0.9f) /* the water's scattering phase g */                                      \
    X(waterRoughness, 0.04f, 0.f, 1.f)                                                                         \
    X(waterWaveCm, 150.f, 10.f, 5000.f) /* the first normal layer's cell size, cm; the second is finer */      \
    X(waterWaveSpeed, 1.f, 0.f, 10.f) /* scale on the layers' pan speed; 0 = frozen */                         \
    X(waterWaveStrength, 1.f, 0.f, 2.f) /* normal strength; 0 = a still mirror */                              \
    /* Air, continued. */                                                                                      \
    X(fogAlbedoR, 1.f, 0.f, 1.f) /* volumetric fog albedo (1 = white, the engine's default) */                 \
    X(fogAlbedoG, 1.f, 0.f, 1.f)                                                                               \
    X(fogAlbedoB, 1.f, 0.f, 1.f)                                                                               \
    /* Practical lights (lights tagged FTPractical in a map): interior lamps, signs, fires. */                 \
    X(practicalKelvin, 3200.f, 1500.f, 15000.f)                                                                \
    X(sodiumKelvin, 2000.f, 1500.f, 15000.f) /* the colour of those also tagged FTSodium, the */               \
                                             /* sodium street lamps (high-pressure sodium) */                  \
    X(practicalCandela, 40.f, 0.f, 10000.f)                                                                    \
    X(practicalScatter, 1.f, 0.f, 20.f) /* volumetric scattering intensity (the engine's default 1) */         \
    /* Glow: emissive light sources. */                                                                        \
    X(glowR, 0.3f, 0.f, 1.f) /* the glow's colour */                                                           \
    X(glowG, 0.55f, 0.f, 1.f)                                                                                  \
    X(glowB, 1.f, 0.f, 1.f)                                                                                    \
    X(glowStrength, 0.f, 0.f, 200.f) /* emissive multiplier at the scenes' exposure scale (sun: a few lux); */ \
                                     /* 0 = dark */                                                            \
    /* The sky (M_Env_Sky, through the environment's collection MPC_Env). The defaults leave the sky as the */ \
    /* atmosphere draws it. */                                                                                 \
    X(skyStars, 0.f, 0.f, 20.f)    /* a magnitude-0 star's core luminance at the scene scale; 0 = no stars */  \
    X(skyStarMag, 4.f, 1.f, 7.f)   /* limiting magnitude: about 3 under a lit city, 6 under a dark sky */      \
    X(skyGlow, 0.f, 0.f, 10.f)     /* a city's skyglow at the horizon toward it, scene scale; 0 = none */      \
    X(skyGlowKelvin, 2100.f, 1500.f, 15000.f) /* its colour (2100 = high-pressure sodium) */                   \
    X(skyGlowDeg, 0.f, 0.f, 360.f) /* toward the city, degrees like a yaw (0 = +X, 90 = +Y) */                 \
    X(skyGlowWidthDeg, 20.f, 1.f, 90.f) /* the band falls by e every this many degrees of elevation */         \
    X(skyFlat, 0.f, 0.f, 1.f)      /* 1 = a dome of skyFlatLuminance with no feature or disc replaces it */    \
    X(skyFlatLuminance, 1.f, 0.f, 50.f) /* that dome's luminance at the scene scale */                         \
    /* Wind, shared by the clouds and the sea. */                                                              \
    X(windDeg, 0.f, 0.f, 360.f)    /* the way the wind blows, degrees like a yaw (0 = toward +X) */            \
    X(windMps, 0.f, 0.f, 40.f)     /* its speed near the ground, metres a second */                            \
    /* The clouds (M_Env_Clouds on the volumetric cloud layer, through MPC_Env). With no cover the layer */     \
    /* keeps the engine's own material; the base and depth move whichever material it wears. */                \
    X(cloudCover, 0.f, 0.f, 1.f)   /* share of the sky under cloud; above 0 the layer wears M_Env_Clouds */    \
    X(cloudType, 0.5f, 0.f, 1.f)   /* 0 = flat stratus sheets .. 1 = tall heaped cumulus */                    \
    X(cloudBaseKm, 5.f, 0.f, 20.f) /* the layer's bottom, km above the ground (the engine's default 5) */      \
    X(cloudThickKm, 10.f, 0.1f, 20.f) /* the layer's depth, km (the engine's default 10) */                    \
    X(cloudAlbedo, 1.f, 0.f, 1.f)  /* single-scattering albedo: 1 = water cloud, about 0.4 = soot smoke */     \
    X(cloudDensity, 0.05f, 0.f, 1.f) /* extinction inside a full cloud, per metre */                           \
    X(cloudDrift, 2.f, 0.f, 5.f)   /* the clouds' speed against windMps (the wind blows harder aloft) */       \
    X(cloudUnderglow, 0.f, 0.f, 10.f) /* a city's light on the cloud bases, luminance at the scene scale */    \
    X(cloudUnderglowKelvin, 2100.f, 1500.f, 15000.f) /* its colour (2100 = high-pressure sodium) */            \
    /* The sea (M_Env_Sea, through MPC_Env): a swell from far off and the wind's own sea, which */             \
    /* ftcore::seaWaves builds. The defaults raise no swell, and with no wind no sea. */                       \
    X(seaSwellM, 0.f, 0.f, 8.f)    /* the swell's significant height, metres; 0 = none */                      \
    X(seaSwellPeriodS, 8.f, 4.f, 20.f) /* its period, seconds (8 s: crests about 100 m apart) */               \
    X(seaSwellDeg, 0.f, 0.f, 360.f) /* the way it travels, degrees like a yaw */                               \
    X(seaChop, 1.f, 0.f, 3.f)      /* scale on the sea windMps raises over seaFetchKm; 0 = none */             \
    X(seaFetchKm, 2.f, 0.1f, 500.f) /* open water upwind, km: a harbour's few give short, steep chop */        \
    X(seaFoam, 0.5f, 0.f, 1.f)     /* whitecaps where the crests crowd, and contact foam; 0 = none */          \
    X(seaFoamBandCm, 60.f, 0.f, 500.f) /* contact foam where the water is shallower than this along the view */

struct SceneLook {
#define FTCORE_LOOK_DECL(name, def, lo, hi) float name = def;
    FTCORE_LOOK_FIELDS(FTCORE_LOOK_DECL)
#undef FTCORE_LOOK_DECL

    friend bool operator==(const SceneLook& a, const SceneLook& b) {
        bool eq = true;
#define FTCORE_LOOK_EQ(name, def, lo, hi) eq = eq && a.name == b.name;
        FTCORE_LOOK_FIELDS(FTCORE_LOOK_EQ)
#undef FTCORE_LOOK_EQ
        return eq;
    }
    friend bool operator!=(const SceneLook& a, const SceneLook& b) { return !(a == b); }

    // The two switches the engine has no in-between for.
    bool hasVolumetricFog() const { return volumetricFog > 0.f; }
    bool hasClouds() const { return clouds >= 0.5f; }
};

// Field-wise linear blend; t is clamped to [0, 1]. lerp(a, b, 0) == a and lerp(a, b, 1) == b exactly, and
// lerp(a, a, t) == a for every t. Hue fields blend linearly too, with no shortest-arc wrap.
FTCORE_API SceneLook lerp(const SceneLook& a, const SceneLook& b, float t);

// Every field by name with its default and accepted range: what the look table reads.
struct SceneLookField {
    const char* name;
    float SceneLook::*member;
    float def, lo, hi;
};
inline constexpr SceneLookField kSceneLookFields[] = {
#define FTCORE_LOOK_INFO(name, def, lo, hi) {#name, &SceneLook::name, def, lo, hi},
    FTCORE_LOOK_FIELDS(FTCORE_LOOK_INFO)
#undef FTCORE_LOOK_INFO
};
inline constexpr std::size_t kSceneLookFieldCount = sizeof(kSceneLookFields) / sizeof(kSceneLookFields[0]);

} // namespace ftcore
