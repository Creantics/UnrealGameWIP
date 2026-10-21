#include "ftcore/AudioCues.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <iterator>

namespace ftcore {

namespace {

constexpr const char* kBusNames[] = {"music", "voice", "sfx", "ambience"};
constexpr const char* kBusFolders[] = {"Music", "Voice", "Sfx", "Ambience"};

std::string audStr(std::string_view s) { return std::string(s); }

std::string audNum(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", v);
    return buf;
}

void audUnknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys,
                    std::vector<std::string>* warnings) {
    if (!warnings) return;
    for (std::uint32_t k = 0; k < j.size(); ++k) {
        const std::string_view key = j.key(k);
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            warnings->push_back(path + ": unknown key: " + audStr(key));
    }
}

// An optional number in [lo, hi]; left alone when absent.
void audNumber(JsonValue j, const char* key, float& out, double lo, double hi, const std::string& path,
               std::vector<std::string>& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent()) return;
    const double d = v.number(-1e30);
    if (!v.isNumber() || !std::isfinite(d) || d < lo || d > hi) {
        errors.push_back(path + "." + key + ": expected a number in " + audNum(lo) + ".." + audNum(hi));
        return;
    }
    out = static_cast<float>(d);
}

AssetIssue audIssue(AssetSeverity severity, const char* code, std::string message) {
    AssetIssue issue;
    issue.severity = severity;
    issue.code = code;
    issue.message = std::move(message);
    return issue;
}

} // namespace

const char* audioBusName(AudioBus bus) {
    const auto i = static_cast<std::size_t>(bus);
    return i < std::size(kBusNames) ? kBusNames[i] : "?";
}

const char* audioBusFolder(AudioBus bus) {
    const auto i = static_cast<std::size_t>(bus);
    return i < std::size(kBusFolders) ? kBusFolders[i] : "?";
}

bool audioBusFromName(std::string_view name, AudioBus& out) {
    for (std::size_t i = 0; i < std::size(kBusNames); ++i) {
        if (name == kBusNames[i]) {
            out = static_cast<AudioBus>(i);
            return true;
        }
    }
    return false;
}

const AudioCue* AudioCueTable::find(std::string_view id) const {
    for (const AudioCue& cue : cues)
        if (cue.id == id) return &cue;
    return nullptr;
}

bool importAudioCues(std::string_view json, AudioCueTable& out, std::string* error, std::vector<std::string>* warnings) {
    out = AudioCueTable{};
    std::vector<std::string> errors;
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(json, &parseError)) {
        errors.push_back("cues file: " + parseError);
    } else if (!doc.root().isObject()) {
        errors.push_back("cues file: expected an object");
    } else {
        const JsonValue root = doc.root();
        if (warnings)
            for (std::uint32_t k = 0; k < root.size(); ++k)
                if (root.key(k) != "version" && root.key(k) != "cues") warnings->push_back("unknown key: " + audStr(root.key(k)));
        if (root["version"].integer(0) != 1) errors.push_back("version: expected 1");
        const JsonValue list = root["cues"];
        if (!list.isArray()) errors.push_back("cues: expected an array");
        AudioCueTable table;
        for (std::uint32_t i = 0; i < list.size() && list.isArray(); ++i) {
            const JsonValue j = list[i];
            const std::string path = "cues[" + std::to_string(i) + "]";
            if (!j.isObject()) {
                errors.push_back(path + ": expected an object");
                continue;
            }
            audUnknownKeys(j, path, {"id", "bus", "playback", "file", "bpm", "offset", "loopStart", "loopEnd", "fadeIn",
                                     "fadeOut", "gainDb"},
                           warnings);
            AudioCue cue;
            cue.id = audStr(j["id"].string());
            if (cue.id.empty()) errors.push_back(path + ".id: expected a cue id");
            else if (table.find(cue.id)) errors.push_back(path + ".id: duplicate cue " + cue.id);

            if (!audioBusFromName(j["bus"].string(), cue.bus))
                errors.push_back(path + ".bus: expected music, voice, sfx or ambience");
            const std::string_view playback = j["playback"].string();
            if (playback == "loop") cue.playback = CuePlayback::Loop;
            else if (playback == "stinger") cue.playback = CuePlayback::Stinger;
            else if (playback == "oneshot") cue.playback = CuePlayback::OneShot;
            else errors.push_back(path + ".playback: expected loop, stinger or oneshot");

            const JsonValue file = j["file"];
            if (file.isAbsent()) cue.file = std::string(audioBusFolder(cue.bus)) + "/" + cue.id + ".wav";
            else if (!file.isString() || file.string().empty()) errors.push_back(path + ".file: expected a path");
            else cue.file = audStr(file.string());

            audNumber(j, "bpm", cue.bpm, 1, 400, path, errors);
            audNumber(j, "offset", cue.offsetSeconds, 0, 600, path, errors);
            audNumber(j, "fadeIn", cue.fadeInSeconds, 0, 30, path, errors);
            audNumber(j, "fadeOut", cue.fadeOutSeconds, 0, 30, path, errors);
            audNumber(j, "gainDb", cue.gainDb, -40, 12, path, errors);
            if (j.has("loopStart") || j.has("loopEnd")) {
                audNumber(j, "loopStart", cue.loopStartSeconds, 0, 3600, path, errors);
                audNumber(j, "loopEnd", cue.loopEndSeconds, 0, 3600, path, errors);
                if (cue.playback != CuePlayback::Loop) errors.push_back(path + ": loop points on a cue that doesn't loop");
                else if (!j.has("loopStart") || !j.has("loopEnd")) errors.push_back(path + ": give both loopStart and loopEnd");
                else if (cue.loopEndSeconds <= cue.loopStartSeconds) errors.push_back(path + ": loopEnd must be after loopStart");
            }
            table.cues.push_back(std::move(cue));
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

LoudnessTarget loudnessTarget(AudioBus bus) {
    LoudnessTarget t;
    switch (bus) {
    case AudioBus::Music: t = {true, -18.f, 2.f, -1.f}; break;
    case AudioBus::Voice: t = {true, -23.f, 2.f, -1.f}; break;
    case AudioBus::Ambience: t = {true, -24.f, 3.f, -1.f}; break;
    default: break;
    }
    return t;
}

std::vector<AssetIssue> checkClip(const WavInfo& info, const Loudness& loudness, AudioBus bus, const AudioCue* cue) {
    std::vector<AssetIssue> issues;
    if (info.sampleRate != 44100 && info.sampleRate != 48000)
        issues.push_back(audIssue(AssetSeverity::Error, "sample-rate",
                                  std::to_string(info.sampleRate) + " Hz; use 44100 or 48000"));
    if (info.channels < 1 || info.channels > 2)
        issues.push_back(audIssue(AssetSeverity::Error, "channels", std::to_string(info.channels) + " channels; use mono or stereo"));
    else if (bus == AudioBus::Voice && info.channels != 1)
        issues.push_back(audIssue(AssetSeverity::Error, "voice-not-mono", "voice lines are mono"));
    if (info.frames == 0) {
        issues.push_back(audIssue(AssetSeverity::Error, "empty", "no samples"));
        return issues;
    }

    const LoudnessTarget target = loudnessTarget(bus);
    if (target.checkIntegrated) {
        if (!loudness.hasIntegrated) {
            issues.push_back(audIssue(AssetSeverity::Warning, "loudness-unmeasured",
                                      "too short or too quiet to measure integrated loudness"));
        } else if (std::fabs(loudness.integratedLufs - target.integratedLufs) > target.toleranceLu) {
            issues.push_back(audIssue(AssetSeverity::Error, "loudness",
                                      audNum(loudness.integratedLufs) + " LUFS; the " + audioBusName(bus) + " target is " +
                                          audNum(target.integratedLufs) + " +-" + audNum(target.toleranceLu)));
        }
    }
    if (loudness.truePeakDb > target.maxTruePeakDb)
        issues.push_back(audIssue(AssetSeverity::Error, "true-peak",
                                  audNum(loudness.truePeakDb) + " dBTP; at most " + audNum(target.maxTruePeakDb)));

    if (cue && cue->hasLoopPoints() && cue->loopEndSeconds > info.durationSeconds() + 0.001)
        issues.push_back(audIssue(AssetSeverity::Error, "loop-range",
                                  "loopEnd " + audNum(cue->loopEndSeconds) + " s is past the clip's end (" +
                                      audNum(info.durationSeconds()) + " s)"));
    return issues;
}

} // namespace ftcore
