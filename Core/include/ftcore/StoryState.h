#pragma once

// Story state: the facts a playthrough remembers (what happened, what the player chose, a few counts),
// kept as one fixed-size, versioned, checksummed save block next to StorySave.
//
//   ftcore::FactRegistry reg;                                        // load time (allocates while importing)
//   ftcore::importStoryRegistry(csvText, jsonText, reg, &error);     // story_facts.csv + story_tuning.json
//   ftcore::StoryState s;                                            // a new game
//   ftcore::storyBeginScene(s, reg, ftcore::storySceneOrdinal(reg, code));
//   ftcore::storySet(s, reg, ftcore::storyFactSlot(reg, id), 2);
//   ftcore::storyCountPress(s, reg, 0, 1);                            // one accepted press: side 0, kind 1
//   ftcore::storyCommitScene(s, reg);
//   ftcore::SceneReads r = ftcore::storySceneReads(s, reg, nextScene);
//   std::array<std::uint8_t, ftcore::kStoryStateBlockSize> block;
//   ftcore::storyEncode(s, reg, block);
//
// The code is the machine; the data is the map. Facts are one byte in a numbered slot; counters are u32
// in a numbered "wide" slot. The csv names each slot, its kind, range, mutability, the scenes that may
// set it, an optional gate, and the scenes that read it. The json lists the scenes in play order and
// binds the generic features (press counters, milestones, the track, derived values, cross-fact rules)
// to slots. Slots are append-only and never reused, so a save from any build loads in any other:
// unknown slots are carried through untouched, new slots read as their default. A feature whose
// binding is absent does nothing. Nothing here allocates except the importer.
// docs/STORY_STATE.md has the format and rules.

#include "ftcore/Api.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
constexpr std::uint32_t kMaxStoryScenes = 128; // scene ordinals 0..127
constexpr std::uint32_t kMaxMilestones = 8;
constexpr std::uint32_t kMaxDerived = 16;
constexpr std::uint32_t kMaxStoryRules = 32;
constexpr std::uint32_t kMaxPressCap = 65535; // the largest cap a press counter may have
constexpr std::uint8_t kNoScene = 255;
constexpr std::uint8_t kNoWide = 255;
constexpr std::uint8_t kNotOffered = 255;
constexpr std::uint16_t kStoryLayoutVersion = 1;
constexpr std::uint16_t kStoryMidScene = 1; // header flags: written while a scene was open
constexpr std::uint16_t kStorySandbox = 2;  // a replay copy (never encoded)
constexpr std::uint16_t kStoryWideEventBit = 0x8000; // a Rejected event's id for a wide slot

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
    Stepped,  // ST: 0..max, only upward, at most one step per set scene
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
    std::uint8_t firstSet = kNoScene;
    SceneSet reads;
    std::string_view id() const { return {idChars.data(), std::char_traits<char>::length(idChars.data())}; }
};

// The slots the generic features use (kNoWide / 0: not bound, the feature does nothing).
struct StoryRoles {
    std::array<std::array<std::uint8_t, 3>, 2> pressCounters{{{kNoWide, kNoWide, kNoWide}, {kNoWide, kNoWide, kNoWide}}};
    std::uint8_t pressScene = kNoWide;  // presses in the open scene (zeroed as a scene begins)
    std::uint8_t pressTotal = kNoWide;  // every press
    std::uint8_t retries = kNoWide;     // +1 per restart
    std::uint8_t milestone = kNoWide;   // raised by the milestones
    std::uint8_t trackMark = kNoWide;   // the track side's presses in the track scene
    std::array<std::uint16_t, 2> lastKind{0, 0}; // per side: the last kind pressed + 1
    std::uint16_t firstPress = 0;       // latched to 1 by side 0's first press
    std::array<std::uint16_t, 2> sideOpen{0, 0}; // a side presses only while this fact is set
};

struct StoryTuning {
    std::uint8_t trackLength = 0;  // squares on the track
    std::uint8_t trackScene = kNoScene;
    std::uint8_t trackSide = 1;
    std::uint8_t milestoneCount = 0; // at a milestone scene's commit the milestone count rises to the value
    std::array<std::uint8_t, kMaxMilestones> milestoneScene{};
    std::array<std::uint32_t, kMaxMilestones> milestoneValue{};
    std::array<std::uint16_t, kMaxMilestones> milestoneNeeds{}; // a fact that must be set once reached (0: none)
    std::uint32_t pressFloor = 0; // storyPressTotal counts each side as at least this
};

// A derived value: computed from facts at scene start, never stored.
enum class DeriveKind : std::uint8_t {
    None,
    Value,      // the fact's value; valid when set
    Read,       // the fact's read value (unset reads the default, 255 never offered); valid unless unknown
    In,         // 1 when the value's bit is in `mask`, else 0; valid when set
    Upgrade,    // 0 unset, else the value, or `to` when it is `from` and `cond` holds; valid when unset or set
    TopKind,    // storyTopKind
    PressTotal, // storyPressTotal
};
struct DeriveRow {
    DeriveKind kind = DeriveKind::None;
    std::uint16_t fact = 0;
    std::uint32_t mask = 0;
    std::uint8_t from = 0;
    std::uint8_t to = 0;
    FactCond cond;
};

// A cross-fact validation rule bound in data.
enum class RuleOp : std::uint8_t {
    Set,     // holds an in-range value (not 0, not 255)
    Equals,  // == value
    Field,   // packed field `field` == value
    AnyBits, // value & mask != 0
};
struct RuleCond {
    std::uint16_t fact = 0;
    RuleOp op = RuleOp::Set;
    std::uint8_t field = 0;
    std::uint8_t value = 0;
};
enum class RuleKind : std::uint8_t {
    Needs,       // when `when` holds, `then` must hold (settled: only once `when`'s first set scene is committed)
    BitsInOrder, // a mask fact sets bit n only after bit n-1
};
struct StoryRule {
    std::uint8_t number = 0; // the issue's rule number
    RuleKind kind = RuleKind::Needs;
    bool settled = false;
    RuleCond when;
    RuleCond then;
};

struct FactRegistry {
    std::array<FactRow, kFactSlots> facts{};
    std::array<WideRow, kWideSlots> wide{};
    std::uint16_t factsKnown = 0; // highest fact slot in the table
    std::uint16_t wideKnown = 0;  // highest wide slot + 1
    std::uint32_t hash = 0;       // FNV-1a 32 of the scene codes and the ids in slot order (0: empty)
    std::array<std::array<char, 8>, kMaxStoryScenes> sceneCodes{};
    std::uint8_t sceneCount = 0;
    StoryRoles roles;
    StoryTuning tuning;
    std::array<DeriveRow, kMaxDerived> derived{};
    std::uint8_t derivedCount = 0;
    std::array<StoryRule, kMaxStoryRules> rules{};
    std::uint8_t ruleCount = 0;
};

// Reads the csv (header: slot,id,kind,max,default,mut,set_scenes,gate,read_scenes,fallback,derive) and
// the json (scenes, roles, track, milestones, derived values, rules); both are required. On failure
// `out` is left empty and `error` lists every problem, one per line.
FTCORE_API bool importStoryRegistry(std::string_view csv, std::string_view json, FactRegistry& out,
                                    std::string* error = nullptr);

// A scene code's ordinal (its place in the json's scene list); kNoScene when not listed.
FTCORE_API std::uint8_t storySceneOrdinal(const FactRegistry& reg, std::string_view code);
// The code for an ordinal ("" when out of range).
FTCORE_API std::string_view storySceneCode(const FactRegistry& reg, std::uint8_t ordinal);
// A fact's slot by id (0 when unknown or retired); a wide slot by id (kNoWide when unknown).
FTCORE_API std::uint16_t storyFactSlot(const FactRegistry& reg, std::string_view id);
FTCORE_API std::uint8_t storyWideSlot(const FactRegistry& reg, std::string_view id);

// ---- The state ----

enum class FactStatus : std::uint8_t { Unset, Set, NotOffered, Stale, Unknown };
enum class SetResult : std::uint8_t {
    Ok, Unchanged, RejectedOnce, RejectedRange, RejectedDownward, RejectedGate, RejectedNoScene,
    RejectedWrongScene, UnknownFact
};
// BadScene: the open or last committed scene, or a stamp, is neither a scene ordinal nor 255 (none).
// BadVersion: a layout version of 0.
enum class StoryLoad : std::uint8_t { Ok, Empty, BadMagic, BadSize, BadChecksum, NewerLayout, BadScene, BadVersion };

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
    // Mid-scene encodes that wrote no stamps-at-entry record (not saved): the extension had no room or
    // was kept raw. That block restarts the open scene with the stamps as saved, not as the scene began.
    std::uint32_t lostStampRecords = 0;
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

// The registry's derived values in its order; valid: bit i for value i.
struct DerivedValues {
    std::uint16_t valid = 0;
    std::array<std::uint32_t, kMaxDerived> value{};
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
    std::uint8_t rule = 0;    // V01..V20, or a data rule's number
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
// The press total: per side, the larger of the registry's press floor and that side's presses, summed
// (saturating).
FTCORE_API std::uint32_t storyPressTotal(const StoryState& s, const FactRegistry& reg);
// The top kind: the kind side 0 pressed most; a tie goes to the last kind pressed if it is tied, else to the lowest
// tied kind. No presses (or no counters bound): 0.
FTCORE_API std::uint8_t storyTopKind(const StoryState& s, const FactRegistry& reg);
// CRC-32 (reflected 0xEDB88320); storyCrc32 of "123456789" is 0xCBF43926.
FTCORE_API std::uint32_t storyCrc32(std::span<const std::uint8_t> bytes, std::uint32_t crc = 0);

// ---- Use ----

FTCORE_API void storyReset(StoryState& s); // a new game
FTCORE_API FactRead storyGet(const StoryState& s, const FactRegistry& reg, std::uint16_t slot);
FTCORE_API SetResult storySet(StoryState& s, const FactRegistry& reg, std::uint16_t slot, std::uint8_t value);
// Adds bits to a Bits fact or a Packed fact's flag bit (the result goes through storySet's rules).
FTCORE_API SetResult storySetBits(StoryState& s, const FactRegistry& reg, std::uint16_t slot, std::uint8_t bits);
FTCORE_API std::uint32_t storyWide(const StoryState& s, std::uint8_t slot);
// A counter: saturating add (Saturated once on reaching the cap). A mask: adds the bits in n.
FTCORE_API SetResult storyAdd(StoryState& s, const FactRegistry& reg, std::uint8_t slot, std::uint32_t n);
// One accepted press: side 0..1, kind 0..2. Counts the kind and, where
// bound, the scene and the total, records the last kind and latches the first press on side 0.
// A side with an open fact bound presses only while it is set. Ok when the kind's counter rose,
// Unchanged at its cap, UnknownFact when that counter is not bound.
FTCORE_API SetResult storyCountPress(StoryState& s, const FactRegistry& reg, std::uint8_t side, std::uint8_t kind);
// Every fact the scene reads (registry order), its wide values, and the derived values. Scene start,
// not per frame.
FTCORE_API SceneReads storySceneReads(const StoryState& s, const FactRegistry& reg, std::uint8_t scene);

// False when a scene is open, the ordinal is past kMaxStoryScenes, or the scene is not after the last
// committed one. Zeroes the per-scene press count when bound.
FTCORE_API bool storyBeginScene(StoryState& s, const FactRegistry& reg, std::uint8_t scene);
// Back to the scene's start: facts, stamps and counters (not the Raw ones); retries + 1 when bound.
FTCORE_API bool storyRestartScene(StoryState& s, const FactRegistry& reg);
// Closes the scene: unanswered gated choices and optional acts become 255 / 1, milestones and the
// track mark apply.
FTCORE_API bool storyCommitScene(StoryState& s, const FactRegistry& reg);
// A replay copy for a scene select: committed up to the scene before, never encoded. Once facts
// stamped with this scene or a later one read as unset again; facts written in several scenes and
// the counters keep their values.
FTCORE_API void storyMakeSandbox(const StoryState& real, const FactRegistry& reg, std::uint8_t scene,
                                 StoryState& out);

FTCORE_API std::uint32_t storyDrainEvents(StoryState& s, std::span<StoryEvent> out);
// The impossible combinations; never repairs. Returns the number written (at most out.size(), 32).
FTCORE_API std::uint32_t storyValidate(const StoryState& s, const FactRegistry& reg, std::span<StoryIssue> out);

// Writes the block (and bumps saveSerial). False for a sandbox (out untouched). With an empty registry
// the loaded registry hash is kept.
FTCORE_API bool storyEncode(StoryState& s, const FactRegistry& reg, std::span<std::uint8_t, kStoryStateBlockSize> out);
FTCORE_API StoryLoad storyDecode(std::span<const std::uint8_t> in, const FactRegistry& reg, StoryState& out,
                                 StoryStateLoadReport* report = nullptr);
// A saved block, or a new game's state: out is the decoded state when storyDecode says Ok, else (an empty
// block from a save made before it, or a refused one) storyReset's, never the state it held before.
FTCORE_API StoryLoad storyLoadOrNew(std::span<const std::uint8_t> in, const FactRegistry& reg, StoryState& out,
                                    StoryStateLoadReport* report = nullptr);
// The result's name, for logs ("ok", "empty", "bad magic", ...).
FTCORE_API const char* storyLoadName(StoryLoad r);

// ---- The scene flow ----

// One story-runner scene id and the registry scene it plays.
struct StorySceneLink {
    std::string scene;
    std::uint8_t ordinal = kNoScene;
};

// The runner's scene ids to registry scene codes: a two-column csv, `scene,code` header, one link a line
// (blank lines and lines starting with '#' skipped, spaces around a field trimmed). Load time (allocates).
// False, with `error` naming the line, on a missing header, a line without exactly two fields, an empty
// field, a code the registry does not list, or a scene id given twice; `out` is then empty.
FTCORE_API bool importStorySceneMap(std::string_view csv, const FactRegistry& reg, std::vector<StorySceneLink>& out,
                                    std::string* error = nullptr);

// What entering a scene did.
struct StorySceneStep {
    bool committed = false; // the open scene was committed (the flow left it)
    bool began = false;     // the scene began
    bool restarted = false; // it was the open scene: back to its start (a retry or a reload inside it)
    bool refused = false;   // a commit or begin Core refused (a scene not after the last committed one, ...)
};

// The runner reached `scene` (kNoScene: a scene with no registry code, or the story's end). The open scene,
// when it is `scene`, restarts; else it is committed and `scene` (when not kNoScene) begins.
FTCORE_API StorySceneStep storyEnterScene(StoryState& s, const FactRegistry& reg, std::uint8_t scene);

static_assert(sizeof(StoryState) <= 4096, "the story state stays a small value");

} // namespace ftcore
