#pragma once

// Builds a world's walkable massing map from its production plan's files, which are kept outside the
// repo and named on the command line:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildMassingWorld -Height=<json> -Boxes=<csv>
//       [-Walls=<csv>] [-Goals=<csv>] -Entry=<x,y,z> [-Look=<x,y>] [-Sea=<x>] [-Side=1000] [-Map=<name>]
//       [-World=<id>] [-Kit=<csv>] [-NoNav] [-Strict] [-Dump=<csv>] [-Load]
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
//   engine's cube, scaled, with its top brought down where it would share one (massingTopNudges); a
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
// a box the build doesn't make places its mesh without a collider and says so. The log gives each mesh's size and
// scale, and the built boxes left.
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
