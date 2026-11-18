#include "ftcore/ParkedCarLook.h"

namespace ftcore {

namespace {

// Dark, clear-coated night paints (multipliers on the mesh's base colour): wet cars read as deep colour with sharp
// lantern highlights, not as pale blocks under the lamps.
constexpr ParkedCarColor kPaints[kParkedCarPaints] = {
    {0.035f, 0.037f, 0.040f}, // charcoal
    {0.030f, 0.075f, 0.050f}, // deep green
    {0.170f, 0.025f, 0.020f}, // oxblood
    {0.025f, 0.040f, 0.080f}, // navy
    {0.090f, 0.095f, 0.100f}, // gunmetal
    {0.090f, 0.060f, 0.035f}, // dark bronze
    {0.025f, 0.070f, 0.075f}, // dark teal
    {0.220f, 0.220f, 0.215f}, // muted silver (the one light paint)
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
