// Carrying in the arena: the Load cap from the chairs, block Load, take half for an ally, and the chairs' effects.
#include "ftcore/ArenaFight.h"
#include "ftcore/Places.h"
#include "ftcore/UiViews.h"

#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 60.f;

ArenaAttack swing(float startup, float active, float recovery, float range, int damage) {
    ArenaAttack a;
    a.startup = startup;
    a.active = active;
    a.recovery = recovery;
    a.range = range;
    a.arcCos = -1.f;
    a.damage = damage;
    a.poiseDamage = 0.f;
    a.guardDamage = 0.f;
    a.lunge = 0.f;
    a.tracking = 0.f;
    a.armor = 0.f;
    a.stamina = 0.f;
    a.cancel = 0.5f;
    return a;
}

// The rule kit of test_arena.cpp, with the carrying fields at their struct defaults.
ArenaFighter ruleFighter() {
    ArenaFighter f;
    f.hp = 100;
    f.stamina = 100.f;
    f.staminaRegen = 50.f;
    f.staminaDelay = 0.5f;
    f.poise = 30.f;
    f.poiseDelay = 1.f;
    f.hitstun = 0.4f;
    f.moveSpeed = 4.f;
    f.turnRate = 20.f;
    f.bodyRadius = 0.4f;
    f.bufferTime = 0.2f;
    f.lightCount = 3;
    f.light[0] = swing(0.2f, 0.1f, 0.3f, 2.f, 10);
    f.light[1] = swing(0.2f, 0.1f, 0.3f, 2.f, 11);
    f.light[2] = swing(0.2f, 0.1f, 0.3f, 2.f, 12);
    f.chainGap = 0.3f;
    f.heavy = swing(0.4f, 0.2f, 0.5f, 2.f, 30);
    f.riposte = swing(0.1f, 0.1f, 0.4f, 2.5f, 50);
    f.release = swing(0.1f, 0.1f, 0.4f, 2.5f, 20);
    f.dodgeTime = 0.6f;
    f.dodgeDistance = 3.f;
    f.dodgeInvulnStart = 0.1f;
    f.dodgeInvulnEnd = 0.4f;
    f.dodgeStamina = 20.f;
    f.dodgeCancel = 0.75f;
    f.guardArcCos = 0.f;
    f.guardAbsorb = 0.75f;
    f.guardMoveScale = 0.5f;
    f.guardBreakTime = 1.f;
    f.parryWindow = 0.2f;
    f.parryRearm = 0.5f;
    f.riposteReach = 2.2f;
    f.healCharges = 2;
    f.healAmount = 40;
    f.healTime = 0.5f;
    f.healRecovery = 0.3f;
    f.healMoveScale = 0.35f;
    f.lockRange = 15.f;
    f.lockBreak = 20.f;
    return f;
}

ArenaEnemyKind dummyKind() {
    ArenaEnemyKind k;
    k.hp = 100;
    k.poise = 30.f;
    k.poiseDelay = 1.f;
    k.staggerTime = 1.f;
    k.openTime = 1.5f;
    k.downTime = 1.f;
    k.attackDelay = 1000.f;
    k.bodyRadius = 0.45f;
    return k;
}

// Asks at once, one move picked while the player is within `maxRange`; it bites once a minute.
ArenaEnemyKind biterKind(int damage, float maxRange = 3.f) {
    ArenaEnemyKind k = dummyKind();
    k.attackDelay = 0.f;
    // It strafes (and so asks for a token) from where it stands: the rig never moves the bodies.
    k.preferredRange = maxRange;
    k.runRange = maxRange + 5.f;
    k.moveCount = 1;
    ArenaEnemyMove& m = k.moves[0];
    m.attack = swing(0.4f, 0.15f, 0.5f, 2.f, damage);
    m.minRange = 0.f;
    m.maxRange = maxRange;
    m.cooldown = 60.f;
    m.weight = 1.f;
    m.chain = -1;
    m.unblockable = false;
    m.parryable = true;
    return k;
}

struct Rig {
    ArenaDef def;
    ArenaFight fight;
    ArenaBodies bodies;
    ArenaInput in;
    std::vector<ArenaEvent> log;
    int ticks = 0;

    explicit Rig(std::initializer_list<ArenaEnemyKind> kinds, const ArenaFighter& fighter = ruleFighter()) {
        def.fighter = fighter;
        std::size_t n = 0;
        for (const ArenaEnemyKind& k : kinds) def.enemies[n++] = k;
        def.enemyCount = static_cast<std::uint8_t>(n);
        def.attackers = 1;
        fight.begin(def, ArenaRing{Vec2{0.f, 0.f}, 30.f}, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 1);
        for (std::size_t i = 0; i < n; ++i) bodies.enemies[i] = Vec2{1.5f, 0.f};
    }

    void tick() {
        const ArenaOutput& out = fight.tick(kDt, in, bodies);
        ++ticks;
        for (const ArenaEvent& e : out.events) log.push_back(e);
        in.light = in.heavy = in.dodge = in.jump = in.heal = in.lock = in.release = in.pair = in.takeHalf = false;
        in.lockSwitch = 0;
        in.read = -1;
    }
    void run(int n) {
        for (int i = 0; i < n; ++i) tick();
    }
    int count(ArenaEventType type) const {
        int n = 0;
        for (const ArenaEvent& e : log)
            if (e.type == type) ++n;
        return n;
    }
    const ArenaEvent* find(ArenaEventType type) const {
        for (const ArenaEvent& e : log)
            if (e.type == type) return &e;
        return nullptr;
    }
    // The Load a blow of enemy 0 stored (a gift has another source).
    int storedByEnemy() const {
        int n = 0;
        for (const ArenaEvent& e : log)
            if (e.type == ArenaEventType::LoadStored && e.source == 0) n += e.amount;
        return n;
    }
    // Parries enemy 0's first bite (guard raised 0.3 s in: the bite lands at 0.4 s).
    void parry() {
        for (int k = 1; k <= 40; ++k) {
            if (k == 19) in.guard = true;
            tick();
        }
        in.guard = false;
    }
    // Presses Release; true if it went. Then lets a refused press age out of the buffer, so it can't go on a
    // later store.
    bool release() {
        const int before = count(ArenaEventType::Released);
        in.release = true;
        tick();
        const bool went = count(ArenaEventType::Released) > before;
        run(20); // past bufferTime
        return went;
    }
};

PlaceTable namedPlaces(int named) {
    PlaceTable p;
    p.reset(3);
    for (int i = 0; i < named; ++i) p.layPlace(static_cast<PersonId>(i + 1));
    for (int i = named; i < 3; ++i) p.leaveOpen();
    return p;
}

// Take half: the player at the origin, the ally `allyAt` m along +x, the biter 2 m past the ally (it reaches the
// ally, never the player). Returns the tick its bite lands on the ally with no press (the fight is deterministic).
struct AllySetup {
    float allyAt = 3.f;
    int damage = 22;
    int allyHp = 100;
    ArenaChairs chairs{};
};

Rig& placeAlly(Rig& rig, const AllySetup& s) {
    rig.fight.setAlly(s.allyHp, 0.4f);
    rig.fight.setChairs(s.chairs);
    rig.bodies.ally = Vec2{s.allyAt, 0.f};
    rig.bodies.enemies[0] = Vec2{s.allyAt + 2.f, 0.f};
    return rig;
}

int landTick(const AllySetup& s) {
    Rig rig({biterKind(s.damage, 10.f)});
    placeAlly(rig, s);
    for (int k = 1; k <= 120; ++k) {
        rig.tick();
        if (rig.count(ArenaEventType::AllyHit) > 0) return k;
    }
    return -1;
}

// Runs the bite with take half pressed on tick `pressTick` (0: never); stops after the land.
void runTakeHalf(Rig& rig, const AllySetup& s, int pressTick, int land) {
    placeAlly(rig, s);
    for (int k = 1; k <= land + 2; ++k) {
        if (k == pressTick) rig.in.takeHalf = true;
        rig.tick();
    }
}

} // namespace

TEST_CASE("arena carry: the Load cap follows the named places; the Release opens above a third of it (S1, S2)") {
    {   // three named: 100, closed at 33, open at 34
        Rig rig({dummyKind()});
        rig.bodies.enemies[0] = Vec2{20.f, 0.f};
        rig.fight.setLoadCap(namedPlaces(3).carryCap());
        CHECK(rig.fight.loadCap() == 100);
        CHECK(rig.fight.releaseOpenLoad() == 33);
        rig.tick(); // into the fight: giveLoad gives nothing before it
        CHECK(rig.fight.giveLoad(33) == 33);
        CHECK_FALSE(rig.release());
        rig.fight.giveLoad(1);
        CHECK(rig.release());
    }
    {   // none named: 40, closed at 13, open at 14
        Rig rig({dummyKind()});
        rig.bodies.enemies[0] = Vec2{20.f, 0.f};
        rig.fight.setLoadCap(namedPlaces(0).carryCap());
        CHECK(rig.fight.loadCap() == 40);
        CHECK(rig.fight.releaseOpenLoad() == 13);
        rig.tick(); // into the fight: giveLoad gives nothing before it
        CHECK(rig.fight.giveLoad(13) == 13);
        CHECK_FALSE(rig.release());
        rig.fight.giveLoad(1);
        CHECK(rig.release());
        rig.tick();
        CHECK(rig.fight.output().player.loadCap == 40);
    }
}

TEST_CASE("arena carry: setLoadCap clamps to 1..100 and never moves the Load (S3)") {
    Rig rig({dummyKind()});
    rig.fight.giveLoad(30);
    rig.fight.setLoadCap(0);
    CHECK(rig.fight.loadCap() == 1);
    CHECK(rig.fight.load() == 30);
    rig.fight.setLoadCap(250);
    CHECK(rig.fight.loadCap() == kMaxLoad);
    CHECK(rig.fight.load() == 30);
}

TEST_CASE("arena carry: a parry stores up to the cap only, and Overloaded comes once (S4, S5)") {
    {   // cap 60, Load 50, a parried 22: 10 stored
        Rig rig({biterKind(22)});
        rig.fight.setLoadCap(60);
        rig.fight.giveLoad(50);
        rig.parry();
        REQUIRE(rig.count(ArenaEventType::Parried) == 1);
        CHECK(rig.storedByEnemy() == 10);
        CHECK(rig.fight.load() == 60);
        CHECK(rig.fight.overloaded());
        CHECK(rig.count(ArenaEventType::Overloaded) == 1);
    }
    {   // already at the cap: nothing stored, no second Overloaded
        Rig rig({biterKind(22)});
        rig.fight.setLoadCap(60);
        rig.fight.giveLoad(60);
        rig.parry();
        REQUIRE(rig.count(ArenaEventType::Parried) == 1);
        CHECK(rig.storedByEnemy() == 0);
        CHECK(rig.fight.load() == 60);
        CHECK(rig.count(ArenaEventType::Overloaded) == 1);
    }
}

TEST_CASE("arena carry: a cap lowered under the Load keeps it, overloads once, and a Release spends it all (S6)") {
    Rig rig({biterKind(22)});
    rig.fight.giveLoad(70);
    rig.tick();
    rig.fight.setLoadCap(60);
    CHECK(rig.fight.load() == 70);
    CHECK(rig.fight.overloaded());
    rig.parry();
    CHECK(rig.count(ArenaEventType::Overloaded) == 1);
    REQUIRE(rig.count(ArenaEventType::Parried) == 1);
    CHECK(rig.storedByEnemy() == 0);
    CHECK(rig.fight.load() == 70);
    REQUIRE(rig.release());
    for (const ArenaEvent& e : rig.log)
        if (e.type == ArenaEventType::Released) CHECK(e.amount == 70);
    CHECK(rig.fight.load() == 0);
}

TEST_CASE("arena carry: a block stores blockLoadShare of the blow while carrying is on (S7, S8)") {
    const auto blockRun = [](Rig& rig) {
        rig.in.guard = true; // held from the start: the parry window is long over at the land
        rig.run(42);
    };
    {   // 11 blocked at 0.25: 3 (2.75 rounded)
        Rig rig({biterKind(11)});
        rig.fight.setCarry(true);
        blockRun(rig);
        REQUIRE(rig.count(ArenaEventType::Blocked) == 1);
        CHECK(rig.count(ArenaEventType::Parried) == 0);
        CHECK(rig.fight.load() == 3);
        CHECK(rig.storedByEnemy() == 3);
        CHECK(rig.fight.stats().blockLoad == 3);
        CHECK(rig.fight.stats().loadStored == 0);
    }
    {   // blockLoadShare 0: nothing
        ArenaFighter f = ruleFighter();
        f.blockLoadShare = 0.f;
        Rig rig({biterKind(30)}, f);
        rig.fight.setCarry(true);
        blockRun(rig);
        REQUIRE(rig.count(ArenaEventType::Blocked) == 1);
        CHECK(rig.fight.load() == 0);
        CHECK(rig.count(ArenaEventType::LoadStored) == 0);
    }
    {   // carrying off (the default fight): a block stores nothing, the cap stays 100
        Rig rig({biterKind(30)});
        CHECK_FALSE(rig.fight.carry());
        CHECK(rig.fight.loadCap() == kMaxLoad);
        blockRun(rig);
        REQUIRE(rig.count(ArenaEventType::Blocked) == 1);
        CHECK(rig.fight.load() == 0);
    }
}

TEST_CASE("arena carry: a guard-breaking block still stores; a parry stores its own and no block Load (S9)") {
    {
        ArenaFighter f = ruleFighter();
        f.stamina = 25.f;
        ArenaEnemyKind k = biterKind(30);
        k.moves[0].attack.guardDamage = 30.f;
        Rig rig({k}, f);
        rig.fight.setCarry(true);
        rig.in.guard = true;
        rig.run(42);
        REQUIRE(rig.count(ArenaEventType::GuardBroken) == 1);
        CHECK(rig.fight.load() == 8); // 7.5 rounded
        CHECK(rig.fight.stats().blockLoad == 8);
    }
    {
        Rig rig({biterKind(22)});
        rig.fight.setCarry(true);
        rig.parry();
        REQUIRE(rig.count(ArenaEventType::Parried) == 1);
        CHECK(rig.fight.load() == 22);
        CHECK(rig.fight.stats().blockLoad == 0);
        CHECK(rig.fight.stats().loadStored == 22);
    }
}

TEST_CASE("arena take half: pressed just before a blow lands on the ally, the player takes (D + 1) / 2 (S10, S11)") {
    for (const int damage : {22, 11}) {
        CAPTURE(damage);
        AllySetup s;
        s.damage = damage;
        const int land = landTick(s);
        REQUIRE(land > 6);
        Rig rig({biterKind(damage, 10.f)});
        runTakeHalf(rig, s, land - 5, land); // pressed 0.1 s before the land
        const int lead = (damage + 1) / 2;
        REQUIRE(rig.count(ArenaEventType::TookHalf) == 1);
        CHECK(rig.find(ArenaEventType::TookHalf)->amount == lead);
        CHECK(rig.find(ArenaEventType::TookHalf)->target == kArenaPlayer);
        CHECK(rig.fight.hp() == 100 - lead);
        CHECK(rig.fight.allyHp() == 100 - (damage - lead));
        REQUIRE(rig.find(ArenaEventType::AllyHit));
        CHECK(rig.find(ArenaEventType::AllyHit)->target == kArenaAlly);
        CHECK(rig.find(ArenaEventType::AllyHit)->amount == damage - lead);
        CHECK(rig.fight.load() == lead);
        CHECK(rig.fight.stats().takeHalfLoad == lead);
        CHECK(rig.fight.stats().takeHalves == 1);
        CHECK(rig.count(ArenaEventType::Hit) == 0); // no hitstun or Hit: the player was never struck
    }
}

TEST_CASE("arena take half: the HUD view's tookHalf is set on the tick it lands only; the ally's down shows") {
    AllySetup s;
    s.allyHp = 10; // the ally's 11 of the bite downs it
    const int land = landTick(s);
    REQUIRE(land > 6);
    Rig rig({biterKind(22, 10.f)});
    placeAlly(rig, s);
    ArenaHudLead lead;
    ArenaHudPartner partner;
    for (int k = 1; k < land; ++k) {
        if (k == land - 5) rig.in.takeHalf = true;
        rig.tick();
        fillArenaHudLead(rig.fight, lead);
        CHECK_FALSE(lead.tookHalf);
    }
    rig.tick();
    fillArenaHudLead(rig.fight, lead);
    fillArenaHudPartner(rig.fight, partner);
    CHECK(lead.tookHalf);
    CHECK(partner.present);
    CHECK(partner.down);
    CHECK(partner.hp == 0);
    rig.tick();
    fillArenaHudLead(rig.fight, lead);
    CHECK_FALSE(lead.tookHalf);
}

TEST_CASE("arena take half: too early or out of reach, the ally takes it all (S12)") {
    {   // pressed 0.28 s before the land: the window is 0.25 s
        AllySetup s;
        const int land = landTick(s);
        REQUIRE(land > 17);
        Rig rig({biterKind(22, 10.f)});
        runTakeHalf(rig, s, land - 16, land);
        CHECK(rig.count(ArenaEventType::TookHalf) == 0);
        CHECK(rig.fight.allyHp() == 78);
        CHECK(rig.fight.hp() == 100);
        CHECK(rig.fight.load() == 0);
    }
    {   // the ally 4.01 m off: out of takeHalfReach
        AllySetup s;
        s.allyAt = 4.01f;
        const int land = landTick(s);
        REQUIRE(land > 6);
        Rig rig({biterKind(22, 10.f)});
        runTakeHalf(rig, s, land - 5, land);
        CHECK(rig.count(ArenaEventType::TookHalf) == 0);
        CHECK(rig.fight.allyHp() == 78);
        CHECK(rig.fight.hp() == 100);
    }
    {   // no press: the ally takes it all; nothing else changes
        AllySetup s;
        const int land = landTick(s);
        Rig rig({biterKind(22, 10.f)});
        runTakeHalf(rig, s, 0, land);
        CHECK(rig.count(ArenaEventType::AllyHit) == 1);
        CHECK(rig.fight.allyHp() == 78);
        CHECK(rig.fight.hp() == 100);
    }
}

TEST_CASE("arena take half: refused inside a dodge's i-frames and while hurt; a dodge past its i-frames may take") {
    // A slow bite (0.8 s startup), so a dodge can begin and end its i-frames before it lands.
    AllySetup s;
    ArenaEnemyKind slow = biterKind(22, 10.f);
    slow.moves[0].attack.startup = 0.8f;
    int land = -1;
    {
        Rig probe({slow});
        placeAlly(probe, s);
        for (int k = 1; k <= 120 && land < 0; ++k) {
            probe.tick();
            if (probe.count(ArenaEventType::AllyHit) > 0) land = k;
        }
    }
    REQUIRE(land > 30);
    {   // the dodge 10 ticks (0.17 s) before the land: the bite lands inside 0.1..0.4 s of it
        Rig rig({slow});
        placeAlly(rig, s);
        bool invulnAtLand = false;
        for (int k = 1; k <= land + 2; ++k) {
            if (k == land - 10) rig.in.dodge = true;
            if (k == land - 5) rig.in.takeHalf = true;
            rig.tick();
            if (k == land) invulnAtLand = rig.fight.output().player.invulnerable;
        }
        CHECK(invulnAtLand);
        CHECK(rig.count(ArenaEventType::TookHalf) == 0);
        CHECK(rig.fight.allyHp() == 78);
        CHECK(rig.fight.hp() == 100);
        CHECK(rig.fight.load() == 0);
    }
    {   // the dodge 28 ticks (0.47 s) before: still dodging, its i-frames over, so the press takes half
        Rig rig({slow});
        placeAlly(rig, s);
        ArenaAction atLand = ArenaAction::Idle;
        for (int k = 1; k <= land + 2; ++k) {
            if (k == land - 28) rig.in.dodge = true;
            if (k == land - 5) rig.in.takeHalf = true;
            rig.tick();
            if (k == land) atLand = rig.fight.output().player.action;
        }
        CHECK(atLand == ArenaAction::Dodge);
        CHECK(rig.count(ArenaEventType::TookHalf) == 1);
        CHECK(rig.fight.allyHp() == 89);
        CHECK(rig.fight.hp() == 89);
    }
    {   // a second biter, close by the player, bites it first: the player is in hitstun when the ally's bite lands
        ArenaEnemyKind quick = biterKind(5, 2.f);
        quick.moves[0].attack.startup = 0.25f;
        quick.moves[0].attack.poiseDamage = 40.f; // past the player's 30 poise: it hurts (hitstun)
        Rig rig({biterKind(22, 10.f), quick});
        rig.def.attackers = 2;
        rig.fight.begin(rig.def, ArenaRing{Vec2{0.f, 0.f}, 30.f}, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 1);
        placeAlly(rig, s);
        rig.bodies.enemies[1] = Vec2{-1.5f, 0.f};
        ArenaAction atLand = ArenaAction::Idle;
        int allyLand = -1;
        for (int k = 1; k <= 120 && allyLand < 0; ++k) {
            if (rig.count(ArenaEventType::Hit) > 0 && rig.fight.output().player.action == ArenaAction::Hitstun)
                rig.in.takeHalf = true; // pressed every tick while hurt
            rig.tick();
            if (rig.count(ArenaEventType::AllyHit) > 0) {
                allyLand = k;
                atLand = rig.fight.output().player.action;
            }
        }
        REQUIRE(allyLand > 0);
        REQUIRE(rig.count(ArenaEventType::Hit) >= 1);
        CHECK(atLand == ArenaAction::Hitstun);
        CHECK(rig.count(ArenaEventType::TookHalf) == 0);
        CHECK(rig.fight.allyHp() == 78);
    }
}

TEST_CASE("arena take half: a takeHalf chair widens the window and heals the ally (S13)") {
    AllySetup s;
    s.damage = 20;
    s.allyHp = 135;
    s.chairs.named[0] = ChairEffect{ChairKind::TakeHalf, 0.1f, 0.05f};
    s.chairs.count = 1;
    const int land = landTick(s);
    REQUIRE(land > 18);
    Rig rig({biterKind(20, 10.f)});
    runTakeHalf(rig, s, land - 17, land); // 0.3 s before: inside 0.25 + 0.1
    REQUIRE(rig.count(ArenaEventType::TookHalf) == 1);
    CHECK(rig.fight.hp() == 90);
    CHECK(rig.fight.allyHp() == 135 - 10 + 7); // 0.05 * 135 = 6.75, rounded
    CHECK(rig.fight.load() == 10);
}

TEST_CASE("arena take half: an ally at 0 hp is down for good and the fight goes on") {
    AllySetup s;
    s.allyHp = 10;
    const int land = landTick(s);
    REQUIRE(land > 0);
    Rig rig({biterKind(22, 10.f)});
    runTakeHalf(rig, s, 0, land);
    CHECK(rig.count(ArenaEventType::AllyDowned) == 1);
    CHECK_FALSE(rig.fight.allyAlive());
    CHECK(rig.fight.allyHp() == 0);
    CHECK(rig.count(ArenaEventType::Defeat) == 0);
    CHECK(rig.fight.phase() == ArenaPhase::Fighting);
    CHECK_FALSE(rig.fight.takeHalfDue(1.f));
}

TEST_CASE("arena take half: takeHalfDue holds in the last `lead` s of a startup aimed past the ally") {
    AllySetup s;
    const int land = landTick(s);
    Rig rig({biterKind(22, 10.f)});
    placeAlly(rig, s);
    int firstDue = -1;
    for (int k = 1; k <= land; ++k) {
        rig.tick();
        if (firstDue < 0 && rig.fight.takeHalfDue(0.1f)) firstDue = k;
    }
    REQUIRE(firstDue > 0);
    CHECK(firstDue >= land - 8);
    CHECK(firstDue < land);
}

TEST_CASE("arena carry: a loadGain chair adds its share while the ally stands within its radius (S14)") {
    for (const float at : {4.5f, 3.9f}) {
        CAPTURE(at);
        Rig rig({biterKind(20)});
        ArenaChairs c;
        c.named[0] = ChairEffect{ChairKind::LoadGain, 0.25f, 4.f};
        c.count = 1;
        rig.fight.setChairs(c);
        rig.fight.setAlly(135, 0.4f);
        rig.bodies.ally = Vec2{0.f, at}; // off to the side: the biter at 1.5 m never reaches it
        rig.parry();
        REQUIRE(rig.count(ArenaEventType::Parried) == 1);
        CHECK(rig.count(ArenaEventType::AllyHit) == 0);
        CHECK(rig.fight.load() == (at > 4.f ? 20 : 25));
    }
}

TEST_CASE("arena carry: spendLoad removes what there is and reports it once") {
    Rig rig({dummyKind()});
    rig.fight.giveLoad(20);
    rig.tick();
    CHECK(rig.fight.spendLoad(30) == 20);
    CHECK(rig.fight.load() == 0);
    CHECK(rig.fight.spendLoad(0) == 0);
    rig.tick();
    rig.tick();
    REQUIRE(rig.count(ArenaEventType::LoadSpent) == 1);
    CHECK(rig.find(ArenaEventType::LoadSpent)->amount == 20);
}

// ---- the default fight is unchanged ----

namespace {

// The shipped fight under the autopilot to its end (or 120 s): its result line and a hash over every event and the
// player's hp, Load and stamina each tick. `carryNeutral` calls the carrying setters at their defaults (carry off,
// cap 100, no chairs, no ally) before begin and presses take half every 7th tick: none of it may change the fight.
struct FightTrace {
    std::string line;
    std::uint64_t hash = 1469598103934665603ull;
};

void mix(std::uint64_t& h, std::int64_t v) {
    h ^= static_cast<std::uint64_t>(v);
    h *= 1099511628211ull;
}

FightTrace traceOf(const ArenaDef& def, const ArenaTable& table, bool carryNeutral) {
    const ArenaRing ring = arenaRingAt(Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, def.ringRadius, ArenaRing{Vec2{0.f, 0.f}, 36.f});
    ArenaFight fight;
    if (carryNeutral) {
        fight.setCarry(false);
        fight.setLoadCap(kMaxLoad);
        fight.setChairs(ArenaChairs{});
    }
    fight.begin(def, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 7);
    ArenaBodies bodies;
    bodies.player = fight.playerStart();
    for (std::size_t i = 0; i < fight.enemyCount(); ++i) bodies.enemies[i] = fight.enemyStart(i);
    FightTrace t;
    int n = 0;
    while (!fight.over() && fight.time() < 120.f) {
        ArenaInput in = fight.autopilot();
        if (carryNeutral && ++n % 7 == 0) in.takeHalf = true;
        const ArenaOutput& out = fight.tick(kDt, in, bodies);
        for (const ArenaEvent& e : out.events) {
            mix(t.hash, static_cast<std::int64_t>(e.type));
            mix(t.hash, e.source);
            mix(t.hash, e.target);
            mix(t.hash, e.move);
            mix(t.hash, e.amount);
        }
        mix(t.hash, fight.hp());
        mix(t.hash, fight.load());
        mix(t.hash, static_cast<std::int64_t>(std::lround(fight.stamina() * 1000.f)));
        arenaStep(fight, bodies, kDt);
    }
    char buf[kArenaResultLineMax] = {};
    ArenaLogIds ids;
    ids.kit = "default";
    ids.pilot = "autoplay";
    ids.seed = 7;
    const std::size_t len = arenaResultLine(buf, sizeof buf, fight, table, ids);
    t.line.assign(buf, len);
    return t;
}

} // namespace

TEST_CASE("arena carry: with carrying off, the default fight and the kite boss fight play exactly as before") {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/arena.json", text)) FAIL("Data/Battle/arena.json not found");
    ArenaTable table;
    std::string err;
    REQUIRE_MESSAGE(importArenaTable(text, table, &err), err);

    // The default fight.
    const FightTrace plain = traceOf(table.fallback, table, false);
    const FightTrace neutral = traceOf(table.fallback, table, true);
    CHECK(plain.line == neutral.line);
    CHECK(plain.hash == neutral.hash);

    // The kite boss alone against the default kit.
    std::size_t kite = 0;
    while (kite < table.enemyIds.size() && table.enemyIds[kite] != "kite_boss") ++kite;
    REQUIRE(kite < table.enemyIds.size());
    ArenaDef def = table.fallback;
    def.enemies[0] = table.enemies[kite];
    def.enemyKind = arenaNoKinds();
    def.enemyKind[0] = static_cast<std::uint8_t>(kite);
    def.enemyCount = 1;
    const FightTrace kitePlain = traceOf(def, table, false);
    const FightTrace kiteNeutral = traceOf(def, table, true);
    CHECK(kitePlain.line == kiteNeutral.line);
    CHECK(kitePlain.hash == kiteNeutral.hash);
}

TEST_CASE("arena take half: refused from the air (no one takes a blow for someone mid-jump); on the ground again it takes") {
    AllySetup s;
    ArenaEnemyKind slow = biterKind(22, 10.f);
    slow.moves[0].attack.startup = 0.8f;
    int land = -1;
    {
        Rig probe({slow});
        placeAlly(probe, s);
        for (int k = 1; k <= 120 && land < 0; ++k) {
            probe.tick();
            if (probe.count(ArenaEventType::AllyHit) > 0) land = k;
        }
    }
    REQUIRE(land > 46);
    {   // the jump 20 ticks (0.33 s) before the land: in the air when the bite lands
        Rig rig({slow});
        placeAlly(rig, s);
        float heightAtLand = 0.f;
        for (int k = 1; k <= land + 2; ++k) {
            if (k == land - 20) rig.in.jump = true;
            if (k == land - 5) rig.in.takeHalf = true;
            rig.tick();
            if (k == land) heightAtLand = rig.fight.output().player.height;
        }
        CHECK(heightAtLand > 0.f);
        CHECK(rig.count(ArenaEventType::TookHalf) == 0);
        CHECK(rig.fight.allyHp() == 78);
        CHECK(rig.fight.hp() == 100);
    }
    {   // the jump 45 ticks (0.75 s) before: landed (0.7 s), so the press takes half
        Rig rig({slow});
        placeAlly(rig, s);
        for (int k = 1; k <= land + 2; ++k) {
            if (k == land - 45) rig.in.jump = true;
            if (k == land - 5) rig.in.takeHalf = true;
            rig.tick();
        }
        CHECK(rig.fight.stats().jumps == 1);
        CHECK(rig.count(ArenaEventType::TookHalf) == 1);
        CHECK(rig.fight.allyHp() == 89);
    }
}

TEST_CASE("lone carry: a hit of 22 takes 25, a block's chip is from the scaled hp, a parry of 22 stores 33") {
    CHECK(loneScale(22, 1.15f) == 25);
    CHECK(loneScale(36, 1.3f) == 47);
    CHECK(loneScale(0, 1.15f) == 0);
    CHECK(loneScale(-5, 1.15f) == 0);
    CHECK(loneScale(22, 0.5f) == 22); // a scale below 1 is not one
    CHECK(loneScale(2000000, 2.f) == 3000000);
    {   // a landed hit
        Rig rig({biterKind(22)});
        rig.fight.setLone(true);
        CHECK(rig.fight.lone());
        rig.run(42);
        REQUIRE(rig.count(ArenaEventType::Hit) == 1);
        CHECK(rig.fight.hp() == 75);
        CHECK(rig.find(ArenaEventType::Hit)->amount == 25);
    }
    {   // not Lone (the default): 22
        Rig rig({biterKind(22)});
        CHECK_FALSE(rig.fight.lone());
        rig.run(42);
        CHECK(rig.fight.hp() == 78);
    }
    {   // a block's chip: 40 is 46 while Lone, 25% of it 12 (10 not Lone); the block's Load reads the 40
        Rig rig({biterKind(40)});
        rig.fight.setLone(true);
        rig.fight.setCarry(true);
        rig.in.guard = true;
        rig.run(42);
        REQUIRE(rig.count(ArenaEventType::Blocked) == 1);
        CHECK(rig.fight.hp() == 88);
        CHECK(rig.fight.load() == 15); // 40 x 0.25 x 1.5
    }
    {   // a parry of 22 stores 22 x 1.5 = 33, and takes no hp
        Rig rig({biterKind(22)});
        rig.fight.setLone(true);
        rig.parry();
        REQUIRE(rig.count(ArenaEventType::Parried) == 1);
        CHECK(rig.fight.load() == 33);
        CHECK(rig.fight.hp() == 100);
    }
}

TEST_CASE("lone carry: the cap is 100 whatever setLoadCap says; ending it at 80 under a cap of 40 overloads once") {
    Rig rig({biterKind(22)});
    rig.fight.setLoadCap(40);
    rig.fight.setLone(true);
    CHECK(rig.fight.loadCap() == 100);
    CHECK(rig.fight.releaseOpenLoad() == 33);
    rig.fight.giveLoad(80);
    rig.tick();
    CHECK(rig.fight.load() == 80);
    CHECK_FALSE(rig.fight.overloaded());
    CHECK(rig.count(ArenaEventType::Overloaded) == 0);
    rig.fight.setLone(false); // a gift given: back to the chairs' cap
    CHECK(rig.fight.loadCap() == 40);
    CHECK(rig.fight.load() == 80);
    CHECK(rig.fight.overloaded());
    rig.tick();
    CHECK(rig.count(ArenaEventType::Overloaded) == 1);
    CHECK(rig.fight.stats().overloads == 1);
    // A refused tuning turns it off.
    LoneTuning bad;
    bad.gain = 4.f;
    rig.fight.setLone(true, bad);
    CHECK_FALSE(rig.fight.lone());
}

TEST_CASE("lone carry: stores gain the stance's x1.5 in place of the chairs'; giveLoad stays ungained") {
    ArenaChairs chairs;
    chairs.named[0] = ChairEffect{ChairKind::LoadGain, 0.25f, 15.f};
    chairs.named[1] = ChairEffect{ChairKind::HealCharges, 2.f, 0.f};
    chairs.count = 2;
    Rig rig({biterKind(10)});
    rig.fight.setAlly(100, 0.4f);
    rig.bodies.ally = Vec2{0.f, 2.f};
    rig.fight.setChairs(chairs);
    rig.fight.setLone(true);
    rig.fight.restart();
    CHECK(rig.fight.healChargesMax() == ruleFighter().healCharges); // the chairs' charges are left out
    rig.parry();
    REQUIRE(rig.count(ArenaEventType::Parried) == 1);
    CHECK(rig.fight.load() == 15); // 10 x 1.5, not x 1.25
    rig.fight.giveLoad(10);
    rig.tick();
    CHECK(rig.fight.load() == 25);
    rig.fight.setLone(false);
    rig.fight.restart();
    CHECK(rig.fight.healChargesMax() == ruleFighter().healCharges + 2);
}

TEST_CASE("lone carry: the Release's hp damage is x1.3 on its base") {
    for (const bool lone : {false, true}) {
        CAPTURE(lone);
        Rig rig({dummyKind()});
        rig.fight.setLone(lone);
        rig.fight.giveLoad(50);
        rig.tick();
        REQUIRE(rig.release());
        int dealt = -1;
        for (const ArenaEvent& e : rig.log)
            if (e.type == ArenaEventType::Hit && e.source == kArenaPlayer && e.move == kArenaReleaseMove) dealt = e.amount;
        CHECK(dealt == (lone ? 26 : 20)); // 20 x 1.3
    }
}

TEST_CASE("lone carry: take half never happens and takeHalfDue stays false") {
    AllySetup s;
    const int land = landTick(s);
    REQUIRE(land > 6);
    Rig rig({biterKind(s.damage, 10.f)});
    rig.fight.setLone(true);
    placeAlly(rig, s);
    bool due = false;
    for (int k = 1; k <= land + 2; ++k) {
        due = due || rig.fight.takeHalfDue(0.2f);
        if (k == land - 5) rig.in.takeHalf = true;
        rig.tick();
    }
    CHECK_FALSE(due);
    CHECK(rig.count(ArenaEventType::TookHalf) == 0);
    CHECK(rig.fight.allyHp() == 100 - s.damage);
    CHECK(rig.fight.hp() == 100);
}
