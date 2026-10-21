#include "ftcore/Banter.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace ftcore;

namespace {

// Invented fixture text.
constexpr const char* kBanter = R"J({
  "version": 1, "scene": "EX-02", "beat": 5, "step": 1,
  "lines": [
    { "speaker": "a", "text": "Short." },
    { "speaker": "b", "text": "A longer line that takes a little more time to read through.", "hold": 3.0 },
    { "speaker": "a", "text": "Third." },
    { "speaker": "c", "text": "Someone else." }
  ]
})J";

BanterDef load(const char* json) {
    BanterDef d;
    std::string err;
    REQUIRE_MESSAGE(importBanter(json, d, &err), err);
    return d;
}

} // namespace

TEST_CASE("banter: a file loads with its key, lines and speakers") {
    std::vector<std::string> warnings;
    BanterDef d;
    std::string err;
    REQUIRE_MESSAGE(importBanter(kBanter, d, &err, &warnings), err);
    CHECK(warnings.empty());
    CHECK(d.matches("EX-02", 5, 1));
    CHECK_FALSE(d.matches("EX-02", 5, 0));
    CHECK(d.lineCount == 4);
    CHECK(d.speakerCount == 3);
    CHECK(d.speakerIndex("b") == 1);
    CHECK(d.speakerIndex("zzz") == 3);
    CHECK(d.lines[1].hold == doctest::Approx(3.0));
    CHECK(banterFileName("EX-02", 5, 1) == "EX-02_5_1.json");
}

TEST_CASE("banter: a line's hold is derived from its length and clamped") {
    BanterLine l;
    l.text = "Hi";
    CHECK(banterHold(l) == doctest::Approx(kBanterMinHold));
    l.text = std::string(400, 'x');
    CHECK(banterHold(l) == doctest::Approx(kBanterMaxHold));
    l.text = std::string(60, 'x');
    CHECK(banterHold(l) == doctest::Approx(kBanterHoldBase + 60 * kBanterHoldPerByte));
    l.hold = 0.7;
    CHECK(banterHold(l) == doctest::Approx(0.7));
}

TEST_CASE("banter: lines advance on their own, sides follow who spoke first") {
    const BanterDef d = load(kBanter);
    BanterSequence b;
    b.begin(&d);
    REQUIRE(b.active());
    CHECK(b.line() == 0);
    CHECK(b.speaker() == "a");
    CHECK(b.text() == "Short.");
    CHECK(b.side() == BanterSide::Left);
    const std::uint32_t e0 = b.epoch();
    CHECK_FALSE(b.tick(kBanterMinHold - 0.1));
    CHECK(b.tick(0.2));
    CHECK(b.epoch() != e0);
    CHECK(b.line() == 1);
    CHECK(b.side() == BanterSide::Right);
    CHECK(b.lineAge() == doctest::Approx(0.1));
    CHECK(b.lineHold() == doctest::Approx(3.0));
    CHECK(b.tick(3.0));
    CHECK(b.line() == 2);
    CHECK(b.side() == BanterSide::Left);
    CHECK(b.press());
    CHECK(b.line() == 3);
    CHECK(b.speaker() == "c");
    CHECK(b.side() == BanterSide::Left); // a third speaker is on the left again
    CHECK(b.tick(100.0));
    CHECK(b.finished());
    CHECK(b.text().empty());
    CHECK(b.linesShown() == 4);
    CHECK(b.presses() == 1);
    CHECK_FALSE(b.skipped());
    CHECK_FALSE(b.press());
}

TEST_CASE("banter: one long frame runs through several lines") {
    const BanterDef d = load(kBanter);
    BanterSequence b;
    b.begin(&d);
    CHECK(b.tick(kBanterMinHold + 3.0 + 0.5));
    CHECK(b.line() == 2);
    CHECK(b.lineAge() == doctest::Approx(0.5));
    CHECK(d.length() == doctest::Approx(kBanterMinHold + 3.0 + kBanterMinHold + (kBanterHoldBase + 13 * kBanterHoldPerByte)));
}

TEST_CASE("banter: skip ends at once; no definition is finished") {
    const BanterDef d = load(kBanter);
    BanterSequence b;
    b.begin(&d);
    CHECK(b.skip());
    CHECK(b.finished());
    CHECK(b.skipped());
    CHECK(b.linesShown() == 1);
    CHECK_FALSE(b.skip());
    CHECK_FALSE(b.tick(1.0));

    BanterSequence none;
    none.begin(nullptr);
    CHECK(none.finished());
    CHECK(none.speaker().empty());
    CHECK_FALSE(none.press());
}

TEST_CASE("banter: bad files are refused with paths only, unknown keys are warnings") {
    BanterDef d;
    std::string err;
    const auto bad = [&](const char* json) {
        err.clear();
        return !importBanter(json, d, &err);
    };
    CHECK(bad("nope"));
    CHECK(bad(R"J({ "version": 2, "scene": "A", "beat": 0, "step": 0, "lines": [ { "speaker": "a", "text": "x" } ] })J"));
    CHECK(bad(R"J({ "version": 1, "beat": 0, "step": 0, "lines": [ { "speaker": "a", "text": "x" } ] })J"));
    CHECK(bad(R"J({ "version": 1, "scene": "A", "step": 0, "lines": [ { "speaker": "a", "text": "x" } ] })J"));
    CHECK(bad(R"J({ "version": 1, "scene": "A", "beat": 0, "step": 0, "lines": [] })J"));
    CHECK(bad(R"J({ "version": 1, "scene": "A", "beat": 0, "step": 0, "lines": [ "just a string" ] })J"));
    CHECK(bad(R"J({ "version": 1, "scene": "A", "beat": 0, "step": 0, "lines": [ { "text": "secret words" } ] })J"));
    CHECK(err.find("lines[0].speaker") != std::string::npos);
    CHECK(bad(R"J({ "version": 1, "scene": "A", "beat": 0, "step": 0, "lines": [ { "speaker": "a", "text": "secret words", "hold": 99 } ] })J"));
    CHECK(err.find("lines[0].hold") != std::string::npos);
    CHECK(err.find("secret") == std::string::npos); // never line text
    CHECK(bad(R"J({ "version": 1, "scene": "A", "beat": 0, "step": 0, "lines": [
        { "speaker": "a", "text": "x" }, { "speaker": "b", "text": "x" }, { "speaker": "c", "text": "x" },
        { "speaker": "d", "text": "x" }, { "speaker": "e", "text": "x" } ] })J"));
    CHECK(d.lineCount == 0); // failure leaves it empty

    std::string many = R"J({ "version": 1, "scene": "A", "beat": 0, "step": 0, "lines": [)J";
    for (std::uint32_t i = 0; i <= BanterDef::kMaxLines; ++i) many += std::string(i ? "," : "") + R"J({ "speaker": "a", "text": "x" })J";
    many += "] }";
    CHECK(bad(many.c_str()));

    std::vector<std::string> warnings;
    REQUIRE(importBanter(R"J({ "version": 1, "scene": "A", "beat": 0, "step": 0, "mood": 1,
                               "lines": [ { "speaker": "a", "text": "x", "face": "smile" } ] })J",
                         d, &err, &warnings));
    CHECK(warnings.size() == 2);
}

TEST_CASE("banter: the committed example loads") {
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Banter/example_banter.json", text));
    BanterDef d;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importBanter(text, d, &err, &warnings), err);
    CHECK(warnings.empty());
    CHECK(d.matches("EX-01", 2, 0));
    CHECK(d.speakerCount == 2);
    CHECK(d.lineCount >= 2);
}

TEST_CASE("banter: playing a sequence allocates nothing") {
    const BanterDef d = load(kBanter);
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    BanterSequence b;
    b.begin(&d);
    int frames = 0;
    while (b.active() && frames++ < 4000) {
        b.tick(1.0 / 60.0);
        (void)b.speaker();
        (void)b.text();
        (void)b.side();
    }
    b.begin(&d);
    b.press();
    b.skip();
    const auto allocs = ftcore_test::allocStats().count - before;
    if (counting) CHECK(allocs == 0);
    CHECK(b.finished());
}
