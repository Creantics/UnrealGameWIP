#pragma once

#include "ftcore/Api.h"

namespace ftcore {

struct Version {
    int majorNum;
    int minorNum;
    int patchNum;
};

inline constexpr Version kVersion{0, 1, 0};

// "major.minor.patch" for kVersion. Points at static storage; never null.
FTCORE_API const char* versionString();

// Negative if a < b, zero if equal, positive if a > b.
constexpr int compareVersions(const Version& a, const Version& b) {
    if (a.majorNum != b.majorNum) return a.majorNum - b.majorNum;
    if (a.minorNum != b.minorNum) return a.minorNum - b.minorNum;
    return a.patchNum - b.patchNum;
}

} // namespace ftcore
