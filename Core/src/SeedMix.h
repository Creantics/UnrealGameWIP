// Internal to Core/src: not part of the public ftcore headers.
#pragma once

#include <cstdint>

namespace ftcore::detail {

// Spreads small seeds (1, 2, 3...) over the state, whose first xorshift draws would otherwise be
// near 0 (murmur3 finaliser). Never 0.
inline std::uint32_t mixSeed(std::uint32_t h) {
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h ? h : 1u;
}

} // namespace ftcore::detail
