#include "ftcore/Emotion.h"

#include "ftcore/Json.h"

#include <algorithm>

namespace ftcore {

namespace {

bool setError(std::string* error, std::string msg) {
    if (error) *error = std::move(msg);
    return false;
}

int findExpression(const FaceShapeSet& set, std::string_view name) {
    for (std::size_t i = 0; i < set.expressions.size(); ++i)
        if (set.expressions[i].name == name) return static_cast<int>(i);
    return -1;
}

// pose(def) * k added into out, on every shape or only on Upper / Eyes ones.
void addPose(const FaceShapeSet& set, const EmotionDef& def, float k, bool upperOnly, FaceWeights& out) {
    const FaceWeights& base = set.expressions[static_cast<std::size_t>(def.base)].weights;
    const std::size_t n = std::min(set.size(), kMaxFaceShapes);
    for (std::size_t s = 0; s < n; ++s) {
        if (upperOnly && set.layers[s] != FaceLayer::Upper && set.layers[s] != FaceLayer::Eyes) continue;
        out[s] += k * (def.weight * base[s] + def.modifiers[s]);
    }
}

} // namespace

const EmotionDef* EmotionMap::find(std::string_view name) const {
    for (const EmotionDef& e : emotions)
        if (e.name == name) return &e;
    return nullptr;
}

bool EmotionMap::add(const FaceShapeSet& set, std::string_view name, std::string_view base, float weight,
                     std::span<const FaceShapeWeight> modifiers, std::string* error) {
    const std::string n(name);
    if (name.empty()) return setError(error, "emotion without a name");
    if (find(name)) return setError(error, "emotion '" + n + "' is listed twice");
    EmotionDef def;
    def.name = n;
    def.base = findExpression(set, base);
    if (def.base < 0) return setError(error, "emotion '" + n + "' names unknown expression '" + std::string(base) + "'");
    def.weight = weight;
    for (const auto& [shape, w] : modifiers) {
        const int s = set.find(shape);
        if (s < 0 || s >= static_cast<int>(kMaxFaceShapes))
            return setError(error, "emotion '" + n + "' names unknown shape '" + std::string(shape) + "'");
        def.modifiers[static_cast<std::size_t>(s)] = w;
    }
    emotions.push_back(std::move(def));
    return true;
}

float emotionIntensityWeight(int intensity, float cap) {
    const int i = std::clamp(intensity, 1, 5);
    return std::min(0.2f * static_cast<float>(i), std::max(0.f, cap));
}

bool resolveEmotion(const FaceShapeSet& set, const EmotionMap& map, std::string_view primary,
                    std::string_view secondary, int intensity, float cap, FacePreset& out) {
    out.weights.fill(0.f);
    out.tongueDeg = {};
    bool ok = true;
    if (!primary.empty()) {
        if (const EmotionDef* p = map.find(primary)) addPose(set, *p, emotionIntensityWeight(intensity, cap), false, out.weights);
        else ok = false;
    }
    if (!secondary.empty()) {
        if (const EmotionDef* s = map.find(secondary)) addPose(set, *s, kSecondaryWeight, true, out.weights);
        else ok = false;
    }
    for (float& w : out.weights) w = std::clamp(w, 0.f, 1.f);
    return ok;
}

bool importEmotionMap(std::string_view json, const FaceShapeSet& set, EmotionMap& out, std::string* error) {
    out = EmotionMap{};
    const auto fail = [&](std::string msg) {
        out = EmotionMap{};
        return setError(error, "emotion_map.json: " + msg);
    };
    JsonDocument doc;
    std::string err;
    if (!doc.parse(json, &err)) return fail(err);
    const JsonValue list = doc.root()["emotions"];
    if (!list.isArray()) return fail("emotions: expected an array");
    std::vector<FaceShapeWeight> mods;
    for (std::uint32_t i = 0; i < list.size(); ++i) {
        const JsonValue e = list[i];
        const std::string_view name = e["name"].string();
        const JsonValue weight = e["weight"], add = e["add"];
        if (!weight.isAbsent() && !weight.isNumber()) return fail("emotion '" + std::string(name) + "': weight is not a number");
        if (!add.isAbsent() && !add.isObject()) return fail("emotion '" + std::string(name) + "': add must be an object");
        mods.clear();
        for (std::uint32_t k = 0; k < add.size(); ++k) {
            if (!add.value(k).isNumber())
                return fail("emotion '" + std::string(name) + "': weight of '" + std::string(add.key(k)) + "' is not a number");
            mods.emplace_back(add.key(k), static_cast<float>(add.value(k).number()));
        }
        if (!out.add(set, name, e["base"].string(), static_cast<float>(weight.number(1.0)), mods, &err)) return fail(err);
    }
    return true;
}

} // namespace ftcore
