#pragma once

// The scene's photographic base: the sun, sky, fog and clouds and the camera finish (exposure, grade, lens)
// that every map builder sets up through FTScene::SpawnLook (Source/ForgottenThird/FTSceneLook.h). One look
// for every world for now; worlds get their own later, so a look is plain data and two blend field by field.
// See docs/SCENE_LOOK.md.
//
// Units are the engine's where it has one: lux and Kelvin for the sun, EV for exposure, and the height fog's
// own density and falloff scales. The scenes run auto exposure, so the sun's lux sets only its ratio to the
// local lights and emissive effects, which are tuned at this scale (a few lux, not daylight's tens of
// thousands).
//
// Plain value maths, no allocation: safe to call every frame.

#include "ftcore/Api.h"

namespace ftcore {

// X-macro of every field: (name, default). Keeps ==, lerp and the tests in sync. Every field is a float.
#define FTCORE_LOOK_FIELDS(X)                                                                                  \
    /* Light. */                                                                                               \
    X(sunLux, 6.f)                /* directional light illuminance */                                          \
    X(sunKelvin, 5800.f)          /* the sun's colour before the atmosphere reddens it by elevation; the  */   \
                                  /* look's one warm cast, so whiteBalanceKelvin stays neutral beside it  */   \
    X(skyLight, 1.f)              /* sky light intensity (a real-time capture of the atmosphere) */            \
    /* Air. */                                                                                                 \
    X(fogDensity, 0.03f)          /* exponential height fog density (the engine's scale; its default 0.02) */  \
    X(fogFalloff, 0.2f)           /* height falloff (the engine's default 0.2): larger thins it faster */      \
    X(volumetricFog, 1.f)         /* volumetric fog extinction scale; 0 = off, the plain height fog only */    \
    X(fogForward, 0.6f)           /* volumetric fog phase g, -0.9..0.9: > 0 makes it glow toward the sun */   \
    X(clouds, 1.f)                /* 0.5 or more: the engine's default volumetric cloud layer; less: clear */  \
    /* Camera. */                                                                                              \
    X(exposureBias, 1.f)          /* exposure compensation, EV (the project default is 1) */                   \
    X(exposureMinEV100, -10.f)    /* auto exposure range, EV100 (the engine's extended-range defaults) */      \
    X(exposureMaxEV100, 20.f)                                                                                  \
    X(whiteBalanceKelvin, 6500.f) /* 6500 = neutral */                                                         \
    X(saturation, 1.f)            /* 1 = unchanged */                                                          \
    X(contrast, 1.f)              /* 1 = unchanged */                                                          \
    X(bloom, 0.675f)              /* bloom intensity (the engine's default 0.675) */                           \
    X(vignette, 0.4f)             /* vignette intensity (the engine's default 0.4) */                          \
    X(filmGrain, 0.f)             /* film grain intensity */

struct SceneLook {
#define FTCORE_LOOK_DECL(name, def) float name = def;
    FTCORE_LOOK_FIELDS(FTCORE_LOOK_DECL)
#undef FTCORE_LOOK_DECL

    friend bool operator==(const SceneLook& a, const SceneLook& b) {
        bool eq = true;
#define FTCORE_LOOK_EQ(name, def) eq = eq && a.name == b.name;
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
// lerp(a, a, t) == a for every t.
FTCORE_API SceneLook lerp(const SceneLook& a, const SceneLook& b, float t);

} // namespace ftcore
