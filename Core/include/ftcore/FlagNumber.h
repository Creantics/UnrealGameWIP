#pragma once

// A strict read of a numeric command-line flag ("-Max=21"), for tools whose engine parser takes any text as a number
// (FParse::Value(float) reads "abc" or an empty value as 0 and says it was given). The key and its value are found the
// way FParse::Value finds them (UE 5.8: FCString::Strifind with quoted text skipped, then FParse::Value's TCHAR read):
// the key case-insensitive, only where the character before it is not a letter or digit (so "Damping=" never matches
// inside "LocalDamping=") and never inside a quoted stretch; a value in quotes (-Max="21") runs to the closing quote,
// any other starts after any spaces and runs to a space, a comma or a ')'. Stricter than FParse in what passes: the
// value must be one whole finite number, a quoted one must close and be followed by a space or the end, and an unquoted
// one must end at a space or the end (FParse would read "-Max=1,5" as 1). A flag FParse would not see because it sits
// in quotes ("-Max=21" whole, or after a stray quote) is Hidden, not Absent, so a caller can refuse it rather than build
// with the default. One difference is left: FParse copies at most 255 characters of a value, this reads the whole run
// (a numeric value that long is refused either way). Plain C++20, header only, no allocation, no exceptions.

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
    Hidden, // written as a flag ("-Max=") but inside a quoted stretch, so FParse would not read it
};

struct Value {
    Read read = Read::Absent;
    float value = 0.f;
    std::string_view text{};    // the value FParse would take, without its quotes (empty when absent)
    std::string_view written{}; // the value as written, quotes and all, to the next space (for an error message)
};

namespace detail {
constexpr char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
constexpr bool alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
constexpr bool stop(char c) { return space(c) || c == ',' || c == ')'; } // FParse's value terminators
constexpr bool digit(char c) { return c >= '0' && c <= '9'; }
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

// Where the key's first match with a clean left edge, outside any quoted stretch, starts, or npos. With inQuotes, the
// first match that is inside one and written as a flag (a '-' right before it) instead.
constexpr std::size_t findKey(std::string_view line, std::string_view key, bool inQuotes = false) {
    if (key.empty()) {
        return std::string_view::npos;
    }
    bool quoted = false;
    for (std::size_t at = 0; at + key.size() <= line.size(); ++at) {
        const bool edge = inQuotes ? (quoted && at > 0 && line[at - 1] == '-')
                                   : (!quoted && (at == 0 || !detail::alnum(line[at - 1])));
        if (edge && detail::sameAt(line, at, key)) {
            return at;
        }
        if (line[at] == '"') {
            quoted = !quoted;
        }
    }
    return std::string_view::npos;
}

// Whether every quote on the line is closed (FParse treats the rest of a line after a stray quote as quoted).
constexpr bool quotesClosed(std::string_view line) {
    bool quoted = false;
    for (const char c : line) {
        quoted = c == '"' ? !quoted : quoted;
    }
    return !quoted;
}

// The flag's value. `key` includes its '=' ("Max=").
inline Value read(std::string_view line, std::string_view key) {
    const std::size_t at = findKey(line, key);
    if (at == std::string_view::npos) {
        Value hidden;
        hidden.read = findKey(line, key, true) == std::string_view::npos ? Read::Absent : Read::Hidden;
        return hidden;
    }
    std::size_t from = at + key.size();
    std::size_t to = from;
    bool ends = false; // the value ends where FParse's does, and nothing FParse would drop follows it
    if (from < line.size() && line[from] == '"') {
        ++from;
        to = line.find('"', from);
        ends = to != std::string_view::npos && (to + 1 == line.size() || detail::space(line[to + 1]));
        to = to == std::string_view::npos ? line.size() : to;
    } else {
        while (from < line.size() && detail::space(line[from])) {
            ++from;
        }
        to = from;
        while (to < line.size() && !detail::stop(line[to])) {
            ++to;
        }
        ends = to == line.size() || detail::space(line[to]);
    }
    Value out;
    out.text = line.substr(from, to - from);
    {
        const std::size_t eq = at + key.size(); // first character after the '='
        const std::size_t start = eq < line.size() && line[eq] == '"' ? eq : from;
        std::size_t last = to < line.size() && line[to] == '"' ? to + 1 : to;
        while (last < line.size() && !detail::space(line[last])) {
            ++last;
        }
        out.written = line.substr(start, last - start);
    }
    out.read = Read::Bad;
    if (out.text.empty() || !ends) {
        return out;
    }
    float v = 0.f;
    const char* first = out.text.data();
    const char* last = first + out.text.size();
    // from_chars takes no '+'; a written "+0.5" is still a number, "+-5" is not.
    if (*first == '+' && last - first > 1 && (detail::digit(first[1]) || first[1] == '.')) {
        ++first;
    }
    const auto [end, ec] = std::from_chars(first, last, v);
    if (ec == std::errc() && end == last && first != last && std::isfinite(v)) {
        out.read = Read::Number;
        out.value = v;
    }
    return out;
}

// The index in keys[0..count) of the first flag given with a value that is not a number, or hidden in quotes, or -1.
inline int firstBad(std::string_view line, const std::string_view* keys, std::size_t count) {
    for (std::size_t i = 0; keys != nullptr && i < count; ++i) {
        const Read r = read(line, keys[i]).read;
        if (r == Read::Bad || r == Read::Hidden) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Whether key is one of keys[0..count) (exact, case-insensitive): a tool that reads a number by a key checks it is one
// firstBad was given, so a numeric flag added later can't skip the strict check.
constexpr bool listed(std::string_view key, const std::string_view* keys, std::size_t count) {
    for (std::size_t i = 0; keys != nullptr && i < count; ++i) {
        if (keys[i].size() == key.size() && detail::sameAt(keys[i], 0, key)) {
            return true;
        }
    }
    return false;
}

} // namespace ftcore::flagnum
