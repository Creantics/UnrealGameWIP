#pragma once

// Load: the weight a lead has taken on and not yet let go, 0 to kMaxLoad. Perfect Guards (and, later,
// perfect Intercepts) store it; it never refills or drains on its own. Release spends it as a
// counter that breaks poise; Take Half shares it with an ally. At kMaxLoad the lead is overloaded:
// in a Turns battle every command but Release is refused until the Load comes down. It is never a
// pool to pay for skills. A plain value type, used by every battle mode.

namespace ftcore {

inline constexpr int kMaxLoad = 100;
// A Release's combo: kReleaseBaseNotes notes, plus one for every kLoadPerReleaseNote Load held.
inline constexpr int kReleaseBaseNotes = 4;
inline constexpr int kLoadPerReleaseNote = 20;

struct Load {
    int value = 0;

    // Adds up to `amount` (never past kMaxLoad); returns what was stored.
    int store(int amount) {
        if (amount <= 0) return 0;
        const int room = kMaxLoad - value;
        const int stored = amount < room ? amount : room;
        value += stored;
        return stored;
    }
    // Empties it; returns what it held.
    int release() {
        const int held = value;
        value = 0;
        return held;
    }
    // Moves `fraction` (0 to 1) of it, rounded, to `other`, as much as other has room for; returns
    // what moved.
    int share(Load& other, float fraction) {
        if (!(fraction > 0.0f)) return 0;
        const float f = fraction < 1.0f ? fraction : 1.0f;
        const int want = static_cast<int>(static_cast<float>(value) * f + 0.5f);
        const int moved = other.store(want < value ? want : value);
        value -= moved;
        return moved;
    }
    bool overloaded() const { return value >= kMaxLoad; }
    // The note count of a Release made now.
    int releaseNotes() const { return kReleaseBaseNotes + value / kLoadPerReleaseNote; }
};

} // namespace ftcore
