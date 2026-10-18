#include "ftcore/Version.h"

#include <array>
#include <cstddef>

namespace ftcore {
namespace {

constexpr int appendNumber(std::array<char, 32>& out, int pos, int value) {
    char digits[11]{};
    int count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value > 0);
    while (count > 0) out[static_cast<std::size_t>(pos++)] = digits[--count];
    return pos;
}

constexpr std::array<char, 32> formatVersion(const Version& v) {
    std::array<char, 32> out{};
    int pos = appendNumber(out, 0, v.majorNum);
    out[static_cast<std::size_t>(pos++)] = '.';
    pos = appendNumber(out, pos, v.minorNum);
    out[static_cast<std::size_t>(pos++)] = '.';
    appendNumber(out, pos, v.patchNum);
    return out;
}

constexpr std::array<char, 32> kVersionText = formatVersion(kVersion);

} // namespace

const char* versionString() {
    return kVersionText.data();
}

} // namespace ftcore
