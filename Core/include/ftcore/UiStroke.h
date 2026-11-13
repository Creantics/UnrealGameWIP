#pragma once
// Hand-drawn strokes for the game UI: the points of a line or an arc with a steady hand's slow wobble, the width and
// the alpha thinning toward both ends. The Slate layer turns the points into one mesh (with a soft edge and a dark
// under-edge); Core only says where the line goes and how thick and strong it is at each point.
//
//   UiStroke s;
//   UiStrokeStyle st; st.width = 4.f; st.seed = 21;
//   uiStrokeArc(s, cx, cy, 70.f, -135.f, 60.f, st);   // degrees, 0 = up, clockwise (screen y down)
//
// A meter draws part of its track: from01..to01 of it. The wobble, the grain and a rubbed line's gaps are keyed to the
// place on the whole track (px from its start), and the points sit at fixed steps along it, so when the drawn part
// grows or shrinks the line already drawn does not crawl; only the visible ends taper. The same inputs always give the
// same points (seeded). A sweep, length or visible part of zero gives no points. Fixed size, zero allocation.
#include "ftcore/Api.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ftcore {

inline constexpr std::size_t kUiStrokeMaxPoints = 160;

struct UiStrokePoint {
    float x = 0.f, y = 0.f;
    float halfWidth = 0.f; // px
    float alpha = 0.f;     // 0..1, times the stroke's colour
};

struct UiStroke {
    std::array<UiStrokePoint, kUiStrokeMaxPoints> pt{};
    std::uint32_t count = 0;
};

struct UiStrokeStyle {
    float width = 3.f;       // px at full weight
    float wobble = 0.8f;     // px: how far the hand drifts across the line, at most
    float taper = 0.15f;     // the fraction of the length over which each end thins (0: no taper)
    float endWidth = 0.35f;  // the width at the very ends, as a fraction of full
    float endAlpha = 0.55f;  // the alpha at the very ends
    float step = 4.f;        // px between points along the track (grows when the drawn part needs more than the cap)
    bool broken = false;     // a rubbed-out trace: some short runs of the track drop to alpha 0
    std::uint32_t seed = 1;
};

// An arc track round (cx, cy) of radius r from a0 to a1 degrees (0 = up, clockwise on screen; |a1 - a0| up to 360),
// drawn from from01 to to01 of it.
FTCORE_API void uiStrokeArc(UiStroke& out, float cx, float cy, float r, float a0Deg, float a1Deg, const UiStrokeStyle& s,
    float from01 = 0.f, float to01 = 1.f);

// A straight track from (x0, y0) to (x1, y1), drawn from from01 to to01 of it.
FTCORE_API void uiStrokeLine(UiStroke& out, float x0, float y0, float x1, float y1, const UiStrokeStyle& s, float from01 = 0.f,
    float to01 = 1.f);

// The point on that arc at angle aDeg (no wobble): where a mark on the line goes.
FTCORE_API void uiArcPoint(float cx, float cy, float r, float aDeg, float& x, float& y);

} // namespace ftcore
