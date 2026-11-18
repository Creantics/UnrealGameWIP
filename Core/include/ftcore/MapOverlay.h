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
};
// The table's x, y are plan metres (the build tables' frame) when planSide > 0, turned to engine metres with
// massingEnginePoint; else they are read as engine metres. A district row's own x, y, when given, is its anchor.
FTCORE_API bool mapLoadNames(const MiniMapModel& m, std::string_view csv, MapNames& out, std::string* error = nullptr,
                             double planSide = 0.0);
// The name of the road at index `road` (kMapNoName for none or out of range).
FTCORE_API std::uint16_t mapRoadName(const MapNames& n, int road);
// The district of the road at index `road`: index into n.districts, or kMapNoName.
FTCORE_API std::uint16_t mapRoadDistrict(const MapNames& n, int road);

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
// (less `margin` px all round), centred on it, turned along it, upright. A stretch is a run of one road's pieces that
// meet end to end, each heading within kMapAlongBendDeg of the run's first piece; its length is its chord (start to
// end), so a lane drawn in short pieces is named along its whole straight run. fits says the stretch is at least
// w + 2 * pad. False when no stretch of them is on screen.
inline constexpr float kMapAlongBendDeg = 12.f; // the most a piece may turn from the run's first heading
inline constexpr float kMapAlongJoinPx = 0.5f;  // pieces closer than this end to end meet
FTCORE_API bool mapLabelAlong(const MiniMapModel& m, const MapNames& n, std::uint16_t name, const MapOverlayView& v,
                              float w, float h, MapLabel& out, float margin = 24.f, float pad = 8.f);
// Whether two labels' turned boxes, each grown by gap / 2 all round, overlap.
FTCORE_API bool mapLabelsOverlap(const MapLabel& a, const MapLabel& b, float gap = 6.f);

// The placer: candidates in, the shown labels out by priority (then by input order), each kept only if it overlaps
// nothing kept before it. Fixed capacity: candidates past it are dropped.
inline constexpr std::size_t kMaxMapLabels = 64;
struct MapLabelSet {
    std::array<MapLabel, kMaxMapLabels> labels{};
    std::size_t count = 0;
};
FTCORE_API void mapPlaceLabels(const MapLabel* candidates, std::size_t count, MapLabelSet& out, float gap = 6.f);

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
// (-1 none). `seen` is the caller's scratch, one byte per name (cleared here). Returns the count. Zero allocation.
FTCORE_API std::size_t mapLabelPicks(const MiniMapModel& m, const MapNames& n, const MapOverlayView& v, MapZoom z, int picked,
                                     MapLabelPick* out, std::size_t cap, std::uint8_t* seen, float margin = 0.f);

} // namespace ftcore
