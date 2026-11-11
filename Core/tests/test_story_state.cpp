#include "ftcore/StoryFactReader.h"
#include "ftcore/StoryState.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

using namespace ftcore;

namespace {

// ---- A small registry built here: every public test runs on it ----

const char* kCsv =
    "slot,id,kind,max,default,mut,set_scenes,gate,read_scenes,fallback,derive\n"
    "1,a_ev,EV,1,0,once,s2,-,s3,-,-\n"
    "2,a_num,NUM,9,3,mono,s2 s3,-,-,-,-\n"
    "3,a_ch,C3,3,0,once,s2,-,s4,all,-\n"
    "4,a_gated,C2,2,0,once,s3,a_ch=B,-,all,-\n"
    "5,a_bits,BITS,7,0,mono,s2 s3,-,-,-,-\n"
    "6,a_pk,PK,31,0,mono,s2 s3,-,-,-,-\n"
    "7,a_once_pk,PK,15,0,once,s2,-,-,-,-\n"
    "8,a_free,NUM,3,0,free,any,-,-,-,-\n"
    "9,a_op,OP,2,0,once,s3,a_ch=A,-,all,-\n"
    "10,a_opt,C2,2,0,once,s3,B:a_ch=C,-,all,-\n"
    "11,a_der,EV,1,0,once,s4,-,-,-,1:a_ch=A\n"
    "12,a_c4,C4,4,0,once,s5,D:a_ch=C,s6,all,-\n"
    "13,a_step,ST,3,0,mono,s5 s7 s9,-,s10,-,-\n"
    "14,a_last0,NUM,3,0,free,any,-,-,-,-\n"
    "15,a_last1,NUM,3,0,free,any,-,-,-,-\n"
    "16,a_first,EV,1,0,once,any,-,all,-,-\n"
    "17,a_open1,NUM,3,0,free,any,-,-,-,-\n"
    "18,a_need,OP,2,0,once,s8,a_pk.f1=2,s9,all,-\n"
    "19,a_late,C2,2,0,once,s11,a_c4=2,s12,all,-\n"
    "0,w_ctr,CTR,3,0,-,any,-,s3,-,-\n"
    "1,w_raw,RAW,100,0,-,any,-,-,-,-\n"
    "2,w_mask,MASK,15,0,-,s2,-,-,-,-\n"
    "3,w_p00,CTR,65535,0,-,any,-,s6,-,-\n"
    "4,w_p01,CTR,65535,0,-,any,-,s6,-,-\n"
    "5,w_p02,CTR,65535,0,-,any,-,s6,-,-\n"
    "6,w_p10,CTR,65535,0,-,any,-,s6,-,-\n"
    "7,w_p11,CTR,65535,0,-,any,-,s6,-,-\n"
    "8,w_p12,CTR,65535,0,-,any,-,s6,-,-\n"
    "9,w_scene,CTR,255,0,-,any,-,-,-,-\n"
    "10,w_mark,CTR,255,0,-,s6,-,s6,-,-\n"
    "11,w_mile,CTR,255,0,-,s5 s8,-,s9,-,-\n"
    "12,w_late,CTR,9,0,-,s9,-,-,-,-\n"
    "13,w_retry,RAW,65535,0,-,any,-,-,-,-\n"
    "14,w_total,RAW,4294967295,0,-,any,-,-,-,-\n";

const char* kJson = R"({
  "version": 2,
  "scenes": ["s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "s12"],
  "roles": {
    "pressCounters": [["w_p00", "w_p01", "w_p02"], ["w_p10", "w_p11", "w_p12"]],
    "pressScene": "w_scene",
    "pressTotal": "w_total",
    "retries": "w_retry",
    "milestone": "w_mile",
    "trackMark": "w_mark",
    "lastKind": ["a_last0", "a_last1"],
    "firstPress": "a_first",
    "sideOpen": ["", "a_open1"]
  },
  "track": { "length": 4, "scene": "s6", "side": 1 },
  "pressFloor": 1,
  "milestones": [
    { "scene": "s5", "value": 10 },
    { "scene": "s8", "value": 20, "needs": "a_free" }
  ],
  "derived": [
    { "kind": "in", "fact": "a_ch", "values": [2, 3] },
    { "kind": "read", "fact": "a_num" },
    { "kind": "value", "fact": "a_c4" },
    { "kind": "upgrade", "fact": "a_c4", "from": 1, "to": 3, "if": "a_num=7" },
    { "kind": "topKind" },
    { "kind": "pressTotal" }
  ],
  "rules": [
    { "number": 41, "kind": "needs", "when": "a_pk.f1=2", "then": "a_need" },
    { "number": 42, "kind": "needs", "when": "a_need=2", "then": "a_pk.f1=2", "settled": true },
    { "number": 43, "kind": "bitsInOrder", "fact": "a_bits" },
    { "number": 44, "kind": "needs", "when": "a_bits&4", "then": "a_ev" }
  ]
})";

const FactRegistry& reg() {
    static const FactRegistry r = [] {
        FactRegistry out;
        std::string err;
        const bool ok = importStoryRegistry(kCsv, kJson, out, &err);
        if (!ok) {
            MESSAGE(err);
            std::abort();
        }
        return out;
    }();
    return r;
}

std::uint8_t sc(const char* code) { return storySceneOrdinal(reg(), code); }
std::uint16_t fs(const char* id) { return storyFactSlot(reg(), id); }
std::uint8_t ws(const char* id) { return storyWideSlot(reg(), id); }

std::uint32_t issues(const StoryState& s, const FactRegistry& r, std::vector<StoryIssue>* list = nullptr) {
    std::array<StoryIssue, kMaxStoryIssues> out{};
    const std::uint32_t n = storyValidate(s, r, out);
    if (list) list->assign(out.begin(), out.begin() + n);
    return n;
}

bool hasRule(const StoryState& s, std::uint8_t rule, const FactRegistry& r = reg()) {
    std::vector<StoryIssue> list;
    issues(s, r, &list);
    for (const StoryIssue& i : list)
        if (i.rule == rule) return true;
    return false;
}

std::string describe(const StoryState& s, const FactRegistry& r) {
    std::vector<StoryIssue> list;
    issues(s, r, &list);
    std::string out;
    for (const StoryIssue& i : list)
        out += "V" + std::to_string(i.rule) + "(" + std::to_string(i.factA) + "," + std::to_string(i.factB) + ") ";
    return out;
}

void fixCrc(std::array<std::uint8_t, kStoryStateBlockSize>& b) {
    std::uint32_t crc = storyCrc32(std::span<const std::uint8_t>(b).first(28));
    crc = storyCrc32(std::span<const std::uint8_t>(b).subspan(32), crc);
    for (std::size_t i = 0; i < 4; ++i) b[28 + i] = static_cast<std::uint8_t>(crc >> (8 * i));
}

// Sets a fact as if a committed scene had written it.
void put(StoryState& s, std::uint16_t slot, std::uint8_t v) {
    s.facts[slot] = v;
    s.factsEntry[slot] = v;
}
void putWide(StoryState& s, std::uint8_t slot, std::uint32_t v) {
    s.wide[slot] = v;
    s.wideEntry[slot] = v;
}
StoryState committedThrough(const char* code) {
    StoryState s;
    s.lastCommitted = sc(code);
    return s;
}

// The game's registry, when Data/Story is here: extra cases only.
struct PrivateRegistry {
    FactRegistry reg;
    bool present = false;
    bool ok = false;
    std::string error;
};

const PrivateRegistry& privateRegistry() {
    static const PrivateRegistry r = [] {
        PrivateRegistry out;
        std::string csv, json;
        const bool haveCsv = ftcore_test::readProjectData("Story/story_facts.csv", csv);
        const bool haveJson = ftcore_test::readProjectData("Story/story_tuning.json", json);
        out.present = haveCsv || haveJson;
        if (!out.present) return out;
        // One without the other is a failure: importStoryRegistry refuses an empty half.
        out.ok = importStoryRegistry(csv, json, out.reg, &out.error);
        return out;
    }();
    return r;
}

bool projectFile(const char* path) {
    std::string unused;
    return ftcore_test::readProjectData(path, unused);
}
bool storyDataAbsent() { return !projectFile("Story/story_facts.csv") && !projectFile("Story/story_tuning.json"); }
bool walkAbsent() { return !projectFile("Story/story_walk.txt"); }

} // namespace

// ---- Formulas and the import ----

TEST_CASE("story state: formulas") {
    CHECK(storySaturatingAdd(5, 3, 10) == 8);
    CHECK(storySaturatingAdd(9, 3, 10) == 10);
    CHECK(storySaturatingAdd(0xFFFFFFFFu, 1, 0xFFFFFFFFu) == 0xFFFFFFFFu);
    const std::uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(storyCrc32(digits) == 0xCBF43926u);
    CHECK(storyCrc32(std::span(digits).subspan(4), storyCrc32(std::span(digits).first(4))) == 0xCBF43926u);

    const FactRegistry& r = reg();
    const auto& c = r.roles.pressCounters;
    StoryState s;
    CHECK(storyPressTotal(s, r) == 2);
    CHECK(storyTopKind(s, r) == 0);
    s.wide[c[0][1]] = 1;
    CHECK(storyPressTotal(s, r) == 2);
    for (int side = 0; side < 2; ++side)
        for (int k = 0; k < 3; ++k) s.wide[c[static_cast<std::size_t>(side)][static_cast<std::size_t>(k)]] = 65535;
    CHECK(storyPressTotal(s, r) == 393210);
    // The totals saturate, even past the caps.
    for (int side = 0; side < 2; ++side)
        for (int k = 0; k < 3; ++k) s.wide[c[static_cast<std::size_t>(side)][static_cast<std::size_t>(k)]] = 0xFFFFFFFFu;
    CHECK(storyPressTotal(s, r) == 0xFFFFFFFFu);
    CHECK(storyPressTotal(s, FactRegistry{}) == 0); // nothing bound, no floor
    FactRegistry floored;
    floored.tuning.pressFloor = 5;
    CHECK(storyPressTotal(StoryState{}, floored) == 10);

    // Most pressed wins; a tie goes to the last kind pressed if it is tied, else to the lowest tied kind.
    const std::uint16_t last0 = r.roles.lastKind[0];
    StoryState t;
    t.wide[c[0][0]] = 2;
    t.wide[c[0][1]] = 3;
    t.wide[c[0][2]] = 3;
    CHECK(storyTopKind(t, r) == 1);
    t.facts[last0] = 3;
    CHECK(storyTopKind(t, r) == 2);
    t.facts[last0] = 1; // the last kind is not among the tied
    CHECK(storyTopKind(t, r) == 1);
    StoryState u;
    u.wide[c[0][0]] = u.wide[c[0][1]] = u.wide[c[0][2]] = 7;
    u.facts[last0] = 2;
    CHECK(storyTopKind(u, r) == 1);
    CHECK(storyTopKind(u, FactRegistry{}) == 0);
}

TEST_CASE("story state: scene codes and ids") {
    const FactRegistry& r = reg();
    CHECK(r.sceneCount == 12);
    CHECK(sc("s1") == 0);
    CHECK(sc("s12") == 11);
    CHECK(sc("s13") == kNoScene);
    CHECK(sc("") == kNoScene);
    for (std::uint8_t i = 0; i < r.sceneCount; ++i) CHECK(storySceneOrdinal(r, storySceneCode(r, i)) == i);
    CHECK(storySceneCode(r, r.sceneCount).empty());
    CHECK(fs("a_ev") == 1);
    CHECK(fs("w_ctr") == 0); // a wide id is not a fact
    CHECK(fs("nope") == 0);
    CHECK(ws("w_total") == 14);
    CHECK(ws("a_ev") == kNoWide);
    CHECK(r.factsKnown == 19);
    CHECK(r.wideKnown == 15);
    CHECK(r.hash != 0);
    CHECK(r.tuning.trackScene == sc("s6"));
    CHECK(r.tuning.milestoneCount == 2);
    CHECK(r.derivedCount == 6);
    CHECK(r.ruleCount == 4);
    for (std::uint8_t k = 0; k < r.sceneCount; ++k) CHECK(storySceneReads(StoryState{}, r, k).complete);
    CHECK_FALSE(storySceneReads(StoryState{}, r, r.sceneCount).complete);
}

TEST_CASE("story state: the csv refuses bad rows") {
    const std::string head = "slot,id,kind,max,default,mut,set_scenes,gate,read_scenes,fallback,derive\n";
    const std::string json = R"({"version":2,"scenes":["s1","s2","s3","s4","s5"]})";
    auto fails = [&](const std::string& rows, const std::string& expect) {
        FactRegistry r;
        std::string err;
        CAPTURE(rows);
        CHECK_FALSE(importStoryRegistry(head + rows, json, r, &err));
        CAPTURE(err);
        CHECK(err.find(expect) != std::string::npos);
        CHECK_FALSE(r.facts[1].used); // nothing half-loaded
        CHECK(r.hash == 0);
        CHECK(r.sceneCount == 0);
    };
    {
        FactRegistry r;
        std::string err;
        CHECK_FALSE(importStoryRegistry("slot,id\n", json, r, &err));
        CHECK(err.find("header") != std::string::npos);
    }
    fails("1,a,EV,1,0,once,s2,-,-,-\n", "expected 11 fields");
    fails("1,Bad,EV,1,0,once,s2,-,-,-,-\n", "id:");
    fails("1,a,EV,1,0,once,s2,-,-,-,-\n2,a,EV,1,0,once,s2,-,-,-,-\n", "id duplicated: a");
    fails("1,a,EV,1,0,once,s2,-,-,-,-\n1,b,EV,1,0,once,s2,-,-,-,-\n", "slot reused: 1");
    fails("0,a,EV,1,0,once,s2,-,-,-,-\n", "slot: expected 1..255");
    fails("64,w,CTR,3,0,-,any,-,-,-,-\n", "slot: expected 0..63");
    fails("1,a,XX,1,0,once,s2,-,-,-,-\n", "kind:");
    fails("1,a,C3,2,0,once,s2,-,-,all,-\n", "max: does not fit the kind");
    fails("1,a,NUM,5,6,mono,s2,-,-,-,-\n", "default not representable");
    fails("1,a,EV,1,0,maybe,s2,-,-,-,-\n", "mut:");
    fails("1,a,EV,1,0,once,s2 s3,-,-,-,-\n", "a once fact with more than one set scene");
    fails("1,a,EV,1,0,once,q2,-,-,-,-\n", "set_scenes");
    fails("1,a,EV,1,0,once,s3,-,s2,-,-\n", "before any scene sets it");
    fails("1,a,C2,2,0,once,s3,-,s4 s5,s4,-\n", "no fallback marked for the read in s5");
    fails("1,a,C2,2,0,once,s3,nope=A,-,all,-\n", "unknown fact nope");
    fails("1,a,C2,2,0,once,s3,b=A,-,all,-\n2,b,C2,2,0,once,s4,-,-,all,-\n", "is set in a later scene");
    fails("1,a,C2,2,0,once,s3,C:b=A,-,all,-\n2,b,C2,2,0,once,s2,-,-,all,-\n", "option gate");
    fails("1,a,C2,2,0,once,s3,b.f2=1,-,all,-\n2,b,PK,15,0,mono,s2,-,-,-,-\n", "has no field 2");
    fails("1,a,EV,1,0,once,s3,-,-,-,5:b=1\n2,b,EV,1,0,once,s2,-,-,-,-\n", "derive:");
    fails("1,a,C2,2,0,once,s3,b=7,-,all,-\n2,b,C3,3,0,once,s2,-,-,all,-\n", "7 is not a value of b");
    fails("1,a,C2,2,0,once,s3,b.f0=3,-,all,-\n2,b,PK,15,0,mono,s2,-,-,-,-\n", "3 is not a value of b");
    fails("1,a,C2,2,0,once,s3,a=A,-,all,-\n", "names its own fact");
    std::string many;
    for (int i = 1; i <= 25; ++i) many += std::to_string(i) + ",f" + std::to_string(i) + ",EV,1,0,once,s2,-,s3,-,-\n";
    fails(many, "scene s3 reads more than 24 facts");
}

TEST_CASE("story state: the json refuses bad settings") {
    const std::string scenes = R"("scenes":["s1","s2","s3","s4","s5","s6","s7","s8","s9","s10","s11","s12"])";
    auto build = [&](const std::string& rest) { return "{\"version\":2," + scenes + rest + "}"; };
    auto fails = [&](const std::string& json, const std::string& expect, const std::string& csv = kCsv) {
        FactRegistry r;
        std::string err;
        CAPTURE(json);
        CHECK_FALSE(importStoryRegistry(csv, json, r, &err));
        CAPTURE(err);
        CHECK(err.find(expect) != std::string::npos);
        CHECK(r.hash == 0);
        CHECK(r.sceneCount == 0);
        CHECK_FALSE(r.facts[1].used);
    };
    {   // Both halves are required: one alone is an error, never a skip.
        FactRegistry r;
        std::string err;
        CHECK_FALSE(importStoryRegistry(kCsv, "", r, &err));
        CHECK(err.find("missing") != std::string::npos);
        CHECK_FALSE(importStoryRegistry("", kJson, r, &err));
        CHECK(err.find("missing") != std::string::npos);
    }
    fails("{\"version\":1," + scenes + "}", "version");
    fails("{\"version\":2,\"scenes\":[]}", "scenes");
    fails("{\"version\":2,\"scenes\":[\"s1\",\"s1\"]}", "duplicated: s1");
    fails("{\"version\":2,\"scenes\":[\"S1\"]}", "expected 1..7 characters");
    fails("{\"version\":2,\"scenes\":[\"abcdefgh\"]}", "expected 1..7 characters");
    fails("not json", "story_tuning.json");
    // Roles.
    fails(build(R"(,"roles":{"pressScene":"nope"})"), "roles.pressScene");
    fails(build(R"(,"roles":{"pressScene":"w_mask"})"), "expected a counter id");
    fails(build(R"(,"roles":{"pressCounters":[["w_p00"]]})"), "roles.pressCounters");
    fails(build(R"(,"roles":{"pressCounters":[["w_p00","w_p01","w_total"],["w_p10","w_p11","w_p12"]]})"),
          "a press counter is a CTR with max at most 65535");
    {   // A press counter with a cap past 65535 is refused (the totals rely on it).
        std::string csv = kCsv;
        const std::string from = "3,w_p00,CTR,65535,";
        csv.replace(csv.find(from), from.size(), "3,w_p00,CTR,65536,");
        fails(build(R"(,"roles":{"pressCounters":[["w_p00","w_p01","w_p02"],["w_p10","w_p11","w_p12"]]})"),
              "a press counter is a CTR with max at most 65535", csv);
    }
    fails(build(R"(,"roles":{"firstPress":"nope"})"), "roles.firstPress");
    fails(build(R"(,"roles":{"lastKind":["a_ev","a_last1"]})"), "expected a NUM fact with max at least 3");
    fails(build(R"(,"roles":{"sideOpen":["a_ev"]})"), "roles.sideOpen");
    fails(build(R"(,"roles":{"pressTotal":"w_retry","retries":"w_retry"})"), "wide w_retry bound to two roles");
    fails(build(R"(,"roles":{"pressCounters":[["w_p00","w_p01","w_p02"],["w_p10","w_p11","w_p00"]]})"),
          "wide w_p00 bound to two roles");
    fails(build(R"(,"roles":{"firstPress":"a_open1","sideOpen":["","a_open1"]})"), "fact a_open1 bound to two roles");
    fails(build(R"(,"pressFloor":70000)"), "pressFloor");
    fails(build(R"(,"pressFloor":"one")"), "pressFloor");
    // The track.
    fails(build(R"(,"roles":{"trackMark":"w_mark"})"), "the trackMark role needs a track");
    fails(build(R"(,"track":{"length":0,"scene":"s6","side":1})"), "track.length");
    fails(build(R"(,"track":{"length":4,"scene":"zz","side":1})"), "track.scene");
    fails(build(R"(,"track":{"length":4,"scene":"s6","side":2})"), "track.side");
    fails(build(R"(,"roles":{"trackMark":"w_ctr"},"track":{"length":4,"scene":"s6","side":1})"),
          "above the trackMark counter's max");
    // Milestones.
    fails(build(R"(,"milestones":[{"scene":"s5","value":10}])"), "need the milestone role");
    const std::string mile = R"(,"roles":{"milestone":"w_mile"})";
    fails(build(mile + R"(,"milestones":[{"scene":"s8","value":10},{"scene":"s5","value":20}])"),
          "expected a later scene and a value no lower");
    fails(build(mile + R"(,"milestones":[{"scene":"s5","value":10},{"scene":"s8","value":5}])"),
          "expected a later scene and a value no lower");
    fails(build(mile + R"(,"milestones":[{"scene":"s5","value":256}])"), "above the milestone counter's max");
    fails(build(mile + R"(,"milestones":[{"scene":"s5","value":0}])"), "milestones[0].value");
    fails(build(mile + R"(,"milestones":[{"scene":"zz","value":3}])"), "milestones[0].scene");
    // Derived values.
    fails(build(R"(,"derived":[{"kind":"guess","fact":"a_ch"}])"), "derived[0].kind");
    fails(build(R"(,"derived":[{"kind":"leadKind"}])"), "derived[0].kind"); // the old name is gone
    fails(build(R"(,"derived":[{"kind":"read"}])"), "derived[0].fact");
    fails(build(R"(,"derived":[{"kind":"in","fact":"a_ch","values":[]}])"), "derived[0].values");
    fails(build(R"(,"derived":[{"kind":"in","fact":"a_ch","values":[32]}])"), "expected 0..31");
    fails(build(R"(,"derived":[{"kind":"upgrade","fact":"a_c4","from":9,"to":3,"if":"a_num=7"}])"), "upgrade:");
    fails(build(R"(,"derived":[{"kind":"upgrade","fact":"a_c4","from":1,"to":3,"if":"a_pk.f0=1"}])"),
          "derived[0].if");
    // Rules.
    fails(build(R"(,"rules":[{"number":0,"kind":"needs","when":"a_ev","then":"a_ch"}])"), "rules[0].number");
    fails(build(R"(,"rules":[{"number":5,"kind":"maybe","when":"a_ev","then":"a_ch"}])"), "rules[0].kind");
    fails(build(R"(,"rules":[{"number":5,"kind":"needs","when":"nope","then":"a_ch"}])"), "rules[0].when");
    fails(build(R"(,"rules":[{"number":5,"kind":"needs","when":"a_ev","then":"a_pk.f9=1"}])"), "rules[0].then");
    fails(build(R"(,"rules":[{"number":5,"kind":"bitsInOrder","fact":"a_num"}])"), "expected a BITS fact");
    // A minimal registry: the scenes alone.
    FactRegistry r;
    std::string err;
    CHECK(importStoryRegistry(kCsv, build(""), r, &err));
    CAPTURE(err);
    CHECK(r.roles.milestone == kNoWide);
    CHECK(r.derivedCount == 0);
}

TEST_CASE("story state: the csv may start with a byte order mark") {
    FactRegistry r;
    std::string err;
    CHECK(importStoryRegistry(std::string("\xEF\xBB\xBF") + kCsv, kJson, r, &err));
    CAPTURE(err);
    CHECK(r.facts[1].used);
    CHECK(r.hash == reg().hash);
}

TEST_CASE("story state: a gate naming its own fact gives one error") {
    FactRegistry r;
    std::string err;
    CHECK_FALSE(importStoryRegistry("slot,id,kind,max,default,mut,set_scenes,gate,read_scenes,fallback,derive\n"
                                    "1,a,C2,2,0,once,s3,a=7,-,all,-\n",
                                    R"({"version":2,"scenes":["s1","s2","s3"]})", r, &err));
    CAPTURE(err);
    CHECK(err.find("names its own fact") != std::string::npos);
    CHECK(err.find("not a value") == std::string::npos);
}

// ---- Play ----

TEST_CASE("story state: set and get") {
    const FactRegistry& r = reg();
    StoryState s;
    CHECK(storyGet(s, r, fs("a_num")).status == FactStatus::Unset);
    CHECK(storyGet(s, r, fs("a_num")).value == 3); // the default
    CHECK(storyGet(s, r, 40).status == FactStatus::Unknown);
    CHECK(storySet(s, r, fs("a_ev"), 1) == SetResult::RejectedNoScene);
    CHECK(storySet(s, r, 40, 1) == SetResult::UnknownFact);

    REQUIRE(storyBeginScene(s, r, sc("s2")));
    CHECK(storySet(s, r, fs("a_gated"), 1) == SetResult::RejectedWrongScene);
    CHECK(storySet(s, r, fs("a_ev"), 2) == SetResult::RejectedRange);
    CHECK(storySet(s, r, fs("a_ev"), 0) == SetResult::RejectedRange);
    CHECK(storySet(s, r, fs("a_ev"), kNotOffered) == SetResult::RejectedRange);
    CHECK(storySet(s, r, fs("a_ev"), 1) == SetResult::Ok);
    CHECK(storySet(s, r, fs("a_ev"), 1) == SetResult::Unchanged);
    CHECK(storyGet(s, r, fs("a_ev")).status == FactStatus::Set);
    CHECK(s.stamp[fs("a_ev")] == sc("s2"));

    CHECK(storySet(s, r, fs("a_num"), 2) == SetResult::RejectedDownward); // below the default
    CHECK(storySet(s, r, fs("a_num"), 5) == SetResult::Ok);
    CHECK(storySet(s, r, fs("a_num"), 4) == SetResult::RejectedDownward);
    CHECK(storySet(s, r, fs("a_num"), 10) == SetResult::RejectedRange);

    CHECK(storySet(s, r, fs("a_ch"), 4) == SetResult::RejectedRange);
    CHECK(storySet(s, r, fs("a_ch"), 2) == SetResult::Ok);
    CHECK(storySet(s, r, fs("a_ch"), 3) == SetResult::RejectedOnce);

    CHECK(storySetBits(s, r, fs("a_bits"), 1) == SetResult::Ok);
    CHECK(storySetBits(s, r, fs("a_bits"), 4) == SetResult::Ok);
    CHECK(storyGet(s, r, fs("a_bits")).value == 5);
    CHECK(storySet(s, r, fs("a_bits"), 4) == SetResult::RejectedDownward);
    CHECK(storySetBits(s, r, fs("a_bits"), 8) == SetResult::RejectedRange);

    // Packed: fields 0..1 (bits 0..3), flag bit 4; a field never holds 3.
    const std::uint16_t pk = fs("a_pk"), once = fs("a_once_pk");
    CHECK(storySet(s, r, pk, 3) == SetResult::RejectedRange);
    CHECK(storySet(s, r, pk, 1) == SetResult::Ok);
    CHECK(storySet(s, r, pk, 2) == SetResult::Ok); // field 0 up
    CHECK(storySet(s, r, pk, 1) == SetResult::RejectedDownward);
    CHECK(storySetBits(s, r, pk, 16) == SetResult::Ok); // the flag
    CHECK(storySet(s, r, pk, 2 | 4) == SetResult::RejectedDownward); // drops the flag
    CHECK(storySet(s, r, once, 1) == SetResult::Ok);
    CHECK(storySet(s, r, once, 1 | 8) == SetResult::Ok); // a new field is fine once
    CHECK(storySet(s, r, once, 2 | 8) == SetResult::RejectedOnce);

    CHECK(storySet(s, r, fs("a_free"), 3) == SetResult::Ok); // any scene, free
    CHECK(storySet(s, r, fs("a_free"), 1) == SetResult::Ok);

    // Every rejection is an event.
    std::array<StoryEvent, kStoryEventRing> ev{};
    const std::uint32_t n = storyDrainEvents(s, ev);
    std::uint32_t rejected = 0;
    for (std::uint32_t i = 0; i < n; ++i) rejected += ev[i].kind == StoryEvent::Kind::Rejected;
    CHECK(rejected > 10);
    CHECK(storyDrainEvents(s, ev) == 0);
}

TEST_CASE("story state: stepped facts and their check") {
    const FactRegistry& r = reg();
    const std::uint16_t st = fs("a_step");
    StoryState s;
    REQUIRE(storyBeginScene(s, r, sc("s5")));
    CHECK(storySet(s, r, st, 4) == SetResult::RejectedRange);
    CHECK(storySet(s, r, st, 1) == SetResult::Ok);
    REQUIRE(storyCommitScene(s, r));
    CHECK_FALSE(hasRule(s, 18));
    REQUIRE(storyBeginScene(s, r, sc("s7")));
    CHECK(storySet(s, r, st, 2) == SetResult::Ok);
    CHECK(storySet(s, r, st, 1) == SetResult::RejectedDownward);
    REQUIRE(storyCommitScene(s, r));
    CHECK_FALSE(hasRule(s, 18));
    // Two steps in one scene is taken, and validation names it.
    StoryState t;
    REQUIRE(storyBeginScene(t, r, sc("s5")));
    REQUIRE(storySet(t, r, st, 2) == SetResult::Ok);
    REQUIRE(storyCommitScene(t, r));
    CHECK(hasRule(t, 18));
}

TEST_CASE("story state: gates and the commit") {
    const FactRegistry& r = reg();
    for (std::uint8_t choice = 1; choice <= 3; ++choice) {
        CAPTURE(int(choice));
        StoryState s;
        REQUIRE(storyBeginScene(s, r, sc("s2")));
        REQUIRE(storySet(s, r, fs("a_ev"), 1) == SetResult::Ok);
        REQUIRE(storySet(s, r, fs("a_ch"), choice) == SetResult::Ok);
        REQUIRE(storyCommitScene(s, r));
        REQUIRE(storyBeginScene(s, r, sc("s3")));
        CHECK((storySet(s, r, fs("a_gated"), 1) == SetResult::Ok) == (choice == 2));
        CHECK((storySet(s, r, fs("a_opt"), 2) == SetResult::Ok) == (choice == 3)); // option B needs a_ch=C
        REQUIRE(storyCommitScene(s, r));
        // A gated choice left unanswered closes as not offered; an optional act as passed or not offered.
        CHECK(storyGet(s, r, fs("a_gated")).status == (choice == 2 ? FactStatus::Set : FactStatus::NotOffered));
        CHECK(storyGet(s, r, fs("a_op")).value == (choice == 1 ? 1 : kNotOffered));
        CHECK(storyGet(s, r, fs("a_opt")).status == (choice == 3 ? FactStatus::Set : FactStatus::Unset));
    }
}

TEST_CASE("story state: wide values") {
    const FactRegistry& r = reg();
    const std::uint8_t ctr = ws("w_ctr"), mask = ws("w_mask");
    StoryState s;
    CHECK(storyAdd(s, r, ctr, 1) == SetResult::RejectedNoScene);
    REQUIRE(storyBeginScene(s, r, sc("s2")));
    CHECK(storyAdd(s, r, ctr, 2) == SetResult::Ok);
    CHECK(storyAdd(s, r, ctr, 5) == SetResult::Ok);
    CHECK(storyWide(s, ctr) == 3);
    CHECK(storyAdd(s, r, ctr, 1) == SetResult::Unchanged);
    CHECK(storyAdd(s, r, mask, 16) == SetResult::RejectedRange);
    CHECK(storyAdd(s, r, mask, 5) == SetResult::Ok);
    CHECK(storyAdd(s, r, mask, 1) == SetResult::Unchanged);
    CHECK(storyAdd(s, r, 40, 1) == SetResult::UnknownFact);
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
    REQUIRE(storyBeginScene(s, r, sc("s3")));
    CHECK(storyAdd(s, r, mask, 8) == SetResult::RejectedWrongScene);
}

TEST_CASE("story state: presses, the track and milestones") {
    const FactRegistry& r = reg();
    const StoryRoles& ro = r.roles;
    StoryState s;
    CHECK(storyCountPress(s, r, 0, 0) == SetResult::RejectedNoScene);
    REQUIRE(storyBeginScene(s, r, sc("s2")));
    REQUIRE(storySet(s, r, fs("a_ev"), 1) == SetResult::Ok);
    REQUIRE(storySet(s, r, fs("a_ch"), 3) == SetResult::Ok);
    CHECK(storyCountPress(s, r, 2, 0) == SetResult::RejectedRange);
    CHECK(storyCountPress(s, r, 0, 3) == SetResult::RejectedRange);
    CHECK(storyCountPress(s, r, 1, 0) == SetResult::RejectedGate); // side 1 waits for its open fact
    CHECK(storyCountPress(s, r, 0, 2) == SetResult::Ok);
    CHECK(storyWide(s, ro.pressCounters[0][2]) == 1);
    CHECK(storyWide(s, ro.pressScene) == 1);
    CHECK(storyWide(s, ro.pressTotal) == 1);
    CHECK(s.facts[ro.lastKind[0]] == 3);
    CHECK(s.facts[ro.firstPress] == 1);
    REQUIRE(storyCommitScene(s, r));
    REQUIRE(storyBeginScene(s, r, sc("s3")));
    REQUIRE(storySet(s, r, fs("a_opt"), 1) == SetResult::Ok);
    REQUIRE(storyCommitScene(s, r));
    REQUIRE(storyBeginScene(s, r, sc("s4")));
    REQUIRE(storySet(s, r, fs("a_der"), 1) == SetResult::Ok);
    REQUIRE(storyCommitScene(s, r));
    REQUIRE(storyBeginScene(s, r, sc("s5")));
    CHECK(storyWide(s, ro.pressScene) == 0); // zeroed as the scene began
    REQUIRE(storySet(s, r, fs("a_open1"), 1) == SetResult::Ok);
    REQUIRE(storySet(s, r, fs("a_c4"), 1) == SetResult::Ok);
    REQUIRE(storyCountPress(s, r, 1, 0) == SetResult::Ok); // before the track scene: not on the mark
    REQUIRE(storyCommitScene(s, r));
    CHECK(storyWide(s, ro.milestone) == 10);
    REQUIRE(storyBeginScene(s, r, sc("s6")));
    for (int i = 0; i < 6; ++i) REQUIRE(storyCountPress(s, r, 1, static_cast<std::uint8_t>(i % 3)) == SetResult::Ok);
    REQUIRE(storyCountPress(s, r, 0, 0) == SetResult::Ok); // the other side: not on the mark
    REQUIRE(storyCommitScene(s, r));
    CHECK(storyWide(s, ro.trackMark) == 4); // six presses, four squares
    CHECK(s.facts[ro.lastKind[1]] == 3);
    CHECK(storyPressTotal(s, r) == 2 + 7);
    CAPTURE(describe(s, r));
    CHECK(issues(s, r) == 0);

    // Nothing bound: a press is unknown and nothing moves.
    const FactRegistry none;
    StoryState t;
    REQUIRE(storyBeginScene(t, none, 3));
    CHECK(storyCountPress(t, none, 0, 0) == SetResult::UnknownFact);
    CHECK(t.wide == StoryState{}.wide);
}

TEST_CASE("story state: restart and the scene order") {
    const FactRegistry& r = reg();
    const StoryRoles& ro = r.roles;
    StoryState s;
    REQUIRE(storyBeginScene(s, r, sc("s2")));
    CHECK_FALSE(storyBeginScene(s, r, sc("s3"))); // one open scene
    REQUIRE(storySet(s, r, fs("a_num"), 5) == SetResult::Ok);
    REQUIRE(storyCountPress(s, r, 0, 1) == SetResult::Ok);
    REQUIRE(storyAdd(s, r, ws("w_raw"), 7) == SetResult::Ok);
    CHECK(storyWide(s, ro.pressTotal) == 1);
    REQUIRE(storyRestartScene(s, r));
    CHECK(storyGet(s, r, fs("a_num")).status == FactStatus::Unset);
    CHECK(storyGet(s, r, ro.firstPress).status == FactStatus::Unset);
    CHECK(storyWide(s, ro.pressCounters[0][1]) == 0);
    CHECK(storyWide(s, ro.pressTotal) == 1); // raw: kept
    CHECK(storyWide(s, ws("w_raw")) == 7);
    CHECK(storyWide(s, ro.retries) == 1);
    CHECK(s.stamp[fs("a_num")] == kNoScene);
    REQUIRE(storySet(s, r, fs("a_num"), 4) == SetResult::Ok);
    REQUIRE(storySet(s, r, fs("a_ev"), 1) == SetResult::Ok);
    REQUIRE(storySet(s, r, fs("a_ch"), 2) == SetResult::Ok);
    REQUIRE(storyCommitScene(s, r));
    CHECK_FALSE(storyRestartScene(s, r));
    CHECK_FALSE(storyCommitScene(s, r));
    CHECK_FALSE(storyBeginScene(s, r, sc("s2"))); // not after the last committed
    CHECK_FALSE(storyBeginScene(s, r, kMaxStoryScenes));
    CHECK(storyBeginScene(s, r, sc("s3"))); // the next one
    CHECK(issues(s, r) == 0);
}

TEST_CASE("story state: the sandbox leaves the real state byte for byte") {
    const FactRegistry& r = reg();
    StoryState played;
    REQUIRE(storyBeginScene(played, r, sc("s2")));
    REQUIRE(storySet(played, r, fs("a_ev"), 1) == SetResult::Ok);
    REQUIRE(storySet(played, r, fs("a_ch"), 2) == SetResult::Ok); // a_ch = B
    REQUIRE(storySet(played, r, fs("a_num"), 5) == SetResult::Ok); // written in s2 and s3
    REQUIRE(storyCountPress(played, r, 0, 0) == SetResult::Ok);
    REQUIRE(storyCommitScene(played, r));
    REQUIRE(storyBeginScene(played, r, sc("s3")));
    REQUIRE(storySet(played, r, fs("a_gated"), 1) == SetResult::Ok); // once, gated on a_ch = B
    REQUIRE(storySet(played, r, fs("a_num"), 7) == SetResult::Ok);
    REQUIRE(storySet(played, r, fs("a_opt"), 1) == SetResult::Ok);
    REQUIRE(storyCommitScene(played, r));
    std::array<StoryEvent, kStoryEventRing> ev{};
    storyDrainEvents(played, ev);

    std::array<std::uint8_t, kStoryStateBlockSize> realBefore{};
    StoryState forBytes = played;
    REQUIRE(storyEncode(forBytes, r, realBefore));
    const StoryState snapshot = played;

    StoryState box;
    storyMakeSandbox(played, r, sc("s3"), box);
    CHECK(box.lastCommitted == sc("s2"));
    CHECK(storyGet(box, r, fs("a_ev")).value == 1);
    CHECK(storyGet(box, r, fs("a_gated")).status == FactStatus::Unset); // written at the chosen scene
    CHECK(box.stamp[fs("a_gated")] == kNoScene);
    CHECK(storyGet(box, r, fs("a_num")).value == 7); // several writers: kept
    // Use the box hard: replay, press, restart, commit, read, validate.
    REQUIRE(storyBeginScene(box, r, sc("s3")));
    CHECK(storySet(box, r, fs("a_gated"), 2) == SetResult::Ok);
    REQUIRE(storyCountPress(box, r, 0, 2) == SetResult::Ok);
    REQUIRE(storyRestartScene(box, r));
    REQUIRE(storySet(box, r, fs("a_gated"), 1) == SetResult::Ok);
    REQUIRE(storyCommitScene(box, r));
    REQUIRE(storyBeginScene(box, r, sc("s4")));
    REQUIRE(storySet(box, r, fs("a_der"), 1) == SetResult::Ok);
    storySceneReads(box, r, sc("s4"));
    std::array<StoryIssue, kMaxStoryIssues> out{};
    storyValidate(box, r, out);
    std::array<std::uint8_t, kStoryStateBlockSize> block{};
    block[0] = 7;
    CHECK_FALSE(storyEncode(box, r, block)); // never encoded
    CHECK(block[0] == 7);

    // The real state is untouched: the same bytes, the same fields.
    CHECK(played.facts == snapshot.facts);
    CHECK(played.stamp == snapshot.stamp);
    CHECK(played.wide == snapshot.wide);
    CHECK(played.eventCount == snapshot.eventCount);
    std::array<std::uint8_t, kStoryStateBlockSize> realAfter{};
    forBytes = played;
    REQUIRE(storyEncode(forBytes, r, realAfter));
    CHECK(realAfter == realBefore);
}

// ---- The block ----

TEST_CASE("story state: the block") {
    const FactRegistry& r = reg();
    StoryState s;
    REQUIRE(storyBeginScene(s, r, sc("s2")));
    REQUIRE(storySet(s, r, fs("a_num"), 6) == SetResult::Ok);
    REQUIRE(storySet(s, r, fs("a_ev"), 1) == SetResult::Ok);
    REQUIRE(storySet(s, r, fs("a_ch"), 1) == SetResult::Ok);
    REQUIRE(storyCommitScene(s, r));
    REQUIRE(storyBeginScene(s, r, sc("s3")));
    REQUIRE(storySet(s, r, fs("a_num"), 8) == SetResult::Ok);
    REQUIRE(storyCountPress(s, r, 0, 2) == SetResult::Ok);

    std::array<std::uint8_t, kStoryStateBlockSize> block{};
    REQUIRE(storyEncode(s, r, block));
    CHECK(s.saveSerial == 1);
    CHECK(block[0] == 0x46);
    CHECK(block[3] == 0x53);
    CHECK((block[18] & kStoryMidScene) != 0);

    StoryState back;
    StoryStateLoadReport rep;
    REQUIRE(storyDecode(block, r, back, &rep) == StoryLoad::Ok);
    CHECK(back.facts == s.facts);
    CHECK(back.factsEntry == s.factsEntry);
    CHECK(back.stamp == s.stamp);
    CHECK(back.stampEntry == s.stampEntry);
    CHECK(back.wide == s.wide);
    CHECK(back.wideEntry == s.wideEntry);
    CHECK(back.openScene == sc("s3"));
    CHECK(back.lastCommitted == sc("s2"));
    CHECK(back.saveSerial == 1);
    CHECK(back.registryHash == r.hash);
    CHECK_FALSE(rep.registryMismatch);
    CHECK(rep.unknownTags == 0);
    // A restart after loading still goes back to the scene's start.
    REQUIRE(storyRestartScene(back, r));
    CHECK(storyGet(back, r, fs("a_num")).value == 6);
    CHECK(back.stamp[fs("a_num")] == sc("s2"));

    std::array<std::uint8_t, kStoryStateBlockSize> again{};
    StoryState copy = s;
    copy.saveSerial = 0;
    REQUIRE(storyEncode(copy, r, again));
    CHECK(again == block);

    // Refusals.
    StoryState untouched;
    CHECK(storyDecode({}, r, untouched) == StoryLoad::Empty);
    CHECK(storyDecode(std::span(block).first(4), r, untouched) == StoryLoad::BadSize);
    CHECK(storyDecode(std::span(block).first(2000), r, untouched) == StoryLoad::BadSize);
    auto bad = block;
    bad[0] = 'X';
    CHECK(storyDecode(bad, r, untouched) == StoryLoad::BadMagic);
    bad = block;
    bad[4] = 2;
    CHECK(storyDecode(bad, r, untouched) == StoryLoad::NewerLayout);
    bad = block;
    bad[4] = 0;
    bad[5] = 0;
    CHECK(storyDecode(bad, r, untouched) == StoryLoad::BadVersion); // checked before the checksum
    fixCrc(bad);
    CHECK(storyDecode(bad, r, untouched) == StoryLoad::BadVersion);
    bad = block;
    bad[100] ^= 1;
    CHECK(storyDecode(bad, r, untouched) == StoryLoad::BadChecksum);
    for (const std::size_t at : {std::size_t{16}, std::size_t{17}}) { // a scene out of range, checksum fixed
        bad = block;
        bad[at] = static_cast<std::uint8_t>(kMaxStoryScenes);
        fixCrc(bad);
        CHECK(storyDecode(bad, r, untouched) == StoryLoad::BadScene);
    }
    CHECK(untouched.facts == StoryState{}.facts);
}

TEST_CASE("story state: a newer writer's slots and records survive") {
    const FactRegistry& r = reg();
    StoryState s;
    REQUIRE(storyBeginScene(s, r, sc("s1")));
    REQUIRE(storyCommitScene(s, r));
    std::array<std::uint8_t, kStoryStateBlockSize> block{};
    REQUIRE(storyEncode(s, r, block));
    // As a newer build: two more facts, one more wide slot, an unknown record, another registry.
    block[8] = static_cast<std::uint8_t>(r.factsKnown + 2);
    block[10] = static_cast<std::uint8_t>(r.wideKnown + 1);
    block[32 + r.factsKnown + 1] = 2;
    block[800 + 4 * r.wideKnown] = 9;
    std::uint8_t* e = block.data() + kStoryExtensionOffset;
    e[0] = 7;
    e[2] = 3;
    e[4] = 'a';
    e[5] = 'b';
    e[6] = 'c';
    block[12] ^= 0x55;
    fixCrc(block);

    StoryState back;
    StoryStateLoadReport rep;
    REQUIRE(storyDecode(block, r, back, &rep) == StoryLoad::Ok);
    CHECK(rep.unknownSlotsPreserved == 3);
    CHECK(rep.unknownTags == 1);
    CHECK(rep.registryMismatch);
    CHECK(storyGet(back, r, static_cast<std::uint16_t>(r.factsKnown + 1)).status == FactStatus::Unknown);
    CHECK_FALSE(hasRule(back, 20)); // the writer knew more slots: newer, not a mismatch to warn on
    std::array<std::uint8_t, kStoryStateBlockSize> out{};
    REQUIRE(storyEncode(back, r, out));
    CHECK(out[32 + r.factsKnown + 1] == 2);
    CHECK(out[800 + 4 * r.wideKnown] == 9);
    CHECK(out[8] == r.factsKnown + 2);
    CHECK(out[kStoryExtensionOffset] == 7);
    CHECK(out[kStoryExtensionOffset + 6] == 'c');

    // A malformed extension is kept byte for byte.
    StoryState broken = back;
    broken.extension.fill(0);
    broken.extension[0] = 9;
    broken.extension[2] = 0xFF;
    broken.extension[3] = 0x0F;
    broken.extensionLength = 4;
    REQUIRE(storyEncode(broken, r, out));
    StoryState back2;
    REQUIRE(storyDecode(out, r, back2, &rep) == StoryLoad::Ok);
    CHECK(rep.extensionMalformed);
    std::array<std::uint8_t, kStoryStateBlockSize> out2{};
    REQUIRE(storyEncode(back2, r, out2));
    CHECK(std::equal(out.begin() + kStoryExtensionOffset, out.end(), out2.begin() + kStoryExtensionOffset));
}

TEST_CASE("story state: no registry: begin, commit, encode, decode") {
    const FactRegistry& r = reg();
    StoryState s;
    for (std::uint8_t w = 0; w < kWideSlots; ++w) s.wide[w] = s.wideEntry[w] = 1000u + w;
    std::array<std::uint8_t, kStoryStateBlockSize> block{};
    REQUIRE(storyEncode(s, r, block));
    // A build with no registry loads it, plays every ordinal, and saves.
    const FactRegistry none;
    StoryState loaded;
    REQUIRE(storyDecode(block, none, loaded) == StoryLoad::Ok);
    REQUIRE(loaded.registryHash == r.hash);
    for (std::uint8_t k = 0; k < kMaxStoryScenes; ++k) {
        CAPTURE(int(k));
        REQUIRE(storyBeginScene(loaded, none, k));
        REQUIRE(storyCommitScene(loaded, none));
    }
    CHECK(loaded.wide == s.wide); // no unregistered wide slot written
    std::array<std::uint8_t, kStoryStateBlockSize> out{};
    REQUIRE(storyEncode(loaded, none, out));
    StoryState back;
    REQUIRE(storyDecode(out, r, back) == StoryLoad::Ok);
    CHECK(back.registryHash == r.hash); // the hash is kept, not zeroed
    CHECK(back.wide == s.wide);
    // A new game with no registry encodes and decodes too.
    StoryState fresh;
    REQUIRE(storyBeginScene(fresh, none, 0));
    REQUIRE(storyCommitScene(fresh, none));
    REQUIRE(storyEncode(fresh, none, out));
    REQUIRE(storyDecode(out, none, back) == StoryLoad::Ok);
    CHECK(back.registryHash == 0);
    CHECK(back.lastCommitted == 0);
}

TEST_CASE("story state: derive on load") {
    const FactRegistry& r = reg();
    StoryState s;
    REQUIRE(storyBeginScene(s, r, sc("s2")));
    REQUIRE(storySet(s, r, fs("a_ch"), 1) == SetResult::Ok);
    REQUIRE(storySet(s, r, fs("a_ev"), 1) == SetResult::Ok);
    REQUIRE(storyCommitScene(s, r));
    REQUIRE(storyBeginScene(s, r, sc("s4")));
    REQUIRE(storyCommitScene(s, r));
    std::array<std::uint8_t, kStoryStateBlockSize> block{};
    REQUIRE(storyEncode(s, r, block));
    {   // This writer knew a_der and left it unset: nothing is derived.
        StoryState back;
        StoryStateLoadReport rep;
        REQUIRE(storyDecode(block, r, back, &rep) == StoryLoad::Ok);
        CHECK(rep.derived == 0);
        CHECK(storyGet(back, r, fs("a_der")).status == FactStatus::Unset);
    }
    // An older writer that knew slots 1..10 only.
    block[8] = 10;
    block[9] = 0;
    fixCrc(block);
    StoryState back;
    StoryStateLoadReport rep;
    REQUIRE(storyDecode(block, r, back, &rep) == StoryLoad::Ok);
    CHECK(rep.derived == 1);
    CHECK(storyGet(back, r, fs("a_der")).value == 1);
}

TEST_CASE("story state: stamps out of range are refused on load") {
    const FactRegistry& r = reg();
    StoryState s;
    REQUIRE(storyBeginScene(s, r, sc("s2")));
    REQUIRE(storySet(s, r, fs("a_ev"), 1) == SetResult::Ok);
    std::array<std::uint8_t, kStoryStateBlockSize> block{};
    REQUIRE(storyEncode(s, r, block)); // mid-scene: the stamps-at-entry record is at the extension's start
    REQUIRE(s.lostStampRecords == 0);
    StoryState back;
    REQUIRE(storyDecode(block, r, back) == StoryLoad::Ok);
    CHECK(back.stamp[fs("a_ev")] == sc("s2"));

    auto bad = block;
    bad[544 + 1] = static_cast<std::uint8_t>(kMaxStoryScenes); // a stamp
    fixCrc(bad);
    CHECK(storyDecode(bad, r, back) == StoryLoad::BadScene);
    bad = block;
    bad[544 + 2] = 200;
    fixCrc(bad);
    CHECK(storyDecode(bad, r, back) == StoryLoad::BadScene);
    bad = block;
    REQUIRE(bad[kStoryExtensionOffset] != 0);
    bad[kStoryExtensionOffset + 4 + 3] = static_cast<std::uint8_t>(kMaxStoryScenes); // a stamp at entry
    fixCrc(bad);
    CHECK(storyDecode(bad, r, back) == StoryLoad::BadScene);
    bad[kStoryExtensionOffset + 4 + 3] = kNoScene;
    fixCrc(bad);
    CHECK(storyDecode(bad, r, back) == StoryLoad::Ok);
}

TEST_CASE("story state: a stamps-at-entry record with no room is counted") {
    const FactRegistry& r = reg();
    StoryState s;
    REQUIRE(storyBeginScene(s, r, sc("s2")));
    REQUIRE(storyCommitScene(s, r));
    REQUIRE(storyBeginScene(s, r, sc("s3")));
    REQUIRE(storySet(s, r, fs("a_num"), 5) == SetResult::Ok);
    // Another build's record fills all but 10 bytes of the extension.
    const std::uint16_t len = static_cast<std::uint16_t>(kStoryExtensionBytes - 10 - 4);
    s.extension[0] = 9;
    s.extension[2] = static_cast<std::uint8_t>(len & 0xFF);
    s.extension[3] = static_cast<std::uint8_t>(len >> 8);
    s.extensionLength = static_cast<std::uint16_t>(4 + len);
    std::array<std::uint8_t, kStoryStateBlockSize> block{};
    REQUIRE(storyEncode(s, r, block));
    CHECK(s.lostStampRecords == 1);
    StoryState back;
    StoryStateLoadReport rep;
    REQUIRE(storyDecode(block, r, back, &rep) == StoryLoad::Ok);
    CHECK(rep.unknownTags == 1);
    CHECK(back.stampEntry == back.stamp); // the record was not written
    // A raw (malformed) extension leaves no room either.
    StoryState raw = s;
    raw.extensionRaw = true;
    REQUIRE(storyEncode(raw, r, block));
    CHECK(raw.lostStampRecords == 2);
    // Between scenes nothing is lost.
    REQUIRE(storyCommitScene(s, r));
    REQUIRE(storyEncode(s, r, block));
    CHECK(s.lostStampRecords == 1);
}

// ---- Validation ----

TEST_CASE("story state: validation rules") {
    const FactRegistry& r = reg();
    const StoryRoles& ro = r.roles;
    CHECK(issues(StoryState{}, r) == 0);
    {   // V01, V02 and V06: the gates.
        StoryState s = committedThrough("s3");
        put(s, fs("a_ev"), 1);
        put(s, fs("a_ch"), 1);
        put(s, fs("a_gated"), 1); // its gate a_ch=B is false
        CHECK(hasRule(s, 1));
        put(s, fs("a_ch"), 2);
        put(s, fs("a_gated"), kNotOffered); // the gate holds, yet not offered
        CHECK(hasRule(s, 2));
        put(s, fs("a_opt"), 2); // option B needs a_ch=C
        CHECK(hasRule(s, 6));
        put(s, fs("a_ch"), 3);
        CHECK_FALSE(hasRule(s, 6));
        StoryState d = committedThrough("s5");
        put(d, fs("a_ch"), 1);
        put(d, fs("a_c4"), 4); // option D needs a_ch=C
        CHECK(hasRule(d, 6)); // one number for any option
        CHECK_FALSE(hasRule(d, 7));
        put(d, fs("a_ch"), 3);
        CHECK_FALSE(hasRule(d, 6));
    }
    {   // V03 and V04: a past scene left a choice or an event at 0.
        const StoryState s = committedThrough("s4");
        CHECK(hasRule(s, 3));
        CHECK(hasRule(s, 4));
    }
    {   // V05: written for a scene not reached.
        StoryState s;
        put(s, fs("a_c4"), 1);
        CHECK(hasRule(s, 5));
    }
    {   // V08: the count below a passed milestone, above a later one, and a milestone's needed fact.
        StoryState s = committedThrough("s5");
        CHECK(hasRule(s, 8));
        putWide(s, ro.milestone, 10);
        CHECK_FALSE(hasRule(s, 8));
        putWide(s, ro.milestone, 20); // past s8's value before s8
        CHECK(hasRule(s, 8));
        StoryState n = committedThrough("s8");
        putWide(n, ro.milestone, 20);
        std::vector<StoryIssue> list;
        issues(n, r, &list);
        const bool needs = std::any_of(list.begin(), list.end(), [&](const StoryIssue& i) {
            return i.rule == 8 && i.factB == fs("a_free");
        });
        CHECK(needs);
        put(n, fs("a_free"), 1);
        list.clear();
        issues(n, r, &list);
        CHECK(std::none_of(list.begin(), list.end(), [](const StoryIssue& i) { return i.rule == 8; }));
    }
    {   // V10 and V15: presses without their open fact or first-press latch, or without a last kind.
        StoryState s;
        putWide(s, ro.pressCounters[1][0], 1);
        CHECK(hasRule(s, 10));
        CHECK(hasRule(s, 15));
        put(s, ro.sideOpen[1], 1);
        put(s, ro.lastKind[1], 1);
        CHECK_FALSE(hasRule(s, 10));
        CHECK_FALSE(hasRule(s, 15));
        put(s, ro.lastKind[1], 2); // a kind never pressed
        CHECK(hasRule(s, 15));
        StoryState z;
        putWide(z, ro.pressCounters[0][2], 1);
        put(z, ro.lastKind[0], 3);
        CHECK(hasRule(z, 10)); // the first press was not latched
        put(z, ro.firstPress, 1);
        CHECK_FALSE(hasRule(z, 10));
    }
    {   // V11: the track mark past the presses or the track.
        StoryState s;
        putWide(s, ro.trackMark, 1);
        CHECK(hasRule(s, 11));
        putWide(s, ro.pressCounters[1][0], 9);
        put(s, ro.sideOpen[1], 1);
        put(s, ro.lastKind[1], 1);
        CHECK_FALSE(hasRule(s, 11));
        putWide(s, ro.trackMark, 5);
        CHECK(hasRule(s, 11));
    }
    {   // V14 and V19: a packed field holding 3 is also out of range.
        StoryState s = committedThrough("s3");
        put(s, fs("a_pk"), 3);
        CHECK(hasRule(s, 14));
        CHECK(hasRule(s, 19));
    }
    {   // V16: a scene counter before its scene.
        StoryState s;
        putWide(s, ws("w_late"), 1);
        CHECK(hasRule(s, 16));
        StoryState t = committedThrough("s9");
        putWide(t, ws("w_late"), 1);
        CHECK_FALSE(hasRule(t, 16));
    }
    {   // V17: facts moved with no scene open.
        StoryState s;
        s.facts[fs("a_free")] = 2;
        CHECK(hasRule(s, 17));
    }
    {   // V18: a stepped value ahead of its set scenes.
        StoryState s = committedThrough("s6");
        put(s, fs("a_step"), 2);
        CHECK(hasRule(s, 18));
        put(s, fs("a_step"), 1);
        CHECK_FALSE(hasRule(s, 18));
        s.lastCommitted = sc("s7");
        put(s, fs("a_step"), 2);
        CHECK_FALSE(hasRule(s, 18));
    }
    {   // V20: another registry of the same size.
        StoryState s;
        s.registryHash = r.hash ^ 1u;
        s.factsKnown = r.factsKnown;
        CHECK(hasRule(s, 20));
    }
    {   // The cap: never more than the span or 32.
        const StoryState s = committedThrough("s12");
        std::array<StoryIssue, 3> few{};
        CHECK(storyValidate(s, r, few) == 3);
        CHECK(issues(s, r) > 3);
    }
}

TEST_CASE("story state: rules bound in data") {
    const FactRegistry& r = reg();
    auto ruleIssue = [&](const StoryState& s, std::uint8_t number, std::uint16_t a, std::uint16_t b) {
        std::vector<StoryIssue> list;
        issues(s, r, &list);
        return std::any_of(list.begin(), list.end(),
                           [&](const StoryIssue& i) { return i.rule == number && i.factA == a && i.factB == b; });
    };
    const std::uint16_t pk = fs("a_pk"), need = fs("a_need"), bits = fs("a_bits"), ev = fs("a_ev");
    {   // Needs, on a packed field: when a_pk.f1 = 2, a_need must be set.
        StoryState s;
        put(s, pk, 8);
        CHECK(ruleIssue(s, 41, pk, need));
        put(s, need, 1);
        CHECK_FALSE(ruleIssue(s, 41, pk, need));
        put(s, need, kNotOffered); // not offered is not set
        CHECK(ruleIssue(s, 41, pk, need));
        put(s, pk, 4); // field 1 = 1: the rule does not apply
        CHECK_FALSE(ruleIssue(s, 41, pk, need));
    }
    {   // Settled: checked only once the when-fact's first set scene is committed.
        StoryState s;
        put(s, need, 2); // a_pk.f1 is not 2
        CHECK_FALSE(ruleIssue(s, 42, need, pk));
        s.lastCommitted = sc("s8");
        CHECK(ruleIssue(s, 42, need, pk));
        put(s, pk, 8);
        CHECK_FALSE(ruleIssue(s, 42, need, pk));
    }
    {   // Any bits, and bits in order.
        StoryState s;
        put(s, bits, 4);
        CHECK(ruleIssue(s, 44, bits, ev));
        CHECK(ruleIssue(s, 43, bits, 0));
        put(s, bits, 7);
        CHECK_FALSE(ruleIssue(s, 43, bits, 0));
        put(s, ev, 1);
        CHECK_FALSE(ruleIssue(s, 44, bits, ev));
        put(s, bits, 2);
        CHECK(ruleIssue(s, 43, bits, 0));
        CHECK_FALSE(ruleIssue(s, 44, bits, ev));
    }
}

TEST_CASE("story state: scene reads and derived values") {
    const FactRegistry& r = reg();
    StoryState s;
    REQUIRE(storyBeginScene(s, r, sc("s2")));
    REQUIRE(storySet(s, r, fs("a_ev"), 1) == SetResult::Ok);
    REQUIRE(storySet(s, r, fs("a_ch"), 3) == SetResult::Ok);
    REQUIRE(storySet(s, r, fs("a_num"), 7) == SetResult::Ok);
    REQUIRE(storyCommitScene(s, r));
    SceneReads reads = storySceneReads(s, r, sc("s3"));
    CHECK(reads.complete);
    REQUIRE(reads.count == 2); // a_ev, and a_first (every scene reads it)
    CHECK(reads.facts[0].fact == fs("a_ev"));
    CHECK(reads.facts[0].status == FactStatus::Set);
    CHECK(reads.facts[1].fact == fs("a_first"));
    CHECK(reads.facts[1].status == FactStatus::Unset);
    REQUIRE(reads.wideCount == 1);
    CHECK(reads.wide[0].wide == ws("w_ctr"));
    const DerivedValues& d = reads.derived;
    CHECK((d.valid & 1u) != 0);
    CHECK(d.value[0] == 1); // a_ch in {B, C}
    CHECK(d.value[1] == 7); // a_num read
    CHECK((d.valid & 4u) == 0); // a_c4 not set: no value
    CHECK((d.valid & 8u) != 0); // an upgrade of an unset fact is 0
    CHECK(d.value[3] == 0);
    CHECK((d.valid & 48u) == 48u);
    CHECK(d.value[5] == 2);
    REQUIRE(storyBeginScene(s, r, sc("s5")));
    REQUIRE(storySet(s, r, fs("a_c4"), 1) == SetResult::Ok);
    reads = storySceneReads(s, r, sc("s6"));
    CHECK(reads.derived.value[2] == 1);
    CHECK(reads.derived.value[3] == 3); // 1 becomes 3 while a_num = 7
}

// ---- No heap while playing ----

TEST_CASE("story state: no heap use while playing") {
    REQUIRE(ftcore_test::allocHookInstalled());
    const FactRegistry& r = reg();
    StoryState s;
    const auto before = ftcore_test::allocStats();
    REQUIRE(storyBeginScene(s, r, sc("s2")));
    REQUIRE(storySet(s, r, fs("a_num"), 4) == SetResult::Ok);
    REQUIRE(storyCountPress(s, r, 0, 0) == SetResult::Ok);
    const SceneReads reads = storySceneReads(s, r, sc("s2"));
    std::array<StoryIssue, kMaxStoryIssues> out{};
    storyValidate(s, r, out);
    REQUIRE(storyRestartScene(s, r));
    REQUIRE(storyCommitScene(s, r));
    std::array<std::uint8_t, kStoryStateBlockSize> block{};
    REQUIRE(storyEncode(s, r, block));
    StoryState back;
    REQUIRE(storyDecode(block, r, back) == StoryLoad::Ok);
    StoryState box;
    storyMakeSandbox(back, r, sc("s2"), box);
    CHECK(ftcore_test::allocStats().count == before.count);
    CHECK(reads.complete);
}

// ---- The fact reader ----

namespace {
// The shape of the fact-reading interface other systems use.
struct FakeFactSource {
    virtual int npcFactIndex(std::string_view id) const = 0;
    virtual bool npcFactValue(int index, std::uint8_t& value) const = 0;

protected:
    ~FakeFactSource() = default;
};
} // namespace

TEST_CASE("story state: the fact reader for other systems") {
    REQUIRE(ftcore_test::allocHookInstalled());
    const FactRegistry& r = reg();
    StoryState s;
    const StoryFactReader<FakeFactSource> reader(s, r);
    const FakeFactSource& src = reader;
    const int choice = src.npcFactIndex("a_ch");
    const int number = src.npcFactIndex("a_num");
    CHECK(choice == static_cast<int>(fs("a_ch")));
    CHECK(src.npcFactIndex("nope") == -1);
    CHECK(src.npcFactIndex("w_mile") == -1); // wide values are not facts
    std::uint8_t v = 99;
    CHECK_FALSE(src.npcFactValue(choice, v)); // unset
    CHECK_FALSE(src.npcFactValue(number, v)); // unset reads its default, yet is not set
    CHECK_FALSE(src.npcFactValue(-1, v));
    CHECK_FALSE(src.npcFactValue(0, v));
    CHECK_FALSE(src.npcFactValue(9999, v));
    CHECK(v == 99);
    REQUIRE(storyBeginScene(s, r, sc("s2")));
    REQUIRE(storySet(s, r, fs("a_num"), 5) == SetResult::Ok);
    REQUIRE(storySet(s, r, fs("a_ch"), 2) == SetResult::Ok);
    CHECK(src.npcFactValue(choice, v));
    CHECK(v == 2);
    CHECK(src.npcFactValue(number, v));
    CHECK(v == 5);
    // Not offered and stale read as false.
    const int gated = src.npcFactIndex("a_gated");
    s.facts[fs("a_gated")] = kNotOffered;
    CHECK_FALSE(src.npcFactValue(gated, v));
    s.facts[fs("a_gated")] = 9;
    CHECK_FALSE(src.npcFactValue(gated, v));
    const auto before = ftcore_test::allocStats();
    for (int i = 0; i < 100; ++i) src.npcFactValue(choice, v);
    CHECK(ftcore_test::allocStats().count == before.count);
}

// ---- The walk: every branch of a scripted playthrough, driven by a walk file ----
//
// part every=N save=M leaves=K   a part is walked from the previous part's ends 1, N+1, 2N+1...; a
//                                mid-scene save every M scenes played; K ends per entry
// scene CODE VARIANTS
// set ID VALUES [if ID=V | ID.fN=V]   a value per variant ("-" skips); with the condition false, refused
// bits ID VALUES                       bits added
// press SIDE KINDS                     a digit per press (0..2)
// wide ID VALUES / derived INDEX VALUES   checked after the commit
// end fact|wide ID VALUE               checked at every end of the part
// One value stands for every variant. '#' starts a comment.

namespace {

struct WLine {
    enum class Op : std::uint8_t { Set, Bits, Press, Wide, Derived } op = Op::Set;
    std::uint16_t slot = 0; // fact slot, wide slot, side, or derived index
    std::vector<std::string> vals;
    FactCond cond;
    bool hasCond = false;
};
struct WScene {
    std::uint8_t ord = kNoScene;
    int variants = 1;
    std::vector<WLine> lines;
};
struct WEnd {
    bool wide = false;
    std::uint16_t slot = 0;
    std::uint32_t value = 0;
};
struct WPart {
    std::uint64_t every = 1, save = 1, leaves = 0;
    std::vector<WScene> scenes;
    std::vector<WEnd> ends;
};

std::vector<std::string> splitWords(std::string_view line) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        std::size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t' && line[j] != '\r') ++j;
        if (j > i) out.emplace_back(line.substr(i, j - i));
        i = j;
    }
    return out;
}

bool toNum(const std::string& t, std::uint64_t& out) {
    if (t.empty() || t.size() > 10) return false;
    out = 0;
    for (char c : t) {
        if (c < '0' || c > '9') return false;
        out = out * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return true;
}

bool keyNum(const std::string& word, const char* key, std::uint64_t& out) {
    const std::string k = std::string(key) + "=";
    return word.rfind(k, 0) == 0 && toNum(word.substr(k.size()), out);
}

bool parseWalk(const FactRegistry& r, std::string_view text, std::vector<WPart>& parts, std::string& err) {
    std::size_t lineNo = 0, at = 0;
    auto fail = [&](const std::string& why) {
        err = "walk line " + std::to_string(lineNo) + ": " + why;
        return false;
    };
    while (at <= text.size()) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(at, nl - at);
        at = nl + 1;
        ++lineNo;
        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        const std::vector<std::string> w = splitWords(line);
        if (w.empty()) continue;
        if (w[0] == "part") {
            WPart p;
            if (w.size() != 4 || !keyNum(w[1], "every", p.every) || !keyNum(w[2], "save", p.save) ||
                !keyNum(w[3], "leaves", p.leaves) || p.every == 0 || p.save == 0)
                return fail("expected part every=N save=M leaves=K");
            parts.push_back(p);
            continue;
        }
        if (parts.empty()) return fail("before any part");
        WPart& part = parts.back();
        if (w[0] == "scene") {
            WScene s;
            std::uint64_t n = 0;
            if (w.size() != 3 || !toNum(w[2], n) || n < 1 || n > 64) return fail("expected scene CODE VARIANTS");
            s.ord = storySceneOrdinal(r, w[1]);
            if (s.ord == kNoScene) return fail("unknown scene " + w[1]);
            s.variants = static_cast<int>(n);
            part.scenes.push_back(s);
            continue;
        }
        if (w[0] == "end") {
            WEnd e;
            std::uint64_t v = 0;
            if (w.size() != 4 || (w[1] != "fact" && w[1] != "wide") || !toNum(w[3], v)) return fail("expected end fact|wide ID VALUE");
            e.wide = w[1] == "wide";
            e.slot = e.wide ? storyWideSlot(r, w[2]) : storyFactSlot(r, w[2]);
            if (e.wide ? e.slot == kNoWide : e.slot == 0) return fail("unknown id " + w[2]);
            e.value = static_cast<std::uint32_t>(v);
            part.ends.push_back(e);
            continue;
        }
        if (part.scenes.empty()) return fail("before any scene");
        WScene& sc = part.scenes.back();
        WLine l;
        std::size_t valsEnd = w.size();
        if (w[0] == "set" || w[0] == "bits") {
            l.op = w[0] == "set" ? WLine::Op::Set : WLine::Op::Bits;
            if (w.size() < 3) return fail("expected " + w[0] + " ID VALUES");
            l.slot = storyFactSlot(r, w[1]);
            if (l.slot == 0) return fail("unknown fact " + w[1]);
            if (w.size() >= 5 && w[w.size() - 2] == "if") {
                if (l.op != WLine::Op::Set) return fail("if on bits");
                const std::string& c = w.back();
                const std::size_t eq = c.find('=');
                if (eq == std::string::npos) return fail("expected if ID=V or ID.fN=V");
                std::string id = c.substr(0, eq);
                std::uint64_t v = 0;
                if (!toNum(c.substr(eq + 1), v) || v > 255) return fail("bad condition value");
                l.cond.value = static_cast<std::uint8_t>(v);
                if (const std::size_t dot = id.find(".f"); dot != std::string::npos) {
                    std::uint64_t f = 0;
                    if (!toNum(id.substr(dot + 2), f) || f > 3) return fail("bad field");
                    l.cond.field = static_cast<std::uint8_t>(f);
                    id = id.substr(0, dot);
                }
                l.cond.fact = storyFactSlot(r, id);
                if (l.cond.fact == 0) return fail("unknown fact " + id);
                l.hasCond = true;
                valsEnd = w.size() - 2;
            }
        } else if (w[0] == "press") {
            l.op = WLine::Op::Press;
            std::uint64_t side = 0;
            if (w.size() < 3 || !toNum(w[1], side) || side > 1) return fail("expected press SIDE KINDS");
            l.slot = static_cast<std::uint16_t>(side);
        } else if (w[0] == "wide") {
            l.op = WLine::Op::Wide;
            if (w.size() < 3) return fail("expected wide ID VALUES");
            const std::uint8_t ws = storyWideSlot(r, w[1]);
            if (ws == kNoWide) return fail("unknown wide " + w[1]);
            l.slot = ws;
        } else if (w[0] == "derived") {
            l.op = WLine::Op::Derived;
            std::uint64_t idx = 0;
            if (w.size() < 3 || !toNum(w[1], idx) || idx >= r.derivedCount) return fail("expected derived INDEX VALUES");
            l.slot = static_cast<std::uint16_t>(idx);
        } else {
            return fail("unknown line " + w[0]);
        }
        l.vals.assign(w.begin() + 2, w.begin() + static_cast<std::ptrdiff_t>(valsEnd));
        if (l.vals.size() != 1 && static_cast<int>(l.vals.size()) != sc.variants)
            return fail("expected 1 or " + std::to_string(sc.variants) + " values");
        for (const std::string& v : l.vals) {
            std::uint64_t n = 0;
            if (v == "-") continue;
            if (l.op == WLine::Op::Press) {
                for (char c : v)
                    if (c < '0' || c > '2') return fail("press kinds are digits 0..2");
            } else if (!toNum(v, n) || ((l.op == WLine::Op::Set || l.op == WLine::Op::Bits) && n > 255)) {
                return fail("bad value " + v);
            }
        }
        sc.lines.push_back(l);
    }
    if (parts.empty()) return fail("no part");
    return true;
}

// The expected stored facts, kept beside the state.
using Model = std::array<std::uint8_t, kFactSlots>;

bool modelCond(const Model& m, const FactCond& c) {
    const std::uint8_t v = m[c.fact];
    if (c.field == 0xFF) return v == c.value;
    return ((v >> (2 * c.field)) & 3u) == c.value;
}

bool choiceKind(FactKind k) { return k == FactKind::Choice2 || k == FactKind::Choice3 || k == FactKind::Choice4; }

const std::string* valueFor(const WLine& l, int variant) {
    const std::string& v = l.vals.size() == 1 ? l.vals[0] : l.vals[static_cast<std::size_t>(variant)];
    return v == "-" ? nullptr : &v;
}

std::uint32_t num(const std::string& v) {
    std::uint64_t n = 0;
    toNum(v, n);
    return static_cast<std::uint32_t>(n);
}

struct Walker {
    const FactRegistry* reg = nullptr;
    std::vector<WPart> parts;
    std::vector<std::uint64_t> leaves, entries;
    std::uint64_t nodes = 0, saves = 0, failures = 0;
    std::string firstFailure;

    void note(const std::string& why) {
        if (!why.empty() && failures++ == 0) firstFailure = why;
    }

    // Plays one variant's lines into the open scene; `why` collects surprises.
    void apply(StoryState& s, Model& m, const WScene& scene, int variant, std::string& why) const {
        const FactRegistry& r = *reg;
        const std::string code(storySceneCode(r, scene.ord));
        for (const WLine& l : scene.lines) {
            const std::string* v = valueFor(l, variant);
            if (!v) continue;
            switch (l.op) {
            case WLine::Op::Set: {
                const std::uint8_t x = static_cast<std::uint8_t>(num(*v));
                const bool offered = !l.hasCond || modelCond(m, l.cond);
                const SetResult res = storySet(s, r, l.slot, x);
                if (res != (offered ? SetResult::Ok : SetResult::RejectedGate))
                    why += " " + code + " set " + std::string(r.facts[l.slot].id()) + "=" + *v + " -> " +
                           std::to_string(int(res));
                if (offered) m[l.slot] = x;
                break;
            }
            case WLine::Op::Bits: {
                const std::uint8_t x = static_cast<std::uint8_t>(num(*v));
                const SetResult res = storySetBits(s, r, l.slot, x);
                if (res != SetResult::Ok) why += " " + code + " bits " + std::string(r.facts[l.slot].id());
                m[l.slot] = static_cast<std::uint8_t>(m[l.slot] | x);
                break;
            }
            case WLine::Op::Press: {
                const std::uint8_t side = static_cast<std::uint8_t>(l.slot);
                for (char c : *v) {
                    const std::uint8_t kind = static_cast<std::uint8_t>(c - '0');
                    if (storyCountPress(s, r, side, kind) != SetResult::Ok) why += " " + code + " press";
                    if (const std::uint16_t lk = r.roles.lastKind[side]; lk != 0)
                        m[lk] = static_cast<std::uint8_t>(kind + 1);
                    if (side == 0 && r.roles.firstPress != 0 && m[r.roles.firstPress] == 0) m[r.roles.firstPress] = 1;
                }
                break;
            }
            case WLine::Op::Wide:
            case WLine::Op::Derived: break;
            }
        }
    }

    // What the commit does to facts the scene left at 0.
    void commitModel(Model& m, std::uint8_t scene) const {
        for (std::uint16_t slot = 1; slot < kFactSlots; ++slot) {
            const FactRow& row = reg->facts[slot];
            if (!row.used || row.setAny || !row.setScenes.has(scene) || m[slot] != 0) continue;
            const bool gateFalse = row.gate.fact != 0 && !modelCond(m, row.gate);
            if (choiceKind(row.kind) && gateFalse) m[slot] = kNotOffered;
            else if (row.kind == FactKind::Optional) m[slot] = gateFalse ? kNotOffered : 1;
        }
    }

    // The scene's reads match the model.
    void checkReads(const StoryState& s, const Model& m, std::uint8_t scene, std::string& why) const {
        const SceneReads reads = storySceneReads(s, *reg, scene);
        const std::string code(storySceneCode(*reg, scene));
        if (!reads.complete) why += " incomplete " + code;
        for (std::uint8_t i = 0; i < reads.count; ++i) {
            const FactRead& f = reads.facts[i];
            const std::uint8_t want = m[f.fact];
            const bool ok = want == 0             ? f.status == FactStatus::Unset
                            : want == kNotOffered ? f.status == FactStatus::NotOffered
                                                  : f.status == FactStatus::Set && f.value == want;
            if (!ok) why += " read " + std::string(reg->facts[f.fact].id()) + " in " + code;
        }
    }

    // After the commit: the wide and derived lines.
    void checkAfter(const StoryState& s, const WScene& scene, int variant, std::string& why) const {
        const std::string code(storySceneCode(*reg, scene.ord));
        DerivedValues d;
        bool haveD = false;
        for (const WLine& l : scene.lines) {
            const std::string* v = valueFor(l, variant);
            if (!v) continue;
            if (l.op == WLine::Op::Wide && storyWide(s, static_cast<std::uint8_t>(l.slot)) != num(*v))
                why += " " + code + " wide " + std::string(reg->wide[l.slot].id()) + " " +
                       std::to_string(storyWide(s, static_cast<std::uint8_t>(l.slot)));
            if (l.op == WLine::Op::Derived) {
                if (!haveD) {
                    d = storySceneReads(s, *reg, 0).derived;
                    haveD = true;
                }
                if (((d.valid >> l.slot) & 1u) == 0 || d.value[l.slot] != num(*v))
                    why += " " + code + " derived " + std::to_string(l.slot) + "=" + std::to_string(d.value[l.slot]);
            }
        }
    }

    // A save in the middle of the scene, loaded back, then the scene played again from its start.
    void saveAndRestart(StoryState& s, const Model& entryModel, const WScene& scene, int variant) {
        const FactRegistry& r = *reg;
        ++saves;
        std::string why;
        std::array<std::uint8_t, kStoryStateBlockSize> blk{};
        StoryState copy = s;
        StoryState back;
        if (!storyEncode(copy, r, blk) || storyDecode(blk, r, back) != StoryLoad::Ok) {
            why += " save";
        } else {
            if (back.openScene != s.openScene || back.facts != s.facts || back.stamp != s.stamp ||
                back.stampEntry != s.stampEntry || back.wide != s.wide || copy.lostStampRecords != 0)
                why += " loaded";
            if (!storyRestartScene(back, r) || back.facts != s.factsEntry || back.stamp != s.stampEntry)
                why += " restart";
            Model m = entryModel;
            apply(back, m, scene, variant, why);
            if (back.facts != s.facts || back.stamp != s.stamp) why += " replay";
            s = back;
        }
        if (!why.empty()) note(std::string(storySceneCode(r, scene.ord)) + why);
    }

    void leaf(std::size_t p, StoryState& s, const Model& m) {
        const FactRegistry& r = *reg;
        const std::uint64_t n = ++leaves[p];
        std::string why;
        std::array<StoryIssue, kMaxStoryIssues> out{};
        if (storyValidate(s, r, out) != 0) why += " issues " + describe(s, r);
        for (std::uint16_t slot = 1; slot <= r.factsKnown; ++slot)
            if (s.facts[slot] != m[slot]) why += " fact " + std::string(r.facts[slot].id());
        for (const WEnd& e : parts[p].ends) {
            const std::uint32_t got = e.wide ? storyWide(s, static_cast<std::uint8_t>(e.slot)) : s.facts[e.slot];
            if (got != e.value)
                why += " end " + std::string(e.wide ? r.wide[e.slot].id() : r.facts[e.slot].id()) + "=" +
                       std::to_string(got);
        }
        // Every eighth end: the block round-trips byte for byte, and every later scene reads the model.
        if ((n & 7u) == 1) {
            std::array<std::uint8_t, kStoryStateBlockSize> a{}, b{};
            StoryState copy = s;
            storyEncode(copy, r, a);
            StoryState back;
            if (storyDecode(a, r, back) != StoryLoad::Ok) why += " decode";
            back.saveSerial = s.saveSerial;
            storyEncode(back, r, b);
            if (a != b) why += " bytes";
            for (std::uint8_t k = static_cast<std::uint8_t>(s.lastCommitted + 1u); k < r.sceneCount; ++k)
                checkReads(s, m, k, why);
        }
        if (!why.empty()) note("part " + std::to_string(p + 1) + " end " + std::to_string(n) + ":" + why);
        if (p + 1 < parts.size() && (n - 1) % parts[p + 1].every == 0) {
            ++entries[p + 1];
            play(p + 1, 0, s, m);
        }
    }

    void play(std::size_t p, std::size_t i, const StoryState& s, const Model& m) {
        const WPart& part = parts[p];
        if (i == part.scenes.size()) {
            StoryState copy = s;
            leaf(p, copy, m);
            return;
        }
        const WScene& scene = part.scenes[i];
        for (int v = 0; v < scene.variants; ++v) {
            StoryState n = s;
            Model nm = m;
            std::string why;
            if (!storyBeginScene(n, *reg, scene.ord)) why += " begin";
            checkReads(n, nm, scene.ord, why);
            apply(n, nm, scene, v, why);
            if (++nodes % part.save == 0) saveAndRestart(n, m, scene, v);
            if (!storyCommitScene(n, *reg)) why += " commit";
            commitModel(nm, scene.ord);
            checkAfter(n, scene, v, why);
            std::array<StoryEvent, kStoryEventRing> ev{};
            storyDrainEvents(n, ev);
            if (!why.empty()) note(std::string(storySceneCode(*reg, scene.ord)) + " v" + std::to_string(v) + ":" + why);
            play(p, i + 1, n, nm);
        }
    }
};

// Walks every branch; then a restart in every scene must give the same end as a straight run.
void runWalk(const FactRegistry& r, std::string_view text, const char* what) {
    Walker w;
    w.reg = &r;
    std::string err;
    REQUIRE_MESSAGE(parseWalk(r, text, w.parts, err), err);
    w.leaves.assign(w.parts.size(), 0);
    w.entries.assign(w.parts.size(), 0);
    w.entries[0] = 1;
    const auto t0 = std::chrono::steady_clock::now();
    w.play(0, 0, StoryState{}, Model{});
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::string counts;
    for (std::size_t p = 0; p < w.parts.size(); ++p) counts += " " + std::to_string(w.leaves[p]);
    MESSAGE(std::string(what) << ": ends per part" << counts << ", mid-scene saves " << w.saves << ", " << secs << " s");
    for (std::size_t p = 0; p < w.parts.size(); ++p) {
        CAPTURE(p);
        CHECK(w.leaves[p] == w.entries[p] * w.parts[p].leaves);
        CHECK(w.entries[p] > 0);
    }
    CHECK(w.saves > 0);
    CAPTURE(w.firstFailure);
    CHECK(w.failures == 0);

    // A restart in each scene: the facts end the same; retries count them, presses stay counted.
    std::vector<const WScene*> all;
    for (const WPart& p : w.parts)
        for (const WScene& s : p.scenes) all.push_back(&s);
    StoryState straight, retried;
    Model ms{}, mr{};
    std::string why;
    for (const WScene* scene : all) {
        REQUIRE(storyBeginScene(straight, r, scene->ord));
        w.apply(straight, ms, *scene, scene->variants - 1, why);
        REQUIRE(storyCommitScene(straight, r));
        w.commitModel(ms, scene->ord);
        REQUIRE(storyBeginScene(retried, r, scene->ord));
        Model scratch = mr;
        std::string ignored;
        w.apply(retried, scratch, *scene, 0, ignored);
        REQUIRE(storyRestartScene(retried, r));
        w.apply(retried, mr, *scene, scene->variants - 1, why);
        REQUIRE(storyCommitScene(retried, r));
        w.commitModel(mr, scene->ord);
    }
    CAPTURE(why);
    CHECK(why.empty());
    CHECK(retried.facts == straight.facts);
    CHECK(retried.facts == mr);
    const StoryRoles& ro = r.roles;
    if (ro.retries != kNoWide) CHECK(storyWide(retried, ro.retries) == all.size());
    if (ro.pressTotal != kNoWide) CHECK(storyWide(retried, ro.pressTotal) > storyWide(straight, ro.pressTotal));
    if (ro.trackMark != kNoWide) CHECK(storyWide(retried, ro.trackMark) == storyWide(straight, ro.trackMark));
    CHECK(issues(retried, r) == 0);
}

const char* kWalk = R"(# A walk over the small registry.
part every=1 save=5 leaves=48
scene s1 1
scene s2 3
set a_ev 1
set a_ch 1 2 3
set a_num 5
bits a_bits 1
set a_pk 1
press 0 - 0 12
derived 0 0 1 1
derived 4 0 0 2
derived 5 2 2 3
scene s3 2
set a_gated 1 2 if a_ch=2
set a_op - 2 if a_ch=1
set a_opt 1
set a_num 7
bits a_bits 2
set a_pk 2 10
scene s4 1
set a_der 1
scene s5 2
set a_c4 1 2
set a_step 1
set a_open1 1
derived 2 1 2
derived 3 3 2
wide w_mile 10
scene s6 2
press 1 0 01201
wide w_mark 1 4
scene s7 1
set a_step 2
scene s8 2
set a_need - 2 if a_pk.f1=2
set a_free 2
wide w_mile 20
end wide w_mile 20
end fact a_bits 3

part every=5 save=3 leaves=2
scene s9 1
set a_step 3
scene s10 1
scene s11 2
set a_late 1 2 if a_c4=2
scene s12 1
end fact a_step 3
end wide w_mile 20
)";

} // namespace

TEST_CASE("story state: the walk parser refuses bad lines") {
    const FactRegistry& r = reg();
    auto fails = [&](const char* text, const char* expect) {
        std::vector<WPart> parts;
        std::string err;
        CAPTURE(text);
        CHECK_FALSE(parseWalk(r, text, parts, err));
        CAPTURE(err);
        CHECK(err.find(expect) != std::string::npos);
    };
    fails("", "no part");
    fails("scene s1 1\n", "before any part");
    fails("part every=0 save=1 leaves=1\n", "expected part");
    fails("part every=1 save=1 leaves=1\nscene zz 1\n", "unknown scene zz");
    fails("part every=1 save=1 leaves=1\nset a_ev 1\n", "before any scene");
    fails("part every=1 save=1 leaves=1\nscene s2 2\nset a_ch 1 2 3\n", "expected 1 or 2 values");
    fails("part every=1 save=1 leaves=1\nscene s2 1\nset nope 1\n", "unknown fact nope");
    fails("part every=1 save=1 leaves=1\nscene s2 1\npress 0 3\n", "digits 0..2");
    fails("part every=1 save=1 leaves=1\nscene s2 1\nderived 9 1\n", "expected derived");
    fails("part every=1 save=1 leaves=1\nscene s2 1\nset a_ch 1 if a_ev\n", "expected if ID=V");
    fails("part every=1 save=1 leaves=1\nscene s2 1\nhop 1\n", "unknown line hop");
}

TEST_CASE("story state: every branch of the small walk") { runWalk(reg(), kWalk, "small walk"); }

TEST_CASE("story state: the walk catches a wrong expectation") {
    // The walker is only worth its passes if it fails: each changed line must be caught.
    const std::string base = kWalk;
    const std::pair<const char*, const char*> breaks[] = {
        {"derived 0 0 1 1", "derived 0 1 1 1"},
        {"wide w_mark 1 4", "wide w_mark 1 5"},
        {"set a_gated 1 2 if a_ch=2", "set a_gated 1 2 if a_ch=1"},
        {"end fact a_bits 3", "end fact a_bits 1"},
        {"part every=1 save=5 leaves=48", "part every=1 save=5 leaves=47"},
        {"set a_need - 2 if a_pk.f1=2", "set a_need - 2"},
    };
    for (const auto& [from, to] : breaks) {
        CAPTURE(to);
        std::string text = base;
        const std::size_t at = text.find(from);
        REQUIRE(at != std::string::npos);
        text.replace(at, std::string(from).size(), to);
        Walker w;
        w.reg = &reg();
        std::string err;
        REQUIRE(parseWalk(reg(), text, w.parts, err));
        w.leaves.assign(w.parts.size(), 0);
        w.entries.assign(w.parts.size(), 0);
        w.entries[0] = 1;
        w.play(0, 0, StoryState{}, Model{});
        bool counted = true;
        for (std::size_t p = 0; p < w.parts.size(); ++p) counted = counted && w.leaves[p] == w.entries[p] * w.parts[p].leaves;
        CHECK((w.failures > 0 || !counted));
    }
}

// ---- With the game's registry (Data/Story, outside the public repo): extra cases ----

// Skipped (and counted as skipped) when Data/Story is absent.
TEST_CASE("story state: the game's registry" * doctest::skip(storyDataAbsent())) {
    const PrivateRegistry& p = privateRegistry();
    CAPTURE(p.error);
    REQUIRE(p.ok);
    const FactRegistry& r = p.reg;
    CHECK(r.hash != 0);
    for (std::uint8_t k = 0; k < r.sceneCount; ++k) CHECK(storySceneReads(StoryState{}, r, k).complete);
    CHECK(issues(StoryState{}, r) == 0);
    StoryState s;
    REQUIRE(storyBeginScene(s, r, 0));
    REQUIRE(storyCommitScene(s, r));
    std::array<std::uint8_t, kStoryStateBlockSize> block{};
    REQUIRE(storyEncode(s, r, block));
    StoryState back;
    REQUIRE(storyDecode(block, r, back) == StoryLoad::Ok);
    CHECK(back.facts == s.facts);
}

// Skipped when the walk file is absent; a walk file without the registry fails.
TEST_CASE("story state: every branch of the game's walk" * doctest::skip(walkAbsent())) {
    const PrivateRegistry& p = privateRegistry();
    CAPTURE(p.error);
    REQUIRE(p.ok);
    std::string walk;
    REQUIRE(ftcore_test::readProjectData("Story/story_walk.txt", walk));
    runWalk(p.reg, walk, "game walk");
}

TEST_CASE("story state: load or new") {
    const FactRegistry& r = reg();
    StoryState s;
    REQUIRE(storyBeginScene(s, r, sc("s2")));
    REQUIRE(storySet(s, r, fs("a_num"), 6) == SetResult::Ok);
    REQUIRE(storyCommitScene(s, r));
    std::array<std::uint8_t, kStoryStateBlockSize> block{};
    REQUIRE(storyEncode(s, r, block));

    StoryState played; // a state with progress in it, overwritten by every load
    REQUIRE(storyBeginScene(played, r, sc("s3")));
    REQUIRE(storySet(played, r, fs("a_num"), 9) == SetResult::Ok);
    const StoryState fresh;

    StoryState out = played;
    CHECK(storyLoadOrNew(block, r, out) == StoryLoad::Ok);
    CHECK(out.facts == s.facts);
    CHECK(out.lastCommitted == sc("s2"));
    CHECK(out.openScene == kNoScene);

    out = played;
    CHECK(storyLoadOrNew({}, r, out) == StoryLoad::Empty); // a save from before the block
    CHECK(out.facts == fresh.facts);
    CHECK(out.stamp == fresh.stamp);
    CHECK(out.openScene == fresh.openScene);
    CHECK(out.lastCommitted == fresh.lastCommitted);

    auto bad = block;
    bad[600] ^= 1;
    out = played;
    CHECK(storyLoadOrNew(bad, r, out) == StoryLoad::BadChecksum);
    CHECK(out.facts == fresh.facts);
    CHECK(out.openScene == fresh.openScene);

    CHECK(std::string(storyLoadName(StoryLoad::Ok)) == "ok");
    CHECK(std::string(storyLoadName(StoryLoad::BadChecksum)) == "bad checksum");
    CHECK(std::string(storyLoadName(StoryLoad::BadVersion)) == "bad version");
}
