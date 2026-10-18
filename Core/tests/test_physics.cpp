#include "ftcore/Physics.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream>
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

std::string readData(const std::string& rel) {
    std::string text;
    if (!ftcore_test::readProjectData(rel, text)) FAIL("cannot read Data/" << rel);
    return text;
}

RagdollProfile loadProfile(const char* name) {
    RagdollProfile p;
    std::string err;
    const bool ok = importRagdollProfile(readData(std::string("Physics/ragdoll/") + name + ".json"), p, &err);
    CHECK_MESSAGE(ok, name << ": " << err);
    return p;
}

RagdollMap loadMap() {
    RagdollMap m;
    std::string err;
    const bool ok = importRagdollMap(readData("Physics/ragdoll/humanoid_map.json"), m, &err);
    CHECK_MESSAGE(ok, err);
    return m;
}

std::string profileError(const char* json) {
    RagdollProfile p;
    std::string err;
    if (importRagdollProfile(json, p, &err)) return "";
    return err;
}

std::string mapError(const char* json) {
    RagdollMap m;
    std::string err;
    if (importRagdollMap(json, m, &err)) return "";
    CHECK(m.parts.empty());
    return err;
}

std::string materialError(const char* json) {
    PhysMaterialSet s;
    std::string err;
    if (importPhysMaterials(json, s, &err)) return "";
    CHECK(s.materials.empty());
    return err;
}

// The mannequin deform names (public Unreal names; proof_head uses them), with the face bones it adds.
std::vector<std::string> mannequinBones() {
    return {"root",        "pelvis",     "spine_01",   "spine_02",   "spine_03",   "neck_01",    "head",
            "jaw",         "tongue_01",  "eye_l",      "eye_r",      "clavicle_l", "upperarm_l", "lowerarm_l",
            "hand_l",      "clavicle_r", "upperarm_r", "lowerarm_r", "hand_r",     "thigh_l",    "calf_l",
            "foot_l",      "ball_l",     "thigh_r",    "calf_r",     "foot_r",     "ball_r"};
}

int boneIndex(const std::vector<std::string>& bones, const char* name) {
    for (std::size_t i = 0; i < bones.size(); ++i)
        if (bones[i] == name) return static_cast<int>(i);
    return -1;
}

} // namespace

TEST_CASE("physics: unit conversion to Unreal") {
    CHECK(physMetresToCm(1.5f) == doctest::Approx(150.f));
    CHECK(physImpulseToUnreal(70.f) == doctest::Approx(7000.f)); // 70 N*s = 7000 kg*cm/s
    CHECK(physDensityToUnreal(600.f) == doctest::Approx(0.6f));   // kg/m^3 -> g/cm^3
    CHECK(physRadToDeg(kPhysPi) == doctest::Approx(180.f));
}

TEST_CASE("physics: Data/Physics/materials.json loads") {
    PhysMaterialSet set;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importPhysMaterials(readData("Physics/materials.json"), set, &err, &warnings), err);
    CHECK(warnings.empty());
    CHECK(set.materials.size() == 6);
    const int wood = set.find("wood");
    REQUIRE(wood >= 0);
    CHECK(set.materials[static_cast<std::size_t>(wood)].surface == PhysSurface::Wood);
    CHECK(set.materials[static_cast<std::size_t>(wood)].density == doctest::Approx(600.f));
    const int sack = set.find("sack");
    REQUIRE(sack >= 0);
    CHECK(set.materials[static_cast<std::size_t>(sack)].frictionCombine == PhysCombine::Max);
    CHECK(set.materials[static_cast<std::size_t>(sack)].restitutionCombine == PhysCombine::Min);
    const int rubber = set.find("rubber");
    REQUIRE(rubber >= 0);
    CHECK(set.materials[static_cast<std::size_t>(rubber)].effectiveStaticFriction() == doctest::Approx(0.9f));
    CHECK(set.find("glass") == -1);
    CHECK(physMaterialAssetName("wood") == "PM_Wood");
    CHECK(physSurfaceName(PhysSurface::Flesh) == "flesh");
    CHECK(physCombineName(PhysCombine::Multiply) == "multiply");
}

TEST_CASE("physics: material errors give reasons") {
    CHECK(materialError("[]") == "materials file: expected object");
    CHECK(materialError("{\"materials\": []}") == "materials: expected a non-empty array");
    CHECK(materialError("{\"version\": 2, \"materials\": [{\"name\": \"a\"}]}") == "version: expected 1");
    CHECK(materialError("{\"materials\": [{\"name\": \"a\", \"friction\": \"x\"}]}") ==
          "materials[0].friction: expected number");
    CHECK(materialError("{\"materials\": [{\"name\": \"a\", \"friction\": 9}]}") ==
          "materials[0].friction: expected a number in 0..4");
    CHECK(materialError("{\"materials\": [{\"name\": \"a\", \"restitution\": 1.5}]}") ==
          "materials[0].restitution: expected a number in 0..1");
    CHECK(materialError("{\"materials\": [{\"name\": \"a\", \"density\": 0}]}") ==
          "materials[0].density: expected kg/m^3 in 1..25000");
    CHECK(materialError("{\"materials\": [{\"name\": \"a\", \"surface\": \"glass\"}]}") ==
          "materials[0].surface: unknown surface \"glass\"");
    CHECK(materialError("{\"materials\": [{\"name\": \"Wood\"}]}") ==
          "materials[0].name: expected lower-case letters, digits and _");
    CHECK(materialError("{\"materials\": [{\"name\": \"a\"}, {\"name\": \"a\"}]}") ==
          "materials[1].name: duplicate \"a\"");
    CHECK(materialError("{\"materials\": [{\"name\": \"a\", \"frictionCombine\": \"sum\"}]}") ==
          "materials[0].frictionCombine: expected default, average, min, multiply or max");
    // Several problems: one line each.
    CHECK(materialError("{\"materials\": [{\"name\": \"a\", \"friction\": -1}, {\"name\": \"b\", \"density\": -1}]}") ==
          "materials[0].friction: expected a number in 0..4\nmaterials[1].density: expected kg/m^3 in 1..25000");
    PhysMaterialSet s;
    std::vector<std::string> warnings;
    CHECK(importPhysMaterials("{\"materials\": [{\"name\": \"a\", \"colour\": 1}], \"extra\": 0}", s, nullptr, &warnings));
    CHECK(warnings.size() == 2);
}

TEST_CASE("physics: the three ragdoll profiles load with their values") {
    const RagdollProfile def = loadProfile("default");
    CHECK(def.name == "default");
    CHECK_FALSE(def.useAuthoredKo);
    CHECK(def.maxLinearVelocity == doctest::Approx(7.5f));
    CHECK(def.positionSteps == 24);
    CHECK(def.motor.maxTorque == doctest::Approx(400.f));
    CHECK(def.limits.size() == 11);
    CHECK(def.limitsFor("shin.L").hinge);
    CHECK(def.limitsFor("shin.L").maxAngle == doctest::Approx(2.3f));
    CHECK(def.limitsFor("pelvis").normalHalfCone == doctest::Approx(0.3f)); // not listed: defaultLimits

    const RagdollProfile gentle = loadProfile("gentle");
    CHECK(gentle.maxLinearVelocity < def.maxLinearVelocity);
    CHECK(gentle.koImpulseScale == doctest::Approx(0.6f));
    CHECK(gentle.limitsFor("upperArm.R").twistMax == doctest::Approx(0.8f));

    const RagdollProfile authored = loadProfile("minor");
    CHECK(authored.useAuthoredKo);
    CHECK(authored.maxLinearVelocity == doctest::Approx(3.f));
    // Every profile stays under the F8 speed gate (8 m/s).
    for (const RagdollProfile* p : {&def, &gentle, &authored}) CHECK(p->maxLinearVelocity <= 8.f);
    // Limits not listed in "minor" fall back to its tighter defaults.
    CHECK(authored.limitsFor("spine").normalHalfCone == doctest::Approx(0.15f));
}

TEST_CASE("physics: ragdoll profile errors give reasons") {
    CHECK(profileError("[1]") == "ragdoll profile: expected object");
    CHECK(profileError("{\"maxLinearVelocity\": \"fast\"}") == "maxLinearVelocity: expected number");
    CHECK(profileError("{\"positionSteps\": 2.5}") == "positionSteps: expected integer");
    CHECK(profileError("{\"positionSteps\": 300}") == "ragdoll profile: value out of range");
    CHECK(profileError("{\"maxLinearVelocity\": 0}") == "ragdoll profile: value out of range");
    CHECK(profileError("{\"motor\": 3}") == "motor: expected object");
    CHECK(profileError("{\"motor\": {\"frequency\": true}}") == "motor.frequency: expected number");
    CHECK(profileError("{\"limits\": []}") == "limits: expected object keyed by part name");
    CHECK(profileError("{\"limits\": {\"shin.L\": {\"type\": \"ball\"}}}") ==
          "limits.shin.L.type: expected \"hinge\" or \"swingTwist\"");
    CHECK(profileError("{\"limits\": {\"shin.L\": {\"type\": \"hinge\", \"minAngle\": 1, \"maxAngle\": 0}}}") ==
          "limits.shin.L.minAngle: must not exceed maxAngle");
    CHECK(profileError("{\"defaultLimits\": {\"normalHalfCone\": -1}}") ==
          "defaultLimits.normalHalfCone/planeHalfCone: expected 0..pi");
    CHECK(profileError("{\"name\": \"x\"") != ""); // bad JSON reports the parser's position
}

TEST_CASE("physics: the humanoid map loads") {
    const RagdollMap m = loadMap();
    CHECK(m.name == "humanoid");
    CHECK(m.totalMass == doctest::Approx(70.f));
    REQUIRE(m.parts.size() == 12);
    CHECK(m.parts[0].name == "pelvis");
    CHECK(m.parts[0].parent.empty());
    float share = 0.f;
    for (const RagdollPartDef& p : m.parts) share += p.massFraction;
    CHECK(share == doctest::Approx(0.99f).epsilon(0.001));
    CHECK(m.noCollide.size() == 2);
    const int head = m.find("head");
    REQUIRE(head >= 0);
    CHECK(m.parts[static_cast<std::size_t>(head)].tip.empty());
    CHECK(m.parts[static_cast<std::size_t>(head)].length == doctest::Approx(0.25f));
    CHECK(m.parts[static_cast<std::size_t>(head)].upper);
}

TEST_CASE("physics: ragdoll map errors give reasons") {
    CHECK(mapError("{}") == "ragdoll map: \"parts\" must be a non-empty array");
    CHECK(mapError("{\"parts\": [{\"name\": \"a\"}]}") == "parts[]: every part needs a name and joint candidates");
    CHECK(mapError("{\"parts\": [{\"name\": \"a\", \"joints\": \"x\", \"radius\": 0}]}") ==
          "parts[a]: radius, length and mass must be > 0");
    CHECK(mapError("{\"parts\": [{\"name\": \"a\", \"joints\": \"x\", \"parent\": \"b\"}]}") ==
          "parts[a]: parent must be listed before it");
    CHECK(mapError("{\"parts\": [{\"name\": \"a\", \"joints\": \"x\"}, {\"name\": \"b\", \"joints\": \"y\"}]}") ==
          "ragdoll map: more than one root part");
    CHECK(mapError("{\"parts\": [{\"name\": \"a\", \"joints\": 3}]}") ==
          "parts[a].joints: expected string or array of strings");
    CHECK(mapError("{\"parts\": [{\"name\": \"a\", \"joints\": \"x\"}], \"noCollide\": [[\"a\"]]}") ==
          "noCollide: expected array of [a, b] pairs");
    CHECK(mapError("{\"parts\": [{\"name\": \"a\", \"joints\": \"x\"}], \"noCollide\": [[\"a\", \"z\"]]}") ==
          "noCollide: unknown part in [a, z]");
}

TEST_CASE("physics: the humanoid map binds to the mannequin bone names") {
    const RagdollMap m = loadMap();
    const std::vector<std::string> bones = mannequinBones();
    RagdollBinding b;
    std::string err;
    REQUIRE_MESSAGE(bindRagdoll(m, bones, b, &err), err);
    REQUIRE(b.parts.size() == 12);
    CHECK(b.dropped.empty());
    auto partNamed = [&](const char* name) -> const RagdollBoundPart& {
        for (const RagdollBoundPart& p : b.parts)
            if (m.parts[static_cast<std::size_t>(p.part)].name == name) return p;
        FAIL("no bound part " << name);
        return b.parts[0];
    };
    CHECK(partNamed("pelvis").bone == boneIndex(bones, "pelvis"));
    CHECK(partNamed("pelvis").tipBone == boneIndex(bones, "spine_01"));
    CHECK(partNamed("spine").bone == boneIndex(bones, "spine_01"));
    CHECK(partNamed("chest").bone == boneIndex(bones, "spine_03"));
    CHECK(partNamed("chest").tipBone == boneIndex(bones, "head")); // candidates in order: "head" before "neck_01"
    CHECK(partNamed("head").tipBone == -1); // no tip: the capsule uses the map's length
    CHECK(partNamed("foreArm.L").bone == boneIndex(bones, "lowerarm_l"));
    CHECK(partNamed("foreArm.L").tipBone == boneIndex(bones, "hand_l"));
    CHECK(partNamed("shin.R").tipBone == boneIndex(bones, "foot_r"));
    CHECK(partNamed("thigh.L").mass == doctest::Approx(7.f));
    CHECK(b.parts[0].parent == -1);
    for (std::size_t i = 1; i < b.parts.size(); ++i) CHECK(b.parts[i].parent >= 0);
    // Parent/child, grandparent and the two listed pairs; no duplicates.
    for (std::size_t i = 0; i < b.noCollide.size(); ++i) {
        CHECK(b.noCollide[i].first < b.noCollide[i].second);
        for (std::size_t k = i + 1; k < b.noCollide.size(); ++k) CHECK(b.noCollide[i] != b.noCollide[k]);
    }
    // 11 parent/child pairs and 8 grandparent pairs; the map's two arm/spine pairs are grandparent pairs already.
    CHECK(b.noCollide.size() == 19);
}

TEST_CASE("physics: binding fails or drops parts with reasons") {
    RagdollMap m = loadMap();
    std::vector<std::string> bones = mannequinBones();
    bones.erase(bones.begin() + boneIndex(bones, "calf_l"));
    RagdollBinding b;
    std::string err;
    CHECK_FALSE(bindRagdoll(m, bones, b, &err));
    CHECK(err == "ragdoll: no joint found for part \"shin.L\"");
    CHECK(b.parts.empty());

    // Optional: the part and its subtree are dropped.
    m.parts[static_cast<std::size_t>(m.find("thigh.L"))].optional = true;
    m.parts[static_cast<std::size_t>(m.find("shin.L"))].optional = true;
    bones = mannequinBones();
    bones.erase(bones.begin() + boneIndex(bones, "thigh_l"));
    REQUIRE_MESSAGE(bindRagdoll(m, bones, b, &err), err);
    CHECK(b.parts.size() == 10);
    CHECK(b.dropped == std::vector<std::string>{"thigh.L", "shin.L"});

    bones = {"spine_01", "head"};
    CHECK_FALSE(bindRagdoll(m, bones, b, &err));
    CHECK(err == "ragdoll: the root part \"pelvis\" is missing");
}

TEST_CASE("physics: Chaos joint limits from the profile") {
    const RagdollProfile def = loadProfile("default");
    const RagdollChaosLimits shin = ragdollChaosLimits(def.limitsFor("shin.L"));
    CHECK(shin.hinge);
    CHECK(shin.swing2Deg == 0.f);
    CHECK(shin.twistDeg == 0.f);
    // -0.05..2.3 rad: half-range 1.175 rad about a centre of 1.125 rad.
    CHECK(shin.swing1Deg == doctest::Approx(physRadToDeg(1.175f)));
    CHECK(shin.swing1OffsetDeg == doctest::Approx(physRadToDeg(1.125f)));
    CHECK(shin.swing1OffsetDeg - shin.swing1Deg == doctest::Approx(physRadToDeg(-0.05f)));
    const RagdollChaosLimits fore = ragdollChaosLimits(def.limitsFor("foreArm.R"));
    CHECK(fore.swing1OffsetDeg + fore.swing1Deg == doctest::Approx(physRadToDeg(0.05f)));
    const RagdollChaosLimits thigh = ragdollChaosLimits(def.limitsFor("thigh.L"));
    CHECK_FALSE(thigh.hinge);
    CHECK(thigh.swing1Deg == doctest::Approx(physRadToDeg(1.0f))); // forward/back about the lateral axis
    CHECK(thigh.swing2Deg == doctest::Approx(physRadToDeg(0.4f)));
    CHECK(thigh.twistDeg == doctest::Approx(physRadToDeg(0.3f)));
    CHECK(thigh.twistOffsetDeg == doctest::Approx(0.f));
    RagdollJointLimits skew;
    skew.twistMin = -0.1f;
    skew.twistMax = 0.3f;
    const RagdollChaosLimits s = ragdollChaosLimits(skew);
    CHECK(s.twistDeg == doctest::Approx(physRadToDeg(0.2f)));
    CHECK(s.twistOffsetDeg == doctest::Approx(physRadToDeg(0.1f)));
    // A hinge whose range leaves out the rest pose is widened to include it (as newengine does).
    RagdollJointLimits off;
    off.hinge = true;
    off.minAngle = 0.5f;
    off.maxAngle = 1.f;
    CHECK(ragdollChaosLimits(off).swing1OffsetDeg - ragdollChaosLimits(off).swing1Deg == doctest::Approx(0.f));
}

TEST_CASE("physics: knockback slide and wall cut (F6)") {
    // 70 kg, 280 N*s: 4 m/s; friction 0.5 slides 16 / (2 * 0.5 * 9.81) = 1.631 m.
    const float slide = knockbackSlideDistance(4.f, 0.5f);
    CHECK(slide == doctest::Approx(1.6310f).epsilon(0.001));
    CHECK(knockbackSpeedForDistance(slide, 0.5f) == doctest::Approx(4.f));
    CHECK(knockbackSlideDistance(4.f, 0.f) == 0.f);

    KnockbackPlan free = planKnockback(70.f, 0.f, 0.f, 280.f, 0.f, 0.5f, -1.f);
    CHECK_FALSE(free.wallCut);
    CHECK(free.distance == doctest::Approx(slide));
    CHECK(free.impulseX == doctest::Approx(280.f));
    CHECK(free.impulseY == doctest::Approx(0.f));
    CHECK(free.dirX == doctest::Approx(1.f));

    // A wall 1 m away: the slide stops 2 cm short.
    KnockbackPlan cut = planKnockback(70.f, 0.f, 0.f, 280.f, 0.f, 0.5f, 1.f);
    CHECK(cut.wallCut);
    CHECK(cut.distance == doctest::Approx(0.98f));
    CHECK(cut.impulseX < 280.f);
    CHECK(knockbackSlideDistance(cut.impulseX / 70.f, 0.5f) == doctest::Approx(0.98f));

    // Up against the wall: no slide at all, and the existing velocity is cancelled.
    KnockbackPlan stop = planKnockback(70.f, 1.f, 0.f, 280.f, 0.f, 0.5f, 0.01f);
    CHECK(stop.distance == 0.f);
    CHECK(stop.impulseX == doctest::Approx(-70.f));

    // A wall beyond the slide changes nothing; the current velocity adds to the shove.
    KnockbackPlan far = planKnockback(70.f, 0.f, 1.f, 0.f, 70.f, 0.5f, 50.f);
    CHECK_FALSE(far.wallCut);
    CHECK(far.dirY == doctest::Approx(1.f));
    CHECK(far.impulseY == doctest::Approx(70.f));
    CHECK(planKnockback(0.f, 0.f, 0.f, 1.f, 0.f, 0.5f, -1.f).distance == 0.f);
}

TEST_CASE("physics: KO impulse split (F8)") {
    const RagdollProfile def = loadProfile("default");
    const float impulse[3] = {300.f, 0.f, 0.f};
    // A 2.1 kg forearm may gain at most 3 m/s: 6.3 N*s of the 90 N*s local share.
    const KoImpulseSplit s = splitKoImpulse(def, impulse, 69.3f, 2.1f);
    CHECK(s.localImpulse[0] == doctest::Approx(6.3f));
    CHECK(s.wholeDeltaV[0] == doctest::Approx((300.f - 6.3f) / 69.3f));
    CHECK(s.localImpulse[1] == 0.f);
    // A heavy part takes the whole 30%.
    const KoImpulseSplit heavy = splitKoImpulse(def, impulse, 69.3f, 50.f);
    CHECK(heavy.localImpulse[0] == doctest::Approx(90.f));
    CHECK(heavy.wholeDeltaV[0] == doctest::Approx(210.f / 69.3f));
    // The gentle profile scales the blow down.
    const RagdollProfile gentle = loadProfile("gentle");
    const KoImpulseSplit g = splitKoImpulse(gentle, impulse, 69.3f, 50.f);
    CHECK(g.localImpulse[0] + g.wholeDeltaV[0] * 69.3f == doctest::Approx(180.f));
}

TEST_CASE("physics: hit reaction weight, going heavy and the settle assist (F8, F9)") {
    const RagdollProfile def = loadProfile("default");
    CHECK(hitReactionWeight(def, 0.f) == 1.f);
    CHECK(hitReactionWeight(def, 0.1f) == 1.f);
    CHECK(hitReactionWeight(def, 0.25f) == doctest::Approx(0.5f));
    CHECK(hitReactionWeight(def, 0.4f) == doctest::Approx(0.f));
    CHECK(hitReactionWeight(def, 5.f) == 0.f);
    CHECK(hitReactionWeight(def, -1.f) == 0.f);
    // The F9 gate: back to the animation in 0.35..0.45 s.
    const float blendBack = def.hitHold + def.hitBlendOut;
    CHECK(blendBack >= 0.35f);
    CHECK(blendBack <= 0.45f);

    CHECK(heavyDampingFactor(8.f, 1.f / 60.f) == doctest::Approx(std::exp(-8.f / 60.f)));
    CHECK(heavyDampingFactor(0.f, 1.f / 60.f) == 1.f);

    SettleTracker t;
    const float dt = 1.f / 60.f;
    int frames = 0;
    bool slept = false;
    for (; frames < 60 && !slept; ++frames) slept = t.update(0.1f, 0.5f, true, 0.3f, 1.f, 0.2f, dt);
    CHECK(slept);
    CHECK(frames == 12); // 0.2 s at 60 Hz
    CHECK(t.quietFor == 0.f);
    // A fast frame resets the clock; nothing awake never sleeps.
    CHECK_FALSE(t.update(0.1f, 0.5f, true, 0.3f, 1.f, 0.2f, 0.15f));
    CHECK_FALSE(t.update(2.f, 0.5f, true, 0.3f, 1.f, 0.2f, dt));
    CHECK(t.quietFor == 0.f);
    CHECK_FALSE(t.update(0.f, 0.f, false, 0.3f, 1.f, 0.2f, 1.f));
    CHECK_FALSE(t.update(0.f, 0.f, true, 0.3f, 1.f, 0.f, 1.f)); // settleTime 0: off
}

TEST_CASE("physics: hit-stop freezes exactly N frames (F7)") {
    HitStop h;
    CHECK_FALSE(h.consume());
    h.trigger(5);
    h.trigger(3); // a shorter trigger keeps the remainder
    int frozen = 0;
    while (h.consume()) ++frozen;
    CHECK(frozen == 5);
    CHECK(h.frozenFrames() == 5);
    CHECK_FALSE(h.frozen());
}

TEST_CASE("physics: the feel maths does not allocate") {
    const RagdollProfile def = loadProfile("default");
    const float impulse[3] = {100.f, 20.f, 5.f};
    REQUIRE(ftcore_test::allocHookInstalled());
    const std::uint64_t before = ftcore_test::allocStats().count;
    float sink = 0.f;
    SettleTracker t;
    HitStop h;
    h.trigger(2);
    for (int i = 0; i < 100; ++i) {
        sink += planKnockback(70.f, 0.f, 0.f, 100.f, 10.f, 0.5f, 1.f).impulseX;
        sink += splitKoImpulse(def, impulse, 70.f, 2.f).wholeDeltaV[0];
        sink += hitReactionWeight(def, 0.01f * static_cast<float>(i));
        sink += heavyDampingFactor(8.f, 1.f / 60.f);
        sink += t.update(0.1f, 0.1f, true, 0.3f, 1.f, 0.2f, 1.f / 60.f) ? 1.f : 0.f;
        sink += h.consume() ? 1.f : 0.f;
        sink += ragdollChaosLimits(def.limitsFor("thigh.L")).swing1Deg;
    }
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(sink != 0.f);
}
