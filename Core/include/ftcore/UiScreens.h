#pragma once

// What of the play HUD may draw while a screen is open over play: the map overlay, the party screen, the pause menu or
// the settings. One table for the explore HUD and the fight HUD, so a screen never shows the crosshair, the minimap or a
// prompt that belongs to play. Header only: the HUDs ask uiHudDraws for each of their parts every frame.
//
// The rules:
//  - Nothing open: every part of play draws; the map overlay and the party rows do not (they are the screens).
//  - Pause or settings open: none of the play HUD (the game stands still under the menu).
//  - Map open (explore): the map overlay alone.
//  - Party open, explore: the party rows alone.
//  - Party open, fight: the party rows and the fight's vitals (the fight runs on under the screen, so the bars stay);
//    no reticle, no aim, no prompt stack, no special, no controls line (the input belongs to the screen).

#include <cstddef>
#include <cstdint>

namespace ftcore {

enum class UiHudMode : std::uint8_t { Explore, Fight };

// The screens that can stand over play, as bits (two can be open at once: settings opened from the pause menu).
enum class UiScreen : std::uint8_t { Map, Party, Pause, Settings };
inline constexpr std::size_t kUiScreenCount = 4;
using UiScreenMask = std::uint8_t;
constexpr UiScreenMask uiScreenBit(UiScreen s) { return static_cast<UiScreenMask>(1u << static_cast<unsigned>(s)); }

enum class UiHudPart : std::uint8_t {
    Reticle,      // the aim's centre reticle (both modes)
    MiniMap,      // the corner minimap (explore)
    MapOverlay,   // the map overlay (explore): the map screen itself
    ControlsLine, // the controls line along the bottom (both modes)
    FieldHud,     // explore: the objective, the prompt, the notices, the banner and the complete card
    FightVitals,  // fight: the party block, the boss and target bars, the world marks, the events
    FightAim,     // fight: the aim's own marks (PaintAim)
    FightPrompts, // fight: the prompt stack
    FightSpecial, // fight: the special prompt's row
    PartyRows,    // the party screen's rows: the party screen itself
};
inline constexpr std::size_t kUiHudPartCount = 10;

constexpr bool uiScreenOpen(UiScreenMask open, UiScreen s) { return (open & uiScreenBit(s)) != 0; }

// True when `part` may draw in `mode` with the screens `open`.
constexpr bool uiHudDraws(UiHudPart part, UiHudMode mode, UiScreenMask open) {
    if (uiScreenOpen(open, UiScreen::Pause) || uiScreenOpen(open, UiScreen::Settings)) {
        return false;
    }
    if (uiScreenOpen(open, UiScreen::Map)) {
        return part == UiHudPart::MapOverlay;
    }
    if (uiScreenOpen(open, UiScreen::Party)) {
        return part == UiHudPart::PartyRows || (mode == UiHudMode::Fight && part == UiHudPart::FightVitals);
    }
    return part != UiHudPart::MapOverlay && part != UiHudPart::PartyRows;
}

} // namespace ftcore
