#pragma once

// The cutscene screen's rules: a shot list (CutscenePlan), the clock that plays it (CutscenePlayer),
// the camera each shot asks for (cutsceneCamera), the blockout stage the shots are written against
// (cutsceneStageMark, StageFrame) and the data file that says which shot list a story step plays
// (Data/Cutscene/cutscenes.json, CutsceneTable).
//
//   ftcore::CutsceneTable table;
//   std::string error;
//   if (!ftcore::importCutsceneTable(text, table, &error)) ...;          // allocates: at load
//   ftcore::CutscenePlayer player;
//   player.begin(table.find(sceneId, beat, step), autoPlay, inPlace);   // the default when not listed
//   every frame:
//       player.update(gameSeconds);                                      // the blends and the shots
//       player.tickReal(realSeconds);                                    // input grace, autoplay cap
//       on a skip press: player.press();
//       const ftcore::CutsceneShot& shot = player.currentShot();
//       ftcore::CameraPose pose = ftcore::cutsceneCamera(shot, player.shotProgress(), spot, subject, subject2);
//       if (player.blending()) pose = ftcore::lerpPose(fromPose, pose, player.blendWeight());
//       in place, while player.blendingIn(): ease from the gameplay camera to pose by blendInWeight();
//                 while player.blendingOut(): ease from the held camera to the gameplay one by blendOutWeight()
//       when player.finished(): continue
//
// In place: a cutscene can play where the gameplay is instead of on the Cutscene map. The stage is
// laid over the map with subj_a's mark on the lead's feet and its front along the lead's facing
// (anchorForLead, stageToWorld), and the player eases in from the gameplay camera (BlendIn) and back
// out to it (BlendOut) over the plan's blend times.
//
// Units: metres, z up (the Unreal side multiplies by 100). A subject position is the point the
// subject stands on (its feet); the framing picks the height the camera looks at.
//
// Ids only, no text: a shot names a camera spot and one or two subjects by id (lower-case letters,
// digits and _, at most 15 characters), which the screen matches to actors in its map. Fixed
// capacity: a plan holds at most kMaxShots shots, copied by value; nothing allocates after load.
//
// cutscenes.json:
//   { "version": 2,
//     "default": { "blend_in": 0.8, "blend_out": 0.8,
//                  "shots": [ { "spot": "spot_a", "framing": "wide", "duration": 3, "subject": "subj_a" } ] },
//     "cutscenes": [ { "scene": "SC-X", "beat": 4, "step": 0, "blend_in": 1.2, "shots": [ ... ] } ] }
//   `version` is 1 or 2 (1 when absent). Version 2 adds blend_in and blend_out to the default and to
//   each entry: seconds an in-place cutscene spends easing in from the gameplay camera and back out
//   to it (0..5, default 0.8 each; a version 1 file warns about them and keeps the default).
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
    // In place: seconds spent easing in from the gameplay camera, and back out to it.
    static constexpr float kDefaultBlend = 0.8f;
    static constexpr float kMaxBlend = 5.f;

    // No shots, default blend times.
    void clear() {
        count_ = 0;
        blendIn_ = kDefaultBlend;
        blendOut_ = kDefaultBlend;
    }
    // Appends a copy of `shot` with its duration raised to kMinDuration and its blend time kept in
    // 0..duration (0 for a cut). False when the plan is full.
    FTCORE_API bool add(const CutsceneShot& shot);
    // Sets both in-place blend times, each kept in 0..kMaxBlend (NaN: 0).
    FTCORE_API void setBlends(float inSeconds, float outSeconds);

    std::uint32_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    const CutsceneShot& shot(std::uint32_t i) const { return shots_[i]; }
    FTCORE_API float totalDuration() const;
    float blendIn() const { return blendIn_; }
    float blendOut() const { return blendOut_; }

private:
    CutsceneShot shots_[kMaxShots];
    std::uint32_t count_ = 0;
    float blendIn_ = kDefaultBlend;
    float blendOut_ = kDefaultBlend;
};

// Appended only: BlendIn and BlendOut came after Done.
enum class CutscenePhase : std::uint8_t {
    Idle,     // not begun
    Playing,  // a shot is on screen
    Done,     // every shot played (and blended out), or skipped
    BlendIn,  // in place: easing from the gameplay camera into the first shot
    BlendOut  // in place: easing from the camera on screen back to the gameplay camera
};

class CutscenePlayer {
public:
    // A skip press counts only after this much real time, so the press that opened the screen
    // can't skip it.
    static constexpr double kInputGrace = 0.25;
    // With autoplay the cutscene skips itself after this much real time (an alpha run stays fast).
    static constexpr double kAutoPlayRealLimit = 1.0;

    // Starts at shot 0 with a copy of `plan`. An empty plan is Done at once. In place it begins in
    // BlendIn and ends through BlendOut, over the plan's blend times; a zero time skips its phase,
    // and autoplay uses none (an alpha run stays fast).
    FTCORE_API void begin(const CutscenePlan& plan, bool autoPlay = false, bool inPlace = false);
    // Advances the clock by `gameSeconds` through the blend in, the shots (several when it is long)
    // and the blend out. The shot clock holds still while blending in or out.
    FTCORE_API void update(double gameSeconds);
    // Real time for the input grace and the autoplay cap.
    FTCORE_API void tickReal(double realSeconds);
    // Ends now (skipped): Done, or in place BlendOut from wherever the camera got to. False when it
    // had already ended or is blending out.
    FTCORE_API bool skip();
    // A skip press: skips only after kInputGrace of real time. True when it skipped.
    FTCORE_API bool press();

    CutscenePhase phase() const { return phase_; }
    bool playing() const { return phase_ == CutscenePhase::Playing; }
    bool blendingIn() const { return phase_ == CutscenePhase::BlendIn; }
    bool blendingOut() const { return phase_ == CutscenePhase::BlendOut; }
    // Begun and not Done: the cutscene owns the camera.
    bool active() const { return phase_ != CutscenePhase::Idle && phase_ != CutscenePhase::Done; }
    bool finished() const { return phase_ == CutscenePhase::Done; }
    bool skipped() const { return skipped_; }
    bool autoPlay() const { return autoPlay_; }
    bool inPlace() const { return inPlace_; }
    const CutscenePlan& plan() const { return plan_; }
    std::uint32_t shotCount() const { return plan_.size(); }
    // The shot on screen (the first while blending in, the last one once Done); only meaningful when
    // shotCount() > 0.
    std::uint32_t currentIndex() const { return index_; }
    const CutsceneShot& currentShot() const { return plan_.shot(index_); }
    // 0..1 through the current shot (0 while blending in, 1 once Done; held while blending out).
    FTCORE_API float shotProgress() const;
    // Seconds into the current shot.
    double shotTime() const { return shotTime_; }
    // Game seconds played (blends included) and real seconds since begin.
    double elapsed() const { return elapsed_; }
    double realElapsed() const { return realElapsed_; }

    // True while the current shot blends in from the previous one (a Blend shot after the first,
    // within its blend time).
    FTCORE_API bool blending() const;
    // The previous shot (the one being blended from); only meaningful while blending().
    const CutsceneShot& previousShot() const { return plan_.shot(index_ > 0 ? index_ - 1 : 0); }
    // Eased 0..1 weight of the current shot's camera while blending; 1 otherwise.
    FTCORE_API float blendWeight() const;

    // The in-place blend times of this run (0 when not in place, and under autoplay).
    float blendInTime() const { return blendInTime_; }
    float blendOutTime() const { return blendOutTime_; }
    // Eased 0..1 through the blend in: 0 before begin, 1 once past it.
    FTCORE_API float blendInWeight() const;
    // Eased 0..1 through the blend out: 0 before it, 1 once Done.
    FTCORE_API float blendOutWeight() const;
    // How much of the cutscene is on screen, 0..1 (the letterbox): it rises through the blend in, is
    // 1 while the shots play and falls through the blend out; 0 when Idle or Done. A skip while
    // blending in falls from where it got to, over the same share of the blend-out time.
    FTCORE_API float presence() const;

private:
    // Leaves the shots with `from` of the cutscene on screen: BlendOut, or Done when there is
    // nothing to blend out.
    void leave(float from);

    CutscenePlan plan_;
    CutscenePhase phase_ = CutscenePhase::Idle;
    std::uint32_t index_ = 0;
    double shotTime_ = 0.0;
    double phaseTime_ = 0.0; // seconds into BlendIn or BlendOut
    double elapsed_ = 0.0;
    double realElapsed_ = 0.0;
    float blendInTime_ = 0.f;
    float blendOutTime_ = 0.f;
    float outFrom_ = 1.f; // presence when BlendOut began
    float outTime_ = 0.f; // this BlendOut's length
    bool autoPlay_ = false;
    bool inPlace_ = false;
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

// The blockout stage the shot lists are written against: FTBuildCutsceneWorld builds the Cutscene map
// from it, and an in-place cutscene lays it over the map it plays in. Camera spots spot_a..spot_f
// stand where a wide shot stands; subject marks subj_a..subj_c are where the subjects stand (their
// feet, on the floor at z = 0). subj_a is the lead's mark. The stage's front is +x (yaw 0): the spots
// stand in front of the subjects.
struct CutsceneStageMark {
    std::string_view id;
    CutsceneVec3 position;
    bool subject = false; // a subject mark, else a camera spot
};
inline constexpr std::string_view kCutsceneLeadMark = "subj_a";
FTCORE_API std::uint32_t cutsceneStageMarkCount();
// Mark `i`, clamped to the last one.
FTCORE_API const CutsceneStageMark& cutsceneStageMark(std::uint32_t i);
// The mark named `id`, or nullptr. No allocation.
FTCORE_API const CutsceneStageMark* findCutsceneStageMark(std::string_view id);

// Where the stage stands in a map: the stage point `stageAnchor` lands on `worldAnchor`, and the stage
// turns by `turn` degrees about the vertical (Unreal's yaw: 0 is +x, 90 is +y).
struct StageFrame {
    CutsceneVec3 stageAnchor;
    CutsceneVec3 worldAnchor;
    float turn = 0.f;
};
// The frame that puts the lead's mark on `leadFeet` and the stage's front along `leadYaw` (degrees).
FTCORE_API StageFrame anchorForLead(const CutsceneVec3& leadFeet, float leadYaw);
// A stage point in the map. No allocation.
FTCORE_API CutsceneVec3 stageToWorld(const StageFrame& frame, const CutsceneVec3& stagePoint);
// A stage yaw (degrees) in the map, in -180..180.
FTCORE_API float stageYawToWorld(const StageFrame& frame, float stageYaw);

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
