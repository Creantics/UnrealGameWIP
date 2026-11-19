// The crowd rework is off by default: without -CrowdV2 the game runs ftcore::CrowdSim on Data/Crowd/crowd.json, both as
// they were before the rework; the rework (ftcore::crowdv2) reads its own Data/Crowd/crowd_v2.json. Both headers in one
// unit, so every crowd name here is qualified.
#include "ftcore/Crowd.h"
#include "ftcore/CrowdV2.h"

#include <doctest/doctest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string readCrowdFile(const char* name) {
    std::ifstream f(std::string(FTCORE_PROJECT_DATA_DIR) + "/Crowd/" + name, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("crowd off by default: crowd.json is the tuning from before the rework, read by the old importer") {
    const std::string text = readCrowdFile("crowd.json");
    REQUIRE(!text.empty());
    ftcore::CrowdTuning t;
    std::string err;
    REQUIRE_MESSAGE(ftcore::importCrowdTuning(text, t, &err), err);
    // The values the rework changed, as they were before it.
    CHECK(t.reactBase == doctest::Approx(0.2f));
    CHECK(t.reactJitter == doctest::Approx(0.35f));
    CHECK(t.threatMemory == doctest::Approx(20.f));
    CHECK(t.safeDist == doctest::Approx(60.f));
}

TEST_CASE("crowd off by default: the rework's tuning is crowd_v2.json, which the old importer refuses") {
    const std::string text = readCrowdFile("crowd_v2.json");
    REQUIRE(!text.empty());
    ftcore::crowdv2::CrowdTuning v2;
    std::string err;
    REQUIRE_MESSAGE(ftcore::crowdv2::importCrowdTuning(text, v2, &err), err);
    CHECK(v2.reactBase == doctest::Approx(0.1f));
    CHECK(v2.safeDist == doctest::Approx(22.f));
    CHECK(v2.minGap == doctest::Approx(0.7f));

    // Its new members (minGap, quietAfter, wallInset, ...) are unknown to the old importer: the two can't be mixed up.
    ftcore::CrowdTuning old;
    std::string oldErr;
    CHECK_FALSE(ftcore::importCrowdTuning(text, old, &oldErr));
    CHECK(!oldErr.empty());
}

TEST_CASE("crowd start: flags beat the map default; -CrowdV2 keeps the old crowd off; -NoCrowdDefault turns the default off") {
    using ftcore::CrowdStart;
    using ftcore::CrowdStartFlags;
    // Every combination of the four inputs.
    for (int bits = 0; bits < 16; ++bits) {
        CrowdStartFlags f;
        f.v2 = (bits & 1) != 0;
        f.streets = (bits & 2) != 0;
        f.noDefault = (bits & 4) != 0;
        f.mapDefault = (bits & 8) != 0;
        CrowdStart want = CrowdStart::Off;
        if (!f.v2 && f.streets) want = CrowdStart::Streets;
        if (!f.v2 && !f.streets && f.mapDefault && !f.noDefault) want = CrowdStart::MapDefault;
        INFO("v2 " << f.v2 << " streets " << f.streets << " noDefault " << f.noDefault << " mapDefault " << f.mapDefault);
        CHECK(ftcore::crowdStart(f) == want);
    }
    // The cases by name. A map with no local streets file has no crowd, as before.
    CHECK(ftcore::crowdStart(CrowdStartFlags{}) == CrowdStart::Off);
    CrowdStartFlags strip;
    strip.mapDefault = true;
    CHECK(ftcore::crowdStart(strip) == CrowdStart::MapDefault);
    strip.noDefault = true;
    CHECK(ftcore::crowdStart(strip) == CrowdStart::Off);
    strip.noDefault = false;
    strip.streets = true; // a test run's own file wins, with its hard failure
    CHECK(ftcore::crowdStart(strip) == CrowdStart::Streets);
    strip.v2 = true;
    CHECK(ftcore::crowdStart(strip) == CrowdStart::Off);
}
