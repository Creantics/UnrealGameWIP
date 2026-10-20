#pragma once

// Which beats the player has already seen, for "skip only seen lines": one bit per beat of the
// loaded story database (a compact bitset, beat i of scene s is bit s.firstBeat + i). Sized once by
// setDatabase(); mark() and seen() never allocate. It is saved with the story save (StorySave::seen,
// by scene id and beat index, so a re-import that reorders scenes keeps it): captureSeen() on save,
// applySeen() on load.
//
//   SeenLines seen;
//   seen.setDatabase(&db);                 // clears
//   seen.mark(runner.sceneIndex(), runner.beatIndex());
//   if (seen.seen(scene, beat)) ...

#include "ftcore/Api.h"
#include "ftcore/StoryDatabase.h"
#include "ftcore/StorySave.h"

#include <cstdint>
#include <vector>

namespace ftcore {

class StoryRunner;

class SeenLines {
public:
    // Sizes the bitset for `db` (allocates once) and clears it. nullptr: empty.
    FTCORE_API void setDatabase(const StoryDatabase* db);
    const StoryDatabase* database() const { return db_; }
    FTCORE_API void clear();

    // False for a beat that does not exist.
    FTCORE_API bool seen(std::uint32_t scene, std::uint32_t beat) const;
    FTCORE_API void mark(std::uint32_t scene, std::uint32_t beat);
    // The runner's current beat.
    FTCORE_API bool seen(const StoryRunner& runner) const;
    FTCORE_API void mark(const StoryRunner& runner);

    // Number of beats marked.
    FTCORE_API std::uint32_t count() const;

private:
    std::size_t indexOf(std::uint32_t scene, std::uint32_t beat) const; // npos when out of range

    const StoryDatabase* db_ = nullptr;
    std::vector<std::uint64_t> words_;
};

// Fills save.seen from `seen` (only scenes with a seen beat). Allocates: call on save.
FTCORE_API void captureSeen(const SeenLines& seen, StorySave& save);
// Clears `seen`, then marks the beats listed in save.seen. Scenes the story no longer has and
// beats past the end of a scene are skipped. Returns the number of beats marked. Allocates nothing.
FTCORE_API std::uint32_t applySeen(const StorySave& save, SeenLines& seen);

} // namespace ftcore
