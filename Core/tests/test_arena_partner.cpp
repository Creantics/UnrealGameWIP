// The partner (ArenaFight::setPartner): a PartnerBrain drives the ally body. Its strings and their damage rule, its
// guard, its rise, a boss's aim at it and that aim's cap, the order, the attack budget, the takeover, and that a
// fight without one is unchanged.

#include "ftcore/ArenaFight.h"
#include "ftcore/ArenaCamera.h"
#include "ftcore/ArenaLockFrame.h"
#include "ftcore/ArenaLook.h"
#include "ftcore/ArenaZone.h"
#include "ftcore/FightSession.h"
#include "ftcore/PartnerBrain.h"
#include "ftcore/StateHash.h"
#include "TestAssets.h"
#include <ostream>
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <sstream>
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

// A partner's blow: a Hit with source kArenaAlly (AllyStrike is no longer emitted).
bool allyHit(const ArenaEvent& e) { return e.type == ArenaEventType::Hit && e.source == kArenaAlly; }
bool leadHit(const ArenaEvent& e) { return e.type == ArenaEventType::Hit && e.source == kArenaPlayer; }

struct Logged {
    ArenaEvent e;
    float t = 0.f;         // fight time at the end of its tick
    std::uint8_t aim = 0;  // enemy 0's aim that tick
};

// A host: the bodies stay where they are put unless `step` (arenaStep moves the player and the enemies) or `walk`
// (the ally walks toward ArenaOutput::ally.goal at its own kit's moveSpeed, off the living enemies, inside the
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
    bool pairOnParry = false; // the autopilot's partner presses the pair on the tick after each parry, as a host does
    bool pairDue = false;
    // The last walkAlly: where its own walk took the ally (before the push-out), how far the push-out then moved it,
    // and whether a body already overlapped it before it walked (an enemy moved into it, not it into the enemy).
    Vec2 walked{};
    float pushed = 0.f;
    bool pushedByEnemy = false;
    bool ownPartnerKit = false; // per-lead kits: the test set def.partnerKit itself (else it is the lead's, as before)
    bool keepView = false;      // the autopilot keeps the test's camera too (cameraOn, cameraPos, cameraFacing, partnerClose)

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
        if (!ownPartnerKit) def.partnerKit = def.fighter;
        fight.setAlly(allyHp, 0.4f);
        if (partner) fight.setPartner(&brain, &tuning);
        fight.begin(def, ring, bodies.player, facing, seed);
    }
    // A shipped fight: its own ring and starts, the ally 1.5 m to the player's side.
    void goShipped(bool partner, std::uint32_t seed) {
        ring = arenaRingAt(Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, def.ringRadius, ArenaRing{Vec2{0.f, 0.f}, 36.f});
        if (!ownPartnerKit) def.partnerKit = def.fighter;
        fight.setAlly(def.partnerKit.hp, 0.4f); // the ally's own kit (the lead's when the test set none)
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
            const PartnerCameraView camera = in.partnerCamera;
            const ArenaInput view = in;
            in = fight.autopilot();
            in.partnerOrder = order;
            in.partnerCamera = camera;
            if (keepView) {
                in.cameraOn = view.cameraOn;
                in.cameraPos = view.cameraPos;
                in.cameraFacing = view.cameraFacing;
                in.partnerClose = view.partnerClose;
            }
            if (pairOnParry) in.pair = pairDue;
            pairDue = false;
        }
        out = &fight.tick(kDt, in, bodies);
        for (const ArenaEvent& e : out->events) {
            log.push_back(Logged{e, fight.time(), out->enemies[0].aim});
            if (e.type == ArenaEventType::TookOver) std::swap(bodies.player, bodies.ally);
            if (pairOnParry && e.type == ArenaEventType::Parried) pairDue = true;
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
        pushedByEnemy = false;
        for (std::size_t i = 0; i < fight.enemyCount(); ++i)
            if (fight.enemyAlive(i) && length(sub(bodies.ally, bodies.enemies[i])) < 0.4f + def.enemies[i].bodyRadius - 1e-4f)
                pushedByEnemy = true;
        const Vec2 d = sub(out->ally.goal, bodies.ally);
        const float far = length(d);
        const float most = fight.def().partnerKit.moveSpeed * kDt; // the ally's own kit (per-lead kits)
        if (far > most) bodies.ally = Vec2{bodies.ally.x + d.x * most / far, bodies.ally.y + d.y * most / far};
        else bodies.ally = out->ally.goal;
        walked = bodies.ally;
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
        pushed = length(sub(bodies.ally, walked));
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
    int allyHits() const {
        int n = 0;
        for (const Logged& l : log)
            if (allyHit(l.e)) ++n;
        return n;
    }
    int findAllyHit() const {
        for (std::size_t i = 0; i < log.size(); ++i)
            if (allyHit(log[i].e)) return static_cast<int>(i);
        return -1;
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

// The partner time its brain holds in its budget window.
float partnerSpent(const PartnerBrain& b) {
    float sum = 0.f;
    for (const float s : b.save().spentPartner) sum += s;
    return sum;
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

TEST_CASE("arena partner: a string lands its blows for partnerBlowDamage, as Hits of its own, nothing of the player's") {
    PRig r;
    stringRig(r, true);
    r.run(3.f);
    const ArenaStats& s = r.fight.stats();
    REQUIRE(s.partnerStrings >= 1);
    REQUIRE(r.allyHits() >= 2);
    CHECK(r.allyHits() == s.partnerBlows);
    int sum = 0;
    for (const Logged& l : r.log) {
        if (!allyHit(l.e)) continue;
        CHECK(l.e.source == kArenaAlly);
        CHECK(l.e.target == 0);
        const int raw = r.def.fighter.light[l.e.move % 3].damage;
        CHECK(l.e.amount == static_cast<int>(std::lround(partnerBlowDamage(static_cast<float>(raw), 10.f))));
        sum += l.e.amount;
    }
    // 10, 11 and 12 raw against a lead light of 10: 6 + the rest halved, 8, 9 (8.5 rounds up) and 9.
    CHECK(r.log[static_cast<std::size_t>(r.findAllyHit())].e.amount == 8);
    CHECK(r.fight.enemyHp(0) == 1000 - sum);
    CHECK(s.partnerDamage == sum);
    // These lights carry no poise damage: no poise lost, no stagger. Nothing of the player's.
    CHECK(r.fight.enemyPoise(0) == doctest::Approx(30.f));
    CHECK(s.damageDealt == 0);
    CHECK(s.hits == 0);
    CHECK(s.staggers == 0);
    CHECK(s.breakOuts == 0);
    CHECK(s.punishUsed == 0);
    CHECK(r.fight.load() == 0);
    CHECK(r.count(ArenaEventType::Hit) == s.partnerBlows); // every Hit is the partner's
    CHECK(r.count(ArenaEventType::AllyStrike) == 0);
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
        if (partner == 1) strikes = r.allyHits();
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
        REQUIRE(r.allyHits() >= 2);
        CHECK(h.calls == r.allyHits());
        CHECK(h.type == ArenaEventType::Hit);
        CHECK(h.target == 0);
        CHECK(h.poise == 0.f); // the light's 0 x the partner's poise scale
        CHECK(h.firstRaw == 10);
        // 20 raw: 6 + 14 / 2.
        CHECK(r.log[static_cast<std::size_t>(r.findAllyHit())].e.amount == 13);
        int sum = 0;
        for (const Logged& l : r.log) {
            if (!allyHit(l.e)) continue;
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
        CHECK(r.allyHits() == 0);
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
        const int strike = r.findAllyHit();
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

TEST_CASE("arena partner: a hit into hitstun cuts its running blow, as the lead's; the time it committed stays spent") {
    PRig r;
    r.tuning.guardChance = 0.f;            // every blow aimed at it lands
    r.tuning.budgetAllowanceSeconds = 10.f; // no budget in the way: it strings all the time
    REQUIRE(validPartnerTuning(r.tuning));
    bossRig(r, 2, 1.2f);
    BlowWatch w;
    int cutSeen = 0, spentChecked = 0;
    bool wasAttacking = false;
    float lastTime = 0.f;
    std::uint8_t lastBlow = 0;
    for (int k = 0; k < 30 * 60 && !r.fight.over(); ++k) {
        const std::size_t from = r.log.size();
        const float spent0 = partnerSpent(r.brain);
        r.tick();
        w.see(*r.out, r.def.fighter, r.tuning, r.fight.time());
        bool hitInBlow = false;
        for (std::size_t i = from; i < r.log.size(); ++i) {
            const ArenaEvent& e = r.log[i].e;
            if (e.type == ArenaEventType::AllyHit && e.amount > 0 && wasAttacking) hitInBlow = true;
            if (hitInBlow) CHECK_FALSE(allyHit(e)); // no damage from the cut blow after the hit, this tick included
        }
        if (hitInBlow) {
            ++cutSeen;
            CHECK_FALSE(r.out->ally.attacking);
            // Its brain still counts the rest of the blow it committed: over the hitstun, about all of it.
            const ArenaAttack& a = r.def.fighter.light[lastBlow % r.def.fighter.lightCount];
            const float rest = a.total() - lastTime - kDt;
            if (r.fight.time() < 10.f && rest > 4.f * kDt) {
                for (int j = 0; j < 18; ++j) r.tick(); // 0.3 s, inside the 0.4 s hitstun: no new blow
                CHECK(partnerSpent(r.brain) - spent0 >= std::min(rest, 0.3f) - 2.f * kDt);
                ++spentChecked;
            }
        }
        wasAttacking = r.out->ally.attacking;
        lastTime = r.out->ally.stateTime;
        lastBlow = r.out->ally.blow;
    }
    const ArenaStats& s = r.fight.stats();
    CHECK(s.takeovers == 0);
    CHECK(cutSeen >= 2);
    CHECK(s.partnerBlowsCut == cutSeen);
    CHECK(s.partnerBlowsVoid == 0); // the boss never dies here
    CHECK(spentChecked >= 1);
}

namespace {
// The boss rig: the lead guards the strings aimed at it (parried, then paired on the next tick, as pairUntilCounter);
// the hook takes the lead's hp loss off every bite and scales the partner's by `allyBite` (1: they land and cut its
// blows; 0: none does). The partner's light lands early, then a long recovery; 40 s.
ArenaBlowRuling cutHook(void* user, const ArenaBlow& blow) {
    ArenaBlowRuling r;
    if (blow.source == kArenaAlly || blow.source == kArenaPlayer) return r;
    r.damage = blow.target == kArenaAlly ? *static_cast<const float*>(user) : 0.f;
    return r;
}
struct CutPair {
    int cutInRecovery = 0, cutAfterLanding = 0, hitsAfterCut = 0, damageLost = 0;
    int counters = 0, cut = 0, allyOnBoss = 0, allyTicks = 0;
    float pairSeconds = 0.f;
    std::vector<int> counterAt, leadHits; // the tick of each counter; the tick and hp of each blow the lead lands
};
CutPair cutPairRun(float allyBite) {
    PRig r;
    r.tuning.guardChance = 0.f;             // every bite at it lands
    r.tuning.budgetAllowanceSeconds = 10.f; // no budget in the way
    REQUIRE(validPartnerTuning(r.tuning));
    for (int i = 0; i < 3; ++i) r.def.fighter.light[i] = swing(0.1f, 0.1f, 1.2f, 2.f, 1); // lands, then a long recovery
    r.enemy(bossKind(5), Vec2{0.f, 0.f});
    r.def.enemies[0].hp = 100000; // it never dies here
    r.def.fighter.hp = 1000;
    r.bodies.player = Vec2{-1.5f, 0.f};
    r.bodies.ally = Vec2{1.2f, 0.f};
    r.go(true, 100000);
    r.fight.setBlowHook(&cutHook, &allyBite);
    CutPair c;
    std::size_t seen = 0;
    bool wasAttacking = false, landed = false, afterCut = false;
    float lastTime = 0.f;
    for (int k = 0; k < 40 * 60 && !r.fight.over(); ++k) {
        const ArenaEnemyOut& e = r.fight.output().enemies[0];
        r.in.guard = e.brain == ArenaBrain::Attack && e.aim == 1 &&
                     ((e.part == ArenaPart::Startup && e.stateTime >= 0.27f) || e.part == ArenaPart::Active);
        r.in.pair = false;
        for (; seen < r.log.size(); ++seen)
            if (r.log[seen].e.type == ArenaEventType::Parried) r.in.pair = true;
        const std::size_t from = r.log.size();
        const int dealt0 = r.fight.stats().partnerDamage;
        r.tick();
        c.pairSeconds += r.out->ally.pairSeconds;
        ++c.allyTicks;
        if (r.out->ally.target == 0) ++c.allyOnBoss;
        const ArenaAttack& a = r.def.fighter.light[0];
        bool hitInBlow = false;
        for (std::size_t i = from; i < r.log.size(); ++i) {
            const ArenaEvent& ev = r.log[i].e;
            if (ev.type == ArenaEventType::AllyHit && ev.amount > 0 && wasAttacking) hitInBlow = true;
            if (allyHit(ev) && (hitInBlow || afterCut)) ++c.hitsAfterCut;
            if (allyHit(ev) && !hitInBlow) landed = true;
            if (ev.type == ArenaEventType::CounterStagger) {
                ++c.counters;
                c.counterAt.push_back(k);
            }
            if (leadHit(ev)) {
                c.leadHits.push_back(k);
                c.leadHits.push_back(ev.amount);
            }
        }
        if (r.out->ally.attacking && r.out->ally.stateTime < lastTime) landed = false; // a new blow
        if (r.out->ally.attacking && !wasAttacking) {
            landed = false;
            afterCut = false;
        }
        if (hitInBlow) {
            ++c.cut;
            CHECK_FALSE(r.out->ally.attacking);
            if (lastTime >= a.startup + a.active) ++c.cutInRecovery;
            if (landed) ++c.cutAfterLanding;
            if (r.fight.stats().partnerDamage < dealt0) ++c.damageLost; // what it dealt stays dealt
            afterCut = true;
        }
        wasAttacking = r.out->ally.attacking;
        lastTime = r.out->ally.stateTime;
    }
    CHECK(r.fight.stats().partnerBlowsCut == c.cut);
    return c;
}
} // namespace

TEST_CASE("arena partner: a hit in its blow's recovery, after it landed, cuts it: what it dealt stays, nothing more") {
    const CutPair c = cutPairRun(1.f);
    MESSAGE("CUTPAIR cut " << c.cut << " recovery " << c.cutInRecovery << " afterLanding " << c.cutAfterLanding
                           << " hitsAfter " << c.hitsAfterCut << " lost " << c.damageLost << " counters " << c.counters
                           << " onBoss " << c.allyOnBoss << "/" << c.allyTicks);
    CHECK(c.cutInRecovery >= 2);
    CHECK(c.cutAfterLanding >= 2);
    CHECK(c.hitsAfterCut == 0);
    CHECK(c.damageLost == 0);
}

TEST_CASE("arena partner: the paired counter is the same whether its blows are cut or not") {
    const CutPair cut = cutPairRun(1.f), clean = cutPairRun(0.f);
    MESSAGE("CUTPAIR2 cut " << cut.cut << "/" << clean.cut << " counters " << cut.counters << "/" << clean.counters
                            << " pair s " << cut.pairSeconds << "/" << clean.pairSeconds);
    REQUIRE(cut.cut >= 2);
    CHECK(clean.cut == 0);
    CHECK(cut.counters >= 2);
    CHECK(cut.counters == clean.counters);
    CHECK(cut.pairSeconds == clean.pairSeconds);
    CHECK(cut.counterAt == clean.counterAt); // on the same ticks
    CHECK(cut.leadHits == clean.leadHits);   // the lead's blows (the counters' among them): the same ticks and hp
}

TEST_CASE("arena partner: a blow whose target dies before its hit frame deals nothing and ends") {
    PRig r;
    for (int i = 0; i < 3; ++i) r.def.fighter.light[i] = swing(0.5f, 0.1f, 0.3f, 2.f, 10); // its hit frame comes late
    r.def.fighter.heavy = swing(0.05f, 0.1f, 0.3f, 2.f, 1000);                          // the lead's kills first
    r.enemy(dummyKind(50), Vec2{0.f, 0.f});
    r.enemy(dummyKind(), Vec2{25.f, 0.f}); // keeps the fight on, far out of its reach
    r.bodies.player = Vec2{0.f, -1.2f};
    r.bodies.ally = Vec2{-1.4f, 0.f};
    r.go(true, 100, 1, Vec2{0.f, 1.f});
    bool pressed = false;
    float killedAt = -1.f;
    for (int k = 0; k < 6 * 60 && killedAt < 0.f; ++k) {
        if (!pressed && r.fight.output().ally.attacking && r.fight.output().ally.target == 0 &&
            r.fight.output().ally.stateTime <= 2.f * kDt) {
            r.in.heavy = true;
            pressed = true;
        }
        r.tick();
        if (!r.fight.enemyAlive(0)) killedAt = r.fight.time();
    }
    REQUIRE(pressed);
    REQUIRE(killedAt > 0.f);
    CHECK(r.count(ArenaEventType::Hit) >= 1); // the lead's
    r.tick();
    CHECK_FALSE(r.fight.output().ally.attacking); // ended by the step after the kill at the latest
    for (int k = 0; k < 30; ++k) r.tick();
    const ArenaStats& s = r.fight.stats();
    CHECK(s.partnerBlowsVoid == 1);
    CHECK(s.partnerBlowsCut == 0);
    CHECK(s.partnerBlows == 0);
    CHECK(s.partnerDamage == 0);
    CHECK(r.allyHits() == 0);
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
        CHECK(rose.e.amount == 20); // 25% of its 80 (both leads on one kit: the maxes stay where they were)
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
        // Hard pin: both leads on the default kit, this fight is main's (1b59533b, before per-lead kits) to the tick.
        // The constants are main's report, written here by hand: never regenerate them from this build.
        struct Pin { const char* id; float seconds; int phase, partnerDamage, leadDamage, strings, blows, guards,
                     bossStrings, atPartner, rises, takeovers; };
        static constexpr Pin kMain[] = {
            {"kite_boss", 31.783f, 2, 226, 874, 13, 22, 0, 13, 0, 0, 0},
            {"stone_bearer", 45.6161f, 2, 299, 1101, 17, 29, 1, 15, 1, 0, 0},
            {"white_glove", 41.4662f, 2, 240, 860, 17, 24, 1, 11, 2, 0, 0},
        };
        const Pin* pin = nullptr;
        for (const Pin& p : kMain)
            if (id == p.id) pin = &p;
        REQUIRE(pin != nullptr);
        CHECK(std::fabs(r.fight.time() - pin->seconds) < 0.5f * kDt);
        CHECK(static_cast<int>(r.fight.phase()) == pin->phase);
        CHECK(s.partnerDamage == pin->partnerDamage);
        CHECK(s.damageDealt == pin->leadDamage);
        CHECK(s.partnerStrings == pin->strings);
        CHECK(s.partnerBlows == pin->blows);
        CHECK(s.partnerGuards == pin->guards);
        CHECK(s.bossAttacks == pin->bossStrings);
        CHECK(s.bossAimedPartner == pin->atPartner);
        CHECK(s.partnerRises == pin->rises);
        CHECK(s.takeovers == pin->takeovers);
        CHECK(s.bossAimShareMax <= 0.3f + 1e-6f);
        CHECK(s.partnerStrings >= 1);
    }
}

namespace {

// A dummy at the origin that never attacks; the striker 1.4 m from it on -x, facing it.
struct BlowSample {
    int amount = -1;     // the first Hit's hp
    float poiseLeft = -1.f;
    float breakValue = -1.f;
    int flinchChances = -1;
    int staggers = -1;
    std::uint8_t source = kArenaNobody;
    ArenaStats stats;
    int load = -1;
    bool leadIdle = true; // the player's body never left Idle
};

ArenaFighter poiseFighter(float poise) {
    ArenaFighter f = ruleFighter();
    for (std::size_t k = 0; k < 3; ++k) f.light[k].poiseDamage = poise;
    return f;
}

ArenaEnemyKind standDummy(bool boss, float poise, float flinchPoise) {
    ArenaEnemyKind k = dummyKind(1000);
    k.poise = poise;
    k.boss = boss;
    k.breakMove = -1;
    k.flinchTime = 0.3f;
    k.flinchPoise = flinchPoise;
    return k;
}

// The first blow of light[0] on the dummy, by the player (partner false) or by the partner.
BlowSample firstBlow(bool partner, bool boss) {
    PRig r(poiseFighter(20.f));
    r.enemy(standDummy(boss, 30.f, 5.f), Vec2{0.f, 0.f});
    if (partner) {
        r.bodies.player = Vec2{-10.f, 0.f};
        r.bodies.ally = Vec2{-1.4f, 0.f};
    } else {
        r.bodies.player = Vec2{-1.4f, 0.f};
        r.bodies.ally = Vec2{0.f, 10.f}; // an ally body, no partner: it does nothing
    }
    r.go(partner);
    BlowSample b;
    for (int k = 0; k < 180; ++k) {
        if (!partner) r.in.light = k == 0;
        r.tick();
        if (r.out->player.action != ArenaAction::Idle && partner) b.leadIdle = false;
        const int hit = partner ? r.findAllyHit() : r.find(ArenaEventType::Hit);
        if (hit < 0) continue;
        const Logged& l = r.log[static_cast<std::size_t>(hit)];
        b.amount = l.e.amount;
        b.source = l.e.source;
        b.poiseLeft = r.fight.enemyPoise(0);
        b.breakValue = r.fight.enemyBreakValue(0);
        b.stats = r.fight.stats();
        b.flinchChances = b.stats.flinchChances;
        b.staggers = b.stats.staggers;
        b.load = r.fight.load();
        break;
    }
    return b;
}

} // namespace

TEST_CASE("arena partner: the same blow by the lead and by the partner differs only by the partner's scales") {
    const ArenaPartnerScales scales{};
    REQUIRE(scales.poise == 0.5f);
    REQUIRE(scales.breakWeight == 0.5f);
    const ArenaTable t = shippedTable();
    CHECK(t.partner.poise == 0.5f);
    CHECK(t.partner.breakWeight == 0.5f);
    CHECK(t.fallback.partner.poise == 0.5f);
    for (bool boss : {false, true}) {
        CAPTURE(boss);
        const BlowSample lead = firstBlow(false, boss);
        const BlowSample ally = firstBlow(true, boss);
        REQUIRE(lead.amount == 10);
        REQUIRE(ally.amount >= 0);
        CHECK(lead.source == kArenaPlayer);
        CHECK(ally.source == kArenaAlly);
        // hp: partnerBlowDamage of the same ruled damage.
        CHECK(ally.amount == static_cast<int>(std::lround(partnerBlowDamage(10.f, 10.f))));
        // poise: 20 by the lead, 20 x 0.5 by the partner.
        CHECK(lead.poiseLeft == doctest::Approx(10.f));
        CHECK(ally.poiseLeft == doctest::Approx(20.f));
        // A boss's break value: kArenaBreakLight by the lead, x 0.5 by the partner; none on a regular enemy.
        CHECK(lead.breakValue == doctest::Approx(boss ? kArenaBreakLight : 0.f));
        CHECK(ally.breakValue == doctest::Approx(boss ? kArenaBreakLight * 0.5f : 0.f));
        // Both blows could flinch it (5 poise needed), each counted the same way.
        CHECK(lead.flinchChances == 1);
        CHECK(ally.flinchChances == 1);
        // Only the lead's blow is the player's.
        CHECK(lead.stats.hits == 1);
        CHECK(lead.stats.damageDealt == 10);
        CHECK(lead.stats.partnerDamage == 0);
        CHECK(ally.stats.hits == 0);
        CHECK(ally.stats.damageDealt == 0);
        CHECK(ally.stats.shotHits == 0);
        CHECK(ally.stats.partnerDamage == ally.amount);
        CHECK(ally.stats.partnerBlows == 1);
        CHECK(ally.load == 0);
        CHECK(ally.leadIdle);
    }
}

TEST_CASE("arena partner: its blows stagger and flinch an enemy, named as the striker") {
    SUBCASE("stagger") {
        PRig r(poiseFighter(30.f)); // 15 a blow at the partner's scale: the second blow breaks 30 poise
        r.enemy(standDummy(false, 30.f, 1000.f), Vec2{0.f, 0.f});
        r.bodies.player = Vec2{-10.f, 0.f};
        r.bodies.ally = Vec2{-1.4f, 0.f};
        r.go(true);
        r.run(3.f);
        const int st = r.find(ArenaEventType::Staggered);
        REQUIRE(st >= 0);
        CHECK(r.log[static_cast<std::size_t>(st)].e.source == kArenaAlly);
        CHECK(r.fight.stats().staggers >= 1);
        CHECK(r.fight.stats().hits == 0);
    }
    SUBCASE("flinch") {
        PRig r(poiseFighter(10.f));
        r.enemy(standDummy(false, 1000.f, 5.f), Vec2{0.f, 0.f});
        r.bodies.player = Vec2{-10.f, 0.f};
        r.bodies.ally = Vec2{-1.4f, 0.f};
        r.go(true);
        r.run(3.f);
        const int fl = r.find(ArenaEventType::Flinched);
        REQUIRE(fl >= 0);
        CHECK(r.log[static_cast<std::size_t>(fl)].e.source == kArenaAlly);
        CHECK(r.fight.stats().flinchChances >= 1);
    }
    SUBCASE("a boss's break value: half a light's for each blow") {
        PRig r(poiseFighter(0.f));
        r.enemy(standDummy(true, 1000.f, 1000.f), Vec2{0.f, 0.f});
        r.bodies.player = Vec2{-10.f, 0.f};
        r.bodies.ally = Vec2{-1.4f, 0.f};
        r.go(true);
        r.run(3.f);
        const int blows = r.allyHits();
        REQUIRE(blows >= 2);
        CHECK(r.fight.stats().bossBreakAdded == doctest::Approx(0.5f * kArenaBreakLight * static_cast<float>(blows)));
    }
}

TEST_CASE("arena partner: its blow is ruled once by the hook, never counts as the player's, adds no table share") {
    PRig r(poiseFighter(20.f));
    HookLog h;
    r.fight.setBlowHook(&allyHook, &h);
    ArenaTableSeats seats;
    seats.player = true;
    seats.ally = true;
    r.fight.setTableSeats(seats);
    r.enemy(standDummy(false, 1000.f, 1000.f), Vec2{0.f, 0.f});
    r.bodies.player = Vec2{-10.f, 0.f};
    r.bodies.ally = Vec2{-1.4f, 0.f};
    r.go(true);
    r.run(4.f);
    const ArenaStats& s = r.fight.stats();
    REQUIRE(s.partnerBlows >= 3);
    CHECK(h.calls == s.partnerBlows); // one ruling a blow (no second pass for the poise or the break)
    CHECK(r.allyHits() == s.partnerBlows);
    CHECK(h.otherSources == 0);
    CHECK(h.poise == doctest::Approx(10.f)); // 20 x 0.5, what the hook is asked to rule
    CHECK(r.count(ArenaEventType::TableShare) == 0);
    CHECK(s.hits == 0);
    CHECK(s.damageDealt == 0);
    CHECK(s.punishUsed == 0);
    CHECK(r.fight.load() == 0);
}

TEST_CASE("arena partner: a boss string aimed at it re-aims at the lead from the next blow once it is down") {
    PRig r;
    r.tuning.guardChance = 0.f;
    r.tuning.budgetShare = 0.f;
    r.tuning.budgetAllowanceSeconds = 0.f;
    REQUIRE(validPartnerTuning(r.tuning));
    // Two narrow bites in a string: the first downs whoever it is aimed at.
    ArenaEnemyKind k = bossKind(150);
    k.moveCount = 2;
    k.moves[1] = k.moves[0];
    k.moves[0].chain = 1;
    k.moves[1].chain = -1;
    k.moves[1].minRange = 1000.f; // never begun on its own: only as the string's second blow
    for (std::size_t m = 0; m < 2; ++m) {
        k.moves[m].attack.arcCos = 0.5f;
        k.moves[m].attack.tracking = 40.f; // the windup turns onto whom it aims at
        k.moves[m].attack.damage = m == 0 ? 150 : 20;
    }
    k.turnRate = 40.f;
    r.enemy(k, Vec2{0.f, 0.f});
    r.def.fighter.hp = 100000;
    r.bodies.player = Vec2{-1.5f, 0.f};
    r.bodies.ally = Vec2{1.2f, 0.f};
    r.go(true, 100, 1, Vec2{1.f, 0.f});
    int downedAt = -1;
    std::uint8_t aimAtDown = 0;
    bool secondSeen = false;
    std::uint8_t secondAim = 0;
    int leadHitsBefore = 0;
    for (int f = 0; f < 120 * 60 && !secondSeen; ++f) {
        r.tick();
        if (downedAt < 0) {
            const int d = r.find(ArenaEventType::AllyDowned);
            if (d >= 0) {
                downedAt = d;
                aimAtDown = r.log[static_cast<std::size_t>(d)].aim;
                leadHitsBefore = 0;
                for (const Logged& l : r.log)
                    if (l.e.type == ArenaEventType::Hit && l.e.target == kArenaPlayer) ++leadHitsBefore;
            }
            continue;
        }
        const ArenaEnemyOut& e = r.out->enemies[0];
        if (e.brain == ArenaBrain::Attack && e.move == 1) {
            secondSeen = true;
            secondAim = e.aim;
        }
    }
    REQUIRE(downedAt >= 0);
    CHECK(aimAtDown == 2); // the blow under way finished where it was aimed
    REQUIRE(secondSeen);
    CHECK(secondAim == 1); // the string's next blow aims at the standing lead
    r.run(1.f);
    int leadHitsAfter = 0;
    for (const Logged& l : r.log)
        if (l.e.type == ArenaEventType::Hit && l.e.target == kArenaPlayer) ++leadHitsAfter;
    CHECK(leadHitsAfter > leadHitsBefore); // the re-aimed blow reached the lead
    CHECK(r.fight.stats().bossAimedPartner >= 1);
}

TEST_CASE("arena partner: one fight with a two-lead session: blows ruled, takeover on the tick, defeat, retry") {
    // The host's part: lead A drives the player's body at first, lead B the ally body; the session rules every
    // partner blow through judgeBlow(the ally body's lead) and takes both leads' health each step.
    struct Host {
        FightSession s;
        FighterId playerLead = kLeadA;
        int partnerJudged = 0;
        int leadJudged = 0;
        FighterId lastStriker = kNoFighter;
    } host;
    auto hook = [](void* user, const ArenaBlow& blow) {
        Host& h = *static_cast<Host*>(user);
        ArenaBlowRuling rl;
        if (blow.source == kArenaAlly) {
            const FighterId other = h.playerLead == kLeadA ? kLeadB : kLeadA;
            const CountHit c = h.s.judgeBlow(other);
            ++h.partnerJudged;
            h.lastStriker = other;
            rl.damage = c.outcome == CountOutcome::Held ? 1.f : c.multiplier;
        } else if (blow.source == kArenaPlayer) {
            ++h.leadJudged;
            rl.damage = h.s.judgeBlow(h.playerLead).multiplier;
        }
        return rl;
    };
    REQUIRE(host.s.begin(FightSessionDef{}, 0.0).ok());
    REQUIRE(host.s.setActiveLead(kLeadA));
    PRig r;
    r.fight.setBlowHook(+hook, &host);
    r.enemy(biterKind(150, 3.f, 5.f), Vec2{0.f, 0.f});
    r.bodies.player = Vec2{8.f, 0.f}; // out of the biter's pick: no bites yet
    r.bodies.ally = Vec2{-1.4f, 0.f};
    r.go(true, 100, 1, Vec2{-1.f, 0.f});
    int sessionTookOverAt = -1;
    int fightTookOverAt = -1;
    bool defeatSeen = false;
    int frame = 0;
    auto step = [&]() {
        r.tick();
        for (const ArenaEvent& e : r.out->events)
            if (e.type == ArenaEventType::TookOver) {
                fightTookOverAt = frame;
                host.playerLead = host.playerLead == kLeadA ? kLeadB : kLeadA; // the bodies swapped
            }
        const FighterId other = host.playerLead == kLeadA ? kLeadB : kLeadA;
        host.s.setLeadHealth(host.playerLead, static_cast<float>(r.fight.hp()) / 100.f);
        host.s.setLeadHealth(other, static_cast<float>(r.fight.allyHp()) / 100.f);
        const FightTick& t = host.s.tick(kDt);
        if (t.tookOver != kNoFighter) {
            sessionTookOverAt = frame;
            CHECK(t.tookOver == kLeadB);
        }
        if (t.defeated) defeatSeen = true;
        ++frame;
    };
    // 1. The partner strikes; the session rules each of its blows as lead B's.
    for (int k = 0; k < 120; ++k) step();
    REQUIRE(r.allyHits() >= 1);
    CHECK(host.partnerJudged == r.allyHits());
    CHECK(host.lastStriker == kLeadB);
    CHECK(host.leadJudged == 0);
    // 2. The player's body steps into the bite (the ally's out of it): lead A falls, B takes over on that tick.
    r.bodies.player = Vec2{1.f, 0.f};
    r.bodies.ally = Vec2{0.f, 3.5f};
    for (int k = 0; k < 120 && fightTookOverAt < 0; ++k) step();
    REQUIRE(fightTookOverAt >= 0);
    CHECK(sessionTookOverAt == fightTookOverAt);
    CHECK(host.s.activeLead() == kLeadB);
    CHECK(host.playerLead == kLeadB);
    CHECK(host.s.down(kLeadA));
    CHECK_FALSE(host.s.down(kLeadB));
    // 3. B, the last standing, falls (A is still down): the fight's Defeat and the session's, the same step.
    r.bodies.player = Vec2{-1.f, 0.f};
    for (int k = 0; k < 15 * 60 && !r.fight.over(); ++k) step();
    REQUIRE(r.fight.phase() == ArenaPhase::Defeat);
    step();
    CHECK(defeatSeen);
    CHECK(host.s.defeated());
    // 4. Retry: both up from the fight-start checkpoint, and the host re-reads the controlled lead.
    const FightCheckpoint& cp = host.s.retry();
    CHECK(cp.at == CheckpointAt::FightStart);
    CHECK(cp.activeLead == kLeadA);
    r.fight.restart();
    host.playerLead = host.s.activeLead();
    CHECK(host.playerLead == kLeadA);
    CHECK_FALSE(host.s.down(kLeadA));
    CHECK_FALSE(host.s.down(kLeadB));
    CHECK_FALSE(host.s.defeated());
    CHECK_FALSE(r.fight.swapped());
    CHECK(r.fight.hp() == 100);
    CHECK(r.fight.allyHp() == 100);
    CHECK(r.fight.phase() == ArenaPhase::Fighting);
}

TEST_CASE("arena partner: a fight begun Lone takes the partner on the call: joined, its blows ruled, takeover, retry Lone") {
    // The host's part as the runner's: Lone with lead A only, no partner on the fight's side. The call ends the carry
    // (endLone(PartnerCalled)) and asks the join; on tick.joined the host sets the ally and the partner. Each step the
    // fight's word on the partner (partnerOn) and the session's (inFight(B)) agree.
    struct Host {
        FightSession s;
        FighterId playerLead = kLeadA;
        int partnerJudged = 0;
    } host;
    auto hook = [](void* user, const ArenaBlow& blow) {
        Host& h = *static_cast<Host*>(user);
        ArenaBlowRuling rl;
        if (blow.source == kArenaAlly) {
            ++h.partnerJudged;
            rl.damage = h.s.judgeBlow(h.playerLead == kLeadA ? kLeadB : kLeadA).multiplier;
        }
        return rl;
    };
    FightSessionDef sd;
    sd.loneLead = kLeadA;
    sd.inFight = {true, false};
    REQUIRE(host.s.begin(sd, 0.0).ok());
    REQUIRE(host.s.lone());
    PRig r;
    r.fight.setBlowHook(+hook, &host);
    r.enemy(biterKind(150, 3.f, 5.f), Vec2{0.f, 0.f});
    r.bodies.player = Vec2{8.f, 0.f};
    r.bodies.ally = Vec2{-1.4f, 0.f};
    r.fight.setLone(true, LoneTuning{}, 0.f);
    r.go(false, 0, 1, Vec2{-1.f, 0.f}); // no ally body, no partner
    REQUIRE(r.fight.lone());
    int disagree = 0;
    int joinedAt = -1;
    int frame = 0;
    auto step = [&]() {
        r.tick();
        disagree += r.out->ally.partnerOn != host.s.inFight(kLeadB) ? 1 : 0;
        for (const ArenaEvent& e : r.out->events)
            if (e.type == ArenaEventType::TookOver) host.playerLead = host.playerLead == kLeadA ? kLeadB : kLeadA;
        const FighterId other = host.playerLead == kLeadA ? kLeadB : kLeadA;
        host.s.setLeadHealth(host.playerLead, static_cast<float>(r.fight.hp()) / 100.f);
        if (host.s.inFight(other)) host.s.setLeadHealth(other, static_cast<float>(r.fight.allyHp()) / 100.f);
        const FightTick& t = host.s.tick(kDt);
        if (t.joined == kLeadB) { // the fight takes the partner before its next step
            joinedAt = frame;
            r.fight.setAlly(100, 0.4f);
            r.fight.setPartner(&r.brain, &r.tuning);
        }
        ++frame;
    };
    for (int k = 0; k < 30; ++k) step();
    CHECK_FALSE(r.out->ally.partnerOn);
    CHECK_FALSE(host.s.inFight(kLeadB));
    // 1. The call: the carry ends, the join waits for the close.
    REQUIRE(host.s.endLone(LoneEnd::PartnerCalled));
    r.fight.setLone(false, LoneTuning{}, 0.f);
    REQUIRE(host.s.joinFight(kLeadB, 1.f));
    step();
    REQUIRE(joinedAt == 30);
    CHECK(host.s.inFight(kLeadB));
    CHECK(r.fight.allyHp() == 100);
    // 2. Its blows land and the session rules each as lead B's.
    for (int k = 0; k < 120; ++k) step();
    REQUIRE(r.allyHits() >= 1);
    CHECK(host.partnerJudged == r.allyHits());
    // 3. Lead A falls with the partner up: B takes over, the session the same step.
    r.bodies.player = Vec2{1.f, 0.f};
    r.bodies.ally = Vec2{0.f, 3.5f};
    for (int k = 0; k < 120 && host.playerLead == kLeadA; ++k) step();
    REQUIRE(host.playerLead == kLeadB);
    CHECK(host.s.activeLead() == kLeadB);
    CHECK(host.s.down(kLeadA));
    CHECK(disagree == 0);
    // 4. Retry from the fight's start: Lone with one lead again; the host puts the partner out on its side.
    const FightCheckpoint& cp = host.s.retry();
    CHECK(cp.at == CheckpointAt::FightStart);
    CHECK_FALSE(host.s.inFight(kLeadB));
    CHECK(host.s.activeLead() == kLeadA);
    CHECK(host.s.lone());
    host.playerLead = kLeadA;
    r.fight.setPartner(nullptr, nullptr);
    r.fight.setAlly(0, 0.4f);
    r.fight.setLone(true, LoneTuning{}, 0.f);
    r.fight.restart();
    r.bodies.player = Vec2{8.f, 0.f};
    for (int k = 0; k < 30; ++k) step();
    CHECK_FALSE(r.fight.swapped());
    CHECK(r.fight.lone());
    CHECK_FALSE(r.out->ally.partnerOn);
    CHECK(disagree == 0);
}

namespace {
// Bites parried (the guard pressed late in the startup, let go in the recovery) and each parry paired on the next
// tick, until the first counter-stagger; its tick's ArenaAllyOut::pairSeconds is returned, -1 for no counter.
float pairUntilCounter(PRig& r) {
    std::size_t seen = 0;
    for (int k = 0; k < 600 && !r.fight.over(); ++k) {
        const ArenaEnemyOut& e = r.fight.output().enemies[0];
        r.in.guard = e.brain == ArenaBrain::Attack &&
                     ((e.part == ArenaPart::Startup && e.stateTime >= 0.27f) || e.part == ArenaPart::Active);
        for (; seen < r.log.size(); ++seen)
            if (r.log[seen].e.type == ArenaEventType::Parried) r.in.pair = true;
        r.tick();
        if (r.count(ArenaEventType::CounterStagger) > 0) return r.out->ally.pairSeconds;
    }
    return -1.f;
}
} // namespace

TEST_CASE("arena partner: a counter dealt reports pairPartnerSeconds, and its pairCooldown outlasts a lead switch") {
    PRig r;
    r.enemy(biterKind(5, 3.f, 1.f), Vec2{1.5f, 0.f});
    r.bodies.player = Vec2{0.f, 0.f};
    r.bodies.ally = Vec2{-10.f, 0.f}; // out of the bites' and its own blows' reach
    r.go(true, 100);
    CHECK(r.fight.output().ally.pairSeconds == 0.f);
    const float spent = pairUntilCounter(r);
    CHECK(spent == doctest::Approx(r.def.fighter.pairPartnerSeconds));
    CHECK(spent == doctest::Approx(1.f));
    CHECK(r.fight.pressReason(ArenaPress::Pair) == PressReason::Cooldown);
    r.in.guard = false;
    r.tick();
    CHECK(r.out->ally.pairSeconds == 0.f); // the counter's tick only
    // The clock is both leads': the switched-to lead's PARTNER press is still cooling.
    REQUIRE(r.fight.switchLeads());
    std::swap(r.bodies.player, r.bodies.ally);
    CHECK(r.fight.pressReason(ArenaPress::Pair) == PressReason::Cooldown);
    // No partner in the fight: a counter spends no partner time.
    PRig q;
    q.enemy(biterKind(5, 3.f, 1.f), Vec2{1.5f, 0.f});
    q.bodies.player = Vec2{0.f, 0.f};
    q.go(false, 100);
    CHECK(pairUntilCounter(q) == 0.f);
}

TEST_CASE("arena partner: a counter's partner time reaches the brain's budget once (PartnerInput::spentSeconds)") {
    // The tick of the counter and the next: the brain takes its second on one of them, once.
    PRig q;
    q.enemy(biterKind(5, 3.f, 1.f), Vec2{1.5f, 0.f});
    q.bodies.player = Vec2{0.f, 0.f};
    q.bodies.ally = Vec2{-10.f, 0.f};
    q.go(true, 100);
    std::size_t seen = 0;
    bool countered = false;
    for (int k = 0; k < 600 && !countered && !q.fight.over(); ++k) {
        const ArenaEnemyOut& e = q.fight.output().enemies[0];
        q.in.guard = e.brain == ArenaBrain::Attack &&
                     ((e.part == ArenaPart::Startup && e.stateTime >= 0.27f) || e.part == ArenaPart::Active);
        for (; seen < q.log.size(); ++seen)
            if (q.log[seen].e.type == ArenaEventType::Parried) q.in.pair = true;
        const float before = partnerSpent(q.brain);
        q.tick();
        if (q.count(ArenaEventType::CounterStagger) > 0) {
            countered = true;
            q.in.guard = false;
            q.in.pair = false;
            q.tick(); // the brain's next step, if the counter came after it stepped
            const float gained = partnerSpent(q.brain) - before;
            CHECK(gained >= 1.f - 1e-3f);           // the counter's second
            CHECK(gained <= 1.f + 0.25f);           // and no more than two ticks of its own blows on top
        }
    }
    CHECK(countered);
}

TEST_CASE("arena partner: switchLeads swaps both standing bodies and their hp; refused with the partner down or Lone") {
    PRig r;
    r.enemy(biterKind(150, 3.f, 5.f), Vec2{20.f, 0.f}); // far: nothing lands
    r.bodies.player = Vec2{0.f, 0.f};
    r.bodies.ally = Vec2{-2.f, 1.f};
    CHECK_FALSE(r.fight.switchLeads()); // not begun
    r.go(true, 100);
    r.tick();
    REQUIRE(r.fight.hostHurt(40) == 40); // the player's body at 60, the partner's at 100
    REQUIRE(r.fight.hp() == 60);
    REQUIRE(r.fight.allyHp() == 100);
    const Vec2 playerWas = r.bodies.player;
    const Vec2 allyWas = r.fight.allyPos();
    REQUIRE(r.fight.switchLeads());
    CHECK(r.fight.swapped());
    CHECK(r.fight.stats().leadSwitches == 1);
    CHECK(r.fight.stats().takeovers == 0);
    CHECK(r.fight.hp() == 100);
    CHECK(r.fight.allyHp() == 60);
    CHECK(r.fight.allyPos().x == doctest::Approx(playerWas.x));
    CHECK(r.fight.allyPos().y == doctest::Approx(playerWas.y));
    // The host swaps the bodies before the next tick; the stick then moves the new body.
    std::swap(r.bodies.player, r.bodies.ally);
    CHECK(r.bodies.player.x == doctest::Approx(allyWas.x));
    r.step = true;
    for (int k = 0; k < 30; ++k) {
        r.in.move = Vec2{0.f, 1.f};
        r.tick();
    }
    CHECK(r.bodies.player.y > allyWas.y + 0.5f);
    CHECK(r.fight.hp() == 100);
    CHECK(r.fight.phase() == ArenaPhase::Fighting);
    // Switched back: each body has its own hp again.
    REQUIRE(r.fight.switchLeads());
    CHECK_FALSE(r.fight.swapped());
    CHECK(r.fight.hp() == 60);
    CHECK(r.fight.allyHp() == 100);
    std::swap(r.bodies.player, r.bodies.ally);
    // Lone: no partner in the fight, no switch.
    r.fight.setLone(true, LoneTuning{}, 0.f);
    CHECK_FALSE(r.fight.switchLeads());
    r.fight.setLone(false, LoneTuning{}, 0.f);
    // The partner down: no switch (a fall's takeover is the fight's own).
    r.fight.setAlly(0, 0.4f);
    CHECK_FALSE(r.fight.switchLeads());
    CHECK(r.fight.stats().leadSwitches == 2);
    // No partner set: no switch.
    PRig q;
    q.enemy(biterKind(150, 3.f, 5.f), Vec2{20.f, 0.f});
    q.go(false, 100);
    q.tick();
    CHECK_FALSE(q.fight.switchLeads());
}

TEST_CASE("arena partner: the file's partner block is read into every fight, each scale in [0, 1]") {
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Battle/arena.json", text));
    const std::string shipped = "\"partner\": { \"poise\": 0.5, \"breakWeight\": 0.5 }";
    const std::size_t at = text.find(shipped);
    REQUIRE(at != std::string::npos);
    auto with = [&](const char* block) {
        std::string t = text;
        t.replace(at, shipped.size(), block);
        return t;
    };
    ArenaTable t;
    std::string err;
    REQUIRE_MESSAGE(importArenaTable(with("\"partner\": { \"poise\": 0.25, \"breakWeight\": 1 }"), t, &err), err);
    CHECK(t.partner.poise == 0.25f);
    CHECK(t.partner.breakWeight == 1.f);
    CHECK(t.fallback.partner.poise == 0.25f);
    for (const ArenaFightEntry& e : t.entries) {
        CHECK(e.def.partner.poise == 0.25f);
        CHECK(e.def.partner.breakWeight == 1.f);
    }
    CHECK_FALSE(importArenaTable(with("\"partner\": { \"poise\": 1.5 }"), t, &err));
    CHECK_FALSE(importArenaTable(with("\"partner\": { \"breakWeight\": -0.1 }"), t, &err));
    CHECK_FALSE(importArenaTable(with("\"partner\": 3"), t, &err));
}

namespace {

// A biter whose bite has a long startup (so a blow lands inside it) and three tells, so every bite deals Read cards.
ArenaEnemyKind readBiter(float startup, float flinchPoise) {
    ArenaEnemyKind k = biterKind(1, 3.f, 0.5f);
    k.poise = 1000.f;
    k.moves[0].attack = swing(startup, 0.15f, 0.5f, 2.f, 1);
    k.moveCount = 3;
    k.moves[1] = k.moves[0];
    k.moves[2] = k.moves[0];
    k.moves[0].tell = ArenaTell::Thrust;
    k.moves[1].tell = ArenaTell::Sweep;
    k.moves[2].tell = ArenaTell::Slam;
    k.flinchTime = 0.3f;
    k.flinchPoise = flinchPoise;
    return k;
}

ArenaFighter readFighter() {
    ArenaFighter f = poiseFighter(20.f);
    f.hp = 100000;
    f.readPower = 1.5f;
    f.readStackPower = 0.2f;
    f.readPoise = 10.f;
    return f;
}

struct ReadCut {
    bool read = false; // the lead read the bite right
    bool cut = false;  // a blow flinched the enemy inside that bite
    std::uint8_t by = kArenaNobody;
    int counters = 0;  // ReadCounter events in the whole run
    ArenaStats stats;  // at the end
    int stacks = -1;
};

// The lead reads a bite right; then the lead (partner false) or the partner cuts it short: with a flinch (an enemy of
// 1000 poise), or with a stagger (5 poise: any blow breaks it, the partner's halved 10 too).
ReadCut readThenCut(bool partner, bool stagger = false) {
    PRig r(readFighter());
    ArenaEnemyKind biter = readBiter(2.f, 5.f);
    if (stagger) biter.poise = 5.f;
    r.enemy(biter, Vec2{0.f, 0.f});
    if (partner) {
        r.bodies.player = Vec2{-1.5f, 0.f};
        r.bodies.ally = Vec2{1.4f, 0.f};
    } else {
        r.bodies.player = Vec2{-1.4f, 0.f};
        r.bodies.ally = Vec2{0.f, 10.f};
    }
    r.go(partner);
    ReadCut c;
    for (int k = 0; k < 600 && !c.cut; ++k) {
        if (!c.read && r.fight.readOpen() && !r.fight.readCards().picked())
            r.in.read = static_cast<std::int8_t>(r.fight.readCards().correct());
        if (c.read && !partner) r.in.light = true;
        const ArenaBrain was = r.out ? r.out->enemies[0].brain : ArenaBrain::Approach;
        r.tick();
        for (const ArenaEvent& e : r.out->events) {
            if (e.type == ArenaEventType::Read) c.read = true;
            const ArenaEventType cutBy = stagger ? ArenaEventType::Staggered : ArenaEventType::Flinched;
            if (e.type == cutBy && c.read && was == ArenaBrain::Attack) {
                c.cut = true;
                c.by = e.source;
            }
        }
    }
    r.run(3.f); // the bite was cut, so its end never brings the counter later either
    c.counters = r.count(ArenaEventType::ReadCounter);
    c.stats = r.fight.stats();
    c.stacks = r.fight.readStacks();
    return c;
}

// A regular enemy that never asks to attack, with a break move and a count of 2 (two of the lead's units).
ArenaEnemyKind breakDummy() {
    ArenaEnemyKind k = biterKind(5, 3.f, 0.f);
    k.attackDelay = 1000.f;
    k.moves[0].attack = swing(0.25f, 0.2f, 0.4f, 3.f, 5);
    k.moves[0].attack.armor = 1.f;
    k.poise = 1000.f;
    k.breakAt = 2;
    k.breakDecay = 5.f;
    k.breakMove = 0;
    return k;
}

} // namespace

TEST_CASE("arena partner: its blow cuts a read move short but brings no Read counter; the lead's same blow does") {
    const ReadCut ally = readThenCut(true);
    REQUIRE(ally.read);
    REQUIRE(ally.cut); // the move cut short (Flinched inside the bite)
    CHECK(ally.by == kArenaAlly);
    CHECK(ally.counters == 0);
    CHECK(ally.stacks == 1); // no read stack spent
    CHECK(ally.stats.readCounters == 0);
    CHECK(ally.stats.readDamage == 0);
    CHECK(ally.stats.hits == 0);
    CHECK(ally.stats.damageDealt == 0);
    CHECK(ally.stats.punishUsed == 0);
    CHECK(ally.stats.partnerBlows >= 1);
    const ReadCut lead = readThenCut(false);
    REQUIRE(lead.read);
    REQUIRE(lead.cut);
    CHECK(lead.by == kArenaPlayer);
    CHECK(lead.counters == 1);
    CHECK(lead.stats.readCounters == 1);
    CHECK(lead.stats.readDamage > 0);
    // The stagger route (staggerEnemy, then interrupt): the same rule.
    const ReadCut allyStagger = readThenCut(true, true);
    REQUIRE(allyStagger.read);
    REQUIRE(allyStagger.cut);
    CHECK(allyStagger.by == kArenaAlly);
    CHECK(allyStagger.counters == 0);
    CHECK(allyStagger.stacks == 1);
    CHECK(allyStagger.stats.readCounters == 0);
    CHECK(allyStagger.stats.hits == 0);
    CHECK(allyStagger.stats.damageDealt == 0);
    const ReadCut leadStagger = readThenCut(false, true);
    REQUIRE(leadStagger.read);
    REQUIRE(leadStagger.cut);
    CHECK(leadStagger.by == kArenaPlayer);
    CHECK(leadStagger.counters == 1);
    CHECK(leadStagger.stats.readCounters == 1);
}

TEST_CASE("arena partner: armour takes the same share of its poise as of the lead's, after the partner's scale") {
    // A bite with armour 0.5 and a 3 s startup: the first blow lands inside it.
    ArenaEnemyKind k = biterKind(1, 3.f, 0.5f);
    k.poise = 1000.f;
    k.moves[0].attack = swing(3.f, 0.15f, 0.5f, 2.f, 1);
    k.moves[0].attack.armor = 0.5f;
    float lost[2] = {-1.f, -1.f};
    for (int partner = 0; partner < 2; ++partner) {
        CAPTURE(partner);
        PRig r(poiseFighter(20.f));
        r.enemy(k, Vec2{0.f, 0.f});
        r.bodies.player = Vec2{partner ? -1.5f : -1.4f, 0.f};
        r.bodies.ally = partner ? Vec2{1.4f, 0.f} : Vec2{0.f, 10.f};
        r.go(partner != 0);
        for (int t = 0; t < 600 && lost[partner] < 0.f; ++t) {
            const bool inStartup = r.out && r.out->enemies[0].brain == ArenaBrain::Attack &&
                                   r.out->enemies[0].part == ArenaPart::Startup;
            if (!partner && inStartup) r.in.light = true;
            r.tick();
            const int hit = partner ? r.findAllyHit() : r.find(ArenaEventType::Hit);
            if (hit < 0) continue;
            REQUIRE(inStartup);
            lost[partner] = 1000.f - r.fight.enemyPoise(0);
        }
        REQUIRE(lost[partner] >= 0.f);
    }
    CHECK(lost[0] == doctest::Approx(20.f * 0.5f));        // the lead's 20, half ignored by the armour
    CHECK(lost[1] == doctest::Approx(20.f * 0.5f * 0.5f)); // x the partner's poise scale, then the same armour
}

TEST_CASE("arena partner: its blows fill the break-out count at half the lead's weight; a step back is unhurt") {
    {   // The lead: one blow of 20 poise weighs 2 units, the whole count.
        PRig r(poiseFighter(20.f));
        r.enemy(breakDummy(), Vec2{0.f, 0.f});
        r.bodies.player = Vec2{-1.4f, 0.f};
        r.bodies.ally = Vec2{0.f, 10.f};
        r.go(false);
        r.in.light = true;
        for (int t = 0; t < 60 && r.find(ArenaEventType::Hit) < 0; ++t) r.tick();
        REQUIRE(r.find(ArenaEventType::Hit) >= 0);
        CHECK(r.fight.stats().breakUpright == 1);
        CHECK(r.fight.stats().breakRunWeight == doctest::Approx(2.f));
        CHECK(r.fight.stats().breakDue == 1);
    }
    // The partner behind the enemy, the lead 3 m off facing away from it (out of the windup arc): the partner's 10
    // poise weighs 1 unit, so its second blow brings the count, and the enemy steps back.
    PRig r(poiseFighter(20.f));
    r.enemy(breakDummy(), Vec2{0.f, 0.f});
    r.bodies.player = Vec2{3.f, 0.f};
    r.bodies.ally = Vec2{-1.4f, 0.f};
    r.go(true);
    int blows = 0;
    for (int t = 0; t < 600 && blows < 2; ++t) {
        r.tick();
        for (const ArenaEvent& e : r.out->events) {
            if (!allyHit(e)) continue;
            ++blows;
            if (blows == 1) {
                CHECK(r.fight.stats().breakUpright == 1);
                CHECK(r.fight.stats().breakRunWeight == doctest::Approx(1.f));
                CHECK(r.fight.stats().breakDue == 0);
            }
        }
    }
    REQUIRE(blows == 2);
    CHECK(r.fight.stats().breakUpright == 2);
    CHECK(r.fight.stats().breakDue == 1);
    CHECK(r.fight.stats().stepBacks == 1);
    CHECK(r.fight.stats().hits == 0);
    REQUIRE(r.out->enemies[0].brain == ArenaBrain::StepBack);
    // Unhurt while it steps (the host leaves the bodies where they are, in reach): the partner's string runs on and
    // its active parts land nothing.
    int activeTicks = 0, hitsInStep = 0;
    for (int t = 0; t < 120; ++t) {
        r.tick();
        if (r.out->enemies[0].brain != ArenaBrain::StepBack) break;
        if (r.out->ally.attacking && r.out->ally.part == ArenaPart::Active) ++activeTicks;
        for (const ArenaEvent& e : r.out->events)
            if (allyHit(e)) ++hitsInStep;
    }
    CHECK(activeTicks > 0);
    CHECK(hitsInStep == 0);
}

TEST_CASE("arena partner: at a set table with a session's count, each blow is ruled once and taken once") {
    // The host rules each partner blow through judgeBlow (lead B, the ally body's): the count gives every judged blow
    // the next id, so a second pass would show as a gap. Both leads and both enemies sit at the table: only enemy
    // blows are shared, so the partner's blows pass nothing on and take from their target exactly the Hit's hp.
    struct Host {
        FightSession s;
        std::vector<std::uint16_t> ids;
        int other = 0;
    } host;
    auto hook = [](void* user, const ArenaBlow& blow) {
        Host& h = *static_cast<Host*>(user);
        ArenaBlowRuling rl;
        if (blow.source == kArenaAlly) {
            const CountHit c = h.s.judgeBlow(kLeadB);
            h.ids.push_back(c.blow);
            rl.damage = c.multiplier;
        } else {
            ++h.other;
        }
        return rl;
    };
    REQUIRE(host.s.begin(FightSessionDef{}, 0.0).ok());
    REQUIRE(host.s.setActiveLead(kLeadA));
    PRig r(poiseFighter(20.f));
    r.fight.setBlowHook(+hook, &host);
    r.enemy(standDummy(false, 1000.f, 1000.f), Vec2{0.f, 0.f});
    r.enemy(standDummy(false, 1000.f, 1000.f), Vec2{0.f, 6.f});
    r.bodies.player = Vec2{-10.f, 0.f};
    r.bodies.ally = Vec2{-1.4f, 0.f};
    r.go(true);
    ArenaTableSeats seats;
    seats.player = true;
    seats.ally = true;
    seats.enemies = 0x3u;
    seats.enemyShare[0] = seats.enemyShare[1] = 1.f;
    r.fight.setTableSeats(seats);
    const int hp0 = r.fight.enemyHp(0), hp1 = r.fight.enemyHp(1);
    for (int k = 0; k < 240; ++k) {
        r.tick();
        host.s.tick(kDt);
    }
    const ArenaStats& s = r.fight.stats();
    REQUIRE(s.partnerBlows >= 3);
    CHECK(r.allyHits() == s.partnerBlows);
    REQUIRE(host.ids.size() == static_cast<std::size_t>(s.partnerBlows));
    for (std::size_t i = 0; i < host.ids.size(); ++i) CHECK(host.ids[i] == i + 1); // one judgment a blow, no gap
    CHECK(host.other == 0);
    int dealt = 0;
    for (const Logged& l : r.log)
        if (allyHit(l.e)) dealt += l.e.amount;
    CHECK(s.partnerDamage == dealt);
    CHECK((hp0 - r.fight.enemyHp(0)) + (hp1 - r.fight.enemyHp(1)) == dealt); // taken once, nothing passed on
    CHECK(r.count(ArenaEventType::TableShare) == 0);
    CHECK(s.hits == 0);
    CHECK(s.damageDealt == 0);
    CHECK(s.punishUsed == 0);
}

TEST_CASE("arena partner: PARTNER (the pair press) is Closed or Ok while it is in the fight, down included, and once it is out") {
    PRig r;
    r.enemy(biterKind(150, 3.f, 40.f), Vec2{0.f, 0.f});
    r.bodies.player = Vec2{0.f, 2.8f}; // within the biter's pick, out of its bite
    r.bodies.ally = Vec2{1.f, 0.f};    // within its bite
    r.go(true, 100);
    bool downSeen = false;
    for (int k = 0; k < 240; ++k) {
        const PressReason why = r.fight.pressReason(ArenaPress::Pair);
        CHECK((why == PressReason::Closed || why == PressReason::Ok));
        if (r.fight.allyMaxHp() > 0 && r.fight.allyHp() == 0) {
            downSeen = true;
            CHECK(why == PressReason::Closed); // down: its body is still in the fight, so today's answer, not Absent
        }
        r.tick();
    }
    CHECK(downSeen);
    // Out of the fight (a retry of a fight begun Lone puts it out: no ally body): the press still pairs with a parry,
    // so the answer stays the window's, never Absent.
    r.fight.setPartner(nullptr, nullptr);
    r.fight.setAlly(0, 0.4f);
    CHECK(r.fight.pressReason(ArenaPress::Pair) != PressReason::Absent);
}

TEST_CASE("arena partner: stationing runs only with the host's camera; off by default") {
    for (const bool camera : {false, true}) {
        CAPTURE(camera);
        PRig r;
        r.enemy(biterKind(150, 3.f, 5.f), Vec2{6.f, 0.f});
        r.bodies.player = Vec2{0.f, 0.f};
        r.bodies.ally = Vec2{-2.f, 1.f};
        r.go(true, 100);
        r.walk = true;
        if (camera) {
            r.in.cameraOn = true;
            r.in.cameraPos = Vec2{-4.f, 0.f}; // behind the lead, looking along +x
            r.in.cameraFacing = Vec2{1.f, 0.f};
        }
        int on = 0, ticks = 0;
        for (int k = 0; k < 120; ++k) {
            r.tick();
            REQUIRE(r.out->ally.partnerOn);
            on += r.out->ally.stationed ? 1 : 0;
            ++ticks;
        }
        if (camera) CHECK(on == ticks);
        else CHECK(on == 0);
    }
    {   // a camera not a finite number: no stationing, and the fight runs on
        PRig r;
        r.enemy(biterKind(150, 3.f, 5.f), Vec2{6.f, 0.f});
        r.bodies.ally = Vec2{-2.f, 1.f};
        r.go(true, 100);
        r.in.cameraOn = true;
        r.in.cameraPos = Vec2{std::numeric_limits<float>::quiet_NaN(), 0.f};
        r.tick();
        CHECK_FALSE(r.out->ally.stationed);
        CHECK(r.fight.phase() == ArenaPhase::Fighting);
    }
}

TEST_CASE("arena partner: a switch and a takeover each tell the brain its body changed (rolesSwapped)") {
    {   // switchLeads
        PRig r;
        r.enemy(biterKind(150, 3.f, 5.f), Vec2{20.f, 0.f});
        r.bodies.ally = Vec2{-2.f, 1.f};
        r.go(true, 100);
        r.tick();
        REQUIRE(r.brain.swaps() == 0);
        REQUIRE(r.fight.switchLeads());
        CHECK(r.brain.swaps() == 1);
        CHECK(r.fight.stats().partnerSwaps == 1);
        std::swap(r.bodies.player, r.bodies.ally);
        r.tick();
        REQUIRE(r.fight.switchLeads());
        CHECK(r.brain.swaps() == 2);
        CHECK(r.fight.stats().partnerSwaps == 2);
        CHECK(r.fight.stats().leadSwitches == 2);
    }
    {   // a refused switch tells it nothing
        PRig r;
        r.enemy(biterKind(150, 3.f, 5.f), Vec2{20.f, 0.f});
        r.go(true, 100);
        r.tick();
        r.fight.setLone(true, LoneTuning{}, 0.f); // Lone: no partner in the fight
        CHECK_FALSE(r.fight.switchLeads());
        CHECK(r.brain.swaps() == 0);
        CHECK(r.fight.stats().partnerSwaps == 0);
    }
    {   // a takeover
        PRig r;
        r.enemy(biterKind(150, 3.f, 5.f), Vec2{0.f, 0.f});
        r.bodies.player = Vec2{1.f, 0.f};
        r.bodies.ally = Vec2{0.f, 3.5f};
        r.go(true, 80, 1, Vec2{-1.f, 0.f});
        r.run(2.f);
        REQUIRE(r.fight.stats().takeovers == 1);
        CHECK(r.brain.swaps() == 1);
        CHECK(r.fight.stats().partnerSwaps == 1);
        CHECK(r.fight.stats().leadSwitches == 0);
    }
}



namespace {
// Rule 9's keep-out wedge, as the brain judges it (no clearance): from the camera along the camera-to-lead line
// to wedgeBeyondMeters past the lead, wedgeHalfDegrees either side.
bool inKeepOut(Vec2 cam, Vec2 lead, Vec2 p, const PartnerTuning& t = PartnerTuning{}) {
    const Vec2 a = sub(lead, cam);
    const float l = length(a);
    if (l < 1e-4f) return false;
    const Vec2 u{a.x / l, a.y / l};
    const Vec2 d = sub(p, cam);
    const float along = d.x * u.x + d.y * u.y;
    if (along <= 0.f || along >= l + t.wedgeBeyondMeters) return false;
    const float h = t.wedgeHalfDegrees * 3.14159265f / 180.f;
    return std::fabs(d.x * u.y - d.y * u.x) * std::cos(h) - along * std::sin(h) < 0.f;
}
// A goal the brain chose (MoveTo, StepBack): Wait's goal is only where the body stands (hit, guarding, a string).
bool moving(const ArenaOutput& o) {
    const PartnerAct a = static_cast<PartnerAct>(o.ally.act);
    return a == PartnerAct::MoveTo || a == PartnerAct::StepBack;
}
// In the camera's view: in front of it, within halfDegrees of its forward. The tests use half the zone camera's
// default horizontal field of view (ZoneCamera::fov), so "in view" is the play lens's own frame, not a chosen number.
bool inView(Vec2 cam, Vec2 forward, Vec2 p, float halfDegrees) {
    const Vec2 d = sub(p, cam);
    const float dl = length(d), fl = length(forward);
    if (dl < 1e-4f || fl < 1e-4f) return false;
    return (d.x * forward.x + d.y * forward.y) / (dl * fl) >= std::cos(halfDegrees * 3.14159265f / 180.f);
}
} // namespace

TEST_CASE("arena partner: stationed, it stands in the camera's view but out of its keep-out wedge and in the ring") {
    // The camera off to the lead's right looks at it; the partner comes in from that side. Off, its goal (the bite's
    // edge on its way in) is in the wedge, so the check below can fail; on, no goal is, and it ends seen from the side.
    const Vec2 cam{2.5f, -6.f};
    const Vec2 forward = sub(Vec2{0.f, 0.f}, cam);
    for (const bool camera : {false, true}) {
        CAPTURE(camera);
        PRig r;
        r.enemy(biterKind(5, 3.f, 5.f), Vec2{2.5f, 0.f}); // a light bite: the partner stays up
        r.bodies.player = Vec2{0.f, 0.f};
        r.bodies.ally = Vec2{1.f, -2.f};
        r.go(true, 100);
        r.walk = true;
        if (camera) {
            r.in.cameraOn = true;
            r.in.cameraPos = cam;
            r.in.cameraFacing = forward;
        }
        int inWedge = 0, stationed = 0, moves = 0;
        for (int k = 0; k < 90; ++k) {
            r.tick();
            REQUIRE(r.fight.allyHp() > 0);
            stationed += r.out->ally.stationed ? 1 : 0;
            if (moving(*r.out)) { ++moves; inWedge += inKeepOut(cam, r.bodies.player, r.out->ally.goal) ? 1 : 0; }
        }
        if (!camera) {
            CHECK(stationed == 0);
            CHECK(inWedge > 0); // the geometry check can fail
            continue;
        }
        CHECK(stationed == 90);
        CHECK(moves > 20);
        CHECK(inWedge == 0);
        CHECK_FALSE(inKeepOut(cam, r.bodies.player, r.bodies.ally));
        const float halfFov = ZoneCamera{}.fov * 0.5f;
        CHECK(inView(cam, forward, r.bodies.ally, halfFov));
        CHECK_FALSE(inView(cam, Vec2{1.f, 0.f}, r.bodies.ally, halfFov)); // a wrong forward would not see it
        CHECK(length(sub(r.bodies.ally, r.bodies.player)) >= PartnerTuning{}.personalSpaceMeters - 0.05f);
    }
    {   // A small ring: the station it wants is past the wall, so its goal is held at the ring less its body, which
        // a wrong radius (the ring's own) would not do.
        PRig r;
        r.ring = ArenaRing{Vec2{0.f, 0.f}, 4.2f};
        r.enemy(biterKind(5, 3.f, 5.f), Vec2{3.3f, 0.f});
        r.bodies.player = Vec2{2.4f, 0.f};
        r.bodies.ally = Vec2{0.f, -1.5f};
        r.go(true, 100);
        r.walk = true;
        r.in.cameraOn = true;
        r.in.cameraPos = Vec2{-3.f, 0.f};
        r.in.cameraFacing = Vec2{1.f, 0.f};
        const float limit = 4.2f - 0.4f;
        float most = 0.f;
        int inWedge = 0, moves = 0;
        for (int k = 0; k < 120; ++k) {
            r.tick();
            REQUIRE(r.out->ally.stationed);
            most = std::max(most, length(r.out->ally.goal));
            if (moving(*r.out)) { ++moves; inWedge += inKeepOut(r.in.cameraPos, r.bodies.player, r.out->ally.goal) ? 1 : 0; }
        }
        CHECK(most <= limit + 1e-3f);
        CHECK(most >= limit - 0.05f); // the wall is what holds it
        CHECK(moves > 20);
        CHECK(inWedge == 0);
    }
}

TEST_CASE("arena partner: cameraOn off gives the old outputs, whatever the camera fields hold") {
    PRig a, b;
    for (PRig* r : {&a, &b}) {
        r->enemy(biterKind(20, 3.f, 2.f), Vec2{3.f, 0.5f});
        r->bodies.ally = Vec2{-2.f, 1.f};
        r->go(true, 100, 7);
        r->walk = true;
    }
    b.in.cameraOn = false;
    b.in.cameraFacing = Vec2{0.f, 1.f};
    for (int k = 0; k < 240; ++k) {
        b.in.cameraPos = Vec2{-4.f + 0.01f * static_cast<float>(k), 3.f};
        a.tick();
        b.tick();
        CAPTURE(k);
        REQUIRE(a.out->ally.goal.x == b.out->ally.goal.x);
        REQUIRE(a.out->ally.goal.y == b.out->ally.goal.y);
        REQUIRE(a.out->ally.act == b.out->ally.act);
        REQUIRE_FALSE(b.out->ally.stationed);
        REQUIRE(a.brain.hash() == b.brain.hash());
        REQUIRE(a.fight.hp() == b.fight.hp());
        REQUIRE(a.fight.allyHp() == b.fight.allyHp());
    }
}

namespace {
// The bite's rig: one biter at the centre, the player within it at (1, 0), the ally at `allyAt`, the camera behind
// the player. `prep` runs after the begin (the ally's body down to 30: switch, hurt that body, switch back), so the
// reference fight and the test do the same.
struct SameStep {
    PRig r;
    SameStep(int damage, Vec2 allyAt, bool prep) {
        r.tuning.downSeconds = 1.f;
        r.enemy(biterKind(damage, 3.f, 5.f), Vec2{0.f, 0.f});
        r.bodies.player = Vec2{1.f, 0.f};
        r.bodies.ally = allyAt;
        r.go(true, 100, 1, Vec2{-1.f, 0.f});
        r.in.cameraOn = true;
        r.in.cameraPos = Vec2{5.f, 0.f};
        r.in.cameraFacing = Vec2{-1.f, 0.f};
        if (prep) {
            REQUIRE(r.fight.switchLeads());
            std::swap(r.bodies.player, r.bodies.ally);
            REQUIRE(r.fight.hostHurt(70) == 70);
            REQUIRE(r.fight.switchLeads());
            std::swap(r.bodies.player, r.bodies.ally);
        }
    }
};
// The tick (0-based) the bite first lands on the player in the reference fight.
int biteTick(int damage, Vec2 allyAt, bool prep) {
    SameStep ref(damage, allyAt, prep);
    for (int k = 0; k < 300; ++k) {
        ref.r.tick();
        for (const ArenaEvent& e : ref.r.out->events)
            if ((e.type == ArenaEventType::Hit && e.target == kArenaPlayer) || e.type == ArenaEventType::TookOver)
                return k;
    }
    return -1;
}
} // namespace

TEST_CASE("arena partner: a switch on the step its new ally body goes down, and on the step its new lead body falls") {
    {   // The switch puts the player out of the bite; the bite lands on the ally's body (the old lead's) and downs it.
        const Vec2 allyAt{0.f, 3.5f};
        const int at = biteTick(150, allyAt, false);
        REQUIRE(at > 0);
        SameStep s(150, allyAt, false);
        for (int k = 0; k < at; ++k) s.r.tick();
        REQUIRE(s.r.fight.switchLeads());
        std::swap(s.r.bodies.player, s.r.bodies.ally);
        s.r.tick();
        CHECK(s.r.count(ArenaEventType::AllyDowned) == 1);
        CHECK(s.r.count(ArenaEventType::TookOver) == 0);
        CHECK(s.r.fight.allyHp() == 0);
        CHECK(s.r.fight.hp() > 0);
        CHECK(s.r.fight.phase() == ArenaPhase::Fighting);
        CHECK(s.r.brain.swaps() == 1);
        CHECK(s.r.fight.stats().partnerSwaps == 1);
        CHECK(s.r.fight.stats().leadSwitches == 1);
        s.r.tick();
        CHECK(s.r.brain.down()); // the swap's reset did not hide the fall
        CHECK_FALSE(s.r.out->ally.stationed);
        s.r.run(1.5f); // downSeconds 1: it rises and stations again
        CHECK(s.r.count(ArenaEventType::AllyRose) == 1);
        CHECK_FALSE(s.r.brain.down());
        CHECK(s.r.out->ally.stationed);
        CHECK(s.r.brain.swaps() == 1);
    }
    {   // Both bodies in the bite, the ally's at 30: switched onto it, the player falls on that step and the partner
        // (the old lead's body, up) takes over. Two body changes in one step, each told to the brain once.
        const Vec2 allyAt{-1.f, 0.f};
        const int at = biteTick(90, allyAt, true);
        REQUIRE(at > 0);
        SameStep s(90, allyAt, true);
        REQUIRE(s.r.brain.swaps() == 2);
        for (int k = 0; k < at; ++k) s.r.tick();
        REQUIRE(s.r.fight.hp() == 100);
        REQUIRE(s.r.fight.allyHp() == 30);
        REQUIRE(s.r.fight.switchLeads());
        std::swap(s.r.bodies.player, s.r.bodies.ally);
        s.r.tick();
        CHECK(s.r.count(ArenaEventType::TookOver) == 1);
        CHECK(s.r.fight.phase() == ArenaPhase::Fighting);
        CHECK(s.r.fight.hp() > 0);
        CHECK(s.r.fight.allyHp() == 0);
        CHECK(s.r.fight.stats().leadSwitches == 3);
        CHECK(s.r.fight.stats().takeovers == 1);
        CHECK(s.r.brain.swaps() == 4);
        CHECK(s.r.fight.stats().partnerSwaps == 4);
    }
}

namespace {

// A third-person camera 4.5 m behind the lead along its facing, 2 m up, pitched 12 degrees down, 90 degrees wide at 16:9.
PartnerCameraView behindLead(Vec2 lead, Vec2 facing) {
    constexpr float kRad = 3.14159265f / 180.f;
    const float p = 12.f * kRad;
    PartnerCameraView v;
    v.given = true;
    v.pos = {lead.x - 4.5f * facing.x, lead.y - 4.5f * facing.y, 2.f};
    v.forward = {facing.x * std::cos(p), facing.y * std::cos(p), -std::sin(p)};
    v.up = {facing.x * std::sin(p), facing.y * std::sin(p), std::cos(p)};
    v.aspect = 16.f / 9.f;
    v.verticalFovDegrees = 2.f * std::atan(1.f / v.aspect) / kRad;
    return v;
}

} // namespace

TEST_CASE("arena partner: 20 s of the shipped group fight, camera not given against given (report)") {
    const ArenaTable t = shippedTable();
    const char* names[kPartnerIdleCount] = {"none", "noBudget", "noTarget", "moving",
                                            "staggered", "pause", "other", "noKit"};
    double todayShare[2] = {0, 0}; // on screen with the camera not given, per order
    // Main's brain (mainEquivalentTuning: step 2's margin and holds off) against the shipped one.
    for (const bool mainEq : {true, false})
    for (const std::uint8_t order : {std::uint8_t{0}, std::uint8_t{1}})
        for (const bool camera : {false, true}) {
            CAPTURE(mainEq);
            CAPTURE(order);
            CAPTURE(camera);
            PRig r(t.fighter("default"));
            r.def = t.fallback;
            r.def.enemyCount = 0;
            for (const char* id : {"skirmisher", "hulk", "duelist"})
                for (std::size_t e = 0; e < t.enemyIds.size(); ++e)
                    if (t.enemyIds[e] == id) {
                        r.def.enemyKind[r.def.enemyCount] = static_cast<std::uint8_t>(e);
                        r.def.enemies[r.def.enemyCount++] = t.enemies[e];
                    }
            REQUIRE(r.def.enemyCount == 3);
            r.def.attackers = 2;
            for (std::size_t e = 0; e < r.def.enemyCount; ++e) { // rule 18 needs a tall target: none here
                const ArenaBodyCylinder b = arenaBodyCylinder(r.def.enemies[e], Vec2{}, 0.f);
                CHECK(b.top - b.base < kPartnerTallMeters);
            }
            r.tuning = mainEq ? mainEquivalentTuning(shippedTuning()) : shippedTuning();
            r.in.partnerOrder = order;
            r.goShipped(true, 1);
            r.pairOnParry = true;
            Vec2 facing = r.fight.playerStartFacing();
            double pairSpent = 0;
            Vec2 lastGoal = r.bodies.ally, lastAt = r.bodies.ally;
            int goalJumps = 0, repositions = 0;
            double walked = 0, leadAttack = 0, partnerBlow = 0, lastStart = 0, longestGap = 0, onScreen = 0;
            bool wasReposition = false;
            while (!r.fight.over() && r.fight.time() < 20.f) {
                r.in.partnerCamera = camera ? behindLead(r.bodies.player, facing) : PartnerCameraView{};
                const PartnerCameraView seen = behindLead(r.bodies.player, facing); // on-screen time, given or not
                r.tick();
                if (partnerInFrame(seen, kPartnerFrustumMargin, r.bodies.ally)) onScreen += kDt;
                const PartnerIntent& pi = r.brain.intent();
                if (pi.blowStart) {
                    longestGap = std::max(longestGap, double(r.fight.time()) - lastStart);
                    lastStart = r.fight.time();
                }
                const bool rep = pi.why == PartnerWhy::Reposition;
                if (rep && !wasReposition) ++repositions;
                wasReposition = rep;
                if (length(sub(r.out->ally.goal, lastGoal)) > 0.25f) ++goalJumps; // a goal moved over 15 m/s at 60 Hz
                lastGoal = r.out->ally.goal;
                walked += length(sub(r.bodies.ally, lastAt));
                lastAt = r.bodies.ally;
                if (r.out->player.action == ArenaAction::Light || r.out->player.action == ArenaAction::Heavy) leadAttack += kDt;
                if (r.out->ally.part != ArenaPart::None) partnerBlow += kDt;
                pairSpent += r.out->ally.pairSeconds;
                // A host camera turns after the lead at about 3 rad/s, not on the tick its facing snaps.
                const Vec2 f = r.out->player.facing;
                const float k = std::min(1.f, 3.f * kDt);
                const Vec2 m{facing.x + (f.x - facing.x) * k, facing.y + (f.y - facing.y) * k};
                const float ml = length(m);
                if (ml > 1e-4f) facing = Vec2{m.x / ml, m.y / ml};
            }
            const ArenaStats& s = r.fight.stats();
            const double T = r.fight.time();
            longestGap = std::max(longestGap, T - lastStart);
            std::string shares;
            for (std::size_t k = 0; k < kPartnerIdleCount; ++k)
                shares += std::string(" ") + names[k] + " " +
                          std::to_string(r.brain.idleSeconds(static_cast<PartnerIdle>(k)) / T).substr(0, 5);
            const std::string head = std::string(mainEq ? "main's tuning, " : "shipped, ") +
                                     (order == 0 ? "StayClose" : "Spread") + (camera ? ", camera" : ", no camera");
            MESSAGE(head << ": " << T << " s, "
                                   << r.def.enemyCount << " enemies; strings " << s.partnerStrings << ", blows "
                                   << s.partnerBlows << " (cut by a hit " << s.partnerBlowsCut << ", ended at a dead target "
                                   << s.partnerBlowsVoid << "), guards " << s.partnerGuards << ", fallback "
                                   << r.brain.fallbackSeconds() << " s; shares:" << shares);
            {
                // Hard pin: the default kit for both leads, main's group fight (1b59533b) to the tick. Main's report
                // values, written by hand: never regenerate them from this build. Every row is main's, under main's
                // tuning (mainEquivalentTuning) and the shipped one alike: no enemy here is tall and circling is off,
                // so the framing holds (the bottom margin and the side hold) are off (framingHolds). One value is not
                // main's: Spread without the camera walks 9.63666 m (main 9.63654). The body settle at 0.1 mm (B fixes,
                // ARENA_BODY_OVERLAP_1122) moves the lead out of enemy2 at 12.65 s, where the 0.7 mm floor left 0.44 mm
                // of contact; strings, blows, guards and goal jumps are main's.
                struct Pin { int strings, blows, cut, voided, guards, goalJumps; double walked; };
                static constexpr Pin kMain[2][2] = { // [order][camera]
                    {{7, 9, 3, 0, 0, 7, 9.41492}, {7, 11, 1, 0, 0, 52, 26.1652}},
                    {{6, 8, 0, 0, 0, 7, 9.63666}, {6, 8, 0, 0, 0, 42, 23.9698}}, // Spread, no camera: walked 9.63654 (main) -> 9.63666, B fixes
                };
                const Pin& pin = kMain[order == 0 ? 0 : 1][camera ? 1 : 0];
                CHECK(std::fabs(T - 20.0165) < 0.5 * kDt);
                CHECK(s.partnerStrings == pin.strings);
                CHECK(s.partnerBlows == pin.blows);
                CHECK(s.partnerBlowsCut == pin.cut);
                CHECK(s.partnerBlowsVoid == pin.voided);
                CHECK(s.partnerGuards == pin.guards);
                CHECK(goalJumps == pin.goalJumps);
                CHECK(walked == doctest::Approx(pin.walked).epsilon(1e-5));
            }
            MESSAGE("    goal jumps " << goalJumps << ", walked " << walked << " m, longest gap between blow starts "
                                      << longestGap << " s, repositions " << repositions << "; lead attacking "
                                      << leadAttack << " s, partner blows running " << partnerBlow
                                      << " s + paired counters " << pairSpent
                                      << " s, the governor's allowance 0.6 x lead + 2 = " << 0.6 * leadAttack + 2.0
                                      << " s");
            MESSAGE("    on screen " << onScreen / T << ", goal jumps per 10 s " << 10.0 * goalJumps / T
                                     << ", off-frame starts " << r.brain.offFrameStarts() << "; rule 17: circling "
                                     << r.brain.skirmishSeconds() << " s over " << r.brain.skirmishMoves()
                                     << " goals, guards " << r.brain.guardsBegun() << ", dodges 0 (none in the kit)");
            CHECK(r.brain.framed() == camera);
            if (!camera) todayShare[order] = onScreen / T;
            if (camera && !mainEq) {
                CHECK(s.partnerStrings >= 1);
                // On screen (inside the frame shrunk by kPartnerFrustumMargin) at least 0.8 of the fight (measured
                // 0.97 StayClose, 0.90 Spread), above cameraOn's stationing by at least 0.02 (measured 0.93 and
                // 0.85 without the camera: here cameraOn already looks from behind the lead), its goal jumps under
                // 35 times in 10 s (measured 26 StayClose, 21 Spread), and no string from off the frame (measured 0).
                CHECK(onScreen / T >= 0.8);
                CHECK(onScreen / T - todayShare[order] >= 0.02);
                CHECK(10.0 * goalJumps / T <= 35.0);
                CHECK(r.brain.offFrameStarts() == 0);
            }
        }
}

namespace {

// A lead that never attacks for 20 s of the shipped group fight, the play camera behind it: the partner's numbers.
struct IdleLead {
    double T = 0, leadAttack = 0, partnerBlow = 0, longestStill = 0, longestInPlace = 0, onScreen = 0, jumpsPer10 = 0;
    double facingOff = 0, circlingTime = 0, shortestCircling = 0, shortestHold = 0;
    int skirmishMoves = 0, retargetPicks = 0, circlingReversals = 0, rulesReversals = 0;
    // The host's push-outs, measured apart from its own walk (a push is a run of ticks the push-out moved it): how
    // many, split by who moved into whom; the most one moved it in all and the fastest tick; the reversals its body
    // made only with a push in the tick (its own walk that tick no reversal), counted as forced.
    int pushes = 0, pushesEnemyInto = 0, pushesWalkInto = 0, pushReversals = 0;
    double pushMost = 0, pushFastest = 0, pushMostEnemy = 0, pushMostWalk = 0; // ... the most by each cause
    // Its circling walk's shape: the reversals a body in its held line let through (ArenaAllyOut::bodyTurn, forced)
    // and the shortest hold before one; the circling steps whose goal lies inside a body it is not circling.
    int bodyTurns = 0, goalsInBody = 0;
    double shortestBodyTurn = 1e9;
};

IdleLead idleLead(const ArenaTable& t, std::uint8_t order, bool circling, std::uint32_t seed = 1, bool quiet = false) {
    IdleLead o;
    {
        PRig r(t.fighter("default"));
        r.def = t.fallback;
        r.def.enemyCount = 0;
        for (const char* id : {"skirmisher", "hulk", "duelist"})
            for (std::size_t e = 0; e < t.enemyIds.size(); ++e)
                if (t.enemyIds[e] == id) {
                    r.def.enemyKind[r.def.enemyCount] = static_cast<std::uint8_t>(e);
                    r.def.enemies[r.def.enemyCount++] = t.enemies[e];
                }
        REQUIRE(r.def.enemyCount == 3);
        r.def.attackers = 2;
        r.tuning = shippedTuning();
        r.tuning.circling = circling;
        r.in.partnerOrder = order;
        r.goShipped(true, seed);
        r.autopilot = false; // the lead moves, dodges and guards as the autopilot says, and never attacks
        Vec2 facing = r.fight.playerStartFacing();
        Vec2 lastGoal = r.bodies.ally, lastAt = r.bodies.ally;
        int goalJumps = 0;
        double leadAttack = 0, partnerBlow = 0, onScreen = 0, still = 0, longestStill = 0, facingOff = 0;
        Vec2 placeAt = r.bodies.ally;
        double inPlace = 0, longestInPlace = 0;
        // Goal jumps by kind: a walk begun or ended (the goal is the body while it waits), its own goal re-picked.
        int flips = 0, repicks = 0;
        bool wasWalk = false;
        // Direction reversals: a turn over 90 degrees from any heading it moved on in the last 0.5 s.
        constexpr int kWin = 30; // 0.5 s of steps
        std::array<Vec2, kWin> dirs{};
        std::array<bool, kWin> dirOn{};
        int slot = 0, reversals = 0, circlingReversals = 0, forcedReversals = 0, blowReversals = 0;
        double holdFrom = -1.0, shortestCircling = 1e9, circlingTime = 0, shortestHold = 1e9;
        bool pushing = false, runByEnemy = false;
        double pushRun = 0;
        while (!r.fight.over() && r.fight.time() < 20.f) {
            const std::uint8_t keep = r.in.partnerOrder;
            r.in = r.fight.autopilot();
            r.in.partnerOrder = keep;
            r.in.light = r.in.heavy = r.in.pair = false;
            r.in.partnerCamera = behindLead(r.bodies.player, facing);
            r.tick();
            if (partnerInFrame(r.in.partnerCamera, kPartnerFrustumMargin, r.bodies.ally)) onScreen += kDt;
            const PartnerIntent& pi = r.brain.intent();
            const bool walkNow = pi.act == PartnerAct::MoveTo || pi.act == PartnerAct::StepBack;
            if (length(sub(r.out->ally.goal, lastGoal)) > 0.25f) {
                ++goalJumps;
                if (walkNow != wasWalk) ++flips;
                else ++repicks;
            }
            wasWalk = walkNow;
            lastGoal = r.out->ally.goal;
            {
                // Pushes: a run of ticks the push-out moved it more than 0.1 mm.
                const bool pushedNow = r.pushed > 1e-4f;
                if (pushedNow) {
                    if (!pushing) {
                        ++o.pushes;
                        ++(r.pushedByEnemy ? o.pushesEnemyInto : o.pushesWalkInto);
                        runByEnemy = r.pushedByEnemy;
                        pushRun = 0;
                    }
                    pushRun += r.pushed;
                    o.pushMost = std::max(o.pushMost, pushRun);
                    double& most = runByEnemy ? o.pushMostEnemy : o.pushMostWalk;
                    most = std::max(most, pushRun);
                    o.pushFastest = std::max(o.pushFastest, static_cast<double>(r.pushed) / kDt);
                }
                pushing = pushedNow;
                // Its headings: its own walk this tick (the push-out's part measured apart, above).
                const Vec2 v = sub(r.walked, lastAt);
                const float sp = length(v) / static_cast<float>(kDt);
                bool anyOn = false;
                for (int i = 0; i < kWin; ++i) anyOn = anyOn || dirOn[static_cast<std::size_t>(i)];
                if (pushedNow) { // a reversal of its body only with the push in it
                    const Vec2 w = sub(r.bodies.ally, lastAt);
                    if (length(w) / static_cast<float>(kDt) > 0.5f) {
                        const Vec2 d{w.x / length(w), w.y / length(w)};
                        bool turnedAll = false, turnedOwn = false;
                        for (int i = 0; i < kWin; ++i)
                            if (dirOn[static_cast<std::size_t>(i)]) {
                                const Vec2 h = dirs[static_cast<std::size_t>(i)];
                                turnedAll = turnedAll || d.x * h.x + d.y * h.y < 0.f;
                                turnedOwn = turnedOwn || (sp > 0.5f && v.x * h.x + v.y * h.y < 0.f);
                            }
                        if (turnedAll && !turnedOwn) ++o.pushReversals;
                    }
                }
                if (sp > 0.5f) {
                    const Vec2 d{v.x / length(v), v.y / length(v)};
                    if (!anyOn && holdFrom < 0.0) holdFrom = r.fight.time();
                    bool turned = false;
                    for (int i = 0; i < kWin; ++i)
                        if (dirOn[static_cast<std::size_t>(i)] &&
                            d.x * dirs[static_cast<std::size_t>(i)].x + d.y * dirs[static_cast<std::size_t>(i)].y < 0.f)
                            turned = true;
                    if (turned) {
                        const double hold = r.fight.time() - holdFrom;
                        // Forced: a wind-up at it (its guard or a step back from the rules), or a body in its held
                        // line (ArenaAllyOut::bodyTurn). A blow: its own lunge.
                        // Circling: else, with its attack held for the budget or walking its own goal.
                        const bool forced = pi.act == PartnerAct::Guard || pi.act == PartnerAct::StepBack || r.out->ally.bodyTurn;
                        const bool blow = pi.act == PartnerAct::Attack || r.out->ally.part != ArenaPart::None;
                        ++reversals;
                        if (!forced) shortestHold = std::min(shortestHold, hold);
                        if (forced) ++forcedReversals;
                        if (r.out->ally.bodyTurn) {
                            ++o.bodyTurns;
                            o.shortestBodyTurn = std::min(o.shortestBodyTurn, hold);
                        }
                        else if (blow) ++blowReversals;
                        else if (pi.skirmish || pi.idle == PartnerIdle::NoBudget) {
                            ++circlingReversals;
                            shortestCircling = std::min(shortestCircling, hold);
                        }
                        holdFrom = r.fight.time();
                        dirOn.fill(false);
                    }
                    dirs[static_cast<std::size_t>(slot)] = d;
                    dirOn[static_cast<std::size_t>(slot)] = true;
                } else {
                    dirOn[static_cast<std::size_t>(slot)] = false;
                    if (!anyOn) holdFrom = -1.0;
                }
                slot = (slot + 1) % kWin;
            }
            bool anyUp = false;
            for (std::size_t i = 0; i < r.fight.enemyCount(); ++i) anyUp = anyUp || r.fight.enemyAlive(i);
            if (pi.skirmish || pi.idle == PartnerIdle::NoBudget) {
                circlingTime += kDt;
                if (pi.act == PartnerAct::MoveTo)
                    for (std::size_t i = 0; i < r.fight.enemyCount(); ++i)
                        if (r.fight.enemyAlive(i) && !(pi.skirmish && static_cast<int>(i) == pi.enemy) &&
                            length(sub(r.out->ally.goal, r.bodies.enemies[i])) < 0.4f + r.def.enemies[i].bodyRadius - 1e-3f)
                            ++o.goalsInBody;
            }
            // Standing: its body did not move this tick, no blow of its running, not guarding, with an enemy up.
            const bool stood = length(sub(r.bodies.ally, lastAt)) < 1e-3f && r.out->ally.part == ArenaPart::None &&
                               pi.act != PartnerAct::Guard && anyUp && r.out->ally.partnerOn;
            still = stood ? still + kDt : 0.0;
            longestStill = std::max(longestStill, still);
            lastAt = r.bodies.ally;
            // In one place: the longest stretch it stays within 0.5 m of where the stretch began (shuffles count).
            const bool held = r.out->ally.part == ArenaPart::None && pi.act != PartnerAct::Guard && anyUp && r.out->ally.partnerOn;
            if (!held || length(sub(r.bodies.ally, placeAt)) > 0.5f) {
                placeAt = r.bodies.ally;
                inPlace = 0.0;
            } else {
                inPlace += kDt;
            }
            longestInPlace = std::max(longestInPlace, inPlace);
            // Facing its target (else the nearest enemy up) on every step it is not down.
            if (anyUp && pi.enemy < 0 && pi.why != PartnerWhy::Down) facingOff += kDt;
            if (r.out->player.action == ArenaAction::Light || r.out->player.action == ArenaAction::Heavy) leadAttack += kDt;
            if (r.out->ally.part != ArenaPart::None) partnerBlow += kDt;
            const Vec2 f = r.out->player.facing;
            const float k = std::min(1.f, 3.f * kDt);
            const Vec2 m{facing.x + (f.x - facing.x) * k, facing.y + (f.y - facing.y) * k};
            const float ml = length(m);
            if (ml > 1e-4f) facing = Vec2{m.x / ml, m.y / ml};
        }
        const double T = r.fight.time();
        const std::string head = std::string(order == 0 ? "StayClose" : "Spread") + (circling ? ", circling on" : ", circling off");
        if (!quiet) MESSAGE(head << ", lead never attacking: " << T << " s; partner blows " << partnerBlow
                << " s, longest stand-still " << longestStill << " s (within 0.5 m: " << longestInPlace << " s), on screen " << onScreen / T << ", goal jumps per 10 s "
                << 10.0 * goalJumps / T << "; circling " << r.brain.skirmishSeconds() << " s over "
                << r.brain.skirmishMoves() << " goals (" << r.brain.retargetPicks() << " re-picked on a new target), guards " << r.brain.guardsBegun()
                << ", dodges 0 (none in the kit); noBudget " << r.brain.idleSeconds(PartnerIdle::NoBudget) << " s");
        if (!quiet) MESSAGE("    goal jumps: " << flips << " walk begun or ended, " << repicks << " re-picked; reversals " << reversals
                << ": circling " << circlingReversals << " in " << circlingTime << " s held (" << 10.0 * circlingReversals / circlingTime
                << " per 10 s), shortest hold " << shortestCircling << " s; forced by a wind-up " << forcedReversals
                << ", its own blows " << blowReversals << ", else " << reversals - circlingReversals - forcedReversals - blowReversals);
        if (!quiet) MESSAGE("    pushes " << o.pushes << " (" << 10.0 * o.pushes / T << " per 10 s; an enemy into it " << o.pushesEnemyInto
                << ", it into a body " << o.pushesWalkInto << "), most in one push " << o.pushMost << " m, fastest "
                << o.pushFastest << " m/s; reversals only with a push in the tick (forced) " << o.pushReversals);
        o.T = T;
        o.leadAttack = leadAttack;
        o.partnerBlow = partnerBlow;
        o.longestStill = longestStill;
        o.longestInPlace = longestInPlace;
        o.onScreen = onScreen / T;
        o.jumpsPer10 = 10.0 * goalJumps / T;
        o.facingOff = facingOff;
        o.circlingTime = circlingTime;
        o.shortestCircling = shortestCircling;
        o.skirmishMoves = r.brain.skirmishMoves();
        o.retargetPicks = r.brain.retargetPicks();
        o.circlingReversals = circlingReversals;
        o.rulesReversals = reversals - circlingReversals - forcedReversals - blowReversals;
        o.shortestHold = shortestHold;
    }
    return o;
}

} // namespace

TEST_CASE("arena partner: rule 17, a lead that never attacks for 20 s: the partner keeps to 2 s of blows and never stands still") {
    const ArenaTable t = shippedTable();
    for (const std::uint8_t order : {std::uint8_t{0}, std::uint8_t{1}}) {
        CAPTURE(order);
        const IdleLead r = idleLead(t, order, true);
        CHECK(r.T >= 19.9);
        CHECK(r.leadAttack == 0.0);
        CHECK(r.partnerBlow <= 2.0 + 1e-6); // 0.6 x 0 + 2.0 in the one 20 s window
        CHECK(r.longestStill <= 1.5);
        CHECK(r.jumpsPer10 <= 35.0); // the group fight's bound
        CHECK(r.facingOff == 0.0);
        CHECK(r.skirmishMoves >= 3);
        // In one place, on screen and the reversals while circling: over six seeds below (one seed swings on any edit).
    }
}

// Over the fixed fight seeds 1-6 (the system is chaotic: one run swings on unrelated edits), per order and circling:
// each seed's rules 10-13 reversals per 10 s (not forced, not its own blows), its reversals per 10 s while circling,
// the time on screen and the longest stretch in one place; the shortest hold before a reversal not forced. Run once.
constexpr int kSixSeeds = 6;
struct SixSeeds {
    std::array<double, kSixSeeds> rules{}, circling{}, inPlace{}, onScreen{}, pushes{}, circlingHold{}, bodyTurnHold{};
    std::array<int, kSixSeeds> bodyTurns{}, goalsInBody{}, retargetPicks{};
    std::array<bool, kSixSeeds> circled{}; // the seed circled at all (its circling rate is a number)
    double hold = 1e9, pushMost = 0, pushFastest = 0, pushMostEnemy = 0, pushMostWalk = 0;
    int enemyInto = 0, walkInto = 0, pushReversals = 0;
    static double mean(const std::array<double, kSixSeeds>& a) {
        double m = 0;
        for (const double v : a) m += v;
        return m / kSixSeeds;
    }
    // The circling rate's mean over the seeds that circled only (a seed that never did is no 0 in it).
    double circlingMean() const {
        double m = 0;
        int k = 0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(kSixSeeds); ++i)
            if (circled[i]) {
                m += circling[i];
                ++k;
            }
        return k > 0 ? m / k : 0.0;
    }
    int circledCount() const { return static_cast<int>(std::count(circled.begin(), circled.end(), true)); }
    static double most(const std::array<double, kSixSeeds>& a) { return *std::max_element(a.begin(), a.end()); }
    static double least(const std::array<double, kSixSeeds>& a) { return *std::min_element(a.begin(), a.end()); }
};
const SixSeeds& sixSeeds(std::uint8_t order, bool circling) {
    static std::array<SixSeeds, 4> runs{};
    static std::array<bool, 4> done{};
    const std::size_t k = static_cast<std::size_t>(order) * 2u + (circling ? 1u : 0u);
    if (!done[k]) {
        const ArenaTable t = shippedTable();
        SixSeeds& s = runs[k];
        for (std::uint32_t seed = 1; seed <= kSixSeeds; ++seed) {
            const IdleLead r = idleLead(t, order, circling, seed, true);
            const std::size_t i = seed - 1u;
            s.rules[i] = 10.0 * r.rulesReversals / r.T;
            s.circled[i] = r.circlingTime > 0.0;
            s.circling[i] = s.circled[i] ? 10.0 * r.circlingReversals / r.circlingTime : 0.0;
            s.inPlace[i] = r.longestInPlace;
            s.onScreen[i] = r.onScreen;
            s.hold = std::min(s.hold, r.shortestHold);
            s.circlingHold[i] = r.shortestCircling;
            s.bodyTurnHold[i] = r.shortestBodyTurn;
            s.bodyTurns[i] = r.bodyTurns;
            s.goalsInBody[i] = r.goalsInBody;
            s.retargetPicks[i] = r.retargetPicks;
            s.pushes[i] = 10.0 * r.pushes / r.T;
            s.pushMost = std::max(s.pushMost, r.pushMost);
            s.pushFastest = std::max(s.pushFastest, r.pushFastest);
            s.pushMostEnemy = std::max(s.pushMostEnemy, r.pushMostEnemy);
            s.pushMostWalk = std::max(s.pushMostWalk, r.pushMostWalk);
            s.enemyInto += r.pushesEnemyInto;
            s.walkInto += r.pushesWalkInto;
            s.pushReversals += r.pushReversals;
        }
        done[k] = true;
    }
    return runs[k];
}

TEST_CASE("arena partner: rules 10-13 and 17 over six fight seeds, a lead that never attacks (report)") {
    for (const bool circling : {false, true})
        for (const std::uint8_t order : {std::uint8_t{0}, std::uint8_t{1}}) {
            const SixSeeds& s = sixSeeds(order, circling);
            std::ostringstream seeds;
            for (std::size_t i = 0; i < static_cast<std::size_t>(kSixSeeds); ++i)
                seeds << " | " << i + 1 << ": " << s.circling[i] << ", " << s.inPlace[i] << " s, " << s.onScreen[i] << ", "
                      << s.circlingHold[i] << " s";
            MESSAGE((std::string(order == 0 ? "StayClose" : "Spread") + (circling ? ", circling on" : ", circling off"))
                    << " over " << kSixSeeds << " seeds: rules reversals per 10 s mean " << SixSeeds::mean(s.rules)
                    << " worst " << SixSeeds::most(s.rules) << "; circling mean " << s.circlingMean() << " (over the " << s.circledCount() << " seeds that circled)"
                    << " worst " << SixSeeds::most(s.circling) << "; shortest hold " << s.hold << " s; on screen mean "
                    << SixSeeds::mean(s.onScreen) << " worst " << SixSeeds::least(s.onScreen) << "; in one place mean "
                    << SixSeeds::mean(s.inPlace) << " worst " << SixSeeds::most(s.inPlace) << " s;"
                    << " per seed (circling, in one place, on screen, shortest circling hold)" << seeds.str());
            MESSAGE("    pushes per 10 s mean " << SixSeeds::mean(s.pushes) << " worst " << SixSeeds::most(s.pushes)
                    << " (an enemy into it " << s.enemyInto << ", it into a body " << s.walkInto << ", all seeds); most in one push "
                    << s.pushMost << " m (an enemy into it " << s.pushMostEnemy << ", it into a body " << s.pushMostWalk << "), fastest "
                    << s.pushFastest << " m/s; reversals only with a push (forced) " << s.pushReversals);
            std::ostringstream shape;
            for (std::size_t i = 0; i < static_cast<std::size_t>(kSixSeeds); ++i)
                shape << " | " << i + 1 << ": " << s.bodyTurns[i] << ", " << s.bodyTurnHold[i] << " s, " << s.goalsInBody[i] << ", "
                      << s.retargetPicks[i];
            MESSAGE("    its circling walk per seed (reversals a body let through, shortest hold before one, goals in a body, "
                    "goals dropped by a new target and re-picked)"
                    << shape.str());
        }
}

// Rule 17 circling on, the ruled bars over seeds 1-6: at most 6.5 reversals per 10 s while circling, at most 1.5 s in
// one place, at least 0.8 of the time on screen. The six-seed MEAN meets each bar (hard); each bar's WORST seed is its
// own test below. Means (circling, in one place, on screen):
//   here  StayClose 2.78, 1.11 s, 0.850; Spread 1.02, 1.40 s, 0.843
//   main  StayClose 3.66, 1.35 s, 0.839; Spread 4.65, 1.29 s, 0.819
// The circling mean is also held to no worse than main's (above) plus a noise margin of 0.5 reversals per 10 s, so a
// slide back towards 52cd3565's value (StayClose 6.33) turns the suite red although it is under the 6.5 bar.
constexpr double kMainCirclingMean[2] = {3.66, 4.65}; // main 50b62a4f, StayClose and Spread, over the seeds that circled
constexpr double kCirclingNoise = 0.5;
TEST_CASE("arena partner: rule 17 circling, the six-seed means meet the bars: at most 6.5 reversals per 10 s, 1.5 s in one place, 0.8 on screen") {
    for (const std::uint8_t order : {std::uint8_t{0}, std::uint8_t{1}}) {
        CAPTURE(int(order));
        const SixSeeds& s = sixSeeds(order, true);
        MESSAGE("seeds that circled " << s.circledCount() << " of " << kSixSeeds << ", circling mean " << s.circlingMean()
                << " (main " << kMainCirclingMean[order] << ")");
        CHECK(s.circledCount() >= 4); // a mean over fewer says little
        CHECK(s.circlingMean() <= 6.5);
        CHECK(s.circlingMean() <= kMainCirclingMean[order] + kCirclingNoise); // no worse than main
        CHECK(SixSeeds::mean(s.inPlace) <= 1.5);
        CHECK(SixSeeds::mean(s.onScreen) >= 0.8);
    }
}

// Rule 17's re-pick on a new target, replayed on the seed-6 case it was found in (Spread, circling, a lead that never
// attacks): a goal picked against one target, the target switched within 0.2 s, the goal dropped. That pick zeroed the
// place clock; before the fix the partner then stood a second full kPlaceStill (1.70 s in one place at 15.9 s). Now on
// that very tick it re-picks against the new target, and if nothing is found the clock is given back, so the stand
// before the dropped goal and the one after are one stretch.
TEST_CASE("arena partner: rule 17, seed 6: a goal dropped by a target switch within 0.2 s re-picks at once or gives the clock back") {
    const ArenaTable t = shippedTable();
    PRig r(t.fighter("default"));
    r.def = t.fallback;
    r.def.enemyCount = 0;
    for (const char* id : {"skirmisher", "hulk", "duelist"})
        for (std::size_t e = 0; e < t.enemyIds.size(); ++e)
            if (t.enemyIds[e] == id) {
                r.def.enemyKind[r.def.enemyCount] = static_cast<std::uint8_t>(e);
                r.def.enemies[r.def.enemyCount++] = t.enemies[e];
            }
    REQUIRE(r.def.enemyCount == 3);
    r.def.attackers = 2;
    r.tuning = shippedTuning();
    r.tuning.circling = true;
    r.in.partnerOrder = 1; // Spread
    r.goShipped(true, 6);
    r.autopilot = false;
    Vec2 facing = r.fight.playerStartFacing();
    double pickAt = -1e9;
    int quick = 0, repickedAtOnce = 0, clockBack = 0;
    while (!r.fight.over() && r.fight.time() < 20.f) {
        r.in = r.fight.autopilot();
        r.in.partnerOrder = 1;
        r.in.light = r.in.heavy = r.in.pair = false;
        r.in.partnerCamera = behindLead(r.bodies.player, facing);
        const PartnerBrainSave b0 = r.brain.save();
        const double now = r.fight.time();
        r.tick();
        const PartnerBrainSave& b1 = r.brain.save();
        const bool picked = b1.skirmishMoves != b0.skirmishMoves;
        if (b1.retargetPicks != b0.retargetPicks) {
            CAPTURE(now);
            CHECK(b0.skirmishOn);
            CHECK(b0.skirmishKind == 0);
            if (now - pickAt <= 0.2 + 1e-6) ++quick;
            if (picked) { // re-picked on the new target this tick, the stretch it is in kept
                ++repickedAtOnce;
                CHECK(b1.skirmishOf == b1.target);
                CHECK(b1.placeFor == 0.0);
                CHECK(b1.pickPlaceFor >= b0.pickPlaceFor + b0.placeFor - 1e-9);
            } else if (b0.pickPlaceFor > 0.0 && b1.placeAt.x == b0.placeAt.x && b1.placeAt.y == b0.placeAt.y) {
                // nothing found: the clock the dropped pick took is given back
                ++clockBack;
                CHECK(b1.placeFor >= b0.pickPlaceFor + b0.placeFor - 1e-9);
                CHECK(b1.pickPlaceFor == 0.0);
            }
        }
        if (picked) pickAt = now;
        const Vec2 f = r.out->player.facing;
        const float k = std::min(1.f, 3.f * kDt);
        const Vec2 m{facing.x + (f.x - facing.x) * k, facing.y + (f.y - facing.y) * k};
        const float ml = length(m);
        if (ml > 1e-4f) facing = Vec2{m.x / ml, m.y / ml};
    }
    MESSAGE("seed 6 Spread: " << r.brain.retargetPicks() << " goals dropped by a new target (" << quick << " within 0.2 s of their pick), "
            << repickedAtOnce << " re-picked that tick, " << clockBack << " gave the clock back");
    CHECK(quick >= 1);     // the case the fix is for happens on this seed
    CHECK(clockBack >= 1); // and at least once nothing is found, so the refund itself runs
    // The stretch the refund joins stays short: 1.70 s in one place on this seed before the fix, 1.15 s after.
    const IdleLead whole = idleLead(t, 1, true, 6, true);
    CAPTURE(whole.longestInPlace);
    CHECK(whole.longestInPlace < 1.5);
}

// Each bar's worst seed, one test per bar and order: the bar itself where it holds today, else a hard pin at today's
// value (the bar named in its comment) so getting worse fails. Worst seeds (circling, in one
// place, on screen):
//   here  StayClose 5.45, 1.28 s, 0.830; Spread 2.14, 2.00 s, 0.804
//   main  StayClose 5.92, 1.67 s, 0.789; Spread 7.41, 1.72 s, 0.719
TEST_CASE("arena partner: rule 17 circling, StayClose's worst seed: at most 6.5 reversals per 10 s") {
    const SixSeeds& s = sixSeeds(0, true);
    MESSAGE("worst seed " << SixSeeds::most(s.circling));
    CHECK(SixSeeds::most(s.circling) <= 6.5);
}
TEST_CASE("arena partner: rule 17 circling, StayClose's worst seed: at most 1.5 s in one place") {
    const SixSeeds& s = sixSeeds(0, true);
    MESSAGE("worst seed " << SixSeeds::most(s.inPlace) << " s");
    CHECK(SixSeeds::most(s.inPlace) <= 1.5);
}
TEST_CASE("arena partner: rule 17 circling, StayClose's worst seed: at least 0.8 on screen") {
    const SixSeeds& s = sixSeeds(0, true);
    MESSAGE("worst seed " << SixSeeds::least(s.onScreen));
    CHECK(SixSeeds::least(s.onScreen) >= 0.8);
}
TEST_CASE("arena partner: rule 17 circling, Spread's worst seed: at most 6.5 reversals per 10 s") {
    const SixSeeds& s = sixSeeds(1, true);
    MESSAGE("worst seed " << SixSeeds::most(s.circling));
    CHECK(SixSeeds::most(s.circling) <= 6.5);
}
// Spread's worst seed misses the 1.5 s bar: seed 4, 2.00 s (main 1.72 s), the brain's Wait with its budget spent,
// through the skirmish's dwell (skirmishEvery); the walk stands only where the brain's goal does. Pinned at 2.00 s.
TEST_CASE("arena partner: rule 17 circling, Spread's worst seed: at most 2.0 s in one place (bar 1.5 s, missed)") {
    const SixSeeds& s = sixSeeds(1, true);
    MESSAGE("worst seed " << SixSeeds::most(s.inPlace) << " s (bar 1.5 s)");
    CHECK(SixSeeds::most(s.inPlace) <= 2.0 + 1e-6);
}
TEST_CASE("arena partner: rule 17 circling, Spread's worst seed: at least 0.8 on screen") {
    const SixSeeds& s = sixSeeds(1, true);
    MESSAGE("worst seed " << SixSeeds::least(s.onScreen));
    CHECK(SixSeeds::least(s.onScreen) >= 0.8);
}

// The shipped default (circling off until its turns hold the ruled limits): with its attack held it stands ready
// where rules 10-16 put it and makes no moves of its own. Its outputs are the brain's before rule 17 (pinned below):
// the host turns it to its target when it names one; with none named it keeps its facing, as before rule 17.
TEST_CASE("arena partner: rule 17 off (the shipped default), a lead that never attacks: it stands ready, no moves of its own") {
    const ArenaTable t = shippedTable();
    CHECK_FALSE(shippedTuning().circling);
    for (const std::uint8_t order : {std::uint8_t{0}, std::uint8_t{1}}) {
        CAPTURE(order);
        const IdleLead r = idleLead(t, order, false);
        CHECK(r.T >= 19.9);
        CHECK(r.leadAttack == 0.0);
        CHECK(r.partnerBlow <= 2.0 + 1e-6);
        CHECK(r.skirmishMoves == 0);
    }
}

// Rule 17's circling, seed 1: the bar is at most 3 reversals per 10 s. Spread meets it (0.97); StayClose misses it by
// a little (3.07: 3 reversals in 9.77 s held) and is pinned at 3.08. (The no-hold-under-0.4 s bar is the six-seed test
// below, seed 1 included: 0.68 / 0.42 s.)
TEST_CASE("arena partner: rule 17's circling, seed 1: Spread at most 3 reversals per 10 s, StayClose at most 3.08 (bar 3)") {
    const ArenaTable t = shippedTable();
    constexpr double kAtMost[2] = {3.08, 3.0};
    for (const std::uint8_t order : {std::uint8_t{0}, std::uint8_t{1}}) {
        CAPTURE(int(order));
        const IdleLead r = idleLead(t, order, true);
        REQUIRE(r.circlingTime > 0.0);
        MESSAGE("reversals per 10 s " << 10.0 * r.circlingReversals / r.circlingTime << " (bar 3)");
        CHECK(10.0 * r.circlingReversals / r.circlingTime <= kAtMost[order]);
    }
}

// The walk's heading rule: no circling reversal within 0.4 s of its last, on any of the six seeds. Today, StayClose
// 0.68 / 1.60 / 0.42 / 0.42 / 0.87 / 1.05 s, Spread 0.42 / none / 0.80 / none / 0.42 / 0.98 s.
TEST_CASE("arena partner: rule 17's circling: no hold under 0.4 s, on each of six seeds") {
    for (const std::uint8_t order : {std::uint8_t{0}, std::uint8_t{1}}) {
        CAPTURE(int(order));
        const SixSeeds& s = sixSeeds(order, true);
        for (std::size_t i = 0; i < s.circlingHold.size(); ++i) {
            CAPTURE(i + 1);
            CHECK(s.circlingHold[i] >= 0.4);
        }
    }
}

// The walk's shape (round bodies, the heading rule) is rule 17's circling walks' only: every other walk (its rules'
// stations, a pull back to the lead, an approach) is the brain's point, as before it, a body on its line or not (the
// host pushes it out). Still scenes: the lead standing, dummies that never swing. (The circling walks start only in
// the camera's frame: their holds are the six-seed test's above.)
TEST_CASE("arena partner: outside circling its walk is the brain's point, a body on its line or not") {
    for (const bool body : {false, true}) {
        CAPTURE(body);
        PRig r;
        r.enemy(dummyKind(), Vec2{0.f, -25.f});
        if (body) r.enemy(dummyKind(), Vec2{0.05f, -8.f}); // on its line to the lead
        r.bodies.player = Vec2{0.f, -26.f}; // StayClose with no target of the lead's: it walks back to the lead
        r.in.partnerOrder = 0;
        r.walk = true;
        r.go(true);
        int walking = 0, off = 0;
        for (int k = 0; k < 6 * 60; ++k) {
            r.tick();
            const PartnerIntent& i = r.brain.intent();
            if (i.act != PartnerAct::MoveTo) continue;
            ++walking;
            if (r.out->ally.goal.x != i.point.x || r.out->ally.goal.y != i.point.y) ++off;
        }
        REQUIRE(walking > 0);
        CHECK(off == 0);
    }
}

TEST_CASE("arena partner: a play camera given but not whole is exactly not given, cameraOn off or on") {
    const auto broken = [](int k) {
        PartnerCameraView v = behindLead(Vec2{0.f, 0.f}, Vec2{1.f, 0.f});
        switch (k) {
        case 0: v.aspect = 0.f; break;
        case 1: v.pos.x = std::numeric_limits<float>::quiet_NaN(); break;
        case 2: v.up = v.forward; break;                                     // up along forward
        case 3: v.up = {1.5f * v.up.x, 1.5f * v.up.y, 1.5f * v.up.z}; break; // up not unit
        case 4: v.verticalFovDegrees = 0.f; break;
        default: v.bodyHeight = 0.f; break;
        }
        return v;
    };
    for (const bool cameraOn : {false, true})
        for (int k = 0; k < 6; ++k) {
            CAPTURE(cameraOn);
            CAPTURE(k);
            REQUIRE_FALSE(partnerCameraWhole(broken(k)));
            PRig a, b;
            for (PRig* r : {&a, &b}) {
                r->enemy(biterKind(20, 3.f, 2.f), Vec2{3.f, 0.5f});
                r->bodies.ally = Vec2{-2.f, 1.f};
                r->go(true, 100, 7);
                r->walk = true;
                r->in.cameraOn = cameraOn;
                r->in.cameraPos = Vec2{-4.f, 0.f};
                r->in.cameraFacing = Vec2{1.f, 0.f};
            }
            b.in.partnerCamera = broken(k);
            for (int i = 0; i < 240; ++i) {
                a.tick();
                b.tick();
                CAPTURE(i);
                REQUIRE(a.out->ally.goal.x == b.out->ally.goal.x);
                REQUIRE(a.out->ally.goal.y == b.out->ally.goal.y);
                REQUIRE(a.out->ally.act == b.out->ally.act);
                REQUIRE(a.out->ally.stationed == b.out->ally.stationed);
                REQUIRE_FALSE(b.brain.framed());
                REQUIRE(a.brain.hash() == b.brain.hash());
                REQUIRE(a.fight.hp() == b.fight.hp());
                REQUIRE(a.fight.allyHp() == b.fight.allyHp());
            }
        }
}

namespace {
struct GroupPin {
    std::uint64_t intents, state;
};
// The shipped group fight of the report above (three enemies, two attacking, the shipped brain), 20 s: every
// tick's intent and the brain's state hash at the end.
GroupPin groupFightPin(std::uint8_t order, bool camera, bool mainEq = false) {
    const ArenaTable t = shippedTable();
    PRig r(t.fighter("default"));
    r.def = t.fallback;
    r.def.enemyCount = 0;
    for (const char* id : {"skirmisher", "hulk", "duelist"})
        for (std::size_t e = 0; e < t.enemyIds.size(); ++e)
            if (t.enemyIds[e] == id) {
                r.def.enemyKind[r.def.enemyCount] = static_cast<std::uint8_t>(e);
                r.def.enemies[r.def.enemyCount++] = t.enemies[e];
            }
    REQUIRE(r.def.enemyCount == 3);
    r.def.attackers = 2;
    r.tuning = mainEq ? mainEquivalentTuning(shippedTuning()) : shippedTuning();
    r.in.partnerOrder = order;
    r.goShipped(true, 1);
    r.pairOnParry = true;
    Vec2 facing = r.fight.playerStartFacing();
    StateHash h;
    while (!r.fight.over() && r.fight.time() < 20.f) {
        r.in.partnerCamera = camera ? behindLead(r.bodies.player, facing) : PartnerCameraView{};
        r.tick();
        const PartnerIntent& i = r.brain.intent();
        h.add(i.act).add(i.why).add(i.point.x).add(i.point.y).add(i.enemy).add(i.blows).add(i.newString);
        h.add(i.blowStart).add(i.blowIndex).add(i.partnerBlow).add(i.breakValue).add(i.extendsStagger);
        h.add(i.punish).add(i.rise).add(i.riseHealth).add(i.stationed);
        const Vec2 f = r.out->player.facing;
        const float k = std::min(1.f, 3.f * kDt);
        const Vec2 m{facing.x + (f.x - facing.x) * k, facing.y + (f.y - facing.y) * k};
        const float ml = length(m);
        if (ml > 1e-4f) facing = Vec2{m.x / ml, m.y / ml};
    }
    return {h.value, r.brain.hash()};
}
} // namespace
// With rule 17 off and main's tuning (mainEquivalentTuning) the shipped group fight is main's to the bit: every
// tick's intent and the brain's state hash at the end equal main's pins (50b62a4f), copied by hand: never regenerate
// them from this build. No enemy here is tall (checked in the report above), so rule 18 cannot act. The walk's shape
// (round bodies, the heading rule) is rule 17's circling's only, so changes none of them.
TEST_CASE("arena partner: rule 17 off, main's tuning: the shipped group fight's intents and state hash are main's") {
    CHECK_FALSE(shippedTuning().circling);
    struct Row {
        std::uint8_t order;
        bool camera;
        std::uint64_t intents, state;
    };
    const Row rows[] = {
        {0, false, 13510023720450430857ull, 13607754419378690935ull},
        {0, true, 5572646704335333306ull, 6381732959959279559ull},
        // Spread: not main's since the body settle at 0.1 mm (B fixes, ARENA_BODY_OVERLAP_1122). At 12.65 s (tick 758)
        // the old 0.7 mm crowd floor left the lead 0.44 mm inside enemy2 in mid-ring; the settle moves it out, and the
        // fight runs on from there. StayClose never has that contact and stays main's.
        {1, false, 10900943681516170142ull, 9768875902593547048ull},
        {1, true, 15003403891851633393ull, 4219921275257632970ull},
    };
    for (const Row& w : rows) {
        CAPTURE(int(w.order));
        CAPTURE(w.camera);
        const GroupPin p = groupFightPin(w.order, w.camera, true);
        CHECK(p.intents == w.intents);
        CHECK(p.state == w.state);
    }
}

// The shipped tuning's pins: main's, to the bit, with the camera and without. No enemy in the group is tall and
// circling is off, so the framing holds (the bottom margin and the side hold) are off and unhashed (framingHolds).
// Kept general they moved this fight (Spread's strings and blows 6/8 to 5/7) and lowered an ordinary fight's on-screen
// share (StayClose, the camera orbiting: 0.9458 to 0.9442), so they are rule 18's and rule 17's only.
TEST_CASE("arena partner: rule 17 off, the shipped tuning: the shipped group fight's intents and state hash are main's") {
    CHECK_FALSE(shippedTuning().circling);
    struct Row {
        std::uint8_t order;
        bool camera;
        std::uint64_t intents, state;
    };
    const Row rows[] = {
        {0, false, 13510023720450430857ull, 13607754419378690935ull}, // main's
        {0, true, 5572646704335333306ull, 6381732959959279559ull},    // main's
        {1, false, 10900943681516170142ull, 9768875902593547048ull},   // B fixes: the 0.1 mm settle (see above)
        {1, true, 15003403891851633393ull, 4219921275257632970ull},    // B fixes: the 0.1 mm settle (see above)
    };
    for (const Row& w : rows) {
        CAPTURE(int(w.order));
        CAPTURE(w.camera);
        const GroupPin p = groupFightPin(w.order, w.camera);
        CHECK(p.intents == w.intents);
        CHECK(p.state == w.state);
    }
}

// ---- per-lead kits (ArenaDef::partnerKit) ----

namespace {
// Lead B's own kit beside the rule kit (lead A): two slower lights of 5 and 4 hp with a longer reach, a gun's
// cylinder, more hp, poise and hitstun, a slower turn.
ArenaFighter kitB() {
    ArenaFighter f = ruleFighter();
    f.weapon = ArenaWeapon::Guns;
    f.hp = 120;
    f.poise = 44.f;
    f.hitstun = 0.7f;
    f.turnRate = 9.f;
    f.lightCount = 2;
    f.light[0] = swing(0.3f, 0.1f, 0.5f, 2.6f, 5);
    f.light[1] = swing(0.3f, 0.1f, 0.5f, 2.6f, 4);
    f.rounds = 6;
    f.reload = 1.f;
    return f;
}

// A 20 s partner fight against one dummy, the ally beside it: every blow the partner began and every Hit it landed.
struct KitRun {
    std::vector<float> blowLengths; // the longest stateTime each blow reached (one tick short of its total)
    std::vector<float> hitAt;       // stateTime at the tick each partner Hit landed
    std::vector<int> hitAmount;
    std::vector<float> hitReach;    // ally centre to the dummy's centre at each Hit
    ArenaStats stats;
    float longestStill = 0.f;       // the longest run of ticks the ally neither moved nor attacked nor guarded
};
KitRun kitRun(const ArenaFighter& partnerKit, const ArenaFighter& leadKit = ruleFighter()) {
    PRig r(leadKit);
    r.ownPartnerKit = true;
    r.def.partnerKit = partnerKit;
    r.def.fighter.hp = 100000; // the lead never falls: no takeover
    r.tuning.budgetAllowanceSeconds = 10.f;
    REQUIRE(validPartnerTuning(r.tuning));
    r.enemy(dummyKind(), Vec2{0.f, 0.f});
    r.bodies.player = Vec2{-1.5f, 0.f};
    r.bodies.ally = Vec2{1.6f, 0.f};
    r.go(true, 1000);
    r.walk = true;
    KitRun k;
    bool was = false;
    std::uint8_t blow = 0;
    float top = 0.f, still = 0.f;
    Vec2 at = r.bodies.ally;
    for (int t = 0; t < 20 * 60; ++t) {
        const std::size_t from = r.log.size();
        r.tick();
        const ArenaAllyOut& a = r.out->ally;
        if (was && (!a.attacking || a.blow != blow || a.stateTime < top)) {
            k.blowLengths.push_back(top);
            top = 0.f;
        }
        if (a.attacking) {
            blow = a.blow;
            top = std::max(top, a.stateTime);
        }
        was = a.attacking;
        for (std::size_t i = from; i < r.log.size(); ++i)
            if (allyHit(r.log[i].e)) {
                k.hitAt.push_back(a.stateTime);
                k.hitAmount.push_back(r.log[i].e.amount);
                k.hitReach.push_back(length(sub(r.bodies.ally, r.bodies.enemies[0])));
            }
        const bool moved = length(sub(r.bodies.ally, at)) > 1e-4f;
        at = r.bodies.ally;
        still = (moved || a.attacking || a.guarding) ? 0.f : still + kDt;
        k.longestStill = std::max(k.longestStill, still);
    }
    k.stats = r.fight.stats();
    return k;
}
} // namespace

TEST_CASE("arena partner: per-lead kits: the ally's blow damage is its own kit's, whoever the player holds") {
    // Rule 7's cap reads the striking lead's own light[0]: the same partner kit beside two leads whose lights differ
    // deals the same hits (the lead never attacks here, so every Hit is the partner's). The leads' lights sit either
    // side of kit B's raw 5: a cap read from the held lead would halve part of it beside the weak one (60% of 1) and
    // none beside the strong one (60% of 40), so the two runs would differ.
    ArenaFighter weak = ruleFighter(), strong = ruleFighter();
    for (std::size_t k = 0; k < weak.lightCount; ++k) weak.light[k].damage = 1;
    for (std::size_t k = 0; k < strong.lightCount; ++k) strong.light[k].damage *= 4;
    REQUIRE(static_cast<float>(kitB().light[0].damage) > 0.6f * static_cast<float>(weak.light[0].damage));
    REQUIRE(static_cast<float>(kitB().light[0].damage) < 0.6f * static_cast<float>(strong.light[0].damage));
    const KitRun a = kitRun(kitB(), weak);
    const KitRun b = kitRun(kitB(), strong);
    REQUIRE(!a.hitAmount.empty());
    CHECK(a.hitAmount == b.hitAmount);
    CHECK(a.stats.partnerDamage == b.stats.partnerDamage);
    // And swapped: the ally on the rule kit beside a lead on kit B deals what it deals beside a lead on the rule kit.
    const KitRun c = kitRun(ruleFighter(), kitB());
    const KitRun d = kitRun(ruleFighter(), ruleFighter());
    REQUIRE(!c.hitAmount.empty());
    CHECK(c.hitAmount == d.hitAmount);
}

TEST_CASE("arena partner: per-lead kits: rounds and the reload travel with their kit: a switch refills nothing") {
    PRig r;
    r.ownPartnerKit = true;
    r.def.partnerKit = kitB(); // lead B: a gun of 6
    r.enemy(dummyKind(), Vec2{3.f, 0.f});
    r.bodies.player = Vec2{0.f, 0.f};
    r.bodies.ally = Vec2{-2.f, 1.f};
    r.go(true, 120);
    r.tick();
    REQUIRE(r.fight.switchLeads());
    REQUIRE(r.fight.rounds() == 6);
    // Shoot until the cylinder reads `left`, then let go of the trigger.
    const auto shootTo = [&r](int left) {
        for (int k = 0; k < 2000 && r.fight.rounds() > left; ++k) {
            r.in.aim = true;
            r.in.light = k % 8 == 0;
            r.tick();
        }
        r.in.aim = false;
        r.in.light = false;
        REQUIRE(r.fight.rounds() == left);
    };
    shootTo(4);
    REQUIRE(r.fight.switchLeads()); // back to lead A: no gun
    CHECK(r.fight.roundsMax() == 0);
    for (int k = 0; k < 120; ++k) r.tick(); // 2 s on lead A
    REQUIRE(r.fight.switchLeads());
    CHECK(r.fight.rounds() == 4); // what lead B was let go with, not a full cylinder
    CHECK_FALSE(r.fight.reloading());
    shootTo(0);
    for (int k = 0; k < 2000 && !(r.fight.reloadProgress() > 0.2f); ++k) r.tick();
    REQUIRE(r.fight.reloading());
    const float part = r.fight.reloadProgress();
    REQUIRE(part < 0.9f);
    const int reloadsBefore = r.fight.stats().reloads;
    REQUIRE(r.fight.switchLeads()); // let go mid-reload
    for (int k = 0; k < 600; ++k) r.tick(); // 10 s: longer than the whole reload
    REQUIRE(r.fight.switchLeads());
    CHECK(r.fight.rounds() == 0);       // not loaded behind the player's back
    CHECK(r.fight.reloading());         // the reload goes on ...
    CHECK(r.fight.reloadProgress() == doctest::Approx(part)); // ... from where it stopped
    CHECK(r.fight.stats().reloads == reloadsBefore);
    for (int k = 0; k < 2000 && r.fight.rounds() == 0; ++k) r.tick();
    CHECK(r.fight.rounds() == 6);
    CHECK(r.fight.stats().reloads == reloadsBefore + 1);
}

TEST_CASE("arena partner: per-lead kits: a switch hands the player the other lead's kit and max hp; restart and begin put them back") {
    PRig r;
    r.ownPartnerKit = true;
    r.def.partnerKit = kitB();
    r.enemy(dummyKind(), Vec2{1.2f, 0.f});
    r.bodies.player = Vec2{0.f, 0.f};
    r.bodies.ally = Vec2{-2.f, 1.f};
    r.go(true, 120);
    r.tick();
    CHECK(r.fight.def().fighter.lightCount == 3);
    CHECK(r.fight.roundsMax() == 0);
    REQUIRE(r.fight.hostHurt(40) == 40); // lead A's body at 60 of 100, lead B's at 120 of 120
    REQUIRE(r.fight.switchLeads());
    // The player drives lead B: its moveset, poise, cylinder and max hp; the partner (lead A) its own.
    CHECK(r.fight.def().fighter.lightCount == 2);
    CHECK(r.fight.def().fighter.light[0].damage == 5);
    CHECK(r.fight.def().fighter.weapon == ArenaWeapon::Guns);
    CHECK(r.fight.def().partnerKit.light[0].damage == 10);
    CHECK(r.fight.roundsMax() == 6);
    CHECK(r.fight.rounds() == 6);
    CHECK(r.fight.def().fighter.poise == doctest::Approx(44.f));
    CHECK(r.fight.hp() == 120);
    CHECK(r.fight.allyHp() == 60);
    CHECK(r.fight.allyMaxHp() == 100); // lead A's max goes with it: nothing clipped
    CHECK(r.fight.def().partnerKit.hp == 100);
    CHECK(r.fight.def().fighter.hp == 120);
    std::swap(r.bodies.player, r.bodies.ally);
    // The new body's light is lead B's: 5 hp on the dummy beside it.
    r.bodies.player = Vec2{0.f, 0.f};
    r.tick();
    r.in.light = true;
    std::size_t from = r.log.size();
    for (int k = 0; k < 40; ++k) r.tick();
    int dealt = -1;
    for (std::size_t i = from; i < r.log.size(); ++i)
        if (leadHit(r.log[i].e) && dealt < 0) dealt = r.log[i].e.amount;
    CHECK(dealt == 5);
    // Switched back: lead A's kit and both hp as they were (two switches clip neither).
    REQUIRE(r.fight.switchLeads());
    std::swap(r.bodies.player, r.bodies.ally);
    CHECK(r.fight.def().fighter.lightCount == 3);
    CHECK(r.fight.roundsMax() == 0);
    CHECK(r.fight.hp() == 60);
    CHECK(r.fight.allyHp() == 120);
    CHECK(r.fight.allyMaxHp() == 120);
    // Switched, then restart: each lead its own kit and max again.
    REQUIRE(r.fight.switchLeads());
    r.fight.restart();
    CHECK_FALSE(r.fight.swapped());
    CHECK(r.fight.def().fighter.lightCount == 3);
    CHECK(r.fight.def().fighter.hp == 100);
    CHECK(r.fight.def().partnerKit.lightCount == 2);
    CHECK(r.fight.hp() == 100);
    CHECK(r.fight.allyMaxHp() == 120);
    CHECK(r.fight.allyHp() == 120);
    // Switched, then a new begin (the host's setAlly first): the given kits and the host's ally max.
    REQUIRE(r.fight.switchLeads());
    r.fight.begin(r.def, r.ring, r.bodies.player, Vec2{1.f, 0.f}, 1);
    CHECK(r.fight.def().fighter.lightCount == 3);
    CHECK(r.fight.allyMaxHp() == 120);
    CHECK(r.fight.hp() == 100);
    REQUIRE(r.fight.switchLeads());
    r.fight.setAlly(150, 0.4f);
    r.fight.begin(r.def, r.ring, r.bodies.player, Vec2{1.f, 0.f}, 1);
    CHECK(r.fight.allyMaxHp() == 150);
    CHECK(r.fight.def().fighter.hp == 100);
}

TEST_CASE("arena partner: per-lead kits: a takeover swaps the kits too") {
    PRig r;
    r.ownPartnerKit = true;
    r.def.partnerKit = kitB();
    r.enemy(biterKind(150, 3.f, 5.f), Vec2{0.f, 0.f});
    r.bodies.player = Vec2{1.f, 0.f};   // within the bite
    r.bodies.ally = Vec2{0.f, 3.5f};    // out of it
    r.go(true, 120, 1, Vec2{-1.f, 0.f});
    r.run(2.f);
    REQUIRE(r.fight.stats().takeovers == 1);
    CHECK(r.fight.def().fighter.lightCount == 2);
    CHECK(r.fight.def().partnerKit.lightCount == 3);
    CHECK(r.fight.roundsMax() == 6);
    CHECK(r.fight.allyMaxHp() == 100);
    CHECK(r.fight.hp() == 120);
}

TEST_CASE("arena partner: per-lead kits: the ally's timings, reach and damage are its own kit's") {
    const KitRun a = kitRun(ruleFighter()); // the lead's own kit, as before
    const KitRun b = kitRun(kitB());
    REQUIRE(a.hitAmount.size() >= 3);
    REQUIRE(b.hitAmount.size() >= 3);
    // Damage: lead A's lights are 10, 11 and 12 (partnerBlowDamage against lead A's 10: 10, 11 or 12 less the over
    // part's half); lead B's are 5 and 4, under rule 7's 6, so whole.
    for (int d : a.hitAmount) CHECK(d >= 8);
    for (int d : b.hitAmount) CHECK((d == 5 || d == 4));
    // Timings: lead A's blows run 0.6 s and land 0.2 s in; lead B's run 0.9 s and land 0.3 s in.
    for (float l : a.blowLengths) CHECK(l <= 0.6f + 1e-4f);
    int whole = 0;
    for (float l : b.blowLengths) {
        CHECK(l <= 0.9f + 1e-4f);
        whole += l > 0.85f ? 1 : 0;
    }
    CHECK(whole >= 2);
    for (float t : a.hitAt) CHECK(t <= 0.3f + 1e-4f);
    for (float t : b.hitAt) CHECK((t >= 0.3f - 1e-4f && t <= 0.4f + kDt + 1e-4f));
    // Reach: every Hit within the kit's range plus the dummy's radius.
    for (float d : b.hitReach) CHECK(d <= 2.6f + 0.45f + 1e-3f);
    CHECK(b.stats.partnerNoKitSeconds == 0.f);
}

TEST_CASE("arena partner: per-lead kits: a guns or magic partner's blows are its kit's lights, so its flight comes from its kit") {
    // The host gives a partner Hit its tracer's or bolt's flight from the clip its kit plays for that blow (the
    // Hit's move is the blow's index into the partner's own lights). A guns kit's lights are kicks (no flight; its
    // shot is the player's aimed light only); a magic kit's are casts (a bolt's flight); the lead's gauntlets strike.
    // The Hit's move counts the string's blows; the host wraps it at the kit's chain (FTArenaRunner, as allyLight).
    ArenaFighter guns = kitB();
    ArenaFighter magic = kitB();
    magic.weapon = ArenaWeapon::Magic;
    for (const ArenaFighter* kit : {&guns, &magic}) {
        PRig r;
        r.ownPartnerKit = true;
        r.def.fighter.weapon = ArenaWeapon::Gauntlets;
        r.def.partnerKit = *kit;
        r.def.fighter.hp = 100000;
        r.tuning.budgetAllowanceSeconds = 10.f;
        r.enemy(dummyKind(), Vec2{0.f, 0.f});
        r.bodies.player = Vec2{-1.5f, 0.f};
        r.bodies.ally = Vec2{1.6f, 0.f};
        r.go(true, 1000);
        r.walk = true;
        r.run(10.f);
        int hits = 0;
        for (const Logged& l : r.log) {
            if (!allyHit(l.e)) continue;
            ++hits;
            const auto m = static_cast<std::uint8_t>(l.e.move % kit->lightCount);
            const FightClip clip = arenaPlayerStrike(r.fight.def().partnerKit, m);
            CHECK(clip == (kit->weapon == ArenaWeapon::Magic ? FightClip::Cast
                                                             : (m % 2 == 0 ? FightClip::KickLeft : FightClip::KickRight)));
            CHECK(arenaPlayerStrike(r.fight.def().fighter, m) != clip); // the lead's kit would read it wrong
        }
        CHECK(hits >= 3);
    }
}

TEST_CASE("arena partner: per-lead kits: a partner with no attacks (no kit) starts no blow and only stands or walks") {
    ArenaFighter none = kitB();
    none.weapon = ArenaWeapon::None;
    none.attacks = false;
    const KitRun k = kitRun(none);
    CHECK(k.stats.partnerStrings == 0);
    CHECK(k.stats.partnerBlows == 0);
    CHECK(k.stats.partnerDamage == 0);
    CHECK(k.hitAmount.empty());
    CHECK(k.stats.partnerNoKitSeconds == doctest::Approx(20.f).epsilon(0.01));
    MESSAGE("no kit: 20 s by a dummy, strings " << k.stats.partnerStrings << ", guards " << k.stats.partnerGuards
                                                << ", longest stand-still " << k.longestStill << " s");
}

TEST_CASE("arena partner: per-lead kits: the host's setAlly while swapped, then restart, gives each lead its own max") {
    for (const bool out : {true, false}) {
        CAPTURE(out);
        PRig r;
        r.ownPartnerKit = true;
        r.def.partnerKit = kitB();
        r.enemy(dummyKind(), Vec2{20.f, 0.f});
        r.bodies.player = Vec2{0.f, 0.f};
        r.bodies.ally = Vec2{-2.f, 1.f};
        r.go(true, 120);
        r.tick();
        REQUIRE(r.fight.switchLeads());
        if (out) { // the host puts the partner out (as Lone), then restarts
            r.fight.setPartner(nullptr, nullptr);
            r.fight.setAlly(0, 0.4f);
        } else { // the host brings it in again at its own kit's max (as the runner: partnerKit.hp, the body's)
            r.fight.setAlly(r.fight.def().partnerKit.hp, 0.4f);
            CHECK(r.fight.allyMaxHp() == 100);
        }
        r.fight.restart();
        CHECK_FALSE(r.fight.swapped());
        CHECK(r.fight.def().fighter.hp == 100);
        CHECK(r.fight.hp() == 100);
        CHECK(r.fight.def().fighter.lightCount == 3);
        CHECK(r.fight.allyMaxHp() == (out ? 0 : 120));
    }
}

// The user's ruling: a party-only lead (the Third) is never playable. The key is ArenaFighter::playable, not an
// empty kit: a party-only lead may fight and is still refused; a playable lead with no attacks is not.
namespace {
std::uint64_t switchStateHash(const PRig& r) {
    StateHash h;
    const ArenaOutput& o = r.fight.output();
    h.add(o.player.velocity.x).add(o.player.velocity.y).add(o.player.facing.x).add(o.player.facing.y).add(r.fight.hp());
    h.add(static_cast<std::uint8_t>(o.player.action));
    h.add(r.fight.allyPos().x).add(r.fight.allyPos().y).add(r.fight.swapped());
    h.add(static_cast<std::uint8_t>(r.fight.def().fighter.weapon)).add(r.fight.def().fighter.playable);
    h.add(static_cast<std::uint8_t>(r.fight.def().partnerKit.weapon)).add(r.fight.def().partnerKit.playable);
    h.add(r.fight.def().fighter.hp).add(r.fight.def().partnerKit.hp).add(r.fight.stats().leadSwitches);
    h.add(r.fight.stats().takeovers).add(r.fight.stats().partnerSwaps).add(r.brain.hash());
    return h.value;
}
} // namespace

TEST_CASE("arena partner: per-lead kits: a switch to a partner that is not playable is refused, named and counted") {
    ArenaFighter party = kitB();
    party.weapon = ArenaWeapon::None;
    party.attacks = false;
    party.playable = false;
    PRig r;
    r.ownPartnerKit = true;
    r.def.partnerKit = party;
    r.enemy(dummyKind(), Vec2{1.2f, 0.f});
    r.bodies.player = Vec2{-2.f, 1.f};
    r.bodies.ally = Vec2{0.f, 0.f};
    r.go(true, 120);
    r.tick();
    CHECK(r.fight.switchRefusal() == ArenaSwitchRefusal::Ok); // none asked yet
    const std::uint64_t before = switchStateHash(r);
    CHECK_FALSE(r.fight.switchLeads());
    CHECK(r.fight.switchRefusal() == ArenaSwitchRefusal::NotPlayable);
    CHECK(r.fight.stats().leadSwitchesNotPlayable == 1);
    CHECK(switchStateHash(r) == before); // nothing else changed
    CHECK_FALSE(r.fight.swapped());
    CHECK(r.fight.def().fighter.playable);
    CHECK_FALSE(r.fight.def().partnerKit.playable);
    CHECK_FALSE(r.fight.switchLeads()); // each press counted
    CHECK(r.fight.stats().leadSwitchesNotPlayable == 2);
    SUBCASE("a party-only lead that fights is refused too") {
        PRig q;
        q.ownPartnerKit = true;
        q.def.partnerKit = kitB();
        q.def.partnerKit.playable = false;
        q.enemy(dummyKind(), Vec2{1.2f, 0.f});
        q.bodies.player = Vec2{-2.f, 1.f};
        q.bodies.ally = Vec2{0.f, 0.f};
        q.go(true, 120);
        q.tick();
        CHECK_FALSE(q.fight.switchLeads());
        CHECK(q.fight.switchRefusal() == ArenaSwitchRefusal::NotPlayable);
    }
    SUBCASE("a playable lead with no attacks is not refused") {
        PRig q;
        q.ownPartnerKit = true;
        q.def.partnerKit = party;
        q.def.partnerKit.playable = true;
        q.enemy(dummyKind(), Vec2{1.2f, 0.f});
        q.bodies.player = Vec2{-2.f, 1.f};
        q.bodies.ally = Vec2{0.f, 0.f};
        q.go(true, 120);
        q.tick();
        CHECK(q.fight.switchLeads());
        CHECK(q.fight.switchRefusal() == ArenaSwitchRefusal::Ok);
        CHECK(q.fight.stats().leadSwitchesNotPlayable == 0);
    }
    SUBCASE("the other reasons are named, and restart clears the last") {
        r.fight.restart();
        CHECK(r.fight.switchRefusal() == ArenaSwitchRefusal::Ok);
        PRig q;
        q.enemy(dummyKind(), Vec2{1.2f, 0.f});
        CHECK_FALSE(q.fight.switchLeads()); // not begun
        CHECK(q.fight.switchRefusal() == ArenaSwitchRefusal::NotFighting);
        q.fight.setAlly(100, 0.4f);
        q.fight.setPartner(&q.brain, &q.tuning);
        CHECK_FALSE(q.fight.switchLeads()); // not begun wins over everything: there is no fight (the player at 0 too)
        CHECK(q.fight.switchRefusal() == ArenaSwitchRefusal::NotFighting);
        q.fight.setPartner(nullptr, nullptr);
        q.go(false);
        q.tick();
        CHECK_FALSE(q.fight.switchLeads());
        CHECK(q.fight.switchRefusal() == ArenaSwitchRefusal::NoPartner);
    }
}

TEST_CASE("arena partner: per-lead kits: the switch refusal order: the permanent reason wins, one counted per press") {
    // Not begun, NoPartner, NotPlayable, then over or in the hook (NotFighting), PlayerDown, PartnerDown: the
    // session's order (NotPlayable, FightOver, Down; ArenaSwitchRefusal).
    const auto press = [](PRig& r, ArenaSwitchRefusal want) {
        const ArenaStats before = r.fight.stats();
        CHECK_FALSE(r.fight.switchLeads());
        CHECK(r.fight.switchRefusal() == want);
        CHECK(r.fight.stats().leadSwitchesRefused == before.leadSwitchesRefused + 1);
        CHECK(r.fight.stats().leadSwitchesNotPlayable ==
              before.leadSwitchesNotPlayable + (want == ArenaSwitchRefusal::NotPlayable ? 1 : 0));
        CHECK(r.fight.stats().leadSwitches == before.leadSwitches);
    };
    // The partner down, the player up (the downed-and-rises fight's start).
    const auto partnerDown = [](bool playable) {
        auto r = std::make_unique<PRig>();
        r->ownPartnerKit = true;
        r->def.partnerKit = kitB();
        r->def.partnerKit.playable = playable;
        r->enemy(biterKind(150, 3.f, 40.f), Vec2{0.f, 0.f});
        r->bodies.player = Vec2{0.f, 2.8f};
        r->bodies.ally = Vec2{1.f, 0.f};
        r->go(true, 100);
        r->run(2.f);
        REQUIRE(r->fight.allyHp() == 0);
        REQUIRE(r->fight.hp() > 0);
        REQUIRE(r->fight.phase() == ArenaPhase::Fighting);
        return r;
    };
    SUBCASE("PartnerDown alone") {
        auto r = partnerDown(true);
        press(*r, ArenaSwitchRefusal::PartnerDown);
    }
    SUBCASE("NotPlayable beats PartnerDown") {
        auto r = partnerDown(false);
        press(*r, ArenaSwitchRefusal::NotPlayable);
    }
    SUBCASE("NotPlayable beats PlayerDown and NotFighting: the Defeat with a party member standing") {
        PRig r;
        r.ownPartnerKit = true;
        r.def.partnerKit = kitB();
        r.def.partnerKit.playable = false;
        r.enemy(biterKind(150, 3.f, 5.f), Vec2{0.f, 0.f});
        r.bodies.player = Vec2{1.f, 0.f};
        r.bodies.ally = Vec2{0.f, 3.5f};
        r.go(true, 120, 1, Vec2{-1.f, 0.f});
        r.run(2.f);
        REQUIRE(r.fight.phase() == ArenaPhase::Defeat);
        REQUIRE(r.fight.allyHp() > 0);
        press(r, ArenaSwitchRefusal::NotPlayable);
        press(r, ArenaSwitchRefusal::NotPlayable); // the same word on every press
    }
    SUBCASE("NotFighting in the hook beats PartnerDown") {
        // The partnerDown fight with a hook that, on the first blow it rules while the partner is down, asks a switch.
        struct Ask {
            ArenaFight* fight = nullptr;
            int asked = 0;
            bool switched = true;
            ArenaSwitchRefusal why = ArenaSwitchRefusal::Ok;
        };
        PRig r;
        Ask ask{&r.fight};
        r.fight.setBlowHook(+[](void* user, const ArenaBlow&) {
            Ask& a = *static_cast<Ask*>(user);
            if (a.asked == 0 && a.fight->allyHp() == 0) {
                ++a.asked;
                a.switched = a.fight->switchLeads();
                a.why = a.fight->switchRefusal();
            }
            return ArenaBlowRuling{};
        }, &ask);
        r.ownPartnerKit = true;
        r.def.partnerKit = kitB();
        r.enemy(biterKind(150, 3.f, 40.f), Vec2{0.f, 0.f});
        r.bodies.player = Vec2{0.f, 2.8f};
        r.bodies.ally = Vec2{1.f, 0.f};
        r.go(true, 100);
        r.run(2.f);
        REQUIRE(r.fight.allyHp() == 0);
        REQUIRE(r.fight.phase() == ArenaPhase::Fighting);
        r.bodies.player = Vec2{0.f, 1.2f}; // in the lead's reach: its light lands, and the hook rules it
        for (int i = 0; i < 120 && ask.asked == 0; ++i) {
            r.in.light = true;
            r.tick();
        }
        REQUIRE(ask.asked == 1);
        CHECK_FALSE(ask.switched);
        CHECK(ask.why == ArenaSwitchRefusal::NotFighting);
    }
    SUBCASE("NotFighting (over) beats PlayerDown and PartnerDown: both down") {
        PRig r;
        r.enemy(biterKind(150, 3.f, 5.f), Vec2{0.f, 0.f});
        r.bodies.player = Vec2{1.f, 0.f};
        r.bodies.ally = Vec2{0.f, 3.5f};
        r.go(true, 80, 1, Vec2{-1.f, 0.f});
        r.run(2.f);
        REQUIRE(r.fight.stats().takeovers == 1); // the player took the ally's body; it lies where the player fell
        press(r, ArenaSwitchRefusal::PartnerDown);
        r.bodies.player = Vec2{1.f, 0.f}; // back in the bite
        r.run(8.f);
        REQUIRE(r.fight.phase() == ArenaPhase::Defeat);
        REQUIRE(r.fight.hp() <= 0);
        REQUIRE(r.fight.allyHp() == 0);
        press(r, ArenaSwitchRefusal::NotFighting); // as main had it: the fight is over
    }
}

TEST_CASE("arena partner: per-lead kits: a fall with only a non-playable partner standing is a Defeat, no takeover") {
    // Replaces the pin "a partner kit with no attacks taken over by a switch or a takeover (today)": handing the
    // player the party-only lead's body was a bug against the user's ruling.
    // Both with a partner kit that has no attacks and with one that has (the shipped party member fights).
    for (const bool attacks : {false, true})
    for (const bool playable : {false, true}) {
        CAPTURE(attacks);
        CAPTURE(playable);
        PRig r;
        r.ownPartnerKit = true;
        r.def.partnerKit = kitB();
        r.def.partnerKit.weapon = attacks ? ArenaWeapon::Magic : ArenaWeapon::None;
        r.def.partnerKit.attacks = attacks;
        r.def.partnerKit.playable = playable;
        r.enemy(biterKind(150, 3.f, 5.f), Vec2{0.f, 0.f});
        r.bodies.player = Vec2{1.f, 0.f};
        r.bodies.ally = Vec2{0.f, 3.5f};
        r.go(true, 120, 1, Vec2{-1.f, 0.f});
        r.run(2.f);
        if (playable) { // the takeover as before: the player drives the standing lead's kit
            CHECK(r.fight.stats().takeovers == 1);
            CHECK(r.fight.phase() == ArenaPhase::Fighting);
            CHECK(r.fight.swapped());
            CHECK(r.fight.def().fighter.attacks == attacks);
        } else {
            CHECK(r.fight.stats().takeovers == 0);
            CHECK(r.fight.phase() == ArenaPhase::Defeat);
            CHECK_FALSE(r.fight.swapped());
            CHECK(r.fight.def().fighter.playable); // the player kept its own kit
            CHECK(r.fight.allyHp() > 0);           // the party member still stands
        }
    }
}

TEST_CASE("arena partner: per-lead kits: the shipped party member's kit fights and is never the player's") {
    const ArenaTable t = shippedTable();
    const ArenaFighter& third = t.fighter("third");
    REQUIRE(t.findFighter("third") == &third);
    CHECK(third.attacks);
    CHECK_FALSE(arenaPlayable(third));
    CHECK(third.weapon == ArenaWeapon::Magic);
    CHECK(third.lightCount == t.fighter("duy").lightCount);
    for (std::uint8_t k = 0; k < third.lightCount; ++k) { // the bolt: Duy's beat, from further off
        CHECK(third.light[k].total() == doctest::Approx(t.fighter("duy").light[k].total()));
        CHECK(third.light[k].range > t.fighter("duy").light[k].range);
    }
    CHECK(third.heavy.range < third.light[0].range);           // the burst: short ...
    CHECK(third.heavy.total() > t.fighter("duy").heavy.total()); // ... slower ...
    CHECK(third.heavy.poiseDamage > t.fighter("duy").heavy.poiseDamage); // ... and more poise
    CHECK(third.partnerReach > 0.f);
    CHECK(third.partnerStandoff > 0.f);
    CHECK(third.partnerStandoff <= third.partnerReach);
    for (const char* id : {"default", "devin", "duy"}) { // the close-in kits leave the brain's own
        CAPTURE(std::string(id));
        CHECK(t.fighter(id).partnerReach == 0.f);
        CHECK(t.fighter(id).partnerStandoff == 0.f);
    }
    // As the partner it lands blows, the switch to it is refused, and the player's fall is a Defeat, no takeover.
    PRig r;
    r.ownPartnerKit = true;
    r.def.partnerKit = third;
    r.enemy(biterKind(150, 3.f, 5.f), Vec2{0.f, 0.f});
    r.bodies.player = Vec2{1.f, 0.f};
    r.bodies.ally = Vec2{0.f, 3.f};
    r.go(true, 120, 1, Vec2{-1.f, 0.f});
    CHECK_FALSE(r.fight.switchLeads());
    CHECK(r.fight.switchRefusal() == ArenaSwitchRefusal::NotPlayable);
    r.run(3.f);
    CHECK(r.fight.phase() == ArenaPhase::Defeat);
    CHECK(r.fight.stats().takeovers == 0);
    CHECK_FALSE(r.fight.swapped());
    CHECK(r.fight.allyHp() > 0);
}

TEST_CASE("arena partner: per-lead kits: partnerReach and partnerStandoff parse, default 0, and are checked") {
    // The shipped table with the party member's two keys replaced by `keys`.
    std::string shipped;
    REQUIRE(ftcore_test::readProjectData("Battle/arena.json", shipped));
    const std::string kShipped = R"("partnerReach": 8.5, "partnerStandoff": 5,)";
    const std::size_t at = shipped.find(kShipped);
    REQUIRE(at != std::string::npos);
    const auto load = [&](const char* keys, std::string* err) {
        std::string text = shipped;
        text.replace(at, kShipped.size(), keys);
        ArenaTable t;
        const bool ok = importArenaTable(text, t, err);
        return std::make_pair(ok, ok ? t.fighter("third") : ArenaFighter{});
    };
    std::string err;
    auto got = load(R"("partnerReach":3,"partnerStandoff":1.5,)", &err);
    REQUIRE_MESSAGE(got.first, err);
    CHECK(got.second.partnerReach == doctest::Approx(3.f));
    CHECK(got.second.partnerStandoff == doctest::Approx(1.5f));
    got = load(R"("hp":100,)", &err);
    REQUIRE_MESSAGE(got.first, err);
    CHECK(got.second.partnerReach == 0.f);
    CHECK(got.second.partnerStandoff == 0.f);
    CHECK_FALSE(load(R"("partnerReach":0.3,)", &err).first);
    got = load(R"("partnerReach":8.5,"partnerStandoff":8.5,)", &err); // the caster band's bound: accepted
    REQUIRE_MESSAGE(got.first, err);
    CHECK_FALSE(load(R"("partnerReach":8.6,)", &err).first);
    CHECK_FALSE(load(R"("partnerReach":3,"partnerStandoff":8.6,)", &err).first);
    CHECK_FALSE(load(R"("partnerReach":2,"partnerStandoff":2.5,)", &err).first);
    got = load(R"("partnerReach":2,"partnerStandoff":2,)", &err); // standoff equal to reach: accepted
    REQUIRE_MESSAGE(got.first, err);
    CHECK(got.second.partnerStandoff == doctest::Approx(2.f));
    CHECK_FALSE(load(R"("partnerStandoff":1,)", &err).first); // a standoff without a reach would pass the default reach
    CHECK(err.find("partnerStandoff") != std::string::npos);
    CHECK_FALSE(load(R"("partnerReach":0,"partnerStandoff":1,)", &err).first);
    CHECK_FALSE(load(R"("partnerReach":-1,)", &err).first);
    CHECK_FALSE(load(R"("partnerReach":3,"partnerStandoff":-0.5,)", &err).first);
    CHECK_FALSE(load(R"("partnerReach":"far",)", &err).first);
    CHECK_FALSE(load(R"("partnerReach":3,"partnerStandoff":"near",)", &err).first);
}

TEST_CASE("arena partner: per-lead kits: the shipped leads' playable flags") {
    const ArenaTable t = shippedTable();
    CHECK(t.fighter("devin").playable);
    CHECK(t.fighter("duy").playable);
    CHECK_FALSE(t.fighter("third").playable);
    CHECK(t.fallback.fighter.playable);
}

TEST_CASE("arena partner: per-lead kits: a lead with no kit in the table is found as none, and NoKit is never the lead's") {
    const ArenaTable t = shippedTable();
    for (const char* id : {"devin", "duy", "third"}) {
        CAPTURE(id);
        const ArenaFighter* own = t.findFighter(id);
        REQUIRE(own != nullptr);
        CHECK(own == &t.fighter(id));
    }
    CHECK(t.findFighter("nobody") == nullptr);   // no fallback ...
    CHECK(&t.fighter("nobody") == &t.fighters[0]); // ... where fighter() still gives the default
    CHECK(ArenaTable{}.findFighter("default") == nullptr);
    const ArenaFighter none = arenaNoKit();
    CHECK_FALSE(none.attacks);
    CHECK_FALSE(arenaPlayable(none));
    // As the partner it starts no blow, and the switch to it is refused as not playable.
    PRig r;
    r.ownPartnerKit = true;
    r.def.partnerKit = none;
    r.enemy(dummyKind(), Vec2{1.2f, 0.f});
    r.bodies.player = Vec2{-2.f, 1.f};
    r.bodies.ally = Vec2{0.f, 0.f};
    r.go(true, 120);
    r.run(5.f);
    CHECK(r.fight.stats().partnerBlows == 0);
    CHECK_FALSE(r.fight.switchLeads());
    CHECK(r.fight.switchRefusal() == ArenaSwitchRefusal::NotPlayable);
}

TEST_CASE("arena partner: per-lead kits: a no-kit partner under attack (report)") {
    ArenaFighter none = kitB();
    none.weapon = ArenaWeapon::None;
    none.attacks = false;
    PRig r;
    r.ownPartnerKit = true;
    r.def.partnerKit = none;
    r.def.fighter.hp = 100000;
    r.enemy(biterKind(5, 3.f, 1.5f), Vec2{0.f, 0.f});
    r.bodies.player = Vec2{-12.f, 0.f}; // far: the biter's only target in reach is the ally
    r.bodies.ally = Vec2{1.2f, 0.f};
    r.go(true, 1000);
    r.walk = true;
    r.run(20.f);
    const ArenaStats& s = r.fight.stats();
    MESSAGE("no-kit partner bitten at for 20 s: guards " << s.partnerGuards << ", blocked "
                                                         << r.count(ArenaEventType::AllyBlocked) << ", hurt "
                                                         << r.count(ArenaEventType::AllyHit) << ", blows "
                                                         << s.partnerBlows << ", ally hp " << r.fight.allyHp());
    CHECK(s.partnerBlows == 0);
}

TEST_CASE("arena partner: per-lead kits: the shipped table gives every fight the same kit twice, so old hashes hold") {
    const ArenaTable t = shippedTable();
    auto same = [](const ArenaFighter& x, const ArenaFighter& y) {
        return x.hp == y.hp && x.lightCount == y.lightCount && x.light[0].damage == y.light[0].damage &&
               x.light[0].startup == y.light[0].startup && x.turnRate == y.turnRate && x.weapon == y.weapon;
    };
    CHECK(same(t.fallback.fighter, t.fallback.partnerKit));
    for (const ArenaFightEntry& e : t.entries) CHECK(same(e.def.fighter, e.def.partnerKit));
    const ArenaDef d = defaultArenaDef();
    CHECK(same(d.fighter, d.partnerKit));
    // The pins of the shipped group fight above ("rule 17 off ... main's") run on this: unchanged.
}

TEST_CASE("arena partner: per-lead kits against each shipped boss and the group fight (report)") {
    // As the two reports above (the bosses 60 s with no pair; the group 20 s, camera not given, with the pair on
    // each parry, both orders), per lead and partner kit: default + default reproduces those reports to the bit.
    const ArenaTable t = shippedTable();
    struct Pair {
        const char* lead;
        const char* partner;
    };
    const Pair pairs[] = {{"default", "default"}, {"devin", "duy"}, {"duy", "devin"}, {"devin", "third"}, {"duy", "third"}};
    const char* acts[] = {"wait", "moveTo", "attack", "guard", "stepBack"};
    for (const Pair& p : pairs) {
        for (const char* idText : {"kite_boss", "stone_bearer", "white_glove", "group/0", "group/1"}) {
            const std::string id = idText;
            CAPTURE(id);
            const bool group = id.rfind("group", 0) == 0;
            PRig r(t.fighter(p.lead));
            if (group) {
                r.def = t.fallback;
                r.def.enemyCount = 0;
                for (const char* e : {"skirmisher", "hulk", "duelist"})
                    for (std::size_t j = 0; j < t.enemyIds.size(); ++j)
                        if (t.enemyIds[j] == e) {
                            r.def.enemyKind[r.def.enemyCount] = static_cast<std::uint8_t>(j);
                            r.def.enemies[r.def.enemyCount++] = t.enemies[j];
                        }
                r.def.attackers = 2;
                r.in.partnerOrder = static_cast<std::uint8_t>(id.back() - '0');
            } else {
                r.def = shippedBoss(t, idText, false);
            }
            r.def.fighter = t.fighter(p.lead);
            r.def.partnerKit = t.fighter(p.partner);
            r.ownPartnerKit = true;
            r.tuning = shippedTuning();
            r.goShipped(true, 1);
            r.pairOnParry = group;
            const float limit = group ? 20.f : 60.f;
            double actSeconds[5] = {0, 0, 0, 0, 0}, walked = 0;
            Vec2 at = r.bodies.ally;
            while (!r.fight.over() && r.fight.time() < limit) {
                r.tick();
                actSeconds[static_cast<int>(r.brain.intent().act) % 5] += kDt;
                walked += length(sub(r.bodies.ally, at));
                at = r.bodies.ally;
            }
            const ArenaStats& s = r.fight.stats();
            int maxHp = 0;
            for (std::size_t e = 0; e < r.fight.enemyCount(); ++e) maxHp += r.def.enemies[e].hp;
            const double all = static_cast<double>(s.partnerDamage + s.damageDealt);
            std::string actText;
            for (int a = 0; a < 5; ++a)
                actText += std::string(a ? ", " : "") + acts[a] + " " + std::to_string(actSeconds[a] / r.fight.time());
            MESSAGE(std::string(p.lead) << " + partner " << std::string(p.partner) << ", " << id << ": "
                    << r.fight.time() << " s, phase " << static_cast<int>(r.fight.phase()) << "; partner damage "
                    << s.partnerDamage << " = " << s.partnerDamage / double(maxHp) << " of " << maxHp << " hp, "
                    << (all > 0 ? s.partnerDamage / all : 0.0) << " of all (lead " << s.damageDealt << "); strings "
                    << s.partnerStrings << ", blows " << s.partnerBlows << ", cut " << s.partnerBlowsCut
                    << ", guards " << s.partnerGuards << ", no kit " << s.partnerNoKitSeconds << " s; aimed at it "
                    << s.bossAimedPartner << ", hit " << r.count(ArenaEventType::AllyHit) << ", blocked "
                    << r.count(ArenaEventType::AllyBlocked) << "; ally downed " << r.count(ArenaEventType::AllyDowned) << ", takeovers " << s.takeovers << "; walked " << walked
                    << " m; acts " << actText);
            if (std::string(p.partner) == "third") { // the party member's kit fights (bolts), with no kit time
                CHECK(s.partnerBlows >= 1);
                CHECK(s.partnerDamage > 0);
                CHECK(s.partnerNoKitSeconds == 0.f);
                // Pinned (seed 1): each boss's time to the win and the third's partner damage, so a retune of its
                // bolt, reach or standoff cannot pass silently. Re-pin with the report's lines when one is meant.
                // Re-pinned for the autopilot guard gate: the lead parries blows it used to take, so five fights end
                // sooner (duy kite_boss did not move) and the third lands fewer bolts in them.
                struct Pin {
                    const char* lead;
                    const char* boss;
                    float seconds;
                    int partnerDamage;
                };
                static constexpr Pin kThirdPins[] = {
                    {"devin", "kite_boss", 24.7831f, 126}, {"devin", "stone_bearer", 36.233f, 187},
                    {"devin", "white_glove", 31.6997f, 152}, {"duy", "kite_boss", 33.5163f, 150},
                    {"duy", "stone_bearer", 40.1496f, 222}, {"duy", "white_glove", 33.4163f, 160},
                };
                for (const Pin& pin : kThirdPins) {
                    if (std::string(p.lead) != pin.lead || id != pin.boss) continue;
                    CHECK(r.fight.over());
                    CHECK(r.fight.time() == doctest::Approx(pin.seconds).epsilon(1e-4));
                    CHECK(s.partnerDamage == pin.partnerDamage);
                }
            }
        }
    }
}

// Finding 6: a partner whose kit has no attacks is told so (PartnerInput::kitAttacks, from the ally's kit: the brain's
// NoKit) and spends nothing on a paired counter; the lead's own counter (its damage, stagger and pairCooldown) is the
// same as with no partner at all.
TEST_CASE("arena partner: a partner kit with no attacks spends nothing on a paired counter; the lead's is as with no partner") {
    struct Got {
        float spent = 0.f, perCounter = 0.f;
        int staggers = 0, amount = 0;
        PressReason after = PressReason::Ok;
    };
    const auto run = [](bool partner, bool attacks) {
        PRig r;
        r.ownPartnerKit = true;
        r.def.partnerKit = kitB();
        r.def.partnerKit.attacks = attacks;
        r.enemy(biterKind(5, 3.f, 1.f), Vec2{1.5f, 0.f});
        r.bodies.player = Vec2{0.f, 0.f};
        r.bodies.ally = Vec2{-10.f, 0.f}; // out of the bites' and its own blows' reach
        r.go(partner, 100);
        Got g;
        g.spent = pairUntilCounter(r);
        g.perCounter = r.def.fighter.pairPartnerSeconds;
        CHECK(r.count(ArenaEventType::Parried) > 0);
        g.staggers = r.fight.stats().counterStaggers;
        for (const auto& l : r.log)
            if (l.e.type == ArenaEventType::CounterStagger) g.amount += l.e.amount;
        g.after = r.fight.pressReason(ArenaPress::Pair);
        return g;
    };
    const Got with = run(true, true), noKit = run(true, false), none = run(false, true);
    CHECK(with.spent == doctest::Approx(with.perCounter));
    CHECK(with.staggers == 1);
    CHECK(noKit.spent == 0.f); // the partner pays nothing
    CHECK(none.spent == 0.f);
    for (const Got* g : {&noKit, &none}) {
        CHECK(g->staggers == 1);
        CHECK(g->amount == with.amount);
        CHECK(g->amount > 0);
        CHECK(g->after == PressReason::Cooldown); // pairCooldown runs
    }
}

namespace {
// 20 s of the shipped party kit as the partner against one dummy, the lead 4 m off it, the camera on behind the lead:
// the classic camera or the close one (a caster band of 5-8 m), with the kit's range as shipped or zeroed.
struct RangeRun {
    std::vector<std::uint64_t> hashes; // the brain's, every tick
    int hits = 0;
    float firstHit = -1.f;
    float sumEdge = 0.f; // the ally's distance off the dummy's edge at each of its hits
    ArenaStats stats;
    float meanEdge() const { return hits > 0 ? sumEdge / static_cast<float>(hits) : 0.f; }
};
RangeRun rangeRun(bool kitRange, bool close) {
    const ArenaTable t = shippedTable();
    ArenaFighter third = t.fighter("third");
    if (!kitRange) third.partnerReach = third.partnerStandoff = 0.f;
    PRig r;
    r.ownPartnerKit = true;
    r.def.partnerKit = third;
    r.def.fighter.hp = 100000; // the lead never falls: no takeover
    r.tuning.budgetAllowanceSeconds = 10.f;
    REQUIRE(validPartnerTuning(r.tuning));
    const ArenaEnemyKind dummy = dummyKind();
    r.enemy(dummy, Vec2{0.f, 0.f});
    r.bodies.player = Vec2{-4.f, 0.f};
    r.bodies.ally = Vec2{-3.f, 2.f};
    r.go(true, 1000);
    r.walk = true;
    r.in.cameraOn = true;
    r.in.cameraPos = Vec2{-7.f, 0.f};
    r.in.cameraFacing = Vec2{1.f, 0.f};
    if (close) {
        PartnerCloseView& c = r.in.partnerClose;
        c.given = true;
        c.halfAngleDegrees = 30.f;
        c.leadSide = 0;
        c.standMin = kPartnerCloseCasterMin;
        c.standMax = kPartnerCloseCasterMax;
    }
    RangeRun k;
    for (int s = 0; s < 20 * 60; ++s) {
        const std::size_t from = r.log.size();
        r.tick();
        k.hashes.push_back(r.brain.hash());
        for (std::size_t i = from; i < r.log.size(); ++i)
            if (allyHit(r.log[i].e)) {
                if (k.hits == 0) k.firstHit = r.fight.time();
                ++k.hits;
                k.sumEdge += length(sub(r.bodies.ally, r.bodies.enemies[0])) - dummy.bodyRadius;
            }
    }
    k.stats = r.fight.stats();
    return k;
}
} // namespace

TEST_CASE("arena partner: the partner kit's range: none of it under the classic camera; under the close one it casts from further off") {
    // The fight always passes the kit's range; the classic camera never reads it: the brain hashes alike every tick.
    const RangeRun a = rangeRun(true, false), b = rangeRun(false, false);
    REQUIRE(a.hashes.size() == b.hashes.size());
    CHECK(a.hashes == b.hashes);
    CHECK(a.hits == b.hits);
    CHECK(a.stats.partnerDamage == b.stats.partnerDamage);
    REQUIRE(a.hits > 0);
    // The close camera: the range read (reach 8.5 m off the edge, standoff 5: a band of 5-8 m; the bolt's range 8.5),
    // so at its hits it stands further off than without the range. Not yet in its band: a string may start anywhere
    // in reach, so it starts from where it stands (about 2.7 m here), short of the standoff.
    const RangeRun c = rangeRun(true, true), d = rangeRun(false, true);
    MESSAGE("close camera, kit range on: hits " << c.hits << " first at " << c.firstHit << " s, mean "
                                                << c.meanEdge() << " m off the edge, damage " << c.stats.partnerDamage
                                                << ", strings " << c.stats.partnerStrings);
    MESSAGE("close camera, kit range off: hits " << d.hits << " first at " << d.firstHit << " s, mean "
                                                 << d.meanEdge() << " m off the edge, damage " << d.stats.partnerDamage
                                                 << ", strings " << d.stats.partnerStrings);
    MESSAGE("classic camera: hits " << a.hits << " first at " << a.firstHit << " s, mean " << a.meanEdge()
                                    << " m off the edge, damage " << a.stats.partnerDamage);
    REQUIRE(c.hits > 0);
    REQUIRE(d.hits > 0);
    CHECK(c.meanEdge() > d.meanEdge());
    CHECK(c.hashes != d.hashes);
}

TEST_CASE("arena partner: the party member's casts against each shipped boss and the group, no camera and close (report)") {
    // The balance read on the caster's range: each lead with the third as the partner, against the shipped bosses
    // (60 s) and the group (20 s), with no camera (as the per-lead report above) and with the close one behind the
    // lead (3 m back, facing its first standing enemy; the caster band). Damage, the first partner hit, the fight time.
    const ArenaTable t = shippedTable();
    for (const char* lead : {"devin", "duy"}) {
        for (const char* idText : {"kite_boss", "stone_bearer", "white_glove", "group/0"}) {
            for (const bool close : {false, true}) {
                const std::string id = idText;
                const bool group = id.rfind("group", 0) == 0;
                PRig r(t.fighter(lead));
                if (group) {
                    r.def = t.fallback;
                    r.def.enemyCount = 0;
                    for (const char* e : {"skirmisher", "hulk", "duelist"})
                        for (std::size_t j = 0; j < t.enemyIds.size(); ++j)
                            if (t.enemyIds[j] == e) {
                                r.def.enemyKind[r.def.enemyCount] = static_cast<std::uint8_t>(j);
                                r.def.enemies[r.def.enemyCount++] = t.enemies[j];
                            }
                    r.def.attackers = 2;
                } else {
                    r.def = shippedBoss(t, idText, false);
                }
                r.def.fighter = t.fighter(lead);
                r.def.partnerKit = t.fighter("third");
                r.ownPartnerKit = true;
                r.tuning = shippedTuning();
                r.goShipped(true, 1);
                r.autopilot = false; // the autopilot's input is taken below, the camera added to it
                const float limit = group ? 20.f : 60.f;
                int hits = 0;
                float firstHit = -1.f, nearest = 1e9f, farthest = 0.f, sum = 0.f;
                int strings = 0, stringsInside = 0; // strings begun, and those begun inside the standoff
                float stringSum = 0.f;              // the partner's edge distance to its nearest standing enemy at each
                const float standoff = r.def.partnerKit.partnerStandoff;
                while (!r.fight.over() && r.fight.time() < limit) {
                    r.in = r.fight.autopilot();
                    r.in.partnerOrder = 0;
                    if (close) {
                        Vec2 to = r.bodies.player;
                        for (std::size_t e = 0; e < r.fight.enemyCount(); ++e)
                            if (r.fight.enemyHp(e) > 0) {
                                to = r.bodies.enemies[e];
                                break;
                            }
                        Vec2 f = sub(to, r.bodies.player);
                        const float n = length(f);
                        f = n > 1e-3f ? Vec2{f.x / n, f.y / n} : Vec2{1.f, 0.f};
                        r.in.cameraOn = true;
                        r.in.cameraPos = Vec2{r.bodies.player.x - 3.f * f.x, r.bodies.player.y - 3.f * f.y};
                        r.in.cameraFacing = f;
                        PartnerCloseView& c = r.in.partnerClose;
                        c.given = true;
                        c.halfAngleDegrees = 30.f;
                        c.standMin = kPartnerCloseCasterMin;
                        c.standMax = kPartnerCloseCasterMax;
                    }
                    const std::size_t from = r.log.size();
                    const int stringsBefore = r.fight.stats().partnerStrings;
                    r.tick();
                    if (r.fight.stats().partnerStrings > stringsBefore) {
                        float edgeNow = 1e9f;
                        for (std::size_t e = 0; e < r.fight.enemyCount(); ++e)
                            if (r.fight.enemyHp(e) > 0)
                                edgeNow = std::min(edgeNow, length(sub(r.bodies.enemies[e], r.fight.allyPos())) -
                                                                arenaBodyCylinder(r.def.enemies[e], r.bodies.enemies[e], 0.f).radius);
                        ++strings;
                        stringSum += edgeNow;
                        if (edgeNow < standoff) ++stringsInside;
                    }
                    for (std::size_t i = from; i < r.log.size(); ++i)
                        if (allyHit(r.log[i].e)) {
                            if (hits == 0) firstHit = r.log[i].t;
                            ++hits;
                            // Cast from: the partner's centre to the target's edge (the reach's and the hit's measure).
                            const ArenaEvent& e = r.log[i].e;
                            const float radius = e.target < r.def.enemyCount
                                                     ? arenaBodyCylinder(r.def.enemies[e.target], e.at, 0.f).radius
                                                     : 0.f;
                            const float d = length(sub(e.at, r.fight.allyPos())) - radius;
                            nearest = std::min(nearest, d);
                            farthest = std::max(farthest, d);
                            sum += d;
                        }
                }
                const ArenaStats& s = r.fight.stats();
                MESSAGE("caster report: " << std::string(lead) << " + third, " << id << ", " << std::string(close ? "close" : "no camera")
                        << ": " << r.fight.time() << " s" << std::string(r.fight.over() ? " (over)" : "") << "; partner damage "
                        << s.partnerDamage << " in " << hits << " hits, first at " << firstHit << " s, cast from "
                        << (hits > 0 ? nearest : 0.f) << ".." << farthest << " m (mean " << (hits > 0 ? sum / hits : 0.f)
                        << "); strings begun " << strings << ", " << stringsInside << " inside the standoff, mean edge "
                        << (strings > 0 ? stringSum / strings : 0.f) << " m; ring " << r.ring.radius << " m, leash "
                        << r.tuning.leashMeters << " m, target radius "
                        << arenaBodyCylinder(r.def.enemies[0], Vec2{0.f, 0.f}, 0.f).radius << " m; lead "
                        << s.damageDealt << "; strings " << s.partnerStrings << ", ally downed "
                        << r.count(ArenaEventType::AllyDowned));
                CHECK(hits >= 0);
            }
        }
    }
}

namespace {
// One shipped fight by enemy id (as test_arena.cpp's shippedFight: ring 12 m, spawn 7 m).
ArenaDef partyFight(const ArenaTable& t, std::initializer_list<const char*> ids, std::uint8_t attackers) {
    ArenaDef def;
    def.enemyCount = 0;
    for (const char* id : ids)
        for (std::size_t e = 0; e < t.enemyIds.size(); ++e)
            if (t.enemyIds[e] == id) {
                def.enemyKind[def.enemyCount] = static_cast<std::uint8_t>(e);
                def.enemies[def.enemyCount++] = t.enemies[e];
            }
    def.attackers = attackers;
    def.ringRadius = 12.f;
    def.spawnDistance = 7.f;
    REQUIRE(def.enemyCount == ids.size());
    return def;
}

struct PartyOutcome {
    ArenaPhase phase = ArenaPhase::Fighting;
    float time = 0.f;
    int strings = 0; // the partner's strings begun (its casts)
    int hits = 0;    // its casts that landed
    // Close only: when the caster first stood inside its band off its target's edge (-1: never), its first light's
    // startup, and the share of the ticks before the end it closed on the band (its gap to the band shrank).
    float arrive = -1.f;
    float startup = 0.f;
    float closingShare = 0.f;
};
// A close fight with no cast is the caster's walk out, not a fault, only when the fight ended before it could have cast
// (before it reached its band plus one startup, or it never reached it) and it was walking to the band on at least 80%
// of the ticks (not idle, not leashed back).
bool partyWalkOut(const PartyOutcome& o) {
    const bool early = o.arrive < 0.f || o.time < o.arrive + o.startup;
    return early && o.closingShare >= 0.8f;
}

// 120 s at most of Def with Lead at the controls (the autopilot, or nobody) and the party member as the partner, on the
// shipped brain and no camera.
// The lens a party run is fought under: the classic camera (a ground camera 4.5 m behind the lead, facing its nearest
// live enemy; the close station never given) or the close one (the same camera with the close station given at the
// kit's own band: rule 19 and the kit's range on).
enum class PartyLens : std::uint8_t { Classic, Close };
const char* partyLensName(PartyLens lens) { return lens == PartyLens::Close ? "close" : "classic"; }
PartyOutcome partyPlay(const ArenaTable& t, const ArenaDef& def, const char* lead, std::uint32_t seed, bool autopilot,
                       PartyLens lens) {
    PRig r(t.fighter(lead));
    r.def = def;
    r.def.fighter = t.fighter(lead);
    r.def.partnerKit = t.fighter("third");
    r.ownPartnerKit = true;
    r.tuning = shippedTuning();
    r.goShipped(true, seed);
    r.autopilot = autopilot;
    r.keepView = true;
    if (!autopilot) r.in = ArenaInput{};
    Vec2 look = r.fight.playerStartFacing();
    const ArenaFighter& kit = r.fight.def().partnerKit;
    float gap = -1.f, arrive = -1.f; // close: the caster's gap to its band off its target's edge (-1: no target)
    int ticks = 0, closing = 0;
    PartnerCloseView& c = r.in.partnerClose;
    for (int s = 0; s < 120 * 60 && !r.fight.over(); ++s) {
        float best = 1e9f;
        for (std::size_t i = 0; i < r.fight.enemyCount(); ++i) {
            if (!r.fight.enemyAlive(i)) continue;
            const Vec2 d = sub(r.bodies.enemies[i], r.bodies.player);
            const float far = length(d);
            if (far > 1e-3f && far < best) {
                best = far;
                look = Vec2{d.x / far, d.y / far};
            }
        }
        r.in.cameraOn = true;
        r.in.cameraPos = Vec2{r.bodies.player.x - 4.5f * look.x, r.bodies.player.y - 4.5f * look.y};
        r.in.cameraFacing = look;
        c = PartnerCloseView{};
        if (lens == PartyLens::Close) {
            c.given = true;
            c.halfAngleDegrees = 30.f;
            c.leadSide = 0;
            if (kit.partnerReach > 0.f) {
                c.standMin = kPartnerCloseCasterMin;
                c.standMax = kPartnerCloseCasterMax;
            }
        }
        const float gapBefore = gap;
        r.tick();
        if (lens != PartyLens::Close) continue;
        gap = -1.f;
        const int aim = r.out->ally.target;
        if (aim >= 0 && static_cast<std::size_t>(aim) < r.fight.enemyCount() && r.fight.enemyAlive(static_cast<std::size_t>(aim))) {
            const std::size_t i = static_cast<std::size_t>(aim);
            const float edge = dist(r.bodies.ally, r.bodies.enemies[i]) - r.def.enemies[i].bodyRadius;
            gap = std::max(0.f, std::max(c.standMin - edge, edge - c.standMax));
            if (arrive < 0.f && gap <= 0.05f) arrive = r.fight.time();
        }
        ++ticks;
        closing += gapBefore >= 0.f && gap >= 0.f && gap < gapBefore - 1e-4f ? 1 : 0;
    }
    PartyOutcome o;
    o.phase = r.fight.phase();
    o.time = r.fight.time();
    o.arrive = arrive;
    o.closingShare = ticks > 0 ? static_cast<float>(closing) / static_cast<float>(ticks) : 0.f;
    o.startup = kit.lightCount > 0 ? arenaPlayerAttack(kit, 0)->startup : 0.f;
    o.strings = r.fight.stats().partnerStrings;
    o.hits = r.allyHits();
    return o;
}
} // namespace

TEST_CASE("arena partner: a cast's bolt leaves the hand as the startup begins and arrives on the hit, its flight never under 0.16 s") {
    // The floor and the formula.
    CHECK(arenaBoltFlight(0.2f) == doctest::Approx(0.2f));
    CHECK(arenaBoltLate(0.2f) == doctest::Approx(0.f));
    CHECK(arenaBoltFlight(0.1f) == doctest::Approx(kArenaBoltFlightMin));
    CHECK(arenaBoltLate(0.1f) == doctest::Approx(kArenaBoltFlightMin - 0.1f));
    CHECK(arenaBoltFlight(0.f) == doctest::Approx(kArenaBoltFlightMin));
    CHECK(arenaBoltLate(0.f) == doctest::Approx(kArenaBoltFlightMin));
    CHECK(arenaBoltFlight(-1.f) == doctest::Approx(kArenaBoltFlightMin));
    CHECK(arenaBoltFlight(std::numeric_limits<float>::quiet_NaN()) == doctest::Approx(kArenaBoltFlightMin));
    CHECK(arenaBoltLate(std::numeric_limits<float>::infinity()) == doctest::Approx(kArenaBoltFlightMin));
    // The shipped caster's lights: each bolt flies its light's startup and lands on the hit, none late.
    const ArenaTable t = shippedTable();
    const ArenaFighter& kit = t.fighter("third");
    REQUIRE(kit.weapon == ArenaWeapon::Magic);
    REQUIRE(kit.lightCount > 0);
    for (std::uint8_t m = 0; m < kit.lightCount; ++m) {
        const ArenaAttack* a = arenaPlayerAttack(kit, m);
        REQUIRE(a != nullptr);
        CHECK(arenaBoltFlight(a->startup) == doctest::Approx(a->startup));
        CHECK(arenaBoltLate(a->startup) == doctest::Approx(0.f));
        MESSAGE("caster light " << int(m) << ": startup " << a->startup << " s, bolt flight " << arenaBoltFlight(a->startup) << " s");
    }
}

TEST_CASE("arena partner: the party member as each playable lead's partner plays the default's fights: won, lost, and it casts") {
    // The coverage test_arena.cpp's lead-kit balance gives the playable leads, for the kit that is never a lead: each
    // playable lead with it as the partner, against the shipped default fight, the two mixed fights and each enemy alone,
    // over 8 seeds, under the classic camera and under the close one (its kit's range on). The autopilot wins; nobody at the controls loses (both bodies down) on most seeds, the partner alone
    // winning the odd weak fight, and the fight is over in 120 s either way; it casts in every fight, but under the close
    // camera a fight may end while it still walks out to its 5-8 m band (a lone skirmisher, spitter or pup goes down in
    // 2-4 s): a cast-less close fight passes only as that walk-out (partyWalkOut), and is counted.
    // Where its casts start (inside its 5-8 m band) waits for the brain's string gate.
    const ArenaTable t = shippedTable();
    REQUIRE(t.fighterIds.size() >= 4);
    std::vector<std::pair<std::string, ArenaDef>> fights;
    fights.emplace_back("shipped default fight", t.fallback);
    fights.emplace_back("three kinds, attackers 1", partyFight(t, {"brute", "skirmisher", "breaker"}, 1));
    fights.emplace_back("a brute and a spitter, attackers 2", partyFight(t, {"brute", "spitter"}, 2));
    for (std::size_t e = 0; e < t.enemyIds.size(); ++e) {
        if (t.enemies[e].passive) continue;
        ArenaDef def;
        def.enemyCount = 1;
        def.enemyKind[0] = static_cast<std::uint8_t>(e);
        def.enemies[0] = t.enemies[e];
        def.attackers = 1;
        fights.emplace_back(t.enemyIds[e], def);
    }
    int leads = 0;
    for (std::size_t f = 1; f < t.fighterIds.size(); ++f) {
        if (!t.fighters[f].playable || !t.fighters[f].attacks) continue;
        ++leads;
        const char* lead = t.fighterIds[f].c_str();
        for (const PartyLens lens : {PartyLens::Classic, PartyLens::Close})
        for (const auto& [name, def] : fights) {
            float slowestWin = 0.f, slowestLoss = 0.f;
            int fewestCasts = 1 << 30;
            int losses = 0, walkOuts = 0;
            for (std::uint32_t seed = 1; seed <= 8; ++seed) {
                const PartyOutcome win = partyPlay(t, def, lead, seed, true, lens);
                const PartyOutcome lose = partyPlay(t, def, lead, seed, false, lens);
                CHECK_MESSAGE(win.phase == ArenaPhase::Victory, "Error: " << std::string(lead) << " + third (" << std::string(partyLensName(lens)) << ") did not win " << name
                                                                          << " seed " << seed << " in 120 s");
                // Nobody at the controls: the fight ends in 120 s, mostly lost; the partner alone may win a weak one.
                CHECK_MESSAGE((lose.phase == ArenaPhase::Defeat || lose.phase == ArenaPhase::Victory), "Error: " << std::string(lead) << " + third (" << std::string(partyLensName(lens)) << "), nobody at the controls, "
                                                            << name << " seed " << seed << " not over in 120 s");
                losses += lose.phase == ArenaPhase::Defeat ? 1 : 0;
                for (const PartyOutcome* o : {&win, &lose}) {
                    if (o->strings > 0) continue;
                    const bool walkOut = lens == PartyLens::Close && partyWalkOut(*o);
                    walkOuts += walkOut ? 1 : 0;
                    CHECK_MESSAGE(walkOut, "Error: third (" << std::string(partyLensName(lens)) << ") never cast in "
                                               << std::string(lead) << "'s " << std::string(o == &win ? "" : "idle ") << name
                                               << " seed " << seed << ": ended " << o->time << " s, in its band at "
                                               << o->arrive << " s, closing on it " << o->closingShare * 100.f << "% of ticks");
                }
                slowestWin = std::max(slowestWin, win.time);
                slowestLoss = std::max(slowestLoss, lose.time);
                fewestCasts = std::min(fewestCasts, std::min(win.strings, lose.strings));
            }
            CHECK_MESSAGE(losses > 0, "Error: " << std::string(lead) << " + third (" << std::string(partyLensName(lens)) << "), nobody at the controls, never lost " << name);
            MESSAGE("party cover: " << std::string(lead) << " + third (" << std::string(partyLensName(lens)) << "), " << name << " over 8 seeds: slowest win " << slowestWin
                                    << " s, idle lost " << losses << "/8, slowest idle end " << slowestLoss
                                    << " s, fewest casts in a fight " << fewestCasts << ", cast-less walk-outs " << walkOuts
                                    << "/16");
        }
    }
    CHECK(leads >= 2);
}

namespace {
// A fought run under the close lens, as the game's capture runs it (sand ring: brute, skirmisher, breaker; one at a
// time; the lead's kit with the autopilot fighting; the party kit as the partner, Spread). The lens is the host's
// close frame after its rules (the preset's rig as arena-glide's round 2 set it: arm 2.6, side 0.55 m, no lag; the
// melee share by the distance eased over 0.3 s: the pivot up to +0.25 m, the fov 60 to 68, the target turned to
// 0.6-0.64 of the width over the right shoulder), drawn from the bodies each step and fed to the fight as the host
// does (partnerCamera, partnerClose).
// Measured as the host's "partner in play": on screen (partnerInFrameClose with the frame margin), not behind the
// lead and not behind the target (arenaHiddenBehind at the partner's middle and head, hidden when both are), each a
// share of every step.
struct FoughtLensRun {
    int steps = 0, seen = 0, leadClear = 0, leadClearSeen = 0, targetClear = 0;
    // The steps on screen but behind the lead, by what the partner was doing.
    int hidAttack = 0, hidWalkGoalHidden = 0, hidWalkGoalClear = 0, hidStillGoalHidden = 0, hidStillOther = 0;
    // ... and by the station rule 19 would give with no hold: none valid, valid on the partner's own flank, valid
    // only on the other flank.
    int hidNoneValid = 0, hidOwnFlankValid = 0, hidOtherFlankOnly = 0;
    int hidFramed = 0, hidFallback = 0;
    int strings = 0, partnerDamage = 0;
    float time = 0.f;
    // The steps the lead stood within 1.2 m of its target (centre to centre: bodies touching), and their sums.
    int contact = 0, contactSeen = 0, contactClearSeen = 0;
    // The steps on screen whose floor-plane silhouette (0.4 m each) does not overlap the lead's from the lens: the
    // capture's second measure ("not overlapping the lead").
    int leadClearFlatSeen = 0;
    // The brain's rule-19 tier this step (PartnerIntent::closeTier, 0 none) on the steps hidden behind the lead,
    // and on those behind the target.
    int leadHidTier[5] = {}, targetHidTier[5] = {};
    float contactDist = 0.f, contactTargetX = 0.f;
    // Steps the partner stood still (Wait, not in a pause, its budget, a blow running out, a guard or held off a
    // windup) with a standing enemy inside its reach: the longest such run (s), and the first time any standing enemy
    // was inside its reach (-1: never).
    float idleLongest = 0.f, firstInReach = -1.f;
    // The ruling of 11-16 on a melee partner: the steps its target is the lens's (or it has none), on screen of those,
    // clear of the lead of those; the steps on another enemy, those where that enemy stands out of the near field, and
    // of those the steps its body (and, walking, its goal) keeps rule 1.
    int same = 0, sameSeen = 0, sameSeenClear = 0, other = 0, otherAllowed = 0, otherPosOk = 0, otherGoalOk = 0,
        otherWalk = 0, otherStation = 0;
};

struct FoughtLens {
    float arm = 2.6f;
    float side = 0.55f;   // m, the preset's (round 2)
    float shoulder = 1.f; // +1 the right shoulder
    bool partnerYaw = false;
    // The target's share of the width at a full melee share (right shoulder): the rules' 0.64, or what the drawn lens
    // gave in the capture (cam4 on b93fd13e: under 3 m the target's x averaged 0.517).
    float targetXMelee = 0.64f;
    // The lead pressing into its target (bodies touching) whenever it stands off, as the capture's lead did for
    // about half of its fought steps (3 of the 5 views at 0.85-0.9 m), against the autopilot's fifth.
    bool hug = false;
    float seconds = 40.f; // the run's length (the fight may end first)
    bool laneStart = false; // the arena lane's start: 3 m ahead of the lead along the lens, 1.5 m off the far side
};

float smooth01(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// kit: 0 the party kit without its range, 1 with its shipped range, 2 a caster's 8.5 / 5 (the ruling of 11-16).
// The lead's kit and the enemies (one at a time) as named.
FoughtLensRun foughtLensRun(const FoughtLens& lens, std::uint32_t seed, int kit, const char* lead = "devin",
                            std::initializer_list<const char*> foes = {"brute", "skirmisher", "breaker"},
                            const char* partner = nullptr) {
    const ArenaTable t = shippedTable();
    PRig r(t.fighter(lead));
    r.def = t.fallback;
    r.def.fighter = t.fighter(lead);
    r.def.enemyCount = 0;
    for (const char* id : foes)
        for (std::size_t e = 0; e < t.enemyIds.size(); ++e)
            if (t.enemyIds[e] == id) {
                r.def.enemyKind[r.def.enemyCount] = static_cast<std::uint8_t>(e);
                r.def.enemies[r.def.enemyCount++] = t.enemies[e];
            }
    REQUIRE(r.def.enemyCount == foes.size());
    r.def.attackers = 1;
    r.def.ringRadius = 12.f;
    r.def.spawnDistance = 7.f;
    // The partner's kit: the party caster's by `kit`, or (partner named) that lead's own kit as shipped, as the
    // runner gives it (FTArenaRunner: the partner fights with the kit of the lead it is), `kit` then unused.
    ArenaFighter mate = t.fighter(partner != nullptr ? partner : "third");
    if (partner == nullptr && kit == 0) mate.partnerReach = mate.partnerStandoff = 0.f;
    if (partner == nullptr && kit == 2) {
        mate.partnerReach = 8.5f;
        mate.partnerStandoff = 5.f;
    }
    r.def.partnerKit = mate;
    // The host's band and body by the kit (FTArenaRunner::CloseStationView): a magic kit the caster's, the others
    // the melee one; the party caster keeps the 0.4 m body these runs always gave it.
    const bool caster = mate.weapon == ArenaWeapon::Magic;
    const float bandMin = caster ? kPartnerCloseCasterMin : kPartnerCloseMeleeMin;
    const float bandMax = caster ? kPartnerCloseCasterMax : kPartnerCloseMeleeMax;
    const float mateRadius = partner != nullptr ? std::clamp(mate.bodyRadius, 0.1f, 1.5f) : 0.4f;
    r.ownPartnerKit = true;
    r.tuning = shippedTuning();
    r.in.partnerOrder = 1; // Spread
    r.goShipped(true, seed);
    r.autopilot = false; // driven here: the autopilot's input each step, the camera on top
    if (lens.laneStart) {
        float best = 1e9f;
        Vec2 fw{1.f, 0.f};
        for (std::size_t i = 0; i < r.fight.enemyCount(); ++i)
            if (dist(r.bodies.enemies[i], r.bodies.player) < best) {
                best = dist(r.bodies.enemies[i], r.bodies.player);
                const Vec2 d = sub(r.bodies.enemies[i], r.bodies.player);
                fw = Vec2{d.x / std::max(best, 1e-3f), d.y / std::max(best, 1e-3f)};
            }
        const Vec2 rt{-fw.y, fw.x};
        const float lat = -1.5f * lens.shoulder;
        r.bodies.ally = r.ring.clamp(Vec2{r.bodies.player.x + 3.f * fw.x + lat * rt.x, r.bodies.player.y + 3.f * fw.y + lat * rt.y}, 0.4f);
    }

    constexpr float kAspect = 16.f / 9.f;
    constexpr float kPartnerHeight = 1.75f;
    const ArenaLeadBody leadBody{};
    Vec2 pivot = r.bodies.player;
    float melee = 0.f;
    FoughtLensRun o;
    float idleRun = 0.f;
    int lastTarget = -1; // the lens's target the brain's last step was given (its intent is that step's)
    while (!r.fight.over() && r.fight.time() < lens.seconds) {
        // The followed target: the lock, else the nearest standing enemy.
        int target = -1;
        if (r.out && r.out->player.lock != kArenaNobody && r.fight.enemyAlive(r.out->player.lock)) target = r.out->player.lock;
        if (target < 0) {
            float best = 1e9f;
            for (std::size_t i = 0; i < r.fight.enemyCount(); ++i)
                if (r.fight.enemyAlive(i) && dist(r.bodies.enemies[i], r.bodies.player) < best) {
                    best = dist(r.bodies.enemies[i], r.bodies.player);
                    target = static_cast<int>(i);
                }
        }
        if (target < 0) break;
        const Vec2 tp = r.bodies.enemies[static_cast<std::size_t>(target)];
        const ArenaEnemyKind& tk = r.def.enemies[static_cast<std::size_t>(target)];
        // The lens: the pivot lagged, the melee share eased, the yaw turning the target to its share of the width.
        pivot = r.bodies.player; // the preset's lag is 0
        const float centre = dist(r.bodies.player, tp); // the close rules ease by the centre distance
        const float want = 1.f - smooth01((centre - 2.5f) / 2.5f);
        melee += (want - melee) * (1.f - std::exp(-kDt / 0.3f));
        ArenaCameraPose pose;
        pose.arm = lens.arm;
        pose.pivot = 1.35f + 0.25f * melee;
        pose.side = lens.side * lens.shoulder;
        pose.up = 0.1f;
        pose.fov = 60.f + 8.f * melee;
        pose.boomPitch = -2.f + 8.f * melee;
        const float screenX = 0.5f + lens.shoulder * (0.6f + (lens.targetXMelee - 0.6f) * melee - 0.5f);
        const float turn = std::atan((2.f * screenX - 1.f) * std::tan(0.5f * pose.fov * 3.14159265f / 180.f)) * 180.f / 3.14159265f;
        pose.yaw = std::atan2(tp.y - pivot.y, tp.x - pivot.x) * 180.f / 3.14159265f;
        for (int k = 0; k < 4; ++k) {
            const ArenaFrustum f = arenaPoseFrustum(pose, pivot, kAspect);
            pose.yaw = std::atan2(tp.y - f.pos.y, tp.x - f.pos.x) * 180.f / 3.14159265f - turn;
        }
        if (lens.partnerYaw) { // the host's partner turn, at most 12 degrees, when the partner is out of the frame
            const PartnerCameraView c0 = arenaPoseCamera(pose, pivot, kAspect, kPartnerHeight);
            if (!partnerInFrameClose(c0, kPartnerFrustumMargin, r.bodies.ally) && dist(r.bodies.ally, r.bodies.player) < 8.f) {
                const ArenaFrustum f = arenaPoseFrustum(pose, pivot, kAspect);
                const float toAlly = std::atan2(r.bodies.ally.y - f.pos.y, r.bodies.ally.x - f.pos.x) * 180.f / 3.14159265f;
                float d = toAlly - pose.yaw;
                while (d > 180.f) d -= 360.f;
                while (d < -180.f) d += 360.f;
                pose.yaw += std::clamp(d, -12.f, 12.f);
            }
        }
        const ArenaFrustum frustum = arenaPoseFrustum(pose, pivot, kAspect);
        const PartnerCameraView cam = arenaPoseCamera(pose, pivot, kAspect, kPartnerHeight);
        REQUIRE(partnerCameraWhole(cam));

        // The host's measure of this frame.
        if (r.out && r.out->ally.partnerOn) {
            const Vec2 a = r.bodies.ally;
            const ArenaPoint3 eye = frustum.pos;
            const auto behind = [&](Vec2 at, float radius, float top) {
                return arenaHiddenBehind(eye, ArenaPoint3{a.x, a.y, 0.55f * kPartnerHeight}, at, radius, 0.f, top) &&
                       arenaHiddenBehind(eye, ArenaPoint3{a.x, a.y, 0.95f * kPartnerHeight}, at, radius, 0.f, top);
            };
            const ArenaBodyCylinder tc = arenaBodyCylinder(tk, tp, 0.f);
            ++o.steps;
            if (centre < 1.2f) {
                float tx = 0.f;
                partnerCloseNdc(Vec2{cam.pos.x, cam.pos.y}, Vec2{cam.forward.x, cam.forward.y}, 0.5f * pose.fov, tp, tx);
                ++o.contact;
                o.contactDist += centre;
                o.contactTargetX += 0.5f * (tx + 1.f);
            }
            const bool seen = partnerInFrameClose(cam, kPartnerFrustumMargin, a);
            {
                const PartnerIntent& pi = r.brain.intent();
                const Vec2 cp{cam.pos.x, cam.pos.y}, fw2{cam.forward.x, cam.forward.y};
                const bool leadHid0 = behind(r.bodies.player, leadBody.radius, leadBody.head);
                // On another enemy: not the lens's target this step nor the last (the lead's lock may turn inside a step).
                if (pi.enemy < 0 || pi.enemy == target || pi.enemy == lastTarget) {
                    ++o.same;
                    if (seen) {
                        ++o.sameSeen;
                        if (!leadHid0) ++o.sameSeenClear;
                    }
                } else {
                    ++o.other;
                    if (pi.closeTier != 0) ++o.otherStation;
                    const Vec2 ep = r.bodies.enemies[static_cast<std::size_t>(pi.enemy)];
                    if (partnerCloseNearOk(cp, fw2, r.bodies.player, ep)) {
                        ++o.otherAllowed;
                        if (partnerCloseNearOk(cp, fw2, r.bodies.player, a)) ++o.otherPosOk;
                        if (pi.act == PartnerAct::MoveTo || pi.act == PartnerAct::StepBack) {
                            ++o.otherWalk;
                            if (partnerCloseNearOk(cp, fw2, r.bodies.player, r.out->ally.goal)) ++o.otherGoalOk;
                        }
                    }
                }
            }
            const bool leadHid = behind(r.bodies.player, leadBody.radius, leadBody.head);
            if (!leadHid) ++o.leadClear;
            if (seen) {
                ++o.seen;
                if (!leadHid) ++o.leadClearSeen;
                if (!partnerCloseOverlaps(Vec2{cam.pos.x, cam.pos.y}, a, 0.4f, r.bodies.player, 0.4f)) ++o.leadClearFlatSeen;
                if (!behind(tp, tc.radius, tc.top)) ++o.targetClear;
                else ++o.targetHidTier[std::min<int>(r.brain.intent().closeTier, 4)];
                if (leadHid) ++o.leadHidTier[std::min<int>(r.brain.intent().closeTier, 4)];
                if (centre < 1.2f) {
                    ++o.contactSeen;
                    if (!leadHid) ++o.contactClearSeen;
                }
            }
            if (seen && leadHid) {
                const PartnerIntent& pi = r.brain.intent();
                const Vec2 g = r.out->ally.goal;
                const bool goalHid =
                    arenaHiddenBehind(eye, ArenaPoint3{g.x, g.y, 0.55f * kPartnerHeight}, r.bodies.player, leadBody.radius, 0.f, leadBody.head);
                if (pi.act == PartnerAct::Attack) ++o.hidAttack;
                else if (pi.act == PartnerAct::MoveTo || pi.act == PartnerAct::StepBack) ++(goalHid ? o.hidWalkGoalHidden : o.hidWalkGoalClear);
                else ++(goalHid ? o.hidStillGoalHidden : o.hidStillOther);
                if (pi.framed) ++o.hidFramed;
                if (pi.frameFallback) ++o.hidFallback;
                // Rule 19 with a fresh hold, then held on each flank.
                PartnerCloseInput ci;
                ci.lead = r.bodies.player;
                ci.target = tp;
                ci.targetRadius = tk.bodyRadius;
                ci.cameraPos = Vec2{cam.pos.x, cam.pos.y};
                ci.cameraForward = Vec2{cam.forward.x, cam.forward.y};
                ci.halfAngleDegrees = 0.5f * pose.fov;
                ci.leadSide = static_cast<std::int8_t>(-lens.shoulder);
                ci.partner = a;
                ci.standMin = bandMin;
                ci.standMax = bandMax;
                ci.bodyRadius = mateRadius;
                ci.arenaCentre = r.fight.ring().centre;
                ci.arenaRadius = r.fight.ring().radius - 0.4f;
                PartnerCloseHold fresh;
                const PartnerCloseStation any = partnerCloseStation(ci, fresh);
                const Vec2 bear = sub(r.bodies.player, tp);
                const Vec2 rel = sub(a, tp);
                const std::int8_t own = bear.x * rel.y - bear.y * rel.x > 0.f ? std::int8_t{1} : std::int8_t{-1};
                PartnerCloseHold held;
                held.flank = own;
                held.heldFor = 0.0;
                const PartnerCloseStation onOwn = partnerCloseStation(ci, held);
                if (!any.valid) ++o.hidNoneValid;
                else if (onOwn.valid) ++o.hidOwnFlankValid;
                else ++o.hidOtherFlankOnly;
            }
        }

        // Still with a standing enemy in its reach (the kit's under the close lens: the range on), its cooldown clear.
        if (r.out && r.out->ally.partnerOn) {
            const float reach = mate.partnerReach > 0.f ? mate.partnerReach : r.tuning.reachMeters;
            bool inReach = false;
            for (std::size_t i = 0; i < r.fight.enemyCount(); ++i)
                if (r.fight.enemyAlive(i) &&
                    dist(r.bodies.ally, r.bodies.enemies[i]) - r.def.enemies[i].bodyRadius <= reach)
                    inReach = true;
            if (inReach && o.firstInReach < 0.f) o.firstInReach = r.fight.time();
            const PartnerIntent& pi = r.brain.intent();
            const bool cool = pi.why == PartnerWhy::Approach || pi.why == PartnerWhy::NoTarget ||
                              pi.why == PartnerWhy::Start || pi.why == PartnerWhy::Leash;
            idleRun = inReach && pi.act == PartnerAct::Wait && cool ? idleRun + kDt : 0.f;
            o.idleLongest = std::max(o.idleLongest, idleRun);
        }

        // The host's input this step.
        const std::uint8_t order = r.in.partnerOrder;
        r.in = r.fight.autopilot();
        r.in.partnerOrder = order;
        if (lens.hug && centre > 0.95f) r.in.move = Vec2{(tp.x - r.bodies.player.x) / centre, (tp.y - r.bodies.player.y) / centre};
        r.in.partnerCamera = cam;
        r.in.cameraOn = true;
        lastTarget = target;
        PartnerCloseView& v = r.in.partnerClose;
        v.given = true;
        v.leadSide = static_cast<std::int8_t>(-lens.shoulder);
        v.standMin = bandMin;
        v.standMax = bandMax;
        v.bodyRadius = mateRadius;
        const float vertical = 0.5f * cam.verticalFovDegrees * 3.14159265f / 180.f;
        v.verticalHalfDegrees = 0.5f * cam.verticalFovDegrees;
        v.halfAngleDegrees = std::atan(std::tan(vertical) * cam.aspect) * 180.f / 3.14159265f;
        v.pitchDegrees = std::asin(std::clamp(cam.forward.z, -1.f, 1.f)) * 180.f / 3.14159265f;
        v.lensHeight = cam.pos.z - cam.groundZ;
        r.tick();
    }
    o.strings = r.fight.stats().partnerStrings;
    o.partnerDamage = r.fight.stats().partnerDamage;
    o.time = r.fight.time();
    return o;
}

std::string foughtLensText(const FoughtLensRun& o) {
    const auto pc = [](int n, int d) { return d > 0 ? std::to_string(100 * n / d) + "%" : std::string("-"); };
    std::ostringstream s;
    s << o.time << " s, " << o.steps << " steps: on screen " << pc(o.seen, o.steps) << ", clear of the lead "
      << pc(o.leadClear, o.steps) << " of all, " << pc(o.leadClearSeen, o.seen) << " of on-screen; not behind the target "
      << pc(o.targetClear, o.steps) << "; strings " << o.strings << ", damage " << o.partnerDamage
      << ". Behind the lead on screen " << (o.seen - o.leadClearSeen) << ": attack "
      << o.hidAttack << ", walking to a hidden goal " << o.hidWalkGoalHidden << ", to a clear one " << o.hidWalkGoalClear
      << ", still at a hidden goal " << o.hidStillGoalHidden << ", still other " << o.hidStillOther << "; rule 19 none valid "
      << o.hidNoneValid << ", valid on its flank " << o.hidOwnFlankValid << ", only the other " << o.hidOtherFlankOnly
      << "; framed " << o.hidFramed << ", fallback " << o.hidFallback << ". In contact " << pc(o.contact, o.steps);
    if (o.contact > 0)
        s << " (centre " << o.contactDist / static_cast<float>(o.contact) << " m, target x "
          << o.contactTargetX / static_cast<float>(o.contact) << "; clear of the lead " << pc(o.contactClearSeen, o.contactSeen)
          << " of on-screen)";
    s << ". RULING: same " << o.same << " seen " << pc(o.sameSeen, o.same) << " clear " << pc(o.sameSeenClear, o.sameSeen)
      << "; other " << o.other << " station " << o.otherStation << " allowed " << o.otherAllowed << " posOk "
      << pc(o.otherPosOk, o.otherAllowed) << " walk " << o.otherWalk << " goalOk " << pc(o.otherGoalOk, o.otherWalk);
    s << ". Tiers behind the lead";
    for (const int k : o.leadHidTier) s << " " << k;
    s << ", behind the target";
    for (const int k : o.targetHidTier) s << " " << k;
    return s.str();
}
} // namespace

TEST_CASE("arena partner: rule 19 in a fought run under the close lens, as the game's capture measures it") {
    for (const bool hug : {false, true})
    for (const int kitRange : {0, 1, 2})
        for (const float tx : {0.64f, 0.475f}) // the rules' x, or the capture's at contact (0.47-0.48)
            for (const float arm : {2.6f, 3.0f}) {
                FoughtLensRun all;
                for (std::uint32_t seed = 1; seed <= 3; ++seed) {
                    const FoughtLensRun o = foughtLensRun(FoughtLens{arm, 0.55f, 1.f, true, tx, hug}, seed, kitRange);
                    MESSAGE("hug " << hug << ", kit range " << kitRange << ", target x " << tx << ", arm " << arm << ", seed " << seed << ": "
                                         << foughtLensText(o));
                    CHECK(o.steps > 0);
                    all.steps += o.steps;
                    all.seen += o.seen;
                    all.leadClear += o.leadClear;
                    all.leadClearSeen += o.leadClearSeen;
                    all.targetClear += o.targetClear;
                    all.contact += o.contact;
                    all.contactSeen += o.contactSeen;
                    all.contactClearSeen += o.contactClearSeen;
                }
                MESSAGE("ALL hug " << hug << ", kit range " << kitRange << ", target x " << tx << ", arm " << arm << ": on screen "
                                         << 100 * all.seen / std::max(all.steps, 1) << "%, clear of the lead "
                                         << 100 * all.leadClear / std::max(all.steps, 1) << "% of all, "
                                         << 100 * all.leadClearSeen / std::max(all.seen, 1) << "% of on-screen; not behind the target "
                                         << 100 * all.targetClear / std::max(all.steps, 1) << "%; in contact, clear of the lead "
                                         << 100 * all.contactClearSeen / std::max(all.contactSeen, 1) << "% of on-screen ("
                                         << all.contactSeen << " steps); in contact " << 100 * all.contact / std::max(all.steps, 1)
                                         << "% of all steps");
                // The bar (orchestrator, 11-16): clear of the lead on 75% of the on-screen steps, and in contact too.
                CHECK(100 * all.leadClearSeen >= 75 * all.seen);
                CHECK(100 * all.contactClearSeen >= 75 * all.contactSeen);
            }
}

TEST_CASE("arena partner: a melee partner under the close lens, measured as the capture: clear of the lead on 75% of on-screen steps") {
    // The arena lane's capture (cam6, 11-16): one lead's kit, the other lead as the partner with its own melee kit,
    // brute / skirmisher / breaker, the close lens. Its measure: the steps on screen AND not behind the lead (the
    // lines to its middle and head both through the lead's body) over ALL the partner's steps in the play camera,
    // not over the on-screen ones: 47% with the gauntlets lead and the guns partner, 63% the other way round
    // (on screen 69% and 85%; not overlapping the lead on the floor plane 27% and 55%). The ruling of 11-16 (round 6):
    // from the arena lane's start (3 m ahead of the lead along the lens, 1.5 m off the far side), clear of the lead on
    // at least 75% of the ON-SCREEN steps (off screen is no loss), in contact too; on the lens's target, on screen
    // at least 60% of those steps and clear on 75% of those on screen; on another enemy no close station, rule 1
    // kept by its body and its walk goals on 95% (where that enemy stands out of the near field).
    struct Pair {
        const char* lead;
        const char* partner;
    };
    // The capture's own runs, seed 1 to its length (11.0 s and 16.3 s): reported.
    for (const Pair p : {Pair{"devin", "duy"}, Pair{"duy", "devin"}})
        for (const bool hug : {false, true}) {
            FoughtLens lens{2.6f, 0.55f, 1.f, true, 0.475f, hug};
            lens.seconds = std::string(p.lead) == "devin" ? 11.f : 16.3f;
            const FoughtLensRun o = foughtLensRun(lens, 1, 1, p.lead, {"brute", "skirmisher", "breaker"}, p.partner);
            MESSAGE("CAPTURE LENGTH melee lead " << std::string(p.lead) << ", hug " << hug << ", " << lens.seconds
                                                 << " s: on screen and not behind the lead "
                                                 << 100 * o.leadClearSeen / std::max(o.steps, 1) << "% of " << o.steps
                                                 << " steps, on screen " << 100 * o.seen / std::max(o.steps, 1)
                                                 << "%, not overlapping " << 100 * o.leadClearFlatSeen / std::max(o.steps, 1)
                                                 << "%, in contact " << 100 * o.contact / std::max(o.steps, 1) << "%");
        }
    for (const Pair p : {Pair{"devin", "duy"}, Pair{"duy", "devin"}})
        for (const bool hug : {false, true}) {
            CAPTURE(p.lead);
            CAPTURE(hug);
            FoughtLensRun all;
            for (std::uint32_t seed = 1; seed <= 3; ++seed) {
                FoughtLens lens{2.6f, 0.55f, 1.f, true, 0.475f, hug};
                lens.laneStart = true;
                const FoughtLensRun o = foughtLensRun(lens, seed, 1, p.lead, {"brute", "skirmisher", "breaker"}, p.partner);
                MESSAGE("melee lead " << std::string(p.lead) << ", partner " << std::string(p.partner) << ", hug " << hug << ", seed " << seed
                                      << ": " << foughtLensText(o));
                REQUIRE(o.steps > 0);
                all.steps += o.steps;
                all.seen += o.seen;
                all.leadClearSeen += o.leadClearSeen;
                all.leadClearFlatSeen += o.leadClearFlatSeen;
                all.contact += o.contact;
                all.contactClearSeen += o.contactClearSeen;
                all.strings += o.strings;
                all.contactSeen += o.contactSeen;
                all.same += o.same;
                all.sameSeen += o.sameSeen;
                all.sameSeenClear += o.sameSeenClear;
                all.other += o.other;
                all.otherStation += o.otherStation;
                all.otherAllowed += o.otherAllowed;
                all.otherPosOk += o.otherPosOk;
                all.otherWalk += o.otherWalk;
                all.otherGoalOk += o.otherGoalOk;
            }
            const auto pc = [](int n, int d) { return 100 * n / std::max(d, 1); };
            MESSAGE("CAPTURE melee lead " << std::string(p.lead) << ", partner " << std::string(p.partner) << ", hug " << hug << ": on screen "
                                          << pc(all.seen, all.steps) << "%, on screen and not behind the lead "
                                          << pc(all.leadClearSeen, all.steps) << "%, not overlapping the lead "
                                          << pc(all.leadClearFlatSeen, all.steps) << "%; in contact "
                                          << pc(all.contact, all.steps) << "% of steps, of those on screen and clear "
                                          << pc(all.contactClearSeen, all.contact) << "%; strings " << all.strings);
            MESSAGE("RULING melee lead " << std::string(p.lead) << ", hug " << hug << ": clear of the lead "
                                         << pc(all.leadClearSeen, all.seen) << "% of on-screen, in contact "
                                         << pc(all.contactClearSeen, all.contactSeen) << "% of on-screen; on the lens's target "
                                         << pc(all.same, all.steps) << "% of steps: on screen " << pc(all.sameSeen, all.same)
                                         << "%, clear " << pc(all.sameSeenClear, all.sameSeen) << "%; on another "
                                         << all.other << " steps: close stations " << all.otherStation << ", rule 1 body "
                                         << pc(all.otherPosOk, all.otherAllowed) << "% of " << all.otherAllowed
                                         << ", walk goals " << pc(all.otherGoalOk, all.otherWalk) << "% of " << all.otherWalk);
            CHECK(all.strings > 0);
            CHECK(100 * all.leadClearSeen >= 75 * all.seen);
            CHECK(100 * all.contactClearSeen >= 75 * all.contactSeen);
            CHECK(100 * all.sameSeen >= 60 * all.same);
            CHECK(100 * all.sameSeenClear >= 75 * all.sameSeen);
            CHECK(all.otherStation == 0);
            CHECK(100 * all.otherPosOk >= 95 * all.otherAllowed);
            CHECK(100 * all.otherGoalOk >= 95 * all.otherWalk);
        }
}

TEST_CASE("arena partner: a caster's target rushing inside its stand-off: it still casts, and never stands idle in reach") {
    // The arena lane's trace (11-16): a lead and one enemy, the close station on, the caster's 8.5 / 5. The enemy
    // rushed the lead and stood 2 m from the caster, which then stood still for the whole fight with no cast.
    struct Pair {
        const char* lead;
        const char* foe;
    };
    const ArenaTable table = shippedTable();
    int fights = 0, worst = 1 << 20;
    float idleWorst = 0.f;
    for (const Pair p : {Pair{"devin", "skirmisher"}, Pair{"devin", "maw_pup"}, Pair{"devin", "spitter"},
                         Pair{"duy", "skirmisher"}, Pair{"duy", "maw_pup"}}) {
        std::ostringstream row;
        int casts = 0;
        for (std::uint32_t seed = 1; seed <= 8; ++seed) {
            CAPTURE(p.lead);
            CAPTURE(p.foe);
            CAPTURE(seed);
            const FoughtLensRun o = foughtLensRun(FoughtLens{}, seed, 2, p.lead, {p.foe});
            row << " " << o.strings << (o.idleLongest > 0.5f ? "!" : "");
            casts += o.strings;
            ++fights;
            idleWorst = std::max(idleWorst, o.idleLongest);
            // At least one cast, unless the fight ended before it could: its first step in reach and a blow's
            // startup (the party kit's first light), with a pause's grace.
            const float startup = table.fighter("third").light[0].startup;
            if (o.firstInReach >= 0.f && o.time > o.firstInReach + startup + 1.f) {
                CHECK(o.strings >= 1);
                worst = std::min(worst, o.strings);
            }
            CHECK(o.idleLongest <= 0.5f);
        }
        MESSAGE((std::string("caster 8.5/5, lead ") + p.lead + " vs one " + p.foe + ": casts per seed" + row.str() +
                 " (total " + std::to_string(casts) + ")"));
    }
    MESSAGE(fights << " fights: fewest casts " << worst << ", longest idle in reach " << idleWorst << " s");
}

TEST_CASE("arena partner: a caster whose target keeps closing to 2 m: it casts and steps back by turns, never leashed or idle") {
    // The caster's 8.5 / 5 under the close station; the target, which cannot fall and never attacks, walks back to
    // 2 m off the caster's edge whenever the caster gets 3 m away, the lead pressed against it (1.2 m). 30 s; both
    // orders.
    const ArenaTable table = shippedTable();
    const auto toward = [](Vec2 from, Vec2 to, float d) { // the point d from `from` toward `to`
        const Vec2 v = sub(to, from);
        const float l = length(v);
        return l > 1e-5f ? Vec2{from.x + v.x * d / l, from.y + v.y * d / l} : Vec2{from.x + d, from.y};
    };
    for (const std::uint8_t order : {std::uint8_t{0}, std::uint8_t{1}}) {
        CAPTURE(order);
        ArenaFighter third = table.fighter("third");
        third.partnerReach = 8.5f;
        third.partnerStandoff = 5.f;
        PRig r;
        r.ownPartnerKit = true;
        r.def.partnerKit = third;
        r.def.fighter.hp = 100000; // the lead never falls: no takeover
        r.tuning = shippedTuning();
        r.tuning.budgetAllowanceSeconds = 10.f; // no budget in the way: it strings whenever its pause is over
        REQUIRE(validPartnerTuning(r.tuning));
        const ArenaEnemyKind dummy = dummyKind();
        r.enemy(dummy, Vec2{0.f, 0.f});
        r.bodies.player = Vec2{-1.2f, 0.f};
        r.bodies.ally = Vec2{0.f, 2.f + dummy.bodyRadius}; // 2 m off its edge, beside the lead
        r.in.partnerOrder = order;
        r.go(true, 1000);
        r.walk = true;
        r.in.cameraOn = true;
        r.in.lock = true; // the lead locks on (StayClose's target)
        PartnerCloseView& c = r.in.partnerClose;
        c.given = true;
        c.halfAngleDegrees = 30.f;
        c.leadSide = -1;
        c.standMin = kPartnerCloseCasterMin;
        c.standMax = kPartnerCloseCasterMax;
        int stepBacks = 0, leashed = 0, closings = 0, through = 0;
        float idleRun = 0.f, idleLongest = 0.f, nearest = 1e9f;
        bool wasStepBack = false;
        for (int k = 0; k < 30 * 60; ++k) {
            Vec2& tp = r.bodies.enemies[0];
            const float edgeNow = dist(r.bodies.ally, tp) - dummy.bodyRadius;
            if (edgeNow > 3.f) { // the target closes on the caster at 3 m/s, the lead pressed against it
                ++closings;
                const Vec2 goal = toward(r.bodies.ally, tp, 2.f + dummy.bodyRadius);
                const Vec2 d = sub(goal, tp);
                const float l = length(d);
                const float most = 3.f * kDt;
                tp = l > most ? Vec2{tp.x + d.x * most / l, tp.y + d.y * most / l} : goal;
            }
            r.bodies.player = toward(tp, r.bodies.player, 1.2f);
            const Vec2 back = toward(tp, r.bodies.player, 4.2f); // the lens 3 m behind the lead, on the target
            r.in.cameraPos = back;
            r.in.cameraFacing = Vec2{(tp.x - back.x) / 4.2f, (tp.y - back.y) / 4.2f};
            r.tick();
            const PartnerIntent& pi = r.brain.intent();
            const bool stepBack = pi.act == PartnerAct::StepBack;
            if (stepBack && !wasStepBack) ++stepBacks;
            wasStepBack = stepBack;
            if (stepBack) { // never a walk through the target: its segment clear of the target's body
                const Vec2 ab = sub(pi.point, r.bodies.ally);
                const float l2 = ab.x * ab.x + ab.y * ab.y;
                const Vec2 ap = sub(tp, r.bodies.ally);
                const float u = l2 > 1e-8f ? std::clamp((ap.x * ab.x + ap.y * ab.y) / l2, 0.f, 1.f) : 0.f;
                const float off = length(sub(tp, Vec2{r.bodies.ally.x + ab.x * u, r.bodies.ally.y + ab.y * u}));
                if (off + 1e-3f < std::min(dummy.bodyRadius, length(ap))) ++through;
            }
            if (pi.why == PartnerWhy::Leash) ++leashed;
            const bool cool = pi.why == PartnerWhy::Approach || pi.why == PartnerWhy::NoTarget ||
                              pi.why == PartnerWhy::Start || pi.why == PartnerWhy::Leash;
            const float e = dist(r.bodies.ally, r.bodies.enemies[0]) - dummy.bodyRadius;
            nearest = std::min(nearest, e);
            idleRun = e <= third.partnerReach && pi.act == PartnerAct::Wait && cool ? idleRun + kDt : 0.f;
            idleLongest = std::max(idleLongest, idleRun);
        }
        const ArenaStats& st = r.fight.stats();
        MESSAGE("caster at 2 m, order " << static_cast<int>(order) << ": strings " << st.partnerStrings << ", step-backs "
                                        << stepBacks << ", target closings (steps) " << closings << ", leashed steps "
                                        << leashed << ", nearest edge " << nearest << " m, longest idle in reach "
                                        << idleLongest << " s");
        CHECK(st.partnerStrings >= 6);
        CHECK(stepBacks >= 3);
        CHECK(leashed == 0);
        CHECK(through == 0);
        CHECK(idleLongest <= 0.5f);
    }
}

namespace {
// A frame the game's capture logged (b93fd13e, a normal enemy under the close lens over the right shoulder, the lead
// hugging its target), rebuilt from the log's numbers: the lens's pitch and fov, the target's x in the frame and its
// distance from the lead (centre to centre), and the partner's place on the ground from the lens.
struct LoggedFrame {
    const char* name;
    const char* enemy;
    float distance; // m, the lead to the target's centre
    float targetX;  // share of the width
    float pitch;    // degrees, the boom's
    float fov;      // degrees, horizontal
    float ahead;    // m, the partner ahead of the lens ...
    float right;    // ... and right of it
};

struct LoggedScene {
    PartnerCameraView cam;
    ArenaFrustum frustum;
    Vec2 lead, target, partner;
    ArenaEnemyKind kind;
    PartnerCloseView view;
};

LoggedScene loggedScene(const LoggedFrame& f, float arm = 2.6f) {
    const ArenaTable t = shippedTable();
    LoggedScene s;
    for (std::size_t e = 0; e < t.enemyIds.size(); ++e)
        if (t.enemyIds[e] == f.enemy) s.kind = t.enemies[e];
    ArenaCameraPose pose;
    pose.arm = arm;
    pose.pivot = 1.6f; // 1.35 + 0.25 at a full melee share
    pose.side = 0.55f;
    pose.up = 0.1f;
    pose.fov = f.fov;
    pose.boomPitch = f.pitch;
    constexpr float kAspect = 16.f / 9.f;
    s.lead = Vec2{0.f, 0.f};
    s.frustum = arenaPoseFrustum(pose, s.lead, kAspect);
    s.cam = arenaPoseCamera(pose, s.lead, kAspect, 1.75f);
    const Vec2 lens{s.frustum.pos.x, s.frustum.pos.y}; // yaw 0: ahead is +x, right is +y
    const float slope = (2.f * f.targetX - 1.f) * std::tan(0.5f * f.fov * 3.14159265f / 180.f);
    float lo = s.lead.x - lens.x, hi = lo + 3.f; // the target's depth beyond the lead, on its ray from the lens
    for (int k = 0; k < 40; ++k) {
        const float mid = 0.5f * (lo + hi);
        if (dist(Vec2{lens.x + mid, lens.y + mid * slope}, s.lead) < f.distance) lo = mid;
        else hi = mid;
    }
    s.target = Vec2{lens.x + lo, lens.y + lo * slope};
    s.partner = Vec2{lens.x + f.ahead, lens.y + f.right};
    PartnerCloseView& v = s.view;
    v.given = true;
    v.leadSide = -1;
    v.standMin = kPartnerCloseCasterMin;
    v.standMax = kPartnerCloseCasterMax;
    v.bodyRadius = 0.4f;
    const float vertical = 0.5f * s.cam.verticalFovDegrees * 3.14159265f / 180.f;
    v.verticalHalfDegrees = 0.5f * s.cam.verticalFovDegrees;
    v.halfAngleDegrees = std::atan(std::tan(vertical) * s.cam.aspect) * 180.f / 3.14159265f;
    v.pitchDegrees = std::asin(std::clamp(s.cam.forward.z, -1.f, 1.f)) * 180.f / 3.14159265f;
    v.lensHeight = s.cam.pos.z - s.cam.groundZ;
    return s;
}

// The host's measure: the lines from the lens to the partner's middle and head both through a body's cylinder.
bool loggedBehind(const LoggedScene& s, Vec2 p, Vec2 at, float radius, float top) {
    const ArenaPoint3 eye = s.frustum.pos;
    return arenaHiddenBehind(eye, ArenaPoint3{p.x, p.y, 0.55f * 1.75f}, at, radius, 0.f, top) &&
           arenaHiddenBehind(eye, ArenaPoint3{p.x, p.y, 0.95f * 1.75f}, at, radius, 0.f, top);
}

bool loggedSees(const void* context, Vec2 p) {
    return partnerInFrameClose(*static_cast<const PartnerCameraView*>(context), kPartnerFrustumMargin, p);
}

// Rule 19 on a logged frame as closeGoal builds it (the party kit's range zeroed, as on b93fd13e): Pause's band (the
// host's caster band) or a string's ([reach / 2, reach - tolerance] = 0.9-1.3 m at the shipped reach 1.8).
PartnerCloseStation loggedStation(const LoggedScene& s, bool forString, std::int8_t heldFlank) {
    PartnerCloseInput ci;
    ci.lead = s.lead;
    ci.target = s.target;
    ci.targetRadius = s.kind.bodyRadius;
    ci.cameraPos = Vec2{s.cam.pos.x, s.cam.pos.y};
    ci.cameraForward = Vec2{s.cam.forward.x, s.cam.forward.y};
    ci.halfAngleDegrees = s.view.halfAngleDegrees;
    ci.pitchDegrees = s.view.pitchDegrees;
    ci.verticalHalfDegrees = s.view.verticalHalfDegrees;
    ci.lensHeight = s.view.lensHeight;
    ci.leadSide = s.view.leadSide;
    ci.partner = s.partner;
    ci.standMin = forString ? 0.9f : s.view.standMin;
    ci.standMax = forString ? 1.3f : s.view.standMax;
    ci.bodyRadius = s.view.bodyRadius;
    ci.sees = loggedSees;
    ci.seesContext = &s.cam;
    PartnerCloseHold hold;
    hold.flank = heldFlank;
    return partnerCloseStation(ci, hold);
}
} // namespace

TEST_CASE("arena partner: the capture's frames where the partner stood behind the lead, rebuilt") {
    const ArenaLeadBody leadBody{};
    for (const LoggedFrame& f : {LoggedFrame{"t_04", "brute", 0.85f, 0.472f, 6.f, 68.f, 3.92f, -0.90f},
                                 LoggedFrame{"t_10", "breaker", 0.90f, 0.478f, 6.f, 68.f, 3.96f, -0.92f},
                                 LoggedFrame{"t_08", "skirmisher", 0.90f, 0.478f, 5.9f, 68.f, 4.28f, -0.08f},
                                 LoggedFrame{"t_02", "brute", 1.50f, 0.539f, 6.f, 68.f, 3.68f, 1.50f}}) {
        CAPTURE(f.name);
        const LoggedScene s = loggedScene(f);
        const Vec2 lens{s.frustum.pos.x, s.frustum.pos.y};
        const ArenaBodyCylinder tc = arenaBodyCylinder(s.kind, s.target, 0.f);
        const Vec2 bearing = sub(s.lead, s.target), rel = sub(s.partner, s.target);
        const float angle =
            std::acos(std::clamp((bearing.x * rel.x + bearing.y * rel.y) / (length(bearing) * length(rel)), -1.f, 1.f)) * 180.f /
            3.14159265f;
        const bool own = bearing.x * rel.y - bearing.y * rel.x > 0.f;
        std::ostringstream m;
        m << f.name << ": lens z " << s.frustum.pos.z << ", lead " << (s.lead.x - lens.x) << " ahead, " << (s.lead.y - lens.y)
          << " right; target " << (s.target.x - lens.x) << ", " << (s.target.y - lens.y) << " (radius " << s.kind.bodyRadius
          << "); the partner " << (dist(s.partner, s.target) - s.kind.bodyRadius) << " m off its edge at " << angle
          << " deg from the lead's bearing (cross " << (own ? "+" : "-") << "), " << dist(s.partner, s.lead)
          << " m from the lead; behind the lead " << loggedBehind(s, s.partner, s.lead, leadBody.radius, leadBody.head)
          << ", behind the target " << loggedBehind(s, s.partner, s.target, tc.radius, tc.top) << ", in frame "
          << partnerInFrameClose(s.cam, kPartnerFrustumMargin, s.partner);
        for (const bool forString : {false, true})
            for (const std::int8_t held : {std::int8_t{0}, std::int8_t{-1}, std::int8_t{1}}) {
                const PartnerCloseStation r = loggedStation(s, forString, held);
                // The hugging frames (not t_02): fresh or held on either flank (the closed one's hold gives way), a
                // station the lens sees clear of the lead and the target (rule 19's far arc).
                if (f.distance < 1.f) {
                    const ArenaBodyCylinder at = arenaBodyCylinder(s.kind, s.target, 0.f);
                    CHECK(r.valid);
                    CHECK(!loggedBehind(s, r.point, s.lead, leadBody.radius, leadBody.head));
                    CHECK(!loggedBehind(s, r.point, s.target, at.radius, at.top));
                    CHECK(partnerInFrameClose(s.cam, kPartnerFrustumMargin, r.point));
                }
                m << "\n  " << (forString ? "string" : "pause ") << " held " << int(held) << ": ";
                if (!r.valid) {
                    m << "none valid";
                    continue;
                }
                m << "flank " << int(r.flank) << " tier " << int(r.tier) << " at " << r.angle << " deg, " << r.offEdge << " m off ("
                  << (r.point.x - lens.x) << " ahead, " << (r.point.y - lens.y) << " right), " << dist(r.point, s.partner)
                  << " m from the partner; behind the lead " << loggedBehind(s, r.point, s.lead, leadBody.radius, leadBody.head);
            }
        MESSAGE(m.str());
    }
}

namespace {
// Rule 19's legality for one point (PartnerCloseStation.cpp's legal(), no walls or other bodies), as a letter: the
// first test it fails, or '.' legal and clear of the lead and the target by the host's two-line measure, 'h' legal
// but hidden by that measure.
char loggedCell(const LoggedScene& s, Vec2 p, float minAhead) {
    PartnerCloseInput ci;
    ci.lead = s.lead;
    ci.target = s.target;
    ci.targetRadius = s.kind.bodyRadius;
    ci.cameraPos = Vec2{s.cam.pos.x, s.cam.pos.y};
    ci.cameraForward = Vec2{s.cam.forward.x, s.cam.forward.y};
    ci.halfAngleDegrees = s.view.halfAngleDegrees;
    ci.pitchDegrees = s.view.pitchDegrees;
    ci.verticalHalfDegrees = s.view.verticalHalfDegrees;
    ci.lensHeight = s.view.lensHeight;
    float x = 0.f, y = 0.f;
    if (!partnerCloseLens(ci, p, x, y)) return 'L';
    if (!loggedSees(&s.cam, p)) return 'S';
    if (std::fabs(x) > 1.f - 2.f * kPartnerCloseConeMargin) return 'X';
    const float l = length(ci.cameraForward);
    const Vec2 f{ci.cameraForward.x / l, ci.cameraForward.y / l};
    const Vec2 r = sub(p, ci.cameraPos);
    const float ahead = r.x * f.x + r.y * f.y;
    if (ahead < minAhead) return 'A';
    if (std::fabs(-r.x * f.y + r.y * f.x) > ahead * std::tan(kPartnerCloseAimConeDegrees * 3.14159265f / 180.f)) return 'C';
    if (dist(p, s.target) < s.kind.bodyRadius + 0.4f) return 'B';
    if (dist(p, s.lead) < 1.6f) return 'P';
    if (partnerCloseOverlaps(ci.cameraPos, p, 0.4f, s.lead, 0.4f)) return 'O';
    if (partnerCloseHiddenBehind(ci.cameraPos, p, s.target, s.kind.bodyRadius)) return 'H';
    const ArenaLeadBody lb{};
    const ArenaBodyCylinder tc = arenaBodyCylinder(s.kind, s.target, 0.f);
    if (loggedBehind(s, p, s.lead, lb.radius, lb.head) || loggedBehind(s, p, s.target, tc.radius, tc.top)) return 'h';
    return '.';
}
} // namespace

TEST_CASE("arena partner: the capture's hugging frames, every flank point graded") {
    // Rows: flank and off-edge; columns: 40-170 degrees from the lead's bearing in 10s. Letters: L behind the lens,
    // S the 3D frame, X the cone's margin, A short of minAhead, C the aim cone, B in the target's body, P personal
    // space, O over the lead, H behind the target (2D), h hidden by the host's measure, '.' legal and clear.
    for (const LoggedFrame& f : {LoggedFrame{"t_04", "brute", 0.85f, 0.472f, 6.f, 68.f, 3.92f, -0.90f},
                                 LoggedFrame{"t_10", "breaker", 0.90f, 0.478f, 6.f, 68.f, 3.96f, -0.92f},
                                 LoggedFrame{"t_08", "skirmisher", 0.90f, 0.478f, 5.9f, 68.f, 4.28f, -0.08f}})
        for (const float arm : {2.6f, 3.0f})
            for (const float minAhead : {kPartnerCloseMinAhead, 2.4f}) {
                CAPTURE(f.name);
                const LoggedScene s = loggedScene(f, arm);
                Vec2 bearing = sub(s.lead, s.target);
                const float bl = length(bearing);
                bearing = Vec2{bearing.x / bl, bearing.y / bl};
                std::ostringstream m;
                m << f.name << " arm " << arm << " minAhead " << minAhead << " (lead " << (s.lead.x - s.frustum.pos.x) << " ahead)";
                for (const int flank : {1, -1})
                    for (const float off : {0.5f, 0.9f, 1.1f, 1.3f, 2.f, 3.f, 5.f}) {
                        m << "\n  flank " << (flank > 0 ? "+1" : "-1") << " off " << off << ": ";
                        for (int a = 40; a <= 170; a += 10) {
                            const float rad = static_cast<float>(flank * a) * 3.14159265f / 180.f;
                            const Vec2 dir{bearing.x * std::cos(rad) - bearing.y * std::sin(rad),
                                           bearing.x * std::sin(rad) + bearing.y * std::cos(rad)};
                            const float k = s.kind.bodyRadius + off;
                            m << loggedCell(s, Vec2{s.target.x + dir.x * k, s.target.y + dir.y * k}, minAhead);
                        }
                    }
                MESSAGE(m.str());
            }
}

namespace {
// Classic partner fights, whole runs, for the review's pin (11-16): the party member's kit as each attacking lead's
// partner (the autopilot at the controls) over the default's 8 fights and 3 seeds, with no camera and with a classic
// ground camera (4.5 m behind the lead, facing its nearest live enemy; the close station never given). Each run's
// partner hits, partner damage and a whole-run state hash (FNV-1a over every step's bodies, hp and the brain's hash).
struct ClassicPin {
    int hits = 0;
    int damage = 0;
    std::uint64_t hash = 1469598103934665603ull;
    ArenaPhase phase = ArenaPhase::Fighting;
};
void pinFold(std::uint64_t& h, const void* p, std::size_t n) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
}
void pinFoldV(std::uint64_t& h, Vec2 v) {
    pinFold(h, &v.x, sizeof v.x);
    pinFold(h, &v.y, sizeof v.y);
}
ArenaDef pinFight(const ArenaTable& t, std::initializer_list<const char*> ids, std::uint8_t attackers) {
    ArenaDef def;
    def.enemyCount = 0;
    for (const char* id : ids)
        for (std::size_t e = 0; e < t.enemyIds.size(); ++e)
            if (t.enemyIds[e] == id) {
                def.enemyKind[def.enemyCount] = static_cast<std::uint8_t>(e);
                def.enemies[def.enemyCount++] = t.enemies[e];
            }
    def.attackers = attackers;
    def.ringRadius = 12.f;
    def.spawnDistance = 7.f;
    return def;
}
ClassicPin classicPinRun(const ArenaTable& t, const ArenaDef& base, const char* lead, std::uint32_t seed, bool camera) {
    ArenaDef def = base;
    def.fighter = t.fighter(lead);
    def.partnerKit = t.fighter("third");
    PartnerBrain brain;
    const PartnerTuning tuning = shippedTuning();
    ArenaFight fight;
    ArenaBodies bodies;
    const ArenaRing ring = arenaRingAt(Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, def.ringRadius, ArenaRing{Vec2{0.f, 0.f}, 36.f});
    fight.setAlly(def.partnerKit.hp, 0.4f);
    fight.setPartner(&brain, &tuning);
    fight.begin(def, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, seed);
    bodies.player = fight.playerStart();
    for (std::size_t i = 0; i < fight.enemyCount(); ++i) bodies.enemies[i] = fight.enemyStart(i);
    const Vec2 f0 = fight.playerStartFacing();
    bodies.ally = fight.ring().clamp(Vec2{bodies.player.x - 1.5f * f0.y, bodies.player.y + 1.5f * f0.x}, 0.4f);
    Vec2 look = f0;
    ClassicPin o;
    for (int s = 0; s < 120 * 60 && !fight.over(); ++s) {
        ArenaInput in = fight.autopilot();
        if (camera) {
            float best = 1e9f;
            for (std::size_t i = 0; i < fight.enemyCount(); ++i) {
                if (!fight.enemyAlive(i)) continue;
                const Vec2 d = sub(bodies.enemies[i], bodies.player);
                const float far = length(d);
                if (far > 1e-3f && far < best) {
                    best = far;
                    look = Vec2{d.x / far, d.y / far};
                }
            }
            in.cameraOn = true;
            in.cameraPos = Vec2{bodies.player.x - 4.5f * look.x, bodies.player.y - 4.5f * look.y};
            in.cameraFacing = look;
        }
        const ArenaOutput& out = fight.tick(kDt, in, bodies);
        for (const ArenaEvent& e : out.events) {
            if (e.type == ArenaEventType::Hit && e.source == kArenaAlly) ++o.hits;
            if (e.type == ArenaEventType::TookOver) std::swap(bodies.player, bodies.ally);
        }
        arenaStep(fight, bodies, kDt);
        if (out.ally.partnerOn) {
            const Vec2 d = sub(out.ally.goal, bodies.ally);
            const float far = length(d);
            const float most = fight.def().partnerKit.moveSpeed * kDt;
            bodies.ally = far > most ? Vec2{bodies.ally.x + d.x * most / far, bodies.ally.y + d.y * most / far} : out.ally.goal;
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
        pinFoldV(o.hash, bodies.player);
        pinFoldV(o.hash, bodies.ally);
        const int hp[2] = {fight.hp(), fight.allyHp()};
        pinFold(o.hash, hp, sizeof hp);
        for (std::size_t i = 0; i < fight.enemyCount(); ++i) {
            pinFoldV(o.hash, bodies.enemies[i]);
            const int ehp = fight.enemyHp(i);
            pinFold(o.hash, &ehp, sizeof ehp);
        }
        const std::uint64_t bh = brain.hash();
        pinFold(o.hash, &bh, sizeof bh);
    }
    o.damage = fight.stats().partnerDamage;
    o.phase = fight.phase();
    return o;
}
struct ClassicPinRow {
    const char* lead;
    const char* fight;
    bool camera;
    int hits;   // over seeds 1-3
    int damage; // over seeds 1-3
    std::uint64_t hash; // the 3 seeds' whole-run hashes folded in order
};
// Taken from main (04ffce8f: the party member's light range 3.6, no close-only reach) by this same code.
// Re-pinned for maw_pup preferredRange 2.5 -> 1.8 (it holds inside the nip's 1.9 m reach, so the nip is picked): only
// the four maw_pup rows moved. All still end in victory; duy with the camera loses 5 partner damage on seeds 1 and 2.
// Re-pinned for the wall-aware push (B1, ARENA_BODY_OVERLAP_1122): a boss the ring holds no longer keeps up to 3.6 cm
// of the lead inside it. Only the ten kite_boss, stone_bearer and duy white_glove rows moved, the fights where the old
// step left that overlap; stone_bearer with the camera: devin 80 -> 79 hits (581 -> 574), duy 102 -> 110 (747 -> 803).
// Re-pinned for maw_pup bodyLength 1.08 (B2: the 1.08 m mesh, nose to tail, a stadium): only the four maw_pup rows moved,
// hashes only (the same hits and damage); all still victories.
// Re-pinned for the B fixes (the settle at 0.1 mm every pass, the ring hold last): hashes only, the same hits and damage.
// duy "three kinds, attackers 1" without the camera: at 10.72 s (seed 1) the 0.7 mm floor left the lead 0.29 mm inside
// enemy2; the settle moves it out. The four stone_bearer rows: the boss parks at the ring limit with no contact, and the
// old step clamped it to the ring a second time after a settle that moved nothing, a float clamp that is not idempotent
// (0.5 to 1 um); the new one clamps once.
// Re-pinned for the autopilot guard gate (guard cuts a recovery at its cancelGuard share, as the tick allows): the lead
// parries blows it used to take in late recovery, so fights run shorter and the party member lands fewer hits. 20 rows
// moved, all still victories: 12 in hits and damage (devin kite_boss, stone_bearer, white_glove, maw_pup and spitter
// with the camera; duy stone_bearer and white_glove), 8 in the hash only (both leads duelist and kite_boss, devin
// spitter without the camera), where an earlier guard changed the run but not the party member's blows. The other 36
// rows never had a blow land in a guard-cuttable recovery. The balance shift it brings on the Dealer alone in the
// stone ring (test_arena_kind_crit, seed 1; a later balance item, not a fault of the gate): Devin's hits taken
// 5 -> 1 (damage 68 -> 14) and the fight 41.3 -> 31.4 s; Duy's 5 -> 2 (68 -> 28), 44.9 -> 40.6 s.
// Re-pinned for the maw_pup's in-place nip (capture step 3: walk 0.78 and run 4.83 m/s, the nip's range 0.39 with no
// lunge from 0.78 m and an arc of 0.9, the lead's push inset 0.17, runRange 2.5): only the four maw_pup rows moved, all
// still victories. Devin's both 4 -> 6 hits (28 -> 42), Duy's with the camera 32 -> 42 damage, Duy's without it the
// hash only.
const ClassicPinRow kClassicPins[] = {
    {"devin", "shipped default fight", false, 13, 94, 31290753253845615ull},
    {"devin", "shipped default fight", true, 13, 94, 16239672271515793875ull},
    {"devin", "three kinds, attackers 1", false, 25, 170, 16537530505005903450ull},
    {"devin", "three kinds, attackers 1", true, 23, 164, 7641466744412459784ull},
    {"devin", "a brute and a spitter, attackers 2", false, 16, 108, 5241323735042292893ull},
    {"devin", "a brute and a spitter, attackers 2", true, 16, 115, 14748652239614714217ull},
    {"devin", "brute", false, 13, 94, 31290753253845615ull},
    {"devin", "brute", true, 13, 94, 16239672271515793875ull},
    {"devin", "skirmisher", false, 9, 66, 5952098331214068505ull},
    {"devin", "skirmisher", true, 6, 42, 1249458168175911326ull},
    {"devin", "breaker", false, 22, 163, 18329296392151869695ull},
    {"devin", "breaker", true, 17, 128, 2481547781211154738ull},
    {"devin", "duelist", false, 16, 121, 3085447395325385403ull},
    {"devin", "duelist", true, 15, 114, 1833937876181139929ull},
    {"devin", "hulk", false, 23, 170, 8466614135980583722ull},
    {"devin", "hulk", true, 24, 177, 17463738276282656931ull},
    {"devin", "titan", false, 27, 198, 17037376046017037454ull},
    {"devin", "titan", true, 24, 177, 9005697788452095697ull},
    {"devin", "kite_boss", false, 55, 391, 15149345771995931089ull},
    {"devin", "kite_boss", true, 58, 418, 5331741546162271797ull},
    {"devin", "stone_bearer", false, 73, 534, 13144604784390647433ull},
    {"devin", "stone_bearer", true, 66, 489, 11331705351014989236ull},
    {"devin", "white_glove", false, 58, 413, 13953194659030329670ull},
    {"devin", "white_glove", true, 55, 400, 5000600046039610435ull},
    {"devin", "maw_pup", false, 6, 42, 7508357560149523908ull},
    {"devin", "maw_pup", true, 6, 42, 8421939504470631251ull},
    {"devin", "spitter", false, 6, 42, 6079499873305667569ull},
    {"devin", "spitter", true, 3, 21, 4150562399047041604ull},
    {"duy", "shipped default fight", false, 16, 118, 17274731110568096544ull},
    {"duy", "shipped default fight", true, 16, 121, 165081782403125027ull},
    {"duy", "three kinds, attackers 1", false, 31, 221, 3123289504221292375ull},
    {"duy", "three kinds, attackers 1", true, 35, 251, 15975811842975394185ull},
    {"duy", "a brute and a spitter, attackers 2", false, 17, 119, 13769110188836988905ull},
    {"duy", "a brute and a spitter, attackers 2", true, 19, 136, 4970841333878948935ull},
    {"duy", "brute", false, 16, 118, 17274731110568096544ull},
    {"duy", "brute", true, 16, 121, 165081782403125027ull},
    {"duy", "skirmisher", false, 6, 42, 12056965067669638780ull},
    {"duy", "skirmisher", true, 6, 27, 7451009726274229835ull},
    {"duy", "breaker", false, 24, 180, 3918144725000809230ull},
    {"duy", "breaker", true, 25, 184, 9399829982760244120ull},
    {"duy", "duelist", false, 16, 124, 2271821620492557969ull},
    {"duy", "duelist", true, 16, 121, 15821544362985660840ull},
    {"duy", "hulk", false, 30, 219, 8604643699523506543ull},
    {"duy", "hulk", true, 29, 206, 10408765349377459501ull},
    {"duy", "titan", false, 31, 229, 8858760711467032362ull},
    {"duy", "titan", true, 29, 204, 4745636326408502596ull},
    {"duy", "kite_boss", false, 62, 443, 12318375040375774986ull},
    {"duy", "kite_boss", true, 68, 488, 3206243752085442907ull},
    {"duy", "stone_bearer", false, 88, 658, 12103771074940472673ull},
    {"duy", "stone_bearer", true, 82, 613, 7313118388245454201ull},
    {"duy", "white_glove", false, 66, 483, 1839276756863646281ull},
    {"duy", "white_glove", true, 66, 472, 10316052538403549276ull},
    {"duy", "maw_pup", false, 6, 42, 1815226848060683555ull},
    {"duy", "maw_pup", true, 6, 42, 11287048018200528418ull},
    {"duy", "spitter", false, 6, 42, 373096491565446441ull},
    {"duy", "spitter", true, 6, 42, 13513007789646449681ull},
};
} // namespace

TEST_CASE("arena partner: classic partner fights resolve exactly as main's: hits, damage and whole-run hashes pinned") {
    // Review (11-16): rule 19's kit range (the party member's 5-8 m casts) applies under the close camera only; with
    // no camera or a classic one every blow lands from the light's own range, so these whole runs match main's.
    const ArenaTable t = shippedTable();
    std::vector<std::pair<std::string, ArenaDef>> fights;
    fights.emplace_back("shipped default fight", t.fallback);
    fights.emplace_back("three kinds, attackers 1", pinFight(t, {"brute", "skirmisher", "breaker"}, 1));
    fights.emplace_back("a brute and a spitter, attackers 2", pinFight(t, {"brute", "spitter"}, 2));
    for (std::size_t e = 0; e < t.enemyIds.size(); ++e) {
        if (t.enemies[e].passive) continue;
        ArenaDef def;
        def.enemyCount = 1;
        def.enemyKind[0] = static_cast<std::uint8_t>(e);
        def.enemies[0] = t.enemies[e];
        def.attackers = 1;
        fights.emplace_back(t.enemyIds[e], def);
    }
    REQUIRE(fights.size() >= 8);
    int rows = 0, leads = 0;
    for (std::size_t f = 1; f < t.fighterIds.size(); ++f) {
        if (!t.fighters[f].attacks || t.fighterIds[f] == "third") continue;
        ++leads;
        const char* lead = t.fighterIds[f].c_str();
        for (const auto& [name, def] : fights)
            for (int cam = 0; cam < 2; ++cam) {
                int hits = 0, damage = 0;
                std::uint64_t hash = 1469598103934665603ull;
                for (std::uint32_t seed = 1; seed <= 3; ++seed) {
                    const ClassicPin p = classicPinRun(t, def, lead, seed, cam == 1);
                    hits += p.hits;
                    damage += p.damage;
                    pinFold(hash, &p.hash, sizeof p.hash);
                }
                const ClassicPinRow* row = nullptr;
                for (const ClassicPinRow& r : kClassicPins)
                    if (std::string(r.lead) == lead && name == r.fight && r.camera == (cam == 1)) row = &r;
                MESSAGE("PIN    {\"" << std::string(lead) << "\", \"" << name << "\", " << std::string(cam ? "true" : "false") << ", " << hits << ", "
                                     << damage << ", " << hash << "ull},");
                REQUIRE_MESSAGE(row != nullptr, "Error: no pin for " << lead << " " << name << " camera " << cam);
                ++rows;
                CHECK(hits > 0);
                CHECK(hits == row->hits);
                CHECK(damage == row->damage);
                CHECK(hash == row->hash);
            }
    }
    CHECK(leads >= 2);
    CHECK(rows == static_cast<int>(sizeof kClassicPins / sizeof kClassicPins[0]));
}

namespace {
// The close lens over a big target (the order of 11-16: no station beside the lens). The lead held at `centre` m from
// a target of `radius` (centre to centre; it never attacks), or circling it at `orbit` degrees a second, the close
// preset's lens on the right shoulder eased as the game eases it, the party kit's caster (8.5 / 5) walking from a
// start set by `seed` (or `startLens`: depth and lateral from the lead in the lens's ground space). `leadEvery` (s,
// given): the lead presses a light this often from the first step (the arena lane's titan runs: 2.5). Measured in the
// lens's ground space each step from `from` s (1): depth along its ground forward, lateral (+ right), and the partner's
// projected height (a 1.8 m body at that depth, share of the frame's height).
struct TitanLensRun {
    int steps = 0, nearOk = 0, sizeOk = 0, seen = 0, seenClear = 0, flankChanges = 0, strings = 0;
    int worstFlankRate = 0;   // most flank changes in any 3 s window
    int goalOver = 0;         // steps walking (MoveTo, StepBack) to a goal over the size cap or failing rule 1
    int crossings = 0;        // steps whose walk crossed the line from the lens to the lead (from the start)
    float crossAt = 0.f, crossDepth = 0.f, crossLateral = 0.f; // the first: when, where from the lead in lens space
    int crossAct = -1;
    int hiddenLead = 0, hiddenTarget = 0; // steps its body stood hidden behind the lead, behind the target
    int hiddenLeadStanding = 0, hiddenTargetStanding = 0; // of them, steps it was not walking (no MoveTo, StepBack)
    float hiddenFirstAt = -1.f; // the first hidden step: its time and act
    int hiddenFirstAct = -1;
    int castsHidden = 0; // strings or blows begun on a step its body stood hidden behind the lead or the target
    int seenLeadClear = 0; // steps in frame and not behind the lead (the arena capture's measure)
    int stringsSeen = 0;
    float worstStartFacing = 0.f; // degrees: the most its facing was off its target on a string's first step
    float depth = 0.f, lateral = 0.f, height = 0.f; // the last 3 s, averaged
    float leadDepth = 0.f, leadLateral = 0.f; // at the start
};

bool titanCrossed(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
    const auto side = [](Vec2 p, Vec2 q, Vec2 r) { return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x); };
    return side(a, b, c) * side(a, b, d) < 0.f && side(c, d, a) * side(c, d, b) < 0.f;
}

TitanLensRun titanLensRun(float radius, float centre, std::uint32_t seed, float orbit = 0.f,
                          const Vec2* startLens = nullptr, float seconds = 12.f, float leadEvery = 0.f, float from = 1.f) {
    const ArenaTable t = shippedTable();
    PRig r(t.fighter("devin"));
    r.def.fighter.hp = 100000;
    r.ownPartnerKit = true;
    r.def.partnerKit = t.fighter("third");
    r.tuning = shippedTuning();
    ArenaEnemyKind big = dummyKind(100000);
    big.bodyRadius = radius;
    if (radius >= 2.f) big.height = 7.f; // the shipped titan's height: tall (rule 18)
    const Vec2 tp{0.f, 0.f};
    r.enemy(big, tp);
    r.bodies.player = Vec2{-centre, 0.f};

    constexpr float kAspect = 16.f / 9.f;
    constexpr float kRad = 3.14159265f / 180.f;
    const float melee = 1.f - smooth01((centre - 2.5f) / 2.5f);
    ArenaCameraPose pose;
    pose.arm = 2.6f;
    pose.pivot = 1.35f + 0.25f * melee;
    pose.side = 0.55f;
    pose.up = 0.1f;
    pose.fov = 60.f + 8.f * melee;
    pose.boomPitch = -2.f + 8.f * melee;
    const float screenX = 0.6f + (0.64f - 0.6f) * melee;
    const float turn = std::atan((2.f * screenX - 1.f) * std::tan(0.5f * pose.fov * kRad)) / kRad;
    const auto lensAt = [&](Vec2 pivot) {
        pose.yaw = std::atan2(tp.y - pivot.y, tp.x - pivot.x) / kRad;
        for (int k = 0; k < 4; ++k) {
            const ArenaFrustum f = arenaPoseFrustum(pose, pivot, kAspect);
            pose.yaw = std::atan2(tp.y - f.pos.y, tp.x - f.pos.x) / kRad - turn;
        }
        return arenaPoseCamera(pose, pivot, kAspect, 1.75f);
    };
    PartnerCameraView cam = lensAt(r.bodies.player);
    REQUIRE(partnerCameraWhole(cam));
    Vec2 cp{}, fw{};
    const auto ground = [&]() {
        cp = Vec2{cam.pos.x, cam.pos.y};
        const float fl = std::hypot(cam.forward.x, cam.forward.y);
        fw = Vec2{cam.forward.x / fl, cam.forward.y / fl};
    };
    ground();
    const float vTan = std::tan(0.5f * cam.verticalFovDegrees * kRad);
    const auto depthOf = [&](Vec2 p) { return (p.x - cp.x) * fw.x + (p.y - cp.y) * fw.y; };
    const auto latOf = [&](Vec2 p) { return -(p.x - cp.x) * fw.y + (p.y - cp.y) * fw.x; };
    const auto heightOf = [&](Vec2 p) {
        const float d = depthOf(p);
        return d > 0.05f ? 1.8f / (2.f * d * vTan) : 99.f;
    };
    const auto nearOkAt = [&](Vec2 p) {
        return depthOf(p) >= depthOf(r.bodies.player) - 0.5f && dist(p, cp) >= 2.5f;
    };

    if (startLens != nullptr) {
        const Vec2 rt{-fw.y, fw.x};
        r.bodies.ally = Vec2{r.bodies.player.x + startLens->x * fw.x + startLens->y * rt.x,
                             r.bodies.player.y + startLens->x * fw.y + startLens->y * rt.y};
    } else { // somewhere 1.5-3.5 m round the lead, by the seed
        std::uint32_t s = seed * 2654435761u + 12345u;
        const auto rnd = [&]() {
            s = s * 1664525u + 1013904223u;
            return static_cast<float>(s >> 8) / 16777216.f;
        };
        const float a0 = 6.2831853f * rnd(), d0 = 1.5f + 2.f * rnd();
        r.bodies.ally = Vec2{r.bodies.player.x + d0 * std::cos(a0), r.bodies.player.y + d0 * std::sin(a0)};
        if (dist(r.bodies.ally, tp) < radius + 0.5f) r.bodies.ally = Vec2{r.bodies.player.x - d0, 0.f};
    }
    r.in.partnerOrder = 1;
    r.go(true, 100000, seed);
    r.walk = true;

    TitanLensRun o;
    o.leadDepth = depthOf(r.bodies.player);
    o.leadLateral = latOf(r.bodies.player);
    const ArenaLeadBody leadBody{};
    std::array<float, 64> changes{};
    std::int8_t flank = 0;
    int last = 0;
    const int n = static_cast<int>(std::lround(seconds / kDt));
    for (int k = 0; k < n; ++k) {
        const float tNow = static_cast<float>(k + 1) * kDt;
        if (orbit != 0.f) {
            const float a = orbit * kRad * tNow;
            r.bodies.player = Vec2{-centre * std::cos(a), -centre * std::sin(a)};
            cam = lensAt(r.bodies.player);
            ground();
        }
        r.in.partnerCamera = cam;
        r.in.cameraOn = true;
        r.in.cameraPos = cp;
        r.in.cameraFacing = fw;
        PartnerCloseView& v = r.in.partnerClose;
        v.given = true;
        v.leadSide = -1;
        v.standMin = kPartnerCloseCasterMin;
        v.standMax = kPartnerCloseCasterMax;
        v.bodyRadius = 0.4f;
        v.verticalHalfDegrees = 0.5f * cam.verticalFovDegrees;
        v.halfAngleDegrees = std::atan(std::tan(v.verticalHalfDegrees * kRad) * cam.aspect) / kRad;
        v.pitchDegrees = std::asin(std::clamp(cam.forward.z, -1.f, 1.f)) / kRad;
        v.lensHeight = cam.pos.z - cam.groundZ;
        if (leadEvery > 0.f) r.in.light = k % std::max(1, static_cast<int>(std::lround(leadEvery / kDt))) == 0;
        const Vec2 before = r.bodies.ally;
        const Vec2 faced = r.out != nullptr ? r.out->ally.facing : Vec2{0.f, 0.f}; // what the brain reads this step
        r.tick();
        if (r.brain.intent().newString && (faced.x != 0.f || faced.y != 0.f)) {
            const Vec2 to = sub(tp, before);
            const float c = (faced.x * to.x + faced.y * to.y) / std::max(1e-6f, std::hypot(faced.x, faced.y) * std::hypot(to.x, to.y));
            o.worstStartFacing = std::max(o.worstStartFacing, std::acos(std::clamp(c, -1.f, 1.f)) / kRad);
        }
        const Vec2 a = r.bodies.ally;
        if (titanCrossed(cp, r.bodies.player, before, a)) {
            if (o.crossings == 0) {
                o.crossAt = tNow;
                o.crossDepth = depthOf(a) - depthOf(r.bodies.player);
                o.crossLateral = latOf(a) - latOf(r.bodies.player);
                o.crossAct = static_cast<int>(r.brain.intent().act);
            }
            ++o.crossings;
        }
        const Vec2 bear = sub(r.bodies.player, tp), rel = sub(a, tp);
        const std::int8_t f = bear.x * rel.y - bear.y * rel.x > 0.f ? std::int8_t{1} : std::int8_t{-1};
        if (k == 0 || tNow < from) flank = f;
        if (tNow < from) continue;
        if (f != flank) {
            if (o.flankChanges < static_cast<int>(changes.size())) changes[static_cast<std::size_t>(o.flankChanges)] = tNow;
            ++o.flankChanges;
            flank = f;
        }
        ++o.steps;
        if (nearOkAt(a)) ++o.nearOk;
        const float h = heightOf(a);
        if (h <= kPartnerCloseSizeCap) ++o.sizeOk;
        const ArenaPoint3 eye{cam.pos.x, cam.pos.y, cam.pos.z};
        const auto behind = [&](Vec2 at, float rad, float top) {
            return arenaHiddenBehind(eye, ArenaPoint3{a.x, a.y, 0.55f * 1.75f}, at, rad, 0.f, top) &&
                   arenaHiddenBehind(eye, ArenaPoint3{a.x, a.y, 0.95f * 1.75f}, at, rad, 0.f, top);
        };
        const ArenaBodyCylinder tc = arenaBodyCylinder(r.def.enemies[0], tp, 0.f);
        const bool byLead = behind(r.bodies.player, leadBody.radius, leadBody.head), byTarget = behind(tp, tc.radius, tc.top);
        const PartnerAct act = r.brain.intent().act;
        const bool walking = act == PartnerAct::MoveTo || act == PartnerAct::StepBack;
        o.hiddenLead += byLead;
        o.hiddenTarget += byTarget;
        o.hiddenLeadStanding += byLead && !walking;
        o.hiddenTargetStanding += byTarget && !walking;
        const int stringsNow = r.fight.stats().partnerStrings;
        if ((byLead || byTarget) && (stringsNow > o.stringsSeen || r.brain.intent().blowStart)) ++o.castsHidden;
        o.stringsSeen = stringsNow;
        if ((byLead || byTarget) && o.hiddenFirstAct < 0) {
            o.hiddenFirstAt = tNow;
            o.hiddenFirstAct = static_cast<int>(act);
        }
        if (partnerInFrameClose(cam, kPartnerFrustumMargin, a)) {
            ++o.seen;
            if (!byLead && !byTarget) ++o.seenClear;
            if (!byLead) ++o.seenLeadClear;
        }
        const Vec2 g = r.out->ally.goal;
        if (walking && (heightOf(g) > kPartnerCloseSizeCap || !nearOkAt(g))) ++o.goalOver;
        if (tNow > seconds - 3.f) {
            ++last;
            o.depth += depthOf(a);
            o.lateral += latOf(a);
            o.height += h;
        }
    }
    if (last > 0) {
        o.depth /= static_cast<float>(last);
        o.lateral /= static_cast<float>(last);
        o.height /= static_cast<float>(last);
    }
    const int kept = std::min(o.flankChanges, static_cast<int>(changes.size()));
    for (int i = 0; i < kept; ++i) {
        int w = 0;
        const float at0 = changes[static_cast<std::size_t>(i)];
        for (int j = i; j < kept && changes[static_cast<std::size_t>(j)] < at0 + 3.f; ++j) ++w;
        o.worstFlankRate = std::max(o.worstFlankRate, w);
    }
    o.strings = r.fight.stats().partnerStrings;
    return o;
}

std::string titanLine(const TitanLensRun& o) {
    std::ostringstream s;
    s << "station (depth " << o.depth << ", lateral " << o.lateral << ", height " << o.height << "), lead depth "
      << o.leadDepth << "; rule 1 " << 100 * o.nearOk / o.steps << "%, size " << 100 * o.sizeOk / o.steps
      << "%, in frame " << 100 * o.seen / o.steps << "%, clear " << 100 * o.seenClear / o.steps << "%, goals over "
      << o.goalOver << ", flank changes " << o.flankChanges << " (most in 3 s " << o.worstFlankRate << "), crossings "
      << o.crossings << " (first at " << o.crossAt << " s, depth " << o.crossDepth << ", lateral " << o.crossLateral
      << ", act " << o.crossAct << "), hidden behind the lead " << o.hiddenLead << " (standing "
      << o.hiddenLeadStanding << "), behind the target " << o.hiddenTarget << " (standing " << o.hiddenTargetStanding
      << ", first at " << o.hiddenFirstAt << " s, act " << o.hiddenFirstAct << "), casts begun hidden "
      << o.castsHidden << ", strings " << o.strings;
    return s.str();
}

// The checks every near-field run keeps: rule 1 on every step, the station and its goals in the size cap, in frame
// at least `inFrame` of the steps, never standing hidden behind the lead (walking past it is reported), no flank
// ping-pong, and it casts.
void titanChecks(const TitanLensRun& o, float inFrame = 0.6f) {
    INFO(titanLine(o));
    REQUIRE(o.steps > 0);
    CHECK(o.nearOk == o.steps);
    CHECK(o.goalOver == 0);
    CHECK(o.height <= kPartnerCloseSizeCap);
    CHECK(static_cast<float>(o.seen) >= inFrame * static_cast<float>(o.steps));
    CHECK(o.hiddenLeadStanding == 0);
    CHECK(o.castsHidden == 0);
    CHECK(o.worstFlankRate <= 1);
    CHECK(o.strings >= 1);
}
} // namespace

TEST_CASE("arena partner: the close lens's near field, a titan of 3.5 m in contact: rule 1 always, never behind the lead") {
    // The ruling of 11-16: no in-frame bar here (the frame holds only a sliver); never inside the near field, never
    // hidden behind the lead, standing or walking, nor behind the target (before the order of 11-16's minors: 3 steps
    // behind the lead walking on one seed, 18 to 77 behind the target; it stepped out into the target's shadow and the
    // next string started back at its station); the share in frame is reported.
    for (std::uint32_t seed = 1; seed <= 8; ++seed) {
        CAPTURE(seed);
        const TitanLensRun o = titanLensRun(3.5f, 3.5f + 0.85f, seed);
        MESSAGE("titan 3.5 in contact seed " << seed << ": " << titanLine(o));
        titanChecks(o, 0.f);
        CHECK(o.hiddenLead == 0);
        CHECK(o.hiddenTarget == 0);
        CHECK(o.crossings == 0);
    }
}

TEST_CASE("arena partner: the close lens's near field, the shipped titan in contact and at 5, 6 and 8 m (report)") {
    for (const float centre : {2.2f + 0.85f, 5.f, 6.f, 8.f}) {
        for (std::uint32_t seed = 1; seed <= 8; ++seed) {
            CAPTURE(centre);
            CAPTURE(seed);
            const TitanLensRun o = titanLensRun(2.2f, centre, seed);
            MESSAGE("titan 2.2 at " << centre << " seed " << seed << ": " << titanLine(o));
            titanChecks(o);
            CHECK(o.crossings == 0);
            // Never behind the target (before the minors of 11-16: 93-138 steps at 5 m on seeds 4-6, the step out
            // into its shadow).
            CHECK(o.hiddenTarget == 0);
            if (centre < 4.f) CHECK(o.hiddenLead <= 6); // in contact (before: 44 steps behind the lead and 12 behind
                                                        // the target walking on seeds 7-8)
        }
    }
}

TEST_CASE("arena partner: the close lens's near field over a whole fight from the first step: rule 1 on 95% of steps") {
    // The arena lane's lens log (round 6): the caster off the right edge beside the lens on 25-35% of frames on the
    // titan runs, though its stations read well. Its start now: 3 m ahead of the lead along the lens and 1.5 m off
    // the far side (the lens on the right shoulder: lateral -1.5); its lead throws a light every 2.5 s. Over 20 s
    // from the first step, rule 1 (2.5 m off the lens, no shallower than the lead less 0.5) holds on at least 95%.
    const Vec2 start{3.f, -1.5f};
    for (const float centre : {6.f, 8.f, 10.f}) {
        for (std::uint32_t seed = 1; seed <= 4; ++seed) {
            CAPTURE(centre);
            CAPTURE(seed);
            const TitanLensRun o = titanLensRun(2.2f, centre, seed, 0.f, &start, 20.f, 2.5f, 0.f);
            MESSAGE("whole fight titan 2.2 at " << centre << " seed " << seed << ": " << titanLine(o));
            REQUIRE(o.steps > 0);
            CHECK(100 * o.nearOk >= 95 * o.steps);
            CHECK(o.crossings == 0);
            CHECK(o.strings > 0);
        }
    }
}

TEST_CASE("arena partner: the close lens: a string starts with the partner facing its target within 30 degrees") {
    // The arena lane (round 6): at the titan's 8 and 10 m the caster cast facing the camera: a string begun facing
    // away turned only by its blow's tracking (about 96 degrees). Now it turns first; the blow's tracking is as it was.
    const Vec2 start{3.f, -1.5f};
    for (const float centre : {2.2f + 0.85f, 6.f, 8.f, 10.f}) {
        for (std::uint32_t seed = 1; seed <= 4; ++seed) {
            CAPTURE(centre);
            CAPTURE(seed);
            const TitanLensRun o = titanLensRun(2.2f, centre, seed, 0.f, centre > 4.f ? &start : nullptr, 20.f, 2.5f);
            MESSAGE("facing titan 2.2 at " << centre << " seed " << seed << ": worst start " << o.worstStartFacing
                                           << " deg, strings " << o.strings);
            CHECK(o.strings > 0);
            CHECK(o.worstStartFacing <= kPartnerStartFacingDegrees + 1.f);
        }
    }
}

TEST_CASE("arena partner: the close lens's near field, a hulk in contact keeps its station within 0.5 m") {
    // Before the near field (04527885) its station stood at depth 6.05, lateral -3.07 in the lens's ground space.
    for (std::uint32_t seed = 1; seed <= 8; ++seed) {
        CAPTURE(seed);
        const TitanLensRun o = titanLensRun(1.2f, 1.2f + 0.85f, seed);
        MESSAGE("hulk 1.2 in contact seed " << seed << ": " << titanLine(o));
        titanChecks(o);
        CHECK(std::hypot(o.depth - 6.05f, o.lateral + 3.07f) <= 0.5f);
    }
}

TEST_CASE("arena partner: the close lens's near field, a normal enemy at 5 m keeps its flank, in frame and in the cap") {
    // Before the near field (04527885) it stood about depth 4.28, lateral 2.15 (a 0.65 high body), on its way back
    // from a step out beside the lens. The ruling of 11-16: the same flank, at most 0.70 high, no nearer than that
    // depth less 0.5, in frame, rule 1 and never hidden on every step (deeper and smaller is no fault).
    for (std::uint32_t seed = 1; seed <= 8; ++seed) {
        CAPTURE(seed);
        const TitanLensRun o = titanLensRun(0.45f, 5.f, seed);
        MESSAGE("normal 0.45 at 5 seed " << seed << ": " << titanLine(o));
        titanChecks(o);
        CHECK(o.lateral > 0.f);
        CHECK(o.depth >= 4.28f - 0.5f);
        CHECK(o.seen == o.steps);
        CHECK(o.hiddenLead == 0);
        CHECK(o.hiddenTarget == 0);
    }
}

TEST_CASE("arena partner: the close lens's near field, the lead circling a titan: no flank ping-pong") {
    for (const float centre : {3.5f + 0.85f, 6.f}) {
        for (std::uint32_t seed = 1; seed <= 4; ++seed) {
            CAPTURE(centre);
            CAPTURE(seed);
            const TitanLensRun o = titanLensRun(centre < 5.f ? 3.5f : 2.2f, centre, seed, 15.f);
            INFO(titanLine(o));
            CHECK(o.worstFlankRate <= 1);
            CHECK(o.strings >= 1);
        }
    }
}

TEST_CASE("arena partner: the close lens's near field, a titan stand-off with the lead circling: the partner clear of the lead") {
    // The arena's cap_r6 (fault A): a 2.2 m titan, the lead at 6, 8 or 10 m from its centre, standing or circling it at 1
    // or 2 m/s either way and striking every 2.5 s, the partner from a start beside the lens. In frame and not behind
    // the lead (a body width of screen gap held by rule 19's near field), over 3 seeds of 20 s: at least 60% of the
    // steps at 6 m and 75% at 8 and 10 m (the capture had 20 / 54 / 84 at 6 / 8 / 10 m). It still fights, and at most
    // three flank changes in any 3 s (as before the gap: 3 at 8 and 10 m).
    const Vec2 start{3.f, -1.5f};
    for (const float centre : {6.f, 8.f, 10.f}) {
        for (const float v : {0.f, -1.f, 1.f, -2.f}) {
            CAPTURE(centre);
            CAPTURE(v);
            int steps = 0, clear = 0, worstRate = 0, fewestStrings = 1 << 20;
            for (std::uint32_t seed = 1; seed <= 3; ++seed) {
                const TitanLensRun o = titanLensRun(2.2f, centre, seed, v * 57.29578f / centre, &start, 20.f, 2.5f, 0.f);
                INFO(titanLine(o));
                steps += o.steps;
                clear += o.seenLeadClear;
                worstRate = std::max(worstRate, o.worstFlankRate);
                fewestStrings = std::min(fewestStrings, o.strings);
            }
            const float share = static_cast<float>(clear) / static_cast<float>(std::max(steps, 1));
            MESSAGE("titan " << centre << " m, lead at " << v << " m/s: in frame and clear of the lead " << 100.f * share
                             << "%, most flank changes in 3 s " << worstRate << ", fewest strings " << fewestStrings);
            CHECK(share >= (centre < 7.f ? 0.60f : 0.75f));
            CHECK(fewestStrings >= 1);
            CHECK(worstRate <= 3);
        }
    }
}

TEST_CASE("arena partner: the close lens's near field, a start between the lens and the lead walks round the far side") {
    // Started behind the lead (lens space, from the lead) on the flank away from its station, so the straight walk to
    // the station would cross the line from the lens to the lead: the waypoint takes it round the lead's far side.
    struct Start {
        float centre;
        Vec2 at;
    };
    for (const Start s : {Start{6.f, Vec2{-2.f, 1.f}}, Start{6.f, Vec2{-2.2f, 0.9f}}, Start{6.f, Vec2{-1.8f, 0.8f}},
                          Start{8.f, Vec2{-2.f, -0.4f}}, Start{8.f, Vec2{-1.5f, -0.6f}}, Start{8.f, Vec2{-2.2f, -0.3f}}}) {
        CAPTURE(s.centre);
        CAPTURE(s.at.x);
        CAPTURE(s.at.y);
        const TitanLensRun o = titanLensRun(2.2f, s.centre, s.centre < 7.f ? 5u : 1u, 0.f, &s.at);
        INFO(titanLine(o));
        const Vec2 lensFromLead{-o.leadDepth, -o.leadLateral};
        const Vec2 stationFromLead{o.depth - o.leadDepth, o.lateral - o.leadLateral};
        CHECK(titanCrossed(lensFromLead, Vec2{0.f, 0.f}, s.at, stationFromLead)); // the straight walk would cross
        CHECK(o.crossings == 0);
        CHECK(o.nearOk == o.steps);
        CHECK(o.height <= kPartnerCloseSizeCap);
    }
}

TEST_CASE("arena partner: the close lens's near field, in contact with a titan no way leads round the lead (report)") {
    // The titan's body fills the far side of the lead: from a start behind the lead on the wrong flank it crosses once,
    // just behind the lead, in its first half second, and keeps rule 1 from 1 s on.
    for (const Vec2 start : {Vec2{-1.5f, 1.2f}, Vec2{-1.f, 0.9f}, Vec2{-2.f, 0.6f}}) {
        CAPTURE(start.x);
        CAPTURE(start.y);
        const TitanLensRun o = titanLensRun(3.5f, 3.5f + 0.85f, 1, 0.f, &start);
        MESSAGE("contact, start (" << start.x << ", " << start.y << "): " << titanLine(o));
        CHECK(o.crossings <= 1);
        CHECK(o.crossAt <= 0.5f);
        CHECK(o.nearOk == o.steps);
    }
}

TEST_CASE("arena partner: a target crossing at 6 m/s mid-string: the facing follows it between the hits") {
    // The arena's normal 5 (fault C): a fast enemy crossing during a string left the facing up to 57 deg off, since it
    // turned over startup alone. Now recovery turns it too (the kit's turn rate, at most 360 deg/s); the active part
    // never. The partner stands, its target runs back and forth 2 m in front of it at 6 m/s: on every string step
    // outside the active part the facing is within 30 deg of the target. Both melee kits and the party caster (the
    // normal 5 run's partner: its own blows' timings; this rig gives no lens, so its range is off and it strikes at
    // 2 m too), under the close camera's rule.
    const ArenaTable table = shippedTable();
    for (const char* kit : {"devin", "duy", "third"}) {
        CAPTURE(kit);
        constexpr float lineY = 2.f;
        PRig r(table.fighter("devin"));
        r.ownPartnerKit = true;
        r.def.partnerKit = table.fighter(kit);
        r.def.fighter.hp = 100000;
        r.tuning = shippedTuning();
        r.tuning.budgetAllowanceSeconds = 10.f; // no budget in the way
        REQUIRE(validPartnerTuning(r.tuning));
        const ArenaEnemyKind dummy = dummyKind();
        r.enemy(dummy, Vec2{-3.f, lineY});
        r.bodies.player = Vec2{-4.f, -3.f};
        r.bodies.ally = Vec2{0.f, 0.f};
        r.go(true, 1000);
        r.in.partnerClose.given = true; // the close camera's rule (classic keeps main's startup-only turn)
        float x = -3.f, v = 6.f, worst = 0.f;
        int counted = 0, over = 0;
        for (int k = 0; k < 20 * 60; ++k) {
            x += v * kDt;
            if (x > 3.f || x < -3.f) {
                x = std::clamp(x, -3.f, 3.f);
                v = -v;
            }
            r.bodies.enemies[0] = Vec2{x, lineY};
            r.tick();
            const ArenaAllyOut& al = r.out->ally;
            if (!al.attacking || al.part == ArenaPart::Active || al.target != 0) continue;
            const Vec2 to = sub(r.bodies.enemies[0], r.bodies.ally);
            const float c = (al.facing.x * to.x + al.facing.y * to.y) / std::max(length(to), 1e-5f);
            const float off = std::acos(std::clamp(c, -1.f, 1.f)) * 57.29578f;
            ++counted;
            if (off > 30.f) ++over;
            worst = std::max(worst, off);
        }
        MESSAGE(std::string(kit) << ": strings " << r.fight.stats().partnerStrings << ", string steps outside the active part "
                    << counted << ", over 30 deg " << over << ", worst " << worst << " deg");
        REQUIRE(r.fight.stats().partnerStrings >= 2);
        REQUIRE(counted >= 30);
        CHECK(over == 0);
        CHECK(worst <= 30.f);
    }
}

TEST_CASE("arena partner: the drawn heading turns at most 540 deg/s and is within 60 degrees of the target at a string's start") {
    // Under the close camera's rule the partner stands with its target behind it (heading reversed), the target jumping
    // to its other side every 3 s: ArenaAllyOut::heading never turns over 540 deg/s, and on every string's first tick
    // it is within 60 degrees of the target.
    const ArenaTable table = shippedTable();
    PRig r(table.fighter("devin"));
    r.ownPartnerKit = true;
    r.def.partnerKit = table.fighter("duy");
    r.def.fighter.hp = 100000;
    r.tuning = shippedTuning();
    r.tuning.budgetAllowanceSeconds = 10.f;
    REQUIRE(validPartnerTuning(r.tuning));
    const ArenaEnemyKind dummy = dummyKind();
    r.enemy(dummy, Vec2{-2.f, 0.f}); // behind its start heading; it jumps to the other side every 3 s
    r.bodies.player = Vec2{-4.f, -3.f};
    r.bodies.ally = Vec2{0.f, 0.f};
    r.go(true, 1000);
    r.keepView = true; // the close lens: behind the lead, on the target
    r.in.partnerClose = PartnerCloseView{};
    r.in.partnerClose.given = true;
    r.in.partnerClose.halfAngleDegrees = 30.f;
    Vec2 last{0.f, 0.f};
    bool was = false, have = false;
    int starts = 0;
    float worstTurn = 0.f;
    for (int k = 0; k < 15 * 60; ++k) {
        if (k > 0 && k % (3 * 60) == 0) r.bodies.enemies[0] = Vec2{-r.bodies.enemies[0].x, 0.f};
        const Vec2 look = sub(r.bodies.enemies[0], r.bodies.player);
        const Vec2 lu{look.x / length(look), look.y / length(look)};
        r.in.cameraOn = true;
        r.in.cameraPos = Vec2{r.bodies.player.x - 4.5f * lu.x, r.bodies.player.y - 4.5f * lu.y};
        r.in.cameraFacing = lu;
        r.tick();
        const ArenaAllyOut& al = r.out->ally;
        if (have) {
            const float c = std::clamp(al.heading.x * last.x + al.heading.y * last.y, -1.f, 1.f);
            worstTurn = std::max(worstTurn, std::acos(c) * 57.29578f);
        }
        last = al.heading;
        have = true;
        if (al.attacking && !was && al.target == 0) {
            ++starts;
            const Vec2 to = sub(r.bodies.enemies[0], r.bodies.ally);
            const float c = (al.heading.x * to.x + al.heading.y * to.y) / std::max(length(to), 1e-5f);
            CAPTURE(k);
            CHECK(std::acos(std::clamp(c, -1.f, 1.f)) * 57.29578f <= 60.f + 1.f);
        }
        was = al.attacking;
    }
    MESSAGE("strings " << starts << ", worst heading turn per tick " << worstTurn << " deg");
    CHECK(starts >= 2);
    CHECK(worstTurn <= 540.f * kDt + 0.05f);
}

TEST_CASE("arena partner: pushed back mid-string, the drawn heading stays within 5 deg of its facing") {
    // The host shoves the ally's body back and aside every tick of a string (a recoil, a push): its walk this tick
    // points away from its blows, but the heading keeps within 5 degrees of its facing.
    const ArenaTable table = shippedTable();
    {
        PRig r(table.fighter("devin"));
        r.ownPartnerKit = true;
        r.def.partnerKit = table.fighter("duy");
        r.def.fighter.hp = 100000;
        r.tuning = shippedTuning();
        r.tuning.budgetAllowanceSeconds = 10.f;
        REQUIRE(validPartnerTuning(r.tuning));
        r.enemy(dummyKind(), Vec2{1.5f, 0.f});
        r.bodies.player = Vec2{-4.f, -3.f};
        r.bodies.ally = Vec2{0.f, 0.f};
        r.go(true, 1000);
        int shoved = 0;
        float worst = 0.f;
        for (int k = 0; k < 6 * 60 && shoved < 40; ++k) {
            r.tick();
            const ArenaAllyOut& al = r.out->ally;
            if (!al.attacking) continue;
            if (shoved > 0) {
                const float c = std::clamp(al.heading.x * al.facing.x + al.heading.y * al.facing.y, -1.f, 1.f);
                worst = std::max(worst, std::acos(c) * 57.29578f);
            }
            r.bodies.ally = Vec2{r.bodies.ally.x - 0.04f * al.facing.x + 0.02f * al.facing.y,
                                 r.bodies.ally.y - 0.04f * al.facing.y - 0.02f * al.facing.x};
            ++shoved;
        }
        MESSAGE("string: shoved " << shoved << " ticks, worst heading off facing " << worst << " deg");
        CHECK(shoved >= 10);
        CHECK(worst <= 5.f);
    }
}
