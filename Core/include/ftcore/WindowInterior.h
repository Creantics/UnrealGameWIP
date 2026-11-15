#pragma once

// Interior-mapped windows: rooms that read as 3D behind a building's flat walls, with no modelled interiors.
//
// A wall face is cut into storeys and bays from its own size: as many storeys as it has storey heights (each between
// storeyMin and storeyMax), as many bays as it has bay widths, so every storey x bay cell has one opening. The opening is
// a window of one width in metres (a material scalar per building finish, so one building never mixes window sizes),
// and on the ground storey of a face that fronts a street a glazed shopfront under a fascia, in a darker street band.
// Looking through the opening, a pixel's view ray runs into a box one room deep. Where the ray meets the box's back,
// floor or a side wall is projected onto one picture of the room (the room as a camera standing one room depth in front
// of the opening sees it: the back wall fills the middle half). The ray is eased first, so an eye far below or to the
// side still looks into the furnished middle of the picture and never at the ceiling. A room atlas holds the pictures,
// and salted hashes of the opening's cell pick the picture (never the same as the four neighbours'), a mirror flip,
// whether it is lit at night, a light tint, and a curtain or blind drawn some way down.
//
// The same maths runs on the GPU: windowInteriorHlsl writes the material's HLSL from these functions and constants, so
// the two can't drift apart. Plain C++20, float maths like the GPU's. No allocation except windowInteriorHlsl (build
// time only).

#include "ftcore/Api.h"

#include <array>
#include <cstdint>
#include <string>

namespace ftcore {

// How a wall face is cut into openings, how the eye looks into a room and how far curtains are drawn. Metres; "along"
// runs across the face, "up" from the face's street floor.
struct WindowGridSpec {
    float storey = 3.0f;        // the storey height a face aims for
    float storeyMin = 2.4f;     // the lowest storey a face may cut
    float storeyMax = 3.8f;     // the highest (a lower face keeps one storey of this and a parapet over it)
    float bay = 2.5f;           // the bay width a face aims for
    float edge = 0.35f;         // the wall kept at each end of a face
    float pier = 0.25f;         // the least wall between a window and its bay's side
    float windowWidth = 1.35f;  // the window width when the material sets none
    float sill = 0.85f;         // a window's bottom over its storey's floor
    float opening = 1.45f;      // a window's height
    float shopSill = 0.15f;     // a shopfront's bottom over the street floor
    float shopTop = 2.55f;      // a shopfront's top (lower when the storey can't fit the fascia over it)
    float shopFill = 0.88f;     // a shopfront's width as a share of its bay
    float fascia = 0.35f;       // the sign band over the shopfronts, across the whole face
    float frame = 0.06f;        // the frame's width round an opening
    float roomDepth = 4.0f;     // how deep each room reads
    float curtainDepth = 0.1f;  // how far inside the glass a curtain or blind hangs
    // The view ray's slopes in room units (across per room depth by the bay's width, up per room depth by the storey's
    // height) are scaled and clamped, and the vertical one tilted down, before the ray enters the room.
    float viewSquashX = 0.75f;
    float viewMaxX = 0.6f;
    float viewSquashY = 0.35f;
    float viewMaxY = 0.25f;
    float viewBiasY = -0.25f;
    // A curtained window is drawn all the way on curtainFull of them, else drawMin + drawSpan * r * r of the way down
    // (r even over 0..1).
    float curtainFull = 0.06f;
    float drawMin = 0.1f;
    float drawSpan = 0.6f;
};

// The room atlas: kWindowAtlasCols x kWindowAtlasRows pictures, row 0 at the top. The first kWindowRooms are upper-floor
// rooms, the kWindowShops after them shops. The curtain atlas holds kWindowCurtains pictures in a row; kWindowTints light
// tints for lit rooms.
inline constexpr std::uint32_t kWindowRooms = 8;
inline constexpr std::uint32_t kWindowShops = 4;
inline constexpr std::uint32_t kWindowAtlasCols = 4;
inline constexpr std::uint32_t kWindowAtlasRows = 3;
inline constexpr std::uint32_t kWindowCurtains = 6;
inline constexpr std::uint32_t kWindowTints = 8;
// The lit rooms' light, one per tint: warm incandescent, neutral and cool fluorescent or LED whites (at least two in
// five neutral or cool, so a lit block at night reads as many homes, not one sodium wash).
inline constexpr std::array<std::array<float, 3>, kWindowTints> kWindowTintRgb{{
    {1.0f, 0.70f, 0.45f}, {1.0f, 0.80f, 0.56f}, {1.0f, 0.90f, 0.76f},   // warm
    {1.0f, 0.96f, 0.90f}, {0.96f, 0.97f, 1.0f},                          // neutral
    {0.84f, 0.92f, 1.0f}, {0.78f, 0.90f, 1.0f}, {0.80f, 0.96f, 1.0f},   // cool
}};
static_assert(kWindowRooms + kWindowShops == kWindowAtlasCols * kWindowAtlasRows);
// How many times an opening steps its room on past a neighbour's: enough for four neighbours with four different rooms.
inline constexpr std::uint32_t kWindowAvoidSteps = 4;
static_assert(kWindowShops > 2 && kWindowRooms > kWindowAvoidSteps);

// Seeds pass to the GPU as floats holding a whole number, so they keep to the 24 bits a float holds exactly.
inline constexpr std::uint32_t kWindowSeedMask = 0xFFFFFFu;

// What a pixel of a face shows. Band is the wall of a street face's ground storey, Fascia the sign strip over its
// shopfronts.
enum class WindowPart : std::uint8_t { Wall = 0, Glass = 1, Frame = 2, Band = 3, Fascia = 4 };

// A 32-bit integer hash, the same on the GPU (unsigned wrap-around maths). No allocation.
FTCORE_API std::uint32_t windowHash(std::uint32_t x);
// One opening's hash: a building's seed with its storey and bay. No allocation.
FTCORE_API std::uint32_t windowCellHash(std::uint32_t seed, std::uint32_t storey, std::uint32_t bay);
// A building's seed from any number (its id's hash), kept to kWindowSeedMask. No allocation.
FTCORE_API std::uint32_t windowSeed(std::uint32_t any);

// A face's grid: storeys x bays cells of storeyHeight x bayWidth, the bays starting spec.edge in from the face's left
// end, the storeys at its street floor. No storeys when the face is lower than storeyMin; no bays when it is too
// narrow for one window and its piers. No allocation.
struct WindowFaceGrid {
    std::uint32_t storeys = 0, bays = 0;
    float storeyHeight = 0.0f, bayWidth = 0.0f;
};
FTCORE_API WindowFaceGrid windowFaceGrid(const WindowGridSpec& spec, float faceWidth, float faceHeight,
                                         float windowWidth);

// Where a point on a face sits in the grid.
struct WindowCell {
    bool bay = false;   // the point lies in a storey x bay cell
    bool shop = false;  // the cell is a shop (the ground storey of a face that fronts a street)
    bool band = false;  // the point lies in a street face's ground storey
    WindowPart part = WindowPart::Wall;
    std::uint32_t storey = 0, column = 0;
    float u = 0.0f, v = 0.0f;  // the point across the cell: 0..1 left to right, 0..1 floor to ceiling
    float bayWidth = 0.0f, storeyHeight = 0.0f;
};

// The cell at (along, up) on a face faceWidth wide and faceHeight high (over its street floor); street: the face fronts
// a street, so its ground storey is the shop band. windowWidth: the building's window width in metres. No allocation.
FTCORE_API WindowCell windowCellAt(const WindowGridSpec& spec, float faceWidth, float faceHeight, float along, float up,
                                   bool street, float windowWidth);

// An opening's rectangle across its cell (u0..u1, v0..v1, same units as WindowCell::u and ::v).
struct WindowOpening {
    float u0 = 0.0f, u1 = 0.0f, v0 = 0.0f, v1 = 0.0f;
};
FTCORE_API WindowOpening windowOpening(const WindowGridSpec& spec, bool shop, float bayWidth, float storeyHeight,
                                       float windowWidth);

// What one opening shows.
struct WindowLook {
    std::uint32_t room = 0;  // picture in the room atlas: 0..kWindowRooms-1, or a shop's kWindowRooms.. after them
    bool mirror = false;     // the picture flipped left to right
    std::uint32_t tint = 0;  // light tint when lit, 0..kWindowTints-1
    bool lit = false;        // lit at night
    int curtain = -1;        // picture in the curtain atlas, or -1 for none (shops have none)
    float drawn = 0.0f;      // how far down from the top the curtain is drawn, 0..1
};

// The room picture an opening's hash alone would pick (no allocation), and the one it shows: on a cell whose storey
// plus column is even, that one; on an odd cell, stepped on (up to kWindowAvoidSteps times) past the even neighbours'
// above, below, left and right, so no two openings side by side or one over the other show the same room. The
// neighbours above and below are taken as upper storeys (a shop's pictures never match an upper room's). No allocation.
FTCORE_API std::uint32_t windowRawRoom(std::uint32_t seed, std::uint32_t storey, std::uint32_t column, bool shop);
FTCORE_API std::uint32_t windowRoomFor(std::uint32_t seed, std::uint32_t storey, std::uint32_t column, bool shop);

// The look of the opening in (storey, column) of a building with this seed. litShare and curtainShare are the shares
// of windows lit at night and with a curtain or blind; shopLit the share of shops lit at night. Each field has its own
// salted hash of the cell. No allocation.
FTCORE_API WindowLook windowLookFor(const WindowGridSpec& spec, std::uint32_t seed, std::uint32_t storey,
                                    std::uint32_t column, bool shop, float litShare, float curtainShare, float shopLit);

// The eased view ray in room units per room depth (WindowGridSpec's view numbers). No allocation.
struct WindowRay {
    float x = 0.0f, y = 0.0f;
};
FTCORE_API WindowRay windowRay(const WindowGridSpec& spec, float bayWidth, float storeyHeight, float dAlong, float dUp,
                               float dIn);

// Where a view ray through the opening lands in the room picture. (u, v) is the point across the cell; the direction
// is the view ray in metres: dAlong across the face, dUp upwards, dIn into the wall (> 0; smaller counts as grazing).
// Returns the picture point, 0..1 left to right and 0..1 bottom to top (the GPU flips v to texture order), and which
// face the ray met: 0 back, 1 floor, 2 ceiling, 3 left, 4 right. No allocation.
struct WindowRoomHit {
    float x = 0.0f, y = 0.0f;
    int face = 0;
};
FTCORE_API WindowRoomHit windowRoomHit(const WindowGridSpec& spec, float bayWidth, float storeyHeight, float u, float v,
                                       float dAlong, float dUp, float dIn);

// Where the same ray crosses the curtain plane, as a point across the window (0..1 each way, bottom to top), and
// whether the curtain covers it (a curtain drawn d covers the top d of the window). No allocation.
struct WindowCurtainHit {
    float x = 0.0f, y = 0.0f;
    bool covered = false;
};
FTCORE_API WindowCurtainHit windowCurtainHit(const WindowGridSpec& spec, float bayWidth, float storeyHeight,
                                             float windowWidth, float u, float v, float dAlong, float dUp, float dIn,
                                             float drawn);

// The material's HLSL (a Custom node's body) for these numbers. Inputs it reads: Along, Up, FaceW, FaceH (metres on
// the face), Seed (the building's seed as a whole-number float), Street (1 when the face fronts a street), ViewT (the
// view direction in the face's frame: x across, y up, z into the wall), WinW (the window width, metres), LitShare,
// CurtainShare, ShopLit, Night (0 day .. 1 night), RoomDay and ShopDay (a room's and a shop's own light by day),
// GlowScale and ShopGlow (a lit room's and shop's light at night), CurtainAlpha (a curtain's most opacity, under 1 so
// the room always shows), CurtainGlow (the share of the room's light a curtain passes at night), Rooms and Curtains
// (the two atlases, with the samplers UE gives them). It returns float4: rgb the room seen past the curtain, a the part
// (WindowPart, 0..4); it writes the curtain's colour and opacity to the out variable Curtain (float4) and the opening's
// own light to Emit (float3): the room's by day, fading out with Night; by night the lit room's and, weaker, its
// curtain's, never a curtain's on its own. Its texture reads run on every pixel, outside any branch, so their mips stay
// right. Allocates: build time only.
FTCORE_API std::string windowInteriorHlsl(const WindowGridSpec& spec);
// The HLSL line declaring float3 tints[kWindowTints] from kWindowTintRgb (both window masters use it). Allocates.
FTCORE_API std::string windowTintTableHlsl();

}  // namespace ftcore
