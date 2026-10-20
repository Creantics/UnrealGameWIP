#include "ftcore/StylePack.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace ftcore {

namespace {

float lerpField(float a, float b, float t) { return a + (b - a) * t; }
StyleVec3 lerpField(const StyleVec3& a, const StyleVec3& b, float t) { return a + (b - a) * t; }

// The same arithmetic as glm::mix(x, y, a), so the pack values match newengine bit for bit.
StyleVec3 mix(const StyleVec3& x, const StyleVec3& y, float a) { return x * (1.f - a) + y * a; }

StyleVec3 srgb(unsigned hex) {
    return StyleVec3(static_cast<float>((hex >> 16) & 0xFF) / 255.f, static_cast<float>((hex >> 8) & 0xFF) / 255.f,
                     static_cast<float>(hex & 0xFF) / 255.f);
}

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool equalsNoCase(std::string_view a, std::string_view lowerB) {
    if (a.size() != lowerB.size()) return false;
    for (std::size_t k = 0; k < a.size(); ++k)
        if (lower(a[k]) != lowerB[k]) return false;
    return true;
}

// The face fields say how a pack's hatching treats a face, so a blend weighs them by how much each side
// hatches rather than by blend weight alone: a third of the Unwritten keeps the Unwritten's face treatment
// in full. Where nothing hatches the plain weighted values stand. faceSnap blends linearly.
template <std::size_t N>
void blendFaceFields(StyleSettings& r, const std::array<const StyleSettings*, N>& in, const std::array<float, N>& w) {
    float sum = 0.f;
    for (std::size_t i = 0; i < N; ++i) sum += w[i] * std::max(in[i]->hatch, 0.f);
    if (!(sum > 0.f)) return;
    constexpr float StyleSettings::*kFields[] = {&StyleSettings::faceFeatures, &StyleSettings::faceContrast,
                                                 &StyleSettings::faceHatch, &StyleSettings::faceHatchMaxLevel,
                                                 &StyleSettings::faceSkinLit};
    for (float StyleSettings::*f : kFields) {
        // A value every hatching side shares is kept exactly (no rounding, so lerp(a, a, t) == a).
        float acc = 0.f, shared = 0.f;
        bool first = true, same = true;
        for (std::size_t i = 0; i < N; ++i) {
            const float k = w[i] * std::max(in[i]->hatch, 0.f);
            if (!(k > 0.f)) continue;
            acc += in[i]->*f * (k / sum);
            if (first) {
                shared = in[i]->*f;
                first = false;
            } else {
                same = same && in[i]->*f == shared;
            }
        }
        r.*f = same ? shared : acc;
    }
}

} // namespace

HatchWeights hatchWeights(float tone, float density, float curve, float maxLevel) {
    float dark = 1.f - std::clamp(tone, 0.f, 1.f);
    if (curve != 1.f) dark = std::pow(dark, std::max(curve, 1e-3f));
    const float d = std::min(std::clamp(dark * std::max(density, 0.f), 0.f, 1.f) * 5.f, std::max(maxLevel, 0.f));
    HatchWeights w;
    // Hat function around each level k (1..5): max(0, 1 - |d - k|); level 0 (paper) has no ink.
    for (int k = 1; k <= 4; ++k) w.tam[k - 1] = std::max(0.f, 1.f - std::abs(d - static_cast<float>(k)));
    w.solid = std::max(0.f, 1.f - std::abs(d - 5.f));
    return w;
}

StyleSettings lerp(const StyleSettings& a, const StyleSettings& b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    if (t <= 0.f) return a;
    if (t >= 1.f) return b;
    StyleSettings r;
#define FTCORE_STYLE_LERP(name, type, def) r.name = lerpField(a.name, b.name, t);
    FTCORE_STYLE_FIELDS(FTCORE_STYLE_LERP)
#undef FTCORE_STYLE_LERP
    // Island placement is not a look: keep the one of the side that actually has an island.
    if (a.island > 0.f && b.island <= 0.f) r.islandCenter = a.islandCenter, r.islandRadius = a.islandRadius;
    if (b.island > 0.f && a.island <= 0.f) r.islandCenter = b.islandCenter, r.islandRadius = b.islandRadius;
    // Same for the ink void's distances: a scene's void fades in or out at its own depth.
    if (a.inkVoid > 0.f && b.inkVoid <= 0.f) r.inkVoidNear = a.inkVoidNear, r.inkVoidFar = a.inkVoidFar;
    if (b.inkVoid > 0.f && a.inkVoid <= 0.f) r.inkVoidNear = b.inkVoidNear, r.inkVoidFar = b.inkVoidFar;
    blendFaceFields<2>(r, {&a, &b}, {1.f - t, t});
    return r;
}

StyleSettings stylePack(StylePack pack) {
    StyleSettings s;
    const StyleVec3 white(1.f);
    switch (pack) {
    case StylePack::None: break;
    case StylePack::Solhaven:
        // Heroic ink-and-gold: bold tapering hatch-black contour, hard cel light, primary colour on chalk,
        // gold in the lights, warm umber in the shadows, faint chalk tooth (2%).
        s.outline = 1.f;
        s.outlineWidth = 2.4f;
        s.outlineColor = srgb(0x14110F); // Hatch black
        s.outlineThreshold = 0.04f;
        s.outlineTaper = 1.f;
        s.cel = 1.f;
        s.celSteps = 2.f;
        s.saturation = 1.25f;
        s.highlightTint = mix(white, srgb(0xFFC93C), 0.22f); // Trophy gold
        s.shadowTint = mix(white, srgb(0xB07A35), 0.25f);    // Bronze
        s.paperGrain = 0.02f;
        s.chromaScale = 0.f;
        s.hull = 1.f;
        s.hullWidth = 2.6f;
        s.hullColor = srgb(0x14110F);
        // Parallel-line hatching inside the shadow band only, inked over the cel colour.
        s.hatch = 1.f;
        s.hatchScale = 240.f;
        s.hatchDensity = 1.f;
        s.hatchInk = srgb(0x14110F);
        s.hatchOverlay = 1.f;
        s.hatchMaxLevel = 2.f;
        s.hatchShadowOnly = 1.f;
        s.rimScale = 1.f;
        s.rimTint = srgb(0xFFC93C);
        s.faceFeatures = 1.f;
        break;
    case StylePack::Lattice:
        // Sharp webtoon finish: crisp, even night-indigo line, hard cel, near-black indigo shadows, neon
        // saturation, no paper; bloom kept to emissives; a cyan rim that stays below the bloom threshold.
        s.outline = 0.9f;
        s.outlineWidth = 1.0f;
        s.outlineColor = srgb(0x12072B); // Night indigo
        s.outlineThreshold = 0.06f;
        s.cel = 0.9f;
        s.celSteps = 2.f;
        s.saturation = 1.3f;
        s.shadowTint = StyleVec3(0.46f, 0.38f, 0.59f);
        s.highlightTint = mix(white, srgb(0x19E3F0), 0.1f); // Cyan
        s.bloomScale = 1.8f;
        s.bloomThresholdScale = 2.5f;
        s.vignetteScale = 1.5f;
        s.hull = 0.8f;
        s.hullWidth = 1.2f;
        s.hullColor = srgb(0x12072B);
        s.rimScale = 0.35f;
        s.rimTint = srgb(0x19E3F0);
        // v2 section 7 item 9 proposes blackPoint 0.06 only if the shadowTint multiply muddies the darks. It stays 0
        // until someone has looked at the picture.
        break;
    case StylePack::Unwritten:
        // Black-and-white pen-line page: fine ink line with an 8 fps tremble, the scene remapped onto ink and
        // paper, tone drawn as cross-hatching, blank-page sky, 7% paper tooth, the cast inked with a hull.
        s.outline = 1.f;
        s.outlineWidth = 1.3f;
        s.outlineColor = srgb(0x0B0B0C); // Ink
        s.outlineThreshold = 0.035f;
        s.outlineJitter = 1.1f;
        s.saturation = 0.f;
        s.monochrome = 1.f;
        s.monoGamma = 1.15f;
        s.halftone = 0.f;
        s.halftonePitch = 5.f;
        s.hatch = 1.f;
        s.hatchScale = 280.f;
        s.hatchDensity = 1.4f;
        s.hatchCurve = 1.8f;
        s.hatchInk = srgb(0x0B0B0C);
        s.hull = 1.f;
        s.hullWidth = 2.4f;
        s.hullColor = srgb(0x0B0B0C);
        s.paperSky = 1.f;
        s.paperGrain = 0.07f;
        s.bloomScale = 0.35f;
        s.vignetteScale = 0.4f;
        s.filmGrainScale = 0.f;
        s.chromaScale = 0.f;
        s.faceFeatures = 1.f;
        s.faceContrast = 1.f;
        s.faceHatch = 0.6f;
        s.faceHatchMaxLevel = 2.f;
        s.faceSkinLit = 0.3f;
        break;
    case StylePack::Restored:
        // Inked and coloured: fine, even restored-ink outline, soft cel bands, violet in the shadows,
        // morning-daylight lights with a breath of dawn rose, 3% paper.
        s.outline = 1.f;
        s.outlineWidth = 1.6f;
        s.outlineColor = srgb(0x17151A); // Restored ink
        s.outlineThreshold = 0.045f;
        s.cel = 0.85f;
        s.celSteps = 3.f;
        s.saturation = 1.08f;
        s.shadowTint = mix(white, srgb(0x7B2FF7), 0.12f);
        s.highlightTint = mix(srgb(0xFFF4DD), srgb(0xF2A3B3), 0.15f); // Morning daylight + dawn rose
        s.paperGrain = 0.03f;
        s.chromaScale = 0.f;
        s.hull = 1.f;
        s.hullWidth = 1.7f;
        s.hullColor = srgb(0x17151A);
        break;
    }
    return s;
}

const char* stylePackName(StylePack pack) {
    switch (pack) {
    case StylePack::None: return "none";
    case StylePack::Solhaven: return "solhaven";
    case StylePack::Lattice: return "lattice";
    case StylePack::Unwritten: return "unwritten";
    case StylePack::Restored: return "restored";
    }
    return "none";
}

std::optional<StylePack> parseStylePack(std::string_view name) {
    for (int i = 0; i < kStylePackCount; ++i) {
        const auto pack = static_cast<StylePack>(i);
        if (equalsNoCase(name, stylePackName(pack))) return pack;
    }
    return std::nullopt;
}

StyleSettings styleFromDial(const StyleDial& dial) {
    const std::array<float, 4> n = {std::clamp(dial.s, 0.f, 3.f), std::clamp(dial.l, 0.f, 3.f),
                                    std::clamp(dial.u, 0.f, 3.f), std::clamp(dial.r, 0.f, 3.f)};
    const float sum = n[0] + n[1] + n[2] + n[3];
    const StyleSettings none;
    if (sum <= 0.f) return none;
    const std::array<StyleSettings, 4> packs = {stylePack(StylePack::Solhaven), stylePack(StylePack::Lattice),
                                                stylePack(StylePack::Unwritten), stylePack(StylePack::Restored)};
    // Exactly one pack at full weight: return it verbatim (no rounding).
    for (std::size_t i = 0; i < 4; ++i)
        if (n[i] >= 3.f && sum - n[i] <= 0.f) return packs[i];
    const float denom = std::max(sum, 3.f);
    const float wNone = 1.f - sum / denom;
    StyleSettings r;
#define FTCORE_STYLE_BLEND(name, type, def)                                                                    \
    r.name = none.name * wNone + packs[0].name * (n[0] / denom) + packs[1].name * (n[1] / denom) +             \
             packs[2].name * (n[2] / denom) + packs[3].name * (n[3] / denom);
    FTCORE_STYLE_FIELDS(FTCORE_STYLE_BLEND)
#undef FTCORE_STYLE_BLEND
    blendFaceFields<5>(r, {&none, &packs[0], &packs[1], &packs[2], &packs[3]},
                       {wNone, n[0] / denom, n[1] / denom, n[2] / denom, n[3] / denom});
    return r;
}

std::optional<StyleDial> parseStyleDial(std::string_view text) {
    StyleDial d;
    float* slots[4] = {&d.s, &d.l, &d.u, &d.r};
    bool seen[4] = {false, false, false, false};
    int bare = 0, tokens = 0;
    std::size_t i = 0;
    auto isSep = [](char c) { return c == ',' || c == ' ' || c == ';' || c == '/'; };
    while (i < text.size()) {
        while (i < text.size() && isSep(text[i])) ++i;
        if (i >= text.size()) break;
        std::size_t j = i;
        while (j < text.size() && !isSep(text[j])) ++j;
        std::string_view tok = text.substr(i, j - i);
        i = j;
        int slot = -1;
        const char c = lower(tok[0]);
        if (c == 's' || c == 'l' || c == 'u' || c == 'r') {
            slot = c == 's' ? 0 : c == 'l' ? 1 : c == 'u' ? 2 : 3;
            tok.remove_prefix(1);
            if (!tok.empty() && (tok[0] == '=' || tok[0] == ':')) tok.remove_prefix(1);
        } else {
            if (bare != tokens) return std::nullopt; // no mixing of named and bare needles
            slot = bare++;
        }
        ++tokens;
        if (slot < 0 || slot > 3 || seen[slot] || tok.empty() || tok.size() > 15) return std::nullopt;
        char buf[16] = {};
        std::memcpy(buf, tok.data(), tok.size());
        char* end = nullptr;
        const float v = std::strtof(buf, &end);
        if (end != buf + tok.size() || !(v >= 0.f && v <= 3.f)) return std::nullopt;
        *slots[slot] = v;
        seen[slot] = true;
    }
    if (tokens == 0 || (bare > 0 && bare != 4)) return std::nullopt;
    return d;
}

StyleSettings inkSolhavenStyle() {
    StyleSettings s = styleFromDial({1.f, 0.f, 3.f, 0.f});
    s.saturation = 1.0f;
    s.monochrome = 0.8f;
    s.cel = 1.f;
    s.celSteps = 2.f;
    s.paperSky = 1.f;
    return s;
}

std::optional<StyleSettings> parseStyleSpec(std::string_view text) {
    if (equalsNoCase(text, "inksolhaven")) return inkSolhavenStyle();
    if (const auto pack = parseStylePack(text)) return stylePack(*pack);
    if (const auto dial = parseStyleDial(text)) return styleFromDial(*dial);
    return std::nullopt;
}

} // namespace ftcore
