#include "ftcore/FightAnimator.h"

#include "ftcore/BattleDemo.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr double kStep = 1.0 / 120.0;

void setClips(FightAnimator& a) {
    a.setClip(FightClip::Stance, {2.f, 0.f, true});
    a.setClip(FightClip::StrikeLeft, {0.9f, 0.4f, false});
    a.setClip(FightClip::StrikeRight, {0.9f, 0.4f, false});
    a.setClip(FightClip::Finisher, {1.2f, 0.5f, false});
    a.setClip(FightClip::Dodge, {0.7f, 0.2f, false});
    a.setClip(FightClip::Parry, {0.6f, 0.12f, false});
    a.setClip(FightClip::Jump, {0.9f, 0.3f, false});
    a.setClip(FightClip::HitReact, {0.6f, 0.05f, false});
    a.setClip(FightClip::Counter, {1.0f, 0.3f, false});
    a.setClip(FightClip::Downed, {1.5f, 0.f, false});
    a.setClip(FightClip::Victory, {2.f, 0.f, false});
    a.setClip(FightClip::Aim, {1.5f, 0.f, true});
    a.setClip(FightClip::PhaseChange, {1.8f, 0.f, false});
    a.setClip(FightClip::KickLeft, {1.1f, 0.5f, false});
    a.setClip(FightClip::KickRight, {1.1f, 0.5f, false});
    a.setClip(FightClip::WeaponLeft, {1.0f, 0.45f, false});
    a.setClip(FightClip::WeaponRight, {1.0f, 0.45f, false});
    a.setClip(FightClip::Cast, {1.3f, 0.6f, false});
    a.setClip(FightClip::Shot, {0.8f, 0.05f, false});
}

FightAnimator makeAnimator() {
    FightAnimator a;
    setClips(a);
    return a;
}

float weightSum(const FightAnimator& a) {
    float s = 0.f;
    for (std::size_t i = 0; i < a.layerCount(); ++i) s += a.layer(i).weight;
    return s;
}

// The layer playing `clip`, or nullptr.
const FightLayer* find(const FightAnimator& a, FightClip clip) {
    for (std::size_t i = 0; i < a.layerCount(); ++i)
        if (a.layer(i).clip == clip) return &a.layer(i);
    return nullptr;
}

bool isStrike(FightClip c) {
    return c == FightClip::StrikeLeft || c == FightClip::StrikeRight || c == FightClip::Finisher || c == FightClip::KickLeft ||
           c == FightClip::KickRight || c == FightClip::WeaponLeft || c == FightClip::WeaponRight || c == FightClip::Cast;
}

// The layer playing `clip` within `tol` of its key frame, or nullptr.
const FightLayer* onKey(const FightAnimator& a, FightClip clip, double tol) {
    for (std::size_t i = 0; i < a.layerCount(); ++i) {
        const FightLayer& l = a.layer(i);
        if (l.clip == clip && std::abs(l.time - a.clip(clip).key) <= tol) return &l;
    }
    return nullptr;
}

// A strike layer whose time is within `tol` of its key frame.
bool strikeOnKey(const FightAnimator& a, double tol) {
    for (std::size_t i = 0; i < a.layerCount(); ++i) {
        const FightLayer& l = a.layer(i);
        if (isStrike(l.clip) && std::abs(l.time - a.clip(l.clip).key) <= tol) return true;
    }
    return false;
}

} // namespace

TEST_CASE("fight animator: a keyed clip with time to spare starts later, at its own pace") {
    FightAnimator a = makeAnimator();
    REQUIRE(a.play(FightClip::StrikeLeft, 0.0, 1.0));
    a.update(0.5);
    CHECK(a.current() == FightClip::Stance);
    CHECK(find(a, FightClip::StrikeLeft) == nullptr);
    a.update(1.0);
    REQUIRE(find(a, FightClip::StrikeLeft) != nullptr);
    CHECK(find(a, FightClip::StrikeLeft)->time == doctest::Approx(0.4f));
    CHECK(a.current() == FightClip::StrikeLeft);
    CHECK(a.lastKeyTime() == doctest::Approx(1.0));
}

TEST_CASE("fight animator: a short lead plays the clip faster so the key still lands on time") {
    FightAnimator a = makeAnimator();
    a.play(FightClip::StrikeLeft, 0.0, 0.3);
    a.update(0.3);
    REQUIRE(find(a, FightClip::StrikeLeft) != nullptr);
    CHECK(find(a, FightClip::StrikeLeft)->time == doctest::Approx(0.4f));
    a.update(0.15);
    CHECK(find(a, FightClip::StrikeLeft)->time == doctest::Approx(0.2f));
}

TEST_CASE("fight animator: too short a lead joins part way at the top rate; a past beat joins after its key") {
    FightAnimator a = makeAnimator();
    a.play(FightClip::StrikeLeft, 0.0, 0.1);
    a.update(0.0);
    CHECK(find(a, FightClip::StrikeLeft) == nullptr); // just started: nothing faded in yet
    a.update(0.1);
    REQUIRE(find(a, FightClip::StrikeLeft) != nullptr);
    CHECK(find(a, FightClip::StrikeLeft)->time == doctest::Approx(0.4f));

    FightAnimator b = makeAnimator();
    b.play(FightClip::HitReact, 1.0, 0.8);
    b.update(1.05);
    REQUIRE(find(b, FightClip::HitReact) != nullptr);
    CHECK(find(b, FightClip::HitReact)->time == doctest::Approx(0.3f)); // key 0.05 at 0.8: 0.25 later at 1.05
}

TEST_CASE("fight animator: a chained strike waits for the key frame of the one before it") {
    FightAnimator a = makeAnimator();
    a.play(FightClip::StrikeLeft, 0.0, 0.5);  // starts at 0.1, impact at 0.5
    a.play(FightClip::StrikeRight, 0.0, 0.6); // no earlier than 0.5: top rate, joined part way
    CHECK(a.lastKeyTime() == doctest::Approx(0.6));
    a.update(0.45);
    CHECK(find(a, FightClip::StrikeRight) == nullptr);
    CHECK(a.current() == FightClip::StrikeLeft);
    a.update(0.6);
    REQUIRE(find(a, FightClip::StrikeRight) != nullptr);
    CHECK(find(a, FightClip::StrikeRight)->time == doctest::Approx(0.4f));
    CHECK(a.current() == FightClip::StrikeRight);
}

TEST_CASE("fight animator: a one-shot fades in over the stance and back out at its end") {
    FightAnimator a = makeAnimator();
    a.update(0.0);
    REQUIRE(a.layerCount() == 1);
    CHECK(a.layer(0).clip == FightClip::Stance);
    a.play(FightClip::StrikeLeft, 0.0);
    a.update(0.05);
    CHECK(find(a, FightClip::StrikeLeft)->weight == doctest::Approx(0.5f));
    CHECK(find(a, FightClip::Stance)->weight == doctest::Approx(0.5f));
    a.update(0.5);
    CHECK(find(a, FightClip::StrikeLeft)->weight == doctest::Approx(1.f));
    CHECK(find(a, FightClip::Stance) == nullptr);
    a.update(0.825);
    CHECK(find(a, FightClip::StrikeLeft)->weight == doctest::Approx(0.5f));
    CHECK(weightSum(a) == doctest::Approx(1.f));
    a.update(0.95);
    CHECK(a.actionCount() == 0);
    REQUIRE(a.layerCount() == 1);
    CHECK(a.layer(0).clip == FightClip::Stance);
    CHECK(a.current() == FightClip::Stance);
}

TEST_CASE("fight animator: downed holds its last frame for good") {
    FightAnimator a = makeAnimator();
    a.play(FightClip::HitReact, 0.0);
    a.play(FightClip::Downed, 0.1);
    a.update(5.0);
    REQUIRE(a.layerCount() == 1);
    CHECK(a.layer(0).clip == FightClip::Downed);
    CHECK(a.layer(0).time == doctest::Approx(1.5f));
    CHECK(a.layer(0).weight == doctest::Approx(1.f));
    CHECK(a.actionCount() == 1);
}

TEST_CASE("fight animator: a body without the clip plays nothing; the queue never overflows") {
    FightAnimator a;
    a.setClip(FightClip::Stance, {2.f, 0.f, true});
    CHECK_FALSE(a.play(FightClip::Finisher, 0.0, 1.0));
    CHECK(a.actionCount() == 0);

    FightAnimator b = makeAnimator();
    for (int i = 0; i < 20; ++i) b.play(i % 2 ? FightClip::StrikeRight : FightClip::StrikeLeft, 0.0, 1.0 + 0.5 * i);
    CHECK(b.actionCount() == FightAnimator::kMaxActions);
    for (double t = 0.0; t < 12.0; t += kStep) {
        b.update(t);
        CHECK(weightSum(b) == doctest::Approx(1.f));
    }
    CHECK(b.actionCount() == 0);
}

TEST_CASE("fight animator: looping aim plays until a stance covers it") {
    FightAnimator a = makeAnimator();
    a.play(FightClip::Aim, 0.0);
    a.update(4.0);
    CHECK(a.current() == FightClip::Aim);
    CHECK(find(a, FightClip::Aim)->time == doctest::Approx(1.0f)); // 4.0 into a 1.5 s loop
    a.play(FightClip::Stance, 4.0);
    a.update(4.2);
    CHECK(a.current() == FightClip::Stance);
    CHECK(a.actionCount() == 1); // the aim is covered and gone
}

namespace {

// The demo fight on a 120 Hz clock with the perfect pilot, a director animating it.
struct DemoFight {
    Battle b{demo::party(), demo::enemies(), demo::tuning()};
    FightDirector dir;
    double now = 0.0;
    DefenceInput defence = DefenceInput::Parry;

    DemoFight() {
        dir.bind(b);
        for (std::size_t i = 0; i < dir.size(); ++i) setClips(dir.animator(static_cast<CombatantId>(i)));
        b.start(now);
    }

    bool over() const { return b.phase() == Phase::Victory || b.phase() == Phase::Defeat || now > 600.0; }

    // One frame. `seen` gets each event of the frame before they are cleared.
    template <class F>
    void step(F&& seen) {
        if (b.phase() == Phase::AwaitingCommand) {
            const demo::PilotChoice c = demo::pilotChoice(b);
            REQUIRE(b.command(c.skill, c.target, now) == CommandError::None);
        }
        demo::pilotPress(b, now);
        if (demo::pilotDefend(b, now + kStep, defence) > 0) dir.pressed(defence, now);
        now += kStep;
        b.update(now);
        dir.update(b, now);
        for (const BattleEvent& e : b.events()) seen(e);
        b.clearEvents();
    }
    void step() {
        step([](const BattleEvent&) {});
    }
};

} // namespace

TEST_CASE("fight director: each combo note is met by a strike's impact, the last by the finisher") {
    DemoFight f;
    int checked = 0;
    bool finisher = false;
    while (!f.over() && checked < 12) {
        const bool party = f.b.phase() == Phase::PartySkill;
        const CombatantId striker = f.b.actor();
        std::size_t next = party ? f.b.combo().nextIndex() : 0;
        const double noteTime = party && next < f.b.combo().size() ? f.b.combo().note(next).time : -1.0;
        const bool last = party && next + 1 == f.b.combo().size();
        f.step();
        if (noteTime < 0.0 || f.now < noteTime || f.now - noteTime > kStep) continue;
        // The first frame at or past the note: a strike plays at (or a frame past) its key.
        CHECK(strikeOnKey(f.dir.animator(striker), 1.6 * kStep + 1e-4));
        if (last) finisher = finisher || find(f.dir.animator(striker), FightClip::Finisher) != nullptr;
        ++checked;
    }
    CHECK(checked >= 6);
    CHECK(finisher);
}

TEST_CASE("fight animator: a strike pattern takes punches, kicks, swings and casts only, 1 to 8 of them") {
    FightAnimator a;
    CHECK(a.patternSize() == 2);
    CHECK(a.patternStrike(0) == FightClip::StrikeLeft);
    CHECK(a.patternStrike(3) == FightClip::StrikeRight);
    const FightClip armed[] = {FightClip::WeaponLeft, FightClip::WeaponRight, FightClip::Cast, FightClip::KickLeft};
    REQUIRE(a.setPattern(armed, 4));
    CHECK(a.patternStrike(6) == FightClip::Cast);
    const FightClip kicks[] = {FightClip::StrikeLeft, FightClip::KickRight, FightClip::StrikeRight};
    REQUIRE(a.setPattern(kicks, 3));
    CHECK(a.patternStrike(4) == FightClip::KickRight);
    const FightClip bad[] = {FightClip::StrikeLeft, FightClip::Finisher};
    CHECK_FALSE(a.setPattern(bad, 2));
    const FightClip shot[] = {FightClip::Shot};
    CHECK_FALSE(a.setPattern(shot, 1));
    CHECK_FALSE(a.setPattern(kicks, 0));
    const FightClip many[FightAnimator::kMaxPattern + 1] = {};
    CHECK_FALSE(a.setPattern(many, FightAnimator::kMaxPattern + 1));
    CHECK(a.patternSize() == 3); // unchanged by the refusals
    a.reset();
    CHECK(a.patternStrike(1) == FightClip::KickRight);
}

TEST_CASE("fight director: combo notes follow the striker's pattern, a missing kick thrown as the punch on its side") {
    DemoFight f;
    const FightClip pattern[] = {FightClip::KickLeft, FightClip::StrikeRight, FightClip::KickRight};
    for (std::size_t i = 0; i < f.dir.size(); ++i) {
        FightAnimator& a = f.dir.animator(static_cast<CombatantId>(i));
        REQUIRE(a.setPattern(pattern, 3));
        a.setClip(FightClip::KickLeft, {}); // no left kick: the left punch stands in
    }
    int checked = 0;
    while (!f.over() && checked < 12) {
        const bool party = f.b.phase() == Phase::PartySkill;
        const CombatantId striker = f.b.actor();
        const std::size_t next = party ? f.b.combo().nextIndex() : 0;
        const double noteTime = party && next < f.b.combo().size() ? f.b.combo().note(next).time : -1.0;
        const bool last = party && next + 1 == f.b.combo().size();
        f.step();
        if (noteTime < 0.0 || last || f.now < noteTime || f.now - noteTime > kStep) continue;
        const FightClip want = next % 3 == 0 ? FightClip::StrikeLeft : next % 3 == 1 ? FightClip::StrikeRight : FightClip::KickRight;
        CHECK(onKey(f.dir.animator(striker), want, 1.6 * kStep + 1e-4) != nullptr);
        ++checked;
    }
    CHECK(checked >= 6);
}

TEST_CASE("fight director: a blade's swings and a caster's spells land on the notes; a missing swing is the punch on its side") {
    DemoFight f;
    const FightClip pattern[] = {FightClip::WeaponLeft, FightClip::WeaponRight, FightClip::Cast};
    for (std::size_t i = 0; i < f.dir.size(); ++i) {
        FightAnimator& a = f.dir.animator(static_cast<CombatantId>(i));
        REQUIRE(a.setPattern(pattern, 3));
        a.setClip(FightClip::WeaponRight, {}); // no right swing: the right punch stands in
    }
    int checked = 0;
    while (!f.over() && checked < 12) {
        const bool party = f.b.phase() == Phase::PartySkill;
        const CombatantId striker = f.b.actor();
        const std::size_t next = party ? f.b.combo().nextIndex() : 0;
        const double noteTime = party && next < f.b.combo().size() ? f.b.combo().note(next).time : -1.0;
        const bool last = party && next + 1 == f.b.combo().size();
        f.step();
        if (noteTime < 0.0 || last || f.now < noteTime || f.now - noteTime > kStep) continue;
        const FightClip want = next % 3 == 0 ? FightClip::WeaponLeft : next % 3 == 1 ? FightClip::StrikeRight : FightClip::Cast;
        CHECK(onKey(f.dir.animator(striker), want, 1.6 * kStep + 1e-4) != nullptr);
        ++checked;
    }
    CHECK(checked >= 6);
}

TEST_CASE("fight director: the aimed shot fires on its hit, and without a shot clip the aim gives way to the stance") {
    for (const bool withShot : {true, false}) {
        CAPTURE(withShot);
        DemoFight f;
        if (!withShot)
            for (std::size_t i = 0; i < f.dir.size(); ++i) f.dir.animator(static_cast<CombatantId>(i)).setClip(FightClip::Shot, {});
        while (f.b.phase() != Phase::AwaitingCommand) f.step();
        const demo::PilotChoice c = demo::pilotChoice(f.b);
        const CombatantId shooter = f.b.actor();
        REQUIRE(f.b.aim(c.target, f.now) == CommandError::None);
        for (int i = 0; i < 30; ++i) f.step();
        CHECK(f.dir.animator(shooter).current() == FightClip::Aim);
        const double firedAt = f.now;
        REQUIRE(demo::pilotShoot(f.b, f.b.combatant(c.target).weakPoints.best(), f.now));
        f.step();
        f.step(); // the first frame of a fade-in has no weight
        const FightAnimator& a = f.dir.animator(shooter);
        CHECK(find(f.dir.animator(c.target), FightClip::HitReact) != nullptr);
        if (withShot) {
            CHECK(a.current() == FightClip::Shot);
            // Its key met the shot: it runs on from the key by the time since.
            const FightLayer* s = find(a, FightClip::Shot);
            REQUIRE(s != nullptr);
            CHECK(s->time == doctest::Approx(0.05 + (f.now - firedAt)).epsilon(0.02));
            // The shot covers the aim for good: once it ends the shooter is in the stance, not back in the aim.
            while (f.now < firedAt + 1.0) f.step();
            CHECK(a.current() == FightClip::Stance);
            CHECK(find(a, FightClip::Aim) == nullptr);
        } else {
            CHECK(a.current() == FightClip::Stance);
        }
    }
}

TEST_CASE("fight director: parries meet the impacts they answer, then the counter strikes before the reaction") {
    DemoFight f;
    bool parried = false, countered = false;
    double counterAt = -1.0;
    CombatantId counterer = 0, countered_ = 0;
    while (!f.over() && !countered) {
        const bool enemy = f.b.phase() == Phase::EnemySkill;
        const CombatantId defender = f.b.defender();
        const std::size_t next = enemy ? f.b.defence().nextIndex() : 0;
        const double impact = enemy && next < f.b.defence().size() ? f.b.defence().impact(next) : -1.0;
        f.step([&](const BattleEvent& e) {
            if (e.type == EventType::Counter) {
                countered = true;
                counterAt = f.now;
                counterer = e.source;
                countered_ = e.target;
            }
        });
        if (impact >= 0.0 && !parried) {
            // Three frames past the impact the parry is fading in, its key frame met at the impact.
            while (f.now < impact + 3 * kStep) f.step();
            const FightLayer* p = find(f.dir.animator(defender), FightClip::Parry);
            REQUIRE(p != nullptr);
            CHECK(p->time == doctest::Approx(0.12 + (f.now - impact)).epsilon(0.02));
            parried = true;
        }
    }
    REQUIRE(parried);
    REQUIRE(countered);
    CHECK(f.dir.animator(counterer).current() == FightClip::Counter);
    // The reaction (key 0.05) is timed to the counter's key frame (0.3 s in).
    bool reacted = false;
    while (f.now < counterAt + 0.5) {
        f.step();
        const bool reacting = f.dir.animator(countered_).current() == FightClip::HitReact;
        if (reacting && !reacted) CHECK(f.now >= counterAt + 0.25 - kStep);
        reacted = reacted || reacting;
    }
    CHECK(reacted);
}

TEST_CASE("fight director: a perfect dodge shows as a dodge") {
    DemoFight f;
    f.defence = DefenceInput::Dodge;
    bool dodged = false;
    while (!f.over() && !dodged) {
        f.step([&](const BattleEvent& e) {
            if (e.type == EventType::Defended && e.label == "PERFECT") dodged = true;
        });
    }
    REQUIRE(dodged);
    bool shown = false;
    for (int i = 0; i < 60 && !shown; ++i) {
        f.step();
        for (std::size_t c = 0; c < f.dir.size(); ++c)
            shown = shown || f.dir.animator(static_cast<CombatantId>(c)).current() == FightClip::Dodge;
    }
    CHECK(shown);
}

TEST_CASE("fight director: a whole fight animates without allocating, the downed down and the victors cheering") {
    DemoFight f;
    // One fighter leans on kicks.
    const FightClip kicker[] = {FightClip::StrikeLeft, FightClip::KickRight, FightClip::StrikeRight, FightClip::KickLeft};
    REQUIRE(f.dir.animator(0).setPattern(kicker, 4));
    const bool hooked = ftcore_test::allocHookInstalled();
    std::uint64_t allocs = 0;
    while (!f.over()) {
        if (f.b.phase() == Phase::AwaitingCommand) {
            const demo::PilotChoice c = demo::pilotChoice(f.b);
            REQUIRE(f.b.command(c.skill, c.target, f.now) == CommandError::None);
        }
        demo::pilotPress(f.b, f.now);
        demo::pilotDefend(f.b, f.now + kStep, f.defence);
        f.now += kStep;
        f.b.update(f.now);
        const auto before = ftcore_test::allocStats().count;
        f.dir.update(f.b, f.now);
        allocs += ftcore_test::allocStats().count - before;
        for (std::size_t i = 0; i < f.dir.size(); ++i) CHECK(weightSum(f.dir.animator(static_cast<CombatantId>(i))) == doctest::Approx(1.f));
        f.b.clearEvents();
    }
    REQUIRE(f.b.phase() == Phase::Victory);
    if (hooked) CHECK(allocs == 0);
    for (int i = 0; i < 240; ++i) {
        f.now += kStep;
        f.dir.update(f.b, f.now);
    }
    for (std::size_t i = 0; i < f.dir.size(); ++i) {
        const Combatant& c = f.b.combatants()[i];
        const FightClip now = f.dir.animator(static_cast<CombatantId>(i)).current();
        if (!c.alive()) CHECK(now == FightClip::Downed);
        else if (c.team == Team::Party) CHECK(now == FightClip::Victory);
    }
}

TEST_CASE("fight clip names") {
    CHECK(std::string(fightClipName(FightClip::StrikeLeft)) == "strike left");
    CHECK(std::string(fightClipName(FightClip::PhaseChange)) == "phase change");
    CHECK(std::string(fightClipName(FightClip::KickRight)) == "kick right");
    CHECK(std::string(fightClipName(FightClip::WeaponLeft)) == "weapon left");
    CHECK(std::string(fightClipName(FightClip::Cast)) == "cast");
    CHECK(std::string(fightClipName(FightClip::Shot)) == "shot");
    CHECK(std::string(fightClipName(FightClip::Count)) == "?");
}
