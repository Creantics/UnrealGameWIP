#pragma once

// World "style packs" (NDD 8.1.2 / 8.15): one shader stack, four parameter sets. Ported from newengine's
// engine/include/ne/render/StylePack.h, which stays the source of truth for the values; see
// docs/reference/style_packs.md for what each field does and docs/STYLE_PACKS_UE.md for how Unreal uses it.
//
// A pack is plain data (floats and colours), so two packs blend field by field: that is how a scene sits
// between worlds (the Style Dial). StyleSettings{} (== stylePack(StylePack::None)) is the identity.
// Colours are display-space sRGB values (the style stage runs after tonemapping). World positions and
// distances are newengine world units (metres, Y up); the Unreal wrapper converts them.
//
// Nothing here allocates except parseStyleSpec's callers' strings: lerp, styleFromDial and the packs are
// plain value maths and are safe to call every frame.

#include "ftcore/Api.h"

#include <optional>
#include <string_view>

namespace ftcore {

// An RGB colour or a world position: three floats, no maths library.
struct StyleVec3 {
    float x = 0.f, y = 0.f, z = 0.f;

    constexpr StyleVec3() = default;
    constexpr explicit StyleVec3(float v) : x(v), y(v), z(v) {}
    constexpr StyleVec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    friend constexpr bool operator==(const StyleVec3& a, const StyleVec3& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
    friend constexpr bool operator!=(const StyleVec3& a, const StyleVec3& b) { return !(a == b); }
    friend constexpr StyleVec3 operator+(const StyleVec3& a, const StyleVec3& b) {
        return {a.x + b.x, a.y + b.y, a.z + b.z};
    }
    friend constexpr StyleVec3 operator-(const StyleVec3& a, const StyleVec3& b) {
        return {a.x - b.x, a.y - b.y, a.z - b.z};
    }
    friend constexpr StyleVec3 operator*(const StyleVec3& a, float k) { return {a.x * k, a.y * k, a.z * k}; }
};

enum class StylePack { None, Solhaven, Lattice, Unwritten, Restored };
inline constexpr int kStylePackCount = 5;

// X-macro of every blendable field: (name, type, default). Keeps ==, lerp and the dial blend in sync.
#define FTCORE_STYLE_FIELDS(X)                                                                                 \
    /* Ink outline (screen-space, from depth). */                                                             \
    X(outline, float, 0.f)            /* line opacity 0..1 (0 = no outline) */                                 \
    X(outlineWidth, float, 1.5f)      /* line half-width in pixels at 1080p (scaled by output height) */       \
    X(outlineColor, StyleVec3, StyleVec3(0.06f))                                                               \
    X(outlineThreshold, float, 0.05f) /* depth-curvature threshold; lower = more crease lines */              \
    X(outlineTaper, float, 0.f)       /* 0 = constant width; 1 = heavier near, thinner far, brush wobble */   \
    X(outlineJitter, float, 0.f)      /* hand tremble amplitude in pixels at 1080p, re-drawn ~8 times/s */    \
    /* Cel / toon (lit shader). */                                                                             \
    X(cel, float, 0.f)                /* 0 = smooth light, 1 = fully banded */                                 \
    X(celSteps, float, 3.f)           /* number of light bands */                                              \
    /* Colour. */                                                                                              \
    X(saturation, float, 1.f)                                                                                  \
    X(shadowTint, StyleVec3, StyleVec3(1.f))    /* multiply on the darks */                                    \
    X(highlightTint, StyleVec3, StyleVec3(1.f)) /* multiply on the lights */                                   \
    X(blackPoint, float, 0.f)         /* 0..1: display values below it clip to black, the rest re-stretch */   \
    X(monochrome, float, 0.f)         /* 0..1: remap luminance onto inkColor -> paperColor */                  \
    X(monoGamma, float, 1.f)          /* tone curve of the remap */                                            \
    X(paperColor, StyleVec3, StyleVec3(0.949f, 0.937f, 0.910f)) /* NDD 8.2.3 Paper #F2EFE8 */                  \
    X(inkColor, StyleVec3, StyleVec3(0.043f, 0.043f, 0.047f))   /* NDD 8.2.3 Ink #0B0B0C */                    \
    X(halftone, float, 0.f)           /* 0..1: screen-tone dots replace continuous tone */                     \
    X(halftonePitch, float, 6.f)      /* dot pitch in pixels at 1080p, grid rotated 45 degrees */              \
    X(paperSky, float, 0.f)           /* 0..1: background (no geometry) becomes blank paper */                 \
    X(paperGrain, float, 0.f)         /* multiply strength of the paper tooth */                               \
    /* Multipliers on the post finish (1 = unchanged). */                                                      \
    X(bloomScale, float, 1.f)                                                                                  \
    X(vignetteScale, float, 1.f)                                                                               \
    X(filmGrainScale, float, 1.f)                                                                              \
    X(chromaScale, float, 1.f)                                                                                 \
    /* Colour-authority island (NDD 8.15.5, single disc). */                                                   \
    X(island, float, 0.f)             /* 0..1: colour kept inside the disc */                                  \
    X(islandCenter, StyleVec3, StyleVec3(0.f)) /* world position; the disc is measured in the ground plane */  \
    X(islandRadius, float, 2.f)       /* world units */                                                        \
    X(islandEdge, float, 2.5f)        /* inked border width in pixels at 1080p */                              \
    X(islandInk, StyleVec3, StyleVec3(0.043f, 0.043f, 0.047f))                                                \
    /* Inverted-hull silhouette (geometry pass). */                                                            \
    X(hull, float, 0.f)               /* 0..1 strength (0 = no hull pass) */                                   \
    X(hullWidth, float, 2.f)          /* band width in pixels at 1080p */                                      \
    X(hullColor, StyleVec3, StyleVec3(0.06f))                                                                  \
    X(hullStatic, float, 0.f)         /* width multiplier for static meshes (0 = characters only) */           \
    /* Tonal-art-map hatching. */                                                                              \
    X(hatch, float, 0.f)              /* 0..1: shading becomes ink strokes */                                  \
    X(hatchScale, float, 150.f)       /* hatch tile size in pixels at 1080p (stroke pitch = scale / 16) */     \
    X(hatchDensity, float, 1.f)       /* > 1 hatches more of the mid-tones, < 1 keeps more paper */            \
    X(hatchInk, StyleVec3, StyleVec3(0.043f, 0.043f, 0.047f))                                                 \
    X(hatchOverlay, float, 0.f)       /* 0 = strokes on a paper-light base; 1 = strokes over the lit colour */ \
    X(hatchMaxLevel, float, 5.f)      /* cap on the stroke level 0..5 (2 = one direction only) */              \
    X(hatchShadowOnly, float, 0.f)    /* 0..1: strokes only in the lowest cel band */                          \
    X(hatchCurve, float, 1.f)         /* exponent on darkness before picking the level */                      \
    /* Pack rim on characters. */                                                                              \
    X(rimScale, float, 0.f)           /* scene-referred rim strength at the silhouette (0 = none) */           \
    X(rimTint, StyleVec3, StyleVec3(1.f))                                                                      \
    X(bloomThresholdScale, float, 1.f) /* > 1 keeps the glow to emissives */                                   \
    /* Ink void (per scene, 0 in every pack). */                                                               \
    X(inkVoid, float, 0.f)            /* 0..1 strength */                                                      \
    X(inkVoidNear, float, 8.f)        /* view distance (world units) where the fade starts */                  \
    X(inkVoidFar, float, 30.f)        /* view distance where geometry is fully ink */                          \
    /* Faces: only materials with a face role. */                                                              \
    X(faceFeatures, float, 0.f)       /* 0..1: features keep their own value (no paper base, no strokes) */    \
    X(faceContrast, float, 0.f)       /* feature value boost, scaled by faceFeatures */                        \
    X(faceHatch, float, 1.f)          /* hatch density multiplier on face skin */                              \
    X(faceHatchMaxLevel, float, 5.f)  /* stroke level cap on face skin */                                      \
    X(faceSkinLit, float, 0.f)        /* 0..1: share of face skin's lit shading kept under its strokes */      \
    X(faceSnap, float, 0.f)           /* > 0: morph weights snap to this many steps; 0 = off */

struct StyleSettings {
#define FTCORE_STYLE_DECL(name, type, def) type name = def;
    FTCORE_STYLE_FIELDS(FTCORE_STYLE_DECL)
#undef FTCORE_STYLE_DECL

    friend bool operator==(const StyleSettings& a, const StyleSettings& b) {
        bool eq = true;
#define FTCORE_STYLE_EQ(name, type, def) eq = eq && a.name == b.name;
        FTCORE_STYLE_FIELDS(FTCORE_STYLE_EQ)
#undef FTCORE_STYLE_EQ
        return eq;
    }
    friend bool operator!=(const StyleSettings& a, const StyleSettings& b) { return !(a == b); }

    // Anything that changes the image. False for the identity (None). The hatch modifiers and the face
    // fields only shape the hatching, so on their own they do not count.
    bool active() const {
        return outline > 0.f || cel > 0.f || monochrome > 0.f || halftone > 0.f || paperSky > 0.f || paperGrain > 0.f ||
               island > 0.f || hull > 0.f || hatch > 0.f || saturation != 1.f || shadowTint != StyleVec3(1.f) ||
               highlightTint != StyleVec3(1.f) || bloomScale != 1.f || vignetteScale != 1.f || filmGrainScale != 1.f ||
               chromaScale != 1.f || rimScale > 0.f || bloomThresholdScale != 1.f || inkVoid > 0.f ||
               blackPoint > 0.f;
    }
    // The style stage needs the scene depth (outline edges, paper sky, island position, ink void distance).
    bool needsDepth() const { return outline > 0.f || paperSky > 0.f || island > 0.f || inkVoid > 0.f; }
};

// Tonal art map lookup weights (the post shader's hatching mirrors this law). Four nested stroke levels
// (tam[0..3] = levels 1..4) plus solid ink (level 5); `tone` is display-space brightness 0..1. The darkness
// (1 - tone)^curve * density * 5, capped at maxLevel, picks a position between two neighbouring levels.
struct HatchWeights {
    float tam[4] = {0.f, 0.f, 0.f, 0.f};
    float solid = 0.f;
};
FTCORE_API HatchWeights hatchWeights(float tone, float density = 1.f, float curve = 1.f, float maxLevel = 5.f);

// The parameter set of each world (v2 values).
FTCORE_API StyleSettings stylePack(StylePack pack);
// "none", "solhaven", "lattice", "unwritten", "restored". Static storage; never null.
FTCORE_API const char* stylePackName(StylePack pack);
// Case-insensitive pack name.
FTCORE_API std::optional<StylePack> parseStylePack(std::string_view name);

// Field-wise linear blend; t is clamped to [0, 1]. lerp(a, b, 0) == a and lerp(a, b, 1) == b exactly.
// Island placement and the ink void distances follow the side that has an island / a void. The face fields
// are weighted by each side's hatch as well as by the blend weight.
FTCORE_API StyleSettings lerp(const StyleSettings& a, const StyleSettings& b, float t);

// The Style Dial (NDD 8.1.2): four needles 0..3, S (Solhaven), L (Lattice), U (Unwritten), R (Restored).
struct StyleDial {
    float s = 0.f, l = 0.f, u = 0.f, r = 0.f;
};

// Weighted blend of the four packs. Needles are clamped to [0, 3]; weight_i = needle_i / max(sum, 3), the
// rest goes to None. One needle at 3 is exactly that pack; S3 U1 is 3/4 Solhaven and 1/4 Unwritten.
FTCORE_API StyleSettings styleFromDial(const StyleDial& dial);

// "S3,L1,U0,R0" (any order, letters case-insensitive, missing needles are 0, optional '=' or ':') or four
// bare numbers "3,1,0,0" in S,L,U,R order. Separators: ',', ' ', ';' or '/'. Values must be within 0..3.
FTCORE_API std::optional<StyleDial> parseStyleDial(std::string_view text);

// Ink Solhaven (art direction v2 §5): the dial S1 U3 with saturation 1, monochrome 0.8, a hard two-band cel
// and the paper sky. A preset, not a StylePack.
FTCORE_API StyleSettings inkSolhavenStyle();

// Command-line / config form: a pack name ("unwritten"), "inksolhaven" or a dial ("S3,U0.5").
FTCORE_API std::optional<StyleSettings> parseStyleSpec(std::string_view text);

} // namespace ftcore
