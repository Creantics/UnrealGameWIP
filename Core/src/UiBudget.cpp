#include "ftcore/UiBudget.h"

#include <algorithm>

namespace ftcore {

namespace {

constexpr std::size_t idx(UiMarkKind k) { return static_cast<std::size_t>(k); }

std::uint8_t bits(UiPromptMask m) {
    std::uint8_t n = 0;
    for (; m; m = static_cast<UiPromptMask>(m & (m - 1))) ++n;
    return n;
}

} // namespace

UiPromptMask uiPromptsOpen(const ArenaHudFrame& f, bool pairOpen) {
    UiPromptMask m = 0;
    const ArenaHudLead& l = f.lead;
    const bool up = !l.down;
    const bool hurt = l.hp < l.hpMax; // at full HP a heal (or a Gift Heal) would do nothing
    if (pairOpen && f.partner.present && !f.partner.down) m |= uiPromptBit(UiPrompt::Pair);
    if (l.heals > 0 && up && hurt && !l.overloaded) m |= uiPromptBit(UiPrompt::Heal);
    if (f.third.giftHeals > 0 && up && hurt) m |= uiPromptBit(UiPrompt::GiftHeal);
    if (f.third.giftGuardReady && up) m |= uiPromptBit(UiPrompt::GiftGuard);
    // Mid-reload the press is already made: the cylinder shows the reload filling, so the row would repeat it.
    if (l.roundsMax > 0 && l.rounds == 0 && l.reload01 <= 0.f && up) m |= uiPromptBit(UiPrompt::Reload);
    return m;
}

bool uiControlUsable(const ArenaHudLead& lead, UiPrompt p) {
    switch (p) {
    case UiPrompt::Attack: return !lead.down && !lead.overloaded;
    case UiPrompt::Guard: return !lead.down && !lead.guardBroken;
    case UiPrompt::Dodge:
    case UiPrompt::Jump: return !lead.down;
    case UiPrompt::Heal:
    case UiPrompt::GiftHeal:
    case UiPrompt::GiftGuard:
    case UiPrompt::Reload:
    case UiPrompt::Pair: return true;
    }
    return true;
}

UiPromptMask uiPromptsDrawable(UiPromptMask open, UiPromptMask keyed, const std::array<UiPromptMask, kUiPromptCount>& sameKeyBelow) {
    UiPromptMask out = 0;
    std::uint32_t rows = 0;
    for (std::size_t p = 0; p < kUiPromptCount; ++p) {
        const UiPromptMask b = static_cast<UiPromptMask>(1u << p);
        const std::uint32_t row = 1u << uiPromptRow(static_cast<UiPrompt>(p));
        if ((open & keyed & b) == 0) continue;
        if ((out & sameKeyBelow[p]) != 0) continue; // a lower row already shows this key
        if (rows & row) continue;                   // a lower prompt holds this row
        out = static_cast<UiPromptMask>(out | b);
        rows |= row;
    }
    return out;
}

std::array<std::uint8_t, kUiMarkKindCount> uiMarkWants(const ArenaHudFrame& f, UiPromptMask prompts) {
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
    // Only open prompts, in their order (so from the lowest row up), as many as were granted; a row not granted is empty.
    std::uint8_t room = std::min(o.allowed(UiMarkKind::Prompt), kUiPromptRows);
    for (std::size_t p = 0; p < kUiPromptCount && room > 0; ++p) {
        const UiPromptMask b = static_cast<UiPromptMask>(1u << p);
        if (in.prompts & b) {
            o.promptsShown = static_cast<UiPromptMask>(o.promptsShown | b);
            --room;
        }
    }
    st.maxMarks = std::max(st.maxMarks, o.marks);
    return o;
}

} // namespace ftcore
