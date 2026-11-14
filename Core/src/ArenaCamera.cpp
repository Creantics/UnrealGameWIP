#include "ftcore/ArenaCamera.h"

#include <cmath>

namespace ftcore {

namespace {
constexpr float kRad = 3.14159265358979f / 180.f;
} // namespace

ArenaCameraPose arenaCameraPose(const ZoneCamera& zone, const AimStance& aim, const ArenaFrameYaw& yaw, float viewPitch) {
    const AimRules& rules = aim.rules();
    const ArenaPitchSplit split = arenaSplitPitch(kArenaLook, arenaClampViewPitch(kArenaLook, viewPitch));
    ArenaCameraPose p;
    p.yaw = arenaFrameViewYaw(yaw);
    p.boomPitch = split.boom;
    p.tilt = split.tilt;
    p.arm = aim.blend(zone.arm, zone.arm * rules.armScale);
    p.pivot = zone.pivot;
    p.side = aim.blend(zone.side, rules.shoulderRight / 100.f);
    p.up = zone.up;
    p.fov = aim.blend(zone.fov, rules.fov);
    return p;
}

ArenaLensAt arenaCameraLens(const ArenaCameraPose& pose) {
    const float b = pose.boomPitch * kRad;
    const float y = pose.yaw * kRad;
    const float cb = std::cos(b);
    const float sb = std::sin(b);
    const float cy = std::cos(y);
    const float sy = std::sin(y);
    // Back along the boom, then right and up in the boom's frame.
    ArenaLensAt at;
    at.x = -cb * cy * pose.arm - sy * pose.side - sb * cy * pose.up;
    at.y = -cb * sy * pose.arm + cy * pose.side - sb * sy * pose.up;
    at.z = -sb * pose.arm + cb * pose.up;
    return at;
}

} // namespace ftcore
