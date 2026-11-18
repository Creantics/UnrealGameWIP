#include <doctest/doctest.h>

#include "AllocCounter.h"
#include "ftcore/PropGloss.h"
#include "ftcore/SurfaceWeather.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

// A deterministic spread of points (a 32-bit LCG), metres.
struct PgPoints {
    std::uint32_t s = 12345u;
    float next(float range) {
        s = s * 1664525u + 1013904223u;
        return range * static_cast<float>(s >> 8) / 16777216.f;
    }
};

PropGlossResult check(std::string_view name, bool hasMap, float p10, float p50, float p90, float metallic) {
    PropGlossInput in;
    in.name = name;
    in.hasMap = hasMap;
    in.p10 = p10;
    in.p50 = p50;
    in.p90 = p90;
    in.metallic = metallic;
    return propGlossCheck(in);
}

PropGlossResult flat(std::string_view name, float rough, float metallic) { return check(name, false, rough, rough, rough, metallic); }

}  // namespace

TEST_CASE("prop gloss: names are matched on whole tokens, with a few endings") {
    // The metals that stay metal, and the ones that are borderline.
    for (const char* name : {"hc_iron", "lantern_iron", "hc_metal", "post_iron", "hc_brass", "hb_bronze", "LanternIron", "hcIronHook",
                             "IRONWORK", "ironwork_gate", "M_Iron01", "bk3iron", "steel2", "hb_galvanised_pail", "CopperPot",
                             "gold_bell", "chromed_tap", "SILVERWARE", "chrome_rim", "metallic_paint"})
        CHECK_MESSAGE(propNameMetal(name), name);
    // Not metals: no token is one (no substring match).
    for (const char* name : {"hc_signink", "hc_lens", "hc_broth", "hc_reflector", "hb_pane", "hb_ring", "ironically", "tiny_box",
                             "goldfish", "alum_block", "golden_bell", "irony", "silvery_glaze", "CopperyLeaf", "LeadenSky", "steely", "tripo_material_6f1c2a90", "BK-M_LOD0_M", "", "_"})
        CHECK_MESSAGE(!propNameMetal(name), name);
    // Liquids take no grime; oiled timber, stockpots and teak are not liquids.
    for (const char* name : {"hc_broth_water", "hc_broth", "hb_water", "SoupSurface", "liquid_01"})
        CHECK_MESSAGE(propNameLiquid(name), name);
    for (const char* name : {"hc_oiled_timber", "hc_pot_metal", "SM_HC_StockPot", "teak_board", "hc_glaze_brown", ""})
        CHECK_MESSAGE(!propNameLiquid(name), name);
    // The smooth whitelist.
    for (const char* name : {"hc_glaze_brown", "hc_glaze_green", "hc_glazed_bowl", "hc_glass", "jpk_glass", "lamp_glass", "GlassPane",
                             "glass2", "glasses", "glassware", "hb_water", "polished_floor", "chrome_rim", "MIRROR"})
        CHECK_MESSAGE(propNameSmooth(name), name);
    for (const char* name : {"hc_lens", "hb_pane", "hc_broth", "hc_bisque", "hc_reflector", "glassblower", "hc_brass", "hb_bronze",
                             "tripo_material_6f1c2a90", ""})
        CHECK_MESSAGE(!propNameSmooth(name), name);
}

TEST_CASE("prop gloss: the borderline materials, at the values they had and at Lane A's new ones") {
    // Before the retune (the brief's list): all but bronze fail.
    CHECK(flat("hc_lens", 0.08f, 0.f).failed == PropGlossRule::Glossy);
    CHECK(flat("hc_broth", 0.08f, 0.f).failed == PropGlossRule::Glossy);
    CHECK(flat("hc_reflector", 0.25f, 0.f).failed == PropGlossRule::Glossy);
    CHECK(flat("hb_pane", 0.12f, 0.f).failed == PropGlossRule::Glossy);
    CHECK(flat("hc_brass", 0.38f, 1.f).failed == PropGlossRule::Glossy);
    CHECK(flat("hb_ring", 0.35f, 0.f).failed == PropGlossRule::Glossy);
    const PropGlossResult bronze = flat("hb_bronze", 0.40f, 1.f);  // on the edge: passes
    CHECK(bronze.pass);
    CHECK(bronze.metal);
    // The sign ink authored as metal fails on the metal rule first (it is glossy too).
    const PropGlossResult ink = flat("hc_signink", 0.32f, 1.f);
    CHECK(!ink.pass);
    CHECK(ink.failed == PropGlossRule::Metal);
    CHECK(ink.wrongMetal);
    CHECK(ink.glossy);
    CHECK(flat("hc_signink", 0.60f, 0.f).pass);
    // After: the lens, reflector, ring and brass pass; the pane (glass by use) and the broth (a liquid) still fail on
    // the briefed whitelist.
    CHECK(flat("hc_lens", 0.42f, 0.f).pass);
    CHECK(flat("hc_reflector", 0.42f, 0.f).pass);
    CHECK(flat("hb_ring", 0.50f, 0.f).pass);
    CHECK(flat("hc_brass", 0.45f, 1.f).pass);
    CHECK(flat("hb_bronze", 0.50f, 1.f).pass);
    CHECK(flat("hb_pane", 0.15f, 0.f).failed == PropGlossRule::Glossy);
    CHECK(flat("hc_broth", 0.08f, 0.f).failed == PropGlossRule::Glossy);
    // Lane A's renames (cfb68675): the pane and the broth pass on a whole smooth token, and the pot and the bell, metal
    // 1, pass on a whole metal token, where the run-together names failed the metal rule.
    CHECK(flat("hb_glass_pane", 0.15f, 0.f).pass);
    CHECK(flat("hc_broth_water", 0.08f, 0.f).pass);
    CHECK(flat("tower_bell_bronze", 0.50f, 1.f).pass);
    CHECK(check("hc_pot_metal", true, 0.38f, 0.48f, 0.60f, 1.f).pass);
    CHECK(check("hc_pot_metal_soot", true, 0.40f, 0.54f, 0.88f, 0.9f).pass);
    CHECK(flat("tower_bell", 0.50f, 1.f).failed == PropGlossRule::Metal);
    CHECK(check("hc_potmetal", true, 0.38f, 0.48f, 0.60f, 1.f).failed == PropGlossRule::Metal);
    // The metals that stay metal pass at their authored values; glass and glaze may be smooth.
    CHECK(flat("hc_iron", 0.65f, 1.f).pass);
    CHECK(flat("lantern_iron", 0.62f, 1.f).pass);
    CHECK(flat("hc_metal", 0.55f, 1.f).pass);
    CHECK(flat("post_iron", 0.62f, 1.f).pass);
    CHECK(flat("hc_glass", 0.06f, 0.f).pass);
    CHECK(flat("hc_glaze_brown", 0.28f, 0.f).pass);
    CHECK(flat("hc_lacquer", 0.45f, 0.f).pass);
    // A metal name may be glossy only if it is also on the smooth list.
    CHECK(flat("hc_iron", 0.2f, 1.f).failed == PropGlossRule::Glossy);
    CHECK(flat("chrome_rim", 0.1f, 1.f).pass);
}

TEST_CASE("prop gloss: the spread rule is for mapped materials only, and the reason says which rule failed") {
    const PropGlossResult factor = flat("hc_timber", 0.6f, 0.f);
    CHECK(factor.pass);
    CHECK(!factor.spreadChecked);
    // Today's timber map, 0.54 to 0.62: too flat.
    const PropGlossResult narrow = check("hc_timber", true, 0.54f, 0.58f, 0.62f, 0.f);
    CHECK(narrow.spreadChecked);
    CHECK(narrow.flatMap);
    CHECK(narrow.failed == PropGlossRule::FlatMap);
    CHECK(check("hc_timber", true, 0.62f, 0.72f, 0.80f, 0.f).pass);
    // A smooth name skips the spread (an even glaze is right).
    const PropGlossResult glaze = check("hc_glaze_brown", true, 0.26f, 0.28f, 0.30f, 0.f);
    CHECK(glaze.pass);
    CHECK(!glaze.spreadChecked);
    // A glossy median fails before the spread is looked at.
    CHECK(check("hc_timber", true, 0.30f, 0.32f, 0.34f, 0.f).failed == PropGlossRule::Glossy);
    // A NaN fails rather than slipping through.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(flat("hc_timber", nan, 0.f).failed == PropGlossRule::Glossy);
    CHECK(flat("hc_timber", 0.6f, nan).failed == PropGlossRule::Metal);
    CHECK(std::string(propGlossReason(PropGlossRule::None)).empty());
    for (PropGlossRule rule : {PropGlossRule::Metal, PropGlossRule::Glossy, PropGlossRule::FlatMap})
        CHECK(!std::string(propGlossReason(rule)).empty());
}

TEST_CASE("prop gloss: histogram percentiles") {
    std::array<std::uint32_t, 256> hist{};
    CHECK(propHistogramPercentile(hist, 0.5f) == 0.f);
    hist[128] = 1000;
    CHECK(propHistogramPercentile(hist, 0.1f) == doctest::Approx(128.f / 255.f));
    CHECK(propHistogramPercentile(hist, 0.9f) == doctest::Approx(128.f / 255.f));
    hist.fill(1u);
    CHECK(propHistogramPercentile(hist, 0.f) == 0.f);
    CHECK(propHistogramPercentile(hist, 0.1f) == doctest::Approx(25.f / 255.f));
    CHECK(propHistogramPercentile(hist, 0.5f) == doctest::Approx(127.f / 255.f));
    CHECK(propHistogramPercentile(hist, 0.9f) == doctest::Approx(230.f / 255.f));
    CHECK(propHistogramPercentile(hist, 1.f) == doctest::Approx(1.f));
    hist.fill(0u);
    hist[100] = 90;
    hist[200] = 10;
    CHECK(propHistogramPercentile(hist, 0.5f) == doctest::Approx(100.f / 255.f));
    CHECK(propHistogramPercentile(hist, 0.9f) == doctest::Approx(100.f / 255.f));
    CHECK(propHistogramPercentile(hist, 0.95f) == doctest::Approx(200.f / 255.f));
}

TEST_CASE("prop gloss: grime per world") {
    CHECK(propGrimeFor(AssetPack::Solhaven) == 1.f);
    CHECK(propGrimeFor(AssetPack::InkSolhaven) == 1.f);
    CHECK(propGrimeFor(AssetPack::None) == 1.f);
    CHECK(propGrimeFor(AssetPack::Lattice) == 0.f);
    CHECK(propGrimeFor(AssetPack::Unwritten) == 0.f);
    CHECK(propGrimeFor(AssetPack::Restored) == 0.f);
}

TEST_CASE("prop gloss: the 3D lattice is the street lattice at z 0, and the noise is smooth and in range") {
    for (int x : {-5, 0, 3, 1000})
        for (int y : {-7, 0, 5, 222}) CHECK(propLatticeValue(x, y, 0) == streetLatticeValue(x, y));
    CHECK(propLatticeValue(3, 5, 1) != propLatticeValue(3, 5, 0));
    CHECK(propLatticeValue(3, 5, -1) != propLatticeValue(3, 5, 0));
    CHECK(propNoise(3.f, 5.f, 2.f) == propLatticeValue(3, 5, 2));
    CHECK(propNoise(-4.f, 9.f, -1.f) == propLatticeValue(-4, 9, -1));
    PgPoints pts;
    for (int k = 0; k < 500; ++k) {
        const float x = pts.next(40.f) - 20.f, y = pts.next(40.f) - 20.f, z = pts.next(10.f) - 5.f;
        const float v = propNoise(x, y, z);
        CHECK(v >= 0.f);
        CHECK(v <= 1.f);
        CHECK(std::fabs(propNoise(x + 1e-3f, y, z) - v) < 0.01f);
        CHECK(std::fabs(propNoise(x, y, z + 1e-3f) - v) < 0.01f);
        const float lv = propLatticeValue(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), static_cast<int>(std::floor(z)));
        CHECK(lv >= 0.f);
        CHECK(lv <= 1.f);
    }
}

TEST_CASE("prop gloss: every layer is neutral at its parameter's 0") {
    PgPoints pts;
    for (int k = 0; k < 300; ++k) {
        PropSurfaceIn in;
        in.albedo = {pts.next(1.f), pts.next(1.f), pts.next(1.f)};
        in.rough = pts.next(1.f);
        in.metallic = pts.next(1.f) < 0.5f ? 0.f : 1.f;
        in.ao = pts.next(1.f);
        in.xM = pts.next(30.f);
        in.yM = pts.next(30.f);
        in.zM = pts.next(5.f);
        in.normalZ = 2.f * pts.next(1.f) - 1.f;
        // Wet weather under cover (Weather 0) is dry too.
        in.wetness = (k % 2) ? 0.f : 1.f;
        in.weather = (k % 2) ? 1.f : 0.f;
        const PropSurfaceOut out = propSurface(in);
        for (int c = 0; c < 3; ++c) CHECK(out.albedo[c] == in.albedo[c]);
        CHECK(out.rough == in.rough);
        CHECK(out.breakup == 0.f);
        CHECK(out.dust == 0.f);
        CHECK(out.soak == 0.f);
    }
}

TEST_CASE("prop gloss: the break-up spreads a flat roughness by about +-0.08 and leaves a smooth one alone") {
    const auto sample = [](float rough, float ao, std::vector<float>& rs) {
        PgPoints pts;
        rs.clear();
        for (int k = 0; k < 4000; ++k) {
            PropSurfaceIn in;
            in.rough = rough;
            in.ao = ao;
            in.breakup = 1.f;
            in.xM = pts.next(12.f);
            in.yM = pts.next(12.f);
            in.zM = pts.next(3.f);
            rs.push_back(propSurface(in).rough);
        }
    };
    std::vector<float> rs;
    sample(0.6f, 1.f, rs);
    double sum = 0.0;
    float most = 0.f;
    for (float r : rs) {
        CHECK(std::fabs(r - 0.6f) <= 0.08f + 1e-5f);
        most = std::max(most, std::fabs(r - 0.6f));
        sum += r;
    }
    const double mean = sum / static_cast<double>(rs.size());
    CHECK(mean == doctest::Approx(0.6).epsilon(0.03));
    CHECK(most > 0.05f);
    std::sort(rs.begin(), rs.end());
    const float p10 = rs[rs.size() / 10], p90 = rs[rs.size() * 9 / 10];
    CHECK(p90 - p10 >= 0.04f);
    CHECK(p90 - p10 <= 0.16f);
    // Cavities (AO 0) push it rougher.
    sample(0.6f, 0.f, rs);
    double sumCav = 0.0;
    for (float r : rs) sumCav += r;
    CHECK(sumCav / static_cast<double>(rs.size()) > mean + 0.02);
    // Under 0.4 it fades: a quarter of the amplitude at 0.1, none at 0.
    sample(0.1f, 1.f, rs);
    for (float r : rs) CHECK(std::fabs(r - 0.1f) <= 0.02f + 1e-5f);
    sample(0.f, 1.f, rs);
    for (float r : rs) CHECK(r == 0.f);
}

TEST_CASE("prop gloss: dust sits on up faces and in cavities, rougher by up to 0.2, greyer and toward the dust colour") {
    PropSurfaceIn in;
    in.rough = 0.5f;
    in.grime = 1.f;
    in.albedo = {0.6f, 0.3f, 0.1f};
    // An underside out of any cavity: clean.
    in.normalZ = -1.f;
    PropSurfaceOut out = propSurface(in);
    CHECK(out.dust == 0.f);
    CHECK(out.rough == 0.5f);
    for (int c = 0; c < 3; ++c) CHECK(out.albedo[c] == in.albedo[c]);
    // The same underside in a full cavity: the cavity's dust.
    in.ao = 0.f;
    out = propSurface(in);
    CHECK(out.dust == doctest::Approx(0.8f));
    CHECK(out.rough == doctest::Approx(0.5f + 0.2f * 0.8f));
    // Up faces, sampled on the dust patches' lattice: between the clean patches' floor and full dust.
    in.ao = 1.f;
    in.normalZ = 1.f;
    float lo = 1.f, hi = 0.f;
    for (int i = 0; i < 12; ++i)
        for (int j = 0; j < 12; ++j) {
            in.xM = 0.5f * static_cast<float>(i);
            in.yM = 0.5f * static_cast<float>(j);
            out = propSurface(in);
            const float added = out.rough - 0.5f;
            CHECK(added >= 0.4f * 0.2f - 1e-5f);
            CHECK(added <= 0.2f + 1e-5f);
            lo = std::min(lo, added);
            hi = std::max(hi, added);
            // Less saturated than it was.
            const float spread = std::max({out.albedo[0], out.albedo[1], out.albedo[2]}) -
                                 std::min({out.albedo[0], out.albedo[1], out.albedo[2]});
            CHECK(spread < 0.5f);
        }
    CHECK(lo <= 0.09f);
    CHECK(hi >= 0.19f);
    // Half the grime, half the dust; black goes toward the dust colour; roughness stops at 1.
    in.xM = 1.3f;
    in.yM = 2.9f;
    const float full = propSurface(in).dust;
    in.grime = 0.5f;
    CHECK(propSurface(in).dust == doctest::Approx(0.5f * full));
    in.grime = 1.f;
    in.albedo = {0.f, 0.f, 0.f};
    out = propSurface(in);
    CHECK(out.albedo[0] > 0.f);
    CHECK(out.albedo[0] > out.albedo[2]);  // the dust colour is warm
    in.rough = 0.95f;
    in.xM = 0.f;
    in.yM = 0.f;
    CHECK(propSurface(in).rough <= 1.f);
}

TEST_CASE("prop gloss: wet props soak on top, darken and smooth toward 0.35, never under 0.25 or under glass's own") {
    // Never under min(dry, 0.25), never rougher than dry, whatever the facing, dust or WetRough.
    PgPoints pts;
    for (float rough : {0.05f, 0.2f, 0.25f, 0.3f, 0.6f, 0.9f})
        for (float wetRough : {0.f, 0.1f, 0.35f, 0.6f})
            for (float nz : {1.f, 0.5f, 0.f, -1.f})
                for (float grime : {0.f, 1.f})
                    for (int k = 0; k < 8; ++k) {
                        PropSurfaceIn in;
                        in.rough = rough;
                        in.wetRough = wetRough;
                        in.normalZ = nz;
                        in.grime = grime;
                        in.breakup = 1.f;
                        in.xM = pts.next(20.f);
                        in.yM = pts.next(20.f);
                        in.zM = pts.next(4.f);
                        const float dry = propSurface(in).rough;
                        in.wetness = 1.f;
                        const PropSurfaceOut wet = propSurface(in);
                        CHECK(wet.rough >= std::min(dry, 0.25f) - 1e-6f);
                        CHECK(wet.rough <= dry + 1e-6f);
                        CHECK(wet.soak >= 0.f);
                        CHECK(wet.soak <= 1.f);
                    }
    // Glass stays as smooth as it was, no smoother.
    PropSurfaceIn glass;
    glass.rough = 0.05f;
    glass.wetness = 1.f;
    CHECK(propSurface(glass).rough == 0.05f);
    // Timber on an up face, on the drying patches' lattice: the soaked patches reach WetRough, the damp ones less.
    PropSurfaceIn timber;
    timber.rough = 0.75f;
    timber.wetness = 1.f;
    timber.albedo = {0.4f, 0.3f, 0.2f};
    float lowest = 1.f, highest = 0.f;
    for (int i = 0; i < 10; ++i)
        for (int j = 0; j < 10; ++j) {
            timber.xM = 2.f * static_cast<float>(i);
            timber.yM = 2.f * static_cast<float>(j);
            const PropSurfaceOut out = propSurface(timber);
            CHECK(out.rough >= 0.35f - 1e-5f);
            CHECK(out.albedo[0] >= 0.6f * 0.4f - 1e-6f);
            CHECK(out.albedo[0] < 0.4f);
            lowest = std::min(lowest, out.rough);
            highest = std::max(highest, out.rough);
        }
    CHECK(lowest == doctest::Approx(0.35f).epsilon(0.003));
    CHECK(highest < 0.75f);
    // An underside stays dry; a side soaks at most sideShare.
    timber.xM = 0.f;
    timber.yM = 0.f;
    timber.normalZ = -1.f;
    PropSurfaceOut under = propSurface(timber);
    CHECK(under.soak == 0.f);
    CHECK(under.rough == 0.75f);
    CHECK(under.albedo[0] == 0.4f);
    for (int i = 0; i < 10; ++i) {
        timber.normalZ = 0.f;
        timber.xM = 2.f * static_cast<float>(i);
        const PropSurfaceOut side = propSurface(timber);
        CHECK(side.soak <= 0.35f + 1e-6f);
        CHECK(side.soak > 0.f);
    }
    // A metal darkens far less than a dielectric at the same soak.
    timber.normalZ = 1.f;
    timber.xM = 0.f;
    const PropSurfaceOut wood = propSurface(timber);
    timber.metallic = 1.f;
    const PropSurfaceOut metal = propSurface(timber);
    CHECK(metal.albedo[0] > wood.albedo[0]);
    CHECK(metal.albedo[0] >= 0.4f * (1.f - 0.2f * 0.4f) - 1e-6f);
}

TEST_CASE("prop gloss: dust keeps the soak patchy") {
    PropSurfaceIn in;
    in.wetness = 1.f;
    in.rough = 0.7f;
    PgPoints pts;
    int patchy = 0;
    for (int k = 0; k < 400; ++k) {
        in.xM = pts.next(10.f);
        in.yM = pts.next(10.f);
        in.grime = 0.f;
        const float clean = propSurface(in).soak;
        in.grime = 1.f;
        const float dusty = propSurface(in).soak;
        CHECK(dusty <= clean + 1e-6f);
        if (dusty < 0.8f * clean) ++patchy;
    }
    CHECK(patchy > 20);
}

TEST_CASE("prop gloss: the maths allocates nothing; the shader block holds the street lattice and the inputs") {
    const auto before = ftcore_test::allocStats().count;
    float sum = 0.f;
    std::array<std::uint32_t, 256> hist{};
    for (int k = 0; k < 64; ++k) {
        PropSurfaceIn in;
        in.rough = static_cast<float>(k) / 63.f;
        in.breakup = 1.f;
        in.grime = 1.f;
        in.wetness = 1.f;
        in.xM = 0.37f * static_cast<float>(k);
        in.normalZ = 1.f - 2.f * static_cast<float>(k) / 63.f;
        sum += propSurface(in).rough;
        const PropGlossResult r = flat("hc_glaze_brown_lantern_iron", in.rough, 0.f);
        sum += r.pass ? 1.f : 0.f;
        sum += propNameMetal("SomeLongTripoName_tripo_material_0042") ? 1.f : 0.f;
        hist[static_cast<std::size_t>(k * 4)] += 1u;
        sum += propHistogramPercentile(hist, 0.5f) + propGrimeFor(AssetPack::Solhaven) + propNoise(in.xM, 1.f, 2.f);
    }
    const auto after = ftcore_test::allocStats().count;
    CHECK(sum > 0.f);
    if (ftcore_test::allocHookInstalled()) CHECK(after == before);
    const std::string code = propSurfaceHlsl();
    CHECK(code.find(streetLatticeHlsl()) != std::string::npos);
    CHECK(code.find("struct FTPropNoise") != std::string::npos);
    CHECK(code.find("float L3(int3 c)") != std::string::npos);
    for (const char* name : {"Albedo", "Rough", "Metal", "AO", "WorldPos", "VertexNormal", "Wetness", "Weather", "Breakup", "Grime",
                             "DustColor", "WetRough"})
        CHECK_MESSAGE(code.find(name) != std::string::npos, name);
    CHECK(code.find("max(WetRough, 0.25)") != std::string::npos);
    CHECK(code.find("RoughOut = saturate(R);") != std::string::npos);
    CHECK(code.find("return C;") != std::string::npos);
    // No puddles on a prop.
    CHECK(code.find("Pud") == std::string::npos);
}
