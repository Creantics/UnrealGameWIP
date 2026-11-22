#pragma once

// A strict read of a numeric command-line flag ("-Max=21"), for tools whose engine parser takes any text as a number
// (FParse::Value(float) reads "abc" or an empty value as 0 and says it was given). The key is found the way FParse::Value
// finds it: case-insensitive, and only where the character before it is not a letter or digit, so "Damping=" never
// matches inside "LocalDamping=". The value runs to the next space or the end; it must be one whole finite number.
// Plain C++20, header only, no allocation, no exceptions.

#include <charconv>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <system_error>

namespace ftcore::flagnum {

enum class Read {
    Absent, // the key is not on the line
    Number, // given, and a whole finite number
    Bad,    // given, but empty or not a number
};

struct Value {
    Read read = Read::Absent;
    float value = 0.f;
    std::string_view text{}; // the value as written (empty when absent)
};

namespace detail {
constexpr char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
constexpr bool alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
constexpr bool sameAt(std::string_view line, std::size_t at, std::string_view key) {
    if (at + key.size() > line.size()) {
        return false;
    }
    for (std::size_t i = 0; i < key.size(); ++i) {
        if (lower(line[at + i]) != lower(key[i])) {
            return false;
        }
    }
    return true;
}
} // namespace detail

// Where the key's first match with a clean left edge starts, or npos.
constexpr std::size_t findKey(std::string_view line, std::string_view key) {
    if (key.empty()) {
        return std::string_view::npos;
    }
    for (std::size_t at = 0; at + key.size() <= line.size(); ++at) {
        if ((at == 0 || !detail::alnum(line[at - 1])) && detail::sameAt(line, at, key)) {
            return at;
        }
    }
    return std::string_view::npos;
}

// The flag's value. `key` includes its '=' ("Max=").
inline Value read(std::string_view line, std::string_view key) {
    const std::size_t at = findKey(line, key);
    if (at == std::string_view::npos) {
        return {};
    }
    const std::size_t from = at + key.size();
    std::size_t to = from;
    while (to < line.size() && !detail::space(line[to])) {
        ++to;
    }
    Value out;
    out.text = line.substr(from, to - from);
    out.read = Read::Bad;
    if (out.text.empty()) {
        return out;
    }
    float v = 0.f;
    const char* first = out.text.data();
    const char* last = first + out.text.size();
    if (*first == '+') { // from_chars takes no '+'; a written "+0.5" is still a number
        ++first;
    }
    const auto [end, ec] = std::from_chars(first, last, v);
    if (ec == std::errc() && end == last && first != last && std::isfinite(v)) {
        out.read = Read::Number;
        out.value = v;
    }
    return out;
}

// The index in keys[0..count) of the first flag given with a value that is not a number, or -1.
inline int firstBad(std::string_view line, const std::string_view* keys, std::size_t count) {
    for (std::size_t i = 0; keys != nullptr && i < count; ++i) {
        if (read(line, keys[i]).read == Read::Bad) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace ftcore::flagnum
