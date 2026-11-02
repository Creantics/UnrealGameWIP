#pragma once

// The Turns arrow lane on screen: where it stands, how fast the arrows travel, its timings and its
// colours, read from Data/Battle/arrow_lane.json, plus the small pure rules the HUD draws from (the
// envelopes of the fade in and out, the streak badge's pop, the early or late tick of a press). The
// Unreal HUD only draws what these say. Nothing here allocates after the load.
//
// Units. Rectangles are in percent of the safe box: the centred 16:9 rectangle of the screen (the
// whole screen at 16:9, the middle 76% of the width at 21:9, the middle band of the height on a
// taller screen). x is a share of its width, y of its height. A rectangle names one corner of the
// box as its anchor and gives its own top-left from there (a negative x from a right anchor goes
// left, a negative y from a bottom anchor goes up).
//
// Scroll. The arrows rise toward the enemies: a note due `dt` seconds from now is at
// y = targetY + dt * speed, speed = (spawnY - targetY) / lookahead, so one second of chart is 46% of
// the box's height at the shipped numbers (about 497 px/s at 1080). A late arrow goes on up through
// the target until its Good window ends.
//
// Lane layout of a combo: columns Left, Down, Up, Right (Arrow.h order) at a fixed pitch from the
// plate's left edge. Arrow.h belongs to the Turns battle alone (cmake/CheckComboFence.cmake).

#include "ftcore/Api.h"
#include "ftcore/ComboSequence.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace ftcore {

struct PxRect {
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
};

// The centred 16:9 rectangle of a width x height screen, in pixels.
FTCORE_API PxRect safeBox(float width, float height);

enum class LaneAnchor : std::uint8_t { TopLeft, TopRight, BottomLeft, BottomRight };

// A rectangle in percent of the safe box, by anchor and offset.
struct LaneRect {
    LaneAnchor anchor = LaneAnchor::TopRight;
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
};

// `r` in pixels inside `box`.
FTCORE_API PxRect resolve(const LaneRect& r, const PxRect& box);

struct LaneColor {
    float r = 1.f, g = 1.f, b = 1.f, a = 1.f;
};

// Colour tokens. Hue is secondary: the grades are told apart by shape and word.
struct ArrowLaneColors {
    LaneColor plate{0.02f, 0.02f, 0.05f, 0.86f};
    LaneColor arrowFill{0.96f, 0.96f, 0.92f, 1.f};
    LaneColor arrowEdge{0.02f, 0.02f, 0.05f, 0.9f};
    LaneColor targetIdle{0.78f, 0.78f, 0.82f, 1.f};
    LaneColor perfect{1.f, 0.82f, 0.25f, 1.f};
    LaneColor great{0.70f, 0.92f, 1.f, 1.f};
    LaneColor good{0.45f, 0.62f, 0.95f, 1.f};
    LaneColor miss{0.85f, 0.35f, 0.35f, 0.7f};
    LaneColor warn{1.f, 0.55f, 0.15f, 1.f};
    LaneColor rim[4] = {{0.34f, 0.71f, 0.91f, 1.f}, {0.80f, 0.47f, 0.65f, 1.f}, {0.f, 0.62f, 0.45f, 1.f}, {0.84f, 0.37f, 0.f, 1.f}};
};

struct ArrowLaneTimes {
    // Chart on screen before a note is due: leadBeats beats at the song's tempo, clamped (see
    // laneLookahead); `lookahead` is used when there is no tempo.
    double lookahead = 1.0;
    double leadBeats = 2.0;
    double lookaheadMin = 0.7, lookaheadMax = 1.2;
    double noteFadeIn = 0.1;     // a note's alpha ramp over its first seconds of travel
    double laneFadeIn = 0.2;     // the plate fades in after the confirm
    double exitFade = 0.25;      // the plate and stamp fade out ending with the recovery
    double exitStart = 0.45;     // after the last judgement
    double exitEnd = 0.70;       // gone by here (the Recovery is 0.7 s)
    double quickFade = 0.15;     // the enemies fell mid-combo: arrows fade at once, no stamp
    double receptorDecay = 0.15; // a pressed receptor lights, then decays
    double burstPerfect = 0.26, burstGreat = 0.22, burstGood = 0.18;
    double wordHold = 0.25, wordOut = 0.35;
    double missFade = 0.2;
    double streakPop = 0.15;
    double streakMissHold = 0.3;
    double strayQuiet = 0.3;    // a stray this soon after the confirm is the confirm press itself: no cue
    double stampDelay = 0.06;   // after the last judgement the plate dims and the stamp pops
    double stampPop = 0.12;
    double fullComboEdge = 0.7;
    double rimHold = 0.25;      // the gold rim on a streak step
    int streakEvery = 4;        // the badge pops every this many in a row
    int streakFrom = 2;         // the badge shows from this many
    float streakPopScale = 1.25f;
    float stampFromScale = 0.8f;
    float plateDim = 0.55f;
    float previewAlpha = 0.25f;   // the receptor row in the choose view
    float unusedAlpha = 0.30f;    // a lane this combo does not use
    float burstFrom = 6.f;        // ring radius, percent of the box's height
    float burstToPerfect = 10.f, burstToGreat = 8.5f, burstToGood = 7.f;
    float wordRisePx = 24.f;      // at 1080 high
    float missSinkPx = 10.f;
};

struct ArrowLaneLayout {
    LaneRect plate{LaneAnchor::TopRight, -35.7f, 35.0f, 17.5f, 58.0f};
    float columnInset = 0.4f;   // from the plate's left edge to column 0 (percent of the box's width)
    float columnPitch = 4.17f;  // percent of the box's width
    float targetY = 44.f;       // the target row's centre (percent of the box's height)
    float spawnY = 90.f;        // where a note enters, its centre
    float glyphWidth = 3.4f;    // percent of the box's width
    float glyphHeight = 6.f;    // percent of the box's height
    float minGlyphPx = 48.f;
    float perfectBandH = 4.2f;  // the Perfect window drawn on the target at scale 1.0, for the layout table
    LaneRect word{LaneAnchor::TopRight, -35.3f, 35.5f, 16.7f, 4.0f};
    float wordStartY = 38.5f;
    LaneRect streak{LaneAnchor::TopRight, -26.2f, 30.5f, 8.0f, 4.0f};
    LaneRect stamp{LaneAnchor::TopRight, -35.3f, 50.0f, 16.7f, 8.0f};
    LaneRect caption{LaneAnchor::TopRight, -35.7f, 94.0f, 17.5f, 3.5f};
    ArrowLaneTimes times;
    ArrowLaneColors colors;
};

// The shipped numbers (the same as Data/Battle/arrow_lane.json, which a test keeps in step).
FTCORE_API ArrowLaneLayout defaultArrowLaneLayout();

// Reads Data/Battle/arrow_lane.json. Missing members keep the shipped default; a number out of range
// refuses the file (false, `*error` says where) and `out` is untouched.
FTCORE_API bool importArrowLaneLayout(std::string_view jsonText, ArrowLaneLayout& out, std::string* error = nullptr);

// The chart on screen before a note is due, in seconds: leadBeats beats at `bpm` clamped to
// [lookaheadMin, lookaheadMax]; the plain `lookahead` when bpm <= 0.
FTCORE_API double laneLookahead(const ArrowLaneTimes& t, double bpm);

// One screen's lane in pixels.
struct LaneGeometry {
    PxRect box;        // the safe box
    PxRect plate;
    float columnX0 = 0.f;  // left edge of column 0
    float pitch = 0.f;
    float glyphW = 0.f, glyphH = 0.f;
    float targetY = 0.f, spawnY = 0.f;
    float pxPerSec = 0.f;  // arrows' speed
    double lookahead = 1.0;

    float columnCentre(int lane) const { return columnX0 + pitch * (static_cast<float>(lane & 3) + 0.5f); }
    // y of a note due `dt` seconds from now (negative: already past the target).
    float noteY(double dt) const { return targetY + static_cast<float>(dt) * pxPerSec; }
    // Half the height of a judgement window of `seconds` each way, in pixels.
    float windowPx(double seconds) const { return static_cast<float>(seconds) * pxPerSec; }
    // A note's alpha: 0 as it enters at the bottom, 1 after `noteFadeIn` seconds of travel.
    FTCORE_API float noteAlpha(const ArrowLaneTimes& t, double dt) const;
};

FTCORE_API LaneGeometry laneGeometry(const ArrowLaneLayout& layout, float width, float height, double lookahead);

// ---- envelopes (ages in seconds; every result 0..1 unless said) ----

// The plate and arrows' visibility: fades in over laneFadeIn from the confirm (`age`); `endAge` < 0
// while the combo runs, else the seconds since its last judgement: it holds until exitStart, then
// fades over exitFade, gone at exitEnd. `quick`: the enemies fell, so it fades over quickFade at once.
FTCORE_API float laneVisibility(const ArrowLaneTimes& t, double age, double endAge, bool quick);

// The plate's extra dimming once the stamp is up (1 until stampDelay after the last judgement, then
// plateDim).
FTCORE_API float plateDimming(const ArrowLaneTimes& t, double endAge);

// The summary stamp: appears stampDelay after the last judgement and shares the exit.
FTCORE_API float stampAlpha(const ArrowLaneTimes& t, double endAge);
// Its scale: stampFromScale to 1 over stampPop.
FTCORE_API float stampScale(const ArrowLaneTimes& t, double endAge);

// The streak badge's pop after a step: streakPopScale falling to 1 over streakPop (1 when `age` is
// outside it).
FTCORE_API float streakPopScale(const ArrowLaneTimes& t, double age);
// True when a streak of `n` pops (n >= streakFrom and a multiple of streakEvery).
FTCORE_API bool streakStep(const ArrowLaneTimes& t, int n);

// How long a grade's burst lasts, how big its ring grows (percent of the box's height), and whether it
// has the four-point star (Perfect only; not under reduce flashes).
FTCORE_API double burstLength(const ArrowLaneTimes& t, Grade g);
FTCORE_API float burstRadius(const ArrowLaneTimes& t, Grade g, double age);
inline bool burstHasStar(Grade g, bool reduceFlashes) { return g == Grade::Perfect && !reduceFlashes; }

// Whether a stray press shows the TOO EARLY cue: not within strayQuiet of the confirm.
FTCORE_API bool strayCueShows(const ArrowLaneTimes& t, double sinceConfirm);

// The early or late mark of a judged note: -1 early, +1 late, 0 none. A Perfect has none; a Miss that
// expired unpressed (`pressed` false) is late; otherwise the press error's sign.
FTCORE_API int pressTick(Grade g, bool pressed, double error);
// Where the press landed on the lane, as pixels from the target (positive = below it): the arrow
// was `error` seconds from the target, so a late press (error > 0) has the arrow above it. Clamped to
// the Good window of `good` seconds.
FTCORE_API float tickOffsetPx(const LaneGeometry& g, double error, double good);

// The worse (lower) of two grades, for a jump pair's single word: Miss < Good < Great < Perfect.
FTCORE_API Grade worseGrade(Grade a, Grade b);

} // namespace ftcore
