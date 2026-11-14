#pragma once

// Weather on the world's surfaces: wet ground, puddles and contact grime, written as one material node that runs after a
// surface master's own node (the photographed boxes, the plain tinted master the roads wear, the window master).
//
// Wet: the look's Wetness (its parameter collection, 0 when no look is applied) soaks up-facing pixels only: the albedo
// darkens toward soakDarken, the roughness falls toward soakRough (along a curve, so a half-wet street keeps out of the
// reflection-traced range) and the normal flattens. A drying-patch noise leaves part of the ground damp (dampShare of
// the soak). Puddles are a world-noise mask whose cover grows with Wetness: a soft darker edge, then a core with roughness
// puddleRough and a flat normal (no ring where all three would change at once); they
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
    float puddleEdge = 0.2f;             // noise span of a puddle's soft wet-to-dry edge (darkens first)
    float puddleCoreFrom = 0.4f;         // the mirror (puddleRough, flat normal) starts this far into the edge ramp
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
    // Asphalt detail tile (SurfaceWeatherOptions::streetDetail): a baked 1 m tile of stones and binder, world-mapped on
    // the asphalt; it replaces the procedural aggregate near by (the mips carry it to its mean far off).
    float detailTileM = 1.0f;            // the tile's side in the world
    float detailAlbedoMean = 0.104f;     // the albedo map's linear luminance mean (the tile adds grain, not tone)
    float detailRoughMean = 0.848f;      // the roughness channel's mean
    // With the tile, a wheel path polishes the exposed stone tops (the tile's blue channel) toward wheelTopRough and the
    // binder only by wheelPolishBinder (the dense binder stays matt; wheelPolish is the tile-less road's).
    float wheelTopRough = 0.55f;
    float wheelPolishBinder = 0.1f;
    float wheelRunM = 4.0f;              // a wheel path's strength wanders along it on this cell (0.75-1)
    // Kerb litter (asphalt, a cambered half): pale silt and fallen blossom petals in from the gutter.
    float litterM = 0.5f;                // the litter's band from the gutter edge
    float litterDust = 0.25f;            // albedo gained at full silt
    float petalCellM = 0.035f;           // one petal at most per cell this size
    float petalShare = 0.35f;            // the share of cells at the gutter holding one (falling to 0 across the band)
    float petalR = 0.68f, petalG = 0.5f, petalB = 0.52f; // a fresh petal's albedo (aged ones brown toward the road)
    float detailTone = 1.0f;             // share of the tile's albedo ratio taken into the tone
    float detailSlope = 0.5f;            // share of the tile's true slope taken into the normal
    float detailRough = 0.6f;            // share of the tile's roughness ratio taken
    float detailAO = 0.7f;               // share of the tile's cavity AO taken
    float tileM = 0.4f;                  // pavement tile
    float tileJointM = 0.005f;           // half a joint
    float tileBevelM = 0.008f;
    float tileDepthM = 0.004f;
    float tileTone = 0.08f;              // per-tile albedo swing, plus and minus
    float kerbStoneM = 0.9f;
    float paintAlbedo = 0.75f;           // a marking's brightest channel
    float laneSlabM = 3.6f;              // a lane's concrete panel
    float cornerSlabM = 0.311f;          // a pavement box this thick (within 4 mm) is a junction corner's piece: its
                                         // tiles lie on the world's grid, not the box's (Roads.h kRoadCornerPaveDeep)
    // Joint depth (SurfaceWeatherOptions::jointDepth): real groove depths, an imperfect grid and parallax near by.
    float paveJointDepthM = 0.0065f;     // a pavement tile's joint floor under its face
    float paverJointDepthM = 0.005f;     // a lane paver's
    float kerbJointDepthM = 0.008f;      // a kerb stone's
    float sunkShare = 0.06f;             // tiles (by their hash) sunk sinkMinM..sinkMaxM
    float tiltShare = 0.06f;             // tiles tilted tiltMinRad..tiltMaxRad across (a constant slope, any direction)
    float chipShare = 0.05f;             // tiles with one chipped corner, chipMinM..chipMaxM across (not kerbs)
    float sinkMinM = 0.001f, sinkMaxM = 0.003f;
    float tiltMinRad = 0.003f, tiltMaxRad = 0.008f;  // 0.17-0.46 degrees
    float chipMinM = 0.02f, chipMaxM = 0.03f;
    float parallaxNearM = 12.0f;         // parallax full within this of the camera, gone by parallaxFarM
    float parallaxFarM = 15.0f;
    float parallaxNdVFull = 0.25f;       // ... and full where the view's cosine to the face is over this, gone under
    float parallaxNdVNone = 0.12f;       // parallaxNdVNone
    float parallaxSlopeClamp = 0.2f;     // the view's cosine floor in the relief solve's slope
    float jointCavityAO = 0.6f;          // AO on a joint's floor (a narrow, deep joint) ...
    float jointOpenAO = 0.75f;           // ... and on the widest, shallowest (light reaches in)
    float jointWidthMin = 0.7f, jointWidthMax = 1.4f;  // each joint's width factor (its own cell's hash, a noise along it)
    float jointDepthMin = 0.7f, jointDepthMax = 1.3f;  // and its depth factor
    float jointNoiseM = 0.35f;           // the noise along a joint, its lattice step: tiles, pavers, kerbs (about a
    float paverNoiseM = 0.08f;           // segment's length, so none is even end to end)
    float kerbNoiseM = 0.12f;
    float jointNoiseShare = 0.3f;        // the noise's share of each factor (the rest the joint's hash)
    float jointBevelScale = 1.5f;        // the relief's arrises this much wider than the plain layer's
    float jointFloorToneHi = 0.6f;       // a joint floor's tone against its unit's, by its grit (never black) ...
    float jointFloorToneLo = 0.45f;
    float jointDirtTone = 0.8f;          // ... dirt patches on the floor darker by this, wet by jointWetTone
    float jointWetTone = 0.9f;
    float jointRoughDry = 0.6f;          // a joint floor's roughness at least this dry, jointRoughWet wet, plus
    float jointRoughWet = 0.45f;         // jointRoughGrit by the grit noise
    float jointRoughGrit = 0.15f;
    float jointSlopeMax = 1.7320508f;    // the relief's normal tips at most 60 degrees from the face's
    float jointNormalFrom = 0.5f;        // the joint normal full until the footprint across the joint is this share
    float jointNormalTo = 1.0f;          // of the groove (joint and both arrises), gone at this
    float paverFlawScale = 0.25f;        // lane pavers' sunk, tilt and chip shares against the tiles'
    float unitToneExtra = 0.04f;         // per-unit tone swing the relief adds (plus and minus)
    float unitRoughSwing = 0.08f;        // per-unit roughness swing (plus and minus)
};

// A jointed grid's numbers for the relief: cells cellX by cellY (m), every other row offset by bond cells, joints
// halfJointM half wide, arrises bevelM round, the floor depthM under the face, flaw shares scaled by flawScale, the
// noise along a joint noiseM a step.
struct StreetJointGrid {
    float cellX = 0.4f, cellY = 0.4f, bond = 0.f, halfJointM = 0.005f, bevelM = 0.008f, depthM = 0.0065f;
    float flawScale = 1.f, noiseM = 0.35f;
};
// The factors on the joints nearest a point: the one across x (widthX, depthX) and the one across y.
struct StreetJointFactors {
    float widthX = 1.f, widthY = 1.f, depthX = 1.f, depthY = 1.f;
};
// The relief at a point: height z (m), its gradient (gx, gy; slope-clamped, joint part faded by the footprint), the
// floor share (1 in a joint or chip) and how open its joint is (0 narrow and deep .. 1 wide and shallow).
struct StreetJointSample {
    float z = 0.f, gx = 0.f, gy = 0.f, floorShare = 0.f, open = 0.5f;
};
// A tile's flaws from its key (its cell id plus the box's offset, as the HLSL's): sinkM down, a tilt (rad, x and y),
// a chipped corner (chipX, chipY its signs, chipR its size in m; 0 none).
struct StreetTileFlaw {
    float sinkM = 0.f, tiltX = 0.f, tiltY = 0.f;
    float chipX = 0.f, chipY = 0.f, chipR = 0.f;
};
struct StreetPoint {
    float x = 0.f, y = 0.f;
};

// 0..1 wheel-path weight at fromLowM across a carriageway half from its low edge, the half sizeY across.
FTCORE_API float streetWheelPath(float fromLowM, float sizeY, const StreetSurfaceSpec& spec);
// Distance (m) from posM along a slab's run to its nearest asphalt joint; the joints depend on the run's position and
// the slab's length alone, so both halves of a road line up.
FTCORE_API float streetJointDistance(float posM, float lengthM, const StreetSurfaceSpec& spec);
// 0..1 paint left on a marking: fade noise 0..1, wheel 0..1 (how much a wheel path crosses there), chip noise 0..1.
FTCORE_API float streetPaintLeft(float fade, float wheel, float chip);
// Whether a pavement box sz thick (m) is a junction corner's piece (tiles on the world's grid).
FTCORE_API bool streetCornerSlab(float sz, const StreetSurfaceSpec& spec);
// A pavement tile's height, 0 (a joint's floor) to 1 (its face), dM from its nearest edge.
FTCORE_API float streetTileHeight(float dM, const StreetSurfaceSpec& spec);
// The jointDepth grids: StreetKind::Pavement (tiles), Lane (pavers, running bond) or Kerb (stones along x; the grid
// is 10 km across y, so pass y = 5000 and there are no corners). Any other kind gives the pavement's.
FTCORE_API StreetJointGrid streetJointGrid(StreetKind kind, const StreetSurfaceSpec& spec);
// The HLSL's FTRelief.Hs flaws for a tile key (cell id + box offset), the shares scaled by shareScale.
FTCORE_API StreetTileFlaw streetTileFlaw(float keyX, float keyY, const StreetSurfaceSpec& spec, float shareScale = 1.f);
// The HLSL's FTRelief.Fac: the width and depth factors of the joints nearest (x, y) (each joint keyed by its own cell,
// so both sides of it agree), with the noise along it.
FTCORE_API StreetJointFactors streetJointFactors(float x, float y, float keyX, float keyY, const StreetJointGrid& grid,
                                                 const StreetSurfaceSpec& spec);
// The HLSL's FTRelief.Z: the relief at (x, y) m in the grid's frame with the factors f and the footprint (m) across
// the x joints (footX) and the y joints (footY); 0 for none.
FTCORE_API StreetJointSample streetJointSample(float x, float y, float keyX, float keyY, const StreetJointFactors& f,
                                               float footX, float footY, const StreetJointGrid& grid,
                                               const StreetSurfaceSpec& spec);
// Its height (m; 0 the nominal face, -depthM times the depth factor a joint's floor), the factors at (x, y), no
// footprint. Sunk, tilted and chipped tiles included.
FTCORE_API float streetJointZ(float x, float y, float keyX, float keyY, const StreetJointGrid& grid,
                              const StreetSurfaceSpec& spec);
// The footprint across a joint (m): the pixel's footprint foot (its long axis or more), nDotV the view's cosine to the
// face, viewAcross the view's unit vector's component across the joint. foot for a joint across the view, foot *
// nDotV for one along it.
FTCORE_API float streetJointFootprint(float foot, float nDotV, float viewAcross);
// A joint floor's tone against its unit's: grit, dirt and wet 0..1.
FTCORE_API float streetJointFloorTone(float grit, float dirt, float wet, const StreetSurfaceSpec& spec);
// A joint floor's roughness from rough: at least jointRoughDry (dry) to jointRoughWet (wet), plus the grit's share.
FTCORE_API float streetJointFloorRough(float rough, float grit, float wet, const StreetSurfaceSpec& spec);
// The HLSL's parallax weight RlW on a top face (the HLSL also multiplies by TopF): switch 0..1, distM to the camera,
// nDotV the view's cosine to the face, fine the detail fade (1 near).
FTCORE_API float streetParallaxWeight(float distM, float nDotV, float jointDepth, float fine, const StreetSurfaceSpec& spec);
// The HLSL's relief solve (before its weight): the point (x, y) shifted for a view whose unit vector toward the
// camera has (viewX, viewY) in the grid's plane and nDotV along its normal.
FTCORE_API StreetPoint streetReliefShift(float x, float y, float viewX, float viewY, float nDotV, float keyX, float keyY,
                                         const StreetJointGrid& grid, const StreetSurfaceSpec& spec);
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
    bool jointDepth = false; // the street's joints as real recesses, by ViewVec and JointDepth inputs (needs street)
    bool streetDetail = false; // the asphalt's baked detail tile, by DetAlb, DetNrm and DetRA texture inputs (needs street)
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
// engine's 100 cm cube); with aboveInput Above; with street Street; with street and jointDepth ViewVec (float3, world,
// cm, from the pixel to the camera, not normalised) and JointDepth (0..1, a run-time switch). Returns the albedo;
// outputs NormalOut, RoughOut, AOOut. At Street 0 and Wetness 0, NormalOut is the Normal input, so a mesh's own normal
// stays as it was; at JointDepth 0 every output is what the node gives without jointDepth.
//
// streetDetail: texture-object inputs DetAlb (sRGB albedo), DetNrm (DirectX normal map) and DetRA (linear: roughness in
// red, AO in green), all on a detailTileM tile mapped on the world's plan. On asphalt (Street 1) the tile's albedo over
// detailAlbedoMean scales the tone, its slope (detailSlope of it) joins the normal, its roughness over detailRoughMean
// the roughness, and its AO the AO; the procedural aggregate's near terms are left out.
//
// jointDepth: pavement tiles, lane pavers and kerb stones get joints paveJointDepthM, paverJointDepthM and
// kerbJointDepthM deep (each joint's width and depth varied by its hash and a noise along it) with wide, convex
// arrises; by a per-tile hash sunkShare of tiles sit sunk, tiltShare tilt slightly and chipShare (not kerbs) lose a
// corner to the joint floor (pavers a paverFlawScale of those). The normal is the height's true slope, at most
// jointSlopeMax, faded per joint by the footprint across it (and the arris widened to that footprint), so joints along
// the view hold further than joints across it. The joint floor is a gritty grey (jointFloorToneLo..Hi of its unit,
// dirt patches darker), rough (jointRoughDry, jointRoughWet wet, except under a puddle), its AO jointCavityAO to
// jointOpenAO. Units get extra tone and roughness swings. Near by (parallaxNearM..parallaxFarM, and not glancing) the
// tile coordinate is shifted by an analytic relief solve on the top face (a kerb's side faces get no parallax). No new
// screen derivatives: the only one is the street layer's footprint, as before.
FTCORE_API std::string surfaceWeatherHlsl(const SurfaceWeatherSpec& spec, const SurfaceWeatherOptions& options,
                                          const StreetSurfaceSpec& street = StreetSurfaceSpec{});

// The same soak and puddles as a flat face of that node at Up 1, for a decal over it (so its wet edge matches the ground
// under it): a snippet for inside another Custom node that reads WorldPos (cm), WetIn (0..1) and GutIn (0..1, the gutter
// band), declares Noise, WSoak and WPud, and darkens float3 C and smooths float R, which the caller has declared.
FTCORE_API std::string surfaceWetHlsl(const SurfaceWeatherSpec& spec);

// The lattice hash under the street layer's and the road decals' value noise (FTStreetNoise, FTRoadDecalNoise N): integer
// mixing on a cell's corner, so a corner reads the same from each of the four cells round it at any distance from the
// origin. (A float hash of the corner's coordinate amplifies one rounding step into a new value, and the shader compiler
// rounds a corner differently from its two sides: the cells' edges then tear.) 0..1, in 2^-24 steps.
FTCORE_API float streetLatticeValue(int x, int y);
// The value noise on it at p (cells): the corners blended by smoothstep, as the HLSL N.
FTCORE_API float streetNoise(float x, float y);
// The hash as an HLSL member function, "float L(int2 c) { ... }", one line.
FTCORE_API const char* streetLatticeHlsl();

}  // namespace ftcore
