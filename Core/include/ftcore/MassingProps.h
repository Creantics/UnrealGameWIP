#pragma once

// Street furniture for a massing map: props set along a street table (ftcore::MassingStreet, MassingPlan.h) by a table of
// rules, one asset per rule. Plain C++20; positions are in the plan frame (MassingPlan.h), the map's builder moving them to
// the engine's. Allocates: build time only.

#include "ftcore/Api.h"
#include "ftcore/MassingPlan.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

// Where along a street a rule sets its props, looking along the street's points in order.
enum class MassingPropSide : std::uint8_t {
    Left,    // the left kerb
    Right,   // the right kerb
    Both,    // both kerbs, opposite each other
    Stagger, // both kerbs in turn, left first on each street
    Wall,    // against the first built face out from the centreline on either side (left first), within reach of the kerb
};

// One rule: an asset set along every street of the kinds it names.
struct MassingPropRule {
    std::string id;                 // unique; as a massing id
    std::string asset;              // the mesh's object path; Core only carries it
    std::vector<std::string> kinds; // the street kinds it stands along; empty for every street
    double spacing = 0.0;           // m along the street between spots, > 0
    double phase = 0.0;             // m from the street's first point to its first spot, >= 0
    MassingPropSide side = MassingPropSide::Both;
    double inset = 0.0;             // m in from the kerb toward the centreline; Wall: out from the face toward it; >= 0
    double reach = 0.0;             // Wall: m past the kerb a face may stand, >= 0
    double clear = 0.0;             // m a kerb prop keeps from every footprint `include` marks, >= 0
    double turn = 0.0;              // degrees (engine yaw) added after the prop is turned to face the street
    double dz = 0.0;                // m over the floor under it
    std::size_t cap = 0;            // the most it places, kept evenly spread over the spots that pass; 0 for no cap
    double line = 0.0;              // m: when > 0, a spot with no built face this near it (massingBuiltFaceWithin)
                                    // places nothing, so the prop stands along a frontage, never alone in the open
    double footX = 0.0, footY = 0.0; // m: the room it takes on the plan along the mesh's own X and Y (massingPropFoot):
                                     // the table's foot_x_m and foot_y_m, or with both 0 there the mesh's plan size,
                                     // which the map's builder sets. With either 0 the prop takes no room and is never
                                     // crowded.
    double keep = 0.0;              // m its foot keeps from the feet of the street props placed before it and its own
                                    // rule's, when more than kMassingPropGap: a lamp kept off a pole
};

// Reads a prop table (CSV, a header row naming the columns; comment rows as in importMassingBoxes). The columns:
//   id         unique; as a massing id
//   asset      a static mesh's object path, not empty
//   spacing_m  > 0
// and optionally, each left empty for its default:
//   phase_m    >= 0, default 0
//   side       left, right, both (the default), stagger or wall
//   inset_m, reach_m, clear_m  >= 0, default 0
//   turn, dz   numbers, default 0
//   cap        a whole number >= 0, default 0
//   line_m     >= 0, default 0
//   foot_x_m, foot_y_m  >= 0, default 0: a foot smaller than the mesh's (a lamp's pole, not its arm)
//   keep_m    >= 0, default 0: the gap its foot keeps from the street props' feet (MassingPropRule::keep)
//   kinds     street kinds joined by ';' (each as a massing id but lower case allowed: a-z, A-Z, 0-9, '_' or '-')
// Any other column is skipped. `out` is cleared first. False, with a reason naming the line, on any error.
FTCORE_API bool importMassingProps(std::string_view csv, std::vector<MassingPropRule>& out, std::string* error = nullptr);

// A placed prop.
struct MassingProp {
    std::size_t rule = 0;       // index into the rules
    std::size_t street = 0;     // index into the streets
    double x = 0.0, y = 0.0;    // plan frame, m: the kerb spot, or the point of the face a Wall prop stands against
    double z = 0.0;             // m: its floor plus the rule's dz
    double face = 0.0;          // plan degrees, counter-clockwise from +x: the way it faces (to the centreline, or out of
                                // its face)
    bool wall = false;          // stands against a face at (x, y) rather than on it
};

// Why spots along a rule's streets placed nothing, and what it placed.
struct MassingPropCounts {
    std::size_t spots = 0;       // spots along its streets (both kerbs counted for Both)
    std::size_t placed = 0;
    std::size_t blocked = 0;     // a kerb spot within `clear` of a box standing over its floor
    std::size_t inRoad = 0;      // on another street's carriageway (a junction)
    std::size_t noWall = 0;      // Wall: no face, or one lower than kMassingPropWallHeight over the floor, within reach
    std::size_t open = 0;        // no built face within the rule's line
    std::size_t steep = 0;       // ground stepping over kMassingPropMaxStep under it
    std::size_t wet = 0;         // its floor at or under `dryAbove`
    std::size_t crowded = 0;     // its foot within kMassingPropGap of a foot already taken
    std::size_t capped = 0;      // passed, but dropped by the cap
};

// The room a placed thing takes on the plan: a rectangle centred on (x, y), hx and hy its half sizes along its own axes,
// its own x axis at `rot` plan degrees (counter-clockwise from +x).
struct MassingFoot {
    double x = 0.0, y = 0.0;
    double hx = 0.0, hy = 0.0;
    double rot = 0.0;
};
// Feet keep at least this far apart (m).
inline constexpr double kMassingPropGap = 0.1;
// Whether two feet come nearer than `gap` (>= 0): each grown by gap / 2 along its own axes, they overlap with positive
// area (the separating-axis test; touching doesn't count). A foot with no area (hx or hy 0) is still a line or a point.
// No allocation.
FTCORE_API bool massingFeetOverlap(const MassingFoot& a, const MassingFoot& b, double gap);
// A placed prop's foot: the rule's footX by footY turned as massingPropPlacement turns the mesh (rot = face - turn: the
// prop's facing less the rule's engine-yaw turn) and centred where it centres the mesh's bounds: on the prop's point, or
// for a Wall prop set out along its facing by half its depth there plus the rule's inset. No allocation.
FTCORE_API MassingFoot massingPropFoot(const MassingProp& prop, const MassingPropRule& rule);

// Whether a box `include` marks shows a built face within `reach` m (>= 0) of the plan point (x, y) at a floor `floor`:
// a box whose footprint lies within reach (massingFootprintGap), whose lowest top corner stands kMassingPropWallHeight
// over the floor and whose bottom lies under floor + kMassingPropBody, so a wall a walker would pass, not a roof or a
// bridge overhead. Sloped boxes never count. No allocation.
FTCORE_API bool massingBuiltFaceWithin(const std::vector<MassingBox>& boxes, const std::vector<bool>& include, double x,
                                       double y, double floor, double reach);
// How far (m) the plan point (x, y), at a floor `floor`, lies from the nearest kerb or frontage line: a kerb line is
// either edge of a street's carriageway where it has pavements (massingCarriageway, MassingStreet::pavement > 0), the
// distance being |d - carriageway / 2| with d the distance to its polyline (a street of one point, a circle); a frontage
// line is the footprint edge of a box massingBuiltFaceWithin would count (0 inside it). Infinity with neither. No
// allocation.
FTCORE_API double massingPropLineGap(const std::vector<MassingStreet>& streets, const std::vector<MassingBox>& boxes,
                                     const std::vector<bool>& include, double x, double y, double floor);

// A prop on the ground is steep when the ground steps more than kMassingPropMaxStep (m) between its spot and any of four
// points kMassingPropFoot out along the plan's axes; one on a box, when the box is sloped.
inline constexpr double kMassingPropMaxStep = 0.3;
inline constexpr double kMassingPropFoot = 0.5;
// A Wall prop's face must stand this high (m) over the floor at its foot.
inline constexpr double kMassingPropWallHeight = 2.5;
// The street's own floor, at the centreline, is read from tops no higher than this (m) over the ground there.
inline constexpr double kMassingPropDeckReach = 6.0;
// A box blocks a kerb prop when it stands over the prop's floor (its top over floor + kMassingStandClear) and its bottom
// lies under floor + this (m): a bridge high overhead doesn't.
inline constexpr double kMassingPropBody = 2.5;

// Sets each rule's props along the streets of its kinds, in rule order, then street order, then along each street.
//
// Spots lie every `spacing` m along a street from `phase` on, up to and including its last point; a street with no
// length (one point, or every point the same) has none. At a spot, the street's direction is its segment's (the next
// segment's at a point between two), and left and right are a quarter turn either way from it. The street's floor there
// is massingFloorAt's at the centreline over the ground (massingGroundAt), from tops no higher than kMassingPropDeckReach
// over it; a prop's floor is massingFloorAt's at its own foot from tops no higher than kMassingPropMaxStep over the
// higher of the ground there and the street's floor, so it stands on the street's deck or level, never on a roof.
// - A kerb prop (Left, Right, Both, Stagger) stands width / 2 - inset out from the centreline (0 when the inset is
//   larger), facing back toward the centreline. It is blocked when a box `include` marks lies within `clear` of it
//   (massingFootprintGap; with clear 0, holding it) and stands over its floor (kMassingPropBody), and is in the road
//   when it lies nearer than half its width to the centreline of any other street.
// - A Wall prop looks out from the centreline to the left, then the right, as far as width / 2 + reach, for the nearest
//   footprint `include` marks that the ray enters from outside; the first side whose face is in reach and whose box's
//   lowest top corner stands kMassingPropWallHeight over the floor at the face's foot (kMassingPropFoot out from it)
//   takes the prop, at the face, facing straight out of it. Its foot is where its floor, steepness and wetness are read;
//   it is in the road as a kerb prop is, at `inset` out from the face.
// - In the road means nearer than half its carriageway (massingCarriageway) to the centreline of another street, so a
//   prop may stand on another street's pavement.
// - With a rule's line over 0 it is open when no built face stands within line of its point at its floor
//   (massingBuiltFaceWithin).
// - It is wet when its floor lies at or under dryAbove. z is the floor plus dz.
// - It is crowded when its foot (massingPropFoot) comes within kMassingPropGap of a foot in `taken` (when given), of a
//   prop already placed by an earlier rule or of one this rule passed before it (massingFeetOverlap); a foot with no area
//   is never crowded and crowds nothing.
// A spot fails on the first of blocked (or no wall), open, in the road, steep, wet and crowded, and counts there. The
// spots that pass, when over `cap` (not 0), are thinned to `cap` of them evenly: the k-th kept is the
// floor(k * passed / cap)-th. `out` and `counts` (one per rule) are cleared first. A grid massingGroundAt reads as empty
// gives a flat ground at 0. Allocates: build time only.
FTCORE_API void massingPlaceProps(const std::vector<MassingStreet>& streets, const std::vector<MassingPropRule>& rules,
                                  const std::uint16_t* grid, std::size_t samplesX, std::size_t samplesY, double spacing,
                                  double originX, double originY, const std::vector<MassingBox>& boxes,
                                  const std::vector<bool>& include, double dryAbove, std::vector<MassingProp>& out,
                                  std::vector<MassingPropCounts>& counts, const std::vector<MassingFoot>* taken = nullptr);

// Places a mesh for a prop, from its bounds in its own axes (m), in the engine frame of a square `side` metres across:
// the mesh turned so its +X faces the prop's way, `turnDeg` more, its bounds' centre over the prop's point and its bottom
// on the prop's z. A Wall prop's bounds are set out from the face by their own depth along the facing, plus `inset`, so
// their back meets the face; a negative inset sinks their back that far into the wall (a shop front deeper than it
// shows). False, leaving `out` alone, on inverted bounds.
FTCORE_API bool massingPropPlacement(const MassingProp& prop, double side, const MassingVec& meshMin,
                                     const MassingVec& meshMax, double turnDeg, double inset, MassingKitPlace& out);

} // namespace ftcore
