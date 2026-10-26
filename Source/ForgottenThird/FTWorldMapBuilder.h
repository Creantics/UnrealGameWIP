#pragma once

// Builds one big world's map (/Game/Maps/<map> from Data/Field/worlds.json) from code, for
// FTBuildExploreWorld -World=<id>:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildExploreWorld -World=solhaven [-Style=<spec>]
//       -NullRHI -unattended -nopause -nosplash
//
// The world is laid out by ftcore::generateWorldLayout (the same call the director makes at run
// time, so the map and the game agree): a floor `side` metres square with perimeter walls, straight
// roads between districts, each district a stone plaza ringed by blockout buildings with a tall
// landmark and a gold beacon on top, trees and rocks on the open ground, and one large shape
// floating over the centre that can be seen from anywhere. Buildings, roads and props are instanced
// (a few actors with instanced mesh components, not thousands of actors). Each district gets its
// own navmesh bounds; every field goal (ftcore::demo::kFieldGoals and Data/Field/goals.json) must be
// reachable from every district's spawn, or the build fails. The player start is district 0's
// spawn; the director's WorldId is set, and the style controller uses the world's style. It
// overwrites the map.

#include "CoreMinimal.h"

#if WITH_EDITOR
// Returns the commandlet's exit code.
int32 FTBuildWorldMap(const FString& WorldId, const FString& Params);
#endif
