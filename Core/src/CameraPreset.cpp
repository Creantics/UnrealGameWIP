#include "ftcore/CameraPreset.h"

#include "ftcore/Json.h"

namespace ftcore {

namespace {

// One preset's object over `p`; false with the reason in *error (p then half read: the caller drops its copy).
bool readPreset(const JsonValue& v, std::string_view name, CameraPreset& p, std::string* error) {
    const auto fail = [&](std::string_view key, const char* why) {
        if (error) *error = "camera_presets." + std::string(name) + (key.empty() ? std::string() : "." + std::string(key)) + ": " + why;
        return false;
    };
    if (!v.isObject()) return fail("", "not an object");
    struct Field {
        const char* name;
        float* value;
        double lo, hi;
    };
    const Field fields[] = {
        {"arm", &p.rig.arm, 1.0, 40.0},          {"pivot", &p.rig.pivot, 0.5, 10.0},
        {"side", &p.rig.side, -3.0, 3.0},        {"up", &p.rig.up, -1.0, 3.0},
        {"fov", &p.rig.fov, 30.0, 120.0},        {"lockPitch", &p.rig.lockPitch, -40.0, 10.0},
        {"cutAt", &p.cutAt, 0.0, 1.8},           {"cutMin", &p.cutMin, -0.1, 0.5},
        {"headMax", &p.headMax, 0.3, 0.95},      {"targetX", &p.targetX, 0.3, 0.7},
        {"lagSeconds", &p.lagSeconds, 0.0, 0.5}, {"lowMax", &p.lowMax, 0.0, 1.0},
    };
    for (std::uint32_t i = 0; i < v.size(); ++i) {
        const std::string_view k = v.key(i);
        bool known = false;
        for (const Field& f : fields) known = known || k == f.name;
        if (!known) return fail(k, "not a known tunable");
    }
    for (const Field& f : fields) {
        const JsonValue x = v[f.name];
        if (x.isAbsent()) continue;
        if (!x.isNumber()) return fail(f.name, "not a number");
        const double d = x.number();
        if (!(d >= f.lo && d <= f.hi)) return fail(f.name, "out of range");
        *f.value = static_cast<float>(d);
    }
    return true;
}

} // namespace

bool importCameraPresets(std::string_view json, CameraPresets& out, std::string* error) {
    JsonDocument doc;
    if (!doc.parse(json, error)) return false;
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        if (error) *error = "camera_presets: not an object";
        return false;
    }
    CameraPresets r = out;
    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view k = root.key(i);
        if (k == "version") continue;
        if (k != "classic" && k != "close") {
            if (error) *error = "camera_presets." + std::string(k) + ": not a known preset";
            return false;
        }
    }
    const JsonValue classicObj = root["classic"];
    if (!classicObj.isAbsent() && !readPreset(classicObj, "classic", r.classic, error)) return false;
    const JsonValue closeObj = root["close"];
    if (!closeObj.isAbsent() && !readPreset(closeObj, "close", r.close, error)) return false;
    out = r;
    return true;
}

const CameraPreset& cameraPreset(const CameraPresets& presets, CameraPresetId id) {
    return id == CameraPresetId::Close ? presets.close : presets.classic;
}

ZoneCamera applyCameraPreset(const ZoneCamera& fit, const CameraPreset& p) {
    // The zone's rig was checked as the zone's (checkArenaZone: arm >= 3, lockPitch <= -5); this runs after that, on the
    // host's own copy, so a preset outside those (close) never reaches the zone's checks. Only the framing cap is the
    // zone's own (a lens that would sit in a building turned further).
    ZoneCamera c = p.rig;
    c.frameTurnCapDeg = fit.frameTurnCapDeg;
    return c;
}

ArenaLeadFrameRules cameraPresetLeadRules(const CameraPreset& p) {
    ArenaLeadFrameRules r = kArenaLeadFrame;
    r.feetMin = p.cutMin;
    r.headMax = p.headMax;
    return r;
}

} // namespace ftcore
