#include "ftcore/LoneCarry.h"

#include "ftcore/Json.h"

#include <type_traits>
#include <vector>

namespace ftcore {

bool importLoneCarry(std::string_view json, LoneTuning& out, std::string* error) {
    out = LoneTuning{};
    std::vector<std::string> errors;
    const auto finish = [&]() {
        if (errors.empty()) return true;
        out = LoneTuning{};
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
        errors.push_back("lone carry file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("lone carry file: expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !(v.isInteger() && v.integer() == 1)) errors.push_back("version: expected 1");

    LoneTuning t;
    if (const JsonValue v = root["cap"]; !v.isAbsent()) {
        if (!v.isInteger() || v.integer() < 1 || v.integer() > kMaxLoad) errors.push_back("cap: expected a whole 1..100");
        else t.cap = static_cast<int>(v.integer());
    }
    const auto real = [&](const char* key, float& to, double lo, double hi) {
        const JsonValue v = root[key];
        if (v.isAbsent()) return;
        if (!v.isNumber() || !(v.number() >= lo && v.number() <= hi)) {
            errors.push_back(std::string(key) + ": expected a number in its range");
            return;
        }
        to = static_cast<float>(v.number());
    };
    real("gain", t.gain, 1.0, 3.0);
    real("release", t.release, 1.0, 3.0);
    real("taken", t.taken, 1.0, 2.0);

    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view k = root.key(i);
        if (k != "version" && k != "cap" && k != "gain" && k != "release" && k != "taken")
            errors.push_back(std::string(k) + ": unknown field");
    }
    if (errors.empty() && !validLoneTuning(t)) errors.push_back("lone carry: a value out of its range");
    if (!errors.empty()) return finish();
    out = t;
    return true;
}

bool importLonePool(std::string_view json, LonePoolTuning& out, std::string* error) {
    out = LonePoolTuning{};
    std::vector<std::string> errors;
    const auto finish = [&]() {
        if (errors.empty()) return true;
        out = LonePoolTuning{};
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
        errors.push_back("lone pool file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("lone pool file: expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !(v.isInteger() && v.integer() == 1)) errors.push_back("version: expected 1");

    LonePoolTuning t;
    const auto whole = [&](const char* key, std::int64_t lo, std::int64_t hi, auto& to) {
        const JsonValue v = root[key];
        if (v.isAbsent()) return;
        if (!v.isInteger() || v.integer() < lo || v.integer() > hi) {
            errors.push_back(std::string(key) + ": expected a whole " + std::to_string(lo) + ".." + std::to_string(hi));
            return;
        }
        to = static_cast<std::remove_reference_t<decltype(to)>>(v.integer());
    };
    whole("maxBonus", 0, 400, t.maxBonus);
    whole("halfPool", 1, 1000000, t.halfPool);
    whole("figurePermille", 50, 400, t.figurePermille);
    whole("maxFigures", 0, 2, t.maxFigures);

    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view k = root.key(i);
        if (k != "version" && k != "maxBonus" && k != "halfPool" && k != "figurePermille" && k != "maxFigures")
            errors.push_back(std::string(k) + ": unknown field");
    }
    if (errors.empty() && !validLonePoolTuning(t)) errors.push_back("lone pool: a value out of its range");
    if (!errors.empty()) return finish();
    out = t;
    return true;
}

} // namespace ftcore
