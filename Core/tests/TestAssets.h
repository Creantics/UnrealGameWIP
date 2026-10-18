#pragma once

// The newengine assets the real-data tests read (the story script, face presets, golden data).
// They stay in the newengine checkout: CMake passes its assets folder as FTCORE_TEST_ASSET_DIR
// (cache variable FTCORE_NEWENGINE_ASSETS), and each test skips when a file is not there.

#include <fstream>
#include <sstream>
#include <string>

namespace ftcore_test {

inline std::string assetPath(const std::string& rel) {
#if defined(FTCORE_TEST_ASSET_DIR)
    return std::string(FTCORE_TEST_ASSET_DIR) + "/" + rel;
#else
    return rel;
#endif
}

// The whole file, or false when it cannot be read.
inline bool readAsset(const std::string& rel, std::string& out) {
    std::ifstream f(assetPath(rel), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

} // namespace ftcore_test
