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
    CHECK(surfaceJointShadeFade(spec, 4.f) == doctest::Approx(1.f));
    CHECK(surfaceJointShadeFade(spec, 5.f) == doctest::Approx(0.5f));
    CHECK(surfaceJointShadeFade(spec, 6.f) == doctest::Approx(0.f));
    CHECK(surfaceJointShadeFade(spec, 9.f) == doctest::Approx(0.f));
    // A 90 cm tile: full shading to about 11.7 m (lod 4), half at 23.4 m, none from 46.9 m (lod 6). So the joints
    // still read at 15 m, where the parallax has gone.
    const float tile90 = 90.f;
    CHECK(surfaceJointLod(spec, 1172.f, tile90) == doctest::Approx(4.f).epsilon(1e-3));
    CHECK(surfaceJointLod(spec, 4687.f, tile90) == doctest::Approx(6.f).epsilon(1e-3));
    CHECK(surfaceJointShadeFade(spec, surfaceJointLod(spec, 1100.f, tile90)) == doctest::Approx(1.f));
    CHECK(surfaceJointShadeFade(spec, surfaceJointLod(spec, 1500.f, tile90)) > 0.9f);
    CHECK(surfaceJointShadeFade(spec, surfaceJointLod(spec, 5000.f, tile90)) == doctest::Approx(0.f));
}

TEST_CASE("joints: parallax on a wall seen along a lane: full at 20 to 60 degrees within 12 m") {
    const SurfaceJointSpec spec;
    // A wall at an angle a to the view has dot(N, V) = sin(a).
    for (float deg : {20.f, 30.f, 45.f, 60.f, 90.f}) {
        const float n = std::sin(deg * 3.14159265f / 180.f);
        CHECK(surfaceJointParallaxWeight(spec, 10.f, n, 1.f) == doctest::Approx(1.f));
    }
    // 14.5 degrees (dot 0.25) is the last full one; under 7 degrees (dot 0.12) none.
    CHECK(surfaceJointParallaxWeight(spec, 10.f, std::sin(14.5f * 3.14159265f / 180.f), 1.f) > 0.99f);
    CHECK(surfaceJointParallaxWeight(spec, 10.f, std::sin(6.8f * 3.14159265f / 180.f), 1.f) == doctest::Approx(0.f));
    // At 20 degrees the run per unit depth (tan of the angle from the normal, 2.75) is under maxSlope: not capped.
    CHECK(std::tan(70.f * 3.14159265f / 180.f) < spec.maxSlope);
}

TEST_CASE("joints: uneven depth: a 1.5 m world noise between 1 - uneven and 1, smooth, varied") {
    const SurfaceJointSpec spec;
    float lo = 1.f, hi = 0.f;
    for (int i = 0; i < 400; ++i) {
        const float x = static_cast<float>(i) * 37.f, z = static_cast<float>(i % 23) * 29.f;
        const float d = surfaceJointUneven(spec, x, -250.f, z);
        CHECK(d >= 1.f - spec.uneven - 1e-5f);
        CHECK(d <= 1.f + 1e-5f);
        lo = std::min(lo, d);
        hi = std::max(hi, d);
        // Smooth: a brick's length further on, little change.
        CHECK(std::fabs(surfaceJointUneven(spec, x + 2.f, -250.f, z) - d) < 0.02f);
    }
    CHECK(hi - lo > 0.5f * spec.uneven);
}

TEST_CASE("joints: directional cavity: upper wall darkest, lower edge lighter, soft core, deeper and glancing darker") {
    const SurfaceJointSpec spec;
    const float d = 1.2f;
    // The face: no cavity.
    CHECK(surfaceJointCavity(spec, 1.f, 1.f, 1.f, 1.f, d, 1.f) == doctest::Approx(0.f));
    // A joint's floor, square on: upper wall (brick above, joint below) > middle > lower edge (brick below).
    const float upper = surfaceJointCavity(spec, 0.f, 1.f, 0.f, 0.5f, d, 1.f);
    const float middle = surfaceJointCavity(spec, 0.f, 0.f, 0.f, 0.f, d, 1.f);
    const float lower = surfaceJointCavity(spec, 0.f, 0.f, 1.f, 0.5f, d, 1.f);
    CHECK(upper > lower);
    CHECK(upper == doctest::Approx(1.f));
    CHECK(lower < 0.85f);
    CHECK(lower > 0.4f);
    // Half way up the joint's side, the ring standing higher still darkens it (the soft core).
    CHECK(surfaceJointCavity(spec, 0.5f, 0.5f, 0.5f, 0.9f, d, 1.f) > surfaceJointCavity(spec, 0.5f, 0.5f, 0.5f, 0.5f, d, 1.f));
    CHECK(middle == doctest::Approx(spec.cavityBase));
    CHECK(upper > middle);
    CHECK(middle > lower);
    // 8 mm reads fully, shallower less; a glancing view darkens a partly lit joint more.
    const float half = surfaceJointCavity(spec, 0.5f, 0.5f, 0.5f, 0.5f, 0.8f, 1.f);
    CHECK(half == doctest::Approx(0.5f * spec.cavityBase));
    CHECK(surfaceJointCavity(spec, 0.5f, 0.5f, 0.5f, 0.5f, 0.4f, 1.f) == doctest::Approx(0.25f * spec.cavityBase));
    CHECK(surfaceJointCavity(spec, 0.5f, 0.5f, 0.5f, 0.5f, 0.8f, 0.3f) > half);
    CHECK(surfaceJointCavity(spec, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f) == doctest::Approx(0.f));
}

TEST_CASE("joints: mortar grey scaled by the tint's luminance") {
    const SurfaceJointSpec spec;
    const SurfaceJointRgb white = surfaceJointMortar(spec, 1.f, 1.f, 1.f);
    CHECK(white.r == doctest::Approx(0.42f));
    CHECK(white.g == doctest::Approx(0.40f));
    CHECK(white.b == doctest::Approx(0.37f));
    const SurfaceJointRgb dark = surfaceJointMortar(spec, 0.3f, 0.3f, 0.3f);
    CHECK(dark.r == doctest::Approx(0.3f * 0.42f));
    CHECK(dark.b < white.b);
    CHECK(spec.mortarRough > 0.85f);
    CHECK(spec.mortarBlend == doctest::Approx(0.5f));
}

TEST_CASE("joints: per-unit tone: flat inside a unit, soft at its edges, half a unit offset per course") {
    const SurfaceJointSpec spec;
    const float uu = 4.f, vv = 13.f;
    // Off without units.
    CHECK(surfaceJointUnitTone(spec, 0.3f, 0.3f, 0.f, 13.f, true) == doctest::Approx(0.5f));
    // Flat over the middle of a unit (outside the edge band).
    const float c = surfaceJointUnitTone(spec, 1.5f / uu, 2.5f / vv, uu, vv, false);
    for (float du : {-0.4f, -0.2f, 0.f, 0.2f, 0.4f})
        for (float dv : {-0.4f, 0.f, 0.4f})
            CHECK(surfaceJointUnitTone(spec, (1.5f + du) / uu, (2.5f + dv) / vv, uu, vv, false) == doctest::Approx(c));
    // Soft: one hundredth of a unit never jumps more than a fifth of the range.
    int differ = 0;
    for (int i = 0; i < 2000; ++i) {
        const float u = static_cast<float>(i) / 2000.f, v = 0.37f;
        const float a = surfaceJointUnitTone(spec, u, v, uu, vv, true);
        const float b = surfaceJointUnitTone(spec, u + 0.01f / uu, v, uu, vv, true);
        CHECK(std::fabs(a - b) < 0.2f);
        CHECK(a >= 0.f);
        CHECK(a <= 1.f);
        if (std::fabs(a - c) > 0.05f) ++differ;
    }
    CHECK(differ > 0);
    // Running bond: an odd course's unit is the even course's grid moved by half a unit.
    const float odd = surfaceJointUnitTone(spec, 2.0f / uu, 3.5f / vv, uu, vv, true);   // course 3, mid of a unit
    const float oddFlat = surfaceJointUnitTone(spec, 2.4f / uu, 3.5f / vv, uu, vv, true);
    CHECK(odd == doctest::Approx(oddFlat));  // the unit's middle is at 2.0 cells, not at an edge
    const float evenEdge0 = surfaceJointUnitTone(spec, 1.95f / uu, 2.5f / vv, uu, vv, true);
    const float evenEdge1 = surfaceJointUnitTone(spec, 2.05f / uu, 2.5f / vv, uu, vv, true);
    const float evenFlat0 = surfaceJointUnitTone(spec, 1.7f / uu, 2.5f / vv, uu, vv, true);
    const float evenFlat1 = surfaceJointUnitTone(spec, 2.3f / uu, 2.5f / vv, uu, vv, true);
    // On an even course 2.0 cells is an edge: the tone moves there between the two units' values.
    CHECK(std::min(evenFlat0, evenFlat1) <= std::min(evenEdge0, evenEdge1) + 1e-5f);
    CHECK(std::max(evenEdge0, evenEdge1) <= std::max(evenFlat0, evenFlat1) + 1e-5f);
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
    CHECK(surfaceJointFor("solhaven_soot_brick").depthCm == doctest::Approx(1.2f));
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
    // Units per tile, and the running bond on brick only.
    CHECK(brick.unitsU == doctest::Approx(4.f));
    CHECK(brick.unitsV == doctest::Approx(13.f));
    CHECK(brick.bond);
    const SurfaceJointSurface block = surfaceJointFor("solhaven_breeze_block");
    CHECK(block.unitsU == doctest::Approx(3.f));
    CHECK(block.unitsV == doctest::Approx(5.f));
    CHECK_FALSE(block.bond);
    CHECK(surfaceJointFor("solhaven_ceramic_tile").unitsU == doctest::Approx(8.f));
    CHECK(surfaceJointFor("solhaven_ceramic_tile").unitsV == doctest::Approx(8.f));
    CHECK(surfaceJointFor("solhaven_paving_slab").unitsV == doctest::Approx(4.f));
    CHECK(surfaceJointFor("solhaven_tactile_paving").unitsU == doctest::Approx(4.f));
    CHECK(surfaceJointFor("solhaven_board_concrete").unitsU == doctest::Approx(0.f));
    CHECK(surfaceJointFor("solhaven_asphalt").unitsU == doctest::Approx(0.f));
    CHECK_FALSE(surfaceJointFor("solhaven_paving_slab").bond);
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
    // The uneven noise sits behind the switch too, before the march, and scales the floor's shift.
    const size_t deep = par.find("if (JointDepth > 0.0 && JointDepthCm > 0.0)");
    REQUIRE(deep != std::string::npos);
    CHECK(deep < gate);
    CHECK(par.find("JtDeep = lerp(0.649999976, 1.0, JtNoise);") > deep);
    CHECK(contains(par, "WorldPos / 150.0"));
    CHECK(contains(par, "JointDepthCm / max(TileCm, 1.0) * JtDeep * JointPar"));
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
    CHECK_FALSE(contains(shade, "Mottle[2]"));  // the parallax snippet's JtDeep instead
    CHECK_FALSE(contains(shade, "Texture2DSampleLevel"));
    // Six taps: the pixel, 1.5 texels on u and v, 3 up and down the image, 6 back along u; all inside the branch.
    const size_t branch = shade.find("if (JtShade > 0.0)");
    CHECK(shade.find("Texture2DSample(ArmTex") > branch);
    CHECK(contains(shade, "Texture2DSample(ArmTex, ArmTexSampler, JtUv)"));
    CHECK(contains(shade, "JtUv + float2(0.00146484375, 0.0))"));
    CHECK(contains(shade, "JtUv - float2(0.0, 0.0029296875))"));
    CHECK(contains(shade, "JtUv + float2(0.0, 0.0029296875))"));
    CHECK(contains(shade, "JtUv - float2(0.005859375, 0.0))"));
    size_t taps = 0;
    for (size_t at = shade.find("Texture2DSample("); at != std::string::npos; at = shade.find("Texture2DSample(", at + 1))
        ++taps;
    CHECK(taps == 6);
    for (const char* in : {"JointDepth)", "JointDepthCm", "JointLo", "JointHi", "TileCm", "JtUv", "JtTu", "JtTv",
                           "JtLod", "JtDeep", "JtNdotV", "Mottle[0]", "Tint.rgb", "JointUnitsU", "JointUnitsV",
                           "JointBond", "NormalOut =", "AOOut *=", "RoughOut =", "Color *=", "Color = lerp(Color, float3(0.419999987, 0.400000006, 0.370000005)"})
        CHECK_MESSAGE(contains(shade, in), in);
    // The order: unit tone on the faces, then mortar, then the cavity darkening.
    CHECK(shade.find("JtToneK") < shade.find("JtMortarK"));
    CHECK(shade.find("JtMortarK") < shade.find("float JtCav"));
    CHECK(contains(shade, "RoughOut = lerp(RoughOut, 0.899999976, JtMortarK);"));
    CHECK(contains(shade, "if (JointUnitsU > 0.0 && JointUnitsV > 0.0)"));
    // A spec's numbers reach the text.
    SurfaceJointSpec other;
    other.maxSteps = 12;
    other.cavityAO = 0.25f;
    CHECK(contains(surfaceJointParallaxHlsl(other), "JtI <= 12;"));
    CHECK(contains(surfaceJointShadeHlsl(other), "lerp(1.0, 0.25, JtCav)"));
}
