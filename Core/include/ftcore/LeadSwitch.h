// The player's lead switch (FightSession::setActiveLead): why one would be refused, and its tuning.
// Plain C++20, no Unreal headers. Data/Battle/lead_switch.json, read by importLeadSwitchTuning.
#pragma once

#include "ftcore/Api.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace ftcore {

// What a switch to a lead gets at the step's close (FightSession::leadSwitchReason previews it; the close uses the
// same function). Ok: control passes to it. Same: it already has control (nothing to report). The rest refuse it.
enum class LeadSwitch : std::uint8_t {
    Ok,
    Same,       // that lead already has control
    NotInFight, // not one of the two leads (the third slot never is), or a lead not in this fight
    Down,       // that lead is down
    FightOver,  // the fight is lost (a defeat) or finished as won
    Cooldown,   // too soon after the last accepted switch (leadSwitchCooldownSeconds)
    Lone,       // a Lone Carry is on and that lead is not the Lone one
};

inline constexpr double kLeadSwitchCooldownMax = 5.0; // seconds
inline constexpr double kLeadSwitchCooldownDefault = 1.0;

struct LeadSwitchTuning {
    // Fight seconds after an accepted switch during which another is refused (Cooldown). A takeover by a fall is
    // never blocked by it and does not start it. 0..kLeadSwitchCooldownMax.
    double cooldownSeconds = kLeadSwitchCooldownDefault;
};

inline bool validLeadSwitchCooldown(double seconds) {
    return std::isfinite(seconds) && seconds >= 0.0 && seconds <= kLeadSwitchCooldownMax;
}

// Reads Data/Battle/lead_switch.json: {"version":1, "leadSwitchCooldownSeconds" (0..5)}; the field is optional
// (its default kept). On any error `out` is the defaults, false is returned and `error` lists every problem.
FTCORE_API bool importLeadSwitchTuning(std::string_view json, LeadSwitchTuning& out, std::string* error = nullptr);

} // namespace ftcore
