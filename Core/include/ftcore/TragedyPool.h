#pragma once

// The tragedies a lead can be shown in the Unwritten: one key per loss the lead has lived. A key enters the pool
// (unlock) only once the story has given it to that lead; a pick only ever looks at the active lead's own keys.
//
// Data/Unwritten/tragedies.json (ids only; the people and the moments stay in the private story data):
//   { "version": 1, "places": ["playground", ...],
//     "keys": [ { "id": "trag_a02", "lead": "a", "places": ["playground"], "voice": "vo_trag_a02",
//                 "figure": "fig_trag_a02" }, ... ] }
// "lead" is "a" or "b" (kLeadA, kLeadB). A key's places are the page place types it answers to.
//
// Picks are uniform over the eligible keys for now (no weights yet). A key shown in the last `recentSeconds` is
// passed over while any other key is eligible. No allocation after construction.
//
// The pool's times (shown(), pick()'s `now`) are the Hallucination's clock (Hallucination::now()), so a pool and the
// Hallucination that feeds it share a lifetime: make them anew together, or a pool's stamps outlive their clock.

#include "ftcore/Api.h"
#include "ftcore/CombatInputs.h"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
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
            defined_.set(i);
        }
    }

    // The lead has lived key `k`. False: no such key.
    bool unlock(int k) {
        if (!defined(k)) return false;
        unlocked_.set(static_cast<std::size_t>(k));
        return true;
    }
    bool has(int k) const { return defined(k) && unlocked_.test(static_cast<std::size_t>(k)); }
    FighterId lead(int k) const { return defined(k) ? lead_[static_cast<std::size_t>(k)] : kNoFighter; }

    // An unlocked key of `who`'s answering to `place` (bits; 0: any place), at play time `now`. -1: none.
    int pick(std::uint32_t& rng, FighterId who, std::uint32_t place, double now, double recentSeconds) const {
        const auto eligible = [&](std::size_t i, bool skipRecent) {
            if (!unlocked_.test(i) || lead_[i] != who) return false;
            if (place != 0 && (places_[i] & place) == 0) return false;
            return !(skipRecent && shownAt_[i] != kNever && now - shownAt_[i] < recentSeconds);
        };
        for (const bool skipRecent : {true, false}) {
            std::uint32_t n = 0;
            for (std::size_t i = 0; i < kMaxTragedyKeys; ++i) n += eligible(i, skipRecent) ? 1u : 0u;
            if (n == 0) continue;
            std::uint32_t r = tragedyRng(rng) % n;
            for (std::size_t i = 0; i < kMaxTragedyKeys; ++i)
                if (eligible(i, skipRecent) && r-- == 0) return static_cast<int>(i);
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

    std::bitset<kMaxTragedyKeys> defined_;
    std::bitset<kMaxTragedyKeys> unlocked_;
    std::array<FighterId, kMaxTragedyKeys> lead_{};
    std::array<std::uint32_t, kMaxTragedyKeys> places_{};
    std::array<double, kMaxTragedyKeys> shownAt_{};
};

} // namespace ftcore
