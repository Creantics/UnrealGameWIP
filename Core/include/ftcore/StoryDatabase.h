#pragma once

// The story script as data: chapters, scenes in play order, beats, choice options, flags and cues.
//
//   ftcore::StoryBuilder b;
//   b.beginChapter("01", "Chapter One");
//   b.beginScene("SC-A", "Alpha");
//   b.line("VO-A-001", "SPEAKER", "Hello.");
//   ...
//   ftcore::StoryDatabase db;
//   std::string error;
//   if (!b.finish(db, &error)) ...;   // allocates: build at load time only
//   ftcore::StoryRunner runner(&db);
//
// Core has no JSON reader: whoever loads the script (an Unreal importer, a tool, a test) feeds the
// builder. Everything is resolved by finish(): scene / flag / cue ids to indices, choice targets,
// `end` targets and the scene that follows each scene in chapter order. After that every query
// returns views into storage owned by the database and nothing allocates.
//
// Display text is the script's own text (markdown `*` emphasis removed unless the builder is told
// to keep it). Localisation is the owner's job: every shown beat and option keeps its `loc` key,
// which the owner resolves against its own string table.
//
// The builder is tolerant in the same way the newengine loader was: an unknown beat is a no-op,
// flags and cues used by beats but never declared are added, an `end` to an unknown scene falls
// back to chapter order and a choice target past the scene's end ends the scene (each a warning).
// A duplicate scene id, an unknown start scene, no scenes at all, or a beat / option / effect /
// param with nothing to attach to are errors.

#include "ftcore/Api.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ftcore {

inline constexpr std::uint32_t kNone = 0xFFFFFFFFu;

enum class BeatType : std::uint8_t { Line, Direction, Choice, Gameplay, Cue, Flag, Hold, End, Unknown };
enum class GameplayKind : std::uint8_t { Combat, Engage, Play, Mini, Other }; // CMB, ENG, PLAY, MINI, anything else
enum class CueAction : std::uint8_t { Play, Stop, Stinger };
// `End` closes a `require` block (a failed require skips past the next `end` flag beat of the scene).
enum class FlagOp : std::uint8_t { Set, Clear, Require, End };

FTCORE_API std::string_view beatTypeName(BeatType t);
FTCORE_API std::string_view gameplayKindName(GameplayKind k);
// "CMB" -> Combat, "ENG" -> Engage, "PLAY" -> Play, "MINI" -> Mini, anything else -> Other.
FTCORE_API GameplayKind gameplayKindFromName(std::string_view name);

// A stored range of characters inside the database (offset + length into its arena).
struct Str {
    std::uint32_t offset = 0, length = 0;
    bool empty() const { return length == 0; }
};

// Flag value. Booleans are 0 / 1, integers (counters) are themselves and strings (enum flags,
// defaults like `UNSET`) are an index into the database's interned flag strings
// (StoryDatabase::flagString), so comparing two values never touches text.
struct FlagValue {
    enum class Kind : std::uint8_t { Bool, Int, String };
    std::int64_t value = 0;
    Kind kind = Kind::Bool;

    static constexpr FlagValue boolean(bool b) { return {b ? 1 : 0, Kind::Bool}; }
    static constexpr FlagValue integer(std::int64_t v) { return {v, Kind::Int}; }
    // Booleans and integers: non-zero. Strings: always true (an enum flag holds some state;
    // `UNSET` is still a value and resolving it is the game's call).
    constexpr bool truthy() const { return kind == Kind::String || value != 0; }
    friend bool operator==(const FlagValue&, const FlagValue&) = default;
};

struct FlagOpDef {
    FlagOp op = FlagOp::Set;
    std::uint32_t flag = kNone;                  // index into StoryDatabase::flag()
    FlagValue value = FlagValue::boolean(true);  // set: the value; require: the value the flag must have
};

struct Beat {
    BeatType type = BeatType::Unknown;
    Str id;        // line: voice / text id
    Str speaker;   // line: script label
    Str direction; // line: delivery tag ("quietly")
    Str text;      // display text (choice: its prompt)
    Str loc;       // loc key for `text`
    Str note;      // direction / hold note (designer notation, never shown)
    bool hidden = false; // direction: a designer notation; the runner skips it
    // choice
    bool scripted = false;
    std::uint32_t firstOption = 0, optionCount = 0;
    // gameplay
    GameplayKind kind = GameplayKind::Other;
    Str kindName;                                 // "CMB", "PLAY", ...
    std::uint32_t firstParam = 0, paramCount = 0; // key/value pairs (StoryDatabase::param)
    // cue
    std::uint32_t cue = kNone; // index into StoryDatabase::cue()
    CueAction cueAction = CueAction::Play;
    // flag
    FlagOpDef flagOp;
    // hold
    float seconds = 0.f;
    // end
    std::uint32_t nextScene = kNone; // resolved `end` target; kNone = chapter order
};

struct Option {
    Str text, loc;
    bool selectable = true; // false: an authored greyed-out option (cannot be chosen)
    bool miss = false;      // a wrong pick in a dialogue exchange: corrected, then asked again
    std::uint32_t firstEffect = 0, effectCount = 0;
    std::uint32_t next = kNone; // beat index inside the scene; kNone = the beat after the choice
};

struct Scene {
    Str id, title, chapter, world, location, time;
    Str mode;     // the script's raw mode string, informational
    Str gameplay; // optional gameplay header text
    std::uint32_t chapterIndex = kNone;
    std::uint32_t firstBeat = 0, beatCount = 0;
    std::uint32_t orderNext = kNone; // the scene after this one in chapter order (kNone: last)
};

struct Chapter {
    Str id, title;
    std::uint32_t firstScene = 0, sceneCount = 0; // range of scene indices (scenes are in play order)
};

struct FlagDef {
    Str id;
    FlagValue initial = FlagValue::boolean(false);
};

struct CueDef {
    Str id, kind;
};

// Optional scene header fields (all informational).
struct SceneInfo {
    std::string_view world, location, time, mode, gameplay;
};

class StoryDatabase {
public:
    bool loaded() const { return !scenes_.empty(); }
    std::string_view title() const { return view(title_); }
    std::uint32_t startScene() const { return start_; }

    std::size_t sceneCount() const { return scenes_.size(); }
    const Scene& scene(std::uint32_t i) const { return scenes_[i]; }
    FTCORE_API std::uint32_t findScene(std::string_view id) const; // kNone when unknown; no allocation
    const Beat& beat(const Scene& s, std::uint32_t index) const { return beats_[s.firstBeat + index]; }
    std::size_t beatCount() const { return beats_.size(); }
    const Option& option(const Beat& choice, std::uint32_t i) const { return options_[choice.firstOption + i]; }
    const FlagOpDef& effect(const Option& o, std::uint32_t i) const { return effects_[o.firstEffect + i]; }

    std::size_t chapterCount() const { return chapters_.size(); }
    const Chapter& chapter(std::uint32_t i) const { return chapters_[i]; }

    std::size_t flagCount() const { return flags_.size(); }
    const FlagDef& flag(std::uint32_t i) const { return flags_[i]; }
    FTCORE_API std::uint32_t findFlag(std::string_view id) const; // kNone when unknown; no allocation
    // The text of a string flag value ("" for booleans / integers).
    FTCORE_API std::string_view flagString(FlagValue v) const;
    // Flag value <-> text (saves): "true" / "false", an integer, or a string value. parseFlagValue
    // only knows string values that occur in the script (false otherwise). formatFlagValue
    // allocates: not meant for per-frame use.
    FTCORE_API std::string formatFlagValue(FlagValue v) const;
    FTCORE_API bool parseFlagValue(std::string_view text, FlagValue& out) const;
    // The string flag value with this text (even one that reads like a number). False when the
    // script never uses it.
    FTCORE_API bool findFlagString(std::string_view text, FlagValue& out) const;

    std::size_t cueCount() const { return cues_.size(); }
    const CueDef& cue(std::uint32_t i) const { return cues_[i]; }

    // Gameplay parameters of a gameplay beat, as text ("" when absent).
    FTCORE_API std::string_view param(const Beat& gameplay, std::string_view key) const;
    std::string_view paramKey(const Beat& gameplay, std::uint32_t i) const { return view(params_[gameplay.firstParam + i].first); }
    std::string_view paramValue(const Beat& gameplay, std::uint32_t i) const { return view(params_[gameplay.firstParam + i].second); }

    std::string_view view(Str s) const { return std::string_view(data_).substr(s.offset, s.length); }
    // Longest display text in bytes (for sizing UI buffers).
    std::size_t longestText() const { return longestText_; }

private:
    friend class StoryBuilder;

    std::string data_; // every string: ids, labels, display text, loc keys
    Str title_;
    std::uint32_t start_ = kNone;
    std::vector<Scene> scenes_;
    std::vector<Beat> beats_;
    std::vector<Option> options_;
    std::vector<FlagOpDef> effects_;
    std::vector<Chapter> chapters_;
    std::vector<FlagDef> flags_;
    std::vector<CueDef> cues_;
    std::vector<Str> flagStrings_; // interned string flag values
    std::vector<std::pair<Str, Str>> params_;
    std::vector<std::uint32_t> sceneById_; // scene indices sorted by id (binary search)
    std::vector<std::uint32_t> flagById_;
    std::size_t longestText_ = 0;
};

// Builds a StoryDatabase in script order: chapters, then each chapter's scenes, then each scene's
// beats. Options follow their choice, effects their option and params their gameplay beat.
// Every call allocates.
class StoryBuilder {
public:
    // Keep markdown `*` emphasis in display text (default: removed).
    void keepEmphasis(bool keep) { keepEmphasis_ = keep; }

    FTCORE_API void setTitle(std::string_view title);
    // The scene play starts at; default: the first scene.
    FTCORE_API void setStart(std::string_view sceneId);
    // Declares a flag (or sets the default of one already used).
    FTCORE_API void addFlag(std::string_view id, FlagValue initial);
    // A string flag value ("SAT", "UNSET"), interned: use it for defaults, sets and requires.
    FTCORE_API FlagValue stringValue(std::string_view s);
    FTCORE_API void addCue(std::string_view id, std::string_view kind);

    FTCORE_API void beginChapter(std::string_view id, std::string_view title);
    FTCORE_API void beginScene(std::string_view id, std::string_view title, const SceneInfo& info = {});

    // Beats, appended to the current scene.
    FTCORE_API void line(std::string_view id, std::string_view speaker, std::string_view text,
                         std::string_view direction = {}, std::string_view loc = {});
    FTCORE_API void direction(std::string_view text, std::string_view loc = {});
    // A designer notation kept in the data but never shown.
    FTCORE_API void hiddenDirection(std::string_view text, std::string_view note = {});
    FTCORE_API void cue(std::string_view cueId, CueAction action = CueAction::Play);
    FTCORE_API void hold(float seconds, std::string_view note = {});
    FTCORE_API void choice(std::string_view prompt, bool scripted = false, std::string_view loc = {});
    // An option of the last beat, which must be a choice. `next`: a beat index in this scene
    // (kNone = the beat after the choice).
    FTCORE_API void option(std::string_view text, std::uint32_t next = kNone, std::string_view loc = {});
    FTCORE_API void greyedOption(std::string_view text, std::string_view loc = {}); // not selectable
    FTCORE_API void missOption(std::string_view text, std::uint32_t next = kNone, std::string_view loc = {});
    // An effect of the last option: Set or Clear.
    FTCORE_API void effect(FlagOp op, std::string_view flagId, FlagValue value = FlagValue::boolean(true));
    FTCORE_API void gameplay(std::string_view kindName, std::string_view text, std::string_view loc = {});
    // A parameter of the last beat, which must be a gameplay beat.
    FTCORE_API void param(std::string_view key, std::string_view value);
    // set / clear / require take a flag; end (closing a require block) takes none.
    FTCORE_API void flag(FlagOp op, std::string_view flagId = {}, FlagValue value = FlagValue::boolean(true));
    // Ends the scene; `nextScene` empty = the next scene in chapter order.
    FTCORE_API void end(std::string_view nextScene = {});
    // A beat type the importer did not recognise: kept as a no-op.
    FTCORE_API void unknown();

    // Resolves everything and moves the result into `out`. False on errors (`out` is then left
    // empty and `*error` says why). Either way the builder is then empty again, except for its
    // warnings, which stay until reset().
    FTCORE_API bool finish(StoryDatabase& out, std::string* error = nullptr);
    const std::vector<std::string>& warnings() const { return warnings_; }
    FTCORE_API void reset();

private:
    Str store(std::string_view s);
    Str storeText(std::string_view s); // display text: emphasis stripped, longestText tracked
    std::uint32_t internFlag(std::string_view id);
    std::uint32_t internCue(std::string_view id, std::string_view kind);
    Beat* addBeat(BeatType type);
    Option* addOption(std::string_view text, std::uint32_t next, std::string_view loc);
    void fail(std::string_view what);
    void warn(std::string what) { warnings_.push_back(std::move(what)); }

    StoryDatabase db_;
    std::vector<std::pair<std::uint32_t, Str>> endTargets_; // (beat index, target scene id), resolved in finish
    std::string start_;
    std::string error_;
    std::vector<std::string> warnings_;
    bool keepEmphasis_ = false;
};

} // namespace ftcore
