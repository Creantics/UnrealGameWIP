#include "ftcore/GiftSlot.h"

#include "ftcore/Json.h"

#include <vector>

namespace ftcore {

bool importGiftTuning(std::string_view json, GiftTuning& out, std::string* error) {
    out = GiftTuning{};
    std::vector<std::string> errors;
    const auto finish = [&]() {
        if (errors.empty()) return true;
        out = GiftTuning{};
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
        errors.push_back("gifts file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("gifts file: expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !(v.isInteger() && v.integer() == 1)) errors.push_back("version: expected 1");

    GiftTuning t;
    const auto real = [&](const char* key, double& to, double lo, double hi) {
        const JsonValue v = root[key];
        if (v.isAbsent()) return;
        if (!v.isNumber() || !(v.number() >= lo && v.number() <= hi)) {
            errors.push_back(std::string(key) + ": expected a number in its range");
            return;
        }
        to = v.number();
    };
    const auto share = [&](const char* key, float& to) {
        double d = static_cast<double>(to);
        const std::size_t before = errors.size();
        real(key, d, 0.0, 1.0);
        if (errors.size() == before) to = static_cast<float>(d);
    };
    const auto whole = [&](const char* key, int& to, int lo, int hi) {
        const JsonValue v = root[key];
        if (v.isAbsent()) return;
        if (!v.isInteger() || v.integer() < lo || v.integer() > hi) {
            errors.push_back(std::string(key) + ": expected a whole number in its range");
            return;
        }
        to = static_cast<int>(v.integer());
    };
    real("healCooldown", t.cooldown[static_cast<std::size_t>(Gift::Heal)], 0.0, 60.0);
    real("guardCooldown", t.cooldown[static_cast<std::size_t>(Gift::Guard)], 0.0, 60.0);
    real("countCooldown", t.cooldown[static_cast<std::size_t>(Gift::Count)], 0.0, 60.0);
    share("healShare", t.healShare);
    whole("healLoad", t.healLoad, 0, 100);
    whole("healsPerFight", t.healsPerFight, 0, 20);
    share("guardScale", t.guardScale);
    real("guardSeconds", t.guardSeconds, 0.0, 10.0);

    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view k = root.key(i);
        if (k != "version" && k != "healCooldown" && k != "guardCooldown" && k != "countCooldown" && k != "healShare" &&
            k != "healLoad" && k != "healsPerFight" && k != "guardScale" && k != "guardSeconds")
            errors.push_back(std::string(k) + ": unknown field");
    }
    if (errors.empty() && !validGiftTuning(t)) errors.push_back("gifts: a value out of its range");
    if (!errors.empty()) return finish();
    out = t;
    return true;
}

} // namespace ftcore
