#pragma once

// Interface text by key, per culture: Data/Loc/<culture>/ui.json. Story text is not here (it has
// its own files); this is menus, prompts, hints and results.
//
//   ftcore::UiTextTable en, fr;
//   std::string error;
//   ftcore::importUiText(enJson, en, &error);
//   std::string_view s = ftcore::lookupUiText(&fr, en, "title.new_game"); // fr, else en, else the key
//
// ui.json:
//   { "version": 1, "culture": "en", "strings": { "title.new_game": "New Game", "mini.try": "Try {0} of {1}" } }
//   Keys: lower-case letters, digits, '_' and '.'. Values: text, with {0}..{9} where the code fills
//   in numbers. A translation must use the same placeholders as English (checkUiTranslation).
//
// Lookups don't allocate (a binary search over the sorted keys).

#include "ftcore/Api.h"

#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

struct UiTextEntry {
    std::string key;
    std::string text;
};

struct UiTextTable {
    std::string culture;
    std::vector<UiTextEntry> entries; // sorted by key

    // The text for a key, or null.
    FTCORE_API const std::string* find(std::string_view key) const;
};

FTCORE_API bool importUiText(std::string_view json, UiTextTable& out, std::string* error = nullptr,
                             std::vector<std::string>* warnings = nullptr);

// The key's text in `primary` (may be null), else in `fallback`, else the key itself, so a missing
// string shows up on screen instead of a blank.
FTCORE_API std::string_view lookupUiText(const UiTextTable* primary, const UiTextTable& fallback, std::string_view key);

// The cultures to try for a requested one, most specific first, ending with "en":
// "pt-BR" -> "pt-BR", "pt", "en". Underscores count as hyphens.
FTCORE_API std::vector<std::string> uiCultureChain(std::string_view culture);

// Problems with a translation against English: missing keys, keys English doesn't have, and
// texts whose {N} placeholders differ. Empty when it's complete.
FTCORE_API std::vector<std::string> checkUiTranslation(const UiTextTable& english, const UiTextTable& other);

// A pseudo-translation of English for testing layout and coverage: every ASCII letter accented,
// the text padded by about a third and bracketed, placeholders untouched.
FTCORE_API UiTextTable pseudoLocalise(const UiTextTable& english);

} // namespace ftcore
