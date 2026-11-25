#pragma once

// Rain under cover: where the rain can't reach, there are no streaks or splashes and the ground stays dry.
//
// A top-down orthographic depth capture around the camera (sizeM square, `resolution` texels a side, static geometry
// only) holds the highest occluder's Z at each texel: captureTopZ minus the captured depth. A point is under cover when
// the occluder over it stands more than biasM over it (the ground itself, or a box top the point lies on, reads as
// its own occluder and stays open).
//
// - The capture's centre snaps to whole texels (no shimmer as it moves) and is recaptured only when the camera has moved
//   recaptureM from it in plan, or on the first frame.
// - Frame: engine metres here (the HLSL works in cm). The capture looks straight down (pitch -90, yaw 0, roll 0): the
//   image's right (u) runs along +Y and its down (v) along -X: u = (y - cy) / size + 0.5, v = (cx - x) / size + 0.5.
//   Outside the square a point counts as open (rain falls, the ground soaks), as with no capture at all.
// - Streaks and splashes (particle sprites): killed where covered (alpha 0); a streak pixel is tested at its own Z.
// - Wetness: openness, with a fringe. A covered point within fringeM of open ground (on one of four sides: the rain
//   blows in) takes some wet: the nearest open sample's weight, 1.25 - r / fringeM, clamped 0..1, sampled at fringeM / 3,
//   2 fringeM / 3 and fringeM along +-X and +-Y (a slow dry-to-wet gradient over the fringe).
// - A wall's face reads the cover just out from it: its point moves wallOffsetM along the horizontal part of its normal
//   (a facade under its own roof edge stays wet below an open sky; one under an awning dry).
//
// The C++ functions mirror the HLSL one term each, for tests. Plain C++20; rainCoverHlsl allocates (build time only),
// the rest doesn't.

#include "ftcore/Api.h"

#include <string>

namespace ftcore {

// Material parameter collection names (Data/Look: the cover's own collection, MPC_RainCover).
inline constexpr const char* kRainCoverMpc = "MPC_RainCover";
inline constexpr const char* kRainCoverParamFrame = "CoverFrame";  // vector: centre x, y (cm), size (cm), top Z (cm)
inline constexpr const char* kRainCoverParamOn = "CoverOn";        // scalar: 1 with a capture, 0 none (all open)
// The capture's render target asset, sampled by every material that reads the cover.
inline constexpr const char* kRainCoverTarget = "/Game/Rendering/Rain/RT_RainCover";

// Off by default: only with -RainCover (kRainCoverSwitch) does the game run the capture and do the material and rain
// builds put the cover term in; -NoRainCover (kRainCoverOffSwitch) wins over it. Off, nothing is created and every
// material and rain sprite builds and draws as before the cover (an emitter's "cover" is ignored). The game and the
// builds both need the switch: a sprite built without the term isn't hidden by a capture.
inline constexpr const char* kRainCoverSwitch = "RainCover";
inline constexpr const char* kRainCoverOffSwitch = "NoRainCover";
FTCORE_API bool rainCoverEnabled(bool rainCoverGiven, bool noRainCoverGiven);
// Whether an emitter draws with a cover sprite material: its "cover" with the cover on (else its plain material).
FTCORE_API bool rainCoverEmitter(bool emitterCover, bool coverEnabled);

struct RainCoverSpec {
    float sizeM = 64.f;          // the capture square (the streak box is 30 m, the splashes 13 m: room for a 5 m drift)
    int resolution = 512;        // texels a side (12.5 cm at 64 m)
    float recaptureM = 5.f;      // camera plan move that triggers a recapture
    float aboveM = 60.f;         // the capture stands this far over the camera
    float depthM = 200.f;        // ... and reads this far down (far plane); deeper counts as open
    float biasM = 0.3f;          // an occluder must stand this far over a point to cover it
    float streakBiasM = 0.05f;   // ... over a streak or splash pixel
    float wallOffsetM = 0.4f;    // a wall face reads the cover this far out along its normal
    float fringeM = 0.75f;       // the wet fringe that blows in under cover at the open sides
};

struct RainCoverFrame {
    float cx = 0.f, cy = 0.f;  // centre (m)
    float sizeM = 0.f;
    float topZ = 0.f;          // the capture's own Z (m): occluder Z = topZ - depth
};

// The texel size (m); 0 for a spec with no size or resolution.
FTCORE_API float rainCoverTexel(const RainCoverSpec& spec);
// The centre for a camera at (x, y): snapped to whole texels.
FTCORE_API void rainCoverSnap(const RainCoverSpec& spec, float camX, float camY, float& cx, float& cy);
// Whether to recapture: none yet, or the camera has moved at least recaptureM in plan from the capture's centre.
FTCORE_API bool rainCoverRecapture(const RainCoverSpec& spec, bool haveCapture, float cx, float cy, float camX, float camY);
// The frame for a camera at (x, y, z) (snapped centre, top aboveM over it).
FTCORE_API RainCoverFrame rainCoverFrameAt(const RainCoverSpec& spec, float camX, float camY, float camZ);
// The capture's UV of a plan point (see the frame above); false outside the square.
FTCORE_API bool rainCoverUV(const RainCoverFrame& frame, float x, float y, float& u, float& v);
// Openness of a point at z under an occluder at occluderZ: 1 open, 0 covered (occluderZ > z + bias).
FTCORE_API float rainCoverOpen(float occluderZ, float z, float bias);
// The fringe weight of an open sample r m away: 1.25 - r / fringe, clamped 0..1 (1 with no fringe at r 0).
FTCORE_API float rainCoverFringeWeight(float r, float fringeM);
// The wet share 0..1 of a point from its own openness and the 12 ring samples' (rings fringe/3, 2/3, 1 in that order,
// each +X, -X, +Y, -Y): its own when open, else the best ring sample's openness times its weight.
FTCORE_API float rainCoverWet(float openHere, const float (&ring)[12], float fringeM);
// Where a pixel reads the cover: (x, y) moved wallOffsetM along the horizontal part of its normal (nx, ny, nz), by the
// horizontal share (a floor not at all, a wall fully).
FTCORE_API void rainCoverReadPoint(const RainCoverSpec& spec, float x, float y, float nx, float ny, float nz, float& rx, float& ry);

// Fixed cover tiles (path 3b, 11-25; the load-time probe only, nothing in play reads them yet). The strip is cut into
// squares of spec.sizeM on a grid from the origin: tile (tx, ty) runs tx * size .. (tx + 1) * size in x, and the same in
// y. Tile edges are whole texels, so a tile's texels lie on the same lattice as a snapped square's.
struct RainCoverTile {
    int x = 0, y = 0;
    bool operator==(const RainCoverTile&) const = default;
};
// The tile holding plan point (x, y): floor, so -0.01 m is tile -1. A non-finite point, one 1000 km out or more, or a spec
// with no size is (0, 0).
FTCORE_API RainCoverTile rainCoverTileAt(const RainCoverSpec& spec, float x, float y);
// A tile's frame: its centre, size, and the given top.
FTCORE_API RainCoverFrame rainCoverTileFrame(const RainCoverSpec& spec, RainCoverTile tile, float topZ);
// Where plan point (x, y) lands: its tile and texel (px along +Y, py along -X, as rainCoverUV, 0 .. resolution - 1).
// Exact on the texel lattice (it works in whole texels), so a snapped square's texel centre maps to one tile texel and
// a tile mosaic can be compared texel for texel. False for a non-finite point or a spec with no texel.
FTCORE_API bool rainCoverTileTexel(const RainCoverSpec& spec, float x, float y, RainCoverTile& tile, int& px, int& py);
// The resident 3 x 3 ring round the lead's tile: slot posmod(x, 3) + 3 * posmod(y, 3), 0..8. A tile keeps its slot for as
// long as it stays in the ring, so a move uploads only the tiles that enter it.
FTCORE_API int rainCoverRingSlot(RainCoverTile tile);
// The tiles of the ring round `to` that the ring round `from` lacks, in out[0 .. n-1]; returns n: 0 staying, 3 across an
// edge, 5 across a corner, 9 for a jump of two tiles or more.
FTCORE_API int rainCoverRingEntered(RainCoverTile from, RainCoverTile to, RainCoverTile (&out)[9]);

// A tile stored as 16-bit codes over its own height range. Code 0 is no occluder (the capture's far plane); codes
// 1..65535 run evenly from minZ to maxZ, rounded to the nearest, so a height is off by at most range / (2 * 65534):
// 2 mm up to a range of about 262 m. A range of 0 keeps every occluder at code 1.
inline constexpr unsigned kRainCoverR16None = 0u;
inline constexpr unsigned kRainCoverR16Top = 65535u;
struct RainCoverR16Range {
    float minZ = 0.f, maxZ = 0.f;
    int hits = 0;  // texels with an occluder
};
// The worst height error (m) of a range of rangeM; 0 for none or a non-finite range.
FTCORE_API float rainCoverR16Error(float rangeM);
// Packs `count` captured depths (m below topZ; at or past farM, or not finite, is no occluder) into out (count codes,
// the caller's buffer) and sets range. Returns whether the worst error is within toleranceM (2 mm in the probe).
FTCORE_API bool rainCoverR16Pack(const float* depthM, int count, float topZ, float farM, unsigned short* out,
                                 RainCoverR16Range& range, float toleranceM);
// A code back to an occluder Z (m); false for no occluder.
FTCORE_API bool rainCoverR16Unpack(unsigned code, const RainCoverR16Range& range, float& z);

// The HLSL: struct FTRainCover with Open(...) for sprites and Wet(...) for surfaces, as one custom node's code. Inputs
// the node declares: CoverTex (Texture2D), CoverTexSampler, CoverFrame (float4, cm), CoverOn (float), WorldPos (cm),
// Normal (world, unit). Kind "sprite" returns the openness, "surface" the wet share. The moving 60 m capture's term
// (dev only since the ring, 11-25; no shipped material carries it).
FTCORE_API std::string rainCoverHlsl(const RainCoverSpec& spec, bool sprite);

// The resident ring (path 3b, 11-25). The 3 x 3 tiles round the lead's tile live in one atlas of 3 * resolution texels a
// side, 16-bit codes (0 none, 1..65535 over the tile's own range). Ring slot (mx, my) = (posmod(tx, 3), posmod(ty, 3))
// holds its tile at atlas texels mx * res + px, my * res + py (px, py as rainCoverTileTexel). Nine slot vectors in the
// cover's collection (CoverSlot0..8) say which tile each slot holds and its range: (tx, ty, minZ cm, maxZ cm). A point
// whose slot holds another tile (not yet uploaded, or no tile: kRainCoverSlotNone) reads open.
inline constexpr float kRainCoverSlotNone = 1.0e6f;  // tile id no tile has
inline constexpr int kRainCoverRingSlots = 9;
// The slot vector names: CoverSlot0 .. CoverSlot8.
FTCORE_API const char* rainCoverSlotParam(int slot);
struct RainCoverSlot {
    float tx = kRainCoverSlotNone, ty = kRainCoverSlotNone;
    float minZcm = 0.f, maxZcm = 0.f;
};
// The slot vector for a tile and its range (m): ids as floats, the range in cm.
FTCORE_API RainCoverSlot rainCoverSlotFor(RainCoverTile tile, const RainCoverR16Range& range);
// The atlas side in texels (3 * resolution; 0 for a spec with no resolution).
FTCORE_API int rainCoverAtlasSide(const RainCoverSpec& spec);
// Where plan point (x, y) reads in the atlas: its tile, ring slot and atlas texel. False as rainCoverTileTexel.
FTCORE_API bool rainCoverRingTexel(const RainCoverSpec& spec, float x, float y, RainCoverTile& tile, int& slot, int& ax, int& ay);
// The atlas origin of a slot's block (texels).
FTCORE_API void rainCoverRingOrigin(const RainCoverSpec& spec, int slot, int& ax, int& ay);
// The CPU mirror of the ring term's OpenAt: the openness at plan point (x, y) for a point at z (m) over the atlas
// (rainCoverAtlasSide² codes, row by row) and the nine slots. 1 when the slot holds another tile, the code is none, or the
// occluder stands no more than bias over z.
FTCORE_API float rainCoverRingOpen(const RainCoverSpec& spec, const unsigned short* atlas, const RainCoverSlot (&slots)[9],
                                   float x, float y, float z, float bias);
// The ring term (HLSL), as rainCoverHlsl's kinds. Inputs: CoverTex (Texture2D, the G16 atlas), CoverOn, CoverSlot0..8
// (float4), WorldPos (cm), Normal (surface only). Off (CoverOn < 0.5) the whole body is skipped ([branch]) and every point
// reads open.
FTCORE_API std::string rainCoverRingHlsl(const RainCoverSpec& spec, bool sprite);

// A baked tile on disk (v2): a 4096-byte header (so the payload starts sector-aligned for unbuffered reads), then
// resolution² codes (u16, little-endian, row by row in image order) when the tile carries a payload. A tile with no
// occluder (hits 0), or flat and full (every texel hit, minZ == maxZ: every code 1), is the header alone (res 0).
inline constexpr int kRainCoverFileHeader = 4096;
inline constexpr unsigned kRainCoverFileVersion = 2u;
struct RainCoverTileHeader {
    RainCoverTile tile;
    int res = 0;      // payload side (0: header only)
    int hits = 0;     // texels with an occluder
    float topZ = 0.f, minZ = 0.f, maxZ = 0.f;  // m
    unsigned long long key = 0;  // the content key the tile was baked from
    RainCoverR16Range range() const { return RainCoverR16Range{minZ, maxZ, hits}; }
};
// Whether a packed tile of `count` texels needs no payload (no hits, or every texel hit at one height).
FTCORE_API bool rainCoverTileHeaderOnly(const RainCoverR16Range& range, int count);
// Writes the header into out (kRainCoverFileHeader bytes, zeros past the fields).
FTCORE_API void rainCoverHeaderWrite(const RainCoverTileHeader& h, unsigned char* out);
// Reads a header; false for a short buffer, another magic or version, or a res outside 0..4096.
FTCORE_API bool rainCoverHeaderRead(const unsigned char* in, int size, RainCoverTileHeader& h);
// The payload's bytes (res² * 2; 0 header only).
FTCORE_API int rainCoverPayloadBytes(const RainCoverTileHeader& h);
// A header-only tile's codes for a res² block: code 1 everywhere with hits, else none.
FTCORE_API void rainCoverTileFill(const RainCoverTileHeader& h, int res, unsigned short* out);

// The strip's tile list (Data/Look/rain_cover_tiles.txt): "t tx ty" lines, '#' comments. Parses one line; false for any
// other line.
FTCORE_API bool rainCoverTileLine(const char* line, RainCoverTile& tile);

}  // namespace ftcore
