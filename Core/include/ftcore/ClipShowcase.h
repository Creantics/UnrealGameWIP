#pragma once

// A boss's clips shown one at a time for review (the arena's -ArenaShowcase capture): no fight, no lead, a fixed camera.
// The reel is built once from the clip table and the host's picks (which clip plays each part, and the attacks with
// where the fight enters and leaves their clips); each frame the host asks for the pose at the reel's time and plays the
// effects of the cues it crossed. The clips play at rate 1.0 and cross-fade as the fight's do; a cut (a hidden boss
// between two takes, or a take begun at once) has no cross-fade.
//
// The order: a turntable on the idle (one turn over its first cycle, then still, so its seam shows twice); the second
// idle; every attack as idle -> its clip from the fight's entry to the move's end, its tell from the entry to the
// impact and its dust at the impact (a marked move's tell light through its window, tellLightWindow) -> idle; the light
// and heavy hits (chips at each); the stagger; the go-down, the
// hold (two cycles) and the get-up; the stepping turns and the walk out and back; the effects alone (a tell ahead, its
// dust, chips on the body, then turned so the weak point faces the front (weakFacing): its glow, then a crit); the two
// deaths.
//
// Angles are degrees in the reel's frame, + turning right; positions are metres, x ahead of the reel's front, y to its
// right. A stepping turn follows the fight's: each cycle of the turn clip yaws turnDeg, eased (smootherstep) over the
// cycle's first turnPivot seconds.

#include "ftcore/Api.h"
#include "ftcore/BeatChoreo.h"
#include "ftcore/Explore.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ftcore {

inline constexpr std::size_t kMaxShowSegments = 160;
inline constexpr std::size_t kMaxShowCues = 128;
inline constexpr std::size_t kMaxShowAttacks = 24;
inline constexpr std::size_t kMaxShowLabel = 39; // characters

// What the host plays at a cue (append only). LightOn and LightOff: a marked move's tell light, at its window's clip times.
enum class ShowCue : std::uint8_t { Tell, TellStop, Impact, Chips, GlowOn, GlowOff, Crit, ChipsHeavy, LightOn, LightOff };
// Where: the attack's strike point (its tell and dust, as the fight places them), a point ahead of the boss, its body's
// surface toward the camera, or its weak point (the glow and the crit, at the fight's flash point).
enum class ShowPlace : std::uint8_t { Strike, Ahead, Surface, WeakPoint };

FTCORE_API const char* showCueKey(ShowCue c);

struct ShowAttack {
    std::int16_t clip = kNoClip;
    std::int16_t move = -1; // the host's move (for its strike point), -1: none
    double entry = 0.0;     // s into the clip the fight enters it at (at rate 1.0)
    double until = 0.0;     // s into the clip the fight leaves it for the idle (the move's end); 0: the clip's end
    bool left = false;      // the clip strikes to the left (the called side, for the strike point)
    bool marked = false;    // the move is marked (its data's flag): its clip's tell light window gets a LightOn/LightOff pair
    double fightRate = 1.0; // the rate the fight plays the clip at for this move (beatClipRate; the reel plays it at 1.0)
};

// The clips are indices into the table (kNoClip: the part is left out). The idle is required.
struct ShowSpec {
    std::int16_t idle = kNoClip;
    std::int16_t idleTwo = kNoClip;
    std::int16_t walk = kNoClip;
    std::int16_t turnL = kNoClip; // both turns or neither
    std::int16_t turnR = kNoClip;
    std::int16_t stagger = kNoClip;
    std::int16_t downIn = kNoClip; // the go-down, the hold and the get-up (the hold alone is enough)
    std::int16_t down = kNoClip;
    std::int16_t downOut = kNoClip;
    std::int16_t defeat = kNoClip;
    std::int16_t defeatKneel = kNoClip; // a death from the hold
    std::int16_t hitLight = kNoClip;
    std::int16_t hitHeavy = kNoClip;
    std::array<ShowAttack, kMaxShowAttacks> attacks{};
    std::size_t attackCount = 0;
    double walkFrom = 0.0;  // s into the walk the fight starts it at
    float walkSpeed = 0.f;  // m/s: 0 is the clip's authored speed
    int walkCycles = 2;     // each way
    int turnSteps = 3;      // each turn out and back (a half turn is twice this)
    float turnDeg = 30.f;
    double turnPivot = 0.35;
    double before = 1.0;    // idle before an attack (s)
    double after = 1.5;     // idle after a take (s)
    double hold = 1.5;      // a death's last frame held (s)
    double hidden = 0.5;    // the boss hidden between two takes (s)
    double tellFor = 2.4;   // the lone tell (s)
    double glowFor = 3.0;   // the lone glow (s)
    // Where the weak point faces, deg in the boss's frame (0 ahead, 90 its right, 180 behind). The glow and crit takes
    // turn the boss so it faces the front: with turn clips in whole steps (rounded), without them from a hidden cut; a
    // weak point ahead is not turned.
    float weakFacing = 180.f;
    // The weak point's window inside the go-down's hold (s), as the fight opens it there: 0 (the default) plays the hold
    // without one. Over 0, the hold's first frame takes a glow_on, the window's middle a crit and its end a glow_off.
    // downGlowTwo over 0 adds a second go-down (a later phase's) with a window that long. Each must fit in the hold.
    double downGlow = 0.0;
    double downGlowTwo = 0.0;
};

struct ShowSegment {
    std::array<char, kMaxShowLabel + 1> labelText{};
    std::int16_t clip = kNoClip; // kNoClip: the boss hidden
    std::int16_t attack = -1;    // the spec's attack this plays, -1: none
    double start = 0.0;          // s into the reel
    double length = 0.0;
    double clipFrom = 0.0;       // s into the clip at the segment's start
    double clipLength = 0.0;     // the clip's length
    bool loop = false;           // the clip wraps, else it holds its last frame
    bool cut = false;            // shown at once, no cross-fade in
    bool goesOn = false;         // the same take as the segment before goes on (the same clip, no new start)
    float yaw = 0.f;             // at the start
    float spin = 0.f;            // deg/s (the turntable)
    float stepDeg = 0.f;         // a stepping turn: per cycle of its clip (signed)
    double cycle = 0.0;          // the stepping turn's cycle (its clip's length)
    double pivot = 0.0;
    float speed = 0.f;           // m/s ahead (the walk)
    Vec2 pos{};                  // at the start

    std::string_view label() const { return std::string_view(labelText.data()); }
};

struct ShowCueAt {
    double at = 0.0;
    ShowCue cue = ShowCue::Tell;
    ShowPlace place = ShowPlace::Strike;
    std::int16_t attack = -1; // the spec's attack (a Strike cue)
};

struct ClipShow {
    std::array<ShowSegment, kMaxShowSegments> segments{};
    std::size_t segmentCount = 0;
    std::array<ShowCueAt, kMaxShowCues> cues{}; // in time order
    std::size_t cueCount = 0;
    double length = 0.0;
};

struct ShowPose {
    int segment = -1; // -1: the reel is over
    std::int16_t clip = kNoClip;
    double clipTime = 0.0; // s into the clip (wrapped when it loops)
    double segmentTime = 0.0;
    bool loop = false;
    float yaw = 0.f;
    Vec2 pos{};
};

// False (and `out` empty) when the idle is missing, a clip index is outside the table, only one turn is given, an
// attack enters past its own end, the weak point's facing is not a number, or the reel outgrows its arrays; *error says
// which.
FTCORE_API bool buildClipShow(const BeatClips& clips, const ShowSpec& spec, ClipShow& out, std::string* error = nullptr);

// The pose at `t` s into the reel.
FTCORE_API ShowPose clipShowPose(const ClipShow& show, double t);

// The cues with from < at <= to: indices [first, last) into show.cues.
FTCORE_API void clipShowCues(const ClipShow& show, double from, double to, std::size_t& first, std::size_t& last);

} // namespace ftcore
