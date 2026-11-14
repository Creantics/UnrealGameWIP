#include "ftcore/LeadSwitch.h"

#include "ftcore/Json.h"

#include <vector>

namespace ftcore {

bool importLeadSwitchTuning(std::string_view json, LeadSwitchTuning& out, std::string* error) {
    out = LeadSwitchTuning{};
    std::vector<std::string> errors;
    const auto finish = [&]() {
        if (errors.empty()) return true;
        out = LeadSwitchTuning{};
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
        errors.push_back("lead switch file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("lead switch file: expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !(v.isInteger() && v.integer() == 1)) errors.push_back("version: expected 1");

    LeadSwitchTuning t;
    if (const JsonValue v = root["leadSwitchCooldownSeconds"]; !v.isAbsent()) {
        if (!v.isNumber() || !validLeadSwitchCooldown(v.number()))
            errors.push_back("leadSwitchCooldownSeconds: expected a number in 0..5");
        else
            t.cooldownSeconds = v.number();
    }
    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view k = root.key(i);
        if (k != "version" && k != "leadSwitchCooldownSeconds") errors.push_back(std::string(k) + ": unknown field");
    }
    if (!errors.empty()) return finish();
    out = t;
    return true;
}

} // namespace ftcore
