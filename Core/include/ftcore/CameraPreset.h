#pragma once

// The arena camera's presets: how the fight is looked at, never how it is fought. "classic" is the zone's own rig
// (ZoneCamera{}) and the lead's frame rule (kArenaLeadFrame) exactly, so a run on it is today's to the bit; "close" is
// a near over-the-shoulder lens. The player picks one in the settings (the Camera row, Settings::cameraPreset); the
// host's -ArenaCamera=close|classic switch wins over it.
//
// A preset is a rig (arm, pivot, shoulder, height, field of view, lock pitch) plus what the lens keeps on screen:
//   - cutAt: the height on the lead's body (m) that must stay in the frame (ArenaLeadBody::cut; 0 the feet),
//   - cutMin: the least share of the frame's height that point sits over the bottom edge (the rule's feetMin),
//   - headMax: the most share the head may reach (the rule's headMax),
//   - targetX: the share of the frame's width the lock's framing turn puts the target at (ArenaFrameScene::screenX;
//     0.5 the centre line),
//   - lagSeconds: the spring arm's location lag time (0: the boom's own lag, as today),
//   - lowMax: the most of the tall-lock low lens the host may blend in (kArenaLow; 1 as today, 0 never: a lens
//     already near chest height looks up from where it is).
//
// The preset is view only. The zone fit is made and checked with the zone's own rig (checkArenaZone holds the rig to
// an arm of at least 3 m and a lock pitch of at most -5, which close does not keep); the host applies the preset to
// its copy of the fit's camera after that (applyCameraPreset), and nothing on the fight's side (the ring, the starts,
// the sizing, the zone's camera-blocked share) ever reads it.
//
// The numbers are in Data/Battle/camera_presets.json (importCameraPresets). Nothing allocates after import.

#include "ftcore/Api.h"
#include "ftcore/ArenaLockFrame.h"
#include "ftcore/ArenaZone.h"
#include "ftcore/CameraPresetId.h"

#include <string>
#include <string_view>

namespace ftcore {

struct CameraPreset {
    ZoneCamera rig{};
    float cutAt = 0.f;                       // m on the lead's body kept on screen (0: the feet)
    float cutMin = kArenaLeadFrame.feetMin;  // share of the frame's height: that point at least this far over the bottom
    float headMax = kArenaLeadFrame.headMax; // share of the frame's height: the head no higher than this
    float targetX = 0.5f;                    // share of the frame's width the locked target is turned to
    float lagSeconds = 0.f;                  // s, the boom's location lag (0: its own)
    float lowMax = 1.f;                      // 0..1: the most low-lens share a tall lock may blend in
};

// The close lens: a short arm low over the shoulder, the lens nearly level, the lead cut at the thighs. The cut may
// sink 4% under the bottom edge: a tall lock's look-up swings the boom, and at +14 degrees the 0.6 m point sits at
// -0.027 (the head at 0.69), so the look-up is never held back by the cut. No low lens: from the 0.6 m low eye the
// head leaves the top of the frame (1.24 at pitch 0).
inline constexpr CameraPreset closeCameraPreset() {
    CameraPreset p;
    p.rig.arm = 2.6f;
    p.rig.pivot = 1.35f;
    p.rig.side = 0.2f;
    p.rig.up = 0.1f;
    p.rig.fov = 60.f;
    p.rig.lockPitch = -2.f;
    p.cutAt = 0.6f;
    p.cutMin = -0.04f;
    p.headMax = 0.75f;
    p.targetX = 0.6f;
    p.lagSeconds = 0.08f;
    p.lowMax = 0.f;
    return p;
}

struct CameraPresets {
    CameraPreset classic{};
    CameraPreset close = closeCameraPreset();
};

// Reads Data/Battle/camera_presets.json over `out` (a preset or a key absent keeps out's values):
//   {"version": 1, "classic": {"arm": 6.4, ...}, "close": {"arm": 2.6, ...}}
// False with a reason in `error`, and `out` unchanged, on a bad file: not JSON, not an object, an unknown preset or
// key, a value not a number or out of its range (arm 1..40, pivot 0.5..10, side -3..3, up -1..3, fov 30..120,
// lockPitch -40..10, cutAt 0..1.8, cutMin -0.1..0.5, headMax 0.3..0.95, targetX 0.3..0.7, lagSeconds 0..0.5, lowMax 0..1).
FTCORE_API bool importCameraPresets(std::string_view json, CameraPresets& out, std::string* error);

// The preset `id` names (an id past the last: classic).
FTCORE_API const CameraPreset& cameraPreset(const CameraPresets& presets, CameraPresetId id);

// The fit's camera seen through the preset: the preset's rig with the zone's own framing cap (frameTurnCapDeg) kept.
// Classic on a default fit gives the fit back field for field. Applied by the host after the fit is made and checked.
FTCORE_API ZoneCamera applyCameraPreset(const ZoneCamera& fit, const CameraPreset& p);

// The lead's frame rule under the preset: kArenaLeadFrame with feetMin = cutMin and headMax = headMax (the width kept).
FTCORE_API ArenaLeadFrameRules cameraPresetLeadRules(const CameraPreset& p);

} // namespace ftcore
