#pragma once

// Which combat system a fight plays (docs/COMBAT_SYSTEMS.md). Each fight names its own in
// Data/Battle/encounters.json (EncounterEntry::combat, EncounterTable::combatFor), by what the fight
// is, not where it is:
//
//   Rhythm  Turns: the turn-based battle with the skill arrow combos (Battle.h, the battle screen)
//   Arena   real-time melee inside a ring (ArenaFight.h)
//   Beat    a real-time fight on the music's beat grid (BeatFight.h)
//
// A fight with no `combat` tag, and one not listed, plays Rhythm. A host that can't run a
// fight's system yet plays Rhythm instead (playableCombat), so every fight stays playable while the
// other systems' screens are built. Header only; nothing here allocates.

#include <cstdint>
#include <string_view>

namespace ftcore {

// Append only: saves and the Unreal side store the value.
enum class CombatKind : std::uint8_t { Rhythm, Arena, Beat };
inline constexpr std::uint8_t kCombatKinds = 3;

// "rhythm", "arena", "beat" (the encounters.json spelling). A literal: valid forever.
inline const char* combatKindName(CombatKind k) {
    switch (k) {
    case CombatKind::Arena: return "arena";
    case CombatKind::Beat: return "beat";
    default: return "rhythm";
    }
}

// The encounters.json spelling to a kind; false (and `out` untouched) for anything else.
inline bool parseCombatKind(std::string_view text, CombatKind& out) {
    for (std::uint8_t i = 0; i < kCombatKinds; ++i) {
        const CombatKind k = static_cast<CombatKind>(i);
        if (text == combatKindName(k)) {
            out = k;
            return true;
        }
    }
    return false;
}

// One bit per kind, for playableCombat's mask.
inline constexpr std::uint32_t combatBit(CombatKind k) { return 1u << static_cast<std::uint32_t>(k); }

// `want` when the host can run it (its bit is set in `supported`), else Rhythm, which every host runs.
inline constexpr CombatKind playableCombat(CombatKind want, std::uint32_t supported) {
    return (supported & combatBit(want)) != 0u ? want : CombatKind::Rhythm;
}

} // namespace ftcore
