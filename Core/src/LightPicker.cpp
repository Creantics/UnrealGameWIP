#include "ftcore/LightPicker.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

// The cell index along one axis of v, clamped into [0, cells).
std::size_t cellOf(double v, double origin, double cell, std::size_t cells) {
    const double c = std::floor((v - origin) / cell);
    if (c <= 0.0) {
        return 0;
    }
    const double last = static_cast<double>(cells - 1);
    return c >= last ? cells - 1 : static_cast<std::size_t>(c);
}

} // namespace

void LightPicker::build(const float* xy, std::size_t count, float cellSize) {
    points_.clear();
    start_.clear();
    items_.clear();
    cellsX_ = cellsY_ = 0;
    if (!xy || count == 0) {
        return;
    }
    points_.assign(xy, xy + count * 2);
    float maxX = xy[0], maxY = xy[1];
    minX_ = xy[0];
    minY_ = xy[1];
    for (std::size_t i = 1; i < count; ++i) {
        minX_ = std::min(minX_, xy[i * 2]);
        maxX = std::max(maxX, xy[i * 2]);
        minY_ = std::min(minY_, xy[i * 2 + 1]);
        maxY = std::max(maxY, xy[i * 2 + 1]);
    }
    const double spread = std::max(static_cast<double>(maxX) - minX_, static_cast<double>(maxY) - minY_);
    double cell = cellSize > 0.f ? static_cast<double>(cellSize) : 1.0;
    cell = std::max(cell, spread / static_cast<double>(kMaxCellsPerAxis - 1));
    cell_ = cell;
    cellsX_ = static_cast<std::size_t>(std::floor((static_cast<double>(maxX) - minX_) / cell)) + 1;
    cellsY_ = static_cast<std::size_t>(std::floor((static_cast<double>(maxY) - minY_) / cell)) + 1;
    cellsX_ = std::min(cellsX_, kMaxCellsPerAxis);
    cellsY_ = std::min(cellsY_, kMaxCellsPerAxis);

    // Counting sort by cell; indices go in ascending order, so each cell's list stays ascending.
    start_.assign(cellsX_ * cellsY_ + 1, 0u);
    std::vector<std::uint32_t> cellOfPoint(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t cx = cellOf(xy[i * 2], minX_, cell, cellsX_);
        const std::size_t cy = cellOf(xy[i * 2 + 1], minY_, cell, cellsY_);
        cellOfPoint[i] = static_cast<std::uint32_t>(cy * cellsX_ + cx);
        ++start_[cellOfPoint[i] + 1];
    }
    for (std::size_t c = 1; c < start_.size(); ++c) {
        start_[c] += start_[c - 1];
    }
    items_.resize(count);
    std::vector<std::uint32_t> fill(start_.begin(), start_.end() - 1);
    for (std::size_t i = 0; i < count; ++i) {
        items_[fill[cellOfPoint[i]]++] = static_cast<std::uint32_t>(i);
    }
}

std::size_t LightPicker::pick(float x, float y, float radius, std::size_t maxCount, std::uint32_t* out) const {
    if (maxCount == 0 || !out || radius < 0.f || cellsX_ == 0) {
        return 0;
    }
    const double r = radius;
    const double r2 = r * r;
    const double cell = cell_;
    // The cells the radius' square touches; a query wholly off the grid touches none.
    const double loX = (static_cast<double>(x) - r - minX_) / cell;
    const double hiX = (static_cast<double>(x) + r - minX_) / cell;
    const double loY = (static_cast<double>(y) - r - minY_) / cell;
    const double hiY = (static_cast<double>(y) + r - minY_) / cell;
    if (hiX < 0.0 || hiY < 0.0 || loX >= static_cast<double>(cellsX_) || loY >= static_cast<double>(cellsY_)) {
        return 0;
    }
    const std::size_t cx0 = cellOf(static_cast<double>(x) - r, minX_, cell, cellsX_);
    const std::size_t cx1 = cellOf(static_cast<double>(x) + r, minX_, cell, cellsX_);
    const std::size_t cy0 = cellOf(static_cast<double>(y) - r, minY_, cell, cellsY_);
    const std::size_t cy1 = cellOf(static_cast<double>(y) + r, minY_, cell, cellsY_);

    // A bounded insertion into out, ordered by (squared distance, index). Kept entries are re-measured from points_
    // when compared, so no distance buffer (and no allocation) is needed.
    std::size_t used = 0;
    auto dist2 = [&](std::uint32_t i) {
        const double dx = static_cast<double>(points_[i * 2]) - x;
        const double dy = static_cast<double>(points_[i * 2 + 1]) - y;
        return dx * dx + dy * dy;
    };
    auto before = [&](double da, std::uint32_t a, double db, std::uint32_t b) { return da < db || (da == db && a < b); };
    for (std::size_t cy = cy0; cy <= cy1; ++cy) {
        for (std::size_t cx = cx0; cx <= cx1; ++cx) {
            const std::size_t c = cy * cellsX_ + cx;
            for (std::uint32_t k = start_[c]; k < start_[c + 1]; ++k) {
                const std::uint32_t idx = items_[k];
                const double d = dist2(idx);
                if (d > r2) {
                    continue;
                }
                if (used == maxCount && !before(d, idx, dist2(out[used - 1]), out[used - 1])) {
                    continue;
                }
                std::size_t at = used < maxCount ? used++ : used - 1;
                while (at > 0 && before(d, idx, dist2(out[at - 1]), out[at - 1])) {
                    out[at] = out[at - 1];
                    --at;
                }
                out[at] = idx;
            }
        }
    }
    return used;
}

} // namespace ftcore
