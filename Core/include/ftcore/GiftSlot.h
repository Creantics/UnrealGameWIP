#pragma once

// The third party slot's gifts: a heal, a guard and a shared count on one button, never an attack. See
// docs/SHARED_COMBAT_INPUTS.md section 2.
//
// * The slot is a Companion (gifts work), Vacant (the button does nothing) or Hostile (the button does
//   nothing). Every press is tallied, save-wide, in one of three tallies: Companion presses per gift
//   (tally(): given, and refused for a cooldown, a bad target, no gift or a refused count), presses
//   while Vacant (vacantPresses()) and presses while Hostile (hostilePresses()); allPresses() is every
//   press in the save across the three.
// * select(gift) picks what the button gives; press(target, t, count) gives it to a lead. A given gift
//   cannot be taken back (there is no cancel). Each gift has its own cooldown. The Count gift calls
//   `count` with the third slot as the caller; when there is no count or it refuses, the gift is not
//   given.
// * Every given gift leaves one thread on its target, forever, and starts its cooldown. A refused press
//   does neither: it is only tallied.
// * The tally and the threads survive save and load (GiftSave). Counters saturate, never wrap.
//
// What the host applies, from GiftResult: Heal (healShare of the target's max HP, and healLoad Load
// moved off it), Guard (guardScaleFor(lead, t) on every blow the lead takes, until guardUntil), Count
// (already called). Times as CombatInputs.h (queries included). No allocation.

#include "ftcore/CombatInputs.h"
#include "ftcore/StrikeCount.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ftcore {

enum class Gift : std::uint8_t { Heal, Guard, Count, None }; // append only; None: no gift
inline constexpr std::size_t kGiftKinds = 3;                 // Heal, Guard, Count
enum class SlotState : std::uint8_t { Companion, Vacant, Hostile }; // append only
enum class GiftError : std::uint8_t { None, Vacant, Hostile, Cooling, BadTarget, CountRefused, NoGift }; // append only

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

// 2: refused presses per gift; the per-gift and vacant counts renamed. Version 1 still loads (refused 0).
inline constexpr std::uint32_t kGiftSaveVersion = 2;
// Refused presses are kept per Gift, with Gift::None (a press with no gift selected) last.
inline constexpr std::size_t kGiftRefusedSlots = kGiftKinds + 1;

// The part that outlives a fight, as plain integers for any save format.
struct GiftSave {
    std::uint32_t version = kGiftSaveVersion;
    std::array<std::uint32_t, kGiftKinds> given{};          // presses that gave the gift, per Gift
    std::array<std::uint32_t, kGiftRefusedSlots> refused{}; // refused Companion presses, per Gift (None last); version 2
    std::uint32_t vacantPresses = 0;                        // presses while Vacant
    std::uint32_t hostilePresses = 0;                       // presses while Hostile
    std::array<std::uint32_t, 2> threads{};                 // per lead (kLeadA, kLeadB)
    std::uint8_t selected = 0;                              // Gift
    std::uint8_t state = 0;                                 // SlotState
};

inline std::uint32_t saturatingAdd(std::uint32_t a, std::uint32_t b) {
    return a > std::numeric_limits<std::uint32_t>::max() - b ? std::numeric_limits<std::uint32_t>::max() : a + b;
}

// Companion presses per gift, save-wide: given and refused.
struct GiftTally {
    std::array<std::uint32_t, kGiftKinds> given{};
    std::array<std::uint32_t, kGiftRefusedSlots> refused{}; // per Gift, None last

    std::uint32_t givenOf(Gift g) const {
        return static_cast<std::size_t>(g) < kGiftKinds ? given[static_cast<std::size_t>(g)] : 0u;
    }
    std::uint32_t refusedOf(Gift g) const {
        return static_cast<std::size_t>(g) < kGiftRefusedSlots ? refused[static_cast<std::size_t>(g)] : 0u;
    }
    // Every press of gift g, given or refused.
    std::uint64_t count(Gift g) const {
        return static_cast<std::uint64_t>(givenOf(g)) + static_cast<std::uint64_t>(refusedOf(g));
    }
    // Every Companion press, given or refused.
    std::uint64_t total() const {
        std::uint64_t n = 0;
        for (std::uint32_t p : given) n += p;
        for (std::uint32_t p : refused) n += p;
        return n;
    }
    // The gift pressed most (given or refused); the earliest (Heal, Guard, Count) on a tie; None when no
    // gift was pressed.
    Gift mostUsed() const {
        std::size_t best = kGiftKinds;
        std::uint64_t bestCount = 0;
        for (std::size_t i = 0; i < kGiftKinds; ++i) {
            const std::uint64_t n = count(static_cast<Gift>(i));
            if (n > bestCount) {
                best = i;
                bestCount = n;
            }
        }
        return best == kGiftKinds ? Gift::None : static_cast<Gift>(best);
    }
};

struct GiftResult {
    bool given = false;
    Gift gift = Gift::None;
    FighterId target = kNoFighter;
    GiftError error = GiftError::None;
    // A given Heal: what the host applies to the target.
    float healShare = 0.0f; // of the target's max HP
    int healLoad = 0;       // Load moved off the target
    // A given Guard: the target takes x guardScale damage until guardUntil (fight clock).
    float guardScale = 1.0f;
    double guardUntil = 0.0;
};

class GiftSlot {
public:
    GiftSlot() { reset(); }

    // Fight state and save-wide state both start over. Returns false (defaults used) on bad tuning.
    bool reset(const GiftTuning& tuning = {}, double t = 0.0) {
        const bool ok = validGiftTuning(tuning);
        tuning_ = ok ? tuning : GiftTuning{};
        tally_ = GiftTally{};
        vacantPresses_ = 0;
        hostilePresses_ = 0;
        threads_.fill(0);
        selected_ = Gift::Heal;
        state_ = SlotState::Companion;
        resetFight(t);
        return ok;
    }
    // New tuning, keeping the save-wide tally and the fight state. Returns false (unchanged) on bad tuning.
    bool setTuning(const GiftTuning& tuning) {
        if (!validGiftTuning(tuning)) return false;
        tuning_ = tuning;
        return true;
    }
    // Clears the fight state only (cooldowns, guards) and sets the clock to the fight's start t.
    void resetFight(double t = 0.0) {
        now_ = startTime(t);
        readyAt_.fill(-std::numeric_limits<double>::infinity());
        guardUntil_.fill(-std::numeric_limits<double>::infinity());
    }

    // The per-frame tick: moves the clock to t (CombatInputs.h rules). Call it every frame, so a
    // long stretch without presses never leaves the clock more than kMaxTimeJump behind.
    void update(double t) { advance(t); }

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
        if (state_ == SlotState::Vacant) {
            vacantPresses_ = saturatingAdd(vacantPresses_, 1);
            r.error = GiftError::Vacant;
            return r;
        }
        const std::size_t g = static_cast<std::size_t>(selected_);
        if (!isLead(target)) return refuse(r, GiftError::BadTarget);
        if (g >= kGiftKinds) return refuse(r, GiftError::NoGift);
        if (t < readyAt_[g]) return refuse(r, GiftError::Cooling);
        if (selected_ == Gift::Count) {
            if (count == nullptr || count->call(kThirdSlot, t) != CountError::None)
                return refuse(r, GiftError::CountRefused);
        }
        if (selected_ == Gift::Heal) {
            r.healShare = tuning_.healShare;
            r.healLoad = tuning_.healLoad;
        }
        if (selected_ == Gift::Guard) {
            guardUntil_[target] = t + tuning_.guardSeconds;
            r.guardScale = tuning_.guardScale;
            r.guardUntil = guardUntil_[target];
        }
        readyAt_[g] = t + tuning_.cooldown[g];
        tally_.given[g] = saturatingAdd(tally_.given[g], 1);
        threads_[target] = saturatingAdd(threads_[target], 1);
        r.given = true;
        return r;
    }

    // The damage scale on a blow lead `lead` takes at t: guardScale while guarded, else 1.
    float guardScaleFor(FighterId lead, double t) const { return guarded(lead, t) ? tuning_.guardScale : 1.0f; }
    bool guarded(FighterId lead, double t) const { return isLead(lead) && inputTime(now_, t) < guardUntil_[lead]; }
    bool ready(Gift g, double t) const {
        return static_cast<std::size_t>(g) < kGiftKinds && inputTime(now_, t) >= readyAt_[static_cast<std::size_t>(g)];
    }
    double readyAt(Gift g) const {
        return static_cast<std::size_t>(g) < kGiftKinds ? readyAt_[static_cast<std::size_t>(g)]
                                                        : std::numeric_limits<double>::infinity(); // never
    }

    const GiftTally& tally() const { return tally_; }
    std::uint32_t vacantPresses() const { return vacantPresses_; }
    std::uint32_t hostilePresses() const { return hostilePresses_; }
    // Every press in the save: Companion presses (given or refused), Vacant and Hostile.
    std::uint64_t allPresses() const {
        return tally_.total() + static_cast<std::uint64_t>(vacantPresses_) + static_cast<std::uint64_t>(hostilePresses_);
    }
    std::uint32_t threads(FighterId lead) const { return isLead(lead) ? threads_[lead] : 0u; }
    const GiftTuning& tuning() const { return tuning_; }
    double now() const { return now_; }

    GiftSave save() const {
        GiftSave s;
        s.given = tally_.given;
        s.refused = tally_.refused;
        s.vacantPresses = vacantPresses_;
        s.hostilePresses = hostilePresses_;
        s.threads = threads_;
        s.selected = static_cast<std::uint8_t>(selected_);
        s.state = static_cast<std::uint8_t>(state_);
        return s;
    }
    // False, and nothing changed, for version 0, a newer version or an out-of-range enum. A version 1
    // save has no refused presses (they read as 0). The fight state is cleared (a load is never
    // mid-fight); the clock is kept.
    bool load(const GiftSave& s) {
        if (s.version == 0 || s.version > kGiftSaveVersion) return false;
        if (s.selected >= kGiftKinds) return false;
        if (s.state > static_cast<std::uint8_t>(SlotState::Hostile)) return false;
        tally_.given = s.given;
        if (s.version >= 2) tally_.refused = s.refused;
        else tally_.refused.fill(0);
        vacantPresses_ = s.vacantPresses;
        hostilePresses_ = s.hostilePresses;
        threads_ = s.threads;
        selected_ = static_cast<Gift>(s.selected);
        state_ = static_cast<SlotState>(s.state);
        resetFight(now_); // the clock stays where it was
        return true;
    }

private:
    double advance(double t) {
        now_ = inputTime(now_, t);
        return now_;
    }

    // A Companion press that gives nothing: tallied under the selected gift, no thread, no cooldown.
    GiftResult& refuse(GiftResult& r, GiftError error) {
        std::size_t i = static_cast<std::size_t>(selected_);
        if (i >= kGiftRefusedSlots) i = kGiftRefusedSlots - 1;
        tally_.refused[i] = saturatingAdd(tally_.refused[i], 1);
        r.error = error;
        return r;
    }

    GiftTuning tuning_{};
    GiftTally tally_{};
    std::uint32_t vacantPresses_ = 0;
    std::uint32_t hostilePresses_ = 0;
    std::array<std::uint32_t, 2> threads_{};
    Gift selected_ = Gift::Heal;
    SlotState state_ = SlotState::Companion;
    double now_ = 0.0;
    std::array<double, kGiftKinds> readyAt_{};
    std::array<double, 2> guardUntil_{};
};

} // namespace ftcore
