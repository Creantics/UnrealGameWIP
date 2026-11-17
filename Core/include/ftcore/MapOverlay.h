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

// The names, joined to the roads by id at load (this allocates; it runs once). The table's text is CSV with the header
// id,kind,name,sign_form,district; rows with kind "district" are the districts (id = the district's id). A road with
// no row has no name; rows that name no road in the model are skipped (counted in `unmatched`).
inline constexpr std::uint16_t kMapNoName = 0xFFFF;
struct MapNames {
    std::vector<std::string> names;      // each distinct name once
    std::vector<std::string> signForms;  // per name: its sign form ("" none; the glyphs are drawn, not typed)
    std::vector<std::uint16_t> roadName; // per model road: index into names, or kMapNoName
    struct District {
        std::string id;
        std::uint16_t name = kMapNoName;
    };
    std::vector<District> districts;
    std::size_t unmatched = 0;
};
FTCORE_API bool mapLoadNames(const MiniMapModel& m, std::string_view csv, MapNames& out, std::string* error = nullptr);
// The name of the road at index `road` (kMapNoName for none or out of range).
FTCORE_API std::uint16_t mapRoadName(const MapNames& n, int road);

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
// (less `margin` px all round), centred on it, turned along it, upright. fits says the stretch is at least w + 2 * pad.
// False when no stretch of them is on screen.
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

} // namespace ftcore
