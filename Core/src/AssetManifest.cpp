#include "ftcore/AssetManifest.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <iterator>

namespace ftcore {

namespace {

constexpr const char* kStateNames[] = {"missing", "failed", "delivered", "imported"};

std::string amfStr(std::string_view s) { return std::string(s); }

} // namespace

const ModelEntry* ModelTable::find(std::string_view id) const {
    for (const ModelEntry& model : models)
        if (model.id == id) return &model;
    return nullptr;
}

bool importModelManifest(std::string_view json, ModelTable& out, std::string* error, std::vector<std::string>* warnings) {
    out = ModelTable{};
    std::vector<std::string> errors;
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(json, &parseError)) {
        errors.push_back("models file: " + parseError);
    } else if (!doc.root().isObject()) {
        errors.push_back("models file: expected an object");
    } else {
        const JsonValue root = doc.root();
        if (warnings)
            for (std::uint32_t k = 0; k < root.size(); ++k)
                if (root.key(k) != "version" && root.key(k) != "models") warnings->push_back("unknown key: " + amfStr(root.key(k)));
        if (root["version"].integer(0) != 1) errors.push_back("version: expected 1");
        const JsonValue list = root["models"];
        if (!list.isArray()) errors.push_back("models: expected an array");
        ModelTable table;
        for (std::uint32_t i = 0; i < list.size() && list.isArray(); ++i) {
            const JsonValue j = list[i];
            const std::string path = "models[" + std::to_string(i) + "]";
            if (!j.isObject()) {
                errors.push_back(path + ": expected an object");
                continue;
            }
            if (warnings)
                for (std::uint32_t k = 0; k < j.size(); ++k) {
                    const std::string_view key = j.key(k);
                    if (key != "id" && key != "kind" && key != "role" && key != "pack" && key != "file")
                        warnings->push_back(path + ": unknown key: " + amfStr(key));
                }
            ModelEntry model;
            model.id = amfStr(j["id"].string());
            if (model.id.empty()) errors.push_back(path + ".id: expected a model id");
            else if (table.find(model.id)) errors.push_back(path + ".id: duplicate model " + model.id);

            const std::string_view kind = j["kind"].string();
            if (kind == "character") model.kind = ModelKind::Character;
            else if (kind == "prop") model.kind = ModelKind::Prop;
            else errors.push_back(path + ".kind: expected character or prop");

            const JsonValue role = j["role"];
            if (model.kind == ModelKind::Character) {
                model.role = AssetRole::Supporting;
                if (!role.isAbsent()) {
                    if (role.string() == "lead") model.role = AssetRole::Lead;
                    else if (role.string() != "supporting") errors.push_back(path + ".role: expected lead or supporting");
                }
            } else if (!role.isAbsent()) {
                errors.push_back(path + ".role: only characters have a role");
            }

            const JsonValue pack = j["pack"];
            if (!pack.isAbsent()) {
                model.pack = assetPackFromName(pack.string());
                if (model.pack == AssetPack::None && pack.string() != "none")
                    errors.push_back(path + ".pack: unknown pack " + amfStr(pack.string()));
            }

            const JsonValue file = j["file"];
            if (file.isAbsent()) model.file = model.id + ".glb";
            else if (!file.isString() || file.string().empty()) errors.push_back(path + ".file: expected a path");
            else model.file = amfStr(file.string());

            table.models.push_back(std::move(model));
        }
        if (errors.empty()) out = std::move(table);
    }
    if (errors.empty()) return true;
    if (error) {
        error->clear();
        for (const std::string& e : errors) *error += e + "\n";
    }
    return false;
}

AssetRules modelRules(const ModelEntry& model) {
    return model.kind == ModelKind::Character ? characterRules(model.pack, model.role) : propRules(model.pack);
}

std::vector<std::string> voiceLineIds(const StoryDatabase& db) {
    std::vector<std::string> ids;
    for (std::uint32_t s = 0; s < db.sceneCount(); ++s) {
        const Scene& scene = db.scene(s);
        for (std::uint32_t b = 0; b < scene.beatCount; ++b) {
            const Beat& beat = db.beat(scene, b);
            if (beat.type != BeatType::Line) continue;
            const std::string_view id = db.view(beat.id);
            if (!id.empty() && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.emplace_back(id);
        }
    }
    return ids;
}

const char* assetStateName(AssetState state) {
    const auto i = static_cast<std::size_t>(state);
    return i < std::size(kStateNames) ? kStateNames[i] : "?";
}

AssetState assetState(bool filePresent, std::uint32_t errors, bool imported) {
    if (!filePresent) return AssetState::Missing;
    if (errors > 0) return AssetState::Failed;
    return imported ? AssetState::Imported : AssetState::Delivered;
}

std::uint32_t AssetTally::total() const {
    std::uint32_t n = 0;
    for (const std::uint32_t c : counts) n += c;
    return n;
}

std::string AssetTally::summary() const {
    return std::to_string(total()) + " expected: " + std::to_string(count(AssetState::Imported)) + " imported, " +
           std::to_string(count(AssetState::Delivered)) + " delivered, " + std::to_string(count(AssetState::Missing)) +
           " missing, " + std::to_string(count(AssetState::Failed)) + " failed";
}

} // namespace ftcore
