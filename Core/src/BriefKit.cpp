#include "ftcore/BriefKit.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ftcore {

namespace {

using i64 = std::int64_t;

// Fixed point: values in ten-thousandths (e4), multipliers in ten-millionths (e7), their product in e11, times a
// change in e4 gives e15. Bases are held to 0..1000 (1e7 in e4), so e15 stays below 4.1e18.
constexpr i64 kE3 = 1000, kE7 = 10'000'000, kE11 = 100'000'000'000, kE15 = 1'000'000'000'000'000;

i64 toE4(float f) {
    if (!std::isfinite(f)) return 0;
    const double v = std::clamp(static_cast<double>(f) * 1e4, -1e13, 1e13);
    return static_cast<i64>(std::llround(v));
}
i64 baseE4(float f) { return std::clamp<i64>(toE4(f), 0, 1000 * 10'000); }
float fromE4(i64 v) { return static_cast<float>(static_cast<double>(v) / 1e4); }

// A Brief change in e4, held to its bounds here too (not only at import); a non-finite one is no change.
i64 changeE4(float c) {
    if (!std::isfinite(c)) return 10'000;
    return std::clamp<i64>(toE4(c), toE4(kBriefChangeMin), toE4(kBriefChangeMax));
}
i64 parryAddE4(float c) {
    if (!std::isfinite(c)) return 0;
    return std::clamp<i64>(toE4(c), -toE4(kBriefParryAddMax), toE4(kBriefParryAddMax));
}

struct Row {
    int k4;         // per stat point, ten-thousandths (0.040 -> 400)
    bool abs;       // the stat clamp is on the value, not the multiplier
    i64 lo4, hi4;   // stat clamp: multiplier (x) or value (abs), e4
    i64 fin4, fax4; // final clamp, e4
};

// base x mult (stat-clamped) x change, final-clamped, in e15.
i64 build(i64 base4, int dMilli, const Row& r, i64 change4) {
    i64 m = kE7 + static_cast<i64>(r.k4) * dMilli;
    i64 v = 0;
    if (r.abs) {
        v = std::clamp(base4 * m, r.lo4 * kE7, r.hi4 * kE7);
    } else {
        m = std::clamp(m, r.lo4 * kE3, r.hi4 * kE3);
        v = base4 * m;
    }
    return std::clamp(v * change4, r.fin4 * kE11, r.fax4 * kE11);
}

int roundWhole(i64 e15) { return static_cast<int>((e15 + kE15 / 2) / kE15); }
i64 roundE4(i64 e15) { return (e15 + kE11 / 2) / kE11; }

int wholeField(int base, int dMilli, const Row& r, float change) {
    return roundWhole(build(baseE4(static_cast<float>(base)), dMilli, r, changeE4(change)));
}
float floatField(float base, int dMilli, const Row& r, float change) {
    return fromE4(roundE4(build(baseE4(base), dMilli, r, changeE4(change))));
}

// The rows (BriefKit.h's table).
constexpr Row kKick{400, false, 8000, 17000, 1 * 10'000, 200 * 10'000};
constexpr Row kKickPoise{500, false, 8000, 18000, 1 * 10'000, 120 * 10'000};
constexpr Row kShot{500, false, 8000, 18000, 1 * 10'000, 60 * 10'000};
constexpr Row kShotPoise{300, false, 8500, 14500, 1 * 10'000, 20 * 10'000};
constexpr Row kTerm{300, false, 8500, 15000, 5000, 20000};
constexpr Row kHp{350, false, 8500, 16000, 90 * 10'000, 230 * 10'000};
constexpr Row kPoise{400, false, 8500, 17000, 15 * 10'000, 60 * 10'000};
constexpr Row kStamina{200, false, 9000, 13500, 85 * 10'000, 140 * 10'000};
constexpr Row kRegen{250, false, 9000, 14000, 40 * 10'000, 65 * 10'000};
constexpr Row kInvuln{120, true, 3200, 4000, 2500, 4000};
constexpr Row kDodge{80, true, 33000, 38000, 28000, 38000};
constexpr Row kMove{40, true, 48000, 51000, 47000, 51000};
constexpr Row kReadPower{500, true, 4000, 9500, 3000, 12000};
constexpr Row kReadStack{300, true, 1500, 3200, 1000, 4000};
constexpr Row kReadPoise{600, true, 40000, 110000, 30000, 120000};
constexpr Row kLoadShare{300, false, 9000, 14500, 8000, 20000};

} // namespace

BriefKitBuild buildBriefKit(const BriefDef& brief, const LicenceStats& stats) {
    const BriefChange& c = brief.change;
    const int reach = licenceCurveMilli(stats[LicenceStat::Reach]);
    const int drive = licenceCurveMilli(stats[LicenceStat::Drive]);
    const int nerve = licenceCurveMilli(stats[LicenceStat::Nerve]);
    const int foot = licenceCurveMilli(stats[LicenceStat::Footwork]);
    const int insight = licenceCurveMilli(stats[LicenceStat::Insight]);

    BriefKitBuild out;
    out.fighter = brief.kit;
    ArenaFighter& f = out.fighter;
    const ArenaFighter& b = brief.kit;

    const std::size_t lights = std::min<std::size_t>(b.lightCount, b.light.size());
    for (std::size_t i = 0; i < lights; ++i) {
        f.light[i].damage = wholeField(b.light[i].damage, reach, kKick, c.kickDamage);
        f.light[i].poiseDamage = floatField(b.light[i].poiseDamage, reach, kKickPoise, c.kickPoiseDamage);
    }
    f.heavy.damage = wholeField(b.heavy.damage, reach, kKick, c.kickDamage);
    f.heavy.poiseDamage = floatField(b.heavy.poiseDamage, reach, kKickPoise, c.kickPoiseDamage);
    f.riposte.damage = wholeField(b.riposte.damage, reach, kKick, c.kickDamage);
    f.release.damage = wholeField(b.release.damage, reach, kKick, c.releaseDamage);

    f.shot.damage = wholeField(b.shot.damage, drive, kShot, c.shotDamage);
    f.shot.poiseDamage = floatField(b.shot.poiseDamage, drive, kShotPoise, c.shotPoiseDamage);
    out.termDuration = floatField(1.f, drive, kTerm, c.termDuration);

    f.hp = wholeField(b.hp, nerve, kHp, c.hp);
    f.poise = floatField(b.poise, nerve, kPoise, c.poise);
    f.stamina = floatField(b.stamina, nerve, kStamina, 1.f);
    {
        // Guard leak: 1 - absorb, x clamp(1 - 0.03 D, 0.50, 1.15) x change; rounded, then absorb = 1 - leak.
        const i64 leak4 = std::clamp<i64>(10'000 - toE4(b.guardAbsorb), 0, 10'000);
        const i64 m = std::clamp<i64>(kE7 - static_cast<i64>(300) * nerve, 5000 * kE3, 11500 * kE3);
        const i64 leak = roundE4(leak4 * m * changeE4(c.guardLeak));
        f.guardAbsorb = fromE4(std::clamp<i64>(10'000 - leak, 8000, 9500));
    }
    {
        // Parry window: clamp(base + 0.0015 D, 0.12, 0.155) + add, final 0.11..0.17 (in e7).
        const i64 v = std::clamp<i64>(baseE4(b.parryWindow) * kE3 + static_cast<i64>(15) * nerve, 1200 * kE3,
                                      1550 * kE3);
        const i64 w = std::clamp<i64>(v + parryAddE4(c.parryWindowAdd) * kE3, 1100 * kE3, 1700 * kE3);
        f.parryWindow = fromE4((w + kE3 / 2) / kE3);
    }

    f.staminaRegen = floatField(b.staminaRegen, foot, kRegen, 1.f);
    {
        const i64 start4 = baseE4(b.dodgeInvulnStart);
        const i64 len4 = std::max<i64>(0, baseE4(b.dodgeInvulnEnd) - start4);
        const i64 l4 = roundE4(build(len4, foot, kInvuln, changeE4(c.invuln)));
        const i64 end4 = std::min({start4 + l4, static_cast<i64>(4500), baseE4(b.dodgeTime)});
        f.dodgeInvulnEnd = fromE4(end4);
    }
    f.dodgeDistance = floatField(b.dodgeDistance, foot, kDodge, c.dodgeDistance);
    f.moveSpeed = floatField(b.moveSpeed, foot, kMove, 1.f);

    f.readPower = floatField(b.readPower, insight, kReadPower, c.readPower);
    f.readStackPower = floatField(b.readStackPower, insight, kReadStack, c.readStackPower);
    f.readPoise = floatField(b.readPoise, insight, kReadPoise, 1.f);
    f.loadShare = floatField(b.loadShare, insight, kLoadShare, c.loadShare);
    return out;
}

} // namespace ftcore
