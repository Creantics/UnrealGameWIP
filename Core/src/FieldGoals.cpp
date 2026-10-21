#include "ftcore/FieldGoals.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace ftcore {

namespace {

void goalUnknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys,
                     std::vector<std::string>* warnings) {
    if (!warnings) return;
    for (std::uint32_t k = 0; k < j.size(); ++k) {
        const std::string_view key = j.key(k);
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            warnings->push_back((path.empty() ? std::string() : path + ": ") + "unknown key: " + std::string(key));
    }
}

// {"x": .., "y": ..} inside the walkable box.
bool goalPoint(JsonValue j, const std::string& path, Vec2& out, std::vector<std::string>& errors,
               std::vector<std::string>* warnings) {
    if (!j.isObject()) {
        errors.push_back(path + ": expected an object with x and y");
        return false;
    }
    goalUnknownKeys(j, path, {"x", "y"}, warnings);
    const JsonValue x = j["x"];
    const JsonValue y = j["y"];
    const double dx = x.number(1e30);
    const double dy = y.number(1e30);
    bool good = true;
    if (!x.isNumber() || !std::isfinite(dx) || dx < FieldGoalTable::kMinX || dx > FieldGoalTable::kMaxX) {
        errors.push_back(path + ".x: expected a number in " + std::to_string(FieldGoalTable::kMinX) + ".." +
                         std::to_string(FieldGoalTable::kMaxX));
        good = false;
    }
    if (!y.isNumber() || !std::isfinite(dy) || std::fabs(dy) > FieldGoalTable::kMaxAbsY) {
        errors.push_back(path + ".y: expected a number in " + std::to_string(-FieldGoalTable::kMaxAbsY) + ".." +
                         std::to_string(FieldGoalTable::kMaxAbsY));
        good = false;
    }
    if (good) out = {static_cast<float>(dx), static_cast<float>(dy)};
    return good;
}

} // namespace

const FieldGoalEntry* FieldGoalTable::findEntry(std::string_view scene, std::uint32_t beat, std::uint32_t step) const {
    for (const FieldGoalEntry& e : entries)
        if (e.beat == beat && e.step == step && e.scene == scene) return &e;
    return nullptr;
}

bool importFieldGoalTable(std::string_view json, FieldGoalTable& out, std::string* error,
                          std::vector<std::string>* warnings) {
    out = FieldGoalTable{};
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
        errors.push_back("field goal file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("field goal file: expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !v.isAbsent() && !(v.isInteger() && v.integer() == 1))
        errors.push_back("version: expected 1");
    goalUnknownKeys(root, "", {"version", "default", "goals"}, warnings);

    FieldGoalTable table;
    const JsonValue def = root["default"];
    if (!def.isAbsent()) {
        if (!def.isObject()) {
            errors.push_back("default: expected an object");
        } else {
            goalUnknownKeys(def, "default", {"goal"}, warnings);
            table.hasFallback = goalPoint(def["goal"], "default.goal", table.fallback.goal, errors, warnings);
        }
    }

    const JsonValue list = root["goals"];
    if (!list.isAbsent() && !list.isArray()) errors.push_back("goals: expected an array");
    for (std::uint32_t i = 0; i < list.size() && list.isArray(); ++i) {
        const JsonValue j = list[i];
        const std::string path = "goals[" + std::to_string(i) + "]";
        if (!j.isObject()) {
            errors.push_back(path + ": expected an object");
            continue;
        }
        goalUnknownKeys(j, path, {"scene", "beat", "step", "goal", "via"}, warnings);
        FieldGoalEntry e;
        e.scene = std::string(j["scene"].string());
        bool good = !e.scene.empty();
        if (!good) errors.push_back(path + ".scene: expected a scene id");
        const JsonValue b = j["beat"];
        const JsonValue s = j["step"];
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
        if (!goalPoint(j["goal"], path + ".goal", e.goal, errors, warnings)) good = false;
        if (j.has("via")) {
            e.hasVia = goalPoint(j["via"], path + ".via", e.via, errors, warnings);
            if (!e.hasVia) good = false;
        }
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
