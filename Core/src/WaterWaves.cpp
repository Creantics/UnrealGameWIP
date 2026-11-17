#include "ftcore/WaterWaves.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.f * kPi;

float smooth01(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

int classIndex(WaterWaveClass c) { return static_cast<int>(c); }

// Scales a set's amplitudes so its significant height is at most capCm, and resets its shared steepness.
void holdHeight(SeaWaves& set, float capCm) {
    const float hs = seaSignificantHeightCm(set);
    if (hs > capCm && hs > 0.f) {
        const float s = capCm / hs;
        for (SeaWave& w : set.waves) w.amplitude *= s;
    }
    float fold = 0.f;
    for (const SeaWave& w : set.waves) fold += w.k * w.amplitude;
    set.q = fold > 0.f ? std::min(1.f, kSeaMaxFold / fold) : 0.f;
}

float mainLength(const SeaWaves& set) {
    float best = 0.f, length = 0.f;
    for (const SeaWave& w : set.waves) {
        if (w.amplitude > best && w.k > 0.f) {
            best = w.amplitude;
            length = kTwoPi / w.k;
        }
    }
    return length;
}

float thetaOf(const SeaWave& w, float x0, float y0, float t) { return w.k * (w.dirX * x0 + w.dirY * y0) - w.omega * t + w.phase; }

float wrapTime(float t) { return std::fmod(t, kSeaLoopS); }

Vec2 boxLocalOf(const WaterBody& body, Vec2 p) {
    const float dx = p.x - body.centre.x;
    const float dy = p.y - body.centre.y;
    return {dx * body.cosYaw + dy * body.sinYaw, -dx * body.sinYaw + dy * body.cosYaw};
}

// The offset and the horizontal map's fold terms at a rest point.
void offsetAndFold(const WaterWaveSets& sets, int ci, float sway, float height, float x0, float y0, float t, float out[3],
                   float& xx, float& yy, float& xy) {
    const SeaWaves& set = sets.set[ci];
    out[0] = out[1] = out[2] = 0.f;
    xx = yy = xy = 0.f;
    for (int i = 0; i < kSeaWaveCount; ++i) {
        const SeaWave& w = set.waves[i];
        const float keep = sets.keep[ci][i];
        if (!(keep > 0.f) || !(w.amplitude > 0.f)) continue;
        const float th = thetaOf(w, x0, y0, t);
        const float c = std::cos(th), s = std::sin(th);
        const float a = keep * w.amplitude;
        const float h = sway * set.q * a;
        out[0] += h * c * w.dirX;
        out[1] += h * c * w.dirY;
        out[2] += height * a * s;
        const float f = h * w.k * s;
        xx += f * w.dirX * w.dirX;
        yy += f * w.dirY * w.dirY;
        xy += f * w.dirX * w.dirY;
    }
}

} // namespace

const char* waterWaveClassName(WaterWaveClass c) {
    switch (c) {
    case WaterWaveClass::Sea: return "sea";
    case WaterWaveClass::River: return "river";
    case WaterWaveClass::Canal: return "canal";
    }
    return "?";
}

WaterWaveSets waterWaveSets(const SceneLook& look, float storm, const WaterWaveSpec& spec) {
    WaterWaveSets out;
    storm = std::isfinite(storm) ? std::clamp(storm, 0.f, 1.f) : 0.f;
    const float wind = std::clamp(look.windMps + storm * spec.stormWindMps, 0.f, 40.f);

    SceneLook sea = look;
    sea.windMps = wind;
    sea.seaSwellM = std::clamp(look.seaSwellM + storm * spec.stormSwellM, 0.f, 8.f);
    out.set[classIndex(WaterWaveClass::Sea)] = seaWaves(sea);

    SceneLook inland = look;
    inland.seaSwellM = 0.f;
    inland.seaChop = 1.f;
    inland.windMps = std::max(wind, spec.calmWindMps);
    inland.seaFetchKm = spec.riverFetchKm;
    SeaWaves river = seaWaves(inland);
    holdHeight(river, spec.riverMaxHsCm + storm * (spec.riverStormHsCm - spec.riverMaxHsCm));
    out.set[classIndex(WaterWaveClass::River)] = river;

    inland.seaFetchKm = spec.canalFetchKm;
    SeaWaves canal = seaWaves(inland);
    holdHeight(canal, spec.canalMaxHsCm + storm * (spec.canalStormHsCm - spec.canalMaxHsCm));
    out.set[classIndex(WaterWaveClass::Canal)] = canal;

    out.lapShare = std::clamp(spec.lapShare, 0.f, 1.f);
    out.fadeLengths = std::max(spec.fadeLengths, 0.f);
    out.minFadeCm = std::max(spec.minFadeCm, 1.f);
    out.gridCm = std::max(spec.gridCm, 1.f);
    for (int c = 0; c < kWaterWaveClasses; ++c) {
        out.mainLengthCm[c] = mainLength(out.set[c]);
        for (int i = 0; i < kSeaWaveCount; ++i) {
            const SeaWave& w = out.set[c].waves[i];
            out.keep[c][i] = w.k > 0.f ? smooth01(4.f, 6.f, kTwoPi / w.k / out.gridCm) : 0.f;
        }
    }
    return out;
}

WaterWaveClass waterWaveClass(const WaterBody& body, const WaterWaveSpec& spec) {
    switch (body.kind) {
    case WaterKind::Sea: return WaterWaveClass::Sea;
    case WaterKind::Lake: return WaterWaveClass::River;
    case WaterKind::Pool: return WaterWaveClass::Canal;
    case WaterKind::Canal:
        return std::hypot(body.flow.x, body.flow.y) >= spec.riverFlowMin ? WaterWaveClass::River : WaterWaveClass::Canal;
    }
    return WaterWaveClass::Canal;
}

float waterEdgeM(const WaterBody& body, Vec2 p) {
    switch (body.shape) {
    case WaterShape::Box: {
        // The long walls only: a canal row's ends open onto the next row.
        const Vec2 l = boxLocalOf(body, p);
        const float fromWall = body.half.y <= body.half.x ? body.half.y - std::fabs(l.y) : body.half.x - std::fabs(l.x);
        return std::max(fromWall, 0.f);
    }
    case WaterShape::HalfPlane: return std::max(body.out(p), 0.f);
    case WaterShape::Polygon: return kWaterNoEdgeM;
    }
    return kWaterNoEdgeM;
}

float waterFadeCm(const WaterWaveSets& sets, WaterWaveClass c) {
    return std::max(sets.minFadeCm, sets.fadeLengths * sets.mainLengthCm[classIndex(c)]);
}

void waterEdgeFactors(const WaterWaveSets& sets, WaterWaveClass c, float edgeCm, float& sway, float& height) {
    sway = smooth01(0.f, waterFadeCm(sets, c), edgeCm);
    height = sets.lapShare + (1.f - sets.lapShare) * sway;
}

void waterWaveOffset(const WaterWaveSets& sets, WaterWaveClass c, float edgeCm, float x0, float y0, float t, float out[3]) {
    float sway, height, xx, yy, xy;
    waterEdgeFactors(sets, c, edgeCm, sway, height);
    offsetAndFold(sets, classIndex(c), sway, height, x0, y0, wrapTime(t), out, xx, yy, xy);
}

float waterWaveHeightCm(const WaterWaveSets& sets, const WaterBody& body, WaterWaveClass c, float x, float y, float t) {
    const int ci = classIndex(c);
    t = wrapTime(t);
    float x0 = x, y0 = y;
    float d[3], xx, yy, xy, sway, height;
    // Newton on the horizontal map; the edge factors change over metres, far slower than the waves, so their own slope
    // is left out of the Jacobian.
    for (int step = 0; step < 5; ++step) {
        waterEdgeFactors(sets, c, waterEdgeM(body, {x0 * 0.01f, y0 * 0.01f}) * 100.f, sway, height);
        offsetAndFold(sets, ci, sway, height, x0, y0, t, d, xx, yy, xy);
        const float rx = x0 + d[0] - x;
        const float ry = y0 + d[1] - y;
        if (std::fabs(rx) + std::fabs(ry) < 1e-3f) break;
        const float a = 1.f - xx, b = -xy, cc = 1.f - yy;
        const float det = a * cc - b * b;
        if (!(std::fabs(det) > 1e-6f)) break;
        x0 -= (cc * rx - b * ry) / det;
        y0 -= (a * ry - b * rx) / det;
    }
    waterEdgeFactors(sets, c, waterEdgeM(body, {x0 * 0.01f, y0 * 0.01f}) * 100.f, sway, height);
    offsetAndFold(sets, ci, sway, height, x0, y0, t, d, xx, yy, xy);
    return d[2];
}

float waterTideGain(const WaterTideSpec& spec, const WaterBody& body, WaterWaveClass c) {
    if (body.kind == WaterKind::Sea) return 1.f;
    if (body.kind != WaterKind::Canal) return 0.f;
    return c == WaterWaveClass::River ? spec.riverGain : spec.canalGain;
}

float waterTideM(const WaterTideSpec& spec, const WaterBody& body, WaterWaveClass c, double totalHours) {
    const float gain = waterTideGain(spec, body, c);
    if (!(gain > 0.f) || !std::isfinite(totalHours) || !(spec.periodHours > 0.f)) return 0.f;
    double lag = 0.0;
    if (body.kind == WaterKind::Canal) lag = c == WaterWaveClass::River ? spec.riverLagHours : spec.canalLagHours;
    // The phase in whole periods, reduced in double before the float cosine so a long game keeps its precision.
    const double turns = (totalHours - spec.highHour - lag) / spec.periodHours;
    const double frac = turns - std::floor(turns);
    return 0.5f * spec.rangeM * gain * static_cast<float>(std::cos(2.0 * 3.14159265358979323846 * frac));
}

void waterTideBand(const WaterTideSpec& spec, const WaterBody& body, WaterWaveClass c, float& lowM, float& highM) {
    const float half = 0.5f * spec.rangeM * waterTideGain(spec, body, c);
    lowM = -half;
    highM = half;
}

float waterSurfaceM(const WaterSpec& spec, const WaterWaveSets& sets, const WaterBody& body, Vec2 p, float t, double totalHours) {
    const WaterWaveClass c = waterWaveClass(body, spec.waves);
    return body.level + waterTideM(spec.tide, body, c, totalHours) + 0.01f * waterWaveHeightCm(sets, body, c, p.x * 100.f, p.y * 100.f, t);
}

// ---- Data/Water/water.json ----

namespace {

struct Field {
    const char* name;
    float lo, hi;
    float WaterWaveSpec::*wave;
    float WaterTideSpec::*tide;
};

constexpr Field kWaveFields[] = {
    {"stormWindMps", 0.f, 30.f, &WaterWaveSpec::stormWindMps, nullptr},
    {"stormSwellM", 0.f, 4.f, &WaterWaveSpec::stormSwellM, nullptr},
    {"calmWindMps", 0.f, 10.f, &WaterWaveSpec::calmWindMps, nullptr},
    {"riverFlowMin", 0.f, 5.f, &WaterWaveSpec::riverFlowMin, nullptr},
    {"riverFetchKm", 0.01f, 50.f, &WaterWaveSpec::riverFetchKm, nullptr},
    {"riverMaxHsCm", 0.f, 100.f, &WaterWaveSpec::riverMaxHsCm, nullptr},
    {"riverStormHsCm", 0.f, 200.f, &WaterWaveSpec::riverStormHsCm, nullptr},
    {"canalFetchKm", 0.01f, 5.f, &WaterWaveSpec::canalFetchKm, nullptr},
    {"canalMaxHsCm", 0.f, 30.f, &WaterWaveSpec::canalMaxHsCm, nullptr},
    {"canalStormHsCm", 0.f, 60.f, &WaterWaveSpec::canalStormHsCm, nullptr},
    {"lapShare", 0.f, 1.f, &WaterWaveSpec::lapShare, nullptr},
    {"fadeLengths", 0.f, 4.f, &WaterWaveSpec::fadeLengths, nullptr},
    {"minFadeCm", 1.f, 5000.f, &WaterWaveSpec::minFadeCm, nullptr},
    {"gridCm", 5.f, 400.f, &WaterWaveSpec::gridCm, nullptr},
};

constexpr Field kTideFields[] = {
    {"periodHours", 1.f, 48.f, nullptr, &WaterTideSpec::periodHours},
    {"rangeM", 0.f, 6.f, nullptr, &WaterTideSpec::rangeM},
    {"highHour", 0.f, 24.f, nullptr, &WaterTideSpec::highHour},
    {"riverGain", 0.f, 1.f, nullptr, &WaterTideSpec::riverGain},
    {"riverLagHours", 0.f, 12.f, nullptr, &WaterTideSpec::riverLagHours},
    {"canalGain", 0.f, 1.f, nullptr, &WaterTideSpec::canalGain},
    {"canalLagHours", 0.f, 12.f, nullptr, &WaterTideSpec::canalLagHours},
};

template <std::size_t N>
bool readGroup(const JsonValue& group, const char* groupName, const Field (&fields)[N], WaterSpec& spec, std::string& why) {
    if (!group.isObject()) {
        why = std::string(groupName) + ": expected an object";
        return false;
    }
    for (std::uint32_t i = 0; i < group.size(); ++i) {
        const std::string_view key = group.key(i);
        if (key.starts_with("_")) continue;
        const Field* found = nullptr;
        for (const Field& f : fields)
            if (key == f.name) found = &f;
        if (!found) {
            why = std::string(groupName) + "." + std::string(key) + ": unknown field";
            return false;
        }
        const JsonValue v = group.value(i);
        const double n = v.number(std::nan(""));
        if (!v.isNumber() || !std::isfinite(n) || n < found->lo || n > found->hi) {
            why = std::string(groupName) + "." + found->name + ": expected a number in " + std::to_string(found->lo) + ".." +
                  std::to_string(found->hi);
            return false;
        }
        if (found->wave) spec.waves.*(found->wave) = static_cast<float>(n);
        else spec.tide.*(found->tide) = static_cast<float>(n);
    }
    return true;
}

} // namespace

bool parseWaterSpec(std::string_view json, WaterSpec& out, std::string* error) {
    std::string why;
    const auto fail = [&]() {
        out = WaterSpec{};
        if (error) *error = why;
        return false;
    };
    JsonDocument doc;
    std::string err;
    if (!doc.parse(json, &err)) {
        why = "water file: " + err;
        return fail();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        why = "water file: expected an object";
        return fail();
    }
    if (const JsonValue v = root["version"]; !(v.isInteger() && v.integer() == 1)) {
        why = "version: expected 1";
        return fail();
    }
    WaterSpec spec;
    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view key = root.key(i);
        if (key == "version" || key.starts_with("_")) continue;
        if (key == "waves") {
            if (!readGroup(root.value(i), "waves", kWaveFields, spec, why)) return fail();
        } else if (key == "tide") {
            if (!readGroup(root.value(i), "tide", kTideFields, spec, why)) return fail();
        } else {
            why = std::string(key) + ": unknown field";
            return fail();
        }
    }
    if (spec.waves.riverStormHsCm < spec.waves.riverMaxHsCm) {
        why = "waves.riverStormHsCm: under riverMaxHsCm";
        return fail();
    }
    if (spec.waves.canalStormHsCm < spec.waves.canalMaxHsCm) {
        why = "waves.canalStormHsCm: under canalMaxHsCm";
        return fail();
    }
    out = spec;
    return true;
}

} // namespace ftcore
