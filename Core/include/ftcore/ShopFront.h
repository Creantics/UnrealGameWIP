#pragma once

// Round 6 shopfronts: what each shop bay of an r5 facade (WindowFacade.h) shows at street level, chosen per bay from the
// building's seed and the bay's column, and the HLSL the window master runs for it.
//
// A bay takes one of six kinds (glazed shop, cafe, shutter shop, eatery, clinic, open stall), never the kind beside it.
// Every bay but a shutter shop stands on a stall riser (tile, stone, painted steel or timber, 0.45..0.75 m; a stall's is
// a 0.9 m counter), so the glass starts over it and the reveal turns at its top. Glazed shops, cafes, eateries and
// clinics have a glazed door down to the floor (left, right or, for a clinic, in the middle) with a kick plate and a
// handle; shutter shops have a roller shutter part or all of the way down; most bays carry a shutter box at the
// opening's head. A cafe has a striped valance under its fascia, an eatery its cloth panels and lantern.
//
// The fascia board over each bay carries a sign: a generic shop word (no names, no brands) in a 5 x 7 pixel face, on a
// pale panel or straight on the board, or an abstract mark of brush-like strokes in square cells. Lit shops' signs glow
// at night: a lightbox (the panel) or lit letters.
//
// The shop picture behind the glass (an atlas cell kWindowRooms..) is picked per bay, stepped on (twice at most) on an
// odd column past both even neighbours' picks, so no two bays side by side show the same picture; each bay also takes
// its own crop (zoom and shift) of the cell.
//
// The same maths runs on the GPU: shopFrontPreHlsl and shopFrontHlsl write two fragments that windowFacadeHlsl splices
// into the r5 window master's Custom node (see each function for the names it reads and sets). Plain C++20, float
// maths like the GPU's. No allocation except the HLSL writers (build time only).

#include "ftcore/Api.h"

#include <cstdint>
#include <string>

namespace ftcore {

enum class ShopFrontKind : std::uint8_t { Glazed = 0, Cafe, Shutter, Eatery, Clinic, Stall };
inline constexpr std::uint32_t kShopFrontKinds = 6;

enum class ShopRiser : std::uint8_t { None = 0, Tile, Stone, Steel, Timber };
enum class ShopDoor : std::uint8_t { None = 0, Left, Right, Middle };
enum class ShopSignStyle : std::uint8_t { PanelWord = 0, BoardWord, Strokes };
inline constexpr std::uint32_t kShopSignStyles = 3;

struct ShopFrontSpec {
    float riserMin = 0.45f, riserMax = 0.75f;  // m: a stall riser's height over the street floor
    float counter = 0.9f;                       // m: a stall's counter
    float doorWidth = 0.95f;                    // m
    float doorHeight = 2.1f;                    // m: its head, a transom bar over it
    float doorFrame = 0.05f;                    // m: a door's frame and the mullion beside it
    float kick = 0.22f;                         // m: a door's kick plate
    float housing = 0.2f;                       // m: a shutter box at the opening's head
    float housingShare = 0.6f;                  // the share of glazed shops, clinics and eateries with a shutter box
    float shutterLeast = 0.35f;                 // the least share of its opening a shutter shop's shutter covers
    float zoomLeast = 0.8f;                     // a bay's crop of its picture: zoom zoomLeast..1, shifted inside it
    float valance = 0.32f;                      // m: a cafe's striped valance under the fascia
    float signMargin = 0.06f;                   // m: a sign panel's margin in the fascia
};

// One bay's choices.
struct ShopBay {
    ShopFrontKind kind = ShopFrontKind::Glazed;
    ShopRiser riser = ShopRiser::None;
    float riserHeight = 0.0f;   // m, 0 with no riser
    ShopDoor door = ShopDoor::None;
    bool housing = false;       // a shutter box at the opening's head
    float shutter = 0.0f;       // the share of the opening a shutter covers from the top (shutter shops only)
    std::uint32_t picture = 0;  // the room atlas cell: kWindowRooms + 0..kWindowShops-1
    float zoom = 1.0f, shift = 0.0f;  // the picture's crop across: px' = shift + px * zoom
    std::uint32_t colour = 0;   // the riser's (and a tile's) colour, 0..3
    std::uint32_t word = 0;     // index into shopSignWord
    ShopSignStyle style = ShopSignStyle::PanelWord;
    std::uint32_t ink = 0;      // the sign's ink or panel colour index, 0..kShopInkCount-1
};

// The kind a bay's hash alone picks (as windowShopRawKind) and the one it shows: on an odd column stepped on (twice at
// most) past its even neighbours', so no two bays side by side share a kind. No allocation.
FTCORE_API std::uint32_t shopFrontRawKind(std::uint32_t seed, std::uint32_t column);
FTCORE_API ShopFrontKind shopFrontKind(std::uint32_t seed, std::uint32_t column);
// The shop picture a bay's hash alone picks, from its kind (a cafe or eatery leans to the cafe picture, a clinic to the
// waiting room), and the one it shows, stepped on an odd column past both even neighbours'. kWindowRooms + 0..3. No
// allocation.
FTCORE_API std::uint32_t shopFrontRawPicture(std::uint32_t seed, std::uint32_t column, ShopFrontKind kind);
FTCORE_API std::uint32_t shopFrontPicture(std::uint32_t seed, std::uint32_t column);
// Everything a bay shows. No allocation.
FTCORE_API ShopBay shopFrontBay(const ShopFrontSpec& spec, std::uint32_t seed, std::uint32_t column);

// The opening's bottom at a point of the bay, in metres over the street floor: 0.02 in a door's span or across a
// shutter shop (the shutter runs to the floor), else the riser's top. `uOpen` is the point's place across the opening,
// 0..1 from its left. No allocation.
FTCORE_API float shopFrontSill(const ShopFrontSpec& spec, const ShopBay& bay, float openingWidth, float uOpen);
// A door's span across the opening, 0..1 from its left; both 0 with no door. No allocation.
struct ShopSpan {
    float a = 0.0f, b = 0.0f;
};
FTCORE_API ShopSpan shopFrontDoorSpan(const ShopFrontSpec& spec, const ShopBay& bay, float openingWidth);

// ---- Signs ----

// The sign words: generic shop words in capitals A-Z, at most kShopWordLetters each; none a name or a brand. Grouped by
// the kind that uses them (shopSignGroup). No allocation.
inline constexpr std::uint32_t kShopWordLetters = 8;
inline constexpr std::uint32_t kShopWordCount = 30;
FTCORE_API const char* shopSignWord(std::uint32_t index);
struct ShopWordGroup {
    std::uint32_t first = 0, count = 0;
};
FTCORE_API ShopWordGroup shopSignGroup(ShopFrontKind kind);
// The 5 x 7 face: whether letter (A-Z, else blank) has ink at column x (0..4 from the left) and row y (0..6 from the
// top). No allocation.
FTCORE_API bool shopGlyphInk(char letter, std::uint32_t x, std::uint32_t y);
inline constexpr std::uint32_t kShopInkCount = 6;

// Whether a sign shows ink at (x, y) across its panel (0..1 from the left, 0..1 from the bottom), the panel `width` by
// `height` metres: a word set in the face, centred, its letters height / 9 high per pixel (narrower when the word would
// pass 92% of the width); or for Strokes, 2..4 square cells of strokes. No allocation.
FTCORE_API bool shopSignInk(const ShopBay& bay, float width, float height, float x, float y);

// ---- HLSL ----

// The fragment run once a bay's opening is set across (after windowFacadeHlsl's u0 and u1, before v0). Reads seed, col,
// u, u0, u1, bw; declares the struct FTShop (its hash) and sets kind, kh (as r5), shopSill (m, the opening's bottom at
// this point, for v0), shopPick (the room cell index past kWindowRooms, for pick), shopZoom and shopShift (the crop, for
// px). Allocates: build time only.
FTCORE_API std::string shopFrontPreHlsl(const ShopFrontSpec& spec);
// The fragment in place of r5's shop kind block (from the kind's front to the lantern). Reads the names the r5 block
// read (shop, inBay, part, gu, gv, u, u0, u1, v0, v1, bw, sh, Up, shopTop, fascia, a, seed, col, kind, kh) and sets sp,
// own, fc, panel, fasciaRgb and part as it did (parts 2 frame, 10 steel, 11 matte own, 12 lantern, 16 valance, 4 the
// fascia and its sign), plus signLight (float3: the glow of this fascia pixel when its shop is lit at night, for
// signGlow). `fasciaHeight` is the WindowGridSpec's fascia. Allocates: build time only.
FTCORE_API std::string shopFrontHlsl(const ShopFrontSpec& spec, float fasciaHeight);

}  // namespace ftcore
