#include "ftcore/UiBudget.h"

#include <doctest/doctest.h>

using namespace ftcore;

namespace {

constexpr std::size_t K(UiMarkKind k) { return static_cast<std::size_t>(k); }
constexpr UiPromptMask kAllPrompts = static_cast<UiPromptMask>((1u << kUiPromptCount) - 1u);

UiPromptReasons allWhy(PressReason r) {
    UiPromptReasons why{};
    why.fill(r);
    return why;
}

ArenaHudFrame fullFrame() {
    ArenaHudFrame f;
    f.lead.hpMax = 100;
    f.lead.hp = 80;
    f.lead.loadMax = 90;
    f.lead.releaseReady = true;
    f.lead.heals = 2;
    f.lead.roundsMax = 6;
    f.lead.rounds = 6; // loaded
    f.partner.present = true;
    f.partner.hpMax = 80;
    f.partner.hp = 60;
    f.boss.present = true;
    f.boss.hp = f.boss.hpMax = 500;
    f.boss.slot = 0;
    f.target.present = true;
    f.target.hp = f.target.hpMax = 40;
    f.target.enemy = 1;
    return f;
}

UiBudgetInput busy(const ArenaHudFrame& f) {
    UiBudgetInput in;
    in.dt = 1.f / 60.f;
    in.activity = true;
    in.bossEvent = true;
    in.prompts = uiPromptBit(UiPrompt::Heal) | uiPromptBit(UiPrompt::Pair); // the fixed rows a two-lead fight has
    in.want = uiMarkWants(f, in.prompts);
    return in;
}

// Every prompt's answer one reason.
UiPromptReasons all(PressReason r) {
    UiPromptReasons w{};
    w.fill(r);
    return w;
}

constexpr UiPromptMask kBasic = static_cast<UiPromptMask>(uiPromptBit(UiPrompt::Attack) | uiPromptBit(UiPrompt::Guard) |
                                                          uiPromptBit(UiPrompt::Dodge) | uiPromptBit(UiPrompt::Jump));

} // namespace

TEST_CASE("ui budget: absent data asks for no mark") {
    ArenaHudFrame f; // nothing present
    const auto w = uiMarkWants(f, 0);
    for (std::uint8_t n : w) CHECK(n == 0);

    // The third place with no data (planned) draws nothing, though it is "present".
    f.lead.hp = f.lead.hpMax = 10;
    f.third.present = true;
    f.third.planned = true;
    CHECK(uiMarkWants(f, 0)[K(UiMarkKind::Party)] == 0);
    f.third.planned = false;
    CHECK(uiMarkWants(f, 0)[K(UiMarkKind::Party)] == 1);

    // No cylinder, no rounds mark; no boss hp, no boss line; the target that is the boss is not a second line.
    ArenaHudFrame g = fullFrame();
    g.lead.roundsMax = 0;
    g.boss.hpMax = 0;
    g.target.enemy = g.boss.slot;
    const auto wg = uiMarkWants(g, 0);
    CHECK(wg[K(UiMarkKind::Rounds)] == 0);
    CHECK(wg[K(UiMarkKind::Boss)] == 0);
    g.boss.hpMax = 500;
    CHECK(uiMarkWants(g, 0)[K(UiMarkKind::Target)] == 0);
}

TEST_CASE("ui budget: the partner's row shows whenever the fight has a partner") {
    ArenaHudFrame f = fullFrame();
    CHECK(uiMarkWants(f, 0)[K(UiMarkKind::Party)] == 1); // hurt
    f.partner.hp = f.partner.hpMax;
    CHECK(uiMarkWants(f, 0)[K(UiMarkKind::Party)] == 1); // full health too
    f.partner.down = true;
    CHECK(uiMarkWants(f, 0)[K(UiMarkKind::Party)] == 1);
    f.partner.present = false;
    CHECK(uiMarkWants(f, 0)[K(UiMarkKind::Party)] == 0);
}

TEST_CASE("ui budget: a prompt is hidden only when its action is not in the fight; refused, it stands dimmed") {
    // Every reason but Absent keeps the prompt in its row; only the refusals dim it (Busy waits in the buffer).
    for (std::size_t r = 0; r < kPressReasonCount; ++r) {
        const PressReason why = static_cast<PressReason>(r);
        CAPTURE(r);
        const UiPromptMask present = uiPromptsPresent(all(why));
        const UiPromptMask dimmed = uiPromptsDimmed(all(why), 0);
        CHECK(present == (why == PressReason::Absent ? 0 : kAllPrompts));
        CHECK(dimmed == (pressRefused(why) ? kAllPrompts : 0));
    }
    CHECK_FALSE(pressRefused(PressReason::Ok));
    CHECK_FALSE(pressRefused(PressReason::Busy));
    CHECK_FALSE(pressRefused(PressReason::Absent));
    // Each prompt on its own: one refused, one Absent, the rest Ok.
    for (std::size_t p = 0; p < kUiPromptCount; ++p) {
        CAPTURE(p);
        const UiPromptMask bit = uiPromptBit(static_cast<UiPrompt>(p));
        UiPromptReasons why = all(PressReason::Ok);
        why[p] = PressReason::Cooldown;
        CHECK(uiPromptsPresent(why) == kAllPrompts);
        CHECK(uiPromptsDimmed(why, 0) == bit);
        why[p] = PressReason::Absent;
        CHECK(uiPromptsPresent(why) == static_cast<UiPromptMask>(kAllPrompts & ~bit));
        CHECK(uiPromptsDimmed(why, 0) == 0);
    }
    // A prompt whose action has no key (the import left it unbound: NO KEY) is dimmed whatever its press would do.
    const UiPromptMask heal = uiPromptBit(UiPrompt::Heal);
    CHECK(uiPromptsDimmed(all(PressReason::Ok), heal) == heal);
    CHECK(uiPromptsDimmed(all(PressReason::Busy), heal) == heal);
    // At most one mark per row is asked for, however many prompts stand.
    ArenaHudFrame f = fullFrame();
    CHECK(uiMarkWants(f, kAllPrompts)[K(UiMarkKind::Prompt)] == kUiPromptRows);
    CHECK(uiMarkWants(f, uiPromptBit(UiPrompt::Dodge))[K(UiMarkKind::Prompt)] == 1);
}

TEST_CASE("ui budget: the basic controls' rows stand while the first-use teaching holds them, dimmed when refused") {
    CHECK(kBasic == kUiBasicPrompts);
    const UiPromptReasons ok = all(PressReason::Ok);
    const UiPromptMask present = uiPromptsPresent(ok);
    CHECK(present == kAllPrompts); // the rules' query alone knows no teaching: every action in the fight
    const ShowBasicControls first = ShowBasicControls::FirstFights; // the default policy
    CHECK((uiPromptsTaught(present, first, 0) & kBasic) == 0); // no teaching: no basic control rows
    CHECK((uiPromptsTaught(present, first, kAllPrompts) & kBasic) == kBasic);
    CHECK(uiPromptsTaught(present, first, uiPromptBit(UiPrompt::Dodge)) ==
          static_cast<UiPromptMask>((kAllPrompts & ~kBasic) | uiPromptBit(UiPrompt::Dodge)));
    // The fixed rows do not wait for the teaching.
    CHECK((uiPromptsTaught(present, first, 0) & ~kBasic) == (kAllPrompts & ~kBasic));
    // A guard break: every control still stands while taught, each dimmed with its reason.
    UiPromptReasons broken = ok;
    for (UiPrompt p : {UiPrompt::Attack, UiPrompt::Guard, UiPrompt::Dodge, UiPrompt::Jump})
        broken[static_cast<std::size_t>(p)] = PressReason::Broken;
    CHECK((uiPromptsTaught(uiPromptsPresent(broken), first, kBasic) & kBasic) == kBasic);
    CHECK(uiPromptsDimmed(broken, 0) == kBasic);
    // A kit with no jump: JUMP is not there even while taught, nor under Always.
    UiPromptReasons noJump = ok;
    noJump[static_cast<std::size_t>(UiPrompt::Jump)] = PressReason::Absent;
    CHECK((uiPromptsTaught(uiPromptsPresent(noJump), first, kBasic) & uiPromptBit(UiPrompt::Jump)) == 0);
    CHECK((uiPromptsTaught(uiPromptsPresent(noJump), ShowBasicControls::Always, kBasic) & uiPromptBit(UiPrompt::Jump)) == 0);
}

TEST_CASE("ui budget: the basic controls' policy is the player's: first fights, always or never") {
    const UiPromptMask present = uiPromptsPresent(all(PressReason::Ok));
    const UiPromptMask others = static_cast<UiPromptMask>(kAllPrompts & ~kBasic);
    for (UiPromptMask teaching : {UiPromptMask{0}, uiPromptBit(UiPrompt::Guard), kBasic}) {
        CAPTURE(teaching);
        // Always: every basic control the fight has, taught or not. Never: none, even while the teaching holds them.
        CHECK(uiPromptsTaught(present, ShowBasicControls::Always, teaching) == kAllPrompts);
        CHECK(uiPromptsTaught(present, ShowBasicControls::Never, teaching) == others);
        CHECK(uiPromptsTaught(present, ShowBasicControls::FirstFights, teaching) ==
              static_cast<UiPromptMask>(others | (teaching & kBasic)));
    }
    // The policy never adds a prompt the rules left out, and never touches the other prompts.
    UiPromptReasons why = all(PressReason::Absent);
    why[static_cast<std::size_t>(UiPrompt::Heal)] = PressReason::Ok;
    why[static_cast<std::size_t>(UiPrompt::Attack)] = PressReason::Cooldown;
    const UiPromptMask some = uiPromptsPresent(why);
    CHECK(some == static_cast<UiPromptMask>(uiPromptBit(UiPrompt::Heal) | uiPromptBit(UiPrompt::Attack)));
    CHECK(uiPromptsTaught(some, ShowBasicControls::Always, 0) == some);
    CHECK(uiPromptsTaught(some, ShowBasicControls::Never, kBasic) == uiPromptBit(UiPrompt::Heal));
    CHECK(uiPromptsTaught(0, ShowBasicControls::Always, kBasic) == 0);
}

TEST_CASE("ui budget: at rest only the lead, the party and the prompts show") {
    const UiBudgetRules rules;
    UiBudgetState st;
    ArenaHudFrame f = fullFrame();
    UiBudgetInput in = busy(f);
    in.want[K(UiMarkKind::Event)] = 2;
    in.want[K(UiMarkKind::Tell)] = 1;
    uiBudgetStep(st, in, rules);
    CHECK_FALSE(st.out.rest);

    // Nothing changes for the rest time (the data still asks for everything).
    in.activity = false;
    in.bossEvent = false;
    in.changed = 0;
    for (int i = 0; i < 60 * 5; ++i) uiBudgetStep(st, in, rules);
    const UiBudgetOut& o = st.out;
    CHECK(o.rest);
    CHECK(o.allowed(UiMarkKind::Lead) == 1);
    CHECK(o.allowed(UiMarkKind::Party) == 1);
    CHECK(o.allowed(UiMarkKind::Prompt) == 2); // a prompt stands for as long as its action is in the fight
    CHECK(o.promptsShown == in.prompts);
    CHECK(o.marks == 4);
    CHECK(o.bossAlpha == 0.f);

    // One change wakes it.
    in.changed = 1;
    uiBudgetStep(st, in, rules);
    CHECK_FALSE(st.out.rest);
}

TEST_CASE("ui budget: at rest the marks fade out, they do not pop") {
    const UiBudgetRules rules;
    UiBudgetState st;
    ArenaHudFrame f = fullFrame();
    UiBudgetInput in = busy(f);
    in.want[K(UiMarkKind::Event)] = 1;
    for (int i = 0; i < 30; ++i) uiBudgetStep(st, in, rules);
    CHECK(st.out.actionAlpha == doctest::Approx(1.f));
    in.activity = false;
    in.bossEvent = false;
    in.changed = 0;
    // A little past the rest time: resting, but everything still on screen and fading.
    const int restTicks = static_cast<int>(rules.rest * 60.f) + 12;
    for (int i = 0; i < restTicks; ++i) uiBudgetStep(st, in, rules);
    const UiBudgetOut& o = st.out;
    CHECK(o.rest);
    CHECK(o.actionAlpha > 0.f);
    CHECK(o.actionAlpha < 1.f);
    CHECK(o.bossAlpha > 0.f);
    CHECK(o.bossAlpha < 1.f);
    CHECK(o.allowed(UiMarkKind::Event) == 1);
    // After the fade-out only the lead, the party and the prompts.
    for (int i = 0; i < 60; ++i) uiBudgetStep(st, in, rules);
    CHECK(st.out.actionAlpha == 0.f);
    CHECK(st.out.marks == 4);
}

TEST_CASE("ui budget: the boss line fades at rest and returns on a boss event") {
    const UiBudgetRules rules;
    UiBudgetState st;
    ArenaHudFrame f = fullFrame();
    UiBudgetInput in = busy(f);
    for (int i = 0; i < 30; ++i) uiBudgetStep(st, in, rules);
    CHECK(st.out.bossAlpha > 0.9f);

    // The lead keeps acting but the boss does nothing: its line goes after the rest time.
    in.bossEvent = false;
    for (int i = 0; i < 60 * 5; ++i) uiBudgetStep(st, in, rules);
    CHECK_FALSE(st.out.rest);
    CHECK(st.out.bossAlpha == 0.f);
    CHECK(st.out.allowed(UiMarkKind::Boss) == 0);

    in.bossEvent = true;
    for (int i = 0; i < 12; ++i) uiBudgetStep(st, in, rules);
    CHECK(st.out.bossAlpha > 0.5f);
    CHECK(st.out.allowed(UiMarkKind::Boss) == 1);
}

TEST_CASE("ui budget: never more than the mark cap in action, highest priority kept, the prompt rows all drawn") {
    const UiBudgetRules rules;
    ArenaHudFrame f = fullFrame();
    // Every mix of the widget's own wants.
    for (int tells = 0; tells <= 4; ++tells) {
        for (int pops = 0; pops <= 8; pops += 2) {
            for (int foes = 0; foes <= 6; foes += 3) {
                for (int extra = 0; extra < 8; ++extra) {
                    UiBudgetState st;
                    UiBudgetInput in = busy(f);
                    in.want[K(UiMarkKind::Tell)] = static_cast<std::uint8_t>(tells);
                    in.want[K(UiMarkKind::Event)] = static_cast<std::uint8_t>(pops);
                    in.want[K(UiMarkKind::Foe)] = static_cast<std::uint8_t>(foes);
                    in.want[K(UiMarkKind::Cards)] = (extra & 1) ? 1 : 0;
                    in.want[K(UiMarkKind::Lock)] = (extra & 2) ? 1 : 0;
                    in.want[K(UiMarkKind::WeakPoint)] = (extra & 4) ? 1 : 0;
                    in.prompts = kAllPrompts; // every prompt open: the controls of a first fight too
                    in.want[K(UiMarkKind::Prompt)] = kUiPromptRows;
                    for (int t = 0; t < 20; ++t) uiBudgetStep(st, in, rules);
                    const UiBudgetOut& o = st.out;
                    // The cap holds for every mark but the fixed prompt rows, which are never dropped.
                    CHECK(o.marks - o.allowed(UiMarkKind::Prompt) <= rules.marks);
                    CHECK(o.allowed(UiMarkKind::Lead) == 1);
                    CHECK(o.allowed(UiMarkKind::Tell) == tells); // a tell is never dropped
                    int sum = 0;
                    for (std::uint8_t a : o.allow) sum += a;
                    CHECK(sum == o.marks);
                    // Every prompt drawn, every row granted (the GIFT row's two prompts in one row).
                    CHECK(o.promptsShown == kAllPrompts);
                    CHECK(o.allowed(UiMarkKind::Prompt) == kUiPromptRows);
                    CHECK(uiPromptRowsOf(o.promptsShown) == o.allowed(UiMarkKind::Prompt));
                    CHECK(st.maxMarks <= rules.marks);
                }
            }
        }
    }
}

TEST_CASE("ui budget: each prompt owns a fixed row, counted up from the bottom") {
    CHECK(uiPromptRow(UiPrompt::Heal) == 0);
    CHECK(uiPromptRow(UiPrompt::GiftHeal) == 1);
    CHECK(uiPromptRow(UiPrompt::GiftGuard) == 1);
    CHECK(uiPromptRow(UiPrompt::Pair) == 2); // no RELOAD row: the rules have no reload press
    CHECK(uiPromptRow(UiPrompt::Attack) == 3);
    CHECK(uiPromptRow(UiPrompt::Guard) == 4);
    CHECK(uiPromptRow(UiPrompt::Dodge) == 5);
    CHECK(uiPromptRow(UiPrompt::Jump) == 6);
    CHECK(kUiPromptCount == 8u);
    CHECK(kUiPromptRows == 7u);
    // The rows rise with the order, so the budget's "first in order" is "lowest rows first".
    for (std::size_t p = 1; p < kUiPromptCount; ++p)
        CHECK(uiPromptRow(static_cast<UiPrompt>(p)) >= uiPromptRow(static_cast<UiPrompt>(p - 1)));

    // HEAL, PARTNER and the basic controls together: all drawn, none packed down into an empty row.
    const UiBudgetRules rules;
    UiBudgetState st;
    ArenaHudFrame f = fullFrame();
    UiBudgetInput in = busy(f);
    in.prompts = uiPromptBit(UiPrompt::Heal) | uiPromptBit(UiPrompt::Pair) | uiPromptBit(UiPrompt::Guard) |
                 uiPromptBit(UiPrompt::Dodge);
    in.want = uiMarkWants(f, in.prompts);
    uiBudgetStep(st, in, rules);
    CHECK(st.out.promptsShown == in.prompts);
    in.prompts = 0;
    in.want = uiMarkWants(f, 0);
    uiBudgetStep(st, in, rules);
    CHECK(st.out.promptsShown == 0);
}

TEST_CASE("ui budget: a first fight's prompts never take a call word's, Read row's or lock's place") {
    const UiBudgetRules rules;
    UiBudgetState st;
    ArenaHudFrame f = fullFrame();
    UiBudgetInput in = busy(f);
    // Every prompt standing (the basic controls of a first fight too), one tell, the Read row, the lock and an event.
    in.prompts = uiPromptsDrawable(kAllPrompts, kAllPrompts, {});
    in.want = uiMarkWants(f, in.prompts);
    in.want[K(UiMarkKind::Tell)] = 1;
    in.want[K(UiMarkKind::Cards)] = 1;
    in.want[K(UiMarkKind::Lock)] = 1;
    in.want[K(UiMarkKind::Event)] = 1;
    for (int t = 0; t < 10; ++t) uiBudgetStep(st, in, rules);
    const UiBudgetOut& o = st.out;
    CHECK(o.marks - o.allowed(UiMarkKind::Prompt) <= rules.marks);
    CHECK(o.allowed(UiMarkKind::Tell) == 1);
    CHECK(o.allowed(UiMarkKind::Cards) == 1);
    CHECK(o.allowed(UiMarkKind::Lock) == 1);
    CHECK(o.allowed(UiMarkKind::Boss) == 1);
    CHECK(o.allowed(UiMarkKind::Event) == 1);
    // Every prompt stands in its own row.
    CHECK(o.promptsShown == in.prompts);
    CHECK(uiPromptRowsOf(o.promptsShown) == o.allowed(UiMarkKind::Prompt));
}

TEST_CASE("ui budget: the gift rows stand while the fight has the slot") {
    // No session or no third place: the host passes Absent, and the GIFT row is not there.
    UiPromptReasons why = all(PressReason::Ok);
    const UiPromptMask gifts = static_cast<UiPromptMask>(uiPromptBit(UiPrompt::GiftHeal) | uiPromptBit(UiPrompt::GiftGuard));
    why[static_cast<std::size_t>(UiPrompt::GiftHeal)] = PressReason::Absent;
    why[static_cast<std::size_t>(UiPrompt::GiftGuard)] = PressReason::Absent;
    CHECK((uiPromptsPresent(why) & gifts) == 0);
    // The slot there: each gift stands; cooling down, used up or at a lead at full health, dimmed with its reason.
    for (PressReason r : {PressReason::Cooldown, PressReason::Empty, PressReason::Full, PressReason::Refused}) {
        CAPTURE(static_cast<int>(r));
        why[static_cast<std::size_t>(UiPrompt::GiftHeal)] = r;
        why[static_cast<std::size_t>(UiPrompt::GiftGuard)] = PressReason::Ok;
        CHECK((uiPromptsPresent(why) & gifts) == gifts);
        CHECK((uiPromptsDimmed(why, 0) & gifts) == uiPromptBit(UiPrompt::GiftHeal));
    }
    // Both in the GIFT row, which costs one row's mark: HEAL, the GIFT row and PARTNER are three rows.
    const UiPromptMask three = static_cast<UiPromptMask>(uiPromptBit(UiPrompt::Heal) | gifts | uiPromptBit(UiPrompt::Pair));
    CHECK(uiPromptsDrawable(three, kAllPrompts, {}) == three);
    CHECK(uiPromptRowsOf(three) == 3);
    const UiBudgetRules rules;
    UiBudgetState st;
    UiBudgetInput in = busy(fullFrame());
    in.prompts = three;
    in.want = uiMarkWants(fullFrame(), in.prompts);
    uiBudgetStep(st, in, rules);
    CHECK(st.out.promptsShown == three);
    CHECK(st.out.allowed(UiMarkKind::Prompt) == 3);
}

TEST_CASE("ui budget: one row per key, and none that nothing reaches on the device") {
    std::array<UiPromptMask, kUiPromptCount> none{};
    const UiPromptMask heal = uiPromptBit(UiPrompt::Heal);
    const UiPromptMask giftHeal = uiPromptBit(UiPrompt::GiftHeal);
    const UiPromptMask giftGuard = uiPromptBit(UiPrompt::GiftGuard);
    const UiPromptMask pair = uiPromptBit(UiPrompt::Pair);
    // Every key bound and apart: the open rows pass as they are.
    CHECK(uiPromptsDrawable(heal | pair, kAllPrompts, none) == (heal | pair));
    // Nothing reaches it on the device (no key, no route, not NO KEY): no row, and no mark in the budget.
    CHECK(uiPromptsDrawable(heal | pair, static_cast<UiPromptMask>(kAllPrompts & ~pair), none) == heal);
    // A binding with the heal and both gifts on the one key: the lowest live row is the one drawn.
    std::array<UiPromptMask, kUiPromptCount> shared{};
    shared[static_cast<std::size_t>(UiPrompt::GiftHeal)] = heal;
    shared[static_cast<std::size_t>(UiPrompt::GiftGuard)] = heal | giftHeal;
    CHECK(uiPromptsDrawable(heal | giftHeal | giftGuard, kAllPrompts, shared) == heal);
    CHECK(uiPromptsDrawable(giftHeal | giftGuard, kAllPrompts, shared) == giftHeal);
    CHECK(uiPromptsDrawable(giftGuard | pair, kAllPrompts, shared) == (giftGuard | pair));
    CHECK(uiPromptsDrawable(0, kAllPrompts, shared) == 0);
    // Keys apart (today's binding): the GIFT row holds both gifts; with no key for one, the other has it alone.
    CHECK(uiPromptsDrawable(giftHeal | giftGuard, kAllPrompts, none) == (giftHeal | giftGuard));
    CHECK(uiPromptsDrawable(giftHeal | giftGuard, static_cast<UiPromptMask>(kAllPrompts & ~giftHeal), none) == giftGuard);
    CHECK(uiPromptRowsOf(static_cast<UiPromptMask>(heal | giftHeal | giftGuard | pair)) == 3);
    CHECK(uiPromptRowsOf(0) == 0);
    // Only the GIFT row is shared: every other row still holds one prompt.
    for (std::size_t p = 0; p < kUiPromptCount; ++p) {
        const UiPrompt q = static_cast<UiPrompt>(p);
        CHECK(uiPromptSharesRow(q) == (uiPromptRow(q) == uiPromptRow(UiPrompt::GiftHeal)));
    }
}

TEST_CASE("ui budget: the result card stands alone") {
    const UiBudgetRules rules;
    UiBudgetState st;
    ArenaHudFrame f = fullFrame();
    UiBudgetInput in = busy(f);
    in.want[K(UiMarkKind::Event)] = 3;
    in.resultOpen = true;
    uiBudgetStep(st, in, rules);
    CHECK(st.out.marks == 0);
    CHECK(st.out.bossAlpha == 0.f);
}

TEST_CASE("ui luma floor lifts dark colours and keeps light ones") {
    const UiColor dark{0.6f, 0.2f, 0.15f, 0.8f};
    const UiColor up = uiLumaFloor(dark, 0.45f);
    CHECK(up.a == doctest::Approx(0.8f));
    CHECK(up.r >= dark.r);
    CHECK(up.g > dark.g);
    // A white text over the lifted colour: its contrast against black is at least that of the floor.
    const UiColor black{0.f, 0.f, 0.f, 1.f};
    CHECK(uiContrast(UiColor{up.r, up.g, up.b, 1.f}, black, black) >= (0.45f + 0.05f) / 0.05f - 0.1f);
    const UiColor light{0.95f, 0.93f, 0.9f, 1.f};
    CHECK(uiLumaFloor(light, 0.45f) == light);
}

TEST_CASE("ui budget: a refused prompt is drawn only for 1.5 s after a press of its key, and a usable one stands") {
    const UiPrompt heal = UiPrompt::Heal;
    const UiPrompt pair = UiPrompt::Pair;
    const UiPromptMask healBit = uiPromptBit(heal);
    const UiPromptMask pairBit = uiPromptBit(pair);
    UiPromptReasons why = allWhy(PressReason::Ok);
    why[static_cast<std::size_t>(heal)] = PressReason::Full;
    why[static_cast<std::size_t>(pair)] = PressReason::Closed;
    UiRefusedRows st;
    // Idle: neither refused row stands; the usable ones do.
    uiRefusedStep(st, why, why, 0, 0.016f);
    CHECK(uiRefusedShowing(st) == 0);
    CHECK(uiPromptsStanding(why, st) == static_cast<UiPromptMask>(kAllPrompts & ~healBit & ~pairBit));
    // A press of HEAL while FULL: its row shows, the PARTNER row (CLOSED) still does not.
    uiRefusedStep(st, why, why, healBit, 0.016f);
    CHECK(uiRefusedShowing(st) == healBit);
    CHECK((uiPromptsStanding(why, st) & healBit) == healBit);
    CHECK((uiPromptsStanding(why, st) & pairBit) == 0);
    CHECK(st.left[static_cast<std::size_t>(heal)] == doctest::Approx(kUiRefusedShow));
    // It holds just under 1.5 s, then goes.
    for (int i = 0; i < 93; ++i) uiRefusedStep(st, why, why, 0, 0.016f); // 1.488 s
    CHECK(uiRefusedShowing(st) == healBit);
    uiRefusedStep(st, why, why, 0, 0.016f);
    uiRefusedStep(st, why, why, 0, 0.016f);
    CHECK(uiRefusedShowing(st) == 0);
    CHECK((uiPromptsStanding(why, st) & healBit) == 0);
    // A second press starts it again; the PARTNER row follows the same rule.
    uiRefusedStep(st, why, why, static_cast<UiPromptMask>(healBit | pairBit), 0.016f);
    CHECK(uiRefusedShowing(st) == static_cast<UiPromptMask>(healBit | pairBit));
    // The heal turns usable: it stands as any other, no longer as a refusal.
    why[static_cast<std::size_t>(heal)] = PressReason::Ok;
    uiRefusedStep(st, why, why, 0, 0.016f);
    CHECK((uiRefusedShowing(st) & healBit) == 0);
    CHECK((uiPromptsStanding(why, st) & healBit) == healBit);
    // A press of a usable key shows no refusal; Absent stays absent; Busy is not refused and stands.
    uiRefusedStep(st, why, why, healBit, 0.016f);
    CHECK((uiRefusedShowing(st) & healBit) == 0);
    UiPromptReasons gone = allWhy(PressReason::Absent);
    UiRefusedRows st2;
    uiRefusedStep(st2, gone, gone, kAllPrompts, 0.016f);
    CHECK(uiRefusedShowing(st2) == 0);
    CHECK(uiPromptsStanding(gone, st2) == 0);
    UiPromptReasons busy = allWhy(PressReason::Busy);
    uiRefusedStep(st2, busy, busy, kAllPrompts, 0.016f);
    CHECK(uiRefusedShowing(st2) == 0);
    CHECK(uiPromptsStanding(busy, st2) == kAllPrompts);
    CHECK(kUiRefusedInk >= 0.8f);
}

TEST_CASE("ui budget: a press that works and leaves its action refused shows no refusal row") {
    const UiPromptMask healBit = uiPromptBit(UiPrompt::Heal);
    const UiPromptMask pairBit = uiPromptBit(UiPrompt::Pair);
    const UiPromptReasons before = allWhy(PressReason::Ok);
    UiPromptReasons after = before;
    after[static_cast<std::size_t>(UiPrompt::Heal)] = PressReason::Empty;    // the last heal, used
    after[static_cast<std::size_t>(UiPrompt::Pair)] = PressReason::Cooldown; // the Pair taken, its cooldown started
    UiRefusedRows st;
    uiRefusedStep(st, before, after, static_cast<UiPromptMask>(healBit | pairBit), 0.016f);
    CHECK(uiRefusedShowing(st) == 0);
    CHECK((uiPromptsStanding(after, st) & (healBit | pairBit)) == 0); // gone while idle, as any refused prompt
    // A second press, now refused before it too: the row shows with its reason.
    uiRefusedStep(st, after, after, healBit, 0.016f);
    CHECK(uiRefusedShowing(st) == healBit);
}

TEST_CASE("ui budget: after a retry the first press is judged on the new fight's answers") {
    const UiPromptMask pairBit = uiPromptBit(UiPrompt::Pair);
    UiPromptReasons cooling = allWhy(PressReason::Ok);
    cooling[static_cast<std::size_t>(UiPrompt::Pair)] = PressReason::Cooldown;
    UiRefusedRows st;
    UiRefusedWatch w;
    // The last fight ends with the Pair cooling down.
    uiRefusedWatchStep(st, w, cooling, 0, 0.016f);
    uiRefusedWatchStep(st, w, cooling, 0, 0.016f);
    // A retry: the view clears the watch and the rows. In the new fight the Pair is usable for a tick, then a press takes
    // it and its cooldown starts: the press worked, so no row.
    w.seeded = false;
    st = UiRefusedRows{};
    uiRefusedWatchStep(st, w, allWhy(PressReason::Ok), 0, 0.016f);
    uiRefusedWatchStep(st, w, cooling, pairBit, 0.016f);
    CHECK(uiRefusedShowing(st) == 0);
    // Without the reset the same press is judged on the old fight's Cooldown and shows a row: the fault the reset fixes.
    UiRefusedRows stale;
    UiRefusedWatch old;
    uiRefusedWatchStep(stale, old, cooling, 0, 0.016f);
    uiRefusedWatchStep(stale, old, cooling, pairBit, 0.016f);
    CHECK(uiRefusedShowing(stale) == pairBit);
    // The seeding tick counts no press, even one on a refused prompt.
    UiRefusedRows fresh;
    UiRefusedWatch seed;
    uiRefusedWatchStep(fresh, seed, cooling, pairBit, 0.016f);
    CHECK(uiRefusedShowing(fresh) == 0);
}
