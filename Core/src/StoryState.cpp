#include "ftcore/StoryState.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace ftcore {

namespace {

// ---- Scenes ----

struct SceneBand {
    char prefix;          // '\0' for numeric acts
    std::uint16_t first;  // first number in the band (p01 -> 1, 101 -> 101)
    std::uint8_t count;
    std::uint8_t ordinal; // ordinal of the band's first scene
};
constexpr std::array<SceneBand, 6> kBands{{
    {'p', 1, 8, 0}, {'\0', 101, 17, 8}, {'\0', 201, 16, 25}, {'\0', 301, 15, 41}, {'\0', 401, 9, 56}, {'f', 1, 10, 65},
}};

constexpr std::array<std::string_view, kStoryScenes> kSceneCodes{
    "p01", "p02", "p03", "p04", "p05", "p06", "p07", "p08", "101", "102", "103", "104", "105", "106", "107",
    "108", "109", "110", "111", "112", "113", "114", "115", "116", "117", "201", "202", "203", "204", "205",
    "206", "207", "208", "209", "210", "211", "212", "213", "214", "215", "216", "301", "302", "303", "304",
    "305", "306", "307", "308", "309", "310", "311", "312", "313", "314", "315", "401", "402", "403", "404",
    "405", "406", "407", "408", "409", "f01", "f02", "f03", "f04", "f05", "f06", "f07", "f08", "f09", "f10",
};

constexpr std::array<std::string_view, static_cast<std::size_t>(StoryFact::Count)> kFactIds{
    "",      "p03_1", "p07_1", "p03_2", "p03_3", "p04_1", "p04_2", "p02_1", "p04_3", "p04_4", "p06_1",
    "p06_2", "p05_1", "p07_2", "p07_3", "p07_4", "104_1", "105_1", "105_2", "106_1", "106_2", "106_3",
    "107_1", "108_1", "109_1", "110_1", "110_2", "111_1", "111_2", "112_1", "112_2", "112_3", "113_1",
    "114_1", "114_2", "115_1", "115_2", "116_1", "116_2", "115_3", "203_1", "203_2", "203_3", "205_1",
    "206_1", "207_1", "207_2", "211_1", "213_1", "214_1", "215_1", "215_2", "216_1", "302_1", "303_1",
    "303_2", "304_1", "305_1", "305_2", "305_3", "306_1", "307_1", "310_1", "313_1", "314_1", "315_1",
    "315_2", "404_1", "405_1", "406_1", "406_2", "407_1", "407_2", "408_1", "409_1", "f05_1", "f07_1",
    "f07_2", "f09_1", "f10_1", "g_hl0", "g_hl1",
};

constexpr std::array<std::string_view, static_cast<std::size_t>(StoryWide::Count)> kWideIds{
    "c_help_s0_k0", "c_help_s0_k1", "c_help_s0_k2", "c_help_s1_k0", "c_help_s1_k1", "c_help_s1_k2",
    "c_help_scene", "c_115_help",   "c_milestone",      "c_hold_done",  "c_hold_slips", "c_203_found",
    "c_303_declines", "c_retries",  "c_help_all",   "m_108",
};

constexpr std::uint16_t slotOf(StoryFact f) { return static_cast<std::uint16_t>(f); }
constexpr std::uint8_t slotOf(StoryWide w) { return static_cast<std::uint8_t>(w); }
std::uint8_t scene(std::string_view code) { return storySceneOrdinal(code); }

// ---- Fact values ----

std::uint8_t choiceCount(FactKind k) {
    switch (k) {
    case FactKind::Choice2: return 2;
    case FactKind::Choice3: return 3;
    case FactKind::Choice4: return 4;
    default: return 0;
    }
}
bool canBeNotOffered(FactKind k) { return k == FactKind::Optional || choiceCount(k) != 0; }

// The packed fields of a mask: pairs of bits from bit 0 while both are legal.
std::uint8_t packedFields(std::uint8_t mask) {
    std::uint8_t n = 0;
    while (n < 4 && ((mask >> (2 * n)) & 3u) == 3u) ++n;
    return n;
}
std::uint8_t fieldOf(std::uint8_t v, std::uint8_t f) { return static_cast<std::uint8_t>((v >> (2 * f)) & 3u); }
std::uint8_t flagBits(std::uint8_t v, std::uint8_t mask) {
    const std::uint8_t fields = packedFields(mask);
    const std::uint8_t fieldMask = static_cast<std::uint8_t>(fields >= 4 ? 0xFF : (1u << (2 * fields)) - 1u);
    return static_cast<std::uint8_t>(v & mask & ~fieldMask);
}

// A value this fact may hold (0 and 255 aside).
bool inRange(const FactRow& r, std::uint8_t v) {
    switch (r.kind) {
    case FactKind::Event: return v == 1;
    case FactKind::Optional: return v == 1 || v == 2;
    case FactKind::Choice2:
    case FactKind::Choice3:
    case FactKind::Choice4: return v >= 1 && v <= choiceCount(r.kind);
    case FactKind::Number:
    case FactKind::Stepped: return v <= r.max;
    case FactKind::Bits: return (v & ~r.max) == 0;
    case FactKind::Packed: {
        if ((v & ~r.max) != 0) return false;
        for (std::uint8_t f = 0; f < packedFields(r.max); ++f)
            if (fieldOf(v, f) == 3) return false;
        return true;
    }
    case FactKind::Retired: return false;
    }
    return false;
}

bool condHolds(const StoryState& s, const FactCond& c) {
    if (c.fact == 0) return true;
    const std::uint8_t v = s.facts[c.fact];
    return c.field == 0xFF ? v == c.value : fieldOf(v, c.field) == c.value;
}

bool committed(const StoryState& s, std::uint8_t sc) {
    return sc != kNoScene && s.lastCommitted != kNoScene && s.lastCommitted >= sc;
}
bool reached(const StoryState& s, std::uint8_t sc) { return committed(s, sc) || (sc != kNoScene && s.openScene == sc); }

// ---- Events ----

void pushEvent(StoryState& s, StoryEvent::Kind kind, std::uint16_t id, std::uint32_t before, std::uint32_t after) {
    StoryEvent e;
    e.kind = kind;
    e.id = id;
    e.before = before;
    e.after = after;
    e.scene = s.openScene;
    if (s.eventCount == kStoryEventRing) {
        s.eventHead = static_cast<std::uint8_t>((s.eventHead + 1) % kStoryEventRing);
        --s.eventCount;
        ++s.lostEvents;
    }
    s.events[(s.eventHead + s.eventCount) % kStoryEventRing] = e;
    ++s.eventCount;
}

SetResult reject(StoryState& s, std::uint16_t id, SetResult why, std::uint32_t tried) {
    pushEvent(s, StoryEvent::Kind::Rejected, id, static_cast<std::uint32_t>(why), tried);
    return why;
}

void writeFact(StoryState& s, std::uint16_t slot, std::uint8_t v) {
    const std::uint8_t before = s.facts[slot];
    if (before == v) return;
    s.facts[slot] = v;
    s.stamp[slot] = s.openScene;
    pushEvent(s, StoryEvent::Kind::FactSet, slot, before, v);
}

// Counter add with events; returns whether the value changed.
bool addWide(StoryState& s, std::uint8_t slot, std::uint32_t n, std::uint32_t cap) {
    const std::uint32_t before = s.wide[slot];
    const std::uint32_t after = storySaturatingAdd(before, n, cap);
    if (after == before) return false;
    s.wide[slot] = after;
    pushEvent(s, StoryEvent::Kind::WideChanged, slot, before, after);
    if (after == cap) pushEvent(s, StoryEvent::Kind::Saturated, slot, before, after);
    return true;
}

void setWide(StoryState& s, std::uint8_t slot, std::uint32_t v) {
    const std::uint32_t before = s.wide[slot];
    if (before == v) return;
    s.wide[slot] = v;
    pushEvent(s, StoryEvent::Kind::WideChanged, slot, before, v);
}

std::uint32_t sideTotal(const std::array<std::uint32_t, kWideSlots>& w, std::uint8_t side) {
    const std::uint8_t base = side == 0 ? slotOf(StoryWide::c_help_s0_k0) : slotOf(StoryWide::c_help_s1_k0);
    return w[base] + w[base + 1u] + w[base + 2u]; // each at most 65 535: no wrap
}

// ---- Little-endian bytes ----

void put16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

constexpr std::array<std::uint32_t, 256> makeCrcTable() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
    return t;
}
constexpr std::array<std::uint32_t, 256> kCrcTable = makeCrcTable();

std::uint32_t crcUpdate(std::uint32_t c, const std::uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) c = kCrcTable[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c;
}
std::uint32_t blockCrc(const std::uint8_t* b) {
    std::uint32_t c = 0xFFFFFFFFu;
    c = crcUpdate(c, b, 28);
    c = crcUpdate(c, b + 32, kStoryStateBlockSize - 32);
    return c ^ 0xFFFFFFFFu;
}

constexpr std::uint16_t kTagStampEntry = 1;

} // namespace

// ---- Scenes and ids ----

std::uint8_t storySceneOrdinal(std::string_view code) {
    if (code.size() != 3) return kNoScene;
    char prefix = '\0';
    std::string_view digits = code;
    if (code[0] == 'p' || code[0] == 'f') {
        prefix = code[0];
        digits = code.substr(1);
    }
    std::uint16_t n = 0;
    for (char c : digits) {
        if (c < '0' || c > '9') return kNoScene;
        n = static_cast<std::uint16_t>(n * 10 + (c - '0'));
    }
    for (const SceneBand& b : kBands)
        if (b.prefix == prefix && n >= b.first && n < b.first + b.count)
            return static_cast<std::uint8_t>(b.ordinal + (n - b.first));
    return kNoScene;
}

std::string_view storySceneCode(std::uint8_t ordinal) { return ordinal < kStoryScenes ? kSceneCodes[ordinal] : ""; }

std::string_view storyFactId(StoryFact fact) {
    const auto i = static_cast<std::size_t>(fact);
    return i < kFactIds.size() ? kFactIds[i] : "";
}
std::string_view storyWideId(StoryWide wide) {
    const auto i = static_cast<std::size_t>(wide);
    return i < kWideIds.size() ? kWideIds[i] : "";
}

// ---- Formulas ----

std::uint32_t storySaturatingAdd(std::uint32_t c, std::uint32_t n, std::uint32_t cap) {
    const std::uint64_t sum = static_cast<std::uint64_t>(c) + n;
    return sum >= cap ? cap : static_cast<std::uint32_t>(sum);
}

std::uint32_t storyHelpShown(const StoryState& s) {
    return std::max<std::uint32_t>(1, sideTotal(s.wide, 0)) + std::max<std::uint32_t>(1, sideTotal(s.wide, 1));
}

std::uint8_t storyLeadKind(const StoryState& s) {
    const std::uint8_t base = slotOf(StoryWide::c_help_s0_k0);
    std::uint32_t best = 0;
    for (std::uint8_t k = 0; k < 3; ++k) best = std::max(best, s.wide[base + k]);
    const std::uint8_t last = s.facts[slotOf(StoryFact::sg_hl0)];
    if (last >= 1 && last <= 3 && s.wide[base + last - 1u] == best) return static_cast<std::uint8_t>(last - 1u);
    for (std::uint8_t k = 0; k < 3; ++k)
        if (s.wide[base + k] == best) return k;
    return 0;
}

std::uint32_t storyCrc32(std::span<const std::uint8_t> bytes, std::uint32_t crc) {
    return crcUpdate(crc ^ 0xFFFFFFFFu, bytes.data(), bytes.size()) ^ 0xFFFFFFFFu;
}

// ---- Get and set ----

void storyReset(StoryState& s) { s = StoryState{}; }

FactRead storyGetSlot(const StoryState& s, const FactRegistry& reg, std::uint16_t slot) {
    FactRead r;
    r.fact = slot;
    if (slot == 0 || slot >= kFactSlots || !reg.facts[slot].used || reg.facts[slot].kind == FactKind::Retired)
        return r; // Unknown
    const FactRow& row = reg.facts[slot];
    const std::uint8_t v = s.facts[slot];
    if (v == 0) {
        r.value = row.def;
        r.status = FactStatus::Unset;
    } else if (v == kNotOffered && canBeNotOffered(row.kind)) {
        r.value = kNotOffered;
        r.status = FactStatus::NotOffered;
    } else if (!inRange(row, v)) {
        r.value = row.def;
        r.status = FactStatus::Stale;
    } else {
        r.value = v;
        r.status = FactStatus::Set;
    }
    return r;
}

FactRead storyGet(const StoryState& s, const FactRegistry& reg, StoryFact fact) {
    return storyGetSlot(s, reg, slotOf(fact));
}

SetResult storySet(StoryState& s, const FactRegistry& reg, StoryFact fact, std::uint8_t value) {
    const std::uint16_t slot = slotOf(fact);
    if (slot == 0 || slot >= kFactSlots || !reg.facts[slot].used || reg.facts[slot].kind == FactKind::Retired)
        return reject(s, slot, SetResult::UnknownFact, value);
    const FactRow& row = reg.facts[slot];
    if (s.openScene == kNoScene) return reject(s, slot, SetResult::RejectedNoScene, value);
    if (!row.setAny && !row.setScenes.has(s.openScene)) return reject(s, slot, SetResult::RejectedWrongScene, value);
    if (value == 0 || value == kNotOffered || !inRange(row, value))
        return reject(s, slot, SetResult::RejectedRange, value);
    if (!condHolds(s, row.gate)) return reject(s, slot, SetResult::RejectedGate, value);
    if (choiceCount(row.kind) != 0 && !condHolds(s, row.optionGates[value - 1u]))
        return reject(s, slot, SetResult::RejectedGate, value);
    const std::uint8_t old = s.facts[slot];
    if (old == value) return SetResult::Unchanged;

    if (row.kind == FactKind::Packed) {
        const bool stale = old != 0 && !inRange(row, old);
        for (std::uint8_t f = 0; f < packedFields(row.max) && !stale; ++f) {
            const std::uint8_t a = fieldOf(old, f);
            const std::uint8_t b = fieldOf(value, f);
            if (row.mut == FactMut::Once && a != 0 && b != a) return reject(s, slot, SetResult::RejectedOnce, value);
            if (row.mut == FactMut::Mono && b < a) return reject(s, slot, SetResult::RejectedDownward, value);
        }
        const std::uint8_t oldFlags = flagBits(old, row.max);
        if (!stale && row.mut != FactMut::Free && (flagBits(value, row.max) & oldFlags) != oldFlags)
            return reject(s, slot, row.mut == FactMut::Once ? SetResult::RejectedOnce : SetResult::RejectedDownward,
                          value);
    } else if (row.mut == FactMut::Once && old != 0) {
        return reject(s, slot, SetResult::RejectedOnce, value);
    } else if (row.mut == FactMut::Mono && old != 0 && inRange(row, old)) {
        const bool down = row.kind == FactKind::Bits ? (value & old) != old : value < old;
        if (down) return reject(s, slot, SetResult::RejectedDownward, value);
    } else if (row.mut == FactMut::Mono && old == 0 && row.kind != FactKind::Bits && value < row.def) {
        return reject(s, slot, SetResult::RejectedDownward, value);
    }
    writeFact(s, slot, value);
    return SetResult::Ok;
}

SetResult storySetBits(StoryState& s, const FactRegistry& reg, StoryFact fact, std::uint8_t bits) {
    const std::uint16_t slot = slotOf(fact);
    if (slot == 0 || slot >= kFactSlots || !reg.facts[slot].used) return storySet(s, reg, fact, bits);
    const std::uint8_t old = s.facts[slot] == kNotOffered ? 0 : s.facts[slot];
    return storySet(s, reg, fact, static_cast<std::uint8_t>(old | bits));
}

std::uint32_t storyWide(const StoryState& s, StoryWide wide) {
    const std::uint8_t w = slotOf(wide);
    return w < kWideSlots ? s.wide[w] : 0;
}

SetResult storyAdd(StoryState& s, const FactRegistry& reg, StoryWide wide, std::uint32_t n) {
    const std::uint8_t w = slotOf(wide);
    const std::uint16_t id = static_cast<std::uint16_t>(kStoryWideEventBit | w);
    if (w >= kWideSlots || reg.wide[w].kind == WideKind::None) return reject(s, id, SetResult::UnknownFact, n);
    const WideRow& row = reg.wide[w];
    if (s.openScene == kNoScene) return reject(s, id, SetResult::RejectedNoScene, n);
    if (!row.setAny && !row.setScenes.has(s.openScene)) return reject(s, id, SetResult::RejectedWrongScene, n);
    if (row.kind == WideKind::Mask) {
        if ((n & ~row.cap) != 0) return reject(s, id, SetResult::RejectedRange, n);
        if ((s.wide[w] | n) == s.wide[w]) return SetResult::Unchanged;
        setWide(s, w, s.wide[w] | n);
        return SetResult::Ok;
    }
    return addWide(s, w, n, row.cap) ? SetResult::Ok : SetResult::Unchanged;
}

SetResult storyCountHelp(StoryState& s, const FactRegistry& reg, std::uint8_t side, std::uint8_t kind) {
    const std::uint16_t id = static_cast<std::uint16_t>(kStoryWideEventBit | (side * 3u + kind));
    if (side > 1 || kind > 2) return reject(s, id, SetResult::RejectedRange, kind);
    const std::uint8_t counter = static_cast<std::uint8_t>(slotOf(StoryWide::c_help_s0_k0) + side * 3u + kind);
    const std::uint16_t last = side == 0 ? slotOf(StoryFact::sg_hl0) : slotOf(StoryFact::sg_hl1);
    const std::uint16_t first = slotOf(StoryFact::sp03_3);
    const std::uint8_t sceneW = slotOf(StoryWide::c_help_scene);
    const std::uint8_t allW = slotOf(StoryWide::c_help_all);
    if (reg.wide[counter].kind == WideKind::None || reg.wide[sceneW].kind == WideKind::None ||
        reg.wide[allW].kind == WideKind::None || !reg.facts[last].used || !reg.facts[first].used)
        return reject(s, id, SetResult::UnknownFact, kind);
    if (s.openScene == kNoScene) return reject(s, id, SetResult::RejectedNoScene, kind);
    if (side == 1 && s.facts[slotOf(StoryFact::sp07_1)] == 0) return reject(s, id, SetResult::RejectedGate, kind);
    const bool rose = addWide(s, counter, 1, reg.wide[counter].cap);
    addWide(s, sceneW, 1, reg.wide[sceneW].cap);
    addWide(s, allW, 1, reg.wide[allW].cap);
    writeFact(s, last, static_cast<std::uint8_t>(kind + 1u));
    if (side == 0 && s.facts[first] == 0) writeFact(s, first, 1);
    return rose ? SetResult::Ok : SetResult::Unchanged;
}

// ---- Scene reads ----

SceneReads storySceneReads(const StoryState& s, const FactRegistry& reg, std::uint8_t sc) {
    SceneReads out;
    out.scene = sc;
    if (sc >= kStoryScenes) return out;
    out.complete = true;
    for (std::uint16_t slot = 1; slot < kFactSlots; ++slot) {
        const FactRow& row = reg.facts[slot];
        if (!row.used || row.kind == FactKind::Retired || !(row.readAll || row.reads.has(sc))) continue;
        if (out.count == kMaxReadsPerScene) {
            out.complete = false;
            break;
        }
        out.facts[out.count++] = storyGetSlot(s, reg, slot);
    }
    for (std::uint8_t w = 0; w < kWideSlots; ++w) {
        if (reg.wide[w].kind == WideKind::None || !reg.wide[w].reads.has(sc)) continue;
        if (out.wideCount == kMaxWideReadsPerScene) {
            out.complete = false;
            break;
        }
        out.wide[out.wideCount++] = WideRead{w, s.wide[w]};
    }

    DerivedValues& d = out.derived;
    const FactRead a = storyGet(s, reg, StoryFact::sp03_2);
    if (a.status == FactStatus::Set) {
        d.d1 = a.value == 2 || a.value == 3;
        d.valid |= 1u << 0;
    }
    const FactRead s0 = storyGet(s, reg, StoryFact::sp03_1);
    const FactRead s1 = storyGet(s, reg, StoryFact::sp07_1);
    if (s0.status != FactStatus::Unknown && s1.status != FactStatus::Unknown) {
        d.d2[0] = s0.value;
        d.d2[1] = s1.value;
        d.valid |= 1u << 1;
    }
    const FactRead f116 = storyGet(s, reg, StoryFact::s116_1);
    const FactRead f315 = storyGet(s, reg, StoryFact::s315_1);
    if (f116.status == FactStatus::Unset || f116.status == FactStatus::Set) {
        if (f116.status == FactStatus::Unset) d.d3 = 0;
        else if (f116.value == 2) d.d3 = 2;
        else d.d3 = f315.status == FactStatus::Set && f315.value == 2 ? 3 : 1;
        d.valid |= 1u << 2;
    }
    const FactRead choice = storyGet(s, reg, StoryFact::sf07_1);
    if (choice.status == FactStatus::Set) {
        d.d4 = choice.value == 1 ? 1 : 0;
        d.valid |= 1u << 3;
    }
    const FactRead f113 = storyGet(s, reg, StoryFact::s113_1);
    if (f113.status == FactStatus::Set) {
        d.d5 = f113.value;
        d.valid |= 1u << 4;
    }
    d.d6 = storyLeadKind(s);
    d.d7 = storyHelpShown(s);
    d.valid |= (1u << 5) | (1u << 6);
    return out;
}

// ---- Scenes ----

bool storyBeginScene(StoryState& s, std::uint8_t sc) {
    if (s.openScene != kNoScene || sc >= kStoryScenes) return false;
    if (s.lastCommitted != kNoScene && sc <= s.lastCommitted) return false;
    s.wide[slotOf(StoryWide::c_help_scene)] = 0;
    s.factsEntry = s.facts;
    s.stampEntry = s.stamp;
    s.wideEntry = s.wide;
    s.openScene = sc;
    pushEvent(s, StoryEvent::Kind::SceneBegun, sc, 0, 0);
    return true;
}

bool storyRestartScene(StoryState& s, const FactRegistry& reg) {
    if (s.openScene == kNoScene) return false;
    s.facts = s.factsEntry;
    s.stamp = s.stampEntry;
    for (std::uint8_t w = 0; w < kWideSlots; ++w)
        if (reg.wide[w].kind != WideKind::Raw) s.wide[w] = s.wideEntry[w];
    const std::uint8_t retries = slotOf(StoryWide::c_retries);
    if (reg.wide[retries].kind != WideKind::None) addWide(s, retries, 1, reg.wide[retries].cap);
    pushEvent(s, StoryEvent::Kind::SceneRolledBack, s.openScene, 0, 0);
    return true;
}

bool storyCommitScene(StoryState& s, const FactRegistry& reg) {
    const std::uint8_t sc = s.openScene;
    if (sc == kNoScene) return false;
    for (std::uint16_t slot = 1; slot < kFactSlots; ++slot) {
        const FactRow& row = reg.facts[slot];
        if (!row.used || row.setAny || !row.setScenes.has(sc) || s.facts[slot] != 0) continue;
        const bool gateFalse = row.gate.fact != 0 && !condHolds(s, row.gate);
        if (choiceCount(row.kind) != 0 && gateFalse) writeFact(s, slot, kNotOffered);
        else if (row.kind == FactKind::Optional) writeFact(s, slot, gateFalse ? kNotOffered : 1);
    }
    const StoryTuning& t = reg.tuning;
    const std::uint8_t mile = slotOf(StoryWide::c_milestone);
    for (std::uint8_t i = 0; i < t.milestoneCount && i < t.milestoneScene.size(); ++i)
        if (t.milestoneScene[i] == sc && s.wide[mile] < t.milestoneValue[i]) setWide(s, mile, t.milestoneValue[i]);
    if (sc == t.trackScene) {
        const std::uint32_t pressed = sideTotal(s.wide, 1) - std::min(sideTotal(s.wide, 1), sideTotal(s.wideEntry, 1));
        setWide(s, slotOf(StoryWide::c_115_help), std::min<std::uint32_t>(pressed, t.trackLength));
    }
    s.lastCommitted = sc;
    s.openScene = kNoScene;
    s.factsEntry = s.facts;
    s.stampEntry = s.stamp;
    s.wideEntry = s.wide;
    pushEvent(s, StoryEvent::Kind::SceneCommitted, sc, 0, 0);
    return true;
}

void storyMakeSandbox(const StoryState& real, const FactRegistry& reg, std::uint8_t sc, StoryState& out) {
    out = real;
    out.flags = static_cast<std::uint16_t>(out.flags | kStorySandbox);
    out.openScene = kNoScene;
    out.lastCommitted = sc == 0 || sc >= kStoryScenes ? kNoScene : static_cast<std::uint8_t>(sc - 1u);
    // A once fact written in this scene or later goes back to unset (its whole value came from that one
    // write). Facts written in several scenes and the counters keep their values: the stamp names only
    // the last writer, so an earlier part cannot be told apart.
    for (std::size_t slot = 1; slot < kFactSlots; ++slot) {
        if (out.stamp[slot] == kNoScene || out.stamp[slot] < sc) continue;
        if (!reg.facts[slot].used || reg.facts[slot].mut != FactMut::Once) continue;
        out.facts[slot] = 0;
        out.stamp[slot] = kNoScene;
    }
    out.factsEntry = out.facts;
    out.stampEntry = out.stamp;
    out.wideEntry = out.wide;
}

std::uint32_t storyDrainEvents(StoryState& s, std::span<StoryEvent> out) {
    std::uint32_t n = 0;
    while (s.eventCount > 0 && n < out.size()) {
        out[n++] = s.events[s.eventHead];
        s.eventHead = static_cast<std::uint8_t>((s.eventHead + 1) % kStoryEventRing);
        --s.eventCount;
    }
    return n;
}

// ---- Validation ----

namespace {

struct IssueSink {
    std::span<StoryIssue> out;
    std::uint32_t n = 0;
    void add(std::uint8_t rule, std::uint16_t a, std::uint16_t b = 0, IssueSeverity sev = IssueSeverity::Error) {
        if (n < out.size() && n < kMaxStoryIssues) out[n++] = StoryIssue{rule, a, b, sev};
    }
};

} // namespace

std::uint32_t storyValidate(const StoryState& s, const FactRegistry& reg, std::span<StoryIssue> out) {
    IssueSink sink{out};
    auto value = [&](StoryFact f) { return s.facts[slotOf(f)]; };
    auto setV = [&](StoryFact f) {
        const std::uint8_t v = value(f);
        return v != 0 && v != kNotOffered;
    };
    const std::uint16_t W = kStoryWideEventBit;

    for (std::uint16_t slot = 1; slot < kFactSlots; ++slot) {
        const FactRow& row = reg.facts[slot];
        if (!row.used || row.kind == FactKind::Retired) continue;
        const std::uint8_t v = s.facts[slot];
        const bool isSet = v != 0 && v != kNotOffered && inRange(row, v);
        // V01 / V02: the whole-fact gate.
        if (row.gate.fact != 0) {
            const bool gate = condHolds(s, row.gate);
            if (isSet && !gate) sink.add(1, slot, row.gate.fact);
            if (v == kNotOffered && gate && committed(s, row.firstSet)) sink.add(2, slot, row.gate.fact);
        }
        // V06 / V07: an option offered only under a condition.
        if (isSet && choiceCount(row.kind) != 0 && !condHolds(s, row.optionGates[v - 1u]))
            sink.add(v == 4 ? 6 : 7, slot, row.optionGates[v - 1u].fact);
        // V03 / V04: a past scene left a choice, an optional act or an event at 0.
        if (!row.setAny && v == 0 && committed(s, row.firstSet)) {
            if (canBeNotOffered(row.kind)) sink.add(3, slot);
            else if (row.kind == FactKind::Event) sink.add(4, slot);
        }
        // V05: written for a scene not reached yet.
        if (!row.setAny && row.firstSet != kNoScene && !reached(s, row.firstSet) && v != 0) sink.add(5, slot);
        const std::uint8_t st = s.stamp[slot];
        if (st != kNoScene && st != s.openScene && !committed(s, st)) sink.add(5, slot);
        // V14: a packed field holds 3.
        if (row.kind == FactKind::Packed && v != 0)
            for (std::uint8_t f = 0; f < packedFields(row.max); ++f)
                if (fieldOf(v, f) == 3) sink.add(14, slot);
        // V19: a value outside the range.
        if (v != 0 && !(v == kNotOffered && canBeNotOffered(row.kind)) && !inRange(row, v))
            sink.add(19, slot, 0, IssueSeverity::Warning);
    }
    for (std::uint16_t slot = reg.factsKnown + 1u; slot < kFactSlots; ++slot) { // V05 for unknown slots too
        const std::uint8_t st = s.stamp[slot];
        if (st != kNoScene && st != s.openScene && !committed(s, st)) sink.add(5, slot);
    }

    // V08: the milestone count against its milestones.
    const std::uint32_t mile = s.wide[slotOf(StoryWide::c_milestone)];
    const StoryTuning& t = reg.tuning;
    for (std::uint8_t i = 0; i < t.milestoneCount && i < t.milestoneScene.size(); ++i) {
        if (committed(s, t.milestoneScene[i]) && mile < t.milestoneValue[i]) sink.add(8, W | slotOf(StoryWide::c_milestone));
        if (!committed(s, t.milestoneScene[i]) && mile >= t.milestoneValue[i] && i > 0 &&
            t.milestoneValue[i] > t.milestoneValue[i - 1u])
            sink.add(8, W | slotOf(StoryWide::c_milestone));
    }
    std::uint32_t at310 = 0; // the milestone 310 sets, from the tuning (none: no check)
    for (std::uint8_t i = 0; i < t.milestoneCount && i < t.milestoneScene.size(); ++i)
        if (t.milestoneScene[i] == scene("310")) at310 = t.milestoneValue[i];
    if (at310 > 0 && mile >= at310 && !setV(StoryFact::s310_1)) sink.add(8, W | slotOf(StoryWide::c_milestone), slotOf(StoryFact::s310_1));
    // V09: p07_3 field 2 at 2 and 310_1 go together.
    const bool mark310 = fieldOf(value(StoryFact::sp07_3), 2) == 2;
    if (mark310 && !setV(StoryFact::s310_1)) sink.add(9, slotOf(StoryFact::sp07_3), slotOf(StoryFact::s310_1));
    if (committed(s, scene("310")) && setV(StoryFact::s310_1) && !mark310)
        sink.add(9, slotOf(StoryFact::sp07_3), slotOf(StoryFact::s310_1));
    // V10: presses need their slot.
    if (value(StoryFact::sp03_3) == 0 && sideTotal(s.wide, 0) > 0)
        sink.add(10, slotOf(StoryFact::sp03_3), W | slotOf(StoryWide::c_help_s0_k0));
    if (value(StoryFact::sp07_1) == 0 && sideTotal(s.wide, 1) > 0)
        sink.add(10, slotOf(StoryFact::sp07_1), W | slotOf(StoryWide::c_help_s1_k0));
    // V11: c_115_help is at most side 1's presses and the track length.
    const std::uint32_t mark = s.wide[slotOf(StoryWide::c_115_help)];
    if (mark > sideTotal(s.wide, 1) || mark > t.trackLength) sink.add(11, W | slotOf(StoryWide::c_115_help));
    // V12: p05_1 bits come in order (bit 2 needs 112_2, bit 3 needs 115_1).
    const std::uint8_t bits05 = value(StoryFact::sp05_1);
    for (std::uint8_t b = 1; b < 4; ++b)
        if ((bits05 >> b) & 1u && !((bits05 >> (b - 1u)) & 1u)) sink.add(12, slotOf(StoryFact::sp05_1));
    if ((bits05 & 4u) && !setV(StoryFact::s112_2)) sink.add(12, slotOf(StoryFact::sp05_1), slotOf(StoryFact::s112_2));
    if ((bits05 & 8u) && !setV(StoryFact::s115_1)) sink.add(12, slotOf(StoryFact::sp05_1), slotOf(StoryFact::s115_1));
    // V13: f07_2, f09_1 and f10_1 need f07_1.
    if (!setV(StoryFact::sf07_1)) {
        if (setV(StoryFact::sf07_2)) sink.add(13, slotOf(StoryFact::sf07_2), slotOf(StoryFact::sf07_1));
        if (value(StoryFact::sf09_1) != 0) sink.add(13, slotOf(StoryFact::sf09_1), slotOf(StoryFact::sf07_1));
        if (value(StoryFact::sf10_1) == 1) sink.add(13, slotOf(StoryFact::sf10_1), slotOf(StoryFact::sf07_1));
    }
    // V15: the last kind pressed matches the counts.
    for (std::uint8_t side = 0; side < 2; ++side) {
        const std::uint8_t last = value(side == 0 ? StoryFact::sg_hl0 : StoryFact::sg_hl1);
        const std::uint16_t lastSlot = side == 0 ? slotOf(StoryFact::sg_hl0) : slotOf(StoryFact::sg_hl1);
        const std::uint8_t base = static_cast<std::uint8_t>(slotOf(StoryWide::c_help_s0_k0) + side * 3u);
        if (last == 0 && sideTotal(s.wide, side) > 0) sink.add(15, lastSlot);
        if (last >= 1 && last <= 3 && s.wide[base + last - 1u] == 0) sink.add(15, lastSlot);
    }
    // V16: scene counters before their scene.
    if (s.wide[slotOf(StoryWide::c_303_declines)] > 0 && !reached(s, scene("303")))
        sink.add(16, W | slotOf(StoryWide::c_303_declines));
    if (s.wide[slotOf(StoryWide::c_203_found)] > 0 && !reached(s, scene("203")))
        sink.add(16, W | slotOf(StoryWide::c_203_found));
    // V17: the scene markers.
    if (s.openScene != kNoScene && s.lastCommitted != kNoScene && s.openScene <= s.lastCommitted) sink.add(17, 0);
    if (s.openScene == kNoScene && s.factsEntry != s.facts) sink.add(17, 0);
    // V18: 112_1 rises only with its later set scenes.
    const std::uint8_t rank = value(StoryFact::s112_1);
    if ((rank >= 2 && !reached(s, scene("308"))) || (rank >= 3 && !reached(s, scene("312"))))
        sink.add(18, slotOf(StoryFact::s112_1));
    // V20: written by a different table that is not newer.
    if (s.registryHash != 0 && s.registryHash != reg.hash && s.factsKnown <= reg.factsKnown)
        sink.add(20, 0, 0, IssueSeverity::Warning);
    return sink.n;
}

// ---- The block ----

bool storyEncode(StoryState& s, const FactRegistry& reg, std::span<std::uint8_t, kStoryStateBlockSize> out) {
    if ((s.flags & kStorySandbox) != 0) return false;
    ++s.saveSerial;
    s.factsKnown = std::max(s.factsKnown, reg.factsKnown);
    s.wideKnown = std::max(s.wideKnown, reg.wideKnown);
    s.registryHash = reg.hash;
    std::uint8_t* b = out.data();
    std::fill(b, b + kStoryStateBlockSize, std::uint8_t{0});
    b[0] = 0x46;
    b[1] = 0x54;
    b[2] = 0x53;
    b[3] = 0x53;
    put16(b + 4, kStoryLayoutVersion);
    put16(b + 6, static_cast<std::uint16_t>(kStoryStateBlockSize));
    put16(b + 8, s.factsKnown);
    put16(b + 10, s.wideKnown);
    put32(b + 12, s.registryHash);
    b[16] = s.openScene;
    b[17] = s.lastCommitted;
    const std::uint16_t flags = s.openScene != kNoScene ? kStoryMidScene : 0;
    put16(b + 18, flags);
    put32(b + 20, s.saveSerial);
    for (std::uint32_t i = 0; i < kFactSlots; ++i) {
        b[32 + i] = s.facts[i];
        b[288 + i] = s.factsEntry[i];
        b[544 + i] = s.stamp[i];
    }
    for (std::uint32_t i = 0; i < kWideSlots; ++i) {
        put32(b + 800 + 4 * i, s.wide[i]);
        put32(b + 1056 + 4 * i, s.wideEntry[i]);
    }
    std::uint8_t* e = b + kStoryExtensionOffset;
    if (s.extensionRaw) {
        std::copy(s.extension.begin(), s.extension.end(), e);
    } else {
        std::uint32_t at = std::min<std::uint32_t>(s.extensionLength, kStoryExtensionBytes);
        std::copy(s.extension.begin(), s.extension.begin() + at, e);
        if (s.openScene != kNoScene && at + 4 + kFactSlots + 2 <= kStoryExtensionBytes) {
            put16(e + at, kTagStampEntry);
            put16(e + at + 2, static_cast<std::uint16_t>(kFactSlots));
            for (std::uint32_t i = 0; i < kFactSlots; ++i) e[at + 4 + i] = s.stampEntry[i];
            at += 4 + kFactSlots;
        }
        // The end tag (0) is already there: the area was zeroed.
    }
    put32(b + 28, blockCrc(b));
    return true;
}

StoryLoad storyDecode(std::span<const std::uint8_t> in, const FactRegistry& reg, StoryState& out,
                      StoryStateLoadReport* report) {
    StoryStateLoadReport rep;
    if (in.empty()) return StoryLoad::Empty;
    if (in.size() < 8) return StoryLoad::BadSize;
    const std::uint8_t* b = in.data();
    if (b[0] != 0x46 || b[1] != 0x54 || b[2] != 0x53 || b[3] != 0x53) return StoryLoad::BadMagic;
    if (get16(b + 4) > kStoryLayoutVersion || get16(b + 4) == 0) return StoryLoad::NewerLayout;
    if (in.size() != kStoryStateBlockSize || get16(b + 6) != kStoryStateBlockSize) return StoryLoad::BadSize;
    if (get32(b + 28) != blockCrc(b)) return StoryLoad::BadChecksum;
    for (const std::uint8_t scene : {b[16], b[17]})
        if (scene != kNoScene && scene >= kStoryScenes) return StoryLoad::BadScene;

    StoryState s;
    s.factsKnown = get16(b + 8);
    s.wideKnown = get16(b + 10);
    const std::uint16_t savedFactsKnown = s.factsKnown;
    s.registryHash = get32(b + 12);
    s.openScene = b[16];
    s.lastCommitted = b[17];
    s.flags = 0;
    s.saveSerial = get32(b + 20);
    for (std::uint32_t i = 0; i < kFactSlots; ++i) {
        s.facts[i] = b[32 + i];
        s.factsEntry[i] = b[288 + i];
        s.stamp[i] = b[544 + i];
    }
    s.stampEntry = s.stamp;
    for (std::uint32_t i = 0; i < kWideSlots; ++i) {
        s.wide[i] = get32(b + 800 + 4 * i);
        s.wideEntry[i] = get32(b + 1056 + 4 * i);
    }
    // The extension records.
    const std::uint8_t* e = b + kStoryExtensionOffset;
    std::uint32_t at = 0;
    bool malformed = false;
    while (at + 2 <= kStoryExtensionBytes) {
        const std::uint16_t tag = get16(e + at);
        if (tag == 0) break;
        if (at + 4 > kStoryExtensionBytes) {
            malformed = true;
            break;
        }
        const std::uint16_t len = get16(e + at + 2);
        if (at + 4u + len > kStoryExtensionBytes) {
            malformed = true;
            break;
        }
        if (tag == kTagStampEntry && len == kFactSlots) {
            for (std::uint32_t i = 0; i < kFactSlots; ++i) s.stampEntry[i] = e[at + 4 + i];
        } else {
            std::copy(e + at, e + at + 4 + len, s.extension.begin() + s.extensionLength);
            s.extensionLength = static_cast<std::uint16_t>(s.extensionLength + 4 + len);
            ++rep.unknownTags;
        }
        at += 4u + len;
    }
    if (malformed) {
        std::copy(e, e + kStoryExtensionBytes, s.extension.begin());
        s.extensionLength = static_cast<std::uint16_t>(kStoryExtensionBytes);
        s.extensionRaw = true;
        rep.extensionMalformed = true;
        rep.unknownTags = 0;
    }
    if (s.openScene == kNoScene) s.stampEntry = s.stamp;

    // What this build makes of it.
    if (s.factsKnown > reg.factsKnown) rep.unknownSlotsPreserved = static_cast<std::uint16_t>(s.factsKnown - reg.factsKnown);
    if (s.wideKnown > reg.wideKnown)
        rep.unknownSlotsPreserved = static_cast<std::uint16_t>(rep.unknownSlotsPreserved + s.wideKnown - reg.wideKnown);
    rep.registryMismatch = s.registryHash != reg.hash;
    for (std::uint16_t slot = 1; slot < kFactSlots; ++slot) {
        const FactRow& row = reg.facts[slot];
        if (!row.used || row.kind == FactKind::Retired) continue;
        const std::uint8_t v = s.facts[slot];
        if (v != 0 && !(v == kNotOffered && canBeNotOffered(row.kind)) && !inRange(row, v)) ++rep.staleFacts;
        // derive-on-load: an old save already past the fact's scene.
        // Only slots the writer did not know: a known slot at 0 was left unset on purpose.
        if (slot > savedFactsKnown && v == 0 && row.deriveWhen.fact != 0 && committed(s, row.firstSet) &&
            condHolds(s, row.deriveWhen)) {
            s.facts[slot] = row.deriveValue;
            if (s.factsEntry[slot] == 0) s.factsEntry[slot] = row.deriveValue;
            ++rep.derived;
        }
    }
    out = s;
    if (report) *report = rep;
    return StoryLoad::Ok;
}

// ---- Import ----

namespace {

bool parseUint(std::string_view t, std::uint64_t maxValue, std::uint64_t& out) {
    if (t.empty() || t.size() > 10) return false;
    std::uint64_t v = 0;
    for (char c : t) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (v > maxValue) return false;
    out = v;
    return true;
}

bool validId(std::string_view id) {
    if (id.empty() || id.size() > 15) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

void setId(std::array<char, 16>& to, std::string_view id) {
    to.fill('\0');
    std::copy(id.begin(), id.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(id.size(), 15)), to.begin());
}

std::vector<std::string_view> splitOn(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t at = s.find(sep, start);
        out.push_back(s.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start));
        if (at == std::string_view::npos) break;
        start = at + 1;
    }
    return out;
}

std::vector<std::string_view> words(std::string_view s) {
    std::vector<std::string_view> out;
    for (std::string_view w : splitOn(s, ' '))
        if (!w.empty()) out.push_back(w);
    return out;
}

// "any" / "all" / "-" / "p03 104 401+".
bool parseScenes(std::string_view field, bool allowPlus, SceneSet& out, std::string& bad) {
    for (std::string_view w : words(field)) {
        const bool plus = allowPlus && w.size() == 4 && w.back() == '+';
        const std::uint8_t sc = storySceneOrdinal(plus ? w.substr(0, 3) : w);
        if (sc == kNoScene) {
            bad = std::string(w);
            return false;
        }
        if (plus)
            for (std::uint8_t k = sc; k < kStoryScenes; ++k) out.add(k);
        else
            out.add(sc);
    }
    return true;
}

std::uint8_t firstScene(const SceneSet& set) {
    for (std::uint8_t k = 0; k < kStoryScenes; ++k)
        if (set.has(k)) return k;
    return kNoScene;
}

struct PendingCond {
    std::string id;
    std::uint8_t field = 0xFF;
    std::uint8_t value = 0;
};

// "id=V" or "id.fN=V"; V is A..D or a number 0..255.
bool parseCond(std::string_view t, PendingCond& out) {
    const std::size_t eq = t.find('=');
    if (eq == std::string_view::npos || eq + 1 >= t.size()) return false;
    std::string_view id = t.substr(0, eq);
    const std::string_view v = t.substr(eq + 1);
    const std::size_t dot = id.find(".f");
    if (dot != std::string_view::npos) {
        std::uint64_t f = 0;
        if (!parseUint(id.substr(dot + 2), 3, f)) return false;
        out.field = static_cast<std::uint8_t>(f);
        id = id.substr(0, dot);
    }
    if (!validId(id)) return false;
    out.id = std::string(id);
    if (v.size() == 1 && v[0] >= 'A' && v[0] <= 'D') {
        out.value = static_cast<std::uint8_t>(v[0] - 'A' + 1);
        return true;
    }
    std::uint64_t n = 0;
    if (!parseUint(v, 255, n)) return false;
    out.value = static_cast<std::uint8_t>(n);
    return true;
}

struct PendingRow {
    std::uint16_t slot = 0;
    std::size_t line = 0;
    PendingCond gate;
    std::array<PendingCond, 4> options;
    PendingCond derive;
};

constexpr std::string_view kHeader = "slot,id,kind,max,default,mut,set_scenes,gate,read_scenes,fallback,derive";

} // namespace

bool importStoryFacts(std::string_view csv, FactRegistry& out, std::string* error) {
    out = FactRegistry{};
    FactRegistry& reg = out;
    std::vector<std::string> errors;
    std::vector<PendingRow> pending;
    auto fail = [&](std::size_t line, const std::string& msg) {
        errors.push_back("story_facts.csv line " + std::to_string(line) + ": " + msg);
    };
    std::vector<std::string_view> lines = splitOn(csv, '\n');
    for (std::string_view& l : lines)
        if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
    if (lines.empty() || lines[0] != kHeader) {
        errors.push_back("story_facts.csv line 1: expected the header " + std::string(kHeader));
    } else {
        std::array<bool, kFactSlots> factTaken{};
        std::array<bool, kWideSlots> wideTaken{};
        std::vector<std::string> ids;
        for (std::size_t li = 1; li < lines.size(); ++li) {
            const std::size_t line = li + 1;
            if (lines[li].empty()) continue;
            const std::vector<std::string_view> f = splitOn(lines[li], ',');
            if (f.size() != 11) {
                fail(line, "expected 11 fields");
                continue;
            }
            const std::string_view id = f[1];
            const std::string_view kind = f[2];
            if (!validId(id)) {
                fail(line, "id: expected a-z, 0-9 and _, up to 15 characters");
                continue;
            }
            if (std::find(ids.begin(), ids.end(), id) != ids.end()) fail(line, "id duplicated: " + std::string(id));
            ids.emplace_back(id);
            const bool wide = kind == "CTR" || kind == "RAW" || kind == "MASK";
            std::uint64_t slot = 0;
            if (!parseUint(f[0], wide ? kWideSlots - 1 : kFactSlots - 1, slot) || (!wide && slot == 0)) {
                fail(line, wide ? "slot: expected 0..63 for a wide slot" : "slot: expected 1..255 for a fact");
                continue;
            }
            if (wide ? wideTaken[slot] : factTaken[slot]) {
                fail(line, "slot reused: " + std::to_string(slot));
                continue;
            }
            (wide ? wideTaken[slot] : factTaken[slot]) = true;
            std::string bad;

            if (wide) {
                WideRow& w = reg.wide[slot];
                setId(w.idChars, id);
                w.kind = kind == "CTR" ? WideKind::Counter : kind == "RAW" ? WideKind::Raw : WideKind::Mask;
                std::uint64_t cap = 0;
                if (!parseUint(f[3], 0xFFFFFFFFull, cap) || cap == 0) fail(line, "max: expected 1..4294967295");
                w.cap = static_cast<std::uint32_t>(cap);
                if (f[4] != "0") fail(line, "default: a wide slot starts at 0");
                if (f[5] != "-" || f[7] != "-" || f[9] != "-" || f[10] != "-")
                    fail(line, "mut, gate, fallback, derive: expected - for a wide slot");
                w.setAny = f[6] == "any";
                if (!w.setAny && !parseScenes(f[6], false, w.setScenes, bad)) fail(line, "set_scenes: unknown scene " + bad);
                if (f[8] != "-" && !parseScenes(f[8], true, w.reads, bad)) fail(line, "read_scenes: unknown scene " + bad);
                reg.wideKnown = std::max<std::uint16_t>(reg.wideKnown, static_cast<std::uint16_t>(slot + 1));
                continue;
            }

            FactRow& r = reg.facts[slot];
            setId(r.idChars, id);
            r.used = true;
            static constexpr std::array<std::pair<std::string_view, FactKind>, 10> kKinds{{
                {"EV", FactKind::Event}, {"OP", FactKind::Optional}, {"C2", FactKind::Choice2},
                {"C3", FactKind::Choice3}, {"C4", FactKind::Choice4}, {"NUM", FactKind::Number},
                {"BITS", FactKind::Bits}, {"PK", FactKind::Packed}, {"ST", FactKind::Stepped},
                {"retired", FactKind::Retired},
            }};
            bool kindOk = false;
            for (const auto& [name, k] : kKinds)
                if (kind == name) {
                    r.kind = k;
                    kindOk = true;
                }
            if (!kindOk) {
                fail(line, "kind: expected EV, OP, C2, C3, C4, NUM, BITS, PK, ST, retired, CTR, RAW or MASK");
                continue;
            }
            reg.factsKnown = std::max<std::uint16_t>(reg.factsKnown, static_cast<std::uint16_t>(slot));
            if (r.kind == FactKind::Retired) continue;
            std::uint64_t mx = 0;
            const std::uint8_t n = choiceCount(r.kind);
            const bool maxOk = parseUint(f[3], 254, mx) && mx >= 1 &&
                               (r.kind != FactKind::Event || mx == 1) && (r.kind != FactKind::Optional || mx == 2) &&
                               (n == 0 || mx == n);
            if (!maxOk) fail(line, "max: does not fit the kind");
            r.max = static_cast<std::uint8_t>(mx);
            std::uint64_t def = 0;
            const bool numeric = r.kind == FactKind::Number || r.kind == FactKind::Stepped;
            if (!parseUint(f[4], 255, def) || (numeric ? def > r.max : def != 0))
                fail(line, "default not representable");
            r.def = static_cast<std::uint8_t>(def);
            if (f[5] == "once") r.mut = FactMut::Once;
            else if (f[5] == "mono") r.mut = FactMut::Mono;
            else if (f[5] == "free") r.mut = FactMut::Free;
            else fail(line, "mut: expected once, mono or free");
            r.setAny = f[6] == "any";
            if (!r.setAny) {
                if (f[6] == "-" || !parseScenes(f[6], false, r.setScenes, bad) || r.setScenes.empty())
                    fail(line, "set_scenes: expected any or scene codes" + (bad.empty() ? "" : ", not " + bad));
                r.firstSet = firstScene(r.setScenes);
                std::uint8_t count = 0;
                for (std::uint8_t k = 0; k < kStoryScenes; ++k) count = static_cast<std::uint8_t>(count + (r.setScenes.has(k) ? 1 : 0));
                if (r.mut == FactMut::Once && count > 1) fail(line, "a once fact with more than one set scene");
            }
            r.readAll = f[8] == "all";
            if (!r.readAll && f[8] != "-" && !parseScenes(f[8], true, r.reads, bad))
                fail(line, "read_scenes: unknown scene " + bad);
            if (f[9] == "all") {
                r.fallback = r.reads;
                if (r.readAll)
                    for (std::uint8_t k = 0; k < kStoryScenes; ++k) r.fallback.add(k);
            } else if (f[9] != "-" && !parseScenes(f[9], true, r.fallback, bad)) {
                fail(line, "fallback: unknown scene " + bad);
            }
            if (!r.setAny && !r.readAll && !r.reads.empty() && firstScene(r.reads) < r.firstSet)
                fail(line, "read in " + std::string(storySceneCode(firstScene(r.reads))) + ", before any scene sets it");
            if (canBeNotOffered(r.kind)) {
                for (std::uint8_t k = 0; k < kStoryScenes; ++k)
                    if ((r.readAll || r.reads.has(k)) && !r.fallback.has(k)) {
                        fail(line, "no fallback marked for the read in " + std::string(storySceneCode(k)));
                        break;
                    }
            }

            PendingRow p;
            p.slot = static_cast<std::uint16_t>(slot);
            p.line = line;
            if (f[7] != "-") {
                for (std::string_view tok : words(f[7])) {
                    PendingCond c;
                    std::uint8_t option = 0;
                    if (tok.size() > 2 && tok[1] == ':' && tok[0] >= 'A' && tok[0] <= 'D') {
                        option = static_cast<std::uint8_t>(tok[0] - 'A' + 1);
                        tok = tok.substr(2);
                    }
                    if (!parseCond(tok, c)) {
                        fail(line, "gate: expected id=V, id.fN=V or X:id=V");
                        continue;
                    }
                    if (option == 0) {
                        if (!p.gate.id.empty()) fail(line, "gate: one whole-fact condition at most");
                        p.gate = c;
                    } else if (n == 0 || option > n) {
                        fail(line, "gate: an option gate needs a choice with that option");
                    } else {
                        p.options[option - 1u] = c;
                    }
                }
            }
            if (f[10] != "-") {
                const std::size_t colon = f[10].find(':');
                std::uint64_t dv = 0;
                if (colon == std::string_view::npos || !parseUint(f[10].substr(0, colon), 255, dv) ||
                    !parseCond(f[10].substr(colon + 1), p.derive) || !inRange(r, static_cast<std::uint8_t>(dv)) ||
                    r.setAny)
                    fail(line, "derive: expected V:id=V with a legal V and set scenes");
                r.deriveValue = static_cast<std::uint8_t>(dv);
            }
            pending.push_back(std::move(p));
        }

        // Second pass: conditions name facts.
        auto resolve = [&](const PendingRow& p, const PendingCond& c, FactCond& to, const char* what) {
            if (c.id.empty()) return;
            std::uint16_t found = 0;
            for (std::uint16_t k = 1; k < kFactSlots; ++k)
                if (reg.facts[k].used && reg.facts[k].id() == c.id) found = k;
            if (found == 0 || reg.facts[found].kind == FactKind::Retired) {
                fail(p.line, std::string(what) + ": unknown fact " + c.id);
                return;
            }
            const FactRow& g = reg.facts[found];
            if (found == p.slot) fail(p.line, std::string(what) + ": names its own fact");
            if (c.field != 0xFF && (g.kind != FactKind::Packed || c.field >= packedFields(g.max)))
                fail(p.line, std::string(what) + ": " + c.id + " has no field " + std::to_string(c.field));
            else if (c.field != 0xFF ? c.value > 2 : (c.value != 0 && !inRange(g, c.value)))
                fail(p.line, std::string(what) + ": " + std::to_string(c.value) + " is not a value of " + c.id);
            const FactRow& self = reg.facts[p.slot];
            if (!g.setAny && !self.setAny && g.firstSet > self.firstSet)
                fail(p.line, std::string(what) + ": " + c.id + " is set in a later scene");
            to = FactCond{found, c.field, c.value};
        };
        for (const PendingRow& p : pending) {
            FactRow& r = reg.facts[p.slot];
            resolve(p, p.gate, r.gate, "gate");
            for (std::size_t o = 0; o < 4; ++o) resolve(p, p.options[o], r.optionGates[o], "gate");
            resolve(p, p.derive, r.deriveWhen, "derive");
        }
        // Per scene limits.
        for (std::uint8_t k = 0; k < kStoryScenes; ++k) {
            std::uint32_t nf = 0, nw = 0;
            for (const FactRow& r : reg.facts) nf += r.used && r.kind != FactKind::Retired && (r.readAll || r.reads.has(k));
            for (const WideRow& w : reg.wide) nw += w.kind != WideKind::None && w.reads.has(k);
            if (nf > kMaxReadsPerScene)
                errors.push_back("story_facts.csv: scene " + std::string(storySceneCode(k)) + " reads more than 24 facts");
            if (nw > kMaxWideReadsPerScene)
                errors.push_back("story_facts.csv: scene " + std::string(storySceneCode(k)) + " reads more than 8 wide values");
        }
    }
    if (!errors.empty()) {
        out = FactRegistry{};
        if (error) {
            error->clear();
            for (const std::string& e : errors) {
                if (!error->empty()) *error += '\n';
                *error += e;
            }
        }
        return false;
    }
    // FNV-1a 32 over the ids in slot order (facts, then wide), each ended by a zero byte.
    std::uint32_t h = 2166136261u;
    auto mix = [&h](std::string_view id) {
        for (char c : id) h = (h ^ static_cast<std::uint8_t>(c)) * 16777619u;
        h = (h ^ 0u) * 16777619u;
    };
    for (const FactRow& r : reg.facts)
        if (r.used) mix(r.id());
    for (const WideRow& w : reg.wide)
        if (w.kind != WideKind::None) mix(w.id());
    reg.hash = h;
    return true;
}

bool importStoryTuning(std::string_view json, StoryTuning& out, std::string* error) {
    StoryTuning t;
    std::vector<std::string> errors;
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(json, &parseError)) {
        errors.push_back("story tuning: " + parseError);
    } else if (!doc.root().isObject()) {
        errors.push_back("story tuning: expected an object");
    } else {
        const JsonValue root = doc.root();
        if (const JsonValue v = root["version"]; !(v.isInteger() && v.integer() == 1)) errors.push_back("version: expected 1");
        const JsonValue track = root["trackLength"];
        if (!track.isInteger() || track.integer() < 1 || track.integer() > 15)
            errors.push_back("trackLength: expected an integer in 1..15");
        else
            t.trackLength = static_cast<std::uint8_t>(track.integer());
        const JsonValue ps = root["trackScene"];
        const std::uint8_t psc = ps.isString() ? storySceneOrdinal(ps.string()) : kNoScene;
        if (psc == kNoScene) errors.push_back("trackScene: expected a scene code");
        else t.trackScene = psc;
        const JsonValue ms = root["milestones"];
        if (!ms.isArray() || ms.size() > t.milestoneScene.size()) {
            errors.push_back("milestones: expected an array of up to 8 milestones");
        } else {
            t.milestoneCount = 0;
            for (std::uint32_t i = 0; i < ms.size(); ++i) {
                const std::string at = "milestones[" + std::to_string(i) + "]";
                const JsonValue m = ms[i];
                const std::uint8_t sc = m["scene"].isString() ? storySceneOrdinal(m["scene"].string()) : kNoScene;
                const JsonValue v = m["value"];
                if (sc == kNoScene) errors.push_back(at + ".scene: expected a scene code");
                if (!v.isInteger() || v.integer() < 0 || v.integer() > 255) errors.push_back(at + ".value: expected 0..255");
                if (sc == kNoScene || !v.isInteger() || v.integer() < 0 || v.integer() > 255) continue;
                if (t.milestoneCount > 0 && (sc <= t.milestoneScene[t.milestoneCount - 1u] ||
                                         v.integer() < t.milestoneValue[t.milestoneCount - 1u]))
                    errors.push_back(at + ": expected a later scene and a value no lower");
                t.milestoneScene[t.milestoneCount] = sc;
                t.milestoneValue[t.milestoneCount] = static_cast<std::uint8_t>(v.integer());
                ++t.milestoneCount;
            }
        }
    }
    if (!errors.empty()) {
        if (error) {
            error->clear();
            for (const std::string& e : errors) {
                if (!error->empty()) *error += '\n';
                *error += e;
            }
        }
        return false;
    }
    out = t;
    return true;
}

} // namespace ftcore
