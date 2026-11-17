#include "ftcore/DemoBoot.h"

#include "ftcore/Json.h"

#include <cctype>

namespace ftcore {
namespace {

bool equalNoCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool fail(DemoBootTable& out, std::string* error, const std::string& what)
{
    out.rows.clear();
    if (error) {
        *error = what;
    }
    return false;
}

// Every token is a switch: '-' then at least one character.
bool switchesValid(std::string_view s, std::string& bad)
{
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') {
            ++i;
        }
        if (i >= s.size()) {
            break;
        }
        std::size_t j = i;
        bool quoted = false;
        while (j < s.size() && (quoted || s[j] != ' ')) {
            if (s[j] == '"') {
                quoted = !quoted;
            }
            ++j;
        }
        const std::string_view token = s.substr(i, j - i);
        if (token.size() < 2 || token[0] != '-') {
            bad = std::string(token);
            return false;
        }
        i = j;
    }
    return true;
}

} // namespace

bool parseDemoBoot(std::string_view text, DemoBootTable& out, std::string* error)
{
    out.rows.clear();
    JsonDocument doc;
    std::string jsonError;
    if (!doc.parse(text, &jsonError)) {
        return fail(out, error, "demo_boot: " + jsonError);
    }
    const JsonValue maps = doc.root()["maps"];
    if (!maps.isArray()) {
        return fail(out, error, "demo_boot: no \"maps\" array");
    }
    for (std::uint32_t i = 0; i < maps.size(); ++i) {
        const JsonValue m = maps[i];
        DemoBootRow row;
        row.map = std::string(demoBootMapName(m["map"].string()));
        row.switches = std::string(m["switches"].string());
        row.status = std::string(m["status"].string());
        if (row.map.empty() || !m["switches"].isString()) {
            return fail(out, error, "demo_boot: row " + std::to_string(i) + " needs \"map\" and \"switches\"");
        }
        std::string bad;
        if (!switchesValid(row.switches, bad)) {
            return fail(out, error, "demo_boot: " + row.map + ": \"" + bad + "\" isn't a -switch");
        }
        if (demoBootFind(out, row.map)) {
            return fail(out, error, "demo_boot: " + row.map + " named twice");
        }
        out.rows.push_back(std::move(row));
    }
    return true;
}

std::string_view demoBootMapName(std::string_view name)
{
    const std::size_t slash = name.find_last_of('/');
    if (slash != std::string_view::npos) {
        name.remove_prefix(slash + 1);
    }
    const std::size_t dot = name.find('.');
    if (dot != std::string_view::npos) {
        name = name.substr(0, dot);
    }
    // UEDPIE_<n>_<map>
    constexpr std::string_view pie = "UEDPIE_";
    if (name.size() > pie.size() && equalNoCase(name.substr(0, pie.size()), pie)) {
        const std::size_t under = name.find('_', pie.size());
        if (under != std::string_view::npos) {
            name.remove_prefix(under + 1);
        }
    }
    return name;
}

const DemoBootRow* demoBootFind(const DemoBootTable& table, std::string_view map)
{
    const std::string_view shortName = demoBootMapName(map);
    for (const DemoBootRow& row : table.rows) {
        if (equalNoCase(row.map, shortName)) {
            return &row;
        }
    }
    return nullptr;
}

std::string demoBootSwitches(std::string_view commandLine, const DemoBootRow* row)
{
    std::string out(commandLine);
    if (row && !row->switches.empty()) {
        out += ' ';
        out += row->switches;
    }
    return out;
}

} // namespace ftcore
