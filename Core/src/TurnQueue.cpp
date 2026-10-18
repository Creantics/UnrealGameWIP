#include "ftcore/TurnQueue.h"

#include "ftcore/Assert.h"

#include <algorithm>
#include <array>
#include <limits>

namespace ftcore {

void TurnQueue::add(CombatantId id, double speed, double initialGauge) {
    FTCORE_ASSERT(speed > 0.0);
    FTCORE_ASSERT(!contains(id));
    entries_.push_back({id, speed, initialGauge});
}

void TurnQueue::remove(CombatantId id) {
    std::erase_if(entries_, [id](const Entry& e) { return e.id == id; });
}

void TurnQueue::setSpeed(CombatantId id, double speed) {
    FTCORE_ASSERT(speed > 0.0);
    for (Entry& e : entries_)
        if (e.id == id) e.speed = speed;
}

bool TurnQueue::contains(CombatantId id) const {
    return std::any_of(entries_.begin(), entries_.end(), [id](const Entry& e) { return e.id == id; });
}

std::size_t TurnQueue::pickNext(const Entry* entries, std::size_t n, double& outTime) {
    std::size_t best = 0;
    double bestTime = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < n; ++i) {
        const Entry& e = entries[i];
        const double t = std::max(0.0, (kThreshold - e.gauge) / e.speed);
        const Entry& b = entries[best];
        const bool better = t < bestTime - 1e-9 ||
                            (t <= bestTime + 1e-9 && (e.speed > b.speed || (e.speed == b.speed && e.id < b.id)));
        if (i == 0 || better) {
            best = i;
            bestTime = std::min(bestTime, t);
        }
    }
    outTime = bestTime;
    return best;
}

CombatantId TurnQueue::advance(Entry* entries, std::size_t n) {
    double dt = 0.0;
    const std::size_t idx = pickNext(entries, n, dt);
    for (std::size_t i = 0; i < n; ++i) entries[i].gauge += entries[i].speed * dt;
    entries[idx].gauge = 0.0;
    return entries[idx].id;
}

CombatantId TurnQueue::next() {
    FTCORE_ASSERT(!entries_.empty());
    return advance(entries_.data(), entries_.size());
}

std::vector<CombatantId> TurnQueue::preview(std::size_t count) const {
    std::vector<CombatantId> out;
    if (entries_.empty()) return out;
    std::vector<Entry> sim = entries_;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) out.push_back(advance(sim.data(), sim.size()));
    return out;
}

std::size_t TurnQueue::preview(std::span<CombatantId> out) const {
    if (entries_.empty()) return 0;
    if (entries_.size() > kPreviewStack) {
        const std::vector<CombatantId> v = preview(out.size());
        std::copy(v.begin(), v.end(), out.begin());
        return v.size();
    }
    std::array<Entry, kPreviewStack> sim;
    std::copy(entries_.begin(), entries_.end(), sim.begin());
    for (CombatantId& id : out) id = advance(sim.data(), entries_.size());
    return out.size();
}

} // namespace ftcore
