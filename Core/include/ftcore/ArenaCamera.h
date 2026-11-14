#pragma once

// What the arena host applies to its camera each frame, all of it from Core: the yaw the view is drawn at (the lock's
// line plus the framing turn), the boom's pitch and the lens's tilt (arenaSplitPitch), and the rig (arm, pivot,
// shoulder, height, field of view) blended from the zone's by the aim stance. The framing turn feeds only the yaw: the
// rig comes from the zone and the aim, never from the turn, so the lens stays the same distance from the pivot at any
// turn and the framing never pulls the camera out.
//
// arenaCameraLens: where a spring arm puts the lens for a pose, from the pivot over the lead's feet: the arm back along
// the boom, then the shoulder (+ right) and height offsets turned with it (UE yaw: + turns right, +y is the right at 0).

#include "ftcore/AimStance.h"
#include "ftcore/Api.h"
#include "ftcore/ArenaLook.h"
#include "ftcore/ArenaZone.h"

namespace ftcore {

struct ArenaCameraPose {
    float yaw = 0.f;       // degrees, the view as drawn
    float boomPitch = 0.f; // degrees, the arm's
    float tilt = 0.f;      // degrees, the lens's on top of the boom
    float arm = 0.f;       // m
    float pivot = 0.f;     // m over the feet
    float side = 0.f;      // m, + right
    float up = 0.f;        // m
    float fov = 0.f;       // degrees, horizontal
};

FTCORE_API ArenaCameraPose arenaCameraPose(const ZoneCamera& zone, const AimStance& aim, const ArenaFrameYaw& yaw, float viewPitch);

struct ArenaLensAt {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f; // m from the pivot
};

FTCORE_API ArenaLensAt arenaCameraLens(const ArenaCameraPose& pose);

} // namespace ftcore
