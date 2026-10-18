#pragma once

// Story progress as a save keeps it: the cursor (scene id, beat index) and every flag that differs
// from its default, each as plain data a save format can write (a boolean, an integer or text).
// Serialising it (JSON, an Unreal SaveGame object) is the owner's job.
//
//   ftcore::StorySave s;
//   if (ftcore::captureStory(runner, s)) write(s);            // false: not inside the story
//   ...
//   ftcore::StoryLoadReport report;
//   if (!ftcore::applyStory(s, runner, &report)) ...;          // the scene no longer exists
//
// Ids are stored rather than indices so a save survives re-imports that reorder scenes or flags.
// capture and apply allocate (they copy text): call them on save and load, not per frame.

#include "ftcore/Api.h"
#include "ftcore/StoryDatabase.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

class StoryRunner;

struct SavedStoryFlag {
    std::string id;
    FlagValue::Kind kind = FlagValue::Kind::Bool;
    std::int64_t number = 0; // Bool: 0 / 1; Int: the value
    std::string text;        // String: the value's text
};

struct StorySave {
    std::string scene; // scene id
    std::uint32_t beat = 0;
    std::vector<SavedStoryFlag> flags; // only flags that differ from their default
};

struct StoryLoadReport {
    // Saved flags the script no longer has, or string values it no longer uses: skipped (the
    // flag keeps its default).
    std::vector<std::string> skippedFlags;
};

// False (and `out` cleared) when the runner has no database or is not on a scene.
FTCORE_API bool captureStory(const StoryRunner& runner, StorySave& out);
// Resets the runner's flags to their defaults, applies the saved ones and restores the cursor
// (StoryRunner::restore: the saved beat is entered again). False when the saved scene does not
// exist; the runner is then stopped.
FTCORE_API bool applyStory(const StorySave& save, StoryRunner& runner, StoryLoadReport* report = nullptr);

// ---- JSON text ----
// {"format":1,"scene":"SC-A","beat":4,"flags":[{"id":"F","kind":"int","number":3,"text":""}]}
// kind is "bool", "int" or "string". Every field is required; unknown extra fields are ignored.

// UTF-8 JSON text for the save.
FTCORE_API std::string encodeStorySave(const StorySave& save);
// False on bad JSON, an unknown format version, or a missing / wrong-typed field; `*error` then
// says why and `out` is left untouched.
FTCORE_API bool decodeStorySave(std::string_view text, StorySave& out, std::string* error = nullptr);

} // namespace ftcore
