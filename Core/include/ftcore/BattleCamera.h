#pragma once

// The camera director of the Turns battle demo: which shot the camera is in, who it is on, and where
// the camera stands. Core decides; the Unreal host only places its camera component from pose().
//
//   - Choosing a command (Phase::AwaitingCommand): a close over-the-shoulder shot from behind and a
//     little to one side of the party member who is about to act, framing the enemy line beyond. The
//     side is the one the chosen target lies on, so the target shows past the actor's shoulder, and
//     the framing glides to the next target when the pick changes. When the choice passes to another
//     lead the camera glides to that lead. Aim (Phase::PartyAim) is the same shot, zoomed on the
//     enemy being aimed at.
//   - Actions being done (PartySkill, EnemySkill, and the Recovery that follows keeps the shot it
//     was in): a view from behind the party, low and over the party's heads, looking at the enemies,
//     so the player sees their own team's backs and the attacks coming toward them. During an enemy
//     attack it drifts sideways toward the defender (the interceptor while an Intercept is held),
//     never forward of the rearmost party member, and leans its look toward the attacker; during a
//     party skill it leans toward the enemy being hit.
//   - Victory and Defeat (and a battle not started): a wide shot from behind and above the party.
//
// Moves between shots are eased blends of 0.35 to 0.6 s from wherever the camera is when the shot
// changes, so nothing snaps; inside a shot the framing follows its goal with a short smoothing.
// The camera never comes within `minClear` of a figure (an ellipsoid around each, so it lifts over
// one rather than jumping) and never lower than `minHeight`. Every shot looks toward the enemy side.
//
// World units are Unreal's (centimetres, left-handed, Z up): forward +X and right +Y when looking
// along +X. `begin` takes the layout once (the two lines' positions at rest) and from it the stage's
// axis, "toward the enemies"; `update` takes the cue and the figures every frame. Figures are
// given at their rest positions, so a lunge or a bob does not shake the camera. No Unreal headers, no
// allocation: fixed-size state only.

#include "ftcore/Api.h"
#include "ftcore/Battle.h"

#include <cstddef>
#include <span>

namespace ftcore {

struct CamVec {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

// One figure on the stage. `pos` is where its feet stand.
struct CameraFigure {
    CamVec pos;
    double height = 190.0;
    double radius = 45.0; // half its width
    bool party = false;
    bool alive = true;
};

struct BattleCameraPose {
    CamVec eye;
    CamVec look; // the point the camera looks at
    double fov = 60.0; // degrees, horizontal
};

enum class CameraShot {
    Behind, // behind the party, low, over their heads, looking at the enemies
    Choose, // over the shoulder of the party member choosing a command
    Aim,    // over the shoulder, zoomed on the enemy being aimed at
    Wide,   // the outcome: a wide shot from behind and above the party
};

// What the battle is doing now. The host reads it off the Battle and its own menu.
struct CameraCue {
    Phase phase = Phase::NotStarted;
    int actor = -1;    // Battle::actor(): who is choosing or acting
    int defender = -1; // during EnemySkill: who the camera drifts toward (the interceptor while one holds the hit)
    int target = -1;   // AwaitingCommand / PartyAim: the enemy picked in the menu (-1: the skill hits them all);
                       // PartySkill: the enemy being hit (-1: all)
};

struct CameraTuning {
    // Choose: over the shoulder.
    double chooseBack = 300.0;    // behind the actor
    double chooseSide = 90.0;     // to the side the target lies on
    double chooseEye = 1.08;      // eye height, in actor heights
    double chooseLookAhead = 0.7; // the look point, from the actor toward the target
    double chooseFov = 54.0;
    double actorFrame = 0.6;      // the actor stays within this share of the half field of view from the look direction
    // Aim: the same, zoomed on the target.
    double aimLookAhead = 0.92;
    double aimFov = 40.0;
    // Behind: the party seen from behind.
    double behindBack = 440.0;   // behind the rearmost living party member
    double behindEye = 1.7;      // eye height, in the tallest party member's heights
    double behindFov = 56.0;
    double driftShare = 0.6;     // enemy turn: share of the way from the party's middle to the defender
    double driftMax = 220.0;     // ...and no further than this
    double leanShare = 0.5;      // the look, from the enemy line's middle toward the attacker (or the enemy being hit)
    // Wide: the outcome.
    double wideBack = 700.0;
    double wideSide = 260.0;
    double wideEye = 430.0;
    double wideFov = 62.0;
    // Blends (seconds): into each shot. All between 0.35 and 0.6.
    double blendChoose = 0.5;
    double blendAim = 0.35;
    double blendBehind = 0.45;
    double blendWide = 0.6;
    double follow = 0.16;       // seconds: how fast the framing inside a shot follows its goal
    double sideHysteresis = 40.0; // the target must lie this far across before the shoulder swaps
    // Clearance.
    double minClear = 70.0;  // the camera stays this far (horizontally, at ground level) from every figure's body
    double minAbove = 45.0;  // ...and this far above its head
    double minHeight = 120.0;
    double maxStep = 0.1;    // update() treats a longer frame as this long
};

// Where figure `slot` of a side stands on the demo stage: the party's two leads in a stagger, the
// enemies in a staggered row that tightens with more of them, the lines facing each other along +Y.
// From behind the party (looking along +Y, so screen-right is -X) the first of each side is on the left.
FTCORE_API CamVec stageHome(bool party, std::size_t slot, std::size_t enemyCount);

class BattleCamera {
public:
    FTCORE_API explicit BattleCamera(const CameraTuning& tuning = {});

    // Fixes the stage's axis from the two lines' rest positions and arms the first frame to take its
    // pose without a blend. Call at the start of every round; a later round blends from where the
    // camera is.
    FTCORE_API void begin(std::span<const CameraFigure> figures);
    // One frame: `dt` seconds (wall clock: paused time does not count). Figures are indexed by combatant id.
    FTCORE_API void update(double dt, const CameraCue& cue, std::span<const CameraFigure> figures);

    const BattleCameraPose& pose() const { return pose_; }
    CameraShot shot() const { return shot_; }
    // The party member the shot is on (Choose and Aim), else -1.
    int shotActor() const { return shot_ == CameraShot::Choose || shot_ == CameraShot::Aim ? actor_ : -1; }
    // 0..1: how far through the blend into the current shot. 1 when it has settled.
    double blend() const { return duration_ > 0.0 ? (elapsed_ >= duration_ ? 1.0 : elapsed_ / duration_) : 1.0; }
    const CameraTuning& tuning() const { return tuning_; }

    // The stage's axis: the unit direction (x, y) from the party toward the enemies, and the party's
    // rest centre. Valid after begin().
    CamVec forward() const { return forward_; }
    CamVec right() const { return {-forward_.y, forward_.x, 0.0}; }
    // Depth along the axis of a point, and its sideways offset, both from the party's centre.
    FTCORE_API double depthOf(const CamVec& p) const;
    FTCORE_API double lateralOf(const CamVec& p) const;

private:
    BattleCameraPose solve(const CameraCue& cue, std::span<const CameraFigure> figures) const;
    void clear(BattleCameraPose& pose, std::span<const CameraFigure> figures) const;
    void follow(double dt, const CameraCue& cue, std::span<const CameraFigure> figures, bool snap);
    CameraShot shotFor(const CameraCue& cue, std::span<const CameraFigure> figures) const;
    double blendTime(CameraShot shot) const;

    CameraTuning tuning_;
    CamVec forward_{0.0, 1.0, 0.0};
    CamVec partyCentre_;
    BattleCameraPose pose_;
    BattleCameraPose from_;
    CameraShot shot_ = CameraShot::Behind;
    int actor_ = -1;
    double elapsed_ = 0.0;
    double duration_ = 0.0;
    bool placed_ = false;   // a pose has been taken (no blend for the very first)
    bool snapFollow_ = true;
    bool restart_ = false;  // begin() after a pose: blend into the first frame's shot
    // The framing the shots follow (smoothed): the look point, how far across the camera stands from the
    // actor (-1..1 shoulders; sideSign_ is the shoulder it is heading for), and the sideways drift of the
    // behind shot.
    CamVec look_;
    double side_ = 1.0;
    double sideSign_ = 1.0;
    double drift_ = 0.0;
};

} // namespace ftcore
