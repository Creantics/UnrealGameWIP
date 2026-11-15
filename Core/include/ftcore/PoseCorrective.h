#pragma once

// Pose correctives for dressed pieces (Data/Anim/pose_correctives.json): a morph target on a garment that a joint's
// flexion drives, so cloth that plain skinning folds badly (the seat and the backs of the knees in a crouch) takes
// a shape settled in that pose. The table is general: any piece that has a listed morph gets it; a piece that has
// none costs nothing.
//
//   ftcore::PoseCorrectiveTable table;
//   if (!ftcore::importPoseCorrectives(text, table, &error, &warnings)) ...;   // allocates: at load
//   float deg = ftcore::flexionDegrees(refDir, curDir, lateral);              // no allocation
//   float w = ftcore::correctiveWeight(deg, e.startDeg, e.endDeg);            // no allocation
//
// The flexion of a joint is the signed angle, about a lateral axis, between the joint-to-child direction in the
// reference (bind) pose and the same direction now. Both are given with the joint's parent turned back to its
// reference orientation, so a pelvis tilt alone does not count as flexion. Positive is the right-hand turn about
// the lateral axis.
//
// pose_correctives.json:
//   { "version": 1,
//     "correctives": [ { "morph": "FT_TrouserCrouch_L", "side": "L", "bone": "thigh_l", "child": "calf_l",
//                        "lateral": "x", "start_deg": 35, "end_deg": 95 } ] }
//   morph, bone and child are 1..128 letters, digits and '_'; side is "L", "R" or "mid"; lateral is one of
//   x, y, z, -x, -y, -z (the component-space axis); start_deg < end_deg, both within -180..180. The weight is 0 up
//   to start_deg, 1 from end_deg, smoothstep between. A morph listed twice (case-insensitive), a bone equal to its
//   child, a bad field or a missing or wrong version is an error and the whole table is refused. `about` at the
//   top level and in an entry is a note; any other unknown key is a warning.

#include "ftcore/Api.h"

#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

struct Dir3 {
    float x = 0.f, y = 0.f, z = 0.f;
};

struct PoseCorrective {
    std::string morph; // the morph target's name on the piece
    char side = 'M';   // 'L', 'R' or 'M' (mid)
    std::string bone;  // the joint whose flexion drives it
    std::string child; // the bone below it: joint-to-child is the measured direction
    Dir3 lateral{1.f, 0.f, 0.f};
    float startDeg = 0.f;
    float endDeg = 90.f;
};

struct PoseCorrectiveTable {
    std::vector<PoseCorrective> entries; // in file order
};

// The signed angle in degrees about `lateral` from `ref` to `cur`, both projected onto the plane normal to
// `lateral`. 0 when either projection (or `lateral`) is shorter than 1e-6. No allocation.
FTCORE_API float flexionDegrees(const Dir3& ref, const Dir3& cur, const Dir3& lateral);

// 0 at or below startDeg, 1 at or above endDeg, smoothstep between; a step at startDeg when endDeg <= startDeg.
// No allocation.
FTCORE_API float correctiveWeight(float deg, float startDeg, float endDeg);

// On failure `out` is empty and *error lists each problem with its path, one per line.
FTCORE_API bool importPoseCorrectives(std::string_view json, PoseCorrectiveTable& out, std::string* error = nullptr,
                                      std::vector<std::string>* warnings = nullptr);

} // namespace ftcore
