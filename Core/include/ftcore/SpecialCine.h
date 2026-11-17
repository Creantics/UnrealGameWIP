#pragma once

// A special move's cinematic: the shot list a lead's special plays over a frozen fight, and the clock that
// plays it. Two files:
//   - Data/Cutscene/specials.json, the table: one entry per move with its id, the shot file, the release beat
//     and the f-stop;
//   - the shot file each entry names (Data/Cutscene/<file>.json), as the animation lane exports it: kept
//     identical to the export so a new take is a copy.
//
//   ftcore::SpecialCineTable table;
//   importSpecialCineTable(tableText, table, &error, &warnings);            // allocates: at load
//   for each move i: importSpecialCineShots(fileText(table.shotFile(i)), table.edit(i), &error, &warnings);
//   ftcore::SpecialCinePlayer player;
//   player.begin(table.find("devin_special_01"));   // nullptr or a move with no shots: never active
//   every frame, while player.active():
//       player.step(seconds);                       // the cinematic's own clock, never dilated
//       stage.bone = <the shot's follow bone, world metres>  (only a "bone" shot reads it)
//       const ftcore::SpecialCineFrame f = player.frame(stage);
//       draw: the shot's camera (f.pose, f.fovDegrees, f.focus, f.fStop, plus f.shake), blended toward the
//             play camera by f.playWeight; f.cut: a hard cut this frame (no motion blur or history across it)
//   when player.finished(): the play camera, with no blend
//
// The stage (SpecialCineStage) is fixed at the press, apart from the bone point: the frame's origin (the lead's
// feet then), its x (along the ground from the lead toward its target), the target's feet, height and radius.
// Axes: x forward, y right (Unreal's: forward (1,0) has right (0,1)), z up; metres. A shot point is written in
// that frame from an anchor:
//   - "lead" and "world": from the origin (the shots are written for the lead's start; the lead's own travel is
//     in the shot numbers, not followed);
//   - "target": from the target's feet, pushed out along its own ground direction by the target's radius (a
//     shot beside a big body stands beside it, not in it), its z scaled by the target's height over 1.8 m;
//   - "bone": from the stage's bone point (the shot's follow_bone, fed by the host each frame).
//
// The shot file (version 1):
//   { "version": 1, "clip": "A_...", "duration_s": 8.5, "fps": 60, "frame": "<words>", "sensor_width_mm": 36,
//     "aspect": "16:9", "shake": { "profile": "decaying", "amp_m": [x, y, z], "hz": [x, y, z], "decay_s": 0.2 },
//     "end_feet": [x, y, z], "cuts": [s, ...],
//     "shots": [ { "id": 1, "name": "...", "in": 0, "out": 0.9, "anchor": "lead", "offset": { "from": [..], "to": [..] },
//                  "look_anchor": "lead", "look_at": { "from": [..], "to": [..] }, "ease": "smoothstep",
//                  "focal_mm": 40 or { "from": 40, "to": 31 }, "focus_m": 1.8 or { "from": .., "to": .. },
//                  "shake": false, "follow_bone": "hand_r", "note": "..." } ] }
//   Shots play in order and touch (each "in" is the one before's "out" within 1 ms, the first at 0, each at least
//   0.1 s), the last "out" is duration_s (within 1 ms). "cuts" must be the shots' inner starts (a mismatch is a
//   warning). The camera and look-at are eased from "from" to "to" over the shot by its ease ("smoothstep" or
//   "linear"), and so are the lens and the focus. focus_m 0: the distance to the look-at point. Shake: on a shaking
//   shot, the eye moves by amp * e^(-u / decay) * sin(2 pi hz u + phase) per axis of the stage frame, u the time into
//   the shot. "note", "frame", "fps", "aspect" and "name" are read for logs or ignored.
//
// The table:
//   { "version": 1, "moves": [ { "id": "devin_special_01", "shots": "special_devin_01", "release": 7.1, "fstop": 2.8,
//                                "blend_in": 0, "blend_out": 0.2 } ] }
//   "shots": the shot file's name under Data/Cutscene (no extension). "release": the move's release beat on its
//   clock (default: the first shaking shot's "in", else the last shot's). "fstop" 0.7..32 (default 2.8).
//   "blend_in": seconds from the play camera into the first shot (default 0: a hard cut in). "blend_out": seconds
//   over which the move's camera is eased into the play camera, ending on it (default: the whole last shot; longer
//   than the last shot, the blend starts in the shot before and runs over the cut).
//
// A bad id, a duplicate move, a gap or an overlap, or a value out of range is an error; unknown keys are warnings.
// Fixed capacity, copied by value: nothing allocates after load.

#include "ftcore/Api.h"
#include "ftcore/Cutscene.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

enum class SpecialCineAnchor : std::uint8_t { Lead, World, Target, Bone };
enum class SpecialCineEase : std::uint8_t { Smooth, Linear };

// "lead", "world", "target", "bone".
FTCORE_API std::string_view specialCineAnchorName(SpecialCineAnchor anchor);

struct SpecialCineShot {
    static constexpr std::uint32_t kNameCapacity = 24; // 23 characters and a terminator (longer names are cut)
    static constexpr std::uint32_t kBoneCapacity = 16; // 15 characters and a terminator
    float in = 0.f, out = 0.f;                         // seconds on the move's clock
    SpecialCineAnchor cameraAnchor = SpecialCineAnchor::Lead;
    CutsceneVec3 cameraFrom{}, cameraTo{};             // the camera's place at the shot's start and end
    SpecialCineAnchor lookAnchor = SpecialCineAnchor::Lead;
    CutsceneVec3 lookFrom{}, lookTo{};
    SpecialCineEase ease = SpecialCineEase::Smooth;
    float lensFrom = 35.f, lensTo = 35.f; // mm
    float focusFrom = 0.f, focusTo = 0.f; // metres; 0: the distance to the look-at point
    float fStop = 2.8f;
    bool shake = false;
    char nameText[kNameCapacity] = {};
    std::uint8_t nameLength = 0;
    char boneText[kBoneCapacity] = {};
    std::uint8_t boneLength = 0;
    std::string_view name() const { return {nameText, nameLength}; }
    // The bone a "bone" anchored point follows (empty: none).
    std::string_view followBone() const { return {boneText, boneLength}; }
};

struct SpecialCineShake {
    CutsceneVec3 amplitude{}; // metres per axis of the stage frame
    CutsceneVec3 hz{};
    float decay = 0.2f;       // seconds (e-folding time)
};

class SpecialCineMove {
public:
    static constexpr std::uint32_t kMaxShots = 12;
    static constexpr std::uint32_t kIdCapacity = 24;   // 23 characters and a terminator
    static constexpr std::uint32_t kFileCapacity = 40; // 39 characters and a terminator
    static constexpr std::uint32_t kClipCapacity = 40;
    static constexpr float kDefaultFStop = 2.8f;

    std::string_view id() const { return {id_, idLength_}; }
    // The shot file's name under Data/Cutscene, no extension.
    std::string_view shotFile() const { return {file_, fileLength_}; }
    // The body clip the shot file names (empty before the shots are read).
    std::string_view clip() const { return {clip_, clipLength_}; }
    std::uint32_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    const SpecialCineShot& shot(std::uint32_t i) const { return shots_[i < count_ ? i : (count_ > 0 ? count_ - 1 : 0)]; }
    // The last shot's out (0 with no shots).
    float length() const { return count_ > 0 ? shots_[count_ - 1].out : 0.f; }
    float blendIn() const { return blendIn_; }
    // The play camera's blend over the move's end; the whole last shot when not set.
    FTCORE_API float blendOut() const;
    // The release beat: as set, else the first shaking shot's in, else the last shot's in.
    FTCORE_API float release() const;
    float fStop() const { return fStop_; }
    const SpecialCineShake& shake() const { return shake_; }
    // The lead's feet at the move's end in the stage frame (metres): its travel.
    const CutsceneVec3& endFeet() const { return endFeet_; }
    // The shot on screen at `t` seconds (clamped to the move): the last whose in is at or before it.
    FTCORE_API std::uint32_t shotAt(float t) const;

    // Building (the importers and tests). setId/setShotFile/setClip: false, and unchanged, unless the text fits
    // (ids: a-z, 0-9, _; files and clips: those and A-Z). add: false, and unchanged, when the list is full, or the
    // shot does not start at the last one's out (within 1 ms; the first at 0), is under 0.1 s, ends past 20 s, or
    // carries a value out of range (lens 10..300 mm, focus 0..100 m, f-stop 0.7..32).
    FTCORE_API bool setId(std::string_view id);
    FTCORE_API bool setShotFile(std::string_view file);
    FTCORE_API bool setClip(std::string_view clip);
    FTCORE_API bool add(const SpecialCineShot& shot);
    FTCORE_API void clearShots();
    // In 0..1 s (NaN or negative: 0). Out: 0..the last shot's length, < 0 for the whole last shot.
    FTCORE_API void setBlends(float inSeconds, float outSeconds);
    // In 0..length; < 0 (or NaN): the default.
    FTCORE_API void setRelease(float seconds);
    // 0.7..32 (out of range: the default).
    FTCORE_API void setFStop(float fStop);
    FTCORE_API void setShake(const SpecialCineShake& shake);
    FTCORE_API void setEndFeet(const CutsceneVec3& feet);
    FTCORE_API void clear();

private:
    SpecialCineShot shots_[kMaxShots];
    std::uint32_t count_ = 0;
    char id_[kIdCapacity] = {};
    std::uint8_t idLength_ = 0;
    char file_[kFileCapacity] = {};
    std::uint8_t fileLength_ = 0;
    char clip_[kClipCapacity] = {};
    std::uint8_t clipLength_ = 0;
    float blendIn_ = 0.f;
    float blendOut_ = -1.f; // < 0: the whole last shot
    float release_ = -1.f;  // < 0: the default
    float fStop_ = kDefaultFStop;
    SpecialCineShake shake_{};
    CutsceneVec3 endFeet_{};
};

class SpecialCineTable {
public:
    static constexpr std::uint32_t kMaxMoves = 8;
    std::uint32_t size() const { return count_; }
    const SpecialCineMove& move(std::uint32_t i) const { return moves_[i < count_ ? i : 0]; }
    // For the shot importer: the move at `i` (i < size()).
    SpecialCineMove& edit(std::uint32_t i) { return moves_[i < count_ ? i : 0]; }
    // The move with this id, or nullptr. No allocation.
    FTCORE_API const SpecialCineMove* find(std::string_view id) const;
    // False when full or the id is already there.
    FTCORE_API bool add(const SpecialCineMove& move);
    void clear() { count_ = 0; }

private:
    SpecialCineMove moves_[kMaxMoves];
    std::uint32_t count_ = 0;
};

// Parses Data/Cutscene/specials.json into `table` (cleared first): ids, shot files, release, f-stop, blends; no
// shots yet. False with `error` set on any error (the table is then empty); warnings (unknown keys) go to
// `warnings` when given. Allocates: at load only.
FTCORE_API bool importSpecialCineTable(std::string_view text, SpecialCineTable& table, std::string* error = nullptr,
                                       std::vector<std::string>* warnings = nullptr);
// Parses a shot file into `move` (its shots cleared first; its id, file, release, f-stop and blends kept, and
// checked again against the shots). False with `error` set on any error (the move then has no shots).
FTCORE_API bool importSpecialCineShots(std::string_view text, SpecialCineMove& move, std::string* error = nullptr,
                                       std::vector<std::string>* warnings = nullptr);

struct SpecialCineStage {
    CutsceneVec3 origin;                     // the frame's origin: the lead's feet at the press (world metres)
    CutsceneVec3 forward{1.f, 0.f, 0.f};     // the frame's x on the ground (normalised by the maths)
    CutsceneVec3 target;                     // the target's feet
    float targetHeight = 1.8f;               // metres
    float targetRadius = 0.f;                // metres
    CutsceneVec3 bone;                       // the follow bone, world metres (fed each frame for "bone" shots)
};

struct SpecialCineFrame {
    CameraPose pose;              // the shot's camera (world metres, z up), shake not included
    CutsceneVec3 shake{};         // metres added to the eye (and the look-at), world axes
    float fovDegrees = 60.f;      // horizontal
    float focus = 1.f;            // metres to the plane in focus
    float fStop = 2.8f;
    float playWeight = 1.f;       // 1: the play camera; 0: the shot's camera
    std::uint32_t shot = 0;
    bool cut = false;             // a hard cut lands this frame (the cut in, when there is no blend in, and between shots)
    bool release = false;         // the release beat was crossed this frame
};

// A shot moved by the host when its camera, or the camera's line to the lead, would be inside a body or the world
// (tested once at the start: the world is frozen). Applied in this order: mirrored to the stage's other side (the
// camera's and the look-at's side offsets negated), raised `raise` metres, then pulled `pull` metres along its own
// axis toward the look-at (never closer than kMinPullGap). An explicit focus is shortened by the pull.
struct SpecialCineShotFix {
    static constexpr float kMinPullGap = 0.5f; // metres left between the pulled camera and its look-at
    bool mirror = false;
    float raise = 0.f; // metres, up (negative or NaN: none)
    float pull = 0.f;  // metres, toward the look-at (negative or NaN: none)
    bool moved() const { return mirror || raise > 0.f || pull > 0.f; }
};

// The horizontal fov of a lens in mm on a 36 mm sensor, degrees (10 mm and below as 10 mm).
FTCORE_API float specialCineFov(float lensMm);
// A point of the stage frame from `anchor` (see the header). No allocation.
FTCORE_API CutsceneVec3 specialCinePoint(const SpecialCineStage& stage, SpecialCineAnchor anchor, const CutsceneVec3& at);
// The camera of `move` at `t` seconds for `stage` (cut and release left false: the player sets them). `fixes`: null,
// or one per shot of the move (the shot at `t` is moved by its fix).
FTCORE_API SpecialCineFrame specialCineFrameAt(const SpecialCineMove& move, float t, const SpecialCineStage& stage,
                                               const SpecialCineShotFix* fixes = nullptr);

class SpecialCinePlayer {
public:
    // Starts `move` at 0. Null or empty: stays idle. A begin while playing starts again.
    FTCORE_API void begin(const SpecialCineMove* move);
    // Advances the move's clock by `seconds` (NaN or negative: 0). Finished at the move's length.
    FTCORE_API void step(float seconds);
    // Ends now: finished (the skip; the host's world runs again).
    FTCORE_API void stop();
    // Moves shot `shot` of the playing move for the rest of this play (a begin clears every fix). False out of range.
    FTCORE_API bool setFix(std::uint32_t shot, const SpecialCineShotFix& fix);
    const SpecialCineShotFix& fix(std::uint32_t shot) const { return fixes_[shot < SpecialCineMove::kMaxShots ? shot : 0]; }
    bool active() const { return move_ != nullptr && !finished_; }
    bool finished() const { return finished_; }
    float time() const { return time_; }
    const SpecialCineMove* move() const { return move_; }
    // This frame's camera: cut on the first frame() of a move with no blend in and on the first frame() in a new
    // shot; release on the first frame() at or past the release beat (each once). Idle: the default frame.
    FTCORE_API SpecialCineFrame frame(const SpecialCineStage& stage);

private:
    const SpecialCineMove* move_ = nullptr;
    float time_ = 0.f;
    bool framed_ = false; // a frame() was taken since begin
    std::uint32_t lastShot_ = 0;
    bool finished_ = false;
    bool releaseTaken_ = false;
    SpecialCineShotFix fixes_[SpecialCineMove::kMaxShots]{};
};

} // namespace ftcore
