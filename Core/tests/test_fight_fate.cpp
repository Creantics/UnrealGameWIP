// Down, takeover, defeat, checkpoints and retry in the fight session (FightSession.h).
#include "ftcore/FightSession.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <limits>
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
    CHECK(s.activeLead() == kLeadB); // at once
    const FightTick& t = s.tick(kFrame);
    CHECK(t.leadsFell == kBitA);
    CHECK(t.tookOver == kLeadB);
    CHECK_FALSE(t.defeated);
    CHECK_FALSE(s.defeated());
    // Once: a second 0 is no new fall, and the next tick is quiet.
    s.setLeadHealth(kLeadA, 0.0f);
    const FightTick& q = s.tick(kFrame);
    CHECK(q.leadsFell == 0);
    CHECK(q.tookOver == kNoFighter);
    // The player cannot switch to a lead that is down.
    CHECK_FALSE(s.setActiveLead(kLeadA));
    CHECK(s.activeLead() == kLeadB);
    // It stands again: control stays where it went.
    s.setLeadHealth(kLeadA, 0.25f);
    CHECK_FALSE(s.down(kLeadA));
    const FightTick& r = s.tick(kFrame);
    CHECK(r.leadsRose == kBitA);
    CHECK(r.leadsFell == 0);
    CHECK(s.activeLead() == kLeadB);
    CHECK(s.tick(kFrame).leadsRose == 0);
    // Only the player's switch moves it back.
    CHECK(s.setActiveLead(kLeadA));
    CHECK(s.activeLead() == kLeadA);
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
    CHECK(s.defeated());
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

TEST_CASE("fight fate: both leads falling on one step is a defeat, after the takeover") {
    FightSession s;
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    REQUIRE(s.setActiveLead(kLeadA));
    s.setLeadHealth(kLeadA, 0.0f);
    s.setLeadHealth(kLeadB, 0.0f);
    const FightTick& t = s.tick(kFrame);
    CHECK(t.leadsFell == (kBitA | kBitB));
    CHECK(t.tookOver == kLeadB);
    CHECK(t.defeated);
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
    CHECK(s.checkpoint().activeLead == kLeadB);
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
        h.partner = brain;
        s.setHostCheckpoint(h);
    }
    void retry(CheckpointAt want = CheckpointAt::FightStart) {
        const FightCheckpoint& cp = s.retry();
        REQUIRE(cp.at == want);
        hp = cp.host.hp;
        brain = cp.host.partner;
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
            const CheckpointAt taken = t.checkpoint;
            std::uint32_t bossBits = 0;
            for (const BossEvent& e : *t.boss) bossBits |= 1u << static_cast<unsigned>(e.type);
            out.push_back(Seen{t.now, t.leadsFell, t.leadsRose, t.tookOver, s.activeLead(), t.defeated,
                               s.down(kLeadA), s.down(kLeadB), t.loneCarryBegan, s.strikeCount().open(s.now()),
                               t.loneCarryEndedBy, t.checkpoint, t.bonuses.size(), t.heldExpired,
                               s.giftSlot().allPresses(), hp[kLeadA], hp[kLeadB], s.keptLoad(), given, giftError,
                               countError, it.act, it.why, it.newString, it.rise, bossBits,
                               s.hasBoss() ? s.boss().phase() : 0, s.hasBoss() ? s.boss().hp() : 0,
                               s.weakPointOpen(), s.lone(), s.loneEnded(), s.lonePooled(), s.loneReleases()});
            if (taken != CheckpointAt::None) writeHost(); // the host's part, right after the tick that took one
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
    bool differs = false;
    for (std::size_t i = 0; i < want.size() && !differs; ++i) differs = !(wrong[i] == want[i]);
    CHECK(differs);
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
    REQUIRE(s.defeated());
    CHECK_FALSE(s.checkpointPhase(3));
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
    // A non-lead is the plain sum; the flag is per fight: a fight without it lets the same lead fall.
    CHECK(s.blowOnLead(kThirdSlot, 10, 30, 100) == -20);
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    CHECK(s.blowOnLead(kLeadA, 40, 500, 100) == -460);
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
    CHECK(s.blowOnLead(kLeadB, 10, 20, 100) == -10);
    CHECK(s.blowOnLead(kLeadA, 10, 20, 100) == -10);
    CHECK(s.defeated());
    CHECK(s.tick(kFrame).heldUp == 0);
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
    s.setLeadHealth(kLeadA, 0.25f);
    CHECK_FALSE(s.down(kLeadA));
    CHECK(s.tick(kFrame).leadsRose == kBitA);
}
