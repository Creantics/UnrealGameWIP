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
//   - Ink on Backing and InkDim on Backing at least 4.5:1, in every world and palette (import rejects less);
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

enum class UiTextRole : std::uint8_t { Title, Heading, Label, Value, Subtitle, Speaker, Prompt, Popup };
inline constexpr std::size_t kUiTextRoleCount = 8;

// Font families the style may name. Sans is the engine's Roboto until the final font is ruled; Hand is the
// Third's handwriting (a marked placeholder italic until an owned font exists).
enum class UiFont : std::uint8_t { Sans, SansBold, Serif, Hand };
inline constexpr std::size_t kUiFontCount = 4;

// The cue shapes that carry meaning besides colour.
enum class UiCueShape : std::uint8_t { None, Diamond, Chevron, Circle, Square, Triangle, Ring };

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
    bool upper = false;    // shown in capitals
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
    float backingDefault = 0.75f; // the backing's alpha unless the player changes it (0 allowed only with an outline)
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

FTCORE_API UiCueShape uiCueShape(const UiStyle& s, UiToken token);

FTCORE_API std::string_view uiWorldName(UiWorld w);     // "solhaven", "lattice", "unwritten"
FTCORE_API std::string_view uiPaletteName(UiPalette p); // "default", "protan", "deutan", "tritan", "high_contrast"
FTCORE_API std::string_view uiTokenName(UiToken t);     // "ink", "ink_dim", ...
FTCORE_API std::string_view uiTextRoleName(UiTextRole r);

} // namespace ftcore
