#pragma once

// The arena camera's presets: how the fight is looked at, never a fight rule or number. "classic" is the zone's own
// rig (the fit's camera, untouched) and the lead's frame rule (kArenaLeadFrame) exactly, so a run on it is today's to
// the bit; "close" is a near over-the-shoulder lens. The player picks one in the settings (the Camera row,
// Settings::cameraPreset); the host's -ArenaCamera=close|classic switch wins over it.
//
// A preset is a rig (arm, pivot, shoulder, height, field of view, lock pitch; close only: classic's rig is the zone's)
// plus what the lens keeps on screen:
//   - cutAt: the height on the lead's body (m over the floor) that must stay in the frame (ArenaLeadBody::cut; 0 the
//     feet). Metres, not a share of the lead's height: the lens's pivot is at a fixed height too, so a fixed cut sits
//     at the same place in the frame for every lead (close's 0.6 m keeps every look-up for leads 1.4 to 1.8 m tall),
//     where a share that keeps 0.6 m on a 1.8 m lead (a third) sinks for a shorter one and holds its look-up back
//     (a 1.7 m lead's +14 to +5.5; 1.4 to 1.6 m lose every look-up), and one high enough for 1.4 m (0.43) lifts the
//     1.8 m lead's cut to 0.77 m. Over about 1.85 m the head leaves close's rule whatever the cut.
//     The host holds it to half the lead's height (cameraPresetCut),
//   - cutMin: the least share of the frame's height that point sits over the bottom edge (the rule's feetMin),
//   - headMax: the most share the head may reach (the rule's headMax),
//   - targetX: the share of the frame's width the lock's framing turn puts the target at (ArenaFrameScene::screenX;
//     0.5 the centre line),
//   - lagSeconds: the spring arm's location lag time (0: the boom's own lag, as today),
//   - lowMax: the most of the tall-lock low lens the host may blend in (kArenaLow; 1 as today, 0 never: a lens
//     already near chest height looks up from where it is).
//
// The zone fit is made and checked with the zone's own rig (checkArenaZone holds the rig to an arm of at least 3 m and
// a lock pitch of at most -5, which close does not keep); the host applies the preset to its copy of the fit's camera
// after that (applyCameraPreset). The ring, the starts, the sizing and the zone's camera-blocked share never read it.
// What the fight reads from the drawn camera every tick does change under close, by design (it is what the player
// sees), and no rule or number does:
//   - the view yaw (ArenaInput::view and cameraFacing: the stick's basis, the lock press's pick, a regular enemy's
//     windup arc), drawn at the lock's line plus the framing turn: close's turn (shaped by the range, ArenaCloseFrame.h)
//     differs from classic's on the right shoulder by 0 at 2.4 m (both at the 30 degree cap), 6.2 at 3 m, 3.5 at 4 m,
//     3.1 at 6 m, 4.0 at 8 m and 4.9 at 12 m, and on the left shoulder (the mirror) by 60, 53, 38, 26, 21 and 16; the
//     close partner turn adds up to 12 more; with nothing locked both are 0 and the view is the free camera's;
//   - the windup arc's camera half-angle (the real lens's: 30 degrees for close's 60 far, up to 34 for its 68 in
//     melee range, 35 for classic's 70; aiming, both 27.5);
//   - the partner's stationing camera (ArenaInput::cameraPos and partnerCamera: the close lens is 2.6 m back, not 6.4)
//     and the partner yaw's frame rule (cameraPresetLeadRules);
//   - the stick's lean on screen: up walks the lead toward the frame's centre at about 45 degrees under close (the
//     lead a third in from the side, 0.55 m off the lens), 18.6 under classic;
//   - the crosshair's ray (the aimed arm is the preset's times armScale, so the close lens is lower and nearer).
// The stick reads the drawn yaw under both: up the stick walks the lead along the lens's own facing, into the screen
// toward the frame's centre at the horizon, as under classic; read off the lock's line instead, the walk would head
// off the centre by the turn.
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
    p.rig.side = 0.55f;
    p.rig.up = 0.1f;
    p.rig.fov = 60.f;
    p.rig.lockPitch = -2.f;
    p.cutAt = 0.6f;
    p.cutMin = -0.04f;
    p.headMax = 0.75f;
    p.targetX = 0.6f;
    p.lagSeconds = 0.f; // the boom's own, as classic: a 0.08 s lag measured no different on the dodge strip
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
// lockPitch -40..10, cutAt 0..1.05 (half the tallest lead), cutMin -0.1..0.5, headMax 0.3..0.95, targetX 0.3..0.7,
// lagSeconds 0..0.5, lowMax 0..1), a cutMin not under headMax, or a rig key on classic (its rig is the zone's).
FTCORE_API bool importCameraPresets(std::string_view json, CameraPresets& out, std::string* error);

// The preset `id` names (an id past the last: classic).
FTCORE_API const CameraPreset& cameraPreset(const CameraPresets& presets, CameraPresetId id);

// The fit's camera seen through the preset `id`: classic (or an id past the last) gives the fit back untouched, the
// zone's own rig; close gives close's rig with the zone's own framing cap (frameTurnCapDeg) kept. Applied by the host
// after the fit is made and checked.
FTCORE_API ZoneCamera applyCameraPreset(const ZoneCamera& fit, const CameraPresets& presets, CameraPresetId id);

// The height on a lead `head` m tall the preset's lens keeps in the frame (ArenaLeadBody::cut): cutAt, held to half
// the lead's height. A head not over 0 (or NaN): 0, the feet.
FTCORE_API float cameraPresetCut(const CameraPreset& p, float head);

// The tall-lock low lens's most share under the preset: `fit` (arenaLowFit) held to lowMax. A NaN fit stays NaN, as
// the host's low lens has always read it; classic's lowMax of 1 is over every fit, so classic's fit is unchanged.
FTCORE_API float cameraPresetLowCap(const CameraPreset& p, float fit);

// The lead's frame rule under the preset: kArenaLeadFrame with feetMin = cutMin and headMax = headMax (the width kept).
FTCORE_API ArenaLeadFrameRules cameraPresetLeadRules(const CameraPreset& p);

} // namespace ftcore
