#include "ftcore/ArrowLane.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

float clamp01(double v) { return static_cast<float>(std::clamp(v, 0.0, 1.0)); }

// Reading helpers: `ok` turns false (with the member's path) on a value of the wrong type or out of range.
struct Reader {
    std::string* error;
    bool ok = true;

    void fail(const char* path, const char* why) {
        if (!ok) return;
        ok = false;
        if (error) *error = std::string(path) + ": " + why;
    }
    // A number member; absent keeps `v`. Must lie in [lo, hi].
    template <typename T>
    void number(const JsonValue& obj, const char* name, T& v, double lo, double hi) {
        if (!ok) return;
        const JsonValue m = obj[name];
        if (m.isAbsent()) return;
        if (!m.isNumber()) return fail(name, "not a number");
        const double d = m.number();
        if (!(d >= lo && d <= hi)) return fail(name, "out of range");
        v = static_cast<T>(d);
    }
    void rect(const JsonValue& obj, const char* name, LaneRect& r) {
        if (!ok) return;
        const JsonValue m = obj[name];
        if (m.isAbsent()) return;
        if (!m.isObject()) return fail(name, "not an object");
        const JsonValue a = m["anchor"];
        if (!a.isAbsent()) {
            const std::string_view s = a.string();
            if (s == "TL") r.anchor = LaneAnchor::TopLeft;
            else if (s == "TR") r.anchor = LaneAnchor::TopRight;
            else if (s == "BL") r.anchor = LaneAnchor::BottomLeft;
            else if (s == "BR") r.anchor = LaneAnchor::BottomRight;
            else return fail(name, "anchor is not TL, TR, BL or BR");
        }
        number(m, "x", r.x, -100.0, 100.0);
        number(m, "y", r.y, -100.0, 100.0);
        number(m, "w", r.w, 0.1, 100.0);
        number(m, "h", r.h, 0.1, 100.0);
    }
    void color(const JsonValue& obj, const char* name, LaneColor& c) {
        if (!ok) return;
        const JsonValue m = obj[name];
        if (m.isAbsent()) return;
        if (!m.isArray() || (m.size() != 3 && m.size() != 4)) return fail(name, "not [r, g, b] or [r, g, b, a]");
        float v[4] = {0.f, 0.f, 0.f, c.a};
        for (std::uint32_t i = 0; i < m.size(); ++i) {
            if (!m[i].isNumber() || m[i].number() < 0.0 || m[i].number() > 1.0) return fail(name, "component out of 0..1");
            v[i] = static_cast<float>(m[i].number());
        }
        c = {v[0], v[1], v[2], v[3]};
    }
};

} // namespace

PxRect safeBox(float width, float height) {
    if (width <= 0.f || height <= 0.f) return {};
    // Wider than 16:9: the full height, the middle of the width. Narrower: the full width, the middle of the height.
    if (width * 9.f >= height * 16.f) {
        const float w = height * 16.f / 9.f;
        return {(width - w) * 0.5f, 0.f, w, height};
    }
    const float h = width * 9.f / 16.f;
    return {0.f, (height - h) * 0.5f, width, h};
}

PxRect resolve(const LaneRect& r, const PxRect& box) {
    const float ox = r.anchor == LaneAnchor::TopLeft || r.anchor == LaneAnchor::BottomLeft ? box.x : box.x + box.w;
    const float oy = r.anchor == LaneAnchor::TopLeft || r.anchor == LaneAnchor::TopRight ? box.y : box.y + box.h;
    return {ox + r.x * 0.01f * box.w, oy + r.y * 0.01f * box.h, r.w * 0.01f * box.w, r.h * 0.01f * box.h};
}

ArrowLaneLayout defaultArrowLaneLayout() { return {}; }

bool importArrowLaneLayout(std::string_view jsonText, ArrowLaneLayout& out, std::string* error) {
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(jsonText, &parseError)) {
        if (error) *error = parseError;
        return false;
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        if (error) *error = "the file is not an object";
        return false;
    }
    if (root["version"].integer(0) != 1) {
        if (error) *error = "version is not 1";
        return false;
    }
    ArrowLaneLayout l;
    Reader rd{error};

    const JsonValue g = root["geometry"];
    rd.rect(g, "plate", l.plate);
    rd.number(g, "columnInset", l.columnInset, 0.0, 20.0);
    rd.number(g, "columnPitch", l.columnPitch, 1.0, 20.0);
    rd.number(g, "targetY", l.targetY, 0.0, 100.0);
    rd.number(g, "spawnY", l.spawnY, 0.0, 100.0);
    rd.number(g, "glyphWidth", l.glyphWidth, 0.5, 20.0);
    rd.number(g, "glyphHeight", l.glyphHeight, 0.5, 20.0);
    rd.number(g, "minGlyphPx", l.minGlyphPx, 8.0, 200.0);
    rd.number(g, "perfectBandH", l.perfectBandH, 0.0, 50.0);
    rd.rect(g, "word", l.word);
    rd.number(g, "wordStartY", l.wordStartY, 0.0, 100.0);
    rd.rect(g, "streak", l.streak);
    rd.rect(g, "stamp", l.stamp);
    rd.rect(g, "caption", l.caption);

    ArrowLaneTimes& t = l.times;
    const JsonValue tm = root["timing"];
    rd.number(tm, "lookahead", t.lookahead, 0.3, 3.0);
    rd.number(tm, "leadBeats", t.leadBeats, 0.5, 8.0);
    rd.number(tm, "lookaheadMin", t.lookaheadMin, 0.3, 3.0);
    rd.number(tm, "lookaheadMax", t.lookaheadMax, 0.3, 3.0);
    rd.number(tm, "noteFadeIn", t.noteFadeIn, 0.0, 1.0);
    rd.number(tm, "laneFadeIn", t.laneFadeIn, 0.0, 2.0);
    rd.number(tm, "exitFade", t.exitFade, 0.01, 2.0);
    rd.number(tm, "exitStart", t.exitStart, 0.0, 2.0);
    rd.number(tm, "exitEnd", t.exitEnd, 0.01, 3.0);
    rd.number(tm, "quickFade", t.quickFade, 0.01, 2.0);
    rd.number(tm, "receptorDecay", t.receptorDecay, 0.01, 2.0);
    rd.number(tm, "burstPerfect", t.burstPerfect, 0.01, 2.0);
    rd.number(tm, "burstGreat", t.burstGreat, 0.01, 2.0);
    rd.number(tm, "burstGood", t.burstGood, 0.01, 2.0);
    rd.number(tm, "wordHold", t.wordHold, 0.0, 3.0);
    rd.number(tm, "wordOut", t.wordOut, 0.01, 3.0);
    rd.number(tm, "missFade", t.missFade, 0.01, 2.0);
    rd.number(tm, "streakPop", t.streakPop, 0.01, 2.0);
    rd.number(tm, "streakMissHold", t.streakMissHold, 0.0, 3.0);
    rd.number(tm, "strayQuiet", t.strayQuiet, 0.0, 2.0);
    rd.number(tm, "stampDelay", t.stampDelay, 0.0, 2.0);
    rd.number(tm, "stampPop", t.stampPop, 0.01, 2.0);
    rd.number(tm, "fullComboEdge", t.fullComboEdge, 0.0, 3.0);
    rd.number(tm, "rimHold", t.rimHold, 0.0, 3.0);
    rd.number(tm, "streakEvery", t.streakEvery, 1.0, 64.0);
    rd.number(tm, "streakFrom", t.streakFrom, 1.0, 64.0);
    rd.number(tm, "streakPopScale", t.streakPopScale, 1.0, 3.0);
    rd.number(tm, "stampFromScale", t.stampFromScale, 0.1, 1.0);
    rd.number(tm, "plateDim", t.plateDim, 0.0, 1.0);
    rd.number(tm, "previewAlpha", t.previewAlpha, 0.0, 1.0);
    rd.number(tm, "unusedAlpha", t.unusedAlpha, 0.0, 1.0);
    rd.number(tm, "burstFrom", t.burstFrom, 0.5, 30.0);
    rd.number(tm, "burstToPerfect", t.burstToPerfect, 0.5, 30.0);
    rd.number(tm, "burstToGreat", t.burstToGreat, 0.5, 30.0);
    rd.number(tm, "burstToGood", t.burstToGood, 0.5, 30.0);
    rd.number(tm, "wordRisePx", t.wordRisePx, 0.0, 200.0);
    rd.number(tm, "missSinkPx", t.missSinkPx, 0.0, 100.0);

    ArrowLaneColors& c = l.colors;
    const JsonValue co = root["colors"];
    rd.color(co, "plate", c.plate);
    rd.color(co, "arrowFill", c.arrowFill);
    rd.color(co, "arrowEdge", c.arrowEdge);
    rd.color(co, "targetIdle", c.targetIdle);
    rd.color(co, "perfect", c.perfect);
    rd.color(co, "great", c.great);
    rd.color(co, "good", c.good);
    rd.color(co, "miss", c.miss);
    rd.color(co, "warn", c.warn);
    const JsonValue rim = co["rim"];
    if (!rim.isAbsent()) {
        if (!rim.isArray() || rim.size() != 4) rd.fail("rim", "not four colours");
        // rim is an array of four colour arrays: read each through a scratch object-free path.
        for (std::uint32_t i = 0; rd.ok && i < 4; ++i) {
            const JsonValue m = rim[i];
            if (!m.isArray() || (m.size() != 3 && m.size() != 4)) {
                rd.fail("rim", "not [r, g, b] or [r, g, b, a]");
                break;
            }
            float v[4] = {0.f, 0.f, 0.f, 1.f};
            for (std::uint32_t k = 0; k < m.size(); ++k) {
                if (!m[k].isNumber() || m[k].number() < 0.0 || m[k].number() > 1.0) {
                    rd.fail("rim", "component out of 0..1");
                    break;
                }
                v[k] = static_cast<float>(m[k].number());
            }
            c.rim[i] = {v[0], v[1], v[2], v[3]};
        }
    }
    if (!rd.ok) return false;

    // Cross-checks.
    auto bad = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (!(l.targetY < l.spawnY)) return bad("geometry: spawnY must be below targetY");
    if (l.glyphWidth > l.columnPitch) return bad("geometry: glyphWidth wider than columnPitch");
    if (l.columnInset + 4.f * l.columnPitch > l.plate.w) return bad("geometry: four columns do not fit the plate");
    if (t.lookaheadMin > t.lookaheadMax) return bad("timing: lookaheadMin above lookaheadMax");
    if (t.exitStart + t.exitFade > t.exitEnd + 1e-9) return bad("timing: the exit fade does not end by exitEnd");
    if (t.lookahead < t.lookaheadMin || t.lookahead > t.lookaheadMax) return bad("timing: lookahead outside its clamp");
    out = l;
    return true;
}

double laneLookahead(const ArrowLaneTimes& t, double bpm) {
    if (!(bpm > 0.0)) return t.lookahead;
    return std::clamp(t.leadBeats * 60.0 / bpm, t.lookaheadMin, t.lookaheadMax);
}

float LaneGeometry::noteAlpha(const ArrowLaneTimes& t, double dt) const {
    if (t.noteFadeIn <= 0.0) return 1.f;
    return clamp01((lookahead - dt) / t.noteFadeIn);
}

LaneGeometry laneGeometry(const ArrowLaneLayout& layout, float width, float height, double lookahead) {
    LaneGeometry g;
    g.box = safeBox(width, height);
    g.plate = resolve(layout.plate, g.box);
    g.columnX0 = g.plate.x + layout.columnInset * 0.01f * g.box.w;
    g.pitch = layout.columnPitch * 0.01f * g.box.w;
    g.glyphH = std::max(layout.glyphHeight * 0.01f * g.box.h, layout.minGlyphPx);
    g.glyphW = std::min(std::max(layout.glyphWidth * 0.01f * g.box.w, layout.minGlyphPx), g.pitch);
    g.targetY = g.box.y + layout.targetY * 0.01f * g.box.h;
    g.spawnY = g.box.y + layout.spawnY * 0.01f * g.box.h;
    g.lookahead = lookahead > 0.0 ? lookahead : layout.times.lookahead;
    g.pxPerSec = (g.spawnY - g.targetY) / static_cast<float>(g.lookahead);
    return g;
}

float laneVisibility(const ArrowLaneTimes& t, double age, double endAge, bool quick) {
    const float in = t.laneFadeIn > 0.0 ? clamp01(age / t.laneFadeIn) : (age >= 0.0 ? 1.f : 0.f);
    if (endAge < 0.0) return in;
    if (quick) return in * clamp01(1.0 - endAge / t.quickFade);
    const double span = std::min(t.exitFade, t.exitEnd);
    const double from = std::min(t.exitStart, t.exitEnd - span);
    return in * clamp01(1.0 - (endAge - from) / span);
}

float plateDimming(const ArrowLaneTimes& t, double endAge) { return endAge >= t.stampDelay ? t.plateDim : 1.f; }

float stampAlpha(const ArrowLaneTimes& t, double endAge) {
    if (endAge < t.stampDelay) return 0.f;
    const double span = std::min(t.exitFade, t.exitEnd);
    const double from = std::min(t.exitStart, t.exitEnd - span);
    return clamp01(1.0 - (endAge - from) / span);
}

float stampScale(const ArrowLaneTimes& t, double endAge) {
    const float k = clamp01((endAge - t.stampDelay) / t.stampPop);
    return t.stampFromScale + (1.f - t.stampFromScale) * k;
}

float streakPopScale(const ArrowLaneTimes& t, double age) {
    if (age < 0.0 || age >= t.streakPop) return 1.f;
    const float k = 1.f - static_cast<float>(age / t.streakPop);
    return 1.f + (t.streakPopScale - 1.f) * k;
}

bool streakStep(const ArrowLaneTimes& t, int n) { return n >= t.streakFrom && t.streakEvery > 0 && n % t.streakEvery == 0; }

double burstLength(const ArrowLaneTimes& t, Grade g) {
    switch (g) {
    case Grade::Perfect: return t.burstPerfect;
    case Grade::Great: return t.burstGreat;
    case Grade::Good: return t.burstGood;
    default: return 0.0;
    }
}

float burstRadius(const ArrowLaneTimes& t, Grade g, double age) {
    const double len = burstLength(t, g);
    if (len <= 0.0 || age < 0.0 || age > len) return 0.f;
    const float to = g == Grade::Perfect ? t.burstToPerfect : g == Grade::Great ? t.burstToGreat : t.burstToGood;
    return t.burstFrom + (to - t.burstFrom) * clamp01(age / len);
}

bool strayCueShows(const ArrowLaneTimes& t, double sinceConfirm) { return sinceConfirm >= t.strayQuiet; }

int pressTick(Grade g, bool pressed, double error) {
    if (g == Grade::Perfect || g == Grade::Pending) return 0;
    if (!pressed) return 1;
    return error < 0.0 ? -1 : error > 0.0 ? 1 : 0;
}

float tickOffsetPx(const LaneGeometry& g, double error, double good) {
    const double e = std::clamp(error, -good, good);
    return -static_cast<float>(e) * g.pxPerSec;
}

Grade worseGrade(Grade a, Grade b) {
    auto rank = [](Grade g) {
        switch (g) {
        case Grade::Miss: return 0;
        case Grade::Good: return 1;
        case Grade::Great: return 2;
        case Grade::Perfect: return 3;
        default: return 4; // Pending counts as best: it never wins over a real grade
        }
    };
    return rank(a) <= rank(b) ? a : b;
}

} // namespace ftcore
