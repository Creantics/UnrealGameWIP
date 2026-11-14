#pragma once

// A hallucination in the Unwritten: an offer made of one of the active lead's tragedies (TragedyPool). It never
// deals damage and only the lead's own acceptance builds its hold, though each blow the lead takes while it builds
// pushes it on by holdPushPerHit (blowsTaken on the tick): hp and time alone never do.
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
//   presses do nothing. A refusal button down while the lead is held stays the refusal's until it is let go, even
//   once the hold has ended (refuseKept()), so a host that shares the button with another action never sees that
//   press as its own.
// * In a fight the partner pulls a held lead back after pullSeconds held; elsewhere the host says when (partnerPull).
//   With no partner present only the refusal ends a hold.
// * Brief links (the lead's Brief on the tick, none by default): Counsel cuts the refusal cooldown to
//   min(refusalCooldownSeconds, counselCooldownSeconds), for a close and a wrong guess alike. A present partner whose
//   Brief is Proxy refuses for a held lead: its press runs from proxyReactSeconds after Held, over the steps the
//   partner is present, still Proxy and able to act (the host says: not downed, in hitstun or guard break), and takes
//   refuseSeconds. Any break (unable, absent, Brief changed) drops the press; it starts afresh when all hold again.
//   Time is counted from the instant the condition holds, so the close lands within one step of the stated time at
//   any step size. The offer ends as Refused with the held lead's own refusal cooldown (Counsel's cut only if that
//   lead is Counsel). The fight's pull after pullSeconds still wins if it comes first. Once per offer.
//   For an Escrow lead the first escrowDelaySeconds inside an offer build no hold (holdLevel stays 0, so the lead
//   is not slowed; escrowWaiting() and escrowWaitLeft() show the wait); then the hold builds over holdSeconds.
//   The wait pauses, it never resets: escrowWaiting() is true only on a step it counts down (the lead accepting,
//   inside the wait, the clock moving; a hit-stop step keeps the last answer). Out of the offer it is false and
//   escrowWaitLeft() holds its last value; back in, the wait goes on from there. Only the offer's end resets it.
//   Stepping out before Held drops the wait and the hold back to nothing.
// * The push: each blow the lead takes while the hold builds (inside the offer, not held, no Escrow wait running)
//   adds holdPushPerHit seconds of hold at the step's start; pushed seconds are hold for the price below, and a
//   push can carry the hold to holdSeconds and Held. It does not interrupt a refusal press under way. When the lead's
//   own refusal press completes on a step, Refused wins, even if the hold would fill on that step too (a tie is
//   Refused's); Held wins only when the hold fills on an earlier step, or by a push at that step's start. Once Held
//   wins, a press of the lead's under way is spent: a new press is needed (the pull and Proxy apply as usual).
//   Against the old behaviour: a press begun strictly after 3.5 s of hold and kept down now leaves the lead Held
//   until a new press, the pull or Proxy (it used to complete as a refusal of the held lead); one begun at exactly
//   3.5 s ties and is Refused.
//   Both lengths count complete within 1e-4 s, so the outcome does not depend on the step size.
// * The price: refusing pays in exposure, so the hold must be let build first. The lead's own refusal that closes an
//   offer before Held, with h seconds of real hold built in it (counted to the closing instant; an Escrow wait is
//   not hold), opens the boss's weak point for refuseOpenBase + refuseOpenPerSecond x (min(h, refuseOpenHoldCap) -
//   refuseOpenMinHold) seconds when h >= refuseOpenMinHold (2 s at 1 s of hold, 5.75 s at 3.5 s or more), and
//   nothing below it. A refusal of a held lead (the lead's or a Proxy partner's), a turn away and a refusal while
//   the weak point is shut open nothing; nor does one within refuseOpenLockSeconds of an opened window's end (the
//   lock: the refusal still closes the offer and starts its cooldown). A refusal while a window is open keeps
//   the longer of the two. The lead's own refusal closing before Held with under refuseOpenMinHold of hold sets
//   refusedTooEarly() on that step only, so the host can say why nothing opened. Held costs the lead
//   heldHealthCost of max health once (heldCost(), on that step only), shuts the weak point for heldShutSeconds (an open one closes) and the fight goes on: no restart.
//   weakPointOpenFor(), weakPointShutFor() and weakPointLockFor() count down on every step, offer or none. Core keeps only these numbers;
//   the host opens and shuts the boss's weak point and takes the health.
// * The intensity band is authored: max(the page's place band, the act's floor). Nothing accumulates.
// * The pick is the pool's, weighted (TragedyPool.h); the lone key weighs x2 once the lead's lone Releases have
//   pooled lonePoolFloor Load (loneWeighs).
// * Only a scripted start flagged as a scare reaches the ScarePacer, once, when it starts (scareForm()); every other
//   start leaves the pacer alone and its scare form is Unpaced. The host completes the scare beat on the pacer.
// * The host draws, voices and closes it (the pen stroke, the tear); Core keeps the rules. No allocation.
//
// Data/Unwritten/hallucination.json: { "version": 1, "holdSeconds": 4, "expireSeconds": 30, "gapSeconds": 90,
//   "switchSeconds": 20, "refuseSeconds": 0.5, "refusalCooldownSeconds": 4, "pullSeconds": 6, "recentSeconds": 1800,
//   "fewerGapScale": 2, "lonePoolFloor": 300, "counselCooldownSeconds": 2, "proxyReactSeconds": 1,
//   "escrowDelaySeconds": 5, "refuseOpenMinHold": 1, "refuseOpenBase": 2, "refuseOpenPerSecond": 1.5,
//   "refuseOpenHoldCap": 3.5, "refuseOpenLockSeconds": 8, "holdPushPerHit": 0.5, "heldHealthCost": 0.25,
//   "heldShutSeconds": 10,
//   "actFloors": ["none", "none", "none", "low", "mid"] }
//   (every field but version optional; refuseOpenHoldCap at least refuseOpenMinHold)

#include "ftcore/Api.h"
#include "ftcore/Brief.h"
#include "ftcore/CombatInputs.h"
#include "ftcore/HeldRefusal.h"
#include "ftcore/ScarePacer.h"
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
    float lonePoolFloor = 300.f; // Load pooled in lone Releases (LoneSave::pooled) from which the lone key weighs x2
    float counselCooldownSeconds = 2.f; // a Counsel lead's refusal cooldown: min(refusalCooldownSeconds, this)
    float proxyReactSeconds = 1.f;      // a Proxy partner starts its refusal for a held lead this long after Held
    float escrowDelaySeconds = 5.f;     // an Escrow lead's first seconds inside an offer build no hold
    float refuseOpenMinHold = 1.f;      // seconds of hold a refusal needs to open the weak point at all
    float refuseOpenBase = 2.f;         // seconds it opens at refuseOpenMinHold of hold ...
    float refuseOpenPerSecond = 1.5f;   // ... plus this for each further second of hold ...
    float refuseOpenHoldCap = 3.5f;     // ... counted up to this much hold
    float refuseOpenLockSeconds = 8.f;  // after an opened window ends, no refusal opens one for this long
    float holdPushPerHit = 0.5f;        // seconds of hold each blow the lead takes while the hold builds adds: 0..2
    float heldHealthCost = 0.25f;       // Held costs the lead this share of max health, once
    float heldShutSeconds = 10.f;       // and shuts the weak point this long from Held
    std::array<IntensityBand, kMaxAct + 1> actFloors{IntensityBand::None, IntensityBand::None, IntensityBand::None,
                                                     IntensityBand::Low, IntensityBand::Mid};
};

FTCORE_API bool importHallucinationTuning(std::string_view json, HallucinationTuning& out, std::string* error = nullptr);

// The band a page shows: its own, or the act's floor if higher. An act out of range is clamped.
inline IntensityBand intensityBand(IntensityBand place, int act, const HallucinationTuning& t = {}) {
    const IntensityBand actFloor = t.actFloors[static_cast<std::size_t>(std::clamp(act, 0, kMaxAct))];
    return std::max(place, actFloor);
}

// Place bleed: of the bands, only High bleeds a place in (none shows nothing; low, a voice; mid, a figure; high, a
// figure plus a place bleeding in). The host draws it: a piece of the lead's home scene replacing part of the page,
// growing from the edges, never over the path.
inline bool placeBleeds(IntensityBand band) { return band == IntensityBand::High; }

// Lone Carry weighs the lone key once the lead's lone Releases have pooled lonePoolFloor Load (and some at all).
inline bool loneWeighs(std::uint32_t loneLoadPool, const HallucinationTuning& t = {}) {
    return loneLoadPool > 0 && static_cast<double>(loneLoadPool) >= static_cast<double>(t.lonePoolFloor);
}

// What a start asks for.
struct HallucinationStartIn {
    std::uint32_t place = 0;                  // the page's place type bits; 0 for any
    IntensityBand band = IntensityBand::None; // intensityBand(); None refuses the start
    bool scripted = false;                    // authored: no gap, no switch delay, and it runs under StoryOnly
    int named = -1;                           // a scripted one naming its key
    std::uint32_t loneLoadPool = 0;           // the Load the lead's lone Releases have pooled (LoneSave::pooled)
    bool flaggedAsScare = false;              // an authored moment flagged as a scare (scripted ones only)
    ScareMoment scare{};                      // that moment
};

struct HallucinationInput {
    FighterId lead = kLeadA;     // the active lead
    bool accepting = false;      // inside the offer: in the figure, staying beside it, or holding the hand
    bool refuse = false;         // the refusal button is down
    bool turnedAway = false;     // the lead turned away from the offer
    bool partnerPresent = false; // the other lead is here
    bool partnerPull = false;    // the other lead has reached a held lead and taken their arm
    bool inFight = false;        // the partner pulls on its own after pullSeconds held
    BriefId brief = BriefId::Count; // the active lead's Brief; Count for none
    BriefId partnerBrief = BriefId::Count; // the partner's Brief; Count for none
    bool partnerAble = false;              // the partner can act: not downed, in hitstun or guard break
    int blowsTaken = 0;                    // blows the active lead took this step (each pushes a building hold)
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

    // Starts one of the active lead's keys for a page of place type `s.place` (bits; 0 for any), picked by `pool`
    // with `rng` (the lone key x2 when loneWeighs(s.loneLoadPool)), or `s.named` itself for a scripted one that names
    // it. Started: shown, and marked in the pool; a scripted one flagged as a scare is then played once on `pacer`
    // (ScarePacer::hallucination; scareForm() says how its scare beat plays, Calm when the pacer holds it back). The
    // host ticks once first (the active lead comes from the tick).
    HallucinationStart start(TragedyPool& pool, std::uint32_t& rng, const HallucinationStartIn& s,
                             ScarePacer* pacer = nullptr) {
        if (active()) return HallucinationStart::Busy;
        if (s.band == IntensityBand::None) return HallucinationStart::NoBand;
        if (!s.scripted) {
            if (frequency_ == HallucinationFrequency::StoryOnly) return HallucinationStart::StoryOnly;
            const float gap = tuning_.gapSeconds * (frequency_ == HallucinationFrequency::Fewer ? tuning_.fewerGapScale : 1.f);
            if (now_ - endedAt_ < gap) return HallucinationStart::Gap;
            if (now_ - switchedAt_ < tuning_.switchSeconds) return HallucinationStart::SwitchDelay;
        }
        const int k = s.scripted && s.named >= 0
                          ? (pool.has(s.named) && pool.lead(s.named) == lead_ ? s.named : -1)
                          : pool.pick(rng, lead_, s.place, now_, tuning_.recentSeconds,
                                      loneWeighs(s.loneLoadPool, tuning_));
        if (k < 0) return HallucinationStart::NoKey;
        key_ = k;
        band_ = s.band;
        shownAt_ = now_;
        hold_ = 0.f;
        built_ = 0.f;
        heldFor_ = 0.f;
        refuseHeld_ = 0.f;
        refusal_.offer();
        proxyPress_ = 0.f;
        proxyDone_ = false;
        delay_ = 0.f;
        waiting_ = false;
        pool.shown(k, now_);
        scareForm_ = s.scripted && s.flaggedAsScare && pacer ? pacer->hallucination(true, s.scare) : ScareForm::Unpaced;
        emit(HallucinationEventType::Started);
        return HallucinationStart::Started;
    }
    // The same by its parts: no Lone Carry weight and no scare.
    HallucinationStart start(TragedyPool& pool, std::uint32_t& rng, std::uint32_t place, IntensityBand band,
                             bool scripted, int named = -1) {
        HallucinationStartIn s;
        s.place = place;
        s.band = band;
        s.scripted = scripted;
        s.named = named;
        return start(pool, rng, s);
    }

    // One step of `dt` seconds (a negative, NaN or infinite step counts as 0, so the clock stays sound). events()
    // holds what happened in it (and a Started from a start() after it).
    void tick(double dt, const HallucinationInput& in) {
        const bool wasHeld = held();
        rules(dt, in);
        refuseKept_ = in.refuse && (refuseKept_ || wasHeld || held());
    }

    bool active() const { return key_ >= 0; }
    bool held() const { return active() && hold_ >= 1.f; }
    // The refusal button went down while the lead was held and is still down: it is the refusal's until let go.
    bool refuseKept() const { return refuseKept_; }
    int key() const { return key_; }
    IntensityBand band() const { return active() ? band_ : IntensityBand::None; }
    bool bleeds() const { return placeBleeds(band()); } // an open offer of a High band: the host bleeds the place in
    float holdLevel() const { return hold_; } // the host slows the lead by it and stills them at 1
    // An Escrow lead inside an offer, in the wait, with no hold building yet; and the seconds of it left.
    bool escrowWaiting() const { return waiting_; }
    // While paused it holds; 0 before the wait began, once it is over, or with no offer open.
    float escrowWaitLeft() const {
        return active() && delay_ > 0.f && delay_ < tuning_.escrowDelaySeconds ? tuning_.escrowDelaySeconds - delay_ : 0.f;
    }
    // The price. The seconds the boss's weak point stays open from a refusal, counting down (it opened on the step
    // whose Refused left this above 0); the seconds it stays shut from Held, counting down; and Held's health cost,
    // a share of the lead's max health, on the step Held began only (0 on every other).
    float weakPointOpenFor() const { return openFor_; }
    float weakPointShutFor() const { return shutFor_; }
    // The seconds left of the lock after a refusal's window ended (by running out or by Held), counting down.
    float weakPointLockFor() const { return lockFor_; }
    float heldCost() const { return heldCost_; }
    // True on the step the lead's own refusal closed an offer before Held with under refuseOpenMinHold of hold
    // (so it opened nothing for that reason); false on every other step.
    bool refusedTooEarly() const { return tooEarly_; }
    // The weak point time a refusal closing with `hold` seconds of hold built would open (0: none).
    float refusalOpens(float hold) const {
        if (!enoughHold(hold)) return 0.f;
        const float over = std::max(0.f, std::min(hold, tuning_.refuseOpenHoldCap) - tuning_.refuseOpenMinHold);
        return tuning_.refuseOpenBase + tuning_.refuseOpenPerSecond * over;
    }
    // Float rounding only: a sum of steps that should be refuseOpenMinHold may fall a hair short (0.9999 s is not 1).
    bool enoughHold(float hold) const { return hold + 1e-5f >= tuning_.refuseOpenMinHold; }
    ScareForm scareForm() const { return scareForm_; } // the last start's scare beat: Unpaced, not a scare
    float refusalCooldown() const { return cooldown_; }
    double now() const { return now_; }
    const HallucinationEvent* events() const { return events_.data(); }
    int eventCount() const { return eventCount_; }

private:
    void rules(double dt, const HallucinationInput& in) {
        if (!(dt > 0.0 && std::isfinite(dt))) dt = 0.0;
        eventCount_ = 0;
        const bool wasWaiting = waiting_;
        waiting_ = false;
        now_ += dt;
        cooldown_ = std::max(0.f, cooldown_ - static_cast<float>(dt));
        lockFor_ = std::max(0.f, lockFor_ - static_cast<float>(dt));
        if (openFor_ > 0.f && openFor_ <= static_cast<float>(dt)) // the window ends inside this step: lock from then
            lockFor_ = std::max(0.f, tuning_.refuseOpenLockSeconds - (static_cast<float>(dt) - openFor_));
        openFor_ = std::max(0.f, openFor_ - static_cast<float>(dt));
        shutFor_ = std::max(0.f, shutFor_ - static_cast<float>(dt));
        heldCost_ = 0.f;
        tooEarly_ = false;
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
            if (!active() && !cooling) cooldown_ = refusalCooldownFor(in.brief); // a wrong guess
        } else if (!in.refuse && refuseDown_) {
            refusal_.holdUp();
            refuseCounts_ = false;
        }
        refuseDown_ = in.refuse;
        if (!active()) return;
        // The push, at the step's start: blows taken while the hold builds (no Escrow wait running).
        if (!held() && in.accepting && in.blowsTaken > 0 && tuning_.holdPushPerHit > 0.f &&
            !(in.brief == BriefId::Escrow && delay_ < tuning_.escrowDelaySeconds)) {
            const float push = static_cast<float>(std::min(in.blowsTaken, 1000)) * tuning_.holdPushPerHit;
            if (hold_ == 0.f) emit(HallucinationEventType::HoldBegan);
            hold_ = std::min(1.f, hold_ + push / tuning_.holdSeconds);
            built_ = std::min(tuning_.holdSeconds, built_ + push);
            if (holdFull()) becomeHeld(); // Held at the step's start: before any press completes in it
        }
        if (refuseCounts_ && refusal_.holding()) {
            refuseHeld_ += static_cast<float>(dt);
            if (refuseHeld_ + kStepSlack >= tuning_.refuseSeconds) {
                // The press completes on this step: Refused, even when the hold would fill on it too (a tie is
                // Refused's). Held wins only from an earlier step, or from a push at this step's start (above).
                refusal_.offerEnded();
                cooldown_ = refusalCooldownFor(in.brief);
                // The hold to the closing instant: this step's real hold counts too (it never reaches Held now).
                const float hold = std::min(tuning_.holdSeconds, built_ + stepBuild(dt, in));
                tooEarly_ = !held() && !enoughHold(hold);
                constexpr float kLockSlack = 1e-4f; // a lock summed from steps may be a hair over 0 at its end
                if (!held() && !(shutFor_ > 0.f) && !(lockFor_ > kLockSlack))
                    openFor_ = std::max(openFor_, refusalOpens(hold)); // an open window keeps the longer
                end(HallucinationEventType::Refused);
                return;
            }
        }

        if (held()) {
            heldFor_ += static_cast<float>(dt);
            if (proxyRefuses(dt, in)) return;
            if (in.partnerPresent && (in.partnerPull || (in.inFight && heldFor_ >= tuning_.pullSeconds)))
                end(HallucinationEventType::PulledBack);
            return;
        }
        if (in.accepting) {
            float build = static_cast<float>(dt);
            bool waiting = false;
            if (in.brief == BriefId::Escrow && delay_ < tuning_.escrowDelaySeconds) { // the wait comes first
                const float wait = std::min(build, tuning_.escrowDelaySeconds - delay_);
                delay_ += wait;
                build -= wait;
                waiting = !(build > 0.f);
                waiting_ = delay_ < tuning_.escrowDelaySeconds && (dt > 0.0 || wasWaiting);
            }
            if (!waiting) {
                if (hold_ == 0.f) emit(HallucinationEventType::HoldBegan);
                hold_ = std::min(1.f, hold_ + build / tuning_.holdSeconds);
                built_ = std::min(tuning_.holdSeconds, built_ + build);
                if (holdFull()) {
                    becomeHeld();
                    return;
                }
            }
        } else {
            hold_ = 0.f;
            built_ = 0.f; // the hold drops; the Escrow wait (delay_) pauses where it is
            if (in.turnedAway) {
                end(HallucinationEventType::Refused);
                return;
            }
        }
        if (now_ - shownAt_ >= tuning_.expireSeconds) end(HallucinationEventType::Expired);
    }

    // A Proxy partner's refusal for the held lead; true once it has closed the offer.
    bool proxyRefuses(double dt, const HallucinationInput& in) {
        if (proxyDone_) return false;
        if (!in.partnerPresent || in.partnerBrief != BriefId::Proxy || !in.partnerAble) {
            proxyPress_ = 0.f; // any break drops a press under way
            return false;
        }
        if (heldFor_ < tuning_.proxyReactSeconds) return false;
        // This step's share from the instant the press could start: the react time's crossing, or the step's start.
        proxyPress_ += std::min(static_cast<float>(dt), heldFor_ - tuning_.proxyReactSeconds);
        if (proxyPress_ + kStepSlack < tuning_.refuseSeconds) return false;
        proxyDone_ = true;
        refusal_.offerEnded();
        cooldown_ = refusalCooldownFor(in.brief);
        end(HallucinationEventType::Refused);
        return true;
    }
    // Float rounding only: a press or a hold summed from steps (1/30 s, say) may land a hair short of its length;
    // within kStepSlack it is complete, so an exact tie stays a tie at any step size.
    static constexpr float kStepSlack = 1e-4f;
    bool holdFull() const { return hold_ >= 1.f || built_ + kStepSlack >= tuning_.holdSeconds; }
    void becomeHeld() {
        // A press of the lead's own under way is spent: a new press is needed to refuse the held lead.
        refuseCounts_ = false;
        refuseHeld_ = 0.f;
        hold_ = 1.f;
        heldFor_ = 0.f;
        heldCost_ = tuning_.heldHealthCost; // the price of a full hold, once
        shutFor_ = tuning_.heldShutSeconds;
        if (openFor_ > 0.f) lockFor_ = tuning_.refuseOpenLockSeconds; // an open window ends here
        openFor_ = 0.f;
        emit(HallucinationEventType::Held);
    }
    // The real hold this step builds for a lead inside the offer (an Escrow lead's wait taken first); 0 outside it.
    float stepBuild(double dt, const HallucinationInput& in) const {
        if (!in.accepting) return 0.f;
        const float d = static_cast<float>(dt);
        if (in.brief == BriefId::Escrow && delay_ < tuning_.escrowDelaySeconds)
            return std::max(0.f, d - (tuning_.escrowDelaySeconds - delay_));
        return d;
    }
    // The refusal cooldown for a lead of Brief `b`: Counsel cuts it, never lengthens it.
    float refusalCooldownFor(BriefId b) const {
        return b == BriefId::Counsel ? std::min(tuning_.refusalCooldownSeconds, tuning_.counselCooldownSeconds)
                                     : tuning_.refusalCooldownSeconds;
    }
    void emit(HallucinationEventType type) {
        if (eventCount_ < static_cast<int>(events_.size())) events_[static_cast<std::size_t>(eventCount_++)] = {type, key_};
    }
    void end(HallucinationEventType why) {
        emit(why);
        key_ = -1;
        hold_ = 0.f;
        built_ = 0.f;
        delay_ = 0.f;
        waiting_ = false;
        proxyPress_ = 0.f;
        proxyDone_ = false;
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
    float built_ = 0.f; // seconds of real hold built in this offer (hold_ in seconds, summed exactly)
    float openFor_ = 0.f;
    float shutFor_ = 0.f;
    float lockFor_ = 0.f;
    bool tooEarly_ = false;
    float heldCost_ = 0.f;
    float heldFor_ = 0.f;
    float refuseHeld_ = 0.f;
    float cooldown_ = 0.f;
    float proxyPress_ = 0.f;  // a Proxy partner's press so far
    float delay_ = 0.f;       // an Escrow lead's wait inside this offer so far
    bool proxyDone_ = false;
    bool waiting_ = false; // escrowWaiting()
    bool refuseDown_ = false;
    bool refuseCounts_ = false; // this hold was pressed on an open offer, not while cooling
    bool refuseKept_ = false;
    ScareForm scareForm_ = ScareForm::Unpaced;
};

} // namespace ftcore
