#include "ftcore/Explore.h"

#include "ftcore/Assert.h"

#include <cmath>

namespace ftcore {

namespace {
float distSq(Vec2 a, Vec2 b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}
} // namespace

int Exploration::addObjective(std::uint8_t required) {
    FTCORE_ASSERT(required > 0);
    if (objectiveCount_ == kMaxObjectives) return -1;
    required_[objectiveCount_] = required;
    progress_[objectiveCount_] = 0;
    return static_cast<int>(objectiveCount_++);
}

int Exploration::add(const Interactable& item) {
    FTCORE_ASSERT(item.radius > 0.f);
    FTCORE_ASSERT(item.objective == kNoObjective || item.objective < objectiveCount_);
    if (count_ == kMaxInteractables) return -1;
    items_[count_] = item;
    return static_cast<int>(count_++);
}

bool Exploration::isUsed(int id) const {
    if (id < 0 || static_cast<std::size_t>(id) >= count_) return true;
    return (used_ >> id) & 1u;
}

bool Exploration::requirementsMet(std::uint32_t mask) const {
    for (std::size_t i = 0; i < objectiveCount_; ++i)
        if (((mask >> i) & 1u) && !objectiveDone(static_cast<int>(i))) return false;
    return true;
}

bool Exploration::isAvailable(int id) const {
    return !isUsed(id) && requirementsMet(items_[static_cast<std::size_t>(id)].needs);
}

int Exploration::focus(Vec2 player, Vec2 facing) const {
    const float facingLen = std::sqrt(facing.x * facing.x + facing.y * facing.y);
    int best = -1;
    float bestDistSq = 0.f;
    for (std::size_t i = 0; i < count_; ++i) {
        const Interactable& it = items_[i];
        const int id = static_cast<int>(i);
        if (it.kind == InteractKind::Encounter || isUsed(id)) continue;
        const float d2 = distSq(player, it.pos);
        if (d2 > it.radius * it.radius) continue;
        if (facingLen > 0.f && d2 > 1e-6f) {
            const float cosAngle = ((it.pos.x - player.x) * facing.x + (it.pos.y - player.y) * facing.y) / (std::sqrt(d2) * facingLen);
            if (cosAngle < kFocusCos) continue;
        }
        if (best < 0 || d2 < bestDistSq) {
            best = id;
            bestDistSq = d2;
        }
    }
    return best;
}

void Exploration::advance(std::uint8_t objective) {
    if (objective == kNoObjective) return;
    if (progress_[objective] < required_[objective]) ++progress_[objective];
}

InteractResult Exploration::interact(int id) {
    if (isUsed(id) || items_[static_cast<std::size_t>(id)].kind == InteractKind::Encounter) return InteractResult::Gone;
    const Interactable& it = items_[static_cast<std::size_t>(id)];
    if (!requirementsMet(it.needs)) return InteractResult::Locked;
    used_ |= 1u << id;
    advance(it.objective);
    return InteractResult::Used;
}

int Exploration::encounterAt(Vec2 player) const {
    for (std::size_t i = 0; i < count_; ++i) {
        const Interactable& it = items_[i];
        const int id = static_cast<int>(i);
        if (it.kind != InteractKind::Encounter || !isAvailable(id)) continue;
        if (distSq(player, it.pos) <= it.radius * it.radius) return id;
    }
    return -1;
}

void Exploration::clearEncounter(int id) {
    if (isUsed(id) || items_[static_cast<std::size_t>(id)].kind != InteractKind::Encounter) return;
    used_ |= 1u << id;
    advance(items_[static_cast<std::size_t>(id)].objective);
}

std::uint8_t Exploration::progress(int objective) const {
    return progress_[static_cast<std::size_t>(objective)];
}

bool Exploration::objectiveDone(int objective) const {
    const std::size_t i = static_cast<std::size_t>(objective);
    return progress_[i] >= required_[i];
}

bool Exploration::allDone() const {
    return currentObjective() < 0;
}

int Exploration::currentObjective() const {
    for (std::size_t i = 0; i < objectiveCount_; ++i)
        if (!objectiveDone(static_cast<int>(i))) return static_cast<int>(i);
    return -1;
}

int Exploration::pilotTarget(Vec2 player) const {
    const int objective = currentObjective();
    if (objective < 0) return -1;
    int best = -1;
    float bestDistSq = 0.f;
    for (std::size_t i = 0; i < count_; ++i) {
        const int id = static_cast<int>(i);
        if (items_[i].objective != objective || !isAvailable(id)) continue;
        const float d2 = distSq(player, items_[i].pos);
        if (best < 0 || d2 < bestDistSq) {
            best = id;
            bestDistSq = d2;
        }
    }
    return best;
}

ExploreState Exploration::save() const {
    ExploreState s;
    s.used = used_;
    for (std::size_t i = 0; i < kMaxObjectives; ++i) s.progress[i] = progress_[i];
    return s;
}

void Exploration::load(const ExploreState& state) {
    const std::uint32_t mask = count_ == 32 ? 0xFFFFFFFFu : ((1u << count_) - 1u);
    used_ = state.used & mask;
    for (std::size_t i = 0; i < objectiveCount_; ++i)
        progress_[i] = state.progress[i] < required_[i] ? state.progress[i] : required_[i];
}

} // namespace ftcore
