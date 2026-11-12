// A scripted fight that rolls away under lock and guards on the first free frame after, over and over: the rules'
// facing has to face the target there (the drawn one may still be turning), so its outcome is the plain autopilot's
// way of meeting such a player, the same build to build.
#include "ftcore/ArenaFight.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <string>

using namespace ftcore;

namespace {
constexpr float kRollDt = 1.f / 60.f;

struct RollGuardOutcome {
    ArenaPhase phase = ArenaPhase::NotStarted;
    float time = 0.f;
    int hp = 0;
    int damageTaken = 0;
    int hitsTaken = 0;
    int blocks = 0;
    int parries = 0;
    int rolls = 0;
};

RollGuardOutcome playRollGuard(const ArenaDef& def, std::uint32_t seed, float delay) {
    const ArenaRing ring = arenaRingAt(Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, def.ringRadius, ArenaRing{Vec2{0.f, 0.f}, 36.f});
    ArenaFight fight;
    fight.begin(def, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, seed);
    ArenaBodies bodies;
    bodies.player = fight.playerStart();
    for (std::size_t i = 0; i < fight.enemyCount(); ++i) bodies.enemies[i] = fight.enemyStart(i);
    RollGuardOutcome o;
    float nextRoll = 0.f;
    bool rolled = false;  // a roll of the script's is running
    float guardLeft = 0.f; // s of the script's guard left
    while (!fight.over() && fight.time() < 60.f) {
        ArenaInput in{}; // no autopilot: the player only locks, rolls and guards
        const ArenaPlayerOut& p = fight.output().player;
        const bool free = p.action == ArenaAction::Idle || p.action == ArenaAction::Move || p.action == ArenaAction::Guard;
        const std::uint8_t lock = fight.lock();
        if (lock == kArenaNobody) {
            // lock the nearest living enemy: the view toward it
            float best = 1e9f;
            for (std::size_t i = 0; i < fight.enemyCount(); ++i) {
                if (!fight.enemyAlive(i)) continue;
                const float dx = bodies.enemies[i].x - bodies.player.x, dy = bodies.enemies[i].y - bodies.player.y;
                if (dx * dx + dy * dy < best) {
                    best = dx * dx + dy * dy;
                    in.view = Vec2{dx, dy};
                }
            }
            in.lock = !rolled && guardLeft <= 0.f && best < 1e8f;
        } else {
            in.lock = false; // keep it
        }
        if (rolled && p.action != ArenaAction::Dodge) { // the first free frame after the roll: guard
            rolled = false;
            guardLeft = 0.4f;
        }
        if (guardLeft > 0.f) {
            const Vec2 view = in.view;
            in = ArenaInput{};
            in.view = view;
            in.guard = true;
            guardLeft -= kRollDt;
        } else if (!rolled && free && lock != kArenaNobody && fight.time() >= nextRoll &&
                   fight.output().enemies[lock].brain == ArenaBrain::Attack && fight.output().enemies[lock].stateTime >= delay && fight.output().enemies[lock].stateTime < delay + 0.1f) {
            // `delay` s into the locked enemy's swing: roll away, then guard into it
            const Vec2 to{bodies.enemies[lock].x - bodies.player.x, bodies.enemies[lock].y - bodies.player.y};
            const float l = std::sqrt(to.x * to.x + to.y * to.y);
            if (l > 1e-3f) {
                // 150 degrees off the target
                const float c = std::cos(2.618f), s = std::sin(2.618f);
                const Vec2 view = in.view;
                in = ArenaInput{};
                in.view = view;
                in.move = Vec2{(to.x * c - to.y * s) / l, (to.x * s + to.y * c) / l};
                in.dodge = true;
                rolled = true;
                ++o.rolls;
                nextRoll = fight.time() + 0.5f;
            }
        } else if (rolled) {
            const Vec2 view = in.view;
            in = ArenaInput{};
            in.view = view;
        }
        fight.tick(kRollDt, in, bodies);
        arenaStep(fight, bodies, kRollDt);
    }
    o.phase = fight.phase();
    o.time = fight.time();
    o.hp = fight.hp();
    o.damageTaken = fight.stats().damageTaken;
    o.hitsTaken = fight.stats().hitsTaken;
    o.blocks = fight.stats().blocks;
    o.parries = fight.stats().parries;
    return o;
}
} // namespace

TEST_CASE("arena roll and guard script: the built-in fight, rolled away under lock as a swing begins and guarded at once, 8 seeds") {
    ArenaDef def = defaultArenaDef();
    for (std::size_t i = 0; i < def.enemyCount; ++i) def.enemies[i].hp *= 8; // long enough for many rolls
    // a short roll that stays in reach, so the swing it began under lands on the guard
    def.fighter.dodgeDistance = 0.3f;
    def.fighter.dodgeStamina = 0.f;
    if (!(def.fighter.turnRate > 0.f)) def.fighter.turnRate = 14.f;
    int wins = 0, damage = 0, hits = 0, blocks = 0, parries = 0, rolls = 0;
    // the roll starts at 0 to 0.75 s into a swing, so on some of them the blow lands on the first free frames after it
    for (std::uint32_t seed = 1; seed <= 8; ++seed) for (int d = 0; d < 16; ++d) {
        const RollGuardOutcome o = playRollGuard(def, seed, 0.05f * static_cast<float>(d));
        CHECK(o.rolls > 0);
        wins += o.phase == ArenaPhase::Victory ? 1 : 0;
        damage += o.damageTaken;
        hits += o.hitsTaken;
        blocks += o.blocks;
        parries += o.parries;
        rolls += o.rolls;
        if (d == 0) MESSAGE("arena roll and guard: seed " << seed << ": " << std::string(o.phase == ArenaPhase::Victory ? "won" : o.phase == ArenaPhase::Defeat ? "lost" : "unfinished") << " in " << o.time
                                              << " s, hp " << o.hp << ", damage taken " << o.damageTaken << " over " << o.hitsTaken
                                              << " hits, blocks " << o.blocks << ", parries " << o.parries << ", rolls " << o.rolls);
    }
    MESSAGE("arena roll and guard: " << wins << "/128 won, damage taken " << damage << " over " << hits << " hits, blocks " << blocks
                                     << ", parries " << parries << ", rolls " << rolls);
}
