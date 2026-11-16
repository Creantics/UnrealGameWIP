#include "ftcore/HeroCorner.h"

#include "ftcore/MassingDress.h"
#include "ftcore/Roads.h"
#include "ftcore/WindowFacade.h"

#include <algorithm>
#include <cmath>
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
    out.nearClear = flatBox("HC-NEAR-CLEAR", "clear", spec.laneX0, spec.laneX1, std::min(spec.nearClearY0, spec.laneY0), spec.laneY0,
                            g - spec.paveThick, g + spec.paveTop);
    // The eave runs the row but the lattice fronts' bays, whose timber fronts carry their own pent roofs under it.
    const int eatery = std::clamp(spec.eateryBay, 0, kHeroBays - 1);
    out.eaveCount = 0;
    constexpr int kMaxEaves = static_cast<int>(sizeof(out.eaves) / sizeof(out.eaves[0]));
    for (int i = 0; i < kHeroBays && out.eaveCount < kMaxEaves;) {
        if (spec.bayKoshi[i]) {
            ++i;
            continue;
        }
        int j = i;
        while (j < kHeroBays && !spec.bayKoshi[j]) ++j;
        MassingBox& eave = out.eaves[out.eaveCount];
        eave = flatBox("", "eave", spec.laneX1 - spec.eaveOut, spec.laneX1, spec.bayY0 + spec.bayWidth * i,
                       spec.bayY0 + spec.bayWidth * j, g + spec.eaveUp, g + spec.eaveUp + spec.eaveThick);
        eave.id = "HC-EAVE-" + std::to_string(++out.eaveCount);
        i = j;
    }

    for (int i = 0; i < kHeroBays; ++i) {
        const double y0 = spec.bayY0 + spec.bayWidth * i;
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
    out.eateryBay = eatery;

    out.lanternCount = 0;
    const double largeX = spec.laneX1 - spec.largeOut;
    for (int p = 0; p < kHeroLargeLanterns / 2; ++p) {
        for (int side = -1; side <= 1; side += 2) {
            HeroLantern& l = out.lanterns[out.lanternCount++];
            l.large = true;
            l.string = -1;
            // The mesh hangs from its hook at largeScale, so its centre drops largeScale times its own hook-to-centre.
            l.hook = {largeX, spec.largePairs[p] + 0.5 * side * spec.largePairGap, g + spec.eaveUp};
            l.centre = {l.hook.x, l.hook.y, l.hook.z - (spec.eaveUp - spec.largeUp) * spec.largeScale};
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

void heroCornerLots(const HeroCornerSpec& spec, HeroLots& out) {
    const double north = spec.bayY0 + spec.bayWidth * kHeroBays;
    out.run = FrontageRun{};
    out.run.id = "HC-RUN";
    out.run.ax = spec.laneX1;
    out.run.ay = north;
    out.run.bx = spec.laneX1;
    out.run.by = spec.bayY0;
    out.run.maxDepth = spec.bayDepth;
    out.run.cornerAtA = false;

    // Per bay, south to north: the dark and lit kinds by the spec, the rest of the traits from these rows.
    struct Traits {
        RoofKind roof;
        WallSurface wall;
        std::uint8_t wallColour, trimColour, accent, timber, roofColour, signColour, upperVariant;
    };
    static constexpr Traits kTraits[kHeroBays] = {
        {RoofKind::Hip, WallSurface::Concrete, 1, 0, 2, 0, 1, 0, 1},
        {RoofKind::Flat, WallSurface::Tile, 3, 1, 0, 1, 0, 1, 0},
        {RoofKind::Gable, WallSurface::Stucco, 0, 2, 3, 2, 2, 2, 0},
        {RoofKind::Flat, WallSurface::Tile, 2, 1, 1, 0, 0, 3, 1},
        {RoofKind::Gable, WallSurface::Concrete, 4, 0, 4, 1, 1, 0, 0},
    };
    const int eatery = std::clamp(spec.eateryBay, 0, kHeroBays - 1);
    for (int i = 0; i < kHeroBays; ++i) {
        BuildingLot& lot = out.lots[i];
        lot = BuildingLot{};
        lot.run = 0;
        lot.parents.clear();
        // Along the run from its north end A: bay i's north edge is the run's s0.
        lot.s0 = north - (spec.bayY0 + spec.bayWidth * (i + 1));
        lot.s1 = lot.s0 + spec.bayWidth;
        lot.depth = spec.bayDepth;
        lot.floorZ = spec.ground;
        lot.footZ = spec.ground;
        lot.oldTop = spec.ground + spec.bayHeight;
        lot.type = spec.bayKoshi[i] ? LotType::TownHouse : spec.bayLit[i] ? LotType::Tiled : LotType::WalkUp;
        out.lit[i] = spec.bayLit[i];
        lot.floors = 2;
        lot.setback = 0.0;
        const Traits& t = kTraits[i];
        lot.roof = t.roof;
        lot.wall = t.wall;
        lot.wallColour = static_cast<std::uint8_t>(t.wallColour % kitPaletteSize(KitPalette::Wall));
        lot.trimColour = static_cast<std::uint8_t>(t.trimColour % kitPaletteSize(KitPalette::Trim));
        lot.accent = static_cast<std::uint8_t>(t.accent % kitPaletteSize(KitPalette::Accent));
        lot.timber = static_cast<std::uint8_t>(t.timber % kitPaletteSize(KitPalette::Timber));
        lot.roofColour = static_cast<std::uint8_t>(t.roofColour % kitPaletteSize(KitPalette::Roof));
        lot.signColour = static_cast<std::uint8_t>(t.signColour % kitPaletteSize(KitPalette::SignFg));
        lot.groundVariant = 0;  // the shop piece first, its blanks after
        lot.upperVariant = t.upperVariant;
        lot.signSet = 0;        // a fascia sign, no awning: the eave is the lane's cover
        lot.flatUppers = true;
        lot.clutterSeed = 0x48430000u + static_cast<std::uint32_t>(i) * 0x101u;  // no vending, bicycle or planters but the eatery's
        lot.seed = 0x48434F52u ^ (static_cast<std::uint32_t>(i + 1) * 0x9E3779B9u);
    }
    out.eatery = eatery;
}

int heroCornerShopLights(const HeroCornerSpec& spec, MassingShopLight out[kHeroBays]) {
    int n = 0;
    for (int i = 0; i < kHeroBays; ++i) {
        if (!spec.bayLit[i]) continue;
        MassingShopLight& l = out[n++];
        l.x = spec.laneX1 + spec.shopLightIn;
        l.y = spec.bayY0 + spec.bayWidth * (i + 0.5);
        l.z = spec.ground + spec.shopLightUp;
        l.face = 180.0;
        l.faceIndex = kHeroShopFace;
    }
    return n;
}

int heroCornerClearMasts(const HeroCornerSpec& spec, std::vector<StandInPoint>& points) {
    int n = 0;
    for (StandInPoint& p : points) {
        if (p.kind != StandInKind::LampMast || p.x < spec.mastClearX0 || p.x > spec.mastClearX1 || p.y < spec.mastClearY0 ||
            p.y > spec.mastClearY1) {
            continue;
        }
        p.kind = StandInKind::Railing;
        p.rot = std::fmod(p.rot + 90.0, 360.0);
        p.len = kQuayMastPlate;
        p.wid = 0.06;
        p.top = p.z + kQuayRailHigh;
        p.hasTop = p.hasZ;
        p.slope = 0.0;
        p.id += "-RAIL";
        p.tag = "railing_span";
        ++n;
    }
    return n;
}

bool heroCornerKeepsLampOut(const HeroCornerSpec& spec, double x, double y) {
    return x >= spec.lampClearX0 && x <= spec.lampClearX1 && y >= spec.lampClearY0 && y <= spec.lampClearY1;
}

} // namespace ftcore
