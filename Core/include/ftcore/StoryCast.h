#pragma once

// The speaking cast on screen: which cast speakers a scene has, where each one stands, and how a face's
// shape weights drive a rig that takes named control curves instead of shape morphs (a MetaHuman face).
//
// Face curves: the rig's own mapping asset (a pose per face shape, each pose a value per control curve) is
// read at bind time into a FaceCurveMap. Nothing is authored by hand; shapes the asset has no pose for are
// reported and stay unmapped. Each tick, writeFaceCurves turns the player's weights into curve values:
//   out[c] = clamp(sum over shapes s of weights[s] * pose[s][c], 0, 1)   (no allocation)
//
// Cast marks: where each cast speaker stands in a scene, in metres and degrees relative to the dialogue map's
// anchor (its stand-in). They are story data, so the file is local (Data/Story/cast_marks.json, gitignored):
//   { "version": 1, "scenes": [ { "scene": "<scene id>",
//       "marks": [ { "speaker": "<script label>", "at": [x, y], "yaw": deg }, ... ] }, ... ] }
// A speaker with no mark stands on the default arc (castArcMark). Errors never quote a speaker label.

#include "ftcore/Api.h"
#include "ftcore/AssetManifest.h"
#include "ftcore/Face.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

class StoryDatabase;

// ---- Face curves ----

struct FaceCurveMap {
    struct Term {
        std::uint16_t shape = 0; // index into FaceShapeSet::shapes
        std::uint16_t curve = 0; // index into curves
        float weight = 0.f;      // the pose's value for that curve
    };
    std::vector<std::string> curves; // the rig's curve names, in the asset's order
    std::vector<Term> terms;         // non-zero pose values, sorted by curve
    std::uint32_t mappedShapes = 0;  // shapes with a pose that moves at least one curve

    bool empty() const { return terms.empty(); }
};

// Builds the map from a pose table: values[p * curveNames.size() + c] is pose p's value for curve c. A shape
// takes the pose whose name matches it ignoring case (ARKit "jawOpen" = pose "JawOpen"). Shapes with no pose
// are appended to *unmatched (may be null). False, and `out` empty, when the table's sizes don't agree or
// there are more than 65535 shapes or curves. Allocates; call at bind time.
FTCORE_API bool buildFaceCurveMap(const FaceShapeSet& set, std::span<const std::string_view> poseNames,
                                  std::span<const std::string_view> curveNames, std::span<const float> values,
                                  FaceCurveMap& out, std::vector<std::string>* unmatched = nullptr);

// out[c] for each of map.curves (out.size() must be at least map.curves.size(); extra entries are zeroed).
// Allocation-free.
FTCORE_API void writeFaceCurves(const FaceCurveMap& map, const FaceWeights& weights, std::span<float> out);

// ---- Scene cast ----

inline constexpr std::size_t kMaxSceneCast = 8; // speakers on screen at once in a dialogue scene

// The scene's cast speakers (labels with a model in `cast`), in the order of their first line, at most
// kMaxSceneCast (later ones are dropped and counted in *dropped). Views into the database. No allocation.
FTCORE_API std::size_t sceneCastSpeakers(const StoryDatabase& db, std::uint32_t scene, const SpeakerCast& cast,
                                         std::array<std::string_view, kMaxSceneCast>& out,
                                         std::uint32_t* dropped = nullptr);

struct CastMark {
    float x = 0.f, y = 0.f; // metres from the anchor: +x toward the camera, +y to its right
    float yawDeg = 0.f;     // facing, from the anchor's facing
};

struct CastMarks {
    struct Entry {
        std::string scene, speaker;
        CastMark mark;
    };
    std::vector<Entry> entries;

    // The scene's mark for a speaker, or null when it has none.
    FTCORE_API const CastMark* find(std::string_view scene, std::string_view speaker) const;
};

// On failure `out` is empty and *error lists each problem, one per line (never a speaker label).
FTCORE_API bool importCastMarks(std::string_view json, CastMarks& out, std::string* error = nullptr);

// The default mark of speaker `index` of `count`: a shallow arc across the anchor, each turned toward its
// centre, 1.1 m apart; one speaker stands on the anchor itself.
FTCORE_API CastMark castArcMark(std::size_t index, std::size_t count);

} // namespace ftcore
