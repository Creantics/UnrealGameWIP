#pragma once

// Glass, as one function every glass surface calls: what the eye gets from a pane is
//
//   seen = street * R(angle) + interior * (1 - R(angle)) * tint
//
// where R is the pane's mirror share (Fresnel at both faces, more at glancing angles), `street` the light the pane
// mirrors and `interior` the light behind it. By day the street is bright and the room dim, so the pane shows the sky,
// the far side of the street and whoever stands before it; at night the lit room wins and the eye sees in. Nothing
// switches on the hour: the flip follows from the two levels.
//
// Three kinds of surface use it (the materials are built in the Unreal module):
//   a true pane: a translucent mesh over real depth; the engine mirrors the scene, the things behind are real;
//   glass over a picture: an opaque smooth surface the engine mirrors the scene in, its interior (a room box on the
//     true view ray, with its own day and night light) added as emitted light times (1 - R) * tint;
//   far glass: the second kind with a cheap mirror.
//
// The detail layer is the same for all: a dust film and dirt toward the bottom edge and the corners, dried rain streaks
// from the top edge, hand marks round a door handle, a slight wave on old panes, the rubber seal and the dark edge of
// the pane, and beads of water when the ground is wet. glassHlsl() writes it as shader code; the functions below are
// the same maths for the tests and for the numbers a still is judged by.
//
// Plain C++20, float maths like the GPU's. No allocation except glassHlsl (build time only).

#include "ftcore/Api.h"

#include <array>
#include <string>

namespace ftcore {

inline constexpr float kGlassF0 = 0.04f;  // a glass face's mirror share seen square-on (index 1.5)

// One face's mirror share at the cosine of the angle between the view and the pane's normal (Schlick). No allocation.
FTCORE_API float glassFresnel(float cosView, float f0 = kGlassF0);
// A pane's (two faces, the light bouncing between them): 2F / (1 + F). 0.077 square-on, 1 at a glancing view.
FTCORE_API float glassPaneMirror(float cosView, float f0 = kGlassF0);
// The light a pane of `thicknessM` passes at that angle (soda-lime glass: a faint green, deeper along a longer path
// and with the pane's age 0..1). No allocation.
FTCORE_API std::array<float, 3> glassTint(float cosView, float thicknessM, float age);

// What the eye gets from a pane: the mirrored street, the interior seen through, and their ratio (through / mirrored;
// over 1 the eye sees in, under 1 it sees the street). Luminances in any one unit. No allocation.
struct GlassSeen {
    float mirrored = 0.f;
    float through = 0.f;
    float ratio = 0.f;
};
FTCORE_API GlassSeen glassSeen(float streetLum, float interiorLum, float cosView, float tintLum = 0.9f);

// The detail layer at a point of a pane. (u, v) 0..1 across and up it, the pane width x height metres, seed 0..1, age
// 0 (new shop glass) .. 1 (an old pane), wet 0..1 (the look's Wetness), the handle at (handleU, handleV) or handleU < 0
// for none. The same maths as glassHlsl. No allocation.
struct GlassDetail {
    float dust = 0.f;     // the film: 0 clean .. 1 (toward the bottom edge and the corners)
    float streak = 0.f;   // dried rain streaks from the top edge
    float hand = 0.f;     // hand and finger marks round the handle
    float bead = 0.f;     // beads of water (wet glass)
    float seal = 0.f;     // 1 on the rubber seal at the pane's rim
    float edge = 0.f;     // 1 on the dark edge of the pane just inside the seal
    float rough = 0.f;    // the surface's roughness with all of the above
    float veil = 0.f;     // the pale film the dirt lays over what is seen (0..1)
};
FTCORE_API GlassDetail glassDetail(float u, float v, float width, float height, float seed, float age, float wet,
                                   float handleU, float handleV);

// The shader code (a block for a Custom node, no function definitions). It reads these, which the node declares first:
//   float2 gUV (0..1 across and up the pane), float2 gSize (m), float gSeed (0..1), float gAge (0..1), float gWet (0..1),
//   float gCos (view . normal), float2 gHandle (the handle's uv, x < 0 for none), float gThick (m)
// and leaves: float gMirror (the pane's mirror share), float3 gTint, float gDust, gStreak, gHand, gBead, gSeal, gEdge,
// gRough, gVeil, float2 gWave (a slope to bend the normal by, old panes only). Allocates: build time only.
FTCORE_API std::string glassHlsl();

// The pane a point x (metres along a glazed front) falls in, when the front's panes are cut at `cuts` (rising; the
// first and last are the glass's ends): its left end and its width. Outside the glass, the nearest pane. No allocation.
struct GlassPane {
    float left = 0.f;
    float width = 1.f;
};
FTCORE_API GlassPane glassPaneAt(float x, const float* cuts, int count);

// The interior behind glass over a picture, as shader code (a block for a Custom node): a room box fixed in the wall's
// frame and met by the true view ray, so it has depth and never moves with the eye. A shop: a floor of tiles, a
// ceiling with light panels, shelving with goods along the back wall and a free-standing shelf run in front of it; a
// room: walls, a door, a picture, a wooden floor, a ceiling lamp, and on some windows a blind let down part of the way
// or a curtain drawn part of the way across, behind the glass. Its light is its own: daylight falling off into the
// room (iDay * iDayLevel) plus its lamps (iLit * iLamp); nothing reads the hour. It reads, declared by the node first:
//   float2 iA (m: along the wall, up), float iFloor (m up: the room's floor), float3 iV (the view ray: along, up, in,
//   in > 0), float iShop (1 a shop, 0 a room), float iSeed, float iLit (0..1), float iDay (0..1), float iDayLevel,
//   float3 iLamp (the lamps' light)
// and leaves float3 iCol: the light the interior sends toward the eye, before the pane. Allocates: build time only.
FTCORE_API std::string glassPictureHlsl();

} // namespace ftcore
