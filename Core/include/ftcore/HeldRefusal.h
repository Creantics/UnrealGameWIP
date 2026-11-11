#pragma once

// Held refusals: a refusal input made by holding a button, with no meter and no timer. The hold lasts
// as long as the host's prompt does; the host ends the prompt with offerEnded(). See docs/SHARED_COMBAT_INPUTS.md section 5.
//
//   Offered --holdDown--> Holding --offerEnded--> Refused
//      ^                     |
//      +------holdUp---------+   released early: the prompt repeats (repeats() + 1)
//   Offered --accept--> Accepted        Offered or Holding --defer, or offerEnded unheld--> Deferred
//
// * "Not yet" is a yes: an offer that ends unheld, or an explicit defer(), resolves Deferred, and
//   accepted() is true for Accepted and Deferred alike. Only a hold kept to the end refuses.
// * A button already down when the offer starts does not count: it must be let go and pressed again.
// * There is no limit on repeats. Nothing here knows time. No allocation.

#include <cstdint>

namespace ftcore {

enum class RefusalState : std::uint8_t { Idle, Offered, Holding, Refused, Accepted, Deferred }; // append only
enum class RefusalInput : std::uint8_t { Ignored, Holding, RepeatPrompt, Resolved }; // append only

class HeldRefusal {
public:
    // Starts (or restarts) an offer; repeats go back to 0.
    void offer() {
        state_ = RefusalState::Offered;
        repeats_ = 0;
        stale_ = down_;
    }

    RefusalInput holdDown() {
        const bool wasDown = down_;
        down_ = true;
        if (wasDown || stale_ || state_ != RefusalState::Offered) return RefusalInput::Ignored;
        state_ = RefusalState::Holding;
        return RefusalInput::Holding;
    }

    RefusalInput holdUp() {
        down_ = false;
        stale_ = false;
        if (state_ != RefusalState::Holding) return RefusalInput::Ignored;
        state_ = RefusalState::Offered;
        if (repeats_ < kMaxRepeats) ++repeats_;
        return RefusalInput::RepeatPrompt;
    }

    // The player takes the offer (a second button). Only while the prompt is up and not held.
    RefusalInput accept() {
        if (state_ != RefusalState::Offered) return RefusalInput::Ignored;
        state_ = RefusalState::Accepted;
        return RefusalInput::Resolved;
    }

    // "Not yet": counts as a yes.
    RefusalInput defer() {
        if (state_ != RefusalState::Offered && state_ != RefusalState::Holding) return RefusalInput::Ignored;
        state_ = RefusalState::Deferred;
        return RefusalInput::Resolved;
    }

    // The offer is over: a hold still on refuses it; anything else is deferred.
    RefusalInput offerEnded() {
        if (state_ == RefusalState::Holding) state_ = RefusalState::Refused;
        else if (state_ == RefusalState::Offered) state_ = RefusalState::Deferred;
        else return RefusalInput::Ignored;
        return RefusalInput::Resolved;
    }

    RefusalState state() const { return state_; }
    bool resolved() const {
        return state_ == RefusalState::Refused || state_ == RefusalState::Accepted || state_ == RefusalState::Deferred;
    }
    bool refused() const { return state_ == RefusalState::Refused; }
    bool accepted() const { return state_ == RefusalState::Accepted || state_ == RefusalState::Deferred; }
    bool deferred() const { return state_ == RefusalState::Deferred; }
    bool holding() const { return state_ == RefusalState::Holding; }
    int repeats() const { return repeats_; }

    // The button is no longer known to be down (focus lost, device gone): forget it without a release,
    // so a hold in progress stays a hold until the next holdUp(), and a stale press is cleared.
    void clearButton() {
        down_ = false;
        stale_ = false;
    }

private:
    static constexpr int kMaxRepeats = 1 << 30; // saturates
    RefusalState state_ = RefusalState::Idle;
    int repeats_ = 0;
    bool down_ = false;  // the button, as last reported
    bool stale_ = false; // it was down when the offer started and has not been let go
};

} // namespace ftcore
