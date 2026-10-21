#pragma once

// Loudness of a clip, after ITU-R BS.1770-4 / EBU R 128: integrated loudness in LUFS (K-weighted,
// 400 ms blocks with 75% overlap, absolute gate -70 LUFS, relative gate -10 LU), sample peak and
// true peak in dBFS (true peak from 4x oversampling). Load-time only (it allocates per call).
//
//   const ftcore::Loudness l = ftcore::measureLoudness(samples.data(), info.frames, info.channels, info.sampleRate);
//   if (l.hasIntegrated) ... l.integratedLufs ...
//
// Every channel weighs 1.0, except that a 6-channel file is taken as 5.1 (L R C LFE Ls Rs: the LFE
// is left out and the surrounds weigh 1.41, as the standard says). A mono file is measured as one
// channel, so a mono sine reads 3 dB below the same sine on two channels. A clip shorter than one block, or with every block under the absolute gate, has
// no integrated loudness (hasIntegrated false, integratedLufs = kSilence).

#include "ftcore/Api.h"

#include <cstdint>

namespace ftcore {

struct Loudness {
    static constexpr float kSilence = -200.f;

    bool hasIntegrated = false;
    float integratedLufs = kSilence;
    float samplePeakDb = kSilence;
    float truePeakDb = kSilence;
};

FTCORE_API Loudness measureLoudness(const float* interleaved, std::uint64_t frames, std::uint32_t channels,
                                    std::uint32_t sampleRate);

} // namespace ftcore
