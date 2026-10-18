#include <doctest/doctest.h>

#include "ftcore/Version.h"

#include <cstdio>
#include <cstring>

TEST_CASE("versionString formats kVersion as major.minor.patch") {
    char expected[32];
    std::snprintf(expected, sizeof(expected), "%d.%d.%d", ftcore::kVersion.majorNum,
                  ftcore::kVersion.minorNum, ftcore::kVersion.patchNum);
    CHECK(std::strcmp(ftcore::versionString(), expected) == 0);
}

TEST_CASE("compareVersions orders by major, then minor, then patch") {
    using ftcore::Version;
    CHECK(ftcore::compareVersions(Version{1, 2, 3}, Version{1, 2, 3}) == 0);
    CHECK(ftcore::compareVersions(Version{1, 2, 3}, Version{1, 2, 4}) < 0);
    CHECK(ftcore::compareVersions(Version{1, 3, 0}, Version{1, 2, 9}) > 0);
    CHECK(ftcore::compareVersions(Version{2, 0, 0}, Version{1, 9, 9}) > 0);
    static_assert(ftcore::compareVersions(Version{0, 1, 0}, Version{0, 0, 9}) > 0);
}
