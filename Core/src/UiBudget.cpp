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

std::uint8_t uiVerbsAvailable(const ArenaHudFrame& f) {
    std::uint8_t m = 0;
    const ArenaHudLead& l = f.lead;
    if (l.hpMax > 0 && !l.down && !l.guardBroken) m |= uiVerbBit(UiVerb::Guard);
    if (l.loadMax > 0 && l.releaseReady) m |= uiVerbBit(UiVerb::Release);
    if (f.partner.present && !f.partner.down) m |= uiVerbBit(UiVerb::Pair);
    if (l.heals > 0 && !l.down && l.hp < l.hpMax) m |= uiVerbBit(UiVerb::Heal); // at full HP a heal would do nothing
    return m;
}

std::array<std::uint8_t, kUiMarkKindCount> uiMarkWants(const ArenaHudFrame& f, std::uint8_t verbsAvailable) {
    std::array<std::uint8_t, kUiMarkKindCount> w{};
    w[idx(UiMarkKind::Lead)] = f.lead.hpMax > 0 ? 1 : 0;
    // The partner's place only when it has something to say: hurt, down or holding Load (hidden at full health).
    const bool partnerNews = f.partner.present && (f.partner.down || f.partner.hp < f.partner.hpMax || f.partner.load > 0);
    w[idx(UiMarkKind::Party)] = static_cast<std::uint8_t>((partnerNews ? 1 : 0)
        + (f.third.present && !f.third.planned ? 1 : 0)); // a planned place has no data: no mark
    w[idx(UiMarkKind::Boss)] = f.boss.present && f.boss.hpMax > 0 ? 1 : 0;
    w[idx(UiMarkKind::Target)] = f.target.present && f.target.hpMax > 0
        && !(f.boss.present && f.target.enemy == f.boss.slot) ? 1 : 0;
    w[idx(UiMarkKind::Rounds)] = f.lead.roundsMax > 0 ? 1 : 0;
    w[idx(UiMarkKind::Verb)] = bits(verbsAvailable);
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

    // The boss line and the verbs fade to nothing at rest; the boss line holds `rest` seconds after its last event.
    const UiFadeTimes gone{0.15f, rules.rest, 0.6f, 0.f};
    uiFadeStep(st.boss, in.bossEvent, dt, gone);
    uiFadeStep(st.verbs, changed && !o.rest, dt, gone);
    uiFadeStep(st.action, changed, dt, gone);
    o.actionAlpha = st.action.alpha;

    if (in.resultOpen) {
        // The result card stands alone.
        st.boss.alpha = 0.f;
        st.verbs.alpha = 0.f;
        o.actionAlpha = 0.f;
        return o;
    }

    std::uint8_t left = rules.marks;
    for (std::size_t k = 0; k < kUiMarkKindCount; ++k) {
        const UiMarkKind kind = static_cast<UiMarkKind>(k);
        std::uint8_t want = in.want[k];
        // At rest the in-action marks keep their place only while they fade out.
        if (o.rest && kind != UiMarkKind::Lead && kind != UiMarkKind::Party && st.action.alpha <= 0.f) want = 0;
        if (kind == UiMarkKind::Boss && st.boss.alpha <= 0.f) want = 0;
        if (kind == UiMarkKind::Verb && st.verbs.alpha <= 0.f) want = 0;
        const std::uint8_t give = std::min(want, left);
        o.allow[k] = give;
        left = static_cast<std::uint8_t>(left - give);
    }
    for (std::uint8_t a : o.allow) o.marks = static_cast<std::uint8_t>(o.marks + a);

    o.bossAlpha = o.allowed(UiMarkKind::Boss) ? st.boss.alpha : 0.f;
    o.verbAlpha = o.allowed(UiMarkKind::Verb) ? st.verbs.alpha : 0.f;
    // Only available verbs, in their order, as many as were granted.
    std::uint8_t room = o.allowed(UiMarkKind::Verb);
    // The prompted verb first claims a place, so the one asked for is never the one dropped.
    if (in.prompted >= 0 && in.prompted < static_cast<std::int8_t>(kUiVerbCount)) {
        const std::uint8_t b = static_cast<std::uint8_t>(1u << in.prompted);
        if ((in.verbsAvailable & b) && room > 0) {
            o.verbsShown |= b;
            o.keyRing = in.prompted;
            --room;
        }
    }
    for (std::size_t v = 0; v < kUiVerbCount && room > 0; ++v) {
        const std::uint8_t b = static_cast<std::uint8_t>(1u << v);
        if ((in.verbsAvailable & b) && !(o.verbsShown & b)) {
            o.verbsShown |= b;
            --room;
        }
    }
    st.maxMarks = std::max(st.maxMarks, o.marks);
    return o;
}

} // namespace ftcore
