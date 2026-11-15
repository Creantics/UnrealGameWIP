#pragma once

// FNV-1a 64 over a piece's play state, field by field (never a struct's raw bytes, so padding never counts). A float
// or double 0 is hashed as +0, so -0 and +0 are one state; every other value by its bits. Replay tests compare it
// step by step: two runs from one checkpoint with the same inputs must give the same hash at every step.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace ftcore {

struct StateHash {
    std::uint64_t value = 14695981039346656037ull;

    void bytes(const void* p, std::size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            value ^= b[i];
            value *= 1099511628211ull;
        }
    }
    template <class T>
        requires(std::is_arithmetic_v<T> || std::is_enum_v<T>)
    StateHash& add(T v) {
        if constexpr (std::is_floating_point_v<T>) {
            if (v == T(0)) v = T(0);
        }
        if constexpr (std::is_same_v<T, bool>) {
            const unsigned char c = v ? 1 : 0;
            bytes(&c, 1);
        } else {
            unsigned char raw[sizeof(T)];
            std::memcpy(raw, &v, sizeof(T));
            bytes(raw, sizeof(T));
        }
        return *this;
    }
    template <class T, std::size_t N>
        requires(std::is_arithmetic_v<T> || std::is_enum_v<T>)
    StateHash& add(const std::array<T, N>& a) {
        for (const T& v : a) add(v);
        return *this;
    }
};

} // namespace ftcore
