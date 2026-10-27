#include "ftcore/FightAnimator.h"

#include "ftcore/Battle.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

namespace ftcore {

const char* fightClipName(FightClip clip) {
    switch (clip) {
    case FightClip::Stance: return "stance";
    case FightClip::StrikeLeft: return "strike left";
    case FightClip::StrikeRight: return "strike right";
    case FightClip::Finisher: return "finisher";
    case FightClip::Dodge: return "dodge";
    case FightClip::Parry: return "parry";
    case FightClip::Jump: return "jump";
    case FightClip::HitReact: return "hit react";
    case FightClip::Counter: return "counter";
    case FightClip::Downed: return "downed";
    case FightClip::Victory: return "victory";
    case FightClip::Aim: return "aim";
    case FightClip::PhaseChange: return "phase change";
    case FightClip::Count: break;
    }
    return "?";
}

// ---- FightAnimator ----

void FightAnimator::reset() {
    count_ = 0;
    queuedLast_ = kMaxActions;
    layerCount_ = 0;
    current_ = FightClip::Stance;
}

double FightAnimator::keyTime(const Action& a) const {
    return a.start + static_cast<double>(clip(a.clip).key - a.offset) / a.rate;
}

double FightAnimator::endTime(const Action& a) const {
    if (holds(a.clip) || clip(a.clip).loop) return std::numeric_limits<double>::infinity();
    return a.start + static_cast<double>(clip(a.clip).length - a.offset) / a.rate;
}

double FightAnimator::lastKeyTime() const { return queuedLast_ < count_ ? keyTime(actions_[queuedLast_]) : -1.0; }

bool FightAnimator::play(FightClip c, double now, double keyAt) {
    if (!has(c)) return false;
    const FightClipInfo& info = clip(c);
    Action a{c, now, 1.f, 0.f};
    if (keyAt >= 0.0) {
        // A chained clip waits for the key frame of the one queued before it.
        if (queuedLast_ < count_ && info.key > 0.f) a.start = std::max(a.start, keyTime(actions_[queuedLast_]));
        const double lead = keyAt - a.start;
        const double key = info.key;
        if (lead >= key) {
            a.start = keyAt - key; // time to spare: at its own pace, starting later
        } else if (lead > 0.0 && key <= lead * tuning_.maxRate) {
            a.rate = static_cast<float>(key / lead);
        } else if (lead > 0.0) {
            a.rate = tuning_.maxRate;
            a.offset = static_cast<float>(key - lead * tuning_.maxRate);
        } else {
            a.offset = std::min(static_cast<float>(key - lead), info.length); // the beat is past: join after it
        }
    }
    if (count_ == kMaxActions) { // full: the oldest goes
        std::move(actions_.begin() + 1, actions_.end(), actions_.begin());
        --count_;
        if (queuedLast_ < kMaxActions) queuedLast_ = queuedLast_ == 0 ? kMaxActions : queuedLast_ - 1;
    }
    // By start time; after any that start at the same time.
    std::size_t at = count_;
    while (at > 0 && actions_[at - 1].start > a.start) --at;
    std::move_backward(actions_.begin() + static_cast<std::ptrdiff_t>(at), actions_.begin() + static_cast<std::ptrdiff_t>(count_),
                       actions_.begin() + static_cast<std::ptrdiff_t>(count_ + 1));
    actions_[at] = a;
    ++count_;
    queuedLast_ = at;
    return true;
}

void FightAnimator::update(double now) {
    // Ended one-shots go.
    std::size_t kept = 0;
    for (std::size_t i = 0; i < count_; ++i) {
        if (now >= endTime(actions_[i])) {
            if (queuedLast_ == i) queuedLast_ = kMaxActions;
            continue;
        }
        if (queuedLast_ == i) queuedLast_ = kept;
        actions_[kept++] = actions_[i];
    }
    count_ = kept;

    layerCount_ = 0;
    current_ = FightClip::Stance;
    float remaining = 1.f;
    std::size_t covered = 0; // actions before this index are hidden for good under a newer one
    bool started = false;
    for (std::size_t i = count_; i-- > 0;) {
        const Action& a = actions_[i];
        if (now < a.start) continue; // queued, not yet started
        if (remaining <= 0.f) {
            covered = i + 1;
            break;
        }
        const FightClipInfo& info = clip(a.clip);
        if (!started) current_ = a.clip;
        started = true;
        const double elapsed = now - a.start;
        float t = a.offset + static_cast<float>(elapsed) * a.rate;
        if (info.loop) t = std::fmod(t, info.length);
        else t = std::min(t, info.length);
        float alpha = tuning_.fadeIn > 0.f ? std::min(1.f, static_cast<float>(elapsed) / tuning_.fadeIn) : 1.f;
        const double end = endTime(a);
        if (tuning_.fadeOut > 0.f && end < std::numeric_limits<double>::infinity())
            alpha = std::min(alpha, static_cast<float>(end - now) / tuning_.fadeOut);
        alpha = std::clamp(alpha, 0.f, 1.f);
        const float w = remaining * alpha;
        remaining -= w;
        if (alpha >= 1.f) remaining = 0.f;
        if (w > 0.f) layers_[layerCount_++] = {a.clip, t, w};
    }
    if (covered > 0) {
        // Started actions under a fully faded-in one never show again; queued ones stay.
        std::size_t k = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            if (i < covered && now >= actions_[i].start) {
                if (queuedLast_ == i) queuedLast_ = kMaxActions;
                continue;
            }
            if (queuedLast_ == i) queuedLast_ = k;
            actions_[k++] = actions_[i];
        }
        count_ = k;
    }
    if (remaining > 0.f && has(FightClip::Stance)) {
        const FightClipInfo& s = clip(FightClip::Stance);
        layers_[layerCount_++] = {FightClip::Stance, static_cast<float>(std::fmod(std::max(0.0, now), static_cast<double>(s.length))),
                                  remaining};
    }
}

// ---- FightDirector ----

void FightDirector::bind(const Battle& battle) {
    animators_.resize(battle.combatants().size());
    for (FightAnimator& a : animators_) a.reset();
    striking_ = false;
    aiming_ = false;
    skipCounterHit_ = false;
    lastInput_ = DefenceInput::Parry;
}

bool FightDirector::playOr(CombatantId id, FightClip clip, FightClip fallback, double now, double keyAt) {
    if (id >= animators_.size()) return false;
    FightAnimator& a = animators_[id];
    return a.play(clip, now, keyAt) || a.play(fallback, now, keyAt);
}

// No earlier than the key frame of what `id` has queued (a strike's impact, a reaction's contact).
double FightDirector::afterKey(CombatantId id, double now) const {
    return id < animators_.size() ? std::max(now, animators_[id].lastKeyTime()) : now;
}

void FightDirector::queueStrikes(const Battle& battle, double now) {
    if (!striking_) return;
    if (partyStrikes_) {
        if (battle.phase() != Phase::PartySkill || battle.actor() != striker_) {
            striking_ = false;
            return;
        }
        const std::vector<ComboNote>& notes = battle.combo().notes();
        for (; nextStrike_ < notes.size() && notes[nextStrike_].time <= now + lookahead_; ++nextStrike_) {
            const FightClip side = nextStrike_ % 2 == 0 ? FightClip::StrikeLeft : FightClip::StrikeRight;
            const bool last = nextStrike_ + 1 == notes.size();
            playOr(striker_, last ? FightClip::Finisher : side, side, now, notes[nextStrike_].time);
        }
        return;
    }
    if (battle.phase() != Phase::EnemySkill || battle.actor() != striker_) {
        striking_ = false;
        return;
    }
    const DefenceTimeline& d = battle.defence();
    for (; nextStrike_ < d.size() && d.impact(nextStrike_) <= now + lookahead_; ++nextStrike_)
        playOr(striker_, nextStrike_ % 2 == 0 ? FightClip::StrikeLeft : FightClip::StrikeRight, FightClip::StrikeLeft, now,
               d.impact(nextStrike_));
}

void FightDirector::update(const Battle& battle, double now) {
    if (animators_.size() != battle.combatants().size()) return; // bind() first
    for (const BattleEvent& e : battle.events()) {
        switch (e.type) {
        case EventType::SkillStarted:
            if (e.label == "AIM") {
                aiming_ = playOr(e.source, FightClip::Aim, FightClip::Stance, now);
                aimer_ = e.source;
                break;
            }
            striker_ = e.source;
            partyStrikes_ = battle.combatant(e.source).team == Team::Party;
            striking_ = true;
            nextStrike_ = 0;
            break;
        case EventType::Defended: {
            FightClip c = FightClip::Stance;
            if (e.label == "DODGE") c = FightClip::Dodge;
            else if (e.label == "JUMP") c = FightClip::Jump;
            else if (e.label == "PERFECT") c = lastInput_ == DefenceInput::Dodge ? FightClip::Dodge : FightClip::Parry;
            if (c == FightClip::Stance) break; // HIT (the Hit reacts) or LATE
            const DefenceTimeline& d = battle.defence();
            playOr(e.target, c, c, now, e.beat < d.size() ? d.impact(e.beat) : e.time);
            break;
        }
        case EventType::Hit:
            if (skipCounterHit_ && e.source == counterSource_ && e.target == counterTarget_) {
                skipCounterHit_ = false;
                break;
            }
            playOr(e.target, FightClip::HitReact, FightClip::HitReact, now, e.time);
            break;
        case EventType::Counter: {
            const bool played = playOr(e.source, FightClip::Counter, FightClip::StrikeRight, now);
            const double contact = played && e.source < animators_.size() ? animators_[e.source].lastKeyTime() : now;
            playOr(e.target, FightClip::HitReact, FightClip::HitReact, now, std::max(contact, now));
            skipCounterHit_ = true;
            counterSource_ = e.source;
            counterTarget_ = e.target;
            break;
        }
        case EventType::Downed: playOr(e.target, FightClip::Downed, FightClip::Downed, now, afterKey(e.target, now)); break;
        case EventType::Victory:
            for (std::size_t i = 0; i < animators_.size(); ++i) {
                const Combatant& c = battle.combatants()[i];
                const CombatantId id = static_cast<CombatantId>(i);
                if (c.team == Team::Party && c.alive()) playOr(id, FightClip::Victory, FightClip::Stance, now, afterKey(id, now));
            }
            break;
        case EventType::PhaseChanged: playOr(e.source, FightClip::PhaseChange, FightClip::PhaseChange, now); break;
        default: break;
        }
    }
    if (aiming_ && (battle.phase() != Phase::PartyAim || battle.actor() != aimer_)) {
        aiming_ = false;
        if (battle.combatant(aimer_).alive()) playOr(aimer_, FightClip::Stance, FightClip::Stance, now);
    }
    queueStrikes(battle, now);
    for (FightAnimator& a : animators_) a.update(now);
}

} // namespace ftcore
