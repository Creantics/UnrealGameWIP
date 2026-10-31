#pragma once

// The four action buttons that answer a prompt outside the Turns battle's arrow combos: the
// quick-time event (Qte.h), the mini-game (Mini.h) and, later, the Beat mode's prompts. Arrows
// (Arrow.h) stay in Turns; everywhere else the player answers with these.
//
// The order is the pad's face buttons clockwise from the left, which the Unreal screens map to the
// face buttons and to the keyboard's I, J, K, L diamond in the same layout:
//   Attack  face left    J
//   Guard   face top     I
//   Dodge   face right   L
//   Jump    face bottom  K
// Data spells them "attack", "guard", "dodge" and "jump".

#include "ftcore/Api.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ftcore {

enum class ActionButton : std::uint8_t { Attack, Guard, Dodge, Jump };
inline constexpr std::size_t kActionButtonCount = 4;
inline constexpr std::array<ActionButton, kActionButtonCount> kActionButtons{
    ActionButton::Attack, ActionButton::Guard, ActionButton::Dodge, ActionButton::Jump};

// "ATTACK", "GUARD", "DODGE", "JUMP". A literal: valid forever.
FTCORE_API const char* actionButtonName(ActionButton b);
// The data spelling: "attack", "guard", "dodge", "jump". A literal.
FTCORE_API const char* actionButtonKey(ActionButton b);
// Reads a data spelling into `out`; false (and `out` untouched) for anything else.
FTCORE_API bool parseActionButton(std::string_view key, ActionButton& out);

} // namespace ftcore
