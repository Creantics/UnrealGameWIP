#pragma once

// Ambient banter: the real screen behind BNT gameplay steps (GameplayScreen::Banter). Two (or a few)
// speakers trade short lines while the player looks on. Each line holds for its own time, then the
// next one shows; nothing waits for input. A press moves on at once, skip() ends the banter. There
// is no choice, no score and no flag.
//
//   ftcore::BanterDef def;
//   if (!ftcore::importBanter(text, def, &error, &warnings)) ...;   // load time, allocates
//   ftcore::BanterSequence banter;
//   banter.begin(&def);
//   every frame: banter.tick(gameSeconds);
//   draw banter.speaker(), banter.text() on the side banter.side() names
//   on continue: banter.press();  on skip: banter.skip();
//   when banter.finished(): continue to the return map
//
// A line's hold is its `hold` when the file gives one, else banterHold(): a base plus a reading time
// per character, clamped to [kBanterMinHold, kBanterMaxHold]. A speaker's side is the order it first
// speaks in: the first speaker is on the left, the second on the right, a third on the left again.
//
// Banter files hold story text, so they live in the gitignored Data/Story/banter/, one per gameplay
// step: banterFileName(scene, beat, step) ("SC-X_4_0.json"). The format is in docs/BANTER_SCREEN.md;
// Data/Banter/example_banter.json is an invented example.
//
// The runtime (BanterSequence) never allocates: it holds a pointer to the definition and hands back
// string_views into it, so the definition must outlive it. Errors and warnings from the loader name
// paths and keys only, never line text.

#include "ftcore/Api.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

inline constexpr double kBanterMinHold = 1.5;
inline constexpr double kBanterMaxHold = 8.0;
inline constexpr double kBanterHoldBase = 1.0;
inline constexpr double kBanterHoldPerByte = 0.05; // about 20 characters a second
// Bounds of an explicit `hold`.
inline constexpr double kBanterHoldFloor = 0.5;
inline constexpr double kBanterHoldCeiling = 15.0;

struct BanterLine {
    std::string speaker; // a speaker id as the script writes it
    std::string text;
    double hold = 0.0;   // seconds; 0: derived from the text (banterHold)
};

struct BanterDef {
    static constexpr std::uint32_t kMaxLines = 32;
    static constexpr std::uint32_t kMaxSpeakers = 4;

    std::string scene;
    std::uint32_t beat = 0, step = 0;
    BanterLine lines[kMaxLines];
    std::uint32_t lineCount = 0;
    // Speaker ids in the order they first speak (side = index % 2).
    std::string speakers[kMaxSpeakers];
    std::uint32_t speakerCount = 0;

    bool matches(std::string_view sceneId, std::uint32_t beatIndex, std::uint32_t stepIndex) const {
        return scene == sceneId && beat == beatIndex && step == stepIndex;
    }
    // Index of `speaker` in speakers (speakerCount when unknown).
    FTCORE_API std::uint32_t speakerIndex(std::string_view speaker) const;
    // Sum of the lines' holds.
    FTCORE_API double length() const;
};

// How long a line shows: its explicit hold, else kBanterHoldBase + kBanterHoldPerByte per byte of
// text, clamped to [kBanterMinHold, kBanterMaxHold].
FTCORE_API double banterHold(const BanterLine& line);

// Parses and validates a banter file. On failure `out` is reset and *error lists each problem with
// its path, one per line. Warnings (unknown keys) are appended to *warnings.
//
// Errors: bad JSON; version not 1; scene, beat or step missing; no lines or more than kMaxLines; a
// line that is not an object, or has no speaker or no text; a hold outside [kBanterHoldFloor,
// kBanterHoldCeiling]; more than kMaxSpeakers distinct speakers.
FTCORE_API bool importBanter(std::string_view json, BanterDef& out, std::string* error = nullptr,
                             std::vector<std::string>* warnings = nullptr);
// "<scene>_<beat>_<step>.json" (the loop files' naming). Allocates (load time).
FTCORE_API std::string banterFileName(std::string_view scene, std::uint32_t beat, std::uint32_t step);

enum class BanterSide : std::uint8_t { Left, Right };

class BanterSequence {
public:
    // Starts at the first line; a null definition (or one with no line) leaves it idle and finished.
    FTCORE_API void begin(const BanterDef* def);
    FTCORE_API void reset();
    const BanterDef* def() const { return def_; }

    bool active() const { return def_ != nullptr && !done_; }
    bool finished() const { return !active(); }
    // Bumped whenever the shown line changes (owners re-cache their strings then).
    std::uint32_t epoch() const { return epoch_; }

    // Moves the line's clock on; advances past each line whose hold ran out (several in one long
    // frame). Returns true when the shown line changed.
    FTCORE_API bool tick(double seconds);
    // The next line now. False when finished.
    FTCORE_API bool press();
    // Ends the banter. False when already finished.
    FTCORE_API bool skip();

    // ---- what to show (empty when finished) ----
    std::uint32_t line() const { return line_; }
    FTCORE_API std::uint32_t count() const;
    FTCORE_API std::string_view speaker() const;
    FTCORE_API std::string_view text() const;
    FTCORE_API BanterSide side() const;
    // Seconds the current line has shown, and its hold.
    double lineAge() const { return age_; }
    double lineHold() const { return hold_; }
    // Lines that have appeared on screen (the done log's count).
    std::uint32_t linesShown() const { return shown_; }
    bool skipped() const { return skipped_; }
    std::uint32_t presses() const { return presses_; }

private:
    void show(std::uint32_t line);

    const BanterDef* def_ = nullptr;
    std::uint32_t line_ = 0;
    std::uint32_t epoch_ = 0;
    std::uint32_t presses_ = 0;
    std::uint32_t shown_ = 0;
    double age_ = 0.0;
    double hold_ = 0.0;
    bool skipped_ = false;
    bool done_ = true;
};

} // namespace ftcore
