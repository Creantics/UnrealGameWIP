#include "ftcore/StoryCast.h"

#include "ftcore/Json.h"
#include "ftcore/StoryDatabase.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

bool sameNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

} // namespace

bool buildFaceCurveMap(const FaceShapeSet& set, std::span<const std::string_view> poseNames,
                       std::span<const std::string_view> curveNames, std::span<const float> values, FaceCurveMap& out,
                       std::vector<std::string>* unmatched) {
    out = FaceCurveMap{};
    const std::size_t curves = curveNames.size();
    if (values.size() != poseNames.size() * curves || set.size() > 0xFFFF || curves > 0xFFFF) return false;
    out.curves.reserve(curves);
    for (const std::string_view c : curveNames) out.curves.emplace_back(c);
    for (std::size_t s = 0; s < set.size(); ++s) {
        std::size_t pose = poseNames.size();
        for (std::size_t p = 0; p < poseNames.size(); ++p)
            if (sameNoCase(poseNames[p], set.shapes[s])) {
                pose = p;
                break;
            }
        if (pose == poseNames.size()) {
            if (unmatched) unmatched->push_back(set.shapes[s]);
            continue;
        }
        bool moves = false;
        for (std::size_t c = 0; c < curves; ++c) {
            const float v = values[pose * curves + c];
            if (v == 0.f || !std::isfinite(v)) continue;
            out.terms.push_back({static_cast<std::uint16_t>(s), static_cast<std::uint16_t>(c), v});
            moves = true;
        }
        if (moves) ++out.mappedShapes;
    }
    std::stable_sort(out.terms.begin(), out.terms.end(),
                     [](const FaceCurveMap::Term& a, const FaceCurveMap::Term& b) { return a.curve < b.curve; });
    return true;
}

void writeFaceCurves(const FaceCurveMap& map, const FaceWeights& weights, std::span<float> out) {
    std::fill(out.begin(), out.end(), 0.f);
    for (const FaceCurveMap::Term& t : map.terms)
        if (t.curve < out.size() && t.shape < weights.size()) out[t.curve] += weights[t.shape] * t.weight;
    const std::size_t n = std::min(out.size(), map.curves.size());
    for (std::size_t c = 0; c < n; ++c) out[c] = std::clamp(out[c], 0.f, 1.f);
}

std::size_t sceneCastSpeakers(const StoryDatabase& db, std::uint32_t sceneIndex, const SpeakerCast& cast,
                              std::array<std::string_view, kMaxSceneCast>& out, std::uint32_t* dropped) {
    std::size_t n = 0;
    std::uint32_t over = 0;
    if (sceneIndex < db.sceneCount()) {
        const Scene& scene = db.scene(sceneIndex);
        for (std::uint32_t b = 0; b < scene.beatCount; ++b) {
            const Beat& beat = db.beat(scene, b);
            if (beat.type != BeatType::Line) continue;
            const std::string_view speaker = db.view(beat.speaker);
            if (speaker.empty() || cast.modelFor(speaker).empty()) continue;
            if (std::find(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n), speaker) !=
                out.begin() + static_cast<std::ptrdiff_t>(n))
                continue;
            if (n < kMaxSceneCast) {
                out[n++] = speaker;
            } else {
                bool seen = false;
                for (std::uint32_t e = 0; e < b && !seen; ++e) {
                    const Beat& earlier = db.beat(scene, e);
                    seen = earlier.type == BeatType::Line && db.view(earlier.speaker) == speaker;
                }
                if (!seen) ++over;
            }
        }
    }
    for (std::size_t i = n; i < kMaxSceneCast; ++i) out[i] = {};
    if (dropped) *dropped = over;
    return n;
}

const CastMark* CastMarks::find(std::string_view scene, std::string_view speaker) const {
    for (const Entry& e : entries)
        if (e.scene == scene && e.speaker == speaker) return &e.mark;
    return nullptr;
}

bool importCastMarks(std::string_view json, CastMarks& out, std::string* error) {
    out = CastMarks{};
    std::vector<std::string> errors;
    JsonDocument doc;
    std::string parseError;
    CastMarks marks;
    if (!doc.parse(json, &parseError)) {
        errors.push_back("cast marks file: " + parseError);
    } else if (!doc.root().isObject()) {
        errors.push_back("cast marks file: expected an object");
    } else {
        const JsonValue root = doc.root();
        if (root["version"].integer(0) != 1) errors.push_back("version: expected 1");
        const JsonValue scenes = root["scenes"];
        if (!scenes.isArray()) errors.push_back("scenes: expected an array");
        for (std::uint32_t i = 0; scenes.isArray() && i < scenes.size(); ++i) {
            const JsonValue s = scenes[i];
            const std::string path = "scenes[" + std::to_string(i) + "]";
            const std::string scene(s["scene"].string());
            if (scene.empty()) errors.push_back(path + ".scene: expected a scene id");
            const JsonValue list = s["marks"];
            if (!list.isArray()) errors.push_back(path + ".marks: expected an array");
            for (std::uint32_t m = 0; list.isArray() && m < list.size(); ++m) {
                const JsonValue j = list[m];
                const std::string mpath = path + ".marks[" + std::to_string(m) + "]";
                CastMarks::Entry e;
                e.scene = scene;
                e.speaker = std::string(j["speaker"].string());
                const JsonValue at = j["at"];
                if (e.speaker.empty()) errors.push_back(mpath + ".speaker: expected a script label");
                else if (marks.find(scene, e.speaker)) errors.push_back(mpath + ".speaker: marked twice in the scene");
                if (!at.isArray() || at.size() != 2 || at[0].type() != JsonType::Number ||
                    at[1].type() != JsonType::Number) {
                    errors.push_back(mpath + ".at: expected [x, y] in metres");
                } else {
                    e.mark.x = static_cast<float>(at[0].number());
                    e.mark.y = static_cast<float>(at[1].number());
                }
                const JsonValue yaw = j["yaw"];
                if (yaw.type() == JsonType::Number) e.mark.yawDeg = static_cast<float>(yaw.number());
                else if (yaw.type() != JsonType::Absent) errors.push_back(mpath + ".yaw: expected degrees");
                if (std::abs(e.mark.x) > 50.f || std::abs(e.mark.y) > 50.f)
                    errors.push_back(mpath + ".at: more than 50 m from the anchor");
                marks.entries.push_back(std::move(e));
            }
        }
    }
    if (errors.empty()) {
        out = std::move(marks);
        return true;
    }
    if (error) {
        error->clear();
        for (const std::string& e : errors) *error += e + "\n";
    }
    return false;
}

CastMark castArcMark(std::size_t index, std::size_t count) {
    CastMark m;
    if (count <= 1 || index >= count) return m;
    const float spacing = 1.1f;
    const float offset = (static_cast<float>(index) - 0.5f * static_cast<float>(count - 1)) * spacing;
    m.y = offset;
    m.x = -0.15f * offset * offset; // the ends a little further from the camera
    m.yawDeg = -12.f * offset;      // each turned toward the middle
    return m;
}

} // namespace ftcore
