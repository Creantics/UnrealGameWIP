#pragma once

// NPC memory and disposition: important NPCs remember the player's deeds, their feelings shift
// (trust, fear, regard), and authored lines are picked by memory, stance and expectations.
// The system only selects lines by id; it never makes text. Everything is integer and has no RNG,
// so a reloaded save replays exactly. Decay is lazy (computed from timestamps when read), so
// nothing runs per frame; record, checkpoint, stance and line pick allocate nothing.
// Tables and lines are loaded once (the parsers allocate); the public repo carries only the
// schemas and role-id test data, the real profiles and line tables are private data.

#include "ftcore/Api.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ftcore {

constexpr std::uint32_t kNpcMax = 64, kMemPerNpc = 16, kPinnedMax = 6, kNpcTagMax = 32, kDeedRing = 256,
                        kRumourQueue = 128, kExpMax = 64, kLeadCount = 3, kCondMax = 6, kSaidMax = 2048,
                        kDeedWitnessMax = 8, kNpcTieMax = 512, kDeedTypeMax = 128, kEventMax = 64;
constexpr std::uint16_t kNoNpc = 0xFFFF;  // crowd victim (tie 0) or "none"
constexpr std::uint8_t kNoLead = 0xFF;    // the party, no single lead
constexpr std::uint16_t kNoType = 0xFFFF;

enum NpcAxis : std::uint8_t { kAxisT = 0, kAxisF = 1, kAxisR = 2 };
// Ranked: Hostile 0, Wary 1, Neutral 2, Warm 3, Loyal 4.
enum class NpcStance : std::uint8_t { Hostile = 0, Wary = 1, Neutral = 2, Warm = 3, Loyal = 4 };
// How a memory was learned; picks K_c.
enum class NpcLearn : std::uint8_t { Victim = 0, Witness = 1, Hop1 = 2, Hop2 = 3 };
enum class NpcExpState : std::uint8_t { Pending = 0, Reminding = 1, Kept = 2, Broken = 3, Missed = 4 };
enum class NpcLineKind : std::uint8_t { Greet = 0, Bark = 1, Talk = 2, Remind = 3, Foreshadow = 4 };

// Knobs (Data/Npc/memory_tuning.json).
struct NpcTuning {
    std::int32_t kVictim = 150, kWitness = 100, kHop1 = 60, kHop2 = 36, kReveal = 100;
    std::array<std::int32_t, 3> tieGain{6, 0, 6};
    std::array<std::int32_t, 3> decayPerDay{2, 8, 1};  // r_a; offsets decay at twice this
    std::int32_t salienceDecayPerHour = 3;               // D
    std::int32_t recallBar = 200;                        // "remembers"
    std::int32_t stayGap = 10;                           // stance hysteresis
    std::int32_t spreadThreshold = 120;                  // closeness * m
    std::int32_t streetThreshold = 1500;                 // m * crowdPermille
    std::int32_t streetDelayMin = 60;
    std::int32_t rkFloor = 25, rkStep = 25, rkWindowMin = 3 * 1440;
    std::int32_t leadCloseness = 70;
};

struct NpcDeedType {
    std::string id;
    std::uint32_t tagMask = 0;
    std::int8_t magnitude = 1;  // 1..5
    std::int8_t polarity = 0;   // -1 harm, 0 neutral, 1 benefit to the victim
    bool secret = false, pin = false, attributable = false;
};

struct NpcDef {
    std::string id;
    std::array<std::int16_t, 3> base{0, 0, 0};
    std::uint16_t district = 0;
    std::array<std::array<std::int8_t, 3>, kNpcTagMax> weights{};
};

enum class NpcTieKind : std::uint8_t { Npc = 0, Faction = 1, Lead = 2 };
struct NpcTie {
    std::uint16_t from = 0, to = 0;  // npc indices (to: a lead index for kind Lead, a faction id for Faction)
    NpcTieKind kind = NpcTieKind::Npc;
    std::int8_t tie = 0;             // -100..100
    std::uint8_t closeness = 0;      // 0..100
    std::uint16_t delayMin = 60;
};

struct NpcConsequence {
    std::int16_t pricePct = 100;
    bool shopOpen = true, doorOpen = true, questOpen = true;
    std::uint8_t fightRole = 1;  // 0 aid, 1 none, 2 hinder, 3 inform
    std::uint16_t greetAnim = 0;
    std::uint16_t greetDistCm = 0;
    bool vanRide = false;
};

struct NpcExpDef {
    std::string id;
    std::uint16_t npc = 0;
    std::uint32_t due = 0;  // game minute
    std::uint32_t windowMin = 60, remindLeadMin = 60;
    std::uint16_t keptBy = kNoType, brokenBy = kNoType;
    std::uint16_t deedKept = kNoType, deedBroken = kNoType, deedMissed = kNoType;
};

struct NpcEventDef {
    std::string id;
    std::uint32_t minute = 0;
    std::uint32_t foreshadowLeadMin = 0;
};

struct NpcTables {
    NpcTuning tuning;
    std::vector<std::string> tags;  // index = bit
    std::vector<NpcDeedType> deeds;
    std::vector<NpcDef> npcs;
    std::vector<NpcTie> ties;
    // consequences[npc * 5 + stance]; filled with defaults for missing rows.
    std::vector<NpcConsequence> consequences;
    std::vector<NpcExpDef> expectations;
    std::vector<NpcEventDef> events;

    FTCORE_API int tagIndex(std::string_view id) const;
    FTCORE_API int deedIndex(std::string_view id) const;
    FTCORE_API int npcIndex(std::string_view id) const;
    FTCORE_API int expIndex(std::string_view id) const;
    FTCORE_API int eventIndex(std::string_view id) const;
};

// A deed the player's party did. `place` groups reinforcement (same type, same place);
// `district` gates street hearsay. Witnesses are important NPCs only (kNoNpc pads).
struct NpcDeed {
    std::uint32_t seq = 0;  // assigned by npcRecordDeed, monotonic from 1
    std::uint32_t minute = 0;
    std::uint16_t type = 0;
    std::uint16_t place = 0;
    std::uint16_t district = 0;
    std::uint16_t victim = kNoNpc;
    std::uint8_t lead = kNoLead;
    std::uint8_t flags = 0;  // internal: street spread pending, secret
    std::array<std::uint16_t, kDeedWitnessMax> witnesses{kNoNpc, kNoNpc, kNoNpc, kNoNpc,
                                                         kNoNpc, kNoNpc, kNoNpc, kNoNpc};
};

struct NpcMemoryEntry {
    std::uint32_t seq = 0;
    std::uint32_t deedMinute = 0;
    std::uint32_t ts = 0;       // salience start (learn time or last reinforcement)
    std::uint32_t learned = 0;  // first learned
    std::uint16_t type = kNoType;
    std::uint16_t place = 0;
    std::int16_t s0 = 0;
    std::uint8_t hops = 0;  // NpcLearn
    std::uint8_t count = 0;
    std::uint8_t lead = kNoLead;
    bool pinned = false;
};

struct NpcTagAggregate {
    std::uint16_t count = 0;
    std::uint32_t lastMinute = 0;
};

struct NpcState {
    std::array<std::int16_t, 3> axes{0, 0, 0};  // A_s at axesTs
    std::uint32_t axesTs = 0;
    std::array<std::array<std::int8_t, 3>, kLeadCount> offsets{};
    std::array<std::uint32_t, kLeadCount> offsetsTs{};
    std::array<NpcStance, kLeadCount + 1> stance{NpcStance::Neutral, NpcStance::Neutral, NpcStance::Neutral,
                                                 NpcStance::Neutral};  // per lead, last = party
    bool dead = false;
    std::uint32_t deadAt = 0;
    std::array<NpcMemoryEntry, kMemPerNpc> mem{};
    std::uint8_t memCount = 0;
    std::array<NpcTagAggregate, kNpcTagMax> aggregates{};
};

struct NpcRumour {
    std::uint32_t seq = 0;
    std::uint32_t due = 0;
    std::uint16_t target = kNoNpc;
    std::uint8_t hops = 1;  // 1 or 2
    std::uint8_t lead = kNoLead;
    std::uint8_t closeness = 0;
};

struct NpcMemoryWorld {
    std::uint32_t now = 0;
    std::uint32_t nextSeq = 1;
    std::uint16_t npcCount = 0;
    std::array<NpcState, kNpcMax> npcs{};
    std::array<NpcDeed, kDeedRing> ring{};
    std::array<NpcRumour, kRumourQueue> rumours{};
    std::uint16_t rumourCount = 0;
    std::array<NpcExpState, kExpMax> exp{};
    std::array<std::uint32_t, kSaidMax> lastSaid{};  // minute + 1, 0 = never said
    std::uint32_t droppedRumours = 0;
};

// Sets the world up from the tables: baselines, expectation states, a clear store.
FTCORE_API void npcInit(NpcMemoryWorld& w, const NpcTables& t);
// now = max(now, minute): the clock never goes back, even when a cutscene sets the hour back.
FTCORE_API void npcAdvance(NpcMemoryWorld& w, std::uint32_t minute);
inline std::uint32_t npcMinute(std::uint32_t day, double hours) {
    return day * 1440u + static_cast<std::uint32_t>(hours * 60.0);
}

// Records a deed at w.now (deed.minute is set to now when 0). The victim and witnesses learn it,
// rumours are queued, expectations kept or broken. Returns the seq (0 if the type is unknown).
FTCORE_API std::uint32_t npcRecordDeed(NpcMemoryWorld& w, const NpcTables& t, NpcDeed deed);
// Delivers due rumours and street hearsay, moves expectations. crowdPermille: crowd density now.
FTCORE_API void npcCheckpoint(NpcMemoryWorld& w, const NpcTables& t, std::uint32_t now, std::uint16_t crowdPermille);
// A secret deed (or any deed) reaches one NPC directly (K reveal); hearsay then flows from them.
FTCORE_API bool npcReveal(NpcMemoryWorld& w, const NpcTables& t, std::uint32_t seq, std::uint16_t npc);
FTCORE_API void npcSetDead(NpcMemoryWorld& w, std::uint16_t npc);

// Reads (lazy decay at w.now; a dead NPC is frozen at its death minute).
FTCORE_API std::int32_t npcAxis(const NpcMemoryWorld& w, const NpcTables& t, std::uint16_t npc, NpcAxis a);
FTCORE_API std::int32_t npcOffset(const NpcMemoryWorld& w, const NpcTables& t, std::uint16_t npc, std::uint8_t lead, NpcAxis a);
// E = clamp(party + offset[lead]); lead kNoLead = party only.
FTCORE_API std::array<std::int32_t, 3> npcEffective(const NpcMemoryWorld& w, const NpcTables& t, std::uint16_t npc, std::uint8_t lead);
FTCORE_API NpcStance npcStance(const NpcMemoryWorld& w, const NpcTables& t, std::uint16_t npc, std::uint8_t lead);
// The F4 rule alone: the next stance from the current one and E.
FTCORE_API NpcStance npcStanceStep(NpcStance current, std::int32_t T, std::int32_t F, std::int32_t R, std::int32_t stayGap = 10);
FTCORE_API NpcConsequence npcConsequence(const NpcMemoryWorld& w, const NpcTables& t, std::uint16_t npc, std::uint8_t lead);
FTCORE_API std::int32_t npcSalience(const NpcMemoryWorld& w, const NpcTables& t, const NpcMemoryEntry& e);
// The NPC's memory of a deed seq, or nullptr.
FTCORE_API const NpcMemoryEntry* npcFindMemory(const NpcMemoryWorld& w, std::uint16_t npc, std::uint32_t seq);
// The strongest memory of a deed type (or of any deed carrying tag bit when tag >= 0), or nullptr.
FTCORE_API const NpcMemoryEntry* npcRecall(const NpcMemoryWorld& w, const NpcTables& t, std::uint16_t npc, int type, int tag);
// The ring slot for seq when it still holds it (nullptr when pruned).
FTCORE_API const NpcDeed* npcDeedAt(const NpcMemoryWorld& w, std::uint32_t seq);

// F1 in isolation: the per-axis Δ' for one learner before it is applied (tests and the debug view).
FTCORE_API std::array<std::int32_t, 3> npcAppraise(const NpcMemoryWorld& w, const NpcTables& t, std::uint16_t npc,
                                                   const NpcDeed& deed, NpcLearn how);

// ---- Lines (ids and conditions only; text lives in private data) ----
enum class NpcCondKind : std::uint8_t { None, Mem, StanceGe, StanceLe, StanceEq, SinceGe, SinceLe, NotSince, Exp, EventLe, Dead };
struct NpcCond {
    NpcCondKind kind = NpcCondKind::None;
    std::int16_t type = -1;  // deed type
    std::int16_t tag = -1;   // tag bit
    std::int16_t other = -1; // NotSince: the redeeming deed type; Exp: exp index; EventLe: event; Dead: npc
    std::int32_t value = 0;  // salience bar, minutes, stance rank or exp state
};
struct NpcLine {
    std::string id;
    std::uint16_t npc = 0;
    NpcLineKind kind = NpcLineKind::Greet;
    std::uint8_t lead = kNoLead;  // kNoLead: any lead
    std::uint8_t priority = 0;    // 0..9
    std::uint32_t cooldownMin = 0;
    bool once = false;
    std::array<NpcCond, kCondMax> cond{};
};
struct NpcLineTable {
    std::vector<NpcLine> lines;  // at most kSaidMax
    FTCORE_API int lineIndex(std::string_view id) const;
};
struct NpcLinePick {
    std::int32_t line = -1;
    std::int32_t score = 0;
};
// Picks the best line and marks it said at w.now. -1 when none qualifies.
FTCORE_API NpcLinePick npcPickLine(NpcMemoryWorld& w, const NpcTables& t, const NpcLineTable& lines, std::uint16_t npc,
                                   std::uint8_t lead, NpcLineKind kind);

// ---- Loading (allocates; once at load) ----
// tags.csv, deeds.csv, npc_values.csv, ties.csv, consequences.csv, expectations.csv, schedule.csv,
// memory_tuning.json. Any empty view is skipped. Header rows are required.
struct NpcTableSources {
    std::string_view tags, deeds, npcValues, ties, consequences, expectations, schedule, tuningJson;
};
FTCORE_API bool importNpcTables(const NpcTableSources& src, NpcTables& out, std::string* error = nullptr);
// npc_lines.csv: line_id,npc_id,kind,lead,priority,cooldown_min,once,cond. Clauses in cond are
// separated by ';' (quote the field when a clause holds a comma).
FTCORE_API bool importNpcLines(std::string_view csv, const NpcTables& t, NpcLineTable& out, std::string* error = nullptr);

// ---- Save (string ids, never indices) ----
struct NpcMemorySaveEntry {
    std::uint32_t seq = 0, deedMinute = 0, ts = 0, learned = 0;
    std::string type;
    std::uint16_t place = 0;
    std::int16_t s0 = 0;
    std::uint8_t hops = 0, count = 0, lead = kNoLead;
    bool pinned = false;
};
struct NpcMemorySaveNpc {
    std::string id;
    std::array<std::int16_t, 3> axes{};
    std::uint32_t axesTs = 0;
    std::array<std::array<std::int8_t, 3>, kLeadCount> offsets{};
    std::array<std::uint32_t, kLeadCount> offsetsTs{};
    std::array<std::uint8_t, kLeadCount + 1> stance{};
    bool dead = false;
    std::uint32_t deadAt = 0;
    std::vector<NpcMemorySaveEntry> mem;
    std::vector<std::pair<std::string, NpcTagAggregate>> aggregates;
};
struct NpcMemorySaveDeed {
    std::uint32_t seq = 0, minute = 0;
    std::string type, victim;
    std::uint16_t place = 0, district = 0;
    std::uint8_t lead = kNoLead, flags = 0;
    std::vector<std::string> witnesses;
};
struct NpcMemorySaveRumour {
    std::uint32_t seq = 0, due = 0;
    std::string target;
    std::uint8_t hops = 1, lead = kNoLead, closeness = 0;
};
struct NpcMemorySave {
    static constexpr int kFormat = 1;
    int format = kFormat;
    std::uint32_t now = 0, nextSeq = 1, droppedRumours = 0;
    std::vector<NpcMemorySaveNpc> npcs;
    std::vector<NpcMemorySaveDeed> deeds;
    std::vector<NpcMemorySaveRumour> rumours;
    std::vector<std::pair<std::string, std::uint8_t>> expectations;
    std::vector<std::pair<std::string, std::uint32_t>> said;  // line id, minute + 1
};
struct NpcMemoryLoadReport {
    std::vector<std::string> unknownIds;  // "npc:x", "deed:x", "tag:x", "exp:x", "line:x"
    std::uint32_t clamped = 0;
    bool formatOk = true;
};
FTCORE_API NpcMemorySave npcCaptureSave(const NpcMemoryWorld& w, const NpcTables& t, const NpcLineTable& lines);
// Rebuilds the world from tables + save; unknown ids are dropped and reported, values clamped.
FTCORE_API NpcMemoryLoadReport npcApplySave(NpcMemoryWorld& w, const NpcTables& t, const NpcLineTable& lines,
                                            const NpcMemorySave& s);

}  // namespace ftcore
