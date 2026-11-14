#include "ftcore/UiLone.h"

#include "AllocCounter.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <limits>

using namespace ftcore;

namespace {

UiLoneInput live(bool alone) {
    UiLoneInput in;
    in.fight = true;
    in.alone = alone;
    return in;
}

// Steps `seconds` at 1/60 s with the same input.
void run(UiLone& s, const UiLoneInput& in, float seconds) {
    for (float t = 0.f; t < seconds - 1e-4f; t += 1.f / 60.f) uiLoneStep(s, in, 1.f / 60.f);
}

UiLone begun(bool full) {
    UiLone s;
    UiLoneInput in = live(true);
    in.began = true;
    in.full = full;
    uiLoneStep(s, in, 1.f / 60.f);
    return s;
}

} // namespace

TEST_CASE("ui lone: the card's columns come from the bargain's fields") {
    const UiLoneCard c = uiLoneCard(loneBargain(LoneTuning{}));
    REQUIRE(c.gainCount == 3);
    CHECK(c.gain[0].kind == UiLoneLine::Release);
    CHECK(c.gain[0].figure.view() == "\xC3\x97" "1.3");
    CHECK(c.gain[1].kind == UiLoneLine::Load);
    CHECK(c.gain[1].figure.view() == "\xC3\x97" "1.5");
    CHECK(c.gain[2].kind == UiLoneLine::LoadCap);
    CHECK(c.gain[2].figure.view() == "100");
    REQUIRE(c.giveCount == 5);
    CHECK(c.give[0].kind == UiLoneLine::DamageTaken);
    CHECK(c.give[0].figure.view() == "\xC3\x97" "1.15");
    CHECK(c.give[1].kind == UiLoneLine::NoCount);
    CHECK(c.give[2].kind == UiLoneLine::NoTable);
    CHECK(c.give[3].kind == UiLoneLine::NoTakeHalf);
    CHECK(c.give[4].kind == UiLoneLine::GiftEnds);
    for (std::size_t i = 1; i < c.giveCount; ++i) CHECK(c.give[i].figure.empty());
}

TEST_CASE("ui lone: other tunings show their own figures; a refusal that does not hold has no line") {
    LoneTuning t;
    t.cap = 60;
    t.gain = 2.f;
    t.release = 1.25f;
    t.taken = 2.f;
    LoneBargain b = loneBargain(t);
    b.noTable = false;
    b.noTakeHalf = false;
    const UiLoneCard c = uiLoneCard(b);
    REQUIRE(c.gainCount == 3);
    CHECK(c.gain[0].figure.view() == "\xC3\x97" "1.25");
    CHECK(c.gain[1].figure.view() == "\xC3\x97" "2");
    CHECK(c.gain[2].figure.view() == "60");
    REQUIRE(c.giveCount == 3);
    CHECK(c.give[0].figure.view() == "\xC3\x97" "2");
    CHECK(c.give[1].kind == UiLoneLine::NoCount);
    CHECK(c.give[2].kind == UiLoneLine::GiftEnds); // the gift's end stays last
}

TEST_CASE("ui lone: a multiplier at x1 or no cap changes nothing and has no line") {
    const UiLoneCard c = uiLoneCard(LoneBargain{}); // no session: x1 throughout, no cap, every refusal held
    CHECK(c.gainCount == 0);
    REQUIRE(c.giveCount == 4);
    CHECK(c.give[0].kind == UiLoneLine::NoCount);
    LoneBargain b;
    b.release = 1.004f; // x1.00 to the hundredth
    b.loadGain = std::numeric_limits<float>::quiet_NaN();
    b.damageTaken = 0.9f;
    b.loadCap = -5;
    b.noCount = b.noTable = b.noTakeHalf = b.giftReceivedEnds = false;
    const UiLoneCard d = uiLoneCard(b);
    CHECK(d.gainCount == 0);
    CHECK(d.giveCount == 0);
}

TEST_CASE("ui lone: the roll's lines, none before the first lone Release") {
    CHECK(uiLoneRoll(LoneRoll{}).count == 0);
    LoneSave save;
    const UiLoneRoll none = uiLoneRoll(loneRoll(save, 0u, 0u, LonePoolTuning{}));
    CHECK(none.count == 0);

    // 4 Releases pooling 500 Load against the defaults: B = 400 x 500 / 2500 = 80 permille, no figure.
    const UiLoneRoll small = uiLoneRoll(loneRoll(save, 4u, 500u, LonePoolTuning{}));
    REQUIRE(small.count == 4);
    CHECK(small.rows[0].kind == UiLoneRollLine::Releases);
    CHECK(small.rows[0].figure.view() == "4");
    CHECK(small.rows[1].kind == UiLoneRollLine::Pooled);
    CHECK(small.rows[1].figure.view() == "500");
    CHECK(small.rows[2].kind == UiLoneRollLine::Extra);
    CHECK(small.rows[2].figure.view() == "+8%");
    CHECK(small.rows[3].kind == UiLoneRollLine::BossHp);
    CHECK(small.rows[3].figure.view() == "+8%");

    // The save's 30 and 6000 plus a fight's 2 and 1000: B = 400 x 7000 / 9000 = 311 permille; two figures of 150
    // leave 11 on the boss's hp.
    save.releases = 30u;
    save.pooled = 6000u;
    const UiLoneRoll big = uiLoneRoll(loneRoll(save, 2u, 1000u, LonePoolTuning{}));
    REQUIRE(big.count == 6);
    CHECK(big.rows[0].figure.view() == "32");
    CHECK(big.rows[1].figure.view() == "7000");
    CHECK(big.rows[2].figure.view() == "+31.1%");
    CHECK(big.rows[3].kind == UiLoneRollLine::Figures);
    CHECK(big.rows[3].figure.view() == "2");
    CHECK(big.rows[4].kind == UiLoneRollLine::FigureShare);
    CHECK(big.rows[4].figure.view() == "15%");
    CHECK(big.rows[5].kind == UiLoneRollLine::BossHp);
    CHECK(big.rows[5].figure.view() == "+1.1%");

    // Saturated counts still fit their figure.
    LoneRoll most;
    most.releases = 0xFFFFFFFFu;
    most.pooled = 0xFFFFFFFFu;
    most.effect = lonePoolEffect(most.pooled, LonePoolTuning{});
    const UiLoneRoll m = uiLoneRoll(most);
    CHECK(m.rows[0].figure.view() == "4294967295");
}

TEST_CASE("ui lone: the card holds six seconds, then fades over half a second") {
    UiLone s = begun(true);
    CHECK(s.full);
    CHECK(uiLoneCardAlpha(s) == 1.f);
    run(s, live(true), 5.9f);
    CHECK(uiLoneCardAlpha(s) == 1.f);
    run(s, live(true), 0.2f);
    CHECK(uiLoneCardAlpha(s) < 1.f);
    CHECK(uiLoneCardAlpha(s) > 0.5f);
    run(s, live(true), 0.5f);
    CHECK(uiLoneCardAlpha(s) == 0.f);
    CHECK_FALSE(s.card);
    CHECK(uiLoneWord(s) == UiLoneWord::Alone); // the word stays while it lasts
}

TEST_CASE("ui lone: the first attack ends the hold; past the first showings the title alone") {
    UiLone s = begun(false);
    CHECK_FALSE(s.full);
    run(s, live(true), 1.f);
    CHECK(uiLoneCardAlpha(s) == 1.f);
    UiLoneInput hit = live(true);
    hit.attack = true;
    uiLoneStep(s, hit, 1.f / 60.f);
    CHECK_FALSE(s.holding);
    run(s, live(true), 0.25f);
    CHECK(uiLoneCardAlpha(s) == doctest::Approx(0.5f).epsilon(0.08));
    run(s, live(true), 0.3f);
    CHECK(uiLoneCardAlpha(s) == 0.f);
    // An attack on the tick it begins does not end it.
    UiLone t;
    UiLoneInput both = live(true);
    both.began = true;
    both.attack = true;
    uiLoneStep(t, both, 1.f / 60.f);
    CHECK(t.holding);
}

TEST_CASE("ui lone: a gift's end shows ENDED for a second and a half; the partner's its own word; the fight's none") {
    UiLone s = begun(true);
    run(s, live(true), 1.f);
    UiLoneInput gift = live(false);
    gift.endedBy = LoneEnd::Gift;
    uiLoneStep(s, gift, 1.f / 60.f);
    CHECK(uiLoneWord(s) == UiLoneWord::EndedGift);
    CHECK_FALSE(s.holding); // the card goes with it
    run(s, live(false), 1.4f);
    CHECK(uiLoneWord(s) == UiLoneWord::EndedGift);
    run(s, live(false), 0.15f);
    CHECK(uiLoneWord(s) == UiLoneWord::None);

    UiLone p = begun(false);
    UiLoneInput partner = live(false);
    partner.endedBy = LoneEnd::PartnerCalled;
    uiLoneStep(p, partner, 1.f / 60.f);
    CHECK(uiLoneWord(p) == UiLoneWord::EndedPartner);

    // The fight's own end: nothing, whether the session says FightEnded or the fight is simply over.
    UiLone f = begun(true);
    UiLoneInput over = live(false);
    over.endedBy = LoneEnd::FightEnded;
    uiLoneStep(f, over, 1.f / 60.f);
    CHECK(uiLoneWord(f) == UiLoneWord::None);
    UiLone g = begun(true);
    uiLoneStep(g, UiLoneInput{}, 1.f / 60.f);
    CHECK(uiLoneWord(g) == UiLoneWord::None);
    CHECK(uiLoneCardAlpha(g) == 0.f);
}

TEST_CASE("ui lone: never alone, nothing shows") {
    UiLone s;
    run(s, live(false), 3.f);
    CHECK(uiLoneWord(s) == UiLoneWord::None);
    CHECK(uiLoneCardAlpha(s) == 0.f);
    // Alone with no card seen (the bargain's moment missed): the word alone.
    run(s, live(true), 0.5f);
    CHECK(uiLoneWord(s) == UiLoneWord::Alone);
    CHECK(uiLoneCardAlpha(s) == 0.f);
}

TEST_CASE("ui lone: the state slot's priority is RELEASE, LOAD, then the Lone Carry's word") {
    CHECK(uiLeadWord(UiLeadSlot::Release, UiLoneWord::Alone) == UiLeadWord::Release);
    CHECK(uiLeadWord(UiLeadSlot::Load, UiLoneWord::Alone) == UiLeadWord::Load);
    CHECK(uiLeadWord(UiLeadSlot::None, UiLoneWord::Alone) == UiLeadWord::Alone);
    CHECK(uiLeadWord(UiLeadSlot::None, UiLoneWord::EndedGift) == UiLeadWord::EndedGift);
    CHECK(uiLeadWord(UiLeadSlot::Load, UiLoneWord::EndedPartner) == UiLeadWord::Load);
    CHECK(uiLeadWord(UiLeadSlot::None, UiLoneWord::EndedPartner) == UiLeadWord::EndedPartner);
    CHECK(uiLeadWord(UiLeadSlot::None, UiLoneWord::None) == UiLeadWord::None);
}

TEST_CASE("ui lone: building and stepping allocate nothing") {
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    LoneSave save;
    save.releases = 3u;
    const UiLoneCard c = uiLoneCard(loneBargain(LoneTuning{}));
    const UiLoneRoll r = uiLoneRoll(loneRoll(save, 1u, 900u, LonePoolTuning{}));
    UiLone s = begun(true);
    run(s, live(true), 7.f);
    if (counting) CHECK(ftcore_test::allocStats().count == before);
    CHECK(c.gainCount == 3);
    CHECK(r.count > 0);
}
