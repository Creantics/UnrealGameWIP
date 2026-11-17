#pragma once

// The plain parts of the clip import (FTImportAnim) and the pose stills (FTPoseShots): their arguments, the bone names
// an FBX file animates, and the checks made on an import. No engine types.

#include "ftcore/Api.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore::animimport {

// A socket to put on a skeleton: "name,bone,x,y,z[,roll,pitch,yaw]" is bone-local (cm, degrees);
// "name,bone,cs,x,y,z[,ax,ay,az]" is a point in the reference pose's component space, with an optional direction
// there for the socket's +X; the importer turns it into the bone's frame.
struct SocketSpec {
    std::string name;
    std::string bone;
    bool componentSpace = false;
    double x = 0.0, y = 0.0, z = 0.0;
    // Bone-local rotation (degrees), when !componentSpace.
    double roll = 0.0, pitch = 0.0, yaw = 0.0;
    // Component-space +X direction, when componentSpace and hasAxis.
    bool hasAxis = false;
    double ax = 1.0, ay = 0.0, az = 0.0;
};

// False (and `error` says why) when the text has the wrong number of fields, an empty name or bone, a field that is
// not a finite number, or a component-space axis of zero length.
FTCORE_API bool ParseSocketSpec(std::string_view text, SocketSpec& out, std::string* error);

// Comma-separated times in seconds ("0.3,2.3,3.8"): every one a finite number at or above zero. Empty text is an error.
FTCORE_API bool ParseTimes(std::string_view text, std::vector<double>& out, std::string* error);

// A camera around the subject: yaw from the subject's facing (0 in front, looking back at it; positive toward the
// subject's left), pitch looking down (degrees).
struct ShotView {
    std::string name;
    double yawDeg = 0.0;
    double pitchDeg = 0.0;
};

// "front", "tq" (front three-quarter, 40 degrees to the subject's left), "tqr" (the same to its right), "side" (its
// left, 90), "sider", "back", or "name:yaw:pitch" for any other view; comma-separated. The presets look down 6 degrees.
FTCORE_API bool ParseViews(std::string_view text, std::vector<ShotView>& out, std::string* error);

// The frame nearest t at fps (0 for t <= 0).
FTCORE_API int FrameAt(double seconds, double fps);

// The still's file name: "<stem>_t<seconds, 2 decimals>_<view>.png", e.g. "A_Clip_t2.30_front.png".
FTCORE_API std::string StillName(std::string_view stem, double seconds, std::string_view view);

// The bones an FBX file holds: the names of its LimbNode models, in file order, for a binary FBX (7.x, 32- or 64-bit
// records) or an ASCII one. Only the strings are read, so compressed arrays don't matter. Empty when there are none or
// the bytes are not FBX.
FTCORE_API std::vector<std::string> FbxLimbNames(const std::uint8_t* data, std::size_t size);

// The names in `wanted` that are not in `have`, in `wanted`'s order (exact match).
FTCORE_API std::vector<std::string> MissingNames(const std::vector<std::string>& wanted, const std::vector<std::string>& have);

// Whether an import kept enough of the file's bones as tracks: tracks >= share * fileBones (true when fileBones is 0).
FTCORE_API bool TrackShareOk(int tracks, int fileBones, double share);

} // namespace ftcore::animimport
