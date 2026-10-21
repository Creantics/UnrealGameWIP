#include "ftcore/Mini.h"

#include "ftcore/ComboGenerator.h"
#include "ftcore/Json.h"

#include <algorithm>
#include <initializer_list>

namespace ftcore {

MiniDef defaultMiniDef() { return MiniDef{}; }

const char* miniOutcomeName(MiniOutcome o) {
    switch (o) {
    case MiniOutcome::Cleared: return "CLEARED";
    case MiniOutcome::Retry: return "AGAIN";
    case MiniOutcome::Continue: return "ONWARD";
    case MiniOutcome::Pending: break;
    }
    return "";
}

// ---- screen ----

void MiniScreen::begin(bool hasStep, const MiniDef& def, bool autoPlay) {
    def_ = def;
    autoPlay_ = autoPlay;
    cleared_ = false;
    attempt_ = 0;
    best_ = last_ = 0.0f;
    time_ = roundAge_ = resultAge_ = 0.0;
    outcome_ = MiniOutcome::Pending;
    lastGrade_ = Grade::Pending;
    lastGradeTime_ = 0.0;
    ConductorConfig grid;
    grid.bpm = def_.bpm > 0.0 ? def_.bpm : 100.0;
    grid_ = Conductor(grid);
    if (!hasStep) {
        phase_ = MiniPhase::NoStep;
        return;
    }
    // Both chart buffers swap in and out of the sequence, so both get the full capacity.
    const std::size_t cap = static_cast<std::size_t>(kMaxMiniSteps) * 2;
    chart_.reserve(cap);
    seq_.reserve(cap);
    results_.reserve(cap + 1);
    startRound();
}

void MiniScreen::startRound() {
    ComboParams p;
    p.steps = std::clamp(def_.steps, 1, kMaxMiniSteps);
    p.lanes = std::clamp(def_.lanes, 2, 4);
    p.bpm = grid_.config().bpm;
    p.subdivision = def_.subdivision > 0.0 ? def_.subdivision : 1.0;
    p.interval = p.subdivision * grid_.secondsPerBeat();
    p.jumpChance = 0.0f;
    p.burstChance = 0.0f;
    // Each retry plays a fresh chart of the same shape.
    generateComboInto(chart_, p, def_.lead, static_cast<std::uint64_t>(def_.seed) + static_cast<std::uint64_t>(attempt_));
    seq_.reset(chart_, scaled(JudgeWindows{}, def_.window > 0.0 ? def_.window : 1.0), kStrayLockout);

    phase_ = MiniPhase::Playing;
    outcome_ = MiniOutcome::Pending;
    time_ = roundAge_ = resultAge_ = 0.0;
    autoNext_ = 0;
    lastGrade_ = Grade::Pending;
    lastGradeTime_ = 0.0;
    scale_ = autoPlay_ ? std::max(1.0, roundLength() / kAutoBudget) : 1.0;
}

double MiniScreen::roundLength() const {
    if (seq_.size() == 0) return 0.0;
    return seq_.note(seq_.size() - 1).time + seq_.windows().good;
}

void MiniScreen::collect(double t) {
    for (const NoteResult& r : results_) {
        lastGrade_ = r.grade;
        lastGradeTime_ = t;
    }
    results_.clear();
}

void MiniScreen::autopilot(double upTo) {
    while (autoNext_ < seq_.size() && seq_.note(autoNext_).time <= upTo) {
        const ComboNote& n = seq_.note(autoNext_);
        seq_.press(n.arrow, n.time, results_);
        collect(n.time);
        ++autoNext_;
    }
}

void MiniScreen::endRound() {
    last_ = seq_.accuracy();
    best_ = std::max(best_, last_);
    if (static_cast<double>(last_) + 1e-6 >= def_.pass) {
        cleared_ = true;
        outcome_ = MiniOutcome::Cleared;
    } else if (attempt_ + 1 < std::max(def_.tries, 1)) {
        outcome_ = MiniOutcome::Retry;
    } else {
        outcome_ = MiniOutcome::Continue;
    }
    phase_ = MiniPhase::Result;
    resultAge_ = 0.0;
}

void MiniScreen::tick(double gameSeconds) {
    if (gameSeconds < 0.0) gameSeconds = 0.0;
    switch (phase_) {
    case MiniPhase::Playing: {
        roundAge_ += gameSeconds;
        const double target = time_ + gameSeconds * scale_;
        if (autoPlay_) autopilot(target);
        time_ = target;
        seq_.update(time_, results_);
        collect(time_);
        if (seq_.complete()) endRound();
        break;
    }
    case MiniPhase::Result:
        resultAge_ += gameSeconds;
        if (resultAge_ >= (autoPlay_ ? kAutoResultTime : kResultTime)) {
            if (outcome_ == MiniOutcome::Retry) {
                ++attempt_;
                startRound();
            } else {
                phase_ = MiniPhase::Done;
            }
        }
        break;
    case MiniPhase::NoStep:
    case MiniPhase::Done: break;
    }
}

void MiniScreen::press(Arrow input) {
    if (phase_ != MiniPhase::Playing || autoPlay_ || roundAge_ < kInputGrace) return;
    seq_.press(input, time_, results_);
    collect(time_);
}

// ---- data ----

const MiniEntry* MiniTable::findEntry(std::string_view scene, std::uint32_t beat, std::uint32_t step) const {
    for (const MiniEntry& e : entries)
        if (e.scene == scene && e.beat == beat && e.step == step) return &e;
    return nullptr;
}

const MiniDef& MiniTable::find(std::string_view scene, std::uint32_t beat, std::uint32_t step) const {
    const MiniEntry* e = findEntry(scene, beat, step);
    return e ? e->def : fallback;
}

namespace {

struct MiniLoad {
    std::vector<std::string> errors;
    std::vector<std::string>* warnings = nullptr;

    void error(const std::string& path, std::string_view what) { errors.push_back(path + ": " + std::string(what)); }
    void unknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys) {
        for (std::uint32_t k = 0; k < j.size(); ++k) {
            const std::string_view key = j.key(k);
            if (std::find(keys.begin(), keys.end(), key) == keys.end() && warnings)
                warnings->push_back((path.empty() ? std::string("mini file") : path) + ": unknown key: " + std::string(key));
        }
    }
    void number(JsonValue j, const std::string& path, const char* key, double lo, double hi, double& out) {
        const JsonValue v = j[key];
        if (v.isAbsent()) return;
        const double x = v.number(lo - 1.0);
        if (!v.isNumber() || x < lo || x > hi) {
            error(path + "." + key, "expected a number in " + trim(lo) + ".." + trim(hi));
            return;
        }
        out = x;
    }
    void integer(JsonValue j, const std::string& path, const char* key, std::int64_t lo, std::int64_t hi, std::int64_t& out) {
        const JsonValue v = j[key];
        if (v.isAbsent()) return;
        if (!v.isInteger() || v.integer() < lo || v.integer() > hi) {
            error(path + "." + key, "expected an integer in " + std::to_string(lo) + ".." + std::to_string(hi));
            return;
        }
        out = v.integer();
    }
    static std::string trim(double x) {
        std::string s = std::to_string(x);
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s;
    }
};

void miniParseDef(JsonValue j, const std::string& path, MiniDef& d, MiniLoad& L) {
    std::int64_t steps = d.steps, lanes = d.lanes, seed = d.seed, tries = d.tries;
    L.number(j, path, "bpm", 60.0, 200.0, d.bpm);
    L.integer(j, path, "steps", 4, kMaxMiniSteps, steps);
    L.integer(j, path, "lanes", 2, 4, lanes);
    L.number(j, path, "subdivision", 0.5, 1.0, d.subdivision);
    L.integer(j, path, "seed", 0, 2147483647, seed);
    L.number(j, path, "pass", 0.0, 1.0, d.pass);
    L.integer(j, path, "tries", 1, 9, tries);
    L.number(j, path, "lead", 1.0, 4.0, d.lead);
    L.number(j, path, "window", 0.5, 2.0, d.window);
    if (d.subdivision != 1.0 && d.subdivision != 0.5) L.error(path + ".subdivision", "expected 1 or 0.5");
    d.steps = static_cast<int>(steps);
    d.lanes = static_cast<int>(lanes);
    d.seed = static_cast<std::uint32_t>(seed);
    d.tries = static_cast<int>(tries);
}

} // namespace

bool importMiniTable(std::string_view json, MiniTable& out, std::string* error, std::vector<std::string>* warnings) {
    out = MiniTable{};
    MiniLoad L;
    L.warnings = warnings;
    const auto finish = [&]() {
        if (L.errors.empty()) return true;
        if (error) {
            error->clear();
            for (const std::string& e : L.errors) {
                if (!error->empty()) *error += '\n';
                *error += e;
            }
        }
        return false;
    };

    JsonDocument doc;
    std::string err;
    if (!doc.parse(json, &err)) {
        L.error("mini file", err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        L.error("mini file", "expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !v.isAbsent() && !(v.isInteger() && v.integer() == 1)) L.error("version", "expected 1");
    L.unknownKeys(root, "", {"version", "default", "minis"});

    MiniTable table;
    const JsonValue def = root["default"];
    if (!def.isObject()) {
        L.error("default", "expected an object");
    } else {
        L.unknownKeys(def, "default", {"bpm", "steps", "lanes", "subdivision", "seed", "pass", "tries", "lead", "window"});
        miniParseDef(def, "default", table.fallback, L);
    }

    const JsonValue list = root["minis"];
    if (!list.isAbsent() && !list.isArray()) L.error("minis", "expected an array");
    for (std::uint32_t i = 0; i < list.size() && list.isArray(); ++i) {
        const JsonValue j = list[i];
        const std::string path = "minis[" + std::to_string(i) + "]";
        if (!j.isObject()) {
            L.error(path, "expected an object");
            continue;
        }
        L.unknownKeys(j, path, {"scene", "beat", "step", "bpm", "steps", "lanes", "subdivision", "seed", "pass", "tries", "lead",
                                "window"});
        MiniEntry e;
        e.scene = std::string(j["scene"].string());
        bool good = !e.scene.empty();
        if (!good) L.error(path + ".scene", "expected a scene id");
        const JsonValue b = j["beat"];
        const JsonValue s = j["step"];
        if (!b.isInteger() || b.integer() < 0 || b.integer() > 1000000) {
            L.error(path + ".beat", "expected an integer in 0..1000000");
            good = false;
        } else {
            e.beat = static_cast<std::uint32_t>(b.integer());
        }
        if (!s.isAbsent() && (!s.isInteger() || s.integer() < 0 || s.integer() > 1000)) {
            L.error(path + ".step", "expected an integer in 0..1000");
            good = false;
        } else {
            e.step = static_cast<std::uint32_t>(s.integer(0));
        }
        miniParseDef(j, path, e.def, L);
        if (good && table.findEntry(e.scene, e.beat, e.step))
            L.error(path, "duplicate entry for " + e.scene + " beat " + std::to_string(e.beat) + " step " + std::to_string(e.step));
        table.entries.push_back(std::move(e));
    }
    if (!finish()) return false;
    out = std::move(table);
    return true;
}

} // namespace ftcore
