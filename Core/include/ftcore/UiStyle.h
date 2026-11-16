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
    Fill,      // a value line's fill (health, guard, partner, boss): the world's own colour
};
inline constexpr std::size_t kUiTokenCount = 10;

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

// A world's rules for where its second accent may go. The Unwritten sets both, so its blood red (accent2) never sits on
// text: it is the one colour in that world's HUD and lives on marks only. A world object in the data that leaves a key
// out sets it false.
//   marksAccent2:    a non-text mark (a value line such as the Load's, the current lead's dot, the weak-point mark)
//                    wears accent2 (uiMarkColor); "marks_accent2" in the data;
//   dangerUnderline: a danger word is drawn in ink, with danger as its underline (uiDangerWord); "danger_underline".
//   The three signature devices (the owner's pick flips one row; all off by default, and off draws the base path):
//   sigPulse:       the Load's Release spike and its travelling pulse (uiReleasePulseStep); "sig_pulse";
//   sigArcs:        the boss and lead lines on concentric ring arcs (uiRingPoint); "sig_arcs";
//   sigMarginRule:  a vertical rule down the party block's left edge (UiMarginRule); "sig_margin_rule".
struct UiWorldRules {
    bool marksAccent2 = false;
    bool dangerUnderline = false;
    bool sigPulse = false;
    bool sigArcs = false;
    bool sigMarginRule = false;
};

// Value-line ends and the dotted guard, 1080p px at HUD scale 1 (the painter scales them and rounds each to whole device
// px). An end cap is a square of ink capGrow px wider than its line's weight, on the fill's end: it moves with the end
// and sits on the track's start at 0 fill (uiCapX); a line with no value has none; the dotted guard has none. The guard
// is square dots guardDot across at guardPitch px centres.
struct UiLineShape {
    float capGrow = 4.f;
    float guardDot = 3.f;
    float guardPitch = 7.f;
};

// The margin rule (the third signature device): a rule `width` px wide `gap` px left of the party block's lines, ink at
// `alpha`, from the guard row's foot to the cap top of the highest name shown; the lines start `gap` px right of it.
// The boss line carries a vertical tick `tickW` x `tickH` at its start.
struct UiMarginRule {
    float gap = 12.f;
    float width = 2.f;
    float alpha = 0.6f;
    float tickW = 2.f;
    float tickH = 16.f;
};

struct UiStyle {
    // Per world, the default palette; each other palette overrides only the tokens it lists (overridden flags).
    std::array<std::array<UiColor, kUiTokenCount>, kUiWorldCount> world{};
    std::array<UiWorldRules, kUiWorldCount> rules{};
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
    UiLineShape lines;
    UiMarginRule marginRule;
    float wordLumaFloor = 0.45f; // a coloured word's relative luminance is lifted to at least this (uiLumaFloor)
    float minTextPx = 18.f;     // the floor at 1080p
    float minTextPxAbsolute = 14.f; // the floor at any height
};

// The built-in style (the hairline system with Solhaven amber, Lattice cyan, Unwritten cold grey; letterbox on).
FTCORE_API UiStyle defaultUiStyle();

// Reads Data/UI/style.json. False on bad JSON, an unknown name, a value out of range, or a failed accessibility rule
// (*error says which); `out` is then left as it was.
FTCORE_API bool importUiStyle(std::string_view text, UiStyle& out, std::string* error = nullptr);

// Checks the accessibility rules above (also run by import). False with *error naming the first failure.
FTCORE_API bool validateUiStyle(const UiStyle& s, std::string* error = nullptr);

FTCORE_API UiColor uiColor(const UiStyle& s, UiWorld world, UiPalette palette, UiToken token);

// A non-text mark's colour: accent2 in a world whose marks wear it (UiWorldRules::marksAccent2), else `usual`, the
// mark's own colour (a value line its meter's colour and the weak-point mark ink). No palette
// overrides accent2, so the mark keeps it in every palette; its shape and edge carry it.
FTCORE_API UiColor uiMarkColor(const UiStyle& s, UiWorld world, UiPalette palette, UiColor usual);

// A value line's three tones (health, guard, partner, boss; one family): the unfilled track's body, the world's backing
// at kUiTrackBodyA (solid in high contrast); a 1 device px top hairline in the track token (white at 0x73); and the
// fill, the world's fill token (high contrast: white). The fill holds 10:1 or more against the body's own colour.
inline constexpr float kUiTrackBodyA = 0.45f;
struct UiTrackSet {
    UiColor body;     // the unfilled track's body (alpha is its strength over the picture)
    UiColor hairline; // its 1 device px top edge (alpha likewise)
    UiColor fill;     // the value
};
FTCORE_API UiTrackSet uiTrackSet(const UiStyle& s, UiWorld world, UiPalette palette);

// How a danger word is drawn. In a world with UiWorldRules::dangerUnderline the glyphs are ink and the danger colour is
// the underline beneath them; elsewhere the glyphs are danger and there is no underline. Both colours follow the
// palette; the painter still lifts a coloured word to s.wordLumaFloor as it does today.
struct UiDangerWord {
    UiColor word;      // the glyphs
    UiColor underline; // the underline (the danger colour; drawn only when underlined)
    bool underlined = false;
};
FTCORE_API UiDangerWord uiDangerWord(const UiStyle& s, UiWorld world, UiPalette palette);

// Every other place the fight HUD may put the danger colour; the painter reads the danger token nowhere but here and
// through uiDangerWord. Red means harm to the player only: the hit cue, the hp just lost, the low-health underline and
// the call cue's triangle. The hit cue is one red in every world (the danger token in the palette shown): a glow on the
// screen's edges over a darkening layer (kUiHitGlow* and kUiHitDarkK below), and the hp just lost drawn in it on the
// lead's hp line. In a world with UiWorldRules::dangerUnderline a low hp or guard line keeps the fill and carries a 1 px
// danger underline under its lit length (the pulse moves onto it); elsewhere the low line is danger, with no underline.
// The weak-point mark is not danger: it wears the WeakPoint token (Mark gold) in every world.
struct UiDangerSet {
    UiColor hitCue;       // the hit's edge glow and the lost hp while the cue lives
    UiColor lowLine;      // a low value line
    UiColor lowUnderline; // the line under it, drawn only when lowUnderlined
    bool lowUnderlined = false;
    UiColor cueMark;      // the call cue's danger triangle
    UiColor hitDark;      // the hit cue's darkening layer under the red (danger scaled by kUiHitDarkK)
};
// The danger colour's contrast against the world's fill, the line it is drawn beside (the lost hp at the fill's end,
// with a 1 px ink separator between them; the low underline under it). Import holds it 3:1, with the hit cue's red
// against every world's fill, as well as danger 3:1 against the track body's colour and the ink (2026-11-16 ruling).
FTCORE_API float uiDangerFillContrast(const UiStyle& s, UiWorld world, UiPalette palette);
// The hit cue's darkening layer: the danger colour's sRGB channels times this (C9000E gives 4E0005).
inline constexpr float kUiHitDarkK = 0.39f;
FTCORE_API UiDangerSet uiDangerSet(const UiStyle& s, UiWorld world, UiPalette palette);

// The hit cue's glow (arm fB, ruled 2026-11-16 01:28): two layers on each edge, laid over the frame under every HUD
// line and word, each falling from the frame's edge to nothing kUiHitGlowShare of that edge's axis in (6 percent of
// the width at the sides, of the height at top and bottom). First a darkening layer in UiDangerSet::hitDark, then the
// red in hitCue over it (uiHitLayers), both on one curve, f^kUiHitGlowPow with f = 1 - smoothstep(in), and one share:
// the struck side at full, the other three at kUiHitOffSide; with no side (a hazard), all four at full. With reduced
// flashes only the lost hp is drawn.
inline constexpr float kUiHitGlowShare = 0.06f;
inline constexpr float kUiHitGlowPeak = 0.38f; // the red layer at the struck edge
inline constexpr float kUiHitGlowPow = 1.6f;   // both layers' fall: (1 - smoothstep)^this
inline constexpr float kUiHitDarkPeak = 0.30f; // the dark layer at the struck edge
// The two layers' alphas at `in` (the share of the way in from the frame's edge, 0 at the edge, 1 at kUiHitGlowShare)
// on an edge of weight `edge` (from uiHitEdges), before the cue's strength. Where two edges meet, take each layer's
// larger value of the two edges, never the two stacked.
struct UiHitLayers {
    float dark = 0.f;
    float red = 0.f;
};
FTCORE_API UiHitLayers uiHitLayers(float in, float edge);
inline constexpr float kUiHitOffSide = 0.25f;
inline constexpr float kUiHitFull = 0.1f;    // seconds at full strength
inline constexpr float kUiHitGone = 0.35f;   // seconds until it is gone
inline constexpr float kUiHitRepeat = 1.f / 3.f; // the least time between two cues' starts (3 a second at most)
// The cue's strength `age` seconds after it starts: 1 until kUiHitFull, a smooth fall to 0 at kUiHitGone, 0 outside.
FTCORE_API float uiHitFlashLevel(float age);
// Each blow's lost health in red: its own segment [lo, hi] of the line (0..1), drawn over lo..lo + (hi - lo) times
// uiHitFlashLevel(age), so every hit shows its loss for kUiHitGone whatever the glow's cap. A fixed ring of the last
// kUiHitSegs blows (the oldest dropped). A rise (a heal) clears them; so does the first step (it only reads the hp).
inline constexpr std::size_t kUiHitSegs = 4;
struct UiHitSeg {
    float lo = 0.f, hi = 0.f, age = kUiHitGone; // gone until a blow sets it
};
struct UiHitSegs {
    std::array<UiHitSeg, kUiHitSegs> seg{};
    float last = -1.f;   // the hp the last step saw; negative before the first
    double clock = -1.0; // the clock time of the last step; negative before the first
    std::uint8_t next = 0;
};
// One step at clock time `now` (the runner's fight clock, GetNow()): segments age by the clock's change since the last
// step, so a second step at the same time ages nothing, and a clock that runs back (a retry) ages nothing. A NaN hp
// makes no segment and leaves `last` as it was.
FTCORE_API void uiHitSegsStep(UiHitSegs& s, float hp01, double now);

// The clock's change since `last` (then set to `now`): 0 on the first call (last negative), on no change and on a clock
// that runs back. Every timed HUD state ages by this, never by a Tick's or a paint's own delta.
FTCORE_API float uiClockStep(double& last, double now);
// Each edge's weight for a blow from `rightAhead` (the striker from the lead on the camera's ground plane: x right, y
// ahead; need not be unit): the nearer of left/right or top (ahead)/bottom (behind) at 1, the others at kUiHitOffSide.
// With no side, or a zero vector, every edge at 1.
struct UiHitEdges {
    float left = 1.f, right = 1.f, top = 1.f, bottom = 1.f;
    bool operator==(const UiHitEdges&) const = default;
};
FTCORE_API UiHitEdges uiHitEdges(bool hasSide, float rightX, float aheadY);
// The cue's age on the runner's fight clock (`now`, GetNow(); it holds in a capture's hold and when the game is paused,
// so the painter hides the cue while paused). `stamp` is the host's last hit time on that clock, negative when the
// fight has had no hit. A second step at the same `now` adds nothing. The cue starts (age exactly 0) only when the stamp
// moves later, is not negative and the last cue started kUiHitRepeat or more ago; a later stamp sooner is taken in
// without a new cue. Any other change (a retry resetting it) ends it. The first tick after `seeded` is cleared (the
// view's first, or a retry's) only reads the stamp. Returns the age.
struct UiHitWatch {
    double seen = 0.0;
    double clock = -1.0;
    float age = 1e3f;
    bool seeded = false;
};
FTCORE_API float uiHitWatchStep(UiHitWatch& w, double stamp, double now);

// A low value line's underline and the guard row under it, in 1080p px at HUD scale 1 (r19c): the underline kUiLowUnderW
// thick, kUiLowUnderGap below the line's bottom edge (the room is kept when no underline is drawn), and the guard row's
// top kUiGuardClear below the underline. Each gap is a whole number of device pixels and never under kUiGapMinPx.
inline constexpr float kUiLowUnderGap = 8.f;
inline constexpr float kUiLowUnderW = 2.f;
inline constexpr float kUiGuardClear = 8.f;
inline constexpr float kUiGapMinPx = 8.f;
inline constexpr float kUiGuardClearMinPx = kUiGapMinPx;
struct UiLowGuardGaps {
    float gap = 0.f;   // line's bottom edge to the underline's top
    float under = 0.f; // the underline's thickness
    float clear = 0.f; // the underline's bottom to the guard row's top
};
// Those gaps in local units for the HUD's scale `s` and a device pixel `px` local units wide: each a whole number of
// device pixels; the two gaps at least kUiGapMinPx device pixels, the underline at least one.
FTCORE_API UiLowGuardGaps uiLowGuardGaps(float s, float px);

// The fight's one-word labels (r19c rule): LOAD, WEAK and GUARD, NotePx ink, 8 px right of their line's end, shown
// kUiLabelHold seconds and faded over kUiLabelFade. LOAD and WEAK show on their first appearance in each fight only (the
// host resets them when a fight begins); GUARD shows again on each change of guard state. Ages run on the fight clock.
inline constexpr float kUiLabelHold = 2.f;
inline constexpr float kUiLabelFade = 1.f;
inline constexpr float kUiGuardLabelHold = kUiLabelHold;
inline constexpr float kUiGuardLabelFade = kUiLabelFade;
// A label's alpha `age` seconds after it showed: 1 to kUiLabelHold, a straight fall to 0 at kUiLabelHold + kUiLabelFade.
FTCORE_API float uiLabelLevel(double age);

// GUARD: only these states count (crossing the low threshold is not a change, so the label never flickers).
enum class UiGuardState : std::uint8_t { Full, Under, Spent, Broken };
FTCORE_API UiGuardState uiGuardState(float stamina01, bool broken);
struct UiGuardLabel {
    UiGuardState state = UiGuardState::Full;
    double since = -1e9; // the clock time of the last change
};
// One step at clock time `now`: returns the label's alpha (0..1). Full shows none.
FTCORE_API float uiGuardLabelStep(UiGuardLabel& g, UiGuardState state, double now);

// LOAD and WEAK: once a fight, on the first step that sees the thing present; none while it is absent.
struct UiOnceLabel {
    bool shown = false;
    double since = 0.0; // the clock time it first showed
};
FTCORE_API float uiOnceLabelStep(UiOnceLabel& g, bool present, double now);
using UiLoadLabel = UiOnceLabel;
// LOAD's label: present while some Load is held. The cap moving or the Release opening is not a new appearance.
FTCORE_API float uiLoadLabelStep(UiLoadLabel& g, int load, double now);

// The boss's percent beside its line: floor(100 hp / hpMax), at least 1 while hp is above 0, 0 at or under 0, at most
// 100; -1 (none shown) when hpMax is 0 or less.
FTCORE_API int uiBossPercent(int hp, int hpMax);

// An end cap's centre x on a line from x0, `len` long, filled to fill01, for a cap `capW` wide: the fill's end, held
// inside the track so the cap sits on its start at 0 fill and on its end when full.
FTCORE_API float uiCapX(float x0, float len, float fill01, float capW);

// The first signature device, the Release pulse (UiWorldRules::sigPulse). When the Release opens, one pulse travels the
// Load line from its start to the spike over kUiReleaseTravel, then the spike's peak grows kUiReleaseGrow times for
// kUiReleasePeak; while overloaded the peak repeats every kUiReleaseRepeat. Clock based: the same `now` twice gives the
// same view.
inline constexpr float kUiReleaseTravel = 0.3f;
inline constexpr float kUiReleasePeak = 0.15f;
inline constexpr float kUiReleaseGrow = 1.5f;
inline constexpr float kUiReleaseRepeat = 1.2f;
struct UiReleasePulse {
    bool ready = false;
    bool over = false;
    double readyAt = -1e9;
    double overAt = -1e9;
};
struct UiReleasePulseView {
    float travel = -1.f; // the pulse's place along the line to the spike, 0..1; negative: none
    float peak = 1.f;    // the spike's peak height factor (1 or kUiReleaseGrow)
};
FTCORE_API UiReleasePulseView uiReleasePulseStep(UiReleasePulse& p, bool ready, bool overloaded, double now);

// The second signature device, ring arcs (UiWorldRules::sigArcs): a line `chord` long bowed `sag` at its middle on a
// circle. uiRingPoint gives the point a share `t` (0..1) along, offset `offset` outward along the radius (a concentric
// line), as x along the chord from its start and y toward the bulge, with the unit tangent there; sag 0 is the straight
// line. uiArcSag gives the sag of a chord on a circle of `radius`; uiArcSegments the segments that keep each piece at
// most maxChordPx long.
struct UiRingPoint {
    float x = 0.f, y = 0.f;
    float tx = 1.f, ty = 0.f;
};
FTCORE_API UiRingPoint uiRingPoint(float t, float chord, float sag, float offset = 0.f);
FTCORE_API float uiArcSag(float chord, float radius);
FTCORE_API int uiArcSegments(float chord, float maxChordPx);

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
