#pragma once

// What the game expects to be delivered, by id, and where each delivery stands. FTValidateAssets
// walks three lists:
//
//   music and sound  Data/Audio/cues.json (ftcore::AudioCueTable)
//   voice            every line id in the local story (voiceLineIds); nothing is committed for it
//   models           Data/Assets/models.json (ftcore::ModelTable, this file)
//
// models.json carries ids, kinds and rule packs only: no names, no descriptions.
//
//   { "version": 1, "models": [
//       { "id": "MDL-001", "kind": "character", "role": "lead", "pack": "Solhaven" },
//       { "id": "MDL-050", "kind": "prop", "pack": "Lattice" } ] }
//   Keys: id (required, unique), kind ("character" or "prop"; required), role ("lead",
//   "supporting"; characters only, default supporting), pack (an AssetPack name; default none),
//   file (default "<id>.glb" under Data/Models). Unknown keys are warnings.
//
// Each expected asset is in one AssetState: Missing (no file yet: the normal state until it is
// delivered), Failed (the file breaks a rule), Delivered (the file passes but isn't imported) or
// Imported (it passes and its asset exists under /Game).

#include "ftcore/Api.h"
#include "ftcore/Glb.h"
#include "ftcore/StoryDatabase.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

enum class ModelKind : std::uint8_t { Character, Prop };

struct ModelEntry {
    std::string id;
    std::string file; // relative to Data/Models
    ModelKind kind = ModelKind::Prop;
    AssetRole role = AssetRole::Prop;
    AssetPack pack = AssetPack::None;
};

struct ModelTable {
    std::vector<ModelEntry> models;
    // The model with that id, or nullptr. No allocation.
    FTCORE_API const ModelEntry* find(std::string_view id) const;
};

// On failure `out` is empty and *error lists each problem with its path, one per line.
FTCORE_API bool importModelManifest(std::string_view json, ModelTable& out, std::string* error = nullptr,
                                    std::vector<std::string>* warnings = nullptr);

// The glb rules the model is held to (characterRules / propRules).
FTCORE_API AssetRules modelRules(const ModelEntry& model);

// Every line id in the story, in play order, each once. Lines without an id are skipped.
FTCORE_API std::vector<std::string> voiceLineIds(const StoryDatabase& db);

enum class AssetState : std::uint8_t { Missing, Failed, Delivered, Imported, Count };

// "missing", "failed", "delivered", "imported"
FTCORE_API const char* assetStateName(AssetState state);
// A file that isn't there is Missing; one with rule errors is Failed; otherwise Imported or Delivered.
FTCORE_API AssetState assetState(bool filePresent, std::uint32_t errors, bool imported);

struct AssetTally {
    std::array<std::uint32_t, static_cast<std::size_t>(AssetState::Count)> counts{};

    void add(AssetState state) { ++counts[static_cast<std::size_t>(state)]; }
    std::uint32_t count(AssetState state) const { return counts[static_cast<std::size_t>(state)]; }
    FTCORE_API std::uint32_t total() const;
    // "N expected: N imported, N delivered, N missing, N failed"
    FTCORE_API std::string summary() const;
};

} // namespace ftcore
