#pragma once

// The tragedies a lead can be shown in the Unwritten: one key per loss the lead has lived. A key enters the pool
// (unlock) only once the story has given it to that lead; a pick only ever looks at the active lead's own keys.
//
// Data/Unwritten/tragedies.json (ids only; the people and the moments stay in the private story data):
//   { "version": 1, "places": ["playground", ...],
//     "keys": [ { "id": "trag_a02", "lead": "a", "places": ["playground"], "voice": "vo_trag_a02",
//                 "figure": "fig_trag_a02", "caused": true, "lone": true }, ... ] }
// "lead" is "a" or "b" (kLeadA, kLeadB). A key's places are the page place types it answers to. "caused" (the lead
// caused it) and "lone" (the key Lone Carry weighs) are optional, false by default.
//
// Picks are weighted: every key 1; the lead's newest key (the last of theirs unlocked) x3; a caused key x2; the lone
// key x2 when Lone Carry weighs it (loneWeighs() in Hallucination.h). The factors multiply. A key shown in the last
// `recentSeconds` is passed over while any other key is eligible; the newest key's x3 stays its own even then. No
// allocation after construction.
//
// The pool's times (shown(), pick()'s `now`) are the Hallucination's clock (Hallucination::now()), so a pool and the
// Hallucination that feeds it share a lifetime: make them anew together, or a pool's stamps outlive their clock.

#include "ftcore/Api.h"
#include "ftcore/CombatInputs.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ftcore {

inline constexpr std::size_t kMaxTragedyKeys = 64;
inline constexpr std::size_t kMaxTragedyPlaces = 32; // place types, one bit each

struct TragedyDef {
    std::string id;
    FighterId lead = kLeadA;
    std::uint32_t places = 0; // bits into TragedyTable::places
    std::string voice;        // voice-line id
    std::string figure;       // figure asset id
    bool caused = false;      // the lead caused it: x2
    bool lone = false;        // the key Lone Carry weighs: x2 while it does
};

struct TragedyTable {
    std::vector<std::string> places;
    std::vector<TragedyDef> keys;
};

FTCORE_API bool importTragedies(std::string_view json, TragedyTable& out, std::string* error = nullptr);

// The bit of a place type (0: not one of the table's).
inline std::uint32_t tragedyPlaceBit(const TragedyTable& t, std::string_view place) {
    for (std::size_t i = 0; i < t.places.size(); ++i)
        if (t.places[i] == place) return 1u << i;
    return 0;
}

// The index of a key id (-1: none).
inline int tragedyKey(const TragedyTable& t, std::string_view id) {
    for (std::size_t i = 0; i < t.keys.size(); ++i)
        if (t.keys[i].id == id) return static_cast<int>(i);
    return -1;
}

// xorshift32; a zero state is moved to 1. The host gives the pool its own stream, not the fight's.
inline std::uint32_t tragedyRng(std::uint32_t& state) {
    std::uint32_t x = state ? state : 1u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
}

class TragedyPool {
public:
    TragedyPool() { shownAt_.fill(kNever); }
    explicit TragedyPool(const TragedyTable& t) : TragedyPool() {
        for (std::size_t i = 0; i < t.keys.size() && i < kMaxTragedyKeys; ++i) {
            lead_[i] = t.keys[i].lead;
            places_[i] = t.keys[i].places;
            caused_.set(i, t.keys[i].caused);
            lone_.set(i, t.keys[i].lone);
            defined_.set(i);
        }
    }

    // The lead has lived key `k`. False: no such key. A key unlocked again keeps its place in the order.
    bool unlock(int k) {
        if (!defined(k)) return false;
        const std::size_t i = static_cast<std::size_t>(k);
        if (!unlocked_.test(i)) {
            unlocked_.set(i);
            order_[i] = ++unlocks_;
        }
        return true;
    }
    // `who`'s newest key: the last of theirs unlocked (-1: none).
    int newest(FighterId who) const {
        int top = -1;
        for (std::size_t i = 0; i < kMaxTragedyKeys; ++i)
            if (unlocked_.test(i) && lead_[i] == who && (top < 0 || order_[i] > order_[static_cast<std::size_t>(top)]))
                top = static_cast<int>(i);
        return top;
    }
    // Key `k`'s weight in a pick (0: not an unlocked key); `loneWeighted`: Lone Carry weighs the lone key.
    std::uint32_t weight(int k, bool loneWeighted = false) const {
        if (!has(k)) return 0;
        const std::size_t i = static_cast<std::size_t>(k);
        return weightOf(i, newest(lead_[i]), loneWeighted);
    }
    // Key `k`'s place in the unlock order, from 1 (0: locked or no such key).
    std::uint32_t unlockOrder(int k) const { return has(k) ? order_[static_cast<std::size_t>(k)] : 0u; }
    bool has(int k) const { return defined(k) && unlocked_.test(static_cast<std::size_t>(k)); }
    FighterId lead(int k) const { return defined(k) ? lead_[static_cast<std::size_t>(k)] : kNoFighter; }

    // An unlocked key of `who`'s answering to `place` (bits; 0: any place), at play time `now`, by weight (one draw
    // from `rng`). -1: none.
    int pick(std::uint32_t& rng, FighterId who, std::uint32_t place, double now, double recentSeconds,
             bool loneWeighted = false) const {
        const int top = newest(who);
        const auto eligible = [&](std::size_t i, bool skipRecent) {
            if (!unlocked_.test(i) || lead_[i] != who) return false;
            if (place != 0 && (places_[i] & place) == 0) return false;
            return !(skipRecent && shownAt_[i] != kNever && now - shownAt_[i] < recentSeconds);
        };
        for (const bool skipRecent : {true, false}) {
            std::uint32_t total = 0;
            for (std::size_t i = 0; i < kMaxTragedyKeys; ++i)
                total += eligible(i, skipRecent) ? weightOf(i, top, loneWeighted) : 0u;
            if (total == 0) continue;
            std::uint32_t r = tragedyRng(rng) % total;
            for (std::size_t i = 0; i < kMaxTragedyKeys; ++i) {
                if (!eligible(i, skipRecent)) continue;
                const std::uint32_t w = weightOf(i, top, loneWeighted);
                if (r < w) return static_cast<int>(i);
                r -= w;
            }
        }
        return -1;
    }

    void shown(int k, double now) {
        if (defined(k)) shownAt_[static_cast<std::size_t>(k)] = now;
    }

private:
    static constexpr double kNever = -1.0e300;
    bool defined(int k) const {
        return k >= 0 && static_cast<std::size_t>(k) < kMaxTragedyKeys && defined_.test(static_cast<std::size_t>(k));
    }
    // 1, x3 the lead's newest (`top`), x2 caused, x2 the lone key when `loneWeighted`: at most 12.
    std::uint32_t weightOf(std::size_t i, int top, bool loneWeighted) const {
        return (static_cast<int>(i) == top ? 3u : 1u) * (caused_.test(i) ? 2u : 1u) *
               (loneWeighted && lone_.test(i) ? 2u : 1u);
    }

    std::bitset<kMaxTragedyKeys> defined_;
    std::bitset<kMaxTragedyKeys> unlocked_;
    std::bitset<kMaxTragedyKeys> caused_;
    std::bitset<kMaxTragedyKeys> lone_;
    std::array<std::uint32_t, kMaxTragedyKeys> order_{}; // unlock order, from 1
    std::uint32_t unlocks_ = 0;
    std::array<FighterId, kMaxTragedyKeys> lead_{};
    std::array<std::uint32_t, kMaxTragedyKeys> places_{};
    std::array<double, kMaxTragedyKeys> shownAt_{};
};

// The pool as a save keeps it: the unlocked keys by id, oldest unlock first, so a restore gives back which key is each
// lead's newest. Ids rather than indices, so a save survives a re-import that reorders the table. The shown stamps
// are not kept (they are on the Hallucination's clock, which a save does not keep either): a restored pool has shown
// nothing yet. Capture and apply allocate (they copy text): call them on save and load, not per frame.
struct TragedySave {
    std::vector<std::string> unlocked;
    bool operator==(const TragedySave&) const = default;
};

// Fills `out` from `p`. Returns how many of the pool's unlocked keys `t` has no row for (at or past its size: a pool
// of a larger table); those are left out of the save.
inline int captureTragedies(const TragedyTable& t, const TragedyPool& p, TragedySave& out) {
    out.unlocked.clear();
    std::vector<std::pair<std::uint32_t, std::size_t>> byOrder;
    int dropped = 0;
    for (std::size_t i = 0; i < kMaxTragedyKeys; ++i) {
        const std::uint32_t o = p.unlockOrder(static_cast<int>(i));
        if (o == 0) continue;
        if (i < t.keys.size()) byOrder.emplace_back(o, i);
        else ++dropped;
    }
    std::sort(byOrder.begin(), byOrder.end());
    out.unlocked.reserve(byOrder.size());
    for (const auto& [order, i] : byOrder) out.unlocked.push_back(t.keys[i].id);
    return dropped;
}

// Unlocks the save's keys, in its order, into `p` (a fresh pool of `t`: a key already unlocked there keeps its own
// place). Returns how many of the save's ids `t` no longer has; those are skipped.
inline int applyTragedies(const TragedySave& s, const TragedyTable& t, TragedyPool& p) {
    int missing = 0;
    for (const std::string& id : s.unlocked)
        missing += p.unlock(tragedyKey(t, id)) ? 0 : 1;
    return missing;
}

} // namespace ftcore
