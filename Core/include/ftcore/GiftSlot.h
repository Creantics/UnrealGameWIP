#pragma once

// The third party slot's gifts: a heal, a guard and a shared count on one button, never an attack. See
// docs/SHARED_COMBAT_INPUTS.md section 2.
//
// * The slot is a Companion (gifts work), an EmptyChair (the button does nothing) or Hostile (the
//   button does nothing). No press is ever discarded: three save-wide tallies keep them, given gifts
//   per gift (tally()), presses in the empty chair (emptyPresses()) and presses while hostile
//   (hostilePresses()); allPresses() is every press in the save across the three.
// * select(gift) picks what the button gives; press(target, t, count) gives it to a lead. A given gift
//   cannot be taken back (there is no cancel). Each gift has its own cooldown. The Count gift calls
//   `count` with the third slot as the caller; when the count refuses, the gift is not given.
// * Every given gift leaves one thread on its target, forever, and adds one to the save-wide tally.
// * The tally and the threads survive save and load (GiftSave). Counters saturate, never wrap.
//
// What the host applies: Heal (healShare of the target's max HP, and healLoad Load moved off it),
// Guard (guardScaleFor(lead, t) on every blow the lead takes), Count (already called). No allocation.

#include "ftcore/CombatInputs.h"
#include "ftcore/StrikeCount.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ftcore {

enum class Gift : std::uint8_t { Heal, Guard, Count, None }; // append only; None: no gift
inline constexpr std::size_t kGiftKinds = 3;                 // Heal, Guard, Count
enum class SlotState : std::uint8_t { Companion, EmptyChair, Hostile }; // append only
enum class GiftError : std::uint8_t { None, EmptyChair, Hostile, Cooling, BadTarget, CountRefused, NoGift }; // append only

struct GiftTuning {
    std::array<double, kGiftKinds> cooldown{8.0, 6.0, 4.0}; // seconds, 0..60, per gift
    float healShare = 0.15f; // of the target's max HP: 0..1
    int healLoad = 20;       // Load moved off the target: 0..kMaxLoad
    float guardScale = 0.0f; // damage the guarded lead takes, times this: 0..1 (0: the guarded lead takes nothing)
    double guardSeconds = 1.2; // 0..10
};

inline bool validGiftTuning(const GiftTuning& t) {
    for (double c : t.cooldown)
        if (!(c >= 0.0 && c <= 60.0)) return false;
    if (!(t.healShare >= 0.0f && t.healShare <= 1.0f)) return false;
    if (t.healLoad < 0 || t.healLoad > 100) return false;
    if (!(t.guardScale >= 0.0f && t.guardScale <= 1.0f)) return false;
    if (!(t.guardSeconds >= 0.0 && t.guardSeconds <= 10.0)) return false;
    return true;
}

inline constexpr std::uint32_t kGiftSaveVersion = 1;

// The part that outlives a fight, as plain integers for any save format.
struct GiftSave {
    std::uint32_t version = kGiftSaveVersion;
    std::array<std::uint32_t, kGiftKinds> presses{}; // given gifts, per Gift
    std::uint32_t emptyPresses = 0;                  // presses on the empty chair
    std::uint32_t hostilePresses = 0;                // presses while hostile
    std::array<std::uint32_t, 2> threads{};          // per lead (kLeadA, kLeadB)
    std::uint8_t selected = 0;                       // Gift
    std::uint8_t state = 0;                          // SlotState
};

inline std::uint32_t saturatingAdd(std::uint32_t a, std::uint32_t b) {
    return a > std::numeric_limits<std::uint32_t>::max() - b ? std::numeric_limits<std::uint32_t>::max() : a + b;
}

// Presses per gift, save-wide.
struct GiftTally {
    std::array<std::uint32_t, kGiftKinds> presses{};

    std::uint32_t count(Gift g) const {
        return static_cast<std::size_t>(g) < kGiftKinds ? presses[static_cast<std::size_t>(g)] : 0u;
    }
    std::uint64_t total() const {
        std::uint64_t n = 0;
        for (std::uint32_t p : presses) n += p;
        return n;
    }
    // The gift pressed most; the earliest (Heal, Guard, Count) on a tie; None when nothing was given.
    Gift mostUsed() const {
        std::size_t best = kGiftKinds;
        for (std::size_t i = 0; i < kGiftKinds; ++i)
            if (presses[i] > 0 && (best == kGiftKinds || presses[i] > presses[best])) best = i;
        return best == kGiftKinds ? Gift::None : static_cast<Gift>(best);
    }
};

struct GiftResult {
    bool given = false;
    Gift gift = Gift::None;
    FighterId target = kNoFighter;
    GiftError error = GiftError::None;
};

class GiftSlot {
public:
    GiftSlot() { reset(); }

    // Fight state and save-wide state both start over. Returns false (defaults used) on bad tuning.
    bool reset(const GiftTuning& tuning = {}) {
        const bool ok = validGiftTuning(tuning);
        tuning_ = ok ? tuning : GiftTuning{};
        tally_ = GiftTally{};
        emptyPresses_ = 0;
        hostilePresses_ = 0;
        threads_.fill(0);
        selected_ = Gift::Heal;
        state_ = SlotState::Companion;
        resetFight();
        return ok;
    }
    // New tuning, keeping the save-wide tally and the fight state. Returns false (unchanged) on bad tuning.
    bool setTuning(const GiftTuning& tuning) {
        if (!validGiftTuning(tuning)) return false;
        tuning_ = tuning;
        return true;
    }
    // Clears the fight state only (cooldowns, guards, the clock) at a fight's start.
    void resetFight() {
        now_ = 0.0;
        readyAt_.fill(-std::numeric_limits<double>::infinity());
        guardUntil_.fill(-std::numeric_limits<double>::infinity());
    }

    void setState(SlotState s) { state_ = s; }
    SlotState state() const { return state_; }

    // False (nothing changed) for None or out of range.
    bool select(Gift g) {
        if (static_cast<std::size_t>(g) >= kGiftKinds) return false;
        selected_ = g;
        return true;
    }
    Gift selected() const { return selected_; }

    // The button, aimed at lead `target`. `count` is needed for the Count gift.
    GiftResult press(FighterId target, double t, StrikeCount* count = nullptr) {
        t = advance(t);
        GiftResult r;
        r.gift = selected_;
        r.target = target;
        if (state_ == SlotState::Hostile) {
            hostilePresses_ = saturatingAdd(hostilePresses_, 1);
            r.error = GiftError::Hostile;
            return r;
        }
        if (state_ == SlotState::EmptyChair) {
            emptyPresses_ = saturatingAdd(emptyPresses_, 1);
            r.error = GiftError::EmptyChair;
            return r;
        }
        if (!isLead(target)) {
            r.error = GiftError::BadTarget;
            return r;
        }
        const std::size_t g = static_cast<std::size_t>(selected_);
        if (g >= kGiftKinds) {
            r.error = GiftError::NoGift;
            return r;
        }
        if (t < readyAt_[g]) {
            r.error = GiftError::Cooling;
            return r;
        }
        if (selected_ == Gift::Count) {
            if (count == nullptr || count->call(kThirdSlot, t) != CountError::None) {
                r.error = GiftError::CountRefused;
                return r;
            }
        }
        if (selected_ == Gift::Guard) guardUntil_[target] = t + tuning_.guardSeconds;
        readyAt_[g] = t + tuning_.cooldown[g];
        tally_.presses[g] = saturatingAdd(tally_.presses[g], 1);
        threads_[target] = saturatingAdd(threads_[target], 1);
        r.given = true;
        return r;
    }

    // The damage scale on a blow lead `lead` takes at t: guardScale while guarded, else 1.
    float guardScaleFor(FighterId lead, double t) const {
        if (!isLead(lead)) return 1.0f;
        return t < guardUntil_[lead] ? tuning_.guardScale : 1.0f;
    }
    bool guarded(FighterId lead, double t) const { return isLead(lead) && t < guardUntil_[lead]; }
    bool ready(Gift g, double t) const {
        return static_cast<std::size_t>(g) < kGiftKinds && t >= readyAt_[static_cast<std::size_t>(g)];
    }
    double readyAt(Gift g) const {
        return static_cast<std::size_t>(g) < kGiftKinds ? readyAt_[static_cast<std::size_t>(g)]
                                                        : std::numeric_limits<double>::infinity(); // never
    }

    const GiftTally& tally() const { return tally_; }
    std::uint32_t emptyPresses() const { return emptyPresses_; }
    std::uint32_t hostilePresses() const { return hostilePresses_; }
    // Every press in the save: given gifts, the empty chair and while hostile.
    std::uint64_t allPresses() const {
        return tally_.total() + static_cast<std::uint64_t>(emptyPresses_) + static_cast<std::uint64_t>(hostilePresses_);
    }
    std::uint32_t threads(FighterId lead) const { return isLead(lead) ? threads_[lead] : 0u; }
    const GiftTuning& tuning() const { return tuning_; }

    GiftSave save() const {
        GiftSave s;
        s.presses = tally_.presses;
        s.emptyPresses = emptyPresses_;
        s.hostilePresses = hostilePresses_;
        s.threads = threads_;
        s.selected = static_cast<std::uint8_t>(selected_);
        s.state = static_cast<std::uint8_t>(state_);
        return s;
    }
    // False, and nothing changed, for a newer version or an out-of-range enum. The fight state is
    // cleared (a load is never mid-fight).
    bool load(const GiftSave& s) {
        if (s.version == 0 || s.version > kGiftSaveVersion) return false;
        if (s.selected >= kGiftKinds) return false;
        if (s.state > static_cast<std::uint8_t>(SlotState::Hostile)) return false;
        tally_.presses = s.presses;
        emptyPresses_ = s.emptyPresses;
        hostilePresses_ = s.hostilePresses;
        threads_ = s.threads;
        selected_ = static_cast<Gift>(s.selected);
        state_ = static_cast<SlotState>(s.state);
        resetFight();
        return true;
    }

private:
    double advance(double t) {
        now_ = inputTime(now_, t);
        return now_;
    }

    GiftTuning tuning_{};
    GiftTally tally_{};
    std::uint32_t emptyPresses_ = 0;
    std::uint32_t hostilePresses_ = 0;
    std::array<std::uint32_t, 2> threads_{};
    Gift selected_ = Gift::Heal;
    SlotState state_ = SlotState::Companion;
    double now_ = 0.0;
    std::array<double, kGiftKinds> readyAt_{};
    std::array<double, 2> guardUntil_{};
};

} // namespace ftcore
