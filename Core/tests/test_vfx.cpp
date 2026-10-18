#include "ftcore/Vfx.h"

#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr float kPi = 3.14159265358979323846f;

bool loadEffect(const std::string& name, VfxEffect& out, std::vector<std::string>* warnings = nullptr) {
    std::string text, err;
    if (!ftcore_test::readProjectData("Vfx/" + name + ".json", text)) {
        FAIL("cannot read Data/Vfx/" << name << ".json");
        return false;
    }
    const bool ok = importVfxEffect(text, out, &err, warnings);
    CHECK_MESSAGE(ok, name << ": " << err);
    return ok;
}

// The single error line of a failed import, or "" when it succeeded.
std::string importError(const char* json) {
    VfxEffect e;
    std::string err;
    if (importVfxEffect(json, e, &err)) return "";
    CHECK(e.emitters.empty());
    return err;
}

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

bool anyGap(const VfxNiagaraEmitter& e, const char* part) {
    for (const std::string& g : e.gaps)
        if (contains(g, part)) return true;
    return false;
}

} // namespace

TEST_CASE("vfx: every Data/Vfx effect loads without warnings") {
    struct Expect {
        const char* file;
        std::size_t emitters;
    };
    const Expect files[] = {{"ember_swirl", 1}, {"falling_leaves", 1}, {"fireworks", 3}, {"flipbook_fire", 1},
                            {"heal", 2},        {"heal_ring", 1},      {"hit_spark", 2}, {"magic_ribbon", 2},
                            {"perfect_burst", 2}, {"smoke", 1},        {"speed_sparks", 1}, {"stretched_sparks", 2}};
    for (const Expect& f : files) {
        CAPTURE(f.file);
        VfxEffect e;
        std::vector<std::string> warnings;
        REQUIRE(loadEffect(f.file, e, &warnings));
        CHECK(e.name == f.file);
        CHECK(e.emitters.size() == f.emitters);
        CHECK_MESSAGE(warnings.empty(), (warnings.empty() ? std::string() : warnings.front()));
        VfxNiagaraPlan plan;
        planVfxNiagara(e, plan);
        CHECK(plan.emitters.size() == f.emitters);
        for (const VfxNiagaraEmitter& p : plan.emitters) {
            CHECK(p.lifetime.lo > 0.f);
            CHECK(p.boundsMin.x < p.boundsMax.x);
            CHECK(p.boundsMin.z < p.boundsMax.z);
            CHECK_FALSE(p.startColor.empty());
        }
    }
}

TEST_CASE("vfx: fireworks reads bursts, modules and sub-emitters") {
    VfxEffect e;
    REQUIRE(loadEffect("fireworks", e));
    const VfxEmitter& rocket = e.emitters[0];
    CHECK(rocket.name == "rocket");
    REQUIRE(rocket.bursts.size() == 2);
    CHECK(rocket.bursts[1].time == doctest::Approx(0.8f));
    CHECK(rocket.bursts[1].count == 1);
    CHECK(rocket.loop);
    CHECK(rocket.duration == doctest::Approx(1.6f));
    CHECK(rocket.shape == VfxShape::Cone);
    CHECK(rocket.speed.mode == VfxCurveMode::TwoConstants);
    CHECK(rocket.speed.lo == doctest::Approx(5.f));
    CHECK(rocket.speed.hi == doctest::Approx(5.8f));
    CHECK(rocket.gravity == VfxVec3{0.f, -3.f, 0.f});
    CHECK(rocket.startColor.mode == VfxGradientMode::RandomColor);
    CHECK(rocket.startColor.stops.size() == 4);
    CHECK(rocket.blend == VfxBlend::Additive);
    CHECK(rocket.sprite == VfxSprite::Spark);
    CHECK(rocket.trails.enabled);
    CHECK(rocket.trails.maxPoints == 24);
    REQUIRE(rocket.subEmitters.size() == 2);
    CHECK(rocket.subEmitters[0].emitter == "burst");
    CHECK(rocket.subEmitters[0].on == VfxSubEmitterEvent::Death);
    CHECK(rocket.subEmitters[0].countMin == 70);
    CHECK(rocket.subEmitters[0].countMax == 90);
    CHECK(rocket.subEmitters[0].inherit == kVfxInheritColor);
    CHECK(rocket.subEmitters[1].countMin == 1);
    CHECK(rocket.subEmitters[1].countMax == 1);
    CHECK(findVfxEmitter(e, "flash") == 2);
    CHECK(findVfxEmitter(e, "nope") == -1);
}

TEST_CASE("vfx: other module forms read as in newengine") {
    VfxEffect e;
    REQUIRE(loadEffect("ember_swirl", e));
    const VfxEmitter& em = e.emitters[0];
    CHECK(em.forceOverLifetime.enabled);
    CHECK(em.forceOverLifetime.force[0] == VfxCurve::constant(0.f));
    CHECK(em.forceOverLifetime.force[1].mode == VfxCurveMode::Curve);
    CHECK(em.forceOverLifetime.force[1].keys.size() == 2);
    CHECK(em.limitVelocity.enabled);
    CHECK(em.limitVelocity.dampen == doctest::Approx(0.25f));
    CHECK(em.noise.enabled);
    CHECK(em.noise.octaves == 2);
    CHECK(em.noise.quality == VfxNoiseQuality::High);
    CHECK(em.stretch == doctest::Approx(0.04f));

    VfxEffect fire;
    REQUIRE(loadEffect("flipbook_fire", fire));
    CHECK(fire.emitters[0].textureSheet.enabled);
    CHECK(fire.emitters[0].textureSheet.tilesX == 4);
    CHECK(fire.emitters[0].textureSheet.tilesY == 4);
    CHECK(fire.emitters[0].rotationOverLifetime.enabled);

    VfxEffect leaves;
    REQUIRE(loadEffect("falling_leaves", leaves));
    CHECK(leaves.emitters[0].renderMode == VfxRenderMode::Mesh);
    CHECK(leaves.emitters[0].mesh == "leaf");

    VfxEffect sparks;
    REQUIRE(loadEffect("stretched_sparks", sparks));
    CHECK(sparks.emitters[0].renderMode == VfxRenderMode::Stretched);
    CHECK(sparks.emitters[0].sortMode == VfxSortMode::None);
    CHECK(sparks.emitters[1].renderMode == VfxRenderMode::Horizontal);
}

TEST_CASE("vfx: curve and gradient value forms") {
    VfxEffect e;
    std::string err;
    REQUIRE_MESSAGE(importVfxEffect(R"J({"name": "one",
        "lifetime": {"curve": [{"t": 1, "v": 4}, {"t": 0, "v": 2}], "mul": 0.5},
        "startSize": {"min": [{"t": 0, "v": 1}], "max": [{"t": 0, "v": 3}]},
        "speed": [2, 2],
        "sizeOverLife": [],
        "startColor": {"between": [[1, 0, 0, 1], [0, 0, 1, 1]]},
        "colorOverLife": [0.5, 0.5, 0.5, 1]})J",
                                    e, &err),
                    err);
    REQUIRE(e.emitters.size() == 1);
    CHECK(e.name == "one"); // a single-emitter file takes the emitter's name
    const VfxEmitter& m = e.emitters[0];
    CHECK(m.lifetime.mode == VfxCurveMode::Curve);
    CHECK(m.lifetime.keys[0].t == 0.f); // sorted by t
    CHECK(m.lifetime.evaluate(0.5f, 0.f) == doctest::Approx(1.5f));
    CHECK(m.startSize.mode == VfxCurveMode::TwoCurves);
    CHECK(m.startSize.evaluate(0.f, 0.5f) == doctest::Approx(2.f));
    CHECK(m.speed.mode == VfxCurveMode::TwoConstants);
    CHECK(m.sizeOverLife.mode == VfxCurveMode::Curve);
    CHECK(m.sizeOverLife.keys.empty());
    CHECK(m.sizeOverLife.evaluate(0.3f, 0.f) == 1.f);
    CHECK(m.startColor.mode == VfxGradientMode::TwoColors);
    CHECK(m.startColor.evaluate(0.f, 0.5f) == VfxColor{0.5f, 0.f, 0.5f, 1.f});
    CHECK(m.colorOverLife.mode == VfxGradientMode::Color);
    CHECK_FALSE(m.colorOverLife.isWhite());
    float lo = 0.f, hi = 0.f;
    m.lifetime.bounds(lo, hi);
    CHECK(lo == doctest::Approx(1.f));
    CHECK(hi == doctest::Approx(2.f));
}

TEST_CASE("vfx: sampling clamps and handles empty inputs") {
    const std::vector<VfxKey> keys{{0.f, 0.f}, {0.5f, 1.f}, {0.5f, 3.f}, {1.f, 2.f}};
    CHECK(sampleVfxKeys(keys, -1.f) == 0.f);
    CHECK(sampleVfxKeys(keys, 0.25f) == doctest::Approx(0.5f));
    CHECK(sampleVfxKeys(keys, 0.5f) == doctest::Approx(1.f));
    CHECK(sampleVfxKeys(keys, 0.75f) == doctest::Approx(2.5f));
    CHECK(sampleVfxKeys(keys, 2.f) == 2.f);
    CHECK(sampleVfxKeys({}, 0.4f) == 1.f);
    CHECK(sampleVfxStops({}, 0.4f) == VfxColor{});
    CHECK(VfxGradient{}.isWhite());
}

TEST_CASE("vfx: unknown keys are warnings with their path") {
    VfxEffect e;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE(importVfxEffect(R"J({"name": "w", "emitters": [{"name": "a", "colour": [1, 1, 1, 1],
                                  "noise": {"strength": 1, "wobble": 2}}], "extra": 1})J",
                            e, &err, &warnings));
    REQUIRE(warnings.size() == 3);
    CHECK(contains(warnings[0], "emitters[0].noise.wobble"));
    CHECK(contains(warnings[1], "emitters[0].colour"));
    CHECK(contains(warnings[2], "extra"));
}

TEST_CASE("vfx: bad input reports a reason with its path") {
    CHECK(contains(importError("{"), "line"));
    CHECK(importError(R"J({"name": "x", "emitters": []})J") == "emitters: expected at least one emitter");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "renderMode": "mesh"}]})J") ==
          "emitters[0].mesh: renderMode \"mesh\" needs a mesh name");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "renderMode": "quad"}]})J") ==
          "emitters[0].renderMode: unknown value \"quad\" (billboard, stretched, horizontal, vertical, mesh)");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "noise": {"octaves": 9}}]})J") ==
          "emitters[0].noise.octaves: expected 1..8");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "textureSheet": {"tiles": [0, 4]}}]})J") ==
          "emitters[0].textureSheet.tiles: expected [x, y], each 1..64");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "trails": {"ratio": 2}}]})J") ==
          "emitters[0].trails.ratio: expected 0..1");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "trails": {"maxPoints": 1}}]})J") ==
          "emitters[0].trails.maxPoints: expected 2..256");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "trails": {"ribbonCount": 0}}]})J") ==
          "emitters[0].trails.ribbonCount: expected >= 1");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "rate": "fast"}]})J") ==
          "emitters[0].rate: expected a number");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "gravity": [0, 1]}]})J") ==
          "emitters[0].gravity: expected [x, y, z]");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "bursts": [{"time": 0}]}]})J") ==
          "emitters[0].bursts[0].count: missing");

    const char* subs[][2] = {
        {R"J([{"emitter": "b", "count": [5, 2]}])J", "emitters[0].subEmitters[0].count: expected n or [min, max] with min <= max"},
        {R"J([{"emitter": "b", "probability": 1.5}])J", "emitters[0].subEmitters[0].probability: expected 0..1"},
        {R"J([{"emitter": "b", "inherit": ["hue"]}])J",
         "emitters[0].subEmitters[0].inherit[0]: unknown property \"hue\" (color, size, rotation, velocity)"},
        {R"J([{"emitter": "zz"}])J", "emitters[0].subEmitters[0].emitter: unknown emitter \"zz\""},
        {R"J([{"emitter": "a"}])J", "emitters[0].subEmitters[0].emitter: an emitter cannot be its own sub-emitter"},
    };
    for (const auto& s : subs) {
        const std::string json =
            std::string(R"J({"name": "x", "emitters": [{"name": "a", "subEmitters": )J") + s[0] + R"J(}, {"name": "b"}]})J";
        CHECK(importError(json.c_str()) == s[1]);
    }
    const std::string cycle = importError(R"J({"name": "x", "emitters": [
        {"name": "a", "subEmitters": [{"emitter": "b"}]}, {"name": "b", "subEmitters": [{"emitter": "a"}]}]})J");
    CHECK(contains(cycle, "emitters[0].subEmitters[0].emitter: sub-emitter cycle through \"b\""));
    CHECK(contains(cycle, "emitters[1].subEmitters[0].emitter: sub-emitter cycle through \"a\""));

    // Several problems: one line each.
    const std::string many =
        importError(R"J({"name": "x", "emitters": [{"name": "a", "drag": true, "noise": {"octaves": 0}}]})J");
    CHECK(many == "emitters[0].drag: expected a number\nemitters[0].noise.octaves: expected 1..8");
}

TEST_CASE("vfx: coordinates map to Unreal centimetres, Z up") {
    CHECK(vfxToUnreal({1.f, 2.f, 3.f}) == VfxVec3{100.f, 300.f, 200.f});
}

TEST_CASE("vfx: Niagara plan for heal_ring orbits a ring, mirrored") {
    VfxEffect e;
    REQUIRE(loadEffect("heal_ring", e));
    VfxNiagaraPlan plan;
    planVfxNiagara(e, plan);
    REQUIRE(plan.emitters.size() == 1);
    const VfxNiagaraEmitter& p = plan.emitters[0];
    CHECK(plan.loops);
    CHECK(p.loop);
    CHECK(p.rate == doctest::Approx(70.f));
    CHECK(p.rotateAroundPoint);
    // 2.5 rad/s about newengine +Y over a 1.7 s mean lifetime, turned the other way about Unreal +Z.
    CHECK(p.orbitRate == doctest::Approx(-2.5f * 180.f / kPi * 1.7f).epsilon(0.001));
    CHECK(p.orbitRadius.lo == doctest::Approx(95.f));
    CHECK(p.orbitRadius.hi == doctest::Approx(100.f));
    CHECK(p.orbitPhase.lo == 0.f);
    CHECK(p.orbitPhase.hi == doctest::Approx(2.f * kPi)); // radians
    CHECK(p.shape == VfxNiagaraShape::None);
    CHECK(p.velocity == VfxNiagaraVelocity::Linear);
    CHECK(p.linearLo == p.linearHi);
    CHECK(p.linearLo.x == 0.f);
    CHECK(p.linearLo.z == doctest::Approx(60.f));
    CHECK(p.additive);
    CHECK(p.spriteSize.lo == doctest::Approx(6.f));
    CHECK(p.spriteSize.hi == doctest::Approx(12.f));
    CHECK(p.sizeOverLife.size() == 3);
    CHECK(p.colorOverLife.size() == 3);
    CHECK(anyGap(p, "radial"));
}

TEST_CASE("vfx: Niagara plan doubles the cone angle and places death bursts") {
    VfxEffect e;
    REQUIRE(loadEffect("fireworks", e));
    VfxNiagaraPlan plan;
    planVfxNiagara(e, plan);
    REQUIRE(plan.emitters.size() == 3);
    const VfxNiagaraEmitter& rocket = plan.emitters[0];
    CHECK(rocket.velocity == VfxNiagaraVelocity::Cone);
    CHECK(rocket.coneAngle == doctest::Approx(24.f)); // full angle
    CHECK(rocket.coneDirection == VfxVec3{0.f, 0.f, 1.f});
    CHECK(rocket.speed.lo == doctest::Approx(500.f));
    CHECK(rocket.gravity == VfxVec3{0.f, 0.f, -300.f});
    CHECK(rocket.startColorRandom);
    CHECK(rocket.startColor.size() == 4);
    CHECK(rocket.shape == VfxNiagaraShape::Ring);
    CHECK(rocket.discCoverage == 1.f);
    CHECK(anyGap(rocket, "trails"));

    const VfxNiagaraEmitter& burst = plan.emitters[1];
    CHECK(burst.rate == 0.f);
    CHECK(burst.loop);
    CHECK(burst.loopDuration == doctest::Approx(1.6f));
    REQUIRE(burst.bursts.size() == 2);
    CHECK(burst.bursts[0].time == doctest::Approx(1.05f));
    CHECK(burst.bursts[1].time == doctest::Approx(0.25f)); // 0.8 + 1.05 wraps in the 1.6 s loop
    CHECK(burst.bursts[0].countLo == 70);
    CHECK(burst.bursts[0].countHi == 90);
    // The rocket dies at about 540 * 1.05 - 150 * 1.05^2 cm up.
    CHECK(burst.shapeOffset.z == doctest::Approx(540.f * 1.05f - 150.f * 1.05f * 1.05f).epsilon(0.01));
    CHECK(burst.pointOrigin.z == doctest::Approx(burst.shapeOffset.z));
    CHECK(burst.velocity == VfxNiagaraVelocity::FromPoint);
    CHECK(anyGap(burst, "inherit"));

    const VfxNiagaraEmitter& flash = plan.emitters[2];
    REQUIRE(flash.bursts.size() == 2);
    CHECK(flash.bursts[0].countLo == 1);
    CHECK(flash.shape == VfxNiagaraShape::Sphere); // carries the offset
    CHECK(flash.shapeOffset.z == doctest::Approx(burst.shapeOffset.z));
}

TEST_CASE("vfx: Niagara plan stretches and lays flat") {
    VfxEffect e;
    REQUIRE(loadEffect("stretched_sparks", e));
    VfxNiagaraPlan plan;
    planVfxNiagara(e, plan);
    const VfxNiagaraEmitter& sparks = plan.emitters[0];
    CHECK(sparks.facing == VfxNiagaraFacing::VelocityAligned);
    CHECK(sparks.scaleBySpeed);
    CHECK(sparks.minScale == VfxVec2{1.f, 2.f});
    CHECK(sparks.maxScale.y > 2.f);
    CHECK(sparks.speedThreshold == doctest::Approx(700.f));
    CHECK(plan.emitters[1].facing == VfxNiagaraFacing::Horizontal);

    VfxEffect embers;
    REQUIRE(loadEffect("ember_swirl", embers));
    planVfxNiagara(embers, plan);
    const VfxNiagaraEmitter& m = plan.emitters[0];
    CHECK(m.facing == VfxNiagaraFacing::VelocityAligned);
    CHECK(m.maxScale.y <= 3.f);
    CHECK(m.curlNoise);
    CHECK(m.noiseFrequency == doctest::Approx(0.4f));
    CHECK(m.noiseStrength == doctest::Approx(120.f / 0.8f));
    CHECK(m.acceleration.z == doctest::Approx(90.f).epsilon(0.01)); // mean of 1.4 -> 0.4 m/s^2
    CHECK(anyGap(m, "limitVelocity"));
    CHECK(anyGap(m, "octaves"));
}

TEST_CASE("vfx: one-shot plans report their duration") {
    VfxEffect e;
    std::string err;
    REQUIRE(importVfxEffect(R"J({"name": "pop", "bursts": [{"count": 10}], "duration": 0.5, "lifetime": [0.2, 0.8]})J", e,
                            &err));
    VfxNiagaraPlan plan;
    planVfxNiagara(e, plan);
    CHECK_FALSE(plan.loops);
    CHECK(plan.duration == doctest::Approx(1.3f));
    CHECK(plan.emitters[0].velocity == VfxNiagaraVelocity::None);
    CHECK(plan.emitters[0].shape == VfxNiagaraShape::None);
}
