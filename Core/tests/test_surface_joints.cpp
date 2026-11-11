// Joint depth on brick and block walls (ftcore/SurfaceJoints.h): the AO joint mask as a height, the parallax march's
// fades and steps, its reference twin, and the HLSL written from the same numbers.

#include "ftcore/SurfaceJoints.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace ftcore;

namespace {

// A vertical joint: the floor (height 0) for u in [0.4, 0.6] of every tile, the brick's face (1) elsewhere.
float grooveU(float u, float, const void*) {
    const float f = u - std::floor(u);
    return f >= 0.4f && f <= 0.6f ? 0.f : 1.f;
}

// A soft-sided joint from a sampled table of the AO mask, as the real map gives it.
float softGroove(float u, float, const void* user) {
    const float* ao = static_cast<const float*>(user);
    const float f = (u - std::floor(u)) * 15.f;
    const int i = static_cast<int>(f);
    const float t = f - static_cast<float>(i);
    const float a = ao[i] + (ao[i + 1] - ao[i]) * t;
    return surfaceJointHeight(a, 0.6f, 0.92f);
}

bool contains(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

} // namespace

TEST_CASE("joints: height from the AO mask, 1 on the face, 0 on the joint's floor") {
    CHECK(surfaceJointHeight(1.0f, 0.6f, 0.92f) == doctest::Approx(1.f));
    CHECK(surfaceJointHeight(0.92f, 0.6f, 0.92f) == doctest::Approx(1.f));
    CHECK(surfaceJointHeight(0.6f, 0.6f, 0.92f) == doctest::Approx(0.f));
    CHECK(surfaceJointHeight(0.5f, 0.6f, 0.92f) == doctest::Approx(0.f));
    CHECK(surfaceJointHeight(0.76f, 0.6f, 0.92f) == doctest::Approx(0.5f));
    // Equal thresholds do not divide by zero.
    CHECK(surfaceJointHeight(0.7f, 0.7f, 0.7f) == doctest::Approx(0.f));
    CHECK(surfaceJointHeight(0.8f, 0.7f, 0.7f) == doctest::Approx(1.f));
}

TEST_CASE("joints: parallax fades out from 12 to 15 m and from dot 0.25 to 0.12, and follows the switch") {
    const SurfaceJointSpec spec;
    CHECK(surfaceJointParallaxWeight(spec, 5.f, 1.f, 1.f) == doctest::Approx(1.f));
    CHECK(surfaceJointParallaxWeight(spec, 12.f, 1.f, 1.f) == doctest::Approx(1.f));
    CHECK(surfaceJointParallaxWeight(spec, 15.f, 1.f, 1.f) == doctest::Approx(0.f));
    CHECK(surfaceJointParallaxWeight(spec, 40.f, 1.f, 1.f) == doctest::Approx(0.f));
    const float mid = surfaceJointParallaxWeight(spec, 13.5f, 1.f, 1.f);
    CHECK(mid > 0.f);
    CHECK(mid < 1.f);
    CHECK(surfaceJointParallaxWeight(spec, 5.f, 0.25f, 1.f) == doctest::Approx(1.f));
    CHECK(surfaceJointParallaxWeight(spec, 5.f, 0.12f, 1.f) == doctest::Approx(0.f));
    CHECK(surfaceJointParallaxWeight(spec, 5.f, -0.5f, 1.f) == doctest::Approx(0.f));
    CHECK(surfaceJointParallaxWeight(spec, 5.f, 1.f, 0.f) == doctest::Approx(0.f));
    CHECK(surfaceJointParallaxWeight(spec, 5.f, 1.f, 0.5f) == doctest::Approx(0.5f));
    CHECK(surfaceJointParallaxWeight(spec, 5.f, 1.f, 3.f) == doctest::Approx(1.f));
}

TEST_CASE("joints: steps run from maxSteps on glancing views to minSteps square on") {
    const SurfaceJointSpec spec;
    CHECK(surfaceJointSteps(spec, 1.f) == spec.minSteps);
    CHECK(surfaceJointSteps(spec, 0.25f) == spec.maxSteps);
    CHECK(surfaceJointSteps(spec, 0.1f) == spec.maxSteps);
    int last = spec.maxSteps;
    for (int i = -10; i <= 110; ++i) {
        const int s = surfaceJointSteps(spec, static_cast<float>(i) / 100.f);
        CHECK(s >= spec.minSteps);
        CHECK(s <= spec.maxSteps);
        CHECK(s <= last);  // never more steps as the view squares up
        last = s;
    }
}

TEST_CASE("joints: the mip level from the camera distance, and the shade's fade on it") {
    const SurfaceJointSpec spec;
    const float tileCm = 100.f;
    const float oneTexel = tileCm / (spec.radPerPixel * spec.texels);  // one texel per pixel here
    CHECK(surfaceJointLod(spec, oneTexel, tileCm) == doctest::Approx(0.f).epsilon(1e-4));
    CHECK(surfaceJointLod(spec, 2.f * oneTexel, tileCm) == doctest::Approx(1.f).epsilon(1e-4));
    CHECK(surfaceJointLod(spec, 8.f * oneTexel, tileCm) == doctest::Approx(3.f).epsilon(1e-4));
    CHECK(surfaceJointLod(spec, 0.f, tileCm) == doctest::Approx(0.f));
    CHECK(surfaceJointLod(spec, 10.f, tileCm) == doctest::Approx(0.f));
    CHECK(surfaceJointShadeFade(spec, 0.f) == doctest::Approx(1.f));
    CHECK(surfaceJointShadeFade(spec, 3.f) == doctest::Approx(1.f));
    CHECK(surfaceJointShadeFade(spec, 5.f) == doctest::Approx(0.f));
    CHECK(surfaceJointShadeFade(spec, 9.f) == doctest::Approx(0.f));
    // Brick on a 1 m tile shades fully at 6 m and is fading by 15 m (a joint is under a pixel there).
    CHECK(surfaceJointShadeFade(spec, surfaceJointLod(spec, 600.f, tileCm)) == doctest::Approx(1.f));
    CHECK(surfaceJointShadeFade(spec, surfaceJointLod(spec, 1500.f, tileCm)) < 0.5f);
}

TEST_CASE("joints: the march: square on no shift, into a groove onto its floor, nothing at weight 0") {
    const SurfaceJointSpec spec;
    const float depth = 0.02f;  // 2 cm on a 1 m tile
    // Square on: no shift, wherever.
    for (float u : {0.1f, 0.5f, 0.59f}) {
        const SurfaceJointShift s = surfaceJointMarch(spec, grooveU, nullptr, u, 0.3f, 0.f, 0.f, 1.f, depth, 1.f);
        CHECK(s.du == doctest::Approx(0.f));
        CHECK(s.dv == doctest::Approx(0.f));
    }
    // Into the groove at 40 degrees along u: ends on the floor, depth * tan(angle) away from the camera's side.
    for (float deg : {20.f, 40.f, 60.f}) {
        const float a = deg * 3.14159265f / 180.f;
        const SurfaceJointShift s =
            surfaceJointMarch(spec, grooveU, nullptr, 0.5f, 0.3f, std::sin(a), 0.f, std::cos(a), depth, 1.f);
        CHECK(s.du == doctest::Approx(-depth * std::tan(a)).epsilon(1e-3));
        CHECK(s.dv == doctest::Approx(0.f));
        // Along v the same, on the other axis.
        const SurfaceJointShift t =
            surfaceJointMarch(spec, grooveU, nullptr, 0.5f, 0.3f, 0.f, std::sin(a), std::cos(a), depth, 1.f);
        CHECK(t.dv == doctest::Approx(-depth * std::tan(a)).epsilon(1e-3));
        CHECK(t.du == doctest::Approx(0.f));
    }
    // On the brick's face the ray stops at once.
    const SurfaceJointShift face = surfaceJointMarch(spec, grooveU, nullptr, 0.2f, 0.3f, 0.6f, 0.f, 0.8f, depth, 1.f);
    CHECK(face.du == doctest::Approx(0.f));
    // Weight 0: nothing; half weight: half the shift on the floor.
    const SurfaceJointShift off = surfaceJointMarch(spec, grooveU, nullptr, 0.5f, 0.3f, 0.6f, 0.f, 0.8f, depth, 0.f);
    CHECK(off.du == doctest::Approx(0.f));
    CHECK(off.dv == doctest::Approx(0.f));
    const SurfaceJointShift half = surfaceJointMarch(spec, grooveU, nullptr, 0.5f, 0.3f, 0.6f, 0.f, 0.8f, depth, 0.5f);
    CHECK(half.du == doctest::Approx(-0.5f * depth * 0.75f).epsilon(1e-3));
}

TEST_CASE("joints: the march never runs further than depth times the held slope") {
    const SurfaceJointSpec spec;
    // A soft joint like the map's: face 1.0, sloping into a 0.65 floor.
    const float ao[16] = {1.f, 1.f, 1.f, 1.f, 1.f, 0.95f, 0.8f, 0.66f, 0.65f, 0.66f, 0.8f, 0.95f, 1.f, 1.f, 1.f, 1.f};
    const float depth = 0.01f;
    const float bound = depth * spec.maxSlope + 1e-6f;
    int moved = 0;
    for (int i = 0; i < 40; ++i) {
        const float u = static_cast<float>(i) / 40.f;
        for (float n : {0.05f, 0.12f, 0.3f, 0.6f, 0.95f}) {
            const float side = std::sqrt(1.f - n * n);
            for (float turn : {0.f, 0.8f, 2.f, 3.5f}) {
                const SurfaceJointShift s = surfaceJointMarch(spec, softGroove, ao, u, 0.f, side * std::cos(turn),
                                                              side * std::sin(turn), n, depth, 1.f);
                const float len = std::sqrt(s.du * s.du + s.dv * s.dv);
                CHECK(len <= bound);
                // Square-on-ish views never run further than depth * tan.
                CHECK(len <= depth * std::min(side / n, spec.maxSlope) + 1e-6f);
                if (len > 0.f) ++moved;
            }
        }
    }
    CHECK(moved > 0);
}

TEST_CASE("joints: per-surface depths by id substring") {
    CHECK(surfaceJointFor("solhaven_soot_brick").depthCm == doctest::Approx(1.0f));
    CHECK(surfaceJointFor("solhaven_breeze_block_grey").depthCm == doctest::Approx(0.8f));
    CHECK(surfaceJointFor("solhaven_ceramic_tile").depthCm == doctest::Approx(0.3f));
    CHECK(surfaceJointFor("solhaven_paving_slab").depthCm == doctest::Approx(0.65f));
    CHECK(surfaceJointFor("solhaven_tactile_paving").depthCm == doctest::Approx(0.5f));
    CHECK(surfaceJointFor("solhaven_board_concrete").depthCm == doctest::Approx(0.2f));
    for (const char* id : {"solhaven_asphalt", "", "brick", "solhaven_plaster"}) {
        const SurfaceJointSurface s = surfaceJointFor(id);
        CHECK(s.depthCm == doctest::Approx(0.f));
        CHECK(s.aoLo == doctest::Approx(0.6f));
        CHECK(s.aoHi == doctest::Approx(0.92f));
    }
    const SurfaceJointSurface brick = surfaceJointFor("soot_brick");
    CHECK(brick.aoLo == doctest::Approx(0.6f));
    CHECK(brick.aoHi == doctest::Approx(0.92f));
}

TEST_CASE("joints: the HLSL: no derivatives, the march behind the switch, every input named") {
    const SurfaceJointSpec spec;
    const std::string par = surfaceJointParallaxHlsl(spec);
    const std::string shade = surfaceJointShadeHlsl(spec);
    for (const std::string* h : {&par, &shade}) {
        CHECK_FALSE(contains(*h, "ddx("));
        CHECK_FALSE(contains(*h, "ddy("));
        CHECK_FALSE(contains(*h, ".Sample("));
        CHECK_FALSE(contains(*h, "SampleGrad"));
    }
    // The march sits inside the dynamic if, with a compile-time bound and level samples only.
    const size_t gate = par.find("if (JointPar > 0.0)");
    REQUIRE(gate != std::string::npos);
    CHECK(par.find("[loop] for (int JtI = 0; JtI <= 8;") > gate);
    CHECK(par.find("Texture2DSampleLevel(ArmTex, ArmTexSampler, JtAt, JtLod)") > gate);
    CHECK_FALSE(contains(par, "Texture2DSample(ArmTex"));
    CHECK(contains(par, "break;"));
    for (const char* in : {"ViewVec", "JointDepth)", "JointDepthCm", "JointLo", "JointHi", "TileCm", "Blend", "UvX",
                           "UvY", "UvZ", "AxX", "AxY", "AxZ", "SideDown", "TopDown", "Up"})
        CHECK_MESSAGE(contains(par, in), in);
    // The shifted UV is written back to the dominant projection, and the frame is left for the shade.
    for (const char* out : {"UvX = JtHit", "UvY = JtHit", "UvZ = JtHit", "float JointPar", "float2 JtUv",
                            "float3 JtTu", "float3 JtTv", "float JtLod", "JtTu = AxY; JtTv = SideDown * AxZ",
                            "JtTu = AxX; JtTv = SideDown * AxZ", "float3 JtTv = TopDown * AxY"})
        CHECK_MESSAGE(contains(par, out), out);
    // The shade: behind its own switch, plain samples outside any loop, every input it reads.
    CHECK(contains(shade, "if (JtShade > 0.0)"));
    CHECK_FALSE(contains(shade, "for ("));
    CHECK(contains(shade, "Texture2DSample(ArmTex, ArmTexSampler, JtUv)"));
    for (const char* in : {"JointDepth)", "JointDepthCm", "JointLo", "JointHi", "TileCm", "JtUv", "JtTu", "JtTv",
                           "JtLod", "Mottle[0]", "Mottle[2]", "NormalOut =", "AOOut *=", "RoughOut =", "Color *="})
        CHECK_MESSAGE(contains(shade, in), in);
    // A spec's numbers reach the text.
    SurfaceJointSpec other;
    other.maxSteps = 12;
    other.cavityAO = 0.25f;
    CHECK(contains(surfaceJointParallaxHlsl(other), "JtI <= 12;"));
    CHECK(contains(surfaceJointShadeHlsl(other), "lerp(1.0, 0.25, JtCav)"));
}
