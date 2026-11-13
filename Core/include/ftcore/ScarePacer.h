#pragma once

// ScarePacer: the governor every jump scare asks before it plays, in all three worlds.
//
// Rules (play time, kept by the pacer's own clock so a reload keeps the gap and the window):
// * Window: a rolling 60 min. A scare counts while it is less than 60 min old (one exactly 60 min old no longer
//   counts). The cap is the new scare's world's: 2, or 3 in the Unwritten. Every world's scares count.
// * Gap: at least 15 min since the last scare (10 if the new one is in the Unwritten); exactly the gap is allowed.
// * Kind: never the same kind as the last scare.
// * Moments: a moment is seen once it completes (its scare beat finished, in any form) and never plays again in that
//   save. One that played but did not complete before a reload may play again.
// * Quiet: none within 180 s of a load, a death or a tutorial prompt (markQuiet), and none in the Finale (setFinale).
// * Setting: Full or Soft play the scare (Soft without the sting); Off drops ambient scares and plays scripted ones
//   Calm. A scripted moment refused by any rule plays Calm too. A Calm play is not recorded; it still completes.
//
// Moments: a scripted moment is `scare_NNN` (Data/Scares/placements.json, NNN 1..kScareScriptedMax); an ambient one
// is its kind plus a place number (0..kScareAmbientPlaces-1), so each place scares once per save per kind. Seen bits
// are keyed by those numbers, never by a table row, so a reordered table leaves saves alone.
//
// No allocation after construction: the ring and the seen bits are fixed arrays. Save and load (ScareSave) copy them.

#include "ftcore/Api.h"

#include <array>
#include <bitset>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

enum class ScareKind : std::uint8_t { Exploration, Scene, Fight, Hallucination, Map, Count }; // append only
enum class ScareWorld : std::uint8_t { Solhaven, Lattice, Unwritten, Count };                // append only
enum class ScareSetting : std::uint8_t { Full, Soft, Off, Count };                           // append only
// How a moment plays. None: an ambient scare that does not play at all. Unpaced: a hallucination not flagged as a
// scare (its own pacing decides; the pacer records nothing).
enum class ScareForm : std::uint8_t { Full, Soft, Calm, None, Unpaced }; // append only

inline constexpr std::size_t kScareRing = 8;
inline constexpr std::uint16_t kScareScriptedMax = 255;   // scare_001 .. scare_255
inline constexpr std::uint16_t kScareAmbientPlaces = 256; // per kind
inline constexpr std::size_t kScareKinds = static_cast<std::size_t>(ScareKind::Count);
inline constexpr std::size_t kScareSeenBits = (kScareScriptedMax + 1) + kScareKinds * kScareAmbientPlaces;
inline constexpr double kScareWindowSeconds = 60.0 * 60.0;
inline constexpr double kScareQuietSeconds = 180.0;

FTCORE_API const char* scareKindName(ScareKind k);   // "exploration", "scene", "fight", "hallucination", "map"
FTCORE_API const char* scareWorldName(ScareWorld w); // "solhaven", "lattice", "unwritten"
FTCORE_API const char* scareSettingName(ScareSetting s);
FTCORE_API const char* scareFormName(ScareForm f);
FTCORE_API bool scareKindFromName(std::string_view name, ScareKind& out);
FTCORE_API bool scareWorldFromName(std::string_view name, ScareWorld& out);
FTCORE_API int scareWindowCap(ScareWorld w);    // 2, or 3 in the Unwritten
FTCORE_API double scareGapSeconds(ScareWorld w); // 900, or 600 in the Unwritten

struct ScareMoment {
    bool ambient = false;
    ScareKind ambientKind = ScareKind::Exploration; // ambient only
    std::uint16_t number = 0;                       // scripted: NNN of scare_NNN; ambient: the place number
    bool operator==(const ScareMoment&) const = default;
};
// "scare_001" .. "scare_255" (three digits or more, no sign); false for anything else.
FTCORE_API bool scareMomentFromId(std::string_view id, ScareMoment& out);
FTCORE_API ScareMoment scareAmbient(ScareKind kind, std::uint16_t place);
FTCORE_API bool scareMomentValid(const ScareMoment& m);

// What a save keeps. seenHex: the seen bits as lowercase hex, bit i is bit (i % 4) of digit i / 4, trailing zeros
// left off (the story save's seen-beat form).
struct ScareSave {
    double playSeconds = 0.0;
    std::array<double, kScareRing> times{};
    std::array<ScareKind, kScareRing> kinds{};
    std::uint8_t count = 0; // entries in the ring (0..8)
    std::uint8_t head = 0;  // the next slot written
    ScareKind lastKind = ScareKind::Count; // Count: none yet
    std::string seenHex;
    bool operator==(const ScareSave&) const = default;
};

class ScarePacer {
public:
    // Play time moves on (seconds; negative or non-finite steps are ignored).
    FTCORE_API void advance(double seconds);
    double playSeconds() const { return now_; }

    void setSetting(ScareSetting s) { setting_ = s; }
    ScareSetting setting() const { return setting_; }
    void setFinale(bool inFinale) { finale_ = inFinale; }
    bool finale() const { return finale_; }
    // A load, a death or a tutorial prompt: no scare for the next 180 s of play.
    FTCORE_API void markQuiet();

    // Whether the rules let this moment be a scare now (the setting aside, except Off refusing ambient ones).
    FTCORE_API bool canFire(ScareKind kind, ScareWorld world, bool scripted, const ScareMoment& m) const;
    // The form for a moment the rules allowed or not: Full or Soft by the setting when allowed; Calm for a scripted
    // moment refused or under Off; None for an ambient one refused or under Off.
    FTCORE_API ScareForm form(bool scripted, bool allowed) const;
    // Records a scare in the ring and as the last kind. Only a Full or Soft form is recorded; others are ignored.
    FTCORE_API void fire(ScareKind kind, ScareForm f);
    // canFire, then form, then fire: the one call a scene makes when a moment starts. Returns the form to play.
    FTCORE_API ScareForm play(ScareKind kind, ScareWorld world, bool scripted, const ScareMoment& m);
    // An authored hallucination moment (always scripted, in the Unwritten): only one flagged as a scare is paced
    // (play as Hallucination); an unflagged one returns Unpaced and records nothing.
    FTCORE_API ScareForm hallucination(bool flaggedAsScare, const ScareMoment& m);
    // The moment's scare beat finished (in any form): it never plays again in this save.
    FTCORE_API void complete(const ScareMoment& m);
    FTCORE_API bool seen(const ScareMoment& m) const;

    // Scares inside the window now, and the last one's play time (negative: none).
    FTCORE_API int inWindow() const;
    FTCORE_API double lastTime() const;
    ScareKind lastKind() const { return lastKind_; }

    FTCORE_API void save(ScareSave& out) const;
    // False (pacer untouched) when the save is malformed: count past the ring, head past it, a kind past Count, a
    // non-finite or negative time, or seenHex with a non-hex digit or more bits than kScareSeenBits.
    FTCORE_API bool load(const ScareSave& in);

private:
    double now_ = 0.0;
    double quietAt_ = -1e30;
    std::array<double, kScareRing> times_{};
    std::array<ScareKind, kScareRing> kinds_{};
    std::uint8_t count_ = 0, head_ = 0;
    ScareKind lastKind_ = ScareKind::Count;
    ScareSetting setting_ = ScareSetting::Full;
    bool finale_ = false;
    std::bitset<kScareSeenBits> seen_;
};

// ---- the placement table and its authoring check ----
// Data/Scares/placements.json: { "version": 1, "placements": [ { "id": "scare_001", "world": "lattice",
//   "kind": "scene", "minute": 41.2, "finale": false }, ... ] }. minute: play minutes from a new game (the scene's
// rough start plus where the scare falls in it). finale is optional (false). Ids and descriptions of scenes stay out
// of the file. Unknown keys refuse it.
struct ScarePlacement {
    ScareMoment moment;
    ScareWorld world = ScareWorld::Solhaven;
    ScareKind kind = ScareKind::Exploration;
    double minute = 0.0;
    bool finale = false;
};
inline constexpr std::size_t kScarePlacementsMax = 64;
struct ScarePlacementTable {
    std::array<ScarePlacement, kScarePlacementsMax> rows{};
    std::size_t count = 0;
};
// False on a refused file (`out` emptied, *error one problem per line): bad JSON, a bad or repeated id, an unknown
// world or kind, a minute that is negative or not a number, more than kScarePlacementsMax rows, unknown keys.
FTCORE_API bool importScarePlacements(std::string_view json, ScarePlacementTable& out, std::string* error = nullptr);
// Plays the table in minute order through a pacer (setting Full, every moment scripted) and lists each placement it
// would not let play as a scare, and each one in the Finale. Empty: the table keeps every rule.
FTCORE_API std::vector<std::string> checkScarePlacements(const ScarePlacementTable& table);

} // namespace ftcore
