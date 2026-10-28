#pragma once

// Going over and up onto things: which traversal clip (a hurdle over a low wall, a mantle or climb up onto
// a ledge) a body plays for an obstacle ahead, from where in the clip, and the path its root follows so the
// clip's obstacle lines up with the real one.
//
//   ftcore::TraversalSet set;
//   set.setClip(i, ftcore::measureTraversal(frames, count, length));  // once per clip, at load
//   const ftcore::TraversalPlan p = set.plan(obstacle, speed, foot);    // while an obstacle is ahead
//   if (p.ok()) play clip p.clip from p.start; every frame: clipTime += dt * set.rate(p, clipTime);
//                                                          root = set.rootAt(p, clipTime);
//
// The clips' root bone traces the obstacle they were made for: it stays on the floor, rises onto the top
// (reaching it at the front edge), and either comes back down beyond it (over) or stays up (up).
// measureTraversal reads from the root: when it starts to rise (the contact: hands and feet go onto the
// obstacle from here), where it reaches the top (the front edge: how far ahead and how high), how long it
// stays up before coming down (the top's depth, going over), when it settles at its end height (the
// landing), plus the speed it comes in at and the foot it takes off from. A clip whose root ends far below
// its start (a drop off an edge) is not measured.
//
// plan() keeps the clips the obstacle allows (a top thinner than standDepth with a floor beyond is gone
// over, a deeper one is climbed onto) and made for a height near the obstacle's, then takes the cheapest
// by height, entry speed and take-off foot. It enters the clip at the point whose distance to the front
// edge matches the real one; a body still farther off than the clip reaches gets tooFar (keep moving and
// ask again). rootAt() is the clip's root path from that point plus an offset that eases (smoothstep) from
// nothing at the entry to the one meeting the real edge and height at the contact, holds the height over
// the top while making up any extra depth, then eases to the one meeting the real floor at the landing.
// rate() plays the clip at the body's own speed as it enters (a sprint into a clip made at a run would
// otherwise drop to the clip's speed in a frame), easing to the clip's own rate by the contact.
//
// A roll is a traversal with no obstacle: its root stays on the floor while the pelvis goes down and
// back up. measureRoll reads it from the pelvis (when the body is tucked, the contact; when it is back
// up, the landing; how far it goes down) and from the root as above (the entry speed, the take-off foot,
// the path). A land roll's clip opens with the fall onto that floor: it is read from the touchdown on.
// planRoll takes the roll clip whose entry speed and take-off foot are nearest the body's, from its
// touchdown (a roll on the floor throughout: its start), its root path as it is; rate() eases to the
// clip's own rate by the tuck. plan() never takes a roll.
//
// Space: x forward (toward the obstacle at the start), y to the left, z up; cm and seconds. Fixed
// size: nothing allocates.

#include "ftcore/Api.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ftcore {

struct TravPoint {
    float x = 0.f, y = 0.f, z = 0.f;
};

// One sample of a clip: the root bone and both feet (ball joints), in the clip's space, and the pelvis
// (read for rolls only).
struct TraversalFrame {
    TravPoint root, ballLeft, ballRight;
    TravPoint pelvis;
};
inline constexpr std::size_t kMaxTraversalFrames = 512;
inline constexpr std::size_t kTraversalSamples = 64;

enum class TraversalMove : std::uint8_t { Over, Up, Roll };

struct TraversalClipInfo {
    float length = 0.f;      // seconds; 0: no such clip (or none could be measured in it)
    TraversalMove move = TraversalMove::Over;
    float contact = 0.f;     // when the root starts to rise (a roll: when the body is tucked)
    float leave = 0.f;       // when it starts down off the top (over); the landing (up, roll)
    float land = 0.f;        // when it settles at its end height (a roll: when the pelvis is back up)
    float ledgeAhead = 0.f;  // the front edge's distance ahead of the root at time 0
    float ledgeHeight = 0.f; // the top's height above the root at time 0
    float topDepth = 0.f;    // how far the root goes along the top before coming down (over)
    float rise = 0.f;        // the root's end height above its start
    float dip = 0.f;         // a roll: how far the pelvis goes below the lower of its start and end heights
    float touchdown = 0.f;   // a roll: when the root is down on the floor it rolls along (0: on it from the start)
    float entrySpeed = 0.f;  // the root's forward speed over the first kEntryTime (a roll's from the touchdown)
    int takeoffFoot = -1;    // 0 left, 1 right: the last foot at rest before the contact (-1: none)
    std::array<TravPoint, kTraversalSamples> path{}; // the root from where it is at the touchdown, evenly to the end
};

inline constexpr float kMinObstacle = 20.f; // a root rising less than this has no obstacle to measure
inline constexpr float kMaxClipDrop = 40.f; // a root ending lower than this below its start is a drop: not measured
inline constexpr float kRiseShare = 0.05f;  // the root has started up past this share of the top's height...
inline constexpr float kTopShare = 0.95f;   // ...and is on the top past this share
inline constexpr float kSettle = 4.f;       // the root within this of its end height has landed
inline constexpr float kRestSpeed = 40.f;   // cm/s: a foot whose net move over kRestTime is slower than this...
inline constexpr float kRestShare = 0.25f;  // ...or than this share of the root's ground speed then is at rest
inline constexpr float kRestTime = 0.08f;
inline constexpr float kFootRest = 4.f;     // a foot at rest within this of the starting floor is on it
inline constexpr float kEntryTime = 0.2f;
inline constexpr float kMinRollDip = 30.f;  // a pelvis going down less than this is no roll

// Measures a clip from `count` frames sampled evenly over it (frame 0 at time 0, the last at
// `length`; at most kMaxTraversalFrames are read). length stays 0 when the root rises less than
// kMinObstacle, ends more than kMaxClipDrop below its start, or never settles.
FTCORE_API TraversalClipInfo measureTraversal(const TraversalFrame* frames, std::size_t count, float length);
// Measures a roll from frames sampled the same way. It is read from the touchdown: the first frame from
// which the root stays within kMinObstacle of the height it ends at, and is within kSettle of it (frame 0
// for a root that never leaves it; a land roll opens with its fall). The body is tucked once the pelvis
// is within kRiseShare of its way down from its height there to its lowest, and back up once it has come
// kTopShare of the way up to its end height. length stays 0 when the pelvis goes down less than
// kMinRollDip below its height at the touchdown or its end height (a pelvis never sampled stays at 0;
// a root that follows the pelvis down touches down only once it is back up, past the dip).
FTCORE_API TraversalClipInfo measureRoll(const TraversalFrame* frames, std::size_t count, float length);

// What the body's probes found ahead.
struct TraversalObstacle {
    float distance = 0.f;   // from the root to the near edge, along the way the body faces
    float height = 0.f;     // the top above the floor the body stands on
    float depth = 0.f;      // the top's length front to back (as far as it was probed)
    bool backFloor = false; // a floor found beyond the top
    float backHeight = 0.f; // its height above the floor the body stands on (negative: lower)
};

struct TraversalTuning {
    float standDepth = 60.f;     // a top at least this deep is climbed onto; a thinner one is gone over
    float heightSlack = 0.3f;    // a clip serves heights within this share of its own...
    float minHeightSlack = 30.f; // ...or within this, whichever is more
    float maxDrop = 150.f;       // going over needs the far floor no lower than this below
    float minLead = 0.15f;       // a clip is entered at least this long before its contact
    float maxPush = 40.f;        // the offset may carry the root this much farther than the clip to the edge
    float maxPull = 40.f;        // or this much less
    float heightCost = 1.f / 100.f; // the cost per cm of height between the obstacle and the clip
    float speedCost = 1.f / 200.f;  // per cm/s of entry speed between the body and the clip
    float footCost = 0.3f;       // a clip taking off from the other foot
    float exitAfter = 0.3f;      // a body that wants to move leaves the clip this long after the landing
    float minEntryRate = 0.8f;   // the play rate at the entry keeps the body's speed within these
    float maxEntryRate = 1.5f;
};

struct TraversalPlan {
    int clip = -1;
    float start = 0.f;     // the clip time it is entered at
    float entryRate = 1.f; // the play rate there: the body's speed over the clip's entry speed, within limits
    TravPoint atContact;   // the offset reached at the contact
    TravPoint atLeave;     // at the leave
    TravPoint atLand;      // and at the landing
    bool tooFar = false;   // no plan yet: a clip fits, but the edge is still out of its reach
    bool ok() const { return clip >= 0; }
};

inline constexpr std::size_t kMaxTraversalClips = 32;

class TraversalSet {
public:
    void setTuning(const TraversalTuning& t) { tuning_ = t; }
    const TraversalTuning& tuning() const { return tuning_; }
    // False when i is out of range.
    FTCORE_API bool setClip(std::size_t i, const TraversalClipInfo& info);
    const TraversalClipInfo& clip(std::size_t i) const { return clips_[i]; }

    // speed: the body's ground speed now; foot: its planted (or last planted) foot, 0 left, 1 right,
    // -1 not known. Clips are taken in index order when they cost the same. Rolls are left out.
    FTCORE_API TraversalPlan plan(const TraversalObstacle& obstacle, float speed, int foot) const;
    // A roll for a body at ground speed `speed` with `foot` planted (as plan()): the roll clip that costs
    // least by entry speed and take-off foot, entered at its touchdown with its root path as it is (no
    // offsets). Not ok without a roll clip.
    FTCORE_API TraversalPlan planRoll(float speed, int foot) const;
    // The root at clip time t (from p.start on), from where it was when the plan was made, in the
    // body's space then.
    FTCORE_API TravPoint rootAt(const TraversalPlan& p, float t) const;
    // When a body that wants to move hands back to its locomotion: exitAfter past the landing (a roll's:
    // once the body is back up), never before the entry nor past the clip's end (the clip's own tail, a
    // run on or a stand-up, is for a body left standing). 0 without a plan.
    FTCORE_API float exitTime(const TraversalPlan& p) const;
    // The play rate at clip time t: the plan's entryRate at the entry, easing (smoothstep) to 1 by the contact,
    // so the climb or vault itself plays at its own pace. A caller advances clip time by dt x rate. 1 without a
    // plan.
    FTCORE_API float rate(const TraversalPlan& p, float t) const;

private:
    TraversalTuning tuning_;
    std::array<TraversalClipInfo, kMaxTraversalClips> clips_{};
};

// The clip's root at time t, from its path (before a roll's touchdown: where it is at the touchdown).
FTCORE_API TravPoint traversalPath(const TraversalClipInfo& info, float t);

} // namespace ftcore
