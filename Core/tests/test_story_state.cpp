#include "ftcore/StoryFactReader.h"
#include "ftcore/StoryState.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr std::size_t slotOf(StoryFact f) { return static_cast<std::size_t>(f); }
constexpr std::size_t slotOf(StoryWide w) { return static_cast<std::size_t>(w); }

// The game's registry lives in Data/Story/, which stays out of the public repo. Tests that need it skip
// when it is absent (a clean clone); when it is present it must import cleanly.
struct RealRegistry {
    FactRegistry reg;
    bool present = false;
    std::string error;
};

const RealRegistry& real() {
    static const RealRegistry r = [] {
        RealRegistry out;
        std::string csv, json;
        if (!ftcore_test::readProjectData("Story/story_facts.csv", csv)) return out;
        if (!ftcore_test::readProjectData("Story/story_tuning.json", json)) return out;
        out.present = true;
        if (!importStoryFacts(csv, out.reg, &out.error)) return out;
        importStoryTuning(json, out.reg.tuning, &out.error);
        return out;
    }();
    return r;
}

// True when the game's registry is here; it then must have imported without an error.
bool haveRegistry() {
    if (!real().present) {
        MESSAGE("Data/Story is absent: skipped");
        return false;
    }
    INFO(real().error);
    REQUIRE(real().error.empty());
    return true;
}

const FactRegistry& registry() { return real().reg; }

std::uint8_t sc(const char* code) { return storySceneOrdinal(code); }

std::uint32_t issues(const StoryState& s, std::vector<StoryIssue>* list = nullptr,
                     const FactRegistry* reg = nullptr) {
    std::array<StoryIssue, kMaxStoryIssues> out{};
    const std::uint32_t n = storyValidate(s, reg ? *reg : registry(), out);
    if (list) list->assign(out.begin(), out.begin() + n);
    return n;
}

bool hasRule(const StoryState& s, std::uint8_t rule, const FactRegistry* reg = nullptr) {
    std::vector<StoryIssue> list;
    issues(s, &list, reg);
    for (const StoryIssue& i : list)
        if (i.rule == rule) return true;
    return false;
}

std::string describe(const StoryState& s) {
    std::vector<StoryIssue> list;
    issues(s, &list);
    std::string out;
    for (const StoryIssue& i : list)
        out += "V" + std::to_string(i.rule) + "(" + std::to_string(i.factA) + "," + std::to_string(i.factB) + ") ";
    return out;
}

// A minimal registry for rule tests: one row of each kind.
const char* kSmallCsv =
    "slot,id,kind,max,default,mut,set_scenes,gate,read_scenes,fallback,derive\n"
    "1,a_ev,EV,1,0,once,p02,-,p03,-,-\n"
    "2,a_num,NUM,9,3,mono,p02 p03,-,-,-,-\n"
    "3,a_ch,C3,3,0,once,p02,-,p04,all,-\n"
    "4,a_gated,C2,2,0,once,p03,a_ch=B,-,all,-\n"
    "5,a_bits,BITS,7,0,mono,p02 p03,-,-,-,-\n"
    "6,a_pk,PK,31,0,mono,p02 p03,-,-,-,-\n"
    "7,a_once_pk,PK,15,0,once,p02,-,-,-,-\n"
    "8,a_free,NUM,3,0,free,any,-,-,-,-\n"
    "9,a_op,OP,2,0,once,p03,a_ch=A,-,all,-\n"
    "10,a_opt,C2,2,0,once,p03,B:a_ch=C,-,all,-\n"
    "11,a_der,EV,1,0,once,p04,-,-,-,1:a_ch=A\n"
    "0,w_ctr,CTR,3,0,-,any,-,p03,-,-\n"
    "1,w_raw,RAW,100,0,-,any,-,-,-,-\n"
    "2,w_mask,MASK,15,0,-,p02,-,-,-,-\n";

FactRegistry small() {
    FactRegistry r;
    std::string err;
    const bool ok = importStoryFacts(kSmallCsv, r, &err);
    INFO(err);
    REQUIRE(ok);
    return r;
}

StoryFact sf(int slot) { return static_cast<StoryFact>(slot); }
StoryWide sw(int slot) { return static_cast<StoryWide>(slot); }

} // namespace

TEST_CASE("story state: formulas") {
    CHECK(storySaturatingAdd(5, 3, 10) == 8);
    CHECK(storySaturatingAdd(9, 3, 10) == 10);
    CHECK(storySaturatingAdd(0xFFFFFFFFu, 1, 0xFFFFFFFFu) == 0xFFFFFFFFu);
    const std::uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(storyCrc32(digits) == 0xCBF43926u);
    CHECK(storyCrc32(std::span(digits).subspan(4), storyCrc32(std::span(digits).first(4))) == 0xCBF43926u);

    StoryState s;
    CHECK(storyHelpShown(s) == 2);
    CHECK(storyLeadKind(s) == 0);
    s.wide[slotOf(StoryWide::c_help_s0_k1)] = 1;
    CHECK(storyHelpShown(s) == 2);
    for (int k = 0; k < 6; ++k) s.wide[static_cast<std::size_t>(k)] = 65535;
    CHECK(storyHelpShown(s) == 393210);

    // Most pressed wins; a tie goes to the last kind pressed, else heal, guard, count.
    StoryState t;
    t.wide[0] = 2;
    t.wide[1] = 3;
    t.wide[2] = 3;
    CHECK(storyLeadKind(t) == 1);
    t.facts[slotOf(StoryFact::sg_hl0)] = 3;
    CHECK(storyLeadKind(t) == 2);
    t.facts[slotOf(StoryFact::sg_hl0)] = 1; // last kind not among the tied
    CHECK(storyLeadKind(t) == 1);
    // The tie order heal, guard, count replaces "highest kind": (0,0,0) with no last kind is 0, and
    // (3,9,9) with last kind 0 is 1.
    StoryState u;
    CHECK(storyLeadKind(u) == 0);
    u.wide[0] = 3;
    u.wide[1] = 9;
    u.wide[2] = 9;
    u.facts[slotOf(StoryFact::sg_hl0)] = 1;
    CHECK(storyLeadKind(u) == 1);
    u.wide[0] = u.wide[1] = u.wide[2] = 7;
    u.facts[slotOf(StoryFact::sg_hl0)] = 2;
    CHECK(storyLeadKind(u) == 1);
}

TEST_CASE("story state: scene codes") {
    CHECK(sc("p01") == 0);
    CHECK(sc("p08") == 7);
    CHECK(sc("101") == 8);
    CHECK(sc("115") == 22);
    CHECK(sc("117") == 24);
    CHECK(sc("201") == 25);
    CHECK(sc("301") == 41);
    CHECK(sc("401") == 56);
    CHECK(sc("f01") == 65);
    CHECK(sc("f10") == 74);
    CHECK(sc("118") == kNoScene);
    CHECK(sc("p09") == kNoScene);
    CHECK(sc("x01") == kNoScene);
    CHECK(sc("1015") == kNoScene);
    for (std::uint8_t i = 0; i < kStoryScenes; ++i) CHECK(storySceneOrdinal(storySceneCode(i)) == i);
    CHECK(storySceneCode(kStoryScenes).empty());
}

TEST_CASE("story state: the data matches the enums") {
    if (!haveRegistry()) return;
    const FactRegistry& reg = registry();
    for (int i = 1; i < static_cast<int>(StoryFact::Count); ++i) {
        CAPTURE(i);
        CHECK(reg.facts[static_cast<std::size_t>(i)].used);
        CHECK(std::string(reg.facts[static_cast<std::size_t>(i)].id()) == std::string(storyFactId(sf(i))));
    }
    for (int i = 0; i < static_cast<int>(StoryWide::Count); ++i) {
        CAPTURE(i);
        CHECK(reg.wide[static_cast<std::size_t>(i)].kind != WideKind::None);
        CHECK(std::string(reg.wide[static_cast<std::size_t>(i)].id()) == std::string(storyWideId(sw(i))));
    }
    CHECK(reg.factsKnown == static_cast<std::uint16_t>(StoryFact::Count) - 1);
    CHECK(reg.wideKnown == static_cast<std::uint16_t>(StoryWide::Count));
    CHECK(reg.hash != 0);
    CHECK(reg.tuning.trackScene == sc("115"));
    CHECK(reg.tuning.trackLength == 7);
    CHECK(reg.tuning.milestoneCount == 4);
    // No scene reads more than its limits.
    for (std::uint8_t k = 0; k < kStoryScenes; ++k) CHECK(storySceneReads(StoryState{}, reg, k).complete);
}

TEST_CASE("story state: the registry refuses bad rows") {
    const std::string head = "slot,id,kind,max,default,mut,set_scenes,gate,read_scenes,fallback,derive\n";
    auto fails = [&](const std::string& rows, const std::string& expect) {
        FactRegistry r;
        std::string err;
        CAPTURE(rows);
        CHECK_FALSE(importStoryFacts(head + rows, r, &err));
        CAPTURE(err);
        CHECK(err.find(expect) != std::string::npos);
        CHECK_FALSE(r.facts[1].used); // nothing half-loaded
    };
    {
        FactRegistry r;
        std::string err;
        CHECK_FALSE(importStoryFacts("slot,id\n", r, &err));
        CHECK(err.find("header") != std::string::npos);
    }
    fails("1,a,EV,1,0,once,p02,-,-,-\n", "expected 11 fields");
    fails("1,Bad,EV,1,0,once,p02,-,-,-,-\n", "id:");
    fails("1,a,EV,1,0,once,p02,-,-,-,-\n2,a,EV,1,0,once,p02,-,-,-,-\n", "id duplicated: a");
    fails("1,a,EV,1,0,once,p02,-,-,-,-\n1,b,EV,1,0,once,p02,-,-,-,-\n", "slot reused: 1");
    fails("0,a,EV,1,0,once,p02,-,-,-,-\n", "slot: expected 1..255");
    fails("64,w,CTR,3,0,-,any,-,-,-,-\n", "slot: expected 0..63");
    fails("1,a,XX,1,0,once,p02,-,-,-,-\n", "kind:");
    fails("1,a,C3,2,0,once,p02,-,-,all,-\n", "max: does not fit the kind");
    fails("1,a,NUM,5,6,mono,p02,-,-,-,-\n", "default not representable");
    fails("1,a,EV,1,0,maybe,p02,-,-,-,-\n", "mut:");
    fails("1,a,EV,1,0,once,p02 p03,-,-,-,-\n", "a once fact with more than one set scene");
    fails("1,a,EV,1,0,once,q02,-,-,-,-\n", "set_scenes");
    fails("1,a,EV,1,0,once,p03,-,p02,-,-\n", "before any scene sets it");
    fails("1,a,C2,2,0,once,p03,-,p04 p05,p04,-\n", "no fallback marked for the read in p05");
    fails("1,a,C2,2,0,once,p03,nope=A,-,all,-\n", "unknown fact nope");
    fails("1,a,C2,2,0,once,p03,b=A,-,all,-\n2,b,C2,2,0,once,p04,-,-,all,-\n", "is set in a later scene");
    fails("1,a,C2,2,0,once,p03,C:b=A,-,all,-\n2,b,C2,2,0,once,p02,-,-,all,-\n", "option gate");
    fails("1,a,C2,2,0,once,p03,b.f2=1,-,all,-\n2,b,PK,15,0,mono,p02,-,-,-,-\n", "has no field 2");
    fails("1,a,EV,1,0,once,p03,-,-,-,5:b=1\n2,b,EV,1,0,once,p02,-,-,-,-\n", "derive:");
    fails("1,a,C2,2,0,once,p03,b=7,-,all,-\n2,b,C3,3,0,once,p02,-,-,all,-\n", "7 is not a value of b");
    fails("1,a,C2,2,0,once,p03,b.f0=3,-,all,-\n2,b,PK,15,0,mono,p02,-,-,-,-\n", "3 is not a value of b");
    fails("1,a,C2,2,0,once,p03,a=A,-,all,-\n", "names its own fact");
    std::string many;
    for (int i = 1; i <= 25; ++i) many += std::to_string(i) + ",f" + std::to_string(i) + ",EV,1,0,once,p02,-,p03,-,-\n";
    fails(many, "scene p03 reads more than 24 facts");
}

TEST_CASE("story state: tuning import") {
    StoryTuning t;
    std::string err;
    CHECK_FALSE(importStoryTuning("{\"version\":2,\"trackLength\":7,\"trackScene\":\"115\",\"milestones\":[]}", t, &err));
    CHECK(err.find("version") != std::string::npos);
    CHECK_FALSE(importStoryTuning("{\"version\":1,\"trackLength\":0,\"trackScene\":\"999\",\"milestones\":[]}", t, &err));
    CHECK(err.find("trackLength") != std::string::npos);
    CHECK(err.find("trackScene") != std::string::npos);
    CHECK_FALSE(importStoryTuning("{\"version\":1,\"trackLength\":7,\"trackScene\":\"115\",\"milestones\":"
                                  "[{\"scene\":\"115\",\"value\":5},{\"scene\":\"p07\",\"value\":6}]}",
                                  t, &err));
    CHECK(err.find("milestones[1]") != std::string::npos);
    CHECK(importStoryTuning("{\"version\":1,\"trackLength\":3,\"trackScene\":\"p05\",\"milestones\":[]}", t, &err));
    CHECK(t.trackLength == 3);
    CHECK(t.trackScene == sc("p05"));
    CHECK(t.milestoneCount == 0);
}

TEST_CASE("story state: set and get") {
    const FactRegistry r = small();
    StoryState s;
    CHECK(storyGet(s, r, sf(2)).status == FactStatus::Unset);
    CHECK(storyGet(s, r, sf(2)).value == 3); // the default
    CHECK(storyGet(s, r, sf(40)).status == FactStatus::Unknown);
    CHECK(storySet(s, r, sf(1), 1) == SetResult::RejectedNoScene);
    CHECK(storySet(s, r, sf(40), 1) == SetResult::UnknownFact);

    REQUIRE(storyBeginScene(s, sc("p02")));
    CHECK(storySet(s, r, sf(4), 1) == SetResult::RejectedWrongScene);
    CHECK(storySet(s, r, sf(1), 2) == SetResult::RejectedRange);
    CHECK(storySet(s, r, sf(1), 0) == SetResult::RejectedRange);
    CHECK(storySet(s, r, sf(1), kNotOffered) == SetResult::RejectedRange);
    CHECK(storySet(s, r, sf(1), 1) == SetResult::Ok);
    CHECK(storySet(s, r, sf(1), 1) == SetResult::Unchanged);
    CHECK(storyGet(s, r, sf(1)).status == FactStatus::Set);
    CHECK(s.stamp[1] == sc("p02"));

    CHECK(storySet(s, r, sf(2), 2) == SetResult::RejectedDownward); // below the default
    CHECK(storySet(s, r, sf(2), 5) == SetResult::Ok);
    CHECK(storySet(s, r, sf(2), 4) == SetResult::RejectedDownward);
    CHECK(storySet(s, r, sf(2), 10) == SetResult::RejectedRange);

    CHECK(storySet(s, r, sf(3), 4) == SetResult::RejectedRange);
    CHECK(storySet(s, r, sf(3), 2) == SetResult::Ok);
    CHECK(storySet(s, r, sf(3), 3) == SetResult::RejectedOnce);

    CHECK(storySetBits(s, r, sf(5), 1) == SetResult::Ok);
    CHECK(storySetBits(s, r, sf(5), 4) == SetResult::Ok);
    CHECK(storyGet(s, r, sf(5)).value == 5);
    CHECK(storySet(s, r, sf(5), 4) == SetResult::RejectedDownward);
    CHECK(storySetBits(s, r, sf(5), 8) == SetResult::RejectedRange);

    // Packed: fields 0..1 (bits 0..3), flag bit 4; a field never holds 3.
    CHECK(storySet(s, r, sf(6), 3) == SetResult::RejectedRange);
    CHECK(storySet(s, r, sf(6), 1) == SetResult::Ok);
    CHECK(storySet(s, r, sf(6), 2) == SetResult::Ok);           // field 0 up
    CHECK(storySet(s, r, sf(6), 1) == SetResult::RejectedDownward);
    CHECK(storySetBits(s, r, sf(6), 16) == SetResult::Ok);      // the flag
    CHECK(storySet(s, r, sf(6), 2 | 4) == SetResult::RejectedDownward); // drops the flag
    CHECK(storySet(s, r, sf(7), 1) == SetResult::Ok);
    CHECK(storySet(s, r, sf(7), 1 | 8) == SetResult::Ok);       // a new field is fine once
    CHECK(storySet(s, r, sf(7), 2 | 8) == SetResult::RejectedOnce);

    CHECK(storySet(s, r, sf(8), 3) == SetResult::Ok); // any scene, free
    CHECK(storySet(s, r, sf(8), 1) == SetResult::Ok);

    // Every rejection is an event.
    std::array<StoryEvent, kStoryEventRing> ev{};
    const std::uint32_t n = storyDrainEvents(s, ev);
    std::uint32_t rejected = 0;
    for (std::uint32_t i = 0; i < n; ++i) rejected += ev[i].kind == StoryEvent::Kind::Rejected;
    CHECK(rejected > 10);
    CHECK(storyDrainEvents(s, ev) == 0);
}

TEST_CASE("story state: gates and the commit") {
    const FactRegistry r = small();
    for (std::uint8_t choice = 1; choice <= 3; ++choice) {
        CAPTURE(int(choice));
        StoryState s;
        REQUIRE(storyBeginScene(s, sc("p02")));
        REQUIRE(storySet(s, r, sf(1), 1) == SetResult::Ok);
        REQUIRE(storySet(s, r, sf(3), choice) == SetResult::Ok);
        REQUIRE(storyCommitScene(s, r));
        REQUIRE(storyBeginScene(s, sc("p03")));
        CHECK((storySet(s, r, sf(4), 1) == SetResult::Ok) == (choice == 2));
        CHECK((storySet(s, r, sf(10), 2) == SetResult::Ok) == (choice == 3)); // option B needs a_ch=C
        REQUIRE(storyCommitScene(s, r));
        // A gated choice left unanswered closes as not offered; an optional act as passed or not offered.
        CHECK(storyGet(s, r, sf(4)).status == (choice == 2 ? FactStatus::Set : FactStatus::NotOffered));
        CHECK(storyGet(s, r, sf(9)).value == (choice == 1 ? 1 : kNotOffered));
        CHECK(storyGet(s, r, sf(10)).status == (choice == 3 ? FactStatus::Set : FactStatus::Unset));
    }
}

TEST_CASE("story state: wide values") {
    const FactRegistry r = small();
    StoryState s;
    CHECK(storyAdd(s, r, sw(0), 1) == SetResult::RejectedNoScene);
    REQUIRE(storyBeginScene(s, sc("p02")));
    CHECK(storyAdd(s, r, sw(0), 2) == SetResult::Ok);
    CHECK(storyAdd(s, r, sw(0), 5) == SetResult::Ok);
    CHECK(storyWide(s, sw(0)) == 3);
    CHECK(storyAdd(s, r, sw(0), 1) == SetResult::Unchanged);
    CHECK(storyAdd(s, r, sw(2), 16) == SetResult::RejectedRange);
    CHECK(storyAdd(s, r, sw(2), 5) == SetResult::Ok);
    CHECK(storyAdd(s, r, sw(2), 1) == SetResult::Unchanged);
    CHECK(storyAdd(s, r, sw(9), 1) == SetResult::UnknownFact);
    std::array<StoryEvent, kStoryEventRing> ev{};
    const std::uint32_t n = storyDrainEvents(s, ev);
    std::uint32_t saturated = 0, wideRejected = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        saturated += ev[i].kind == StoryEvent::Kind::Saturated;
        wideRejected += ev[i].kind == StoryEvent::Kind::Rejected && (ev[i].id & kStoryWideEventBit) != 0;
    }
    CHECK(saturated == 1);
    CHECK(wideRejected == 3); // no scene, range, unknown
    REQUIRE(storyCommitScene(s, r));
    REQUIRE(storyBeginScene(s, sc("p03")));
    CHECK(storyAdd(s, r, sw(2), 8) == SetResult::RejectedWrongScene);
}

TEST_CASE("story state: restart and the scene order") {
    if (!haveRegistry()) return;
    const FactRegistry& reg = registry();
    StoryState s;
    REQUIRE(storyBeginScene(s, sc("p02")));
    CHECK_FALSE(storyBeginScene(s, sc("p03"))); // one open scene
    REQUIRE(storySet(s, reg, StoryFact::sp02_1, 5) == SetResult::Ok);
    REQUIRE(storyCountHelp(s, reg, 0, 1) == SetResult::Ok);
    CHECK(storyWide(s, StoryWide::c_help_all) == 1);
    REQUIRE(storyRestartScene(s, reg));
    CHECK(storyGet(s, reg, StoryFact::sp02_1).status == FactStatus::Unset);
    CHECK(storyGet(s, reg, StoryFact::sp03_3).status == FactStatus::Unset);
    CHECK(storyWide(s, StoryWide::c_help_s0_k1) == 0);
    CHECK(storyWide(s, StoryWide::c_help_all) == 1); // raw: kept
    CHECK(storyWide(s, StoryWide::c_retries) == 1);
    CHECK(s.stamp[slotOf(StoryFact::sp02_1)] == kNoScene);
    REQUIRE(storySet(s, reg, StoryFact::sp02_1, 4) == SetResult::Ok);
    REQUIRE(storyCommitScene(s, reg));
    CHECK_FALSE(storyRestartScene(s, reg));
    CHECK_FALSE(storyCommitScene(s, reg));
    CHECK_FALSE(storyBeginScene(s, sc("p02"))); // not after the last committed
    CHECK_FALSE(storyBeginScene(s, kStoryScenes));
    CHECK(storyBeginScene(s, sc("p04")));      // skipping is allowed
    CHECK(issues(s) == 0);

    // Side 1 needs its slot named.
    CHECK(storyCountHelp(s, reg, 1, 0) == SetResult::RejectedGate);
    CHECK(storyCountHelp(s, reg, 2, 0) == SetResult::RejectedRange);
    CHECK(storyCountHelp(s, reg, 0, 3) == SetResult::RejectedRange);
}

TEST_CASE("story state: the sandbox is never written") {
    if (!haveRegistry()) return;
    const FactRegistry& reg = registry();
    StoryState real;
    REQUIRE(storyBeginScene(real, sc("p02")));
    REQUIRE(storyCommitScene(real, reg));
    StoryState box;
    storyMakeSandbox(real, reg, sc("p05"), box);
    CHECK(box.lastCommitted == sc("p04"));
    CHECK(storyBeginScene(box, sc("p05")));
    {   // Facts written at the chosen scene or later read as unset in the box; earlier ones stay.
        const FactRegistry r = small();
        StoryState played;
        REQUIRE(storyBeginScene(played, sc("p02")));
        REQUIRE(storySet(played, r, sf(1), 1) == SetResult::Ok);
        REQUIRE(storySet(played, r, sf(3), 2) == SetResult::Ok); // a_ch = B
        REQUIRE(storySet(played, r, sf(2), 5) == SetResult::Ok); // a_num, written in p02 and p03
        REQUIRE(storyCommitScene(played, r));
        REQUIRE(storyBeginScene(played, sc("p03")));
        REQUIRE(storySet(played, r, sf(4), 1) == SetResult::Ok); // once, gated on a_ch = B
        REQUIRE(storySet(played, r, sf(2), 7) == SetResult::Ok);
        REQUIRE(storyCommitScene(played, r));
        StoryState b3;
        storyMakeSandbox(played, r, sc("p03"), b3);
        CHECK(storyGet(b3, r, sf(1)).value == 1);
        CHECK(storyGet(b3, r, sf(4)).status == FactStatus::Unset);
        CHECK(b3.stamp[4] == kNoScene);
        CHECK(storyGet(b3, r, sf(2)).value == 7); // several writers: kept
        REQUIRE(storyBeginScene(b3, sc("p03")));
        CHECK(storySet(b3, r, sf(4), 1) == SetResult::Ok);
    }
    std::array<std::uint8_t, kStoryStateBlockSize> block{};
    block[0] = 7;
    CHECK_FALSE(storyEncode(box, reg, block));
    CHECK(block[0] == 7);
}

TEST_CASE("story state: the block") {
    if (!haveRegistry()) return;
    const FactRegistry& reg = registry();
    StoryState s;
    REQUIRE(storyBeginScene(s, sc("p02")));
    REQUIRE(storySet(s, reg, StoryFact::sp02_1, 6) == SetResult::Ok);
    REQUIRE(storyCommitScene(s, reg));
    REQUIRE(storyBeginScene(s, sc("p03")));
    REQUIRE(storySet(s, reg, StoryFact::sp03_2, 2) == SetResult::Ok);
    REQUIRE(storyCountHelp(s, reg, 0, 2) == SetResult::Ok);

    std::array<std::uint8_t, kStoryStateBlockSize> block{};
    REQUIRE(storyEncode(s, reg, block));
    CHECK(s.saveSerial == 1);
    CHECK(block[0] == 0x46);
    CHECK(block[3] == 0x53);
    CHECK((block[18] & kStoryMidScene) != 0);

    StoryState back;
    StoryStateLoadReport rep;
    REQUIRE(storyDecode(block, reg, back, &rep) == StoryLoad::Ok);
    CHECK(back.facts == s.facts);
    CHECK(back.factsEntry == s.factsEntry);
    CHECK(back.stamp == s.stamp);
    CHECK(back.stampEntry == s.stampEntry);
    CHECK(back.wide == s.wide);
    CHECK(back.wideEntry == s.wideEntry);
    CHECK(back.openScene == sc("p03"));
    CHECK(back.lastCommitted == sc("p02"));
    CHECK(back.saveSerial == 1);
    CHECK_FALSE(rep.registryMismatch);
    CHECK(rep.unknownTags == 0);
    // A restart after loading still goes back to the scene's start.
    REQUIRE(storyRestartScene(back, reg));
    CHECK(storyGet(back, reg, StoryFact::sp03_2).status == FactStatus::Unset);
    CHECK(back.stamp[slotOf(StoryFact::sp02_1)] == sc("p02"));

    std::array<std::uint8_t, kStoryStateBlockSize> again{};
    StoryState copy = s;
    copy.saveSerial = 0;
    REQUIRE(storyEncode(copy, reg, again));
    CHECK(again == block);

    // Refusals.
    StoryState untouched;
    CHECK(storyDecode({}, reg, untouched) == StoryLoad::Empty);
    CHECK(storyDecode(std::span(block).first(4), reg, untouched) == StoryLoad::BadSize);
    CHECK(storyDecode(std::span(block).first(2000), reg, untouched) == StoryLoad::BadSize);
    auto bad = block;
    bad[0] = 'X';
    CHECK(storyDecode(bad, reg, untouched) == StoryLoad::BadMagic);
    bad = block;
    bad[4] = 2;
    CHECK(storyDecode(bad, reg, untouched) == StoryLoad::NewerLayout);
    bad = block;
    bad[100] ^= 1;
    CHECK(storyDecode(bad, reg, untouched) == StoryLoad::BadChecksum);
    for (const std::size_t at : {std::size_t{16}, std::size_t{17}}) { // a scene out of range, checksum fixed
        bad = block;
        bad[at] = kStoryScenes;
        std::uint32_t crc = storyCrc32(std::span<const std::uint8_t>(bad).first(28));
        crc = storyCrc32(std::span<const std::uint8_t>(bad).subspan(32), crc);
        for (std::size_t i = 0; i < 4; ++i) bad[28 + i] = static_cast<std::uint8_t>(crc >> (8 * i));
        CHECK(storyDecode(bad, reg, untouched) == StoryLoad::BadScene);
    }
    CHECK(untouched.facts == StoryState{}.facts);
}

TEST_CASE("story state: a newer writer's slots and records survive") {
    if (!haveRegistry()) return;
    const FactRegistry& reg = registry();
    StoryState s;
    REQUIRE(storyBeginScene(s, sc("p02")));
    REQUIRE(storyCommitScene(s, reg));
    std::array<std::uint8_t, kStoryStateBlockSize> block{};
    REQUIRE(storyEncode(s, reg, block));
    // As a newer build: two more facts, one more wide slot, an unknown record.
    block[8] = static_cast<std::uint8_t>(reg.factsKnown + 2);
    block[10] = static_cast<std::uint8_t>(reg.wideKnown + 1);
    block[32 + reg.factsKnown + 1] = 2;
    block[800 + 4 * reg.wideKnown] = 9;
    std::uint8_t* e = block.data() + kStoryExtensionOffset;
    e[0] = 7;
    e[2] = 3;
    e[4] = 'a';
    e[5] = 'b';
    e[6] = 'c';
    block[12] ^= 0x55; // a different registry
    block[28] = block[29] = block[30] = block[31] = 0;
    const std::uint32_t crc = [&] {
        std::array<std::uint8_t, kStoryStateBlockSize> tmp = block;
        tmp[28] = tmp[29] = tmp[30] = tmp[31] = 0;
        std::uint32_t c = storyCrc32(std::span(tmp).first(28));
        return storyCrc32(std::span(tmp).subspan(32), c);
    }();
    for (int i = 0; i < 4; ++i) block[28 + i] = static_cast<std::uint8_t>(crc >> (8 * i));

    StoryState back;
    StoryStateLoadReport rep;
    REQUIRE(storyDecode(block, reg, back, &rep) == StoryLoad::Ok);
    CHECK(rep.unknownSlotsPreserved == 3);
    CHECK(rep.unknownTags == 1);
    CHECK(rep.registryMismatch);
    CHECK(storyGetSlot(back, reg, static_cast<std::uint16_t>(reg.factsKnown + 1)).status == FactStatus::Unknown);
    CHECK(hasRule(back, 20) == false); // the writer knew more slots: newer, not a mismatch to warn on
    std::array<std::uint8_t, kStoryStateBlockSize> out{};
    REQUIRE(storyEncode(back, reg, out));
    CHECK(out[32 + reg.factsKnown + 1] == 2);
    CHECK(out[800 + 4 * reg.wideKnown] == 9);
    CHECK(out[8] == reg.factsKnown + 2);
    CHECK(out[kStoryExtensionOffset] == 7);
    CHECK(out[kStoryExtensionOffset + 6] == 'c');

    // A malformed extension is kept byte for byte.
    StoryState broken = back;
    broken.extension.fill(0);
    broken.extension[0] = 9;
    broken.extension[2] = 0xFF;
    broken.extension[3] = 0x0F;
    broken.extensionLength = 4;
    REQUIRE(storyEncode(broken, reg, out));
    StoryState back2;
    REQUIRE(storyDecode(out, reg, back2, &rep) == StoryLoad::Ok);
    CHECK(rep.extensionMalformed);
    std::array<std::uint8_t, kStoryStateBlockSize> out2{};
    REQUIRE(storyEncode(back2, reg, out2));
    CHECK(std::equal(out.begin() + kStoryExtensionOffset, out.end(), out2.begin() + kStoryExtensionOffset));
}

TEST_CASE("story state: derive on load") {
    const FactRegistry r = small();
    StoryState s;
    REQUIRE(storyBeginScene(s, sc("p02")));
    REQUIRE(storySet(s, r, sf(3), 1) == SetResult::Ok);
    REQUIRE(storySet(s, r, sf(1), 1) == SetResult::Ok);
    REQUIRE(storyCommitScene(s, r));
    REQUIRE(storyBeginScene(s, sc("p04")));
    REQUIRE(storyCommitScene(s, r));
    std::array<std::uint8_t, kStoryStateBlockSize> block{};
    REQUIRE(storyEncode(s, r, block));
    {   // This writer knew a_der and left it unset: nothing is derived.
        StoryState back;
        StoryStateLoadReport rep;
        REQUIRE(storyDecode(block, r, back, &rep) == StoryLoad::Ok);
        CHECK(rep.derived == 0);
        CHECK(storyGet(back, r, sf(11)).status == FactStatus::Unset);
    }
    // An older writer that knew slots 1..10 only.
    block[8] = 10;
    block[9] = 0;
    std::uint32_t crc = storyCrc32(std::span<const std::uint8_t>(block).first(28));
    crc = storyCrc32(std::span<const std::uint8_t>(block).subspan(32), crc);
    for (int i = 0; i < 4; ++i) block[28 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(crc >> (8 * i));
    StoryState back;
    StoryStateLoadReport rep;
    REQUIRE(storyDecode(block, r, back, &rep) == StoryLoad::Ok);
    CHECK(rep.derived == 1);
    CHECK(storyGet(back, r, sf(11)).value == 1);
}

TEST_CASE("story state: validation rules") {
    if (!haveRegistry()) return;
    const FactRegistry& reg = registry();
    auto at = [](std::uint8_t committedUpTo) {
        StoryState s;
        s.lastCommitted = committedUpTo;
        return s;
    };
    auto put = [](StoryState& s, StoryFact f, std::uint8_t v) {
        s.facts[slotOf(f)] = v;
        s.factsEntry[slotOf(f)] = v;
    };
    CHECK(issues(StoryState{}) == 0);
    {   // V01, V02 and V07 on the small table (the full one at a late scene hits the cap first).
        const FactRegistry r = small();
        StoryState s = at(sc("p03"));
        put(s, sf(1), 1);
        put(s, sf(3), 1);
        put(s, sf(4), 1); // its gate a_ch=B is false
        CHECK(hasRule(s, 1, &r));
        put(s, sf(3), 2);
        put(s, sf(4), kNotOffered); // the gate holds, yet not offered
        CHECK(hasRule(s, 2, &r));
        put(s, sf(10), 2); // option B needs a_ch=C
        CHECK(hasRule(s, 7, &r));
        put(s, sf(3), 3);
        CHECK_FALSE(hasRule(s, 7, &r));
    }
    {   // V03 and V04: a past scene left a choice or an event at 0.
        StoryState s = at(sc("p04"));
        CHECK(hasRule(s, 3));
        CHECK(hasRule(s, 4));
    }
    {   // V05: written for a scene not reached.
        StoryState s;
        put(s, StoryFact::s104_1, 1);
        CHECK(hasRule(s, 5));
    }
    {   // V08: the count below a passed milestone.
        StoryState s = at(sc("p07"));
        CHECK(hasRule(s, 8));
        s.wide[slotOf(StoryWide::c_milestone)] = 48;
        s.wideEntry = s.wide;
        CHECK_FALSE(hasRule(s, 8));
    }
    {   // V10 and V15: presses without their slot, or without a last kind.
        StoryState s;
        s.wide[slotOf(StoryWide::c_help_s1_k0)] = 1;
        CHECK(hasRule(s, 10));
        CHECK(hasRule(s, 15));
    }
    {   // V11: the track mark past the presses.
        StoryState s;
        s.wide[slotOf(StoryWide::c_115_help)] = 1;
        CHECK(hasRule(s, 11));
    }
    {   // V12: bits out of order.
        StoryState s;
        put(s, StoryFact::sp05_1, 2);
        CHECK(hasRule(s, 12));
    }
    {   // V13: a last-act fact without the choice.
        StoryState s;
        put(s, StoryFact::sf10_1, 1);
        CHECK(hasRule(s, 13));
    }
    {   // V14 and V19: a packed field holding 3 is also out of range.
        StoryState s;
        put(s, StoryFact::s203_1, 3);
        CHECK(hasRule(s, 14));
        CHECK(hasRule(s, 19));
    }
    {   // V16: a scene counter before its scene.
        StoryState s;
        s.wide[slotOf(StoryWide::c_303_declines)] = 1;
        CHECK(hasRule(s, 16));
    }
    {   // V17: facts moved with no scene open.
        StoryState s;
        s.facts[slotOf(StoryFact::sp02_1)] = 4;
        CHECK(hasRule(s, 17));
    }
    {   // V18: the rank ahead of its scenes.
        StoryState s = at(sc("115"));
        put(s, StoryFact::s112_1, 2);
        CHECK(hasRule(s, 18));
    }
    {   // V20: another registry of the same size.
        StoryState s;
        s.registryHash = reg.hash ^ 1u;
        s.factsKnown = reg.factsKnown;
        CHECK(hasRule(s, 20));
    }
    {   // The cap: never more than the span or 32.
        StoryState s = at(sc("f10"));
        std::array<StoryIssue, 3> few{};
        CHECK(storyValidate(s, reg, few) == 3);
        CHECK(issues(s) == kMaxStoryIssues);
    }
}

TEST_CASE("story state: no heap use while playing") {
    if (!haveRegistry()) return;
    const FactRegistry& reg = registry();
    StoryState s;
    const auto before = ftcore_test::allocStats();
    REQUIRE(storyBeginScene(s, sc("p02")));
    REQUIRE(storySet(s, reg, StoryFact::sp02_1, 4) == SetResult::Ok);
    REQUIRE(storyCountHelp(s, reg, 0, 0) == SetResult::Ok);
    const SceneReads reads = storySceneReads(s, reg, sc("p02"));
    std::array<StoryIssue, kMaxStoryIssues> out{};
    storyValidate(s, reg, out);
    REQUIRE(storyCommitScene(s, reg));
    std::array<std::uint8_t, kStoryStateBlockSize> block{};
    REQUIRE(storyEncode(s, reg, block));
    StoryState back;
    REQUIRE(storyDecode(block, reg, back) == StoryLoad::Ok);
    CHECK(ftcore_test::allocStats().count == before.count);
    CHECK(reads.complete);
}

// ---- act 1: every branch ----

namespace {

struct Walk {
    const FactRegistry* reg = nullptr;
    std::uint64_t leaves = 0;
    std::uint64_t failures = 0;
    std::string firstFailure;
    // What the leaf checks against.
    std::uint8_t choice032 = 0, ch104 = 0, ch116 = 0, ch113 = 0;
    std::uint32_t presses = 0;
};

using Apply = void (*)(StoryState&, const FactRegistry&, int);
struct Step {
    const char* scene;
    int variants;
    Apply apply;
};

void must(StoryState& s, const FactRegistry& r, StoryFact f, std::uint8_t v) {
    const SetResult res = storySet(s, r, f, v);
    if (res != SetResult::Ok) FAIL_CHECK("set " << std::string(storyFactId(f)) << "=" << int(v) << " -> " << int(res));
}
void bits(StoryState& s, const FactRegistry& r, StoryFact f, std::uint8_t b) {
    const SetResult res = storySetBits(s, r, f, b);
    if (res != SetResult::Ok) FAIL_CHECK("bits " << std::string(storyFactId(f)) << " -> " << int(res));
}
// OP: variant 1 does the act; variant 0 lets the commit mark it passed.
void op(StoryState& s, const FactRegistry& r, StoryFact f, int v) {
    if (v == 1) must(s, r, f, 2);
}

constexpr std::uint32_t kPresses[] = {0, 1, 7, 12};

const Step kAct1Steps[] = {
    {"p01", 1, [](StoryState&, const FactRegistry&, int) {}},
    {"p02", 1, [](StoryState& s, const FactRegistry& r, int) { must(s, r, StoryFact::sp02_1, 4); }},
    {"p03", 3,
     [](StoryState& s, const FactRegistry& r, int v) {
         must(s, r, StoryFact::sp03_1, 1);
         must(s, r, StoryFact::sp03_2, static_cast<std::uint8_t>(v + 1));
         if (storyCountHelp(s, r, 0, static_cast<std::uint8_t>(v)) != SetResult::Ok) FAIL_CHECK("press side 0");
     }},
    {"p04", 1,
     [](StoryState& s, const FactRegistry& r, int) {
         must(s, r, StoryFact::sp04_1, 1);
         must(s, r, StoryFact::sp04_2, 1);
         must(s, r, StoryFact::sp04_3, 1);
         must(s, r, StoryFact::sp04_4, 1);
     }},
    {"p05", 1, [](StoryState& s, const FactRegistry& r, int) { bits(s, r, StoryFact::sp05_1, 1); }},
    {"p06", 1,
     [](StoryState& s, const FactRegistry& r, int) {
         must(s, r, StoryFact::sp06_1, 1);
         must(s, r, StoryFact::sp06_2, 1);
     }},
    {"p07", 1,
     [](StoryState& s, const FactRegistry& r, int) {
         must(s, r, StoryFact::sp07_1, 1);
         must(s, r, StoryFact::sp07_2, 1);
         must(s, r, StoryFact::sp07_3, 8);
         must(s, r, StoryFact::sp07_4, 1);
     }},
    {"p08", 1, [](StoryState&, const FactRegistry&, int) {}},
    {"101", 1, [](StoryState&, const FactRegistry&, int) {}},
    {"102", 1,
     [](StoryState& s, const FactRegistry& r, int) {
         bits(s, r, StoryFact::sp05_1, 2);
         bits(s, r, StoryFact::sp07_3, 2 | 16);
     }},
    {"103", 1, [](StoryState&, const FactRegistry&, int) {}},
    {"104", 2,
     [](StoryState& s, const FactRegistry& r, int v) { must(s, r, StoryFact::s104_1, static_cast<std::uint8_t>(v + 1)); }},
    {"105", 2,
     [](StoryState& s, const FactRegistry& r, int v) {
         op(s, r, StoryFact::s105_1, v);
         must(s, r, StoryFact::s105_2, 1);
     }},
    {"106", 2,
     [](StoryState& s, const FactRegistry& r, int v) {
         must(s, r, StoryFact::s106_1, 1);
         must(s, r, StoryFact::s106_2, static_cast<std::uint8_t>(v + 1));
         must(s, r, StoryFact::s106_3, 1);
         bits(s, r, StoryFact::sp07_3, 64);
     }},
    {"107", 1, [](StoryState& s, const FactRegistry& r, int) { must(s, r, StoryFact::s107_1, 1); }},
    {"108", 2,
     [](StoryState& s, const FactRegistry& r, int v) {
         if (v == 1) must(s, r, StoryFact::s108_1, 7);
     }},
    {"109", 2, [](StoryState& s, const FactRegistry& r, int v) { op(s, r, StoryFact::s109_1, v); }},
    {"110", 4,
     [](StoryState& s, const FactRegistry& r, int v) {
         must(s, r, StoryFact::s110_1, static_cast<std::uint8_t>(v + 1));
         must(s, r, StoryFact::s110_2, 1);
     }},
    {"111", 4,
     [](StoryState& s, const FactRegistry& r, int v) {
         op(s, r, StoryFact::s111_1, v & 1);
         must(s, r, StoryFact::s111_2, static_cast<std::uint8_t>((v >> 1) + 1));
     }},
    {"112", 1,
     [](StoryState& s, const FactRegistry& r, int) {
         must(s, r, StoryFact::s112_1, 1);
         must(s, r, StoryFact::s112_2, 1);
         must(s, r, StoryFact::s112_3, 1);
         bits(s, r, StoryFact::sp05_1, 4);
     }},
    {"113", 2,
     [](StoryState& s, const FactRegistry& r, int v) { must(s, r, StoryFact::s113_1, static_cast<std::uint8_t>(v + 1)); }},
    {"114", 2,
     [](StoryState& s, const FactRegistry& r, int v) {
         op(s, r, StoryFact::s114_1, v);
         must(s, r, StoryFact::s114_2, 1);
     }},
    {"115", 8,
     [](StoryState& s, const FactRegistry& r, int v) {
         for (std::uint32_t i = 0; i < kPresses[v >> 1]; ++i)
             if (storyCountHelp(s, r, 1, static_cast<std::uint8_t>(i % 3)) != SetResult::Ok) FAIL_CHECK("press side 1");
         must(s, r, StoryFact::s115_1, 1);
         bits(s, r, StoryFact::sp05_1, 8);
         must(s, r, StoryFact::s115_2, 1);
         op(s, r, StoryFact::s115_3, v & 1);
     }},
    {"116", 4,
     [](StoryState& s, const FactRegistry& r, int v) {
         must(s, r, StoryFact::s116_1, static_cast<std::uint8_t>((v & 1) + 1));
         must(s, r, StoryFact::s116_2, static_cast<std::uint8_t>((v >> 1) + 1));
     }},
    {"117", 1, [](StoryState&, const FactRegistry&, int) {}},
};
constexpr std::size_t kSteps = sizeof(kAct1Steps) / sizeof(kAct1Steps[0]);

void leaf(Walk& w, StoryState& s) {
    const FactRegistry& r = *w.reg;
    ++w.leaves;
    std::string why;
    std::array<StoryIssue, kMaxStoryIssues> out{};
    if (storyValidate(s, r, out) != 0) why += "issues " + describe(s);
    if (storyWide(s, StoryWide::c_milestone) != 61) why += " mile";
    if (s.facts[slotOf(StoryFact::sp05_1)] != 15) why += " bits05";
    if (s.facts[slotOf(StoryFact::sp07_3)] != 90) why += " packed";
    if (storyWide(s, StoryWide::c_115_help) != std::min<std::uint32_t>(w.presses, r.tuning.trackLength)) why += " mark";
    const SceneReads reads = storySceneReads(s, r, sc("117"));
    if (!reads.complete) why += " reads";
    bool saw116 = false;
    for (std::uint8_t i = 0; i < reads.count; ++i)
        if (reads.facts[i].fact == slotOf(StoryFact::s116_1))
            saw116 = reads.facts[i].status == FactStatus::Set && reads.facts[i].value == w.ch116;
    if (!saw116) why += " read116";
    const DerivedValues& d = reads.derived;
    if (d.d1 != (w.choice032 != 1) || d.d3 != (w.ch116 == 1 ? 1 : 2) || d.d5 != w.ch113 || (d.valid & 0x77) != 0x77)
        why += " derived";
    if (d.d7 != std::max<std::uint32_t>(1, 1) + std::max<std::uint32_t>(1, w.presses)) why += " shown";
    // The block round-trips byte for byte (every eighth leaf: the rest are the same code path).
    if ((w.leaves & 7u) == 0) {
        std::array<std::uint8_t, kStoryStateBlockSize> a{}, b{};
        StoryState copy = s;
        storyEncode(copy, r, a);
        StoryState back;
        if (storyDecode(a, r, back) != StoryLoad::Ok) why += " decode";
        back.saveSerial = s.saveSerial;
        storyEncode(back, r, b);
        if (a != b) why += " bytes";
    }
    if (!why.empty()) {
        if (w.failures++ == 0) w.firstFailure = why;
    }
}

void walk(Walk& w, const StoryState& s, std::size_t step) {
    if (step == kSteps) {
        StoryState copy = s;
        leaf(w, copy);
        return;
    }
    const Step& st = kAct1Steps[step];
    for (int v = 0; v < st.variants; ++v) {
        StoryState next = s;
        if (!storyBeginScene(next, sc(st.scene))) FAIL_CHECK("begin " << st.scene);
        st.apply(next, *w.reg, v);
        if (!storyCommitScene(next, *w.reg)) FAIL_CHECK("commit " << st.scene);
        std::array<StoryEvent, kStoryEventRing> ev{};
        storyDrainEvents(next, ev);
        const std::string code = st.scene;
        if (code == "p03") w.choice032 = static_cast<std::uint8_t>(v + 1);
        if (code == "113") w.ch113 = static_cast<std::uint8_t>(v + 1);
        if (code == "115") w.presses = kPresses[v >> 1];
        if (code == "116") w.ch116 = static_cast<std::uint8_t>((v & 1) + 1);
        walk(w, next, step + 1);
    }
}

} // namespace

TEST_CASE("story state: every act 1 branch") {
    if (!haveRegistry()) return;
    Walk w;
    w.reg = &registry();
    const auto t0 = std::chrono::steady_clock::now();
    walk(w, StoryState{}, 0);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("act 1 leaves: " << w.leaves << " in " << secs << " s");
    CHECK(w.leaves == 3ull * 2 * 2 * 2 * 2 * 2 * 4 * 4 * 2 * 2 * 8 * 4);
    CAPTURE(w.firstFailure);
    CHECK(w.failures == 0);
}

TEST_CASE("story state: a restart in each act 1 scene gives the same end") {
    if (!haveRegistry()) return;
    const FactRegistry& reg = registry();
    StoryState straight;
    for (const Step& st : kAct1Steps) {
        REQUIRE(storyBeginScene(straight, sc(st.scene)));
        st.apply(straight, reg, st.variants - 1);
        REQUIRE(storyCommitScene(straight, reg));
    }
    StoryState retried;
    for (const Step& st : kAct1Steps) {
        REQUIRE(storyBeginScene(retried, sc(st.scene)));
        st.apply(retried, reg, 0);
        REQUIRE(storyRestartScene(retried, reg));
        st.apply(retried, reg, st.variants - 1);
        REQUIRE(storyCommitScene(retried, reg));
    }
    CHECK(retried.facts == straight.facts);
    CHECK(storyWide(retried, StoryWide::c_retries) == kSteps);
    CHECK(storyWide(retried, StoryWide::c_help_all) > storyWide(straight, StoryWide::c_help_all));
    CHECK(storyWide(retried, StoryWide::c_115_help) == storyWide(straight, StoryWide::c_115_help));
    CHECK(issues(retried) == 0);
}

namespace {
// The shape of the fact-reading interface the NPC memory uses.
struct FakeFactSource {
    virtual int npcFactIndex(std::string_view id) const = 0;
    virtual bool npcFactValue(int index, std::uint8_t& value) const = 0;

protected:
    ~FakeFactSource() = default;
};
} // namespace

TEST_CASE("story state: the fact reader for other systems") {
    if (!haveRegistry()) return;
    const FactRegistry& reg = registry();
    StoryState s;
    const StoryFactReader<FakeFactSource> reader(s, reg);
    const FakeFactSource& src = reader;
    const int choice = src.npcFactIndex("p03_2");
    const int number = src.npcFactIndex("p02_1");
    CHECK(choice == static_cast<int>(slotOf(StoryFact::sp03_2)));
    CHECK(src.npcFactIndex("nope") == -1);
    CHECK(src.npcFactIndex("c_milestone") == -1); // wide values are not facts
    std::uint8_t v = 99;
    CHECK_FALSE(src.npcFactValue(choice, v)); // unset
    CHECK_FALSE(src.npcFactValue(number, v)); // unset reads its default, yet is not set
    CHECK_FALSE(src.npcFactValue(-1, v));
    CHECK_FALSE(src.npcFactValue(0, v));
    CHECK_FALSE(src.npcFactValue(9999, v));
    CHECK(v == 99);
    REQUIRE(storyBeginScene(s, sc("p02")));
    REQUIRE(storySet(s, reg, StoryFact::sp02_1, 5) == SetResult::Ok);
    REQUIRE(storyCommitScene(s, reg));
    REQUIRE(storyBeginScene(s, sc("p03")));
    REQUIRE(storySet(s, reg, StoryFact::sp03_2, 2) == SetResult::Ok);
    CHECK(src.npcFactValue(choice, v));
    CHECK(v == 2);
    CHECK(src.npcFactValue(number, v));
    CHECK(v == 5);
    // Not offered and stale read as false.
    s.facts[slotOf(StoryFact::s104_1)] = kNotOffered;
    CHECK_FALSE(src.npcFactValue(src.npcFactIndex("104_1"), v));
    s.facts[slotOf(StoryFact::s104_1)] = 9;
    CHECK_FALSE(src.npcFactValue(src.npcFactIndex("104_1"), v));
    const auto before = ftcore_test::allocStats();
    for (int i = 0; i < 100; ++i) src.npcFactValue(choice, v);
    CHECK(ftcore_test::allocStats().count == before.count);
}
