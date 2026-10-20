#pragma once

// The title screen's rules, independent of any engine: two items (New Game, Continue), which one
// is selected, which ones can be picked, and what a key does. The screen feeds it keys and acts on
// the returned action; drawing and the save slot live elsewhere. Header only, no allocation.
//
//   ftcore::TitleMenu menu;
//   menu.setContinueEnabled(saveIsUsable);
//   switch (menu.press(ftcore::TitleKey::Confirm)) { case ftcore::TitleAction::NewGame: ... }
//
// Up / Down move to the other enabled item (no wrap, no move onto a greyed item). Confirm picks
// the selected item. Pick1 / Pick2 are the number-key shortcuts for New Game / Continue; a
// shortcut for a greyed item does nothing. Selection starts on Continue when it is enabled, else
// on New Game, and falls back to New Game if Continue gets disabled under it.

#include <cstdint>

namespace ftcore {

enum class TitleItem : std::uint8_t { NewGame = 0, Continue = 1 };

enum class TitleKey : std::uint8_t { Up, Down, Confirm, Pick1, Pick2 };

enum class TitleAction : std::uint8_t {
    None,     // nothing happened (a greyed item, or an edge)
    Moved,    // the selection changed
    NewGame,  // start the story from the beginning
    Continue, // load the save and resume
};

class TitleMenu {
public:
    static constexpr std::uint8_t kItemCount = 2;

    TitleItem selected() const { return selected_; }
    bool enabled(TitleItem item) const { return item == TitleItem::NewGame || continueEnabled_; }
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
        case TitleKey::Up: return move(TitleItem::NewGame);
        case TitleKey::Down: return move(TitleItem::Continue);
        case TitleKey::Confirm: return activate(selected_);
        case TitleKey::Pick1: return activate(TitleItem::NewGame);
        case TitleKey::Pick2: return activate(TitleItem::Continue);
        }
        return TitleAction::None;
    }

private:
    TitleAction move(TitleItem target) {
        if (!enabled(target) || target == selected_) return TitleAction::None;
        selected_ = target;
        return TitleAction::Moved;
    }
    TitleAction activate(TitleItem item) const {
        if (!enabled(item)) return TitleAction::None;
        return item == TitleItem::NewGame ? TitleAction::NewGame : TitleAction::Continue;
    }

    TitleItem selected_ = TitleItem::NewGame;
    bool continueEnabled_ = false;
};

} // namespace ftcore
