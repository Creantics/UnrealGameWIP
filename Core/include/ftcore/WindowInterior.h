#pragma once

// Interior-mapped windows: rooms that read as 3D behind a building's flat walls, with no modelled interiors.
//
// A wall face is cut into storeys and bays (a bay is one room's width). Each bay has one window opening. Looking through
// the opening, a pixel's view ray runs into a box one room deep. Where the ray meets the box's back, floor, ceiling or a
// side wall is projected onto one picture of the room (the room as a camera standing one room depth in front of the
// window sees it: the back wall fills the middle half). A room atlas holds several such pictures, and a hash of the
// window's cell picks the picture, a mirror flip, whether it is lit at night, a light tint, and a curtain or blind.
//
// The same maths runs on the GPU: windowInteriorHlsl writes the material's HLSL from these functions and constants, so
// the two can't drift apart. Plain C++20, float maths like the GPU's. No allocation except windowInteriorHlsl (build
// time only).

#include "ftcore/Api.h"

#include <cstdint>
#include <string>

namespace ftcore {

// How a wall face is cut into windows. Metres; "along" runs across the face, "up" from the building's base.
struct WindowGridSpec {
    float storey = 3.0f;        // storey height
    float bayMin = 2.6f;        // the narrowest bay a building may pick
    float bayMax = 3.2f;        // the widest
    float edge = 0.6f;          // the least wall kept at each end of a face
    float sill = 0.9f;          // the opening's bottom over its storey's floor
    float opening = 1.5f;       // the opening's height
    float fill = 0.55f;         // the opening's width as a share of its bay
    float groundBand = 0.0f;    // no windows in a storey whose opening starts under this height (shop fronts there)
    float roomDepth = 4.0f;     // how deep each room reads
    float curtainDepth = 0.1f;  // how far inside the glass a curtain or blind hangs
};

// The room atlas: kWindowRooms room pictures, kWindowAtlasCols across and kWindowAtlasRows down; the curtain atlas holds
// kWindowCurtains pictures in a row; kWindowTints light tints for lit rooms.
inline constexpr std::uint32_t kWindowRooms = 8;
inline constexpr std::uint32_t kWindowAtlasCols = 4;
inline constexpr std::uint32_t kWindowAtlasRows = 2;
inline constexpr std::uint32_t kWindowCurtains = 6;
inline constexpr std::uint32_t kWindowTints = 4;

// A 32-bit integer hash, the same on the GPU (unsigned wrap-around maths). No allocation.
FTCORE_API std::uint32_t windowHash(std::uint32_t x);
// One window cell's hash: a building's seed with its storey and bay. No allocation.
FTCORE_API std::uint32_t windowCellHash(std::uint32_t seed, std::uint32_t storey, std::uint32_t bay);

// The bay width a building with this seed uses on all its faces: one of seven steps from bayMin to bayMax.
FTCORE_API float windowBayWidth(const WindowGridSpec& spec, std::uint32_t seed);

// Where a point on a face sits in the grid.
struct WindowCell {
    bool bay = false;      // the point lies in a bay with a room (inside the edges, a whole storey, over the ground band)
    bool opening = false;  // ...and inside that bay's window opening
    std::uint32_t storey = 0, column = 0;
    float u = 0.0f, v = 0.0f;  // the point across the bay's front: 0..1 left to right, 0..1 floor to ceiling
    float bayWidth = 0.0f;
};

// The cell at (along, up) on a face faceWidth wide and faceHeight high. The bays are centred on the face, so no face
// ends in a cut window; a storey that would rise past the face's top has no windows. No allocation.
FTCORE_API WindowCell windowCellAt(const WindowGridSpec& spec, std::uint32_t seed, float faceWidth, float faceHeight,
                                   float along, float up);

// The opening's rectangle across the bay's front (u0..u1, v0..v1, same units as WindowCell::u and ::v).
struct WindowOpening {
    float u0 = 0.0f, u1 = 0.0f, v0 = 0.0f, v1 = 0.0f;
};
FTCORE_API WindowOpening windowOpening(const WindowGridSpec& spec, float bayWidth);

// What one window shows.
struct WindowLook {
    std::uint32_t room = 0;  // picture in the room atlas, 0..kWindowRooms-1
    bool mirror = false;     // the picture flipped left to right
    std::uint32_t tint = 0;  // light tint when lit, 0..kWindowTints-1
    bool lit = false;        // lit at night
    int curtain = -1;        // picture in the curtain atlas, or -1 for none
    float drawn = 0.0f;      // how far down from the top the curtain is drawn, 0.25..1
};

// The look of the window in (storey, column) of a building with this seed. litShare and curtainShare are the share of
// windows lit at night and the share with a curtain or blind. No allocation.
FTCORE_API WindowLook windowLookFor(std::uint32_t seed, std::uint32_t storey, std::uint32_t column, float litShare,
                                    float curtainShare);

// Where a view ray through the opening lands in the room picture. (u, v) is the point across the bay's front; the
// direction is the view ray in metres: dAlong across the face, dUp upwards, dIn into the wall (must be > 0). Returns
// the picture point, 0..1 left to right and 0..1 bottom to top (the GPU flips v to texture order), and which face the
// ray met: 0 back, 1 floor, 2 ceiling, 3 left, 4 right. No allocation.
struct WindowRoomHit {
    float x = 0.0f, y = 0.0f;
    int face = 0;
};
FTCORE_API WindowRoomHit windowRoomHit(const WindowGridSpec& spec, float bayWidth, float u, float v, float dAlong,
                                       float dUp, float dIn);

// Where the same ray crosses the curtain plane, as a point across the opening (0..1 each way, bottom to top), and
// whether the curtain covers it (a curtain drawn d covers the top d of the opening). No allocation.
struct WindowCurtainHit {
    float x = 0.0f, y = 0.0f;
    bool covered = false;
};
FTCORE_API WindowCurtainHit windowCurtainHit(const WindowGridSpec& spec, float bayWidth, float u, float v,
                                             float dAlong, float dUp, float dIn, float drawn);

// The material's HLSL (a Custom node's body) for these numbers. Inputs it reads: Along, Up, FaceW, FaceH (metres on
// the face), Seed (a float holding the building seed's bits), ViewT (the view direction in the face's frame: x across,
// y up, z into the wall), LitShare, CurtainShare, Night (0 day .. 1 night), GlowScale (a lit room's brightness),
// Rooms and Curtains (the two atlases), and
// the samplers UE gives them. It returns float4: rgb the window's colour (room through glass, curtain over it), a = 1
// inside an opening, 0 on the wall; and it writes the night glow to the out variable Glow. Allocates: build time only.
FTCORE_API std::string windowInteriorHlsl(const WindowGridSpec& spec);

}  // namespace ftcore
