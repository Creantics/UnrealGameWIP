#pragma once
// The play camera as the partner's brain reads it (PartnerBrain rules 13-16), in its own header so ArenaFight's input
// can carry it without PartnerBrain.h (which includes ArenaFight.h).

namespace ftcore {

struct PartnerVec3 {
    float x = 0.f, y = 0.f, z = 0.f; // the ground plane's x and y, z up
};

// The play camera (rules 13-16), in the ground plane's x and y with z up, the units of the rest. Not given (the
// default), or a field not finite, forward or up not about unit (within 0.01), up within 1 degree of forward, the
// field of view outside 1..170 degrees, the aspect outside 0.2..5 or bodyHeight outside 0.5..4: not given.
// An Unreal camera's FOV is its horizontal angle: verticalFovDegrees = 2 atan(tan(fov / 2) / aspect).
struct PartnerCameraView {
    bool given = false;
    PartnerVec3 pos{};
    PartnerVec3 forward{};          // unit
    PartnerVec3 up{};               // unit, not along forward (need not be square to it)
    float verticalFovDegrees = 0.f; // the whole vertical angle
    float aspect = 0.f;             // width over height
    float groundZ = 0.f;            // the floor under the partner (its feet)
    float bodyHeight = 1.8f;        // feet to head
};

} // namespace ftcore
