#pragma once

// The nearest points to a spot, for a pool of real lights moved among many fixed light positions: a uniform grid over
// the points' xy, built once (it allocates), then queried as often as wanted with no heap allocation.
#include "ftcore/Api.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ftcore {

class LightPicker {
public:
    // xy holds count pairs (x0, y0, x1, y1, ...); cellSize is the grid's square cell, in the points' units (a size
    // near the usual query radius works well). Replaces whatever was built before. A cellSize at or under 0 is taken
    // as 1. The grid is held to at most kMaxCellsPerAxis cells a side by growing the cell when the points spread wider.
    FTCORE_API void build(const float* xy, std::size_t count, float cellSize);
    // Up to maxCount point indices within radius of (x, y) into out, nearest first, equal distances by lower index.
    // Returns how many were written. 0 when maxCount is 0, out is null, radius is negative or nothing was built.
    FTCORE_API std::size_t pick(float x, float y, float radius, std::size_t maxCount, std::uint32_t* out) const;

    std::size_t size() const { return points_.size() / 2; }
    std::size_t cellsX() const { return cellsX_; }
    std::size_t cellsY() const { return cellsY_; }

    static constexpr std::size_t kMaxCellsPerAxis = 1024;

private:
    std::vector<float> points_;          // xy pairs, by index
    std::vector<std::uint32_t> start_;   // cellsX_ * cellsY_ + 1 offsets into items_
    std::vector<std::uint32_t> items_;   // point indices grouped by cell, ascending within a cell
    float minX_ = 0.f, minY_ = 0.f;
    double cell_ = 1.0; // the same double the points were binned with, so build and pick agree on every boundary
    std::size_t cellsX_ = 0, cellsY_ = 0;
};

} // namespace ftcore
