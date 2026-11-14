#include "ftcore/WetPrints.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

int WetPrints::step(float phase, bool onFoot, float wetness, float dt, float dryScale) {
    if (std::isfinite(dt) && dt > 0.f && std::isfinite(dryScale) && dryScale > 0.f) clock_ += dt * dryScale;
    if (!onFoot || !std::isfinite(phase)) {
        phase_ = -1.f;
        return -1;
    }
    const float now = phase - std::floor(phase);
    const float was = phase_;
    phase_ = now;
    if (was < 0.f) return -1;
    // A phase below the last wrapped past 1 (the left plant at 0); within one step it can't also have passed 0.5.
    const bool wrapped = now < was;
    if ((wrapped ? now + 1.f - was : now - was) > kWetPrintMaxAdvance) return -1;
    WetFoot foot;
    if (wrapped) {
        foot = WetFoot::Left;
    } else if (was < 0.5f && now >= 0.5f) {
        foot = WetFoot::Right;
    } else {
        return -1;
    }
    const float stamp = std::isfinite(wetness) ? std::clamp(wetness, 0.f, 1.f) : 0.f;
    if (!(stamp > 0.f)) return -1;
    WetPrint& p = prints_[next_];
    p.stamp = stamp;
    p.laidAt = clock_;
    p.drops = std::clamp((stamp - kWetPrintDropsFrom) / (1.f - kWetPrintDropsFrom), 0.f, 1.f);
    p.foot = foot;
    p.serial = ++serial_;
    const int index = static_cast<int>(next_);
    next_ = (next_ + 1) % kWetPrintCap;
    return index;
}

float WetPrints::strength(std::size_t i) const {
    if (i >= kWetPrintCap) return 0.f;
    const WetPrint& p = prints_[i];
    if (p.serial == 0) return 0.f;
    return std::max(p.stamp - (clock_ - p.laidAt) / kWetPrintDryTime, 0.f);
}

std::size_t WetPrints::live() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < kWetPrintCap; ++i) n += strength(i) > 0.f ? 1u : 0u;
    return n;
}

void WetPrints::clear() {
    prints_ = {};
    next_ = 0;
    serial_ = 0;
    clock_ = 0.f;
    phase_ = -1.f;
}

} // namespace ftcore
