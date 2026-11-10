#pragma once

// Weather on the world's surfaces: wet ground, puddles and contact grime, written as one material node that runs after a
// surface master's own node (the photographed boxes, the plain tinted master the roads wear, the window master).
//
// Wet: the look's Wetness (its parameter collection, 0 when no look is applied) soaks up-facing pixels only: the albedo
// darkens toward soakDarken, the roughness falls toward soakRough (along a curve, so a half-wet street keeps out of the
// reflection-traced range) and the normal flattens. A drying-patch noise leaves part of the ground damp (dampShare of
// the soak). Puddles are a world-noise mask (roughness puddleRough, darker, flat) whose cover grows with Wetness; they
// favour gutters: a box tilted across its own Y axis (a carriageway slab falling from the crown to the kerb) has its low
// long edge as its gutter, and the noise is lifted within gutterM of it. Flat boxes get the noise alone.
//
// Dirt (boxes only): a grime band 0..grimeHeightM over the ground on side faces (the box's bottom, or a given height
// over the street floor for the window master) and a darkening within edgeWidthM of a side face's vertical edges.
// Pieces thinner than grimeThinM (sign plates, poles, rails) take neither. Top grime (boxes, its own option): on the top
// of a ground slab (a street piece, or any box at most topSlabMaxZ thick and topSlabMinPlan across) a dark band along
// its low (gutter) edge when it is cambered, else along both long edges (a pavement's kerb line and wall foot).
//
// Street (the plain master, its own option): a street piece's texture by its "Street" kind (StreetKind; 0 none):
// asphalt with aggregate, a 14 m tone layer, polished wheel paths per lane, a worn lane centre and edges, a damp
// gutter, tar-sealed joints every 20-40 m and sealed crack lines; markings at paint albedo 0.75, faded and chipped,
// worn where wheels cross; pavements in tiles with sunk joints and per-tile tone; kerbs in stones with joints; lanes in
// old concrete panels with patches. Fine detail fades with the pixel's footprint. It runs before the weather.
//
// The C++ functions mirror the HLSL, one term each, for tests. Plain C++20; surfaceWeatherHlsl allocates (build time
// only), the rest doesn't.

#include "ftcore/Api.h"

#include <string>

namespace ftcore {

struct SurfaceWeatherSpec {
    float upFrom = 0.55f, upTo = 0.85f;  // vertex normal z: dry below upFrom, fully up-facing from upTo
    float soakDarken = 0.5f;             // albedo multiplier at full soak
    float soakRough = 0.05f;             // roughness at full soak
    float roughCurve = 0.8f;             // roughness falls by soak^roughCurve
    float soakFlatten = 0.7f;            // share of the normal flattened at full soak
    float dampShare = 0.45f;             // soak in a drying patch, as a share of a wet patch's
    float patchScaleM = 6.0f;            // drying-patch noise cell
    float puddleScaleM = 3.5f;           // puddle noise cell
    float puddleCover = 0.65f;            // the puddle threshold falls from 1 by this at Wetness 1
    float puddleEdge = 0.06f;            // noise span of a puddle's rim
    float puddleRough = 0.02f;
    float puddleDarken = 0.75f;          // albedo multiplier in a puddle, over the soak's
    float tiltMin = 0.005f;              // |world z of the box's Y axis| over which a box counts as cambered
    float gutterM = 0.6f;                // the gutter band's width over the low edge
    float gutterBias = 0.25f;            // noise lift at the low edge, falling to 0 across the band
    float grimeHeightM = 1.0f;
    float grimeStrength = 0.8f;
    float grimeScaleM = 0.5f;            // grime noise cell
    float grimeThinM = 0.12f;            // pieces thinner than this (either plan side) take no dirt
    float edgeWidthM = 0.12f;
    float edgeStrength = 0.35f;
    float dirtR = 0.55f, dirtG = 0.5f, dirtB = 0.44f;  // albedo multiplier at full dirt
    float dirtOcclusion = 0.5f;          // AO lost at full dirt
    float grimeCurve = 1.0f;             // the band's falloff exponent over its height
    float topGrimeM = 0.35f;             // a ground slab's dirty band, in from its edge
    float topGrimeStrength = 0.45f;
    float topSlabMaxZ = 0.45f;           // a box no thicker than this and ...
    float topSlabMinPlan = 1.5f;         // ... at least this across both ways counts as a ground slab
};

// The street kinds the plain master's "Street" parameter takes (the road tints' MICs set it; roadStreetKind).
enum class StreetKind : int { None = 0, Asphalt = 1, Pavement = 2, Kerb = 3, Marking = 4, Lane = 5 };

struct StreetSurfaceSpec {
    float laneM = 3.4f;                  // a carriageway half has round(width / laneM) lanes, at least 1
    float wheelOffM = 0.85f;             // wheel paths at a lane's centre plus and minus this
    float wheelWideM = 0.22f;
    float wheelDarken = 0.12f;           // albedo lost in a wheel path
    float wheelPolish = 0.25f;           // share of roughness lost in a wheel path
    float wornCentre = 0.07f;            // albedo gained along a lane's centre
    float wornEdge = 0.06f;              // albedo gained 0.35-0.9 m off the gutter and along the crown
    float gutterDampM = 0.35f;           // the damp strip at the gutter: smoother
    float gutterDampPolish = 0.2f;
    float macroScaleM = 14.0f;           // the tone layer's cell
    float macroTone = 0.1f;              // its albedo swing, plus and minus
    float aggregateM = 0.025f;           // aggregate cell
    float aggregateSlope = 0.08f;        // its normal's tilt at most
    float jointEveryM = 30.0f;           // asphalt joints: one per cell of this, jittered +-jointJitterM (20-40 m apart)
    float jointJitterM = 5.0f;
    float jointWideM = 0.02f;
    float sealDarken = 0.5f;             // albedo multiplier on tar seal (joints and cracks)
    float crackScaleM = 3.0f;            // sealed-crack noise cell
    float tileM = 0.4f;                  // pavement tile
    float tileJointM = 0.005f;           // half a joint
    float tileBevelM = 0.008f;
    float tileDepthM = 0.004f;
    float tileTone = 0.08f;              // per-tile albedo swing, plus and minus
    float kerbStoneM = 0.9f;
    float paintAlbedo = 0.75f;           // a marking's brightest channel
    float laneSlabM = 3.6f;              // a lane's concrete panel
};

// 0..1 wheel-path weight at fromLowM across a carriageway half from its low edge, the half sizeY across.
FTCORE_API float streetWheelPath(float fromLowM, float sizeY, const StreetSurfaceSpec& spec);
// Distance (m) from posM along a slab's run to its nearest asphalt joint; the joints depend on the run's position and
// the slab's length alone, so both halves of a road line up.
FTCORE_API float streetJointDistance(float posM, float lengthM, const StreetSurfaceSpec& spec);
// 0..1 paint left on a marking: fade noise 0..1, wheel 0..1 (how much a wheel path crosses there), chip noise 0..1.
FTCORE_API float streetPaintLeft(float fade, float wheel, float chip);
// A pavement tile's height, 0 (a joint's floor) to 1 (its face), dM from its nearest edge.
FTCORE_API float streetTileHeight(float dM, const StreetSurfaceSpec& spec);
// 1 when a box (thickness and plan sides, m) counts as a ground slab.
FTCORE_API float weatherGroundSlab(float sz, float sx, float sy, const SurfaceWeatherSpec& spec);
// Top grime on a ground slab: edgeM from its dirty edge, noise 0..1, slab 0..1.
FTCORE_API float weatherTopDirt(float edgeM, float noise, float slab, const SurfaceWeatherSpec& spec);

struct SurfaceWeatherOptions {
    bool boxFrame = true;    // the node reads LocalPos and AxisX/Y/Z (gutters, and dirt when grime)
    bool grime = true;       // dirt: contact grime and edge darkening (needs boxFrame)
    bool aboveInput = false; // dirt measures height from an Above input (m over the ground), not the box's bottom
    bool topGrime = true;    // dirt along a ground slab's top edges (needs boxFrame)
    bool street = false;     // the street layer, by a Street input (needs boxFrame)
};

// 0..1, how up-facing a pixel is from its vertex normal's world z.
FTCORE_API float weatherUp(float normalZ, const SurfaceWeatherSpec& spec);
// 0..1, how much a pixel is a side face (vertical) from its vertex normal's world z.
FTCORE_API float weatherSide(float normalZ);
// The soak: wetness (0..1) on an up-facing share up, less in a drying patch (patchNoise 0..1 low).
FTCORE_API float weatherSoak(float wetness, float up, float patchNoise, const SurfaceWeatherSpec& spec);
// 0..1 gutter weight: axisYz the world z of the box's unit Y axis, localY the pixel's metres from the centre along it,
// sizeY the box's size along it in metres. 0 on a flat box.
FTCORE_API float weatherGutter(float axisYz, float localY, float sizeY, const SurfaceWeatherSpec& spec);
// 0..1 puddle: noise 0..1 at puddleScaleM.
FTCORE_API float weatherPuddle(float wetness, float up, float noise, float gutter, const SurfaceWeatherSpec& spec);
// 0..1 dirt on a face: side 0..1 (how vertical), thinnest the box's thinner plan side (m), aboveM over the ground,
// edgeM the distance to the face's nearest vertical edge, noise 0..1.
FTCORE_API float weatherDirt(float side, float thinnest, float aboveM, float edgeM, float noise, const SurfaceWeatherSpec& spec);
// The roughness after soak and puddle.
FTCORE_API float weatherRough(float rough, float soak, float puddle, const SurfaceWeatherSpec& spec);
// The albedo multiplier on one channel (0 r, 1 g, 2 b) after soak, puddle and dirt.
FTCORE_API float weatherAlbedoScale(int channel, float soak, float puddle, float dirt, const SurfaceWeatherSpec& spec);

// The node's body. Inputs: Albedo, Normal (world), Rough, AO, WorldPos, VertexNormal, Wetness, Amount (0 opts a
// material out); with boxFrame LocalPos, AxisX, AxisY, AxisZ (the instance's axes in world space, scale included, on the
// engine's 100 cm cube); with aboveInput Above; with street Street. Returns the albedo; outputs NormalOut, RoughOut,
// AOOut. At Street 0 and Wetness 0, NormalOut is the Normal input, so a mesh's own normal stays as it was.
FTCORE_API std::string surfaceWeatherHlsl(const SurfaceWeatherSpec& spec, const SurfaceWeatherOptions& options,
                                          const StreetSurfaceSpec& street = StreetSurfaceSpec{});

// The same soak and puddles as a flat face of that node at Up 1, for a decal over it (so its wet edge matches the ground
// under it): a snippet for inside another Custom node that reads WorldPos (cm), WetIn (0..1) and GutIn (0..1, the gutter
// band), declares Noise, WSoak and WPud, and darkens float3 C and smooths float R, which the caller has declared.
FTCORE_API std::string surfaceWetHlsl(const SurfaceWeatherSpec& spec);

}  // namespace ftcore
