#pragma once

// Places a world's vertical-route pieces (stairs, landings, decks, footbridges, rails) into a saved world map, from a
// piece table kept outside the repo and named on the command line, and saves the map in place:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTPlaceVertical -Map=/Game/Maps/<map> -Pieces=<csv> [-Side=1000]
//       [-ProbeOut=<csv>] [-Tour=<csv>] [-NoNav] [-RebuildNav] [-DryRun] -NullRHI -unattended -nopause -nosplash
//
// The massing build (FTBuildMassingWorld) is not touched and its tables are not read: this runs after it, on the map
// it saved. Every actor it places carries the tag FTVertical, and a run first removes the actors a run before it
// placed, so the table can be run again and again. An empty table (the header alone) removes them and places none.
//
// The table's columns, by header name, in the plan frame of a square -Side metres across (ftcore/MassingPlan.h):
//   id        the piece's label (unique in the table)
//   kind      tread, landing, deck, ramp, rail, post, stringer, pier, collider or probe
//   cx,cy,cz  the piece's centre, metres (for a probe: the point a surface is expected at)
//   sx,sy,sz  its size along its own axes, metres (the engine's cube scaled)
//   rot       plan rotation of its x axis, degrees counter-clockwise from east
//   pitch     degrees its x axis rises (a ramp or stringer), 0 for level pieces
//   material  a material's object path, or none for the engine's default
//   collide   1 blocks everything, 0 nothing (it neither stops pawns nor touches the navmesh), 2 a hidden collider
//             (blocks, never drawn: the walk ramp under a flight's treads, so the navmesh and the lead's feet
//             follow the nosing line and not each riser)
// A probe places nothing. Each probe traces down the pawn channel from over its point (sz metres over it, 4 m when sz
// is 0, so a probe under a stacked flight starts below the flight over it) to 12 m under it, before and after the
// pieces are placed, and logs the surface it meets, the gap to the point, the actor it belongs to and whether the
// navmesh lies on that surface (and at what height): the check that the table's heights match the map's real walk
// surfaces and that a walk can stand there. -ProbeOut writes the same as a csv (when,id,x,y,expected_z,surface_z,
// actor,nav,nav_z).
//
// -Tour names a tour table (ftcore::importTourStops: id, kind, x, y, z, start, edge, still) whose stops replace the
// ones saved on the map's explore director, for a -WorldAutoPlay -WorldTourStops=only walk of the new routes.
//
// Unless -NoNav, the navmesh is rebuilt and saved with the map when a run placed or removed anything, so a walk
// finds the new decks; -RebuildNav rebuilds it even when nothing changed, and the probes run once more on the rebuilt
// navmesh. -DryRun reads, places, rebuilds and probes but doesn't save. A bad row, a duplicate id, a material that
// doesn't load or a bad tour table fails the run before the map changes.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTPlaceVerticalCommandlet.generated.h"

UCLASS()
class UFTPlaceVerticalCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTPlaceVerticalCommandlet();

	virtual int32 Main(const FString& Params) override;
};
