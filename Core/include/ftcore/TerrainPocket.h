#pragma once

// Stuck pockets: places the lead can get into (walking, or dropping off something) but not out of on foot, and the
// retaining plinths and trough fills that close them.
//
// The check lays a grid (PocketSpec::cell) over a plan rect and reads one floor per cell: the ground (massingGroundAt),
// raised to the top of every box `include` marks that holds the cell's middle and whose bottom stands under floor +
// capsuleHeight (taken lowest bottom first, so a box on a box counts; one higher up is overhead and the capsule passes
// under it). A cell is standable when its floor's normal is at least walkableZ up (the ground's slope from the cells
// around it; a box top by its own slope). A cell is out of play when its floor lies under seaLevel (water), or squeezed
// when the cells on both sides of it along x, or along y, rise more than maxStep over it (a slot under a cell and a half
// wide that the capsule doesn't fit).
//
// The lead moves to a side neighbour b from a when b is in play and not squeezed, and its floor rises no more than
// maxStep over a's (jumpRise when a and b are standable and jumpRise > maxStep); a rise of more than kPocketFlatRise needs
// b standable; from a cell that isn't standable the lead slides: down or across, never up. A drop of any height is a move. The lead comes
// in at the anchor points (at the rect's standable edge cells when there are none) and gets out at an anchor point, a
// standable edge cell or the water (it drops into the sea). A pocket cell is reached from the way in but reaches no way
// out; pockets are their side-connected groups.
//
// planPlinths closes them in two steps, as boxes (kind "plinth", source Invented, no cover) for the build to lay:
//   skirts  each building (kind kit, box or slab, at least kPocketBuildingHeight tall, level) on sloping ground (the
//           ground within `band` of its footprint spanning more than maxStep): its faces in pieces `segment` long and
//           `band` deep, a piece wherever the ground in it is steeper than walkable, or dips at the wall (the ground at
//           the wall more than maxStep under the ground further out), and nothing taller already stands there. Its top is
//           level at the highest ground in it, its bottom `sink` under the lowest: a retaining plinth with no gap between
//           the building's base and the bank. Runs of pieces whose tops are within kPocketSkirtMerge join.
//           So do the skirts of different buildings standing in one line, end to end (PlinthSpec::joinSkirts).
//   fills   then, up to fillPasses times, the check on the boxes and the plinths so far: each pocket no wider than
//           narrowWidth (Pocket::width) whose spill level, the lowest floor of a cell next to it that still reaches a way
//           out, stands no more than fillMaxRise over its lowest floor is raised to that level, as grid-aligned boxes
//           over its cells under it (its banks included: a level terrace out to the ground it spills onto).
//   stairs  in the same passes, each other pocket with a spill gets one stair out: from the side cell with a way out
//           (lowest first) straight in, a step of stairRise every cell, three cells wide, down to a floor of the pocket;
//           where none fits (a pocket too thin for its rise) it is filled as above if its rise is within fillMaxRise.
// The check runs once more after; pockets with no way out, or that no stair fits, stay, reported.
//
// Plain C++20. Allocates (build time only): the grid is about 10 bytes a cell.

#include "ftcore/Api.h"
#include "ftcore/MassingDress.h"
#include "ftcore/MassingPlan.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ftcore {

// A rise of up to this (m) onto a cell that isn't standable is still a move (bumps across a sampled slope).
inline constexpr double kPocketFlatRise = 0.05;
// The skirts' buildings: at least this tall (m).
inline constexpr double kPocketBuildingHeight = 2.5;
// Skirt pieces along a face whose tops lie within this (m) join into one.
inline constexpr double kPocketSkirtMerge = 0.05;

// The lead as the check moves it. The defaults are AFTExploreCharacter's: a capsule 0.34 m across the radius and 1.76 m
// tall (0.35 and 1.8 here), UCharacterMovementComponent's default MaxStepHeight 45 cm and WalkableFloorAngle 44.765
// degrees (WalkableFloorZ 0.71), and a jump of JumpZVelocity 428 cm/s under 980 cm/s2, 0.93 m at its top.
struct PocketRect {
    double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
};

struct PocketSpec {
    double cell = 0.5;          // m, the grid's spacing
    double capsuleHeight = 1.8; // m: a box whose bottom stands this far over a floor is overhead
    double maxStep = 0.45;      // m, a step up the lead walks
    double jumpRise = 0.0;      // m, a rise a jump makes from a standable cell to a standable cell (0: no jumps)
    double walkableZ = 0.71;    // the least normal z standable
    double seaLevel = 0.2;      // m: a floor under this is water, out of play
    double narrowWidth = 2.0;   // m: planPlinths fills pockets no wider than this
    std::vector<PocketRect> exempt; // pockets lying inside one of these (0.5 m slack) aren't reported (counted in exempted)
};

struct PocketPoint {
    double x = 0.0, y = 0.0;
};

struct Pocket {
    double cx = 0.0, cy = 0.0;                       // the middle of its cells, plan m
    double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
    std::size_t cells = 0;
    double area = 0.0;     // m2
    double width = 0.0;    // m: twice its floor's deepest cell's distance from the floor's edge (its floor: the cells
                           // the lead stands on, or all of them when it stands on none)
    double floorMin = 0.0; // m, its lowest floor
    double spill = 0.0;    // m, the lowest floor of a cell next to it that reaches a way out (when hasSpill)
    bool hasSpill = false;
    int nearBox = -1;      // a building box (planPlinths' kinds) within 1 m of one of its cells, or -1
};

struct PocketReport {
    std::size_t cellsX = 0, cellsY = 0;
    std::size_t inPlay = 0;       // cells neither water nor squeezed
    std::size_t reached = 0;      // cells reached from the way in
    std::size_t pocketCells = 0;
    std::size_t exempted = 0;     // pockets inside an exempt rect, left out
    std::vector<Pocket> pockets;  // largest first
};

// Parses "X0,Y0,X1,Y1[;X0,Y0,X1,Y1]" into rects (each made min..max); false on a malformed list.
FTCORE_API bool parsePocketRects(const std::string& text, std::vector<PocketRect>& out);

// The rect the boxes `include` marks cover, grown by margin and cut to the ground's extent (an empty ground: not cut).
FTCORE_API PocketRect pocketRectOf(const MassingGround& ground, const std::vector<MassingBox>& boxes,
                                   const std::vector<bool>& include, double margin);

// Whether a box is a building the skirts stand against.
FTCORE_API bool pocketBuilding(const MassingBox& box);

// The check. False (error set) on a rect with no area or over 100 million cells.
FTCORE_API bool findPockets(const MassingGround& ground, const std::vector<MassingBox>& boxes, const std::vector<bool>& include,
                            const PocketRect& rect, const std::vector<PocketPoint>& anchors, const PocketSpec& spec,
                            PocketReport& out, std::string* error = nullptr);

struct PlinthSpec {
    PocketSpec pocket;
    double band = 2.0;     // m, a skirt piece's depth out from the wall
    double segment = 1.0;  // m, a skirt piece's length along the wall
    double sink = 0.3;     // m, a plinth's bottom under the lowest ground it covers
    double fillMaxRise = 8.0; // m: a pocket whose spill stands higher over its floor isn't filled
    double stairRise = 0.3;   // m, a stair's riser (under maxStep)
    int fillPasses = 16;      // passes of fills and stairs
    bool skirts = true;
    bool joinSkirts = true; // skirts of different buildings in one line, end to end, join as a face's own runs do
    bool fills = true;
    bool stairs = true;
};

struct PlinthStats {
    std::size_t slopedBuildings = 0; // buildings on sloping ground
    std::size_t skirts = 0;          // skirt boxes (after joining)
    std::size_t fills = 0;           // fill boxes
    std::size_t filledPockets = 0;
    std::size_t wideFilled = 0;      // of them, wider than narrowWidth (no stair fitted)
    std::size_t stairs = 0;          // stair step boxes
    std::size_t stairedPockets = 0;
    int passes = 0;                // fill/stair passes that placed something
    std::size_t pocketsBefore = 0, pocketCellsBefore = 0;
    std::size_t pocketsAfter = 0, pocketCellsAfter = 0;
    std::size_t narrowLeft = 0, wideLeft = 0; // pockets after, no wider than narrowWidth / wider
    PocketReport after;                       // the last check
};

// The plinths for the boxes `include` marks, appended to `out` (ids PL-S-<n> for skirts, PL-F-<n> for fills, PL-X-<n>
// for stair steps).
FTCORE_API bool planPlinths(const MassingGround& ground, const std::vector<MassingBox>& boxes, const std::vector<bool>& include,
                            const PocketRect& rect, const std::vector<PocketPoint>& anchors, const PlinthSpec& spec,
                            std::vector<MassingBox>& out, PlinthStats& stats, std::string* error = nullptr);

// Floating boxes: a box `include` marks whose base stands over the ground somewhere under its footprint. The check reads
// its footprint's edge and inside every `sample` m (corners included) and keeps, per point, the base (z0, lifted as the
// box's rise lifts it) less the ground; a point is held up when another box `include` marks stands there with its top
// within `support` of the base and its bottom under it, and out of play when its ground lies under seaLevel (decks,
// piers, boats). A box's gap is its largest over the points neither held up nor out of play. Boxes no taller than
// kMassingMinHeight (flat paint, pads) aren't checked. The check reads the box (its z0), not a kit mesh laid in its place.
// The build fails on a float over failAny, or a building's over failBuilding (the 2026-11-17 ruling).
//
// With spanRule (the 2026-11-17 20:04 ruling) a box of a spanKinds kind need not be held at every point. Its supporters are
// the boxes that hold at least one of its points; the points left hanging (gap over visible, neither held nor out of play)
// pass when they lie inside the plan hull of the supporters' footprint corners and no more than maxSpan / 2 from the
// nearest supporter's footprint (a span of up to maxSpan between two held points), or outside that hull by no more than
// maxEave (an eave past the outermost held point). A box all of whose hanging points pass is spanned (FloatReport::
// spanned), not floating. Only sound boxes hold up others: the `sound` mask when given, and with chainCheck, no box that
// itself floats (the check reruns until no supporter drops out, so no chain of floating pieces holds anything).
// A kind's own eave in place of FloatSpec::maxEave (the 2026-11-18 ruling: a viaduct deck's wing is a cantilever off a
// continuous deck, so decks reach 3.5 m past their outermost held point; every other kind keeps maxEave).
// maxDepth caps the box's height (z1 - z0) the own eave applies to: a stepped viaduct segment rises at most ~9.3 m, so
// a piece of that kind deeper than this (one segment rising 35 m) is judged at maxEave like any other kind.
struct KindEave {
    std::string kind;
    double eave = 0.0;      // m
    double maxDepth = 1e9;  // m
};

struct FloatSpec {
    double sample = 1.0;        // m between the points read
    double sink = 0.3;          // m: a base this far under the ground everywhere is sound
    double visible = 0.05;      // m: a gap over this is a float (a crouch-under or a lit seam)
    double support = 0.05;      // m, a supporter's top within this of the base holds it up
    double seaLevel = 0.2;      // m: ground under this is water
    double maxSkirt = 1.5;      // m: a building floating up to this gets a foundation; over it, a placement error (snapped)
    double failAny = 0.9;       // m: a float over this fails the build
    double failBuilding = 0.3;  // m: a building's float over this fails the build
    double crouchLow = 0.9, crouchHigh = 1.4; // m: a building's gap in this band lets the crouched capsule in under it
    bool spanRule = false;      // carried boxes pass by span and eave (above)
    std::vector<std::string> spanKinds = {"deck", "bridge", "gate", "brace"}; // the kinds that span (the rest are held everywhere)
    double maxSpan = 30.0;      // m, the longest span between two held points
    double maxEave = 1.5;       // m, the furthest a box reaches past its outermost held point
    std::vector<KindEave> kindEaves = {{"deck", 3.5, 12.0}}; // kinds with their own eave (spanKinds only)
    bool chainCheck = true;     // with spanRule: a floating box holds nothing up
    double podiumCut = 1.0;     // m: a podium wears its building's surface this far down, terrace stone under that
    double postLength = 1.0;    // m along a bridge, each end post (planFoundations)
    // Deck posts (the 2026-11-18 ruling, behind the Strip's -DeckPosts; off, the plan is as before): a floating deck gets
    // posts down to the ground or the seabed, a grid at most postSpacing apart along and across it, flush with its ends
    // and set in from its sides by its eave less half a metre, each postSize square, its top along the deck's underside
    // (a ramp's posts slope with it), wearing the deck's surface (PL-D-<n>); and between neighbouring posts, along each
    // row and across each bent, diagonal braces braceSize square in zig-zag panels of about 45 degrees (PL-K-<n>, kind
    // "brace": spanned between the posts they sit in).
    bool deckPosts = false;
    double postSpacing = 6.0;   // m, the most between neighbouring posts' centres
    double postSize = 1.0;      // m, a post's side
    double braceSize = 0.3;     // m, a brace's side
};

// The furthest the box may reach past its outermost held point under spanRule: its kind's own eave (kindEaves, up to its
// maxDepth), else maxEave.
FTCORE_API double floatEave(const FloatSpec& spec, const MassingBox& box);

struct FloatBox {
    std::size_t box = 0;       // index into boxes
    double gap = 0.0;          // m, the base over the ground at its worst point (spanned: its worst hanging point)
    double x = 0.0, y = 0.0;   // that point
    double base = 0.0, ground = 0.0;
    double top = 0.0;          // the box's top at that point (a ramp's or landing's lip there: top less ground)
    double groundMin = 0.0;    // the lowest ground under its footprint (points out of play left out)
    double span = 0.0;         // spanRule: m, twice the furthest hanging point inside the hull from its nearest supporter
    double overhang = 0.0;     // spanRule: m, the furthest hanging point outside the hull from it
    std::size_t supporters = 0; // spanRule: the sound boxes holding at least one of its points
    bool building = false;     // pocketBuilding
    bool failing = false;      // over failAny, or a building over failBuilding
    bool crouch = false;       // a building whose gap lies in crouchLow..crouchHigh
};

struct FloatReport {
    std::size_t checked = 0;
    std::size_t shallow = 0;           // checked boxes whose gap lies over -sink (not sunk sink everywhere), floats included
    std::size_t failing = 0, crouch = 0;
    std::vector<FloatBox> floating;    // gap over visible, largest first
    std::vector<FloatBox> spanned;     // spanRule: carried boxes passed by span and eave, largest gap first
    int passes = 0;                    // chainCheck passes run
};

// The check. False (error set) on a sample under 0.05 m. `sound` (when given, by box): the boxes that may hold others up.
FTCORE_API bool findFloats(const MassingGround& ground, const std::vector<MassingBox>& boxes, const std::vector<bool>& include,
                           const FloatSpec& spec, FloatReport& out, std::string* error = nullptr,
                           const std::vector<bool>* sound = nullptr);

// What the fix pass does to a float.
enum class FloatFix : std::uint8_t {
    None,      // no fix of its own (spanned in the plan, or held once a supporter is fixed)
    Skirt,     // a building up to maxSkirt: a foundation box in its own surface (PL-B-<n>)
    Podium,    // a building over maxSkirt: podiumCut of its own surface (PL-P-<n>), terrace stone under it (PL-T-<n>)
    Lower,     // any other piece: its base down to sink under the lowest ground under it, its top, x and y kept
    UnderBox,  // a piece whose shape is kept (a kit mesh is fitted to it): a box under it in its surface (PL-U-<n>)
    Snap,      // a marking (flat paint): moved down onto the ground, never thickened
    Drop,      // a mast: moved down onto the highest sound top under its middle (the ground when there is none)
    RaisePiers, // a deck: the piers under it whose tops stand short of its base raised to it
    EndPosts,  // a bridge: a post at each end, its width and postLength long, down from its base (PL-E-<n>)
    Left,      // a deck with no pier to raise: stays failing, by name
    Posts,     // deckPosts: a deck gets posts and braces under it (PL-D-<n>, PL-K-<n>), short piers raised as well
};
FTCORE_API const char* floatFixName(FloatFix fix);

// A box the fix pass changes: boxes[box] becomes `to` (same id; only its z0, z1 and slope fields differ).
struct FloatEdit {
    std::size_t box = 0;
    MassingBox to;
};

struct FloatFixRow {
    std::size_t box = 0;
    FloatFix fix = FloatFix::None;
    double gap = 0.0;          // the float the plan pass saw
};

inline constexpr std::size_t kFoundationTerrace = static_cast<std::size_t>(-1);

struct FoundationPlan {
    std::vector<MassingBox> added;     // new boxes, to lay after the boxes (kind "plinth")
    std::vector<std::size_t> wears;    // per added box: the box whose surface it wears, or kFoundationTerrace
    std::vector<FloatEdit> edits;      // at most one per box
    std::vector<FloatFixRow> rows;     // per float the plan pass found, largest first
    int passes = 0;                    // plan passes (a deck left by name stops holding; the plan reruns)
};

struct FoundationStats {
    std::size_t floating = 0;     // floats the plan pass found
    std::size_t skirts = 0, podiums = 0, lowered = 0, underBoxes = 0, snapped = 0, dropped = 0;
    std::size_t piersRaised = 0;  // pier boxes raised (RaisePiers)
    std::size_t endPosts = 0;     // bridges given end posts
    std::size_t left = 0;         // floats left by name
    std::size_t posted = 0;       // deckPosts: decks given posts
    std::size_t posts = 0, braces = 0; // deckPosts: the boxes added
};

// The fixes for the floats the check (spanRule on, chainCheck off) finds; planFoundations runs before planPlinths, its
// boxes fed in. `keepShape` (when given, by box): boxes whose shape can't change (a kit mesh fitted to them). By kind,
// never by id: a building (pocketBuilding) gets a skirt or a podium; a marking a Snap; a mast a Drop; a deck RaisePiers
// (Left without a pier under it to raise); a bridge EndPosts; any other piece Lower (UnderBox when its shape is kept). A
// deck left stops holding others up and the plan runs again, until nothing more is left (at most 8 passes). With
// deckPosts, a deck floating over failAny is never left: it gets Posts (its short piers raised too).
FTCORE_API bool planFoundations(const MassingGround& ground, const std::vector<MassingBox>& boxes, const std::vector<bool>& include,
                                const std::vector<bool>* keepShape, const FloatSpec& spec, FoundationPlan& plan,
                                FoundationStats& stats, std::string* error = nullptr);

// The boxes with the plan's edits made and its boxes appended (include: true for them).
FTCORE_API void applyFoundations(const std::vector<MassingBox>& boxes, const std::vector<bool>& include, const FoundationPlan& plan,
                                 std::vector<MassingBox>& outBoxes, std::vector<bool>& outInclude);

// Each float of the check before the fix pass, as the check after it (spanRule and chainCheck on, on applyFoundations'
// boxes with any plinths appended) finds it: Failing when it still floats, Spanned when it passes by span and eave,
// Fixed otherwise.
enum class FloatOutcome : std::uint8_t { Fixed, Spanned, Failing };
struct FloatOutcomeRow {
    std::size_t box = 0;
    double before = 0.0;       // its gap before
    FloatFix fix = FloatFix::None;
    FloatOutcome outcome = FloatOutcome::Fixed;
    double after = 0.0;        // its gap after (Failing, Spanned)
    double span = 0.0, overhang = 0.0;
};
struct FloatOutcomes {
    std::vector<FloatOutcomeRow> rows; // before's floats, in its order
    std::size_t fixed = 0, spanned = 0, failing = 0;
    std::size_t newFloats = 0;         // floats after that weren't floats before
};
FTCORE_API void classifyFloats(const FloatReport& before, const FoundationPlan& plan, const FloatReport& after, FloatOutcomes& out);

} // namespace ftcore
