#include "ftcore/CountLiftData.h"

#include "ftcore/Json.h"

#include <vector>

namespace ftcore {

bool importCountLift(std::string_view json, CountLiftData& out, std::string* error) {
    out = CountLiftData{};
    std::vector<std::string> errors;
    const auto finish = [&]() {
        if (errors.empty()) return true;
        out = CountLiftData{};
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
        errors.push_back("count lift file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("count lift file: expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !(v.isInteger() && v.integer() == 1)) errors.push_back("version: expected 1");

    CountLiftData d;
    const auto whole = [&](const char* key, int& to) {
        const JsonValue v = root[key];
        if (v.isAbsent()) return;
        if (!v.isInteger() || v.integer() < -1000000 || v.integer() > 1000000) {
            errors.push_back(std::string(key) + ": expected a whole number");
            return;
        }
        to = static_cast<int>(v.integer());
    };
    const auto real = [&](const char* key, double& to) {
        const JsonValue v = root[key];
        if (v.isAbsent()) return;
        if (!v.isNumber()) {
            errors.push_back(std::string(key) + ": expected a number");
            return;
        }
        to = v.number();
    };
    whole("countLoad", d.lift.countLoad);
    real("countLoadEvery", d.lift.countLoadEvery);
    whole("soloCountLoad", d.lift.soloCountLoad);
    whole("liftThreshold", d.lift.liftThreshold);
    double mult = d.lift.liftMultiplier;
    real("liftMultiplier", mult);
    d.lift.liftMultiplier = static_cast<float>(mult);
    whole("liftCost", d.lift.liftCost);
    double cap = d.blowCap;
    real("blowCap", cap);
    d.blowCap = static_cast<float>(cap);

    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view k = root.key(i);
        if (k != "version" && k != "countLoad" && k != "countLoadEvery" && k != "soloCountLoad" && k != "liftThreshold" &&
            k != "liftMultiplier" && k != "liftCost" && k != "blowCap")
            errors.push_back(std::string(k) + ": unknown field");
    }
    if (errors.empty() && !validLiftTuning(d.lift, CountTuning{})) errors.push_back("lift: a value out of its range");
    if (!(cap >= 0.0 && cap <= static_cast<double>(kMaxBlowCap)) || !validBlowCap(d.blowCap))
        errors.push_back("blowCap: expected 0 or 1..20");
    if (!errors.empty()) return finish();
    out = d;
    return true;
}

} // namespace ftcore
