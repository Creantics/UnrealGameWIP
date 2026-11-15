# Inertial blending

`Core/include/ftcore/Inertial.h` (header only, plain C++20, no allocation).

## What it is

When the body switches clips (idle, run, strike, hit, land) the pose either pops or, cross-faded, plays two clips at once and looks mushy. Inertial blending plays only the new clip. At the switch it takes, per joint, the offset from the new clip's pose to the last output, and how fast that offset is changing: the old output's velocity minus the new clip's. Over a short blend time it then decays the offset to zero and adds it on top of the new clip each frame.

- **Curve.** One quintic per offset. It starts at the offset with that relative velocity and ends at rest: zero offset, zero velocity, zero acceleration. The output's velocity at the switch is therefore the old output's.
- **Overshoot.** The blend time is cut to `-5 x0 / v0` when the start velocity would carry the offset past zero sooner. The cut is floored at 0.1 ms. A new clip moving with the old output gives a rate near 0, so it does not cut the time.
- **Rotations.** An axis and an angle: the rotation from the new pose to the old one, `out = offset * new`, the short way (350 degrees is -10). Only the angle decays.
- **Translations.** A direction and a length. Only the length decays.
- **Limits, by design:**
  - a relative velocity that would grow the offset (pointing away from the new pose) is taken as 0, so the offset never grows;
  - only the part of the relative velocity along a translation's offset is kept, and for a rotation only the turn about the offset's axis.
- **Capacity.** `kInertialMaxJoints = 512`, meant to cover the leads' body skeletons. The repo records no bone count, so check it when wiring this up. Joints past it are copied from the new clip unblended; every joint of `outPose` up to the shorter span is written.
- **Units.** Seconds, centimetres, radians. A non-finite or negative dt counts as 0, and every divide is guarded.

## API

```cpp
namespace ftcore {
inline constexpr int kInertialMaxJoints = 512;
struct InertialJoint { TwistQuat rotation; TwistVec translation; };   // local, parent space
enum class InertialFrom : unsigned char { Given, Continue };
struct InertialCurve { void init(double x0, double v0, double t1); double value(double t) const;
                       double rawValue(double t) const; double velocity(double t) const;
                       double acceleration(double t) const; /* x0 v0 a0 t1 a b c */ };
class InertialBlend {
public:
    bool begin(std::span<const InertialJoint> prevPose, std::span<const InertialJoint> prevPrevPose, double dtPrev,
               std::span<const InertialJoint> newPose, std::span<const InertialJoint> newPrevPose, double dtNew,
               double blendSeconds, InertialFrom from = InertialFrom::Given);
    void apply(std::span<const InertialJoint> newPose, double dt, std::span<InertialJoint> outPose);
    bool active() const;
    double elapsed() const;
    double blendSeconds() const;   // as asked
    double endSeconds() const;     // when the last offset reaches zero
    const InertialCurve& translationCurve(std::size_t joint) const;
    const InertialCurve& rotationCurve(std::size_t joint) const;
    void reset();
};
}
```

- **`begin`**
  - `prevPose` and `prevPrevPose` are the last two outputs, `prevPose` the latest, taken `dtPrev` seconds apart.
  - `newPose` is the new clip at the switch. `newPrevPose` is the new clip sampled `dtNew` seconds earlier; its velocity is taken out of the offset's rate. An empty `newPrevPose`, a bad joint in it, or `dtNew <= 0` counts the new clip as still.
  - `from`:
    - `Given` (the default) uses `prevPose` and `prevPrevPose`.
    - `Continue` uses the blender's own last two outputs, so a switch in the middle of a blend starts from the blended output.
    - `Continue` falls back to the given poses in three cases: `apply` has not run since the last `begin`, the history holds fewer than two outputs, or the joint count differs from this `begin`'s.
    - The return value is true when it continued.
  - A blend time that is not finite or is <= 0 ends the blend.
- **`apply`**
  - Call it every frame, blending or not, so the state keeps its output history.
  - `dt` is the time since the last `apply` (or since the switch).
  - `apply(newPose, 0, out)` right after `begin` gives the start pose itself. Time only advances when `dt > 0`.
  - A non-finite joint in `newPose` holds its last output.
- **`active()`**
  - It is true while any joint's offset is not yet zero, so until `endSeconds()`: the longest per-joint curve after shortening and the 0.1 ms floor.
  - A curve cut short ends the blend early. A blend asked for under 0.1 ms lasts the 0.1 ms.
  - From then on the output is `newPose` exactly.
  - A switch with no offset anywhere is not active at all.
- **Memory.** The state is about 140 KB of fixed arrays. Keep it as a member, not on the stack.

## Calling it from Unreal (not yet wired)

`UFTLocomotionAnimInstance`'s proxy samples and blends its layers in `Evaluate(FPoseContext&)`. It would own one `ftcore::InertialBlend` and fixed arrays of `InertialJoint`, all sized once when the bones are bound:

- the new pose and the new clip's pose one step earlier;
- the output and the last two outputs.

Each holds the compact pose's local transforms in cm and radians, with quaternions copied component for component. This section was written from the header; check `Evaluate`'s body before wiring it.

1. Each `Evaluate` builds the new pose as it does today.
2. **On a clip switch:** the frame where the chosen base clip, an action clip or the landing changes, instead of starting a cross-fade.
   - Sample the incoming clip at its start time (`newPose`) and one frame earlier (`newPrevPose`, `dtNew` = that step). For a looping clip, wrap that time to the clip's end.
   - Call `begin(lastOut, lastOutBefore, lastDelta, newPose, newPrevPose, dtNew, BlendSeconds, InertialFrom::Continue)`.
3. **Every frame:** call `apply(newPose, DeltaSeconds, out)` and write `out` back to the compact pose before `HoldFeet`, so the foot hold still works on the final pose.
4. **Settings:**
   - **Root bone.** Leave it out (or pass it unchanged), as the root is held at the ref pose.
   - **Blend times.** About 0.15 to 0.3 s for gait changes, about 0.1 s into strikes and hits.

## Tests (`Core/tests/test_inertial.cpp`)

- **The curve.** It starts at x0 and v0 and ends with zero value, velocity and acceleration. The raw polynomial stays in [0, x0] for start velocities toward and away from the target.
- **No pop at the switch.** The output equals the old pose within 1e-4. Old and new rotations are about different axes, and the test asserts that the wrong compose order would fail.
- **A moving new clip.** The new clip runs at 200 cm/s and 2 rad/s against the old output:
  - the offset's rate is the difference;
  - the output's velocity at the switch is the old output's;
  - T is not cut.
- **Both clips moving together at 600 cm/s.** The rate is 0 and T keeps the asked time; treating the new clip as still would cut it to 0.25 s.
- **A still new clip.** The output's velocity at the switch matches the old output's.
- **The end.** Once the offsets end, the output is the new clip exactly and stays there while the new clip moves.
- **`active()`.** It ends with shortened curves (before the blend time), it lasts the floored curve for a blend under 0.1 ms, and it is false for a switch with no offset.
- **No overshoot.** The offset never grows past its start value for a sweep of start velocities, including ones pointing away and sideways.
- **Chained switches.**
  - `Continue` mid-blend has no pop, whatever poses the caller passes.
  - `Given` uses the poses passed in.
  - `Continue` refuses with no `apply` since the last `begin`, and after a joint count change.
- **History.** A joint count that grows on a dt = 0 frame leaves no stale history: a chained `begin` after it gets no false velocity.
- **Capacity and spans.** With more joints than the capacity, every joint is written (those past it unblended). Spans of different lengths are handled: a shorter output, a longer output (its tail untouched) and a shorter `prevPose`.
- **Rotations take the short way.** A 350 degree offset decays from -10 degrees.
- **Bad input.** Non-finite and zero dt, bad `dtPrev` and `dtNew`, bad blend times and bad joints are all safe.
- **Frame rate.** 30, 60 and 120 steps per second agree at the same times within 1e-6, with a moving new clip.
- **Allocation.** `begin` and `apply` do not allocate. The test requires the allocation hook to be installed.
