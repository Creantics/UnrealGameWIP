#pragma once

// The road graph: the drivable streets as lanes (centre lines with a direction, width, speed and one-way flag),
// junctions with their signal phases, crossing polygons and parking bays, read and written as one CSV, and the
// drivable loop of a street net. For the traffic simulation later and the vehicle placer now. Plain C++20; plan frame,
// metres. Left-hand traffic. Allocates: build time only.

#include "ftcore/Api.h"
#include "ftcore/MassingPlan.h"
#include "ftcore/Roads.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

inline constexpr double kRoadGraphSnap = 2.0; // m: points this near are one junction

struct RoadGraphLane {
    std::string id, street;
    int dir = 1;                 // +1 along the street's points, -1 against them
    double width = 0.0;          // m
    double speedKmh = 0.0;
    bool oneWay = false;
    std::vector<RoadPoint> pts;  // the centre line in the way of travel
};
struct RoadGraphJunction {
    std::string id;
    RoadPoint at;
    std::vector<std::string> streets; // the streets meeting there
    std::string phases;               // "A:ST-1+ST-2;B:ST-3"; empty: unsignalised
};
struct RoadGraphCrossing {
    std::string id, street, junction; // the street it crosses, the junction it serves ("" none)
    std::array<RoadPoint, 4> pts{};
};
struct RoadGraphBay {
    std::string id, street;
    int dir = 1;          // the way a car parked in it faces, as a lane's
    double width = 0.0;   // m: across the street
    std::array<RoadPoint, 4> pts{};
};
struct RoadGraph {
    std::vector<RoadGraphLane> lanes;
    std::vector<RoadGraphJunction> junctions;
    std::vector<RoadGraphCrossing> crossings;
    std::vector<RoadGraphBay> bays;
};

// The CSV's comment header (each line starting "# "), naming every column; roadGraphWrite starts with it.
FTCORE_API const char* roadGraphHeader();
// Writes the graph: the header comment, the column row, then lanes, junctions, crossings and bays.
FTCORE_API std::string roadGraphWrite(const RoadGraph& graph);
// Reads what roadGraphWrite writes (any row order). `out` is cleared first. False, naming the line, on any error.
FTCORE_API bool importRoadGraph(std::string_view csv, RoadGraph& out, std::string* error = nullptr);

// A street's lanes: its carriageway (massingCarriageway, or `pavementShare` of the width each side when it has no
// pavements) split into max(1, floor(carriageway / 2 / 3.5)) lanes each way, no wider than kRoadLaneWidth, offset to
// the left of each way (left-hand traffic), mitred at bends. One way: all lanes along its points. Appends.
FTCORE_API void roadGraphStreetLanes(const MassingStreet& street, double speedKmh, bool oneWay, double pavementShare,
                                     RoadGraph& out);
// The junctions of a street net: points within `snap` of each other, an end within `snap` of another street, and
// crossing segments. Each with at least three arms gets two phases by bearing (the first street's axis A, the rest B)
// and a zebra across each arm, kRoadZebraOff past the widest crossing street's kerb. Appends.
FTCORE_API void roadGraphJunctions(const std::vector<MassingStreet>& streets, double snap, double pavementShare,
                                   RoadGraph& out);
// A built road's lanes, its junctions (phases A: the road, B: the lane and the stub), its zebras and its bays. Appends.
FTCORE_API void roadGraphFromRoad(const RoadSpec& road, const RoadBuild& build, RoadGraph& out);

// The drivable loop of a street net: the net split at every junction, its bridges dropped, the largest remaining
// piece by length, and that piece's outer boundary.
struct RoadLoop {
    double length = 0.0;     // m: the outer boundary's length; 0 with no loop
    double netLength = 0.0;  // m: every street piece in the largest bridgeless piece
    std::size_t nodes = 0, edges = 0, bridges = 0;
    std::vector<RoadPoint> ring; // the boundary's points in order
};
FTCORE_API RoadLoop roadGraphLoop(const std::vector<MassingStreet>& streets, double snap);

} // namespace ftcore
