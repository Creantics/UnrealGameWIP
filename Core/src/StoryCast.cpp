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

constexpr FaceCurveHand kHand[] = {
    // mouthClose (closed-lip sounds: m, b, p): the four lip corners pressed together, the jaw pulled shut.
    {"mouthClose", "CTRL_expressions_mouthLipsTogetherUL", 1.f},
    {"mouthClose", "CTRL_expressions_mouthLipsTogetherUR", 1.f},
    {"mouthClose", "CTRL_expressions_mouthLipsTogetherDL", 1.f},
    {"mouthClose", "CTRL_expressions_mouthLipsTogetherDR", 1.f},
    {"mouthClose", "CTRL_expressions_jawOpen", -1.f},
};

} // namespace

std::span<const FaceCurveHand> faceCurveHandMap() { return kHand; }

bool buildFaceCurveMap(const FaceShapeSet& set, std::span<const std::string_view> poseNames,
                       std::span<const std::string_view> curveNames, std::span<const float> values, FaceCurveMap& out,
                       std::vector<std::string>* unmatched, std::span<const FaceCurveHand> hand) {
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
            bool handMoves = false;
            for (const FaceCurveHand& h : hand) {
                if (!sameNoCase(h.shape, set.shapes[s]) || h.weight == 0.f || !std::isfinite(h.weight)) continue;
                for (std::size_t c = 0; c < curves; ++c)
                    if (sameNoCase(curveNames[c], h.curve)) {
                        out.terms.push_back({static_cast<std::uint16_t>(s), static_cast<std::uint16_t>(c), h.weight});
                        handMoves = true;
                        break;
                    }
            }
            if (handMoves) {
                ++out.mappedShapes;
                ++out.handShapes;
            } else if (unmatched) {
                unmatched->push_back(set.shapes[s]);
            }
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
        const std::string_view sceneId = db.view(scene.id);
        for (std::uint32_t b = 0; b < scene.beatCount; ++b) {
            const Beat& beat = db.beat(scene, b);
            if (beat.type != BeatType::Line) continue;
            const std::string_view speaker = db.view(beat.speaker);
            const std::string_view model = speaker.empty() ? std::string_view{} : cast.modelFor(speaker, sceneId);
            if (model.empty()) continue;
            bool listed = false;
            for (std::size_t i = 0; i < n && !listed; ++i) listed = cast.modelFor(out[i], sceneId) == model;
            if (listed) continue;
            if (n < kMaxSceneCast) {
                out[n++] = speaker;
            } else {
                // Dropped: counted at the model's first line only.
                bool seen = false;
                for (std::uint32_t e = 0; e < b && !seen; ++e) {
                    const Beat& earlier = db.beat(scene, e);
                    const std::string_view other = earlier.type == BeatType::Line ? db.view(earlier.speaker) : std::string_view{};
                    seen = !other.empty() && cast.modelFor(other, sceneId) == model;
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

CastMark castArcMark(std::size_t index, std::size_t count, std::size_t row) {
    CastMark m;
    if (index >= count && count > 0) return m;
    const float spacing = 1.1f;
    const float offset = count <= 1 ? 0.f : (static_cast<float>(index) - 0.5f * static_cast<float>(count - 1)) * spacing;
    const float shifted = offset + (row % 2 == 1 ? 0.5f * spacing : 0.f);
    m.y = shifted;
    m.x = -0.15f * offset * offset - kCastRowStep * static_cast<float>(row); // the ends a little further back
    m.yawDeg = -12.f * shifted;                                              // each turned toward the middle
    return m;
}

bool castMarkFree(std::span<const CastMark> taken, const CastMark& at, float minGap) {
    for (const CastMark& t : taken) {
        const float dx = t.x - at.x, dy = t.y - at.y;
        if (dx * dx + dy * dy < minGap * minGap) return false;
    }
    return true;
}

const CastMark* findCastMark(const CastMarks& marks, const SpeakerCast& cast, std::string_view scene,
                             std::string_view speaker) {
    if (const CastMark* own = marks.find(scene, speaker)) return own;
    const std::string_view model = cast.modelFor(speaker, scene);
    if (model.empty()) return nullptr;
    for (const SpeakerCast::Entry& e : cast.entries) {
        if (e.speaker == speaker || cast.modelFor(e.speaker, scene) != model) continue;
        if (const CastMark* other = marks.find(scene, e.speaker)) return other;
    }
    return nullptr;
}

namespace {

CastStandPlan planOnce(std::span<const CastMark* const> marks, bool standIn, const CastSpotTest* test) {
    CastStandPlan plan;
    plan.standInShown = standIn;
    const std::size_t count = std::min(marks.size(), kMaxSceneCast);
    std::array<CastMark, kMaxSceneCast + 1> taken{};
    std::size_t takenCount = 0;
    if (standIn) taken[takenCount++] = CastMark{};
    const auto isFree = [&](const CastMark& at) {
        return castMarkFree(std::span<const CastMark>(taken.data(), takenCount), at, kCastGap) &&
               !(test && test->blocked(at));
    };
    for (std::size_t i = 0; i < count; ++i) {
        if (!marks[i]) continue;
        if (isFree(*marks[i])) {
            plan.at[i] = *marks[i];
            plan.stand[i] = CastStand::OnMark;
            taken[takenCount++] = *marks[i];
            ++plan.onMark;
        } else {
            ++plan.marksRefused;
        }
    }
    std::size_t slots = count;
    if (standIn && count > 0) {
        slots = count + 1;
        if (slots % 2 == 0) ++slots;
    }
    std::size_t next = 0; // candidate over rows of slots
    for (std::size_t i = 0; i < count; ++i) {
        if (plan.stand[i] != CastStand::Hidden) continue;
        bool found = false;
        CastMark at;
        while (!found && next < kCastRows * slots) {
            at = castArcMark(next % slots, slots, next / slots);
            ++next;
            found = isFree(at);
        }
        if (!found) {
            ++plan.hidden;
            continue;
        }
        plan.at[i] = at;
        plan.stand[i] = CastStand::OnArc;
        taken[takenCount++] = at;
    }
    return plan;
}

} // namespace

CastStandPlan planCastStands(std::span<const CastMark* const> marks, bool standInStays, const CastSpotTest* test) {
    CastStandPlan plan = planOnce(marks, standInStays, test);
    if (plan.hidden > 0 && !standInStays) plan = planOnce(marks, true, test); // a hidden speaker's face: the stand-in
    return plan;
}

} // namespace ftcore
