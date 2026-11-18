#include "ftcore/UiScreens.h"

#include <doctest/doctest.h>

using namespace ftcore;

namespace {

// What may draw under each screen, written out: per mode, per screen state (none, map, party, pause, settings), the
// parts that draw. A part added to UiHudPart must be added here (the count check below fails until it is).
static_assert(kUiHudPartCount == static_cast<std::size_t>(UiHudPart::PartyRows) + 1, "a new part needs a row below");

constexpr std::uint32_t bit(UiHudPart p) { return 1u << static_cast<unsigned>(p); }

constexpr std::uint32_t kPlayExplore = bit(UiHudPart::Reticle) | bit(UiHudPart::MiniMap) | bit(UiHudPart::ControlsLine) |
                                       bit(UiHudPart::FieldHud) | bit(UiHudPart::FightVitals) | bit(UiHudPart::FightAim) |
                                       bit(UiHudPart::FightPrompts) | bit(UiHudPart::FightSpecial);

struct Row {
    UiHudMode mode;
    UiScreenMask open;
    std::uint32_t draws;
    const char* what;
};

constexpr UiScreenMask kMap = uiScreenBit(UiScreen::Map);
constexpr UiScreenMask kParty = uiScreenBit(UiScreen::Party);
constexpr UiScreenMask kPause = uiScreenBit(UiScreen::Pause);
constexpr UiScreenMask kSettings = uiScreenBit(UiScreen::Settings);

constexpr Row kRows[] = {
    {UiHudMode::Explore, 0, kPlayExplore, "explore, play: all of play"},
    {UiHudMode::Explore, kMap, bit(UiHudPart::MapOverlay), "explore, map: the overlay alone (no crosshair)"},
    {UiHudMode::Explore, kParty, bit(UiHudPart::PartyRows), "explore, party: the rows alone"},
    {UiHudMode::Explore, kPause, 0, "explore, pause: nothing"},
    {UiHudMode::Explore, kSettings, 0, "explore, settings: nothing"},
    {UiHudMode::Explore, kPause | kSettings, 0, "explore, settings over pause: nothing"},
    {UiHudMode::Explore, kMap | kPause, 0, "explore, pause over the map: nothing"},
    {UiHudMode::Explore, kParty | kPause, 0, "explore, pause over the party: nothing"},
    {UiHudMode::Fight, 0, kPlayExplore, "fight, play: all of play"},
    {UiHudMode::Fight, kParty, bit(UiHudPart::PartyRows) | bit(UiHudPart::FightVitals),
     "fight, party: the rows and the vitals (the fight runs on)"},
    {UiHudMode::Fight, kPause, 0, "fight, pause: nothing"},
    {UiHudMode::Fight, kSettings, 0, "fight, settings: nothing"},
    {UiHudMode::Fight, kParty | kSettings, 0, "fight, settings over the party: nothing"},
};

} // namespace

TEST_CASE("play HUD under screens: each screen's parts, written out") {
    for (const Row& r : kRows) {
        for (std::size_t p = 0; p < kUiHudPartCount; ++p) {
            const auto part = static_cast<UiHudPart>(p);
            INFO(r.what << ", part " << p);
            CHECK(uiHudDraws(part, r.mode, r.open) == ((r.draws & bit(part)) != 0));
        }
    }
}

TEST_CASE("play HUD under screens: the crosshair never draws with any screen open") {
    for (unsigned open = 1; open < (1u << kUiScreenCount); ++open) {
        CHECK_FALSE(uiHudDraws(UiHudPart::Reticle, UiHudMode::Explore, static_cast<UiScreenMask>(open)));
        CHECK_FALSE(uiHudDraws(UiHudPart::Reticle, UiHudMode::Fight, static_cast<UiScreenMask>(open)));
        CHECK_FALSE(uiHudDraws(UiHudPart::ControlsLine, UiHudMode::Explore, static_cast<UiScreenMask>(open)));
        CHECK_FALSE(uiHudDraws(UiHudPart::MiniMap, UiHudMode::Explore, static_cast<UiScreenMask>(open)));
    }
}
