#include "ftcore/ScarePacer.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ftcore {

const char* scareKindName(ScareKind k) {
    switch (k) {
    case ScareKind::Exploration: return "exploration";
    case ScareKind::Scene: return "scene";
    case ScareKind::Fight: return "fight";
    case ScareKind::Hallucination: return "hallucination";
    case ScareKind::Map: return "map";
    default: return "none";
    }
}

const char* scareWorldName(ScareWorld w) {
    switch (w) {
    case ScareWorld::Solhaven: return "solhaven";
    case ScareWorld::Lattice: return "lattice";
    case ScareWorld::Unwritten: return "unwritten";
    default: return "none";
    }
}

const char* scareSettingName(ScareSetting s) {
    switch (s) {
    case ScareSetting::Full: return "full";
    case ScareSetting::Soft: return "soft";
    case ScareSetting::Off: return "off";
    default: return "none";
    }
}

const char* scareFormName(ScareForm f) {
    switch (f) {
    case ScareForm::Full: return "full";
    case ScareForm::Soft: return "soft";
    case ScareForm::Calm: return "calm";
    case ScareForm::None: return "none";
    case ScareForm::Unpaced: return "unpaced";
    default: return "?";
    }
}

bool scareKindFromName(std::string_view name, ScareKind& out) {
    for (std::size_t i = 0; i < kScareKinds; ++i) {
        const ScareKind k = static_cast<ScareKind>(i);
        if (name == scareKindName(k)) {
            out = k;
            return true;
        }
    }
    return false;
}

bool scareWorldFromName(std::string_view name, ScareWorld& out) {
    for (std::size_t i = 0; i < static_cast<std::size_t>(ScareWorld::Count); ++i) {
        const ScareWorld w = static_cast<ScareWorld>(i);
        if (name == scareWorldName(w)) {
            out = w;
            return true;
        }
    }
    return false;
}

int scareWindowCap(ScareWorld w) { return w == ScareWorld::Unwritten ? 3 : 2; }
double scareGapSeconds(ScareWorld w) { return w == ScareWorld::Unwritten ? 600.0 : 900.0; }

bool scareMomentFromId(std::string_view id, ScareMoment& out) {
    constexpr std::string_view kPrefix = "scare_";
    if (id.size() < kPrefix.size() + 3 || id.substr(0, kPrefix.size()) != kPrefix) return false;
    unsigned n = 0;
    for (const char c : id.substr(kPrefix.size())) {
        if (c < '0' || c > '9') return false;
        n = n * 10u + static_cast<unsigned>(c - '0');
        if (n > kScareScriptedMax) return false;
    }
    if (n == 0) return false;
    out = ScareMoment{};
    out.number = static_cast<std::uint16_t>(n);
    return true;
}

ScareMoment scareAmbient(ScareKind kind, std::uint16_t place) {
    ScareMoment m;
    m.ambient = true;
    m.ambientKind = kind;
    m.number = place;
    return m;
}

bool scareMomentValid(const ScareMoment& m) {
    if (!m.ambient) return m.number >= 1 && m.number <= kScareScriptedMax;
    return m.ambientKind < ScareKind::Count && m.number < kScareAmbientPlaces;
}

namespace {

std::size_t seenBit(const ScareMoment& m) {
    if (!m.ambient) return m.number;
    return (kScareScriptedMax + 1u) + static_cast<std::size_t>(m.ambientKind) * kScareAmbientPlaces + m.number;
}

} // namespace

void ScarePacer::advance(double seconds) {
    if (std::isfinite(seconds) && seconds > 0.0) now_ += seconds;
}

void ScarePacer::markQuiet() { quietAt_ = now_; }

int ScarePacer::inWindow() const {
    int n = 0;
    for (std::size_t i = 0; i < count_; ++i)
        if (now_ - times_[i] < kScareWindowSeconds) ++n;
    return n;
}

double ScarePacer::lastTime() const {
    if (count_ == 0) return -1.0;
    return times_[(head_ + kScareRing - 1) % kScareRing];
}

bool ScarePacer::seen(const ScareMoment& m) const { return scareMomentValid(m) && seen_.test(seenBit(m)); }

bool ScarePacer::canFire(ScareKind kind, ScareWorld world, bool scripted, const ScareMoment& m) const {
    if (finale_) return false;
    if (setting_ == ScareSetting::Off && !scripted) return false;
    if (!scareMomentValid(m) || seen(m)) return false;
    if (now_ - quietAt_ < kScareQuietSeconds) return false;
    if (kind == lastKind_) return false;
    if (count_ > 0 && now_ - lastTime() < scareGapSeconds(world)) return false;
    if (inWindow() >= scareWindowCap(world)) return false;
    return true;
}

ScareForm ScarePacer::form(bool scripted, bool allowed) const {
    if (!allowed || setting_ == ScareSetting::Off) return scripted ? ScareForm::Calm : ScareForm::None;
    return setting_ == ScareSetting::Soft ? ScareForm::Soft : ScareForm::Full;
}

void ScarePacer::fire(ScareKind kind, ScareForm f) {
    if (f != ScareForm::Full && f != ScareForm::Soft) return;
    times_[head_] = now_;
    kinds_[head_] = kind;
    head_ = static_cast<std::uint8_t>((head_ + 1u) % kScareRing);
    if (count_ < kScareRing) ++count_;
    lastKind_ = kind;
}

ScareForm ScarePacer::play(ScareKind kind, ScareWorld world, bool scripted, const ScareMoment& m) {
    // A seen moment never plays again, in any form.
    if (seen(m) || !scareMomentValid(m)) return ScareForm::None;
    const ScareForm f = form(scripted, canFire(kind, world, scripted, m));
    fire(kind, f);
    return f;
}

ScareForm ScarePacer::hallucination(bool flaggedAsScare, const ScareMoment& m) {
    if (!flaggedAsScare) return ScareForm::Unpaced;
    return play(ScareKind::Hallucination, ScareWorld::Unwritten, true, m);
}

void ScarePacer::complete(const ScareMoment& m) {
    if (scareMomentValid(m)) seen_.set(seenBit(m));
}

void ScarePacer::save(ScareSave& out) const {
    out.playSeconds = now_;
    out.times = times_;
    out.kinds = kinds_;
    out.count = count_;
    out.head = head_;
    out.lastKind = lastKind_;
    out.seenHex.clear();
    std::size_t last = 0; // one past the last set bit
    for (std::size_t i = 0; i < kScareSeenBits; ++i)
        if (seen_.test(i)) last = i + 1;
    const std::size_t digits = (last + 3) / 4;
    out.seenHex.reserve(digits);
    for (std::size_t d = 0; d < digits; ++d) {
        unsigned v = 0;
        for (unsigned b = 0; b < 4; ++b) {
            const std::size_t i = d * 4 + b;
            if (i < kScareSeenBits && seen_.test(i)) v |= 1u << b;
        }
        out.seenHex += "0123456789abcdef"[v];
    }
}

bool ScarePacer::load(const ScareSave& in) {
    if (!std::isfinite(in.playSeconds) || in.playSeconds < 0.0) return false;
    if (in.count > kScareRing || in.head >= kScareRing) return false;
    if (in.lastKind > ScareKind::Count) return false;
    for (std::size_t i = 0; i < kScareRing; ++i) {
        if (!std::isfinite(in.times[i]) || in.times[i] < 0.0 || in.kinds[i] > ScareKind::Count) return false;
    }
    if (in.seenHex.size() * 4 > kScareSeenBits + 3) return false;
    std::bitset<kScareSeenBits> bits;
    for (std::size_t d = 0; d < in.seenHex.size(); ++d) {
        const char c = in.seenHex[d];
        unsigned v = 0;
        if (c >= '0' && c <= '9') v = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') v = static_cast<unsigned>(c - 'a' + 10);
        else return false;
        for (unsigned b = 0; b < 4; ++b) {
            if (!(v & (1u << b))) continue;
            const std::size_t i = d * 4 + b;
            if (i >= kScareSeenBits) return false;
            bits.set(i);
        }
    }
    now_ = in.playSeconds;
    times_ = in.times;
    kinds_ = in.kinds;
    count_ = in.count;
    head_ = in.head;
    lastKind_ = in.lastKind;
    seen_ = bits;
    quietAt_ = -1e30;
    return true;
}

// ---- placements ----

bool importScarePlacements(std::string_view json, ScarePlacementTable& out, std::string* error) {
    out = ScarePlacementTable{};
    std::vector<std::string> errors;
    const auto finish = [&]() {
        if (errors.empty()) return true;
        out = ScarePlacementTable{};
        if (error) {
            error->clear();
            for (const std::string& e : errors) {
                if (!error->empty()) *error += '\n';
                *error += e;
            }
        }
        return false;
    };
    const auto unknownKeys = [&](JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys) {
        for (std::uint32_t k = 0; k < j.size(); ++k) {
            const std::string_view key = j.key(k);
            bool known = false;
            for (const std::string_view w : keys) known = known || key == w;
            if (!known) errors.push_back((path.empty() ? std::string() : path + ": ") + "unknown key: " + std::string(key));
        }
    };

    JsonDocument doc;
    std::string err;
    if (!doc.parse(json, &err)) {
        errors.push_back("placements file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("placements file: expected an object");
        return finish();
    }
    unknownKeys(root, "", {"version", "placements"});
    if (const JsonValue v = root["version"]; !(v.isInteger() && v.integer() == 1)) errors.push_back("version: expected 1");
    const JsonValue list = root["placements"];
    if (!list.isArray()) {
        errors.push_back("placements: expected an array");
        return finish();
    }
    if (list.size() > kScarePlacementsMax) {
        errors.push_back("placements: more than " + std::to_string(kScarePlacementsMax) + " rows");
        return finish();
    }
    ScarePlacementTable table;
    for (std::uint32_t i = 0; i < list.size(); ++i) {
        const JsonValue row = list[i];
        const std::string path = "placements[" + std::to_string(i) + "]";
        if (!row.isObject()) {
            errors.push_back(path + ": expected an object");
            continue;
        }
        unknownKeys(row, path, {"id", "world", "kind", "minute", "finale"});
        ScarePlacement p;
        if (!row["id"].isString() || !scareMomentFromId(row["id"].string(), p.moment)) {
            errors.push_back(path + ".id: expected scare_001 .. scare_255");
            continue;
        }
        for (std::size_t r = 0; r < table.count; ++r)
            if (table.rows[r].moment == p.moment) errors.push_back(path + ".id: duplicate " + std::string(row["id"].string()));
        if (!row["world"].isString() || !scareWorldFromName(row["world"].string(), p.world))
            errors.push_back(path + ".world: expected solhaven, lattice or unwritten");
        if (!row["kind"].isString() || !scareKindFromName(row["kind"].string(), p.kind))
            errors.push_back(path + ".kind: expected exploration, scene, fight, hallucination or map");
        const JsonValue minute = row["minute"];
        if (!minute.isNumber() || !std::isfinite(minute.number()) || minute.number() < 0.0)
            errors.push_back(path + ".minute: expected a number of play minutes, 0 or more");
        else
            p.minute = minute.number();
        if (row.has("finale")) {
            if (!row["finale"].isBool()) errors.push_back(path + ".finale: expected true or false");
            else p.finale = row["finale"].boolean();
        }
        table.rows[table.count++] = p;
    }
    if (!errors.empty()) return finish();
    out = table;
    return true;
}

std::vector<std::string> checkScarePlacements(const ScarePlacementTable& table) {
    std::vector<std::string> problems;
    std::array<std::size_t, kScarePlacementsMax> order{};
    for (std::size_t i = 0; i < table.count; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(table.count),
                     [&](std::size_t a, std::size_t b) { return table.rows[a].minute < table.rows[b].minute; });
    ScarePacer pacer;
    pacer.markQuiet(); // a new game starts like a load
    for (std::size_t n = 0; n < table.count; ++n) {
        const ScarePlacement& p = table.rows[order[n]];
        char id[16];
        std::snprintf(id, sizeof id, "scare_%03u", static_cast<unsigned>(p.moment.number));
        pacer.advance(p.minute * 60.0 - pacer.playSeconds());
        if (p.finale) {
            problems.push_back(std::string(id) + ": placed in the Finale");
            continue;
        }
        const ScareForm f = pacer.play(p.kind, p.world, true, p.moment);
        if (f != ScareForm::Full) {
            const double last = pacer.lastTime();
            std::string why = pacer.seen(p.moment)            ? "already seen"
                              : pacer.playSeconds() < kScareQuietSeconds ? "in the first 3 minutes"
                              : p.kind == pacer.lastKind() ? "same kind as the last scare"
                              : (last >= 0.0 && pacer.playSeconds() - last < scareGapSeconds(p.world))
                                  ? "inside the gap"
                                  : "the window is full";
            problems.push_back(std::string(id) + " (" + scareWorldName(p.world) + ", " + scareKindName(p.kind) +
                               ", minute " + std::to_string(p.minute) + "): " + why);
        }
        pacer.complete(p.moment);
    }
    return problems;
}

} // namespace ftcore
