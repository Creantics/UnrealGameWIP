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
#include "ftcore/Progression.h"
#include "ftcore/ScarePacer.h"
#include "ftcore/StoryDatabase.h"
#include "ftcore/WorldClock.h"

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

// The beats of one scene the player has seen, as lowercase hex: beat i is bit (i % 4) of hex digit
// i / 4 (a digit is 0-9 a-f; digits past the end are zero, so trailing zeros are left off).
struct SavedSeenScene {
    std::string scene; // scene id
    std::string bits;
};

struct StorySave {
    std::string scene; // scene id
    std::uint32_t beat = 0;
    std::vector<SavedStoryFlag> flags; // only flags that differ from their default
    std::vector<SavedSeenScene> seen;  // only scenes with a seen beat (format 2; empty for a format 1 save)
    PartyProgress party;               // xp, gold and items; the owner fills it (capture and apply leave it alone)
    WorldClock clock;                  // time of day; the owner fills it (capture and apply leave it alone)
    bool hasClock = false;             // the save holds a clock (false for a save from before the clock)
    ScareSave scares;                  // the scare pacer (play clock, ring, last kind, seen moments); the owner fills it
    bool hasScares = false;            // the save holds the pacer (false for a save from before it)
    // Decoding only: the save's "scares" was malformed or of an unknown version, so it was dropped (hasScares false;
    // the owner starts a fresh pacer, which starts quiet) and the rest of the save loaded. scaresProblem says why,
    // for the owner's warning log.
    bool scaresDropped = false;
    std::string scaresProblem;
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
// {"format":2,"scene":"SC-A","beat":4,"flags":[{"id":"F","kind":"int","number":3,"text":""}],
//  "seen":[{"scene":"SC-A","bits":"1f"}],"party":{"xp":240,"gold":130,
//  "items":[{"id":"bandage_roll","count":2}]}}
// kind is "bool", "int" or "string". Every field is required except "party" (a save from before
// progression has none: xp and gold read as 0) and "seen" in a format 1 save (it then reads as
// empty); unknown extra fields are ignored. Encoding always writes format 2.
// Inside "party", "items" is optional too (a save from before items has none: the inventory reads
// as empty); encoding always writes it. Each entry needs a valid item id (validItemId) and a count
// in 1..kMaxItemStack, or the save is refused. The id is not looked up in the item table (the table
// may change between versions), so an id the game no longer knows loads as it is. The entries go
// into the inventory in order; whatever does not fit (more than kMaxInventoryStacks different ids,
// a stack past kMaxItemStack) is dropped.
// "scares" is optional (hasScares): {"version":1,"playSeconds":5400.5,"ring":[{"t":1200,"kind":"scene"}],"head":1,
// "lastKind":"scene","seen":"02"}. ring lists the slots 0..count-1 in slot order; lastKind is "none" before any
// scare; a missing version reads as 1. A malformed "scares" (ScarePacer::load's checks) or one of another version
// does not refuse the save: it is dropped (scaresDropped, scaresProblem) and everything else loads.

// UTF-8 JSON text for the save.
FTCORE_API std::string encodeStorySave(const StorySave& save);
// False on bad JSON, an unknown format version, or a missing / wrong-typed field; `*error` then
// says why and `out` is left untouched.
FTCORE_API bool decodeStorySave(std::string_view text, StorySave& out, std::string* error = nullptr);

// ---- checking a save before loading it ----

// Why a save text can't be used. The name (saveRefusalName) is what gets logged.
enum class SaveRefusal : std::uint8_t {
    None,           // usable
    Empty,          // no text: nothing was saved
    Invalid,        // bad JSON, unknown format version, or a missing / wrong-typed field
    NoStory,        // no story database is loaded
    SceneMissing,   // the saved scene is not in the loaded story
    BeatOutOfRange, // the saved beat is past the end of its scene
};

// A short stable code for logs ("ok", "empty", "invalid", "no_story", "scene_missing",
// "beat_out_of_range"). Never script text.
FTCORE_API const char* saveRefusalName(SaveRefusal reason);
// Decodes `text` and checks it against `db` without touching any runner. `*out` (when given) gets
// the decoded save only when the result is None.
FTCORE_API SaveRefusal checkStorySave(std::string_view text, const StoryDatabase* db, StorySave* out = nullptr);

} // namespace ftcore
