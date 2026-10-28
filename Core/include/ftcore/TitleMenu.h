#pragma once

// The title screen's rules, independent of any engine: four items (New Game, Continue, Settings,
// Quit), which one is selected, which ones can be picked, and what a key does. The screen feeds it
// keys and acts on the returned action; drawing and the save slot live elsewhere. Header only, no
// allocation.
//
//   ftcore::TitleMenu menu;
//   menu.setContinueEnabled(saveIsUsable);
//   switch (menu.press(ftcore::TitleKey::Confirm)) { case ftcore::TitleAction::NewGame: ... }
//
// Up / Down move to the previous / next enabled item (no wrap, no move onto a greyed item, so with
// Continue greyed Down goes from New Game to Settings, and Down from Settings reaches Quit). Confirm
// picks the selected item. Pick1 / Pick2 / Pick3 / Pick4 are the number-key shortcuts for New Game /
// Continue / Settings / Quit; a shortcut for a greyed item does nothing. Selection starts on Continue
// when it is enabled, else on New Game, and falls back to New Game if Continue gets disabled under
// it. Settings and Quit are always enabled. Quit needs no confirm step: nothing is open on the title,
// and the story autosaves at every scene start.

#include <cstdint>

namespace ftcore {

enum class TitleItem : std::uint8_t { NewGame = 0, Continue = 1, Settings = 2, Quit = 3 };

enum class TitleKey : std::uint8_t { Up, Down, Confirm, Pick1, Pick2, Pick3, Pick4 };

enum class TitleAction : std::uint8_t {
    None,     // nothing happened (a greyed item, or an edge)
    Moved,    // the selection changed
    NewGame,  // start the story from the beginning
    Continue, // load the save and resume
    Settings, // open the settings menu
    Quit,     // leave the game
};

class TitleMenu {
public:
    static constexpr std::uint8_t kItemCount = 4;

    TitleItem selected() const { return selected_; }
    bool enabled(TitleItem item) const { return item != TitleItem::Continue || continueEnabled_; }
    bool continueEnabled() const { return continueEnabled_; }

    // Enabling Continue (a usable save found) puts the selection on it; disabling it moves the
    // selection to New Game.
    void setContinueEnabled(bool enabled) {
        if (enabled == continueEnabled_) return;
        continueEnabled_ = enabled;
        selected_ = enabled ? TitleItem::Continue : TitleItem::NewGame;
    }

    TitleAction press(TitleKey key) {
        switch (key) {
        case TitleKey::Up: return move(-1);
        case TitleKey::Down: return move(1);
        case TitleKey::Confirm: return activate(selected_);
        case TitleKey::Pick1: return activate(TitleItem::NewGame);
        case TitleKey::Pick2: return activate(TitleItem::Continue);
        case TitleKey::Pick3: return activate(TitleItem::Settings);
        case TitleKey::Pick4: return activate(TitleItem::Quit);
        }
        return TitleAction::None;
    }

    // The mouse is over `item`: selects it when it is enabled.
    void hover(TitleItem item) {
        if (enabled(item)) selected_ = item;
    }

private:
    TitleAction move(int step) {
        for (int i = static_cast<int>(selected_) + step; i >= 0 && i < kItemCount; i += step) {
            const TitleItem item = static_cast<TitleItem>(i);
            if (!enabled(item)) continue;
            selected_ = item;
            return TitleAction::Moved;
        }
        return TitleAction::None;
    }
    TitleAction activate(TitleItem item) const {
        if (!enabled(item)) return TitleAction::None;
        switch (item) {
        case TitleItem::NewGame: return TitleAction::NewGame;
        case TitleItem::Continue: return TitleAction::Continue;
        case TitleItem::Settings: return TitleAction::Settings;
        case TitleItem::Quit: return TitleAction::Quit;
        }
        return TitleAction::None;
    }

    TitleItem selected_ = TitleItem::NewGame;
    bool continueEnabled_ = false;
};

} // namespace ftcore
