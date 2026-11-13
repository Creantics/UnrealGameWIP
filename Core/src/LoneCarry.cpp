#include "ftcore/LoneCarry.h"

#include "ftcore/Json.h"

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

} // namespace ftcore
