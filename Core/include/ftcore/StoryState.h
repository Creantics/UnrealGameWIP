#pragma once

// Story state: the facts a playthrough remembers (what happened, what the player chose, a few counts),
// kept as one fixed-size, versioned, checksummed save block next to StorySave.
//
//   ftcore::FactRegistry reg;                                   // load time (allocates while importing)
//   ftcore::importStoryFacts(csvText, reg, &error);             // Data/Story/story_facts.csv
//   ftcore::importStoryTuning(jsonText, reg.tuning, &error);    // Data/Story/story_tuning.json
//   ftcore::StoryState s;                                       // a new game
//   ftcore::storyBeginScene(s, ftcore::storySceneOrdinal("p03"));
//   ftcore::storySet(s, reg, ftcore::StoryFact::sp03_2, 2);     // B
//   ftcore::storyCountHelp(s, reg, 0, 1);                       // one accepted press: side 0, kind 1
//   ftcore::storyCommitScene(s, reg);
//   ftcore::SceneReads r = ftcore::storySceneReads(s, reg, ftcore::storySceneOrdinal("p04"));
//   std::array<std::uint8_t, ftcore::kStoryStateBlockSize> block;
//   ftcore::storyEncode(s, reg, block);
//
// Facts are one byte in a numbered slot; counters are u32 in a numbered "wide" slot. The registry (csv)
// names each slot, its kind, range, mutability, the scenes that may set it, an optional gate, and the
// scenes that read it. Slots are append-only and never reused, so a save from any build loads in any
// other: unknown slots are carried through untouched, new slots read as their default.
// Scenes are ordinals 0..74 (p01..p08, 101..117, 201..216, 301..315, 401..409, f01..f10).
// Nothing here allocates except the two importers. docs/STORY_STATE.md has the format and rules.

#include "ftcore/Api.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ftcore {

constexpr std::uint32_t kFactSlots = 256;
constexpr std::uint32_t kWideSlots = 64;
constexpr std::uint32_t kStoryStateBlockSize = 2048;
constexpr std::uint32_t kStoryExtensionOffset = 1312;
constexpr std::uint32_t kStoryExtensionBytes = kStoryStateBlockSize - kStoryExtensionOffset; // 736
constexpr std::uint32_t kMaxReadsPerScene = 24;
constexpr std::uint32_t kMaxWideReadsPerScene = 8;
constexpr std::uint32_t kStoryEventRing = 32;
constexpr std::uint32_t kMaxStoryIssues = 32;
constexpr std::uint8_t kStoryScenes = 75;
constexpr std::uint8_t kNoScene = 255;
constexpr std::uint8_t kNotOffered = 255;
constexpr std::uint16_t kStoryLayoutVersion = 1;
constexpr std::uint16_t kStoryMidScene = 1; // header flags: written while a scene was open
constexpr std::uint16_t kStorySandbox = 2;  // a replay copy (never encoded)
constexpr std::uint16_t kStoryWideEventBit = 0x8000; // a Rejected event's id for a wide slot

// "p03" -> 2, "117" -> 24, "f10" -> 74; kNoScene for anything else.
FTCORE_API std::uint8_t storySceneOrdinal(std::string_view code);
// The code for an ordinal ("" when out of range).
FTCORE_API std::string_view storySceneCode(std::uint8_t ordinal);

// The fact slots this build knows. The enumerator is the slot; the registry id is the name without the
// leading 's' (sp03_1 is "p03_1", s104_1 is "104_1"). Append only.
enum class StoryFact : std::uint16_t {
    None = 0,
    sp03_1, sp07_1, sp03_2, sp03_3, sp04_1, sp04_2, sp02_1, sp04_3, sp04_4, sp06_1,       // 1..10
    sp06_2, sp05_1, sp07_2, sp07_3, sp07_4, s104_1, s105_1, s105_2, s106_1, s106_2,       // 11..20
    s106_3, s107_1, s108_1, s109_1, s110_1, s110_2, s111_1, s111_2, s112_1, s112_2,       // 21..30
    s112_3, s113_1, s114_1, s114_2, s115_1, s115_2, s116_1, s116_2, s115_3, s203_1,       // 31..40
    s203_2, s203_3, s205_1, s206_1, s207_1, s207_2, s211_1, s213_1, s214_1, s215_1,       // 41..50
    s215_2, s216_1, s302_1, s303_1, s303_2, s304_1, s305_1, s305_2, s305_3, s306_1,       // 51..60
    s307_1, s310_1, s313_1, s314_1, s315_1, s315_2, s404_1, s405_1, s406_1, s406_2,       // 61..70
    s407_1, s407_2, s408_1, s409_1, sf05_1, sf07_1, sf07_2, sf09_1, sf10_1, sg_hl0,       // 71..80
    sg_hl1,                                                                               // 81
    Count
};

// The wide slots this build knows (the enumerator is the slot and the registry id). Append only.
enum class StoryWide : std::uint8_t {
    c_help_s0_k0, c_help_s0_k1, c_help_s0_k2, c_help_s1_k0, c_help_s1_k1, c_help_s1_k2,
    c_help_scene, c_115_help, c_milestone, c_hold_done, c_hold_slips, c_203_found, c_303_declines,
    c_retries, c_help_all, m_108,
    Count
};

// The registry id an enumerator stands for ("" for None / Count).
FTCORE_API std::string_view storyFactId(StoryFact fact);
FTCORE_API std::string_view storyWideId(StoryWide wide);

// ---- The registry ----

enum class FactKind : std::uint8_t {
    Event,    // EV: 1 happened
    Optional, // OP: 1 scene ended without it, 2 done, 255 never offered
    Choice2,  // C2..C4: 1..n = A..D, 255 never offered
    Choice3,
    Choice4,
    Number,   // NUM: 0..max (stored 0 reads as the row's default)
    Bits,     // BITS: a mask, max = the legal bits
    Packed,   // PK: 2-bit fields from bit 0 (0 unset, 1, 2; 3 is illegal); a lone top bit is a flag
    Stepped,  // ST: 0..max, only upward
    Retired,  // a dropped fact: its slot stays dead
};
enum class FactMut : std::uint8_t { Once, Mono, Free };
enum class WideKind : std::uint8_t { None, Counter, Raw, Mask }; // Raw: not rolled back by a restart

// A set of scene ordinals.
struct SceneSet {
    std::uint64_t lo = 0, hi = 0;
    bool has(std::uint8_t s) const { return s < 64 ? ((lo >> s) & 1u) != 0 : s < 128 && ((hi >> (s - 64)) & 1u) != 0; }
    void add(std::uint8_t s) {
        if (s < 64) lo |= std::uint64_t{1} << s;
        else if (s < 128) hi |= std::uint64_t{1} << (s - 64);
    }
    bool empty() const { return lo == 0 && hi == 0; }
};

// "fact = value" or "fact.fN = value" (field N of a packed fact). fact 0: no condition.
struct FactCond {
    std::uint16_t fact = 0;
    std::uint8_t field = 0xFF;
    std::uint8_t value = 0;
};

struct FactRow {
    std::array<char, 16> idChars{};
    FactKind kind = FactKind::Retired;
    FactMut mut = FactMut::Once;
    std::uint8_t max = 0;
    std::uint8_t def = 0;          // what a stored 0 reads as (Number and Stepped only)
    bool used = false;
    bool setAny = false;           // any scene may set it
    SceneSet setScenes;
    std::uint8_t firstSet = kNoScene;
    FactCond gate;                 // the whole fact is offered only if this holds
    std::array<FactCond, 4> optionGates{}; // option A..D is offered only if this holds
    bool readAll = false;          // every scene reads it (the HUD)
    SceneSet reads;
    SceneSet fallback;             // reading scenes with a branch for "not set" / "never offered"
    std::uint8_t deriveValue = 0;  // derive-on-load: write this into an old save past the set scene...
    FactCond deriveWhen;           // ...when this holds (fact 0: no rule)
    std::string_view id() const { return {idChars.data(), std::char_traits<char>::length(idChars.data())}; }
};

struct WideRow {
    std::array<char, 16> idChars{};
    WideKind kind = WideKind::None;
    std::uint32_t cap = 0; // Counter / Raw: the logical cap; Mask: the legal bits
    bool setAny = false;
    SceneSet setScenes;
    SceneSet reads;
    std::string_view id() const { return {idChars.data(), std::char_traits<char>::length(idChars.data())}; }
};

struct StoryTuning {
    std::uint8_t trackLength = 7;   // squares on the track
    std::uint8_t trackScene = 22;  // the scene whose side-1 presses move the mark ("115")
    std::uint8_t milestoneCount = 4;   // milestones: at a scene's commit the milestone count rises to the value
    std::array<std::uint8_t, 8> milestoneScene{6, 22, 50, 68};
    std::array<std::uint8_t, 8> milestoneValue{48, 61, 62, 63};
};

struct FactRegistry {
    std::array<FactRow, kFactSlots> facts{};
    std::array<WideRow, kWideSlots> wide{};
    std::uint16_t factsKnown = 0; // highest fact slot in the table
    std::uint16_t wideKnown = 0;  // highest wide slot + 1
    std::uint32_t hash = 0;       // FNV-1a 32 of the ids in slot order
    StoryTuning tuning;
};

// Reads the csv (header: slot,id,kind,max,default,mut,set_scenes,gate,read_scenes,fallback,derive).
// On failure `out` is left empty and `error` lists every problem, one per line, with its csv line.
FTCORE_API bool importStoryFacts(std::string_view csv, FactRegistry& out, std::string* error = nullptr);
// {"version":1,"trackLength":7,"trackScene":"115","milestones":[{"scene":"p07","value":48},...]}
FTCORE_API bool importStoryTuning(std::string_view json, StoryTuning& out, std::string* error = nullptr);

// ---- The state ----

enum class FactStatus : std::uint8_t { Unset, Set, NotOffered, Stale, Unknown };
enum class SetResult : std::uint8_t {
    Ok, Unchanged, RejectedOnce, RejectedRange, RejectedDownward, RejectedGate, RejectedNoScene,
    RejectedWrongScene, UnknownFact
};
// BadScene: the open or last committed scene is neither a scene nor 255 (none).
enum class StoryLoad : std::uint8_t { Ok, Empty, BadMagic, BadSize, BadChecksum, NewerLayout, BadScene };

struct StoryEvent {
    enum class Kind : std::uint8_t {
        FactSet, WideChanged, Saturated, Rejected, SceneBegun, SceneCommitted, SceneRolledBack
    };
    Kind kind = Kind::FactSet;
    std::uint16_t id = 0;       // fact slot, wide slot, or (Rejected on a wide slot) kStoryWideEventBit | slot
    std::uint32_t before = 0;   // Rejected: the SetResult
    std::uint32_t after = 0;    // Rejected: the value tried
    std::uint8_t scene = kNoScene;
};

namespace detail {
constexpr std::array<std::uint8_t, kFactSlots> noStamps() {
    std::array<std::uint8_t, kFactSlots> a{};
    for (std::uint8_t& b : a) b = kNoScene;
    return a;
}
} // namespace detail

// A plain value: copying it makes a sandbox.
struct StoryState {
    std::array<std::uint8_t, kFactSlots> facts{};
    std::array<std::uint8_t, kFactSlots> factsEntry{}; // as the open scene began
    std::array<std::uint8_t, kFactSlots> stamp = detail::noStamps(); // scene that last wrote each slot
    std::array<std::uint8_t, kFactSlots> stampEntry = detail::noStamps();
    std::array<std::uint32_t, kWideSlots> wide{};
    std::array<std::uint32_t, kWideSlots> wideEntry{};
    std::array<std::uint8_t, kStoryExtensionBytes> extension{}; // other builds' records, kept verbatim
    std::uint16_t extensionLength = 0;
    bool extensionRaw = false;  // the loaded area was malformed: kept whole as raw bytes
    std::uint8_t openScene = kNoScene;
    std::uint8_t lastCommitted = kNoScene;
    std::uint16_t flags = 0;
    std::uint32_t saveSerial = 0;
    std::uint16_t factsKnown = 0;  // from the loaded header (0 for a new game)
    std::uint16_t wideKnown = 0;
    std::uint32_t registryHash = 0;
    std::array<StoryEvent, kStoryEventRing> events{};
    std::uint8_t eventHead = 0;
    std::uint8_t eventCount = 0;
    std::uint32_t lostEvents = 0; // not saved
};

struct FactRead {
    std::uint16_t fact = 0;
    std::uint8_t value = 0;
    FactStatus status = FactStatus::Unknown;
};
struct WideRead {
    std::uint8_t wide = 0;
    std::uint32_t value = 0;
};

// Values computed from facts, never stored. valid: bit n-1 for Dn.
struct DerivedValues {
    std::uint8_t valid = 0;
    bool d1 = false;            // p03_2 is B or C
    std::uint8_t d2[2] = {0, 0}; // the two third-slot states (p03_1, p07_1)
    std::uint8_t d3 = 0;        // 116_1: 0 unset, 1 A, 2 B, 3 A and 315_1 done
    std::uint8_t d4 = 0;        // 1 when f07_1 is A, else 0 (only when f07_1 is set)
    std::uint8_t d5 = 0;        // 1 or 2 from 113_1
    std::uint8_t d6 = 0;        // storyLeadKind
    std::uint32_t d7 = 0;       // storyHelpShown
};

struct SceneReads {
    std::uint8_t scene = kNoScene;
    std::uint8_t count = 0;
    std::uint8_t wideCount = 0;
    bool complete = false; // a known scene and nothing left out
    std::array<FactRead, kMaxReadsPerScene> facts{};
    std::array<WideRead, kMaxWideReadsPerScene> wide{};
    DerivedValues derived;
};

enum class IssueSeverity : std::uint8_t { Warning, Error };
struct StoryIssue {
    std::uint8_t rule = 0;    // 1..20 for V01..V20
    std::uint16_t factA = 0;  // a fact slot, or kStoryWideEventBit | wide slot
    std::uint16_t factB = 0;
    IssueSeverity severity = IssueSeverity::Error;
};

struct StoryStateLoadReport {
    std::uint16_t unknownSlotsPreserved = 0; // fact and wide slots past this build's registry
    std::uint16_t staleFacts = 0;            // known facts holding a value outside their range
    std::uint16_t derived = 0;               // facts filled by derive-on-load
    std::uint16_t unknownTags = 0;           // extension records kept verbatim
    bool registryMismatch = false;           // the writer's registry hash differs
    bool extensionMalformed = false;         // the extension area was kept as raw bytes
};

// ---- Formulas ----

// min(c + n, cap), without wrapping.
FTCORE_API std::uint32_t storySaturatingAdd(std::uint32_t c, std::uint32_t n, std::uint32_t cap);
// The help total: per side, the larger of 1 and that side's presses, summed (2 at least).
FTCORE_API std::uint32_t storyHelpShown(const StoryState& s);
// The kind side 0 pressed most; a tie goes to the last kind pressed if it is tied, else to the first of
// heal (0), guard (1), count (2) among the tied. No presses: 0.
FTCORE_API std::uint8_t storyLeadKind(const StoryState& s);
// CRC-32 (reflected 0xEDB88320); storyCrc32 of "123456789" is 0xCBF43926.
FTCORE_API std::uint32_t storyCrc32(std::span<const std::uint8_t> bytes, std::uint32_t crc = 0);

// ---- Use ----

FTCORE_API void storyReset(StoryState& s); // a new game
FTCORE_API FactRead storyGet(const StoryState& s, const FactRegistry& reg, StoryFact fact);
FTCORE_API FactRead storyGetSlot(const StoryState& s, const FactRegistry& reg, std::uint16_t slot);
FTCORE_API SetResult storySet(StoryState& s, const FactRegistry& reg, StoryFact fact, std::uint8_t value);
// Adds bits to a Bits fact or a Packed fact's flag bit (the result goes through storySet's rules).
FTCORE_API SetResult storySetBits(StoryState& s, const FactRegistry& reg, StoryFact fact, std::uint8_t bits);
FTCORE_API std::uint32_t storyWide(const StoryState& s, StoryWide wide);
// A counter: saturating add (Saturated once on reaching the cap). A mask: adds the bits in n.
FTCORE_API SetResult storyAdd(StoryState& s, const FactRegistry& reg, StoryWide wide, std::uint32_t n);
// One accepted press of the third slot's button: side 0..1, kind 0 heal, 1 guard, 2 the shared count.
// Counts the kind, the scene and the raw total, records the last kind, latches p03_3 on side 0.
// Side 1 needs its slot named (p07_1). Ok when the kind's counter rose, Unchanged at its cap.
FTCORE_API SetResult storyCountHelp(StoryState& s, const FactRegistry& reg, std::uint8_t side, std::uint8_t kind);
// Every fact the scene reads (registry order), its wide values, and D1..D7. Scene start, not per frame.
FTCORE_API SceneReads storySceneReads(const StoryState& s, const FactRegistry& reg, std::uint8_t scene);

// False when a scene is open, the ordinal is unknown, or the scene is not after the last committed one.
FTCORE_API bool storyBeginScene(StoryState& s, std::uint8_t scene);
// Back to the scene's start: facts, stamps and counters (not the Raw ones); c_retries + 1.
FTCORE_API bool storyRestartScene(StoryState& s, const FactRegistry& reg);
// Closes the scene: unanswered gated choices and optional acts become 255 / 1, milestones apply.
FTCORE_API bool storyCommitScene(StoryState& s, const FactRegistry& reg);
// A replay copy for a scene select: committed up to the scene before, never encoded. Once facts
// stamped with this scene or a later one read as unset again; facts written in several scenes and
// the counters keep their values.
FTCORE_API void storyMakeSandbox(const StoryState& real, const FactRegistry& reg, std::uint8_t scene,
                                 StoryState& out);

FTCORE_API std::uint32_t storyDrainEvents(StoryState& s, std::span<StoryEvent> out);
// The impossible combinations; never repairs. Returns the number written (at most out.size(), 32).
FTCORE_API std::uint32_t storyValidate(const StoryState& s, const FactRegistry& reg, std::span<StoryIssue> out);

// Writes the block (and bumps saveSerial). False for a sandbox (out untouched).
FTCORE_API bool storyEncode(StoryState& s, const FactRegistry& reg, std::span<std::uint8_t, kStoryStateBlockSize> out);
FTCORE_API StoryLoad storyDecode(std::span<const std::uint8_t> in, const FactRegistry& reg, StoryState& out,
                                 StoryStateLoadReport* report = nullptr);

static_assert(sizeof(StoryState) <= 4096, "the story state stays a small value");

} // namespace ftcore
