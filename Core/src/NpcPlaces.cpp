#include "ftcore/NpcPlaces.h"

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

}  // namespace

bool importNpcPlaces(std::string_view csv, const NpcTables& t, NpcPlaceTable& out, std::string* error) {
    NpcPlaceTable p;
    std::vector<std::string_view> cells;
    std::vector<std::vector<std::uint8_t>> used(t.npcs.size());
    int cNpc = -1, cFrom = -1, cTo = -1, cPlace = -1;
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
            }
            if (cNpc < 0 || cFrom < 0 || cTo < 0 || cPlace < 0) return fail(error, row, "needs npc_id,from,to,place_id");
            continue;
        }
        const auto cell = [&](int c) { return static_cast<std::size_t>(c) < cells.size() ? cells[static_cast<std::size_t>(c)] : std::string_view{}; };
        const int npc = t.npcIndex(cell(cNpc));
        if (npc < 0) return fail(error, row, "unknown npc_id");
        int from = 0, to = 0;
        if (!toMinute(cell(cFrom), from) || !toMinute(cell(cTo), to)) return fail(error, row, "from and to must be HH:MM, 00:00..24:00");
        if (from == to) return fail(error, row, "empty span (from equals to)");
        if (from == kNpcDayMinutes) return fail(error, row, "from must be before 24:00 (write 00:00)");
        if (to == 0) to = kNpcDayMinutes;      // 00:00 ends at midnight
        const std::string_view place = cell(cPlace);
        if (place.empty()) return fail(error, row, "empty place_id");
        NpcPlaceSpan s;
        s.npc = static_cast<std::uint16_t>(npc);
        s.from = static_cast<std::uint16_t>(from);
        s.to = static_cast<std::uint16_t>(to);
        std::size_t pi = 0;
        while (pi < p.places.size() && p.places[pi] != place) ++pi;
        if (pi == p.places.size()) p.places.emplace_back(place);
        s.place = static_cast<std::uint16_t>(pi);
        std::vector<std::uint8_t>& u = used[static_cast<std::size_t>(npc)];
        if (u.empty()) u.assign(kNpcDayMinutes, 0);
        for (int m = 0; m < kNpcDayMinutes; ++m) {
            if (!covers(s, m)) continue;
            if (u[static_cast<std::size_t>(m)]) return fail(error, row, "overlaps another span of this npc");
            u[static_cast<std::size_t>(m)] = 1;
        }
        p.spans.push_back(s);
    }
    if (header) return fail(error, 0, "no header");
    out = std::move(p);
    return true;
}

int npcPlaceAt(const NpcPlaceTable& p, std::uint16_t npc, double hours) {
    double h = std::fmod(hours, 24.0);
    if (!(h >= 0.0)) h = h < 0.0 ? h + 24.0 : 0.0;  // negative wraps; NaN reads as midnight
    // A hair over the minute, so 8.55 h (513 minutes in decimal, 512.999... in binary) reads 08:33, not 08:32.
    int m = static_cast<int>(std::floor(h * 60.0 + 1e-6));
    if (m >= kNpcDayMinutes) m = kNpcDayMinutes - 1;
    for (const NpcPlaceSpan& s : p.spans)
        if (s.npc == npc && covers(s, m)) return s.place;
    return -1;
}

}  // namespace ftcore
