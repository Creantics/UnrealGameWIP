#include "ftcore/GameplayPlan.h"
#include "ftcore/StoryDatabase.h"
#include "ftcore/StoryRunner.h"

#include "AllocCounter.h"
#include "RealStory.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <string>
#include <string_view>

using namespace ftcore;

namespace {

// Every fixture mode below is invented notation in the script's shapes.

void checkStep(const GameplayPlan& p, std::uint32_t i, GameplayToken token, GameplayScreen screen,
               std::string_view label = {}) {
    REQUIRE(i < p.size());
    CHECK(p.step(i).token == token);
    CHECK(p.step(i).screen == screen);
    CHECK(p.step(i).tokenName() == gameplayTokenName(token));
    CHECK(p.step(i).label() == label);
}

bool validUtf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const std::size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (n == 0 || i + n > s.size()) return false;
        for (std::size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

} // namespace

TEST_CASE("gameplay tokens map to screens") {
    CHECK(gameplayTokenScreen(GameplayToken::Cmb) == GameplayScreen::Battle);
    CHECK(gameplayTokenScreen(GameplayToken::Eng) == GameplayScreen::Field);
    CHECK(gameplayTokenScreen(GameplayToken::Play) == GameplayScreen::Field);
    CHECK(gameplayTokenScreen(GameplayToken::Chc) == GameplayScreen::Choice);
    CHECK(gameplayTokenScreen(GameplayToken::Qte) == GameplayScreen::Qte);
    CHECK(gameplayTokenScreen(GameplayToken::CoopQte) == GameplayScreen::Qte);
    CHECK(gameplayTokenScreen(GameplayToken::Cg) == GameplayScreen::Cutscene);
    CHECK(gameplayTokenScreen(GameplayToken::He) == GameplayScreen::Choice);
    for (GameplayToken t : {GameplayToken::Bnt, GameplayToken::Mini, GameplayToken::Unknown})
        CHECK(gameplayTokenScreen(t) == GameplayScreen::Stub);
    CHECK(gameplayTokenFromName("cmb") == GameplayToken::Cmb);
    CHECK(gameplayTokenFromName("Co-Op QTE") == GameplayToken::CoopQte);
    CHECK(gameplayTokenFromName("CMBX") == GameplayToken::Unknown);
    CHECK(gameplayTokenName(GameplayToken::CoopQte) == "co-op QTE");
    CHECK(gameplayTokenName(GameplayToken::Unknown).empty());
    CHECK(gameplayScreenName(GameplayScreen::Stub) == "stub");
    CHECK(gameplayScreenName(GameplayScreen::Choice) == "choice");
    CHECK(gameplayScreenName(GameplayScreen::Qte) == "qte");
    CHECK(gameplayScreenName(GameplayScreen::Cutscene) == "cutscene");
}

TEST_CASE("screens route: new screens open the stub for now") {
    static_assert(screenRoutesTo(GameplayScreen::Choice) == GameplayScreen::Choice);
    static_assert(screenRoutesTo(GameplayScreen::Qte) == GameplayScreen::Stub);
    static_assert(screenRoutesTo(GameplayScreen::Cutscene) == GameplayScreen::Stub);
    static_assert(screenRoutesTo(GameplayScreen::Battle) == GameplayScreen::Battle);
    static_assert(screenRoutesTo(GameplayScreen::Field) == GameplayScreen::Field);
    static_assert(screenRoutesTo(GameplayScreen::Stub) == GameplayScreen::Stub);
    static_assert(screenRoutesTo(GameplayScreen::Box) == GameplayScreen::Box);
    GameplayPlan p; // invented fixture tokens
    REQUIRE(p.lower("CHC + QTE + CG + BNT + ZZZ"));
    CHECK(p.count(GameplayScreen::Choice) == 1);
    CHECK(p.count(GameplayScreen::Qte) == 1);
    CHECK(p.count(GameplayScreen::Cutscene) == 1);
    CHECK(p.count(GameplayScreen::Stub) == 2);
    CHECK(p.unknownTokens() == 1);
}

TEST_CASE("a mode chain lowers to its steps in order") {
    GameplayPlan p;
    SUBCASE("arrows") {
        REQUIRE(p.lower("PLAY → CHC → CMB"));
        CHECK(p.size() == 3);
        checkStep(p, 0, GameplayToken::Play, GameplayScreen::Field);
        checkStep(p, 1, GameplayToken::Chc, GameplayScreen::Choice);
        checkStep(p, 2, GameplayToken::Cmb, GameplayScreen::Battle);
        CHECK(p.count(GameplayScreen::Battle) == 1);
        CHECK(p.count(GameplayScreen::Field) == 1);
        CHECK(p.count(GameplayScreen::Choice) == 1);
        CHECK(p.warnings() == 0);
    }
    SUBCASE("qualifiers become labels") {
        REQUIRE(p.lower("CMB (duel) → CMB (set-piece)"));
        CHECK(p.size() == 2);
        checkStep(p, 0, GameplayToken::Cmb, GameplayScreen::Battle, "duel");
        checkStep(p, 1, GameplayToken::Cmb, GameplayScreen::Battle, "set-piece");
    }
    SUBCASE("plus and arrows flatten together") {
        REQUIRE(p.lower("ENG + CHC (loop) → CMB → ENG"));
        CHECK(p.size() == 4);
        checkStep(p, 0, GameplayToken::Eng, GameplayScreen::Field);
        checkStep(p, 1, GameplayToken::Chc, GameplayScreen::Choice, "loop");
        checkStep(p, 2, GameplayToken::Cmb, GameplayScreen::Battle);
        checkStep(p, 3, GameplayToken::Eng, GameplayScreen::Field);
    }
    SUBCASE("a multi-word token") {
        REQUIRE(p.lower("PLAY → ENG → co-op QTE"));
        CHECK(p.size() == 3);
        checkStep(p, 2, GameplayToken::CoopQte, GameplayScreen::Qte);
        CHECK(p.unknownTokens() == 0);
    }
    SUBCASE("a bare lower-case word qualifies the step before it") {
        REQUIRE(p.lower("CMB (Phase IV) + CHC (loop), intercut"));
        CHECK(p.size() == 2);
        checkStep(p, 0, GameplayToken::Cmb, GameplayScreen::Battle, "Phase IV");
        checkStep(p, 1, GameplayToken::Chc, GameplayScreen::Choice, "loop, intercut");
    }
    SUBCASE("a qualifier before any step goes to the first one") {
        REQUIRE(p.lower("intercut, CMB (duel)"));
        CHECK(p.size() == 1);
        checkStep(p, 0, GameplayToken::Cmb, GameplayScreen::Battle, "intercut, duel");
    }
    SUBCASE("a lone parenthetical qualifies too") {
        REQUIRE(p.lower("ENG + (loop) → CMB"));
        CHECK(p.size() == 2);
        checkStep(p, 0, GameplayToken::Eng, GameplayScreen::Field, "loop");
    }
    SUBCASE("no spaces around the separator") {
        REQUIRE(p.lower("CHC → PLAY+CMB"));
        CHECK(p.size() == 3);
        checkStep(p, 1, GameplayToken::Play, GameplayScreen::Field);
        checkStep(p, 2, GameplayToken::Cmb, GameplayScreen::Battle);
    }
    SUBCASE("ASCII arrow") {
        REQUIRE(p.lower("CG -> HE (intercut) -> BNT"));
        CHECK(p.size() == 3);
        checkStep(p, 0, GameplayToken::Cg, GameplayScreen::Cutscene);
        checkStep(p, 1, GameplayToken::He, GameplayScreen::Choice, "intercut");
        checkStep(p, 2, GameplayToken::Bnt, GameplayScreen::Stub);
    }
    SUBCASE("separators inside a parenthetical don't split") {
        REQUIRE(p.lower("CMB (Phase I + II, sky duel) → ENG"));
        CHECK(p.size() == 2);
        checkStep(p, 0, GameplayToken::Cmb, GameplayScreen::Battle, "Phase I + II, sky duel");
    }
    SUBCASE("two parentheticals on one token") {
        REQUIRE(p.lower("ENG (pair action) (one live option)"));
        CHECK(p.size() == 1);
        checkStep(p, 0, GameplayToken::Eng, GameplayScreen::Field, "pair action, one live option");
    }
    SUBCASE("a list-shaped mode") {
        REQUIRE(p.lower("('PLAY', 'CMB (pair action)')"));
        CHECK(p.size() == 2);
        checkStep(p, 0, GameplayToken::Play, GameplayScreen::Field);
        checkStep(p, 1, GameplayToken::Cmb, GameplayScreen::Battle, "pair action");
        REQUIRE(p.lower("[\"CHC\", \"ENG\"]"));
        CHECK(p.size() == 2);
        checkStep(p, 0, GameplayToken::Chc, GameplayScreen::Choice);
    }
    SUBCASE("empty pieces are skipped") {
        REQUIRE(p.lower(" → CMB →  , + ENG → "));
        CHECK(p.size() == 2);
        CHECK(p.warnings() == 0);
    }
}

TEST_CASE("an unknown token is a stub step and a warning") {
    GameplayPlan p;
    REQUIRE(p.lower("ZAP (x) → CMB"));
    CHECK(p.size() == 2);
    CHECK(p.step(0).token == GameplayToken::Unknown);
    CHECK(p.step(0).screen == GameplayScreen::Stub);
    CHECK(p.step(0).tokenName() == "ZAP");
    CHECK(p.step(0).label() == "x");
    CHECK(p.unknownTokens() == 1);
    CHECK(p.warnings() == 1);
    // A very long unknown token is cut to the name buffer.
    REQUIRE(p.lower("SOMEVERYLONGTOKENNAMEHERE"));
    CHECK(p.step(0).tokenName().size() == GameplayStep::kNameCapacity);
}

TEST_CASE("an empty mode falls back to the beat's kind") {
    GameplayPlan p;
    REQUIRE(p.lower("", "CMB"));
    CHECK(p.size() == 1);
    checkStep(p, 0, GameplayToken::Cmb, GameplayScreen::Battle);
    REQUIRE(p.lower("  ", "PLAY"));
    checkStep(p, 0, GameplayToken::Play, GameplayScreen::Field);
    REQUIRE(p.lower("intercut", "ENG")); // a qualifier alone holds no step
    CHECK(p.size() == 1);
    checkStep(p, 0, GameplayToken::Eng, GameplayScreen::Field);
    REQUIRE(p.lower("", "MINI"));
    checkStep(p, 0, GameplayToken::Mini, GameplayScreen::Stub);
    REQUIRE(p.lower("", "WHAT"));
    CHECK(p.unknownTokens() == 1);
    CHECK_FALSE(p.lower("", ""));
    CHECK(p.empty());
    // A mode with steps ignores the kind.
    REQUIRE(p.lower("ENG", "CMB"));
    checkStep(p, 0, GameplayToken::Eng, GameplayScreen::Field);
}

TEST_CASE("steps past the capacity are dropped and counted") {
    GameplayPlan p;
    REQUIRE(p.lower("CMB → ENG → PLAY → CHC → BNT → CG → HE → QTE → CMB (late) → ENG, intercut"));
    CHECK(p.size() == GameplayPlan::kMaxSteps);
    CHECK(p.droppedSteps() == 2);
    CHECK(p.warnings() == 2);
    CHECK(p.step(7).token == GameplayToken::Qte);
    CHECK(p.step(7).label().empty()); // the trailing qualifier belonged to a dropped step
    // Exactly at capacity: nothing dropped.
    REQUIRE(p.lower("CMB+CMB+CMB+CMB+CMB+CMB+CMB+CMB"));
    CHECK(p.size() == 8);
    CHECK(p.droppedSteps() == 0);
    // A fresh lower() starts clean.
    REQUIRE(p.lower("ENG"));
    CHECK(p.size() == 1);
    CHECK(p.warnings() == 0);
}

TEST_CASE("long labels are cut at a character boundary") {
    GameplayPlan p;
    // 'é' is two bytes: an odd cut would split one.
    REQUIRE(p.lower("CMB (xééééééééééééééééééééééééééééééééééé)"));
    const std::string_view label = p.step(0).label();
    CHECK(label.size() <= GameplayStep::kLabelCapacity);
    CHECK(label.size() >= GameplayStep::kLabelCapacity - 1);
    CHECK(validUtf8(label));
    REQUIRE(p.lower("CMB (aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa), bbbbbbbbbb, cccccccccc"));
    CHECK(p.step(0).label().size() == GameplayStep::kLabelCapacity);
}

TEST_CASE("the minutes param is kept") {
    GameplayPlan p;
    p.lower("CMB", {}, "5");
    CHECK(p.minutes() == 5);
    p.lower("CMB", {}, " 12 ");
    CHECK(p.minutes() == 12);
    p.lower("CMB", {}, "soon");
    CHECK(p.minutes() == 0);
    p.lower("CMB");
    CHECK(p.minutes() == 0);
}

TEST_CASE("a plan lowers from a database beat and from a runner") {
    StoryBuilder b;
    b.beginChapter("01", "One");
    b.beginScene("SC-T", "T");
    b.line("VO-T-1", "A", "Hi.");
    b.gameplay("ENG", "notation");
    b.param("mode", "PLAY → CMB (duel)");
    b.param("minutes", "7");
    b.gameplay("CMB", "notation");
    b.end();
    StoryDatabase db;
    std::string err;
    REQUIRE_MESSAGE(b.finish(db, &err), err);

    GameplayPlan p;
    const Scene& sc = db.scene(0);
    CHECK_FALSE(p.lower(db, db.beat(sc, 0))); // a line
    REQUIRE(p.lower(db, db.beat(sc, 1)));
    CHECK(p.size() == 2);
    CHECK(p.minutes() == 7);
    checkStep(p, 1, GameplayToken::Cmb, GameplayScreen::Battle, "duel");
    REQUIRE(p.lower(db, db.beat(sc, 2))); // no mode: its kind
    CHECK(p.size() == 1);
    checkStep(p, 0, GameplayToken::Cmb, GameplayScreen::Battle);

    StoryRunner r(&db);
    REQUIRE(r.start());
    CHECK_FALSE(p.lower(r));
    CHECK(p.empty());
    REQUIRE(r.advance());
    REQUIRE(p.lower(r));
    CHECK(p.size() == 2);
}

TEST_CASE("lowering a plan does not allocate") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("allocation hook not installed; skipping");
        return;
    }
    static constexpr std::string_view kModes[] = {
        "PLAY → CHC → CMB",
        "CMB (duel) → CMB (set-piece)",
        "ENG + CHC (loop) → CMB → ENG",
        "PLAY → ENG → co-op QTE",
        "CMB (Phase IV) + CHC (loop), intercut",
        "('PLAY', 'CMB (pair action)')",
        "CMB → ENG → PLAY → CHC → BNT → CG → HE → QTE → CMB → ENG",
        "ZAP",
        "",
    };
    GameplayPlan p;
    std::uint32_t steps = 0;
    const auto before = ftcore_test::allocStats().count;
    for (int round = 0; round < 10; ++round)
        for (const std::string_view m : kModes) {
            p.lower(m, "CMB", "5");
            steps += p.size();
        }
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(steps > 0);
}

TEST_CASE("every gameplay beat of the real script lowers with no unknown token") {
    FTCORE_REQUIRE_REAL_STORY(s);
    const StoryDatabase& db = s.db;
    GameplayPlan p;
    std::uint32_t beats = 0, empty = 0, unknown = 0, dropped = 0, withMinutes = 0;
    std::uint32_t battle = 0, field = 0, stub = 0, choice = 0, qte = 0, cutscene = 0;
    for (std::uint32_t si = 0; si < db.sceneCount(); ++si) {
        const Scene& sc = db.scene(si);
        for (std::uint32_t bi = 0; bi < sc.beatCount; ++bi) {
            const Beat& beat = db.beat(sc, bi);
            if (beat.type != BeatType::Gameplay) continue;
            ++beats;
            if (!p.lower(db, beat)) ++empty;
            unknown += p.unknownTokens();
            dropped += p.droppedSteps();
            if (p.minutes() > 0) ++withMinutes;
            battle += p.count(GameplayScreen::Battle);
            field += p.count(GameplayScreen::Field);
            stub += p.count(GameplayScreen::Stub);
            choice += p.count(GameplayScreen::Choice);
            qte += p.count(GameplayScreen::Qte);
            cutscene += p.count(GameplayScreen::Cutscene);
        }
    }
    CHECK(beats == 74);
    CHECK(empty == 0);
    CHECK(unknown == 0);
    CHECK(dropped == 0);
    CHECK(withMinutes > 0);
    CHECK(battle > 0);
    CHECK(field > 0);
    CHECK(stub + choice + qte + cutscene > 0);
    MESSAGE("real script plans: " << beats << " beats, " << battle << " battle, " << field << " field, " << stub
                                  << " stub, " << choice << " choice, " << qte << " qte, " << cutscene
                                  << " cutscene steps");
}
