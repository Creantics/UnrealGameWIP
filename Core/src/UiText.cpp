#include "ftcore/UiText.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <bitset>
#include <cstdint>

namespace ftcore {

namespace {

bool validKey(std::string_view k) {
    if (k.empty()) return false;
    for (const char c : k)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) return false;
    return true;
}

// Which of {0}..{9} the text uses.
std::bitset<10> placeholders(std::string_view s) {
    std::bitset<10> used;
    for (std::size_t i = 0; i + 2 < s.size(); ++i)
        if (s[i] == '{' && s[i + 1] >= '0' && s[i + 1] <= '9' && s[i + 2] == '}') used.set(static_cast<std::size_t>(s[i + 1] - '0'));
    return used;
}

// UTF-8 for an accented form of each ASCII letter; null for anything else.
const char* accented(char c) {
    static const char* const lower[26] = {"\xC3\xA5", "\xC6\x80", "\xC3\xA7", "\xC3\xB0", "\xC3\xA9", "\xC6\x92", "\xC4\x9D",
                                          "\xC4\xA5", "\xC3\xAE", "\xC4\xB5", "\xC4\xB7", "\xC4\xBC", "\xC9\xB1", "\xC3\xB1",
                                          "\xC3\xB6", "\xC3\xBE", "\xC7\xAB", "\xC5\x95", "\xC5\xA1", "\xC5\xA3", "\xC3\xBB",
                                          "\xE1\xB9\xBD", "\xC5\xB5", "\xE1\xBA\x8B", "\xC3\xBD", "\xC5\xBE"};
    static const char* const upper[26] = {"\xC3\x85", "\xC6\x81", "\xC3\x87", "\xC3\x90", "\xC3\x89", "\xC6\x91", "\xC4\x9C",
                                          "\xC4\xA4", "\xC3\x8E", "\xC4\xB4", "\xC4\xB6", "\xC4\xBB", "\xE1\xB9\x80", "\xC3\x91",
                                          "\xC3\x96", "\xC3\x9E", "\xC7\xAA", "\xC5\x94", "\xC5\xA0", "\xC5\xA2", "\xC3\x9B",
                                          "\xE1\xB9\xBC", "\xC5\xB4", "\xE1\xBA\x8A", "\xC3\x9D", "\xC5\xBD"};
    if (c >= 'a' && c <= 'z') return lower[c - 'a'];
    if (c >= 'A' && c <= 'Z') return upper[c - 'A'];
    return nullptr;
}

} // namespace

const std::string* UiTextTable::find(std::string_view key) const {
    const auto it = std::lower_bound(entries.begin(), entries.end(), key,
                                     [](const UiTextEntry& e, std::string_view k) { return std::string_view(e.key) < k; });
    return it != entries.end() && it->key == key ? &it->text : nullptr;
}

bool importUiText(std::string_view json, UiTextTable& out, std::string* error, std::vector<std::string>* warnings) {
    std::vector<std::string> errors;
    JsonDocument doc;
    std::string parseError;
    UiTextTable table;
    if (!doc.parse(json, &parseError)) {
        errors.push_back(parseError);
    } else if (!doc.root().isObject()) {
        errors.push_back("expected an object");
    } else {
        const JsonValue root = doc.root();
        if (!root["version"].isNumber() || root["version"].number() != 1.0) errors.push_back("version: expected 1");
        const JsonValue culture = root["culture"];
        if (!culture.isString() || culture.string().empty()) errors.push_back("culture: expected a name");
        else table.culture = std::string(culture.string());
        const JsonValue strings = root["strings"];
        if (!strings.isObject()) {
            errors.push_back("strings: expected an object");
        } else {
            for (std::uint32_t i = 0; i < strings.size(); ++i) {
                const std::string key(strings.key(i));
                const JsonValue v = strings.value(i);
                if (!validKey(key)) errors.push_back("strings: bad key \"" + key + "\"");
                else if (!v.isString()) errors.push_back("strings." + key + ": expected text");
                else table.entries.push_back({key, std::string(v.string())});
            }
            std::sort(table.entries.begin(), table.entries.end(),
                      [](const UiTextEntry& a, const UiTextEntry& b) { return a.key < b.key; });
            for (std::size_t i = 1; i < table.entries.size(); ++i)
                if (table.entries[i].key == table.entries[i - 1].key) errors.push_back("strings: duplicate key " + table.entries[i].key);
        }
        if (warnings)
            for (std::uint32_t i = 0; i < root.size(); ++i) {
                const std::string_view k = root.key(i);
                if (k != "version" && k != "culture" && k != "strings") warnings->push_back("unknown key: " + std::string(k));
            }
    }
    if (errors.empty()) {
        out = std::move(table);
        return true;
    }
    if (error) {
        error->clear();
        for (const std::string& e : errors) *error += e + "\n";
    }
    return false;
}

std::string_view lookupUiText(const UiTextTable* primary, const UiTextTable& fallback, std::string_view key) {
    if (primary)
        if (const std::string* s = primary->find(key)) return *s;
    if (const std::string* s = fallback.find(key)) return *s;
    return key;
}

std::vector<std::string> uiCultureChain(std::string_view culture) {
    std::string c(culture);
    std::replace(c.begin(), c.end(), '_', '-');
    std::vector<std::string> chain;
    if (!c.empty()) chain.push_back(c);
    const std::size_t dash = c.find('-');
    if (dash != std::string::npos && dash > 0) chain.push_back(c.substr(0, dash));
    if (std::find(chain.begin(), chain.end(), "en") == chain.end()) chain.push_back("en");
    return chain;
}

std::vector<std::string> checkUiTranslation(const UiTextTable& english, const UiTextTable& other) {
    std::vector<std::string> issues;
    for (const UiTextEntry& e : english.entries) {
        const std::string* t = other.find(e.key);
        if (!t) issues.push_back(other.culture + ": missing " + e.key);
        else if (placeholders(*t) != placeholders(e.text)) issues.push_back(other.culture + ": " + e.key + " has different {N} placeholders");
    }
    for (const UiTextEntry& e : other.entries)
        if (!english.find(e.key)) issues.push_back(other.culture + ": " + e.key + " isn't in English");
    return issues;
}

UiTextTable pseudoLocalise(const UiTextTable& english) {
    UiTextTable out;
    out.culture = "pseudo";
    out.entries.reserve(english.entries.size());
    for (const UiTextEntry& e : english.entries) {
        std::string t = "[";
        std::size_t letters = 0;
        for (std::size_t i = 0; i < e.text.size(); ++i) {
            const char c = e.text[i];
            if (c == '{' && i + 2 < e.text.size() && e.text[i + 2] == '}') {
                t.append(e.text, i, 3);
                i += 2;
                continue;
            }
            if (const char* a = accented(c)) {
                t += a;
                ++letters;
            } else {
                t += c;
            }
        }
        t += ' ';
        t.append(std::max<std::size_t>(1, letters / 3), '~');
        t += ']';
        out.entries.push_back({e.key, std::move(t)});
    }
    return out;
}

} // namespace ftcore
