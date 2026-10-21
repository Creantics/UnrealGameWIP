# Localisation

Interface text (menus, prompts, hints, results) comes from a table per culture. Story text isn't here. It has its own files, and translating it is a separate job for later.

## Files

- `Data/Loc/en/ui.json` is the English table and the fallback for every other culture:

  ```json
  { "version": 1, "culture": "en", "strings": { "title.new_game": "New Game", "mini.try": "Try {0} of {1}" } }
  ```

- **Keys** use lower-case letters, digits, `_` and `.`, grouped by screen (`battle.`, `title.`, ...). Strings shared by several screens use `common.`.
- **Placeholders** `{0}`..`{9}` are where the code fills in numbers. A translation must use the same ones.
- A new culture is a new folder: `Data/Loc/fr/ui.json`, `Data/Loc/pt-BR/ui.json`. A partial table is fine, because missing keys fall back to English.

## Code

- **Core:** `ftcore::UiTextTable` (`Core/include/ftcore/UiText.h`) handles import, lookup, the culture chain, the translation check and the pseudo-translation. Lookups don't allocate.
- **Unreal:** `FTUiText` (`Source/ForgottenThird/FTUiText.h`) takes the place of the old `const FString` constants:

  ```cpp
  const FTUiText kRetry(TEXT("battle.retry"));
  ```

  It converts to `const FString&` and resolves once, on first draw.
- **Formatted text** uses `FString::Format(*kTry.Get(), {Used, Total})`. Build it when the numbers change, not every frame.

## Choosing the culture

- `-UiCulture=<name>` on the command line, otherwise the system culture.
- The chain runs most specific first, then English: `pt-BR` → `pt` → `en`.
- **On load, Unreal logs `UiText: culture <name>`,** plus a warning for each missing key, extra key or placeholder mismatch in the chosen table.
- `-UiCulture=pseudo` shows every string accented, padded by about a third and bracketed (`[Ńéŵ Ĝåɱé ~~]`). Use it to find text that overflows its box, or text that's still hard-coded (it stays plain).

## Checks

- `ftcore_tests` (`test_ui_text.cpp`):
  - the English table loads cleanly
  - no string is over 160 characters
  - the pseudo-translation covers every key
- **ctest `ui_text_keys`:**
  - every key used in `Source/ForgottenThird` is in the English table
  - every key in the table is used somewhere

## Not done yet

- **Fonts:** the HUD font covers Latin text only. Cyrillic, Greek and CJK need a font with those glyphs, plus a per-culture font choice.
- **Layout:** right-to-left text and boxes that grow to fit longer strings aren't handled. Check with `-UiCulture=pseudo` first.
- **Settings:** there's no in-game language setting yet; the language is picked at launch.
- **Logs:** log and debug text stays in English and isn't in the table.
