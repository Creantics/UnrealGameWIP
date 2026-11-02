#pragma once

// The cinematic beat boss's accessibility options (docs/BEAT_BOSS_DEMO.md, "Accessibility"). They are for the beat
// screen only; the story's options (Settings.h: auto-complete, hold toggle, reduce flashes) stay where they are
// and the host merges the two. Every default is today's behaviour.
//
//   ftcore::BeatAccessOptions access;                       // defaults
//   ftcore::decodeBeatAccess(text, access);                 // missing keys keep their defaults, values are clamped
//   BeatBossTuning tuning = ftcore::beatAccessTuning({}, access);
//   fight.requestAccess(tuning, assist);                    // lands at the next bar line, or on a retry
//
// Options (ids as in the file; the range after the default):
//   beat.window_scale      1.0   1..2    Scales the Good windows (a strike's, a counter's early edge and late limit,
//                                        a hold's slack, a pair's width) fully, and the Perfect widths at half the
//                                        rate (capped at 1.5): Perfect stays worth earning. The result screen carries
//                                        no "assisted" mark. A track is playable at a scale only while no two windows
//                                        of one button overlap (maxBeatWindowScale).
//   beat.whiff_lockout_ms  400   0..400  The lockout of a counter pressed before its window starts. It shortens as the
//                                        windows widen: the lockout is this divided by beat.window_scale.
//   beat.pair_mode         0     0..2    0 two buttons together; 1 the second may follow the first by half a second
//                                        (nothing simultaneous); 2 one press of either button counts as both.
//   beat.ui_scale          1.0   1..2    The prompts, labels and strip of the HUD, all of them.
//   beat.cuts_reduced      0     0..2    0 normal; 1 a shot holds 2.5 s at least and no cut falls on an impact beat;
//                                        2 one shot for the whole phase.
//   beat.shake_scale       1.0   0..1    The camera shake and jolt.
//   beat.flash_level       0     0..2    0 normal; 1 no edge frame and the bursts fade slowly; 2 no screen-wide effect
//                                        and no ring bursts. Settings::reduceFlashes raises it to at least 1.
//   beat.tempo_scale       1.0   0.5..1  Practice tempo: the track is played at this share of its tempo
//                                        (rescaleBeatBossDef), never under 40 bpm in any phase.
//
// Changing the assists or the windows in the middle of a fight lands at the next bar line (BeatBoss::requestAccess)
// and on a retry; the tempo of a started fight applies when the def is built again.

#include "ftcore/Api.h"
#include "ftcore/BeatBoss.h"

#include <string>
#include <string_view>

namespace ftcore {

struct BeatAccessOptions {
    float windowScale = 1.0f;
    float whiffLockoutMs = 400.0f;
    int pairMode = 0;
    float uiScale = 1.0f;
    int cutsReduced = 0;
    float shakeScale = 1.0f;
    int flashLevel = 0;
    float tempoScale = 1.0f;

    bool operator==(const BeatAccessOptions&) const = default;
};

// Puts every value in range (NaN becomes the default; the window scale snaps to steps of a quarter).
FTCORE_API void clampBeatAccess(BeatAccessOptions& o);
// {"beat.window_scale":1.0,"beat.whiff_lockout_ms":400,...}: every option, under the ids above.
FTCORE_API std::string encodeBeatAccess(const BeatAccessOptions& o);
// Reads the text; false (and `out` untouched) when it is not a JSON object. A missing or wrong-typed key keeps its
// default; an out-of-range value is clamped; other keys are ignored.
FTCORE_API bool decodeBeatAccess(std::string_view text, BeatAccessOptions& out, std::string* error = nullptr);

// The share Perfect widths take of a window scale: half of what the scale adds, at most 1.5.
FTCORE_API double beatPerfectScale(double windowScale);
// `base` with the options applied (the windows, the lockout, the pair mode). Pure.
FTCORE_API BeatBossTuning beatAccessTuning(const BeatBossTuning& base, const BeatAccessOptions& o);
// The widest window scale (at most `wanted`, in steps of a quarter, at least 1) at which no two windows of one
// button overlap on `def` under `base`.
FTCORE_API float maxBeatWindowScale(const BeatBossDef& def, const BeatBossTuning& base, float wanted);

} // namespace ftcore
