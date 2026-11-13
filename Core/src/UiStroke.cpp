#include "ftcore/UiStroke.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

constexpr float kStrokePi = 3.14159265358979f;

std::uint32_t strokeHash(std::uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

float strokeUnit(std::uint32_t seed, std::int32_t i) // [0, 1)
{
    return static_cast<float>(strokeHash(seed * 0x9e3779b9U + static_cast<std::uint32_t>(i)) >> 8) / 16777216.f;
}

// The hand's drift at u px along the track: two slow waves of seeded phase plus a smooth grain. At most 1 in size.
float strokeDrift(std::uint32_t seed, float u)
{
    const float a = strokeUnit(seed, 1001) * 6.f;
    const float b = strokeUnit(seed, 1002) * 6.f;
    const float g = u / 6.f; // grain: value noise on 6 px cells, interpolated so it never steps
    const auto cell = static_cast<std::int32_t>(std::floor(g));
    const float f = g - static_cast<float>(cell);
    const float grain = strokeUnit(seed, cell) * (1.f - f) + strokeUnit(seed, cell + 1) * f;
    const float v = std::sin(u * (2.f * kStrokePi / 90.f) + a) * 0.55f + std::sin(u * (2.f * kStrokePi / 27.f) + b) * 0.25f +
                    (grain - 0.5f) * 0.4f;
    return std::clamp(v, -1.f, 1.f);
}

// Visits the drawn part [uFrom, uTo] of a track: its two ends and every whole step between, so the samples stay put on
// the track as the drawn part changes. place(u, d, x, y) gives the point at u with drift d px across the track.
template <class Place>
void strokeTrack(UiStroke& out, const UiStrokeStyle& s, float uFrom, float uTo, Place place)
{
    out.count = 0;
    const float length = uTo - uFrom;
    if (!(length > 0.f)) {
        return;
    }
    float step = std::max(0.5f, s.step);
    const auto cap = static_cast<float>(kUiStrokeMaxPoints - 2);
    if (length / step > cap) {
        step = length / cap;
    }
    const auto first = static_cast<std::int32_t>(std::floor(uFrom / step)) + 1; // the first whole step past uFrom
    std::uint32_t n = 0;
    const auto put = [&](float u) {
        const float t = (u - uFrom) / length; // 0..1 along the drawn part, for the taper
        const float k = s.taper <= 0.f ? 1.f : std::clamp(std::min(t, 1.f - t) / s.taper, 0.f, 1.f);
        UiStrokePoint& p = out.pt[n++];
        place(u, strokeDrift(s.seed, u) * s.wobble, p.x, p.y);
        p.halfWidth = 0.5f * std::max(0.f, s.width) * (s.endWidth + (1.f - s.endWidth) * k);
        p.alpha = s.endAlpha + (1.f - s.endAlpha) * k;
        // runs of 20 px of the track: two in five of them gone
        if (s.broken && strokeUnit(s.seed ^ 0x5bd1e995U, static_cast<std::int32_t>(std::floor(u / 20.f))) < 0.4f) {
            p.alpha = 0.f;
        }
    };
    put(uFrom);
    for (std::int32_t i = first; n < kUiStrokeMaxPoints - 1; ++i) {
        const float u = static_cast<float>(i) * step;
        if (u >= uTo - 0.25f * step) {
            break;
        }
        put(u);
    }
    put(uTo);
    out.count = n;
}

void strokeVisible(float from01, float to01, float length, float& uFrom, float& uTo)
{
    uFrom = std::clamp(from01, 0.f, 1.f) * length;
    uTo = std::clamp(to01, 0.f, 1.f) * length;
}

} // namespace

void uiArcPoint(float cx, float cy, float r, float aDeg, float& x, float& y)
{
    const float a = aDeg * kStrokePi / 180.f;
    x = cx + r * std::sin(a);
    y = cy - r * std::cos(a);
}

void uiStrokeArc(UiStroke& out, float cx, float cy, float r, float a0Deg, float a1Deg, const UiStrokeStyle& s, float from01, float to01)
{
    out.count = 0;
    const float sweep = std::clamp(a1Deg - a0Deg, -360.f, 360.f);
    const float length = std::fabs(sweep) * kStrokePi / 180.f * r;
    if (!(length > 0.f) || !(r > 0.f)) {
        return;
    }
    float uFrom = 0.f;
    float uTo = 0.f;
    strokeVisible(from01, to01, length, uFrom, uTo);
    const float degPerPx = sweep / length;
    strokeTrack(out, s, uFrom, uTo, [&](float u, float d, float& x, float& y) { uiArcPoint(cx, cy, r + d, a0Deg + u * degPerPx, x, y); });
}

void uiStrokeLine(UiStroke& out, float x0, float y0, float x1, float y1, const UiStrokeStyle& s, float from01, float to01)
{
    out.count = 0;
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (!(length > 0.f)) {
        return;
    }
    float uFrom = 0.f;
    float uTo = 0.f;
    strokeVisible(from01, to01, length, uFrom, uTo);
    const float ux = dx / length;
    const float uy = dy / length;
    strokeTrack(out, s, uFrom, uTo, [&](float u, float d, float& x, float& y) {
        x = x0 + ux * u - uy * d; // d across the line
        y = y0 + uy * u + ux * d;
    });
}

} // namespace ftcore
