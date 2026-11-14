// Down, takeover, defeat, checkpoints and retry in the fight session (FightSession.h).
#include "ftcore/FightSession.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

using namespace ftcore;

namespace {

constexpr double kFrame = 1.0 / 60.0;
constexpr std::uint8_t kBitA = 1u << kLeadA;
constexpr std::uint8_t kBitB = 1u << kLeadB;

GiftTuning freeGifts() {
    GiftTuning g;
    g.cooldown = {0.0, 0.0, 0.0};
    g.healsPerFight = 20;
    return g;
}

// A two-phase boss: phase 1 enters at half hp.
BossScriptDef twoPhaseBoss(bool checkpointAtPhase) {
    BossScriptDef d;
    d.setId("two");
    d.hp = 100;
    d.restSeconds = 1.0f;
    d.lock.keys = kUnlockSharedBlow;
    d.lock.openSeconds = 2.0;
    d.moves[0].setId("jab");
    d.moves[0].counter = BossCounter::Guard;
    d.moves[0].tell = 1.0f;
    d.moves[0].window = 0.5f;
    d.moves[0].recover = 1.0f;
    d.moves[0].exposeSeconds = 2.0f;
    d.moveCount = 1;
    d.offer.setId("offer");
    d.offer.counter = BossCounter::Answer;
    d.offer.tell = 1.0f;
    d.offer.window = 2.0f;
    d.offer.needsAnswer = true;
    d.phases[0].moves = {0};
    d.phases[0].moveCount = 1;
    d.phases[1].enterAt = 0.5f;
    d.phases[1].moves = {0};
    d.phases[1].moveCount = 1;
    d.phaseCount = 2;
    d.checkpointAtPhase = checkpointAtPhase;
    return d;
}

} // namespace

TEST_CASE("fight fate: the controlled lead's fall hands control to the standing lead, once, never back by itself") {
    FightSession s;
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    REQUIRE(s.setActiveLead(kLeadA));
    s.tick(kFrame);
    s.setLeadHealth(kLeadA, 0.4f);
    CHECK_FALSE(s.down(kLeadA));
    s.setLeadHealth(kLeadA, 0.0f);
    CHECK(s.down(kLeadA));
    CHECK(s.activeLead() == kLeadA); // until the step closes
    const FightTick& t = s.tick(kFrame);
    CHECK(s.activeLead() == kLeadB);
    CHECK(t.leadsFell == kBitA);
    CHECK(t.tookOver == kLeadB);
    CHECK_FALSE(t.defeated);
    CHECK_FALSE(s.defeated());
    // Once: a second 0 is no new fall, and the next tick is quiet.
    s.setLeadHealth(kLeadA, 0.0f);
    const FightTick& q = s.tick(kFrame);
    CHECK(q.leadsFell == 0);
    CHECK(q.tookOver == kNoFighter);
    // The player cannot switch to a lead that is down: asked, then refused at the close.
    CHECK(s.setActiveLead(kLeadA));
    const FightTick& no = s.tick(kFrame);
    CHECK(no.switchRefused == kLeadA);
    CHECK(no.switchRefusal == SwitchRefusal::Down);
    CHECK(no.switched == kNoFighter);
    CHECK(s.activeLead() == kLeadB);
    // It stands again: control stays where it went.
    s.setLeadHealth(kLeadA, 0.25f);
    CHECK_FALSE(s.down(kLeadA));
    const FightTick& r = s.tick(kFrame);
    CHECK(r.leadsRose == kBitA);
    CHECK(r.leadsFell == 0);
    CHECK(s.activeLead() == kLeadB);
    CHECK(s.tick(kFrame).leadsRose == 0);
    // Only the player's switch moves it back, at the step's close.
    CHECK(s.setActiveLead(kLeadA));
    CHECK(s.activeLead() == kLeadB);
    const FightTick& back = s.tick(kFrame);
    CHECK(back.switched == kLeadA);
    CHECK(back.switchRefused == kNoFighter);
    CHECK(s.activeLead() == kLeadA);
    CHECK(s.tick(kFrame).switched == kNoFighter); // once
}

TEST_CASE("fight fate: the partner's fall changes no control; both down is a defeat, reported once") {
    FightSession s;
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    REQUIRE(s.setActiveLead(kLeadA));
    s.setLeadHealth(kLeadB, -0.3f); // an overkill under 0 is down too
    CHECK(s.down(kLeadB));
    CHECK(s.leadHealth(kLeadB) == kHealthUnknown); // stored as unknown for the gift Heal
    const FightTick& t = s.tick(kFrame);
    CHECK(t.leadsFell == kBitB);
    CHECK(t.tookOver == kNoFighter);
    CHECK(s.activeLead() == kLeadA);
    CHECK_FALSE(t.defeated);
    // The last standing lead falls: a defeat.
    s.setLeadHealth(kLeadA, 0.0f);
    CHECK_FALSE(s.defeated()); // at the step's close
    CHECK(s.activeLead() == kLeadA); // nobody to take over
    const FightTick& d = s.tick(kFrame);
    CHECK(d.defeated);
    CHECK(d.leadsFell == kBitA);
    CHECK(d.tookOver == kNoFighter);
    CHECK_FALSE(s.tick(kFrame).defeated); // once
    CHECK(s.defeated());
    // After a defeat nothing falls or stands until a retry or a begin.
    s.setLeadHealth(kLeadB, 0.5f);
    CHECK(s.down(kLeadB));
    CHECK(s.tick(kFrame).leadsRose == 0);
    CHECK(s.leadHealth(kLeadB) == 0.5f); // still stored for the gift Heal
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    CHECK_FALSE(s.defeated());
    CHECK_FALSE(s.down(kLeadA));
    CHECK_FALSE(s.down(kLeadB));
}

namespace {

// What one closed step showed.
struct Closed {
    std::uint8_t fell = 0, rose = 0;
    FighterId tookOver = kNoFighter, active = kNoFighter, switched = kNoFighter, switchRefused = kNoFighter;
    SwitchRefusal refusal = SwitchRefusal::None;
    bool defeated = false, downA = false, downB = false;
    std::uint64_t hash = 0;
    bool operator==(const Closed&) const = default;
};
// One call in a step: a health feed, (sw) the player's switch to that lead, or a checkpoint between ticks.
enum class Cp : std::uint8_t { None, Phase, Host };
struct Feed {
    FighterId lead = kNoFighter;
    float health = 0.f;
    bool sw = false;
    Cp cp = Cp::None;
};
Feed switchTo(FighterId lead) { return Feed{lead, 0.f, true}; }
Feed checkpointBy(Cp how) { return Feed{kNoFighter, 0.f, false, how}; }
// A fight with kLeadA controlled (kLeadB down first when bDown), then one step's calls in the order given, closed
// by a tick.
Closed closeStep(bool bDown, std::initializer_list<Feed> feeds) {
    FightSession s;
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    REQUIRE(s.setActiveLead(kLeadA));
    s.tick(kFrame);
    if (bDown) {
        s.setLeadHealth(kLeadB, 0.f);
        REQUIRE(s.tick(kFrame).leadsFell == kBitB);
    }
    bool cp = false;
    for (const Feed& f : feeds) {
        if (f.cp == Cp::Phase) {
            REQUIRE(s.checkpointPhase(1));
            cp = true;
        } else if (f.cp == Cp::Host) {
            REQUIRE(s.setHostCheckpoint(FightHostCheckpoint{}) == CheckpointAt::FightStart); // the start's part
            cp = true;
        } else if (f.sw) {
            REQUIRE(s.setActiveLead(f.lead));
        } else {
            s.setLeadHealth(f.lead, f.health);
        }
    }
    if (!cp) CHECK_FALSE(s.defeated()); // nothing settles before the step closes (a checkpoint settles falls)
    CHECK(s.activeLead() == kLeadA);    // a switch never resolves before the tick, checkpoint or not
    const FightTick& t = s.tick(kFrame);
    Closed c;
    c.fell = t.leadsFell;
    c.rose = t.leadsRose;
    c.tookOver = t.tookOver;
    c.switched = t.switched;
    c.switchRefused = t.switchRefused;
    c.refusal = t.switchRefusal;
    c.defeated = t.defeated;
    c.active = s.activeLead();
    c.downA = s.down(kLeadA);
    c.downB = s.down(kLeadB);
    c.hash = s.stateHash();
    return c;
}

} // namespace

TEST_CASE("fight fate: both leads falling on one step is a defeat, no takeover, control unchanged, in either order") {
    const Closed ab = closeStep(false, {{kLeadA, 0.f}, {kLeadB, 0.f}});
    const Closed ba = closeStep(false, {{kLeadB, 0.f}, {kLeadA, 0.f}});
    CHECK(ab == ba);
    CHECK(ab.fell == (kBitA | kBitB));
    CHECK(ab.rose == 0);
    CHECK(ab.tookOver == kNoFighter);
    CHECK(ab.active == kLeadA);
    CHECK(ab.defeated);
}

TEST_CASE("fight fate: one lead falls on the step the other stands: control to the one that stood, no defeat") {
    const Closed ab = closeStep(true, {{kLeadA, 0.f}, {kLeadB, 0.25f}});
    const Closed ba = closeStep(true, {{kLeadB, 0.25f}, {kLeadA, 0.f}});
    CHECK(ab == ba);
    CHECK(ab.fell == kBitA);
    CHECK(ab.rose == kBitB);
    CHECK(ab.tookOver == kLeadB);
    CHECK(ab.active == kLeadB);
    CHECK_FALSE(ab.defeated);
    CHECK(ab.downA);
    CHECK_FALSE(ab.downB);
}

TEST_CASE("fight fate: a switch to a lead who stands up on the same step is honoured, in either order") {
    const Closed sr = closeStep(true, {switchTo(kLeadB), {kLeadB, 0.25f}});
    const Closed rs = closeStep(true, {{kLeadB, 0.25f}, switchTo(kLeadB)});
    CHECK(sr == rs);
    CHECK(sr.rose == kBitB);
    CHECK(sr.fell == 0);
    CHECK(sr.switched == kLeadB);
    CHECK(sr.switchRefused == kNoFighter);
    CHECK(sr.tookOver == kNoFighter);
    CHECK(sr.active == kLeadB);
}

TEST_CASE("fight fate: a switch to a lead who falls on the same step is refused Down, in either order") {
    const Closed sf = closeStep(false, {switchTo(kLeadB), {kLeadB, 0.f}});
    const Closed fs = closeStep(false, {{kLeadB, 0.f}, switchTo(kLeadB)});
    CHECK(sf == fs);
    CHECK(sf.fell == kBitB);
    CHECK(sf.switched == kNoFighter);
    CHECK(sf.switchRefused == kLeadB);
    CHECK(sf.refusal == SwitchRefusal::Down);
    CHECK(sf.tookOver == kNoFighter);
    CHECK(sf.active == kLeadA);
    CHECK_FALSE(sf.defeated);
}

TEST_CASE("fight fate: a switch on a quiet step, and beside the controlled lead's fall, in either order") {
    const Closed quiet = closeStep(false, {switchTo(kLeadB)});
    CHECK(quiet.switched == kLeadB);
    CHECK(quiet.active == kLeadB);
    CHECK(quiet.tookOver == kNoFighter);
    // To the other lead as the controlled one falls: the takeover gives it control, so the switch reports nothing.
    const Closed sf = closeStep(false, {switchTo(kLeadB), {kLeadA, 0.f}});
    const Closed fs = closeStep(false, {{kLeadA, 0.f}, switchTo(kLeadB)});
    CHECK(sf == fs);
    CHECK(sf.tookOver == kLeadB);
    CHECK(sf.switched == kNoFighter);
    CHECK(sf.switchRefused == kNoFighter);
    CHECK(sf.active == kLeadB);
    // To the controlled lead as it falls: the takeover stands, the switch is refused Down.
    const Closed kf = closeStep(false, {switchTo(kLeadA), {kLeadA, 0.f}});
    const Closed fk = closeStep(false, {{kLeadA, 0.f}, switchTo(kLeadA)});
    CHECK(kf == fk);
    CHECK(kf.tookOver == kLeadB);
    CHECK(kf.switchRefused == kLeadA);
    CHECK(kf.refusal == SwitchRefusal::Down);
    CHECK(kf.active == kLeadB);
    // On a defeat: refused Defeated, control unchanged.
    const Closed d1 = closeStep(false, {switchTo(kLeadB), {kLeadA, 0.f}, {kLeadB, 0.f}});
    const Closed d2 = closeStep(false, {{kLeadB, 0.f}, {kLeadA, 0.f}, switchTo(kLeadB)});
    CHECK(d1 == d2);
    CHECK(d1.defeated);
    CHECK(d1.switchRefused == kLeadB);
    CHECK(d1.refusal == SwitchRefusal::Defeated);
    CHECK(d1.tookOver == kNoFighter);
    CHECK(d1.active == kLeadA);
}

TEST_CASE("fight fate: a checkpoint between ticks never resolves the switch: one to a lead who falls in that step is refused at the close, whatever the order") {
    using L = std::initializer_list<Feed>;
    const Feed sw = switchTo(kLeadB);
    const Feed fall{kLeadB, 0.f};
    for (const Cp how : {Cp::Phase, Cp::Host}) {
        const Feed cp = checkpointBy(how);
        const L orders[] = {{sw, cp, fall}, {sw, fall, cp}, {cp, sw, fall}, {fall, cp, sw}, {fall, sw, cp},
                            {cp, fall, sw}};
        for (const L& o : orders) {
            const Closed c = closeStep(false, o);
            CHECK(c.fell == kBitB);
            CHECK(c.switched == kNoFighter);
            CHECK(c.switchRefused == kLeadB);
            CHECK(c.refusal == SwitchRefusal::Down);
            CHECK(c.tookOver == kNoFighter);
            CHECK(c.active == kLeadA);
            CHECK_FALSE(c.defeated);
            CHECK(c.downB);
        }
    }
    // The request still waits after the checkpoint, and the close reports it once.
    FightSession s;
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    s.tick(kFrame);
    REQUIRE(s.setActiveLead(kLeadB));
    REQUIRE(s.checkpointPhase(1));
    CHECK(s.switchRequested() == kLeadB);
    CHECK(s.activeLead() == kLeadA);
    CHECK(s.tick(kFrame).switched == kLeadB);
    CHECK(s.switchRequested() == kNoFighter);
    CHECK(s.tick(kFrame).switched == kNoFighter);
}

TEST_CASE("fight fate: the start checkpoint the host writes at set-up never resolves the switch, whatever the order") {
    using L = std::initializer_list<Feed>;
    const Feed sw = switchTo(kLeadB);
    const Feed fall{kLeadB, 0.f};
    const Feed cp = checkpointBy(Cp::Host);
    const L orders[] = {{sw, cp, fall}, {sw, fall, cp}, {cp, sw, fall}, {fall, cp, sw}, {fall, sw, cp}, {cp, fall, sw}};
    for (const L& o : orders) {
        FightSession s;
        REQUIRE(s.begin(FightSessionDef{}, 0.0).ok()); // no tick: the start checkpoint is still due
        for (const Feed& f : o) {
            if (f.cp == Cp::Host)
                REQUIRE(s.setHostCheckpoint(FightHostCheckpoint{}) == CheckpointAt::FightStart);
            else if (f.sw)
                REQUIRE(s.setActiveLead(f.lead));
            else
                s.setLeadHealth(f.lead, f.health);
        }
        CHECK(s.activeLead() == kLeadA);
        const FightTick& t = s.tick(kFrame);
        CHECK(t.leadsFell == kBitB);
        CHECK(t.switched == kNoFighter);
        CHECK(t.switchRefused == kLeadB);
        CHECK(t.switchRefusal == SwitchRefusal::Down);
        CHECK(t.tookOver == kNoFighter);
        CHECK(s.activeLead() == kLeadA);
        CHECK_FALSE(s.defeated());
    }
}

TEST_CASE("fight fate: a retry opens on the switch its checkpoint carried, if that lead stands there") {
    // The same lead after a retry whether the host's write or the first tick took the start checkpoint.
    for (const bool hostFirst : {true, false}) {
        CAPTURE(hostFirst);
        FightSession s;
        REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
        REQUIRE(s.setActiveLead(kLeadB));
        if (hostFirst) REQUIRE(s.setHostCheckpoint(FightHostCheckpoint{}) == CheckpointAt::FightStart);
        CHECK(s.tick(kFrame).switched == kLeadB);
        REQUIRE(s.setActiveLead(kLeadA));
        s.tick(kFrame);
        REQUIRE(s.activeLead() == kLeadA);
        CHECK(s.retry().at == CheckpointAt::FightStart);
        CHECK(s.activeLead() == kLeadB);
        CHECK(s.switchRequested() == kNoFighter);
        CHECK(s.tick(kFrame).switched == kNoFighter); // applied by the retry: no event
    }
    // Waiting at a phase checkpoint: the retry opens on it.
    {
        FightSession s;
        REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
        s.tick(kFrame);
        REQUIRE(s.setActiveLead(kLeadB));
        REQUIRE(s.checkpointPhase(1));
        CHECK(s.checkpoint().activeLead == kLeadA);
        CHECK(s.checkpoint().switchRequested == kLeadB);
        CHECK(s.tick(kFrame).switched == kLeadB);
        REQUIRE(s.setActiveLead(kLeadA));
        s.tick(kFrame);
        CHECK(s.retry().at == CheckpointAt::Phase);
        CHECK(s.activeLead() == kLeadB);
    }
    // For a lead down in the checkpoint: dropped.
    {
        FightSession s;
        REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
        s.tick(kFrame);
        s.setLeadHealth(kLeadB, 0.f);
        REQUIRE(s.setActiveLead(kLeadB));
        REQUIRE(s.checkpointPhase(1));
        CHECK(s.checkpoint().down[kLeadB]);
        CHECK(s.checkpoint().switchRequested == kLeadB);
        CHECK(s.tick(kFrame).switchRefused == kLeadB);
        CHECK(s.retry().at == CheckpointAt::Phase);
        CHECK(s.activeLead() == kLeadA);
        CHECK(s.switchRequested() == kNoFighter);
        const FightTick& t = s.tick(kFrame);
        CHECK(t.switched == kNoFighter);
        CHECK(t.switchRefused == kNoFighter);
    }
}

TEST_CASE("fight fate: of two switch requests in one step the last wins, and the earlier raises no event") {
    const Closed c = closeStep(false, {switchTo(kLeadB), switchTo(kLeadA)});
    CHECK(c.switched == kNoFighter); // the last asked for the lead already in control
    CHECK(c.switchRefused == kNoFighter);
    CHECK(c.active == kLeadA);
    const Closed d = closeStep(true, {switchTo(kLeadB), switchTo(kLeadA)}); // the earlier would be refused Down
    CHECK(d.switchRefused == kNoFighter);
    CHECK(d.refusal == SwitchRefusal::None);
    CHECK(d.active == kLeadA);
}

TEST_CASE("fight fate: a lead that falls and stands again within one step does neither; the partner's fall moves nothing") {
    const Closed up = closeStep(false, {{kLeadA, 0.f}, {kLeadA, 0.5f}});
    CHECK(up.fell == 0);
    CHECK(up.rose == 0);
    CHECK(up.tookOver == kNoFighter);
    CHECK(up.active == kLeadA);
    CHECK_FALSE(up.downA);
    const Closed back = closeStep(true, {{kLeadB, 0.3f}, {kLeadB, 0.f}});
    CHECK(back.fell == 0);
    CHECK(back.rose == 0);
    CHECK(back.downB);
    CHECK_FALSE(back.defeated);
    const Closed partner = closeStep(false, {{kLeadB, 0.f}});
    CHECK(partner.fell == kBitB);
    CHECK(partner.tookOver == kNoFighter);
    CHECK(partner.active == kLeadA);
    // Where the end state differs, so does the hash.
    CHECK(up.hash != partner.hash);
}

TEST_CASE("fight fate: a checkpoint between ticks closes the step: it holds the settled fall and takeover") {
    FightSession s;
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    REQUIRE(s.setActiveLead(kLeadA));
    s.tick(kFrame);
    s.setLeadHealth(kLeadA, 0.f);
    CHECK(s.activeLead() == kLeadA);
    REQUIRE(s.checkpointPhase(1));
    CHECK(s.activeLead() == kLeadB);
    CHECK(s.checkpoint().down[kLeadA]);
    CHECK(s.checkpoint().activeLead == kLeadB);
    const FightTick& t = s.tick(kFrame);
    CHECK(t.leadsFell == kBitA);
    CHECK(t.tookOver == kLeadB);
    CHECK(t.checkpoint == CheckpointAt::Phase);
    const FightTick& q = s.tick(kFrame);
    CHECK(q.leadsFell == 0);
    CHECK(q.tookOver == kNoFighter);
}

TEST_CASE("fight fate: NaN and infinities change nothing; above 0 stands; non-leads are ignored") {
    FightSession s;
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (const float v : {nan, inf, -inf}) {
        s.setLeadHealth(kLeadA, v);
        CHECK_FALSE(s.down(kLeadA));
    }
    s.setLeadHealth(kLeadA, 0.0f);
    REQUIRE(s.down(kLeadA));
    for (const float v : {nan, inf, -inf}) {
        s.setLeadHealth(kLeadA, v);
        CHECK(s.down(kLeadA));
    }
    s.setLeadHealth(kLeadA, 1.5f); // over 1: unknown for the Heal, but standing
    CHECK_FALSE(s.down(kLeadA));
    s.setLeadHealth(kThirdSlot, 0.0f);
    CHECK_FALSE(s.down(kThirdSlot));
    CHECK_FALSE(s.inFight(kThirdSlot));
    CHECK_FALSE(s.defeated());
}

TEST_CASE("fight fate: a fight with one lead: the other is not in it, and its lead's fall is a defeat") {
    FightSession s;
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    REQUIRE(s.setActiveLead(kLeadB));
    FightSessionDef d;
    d.inFight = {true, false};
    REQUIRE(s.begin(d, 0.0).ok());
    CHECK(s.inFight(kLeadA));
    CHECK_FALSE(s.inFight(kLeadB));
    CHECK(s.activeLead() == kLeadA); // moved off the lead not in the fight
    CHECK_FALSE(s.setActiveLead(kLeadB));
    s.setLeadHealth(kLeadB, 0.0f); // not in the fight: no fall
    CHECK_FALSE(s.down(kLeadB));
    CHECK(s.tick(kFrame).leadsFell == 0);
    s.setLeadHealth(kLeadA, 0.0f);
    const FightTick& t = s.tick(kFrame);
    CHECK(t.leadsFell == kBitA);
    CHECK(t.defeated);
    CHECK(t.tookOver == kNoFighter);
    // No lead in the fight is a refused def; the session then has both.
    d.inFight = {false, false};
    const FightSessionCheck c = s.begin(d, 0.0);
    CHECK_FALSE(c.party);
    CHECK_FALSE(c.ok());
    CHECK(s.inFight(kLeadA));
    CHECK(s.inFight(kLeadB));
}

TEST_CASE("fight fate: the fallen lead stands by itself through the partner's brain after 30 s at 25%") {
    // The host's loop: the brain drives the body the player does not, which a fallen lead always is.
    FightSession s;
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    REQUIRE(s.setActiveLead(kLeadA));
    PartnerBrain brain;
    const PartnerTuning tuning;
    REQUIRE(tuning.downSeconds == 30.f);
    REQUIRE(tuning.riseHealth == 0.25f);
    REQUIRE(brain.begin(tuning, 5));
    std::array<float, 2> hp{1.f, 1.f};
    hp[kLeadA] = 0.f;
    s.setLeadHealth(kLeadA, hp[kLeadA]);
    REQUIRE(s.tick(kFrame).tookOver == kLeadB);
    int rose = 0;
    double roseAt = -1.0;
    for (int i = 0; i < 40 * 60; ++i) {
        const FighterId body = s.activeLead() == kLeadA ? kLeadB : kLeadA;
        PartnerInput in;
        in.health = hp[body];
        in.canAct = !s.down(body);
        const PartnerIntent& it = brain.step(kFrame, in);
        if (it.rise) {
            hp[body] = it.riseHealth;
            s.setLeadHealth(body, hp[body]);
        }
        const FightTick& t = s.tick(kFrame);
        if (t.leadsRose != 0) {
            CHECK(t.leadsRose == kBitA);
            ++rose;
            roseAt = s.now();
        }
        REQUIRE(s.activeLead() == kLeadB);
    }
    CHECK(rose == 1);
    CHECK(roseAt >= 30.0);
    CHECK(roseAt <= 30.0 + 3 * kFrame);
    CHECK(s.leadHealth(kLeadA) == 0.25f);
    CHECK_FALSE(s.down(kLeadA));
    CHECK(s.activeLead() == kLeadB);
}

TEST_CASE("fight fate: the fight-start checkpoint is taken once, at the host's write or the first tick") {
    FightSession s;
    FightSessionDef d;
    REQUIRE(s.begin(d, 3.0).ok());
    CHECK(s.checkpoint().at == CheckpointAt::None);
    const FightTick& t = s.tick(kFrame);
    CHECK(t.checkpoint == CheckpointAt::FightStart);
    CHECK(s.checkpoint().at == CheckpointAt::FightStart);
    CHECK(s.checkpoint().now == 3.0); // before the first step
    CHECK(s.tick(kFrame).checkpoint == CheckpointAt::None);
    // The host's write after set-up takes it, and the tick then does not ask again.
    REQUIRE(s.begin(d, 5.0).ok());
    REQUIRE(s.setActiveLead(kLeadB));
    s.setLeadHealth(kLeadA, 0.5f);
    FightHostCheckpoint host;
    host.hp = {50, 100};
    host.currency = 12;
    s.setHostCheckpoint(host);
    CHECK(s.checkpoint().at == CheckpointAt::FightStart);
    CHECK(s.checkpoint().activeLead == kLeadA);      // the set-up's switch waits for the first tick
    CHECK(s.checkpoint().switchRequested == kLeadB); // and the checkpoint carries it
    CHECK(s.checkpoint().leadHealth[kLeadA] == 0.5f);
    CHECK(s.checkpoint().host.currency == 12);
    CHECK(s.tick(kFrame).checkpoint == CheckpointAt::None);
    CHECK(s.checkpoint().host.hp[kLeadA] == 50);
    // A retry before any checkpoint is a begin again at the fight's start.
    REQUIRE(s.begin(d, 7.0).ok());
    s.tick(kFrame);
    REQUIRE(s.begin(d, 7.0).ok());
    s.setLeadHealth(kLeadA, 0.0f);
    const FightCheckpoint& none = s.retry();
    CHECK(none.at == CheckpointAt::None);
    CHECK(s.now() == 7.0);
    CHECK_FALSE(s.down(kLeadA));
    CHECK(s.tick(kFrame).checkpoint == CheckpointAt::FightStart);
}

TEST_CASE("fight fate: a retry puts back each checkpoint field and starts the rest clean; a defeat costs nothing") {
    FightSession s;
    FightSessionDef d;
    d.gift = freeGifts();
    d.gift.guardSeconds = 0.0;
    d.lift = true;
    d.keptLead = kLeadA;
    d.loneLead = kLeadB;
    // A fight won with one gift: the tally the next fight starts from (a gift ends the Lone Carry, so not in it).
    REQUIRE(s.begin(d, 0.0).ok());
    REQUIRE(s.selectGift(Gift::Heal));
    REQUIRE(s.giveGift(kLeadA).given);
    s.finish(true);
    REQUIRE(s.begin(d, 2.0).ok());
    REQUIRE(s.lone());
    REQUIRE(s.giftSlot().allPresses() == 1);
    REQUIRE(s.setActiveLead(kLeadB));
    REQUIRE(s.layPlace(kLeadA, 5) == PlaceError::None);
    s.keptParried(40);
    const int keptAt = s.keptLoad();
    s.setLeadHealth(kLeadA, 0.6f);
    FightHostCheckpoint host;
    host.hp = {60, 100};
    host.load = {0, 7};
    host.draws = 99;
    host.order = PartnerOrder::Spread;
    host.currency = 250;
    s.setHostCheckpoint(host);
    const std::uint64_t pressesAt = s.giftSlot().allPresses();
    REQUIRE(s.tick(kFrame).loneCarryBegan);

    // The attempt dirties every piece, then is lost.
    REQUIRE(s.setActiveLead(kLeadA));
    REQUIRE(s.poolRelease(kLeadB, 3));
    REQUIRE(s.endLone(LoneEnd::PartnerCalled));
    REQUIRE(s.selectGift(Gift::Heal));
    REQUIRE(s.giveGift(kLeadA).given);
    REQUIRE(s.selectGift(Gift::Guard));
    REQUIRE(s.giveGift(kLeadB).given);
    REQUIRE(s.layPlace(kLeadB, 9) == PlaceError::None);
    s.keptParried(40);
    s.setKeptDown(true);
    REQUIRE(s.callCount(kLeadA) == CountError::None);
    REQUIRE(s.markEnemy(1, 0.5f, 10.0));
    for (int i = 0; i < 90; ++i) s.tick(kFrame);
    s.setLeadHealth(kLeadA, 0.0f);
    s.setLeadHealth(kLeadB, 0.0f);
    REQUIRE(s.tick(kFrame).defeated);
    REQUIRE(s.giftSlot().allPresses() > pressesAt);

    const FightCheckpoint& cp = s.retry();
    CHECK(cp.at == CheckpointAt::FightStart);
    CHECK(s.now() == 2.0);
    CHECK_FALSE(s.defeated());
    CHECK_FALSE(s.down(kLeadA));
    CHECK_FALSE(s.down(kLeadB));
    CHECK(s.activeLead() == kLeadB);
    CHECK(s.leadHealth(kLeadA) == 0.6f);
    CHECK(s.leadHealth(kLeadB) == kHealthUnknown);
    // Nothing lost but the attempt: the gift tally, the Load and (the host's part) the currency.
    CHECK(s.giftSlot().allPresses() == pressesAt);
    CHECK(s.giftSlot().tally().givenOf(Gift::Heal) == 1);
    CHECK(s.giftSlot().tally().givenOf(Gift::Guard) == 0);
    CHECK(s.keptLoad() == keptAt);
    CHECK_FALSE(s.keptDown());
    CHECK(cp.host.currency == 250);
    CHECK(cp.host.hp[kLeadA] == 60);
    CHECK(cp.host.load[kLeadB] == 7);
    CHECK(cp.host.draws == 99);
    CHECK(cp.host.order == PartnerOrder::Spread);
    CHECK(s.places(kLeadA).holding(5));
    CHECK_FALSE(s.places(kLeadB).holding(9));
    CHECK(s.lone());
    CHECK_FALSE(s.loneEnded());
    CHECK(s.loneEndedBy() == LoneEnd::None);
    CHECK(s.lonePooled() == 0);
    CHECK(s.loneReleases() == 0);
    // Clean: no count window, no mark; the Lone Carry is shown again on the next tick, as at a begin.
    CHECK_FALSE(s.strikeCount().open(s.now() + 0.5));
    CHECK(s.mark(1).bonus == 0.f);
    const FightTick& t = s.tick(kFrame);
    CHECK(t.loneCarryBegan);
    CHECK_FALSE(t.defeated);
    CHECK(t.leadsFell == 0);
    CHECK(t.checkpoint == CheckpointAt::None); // the checkpoint stays: no new one taken
    // A second loss goes back to the same checkpoint.
    REQUIRE(s.selectGift(Gift::Heal));
    REQUIRE(s.giveGift(kLeadA).given);
    s.retry();
    CHECK(s.giftSlot().allPresses() == pressesAt);
    CHECK(s.now() == 2.0);
}

namespace {

// One frame's input to the replay host.
struct Act {
    enum class Kind : std::uint8_t { Switch, Heal, Guard, Count, Held, Mark, Hit, EndLone, Pool, Parry, Place, BossHit, Guard2 };
    int frame;
    Kind kind;
    FighterId lead;
    int amount = 0;
};

// What a frame showed: every tick output and query a host reads.
struct Seen {
    double now;
    std::uint8_t fell, rose;
    FighterId tookOver, active;
    bool defeated, downA, downB, loneBegan, countOpen;
    LoneEnd loneEnd;
    CheckpointAt cp;
    std::size_t bonuses;
    int heldExpired;
    std::uint64_t presses;
    int hpA, hpB, keptLoad;
    bool given;
    GiftError giftError;
    CountError countError;
    PartnerAct act;
    PartnerWhy why;
    bool newString, rise;
    std::uint32_t bossEvents = 0; // bit per BossEventType on this tick
    std::size_t bossPhase = 0;
    int bossHp = 0;
    bool weakOpen = false, lone = false, loneEnded = false;
    std::uint32_t pooled = 0, releases = 0;
    std::uint64_t sessionHash = 0, brainHash = 0; // every play field of the session and of the brain
    bool operator==(const Seen&) const = default;
};

// A small host: hp, the partner's brain on the uncontrolled body, and the session fed every frame.
struct Host {
    FightSession s;
    PartnerBrain brain;
    std::array<int, 2> hp{100, 100};
    std::vector<PartnerEnemyView> enemies;

    void setUp(const FightSessionDef& d) {
        REQUIRE(s.begin(d, 0.0).ok());
        REQUIRE(s.setActiveLead(kLeadA));
        REQUIRE(brain.begin(PartnerTuning{}, 1234));
        hp = {100, 100};
        enemies.assign(2, PartnerEnemyView{});
        enemies[0].pos = {0.f, 0.f};
        enemies[1].pos = {3.f, 1.f};
        s.setLeadHealth(kLeadA, 1.f);
        s.setLeadHealth(kLeadB, 1.f);
        writeHost();
    }
    void writeHost() {
        FightHostCheckpoint h;
        h.hp = hp;
        h.draws = 1234;
        h.partner = brain.save();
        REQUIRE(s.setHostCheckpoint(h) != CheckpointAt::None);
    }
    void retry(CheckpointAt want = CheckpointAt::FightStart) {
        const FightCheckpoint& cp = s.retry();
        REQUIRE(cp.at == want);
        REQUIRE(cp.hostWritten);
        hp = cp.host.hp;
        brain.load(cp.host.partner);
    }
    void damage(FighterId lead, int n) {
        hp[lead] = hp[lead] - n < 0 ? 0 : hp[lead] - n;
        s.setLeadHealth(lead, static_cast<float>(hp[lead]) / 100.f);
    }
    std::vector<Seen> run(const std::vector<Act>& acts, int frames) {
        std::vector<Seen> out;
        std::size_t next = 0;
        for (int f = 0; f < frames; ++f) {
            bool given = false;
            GiftError giftError = GiftError::None;
            CountError countError = CountError::None;
            for (; next < acts.size() && acts[next].frame == f; ++next) {
                const Act& a = acts[next];
                switch (a.kind) {
                case Act::Kind::Switch: s.setActiveLead(a.lead); break;
                case Act::Kind::Heal:
                case Act::Kind::Guard: {
                    s.selectGift(a.kind == Act::Kind::Heal ? Gift::Heal : Gift::Guard);
                    const GiftResult g = s.giveGift(a.lead);
                    given = g.given;
                    giftError = g.error;
                    if (g.given && g.gift == Gift::Heal) {
                        hp[a.lead] += static_cast<int>(g.healShare * 100.f);
                        if (hp[a.lead] > 100) hp[a.lead] = 100;
                        s.setLeadHealth(a.lead, static_cast<float>(hp[a.lead]) / 100.f);
                    }
                    break;
                }
                case Act::Kind::Count: countError = s.callCount(a.lead); break;
                case Act::Kind::Held: s.judgeHeldBlow(a.lead, 0, 10, 1); break;
                case Act::Kind::Mark: s.markEnemy(0, 0.5f, 5.0); break;
                case Act::Kind::Hit: damage(a.lead, a.amount); break;
                case Act::Kind::EndLone: s.endLone(LoneEnd::PartnerCalled); break;
                case Act::Kind::Pool: s.poolRelease(a.lead, a.amount); break;
                case Act::Kind::Parry: s.keptParried(a.amount); break;
                case Act::Kind::Place: s.layPlace(a.lead, static_cast<PersonId>(a.amount)); break;
                case Act::Kind::BossHit: s.bossHit(a.amount, false); break;
                case Act::Kind::Guard2: s.counter(BossCounter::Guard, a.lead); break;
                }
            }
            const FighterId body = s.activeLead() == kLeadA ? kLeadB : kLeadA;
            PartnerInput in;
            in.order = PartnerOrder::StayClose;
            in.pos = body == kLeadA ? Vec2{0.f, 2.f} : Vec2{1.f, 2.f};
            in.leadPos = {0.f, -2.f};
            in.health = static_cast<float>(hp[body]) / 100.f;
            in.canAct = !s.down(body);
            in.leadAttacking = f % 120 < 60;
            in.enemies = std::span<const PartnerEnemyView>(enemies.data(), enemies.size());
            const PartnerIntent& it = brain.step(kFrame, in);
            if (it.rise) {
                hp[body] = static_cast<int>(it.riseHealth * 100.f);
                s.setLeadHealth(body, it.riseHealth);
            }
            const FightTick& t = s.tick(kFrame);
            const std::uint8_t taken = t.checkpoints;
            std::uint32_t bossBits = 0;
            for (const BossEvent& e : *t.boss) bossBits |= 1u << static_cast<unsigned>(e.type);
            out.push_back(Seen{t.now, t.leadsFell, t.leadsRose, t.tookOver, s.activeLead(), t.defeated,
                               s.down(kLeadA), s.down(kLeadB), t.loneCarryBegan, s.strikeCount().open(s.now()),
                               t.loneCarryEndedBy, t.checkpoint, t.bonuses.size(), t.heldExpired,
                               s.giftSlot().allPresses(), hp[kLeadA], hp[kLeadB], s.keptLoad(), given, giftError,
                               countError, it.act, it.why, it.newString, it.rise, bossBits,
                               s.hasBoss() ? s.boss().phase() : 0, s.hasBoss() ? s.boss().hp() : 0,
                               s.weakPointOpen(), s.lone(), s.loneEnded(), s.lonePooled(), s.loneReleases(),
                               s.stateHash(), brain.hash()});
            // The host's part, right after the tick that took one: once per checkpoint taken, the start first.
            for (std::uint8_t b = taken; b != 0; b = static_cast<std::uint8_t>(b & (b - 1))) writeHost();
        }
        return out;
    }
};

FightSessionDef replayDef() {
    FightSessionDef d;
    d.lift = true; // the kept gauge needs the Lift
    d.keptLead = kLeadB;
    d.loneLead = kLeadB;
    return d;
}

// The fight replayed: a Heal, a count with a held blow, the controlled lead felled (the partner takes over), a
// Guard, the fallen lead standing through the brain after 30 s, a Lone release and its end.
std::vector<Act> replayActs() {
    using K = Act::Kind;
    return {
        {1, K::Place, kLeadB, 5},    {5, K::Hit, kLeadA, 30},     {10, K::Heal, kLeadA},     {20, K::Count, kLeadA},   {25, K::Held, kLeadA},
        {40, K::Hit, kLeadA, 100},   {45, K::Guard, kLeadA},    {50, K::Heal, kLeadB},    {60, K::Count, kLeadB},
        {80, K::Mark, kLeadB},       {300, K::Hit, kLeadB, 40}, {400, K::Heal, kLeadB},   {500, K::Switch, kLeadA},
        {700, K::Pool, kLeadB, 2},   {900, K::EndLone, kLeadB}, {1900, K::Switch, kLeadA}, {1950, K::Heal, kLeadB},
        {2000, K::Count, kLeadA},    {2010, K::Held, kLeadA},    {2050, K::Parry, kLeadB, 30},
    };
}

} // namespace

TEST_CASE("fight fate: a fight replayed from its checkpoint with the same inputs and seed gives the same result") {
    const int frames = 2100;
    Host fresh;
    fresh.setUp(replayDef());
    const std::vector<Seen> want = fresh.run(replayActs(), frames);
    // The replayed fight is not quiet: it falls, takes over, stands and shows the Lone Carry.
    bool fell = false, took = false, rose = false, began = false;
    for (const Seen& x : want) {
        fell = fell || x.fell != 0;
        took = took || x.tookOver != kNoFighter;
        rose = rose || x.rose != 0;
        began = began || x.loneBegan;
        REQUIRE_FALSE(x.defeated);
    }
    REQUIRE(fell);
    REQUIRE(took);
    REQUIRE(rose);
    REQUIRE(began);
    REQUIRE(want.back().keptLoad > 0);

    // Another session: an attempt that dirties every piece (a switch, gifts, a live count and held blows, a
    // mark, the Lone Carry ended, a takeover) and ends in a defeat, then the retry and the same fight.
    using K = Act::Kind;
    Host again;
    again.setUp(replayDef());
    const std::vector<Act> lost = {
        {1, K::Place, kLeadB, 5},  {2, K::Switch, kLeadB},   {3, K::Heal, kLeadA},    {4, K::Guard, kLeadA},  {6, K::Count, kLeadB},
        {8, K::Held, kLeadB},     {9, K::Mark, kLeadB},    {12, K::Pool, kLeadB, 5}, {13, K::EndLone, kLeadB}, {14, K::Parry, kLeadB, 50},
        {30, K::Hit, kLeadB, 100}, {31, K::Heal, kLeadA},  {33, K::Count, kLeadA}, {34, K::Held, kLeadA},
        {200, K::Hit, kLeadA, 100},
    };
    const std::vector<Seen> lostSeen = again.run(lost, 210);
    REQUIRE(again.s.defeated());
    REQUIRE(lostSeen[200].defeated);
    again.retry();
    const std::vector<Seen> got = again.run(replayActs(), frames);
    REQUIRE(got.size() == want.size());
    std::size_t firstDiff = want.size();
    for (std::size_t i = 0; i < want.size(); ++i)
        if (!(got[i] == want[i])) {
            firstDiff = i;
            break;
        }
    CHECK(firstDiff == want.size());

    // The same checks catch a retry that leaves the attempt's state: without the retry the stream differs.
    Host stale;
    stale.setUp(replayDef());
    stale.run(lost, 210);
    const std::vector<Seen> wrong = stale.run(replayActs(), frames);
    bool differs = false, hashDiffers = false;
    for (std::size_t i = 0; i < want.size() && !differs; ++i) differs = !(wrong[i] == want[i]);
    for (std::size_t i = 0; i < want.size(); ++i) hashDiffers = hashDiffers || wrong[i].sessionHash != want[i].sessionHash;
    CHECK(differs);
    CHECK(hashDiffers);
    CHECK(got.front().sessionHash == want.front().sessionHash);
    CHECK(got.front().brainHash == want.front().brainHash);
}

TEST_CASE("fight fate: a boss whose def says checkpointAtPhase checkpoints at its phase change; by default it does not") {
    for (const bool atPhase : {true, false}) {
        CAPTURE(atPhase);
        FightSession s;
        FightSessionDef d;
        d.gift = freeGifts();
        d.loneLead = kLeadA;
        d.hasBoss = true;
        d.boss = twoPhaseBoss(atPhase);
        REQUIRE(s.begin(d, 0.0).ok());
        REQUIRE(s.hasBoss());
        REQUIRE(s.tick(kFrame).checkpoint == CheckpointAt::FightStart);
        REQUIRE(s.selectGift(Gift::Heal));
        REQUIRE(s.giveGift(kLeadA).given);
        s.setLeadHealth(kLeadB, 0.0f); // a lead down when the phase changes
        for (int i = 0; i < 30; ++i) s.tick(kFrame);
        s.bossHit(60, false);
        const FightTick& t = s.tick(kFrame);
        REQUIRE(s.boss().phase() == 1);
        const int bossHp = s.boss().hp();
        const double at = s.now();
        const std::uint64_t presses = s.giftSlot().allPresses();
        CHECK(t.checkpoint == (atPhase ? CheckpointAt::Phase : CheckpointAt::None));
        CHECK(s.checkpoint().at == (atPhase ? CheckpointAt::Phase : CheckpointAt::FightStart));
        if (atPhase) {
            CHECK(s.checkpoint().phase == 1);
            CHECK(s.checkpoint().now == at);
            CHECK(s.checkpoint().down[kLeadB]);
        }
        // The attempt goes on and is lost.
        REQUIRE(s.giveGift(kLeadA).given);
        for (int i = 0; i < 60; ++i) s.tick(kFrame);
        s.setLeadHealth(kLeadA, 0.0f);
        REQUIRE(s.tick(kFrame).defeated);
        s.retry();
        if (atPhase) {
            CHECK(s.now() == at);
            CHECK(s.boss().phase() == 1);
            CHECK(s.boss().hp() == bossHp);
            CHECK(s.giftSlot().allPresses() == presses);
            CHECK(s.down(kLeadB)); // as it stood
            CHECK_FALSE(s.down(kLeadA));
            CHECK_FALSE(s.tick(kFrame).loneCarryBegan); // shown at the fight's start, not again mid-fight
        } else {
            CHECK(s.now() == 0.0);
            CHECK(s.boss().phase() == 0);
            CHECK(s.boss().hp() == 100);
            CHECK(s.giftSlot().allPresses() == 0);
            CHECK_FALSE(s.down(kLeadB));
            CHECK(s.tick(kFrame).loneCarryBegan);
        }
    }
}

TEST_CASE("fight fate: a boss the host runs takes its phase checkpoint by call, never after a defeat") {
    FightSession s;
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    s.tick(kFrame);
    REQUIRE(s.setHostCheckpoint(FightHostCheckpoint{}) == CheckpointAt::FightStart); // the start's part first
    for (int i = 0; i < 10; ++i) s.tick(kFrame);
    const double at = s.now();
    CHECK(s.checkpointPhase(2));
    CHECK(s.checkpoint().at == CheckpointAt::Phase);
    CHECK(s.checkpoint().phase == 2);
    FightHostCheckpoint h;
    h.bossHp = 300;
    h.bossPhase = 2;
    s.setHostCheckpoint(h);
    CHECK(s.tick(kFrame).checkpoint == CheckpointAt::None); // written: not asked again
    CHECK(s.checkpoint().host.bossHp == 300);
    // Not written: the next tick asks for it.
    CHECK(s.checkpointPhase(2));
    CHECK(s.checkpoint().host.bossHp == 0);
    CHECK(s.tick(kFrame).checkpoint == CheckpointAt::Phase);
    s.setLeadHealth(kLeadA, 0.0f);
    s.setLeadHealth(kLeadB, 0.0f);
    CHECK_FALSE(s.defeated()); // the step is still open
    CHECK_FALSE(s.checkpointPhase(3)); // the call closes it first: a defeat, so none is taken
    REQUIRE(s.defeated());
    const FightCheckpoint& cp = s.retry();
    CHECK(cp.at == CheckpointAt::Phase);
    CHECK(cp.phase == 2);
    CHECK(s.now() > at);
    // A begin forgets both checkpoints.
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    CHECK(s.checkpoint().at == CheckpointAt::None);
}

TEST_CASE("fight fate: falls, checkpoints and retries allocate nothing") {
    FightSession s;
    FightSessionDef d;
    d.hasBoss = true;
    d.boss = twoPhaseBoss(true);
    REQUIRE(s.begin(d, 0.0).ok());
    REQUIRE(ftcore_test::allocHookInstalled());
    const std::uint64_t before = ftcore_test::allocStats().count;
    s.tick(kFrame);
    s.setLeadHealth(kLeadA, 0.0f);
    s.tick(kFrame);
    s.bossHit(60, false);
    s.tick(kFrame);
    s.setHostCheckpoint(FightHostCheckpoint{});
    s.setLeadHealth(kLeadB, 0.0f);
    s.tick(kFrame);
    s.retry();
    s.tick(kFrame);
    CHECK(ftcore_test::allocStats().count == before);
}

TEST_CASE("fight fate: a fight replayed from a phase checkpoint with the same inputs and seed gives the same result") {
    using K = Act::Kind;
    FightSessionDef d = replayDef();
    d.hasBoss = true;
    d.boss = twoPhaseBoss(true);
    // Up to the phase change: a place, a Lone release, the controlled lead felled (the partner takes over).
    const std::vector<Act> before = {
        {1, K::Place, kLeadB, 5}, {10, K::Pool, kLeadB, 2}, {20, K::Parry, kLeadB, 30}, {40, K::Hit, kLeadA, 100},
        {60, K::Guard2, kLeadB},  {100, K::BossHit, kLeadB, 60},
    };
    // After it: the boss's rotation, counters, hits, another release and the Lone Carry's end, a Heal, and the
    // fallen lead standing through the brain.
    const std::vector<Act> after = {
        {5, K::Pool, kLeadB, 3},     {30, K::Guard2, kLeadB},     {90, K::BossHit, kLeadB, 5},
        {150, K::Count, kLeadB},     {160, K::Held, kLeadB},      {200, K::EndLone, kLeadB},
        {300, K::Hit, kLeadB, 30},   {320, K::Heal, kLeadB},      {400, K::Guard2, kLeadB},
        {600, K::Parry, kLeadB, 20}, {1800, K::Switch, kLeadA},   {1850, K::BossHit, kLeadA, 3},
    };
    const int frames = 1900;
    Host fresh;
    fresh.setUp(d);
    const std::vector<Seen> pre = fresh.run(before, 101);
    REQUIRE(pre.back().cp == CheckpointAt::Phase);
    REQUIRE(fresh.s.checkpoint().at == CheckpointAt::Phase);
    REQUIRE(fresh.s.checkpoint().down[kLeadA]); // a lead down at the checkpoint: the brain's copy matters
    REQUIRE(fresh.s.checkpoint().lonePooled == 2);
    const std::vector<Seen> want = fresh.run(after, frames);
    bool rose = false, ended = false, events = false;
    for (const Seen& x : want) {
        rose = rose || x.rose != 0;
        ended = ended || x.loneEnd != LoneEnd::None;
        events = events || x.bossEvents != 0;
        REQUIRE_FALSE(x.defeated);
        REQUIRE(x.cp == CheckpointAt::None); // no later phase
    }
    REQUIRE(rose);
    REQUIRE(ended);
    REQUIRE(events);

    // The same fight to its phase change, then an attempt that dirties every piece and is lost.
    Host again;
    again.setUp(d);
    REQUIRE(again.run(before, 101).back().cp == CheckpointAt::Phase);
    const FightCheckpoint taken = again.s.checkpoint();
    const std::vector<Act> lost = {
        {2, K::Heal, kLeadB},       {3, K::Guard, kLeadB},      {5, K::Guard2, kLeadB},  {8, K::Count, kLeadB},
        {9, K::Held, kLeadB},       {10, K::Mark, kLeadB},      {11, K::Place, kLeadB, 9}, {12, K::Parry, kLeadB, 40},
        {20, K::BossHit, kLeadB, 30}, {50, K::Hit, kLeadB, 100},
    };
    again.run(lost, 60);
    REQUIRE(again.s.defeated());
    again.retry(CheckpointAt::Phase);
    const std::vector<Seen> got = again.run(after, frames);
    REQUIRE(got.size() == want.size());
    std::size_t firstDiff = want.size();
    for (std::size_t i = 0; i < want.size(); ++i)
        if (!(got[i] == want[i])) {
            firstDiff = i;
            break;
        }
    CHECK(firstDiff == want.size());
    CHECK(got.front().bossEvents == want.front().bossEvents); // no phase change delivered again
    CHECK(got.front().cp == CheckpointAt::None);
    // The checkpoint itself is untouched by the replay.
    CHECK(again.s.checkpoint().at == CheckpointAt::Phase);
    CHECK(again.s.checkpoint().now == taken.now);
    CHECK(again.s.checkpoint().host.hp == taken.host.hp);
    CHECK(again.s.checkpoint().lonePooled == taken.lonePooled);
}

TEST_CASE("fight fate: a lead that cannot fall in this fight survives a lethal blow at 1 hp; the other still falls") {
    FightSessionDef d;
    d.cannotFall[kLeadA] = true;
    FightSession s;
    REQUIRE(s.begin(d, 0.0).ok());
    REQUIRE(s.setActiveLead(kLeadA));
    CHECK(s.cannotFall(kLeadA));
    CHECK_FALSE(s.cannotFall(kLeadB));
    CHECK_FALSE(s.cannotFall(kThirdSlot));
    // The host's sum: a blow far past 0 leaves the flagged lead at 1 and raises heldUp once.
    CHECK(s.blowOnLead(kLeadA, 40, 500, 100) == 1);
    CHECK_FALSE(s.down(kLeadA));
    CHECK(s.leadHealth(kLeadA) == 0.01f);
    const FightTick& t = s.tick(kFrame);
    CHECK(t.heldUp == kBitA);
    CHECK(t.leadsFell == 0);
    CHECK(t.tookOver == kNoFighter);
    CHECK(s.tick(kFrame).heldUp == 0); // once
    // A blow that does not reach 0 raises nothing; a health fed as 0 holds it up the same.
    CHECK(s.blowOnLead(kLeadA, 50, 10, 100) == 40);
    CHECK(s.tick(kFrame).heldUp == 0);
    s.setLeadHealth(kLeadA, 0.f);
    CHECK_FALSE(s.down(kLeadA));
    CHECK(s.tick(kFrame).heldUp == kBitA);
    // The other lead falls as in any fight, and with the flagged lead standing the fight goes on.
    CHECK(s.blowOnLead(kLeadB, 30, 30, 100) == 0);
    CHECK(s.down(kLeadB));
    const FightTick& f = s.tick(kFrame);
    CHECK(f.leadsFell == kBitB);
    CHECK(f.heldUp == 0);
    CHECK_FALSE(f.defeated);
    // The flagged lead alone: lethal blows still never make a defeat.
    for (int i = 0; i < 5; ++i) CHECK(s.blowOnLead(kLeadA, 1, 90, 100) == 1);
    CHECK_FALSE(s.down(kLeadA));
    CHECK_FALSE(s.defeated());
    CHECK(s.tick(kFrame).heldUp == kBitA);
    // A non-lead is the plain sum (never under 0); the flag is per fight: a fight without it lets the same lead fall.
    CHECK(s.blowOnLead(kThirdSlot, 10, 30, 100) == 0);
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    CHECK(s.blowOnLead(kLeadA, 40, 500, 100) == 0);
    CHECK(s.down(kLeadA));
    CHECK(s.tick(kFrame).heldUp == 0);
}

TEST_CASE("fight fate: with both leads unable to fall, health never loses the fight") {
    // What ends such a fight (a round timer, say) is the host's: Core has no end for it.
    FightSessionDef d;
    d.cannotFall = {true, true};
    FightSession s;
    REQUIRE(s.begin(d, 0.0).ok());
    for (int i = 0; i < 60 * 60; ++i) {
        const FighterId lead = (i & 1) ? kLeadA : kLeadB;
        CHECK(s.blowOnLead(lead, 1, 1000, 100) == 1);
        s.setLeadHealth(lead == kLeadA ? kLeadB : kLeadA, -0.5f); // an overkill fed straight in holds up too
        const FightTick& t = s.tick(kFrame);
        REQUIRE(t.heldUp == (kBitA | kBitB));
        REQUIRE(t.leadsFell == 0);
        REQUIRE_FALSE(t.defeated);
    }
    CHECK_FALSE(s.down(kLeadA));
    CHECK_FALSE(s.down(kLeadB));
    CHECK_FALSE(s.defeated());
    // A lead not in the fight is never held up (and its flag changes nothing).
    FightSessionDef one;
    one.inFight = {true, false};
    one.cannotFall = {false, true};
    REQUIRE(s.begin(one, 0.0).ok());
    CHECK_FALSE(s.cannotFall(kLeadB));
    CHECK(s.blowOnLead(kLeadB, 10, 20, 100) == 0);
    CHECK(s.blowOnLead(kLeadA, 10, 20, 100) == 0);
    const FightTick& end = s.tick(kFrame);
    CHECK(end.defeated);
    CHECK(end.heldUp == 0);
    CHECK(s.defeated());
}
TEST_CASE("fight fate: a checkpoint the host never wrote says so at the retry; a written one does not") {
    FightSession s;
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    REQUIRE(s.tick(kFrame).checkpoint == CheckpointAt::FightStart); // taken; the host writes nothing
    s.setLeadHealth(kLeadA, 0.f);
    s.setLeadHealth(kLeadB, 0.f);
    REQUIRE(s.tick(kFrame).defeated);
    const FightCheckpoint& empty = s.retry();
    CHECK(empty.at == CheckpointAt::FightStart);
    CHECK_FALSE(empty.hostWritten);
    CHECK_FALSE(empty.host.partner.begun); // an unbegun brain's save: not the fight's
    // Written, it says so, and the brain comes back exactly.
    PartnerBrain b;
    REQUIRE(b.begin(PartnerTuning{}, 9));
    FightHostCheckpoint h;
    h.partner = b.save();
    h.currency = 40;
    REQUIRE(s.setHostCheckpoint(h) == CheckpointAt::FightStart);
    s.setLeadHealth(kLeadA, 0.f);
    s.setLeadHealth(kLeadB, 0.f);
    REQUIRE(s.tick(kFrame).defeated);
    const FightCheckpoint& full = s.retry();
    CHECK(full.hostWritten);
    CHECK(full.host.partner.begun);
    CHECK(full.host.currency == 40);
    PartnerBrain c;
    c.load(full.host.partner);
    CHECK(c.hash() == b.hash());
    // A new checkpoint starts unwritten.
    REQUIRE(s.checkpointPhase(1));
    CHECK_FALSE(s.checkpoint().hostWritten);
    CHECK(s.checkpoint(CheckpointAt::FightStart).hostWritten);
}

TEST_CASE("fight fate: a phase checkpoint on the tick that takes the start one asks for both, and the host writes the start first") {
    constexpr std::uint8_t kBoth = (1u << static_cast<unsigned>(CheckpointAt::FightStart)) |
                                   (1u << static_cast<unsigned>(CheckpointAt::Phase));
    FightSession s;
    FightSessionDef d;
    d.hasBoss = true;
    d.boss = twoPhaseBoss(true);
    REQUIRE(s.begin(d, 0.0).ok());
    s.bossHit(60, false); // the phase changes on the first tick: the one that takes the start checkpoint
    const FightTick& t = s.tick(kFrame);
    CHECK(t.checkpoints == kBoth);
    CHECK(t.checkpoint == CheckpointAt::Phase);
    REQUIRE(s.checkpoint(CheckpointAt::FightStart).at == CheckpointAt::FightStart);
    REQUIRE(s.checkpoint(CheckpointAt::Phase).at == CheckpointAt::Phase);
    FightHostCheckpoint a, b;
    a.currency = 1;
    b.currency = 2;
    CHECK(s.setHostCheckpoint(a) == CheckpointAt::FightStart);
    CHECK(s.setHostCheckpoint(b) == CheckpointAt::Phase);
    CHECK(s.checkpoint(CheckpointAt::FightStart).hostWritten);
    CHECK(s.checkpoint(CheckpointAt::FightStart).host.currency == 1);
    CHECK(s.checkpoint(CheckpointAt::Phase).hostWritten);
    CHECK(s.checkpoint(CheckpointAt::Phase).host.currency == 2);
    CHECK(s.tick(kFrame).checkpoints == 0);
    // checkpointPhase before the first tick: both, reported by the next tick; a named write and a bad name.
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    REQUIRE(s.checkpointPhase(1));
    const FightTick& u = s.tick(kFrame);
    CHECK(u.checkpoints == kBoth);
    CHECK(u.checkpoint == CheckpointAt::Phase);
    CHECK(s.setHostCheckpoint(b, CheckpointAt::Phase));
    CHECK_FALSE(s.checkpoint(CheckpointAt::FightStart).hostWritten);
    CHECK_FALSE(s.setHostCheckpoint(a, CheckpointAt::None));
    CHECK(s.setHostCheckpoint(a) == CheckpointAt::FightStart);
    CHECK(s.checkpoint(CheckpointAt::FightStart).host.currency == 1);
    // A tick that took only a phase checkpoint asks for that one alone.
    REQUIRE(s.checkpointPhase(2));
    CHECK(s.tick(kFrame).checkpoints == (1u << static_cast<unsigned>(CheckpointAt::Phase)));
}


TEST_CASE("fight fate: a gift at a lead who is down is refused Down: no charge, cooldown, thread or tally") {
    FightSessionDef d;
    d.gift = freeGifts();
    d.gift.cooldown = {5.0, 5.0, 5.0};
    d.loneLead = kLeadB;
    FightSession s;
    REQUIRE(s.begin(d, 0.0).ok());
    REQUIRE(s.setActiveLead(kLeadB));
    s.setHostCheckpoint(FightHostCheckpoint{});
    s.setLeadHealth(kLeadA, 0.f);
    REQUIRE(s.down(kLeadA));
    s.tick(kFrame);
    const GiftSlot& slot = s.giftSlot();
    for (Gift g : {Gift::Heal, Gift::Guard, Gift::Count}) {
        REQUIRE(s.selectGift(g));
        const std::uint64_t presses = slot.allPresses();
        const int heals = slot.healsLeft();
        const double ready = slot.readyAt(g);
        const std::uint32_t threads = slot.threads(kLeadA);
        const GiftResult r = s.giveGift(kLeadA);
        CHECK_FALSE(r.given);
        CHECK(r.error == GiftError::Down);
        CHECK(r.gift == g);
        CHECK(r.target == kLeadA);
        CHECK(r.healShare == 0.f);
        CHECK(slot.allPresses() == presses); // no tally
        CHECK(slot.healsLeft() == heals);    // no charge
        CHECK(slot.readyAt(g) == ready);     // no cooldown
        CHECK(slot.threads(kLeadA) == threads);
        CHECK(s.down(kLeadA)); // a gift never stands a lead up
        CHECK(s.lone());       // a refused press leaves the Lone Carry
        // The shared press is the same call.
        const SharedReply p = s.press(SharedAction::Gift, kThirdSlot, kLeadA);
        CHECK_FALSE(p.accepted);
        CHECK(p.gift.error == GiftError::Down);
    }
    // The standing lead takes gifts as ever.
    REQUIRE(s.selectGift(Gift::Heal));
    s.setLeadHealth(kLeadB, 0.5f);
    CHECK(s.giveGift(kLeadB).given);
    // The down lead stands only by time (the brain's rise, fed as health) or at a retry.
    CHECK(s.retry().at == CheckpointAt::FightStart);
    CHECK_FALSE(s.down(kLeadA));
    REQUIRE(s.selectGift(Gift::Heal));
    CHECK(s.giveGift(kLeadA).given);
    s.setLeadHealth(kLeadA, 0.f);
    REQUIRE(s.down(kLeadA));
    REQUIRE(s.tick(kFrame).leadsFell == kBitA);
    s.setLeadHealth(kLeadA, 0.25f);
    CHECK_FALSE(s.down(kLeadA));
    CHECK(s.tick(kFrame).leadsRose == kBitA);
}

TEST_CASE("fight fate: a start checkpoint is never taken in a defeat; a fall before it is settled into it") {
    // Both leads at 0 before the first tick: a defeat, and no start checkpoint.
    FightSession s;
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    s.setLeadHealth(kLeadA, 0.f);
    s.setLeadHealth(kLeadB, 0.f);
    const FightTick& t = s.tick(kFrame);
    CHECK(t.defeated);
    CHECK(t.checkpoints == 0);
    CHECK(s.checkpoint(CheckpointAt::FightStart).at == CheckpointAt::None);
    CHECK(s.setHostCheckpoint(FightHostCheckpoint{}) == CheckpointAt::None);
    // The retry is a begin again: both stand, the start checkpoint is taken on the next tick, and the fight can
    // be lost again.
    CHECK(s.retry().at == CheckpointAt::None);
    CHECK_FALSE(s.defeated());
    CHECK_FALSE(s.down(kLeadA));
    CHECK_FALSE(s.down(kLeadB));
    CHECK(s.tick(kFrame).checkpoint == CheckpointAt::FightStart);
    s.setLeadHealth(kLeadA, 0.f);
    s.setLeadHealth(kLeadB, 0.f);
    CHECK(s.tick(kFrame).defeated);
    // The same through setHostCheckpoint before any tick: none taken.
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    s.setLeadHealth(kLeadA, 0.f);
    s.setLeadHealth(kLeadB, 0.f);
    CHECK(s.setHostCheckpoint(FightHostCheckpoint{}) == CheckpointAt::None);
    CHECK(s.defeated());
    CHECK(s.checkpoint(CheckpointAt::FightStart).at == CheckpointAt::None);
    // One lead fed 0, then setHostCheckpoint before the first tick: the checkpoint holds the settled fall and
    // takeover, the tick reports them once, and a retry comes back to that state with nothing to report.
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    REQUIRE(s.setActiveLead(kLeadA));
    s.setLeadHealth(kLeadA, 0.f);
    REQUIRE(s.setHostCheckpoint(FightHostCheckpoint{}) == CheckpointAt::FightStart);
    CHECK(s.checkpoint().down[kLeadA]);
    CHECK_FALSE(s.checkpoint().down[kLeadB]);
    CHECK(s.checkpoint().activeLead == kLeadB);
    CHECK(s.activeLead() == kLeadB);
    const FightTick& u = s.tick(kFrame);
    CHECK(u.leadsFell == kBitA);
    CHECK(u.tookOver == kLeadB);
    CHECK_FALSE(u.defeated);
    s.setLeadHealth(kLeadB, 0.f);
    REQUIRE(s.tick(kFrame).defeated);
    const FightCheckpoint& cp = s.retry();
    REQUIRE(cp.at == CheckpointAt::FightStart);
    CHECK(s.down(kLeadA));
    CHECK_FALSE(s.down(kLeadB));
    CHECK(s.activeLead() == kLeadB);
    const FightTick& v = s.tick(kFrame);
    CHECK(v.leadsFell == 0);
    CHECK(v.tookOver == kNoFighter);
    CHECK_FALSE(v.defeated);
}

TEST_CASE("fight fate: the third slot: a Vacant or Hostile one gives no gift, refused with its own reason, and the checkpoint holds it") {
    for (const SlotState st : {SlotState::Companion, SlotState::Vacant, SlotState::Hostile}) {
        CAPTURE(static_cast<int>(st));
        FightSessionDef d;
        d.thirdSlot = st;
        d.loneLead = kLeadA;
        d.inFight = {true, false};
        FightSession s;
        REQUIRE(s.begin(d, 0.0).ok());
        CHECK(s.thirdSlot() == st);
        const GiftError want = st == SlotState::Vacant ? GiftError::Vacant : st == SlotState::Hostile ? GiftError::Hostile : GiftError::None;
        CHECK(s.thirdSlotRefusal() == want);
        s.tick(kFrame);
        REQUIRE(s.setHostCheckpoint(FightHostCheckpoint{}) == CheckpointAt::FightStart);
        const std::uint64_t given = s.giftSlot().tally().givenOf(Gift::Heal);
        const int heals = s.giftHealsLeft();
        REQUIRE(s.selectGift(Gift::Heal));
        s.setLeadHealth(kLeadA, 0.5f);
        const GiftResult r = s.giveGift(kLeadA);
        if (st == SlotState::Companion) {
            CHECK(r.given);
            CHECK_FALSE(s.lone()); // a gift given ends the Lone Carry
            continue;
        }
        CHECK_FALSE(r.given);
        CHECK(r.error == want);
        CHECK(s.giftHealsLeft() == heals);                       // no charge
        CHECK(s.giftHealReadyIn() == 0.0);                       // no cooldown
        CHECK(s.giftSlot().tally().givenOf(Gift::Heal) == given); // no gift tally
        CHECK(s.lone());                                         // the Lone Carry stays
        // At a down lead the slot still answers first.
        s.setLeadHealth(kLeadA, 0.f);
        CHECK(s.giveGift(kLeadA).error == want);
        // The state is in the hash, and the checkpoint brings it back.
        const std::uint64_t h = s.stateHash();
        s.giftSlot().setState(SlotState::Companion);
        CHECK(s.stateHash() != h);
        REQUIRE(s.tick(kFrame).defeated);
        s.retry();
        CHECK(s.thirdSlot() == st);
    }
    // A host that sets the state after begin (as before) still holds over the def, and the start checkpoint keeps it.
    FightSession s;
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    s.giftSlot().setState(SlotState::Vacant);
    s.tick(kFrame);
    s.setLeadHealth(kLeadA, 0.f);
    s.setLeadHealth(kLeadB, 0.f);
    REQUIRE(s.tick(kFrame).defeated);
    s.retry();
    CHECK(s.thirdSlot() == SlotState::Vacant);
    // An out-of-range def value is a Companion.
    FightSessionDef bad;
    bad.thirdSlot = static_cast<SlotState>(9);
    REQUIRE(s.begin(bad, 0.0).ok());
    CHECK(s.thirdSlot() == SlotState::Companion);
}

TEST_CASE("fight fate: stateHash moves with each play field the host can change alone") {
    FightSessionDef d;
    d.lift = true;
    d.keptLead = kLeadA;
    const auto fresh = [&](FightSession& s) {
        REQUIRE(s.begin(d, 0.0).ok());
        s.tick(kFrame);
    };
    FightSession base;
    fresh(base);
    const std::uint64_t h0 = base.stateHash();
    using Edit = void (*)(FightSession&);
    const Edit edits[] = {
        [](FightSession& s) { s.tick(kFrame); },                         // the clock
        [](FightSession& s) { s.setLoad(kLeadA, 3); },                    // the kept gauge
        [](FightSession& s) { s.setLeadHealth(kLeadB, 0.5f); },           // a lead's health
        [](FightSession& s) { s.setLeadHealth(kLeadB, 0.f); },            // a fall, marked (the step still open)
        [](FightSession& s) { REQUIRE(s.setActiveLead(kLeadB)); },        // control
        [](FightSession& s) { s.giftSlot().setState(SlotState::Hostile); }, // the third slot
        [](FightSession& s) { REQUIRE(s.selectGift(Gift::Guard)); },      // the gift chosen
        [](FightSession& s) { REQUIRE(s.checkpointPhase(1)); },           // a checkpoint taken
        [](FightSession& s) { REQUIRE(s.setHostCheckpoint(FightHostCheckpoint{}) != CheckpointAt::None); }, // written
    };
    int i = 0;
    for (const Edit e : edits) {
        CAPTURE(i++);
        FightSession s;
        fresh(s);
        REQUIRE(s.stateHash() == h0); // the same set-up hashes the same
        e(s);
        CHECK(s.stateHash() != h0);
    }
}

TEST_CASE("fight fate: blowOnLead holds the damage to 0..hp, so extreme ints never overflow and a blow never heals") {
    constexpr int kMax = std::numeric_limits<int>::max();
    constexpr int kMin = std::numeric_limits<int>::min();
    FightSessionDef d;
    d.cannotFall = {true, false};
    FightSession s;
    REQUIRE(s.begin(d, 0.0).ok());
    // Huge blows: the plain lead goes to 0 and falls; the one that cannot fall stays at 1.
    CHECK(s.blowOnLead(kLeadB, 40, kMax, 100) == 0);
    CHECK(s.down(kLeadB));
    CHECK(s.blowOnLead(kLeadA, 40, kMax, 100) == 1);
    CHECK_FALSE(s.down(kLeadA));
    CHECK(s.blowOnLead(kLeadA, kMax, kMax, 100) == 1);
    CHECK(s.blowOnLead(kThirdSlot, kMax, kMax, 100) == 0);
    CHECK(s.blowOnLead(kThirdSlot, kMin, kMax, 100) == kMin); // already under 0: no damage left to take
    // Negative damage heals nothing.
    CHECK(s.blowOnLead(kLeadA, 40, -5, 100) == 40);
    CHECK(s.blowOnLead(kLeadA, 40, kMin, 100) == 40);
    CHECK(s.blowOnLead(kThirdSlot, kMax, kMin, 100) == kMax);
    CHECK(s.blowOnLead(kThirdSlot, kMin, kMin, 100) == kMin);
    CHECK(s.leadHealth(kLeadA) == 0.4f);
    // A blow of 0 at hp 0 is no blow: the lead that cannot fall is still held at 1.
    CHECK(s.blowOnLead(kLeadA, 0, 0, 100) == 1);
}
