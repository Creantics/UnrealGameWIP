#pragma once

// When the game asks for a garbage collection at a hidden moment (the pause menu opening, a fade to black, a load).
// A full collection is a visible hitch in play, so the game runs it where nobody sees it, and the engine's own
// periodic pass (gc.TimeBetweenPurgingPendingKillObjects, raised to a ceiling) rarely has to. A hidden moment asks
// for one only once minGap seconds have passed since the last collection of any kind, so opening and closing a menu
// in a row does not collect twice.
namespace ftcore {

struct GcPacing {
    double minGap = 30.0;

    // A hidden moment at now (s, any monotonic clock): true when a collection should run now.
    bool hiddenMoment(double now) {
        if (now - last_ < minGap) {
            return false;
        }
        last_ = now; // counted as collected; the engine's own report of it lands on the same second
        return true;
    }
    // A collection ran (the engine's periodic pass, a load's, or one asked for here).
    void collected(double now) { last_ = now; }
    double last() const { return last_; }

private:
    double last_ = 0.0;
};

} // namespace ftcore
