#include "ftcore/Qte.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace ftcore {

const char* qteGradeName(QteGrade g) {
    switch (g) {
    case QteGrade::Perfect: return "PERFECT";
    case QteGrade::Good: return "GOOD";
    case QteGrade::Miss: return "MISS";
    default: return "";
    }
}

const char* qteOutcomeName(QteOutcome o) {
    switch (o) {
    case QteOutcome::Perfect: return "Perfect";
    case QteOutcome::Good: return "Good";
    case QteOutcome::Fair: return "Fair";
    default: return "Poor";
    }
}

QteDef defaultQteDef() {
    QteDef def;
    QtePrompt p;
    p.kind = QteKind::Tap;
    p.input = Arrow::Left;
    p.at = 1.0;
    def.add(p);
    p.input = Arrow::Right;
    p.at = 1.8;
    def.add(p);
    p.input = Arrow::Up;
    p.window = QteWindow::Narrow;
    p.at = 2.6;
    def.add(p);
    p.kind = QteKind::Hold;
    p.input = Arrow::Down;
    p.window = QteWindow::Wide;
    p.at = 3.4;
    p.hold = 0.8;
    def.add(p);
    p = QtePrompt{};
    p.kind = QteKind::Mash;
    p.input = Arrow::Left;
    p.at = 5.0;
    p.mashCount = 8;
    p.limit = 1.5;
    def.add(p);
    return def;
}

// ---- QteSequence ----

void QteSequence::begin(const QteDef& def, bool coop, const DefenceWindows& windows) {
    def_ = def;
    if (def_.count > kMaxQtePrompts) def_.count = static_cast<std::uint8_t>(kMaxQtePrompts);
    coop_ = coop;
    for (std::size_t i = 0; i < def_.count; ++i) def_.prompts[i].actor = coop ? static_cast<std::uint8_t>(i & 1u) : 0;
    windows_ = windows;
    grades_.fill(QteGrade::Pending);
    now_ = 0.0;
    next_ = 0;
    strays_ = 0;
    arm();
}

void QteSequence::arm() {
    mashDone_ = 0;
    holdGrade_ = QteGrade::Pending;
    if (next_ >= def_.size()) {
        phase_ = Phase::Done;
        judge_.clear();
        return;
    }
    const QtePrompt& p = def_[next_];
    if (p.kind == QteKind::Mash) {
        phase_ = Phase::Mashing;
        judge_.clear();
        return;
    }
    // The defence maths: the prompt is one hit whose impact is `at`.
    AttackTimeline t;
    t.clear();
    TimelineHit h;
    h.at = 0.0;
    h.tell = qteTell(p);
    t.add(h);
    judge_.reset(t, p.at, windows_);
    phase_ = Phase::Awaiting;
}

void QteSequence::resolve(QteGrade g, double time, QteResults& out) {
    grades_[next_] = g;
    if (out.count < out.items.size()) {
        QteResult& r = out.items[out.count++];
        r.prompt = static_cast<std::uint8_t>(next_);
        r.grade = g;
        r.time = time;
    }
    ++next_;
    arm();
}

void QteSequence::advance(double t, QteResults& out) {
    if (t > now_) now_ = t;
    while (next_ < def_.size()) {
        const QtePrompt& p = def_[next_];
        if (phase_ == Phase::Awaiting) {
            if (judge_.update(now_).empty()) return; // nothing landed
            resolve(QteGrade::Miss, p.at, out);
        } else if (phase_ == Phase::Holding) {
            if (now_ < p.at + p.hold) return;
            resolve(holdGrade_, p.at + p.hold, out);
        } else if (phase_ == Phase::Mashing) {
            if (now_ < p.at + p.limit) return;
            resolve(QteGrade::Miss, p.at + p.limit, out);
        } else {
            return;
        }
    }
}

QteResults QteSequence::update(double now) {
    QteResults out;
    advance(now, out);
    return out;
}

QteResults QteSequence::press(Arrow input, double t) {
    QteResults out;
    advance(t, out);
    if (complete()) {
        ++strays_;
        return out;
    }
    const QtePrompt& p = def_[next_];
    if (input != p.input) {
        ++strays_;
        return out;
    }
    if (phase_ == Phase::Awaiting) {
        const bool narrow = p.window == QteWindow::Narrow;
        DefenceResults r = judge_.press(narrow ? DefenceInput::Parry : DefenceInput::Dodge, now_);
        if (r.empty() && narrow) r = judge_.press(DefenceInput::Dodge, now_); // outside the parry window: still Good
        if (r.empty()) {
            ++strays_; // too early
            return out;
        }
        const DefenceResult& d = r[0];
        const QteGrade g = (d.outcome == DefenceOutcome::Perfect || d.refund) ? QteGrade::Perfect : QteGrade::Good;
        if (p.kind == QteKind::Hold) {
            holdGrade_ = g;
            phase_ = Phase::Holding;
        } else {
            resolve(g, now_, out);
        }
    } else if (phase_ == Phase::Mashing) {
        if (now_ < p.at) {
            ++strays_;
            return out;
        }
        ++mashDone_;
        if (mashDone_ >= static_cast<int>(p.mashCount))
            resolve(now_ <= p.at + p.limit * kQtePerfectFraction ? QteGrade::Perfect : QteGrade::Good, now_, out);
    }
    return out;
}

QteResults QteSequence::release(Arrow input, double t) {
    QteResults out;
    advance(t, out);
    if (phase_ == Phase::Holding && next_ < def_.size()) {
        const QtePrompt& p = def_[next_];
        if (input == p.input && now_ < p.at + p.hold - kQteHoldSlack) resolve(QteGrade::Miss, now_, out);
    }
    return out;
}

QteResults QteSequence::finish(double now) {
    QteResults out;
    advance(now, out);
    while (next_ < def_.size()) resolve(QteGrade::Miss, now_, out);
    return out;
}

double QteSequence::idealTime(const QtePrompt& p) const {
    return judge_.idealPress(0, p.window == QteWindow::Narrow ? DefenceInput::Parry : DefenceInput::Dodge);
}

QteAction QteSequence::autopilot(double now) const {
    QteAction a;
    if (complete()) return a;
    const QtePrompt& p = def_[next_];
    double t = 0.0;
    QteAction::Kind kind = QteAction::Kind::Press;
    if (phase_ == Phase::Awaiting) {
        t = idealTime(p);
    } else if (phase_ == Phase::Mashing) {
        t = p.at + p.limit * (kQtePerfectFraction - 0.1) * static_cast<double>(mashDone_ + 1) / static_cast<double>(p.mashCount);
    } else if (phase_ == Phase::Holding) {
        // Let go at the end of the hold. The release moves the clock there, which resolves the prompt, so
        // a long frame still reaches the prompts after it in order.
        t = p.at + p.hold;
        kind = QteAction::Kind::Release;
    } else {
        return a;
    }
    if (t < now_) t = now_;
    if (t <= now) {
        a.kind = kind;
        a.input = p.input;
        a.time = t;
    }
    return a;
}

double QteSequence::holdProgress() const {
    if (phase_ != Phase::Holding || next_ >= def_.size()) return 0.0;
    const QtePrompt& p = def_[next_];
    const double k = p.hold > 0.0 ? (now_ - p.at) / p.hold : 1.0;
    return k < 0.0 ? 0.0 : (k > 1.0 ? 1.0 : k);
}

double QteSequence::tellProgress(std::size_t i, double t) const {
    if (i >= def_.size() || grades_[i] != QteGrade::Pending) return -1.0;
    const QtePrompt& p = def_[i];
    const double tell = qteTell(p);
    const double left = p.at - t;
    if (tell <= 0.0 || left > tell || left <= 0.0) return -1.0;
    return left / tell;
}

std::size_t QteSequence::count(QteGrade g) const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < def_.size(); ++i) n += grades_[i] == g ? 1u : 0u;
    return n;
}

QteOutcome QteSequence::outcome() const {
    const std::size_t n = def_.size();
    const std::size_t m = misses();
    if (perfects() == n) return QteOutcome::Perfect;
    if (m == 0) return QteOutcome::Good;
    return m * 2 <= n ? QteOutcome::Fair : QteOutcome::Poor;
}

// ---- QteScreen ----

void QteScreen::begin(bool hasStep, const QteDef& def, bool coop, bool autoPlay) {
    seq_.begin(def, coop);
    fresh_.count = 0;
    elapsed_ = 0.0;
    resultAge_ = 0.0;
    autoPlay_ = hasStep && autoPlay;
    scale_ = 1.0;
    if (autoPlay_ && def.length() > kAutoBudget) scale_ = def.length() / kAutoBudget;
    phase_ = !hasStep ? QtePhase::NoStep : (seq_.complete() ? QtePhase::Result : QtePhase::Playing);
}

void QteScreen::collect(const QteResults& r) {
    for (const QteResult& x : r)
        if (fresh_.count < fresh_.items.size()) fresh_.items[fresh_.count++] = x;
}

void QteScreen::tick(double gameSeconds) {
    if (phase_ == QtePhase::NoStep || phase_ == QtePhase::Done) return;
    const double dt = gameSeconds > 0.0 ? gameSeconds : 0.0;
    elapsed_ += dt;
    if (phase_ == QtePhase::Result) {
        resultAge_ += dt;
        if (resultAge_ >= kResultTime) phase_ = QtePhase::Done;
        return;
    }
    const double t = elapsed_ * scale_;
    if (autoPlay_) {
        for (int guard = 0; guard < 256; ++guard) { // bounded: every action moves the sequence on
            const QteAction a = seq_.autopilot(t);
            if (a.kind == QteAction::Kind::None) break;
            collect(a.kind == QteAction::Kind::Press ? seq_.press(a.input, a.time) : seq_.release(a.input, a.time));
        }
    }
    collect(seq_.update(t));
    if (autoPlay_ && elapsed_ >= kAutoCap) collect(seq_.finish(t));
    if (seq_.complete()) {
        phase_ = QtePhase::Result;
        resultAge_ = 0.0;
    }
}

void QteScreen::press(Arrow input) {
    if (phase_ != QtePhase::Playing || autoPlay_ || elapsed_ < kInputGrace) return;
    collect(seq_.press(input, elapsed_));
}

void QteScreen::release(Arrow input) {
    if (phase_ != QtePhase::Playing || autoPlay_ || elapsed_ < kInputGrace) return;
    collect(seq_.release(input, elapsed_));
}

// ---- data ----

namespace {

std::string qteStr(std::string_view s) { return std::string(s); }

void qteUnknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys,
                    std::vector<std::string>* warnings) {
    if (!warnings) return;
    for (std::uint32_t k = 0; k < j.size(); ++k) {
        const std::string_view key = j.key(k);
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            warnings->push_back((path.empty() ? std::string() : path + ": ") + "unknown key: " + qteStr(key));
    }
}

// A number in [lo, hi]; required or optional (left alone when absent). False (and an error) when bad.
bool qteNum(JsonValue j, const char* key, double& out, double lo, double hi, bool required, const std::string& path,
            std::vector<std::string>& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent() && !required) return false;
    const double d = v.number(-1e30);
    if (!v.isNumber() || !std::isfinite(d) || d < lo || d > hi) {
        errors.push_back(path + "." + key + ": expected a number in " + std::to_string(lo) + ".." + std::to_string(hi));
        return false;
    }
    out = d;
    return true;
}

bool qteParseDef(JsonValue prompts, const std::string& path, QteDef& out, std::vector<std::string>& errors,
                 std::vector<std::string>* warnings) {
    if (!prompts.isArray() || prompts.size() == 0 || prompts.size() > kMaxQtePrompts) {
        errors.push_back(path + ": expected an array of 1.." + std::to_string(kMaxQtePrompts) + " prompts");
        return false;
    }
    bool ok = true;
    double earliest = kQteMinFirst; // the next prompt may start no sooner than this
    for (std::uint32_t i = 0; i < prompts.size(); ++i) {
        const JsonValue j = prompts[i];
        const std::string p = path + "[" + std::to_string(i) + "]";
        if (!j.isObject()) {
            errors.push_back(p + ": expected an object");
            ok = false;
            continue;
        }
        qteUnknownKeys(j, p, {"input", "at", "window", "hold", "mash", "limit"}, warnings);
        QtePrompt q;
        const std::size_t before = errors.size();

        const std::string_view input = j["input"].string();
        if (input == "left") q.input = Arrow::Left;
        else if (input == "down") q.input = Arrow::Down;
        else if (input == "up") q.input = Arrow::Up;
        else if (input == "right") q.input = Arrow::Right;
        else errors.push_back(p + ".input: expected \"left\", \"down\", \"up\" or \"right\"");

        if (j.has("window")) {
            const std::string_view w = j["window"].string();
            if (w == "wide") q.window = QteWindow::Wide;
            else if (w == "narrow") q.window = QteWindow::Narrow;
            else errors.push_back(p + ".window: expected \"wide\" or \"narrow\"");
        }

        if (qteNum(j, "at", q.at, 0.0, kQteMaxAt, true, p, errors) && q.at < earliest)
            errors.push_back(p + ".at: expected a time of at least " + std::to_string(earliest) +
                             (i == 0 ? " (the first prompt)" : " (after the previous prompt, with the gap)"));

        const bool hasHold = j.has("hold");
        const bool hasMash = j.has("mash");
        if (hasHold && hasMash) errors.push_back(p + ": hold and mash are exclusive");
        if (hasHold && !hasMash) {
            q.kind = QteKind::Hold;
            qteNum(j, "hold", q.hold, kQteMinHold, kQteMaxHold, true, p, errors);
        }
        if (hasMash && !hasHold) {
            q.kind = QteKind::Mash;
            const JsonValue m = j["mash"];
            if (!m.isInteger() || m.integer() < kQteMinMash || m.integer() > kQteMaxMash)
                errors.push_back(p + ".mash: expected an integer in " + std::to_string(kQteMinMash) + ".." + std::to_string(kQteMaxMash));
            else
                q.mashCount = static_cast<std::uint8_t>(m.integer());
            qteNum(j, "limit", q.limit, kQteMinLimit, kQteMaxLimit, true, p, errors);
        } else if (j.has("limit")) {
            errors.push_back(p + ".limit: only a mash has a limit");
        }

        if (errors.size() != before) {
            ok = false;
            continue;
        }
        earliest = q.end() + kQteMinGap;
        out.add(q);
    }
    return ok;
}

} // namespace

const QteEntry* QteTable::findEntry(std::string_view scene, std::uint32_t beat, std::uint32_t step) const {
    for (const QteEntry& e : entries)
        if (e.beat == beat && e.step == step && e.scene == scene) return &e;
    return nullptr;
}

const QteDef& QteTable::find(std::string_view scene, std::uint32_t beat, std::uint32_t step) const {
    const QteEntry* e = findEntry(scene, beat, step);
    return e ? e->def : fallback;
}

bool importQteTable(std::string_view json, QteTable& out, std::string* error, std::vector<std::string>* warnings) {
    out = QteTable{};
    std::vector<std::string> errors;
    const auto finish = [&]() {
        if (errors.empty()) return true;
        if (error) {
            error->clear();
            for (const std::string& e : errors) {
                if (!error->empty()) *error += '\n';
                *error += e;
            }
        }
        return false;
    };

    JsonDocument doc;
    std::string err;
    if (!doc.parse(json, &err)) {
        errors.push_back("qte file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("qte file: expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !v.isAbsent() && !(v.isInteger() && v.integer() == 1))
        errors.push_back("version: expected 1");
    qteUnknownKeys(root, "", {"version", "default", "qtes"}, warnings);

    QteTable table;
    const JsonValue def = root["default"];
    if (!def.isObject()) {
        errors.push_back("default: expected an object");
    } else {
        qteUnknownKeys(def, "default", {"prompts"}, warnings);
        qteParseDef(def["prompts"], "default.prompts", table.fallback, errors, warnings);
    }

    const JsonValue list = root["qtes"];
    if (!list.isAbsent() && !list.isArray()) errors.push_back("qtes: expected an array");
    for (std::uint32_t i = 0; i < list.size() && list.isArray(); ++i) {
        const JsonValue j = list[i];
        const std::string path = "qtes[" + std::to_string(i) + "]";
        if (!j.isObject()) {
            errors.push_back(path + ": expected an object");
            continue;
        }
        qteUnknownKeys(j, path, {"scene", "beat", "step", "prompts"}, warnings);
        QteEntry e;
        e.scene = qteStr(j["scene"].string());
        if (e.scene.empty()) errors.push_back(path + ".scene: expected a scene id");
        const JsonValue b = j["beat"];
        const JsonValue s = j["step"];
        bool good = !e.scene.empty();
        if (!b.isInteger() || b.integer() < 0 || b.integer() > 1000000) {
            errors.push_back(path + ".beat: expected an integer in 0..1000000");
            good = false;
        } else {
            e.beat = static_cast<std::uint32_t>(b.integer());
        }
        if (!s.isAbsent() && (!s.isInteger() || s.integer() < 0 || s.integer() > 1000)) {
            errors.push_back(path + ".step: expected an integer in 0..1000");
            good = false;
        } else {
            e.step = static_cast<std::uint32_t>(s.integer(0));
        }
        qteParseDef(j["prompts"], path + ".prompts", e.def, errors, warnings);
        if (good && table.findEntry(e.scene, e.beat, e.step))
            errors.push_back(path + ": duplicate entry for " + e.scene + " beat " + std::to_string(e.beat) + " step " +
                             std::to_string(e.step));
        table.entries.push_back(std::move(e));
    }
    if (!finish()) return false;
    out = std::move(table);
    return true;
}

} // namespace ftcore
