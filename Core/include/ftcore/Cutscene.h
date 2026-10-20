#pragma once

// The cutscene screen's rules: a shot list (CutscenePlan), the clock that plays it (CutscenePlayer),
// the camera each shot asks for (cutsceneCamera) and the data file that says which shot list a story
// step plays (Data/Cutscene/cutscenes.json, CutsceneTable).
//
//   ftcore::CutsceneTable table;
//   std::string error;
//   if (!ftcore::importCutsceneTable(text, table, &error)) ...;          // allocates: at load
//   ftcore::CutscenePlayer player;
//   player.begin(table.find(sceneId, beat, step), autoPlay);            // the default when not listed
//   every frame:
//       player.update(gameSeconds);                                      // advances the shots
//       player.tickReal(realSeconds);                                    // input grace, autoplay cap
//       on a skip press: player.press();
//       const ftcore::CutsceneShot& shot = player.currentShot();
//       ftcore::CameraPose pose = ftcore::cutsceneCamera(shot, player.shotProgress(), spot, subject, subject2);
//       if (player.blending()) pose = ftcore::lerpPose(fromPose, pose, player.blendWeight());
//       when player.finished(): continue
//
// Units: metres, z up (the Unreal side multiplies by 100). A subject position is the point the
// subject stands on (its feet); the framing picks the height the camera looks at.
//
// Ids only, no text: a shot names a camera spot and one or two subjects by id (lower-case letters,
// digits and _, at most 15 characters), which the screen matches to actors in its map. Fixed
// capacity: a plan holds at most kMaxShots shots, copied by value; nothing allocates after load.
//
// cutscenes.json:
//   { "version": 1,
//     "default": { "shots": [ { "spot": "spot_a", "framing": "wide", "duration": 3, "subject": "subj_a" } ] },
//     "cutscenes": [ { "scene": "SC-X", "beat": 4, "step": 0, "shots": [ ... ] } ] }
//   Shot keys: spot and subject (required ids), subject2 (an id; required by two_shot and
//   over_shoulder), framing ("wide", "medium" default, "close", "over_shoulder", "two_shot",
//   "insert"), duration (seconds, 0.1..60, default 2), move ("static" default, "push_in", "pull_out",
//   "pan", "orbit"), amount (metres for push_in and pull_out, 0..20, default 0.5; degrees for pan
//   and orbit, -360..360, default 20 and 30), transition ("cut" default, "blend"), blend (seconds of
//   the incoming shot spent blending from the previous one, 0..duration, default 0.5 for a blend).
//   `step` defaults to 0. A duplicate (scene, beat, step), an empty or over-long shot list, a bad id
//   or a value out of range is an error; unknown keys are warnings.

#include "ftcore/Api.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

enum class ShotFraming : std::uint8_t { Wide, Medium, Close, OverShoulder, TwoShot, Insert };
enum class CameraMove : std::uint8_t { Static, PushIn, PullOut, Pan, Orbit };
enum class ShotTransition : std::uint8_t { Cut, Blend };

// "wide", "medium", "close", "over_shoulder", "two_shot", "insert".
FTCORE_API std::string_view shotFramingName(ShotFraming framing);
// "static", "push_in", "pull_out", "pan", "orbit".
FTCORE_API std::string_view cameraMoveName(CameraMove move);
// "cut", "blend".
FTCORE_API std::string_view shotTransitionName(ShotTransition transition);

struct CutsceneShot {
    static constexpr std::uint32_t kIdCapacity = 16; // 15 characters and a terminator
    static constexpr float kMinDuration = 0.1f;

    char spotId[kIdCapacity] = {};
    char subjectId[kIdCapacity] = {};
    char subject2Id[kIdCapacity] = {}; // empty: no second subject
    std::uint8_t spotLength = 0, subjectLength = 0, subject2Length = 0;
    ShotFraming framing = ShotFraming::Medium;
    CameraMove move = CameraMove::Static;
    float moveAmount = 0.f; // metres (push_in, pull_out) or degrees (pan, orbit)
    float duration = 2.f;   // seconds
    ShotTransition transition = ShotTransition::Cut;
    float blendTime = 0.f;  // seconds at the start of this shot spent blending in (Blend only)

    std::string_view spot() const { return {spotId, spotLength}; }
    std::string_view subject() const { return {subjectId, subjectLength}; }
    std::string_view subject2() const { return {subject2Id, subject2Length}; }
    bool hasSubject2() const { return subject2Length > 0; }

    // Each copies `id`; false (and unchanged) when it does not fit or is not a valid id. An empty
    // id is valid for subject2 only (it clears it).
    FTCORE_API bool setSpot(std::string_view id);
    FTCORE_API bool setSubject(std::string_view id);
    FTCORE_API bool setSubject2(std::string_view id);
};

// Lower-case letters, digits and _, 1..15 characters.
FTCORE_API bool isCutsceneId(std::string_view id);

class CutscenePlan {
public:
    static constexpr std::uint32_t kMaxShots = 16;

    void clear() { count_ = 0; }
    // Appends a copy of `shot` with its duration raised to kMinDuration and its blend time kept in
    // 0..duration (0 for a cut). False when the plan is full.
    FTCORE_API bool add(const CutsceneShot& shot);

    std::uint32_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    const CutsceneShot& shot(std::uint32_t i) const { return shots_[i]; }
    FTCORE_API float totalDuration() const;

private:
    CutsceneShot shots_[kMaxShots];
    std::uint32_t count_ = 0;
};

enum class CutscenePhase : std::uint8_t {
    Idle,    // not begun
    Playing, // a shot is on screen
    Done     // every shot played, or skipped
};

class CutscenePlayer {
public:
    // A skip press counts only after this much real time, so the press that opened the screen
    // can't skip it.
    static constexpr double kInputGrace = 0.25;
    // With autoplay the cutscene skips itself after this much real time (an alpha run stays fast).
    static constexpr double kAutoPlayRealLimit = 1.0;

    // Starts at shot 0 with a copy of `plan`. An empty plan is Done at once.
    FTCORE_API void begin(const CutscenePlan& plan, bool autoPlay = false);
    // Advances the shot clock by `gameSeconds` (several shots when it is long).
    FTCORE_API void update(double gameSeconds);
    // Real time for the input grace and the autoplay cap.
    FTCORE_API void tickReal(double realSeconds);
    // Ends now (Done, skipped). False when it was not playing.
    FTCORE_API bool skip();
    // A skip press: skips only after kInputGrace of real time. True when it skipped.
    FTCORE_API bool press();

    CutscenePhase phase() const { return phase_; }
    bool playing() const { return phase_ == CutscenePhase::Playing; }
    bool finished() const { return phase_ == CutscenePhase::Done; }
    bool skipped() const { return skipped_; }
    bool autoPlay() const { return autoPlay_; }
    const CutscenePlan& plan() const { return plan_; }
    std::uint32_t shotCount() const { return plan_.size(); }
    // The shot on screen (the last one once Done); only meaningful when shotCount() > 0.
    std::uint32_t currentIndex() const { return index_; }
    const CutsceneShot& currentShot() const { return plan_.shot(index_); }
    // 0..1 through the current shot (1 once Done).
    FTCORE_API float shotProgress() const;
    // Seconds into the current shot.
    double shotTime() const { return shotTime_; }
    // Game seconds played and real seconds since begin.
    double elapsed() const { return elapsed_; }
    double realElapsed() const { return realElapsed_; }

    // True while the current shot blends in from the previous one (a Blend shot after the first,
    // within its blend time).
    FTCORE_API bool blending() const;
    // The previous shot (the one being blended from); only meaningful while blending().
    const CutsceneShot& previousShot() const { return plan_.shot(index_ > 0 ? index_ - 1 : 0); }
    // Eased 0..1 weight of the current shot's camera while blending; 1 otherwise.
    FTCORE_API float blendWeight() const;

private:
    CutscenePlan plan_;
    CutscenePhase phase_ = CutscenePhase::Idle;
    std::uint32_t index_ = 0;
    double shotTime_ = 0.0;
    double elapsed_ = 0.0;
    double realElapsed_ = 0.0;
    bool autoPlay_ = false;
    bool skipped_ = false;
};

struct CutsceneVec3 {
    float x = 0.f, y = 0.f, z = 0.f;
};

struct CameraPose {
    CutsceneVec3 position;
    CutsceneVec3 lookAt;
};

// Framing constants (metres): how far the camera stands from the point it looks at, how high that
// point is above the subject's feet, and how high the camera is. Wide stands at the spot itself.
struct FramingRule {
    float distance;
    float lookHeight;
    float cameraHeight;
};
FTCORE_API FramingRule framingRule(ShotFraming framing);

// The camera of `shot` at `progress` (0..1, eased) through it. `spot`: the camera spot's position;
// `subject`, `subject2`: the subjects' feet (subject2 may be null).
//   - wide: the camera stands at the spot and looks at the subject;
//   - medium, close, insert: the camera stands on the line from the subject toward the spot (seen
//     from above), at the framing's distance and height;
//   - two_shot: looks at the middle of both subjects, far enough back to hold both; the spot gives
//     the side (medium without subject2);
//   - over_shoulder: stands behind subject2's shoulder looking at the subject; the spot picks the
//     shoulder (medium without subject2).
// Moves: push_in walks toward the look point by `amount` metres over the shot (stopping 0.3 m short),
// pull_out walks away; pan turns the look point about the camera from -amount/2 to +amount/2
// degrees; orbit turns the camera about the look point by `amount` degrees. No allocation.
FTCORE_API CameraPose cutsceneCamera(const CutsceneShot& shot, float progress, const CutsceneVec3& spot,
                                     const CutsceneVec3& subject, const CutsceneVec3* subject2);
// Linear blend of both points (t clamped to 0..1).
FTCORE_API CameraPose lerpPose(const CameraPose& from, const CameraPose& to, float t);

struct CutsceneEntry {
    std::string scene;
    std::uint32_t beat = 0, step = 0;
    CutscenePlan plan;
};

struct CutsceneTable {
    CutscenePlan fallback; // the "default" shot list
    std::vector<CutsceneEntry> entries;

    // The entry for (scene, beat, step), or nullptr. No allocation.
    FTCORE_API const CutsceneEntry* findEntry(std::string_view scene, std::uint32_t beat, std::uint32_t step) const;
    // The shot list for (scene, beat, step): the entry's, else the default. No allocation.
    FTCORE_API const CutscenePlan& find(std::string_view scene, std::uint32_t beat, std::uint32_t step) const;
};

// On failure `out` is empty and *error lists each problem with its path, one per line.
FTCORE_API bool importCutsceneTable(std::string_view json, CutsceneTable& out, std::string* error = nullptr,
                                    std::vector<std::string>* warnings = nullptr);

} // namespace ftcore
