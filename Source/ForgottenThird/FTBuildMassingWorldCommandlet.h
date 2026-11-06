#pragma once

// Builds a world's walkable massing map from its production plan's files, which are kept outside the
// repo and named on the command line:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildMassingWorld -Height=<json> -Boxes=<csv>
//       [-Walls=<csv>] [-Goals=<csv>] -Entry=<x,y,z> [-Look=<x,y>] [-Sea=<x>] [-Side=1000] [-Map=<name>]
//       [-NoNav] [-Strict]
//       -NullRHI -unattended -nopause -nosplash
//
// -Height names the world's 1 m height field's json (samples_x, samples_y, spacing_m,
// origin_of_sample_0_0_m and the raw 16-bit file beside it, ftcore::massingHeightFromValue's encoding);
// -Boxes and -Walls name massing tables (ftcore/MassingPlan.h), the walls table holding WALL rows only;
// -Goals names a CSV with the columns id, x, y, z (plan frame, metres). Every position is in the plan
// frame of a square -Side metres across (MassingPlan.h); the map is in the engine frame, so it reads
// true from above.
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
// - unless -NoNav, a navmesh over the square, and each goal checked for a path from the entry; the map
//   is saved either way and the count of goals reached is logged (with -Strict, a missing path fails).
// No director: the map is a free walk. Zones, hidden kinds, open options and proxies are not built.

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
