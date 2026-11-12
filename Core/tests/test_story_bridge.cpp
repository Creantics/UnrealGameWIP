#include "ftcore/StoryBridge.h"

#include "AllocCounter.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <array>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

const char* kCsv =
    "slot,id,kind,max,default,mut,set_scenes,gate,read_scenes,fallback,derive\n"
    "1,f_pick,C3,3,0,once,s1,-,s2,all,-\n"
    "2,f_seen,EV,1,0,once,s1,-,s2,-,-\n"
    "3,f_count,NUM,9,0,free,any,-,all,-,-\n";
const char* kJson = R"({ "version": 2, "scenes": ["s1", "s2", "s3"] })";

const FactRegistry& reg() {
    static const FactRegistry r = [] {
        FactRegistry out;
        std::string err;
        const bool ok = importStoryRegistry(kCsv, kJson, out, &err);
        REQUIRE_MESSAGE(ok, err);
        return out;
    }();
    return r;
}

// Scene ids are the registry codes unless `prefix` renames them (for the scene map tests).
void build(StoryDatabase& db, std::string_view prefix = {}) {
    const std::string p(prefix);
    StoryBuilder b;
    b.addFlag("sf.f_pick", FlagValue::integer(0));
    b.addFlag("sf.f_seen", FlagValue::boolean(false));
    b.addFlag("sf.f_count", FlagValue::integer(0));
    b.addFlag("local", FlagValue::boolean(false));
    b.beginChapter("c1", "C");
    b.beginScene(p + "s1", "One");
    b.line("l1", "A", "one");                          // 0
    b.gameplay("ACT", "[act]");                        // 1
    b.param("sets", "sf.f_pick");
    b.flag(FlagOp::Set, "sf.f_pick", FlagValue::integer(1)); // 2: a second set of a once-only fact
    b.flag(FlagOp::Set, "sf.f_seen");                  // 3
    b.flag(FlagOp::Set, "local");                      // 4
    b.line("l2", "A", "two");                          // 5
    b.gameplay("PLAY", "[play]");                      // 6
    b.param("sets", "sf.f_count");
    b.param("stub", "4");
    b.line("l3", "A", "three");                        // 7
    b.beginScene(p + "s2", "Two");
    b.flag(FlagOp::Require, "sf.f_pick", FlagValue::integer(2)); // 0: the top of the scene
    b.line("l4", "A", "picked two");                   // 1
    b.flag(FlagOp::End);                               // 2
    b.line("l5", "A", "after");                        // 3
    b.flag(FlagOp::Set, "sf.f_seen", FlagValue::boolean(false)); // 4: a clear of a once-only fact: ignored
    b.line("l6", "A", "end of two");                   // 5
    b.beginScene(p + "s3", "Three");
    b.gameplay("CMB", "[fight]");                      // 0: no `sets`
    b.line("l7", "A", "last");                         // 1
    std::string error;
    REQUIRE_MESSAGE(b.finish(db, &error), error);
}

struct Reports {
    std::vector<StoryBridgeReport> list;
    static void on(void* user, const StoryBridgeReport& r) { static_cast<Reports*>(user)->list.push_back(r); }
};

std::uint8_t fact(const StoryState& s, std::string_view id) { return storyGet(s, reg(), storyFactSlot(reg(), id)).value; }
FactStatus status(const StoryState& s, std::string_view id) {
    return storyGet(s, reg(), storyFactSlot(reg(), id)).status;
}

} // namespace

TEST_CASE("story bridge: binding") {
    StoryDatabase db;
    build(db);
    StoryRunner r(&db);
    StoryState s;
    StoryBridge br;
    std::string error;
    REQUIRE_MESSAGE(br.bind(r, s, reg(), {}, &error), error);
    CHECK(br.boundFlags() == 3);
    CHECK(br.mappedScenes() == 3); // identity: the scene ids are the codes
    CHECK(br.ordinalOf(db.findScene("s2")) == 1);

    SUBCASE("fact flags with no scene that plays a registry scene are refused") {
        StoryDatabase other;
        build(other, "x-");
        StoryRunner r2(&other);
        CHECK_FALSE(br.bind(r2, s, reg(), {}, &error));
        CHECK(error.find("registry scene") != std::string::npos);
        CHECK_FALSE(br.bound());
    }
    SUBCASE("a story without fact flags on ids that are not codes is inert") {
        StoryBuilder b;
        b.addFlag("local", FlagValue::boolean(false));
        b.beginChapter("c1", "C");
        b.beginScene("x-s1", "One");
        b.line("l", "A", "x");
        StoryDatabase other;
        REQUIRE(b.finish(other, &error));
        StoryRunner r2(&other);
        REQUIRE(br.bind(r2, s, reg(), {}, &error));
        CHECK(br.mappedScenes() == 0);
        REQUIRE(r2.start());
        CHECK(s.openScene == kNoScene);
    }
    SUBCASE("a string fact flag or an unknown fact is refused") {
        StoryBuilder b;
        b.addFlag("sf.f_pick", b.stringValue("NONE"));
        b.addFlag("sf.nope", FlagValue::boolean(false));
        b.beginChapter("c1", "C");
        b.beginScene("s1", "One");
        b.line("l", "A", "x");
        StoryDatabase bad;
        REQUIRE(b.finish(bad, &error));
        StoryRunner r2(&bad);
        CHECK_FALSE(br.bind(r2, s, reg(), {}, &error));
        CHECK(error.find("sf.f_pick") != std::string::npos);
        CHECK(error.find("sf.nope") != std::string::npos);
        CHECK_FALSE(br.bound());
    }
    SUBCASE("a map naming a scene the story lacks is refused") {
        const std::vector<StorySceneLink> links{{"s9", 0}};
        CHECK_FALSE(br.bind(r, s, reg(), links, &error));
        CHECK(error.find("s9") != std::string::npos);
    }
    SUBCASE("unbinding gives the handlers back") {
        br.unbind();
        s.openScene = kNoScene;
        REQUIRE(r.start());
        CHECK(s.openScene == kNoScene);
    }
}

TEST_CASE("story bridge: a playthrough records the facts") {
    StoryDatabase db;
    build(db);
    StoryRunner r(&db);
    StoryState s;
    StoryBridge br;
    Reports reports;
    reports.list.reserve(8); // the test's own record must not count as the bridge allocating
    br.setReportHandler(&Reports::on, &reports);
    REQUIRE(br.bind(r, s, reg(), {}));

    s.openScene = 2; // start() resets the state
    REQUIRE(r.start());
    CHECK(s.openScene == 0);
    CHECK(r.text() == "one");
    CHECK_FALSE(r.resume(FlagValue::integer(2))); // not on gameplay
    REQUIRE(r.advance());
    REQUIRE(r.waitingForGameplay());
    CHECK_FALSE(r.resume(FlagValue{0, FlagValue::Kind::String})); // a string does not fit an int flag
    CHECK(r.waitingForGameplay());

    SUBCASE("the act's result branches the next scene") {
        const auto before = ftcore_test::allocStats().count;
        REQUIRE(r.resume(FlagValue::integer(2)));
        CHECK(ftcore_test::allocStats().count == before); // the handlers allocate nothing
        CHECK(fact(s, "f_pick") == 2);
        CHECK(r.flag("sf.f_pick") == FlagValue::integer(2)); // the second set was refused and written back
        REQUIRE(reports.list.size() == 1);
        CHECK(reports.list[0].result == SetResult::RejectedOnce);
        CHECK(reports.list[0].value == 1);
        CHECK(status(s, "f_seen") == FactStatus::Set);
        CHECK(r.flag("sf.f_seen") == FlagValue::boolean(true));
        CHECK(r.text() == "two");
        REQUIRE(r.advance());
        REQUIRE(r.waitingForGameplay());
        REQUIRE(r.resume()); // the stub
        CHECK(fact(s, "f_count") == 4);
        CHECK(r.flag("sf.f_count") == FlagValue::integer(4));
        REQUIRE(r.advance()); // l3 -> s2
        CHECK(s.openScene == 1);
        CHECK(s.lastCommitted == 0);
        CHECK(r.text() == "picked two");
        REQUIRE(r.advance());
        CHECK(r.text() == "after");
        REQUIRE(r.advance()); // the ignored clear: a note, not a refusal
        REQUIRE(reports.list.size() == 2);
        CHECK(reports.list[1].kind == StoryBridgeReport::Kind::ClearIgnored);
        CHECK(reports.list[1].fact == storyFactSlot(reg(), "f_seen"));
        CHECK(status(s, "f_seen") == FactStatus::Set);
        CHECK(r.flag("sf.f_seen") == FlagValue::boolean(true));
        CHECK(r.text() == "end of two");
        REQUIRE(r.advance()); // -> s3: f_pick and f_seen are not its business
        CHECK(s.openScene == 2);
        CHECK(r.flag("sf.f_pick") == FlagValue::integer(0));
        CHECK(r.flag("sf.f_seen") == FlagValue::boolean(false));
        CHECK(r.flag("sf.f_count") == FlagValue::integer(4));
        REQUIRE(r.waitingForGameplay());
        CHECK_FALSE(r.resume(FlagValue::integer(1))); // no `sets`
        CHECK(r.waitingForGameplay());
        REQUIRE(r.resume());
        REQUIRE(r.advance());
        CHECK(r.finished());
        CHECK(s.openScene == kNoScene);
        CHECK(s.lastCommitted == 2);
    }
    SUBCASE("another result skips the block") {
        REQUIRE(r.resume(FlagValue::integer(3)));
        REQUIRE(r.advance());
        REQUIRE(r.resume());
        REQUIRE(r.advance());
        CHECK(r.text() == "after");
    }
    SUBCASE("a value past a byte is refused") {
        REQUIRE(r.resume(FlagValue::integer(2)));
        REQUIRE(r.advance());
        REQUIRE(r.waitingForGameplay());
        reports.list.clear();
        REQUIRE(r.resume(FlagValue::integer(300)));
        REQUIRE(reports.list.size() == 1);
        CHECK(reports.list[0].result == SetResult::RejectedRange);
        CHECK(r.flag("sf.f_count") == FlagValue::integer(0));
        CHECK(status(s, "f_count") == FactStatus::Unset);
    }
}

TEST_CASE("story bridge: restarting a scene") {
    StoryDatabase db;
    build(db);
    StoryRunner r(&db);
    StoryState s;
    StoryBridge br;
    REQUIRE(br.bind(r, s, reg(), {}));
    CHECK_FALSE(r.restartScene()); // idle
    REQUIRE(r.start());
    REQUIRE(r.advance());
    REQUIRE(r.resume(FlagValue::integer(2)));
    CHECK(r.flag("local") == FlagValue::boolean(true));
    const auto epoch = r.sceneEpoch();
    REQUIRE(r.restartScene());
    CHECK(r.sceneEpoch() == epoch + 1);
    CHECK(r.beatIndex() == 0);
    CHECK(r.text() == "one");
    CHECK(status(s, "f_pick") == FactStatus::Unset);
    CHECK(status(s, "f_seen") == FactStatus::Unset);
    CHECK(r.flag("sf.f_pick") == FlagValue::integer(0));
    CHECK(r.flag("local") == FlagValue::boolean(false)); // the runner's own flags roll back too
    REQUIRE(r.advance());
    REQUIRE(r.resume(FlagValue::integer(1))); // the retry picks again
    CHECK(fact(s, "f_pick") == 1);
}

TEST_CASE("story bridge: a load") {
    StoryDatabase db;
    build(db);
    StoryRunner r(&db);
    StoryState s;
    StoryBridge br;
    REQUIRE(br.bind(r, s, reg(), {}));
    REQUIRE(r.start());
    REQUIRE(r.advance());
    REQUIRE(r.resume(FlagValue::integer(2)));

    // What a save keeps: the cursor, the runner's flags, the state block.
    auto save = [&](StoryPosition& pos, std::string& scene, std::vector<FlagValue>& flags,
                    std::array<std::uint8_t, kStoryStateBlockSize>& block) {
        pos = r.position();
        scene = std::string(pos.scene);
        pos.scene = scene;
        flags.clear();
        for (std::uint32_t i = 0; i < r.flagCount(); ++i) flags.push_back(r.flag(i));
        REQUIRE(storyEncode(s, reg(), block));
    };
    auto load = [&](StoryRunner& r2, StoryState& s2, StoryBridge& br2, const StoryPosition& pos,
                    const std::vector<FlagValue>& flags, const std::array<std::uint8_t, kStoryStateBlockSize>& block) {
        REQUIRE(br2.bind(r2, s2, reg(), {}));
        for (std::uint32_t i = 0; i < flags.size(); ++i) r2.setFlag(i, flags[i]);
        REQUIRE(r2.restore(pos));
        REQUIRE(storyDecode(block, reg(), s2) == StoryLoad::Ok);
        br2.sync();
    };
    StoryPosition pos;
    std::string scene;
    std::vector<FlagValue> flags;
    std::array<std::uint8_t, kStoryStateBlockSize> block{};

    SUBCASE("inside a scene: runner and state carry on from the save, a restart goes back to it") {
        save(pos, scene, flags, block);
        StoryRunner r2(&db);
        StoryState s2;
        StoryBridge br2;
        load(r2, s2, br2, pos, flags, block);
        CHECK(s2.openScene == 0);
        CHECK(r2.text() == "two");
        CHECK(fact(s2, "f_pick") == 2);
        CHECK(r2.flag("sf.f_pick") == FlagValue::integer(2));
        REQUIRE(r2.restartScene());
        CHECK(r2.text() == "one");
        CHECK(fact(s2, "f_pick") == 2); // the load point, the runner's restart point too
        CHECK(r2.flag("sf.f_pick") == FlagValue::integer(2));
    }
    SUBCASE("between scenes: the next scene begins with the facts pushed") {
        REQUIRE(r.advance());
        REQUIRE(r.resume());
        REQUIRE(r.text() == "three");
        REQUIRE(storyCommitScene(s, reg())); // a save made as s1 closed, before s2 began
        save(pos, scene, flags, block);
        pos.scene = "s2";
        pos.beat = 0;
        StoryRunner r2(&db);
        StoryState s2;
        StoryBridge br2;
        load(r2, s2, br2, pos, flags, block);
        CHECK(s2.openScene == 1);
        CHECK(fact(s2, "f_pick") == 2);
        CHECK(r2.flag("sf.f_pick") == FlagValue::integer(2));
        CHECK(r2.text() == "picked two");
    }
}

TEST_CASE("story bridge: a scene map") {
    StoryDatabase db;
    build(db, "r-");
    StoryRunner r(&db);
    StoryState s;
    StoryBridge br;
    // r-s1 and r-s2 both play the first registry scene; r-s3 is unmapped.
    const std::vector<StorySceneLink> links{{"r-s1", 0}, {"r-s2", 0}};
    REQUIRE(br.bind(r, s, reg(), links));
    CHECK(br.mappedScenes() == 2);
    REQUIRE(r.start());
    REQUIRE(r.advance());
    REQUIRE(r.resume(FlagValue::integer(3)));
    REQUIRE(r.advance());
    REQUIRE(r.resume());
    REQUIRE(r.advance()); // r-s2: the registry scene stays open, its facts kept
    CHECK(s.openScene == 0);
    CHECK(fact(s, "f_pick") == 3);
    CHECK(r.text() == "after");
    REQUIRE(r.advance());
    REQUIRE(r.advance()); // r-s3: unmapped, the state stays as it is
    CHECK(s.openScene == 0);
    REQUIRE(r.resume());
    REQUIRE(r.advance());
    CHECK(r.finished()); // the end commits
    CHECK(s.openScene == kNoScene);
    CHECK(s.lastCommitted == 0);
}

TEST_CASE("story bridge: gameplay params") {
    StoryBuilder b;
    b.beginChapter("c1", "C");
    b.beginScene("s1", "One");
    b.gameplay("ACT", "[act]");
    b.param("stub", "x1");
    StoryDatabase db;
    std::string error;
    CHECK_FALSE(b.finish(db, &error));
    CHECK(error.find("stub") != std::string::npos);

    StoryBuilder b2;
    b2.beginChapter("c1", "C");
    b2.beginScene("s1", "One");
    b2.gameplay("ACT", "[act]");
    b2.param("sets", "undeclared");
    b2.param("stub", "-2");
    REQUIRE_MESSAGE(b2.finish(db, &error), error);
    StoryRunner r(&db);
    REQUIRE(r.start());
    REQUIRE(r.waitingForGameplay());
    CHECK(r.param("sets") == "undeclared"); // the params stay readable
    REQUIRE(r.resume());                    // an undeclared flag is a bool: the stub -2 sets it true
    CHECK(r.flag("undeclared") == FlagValue::boolean(true));
    CHECK(r.finished());
}

// An autosave on a scene change: the runner and the block are taken as the subsystem reports the change (after
// the runner entered the scene and played up to its first stop), so the block has the new scene open.
TEST_CASE("story bridge: a save at a scene start loads back across a scene change") {
    StoryDatabase db;
    build(db);
    StoryRunner r(&db);
    StoryState s;
    StoryBridge br;
    REQUIRE(br.bind(r, s, reg(), {}));
    REQUIRE(r.start());
    REQUIRE(r.advance());
    REQUIRE(r.resume(FlagValue::integer(2)));
    REQUIRE(r.advance());
    REQUIRE(r.resume());
    REQUIRE(r.advance()); // into s2: what an autosave sees
    REQUIRE(r.position().scene == "s2");
    REQUIRE(s.openScene == 1);

    struct Saved {
        StoryPosition pos;
        std::string scene;
        std::vector<FlagValue> flags;
        std::array<std::uint8_t, kStoryStateBlockSize> block{};
    };
    auto save = [&](Saved& out) {
        out.pos = r.position();
        out.scene = std::string(out.pos.scene);
        out.pos.scene = out.scene;
        out.flags.clear();
        for (std::uint32_t i = 0; i < r.flagCount(); ++i) out.flags.push_back(r.flag(i));
        REQUIRE(storyEncode(s, reg(), out.block));
    };
    auto load = [&](const Saved& in) {
        for (std::uint32_t i = 0; i < in.flags.size(); ++i) r.setFlag(i, in.flags[i]);
        REQUIRE(r.restore(in.pos));
        REQUIRE(storyDecode(in.block, reg(), s) == StoryLoad::Ok);
        br.sync();
    };
    Saved atTwo;
    save(atTwo);
    CHECK(r.text() == "picked two");

    SUBCASE("back: from a later scene to the save") {
        REQUIRE(r.advance());
        REQUIRE(r.advance());
        REQUIRE(r.advance()); // into s3
        REQUIRE(s.openScene == 2);
        REQUIRE(s.lastCommitted == 1);
        load(atTwo);
        CHECK(s.openScene == 1);
        CHECK(s.lastCommitted == 0);
        CHECK(r.position().scene == "s2");
        CHECK(r.text() == "picked two");
        CHECK(r.flag("sf.f_pick") == FlagValue::integer(2));
        REQUIRE(r.advance());
        REQUIRE(r.advance());
        REQUIRE(r.advance()); // s3 again: the state follows
        CHECK(s.openScene == 2);
        CHECK(s.lastCommitted == 1);
    }
    SUBCASE("forward: from a new game to the save") {
        REQUIRE(r.start());
        REQUIRE(s.openScene == 0);
        REQUIRE(status(s, "f_pick") == FactStatus::Unset);
        load(atTwo);
        CHECK(s.openScene == 1);
        CHECK(s.lastCommitted == 0);
        CHECK(fact(s, "f_pick") == 2);
        CHECK(r.text() == "picked two");
        CHECK(r.flag("sf.f_pick") == FlagValue::integer(2));
    }
}

// A restore settles from the saved beat: flag beats it runs there are the runner's, not the story's moves, so
// nothing reaches the state until sync, which takes the decoded block as it is.
TEST_CASE("story bridge: the runner's moves while it restores do not reach the state") {
    StoryDatabase db;
    build(db);
    StoryRunner r(&db);
    StoryState s;
    StoryBridge br;
    std::vector<StoryBridgeReport> reports;
    reports.reserve(8);
    br.setReportHandler([](void* u, const StoryBridgeReport& rep) {
        static_cast<std::vector<StoryBridgeReport>*>(u)->push_back(rep);
    }, &reports);
    REQUIRE(br.bind(r, s, reg(), {}));
    REQUIRE(r.start());
    // The block: s1 open, the pick made as 3, nothing seen.
    REQUIRE(storySet(s, reg(), storyFactSlot(reg(), "f_pick"), 3) == SetResult::Ok);
    std::array<std::uint8_t, kStoryStateBlockSize> block{};
    REQUIRE(storyEncode(s, reg(), block));
    REQUIRE(r.start()); // a new game: the state as a new game has it
    REQUIRE(status(s, "f_pick") == FactStatus::Unset);
    // A position on s1's two flag beats (the pick as 1, the seen fact): the restore runs them before "two".
    StoryPosition pos;
    pos.scene = "s1";
    pos.beat = 2;
    REQUIRE(r.restore(pos));
    REQUIRE(r.text() == "two");
    CHECK(reports.empty());
    CHECK(status(s, "f_pick") == FactStatus::Unset); // not forwarded while restoring
    CHECK(status(s, "f_seen") == FactStatus::Unset);
    REQUIRE(storyDecode(block, reg(), s) == StoryLoad::Ok);
    br.sync();
    CHECK(reports.empty());
    CHECK(s.openScene == 0);
    CHECK(fact(s, "f_pick") == 3); // the block as it is
    CHECK(status(s, "f_seen") == FactStatus::Unset);
    CHECK(r.flag("sf.f_pick") == FlagValue::integer(3));
    CHECK(r.flag("sf.f_seen") == FlagValue::boolean(false));
    // After sync the story's writes are forwarded again.
    REQUIRE(r.advance());
    REQUIRE(r.resume());
    CHECK(fact(s, "f_count") == 4);
}

// Two runner scenes play one registry scene: restarting the second keeps what the first set.
TEST_CASE("story bridge: restarting the second runner scene of a registry scene") {
    StoryDatabase db;
    build(db, "r-");
    StoryRunner r(&db);
    StoryState s;
    StoryBridge br;
    const std::vector<StorySceneLink> links{{"r-s1", 0}, {"r-s2", 0}};
    REQUIRE(br.bind(r, s, reg(), links));
    REQUIRE(r.start());
    REQUIRE(r.advance());
    REQUIRE(r.resume(FlagValue::integer(2)));
    REQUIRE(r.advance());
    REQUIRE(r.resume());
    REQUIRE(r.advance()); // r-s2, the same registry scene
    REQUIRE(r.position().scene == "r-s2");
    REQUIRE(fact(s, "f_pick") == 2);
    REQUIRE(r.text() == "picked two");

    SUBCASE("a restart") {
        REQUIRE(r.restartScene());
        CHECK(r.text() == "picked two");
        CHECK(fact(s, "f_pick") == 2);
        CHECK(status(s, "f_seen") == FactStatus::Set);
        CHECK(r.flag("sf.f_pick") == FlagValue::integer(2));
        CHECK(s.openScene == 0);
        // The registry scene's own start is kept: a restart of the first runner scene would still clear it.
        CHECK(storyRestartScene(s, reg()));
        CHECK(status(s, "f_pick") == FactStatus::Unset);
    }
    SUBCASE("a load then a restart") {
        StoryPosition pos = r.position();
        const std::string scene(pos.scene);
        pos.scene = scene;
        std::vector<FlagValue> flags;
        for (std::uint32_t i = 0; i < r.flagCount(); ++i) flags.push_back(r.flag(i));
        std::array<std::uint8_t, kStoryStateBlockSize> block{};
        REQUIRE(storyEncode(s, reg(), block));
        StoryRunner r2(&db);
        StoryState s2;
        StoryBridge br2;
        REQUIRE(br2.bind(r2, s2, reg(), links));
        for (std::uint32_t i = 0; i < flags.size(); ++i) r2.setFlag(i, flags[i]);
        REQUIRE(r2.restore(pos));
        REQUIRE(storyDecode(block, reg(), s2) == StoryLoad::Ok);
        br2.sync();
        REQUIRE(r2.restartScene());
        CHECK(r2.text() == "picked two");
        CHECK(fact(s2, "f_pick") == 2);
        CHECK(r2.flag("sf.f_pick") == FlagValue::integer(2));
    }
}

TEST_CASE("story bridge: a stub its flag cannot take carries on") {
    StoryBuilder b;
    b.addFlag("txt", b.stringValue("NONE"));
    b.beginChapter("c1", "C");
    b.beginScene("s1", "One");
    b.gameplay("ACT", "[act]");
    b.param("sets", "txt");
    b.param("stub", "3");
    b.gameplay("ACT", "[act]"); // a stub with no `sets`
    b.param("stub", "3");
    b.line("l", "A", "after");
    StoryDatabase db;
    std::string error;
    REQUIRE_MESSAGE(b.finish(db, &error), error);
    StoryRunner r(&db);
    REQUIRE(r.start());
    CHECK_FALSE(r.resume(FlagValue::integer(3))); // a string flag takes no result
    REQUIRE(r.resume());
    REQUIRE(r.state() == StoryRunner::State::Gameplay);
    REQUIRE(r.resume());
    CHECK(r.text() == "after");
}

// Mapped, unmapped, then mapped to the same registry scene: the registry scene stays open across the gap.
TEST_CASE("story bridge: a continuation across an unmapped scene") {
    StoryDatabase db;
    build(db, "r-");
    StoryRunner r(&db);
    StoryState s;
    StoryBridge br;
    const std::vector<StorySceneLink> links{{"r-s1", 0}, {"r-s3", 0}};
    REQUIRE(br.bind(r, s, reg(), links));
    REQUIRE(r.start());
    REQUIRE(r.advance());
    REQUIRE(r.resume(FlagValue::integer(3)));
    REQUIRE(r.advance());
    REQUIRE(r.resume());
    REQUIRE(r.advance()); // r-s2: unmapped
    REQUIRE(r.position().scene == "r-s2");
    CHECK(s.openScene == 0);
    CHECK(r.flag("sf.f_pick") == FlagValue::integer(3)); // unmapped: the flags stay as they were too
    REQUIRE(r.advance());
    REQUIRE(r.advance()); // r-s3: the same registry scene again
    REQUIRE(r.position().scene == "r-s3");
    CHECK(s.openScene == 0);
    CHECK(s.lastCommitted == kNoScene);
    CHECK(fact(s, "f_pick") == 3);
    CHECK(r.flag("sf.f_pick") == FlagValue::integer(3));
    REQUIRE(r.resume());
    REQUIRE(r.advance());
    CHECK(r.finished());
    CHECK(s.lastCommitted == 0);
}

// A restore that settles past the story's last beat: the end is not forwarded while restoring; sync commits.
TEST_CASE("story bridge: the end reached through a restore") {
    StoryDatabase db;
    build(db);
    StoryRunner r(&db);
    StoryState s;
    StoryBridge br;
    std::vector<StoryBridgeReport> reports;
    reports.reserve(8);
    br.setReportHandler([](void* u, const StoryBridgeReport& rep) {
        static_cast<std::vector<StoryBridgeReport>*>(u)->push_back(rep);
    }, &reports);
    REQUIRE(br.bind(r, s, reg(), {}));
    REQUIRE(r.start());
    // The block: s3 open after s1 and s2.
    REQUIRE(storyEnterScene(s, reg(), 1).began);
    REQUIRE(storyEnterScene(s, reg(), 2).began);
    std::array<std::uint8_t, kStoryStateBlockSize> block{};
    REQUIRE(storyEncode(s, reg(), block));
    REQUIRE(r.start());
    StoryPosition pos;
    pos.scene = "s3";
    pos.beat = 2; // past its last beat
    REQUIRE(r.restore(pos));
    REQUIRE(r.finished());
    CHECK(s.openScene == 0); // not forwarded: still the new game's state
    REQUIRE(storyDecode(block, reg(), s) == StoryLoad::Ok);
    REQUIRE(s.openScene == 2);
    br.sync();
    CHECK(reports.empty());
    CHECK(s.openScene == kNoScene);
    CHECK(s.lastCommitted == 2);
}

TEST_CASE("story bridge: clears") {
    StoryDatabase db;
    {
        StoryBuilder b;
        b.addFlag("sf.f_pick", FlagValue::integer(0));
        b.addFlag("sf.f_seen", FlagValue::boolean(false));
        b.addFlag("sf.f_count", FlagValue::integer(0));
        b.beginChapter("c1", "C");
        b.beginScene("s1", "One");
        b.flag(FlagOp::Set, "sf.f_seen", FlagValue::boolean(false)); // 0: a clear of an unset once fact
        b.line("l1", "A", "one");                                    // 1
        b.flag(FlagOp::Set, "sf.f_count", FlagValue::integer(5));    // 2
        b.flag(FlagOp::Set, "sf.f_seen");                            // 3
        b.flag(FlagOp::Set, "sf.f_pick", FlagValue::integer(2));     // 4
        b.line("l2", "A", "two");                                    // 5
        b.flag(FlagOp::Set, "sf.f_count", FlagValue::integer(0));    // 6: not even a free fact clears
        b.flag(FlagOp::Clear, "sf.f_seen");                          // 7: a once fact does not
        b.flag(FlagOp::Set, "sf.f_pick", FlagValue::integer(0));     // 8: nor does an int once fact
        b.line("l3", "A", "three");                                  // 9
        std::string error;
        REQUIRE_MESSAGE(b.finish(db, &error), error);
    }
    StoryRunner r(&db);
    StoryState s;
    StoryBridge br;
    Reports reports;
    reports.list.reserve(8);
    br.setReportHandler(&Reports::on, &reports);
    REQUIRE(br.bind(r, s, reg(), {}));
    REQUIRE(r.start());
    CHECK(reports.list.empty()); // clearing what is not set says nothing
    CHECK(status(s, "f_seen") == FactStatus::Unset);
    REQUIRE(r.advance());
    CHECK(fact(s, "f_count") == 5);
    REQUIRE(r.advance());
    CHECK(fact(s, "f_count") == 5);
    CHECK(r.flag("sf.f_count") == FlagValue::integer(5)); // the fact's value back
    CHECK(status(s, "f_seen") == FactStatus::Set);
    CHECK(r.flag("sf.f_seen") == FlagValue::boolean(true)); // the fact's value back
    CHECK(fact(s, "f_pick") == 2);
    CHECK(r.flag("sf.f_pick") == FlagValue::integer(2));
    REQUIRE(reports.list.size() == 3);
    for (const StoryBridgeReport& rep : reports.list) CHECK(rep.kind == StoryBridgeReport::Kind::ClearIgnored);
    CHECK(reports.list[0].fact == storyFactSlot(reg(), "f_count"));
    CHECK(reports.list[1].fact == storyFactSlot(reg(), "f_seen"));
    CHECK(reports.list[2].fact == storyFactSlot(reg(), "f_pick"));
    CHECK(reports.list[2].value == 0);
}

TEST_CASE("story bridge: sync on an idle or finished runner") {
    StoryDatabase db;
    build(db);
    StoryRunner r(&db);
    StoryState s;
    StoryBridge br;
    REQUIRE(br.bind(r, s, reg(), {}));
    br.sync(); // idle: nothing opens
    CHECK(s.openScene == kNoScene);
    CHECK(s.lastCommitted == kNoScene);
    REQUIRE(r.start());
    REQUIRE(r.advance());
    REQUIRE(r.resume(FlagValue::integer(2)));
    while (!r.finished()) {
        if (r.waitingForGameplay()) REQUIRE(r.resume());
        else REQUIRE(r.advance());
    }
    CHECK(s.lastCommitted == 2);
    br.sync(); // finished: nothing reopens
    CHECK(s.openScene == kNoScene);
    CHECK(s.lastCommitted == 2);
}

TEST_CASE("story bridge: a restore with no sync") {
    StoryDatabase db;
    build(db);
    StoryRunner r(&db);
    StoryState s;
    StoryBridge br;
    Reports reports;
    reports.list.reserve(8);
    br.setReportHandler(&Reports::on, &reports);
    REQUIRE(br.bind(r, s, reg(), {}));
    REQUIRE(r.start());
    StoryPosition pos;
    pos.scene = "s1";
    pos.beat = 2;
    REQUIRE(r.restore(pos)); // the flag beats it settles through are the restore's own: no report
    CHECK_FALSE(r.restoring());
    CHECK(br.awaitingSync());
    CHECK(reports.list.empty());
    REQUIRE(r.advance());
    REQUIRE(r.waitingForGameplay());
    REQUIRE(r.resume()); // the stub's write with no sync: reported, the state untouched
    REQUIRE(reports.list.size() == 1);
    CHECK(reports.list[0].kind == StoryBridgeReport::Kind::SyncMissing);
    CHECK(reports.list[0].ordinal == 0);
    CHECK(status(s, "f_count") == FactStatus::Unset);
    SUBCASE("reported once per restore") {
        REQUIRE(r.advance()); // -> s2, still paused
        CHECK(reports.list.size() == 1);
        CHECK(s.openScene == 0);
        REQUIRE(r.restore(pos));
        REQUIRE(r.advance());
        REQUIRE(r.resume());
        CHECK(reports.list.size() == 2);
    }
    SUBCASE("a new game instead of the sync") {
        REQUIRE(r.restore(pos));
        REQUIRE(r.start()); // the bridge follows the new game: no report, the state reset and forwarded
        CHECK_FALSE(br.awaitingSync());
        REQUIRE(r.advance());
        REQUIRE(r.resume(FlagValue::integer(2)));
        // The earlier SyncMissing, then beat 2 of s1 (build() above): its set of f_pick after the act's write of 2
        // is a second set of a once-only fact, refused once.
        CHECK(reports.list.size() == 1 + 1);
        CHECK(reports.list[1].kind == StoryBridgeReport::Kind::FactRefused);
        CHECK(fact(s, "f_pick") == 2);
    }
    SUBCASE("a late sync resumes forwarding") {
        br.sync();
        CHECK_FALSE(br.awaitingSync());
        REQUIRE(r.advance()); // -> s2
        CHECK(s.openScene == 1);
        CHECK(reports.list.size() == 1);
    }
}
