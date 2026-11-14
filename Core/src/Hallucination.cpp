#include "ftcore/Hallucination.h"

#include "ftcore/Json.h"

#include <vector>

namespace ftcore {
namespace {

// Joins `errors` into `*error`; false if there were any.
bool report(const std::vector<std::string>& errors, std::string* error) {
    if (errors.empty()) return true;
    if (error) {
        error->clear();
        for (const std::string& e : errors) {
            if (!error->empty()) *error += '\n';
            *error += e;
        }
    }
    return false;
}

bool parseRoot(std::string_view json, JsonDocument& doc, const char* what, std::vector<std::string>& errors) {
    std::string err;
    if (!doc.parse(json, &err)) {
        errors.push_back(std::string(what) + ": " + err);
        return false;
    }
    if (!doc.root().isObject()) {
        errors.push_back(std::string(what) + ": expected an object");
        return false;
    }
    if (const JsonValue v = doc.root()["version"]; !(v.isInteger() && v.integer() == 1)) errors.push_back("version: expected 1");
    return true;
}

bool bandOf(std::string_view s, IntensityBand& out) {
    static constexpr std::string_view kNames[] = {"none", "low", "mid", "high"};
    for (std::size_t i = 0; i < 4; ++i)
        if (s == kNames[i]) {
            out = static_cast<IntensityBand>(i);
            return true;
        }
    return false;
}

} // namespace

bool importTragedies(std::string_view json, TragedyTable& out, std::string* error) {
    out = TragedyTable{};
    std::vector<std::string> errors;
    JsonDocument doc;
    if (!parseRoot(json, doc, "tragedies file", errors)) return report(errors, error);
    const JsonValue root = doc.root();
    TragedyTable t;

    const JsonValue places = root["places"];
    if (!places.isArray() || places.size() == 0 || places.size() > kMaxTragedyPlaces) {
        errors.push_back("places: expected 1..32 place types");
    } else {
        for (std::uint32_t i = 0; i < places.size(); ++i) {
            const std::string_view p = places[i].string();
            if (p.empty() || tragedyPlaceBit(t, p) != 0) errors.push_back("places: each a distinct, non-empty string");
            else t.places.emplace_back(p);
        }
    }

    const JsonValue keys = root["keys"];
    if (!keys.isArray() || keys.size() > kMaxTragedyKeys) {
        errors.push_back("keys: expected an array of at most 64");
    } else {
        for (std::uint32_t i = 0; i < keys.size(); ++i) {
            const JsonValue k = keys[i];
            const std::string at = "keys[" + std::to_string(i) + "]";
            TragedyDef d;
            d.id = std::string(k["id"].string());
            if (d.id.empty() || tragedyKey(t, d.id) >= 0) errors.push_back(at + ".id: expected a distinct, non-empty id");
            const std::string_view lead = k["lead"].string();
            if (lead == "a") d.lead = kLeadA;
            else if (lead == "b") d.lead = kLeadB;
            else errors.push_back(at + ".lead: expected \"a\" or \"b\"");
            const JsonValue kp = k["places"];
            if (!kp.isArray() || kp.size() == 0) errors.push_back(at + ".places: expected at least one place type");
            for (std::uint32_t j = 0; j < kp.size(); ++j) {
                const std::uint32_t bit = tragedyPlaceBit(t, kp[j].string());
                if (bit == 0) errors.push_back(at + ".places: not one of the file's place types");
                d.places |= bit;
            }
            d.voice = std::string(k["voice"].string());
            d.figure = std::string(k["figure"].string());
            if (d.voice.empty() || d.figure.empty()) errors.push_back(at + ": expected a voice and a figure id");
            const JsonValue caused = k["caused"], lone = k["lone"];
            if (!(caused.isAbsent() || caused.isBool()) || !(lone.isAbsent() || lone.isBool()))
                errors.push_back(at + ": caused and lone are true or false");
            d.caused = caused.boolean();
            d.lone = lone.boolean();
            for (std::uint32_t j = 0; j < k.size(); ++j) {
                const std::string_view f = k.key(j);
                if (f != "id" && f != "lead" && f != "places" && f != "voice" && f != "figure" && f != "caused" &&
                    f != "lone")
                    errors.push_back(at + "." + std::string(f) + ": unknown field");
            }
            t.keys.push_back(std::move(d));
        }
    }
    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view k = root.key(i);
        if (k != "version" && k != "places" && k != "keys") errors.push_back(std::string(k) + ": unknown field");
    }
    if (!errors.empty()) return report(errors, error);
    out = std::move(t);
    return true;
}

bool importHallucinationTuning(std::string_view json, HallucinationTuning& out, std::string* error) {
    out = HallucinationTuning{};
    std::vector<std::string> errors;
    JsonDocument doc;
    if (!parseRoot(json, doc, "hallucination file", errors)) return report(errors, error);
    const JsonValue root = doc.root();
    HallucinationTuning t;
    struct Field {
        const char* key;
        float* to;
        double lo, hi;
    };
    const Field fields[] = {
        {"holdSeconds", &t.holdSeconds, 0.5, 30.0},       {"expireSeconds", &t.expireSeconds, 1.0, 600.0},
        {"gapSeconds", &t.gapSeconds, 0.0, 3600.0},       {"switchSeconds", &t.switchSeconds, 0.0, 600.0},
        {"refuseSeconds", &t.refuseSeconds, 0.0, 5.0},    {"refusalCooldownSeconds", &t.refusalCooldownSeconds, 0.0, 60.0},
        {"pullSeconds", &t.pullSeconds, 0.5, 60.0},       {"recentSeconds", &t.recentSeconds, 0.0, 36000.0},
        {"fewerGapScale", &t.fewerGapScale, 1.0, 10.0},     {"lonePoolFloor", &t.lonePoolFloor, 0.0, 1.0e9},
        {"counselCooldownSeconds", &t.counselCooldownSeconds, 0.0, 60.0}, // 0 to refusalCooldownSeconds' own maximum
        {"proxyReactSeconds", &t.proxyReactSeconds, 0.0, 6.0},
        {"escrowDelaySeconds", &t.escrowDelaySeconds, 0.0, 30.0},
        {"refuseOpenMinHold", &t.refuseOpenMinHold, 0.0, 30.0},
        {"refuseOpenBase", &t.refuseOpenBase, 0.0, 30.0},
        {"refuseOpenPerSecond", &t.refuseOpenPerSecond, 0.0, 10.0},
        {"refuseOpenHoldCap", &t.refuseOpenHoldCap, 0.0, 30.0},
        {"heldHealthCost", &t.heldHealthCost, 0.0, 1.0},
        {"heldShutSeconds", &t.heldShutSeconds, 0.0, 120.0},
        {"refuseOpenLockSeconds", &t.refuseOpenLockSeconds, 0.0, 30.0},
    };
    for (const Field& f : fields) {
        const JsonValue v = root[f.key];
        if (v.isAbsent()) continue;
        if (!v.isNumber() || !(v.number() >= f.lo && v.number() <= f.hi))
            errors.push_back(std::string(f.key) + ": expected a number in its range");
        else *f.to = static_cast<float>(v.number());
    }
    if (t.refuseOpenHoldCap < t.refuseOpenMinHold) errors.push_back("refuseOpenHoldCap: expected at least refuseOpenMinHold");
    if (const JsonValue v = root["actFloors"]; !v.isAbsent()) {
        if (!v.isArray() || v.size() != kMaxAct + 1) {
            errors.push_back("actFloors: expected 5 bands (acts 0 to 4)");
        } else {
            for (std::uint32_t i = 0; i < v.size(); ++i)
                if (!bandOf(v[i].string(), t.actFloors[i])) errors.push_back("actFloors: none, low, mid or high");
        }
    }
    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view k = root.key(i);
        bool known = k == "version" || k == "actFloors";
        for (const Field& f : fields) known = known || k == f.key;
        if (!known) errors.push_back(std::string(k) + ": unknown field");
    }
    if (!errors.empty()) return report(errors, error);
    out = t;
    return true;
}

} // namespace ftcore
