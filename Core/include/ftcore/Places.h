#pragma once

// Laying a place: a fighter assigns a chair to a person id. Each named place raises what the fighter
// can carry (the cap on their Load); a place can empty, and its weight then falls back on the fighter.
// See docs/SHARED_COMBAT_INPUTS.md section 3.
//
// * Chairs 0..seats-1 (the fighter's rating, 1..kMaxPlaces; only ever raised). Each chair is Free,
//   Named (a person, by an opaque id from data; 0 is no one), LeftOpen (deliberately left open) or
//   Emptied (the host emptied it; the chair stays taken, with nobody in it, and keeps the person id).
// * One chair per person: a person in a Named or an Emptied chair cannot be laid again until that
//   chair is cleared (clearPlace), so a person id is never in two chairs.
// * carryCap() = baseCarry + perPlace x Named chairs, at most kMaxLoad. An open or emptied chair holds
//   nobody, so it carries nothing.
// * emptyPlace(person, load): the chair turns Emptied, the cap falls, and fallLoad Load is stored on
//   the fighter past the cap (up to kMaxLoad). Over the cap the fighter is overCap(), as a full Load
//   is overloaded.
// * tableSeats(): Named + LeftOpen, what setting the table counts (TableSet.h).
//
// No allocation. Plain data save (PlaceSave).

#include "ftcore/StateHash.h"
#include "ftcore/Load.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ftcore {

inline constexpr std::size_t kMaxPlaces = 8;
using PersonId = std::uint16_t;
inline constexpr PersonId kNoPerson = 0;

enum class PlaceState : std::uint8_t { Free, Named, LeftOpen, Emptied }; // append only
enum class PlaceError : std::uint8_t { None, NoSeat, AlreadySeated, BadPerson, NotSeated, BadChair }; // append only

struct PlaceTuning {
    int baseCarry = 40; // the cap with no named place: 1..kMaxLoad
    int perPlace = 20;  // added per named place: 0..kMaxLoad
    int fallLoad = 20;  // Load that falls on the fighter when a place empties: 0..kMaxLoad
};

inline bool validPlaceTuning(const PlaceTuning& t) {
    return t.baseCarry >= 1 && t.baseCarry <= kMaxLoad && t.perPlace >= 0 && t.perPlace <= kMaxLoad &&
           t.fallLoad >= 0 && t.fallLoad <= kMaxLoad;
}

struct Place {
    PlaceState state = PlaceState::Free;
    PersonId person = kNoPerson; // Named and Emptied: who it was laid for
};

inline constexpr std::uint32_t kPlaceSaveVersion = 1;

struct PlaceSave {
    std::uint32_t version = kPlaceSaveVersion;
    std::uint8_t seats = 1;
    std::array<std::uint8_t, kMaxPlaces> states{}; // PlaceState
    std::array<PersonId, kMaxPlaces> persons{};
};

struct PlaceEmptied {
    bool emptied = false;
    PlaceError error = PlaceError::None;
    std::uint8_t chair = 0;
    int capBefore = 0;
    int capAfter = 0;
    int fell = 0;   // Load stored on the fighter
    int excess = 0; // how far the fighter's Load now stands above the new cap (0 if not)
};

class PlaceTable {
public:
    PlaceTable() { reset(); }

    // Every chair Free, `seats` chairs (clamped to 1..kMaxPlaces). False (defaults used) on bad tuning.
    bool reset(int seats = 3, const PlaceTuning& tuning = {}) {
        const bool ok = validPlaceTuning(tuning);
        tuning_ = ok ? tuning : PlaceTuning{};
        seats_ = clampSeats(seats);
        places_.fill(Place{});
        return ok;
    }

    int seats() const { return seats_; }
    // Raises the rating; false (nothing changed) for a lower or out-of-range one.
    bool raiseSeats(int seats) {
        if (seats < seats_ || seats > static_cast<int>(kMaxPlaces)) return false;
        seats_ = seats;
        return true;
    }

    // AlreadySeated when the person is in a Named or an Emptied chair (clear an emptied one first).
    PlaceError layPlace(PersonId person) {
        if (person == kNoPerson) return PlaceError::BadPerson;
        if (holding(person)) return PlaceError::AlreadySeated;
        return fillFree(PlaceState::Named, person);
    }
    PlaceError leaveOpen() { return fillFree(PlaceState::LeftOpen, kNoPerson); }

    // The person's place empties. With `load`, the fall is stored on it.
    PlaceEmptied emptyPlace(PersonId person, Load* load = nullptr) {
        PlaceEmptied r;
        const int i = find(person);
        if (person == kNoPerson || i < 0) {
            r.error = PlaceError::NotSeated;
            return r;
        }
        r.capBefore = carryCap();
        places_[static_cast<std::size_t>(i)].state = PlaceState::Emptied;
        r.chair = static_cast<std::uint8_t>(i);
        r.capAfter = carryCap();
        r.emptied = true;
        if (load != nullptr) {
            r.fell = load->store(tuning_.fallLoad);
            r.excess = load->value > r.capAfter ? load->value - r.capAfter : 0;
        }
        return r;
    }

    // Frees an Emptied or LeftOpen chair. A Named chair is never cleared here: it empties first.
    PlaceError clearPlace(int chair) {
        if (chair < 0 || chair >= seats_) return PlaceError::BadChair;
        Place& p = places_[static_cast<std::size_t>(chair)];
        if (p.state != PlaceState::Emptied && p.state != PlaceState::LeftOpen) return PlaceError::BadChair;
        p = Place{};
        return PlaceError::None;
    }

    // The chair laid for `person` (Named only), or -1.
    int find(PersonId person) const {
        if (person == kNoPerson) return -1;
        for (int i = 0; i < seats_; ++i) {
            const Place& p = places_[static_cast<std::size_t>(i)];
            if (p.state == PlaceState::Named && p.person == person) return i;
        }
        return -1;
    }
    // The person is in a chair, Named or Emptied.
    bool holding(PersonId person) const {
        if (person == kNoPerson) return false;
        for (int i = 0; i < seats_; ++i) {
            const Place& p = places_[static_cast<std::size_t>(i)];
            if ((p.state == PlaceState::Named || p.state == PlaceState::Emptied) && p.person == person) return true;
        }
        return false;
    }
    // The place at `chair`; outside 0..seats()-1, a Free place.
    const Place& place(int chair) const {
        static const Place kOutside{};
        if (chair < 0 || chair >= seats_) return kOutside;
        return places_[static_cast<std::size_t>(chair)];
    }
    int countOf(PlaceState s) const {
        int n = 0;
        for (int i = 0; i < seats_; ++i)
            if (places_[static_cast<std::size_t>(i)].state == s) ++n;
        return n;
    }
    int named() const { return countOf(PlaceState::Named); }
    int tableSeats() const { return countOf(PlaceState::Named) + countOf(PlaceState::LeftOpen); }

    int carryCap() const {
        const int cap = tuning_.baseCarry + tuning_.perPlace * named();
        return cap < kMaxLoad ? cap : kMaxLoad;
    }
    bool full(const Load& load) const { return load.value >= carryCap(); }
    bool overCap(const Load& load) const { return load.value > carryCap(); }
    // Stores up to `amount` on `load`, never past the cap (nor past what it already holds over it).
    int storeCapped(Load& load, int amount) const {
        const int room = carryCap() - load.value;
        if (amount <= 0 || room <= 0) return 0;
        return load.store(amount < room ? amount : room);
    }

    const PlaceTuning& tuning() const { return tuning_; }

    PlaceSave save() const {
        PlaceSave s;
        s.seats = static_cast<std::uint8_t>(seats_);
        for (std::size_t i = 0; i < kMaxPlaces; ++i) {
            s.states[i] = static_cast<std::uint8_t>(places_[i].state);
            s.persons[i] = places_[i].person;
        }
        return s;
    }
    // False, and nothing changed, for a newer version, a bad rating, a bad state, a chair past the
    // rating that is not Free, a Named or Emptied chair with no person, or a person in two chairs
    // (Named or Emptied).
    bool load(const PlaceSave& s) {
        if (s.version == 0 || s.version > kPlaceSaveVersion) return false;
        if (s.seats < 1 || s.seats > kMaxPlaces) return false;
        std::array<Place, kMaxPlaces> next{};
        for (std::size_t i = 0; i < kMaxPlaces; ++i) {
            if (s.states[i] > static_cast<std::uint8_t>(PlaceState::Emptied)) return false;
            const PlaceState st = static_cast<PlaceState>(s.states[i]);
            if (i >= s.seats && st != PlaceState::Free) return false;
            const bool hasPerson = st == PlaceState::Named || st == PlaceState::Emptied;
            if (hasPerson && s.persons[i] == kNoPerson) return false;
            next[i].state = st;
            next[i].person = hasPerson ? s.persons[i] : kNoPerson;
            if (hasPerson)
                for (std::size_t j = 0; j < i; ++j)
                    if (next[j].person == s.persons[i]) return false; // only Named/Emptied keep a person
        }
        seats_ = s.seats;
        places_ = next;
        return true;
    }

private:
    static int clampSeats(int n) {
        if (n < 1) return 1;
        if (n > static_cast<int>(kMaxPlaces)) return static_cast<int>(kMaxPlaces);
        return n;
    }
    PlaceError fillFree(PlaceState state, PersonId person) {
        for (int i = 0; i < seats_; ++i) {
            Place& p = places_[static_cast<std::size_t>(i)];
            if (p.state == PlaceState::Free) {
                p.state = state;
                p.person = person;
                return PlaceError::None;
            }
        }
        return PlaceError::NoSeat;
    }

public:
    // Its play state for replay checks (StateHash.h): every field that changes what it does next.
    void hashState(StateHash& h) const {
        h.add(seats_);
        for (const Place& p : places_) h.add(p.state).add(p.person);
    }

private:
    PlaceTuning tuning_{};
    int seats_ = 1;
    std::array<Place, kMaxPlaces> places_{};
};

} // namespace ftcore
