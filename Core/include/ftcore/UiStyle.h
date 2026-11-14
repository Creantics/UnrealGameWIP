#pragma once
// The game UI's style table (Data/UI/style.json): colours per world and per colour-vision palette, the type ramp,
// line weights, and the optional layers each visual direction turns on. The Slate side reads every look from here,
// so the three directions (hairline, inscribed plates, world-anchored lens) are data, not code.
//
//   UiStyle s;  importUiStyle(text, s, &error);           // once; or defaultUiStyle()
//   UiColor c = uiColor(s, UiWorld::Solhaven, UiPalette::Default, UiToken::Accent);
//   float px  = uiTextPx(s, UiTextRole::Label, viewportHeight, textScale);
//
// Accessibility rules the table enforces (production/ui plan, section f):
//   - no text below 18 px at 1080p, scaled with the viewport height, and never below 14 px at any height;
//   - Ink on Backing and InkDim on Backing at least 4.5:1, in every world and palette (import rejects less); this is
//     the solid backing the player can turn on;
//   - the default look is a faint soft backing plus a hairline dark edge on every glyph: Ink, InkDim and Accent on
//     the edge colour at least 4.5:1 over a white picture (import rejects less), and the edge is never thinner than
//     1 px; a glyph takes the edge at 0.5..1 of its strength (edge.text), never fainter;
//   - Danger, Ally and WeakPoint are never the only cue: the table carries a shape id for each (uiCueShape), and
//     no two of them share a shape.
//
// Zero allocation after import.
#include "ftcore/Api.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ftcore {

enum class UiWorld : std::uint8_t { Solhaven, Lattice, Unwritten };
inline constexpr std::size_t kUiWorldCount = 3;

// Colour-vision palettes: Default, then a token set for each colour-vision type, then high contrast.
enum class UiPalette : std::uint8_t { Default, Protan, Deutan, Tritan, HighContrast };
inline constexpr std::size_t kUiPaletteCount = 5;

enum class UiToken : std::uint8_t {
    Ink,       // body text
    InkDim,    // labels
    Backing,   // the plate or scrim behind text (alpha is its strength)
    Track,     // a meter's empty part
    Accent,    // the world's accent: names, the lead's meters
    Accent2,   // the world's second accent
    Danger,    // tells, low health
    Ally,      // the partner and allies
    WeakPoint, // an open weak point
};
inline constexpr std::size_t kUiTokenCount = 9;

enum class UiTextRole : std::uint8_t { Title, Heading, Label, Value, Subtitle, Speaker, Prompt, Popup, Verb };
inline constexpr std::size_t kUiTextRoleCount = 9;

// Font families the style may name. The display and label face is a serif with real small caps (SerifSc: lowercase
// draws as small caps, figures stay lining); Sans is the secondary face, for key caps and figures only where the serif
// fails small. Hand is the Third's handwriting (a marked placeholder italic until an owned font exists).
enum class UiFont : std::uint8_t { Sans, SansBold, Serif, Hand, SerifBold, SerifSc, SerifScBold };
inline constexpr std::size_t kUiFontCount = 7;

// The cue shapes that carry meaning besides colour.
enum class UiCueShape : std::uint8_t { None, Diamond, Chevron, Circle, Square, Triangle, Ring };

// How meters are drawn. The direction's pick swaps this with the colours: Line (a hairline), Ring (an arc), Teeth (fine
// tick marks that drop out), Stroke (a drawn line rubbed out as it drains).
enum class UiMeterShape : std::uint8_t { Line, Ring, Teeth, Stroke };
inline constexpr std::size_t kUiMeterShapeCount = 4;

// A block that shows on change and rests after: fades in, stays for `rest` seconds after its last change, then fades
// to `restAlpha` (0 hides it).
struct UiFadeTimes {
    float in = 0.15f;   // seconds to full
    float rest = 2.5f;  // seconds held after the last change
    float out = 0.6f;   // seconds from full to restAlpha
    float restAlpha = 0.35f;
};

// The arena HUD's element budget (Core UiBudget): how much may be on screen at once.
struct UiBudgetRules {
    float rest = 3.f;            // seconds with nothing changed before the HUD rests (only the lead and party show)
    std::uint8_t marks = 10;     // at most this many marks in action
    float hint = 3.f;            // seconds the dialogue's key hints show after an input
};

// The visual direction's optional layers (any mix may be on).
struct UiLayers {
    bool hairlines = true;      // thin-line meters and frames
    bool plates = false;        // material plates behind blocks (per-world plate id)
    bool worldAnchored = false; // ground rings and leader lines placed in the world
    bool letterbox = true;      // cinematic bars in staged dialogue and cutscenes
};

struct UiColor {
    float r = 0.f, g = 0.f, b = 0.f, a = 1.f; // sRGB, 0..1
    bool operator==(const UiColor&) const = default;
};

struct UiTextStyle {
    UiFont font = UiFont::Sans;
    float px = 18.f;       // at 1080p, before the player's text scale
    float tracking = 0.f;  // extra spacing, ems
    bool upper = false;    // shown in capitals (in a small-caps face: all small caps, one even height)
};

struct UiStyle {
    // Per world, the default palette; each other palette overrides only the tokens it lists (overridden flags).
    std::array<std::array<UiColor, kUiTokenCount>, kUiWorldCount> world{};
    std::array<std::array<UiColor, kUiTokenCount>, kUiPaletteCount> paletteColor{};
    std::array<std::array<bool, kUiTokenCount>, kUiPaletteCount> paletteOverrides{};
    std::array<UiTextStyle, kUiTextRoleCount> text{};
    std::array<UiCueShape, kUiTokenCount> cueShape{}; // Danger, Ally, WeakPoint carry one; None elsewhere
    std::array<std::array<char, 24>, kUiWorldCount> plate{}; // plate material id per world ("" for none)
    UiLayers layers;
    float lineWeight = 3.f;     // px at 1080p, meters and frames
    float backingDefault = 0.75f; // the solid backing's alpha when the player turns it on
    // The soft backing drawn by default: a wide falloff with no edge, so it reads as shade, not as an object.
    float backingSoftHud = 0.24f;      // peak alpha behind HUD blocks
    float backingSoftDialogue = 0.42f; // peak alpha under subtitles
    // The local dim behind dialogue options, measured once per choice from the picture behind them (uiOptionDim): just
    // strong enough for the option ink to stand at this contrast over the picture's brightest tenth, never above max.
    float optionsDimContrast = 3.f;
    float optionsDimMax = 0.65f;
    UiColor edge{0.039f, 0.035f, 0.031f, 0.9f}; // the hairline edge round every glyph
    float edgePx = 1.f;                // its width at 1080p, at least 1 px at any height
    float edgeText = 0.6f;             // the glyph edge's share of the edge colour's alpha (0.5..1): lighter than a
                                       // stroke's edge, so a light word on bright ground does not read as doubled
    UiMeterShape meter = UiMeterShape::Line;
    // The Load's gold in the lead's name, the one gold on the fight screen: the fill, and the colour the whole name
    // brightens toward above the Release threshold. Its own entry, so no palette's cue colours move it.
    UiColor loadGold{0.914f, 0.702f, 0.420f, 1.f};   // #E9B36B
    UiColor loadBright{1.f, 0.878f, 0.541f, 1.f};    // #FFE08A
    UiFadeTimes fade;
    UiBudgetRules budget;
    float wordLumaFloor = 0.45f; // a coloured word's relative luminance is lifted to at least this (uiLumaFloor)
    float minTextPx = 18.f;     // the floor at 1080p
    float minTextPxAbsolute = 14.f; // the floor at any height
};

// The built-in style (the hairline system with Solhaven amber, Lattice cyan, Unwritten paper; letterbox on).
FTCORE_API UiStyle defaultUiStyle();

// Reads Data/UI/style.json. False on bad JSON, an unknown name, a value out of range, or a failed accessibility rule
// (*error says which); `out` is then left as it was.
FTCORE_API bool importUiStyle(std::string_view text, UiStyle& out, std::string* error = nullptr);

// Checks the accessibility rules above (also run by import). False with *error naming the first failure.
FTCORE_API bool validateUiStyle(const UiStyle& s, std::string* error = nullptr);

FTCORE_API UiColor uiColor(const UiStyle& s, UiWorld world, UiPalette palette, UiToken token);

// A role's size in pixels for a viewport `height` px tall at the player's text scale (1.0..2.0, clamped), never below
// the floors.
FTCORE_API float uiTextPx(const UiStyle& s, UiTextRole role, float height, float textScale);

// A line's thickness in pixels at `height`, at least 1.
FTCORE_API float uiLinePx(const UiStyle& s, float height);

// WCAG contrast ratio of `fg` over `bg`, each first composited over `under` (opaque) by its alpha.
FTCORE_API float uiContrast(UiColor fg, UiColor bg, UiColor under);

// `c` lifted toward white until its relative luminance is at least `floor` (alpha kept); unchanged when already there.
FTCORE_API UiColor uiLumaFloor(UiColor c, float floor);

// The dim behind the dialogue options: the least alpha of `backing` (0..s.optionsDimMax) under which `ink` reaches
// s.optionsDimContrast over `picture`; 0 when it already does, the max when even that falls short.
FTCORE_API float uiOptionDim(const UiStyle& s, UiColor ink, UiColor backing, UiColor picture);

// The picture behind a block, by luminance: add each pixel (sRGB, 0..1), then brightest() gives the mean colour of its
// brightest `share` of pixels. Fixed size, no allocation.
struct UiLumaHistogram {
    static constexpr std::size_t kBins = 64;
    std::array<std::uint32_t, kBins> n{};
    std::array<float, kBins> r{}, g{}, b{};
    std::uint32_t total = 0;
};
FTCORE_API void uiLumaAdd(UiLumaHistogram& h, UiColor c);
FTCORE_API UiColor uiLumaBrightest(const UiLumaHistogram& h, float share);

FTCORE_API UiCueShape uiCueShape(const UiStyle& s, UiToken token);

// The glyph edge's width in pixels at `height`, at least 1.
FTCORE_API float uiEdgePx(const UiStyle& s, float height);

// The fight HUD's margin on each side, px. Each side is the HUD's own margin (`basePx`: its 1080p margin at the frame's
// scale), plus the platform's title-safe padding on that side (`padding`, px in the same units), plus the player's HUD
// safe margin (Settings::hudSafeMargin, clamped to 0..10) as a percent of the viewport's `width` on the left and right
// and of its `height` at the top and bottom. A negative or NaN input counts as 0. Edge marks sit on these; a centred mark
// stays on the viewport's centre whatever the sides are, so a wide screen (21:9) widens the gap between them, not the
// marks' places from the middle.
struct UiMargins {
    float left = 0.f, top = 0.f, right = 0.f, bottom = 0.f;
};
FTCORE_API UiMargins uiHudMargins(float width, float height, float basePx, const UiMargins& padding, int safePercent);

// A fading block's state. Call uiFadeStep each frame with whether its values changed this frame.
struct UiFade {
    float alpha = 0.f;
    float sinceChange = 1e9f; // seconds since the last change
};
FTCORE_API void uiFadeStep(UiFade& f, bool changed, float dt, const UiFadeTimes& t);

FTCORE_API std::string_view uiWorldName(UiWorld w);     // "solhaven", "lattice", "unwritten"
FTCORE_API std::string_view uiPaletteName(UiPalette p); // "default", "protan", "deutan", "tritan", "high_contrast"
FTCORE_API std::string_view uiTokenName(UiToken t);     // "ink", "ink_dim", ...
FTCORE_API std::string_view uiTextRoleName(UiTextRole r);
FTCORE_API std::string_view uiMeterShapeName(UiMeterShape m); // "line", "ring", "teeth", "stroke"

} // namespace ftcore
