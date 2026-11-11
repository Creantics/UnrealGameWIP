#include "ftcore/NpcMemory.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <string>

namespace ftcore {

namespace {

constexpr std::uint8_t kFlagSecret = 1, kFlagStreet = 2, kFlagKnown = kFlagSecret | kFlagStreet;

std::uint32_t satAdd(std::uint32_t a, std::uint32_t b) { return a > 0xFFFFFFFFu - b ? 0xFFFFFFFFu : a + b; }

std::int32_t clampi(std::int32_t v, std::int32_t lo, std::int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
std::int32_t axisLo(int a) { return a == kAxisF ? 0 : -100; }

std::int32_t decayToward(std::int32_t as, std::int32_t b, std::uint32_t ts, std::uint32_t t, std::int32_t rate) {
    if (t <= ts || as == b) return as;
    const std::int64_t gap = as > b ? as - b : b - as;
    const std::int64_t d = gap - static_cast<std::int64_t>(rate) * static_cast<std::int64_t>(t - ts) / 1440;
    if (d <= 0) return b;
    return as > b ? b + static_cast<std::int32_t>(d) : b - static_cast<std::int32_t>(d);
}

std::uint32_t readTime(const NpcMemoryWorld& w, std::uint16_t n) {
    const NpcState& s = w.npcs[n];
    return s.dead ? std::min(w.now, s.deadAt) : w.now;
}

bool validNpc(const NpcMemoryWorld& w, std::uint16_t n) { return n < w.npcCount; }

std::int32_t kFor(const NpcTuning& tu, NpcLearn how) {
    switch (how) {
    case NpcLearn::Victim: return tu.kVictim;
    case NpcLearn::Witness: return tu.kWitness;
    case NpcLearn::Hop1: return tu.kHop1;
    case NpcLearn::Hop2: return tu.kHop2;
    }
    return 0;
}

std::int32_t tieTo(const NpcTables& t, std::uint16_t from, std::uint16_t to) {
    if (from == to) return 100;
    for (const NpcTie& e : t.ties)
        if (e.kind == NpcTieKind::Npc && e.from == from && e.to == to) return e.tie;
    return 0;
}

std::int32_t closenessOf(const NpcTables& t, std::uint16_t a, std::uint16_t b, std::uint16_t* delay) {
    std::int32_t best = -1;
    for (const NpcTie& e : t.ties) {
        if (e.kind != NpcTieKind::Npc) continue;
        if ((e.from == a && e.to == b) || (e.from == b && e.to == a)) {
            if (e.closeness > best) {
                best = e.closeness;
                if (delay) *delay = e.delayMin;
            }
        }
    }
    return best;
}

std::uint32_t typeMask(const NpcTables& t, std::uint16_t type) {
    return type < t.deeds.size() ? t.deeds[type].tagMask : 0u;
}

std::int32_t salienceAt(const NpcTuning& tu, const NpcMemoryEntry& e, std::uint32_t at) {
    if (e.pinned || at <= e.ts) return e.s0;
    const std::int64_t s = e.s0 - static_cast<std::int64_t>(tu.salienceDecayPerHour) * ((at - e.ts) / 60);
    return s < 0 ? 0 : static_cast<std::int32_t>(s);
}

void updateStances(NpcMemoryWorld& w, const NpcTables& t, std::uint16_t n) {
    NpcState& s = w.npcs[n];
    for (std::uint32_t l = 0; l <= kLeadCount; ++l) {
        const std::uint8_t lead = l == kLeadCount ? kNoLead : static_cast<std::uint8_t>(l);
        const auto e = npcEffective(w, t, n, lead);
        s.stance[l] = npcStanceStep(s.stance[l], e[0], e[1], e[2], t.tuning.stayGap);
    }
}

// Rk's k: how many times this NPC learned the same type within the window before now.
std::int32_t repeatCount(const NpcMemoryWorld& w, const NpcTables& t, std::uint16_t n, std::uint16_t type) {
    const NpcState& s = w.npcs[n];
    std::int32_t k = 0;
    for (const NpcRecent& r : s.recent) {
        if (r.type != type) continue;
        for (std::uint32_t i = 0; i < r.count; ++i)
            if (w.now - std::min(w.now, r.minute[i]) < static_cast<std::uint32_t>(t.tuning.rkWindowMin)) ++k;
        break;
    }
    return k;
}

void noteLearn(NpcState& s, std::uint16_t type, std::uint32_t minute) {
    NpcRecent* slot = nullptr;
    NpcRecent* stalest = &s.recent[0];
    for (NpcRecent& r : s.recent) {
        if (r.type == type) {
            slot = &r;
            break;
        }
        if (r.type == kNoType) {
            if (!slot) slot = &r;  // the first free one, unless the type is further on
            continue;
        }
        if (stalest->type != kNoType && r.minute[r.count - 1u] < stalest->minute[stalest->count - 1u]) stalest = &r;
    }
    if (!slot) slot = stalest;
    if (slot->type != type) *slot = NpcRecent{type, 0, {}};
    if (slot->count == kRepeatKeep) {
        for (std::uint32_t i = 1; i < kRepeatKeep; ++i) slot->minute[i - 1u] = slot->minute[i];
        --slot->count;
    }
    slot->minute[slot->count++] = minute;
}

void setSeen(NpcState& s, std::uint32_t seq, bool on) {
    const std::uint32_t slot = seq % kDeedRing, bit = 1u << (slot % 32u);
    std::uint32_t& word = s.seen[slot / 32u];
    word = on ? (word | bit) : (word & ~bit);
}

std::array<std::int32_t, 3> appraise(const NpcMemoryWorld& w, const NpcTables& t, std::uint16_t n, const NpcDeed& d,
                                     NpcLearn how, std::int32_t kOverride) {
    std::array<std::int32_t, 3> out{0, 0, 0};
    if (!validNpc(w, n) || d.type >= t.deeds.size() || n >= t.npcs.size()) return out;
    const NpcTuning& tu = t.tuning;
    const NpcDeedType& dt = t.deeds[d.type];
    const NpcDef& def = t.npcs[n];
    const bool hop = how == NpcLearn::Hop1 || how == NpcLearn::Hop2;
    const std::int32_t tie = (!hop && d.victim != kNoNpc) ? tieTo(t, n, d.victim) : 0;
    const std::int32_t K = kOverride > 0 ? kOverride : kFor(tu, how);
    const std::int32_t k = repeatCount(w, t, n, d.type);
    const std::int32_t Rk = std::max(tu.rkFloor, 100 - tu.rkStep * k);
    for (int a = 0; a < 3; ++a) {
        std::int32_t S = 0;
        for (std::uint32_t b = 0; b < kNpcTagMax; ++b)
            if (dt.tagMask & (1u << b)) S += def.weights[b][static_cast<std::size_t>(a)];
        S += dt.polarity * tie * tu.tieGain[static_cast<std::size_t>(a)] / 100;
        S = clampi(S, -12, 12);
        const std::int32_t delta = dt.magnitude * S * K * Rk / 10000;
        const std::int32_t A = npcAxis(w, t, n, static_cast<NpcAxis>(a));
        const bool away = a == kAxisF ? delta > 0 : ((A >= 0 && delta > 0) || (A <= 0 && delta < 0));
        const std::int32_t absA = A < 0 ? -A : A;
        out[static_cast<std::size_t>(a)] = away ? delta * (100 - absA) / 100 : delta;
    }
    return out;
}

void evictOne(NpcState& s, const NpcTables& t, std::uint32_t now) {
    int victim = -1;
    std::int32_t bestS = 0;
    for (std::uint32_t i = 0; i < s.memCount; ++i) {
        const NpcMemoryEntry& e = s.mem[i];
        if (e.pinned) continue;
        const std::int32_t S = salienceAt(t.tuning, e, now);
        if (victim < 0) {
            victim = static_cast<int>(i);
            bestS = S;
            continue;
        }
        const NpcMemoryEntry& b = s.mem[static_cast<std::size_t>(victim)];
        if (S < bestS || (S == bestS && (e.ts < b.ts || (e.ts == b.ts && e.seq < b.seq)))) {
            victim = static_cast<int>(i);
            bestS = S;
        }
    }
    if (victim < 0) {  // every entry pinned (pins cap below the store, so only a forged state): drop the weakest, oldest
        for (std::uint32_t i = 0; i < s.memCount; ++i) {
            const NpcMemoryEntry& e = s.mem[i];
            const NpcMemoryEntry* b = victim < 0 ? nullptr : &s.mem[static_cast<std::size_t>(victim)];
            if (!b || e.s0 < b->s0 || (e.s0 == b->s0 && (e.ts < b->ts || (e.ts == b->ts && e.seq < b->seq)))) victim = static_cast<int>(i);
        }
    }
    const NpcMemoryEntry& gone = s.mem[static_cast<std::size_t>(victim)];
    const std::uint32_t mask = typeMask(t, gone.type);
    for (std::uint32_t b = 0; b < kNpcTagMax; ++b) {
        if (!(mask & (1u << b))) continue;
        NpcTagAggregate& ag = s.aggregates[b];
        ag.count = static_cast<std::uint16_t>(std::min<std::uint32_t>(0xFFFFu, ag.count + std::max<std::uint32_t>(1u, gone.count)));
        ag.lastMinute = std::max(ag.lastMinute, gone.deedMinute);
    }
    s.mem[static_cast<std::size_t>(victim)] = s.mem[s.memCount - 1u];
    s.mem[s.memCount - 1u] = NpcMemoryEntry{};
    --s.memCount;
}

void pin(NpcState& s, NpcMemoryEntry& e, std::uint32_t now) {
    if (e.pinned) return;
    std::uint32_t pinned = 0;
    int oldest = -1;
    for (std::uint32_t i = 0; i < s.memCount; ++i) {
        if (!s.mem[i].pinned) continue;
        ++pinned;
        if (oldest < 0 || s.mem[i].ts < s.mem[static_cast<std::size_t>(oldest)].ts) oldest = static_cast<int>(i);
    }
    if (pinned >= kPinnedMax && oldest >= 0) {
        NpcMemoryEntry& o = s.mem[static_cast<std::size_t>(oldest)];
        o.pinned = false;
        o.ts = now;  // fades from now on
    }
    e.pinned = true;
}

bool knows(const NpcState& s, std::uint32_t seq) {
    const std::uint32_t slot = seq % kDeedRing;
    if (s.seen[slot / 32u] & (1u << (slot % 32u))) return true;
    for (std::uint32_t i = 0; i < s.memCount; ++i)
        if (s.mem[i].seq == seq) return true;
    return false;
}

// One NPC learns a deed now. Returns false when nothing was learned.
bool learn(NpcMemoryWorld& w, const NpcTables& t, std::uint16_t n, const NpcDeed& d, NpcLearn how, std::uint8_t lead,
           std::int32_t closeness, std::int32_t kOverride) {
    if (!validNpc(w, n) || n >= t.npcs.size() || d.type >= t.deeds.size()) return false;
    NpcState& s = w.npcs[n];
    if (s.dead || knows(s, d.seq)) return false;
    updateStances(w, t, n);  // store any stance change decay made since the last write
    const NpcTuning& tu = t.tuning;
    const NpcDeedType& dt = t.deeds[d.type];
    const auto dp = appraise(w, t, n, d, how, kOverride);
    noteLearn(s, d.type, w.now);
    setSeen(s, d.seq, true);

    std::array<std::int16_t, 3> next{};
    for (int a = 0; a < 3; ++a)
        next[static_cast<std::size_t>(a)] = static_cast<std::int16_t>(
            clampi(npcAxis(w, t, n, static_cast<NpcAxis>(a)) + dp[static_cast<std::size_t>(a)], axisLo(a), 100));
    s.axes = next;
    s.axesTs = w.now;

    const bool attributable = dt.attributable && lead < kLeadCount &&
                              (how == NpcLearn::Victim || how == NpcLearn::Witness ||
                               (how == NpcLearn::Hop1 && closeness >= tu.leadCloseness));
    if (attributable) {
        std::array<std::int8_t, 3> off{};
        for (int a = 0; a < 3; ++a)
            off[static_cast<std::size_t>(a)] = static_cast<std::int8_t>(
                clampi(npcOffset(w, t, n, lead, static_cast<NpcAxis>(a)) + dp[static_cast<std::size_t>(a)] / 2, -30, 30));
        s.offsets[lead] = off;
        s.offsetsTs[lead] = w.now;
    }

    const std::int32_t K = kOverride > 0 ? kOverride : kFor(tu, how);
    const std::int32_t sumAbs = (dp[0] < 0 ? -dp[0] : dp[0]) + (dp[1] < 0 ? -dp[1] : dp[1]) + (dp[2] < 0 ? -dp[2] : dp[2]);
    const std::int32_t sNew = clampi(dt.magnitude * 150 * K / 100 + 10 * sumAbs, 50, 1000);
    const std::uint8_t memLead = (how == NpcLearn::Victim || how == NpcLearn::Witness || closeness >= tu.leadCloseness) ? lead : kNoLead;

    NpcMemoryEntry* e = nullptr;
    for (std::uint32_t i = 0; i < s.memCount; ++i)
        if (s.mem[i].type == d.type && s.mem[i].place == d.place) e = &s.mem[i];
    if (e) {
        e->s0 = static_cast<std::int16_t>(std::min(1000, salienceAt(tu, *e, w.now) + sNew / 2));
        e->ts = w.now;
        e->count = static_cast<std::uint8_t>(std::min(255, e->count + 1));
        e->seq = d.seq;
        e->deedMinute = d.minute;
        e->hops = std::min(e->hops, static_cast<std::uint8_t>(how));
        if (memLead < kLeadCount) {
            e->lead = memLead;
            e->leads = static_cast<std::uint8_t>(e->leads | (1u << memLead));
        }
    } else {
        if (s.memCount >= kMemPerNpc) evictOne(s, t, w.now);
        e = &s.mem[s.memCount++];
        *e = NpcMemoryEntry{};
        e->seq = d.seq;
        e->deedMinute = d.minute;
        e->ts = w.now;
        e->learned = w.now;
        e->type = d.type;
        e->place = d.place;
        e->s0 = static_cast<std::int16_t>(sNew);
        e->hops = static_cast<std::uint8_t>(how);
        e->count = 1;
        e->lead = memLead;
        e->leads = memLead < kLeadCount ? static_cast<std::uint8_t>(1u << memLead) : 0;
    }
    // Pins are for what the NPC lived through or was told outright; hearsay of a pinned deed fades like any other.
    if (dt.pin && (how == NpcLearn::Victim || how == NpcLearn::Witness)) pin(s, *e, w.now);
    updateStances(w, t, n);
    return true;
}

// Queues rumours from an NPC that just learned a deed at `fromHops` (0 first hand, 1 hop1).
void queueRumours(NpcMemoryWorld& w, const NpcTables& t, std::uint16_t from, const NpcDeed& d, std::uint8_t fromHops, std::uint8_t lead) {
    if (fromHops >= 2 || !validNpc(w, from) || w.npcs[from].dead || d.type >= t.deeds.size()) return;
    const std::int32_t m = t.deeds[d.type].magnitude;
    for (const NpcTie& tie : t.ties) {
        if (tie.kind != NpcTieKind::Npc) continue;
        std::uint16_t to = kNoNpc;
        if (tie.from == from) to = tie.to;
        else if (tie.to == from) to = tie.from;
        if (to == kNoNpc || to == from || !validNpc(w, to) || w.npcs[to].dead || knows(w.npcs[to], d.seq)) continue;
        std::uint16_t delay = tie.delayMin;
        const std::int32_t close = closenessOf(t, from, to, &delay);
        if (close != tie.closeness) continue;  // the closest tie between the pair speaks once
        if (close * m < t.tuning.spreadThreshold) continue;
        bool queued = false;
        for (std::uint32_t i = 0; i < w.rumourCount; ++i)
            if (w.rumours[i].seq == d.seq && w.rumours[i].target == to) queued = true;
        if (queued) continue;
        if (w.rumourCount >= kRumourQueue) {
            ++w.droppedRumours;
            continue;
        }
        NpcRumour& r = w.rumours[w.rumourCount++];
        r.seq = d.seq;
        r.due = satAdd(w.now, delay);
        r.target = to;
        r.hops = static_cast<std::uint8_t>(fromHops + 1);
        r.lead = close >= t.tuning.leadCloseness ? lead : kNoLead;
        r.closeness = static_cast<std::uint8_t>(close);
    }
}

std::uint32_t recordDeed(NpcMemoryWorld& w, const NpcTables& t, NpcDeed d, bool checkExpectations, bool fromExp, bool quiet);

// An expectation's outcome reaches the NPC it was made to (even when the deed that settled it was
// secret), never the street; `quiet` keeps it from spreading further until it is revealed.
void emitExpDeed(NpcMemoryWorld& w, const NpcTables& t, std::uint32_t expIdx, std::uint16_t type, std::uint8_t lead, bool quiet) {
    if (type == kNoType || type >= t.deeds.size()) return;
    NpcDeed d;
    d.type = type;
    d.victim = t.expectations[expIdx].npc;
    d.lead = lead;
    d.place = static_cast<std::uint16_t>(0xFFFFu - expIdx);
    d.district = 0;
    recordDeed(w, t, d, false, true, quiet);
}

std::uint32_t recordDeed(NpcMemoryWorld& w, const NpcTables& t, NpcDeed d, bool checkExpectations, bool fromExp, bool quiet) {
    if (d.type >= t.deeds.size()) return 0;
    if (d.minute == 0) d.minute = w.now;
    d.seq = w.nextSeq++;
    const NpcDeedType& dt = t.deeds[d.type];
    std::uint32_t witnesses = 0;
    for (std::uint16_t x : d.witnesses)
        if (x != kNoNpc && validNpc(w, x)) ++witnesses;
    const bool secret = dt.secret && witnesses == 0;
    const bool hold = secret || quiet;
    d.flags = static_cast<std::uint8_t>((hold ? kFlagSecret : 0) | (!hold && !fromExp && witnesses == 0 ? kFlagStreet : 0));
    const std::uint32_t slot = d.seq % kDeedRing;
    if (const std::uint32_t old = w.ring[slot].seq)  // the old deed is gone
        for (std::uint16_t n = 0; n < w.npcCount; ++n) setSeen(w.npcs[n], old, false);
    w.ring[slot] = d;

    // Everyone told first hand learns before anyone spreads, so no rumour races a direct learner.
    std::array<std::uint16_t, 1 + kDeedWitnessMax> learners{};
    std::uint32_t count = 0;
    if (d.victim != kNoNpc && (!secret || fromExp) && learn(w, t, d.victim, d, NpcLearn::Victim, d.lead, 100, 0))
        learners[count++] = d.victim;
    for (std::uint16_t x : d.witnesses)
        if (x != kNoNpc && learn(w, t, x, d, NpcLearn::Witness, d.lead, 100, 0)) learners[count++] = x;
    if (!hold)
        for (std::uint32_t i = 0; i < count; ++i) queueRumours(w, t, learners[i], d, 0, d.lead);

    if (checkExpectations) {
        for (std::uint32_t i = 0; i < t.expectations.size() && i < kExpMax; ++i) {
            const NpcExpDef& e = t.expectations[i];
            NpcExpState& st = w.exp[i];
            if (st != NpcExpState::Pending && st != NpcExpState::Reminding) continue;
            if (std::uint64_t{d.minute} > std::uint64_t{e.due} + e.windowMin) continue;
            if (d.type == e.keptBy) {
                st = NpcExpState::Kept;
                emitExpDeed(w, t, i, e.deedKept, d.lead, secret);
            } else if (d.type == e.brokenBy) {
                st = NpcExpState::Broken;
                emitExpDeed(w, t, i, e.deedBroken, d.lead, secret);
            }
        }
    }
    return d.seq;
}

// Who did it, against the lead being spoken to (NpcCond::by).
bool byMatches(std::uint8_t leads, std::int8_t by, std::uint8_t lead) {
    if (by < 0) return true;
    const std::uint32_t self = lead < kLeadCount ? (1u << lead) : 0u;
    if (by == kBySelf) return (leads & self) != 0;
    if (by == kByOther) return (leads & ~self & ((1u << kLeadCount) - 1u)) != 0;
    return by < static_cast<std::int8_t>(kLeadCount) && (leads & (1u << by)) != 0;
}

const NpcMemoryEntry* recallAt(const NpcMemoryWorld& w, const NpcTables& t, std::uint16_t n, int type, int tag,
                               std::int32_t* sOut, std::int8_t by = -1, std::uint8_t lead = kNoLead) {
    if (!validNpc(w, n) || tag >= static_cast<int>(kNpcTagMax)) return nullptr;
    const NpcState& s = w.npcs[n];
    const std::uint32_t at = readTime(w, n);
    const NpcMemoryEntry* best = nullptr;
    std::int32_t bestS = -1;
    for (std::uint32_t i = 0; i < s.memCount; ++i) {
        const NpcMemoryEntry& e = s.mem[i];
        const bool match = tag >= 0 ? (typeMask(t, e.type) & (1u << tag)) != 0 : e.type == type;
        if (!match || !byMatches(e.leads, by, lead)) continue;
        const std::int32_t S = salienceAt(t.tuning, e, at);
        if (S > bestS) {
            bestS = S;
            best = &e;
        }
    }
    if (sOut) *sOut = bestS;
    return best;
}

// Latest deedMinute among memories of a type or tag; -1 when none.
std::int64_t latestOf(const NpcMemoryWorld& w, const NpcTables& t, std::uint16_t n, int type, int tag) {
    if (!validNpc(w, n) || tag >= static_cast<int>(kNpcTagMax)) return -1;
    const NpcState& s = w.npcs[n];
    std::int64_t best = -1;
    for (std::uint32_t i = 0; i < s.memCount; ++i) {
        const NpcMemoryEntry& e = s.mem[i];
        const bool match = tag >= 0 ? (typeMask(t, e.type) & (1u << tag)) != 0 : e.type == type;
        if (match && static_cast<std::int64_t>(e.deedMinute) > best) best = e.deedMinute;
    }
    return best;
}

}  // namespace

int NpcTables::tagIndex(std::string_view id) const {
    for (std::size_t i = 0; i < tags.size(); ++i)
        if (tags[i] == id) return static_cast<int>(i);
    return -1;
}
int NpcTables::deedIndex(std::string_view id) const {
    for (std::size_t i = 0; i < deeds.size(); ++i)
        if (deeds[i].id == id) return static_cast<int>(i);
    return -1;
}
int NpcTables::npcIndex(std::string_view id) const {
    for (std::size_t i = 0; i < npcs.size(); ++i)
        if (npcs[i].id == id) return static_cast<int>(i);
    return -1;
}
int NpcTables::expIndex(std::string_view id) const {
    for (std::size_t i = 0; i < expectations.size(); ++i)
        if (expectations[i].id == id) return static_cast<int>(i);
    return -1;
}
int NpcTables::eventIndex(std::string_view id) const {
    for (std::size_t i = 0; i < events.size(); ++i)
        if (events[i].id == id) return static_cast<int>(i);
    return -1;
}
int NpcLineTable::lineIndex(std::string_view id) const {
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (lines[i].id == id) return static_cast<int>(i);
    return -1;
}

void npcInit(NpcMemoryWorld& w, const NpcTables& t) {
    // Cleared in place, field by field: `w = NpcMemoryWorld{}` would build the whole world (see the size check in
    // NpcMemory.h) as a temporary on the stack first. A field added to NpcMemoryWorld must be cleared here too.
    w.now = 0;
    w.nextSeq = 1;
    w.npcCount = 0;
    for (NpcState& s : w.npcs) s = NpcState{};
    w.ring.fill(NpcDeed{});
    w.rumours.fill(NpcRumour{});
    w.rumourCount = 0;
    w.lastSaid.fill(0u);
    w.droppedRumours = 0;
    w.npcCount = static_cast<std::uint16_t>(std::min<std::size_t>(t.npcs.size(), kNpcMax));
    for (std::uint16_t i = 0; i < w.npcCount; ++i) {
        w.npcs[i].axes = t.npcs[i].base;
        updateStances(w, t, i);
    }
    for (auto& e : w.exp) e = NpcExpState::Pending;
}

void npcAdvance(NpcMemoryWorld& w, std::uint32_t minute) { w.now = std::max(w.now, minute); }

std::uint32_t npcRecordDeed(NpcMemoryWorld& w, const NpcTables& t, NpcDeed deed) { return recordDeed(w, t, deed, true, false, false); }

void npcCheckpoint(NpcMemoryWorld& w, const NpcTables& t, std::uint32_t now, std::uint16_t crowdPermille) {
    npcAdvance(w, now);
    const NpcTuning& tu = t.tuning;

    // Rumours: deliver the due ones present at the start (new hop2 rumours wait their delay).
    const std::uint32_t start = w.rumourCount;
    for (std::uint32_t i = 0; i < start; ++i) {
        NpcRumour r = w.rumours[i];
        if (r.due > w.now) continue;
        w.rumours[i].target = kNoNpc;  // consumed
        const NpcDeed* d = npcDeedAt(w, r.seq);
        if (!d || !validNpc(w, r.target)) continue;
        const NpcDeed deed = *d;
        const NpcLearn how = r.hops == 1 ? NpcLearn::Hop1 : NpcLearn::Hop2;
        if (learn(w, t, r.target, deed, how, r.lead, r.closeness, 0)) queueRumours(w, t, r.target, deed, r.hops, r.lead);
    }
    std::uint32_t keep = 0;
    for (std::uint32_t i = 0; i < w.rumourCount; ++i)
        if (w.rumours[i].target != kNoNpc) w.rumours[keep++] = w.rumours[i];
    for (std::uint32_t i = keep; i < w.rumourCount; ++i) w.rumours[i] = NpcRumour{};
    w.rumourCount = static_cast<std::uint16_t>(keep);

    // Street hearsay: unwitnessed deeds reach their district when the crowd is thick enough; a thin
    // crowd only delays them (they retry here until their ring slot is reused).
    for (NpcDeed& slot : w.ring) {
        if (!(slot.flags & kFlagStreet) || slot.seq == 0 || slot.type >= t.deeds.size()) continue;
        if (std::uint64_t{slot.minute} + static_cast<std::uint64_t>(tu.streetDelayMin) > w.now) continue;
        if (t.deeds[slot.type].magnitude * static_cast<std::int32_t>(crowdPermille) < tu.streetThreshold) continue;
        slot.flags = static_cast<std::uint8_t>(slot.flags & ~kFlagStreet);
        const NpcDeed deed = slot;
        std::array<std::uint16_t, kNpcMax> learners{};
        std::uint32_t count = 0;
        for (std::uint16_t n = 0; n < w.npcCount; ++n) {
            if (n == deed.victim || n >= t.npcs.size() || t.npcs[n].district != deed.district) continue;
            if (learn(w, t, n, deed, NpcLearn::Hop1, kNoLead, 0, 0)) learners[count++] = n;
        }
        for (std::uint32_t i = 0; i < count; ++i) queueRumours(w, t, learners[i], deed, 1, kNoLead);
    }

    // Expectations: a skip past the window goes straight to Missed (the reminder never plays).
    for (std::uint32_t i = 0; i < t.expectations.size() && i < kExpMax; ++i) {
        const NpcExpDef& e = t.expectations[i];
        NpcExpState& st = w.exp[i];
        if (st != NpcExpState::Pending && st != NpcExpState::Reminding) continue;
        if (std::uint64_t{w.now} > std::uint64_t{e.due} + e.windowMin) {
            st = NpcExpState::Missed;
            emitExpDeed(w, t, i, e.deedMissed, kNoLead, false);
        } else if (st == NpcExpState::Pending && std::uint64_t{w.now} + e.remindLeadMin >= e.due) {
            st = NpcExpState::Reminding;
        }
    }

    for (std::uint16_t n = 0; n < w.npcCount; ++n)
        if (!w.npcs[n].dead) updateStances(w, t, n);
}

bool npcReveal(NpcMemoryWorld& w, const NpcTables& t, std::uint32_t seq, std::uint16_t npc) {
    const NpcDeed* d = npcDeedAt(w, seq);
    if (!d) return false;
    const NpcDeed deed = *d;
    // The NPC is told now; one who already knew (the party to a quiet promise) may now pass it on.
    if (!learn(w, t, npc, deed, NpcLearn::Witness, deed.lead, 100, t.tuning.kReveal) &&
        !(validNpc(w, npc) && !w.npcs[npc].dead && knows(w.npcs[npc], seq)))
        return false;
    queueRumours(w, t, npc, deed, 0, deed.lead);
    return true;
}

void npcSetDead(NpcMemoryWorld& w, std::uint16_t npc) {
    if (!validNpc(w, npc) || w.npcs[npc].dead) return;
    w.npcs[npc].dead = true;
    w.npcs[npc].deadAt = w.now;
}

std::int32_t npcAxis(const NpcMemoryWorld& w, const NpcTables& t, std::uint16_t npc, NpcAxis a) {
    if (!validNpc(w, npc) || npc >= t.npcs.size()) return 0;
    const NpcState& s = w.npcs[npc];
    const auto ai = static_cast<std::size_t>(a);
    return decayToward(s.axes[ai], t.npcs[npc].base[ai], s.axesTs, readTime(w, npc), t.tuning.decayPerDay[ai]);
}

std::int32_t npcOffset(const NpcMemoryWorld& w, const NpcTables& t, std::uint16_t npc, std::uint8_t lead, NpcAxis a) {
    if (!validNpc(w, npc) || lead >= kLeadCount) return 0;
    const NpcState& s = w.npcs[npc];
    const auto ai = static_cast<std::size_t>(a);
    return decayToward(s.offsets[lead][ai], 0, s.offsetsTs[lead], readTime(w, npc), 2 * t.tuning.decayPerDay[ai]);
}

std::array<std::int32_t, 3> npcEffective(const NpcMemoryWorld& w, const NpcTables& t, std::uint16_t npc, std::uint8_t lead) {
    std::array<std::int32_t, 3> e{};
    for (int a = 0; a < 3; ++a)
        e[static_cast<std::size_t>(a)] = clampi(npcAxis(w, t, npc, static_cast<NpcAxis>(a)) +
                                                    npcOffset(w, t, npc, lead, static_cast<NpcAxis>(a)),
                                                axisLo(a), 100);
    return e;
}

NpcStance npcStanceStep(NpcStance current, std::int32_t T, std::int32_t F, std::int32_t R, std::int32_t g) {
    NpcStance cand = NpcStance::Neutral;
    if (T <= -50 && F < 60) cand = NpcStance::Hostile;
    else if (F >= 50 || T <= -20) cand = NpcStance::Wary;
    else if (T >= 60 && R >= 50 && F < 30) cand = NpcStance::Loyal;
    else if (T >= 25 && F < 40) cand = NpcStance::Warm;
    bool stay = false;
    switch (current) {
    case NpcStance::Hostile: stay = T <= -50 + g && F < 60 + g; break;
    case NpcStance::Wary: stay = F >= 50 - g || T <= -20 + g; break;
    case NpcStance::Loyal: stay = T >= 60 - g && R >= 50 - g && F < 30 + g; break;
    case NpcStance::Warm: stay = T >= 25 - g && F < 40 + g; break;
    case NpcStance::Neutral: stay = true; break;
    }
    const int cur = static_cast<int>(current), c = static_cast<int>(cand);
    if (stay && c >= std::min(cur, 2) && c <= std::max(cur, 2)) return current;
    return cand;
}

NpcStance npcStance(const NpcMemoryWorld& w, const NpcTables& t, std::uint16_t npc, std::uint8_t lead) {
    if (!validNpc(w, npc)) return NpcStance::Neutral;
    const std::size_t slot = lead < kLeadCount ? lead : kLeadCount;
    const auto e = npcEffective(w, t, npc, lead);
    return npcStanceStep(w.npcs[npc].stance[slot], e[0], e[1], e[2], t.tuning.stayGap);
}

NpcConsequence npcConsequence(const NpcMemoryWorld& w, const NpcTables& t, std::uint16_t npc, std::uint8_t lead) {
    const std::size_t i = static_cast<std::size_t>(npc) * 5u + static_cast<std::size_t>(npcStance(w, t, npc, lead));
    return i < t.consequences.size() ? t.consequences[i] : NpcConsequence{};
}

std::int32_t npcSalience(const NpcMemoryWorld& w, const NpcTables& t, std::uint16_t npc, const NpcMemoryEntry& e) {
    return salienceAt(t.tuning, e, validNpc(w, npc) ? readTime(w, npc) : w.now);
}

bool npcRemembers(const NpcMemoryWorld& w, const NpcTables& t, std::uint16_t npc, int type, int tag) {
    std::int32_t S = -1;
    return recallAt(w, t, npc, type, tag, &S) != nullptr && S >= t.tuning.recallBar;
}

const NpcMemoryEntry* npcFindMemory(const NpcMemoryWorld& w, std::uint16_t npc, std::uint32_t seq) {
    if (!validNpc(w, npc)) return nullptr;
    const NpcState& s = w.npcs[npc];
    for (std::uint32_t i = 0; i < s.memCount; ++i)
        if (s.mem[i].seq == seq) return &s.mem[i];
    return nullptr;
}

const NpcMemoryEntry* npcRecall(const NpcMemoryWorld& w, const NpcTables& t, std::uint16_t npc, int type, int tag) {
    return recallAt(w, t, npc, type, tag, nullptr);
}

const NpcDeed* npcDeedAt(const NpcMemoryWorld& w, std::uint32_t seq) {
    if (seq == 0) return nullptr;
    const NpcDeed& d = w.ring[seq % kDeedRing];
    return d.seq == seq ? &d : nullptr;
}

std::array<std::int32_t, 3> npcAppraise(const NpcMemoryWorld& w, const NpcTables& t, std::uint16_t npc, const NpcDeed& deed,
                                        NpcLearn how) {
    return appraise(w, t, npc, deed, how, 0);
}

NpcLinePick npcPickLine(NpcMemoryWorld& w, const NpcTables& t, const NpcLineTable& lines, std::uint16_t npc,
                        std::uint8_t lead, NpcLineKind kind) {
    NpcLinePick best;
    if (!validNpc(w, npc)) return best;
    const NpcStance stance = npcStance(w, t, npc, lead);
    const std::size_t n = std::min<std::size_t>(lines.lines.size(), kSaidMax);
    for (std::size_t i = 0; i < n; ++i) {
        const NpcLine& L = lines.lines[i];
        if (L.npc != npc || L.kind != kind) continue;
        if (L.lead != kNoLead && L.lead != lead) continue;
        const std::uint32_t last = w.lastSaid[i];
        if (last && L.once) continue;
        if (last && w.now - (last - 1u) < L.cooldownMin) continue;
        bool ok = true;
        std::int32_t sal = 0;
        for (const NpcCond& c : L.cond) {
            if (!ok) break;
            switch (c.kind) {
            case NpcCondKind::None: break;
            case NpcCondKind::Mem: {
                std::int32_t S = -1;
                ok = recallAt(w, t, npc, c.type, c.tag, &S, c.by, lead) != nullptr && S >= (c.value < 0 ? t.tuning.recallBar : c.value);
                if (ok) sal = std::max(sal, S);
                break;
            }
            case NpcCondKind::StanceGe: ok = static_cast<int>(stance) >= c.value; break;
            case NpcCondKind::StanceLe: ok = static_cast<int>(stance) <= c.value; break;
            case NpcCondKind::StanceEq: ok = static_cast<int>(stance) == c.value; break;
            case NpcCondKind::SinceGe:
            case NpcCondKind::SinceLe: {
                const std::int64_t at = latestOf(w, t, npc, c.type, c.tag);
                if (at < 0) {
                    ok = false;
                    break;
                }
                const std::int64_t since = static_cast<std::int64_t>(w.now) - at;
                ok = c.kind == NpcCondKind::SinceGe ? since >= c.value : since <= c.value;
                break;
            }
            case NpcCondKind::NotSince: {
                const std::int64_t x = latestOf(w, t, npc, c.type, c.tag);
                const std::int64_t y = latestOf(w, t, npc, c.other, -1);
                ok = x < 0 || y < x;
                break;
            }
            case NpcCondKind::Exp:
                ok = c.other >= 0 && static_cast<std::uint32_t>(c.other) < kExpMax &&
                     static_cast<int>(w.exp[static_cast<std::size_t>(c.other)]) == c.value;
                break;
            case NpcCondKind::EventLe: {
                ok = false;
                if (c.other >= 0 && static_cast<std::size_t>(c.other) < t.events.size()) {
                    const std::uint32_t at = t.events[static_cast<std::size_t>(c.other)].minute;
                    ok = at >= w.now && at - w.now <= static_cast<std::uint32_t>(c.value);
                }
                break;
            }
            case NpcCondKind::Dead:
                ok = c.other >= 0 && validNpc(w, static_cast<std::uint16_t>(c.other)) &&
                     w.npcs[static_cast<std::size_t>(c.other)].dead;
                break;
            }
        }
        if (!ok) continue;
        const std::int32_t score = L.priority * 1000 + sal;
        if (best.line < 0 || score > best.score) {
            best.line = static_cast<std::int32_t>(i);
            best.score = score;
        }
    }
    if (best.line >= 0) w.lastSaid[static_cast<std::size_t>(best.line)] = satAdd(w.now, 1u);
    return best;
}

// ---------------------------------------------------------------- loading

namespace {

using Row = std::vector<std::string>;

bool npcCsvSplit(std::string_view text, std::vector<Row>& rows) {
    rows.clear();
    Row row;
    std::string cell;
    bool quoted = false, any = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    cell += '"';
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                cell += c;
            }
            continue;
        }
        if (c == '"') {
            quoted = true;
            any = true;
        } else if (c == ',') {
            row.push_back(cell);
            cell.clear();
            any = true;
        } else if (c == '\r') {
        } else if (c == '\n') {
            if (any || !cell.empty()) {
                row.push_back(cell);
                rows.push_back(row);
            }
            row.clear();
            cell.clear();
            any = false;
        } else {
            cell += c;
            any = true;
        }
    }
    if (any || !cell.empty()) {
        row.push_back(cell);
        rows.push_back(row);
    }
    for (Row& r : rows)
        for (std::string& s : r) {
            std::size_t a = 0, b = s.size();
            while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
            s = s.substr(a, b - a);
        }
    return !quoted;
}

bool toInt(std::string_view s, std::int64_t& out) {
    if (!s.empty() && s[0] == '+') s.remove_prefix(1);
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

// "HH:MM" or a whole number of hours, as minutes into the day.
bool toClock(std::string_view s, std::int64_t& minutes) {
    const auto colon = s.find(':');
    std::int64_t h = 0, m = 0;
    if (colon == std::string_view::npos) {
        if (!toInt(s, h)) return false;
    } else if (!toInt(s.substr(0, colon), h) || !toInt(s.substr(colon + 1), m)) {
        return false;
    }
    minutes = h * 60 + m;
    return h >= 0 && h < 24 && m >= 0 && m < 60;
}

struct Sheet {
    const char* name;
    std::string* error;
    std::vector<Row> rows;
    Row header;
    bool ok = true;
    Sheet(const char* n, std::string* e) : name(n), error(e) {}

    bool open(std::string_view text, std::initializer_list<const char*> need) {
        if (!npcCsvSplit(text, rows)) return fail(0, "unclosed quote");
        if (rows.empty()) return fail(0, "no header");
        header = rows.front();
        rows.erase(rows.begin());
        for (const char* h : need)
            if (col(h) < 0) return fail(0, std::string("missing column ") + h);
        return true;
    }
    int col(std::string_view h) const {
        for (std::size_t i = 0; i < header.size(); ++i)
            if (header[i] == h) return static_cast<int>(i);
        return -1;
    }
    std::string_view get(const Row& r, std::string_view h) const {
        const int c = col(h);
        return c >= 0 && static_cast<std::size_t>(c) < r.size() ? std::string_view(r[static_cast<std::size_t>(c)]) : std::string_view{};
    }
    bool fail(std::size_t row, const std::string& why) {
        if (ok && error) *error = std::string(name) + ":" + std::to_string(row + 1) + ": " + why;
        ok = false;
        return false;
    }
    bool num(std::size_t row, const Row& r, std::string_view h, std::int64_t lo, std::int64_t hi, std::int64_t& out,
             bool optional = false) {
        const std::string_view s = get(r, h);
        if (s.empty() && optional) return true;
        if (!toInt(s, out) || out < lo || out > hi) return fail(row + 1, std::string(h) + " out of range");
        return true;
    }
};

std::vector<std::string_view> splitOn(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    std::size_t a = 0;
    while (a <= s.size()) {
        const std::size_t b = s.find(sep, a);
        const std::string_view part = s.substr(a, b == std::string_view::npos ? std::string_view::npos : b - a);
        if (!part.empty()) out.push_back(part);
        if (b == std::string_view::npos) break;
        a = b + 1;
    }
    return out;
}

int stanceOf(std::string_view s) {
    static constexpr const char* kNames[] = {"hostile", "wary", "neutral", "warm", "loyal"};
    for (int i = 0; i < 5; ++i)
        if (s == kNames[i]) return i;
    return -1;
}

int expStateOf(std::string_view s) {
    static constexpr const char* kNames[] = {"pending", "reminding", "kept", "broken", "missed"};
    for (int i = 0; i < 5; ++i)
        if (s == kNames[i]) return i;
    return -1;
}

bool deedOrNone(const NpcTables& t, std::string_view s, std::uint16_t& out) {
    if (s.empty() || s == "-") {
        out = kNoType;
        return true;
    }
    const int i = t.deedIndex(s);
    if (i < 0) return false;
    out = static_cast<std::uint16_t>(i);
    return true;
}

void readTuning(const JsonValue& root, NpcTuning& tu) {
    auto one = [&](std::string_view k, std::int32_t& v, std::int32_t lo, std::int32_t hi) {
        const JsonValue x = root[k];
        if (x.isInteger()) v = clampi(static_cast<std::int32_t>(x.integer()), lo, hi);
    };
    one("kVictim", tu.kVictim, 100, 200);
    one("kWitness", tu.kWitness, 50, 150);
    one("kHop1", tu.kHop1, 30, 80);
    one("kHop2", tu.kHop2, 30, 80);
    one("kReveal", tu.kReveal, 30, 200);
    one("tieGainT", tu.tieGain[0], 0, 10);
    one("tieGainF", tu.tieGain[1], 0, 10);
    one("tieGainR", tu.tieGain[2], 0, 10);
    one("decayT", tu.decayPerDay[0], 0, 20);
    one("decayF", tu.decayPerDay[1], 0, 20);
    one("decayR", tu.decayPerDay[2], 0, 20);
    one("salienceDecayPerHour", tu.salienceDecayPerHour, 1, 10);
    one("recallBar", tu.recallBar, 100, 400);
    one("stayGap", tu.stayGap, 5, 15);
    one("spreadThreshold", tu.spreadThreshold, 60, 250);
    one("streetThreshold", tu.streetThreshold, 100, 10000);
    one("streetDelayMin", tu.streetDelayMin, 30, 1440);
    one("rkFloor", tu.rkFloor, 10, 50);
    one("rkStep", tu.rkStep, 0, 50);
    one("rkWindowMin", tu.rkWindowMin, 60, 14 * 1440);
    one("leadCloseness", tu.leadCloseness, 0, 100);
}

}  // namespace

bool importNpcTables(const NpcTableSources& src, NpcTables& out, std::string* error) {
    NpcTables t;
    if (!src.tuningJson.empty()) {
        JsonDocument doc;
        if (!doc.parse(src.tuningJson, error)) return false;
        if (!doc.root().isObject()) {
            if (error) *error = "memory_tuning.json: not an object";
            return false;
        }
        readTuning(doc.root(), t.tuning);
    }
    if (!src.tags.empty()) {
        Sheet s("tags.csv", error);
        if (!s.open(src.tags, {"tag_id", "bit"})) return false;
        t.tags.assign(kNpcTagMax, std::string{});
        for (std::size_t i = 0; i < s.rows.size(); ++i) {
            std::int64_t bit = 0;
            if (!s.num(i, s.rows[i], "bit", 0, kNpcTagMax - 1, bit)) return false;
            std::string& slot = t.tags[static_cast<std::size_t>(bit)];
            if (!slot.empty()) return s.fail(i + 1, "bit used twice");
            slot = std::string(s.get(s.rows[i], "tag_id"));
        }
        while (!t.tags.empty() && t.tags.back().empty()) t.tags.pop_back();
    }
    if (!src.deeds.empty()) {
        Sheet s("deeds.csv", error);
        if (!s.open(src.deeds, {"deed_id", "tags", "magnitude", "polarity", "secret", "pin", "attributable"})) return false;
        for (std::size_t i = 0; i < s.rows.size(); ++i) {
            const Row& r = s.rows[i];
            NpcDeedType d;
            if (t.deeds.size() >= kDeedTypeMax) return s.fail(i + 1, "more than 128 deed types");
            d.id = std::string(s.get(r, "deed_id"));
            for (std::string_view tag : splitOn(s.get(r, "tags"), ';')) {
                const int b = t.tagIndex(tag);
                if (b < 0) return s.fail(i + 1, "unknown tag " + std::string(tag));
                d.tagMask |= 1u << b;
            }
            std::int64_t m = 0, p = 0, sec = 0, pn = 0, at = 0;
            if (!s.num(i, r, "magnitude", 1, 5, m) || !s.num(i, r, "polarity", -1, 1, p) || !s.num(i, r, "secret", 0, 1, sec) ||
                !s.num(i, r, "pin", 0, 1, pn) || !s.num(i, r, "attributable", 0, 1, at))
                return false;
            d.magnitude = static_cast<std::int8_t>(m);
            d.polarity = static_cast<std::int8_t>(p);
            d.secret = sec != 0;
            d.pin = pn != 0;
            d.attributable = at != 0;
            if (d.id.empty() || t.deedIndex(d.id) >= 0) return s.fail(i + 1, "empty or repeated deed_id");
            t.deeds.push_back(d);
        }
    }
    if (!src.npcValues.empty()) {
        Sheet s("npc_values.csv", error);
        if (!s.open(src.npcValues, {"npc_id", "base_T", "base_F", "base_R", "district_id", "weights"})) return false;
        for (std::size_t i = 0; i < s.rows.size(); ++i) {
            const Row& r = s.rows[i];
            if (t.npcs.size() >= kNpcMax) return s.fail(i + 1, "more than 64 NPCs");
            NpcDef n;
            n.id = std::string(s.get(r, "npc_id"));
            std::int64_t bt = 0, bf = 0, br = 0, dist = 0;
            if (!s.num(i, r, "base_T", -100, 100, bt) || !s.num(i, r, "base_F", 0, 100, bf) || !s.num(i, r, "base_R", -100, 100, br) ||
                !s.num(i, r, "district_id", 0, 65535, dist))
                return false;
            n.base = {static_cast<std::int16_t>(bt), static_cast<std::int16_t>(bf), static_cast<std::int16_t>(br)};
            n.district = static_cast<std::uint16_t>(dist);
            for (std::string_view wpart : splitOn(s.get(r, "weights"), ';')) {
                const auto colon = wpart.find(':');
                if (colon == std::string_view::npos) return s.fail(i + 1, "weight needs tag:T/F/R");
                const int b = t.tagIndex(wpart.substr(0, colon));
                if (b < 0) return s.fail(i + 1, "unknown tag " + std::string(wpart.substr(0, colon)));
                const auto parts = splitOn(wpart.substr(colon + 1), '/');
                if (parts.size() != 3) return s.fail(i + 1, "weight needs three values");
                for (std::size_t a = 0; a < 3; ++a) {
                    std::int64_t v = 0;
                    if (!toInt(parts[a], v) || v < -6 || v > 6) return s.fail(i + 1, "weight out of -6..6");
                    n.weights[static_cast<std::size_t>(b)][a] = static_cast<std::int8_t>(v);
                }
            }
            if (n.id.empty() || t.npcIndex(n.id) >= 0) return s.fail(i + 1, "empty or repeated npc_id");
            t.npcs.push_back(n);
        }
    }
    if (!src.ties.empty()) {
        Sheet s("ties.csv", error);
        if (!s.open(src.ties, {"from_id", "to_id", "kind", "tie", "closeness", "delay_min"})) return false;
        for (std::size_t i = 0; i < s.rows.size(); ++i) {
            const Row& r = s.rows[i];
            if (t.ties.size() >= kNpcTieMax) return s.fail(i + 1, "more than 512 ties");
            NpcTie e;
            const int from = t.npcIndex(s.get(r, "from_id"));
            if (from < 0) return s.fail(i + 1, "unknown from_id");
            e.from = static_cast<std::uint16_t>(from);
            const std::string_view kind = s.get(r, "kind");
            std::int64_t to = 0;
            if (kind == "npc") {
                const int x = t.npcIndex(s.get(r, "to_id"));
                if (x < 0) return s.fail(i + 1, "unknown to_id");
                to = x;
                e.kind = NpcTieKind::Npc;
            } else if (kind == "lead") {
                if (!s.num(i, r, "to_id", 0, kLeadCount - 1, to)) return false;
                e.kind = NpcTieKind::Lead;
            } else if (kind == "faction") {
                e.kind = NpcTieKind::Faction;
            } else {
                return s.fail(i + 1, "kind must be npc, faction or lead");
            }
            e.to = static_cast<std::uint16_t>(to);
            std::int64_t tie = 0, close = 0, delay = 0;
            if (!s.num(i, r, "tie", -100, 100, tie) || !s.num(i, r, "closeness", 0, 100, close) ||
                !s.num(i, r, "delay_min", 0, 1440, delay))
                return false;
            e.tie = static_cast<std::int8_t>(tie);
            e.closeness = static_cast<std::uint8_t>(close);
            e.delayMin = static_cast<std::uint16_t>(delay);
            t.ties.push_back(e);
        }
    }
    t.consequences.assign(t.npcs.size() * 5u, NpcConsequence{});
    if (!src.consequences.empty()) {
        Sheet s("consequences.csv", error);
        if (!s.open(src.consequences, {"npc_id", "stance", "price_pct", "shop_open", "door_open", "quest_open", "fight_role",
                                       "greet_anim", "greet_dist_cm", "van_ride"}))
            return false;
        for (std::size_t i = 0; i < s.rows.size(); ++i) {
            const Row& r = s.rows[i];
            const int n = t.npcIndex(s.get(r, "npc_id"));
            const int st = stanceOf(s.get(r, "stance"));
            if (n < 0 || st < 0) return s.fail(i + 1, "unknown npc_id or stance");
            NpcConsequence c;
            std::int64_t price = 0, shop = 0, door = 0, quest = 0, anim = 0, dist = 0, van = 0;
            if (!s.num(i, r, "price_pct", 10, 1000, price) || !s.num(i, r, "shop_open", 0, 1, shop) ||
                !s.num(i, r, "door_open", 0, 1, door) || !s.num(i, r, "quest_open", 0, 1, quest) ||
                !s.num(i, r, "greet_anim", 0, 65535, anim) || !s.num(i, r, "greet_dist_cm", 0, 65535, dist) ||
                !s.num(i, r, "van_ride", 0, 1, van))
                return false;
            const std::string_view role = s.get(r, "fight_role");
            static constexpr const char* kRoles[] = {"aid", "none", "hinder", "inform"};
            int ri = -1;
            for (int k = 0; k < 4; ++k)
                if (role == kRoles[k]) ri = k;
            if (ri < 0) return s.fail(i + 1, "fight_role must be aid, none, hinder or inform");
            c.pricePct = static_cast<std::int16_t>(price);
            c.shopOpen = shop != 0;
            c.doorOpen = door != 0;
            c.questOpen = quest != 0;
            c.fightRole = static_cast<std::uint8_t>(ri);
            c.greetAnim = static_cast<std::uint16_t>(anim);
            c.greetDistCm = static_cast<std::uint16_t>(dist);
            c.vanRide = van != 0;
            t.consequences[static_cast<std::size_t>(n) * 5u + static_cast<std::size_t>(st)] = c;
        }
    }
    if (!src.expectations.empty()) {
        Sheet s("expectations.csv", error);
        if (!s.open(src.expectations, {"exp_id", "npc_id", "kind", "due", "window_min", "remind_lead_min", "kept_by", "broken_by",
                                       "deed_kept", "deed_broken", "deed_missed"}))
            return false;
        for (std::size_t i = 0; i < s.rows.size(); ++i) {
            const Row& r = s.rows[i];
            if (t.expectations.size() >= kExpMax) return s.fail(i + 1, "more than 64 expectations");
            NpcExpDef e;
            e.id = std::string(s.get(r, "exp_id"));
            const int n = t.npcIndex(s.get(r, "npc_id"));
            if (n < 0) return s.fail(i + 1, "unknown npc_id");
            e.npc = static_cast<std::uint16_t>(n);
            // due: "day:HH:MM" or a game minute.
            const std::string_view due = s.get(r, "due");
            std::int64_t v = 0;
            const auto c1 = due.find(':');
            if (c1 != std::string_view::npos) {
                std::int64_t day = 0, clock = 0;
                if (!toInt(due.substr(0, c1), day) || !toClock(due.substr(c1 + 1), clock) || day < 0 || day > kNpcDayMax)
                    return s.fail(i + 1, "due must be day:HH:MM or a minute");
                v = day * 1440 + clock;
            } else if (!toInt(due, v) || v < 0 || v > kNpcDayMax * 1440) {
                return s.fail(i + 1, "due must be day:HH:MM or a minute");
            }
            e.due = static_cast<std::uint32_t>(v);
            std::int64_t win = 0, lead = 0;
            if (!s.num(i, r, "window_min", 0, 14 * 1440, win) || !s.num(i, r, "remind_lead_min", 0, 14 * 1440, lead)) return false;
            e.windowMin = static_cast<std::uint32_t>(win);
            e.remindLeadMin = static_cast<std::uint32_t>(lead);
            if (!deedOrNone(t, s.get(r, "kept_by"), e.keptBy) || !deedOrNone(t, s.get(r, "broken_by"), e.brokenBy) ||
                !deedOrNone(t, s.get(r, "deed_kept"), e.deedKept) || !deedOrNone(t, s.get(r, "deed_broken"), e.deedBroken) ||
                !deedOrNone(t, s.get(r, "deed_missed"), e.deedMissed))
                return s.fail(i + 1, "unknown deed id");
            t.expectations.push_back(e);
        }
    }
    if (!src.schedule.empty()) {
        Sheet s("schedule.csv", error);
        if (!s.open(src.schedule, {"event_id", "day", "hour", "foreshadow_lead_min"})) return false;
        for (std::size_t i = 0; i < s.rows.size(); ++i) {
            const Row& r = s.rows[i];
            if (t.events.size() >= kEventMax) return s.fail(i + 1, "more than 64 events");
            NpcEventDef e;
            e.id = std::string(s.get(r, "event_id"));
            std::int64_t day = 0, clock = 0, lead = 0;
            if (!s.num(i, r, "day", 0, kNpcDayMax, day) || !toClock(s.get(r, "hour"), clock) ||
                !s.num(i, r, "foreshadow_lead_min", 0, 14 * 1440, lead))
                return s.fail(i + 1, "bad day, hour or foreshadow_lead_min");
            e.minute = static_cast<std::uint32_t>(day * 1440 + clock);
            e.foreshadowLeadMin = static_cast<std::uint32_t>(lead);
            t.events.push_back(e);
        }
    }
    out = std::move(t);
    return true;
}

namespace {

// One clause of a line condition.
bool parseCond(const NpcTables& t, std::string_view s, NpcCond& c) {
    auto memRef = [&](std::string_view ref, NpcCond& into) {
        if (ref.substr(0, 5) == "deed=") {
            into.type = static_cast<std::int16_t>(t.deedIndex(ref.substr(5)));
            return into.type >= 0;
        }
        if (ref.substr(0, 4) == "tag=") {
            into.tag = static_cast<std::int16_t>(t.tagIndex(ref.substr(4)));
            return into.tag >= 0;
        }
        return false;
    };
    std::int64_t v = 0;
    if (s.substr(0, 4) == "mem:") {
        const auto ge = s.find(">=");
        c.kind = NpcCondKind::Mem;
        c.value = -1;  // the tuning's recall bar
        if (ge != std::string_view::npos) {
            if (!toInt(s.substr(ge + 2), v) || v < 0 || v > 1000) return false;
            c.value = static_cast<std::int32_t>(v);
        }
        std::string_view ref = s.substr(4, ge == std::string_view::npos ? std::string_view::npos : ge - 4);
        const auto comma = ref.find(',');
        if (comma != std::string_view::npos) {
            const std::string_view by = ref.substr(comma + 1);
            ref = ref.substr(0, comma);
            if (by == "by=self") c.by = kBySelf;
            else if (by == "by=other") c.by = kByOther;
            else if (by.substr(0, 3) == "by=" && toInt(by.substr(3), v) && v >= 0 && v < kLeadCount)
                c.by = static_cast<std::int8_t>(v);
            else return false;
        }
        return memRef(ref, c);
    }
    if (s.substr(0, 6) == "stance") {
        std::string_view rest = s.substr(6);
        if (rest.substr(0, 2) == ">=") c.kind = NpcCondKind::StanceGe, rest.remove_prefix(2);
        else if (rest.substr(0, 2) == "<=") c.kind = NpcCondKind::StanceLe, rest.remove_prefix(2);
        else if (rest.substr(0, 1) == "=") c.kind = NpcCondKind::StanceEq, rest.remove_prefix(1);
        else return false;
        c.value = stanceOf(rest);
        return c.value >= 0;
    }
    if (s.substr(0, 11) == "since_deed:") {
        std::string_view rest = s.substr(11);
        auto op = rest.find(">=");
        c.kind = NpcCondKind::SinceGe;
        if (op == std::string_view::npos) {
            op = rest.find("<=");
            c.kind = NpcCondKind::SinceLe;
        }
        if (op == std::string_view::npos || !toInt(rest.substr(op + 2), v)) return false;
        c.value = static_cast<std::int32_t>(v);
        std::string_view ref = rest.substr(0, op);
        if (ref.find('=') == std::string_view::npos) {
            c.type = static_cast<std::int16_t>(t.deedIndex(ref));
            return c.type >= 0;
        }
        return memRef(ref, c);
    }
    if (s.substr(0, 10) == "not_since:") {
        const std::string_view rest = s.substr(10);
        const auto comma = rest.find(',');
        if (comma == std::string_view::npos) return false;
        c.kind = NpcCondKind::NotSince;
        NpcCond y;
        if (!memRef(rest.substr(0, comma), c) || !memRef(rest.substr(comma + 1), y) || y.type < 0) return false;
        c.other = y.type;
        return true;
    }
    if (s.substr(0, 4) == "exp:") {
        const std::string_view rest = s.substr(4);
        const auto eq = rest.find('=');
        if (eq == std::string_view::npos) return false;
        c.kind = NpcCondKind::Exp;
        c.other = static_cast<std::int16_t>(t.expIndex(rest.substr(0, eq)));
        c.value = expStateOf(rest.substr(eq + 1));
        return c.other >= 0 && c.value >= 0;
    }
    if (s.substr(0, 6) == "event:") {
        const std::string_view rest = s.substr(6);
        const auto le = rest.find("<=");
        if (le == std::string_view::npos || !toInt(rest.substr(le + 2), v) || v < 0) return false;
        c.kind = NpcCondKind::EventLe;
        c.other = static_cast<std::int16_t>(t.eventIndex(rest.substr(0, le)));
        c.value = static_cast<std::int32_t>(v);
        return c.other >= 0;
    }
    if (s.substr(0, 5) == "dead:") {
        c.kind = NpcCondKind::Dead;
        c.other = static_cast<std::int16_t>(t.npcIndex(s.substr(5)));
        return c.other >= 0;
    }
    return false;
}

}  // namespace

bool importNpcLines(std::string_view csv, const NpcTables& t, NpcLineTable& out, std::string* error) {
    Sheet s("npc_lines.csv", error);
    if (!s.open(csv, {"line_id", "npc_id", "kind", "lead", "priority", "cooldown_min", "once", "cond"})) return false;
    NpcLineTable table;
    for (std::size_t i = 0; i < s.rows.size(); ++i) {
        const Row& r = s.rows[i];
        if (table.lines.size() >= kSaidMax) return s.fail(i + 1, "more than 2048 lines");
        NpcLine L;
        L.id = std::string(s.get(r, "line_id"));
        if (L.id.empty() || table.lineIndex(L.id) >= 0) return s.fail(i + 1, "empty or repeated line_id");
        const int n = t.npcIndex(s.get(r, "npc_id"));
        if (n < 0) return s.fail(i + 1, "unknown npc_id");
        L.npc = static_cast<std::uint16_t>(n);
        static constexpr const char* kKinds[] = {"greet", "bark", "talk", "remind", "foreshadow"};
        int k = -1;
        for (int j = 0; j < 5; ++j)
            if (s.get(r, "kind") == kKinds[j]) k = j;
        if (k < 0) return s.fail(i + 1, "kind must be greet, bark, talk, remind or foreshadow");
        L.kind = static_cast<NpcLineKind>(k);
        const std::string_view lead = s.get(r, "lead");
        std::int64_t v = 0;
        if (lead.empty() || lead == "any") L.lead = kNoLead;
        else if (toInt(lead, v) && v >= 0 && v < kLeadCount) L.lead = static_cast<std::uint8_t>(v);
        else return s.fail(i + 1, "lead must be any or 0..2");
        std::int64_t pri = 0, cool = 0, once = 0;
        if (!s.num(i, r, "priority", 0, 9, pri) || !s.num(i, r, "cooldown_min", 0, 100000, cool) || !s.num(i, r, "once", 0, 1, once))
            return false;
        L.priority = static_cast<std::uint8_t>(pri);
        L.cooldownMin = static_cast<std::uint32_t>(cool);
        L.once = once != 0;
        const auto clauses = splitOn(s.get(r, "cond"), ';');
        if (clauses.size() > kCondMax) return s.fail(i + 1, "more than 6 clauses");
        for (std::size_t c = 0; c < clauses.size(); ++c)
            if (!parseCond(t, clauses[c], L.cond[c])) return s.fail(i + 1, "bad clause " + std::string(clauses[c]));
        table.lines.push_back(std::move(L));
    }
    out = std::move(table);
    return true;
}

// ---------------------------------------------------------------- save

NpcMemorySave npcCaptureSave(const NpcMemoryWorld& w, const NpcTables& t, const NpcLineTable& lines) {
    NpcMemorySave s;
    s.now = w.now;
    s.nextSeq = w.nextSeq;
    s.droppedRumours = w.droppedRumours;
    auto npcId = [&](std::uint16_t n) { return n < t.npcs.size() ? t.npcs[n].id : std::string{}; };
    auto typeId = [&](std::uint16_t d) { return d < t.deeds.size() ? t.deeds[d].id : std::string{}; };
    for (std::uint16_t n = 0; n < w.npcCount; ++n) {
        const NpcState& st = w.npcs[n];
        NpcMemorySaveNpc o;
        o.id = npcId(n);
        o.axes = st.axes;
        o.axesTs = st.axesTs;
        o.offsets = st.offsets;
        o.offsetsTs = st.offsetsTs;
        for (std::size_t l = 0; l <= kLeadCount; ++l) o.stance[l] = static_cast<std::uint8_t>(st.stance[l]);
        o.dead = st.dead;
        o.deadAt = st.deadAt;
        for (std::uint32_t i = 0; i < st.memCount; ++i) {
            const NpcMemoryEntry& e = st.mem[i];
            o.mem.push_back({e.seq, e.deedMinute, e.ts, e.learned, typeId(e.type), e.place, e.s0, e.hops, e.count, e.lead, e.pinned, e.leads});
        }
        for (std::size_t b = 0; b < kNpcTagMax && b < t.tags.size(); ++b)
            if (st.aggregates[b].count) o.aggregates.emplace_back(t.tags[b], st.aggregates[b]);
        for (std::uint32_t slot = 0; slot < kDeedRing; ++slot)
            if ((st.seen[slot / 32u] & (1u << (slot % 32u))) && w.ring[slot].seq) o.seen.push_back(w.ring[slot].seq);
        std::sort(o.seen.begin(), o.seen.end());
        for (const NpcRecent& r : st.recent)
            for (std::uint32_t i = 0; i < r.count; ++i) o.recent.emplace_back(typeId(r.type), r.minute[i]);
        s.npcs.push_back(std::move(o));
    }
    for (const NpcDeed& d : w.ring) {
        if (d.seq == 0) continue;
        NpcMemorySaveDeed o;
        o.seq = d.seq;
        o.minute = d.minute;
        o.type = typeId(d.type);
        o.victim = d.victim == kNoNpc ? std::string{} : npcId(d.victim);
        o.place = d.place;
        o.district = d.district;
        o.lead = d.lead;
        o.flags = d.flags;
        for (std::uint16_t x : d.witnesses)
            if (x != kNoNpc) o.witnesses.push_back(npcId(x));
        s.deeds.push_back(std::move(o));
    }
    std::sort(s.deeds.begin(), s.deeds.end(), [](const NpcMemorySaveDeed& a, const NpcMemorySaveDeed& b) { return a.seq < b.seq; });
    for (std::uint32_t i = 0; i < w.rumourCount; ++i) {
        const NpcRumour& r = w.rumours[i];
        s.rumours.push_back({r.seq, r.due, npcId(r.target), r.hops, r.lead, r.closeness});
    }
    for (std::size_t i = 0; i < t.expectations.size() && i < kExpMax; ++i)
        s.expectations.emplace_back(t.expectations[i].id, static_cast<std::uint8_t>(w.exp[i]));
    for (std::size_t i = 0; i < lines.lines.size() && i < kSaidMax; ++i)
        if (w.lastSaid[i]) s.said.emplace_back(lines.lines[i].id, w.lastSaid[i]);
    return s;
}

NpcMemoryLoadReport npcApplySave(NpcMemoryWorld& w, const NpcTables& t, const NpcLineTable& lines, const NpcMemorySave& s) {
    NpcMemoryLoadReport rep;
    npcInit(w, t);
    if (s.format != NpcMemorySave::kFormat) {
        rep.formatOk = false;
        return rep;
    }
    auto unknown = [&](const char* kind, const std::string& id) {
        const std::string key = std::string(kind) + ":" + id;
        if (std::find(rep.unknownIds.begin(), rep.unknownIds.end(), key) == rep.unknownIds.end()) rep.unknownIds.push_back(key);
    };
    auto clampCount = [&](std::int32_t v, std::int32_t lo, std::int32_t hi) {
        const std::int32_t c = clampi(v, lo, hi);
        if (c != v) ++rep.clamped;
        return c;
    };
    w.now = s.now;
    w.nextSeq = std::max<std::uint32_t>(1u, s.nextSeq);
    w.droppedRumours = s.droppedRumours;
    for (const NpcMemorySaveNpc& o : s.npcs) {
        const int n = t.npcIndex(o.id);
        if (n < 0 || n >= w.npcCount) {
            unknown("npc", o.id);
            continue;
        }
        NpcState& st = w.npcs[static_cast<std::size_t>(n)];
        for (int a = 0; a < 3; ++a)
            st.axes[static_cast<std::size_t>(a)] =
                static_cast<std::int16_t>(clampCount(o.axes[static_cast<std::size_t>(a)], axisLo(a), 100));
        st.axesTs = std::min(o.axesTs, s.now);
        for (std::size_t l = 0; l < kLeadCount; ++l) {
            for (std::size_t a = 0; a < 3; ++a) st.offsets[l][a] = static_cast<std::int8_t>(clampCount(o.offsets[l][a], -30, 30));
            st.offsetsTs[l] = std::min(o.offsetsTs[l], s.now);
        }
        for (std::size_t l = 0; l <= kLeadCount; ++l)
            st.stance[l] = static_cast<NpcStance>(clampCount(o.stance[l], 0, 4));
        st.dead = o.dead;
        st.deadAt = std::min(o.deadAt, s.now);
        st.memCount = 0;
        for (const NpcMemorySaveEntry& e : o.mem) {
            const int ty = t.deedIndex(e.type);
            if (ty < 0) {
                unknown("deed", e.type);
                continue;
            }
            if (st.memCount >= kMemPerNpc) {
                ++rep.clamped;
                break;
            }
            NpcMemoryEntry& m = st.mem[st.memCount++];
            m.seq = e.seq;
            m.deedMinute = e.deedMinute;
            m.ts = std::min(e.ts, s.now);
            m.learned = std::min(e.learned, s.now);
            m.type = static_cast<std::uint16_t>(ty);
            m.place = e.place;
            m.s0 = static_cast<std::int16_t>(clampCount(e.s0, 0, 1000));
            m.hops = static_cast<std::uint8_t>(clampCount(e.hops, 0, 3));
            m.count = std::max<std::uint8_t>(1, e.count);
            m.lead = e.lead < kLeadCount ? e.lead : kNoLead;
            m.leads = static_cast<std::uint8_t>(e.leads & ((1u << kLeadCount) - 1u));
            if (m.lead < kLeadCount) m.leads = static_cast<std::uint8_t>(m.leads | (1u << m.lead));
            m.pinned = e.pinned;
        }
        std::uint32_t pins = 0;
        for (std::uint32_t i = 0; i < st.memCount; ++i)
            if (st.mem[i].pinned && ++pins > kPinnedMax) {
                st.mem[i].pinned = false;
                ++rep.clamped;
            }
        for (const auto& ag : o.aggregates) {
            const int b = t.tagIndex(ag.first);
            if (b < 0) {
                unknown("tag", ag.first);
                continue;
            }
            st.aggregates[static_cast<std::size_t>(b)] = ag.second;
        }
        for (const auto& rc : o.recent) {
            const int ty = t.deedIndex(rc.first);
            if (ty < 0) {
                unknown("deed", rc.first);
                continue;
            }
            noteLearn(st, static_cast<std::uint16_t>(ty), std::min(rc.second, s.now));
        }
    }
    for (const NpcMemorySaveDeed& o : s.deeds) {
        const int ty = t.deedIndex(o.type);
        if (ty < 0) {
            unknown("deed", o.type);
            continue;
        }
        if (o.seq == 0 || o.seq >= w.nextSeq) {
            ++rep.clamped;
            continue;
        }
        NpcDeed d;
        d.seq = o.seq;
        d.minute = o.minute;
        d.type = static_cast<std::uint16_t>(ty);
        d.place = o.place;
        d.district = o.district;
        d.lead = o.lead < kLeadCount ? o.lead : kNoLead;
        d.flags = static_cast<std::uint8_t>(o.flags & kFlagKnown);
        if (d.flags != o.flags || d.minute != o.minute) ++rep.clamped;
        if (!o.victim.empty()) {
            const int v = t.npcIndex(o.victim);
            if (v < 0) unknown("npc", o.victim);
            else d.victim = static_cast<std::uint16_t>(v);
        }
        std::size_t k = 0;
        for (const std::string& x : o.witnesses) {
            const int v = t.npcIndex(x);
            if (v < 0) {
                unknown("npc", x);
                continue;
            }
            if (k < kDeedWitnessMax) d.witnesses[k++] = static_cast<std::uint16_t>(v);
        }
        w.ring[d.seq % kDeedRing] = d;
    }
    for (const NpcMemorySaveNpc& o : s.npcs) {  // seen bits only for deeds the ring still holds
        const int n = t.npcIndex(o.id);
        if (n < 0 || n >= w.npcCount) continue;
        for (std::uint32_t seq : o.seen)
            if (npcDeedAt(w, seq)) setSeen(w.npcs[static_cast<std::size_t>(n)], seq, true);
    }
    for (const NpcMemorySaveRumour& o : s.rumours) {
        const int v = t.npcIndex(o.target);
        if (v < 0) {
            unknown("npc", o.target);
            continue;
        }
        if (w.rumourCount >= kRumourQueue) {
            ++rep.clamped;
            break;
        }
        NpcRumour& r = w.rumours[w.rumourCount++];
        r.seq = o.seq;
        r.due = o.due;
        r.target = static_cast<std::uint16_t>(v);
        r.hops = static_cast<std::uint8_t>(clampCount(o.hops, 1, 2));
        r.lead = o.lead < kLeadCount ? o.lead : kNoLead;
        r.closeness = static_cast<std::uint8_t>(clampCount(o.closeness, 0, 100));
    }
    for (const auto& e : s.expectations) {
        const int i = t.expIndex(e.first);
        if (i < 0) {
            unknown("exp", e.first);
            continue;
        }
        if (static_cast<std::uint32_t>(i) < kExpMax) w.exp[static_cast<std::size_t>(i)] = static_cast<NpcExpState>(clampCount(e.second, 0, 4));
    }
    for (const auto& e : s.said) {
        const int i = lines.lineIndex(e.first);
        if (i < 0) {
            unknown("line", e.first);
            continue;
        }
        if (static_cast<std::uint32_t>(i) < kSaidMax) w.lastSaid[static_cast<std::size_t>(i)] = std::min(e.second, satAdd(s.now, 1u));
    }
    return rep;
}

// ---- Game hooks ----
bool npcResolveGameDeeds(const NpcTables& t, NpcGameDeeds& out, std::string* error) {
    NpcGameDeeds g;
    const std::pair<std::string_view, std::uint16_t*> want[] = {
        {"van_knockdown", &g.vanKnockdown},   {"van_knockdown_tied", &g.vanKnockdownTied},
        {"street_fight_won", &g.fightWon},    {"enemy_spared", &g.enemySpared},
        {"shop_theft", &g.shopTheft},         {"shop_bought", &g.shopBought},
        {"helped_pedestrian", &g.helpedPedestrian}, {"promise_meet_dusk", &g.meet},
        {"delivery_made", &g.delivery},       {"gift_accepted", &g.giftAccepted},
        {"gift_returned", &g.giftReturned},   {"gate_broken", &g.gateBroken},
        {"gate_mended", &g.gateMended},
    };
    for (const auto& [id, slot] : want) {
        const int i = t.deedIndex(id);
        if (i < 0) {
            if (error) *error = "deeds.csv: no deed " + std::string(id);
            return false;
        }
        *slot = static_cast<std::uint16_t>(i);
    }
    out = g;
    return true;
}

namespace {

// Records a deed of this type where it happened; `also` (an NPC who is there whatever the caller saw: the keeper, the
// one met) joins the witnesses when there is room and it is not one already.
std::uint32_t recordAt(NpcMemoryWorld& w, const NpcTables& t, std::uint16_t type, const NpcWhere& at,
                       std::uint16_t victim, std::uint16_t also) {
    if (type == kNoType || type >= t.deeds.size()) return 0;
    NpcDeed d;
    d.type = type;
    d.place = at.place;
    d.district = at.district;
    d.victim = victim;
    d.lead = at.lead;
    d.witnesses = at.witnesses;
    if (also != kNoNpc) {
        bool there = false;
        for (const std::uint16_t x : d.witnesses) there = there || x == also;
        for (std::uint16_t& x : d.witnesses)
            if (!there && x == kNoNpc) {
                x = also;
                there = true;
            }
    }
    return npcRecordDeed(w, t, d);
}

}  // namespace

std::uint32_t npcOnVanKnockdown(NpcMemoryWorld& w, const NpcTables& t, const NpcGameDeeds& g, const NpcWhere& at,
                                std::uint16_t victim) {
    const bool tied = victim != kNoNpc && victim < w.npcCount;
    return recordAt(w, t, tied ? g.vanKnockdownTied : g.vanKnockdown, at, tied ? victim : kNoNpc, kNoNpc);
}

std::uint32_t npcOnFightEnd(NpcMemoryWorld& w, const NpcTables& t, const NpcGameDeeds& g, const NpcWhere& at, bool won,
                            bool spared) {
    std::uint32_t seq = 0;
    if (won) seq = recordAt(w, t, g.fightWon, at, kNoNpc, kNoNpc);
    if (spared) {
        const std::uint32_t s = recordAt(w, t, g.enemySpared, at, kNoNpc, kNoNpc);
        if (s != 0) seq = s;
    }
    return seq;
}

NpcConsequence npcOnShop(NpcMemoryWorld& w, const NpcTables& t, const NpcGameDeeds& g, const NpcWhere& at,
                         std::uint16_t keeper, NpcShopAct act) {
    const NpcConsequence terms = npcConsequence(w, t, keeper, at.lead);
    if (act == NpcShopAct::Buy) recordAt(w, t, g.shopBought, at, keeper, kNoNpc); // the keeper learns it as the one dealt with
    else if (act == NpcShopAct::Steal) recordAt(w, t, g.shopTheft, at, keeper, kNoNpc);
    return terms;
}

std::uint32_t npcOnHelpedPedestrian(NpcMemoryWorld& w, const NpcTables& t, const NpcGameDeeds& g, const NpcWhere& at) {
    return recordAt(w, t, g.helpedPedestrian, at, kNoNpc, kNoNpc);
}

std::uint32_t npcOnMeet(NpcMemoryWorld& w, const NpcTables& t, const NpcGameDeeds& g, const NpcWhere& at,
                        std::uint16_t npc) {
    return recordAt(w, t, g.meet, at, kNoNpc, npc < w.npcCount ? npc : kNoNpc);
}

std::uint32_t npcOnDelivery(NpcMemoryWorld& w, const NpcTables& t, const NpcGameDeeds& g, const NpcWhere& at,
                            std::uint16_t recipient) {
    if (recipient >= w.npcCount) return 0;
    return recordAt(w, t, g.delivery, at, recipient, kNoNpc);
}

std::uint32_t npcOnGift(NpcMemoryWorld& w, const NpcTables& t, const NpcGameDeeds& g, const NpcWhere& at,
                        std::uint16_t giver, bool accepted) {
    if (giver >= w.npcCount) return 0;
    return recordAt(w, t, accepted ? g.giftAccepted : g.giftReturned, at, giver, kNoNpc);
}

std::uint32_t npcOnGate(NpcMemoryWorld& w, const NpcTables& t, const NpcGameDeeds& g, const NpcWhere& at,
                        std::uint16_t owner, bool mended) {
    if (owner >= w.npcCount) return 0;
    return recordAt(w, t, mended ? g.gateMended : g.gateBroken, at, owner, kNoNpc);
}

void npcOnClock(NpcMemoryWorld& w, const NpcTables& t, const WorldClock& clock, const TimeCurves& curves) {
    const std::uint32_t minute = npcMinute(clock.day, clock.hours);
    const float crowd = std::clamp(timeCurveAt(curves.crowd, clock.hours), 0.f, 1.f);
    npcAdvance(w, minute);
    npcCheckpoint(w, t, w.now, static_cast<std::uint16_t>(std::lround(crowd * 1000.f)));
}

}  // namespace ftcore
