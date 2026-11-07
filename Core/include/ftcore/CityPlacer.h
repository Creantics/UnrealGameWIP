#pragma once

// The city building placer: stands buildings along a street plan's frontages on a height grid, each
// facing its street, and counts every candidate it turns down by its reason (never silently). An
// offline build tool (tools/ftcore_place.cpp runs it): it allocates while it works and nothing here
// runs per frame. Ids, class names and metres only.
//
//   ftcore::PlacerConfig config;                        // Data/Field/city_placer.json
//   if (!ftcore::importPlacerConfig(json, config, &error)) ...;
//   ftcore::PlacerGrid grid;                            // heights, roof caps and no-build bits
//   std::vector<ftcore::PlacerZone> zones;              // importPlacerZones (CSV)
//   std::vector<ftcore::PlacerFrontage> frontages;      // importPlacerFrontages (CSV)
//   ftcore::PlacerResult result;
//   if (!ftcore::placeCity(config, grid, zones, frontages, result, &error)) ...;
//
// Frame: MassingPlan.h's plan frame. x east and y north in metres; rotations in degrees
// counter-clockwise from +x.
//
// Footprints are read at the outer wall planes; balconies, galleries and eaves stand outboard of them
// as the class's envelope growth (front_growth at the street side, rear_growth at the back), and every
// test (grid, no-build, slope, roof cap, spacing) is made on the grown envelope. A building's height is
// read to the roof deck (flat roof) or the ridge (pitched roof); its top adds what stands above that
// (parapets, rails, a stair tower's head), and the roof cap is tested on the top.
//
// The walk. Each frontage is a segment (x0, y0) to (x1, y1) along a street's centre line, with the
// street's half-width and the sides that take buildings: left is +90 degrees counter-clockwise from the
// walking direction, right is -90. Each side is walked on its own seeded stream (config seed, the
// frontage's id and row, the side), from end_margin to the segment's length less end_margin. Per slot:
// a class by the zone's class weights, a variant by the variants' weights, a lead of 0..the class's
// jitter, and an offer drawn against the zone's share (a slot not offered is "thinned", an empty lot of
// the variant's width). An offered slot is a candidate: its front wall plane is set back the half-width
// plus the setback margin plus the class's front growth from the line, centred on the slot, facing the
// street, then turned by the zone's and the frontage's axis offsets about its own centre. After a placed
// building the next slot starts its width on, plus the larger of the two classes' gaps, plus the lead;
// after a turned-down candidate the walk moves on reject_step metres; a slot that would run past the
// segment's end ends the walk.
//
// A candidate's tests, in order; the first that fails is its reason:
//   off_grid  an envelope corner lies outside the grid's samples
//   no-build  a sample under the envelope has a no-build bit; the lowest bit set among them names the
//             reason (water, band, shoulder, carriageway, hold, other)
//   slope     the steepest sample under the envelope is over the zone's limit. When the zone allows
//             benches, the class may stand on one and the slope is no more than bench_max_slope_deg,
//             the building is placed on a bench instead (bench = true)
//   roof_cap  ground z (the highest sample under the envelope) + top is over a sample's max roof by
//             more than kPlacerRoofTolerance. Then the class's lower variants (top under this one's)
//             are tried in a seeded order weighted by their weights (weight-0 variants last, tallest
//             first), each through every test from the start; the first that passes stands in
//             (lowered = true). If none does, the candidate counts as roof_cap
//   overlap   the envelope comes nearer than the larger of the two classes' gaps to an envelope already
//             placed (both grown by half that gap, then tested with massingFootprintsOverlap, so
//             envelopes exactly a gap apart pass)
// A sample is under an envelope when its point lies inside it (edges included); an envelope that holds
// no sample uses the sample nearest its centre.

#include "ftcore/Api.h"
#include "ftcore/MassingPlan.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

// No-build reasons: one bit each in a no-build cell; 0 is buildable. Bits 64 and 128 read as Other.
enum class NoBuild : std::uint8_t { Water = 1, Band = 2, Shoulder = 4, Carriageway = 8, Hold = 16, Other = 32 };

enum class PlaceReject : std::uint8_t { OffGrid, Water, Band, Shoulder, Carriageway, Hold, OtherMask, Slope, RoofCap, Overlap, Variety };
inline constexpr std::size_t kPlaceRejectCount = 11;
// "off_grid", "water", "band", "shoulder", "carriageway", "hold", "other", "slope", "roof_cap", "overlap",
// "variety".
FTCORE_API std::string_view placeRejectName(PlaceReject reason);

// A roof may stand this far over its cap (metres): well under the 1/128 m step of the 16-bit rasters.
inline constexpr double kPlacerRoofTolerance = 0.001;

// The ground and its rasters: samples (x0 + i * cell, y0 + j * cell), i < nx running east, j < ny
// running north, stored row by row from the south (index j * nx + i). Each raster sample stands for
// the cell around its point.
struct PlacerGrid {
    double x0 = 0.0, y0 = 0.0;
    double cell = 1.0;
    std::size_t nx = 0, ny = 0;
    std::vector<float> height;          // nx * ny, metres above the datum
    std::vector<float> maxRoof;         // empty (no caps), or nx * ny absolute z; +infinity where uncapped
    std::vector<std::uint8_t> noBuild;  // empty (all buildable), or nx * ny NoBuild bits

    // Sizes agree, the cell is positive and the origin finite.
    FTCORE_API bool valid() const;
    // The ground's steepest slope at a sample, degrees: central differences on each axis (one-sided
    // at the grid's edge), the gradient's magnitude as an angle.
    FTCORE_API double slopeDegAt(std::size_t i, std::size_t j) const;
};

struct PlacerPoint {
    double x = 0.0, y = 0.0;
};

// One footprint-and-storey class of a building class.
struct PlacerVariant {
    std::string name;          // unique in its class
    double width = 0.0;        // along the street, at the outer wall planes (m)
    double depth = 0.0;        // away from the street, at the outer wall planes (m)
    std::uint16_t storeys = 0;
    double height = 0.0;       // to the roof deck or the ridge (m)
    double top = 0.0;          // overall, what stands above the deck included (m); >= height
    double weight = 1.0;       // pick weight; 0: only as a lower variant under a roof cap
};

struct PlacerClass {
    std::string name;
    double frontGrowth = 0.0;  // envelope outboard of the front wall plane (m)
    double rearGrowth = 0.0;   // envelope outboard of the rear wall plane (m)
    double gap = 0.0;          // clear distance this class keeps between its envelope and any other (m)
    double jitter = 0.0;       // the walk's lead before each of its buildings: 0..jitter (m)
    bool benchOk = false;      // may stand on a bench
    std::vector<PlacerVariant> variants;
};

// The variety check's thresholds (checkVariety, checkLandmarkReach).
struct VarietyParams {
    double radius = 300.0;         // disc radius (m)
    double spacing = 100.0;        // disc centres' grid (m)
    double maxShare = 0.25;        // the most any footprint-and-storey class may hold of a disc
    std::uint32_t minClasses = 6;  // the fewest footprint-and-storey classes a disc may hold
    std::uint32_t minBuildings = 20; // a disc holding fewer is counted but not judged
    double maxSlopeDeg = 4.0;      // the buildings counted stand on ground under this slope
    double landmarkReach = 500.0;  // every point within this of a landmark (m)
};

struct PlacerConfig {
    std::uint32_t seed = 1;
    double setbackMargin = 1.0;     // between the street's edge and the envelope's front (m)
    double endMargin = 0.0;         // kept clear at each end of a frontage (m)
    double rejectStep = 4.0;        // the walk moves on this far after a turned-down candidate (m), > 0
    double benchMaxSlopeDeg = 30.0; // the steepest ground a bench is cut into
    VarietyParams variety;
    std::vector<PlacerClass> classes;

    // The number of variants over all classes.
    FTCORE_API std::size_t variantCount() const;
};

struct PlacerZone {
    std::string id;                    // A-Z, a-z, 0-9, '_', '-', '.'; building ids are <id>_<n>
    double share = 1.0;                // the chance each slot is offered (0..1)
    double targetPerKm2 = 0.0;         // reported beside the result, never enforced; 0: none
    double slopeMaxDeg = 20.0;         // the steepest ground a building stands on without a bench
    bool bench = false;                // benches may be cut here
    double axisDeg = 0.0;              // turns this zone's buildings about their centres
    double buildableM2 = 0.0;          // coverage's denominator, the caller's; 0: no coverage
    std::vector<double> classWeights;  // one per config class
};

struct PlacerFrontage {
    std::string id;
    std::string zone;                  // a zone's id
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    double halfWidth = 0.0;            // from the line to the street's edge (m)
    bool left = false, right = false;  // the sides that take buildings
    double axisDeg = 0.0;              // turns this frontage's buildings about their centres, after the zone's
};

struct PlacedBuilding {
    std::string id;                    // <zone>_<n>, n from 1 in placement order within the zone
    std::uint32_t zone = 0;            // index into the zones
    std::uint32_t frontage = 0;        // index into the frontages
    std::uint16_t cls = 0, variant = 0; // indexes into the config
    std::int8_t side = 1;              // +1 left of the frontage, -1 right
    bool bench = false;                // stands on a bench
    bool lowered = false;              // a lower variant stood in under a roof cap
    double cx = 0.0, cy = 0.0;         // the footprint's centre at the outer wall planes
    double rot = 0.0;                  // the facing (the front's outward normal), (-180, 180]
    double groundZ = 0.0;              // the highest ground under the envelope: the building stands here
    double groundMinZ = 0.0;           // the lowest
    double roofZ = 0.0;                // groundZ + the variant's top
    double slopeDeg = 0.0;             // the steepest ground under the envelope
};

struct PlacerZoneStats {
    std::array<std::uint32_t, kPlaceRejectCount> rejects{};
    std::uint32_t candidates = 0;      // offered slots tested
    std::uint32_t placed = 0;
    std::uint32_t thinned = 0;         // slots the share left empty
    std::uint32_t lowered = 0;         // placed as a lower variant under a roof cap
    std::uint32_t substituted = 0;     // placed as another variant because the variety guard turned the pick down
    std::uint32_t benched = 0;         // placed on a bench
    double bodyArea = 0.0;             // placed footprints at the outer wall planes (m^2)
    double envelopeArea = 0.0;         // placed envelopes (m^2)

    FTCORE_API std::uint32_t rejected() const;
};

struct PlacerResult {
    std::vector<PlacedBuilding> placed;
    std::vector<PlacerZoneStats> zones; // one per input zone
};

// A placed building's footprint as a massing box (the building's id, kind "building"): with envelope
// its grown envelope, else its outer wall planes. u runs along the facing (sx is the depth), w across
// it (sy the width), rot is the facing; z0 is groundZ and z1 roofZ.
FTCORE_API MassingBox placerFootprint(const PlacerConfig& config, const PlacedBuilding& b, bool envelope);

// Placed area over the zone's buildableM2 (0 when that is 0): envelopes or wall-plane footprints.
FTCORE_API double placerCoverage(const PlacerZoneStats& stats, const PlacerZone& zone, bool envelope);

// The variety the walk holds while it places, in the discs checkVariety judges (config.variety's radius,
// max share and min buildings). It counts what checkVariety counts: a guarded zone's buildings standing
// on ground under maxSlopeDeg, by body centre. No such building is placed if it would leave any disc
// holding more of its footprint-and-storey class than floor(maxShare * max(n, minBuildings)), n being
// the disc's count with it. Counts only grow, so every disc that ends judged ends within maxShare. A
// pick the guard turns down tries the zone's other variants (every class it weighs) in a seeded order
// by weight; if none stands, the slot is a variety reject. The class minimum is checked, not held.
struct PlacerVarietyGuard {
    std::vector<PlacerPoint> centres;  // the discs' centres: placerFlatPoints over the checked bounds
    std::vector<bool> zones;           // one per zone: held
};

// Runs the walk. False with a reason, and `out` empty, on inputs that don't fit together: an invalid
// grid, a zone whose class weights don't match the config's classes, a frontage naming no zone, a
// frontage of no length, a variant without size, a guard whose zone flags don't match the zones.
// Same inputs, same output. With no guard the walk holds no variety.
FTCORE_API bool placeCity(const PlacerConfig& config, const PlacerGrid& grid, const std::vector<PlacerZone>& zones,
                          const std::vector<PlacerFrontage>& frontages, PlacerResult& out, std::string* error = nullptr,
                          const PlacerVarietyGuard* guard = nullptr);

// --- The variety check ---------------------------------------------------------------------------------

struct VarietyDisc {
    PlacerPoint centre;
    std::uint32_t buildings = 0;       // counted buildings whose centre lies in the disc
    std::uint32_t classes = 0;         // distinct footprint-and-storey classes among them
    std::uint32_t topCount = 0;        // buildings of the commonest one
    std::uint16_t topCls = 0, topVariant = 0;
    bool judged = false;               // buildings >= minBuildings
    bool pass = true;                  // not judged, or the share and class rules hold
};

struct VarietyResult {
    std::vector<VarietyDisc> discs;
    std::uint32_t counted = 0;         // buildings on ground under maxSlopeDeg
    std::uint32_t judged = 0;
    std::uint32_t failedShare = 0;     // judged discs whose commonest class is over maxShare
    std::uint32_t failedClasses = 0;   // judged discs with fewer than minClasses classes
    bool pass = false;                 // at least one disc judged and none failed
};

// Over the buildings standing on ground under params.maxSlopeDeg, in the disc of params.radius around
// each centre: the footprint-and-storey classes (cls, variant) and their counts.
FTCORE_API void checkVariety(const std::vector<PlacedBuilding>& placed, const std::vector<PlacerPoint>& centres,
                             const VarietyParams& params, VarietyResult& out);

struct LandmarkReach {
    std::uint32_t points = 0;
    std::uint32_t uncovered = 0;       // points farther than the reach from every landmark
    double worst = 0.0;                // the largest nearest-landmark distance (m); +infinity with no landmarks
    PlacerPoint worstAt;
    bool pass = false;                 // points and landmarks given, every point covered
};

FTCORE_API void checkLandmarkReach(const std::vector<PlacerPoint>& points, const std::vector<PlacerPoint>& landmarks,
                                   double reach, LandmarkReach& out);

// The grid points (minX + k * spacing, minY + m * spacing) inside the box and on the grid whose nearest
// sample's slope is under maxSlopeDeg and holds no water bit: the centres and points for the two checks
// above on flat land. `out` is cleared first.
FTCORE_API void placerFlatPoints(const PlacerGrid& grid, double minX, double minY, double maxX, double maxY,
                                 double spacing, double maxSlopeDeg, std::vector<PlacerPoint>& out);

// --- Reading and writing ---------------------------------------------------------------------------------
// Each reader clears its output first and, on failure, leaves it empty with a reason naming the line
// or the member. The formats are documented in the build's placer IO notes; in short:

// Data/Field/city_placer.json (version 1): the walk's tunables, the variety thresholds and the classes.
// A variant's "storeys" is a count or [min, max] (one variant per count, named <name>_s<n>); its height
// is storeys * storey_m + rise_m and its top height + top_extra_m (class value unless the variant has
// its own).
FTCORE_API bool importPlacerConfig(std::string_view json, PlacerConfig& out, std::string* error = nullptr);

// The height raster's sidecar JSON: samples_x, samples_y, spacing_m and origin_of_sample_0_0_m [x, y]
// (other members skipped). Sets the grid's size and frame and clears its rasters.
FTCORE_API bool importPlacerGridMeta(std::string_view json, PlacerGrid& out, std::string* error = nullptr);

// Raw rasters for a grid whose size is set: little-endian u16 heights h = (v - 32768) / 128; the same
// for max roof with 65535 as +infinity (uncapped); u8 no-build bits. False on a size that doesn't match.
FTCORE_API bool placerDecodeHeights(const std::uint8_t* bytes, std::size_t size, PlacerGrid& grid, std::string* error = nullptr);
FTCORE_API bool placerDecodeMaxRoof(const std::uint8_t* bytes, std::size_t size, PlacerGrid& grid, std::string* error = nullptr);
FTCORE_API bool placerDecodeNoBuild(const std::uint8_t* bytes, std::size_t size, PlacerGrid& grid, std::string* error = nullptr);

// Zone CSV: id, share, slope_max_deg, bench, buildable_m2, weights ("class:w;class:w", every name a
// config class, the rest weigh 0); optional axis_deg, target_per_km2. Ids unique.
FTCORE_API bool importPlacerZones(std::string_view csv, const PlacerConfig& config, std::vector<PlacerZone>& out,
                                  std::string* error = nullptr);
// Frontage CSV: id, zone, x0, y0, x1, y1, half_width, sides (left, right or both); optional axis_deg.
FTCORE_API bool importPlacerFrontages(std::string_view csv, std::vector<PlacerFrontage>& out, std::string* error = nullptr);
// Point CSV (landmarks): id, x, y.
FTCORE_API bool importPlacerPoints(std::string_view csv, std::vector<PlacerPoint>& out, std::string* error = nullptr);

// The placed table as CSV, one row per building, header first (see the IO notes for the columns).
FTCORE_API std::string placerCsv(const PlacerConfig& config, const std::vector<PlacerZone>& zones,
                                 const std::vector<PlacerFrontage>& frontages, const PlacerResult& result);

// The summary's zone lines: per zone its counts and rejects by reason, then its coverage (one
// "zone ..." and one "coverage ..." line each, key=value tokens).
FTCORE_API std::string placerZoneLines(const std::vector<PlacerZone>& zones, const PlacerResult& result);
// The variety check's lines: one "variety ..." line, then a "disc ..." line per disc.
FTCORE_API std::string placerVarietyLines(const PlacerConfig& config, const VarietyResult& variety);

} // namespace ftcore
