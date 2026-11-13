#include "ftcore/UiStyle.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ftcore {

namespace {

constexpr std::array<std::string_view, kUiWorldCount> kWorldNames{"solhaven", "lattice", "unwritten"};
constexpr std::array<std::string_view, kUiPaletteCount> kPaletteNames{"default", "protan", "deutan", "tritan", "high_contrast"};
constexpr std::array<std::string_view, kUiTokenCount> kTokenNames{"ink", "ink_dim", "backing", "track", "accent",
    "accent2", "danger", "ally", "weak_point"};
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
    // ink, ink_dim, backing, track, accent, accent2, danger, ally, weak_point
    s.world[0] = {hex(0xF3EEE6), hex(0xD2CBBF), hex(0x0A0806), hex(0xFFFFFF, 0.3f), hex(0xE9B36B), hex(0xC9643C),
        hex(0xFF7A5C), hex(0x8FD3B4), hex(0xFFE08A)};
    s.world[1] = {hex(0xEEF4F6), hex(0xC4D1D6), hex(0x05090C), hex(0xFFFFFF, 0.3f), hex(0x8FD6E8), hex(0xA88BE0),
        hex(0xFF7A86), hex(0x9BE3B0), hex(0xFFE08A)};
    s.world[2] = {hex(0xF2ECE0), hex(0xD0C8B8), hex(0x0B0907), hex(0xFFFFFF, 0.3f), hex(0xE6DDCB), hex(0x9E3B2E),
        hex(0xFF7A66), hex(0x9CC7A4), hex(0xF4D58A)};
    // Colour-vision sets: danger, ally and weak point moved to hues each type tells apart (orange / sky / yellow for
    // red-green; vermilion / teal / pink for blue-yellow). High contrast: white text on solid black, saturated cues.
    const auto cue = [&](UiPalette p, UiToken t, UiColor c) {
        s.paletteColor[static_cast<std::size_t>(p)][static_cast<std::size_t>(t)] = c;
        s.paletteOverrides[static_cast<std::size_t>(p)][static_cast<std::size_t>(t)] = true;
    };
    for (UiPalette p : {UiPalette::Protan, UiPalette::Deutan}) {
        cue(p, UiToken::Danger, hex(0xF0A030));
        cue(p, UiToken::Ally, hex(0x7CC4F0));
        cue(p, UiToken::WeakPoint, hex(0xF5EE70));
    }
    cue(UiPalette::Tritan, UiToken::Danger, hex(0xFF7A4A));
    cue(UiPalette::Tritan, UiToken::Ally, hex(0x40C8B0));
    cue(UiPalette::Tritan, UiToken::WeakPoint, hex(0xF5A0C8));
    cue(UiPalette::HighContrast, UiToken::Ink, hex(0xFFFFFF));
    cue(UiPalette::HighContrast, UiToken::InkDim, hex(0xFFFFFF));
    cue(UiPalette::HighContrast, UiToken::Backing, hex(0x000000));
    cue(UiPalette::HighContrast, UiToken::Track, hex(0xFFFFFF, 0.45f));
    cue(UiPalette::HighContrast, UiToken::Danger, hex(0xFF8A00));
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
    s.text[static_cast<std::size_t>(UiTextRole::Verb)] = {UiFont::SerifScBold, 27.f, 0.22f, true};

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

float uiTextPx(const UiStyle& s, UiTextRole role, float height, float textScale) {
    const float h = std::max(height, 1.f);
    const float scale = std::clamp(textScale, 1.f, 2.f);
    const float k = h / 1080.f;
    const float base = s.text[static_cast<std::size_t>(role) % kUiTextRoleCount].px;
    const float px = std::max(base, s.minTextPx) * k * scale;
    return std::max(px, s.minTextPxAbsolute);
}

float uiLinePx(const UiStyle& s, float height) { return std::max(1.f, s.lineWeight * std::max(height, 1.f) / 1080.f); }

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
            // Text tokens 4.5:1; cue colours (drawn as shapes and fills, with a word beside them) 3:1.
            constexpr Need kNeeds[] = {{UiToken::Ink, 4.5f}, {UiToken::InkDim, 4.5f}, {UiToken::Accent, 4.5f},
                {UiToken::Danger, 3.f}, {UiToken::Ally, 3.f}, {UiToken::WeakPoint, 3.f}};
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
                             std::array<bool, kUiTokenCount>* set, bool allowPlate, std::array<char, 24>* plate) {
        if (obj.type() != JsonType::Object) return bad(section, "not an object");
        for (std::uint32_t i = 0; i < obj.size(); ++i) {
            const std::string_view key = obj.key(i);
            const JsonValue v = obj.value(i);
            if (allowPlate && key == "plate") {
                if (v.type() != JsonType::String || v.string().size() >= 24) return bad(section, "plate");
                setPlate(*plate, v.string());
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
                } else {
                    return bad(key, ek);
                }
            }
        } else if (key == "meter") {
            std::size_t m = 0;
            if (v.type() != JsonType::String || !findName(kMeterNames, v.string(), m)) return bad("style", key);
            s.meter = static_cast<UiMeterShape>(m);
        } else if (key == "fade") {
            if (v.type() != JsonType::Object) return bad("style", key);
            for (std::uint32_t j = 0; j < v.size(); ++j) {
                const std::string_view fk = v.key(j);
                const JsonValue fv = v.value(j);
                float* dst = fk == "in" ? &s.fade.in : fk == "rest" ? &s.fade.rest : fk == "out" ? &s.fade.out
                    : fk == "rest_alpha" ? &s.fade.restAlpha : nullptr;
                if (!dst || !number(fv, 0.f, fk == "rest_alpha" ? 1.f : 30.f, *dst)) return bad(key, fk);
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
                if (!colours(v.value(j), v.key(j), s.world[w], nullptr, true, &s.plate[w])) return false;
            }
        } else if (key == "palettes") {
            if (v.type() != JsonType::Object) return bad("style", key);
            for (std::uint32_t j = 0; j < v.size(); ++j) {
                std::size_t p = 0;
                if (!findName(kPaletteNames, v.key(j), p) || p == 0) return bad("palettes", v.key(j));
                s.paletteOverrides[p].fill(false);
                if (!colours(v.value(j), v.key(j), s.paletteColor[p], &s.paletteOverrides[p], false, nullptr)) return false;
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
