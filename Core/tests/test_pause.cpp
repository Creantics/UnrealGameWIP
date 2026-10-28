// The pause menu's rules: the list, the quit prompt, greyed items, the mouse and starting over. No
// story data anywhere.

#include "ftcore/PauseMenu.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <initializer_list>

using namespace ftcore;

namespace {

// Moves the selection down the list until it is on `item`.
void goTo(PauseMenu& m, PauseItem item) {
    for (int i = 0; i < 8 && m.selected() != item; ++i) m.press(PauseKey::Down);
    REQUIRE(m.selected() == item);
}

} // namespace

TEST_CASE("pause menu starts on Resume with every item enabled") {
    static_assert(PauseMenu::kItemCount == 4);
    PauseMenu m;
    CHECK(m.selected() == PauseItem::Resume);
    CHECK_FALSE(m.confirming());
    CHECK_FALSE(m.confirmYes());
    for (PauseItem item : {PauseItem::Resume, PauseItem::Settings, PauseItem::QuitToTitle, PauseItem::QuitGame}) CHECK(m.enabled(item));
    m.open();
    CHECK(m.selected() == PauseItem::Resume);
    CHECK_FALSE(m.confirming());
}

TEST_CASE("pause menu: Up and Down step through the items without wrapping") {
    PauseMenu m;
    m.open();
    CHECK(m.press(PauseKey::Up) == PauseAction::None); // at the top
    CHECK(m.selected() == PauseItem::Resume);
    CHECK(m.press(PauseKey::Down) == PauseAction::Moved);
    CHECK(m.selected() == PauseItem::Settings);
    CHECK(m.press(PauseKey::Down) == PauseAction::Moved);
    CHECK(m.selected() == PauseItem::QuitToTitle);
    CHECK(m.press(PauseKey::Down) == PauseAction::Moved);
    CHECK(m.selected() == PauseItem::QuitGame);
    CHECK(m.press(PauseKey::Down) == PauseAction::None); // at the bottom: no wrap
    CHECK(m.selected() == PauseItem::QuitGame);
    CHECK(m.press(PauseKey::Up) == PauseAction::Moved);
    CHECK(m.selected() == PauseItem::QuitToTitle);
    CHECK_FALSE(m.confirming()); // moving never opens a prompt
}

TEST_CASE("pause menu: Confirm on Resume and Back on the list resume") {
    PauseMenu m;
    m.open();
    CHECK(m.press(PauseKey::Confirm) == PauseAction::Resume);
    CHECK(m.press(PauseKey::Back) == PauseAction::Resume);
    CHECK(m.selected() == PauseItem::Resume); // neither moves anything
    CHECK_FALSE(m.confirming());
    // Back resumes from any row of the list.
    goTo(m, PauseItem::QuitGame);
    CHECK(m.press(PauseKey::Back) == PauseAction::Resume);
    CHECK(m.selected() == PauseItem::QuitGame);
    CHECK_FALSE(m.confirming());
}

TEST_CASE("pause menu: Confirm on Settings opens the settings and keeps the list where it was") {
    PauseMenu m;
    m.open();
    CHECK(m.press(PauseKey::Down) == PauseAction::Moved);
    CHECK(m.selected() == PauseItem::Settings);
    CHECK(m.press(PauseKey::Confirm) == PauseAction::OpenSettings);
    CHECK(m.selected() == PauseItem::Settings); // closing the settings lands back here
    CHECK_FALSE(m.confirming());
    CHECK(m.press(PauseKey::Confirm) == PauseAction::OpenSettings); // and it can be opened again
}

TEST_CASE("pause menu: the quit items ask first, with No selected") {
    for (PauseItem quit : {PauseItem::QuitToTitle, PauseItem::QuitGame}) {
        PauseMenu m;
        m.open();
        goTo(m, quit);
        CHECK(m.press(PauseKey::Confirm) == PauseAction::Prompt);
        CHECK(m.confirming());
        CHECK_FALSE(m.confirmYes()); // No is the default
        CHECK(m.selected() == quit); // the selection stays on the item the prompt is about
        // Confirm on the default (No) goes back to the list on that item.
        CHECK(m.press(PauseKey::Confirm) == PauseAction::Moved);
        CHECK_FALSE(m.confirming());
        CHECK(m.selected() == quit);
        // And asking again starts on No again.
        CHECK(m.press(PauseKey::Confirm) == PauseAction::Prompt);
        CHECK_FALSE(m.confirmYes());
    }
}

TEST_CASE("pause menu: Yes confirms the quit, whichever way the prompt was flipped") {
    PauseMenu m;
    m.open();
    goTo(m, PauseItem::QuitToTitle);
    CHECK(m.press(PauseKey::Confirm) == PauseAction::Prompt);
    CHECK(m.press(PauseKey::Up) == PauseAction::Moved);
    CHECK(m.confirmYes());
    CHECK(m.press(PauseKey::Confirm) == PauseAction::QuitToTitle);
    CHECK_FALSE(m.confirming()); // the prompt is spent
    CHECK_FALSE(m.confirmYes());
    CHECK(m.selected() == PauseItem::QuitToTitle);

    goTo(m, PauseItem::QuitGame);
    CHECK(m.press(PauseKey::Confirm) == PauseAction::Prompt);
    CHECK(m.press(PauseKey::Down) == PauseAction::Moved);
    CHECK(m.confirmYes());
    CHECK(m.press(PauseKey::Confirm) == PauseAction::QuitGame);
    CHECK_FALSE(m.confirming());
    // Spent means spent: the next Confirm asks again rather than quitting.
    CHECK(m.press(PauseKey::Confirm) == PauseAction::Prompt);
    CHECK_FALSE(m.confirmYes());
}

TEST_CASE("pause menu: Up and Down toggle Yes and No in the prompt") {
    PauseMenu m;
    m.open();
    goTo(m, PauseItem::QuitGame);
    REQUIRE(m.press(PauseKey::Confirm) == PauseAction::Prompt);
    for (int round = 0; round < 3; ++round) {
        CHECK(m.press(PauseKey::Up) == PauseAction::Moved);
        CHECK(m.confirmYes());
        CHECK(m.press(PauseKey::Up) == PauseAction::Moved); // there are only two: any direction flips
        CHECK_FALSE(m.confirmYes());
        CHECK(m.press(PauseKey::Down) == PauseAction::Moved);
        CHECK(m.confirmYes());
        CHECK(m.press(PauseKey::Down) == PauseAction::Moved);
        CHECK_FALSE(m.confirmYes());
    }
    CHECK(m.selected() == PauseItem::QuitGame); // flipping never moves the list
    CHECK(m.confirming());
}

TEST_CASE("pause menu: Back from each state") {
    PauseMenu m;
    m.open();
    // The list: Back resumes.
    CHECK(m.press(PauseKey::Back) == PauseAction::Resume);

    // The prompt on No: Back closes the prompt and lands on the item; a second Back resumes.
    goTo(m, PauseItem::QuitToTitle);
    REQUIRE(m.press(PauseKey::Confirm) == PauseAction::Prompt);
    CHECK(m.press(PauseKey::Back) == PauseAction::Moved);
    CHECK_FALSE(m.confirming());
    CHECK(m.selected() == PauseItem::QuitToTitle);
    CHECK(m.press(PauseKey::Back) == PauseAction::Resume);

    // The prompt on Yes: Back is a No, not a Yes, and Yes doesn't stick to the next prompt.
    REQUIRE(m.press(PauseKey::Confirm) == PauseAction::Prompt);
    REQUIRE(m.press(PauseKey::Up) == PauseAction::Moved);
    REQUIRE(m.confirmYes());
    CHECK(m.press(PauseKey::Back) == PauseAction::Moved);
    CHECK_FALSE(m.confirming());
    CHECK_FALSE(m.confirmYes());
    CHECK(m.selected() == PauseItem::QuitToTitle);
    CHECK(m.press(PauseKey::Confirm) == PauseAction::Prompt);
    CHECK_FALSE(m.confirmYes());

    // Back from the settings row is the list's Back too.
    PauseMenu s;
    s.open();
    s.press(PauseKey::Down);
    REQUIRE(s.press(PauseKey::Confirm) == PauseAction::OpenSettings);
    CHECK(s.press(PauseKey::Back) == PauseAction::Resume);
}

TEST_CASE("pause menu: greyed items are skipped by the keys and the mouse") {
    PauseMenu m;
    m.setEnabled(PauseItem::Settings, false);
    m.open();
    CHECK_FALSE(m.enabled(PauseItem::Settings));
    CHECK(m.enabled(PauseItem::QuitToTitle));
    CHECK(m.press(PauseKey::Down) == PauseAction::Moved); // steps over the greyed Settings
    CHECK(m.selected() == PauseItem::QuitToTitle);
    CHECK(m.press(PauseKey::Up) == PauseAction::Moved); // and back over it
    CHECK(m.selected() == PauseItem::Resume);
    m.hover(PauseItem::Settings); // the mouse can't land on it either
    CHECK(m.selected() == PauseItem::Resume);

    m.setEnabled(PauseItem::Settings, true); // restored
    CHECK(m.enabled(PauseItem::Settings));
    CHECK(m.press(PauseKey::Down) == PauseAction::Moved);
    CHECK(m.selected() == PauseItem::Settings);
    CHECK(m.press(PauseKey::Confirm) == PauseAction::OpenSettings);

    // A greyed last row moves the bottom edge up.
    PauseMenu n;
    n.setEnabled(PauseItem::QuitGame, false);
    n.open();
    goTo(n, PauseItem::QuitToTitle);
    CHECK(n.press(PauseKey::Down) == PauseAction::None);
    CHECK(n.selected() == PauseItem::QuitToTitle);
}

TEST_CASE("pause menu: greying the selected item moves the selection") {
    PauseMenu m;
    m.open();
    goTo(m, PauseItem::QuitToTitle);
    m.setEnabled(PauseItem::QuitToTitle, false);
    CHECK(m.selected() == PauseItem::Resume); // the first enabled item

    // A prompt about the item that gets greyed closes with it.
    PauseMenu p;
    p.open();
    goTo(p, PauseItem::QuitGame);
    REQUIRE(p.press(PauseKey::Confirm) == PauseAction::Prompt);
    REQUIRE(p.press(PauseKey::Up) == PauseAction::Moved);
    p.setEnabled(PauseItem::QuitGame, false);
    CHECK_FALSE(p.confirming());
    CHECK_FALSE(p.confirmYes());
    CHECK(p.selected() == PauseItem::Resume);

    // Greying a different item leaves the prompt alone.
    PauseMenu q;
    q.open();
    goTo(q, PauseItem::QuitToTitle);
    REQUIRE(q.press(PauseKey::Confirm) == PauseAction::Prompt);
    q.setEnabled(PauseItem::Settings, false);
    CHECK(q.confirming());
    CHECK(q.selected() == PauseItem::QuitToTitle);
}

TEST_CASE("pause menu: Resume can be greyed, and Back still resumes") {
    PauseMenu m;
    m.setEnabled(PauseItem::Resume, false);
    m.open();
    CHECK(m.selected() == PauseItem::Settings); // opens on the first enabled item
    CHECK(m.press(PauseKey::Up) == PauseAction::None); // Resume is skipped going up too
    CHECK(m.selected() == PauseItem::Settings);
    m.hover(PauseItem::Resume);
    CHECK(m.selected() == PauseItem::Settings);
    CHECK(m.press(PauseKey::Back) == PauseAction::Resume); // always a way out
}

TEST_CASE("pause menu: with every item greyed nothing can be picked but Back still resumes") {
    PauseMenu m;
    for (PauseItem item : {PauseItem::Resume, PauseItem::Settings, PauseItem::QuitToTitle, PauseItem::QuitGame}) m.setEnabled(item, false);
    m.open();
    CHECK(m.press(PauseKey::Confirm) == PauseAction::None);
    CHECK(m.press(PauseKey::Down) == PauseAction::None);
    CHECK(m.press(PauseKey::Up) == PauseAction::None);
    CHECK_FALSE(m.confirming());
    CHECK(m.press(PauseKey::Back) == PauseAction::Resume);

    m.setEnabled(PauseItem::Settings, true);
    m.open();
    CHECK(m.selected() == PauseItem::Settings);
    CHECK(m.press(PauseKey::Confirm) == PauseAction::OpenSettings);
}

TEST_CASE("pause menu: the mouse selects on the list and on the prompt, each in its own state") {
    PauseMenu m;
    m.open();
    m.hover(PauseItem::QuitGame);
    CHECK(m.selected() == PauseItem::QuitGame);
    m.hover(PauseItem::Settings);
    CHECK(m.selected() == PauseItem::Settings);
    m.hoverChoice(true); // no prompt: nothing to hover
    CHECK_FALSE(m.confirmYes());
    CHECK_FALSE(m.confirming());

    m.hover(PauseItem::QuitToTitle);
    REQUIRE(m.press(PauseKey::Confirm) == PauseAction::Prompt);
    m.hover(PauseItem::Resume); // the list is covered by the prompt
    CHECK(m.selected() == PauseItem::QuitToTitle);
    m.hoverChoice(true);
    CHECK(m.confirmYes());
    m.hoverChoice(false);
    CHECK_FALSE(m.confirmYes());
    // A click on the Yes under the mouse: hover, then Confirm.
    m.hoverChoice(true);
    CHECK(m.press(PauseKey::Confirm) == PauseAction::QuitToTitle);
}

TEST_CASE("pause menu: open starts over but keeps the greyed items") {
    PauseMenu m;
    m.setEnabled(PauseItem::Settings, false);
    m.open();
    goTo(m, PauseItem::QuitGame);
    REQUIRE(m.press(PauseKey::Confirm) == PauseAction::Prompt);
    REQUIRE(m.press(PauseKey::Up) == PauseAction::Moved);
    REQUIRE(m.confirmYes());

    m.open();
    CHECK(m.selected() == PauseItem::Resume);
    CHECK_FALSE(m.confirming());
    CHECK_FALSE(m.confirmYes());
    CHECK_FALSE(m.enabled(PauseItem::Settings)); // the flags belong to the screen, not to one opening
    CHECK(m.press(PauseKey::Confirm) == PauseAction::Resume);
    CHECK(m.press(PauseKey::Down) == PauseAction::Moved);
    CHECK(m.selected() == PauseItem::QuitToTitle);
}

TEST_CASE("pause menu allocates nothing") {
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    PauseMenu m;
    int acted = 0;
    for (int round = 0; round < 200; ++round) {
        m.open();
        for (PauseKey k : {PauseKey::Down, PauseKey::Down, PauseKey::Confirm, PauseKey::Up, PauseKey::Confirm, PauseKey::Back,
                           PauseKey::Up, PauseKey::Confirm}) {
            if (m.press(k) != PauseAction::None) ++acted;
        }
        m.hover(PauseItem::Settings);
        m.hoverChoice(true);
        m.setEnabled(PauseItem::QuitGame, (round & 1) == 0);
    }
    const auto after = ftcore_test::allocStats().count;
    CHECK(acted > 0);
    if (counting) CHECK(after == before);
}
