#include "ftcore/FaceImport.h"

#include "ftcore/Json.h"

#include <vector>

namespace ftcore {

namespace {

bool setError(std::string* error, std::string what) {
    if (error) *error = "face_presets.json: " + std::move(what);
    return false;
}

bool readPreset(FaceShapeSet& set, JsonValue j, bool viseme, std::vector<FaceShapeWeight>& scratch,
                std::string* error) {
    const std::string_view name = j["name"].string();
    if (name.empty()) return setError(error, std::string(viseme ? "viseme" : "expression") + " without a name");
    const JsonValue shapes = j["shapes"];
    if (!shapes.isObject()) return setError(error, "face preset '" + std::string(name) + "': shapes must be an object");
    scratch.clear();
    for (std::uint32_t i = 0; i < shapes.size(); ++i) {
        const JsonValue w = shapes.value(i);
        if (!w.isNumber())
            return setError(error, "face preset '" + std::string(name) + "': weight of '" + std::string(shapes.key(i)) +
                                       "' is not a number");
        scratch.emplace_back(shapes.key(i), static_cast<float>(w.number()));
    }
    std::string err;
    if (!viseme) return set.addExpression(name, scratch, &err) || setError(error, err);
    FaceVec3 tongue;
    const JsonValue t = j["tongue_deg"];
    if (!t.isAbsent()) {
        if (!t.isArray() || t.size() != 3 || !t[0].isNumber() || !t[1].isNumber() || !t[2].isNumber())
            return setError(error, "face preset '" + std::string(name) + "': tongue_deg needs 3 values");
        tongue = {static_cast<float>(t[0].number()), static_cast<float>(t[1].number()),
                  static_cast<float>(t[2].number())};
    }
    return set.addViseme(name, scratch, tongue, &err) || setError(error, err);
}

bool importInto(std::string_view json, FaceShapeSet& set, std::string* error) {
    JsonDocument doc;
    std::string err;
    if (!doc.parse(json, &err)) return setError(error, err);
    const JsonValue root = doc.root();
    const JsonValue shapes = root["shapes"], visemes = root["visemes"], expressions = root["expressions"];
    if (!shapes.isArray()) return setError(error, "shapes: expected an array");
    if (!visemes.isArray()) return setError(error, "visemes: expected an array");
    if (!expressions.isArray()) return setError(error, "expressions: expected an array");

    for (std::uint32_t i = 0; i < shapes.size(); ++i) {
        const std::string_view name = shapes[i]["name"].string();
        FaceLayer layer = FaceLayer::Upper;
        if (!faceLayerFromName(shapes[i]["layer"].string(), layer))
            return setError(error, "face shape '" + std::string(name) + "' has an unknown layer");
        if (!set.addShape(name, layer, &err)) return setError(error, err);
    }
    std::vector<FaceShapeWeight> scratch;
    for (std::uint32_t i = 0; i < visemes.size(); ++i)
        if (!readPreset(set, visemes[i], true, scratch, error)) return false;
    for (std::uint32_t i = 0; i < expressions.size(); ++i)
        if (!readPreset(set, expressions[i], false, scratch, error)) return false;
    set.jawMaxDeg = static_cast<float>(root["jaw_max_deg"].number(25.0));
    if (!set.finish(&err)) return setError(error, err);
    return true;
}

} // namespace

bool importFaceShapeSet(std::string_view json, FaceShapeSet& out, std::string* error) {
    out = FaceShapeSet{};
    if (importInto(json, out, error)) return true;
    out = FaceShapeSet{};
    return false;
}

} // namespace ftcore
