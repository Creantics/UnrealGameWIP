#pragma once

// Builds a world's walkable massing map from its production plan's files, which are kept outside the
// repo and named on the command line:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildMassingWorld -Height=<json> -Boxes=<csv>
//       [-Walls=<csv>] [-Goals=<csv>] -Entry=<x,y,z> [-Look=<x,y>] [-Sea=<x>] [-Side=1000] [-Map=<name>]
//       [-World=<id>] [-Kit=<csv>] [-NoNav] [-Strict] [-Dump=<csv>] [-Load]
//       [-Tour=<csv>] [-Coverage=<csv>] [-ReadBack=<json>] [-Surfaces -Streets=<csv>]
//       [-Props=<csv> [-PropStreets=<csv>]]
//       -NullRHI -unattended -nopause -nosplash
//
// -Height names a height field's json (samples_x, samples_y, spacing_m, origin_of_sample_0_0_m and the raw
// 16-bit file beside it, ftcore::massingHeightFromValue's encoding), the world's 1 m field or a coarser one;
// -Boxes and -Walls name massing tables (ftcore/MassingPlan.h), the walls table holding WALL rows only;
// -Goals names a CSV with the columns id, x, y, z and optionally spawn_x, spawn_y (plan frame, metres) and
// held, goal_box, spawn_box (massing ids): a held goal stands inside or on a box held for its own piece and
// is listed apart, never walked or counted; goal_box and spawn_box mark the goal or the spawn as a temporary
// placement just outside that box. Each is logged with its box, and a spawn_box with the generator's spawn
// that stands in it.
// Every position is in the plan frame of a square -Side metres across (MassingPlan.h); the map is in the
// engine frame, so it reads true from above.
//
// -World=<id> (a world of Data/Field/worlds.json) makes the map that world's: the goals table's rows D0,
// D1, ... (one per district) give each district's plaza centre and floor, and its spawn is the row's
// spawn_x, spawn_y or else the generator's rule (ftcore::WorldLayout::spawnFor), set down on the highest
// deck or landing at most 6 m over the higher of the plaza's floor and the ground there. The build pins
// the world's layout to them (ftcore::WorldLayout::pinDistricts, refused on a missing row or a place
// outside the square), logs each district's spawn and what it stands on (warning when the lead would
// stand inside a box or wall there) and its footing (ftcore::massingSpawnFooting, warning when the floor
// steps over the tolerance within the lead's reach, where the lead slides off and a path asked to the
// spawn finds no navmesh), with a count of the footings that hold, and places an AFTExploreDirector with
// them, so a story step, a
// -WorldDistrict=<n> start and the -WorldAutoPlay tour play in the plan's own districts.
//
// The map (/Game/Maps/<Map>, World_Massing by default; overwritten):
// - the height field as a Landscape, a vertex per sample, padded to whole components;
// - every built box (ftcore::massingBuilt) as a static mesh actor labelled with its id: a flat box is the
//   engine's cube, scaled, with its top brought down where it would share one (massingTopNudges) and
//   raised off the ground where it would lie on it (massingGroundRaises, each raised top logged); a
//   sloped one gets its own mesh in the map's package (SM_Massing_<id>, collision from its triangles);
//   each wears a flat tint by its finish (plain, wall or deck) until its kit or hero mesh replaces it;
// - the walls table's rows as hidden walls that stop pawns, not the camera;
// - with -Sea, a sea plane at the datum from plan x = <x> east past the horizon (no collision);
// - the shared photoreal look (FTSceneLook.h), and a player start at -Entry facing -Look;
// - unless -NoNav, a navmesh over the square, and each goal (and with -World, each district's spawn)
//   checked for a path from the entry; the map is saved either way and the counts reached are logged
//   (with -Strict, a missing path or a spawn footing over the tolerance fails). The entry's own floor and
//   footing are logged too. The check's path search has a budget of 32768 nodes (the engine's
//   default is 2048); each target's line says whether its query came back full, partial or with none, and
//   whether it hit the node limit, and a summary line counts them.
// Without -World there is no director: the map is a free walk. Zones, hidden kinds, open options and
// proxies are not built.
//
// -Kit=<csv> swaps boxes for finished meshes already imported to the project, the table's columns being box (a
// massing id), asset (a static mesh's object path) and optionally fit (real, the default, or footprint), turn and
// dz (ftcore::massingKitPlacement). The mesh is labelled with the box's id and collides with nothing; the box stays
// as its hidden collider, labelled <id>-COL. A missing box or mesh fails the run before anything is built. A row on
// a box the build doesn't make places its mesh without a collider and says so. The asset none places nothing and
// only hides its box (still a collider), for a box another row's mesh stands in for. The log gives each mesh's size
// and scale, the boxes only hidden, and the built boxes left.
//
// -Props=<csv> sets street furniture along a streets table: -PropStreets=<csv>, or the -Streets table when it isn't
// given (the run fails with neither). Each row of the prop table (ftcore::importMassingProps: id, asset, spacing_m and
// the optional phase_m, side, inset_m, reach_m, clear_m, turn, dz, cap and kinds) sets its mesh along the streets of its
// kinds, at the kerbs or against the built faces beside them, on the floor the lead walks there
// (ftcore::massingPlaceProps over the built boxes, tops as built, and the height field), never on the sea's side of
// 0.5 m. Each is labelled <rule id>-<n> and, like the kit, collides with nothing and leaves the navmesh alone. A
// missing mesh fails the run before anything is built. The log gives each rule's mesh size, how many it placed of its
// spots and why the others placed nothing, and the total.
//
// -Surfaces -Streets=<csv> dresses the map in Solhaven's photographed surfaces (the surfaces of /Game/Rendering/Surfaces/solhaven_*, the
// ground master M_WorldGround and M_Look_Water), and without -Surfaces the build is exactly as without them (-Streets alone does nothing but give -Props its streets).
// Each built box wears the surface ftcore::massingLookSurface gives its src, kind, id prefix and row (the boxes table's row column, when it
// has one): kerbs and the other added walls pole concrete; flights, ramps, landings and terraces paving slab; cliff cover plates, the
// seawall, breakwater and quay board concrete; decks, piers, boats and bridges oak plank; cranes, masts, posts, barriers and gates
// galvanised steel; every other built box one of three building finishes (soot brick, breeze block, aluminium cladding) by a hash of its
// row. The Landscape wears an instance of M_WorldGround: paving slab for the ground, board concrete for the rock (from 20 m over the
// field's highest ground, so only steep faces turn to it) and bitumen for the carriageway (board concrete if bitumen isn't built); its road
// mask is the streets table's (columns id, width_m and pts, "x:y[:z];..." in the plan frame; ftcore::importMassingStreets and
// massingRoadMask), 8192 x 8192 pixels centred on the engine's origin and wide enough for the whole Landscape, with the streets moved to the
// engine frame as the boxes are. The -Sea plane wears M_Look_Water. Everything is loaded and checked before anything is built; one log
// line ("FTBuildMassingWorld: look: ...") gives the boxes by surface, the streets read, the mask and the sea's material. -Kit is not part of it.
//
// The walls table's blockers carry the actor tag FTHiddenWall, so the tour's edge legs can tell a hidden wall from a
// built one. (WALL rows of -Boxes are not built, so they have no actor to tag.)
//
// -Tour=<csv> (with -World) gives the map's director the stops its -WorldAutoPlay tour walks after the districts:
// reach, edge and push stops (ftcore::importTourStops; docs/WORLDS.md gives the columns), with the sea's level and the
// plan's square for its judging. The build logs the count by kind. Under -Load the saved director's count is checked
// against the table's.
//
// -Coverage=<csv> (with -World) checks the navmesh's islands after the reach check, in a build and with -Load: the
// navmesh's ground polys joined where they link (ftcore::navIslands), district 0's island the one its spawn projects to,
// and each island over 50 m2 besides it declared by a pocket point in it (ftcore::classifyNavIslands). An island on a
// built box's roof that no walk surface meets, or inside built boxes, is left out and counted apart (Recast takes a
// closed box as hollow; ftcore::navIslandsInBoxes over the boxes and walls tables). It logs a summary line, a line for
// each of the largest islands left out, a line per undeclared island and one per on-point off district 0's island or
// the navmesh; with -Strict, an undeclared island or a missed on-point fails.
//
// -ReadBack=<json> (a build only; a height field at the built field's spacing, on its grid and inside it) reads the
// Landscape back once it is made: every sample of that window from the heightmap, which must match exactly, and the
// collision surface at a fixed set of points between the samples (ftcore::readBackSurfacePoints), which must match the
// field's two triangles a cell within 5 mm. Two lines give the counts, the worst error and where it is; with -Strict, a
// mismatch fails.
//
// -Dump=<csv> also writes the saved map's actors to that file, read back from the actors after the save,
// so it changes nothing in the map; docs/WORLDS.md gives the columns. A dump it can't write fails the run.
//
// -Load builds nothing: it loads the saved /Game/Maps/<Map> in an editor world of its own, with the navmesh
// saved with it (the one the game walks), and runs the same reach check from -Entry to the goals and the
// district spawns, worked out from the same files as a build would. It logs the navmesh's tile count and
// how far the map director's spawns lie from the plan's, and never saves: the map file is not written. Only a
// map saved without navmesh tiles has them built, in memory, and the log says so. A -Kit table is still checked; -Dump is ignored.
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTBuildMassingWorldCommandlet.generated.h"

UCLASS()
class UFTBuildMassingWorldCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTBuildMassingWorldCommandlet();

	virtual int32 Main(const FString& Params) override;
};
