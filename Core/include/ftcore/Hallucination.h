#pragma once

// A hallucination in the Unwritten: an offer made of one of the active lead's tragedies (TragedyPool). It never
// deals damage and nothing but the lead's own acceptance builds its hold: no blow, hp or time does.
//
//   Idle --start--> Shown --accepting--> Holding (holdLevel 0..1 over holdSeconds; out of the offer: back to 0)
//                                         at 1: Held, still and blank until refused or pulled back
//   Shown or Holding --refusal held refuseSeconds--> Refused       Shown --turned away--> Refused
//   Shown or a partial hold --expireSeconds--> Expired             Held --partner pull--> PulledBack
//   any --the active lead changes--> Torn (the hold ends with it)
//
// * One at a time. A start needs a band above None, and (unless scripted) the gap since the last one ended
//   (gapSeconds; x fewerGapScale under Fewer; none at all under StoryOnly) and switchSeconds since a lead switch.
// * The refusal uses HeldRefusal: a button already down when it starts does not count. A refusal pressed with no
//   offer open starts the refusal cooldown and nothing else; a refusal that closes one starts it too. While cooling,
//   presses do nothing.
// * In a fight the partner pulls a held lead back after pullSeconds held; elsewhere the host says when (partnerPull).
//   With no partner present only the refusal ends a hold.
// * The intensity band is authored: max(the page's place band, the act's floor). Nothing accumulates.
// * The host draws, voices and closes it (the pen stroke, the tear); Core keeps the rules. No allocation.
//
// Data/Unwritten/hallucination.json: { "version": 1, "holdSeconds": 4, "expireSeconds": 30, "gapSeconds": 90,
//   "switchSeconds": 20, "refuseSeconds": 0.5, "refusalCooldownSeconds": 4, "pullSeconds": 6, "recentSeconds": 1800,
//   "fewerGapScale": 2, "actFloors": ["none", "none", "none", "low", "mid"] }   (every field but version optional)

#include "ftcore/Api.h"
#include "ftcore/CombatInputs.h"
#include "ftcore/HeldRefusal.h"
#include "ftcore/TragedyPool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace ftcore {

enum class IntensityBand : std::uint8_t { None, Low, Mid, High }; // append only
enum class HallucinationFrequency : std::uint8_t { Normal, Fewer, StoryOnly }; // append only
enum class HallucinationEventType : std::uint8_t { Started, HoldBegan, Held, Refused, PulledBack, Expired, Torn }; // append only
enum class HallucinationStart : std::uint8_t { Started, Busy, NoBand, StoryOnly, Gap, SwitchDelay, NoKey }; // append only

inline constexpr int kMaxAct = 4; // acts 0 (the prologue) to 4

struct HallucinationTuning {
    float holdSeconds = 4.f;
    float expireSeconds = 30.f;
    float gapSeconds = 90.f;
    float switchSeconds = 20.f;
    float refuseSeconds = 0.5f;
    float refusalCooldownSeconds = 4.f;
    float pullSeconds = 6.f;
    float recentSeconds = 1800.f;
    float fewerGapScale = 2.f;
    std::array<IntensityBand, kMaxAct + 1> actFloors{IntensityBand::None, IntensityBand::None, IntensityBand::None,
                                                     IntensityBand::Low, IntensityBand::Mid};
};

FTCORE_API bool importHallucinationTuning(std::string_view json, HallucinationTuning& out, std::string* error = nullptr);

// The band a page shows: its own, or the act's floor if higher. An act out of range is clamped.
inline IntensityBand intensityBand(IntensityBand place, int act, const HallucinationTuning& t = {}) {
    const IntensityBand actFloor = t.actFloors[static_cast<std::size_t>(std::clamp(act, 0, kMaxAct))];
    return std::max(place, actFloor);
}

struct HallucinationInput {
    FighterId lead = kLeadA;     // the active lead
    bool accepting = false;      // inside the offer: in the figure, staying beside it, or holding the hand
    bool refuse = false;         // the refusal button is down
    bool turnedAway = false;     // the lead turned away from the offer
    bool partnerPresent = false; // the other lead is here
    bool partnerPull = false;    // the other lead has reached a held lead and taken their arm
    bool inFight = false;        // the partner pulls on its own after pullSeconds held
};

struct HallucinationEvent {
    HallucinationEventType type = HallucinationEventType::Started;
    int key = -1;
};

class Hallucination {
public:
    explicit Hallucination(const HallucinationTuning& t = {}, HallucinationFrequency f = HallucinationFrequency::Normal)
        : tuning_(t), frequency_(f) {}

    void setFrequency(HallucinationFrequency f) { frequency_ = f; }

    // Starts one of the active lead's keys, picked by `pool` with `rng` for a page of place type `place` (bits; 0 for
    // any), or `named` itself for a scripted one that names it. Started: shown, and marked in the pool. The host ticks
    // once first (the active lead comes from the tick).
    HallucinationStart start(TragedyPool& pool, std::uint32_t& rng, std::uint32_t place, IntensityBand band,
                             bool scripted, int named = -1) {
        if (active()) return HallucinationStart::Busy;
        if (band == IntensityBand::None) return HallucinationStart::NoBand;
        if (!scripted) {
            if (frequency_ == HallucinationFrequency::StoryOnly) return HallucinationStart::StoryOnly;
            const float gap = tuning_.gapSeconds * (frequency_ == HallucinationFrequency::Fewer ? tuning_.fewerGapScale : 1.f);
            if (now_ - endedAt_ < gap) return HallucinationStart::Gap;
            if (now_ - switchedAt_ < tuning_.switchSeconds) return HallucinationStart::SwitchDelay;
        }
        const int k = scripted && named >= 0 ? (pool.has(named) && pool.lead(named) == lead_ ? named : -1)
                                           : pool.pick(rng, lead_, place, now_, tuning_.recentSeconds);
        if (k < 0) return HallucinationStart::NoKey;
        key_ = k;
        band_ = band;
        shownAt_ = now_;
        hold_ = 0.f;
        heldFor_ = 0.f;
        refuseHeld_ = 0.f;
        refusal_.offer();
        pool.shown(k, now_);
        emit(HallucinationEventType::Started);
        return HallucinationStart::Started;
    }

    // One step of `dt` seconds (a negative, NaN or infinite step counts as 0, so the clock stays sound). events()
    // holds what happened in it (and a Started from a start() after it).
    void tick(double dt, const HallucinationInput& in) {
        if (!(dt > 0.0 && std::isfinite(dt))) dt = 0.0;
        eventCount_ = 0;
        now_ += dt;
        cooldown_ = std::max(0.f, cooldown_ - static_cast<float>(dt));
        if (lead_ == kNoFighter) {
            lead_ = in.lead;
        } else if (in.lead != lead_) {
            lead_ = in.lead;
            switchedAt_ = now_;
            if (active()) end(HallucinationEventType::Torn);
        }

        // Every edge goes to the refusal, so a button down when an offer starts is stale there.
        if (in.refuse && !refuseDown_) {
            const bool cooling = cooldown_ > 0.f;
            refuseCounts_ = refusal_.holdDown() == RefusalInput::Holding && active() && !cooling;
            refuseHeld_ = 0.f;
            if (!active() && !cooling) cooldown_ = tuning_.refusalCooldownSeconds; // a wrong guess
        } else if (!in.refuse && refuseDown_) {
            refusal_.holdUp();
            refuseCounts_ = false;
        }
        refuseDown_ = in.refuse;
        if (!active()) return;
        if (refuseCounts_ && refusal_.holding()) {
            refuseHeld_ += static_cast<float>(dt);
            if (refuseHeld_ >= tuning_.refuseSeconds) {
                refusal_.offerEnded();
                cooldown_ = tuning_.refusalCooldownSeconds;
                end(HallucinationEventType::Refused);
                return;
            }
        }

        if (held()) {
            heldFor_ += static_cast<float>(dt);
            if (in.partnerPresent && (in.partnerPull || (in.inFight && heldFor_ >= tuning_.pullSeconds)))
                end(HallucinationEventType::PulledBack);
            return;
        }
        if (in.accepting) {
            if (hold_ == 0.f) emit(HallucinationEventType::HoldBegan);
            hold_ = std::min(1.f, hold_ + static_cast<float>(dt) / tuning_.holdSeconds);
            if (hold_ >= 1.f) {
                heldFor_ = 0.f;
                emit(HallucinationEventType::Held);
                return;
            }
        } else {
            hold_ = 0.f;
            if (in.turnedAway) {
                end(HallucinationEventType::Refused);
                return;
            }
        }
        if (now_ - shownAt_ >= tuning_.expireSeconds) end(HallucinationEventType::Expired);
    }

    bool active() const { return key_ >= 0; }
    bool held() const { return active() && hold_ >= 1.f; }
    int key() const { return key_; }
    IntensityBand band() const { return active() ? band_ : IntensityBand::None; }
    float holdLevel() const { return hold_; } // the host slows the lead by it and stills them at 1
    float refusalCooldown() const { return cooldown_; }
    double now() const { return now_; }
    const HallucinationEvent* events() const { return events_.data(); }
    int eventCount() const { return eventCount_; }

private:
    void emit(HallucinationEventType type) {
        if (eventCount_ < static_cast<int>(events_.size())) events_[static_cast<std::size_t>(eventCount_++)] = {type, key_};
    }
    void end(HallucinationEventType why) {
        emit(why);
        key_ = -1;
        hold_ = 0.f;
        endedAt_ = now_;
    }

    static constexpr double kLongAgo = -1.0e300;
    HallucinationTuning tuning_;
    HallucinationFrequency frequency_;
    HeldRefusal refusal_;
    std::array<HallucinationEvent, 4> events_{};
    int eventCount_ = 0;
    double now_ = 0.0;
    double endedAt_ = kLongAgo;
    double switchedAt_ = kLongAgo;
    double shownAt_ = 0.0;
    FighterId lead_ = kNoFighter;
    int key_ = -1;
    IntensityBand band_ = IntensityBand::None;
    float hold_ = 0.f;
    float heldFor_ = 0.f;
    float refuseHeld_ = 0.f;
    float cooldown_ = 0.f;
    bool refuseDown_ = false;
    bool refuseCounts_ = false; // this hold was pressed on an open offer, not while cooling
};

} // namespace ftcore
