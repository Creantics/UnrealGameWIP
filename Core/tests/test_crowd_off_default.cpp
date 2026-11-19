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
