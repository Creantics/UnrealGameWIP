// The dialogue loop (CHC / HE screen). Every line here is invented fixture text; real loops are
// story text and stay out of the repo.

#include "ftcore/DialogueLoop.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace ftcore;

namespace {

// One round: a claim, a red (right) option and up to three wrong ones, with concede and companion
// lines; a two-line tag and one flag.
std::string lampLoop(int wrong, bool resolve = true, int rounds = 1) {
    // Wrong options first (Blue, Green, Grey), then Red.
    std::string opts;
    const char* colours[3] = {"Blue", "Green", "Grey"};
    for (int i = 0; i < wrong; ++i)
        opts += std::string(R"({ "text": ")") + colours[i] + R"(.", "right": false, "reply": "Not )" + colours[i] + R"(." }, )";
    opts += R"({ "text": "Red.", "right": true, "reply": "Red, yes." })";
    std::string round = R"({ "claim": "Which lamp is lit first?", "options": [ )" + opts + " ]";
    if (resolve) round += R"(, "concede": "Think of the door.", "companion": { "speaker": "companion", "text": "It's red." })";
    round += " }";
    std::string all = round;
    for (int r = 1; r < rounds; ++r) all += ", " + round;
    std::string json = R"({ "version": 1, "scene": "EX-01", "beat": 2, "step": 1, "resolve": )" +
                       std::string(resolve ? "true" : "false") + R"(, "rounds": [ )" + all + " ]";
    if (resolve) json += R"(, "tag": [ "The hall is ready.", { "speaker": "companion", "text": "Told you." } ])";
    json += R"(, "flags": [ "lamp_done" ] })";
    return json;
}

DialogueLoopDef load(const std::string& json, std::vector<std::string>* warnings = nullptr) {
    DialogueLoopDef def;
    std::string error;
    const bool ok = importDialogueLoop(json, def, &error, warnings);
    INFO(error);
    REQUIRE(ok);
    return def;
}

bool refuses(const std::string& json, const char* needle) {
    DialogueLoopDef def;
    std::string error;
    if (importDialogueLoop(json, def, &error)) return false;
    INFO(error);
    CHECK(def.roundCount == 0);
    return error.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("loop file name keys scene, beat and step") {
    CHECK(dialogueLoopFileName("SC-X-01", 4, 0) == "SC-X-01_4_0.json");
}

TEST_CASE("the committed example loads") {
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Choice/example_loop.json", text));
    std::vector<std::string> warnings;
    const DialogueLoopDef def = load(text, &warnings);
    CHECK(warnings.empty());
    CHECK(def.matches("EX-01", 3, 0));
    CHECK_FALSE(def.matches("EX-01", 3, 1));
    CHECK(def.resolve);
    CHECK(def.roundCount == 2);
    CHECK(def.rounds[0].optionCount == 4);
    CHECK(def.rounds[0].wrongCount() == 3);
    CHECK(def.rounds[1].wrongCount() == 0);
    CHECK(def.rounds[0].claim.speaker == "host");
    CHECK(def.rounds[0].companion.speaker == "companion");
    CHECK(def.tagCount == 2);
    CHECK(def.tag[0].speaker == "host");
    CHECK(def.tag[1].speaker == "companion");
    REQUIRE(def.flagCount == 2);
    CHECK(def.flags[0].id == "example_loop_done");
    CHECK(def.flags[0].value == "true");
    CHECK(def.flags[1].value == "2");
    CHECK(def.worstCasePicks() == 4);
}

TEST_CASE("claim, audit, right pick, reply, tag, done") {
    const DialogueLoopDef def = load(lampLoop(1));
    DialogueLoop loop;
    CHECK(loop.state() == DialogueLoopState::Idle);
    loop.begin(&def);
    CHECK(loop.state() == DialogueLoopState::Claim);
    CHECK(loop.speaker() == "host");
    CHECK(loop.text() == "Which lamp is lit first?");
    CHECK_FALSE(loop.optionsVisible());
    CHECK_FALSE(loop.choose(0)); // not before the claim is read
    REQUIRE(loop.press());
    CHECK(loop.state() == DialogueLoopState::Audit);
    CHECK(loop.optionsVisible());
    CHECK(loop.text() == "Which lamp is lit first?"); // the claim stays up
    CHECK(loop.optionCount() == 2);
    CHECK(loop.optionText(1) == "Red.");
    CHECK_FALSE(loop.press()); // a pick is needed
    CHECK_FALSE(loop.choose(2));
    REQUIRE(loop.choose(1));
    CHECK(loop.state() == DialogueLoopState::Reply);
    CHECK(loop.text() == "Red, yes.");
    REQUIRE(loop.press());
    CHECK(loop.state() == DialogueLoopState::Tag);
    CHECK(loop.text() == "The hall is ready.");
    REQUIRE(loop.press());
    CHECK(loop.tagLine() == 1);
    CHECK(loop.speaker() == "companion");
    CHECK_FALSE(loop.finished());
    REQUIRE(loop.press());
    CHECK(loop.finished());
    CHECK(loop.text().empty());
    CHECK(loop.picks() == 1);
    CHECK_FALSE(loop.skipped());
    REQUIRE(loop.flagCount() == 1);
    CHECK(loop.flagId(0) == "lamp_done");
    CHECK(loop.flagValue(0) == "true");
    CHECK_FALSE(loop.press());
    CHECK_FALSE(loop.skip());
}

TEST_CASE("a wrong pick escalates with its own line and greys out") {
    const DialogueLoopDef def = load(lampLoop(2));
    DialogueLoop loop;
    loop.begin(&def);
    loop.press();
    REQUIRE(loop.choose(0)); // Blue
    CHECK(loop.state() == DialogueLoopState::Escalate);
    CHECK(loop.speaker() == "host");
    CHECK(loop.text() == "Not Blue.");
    CHECK(loop.misses() == 1);
    REQUIRE(loop.press());
    CHECK(loop.state() == DialogueLoopState::Audit);
    CHECK(loop.optionGreyed(0));
    CHECK_FALSE(loop.optionSelectable(0));
    CHECK_FALSE(loop.choose(0)); // used: can't be picked again
    CHECK(loop.picks() == 1);
    CHECK(loop.optionSelectable(1));
}

TEST_CASE("the second miss concedes with a new line; the third has the companion resolve") {
    const DialogueLoopDef def = load(lampLoop(3));
    DialogueLoop loop;
    loop.begin(&def);
    loop.press();
    REQUIRE(loop.choose(0));
    CHECK(loop.state() == DialogueLoopState::Escalate);
    loop.press();
    REQUIRE(loop.choose(1));
    CHECK(loop.state() == DialogueLoopState::Concede);
    CHECK(loop.speaker() == "host");
    CHECK(loop.text() == "Think of the door."); // not Green's own correction
    loop.press();
    CHECK(loop.state() == DialogueLoopState::Audit);
    REQUIRE(loop.choose(2));
    CHECK(loop.state() == DialogueLoopState::Companion);
    CHECK(loop.speaker() == "companion");
    CHECK(loop.text() == "It's red.");
    CHECK(loop.companionResolves() == 1);
    loop.press();
    CHECK(loop.state() == DialogueLoopState::Tag); // resolved
    CHECK(loop.picks() == 3);
}

TEST_CASE("several rounds, each with its own claim") {
    const DialogueLoopDef def = load(lampLoop(1, true, 2));
    DialogueLoop loop;
    loop.begin(&def);
    CHECK(loop.roundCount() == 2);
    loop.press();
    loop.choose(0); // miss
    loop.press();
    CHECK(loop.optionGreyed(0));
    loop.choose(1); // right
    loop.press();
    CHECK(loop.state() == DialogueLoopState::Claim);
    CHECK(loop.round() == 1);
    CHECK(loop.misses() == 0);
    loop.press();
    CHECK_FALSE(loop.optionGreyed(0)); // a fresh round
    loop.choose(1);
    loop.press();
    CHECK(loop.state() == DialogueLoopState::Tag);
    CHECK(loop.picks() == 3);
}

TEST_CASE("at most five picks across the loop") {
    // Three rounds of three wrong options: up to 9 picks without the cap.
    std::vector<std::string> warnings;
    const DialogueLoopDef def = load(lampLoop(3, true, 3), &warnings);
    CHECK(def.worstCasePicks() == 9);
    CHECK(warnings.size() == 1); // the worst case is past the cap
    DialogueLoop loop;
    loop.begin(&def);
    // Round 1: three misses (the companion resolves).
    loop.press();
    for (std::uint32_t i = 0; i < 3; ++i) {
        REQUIRE(loop.choose(i));
        loop.press();
    }
    CHECK(loop.round() == 1);
    CHECK(loop.picks() == 3);
    // Round 2: miss, then the fifth pick misses too: the companion resolves and the loop ends.
    loop.press();
    REQUIRE(loop.choose(0));
    CHECK(loop.state() == DialogueLoopState::Escalate);
    loop.press();
    REQUIRE(loop.choose(1));
    CHECK(loop.picks() == 5);
    CHECK(loop.state() == DialogueLoopState::Companion);
    loop.press();
    CHECK(loop.state() == DialogueLoopState::Tag); // round 3 never opens
    CHECK(loop.picks() == 5);
}

TEST_CASE("the pick cap ends a no-resolve loop after the correction") {
    const DialogueLoopDef def = load(lampLoop(3, false, 2));
    DialogueLoop loop;
    loop.begin(&def);
    loop.press();
    for (std::uint32_t i = 0; i < 3; ++i) {
        REQUIRE(loop.choose(i));
        CHECK(loop.state() == DialogueLoopState::Escalate); // no concede, no companion
        loop.press();
    }
    REQUIRE(loop.choose(3)); // right
    loop.press();
    CHECK(loop.round() == 1);
    loop.press();
    REQUIRE(loop.choose(0)); // the fifth pick
    CHECK(loop.picks() == 5);
    CHECK(loop.state() == DialogueLoopState::Escalate);
    CHECK_FALSE(loop.optionSelectable(1));
    loop.press();
    CHECK(loop.finished());
}

TEST_CASE("skip goes straight to the tag at any point") {
    const DialogueLoopDef def = load(lampLoop(3, true, 2));
    for (int at = 0; at < 4; ++at) {
        DialogueLoop loop;
        loop.begin(&def);
        if (at >= 1) loop.press();    // audit
        if (at >= 2) loop.choose(0);  // escalate
        if (at >= 3) loop.press();    // audit again
        REQUIRE(loop.skip());
        CHECK(loop.skipped());
        CHECK(loop.state() == DialogueLoopState::Tag);
        CHECK(loop.tagLine() == 0);
        CHECK_FALSE(loop.skip()); // already there
        loop.press();
        loop.press();
        CHECK(loop.finished());
        CHECK(loop.flagCount() == 1);
    }
}

TEST_CASE("a no-resolve loop exits on skip or at the end, flags due") {
    std::vector<std::string> warnings;
    const DialogueLoopDef def = load(lampLoop(1, false), &warnings);
    CHECK_FALSE(def.resolve);
    CHECK(def.tagCount == 0);
    DialogueLoop loop;
    loop.begin(&def);
    loop.press();
    loop.choose(1);
    loop.press(); // the reply
    CHECK(loop.finished());
    CHECK(loop.flagId(0) == "lamp_done");

    loop.begin(&def);
    CHECK(loop.state() == DialogueLoopState::Claim);
    REQUIRE(loop.skip());
    CHECK(loop.finished());
    CHECK(loop.skipped());
}

TEST_CASE("no-resolve loops warn about ignored concede, companion and tag") {
    const std::string json = R"({ "version": 1, "scene": "EX-02", "beat": 0, "step": 0, "resolve": false,
        "rounds": [ { "claim": "Which bell rings at noon?", "concede": "x", "companion": "y",
                      "options": [ { "text": "The small one.", "right": true }, { "text": "The big one.", "right": true } ] } ],
        "tag": [ "z" ], "flags": [ "bell_done" ] })";
    std::vector<std::string> warnings;
    load(json, &warnings);
    CHECK(warnings.size() == 3);
}

TEST_CASE("a round where every pick is right") {
    const std::string json = R"({ "version": 1, "scene": "EX-03", "beat": 1, "step": 0,
        "rounds": [ { "claim": "Where shall we sit?",
                      "options": [ { "text": "By the window.", "right": true, "reply": "Good light there." },
                                   { "text": "Near the stove.", "right": true } ] } ],
        "tag": [ "Then sit." ], "flags": [ { "id": "seat_mood", "value": "warm" }, { "id": "seat_set", "value": false } ] })";
    const DialogueLoopDef def = load(json);
    CHECK(def.flags[0].value == "warm");
    CHECK(def.flags[1].value == "false");
    DialogueLoop loop;
    loop.begin(&def);
    loop.press();
    REQUIRE(loop.choose(1)); // no reply: straight on
    CHECK(loop.state() == DialogueLoopState::Tag);
    loop.begin(&def);
    loop.press();
    REQUIRE(loop.choose(0));
    CHECK(loop.state() == DialogueLoopState::Reply);
}

TEST_CASE("autoplay takes the skip path and finishes") {
    const DialogueLoopDef def = load(lampLoop(2, true, 2));
    DialogueLoop loop;
    loop.begin(&def);
    CHECK(loop.autoplay().kind == DialogueLoopCommand::Kind::Skip);
    int moves = 0;
    while (!loop.finished() && moves < 20) {
        REQUIRE(loop.apply(loop.autoplay()));
        ++moves;
    }
    CHECK(loop.finished());
    CHECK(moves == 3); // skip, then two tag lines
    CHECK(loop.skipped());
    CHECK(loop.picks() == 0);
    CHECK(loop.autoplay().none());
    CHECK_FALSE(loop.apply(loop.autoplay()));
}

TEST_CASE("an idle loop does nothing") {
    DialogueLoop loop;
    loop.begin(nullptr);
    CHECK(loop.state() == DialogueLoopState::Idle);
    CHECK_FALSE(loop.press());
    CHECK_FALSE(loop.choose(0));
    CHECK_FALSE(loop.skip());
    CHECK(loop.autoplay().none());
    CHECK(loop.text().empty());
    CHECK(loop.optionCount() == 0);
    CHECK(loop.flagCount() == 0);
    CHECK(dialogueLoopStateName(DialogueLoopState::Companion) == "companion");
}

TEST_CASE("the epoch moves with every change") {
    const DialogueLoopDef def = load(lampLoop(1));
    DialogueLoop loop;
    loop.begin(&def);
    std::uint32_t e = loop.epoch();
    loop.press();
    CHECK(loop.epoch() != e);
    e = loop.epoch();
    CHECK_FALSE(loop.choose(5));
    CHECK(loop.epoch() == e);
    loop.choose(1);
    loop.press(); // tag line 0
    e = loop.epoch();
    loop.press(); // tag line 1
    CHECK(loop.epoch() != e);
}

TEST_CASE("playing a loop allocates nothing") {
    const DialogueLoopDef def = load(lampLoop(3, true, 2));
    DialogueLoop loop;
    const auto before = ftcore_test::allocStats().count;
    std::size_t seen = 0;
    for (int pass = 0; pass < 2; ++pass) {
        loop.begin(&def);
        while (!loop.finished()) {
            seen += loop.text().size() + loop.speaker().size();
            if (loop.optionsVisible()) {
                std::uint32_t i = 0;
                while (!loop.optionSelectable(i)) ++i;
                seen += loop.optionText(i).size();
                loop.choose(i);
            } else {
                loop.press();
            }
        }
        for (std::uint32_t f = 0; f < loop.flagCount(); ++f) seen += loop.flagId(f).size();
    }
    const auto allocs = ftcore_test::allocStats().count - before;
    if (ftcore_test::allocHookInstalled()) CHECK(allocs == 0);
    CHECK(seen > 0);
}

TEST_CASE("strict validation") {
    const std::string ok = lampLoop(1);
    CHECK(refuses("[1]", "top-level object"));
    CHECK(refuses("{", "bad JSON"));
    std::string s = ok;
    s.replace(s.find("\"version\": 1"), 12, "\"version\": 2");
    CHECK(refuses(s, "version"));
    s = ok;
    s.replace(s.find("\"scene\": \"EX-01\""), 16, "\"scene\": \"\"");
    CHECK(refuses(s, "scene"));
    s = ok;
    s.replace(s.find("\"beat\": 2"), 9, "\"beat\": -1");
    CHECK(refuses(s, "beat"));

    // Option counts.
    CHECK(refuses(R"({ "version": 1, "scene": "E", "beat": 0, "step": 0,
        "rounds": [ { "claim": "Q?", "options": [ { "text": "A.", "right": true } ] } ], "tag": [ "T." ] })",
                  "2 to 4 options"));
    CHECK(refuses(R"({ "version": 1, "scene": "E", "beat": 0, "step": 0,
        "rounds": [ { "claim": "Q?", "options": [ {"text":"A","right":true},{"text":"B","right":true},
          {"text":"C","right":true},{"text":"D","right":true},{"text":"E","right":true} ] } ], "tag": [ "T." ] })",
                  "2 to 4 options"));
    // No right option.
    CHECK(refuses(R"({ "version": 1, "scene": "E", "beat": 0, "step": 0,
        "rounds": [ { "claim": "Q?", "options": [ {"text":"A","right":false,"reply":"x"},{"text":"B","right":false,"reply":"y"} ],
          "concede": "c" } ], "tag": [ "T." ] })",
                  "no right option"));
    // A wrong option needs its correction.
    CHECK(refuses(R"({ "version": 1, "scene": "E", "beat": 0, "step": 0,
        "rounds": [ { "claim": "Q?", "options": [ {"text":"A","right":true},{"text":"B","right":false} ] } ], "tag": [ "T." ] })",
                  "correction"));
    // Missing `right`.
    CHECK(refuses(R"({ "version": 1, "scene": "E", "beat": 0, "step": 0,
        "rounds": [ { "claim": "Q?", "options": [ {"text":"A","right":true},{"text":"B"} ] } ], "tag": [ "T." ] })",
                  "right"));
    // Missing claim.
    CHECK(refuses(R"({ "version": 1, "scene": "E", "beat": 0, "step": 0,
        "rounds": [ { "options": [ {"text":"A","right":true},{"text":"B","right":true} ] } ], "tag": [ "T." ] })",
                  "claim"));
    // Concede and companion required by the wrong count.
    CHECK(refuses(R"({ "version": 1, "scene": "E", "beat": 0, "step": 0,
        "rounds": [ { "claim": "Q?", "options": [ {"text":"A","right":true},{"text":"B","right":false,"reply":"x"},
          {"text":"C","right":false,"reply":"y"} ] } ], "tag": [ "T." ] })",
                  "concede"));
    CHECK(refuses(R"({ "version": 1, "scene": "E", "beat": 0, "step": 0,
        "rounds": [ { "claim": "Q?", "options": [ {"text":"A","right":true},{"text":"B","right":false,"reply":"x"},
          {"text":"C","right":false,"reply":"y"},{"text":"D","right":false,"reply":"z"} ], "concede": "c" } ], "tag": [ "T." ] })",
                  "companion"));
    // A resolving loop needs a tag.
    CHECK(refuses(R"({ "version": 1, "scene": "E", "beat": 0, "step": 0,
        "rounds": [ { "claim": "Q?", "options": [ {"text":"A","right":true},{"text":"B","right":true} ] } ] })",
                  "tag"));
    // Rounds: none, or too many.
    CHECK(refuses(R"({ "version": 1, "scene": "E", "beat": 0, "step": 0, "rounds": [], "tag": [ "T." ] })", "rounds"));
    {
        const std::string r = R"({ "claim": "Q?", "options": [ {"text":"A","right":true},{"text":"B","right":true} ] })";
        std::string many = R"({ "version": 1, "scene": "E", "beat": 0, "step": 0, "tag": [ "T." ], "rounds": [ )";
        for (int i = 0; i < 6; ++i) many += (i ? ", " : "") + r;
        many += " ] }";
        CHECK(refuses(many, "1 to 5 rounds"));
    }
    // Malformed line and flag.
    CHECK(refuses(R"({ "version": 1, "scene": "E", "beat": 0, "step": 0,
        "rounds": [ { "claim": { "speaker": "host" }, "options": [ {"text":"A","right":true},{"text":"B","right":true} ] } ], "tag": [ "T." ] })",
                  "claim.text"));
    CHECK(refuses(R"({ "version": 1, "scene": "E", "beat": 0, "step": 0,
        "rounds": [ { "claim": "Q?", "options": [ {"text":"A","right":true},{"text":"B","right":true} ] } ], "tag": [ "T." ],
        "flags": [ { "value": 1 } ] })",
                  "flags[0].id"));
}

TEST_CASE("errors and warnings name paths, never line text") {
    std::vector<std::string> warnings;
    DialogueLoopDef def;
    std::string error;
    CHECK_FALSE(importDialogueLoop(R"({ "version": 1, "scene": "E", "beat": 0, "step": 0, "mystery": 1,
        "rounds": [ { "claim": "Secret question?", "options": [ {"text":"Secret answer","right":true},
          {"text":"Hidden wrong","right":false} ] } ] })",
                                   def, &error, &warnings));
    CHECK(error.find("Secret") == std::string::npos);
    CHECK(error.find("Hidden") == std::string::npos);
    REQUIRE(warnings.size() == 2); // the unknown key, no flags
    CHECK(warnings[0].find("mystery") != std::string::npos);
}
