#include "ftcore/UiBudget.h"

#include <algorithm>

namespace ftcore {

namespace {

constexpr std::size_t idx(UiMarkKind k) { return static_cast<std::size_t>(k); }

std::uint8_t bits(std::uint8_t m) {
    std::uint8_t n = 0;
    for (; m; m = static_cast<std::uint8_t>(m & (m - 1))) ++n;
    return n;
}

} // namespace

std::uint8_t uiPromptsOpen(const ArenaHudFrame& f, bool pairOpen) {
    std::uint8_t m = 0;
    const ArenaHudLead& l = f.lead;
    if (pairOpen && f.partner.present && !f.partner.down) m |= uiPromptBit(UiPrompt::Pair);
    if (l.heals > 0 && !l.down && l.hp < l.hpMax) m |= uiPromptBit(UiPrompt::Heal); // at full HP a heal would do nothing
    return m;
}

std::array<std::uint8_t, kUiMarkKindCount> uiMarkWants(const ArenaHudFrame& f, std::uint8_t prompts) {
    std::array<std::uint8_t, kUiMarkKindCount> w{};
    w[idx(UiMarkKind::Lead)] = f.lead.hpMax > 0 ? 1 : 0;
    // The partner's row whenever the fight has a partner: a party list that comes and goes is itself a puzzle.
    w[idx(UiMarkKind::Party)] = static_cast<std::uint8_t>((f.partner.present ? 1 : 0)
        + (f.third.present && !f.third.planned ? 1 : 0)); // a planned place has no data: no mark
    w[idx(UiMarkKind::Boss)] = f.boss.present && f.boss.hpMax > 0 ? 1 : 0;
    w[idx(UiMarkKind::Target)] = f.target.present && f.target.hpMax > 0
        && !(f.boss.present && f.target.enemy == f.boss.slot) ? 1 : 0;
    w[idx(UiMarkKind::Rounds)] = f.lead.roundsMax > 0 ? 1 : 0;
    w[idx(UiMarkKind::Prompt)] = std::min(bits(prompts), kUiPromptRows);
    return w;
}

const UiBudgetOut& uiBudgetStep(UiBudgetState& st, const UiBudgetInput& in, const UiBudgetRules& rules) {
    const float dt = std::max(in.dt, 0.f);
    const bool changed = in.activity || in.changed != 0 || in.bossEvent;
    st.sinceChange = changed ? 0.f : st.sinceChange + dt;
    st.sinceBoss = in.bossEvent ? 0.f : st.sinceBoss + dt;

    UiBudgetOut& o = st.out;
    o = UiBudgetOut{};
    o.rest = st.sinceChange >= rules.rest && !in.resultOpen;

    // The boss line fades to nothing at rest; it holds `rest` seconds after its last event.
    const UiFadeTimes gone{0.15f, rules.rest, 0.6f, 0.f};
    uiFadeStep(st.boss, in.bossEvent, dt, gone);
    uiFadeStep(st.action, changed, dt, gone);
    o.actionAlpha = st.action.alpha;

    if (in.resultOpen) {
        // The result card stands alone.
        st.boss.alpha = 0.f;
        o.actionAlpha = 0.f;
        return o;
    }

    std::uint8_t left = rules.marks;
    for (std::size_t k = 0; k < kUiMarkKindCount; ++k) {
        const UiMarkKind kind = static_cast<UiMarkKind>(k);
        std::uint8_t want = in.want[k];
        // At rest the in-action marks keep their place only while they fade out; a prompt stands while it is open.
        const bool stays = kind == UiMarkKind::Lead || kind == UiMarkKind::Party || kind == UiMarkKind::Prompt;
        if (o.rest && !stays && st.action.alpha <= 0.f) want = 0;
        if (kind == UiMarkKind::Boss && st.boss.alpha <= 0.f) want = 0;
        const std::uint8_t give = std::min(want, left);
        o.allow[k] = give;
        left = static_cast<std::uint8_t>(left - give);
    }
    for (std::uint8_t a : o.allow) o.marks = static_cast<std::uint8_t>(o.marks + a);

    o.bossAlpha = o.allowed(UiMarkKind::Boss) ? st.boss.alpha : 0.f;
    // Only open prompts, in their order, as many as were granted.
    std::uint8_t room = std::min(o.allowed(UiMarkKind::Prompt), kUiPromptRows);
    for (std::size_t p = 0; p < kUiPromptCount && room > 0; ++p) {
        const std::uint8_t b = static_cast<std::uint8_t>(1u << p);
        if (in.prompts & b) {
            o.promptsShown |= b;
            --room;
        }
    }
    st.maxMarks = std::max(st.maxMarks, o.marks);
    return o;
}

} // namespace ftcore
