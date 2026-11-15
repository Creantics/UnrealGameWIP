#pragma once
// The HUD as the world changes (UI round 18b). By default it does nothing while the wave passes the camera: no banner,
// no name, no change on screen. With the exception switched on, the lead and partner rows and the prompts take the new
// world's ink over 0.3 s as the front crosses the camera, sweeping along them.
//
// The host reads the world at the camera each frame (the world wave's WhichWorldAt at the camera, mapped to a UiWorld
// index) and passes it in; this view edge-detects it.
//
//   UiWorldChange s; each frame: uiWorldChangeStep(s, in, dt);
//   per glyph run at x along the sweep (0 first): lerp(uiColor(style, s.from, ..), uiColor(style, s.shown, ..),
//   uiWorldChangeMix(s, x)); with nothing running, the ink is s.shown's.
//
// Header-only, plain values, zero allocation.
#include "ftcore/UiStyle.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ftcore {

inline constexpr float kUiWorldChangeBlend = 0.2f;  // one point's blend
inline constexpr float kUiWorldChangeSpread = 0.1f; // the start times' spread along the sweep
inline constexpr float kUiWorldChangeTime = kUiWorldChangeBlend + kUiWorldChangeSpread; // the whole envelope
inline constexpr std::int8_t kUiWorldNone = -1;

struct UiWorldChangeIn {
    bool on = false;                  // the exception's switch: off by default
    std::int8_t world = kUiWorldNone; // the world at the camera this frame (a UiWorld index), or none
    bool moving = false;              // a wave is running (not settled)
    bool drawn = false;               // a lead row, the partner row or a prompt is on screen
};

struct UiWorldChange {
    std::int8_t shown = kUiWorldNone; // the world whose ink the rows wear (during the envelope: the one coming)
    std::int8_t from = kUiWorldNone;  // during the envelope: the world going
    float t = 0.f;                    // envelope seconds
    bool running = false;
    bool flip = false;                // the sweep runs from its far end (the camera backed out)
};

inline bool uiWorldChangeValid(std::int8_t world) {
    return world >= 0 && static_cast<std::size_t>(world) < kUiWorldCount;
}

// One frame. Fires on the frame the camera's world differs from the rows' ink, only when switched on, a wave is moving
// and something is drawn; with nothing drawn the ink changes unseen, otherwise it waits.
inline void uiWorldChangeStep(UiWorldChange& s, const UiWorldChangeIn& in, float dt) {
    const float secs = std::max(dt, 0.f);
    if (s.running) {
        if (in.world == s.from) { // the camera backed out: run back from here, from the other end
            std::swap(s.from, s.shown);
            s.t = std::max(kUiWorldChangeTime - s.t, 0.f);
            s.flip = !s.flip;
        }
        s.t += secs;
        if (s.t >= kUiWorldChangeTime) {
            s.running = false;
            s.t = 0.f;
            s.from = kUiWorldNone;
            s.flip = false;
        }
        return; // a third world waits for the end
    }
    if (!uiWorldChangeValid(in.world) || in.world == s.shown) return;
    if (!uiWorldChangeValid(s.shown)) {
        s.shown = in.world; // the first world: taken silently
        return;
    }
    if (in.on && in.moving && in.drawn) {
        s.from = s.shown;
        s.shown = in.world;
        s.t = secs; // the crossing's frame is the envelope's first
        s.running = true;
        s.flip = false;
    } else if (!in.drawn) {
        s.shown = in.world;
    }
}

// The share of the coming world's ink at x along the sweep (0 the first point reached, 1 the last); 1 with nothing
// running. Each point blends over kUiWorldChangeBlend, smoothstep, starting x * kUiWorldChangeSpread in.
inline float uiWorldChangeMix(const UiWorldChange& s, float x) {
    if (!s.running) return 1.f;
    const float x01 = std::clamp(x, 0.f, 1.f);
    const float along = s.flip ? 1.f - x01 : x01;
    const float c = std::clamp((s.t - along * kUiWorldChangeSpread) / kUiWorldChangeBlend, 0.f, 1.f);
    return c * c * (3.f - 2.f * c);
}

} // namespace ftcore
