#pragma once

// Physics data and feel maths carried over from newengine's physics plan (docs/reference/physics_plan.md, P2a).
// newengine ran Jolt; here the solver is Chaos, so what carries over is the data and the maths around the
// solver, not the solver. See docs/PHYSICS_UE.md.
//
//   Physical materials  Data/Physics/materials.json: friction, restitution, density and a surface per material
//                       (a draft authored here; newengine had no material file). Built into UPhysicalMaterial.
//   Ragdoll map         Data/Physics/ragdoll/humanoid_map.json: which bones become capsule parts. One map serves
//                       several rigs, because each part lists candidate bone names.
//   Ragdoll profiles    Data/Physics/ragdoll/{default,gentle,...}.json: joint limits, damping, velocity caps,
//                       KO and hit-reaction tuning. A profile with useAuthoredKo never ragdolls on KO.
//   Feel maths          knockback slide and wall cut (F6), hit-stop (F7), the KO impulse split (F8), the
//                       hit-reaction blend weight (F9), the settle assist and "going heavy" (F8, F10).
//   Unreal mapping      metres, N*s and kg/m^3 in the data; centimetres, kg*cm/s and g/cm^3 in Unreal. Joint
//                       limits become Chaos cone/twist limits (symmetric, so asymmetric ranges get an offset).
//
//   ftcore::RagdollProfile profile;
//   std::string error;
//   if (!ftcore::importRagdollProfile(text, profile, &error)) ...; // error: "limits.shin.L.type: expected ..."
//
// Loading allocates (load time only). Nothing else here allocates.

#include "ftcore/Api.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ftcore {

// ---- units -----------------------------------------------------------------------------------------------

inline constexpr float kPhysCmPerMetre = 100.f;
inline constexpr float kPhysGravity = 9.81f;                // m/s^2, the plan's gravity
inline constexpr float kPhysPi = 3.14159265358979323846f;

constexpr float physMetresToCm(float m) { return m * kPhysCmPerMetre; }
// An impulse in N*s (kg*m/s) as Unreal's AddImpulse takes it (kg*cm/s).
constexpr float physImpulseToUnreal(float newtonSeconds) { return newtonSeconds * kPhysCmPerMetre; }
// Density in kg/m^3 as UPhysicalMaterial::Density takes it (g/cm^3).
constexpr float physDensityToUnreal(float kgPerCubicMetre) { return kgPerCubicMetre / 1000.f; }
constexpr float physRadToDeg(float rad) { return rad * (180.f / kPhysPi); }

// ---- physical materials ----------------------------------------------------------------------------------

// Combine modes for a contact pair. Default leaves it to the project setting (Unreal: average).
enum class PhysCombine : std::uint8_t { Default, Average, Min, Multiply, Max };

// Surface kinds for audio/VFX contact hooks. The order is the Unreal SurfaceType index (SurfaceType1 = Wood ...,
// 0 is SurfaceType_Default), set up in Config/DefaultEngine.ini.
enum class PhysSurface : std::uint8_t { Default, Wood, Stone, Metal, Cloth, Rubber, Flesh, Count };

FTCORE_API std::string_view physSurfaceName(PhysSurface surface); // "wood"; "default" for Default
FTCORE_API std::string_view physCombineName(PhysCombine combine);  // "average"; "default" for Default

struct PhysMaterial {
    std::string name;
    PhysSurface surface = PhysSurface::Default;
    float friction = 0.7f;        // dynamic (kinetic) friction coefficient
    float staticFriction = -1.f;  // < 0: same as friction
    float restitution = 0.3f;     // 0..1
    float density = 1000.f;       // kg/m^3
    PhysCombine frictionCombine = PhysCombine::Default;
    PhysCombine restitutionCombine = PhysCombine::Default;

    float effectiveStaticFriction() const { return staticFriction < 0.f ? friction : staticFriction; }
};

struct PhysMaterialSet {
    std::vector<PhysMaterial> materials;
    // Index of the material with that name, or -1.
    FTCORE_API int find(std::string_view name) const;
};

// Data/Physics/materials.json. On failure `out` is empty and *error lists each problem with its path, one per
// line ("materials[2].friction: expected a number in 0..4"). Unknown keys are warnings.
FTCORE_API bool importPhysMaterials(std::string_view json, PhysMaterialSet& out, std::string* error = nullptr,
                                    std::vector<std::string>* warnings = nullptr);

// "PM_<Name>" with the first letter upper-cased: "wood" -> "PM_Wood".
FTCORE_API std::string physMaterialAssetName(std::string_view name);

// ---- ragdoll data ----------------------------------------------------------------------------------------

struct RagdollPartDef {
    std::string name;
    std::vector<std::string> joints; // candidate bone names, first match wins
    std::string parent;              // parent part; empty for the one root part
    std::vector<std::string> tip;    // candidate bones where the capsule ends; none found: use `length`
    float length = 0.25f;            // m
    float radius = 0.06f;            // m
    float massFraction = 0.05f;      // of RagdollMap::totalMass
    bool upper = false;              // upper body: driven in a hit reaction
    bool optional = false;           // missing from the skeleton: dropped with its subtree instead of failing
};

struct RagdollMap {
    std::string name;
    float totalMass = 70.f; // kg
    std::vector<RagdollPartDef> parts; // parents before children
    std::vector<std::pair<std::string, std::string>> noCollide;
    // Index of the part with that name, or -1.
    FTCORE_API int find(std::string_view part) const;
};

struct RagdollJointLimits {
    bool hinge = false;                                // elbow/knee: about the character's lateral axis
    float minAngle = 0.f, maxAngle = 0.f;              // hinge, radians
    float normalHalfCone = 0.3f, planeHalfCone = 0.3f; // swing-twist, radians
    float twistMin = -0.2f, twistMax = 0.2f;
};

struct RagdollMotor {
    float frequency = 8.f; // Hz
    float damping = 1.f;   // ratio
    float maxTorque = 400.f; // N*m
};

struct RagdollProfile {
    std::string name;
    bool useAuthoredKo = false;
    float maxLinearVelocity = 7.5f;  // m/s per part
    float maxAngularVelocity = 20.f; // rad/s
    float linearDamping = 0.5f, angularDamping = 1.f;
    float jointFriction = 2.f; // N*m
    float friction = 0.8f, restitution = 0.f;
    int positionSteps = 4, velocitySteps = 10;
    float koImpulseScale = 1.f;
    float koLocalShare = 0.3f;     // share of the KO impulse applied at the struck part
    float koMaxLocalDeltaV = 3.f;  // m/s cap on the struck part's extra speed from the local share
    float inertiaScale = 1.f;
    float settleSpeed = 0.2f, settleAngularSpeed = 1.f, settleTime = 0.25f; // settleTime <= 0: off
    float koHeavyAfter = 1.2f, koHeavyRate = 8.f, koHeavyBelow = 1.f;
    float hitImpulseScale = 1.f;
    float hitHold = 0.1f, hitBlendOut = 0.3f; // seconds
    RagdollMotor motor;
    RagdollJointLimits defaultLimits;
    std::vector<std::pair<std::string, RagdollJointLimits>> limits; // per part name

    // The part's limits, or defaultLimits.
    FTCORE_API const RagdollJointLimits& limitsFor(std::string_view part) const;
};

// JSON readers. On failure `out` is reset and *error says what and where, with the newengine messages
// ("parts[spine]: parent must be listed before it", "limits.shin.L.type: expected \"hinge\" or \"swingTwist\"").
FTCORE_API bool importRagdollMap(std::string_view json, RagdollMap& out, std::string* error = nullptr);
FTCORE_API bool importRagdollProfile(std::string_view json, RagdollProfile& out, std::string* error = nullptr);

// ---- binding a map to a skeleton ----------------------------------------------------------------------------

struct RagdollBoundPart {
    int part = -1;       // index in RagdollMap::parts
    int bone = -1;       // skeleton bone the body follows
    int tipBone = -1;    // bone where the capsule ends, or -1 (then RagdollPartDef::length)
    int parent = -1;     // index in RagdollBinding::parts, -1 for the root
    float mass = 0.f;    // kg
};

struct RagdollBinding {
    std::vector<RagdollBoundPart> parts;            // parents before children
    std::vector<std::pair<int, int>> noCollide;     // binding indices: the map's pairs plus parent/child and grandparent
    std::vector<std::string> dropped;               // optional parts missing from the skeleton (with their subtrees)
};

// Resolves `map` against a skeleton given as bone names. Fails (with the part's name) if a non-optional part has
// none of its candidate bones, or if the root part is missing. A part whose parent was dropped is dropped too.
FTCORE_API bool bindRagdoll(const RagdollMap& map, const std::vector<std::string>& boneNames, RagdollBinding& out,
                            std::string* error = nullptr);

// ---- Chaos joint limits ---------------------------------------------------------------------------------------

// A Chaos constraint's angular limits for one ragdoll joint, in degrees. The constraint frame is X along the child
// bone (twist), Z the character's lateral axis (swing1, the hinge axis) and Y = Z x X (swing2). Chaos limits are
// symmetric about the frame, so an asymmetric range becomes a half-range plus an offset (AngularRotationOffset).
struct RagdollChaosLimits {
    bool hinge = false;
    float swing1Deg = 0.f, swing2Deg = 0.f, twistDeg = 0.f; // half-ranges; 0 = locked
    float swing1OffsetDeg = 0.f; // centre of the swing1 range (hinges)
    float twistOffsetDeg = 0.f;  // centre of the twist range
};

FTCORE_API RagdollChaosLimits ragdollChaosLimits(const RagdollJointLimits& limits);

// ---- feel maths -------------------------------------------------------------------------------------------------

// F6. Slide length of a body given `speed` on flat ground with Coulomb `friction`: v^2 / (2 mu g). Metres.
FTCORE_API float knockbackSlideDistance(float speed, float friction, float gravity = kPhysGravity);
// The speed that slides exactly `distance`: sqrt(2 mu g d).
FTCORE_API float knockbackSpeedForDistance(float distance, float friction, float gravity = kPhysGravity);

struct KnockbackPlan {
    float requestedDistance = 0.f; // m the full impulse would slide on flat ground
    float distance = 0.f;          // m after the wall cut
    bool wallCut = false;
    float dirX = 0.f, dirY = 0.f;  // unit horizontal slide direction (in the caller's horizontal plane)
    float impulseX = 0.f, impulseY = 0.f; // N*s to apply (replaces the caller's horizontal impulse)
};

// The newengine knockback: horizontal velocity (vx, vy) plus impulse (ix, iy) / mass gives a slide; when a wall
// lies `wallDistance` metres along it (< 0: no wall), the impulse is cut so the slide ends `skin` short of it.
// Units: kg, m/s, N*s, m.
FTCORE_API KnockbackPlan planKnockback(float mass, float vx, float vy, float ix, float iy, float friction,
                                       float wallDistance, float skin = 0.02f, float gravity = kPhysGravity);

struct KoImpulseSplit {
    float wholeDeltaV[3] = {0.f, 0.f, 0.f}; // m/s added to every part
    float localImpulse[3] = {0.f, 0.f, 0.f}; // N*s applied at the struck part
};

// F8. Scales `impulse` by koImpulseScale, gives koLocalShare of it to the struck part (capped so that part gains
// at most koMaxLocalDeltaV), and spreads the rest over the whole body as one velocity change.
FTCORE_API KoImpulseSplit splitKoImpulse(const RagdollProfile& profile, const float impulse[3], float totalMass,
                                         float partMass);

// F9. Physics blend weight `t` seconds into a hit reaction: 1 for hitHold, then a smoothstep down to 0 over
// hitBlendOut.
FTCORE_API float hitReactionWeight(const RagdollProfile& profile, float t);

// "Going heavy": the velocity multiplier for one step of dt at `rate` (exp(-rate dt)); 1 when rate <= 0.
FTCORE_API float heavyDampingFactor(float rate, float dt);

// The settle assist: once every part has been slower than the thresholds for `settleTime`, put the body to sleep.
struct SettleTracker {
    float quietFor = 0.f;
    // Feed the fastest linear (m/s) and angular (rad/s) speed of the awake parts; `anyAwake` false resets.
    // Returns true on the step the body should be put to sleep (and resets).
    FTCORE_API bool update(float maxLinear, float maxAngular, bool anyAwake, float settleSpeed, float settleAngularSpeed,
                           float settleTime, float dt);
};

// F7. Freezes N presentation frames (a new trigger keeps the longer remainder).
class HitStop {
public:
    void trigger(int frames) { remaining_ = frames > remaining_ ? frames : remaining_; }
    bool frozen() const { return remaining_ > 0; }
    int remaining() const { return remaining_; }
    // One frame: true (and consumes it) while frozen, meaning "do not simulate this frame".
    bool consume() {
        if (remaining_ <= 0) return false;
        --remaining_;
        ++frozenTotal_;
        return true;
    }
    std::uint64_t frozenFrames() const { return frozenTotal_; }

private:
    int remaining_ = 0;
    std::uint64_t frozenTotal_ = 0;
};

} // namespace ftcore
