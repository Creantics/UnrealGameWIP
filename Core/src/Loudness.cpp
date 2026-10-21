#include "ftcore/Loudness.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ftcore {

namespace {

constexpr double kPi = 3.14159265358979323846;

// One direct form I biquad.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    double run(double x) {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
};

// The two K-weighting stages for any sample rate (coefficients from the analogue prototypes, as
// libebur128 does; at 48 kHz they match the table in BS.1770).
void kWeighting(std::uint32_t rate, Biquad& shelf, Biquad& highPass) {
    {
        const double f0 = 1681.974450955533, gainDb = 3.999843853973347, q = 0.7071752369554196;
        const double k = std::tan(kPi * f0 / rate);
        const double vh = std::pow(10.0, gainDb / 20.0);
        const double vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        shelf.b0 = (vh + vb * k / q + k * k) / a0;
        shelf.b1 = 2.0 * (k * k - vh) / a0;
        shelf.b2 = (vh - vb * k / q + k * k) / a0;
        shelf.a1 = 2.0 * (k * k - 1.0) / a0;
        shelf.a2 = (1.0 - k / q + k * k) / a0;
    }
    {
        const double f0 = 38.13547087602444, q = 0.5003270373238773;
        const double k = std::tan(kPi * f0 / rate);
        const double a0 = 1.0 + k / q + k * k;
        highPass.b0 = 1.0;
        highPass.b1 = -2.0;
        highPass.b2 = 1.0;
        highPass.a1 = 2.0 * (k * k - 1.0) / a0;
        highPass.a2 = (1.0 - k / q + k * k) / a0;
    }
}

double channelWeight(std::uint32_t channel, std::uint32_t channels) {
    if (channels == 6) { // 5.1: L R C LFE Ls Rs
        if (channel == 3) return 0.0;
        if (channel >= 4) return 1.41;
    }
    return 1.0;
}

float toDb(double linear) {
    return linear > 0.0 ? static_cast<float>(20.0 * std::log10(linear)) : Loudness::kSilence;
}

// 4x oversampling: the three in-between phases from a Hann-windowed sinc, kTaps samples either side.
constexpr int kTaps = 16;
constexpr int kPhases = 4;

double truePeak(const float* interleaved, std::uint64_t frames, std::uint32_t channels, double samplePeak) {
    double coeff[kPhases][2 * kTaps];
    for (int p = 1; p < kPhases; ++p) {
        for (int k = 0; k < 2 * kTaps; ++k) {
            // Interpolating at n + p/4 from x[n - kTaps + 1 + k].
            const double t = static_cast<double>(kTaps - 1 - k) + static_cast<double>(p) / kPhases;
            const double sinc = std::sin(kPi * t) / (kPi * t);
            const double window = 0.5 * (1.0 + std::cos(kPi * t / (kTaps + 1)));
            coeff[p][k] = sinc * window;
        }
    }
    double peak = samplePeak;
    const std::int64_t n = static_cast<std::int64_t>(frames);
    for (std::uint32_t c = 0; c < channels; ++c) {
        for (std::int64_t i = 0; i + 1 < n; ++i) {
            for (int p = 1; p < kPhases; ++p) {
                double v = 0.0;
                for (int k = 0; k < 2 * kTaps; ++k) {
                    const std::int64_t at = i - kTaps + 1 + k;
                    if (at >= 0 && at < n) v += coeff[p][k] * interleaved[static_cast<std::size_t>(at) * channels + c];
                }
                peak = std::max(peak, std::fabs(v));
            }
        }
    }
    return peak;
}

} // namespace

Loudness measureLoudness(const float* interleaved, std::uint64_t frames, std::uint32_t channels, std::uint32_t sampleRate) {
    Loudness out;
    if (!interleaved || frames == 0 || channels == 0 || sampleRate == 0) return out;

    double samplePeak = 0.0;
    for (std::uint64_t i = 0; i < frames * channels; ++i) samplePeak = std::max(samplePeak, std::fabs(static_cast<double>(interleaved[i])));
    out.samplePeakDb = toDb(samplePeak);
    out.truePeakDb = toDb(truePeak(interleaved, frames, channels, samplePeak));

    // Weighted K-filtered energy per 100 ms step; a gating block is four consecutive steps.
    const std::uint64_t step = std::max<std::uint64_t>(1, (sampleRate + 5) / 10);
    const std::uint64_t steps = frames / step;
    if (steps < 4) return out;
    std::vector<double> stepEnergy(static_cast<std::size_t>(steps), 0.0);
    for (std::uint32_t c = 0; c < channels; ++c) {
        const double weight = channelWeight(c, channels);
        if (weight == 0.0) continue;
        Biquad shelf, highPass;
        kWeighting(sampleRate, shelf, highPass);
        for (std::uint64_t s = 0; s < steps; ++s) {
            double sum = 0.0;
            for (std::uint64_t i = s * step; i < (s + 1) * step; ++i) {
                const double y = highPass.run(shelf.run(interleaved[static_cast<std::size_t>(i * channels + c)]));
                sum += y * y;
            }
            stepEnergy[static_cast<std::size_t>(s)] += weight * sum;
        }
    }

    const double blockSamples = 4.0 * static_cast<double>(step);
    std::vector<double> blocks;
    blocks.reserve(static_cast<std::size_t>(steps - 3));
    for (std::uint64_t b = 0; b + 4 <= steps; ++b) {
        double z = 0.0;
        for (std::uint64_t s = b; s < b + 4; ++s) z += stepEnergy[static_cast<std::size_t>(s)];
        blocks.push_back(z / blockSamples);
    }

    const auto lufs = [](double z) { return -0.691 + 10.0 * std::log10(z); };
    const double absoluteZ = std::pow(10.0, (-70.0 + 0.691) / 10.0);
    double sum = 0.0;
    std::size_t count = 0;
    for (double z : blocks) {
        if (z > absoluteZ) {
            sum += z;
            ++count;
        }
    }
    if (count == 0) return out;
    const double relativeZ = std::pow(10.0, (lufs(sum / count) - 10.0 + 0.691) / 10.0);
    const double gateZ = std::max(absoluteZ, relativeZ);
    sum = 0.0;
    count = 0;
    for (double z : blocks) {
        if (z > gateZ) {
            sum += z;
            ++count;
        }
    }
    if (count == 0) return out;
    out.hasIntegrated = true;
    out.integratedLufs = static_cast<float>(lufs(sum / count));
    return out;
}

} // namespace ftcore
