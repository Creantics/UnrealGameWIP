#include "ftcore/NpcPlaces.h"

#include <array>
#include <cmath>
#include <string>
#include <utility>

namespace ftcore {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

void splitComma(std::string_view line, std::vector<std::string_view>& out) {
    out.clear();
    std::size_t start = 0;
    for (;;) {
        const std::size_t c = line.find(',', start);
        out.push_back(trim(line.substr(start, c == std::string_view::npos ? std::string_view::npos : c - start)));
        if (c == std::string_view::npos) return;
        start = c + 1;
    }
}

// "HH:MM", 00:00..24:00, as minutes of the day.
bool toMinute(std::string_view s, int& out) {
    if (s.size() != 5 || s[2] != ':') return false;
    for (int i : {0, 1, 3, 4})
        if (s[static_cast<std::size_t>(i)] < '0' || s[static_cast<std::size_t>(i)] > '9') return false;
    const int h = (s[0] - '0') * 10 + (s[1] - '0');
    const int m = (s[3] - '0') * 10 + (s[4] - '0');
    if (m > 59 || h > 24 || (h == 24 && m != 0)) return false;
    out = h * 60 + m;
    return true;
}

bool covers(const NpcPlaceSpan& s, int m) { return s.from < s.to ? (m >= s.from && m < s.to) : (m >= s.from || m < s.to); }

bool fail(std::string* error, std::size_t row, const char* why) {
    if (error) *error = "npc_places.csv:" + std::to_string(row) + ": " + why;
    return false;
}

// A span as one or two half-open minute ranges.
int ranges(const NpcPlaceSpan& s, int (&r)[2][2]) {
    if (s.from < s.to) {
        r[0][0] = s.from, r[0][1] = s.to;
        return 1;
    }
    r[0][0] = s.from, r[0][1] = kNpcDayMinutes;
    r[1][0] = 0, r[1][1] = s.to;
    return 2;
}

bool overlap(const NpcPlaceSpan& a, const NpcPlaceSpan& b) {
    int ra[2][2]{}, rb[2][2]{};
    const int na = ranges(a, ra), nb = ranges(b, rb);
    for (int i = 0; i < na; ++i)
        for (int j = 0; j < nb; ++j)
            if (ra[i][0] < rb[j][1] && rb[j][0] < ra[i][1]) return true;
    return false;
}

// Both rows can hold at once: no fact is given two different values between them.
bool compatible(const NpcPlaceSpan& a, const NpcPlaceSpan& b) {
    for (std::uint8_t i = 0; i < a.condCount; ++i)
        for (std::uint8_t j = 0; j < b.condCount; ++j)
            if (a.cond[i].fact == b.cond[j].fact && a.cond[i].value != b.cond[j].value) return false;
    return true;
}

// `fact:<id>=<value>;...` into the span. False with the reason.
bool readCond(std::string_view text, const NpcFactSource* facts, NpcPlaceSpan& s, const char*& why) {
    while (!text.empty()) {
        const std::size_t semi = text.find(';');
        const std::string_view clause = trim(text.substr(0, semi));
        text.remove_prefix(semi == std::string_view::npos ? text.size() : semi + 1);
        if (clause.empty()) continue;
        why = "cond clauses must be fact:<id>=<value>, value A..D or 0..254";
        if (clause.substr(0, 5) != "fact:") return false;
        const std::string_view rest = clause.substr(5);
        const std::size_t eq = rest.find('=');
        if (eq == std::string_view::npos || eq == 0) return false;
        const std::string_view val = rest.substr(eq + 1);
        int v = 0;
        if (val.size() == 1 && val[0] >= 'A' && val[0] <= 'D') {
            v = val[0] - 'A' + 1;
        } else {
            if (val.empty() || val.size() > 3) return false;
            for (char c : val) {
                if (c < '0' || c > '9') return false;
                v = v * 10 + (c - '0');
            }
            if (v > 254) return false;
        }
        why = "a cond clause needs the story fact source";
        if (!facts) return false;
        const int f = facts->npcFactIndex(rest.substr(0, eq));
        why = "unknown fact in cond";
        if (f < 0 || f > 0x7FFF) return false;
        why = "more than four clauses in cond";
        if (s.condCount >= kNpcPlaceCondMax) return false;
        why = "a fact twice in one cond";
        for (std::uint8_t i = 0; i < s.condCount; ++i)
            if (s.cond[i].fact == f) return false;
        s.cond[s.condCount].fact = static_cast<std::int16_t>(f);
        s.cond[s.condCount].value = static_cast<std::uint8_t>(v);
        ++s.condCount;
    }
    return true;
}

bool holds(const NpcPlaceSpan& s, const NpcFactSource* facts) {
    for (std::uint8_t i = 0; i < s.condCount; ++i) {
        std::uint8_t v = 0;
        if (!facts || !facts->npcFactValue(s.cond[i].fact, v) || v != s.cond[i].value) return false;
    }
    return true;
}

}  // namespace

bool importNpcPlaces(std::string_view csv, const NpcTables& t, NpcPlaceTable& out, std::string* error) {
    return importNpcPlaces(csv, t, nullptr, out, error);
}

bool importNpcPlaces(std::string_view csv, const NpcTables& t, const NpcFactSource* facts, NpcPlaceTable& out,
                     std::string* error) {
    NpcPlaceTable p;
    std::vector<std::string_view> cells;
    std::vector<std::size_t> spanRow;
    int cNpc = -1, cFrom = -1, cTo = -1, cPlace = -1, cCond = -1;
    std::size_t row = 0;
    bool header = true;
    if (csv.size() >= 3 && csv.substr(0, 3) == "\xEF\xBB\xBF") csv.remove_prefix(3);  // a UTF-8 BOM
    while (!csv.empty()) {
        const std::size_t nl = csv.find('\n');
        const std::string_view line = trim(csv.substr(0, nl));
        csv.remove_prefix(nl == std::string_view::npos ? csv.size() : nl + 1);
        ++row;
        if (line.empty()) continue;
        splitComma(line, cells);
        if (header) {
            header = false;
            for (std::size_t i = 0; i < cells.size(); ++i) {
                const int c = static_cast<int>(i);
                if (cells[i] == "npc_id") cNpc = c;
                else if (cells[i] == "from") cFrom = c;
                else if (cells[i] == "to") cTo = c;
                else if (cells[i] == "place_id") cPlace = c;
                else if (cells[i] == "cond") cCond = c;
            }
            if (cNpc < 0 || cFrom < 0 || cTo < 0 || cPlace < 0) return fail(error, row, "needs npc_id,from,to,place_id");
            continue;
        }
        const auto cell = [&](int c) {
            return c >= 0 && static_cast<std::size_t>(c) < cells.size() ? cells[static_cast<std::size_t>(c)] : std::string_view{};
        };
        const int npc = t.npcIndex(cell(cNpc));
        if (npc < 0) return fail(error, row, "unknown npc_id");
        int from = 0, to = 0;
        if (!toMinute(cell(cFrom), from) || !toMinute(cell(cTo), to)) return fail(error, row, "from and to must be HH:MM, 00:00..24:00");
        if (from == to) return fail(error, row, "empty span (from equals to)");
        if (from == kNpcDayMinutes) return fail(error, row, "from must be before 24:00 (write 00:00)");
        if (to == 0) to = kNpcDayMinutes;  // 00:00 ends at midnight
        const std::string_view place = cell(cPlace);
        if (place.empty()) return fail(error, row, "empty place_id");
        NpcPlaceSpan s;
        s.npc = static_cast<std::uint16_t>(npc);
        s.from = static_cast<std::uint16_t>(from);
        s.to = static_cast<std::uint16_t>(to);
        const char* why = "";
        if (!readCond(cell(cCond), facts, s, why)) return fail(error, row, why);
        if (place == "-") {
            s.place = kNpcOffMap;
        } else {
            std::size_t pi = 0;
            while (pi < p.places.size() && p.places[pi] != place) ++pi;
            if (pi == p.places.size()) p.places.emplace_back(place);
            s.place = static_cast<std::uint16_t>(pi);
        }
        for (const NpcPlaceSpan& o : p.spans) {
            if (o.npc == s.npc && o.condCount == s.condCount && overlap(o, s) && compatible(o, s)) {
                const std::size_t other = spanRow[static_cast<std::size_t>(&o - p.spans.data())];
                if (error) *error = "npc_places.csv:" + std::to_string(row) + ": can win the same minute as row " + std::to_string(other) +
                                    " (overlapping times, as many clauses, no fact that tells them apart)";
                return false;
            }
        }
        p.spans.push_back(s);
        spanRow.push_back(row);
    }
    if (header) return fail(error, 0, "no header");
    out = std::move(p);
    return true;
}

int npcPlaceAt(const NpcPlaceTable& p, std::uint16_t npc, double hours, const NpcFactSource* facts) {
    double h = std::fmod(hours, 24.0);
    if (!(h >= 0.0)) h = h < 0.0 ? h + 24.0 : 0.0;  // negative wraps; NaN reads as midnight
    // A hair over the minute, so 8.55 h (513 minutes in decimal, 512.999... in binary) reads 08:33, not 08:32.
    int m = static_cast<int>(std::floor(h * 60.0 + 1e-6));
    if (m >= kNpcDayMinutes) m = kNpcDayMinutes - 1;
    const NpcPlaceSpan* best = nullptr;
    for (const NpcPlaceSpan& s : p.spans)
        if (s.npc == npc && covers(s, m) && (!best || s.condCount > best->condCount) && holds(s, facts)) best = &s;
    return !best || best->place == kNpcOffMap ? -1 : static_cast<int>(best->place);
}

}  // namespace ftcore
