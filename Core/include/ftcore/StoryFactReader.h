#pragma once

// Read-only access to story facts by neutral id, for systems that read facts through an interface
// with this shape:
//     virtual int npcFactIndex(std::string_view id) const = 0;               // -1 when unknown
//     virtual bool npcFactValue(int index, std::uint8_t& value) const = 0;   // false unless Set
// StoryFactReader<Interface> implements it over a StoryState and its registry (both outlive it).
// Header only; no allocation. Index = the fact's slot.

#include "ftcore/StoryState.h"

#include <cstdint>
#include <string_view>

namespace ftcore {

template <class Interface>
class StoryFactReader final : public Interface {
public:
    StoryFactReader(const StoryState& state, const FactRegistry& reg) : state_(&state), reg_(&reg) {}

    int npcFactIndex(std::string_view id) const override {
        for (std::uint32_t slot = 1; slot < kFactSlots; ++slot) {
            const FactRow& row = reg_->facts[slot];
            if (row.used && row.kind != FactKind::Retired && row.id() == id) return static_cast<int>(slot);
        }
        return -1;
    }

    bool npcFactValue(int index, std::uint8_t& value) const override {
        if (index <= 0 || index >= static_cast<int>(kFactSlots)) return false;
        const FactRead r = storyGet(*state_, *reg_, static_cast<std::uint16_t>(index));
        if (r.status != FactStatus::Set) return false;
        value = r.value;
        return true;
    }

private:
    const StoryState* state_;
    const FactRegistry* reg_;
};

} // namespace ftcore
