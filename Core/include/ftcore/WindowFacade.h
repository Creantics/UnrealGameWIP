#pragma once

// Round 5 facades: the interior-mapped windows of WindowInterior.h, varied per building type, with real depth, a ground
// floor band, six kinds of shopfront, wall tints and roof finishes.
//
// A building takes a type (apartment, walkway block, shophouse, office; 0 keeps WindowInterior's r4 grid): each type
// cuts its own storeys and bays and opens its own window (width, sill, height; an office's runs as a ribbon from mullion
// to mullion), the width jittered a little per building. Each opening sits a reveal deep in the wall: the eye sees the
// glass shifted by the reveal along its view, and between the wall's edge and the shifted glass the jambs, the reveal's
// bottom and its head; a sill lip juts under each window. The smear r4 showed at grazing angles (the eased ray clamped
// so far sideways that most of the glass met one thin strip of the side wall) is cut at its cause: the sideways slope
// is clamped so that at most sideShare of the glass ever meets a side wall.
//
// A street face's ground storey is a shop band; on a plinth type (walkway, office, or any face of plinthFrom metres or
// more) every face's ground storey is a band, of one of kWindowBandColors with tile courses, under a string course.
// Shop bays take one of six kinds (glazed shop, cafe, shutter shop, eatery, clinic lobby, laundromat), never the same as
// the bay beside, each under a fascia board of its own colour with a plain sign panel (no lettering).
//
// The same maths runs on the GPU: windowFacadeHlsl, windowFacadeFaceHlsl and windowFacadeComposeHlsl write the r5
// window master's three Custom nodes from these functions, tables and constants. Plain C++20, float maths like the
// GPU's. No allocation except the HLSL writers (build time only).

#include "ftcore/Api.h"
#include "ftcore/MassingPlan.h"
#include "ftcore/WindowInterior.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ftcore {

enum class WindowType : std::uint8_t { Legacy = 0, Apartment = 1, Walkway = 2, Shophouse = 3, Office = 4 };
inline constexpr std::uint32_t kWindowTypes = 5;

// One type's grid and opening (metres).
struct WindowTypeSpec {
    float storey = 3.0f, bay = 2.5f;
    float width = 1.35f;   // the window width (Legacy: the material's WinW)
    float sill = 0.85f, opening = 1.45f;
    float pier = 0.25f;    // the least wall between a window and its bay's side (a ribbon's mullion is twice this)
    float reveal = 0.0f;   // how deep the glass sits in the wall
    bool ribbon = false;   // the window fills its bay but for the mullion
    bool plinth = false;   // every face's ground storey is a band
    float frame = 0.06f;   // the window frame's width (Legacy: the base spec's)
};
FTCORE_API WindowTypeSpec windowTypeSpec(WindowType type);

struct WindowFacadeSpec {
    WindowGridSpec base;          // the r4 numbers every type keeps but its own
    float sideShare = 0.25f;      // at most this share of the glass meets a side wall of the room
    float widthJitter = 0.08f;    // a building's window width: the type's times 1 -/+ this, by its seed
    float plinthFrom = 15.0f;     // m: a face this high (or more) over its street floor has a ground floor band
    float course = 0.2f;          // m: the string course along the top of a band
    float tileCourse = 0.25f;     // m between a band's tile courses
    float lipHeight = 0.09f;      // m: a window's sill lip, under its opening
    float lipOut = 0.08f;         // m the lip runs past the opening each side
    float sillShadow = 0.14f;     // m: the lip's shadow on the wall under it (a darker strip)
    float shopReveal = 0.2f;      // m: a shopfront's glass in from the wall (deep enough that its jambs show the depth)
    float headShadow = 0.4f;      // m: by day the glass darkens toward its head over this height (the reveal's shade)
    float loggiaDepth = 1.1f;     // m: a recessed balcony's glass in from the wall
    float railHeight = 1.0f;      // m: its rail or parapet over its floor
    float baluster = 0.11f;       // m between its balusters
    float smallWidth = 0.6f, smallHeight = 0.6f;  // m: a small frosted window, at the head of the type's opening
    float grillePitch = 0.15f;    // m between a security grille's uprights (its rails twice that)
    float litShare = 0.68f;       // the master's LitShare: the share of rooms lit at night on storeys 0 and 1
    float upperLit = 0.2f;        // the top storey's lit share is LitShare * (1 + upperLit), the first storey's LitShare
    float signGlow = 1.6f;        // the master's SignGlow: a lit shop sign's light at night (times the collection's NightGlow)
    float litMax = 0.85f;         // the most any storey's lit share reaches (never a whole facade)
    float wallMax = 0.75f;        // the brightest a tinted wall's or roof's albedo gets (no glare under night lights)
    // Round 10 (motion sickness): a room is a box fixed in the wall's own frame, seen by the true view ray (no squash,
    // clamp or tilt), roomDepth deep; only roomShare of the non-shop windows show one. The rest are covered: frostShare
    // of them frosted glass, the others a curtain or blind drawn all the way (opaque, no room behind), lit at night in
    // the window's tint (screenGlow of the cloth's colour, frostGlow of the tint). Shops show their picture flat (a
    // lit backdrop at the back wall, no parallax), or on a kerbed street with shop holes, the modelled interior.
    float roomDepth = 1.5f;        // m
    float roomShare = 1.0f / 6.0f; // the master's RoomShare
    float frostShare = 0.3f;       // the master's FrostShare
    float screenGlow = 0.8f;       // the master's ScreenGlow
    float frostGlow = 0.8f;        // a lit frosted window: the tint times this, times GlowScale
    float shopDepth = 1.5f;        // m: a modelled shop interior's depth (at most shopDepthShare of the box's depth)
    float shopDepthShare = 0.4f;
    // Round 11 (night glare): every night light of the facade is soft-capped (windowGlowSoftCap), so lit glass keeps
    // its tint and what is behind it and never clips. Luminances in the engine's emissive units (cd/m2). Upper windows
    // are lit in clusters (litCluster); a lit shop is full or dim, in one of kWindowShopTintRgb.
    float glowScale = 0.5f;        // the master's GlowScale: a lit room's light at night
    float glowCap = 0.2f;          // the master's GlowCap: no upper window's light passes this
    float shopGlow = 0.6f;         // the master's ShopGlow (and the modelled interiors')
    float shopGlowCap = 0.3f;      // the master's ShopGlowCap: no shop glass's light passes this
    float signGlowCap = 0.35f;     // the master's SignGlowCap: no lit sign's
    float lanternNight = 1.1f;     // a lantern's glow by night over its 0.4 by day
    float shopDay = 0.15f;         // the master's ShopDay: a shop's display case by day (its own light: under a sunlit
                                   // wall's and readable in a shaded lane, where the exposure rises; 0.6 was a white pane)
    float shopCaseNight = 0.4f;    // a display case by night: this share of ShopGlow (lit shelves, not a white pane)
    // Round 12: a shop's glass shows a display case drawn in the shader (no photo, no modelled cubes): a box this deep
    // behind the glass in the wall's own frame, seen by the true view ray times the collection's Parallax.
    float shopCaseDepth = 0.45f;     // m
    float shopCaseShelf = 0.36f;     // m between its shelf boards (a shop kind adds up to 0.08)
    float shopCaseCabinet = 0.35f;   // m: the cabinet along the back wall's foot
    float shopCaseHead = 0.3f;       // m: the plain strip at the back wall's head
    float shopCaseBack = 0.7f;       // the light at the case's back as a share of that at the glass
    float shopCaseHeadShade = 0.4f;  // the case darkens toward its head by this share
    float revealSpill = 0.35f;     // a lit window's light on its own jambs, bottom and head, as a share of GlowScale
    float shopModelDay = 0.7f;     // the modelled interiors' ShopDay: a piece's own light by day (it stands in a shut box)
    float shopNightFloor = 0.02f;  // the modelled interiors' NightFloor: an unlit shop's pieces at night
    float shopDimShare = 0.31f;    // the share of lit shops that are dim (about a quarter of all at ShopLit 0.8)
    float shopDimLevel = 0.3f;     // a dim shop's light as a share of a full one's
    float litCluster = 0.6f;       // a 3 bay x 2 storey cluster's lit share: the storey's times 1 -/+ this, by its hash
    float shopModelShare = 0.5f;   // the share of kerbed buildings whose shops are modelled (the rest keep the picture)
};

// ---- Round 11: capped night light, shop lights, lit clusters ----

// The factor a light of luminance `lum` is scaled by so it never passes `cap`: 1 / (1 + lum / cap). Small lights keep
// nearly all of theirs; colour ratios (the tint) are kept at any level. No allocation.
FTCORE_API float windowGlowSoftCap(float lum, float cap);

// The shops' lights: warm whites, a neutral, a yellow, cool whites and, one in eight each, a green and a pink.
inline constexpr std::uint32_t kWindowShopTints = 8;
inline constexpr std::array<std::array<float, 3>, kWindowShopTints> kWindowShopTintRgb{{
    {1.0f, 0.86f, 0.68f}, {1.0f, 0.80f, 0.58f},   // warm white
    {1.0f, 0.96f, 0.90f},                           // neutral
    {1.0f, 0.88f, 0.45f},                           // yellow
    {0.82f, 0.92f, 1.0f}, {0.86f, 0.95f, 1.0f},   // cool
    {0.62f, 1.0f, 0.72f},                           // green
    {1.0f, 0.62f, 0.80f},                           // pink
}};
// The HLSL line declaring float3 shopTints[kWindowShopTints] (the window master and the modelled interiors' master).
// Allocates: build time only.
FTCORE_API std::string windowShopTintTableHlsl();

// A shop bay's light at night: lit (windowShopLit), its level (1, or shopDimLevel on shopDimShare of the lit ones) and
// its tint, from salted hashes of the bay (as the HLSL's). No allocation.
struct WindowShopLight {
    bool lit = false;
    float level = 1.0f;
    std::uint32_t tint = 0;  // kWindowShopTintRgb index
};
FTCORE_API WindowShopLight windowShopLight(const WindowFacadeSpec& spec, std::uint32_t seed, std::uint32_t column,
                                           float shopLit);
// The light as one float for an instance's custom data: 0 unlit, else 1 + tint, plus 0.5 when dim. The modelled
// interiors' master reads it back (windowShopModelGlowHlsl). No allocation.
FTCORE_API float windowShopLightCode(const WindowShopLight& light);
// The modelled interiors' master's emissive (a Custom node's body). Inputs: Color (the piece's albedo), Code (the
// custom data above), Night, NightGlow, ShopGlow, ShopGlowCap, DimLevel, ShopDay, NightFloor. By day Color * ShopDay (a
// fixed interior ambient: the pieces stand in a shut box, so the sky never reaches them); by night the lit bay's pieces
// in its tint at its level, soft-capped, over Color * NightFloor. Allocates: build time only.
FTCORE_API std::string windowShopModelGlowHlsl();

// The factor on a storey's lit share for the cell's cluster (3 bays x 2 storeys): 1 - litCluster .. 1 + litCluster by
// the cluster's hash, so lit windows gather in groups and dark patches stay between them. No allocation.
FTCORE_API float windowLitCluster(const WindowFacadeSpec& spec, std::uint32_t seed, std::uint32_t storey,
                                  std::uint32_t column);

// Whether a kerbed building's shops are modelled (FTBuildShopInteriors) or keep the master's flat picture: `share` of
// the buildings, by the seed. No allocation.
FTCORE_API bool windowShopModelled(std::uint32_t seed, float share);

// ---- Round 10: rooms fixed in the wall, covered windows, shop interiors ----

// What a non-shop window shows: a room (true parallax), a curtain or blind drawn all the way, or frosted glass. Shops
// are always Room (their own front and picture). From a salted hash of the cell (as the HLSL's), so a room never
// changes with the view. No allocation.
enum class WindowCover : std::uint8_t { Room = 0, Screen = 1, Frosted = 2 };
FTCORE_API WindowCover windowCoverFor(const WindowFacadeSpec& spec, std::uint32_t seed, std::uint32_t storey,
                                      std::uint32_t column, bool shop, float roomShare, float frostShare);

// The room a view ray meets through a glass point (gu, gv) of a cell (bayWidth x storeyHeight m): a box the cell's
// size, `depth` metres deep, fixed in the wall's frame; the ray is the true one (dAlong, dUp across and up the wall,
// dIn > 0 into it) times `parallax` (the master's Parallax switch: 1 true parallax, 0 a flat picture). A point of the
// room maps to one picture point whatever the eye's place. No allocation.
FTCORE_API WindowRoomHit windowParallaxRoomHit(float depth, float bayWidth, float storeyHeight, float gu, float gv,
                                               float dAlong, float dUp, float dIn, float parallax);

// Round 12: where a view ray ends in a shop's display case, a box width x height m behind the glass and `depth` deep,
// fixed in the wall's frame: the ray enters at (x, y) m from the opening's lower corner, the true ray (dAlong, dUp
// across and up the wall, dIn > 0 into it) times `parallax`. face 0 the back wall, 1 the floor, 2 the soffit, 3 and 4
// the sides at x = 0 and x = width; (x, y) the point across and up, `in` how deep (m). At parallax 0, always the back
// wall straight behind the entry point: a flat picture. The master's shop glass draws the same. No allocation.
struct WindowCaseHit {
    int face = 0;
    float x = 0.0f, y = 0.0f, in = 0.0f;
};
FTCORE_API WindowCaseHit windowShopCaseHit(float depth, float width, float height, float x, float y, float dAlong,
                                           float dUp, float dIn, float parallax);

// Whether a shop bay is lit at night (the master's lit hash for storey 0 at ShopLit). No allocation.
FTCORE_API bool windowShopLit(std::uint32_t seed, std::uint32_t column, float shopLit);

// A modelled shop interior's piece behind a kerbed street face's shop glass (FTBuildShopInteriors lays them as
// instances): a box in the face's frame, metres. along: from the face's left end as the master counts it (the facade's
// Along); in: into the wall from its face; up: over the face's street floor.
enum class WindowShopPart : std::uint8_t { Shell = 0, Shelf = 1, Goods = 2, Counter = 3, Table = 4 };
struct WindowShopPiece {
    float along0 = 0.0f, along1 = 0.0f, in0 = 0.0f, in1 = 0.0f, up0 = 0.0f, up1 = 0.0f;
    WindowShopPart part = WindowShopPart::Shell;
    std::uint32_t colour = 0;  // windowShopColour index
    std::uint32_t column = 0;
    bool lit = false;          // the bay is lit at night (windowShopLit)
    float light = 0.0f;        // windowShopLightCode: the bay's light (a face-long floor or ceiling: its first lit bay's, dim)
};
inline constexpr std::uint32_t kWindowShopPiecesPerBay = 14;
inline constexpr std::uint32_t kWindowShopColourCount = 8;
struct WindowTint;
// Linear albedo of a piece's colour (0: the shell's pale wall, 1: the floor's, 2..: shelving, counters and goods).
FTCORE_API const WindowTint& windowShopColour(std::uint32_t index);
// The pieces behind every shop bay of a face faceWidth wide and faceHeight high over its street floor, on a box
// boxDepth deep across the face, of type `type` and seed `seed`: a shell (back, floor, ceiling and the walls between the
// bays) shopDepth deep (at most shopDepthShare of boxDepth) and, by the bay's shop kind (ShopFront.h), shelves with
// goods, a counter or a table. Written to out (at most capacity; kWindowShopPiecesPerBay a bay is enough); the count
// written is returned. None when the face cuts no shop storey or bay. No allocation.
// Round 11 (fewer instances): one floor and one ceiling a face, a back wall a bay (its own light), a wall at each bay
// line; a shelf row is one box of goods (no board under it); a bay behind a shutter drawn all the way keeps only its
// shell. 2 * bays + 3 shell pieces and at most 4 fittings a bay (round 10: 4 * bays + 1 and at most 7).
FTCORE_API std::size_t windowShopInterior(const WindowFacadeSpec& spec, WindowType type, std::uint32_t seed,
                                          float faceWidth, float faceHeight, float boxDepth, float shopLit,
                                          WindowShopPiece* out, std::size_t capacity);

// The faces (bit f, boxFace order as MassingWindowData: 0 +u, 1 +w, 2 -u, 3 -w) of `box` whose street bit is set in
// streetBits and whose street is kerbed: the street whose centreline passes nearest the face's middle (within its half
// width plus reach) is wider than sharedUpTo (a narrower one is a shared surface with no kerb). The massing build ORs
// these, shifted by 4, into StreetBits. No allocation.
FTCORE_API std::uint8_t windowKerbBits(const MassingBox& box, std::uint8_t streetBits,
                                       const std::vector<MassingStreet>& streets, double sharedUpTo, double reach);
inline constexpr double kWindowKerbReach = 4.0;  // m past a street's half width a face's middle may stand
// A shop piece as a plan-frame box (as the massing boxes: centre, size, rot with u out of the face, z) on face `face` of
// `box`, whose street floor is streetFloor m above the datum. No allocation.
FTCORE_API MassingBox windowShopPieceBox(const MassingBox& box, int face, double streetFloor, const WindowShopPiece& p);

// Window styles: each column of a building (the same up every storey) takes one, by a weight per type. Plain: the
// type's window; Mullion: with a mullion and a transom; Grille: behind a steel security grille at the wall's face;
// Loggia: a recessed balcony, the bay's width from floor to head, loggiaDepth deep, a rail or parapet at the face and
// glazed doors at the back (never on the ground storey, a shop or an office); Small: a small high frosted window (on a
// ribbon, the opening keeps its size and frosts its lower part).
enum class WindowStyle : std::uint8_t { Plain = 0, Mullion = 1, Grille = 2, Loggia = 3, Small = 4 };
inline constexpr std::uint32_t kWindowStyles = 5;
inline constexpr std::uint32_t kWindowStyleWeightTotal = 16;
// A type's weight for a style (out of kWindowStyleWeightTotal; Legacy: all Plain). No allocation.
FTCORE_API std::uint32_t windowStyleWeight(WindowType type, WindowStyle style);
// The style a column takes (its hash alone, by the weights), and the one a cell shows: Loggia falls back to Plain on
// storey 0, a shop or an office. No allocation.
FTCORE_API WindowStyle windowColumnStyle(WindowType type, std::uint32_t seed, std::uint32_t column);
FTCORE_API WindowStyle windowStyleFor(WindowType type, std::uint32_t seed, std::uint32_t column, std::uint32_t storey,
                                      bool shop);
// The opening a style cuts in a (non-shop) cell, from the type's (as windowOpening with windowGridFor's spec). No
// allocation.
FTCORE_API WindowOpening windowStyleOpening(const WindowFacadeSpec& spec, WindowType type, WindowStyle style,
                                            float bayWidth, float storeyHeight, float windowWidth);
// How deep a style's glass sits: the type's reveal, a loggia's loggiaDepth. No allocation.
FTCORE_API float windowStyleReveal(const WindowFacadeSpec& spec, WindowType type, WindowStyle style);
// The share of a storey's windows lit at night: litShare on storeys 0 and 1, rising to litShare * (1 + upperLit) on
// the top storey, at most litMax (Legacy: litShare everywhere). No allocation.
FTCORE_API float windowLitShare(const WindowFacadeSpec& spec, WindowType type, float litShare, float upperLit,
                                std::uint32_t storey, std::uint32_t storeys);

// The grid spec a type cuts (the base with the type's storey, bay, pier, sill, opening and width). No allocation.
FTCORE_API WindowGridSpec windowGridFor(const WindowFacadeSpec& spec, WindowType type);
// A building's window width: Legacy keeps materialWidth; a ribbon fills its nominal bay but for the mullion; else the
// type's width jittered by the seed. No allocation.
FTCORE_API float windowWidthFor(const WindowFacadeSpec& spec, WindowType type, std::uint32_t seed, float materialWidth);

// r5..r9's eased ray, no longer in the master (r10 uses windowParallaxRoomHit); kept for the comparison test.
// The view ray with the side clamp: |x| at most min(viewMaxX, min(u0, 1 - u1) + sideShare * (u1 - u0)), so a ray from
// inside the glass meets a side wall only on the last sideShare of it. No allocation.
FTCORE_API WindowRay windowFacadeRay(const WindowFacadeSpec& spec, const WindowOpening& o, float bayWidth,
                                     float storeyHeight, float dAlong, float dUp, float dIn);
// windowRoomHit with that ray. No allocation.
FTCORE_API WindowRoomHit windowFacadeRoomHit(const WindowFacadeSpec& spec, const WindowOpening& o, float bayWidth,
                                             float storeyHeight, float u, float v, float dAlong, float dUp, float dIn);

// What a pixel of an r5 face shows (the window master's part code, Win.a). 0..4 as WindowPart.
enum class WindowFacadePart : std::uint8_t {
    Wall = 0, Glass = 1, Frame = 2, Band = 3, Fascia = 4,
    JambPlus = 5,   // a reveal's side facing +along (the opening's left jamb)
    JambMinus = 6,  // facing -along (its right jamb)
    RevealBottom = 7, RevealHead = 8,
    Lip = 9,        // the sill lip under a window
    Shutter = 10,   // a roller shutter, or a solid panel
    Cloth = 11,     // an eatery's curtain panels
    Lantern = 12,   // an eatery's paper lantern (lit)
    Frosted = 13,   // frosted glass or a white machine front
    Porthole = 14,  // a laundromat's machine door
    Course = 15,    // the string course along the top of a band
    Awning = 16,    // a cafe's striped awning, over its fascia
    Grille = 17,    // a security grille's bars, at the wall's face
    Rail = 18,      // a loggia's rail and balusters, at the wall's face
    SillShadow = 20,  // the wall in the lip's shadow
};

// Through an opening u0..u1 x v0..v1 of a cell (bayWidth x storeyHeight metres) whose glass sits `reveal` metres in: the
// point (u, v) on the wall's plane, the view ray (dAlong, dUp, dIn > 0) carried to the glass plane, lands at (gu, gv).
// part: Wall when (u, v) is outside the opening; else Glass or Frame by (gu, gv) against the frame, or the reveal's
// surface the ray meets first (a jamb, its bottom or head) when (gu, gv) falls outside the opening. The shift is at most
// the opening's size each way. No allocation.
struct WindowRevealHit {
    WindowFacadePart part = WindowFacadePart::Wall;
    float gu = 0.0f, gv = 0.0f;
};
FTCORE_API WindowRevealHit windowReveal(float frame, float reveal, const WindowOpening& o, float bayWidth,
                                        float storeyHeight, float u, float v, float dAlong, float dUp, float dIn);

// Shopfront kinds, each with its room picture (an atlas index kWindowRooms..) and its own front.
enum class WindowShopKind : std::uint8_t { Glazed = 0, Cafe, Shutter, Eatery, Clinic, Laundromat };
inline constexpr std::uint32_t kWindowShopKinds = 6;
// The kind a shop bay's hash alone picks, and the one it shows: on an even column that one, on an odd column stepped
// on (twice at most) past its even neighbours', so no two bays side by side share a kind. No allocation.
FTCORE_API std::uint32_t windowShopRawKind(std::uint32_t seed, std::uint32_t column);
FTCORE_API WindowShopKind windowShopKind(std::uint32_t seed, std::uint32_t column);
// The shop picture a kind shows: glazed the shelves or the stationery by the hash, cafe and eatery the cafe, clinic the
// waiting room, laundromat the stationery (machines in front), shutter the shelves. No allocation.
FTCORE_API std::uint32_t windowShopPicture(WindowShopKind kind, std::uint32_t seed, std::uint32_t column);

// Colours (linear rgb).
struct WindowTint {
    float r = 1.0f, g = 1.0f, b = 1.0f;
    float strength = 0.0f;  // how far the wall's albedo is pulled to the colour (times its own grain)
    bool pastel = false;    // only plain finishes (render, board concrete, breeze block) take it
};
inline constexpr std::uint32_t kWindowWallTintCount = 10;
FTCORE_API const WindowTint& windowWallTint(std::uint32_t index);  // 0: the photographed wall as it is
inline constexpr std::uint32_t kWindowRoofFinishCount = 6;           // as kMassingRoofFinishes
FTCORE_API const WindowTint& windowRoofFinish(std::uint32_t index);
inline constexpr std::uint32_t kWindowBandColorCount = 6;
inline constexpr std::uint32_t kWindowFasciaColorCount = 8;
inline constexpr std::uint32_t kWindowClothColorCount = 4;

// A building's wall tint by its seed: a plain finish takes any of the ten; a textured one (brick, tile, cladding) only
// the neutral ones (not pastel). No allocation.
FTCORE_API std::uint32_t windowWallTintFor(std::uint32_t seed, bool plainFinish);

// The r5 window master's Custom node bodies. windowFacadeFaceHlsl: the face (as r4's, plus outputs TOut and NzOut, the
// face's along and up axes in world space, TopOut, 1 on the top face, and KerbOut, bit 4 + face of StreetBits: the face
// fronts a kerbed street). windowFacadeHlsl: WindowInterior's inputs plus Type (the WindowType as a float), UpperLit,
// SignGlow, Kerb, Parallax (the collection's switch), RoomShare, FrostShare and ScreenGlow; returns float4 (rgb: the room
// seen, or a part's own albedo for Band, Fascia and parts 10 on; a: the part) and writes Curtain, Emit (the glass's
// light, a lantern's and a lit sign's), Hole (1 on a kerbed street face's shop glass: the opacity mask's cut), Pane (the
// glass point's pane, cut at a mullion and a transom: uv across and up it, its width and height in m) and PaneAge (its
// seed, its age: a shop's glass 0.15, a home's 0.3 to 0.8).
// windowFacadeComposeHlsl: r4's compose plus inputs T, Nz, Top, Tint and RoofFin; it tints the wall, shades the reveal
// with its own normals and colours the roof. With `glass` (ftcore/GlassSurface.h: glass over a picture) it reads CamV
// too and writes SpecOut: the room's light reaches the eye times the pane's pass share at the view's angle (as a share
// of a square-on view's, so a window seen square-on keeps its light) and its faint green; the surface's specular is
// GlassSpec (0.5 mirrors as one glass face does, as before; 0.96 as a pane's two faces). It also lays the glass
// function's detail layer (glassHlsl) on the pane from inputs Pane, PaneAge and Wet: the dust film toward the bottom edge
// and corners, dried streaks, the dark pane edge and the rubber seal, beads when wet. Allocates: build time only.
FTCORE_API std::string windowFacadeFaceHlsl();
FTCORE_API std::string windowFacadeHlsl(const WindowFacadeSpec& spec);
FTCORE_API std::string windowFacadeComposeHlsl(bool glass = false);

}  // namespace ftcore
