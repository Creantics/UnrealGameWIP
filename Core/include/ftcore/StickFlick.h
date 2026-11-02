#pragma once

// The left stick as the arrow keys of the Turns combos (the "Stick flicks" setting, off by default).
// Fed the stick's position each frame, it says which arrow, if any, was flicked this frame.
//
//  - Four ways by the dominant axis (a tie goes to the horizontal); y is up-positive.
//  - A flick fires when the dominant axis crosses `fire` (0.6, the same threshold the Read cards use).
//  - It re-arms when the stick comes back under `rearm` (0.3) on both axes.
//  - The stick must be under `rearm` for a frame before the first flick, so a stick already held over
//    when the combo begins fires nothing.
//  - A flick straight to the opposite direction fires without a rest, so 125 ms alternations work.
//
// Limit: two opposite arrows at once (the jump pairs Left+Right and Up+Down) cannot be flicked
// together; a diagonal serves no pair either, since each frame yields at most one arrow. Both presses
// only have to land inside the shared window, so a quick roll works; a physical d-pad has the same
// limit. Arrow.h belongs to the Turns battle alone (cmake/CheckComboFence.cmake).

#include "ftcore/Api.h"
#include "ftcore/Arrow.h"

#include <optional>

namespace ftcore {

class StickFlick {
public:
    static constexpr float kFire = 0.6f;
    static constexpr float kRearm = 0.3f;

    // The arrow flicked by this frame's stick position (x right-positive, y up-positive).
    FTCORE_API std::optional<Arrow> update(float x, float y);
    // Back to "wait for the stick to come to rest" (a new combo, or the screen was paused).
    void reset() { state_ = State::Unarmed; }

private:
    enum class State : std::uint8_t { Unarmed, Armed, Held };
    State state_ = State::Unarmed;
    Arrow held_ = Arrow::Left;
};

} // namespace ftcore
