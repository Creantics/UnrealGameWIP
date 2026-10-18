#pragma once

#include "ftcore/Api.h"
#include "ftcore/CombatTypes.h"
#include "ftcore/ComboGenerator.h"
#include "ftcore/ComboSequence.h"
#include "ftcore/TurnQueue.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

// Optional music sync. All times stay in seconds on one timeline (the song time that
// Conductor produces). With bpm > 0 a combo's first note waits for the next beat (or bar)
// after leadIn/windup has elapsed, and every note sits on the beat grid.
struct MusicConfig {
    double bpm = 0.0;        // <= 0: music off, behaviour is the plain seconds-based one
    double offset = 0.0;     // seconds of the song's beat 0
    bool startOnBar = false; // align the first note to a bar line instead of any beat
    int beatsPerBar = 4;

    bool enabled() const { return bpm > 0.0; }
};

struct BattleTuning {
    double leadIn = 1.0;        // time between choosing a skill and its first note
    double windup = 1.2;        // enemy telegraph before the first note
    double recovery = 0.7;      // pause after a skill resolves before the next turn
    double lockout = 0.25;      // input ignored this long after a stray press
    JudgeWindows windows;       // base windows, scaled by combo difficulty
    GradeWeights weights;
    DifficultyTuning difficulty;

    float fullComboBonus = 0.25f;  // attack: extra damage (fraction of full) if no note was missed
    float counterAccuracy = 0.9f;  // defense: block at least this much with no misses to counter
    float counterPower = 1.0f;
    int apOnCounter = 1;
    int startingAp = 2;
    std::uint64_t seed = 0x5EEDC0DEull; // combo charts are generated deterministically from this
    MusicConfig music;                  // beat-synced charts when music.bpm > 0
};

enum class Phase {
    NotStarted,
    AwaitingCommand, // a party member is choosing what to do
    PartySkill,      // party member performs an attack combo
    EnemySkill,      // enemy windup, then the defender performs a block combo
    Recovery,
    Victory,
    Defeat,
};

enum class EventType {
    TurnStarted,
    SkillStarted,
    NoteJudged,    // one combo note resolved: grade, beat = note index, label = "WRONG" for a wrong arrow
    Hit,           // damage dealt (amount); grade/beat of the note that produced it
    Blocked,       // defense: damage blocked (amount) by a note
    ComboFinished, // amount = accuracy in percent; label = "FULL COMBO" when no misses
    Counter,       // defender blocked nearly everything and strikes back
    Downed,
    Victory,
    Defeat,
};

struct BattleEvent {
    EventType type;
    CombatantId source = 0;
    CombatantId target = 0;
    int amount = 0;
    Grade grade = Grade::Pending;
    std::size_t beat = 0;
    double time = 0.0;
    // A skill name, "WRONG", "FULL COMBO" or empty. Points into the battle's own data (or a
    // literal), so it is valid while the Battle lives and its skills are not edited.
    std::string_view label;
};

enum class CommandError { None, WrongPhase, NoSuchSkill, NotEnoughAp, InvalidTarget };

class Battle {
public:
    FTCORE_API Battle(std::vector<Combatant> party, std::vector<Combatant> enemies, BattleTuning tuning = {});

    // Starts over with a new line-up (phase NotStarted again, no events, same rules as the
    // constructor). Reuses the storage of the previous battle, so resetting to a line-up of the same
    // shape (same names, skill lists and tuning limits) does not allocate.
    FTCORE_API void reset(const std::vector<Combatant>& party, const std::vector<Combatant>& enemies, const BattleTuning& tuning);

    FTCORE_API void start(double now);
    FTCORE_API void update(double now);

    // Party input. Timestamps should come from input events, not frame time.
    FTCORE_API CommandError command(std::size_t skillIndex, CombatantId target, double now);
    FTCORE_API void press(Arrow arrow, double timestamp);

    // The combo the player would face for a skill, without starting it (for UI previews).
    // `opponent` is the enemy: the target for party skills, the attacker for enemy skills.
    FTCORE_API ComboParams previewCombo(CombatantId performer, CombatantId opponent, const Skill& skill) const;

    Phase phase() const { return phase_; }
    CombatantId actor() const { return actor_; }
    const std::vector<Combatant>& combatants() const { return combatants_; }
    const Combatant& combatant(CombatantId id) const { return combatants_[id]; }
    FTCORE_API std::vector<CombatantId> living(Team team) const;
    // Allocation-free forms for per-frame code: fill a reused buffer / just count.
    FTCORE_API void living(Team team, std::vector<CombatantId>& out) const; // clears `out` first
    FTCORE_API std::size_t livingCount(Team team) const;
    std::vector<CombatantId> turnOrder(std::size_t count) const { return queue_.preview(count); }
    // Upcoming actors into `out` (out.size() entries); returns how many were written. No allocation.
    std::size_t turnOrder(std::span<CombatantId> out) const { return queue_.preview(out); }

    // Active skill (valid during PartySkill / EnemySkill).
    const ComboSequence& combo() const { return combo_; }
    const ComboParams& comboParams() const { return params_; }
    FTCORE_API const Skill* activeSkill() const;
    const std::vector<CombatantId>& activeTargets() const { return targets_; }
    double phaseStart() const { return phaseStart_; }

    const BattleTuning& tuning() const { return tuning_; }

    // Events since the last drain. drainEvents() hands them over (one vector per call, so it
    // allocates); events() + clearEvents() read them in place and never allocate.
    FTCORE_API std::vector<BattleEvent> drainEvents();
    std::span<const BattleEvent> events() const { return events_; }
    void clearEvents() { events_.clear(); }

private:
    struct Share {
        CombatantId target;
        float full = 0.0f; // damage at 100% (unrounded)
        int applied = 0;   // dealt (attack) or blocked (defense) so far
        int taken = 0;     // defense only: damage let through so far
    };

    void beginTurn(double now);
    void startEnemyTurn(double now);
    void startCombo(const Skill& skill, CombatantId performer, CombatantId opponent, double firstNote, double now);
    void resolve(double now); // judges results_ (filled by the combo)
    void resolveAttackNote(const NoteResult& r, double now);
    void resolveDefenseNote(const NoteResult& r, double now);
    void finishSkill(double now);
    bool targetsGone() const;
    void dealDamage(CombatantId source, CombatantId target, int amount, Grade grade, std::size_t beat, double now);
    bool checkEnd(double now);
    float damage(const Combatant& attacker, const Combatant& defender, float power) const;
    void emit(BattleEvent e) { events_.push_back(std::move(e)); }

    std::vector<Combatant> combatants_;
    BattleTuning tuning_;
    TurnQueue queue_;
    Phase phase_ = Phase::NotStarted;
    CombatantId actor_ = 0;
    double phaseStart_ = 0.0;
    double recoveryEnd_ = 0.0;

    std::optional<std::size_t> skillIndex_;
    std::vector<CombatantId> targets_;
    ComboSequence combo_;
    ComboParams params_;
    std::vector<Share> shares_;
    std::uint64_t combosPlayed_ = 0;
    std::vector<int> enemyTurnCount_;
    std::vector<BattleEvent> events_;
    std::vector<NoteResult> results_;   // scratch, reused by press()/update()
    std::vector<ComboNote> noteScratch_; // scratch for chart generation, swapped with combo_'s notes
};

} // namespace ftcore
