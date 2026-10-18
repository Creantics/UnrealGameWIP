#pragma once

// Face direction: the parts of a VO line's direction that act on the face over time (newengine
// docs/rigging_plan.md §7.2 and §7.4), on top of the emotion map (Emotion.h) and FacePlayer:
//
// - Containment profiles. A character's profile caps the shown emotion weight, adds its own
//   holding-back shapes (a lip press in Devin's), scales blinks and head motion and can cap the
//   intensity (minors at 4). What the cap holds back comes out as micro-expressions instead.
// - Micro-expression flashes: a brief flash of the secondary emotion (or, with none, of the held
//   back primary) in the brows, lids and cheeks. The rate rises with the weight above the cap,
//   and each pause is a slot for one.
// - Emphasis: a 120 ms brow flick and a head-nod envelope at each emphasis word, and no blink
//   starts so that it would land on the word.
// - Pauses: `/` and `//` are preferred blink slots (a `//` forces a blink at least half the time).
// - The listening state: the listener's last emotion decays to rest (half-life 4 s) under 0.3x
//   the speaker's emotion when that emotion mirrors (warmth, sadness; contempt does not). Each
//   speaker phrase may make the listener glance away, and a `//` pause makes a warm listener nod.
// - Delivery (FaceDelivery, passed to speak): pace scales the blink rate (x0.8 slow .. x1.2 fast),
//   the expression blend time (400 .. 150 ms) and the head-motion amplitude. The pitch contour
//   moves the brows (`rising` lifts, `falling` settles into a slight browDown), flutters the lids
//   (`breaking`, at its start and at each pause in it) or adds a lip press (`clipped`). The other
//   contours have no face effect in the spec and do nothing. Volume adds mouthStretch at raised /
//   shout; its jaw aperture scale (whisper 0.5 .. shout 1.3) is only an output, jawScale(),
//   because baked tracks already carry it (bake_face_tracks.py VOLUME_JAW): the owner applies it
//   to unbaked lines (the RMS fallback) only. Contour brows and flutters go to the micro layer;
//   the lip press and mouthStretch go into the line's pose, so they fade with it when listening.
//
// Expression-layer output (the contained pose, the listening pose) goes to the player through
// setExpression; flicks and flashes go to its directed micro layer (Upper and Eyes shapes only,
// which is where composeFace lets micro weights show); blinks go through its blink controls.
// Head nods, head-motion scale and glance-aways are outputs for the owner's head and gaze code.
//
// Tuning values (FaceDirectionTuning, Data/Rig/containment_profiles.json, the "mirror" flags in
// emotion_map.json) are DRAFTS pending the F10 face review.
//
// Determinism: one xorshift32 per director, seeded in the constructor; the same seed, calls and
// dt sequence give the same output. Allocation: speak / listen / speakerPhrase / speakerPause /
// tick never allocate. Only the profile import does.

#include "ftcore/Api.h"
#include "ftcore/Emotion.h"
#include "ftcore/Face.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

struct ContainmentProfile {
    std::string name;
    float cap = 1.f;          // highest emotion weight the face shows (1 = none)
    int maxIntensity = 5;     // intensity is clamped to this first (minors: 4)
    FaceWeights added{};      // containment shapes, times the felt weight 0.2 * intensity
    float blinkRate = 1.f;    // FacePlayer::setBlinkRate
    float headMotion = 1.f;   // head-motion amplitude for the owner (0 = still head)
    float microPerExcess = 2.f; // extra flashes per second per 1.0 of weight above the cap (draft)
};

struct ContainmentProfiles {
    std::vector<ContainmentProfile> profiles;
    FTCORE_API const ContainmentProfile* find(std::string_view name) const;
};

// Loads containment_profiles.json: {"profiles": [{"name", "cap"?, "minor"?: bool, "add"?: {shape:
// weight}, "blink_rate"?, "head_motion"?, "micro_per_excess"?}]}. Names resolve against `set`.
// False (and `*error`) on bad JSON or a bad entry; `out` is then empty. Load time only.
FTCORE_API bool importContainmentProfiles(std::string_view json, const FaceShapeSet& set, ContainmentProfiles& out,
                                          std::string* error = nullptr);

// resolveEmotion with a profile (nullptr = none): intensity clamped to maxIntensity, the cap
// applied, then profile.added * 0.2 * intensity added and the result clamped to [0, 1]. `excess`
// (optional) gets max(0, 0.2 * intensity - cap): what the cap held back. No allocation.
FTCORE_API bool resolveContainedEmotion(const FaceShapeSet& set, const EmotionMap& map, const ContainmentProfile* profile,
                                        std::string_view primary, std::string_view secondary, int intensity,
                                        FacePreset& out, float* excess = nullptr);

// All DRAFT values pending the F10 face review. Times in seconds, rates per second.
struct FaceDirectionTuning {
    // micro-expression flashes
    float microBaseRate = 0.1f;      // with a secondary emotion, before any excess
    float microWeight = 0.6f;        // flash peak, times the flashed pose
    float microAttack = 0.04f, microHold = 0.06f, microRelease = 0.15f;
    float pauseMicroChance = 0.5f;   // a pause fires a flash (when there is a pose to flash)
    // emphasis
    float flickDuration = 0.12f;
    float flickWeight = 0.3f;        // browInnerUp and browOuterUp L/R at the peak
    float nodDuration = 0.25f;       // head-nod envelope at emphasis words and listener nods
    float blinkGuard = 0.2f;         // no blink starts this long before an emphasis word (a blink lasts 0.18)
    // pause blink slots
    float shortPauseBlinkChance = 0.35f; // `/`
    float longPauseBlinkChance = 0.75f;  // `//` (F7 asks for at least 0.5)
    float shortPausePull = 1.f;      // a `/` also blinks when a blink is due within this long
    // listening
    float listenHalfLife = 4.f;
    float mirrorWeight = 0.3f;
    float mirrorFade = 0.4f;         // the mirrored part eases in over this long
    float glanceAwayChance = 0.2f;   // per speaker phrase
    // pace: wpm maps linearly from paceSlowWpm (0) to paceFastWpm (1), clamped, and each output
    // interpolates over that 0..1
    float paceSlowWpm = 80.f, paceFastWpm = 230.f; // the ends of the schema's pace bands
    float paceBlinkSlow = 0.8f, paceBlinkFast = 1.2f;
    float paceBlendSlow = 0.4f, paceBlendFast = 0.15f;
    float paceHeadSlow = 0.7f, paceHeadFast = 1.3f;
    // pitch contour
    float contourLift = 0.25f;       // `rising`: browInnerUp and browOuterUp L/R
    float contourSettle = 0.15f;     // `falling`: browDown L/R
    float contourEase = 0.35f;       // time constant the contour brows ease with
    float contourPress = 0.3f;       // `clipped`: mouthPress L/R, added to the pose
    float flutterWeight = 0.35f;     // `breaking`: eyeBlink L/R at each flutter's peak
    float flutterPeriod = 0.11f;     // one flutter (close and open)
    int flutterMin = 2, flutterMax = 3; // flutters per burst (seeded)
    // volume: mouthStretch L/R added to the pose
    float stretchRaised = 0.15f, stretchShout = 0.3f;
};

enum class FaceVolume : std::uint8_t { Whisper, Soft, Conversational, Raised, Shout };
// The line direction schema's contour vocabulary.
enum class PitchContour : std::uint8_t {
    Flat, Falling, Rising, Arc, Dipping, Breaking, Climbing, Stepped, Trailing, Clipped, Lilting
};

// The delivery fields of a VO line's direction.
struct FaceDelivery {
    static constexpr std::size_t kMaxContours = 4;
    float paceWpm = 0.f;              // pace_wpm, or paceBandWpm of the band; 0 = unknown (mid pace)
    FaceVolume volume = FaceVolume::Conversational;
    std::array<PitchContour, kMaxContours> contours{}; // a `>` chain, in order
    std::uint8_t contourCount = 0;    // 0 = flat
    float duration = 0.f;             // line length; splits a chain into equal parts (0: the first part throughout)
};

// "whisper", "soft", "conversational", "raised", "shout". False for anything else.
FTCORE_API bool parseFaceVolume(std::string_view name, FaceVolume& out);
// One contour word ("falling", ...). False for anything else.
FTCORE_API bool parsePitchContour(std::string_view name, PitchContour& out);
// A `>` chain ("stepped>climbing>falling") into out.contours / contourCount. False (count 0) on an
// unknown word, an empty part or more than kMaxContours parts.
FTCORE_API bool parsePitchContourChain(std::string_view chain, FaceDelivery& out);
// The middle of a pace band ("very slow", "slow", "normal", "quick", "rushed"); 0 for anything else.
FTCORE_API float paceBandWpm(std::string_view band);
// The jaw aperture scale of a volume: whisper 0.5, soft 0.8, conversational 1, raised 1.15, shout 1.3.
FTCORE_API float volumeJawScale(FaceVolume volume);

enum class FaceCueKind : std::uint8_t { Emphasis, Pause, LongPause }; // emphasis word, `/`, `//`

// One aligned event in a line, in seconds from the line start. `duration` is the emphasis word's
// length (pauses ignore it).
struct FaceCue {
    float time = 0.f;
    float duration = 0.f;
    FaceCueKind kind = FaceCueKind::Emphasis;
};

// Directs one character's face through a FacePlayer: speaking lines and listening to others.
class FaceDirector {
public:
    // `set`, `map` and `player` must outlive the director.
    FTCORE_API FaceDirector(const FaceShapeSet& set, const EmotionMap& map, FacePlayer& player,
                            std::uint32_t seed = 0x5eedu, const FaceDirectionTuning& tuning = {});

    // The character's profile (nullptr = none); it must outlive the director. Sets the blink rate.
    FTCORE_API void setProfile(const ContainmentProfile* profile);

    // Starts a line now: the contained pose crossfades in over `fade`, and `cues` (sorted by time;
    // they must outlive the line) play as tick() advances the line clock. False when an emotion
    // name isn't in the map (that part is left out; the line still plays).
    FTCORE_API bool speak(std::string_view primary, std::string_view secondary, int intensity,
                          std::span<const FaceCue> cues = {}, float fade = 0.25f);
    // The same with the line's delivery: the blend time comes from the pace, and the blink rate,
    // head motion, contour and volume follow it until the next speak or listen.
    FTCORE_API bool speak(std::string_view primary, std::string_view secondary, int intensity,
                          const FaceDelivery& delivery, std::span<const FaceCue> cues = {});
    // Starts listening: the current pose decays to rest under the speaker's emotion, mirrored when
    // it mirrors. An empty or unknown speakerPrimary mirrors nothing. Call again on each new line.
    FTCORE_API void listen(std::string_view speakerPrimary, int speakerIntensity);
    // Listener only: the speaker starts a phrase. True when the listener should glance away.
    FTCORE_API bool speakerPhrase();
    // Listener only: the speaker pauses. A `//` makes a warm listener nod (headNod()).
    FTCORE_API void speakerPause(FaceCueKind kind);

    // Advances by dt (feed it the same dt as the player, before player.update).
    FTCORE_API void tick(float dt);

    bool listening() const { return listening_; }
    float lineTime() const { return lineTime_; }
    float excess() const { return excess_; }
    float microRate() const { return microRate_; } // flashes per second while speaking
    const FaceWeights& micro() const { return micro_; } // the directed micro layer last tick
    float headNod() const { return nod_; }         // 0..1 envelope times headMotion(); the owner maps it to neck_02
    FTCORE_API float headMotion() const;           // the profile's head-motion scale, times the pace's while speaking
    const FacePreset& pose() const { return pose_; } // the expression pose given to the player
    // Delivery outputs (neutral while listening): the pace's 0..1 position, blink factor and blend
    // time, and the volume's jaw aperture scale for unbaked lines (see the top of this file).
    float pace() const { return pace_; }
    FTCORE_API float paceBlinkScale() const;
    FTCORE_API float blendTime() const;
    float jawScale() const { return jawScale_; }
    PitchContour contour() const { return contour_; } // the chain part playing now (Flat when none)

private:
    float nextRandom();
    bool roll(float chance) { return nextRandom() < chance; }
    bool speakLine(std::string_view primary, std::string_view secondary, int intensity, const FaceDelivery& delivery,
                   std::span<const FaceCue> cues, float fade);
    void scheduleFlash();
    void startFlash();
    void startFlutter(float late);
    void runCues(float now);
    void updateContour(float dt);
    float paceLerp(float slow, float fast) const { return slow + (fast - slow) * pace_; }

    const FaceShapeSet* set_;
    const EmotionMap* map_;
    FacePlayer* player_;
    FaceDirectionTuning tuning_;
    const ContainmentProfile* profile_ = nullptr;
    std::uint32_t rng_;
    int browInner_ = -1, browOuterLeft_ = -1, browOuterRight_ = -1, browDownLeft_ = -1, browDownRight_ = -1;
    int pressLeft_ = -1, pressRight_ = -1, stretchLeft_ = -1, stretchRight_ = -1;

    FaceDelivery delivery_;
    float pace_ = 0.5f, jawScale_ = 1.f;
    bool paced_ = false; // a line with a known pace is playing (cleared by listen)
    PitchContour contour_ = PitchContour::Flat;
    float lift_ = 0.f, settle_ = 0.f; // eased contour brows
    float flutterTime_ = -1.f;
    int flutterCount_ = 0;

    FacePreset pose_;
    FaceWeights flashPose_{}, micro_{};
    bool hasFlashPose_ = false;
    float excess_ = 0.f, microRate_ = 0.f;
    float nextFlash_ = 0.f, flashTime_ = -1.f;
    float flickTime_ = -1.f, nodTime_ = -1.f, nod_ = 0.f;

    std::span<const FaceCue> cues_;
    std::size_t cueNext_ = 0, guardNext_ = 0;
    float lineTime_ = 0.f;

    bool listening_ = false, listenerWarm_ = false;
    const EmotionDef* lastPrimary_ = nullptr;
    FaceWeights listenFrom_{}, mirrorFrom_{}, mirror_{};
    float decay_ = 1.f, mirrorEase_ = 0.f;
};

} // namespace ftcore
