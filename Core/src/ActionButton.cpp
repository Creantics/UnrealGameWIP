#include "ftcore/ActionButton.h"

namespace ftcore {

const char* actionButtonName(ActionButton b) {
    switch (b) {
    case ActionButton::Attack: return "ATTACK";
    case ActionButton::Guard: return "GUARD";
    case ActionButton::Dodge: return "DODGE";
    case ActionButton::Jump: return "JUMP";
    }
    return "?";
}

const char* actionButtonKey(ActionButton b) {
    switch (b) {
    case ActionButton::Attack: return "attack";
    case ActionButton::Guard: return "guard";
    case ActionButton::Dodge: return "dodge";
    case ActionButton::Jump: return "jump";
    }
    return "?";
}

bool parseActionButton(std::string_view key, ActionButton& out) {
    for (ActionButton b : kActionButtons) {
        if (key == actionButtonKey(b)) {
            out = b;
            return true;
        }
    }
    return false;
}

} // namespace ftcore
