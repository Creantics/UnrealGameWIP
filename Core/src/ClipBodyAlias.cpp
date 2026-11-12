#include "ftcore/ClipBodyAlias.h"

#include "ftcore/Json.h"

#include <utility>

namespace ftcore {

namespace {

char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
    return true;
}

// Letters, digits and '_'; 1..128 characters.
bool assetName(std::string_view s) {
    if (s.empty() || s.size() > 128) return false;
    for (const char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

const ClipBodyAliases::Entry* findEntry(const std::vector<ClipBodyAliases::Entry>& entries, std::string_view body) {
    for (const ClipBodyAliases::Entry& e : entries)
        if (equalsNoCase(e.body, body)) return &e;
    return nullptr;
}

} // namespace

std::string_view ClipBodyAliases::resolve(std::string_view body) const {
    std::string_view cur = body;
    for (int step = 0; step < kMaxChain; ++step) {
        const Entry* e = findEntry(entries, cur);
        if (!e) break;
        cur = e->clips;
    }
    return cur;
}

bool importClipBodyAliases(std::string_view json, ClipBodyAliases& out, std::string* error,
                           std::vector<std::string>* warnings) {
    out.entries.clear();
    std::string problems;
    auto fail = [&](const std::string& line) { problems += line + "\n"; };
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(json, &parseError)) {
        if (error) *error = "json: " + parseError;
        return false;
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        if (error) *error = "(root): not an object";
        return false;
    }
    if (!root["version"].isInteger() || root["version"].integer() != 1) fail("version: must be 1");
    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view k = root.key(i);
        if (k != "version" && k != "aliases" && k != "about" && warnings)
            warnings->push_back("unknown key " + std::string(k) + " (ignored)");
    }
    const JsonValue aliases = root["aliases"];
    if (!aliases.isObject()) fail("aliases: must be an object");
    std::vector<ClipBodyAliases::Entry> entries;
    for (std::uint32_t i = 0; i < aliases.size(); ++i) {
        const std::string key(aliases.key(i));
        const JsonValue v = aliases.value(i);
        const std::string path = "aliases." + key;
        if (!assetName(key)) {
            fail(path + ": the body name must be 1..128 letters, digits and '_'");
            continue;
        }
        if (!v.isString() || !assetName(v.string())) {
            fail(path + ": the value must be a body name (1..128 letters, digits and '_')");
            continue;
        }
        if (findEntry(entries, key)) {
            fail(path + ": listed twice");
            continue;
        }
        if (equalsNoCase(key, v.string())) {
            fail(path + ": aliased to itself");
            continue;
        }
        entries.push_back({key, std::string(v.string())});
    }
    for (const ClipBodyAliases::Entry& e : entries) {
        std::string_view cur = e.body;
        int steps = 0;
        while (const ClipBodyAliases::Entry* next = findEntry(entries, cur)) {
            cur = next->clips;
            if (++steps > ClipBodyAliases::kMaxChain || equalsNoCase(cur, e.body)) {
                fail("aliases." + e.body + (equalsNoCase(cur, e.body) ? ": the chain loops back to itself"
                                                                        : ": the chain is longer than 8"));
                break;
            }
        }
    }
    if (!problems.empty()) {
        if (error) *error = problems;
        return false;
    }
    out.entries = std::move(entries);
    return true;
}

} // namespace ftcore
