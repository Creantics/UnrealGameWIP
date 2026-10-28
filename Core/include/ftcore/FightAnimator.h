#pragma once

// Which fight clips a combatant plays, when and how fast, so that a strike lands on its note and a
// dodge on the hit it answers.
//
//   ftcore::FightAnimator a;
//   a.setClip(FightClip::StrikeLeft, {0.9f, 0.42f, false});   // length, key time, loop: once per clip
//   a.play(FightClip::StrikeLeft, now, noteTime);             // its key frame plays at noteTime
//   a.update(now);                                            // every frame
//   for (i < a.layerCount()) pose += sample(a.layer(i).clip, a.layer(i).time) * a.layer(i).weight;
//
// Every clip has a key time: the frame that must meet the beat (a strike's impact, a dodge's
// furthest point, a hit reaction's contact). play() with a key time starts the clip early enough to
// reach that frame at its own pace, later if there is time to spare; with too little time it plays
// faster (up to maxRate), and past that it joins the clip part way through, so the key frame still
// falls on the beat. A chained clip never starts before the key frame of the one queued before it,
// so a run of quick strikes each lands before the next winds up.
//
// Actions blend newest over oldest: each fades in over fadeIn and a one-shot fades out over fadeOut
// at its end; what they leave goes to the Stance loop. Downed and Victory hold their last frame;
// looping clips (Stance, Aim) play until something newer covers them. Fixed size: nothing allocates.
//
// Each combatant has a strike pattern: the punches, kicks, weapon swings and casts its combo notes
// and attack hits cycle through, a punch each side in turn by default (a fighter at home with kicks
// gets kicks in it, one with a blade swings it). A kick or swing the body has no clip for is thrown as
// the punch on its side; a cast without its clip, as the right punch.
//
// FightDirector runs one animator per combatant from a Battle's events: the party's combo strikes
// keyed to their notes (by the striker's pattern, the last one the finisher), an enemy's strikes keyed to
// its attack's impacts, dodges, parries and jumps keyed to the hits they answer, hit reactions,
// counters, phase changes, the downed, the victors, the aim and the aimed shot.

#include "ftcore/Api.h"
#include "ftcore/DefenceTimeline.h"
#include "ftcore/TurnQueue.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ftcore {

class Battle;

enum class FightClip : std::uint8_t {
    Stance,      // the loop between actions
    StrikeLeft,  // party: combo strikes alternate sides; enemies: the tell and strike of each attack hit
    StrikeRight,
    Finisher,    // party: the combo's last note
    Dodge,
    Parry,
    Jump,
    HitReact,
    Counter,     // party: the strike back after a fully parried attack
    Downed,      // holds its last frame
    Victory,     // holds its last frame
    Aim,         // party: loops while aiming
    PhaseChange, // enemies: entering a new phase of the fight
    KickLeft,    // in a combatant's strike pattern, in place of a punch
    KickRight,
    WeaponLeft,  // in a strike pattern: a held weapon's swing from each side
    WeaponRight,
    Cast,        // in a strike pattern: a spell thrown from the hands
    Shot,        // party: the aimed shot firing, its key on the shot
    Count
};

// "stance", "strike left", ... A literal: valid forever.
FTCORE_API const char* fightClipName(FightClip clip);

struct FightClipInfo {
    float length = 0.f; // seconds; 0: the body has no such clip
    float key = 0.f;    // seconds into the clip of the frame that meets the beat
    bool loop = false;
};

struct FightTuning {
    float fadeIn = 0.1f;
    float fadeOut = 0.15f;
    float maxRate = 1.6f; // fastest a clip plays to reach its key frame in time
};

struct FightLayer {
    FightClip clip = FightClip::Stance;
    float time = 0.f; // seconds into the clip
    float weight = 0.f;
};

class FightAnimator {
public:
    static constexpr std::size_t kMaxActions = 8;
    static constexpr std::size_t kMaxPattern = 8;

    void setClip(FightClip c, FightClipInfo info) { clips_[index(c)] = info; }
    // The strike pattern (see the top of this file): 1 to kMaxPattern of StrikeLeft, StrikeRight,
    // KickLeft, KickRight, WeaponLeft, WeaponRight and Cast. Returns false, keeping the pattern it
    // had, for anything else.
    FTCORE_API bool setPattern(const FightClip* strikes, std::size_t count);
    // The strike for the i-th note or hit of a combo or attack.
    FightClip patternStrike(std::size_t i) const { return pattern_[i % patternCount_]; }
    std::size_t patternSize() const { return patternCount_; }
    const FightClipInfo& clip(FightClip c) const { return clips_[index(c)]; }
    bool has(FightClip c) const { return clips_[index(c)].length > 0.f; }
    FightTuning& tuning() { return tuning_; }
    const FightTuning& tuning() const { return tuning_; }

    // Back to the Stance loop alone (the clips, pattern and tuning stay).
    FTCORE_API void reset();

    // Plays `clip` from `now`, or so that its key frame plays at `keyAt` when keyAt >= 0 (see the top
    // of this file). Returns false, playing nothing, when the body has no such clip.
    FTCORE_API bool play(FightClip c, double now, double keyAt = -1.0);

    // Moves to `now` (non-decreasing) and recomputes the layers.
    FTCORE_API void update(double now);

    std::size_t layerCount() const { return layerCount_; }
    const FightLayer& layer(std::size_t i) const { return layers_[i]; }
    // The newest action under way at the last update, Stance when none.
    FightClip current() const { return current_; }
    // Actions queued or playing (Stance aside).
    std::size_t actionCount() const { return count_; }
    // When the key frame of the action queued last plays (-1 when none is queued).
    FTCORE_API double lastKeyTime() const;

private:
    struct Action {
        FightClip clip = FightClip::Stance;
        double start = 0.0; // when the clip starts playing at `offset`
        float rate = 1.f;
        float offset = 0.f;
    };

    static std::size_t index(FightClip c) { return static_cast<std::size_t>(c); }
    bool holds(FightClip c) const { return c == FightClip::Downed || c == FightClip::Victory; }
    double keyTime(const Action& a) const;
    double endTime(const Action& a) const;

    std::array<FightClipInfo, static_cast<std::size_t>(FightClip::Count)> clips_{};
    FightTuning tuning_;
    std::array<FightClip, kMaxPattern> pattern_{FightClip::StrikeLeft, FightClip::StrikeRight};
    std::size_t patternCount_ = 2;
    std::array<Action, kMaxActions> actions_{}; // by start time, oldest first
    std::size_t count_ = 0;
    std::size_t queuedLast_ = kMaxActions;      // index of the action play() added last
    std::array<FightLayer, kMaxActions + 1> layers_{};
    std::size_t layerCount_ = 0;
    FightClip current_ = FightClip::Stance;
};

// One animator per combatant, driven by a Battle.
//
//   director.bind(battle);                                   // once per battle (allocates)
//   director.animator(id).setClip(...);                      // each combatant's clips
//   each frame: battle.command/defend/update(...); director.update(battle, now); battle.clearEvents();
//   on a defence press: battle.defend(input, t); director.pressed(input, t);
class FightDirector {
public:
    FTCORE_API void bind(const Battle& battle);

    std::size_t size() const { return animators_.size(); }
    FightAnimator& animator(CombatantId id) { return animators_[id]; }
    const FightAnimator& animator(CombatantId id) const { return animators_[id]; }

    // Strikes are queued this far ahead of their notes and impacts (at least the longest key time).
    double lookahead() const { return lookahead_; }
    void setLookahead(double seconds) { lookahead_ = seconds; }

    // The defender pressed `input` (as given to Battle::defend): a PERFECT outcome is then shown as
    // the perfect dodge or the parry it was. Without it a perfect outcome shows as a parry.
    void pressed(DefenceInput input, double /*t*/) { lastInput_ = input; }

    // Reads the battle's events since its last clearEvents() and its combo or attack, queues the
    // clips they call for and updates every animator to `now`. Allocation-free.
    FTCORE_API void update(const Battle& battle, double now);

private:
    void queueStrikes(const Battle& battle, double now);
    bool playOr(CombatantId id, FightClip clip, FightClip fallback, double now, double keyAt = -1.0);
    // The i-th strike of id's pattern, or the punch on its side, or StrikeLeft.
    bool playStrike(CombatantId id, std::size_t i, double now, double keyAt);
    double afterKey(CombatantId id, double now) const;

    std::vector<FightAnimator> animators_;
    double lookahead_ = 1.0;
    DefenceInput lastInput_ = DefenceInput::Parry;
    // The combo or enemy attack under way: who strikes and the next note or hit to queue.
    CombatantId striker_ = 0;
    bool partyStrikes_ = false;
    bool striking_ = false;
    std::size_t nextStrike_ = 0;
    CombatantId aimer_ = 0;
    bool aiming_ = false;
    // A Counter is followed at once by its Hit: the reaction is queued with the counter instead.
    bool skipCounterHit_ = false;
    CombatantId counterSource_ = 0;
    CombatantId counterTarget_ = 0;
};

} // namespace ftcore
