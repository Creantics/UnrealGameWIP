#include "ftcore/Vfx.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace ftcore {

// ---- curves and gradients -----------------------------------------------------------------------------

float sampleVfxKeys(const std::vector<VfxKey>& keys, float t) {
    if (keys.empty()) return 1.f;
    if (t <= keys.front().t) return keys.front().v;
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (t <= keys[i].t) {
            const VfxKey& a = keys[i - 1];
            const VfxKey& b = keys[i];
            const float span = b.t - a.t;
            return span <= 0.f ? b.v : a.v + (b.v - a.v) * ((t - a.t) / span);
        }
    }
    return keys.back().v;
}

namespace {

VfxColor mixColor(const VfxColor& a, const VfxColor& b, float k) {
    return {a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k, a.a + (b.a - a.a) * k};
}

bool isWhiteColor(const VfxColor& c) { return c.r == 1.f && c.g == 1.f && c.b == 1.f && c.a == 1.f; }

bool allWhite(const std::vector<VfxColorStop>& stops) {
    for (const VfxColorStop& s : stops)
        if (!isWhiteColor(s.color)) return false;
    return true;
}

void keyBounds(const std::vector<VfxKey>& keys, float mul, float& lo, float& hi) {
    if (keys.empty()) {
        lo = std::min(lo, mul);
        hi = std::max(hi, mul);
        return;
    }
    for (const VfxKey& k : keys) {
        lo = std::min(lo, k.v * mul);
        hi = std::max(hi, k.v * mul);
    }
}

} // namespace

VfxColor sampleVfxStops(const std::vector<VfxColorStop>& stops, float t) {
    if (stops.empty()) return {};
    if (t <= stops.front().t) return stops.front().color;
    for (std::size_t i = 1; i < stops.size(); ++i) {
        if (t <= stops[i].t) {
            const VfxColorStop& a = stops[i - 1];
            const VfxColorStop& b = stops[i];
            const float span = b.t - a.t;
            return span <= 0.f ? b.color : mixColor(a.color, b.color, (t - a.t) / span);
        }
    }
    return stops.back().color;
}

float VfxCurve::evaluate(float x, float r) const {
    switch (mode) {
    case VfxCurveMode::Constant:
    case VfxCurveMode::TwoConstants: return lo + (hi - lo) * r;
    case VfxCurveMode::Curve: return sampleVfxKeys(keys, x) * multiplier;
    case VfxCurveMode::TwoCurves: {
        const float a = sampleVfxKeys(keysLo, x);
        return (a + (sampleVfxKeys(keys, x) - a) * r) * multiplier;
    }
    }
    return lo;
}

void VfxCurve::bounds(float& outLo, float& outHi) const {
    if (isConstant()) {
        outLo = std::min(lo, hi);
        outHi = std::max(lo, hi);
        return;
    }
    outLo = std::numeric_limits<float>::max();
    outHi = -std::numeric_limits<float>::max();
    keyBounds(keys, multiplier, outLo, outHi);
    if (mode == VfxCurveMode::TwoCurves) keyBounds(keysLo, multiplier, outLo, outHi);
}

float VfxCurve::mean() const {
    if (isConstant()) return 0.5f * (lo + hi);
    constexpr int kSamples = 64;
    float sum = 0.f;
    for (int i = 0; i <= kSamples; ++i) {
        const float w = (i == 0 || i == kSamples) ? 0.5f : 1.f;
        sum += w * evaluate(static_cast<float>(i) / static_cast<float>(kSamples), 0.5f);
    }
    return sum / static_cast<float>(kSamples);
}

VfxColor VfxGradient::evaluate(float x, float r) const {
    switch (mode) {
    case VfxGradientMode::Color: return colorHi;
    case VfxGradientMode::TwoColors: return mixColor(colorLo, colorHi, r);
    case VfxGradientMode::Gradient: return sampleVfxStops(stops, x);
    case VfxGradientMode::TwoGradients: return mixColor(sampleVfxStops(stopsLo, x), sampleVfxStops(stops, x), r);
    case VfxGradientMode::RandomColor: return sampleVfxStops(stops, r);
    }
    return colorHi;
}

bool VfxGradient::isWhite() const {
    switch (mode) {
    case VfxGradientMode::Color: return isWhiteColor(colorHi);
    case VfxGradientMode::TwoColors: return isWhiteColor(colorLo) && isWhiteColor(colorHi);
    case VfxGradientMode::Gradient:
    case VfxGradientMode::RandomColor: return allWhite(stops);
    case VfxGradientMode::TwoGradients: return allWhite(stops) && allWhite(stopsLo);
    }
    return false;
}

// ---- reading ------------------------------------------------------------------------------------------

namespace {

struct Diagnostics {
    std::vector<std::string> errors, warnings;
};

std::string joinPath(const std::string& base, std::string_view key) {
    return base.empty() ? std::string(key) : base + "." + std::string(key);
}

std::string indexPath(const std::string& base, std::uint32_t i) { return base + "[" + std::to_string(i) + "]"; }

std::string withPath(const std::string& path, std::string_view message) {
    return path.empty() ? std::string(message) : path + ": " + std::string(message);
}

// A JSON value plus its path. Object readers mark the keys they read; finish() reports the others.
class Reader {
public:
    Reader(JsonValue value, std::string path, Diagnostics& diag)
        : v_(value), path_(std::move(path)), diag_(&diag), used_(value.isObject() ? value.size() : 0u, 0) {}

    JsonValue json() const { return v_; }
    const std::string& path() const { return path_; }
    Diagnostics& diagnostics() const { return *diag_; }
    std::uint32_t size() const { return v_.size(); }

    void error(std::string_view message) const { diag_->errors.push_back(withPath(path_, message)); }
    void errorAt(std::string_view key, std::string_view message) const {
        diag_->errors.push_back(withPath(joinPath(path_, key), message));
    }

    bool expectObject() const {
        if (v_.isObject()) return true;
        error("expected an object");
        return false;
    }
    bool expectArray() const {
        if (v_.isArray()) return true;
        error("expected an array");
        return false;
    }

    bool has(std::string_view key) const { return v_.isObject() && v_.has(key); }

    std::optional<Reader> child(std::string_view key) {
        const int i = find(key);
        if (i < 0) return std::nullopt;
        used_[static_cast<std::size_t>(i)] = 1;
        return Reader(v_.value(static_cast<std::uint32_t>(i)), joinPath(path_, key), *diag_);
    }
    std::optional<Reader> requireChild(std::string_view key) {
        std::optional<Reader> c = child(key);
        if (!c) errorAt(key, "missing");
        return c;
    }
    Reader element(std::uint32_t i) const { return Reader(v_[i], indexPath(path_, i), *diag_); }

    // Each get returns true when the key is present and well-typed (then `out` is set).
    bool get(std::string_view key, float& out) {
        std::optional<Reader> c = child(key);
        if (!c) return false;
        if (!c->v_.isNumber()) return c->error("expected a number"), false;
        out = static_cast<float>(c->v_.number());
        return true;
    }
    bool get(std::string_view key, bool& out) {
        std::optional<Reader> c = child(key);
        if (!c) return false;
        if (!c->v_.isBool()) return c->error("expected true or false"), false;
        out = c->v_.boolean();
        return true;
    }
    bool get(std::string_view key, int& out) {
        std::optional<Reader> c = child(key);
        if (!c) return false;
        const std::int64_t n = c->v_.integer();
        if (!c->v_.isInteger() || n < std::numeric_limits<int>::min() || n > std::numeric_limits<int>::max())
            return c->error("expected an integer"), false;
        out = static_cast<int>(n);
        return true;
    }
    bool get(std::string_view key, std::uint32_t& out) {
        std::optional<Reader> c = child(key);
        if (!c) return false;
        return c->readUint(out);
    }
    bool get(std::string_view key, std::string& out) {
        std::optional<Reader> c = child(key);
        if (!c) return false;
        if (!c->v_.isString()) return c->error("expected a string"), false;
        out = std::string(c->v_.string());
        return true;
    }
    template <class T>
    bool require(std::string_view key, T& out) {
        if (!has(key)) return errorAt(key, "missing"), false;
        return get(key, out);
    }
    template <class E>
    bool getEnum(std::string_view key, E& out, std::initializer_list<std::pair<std::string_view, E>> names) {
        std::optional<Reader> c = child(key);
        if (!c) return false;
        if (!c->v_.isString()) return c->error("expected a string"), false;
        const std::string_view s = c->v_.string();
        for (const auto& n : names)
            if (n.first == s) return out = n.second, true;
        std::string message = "unknown value \"" + std::string(s) + "\" (";
        bool first = true;
        for (const auto& n : names) {
            if (!first) message += ", ";
            message += n.first;
            first = false;
        }
        c->error(message + ")");
        return false;
    }

    bool readUint(std::uint32_t& out) const {
        const std::int64_t n = v_.integer();
        if (!v_.isInteger() || n < 0 || n > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()))
            return error("expected a non-negative integer"), false;
        out = static_cast<std::uint32_t>(n);
        return true;
    }

    // Warns about every key nobody read.
    void finish() const {
        for (std::size_t i = 0; i < used_.size(); ++i)
            if (!used_[i])
                diag_->warnings.push_back(
                    withPath(joinPath(path_, v_.key(static_cast<std::uint32_t>(i))), "unknown key (ignored)"));
    }

private:
    int find(std::string_view key) const {
        if (!v_.isObject()) return -1;
        for (std::uint32_t i = 0; i < v_.size(); ++i)
            if (v_.key(i) == key) return static_cast<int>(i);
        return -1;
    }

    JsonValue v_;
    std::string path_;
    Diagnostics* diag_;
    std::vector<char> used_;
};

bool readNumber(const Reader& v, float& out) {
    if (!v.json().isNumber()) return v.error("expected a number"), false;
    out = static_cast<float>(v.json().number());
    return true;
}

bool readNumbers(const Reader& v, float* out, std::uint32_t n, std::string_view what) {
    if (!v.json().isArray() || v.size() != n) return v.error("expected " + std::string(what)), false;
    bool ok = true;
    for (std::uint32_t i = 0; i < n; ++i) ok = readNumber(v.element(i), out[i]) && ok;
    return ok;
}

void readVec2(Reader& r, std::string_view key, VfxVec2& out) {
    if (std::optional<Reader> c = r.child(key)) {
        float v[2];
        if (readNumbers(*c, v, 2, "[a, b]")) out = {v[0], v[1]};
    }
}

void readVec3(Reader& r, std::string_view key, VfxVec3& out) {
    if (std::optional<Reader> c = r.child(key)) {
        float v[3];
        if (readNumbers(*c, v, 3, "[x, y, z]")) out = {v[0], v[1], v[2]};
    }
}

bool readColor(const Reader& v, VfxColor& out) {
    float c[4];
    if (!readNumbers(v, c, 4, "[r, g, b, a]")) return false;
    out = {c[0], c[1], c[2], c[3]};
    return true;
}

// [{t, v}, ...]
bool readKeys(const Reader& v, std::vector<VfxKey>& out) {
    if (!v.expectArray()) return false;
    std::vector<VfxKey> keys;
    for (std::uint32_t i = 0; i < v.size(); ++i) {
        Reader e = v.element(i);
        if (!e.expectObject()) continue;
        VfxKey k;
        e.require("t", k.t);
        e.require("v", k.v);
        e.finish();
        keys.push_back(k);
    }
    std::stable_sort(keys.begin(), keys.end(), [](const VfxKey& a, const VfxKey& b) { return a.t < b.t; });
    out = std::move(keys);
    return true;
}

// [{t, color: [r, g, b, a]}, ...]
bool readStops(const Reader& v, std::vector<VfxColorStop>& out) {
    if (!v.expectArray()) return false;
    std::vector<VfxColorStop> stops;
    for (std::uint32_t i = 0; i < v.size(); ++i) {
        Reader e = v.element(i);
        if (!e.expectObject()) continue;
        VfxColorStop s;
        e.require("t", s.t);
        if (std::optional<Reader> c = e.requireChild("color")) readColor(*c, s.color);
        e.finish();
        stops.push_back(s);
    }
    std::stable_sort(stops.begin(), stops.end(),
                     [](const VfxColorStop& a, const VfxColorStop& b) { return a.t < b.t; });
    out = std::move(stops);
    return true;
}

// An empty array counts as a (keyless) key array, as in newengine.
bool isArrayOfObjects(JsonValue j) { return j.isArray() && (j.size() == 0 || j[0u].isObject()); }

// number | [min, max] | [{t, v}...] | {"curve": [...], "mul"} | {"min": [...], "max": [...], "mul"}
void readCurveValue(Reader& v, VfxCurve& out) {
    const JsonValue j = v.json();
    if (j.isNumber()) {
        out = VfxCurve::constant(static_cast<float>(j.number()));
    } else if (isArrayOfObjects(j)) {
        std::vector<VfxKey> keys;
        if (readKeys(v, keys)) out = VfxCurve::curve(std::move(keys));
    } else if (j.isArray()) {
        float mm[2];
        if (readNumbers(v, mm, 2, "a number, [min, max], a key array [{t, v}] or a curve object")) {
            VfxCurve c;
            c.mode = VfxCurveMode::TwoConstants; // keeps the [a, b] form even when a == b
            c.lo = mm[0];
            c.hi = mm[1];
            out = c;
        }
    } else if (j.isObject()) {
        VfxCurve c;
        if (v.has("curve")) {
            c.mode = VfxCurveMode::Curve;
            if (std::optional<Reader> k = v.child("curve")) readKeys(*k, c.keys);
        } else if (v.has("min") || v.has("max")) {
            c.mode = VfxCurveMode::TwoCurves;
            if (std::optional<Reader> k = v.requireChild("min")) readKeys(*k, c.keysLo);
            if (std::optional<Reader> k = v.requireChild("max")) readKeys(*k, c.keys);
        } else {
            v.error("expected {\"curve\": [...]} or {\"min\": [...], \"max\": [...]}");
            return;
        }
        v.get("mul", c.multiplier);
        v.finish();
        out = std::move(c);
    } else {
        v.error("expected a number, [min, max], a key array [{t, v}] or a curve object");
    }
}

void readCurve(Reader& r, std::string_view key, VfxCurve& out) {
    if (std::optional<Reader> c = r.child(key)) readCurveValue(*c, out);
}

void readCurves3(Reader& r, std::string_view key, VfxCurve (&out)[3]) {
    std::optional<Reader> c = r.child(key);
    if (!c) return;
    if (!c->json().isArray() || c->size() != 3) return c->error("expected [x, y, z] (each a number or curve)");
    for (std::uint32_t i = 0; i < 3; ++i) {
        Reader e = c->element(i);
        readCurveValue(e, out[i]);
    }
}

// [r,g,b,a] | [{t,color}...] | {"between": [c, c]} | {"min": [...], "max": [...]} | {"random": [...]}
void readGradientValue(Reader& v, VfxGradient& out) {
    const JsonValue j = v.json();
    if (isArrayOfObjects(j)) {
        VfxGradient g;
        if (readStops(v, g.stops)) out = std::move(g);
    } else if (j.isArray()) {
        VfxColor c;
        if (readColor(v, c)) out = VfxGradient::color(c);
    } else if (j.isObject()) {
        VfxGradient g;
        if (std::optional<Reader> b = v.child("between")) {
            g.mode = VfxGradientMode::TwoColors;
            if (!b->json().isArray() || b->size() != 2) {
                b->error("expected [[r,g,b,a], [r,g,b,a]]");
            } else {
                readColor(b->element(0), g.colorLo);
                readColor(b->element(1), g.colorHi);
            }
        } else if (std::optional<Reader> rnd = v.child("random")) {
            g.mode = VfxGradientMode::RandomColor;
            readStops(*rnd, g.stops);
        } else if (v.has("min") || v.has("max")) {
            g.mode = VfxGradientMode::TwoGradients;
            if (std::optional<Reader> s = v.requireChild("min")) readStops(*s, g.stopsLo);
            if (std::optional<Reader> s = v.requireChild("max")) readStops(*s, g.stops);
        } else {
            v.error("expected {\"between\": ...}, {\"min\": ..., \"max\": ...} or {\"random\": ...}");
            return;
        }
        v.finish();
        out = std::move(g);
    } else {
        v.error("expected [r, g, b, a], a stop array [{t, color}] or a gradient object");
    }
}

void readGradient(Reader& r, std::string_view key, VfxGradient& out) {
    if (std::optional<Reader> c = r.child(key)) readGradientValue(*c, out);
}

// A module object that is present is enabled unless it says "enabled": false.
template <class M, class F>
void readModule(Reader& r, std::string_view key, M& m, F&& body) {
    std::optional<Reader> c = r.child(key);
    if (!c || !c->expectObject()) return;
    m.enabled = true;
    c->get("enabled", m.enabled);
    body(*c);
    c->finish();
}

void readSpace(Reader& r, VfxSpace& s) {
    r.getEnum("space", s, {{"world", VfxSpace::World}, {"local", VfxSpace::Local}});
}

constexpr std::string_view kInheritNames[] = {"color", "size", "rotation", "velocity"}; // kVfxInherit* order

void readSubEmitter(Reader& e, VfxSubEmitter& s) {
    if (!e.expectObject()) return;
    e.require("emitter", s.emitter);
    e.getEnum("on", s.on, {{"birth", VfxSubEmitterEvent::Birth}, {"death", VfxSubEmitterEvent::Death}});
    if (std::optional<Reader> c = e.child("count")) {
        if (c->json().isArray()) {
            std::uint32_t lo = 0, hi = 0;
            if (c->size() != 2 || !c->element(0).readUint(lo) || !c->element(1).readUint(hi) || lo > hi)
                c->error("expected n or [min, max] with min <= max");
            else
                s.countMin = lo, s.countMax = hi;
        } else if (std::uint32_t n = 0; c->readUint(n)) {
            s.countMin = s.countMax = n;
        }
    }
    if (e.get("probability", s.probability) && (s.probability < 0.f || s.probability > 1.f))
        e.errorAt("probability", "expected 0..1");
    if (std::optional<Reader> list = e.child("inherit"); list && list->expectArray()) {
        s.inherit = 0;
        for (std::uint32_t i = 0; i < list->size(); ++i) {
            const Reader n = list->element(i);
            if (!n.json().isString()) {
                n.error("expected a string");
                continue;
            }
            const std::string_view name = n.json().string();
            bool found = false;
            for (std::uint32_t b = 0; b < 4; ++b)
                if (name == kInheritNames[b]) s.inherit |= 1u << b, found = true;
            if (!found) n.error("unknown property \"" + std::string(name) + "\" (color, size, rotation, velocity)");
        }
    }
    e.get("velocityScale", s.velocityScale);
    e.finish();
}

void readEmitter(Reader& r, VfxEmitter& d) {
    if (!r.expectObject()) return;
    r.get("name", d.name);
    r.get("rate", d.rate);
    if (std::optional<Reader> b = r.child("bursts"); b && b->expectArray()) {
        d.bursts.clear();
        for (std::uint32_t i = 0; i < b->size(); ++i) {
            Reader e = b->element(i);
            if (!e.expectObject()) continue;
            VfxBurst burst;
            e.get("time", burst.time);
            e.require("count", burst.count);
            e.finish();
            d.bursts.push_back(burst);
        }
    }
    r.get("duration", d.duration);
    r.get("loop", d.loop);
    r.get("maxParticles", d.maxParticles);
    readCurve(r, "lifetime", d.lifetime);
    r.getEnum("shape", d.shape,
              {{"point", VfxShape::Point},
               {"sphere", VfxShape::Sphere},
               {"cone", VfxShape::Cone},
               {"box", VfxShape::Box},
               {"ring", VfxShape::Ring}});
    r.get("radius", d.radius);
    r.get("innerRadius", d.innerRadius);
    r.get("coneAngle", d.coneAngle);
    readVec3(r, "boxHalfExtents", d.boxHalfExtents);
    readCurve(r, "speed", d.speed);
    readVec3(r, "velocityMin", d.velocityMin);
    readVec3(r, "velocityMax", d.velocityMax);
    readVec3(r, "gravity", d.gravity);
    r.get("drag", d.drag);
    r.get("orbit", d.orbit);
    readCurve(r, "startSize", d.startSize);
    readCurve(r, "sizeOverLife", d.sizeOverLife);
    readGradient(r, "startColor", d.startColor);
    readGradient(r, "colorOverLife", d.colorOverLife);
    readCurve(r, "startRotation", d.startRotation);
    readCurve(r, "angularVelocity", d.angularVelocity);
    r.getEnum("blend", d.blend, {{"alpha", VfxBlend::Alpha}, {"additive", VfxBlend::Additive}});
    r.getEnum("sprite", d.sprite, {{"soft", VfxSprite::Soft}, {"puff", VfxSprite::Puff}, {"spark", VfxSprite::Spark}});
    r.get("stretch", d.stretch);
    r.get("stretchMax", d.stretchMax);
    r.get("softness", d.softness);
    r.get("coreShade", d.coreShade);
    r.get("turbulence", d.turbulence);
    r.get("texture", d.texture);
    readSpace(r, d.space);
    r.getEnum("renderMode", d.renderMode,
              {{"billboard", VfxRenderMode::Billboard},
               {"stretched", VfxRenderMode::Stretched},
               {"horizontal", VfxRenderMode::Horizontal},
               {"vertical", VfxRenderMode::Vertical},
               {"mesh", VfxRenderMode::Mesh}});
    r.get("lengthScale", d.lengthScale);
    r.get("speedScale", d.speedScale);
    r.get("mesh", d.mesh);
    r.getEnum("sortMode", d.sortMode,
              {{"auto", VfxSortMode::Auto},
               {"none", VfxSortMode::None},
               {"distance", VfxSortMode::ByDistance},
               {"oldest", VfxSortMode::OldestInFront},
               {"youngest", VfxSortMode::YoungestInFront}});
    if (d.renderMode == VfxRenderMode::Mesh && d.mesh.empty()) r.errorAt("mesh", "renderMode \"mesh\" needs a mesh name");

    readModule(r, "velocityOverLifetime", d.velocityOverLifetime, [&](Reader& m) {
        VfxVelocityOverLifetime& v = d.velocityOverLifetime;
        readCurves3(m, "linear", v.linear);
        readSpace(m, v.space);
        readCurves3(m, "orbital", v.orbital);
        readVec3(m, "offset", v.offset);
        readCurve(m, "radial", v.radial);
    });
    readModule(r, "limitVelocity", d.limitVelocity, [&](Reader& m) {
        readCurve(m, "speed", d.limitVelocity.speed);
        m.get("dampen", d.limitVelocity.dampen);
        m.get("drag", d.limitVelocity.drag);
    });
    readModule(r, "forceOverLifetime", d.forceOverLifetime, [&](Reader& m) {
        readCurves3(m, "force", d.forceOverLifetime.force);
        readSpace(m, d.forceOverLifetime.space);
        m.get("randomize", d.forceOverLifetime.randomize);
    });
    readModule(r, "inheritVelocity", d.inheritVelocity, [&](Reader& m) {
        m.getEnum("mode", d.inheritVelocity.mode,
                  {{"initial", VfxInheritMode::Initial}, {"current", VfxInheritMode::Current}});
        readCurve(m, "multiplier", d.inheritVelocity.multiplier);
    });
    readModule(r, "noise", d.noise, [&](Reader& m) {
        VfxNoise& n = d.noise;
        readCurve(m, "strength", n.strength);
        m.get("frequency", n.frequency);
        m.get("scrollSpeed", n.scrollSpeed);
        m.get("damping", n.damping);
        if (m.get("octaves", n.octaves) && (n.octaves < 1 || n.octaves > 8)) m.errorAt("octaves", "expected 1..8");
        m.get("octaveMultiplier", n.octaveMultiplier);
        m.get("octaveScale", n.octaveScale);
        m.getEnum("quality", n.quality,
                  {{"low", VfxNoiseQuality::Low}, {"medium", VfxNoiseQuality::Medium}, {"high", VfxNoiseQuality::High}});
        if (std::optional<Reader> k = m.child("remap")) readKeys(*k, n.remap);
        readCurve(m, "positionAmount", n.positionAmount);
        readCurve(m, "rotationAmount", n.rotationAmount);
        readCurve(m, "sizeAmount", n.sizeAmount);
    });
    readModule(r, "colorBySpeed", d.colorBySpeed, [&](Reader& m) {
        readGradient(m, "color", d.colorBySpeed.color);
        readVec2(m, "range", d.colorBySpeed.range);
    });
    readModule(r, "sizeBySpeed", d.sizeBySpeed, [&](Reader& m) {
        readCurve(m, "size", d.sizeBySpeed.size);
        readVec2(m, "range", d.sizeBySpeed.range);
    });
    readModule(r, "rotationOverLifetime", d.rotationOverLifetime,
               [&](Reader& m) { readCurve(m, "angularVelocity", d.rotationOverLifetime.angularVelocity); });
    readModule(r, "rotationBySpeed", d.rotationBySpeed, [&](Reader& m) {
        readCurve(m, "angularVelocity", d.rotationBySpeed.angularVelocity);
        readVec2(m, "range", d.rotationBySpeed.range);
    });
    readModule(r, "textureSheet", d.textureSheet, [&](Reader& m) {
        VfxTextureSheet& s = d.textureSheet;
        if (std::optional<Reader> t = m.child("tiles")) {
            float xy[2] = {0.f, 0.f};
            const bool numbers = t->json().isArray() && t->size() == 2 && t->json()[0u].isInteger() &&
                                 t->json()[1u].isInteger();
            if (numbers) {
                xy[0] = static_cast<float>(t->json()[0u].integer());
                xy[1] = static_cast<float>(t->json()[1u].integer());
            }
            if (!numbers || xy[0] < 1.f || xy[1] < 1.f || xy[0] > 64.f || xy[1] > 64.f) {
                t->error("expected [x, y], each 1..64");
            } else {
                s.tilesX = static_cast<int>(xy[0]);
                s.tilesY = static_cast<int>(xy[1]);
            }
        }
        m.getEnum("animation", s.animation,
                  {{"wholeSheet", VfxSheetAnimation::WholeSheet}, {"singleRow", VfxSheetAnimation::SingleRow}});
        m.getEnum("rowMode", s.rowMode, {{"custom", VfxSheetRowMode::Custom}, {"random", VfxSheetRowMode::Random}});
        m.get("row", s.row);
        m.getEnum("timeMode", s.timeMode,
                  {{"lifetime", VfxSheetTimeMode::Lifetime}, {"speed", VfxSheetTimeMode::Speed}, {"fps", VfxSheetTimeMode::Fps}});
        readCurve(m, "frameOverTime", s.frameOverTime);
        readCurve(m, "startFrame", s.startFrame);
        m.get("cycles", s.cycles);
        m.get("fps", s.fps);
        readVec2(m, "speedRange", s.speedRange);
        m.get("frameBlend", s.frameBlend);
    });
    readModule(r, "trails", d.trails, [&](Reader& m) {
        VfxTrails& t = d.trails;
        m.getEnum("mode", t.mode, {{"perParticle", VfxTrailMode::PerParticle}, {"ribbon", VfxTrailMode::Ribbon}});
        if (m.get("ratio", t.ratio) && (t.ratio < 0.f || t.ratio > 1.f)) m.errorAt("ratio", "expected 0..1");
        readCurve(m, "lifetime", t.lifetime);
        m.get("minVertexDistance", t.minVertexDistance);
        if (m.get("maxPoints", t.maxPoints) && (t.maxPoints < 2 || t.maxPoints > 256))
            m.errorAt("maxPoints", "expected 2..256");
        readCurve(m, "widthOverTrail", t.widthOverTrail);
        readGradient(m, "colorOverTrail", t.colorOverTrail);
        m.get("inheritParticleColor", t.inheritParticleColor);
        m.get("sizeAffectsWidth", t.sizeAffectsWidth);
        if (m.get("ribbonCount", t.ribbonCount) && t.ribbonCount < 1) m.errorAt("ribbonCount", "expected >= 1");
    });
    if (std::optional<Reader> s = r.child("subEmitters"); s && s->expectArray()) {
        d.subEmitters.clear();
        for (std::uint32_t i = 0; i < s->size(); ++i) {
            Reader e = s->element(i);
            VfxSubEmitter sub;
            readSubEmitter(e, sub);
            d.subEmitters.push_back(std::move(sub));
        }
    }
    r.finish();
}

// Reports sub-emitter links that name an unknown emitter, the emitter itself, or close a cycle.
void validateLinks(const std::vector<VfxEmitter>& em, const std::vector<std::string>& paths, Diagnostics& diag) {
    auto find = [&](const std::string& name) -> int {
        for (std::size_t i = 0; i < em.size(); ++i)
            if (em[i].name == name) return static_cast<int>(i);
        return -1;
    };
    auto reachable = [&](int from, int to) {
        std::vector<int> stack{from};
        std::vector<char> seen(em.size(), 0);
        while (!stack.empty()) {
            const int e = stack.back();
            stack.pop_back();
            if (e == to) return true;
            if (seen[static_cast<std::size_t>(e)]) continue;
            seen[static_cast<std::size_t>(e)] = 1;
            for (const VfxSubEmitter& s : em[static_cast<std::size_t>(e)].subEmitters) {
                const int c = find(s.emitter);
                if (c >= 0 && c != e) stack.push_back(c);
            }
        }
        return false;
    };
    for (std::size_t i = 0; i < em.size(); ++i) {
        for (std::size_t k = 0; k < em[i].subEmitters.size(); ++k) {
            const std::string& name = em[i].subEmitters[k].emitter;
            const std::string p =
                joinPath(indexPath(joinPath(paths[i], "subEmitters"), static_cast<std::uint32_t>(k)), "emitter");
            const int c = find(name);
            if (c < 0)
                diag.errors.push_back(withPath(p, "unknown emitter \"" + name + "\""));
            else if (c == static_cast<int>(i))
                diag.errors.push_back(withPath(p, "an emitter cannot be its own sub-emitter"));
            else if (reachable(c, static_cast<int>(i)))
                diag.errors.push_back(withPath(p, "sub-emitter cycle through \"" + name + "\""));
        }
    }
}

void readEffect(Reader& r, VfxEffect& out) {
    if (!r.expectObject()) return;
    if (!r.has("emitters")) {
        VfxEmitter single;
        readEmitter(r, single);
        out.name = single.name;
        out.emitters.assign(1, std::move(single));
        validateLinks(out.emitters, {r.path()}, r.diagnostics());
        return;
    }
    r.get("name", out.name);
    if (std::optional<Reader> list = r.child("emitters"); list && list->expectArray()) {
        out.emitters.clear();
        std::vector<std::string> paths;
        for (std::uint32_t i = 0; i < list->size(); ++i) {
            Reader e = list->element(i);
            VfxEmitter d;
            readEmitter(e, d);
            out.emitters.push_back(std::move(d));
            paths.push_back(e.path());
        }
        validateLinks(out.emitters, paths, r.diagnostics());
    }
    r.finish();
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string s;
    for (const std::string& l : lines) {
        if (!s.empty()) s += '\n';
        s += l;
    }
    return s;
}

} // namespace

bool importVfxEffect(std::string_view json, VfxEffect& out, std::string* error, std::vector<std::string>* warnings) {
    out = VfxEffect{};
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(json, &parseError)) {
        if (error) *error = parseError;
        return false;
    }
    Diagnostics diag;
    Reader root(doc.root(), "", diag);
    readEffect(root, out);
    if (warnings) warnings->insert(warnings->end(), diag.warnings.begin(), diag.warnings.end());
    if (diag.errors.empty() && out.emitters.empty()) diag.errors.push_back("emitters: expected at least one emitter");
    if (!diag.errors.empty()) {
        if (error) *error = joinLines(diag.errors);
        out = VfxEffect{};
        return false;
    }
    return true;
}

int findVfxEmitter(const VfxEffect& effect, std::string_view name) {
    for (std::size_t i = 0; i < effect.emitters.size(); ++i)
        if (effect.emitters[i].name == name) return static_cast<int>(i);
    return -1;
}

// ---- Niagara plan -------------------------------------------------------------------------------------

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegPerRad = 180.f / kPi;

VfxVec3 add(const VfxVec3& a, const VfxVec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
VfxVec3 scale(const VfxVec3& a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float length(const VfxVec3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
bool isZero(const VfxVec3& a) { return a.x == 0.f && a.y == 0.f && a.z == 0.f; }
VfxVec3 absMax(const VfxVec3& a, const VfxVec3& b) {
    return {std::max(std::fabs(a.x), std::fabs(b.x)), std::max(std::fabs(a.y), std::fabs(b.y)),
            std::max(std::fabs(a.z), std::fabs(b.z))};
}

// A newengine box (lo, hi) in Unreal: the swap keeps each axis's lo <= hi.
void boxToUnreal(VfxVec3 lo, VfxVec3 hi, VfxVec3& outLo, VfxVec3& outHi) {
    const VfxVec3 a = vfxToUnreal(lo), b = vfxToUnreal(hi);
    outLo = {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
    outHi = {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

VfxRange curveRange(const VfxCurve& c, float k) {
    float lo = 0.f, hi = 0.f;
    c.bounds(lo, hi);
    lo *= k;
    hi *= k;
    return lo <= hi ? VfxRange{lo, hi} : VfxRange{hi, lo};
}

float mid(const VfxRange& r) { return 0.5f * (r.lo + r.hi); }

std::string fmt(float v) {
    // Short, locale-free formatting for gap messages: up to 3 decimals, trailing zeros dropped.
    const long long scaled = std::llround(static_cast<double>(v) * 1000.0);
    const bool neg = scaled < 0;
    const long long a = neg ? -scaled : scaled;
    std::string s = std::to_string(a / 1000);
    long long frac = a % 1000;
    if (frac != 0) {
        std::string f = std::to_string(frac);
        while (f.size() < 3) f.insert(f.begin(), '0');
        while (!f.empty() && f.back() == '0') f.pop_back();
        s += "." + f;
    }
    return neg ? "-" + s : s;
}

// Mean colour of a gradient over [0, 1] (16 samples at r = 0.5).
VfxColor meanColor(const VfxGradient& g) {
    VfxColor sum{0.f, 0.f, 0.f, 0.f};
    constexpr int kSamples = 16;
    for (int i = 0; i < kSamples; ++i) {
        const VfxColor c = g.evaluate((static_cast<float>(i) + 0.5f) / static_cast<float>(kSamples), 0.5f);
        sum = {sum.r + c.r, sum.g + c.g, sum.b + c.b, sum.a + c.a};
    }
    const float k = 1.f / static_cast<float>(kSamples);
    return {sum.r * k, sum.g * k, sum.b * k, sum.a * k};
}

// The two curves of a TwoCurves value averaged into one key list (at every key time of either).
std::vector<VfxKey> averageCurves(const VfxCurve& c) {
    std::vector<float> times;
    for (const VfxKey& k : c.keys) times.push_back(k.t);
    for (const VfxKey& k : c.keysLo) times.push_back(k.t);
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    std::vector<VfxKey> out;
    for (float t : times) out.push_back({t, c.evaluate(t, 0.5f)});
    return out;
}

void planSizeOverLife(const VfxEmitter& e, VfxNiagaraEmitter& p) {
    const VfxCurve& c = e.sizeOverLife;
    std::vector<VfxKey> keys;
    switch (c.mode) {
    case VfxCurveMode::Curve:
        if (c.keys.empty()) {
            if (c.multiplier != 1.f) keys.push_back({0.f, c.multiplier});
        } else {
            for (const VfxKey& k : c.keys) keys.push_back({k.t, k.v * c.multiplier});
        }
        break;
    case VfxCurveMode::Constant:
    case VfxCurveMode::TwoConstants:
        keys.push_back({0.f, c.mean()});
        if (c.lo != c.hi) p.gaps.push_back("sizeOverLife: a random constant per particle; its mean is used");
        break;
    case VfxCurveMode::TwoCurves:
        keys = averageCurves(c);
        p.gaps.push_back("sizeOverLife: a random blend of two curves; their average is used");
        break;
    }
    bool identity = true;
    for (const VfxKey& k : keys) identity = identity && k.v == 1.f;
    if (!identity) p.sizeOverLife = std::move(keys);
}

void planColors(const VfxEmitter& e, VfxNiagaraEmitter& p) {
    // Start colour (InitializeParticle Color: a constant, or a random pick along stops).
    const VfxGradient& s = e.startColor;
    switch (s.mode) {
    case VfxGradientMode::Color: p.startColor = {{0.f, s.colorHi}}; break;
    case VfxGradientMode::TwoColors:
        p.startColor = {{0.f, s.colorLo}, {1.f, s.colorHi}};
        p.startColorRandom = true;
        break;
    case VfxGradientMode::RandomColor:
        p.startColor = s.stops;
        p.startColorRandom = true;
        break;
    case VfxGradientMode::Gradient:
        p.startColor = s.stops;
        p.startColorRandom = true;
        p.gaps.push_back("startColor: follows the emitter cycle in newengine; picked at random along it instead");
        break;
    case VfxGradientMode::TwoGradients:
        p.startColor = s.stops;
        p.startColorRandom = true;
        p.gaps.push_back("startColor: two gradients over the cycle; a random pick along the upper one is used");
        break;
    }
    if (p.startColor.empty()) p.startColor = {{0.f, VfxColor{}}};
    if (p.startColor.size() == 1) p.startColorRandom = false;

    // Colour over life (ScaleColor).
    const VfxGradient& g = e.colorOverLife;
    if (g.isWhite()) return;
    switch (g.mode) {
    case VfxGradientMode::Gradient: p.colorOverLife = g.stops; break;
    case VfxGradientMode::Color: p.colorOverLife = {{0.f, g.colorHi}}; break;
    case VfxGradientMode::TwoColors:
        p.colorOverLife = {{0.f, meanColor(g)}};
        p.gaps.push_back("colorOverLife: a random blend of two colours; their mean is used");
        break;
    case VfxGradientMode::TwoGradients:
        for (int i = 0; i <= 8; ++i) {
            const float t = static_cast<float>(i) / 8.f;
            p.colorOverLife.push_back({t, g.evaluate(t, 0.5f)});
        }
        p.gaps.push_back("colorOverLife: a random blend of two gradients; their average is used");
        break;
    case VfxGradientMode::RandomColor:
        p.gaps.push_back("colorOverLife: a random colour per particle is not mapped (left white)");
        break;
    }
}

void planShapeAndVelocity(const VfxEmitter& e, VfxNiagaraEmitter& p) {
    const VfxVec3 up{0.f, 0.f, 1.f};
    const VfxRange speed = curveRange(e.speed, kVfxUnrealUnitsPerMetre);
    if (!e.speed.isConstant()) p.gaps.push_back("speed: varies over the emitter cycle; its full range is used");
    VfxVec3 boxLo, boxHi;
    boxToUnreal(e.velocityMin, e.velocityMax, boxLo, boxHi);

    // Velocity over lifetime: its linear part becomes start velocity.
    const VfxVelocityOverLifetime& vol = e.velocityOverLifetime;
    if (vol.enabled) {
        const VfxVec3 linear =
            vfxToUnreal({vol.linear[0].mean(), vol.linear[1].mean(), vol.linear[2].mean()});
        if (!isZero(linear)) {
            boxLo = add(boxLo, linear);
            boxHi = add(boxHi, linear);
            if (!vol.linear[0].isConstant() || !vol.linear[1].isConstant() || !vol.linear[2].isConstant())
                p.gaps.push_back("velocityOverLifetime.linear: curves; their mean is used");
            if (e.drag != 0.f)
                p.gaps.push_back("velocityOverLifetime.linear: becomes start velocity, which drag now slows");
        }
    }

    const bool hasSpeed = speed.lo != 0.f || speed.hi != 0.f;
    const bool hasBox = !isZero(boxLo) || !isZero(boxHi);
    const VfxVec3 boxMean = scale(add(boxLo, boxHi), 0.5f);
    const float speedMean = mid(speed);
    const float r = e.radius * kVfxUnrealUnitsPerMetre;

    auto linearBox = [&] {
        p.velocity = VfxNiagaraVelocity::Linear;
        p.linearLo = boxLo;
        p.linearHi = boxHi;
    };
    // Outward speed plus a velocity box: move the point the speed pushes away from so that, at the
    // shape's typical radius, outward + box mean comes out in the right direction.
    auto fromPoint = [&](float refRadius) {
        p.velocity = VfxNiagaraVelocity::FromPoint;
        p.speed = speed;
        if (!hasBox) return;
        if (refRadius > 0.f && speedMean > 0.f) {
            const VfxVec3 m = scale(boxMean, 1.f / speedMean);
            p.pointOrigin = scale(m, -refRadius);
            const float k = std::sqrt(1.f + length(m) * length(m));
            p.speed = {speed.lo * k, speed.hi * k};
            p.gaps.push_back("velocityMin/Max: folded into the velocity's point origin (outward speed tilted by the box mean)");
            if (!(boxLo == boxHi)) p.gaps.push_back("velocityMin/Max: only the box mean is kept, not its spread");
        } else {
            p.gaps.push_back("velocityMin/Max: dropped (a point shape with speed takes one velocity type)");
        }
    };

    switch (e.shape) {
    case VfxShape::Point:
        if (hasSpeed) fromPoint(0.f);
        else if (hasBox) linearBox();
        break;
    case VfxShape::Sphere:
        p.shape = VfxNiagaraShape::Sphere;
        p.sphereRadius = r;
        if (hasSpeed) fromPoint(0.75f * r); // mean distance from the centre, uniform in volume
        else if (hasBox) linearBox();
        break;
    case VfxShape::Ring:
        p.shape = VfxNiagaraShape::Ring;
        p.ringRadius = r;
        p.discCoverage = e.radius > 0.f ? std::clamp(1.f - e.innerRadius / e.radius, 0.f, 1.f) : 0.f;
        if (hasSpeed) fromPoint(0.5f * (e.radius + e.innerRadius) * kVfxUnrealUnitsPerMetre);
        else if (hasBox) linearBox();
        break;
    case VfxShape::Cone:
        if (r > 0.f) {
            p.shape = VfxNiagaraShape::Ring; // newengine spawns on the cone's base disc
            p.ringRadius = r;
            p.discCoverage = 1.f;
        }
        if (hasSpeed) {
            p.velocity = VfxNiagaraVelocity::Cone;
            p.coneAngle = 2.f * e.coneAngle; // Niagara's ConeAngle is the full angle
            p.speed = speed;
            if (hasBox && speedMean > 0.f) {
                const VfxVec3 dir = add(scale(up, speedMean), boxMean);
                const float len = length(dir);
                p.coneDirection = scale(dir, 1.f / len);
                p.speed = {speed.lo * len / speedMean, speed.hi * len / speedMean};
                p.gaps.push_back("velocityMin/Max: folded into the cone direction (tilted by the box mean)");
                if (!(boxLo == boxHi)) p.gaps.push_back("velocityMin/Max: only the box mean is kept, not its spread");
            }
        } else if (hasBox) {
            linearBox();
        }
        break;
    case VfxShape::Box: {
        p.shape = VfxNiagaraShape::Box;
        const VfxVec3 size = vfxToUnreal(scale(e.boxHalfExtents, 2.f));
        p.boxSize = {std::fabs(size.x), std::fabs(size.y), std::fabs(size.z)};
        if (hasSpeed || hasBox) {
            p.velocity = VfxNiagaraVelocity::Linear; // newengine's box shape pushes along +Y (up)
            p.linearLo = add(scale(up, speed.lo), boxLo);
            p.linearHi = add(scale(up, speed.hi), boxHi);
        }
        break;
    }
    }

    // Orbital velocity about the vertical axis: RotateAroundPoint (a horizontal circle about the centre).
    if (vol.enabled) {
        const float omega = vol.orbital[1].mean();
        if (omega != 0.f) {
            const VfxVec3 center = vfxToUnreal(vol.offset);
            const float meanLife = e.lifetime.mean();
            bool mapped = true;
            if (e.shape == VfxShape::Ring) {
                p.orbitRadius = {e.innerRadius * kVfxUnrealUnitsPerMetre, r};
                p.orbitPhase = {0.f, 2.f * kPi};
                p.shape = VfxNiagaraShape::None; // the module places the particles on the circle
            } else if (e.shape == VfxShape::Point) {
                const float dx = -center.x, dy = -center.y;
                const float radius = std::sqrt(dx * dx + dy * dy);
                p.orbitRadius = {radius, radius};
                const float phase = radius > 0.f ? std::atan2(dy, dx) : 0.f;
                p.orbitPhase = {phase, phase};
            } else {
                mapped = false;
                p.gaps.push_back("velocityOverLifetime.orbital: only mapped for point and ring shapes");
            }
            if (mapped) {
                p.rotateAroundPoint = true;
                p.orbitCenter = center;
                // Degrees per lifetime (the module scales by normalised age). The axis swap mirrors, so
                // a turn about newengine's +Y is the opposite turn about Unreal's +Z.
                p.orbitRate = -omega * kDegPerRad * meanLife;
                const VfxRange life = curveRange(e.lifetime, 1.f);
                if (life.lo != life.hi)
                    p.gaps.push_back("velocityOverLifetime.orbital: angular speed is exact at the mean lifetime only");
                if (!vol.orbital[1].isConstant())
                    p.gaps.push_back("velocityOverLifetime.orbital: a curve; its mean is used");
            }
        }
        if (vol.orbital[0].mean() != 0.f || vol.orbital[2].mean() != 0.f)
            p.gaps.push_back("velocityOverLifetime.orbital: rotation about horizontal axes is not mapped");
        if (!(vol.radial == VfxCurve{}))
            p.gaps.push_back("velocityOverLifetime.radial: not mapped");
    }
    if (e.orbit != 0.f) p.gaps.push_back("orbit: turning the velocity about the vertical axis is not mapped");
}

void planForces(const VfxEmitter& e, VfxNiagaraEmitter& p) {
    p.gravity = vfxToUnreal(e.gravity);
    p.drag = e.drag;
    if (e.forceOverLifetime.enabled) {
        const VfxCurve* f = e.forceOverLifetime.force;
        p.acceleration = vfxToUnreal({f[0].mean(), f[1].mean(), f[2].mean()});
        if (!f[0].isConstant() || !f[1].isConstant() || !f[2].isConstant())
            p.gaps.push_back("forceOverLifetime: curves over age; their mean is used as a constant acceleration");
        if (e.forceOverLifetime.randomize) p.gaps.push_back("forceOverLifetime.randomize: not mapped");
    }
    const VfxNoise& n = e.noise;
    if (n.enabled) {
        // Niagara's curl noise samples position(cm) * frequency / 50 and pushes at up to `strength` cm/s.
        p.curlNoise = true;
        p.noiseFrequency = 0.5f * n.frequency;
        float strength = n.strength.mean() * kVfxUnrealUnitsPerMetre;
        if (n.damping && n.frequency > 0.f) strength /= n.frequency;
        p.noiseStrength = strength * n.positionAmount.mean();
        if (n.scrollSpeed != 0.f) p.gaps.push_back("noise.scrollSpeed: not mapped (the field does not scroll)");
        if (n.octaves > 1) p.gaps.push_back("noise.octaves: one octave only");
        if (!(n.sizeAmount == VfxCurve::constant(0.f))) p.gaps.push_back("noise.sizeAmount: not mapped");
        if (!(n.rotationAmount == VfxCurve::constant(0.f))) p.gaps.push_back("noise.rotationAmount: not mapped");
        if (!n.remap.empty()) p.gaps.push_back("noise.remap: not mapped");
    }
    if (e.limitVelocity.enabled) p.gaps.push_back("limitVelocity: not mapped (no speed limit in Lightweight emitters)");
    if (e.inheritVelocity.enabled) p.gaps.push_back("inheritVelocity: not mapped");
}

void planLook(const VfxEmitter& e, VfxNiagaraEmitter& p) {
    p.spriteSize = curveRange(e.startSize, kVfxUnrealUnitsPerMetre);
    if (!e.startSize.isConstant()) p.gaps.push_back("startSize: varies over the emitter cycle; its full range is used");
    p.spriteRotation = curveRange(e.startRotation, kDegPerRad);
    VfxRange rate = curveRange(e.angularVelocity, kDegPerRad);
    if (e.rotationOverLifetime.enabled) {
        const VfxRange extra = curveRange(e.rotationOverLifetime.angularVelocity, 1.f);
        rate = {rate.lo + extra.lo, rate.hi + extra.hi};
        if (!e.rotationOverLifetime.angularVelocity.isConstant())
            p.gaps.push_back("rotationOverLifetime: a curve over age; its range is used as a constant rate");
    }
    p.rotationRate = rate;
    planSizeOverLife(e, p);
    planColors(e, p);

    p.additive = e.blend == VfxBlend::Additive;
    p.sprite = e.sprite;

    // Typical and largest start speed, for the velocity stretch.
    float vmax = 0.f;
    switch (p.velocity) {
    case VfxNiagaraVelocity::None: break;
    case VfxNiagaraVelocity::Linear: vmax = length(absMax(p.linearLo, p.linearHi)); break;
    case VfxNiagaraVelocity::FromPoint:
    case VfxNiagaraVelocity::Cone: vmax = std::max(std::fabs(p.speed.lo), std::fabs(p.speed.hi)); break;
    }
    const float meanSize = std::max(mid(p.spriteSize), 0.01f);
    switch (e.renderMode) {
    case VfxRenderMode::Billboard:
        if (e.stretch > 0.f) {
            p.facing = VfxNiagaraFacing::VelocityAligned;
            if (vmax > 0.f) {
                p.scaleBySpeed = true;
                p.speedThreshold = vmax;
                p.maxScale = {1.f, std::min(e.stretchMax, 1.f + vmax * e.stretch / meanSize)};
            }
        }
        break;
    case VfxRenderMode::Stretched:
        p.facing = VfxNiagaraFacing::VelocityAligned;
        p.scaleBySpeed = true;
        p.minScale = {1.f, e.lengthScale};
        p.maxScale = {1.f, e.lengthScale + vmax * e.speedScale / meanSize};
        p.speedThreshold = vmax > 0.f ? vmax : 1.f;
        break;
    case VfxRenderMode::Horizontal: p.facing = VfxNiagaraFacing::Horizontal; break;
    case VfxRenderMode::Vertical: p.facing = VfxNiagaraFacing::Vertical; break;
    case VfxRenderMode::Mesh:
        p.gaps.push_back("renderMode mesh (\"" + e.mesh + "\"): drawn as camera-facing sprites");
        break;
    }

    if (e.sprite == VfxSprite::Puff) p.gaps.push_back("sprite puff: drawn with the soft round sprite (no eroded cloud)");
    if (!e.texture.empty()) p.gaps.push_back("texture \"" + e.texture + "\": not mapped (procedural sprite used)");
    if (e.textureSheet.enabled) p.gaps.push_back("textureSheet: not mapped (needs a flipbook texture)");
    if (e.softness > 0.f) p.gaps.push_back("softness: no soft-particle depth fade");
    if (e.trails.enabled) p.gaps.push_back("trails: not mapped (Lightweight emitters have no ribbons)");
    if (e.colorBySpeed.enabled) p.gaps.push_back("colorBySpeed: not mapped");
    if (e.sizeBySpeed.enabled) p.gaps.push_back("sizeBySpeed: not mapped");
    if (e.rotationBySpeed.enabled) p.gaps.push_back("rotationBySpeed: not mapped");
    if (e.sortMode == VfxSortMode::OldestInFront || e.sortMode == VfxSortMode::YoungestInFront)
        p.gaps.push_back("sortMode by age: not mapped");
}

// Where a parent particle is expected to die (mean start velocity, gravity and acceleration, drag).
VfxVec3 expectedDeathPosition(const VfxEmitter& e, const VfxNiagaraEmitter& p) {
    VfxVec3 v0;
    switch (p.velocity) {
    case VfxNiagaraVelocity::None:
    case VfxNiagaraVelocity::FromPoint: break; // symmetric about the origin
    case VfxNiagaraVelocity::Linear: v0 = scale(add(p.linearLo, p.linearHi), 0.5f); break;
    case VfxNiagaraVelocity::Cone: v0 = scale(p.coneDirection, mid(p.speed)); break;
    }
    const VfxVec3 g = add(p.gravity, p.acceleration);
    const float t = e.lifetime.mean();
    const float k = p.drag;
    float f = t, h = 0.5f * t * t;
    if (k > 0.f) {
        f = (1.f - std::exp(-k * t)) / k;
        h = (t - f) / k;
    }
    return add(p.shapeOffset, add(scale(v0, f), scale(g, h)));
}

// Sub-emitter targets spawn only through events. Death events become bursts at the parent's expected
// death time and place.
void planSubEmitters(const VfxEffect& effect, VfxNiagaraPlan& plan) {
    std::vector<char> cleared(effect.emitters.size(), 0);
    for (std::size_t i = 0; i < effect.emitters.size(); ++i) {
        const VfxEmitter& parent = effect.emitters[i];
        const VfxNiagaraEmitter& pp = plan.emitters[i];
        for (const VfxSubEmitter& s : parent.subEmitters) {
            const int ci = findVfxEmitter(effect, s.emitter);
            if (ci < 0) continue;
            VfxNiagaraEmitter& child = plan.emitters[static_cast<std::size_t>(ci)];
            if (!cleared[static_cast<std::size_t>(ci)]) {
                cleared[static_cast<std::size_t>(ci)] = 1;
                child.rate = 0.f;
                child.bursts.clear();
                child.loop = pp.loop;
                child.loopDuration = pp.loopDuration;
            }
            if (s.on == VfxSubEmitterEvent::Birth) {
                child.gaps.push_back("sub-emitter of \"" + parent.name + "\" on birth: not mapped");
                continue;
            }
            const float life = parent.lifetime.mean();
            for (const VfxNiagaraBurst& b : pp.bursts) {
                float t = b.time + life;
                if (pp.loop && pp.loopDuration > 0.f) t = std::fmod(t, pp.loopDuration);
                if (!pp.loop) child.loopDuration = std::max(child.loopDuration, t + 0.01f);
                child.bursts.push_back({t, b.countLo * s.countMin, b.countHi * s.countMax});
            }
            if (pp.rate > 0.f) {
                child.rate += pp.rate * 0.5f * static_cast<float>(s.countMin + s.countMax);
                child.gaps.push_back("sub-emitter of \"" + parent.name +
                                     "\": its continuous deaths become a continuous rate at the expected death place");
            }
            const VfxVec3 at = expectedDeathPosition(parent, pp);
            child.shapeOffset = at;
            child.pointOrigin = add(child.pointOrigin, at);
            if (child.rotateAroundPoint) child.orbitCenter = add(child.orbitCenter, at);
            if (child.shape == VfxNiagaraShape::None && !isZero(at)) {
                child.shape = VfxNiagaraShape::Sphere; // ShapeLocation carries the offset
                child.sphereRadius = 0.1f;
            }
            child.gaps.push_back("sub-emitter of \"" + parent.name + "\" on death: bursts at the parent's mean lifetime (" +
                                 fmt(life) + " s) and expected death place, not at each particle");
            if (s.inherit != 0) child.gaps.push_back("sub-emitter inherit flags: not mapped");
            if (s.probability < 1.f) child.gaps.push_back("sub-emitter probability: not mapped");
        }
    }
}

void planBounds(VfxNiagaraEmitter& p) {
    float extent = 0.f;
    switch (p.shape) {
    case VfxNiagaraShape::None: break;
    case VfxNiagaraShape::Sphere: extent = p.sphereRadius; break;
    case VfxNiagaraShape::Box: extent = 0.5f * std::max(p.boxSize.x, std::max(p.boxSize.y, p.boxSize.z)); break;
    case VfxNiagaraShape::Ring: extent = p.ringRadius; break;
    }
    float vmax = 0.f;
    switch (p.velocity) {
    case VfxNiagaraVelocity::None: break;
    case VfxNiagaraVelocity::Linear: vmax = length(absMax(p.linearLo, p.linearHi)); break;
    case VfxNiagaraVelocity::FromPoint:
    case VfxNiagaraVelocity::Cone: vmax = std::max(std::fabs(p.speed.lo), std::fabs(p.speed.hi)); break;
    }
    const float t = std::max(p.lifetime.hi, 0.f);
    const float travel = p.drag > 0.f ? (1.f - std::exp(-p.drag * t)) / p.drag : t;
    float sizeScale = 1.f;
    for (const VfxKey& k : p.sizeOverLife) sizeScale = std::max(sizeScale, k.v);
    const float stretch = std::max(1.f, std::max(p.maxScale.y, p.minScale.y));
    const float reach = extent + vmax * travel + p.noiseStrength * t +
                        (p.rotateAroundPoint ? std::max(p.orbitRadius.hi, p.orbitRadius.lo) : 0.f) +
                        0.5f * p.spriteSize.hi * sizeScale * stretch;
    const VfxVec3 g = scale(add(p.gravity, p.acceleration), 0.5f * t * t);
    const VfxVec3 c = add(p.shapeOffset, p.rotateAroundPoint ? p.orbitCenter : VfxVec3{});
    p.boundsMin = add(c, {std::min(0.f, g.x) - reach, std::min(0.f, g.y) - reach, std::min(0.f, g.z) - reach});
    p.boundsMax = add(c, {std::max(0.f, g.x) + reach, std::max(0.f, g.y) + reach, std::max(0.f, g.z) + reach});
}

} // namespace

void planVfxNiagara(const VfxEffect& effect, VfxNiagaraPlan& out) {
    out = VfxNiagaraPlan{};
    out.name = effect.name;
    for (const VfxEmitter& e : effect.emitters) {
        VfxNiagaraEmitter p;
        p.name = e.name;
        p.loop = e.loop;
        p.loopDuration = std::max(e.duration, 0.001f);
        p.rate = e.rate;
        for (const VfxBurst& b : e.bursts) p.bursts.push_back({b.time, b.count, b.count});
        p.lifetime = curveRange(e.lifetime, 1.f);
        if (!e.lifetime.isConstant()) p.gaps.push_back("lifetime: varies over the emitter cycle; its full range is used");
        planShapeAndVelocity(e, p);
        planForces(e, p);
        planLook(e, p);
        out.emitters.push_back(std::move(p));
    }
    planSubEmitters(effect, out);
    for (VfxNiagaraEmitter& p : out.emitters) {
        planBounds(p);
        if (p.loop) out.loops = true;
        else out.duration = std::max(out.duration, p.loopDuration + p.lifetime.hi);
    }
}

} // namespace ftcore
