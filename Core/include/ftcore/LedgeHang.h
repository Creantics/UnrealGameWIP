#pragma once

// Hanging from a ledge caught in the air, and leaving it: climbing up or dropping off. The catch itself (which
// clip, from when, along which path) is TraversalSet::planCatch's; this decides what happens once the hands
// are on the edge, at the catch clip's contact.
//
//   ftcore::LedgeHang ledge;                // ledge.setTuning(t) from Data/Movement/movement.json
//   ftcore::LedgeState s;
//   if (ledge.canCatch(s)) plan and play a catch, as TraversalSet::planCatch says;
//   at the clip's contact: ledge.reach(s, input);  // Hanging (the clip holds there) or Climbing (it plays on)
//   every frame: ledge.step(s, input, dt);
//     Hanging: hold the clip at its contact; Climbing: play it on from there; Dropping: let go and fall,
//     pushed dropPush away from the face, and hand the body back (s is None again, the regrab running).
//
// At the contact a body still holding jump with the stick pushed into the face climbs straight through, as
// does every catch with hang 0. Any other hangs. Once it has hung minHang, a jump press or the stick pushed
// into the face climbs up; a crouch press or the stick pulled away from the face drops. The stick counts
// once it is pushed at least stickShare of the way, within intoFace degrees of the way into the face (away:
// within 180 - intoFace of it). After a drop no ledge is caught for regrab seconds, so the body falls clear
// of the one it let go of.
//
// Seconds, degrees and cm/s. Fixed size: nothing allocates.

#include "ftcore/Api.h"

#include <cstdint>

namespace ftcore {

enum class LedgePhase : std::uint8_t { None, Hanging, Climbing, Dropping };

struct LedgeTuning {
    float hang = 1.f;         // 1: a catch hangs unless it climbs straight through; 0: every catch climbs on
    float minHang = 0.2f;     // a hang takes no climb or drop before this long (s)
    float stickShare = 0.5f;  // the share of a full push the stick must reach to count
    float intoFace = 50.f;    // degrees either side of the way into the face that count as into it
    float dropPush = 80.f;    // cm/s the body is pushed away from the face as it lets go
    float regrab = 0.5f;      // no ledge is caught this long after a drop (s)
};

// What the player asks for, each frame.
struct LedgeInput {
    bool jumpHeld = false;     // jump held
    bool jumpPressed = false;  // jump pressed this frame (or a press the host keeps a moment)
    bool dropPressed = false;  // crouch pressed this frame (likewise)
    float stick = 0.f;         // how far the stick is pushed (0 to 1)
    float stickOff = 0.f;      // degrees from the way into the face to the way the stick points
};

struct LedgeState {
    LedgePhase phase = LedgePhase::None;
    float time = 0.f;     // how long it has been in its phase (s)
    float regrab = 0.f;   // left before a ledge may be caught again (s)
    bool through = false; // it climbed straight through at the catch, without hanging
    bool holding() const { return phase == LedgePhase::Hanging || phase == LedgePhase::Climbing; }
};

class LedgeHang {
public:
    void setTuning(const LedgeTuning& t) { tuning_ = t; }
    const LedgeTuning& tuning() const { return tuning_; }

    // Whether a ledge may be caught now: not on one already, and the regrab gone.
    FTCORE_API bool canCatch(const LedgeState& s) const;
    // The hands reach the edge (the catch clip's contact): Hanging, or Climbing straight through.
    FTCORE_API LedgePhase reach(LedgeState& s, const LedgeInput& in) const;
    // Moves it on by dt: a hang climbs or drops as asked (Dropping for the one step it is asked in, None from
    // the next, the regrab running from the drop); the regrab runs down. Call it every frame, holding or not.
    FTCORE_API void step(LedgeState& s, const LedgeInput& in, float dt) const;
    // The climb is done (the catch clip handed the body back): None.
    FTCORE_API void finish(LedgeState& s) const;
    // Whether the stick asks into the face (climb) or away from it (drop).
    FTCORE_API bool stickInto(const LedgeInput& in) const;
    FTCORE_API bool stickAway(const LedgeInput& in) const;

private:
    LedgeTuning tuning_;
};

} // namespace ftcore
