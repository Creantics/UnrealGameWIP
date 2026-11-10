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
#include "ftcore/WindowInterior.h"

#include <cstdint>
#include <string>

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
    float shopReveal = 0.08f;     // m: a shopfront's glass in from the wall
    float headShadow = 0.4f;      // m: by day the glass darkens toward its head over this height (the reveal's shade)
    float loggiaDepth = 1.1f;     // m: a recessed balcony's glass in from the wall
    float railHeight = 1.0f;      // m: its rail or parapet over its floor
    float baluster = 0.11f;       // m between its balusters
    float smallWidth = 0.6f, smallHeight = 0.6f;  // m: a small frosted window, at the head of the type's opening
    float grillePitch = 0.15f;    // m between a security grille's uprights (its rails twice that)
    float upperLit = 1.0f;        // the top storey's lit share is LitShare * (1 + upperLit), the first storey's LitShare
    float litMax = 0.9f;          // the most any storey's lit share reaches
    float wallMax = 0.75f;        // the brightest a tinted wall's or roof's albedo gets (no glare under night lights)
};

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
// face's along and up axes in world space, and TopOut, 1 on the top face). windowFacadeHlsl: WindowInterior's inputs
// plus Type (the WindowType as a float); returns float4 (rgb: the room seen, or a part's own albedo for Band, Fascia
// and parts 10 on; a: the part) and writes Curtain and Emit (the glass's light, a lantern's and a lit sign's).
// windowFacadeComposeHlsl: r4's compose plus inputs T, Nz, Top, Tint and RoofFin; it tints the wall, shades the reveal
// with its own normals and colours the roof. Allocates: build time only.
FTCORE_API std::string windowFacadeFaceHlsl();
FTCORE_API std::string windowFacadeHlsl(const WindowFacadeSpec& spec);
FTCORE_API std::string windowFacadeComposeHlsl();

}  // namespace ftcore
