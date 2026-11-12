#include "ftcore/StoryState.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace ftcore {

namespace {

// ---- Bindings ----

// A wide slot when it is registered, else kNoWide (an unbound role is inert).
std::uint8_t bound(std::uint8_t wideSlot, const FactRegistry& reg) {
    return wideSlot < kWideSlots && reg.wide[wideSlot].kind != WideKind::None ? wideSlot : kNoWide;
}
bool factBound(std::uint16_t slot, const FactRegistry& reg) {
    return slot != 0 && slot < kFactSlots && reg.facts[slot].used && reg.facts[slot].kind != FactKind::Retired;
}


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

std::uint32_t sideTotal(const std::array<std::uint32_t, kWideSlots>& w, const FactRegistry& reg, std::uint8_t side) {
    std::uint32_t sum = 0;
    for (const std::uint8_t c : reg.roles.pressCounters[side])
        if (bound(c, reg) != kNoWide) sum = storySaturatingAdd(sum, w[c], 0xFFFFFFFFu);
    return sum;
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

std::uint8_t storySceneOrdinal(const FactRegistry& reg, std::string_view code) {
    if (code.empty() || code.size() >= reg.sceneCodes[0].size()) return kNoScene;
    for (std::uint8_t i = 0; i < reg.sceneCount; ++i)
        if (code == std::string_view(reg.sceneCodes[i].data())) return i;
    return kNoScene;
}

std::string_view storySceneCode(const FactRegistry& reg, std::uint8_t ordinal) {
    return ordinal < reg.sceneCount ? std::string_view(reg.sceneCodes[ordinal].data()) : std::string_view();
}

std::uint16_t storyFactSlot(const FactRegistry& reg, std::string_view id) {
    for (std::uint16_t slot = 1; slot < kFactSlots; ++slot)
        if (factBound(slot, reg) && reg.facts[slot].id() == id) return slot;
    return 0;
}

std::uint8_t storyWideSlot(const FactRegistry& reg, std::string_view id) {
    for (std::uint8_t w = 0; w < kWideSlots; ++w)
        if (reg.wide[w].kind != WideKind::None && reg.wide[w].id() == id) return w;
    return kNoWide;
}

// ---- Formulas ----

std::uint32_t storySaturatingAdd(std::uint32_t c, std::uint32_t n, std::uint32_t cap) {
    const std::uint64_t sum = static_cast<std::uint64_t>(c) + n;
    return sum >= cap ? cap : static_cast<std::uint32_t>(sum);
}

std::uint32_t storyPressTotal(const StoryState& s, const FactRegistry& reg) {
    const std::uint32_t floor = reg.tuning.pressFloor;
    return storySaturatingAdd(std::max(floor, sideTotal(s.wide, reg, 0)), std::max(floor, sideTotal(s.wide, reg, 1)),
                              0xFFFFFFFFu);
}

std::uint8_t storyTopKind(const StoryState& s, const FactRegistry& reg) {
    const std::array<std::uint8_t, 3>& c = reg.roles.pressCounters[0];
    auto count = [&](std::uint8_t k) { return bound(c[k], reg) == kNoWide ? 0u : s.wide[c[k]]; };
    std::uint32_t best = 0;
    for (std::uint8_t k = 0; k < 3; ++k) best = std::max(best, count(k));
    const std::uint16_t lastSlot = reg.roles.lastKind[0];
    const std::uint8_t last = factBound(lastSlot, reg) ? s.facts[lastSlot] : 0;
    if (last >= 1 && last <= 3 && count(static_cast<std::uint8_t>(last - 1u)) == best) return static_cast<std::uint8_t>(last - 1u);
    for (std::uint8_t k = 0; k < 3; ++k)
        if (count(k) == best) return k;
    return 0;
}

std::uint32_t storyCrc32(std::span<const std::uint8_t> bytes, std::uint32_t crc) {
    return crcUpdate(crc ^ 0xFFFFFFFFu, bytes.data(), bytes.size()) ^ 0xFFFFFFFFu;
}

// ---- Get and set ----

void storyReset(StoryState& s) { s = StoryState{}; }

FactRead storyGet(const StoryState& s, const FactRegistry& reg, std::uint16_t slot) {
    FactRead r;
    r.fact = slot;
    if (!factBound(slot, reg)) return r; // Unknown
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

SetResult storySet(StoryState& s, const FactRegistry& reg, std::uint16_t slot, std::uint8_t value) {
    if (!factBound(slot, reg)) return reject(s, slot, SetResult::UnknownFact, value);
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

SetResult storySetBits(StoryState& s, const FactRegistry& reg, std::uint16_t slot, std::uint8_t bits) {
    if (!factBound(slot, reg)) return storySet(s, reg, slot, bits);
    const std::uint8_t old = s.facts[slot] == kNotOffered ? 0 : s.facts[slot];
    return storySet(s, reg, slot, static_cast<std::uint8_t>(old | bits));
}

std::uint32_t storyWide(const StoryState& s, std::uint8_t slot) { return slot < kWideSlots ? s.wide[slot] : 0; }

SetResult storyAdd(StoryState& s, const FactRegistry& reg, std::uint8_t w, std::uint32_t n) {
    const std::uint16_t id = static_cast<std::uint16_t>(kStoryWideEventBit | w);
    if (bound(w, reg) == kNoWide) return reject(s, id, SetResult::UnknownFact, n);
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

SetResult storyCountPress(StoryState& s, const FactRegistry& reg, std::uint8_t side, std::uint8_t kind) {
    if (side > 1 || kind > 2)
        return reject(s, static_cast<std::uint16_t>(kStoryWideEventBit | kNoWide), SetResult::RejectedRange, kind);
    const StoryRoles& ro = reg.roles;
    const std::uint8_t counter = bound(ro.pressCounters[side][kind], reg);
    const std::uint16_t id = static_cast<std::uint16_t>(kStoryWideEventBit | ro.pressCounters[side][kind]);
    if (counter == kNoWide) return reject(s, id, SetResult::UnknownFact, kind);
    if (s.openScene == kNoScene) return reject(s, id, SetResult::RejectedNoScene, kind);
    const std::uint16_t open = ro.sideOpen[side];
    if (factBound(open, reg) && s.facts[open] == 0) return reject(s, id, SetResult::RejectedGate, kind);
    const bool rose = addWide(s, counter, 1, reg.wide[counter].cap);
    if (const std::uint8_t w = bound(ro.pressScene, reg); w != kNoWide) addWide(s, w, 1, reg.wide[w].cap);
    if (const std::uint8_t w = bound(ro.pressTotal, reg); w != kNoWide) addWide(s, w, 1, reg.wide[w].cap);
    if (factBound(ro.lastKind[side], reg)) writeFact(s, ro.lastKind[side], static_cast<std::uint8_t>(kind + 1u));
    if (side == 0 && factBound(ro.firstPress, reg) && s.facts[ro.firstPress] == 0) writeFact(s, ro.firstPress, 1);
    return rose ? SetResult::Ok : SetResult::Unchanged;
}

// ---- Scene reads ----

SceneReads storySceneReads(const StoryState& s, const FactRegistry& reg, std::uint8_t sc) {
    SceneReads out;
    out.scene = sc;
    if (sc >= reg.sceneCount) return out;
    out.complete = true;
    for (std::uint16_t slot = 1; slot < kFactSlots; ++slot) {
        const FactRow& row = reg.facts[slot];
        if (!row.used || row.kind == FactKind::Retired || !(row.readAll || row.reads.has(sc))) continue;
        if (out.count == kMaxReadsPerScene) {
            out.complete = false;
            break;
        }
        out.facts[out.count++] = storyGet(s, reg, slot);
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
    for (std::uint8_t i = 0; i < reg.derivedCount && i < kMaxDerived; ++i) {
        const DeriveRow& row = reg.derived[i];
        const FactRead a = storyGet(s, reg, row.fact);
        bool valid = false;
        std::uint32_t v = 0;
        switch (row.kind) {
        case DeriveKind::None: break;
        case DeriveKind::Value:
            valid = a.status == FactStatus::Set;
            v = a.value;
            break;
        case DeriveKind::Read:
            valid = a.status != FactStatus::Unknown;
            v = a.value;
            break;
        case DeriveKind::In:
            valid = a.status == FactStatus::Set;
            v = a.value < 32 ? (row.mask >> a.value) & 1u : 0u;
            break;
        case DeriveKind::Upgrade:
            valid = a.status == FactStatus::Unset || a.status == FactStatus::Set;
            v = a.status != FactStatus::Set ? 0u
                : a.value == row.from && row.cond.fact != 0 && condHolds(s, row.cond) ? row.to
                                                                                    : a.value;
            break;
        case DeriveKind::TopKind:
            valid = true;
            v = storyTopKind(s, reg);
            break;
        case DeriveKind::PressTotal:
            valid = true;
            v = storyPressTotal(s, reg);
            break;
        }
        if (!valid) continue;
        d.value[i] = v;
        d.valid = static_cast<std::uint16_t>(d.valid | (1u << i));
    }
    return out;
}

// ---- Scenes ----

bool storyBeginScene(StoryState& s, const FactRegistry& reg, std::uint8_t sc) {
    if (s.openScene != kNoScene || sc >= kMaxStoryScenes) return false;
    if (s.lastCommitted != kNoScene && sc <= s.lastCommitted) return false;
    if (const std::uint8_t w = bound(reg.roles.pressScene, reg); w != kNoWide) s.wide[w] = 0;
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
    if (const std::uint8_t w = bound(reg.roles.retries, reg); w != kNoWide) addWide(s, w, 1, reg.wide[w].cap);
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
    if (const std::uint8_t mile = bound(reg.roles.milestone, reg); mile != kNoWide)
        for (std::uint8_t i = 0; i < t.milestoneCount && i < kMaxMilestones; ++i)
            if (t.milestoneScene[i] == sc && s.wide[mile] < t.milestoneValue[i]) setWide(s, mile, t.milestoneValue[i]);
    if (const std::uint8_t mark = bound(reg.roles.trackMark, reg); mark != kNoWide && sc == t.trackScene) {
        const std::uint8_t side = t.trackSide > 1 ? 1 : t.trackSide;
        const std::uint32_t now = sideTotal(s.wide, reg, side);
        const std::uint32_t pressed = now - std::min(now, sideTotal(s.wideEntry, reg, side));
        setWide(s, mark, std::min<std::uint32_t>(pressed, t.trackLength));
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
    out.lastCommitted = sc == 0 || sc >= kMaxStoryScenes ? kNoScene : static_cast<std::uint8_t>(sc - 1u);
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

bool ruleHolds(const StoryState& s, const FactRegistry& reg, const RuleCond& c) {
    if (!factBound(c.fact, reg)) return false;
    const std::uint8_t v = s.facts[c.fact];
    if (v == 0 || v == kNotOffered) return false;
    switch (c.op) {
    case RuleOp::Set: return inRange(reg.facts[c.fact], v);
    case RuleOp::Equals: return v == c.value;
    case RuleOp::Field: return fieldOf(v, c.field) == c.value;
    case RuleOp::AnyBits: return (v & c.value) != 0;
    }
    return false;
}

std::uint8_t setScenesReached(const StoryState& s, const SceneSet& set) {
    std::uint8_t n = 0;
    for (std::uint8_t sc = 0; sc < kMaxStoryScenes; ++sc)
        if (set.has(sc) && reached(s, sc)) ++n;
    return n;
}

} // namespace

std::uint32_t storyValidate(const StoryState& s, const FactRegistry& reg, std::span<StoryIssue> out) {
    IssueSink sink{out};
    const StoryRoles& ro = reg.roles;
    auto setV = [&](std::uint16_t slot) {
        const std::uint8_t v = s.facts[slot];
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
        // V06: an option offered only under a condition was taken while it did not hold.
        if (isSet && choiceCount(row.kind) != 0 && !condHolds(s, row.optionGates[v - 1u]))
            sink.add(6, slot, row.optionGates[v - 1u].fact);
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
        // V18: a stepped value rises at most one step per set scene reached.
        if (row.kind == FactKind::Stepped && !row.setAny && isSet && v > row.def &&
            v - row.def > setScenesReached(s, row.setScenes))
            sink.add(18, slot);
        // V19: a value outside the range.
        if (v != 0 && !(v == kNotOffered && canBeNotOffered(row.kind)) && !inRange(row, v))
            sink.add(19, slot, 0, IssueSeverity::Warning);
    }
    for (std::uint16_t slot = reg.factsKnown + 1u; slot < kFactSlots; ++slot) { // V05 for unknown slots too
        const std::uint8_t st = s.stamp[slot];
        if (st != kNoScene && st != s.openScene && !committed(s, st)) sink.add(5, slot);
    }

    // V08: the milestone count against its milestones.
    const StoryTuning& t = reg.tuning;
    if (const std::uint8_t m = bound(ro.milestone, reg); m != kNoWide) {
        const std::uint32_t mile = s.wide[m];
        for (std::uint8_t i = 0; i < t.milestoneCount && i < kMaxMilestones; ++i) {
            if (committed(s, t.milestoneScene[i]) && mile < t.milestoneValue[i]) sink.add(8, W | m);
            if (!committed(s, t.milestoneScene[i]) && mile >= t.milestoneValue[i] && i > 0 &&
                t.milestoneValue[i] > t.milestoneValue[i - 1u])
                sink.add(8, W | m);
            const std::uint16_t needs = t.milestoneNeeds[i];
            if (factBound(needs, reg) && t.milestoneValue[i] > 0 && mile >= t.milestoneValue[i] && !setV(needs))
                sink.add(8, W | m, needs);
        }
    }
    // V10: presses need their slot.
    for (std::uint8_t side = 0; side < 2; ++side) {
        if (sideTotal(s.wide, reg, side) == 0) continue;
        const std::uint16_t first = W | ro.pressCounters[side][0];
        if (side == 0 && factBound(ro.firstPress, reg) && s.facts[ro.firstPress] == 0)
            sink.add(10, ro.firstPress, first);
        if (factBound(ro.sideOpen[side], reg) && s.facts[ro.sideOpen[side]] == 0)
            sink.add(10, ro.sideOpen[side], first);
    }
    // V11: the track mark is at most the track side's presses and the track length.
    if (const std::uint8_t mk = bound(ro.trackMark, reg); mk != kNoWide) {
        const std::uint32_t mark = s.wide[mk];
        if (mark > sideTotal(s.wide, reg, t.trackSide > 1 ? 1 : t.trackSide) || mark > t.trackLength)
            sink.add(11, W | mk);
    }
    // V15: the last kind pressed matches the counts.
    for (std::uint8_t side = 0; side < 2; ++side) {
        const std::uint16_t lastSlot = ro.lastKind[side];
        if (!factBound(lastSlot, reg)) continue;
        const std::uint8_t last = s.facts[lastSlot];
        if (last == 0 && sideTotal(s.wide, reg, side) > 0) sink.add(15, lastSlot);
        if (last >= 1 && last <= 3) {
            const std::uint8_t c = bound(ro.pressCounters[side][last - 1u], reg);
            if (c == kNoWide || s.wide[c] == 0) sink.add(15, lastSlot);
        }
    }
    // V16: a scene counter is nonzero before its first set scene.
    for (std::uint8_t w = 0; w < kWideSlots; ++w) {
        const WideRow& row = reg.wide[w];
        if (row.kind == WideKind::None || row.setAny || row.firstSet == kNoScene) continue;
        if (s.wide[w] != 0 && !reached(s, row.firstSet)) sink.add(16, W | w);
    }
    // V17: the scene markers.
    if (s.openScene != kNoScene && s.lastCommitted != kNoScene && s.openScene <= s.lastCommitted) sink.add(17, 0);
    if (s.openScene == kNoScene && s.factsEntry != s.facts) sink.add(17, 0);
    // V20: written by a different table that is not newer.
    if (s.registryHash != 0 && s.registryHash != reg.hash && s.factsKnown <= reg.factsKnown)
        sink.add(20, 0, 0, IssueSeverity::Warning);

    // The registry's own rules.
    for (std::uint8_t i = 0; i < reg.ruleCount && i < kMaxStoryRules; ++i) {
        const StoryRule& r = reg.rules[i];
        if (r.kind == RuleKind::BitsInOrder) {
            if (!factBound(r.when.fact, reg)) continue;
            const std::uint8_t bits = s.facts[r.when.fact] == kNotOffered ? 0 : s.facts[r.when.fact];
            for (std::uint8_t b = 1; b < 8; ++b)
                if (((bits >> b) & 1u) && !((bits >> (b - 1u)) & 1u)) sink.add(r.number, r.when.fact);
            continue;
        }
        if (!ruleHolds(s, reg, r.when)) continue;
        if (r.settled && !committed(s, reg.facts[r.when.fact].firstSet)) continue;
        if (!ruleHolds(s, reg, r.then)) sink.add(r.number, r.when.fact, r.then.fact);
    }
    return sink.n;
}

// ---- The block ----

bool storyEncode(StoryState& s, const FactRegistry& reg, std::span<std::uint8_t, kStoryStateBlockSize> out) {
    if ((s.flags & kStorySandbox) != 0) return false;
    ++s.saveSerial;
    s.factsKnown = std::max(s.factsKnown, reg.factsKnown);
    s.wideKnown = std::max(s.wideKnown, reg.wideKnown);
    if (reg.hash != 0) s.registryHash = reg.hash; // an empty registry keeps the writer's hash
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
        if (s.openScene != kNoScene) ++s.lostStampRecords;
    } else {
        std::uint32_t at = std::min<std::uint32_t>(s.extensionLength, kStoryExtensionBytes);
        std::copy(s.extension.begin(), s.extension.begin() + at, e);
        if (s.openScene != kNoScene && at + 4 + kFactSlots + 2 <= kStoryExtensionBytes) {
            put16(e + at, kTagStampEntry);
            put16(e + at + 2, static_cast<std::uint16_t>(kFactSlots));
            for (std::uint32_t i = 0; i < kFactSlots; ++i) e[at + 4 + i] = s.stampEntry[i];
            at += 4 + kFactSlots;
        } else if (s.openScene != kNoScene) {
            ++s.lostStampRecords;
        }
        // The end tag (0) is already there: the area was zeroed.
    }
    put32(b + 28, blockCrc(b));
    return true;
}

StoryLoad storyLoadOrNew(std::span<const std::uint8_t> in, const FactRegistry& reg, StoryState& out,
                         StoryStateLoadReport* report) {
    StoryState loaded;
    const StoryLoad r = storyDecode(in, reg, loaded, report);
    if (r == StoryLoad::Ok) out = loaded;
    else storyReset(out);
    return r;
}

const char* storyLoadName(StoryLoad r) {
    switch (r) {
    case StoryLoad::Ok: return "ok";
    case StoryLoad::Empty: return "empty";
    case StoryLoad::BadMagic: return "bad magic";
    case StoryLoad::BadSize: return "bad size";
    case StoryLoad::BadChecksum: return "bad checksum";
    case StoryLoad::NewerLayout: return "newer layout";
    case StoryLoad::BadScene: return "bad scene";
    case StoryLoad::BadVersion: return "bad version";
    }
    return "unknown";
}

bool storySameSavedImage(const StoryState& a, const StoryState& b) {
    if (a.factsKnown != b.factsKnown || a.wideKnown != b.wideKnown || a.registryHash != b.registryHash ||
        a.openScene != b.openScene || a.lastCommitted != b.lastCommitted || a.saveSerial != b.saveSerial)
        return false;
    if (a.facts != b.facts || a.factsEntry != b.factsEntry || a.stamp != b.stamp || a.wide != b.wide ||
        a.wideEntry != b.wideEntry)
        return false;
    // The stamps-at-entry record is written only while a scene is open.
    if (a.openScene != kNoScene && a.stampEntry != b.stampEntry) return false;
    if (a.extensionRaw != b.extensionRaw) return false;
    if (a.extensionRaw) return a.extension == b.extension;
    const std::uint32_t na = std::min<std::uint32_t>(a.extensionLength, kStoryExtensionBytes);
    const std::uint32_t nb = std::min<std::uint32_t>(b.extensionLength, kStoryExtensionBytes);
    return na == nb && std::equal(a.extension.begin(), a.extension.begin() + na, b.extension.begin());
}

std::uint32_t storyProgress(std::uint8_t openScene, std::uint8_t lastCommitted) {
    if (openScene != kNoScene) return 2u * openScene + 1u;
    return lastCommitted != kNoScene ? 2u * lastCommitted + 2u : 0u;
}

std::uint8_t storyScenePosition(const StoryState& s) {
    return s.openScene != kNoScene ? s.openScene : s.lastCommitted;
}

StorySaveFallback storySaveFallback(const StoryState& now, const StoryState* good,
                                    std::span<const std::uint8_t> goodBlock) {
    if (!good || goodBlock.size() != kStoryStateBlockSize) return StorySaveFallback::SkipNoGoodBlock;
    // b[16], b[17]: the block's open and last committed scene (storyEncode).
    if (storyProgress(goodBlock[16], goodBlock[17]) != storyProgress(now.openScene, now.lastCommitted))
        return StorySaveFallback::SkipMoved;
    return storySameSavedImage(now, *good) ? StorySaveFallback::WriteLastGood : StorySaveFallback::SkipMoved;
}

bool storyNoteSave(StorySaveSkip& k, bool skipped, std::uint8_t scene) {
    if (!skipped) return storyClearSaveSkip(k);
    const bool started = !k.skipping;
    if (started) {
        k.skipping = true;
        k.sinceScene = scene;
        k.skipped = 0;
    }
    if (k.skipped != UINT32_MAX) ++k.skipped;
    return started;
}

bool storyClearSaveSkip(StorySaveSkip& k) {
    const bool was = k.skipping;
    k = StorySaveSkip{};
    return was;
}

StoryLoad storyDecode(std::span<const std::uint8_t> in, const FactRegistry& reg, StoryState& out,
                      StoryStateLoadReport* report) {
    StoryStateLoadReport rep;
    if (in.empty()) return StoryLoad::Empty;
    if (in.size() < 8) return StoryLoad::BadSize;
    const std::uint8_t* b = in.data();
    if (b[0] != 0x46 || b[1] != 0x54 || b[2] != 0x53 || b[3] != 0x53) return StoryLoad::BadMagic;
    if (get16(b + 4) == 0) return StoryLoad::BadVersion;
    if (get16(b + 4) > kStoryLayoutVersion) return StoryLoad::NewerLayout;
    if (in.size() != kStoryStateBlockSize || get16(b + 6) != kStoryStateBlockSize) return StoryLoad::BadSize;
    if (get32(b + 28) != blockCrc(b)) return StoryLoad::BadChecksum;
    auto badScene = [](std::uint8_t scene) { return scene != kNoScene && scene >= kMaxStoryScenes; };
    if (badScene(b[16]) || badScene(b[17])) return StoryLoad::BadScene;
    for (std::uint32_t i = 0; i < kFactSlots; ++i)
        if (badScene(b[544 + i])) return StoryLoad::BadScene;

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
            for (std::uint32_t i = 0; i < kFactSlots; ++i) {
                if (badScene(e[at + 4 + i])) return StoryLoad::BadScene;
                s.stampEntry[i] = e[at + 4 + i];
            }
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

// "any" / "all" / "-" / "s1 s4 s9+" (a trailing + adds every later scene).
bool parseScenes(const FactRegistry& reg, std::string_view field, bool allowPlus, SceneSet& out, std::string& bad) {
    for (std::string_view w : words(field)) {
        const bool plus = allowPlus && w.size() > 1 && w.back() == '+';
        const std::uint8_t sc = storySceneOrdinal(reg, plus ? w.substr(0, w.size() - 1) : w);
        if (sc == kNoScene) {
            bad = std::string(w);
            return false;
        }
        if (plus)
            for (std::uint8_t k = sc; k < reg.sceneCount; ++k) out.add(k);
        else
            out.add(sc);
    }
    return true;
}

std::uint8_t firstScene(const SceneSet& set) {
    for (std::uint8_t k = 0; k < kMaxStoryScenes; ++k)
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

void importCsv(std::string_view csv, FactRegistry& reg, std::vector<std::string>& errors) {
    std::vector<PendingRow> pending;
    auto fail = [&](std::size_t line, const std::string& msg) {
        errors.push_back("story_facts.csv line " + std::to_string(line) + ": " + msg);
    };
    if (csv.starts_with("\xEF\xBB\xBF")) csv.remove_prefix(3); // a UTF-8 byte order mark
    std::vector<std::string_view> lines = splitOn(csv, '\n');
    for (std::string_view& l : lines)
        if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
    if (lines.empty() || lines[0] != kHeader) {
        errors.push_back("story_facts.csv line 1: expected the header " + std::string(kHeader));
        return;
    }
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
            if (!w.setAny && f[6] != "-" && !parseScenes(reg, f[6], false, w.setScenes, bad))
                fail(line, "set_scenes: unknown scene " + bad);
            w.firstSet = w.setAny ? kNoScene : firstScene(w.setScenes);
            if (f[8] != "-" && !parseScenes(reg, f[8], true, w.reads, bad)) fail(line, "read_scenes: unknown scene " + bad);
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
            if (f[6] == "-" || !parseScenes(reg, f[6], false, r.setScenes, bad) || r.setScenes.empty())
                fail(line, "set_scenes: expected any or scene codes" + (bad.empty() ? "" : ", not " + bad));
            r.firstSet = firstScene(r.setScenes);
            std::uint8_t count = 0;
            for (std::uint8_t k = 0; k < reg.sceneCount; ++k) count = static_cast<std::uint8_t>(count + (r.setScenes.has(k) ? 1 : 0));
            if (r.mut == FactMut::Once && count > 1) fail(line, "a once fact with more than one set scene");
        }
        r.readAll = f[8] == "all";
        if (!r.readAll && f[8] != "-" && !parseScenes(reg, f[8], true, r.reads, bad))
            fail(line, "read_scenes: unknown scene " + bad);
        if (f[9] == "all") {
            r.fallback = r.reads;
            if (r.readAll)
                for (std::uint8_t k = 0; k < reg.sceneCount; ++k) r.fallback.add(k);
        } else if (f[9] != "-" && !parseScenes(reg, f[9], true, r.fallback, bad)) {
            fail(line, "fallback: unknown scene " + bad);
        }
        if (!r.setAny && !r.readAll && !r.reads.empty() && firstScene(r.reads) < r.firstSet)
            fail(line, "read in " + std::string(storySceneCode(reg, firstScene(r.reads))) + ", before any scene sets it");
        if (canBeNotOffered(r.kind)) {
            for (std::uint8_t k = 0; k < reg.sceneCount; ++k)
                if ((r.readAll || r.reads.has(k)) && !r.fallback.has(k)) {
                    fail(line, "no fallback marked for the read in " + std::string(storySceneCode(reg, k)));
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
        if (found == p.slot) {
            fail(p.line, std::string(what) + ": names its own fact");
            return;
        }
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
    for (std::uint8_t k = 0; k < reg.sceneCount; ++k) {
        std::uint32_t nf = 0, nw = 0;
        for (const FactRow& r : reg.facts) nf += r.used && r.kind != FactKind::Retired && (r.readAll || r.reads.has(k));
        for (const WideRow& w : reg.wide) nw += w.kind != WideKind::None && w.reads.has(k);
        if (nf > kMaxReadsPerScene)
            errors.push_back("story_facts.csv: scene " + std::string(storySceneCode(reg, k)) + " reads more than 24 facts");
        if (nw > kMaxWideReadsPerScene)
            errors.push_back("story_facts.csv: scene " + std::string(storySceneCode(reg, k)) + " reads more than 8 wide values");
    }
}

// ---- The json ----

struct JsonErrors {
    std::vector<std::string>& errors;
    void add(const std::string& at, const std::string& msg) { errors.push_back("story_tuning.json " + at + ": " + msg); }
};

void readScenes(const JsonValue& root, FactRegistry& reg, JsonErrors& je) {
    const JsonValue sc = root["scenes"];
    if (!sc.isArray() || sc.size() == 0 || sc.size() > kMaxStoryScenes) {
        je.add("scenes", "expected an array of 1..128 scene codes");
        return;
    }
    for (std::uint32_t i = 0; i < sc.size(); ++i) {
        const std::string at = "scenes[" + std::to_string(i) + "]";
        const std::string_view code = sc[i].isString() ? sc[i].string() : std::string_view();
        bool ok = !code.empty() && code.size() < reg.sceneCodes[0].size();
        for (char c : code) ok = ok && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'));
        if (!ok) {
            je.add(at, "expected 1..7 characters a-z and 0-9");
            continue;
        }
        if (storySceneOrdinal(reg, code) != kNoScene) {
            je.add(at, "duplicated: " + std::string(code));
            continue;
        }
        std::array<char, 8>& to = reg.sceneCodes[reg.sceneCount++];
        to.fill('\0');
        std::copy(code.begin(), code.end(), to.begin());
    }
}

// A wide role: absent or "" leaves it unbound.
void wideRole(const JsonValue& v, const FactRegistry& reg, std::uint8_t& to, const std::string& at, JsonErrors& je) {
    if (v.isAbsent() || (v.isString() && v.string().empty())) return;
    const std::uint8_t w = v.isString() ? storyWideSlot(reg, v.string()) : kNoWide;
    if (w == kNoWide || reg.wide[w].kind == WideKind::Mask) je.add(at, "expected a counter id");
    else to = w;
}

void factRole(const JsonValue& v, const FactRegistry& reg, std::uint16_t& to, const std::string& at, JsonErrors& je) {
    if (v.isAbsent() || (v.isString() && v.string().empty())) return;
    const std::uint16_t f = v.isString() ? storyFactSlot(reg, v.string()) : 0;
    if (f == 0) je.add(at, "expected a fact id");
    else to = f;
}

void readRoles(const JsonValue& root, FactRegistry& reg, JsonErrors& je) {
    const JsonValue r = root["roles"];
    if (r.isAbsent()) return;
    if (!r.isObject()) {
        je.add("roles", "expected an object");
        return;
    }
    StoryRoles& ro = reg.roles;
    const JsonValue pc = r["pressCounters"];
    if (!pc.isAbsent()) {
        if (!pc.isArray() || pc.size() != 2 || pc[0].size() != 3 || pc[1].size() != 3) {
            je.add("roles.pressCounters", "expected two arrays of three counter ids");
        } else {
            for (std::uint32_t side = 0; side < 2; ++side)
                for (std::uint32_t k = 0; k < 3; ++k) {
                    const std::string at = "roles.pressCounters[" + std::to_string(side) + "][" + std::to_string(k) + "]";
                    std::uint8_t& to = ro.pressCounters[side][k];
                    wideRole(pc[side][k], reg, to, at, je);
                    // The press sums rely on this cap: three counters per side cannot wrap 32 bits.
                    if (to != kNoWide && (reg.wide[to].kind != WideKind::Counter || reg.wide[to].cap > kMaxPressCap)) {
                        je.add(at, "a press counter is a CTR with max at most 65535");
                        to = kNoWide;
                    }
                }
        }
    }
    wideRole(r["pressScene"], reg, ro.pressScene, "roles.pressScene", je);
    wideRole(r["pressTotal"], reg, ro.pressTotal, "roles.pressTotal", je);
    wideRole(r["retries"], reg, ro.retries, "roles.retries", je);
    wideRole(r["milestone"], reg, ro.milestone, "roles.milestone", je);
    wideRole(r["trackMark"], reg, ro.trackMark, "roles.trackMark", je);
    factRole(r["firstPress"], reg, ro.firstPress, "roles.firstPress", je);
    if (ro.firstPress != 0 && !inRange(reg.facts[ro.firstPress], 1)) {
        je.add("roles.firstPress", "expected a fact that can hold 1");
        ro.firstPress = 0;
    }
    for (const char* name : {"lastKind", "sideOpen"}) {
        const JsonValue a = r[name];
        if (a.isAbsent()) continue;
        if (!a.isArray() || a.size() != 2) {
            je.add(std::string("roles.") + name, "expected two fact ids (\"\" for none)");
            continue;
        }
        std::array<std::uint16_t, 2>& to = name[0] == 'l' ? ro.lastKind : ro.sideOpen;
        for (std::uint32_t side = 0; side < 2; ++side) {
            const std::string at = std::string("roles.") + name + "[" + std::to_string(side) + "]";
            factRole(a[side], reg, to[side], at, je);
            if (name[0] == 'l' && to[side] != 0 &&
                (reg.facts[to[side]].kind != FactKind::Number || reg.facts[to[side]].max < 3)) {
                je.add(at, "expected a NUM fact with max at least 3");
                to[side] = 0;
            }
        }
    }
    // No slot serves two roles: each role's writes would land on the other's.
    std::array<std::uint8_t, kWideSlots> wideUses{};
    auto wideOnce = [&](std::uint8_t w) {
        if (w != kNoWide && wideUses[w]++ == 1) je.add("roles", "wide " + std::string(reg.wide[w].id()) + " bound to two roles");
    };
    for (const auto& side : ro.pressCounters)
        for (const std::uint8_t w : side) wideOnce(w);
    for (const std::uint8_t w : {ro.pressScene, ro.pressTotal, ro.retries, ro.milestone, ro.trackMark}) wideOnce(w);
    std::array<std::uint8_t, kFactSlots> factUses{};
    for (const std::uint16_t f : {ro.lastKind[0], ro.lastKind[1], ro.firstPress, ro.sideOpen[0], ro.sideOpen[1]})
        if (f != 0 && factUses[f]++ == 1) je.add("roles", "fact " + std::string(reg.facts[f].id()) + " bound to two roles");
}

void readPressFloor(const JsonValue& root, FactRegistry& reg, JsonErrors& je) {
    const JsonValue f = root["pressFloor"];
    if (f.isAbsent()) return;
    if (!f.isInteger() || f.integer() < 0 || f.integer() > kMaxPressCap) je.add("pressFloor", "expected 0..65535");
    else reg.tuning.pressFloor = static_cast<std::uint32_t>(f.integer());
}

void readTrack(const JsonValue& root, FactRegistry& reg, JsonErrors& je) {
    StoryTuning& t = reg.tuning;
    const JsonValue tr = root["track"];
    if (tr.isAbsent()) {
        if (reg.roles.trackMark != kNoWide) je.add("track", "the trackMark role needs a track");
        return;
    }
    const JsonValue len = tr["length"];
    if (!len.isInteger() || len.integer() < 1 || len.integer() > 255) je.add("track.length", "expected 1..255");
    else t.trackLength = static_cast<std::uint8_t>(len.integer());
    const JsonValue sc = tr["scene"];
    const std::uint8_t ord = sc.isString() ? storySceneOrdinal(reg, sc.string()) : kNoScene;
    if (ord == kNoScene) je.add("track.scene", "expected a scene code");
    else t.trackScene = ord;
    const JsonValue side = tr["side"];
    if (!side.isInteger() || side.integer() < 0 || side.integer() > 1) je.add("track.side", "expected 0 or 1");
    else t.trackSide = static_cast<std::uint8_t>(side.integer());
    if (reg.roles.trackMark != kNoWide && t.trackLength > reg.wide[reg.roles.trackMark].cap)
        je.add("track.length", "above the trackMark counter's max");
}

void readMilestones(const JsonValue& root, FactRegistry& reg, JsonErrors& je) {
    StoryTuning& t = reg.tuning;
    const JsonValue ms = root["milestones"];
    if (ms.isAbsent()) return;
    if (!ms.isArray() || ms.size() > kMaxMilestones) {
        je.add("milestones", "expected an array of up to 8 milestones");
        return;
    }
    if (ms.size() > 0 && reg.roles.milestone == kNoWide) je.add("milestones", "need the milestone role");
    for (std::uint32_t i = 0; i < ms.size(); ++i) {
        const std::string at = "milestones[" + std::to_string(i) + "]";
        const JsonValue m = ms[i];
        const std::uint8_t sc = m["scene"].isString() ? storySceneOrdinal(reg, m["scene"].string()) : kNoScene;
        const JsonValue v = m["value"];
        const bool valueOk = v.isInteger() && v.integer() >= 1 && v.integer() <= 0xFFFFFFFFll;
        if (sc == kNoScene) je.add(at + ".scene", "expected a scene code");
        if (!valueOk) je.add(at + ".value", "expected 1..4294967295");
        if (sc == kNoScene || !valueOk) continue;
        if (t.milestoneCount > 0 && (sc <= t.milestoneScene[t.milestoneCount - 1u] ||
                                     v.integer() < t.milestoneValue[t.milestoneCount - 1u]))
            je.add(at, "expected a later scene and a value no lower");
        if (reg.roles.milestone != kNoWide && v.integer() > reg.wide[reg.roles.milestone].cap)
            je.add(at + ".value", "above the milestone counter's max");
        std::uint16_t needs = 0;
        factRole(m["needs"], reg, needs, at + ".needs", je);
        t.milestoneScene[t.milestoneCount] = sc;
        t.milestoneValue[t.milestoneCount] = static_cast<std::uint32_t>(v.integer());
        t.milestoneNeeds[t.milestoneCount] = needs;
        ++t.milestoneCount;
    }
}

// "id", "id=V", "id.fN=V" or "id&M".
bool parseRuleCond(const FactRegistry& reg, std::string_view t, RuleCond& out) {
    std::string_view id = t;
    std::uint64_t n = 0;
    if (const std::size_t amp = t.find('&'); amp != std::string_view::npos) {
        if (!parseUint(t.substr(amp + 1), 255, n) || n == 0) return false;
        id = t.substr(0, amp);
        out.op = RuleOp::AnyBits;
        out.value = static_cast<std::uint8_t>(n);
    } else if (t.find('=') != std::string_view::npos) {
        PendingCond c;
        if (!parseCond(t, c)) return false;
        out.fact = storyFactSlot(reg, c.id);
        out.op = c.field == 0xFF ? RuleOp::Equals : RuleOp::Field;
        out.field = c.field == 0xFF ? 0 : c.field;
        out.value = c.value;
        if (out.fact == 0) return false;
        const FactRow& row = reg.facts[out.fact];
        if (out.op == RuleOp::Field) return row.kind == FactKind::Packed && out.field < packedFields(row.max) && out.value <= 2;
        return inRange(row, out.value);
    } else {
        out.op = RuleOp::Set;
    }
    out.fact = storyFactSlot(reg, id);
    return out.fact != 0;
}

void readDerived(const JsonValue& root, FactRegistry& reg, JsonErrors& je) {
    const JsonValue ds = root["derived"];
    if (ds.isAbsent()) return;
    if (!ds.isArray() || ds.size() > kMaxDerived) {
        je.add("derived", "expected an array of up to 16 values");
        return;
    }
    static constexpr std::array<std::pair<std::string_view, DeriveKind>, 6> kKinds{{
        {"value", DeriveKind::Value}, {"read", DeriveKind::Read}, {"in", DeriveKind::In},
        {"upgrade", DeriveKind::Upgrade}, {"topKind", DeriveKind::TopKind}, {"pressTotal", DeriveKind::PressTotal},
    }};
    for (std::uint32_t i = 0; i < ds.size(); ++i) {
        const std::string at = "derived[" + std::to_string(i) + "]";
        const JsonValue d = ds[i];
        DeriveRow row;
        const std::string_view kind = d["kind"].isString() ? d["kind"].string() : std::string_view();
        for (const auto& [name, k] : kKinds)
            if (kind == name) row.kind = k;
        if (row.kind == DeriveKind::None) {
            je.add(at + ".kind", "expected value, read, in, upgrade, topKind or pressTotal");
            continue;
        }
        if (row.kind != DeriveKind::TopKind && row.kind != DeriveKind::PressTotal) {
            factRole(d["fact"], reg, row.fact, at + ".fact", je);
            if (row.fact == 0) je.add(at + ".fact", "required");
        }
        if (row.kind == DeriveKind::In) {
            const JsonValue vs = d["values"];
            if (!vs.isArray() || vs.size() == 0) je.add(at + ".values", "expected an array of values 0..31");
            for (std::uint32_t k = 0; k < vs.size(); ++k) {
                if (!vs[k].isInteger() || vs[k].integer() < 0 || vs[k].integer() > 31) je.add(at + ".values", "expected 0..31");
                else row.mask |= 1u << vs[k].integer();
            }
        }
        if (row.kind == DeriveKind::Upgrade) {
            const JsonValue from = d["from"], to = d["to"], cond = d["if"];
            const bool ok = from.isInteger() && to.isInteger() && row.fact != 0 &&
                            inRange(reg.facts[row.fact], static_cast<std::uint8_t>(from.integer(0) & 0xFF)) &&
                            from.integer() >= 1 && from.integer() <= 254 && to.integer() >= 1 && to.integer() <= 254;
            if (!ok) je.add(at, "upgrade: expected from and to, values of the fact");
            row.from = static_cast<std::uint8_t>(from.integer(0) & 0xFF);
            row.to = static_cast<std::uint8_t>(to.integer(0) & 0xFF);
            PendingCond c;
            const std::uint16_t f = cond.isString() && parseCond(cond.string(), c) ? storyFactSlot(reg, c.id) : 0;
            if (f == 0 || c.field != 0xFF || !inRange(reg.facts[f], c.value)) je.add(at + ".if", "expected id=V");
            else row.cond = FactCond{f, 0xFF, c.value};
        }
        reg.derived[reg.derivedCount++] = row;
    }
}

void readRules(const JsonValue& root, FactRegistry& reg, JsonErrors& je) {
    const JsonValue rs = root["rules"];
    if (rs.isAbsent()) return;
    if (!rs.isArray() || rs.size() > kMaxStoryRules) {
        je.add("rules", "expected an array of up to 32 rules");
        return;
    }
    for (std::uint32_t i = 0; i < rs.size(); ++i) {
        const std::string at = "rules[" + std::to_string(i) + "]";
        const JsonValue r = rs[i];
        StoryRule rule;
        const JsonValue num = r["number"];
        if (!num.isInteger() || num.integer() < 1 || num.integer() > 255) je.add(at + ".number", "expected 1..255");
        rule.number = static_cast<std::uint8_t>(num.integer(0) & 0xFF);
        const std::string_view kind = r["kind"].isString() ? r["kind"].string() : std::string_view();
        if (kind == "bitsInOrder") {
            rule.kind = RuleKind::BitsInOrder;
            factRole(r["fact"], reg, rule.when.fact, at + ".fact", je);
            if (rule.when.fact == 0 || reg.facts[rule.when.fact].kind != FactKind::Bits)
                je.add(at + ".fact", "expected a BITS fact");
        } else if (kind == "needs") {
            rule.kind = RuleKind::Needs;
            rule.settled = r["settled"].isBool() && r["settled"].boolean();
            const JsonValue w = r["when"], t = r["then"];
            if (!w.isString() || !parseRuleCond(reg, w.string(), rule.when)) je.add(at + ".when", "expected id, id=V, id.fN=V or id&M");
            if (!t.isString() || !parseRuleCond(reg, t.string(), rule.then)) je.add(at + ".then", "expected id, id=V, id.fN=V or id&M");
        } else {
            je.add(at + ".kind", "expected needs or bitsInOrder");
            continue;
        }
        reg.rules[reg.ruleCount++] = rule;
    }
}

void joinErrors(const std::vector<std::string>& errors, std::string* error) {
    if (!error) return;
    error->clear();
    for (const std::string& e : errors) {
        if (!error->empty()) *error += '\n';
        *error += e;
    }
}

} // namespace

bool importStoryRegistry(std::string_view csv, std::string_view json, FactRegistry& out, std::string* error) {
    out = FactRegistry{};
    FactRegistry& reg = out;
    std::vector<std::string> errors;
    JsonErrors je{errors};
    JsonDocument doc;
    std::string parseError;
    if (json.empty()) {
        errors.push_back("story_tuning.json: missing (the csv needs it)");
    } else if (!doc.parse(json, &parseError)) {
        errors.push_back("story_tuning.json: " + parseError);
    } else if (!doc.root().isObject()) {
        errors.push_back("story_tuning.json: expected an object");
    } else if (const JsonValue v = doc.root()["version"]; !(v.isInteger() && v.integer() == 2)) {
        je.add("version", "expected 2");
    } else {
        readScenes(doc.root(), reg, je);
    }
    if (csv.empty()) errors.push_back("story_facts.csv: missing (the json needs it)");
    else if (errors.empty()) importCsv(csv, reg, errors);
    if (errors.empty()) {
        const JsonValue root = doc.root();
        readRoles(root, reg, je);
        readPressFloor(root, reg, je);
        readTrack(root, reg, je);
        readMilestones(root, reg, je);
        readDerived(root, reg, je);
        readRules(root, reg, je);
    }
    if (!errors.empty()) {
        out = FactRegistry{};
        joinErrors(errors, error);
        return false;
    }
    // FNV-1a 32 over the scene codes, then the ids in slot order (facts, then wide), each ended by a zero byte.
    std::uint32_t h = 2166136261u;
    auto mix = [&h](std::string_view id) {
        for (char c : id) h = (h ^ static_cast<std::uint8_t>(c)) * 16777619u;
        h = (h ^ 0u) * 16777619u;
    };
    for (std::uint8_t k = 0; k < reg.sceneCount; ++k) mix(storySceneCode(reg, k));
    for (const FactRow& r : reg.facts)
        if (r.used) mix(r.id());
    for (const WideRow& w : reg.wide)
        if (w.kind != WideKind::None) mix(w.id());
    reg.hash = h == 0 ? 1u : h; // 0 means no registry
    if (error) error->clear();
    return true;
}

namespace {
std::string_view trimField(std::string_view v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.remove_suffix(1);
    return v;
}
} // namespace

namespace {
// ASCII only: ids are ASCII.
bool sameIgnoringCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        const char y = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
        if (x != y) return false;
    }
    return true;
}
} // namespace

bool importStorySceneMap(std::string_view csv, const FactRegistry& reg, std::vector<StorySceneLink>& out,
                         std::string* error) {
    out.clear();
    auto fail = [&](std::size_t line, const char* why) {
        out.clear();
        if (error) *error = "line " + std::to_string(line) + ": " + why;
        return false;
    };
    if (csv.size() >= 3 && static_cast<unsigned char>(csv[0]) == 0xEF && static_cast<unsigned char>(csv[1]) == 0xBB &&
        static_cast<unsigned char>(csv[2]) == 0xBF)
        csv.remove_prefix(3);
    bool header = false;
    std::size_t line = 0;
    while (!csv.empty()) {
        const std::size_t nl = csv.find('\n');
        std::string_view row = trimField(csv.substr(0, nl));
        csv = nl == std::string_view::npos ? std::string_view() : csv.substr(nl + 1);
        ++line;
        if (row.empty() || row.front() == '#') continue;
        const std::size_t comma = row.find(',');
        if (comma == std::string_view::npos || row.find(',', comma + 1) != std::string_view::npos)
            return fail(line, "expected two fields: scene,code");
        const std::string_view scene = trimField(row.substr(0, comma));
        const std::string_view code = trimField(row.substr(comma + 1));
        if (!header) {
            if (scene != "scene" || code != "code") return fail(line, "expected the header scene,code");
            header = true;
            continue;
        }
        if (scene.empty() || code.empty()) return fail(line, "empty field");
        const std::uint8_t ordinal = storySceneOrdinal(reg, code);
        if (ordinal == kNoScene) {
            for (std::uint8_t i = 0; i < reg.sceneCount; ++i)
                if (sameIgnoringCase(code, std::string_view(reg.sceneCodes[i].data())))
                    return fail(line, "the scene code differs from the registry's only by case (codes are case-sensitive)");
            return fail(line, "the registry lists no such scene code");
        }
        for (const StorySceneLink& l : out) {
            if (l.scene == scene) return fail(line, "scene id given twice");
            if (sameIgnoringCase(l.scene, scene)) return fail(line, "scene ids that differ only by case");
        }
        out.push_back(StorySceneLink{std::string(scene), ordinal});
    }
    if (!header) return fail(line, "expected the header scene,code");
    return true;
}

StorySceneStep storyEnterScene(StoryState& s, const FactRegistry& reg, std::uint8_t scene) {
    StorySceneStep step;
    if (s.openScene != kNoScene && s.openScene == scene) {
        step.restarted = storyRestartScene(s, reg);
        step.refused = !step.restarted;
        return step;
    }
    if (s.openScene != kNoScene) {
        step.committed = storyCommitScene(s, reg);
        if (!step.committed) {
            step.refused = true;
            return step; // the open scene stays open: nothing begins over it
        }
    }
    if (scene != kNoScene) {
        step.began = storyBeginScene(s, reg, scene);
        step.refused = !step.began;
    }
    return step;
}

} // namespace ftcore
