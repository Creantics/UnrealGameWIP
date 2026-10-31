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
        d.poise = std::max(0, d.stats.maxPoise);
        d.staggered = false;
    }
    for (const Combatant& c : enemies) {
        Combatant& d = put(c);
        d.team = Team::Enemies;
        d.poise = std::max(0, d.stats.maxPoise);
        d.staggered = false;
        FTCORE_ASSERT(!d.skills.empty());
    }
    enemyTurnCount_.assign(combatants_.size(), 0);
    enemyPhase_.assign(combatants_.size(), 0);
    baseAttack_.resize(combatants_.size());
    for (std::size_t i = 0; i < combatants_.size(); ++i) baseAttack_[i] = combatants_[i].stats.attack;
    stats_ = {};
    items_ = ItemTable{};
    carried_ = Inventory{};

    queue_.clear();
    queue_.reserve(combatants_.size()); // a revived member rejoins it without allocating
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
    aim_.reset(WeakPoints{}, 0.0, tuning_.aim);
    lastShot_ = {};
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
    stats_ = {};
    stats_.startTime = now;
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

std::size_t Battle::nextEnemySkill(CombatantId id) const {
    if (id >= combatants_.size()) return 0;
    const Combatant& c = combatants_[id];
    const std::size_t n = static_cast<std::size_t>(enemyTurnCount_[id]);
    if (c.phases.empty()) return c.skills.empty() ? 0 : n % c.skills.size();
    const std::size_t s = c.phases[enemyPhase_[id]].step(n, c.skills.size());
    return s < c.skills.size() ? s : 0;
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
    Combatant& c = combatants_[actor_];
    if (c.staggered) { // a broken stance: the turn passes, the poise comes back
        c.staggered = false;
        const int restored = std::max(0, c.stats.maxPoise) - c.poise;
        c.poise += restored;
        emit({.type = EventType::Steadied, .source = actor_, .amount = restored, .time = now, .label = {}});
        phase_ = Phase::Recovery;
        recoveryEnd_ = now + tuning_.recovery;
        return;
    }
    if (c.team == Team::Party)
        phase_ = Phase::AwaitingCommand;
    else
        startEnemyTurn(now);
}

void Battle::startEnemyTurn(double now) {
    Combatant& enemy = combatants_[actor_];
    const std::size_t idx = nextEnemySkill(actor_);
    ++enemyTurnCount_[actor_];

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
    // The banner is now; the hits follow at the skill's offsets, each moved later onto the song's
    // next beat when music is on (a fixed-size copy: no allocation).
    landedWeight_ = 0.0f;
    const MusicConfig& music = tuning_.music;
    defence_.reset(music.enabled() ? snapToBeat(enemy.skills[idx].timeline, now, music.bpm, music.offset)
                                   : enemy.skills[idx].timeline,
                   now, tuning_.defence);
    // The count: the beats before the first hit, as many of tuning_.countIn as fit after the banner.
    countLeft_ = 0;
    if (music.enabled() && tuning_.countIn > 0 && defence_.size() > 0) {
        const double spb = 60.0 / music.bpm;
        countHit_ = defence_.impact(0);
        const int fits = static_cast<int>(std::floor((countHit_ - now) / spb + 1e-6));
        countLeft_ = std::clamp(fits, 0, tuning_.countIn);
    }
    emit({.type = EventType::SkillStarted, .source = actor_, .target = target, .time = now, .label = enemy.skills[idx].name});
    if (defence_.complete()) {
        finishSkill(now); // a skill without hits
        return;
    }
    emitCountIn(now);
}

void Battle::emitCountIn(double now) {
    const double spb = tuning_.music.enabled() ? 60.0 / tuning_.music.bpm : 0.0;
    while (countLeft_ > 0 && phase_ == Phase::EnemySkill) {
        const double at = countHit_ - countLeft_ * spb;
        if (now + 1e-9 < at) return;
        emit({.type = EventType::CountIn,
              .source = actor_,
              .target = targets_.front(),
              .beat = static_cast<std::size_t>(countLeft_),
              .time = at,
              .label = {}});
        --countLeft_;
    }
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
    const bool afterAttack =
        phase_ == Phase::Recovery && combatants_[actor_].team == Team::Enemies && !targets_.empty();
    if (phase_ != Phase::EnemySkill && !afterAttack) return;
    const int lateBefore = defence_.lateCount();
    resolveDefence(defence_.press(input, timestamp), timestamp);
    if (defence_.lateCount() == lateBefore) return;
    if (phase_ != Phase::EnemySkill && phase_ != Phase::Recovery) return; // the outcome already came
    emit({.type = EventType::Defended,
          .source = actor_,
          .target = targets_.front(),
          .grade = Grade::Miss,
          .beat = defence_.lastLate(),
          .time = timestamp,
          .label = "LATE"});
}

CommandError Battle::aim(CombatantId target, double now) {
    if (phase_ != Phase::AwaitingCommand) return CommandError::WrongPhase;
    Combatant& c = combatants_[actor_];
    if (target >= combatants_.size() || combatants_[target].team != Team::Enemies || !combatants_[target].alive())
        return CommandError::InvalidTarget;
    const int cost = std::max(0, tuning_.aim.apCost);
    if (c.ap < cost) return CommandError::NotEnoughAp;
    c.ap -= cost;
    skillIndex_.reset();
    targets_.clear();
    targets_.push_back(target);
    shares_.clear();
    shares_.push_back({.target = target, .full = damage(c, combatants_[target], tuning_.aim.power)});
    phase_ = Phase::PartyAim;
    phaseStart_ = now;
    aim_.reset(combatants_[target].weakPoints, now, tuning_.aim);
    emit({.type = EventType::SkillStarted, .source = actor_, .target = target, .time = now, .label = "AIM"});
    return CommandError::None;
}

void Battle::moveReticle(float dx, float dy) {
    if (phase_ == Phase::PartyAim) aim_.move(dx, dy);
}

void Battle::setReticle(float x, float y) {
    if (phase_ == Phase::PartyAim) aim_.set(x, y);
}

bool Battle::cancelAim(double now) {
    if (phase_ != Phase::PartyAim) return false;
    Combatant& c = combatants_[actor_];
    c.ap = std::min(c.ap + std::max(0, tuning_.aim.apCost), c.maxAp);
    targets_.clear();
    shares_.clear();
    phase_ = Phase::AwaitingCommand;
    phaseStart_ = now;
    return true;
}

// The shot deals the aim damage times the shot's multiplier, at least 1. A shot after the
// deadline is judged at the deadline.
bool Battle::fire(double timestamp) {
    if (phase_ != Phase::PartyAim) return false;
    const double t = std::min(timestamp, aim_.deadline());
    lastShot_ = aim_.judge(t);
    const Share s = shares_.front();
    const int amount = std::max(1, static_cast<int>(std::lround(s.full * lastShot_.multiplier)));
    const Grade grade = lastShot_.kind == ShotKind::Weak   ? Grade::Perfect
                        : lastShot_.kind == ShotKind::Body ? Grade::Good
                                                           : Grade::Miss;
    dealDamage(actor_, s.target, amount, grade, lastShot_.weakPoint, t, shotKindName(lastShot_.kind));
    if (checkEnd(t)) return true;
    phase_ = Phase::Recovery;
    recoveryEnd_ = t + tuning_.recovery;
    return true;
}

void Battle::setItems(const ItemTable& table, const Inventory& carried) {
    FTCORE_ASSERT(phase_ == Phase::NotStarted);
    items_ = table;
    carried_ = carried;
}

// The recipients are the members the item can work on: living party members for a heal or an AP
// restore, downed ones for a revive. A single-ally item takes `target` if it is one of them; an item for
// the whole party takes all of them. The turn queue and the recipient list are sized up front, so
// nothing here allocates.
CommandError Battle::useItem(std::size_t slot, CombatantId target, double now) {
    if (phase_ != Phase::AwaitingCommand) return CommandError::WrongPhase;
    if (slot >= carried_.size()) return CommandError::NoSuchItem;
    const ItemId used = carried_[slot].id; // a copy: removing the stack below moves the slots
    const ItemDef* def = items_.find(used.view());
    if (!def) return CommandError::NoSuchItem;

    const bool revive = def->effect == ItemEffect::Revive;
    const auto eligible = [&](CombatantId id) {
        const Combatant& c = combatants_[id];
        return c.team == Team::Party && c.alive() != revive;
    };
    targets_.clear();
    if (def->scope == ItemScope::Party) {
        for (CombatantId id = 0; id < combatants_.size(); ++id)
            if (eligible(id)) targets_.push_back(id);
    } else if (target < combatants_.size() && eligible(target)) {
        targets_.push_back(target);
    }
    if (targets_.empty()) return CommandError::InvalidTarget;

    for (const CombatantId id : targets_) {
        Combatant& c = combatants_[id];
        int restored = 0;
        switch (def->effect) {
        case ItemEffect::Heal:
            restored = std::min(std::max(0, def->hpFor(c.stats.maxHp)), std::max(0, c.stats.maxHp - c.hp));
            c.hp += restored;
            break;
        case ItemEffect::RestoreAp:
            restored = std::min(std::max(0, def->ap), std::max(0, c.maxAp - c.ap));
            c.ap += restored;
            break;
        case ItemEffect::Revive:
            restored = std::min(std::max(1, def->hpFor(c.stats.maxHp)), std::max(1, c.stats.maxHp));
            c.hp = restored;
            if (!queue_.contains(id)) queue_.add(id, std::max(1, c.stats.speed));
            break;
        }
        emit({.type = EventType::ItemUsed,
              .source = actor_,
              .target = id,
              .amount = restored,
              .time = now,
              .label = def->id.view()});
    }
    carried_.remove(used.view(), 1);
    ++stats_.itemsUsed;
    targets_.clear();
    phase_ = Phase::Recovery;
    phaseStart_ = now;
    recoveryEnd_ = now + tuning_.recovery;
    return CommandError::None;
}

void Battle::update(double now) {
    switch (phase_) {
    case Phase::PartySkill:
        results_.clear();
        combo_.update(now, results_);
        resolve(now);
        break;
    case Phase::EnemySkill:
        emitCountIn(now);
        resolveDefence(defence_.update(now), now);
        break;
    case Phase::PartyAim:
        if (aim_.expired(now)) fire(aim_.deadline());
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
            losePoise(defender, actor_, tuning_.counterPoise, now);
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
                        double now, std::string_view label) {
    Combatant& t = combatants_[target];
    if (!t.alive() || amount <= 0) return;
    t.hp = std::max(0, t.hp - amount);
    emit({.type = EventType::Hit, .source = source, .target = target, .amount = amount, .grade = grade, .beat = beat,
          .time = now, .label = label});
    if (!t.alive()) {
        queue_.remove(target);
        emit({.type = EventType::Downed, .source = source, .target = target, .time = now, .label = {}});
        checkEnd(now);
        return;
    }
    if (t.team == Team::Enemies) checkPhase(target, now);
}

// Poise only falls for a combatant that has some and is standing; at 0 it is staggered, and it
// takes no more poise loss until its skipped turn refills it.
void Battle::losePoise(CombatantId source, CombatantId target, int amount, double now) {
    Combatant& t = combatants_[target];
    if (amount <= 0 || !t.alive() || t.staggered || t.stats.maxPoise <= 0) return;
    const int lost = std::min(amount, t.poise);
    t.poise -= lost;
    emit({.type = EventType::PoiseLost, .source = source, .target = target, .amount = lost, .time = now, .label = {}});
    if (t.poise > 0) return;
    t.staggered = true;
    emit({.type = EventType::Staggered, .source = source, .target = target, .time = now, .label = {}});
}

// Enters the deepest later phase whose threshold the enemy's HP has fallen to (skipped phases are
// passed over: only the phase entered applies its attack and heal).
void Battle::checkPhase(CombatantId enemy, double now) {
    Combatant& c = combatants_[enemy];
    const std::size_t current = enemyPhase_[enemy];
    std::size_t next = current;
    for (std::size_t p = current + 1; p < c.phases.size(); ++p)
        if (static_cast<float>(c.hp) <= c.phases[p].hpBelow * static_cast<float>(c.stats.maxHp)) next = p;
    if (next == current) return;
    const EnemyPhase& phase = c.phases[next];
    enemyPhase_[enemy] = static_cast<std::uint8_t>(next);
    enemyTurnCount_[enemy] = 0;
    c.stats.attack = baseAttack_[enemy] + baseAttack_[enemy] * std::max(0, phase.attackPercent) / 100;
    const int heal = std::min(c.stats.maxHp * std::max(0, phase.healPercent) / 100, c.stats.maxHp - c.hp);
    c.hp += std::max(0, heal);
    emit({.type = EventType::PhaseChanged,
          .source = enemy,
          .target = enemy,
          .amount = std::max(0, heal),
          .beat = next,
          .time = now,
          .label = phase.label()});
}

void Battle::tally(const BattleEvent& e) {
    const auto onParty = [&](CombatantId id) { return id < combatants_.size() && combatants_[id].team == Team::Party; };
    switch (e.type) {
    case EventType::TurnStarted: ++(onParty(e.source) ? stats_.partyTurns : stats_.enemyTurns); break;
    case EventType::NoteJudged:
        ++stats_.notes;
        stats_.perfectNotes += e.grade == Grade::Perfect;
        stats_.missedNotes += e.grade == Grade::Miss;
        break;
    case EventType::Hit:
        if (onParty(e.source)) stats_.damageDealt += e.amount;
        if (onParty(e.target)) stats_.damageTaken += e.amount;
        stats_.weakPointHits += e.label == shotKindName(ShotKind::Weak);
        break;
    case EventType::Defended:
        if (e.label == "LATE") break;
        if (e.label == "HIT") ++stats_.hitsTaken;
        else ++stats_.hitsDefended;
        stats_.perfectDefences += e.label == "PERFECT";
        break;
    case EventType::ComboFinished: stats_.fullCombos += e.label == "FULL COMBO"; break;
    case EventType::Counter: ++stats_.counters; break;
    case EventType::PhaseChanged: ++stats_.phaseChanges; break;
    case EventType::Staggered: ++stats_.staggers; break;
    case EventType::Victory:
    case EventType::Defeat: stats_.endTime = e.time; break;
    default: break;
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
