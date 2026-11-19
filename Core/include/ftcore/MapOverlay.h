// The map overlay (M), option C (the orchestrator's pick, 2026-11-17): a north-up quick map over the paused play, with
// three zoom levels, a cursor, and few words. The districts carry their names, the road under the cursor is lit along
// its whole length and named once along it, and the player's district and street sit bottom left. The clockwork relief
// world map is a separate screen.
//
// This file is the overlay's rules over a MiniMapModel (ftcore/MiniMap.h): the zoom levels and what each shows, the
// view (centre and scale, the model's metres to screen px, both y down: the model's y runs south), the names joined to
// the roads, and the label placer (candidates in, the ones that fit out, highest priority first, never overlapping,
// always upright). Nothing below the names' load allocates; the host recomputes labels when the view or cursor moves.
#pragma once

#include "ftcore/Api.h"
#include "ftcore/MiniMap.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

// The zoom levels, far to near. Far: districts and main roads; mid: streets too; near: lanes, alleys and stairs too
// (absent until the level's model holds their geometry: nothing is guessed).
enum class MapZoom : std::uint8_t { Far, Mid, Near };
inline constexpr int kMapZooms = 3;
inline constexpr float kMapZoomMPerPx[kMapZooms] = {3.f, 1.4f, 0.6f}; // m per px at 1080p
FTCORE_API bool mapRoadShown(MapRoadClass c, MapZoom z);
FTCORE_API MapZoom mapZoomIn(MapZoom z);  // one step nearer, held at Near
FTCORE_API MapZoom mapZoomOut(MapZoom z); // one step farther, held at Far
// The road the cursor picks: of the roads shown at zoom z, the one whose centre line is nearest `world`, if within
// maxM m; -1 for none. Ties keep the higher class (main over street).
FTCORE_API int mapRoadNear(const MiniMapModel& m, Vec2 world, float maxM, MapZoom z);
// The water as rectangles to fill: the model's water rectangles, then the water grid's cells merged along each row
// into runs (axis-aligned, rot 0). Runs once at load (it sizes `out`).
FTCORE_API void mapWaterRuns(const MiniMapModel& m, std::vector<MapRect>& out);

// The overlay's view: the world point at the screen's centre and the scale. North up, never rotated.
struct MapOverlayView {
    Vec2 centre{};
    float mPerPx = kMapZoomMPerPx[1];
    float screenW = 1920.f, screenH = 1080.f; // px
};
// The view at a zoom: m per px is the zoom's at 1080p, scaled for the screen's height (uiScale is the player's).
FTCORE_API MapOverlayView mapOverlayView(Vec2 centre, MapZoom z, float screenW, float screenH, float uiScale = 1.f);
FTCORE_API Vec2 mapOverlayToScreen(const MapOverlayView& v, Vec2 world);
FTCORE_API Vec2 mapOverlayFromScreen(const MapOverlayView& v, Vec2 screen);
// The centre held so the view never leaves the model's area (a side larger than the area centres on it).
FTCORE_API Vec2 mapOverlayClampCentre(const MiniMapModel& m, const MapOverlayView& v);
// The view at a zoom over this model: as mapOverlayView, except Far, which shows the model's whole area (the side that
// binds fills the screen less kMapFarMargin of it each side; the other is letterboxed) and is never nearer than
// kMapFarOverMid times Mid's scale, so zooming out always widens the view.
inline constexpr float kMapFarMargin = 0.04f;
inline constexpr float kMapFarOverMid = 1.25f;
FTCORE_API MapOverlayView mapOverlayViewIn(const MiniMapModel& m, Vec2 centre, MapZoom z, float screenW, float screenH,
                                           float uiScale = 1.f);
// The side the sea lies past the model's area, for the letterbox: +1 east, -1 west, 0 neither (an edge column of the
// water grid more than half water; east wins a tie).
FTCORE_API int mapSeaSide(const MiniMapModel& m);

// Which buildings are enterable: each door marks the one building nearest it (distance outside the footprint, ties
// to the nearer centre), if that is within `reach` m (doors stand just off the wall line). Terraced neighbours share
// walls, so a door never lights two. One flag per model building, 1 enterable. Runs once at load (it sizes `out`).
FTCORE_API void mapEnterableBuildings(const MiniMapModel& m, std::vector<std::uint8_t>& out, float reach = 1.f);

// The names, joined to the roads by id at load (this allocates; it runs once). The table's text is CSV with the header
// id,kind,name,sign_form,district; rows with kind "district" are the districts (id = the district's id), and a road
// row's district column joins the road to its district. A road with no row has no name; rows that name no road in the
// model are skipped (counted in `unmatched`: water, bridges and squares have no road geometry in the model), unless the
// table has the optional columns x and y and the row fills them: then it is a place, named at that point. x, y are plan m
// when `planSide` > 0 (turned to engine m by massingEnginePoint), else engine m. A line kind (street, lane, stair,
// bridge, *_walk...) off the model is never a point name and stays unmatched; a zone row (id Z + digits) always is one.
// A district row's own x, y is its anchor; without one the anchor is the mean of its roads.
// A name is Water when any of its rows' kind is river, lake, canal, sea, harbour or bay (drawn in italics), else a Place
// when its rows are places only, else a Road.
inline constexpr std::uint16_t kMapNoName = 0xFFFF;
enum class MapNameKind : std::uint8_t { Road, Water, Place };
struct MapNames {
    std::vector<std::string> names;      // each distinct name once
    std::vector<std::string> signForms;  // per name: its sign form ("" none; the glyphs are drawn, not typed)
    std::vector<MapNameKind> nameKind;   // per name
    struct Spot {
        std::uint16_t name = kMapNoName;
        Vec2 at{};
        bool green = false; // its row's kind is open public ground (mapGreenKind): the block it stands in is a park
    };
    std::vector<Spot> places; // the rows placed at a point (x, y), in table order
    std::vector<std::uint16_t> roadName; // per model road: index into names, or kMapNoName
    struct District {
        std::string id;
        std::uint16_t name = kMapNoName;
        Vec2 anchor{};      // its row's x, y, else the mean of its named roads' midpoints (its label's place at far zoom)
        bool hasAnchor = false;
    };
    std::vector<District> districts;
    std::vector<std::uint16_t> roadDistrict; // per model road: index into districts, or kMapNoName
    std::size_t unmatched = 0;
    // Per model road: its row's own point (x, y), when the row gave one: the road's anchor on a shared run (below).
    std::vector<Vec2> roadAnchor;
    std::vector<std::uint8_t> roadHasAnchor;
    // Per model road: the bounds of its centre line (x0, y0, x1, y1, engine m), measured once at load.
    std::vector<float> roadBox;
    // A stretch of one road that a zone row names (mapLoadNames: a shared straight run, split between the anchors).
    struct Segment {
        std::uint32_t road = 0;
        float s0 = 0.f, s1 = 0.f;            // m along the road's centre line from its first point, s0 < s1
        std::uint16_t name = kMapNoName;
        std::uint16_t district = kMapNoName; // into districts: the zone row's own district column
        float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f; // its bounds (engine m)
    };
    std::vector<Segment> segments;           // by road, then s0; never overlapping
    std::vector<std::uint32_t> roadSegments; // per model road, plus one: its first segment (the next road's is its end)
    std::uint16_t seaName = kMapNoName;      // the first sea, bay or harbour row's name: named once, over the open sea
};
// Kinds of open public ground: a block (MapLook) holding a point of one of these is a park. park, green, garden,
// grove, lawn, orchard, cemetery, square, plaza.
FTCORE_API bool mapGreenKind(std::string_view kind);
// The table's x, y are plan metres (the build tables' frame) when planSide > 0, turned to engine metres with
// massingEnginePoint; else they are read as engine metres. A district row's own x, y, when given, is its anchor.
FTCORE_API bool mapLoadNames(const MiniMapModel& m, std::string_view csv, MapNames& out, std::string* error = nullptr,
                             double planSide = 0.0);
// The name of the road at index `road` (kMapNoName for none or out of range).
FTCORE_API std::uint16_t mapRoadName(const MapNames& n, int road);
// The district of the road at index `road`: index into n.districts, or kMapNoName.
FTCORE_API std::uint16_t mapRoadDistrict(const MapNames& n, int road);

// Naming per named segment. A zone row (id Z + digits) of a road kind (mapRoadClass knows it) whose point lies within
// kMapZoneReachM of a named road's straight run (the run rule of mapLabelAlong, on the road's own centre line: pieces
// that each head within kMapAlongBendDeg of the run's first) shares that run with the road. The run is split between
// its anchors, ordered along it: the road's own anchor (its row's x, y, when that lies within kMapZoneReachM of the
// run) and each zone's point; each split falls at the midpoint between two neighbouring anchors, and each part takes its
// anchor's name. A road with no anchor on the run keeps the half of the run away from its zones (the run's midpoint
// splits it; several zones share their half by the same midpoint rule). The zone then names its part (a Segment) and is
// no longer a point place; the road keeps its name elsewhere.
inline constexpr float kMapZoneReachM = 8.f;
// m along the road's centre line from its first point to the point of it nearest `world` (distance: how far that is).
FTCORE_API float mapRoadAlong(const MiniMapModel& m, int road, Vec2 world, float* distance = nullptr);
// The one rule for "the name at this point on this road", used by the labels along roads, the cursor's road name and
// the caption: a segment's name (and its zone's district) where one covers the point, else the road's name and district.
struct MapPlaceName {
    std::uint16_t name = kMapNoName;     // into names
    std::uint16_t district = kMapNoName; // into districts
};
FTCORE_API MapPlaceName mapNameAt(const MiniMapModel& m, const MapNames& n, int road, Vec2 world);
// Where the player is, for the caption: the road whose centre line is nearest `world` within `reach` m (ties to the
// higher class), and the name and district there (mapNameAt). With no road in reach: no road and no street, and the
// district whose anchor is nearest. Zero allocation.
inline constexpr float kMapHereReachM = 12.f;
struct MapHere {
    int road = -1;
    std::uint16_t name = kMapNoName;
    std::uint16_t district = kMapNoName;
};
FTCORE_API MapHere mapHereAt(const MiniMapModel& m, const MapNames& n, Vec2 world, float reach = kMapHereReachM);

// A label: a box of w x h px centred at `at`, turned by `angleDeg` (screen degrees, clockwise, y down), always in
// [-90, 90) so it reads left to right or, standing, bottom to top.
struct MapLabel {
    Vec2 at{};
    float angleDeg = 0.f;
    float w = 0.f, h = 0.f;
    std::uint16_t name = kMapNoName;
    std::uint8_t priority = 0; // lower wins
    bool fits = true;          // along a road: the stretch was at least as long as the label
};
// An angle (screen degrees) turned by half turns into [-90, 90).
FTCORE_API float mapUprightDeg(float deg);
// The label along the roads named `name` at view v: on the longest stretch of their centre lines inside the screen
// (less `margin` px all round), centred on it, turned along it, upright. Only the parts of a road that carry the name
// count (mapNameAt: a zone's segment carries the zone's name, the rest of the road its own), so two names that share
// a run are each set on their own part. A stretch is a run of one road's pieces that
// meet end to end, each heading within kMapAlongBendDeg of the run's first piece; its length is its chord (start to
// end), so a lane drawn in short pieces is named along its whole straight run. fits says the stretch is at least
// w + 2 * pad. False when no stretch of them is on screen.
inline constexpr float kMapAlongBendDeg = 12.f; // the most a piece may turn from the run's first heading
inline constexpr float kMapAlongJoinPx = 0.5f;  // pieces closer than this end to end meet
FTCORE_API bool mapLabelAlong(const MiniMapModel& m, const MapNames& n, std::uint16_t name, const MapOverlayView& v,
                              float w, float h, MapLabel& out, float margin = 24.f, float pad = 8.f);
// Whether two labels' turned boxes, each grown by gap / 2 all round, overlap.
FTCORE_API bool mapLabelsOverlap(const MapLabel& a, const MapLabel& b, float gap = 6.f);
// A label's turned box grown by `grow` px all round, as its four corners (clockwise on screen from the top left of
// the unturned box). The host fills the halo from it, or tests a point against it.
FTCORE_API void mapLabelCorners(const MapLabel& l, float grow, Vec2 out[4]);
// The halo: every label's text has a band this wide (px at 1080p) in the ground's colour behind it, so no line runs
// through a name; the placer counts it as part of the label.
inline constexpr float kMapLabelHaloPx = 3.f;
// Whether a label's box grown by `grow` px meets road `road`'s line as drawn at view v: each piece a box `widthPx` wide,
// capped half that width past its points. False for a road out of range or with fewer than two points. The host keeps
// every name but the cursor's off the cursor's lit road, which is drawn wider than the halo.
FTCORE_API bool mapLabelOnRoad(const MiniMapModel& m, int road, const MapOverlayView& v, float widthPx, const MapLabel& l,
                               float grow = 0.f);
// Moves a label the least it takes for its box grown by `grow` px to lie inside the screen with kMapLabelInsetPx to
// spare, turned boxes too. False, the label unchanged, when the grown box will not fit on the screen.
inline constexpr float kMapLabelInsetPx = 0.5f;
FTCORE_API bool mapLabelKeepInside(MapLabel& l, float screenW, float screenH, float grow = 0.f);

// The placer: candidates in, the shown labels out by priority (then by input order), each kept only if it overlaps
// nothing kept before it and no blocker (screen boxes already taken: the caption's, mapCaptionBox). `halo` px grows
// every label's box all round before the test (the halo is part of the label). Fixed capacity: candidates past it are
// dropped.
inline constexpr std::size_t kMaxMapLabels = 64;
struct MapLabelSet {
    std::array<MapLabel, kMaxMapLabels> labels{};
    std::size_t count = 0;
};
FTCORE_API void mapPlaceLabels(const MapLabel* candidates, std::size_t count, MapLabelSet& out, float gap = 6.f,
                               const MapLabel* blockers = nullptr, std::size_t blockerCount = 0, float halo = 0.f);
// The bottom-left caption (the player's district and street over a thin rule): where it stands and the screen box it
// reserves, which the placer treats as taken. The text's top left is (kMapCaptionX, screenH - kMapCaptionUp) times the
// scale; the box runs kMapCaptionPad round it, kMapCaptionW wide and kMapCaptionH tall (px at 1080p, times scale).
inline constexpr float kMapCaptionX = 64.f;
inline constexpr float kMapCaptionUp = 96.f;
inline constexpr float kMapCaptionW = 520.f;
inline constexpr float kMapCaptionH = 40.f;
inline constexpr float kMapCaptionPad = 12.f;
FTCORE_API MapLabel mapCaptionBox(float screenW, float screenH, float scale = 1.f);

// Which names are tried at a view, best first. Ranks are MapLabel::priority (lower wins): the cursor's road, then the
// districts (boxed, at every zoom), water, main roads, streets, places, lanes. Roads' names are set along them: main
// roads and streets from Mid, lanes at Near; stairs and terraces (class Stair) only under the cursor. Water at its point
// at every zoom, along a canal from Mid; the districts at their anchors; places at Near. A name is picked once, and only when
// its roads' bounds or its point meet the screen grown by `margin` px.
enum class MapLabelRank : std::uint8_t { Cursor, District, Water, Main, Street, Place, Lane };
FTCORE_API bool mapRoadLabelled(MapRoadClass c, MapZoom z);
struct MapLabelPick {
    std::uint16_t name = kMapNoName; // into MapNames::names
    MapLabelRank rank = MapLabelRank::Main;
    bool along = true; // set along its roads (mapLabelAlong); else centred on `at` (world m), level
    Vec2 at{};
};
// The picks into out (at most cap), cursor first, then by rank, then by table order. `picked` is the cursor's road
// (-1 none); `pickedName` the name at the cursor on it (mapNameAt), kMapNoName for the road's own. A zone's segment is
// picked with its road's class, when its bounds meet the screen. `seaAt` (world m, from mapSeaLabelAt), when given and
// the table names the sea (MapNames::seaName), sets the sea's name there once, level, at every zoom. `seen` is the
// caller's scratch, one byte per name (cleared here). Returns the count. Zero allocation: the roads' bounds are the
// ones measured at load (MapNames::roadBox).
FTCORE_API std::size_t mapLabelPicks(const MiniMapModel& m, const MapNames& n, const MapOverlayView& v, MapZoom z, int picked,
                                     MapLabelPick* out, std::size_t cap, std::uint8_t* seen, float margin = 0.f,
                                     std::uint16_t pickedName = kMapNoName, const Vec2* seaAt = nullptr);

} // namespace ftcore
