#include "ftcore/LicenceRating.h"

#include <algorithm>

namespace ftcore {

const char* licenceStatName(LicenceStat s) {
    switch (s) {
    case LicenceStat::Reach: return "reach";
    case LicenceStat::Drive: return "drive";
    case LicenceStat::Nerve: return "nerve";
    case LicenceStat::Footwork: return "footwork";
    case LicenceStat::Insight: return "insight";
    default: return "none";
    }
}

int licenceCurveMilli(int stat) {
    const int s = std::clamp(stat, kStatMin, kStatMax);
    if (s <= kStatPlain) return (s - kStatPlain) * 1000;
    return (s - kStatPlain) * kStatKnee * 1000 / (kStatKnee + s - kStatPlain); // positive: truncates
}

int ratingNeed(int rating) {
    const int r = std::clamp(rating, 1, kMaxRating);
    return kRatingNeedBase + kRatingNeedStep * (r - 1);
}

int ratingXpTotal(int rating) {
    const int r = std::clamp(rating, 1, kMaxRating);
    return kRatingNeedBase * (r - 1) + (kRatingNeedStep / 2) * (r - 1) * (r - 2);
}

int ratingForXp(int xp) {
    int r = 1;
    while (r < kMaxRating && xp >= ratingXpTotal(r + 1)) ++r;
    return r;
}

int ratingAllowances(int rating) { return std::clamp(rating, 1, kMaxRating) / kAllowanceEvery; }

int fightXp(int target, FightXpKind kind, int rating) {
    int q = 35; // hundredths
    switch (kind) {
    case FightXpKind::Standard: q = 35; break;
    case FightXpKind::Elite: q = 60; break;
    case FightXpKind::Tutorial: q = 50; break;
    case FightXpKind::Boss: q = 125; break;
    default: return 0;
    }
    const int t = std::clamp(target, 1, kMaxRating);
    const int r = std::clamp(rating, 1, kMaxRating);
    const int c = std::clamp(100 + 35 * (t - r), 50, 200); // hundredths
    return (ratingNeed(t) * q * c + 5000) / 10000;          // half up
}

int LicenceRating::rating() const { return ratingForXp(xp); }

int LicenceRating::spentTotal() const {
    int n = 0;
    for (const std::uint8_t s : spent) n += s;
    return n;
}

int LicenceRating::unspent() const { return std::max(0, ratingAllowances(rating()) - spentTotal()); }

int LicenceRating::addXp(int amount) {
    if (amount <= 0) return 0;
    const int before = std::clamp(xp, 0, kMaxRatingXp);
    xp = static_cast<int>(std::min<long long>(static_cast<long long>(before) + amount, kMaxRatingXp));
    return xp - before;
}

bool LicenceRating::place(LicenceStat stat, const BriefDef& brief) {
    const std::size_t i = static_cast<std::size_t>(stat);
    if (i >= kLicenceStats || unspent() <= 0) return false;
    if (statsFor(brief).v[i] >= kStatMax) return false;
    ++spent[i];
    return true;
}

LicenceStats LicenceRating::statsFor(const BriefDef& brief, int statsRating) const {
    const int r = std::clamp(statsRating, 1, kMaxRating);
    LicenceStats s;
    for (std::size_t i = 0; i < kLicenceStats; ++i)
        s.v[i] = std::clamp(brief.base[i] + brief.growth[i] * (r - 1) / 100 + spent[i], kStatMin, kStatMax);
    return s;
}

LicenceSaveBytes encodeLicence(BriefId brief, const LicenceRating& r) {
    LicenceSaveBytes b{};
    const int xp = std::clamp(r.xp, 0, kMaxRatingXp);
    b[0] = kLicenceSaveVersion;
    b[1] = static_cast<std::uint8_t>(brief > BriefId::Count ? BriefId::Count : brief);
    b[2] = static_cast<std::uint8_t>(xp & 0xFF);
    b[3] = static_cast<std::uint8_t>((xp >> 8) & 0xFF);
    for (std::size_t i = 0; i < kLicenceStats; ++i) b[4 + i] = r.spent[i];
    return b;
}

bool decodeLicence(const LicenceSaveBytes& b, BriefId& brief, LicenceRating& r) {
    if (b[0] != kLicenceSaveVersion) return false;
    if (b[1] > static_cast<std::uint8_t>(BriefId::Count)) return false;
    LicenceRating out;
    out.xp = b[2] | (b[3] << 8);
    if (out.xp > kMaxRatingXp) return false;
    for (std::size_t i = 0; i < kLicenceStats; ++i) {
        if (b[4 + i] > kStatMax) return false;
        out.spent[i] = b[4 + i];
    }
    if (out.spentTotal() > ratingAllowances(out.rating())) out.refund(); // overspent: back to unspent
    brief = static_cast<BriefId>(b[1]);
    r = out;
    return true;
}

} // namespace ftcore
