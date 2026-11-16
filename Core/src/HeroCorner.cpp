#include "ftcore/HeroCorner.h"

#include "ftcore/MassingDress.h"
#include "ftcore/WindowFacade.h"

#include <algorithm>
#include <string>

namespace ftcore {

namespace {

MassingBox flatBox(const char* id, const char* kind, double x0, double x1, double y0, double y1, double z0, double z1) {
    MassingBox b;
    b.id = id;
    b.kind = kind;
    b.source = MassingSource::Invented;
    b.cover = false;
    b.cx = 0.5 * (x0 + x1);
    b.cy = 0.5 * (y0 + y1);
    b.sx = x1 - x0;
    b.sy = y1 - y0;
    b.rot = 0.0;
    b.z0 = z0;
    b.z1 = z1;
    return b;
}

bool wantedWall(MassingLookSurface s) {
    return s == MassingLookSurface::BuildingRender || s == MassingLookSurface::BuildingTile ||
           s == MassingLookSurface::BuildingBoard || s == MassingLookSurface::BuildingA;
}

// The bay's facade as the build reads it: fronting the lane on its shop face only, the street floor baySink over z0.
bool shophouse(const MassingBox& box, double sink) {
    MassingWindowData data;
    data.streetBits = static_cast<std::uint8_t>(1u << kHeroShopFace);
    for (double& b : data.base) b = sink;
    return massingFacadeData(box, data).type == WindowType::Shophouse && wantedWall(massingLookSurface(box));
}

} // namespace

double heroCableZ(const HeroCornerSpec& spec, double t) {
    const double a = spec.ground + spec.stringFacadeUp, b = spec.ground + spec.stringPostUp;
    return a + (b - a) * t - 4.0 * spec.stringSag * t * (1.0 - t);
}

void heroCornerLayout(const HeroCornerSpec& spec, HeroCornerLayout& out) {
    const double g = spec.ground;
    out.pave = flatBox("HC-PAVE", "paving", spec.laneX0, spec.laneX1, spec.laneY0, spec.laneY1,
                       g + spec.paveTop - spec.paveThick, g + spec.paveTop);
    out.eave = flatBox("HC-EAVE", "eave", spec.laneX1 - spec.eaveOut, spec.laneX1, spec.laneY0, spec.laneY1,
                       g + spec.eaveUp, g + spec.eaveUp + spec.eaveThick);

    for (int i = 0; i < kHeroBays; ++i) {
        const double y0 = spec.laneY0 + spec.bayWidth * i;
        MassingBox box = flatBox("", "kit", spec.laneX1, spec.laneX1 + spec.bayDepth, y0, y0 + spec.bayWidth,
                                 g - spec.baySink, g + spec.bayHeight);
        const std::string stem = "HC-BAY-" + std::to_string(i + 1);
        box.id = stem + "a";
        for (char letter = 'a'; letter <= 'z'; ++letter) {
            box.id = stem + letter;
            if (shophouse(box, spec.baySink)) break;
            if (letter == 'z') box.id = stem + "a";
        }
        out.bays[i] = box;
        out.bayLit[i] = spec.bayLit[i];
    }
    out.eateryBay = std::clamp(spec.eateryBay, 0, kHeroBays - 1);

    out.lanternCount = 0;
    const double largeX = spec.laneX1 - spec.largeOut;
    for (int p = 0; p < kHeroLargeLanterns / 2; ++p) {
        for (int side = -1; side <= 1; side += 2) {
            HeroLantern& l = out.lanterns[out.lanternCount++];
            l.large = true;
            l.string = -1;
            l.centre = {largeX, spec.largePairs[p] + 0.5 * side * spec.largePairGap, g + spec.largeUp};
            l.hook = {l.centre.x, l.centre.y, g + spec.eaveUp};
        }
    }

    const double span = spec.laneX1 - spec.stringPostX;  // the facade end at laneX1, the post end at stringPostX
    const double run = spec.stringPitch * (kHeroStringLanterns - 1);
    const double first = 0.5 * (span - run);
    for (int s = 0; s < kHeroStrings; ++s) {
        const double y = spec.stringY[s];
        for (int k = 0; k < kHeroCablePoints; ++k) {
            const double t = static_cast<double>(k) / (kHeroCablePoints - 1);
            out.cable[s][k] = {spec.laneX1 - span * t, y, heroCableZ(spec, t)};
        }
        for (int k = 0; k < kHeroStringLanterns; ++k) {
            const double t = (first + spec.stringPitch * k) / span;
            HeroLantern& l = out.lanterns[out.lanternCount++];
            l.large = false;
            l.string = s;
            l.hook = {spec.laneX1 - span * t, y, heroCableZ(spec, t) - spec.smallDrop};
            l.centre = {l.hook.x, y, l.hook.z - 0.5 * spec.smallHeight};
        }
        out.postFoot[s] = {spec.stringPostX, y, g};
        out.stringLamp[s] = {spec.laneX1 - 0.5 * span, y, heroCableZ(spec, 0.5) - spec.stringLampUnder};
    }
    out.stringLampGain = spec.stringLampGain;
    out.eye = {spec.eye[0], spec.eye[1], spec.eye[2]};
    out.at = {spec.at[0], spec.at[1], spec.at[2]};
    out.lensMm = spec.lensMm;
}

bool heroCornerBay(std::string_view id, int* index) {
    constexpr std::string_view kStem = "HC-BAY-";
    if (id.size() <= kStem.size() || id.substr(0, kStem.size()) != kStem) return false;
    const char c = id[kStem.size()];
    if (c < '1' || c > '0' + kHeroBays) return false;
    if (index) *index = c - '1';
    return true;
}

} // namespace ftcore
