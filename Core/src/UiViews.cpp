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
    out.releaseReady = out.loadMax > 0 && out.load > kArenaReleaseOpenLoad;
    out.heals = fight.healCharges();
    out.healsMax = f.healCharges;
    out.read = fight.readStacks();
    out.readMax = kMaxReadStacks;
    out.down = p.action == ArenaAction::Downed;
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
                view.options[i] = DialogueViewOption{r->optionText(i), r->optionSelectable(i), r->optionReturns(i)};
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
        flags.logAvailable != view.flags.logAvailable) {
        view.continuePrompt = prompt;
        view.flags = flags;
        ++view.promptRevision;
        changed = true;
    }
    return changed;
}

} // namespace ftcore
