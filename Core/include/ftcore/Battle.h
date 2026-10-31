#pragma once

#include "ftcore/Api.h"
#include "ftcore/CombatTypes.h"
#include "ftcore/ComboGenerator.h"
#include "ftcore/ComboSequence.h"
#include "ftcore/DefenceTimeline.h"
#include "ftcore/Inventory.h"
#include "ftcore/TurnQueue.h"
#include "ftcore/WeakPoint.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

// Optional music sync. All times stay in seconds on one timeline (the song time that
// Conductor produces). With bpm > 0 a party combo's first note waits for the next beat (or bar)
// after leadIn has elapsed, and every note sits on the beat grid. Enemy attack timelines are not
// snapped: their hits sit at fixed offsets from the banner.
struct MusicConfig {
    double bpm = 0.0;        // <= 0: music off, behaviour is the plain seconds-based one
    double offset = 0.0;     // seconds of the song's beat 0
    bool startOnBar = false; // align the first note to a bar line instead of any beat
    int beatsPerBar = 4;

    bool enabled() const { return bpm > 0.0; }
};

struct BattleTuning {
    double leadIn = 1.0;        // time between choosing a skill and its first note
    double recovery = 0.7;      // pause after a skill resolves before the next turn
    double lockout = 0.25;      // input ignored this long after a stray press
    JudgeWindows windows;       // base windows, scaled by combo difficulty
    GradeWeights weights;
    DifficultyTuning difficulty;

    float fullComboBonus = 0.25f;  // attack: extra damage (fraction of full) if no note was missed
    DefenceWindows defence;        // enemy turns: dodge / parry / jump windows and lockouts
    int apOnPerfect = 1;           // defence: AP refunded to the defender per perfect parry or dodge
    float counterPower = 1.0f;     // defence: a fully parried attack is countered at this power
    int apOnCounter = 1;
    int counterPoise = 10;         // defence: a counter also costs the attacker this much poise
    float guardShare = 0.5f;       // Guard: the share of a held-guarded hit that still lands
    float releasePower = 0.5f;     // Release: HP damage to each enemy at 100% accuracy
    float releasePoise = 1.0f;     // Release: poise lost by each enemy, per Load spent, at 100% accuracy
    float releaseDifficulty = 2.0f; // Release: the combo's move complexity (0-10)
    int startingAp = 2;
    std::uint64_t seed = 0x5EEDC0DEull; // combo charts are generated deterministically from this
    MusicConfig music;                  // beat-synced charts when music.bpm > 0
    // Enemy turns with music on: the hits land on the song's beats (snapToBeat), and this many
    // CountIn cues come on the beats before the first one (0: none).
    int countIn = 2;
    AimTuning aim;                      // the Aim command: cost, power, time limit, sway (WeakPoint.h)
};

enum class Phase {
    NotStarted,
    AwaitingCommand, // a party member is choosing what to do
    PartySkill,      // party member performs an attack combo
    EnemySkill,      // enemy attack timeline: the defender dodges, parries or jumps each hit
    Recovery,
    Victory,
    Defeat,
    PartyAim, // a party member aims at an enemy's weak points (appended: the Unreal mirror keeps its order)
};

enum class EventType {
    TurnStarted,
    SkillStarted,
    NoteJudged,    // one combo note resolved: grade, beat = note index, label = "WRONG" for a wrong arrow
    Hit,           // damage dealt (amount); grade/beat of the note (or attack hit) that produced it. An aimed
                   // shot: label "WEAK POINT" (beat = the weak point), "HIT" or "GRAZE" (ShotKind)
    Defended,      // enemy turn: attack hit `beat` resolved; label "PERFECT" (a perfect parry or dodge),
                   // "DODGE", "JUMP" or "HIT"; amount = AP refunded; grade Perfect on a refund.
                   // Also "LATE" (grade Miss, nothing else changes) after a "HIT" when a press came just
                   // after that hit's impact (DefenceWindows::late), even in the Recovery after the attack
    ComboFinished, // party combo: amount = accuracy in percent; label = "FULL COMBO" when no misses
    Counter,       // the defender parried every hit and strikes back
    Downed,
    Victory,
    Defeat,
    PhaseChanged, // an enemy (source and target) entered phase `beat` of its EnemyPhases; label = the phase's
                  // label (may be empty); amount = HP it healed (appended: the Unreal mirror keeps its order)
    ItemUsed,     // an item took effect on `target`, one event per recipient: source = the member who used it,
                  // amount = the HP or AP restored (0 when the target was already full), label = the item's id
                  // (appended: the Unreal mirror keeps its order)
    CountIn,      // enemy turn with music on: a count beat before the attack's first hit; beat = the beats left
                  // to that hit (countIn down to 1), time = the count beat, source = the enemy, target = the
                  // defender (appended)
    PoiseLost,    // `target` lost `amount` poise to `source` (appended)
    Staggered,    // `target`'s poise reached 0: it skips its next turn; source = who broke it (appended)
    Steadied,     // `source`'s skipped turn: its poise refills (amount = poise restored), then the next turn
                  // comes after the recovery pause (appended)
    LoadStored,   // a perfect Guard: `target` stored `amount` Load from hit `beat`; source = the enemy (appended)
    Overloaded,   // `target`'s Load reached kMaxLoad: only Release is open to it (appended)
    Released,     // `source` let go of `amount` Load: its Release combo finished (the poise losses follow)
                  // (appended)
};

// Running totals over one battle (from start() to the outcome), for the results screen.
struct BattleStats {
    int damageDealt = 0;     // by the party, counters and aimed shots included
    int damageTaken = 0;     // by the party
    int notes = 0;           // party combo notes judged
    int perfectNotes = 0;
    int missedNotes = 0;     // misses and wrong arrows
    int fullCombos = 0;
    int hitsDefended = 0;    // enemy hits dodged, jumped or parried (perfect ones included)
    int perfectDefences = 0; // perfect parries and dodges
    int hitsTaken = 0;       // enemy hits that landed
    int counters = 0;
    int weakPointHits = 0;   // aimed shots on a weak point
    int partyTurns = 0;
    int enemyTurns = 0;
    int phaseChanges = 0;
    double startTime = 0.0;
    double endTime = 0.0;    // the time of the outcome (0 until then)
    int itemsUsed = 0;       // items used (an item for the whole party counts once)
    int staggers = 0;        // enemies staggered
    int loadStored = 0;      // Load stored by perfect Guards
    int releases = 0;
};

struct BattleEvent {
    EventType type;
    CombatantId source = 0;
    CombatantId target = 0;
    int amount = 0;
    Grade grade = Grade::Pending;
    std::size_t beat = 0;
    double time = 0.0;
    // A skill name, "WRONG", "FULL COMBO", a defence outcome or empty. Points into the battle's own data (or a
    // literal), so it is valid while the Battle lives and its skills are not edited.
    std::string_view label;
};

// NoSuchItem (appended: the Unreal mirror does not have it yet): useItem's slot is empty or past the end, or
// holds an id the item table does not know. Overloaded (appended): the member's Load is full, so only
// Release is open. NoLoad (appended): a Release with no Load. NoSuchVerb (appended): the member lacks
// the verb.
enum class CommandError { None, WrongPhase, NoSuchSkill, NotEnoughAp, InvalidTarget, NoSuchItem, Overloaded, NoLoad, NoSuchVerb };

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
    // An arrow during a party combo (ignored otherwise).
    FTCORE_API void press(Arrow arrow, double timestamp);
    // A dodge / parry / jump during an enemy attack (ignored otherwise). Also taken in the Recovery
    // right after an enemy attack, where it can only be judged late for the attack's last hit.
    // Guard starts a hold (only for a defender with Verb::Guard; ignored otherwise) that releaseGuard
    // ends: a perfect Guard takes nothing and stores the hit's damage as the defender's Load, a held one
    // lets tuning().guardShare of it land. Its result is a Defended event with label "GUARD" or
    // "PERFECT GUARD" (then LoadStored, and Overloaded when the Load fills).
    FTCORE_API void defend(DefenceInput input, double timestamp);
    FTCORE_API void releaseGuard(double timestamp);

    // Release (Verb::Release): from AwaitingCommand, the acting member lets go of all its Load in an
    // arrow combo of exactly load.releaseNotes() notes against every living enemy. Each takes
    // tuning().releasePower HP damage and loses round(Load x accuracy x releasePoise) poise (a stagger
    // at 0). The Load is spent at once; Released comes when the combo ends. NoSuchVerb / NoLoad /
    // WrongPhase. An overloaded member (Load at kMaxLoad) can only Release: command, aim and useItem
    // return Overloaded.
    FTCORE_API CommandError release(double now);
    // The Release in progress (PartySkill).
    bool releasing() const { return releasing_; }

    // Aim (see WeakPoint.h and docs/BATTLE_AIM.md). From AwaitingCommand: the acting party member
    // spends tuning().aim.apCost and aims at `target` (a living enemy); phase PartyAim. Emits
    // SkillStarted with label "AIM".
    FTCORE_API CommandError aim(CombatantId target, double now);
    // Moves / places the reticle (figure units, clamped). Ignored outside PartyAim.
    FTCORE_API void moveReticle(float dx, float dy);
    FTCORE_API void setReticle(float x, float y);
    // Fires at the aim point of `timestamp`: one Hit event, then Recovery (or the outcome). update()
    // fires on its own at the deadline. False outside PartyAim.
    FTCORE_API bool fire(double timestamp);
    // Leaves aim before the shot: the AP comes back and the same member chooses again.
    FTCORE_API bool cancelAim(double now);
    // The aim in progress (valid during PartyAim) and the last shot fired.
    const AimShot& aimShot() const { return aim_; }
    const ShotResult& lastShot() const { return lastShot_; }
    // The enemy being aimed at (valid during PartyAim).
    CombatantId aimTarget() const { return targets_.empty() ? actor_ : targets_.front(); }

    // Items (Inventory.h, docs/BATTLE_DEMO.md "Items"). Before start() (it asserts the battle has not
    // started): what the items do and what the party carries. The battle keeps its own copy (fixed-size,
    // so no allocation) and reset() empties it: load them again after a reset.
    FTCORE_API void setItems(const ItemTable& table, const Inventory& carried);
    // From AwaitingCommand: the acting party member uses the item in slot `slot` of carriedItems() on
    // `target` and the turn is spent (no AP): the item takes effect, then Recovery, then the next turn, as
    // after a skill. One ItemUsed event per recipient. A heal or an AP restore needs a living ally, a revive
    // a downed one (it rejoins the turn queue); an item for the whole party ignores `target` and reaches
    // every member it can (InvalidTarget when there is none). NoSuchItem: the slot is past the end, or its
    // id is not in the item table (the stack stays carried). A refused use changes nothing. No allocation.
    FTCORE_API CommandError useItem(std::size_t slot, CombatantId target, double now);
    // What the items do, and what is still carried (one stack per slot; a used-up stack is gone and the
    // later ones close up). After the fight the game writes carriedItems() back to PartyProgress::items.
    const ItemTable& itemTable() const { return items_; }
    const Inventory& carriedItems() const { return carried_; }

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

    // The party combo (valid during PartySkill; empty during an enemy turn).
    const ComboSequence& combo() const { return combo_; }
    // The enemy attack being defended (valid during EnemySkill).
    const DefenceTimeline& defence() const { return defence_; }
    // The defender of the current enemy attack (valid during EnemySkill).
    CombatantId defender() const { return targets_.empty() ? actor_ : targets_.front(); }
    const ComboParams& comboParams() const { return params_; }
    // Active skill (valid during PartySkill / EnemySkill).
    FTCORE_API const Skill* activeSkill() const;
    const std::vector<CombatantId>& activeTargets() const { return targets_; }
    double phaseStart() const { return phaseStart_; }

    const BattleTuning& tuning() const { return tuning_; }

    // Enemy phases (CombatTypes.h EnemyPhase). The phase an enemy is in (0 for an enemy without
    // phases), and the index of the skill it will use on its next turn (its phase pattern, else the
    // next skill in order), so the UI can show what is coming.
    std::size_t enemyPhase(CombatantId id) const { return id < enemyPhase_.size() ? enemyPhase_[id] : 0; }
    FTCORE_API std::size_t nextEnemySkill(CombatantId id) const;

    // Running totals for the results screen (complete once the outcome came).
    const BattleStats& stats() const { return stats_; }

    // Events since the last drain. drainEvents() hands them over (one vector per call, so it
    // allocates); events() + clearEvents() read them in place and never allocate.
    FTCORE_API std::vector<BattleEvent> drainEvents();
    std::span<const BattleEvent> events() const { return events_; }
    void clearEvents() { events_.clear(); }

private:
    struct Share {
        CombatantId target;
        float full = 0.0f; // damage at 100% (unrounded)
        int applied = 0;   // attack: dealt so far
        int taken = 0;     // defence: damage let through so far
    };

    void beginTurn(double now);
    void startEnemyTurn(double now);
    // exactSteps > 0: the chart has exactly that many notes (no jumps), whatever the difficulty.
    void startCombo(const Skill& skill, CombatantId performer, CombatantId opponent, double firstNote, double now,
                    int exactSteps = 0);
    void storeLoad(CombatantId member, int amount, std::size_t hit, double now);
    void resolve(double now); // judges results_ (filled by the combo)
    void resolveAttackNote(const NoteResult& r, double now);
    void resolveDefence(const DefenceResults& results, double now);
    void emitCountIn(double now);
    void finishSkill(double now);
    bool targetsGone() const;
    void dealDamage(CombatantId source, CombatantId target, int amount, Grade grade, std::size_t beat, double now,
                    std::string_view label = {});
    bool checkEnd(double now);
    float damage(const Combatant& attacker, const Combatant& defender, float power) const;
    void checkPhase(CombatantId enemy, double now);
    void losePoise(CombatantId source, CombatantId target, int amount, double now);
    void tally(const BattleEvent& e);
    void emit(BattleEvent e) {
        tally(e);
        events_.push_back(std::move(e));
    }

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
    DefenceTimeline defence_;
    AimShot aim_;
    ShotResult lastShot_;
    float landedWeight_ = 0.0f; // defence: summed weight of the hits that landed
    Skill release_;             // the Release's move (its notes set per use); activeSkill() while releasing
    bool releasing_ = false;
    int releaseLoad_ = 0;       // the Load the Release in progress spends
    double countHit_ = 0.0;     // enemy turn: the first hit's impact, which the count leads to
    int countLeft_ = 0;         // CountIn cues still to come
    std::uint64_t combosPlayed_ = 0;
    std::vector<int> enemyTurnCount_;      // turns taken in the current phase (the pattern cursor)
    std::vector<std::uint8_t> enemyPhase_; // per combatant: its current phase
    std::vector<int> baseAttack_;          // per combatant: attack at battle start
    BattleStats stats_;
    ItemTable items_;   // set by setItems(); event labels of ItemUsed point into it
    Inventory carried_; // the party's items, down by one for each use
    std::vector<BattleEvent> events_;
    std::vector<NoteResult> results_;   // scratch, reused by press()/update()
    std::vector<ComboNote> noteScratch_; // scratch for chart generation, swapped with combo_'s notes
};

} // namespace ftcore
