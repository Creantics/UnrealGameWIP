#pragma once

// The four step directions of the skill arrow combos (ComboSequence.h). Arrows belong to the Turns
// battle (CombatKind::Rhythm, Battle.h) alone: the Arena and Beat modes never name a combo, which a
// ctest case checks (cmake/CheckComboFence.cmake). Split out of ComboSequence.h so that a file that
// needs only the four directions does not pull in the combo itself.

#include "ftcore/Api.h"

#include <array>
#include <cstdint>

namespace ftcore {

// The four step directions, in dance-pad lane order.
enum class Arrow : std::uint8_t { Left, Down, Up, Right };
inline constexpr std::array<Arrow, 4> kArrows{Arrow::Left, Arrow::Down, Arrow::Up, Arrow::Right};
// "LEFT", "DOWN", "UP", "RIGHT". A literal: valid forever.
FTCORE_API const char* arrowName(Arrow a);

} // namespace ftcore
