#include "ftcore/UiStyle.h"

#include "ftcore/Json.h"
#include "ftcore/Settings.h" // the HUD safe margin's range

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ftcore {

namespace {

constexpr std::array<std::string_view, kUiWorldCount> kWorldNames{"solhaven", "lattice", "unwritten"};
constexpr std::array<std::string_view, kUiPaletteCount> kPaletteNames{"default", "protan", "deutan", "tritan", "high_contrast"};
constexpr std::array<std::string_view, kUiTokenCount> kTokenNames{"ink", "ink_dim", "backing", "track", "accent",
    "accent2", "danger", "ally", "weak_point", "fill"};
constexpr std::array<std::string_view, kUiTextRoleCount> kRoleNames{"title", "heading", "label", "value", "subtitle",
    "speaker", "prompt", "popup", "verb"};
constexpr std::array<std::string_view, kUiFontCount> kFontNames{"sans",  "sans_bold", "serif",        "hand",
                                                                 "serif_bold", "serif_sc", "serif_sc_bold"};
constexpr std::array<std::string_view, 7> kShapeNames{"none", "diamond", "chevron", "circle", "square", "triangle", "ring"};
constexpr std::array<std::string_view, kUiMeterShapeCount> kMeterNames{"line", "ring", "teeth", "stroke"};

template <std::size_t N>
bool findName(const std::array<std::string_view, N>& names, std::string_view key, std::size_t& out) {
    for (std::size_t i = 0; i < N; ++i) {
        if (names[i] == key) {
            out = i;
            return true;
        }
    }
    return false;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "#RRGGBB" or "#RRGGBBAA".
bool parseHex(std::string_view s, UiColor& out) {
    if ((s.size() != 7 && s.size() != 9) || s[0] != '#') return false;
    float v[4] = {0.f, 0.f, 0.f, 1.f};
    for (std::size_t i = 0; i * 2 + 1 < s.size(); ++i) {
        const int hi = hexDigit(s[1 + i * 2]);
        const int lo = hexDigit(s[2 + i * 2]);
        if (hi < 0 || lo < 0) return false;
        v[i] = static_cast<float>(hi * 16 + lo) / 255.f;
    }
    out = UiColor{v[0], v[1], v[2], v[3]};
    return true;
}

constexpr UiColor hex(std::uint32_t rgb, float a = 1.f) {
    return UiColor{static_cast<float>((rgb >> 16) & 0xFF) / 255.f, static_cast<float>((rgb >> 8) & 0xFF) / 255.f,
        static_cast<float>(rgb & 0xFF) / 255.f, a};
}

float linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

float luminance(UiColor c) { return 0.2126f * linear(c.r) + 0.7152f * linear(c.g) + 0.0722f * linear(c.b); }

UiColor over(UiColor top, UiColor under) {
    const float a = std::clamp(top.a, 0.f, 1.f);
    return UiColor{top.r * a + under.r * (1.f - a), top.g * a + under.g * (1.f - a), top.b * a + under.b * (1.f - a), 1.f};
}

bool fail(std::string* error, std::string_view what) {
    if (error) error->assign(what);
    return false;
}

void setPlate(std::array<char, 24>& plate, std::string_view id) {
    plate.fill('\0');
    std::memcpy(plate.data(), id.data(), std::min(id.size(), plate.size() - 1));
}

} // namespace

std::string_view uiWorldName(UiWorld w) { return kWorldNames[static_cast<std::size_t>(w)]; }
std::string_view uiPaletteName(UiPalette p) { return kPaletteNames[static_cast<std::size_t>(p)]; }
std::string_view uiTokenName(UiToken t) { return kTokenNames[static_cast<std::size_t>(t)]; }
std::string_view uiTextRoleName(UiTextRole r) { return kRoleNames[static_cast<std::size_t>(r)]; }
std::string_view uiMeterShapeName(UiMeterShape m) { return kMeterNames[static_cast<std::size_t>(m) % kUiMeterShapeCount]; }

float uiEdgePx(const UiStyle& s, float height) { return std::max(1.f, s.edgePx * std::max(height, 1.f) / 1080.f); }

UiMargins uiHudMargins(float width, float height, float basePx, const UiMargins& padding, int safePercent) {
    const auto pos = [](float v) { return v > 0.f ? v : 0.f; }; // NaN fails the test too
    const float pct = static_cast<float>(std::clamp(safePercent, Settings::kMinHudSafeMargin, Settings::kMaxHudSafeMargin)) / 100.f;
    const float base = pos(basePx);
    const float w = pos(width) * pct;
    const float h = pos(height) * pct;
    UiMargins m;
    m.left = base + pos(padding.left) + w;
    m.right = base + pos(padding.right) + w;
    m.top = base + pos(padding.top) + h;
    m.bottom = base + pos(padding.bottom) + h;
    return m;
}

void uiFadeStep(UiFade& f, bool changed, float dt, const UiFadeTimes& t) {
    const float step = std::max(dt, 0.f);
    if (changed) f.sinceChange = 0.f;
    else f.sinceChange += step;
    const float rest = std::clamp(t.restAlpha, 0.f, 1.f);
    if (f.sinceChange <= t.rest) {
        // Showing: rise to full.
        f.alpha = t.in > 0.f ? std::min(1.f, f.alpha + step / t.in) : 1.f;
        if (changed && t.in <= 0.f) f.alpha = 1.f;
    } else if (f.alpha > rest) {
        // Resting: fall to the rest level.
        f.alpha = t.out > 0.f ? std::max(rest, f.alpha - step * (1.f - rest) / t.out) : rest;
    } else {
        f.alpha = std::min(f.alpha + (t.in > 0.f ? step / t.in : 1.f), rest);
    }
}

UiStyle defaultUiStyle() {
    UiStyle s;
    // ink, ink_dim, backing, track, accent, accent2, danger, ally, weak_point, fill. The fills are the worlds' own
    // (2026-11-16): Solhaven parchment, Lattice cyan, Unwritten grey; the track's hairline is white at 0x73.
    s.world[0] = {hex(0xF3EEE6), hex(0xD2CBBF), hex(0x0A0806), hex(0xFFFFFF, 0x73 / 255.f), hex(0xE9B36B), hex(0xC9643C),
        hex(0xE60012), hex(0x8FD3B4), hex(0xFFE08A), hex(0xF2DDB4)};
    s.world[1] = {hex(0xEEF4F6), hex(0xC4D1D6), hex(0x05090C), hex(0xFFFFFF, 0x73 / 255.f), hex(0x8FD6E8), hex(0xA88BE0),
        hex(0xE60012), hex(0x9BE3B0), hex(0xFFE08A), hex(0x8FD6E8)};
    // One danger red in every world (E60012, luminance about 0.17: 3:1 or more against the track's black body and the
    // white fill): red means harm to the player, and only the hit cue, the lost hp, the low underline and the call
    // triangle wear it. The weak point is Mark (pale gold) in every world.
    // The Unwritten: a cold grey ink, its dim scaled from it as the others are (luminance x 0.6975), its accent the
    // ink itself (the prompt glyph's ring is ink there), and cold neutral backing and ally.
    s.world[2] = {hex(0xDCE1E5), hex(0xBBC0C3), hex(0x080A0C), hex(0xFFFFFF, 0x73 / 255.f), hex(0xDCE1E5), hex(0xB01E23),
        hex(0xE60012), hex(0xA3ADB5), hex(0xFFE08A), hex(0xBBC0C3)};
    // The danger red is a mid-tone mark that fails the text rule, so in every world a danger word is ink over a danger
    // underline.
    for (UiWorldRules& r : s.rules) r.dangerUnderline = true;
    // Colour-vision sets: danger, ally and weak point moved to hues each type tells apart (orange / sky / yellow for
    // red-green; vermilion / teal / pink for blue-yellow). High contrast: white text on solid black, saturated cues.
    // Each danger sits at luminance about 0.18 at its own hue (full saturation), so it holds 3:1 against the track
    // body's colour and the ink as the default red does.
    const auto cue = [&](UiPalette p, UiToken t, UiColor c) {
        s.paletteColor[static_cast<std::size_t>(p)][static_cast<std::size_t>(t)] = c;
        s.paletteOverrides[static_cast<std::size_t>(p)][static_cast<std::size_t>(t)] = true;
    };
    for (UiPalette p : {UiPalette::Protan, UiPalette::Deutan}) {
        cue(p, UiToken::Danger, hex(0xAC6400));
        cue(p, UiToken::Ally, hex(0x7CC4F0));
        cue(p, UiToken::WeakPoint, hex(0xF5EE70));
    }
    cue(UiPalette::Tritan, UiToken::Danger, hex(0xDA3A00));
    cue(UiPalette::Tritan, UiToken::Ally, hex(0x40C8B0));
    cue(UiPalette::Tritan, UiToken::WeakPoint, hex(0xF5A0C8));
    cue(UiPalette::HighContrast, UiToken::Ink, hex(0xFFFFFF));
    cue(UiPalette::HighContrast, UiToken::InkDim, hex(0xFFFFFF));
    cue(UiPalette::HighContrast, UiToken::Backing, hex(0x000000));
    cue(UiPalette::HighContrast, UiToken::Track, hex(0xFFFFFF, 0.6f));
    cue(UiPalette::HighContrast, UiToken::Fill, hex(0xFFFFFF));
    cue(UiPalette::HighContrast, UiToken::Danger, hex(0xB46000));
    cue(UiPalette::HighContrast, UiToken::Ally, hex(0x00E0FF));
    cue(UiPalette::HighContrast, UiToken::WeakPoint, hex(0xFFFF00));

    s.text[static_cast<std::size_t>(UiTextRole::Title)] = {UiFont::SerifScBold, 40.f, 0.08f, false};
    s.text[static_cast<std::size_t>(UiTextRole::Heading)] = {UiFont::SerifScBold, 28.f, 0.06f, false};
    s.text[static_cast<std::size_t>(UiTextRole::Label)] = {UiFont::SerifSc, 22.f, 0.08f, false};
    s.text[static_cast<std::size_t>(UiTextRole::Value)] = {UiFont::SerifScBold, 28.f, 0.f, false};
    s.text[static_cast<std::size_t>(UiTextRole::Subtitle)] = {UiFont::Serif, 36.f, 0.f, false};
    s.text[static_cast<std::size_t>(UiTextRole::Speaker)] = {UiFont::SerifScBold, 26.f, 0.08f, false};
    s.text[static_cast<std::size_t>(UiTextRole::Prompt)] = {UiFont::Serif, 22.f, 0.02f, false};
    s.text[static_cast<std::size_t>(UiTextRole::Popup)] = {UiFont::SerifScBold, 30.f, 0.04f, false};
    s.text[static_cast<std::size_t>(UiTextRole::Verb)] = {UiFont::SerifScBold, 29.f, 0.22f, true};

    s.cueShape[static_cast<std::size_t>(UiToken::Danger)] = UiCueShape::Triangle;
    s.cueShape[static_cast<std::size_t>(UiToken::Ally)] = UiCueShape::Circle;
    s.cueShape[static_cast<std::size_t>(UiToken::WeakPoint)] = UiCueShape::Diamond;
    return s;
}

UiColor uiColor(const UiStyle& s, UiWorld world, UiPalette palette, UiToken token) {
    const std::size_t p = static_cast<std::size_t>(palette);
    const std::size_t t = static_cast<std::size_t>(token);
    if (p < kUiPaletteCount && t < kUiTokenCount && s.paletteOverrides[p][t]) return s.paletteColor[p][t];
    return s.world[static_cast<std::size_t>(world) % kUiWorldCount][t % kUiTokenCount];
}

UiColor uiMarkColor(const UiStyle& s, UiWorld world, UiPalette palette, UiColor usual) {
    if (!s.rules[static_cast<std::size_t>(world) % kUiWorldCount].marksAccent2) return usual;
    return uiColor(s, world, palette, UiToken::Accent2);
}

UiTrackSet uiTrackSet(const UiStyle& s, UiWorld world, UiPalette palette) {
    UiTrackSet t;
    t.body = uiColor(s, world, palette, UiToken::Backing);
    t.body.a = palette == UiPalette::HighContrast ? 1.f : kUiTrackBodyA;
    t.hairline = uiColor(s, world, palette, UiToken::Track);
    t.fill = uiColor(s, world, palette, UiToken::Fill);
    t.fill.a = 1.f;
    return t;
}

float uiDangerFillContrast(const UiStyle& s, UiWorld world, UiPalette palette) {
    const UiColor black{0.f, 0.f, 0.f, 1.f};
    return uiContrast(uiColor(s, world, palette, UiToken::Danger), uiTrackSet(s, world, palette).fill, black);
}

UiDangerWord uiDangerWord(const UiStyle& s, UiWorld world, UiPalette palette) {
    UiDangerWord d;
    d.underline = uiColor(s, world, palette, UiToken::Danger);
    d.underlined = s.rules[static_cast<std::size_t>(world) % kUiWorldCount].dangerUnderline;
    d.word = d.underlined ? uiColor(s, world, palette, UiToken::Ink) : d.underline;
    return d;
}

UiDangerSet uiDangerSet(const UiStyle& s, UiWorld world, UiPalette palette) {
    const UiColor danger = uiColor(s, world, palette, UiToken::Danger);
    UiDangerSet d;
    d.lowUnderlined = s.rules[static_cast<std::size_t>(world) % kUiWorldCount].dangerUnderline;
    // One hit cue in every world: the Unwritten's danger red in the palette shown.
    d.hitCue = uiColor(s, UiWorld::Unwritten, palette, UiToken::Danger);
    d.lowLine = d.lowUnderlined ? uiTrackSet(s, world, palette).fill : danger;
    d.lowUnderline = danger;
    d.cueMark = uiMarkColor(s, world, palette, danger);
    d.hitDark = UiColor{d.hitCue.r * kUiHitDarkK, d.hitCue.g * kUiHitDarkK, d.hitCue.b * kUiHitDarkK, d.hitCue.a};
    return d;
}

float uiHitFlashLevel(float age) {
    if (!(age >= 0.f) || age >= kUiHitGone) return 0.f;
    if (age <= kUiHitFull) return 1.f;
    const float u = (age - kUiHitFull) / (kUiHitGone - kUiHitFull);
    return 1.f - u * u * (3.f - 2.f * u);
}

float uiClockStep(double& last, double now) {
    if (!(now == now)) return 0.f; // NaN: no time passes, the last time stays
    const double was = last;
    last = now;
    if (was < 0.0 || !(now > was)) return 0.f;
    return static_cast<float>(std::min(now - was, 1e3));
}

void uiHitSegsStep(UiHitSegs& s, float hp01, double now) {
    const float dt = uiClockStep(s.clock, now);
    for (UiHitSeg& g : s.seg) {
        if (g.age < kUiHitGone) g.age = std::min(kUiHitGone, g.age + dt);
    }
    if (!(hp01 == hp01)) return; // NaN: no segment, `last` kept
    const float hp = std::clamp(hp01, 0.f, 1.f);
    if (s.last < 0.f || hp > s.last + 1e-4f) {
        for (UiHitSeg& g : s.seg) g.age = kUiHitGone;
    } else if (hp < s.last - 1e-4f) {
        s.seg[s.next] = UiHitSeg{hp, s.last, 0.f};
        s.next = static_cast<std::uint8_t>((s.next + 1u) % kUiHitSegs);
    }
    s.last = hp;
}

UiHitLayers uiHitLayers(float in, float edge) {
    UiHitLayers l;
    if (!(edge > 0.f)) return l;
    const float u = std::clamp(in, 0.f, 1.f);
    const float f = 1.f - u * u * (3.f - 2.f * u);
    if (f <= 0.f) return l;
    const float k = std::min(edge, 1.f) * std::pow(f, kUiHitGlowPow); // one curve, one share, both layers
    l.dark = kUiHitDarkPeak * k;
    l.red = kUiHitGlowPeak * k;
    return l;
}

UiHitEdges uiHitEdges(bool hasSide, float rightX, float aheadY) {
    UiHitEdges e;
    const float ax = std::fabs(rightX), ay = std::fabs(aheadY);
    if (!hasSide || !(ax + ay > 1e-6f)) return e;
    e.left = e.right = e.top = e.bottom = kUiHitOffSide;
    if (ax >= ay) (rightX > 0.f ? e.right : e.left) = 1.f;
    else (aheadY > 0.f ? e.top : e.bottom) = 1.f;
    return e;
}

float uiHitWatchStep(UiHitWatch& w, double stamp, double now) {
    const float dt = uiClockStep(w.clock, now);
    if (!w.seeded) {
        w.seen = stamp;
        w.age = 1e3f;
        w.seeded = true;
    } else if (stamp != w.seen) {
        const bool fresh = stamp > w.seen && stamp >= 0.0;
        w.seen = stamp;
        if (!fresh) w.age = 1e3f;
        else if (w.age >= kUiHitRepeat) w.age = 0.f;
        else w.age = std::min(w.age + dt, 1e3f); // too soon after the last: no new cue
    } else {
        w.age = std::min(w.age + dt, 1e3f);
    }
    return w.age;
}

UiGuardState uiGuardState(float stamina01, bool broken) {
    if (broken) return UiGuardState::Broken;
    if (!(stamina01 < 0.999f)) return UiGuardState::Full;
    return stamina01 <= 0.001f ? UiGuardState::Spent : UiGuardState::Under;
}

float uiLabelLevel(double age) {
    if (!(age >= 0.0) || age >= static_cast<double>(kUiLabelHold + kUiLabelFade)) return 0.f;
    if (age <= static_cast<double>(kUiLabelHold)) return 1.f;
    return std::clamp(1.f - static_cast<float>(age - kUiLabelHold) / kUiLabelFade, 0.f, 1.f);
}

float uiGuardLabelStep(UiGuardLabel& g, UiGuardState state, double now) {
    if (state != g.state) {
        g.state = state;
        g.since = now;
    }
    if (g.state == UiGuardState::Full) return 0.f;
    return uiLabelLevel(now - g.since);
}

float uiOnceLabelStep(UiOnceLabel& g, bool present, double now) {
    if (!present) return 0.f;
    if (!g.shown) {
        g.shown = true;
        g.since = now;
    }
    return uiLabelLevel(now - g.since);
}

float uiLoadLabelStep(UiLoadLabel& g, int load, double now) { return uiOnceLabelStep(g, load > 0, now); }

int uiBossPercent(int hp, int hpMax) {
    if (hpMax <= 0) return -1;
    if (hp <= 0) return 0;
    if (hp >= hpMax) return 100;
    const long long pc = (100LL * hp) / hpMax;
    return static_cast<int>(std::max<long long>(1, pc));
}

float uiCapX(float x0, float len, float fill01, float capW) {
    const float f = fill01 == fill01 ? std::clamp(fill01, 0.f, 1.f) : 0.f;
    const float l = std::max(len, 0.f);
    const float half = std::min(0.5f * std::max(capW, 0.f), 0.5f * l);
    return std::clamp(x0 + l * f, x0 + half, x0 + l - half);
}

UiReleasePulseView uiReleasePulseStep(UiReleasePulse& p, bool ready, bool overloaded, double now) {
    if (ready && !p.ready) p.readyAt = now;
    if (overloaded && !p.over) p.overAt = now;
    p.ready = ready;
    p.over = overloaded;
    UiReleasePulseView v;
    if (ready) {
        const double a = now - p.readyAt;
        if (a >= 0.0 && a < kUiReleaseTravel) v.travel = static_cast<float>(a / kUiReleaseTravel);
        else if (a >= kUiReleaseTravel && a < kUiReleaseTravel + kUiReleasePeak) v.peak = kUiReleaseGrow;
    }
    if (overloaded) {
        const double a = now - p.overAt;
        if (a >= 0.0 && std::fmod(a, static_cast<double>(kUiReleaseRepeat)) < kUiReleasePeak) v.peak = kUiReleaseGrow;
    }
    return v;
}

UiRingPoint uiRingPoint(float t, float chord, float sag, float offset) {
    UiRingPoint q;
    const float c = std::max(chord, 0.f);
    const float u = t == t ? std::clamp(t, 0.f, 1.f) : 0.f;
    if (!(sag > 1e-4f) || c <= 0.f) {
        q.x = c * u;
        q.y = -offset;
        return q;
    }
    // The circle through the chord's ends whose middle stands `sag` off the chord: radius r, centre (c/2, sag - r) with y
    // toward the bulge. Outward along the radius is away from the centre, so a positive offset moves toward the bulge.
    const double cs = c, ss = sag;
    const double r = (cs * cs / 4.0 + ss * ss) / (2.0 * ss);
    const double half = std::asin(std::min(1.0, cs / (2.0 * r)));
    const double th = (2.0 * u - 1.0) * half;
    const double rr = r + offset;
    q.x = static_cast<float>(cs / 2.0 + rr * std::sin(th));
    q.y = static_cast<float>(ss - r + rr * std::cos(th));
    q.tx = static_cast<float>(std::cos(th));
    q.ty = static_cast<float>(-std::sin(th));
    return q;
}

float uiArcSag(float chord, float radius) {
    const float half = 0.5f * std::max(chord, 0.f);
    if (!(radius > half)) return 0.f;
    return radius - std::sqrt(radius * radius - half * half);
}

int uiArcSegments(float chord, float maxChordPx) {
    const float m = maxChordPx > 0.f ? maxChordPx : 4.f;
    const float n = std::ceil(std::max(chord, 0.f) / m);
    return std::clamp(static_cast<int>(n), 1, 1024);
}

UiLowGuardGaps uiLowGuardGaps(float s, float px) {
    const float d = px > 0.f ? px : 1.f;
    const auto whole = [d](float v) { return std::max(1.f, std::floor(v / d + 0.5f)) * d; };
    UiLowGuardGaps g;
    g.gap = std::max(kUiGapMinPx * d, whole(kUiLowUnderGap * s));
    g.under = whole(kUiLowUnderW * s);
    g.clear = std::max(kUiGapMinPx * d, whole(kUiGuardClear * s));
    return g;
}

float uiTextPx(const UiStyle& s, UiTextRole role, float height, float textScale) {
    const float h = std::max(height, 1.f);
    const float scale = std::clamp(textScale, 1.f, 2.f);
    const float k = h / 1080.f;
    const float base = s.text[static_cast<std::size_t>(role) % kUiTextRoleCount].px;
    const float px = std::max(base, s.minTextPx) * k * scale;
    return std::max(px, s.minTextPxAbsolute);
}

float uiLinePx(const UiStyle& s, float height) { return std::max(1.f, s.lineWeight * std::max(height, 1.f) / 1080.f); }

UiColor uiLumaFloor(UiColor c, float floor) {
    const float f = std::clamp(floor, 0.f, 1.f);
    if (luminance(c) >= f) return c;
    // Luminance rises monotonically with the mix toward white: bisect the smallest mix that reaches the floor.
    float lo = 0.f, hi = 1.f;
    for (int i = 0; i < 20; ++i) {
        const float m = 0.5f * (lo + hi);
        const UiColor t{c.r + (1.f - c.r) * m, c.g + (1.f - c.g) * m, c.b + (1.f - c.b) * m, c.a};
        (luminance(t) >= f ? hi : lo) = m;
    }
    return UiColor{c.r + (1.f - c.r) * hi, c.g + (1.f - c.g) * hi, c.b + (1.f - c.b) * hi, c.a};
}

float uiOptionDim(const UiStyle& s, UiColor ink, UiColor backing, UiColor picture) {
    const UiColor pic{picture.r, picture.g, picture.b, 1.f};
    const auto at = [&](float a) { return uiContrast(ink, UiColor{backing.r, backing.g, backing.b, a}, pic); };
    const float top = std::clamp(s.optionsDimMax, 0.f, 1.f);
    if (at(0.f) >= s.optionsDimContrast) return 0.f;
    if (at(top) < s.optionsDimContrast) return top;
    float lo = 0.f, hi = top; // contrast rises with the dim: bisect the least that reaches the target
    for (int i = 0; i < 20; ++i) {
        const float m = 0.5f * (lo + hi);
        (at(m) >= s.optionsDimContrast ? hi : lo) = m;
    }
    return hi;
}

void uiLumaAdd(UiLumaHistogram& h, UiColor c) {
    const float l = std::clamp(luminance(c), 0.f, 1.f);
    const std::size_t i = std::min(UiLumaHistogram::kBins - 1, static_cast<std::size_t>(l * UiLumaHistogram::kBins));
    ++h.n[i];
    h.r[i] += c.r;
    h.g[i] += c.g;
    h.b[i] += c.b;
    ++h.total;
}

UiColor uiLumaBrightest(const UiLumaHistogram& h, float share) {
    if (h.total == 0) return UiColor{0.f, 0.f, 0.f, 1.f};
    const std::uint32_t want = std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(std::clamp(share, 0.f, 1.f) * h.total));
    std::uint32_t got = 0;
    float r = 0.f, g = 0.f, b = 0.f;
    for (std::size_t i = UiLumaHistogram::kBins; i-- > 0 && got < want;) {
        got += h.n[i];
        r += h.r[i];
        g += h.g[i];
        b += h.b[i];
    }
    return UiColor{r / got, g / got, b / got, 1.f};
}

float uiContrast(UiColor fg, UiColor bg, UiColor under) {
    const UiColor b = over(bg, under);
    const UiColor f = over(fg, b);
    const float l1 = luminance(f);
    const float l2 = luminance(b);
    return (std::max(l1, l2) + 0.05f) / (std::min(l1, l2) + 0.05f);
}

UiCueShape uiCueShape(const UiStyle& s, UiToken token) { return s.cueShape[static_cast<std::size_t>(token) % kUiTokenCount]; }

bool validateUiStyle(const UiStyle& s, std::string* error) {
    if (s.minTextPx < 18.f) return fail(error, "min_text_px is below 18");
    if (s.minTextPxAbsolute < 14.f) return fail(error, "min_text_px_absolute is below 14");
    if (s.backingDefault < 0.f || s.backingDefault > 1.f) return fail(error, "backing_default is outside 0..1");
    if (s.edgePx < 1.f) return fail(error, "edge px is below 1");
    if (s.edgeText < 0.5f || s.edgeText > 1.f) return fail(error, "edge text is outside 0.5..1");
    if (s.backingSoftHud < 0.f || s.backingSoftHud > 1.f || s.backingSoftDialogue < 0.f || s.backingSoftDialogue > 1.f)
        return fail(error, "backing_soft is outside 0..1");
    if (s.edge.a < 0.5f) return fail(error, "the edge is fainter than 50%");
    // The backing at its default strength over the worst picture behind it (white for light text).
    const UiColor white{1.f, 1.f, 1.f, 1.f};
    char buf[128];
    for (std::size_t w = 0; w < kUiWorldCount; ++w) {
        for (std::size_t p = 0; p < kUiPaletteCount; ++p) {
            const UiWorld world = static_cast<UiWorld>(w);
            const UiPalette palette = static_cast<UiPalette>(p);
            UiColor back = uiColor(s, world, palette, UiToken::Backing);
            back.a = palette == UiPalette::HighContrast ? 1.f : s.backingDefault;
            struct Need {
                UiToken token;
                float ratio;
            };
            // Text tokens 4.5:1; cue colours (drawn as shapes and fills, with a word beside them) 3:1. Danger is a
            // mid-tone mark that is never text (danger_underline in every world): it is measured below where it is drawn.
            constexpr Need kNeeds[] = {{UiToken::Ink, 4.5f}, {UiToken::InkDim, 4.5f}, {UiToken::Accent, 4.5f},
                {UiToken::Ally, 3.f}, {UiToken::WeakPoint, 3.f}, {UiToken::Fill, 3.f}};
            for (const Need& n : kNeeds) {
                const float c = uiContrast(uiColor(s, world, palette, n.token), back, white);
                if (c < n.ratio) {
                    std::snprintf(buf, sizeof buf, "%.*s/%.*s: %.*s on the backing is %.2f:1, needs %.1f:1",
                        static_cast<int>(kWorldNames[w].size()), kWorldNames[w].data(),
                        static_cast<int>(kPaletteNames[p].size()), kPaletteNames[p].data(),
                        static_cast<int>(kTokenNames[static_cast<std::size_t>(n.token)].size()),
                        kTokenNames[static_cast<std::size_t>(n.token)].data(), static_cast<double>(c),
                        static_cast<double>(n.ratio));
                    return fail(error, buf);
                }
            }
            // Danger 3:1 against the value track body's own colour (the lost hp is drawn over it) and the ink (the
            // light line a danger underline sits under where the fill steps to ink). Against the world fills it is
            // reported only (uiDangerFillContrast).
            {
                UiColor body = uiTrackSet(s, world, palette).body;
                body.a = 1.f;
                const UiColor dg = uiColor(s, world, palette, UiToken::Danger);
                const float c = std::min(uiContrast(dg, body, white), uiContrast(dg, uiColor(s, world, palette, UiToken::Ink), white));
                if (c < 3.f) {
                    std::snprintf(buf, sizeof buf, "%.*s/%.*s: danger on the track is %.2f:1, needs 3.0:1",
                        static_cast<int>(kWorldNames[w].size()), kWorldNames[w].data(),
                        static_cast<int>(kPaletteNames[p].size()), kPaletteNames[p].data(), static_cast<double>(c));
                    return fail(error, buf);
                }
            }
            // The default look: text tokens on their own hairline edge, over the worst picture.
            for (UiToken t : {UiToken::Ink, UiToken::InkDim, UiToken::Accent}) {
                const float c = uiContrast(uiColor(s, world, palette, t), s.edge, white);
                if (c < 4.5f) {
                    std::snprintf(buf, sizeof buf, "%.*s/%.*s: %.*s on the edge is %.2f:1, needs 4.5:1",
                        static_cast<int>(kWorldNames[w].size()), kWorldNames[w].data(),
                        static_cast<int>(kPaletteNames[p].size()), kPaletteNames[p].data(),
                        static_cast<int>(kTokenNames[static_cast<std::size_t>(t)].size()),
                        kTokenNames[static_cast<std::size_t>(t)].data(), static_cast<double>(c));
                    return fail(error, buf);
                }
            }
        }
    }
    // Each meaning-carrying cue has its own shape.
    const UiCueShape d = uiCueShape(s, UiToken::Danger);
    const UiCueShape a = uiCueShape(s, UiToken::Ally);
    const UiCueShape k = uiCueShape(s, UiToken::WeakPoint);
    if (d == UiCueShape::None || a == UiCueShape::None || k == UiCueShape::None)
        return fail(error, "danger, ally and weak_point each need a cue shape");
    if (d == a || d == k || a == k) return fail(error, "danger, ally and weak_point share a cue shape");
    return true;
}

bool importUiStyle(std::string_view text, UiStyle& out, std::string* error) {
    JsonDocument doc;
    if (!doc.parse(text, error)) return false;
    const JsonValue root = doc.root();
    if (root.type() != JsonType::Object) return fail(error, "style: the top level is not an object");
    UiStyle s = defaultUiStyle();
    std::string what;
    const auto bad = [&](std::string_view section, std::string_view key) {
        what.assign(section);
        what += ": ";
        what += key;
        return fail(error, what);
    };
    const auto number = [&](JsonValue v, float lo, float hi, float& dst) {
        if (v.type() != JsonType::Number) return false;
        const float f = static_cast<float>(v.number());
        if (!(f >= lo && f <= hi)) return false;
        dst = f;
        return true;
    };
    const auto colours = [&](JsonValue obj, std::string_view section, std::array<UiColor, kUiTokenCount>& dst,
                             std::array<bool, kUiTokenCount>* set, bool allowPlate, std::array<char, 24>* plate,
                             UiWorldRules* rules) {
        if (obj.type() != JsonType::Object) return bad(section, "not an object");
        for (std::uint32_t i = 0; i < obj.size(); ++i) {
            const std::string_view key = obj.key(i);
            const JsonValue v = obj.value(i);
            if (allowPlate && key == "plate") {
                if (v.type() != JsonType::String || v.string().size() >= 24) return bad(section, "plate");
                setPlate(*plate, v.string());
                continue;
            }
            bool* rule = !rules ? nullptr
                : key == "marks_accent2" ? &rules->marksAccent2
                : key == "danger_underline" ? &rules->dangerUnderline
                : key == "sig_pulse" ? &rules->sigPulse
                : key == "sig_arcs" ? &rules->sigArcs
                : key == "sig_margin_rule" ? &rules->sigMarginRule : nullptr;
            if (rule) {
                if (v.type() != JsonType::Bool) return bad(section, key);
                *rule = v.boolean();
                continue;
            }
            std::size_t t = 0;
            UiColor c;
            if (!findName(kTokenNames, key, t)) return bad(section, key);
            if (v.type() != JsonType::String || !parseHex(v.string(), c)) return bad(section, key);
            dst[t] = c;
            if (set) (*set)[t] = true;
        }
        return true;
    };

    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view key = root.key(i);
        const JsonValue v = root.value(i);
        if (key == "version" || key == "note") continue;
        if (key == "line_weight") {
            if (!number(v, 1.f, 12.f, s.lineWeight)) return bad("style", key);
        } else if (key == "backing_default") {
            if (!number(v, 0.f, 1.f, s.backingDefault)) return bad("style", key);
        } else if (key == "min_text_px") {
            if (!number(v, 0.f, 64.f, s.minTextPx)) return bad("style", key);
        } else if (key == "min_text_px_absolute") {
            if (!number(v, 0.f, 64.f, s.minTextPxAbsolute)) return bad("style", key);
        } else if (key == "backing_soft") {
            if (v.type() != JsonType::Object) return bad("style", key);
            for (std::uint32_t j = 0; j < v.size(); ++j) {
                const std::string_view bk = v.key(j);
                if (bk == "hud") {
                    if (!number(v.value(j), 0.f, 1.f, s.backingSoftHud)) return bad(key, bk);
                } else if (bk == "dialogue") {
                    if (!number(v.value(j), 0.f, 1.f, s.backingSoftDialogue)) return bad(key, bk);
                } else if (bk == "options_contrast") {
                    if (!number(v.value(j), 1.f, 21.f, s.optionsDimContrast)) return bad(key, bk);
                } else if (bk == "options_max") {
                    if (!number(v.value(j), 0.f, 1.f, s.optionsDimMax)) return bad(key, bk);
                } else {
                    return bad(key, bk);
                }
            }
        } else if (key == "edge") {
            if (v.type() != JsonType::Object) return bad("style", key);
            for (std::uint32_t j = 0; j < v.size(); ++j) {
                const std::string_view ek = v.key(j);
                const JsonValue ev = v.value(j);
                if (ek == "color") {
                    if (ev.type() != JsonType::String || !parseHex(ev.string(), s.edge)) return bad(key, ek);
                } else if (ek == "px") {
                    if (!number(ev, 0.f, 8.f, s.edgePx)) return bad(key, ek);
                } else if (ek == "text") {
                    if (!number(ev, 0.f, 1.f, s.edgeText)) return bad(key, ek);
                } else {
                    return bad(key, ek);
                }
            }
        } else if (key == "meter") {
            std::size_t m = 0;
            if (v.type() != JsonType::String || !findName(kMeterNames, v.string(), m)) return bad("style", key);
            s.meter = static_cast<UiMeterShape>(m);
        } else if (key == "load") {
            if (v.type() != JsonType::Object) return bad("style", key);
            for (std::uint32_t j = 0; j < v.size(); ++j) {
                const std::string_view lk = v.key(j);
                UiColor* dst = lk == "gold" ? &s.loadGold : lk == "bright" ? &s.loadBright : nullptr;
                const JsonValue lv = v.value(j);
                if (!dst || lv.type() != JsonType::String || !parseHex(lv.string(), *dst)) return bad(key, lk);
            }
        } else if (key == "fade") {
            if (v.type() != JsonType::Object) return bad("style", key);
            for (std::uint32_t j = 0; j < v.size(); ++j) {
                const std::string_view fk = v.key(j);
                const JsonValue fv = v.value(j);
                float* dst = fk == "in" ? &s.fade.in : fk == "rest" ? &s.fade.rest : fk == "out" ? &s.fade.out
                    : fk == "rest_alpha" ? &s.fade.restAlpha : nullptr;
                if (!dst || !number(fv, 0.f, fk == "rest_alpha" ? 1.f : 30.f, *dst)) return bad(key, fk);
            }
        } else if (key == "lines") {
            if (v.type() != JsonType::Object) return bad("style", key);
            for (std::uint32_t j = 0; j < v.size(); ++j) {
                const std::string_view lk = v.key(j);
                float* dst = lk == "cap_grow" ? &s.lines.capGrow : lk == "guard_dot" ? &s.lines.guardDot
                    : lk == "guard_pitch" ? &s.lines.guardPitch : nullptr;
                if (!dst || !number(v.value(j), lk == "cap_grow" ? 0.f : 1.f, 32.f, *dst)) return bad(key, lk);
            }
            if (s.lines.guardPitch <= s.lines.guardDot) return bad(key, "guard_pitch");
        } else if (key == "margin_rule") {
            if (v.type() != JsonType::Object) return bad("style", key);
            for (std::uint32_t j = 0; j < v.size(); ++j) {
                const std::string_view mk = v.key(j);
                float* dst = mk == "gap" ? &s.marginRule.gap : mk == "width" ? &s.marginRule.width
                    : mk == "alpha" ? &s.marginRule.alpha : mk == "tick_w" ? &s.marginRule.tickW
                    : mk == "tick_h" ? &s.marginRule.tickH : nullptr;
                if (!dst || !number(v.value(j), 0.f, mk == "alpha" ? 1.f : 64.f, *dst)) return bad(key, mk);
            }
        } else if (key == "word_luma_floor") {
            if (!number(v, 0.f, 1.f, s.wordLumaFloor)) return bad("style", key);
        } else if (key == "budget") {
            if (v.type() != JsonType::Object) return bad("style", key);
            for (std::uint32_t j = 0; j < v.size(); ++j) {
                const std::string_view bk = v.key(j);
                const JsonValue bv = v.value(j);
                float n = 0.f;
                if (bk == "rest") {
                    if (!number(bv, 0.f, 30.f, s.budget.rest)) return bad(key, bk);
                } else if (bk == "hint") {
                    if (!number(bv, 0.f, 30.f, s.budget.hint)) return bad(key, bk);
                } else if (bk == "marks") {
                    if (!number(bv, 2.f, 40.f, n)) return bad(key, bk);
                    s.budget.marks = static_cast<std::uint8_t>(n);
                } else {
                    return bad(key, bk);
                }
            }
        } else if (key == "layers") {
            if (v.type() != JsonType::Object) return bad("style", key);
            for (std::uint32_t j = 0; j < v.size(); ++j) {
                const std::string_view lk = v.key(j);
                const JsonValue lv = v.value(j);
                if (lv.type() != JsonType::Bool) return bad("layers", lk);
                if (lk == "hairlines") s.layers.hairlines = lv.boolean();
                else if (lk == "plates") s.layers.plates = lv.boolean();
                else if (lk == "world_anchored") s.layers.worldAnchored = lv.boolean();
                else if (lk == "letterbox") s.layers.letterbox = lv.boolean();
                else return bad("layers", lk);
            }
        } else if (key == "worlds") {
            if (v.type() != JsonType::Object) return bad("style", key);
            for (std::uint32_t j = 0; j < v.size(); ++j) {
                std::size_t w = 0;
                if (!findName(kWorldNames, v.key(j), w)) return bad("worlds", v.key(j));
                s.rules[w] = UiWorldRules{};
                if (!colours(v.value(j), v.key(j), s.world[w], nullptr, true, &s.plate[w], &s.rules[w])) return false;
            }
        } else if (key == "palettes") {
            if (v.type() != JsonType::Object) return bad("style", key);
            for (std::uint32_t j = 0; j < v.size(); ++j) {
                std::size_t p = 0;
                if (!findName(kPaletteNames, v.key(j), p) || p == 0) return bad("palettes", v.key(j));
                s.paletteOverrides[p].fill(false);
                if (!colours(v.value(j), v.key(j), s.paletteColor[p], &s.paletteOverrides[p], false, nullptr, nullptr))
                    return false;
            }
        } else if (key == "text") {
            if (v.type() != JsonType::Object) return bad("style", key);
            for (std::uint32_t j = 0; j < v.size(); ++j) {
                std::size_t r = 0;
                if (!findName(kRoleNames, v.key(j), r)) return bad("text", v.key(j));
                const JsonValue t = v.value(j);
                if (t.type() != JsonType::Object) return bad("text", v.key(j));
                UiTextStyle& ts = s.text[r];
                for (std::uint32_t m = 0; m < t.size(); ++m) {
                    const std::string_view tk = t.key(m);
                    const JsonValue tv = t.value(m);
                    std::size_t f = 0;
                    if (tk == "font") {
                        if (tv.type() != JsonType::String || !findName(kFontNames, tv.string(), f)) return bad(v.key(j), tk);
                        ts.font = static_cast<UiFont>(f);
                    } else if (tk == "px") {
                        if (!number(tv, 8.f, 128.f, ts.px)) return bad(v.key(j), tk);
                    } else if (tk == "tracking") {
                        if (!number(tv, 0.f, 0.5f, ts.tracking)) return bad(v.key(j), tk);
                    } else if (tk == "upper") {
                        if (tv.type() != JsonType::Bool) return bad(v.key(j), tk);
                        ts.upper = tv.boolean();
                    } else {
                        return bad(v.key(j), tk);
                    }
                }
            }
        } else if (key == "cues") {
            if (v.type() != JsonType::Object) return bad("style", key);
            for (std::uint32_t j = 0; j < v.size(); ++j) {
                std::size_t t = 0;
                std::size_t shape = 0;
                const JsonValue cv = v.value(j);
                if (!findName(kTokenNames, v.key(j), t)) return bad("cues", v.key(j));
                if (cv.type() != JsonType::String || !findName(kShapeNames, cv.string(), shape)) return bad("cues", v.key(j));
                s.cueShape[t] = static_cast<UiCueShape>(shape);
            }
        } else {
            return bad("style", key);
        }
    }
    if (!validateUiStyle(s, error)) return false;
    out = s;
    return true;
}

} // namespace ftcore
