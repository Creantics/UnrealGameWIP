#pragma once

// The pause menu's rules, independent of any engine: four items (Resume, Settings, Quit to Title,
// Quit Game), which one is selected, which ones can be picked, and what a key does. The screen feeds
// it keys and acts on the returned action; drawing, pausing the game and the settings overlay live
// elsewhere. Header only, no allocation.
//
//   ftcore::PauseMenu menu;
//   menu.open();
//   switch (menu.press(ftcore::PauseKey::Confirm)) { case ftcore::PauseAction::Resume: ... }
//
// open() puts the selection on Resume and closes the quit prompt. The enabled flags stay as they
// are: a screen that can't offer an item greys it once with setEnabled. Up / Down move to the
// previous / next enabled item (no wrap, no move onto a greyed item, like the title menu). Confirm
// picks the selected item: Resume gives Resume and Settings gives OpenSettings. Back on the list gives
// Resume whatever Resume's flag says, so there is always a way out.
//
// Quit to Title and Quit Game ask first. Confirm on either opens the prompt (Prompt) with No
// selected. Up / Down flip between Yes and No (there are only two, so any direction flips). Confirm on
// Yes gives QuitToTitle or QuitGame and closes the prompt. Confirm on No, or Back, closes the prompt
// and leaves the selection on the item it came from.
//
// The mouse: hover(item) selects an enabled item on the list, hoverChoice(yes) selects Yes or No while
// the prompt is up. Each does nothing in the other state, and nothing for a greyed item.

#include <cstdint>

namespace ftcore {

enum class PauseItem : std::uint8_t { Resume = 0, Settings = 1, QuitToTitle = 2, QuitGame = 3 };

enum class PauseKey : std::uint8_t { Up, Down, Confirm, Back };

enum class PauseAction : std::uint8_t {
    None,         // nothing happened (an edge, or a greyed item)
    Moved,        // the selection changed, or the quit prompt flipped or closed
    Prompt,       // a quit item asked to be confirmed: the Yes / No prompt is up, with No selected
    Resume,       // close the menu and carry on
    OpenSettings, // open the settings menu over this one
    QuitToTitle,  // confirmed: leave for the title screen
    QuitGame,     // confirmed: leave the game
};

class PauseMenu {
public:
    static constexpr std::uint8_t kItemCount = 4;

    PauseItem selected() const { return selected_; }
    bool enabled(PauseItem item) const { return (disabled_ & bit(item)) == 0; }
    // The Yes / No prompt of a quit item is up. confirmYes() is its selection (false: No, the default).
    bool confirming() const { return confirming_; }
    bool confirmYes() const { return yes_; }

    // Greys (on false) or restores an item. Greying the selected item moves the selection to the first
    // enabled one and closes a quit prompt that was about it.
    void setEnabled(PauseItem item, bool on) {
        if (on) {
            disabled_ = static_cast<std::uint8_t>(disabled_ & ~bit(item));
            return;
        }
        disabled_ = static_cast<std::uint8_t>(disabled_ | bit(item));
        if (item == selected_) {
            confirming_ = false;
            yes_ = false;
            selectFirstEnabled();
        }
    }

    // A fresh opening: the selection on Resume (or the first enabled item) and no quit prompt.
    void open() {
        confirming_ = false;
        yes_ = false;
        selected_ = PauseItem::Resume;
        if (!enabled(selected_)) selectFirstEnabled();
    }

    PauseAction press(PauseKey key) {
        switch (key) {
        case PauseKey::Up: return move(-1);
        case PauseKey::Down: return move(1);
        case PauseKey::Confirm: return activate();
        case PauseKey::Back: return back();
        }
        return PauseAction::None;
    }

    // The mouse is over `item`: selects it when it is enabled and no prompt is up.
    void hover(PauseItem item) {
        if (!confirming_ && enabled(item)) selected_ = item;
    }
    // The mouse is over Yes (true) or No (false) of the quit prompt.
    void hoverChoice(bool yes) {
        if (confirming_) yes_ = yes;
    }

private:
    static constexpr std::uint8_t bit(PauseItem item) { return static_cast<std::uint8_t>(1u << static_cast<unsigned>(item)); }

    void selectFirstEnabled() {
        for (int i = 0; i < kItemCount; ++i) {
            const PauseItem item = static_cast<PauseItem>(i);
            if (!enabled(item)) continue;
            selected_ = item;
            return;
        }
    }
    PauseAction move(int step) {
        if (confirming_) {
            yes_ = !yes_;
            return PauseAction::Moved;
        }
        for (int i = static_cast<int>(selected_) + step; i >= 0 && i < kItemCount; i += step) {
            const PauseItem item = static_cast<PauseItem>(i);
            if (!enabled(item)) continue;
            selected_ = item;
            return PauseAction::Moved;
        }
        return PauseAction::None;
    }
    PauseAction activate() {
        if (confirming_) {
            const bool yes = yes_;
            confirming_ = false;
            yes_ = false;
            if (!yes) return PauseAction::Moved;
            return selected_ == PauseItem::QuitGame ? PauseAction::QuitGame : PauseAction::QuitToTitle;
        }
        if (!enabled(selected_)) return PauseAction::None;
        switch (selected_) {
        case PauseItem::Resume: return PauseAction::Resume;
        case PauseItem::Settings: return PauseAction::OpenSettings;
        case PauseItem::QuitToTitle:
        case PauseItem::QuitGame:
            confirming_ = true;
            yes_ = false;
            return PauseAction::Prompt;
        }
        return PauseAction::None;
    }
    PauseAction back() {
        if (confirming_) {
            confirming_ = false;
            yes_ = false;
            return PauseAction::Moved;
        }
        return PauseAction::Resume;
    }

    PauseItem selected_ = PauseItem::Resume;
    std::uint8_t disabled_ = 0; // one bit per greyed item
    bool confirming_ = false;
    bool yes_ = false;
};

} // namespace ftcore
