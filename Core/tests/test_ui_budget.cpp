#include "ftcore/UiBudget.h"

#include <doctest/doctest.h>

using namespace ftcore;

namespace {

constexpr std::size_t K(UiMarkKind k) { return static_cast<std::size_t>(k); }
constexpr UiPromptMask kAllPrompts = static_cast<UiPromptMask>((1u << kUiPromptCount) - 1u);

ArenaHudFrame fullFrame() {
    ArenaHudFrame f;
    f.lead.hpMax = 100;
    f.lead.hp = 80;
    f.lead.loadMax = 90;
    f.lead.releaseReady = true;
    f.lead.heals = 2;
    f.lead.roundsMax = 6;
    f.lead.rounds = 6; // loaded: an empty gun opens RELOAD
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
    in.prompts = uiPromptsOpen(f, true);
    in.want = uiMarkWants(f, in.prompts);
    return in;
}

} // namespace

TEST_CASE("ui budget: absent data asks for no mark") {
    ArenaHudFrame f; // nothing present
    const auto w = uiMarkWants(f, uiPromptsOpen(f, true));
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

TEST_CASE("ui budget: HEAL only while hurt with heals left, PARTNER only in its window with the partner up") {
    ArenaHudFrame f = fullFrame();
    CHECK(uiPromptsOpen(f, true) == (uiPromptBit(UiPrompt::Pair) | uiPromptBit(UiPrompt::Heal)));
    CHECK(uiPromptsOpen(f, false) == uiPromptBit(UiPrompt::Heal));
    f.lead.hp = f.lead.hpMax;
    CHECK((uiPromptsOpen(f, false) & uiPromptBit(UiPrompt::Heal)) == 0); // a heal would do nothing
    f.lead.hp = 10;
    f.lead.heals = 0;
    CHECK((uiPromptsOpen(f, false) & uiPromptBit(UiPrompt::Heal)) == 0);
    f.partner.down = true;
    CHECK(uiPromptsOpen(f, true) == 0);
    f.partner.down = false;
    f.partner.present = false;
    CHECK(uiPromptsOpen(f, true) == 0);
    // At most four rows are asked for, however many are open.
    CHECK(uiMarkWants(f, kAllPrompts)[K(UiMarkKind::Prompt)] == kUiPromptRows);
    CHECK(uiMarkWants(f, uiPromptBit(UiPrompt::Dodge))[K(UiMarkKind::Prompt)] == 1);
}

TEST_CASE("ui budget: at rest only the lead, the party and the open prompts show") {
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
    CHECK(o.allowed(UiMarkKind::Prompt) == 2); // a prompt stands for as long as its press is open
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

TEST_CASE("ui budget: never more than the mark cap in action, highest priority kept") {
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
                    CHECK(o.marks <= rules.marks);
                    CHECK(o.allowed(UiMarkKind::Lead) == 1);
                    CHECK(o.allowed(UiMarkKind::Tell) == tells); // a tell is never dropped
                    int sum = 0;
                    for (std::uint8_t a : o.allow) sum += a;
                    CHECK(sum == o.marks);
                    // The prompts drawn are as many as granted, the first ones in order.
                    int shown = 0;
                    for (unsigned b = o.promptsShown; b; b &= b - 1) ++shown;
                    CHECK(shown == o.allowed(UiMarkKind::Prompt));
                    CHECK(shown <= kUiPromptRows);
                    CHECK((o.promptsShown & ~((1u << shown) - 1u)) == 0);
                    CHECK(st.maxMarks <= rules.marks);
                }
            }
        }
    }
}

TEST_CASE("ui budget: HEAL claims the first row, then PARTNER, then the basic controls in order") {
    const UiBudgetRules rules;
    UiBudgetState st;
    ArenaHudFrame f = fullFrame();
    UiBudgetInput in = busy(f);
    in.prompts = uiPromptBit(UiPrompt::Heal) | uiPromptBit(UiPrompt::Guard) | uiPromptBit(UiPrompt::Dodge) |
                 uiPromptBit(UiPrompt::Jump);
    in.want = uiMarkWants(f, in.prompts);
    uiBudgetStep(st, in, rules);
    CHECK(st.out.promptsShown == in.prompts); // four rows: all four
    in.prompts = static_cast<UiPromptMask>(in.prompts | uiPromptBit(UiPrompt::Pair));
    in.want = uiMarkWants(f, in.prompts);
    uiBudgetStep(st, in, rules);
    CHECK(st.out.promptsShown ==
          (uiPromptBit(UiPrompt::Heal) | uiPromptBit(UiPrompt::Pair) | uiPromptBit(UiPrompt::Guard) | uiPromptBit(UiPrompt::Dodge)));
    in.prompts = 0;
    in.want = uiMarkWants(f, 0);
    uiBudgetStep(st, in, rules);
    CHECK(st.out.promptsShown == 0);
}

TEST_CASE("ui budget: the gift and reload rows open only on reported data") {
    ArenaHudFrame f = fullFrame();
    const UiPromptMask added = uiPromptBit(UiPrompt::GiftHeal) | uiPromptBit(UiPrompt::GiftGuard) | uiPromptBit(UiPrompt::Reload);
    // Nothing reported (the defaults), and a loaded gun: none of the three.
    CHECK(f.third.giftHeals == -1);
    CHECK_FALSE(f.third.giftGuardReady);
    CHECK((uiPromptsOpen(f, true) & added) == 0);
    CHECK((uiPromptsOpen(ArenaHudFrame{}, true) & added) == 0); // an empty frame: no cylinder, no gift data

    // GIFT HEAL: hurt with Gift Heals left, the lead up.
    f.third.giftHeals = 3;
    CHECK((uiPromptsOpen(f, false) & uiPromptBit(UiPrompt::GiftHeal)) != 0);
    f.lead.hp = f.lead.hpMax;
    CHECK((uiPromptsOpen(f, false) & uiPromptBit(UiPrompt::GiftHeal)) == 0); // it would do nothing
    f.lead.hp = 10;
    f.third.giftHeals = 0;
    CHECK((uiPromptsOpen(f, false) & uiPromptBit(UiPrompt::GiftHeal)) == 0);
    f.third.giftHeals = 3;
    f.lead.down = true;
    CHECK((uiPromptsOpen(f, false) & added) == 0);
    f.lead.down = false;

    // GIFT GUARD: only off its cooldown.
    CHECK((uiPromptsOpen(f, false) & uiPromptBit(UiPrompt::GiftGuard)) == 0);
    f.third.giftGuardReady = true;
    CHECK((uiPromptsOpen(f, false) & uiPromptBit(UiPrompt::GiftGuard)) != 0);

    // RELOAD: only with the gun empty, and not once the reload is under way.
    CHECK((uiPromptsOpen(f, false) & uiPromptBit(UiPrompt::Reload)) == 0);
    f.lead.rounds = 0;
    CHECK((uiPromptsOpen(f, false) & uiPromptBit(UiPrompt::Reload)) != 0);
    f.lead.reload01 = 0.4f;
    CHECK((uiPromptsOpen(f, false) & uiPromptBit(UiPrompt::Reload)) == 0);
    f.lead.reload01 = 0.f;
    f.lead.roundsMax = 0; // no gun
    CHECK((uiPromptsOpen(f, false) & uiPromptBit(UiPrompt::Reload)) == 0);
}

TEST_CASE("ui budget: the rows stack from the bottom HEAL, GIFT, RELOAD, PARTNER, four at most") {
    const UiBudgetRules rules;
    ArenaHudFrame f = fullFrame();
    f.third.giftHeals = 3;
    f.lead.rounds = 0;
    UiBudgetState st;
    UiBudgetInput in = busy(f);
    CHECK(in.prompts == (uiPromptBit(UiPrompt::Heal) | uiPromptBit(UiPrompt::GiftHeal) | uiPromptBit(UiPrompt::Reload) |
                         uiPromptBit(UiPrompt::Pair)));
    uiBudgetStep(st, in, rules);
    CHECK(st.out.promptsShown == in.prompts);
    // A fifth live row: the four lowest in the order keep theirs.
    f.third.giftGuardReady = true;
    in = busy(f);
    uiBudgetStep(st, in, rules);
    CHECK(st.out.promptsShown == (uiPromptBit(UiPrompt::Heal) | uiPromptBit(UiPrompt::GiftHeal) | uiPromptBit(UiPrompt::GiftGuard) |
                                  uiPromptBit(UiPrompt::Reload)));
}

TEST_CASE("ui budget: one row per key, and none without a key") {
    std::array<UiPromptMask, kUiPromptCount> none{};
    const UiPromptMask heal = uiPromptBit(UiPrompt::Heal);
    const UiPromptMask giftHeal = uiPromptBit(UiPrompt::GiftHeal);
    const UiPromptMask giftGuard = uiPromptBit(UiPrompt::GiftGuard);
    const UiPromptMask reload = uiPromptBit(UiPrompt::Reload);
    // Every key bound and apart: the open rows pass as they are.
    CHECK(uiPromptsDrawable(heal | reload, kAllPrompts, none) == (heal | reload));
    // No key on the device: no row (so it takes no row and no mark in the budget).
    CHECK(uiPromptsDrawable(heal | reload, static_cast<UiPromptMask>(kAllPrompts & ~reload), none) == heal);
    // Today's binding: the heal and both gifts on the one key. The lowest live row is the one drawn.
    std::array<UiPromptMask, kUiPromptCount> shared{};
    shared[static_cast<std::size_t>(UiPrompt::GiftHeal)] = heal;
    shared[static_cast<std::size_t>(UiPrompt::GiftGuard)] = heal | giftHeal;
    CHECK(uiPromptsDrawable(heal | giftHeal | giftGuard, kAllPrompts, shared) == heal);
    CHECK(uiPromptsDrawable(giftHeal | giftGuard, kAllPrompts, shared) == giftHeal);
    CHECK(uiPromptsDrawable(giftGuard | reload, kAllPrompts, shared) == (giftGuard | reload));
    CHECK(uiPromptsDrawable(0, kAllPrompts, shared) == 0);
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
