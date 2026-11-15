# Inertial blending

`Core/include/ftcore/Inertial.h` (header only, plain C++20, no allocation).

## What it is

When the body switches clips (idle, run, strike, hit, land) the pose either pops or, cross-faded, plays two clips at once and looks mushy. Inertial blending plays only the new clip. At the switch it takes, per joint, the offset from the new clip's pose to the last output and how fast that offset was changing. Over a short blend time it then decays the offset to zero and adds it on top of the new clip each frame.

- **Curve.** One quintic per offset. It starts at the offset with the old velocity and ends at rest: zero offset, zero velocity, zero acceleration.
- **Overshoot.** The blend time is cut to `-5 x0 / v0` when the start velocity would carry the offset past zero sooner. The cut is floored at 0.1 ms.
- **Rotations.** An axis and an angle: the rotation from the new pose to the old one, the short way (350 degrees is -10). Only the angle decays.
- **Translations.** A direction and a length. Only the length decays.
- **Limits, by design:**
  - a start velocity that would grow the offset (pointing away from the new pose) is taken as 0, so the offset never grows;
  - only the part of a translation's velocity along the offset is kept, and for a rotation only the turn about the offset's axis;
  - the start rate is the old output's own velocity: both old poses are measured against the one new pose passed to `begin`, so a new clip that is itself moving adds its velocity from the first frame on.
- **Units.** Seconds, centimetres, radians. A non-finite or negative dt counts as 0, and every divide is guarded.

## API

```cpp
namespace ftcore {
inline constexpr int kInertialMaxJoints = 256;
struct InertialJoint { TwistQuat rotation; TwistVec translation; };   // local, parent space
struct InertialCurve { void init(double x0, double v0, double t1); double value(double t) const;
                       double rawValue(double t) const; double velocity(double t) const;
                       double acceleration(double t) const; /* x0 v0 a0 t1 a b c */ };
class InertialBlend {
public:
    void begin(std::span<const InertialJoint> prevPose, std::span<const InertialJoint> prevPrevPose,
               double dtPrev, std::span<const InertialJoint> newPose, double blendSeconds);
    void apply(std::span<const InertialJoint> newPose, double dt, std::span<InertialJoint> outPose);
    bool active() const;
    double elapsed() const;
    double blendSeconds() const;
    const InertialCurve& translationCurve(std::size_t joint) const;
    const InertialCurve& rotationCurve(std::size_t joint) const;
    void reset();
};
}
```

- **`begin`**
  - Pass the last two outputs (`prevPose` the latest, `dtPrev` seconds after `prevPrevPose`) and the new clip's pose at the switch.
  - While a blend is running, the state's own last two outputs are used instead of the poses passed in. A chained switch therefore starts from the blended output, whatever the caller passes.
  - A blend time that is not finite or is <= 0 ends the blend.
- **`apply`**
  - Call it every frame, blending or not, so the state keeps its output history.
  - `dt` is the time since the last `apply` (or since the switch).
  - `apply(newPose, 0, out)` right after `begin` gives the start pose itself. Time only advances when `dt > 0`.
  - A non-finite joint in `newPose` holds its last output.
  - From the blend time on, the output is `newPose` exactly and `active()` turns false.
- **Memory.** The state is about 70 KB of fixed arrays. Keep it as a member, not on the stack.

## Calling it from Unreal (not yet wired)

`UFTLocomotionAnimInstance`'s proxy samples and blends its layers in `Evaluate(FPoseContext&)`. It would own one `ftcore::InertialBlend` and fixed arrays of `InertialJoint` for the new pose, the output and the last two outputs (the compact pose's local transforms, cm and radians, with quaternions copied component for component), all sized once when the bones are bound. This section was written from the header; check `Evaluate`'s body before wiring it.

1. Each `Evaluate` builds the new pose as it does today.
2. **On a clip switch:**
   - Compute the incoming clip's pose at the switch: the frame where the chosen base clip, an action clip or the landing changes, instead of starting a cross-fade.
   - Call `begin(lastOut, lastOutBefore, lastDelta, newPose, BlendSeconds)`. The proxy keeps the last two outputs as it writes them; while a blend is running the state uses its own copies anyway.
3. **Every frame:** call `apply(newPose, DeltaSeconds, out)` and write `out` back to the compact pose before `HoldFeet`, so the foot hold still works on the final pose.
4. **Settings:**
   - **Root bone.** Leave it out (or pass it unchanged), as the root is held at the ref pose.
   - **Blend times.** About 0.15 to 0.3 s for gait changes, about 0.1 s into strikes and hits.

## Tests (`Core/tests/test_inertial.cpp`)

- The curve starts at x0 and v0, ends with zero value, velocity and acceleration, and the raw polynomial stays in [0, x0] for start velocities toward and away from the target.
- No pop at the switch: the output equals the old pose within 1e-4. The output's velocity at the switch equals the old velocity along the offset, for translation and rotation.
- At and after the blend time the output is the new clip exactly, and it stays there while the new clip moves.
- The offset never grows past its start value for a sweep of start velocities, including ones pointing away and sideways.
- A switch in the middle of a blend (the caller passing the raw clip) has no pop. So does `begin` twice with no `apply` between.
- Rotations take the short way: a 350 degree offset decays from -10 degrees.
- Non-finite and zero dt, a bad `dtPrev`, bad blend times and bad joints are all safe.
- 30, 60 and 120 steps per second agree at the same times within 1e-6.
- `begin` and `apply` do not allocate (counted by the test allocation hook).
