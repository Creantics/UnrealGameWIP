// ftcore_storywalk: plays a story through the runner with the story state bound (StoryBridge), headless, and prints
// what each registry scene recorded when it committed. An offline check of a story folder against its registry, not
// the game.
//
//   ftcore_storywalk <story dir> <registry dir> [--result FLAG=VALUE]... [--steps N]
//   ftcore_storywalk <story dir> <registry dir> --inputs FILE [--steps N]
//
// <story dir> holds story.json and scenes/<scene id>.json. <registry dir> holds story_facts.csv and story_tuning.json,
// and may hold story_scene_map.csv. Lines, directions and holds are passed; a choice takes its first selectable option.
// A gameplay beat whose `sets` flag has a --result takes that value (true, false or an integer); one with a `stub`
// takes the stub; one with `sets` and neither fails the walk. Gameplay without `sets` is resumed as done.
//
// --inputs FILE scripts every gameplay hand-off instead (no stubs, no --result): one entry per hand-off, in order,
//   gameplay KIND [FLAG=VALUE] [press SIDE:KIND]...     ('#' starts a comment)
// The entry's kind must be the beat's; it gives a value exactly when the beat `sets` a flag, and that flag. Its
// presses are counted on the state (storyCountPress) before the beat resumes. A hand-off with no entry left, an
// entry left over, a mismatch or a refused press fails the walk. It also traces writes in the order they happen:
//   press SIDE KIND CODE  a press counted, with the registry scene open at the time
//   write ID VALUE CODE   a fact the state took, with the registry scene open at the time
//
// Output, one record per line:
//   scene CODE            a registry scene committed
//   set ID VALUE          a fact that scene was the last to write, as it stands
//   wide ID VALUE         a wide value that changed since the last commit
//   derived INDEX VALUE   after a commit: each registry derived value that is valid and changed, read at the committed
//                         scene (read after the move that committed, so flag beats the same move ran at the start
//                         of the next scene count)
//   gameplay KIND [FLAG=VALUE]
//   choice INDEX
//   flag ID VALUE         at the end: every flag that is not a fact flag (sf.)
//   kept ID SLOT          the story cleared a set fact: kept as it is (facts are never cleared; a note, not a failure)
//   WALK OK | WALK FAILED: reason
// Exit code 0 only with WALK OK, 2 for bad arguments, 1 for any other failure: a refused import or bind, a scene id
// that is not a plain file name, any bridge refusal (a fact write or a scene), a --result no gameplay beat used, a
// gameplay value it cannot give, a stuck runner, or the step cap. Each move commits at most one registry scene, so the
// wide lines after a scene line belong to it.

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "ftcore/StoryBridge.h"
#include "ftcore/StoryDatabase.h"
#include "ftcore/StoryImport.h"
#include "ftcore/StoryRunner.h"
#include "ftcore/StoryState.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace ftcore;

namespace {

bool readFile(const std::string& path, std::string& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[1 << 16];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out.append(buf, n);
    std::fclose(f);
    return true;
}

int fail(const std::string& why, int code = 1) {
    std::printf("WALK FAILED: %s\n", why.c_str());
    return code;
}

int usage(const std::string& why) { return fail(why, 2); }

// A scene id names a file in scenes/: no separators, drive colons or parent steps.
bool plainName(std::string_view id) {
    return !id.empty() && id.find_first_of("/\\:") == std::string_view::npos && id.find("..") == std::string_view::npos;
}

bool parseValue(std::string_view text, FlagValue& out) {
    if (text == "true") return out = FlagValue::boolean(true), true;
    if (text == "false") return out = FlagValue::boolean(false), true;
    if (text.empty()) return false;
    std::int64_t v = 0;
    bool neg = false;
    std::size_t i = 0;
    if (text[0] == '-') neg = true, i = 1;
    if (i == text.size()) return false;
    for (; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9' || v > 100000000000LL) return false;
        v = v * 10 + (text[i] - '0');
    }
    out = FlagValue::integer(neg ? -v : v);
    return true;
}

struct Result {
    std::string flag;
    FlagValue value;
    bool used = false;
};

struct Reports {
    int count = 0;
    const StoryDatabase* db = nullptr;
    const FactRegistry* reg = nullptr;
};

// One scripted gameplay hand-off.
struct Input {
    int line = 0;
    std::string kind, flag;
    FlagValue value;
    bool hasValue = false;
    std::vector<std::array<std::uint8_t, 2>> presses; // side, kind
};

bool parseInputs(const std::string& text, std::vector<Input>& out, std::string& error) {
    int lineNo = 0;
    const std::size_t start = text.rfind("\xEF\xBB\xBF", 0) == 0 ? 3 : 0; // a UTF-8 byte order mark
    for (std::size_t at = start; at < text.size();) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string_view line(text.data() + at, end - at);
        at = end + 1;
        ++lineNo;
        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        std::vector<std::string_view> w;
        for (std::size_t i = 0; i < line.size();) {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
            std::size_t j = i;
            while (j < line.size() && line[j] != ' ' && line[j] != '\t' && line[j] != '\r') ++j;
            if (j > i) w.push_back(line.substr(i, j - i));
            i = j;
        }
        if (w.empty()) continue;
        const std::string where = "inputs line " + std::to_string(lineNo) + ": ";
        if (w[0] != "gameplay" || w.size() < 2) return error = where + "expected gameplay KIND ...", false;
        Input in;
        in.line = lineNo;
        in.kind = std::string(w[1]);
        for (std::size_t i = 2; i < w.size(); ++i) {
            if (w[i] == "press") {
                const std::string_view sk = i + 1 < w.size() ? w[++i] : std::string_view();
                if (sk.size() != 3 || sk[1] != ':' || sk[0] < '0' || sk[0] > '1' || sk[2] < '0' || sk[2] > '2')
                    return error = where + "press: expected SIDE:KIND (side 0..1, kind 0..2)", false;
                in.presses.push_back({static_cast<std::uint8_t>(sk[0] - '0'), static_cast<std::uint8_t>(sk[2] - '0')});
                continue;
            }
            const std::size_t eq = w[i].find('=');
            if (in.hasValue || eq == std::string_view::npos || eq == 0 || !parseValue(w[i].substr(eq + 1), in.value))
                return error = where + "expected one FLAG=VALUE (true, false or an integer) or press SIDE:KIND", false;
            in.flag = std::string(w[i].substr(0, eq));
            in.hasValue = true;
        }
        out.push_back(std::move(in));
    }
    return true;
}

void onWrite(void* user, std::uint16_t fact, std::uint8_t value, std::uint8_t ordinal) {
    const FactRegistry& reg = *static_cast<const Reports*>(user)->reg;
    const std::string_view id = reg.facts[fact].id();
    std::printf("write %.*s %u %s\n", static_cast<int>(id.size()), id.data(), static_cast<unsigned>(value),
                ordinal == kNoScene ? "-" : std::string(storySceneCode(reg, ordinal)).c_str());
}

void onReport(void* user, const StoryBridgeReport& r) {
    Reports& rep = *static_cast<Reports*>(user);
    const std::string_view flag = r.flag < rep.db->flagCount() ? rep.db->view(rep.db->flag(r.flag).id) : "?";
    switch (r.kind) {
    case StoryBridgeReport::Kind::ClearIgnored: // by design: a note, not a failure
        std::printf("kept %.*s %u\n", static_cast<int>(flag.size()), flag.data(), static_cast<unsigned>(r.fact));
        return;
    case StoryBridgeReport::Kind::FactRefused:
        std::printf("refused %.*s %lld (result %d)\n", static_cast<int>(flag.size()), flag.data(),
                    static_cast<long long>(r.value), static_cast<int>(r.result));
        break;
    case StoryBridgeReport::Kind::SceneRefused: std::printf("refused scene %u\n", static_cast<unsigned>(r.ordinal)); break;
    case StoryBridgeReport::Kind::SyncMissing: std::printf("sync missing\n"); break;
    }
    ++rep.count;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: ftcore_storywalk <story dir> <registry dir> [--result FLAG=VALUE]... | --inputs FILE [--steps N]\n");
        return 2;
    }
    const std::string storyDir = argv[1], regDir = argv[2];
    std::vector<Result> results;
    std::string inputsPath;
    long steps = 100000;
    for (int i = 3; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--result" && i + 1 < argc) {
            const std::string_view kv = argv[++i];
            const std::size_t eq = kv.find('=');
            Result r;
            if (eq == std::string_view::npos || eq == 0 || !parseValue(kv.substr(eq + 1), r.value))
                return usage("--result: expected FLAG=VALUE (true, false or an integer)");
            r.flag = std::string(kv.substr(0, eq));
            for (const Result& o : results)
                if (o.flag == r.flag) return usage("--result: " + r.flag + " given twice");
            results.push_back(r);
        } else if (a == "--inputs" && i + 1 < argc) {
            if (!inputsPath.empty()) return usage("--inputs given twice");
            inputsPath = argv[++i];
            if (inputsPath.empty()) return usage("--inputs: expected a file");
        } else if (a == "--steps" && i + 1 < argc) {
            const char* text = argv[++i];
            char* end = nullptr;
            errno = 0;
            steps = std::strtol(text, &end, 10);
            if (end == text || *end != 0 || errno == ERANGE || steps <= 0) return usage("--steps: expected a positive number");
        } else {
            return usage("unknown argument: " + std::string(a));
        }
    }
    if (!inputsPath.empty() && !results.empty()) return usage("--inputs scripts every gameplay value: no --result");
    std::vector<Input> inputs;
    if (!inputsPath.empty()) {
        std::string text, error;
        if (!readFile(inputsPath, text)) return usage("--inputs: cannot read " + inputsPath);
        if (!parseInputs(text, inputs, error)) return usage(error);
    }
    const bool scripted = !inputsPath.empty();
    std::size_t nextInput = 0;

    // The registry and the optional scene map.
    std::string csv, json, mapCsv, error;
    if (!readFile(regDir + "/story_facts.csv", csv) || !readFile(regDir + "/story_tuning.json", json))
        return fail("no story_facts.csv and story_tuning.json in " + regDir);
    auto reg = std::make_unique<FactRegistry>();
    if (!importStoryRegistry(csv, json, *reg, &error)) return fail("registry refused:\n" + error);
    std::vector<StorySceneLink> links;
    if (readFile(regDir + "/story_scene_map.csv", mapCsv) && !importStorySceneMap(mapCsv, *reg, links, &error))
        return fail("scene map refused: " + error);

    // The story.
    std::string storyJson;
    if (!readFile(storyDir + "/story.json", storyJson)) return fail("no story.json in " + storyDir);
    std::vector<std::string> ids;
    if (!storySceneIds(storyJson, ids, &error)) return fail(error);
    std::vector<std::string> sceneTexts(ids.size());
    std::vector<std::string_view> sceneViews;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (!plainName(ids[i])) return fail("scene id " + ids[i] + " is not a plain file name");
        if (!readFile(storyDir + "/scenes/" + ids[i] + ".json", sceneTexts[i]))
            return fail("no scenes/" + ids[i] + ".json");
        sceneViews.push_back(sceneTexts[i]);
    }
    StoryDatabase db;
    std::vector<std::string> warnings;
    if (!importStory(storyJson, sceneViews, db, &error, &warnings)) return fail("story refused:\n" + error);
    for (const std::string& w : warnings) std::printf("warning %s\n", w.c_str());
    for (const Result& r : results)
        if (db.findFlag(r.flag) == kNone) return fail("--result: the story has no flag " + r.flag);

    auto state = std::make_unique<StoryState>();
    StoryRunner runner(&db);
    StoryBridge bridge;
    Reports reports{0, &db, reg.get()};
    bridge.setReportHandler(onReport, &reports);
    if (scripted) bridge.setWriteHandler(onWrite, &reports);
    if (!bridge.bind(runner, *state, *reg, links, &error)) return fail("bind refused: " + error);
    if (!runner.start()) return fail("the story does not start");

    std::uint8_t committed = kNoScene;
    std::array<std::uint32_t, kWideSlots> wideBefore{};
    DerivedValues derivedBefore;
    // Prints the scenes committed since the last call, each with the facts it was the last to write. False when the
    // last committed scene went backwards (the state only moves forward while the story plays).
    auto printCommits = [&]() -> bool {
        const StoryState& s = *state;
        if (s.lastCommitted == committed) return true;
        if (s.lastCommitted == kNoScene || (committed != kNoScene && s.lastCommitted < committed)) return false;
        const unsigned from = committed == kNoScene ? 0u : committed + 1u;
        for (unsigned o = from; o <= s.lastCommitted; ++o) {
            bool any = o == s.lastCommitted;
            for (std::uint16_t f = 1; f <= reg->factsKnown && !any; ++f) any = s.stamp[f] == o;
            if (!any) continue;
            std::printf("scene %s\n", reg->sceneCodes[o].data());
            for (std::uint16_t f = 1; f <= reg->factsKnown; ++f) {
                if (s.stamp[f] != o) continue;
                const std::string_view id = reg->facts[f].id();
                std::printf("set %.*s %u\n", static_cast<int>(id.size()), id.data(), static_cast<unsigned>(s.facts[f]));
            }
        }
        for (std::uint16_t w = 0; w < reg->wideKnown; ++w) {
            if (s.wide[w] == wideBefore[w]) continue;
            const std::string_view id = reg->wide[w].id();
            std::printf("wide %.*s %u\n", static_cast<int>(id.size()), id.data(), static_cast<unsigned>(s.wide[w]));
            wideBefore[w] = s.wide[w];
        }
        const DerivedValues d = storySceneReads(s, *reg, s.lastCommitted).derived; // as read at the committed scene
        for (std::uint8_t i = 0; i < reg->derivedCount; ++i) {
            const bool valid = ((d.valid >> i) & 1u) != 0;
            if (!valid || (((derivedBefore.valid >> i) & 1u) != 0 && derivedBefore.value[i] == d.value[i])) continue;
            std::printf("derived %u %u\n", static_cast<unsigned>(i), static_cast<unsigned>(d.value[i]));
        }
        derivedBefore = d;
        committed = s.lastCommitted;
        return true;
    };

    long step = 0;
    for (; step < steps && runner.state() != StoryRunner::State::Finished; ++step) {
        bool moved = false;
        switch (runner.state()) {
        case StoryRunner::State::Line:
        case StoryRunner::State::Direction: moved = runner.advance(); break;
        case StoryRunner::State::Hold:
            runner.skipHold();
            moved = true;
            break;
        case StoryRunner::State::Choice:
            for (std::uint32_t i = 0; i < runner.optionCount() && !moved; ++i) {
                if (!runner.optionSelectable(i)) continue;
                moved = runner.choose(i);
                if (moved) std::printf("choice %u\n", static_cast<unsigned>(i));
            }
            break;
        case StoryRunner::State::Gameplay: {
            const std::string kind(runner.gameplayKindName());
            const std::string sets(runner.param("sets"));
            if (scripted) {
                if (nextInput == inputs.size()) return fail("a " + kind + " hand-off with no input left");
                const Input& in = inputs[nextInput++];
                const std::string at = "inputs line " + std::to_string(in.line) + ": ";
                if (in.kind != kind) return fail(at + "expected gameplay " + in.kind + ", the story hands off " + kind);
                if (in.hasValue != !sets.empty() || (in.hasValue && in.flag != sets))
                    return fail(at + "the hand-off sets " + (sets.empty() ? std::string("nothing") : sets) + ", the input gives " +
                                (in.hasValue ? in.flag : std::string("no value")));
                if (in.hasValue)
                    std::printf("gameplay %s %s=%s\n", kind.c_str(), sets.c_str(), db.formatFlagValue(in.value).c_str());
                else
                    std::printf("gameplay %s\n", kind.c_str());
                for (const auto& pk : in.presses) {
                    const auto factsBefore = state->facts;
                    const SetResult res = storyCountPress(*state, *reg, pk[0], pk[1]);
                    std::printf("press %u %u %s\n", static_cast<unsigned>(pk[0]), static_cast<unsigned>(pk[1]),
                                state->openScene == kNoScene ? "-" : std::string(storySceneCode(*reg, state->openScene)).c_str());
                    // Unchanged: accepted with its counter at the cap.
                    if (res != SetResult::Ok && res != SetResult::Unchanged) return fail(at + "press refused (result " + std::to_string(static_cast<int>(res)) + ")");
                    for (std::uint16_t f = 1; f <= reg->factsKnown; ++f)
                        if (state->facts[f] != factsBefore[f]) onWrite(&reports, f, state->facts[f], state->openScene);
                }
                moved = in.hasValue ? runner.resume(in.value) : runner.resume();
                break;
            }
            if (sets.empty()) {
                std::printf("gameplay %s\n", kind.c_str());
                moved = runner.resume();
                break;
            }
            Result* given = nullptr;
            for (Result& r : results)
                if (r.flag == sets) {
                    r.used = true;
                    given = &r;
                }
            if (given) {
                std::printf("gameplay %s %s=%s\n", kind.c_str(), sets.c_str(), db.formatFlagValue(given->value).c_str());
                moved = runner.resume(given->value);
            } else if (!runner.param("stub").empty()) {
                std::printf("gameplay %s %s=%s (stub)\n", kind.c_str(), sets.c_str(), std::string(runner.param("stub")).c_str());
                moved = runner.resume();
            } else {
                return fail("a gameplay beat sets " + sets + " and has no --result or stub");
            }
            break;
        }
        case StoryRunner::State::Idle:
        case StoryRunner::State::Finished: break;
        }
        if (!moved) return fail("the runner refused to move on in state " + std::to_string(static_cast<int>(runner.state())));
        if (!printCommits()) return fail("the last committed scene went backwards");
        if (reports.count) return fail("the bridge reported a refusal");
    }
    if (runner.state() != StoryRunner::State::Finished) return fail("step cap reached (" + std::to_string(steps) + ")");
    if (!printCommits()) return fail("the last committed scene went backwards");
    for (std::uint32_t i = 0; i < db.flagCount(); ++i) {
        const std::string_view id = db.view(db.flag(i).id);
        if (id.substr(0, 3) == "sf.") continue;
        std::printf("flag %.*s %s\n", static_cast<int>(id.size()), id.data(), db.formatFlagValue(runner.flag(i)).c_str());
    }
    if (reports.count) return fail("the bridge reported a refusal");
    for (const Result& r : results)
        if (!r.used) return fail("--result " + r.flag + ": no gameplay beat set it");
    if (nextInput < inputs.size())
        return fail("inputs line " + std::to_string(inputs[nextInput].line) + ": no hand-off left for it");
    std::printf("WALK OK\n");
    return 0;
}
