#pragma once

// Modelled trees in place of the stand-in trees: which model a tree row gets and how it is scaled and turned.
//
// The build's -TreeMeshes=<file> names a table (CSV, '#' comment lines, a header row naming the columns):
//   kind     the rows it serves, as a tree table writes its kind: street, park, forecourt, precinct, cherry, cherry_old ...
//   asset    the model (the engine's asset path or name; the build resolves it)
//   weight   optional, how often it is picked among the models that fit (default 1; 0 leaves the row out)
// A tree row takes a model of its own kind; a kind with no model of its own falls back to "street" (a street tree's
// kind) or "park" (every other tree's). Among them only the models the row's height reaches by a scale within the
// limits are picked from, by weight, with the row's own seed; when none fits, the one nearest in height, its scale
// clamped (counted). Its crown width follows the row's canopy within a share of the height's scale (or a small seeded
// spread without one), it turns by a seeded yaw, and its scale varies a little by seed.
//
// The seed is the row's id (or, without one, its plan position), so a rebuild puts the same tree in the same place.
// Plain C++20; the table's reading allocates (build time only), the pick doesn't.

#include "ftcore/Api.h"
#include "ftcore/StandIns.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

struct TreeModel {
    std::string kind;   // lower case
    std::string asset;
    double weight = 1.0;
    double height = 0.0; // metres, the model's own (the build fills it from the mesh's bounds)
    double crown = 0.0;  // metres, the model's own crown width (the larger of its bounds' x and y)
};

// Reads the table into out (appended). False with a line-numbered error on a bad header, a short row or a bad weight.
FTCORE_API bool parseTreeModels(std::string_view csv, std::vector<TreeModel>& out, std::string* error = nullptr);

struct TreePickSpec {
    double minScale = 0.6;   // the height's scale a model may take, at least ...
    double maxScale = 1.4;   // ... and at most
    double crownSpread = 0.2; // the crown's scale stays within this share of the height's
    double jitter = 0.06;    // the seeded spread of the height's scale (+-)
};

struct TreePick {
    int model = -1;          // into the models; -1: none serves the row
    double scaleZ = 1.0;     // the height's scale
    double scaleXY = 1.0;    // the crown's
    double yawDeg = 0.0;     // plan degrees, counter-clockwise
    bool clamped = false;    // no model fitted: the nearest, its scale clamped
};

// The kind the row's models are looked up by: its tag, or the tag of its stand-in kind when it has none.
FTCORE_API std::string_view treeKindOf(const StandInPoint& point);
// The pick for one tree row (a StreetTree or ParkTree point). The row's height: its h, else its scale times the model's.
FTCORE_API TreePick pickTreeModel(const StandInPoint& point, const TreeModel* models, std::size_t count, const TreePickSpec& spec = {});

} // namespace ftcore
