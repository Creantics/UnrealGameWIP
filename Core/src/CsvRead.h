// Internal to Core/src: not part of the public ftcore headers.
#pragma once

// The CSV reading the build-time table readers share (MassingPlan.cpp, WorldChecks.cpp): RFC 4180 records with
// the line each starts on, trimmed fields, numbers, ids and comment rows. Allocates: build time only.

#include <charconv>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ftcore::detail {

// Sets `error` to "line <line>: <what>" and returns false.
inline bool fail(std::string* error, std::size_t line, const std::string& what) {
    if (error) *error = "line " + std::to_string(line) + ": " + what;
    return false;
}

// One CSV record: its fields and the line it starts on.
struct Record {
    std::vector<std::string> fields;
    std::size_t line = 0;
};

// Splits RFC 4180 CSV into records. A quoted field may hold commas, doubled quotes and line breaks.
// Blank lines are skipped. False on a quote left open or text after a closing quote.
inline bool splitCsv(std::string_view text, std::vector<Record>& out, std::string* error) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);
    std::size_t i = 0, line = 1;
    const std::size_t n = text.size();
    while (i < n) {
        Record rec;
        rec.line = line;
        std::string field;
        bool any = false; // the record has a field or a separator
        for (;;) {
            if (i < n && text[i] == '"') {
                any = true;
                ++i;
                for (;;) {
                    if (i >= n) return fail(error, rec.line, "a quoted field is never closed");
                    const char c = text[i++];
                    if (c == '"') {
                        if (i < n && text[i] == '"') {
                            field.push_back('"');
                            ++i;
                            continue;
                        }
                        break;
                    }
                    if (c == '\n') ++line;
                    field.push_back(c);
                }
                if (i < n && text[i] != ',' && text[i] != '\n' && text[i] != '\r')
                    return fail(error, line, "text after a closing quote");
            } else {
                while (i < n && text[i] != ',' && text[i] != '\n' && text[i] != '\r') {
                    field.push_back(text[i++]);
                    any = true;
                }
            }
            if (i < n && text[i] == ',') {
                any = true;
                rec.fields.push_back(std::move(field));
                field.clear();
                ++i;
                continue;
            }
            rec.fields.push_back(std::move(field));
            break;
        }
        if (i < n && text[i] == '\r') ++i;
        if (i < n && text[i] == '\n') {
            ++i;
            ++line;
        }
        if (any) out.push_back(std::move(rec));
    }
    return true;
}

// Strips spaces and tabs from both ends.
inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// A finite number, trimmed, with an optional leading '+'.
inline bool parseNumber(std::string_view s, double& out) {
    s = trim(s);
    if (s.empty()) return false;
    if (s.front() == '+') s.remove_prefix(1);
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && end == s.data() + s.size() && std::isfinite(out);
}

// A-Z, 0-9, '_' or '-', at least one.
inline bool validId(std::string_view id) {
    if (id.empty()) return false;
    for (const char c : id)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

// A record whose first field, trimmed, starts with '#'.
inline bool isComment(const Record& r) {
    const std::string_view first = trim(r.fields.front());
    return !first.empty() && first.front() == '#';
}

} // namespace ftcore::detail
