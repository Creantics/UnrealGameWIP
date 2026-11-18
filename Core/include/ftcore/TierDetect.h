#pragma once

// First-launch tier pick (FRAME_BUDGET_1117 sections 0.5 and 4): the player never tunes to get 60.
// 1. The adapter name is looked up in a table of cards with their GPU factor vs the reference PC (an RTX 3080 Ti):
//    how much longer a GPU-bound frame takes there. The longest matching name wins ("rtx 4060 laptop" over
//    "rtx 4060"); a card missing from the table but named like a slower one (an "RTX 3060 Ti" reads as "rtx 3060")
//    gets the slower card's factor, which errs safe.
// 2. An unknown card: the engine's synth benchmark GPU index, against the reference PC's index (factor = ref / index).
//    With neither, Low (the safe pick).
// 3. The factor gives the tier; VRAM caps it (under 7 GB: Low at most; under 5.5 GB: Very Low); under 6 cores flags
//    the CPU-heavy knobs (clutter, animation) to Low.
// 4. A short check behind the first load screen steps down one tier while a thread's p95 is over the limit, twice at
//    most (TierCheck).
// 5. The saved pick is kept until the adapter or driver changes (fingerprint).
// Plain data and arithmetic: no allocation, no exceptions.
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ftcore {

enum class Tier : std::uint8_t { VeryLow = 0, Low, Medium, High, Top };

struct GpuEntry {
    std::string_view name; // lower case, matched as a substring of the adapter name (case ignored)
    float factor;          // 1080p raster frame-time factor vs the reference PC (1.0)
    float rtFactor;        // the same with ray tracing on; 0: no RT hardware
};

// Section 0.2 / 0.5 factors (Tom's Hardware 2026 table where it has the card, PassMark-scaled where not: E).
inline constexpr GpuEntry kGpuTable[] = {
    {"rtx 5090", 0.6f, 0.5f},          {"rtx 5080", 0.75f, 0.7f},         {"rtx 4090", 0.6f, 0.55f},
    {"rtx 4080", 0.8f, 0.75f},         {"rtx 5070 ti", 0.85f, 0.8f},      {"rtx 5070", 0.95f, 0.95f},
    {"rtx 4070 super", 1.0f, 1.0f},    {"rx 9070 xt", 0.95f, 1.1f},       {"rtx 3080 ti", 1.0f, 1.0f},
    {"rtx 5060", 1.35f, 1.58f},        {"rtx 4060 laptop", 1.85f, 2.1f},  {"rtx 4060", 1.67f, 1.92f},
    {"arc b580", 1.67f, 1.85f},        {"rx 7600", 1.71f, 4.04f},         {"rtx 5050", 1.73f, 2.07f},
    {"rtx 3060 laptop", 2.3f, 2.5f},   {"rtx 3060", 1.94f, 2.13f},        {"rtx 4050 laptop", 2.35f, 2.6f},
    {"rx 6600", 2.3f, 5.05f},          {"rtx 3050", 2.68f, 3.3f},         {"gtx 1660 super", 2.7f, 0.f},
    {"gtx 1650", 5.0f, 0.f},
};

struct HardwareInfo {
    std::string_view adapter;   // as the RHI reports it, any case
    std::uint32_t vramMB = 0;   // dedicated video memory
    std::uint32_t cores = 0;    // physical cores
    float benchGpuIndex = -1.f; // the synth benchmark's GPU index; below 0: not run
    std::uint32_t displayHeight = 1080;
};

struct TierRules {
    float benchRefIndex = 0.f;  // the reference PC's synth benchmark GPU index; 0: not calibrated (unknown cards go Low)
    float topMax = 1.1f;        // factor at or under which Top is picked
    float highMax = 1.95f;      // High (1080p) or Medium (1440p display)
    float lowMax = 2.75f;       // Low; above: Very Low
    float rtMax = 2.0f;         // hardware RT only on Top and only at or under this RT factor
    std::uint32_t lowVramMB = 7000;
    std::uint32_t veryLowVramMB = 5500;
    std::uint32_t lowCpuCores = 6;
};

struct TierPick {
    Tier tier = Tier::Low;
    float factor = 0.f;          // 0: unknown
    bool known = false;          // the adapter matched the table
    bool fromBench = false;      // the factor came from the synth benchmark
    bool hardwareRt = false;
    bool lowTextures = false;    // VRAM under lowVramMB
    bool lowCpu = false;         // fewer than lowCpuCores: clutter and animation at Low
};

namespace tierdetect_detail {
constexpr char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
constexpr bool containsNoCase(std::string_view hay, std::string_view needle) {
    if (needle.empty() || needle.size() > hay.size()) {
        return needle.empty();
    }
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        std::size_t j = 0;
        while (j < needle.size() && lower(hay[i + j]) == needle[j]) {
            ++j;
        }
        if (j == needle.size()) {
            return true;
        }
    }
    return false;
}
} // namespace tierdetect_detail

// The table entry for an adapter name: the longest matching name, or nullptr.
template <std::size_t N>
constexpr const GpuEntry* findGpu(std::string_view adapter, const GpuEntry (&table)[N]) {
    const GpuEntry* best = nullptr;
    for (const GpuEntry& e : table) {
        if (tierdetect_detail::containsNoCase(adapter, e.name) && (!best || e.name.size() > best->name.size())) {
            best = &e;
        }
    }
    return best;
}

constexpr Tier minTier(Tier a, Tier b) { return static_cast<std::uint8_t>(a) < static_cast<std::uint8_t>(b) ? a : b; }

// The tier one step down (the check's step and the in-play fallback): Top -> High -> Low; Medium -> Low; Low ->
// Very Low; Very Low stays. Medium (1440p) and High (1080p) are side by side on the same cards, so both fall to Low.
constexpr Tier stepDown(Tier t) {
    switch (t) {
    case Tier::Top: return Tier::High;
    case Tier::High:
    case Tier::Medium: return Tier::Low;
    case Tier::Low:
    case Tier::VeryLow: return Tier::VeryLow;
    }
    return Tier::VeryLow;
}

template <std::size_t N>
constexpr TierPick pickTier(const HardwareInfo& hw, const TierRules& rules, const GpuEntry (&table)[N]) {
    TierPick p;
    const GpuEntry* e = findGpu(hw.adapter, table);
    float rt = 0.f;
    if (e) {
        p.known = true;
        p.factor = e->factor;
        rt = e->rtFactor;
    } else if (hw.benchGpuIndex > 0.f && rules.benchRefIndex > 0.f) {
        p.fromBench = true;
        p.factor = rules.benchRefIndex / hw.benchGpuIndex;
    }
    if (p.factor <= 0.f) {
        p.tier = Tier::Low;
    } else if (p.factor <= rules.topMax) {
        p.tier = Tier::Top;
    } else if (p.factor <= rules.highMax) {
        p.tier = hw.displayHeight >= 1440 ? Tier::Medium : Tier::High;
    } else if (p.factor <= rules.lowMax) {
        p.tier = Tier::Low;
    } else {
        p.tier = Tier::VeryLow;
    }
    if (hw.vramMB > 0 && hw.vramMB < rules.veryLowVramMB) {
        p.tier = Tier::VeryLow;
    } else if (hw.vramMB > 0 && hw.vramMB < rules.lowVramMB) {
        p.tier = minTier(p.tier, Tier::Low);
    }
    p.lowTextures = hw.vramMB > 0 && hw.vramMB < rules.lowVramMB;
    p.lowCpu = hw.cores > 0 && hw.cores < rules.lowCpuCores;
    p.hardwareRt = p.tier == Tier::Top && rt > 0.f && rt <= rules.rtMax;
    return p;
}

inline TierPick pickTier(const HardwareInfo& hw, const TierRules& rules = {}) { return pickTier(hw, rules, kGpuTable); }

// The check behind the first load screen: feed the worst thread p95 (ms) of each short run at the current tier.
struct TierCheck {
    Tier tier = Tier::Low;
    float limitMs = 15.f;
    int stepsLeft = 2;
    bool done = false;

    // True when another run is wanted (the tier stepped down); false once the tier holds or the steps run out.
    bool ran(float worstP95Ms) {
        if (done) {
            return false;
        }
        if (worstP95Ms <= limitMs || stepsLeft <= 0 || tier == Tier::VeryLow) {
            done = true;
            return false;
        }
        tier = stepDown(tier);
        --stepsLeft;
        return true;
    }
};

// The saved pick holds until the adapter or driver changes: FNV-1a over both strings.
constexpr std::uint64_t hardwareFingerprint(std::string_view adapter, std::string_view driver) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](std::string_view s) {
        for (char c : s) {
            h ^= static_cast<std::uint8_t>(tierdetect_detail::lower(c));
            h *= 1099511628211ull;
        }
        h ^= 0xFFu; // separator, so "ab"+"c" and "a"+"bc" differ
        h *= 1099511628211ull;
    };
    mix(adapter);
    mix(driver);
    return h;
}

} // namespace ftcore
