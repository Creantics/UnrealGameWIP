#pragma once

// Far-field sign cards: tall lit vertical signs on the faces of the blocks beyond a street, turned toward it, so a night
// street has coloured accents in its distance (and in its wet ground). Plain C++; the Unreal side gives the blocks'
// plan boxes and places a thin lit box at each card.

#include "ftcore/Api.h"

#include <cstddef>

namespace ftcore {

// A block's footprint (metres, axis-aligned in plan) and its height range.
struct FarSignBlock {
    float minX = 0.f;
    float minY = 0.f;
    float maxX = 0.f;
    float maxY = 0.f;
    float baseZ = 0.f;
    float topZ = 0.f;
};

// A card: its centre (metres), the way its face looks (degrees like a yaw, 0 = +X), its size and a palette index.
struct FarSign {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float yawDeg = 0.f;
    float widthM = 0.f;
    float heightM = 0.f;
    int colour = 0;
};

struct FarSignSpec {
    float nearM = 20.f;     // blocks whose nearest face is closer than this to the centre get none
    float farM = 90.f;      // nor farther than this
    float minHeightM = 7.f; // nor lower than this (sheds, walls, kerbs)
    float maxFootM = 80.f;  // nor a footprint side longer than this (the ground, a terrain piece)
    float widthM = 0.9f;
    float heightM = 3.2f;
    float outM = 0.25f;     // the card's centre stands this far off the face
    float cornerM = 1.2f;   // and at least this far from the face's ends
};

// The palette: 0 red, 1 pink, 2 cyan; red and pink about twice as often as cyan.
inline constexpr int kFarSignColours = 3;
FTCORE_API int farSignColour(std::size_t index);

// Fills out[0..maxOut) with a card on the face of each eligible block nearest to (cx, cy), nearest blocks first;
// returns how many. A block containing the centre gets none. No allocation: blocks are scanned once per card slot.
FTCORE_API std::size_t placeFarSigns(const FarSignBlock* blocks, std::size_t count, float cx, float cy, const FarSignSpec& spec,
                          FarSign* out, std::size_t maxOut);

} // namespace ftcore
