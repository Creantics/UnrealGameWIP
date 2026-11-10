#pragma once

// An arcade car on flat ground: a kinematic bicycle model (heading, forward and sideways speed, steer angle) stepped
// at a fixed substep so the same inputs always give the same path. It holds no mesh and no physics: the host sweeps
// the body box along the move it reads back, calls collide() for each wall or prop it hits, and mountKerb() when
// a kerb edge is low enough to drive up. Tunables come from Data/Vehicles/<car>.json (importVehicleTuning).
//
//   ftcore::VehicleDrive car;                    // car.setTuning(t) from the car's json
//   ftcore::VehicleState s = car.parked(x, y, heading);
//   every frame: car.step(s, input, dt); move the body to s.x, s.y, s.heading; turn the wheels by s.steer.
//
// The rules, all from the tuning:
// - Steering eases toward input.steer * maxSteer at steerRate (back toward straight at returnRate), and the lock
//   narrows with speed (halved at steerFadeSpeed), so keys and a stick feel alike.
// - Throttle accelerates forward, tapering to nothing at the top speed; brake slows a car going forward and, once it
//   is all but stopped, backs it up to reverseSpeed. Throttle while rolling backward brakes first. Released, the
//   car coasts down on rolling and air drag and settles at rest below restSpeed.
// - Top speed is the car's speedCap, lowered by input.limit when the host passes a road's limit (a shared lane's
//   10 km/h); above the limit the car is reined back at governorDecel, or at input.limitDecel when given (a
//   pedestrian ahead), and input.clamp is a hard ceiling. Input that is not finite reads as none, and a state
//   whose motion is not finite is reset rather than frozen.
// - The yaw rate follows the bicycle model (speed * tan(steer) / wheelbase), held under gripLat so a fast tight turn
//   understeers. Coasting (no throttle) adds rolling and air drag. Sideways speed is pulled back to zero by the tyres' grip each substep; the handbrake drops the
//   grip to handbrakeGrip, boosts the yaw by handbrakeYaw and slows the car at handbrakeDecel, so the tail slides.
// - A guide surface (a rail set in the road, an ink channel) is a line the vehicle can latch to. The host passes the
//   nearest one each frame (its direction and the vehicle's sideways offset from it). A vehicle whose tuning has a
//   guideSpeedCap latches when it is within latchDistance of the line and within latchAngle of its direction (either
//   way along it); latched, the steering is ignored, the heading turns onto the line at guideTurn, the vehicle is
//   drawn onto it at guidePull and it runs up to guideSpeedCap with no side slip. It lets go when the guide ends,
//   drifts beyond twice latchDistance, or the host asks to detach. Off the guide the ordinary rules and speedCap hold,
//   so a rail bike's or a skiff's data makes it quick on its guide and slow or twitchy off it. A vehicle with
//   guideSpeedCap 0 never latches.
//
// Positions in cm, speeds in cm/s, angles in radians (tuning angles in degrees), time in seconds. Nothing allocates.

#include "ftcore/Api.h"

#include <cmath>
#include <string>
#include <string_view>

namespace ftcore {

struct VehicleTuning {
    float wheelbase = 240.f;       // front to rear axle (cm)
    float maxSteer = 35.f;         // full lock at a standstill (degrees)
    float steerRate = 140.f;       // the wheels turn toward the asked angle this fast (degrees/s)...
    float returnRate = 240.f;      // ...and back toward straight this fast
    float steerFadeSpeed = 1600.f; // the lock is halved at this speed (cm/s)
    float engineAccel = 380.f;     // full throttle from rest (cm/s^2), tapering to 0 at the top speed
    float brakeDecel = 950.f;      // full brake (cm/s^2)
    float reverseAccel = 260.f;
    float reverseSpeed = 500.f;    // top speed backward (cm/s)
    float speedCap = 1667.f;       // top speed forward (cm/s; 60 km/h)
    float governorDecel = 500.f;   // above the road's limit the car slows at this (cm/s^2)
    float rollDrag = 40.f;         // coasting slows the car by this (cm/s^2)...
    float airDrag = 0.00012f;      // ...plus this times speed^2 (1/cm)
    float restSpeed = 15.f;        // coasting below this the car stops dead (cm/s)
    float gripLat = 850.f;         // the tyres' sideways hold (cm/s^2)
    float handbrakeGrip = 0.3f;    // the share of it left with the handbrake on (0 to 1)
    float handbrakeYaw = 1.6f;     // the yaw rate is boosted this much with the handbrake on
    float handbrakeDecel = 450.f;  // the handbrake slows the car by this (cm/s^2)
    float restitution = 0.15f;     // the share of speed into a wall that bounces back (0 to 1)
    float scrape = 0.6f;           // speed along a wall lost per unit of speed into it (0 to 2)
    float kerbMax = 16.f;          // the highest kerb edge the car drives up (cm)
    float kerbLoss = 4.f;          // speed lost per cm of kerb mounted (cm/s per cm)
    float substep = 1.f / 120.f;   // the fixed step (s)
    float guideSpeedCap = 0.f;     // top speed latched to a guide (cm/s); 0: this vehicle never latches
    float latchDistance = 60.f;    // latches within this of the guide's line (cm)...
    float latchAngle = 25.f;       // ...heading within this of its direction (degrees)
    float guideTurn = 90.f;        // latched, the heading turns onto the line this fast (degrees/s)
    float guidePull = 150.f;       // latched, the vehicle is drawn onto the line this fast (cm/s)
};

// The nearest guide surface, from the host each frame.
struct VehicleGuide {
    bool present = false;         // a guide within reach
    float dirX = 1.f, dirY = 0.f; // its direction here (normalised by the model)
    float offset = 0.f;           // the vehicle's distance from its line, + on the side 90 degrees toward +y of dir (cm)
};

struct VehicleInput {
    float throttle = 0.f; // 0 to 1
    float brake = 0.f;    // 0 to 1 (brakes, then reverses once stopped)
    float steer = 0.f;    // -1 (left) to 1 (right)
    bool handbrake = false;
    float limit = 0.f;    // the road's speed limit (cm/s); 0: none but the car's own
    float limitDecel = 0.f; // above `limit`, slow at this (cm/s^2); 0: the governor's own rate
    float clamp = 0.f;    // a hard ceiling on the speed, either way (cm/s); 0: none
    VehicleGuide guide;   // the nearest guide surface
    bool detach = false;  // let go of the guide (and don't latch while held)
};

struct VehicleState {
    float x = 0.f, y = 0.f; // the rear axle's centre (cm)
    float heading = 0.f;    // radians, 0 along +x, positive turning toward +y (Unreal's yaw)
    float forward = 0.f;    // speed along the heading (cm/s; negative backing up)
    float side = 0.f;       // sideways speed, + toward the side 90 degrees toward +y of the heading (Unreal's right) (cm/s)
    float steer = 0.f;      // the front wheels' angle (radians; + turns toward +y)
    float yawRate = 0.f;    // radians/s, from the last substep
    float carry = 0.f;      // time not yet stepped (s, under one substep)
    bool latched = false;   // running on a guide surface
    float speed() const { return std::sqrt(forward * forward + side * side); } // ground speed (cm/s)
    float velX() const { return forward * std::cos(heading) - side * std::sin(heading); }
    float velY() const { return forward * std::sin(heading) + side * std::cos(heading); }
};

class VehicleDrive {
public:
    void setTuning(const VehicleTuning& t) { tuning_ = t; }
    const VehicleTuning& tuning() const { return tuning_; }

    // A car at rest at (x, y) facing `heading`.
    FTCORE_API VehicleState parked(float x, float y, float heading) const;
    // Moves the car on by dt in whole substeps (the rest carried to the next call; at most 32 a call).
    FTCORE_API void step(VehicleState& s, const VehicleInput& in, float dt) const;
    // One substep of `h` seconds (in.guide.offset as it stands at this substep).
    FTCORE_API void substep(VehicleState& s, const VehicleInput& in, float h) const;
    // The car hit something whose surface faces (nx, ny) (normalised by the call). Returns the speed into it
    // (cm/s, 0 when moving away), which the host may turn into damage; the speed into it bounces back at
    // restitution and the speed along it loses scrape times that.
    FTCORE_API float collide(VehicleState& s, float nx, float ny) const;
    // A kerb edge `height` cm high ahead: true, and the car slowed by kerbLoss per cm, when it can drive up it;
    // false (the host treats it as a wall) when it is higher than kerbMax.
    FTCORE_API bool mountKerb(VehicleState& s, float height) const;
    // The top speed forward now: the guide's when latched, else the vehicle's own, under the road's limit.
    FTCORE_API float topSpeed(const VehicleState& s, const VehicleInput& in) const;

private:
    VehicleTuning tuning_;
};

// Reads {"version": 1, "drive": {...}} into `out`. Members are optional and keep their defaults; speeds may be
// given in km/h as speedCapKmh and reverseKmh. On failure `out` is unchanged and `error` says why. Load time only.
FTCORE_API bool importVehicleTuning(std::string_view json, VehicleTuning& out, std::string* error = nullptr);

} // namespace ftcore
