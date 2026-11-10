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
// Pieces thinner than grimeThinM (sign plates, poles, rails) take neither.
//
// The C++ functions mirror the HLSL, one term each, for tests. Plain C++20; surfaceWeatherHlsl allocates (build time
// only), the rest doesn't.

#include "ftcore/Api.h"

#include <string>

namespace ftcore {

struct SurfaceWeatherSpec {
    float upFrom = 0.55f, upTo = 0.85f;  // vertex normal z: dry below upFrom, fully up-facing from upTo
    float soakDarken = 0.6f;             // albedo multiplier at full soak
    float soakRough = 0.1f;              // roughness at full soak
    float roughCurve = 1.5f;             // roughness falls by soak^roughCurve
    float soakFlatten = 0.7f;            // share of the normal flattened at full soak
    float dampShare = 0.45f;             // soak in a drying patch, as a share of a wet patch's
    float patchScaleM = 6.0f;            // drying-patch noise cell
    float puddleScaleM = 3.5f;           // puddle noise cell
    float puddleCover = 0.5f;            // the puddle threshold falls from 1 by this at Wetness 1
    float puddleEdge = 0.06f;            // noise span of a puddle's rim
    float puddleRough = 0.02f;
    float puddleDarken = 0.75f;          // albedo multiplier in a puddle, over the soak's
    float tiltMin = 0.005f;              // |world z of the box's Y axis| over which a box counts as cambered
    float gutterM = 0.6f;                // the gutter band's width over the low edge
    float gutterBias = 0.25f;            // noise lift at the low edge, falling to 0 across the band
    float grimeHeightM = 0.6f;
    float grimeStrength = 0.6f;
    float grimeScaleM = 0.5f;            // grime noise cell
    float grimeThinM = 0.2f;             // pieces thinner than this (either plan side) take no dirt
    float edgeWidthM = 0.12f;
    float edgeStrength = 0.35f;
    float dirtR = 0.55f, dirtG = 0.5f, dirtB = 0.44f;  // albedo multiplier at full dirt
    float dirtOcclusion = 0.5f;          // AO lost at full dirt
};

struct SurfaceWeatherOptions {
    bool boxFrame = true;    // the node reads LocalPos and AxisX/Y/Z (gutters, and dirt when grime)
    bool grime = true;       // dirt: contact grime and edge darkening (needs boxFrame)
    bool aboveInput = false; // dirt measures height from an Above input (m over the ground), not the box's bottom
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
// engine's 100 cm cube); with aboveInput Above. Returns the albedo; outputs NormalOut, RoughOut, AOOut.
FTCORE_API std::string surfaceWeatherHlsl(const SurfaceWeatherSpec& spec, const SurfaceWeatherOptions& options);

}  // namespace ftcore
