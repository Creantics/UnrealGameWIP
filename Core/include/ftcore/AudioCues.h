#pragma once

// Music and sound cues by id: Data/Audio/cues.json maps every cue id the story fires (MUS-xxx,
// SFX-xxx) to its bus, how it plays and where its file is; checkClip holds a decoded clip to the
// mix rules for its bus. The manifest carries ids and numbers only: no names, no descriptions.
//
//   ftcore::AudioCueTable cues;
//   std::string error;
//   if (!ftcore::importAudioCues(text, cues, &error)) ...;
//   const ftcore::AudioCue* cue = cues.find("MUS-001"); // no allocation
//
// cues.json:
//   { "version": 1, "cues": [
//       { "id": "MUS-001", "bus": "music", "playback": "loop", "bpm": 54, "fadeIn": 2, "fadeOut": 3 },
//       { "id": "SFX-004", "bus": "sfx", "playback": "oneshot", "gainDb": -3 } ] }
//   Keys: id (required, unique), bus ("music", "voice", "sfx", "ambience"; required), playback
//   ("loop", "stinger", "oneshot"; required), file (default "<Bus>/<id>.wav" under Data/Audio, e.g.
//   "Music/MUS-001.wav"), bpm (> 0, optional), offset (seconds of lead-in before the first beat,
//   >= 0), loopStart / loopEnd (seconds, loops only, both or neither, start < end), fadeIn /
//   fadeOut (seconds, 0..30), gainDb (-40..12). Unknown keys are warnings.
//
// Mix rules (docs/ASSET_INTAKE.md): 44.1 or 48 kHz, mono or stereo (voice mono), integrated
// loudness within the bus target's tolerance (sfx: no loudness target), true peak at most -1 dBTP.

#include "ftcore/Api.h"
#include "ftcore/Glb.h" // AssetIssue
#include "ftcore/Loudness.h"
#include "ftcore/Wav.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

enum class AudioBus : std::uint8_t { Music, Voice, Sfx, Ambience, Count };
enum class CuePlayback : std::uint8_t { Loop, Stinger, OneShot };

// "music", "voice", "sfx", "ambience"; and the folder name "Music", "Voice", "Sfx", "Ambience".
FTCORE_API const char* audioBusName(AudioBus bus);
FTCORE_API const char* audioBusFolder(AudioBus bus);
FTCORE_API bool audioBusFromName(std::string_view name, AudioBus& out);

struct AudioCue {
    std::string id;
    std::string file; // relative to Data/Audio
    AudioBus bus = AudioBus::Music;
    CuePlayback playback = CuePlayback::OneShot;
    float bpm = 0.f;                 // 0 = unknown / free time
    float offsetSeconds = 0.f;
    float loopStartSeconds = -1.f;   // -1 = the whole clip loops
    float loopEndSeconds = -1.f;
    float fadeInSeconds = 0.f;
    float fadeOutSeconds = 0.f;
    float gainDb = 0.f;

    bool hasLoopPoints() const { return loopStartSeconds >= 0.f && loopEndSeconds > loopStartSeconds; }
};

struct AudioCueTable {
    std::vector<AudioCue> cues;
    // The cue with that id, or nullptr. No allocation.
    FTCORE_API const AudioCue* find(std::string_view id) const;
};

// On failure `out` is empty and *error lists each problem with its path, one per line.
FTCORE_API bool importAudioCues(std::string_view json, AudioCueTable& out, std::string* error = nullptr,
                                std::vector<std::string>* warnings = nullptr);

struct LoudnessTarget {
    bool checkIntegrated = false;
    float integratedLufs = 0.f;
    float toleranceLu = 0.f;
    float maxTruePeakDb = -1.f;
};

// Music -18 LUFS, voice -23, ambience -24 (each +-2 LU, ambience +-3); sfx peak only.
FTCORE_API LoudnessTarget loudnessTarget(AudioBus bus);

// Issue codes: sample-rate, channels, voice-not-mono, empty, loudness, loudness-unmeasured
// (warning: too short or too quiet to gate), true-peak, loop-range. `cue` may be null (voice lines).
FTCORE_API std::vector<AssetIssue> checkClip(const WavInfo& info, const Loudness& loudness, AudioBus bus,
                                             const AudioCue* cue);

} // namespace ftcore
