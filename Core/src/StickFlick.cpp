#include "ftcore/StickFlick.h"

#include <cmath>

namespace ftcore {

namespace {
Arrow opposite(Arrow a) {
    switch (a) {
    case Arrow::Left: return Arrow::Right;
    case Arrow::Right: return Arrow::Left;
    case Arrow::Up: return Arrow::Down;
    case Arrow::Down: return Arrow::Up;
    }
    return a;
}
} // namespace

std::optional<Arrow> StickFlick::update(float x, float y) {
    const float ax = std::fabs(x);
    const float ay = std::fabs(y);
    const float mag = ax > ay ? ax : ay;
    if (mag < kRearm) {
        state_ = State::Armed;
        return std::nullopt;
    }
    if (mag < kFire) return std::nullopt;

    const bool horizontal = ax >= ay;
    const Arrow dir = horizontal ? (x < 0.f ? Arrow::Left : Arrow::Right) : (y > 0.f ? Arrow::Up : Arrow::Down);
    switch (state_) {
    case State::Unarmed: return std::nullopt;
    case State::Armed:
        state_ = State::Held;
        held_ = dir;
        return dir;
    case State::Held:
        if (dir == opposite(held_)) {
            held_ = dir;
            return dir;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace ftcore
