#include "ftcore/Battle.h"

#include "ftcore/Assert.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

Battle::Battle(std::vector<Combatant> party, std::vector<Combatant> enemies, BattleTuning tuning) {
    reset(party, enemies, tuning);
}

void Battle::reset(const std::vector<Combatant>& party, const std::vector<Combatant>& enemies,
                   const BattleTuning& tuning) {
    tuning_ = tuning;
    const std::size_t count = party.size() + enemies.size();
    // Assign in place: strings and skill lists reuse their buffers, so a same-shaped line-up is free.
    if (combatants_.size() > count) combatants_.resize(count);
    combatants_.reserve(count);
    std::size_t at = 0;
    auto put = [&](const Combatant& c) -> Combatant& {
        if (at < combatants_.size()) combatants_[at] = c;
        else combatants_.push_back(c);
        return combatants_[at++];
    };
    for (const Combatant& c : party) {
        Combatant& d = put(c);
        d.team = Team::Party;
        d.ap = std::min(tuning_.startingAp, d.maxAp);
    }
    for (const Combatant& c : enemies) {
        Combatant& d = put(c);
        d.team = Team::Enemies;
        FTCORE_ASSERT(!d.skills.empty());
    }
    enemyTurnCount_.assign(combatants_.size(), 0);

    queue_.clear();
    phase_ = Phase::NotStarted;
    actor_ = 0;
    phaseStart_ = 0.0;
    recoveryEnd_ = 0.0;
    skillIndex_.reset();
    targets_.clear();
    shares_.clear();
    params_ = {};
    combosPlayed_ = 0;
    defence_.clear();
    landedWeight_ = 0.0f;
    events_.clear();
    results_.clear();

    // Everything the per-frame path touches is sized up front so a running battle never allocates.
    const std::size_t maxNotes = static_cast<std::size_t>(std::max(tuning_.difficulty.maxNotes, 1)) * 2;
    targets_.reserve(combatants_.size());
    shares_.reserve(combatants_.size());
    events_.reserve(256);
    results_.reserve(maxNotes + 2);
    noteScratch_.reserve(maxNotes);
    combo_.reserve(maxNotes);
    noteScratch_.clear();
    if (combo_.size() > 0) // a previous battle's chart: start with an empty one
        combo_.reset(noteScratch_, scaled(tuning_.windows, 1.0), tuning_.lockout, tuning_.weights);
}

void Battle::start(double now) {
    FTCORE_ASSERT(phase_ == Phase::NotStarted);
    for (CombatantId id = 0; id < combatants_.size(); ++id)
        if (combatants_[id].alive()) queue_.add(id, std::max(1, combatants_[id].stats.speed));
    if (!checkEnd(now)) beginTurn(now);
}

std::vector<CombatantId> Battle::living(Team team) const {
    std::vector<CombatantId> out;
    living(team, out);
    return out;
}

void Battle::living(Team team, std::vector<CombatantId>& out) const {
    out.clear();
    for (CombatantId id = 0; id < combatants_.size(); ++id)
        if (combatants_[id].team == team && combatants_[id].alive()) out.push_back(id);
}

std::size_t Battle::livingCount(Team team) const {
    std::size_t n = 0;
    for (const Combatant& c : combatants_) n += c.team == team && c.alive();
    return n;
}

const Skill* Battle::activeSkill() const {
    if (!skillIndex_) return nullptr;
    return &combatants_[actor_].skills[*skillIndex_];
}

ComboParams Battle::previewCombo(CombatantId performer, CombatantId opponent, const Skill& skill) const {
    const float rating = comboRating(skill.difficulty, combatants_[performer].stats, combatants_[opponent].stats,
                                     tuning_.difficulty);
    if (!tuning_.music.enabled()) return ::ftcore::comboParams(skill.notes, rating, tuning_.difficulty);
    ComboParams p = ::ftcore::comboParams(skill.notes, rating, tuning_.difficulty, tuning_.music.bpm);
    p.beatOffset = tuning_.music.offset;
    return p;
}

void Battle::beginTurn(double now) {
    actor_ = queue_.next();
    phaseStart_ = now;
    skillIndex_.reset();
    targets_.clear();
    shares_.clear();
    emit({.type = EventType::TurnStarted, .source = actor_, .time = now, .label = {}});
    if (combatants_[actor_].team == Team::Party)
        phase_ = Phase::AwaitingCommand;
    else
        startEnemyTurn(now);
}

void Battle::startEnemyTurn(double now) {
    Combatant& enemy = combatants_[actor_];
    const std::size_t idx = static_cast<std::size_t>(enemyTurnCount_[actor_]++) % enemy.skills.size();

    // Focus the most wounded party member (ties: lowest id) so behaviour is deterministic.
    CombatantId target = 0;
    bool found = false;
    for (CombatantId id = 0; id < combatants_.size(); ++id) {
        const Combatant& c = combatants_[id];
        if (c.team != Team::Party || !c.alive()) continue;
        if (!found || c.hp < combatants_[target].hp) target = id;
        found = true;
    }
    FTCORE_ASSERT(found);

    skillIndex_ = idx;
    targets_.clear();
    targets_.push_back(target);
    shares_.clear();
    shares_.push_back({target, damage(enemy, combatants_[target], enemy.skills[idx].power)});
    phase_ = Phase::EnemySkill;
    phaseStart_ = now;
    // No party chart during an enemy turn (swaps buffers: no allocation).
    noteScratch_.clear();
    combo_.reset(noteScratch_, scaled(tuning_.windows, 1.0), tuning_.lockout, tuning_.weights);
    // The banner is now; the hits follow at the skill's fixed offsets.
    landedWeight_ = 0.0f;
    defence_.reset(enemy.skills[idx].timeline, now, tuning_.defence);
    emit({.type = EventType::SkillStarted, .source = actor_, .target = target, .time = now, .label = enemy.skills[idx].name});
    if (defence_.complete()) finishSkill(now); // a skill without hits
}

void Battle::startCombo(const Skill& skill, CombatantId performer, CombatantId opponent, double firstNote,
                        double now) {
    params_ = previewCombo(performer, opponent, skill);
    const std::uint64_t seed = tuning_.seed ^ (++combosPlayed_ * 0xD1B54A32D192ED03ull);
    std::vector<ComboNote>& notes = noteScratch_;
    if (params_.bpm > 0.0) {
        // First note on the next beat (or bar) at or after the earliest allowed time.
        const double spb = 60.0 / params_.bpm;
        const double quantum = tuning_.music.startOnBar ? static_cast<double>(std::max(1, tuning_.music.beatsPerBar)) : 1.0;
        generateComboOnGridInto(notes, params_, ceilToGrid((firstNote - params_.beatOffset) / spb, quantum), seed);
    } else {
        generateComboInto(notes, params_, firstNote, seed);
    }
    combo_.reset(notes, scaled(tuning_.windows, params_.windowScale), tuning_.lockout, tuning_.weights);
    emit({.type = EventType::SkillStarted,
          .source = actor_,
          .target = targets_.empty() ? actor_ : targets_.front(),
          .time = now,
          .label = skill.name});
}

CommandError Battle::command(std::size_t skillIndex, CombatantId target, double now) {
    if (phase_ != Phase::AwaitingCommand) return CommandError::WrongPhase;
    Combatant& c = combatants_[actor_];
    if (skillIndex >= c.skills.size()) return CommandError::NoSuchSkill;
    const Skill& skill = c.skills[skillIndex];
    if (c.ap < skill.apCost) return CommandError::NotEnoughAp;

    if (skill.targeting == Targeting::SingleEnemy) {
        if (target >= combatants_.size() || combatants_[target].team != Team::Enemies ||
            !combatants_[target].alive())
            return CommandError::InvalidTarget;
        targets_.clear();
        targets_.push_back(target);
    } else {
        targets_.clear();
        for (CombatantId id = 0; id < combatants_.size(); ++id)
            if (combatants_[id].team == Team::Enemies && combatants_[id].alive()) targets_.push_back(id);
    }

    c.ap = std::clamp(c.ap - skill.apCost + skill.apGain, 0, c.maxAp);
    skillIndex_ = skillIndex;
    phase_ = Phase::PartySkill;
    phaseStart_ = now;
    shares_.clear();
    for (CombatantId t : targets_) shares_.push_back({t, damage(c, combatants_[t], skill.power)});

    // Area skills are judged against the toughest technique among the targets.
    const CombatantId opponent = *std::max_element(targets_.begin(), targets_.end(), [&](CombatantId a, CombatantId b) {
        return combatants_[a].stats.technique < combatants_[b].stats.technique;
    });
    startCombo(skill, actor_, opponent, now + tuning_.leadIn, now);
    return CommandError::None;
}

void Battle::press(Arrow arrow, double timestamp) {
    if (phase_ != Phase::PartySkill) return;
    results_.clear();
    combo_.press(arrow, timestamp, results_);
    resolve(timestamp);
}

void Battle::defend(DefenceInput input, double timestamp) {
    if (phase_ != Phase::EnemySkill) return;
    resolveDefence(defence_.press(input, timestamp), timestamp);
}

void Battle::update(double now) {
    switch (phase_) {
    case Phase::PartySkill:
        results_.clear();
        combo_.update(now, results_);
        resolve(now);
        break;
    case Phase::EnemySkill:
        resolveDefence(defence_.update(now), now);
        break;
    case Phase::Recovery:
        if (now >= recoveryEnd_) beginTurn(now);
        break;
    default: break;
    }
}

void Battle::resolve(double now) {
    for (const NoteResult& r : results_) {
        if (phase_ != Phase::PartySkill) return;
        emit({.type = EventType::NoteJudged,
              .source = actor_,
              .target = targets_.front(),
              .grade = r.grade,
              .beat = r.index,
              .time = now,
              .label = r.wrongArrow ? "WRONG" : ""});
        resolveAttackNote(r, now);
    }
    if (phase_ == Phase::PartySkill && (combo_.complete() || targetsGone())) finishSkill(now);
}

// Damage is spread over the notes: after each note the total dealt so far is the full
// damage times (score so far / note count), rounded. So the final total is exactly
// round(full * accuracy) no matter how many notes the combo had.
void Battle::resolveAttackNote(const NoteResult& r, double now) {
    const float fraction = combo_.score() / static_cast<float>(combo_.size());
    for (Share& s : shares_) {
        if (!combatants_[s.target].alive()) continue;
        const int amount = static_cast<int>(std::lround(s.full * fraction)) - s.applied;
        if (amount <= 0) continue;
        s.applied += amount;
        dealDamage(actor_, s.target, amount, r.grade, r.index, now);
        if (phase_ == Phase::Victory) return;
    }
}

// Each hit carries its weight's share of the blow. A hit that lands deals its share (rounded
// cumulatively, so every hit landing deals exactly the full blow); a dodged, jumped or parried hit
// deals nothing, and a perfect parry or dodge refunds AP to the defender.
void Battle::resolveDefence(const DefenceResults& results, double now) {
    for (const DefenceResult& r : results) {
        if (phase_ != Phase::EnemySkill) return;
        Share& s = shares_.front();
        Combatant& d = combatants_[s.target];
        const int refund = r.refund && d.alive() ? std::max(0, std::min(tuning_.apOnPerfect, d.maxAp - d.ap)) : 0;
        d.ap += refund;
        const Grade grade = r.refund ? Grade::Perfect : r.outcome == DefenceOutcome::Hit ? Grade::Miss : Grade::Good;
        emit({.type = EventType::Defended,
              .source = actor_,
              .target = s.target,
              .amount = refund,
              .grade = grade,
              .beat = r.hit,
              .time = r.time,
              .label = r.refund ? "PERFECT" : defenceOutcomeName(r.outcome)});
        if (r.outcome != DefenceOutcome::Hit || !d.alive()) continue;
        landedWeight_ += defence_.hit(r.hit).weight;
        const float total = defence_.timeline().totalWeight();
        const float fraction = total > 0.0f ? std::min(landedWeight_ / total, 1.0f) : 1.0f;
        const int taken = static_cast<int>(std::lround(s.full * fraction)) - s.taken;
        s.taken += taken;
        if (taken > 0) dealDamage(actor_, s.target, taken, grade, r.hit, now);
    }
    if (phase_ == Phase::EnemySkill && (defence_.complete() || targetsGone())) finishSkill(now);
}

bool Battle::targetsGone() const {
    return std::none_of(targets_.begin(), targets_.end(), [&](CombatantId t) { return combatants_[t].alive(); });
}

void Battle::finishSkill(double now) {
    const bool full = combo_.complete() && combo_.fullCombo();
    if (phase_ == Phase::PartySkill)
        emit({.type = EventType::ComboFinished,
              .source = actor_,
              .target = targets_.front(),
              .amount = static_cast<int>(std::lround(combo_.accuracy() * 100.0f)),
              .time = now,
              .label = full ? "FULL COMBO" : ""});

    if (phase_ == Phase::PartySkill && full && tuning_.fullComboBonus > 0.0f) {
        for (Share& s : shares_) {
            if (!combatants_[s.target].alive()) continue;
            const int bonus = static_cast<int>(std::lround(s.full * tuning_.fullComboBonus));
            if (bonus > 0) dealDamage(actor_, s.target, bonus, Grade::Perfect, combo_.size(), now);
            if (phase_ == Phase::Victory) return;
        }
    }
    // A fully parried attack: the defender strikes back.
    if (phase_ == Phase::EnemySkill && defence_.allParried()) {
        const CombatantId defender = targets_.front();
        Combatant& d = combatants_[defender];
        if (d.alive() && combatants_[actor_].alive()) {
            const int dmg = std::max(1, static_cast<int>(std::lround(damage(d, combatants_[actor_], tuning_.counterPower))));
            d.ap = std::min(d.ap + tuning_.apOnCounter, d.maxAp);
            emit({.type = EventType::Counter, .source = defender, .target = actor_, .amount = dmg, .time = now, .label = {}});
            dealDamage(defender, actor_, dmg, Grade::Perfect, defence_.size(), now);
            if (phase_ == Phase::Victory || phase_ == Phase::Defeat) return;
        }
    }
    if (checkEnd(now)) return;
    phase_ = Phase::Recovery;
    recoveryEnd_ = now + tuning_.recovery;
}

float Battle::damage(const Combatant& attacker, const Combatant& defender, float power) const {
    const float raw = static_cast<float>(attacker.stats.attack) * power;
    return raw * 100.f / (100.f + static_cast<float>(std::max(0, defender.stats.defense)) * 5.f);
}

void Battle::dealDamage(CombatantId source, CombatantId target, int amount, Grade grade, std::size_t beat,
                        double now) {
    Combatant& t = combatants_[target];
    if (!t.alive() || amount <= 0) return;
    t.hp = std::max(0, t.hp - amount);
    emit({.type = EventType::Hit, .source = source, .target = target, .amount = amount, .grade = grade, .beat = beat,
          .time = now, .label = {}});
    if (!t.alive()) {
        queue_.remove(target);
        emit({.type = EventType::Downed, .source = source, .target = target, .time = now, .label = {}});
        checkEnd(now);
    }
}

bool Battle::checkEnd(double now) {
    if (phase_ == Phase::Victory || phase_ == Phase::Defeat) return true;
    if (livingCount(Team::Enemies) == 0) {
        phase_ = Phase::Victory;
        emit({.type = EventType::Victory, .time = now, .label = {}});
        return true;
    }
    if (livingCount(Team::Party) == 0) {
        phase_ = Phase::Defeat;
        emit({.type = EventType::Defeat, .time = now, .label = {}});
        return true;
    }
    return false;
}

std::vector<BattleEvent> Battle::drainEvents() {
    std::vector<BattleEvent> out;
    out.swap(events_);
    return out;
}

} // namespace ftcore
