// ScarePacer: the jump-scare governor (window, gap, kind, seen moments, setting), its save, and the placement check.
#include "ftcore/ScarePacer.h"
#include "ftcore/StorySave.h"

#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr double kMin = 60.0;

ScareMoment scripted(std::uint16_t n) {
    ScareMoment m;
    m.number = n;
    return m;
}

// A pacer some way into play, past the start's quiet.
ScarePacer fresh() {
    ScarePacer p;
    p.advance(10.0 * kMin);
    return p;
}

} // namespace

TEST_CASE("scare pacer: ids, names and moments") {
    ScareMoment m;
    CHECK(scareMomentFromId("scare_001", m));
    CHECK(m.number == 1);
    CHECK_FALSE(m.ambient);
    CHECK(scareMomentFromId("scare_255", m));
    CHECK_FALSE(scareMomentFromId("scare_000", m));
    CHECK_FALSE(scareMomentFromId("scare_256", m));
    CHECK_FALSE(scareMomentFromId("scare_01", m));
    CHECK_FALSE(scareMomentFromId("scare_0a1", m));
    CHECK_FALSE(scareMomentFromId("Scare_001", m));
    for (std::size_t i = 0; i < kScareKinds; ++i) {
        ScareKind k = ScareKind::Count;
        CHECK(scareKindFromName(scareKindName(static_cast<ScareKind>(i)), k));
        CHECK(k == static_cast<ScareKind>(i));
    }
    CHECK(scareMomentValid(scareAmbient(ScareKind::Map, kScareAmbientPlaces - 1)));
    CHECK_FALSE(scareMomentValid(scareAmbient(ScareKind::Map, kScareAmbientPlaces)));
    CHECK_FALSE(scareMomentValid(scripted(0)));
}

TEST_CASE("scare pacer 1, 2: 14 min apart is refused, 15 min with another kind is allowed (Solhaven)") {
    ScarePacer p = fresh();
    CHECK(p.play(ScareKind::Exploration, ScareWorld::Solhaven, true, scripted(1)) == ScareForm::Full);
    p.advance(14.0 * kMin);
    CHECK_FALSE(p.canFire(ScareKind::Fight, ScareWorld::Solhaven, true, scripted(2)));
    p.advance(1.0 * kMin);
    CHECK(p.canFire(ScareKind::Fight, ScareWorld::Solhaven, true, scripted(2)));
    CHECK(p.play(ScareKind::Fight, ScareWorld::Solhaven, true, scripted(2)) == ScareForm::Full);
    CHECK(p.inWindow() == 2);
}

TEST_CASE("scare pacer 3: the same kind back to back is refused, however far apart") {
    ScarePacer p = fresh();
    p.play(ScareKind::Scene, ScareWorld::Lattice, true, scripted(1));
    p.advance(120.0 * kMin);
    CHECK_FALSE(p.canFire(ScareKind::Scene, ScareWorld::Lattice, true, scripted(2)));
    CHECK(p.canFire(ScareKind::Exploration, ScareWorld::Lattice, true, scripted(2)));
}

TEST_CASE("scare pacer 4: a third inside 60 min is refused in Solhaven and the Lattice, allowed in the Unwritten") {
    for (const ScareWorld w : {ScareWorld::Solhaven, ScareWorld::Lattice, ScareWorld::Unwritten}) {
        CAPTURE(scareWorldName(w));
        ScarePacer p = fresh();
        p.play(ScareKind::Exploration, w, true, scripted(1));
        p.advance(20.0 * kMin);
        p.play(ScareKind::Fight, w, true, scripted(2));
        p.advance(20.0 * kMin);
        const bool third = p.canFire(ScareKind::Scene, w, true, scripted(3));
        CHECK(third == (w == ScareWorld::Unwritten));
        if (w == ScareWorld::Unwritten) {
            p.play(ScareKind::Scene, w, true, scripted(3));
            p.advance(15.0 * kMin); // 55 min after the first: four inside the window
            CHECK_FALSE(p.canFire(ScareKind::Exploration, w, true, scripted(4)));
            p.advance(5.0 * kMin);  // the first is exactly 60 min old: it no longer counts
            CHECK(p.inWindow() == 2);
            CHECK(p.canFire(ScareKind::Exploration, w, true, scripted(4)));
        }
    }
}

TEST_CASE("scare pacer 5: in the Unwritten 10 min apart is allowed and 9 is refused") {
    ScarePacer p = fresh();
    p.play(ScareKind::Exploration, ScareWorld::Unwritten, true, scripted(1));
    p.advance(9.0 * kMin);
    CHECK_FALSE(p.canFire(ScareKind::Fight, ScareWorld::Unwritten, true, scripted(2)));
    // The gap is the new scare's world's: a Solhaven scare 9 min on is refused for its own 15.
    p.advance(1.0 * kMin);
    CHECK(p.canFire(ScareKind::Fight, ScareWorld::Unwritten, true, scripted(2)));
    CHECK_FALSE(p.canFire(ScareKind::Fight, ScareWorld::Solhaven, true, scripted(2)));
}

TEST_CASE("scare pacer 6: a completed moment never plays again; one played but not completed before a reload does") {
    ScarePacer p = fresh();
    CHECK(p.play(ScareKind::Exploration, ScareWorld::Solhaven, true, scripted(7)) == ScareForm::Full);
    p.complete(scripted(7));
    p.advance(120.0 * kMin);
    CHECK(p.seen(scripted(7)));
    CHECK_FALSE(p.canFire(ScareKind::Fight, ScareWorld::Solhaven, true, scripted(7)));
    CHECK(p.play(ScareKind::Fight, ScareWorld::Solhaven, true, scripted(7)) == ScareForm::None);

    // Saved before moment 8 plays; it plays, the game reloads before its beat completes: it plays again.
    ScarePacer q = fresh();
    ScareSave before;
    q.save(before);
    CHECK(q.play(ScareKind::Scene, ScareWorld::Lattice, true, scripted(8)) == ScareForm::Full);
    REQUIRE(q.load(before));
    q.markQuiet();
    q.advance(kScareQuietSeconds);
    CHECK(q.play(ScareKind::Scene, ScareWorld::Lattice, true, scripted(8)) == ScareForm::Full);
}

TEST_CASE("scare pacer 7: Off drops ambient scares and plays scripted ones Calm, unrecorded") {
    ScarePacer p = fresh();
    p.setSetting(ScareSetting::Off);
    const ScareMoment amb = scareAmbient(ScareKind::Exploration, 3);
    CHECK_FALSE(p.canFire(ScareKind::Exploration, ScareWorld::Solhaven, false, amb));
    CHECK(p.play(ScareKind::Exploration, ScareWorld::Solhaven, false, amb) == ScareForm::None);
    CHECK(p.play(ScareKind::Scene, ScareWorld::Solhaven, true, scripted(1)) == ScareForm::Calm);
    CHECK(p.inWindow() == 0);
    CHECK(p.lastKind() == ScareKind::Count);
    CHECK(p.lastTime() < 0.0);
    // The Calm play still completes: the beat stays told once.
    p.complete(scripted(1));
    p.setSetting(ScareSetting::Full);
    CHECK(p.play(ScareKind::Scene, ScareWorld::Solhaven, true, scripted(1)) == ScareForm::None);
}

TEST_CASE("scare pacer 8: Soft plays Soft and is recorded") {
    ScarePacer p = fresh();
    p.setSetting(ScareSetting::Soft);
    CHECK(p.play(ScareKind::Fight, ScareWorld::Lattice, true, scripted(1)) == ScareForm::Soft);
    CHECK(p.inWindow() == 1);
    CHECK(p.lastKind() == ScareKind::Fight);
    CHECK(p.play(ScareKind::Map, ScareWorld::Lattice, false, scareAmbient(ScareKind::Map, 0)) == ScareForm::None);
}

TEST_CASE("scare pacer 9: within 180 s of a load or a death, ambient is refused and scripted plays Calm") {
    ScarePacer p = fresh();
    p.markQuiet(); // a death
    p.advance(179.0);
    CHECK(p.play(ScareKind::Exploration, ScareWorld::Solhaven, false, scareAmbient(ScareKind::Exploration, 1)) ==
          ScareForm::None);
    CHECK(p.play(ScareKind::Scene, ScareWorld::Solhaven, true, scripted(1)) == ScareForm::Calm);
    CHECK(p.inWindow() == 0);
    p.advance(1.0);
    CHECK(p.play(ScareKind::Exploration, ScareWorld::Solhaven, false, scareAmbient(ScareKind::Exploration, 1)) ==
          ScareForm::Full);
}

TEST_CASE("scare pacer 10: a scripted moment that would break the window, gap or kind plays Calm, unrecorded") {
    ScarePacer p = fresh();
    p.play(ScareKind::Scene, ScareWorld::Solhaven, true, scripted(1));
    p.advance(20.0 * kMin);
    p.play(ScareKind::Fight, ScareWorld::Solhaven, true, scripted(2));
    const double last = p.lastTime();
    // Gap.
    p.advance(5.0 * kMin);
    CHECK(p.play(ScareKind::Exploration, ScareWorld::Solhaven, true, scripted(3)) == ScareForm::Calm);
    // Kind.
    p.advance(20.0 * kMin);
    CHECK(p.play(ScareKind::Fight, ScareWorld::Solhaven, true, scripted(4)) == ScareForm::Calm);
    // Window (two inside 60 min).
    CHECK(p.play(ScareKind::Exploration, ScareWorld::Solhaven, true, scripted(5)) == ScareForm::Calm);
    CHECK(p.inWindow() == 2);
    CHECK(p.lastTime() == doctest::Approx(last));
    CHECK(p.lastKind() == ScareKind::Fight);
}

TEST_CASE("scare pacer 11: the ring, the last kind and the seen moments survive a save, and the gap still holds") {
    ScarePacer p = fresh();
    p.play(ScareKind::Scene, ScareWorld::Lattice, true, scripted(12));
    p.complete(scripted(12));
    p.complete(scareAmbient(ScareKind::Map, 200));
    p.advance(5.0 * kMin);

    StorySave s;
    p.save(s.scares);
    s.hasScares = true;
    s.scene = "SC-A";
    const std::string text = encodeStorySave(s);
    StorySave back;
    std::string err;
    REQUIRE_MESSAGE(decodeStorySave(text, back, &err), err);
    REQUIRE(back.hasScares);
    CHECK(back.scares == s.scares);

    ScarePacer q;
    REQUIRE(q.load(back.scares));
    CHECK(q.playSeconds() == doctest::Approx(p.playSeconds()));
    CHECK(q.lastKind() == ScareKind::Scene);
    CHECK(q.seen(scripted(12)));
    CHECK(q.seen(scareAmbient(ScareKind::Map, 200)));
    CHECK_FALSE(q.seen(scareAmbient(ScareKind::Map, 199)));
    q.markQuiet();
    q.advance(kScareQuietSeconds); // 8 min after the scare: the 15 min gap still refuses
    CHECK_FALSE(q.canFire(ScareKind::Fight, ScareWorld::Lattice, true, scripted(13)));
    q.advance(7.0 * kMin);
    CHECK(q.canFire(ScareKind::Fight, ScareWorld::Lattice, true, scripted(13)));

    // A save from before the pacer has none; a malformed one is refused.
    StorySave old;
    old.scene = "SC-A";
    REQUIRE(decodeStorySave(encodeStorySave(old), back, &err));
    CHECK_FALSE(back.hasScares);
    std::string bad = text;
    const std::size_t at = bad.find("\"head\":");
    REQUIRE(at != std::string::npos);
    bad.replace(at, 8, "\"head\":9");
    CHECK_FALSE(decodeStorySave(bad, back, &err));
    ScareSave neg = s.scares;
    neg.playSeconds = -1.0;
    CHECK_FALSE(q.load(neg));
    neg = s.scares;
    neg.seenHex = "zz";
    CHECK_FALSE(q.load(neg));
}

TEST_CASE("scare pacer 12: two ambient scares with the same kind and place: the second never plays") {
    ScarePacer p = fresh();
    const ScareMoment a = scareAmbient(ScareKind::Exploration, 42);
    CHECK(p.play(ScareKind::Exploration, ScareWorld::Solhaven, false, a) == ScareForm::Full);
    p.complete(a);
    p.advance(30.0 * kMin);
    p.play(ScareKind::Fight, ScareWorld::Solhaven, true, scripted(1));
    p.advance(120.0 * kMin);
    CHECK(p.play(ScareKind::Exploration, ScareWorld::Solhaven, false, a) == ScareForm::None);
    // The same place under another kind is its own moment.
    CHECK(p.play(ScareKind::Map, ScareWorld::Solhaven, false, scareAmbient(ScareKind::Map, 42)) == ScareForm::Full);
}

TEST_CASE("scare pacer 13: in the Finale every scare is refused") {
    ScarePacer p = fresh();
    p.setFinale(true);
    for (std::size_t k = 0; k < kScareKinds; ++k)
        for (const ScareWorld w : {ScareWorld::Solhaven, ScareWorld::Lattice, ScareWorld::Unwritten})
            CHECK_FALSE(p.canFire(static_cast<ScareKind>(k), w, true, scripted(1)));
    CHECK(p.play(ScareKind::Scene, ScareWorld::Lattice, true, scripted(1)) == ScareForm::Calm);
    CHECK(p.inWindow() == 0);
    p.setFinale(false);
    CHECK(p.canFire(ScareKind::Scene, ScareWorld::Lattice, true, scripted(1)));
}

TEST_CASE("scare pacer 14: a hallucination is paced only when flagged as a scare, and is recorded once") {
    ScarePacer p = fresh();
    CHECK(p.hallucination(false, scripted(20)) == ScareForm::Unpaced);
    CHECK(p.inWindow() == 0);
    CHECK(p.lastKind() == ScareKind::Count);
    CHECK(p.hallucination(true, scripted(21)) == ScareForm::Full);
    p.complete(scripted(21));
    CHECK(p.inWindow() == 1);
    CHECK(p.lastKind() == ScareKind::Hallucination);
    p.advance(30.0 * kMin);
    CHECK(p.hallucination(true, scripted(21)) == ScareForm::None);
    CHECK(p.inWindow() == 1);
}

TEST_CASE("scare pacer 15: the ring wraps after 8 with no allocation") {
    ScarePacer p = fresh();
    for (std::uint16_t i = 1; i <= 11; ++i) {
        p.advance(61.0 * kMin);
        const ScareKind k = (i % 2) ? ScareKind::Fight : ScareKind::Scene;
        CHECK(p.play(k, ScareWorld::Solhaven, true, scripted(i)) == ScareForm::Full);
    }
    ScareSave s;
    p.save(s);
    CHECK(s.count == kScareRing);
    CHECK(s.head == 11 % kScareRing);
    CHECK(p.lastTime() == doctest::Approx(p.playSeconds()));
    CHECK(sizeof(ScarePacer) < 512); // fixed arrays only
}

TEST_CASE("scare pacer 16: the shipped placement table keeps every rule; a bad table is caught") {
    std::string text;
    REQUIRE_MESSAGE(ftcore_test::readProjectData("Scares/placements.json", text), "Data/Scares/placements.json");
    ScarePlacementTable t;
    std::string err;
    REQUIRE_MESSAGE(importScarePlacements(text, t, &err), err);
    CHECK(t.count >= 1);
    for (const std::string& problem : checkScarePlacements(t)) FAIL_CHECK(problem);

    // Breaks: a gap, a repeated kind, a full window, the start's quiet, and the Finale.
    const char* const kBad = R"({ "version": 1, "placements": [
      { "id": "scare_001", "world": "solhaven", "kind": "scene", "minute": 10 },
      { "id": "scare_002", "world": "solhaven", "kind": "fight", "minute": 20 },
      { "id": "scare_003", "world": "lattice", "kind": "scene", "minute": 30 },
      { "id": "scare_007", "world": "lattice", "kind": "fight", "minute": 35 },
      { "id": "scare_004", "world": "lattice", "kind": "exploration", "minute": 55 },
      { "id": "scare_005", "world": "unwritten", "kind": "map", "minute": 1 },
      { "id": "scare_006", "world": "unwritten", "kind": "map", "minute": 300, "finale": true } ] })";
    REQUIRE(importScarePlacements(kBad, t, &err));
    const std::vector<std::string> problems = checkScarePlacements(t);
    REQUIRE(problems.size() == 5);
    CHECK(problems[0].find("scare_005") != std::string::npos);
    CHECK(problems[0].find("first 3 minutes") != std::string::npos);
    CHECK(problems[1].find("scare_002") != std::string::npos);
    CHECK(problems[1].find("gap") != std::string::npos);
    CHECK(problems[2].find("scare_003") != std::string::npos);
    CHECK(problems[2].find("same kind") != std::string::npos);
    CHECK(problems[3].find("scare_004") != std::string::npos);
    CHECK(problems[3].find("window") != std::string::npos);
    CHECK(problems[4].find("Finale") != std::string::npos);

    // Refused files.
    CHECK_FALSE(importScarePlacements(R"({"version":1,"placements":[{"id":"scare_1","world":"lattice","kind":"scene","minute":1}]})", t, &err));
    CHECK(t.count == 0);
    CHECK_FALSE(importScarePlacements(R"({"version":1,"placements":[{"id":"scare_001","world":"moon","kind":"scene","minute":1}]})", t, &err));
    CHECK_FALSE(importScarePlacements(R"({"version":1,"placements":[{"id":"scare_001","world":"lattice","kind":"scene","minute":-1}]})", t, &err));
    CHECK_FALSE(importScarePlacements(R"({"version":1,"placements":[{"id":"scare_001","world":"lattice","kind":"scene","minute":1,"scene":"x"}]})", t, &err));
    CHECK(err.find("unknown key") != std::string::npos);
    CHECK_FALSE(importScarePlacements(R"({"version":1,"placements":[{"id":"scare_001","world":"lattice","kind":"scene","minute":1},{"id":"scare_001","world":"lattice","kind":"fight","minute":90}]})", t, &err));
    CHECK(err.find("duplicate") != std::string::npos);
}
