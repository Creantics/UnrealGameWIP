#pragma once

#include "ftcore/Api.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ftcore {

// Read cards: before an enemy action a reader is dealt up to kReadCards cards, each naming one enemy
// action and the tell it shows. Exactly one is the action that is coming, and it is the card whose
// tell that action shows; the others are decoys with tells of their own. A plain value type with a
// fixed buffer: dealing and picking never allocate. Battle (the Turns verb Read) deals before each
// enemy attack; another mode can deal from its own actions the same way.
inline constexpr std::size_t kReadCards = 3;
// The most correct Reads in a row a member keeps (each one strengthens its next Read counter).
inline constexpr int kMaxReadStacks = 5;

struct ReadCard {
    std::uint32_t actor = 0;  // the enemy whose action it is
    std::uint32_t action = 0; // that enemy's skill index
    std::string_view tell;    // what the action shows before it lands (points into the caller's data)
};

enum class ReadPick : std::uint8_t {
    Correct, // the card of the coming action
    Wrong,   // a decoy
    Refused, // nothing dealt, already picked, or no such card
};

class ReadCards {
public:
    // Deals for the coming action `pool[right]`: that card and up to kReadCards - 1 decoys from the
    // rest of `pool`, each with a tell unlike the right card's and every other decoy's. Which decoys,
    // and where the right card sits, follow from `seed` alone, so the same pool and seed always deal
    // the same cards. Returns the cards dealt: 0 (and nothing dealt) when `right` is out of range or no
    // other tell is in the pool, so there is nothing to read. Replaces any earlier deal.
    FTCORE_API std::size_t deal(std::span<const ReadCard> pool, std::size_t right, std::uint64_t seed);
    // Picks card `i`, once per deal: Correct for the right card, Wrong for a decoy, Refused when
    // nothing is dealt, a card was already picked, or `i` is past the last card.
    FTCORE_API ReadPick pick(std::size_t i);
    void clear() {
        count_ = 0;
        picked_ = kReadCards;
    }

    std::size_t size() const { return count_; }
    bool dealt() const { return count_ > 0; }
    bool picked() const { return picked_ < count_; }
    // The card picked (valid when picked()).
    std::size_t pickedIndex() const { return picked_; }
    // The right card's index (valid when dealt()).
    std::size_t correct() const { return correct_; }
    const ReadCard& operator[](std::size_t i) const { return cards_[i]; }
    std::span<const ReadCard> cards() const { return {cards_.data(), count_}; }

private:
    std::array<ReadCard, kReadCards> cards_{};
    std::size_t count_ = 0;
    std::size_t correct_ = 0;
    std::size_t picked_ = kReadCards; // kReadCards: none yet
};

} // namespace ftcore
