#pragma once

// Which arena camera preset the player picked (Settings::cameraPreset; the rigs are in CameraPreset.h). Kept on its own
// so the settings need none of the camera's headers.

#include <cstdint>

namespace ftcore {

enum class CameraPresetId : std::uint8_t { Classic, Close };

// "classic", "close": the preset's name in Data/Battle/camera_presets.json and on the command line.
inline constexpr const char* cameraPresetName(CameraPresetId id) { return id == CameraPresetId::Close ? "close" : "classic"; }

} // namespace ftcore
