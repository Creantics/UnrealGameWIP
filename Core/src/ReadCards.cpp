#include "ftcore/ReadCards.h"

namespace ftcore {

namespace {

// splitmix64: a fresh, well-spread draw per call from a 64-bit state.
std::uint64_t nextDraw(std::uint64_t& state) {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

} // namespace

std::size_t ReadCards::deal(std::span<const ReadCard> pool, std::size_t right, std::uint64_t seed) {
    clear();
    if (right >= pool.size()) return 0;
    const std::string_view rightTell = pool[right].tell;
    std::array<std::size_t, kReadCards - 1> decoys{};
    std::size_t found = 0;
    std::uint64_t state = seed;
    const std::size_t n = pool.size();
    // Each decoy: from a drawn place in the pool, the first card whose tell is new to the deal.
    for (std::size_t k = 0; k < decoys.size(); ++k) {
        const std::size_t start = static_cast<std::size_t>(nextDraw(state) % n);
        for (std::size_t j = 0; j < n; ++j) {
            const std::size_t at = (start + j) % n;
            const std::string_view tell = pool[at].tell;
            bool fresh = tell != rightTell;
            for (std::size_t d = 0; d < found && fresh; ++d) fresh = pool[decoys[d]].tell != tell;
            if (!fresh) continue;
            decoys[found++] = at;
            break;
        }
    }
    if (found == 0) return 0;
    count_ = found + 1;
    correct_ = static_cast<std::size_t>(nextDraw(state) % count_);
    std::size_t next = 0;
    for (std::size_t i = 0; i < count_; ++i) cards_[i] = i == correct_ ? pool[right] : pool[decoys[next++]];
    return count_;
}

ReadPick ReadCards::pick(std::size_t i) {
    if (!dealt() || picked() || i >= count_) return ReadPick::Refused;
    picked_ = i;
    return i == correct_ ? ReadPick::Correct : ReadPick::Wrong;
}

} // namespace ftcore
