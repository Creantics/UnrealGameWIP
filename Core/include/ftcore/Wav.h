#pragma once

// Reads a RIFF WAVE file (.wav): its format, and optionally its samples as interleaved floats in
// [-1, 1]. Load-time only (decoding allocates).
//
//   ftcore::WavInfo info;
//   std::vector<float> samples;
//   std::string error;
//   if (!ftcore::readWav(bytes, info, &samples, &error)) ...;
//
// Accepted: PCM 16, 24 and 32 bit, and IEEE float 32 bit, any channel count, plain or
// WAVE_FORMAT_EXTENSIBLE. The first loop of a `smpl` chunk, when there is one, is reported in
// frames. Anything else (compressed formats, 8-bit, a missing fmt or data chunk) fails with a reason.

#include "ftcore/Api.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

struct WavInfo {
    std::uint32_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t bitsPerSample = 0;
    bool isFloat = false;
    std::uint64_t frames = 0; // samples per channel
    bool hasLoop = false;     // a smpl chunk with at least one loop
    std::uint64_t loopStart = 0, loopEnd = 0; // frames, inclusive end as stored in smpl

    double durationSeconds() const { return sampleRate ? static_cast<double>(frames) / sampleRate : 0.0; }
};

// False on a file that isn't a WAVE this reads; `*error` says why. `samples`, when given, gets
// frames * channels interleaved values.
FTCORE_API bool readWav(std::string_view bytes, WavInfo& info, std::vector<float>* samples = nullptr,
                        std::string* error = nullptr);

} // namespace ftcore
