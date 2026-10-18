#pragma once

// Loads the story script JSON (newengine docs/story_format.md: story.json plus one scenes/<id>.json
// per scene) into a StoryDatabase through StoryBuilder. Load time only: everything allocates.
//
//   std::vector<std::string> ids;
//   ftcore::storySceneIds(storyText, ids);          // which scene files to read
//   ... read scenes/<id>.json for each id ...
//   ftcore::StoryDatabase db;
//   std::string error;
//   std::vector<std::string> warnings;
//   if (!ftcore::importStory(storyText, sceneTexts, db, &error, &warnings)) ...;
//
// The owner reads the files. Scene documents may come in any order: they are matched by their
// `id` and fed to the builder in chapter order. The rules are the newengine loader's: a scene
// listed twice, a listed scene without a document, bad JSON, a missing `chapters` / `beats` array,
// a flag op without an id, a bad flag value or a bad option `next` are errors; an unknown format
// version, a document no chapter lists, an unknown beat type (kept as a no-op) and an unknown cue
// action (played) are warnings, as are the builder's own.

#include "ftcore/Api.h"
#include "ftcore/StoryDatabase.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

// The scene ids story.json lists, in chapter order. False (and `*error`) on bad JSON.
FTCORE_API bool storySceneIds(std::string_view storyJson, std::vector<std::string>& out,
                              std::string* error = nullptr);

// False on errors: `out` is then empty and `*error` lists every error found, one per line.
// `keepEmphasis` keeps markdown `*` emphasis in display text (see StoryBuilder::keepEmphasis).
FTCORE_API bool importStory(std::string_view storyJson, std::span<const std::string_view> sceneJsons,
                            StoryDatabase& out, std::string* error = nullptr,
                            std::vector<std::string>* warnings = nullptr, bool keepEmphasis = false);

} // namespace ftcore
