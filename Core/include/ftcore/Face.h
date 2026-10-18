#pragma once

// Facial animation: the face shape set (ARKit 52 + house shapes), viseme / expression presets, the
// layering math, baked lip-sync tracks (.nface) and a player that turns them into one weight per
// shape plus jaw / tongue angles.
//
// Core has no JSON reader, so the owner fills a FaceShapeSet from face_presets.json (addShape /
// addViseme / addExpression, then finish()). The owner also applies the output: weights() to the
// mesh's morph targets (bindMesh / writeMeshWeights), and jawDeg() / tongueDeg() as rotations
// about each joint's local +X from its rest pose.

#include "ftcore/Api.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ftcore {

inline constexpr std::size_t kMaxFaceShapes = 64; // shape budget per face mesh
using FaceWeights = std::array<float, kMaxFaceShapes>;

struct FaceVec3 {
    float x = 0.f, y = 0.f, z = 0.f;
    friend bool operator==(const FaceVec3&, const FaceVec3&) = default;
};

// Which layer a shape belongs to (face_spec.py SHAPE_INFO[...]["layer"]).
enum class FaceLayer : std::uint8_t { Upper, Eyes, MouthArticulation, MouthTone, Corrective };

// "upper", "eyes", "mouth_art", "mouth_tone", "corrective". False for anything else.
FTCORE_API bool faceLayerFromName(std::string_view name, FaceLayer& out);

struct FacePreset {
    std::string name;
    FaceWeights weights{}; // indexed like FaceShapeSet::shapes
    FaceVec3 tongueDeg;    // visemes only: tongue_01..03 rotation about local +X
};

// A preset's non-zero shapes, by name.
using FaceShapeWeight = std::pair<std::string_view, float>;

struct FaceShapeSet {
    std::vector<std::string> shapes; // canonical order
    std::vector<FaceLayer> layers;   // per shape
    // Corrective shapes: weight = w[driverA] * max(w[driverB], w[driverB2]).
    struct Corrective {
        int shape = -1, driverA = -1, driverB = -1, driverB2 = -1;
    };
    std::vector<Corrective> correctives;
    std::vector<FacePreset> visemes;
    std::vector<FacePreset> expressions;
    float jawMaxDeg = 25.f;
    // Shapes the layering and the player address directly (-1 when absent). Set by finish().
    int jawOpen = -1, mouthFunnel = -1, mouthPucker = -1, mouthClose = -1, blinkLeft = -1, blinkRight = -1;

    std::size_t size() const { return shapes.size(); }
    FTCORE_API int find(std::string_view name) const;
    FTCORE_API const FacePreset* viseme(std::string_view name) const;
    FTCORE_API const FacePreset* expression(std::string_view name) const;

    // Building, in face_presets.json order: every shape first, then the presets (which name
    // shapes), then finish(). Each returns false (and says why) on a bad entry.
    FTCORE_API bool addShape(std::string_view name, FaceLayer layer, std::string* error = nullptr);
    FTCORE_API bool addViseme(std::string_view name, std::span<const FaceShapeWeight> weights, FaceVec3 tongueDeg = {},
                              std::string* error = nullptr);
    FTCORE_API bool addExpression(std::string_view name, std::span<const FaceShapeWeight> weights,
                                  std::string* error = nullptr);
    // Resolves the correctives (a shape named `a_x_b` is driven by a and b; a side-less b means
    // the larger of bLeft / bRight) and the named shapes. False when there are more than
    // kMaxFaceShapes shapes or a corrective names unknown drivers.
    FTCORE_API bool finish(std::string* error = nullptr);
};

// The layering, the same function as face_spec.layer_weights:
//   upper:      e + m
//   eyes:       max(e, v, m)            (blinks arrive in the micro layer)
//   mouth art.: v + (1 - speech) * e    (jawOpen: max(v, (1 - speech) * e))
//   mouth tone: e * (1 - 0.7 rho) * (1 - 0.5 close), rho = max(funnel, pucker) of the viseme
//   corrective: product of its drivers' final weights
// every result clamped to [0, 1]. e / v / m are expression, viseme and micro weights.
FTCORE_API void composeFace(const FaceShapeSet& set, const FaceWeights& expr, const FaceWeights& vis, float speech,
                            const FaceWeights& micro, FaceWeights& out);

// A baked lip-sync track (.nface, written by tools/lipsync/bake_face_tracks.py). Channels are
// shape names, plus "@speech" (0..1) and "@tongue_01".."@tongue_03" (degrees). Values are u8,
// frame-major: data[frame * channels.size() + channel]. Shape and speech channels map 0..255 to
// 0..1; tongue channels map 0..255 to -40..+40 degrees.
//
// File layout (little-endian): "NFACE\0" u16 version(1), u16 rate (frames per second),
// u32 frameCount, u16 channelCount, then per channel u8 nameLength + name bytes, then the data.
struct FaceTrack {
    float rate = 60.f;
    std::uint32_t frames = 0;
    std::vector<std::string> channels;
    std::vector<std::uint8_t> data;

    float duration() const { return frames > 1 ? static_cast<float>(frames - 1) / rate : 0.f; }
};

inline constexpr float kTongueTrackRangeDeg = 40.f;

// Parses the bytes of an .nface file (the owner reads the file). False (and `*error`) on bad data.
FTCORE_API bool parseFaceTrack(std::span<const std::uint8_t> bytes, FaceTrack& out, std::string* error = nullptr);
FTCORE_API std::vector<std::uint8_t> serializeFaceTrack(const FaceTrack& track);

// Plays one expression (with crossfades), an optional baked lip-sync track, procedural blinks and
// a micro layer, and composes them each update().
//
// Allocation: setExpression / play / update / setMicro and the output calls never allocate (play()
// resolves channel names into a fixed table). Determinism: blinks come from a xorshift32 seeded in
// the constructor, so a seed and a dt sequence give the same weights on every platform.
class FacePlayer {
public:
    static constexpr std::size_t kMaxTrackChannels = kMaxFaceShapes + 8;

    // `set` must outlive the player.
    FTCORE_API explicit FacePlayer(const FaceShapeSet& set, std::uint32_t seed = 0x5eedu);

    // Crossfades to `preset` (nullptr = neutral) at `intensity` (0..5; 5 = preset weights).
    FTCORE_API void setExpression(const FacePreset* preset, float intensity = 5.f, float fade = 0.25f);
    // Starts a lip-sync track from `time` seconds. Channels that name no shape are ignored. The
    // track must outlive playback. False (and plays nothing) on too many channels or short data.
    FTCORE_API bool play(const FaceTrack& track, float time = 0.f);
    FTCORE_API void stop();
    bool playing() const { return track_ != nullptr; }
    // Micro layer (gaze darts, twitches): set by gameplay, held until changed.
    FTCORE_API void setMicro(int shape, float weight);
    FTCORE_API void clearMicro();
    void setBlinking(bool on) { blinking_ = on; }

    FTCORE_API void update(float dt);

    const FaceWeights& weights() const { return out_; } // indexed like FaceShapeSet::shapes
    float speech() const { return speech_; }
    FTCORE_API float jawDeg() const; // jawOpen * jawMaxDeg
    FaceVec3 tongueDeg() const { return tongue_; }

    // map[t] = shape index for the mesh's morph target t (by name), or -1. Allocates; call at setup.
    FTCORE_API static void bindMesh(const FaceShapeSet& set, std::span<const std::string_view> targetNames,
                                    std::vector<int>& map);
    // out[t] = weights()[map[t]] (0 for unmapped targets). Allocation-free.
    FTCORE_API void writeMeshWeights(std::span<const int> map, std::span<float> out) const;

private:
    float nextRandom();
    void scheduleBlink();
    void sampleTrack();

    const FaceShapeSet* set_;
    FaceWeights exprFrom_{}, exprTo_{}, expr_{}, vis_{}, micro_{}, microAll_{}, out_{};
    float fadeTime_ = 0.f, fadeDuration_ = 0.f;
    const FaceTrack* track_ = nullptr;
    float trackTime_ = 0.f;
    std::array<std::int16_t, kMaxTrackChannels> channelShape_{}; // shape index, or -2 speech, -3.. tongue, -1 none
    bool trackHasSpeech_ = false;
    float speechTarget_ = 0.f, speech_ = 0.f;
    FaceVec3 tongue_;
    bool blinking_ = true;
    float blinkClock_ = 0.f, nextBlink_ = 0.f, blinkPhase_ = -1.f;
    std::uint32_t rng_;
};

} // namespace ftcore
