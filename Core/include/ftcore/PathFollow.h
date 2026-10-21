#pragma once

// Following a path of ground points (a navmesh path, in metres): which point to steer at next.
//
//   int next = 1;                     // point 0 is where the path started
//   next = ftcore::advanceWaypoint(points, count, next, playerPos, 0.5f);
//   steer toward points[next];
//
// No allocation.

#include "ftcore/Api.h"
#include "ftcore/Explore.h"

namespace ftcore {

// The index to steer at, from `current` on: it moves past every point within `reach` of `pos`,
// and past a point the walker has already passed (it lies behind, along the leg that led to it). It never
// goes back and stops at the last point. A path of 0 points gives -1.
FTCORE_API int advanceWaypoint(const Vec2* points, int count, int current, Vec2 pos, float reach);

} // namespace ftcore
