#include "ftcore/BeatAccess.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ftcore {

namespace {

float clampValue(float v, float lo, float hi, float fallback) {
    if (!(v == v)) return fallback;
    return v < lo ? lo : v > hi ? hi : v;
}

int clampInt(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

} // namespace

void clampBeatAccess(BeatAccessOptions& o) {
    const BeatAccessOptions d;
    o.windowScale = clampValue(o.windowScale, 1.0f, 2.0f, d.windowScale);
    o.windowScale = std::round(o.windowScale * 4.0f) / 4.0f;
    o.whiffLockoutMs = clampValue(o.whiffLockoutMs, 0.0f, 400.0f, d.whiffLockoutMs);
    o.pairMode = clampInt(o.pairMode, 0, 2);
    o.uiScale = clampValue(o.uiScale, 1.0f, 2.0f, d.uiScale);
    o.cutsReduced = clampInt(o.cutsReduced, 0, 2);
    o.shakeScale = clampValue(o.shakeScale, 0.0f, 1.0f, d.shakeScale);
    o.flashLevel = clampInt(o.flashLevel, 0, 2);
    o.tempoScale = clampValue(o.tempoScale, 0.5f, 1.0f, d.tempoScale);
}

std::string encodeBeatAccess(const BeatAccessOptions& in) {
    BeatAccessOptions o = in;
    clampBeatAccess(o);
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "{\"beat.window_scale\":%.2f,\"beat.whiff_lockout_ms\":%.0f,\"beat.pair_mode\":%d,"
                  "\"beat.ui_scale\":%.2f,\"beat.cuts_reduced\":%d,\"beat.shake_scale\":%.2f,"
                  "\"beat.flash_level\":%d,\"beat.tempo_scale\":%.2f}",
                  static_cast<double>(o.windowScale), static_cast<double>(o.whiffLockoutMs), o.pairMode,
                  static_cast<double>(o.uiScale), o.cutsReduced, static_cast<double>(o.shakeScale), o.flashLevel,
                  static_cast<double>(o.tempoScale));
    return buf;
}

bool decodeBeatAccess(std::string_view text, BeatAccessOptions& out, std::string* error) {
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(text, &parseError)) {
        if (error) *error = "beat access is not valid JSON: " + parseError;
        return false;
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        if (error) *error = "beat access: the top level must be an object";
        return false;
    }
    BeatAccessOptions o;
    const auto num = [&root](const char* key, float fallback) {
        const JsonValue v = root[key];
        return v.isNumber() ? static_cast<float>(v.number()) : fallback;
    };
    const auto whole = [&root](const char* key, int fallback) {
        const JsonValue v = root[key];
        return v.isInteger() ? static_cast<int>(std::clamp<std::int64_t>(v.integer(), -1000, 1000)) : fallback;
    };
    o.windowScale = num("beat.window_scale", o.windowScale);
    o.whiffLockoutMs = num("beat.whiff_lockout_ms", o.whiffLockoutMs);
    o.pairMode = whole("beat.pair_mode", o.pairMode);
    o.uiScale = num("beat.ui_scale", o.uiScale);
    o.cutsReduced = whole("beat.cuts_reduced", o.cutsReduced);
    o.shakeScale = num("beat.shake_scale", o.shakeScale);
    o.flashLevel = whole("beat.flash_level", o.flashLevel);
    o.tempoScale = num("beat.tempo_scale", o.tempoScale);
    clampBeatAccess(o);
    out = o;
    return true;
}

double beatPerfectScale(double windowScale) {
    const double s = std::max(1.0, windowScale);
    return std::min(1.5, 1.0 + (s - 1.0) * 0.5);
}

BeatBossTuning beatAccessTuning(const BeatBossTuning& base, const BeatAccessOptions& in) {
    BeatAccessOptions o = in;
    clampBeatAccess(o);
    const double ws = static_cast<double>(o.windowScale);
    const double ps = beatPerfectScale(ws);
    BeatBossTuning t = base;
    t.counterLate = base.lateLimit() * ws;
    t.strikePerfect = base.strikePerfect * ps;
    t.strikeGood = base.strikeGood * ws;
    t.perfect = base.perfect * ps;
    t.counterEarly = base.counterEarly * ws;
    t.pairWidth = base.pairWidth * ws;
    t.holdSlack = base.holdSlack * ws;
    t.counterLockout = std::min(base.counterLockout, static_cast<double>(o.whiffLockoutMs) * 0.001) / ws;
    t.pairMode = static_cast<std::uint8_t>(o.pairMode);
    return t;
}

float maxBeatWindowScale(const BeatBossDef& def, const BeatBossTuning& base, float wanted) {
    BeatAccessOptions o;
    o.windowScale = wanted;
    clampBeatAccess(o);
    while (o.windowScale > 1.0f && beatBossWindowsOverlap(def, beatAccessTuning(base, o))) o.windowScale -= 0.25f;
    return o.windowScale;
}

} // namespace ftcore
