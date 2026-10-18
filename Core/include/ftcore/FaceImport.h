#pragma once

// Loads face_presets.json (written by newengine tools/blender/face_spec.py) into a FaceShapeSet:
// `shapes` [{name, layer}], `visemes` and `expressions` [{name, shapes: {shape: weight},
// tongue_deg?: [x, y, z]}] and `jaw_max_deg` (default 25). Other keys (masks, expression
// variants, phone maps, joint names) are rig-tool data and are ignored. Load time only.

#include "ftcore/Api.h"
#include "ftcore/Face.h"

#include <string>
#include <string_view>

namespace ftcore {

// False (and `*error`) on bad JSON or a bad entry; `out` is then empty.
FTCORE_API bool importFaceShapeSet(std::string_view json, FaceShapeSet& out, std::string* error = nullptr);

} // namespace ftcore
