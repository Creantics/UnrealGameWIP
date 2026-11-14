#include "ftcore/UiViews.h"

#include "ftcore/ArenaFight.h"
#include "ftcore/Dialogue.h"
#include "ftcore/Load.h"
#include "ftcore/ReadCards.h"

#include <algorithm>

namespace ftcore {

void fillArenaHudLead(const ArenaFight& fight, ArenaHudLead& out) {
    const ArenaFighter& f = fight.def().fighter;
    const ArenaPlayerOut& p = fight.output().player;
    out.hp = fight.hp();
    out.hpMax = f.hp;
    out.stamina01 = f.stamina > 0.f ? std::clamp(fight.stamina() / f.stamina, 0.f, 1.f) : 0.f;
    out.guarding = p.action == ArenaAction::Guard;
    out.guardBroken = p.action == ArenaAction::GuardBreak;
    out.loadMax = f.attacks ? kMaxLoad : 0; // a kit with no attacks stores no Load
    out.load = out.loadMax > 0 ? fight.load() : 0;
    out.overloaded = out.loadMax > 0 && fight.overloaded();
    out.loadCap = out.loadMax > 0 ? fight.loadCap() : 0;
    out.releaseAt = out.loadMax > 0 ? fight.releaseOpenLoad() : 0;
    out.releaseReady = out.loadMax > 0 && out.load > fight.releaseOpenLoad();
    out.heals = fight.healCharges();
    out.healsMax = fight.healChargesMax();
    const ArenaChairs& chairs = fight.chairs();
    out.chairs = {};
    out.chairCount = chairs.count;
    for (std::size_t i = 0; i < chairs.count && i < kMaxPlaces; ++i) out.chairs[i] = chairs.named[i].kind;
    out.tookHalf = false;
    for (const ArenaEvent& e : fight.output().events)
        if (e.type == ArenaEventType::TookHalf) out.tookHalf = true;
    out.read = fight.readStacks();
    out.readMax = kMaxReadStacks;
    out.roundsMax = fight.roundsMax();
    out.rounds = fight.rounds();
    out.reload01 = fight.reloadProgress();
    out.down = p.action == ArenaAction::Downed;
}

void fillArenaHudPartner(const ArenaFight& fight, ArenaHudPartner& out) {
    out.present = fight.allyMaxHp() > 0;
    out.hp = out.present ? fight.allyHp() : 0;
    out.hpMax = out.present ? fight.allyMaxHp() : 0;
    out.down = out.present && !fight.allyAlive();
}

void fillArenaHudTarget(const ArenaFight& fight, ArenaHudTarget& out) {
    std::uint8_t i = fight.lock();
    const bool locked = i != kArenaNobody;
    if (!locked) i = fight.output().player.soft;
    if (i == kArenaNobody || i >= fight.enemyCount() || !fight.enemyAlive(i)) {
        out = ArenaHudTarget{};
        return;
    }
    const ArenaEnemyKind& k = fight.def().enemies[i];
    out.present = true;
    out.enemy = i;
    out.locked = locked;
    out.hp = fight.enemyHp(i);
    out.hpMax = k.hp;
    out.poise01 = k.poise > 0.f ? std::clamp(fight.enemyPoise(i) / k.poise, 0.f, 1.f) : 0.f;
}

std::uint32_t updateArenaHudView(ArenaHudView& view, const ArenaHudFrame& f) {
    std::uint32_t mask = 0;
    const auto step = [&](auto& dst, const auto& src, ArenaHudSection s) {
        if (dst == src) return;
        dst = src;
        ++view.revision[static_cast<std::size_t>(s)];
        mask |= 1u << static_cast<std::uint32_t>(s);
    };
    step(view.now.lead, f.lead, ArenaHudSection::Lead);
    step(view.now.partner, f.partner, ArenaHudSection::Partner);
    step(view.now.third, f.third, ArenaHudSection::Third);
    step(view.now.target, f.target, ArenaHudSection::Target);
    step(view.now.boss, f.boss, ArenaHudSection::Boss);
    return mask;
}

bool updateDialogueView(DialogueView& view, const DialoguePresenter& p, const DialogueViewFlags& flags) {
    const StoryRunner* r = p.runner();
    DialogueViewKind kind = DialogueViewKind::Hidden;
    switch (p.state()) {
    case StoryRunner::State::Line: kind = DialogueViewKind::Line; break;
    case StoryRunner::State::Direction: kind = DialogueViewKind::Direction; break;
    case StoryRunner::State::Choice: kind = DialogueViewKind::Choice; break;
    case StoryRunner::State::Idle:
    case StoryRunner::State::Hold:
    case StoryRunner::State::Gameplay:
    case StoryRunner::State::Finished: break;
    }
    if (!r) kind = DialogueViewKind::Hidden;
    bool changed = false;

    if (view.epoch != p.beatEpoch() || view.kind != kind) {
        view.epoch = p.beatEpoch();
        view.kind = kind;
        const bool shown = kind != DialogueViewKind::Hidden;
        view.speaker = shown && kind == DialogueViewKind::Line ? r->speaker() : std::string_view();
        view.lineId = shown && kind == DialogueViewKind::Line ? r->lineId() : std::string_view();
        view.optionCount = 0;
        if (shown && kind == DialogueViewKind::Choice) {
            view.optionCount = std::min<std::uint32_t>(r->optionCount(), kDialogueViewOptions);
            for (std::uint32_t i = 0; i < view.optionCount; ++i) {
                // No reason yet: the runner's options carry none (the UI then shows the greyed reply alone).
                view.options[i] = DialogueViewOption{r->optionText(i), r->optionSelectable(i), r->optionReturns(i), std::string_view()};
            }
        }
        view.text = std::string_view();
        view.fullText = shown ? r->text() : std::string_view();
        view.revealedChars = 0;
        view.complete = false;
        ++view.beatRevision;
        changed = true;
    }

    const std::string_view text = kind == DialogueViewKind::Hidden ? std::string_view() : p.revealedText();
    const bool complete = kind != DialogueViewKind::Hidden && p.revealed();
    if (text.size() != view.text.size() || text.data() != view.text.data() || complete != view.complete) {
        view.text = text;
        view.revealedChars = kind == DialogueViewKind::Hidden ? 0u : p.revealedChars();
        view.complete = complete;
        ++view.textRevision;
        changed = true;
    }

    const bool prompt = complete && (kind == DialogueViewKind::Line || kind == DialogueViewKind::Direction);
    if (prompt != view.continuePrompt || flags.autoOn != view.flags.autoOn || flags.skipOn != view.flags.skipOn ||
        flags.logAvailable != view.flags.logAvailable || flags.cursor != view.flags.cursor) {
        view.continuePrompt = prompt;
        view.flags = flags;
        ++view.promptRevision;
        changed = true;
    }
    return changed;
}

void uiLoadNameStep(UiLoadName& s, const ArenaHudLead& lead, float dt) {
    const float fill = lead.releaseAt > 0 ? std::clamp(static_cast<float>(lead.load) / static_cast<float>(lead.releaseAt), 0.f, 1.f) : 0.f;
    s.fill = fill >= s.fill ? fill : std::max(fill, s.fill - dt / kUiLoadDrain);
    // A cap at or under the threshold leaves nothing to brighten toward.
    s.bright = lead.loadCap > lead.releaseAt
                   ? std::clamp(static_cast<float>(lead.load - lead.releaseAt) / static_cast<float>(lead.loadCap - lead.releaseAt), 0.f, 1.f)
                   : 0.f;
    if (!lead.releaseReady) {
        s.shimmer = -1.f;
    } else if (!s.ready) {
        s.shimmer = 0.f; // the threshold just crossed
    } else if (s.shimmer >= 0.f) {
        s.shimmer += dt / kUiLoadShimmer;
        if (s.shimmer > 1.f) s.shimmer = -1.f;
    }
    s.ready = lead.releaseReady;
}

void uiRowSwapStep(UiRowSwap& s, const ArenaHudFrame& f, float dt) {
    // Without a partner row there is nothing to trade places with, so a change of seat just shows.
    if (s.lead != 0xFF && f.lead.lead != s.lead && f.partner.present) {
        s.left01 = 1.f;
    } else {
        s.left01 = std::max(0.f, s.left01 - std::max(dt, 0.f) / kUiRowSwap);
    }
    s.lead = f.lead.lead;
}

float uiQteLeft01(const ArenaHudQte& q, float now) {
    if (!q.live || !(q.to > q.from)) return 0.f;
    return std::clamp((q.to - now) / (q.to - q.from), 0.f, 1.f);
}

} // namespace ftcore
