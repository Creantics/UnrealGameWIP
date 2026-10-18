#pragma once

// Particle effects ("VFX"): the authoring data model of newengine's particle system (engine/include/ne/vfx/
// Particles.h and EffectLoader.h, which stay the source of truth for what each field does), its JSON reader,
// and the plan that turns an effect into Niagara Lightweight (stateless) emitters. See docs/VFX_UE.md.
//
//   std::string text = ...; // Data/Vfx/<name>.json
//   ftcore::VfxEffect effect;
//   std::string error;
//   std::vector<std::string> warnings;
//   if (!ftcore::importVfxEffect(text, effect, &error, &warnings)) ...; // error: "emitters[0].noise.octaves: expected 1..8"
//   ftcore::VfxNiagaraPlan plan;
//   ftcore::planVfxNiagara(effect, plan); // centimetres, Z up, degrees; plan.emitters[i].gaps lists what is lost
//
// Values follow the Shuriken-style model newengine uses: a VfxCurve is a constant, a curve, a random pick
// between two constants or a random blend of two curves; a VfxGradient is the colour analogue. Spec data is
// in newengine world units (metres, Y up, right-handed); the plan converts to Unreal (centimetres, Z up,
// left-handed) with vfxToUnreal.
//
// Loading and planning allocate (load time only). Evaluating a curve or gradient does not.

#include "ftcore/Api.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ftcore {

struct VfxVec2 {
    float x = 0.f, y = 0.f;
    friend constexpr bool operator==(const VfxVec2& a, const VfxVec2& b) { return a.x == b.x && a.y == b.y; }
};

struct VfxVec3 {
    float x = 0.f, y = 0.f, z = 0.f;
    friend constexpr bool operator==(const VfxVec3& a, const VfxVec3& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
};

// Linear RGBA; components may exceed 1 (HDR glow).
struct VfxColor {
    float r = 1.f, g = 1.f, b = 1.f, a = 1.f;
    friend constexpr bool operator==(const VfxColor& p, const VfxColor& q) {
        return p.r == q.r && p.g == q.g && p.b == q.b && p.a == q.a;
    }
};

struct VfxKey {
    float t = 0.f, v = 1.f;
    friend constexpr bool operator==(const VfxKey& a, const VfxKey& b) { return a.t == b.t && a.v == b.v; }
};

struct VfxColorStop {
    float t = 0.f;
    VfxColor color;
    friend constexpr bool operator==(const VfxColorStop& a, const VfxColorStop& b) {
        return a.t == b.t && a.color == b.color;
    }
};

// Piecewise-linear over t (keys sorted by t), clamped at the ends. No keys = 1; no stops = opaque white.
FTCORE_API float sampleVfxKeys(const std::vector<VfxKey>& keys, float t);
FTCORE_API VfxColor sampleVfxStops(const std::vector<VfxColorStop>& stops, float t);

enum class VfxCurveMode : std::uint8_t { Constant, Curve, TwoConstants, TwoCurves };

// A scalar: x is normalised time (the emitter cycle for start values, the particle's age for over-life
// values), r the particle's fixed random factor in [0, 1).
//   Constant / TwoConstants: lo + (hi - lo) * r
//   Curve:     keys(x) * multiplier
//   TwoCurves: mix(keysLo(x), keys(x), r) * multiplier
struct VfxCurve {
    VfxCurveMode mode = VfxCurveMode::Constant;
    float lo = 0.f, hi = 0.f;
    std::vector<VfxKey> keys;   // Curve, or the upper curve of TwoCurves
    std::vector<VfxKey> keysLo; // lower curve of TwoCurves
    float multiplier = 1.f;     // curve modes only

    static VfxCurve constant(float v) { return range(v, v); }
    static VfxCurve range(float a, float b) {
        VfxCurve c;
        c.mode = a == b ? VfxCurveMode::Constant : VfxCurveMode::TwoConstants;
        c.lo = a;
        c.hi = b;
        return c;
    }
    static VfxCurve curve(std::vector<VfxKey> k, float mul = 1.f) {
        VfxCurve c;
        c.mode = VfxCurveMode::Curve;
        c.keys = std::move(k);
        c.multiplier = mul;
        return c;
    }

    FTCORE_API float evaluate(float x, float r) const;
    bool isConstant() const { return mode == VfxCurveMode::Constant || mode == VfxCurveMode::TwoConstants; }
    // Smallest and largest value over x and r in [0, 1].
    FTCORE_API void bounds(float& outLo, float& outHi) const;
    // Average over x in [0, 1] at r = 0.5.
    FTCORE_API float mean() const;

    friend bool operator==(const VfxCurve& a, const VfxCurve& b) {
        return a.mode == b.mode && a.lo == b.lo && a.hi == b.hi && a.keys == b.keys && a.keysLo == b.keysLo &&
               a.multiplier == b.multiplier;
    }
};

enum class VfxGradientMode : std::uint8_t { Color, Gradient, TwoColors, TwoGradients, RandomColor };

// A colour: a constant, a gradient over x, a random blend of two colours or two gradients, or a random
// colour picked from a gradient (x ignored).
struct VfxGradient {
    VfxGradientMode mode = VfxGradientMode::Gradient;
    VfxColor colorLo, colorHi;        // Color uses colorHi; TwoColors blends lo..hi
    std::vector<VfxColorStop> stops;   // Gradient / upper / RandomColor; empty = white
    std::vector<VfxColorStop> stopsLo; // lower gradient of TwoGradients

    static VfxGradient color(VfxColor c) {
        VfxGradient g;
        g.mode = VfxGradientMode::Color;
        g.colorLo = g.colorHi = c;
        return g;
    }

    FTCORE_API VfxColor evaluate(float x, float r) const;
    // True when every value is opaque white (a no-op multiplier).
    FTCORE_API bool isWhite() const;

    friend bool operator==(const VfxGradient& a, const VfxGradient& b) {
        return a.mode == b.mode && a.colorLo == b.colorLo && a.colorHi == b.colorHi && a.stops == b.stops &&
               a.stopsLo == b.stopsLo;
    }
};

enum class VfxBlend : std::uint8_t { Alpha, Additive };
enum class VfxShape : std::uint8_t { Point, Sphere, Cone, Box, Ring };
enum class VfxSpace : std::uint8_t { World, Local };
// Procedural sprite when an emitter has no texture: Soft (round glow), Puff (eroded cloud), Spark (hot core).
enum class VfxSprite : std::uint8_t { Soft, Puff, Spark };
enum class VfxRenderMode : std::uint8_t { Billboard, Stretched, Horizontal, Vertical, Mesh };
enum class VfxSortMode : std::uint8_t { Auto, None, ByDistance, OldestInFront, YoungestInFront };
enum class VfxNoiseQuality : std::uint8_t { Low, Medium, High };
enum class VfxInheritMode : std::uint8_t { Initial, Current };
enum class VfxSheetAnimation : std::uint8_t { WholeSheet, SingleRow };
enum class VfxSheetRowMode : std::uint8_t { Custom, Random };
enum class VfxSheetTimeMode : std::uint8_t { Lifetime, Speed, Fps };
enum class VfxTrailMode : std::uint8_t { PerParticle, Ribbon };
enum class VfxSubEmitterEvent : std::uint8_t { Birth, Death };

// Properties a sub-emitter's particles take from the parent particle (VfxSubEmitter::inherit bits).
inline constexpr std::uint32_t kVfxInheritColor = 1u << 0;
inline constexpr std::uint32_t kVfxInheritSize = 1u << 1;
inline constexpr std::uint32_t kVfxInheritRotation = 1u << 2;
inline constexpr std::uint32_t kVfxInheritVelocity = 1u << 3;

struct VfxBurst {
    float time = 0.f; // seconds into the emitter cycle
    std::uint32_t count = 0;
    friend constexpr bool operator==(const VfxBurst& a, const VfxBurst& b) {
        return a.time == b.time && a.count == b.count;
    }
};

// ---- modules (a module object present in the JSON is enabled unless it says "enabled": false) ----------
// Angles in module fields are degrees; orbital velocity is rad/s; the emitter's startRotation,
// angularVelocity and orbit are radians.

struct VfxVelocityOverLifetime {
    bool enabled = false;
    VfxCurve linear[3];  // units/s over age, added (not accumulated)
    VfxSpace space = VfxSpace::Local;
    VfxCurve orbital[3]; // rad/s about the emitter's X, Y, Z axes
    VfxVec3 offset;      // orbit centre, emitter-local
    VfxCurve radial;     // units/s away from the centre
};

struct VfxLimitVelocity {
    bool enabled = false;
    VfxCurve speed = VfxCurve::constant(1.f);
    float dampen = 1.f;
    float drag = 0.f;
};

struct VfxForceOverLifetime {
    bool enabled = false;
    VfxCurve force[3]; // units/s^2 over age
    VfxSpace space = VfxSpace::Local;
    bool randomize = false;
};

struct VfxInheritVelocity {
    bool enabled = false;
    VfxInheritMode mode = VfxInheritMode::Initial;
    VfxCurve multiplier = VfxCurve::constant(1.f);
};

struct VfxNoise {
    bool enabled = false;
    VfxCurve strength = VfxCurve::constant(1.f); // units/s at full noise, over age
    float frequency = 0.5f;
    float scrollSpeed = 0.f;
    bool damping = true; // strength is divided by frequency
    int octaves = 1;     // 1..8
    float octaveMultiplier = 0.5f;
    float octaveScale = 2.f;
    VfxNoiseQuality quality = VfxNoiseQuality::Medium;
    std::vector<VfxKey> remap;
    VfxCurve positionAmount = VfxCurve::constant(1.f);
    VfxCurve rotationAmount = VfxCurve::constant(0.f);
    VfxCurve sizeAmount = VfxCurve::constant(0.f);
};

struct VfxColorBySpeed {
    bool enabled = false;
    VfxGradient color;
    VfxVec2 range{0.f, 1.f};
};

struct VfxSizeBySpeed {
    bool enabled = false;
    VfxCurve size = VfxCurve::curve({});
    VfxVec2 range{0.f, 1.f};
};

struct VfxRotationOverLifetime {
    bool enabled = false;
    VfxCurve angularVelocity = VfxCurve::constant(45.f); // degrees/s
};

struct VfxRotationBySpeed {
    bool enabled = false;
    VfxCurve angularVelocity = VfxCurve::constant(45.f); // degrees/s
    VfxVec2 range{0.f, 1.f};
};

struct VfxTextureSheet {
    bool enabled = false;
    int tilesX = 1, tilesY = 1; // 1..64
    VfxSheetAnimation animation = VfxSheetAnimation::WholeSheet;
    VfxSheetRowMode rowMode = VfxSheetRowMode::Custom;
    int row = 0;
    VfxSheetTimeMode timeMode = VfxSheetTimeMode::Lifetime;
    VfxCurve frameOverTime = VfxCurve::curve({{0.f, 0.f}, {1.f, 1.f}});
    VfxCurve startFrame = VfxCurve::constant(0.f);
    float cycles = 1.f;
    float fps = 10.f;
    VfxVec2 speedRange{0.f, 1.f};
    bool frameBlend = false;
};

struct VfxTrails {
    bool enabled = false;
    VfxTrailMode mode = VfxTrailMode::PerParticle;
    float ratio = 1.f; // 0..1
    VfxCurve lifetime = VfxCurve::constant(1.f);
    float minVertexDistance = 0.2f;
    std::uint32_t maxPoints = 16; // 2..256
    VfxCurve widthOverTrail = VfxCurve::constant(1.f);
    VfxGradient colorOverTrail;
    bool inheritParticleColor = true;
    bool sizeAffectsWidth = true;
    std::uint32_t ribbonCount = 1; // >= 1
};

// On every `on` event of a particle of this emitter, emitter `emitter` of the same effect spawns
// countMin..countMax particles there (with `probability`). The target then emits only through events.
struct VfxSubEmitter {
    std::string emitter;
    VfxSubEmitterEvent on = VfxSubEmitterEvent::Death;
    std::uint32_t countMin = 1, countMax = 1;
    float probability = 1.f;
    std::uint32_t inherit = 0; // kVfxInherit* bits
    float velocityScale = 1.f;
};

struct VfxEmitter {
    std::string name;

    // emission
    float rate = 0.f; // particles per second
    std::vector<VfxBurst> bursts;
    float duration = 1.f; // seconds per cycle
    bool loop = false;
    std::uint32_t maxParticles = 256;
    VfxCurve lifetime = VfxCurve::constant(1.f);

    // spawn shape (emitter frame, Y up)
    VfxShape shape = VfxShape::Point;
    float radius = 1.f;      // sphere / ring outer radius, cone base radius
    float innerRadius = 0.f; // ring
    float coneAngle = 25.f;  // degrees, half-angle around +Y
    VfxVec3 boxHalfExtents{0.5f, 0.5f, 0.5f};

    // motion
    VfxCurve speed = VfxCurve::constant(0.f); // along the shape's outward direction (point: any direction)
    VfxVec3 velocityMin, velocityMax;          // extra per-axis velocity, uniform in the box
    VfxVec3 gravity;                           // acceleration, world space
    float drag = 0.f;                          // v *= exp(-drag * dt)
    float orbit = 0.f;                         // rad/s: turns the velocity about the emitter's Y axis

    // look
    VfxCurve startSize = VfxCurve::constant(0.1f); // world units (quad edge)
    VfxCurve sizeOverLife = VfxCurve::curve({});
    VfxGradient startColor = VfxGradient::color({});
    VfxGradient colorOverLife;
    VfxCurve startRotation = VfxCurve::constant(0.f);   // radians
    VfxCurve angularVelocity = VfxCurve::constant(0.f); // radians / second
    VfxBlend blend = VfxBlend::Alpha;
    VfxSprite sprite = VfxSprite::Soft;
    float stretch = 0.f; // seconds of travel the billboard stretches along its velocity
    float stretchMax = 8.f;
    float softness = 0.35f;
    float coreShade = 0.55f;
    float turbulence = 0.55f;
    std::string texture;
    VfxSpace space = VfxSpace::World;
    VfxRenderMode renderMode = VfxRenderMode::Billboard;
    float lengthScale = 2.f;
    float speedScale = 0.f;
    std::string mesh;
    VfxSortMode sortMode = VfxSortMode::Auto;

    // modules (all off by default)
    VfxVelocityOverLifetime velocityOverLifetime;
    VfxLimitVelocity limitVelocity;
    VfxForceOverLifetime forceOverLifetime;
    VfxInheritVelocity inheritVelocity;
    VfxNoise noise;
    VfxColorBySpeed colorBySpeed;
    VfxSizeBySpeed sizeBySpeed;
    VfxRotationOverLifetime rotationOverLifetime;
    VfxRotationBySpeed rotationBySpeed;
    VfxTextureSheet textureSheet;
    VfxTrails trails;
    std::vector<VfxSubEmitter> subEmitters;
};

struct VfxEffect {
    std::string name;
    std::vector<VfxEmitter> emitters;
};

// Reads an effect file: {"name", "emitters": [...]}, or a single emitter object without "emitters". False
// on bad input: `out` is then empty and `*error` lists every problem, one per line, each with its path
// ("emitters[1].subEmitters[0].emitter: unknown emitter \"x\""). Unknown keys are not errors; they are
// appended to `*warnings` the same way.
FTCORE_API bool importVfxEffect(std::string_view json, VfxEffect& out, std::string* error = nullptr,
                                std::vector<std::string>* warnings = nullptr);

// Index of the emitter called `name`, or -1.
FTCORE_API int findVfxEmitter(const VfxEffect& effect, std::string_view name);

// ---- Unreal ---------------------------------------------------------------------------------------------

inline constexpr float kVfxUnrealUnitsPerMetre = 100.f;

// A newengine position or direction (metres, Y up, right-handed) in Unreal (centimetres, Z up,
// left-handed): (x, y, z) -> 100 * (x, z, y). Swapping two axes mirrors, so a turn about Y in newengine
// is a turn the other way about Z in Unreal.
constexpr VfxVec3 vfxToUnreal(VfxVec3 p) {
    return {p.x * kVfxUnrealUnitsPerMetre, p.z * kVfxUnrealUnitsPerMetre, p.y * kVfxUnrealUnitsPerMetre};
}

struct VfxRange {
    float lo = 0.f, hi = 0.f;
    friend constexpr bool operator==(const VfxRange& a, const VfxRange& b) { return a.lo == b.lo && a.hi == b.hi; }
};

struct VfxNiagaraBurst {
    float time = 0.f;
    std::uint32_t countLo = 0, countHi = 0;
};

// Niagara Lightweight ShapeLocation primitive (None: the module is off, particles start at the origin).
enum class VfxNiagaraShape : std::uint8_t { None, Sphere, Box, Ring };
// AddVelocity's single velocity type (None: the module is off).
enum class VfxNiagaraVelocity : std::uint8_t { None, Linear, FromPoint, Cone };
// Sprite renderer facing / alignment.
enum class VfxNiagaraFacing : std::uint8_t { Camera, VelocityAligned, Horizontal, Vertical };

// One spec emitter as Niagara Lightweight module values. Units: centimetres, seconds, degrees; Z up.
// Fields mirror the module properties the builder writes (docs/VFX_UE.md has the table).
struct VfxNiagaraEmitter {
    std::string name;

    // EmitterState and spawn infos
    bool loop = false;
    float loopDuration = 1.f;
    float rate = 0.f; // 0: no rate spawn info
    std::vector<VfxNiagaraBurst> bursts;

    // InitializeParticle
    VfxRange lifetime{1.f, 1.f};
    VfxRange spriteSize{10.f, 10.f}; // cm, square sprites
    VfxRange spriteRotation;         // degrees
    std::vector<VfxColorStop> startColor; // one stop = constant; several = a random pick along the stops
    bool startColorRandom = false;        // pick per particle (several stops)

    // ShapeLocation
    VfxNiagaraShape shape = VfxNiagaraShape::None;
    float sphereRadius = 0.f;
    VfxVec3 boxSize; // full size
    float ringRadius = 0.f;
    float discCoverage = 0.f; // 0 = the ring's edge, 1 = the whole disc
    VfxVec3 shapeOffset;       // added to every spawn position

    // AddVelocity
    VfxNiagaraVelocity velocity = VfxNiagaraVelocity::None;
    VfxVec3 linearLo, linearHi;  // Linear
    VfxRange speed;              // FromPoint / Cone
    VfxVec3 pointOrigin;         // FromPoint
    VfxVec3 coneDirection{0.f, 0.f, 1.f};
    float coneAngle = 0.f;       // full angle, degrees

    // Forces
    VfxVec3 gravity; // GravityForce, off when zero
    float drag = 0.f;
    VfxVec3 acceleration; // AccelerationForce, off when zero
    bool curlNoise = false;
    float noiseStrength = 0.f;  // cm/s
    float noiseFrequency = 0.f; // Niagara units (samples position * frequency / 50)

    // ScaleSpriteSize / ScaleColor (empty: module off)
    std::vector<VfxKey> sizeOverLife;
    std::vector<VfxColorStop> colorOverLife;

    // SpriteRotationRate (off when zero)
    VfxRange rotationRate; // degrees/s

    // Renderer
    bool additive = false;
    VfxSprite sprite = VfxSprite::Soft;
    VfxNiagaraFacing facing = VfxNiagaraFacing::Camera;
    bool scaleBySpeed = false; // ScaleSpriteSizeBySpeed: scale lerps min..max as speed goes 0..threshold
    float speedThreshold = 0.f; // cm/s
    VfxVec2 minScale{1.f, 1.f}, maxScale{1.f, 1.f};

    // FixedBounds estimate (local space, cm)
    VfxVec3 boundsMin, boundsMax;

    // What the Lightweight emitter cannot express, in words, for the build log and docs.
    std::vector<std::string> gaps;
};

struct VfxNiagaraPlan {
    std::string name;
    std::vector<VfxNiagaraEmitter> emitters;
    bool loops = false;    // any emitter loops
    float duration = 0.f;  // one-shot effects: seconds until the last particle can have died
};

// Builds the plan for `effect` (which importVfxEffect accepted).
FTCORE_API void planVfxNiagara(const VfxEffect& effect, VfxNiagaraPlan& out);

// Runtime knobs every built Niagara system exposes as user parameters ("User.<name>"); UFTVfxComponent
// fills them. Each multiplies a module value, so 1 leaves the effect as authored.
inline constexpr const char* kVfxUserSpeedScale = "SpeedScale"; // float: every start velocity
inline constexpr const char* kVfxUserShapeScale = "ShapeScale"; // vector: the spawn shape (per axis, cm stay cm)

struct VfxUserParameters {
    float speedScale = 1.f;
    float shapeScale = 1.f; // uniform
};

// Clamps each knob to [0, 10]; a non-finite value becomes 1.
FTCORE_API VfxUserParameters sanitizeVfxUserParameters(VfxUserParameters p);

} // namespace ftcore
