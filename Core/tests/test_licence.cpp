// The Lattice lead's licence: stats and their curve, Rating and xp, allowances, the 9-byte save, and BriefKit (stats
// into arena fields), checked against the design's comparison table at Rating 1, 10, 20 and 30.
#include "ftcore/ArenaFight.h"
#include "ftcore/Brief.h"
#include "ftcore/BriefKit.h"
#include "ftcore/Json.h"
#include "ftcore/LicenceRating.h"
#include "ftcore/LicenceStats.h"

#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

ArenaTable shippedArena() {
    std::string text;
    ArenaTable t;
    if (!ftcore_test::readProjectData("Battle/arena.json", text)) return t;
    std::string err;
    importArenaTable(text, t, &err);
    return t;
}

bool shippedBriefs(const ArenaFighter& lead, BriefTable& out, std::string& err) {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/briefs.json", text)) {
        err = "Data/Battle/briefs.json not found";
        return false;
    }
    return importBriefTable(text, lead, out, &err);
}

// The other three Briefs' bases, growths and changes, for the comparison table until they ship with their passives.
constexpr const char* kFixtureBriefs = R"({ "version": 1,
  "common": { "rounds": 6, "reload": 1.2, "shot": { "stamina": 0 } },
  "briefs": [
    { "id": "counsel", "base": [8, 9, 8, 10, 15], "growth": [35, 45, 40, 40, 90],
      "change": { "hp": 0.90, "readPower": 1.20, "readStackPower": 1.25 } },
    { "id": "proxy", "base": [9, 9, 11, 11, 10], "growth": [40, 45, 60, 55, 50], "kit": { "pairWindow": 0.17 } },
    { "id": "escrow", "base": [10, 8, 11, 7, 14], "growth": [50, 35, 55, 40, 70],
      "change": { "loadShare": 1.35, "parryWindowAdd": 0.010, "releaseDamage": 1.30, "dodgeDistance": 0.85,
                  "invuln": 0.85 },
      "kit": { "releasePoise": 1.25 } } ] })";

// Shipped Bailiff and Notary, and the fixture's other three.
struct AllBriefs {
    ArenaFighter duy;
    BriefTable table;
    bool ok = false;
};
AllBriefs allBriefs() {
    AllBriefs a;
    const ArenaTable arena = shippedArena();
    a.duy = arena.fighter("duy");
    BriefTable shipped, fixture;
    std::string err;
    if (!shippedBriefs(a.duy, shipped, err)) return a;
    if (!importBriefTable(kFixtureBriefs, a.duy, fixture, &err)) return a;
    for (std::size_t i = 0; i < kBriefCount; ++i)
        a.table.briefs[i] = shipped.briefs[i].present ? shipped.briefs[i] : fixture.briefs[i];
    a.ok = true;
    return a;
}

#define FT_SAME(field) CHECK_MESSAGE(a.field == b.field, #field)

void sameAttack(const ArenaAttack& a, const ArenaAttack& b) {
    FT_SAME(startup); FT_SAME(active); FT_SAME(recovery); FT_SAME(range); FT_SAME(arcCos); FT_SAME(width);
    FT_SAME(damage); FT_SAME(poiseDamage); FT_SAME(guardDamage); FT_SAME(lunge); FT_SAME(tracking); FT_SAME(armor);
    FT_SAME(stamina); FT_SAME(cancel); FT_SAME(cancelGuard); FT_SAME(cancelRoll); FT_SAME(cancelLight);
    FT_SAME(hitStop); FT_SAME(hitStopSelf); FT_SAME(jolt); FT_SAME(maxTargets);
}

// Every field BriefKit writes, and the ones around them, bit for bit.
void sameFighter(const ArenaFighter& a, const ArenaFighter& b) {
    FT_SAME(hp); FT_SAME(stamina); FT_SAME(staminaRegen); FT_SAME(poise); FT_SAME(moveSpeed); FT_SAME(lightCount);
    FT_SAME(dodgeTime); FT_SAME(dodgeDistance); FT_SAME(dodgeInvulnStart); FT_SAME(dodgeInvulnEnd);
    FT_SAME(guardAbsorb); FT_SAME(parryWindow); FT_SAME(loadShare); FT_SAME(releasePoise); FT_SAME(pairWindow);
    FT_SAME(readPower); FT_SAME(readStackPower); FT_SAME(readPoise); FT_SAME(readWrongPoise); FT_SAME(rounds);
    FT_SAME(reload); FT_SAME(healCharges); FT_SAME(healAmount);
    for (std::size_t i = 0; i < a.light.size(); ++i) sameAttack(a.light[i], b.light[i]);
    sameAttack(a.heavy, b.heavy);
    sameAttack(a.riposte, b.riposte);
    sameAttack(a.jumpLight, b.jumpLight);
    sameAttack(a.shot, b.shot);
    sameAttack(a.release, b.release);
}

#undef FT_SAME

LicenceStats statsAt(const BriefDef& d, int rating) { return LicenceRating{}.statsFor(d, rating); }

float invuln(const ArenaFighter& f) { return f.dodgeInvulnEnd - f.dodgeInvulnStart; }
long readCounter(const ArenaFighter& f) { return std::lround(f.readPower * static_cast<float>(f.light[0].damage)); }
long suretyPerParry(const ArenaFighter& f) { return std::lround(18.f * f.loadShare); }

} // namespace

TEST_CASE("licence: the stat curve D (thousandths, truncated)") {
    CHECK(licenceCurveMilli(10) == 0);
    CHECK(licenceCurveMilli(1) == -9000);
    CHECK(licenceCurveMilli(50) == 17142);
    CHECK(licenceCurveMilli(22) == 8571); // 8571.43, truncated
    CHECK(licenceCurveMilli(7) == -3000);
    CHECK(licenceCurveMilli(16) == 5000);
    CHECK(licenceCurveMilli(20) == 7500);
    CHECK(licenceCurveMilli(40) == 15000);
    CHECK(licenceCurveMilli(0) == -9000);   // held to 1..50
    CHECK(licenceCurveMilli(99) == 17142);
    for (int s = 1; s < kStatMax; ++s) CHECK(licenceCurveMilli(s) < licenceCurveMilli(s + 1));
}

TEST_CASE("licence: the Rating curve, fight xp and allowances") {
    CHECK(ratingXpTotal(1) == 0);
    CHECK(ratingXpTotal(2) == 100);
    CHECK(ratingXpTotal(5) == 520);
    CHECK(ratingXpTotal(8) == 1120);
    CHECK(ratingXpTotal(10) == 1620);
    CHECK(ratingXpTotal(20) == 5320);
    CHECK(ratingXpTotal(30) == 11020);
    CHECK(kMaxRatingXp == 11020);
    CHECK(ratingNeed(1) == 100);
    CHECK(ratingNeed(30) == 680);
    CHECK(ratingForXp(0) == 1);
    CHECK(ratingForXp(1119) == 7); // one xp short of Rating 8
    CHECK(ratingForXp(1120) == 8);
    CHECK(ratingForXp(1134) == 8);
    CHECK(ratingForXp(99999) == 30);

    LicenceRating r;
    CHECK(r.addXp(99999) == 11020); // xp past the cap is dropped
    CHECK(r.xp == 11020);
    CHECK(r.rating() == 30);
    CHECK(r.addXp(500) == 0);
    CHECK(r.addXp(-5) == 0);

    CHECK(fightXp(7, FightXpKind::Boss, 7) == 275);   // a boss at the lead's own Rating
    CHECK(fightXp(30, FightXpKind::Boss, 30) == 850);
    CHECK(fightXp(1, FightXpKind::Standard, 3) == 18); // c 0.5 (0.3 clamped): the least a fight gives
    CHECK(fightXp(10, FightXpKind::Standard, 12) == fightXp(10, FightXpKind::Standard, 20)); // both clamp to 0.5
    CHECK(fightXp(10, FightXpKind::Standard, 6) == (ratingNeed(10) * 35 * 200 + 5000) / 10000); // c 2.0 (2.4)
    CHECK(fightXp(30, FightXpKind::Boss, 26) == 1700);                                         // the most

    CHECK(ratingAllowances(1) == 0);
    CHECK(ratingAllowances(2) == 1);
    CHECK(ratingAllowances(29) == 14);
    CHECK(ratingAllowances(30) == 15);
}

TEST_CASE("licence: a Brief's stats by Rating, allowances placed and refused") {
    const AllBriefs a = allBriefs();
    REQUIRE(a.ok);
    const BriefDef& bailiff = *a.table.find(BriefId::Bailiff);
    const BriefDef& notary = *a.table.find(BriefId::Notary);
    const BriefDef& counsel = *a.table.find(BriefId::Counsel);
    CHECK(statsAt(bailiff, 1).v == bailiff.base);
    CHECK(statsAt(bailiff, 20).v == std::array<int, 5>{30, 12, 25, 15, 13});
    CHECK(statsAt(notary, 30).v[0] == 14);

    LicenceRating r;
    r.addXp(ratingXpTotal(20));
    CHECK(r.unspent() == 10);
    CHECK(r.place(LicenceStat::Insight, counsel));
    CHECK(r.unspent() == 9);
    CHECK(r.statsFor(counsel).v[4] == statsAt(counsel, 20).v[4] + 1);
    r.refund();
    CHECK(r.unspent() == 10);

    // Counsel Insight at Rating 30 is 41; nine allowances bring it to 50, the tenth is refused and stays unspent.
    LicenceRating top;
    top.addXp(kMaxRatingXp);
    for (int i = 0; i < 9; ++i) REQUIRE(top.place(LicenceStat::Insight, counsel));
    CHECK(top.statsFor(counsel).v[4] == 50);
    CHECK_FALSE(top.place(LicenceStat::Insight, counsel));
    CHECK(top.unspent() == 6);
    CHECK(top.spent[4] == 9);

    LicenceRating none; // Rating 1 has no allowances
    CHECK_FALSE(none.place(LicenceStat::Reach, counsel));
}

TEST_CASE("licence: the 9-byte save keeps the Brief, xp and spends") {
    const AllBriefs a = allBriefs();
    REQUIRE(a.ok);
    const BriefDef& notary = *a.table.find(BriefId::Notary);
    LicenceRating r;
    r.xp = 5320;
    r.spent = {2, 0, 3, 0, 5};
    const LicenceSaveBytes bytes = encodeLicence(BriefId::Notary, r);
    CHECK(bytes == LicenceSaveBytes{1, 2, 0xC8, 0x14, 2, 0, 3, 0, 5});
    BriefId b = BriefId::Count;
    LicenceRating back;
    REQUIRE(decodeLicence(bytes, b, back));
    CHECK(b == BriefId::Notary);
    CHECK(back.xp == 5320);
    CHECK(back.spent == r.spent);
    CHECK(back.rating() == 20);
    CHECK(encodeLicence(b, back) == bytes);
    const ArenaFighter before = buildBriefKit(notary, r.statsFor(notary)).fighter;
    const ArenaFighter after = buildBriefKit(notary, back.statsFor(notary)).fighter;
    CHECK(before.hp == after.hp);
    CHECK(before.shot.damage == after.shot.damage);

    // None (a save from before Briefs) loads; a bad version, Brief, xp or spend is refused and changes nothing.
    LicenceSaveBytes none = bytes;
    none[1] = static_cast<std::uint8_t>(BriefId::Count);
    REQUIRE(decodeLicence(none, b, back));
    CHECK(b == BriefId::Count);
    const auto refused = [&](std::size_t at, std::uint8_t v) {
        LicenceSaveBytes bad = bytes;
        bad[at] = v;
        BriefId keep = BriefId::Proxy;
        LicenceRating kept;
        kept.xp = 7;
        CHECK_FALSE(decodeLicence(bad, keep, kept));
        CHECK(keep == BriefId::Proxy);
        CHECK(kept.xp == 7);
    };
    refused(0, 2);    // version
    refused(1, 6);    // past Count
    refused(3, 0x2B); // xp 11208 > 11020
    refused(4, 51);   // a spend past 50

    // Spends past what the Rating earned are refunded, not refused.
    LicenceRating over;
    over.xp = ratingXpTotal(4); // 2 earned
    over.spent = {3, 0, 0, 0, 0};
    REQUIRE(decodeLicence(encodeLicence(BriefId::Bailiff, over), b, back));
    CHECK(back.spentTotal() == 0);
    CHECK(back.unspent() == 2);
}

TEST_CASE("brief kit: every stat 10 builds the slice-1 kits field for field, and the plain kit is the lead's own") {
    const ArenaTable arena = shippedArena();
    const ArenaFighter& duy = arena.fighter("duy");
    BriefTable t;
    std::string err;
    REQUIRE_MESSAGE(shippedBriefs(duy, t, err), err);

    // Slice 1 shipped the kit changes as absolute values; laid over duy + common they are the golden kits.
    // The air light came later and takes the Reach row like the heavy (13 x 1.10 = 14, 14 x 1.20 = 16.8; 13 x 0.90 = 12).
    const char* slice1[2] = {
        R"({ "rounds": 6, "reload": 1.2, "shot": { "stamina": 0, "startup": 0.26, "recovery": 0.38 },
             "poise": 31.25, "guardAbsorb": 0.88,
             "light": [ { "damage": 12, "poiseDamage": 12 }, { "damage": 12, "poiseDamage": 12 },
                        { "damage": 19, "poiseDamage": 24 } ],
             "heavy": { "damage": 40, "poiseDamage": 54 }, "riposte": { "damage": 48 },
             "jumpLight": { "damage": 14, "poiseDamage": 16.8 } })",
        R"({ "rounds": 6, "reload": 1.2, "shot": { "stamina": 0, "damage": 12, "range": 36, "poiseDamage": 7.5 },
             "light": [ { "damage": 10 }, { "damage": 10 }, { "damage": 15 } ],
             "heavy": { "damage": 32 }, "riposte": { "damage": 40 }, "jumpLight": { "damage": 12 } })"};
    const BriefId ids[2] = {BriefId::Bailiff, BriefId::Notary};
    for (int i = 0; i < 2; ++i) {
        CAPTURE(i);
        JsonDocument doc;
        REQUIRE(doc.parse(slice1[i], &err));
        ArenaFighter golden = duy;
        std::vector<std::string> errors, warnings;
        arenaOverlayFighter(doc.root(), "slice1", golden, errors, &warnings);
        REQUIRE(errors.empty());
        sameFighter(buildBriefKit(*t.find(ids[i]), LicenceStats{}).fighter, golden);
    }

    // No Brief change and every stat 10: the lead's own kit (plus the cylinder).
    BriefDef plain;
    plain.kit = t.find(BriefId::Bailiff)->kit; // duy + common, no stat-driven changes
    const BriefKitBuild k = buildBriefKit(plain, LicenceStats{});
    sameFighter(k.fighter, plain.kit);
    CHECK(k.fighter.hp == 135);
    CHECK(k.fighter.light[0].damage == 11);
    CHECK(k.fighter.shot.damage == 10);
    CHECK(k.fighter.parryWindow == doctest::Approx(0.13f));
    CHECK(k.termDuration == doctest::Approx(1.f));
}

TEST_CASE("brief kit: the comparison table, every Brief at Rating 1, 10, 20 and 30") {
    const AllBriefs a = allBriefs();
    REQUIRE(a.ok);
    struct Row {
        BriefId brief;
        int rating;
        std::array<int, 5> stats;
        int hp, l1, heavy, shot;
        float poise, absorb, parry, invuln;
        long readCtr, surety;
    };
    using B = BriefId;
    const Row rows[] = {
        {B::Counsel, 1, {8, 9, 8, 10, 15}, 113, 10, 33, 10, 23.0f, 0.8410f, 0.1270f, 0.3500f, 7, 20},
        // The table shows invuln .3614: 0.35 x 1.032724 = 0.361453, which the one half-up rounding makes .3615.
        {B::Counsel, 10, {11, 13, 11, 13, 23}, 126, 11, 37, 11, 26.0f, 0.8544f, 0.1315f, 0.3615f, 10, 23},
        {B::Counsel, 20, {14, 17, 15, 17, 32}, 140, 13, 41, 13, 29.3f, 0.8693f, 0.1364f, 0.3738f, 13, 25},
        {B::Counsel, 30, {18, 22, 19, 21, 41}, 151, 14, 45, 14, 31.9f, 0.8812f, 0.1404f, 0.3838f, 15, 26},
        {B::Bailiff, 1, {14, 7, 12, 9, 8}, 144, 14, 45, 9, 33.6f, 0.8867f, 0.1328f, 0.3458f, 6, 17},
        {B::Bailiff, 10, {21, 9, 18, 12, 10}, 165, 16, 52, 10, 39.1f, 0.9027f, 0.1395f, 0.3579f, 8, 18},
        {B::Bailiff, 20, {30, 12, 25, 15, 13}, 182, 18, 59, 11, 43.8f, 0.9160f, 0.1450f, 0.3680f, 10, 19},
        {B::Bailiff, 30, {38, 15, 32, 19, 16}, 195, 19, 63, 12, 47.1f, 0.9257f, 0.1490f, 0.3791f, 12, 21},
        {B::Notary, 1, {7, 15, 9, 10, 9}, 130, 9, 29, 14, 24.0f, 0.8455f, 0.1285f, 0.3500f, 4, 17},
        {B::Notary, 10, {9, 23, 13, 14, 13}, 148, 10, 31, 17, 27.7f, 0.8623f, 0.1341f, 0.3648f, 6, 19},
        {B::Notary, 20, {11, 32, 17, 18, 17}, 162, 10, 34, 19, 30.7f, 0.8755f, 0.1385f, 0.3765f, 6, 21},
        {B::Notary, 30, {14, 41, 22, 23, 22}, 175, 11, 37, 20, 33.6f, 0.8886f, 0.1429f, 0.3881f, 8, 23},
        {B::Proxy, 1, {9, 9, 11, 11, 10}, 140, 11, 35, 10, 26.0f, 0.8544f, 0.1315f, 0.3541f, 6, 18},
        {B::Proxy, 10, {12, 13, 16, 15, 14}, 159, 12, 39, 11, 30.0f, 0.8725f, 0.1375f, 0.3680f, 7, 20},
        {B::Proxy, 20, {16, 17, 22, 21, 19}, 175, 13, 43, 13, 33.6f, 0.8886f, 0.1429f, 0.3838f, 9, 22},
        {B::Proxy, 30, {20, 22, 28, 26, 24}, 188, 14, 47, 14, 36.25f, 0.9006f, 0.1469f, 0.3938f, 10, 23},
        {B::Escrow, 1, {10, 8, 11, 7, 14}, 140, 11, 36, 9, 26.0f, 0.8544f, 0.1415f, 0.2868f, 6, 27},
        {B::Escrow, 10, {14, 11, 15, 10, 20}, 155, 13, 41, 10, 29.3f, 0.8693f, 0.1464f, 0.2975f, 9, 30},
        {B::Escrow, 20, {19, 14, 21, 14, 27}, 173, 14, 46, 12, 33.0f, 0.8862f, 0.1521f, 0.3101f, 11, 32},
        {B::Escrow, 30, {24, 18, 26, 18, 34}, 184, 15, 50, 13, 35.4f, 0.8970f, 0.1557f, 0.3200f, 12, 34},
    };
    for (const Row& row : rows) {
        const BriefDef& d = *a.table.find(row.brief);
        const LicenceStats s = statsAt(d, row.rating);
        const ArenaFighter f = buildBriefKit(d, s).fighter;
        char line[200];
        std::snprintf(line, sizeof line,
                      "%-8s R%-2d %2d/%2d/%2d/%2d/%2d hp %3d L1 %2d heavy %2d shot %2d poise %.4f absorb %.4f parry "
                      "%.4f invuln %.4f read %ld surety %ld",
                      briefName(row.brief), row.rating, s.v[0], s.v[1], s.v[2], s.v[3], s.v[4], f.hp,
                      f.light[0].damage, f.heavy.damage, f.shot.damage, static_cast<double>(f.poise),
                      static_cast<double>(f.guardAbsorb), static_cast<double>(f.parryWindow),
                      static_cast<double>(invuln(f)), readCounter(f), suretyPerParry(f));
        MESSAGE(line);
        CAPTURE(line);
        CHECK(s.v == row.stats);
        CHECK(f.hp == row.hp);
        CHECK(f.light[0].damage == row.l1);
        CHECK(f.heavy.damage == row.heavy);
        CHECK(f.shot.damage == row.shot);
        CHECK(f.poise == doctest::Approx(row.poise).epsilon(0).scale(0).epsilon(0.0016)); // the table shows 1 decimal
        CHECK(std::fabs(f.guardAbsorb - row.absorb) < 0.00006f);
        CHECK(std::fabs(f.parryWindow - row.parry) < 0.00006f);
        CHECK(std::fabs(invuln(f) - row.invuln) < 0.00006f);
        CHECK(readCounter(f) == row.readCtr);
        CHECK(suretyPerParry(f) == row.surety);
    }
}

TEST_CASE("brief kit: worked cases for each Brief") {
    const AllBriefs a = allBriefs();
    REQUIRE(a.ok);
    const auto kit = [&](BriefId b, int rating) { return buildBriefKit(*a.table.find(b), statsAt(*a.table.find(b), rating)); };

    const ArenaFighter counsel1 = kit(BriefId::Counsel, 1).fighter;
    CHECK(counsel1.hp == 113);
    CHECK(counsel1.heavy.damage == 33);
    CHECK(counsel1.parryWindow == doctest::Approx(0.127f));
    CHECK(counsel1.readPower == doctest::Approx(0.7286f).epsilon(0.0001)); // abs clamp, then x1.20
    CHECK(readCounter(counsel1) == 7);

    const ArenaFighter notary30 = kit(BriefId::Notary, 30).fighter;
    CHECK(notary30.shot.damage == 20);
    CHECK(notary30.shot.range == doctest::Approx(36.f));
    CHECK(notary30.hp == 175);
    CHECK(kit(BriefId::Notary, 1).termDuration == doctest::Approx(1.25f * (1.f + 0.03f * 4.286f)).epsilon(0.001));

    CHECK(kit(BriefId::Proxy, 30).fighter.pairWindow == doctest::Approx(0.17f)); // a kit absolute, no stat

    const ArenaFighter escrow1 = kit(BriefId::Escrow, 1).fighter;
    CHECK(escrow1.dodgeInvulnEnd == doctest::Approx(0.05f + 0.2868f));
    CHECK(escrow1.dodgeDistance == doctest::Approx(2.9036f));
    CHECK(escrow1.releasePoise == doctest::Approx(1.25f));

    CHECK(kit(BriefId::Bailiff, 1).fighter.shot.damage == 9); // 8.5 rounds half up

    // The release (Enforce) takes the Reach mult only, x1.30 for Escrow.
    CHECK(kit(BriefId::Escrow, 10).fighter.release.damage == 45);
    CHECK(kit(BriefId::Counsel, 10).fighter.release.damage == 31);
    CHECK(kit(BriefId::Escrow, 1).fighter.release.damage == 39);
    CHECK(kit(BriefId::Bailiff, 30).fighter.release.damage == 47);

    // Every stat at 50.
    LicenceStats top;
    top.v = {50, 50, 50, 50, 50};
    for (std::size_t i = 0; i < kBriefCount; ++i) {
        const BriefId b = static_cast<BriefId>(i);
        const ArenaFighter f = buildBriefKit(*a.table.find(b), top).fighter;
        CAPTURE(briefName(b));
        CHECK(f.hp == (b == BriefId::Counsel ? 194 : 216));
        const float len = b == BriefId::Escrow ? 0.34f : 0.40f;
        CHECK(invuln(f) == doctest::Approx(len));
        CHECK(f.dodgeInvulnEnd <= f.dodgeTime);
        if (b == BriefId::Escrow) CHECK(f.loadShare == doctest::Approx(1.9575f));
    }
}

TEST_CASE("brief kit: the air light takes the Reach row like the heavy") {
    const AllBriefs a = allBriefs();
    REQUIRE(a.ok);
    for (std::size_t i = 0; i < kBriefCount; ++i) {
        BriefDef d = *a.table.find(static_cast<BriefId>(i));
        d.kit.jumpLight.damage = 12;
        d.kit.jumpLight.poiseDamage = 14.f;
        BriefDef twin = d;
        twin.kit.heavy.damage = d.kit.jumpLight.damage;
        twin.kit.heavy.poiseDamage = d.kit.jumpLight.poiseDamage;
        for (int rating : {1, 10, 30}) {
            CAPTURE(i);
            CAPTURE(rating);
            const ArenaFighter f = buildBriefKit(d, statsAt(d, rating)).fighter;
            const ArenaFighter t = buildBriefKit(twin, statsAt(twin, rating)).fighter;
            CHECK(f.jumpLight.damage == t.heavy.damage);
            CHECK(f.jumpLight.poiseDamage == t.heavy.poiseDamage);
        }
    }
}

TEST_CASE("brief kit: every Brief, Rating and stat extreme stays inside the final clamps") {
    const AllBriefs a = allBriefs();
    REQUIRE(a.ok);
    int built = 0;
    for (std::size_t i = 0; i < kBriefCount; ++i) {
        const BriefDef& d = *a.table.find(static_cast<BriefId>(i));
        for (int rating = 1; rating <= kMaxRating; ++rating) {
            for (int lowHigh = 0; lowHigh < 3; ++lowHigh) {
                LicenceStats s = statsAt(d, rating);
                if (lowHigh == 1) s.v = {1, 1, 1, 1, 1};
                if (lowHigh == 2) s.v = {50, 50, 50, 50, 50};
                const BriefKitBuild k = buildBriefKit(d, s);
                const ArenaFighter& f = k.fighter;
                ++built;
                bool inside = f.hp >= 90 && f.hp <= 230 && f.poise >= 15.f && f.poise <= 60.f;
                for (std::size_t l = 0; l < f.lightCount; ++l)
                    inside = inside && f.light[l].damage >= 1 && f.light[l].damage <= 200 &&
                             f.light[l].poiseDamage >= 1.f && f.light[l].poiseDamage <= 120.f;
                inside = inside && f.shot.damage >= 1 && f.shot.damage <= 60 && f.release.damage >= 1 &&
                         f.release.damage <= 200;
                inside = inside && f.guardAbsorb >= 0.8f && f.guardAbsorb <= 0.95f && f.parryWindow >= 0.11f &&
                         f.parryWindow <= 0.17f;
                inside = inside && invuln(f) >= 0.25f - 1e-6f && invuln(f) <= 0.40f + 1e-6f &&
                         f.dodgeInvulnEnd <= 0.45f + 1e-6f && f.dodgeInvulnEnd <= f.dodgeTime;
                inside = inside && f.dodgeDistance >= 2.8f && f.dodgeDistance <= 3.8f && f.moveSpeed >= 4.7f &&
                         f.moveSpeed <= 5.1f;
                inside = inside && f.readPower >= 0.3f && f.readPower <= 1.2f && f.loadShare >= 0.8f &&
                         f.loadShare <= 2.f && k.termDuration >= 0.5f && k.termDuration <= 2.f;
                if (!inside) {
                    CAPTURE(briefName(static_cast<BriefId>(i)));
                    CAPTURE(rating);
                    CAPTURE(lowHigh);
                    CHECK(inside);
                }
            }
        }
    }
    CHECK(built == 5 * 30 * 3);
}

TEST_CASE("briefs: a Brief's kit may not set a stat-driven field; change keys are checked") {
    const ArenaTable arena = shippedArena();
    const ArenaFighter& duy = arena.fighter("duy");
    const std::string row = R"("id": "notary", "base": [7, 15, 9, 10, 9], "growth": [25, 90, 45, 45, 45])";
    const auto refused = [&](const std::string& rest, const char* what) {
        BriefTable t;
        std::string err;
        CHECK_FALSE(importBriefTable(R"({ "version": 1, "briefs": [ { )" + row + rest + " } ] }", duy, t, &err));
        CHECK_MESSAGE(err.find(what) != std::string::npos, err);
        CHECK(t.find(BriefId::Notary) == nullptr);
    };
    refused(R"(, "kit": { "hp": 150 })", "kit.hp");
    refused(R"(, "kit": { "shot": { "damage": 12 } })", "kit.shot.damage");
    refused(R"(, "kit": { "light": [ { "poiseDamage": 3 } ] })", "kit.light[0].poiseDamage");
    refused(R"(, "kit": { "riposte": { "damage": 40 } })", "kit.riposte.damage");
    refused(R"(, "change": { "shotDamage": 3 })", "change.shotDamage");
    refused(R"(, "change": { "luck": 1.1 })", "unknown key: luck");
    BriefTable t;
    std::string err;
    CHECK_MESSAGE(importBriefTable(R"({ "version": 1, "briefs": [ { )" + row +
                                       R"(, "kit": { "riposte": { "poiseDamage": 9 }, "shot": { "range": 36 } } } ] })",
                                   duy, t, &err),
                  err); // the riposte's poise and the shot's range are no stat's
}

TEST_CASE("brief kit: what every stat 10 does to each lead row (no clamp moves a kit silently)") {
    // The value-clamped rows (invuln, dodge, move, the reads), the absorb and the parry clamp even at a mult of 1,
    // so a row outside them changes at plain stats. The guns lead's row must come through untouched; the others
    // (never given a Brief: the runner refuses) are pinned with exactly the fields the clamps move, so a retune
    // that crosses a clamp shows up here.
    const ArenaTable arena = shippedArena();
    REQUIRE_FALSE(arena.fighters.empty());
    struct Moves {
        const char* id;
        float moveSpeed, guardAbsorb, parryWindow; // the clamped values; 0 = the row's own
    };
    const Moves leads[] = {
        {"duy", 0.f, 0.f, 0.f},
        {"devin", 4.8f, 0.95f, 0.155f},
        {"third", 4.8f, 0.8f, 0.f},
        {"default", 4.8f, 0.95f, 0.f},
    };
    for (const Moves& m : leads) {
        CAPTURE(std::string(m.id));
        BriefDef plain;
        plain.kit = arena.fighter(m.id);
        ArenaFighter expect = plain.kit;
        if (m.moveSpeed > 0.f) expect.moveSpeed = m.moveSpeed;
        if (m.guardAbsorb > 0.f) expect.guardAbsorb = m.guardAbsorb;
        if (m.parryWindow > 0.f) expect.parryWindow = m.parryWindow;
        sameFighter(buildBriefKit(plain, LicenceStats{}).fighter, expect);
    }
}

TEST_CASE("brief kit: a change outside its bounds is held to them, a non-finite one is no change") {
    const ArenaTable arena = shippedArena();
    BriefDef d;
    d.kit = arena.fighter("duy");
    const ArenaFighter plain = buildBriefKit(d, LicenceStats{}).fighter;
    BriefDef wild = d;
    wild.change.hp = 9.f;
    wild.change.kickDamage = 0.01f;
    wild.change.parryWindowAdd = 1.f;
    wild.change.poise = std::nanf("");
    wild.change.loadShare = -INFINITY;
    BriefDef bound = d;
    bound.change.hp = kBriefChangeMax;
    bound.change.kickDamage = kBriefChangeMin;
    bound.change.parryWindowAdd = kBriefParryAddMax;
    bound.change.loadShare = 1.f;
    const ArenaFighter w = buildBriefKit(wild, LicenceStats{}).fighter;
    const ArenaFighter b = buildBriefKit(bound, LicenceStats{}).fighter;
    CHECK(w.hp == b.hp);
    CHECK(w.light[0].damage == b.light[0].damage);
    CHECK(w.parryWindow == b.parryWindow);
    CHECK(w.poise == plain.poise);
    CHECK(w.loadShare == plain.loadShare);
}
