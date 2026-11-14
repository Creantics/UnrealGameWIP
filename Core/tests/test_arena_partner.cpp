// The partner (ArenaFight::setPartner): a PartnerBrain drives the ally body. Its strings and their damage rule, its
// guard, its rise, a boss's aim at it and that aim's cap, the order, the attack budget, the takeover, and that a
// fight without one is unchanged.

#include "ftcore/ArenaFight.h"
#include "ftcore/PartnerBrain.h"
#include "TestAssets.h"
#include <ostream>
#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 60.f;

Vec2 sub(Vec2 a, Vec2 b) { return Vec2{a.x - b.x, a.y - b.y}; }
float length(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
float dist(Vec2 a, Vec2 b) { return length(sub(a, b)); }

ArenaTable shippedTable() {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/arena.json", text)) FAIL("Data/Battle/arena.json not found");
    ArenaTable t;
    std::string err;
    REQUIRE_MESSAGE(importArenaTable(text, t, &err), err);
    return t;
}

PartnerTuning shippedTuning() {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/partner_brain.json", text)) FAIL("Data/Battle/partner_brain.json not found");
    PartnerTuning t;
    std::string err;
    REQUIRE_MESSAGE(importPartnerBrain(text, t, &err), err);
    return t;
}

ArenaDef shippedBoss(const ArenaTable& t, const char* id, bool harmless) {
    ArenaDef def;
    def.fighter = t.fighter("default");
    def.enemyCount = 1;
    def.attackers = 1;
    for (std::size_t e = 0; e < t.enemyIds.size(); ++e)
        if (t.enemyIds[e] == id) def.enemies[0] = t.enemies[e];
    REQUIRE(def.enemies[0].boss);
    if (harmless)
        for (std::size_t m = 0; m < def.enemies[0].moveCount; ++m) {
            ArenaAttack& a = def.enemies[0].moves[m].attack;
            a.damage = 0;
            a.poiseDamage = 0.f;
            a.guardDamage = 0.f;
        }
    return def;
}

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

// The rule kit of test_arena.cpp: lights of 10, 11 and 12 hp, each 0.6 s long.
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

// Never attacks.
ArenaEnemyKind dummyKind(int hp = 100000) {
    ArenaEnemyKind k;
    k.hp = hp;
    k.poise = 30.f;
    k.poiseDelay = 1.f;
    k.staggerTime = 1.f;
    k.openTime = 1.5f;
    k.downTime = 1.f;
    k.attackDelay = 1000.f;
    k.bodyRadius = 0.45f;
    return k;
}

// One full-circle bite (0.4 s startup, range 2) while the player is within `maxRange`, once per `cooldown`.
ArenaEnemyKind biterKind(int damage, float maxRange, float cooldown) {
    ArenaEnemyKind k = dummyKind();
    k.attackDelay = 0.f;
    k.preferredRange = maxRange;
    k.runRange = maxRange + 5.f;
    k.moveCount = 1;
    ArenaEnemyMove& m = k.moves[0];
    m.attack = swing(0.4f, 0.15f, 0.5f, 2.f, damage);
    m.minRange = 0.f;
    m.maxRange = maxRange;
    m.cooldown = cooldown;
    m.weight = 1.f;
    m.chain = -1;
    m.unblockable = false;
    m.parryable = true;
    return k;
}

// A boss that bites every second or so (no break move).
ArenaEnemyKind bossKind(int damage) {
    ArenaEnemyKind k = biterKind(damage, 3.f, 1.f);
    k.boss = true;
    k.breakMove = -1;
    return k;
}

struct Logged {
    ArenaEvent e;
    float t = 0.f;         // fight time at the end of its tick
    std::uint8_t aim = 0;  // enemy 0's aim that tick
};

// A host: the bodies stay where they are put unless `step` (arenaStep moves the player and the enemies) or `walk`
// (the ally walks toward ArenaOutput::ally.goal at the lead fighter's moveSpeed, off the living enemies, inside the
// ring). On TookOver it swaps the player's and the ally's bodies, as the game's host does.
struct PRig {
    ArenaDef def;
    ArenaRing ring{Vec2{0.f, 0.f}, 30.f};
    ArenaFight fight;
    ArenaBodies bodies;
    ArenaInput in;
    PartnerBrain brain;
    PartnerTuning tuning;
    std::vector<Logged> log;
    const ArenaOutput* out = nullptr;
    bool step = false;
    bool walk = false;
    bool autopilot = false;

    explicit PRig(const ArenaFighter& fighter = ruleFighter()) {
        def.fighter = fighter;
        def.enemyCount = 0; // enemy() adds them
        def.attackers = 1;
    }
    void enemy(const ArenaEnemyKind& k, Vec2 at) {
        bodies.enemies[def.enemyCount] = at;
        def.enemies[def.enemyCount++] = k;
    }
    // A custom fight: the bodies are set first.
    void go(bool partner, int allyHp = 100, std::uint32_t seed = 1, Vec2 facing = Vec2{1.f, 0.f}) {
        fight.setAlly(allyHp, 0.4f);
        if (partner) fight.setPartner(&brain, &tuning);
        fight.begin(def, ring, bodies.player, facing, seed);
    }
    // A shipped fight: its own ring and starts, the ally 1.5 m to the player's side.
    void goShipped(bool partner, std::uint32_t seed) {
        ring = arenaRingAt(Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, def.ringRadius, ArenaRing{Vec2{0.f, 0.f}, 36.f});
        fight.setAlly(def.fighter.hp, 0.4f);
        if (partner) fight.setPartner(&brain, &tuning);
        fight.begin(def, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, seed);
        bodies.player = fight.playerStart();
        for (std::size_t i = 0; i < fight.enemyCount(); ++i) bodies.enemies[i] = fight.enemyStart(i);
        const Vec2 f = fight.playerStartFacing();
        bodies.ally = ring.clamp(Vec2{bodies.player.x - 1.5f * f.y, bodies.player.y + 1.5f * f.x}, 0.4f);
        step = walk = autopilot = true;
    }
    void tick() {
        if (autopilot) {
            const std::uint8_t order = in.partnerOrder;
            in = fight.autopilot();
            in.partnerOrder = order;
        }
        out = &fight.tick(kDt, in, bodies);
        for (const ArenaEvent& e : out->events) {
            log.push_back(Logged{e, fight.time(), out->enemies[0].aim});
            if (e.type == ArenaEventType::TookOver) std::swap(bodies.player, bodies.ally);
        }
        if (step) arenaStep(fight, bodies, kDt);
        if (walk && out->ally.partnerOn) walkAlly();
        if (!autopilot) {
            in.light = in.heavy = in.dodge = in.jump = in.heal = in.lock = in.release = in.pair = in.takeHalf = false;
            in.lockSwitch = 0;
            in.read = -1;
        }
    }
    void walkAlly() {
        const Vec2 d = sub(out->ally.goal, bodies.ally);
        const float far = length(d);
        const float most = def.fighter.moveSpeed * kDt;
        if (far > most) bodies.ally = Vec2{bodies.ally.x + d.x * most / far, bodies.ally.y + d.y * most / far};
        else bodies.ally = out->ally.goal;
        for (std::size_t i = 0; i < fight.enemyCount(); ++i) {
            if (!fight.enemyAlive(i)) continue;
            const float least = 0.4f + def.enemies[i].bodyRadius;
            const Vec2 e = sub(bodies.ally, bodies.enemies[i]);
            const float apart = length(e);
            if (apart >= least) continue;
            const Vec2 dir = apart > 1e-4f ? Vec2{e.x / apart, e.y / apart} : Vec2{1.f, 0.f};
            bodies.ally = Vec2{bodies.enemies[i].x + dir.x * least, bodies.enemies[i].y + dir.y * least};
        }
        bodies.ally = fight.ring().clamp(bodies.ally, 0.4f);
    }
    void run(float seconds) {
        const int n = static_cast<int>(std::lround(seconds / kDt));
        for (int i = 0; i < n && !fight.over(); ++i) tick();
    }
    int count(ArenaEventType type) const {
        int n = 0;
        for (const Logged& l : log)
            if (l.e.type == type) ++n;
        return n;
    }
    // The first such event after `after` (s), or -1; read it with log[i] (the log grows, so keep indices).
    int find(ArenaEventType type, float after = -1.f) const {
        for (std::size_t i = 0; i < log.size(); ++i)
            if (log[i].e.type == type && log[i].t > after) return static_cast<int>(i);
        return -1;
    }
};

struct Fnv {
    std::uint64_t h = 1469598103934665603ull;
    void byte(std::uint8_t b) {
        h ^= b;
        h *= 1099511628211ull;
    }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) byte(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void f32(float f) {
        std::uint32_t v = 0;
        std::memcpy(&v, &f, sizeof v);
        u32(v);
    }
};

// A 60 s autopilot fight with the ally body standing beside the player, hashed tick by tick. mode: 0 setAlly only;
// 1 setPartner(nullptr, nullptr); 2 a partner set, then cleared before begin; 3 a brain without tuning; 4 no ally and
// no partner; 5 a partner without an ally.
std::uint64_t hashAllyFight(const ArenaDef& def, int mode) {
    const ArenaRing ring = arenaRingAt(Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, def.ringRadius, ArenaRing{Vec2{0.f, 0.f}, 36.f});
    ArenaFight fight;
    PartnerBrain brain;
    const PartnerTuning tuning{};
    if (mode <= 3) fight.setAlly(def.fighter.hp, 0.4f);
    if (mode == 1) fight.setPartner(nullptr, nullptr);
    if (mode == 2) {
        fight.setPartner(&brain, &tuning);
        fight.setPartner(nullptr, nullptr);
    }
    if (mode == 3) fight.setPartner(&brain, nullptr);
    if (mode == 5) fight.setPartner(&brain, &tuning);
    fight.begin(def, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 1);
    ArenaBodies bodies;
    bodies.player = fight.playerStart();
    for (std::size_t i = 0; i < fight.enemyCount(); ++i) bodies.enemies[i] = fight.enemyStart(i);
    bodies.ally = ring.clamp(Vec2{bodies.player.x, bodies.player.y + 1.5f}, 0.4f);
    Fnv h;
    bool quiet = true; // the ally's output stays at rest
    while (!fight.over() && fight.time() < 60.f) {
        const ArenaOutput& out = fight.tick(kDt, fight.autopilot(), bodies);
        arenaStep(fight, bodies, kDt);
        if (out.ally.partnerOn || out.ally.attacking || out.ally.guarding || out.ally.goal.x != fight.allyPos().x ||
            out.ally.goal.y != fight.allyPos().y)
            quiet = false;
        for (const ArenaEvent& e : out.events) {
            h.byte(static_cast<std::uint8_t>(e.type));
            h.byte(e.source);
            h.byte(e.target);
            h.byte(e.move);
            h.u32(static_cast<std::uint32_t>(e.amount));
            h.f32(e.at.x);
            h.f32(e.at.y);
        }
        h.u32(static_cast<std::uint32_t>(fight.hp()));
        h.u32(static_cast<std::uint32_t>(fight.allyHp()));
        h.f32(bodies.player.x);
        h.f32(bodies.player.y);
        for (std::size_t i = 0; i < fight.enemyCount(); ++i) {
            h.f32(bodies.enemies[i].x);
            h.f32(bodies.enemies[i].y);
            h.u32(static_cast<std::uint32_t>(fight.enemyHp(i)));
            h.byte(out.enemies[i].aim == 2 ? 2 : 0);
        }
    }
    CHECK(quiet);
    const ArenaStats& s = fight.stats();
    CHECK(s.partnerDamage == 0);
    CHECK(s.partnerBlows == 0);
    CHECK(s.partnerStrings == 0);
    CHECK(s.partnerGuards == 0);
    CHECK(s.partnerRises == 0);
    CHECK(s.bossAimedPartner == 0);
    CHECK(s.takeovers == 0);
    CHECK_FALSE(fight.swapped());
    h.byte(static_cast<std::uint8_t>(fight.phase()));
    for (int v : {s.hits, s.damageDealt, s.damageTaken, s.staggers, s.breakOuts, s.parries, s.kills})
        h.u32(static_cast<std::uint32_t>(v));
    return h.h;
}

// Each started blow of the ally (from ArenaOutput::ally), and the lead's attacking ticks, as spans of fight time.
struct Span {
    double a = 0.0;
    double b = 0.0;
};

double clipped(const std::vector<Span>& spans, double w0, double w1) {
    double sum = 0.0;
    for (const Span& s : spans) sum += std::max(0.0, std::min(s.b, w1) - std::max(s.a, w0));
    return sum;
}

struct BlowWatch {
    bool open = false;
    std::uint8_t blow = 0;
    float last = 0.f;
    float blowLength = 0.f;
    bool guarded = false;
    int started = 0;
    int ranOut = 0;
    int cut = 0;
    int guardedBlows = 0;
    int guardUpInBlow = 0; // ticks its guard showed up while a blow ran (never)
    std::vector<Span> blows;
    std::vector<Span> lead;

    void close() {
        if (!open) return;
        open = false;
        if (last + kDt + 1e-4f >= blowLength) ++ranOut;
        else ++cut;
        if (guarded) ++guardedBlows;
    }
    void see(const ArenaOutput& out, const ArenaFighter& f, const PartnerTuning& t, float now) {
        const ArenaAllyOut& a = out.ally;
        if (a.attacking && (!open || a.blow != blow || a.stateTime < last)) {
            close();
            open = true;
            blow = a.blow;
            guarded = false;
            const float l = f.light[a.blow % std::max<int>(f.lightCount, 1)].total();
            blowLength = l >= 0.1f && l <= 2.f ? l : t.blowSeconds;
            const double start = static_cast<double>(now) - kDt;
            blows.push_back(Span{start, start + blowLength});
            ++started;
        }
        if (a.attacking) {
            last = a.stateTime;
            if (a.act == static_cast<std::uint8_t>(PartnerAct::Guard)) guarded = true;
            if (a.guarding) ++guardUpInBlow;
        } else {
            close();
        }
        if (out.player.action == ArenaAction::Light || out.player.action == ArenaAction::Heavy)
            lead.push_back(Span{static_cast<double>(now) - kDt, static_cast<double>(now)});
    }
    // The worst (partner - (0.6 x lead + allowance + 0.05)) over every window, its start stepped by 0.05 s.
    double worst(const PartnerTuning& t, double seconds) const {
        double w = -1e9;
        for (double s = 0.0; s + t.budgetWindowSeconds <= seconds + 1e-9; s += 0.05) {
            const double e = s + t.budgetWindowSeconds;
            const double over = clipped(blows, s, e) -
                                (t.budgetShare * clipped(lead, s, e) + t.budgetAllowanceSeconds + 0.05);
            w = std::max(w, over);
        }
        return w;
    }
};

// The dummy at the origin, the ally 1.4 m from it, the player far off and idle.
void stringRig(PRig& r, bool partner) {
    r.enemy(dummyKind(1000), Vec2{0.f, 0.f});
    r.bodies.player = Vec2{-10.f, 0.f};
    r.bodies.ally = Vec2{-1.4f, 0.f};
    r.go(partner);
}

// The boss at the origin, the player 1.5 m off on one side and the ally `allyAt` m off on the other.
void bossRig(PRig& r, int damage, float allyAt, std::uint32_t seed = 1) {
    r.enemy(bossKind(damage), Vec2{0.f, 0.f});
    r.def.fighter.hp = 1000; // the lead never falls here, so no takeover moves the bodies
    r.bodies.player = Vec2{-1.5f, 0.f};
    r.bodies.ally = Vec2{allyAt, 0.f};
    r.go(true, 100, seed);
}

struct HookLog {
    float scale = 1.f;
    bool veto = false;
    int calls = 0;
    int otherSources = 0;
    std::uint8_t target = kArenaNobody;
    float poise = -1.f;
    int firstRaw = -1;
    ArenaEventType type = ArenaEventType::Defeat;
};

ArenaBlowRuling allyHook(void* user, const ArenaBlow& blow) {
    HookLog& h = *static_cast<HookLog*>(user);
    ArenaBlowRuling r;
    if (blow.source != kArenaAlly) {
        ++h.otherSources;
        return r;
    }
    if (h.calls++ == 0) {
        h.target = blow.target;
        h.poise = blow.poise;
        h.firstRaw = blow.damage;
        h.type = blow.type;
    }
    r.damage = h.scale;
    r.veto = h.veto;
    return r;
}

} // namespace

TEST_CASE("arena partner: with no partner set, or a null one, a fight is unchanged") {
    const ArenaTable t = shippedTable();
    std::vector<ArenaDef> defs{t.fallback, shippedBoss(t, "stone_bearer", false)};
    for (const ArenaDef& def : defs) {
        const std::uint64_t a = hashAllyFight(def, 0);
        CHECK(hashAllyFight(def, 1) == a);
        CHECK(hashAllyFight(def, 2) == a);
        CHECK(hashAllyFight(def, 3) == a);
        // Without an ally a partner does nothing at all.
        CHECK(hashAllyFight(def, 5) == hashAllyFight(def, 4));
    }
}

TEST_CASE("arena partner: a string lands its blows for partnerBlowDamage and nothing else") {
    PRig r;
    stringRig(r, true);
    r.run(3.f);
    const ArenaStats& s = r.fight.stats();
    REQUIRE(s.partnerStrings >= 1);
    REQUIRE(r.count(ArenaEventType::AllyStrike) >= 2);
    CHECK(r.count(ArenaEventType::AllyStrike) == s.partnerBlows);
    int sum = 0;
    for (const Logged& l : r.log) {
        if (l.e.type != ArenaEventType::AllyStrike) continue;
        CHECK(l.e.source == kArenaAlly);
        CHECK(l.e.target == 0);
        const int raw = r.def.fighter.light[l.e.move % 3].damage;
        CHECK(l.e.amount == static_cast<int>(std::lround(partnerBlowDamage(static_cast<float>(raw), 10.f))));
        sum += l.e.amount;
    }
    // 10, 11 and 12 raw against a lead light of 10: 6 + the rest halved, 8, 9 (8.5 rounds up) and 9.
    CHECK(r.log[static_cast<std::size_t>(r.find(ArenaEventType::AllyStrike))].e.amount == 8);
    CHECK(r.fight.enemyHp(0) == 1000 - sum);
    CHECK(s.partnerDamage == sum);
    // No poise, no break count, nothing of the player's.
    CHECK(r.fight.enemyPoise(0) == doctest::Approx(30.f));
    CHECK(s.damageDealt == 0);
    CHECK(s.hits == 0);
    CHECK(s.staggers == 0);
    CHECK(s.breakOuts == 0);
    CHECK(s.breakUpright == 0);
    CHECK(s.flinchChances == 0);
    CHECK(r.fight.load() == 0);
    CHECK(r.count(ArenaEventType::Hit) == 0);
    CHECK(r.count(ArenaEventType::Staggered) == 0);
    CHECK(r.count(ArenaEventType::Flinched) == 0);
    CHECK(r.count(ArenaEventType::LoadStored) == 0);
}

TEST_CASE("arena partner: its blows on a held stagger never lengthen it") {
    int ends[2] = {0, 0};
    int strikes = 0;
    for (int partner = 0; partner < 2; ++partner) {
        PRig r;
        stringRig(r, partner == 1);
        REQUIRE(r.fight.holdEnemy(0, ArenaBrain::Staggered, 1.5f));
        bool seen = false;
        for (int k = 1; k <= 300; ++k) {
            r.tick();
            const bool staggered = r.out->enemies[0].brain == ArenaBrain::Staggered;
            if (staggered) seen = true;
            if (seen && !staggered) {
                ends[partner] = k;
                break;
            }
        }
        if (partner == 1) strikes = r.count(ArenaEventType::AllyStrike);
    }
    CHECK(ends[0] > 0);
    CHECK(ends[1] == ends[0]);
    CHECK(strikes >= 1);
}

TEST_CASE("arena partner: the part of a blow over 60% of the lead's light is halved, after the blow hook") {
    CHECK(partnerBlowDamage(20.f, 10.f) == doctest::Approx(13.f));
    CHECK(partnerBlowDamage(6.f, 10.f) == doctest::Approx(6.f));
    PRig r;
    HookLog h;
    r.fight.setBlowHook(&allyHook, &h);
    SUBCASE("a doubled blow") {
        h.scale = 2.f;
        stringRig(r, true);
        r.run(3.f);
        REQUIRE(r.count(ArenaEventType::AllyStrike) >= 2);
        CHECK(h.calls == r.count(ArenaEventType::AllyStrike));
        CHECK(h.type == ArenaEventType::Hit);
        CHECK(h.target == 0);
        CHECK(h.poise == 0.f);
        CHECK(h.firstRaw == 10);
        // 20 raw: 6 + 14 / 2.
        CHECK(r.log[static_cast<std::size_t>(r.find(ArenaEventType::AllyStrike))].e.amount == 13);
        int sum = 0;
        for (const Logged& l : r.log) {
            if (l.e.type != ArenaEventType::AllyStrike) continue;
            const int raw = 2 * r.def.fighter.light[l.e.move % 3].damage;
            CHECK(l.e.amount == static_cast<int>(std::lround(partnerBlowDamage(static_cast<float>(raw), 10.f))));
            sum += l.e.amount;
        }
        CHECK(r.fight.enemyHp(0) == 1000 - sum);
    }
    SUBCASE("a vetoed blow does nothing") {
        h.veto = true;
        stringRig(r, true);
        r.run(3.f);
        CHECK(h.calls >= 1);
        CHECK(r.fight.stats().partnerStrings >= 1);
        CHECK(r.count(ArenaEventType::AllyStrike) == 0);
        CHECK(r.fight.stats().partnerDamage == 0);
        CHECK(r.fight.stats().partnerBlows == 0);
        CHECK(r.fight.enemyHp(0) == 1000);
    }
}

TEST_CASE("arena partner: a boss string aimed at it meets its guard; without the guard it lands") {
    PRig r;
    // It never attacks here, so no blow of its own runs when a guard is due.
    r.tuning.budgetShare = 0.f;
    r.tuning.budgetAllowanceSeconds = 0.f;
    SUBCASE("guard always") {
        r.tuning.guardChance = 1.f;
        REQUIRE(validPartnerTuning(r.tuning));
        bossRig(r, 2, 1.2f);
        r.run(30.f);
        const ArenaStats& s = r.fight.stats();
        CHECK(s.takeovers == 0);
        CHECK(s.bossAimedPartner >= 2);
        CHECK(r.count(ArenaEventType::AllyBlocked) >= 2);
        CHECK(r.count(ArenaEventType::AllyBlocked) == s.partnerGuards);
        int lost = 0;
        for (const Logged& l : r.log) {
            if (l.e.type == ArenaEventType::AllyBlocked) {
                CHECK(l.e.source == 0);
                CHECK(l.e.target == kArenaAlly);
                CHECK(l.aim == 2);
            }
            if (l.e.type == ArenaEventType::AllyHit) {
                CHECK(l.aim == 1); // only the strings aimed at the lead reach it
                lost += l.e.amount;
            }
        }
        CHECK(r.fight.allyHp() == 100 - lost);
        CHECK(s.partnerStrings == 0);
    }
    SUBCASE("guard never") {
        r.tuning.guardChance = 0.f;
        bossRig(r, 2, 1.2f);
        r.run(30.f);
        CHECK(r.fight.stats().takeovers == 0);
        CHECK(r.fight.stats().bossAimedPartner >= 2);
        CHECK(r.count(ArenaEventType::AllyBlocked) == 0);
        CHECK(r.fight.stats().partnerGuards == 0);
        int aimedHits = 0;
        for (const Logged& l : r.log)
            if (l.e.type == ArenaEventType::AllyHit && l.aim == 2) ++aimedHits;
        CHECK(aimedHits >= 2);
    }
}

TEST_CASE("arena partner: a boss aims at most 30% of its strings at it, counted at every string's start") {
    for (std::uint32_t seed = 1; seed <= 3; ++seed) {
        CAPTURE(seed);
        {   // the ally nearer
            PRig r;
            bossRig(r, 0, 1.2f, seed);
            int attacks = 0;
            int aimTwoTicks = 0;
            for (int k = 0; k < 120 * 60; ++k) {
                r.tick();
                const ArenaStats& s = r.fight.stats();
                if (s.bossAttacks != attacks) {
                    attacks = s.bossAttacks;
                    CHECK(s.bossAimedPartner * 10 <= kArenaBossAimShareTenths * s.bossAttacks);
                }
                if (r.out->enemies[0].aim == 2) ++aimTwoTicks;
            }
            const ArenaStats& s = r.fight.stats();
            CHECK(s.bossAttacks >= 30);
            CHECK(s.bossAimedPartner >= 5);
            CHECK(s.bossAimShareMax <= 0.3f + 1e-6f);
            CHECK(s.bossAimShareMax > 0.f);
            CHECK(aimTwoTicks > 0);
        }
        {   // the ally further
            PRig r;
            bossRig(r, 0, 2.f, seed);
            int aimTwoTicks = 0;
            for (int k = 0; k < 60 * 60; ++k) {
                r.tick();
                if (r.out->enemies[0].aim == 2) ++aimTwoTicks;
            }
            CHECK(r.fight.stats().bossAttacks >= 15);
            CHECK(r.fight.stats().bossAimedPartner == 0);
            CHECK(r.fight.stats().bossAimShareMax == 0.f);
            CHECK(aimTwoTicks == 0);
        }
    }
}

TEST_CASE("arena partner: StayClose keeps it on the leash of a running lead; Spread lets it go") {
    for (int order = 0; order < 2; ++order) {
        CAPTURE(order);
        PRig r;
        r.enemy(dummyKind(), Vec2{0.f, -25.f});
        r.bodies.player = Vec2{6.f, 0.f};
        r.bodies.ally = Vec2{0.f, 0.f};
        r.walk = true;
        r.in.partnerOrder = static_cast<std::uint8_t>(order);
        r.go(true);
        bool returned = false;
        float worst = 0.f;
        float most = 0.f;
        for (int k = 1; k <= 40 * 60; ++k) {
            const float t = static_cast<float>(k) * kDt;
            r.bodies.player = Vec2{6.f * std::cos(0.5f * t), 6.f * std::sin(0.5f * t)}; // 3 m/s round a circle
            r.tick();
            const float d = dist(r.bodies.ally, r.bodies.player);
            most = std::max(most, d);
            if (d <= r.tuning.returnMeters + 0.05f) returned = true;
            if (returned) worst = std::max(worst, d);
        }
        if (order == 0) {
            CHECK(returned);
            CHECK(worst <= r.tuning.leashMeters + 0.15f);
            CHECK(r.fight.stats().partnerStrings == 0);
        } else {
            CHECK(most > 10.f);
            CHECK(r.fight.stats().partnerStrings >= 1);
        }
    }
}

TEST_CASE("arena partner: Spread takes an enemy the lead is not attacking; StayClose takes the lead's") {
    for (int order = 0; order < 2; ++order) {
        CAPTURE(order);
        PRig r;
        r.enemy(dummyKind(), Vec2{1.5f, 0.f});
        r.enemy(dummyKind(), Vec2{1.5f, 3.4f});
        r.bodies.player = Vec2{0.f, 0.f};
        r.bodies.ally = Vec2{1.5f, 1.6f}; // nearer enemy 0, both within its reach
        r.in.partnerOrder = static_cast<std::uint8_t>(order);
        r.go(true);
        int target = -2;
        for (int k = 1; k <= 120 && target == -2; ++k) {
            r.in.lock = k == 1;
            r.in.light = true;
            r.tick();
            REQUIRE(r.out->player.lock == 0);
            if (r.out->ally.attacking) target = r.out->ally.target;
        }
        CHECK(target == (order == 0 ? 0 : 1));
        r.run(1.f);
        const int strike = r.find(ArenaEventType::AllyStrike);
        REQUIRE(strike >= 0);
        CHECK(static_cast<int>(r.log[static_cast<std::size_t>(strike)].e.target) == (order == 0 ? 0 : 1));
    }
}

TEST_CASE("arena partner: its attack time keeps to the budget in every 20 s window") {
    SUBCASE("an idle lead") {
        PRig r;
        stringRig(r, true);
        BlowWatch w;
        for (int k = 0; k < 60 * 60; ++k) {
            r.tick();
            w.see(*r.out, r.def.fighter, r.tuning, r.fight.time());
        }
        CHECK(w.started >= 3);
        CHECK(w.lead.empty());
        CHECK(w.worst(r.tuning, 60.0) <= 0.0);
        CHECK(w.cut == 0);
    }
    SUBCASE("the autopilot lead against a boss") {
        const ArenaTable t = shippedTable();
        PRig r(t.fighter("default"));
        r.def = shippedBoss(t, "stone_bearer", true);
        r.goShipped(true, 1);
        BlowWatch w;
        for (int k = 0; k < 60 * 60 && !r.fight.over(); ++k) {
            r.tick();
            w.see(*r.out, r.def.fighter, r.tuning, r.fight.time());
        }
        MESSAGE("budget: " << w.started << " partner blows, lead attacking " << w.lead.size() * kDt << " s, worst "
                           << w.worst(r.tuning, r.fight.time()));
        CHECK(w.started >= 5);
        CHECK(!w.lead.empty());
        CHECK(w.worst(r.tuning, r.fight.time()) <= 0.0);
    }
}

TEST_CASE("arena partner: a blow it began runs out after a Guard intent") {
    int guarded = 0;
    for (std::uint32_t seed = 1; seed <= 4; ++seed) {
        CAPTURE(seed);
        PRig r;
        r.tuning.guardChance = 1.f;
        r.tuning.budgetWindowSeconds = 5.f;
        r.tuning.budgetAllowanceSeconds = 10.f; // no budget in the way: it strings all the time
        REQUIRE(validPartnerTuning(r.tuning));
        bossRig(r, 0, 1.2f, seed);
        BlowWatch w;
        for (int k = 0; k < 60 * 60; ++k) {
            r.tick();
            w.see(*r.out, r.def.fighter, r.tuning, r.fight.time());
        }
        CHECK(w.started >= 20);
        CHECK(w.cut == 0);
        CHECK(w.guardUpInBlow == 0);
        guarded += w.guardedBlows;
    }
    CHECK(guarded >= 1);
}

TEST_CASE("arena partner: the player falling with the partner up takes its body; it rises; both down is the Defeat") {
    PRig r;
    r.enemy(biterKind(150, 3.f, 5.f), Vec2{0.f, 0.f});
    r.bodies.player = Vec2{1.f, 0.f};   // within the bite
    r.bodies.ally = Vec2{0.f, 3.5f};    // out of it and of the biter's pick
    r.go(true, 80, 1, Vec2{-1.f, 0.f}); // facing the biter, so it bites without the unseen wait
    const Vec2 allyWas = r.bodies.ally;
    r.run(2.f);
    REQUIRE(r.fight.phase() == ArenaPhase::Fighting);
    const int tookAt = r.find(ArenaEventType::TookOver);
    REQUIRE(tookAt >= 0);
    const Logged took = r.log[static_cast<std::size_t>(tookAt)];
    CHECK(took.e.source == kArenaAlly);
    CHECK(took.e.target == kArenaPlayer);
    CHECK(took.e.amount == 80); // min(the ally's 80, the fighter's 100)
    CHECK(took.e.at.x == doctest::Approx(allyWas.x));
    CHECK(took.e.at.y == doctest::Approx(allyWas.y));
    const int downedAt = r.find(ArenaEventType::AllyDowned);
    REQUIRE(downedAt > tookAt); // TookOver, then AllyDowned, in the same tick
    const Logged downed = r.log[static_cast<std::size_t>(downedAt)];
    CHECK(downed.t == took.t);
    CHECK(downed.e.at.x == doctest::Approx(1.f));
    CHECK(r.count(ArenaEventType::Defeat) == 0);
    // The fall itself still shows: Downed on the player where it fell, before the swap.
    CHECK(r.count(ArenaEventType::Downed) == 1);
    const int fellAt = r.find(ArenaEventType::Downed);
    REQUIRE(fellAt >= 0);
    CHECK(fellAt < tookAt);
    CHECK(r.log[static_cast<std::size_t>(fellAt)].e.target == kArenaPlayer);
    CHECK(r.log[static_cast<std::size_t>(fellAt)].e.at.x == doctest::Approx(1.f));
    CHECK(r.log[static_cast<std::size_t>(fellAt)].t == took.t);
    CHECK(r.fight.swapped());
    CHECK(r.fight.stats().takeovers == 1);
    CHECK(r.fight.hp() == 80);
    CHECK(r.fight.allyHp() == 0);
    CHECK(r.bodies.player.x == doctest::Approx(allyWas.x));
    CHECK(r.bodies.player.y == doctest::Approx(allyWas.y));
    CHECK(r.fight.allyPos().x == doctest::Approx(1.f));
    CHECK(r.out->player.action == ArenaAction::Idle);

    SUBCASE("the player falls again with it down: Defeat") {
        r.bodies.player = Vec2{-1.f, 0.f};
        r.run(15.f);
        CHECK(r.fight.phase() == ArenaPhase::Defeat);
        CHECK(r.fight.stats().takeovers == 1);
        CHECK(r.count(ArenaEventType::AllyRose) == 0);
    }
    SUBCASE("it rises at 25%, then both fall at once: Defeat") {
        r.run(31.f);
        const int roseAt = r.find(ArenaEventType::AllyRose);
        REQUIRE(roseAt >= 0);
        const Logged rose = r.log[static_cast<std::size_t>(roseAt)];
        CHECK(rose.t - downed.t == doctest::Approx(r.tuning.downSeconds).epsilon(0.002));
        CHECK(rose.e.amount == 20); // 25% of its 80
        CHECK(r.fight.allyHp() == 20);
        CHECK(r.fight.stats().partnerRises == 1);
        CHECK(r.fight.phase() == ArenaPhase::Fighting);
        r.bodies.player = Vec2{-1.f, 0.f}; // the bite reaches both now
        r.run(15.f);
        CHECK(r.fight.phase() == ArenaPhase::Defeat);
        CHECK(r.fight.stats().takeovers == 1);
    }
}

TEST_CASE("arena partner: without a partner the player at 0 is the Defeat as before") {
    for (int mode = 0; mode < 3; ++mode) {
        CAPTURE(mode);
        PRig r;
        r.enemy(biterKind(150, 3.f, 5.f), Vec2{0.f, 0.f});
        r.bodies.player = Vec2{1.f, 0.f};
        r.bodies.ally = Vec2{0.f, 3.5f};
        const Vec2 facing{-1.f, 0.f};              // facing the biter
        if (mode == 0) r.go(false, 80, 1, facing); // an ally, no partner
        if (mode == 1) r.go(false, 0, 1, facing);  // neither
        if (mode == 2) {                           // a partner, no ally
            r.fight.setPartner(&r.brain, &r.tuning);
            r.go(false, 0, 1, facing);
        }
        r.run(2.f);
        CHECK(r.fight.phase() == ArenaPhase::Defeat);
        CHECK(r.count(ArenaEventType::TookOver) == 0);
        const int downed = r.find(ArenaEventType::Downed);
        REQUIRE(downed >= 0);
        CHECK(r.log[static_cast<std::size_t>(downed)].e.target == kArenaPlayer);
        CHECK(r.fight.stats().takeovers == 0);
        CHECK_FALSE(r.fight.swapped());
    }
}

TEST_CASE("arena partner: downed, it rises after downSeconds at 25%; take half and the table share work again") {
    PRig r;
    r.enemy(biterKind(150, 3.f, 40.f), Vec2{0.f, 0.f});
    r.bodies.player = Vec2{0.f, 2.8f}; // within the biter's pick, out of its bite
    r.bodies.ally = Vec2{1.f, 0.f};    // within its bite
    r.go(true, 100);
    r.run(2.f);
    const int downedAt = r.find(ArenaEventType::AllyDowned);
    REQUIRE(downedAt >= 0);
    const float downedT = r.log[static_cast<std::size_t>(downedAt)].t;
    CHECK(r.fight.allyHp() == 0);
    CHECK(r.fight.hp() == 100);
    r.run(30.f);
    const int roseIdx = r.find(ArenaEventType::AllyRose);
    REQUIRE(roseIdx >= 0);
    const Logged rose = r.log[static_cast<std::size_t>(roseIdx)];
    CHECK(rose.t - downedT == doctest::Approx(r.tuning.downSeconds).epsilon(0.002));
    CHECK(rose.e.amount == 25);
    CHECK(rose.e.target == kArenaAlly);
    CHECK(r.fight.allyHp() == 25);
    CHECK(r.fight.allyAlive());
    CHECK(r.fight.stats().partnerRises == 1);
    const float roseAt = rose.t;

    SUBCASE("take half") {
        for (int k = 0; k < 20 * 60; ++k) {
            r.in.takeHalf = r.fight.takeHalfDue(0.2f);
            r.tick();
        }
        const int half = r.find(ArenaEventType::TookHalf, roseAt);
        REQUIRE(half >= 0);
        CHECK(r.log[static_cast<std::size_t>(half)].e.amount == 75); // (150 + 1) / 2
        CHECK(r.fight.hp() == 25);
        CHECK(r.find(ArenaEventType::AllyDowned, roseAt) >= 0);
    }
    SUBCASE("the table share") {
        ArenaTableSeats seats;
        seats.player = true;
        seats.ally = true;
        r.fight.setTableSeats(seats);
        r.run(20.f);
        const int share = r.find(ArenaEventType::TableShare, roseAt);
        REQUIRE(share >= 0);
        CHECK(r.log[static_cast<std::size_t>(share)].e.target == kArenaPlayer);
        CHECK(r.log[static_cast<std::size_t>(share)].e.amount == 75);
        CHECK(r.fight.hp() == 25);
        CHECK(r.find(ArenaEventType::AllyDowned, roseAt) >= 0);
    }
}

TEST_CASE("arena partner: 60 s against each shipped boss with the shipped brain (report)") {
    const ArenaTable t = shippedTable();
    const PartnerTuning tuning = shippedTuning();
    for (const char* idText : {"kite_boss", "stone_bearer", "white_glove"}) {
        const std::string id = idText;
        CAPTURE(id);
        PRig r(t.fighter("default"));
        r.def = shippedBoss(t, idText, false);
        r.tuning = tuning;
        r.goShipped(true, 1);
        int attacks = 0;
        while (!r.fight.over() && r.fight.time() < 60.f) {
            r.tick();
            const ArenaStats& s = r.fight.stats();
            if (s.bossAttacks != attacks) {
                attacks = s.bossAttacks;
                CHECK(s.bossAimedPartner * 10 <= kArenaBossAimShareTenths * s.bossAttacks);
            }
        }
        const ArenaStats& s = r.fight.stats();
        const double maxHp = r.def.enemies[0].hp;
        const double all = static_cast<double>(s.partnerDamage + s.damageDealt);
        MESSAGE(id << ": " << r.fight.time() << " s, phase " << static_cast<int>(r.fight.phase())
                   << "; partnerDamage " << s.partnerDamage << " = " << s.partnerDamage / maxHp << " of the boss's "
                   << maxHp << " hp, " << (all > 0 ? s.partnerDamage / all : 0.0) << " of all damage (lead "
                   << s.damageDealt << "); strings " << s.partnerStrings << ", blows " << s.partnerBlows
                   << ", guards " << s.partnerGuards << "; boss strings " << s.bossAttacks << ", at the partner "
                   << s.bossAimedPartner << " (" << (s.bossAttacks > 0 ? double(s.bossAimedPartner) / s.bossAttacks : 0.0)
                   << "), share max " << s.bossAimShareMax << "; ally downed "
                   << r.count(ArenaEventType::AllyDowned) << ", rises " << s.partnerRises << ", takeovers "
                   << s.takeovers);
        CHECK(s.bossAimShareMax <= 0.3f + 1e-6f);
        CHECK(s.partnerStrings >= 1);
    }
}
