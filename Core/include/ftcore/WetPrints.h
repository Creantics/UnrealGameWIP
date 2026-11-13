#pragma once

// The prints a wet body leaves on dry ground as it walks, fading as it dries.
//
//   ftcore::WetPrints prints;
//   const int laid = prints.step(loco.phase(), onFoot, wet.wetness, dt, dryScale);   // once a frame
//   if (laid >= 0) place a print at the planted foot; prints.strength(i) is its darkness now
//
// The rules:
//   - A print is laid at each plant of the gaits' shared foot phase: a right print where the phase passes 0.5, a left
//     one where it wraps past 1 back to 0. Only on foot (on the ground, moving on its gaits, out of the water); a step
//     off foot forgets the phase, so the first step back on foot lays nothing, and a phase that jumps more than
//     kWetPrintMaxAdvance in one step (a clip handing the body back) is taken up without a plant.
//   - A print's stamp is the body's wetness at the plant (0..1). A dry body lays none.
//   - strength = stamp - age / kWetPrintDryTime, never below 0, the age counted on the dry clock (the sum of
//     dryScale x dt, the scale the body dries by). kWetPrintDryTime (120 s) is longer than the body's own drying
//     (kWaterDryTime, 90 s) on purpose: a print fades slower than the body dries, so a newer print is never stronger
//     than an older one, and every print of one soaking is gone 120 s after the body left the water.
//   - The first prints, laid at kWetPrintDropsFrom and wetter, have drops round them, the more the wetter.
//   - kWetPrintCap prints at most, a ring: the next print replaces the oldest. A walk lays about two prints a second,
//     a run about three, so a body that runs the whole 90 s it takes to dry lays about 270 (see the cap).
//
// No heap: the ring is a fixed array.

#include "ftcore/Api.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ftcore {

inline constexpr std::size_t kWetPrintCap = 320;      // a run from the water until dry, with room over
inline constexpr float kWetPrintDryTime = 120.f;      // s on the dry clock for a print of stamp 1 to fade out
inline constexpr float kWetPrintDropsFrom = 0.9f;     // wetness from which a print has drops round it
inline constexpr float kWetPrintMaxAdvance = 0.25f;   // stride share per step over which the phase jumped, not walked

enum class WetFoot : std::uint8_t { Left, Right };

struct WetPrint {
    float stamp = 0.f;        // the body's wetness at the plant, 0..1
    float laidAt = 0.f;       // the dry clock at the plant
    float drops = 0.f;        // 0..1: the drops round it
    WetFoot foot = WetFoot::Left;
    std::uint32_t serial = 0; // 1, 2, ... in the order laid; 0: never laid
};

class WetPrints {
public:
    // One step of dt seconds. phase: the gaits' shared foot phase (0..1, 0 at a left plant); onFoot: on the ground,
    // moving on its gaits and out of the water; wetness: the body's, 0..1; dryScale: as WaterWetness::step.
    // Returns the ring index of the print laid in this step, or -1.
    FTCORE_API int step(float phase, bool onFoot, float wetness, float dt, float dryScale = 1.f);

    // A print's darkness now, 0..stamp; 0 for a slot never laid or faded out.
    FTCORE_API float strength(std::size_t i) const;
    // Seconds on the dry clock a print of this stamp lasts.
    static float life(float stamp) { return stamp * kWetPrintDryTime; }

    const WetPrint& print(std::size_t i) const { return prints_[i]; }
    // Prints with a strength over 0.
    FTCORE_API std::size_t live() const;
    std::uint32_t laid() const { return serial_; }
    float clock() const { return clock_; }

    FTCORE_API void clear();

private:
    std::array<WetPrint, kWetPrintCap> prints_{};
    std::size_t next_ = 0;
    std::uint32_t serial_ = 0;
    float clock_ = 0.f;
    float phase_ = -1.f; // the last step's phase on foot; below 0 after a step off foot
};

} // namespace ftcore
