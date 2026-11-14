#include "ftcore/ParkedCarLook.h"

namespace ftcore {

namespace {

constexpr ParkedCarColor kPaints[kParkedCarPaints] = {
    {0.255f, 0.306f, 0.366f}, // pale blue
    {0.78f, 0.78f, 0.74f},    // off-white
    {0.42f, 0.43f, 0.45f},    // silver
    {0.68f, 0.60f, 0.42f},    // cream
    {0.36f, 0.55f, 0.45f},    // mint
    {0.42f, 0.07f, 0.05f},    // brick red
    {0.05f, 0.055f, 0.06f},   // charcoal
    {0.60f, 0.40f, 0.07f},    // mustard
};

std::uint32_t carMix(std::uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352dU;
    h ^= h >> 15;
    h *= 0x846ca68bU;
    h ^= h >> 16;
    return h;
}

std::uint32_t carHash(std::string_view id, std::uint32_t seed) {
    std::uint32_t h = 2166136261U ^ carMix(seed + 0x9e3779b9U);
    for (const char c : id) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619U;
    }
    return carMix(h);
}

float unit(std::uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); } // [0, 1)

} // namespace

ParkedCarColor parkedCarPaint(int index) {
    return index >= 0 && index < kParkedCarPaints ? kPaints[index] : kPaints[0];
}

ParkedCarLook parkedCarLook(std::string_view id, std::uint32_t seed, float lampShare) {
    const std::uint32_t h = carHash(id, seed);
    ParkedCarLook look;
    look.paint = static_cast<int>(h % static_cast<std::uint32_t>(kParkedCarPaints));
    look.color = kPaints[look.paint];
    look.dirt = kParkedCarDirtMin + (kParkedCarDirtMax - kParkedCarDirtMin) * unit(carMix(h ^ 0x68e31da4U));
    look.lamp = unit(carMix(h ^ 0xb5297a4dU)) < lampShare ? 1.f : 0.f;
    return look;
}

void parkedCarCustomData(const ParkedCarLook& look, float* out) {
    out[0] = look.color.r;
    out[1] = look.color.g;
    out[2] = look.color.b;
    out[3] = look.dirt;
    out[4] = look.lamp;
}

} // namespace ftcore
