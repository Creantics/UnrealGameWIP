#include "ftcore/HeroCorner.h"

#include "ftcore/MassingDress.h"
#include "ftcore/Roads.h"
#include "ftcore/WindowFacade.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

    // A pair flanking each lit bay's entrance: under the pent on a lattice front, under the eave on a glazed one.
    out.lanternCount = 0;
    out.pairCount = 0;
    HeroPoint pairHook[kHeroLargeLanterns / 2];
    for (int i = 0; i < kHeroBays && out.pairCount < kHeroLargeLanterns / 2; ++i) {
        if (!spec.bayLit[i]) continue;
        const int p = out.pairCount++;
        const bool pent = spec.bayKoshi[i];
        const double south = spec.bayY0 + spec.bayWidth * i;
        const double y = pent ? south + spec.bayWidth - spec.koshiEntranceFromNorth : south + 0.5 * spec.bayWidth;
        out.pairBay[p] = i;
        out.entranceY[p] = y;
        const double scale = pent ? spec.pentScale : spec.largeScale;
        const double gap = pent ? spec.pentPairGap : spec.largePairGap;
        pairHook[p] = {spec.laneX1 - (pent ? spec.pentHookOut : spec.largeOut), y, g + (pent ? spec.pentHookUp : spec.eaveUp)};
        for (int side = -1; side <= 1; side += 2) {
            HeroLantern& l = out.lanterns[out.lanternCount++];
            l.large = true;
            l.pent = pent;
            l.string = -1;
            l.bay = i;
            l.scale = scale;
            // The mesh hangs from its hook at its scale, so its centre drops that times its own hook-to-centre.
            l.hook = {pairHook[p].x, y + 0.5 * side * gap, pairHook[p].z};
            l.centre = {l.hook.x, l.hook.y, l.hook.z - (spec.eaveUp - spec.largeUp) * scale};
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

    out.lampCount = 0;
    for (int s = 0; s < kHeroStrings; ++s) {
        out.lamps[out.lampCount++] = {out.stringLamp[s], spec.stringLampGain, HeroLampKind::String, s};
    }
    for (int p = 0; p < out.pairCount; ++p) {
        const bool pent = spec.bayKoshi[out.pairBay[p]];
        out.lamps[out.lampCount++] = {{pairHook[p].x, pairHook[p].y, pairHook[p].z - spec.pairLampUnder}, spec.pairLampGain, HeroLampKind::Pair, p};
        out.lamps[out.lampCount++] = {{spec.laneX1 - spec.entranceLampOut, out.entranceY[p], g + (pent ? spec.entranceLampUpPent : spec.entranceLampUpEave)},
                                      spec.entranceLampGain, HeroLampKind::Entrance, p};
    }
    out.eye ={spec.eye[0], spec.eye[1], spec.eye[2]};
    out.at = {spec.at[0], spec.at[1], spec.at[2]};
    out.lensMm = spec.lensMm;
}

void lampPoolMerge(const LampPoolSet* sets, std::size_t setCount, std::vector<LampPoolSlot>& out) {
    out.clear();
    for (std::size_t s = 0; sets && s < setCount; ++s) {
        const LampPoolSet& set = sets[s];
        for (std::size_t i = 0; set.at && i < set.count; ++i) {
            LampPoolSlot slot;
            slot.at = set.at[i];
            slot.gain = lampSlotGain(set.gains, set.gainCount, i);
            slot.set = static_cast<int>(s);
            slot.index = i;
            out.push_back(slot);
        }
    }
}

std::size_t lampPoolClashes(const std::vector<LampPoolSlot>& slots, double tolerance, std::size_t* firstA,
                            std::size_t* firstB) {
    const double t2 = tolerance * tolerance;
    std::size_t clashes = 0;
    for (std::size_t a = 0; a < slots.size(); ++a) {
        for (std::size_t b = a + 1; b < slots.size(); ++b) {
            if (slots[a].set == slots[b].set) {
                continue;
            }
            const double dx = slots[a].at.x - slots[b].at.x;
            const double dy = slots[a].at.y - slots[b].at.y;
            const double dz = slots[a].at.z - slots[b].at.z;
            if (dx * dx + dy * dy + dz * dz < t2) {
                if (clashes == 0) {
                    if (firstA) *firstA = a;
                    if (firstB) *firstB = b;
                }
                ++clashes;
            }
        }
    }
    return clashes;
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

int heroCornerShopSlots(const HeroCornerSpec& spec, HeroShopSlot out[kHeroShopSlots]) {
    int n = 0;
    for (int i = 0; i < kHeroBays; ++i) {
        if (!spec.bayLit[i]) continue;
        HeroShopSlot& s = out[n++];
        s.at.x = spec.laneX1 + spec.shopLightIn;
        s.at.y = spec.bayY0 + spec.bayWidth * (i + 0.5);
        s.at.z = spec.ground + spec.shopLightUp;
        s.at.face = 180.0;
        s.at.faceIndex = kHeroShopFace;
        s.pitch = spec.shopLightPitch;
        s.gain = spec.shopLightGain;
        s.bay = i;
        s.back = false;
    }
    for (int i = 0; i < kHeroBays; ++i) {
        if (!spec.bayLit[i] || !spec.shopBack[i]) continue;
        HeroShopSlot& s = out[n++];
        s.at.x = spec.laneX1 + spec.shopBackIn;
        s.at.y = spec.bayY0 + spec.bayWidth * (i + 0.5);
        s.at.z = spec.ground + spec.shopBackUp;
        s.at.face = 0.0;
        s.at.faceIndex = 0;
        s.pitch = spec.shopBackPitch;
        s.gain = spec.shopBackGain;
        s.bay = i;
        s.back = true;
    }
    return n;
}

int heroCornerAreaLights(const HeroCornerSpec& spec, HeroAreaLight out[kHeroAreaLights]) {
    int n = 0;
    const double paving = spec.ground + spec.paveTop;
    for (int i = 0; i < kHeroBays; ++i) {
        if (!spec.bayLit[i]) continue;
        HeroAreaLight& l = out[n++];
        l = HeroAreaLight{};
        const double north = spec.bayY0 + spec.bayWidth * (i + 1);
        l.x = spec.laneX1 + spec.frontIn;
        if (spec.bayKoshi[i]) {
            // The lattice room runs koshiRoomX0..X1 south of the bay's north edge.
            l.y = north - 0.5 * (spec.koshiRoomX0 + spec.koshiRoomX1);
            l.width = spec.koshiRoomX1 - spec.koshiRoomX0;
        } else {
            l.y = north - 0.5 * spec.bayWidth;
            l.width = std::min(spec.glazedFrontWidth, spec.bayWidth);
        }
        l.z = paving + spec.frontUp;
        l.height = spec.frontHeight;
        l.fx = -1.0;
        l.fy = 0.0;
        // Past the lane's far side at the front's ends, with a margin for the falloff's tail.
        l.reach = std::hypot(l.x - spec.laneX0, 0.5 * l.width) + 1.5;
        l.bay = i;
    }
    const double length = (spec.laneY1 - spec.laneY0) / kHeroSkyFloors;
    for (int k = 0; k < kHeroSkyFloors; ++k) {
        HeroAreaLight& s = out[n++];
        s = HeroAreaLight{};
        s.x = spec.laneX0 - spec.skyOut;
        s.y = spec.laneY0 + length * (k + 0.5);
        s.z = paving + spec.skyUp;
        s.width = length;
        s.height = spec.skyHeight;
        s.fx = 1.0;
        // Past the shop faces at its length's ends (a little beyond, so neighbours overlap).
        s.reach = std::hypot(spec.laneX1 - s.x, 0.5 * length + 1.0) + 1.5;
        s.sky = true;
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

int heroCornerProbes(const HeroCornerSpec& spec, const HeroCornerLayout& layout, HeroProbe* out, int max) {
    int n = 0;
    const double g = spec.ground;
    const auto add = [&](const char* fmt, int a, double b, const HeroPoint& at) {
        if (n >= max) return;
        HeroProbe& p = out[n++];
        std::snprintf(p.name, sizeof(p.name), fmt, a, b);
        p.at = at;
    };
    constexpr double probeRoomIn = 2.5;
    for (std::size_t i = 0; i < layout.lanternCount; ++i) {
        const HeroLantern& l = layout.lanterns[i];
        if (l.large) add("lantern bay%d y%.1f", l.bay + 1, l.centre.y, l.centre);
    }
    for (int p = 0; p < layout.pairCount; ++p) {
        const int bay = layout.pairBay[p];
        const bool pent = spec.bayKoshi[bay];
        const double y = layout.entranceY[p];
        const double gap = pent ? spec.pentPairGap : spec.largePairGap;
        for (int side = -1; side <= 1; side += 2) {
            const double fy = y + 0.5 * side * gap;
            add("face bay%d y%.1f z2.0", bay + 1, fy, {spec.laneX1, fy, g + 2.0});
            add("face bay%d y%.1f z1.0", bay + 1, fy, {spec.laneX1, fy, g + 1.0});
        }
        add("lane bay%d y%.1f", bay + 1, y, {spec.laneX1 - 1.5, y, g + spec.paveTop});
        // The back wall seen through the entrance: in a lattice room under the noren's foot (it hangs 1.55..2.2 m), else
        // the room's top third.
        add("backwall bay%d y%.1f", bay + 1, y, {spec.laneX1 + probeRoomIn, y, g + (pent ? 1.2 : 2.2)});
        if (pent) {
            add("noren bay%d y%.1f", bay + 1, y, {spec.laneX1 - 0.13, y, g + 1.9});
            const double north = spec.bayY0 + spec.bayWidth * (bay + 1);
            add("lattice bay%d y%.1f", bay + 1, north - 1.5, {spec.laneX1 - 0.1, north - 1.5, g + 1.4});
        }
    }
    for (int i = 0; i < kHeroBays; ++i) {
        if (spec.bayLit[i]) continue;
        const double y = spec.bayY0 + spec.bayWidth * (i + 0.5);
        add("dark bay%d y%.1f z1.5", i + 1, y, {spec.laneX1, y, g + 1.5});
    }
    return n;
}

bool heroCornerKeepsLampOut(const HeroCornerSpec& spec, double x, double y) {
    return x >= spec.lampClearX0 && x <= spec.lampClearX1 && y >= spec.lampClearY0 && y <= spec.lampClearY1;
}

const char* heroPieceName(HeroPiece piece) {
    switch (piece) {
    case HeroPiece::Shelf: return "ShelfUnit";
    case HeroPiece::Counter: return "Counter";
    case HeroPiece::HangGoods: return "HangGoods";
    case HeroPiece::Lantern: return "LanternSmall";
    case HeroPiece::Crate: return "Crate";
    case HeroPiece::Bucket: return "Bucket";
    case HeroPiece::Figure: return "Figure";
    case HeroPiece::StockPot: return "StockPot";
    case HeroPiece::Steam: return "Steam";
    case HeroPiece::Jars: return "GoodsJars";
    case HeroPiece::Sacks: return "GoodsSacks";
    case HeroPiece::Bottles: return "GoodsBottles";
    case HeroPiece::GoodsHanging: return "GoodsHanging";
    case HeroPiece::UmbrellaStand: return "UmbrellaStand";
    case HeroPiece::SignLarge: return "HangSignL";
    case HeroPiece::SignSmall: return "HangSignS";
    case HeroPiece::Banner: return "Banner";
    case HeroPiece::Bicycle: return "Bicycle";
    case HeroPiece::Gate: return "Gate";
    case HeroPiece::GateLantern: return "LanternLarge";
    case HeroPiece::Scooter: return "Scooter";
    case HeroPiece::Count: break;
    }
    return "";
}

bool heroItemFootprint(const HeroItem& item, double& x0, double& x1, double& y0, double& y1) {
    // The footprint at face (0, -1) about the origin: lx across, ly from front (-) to back (+).
    double lx0 = 0.0, lx1 = 0.0, ly0 = 0.0, ly1 = 0.0;
    const auto set = [&](double a, double b, double c, double d) { lx0 = a; lx1 = b; ly0 = c; ly1 = d; };
    switch (item.piece) {
    case HeroPiece::Shelf: set(-0.9, 0.9, -0.4, 0.0); break;    // origin at its back face
    case HeroPiece::Counter: set(-1.2, 1.2, -0.6, 0.0); break;  // origin at its back face
    case HeroPiece::Crate: set(-0.25, 0.25, -0.25, 0.25); break;
    case HeroPiece::Bucket: set(-0.17, 0.17, -0.17, 0.17); break;
    case HeroPiece::Figure: set(-0.25, 0.25, -0.25, 0.25); break;
    case HeroPiece::Sacks: set(-0.3, 0.3, -0.3, 0.3); break;
    case HeroPiece::UmbrellaStand: set(-0.15, 0.15, -0.15, 0.15); break;
    case HeroPiece::Bicycle: set(-0.3, 0.3, -0.9, 0.9); break;
    case HeroPiece::Scooter: set(-0.35, 0.35, -0.9, 0.9); break;
    default: return false;
    }
    // Local x runs along (-faceY, faceX), local +y along -face.
    const double ax = -item.faceY, ay = item.faceX, bx = -item.faceX, by = -item.faceY;
    x0 = y0 = 1e30;
    x1 = y1 = -1e30;
    for (int k = 0; k < 4; ++k) {
        const double lx = (k & 1) ? lx1 : lx0, ly = (k & 2) ? ly1 : ly0;
        const double px = item.at.x + lx * ax + ly * bx, py = item.at.y + lx * ay + ly * by;
        x0 = std::min(x0, px);
        x1 = std::max(x1, px);
        y0 = std::min(y0, py);
        y1 = std::max(y1, py);
    }
    return true;
}

int heroRoomFurnish(const HeroRoomSpec& room, HeroItem* out, int max) {
    int n = 0;
    const double W = room.width, D = room.depth, H = room.height;
    // A piece goes in only where it fits: inside the room, out of the entrance's threshold, clear of what stands there.
    const auto add = [&](HeroPiece piece, double x, double y, double z, double fx = 0.0, double fy = -1.0) {
        if (n >= max) return false;
        HeroItem it;
        it.piece = piece;
        it.at = {x, y, z};
        it.faceX = fx;
        it.faceY = fy;
        double x0 = 0.0, x1 = 0.0, y0 = 0.0, y1 = 0.0;
        if (!heroItemFootprint(it, x0, x1, y0, y1)) {
            if (x < 0.0 || x > W || y < 0.0 || y > D) return false;
        } else {
            if (x0 < -1e-9 || x1 > W + 1e-9 || y0 < -1e-9 || y1 > D + 1e-9) return false;
            if (x1 > room.doorX0 && x0 < room.doorX1 && y0 < room.doorClear) return false;
            for (int i = 0; i < n; ++i) {
                double a0 = 0.0, a1 = 0.0, b0 = 0.0, b1 = 0.0;
                if (heroItemFootprint(out[i], a0, a1, b0, b1) && x0 < a1 - 1e-6 && a0 < x1 - 1e-6 && y0 < b1 - 1e-6 && b0 < y1 - 1e-6) return false;
            }
        }
        out[n++] = it;
        return true;
    };
    if (W < 1.0 || D < 0.6 || H < 1.8) return 0;
    const double counterH = 0.95;
    if (!room.eatery) {
        // Two shelf units on the back wall (the second where the room is wide enough), a crate in the far corner.
        add(HeroPiece::Shelf, W - 1.0, D - 0.04, 0.0);
        if (W >= 3.9) add(HeroPiece::Shelf, W - 2.9, D - 0.04, 0.0);
        const double shelfLeft = W >= 3.9 ? W - 3.8 : W - 1.9;
        if (shelfLeft >= 0.75 && D >= 1.2) add(HeroPiece::Crate, 0.4, D - 0.4, 0.0);
        // The counter behind the lattice, short of the entrance, with an aisle to the shelves; jars and bottles on it.
        const double cx = std::min(1.5, room.doorX0 - 0.15 - 1.2);
        const double cBack = 1.6;
        if (cx >= 1.25 && D - 0.44 - cBack >= 0.6 && add(HeroPiece::Counter, cx, cBack, 0.0)) {
            add(HeroPiece::Jars, cx - 0.6, cBack - 0.3, counterH);
            add(HeroPiece::Bottles, cx + 0.55, cBack - 0.3, counterH);
        }
        if (room.doorX0 >= 0.8 && D >= 1.0) add(HeroPiece::Sacks, room.doorX0 - 0.35, 0.4, 0.0);
        add(HeroPiece::HangGoods, std::min(1.0, 0.25 * W), std::min(0.9, 0.4 * D), H - 0.06);
        add(HeroPiece::HangGoods, std::min(2.3, 0.5 * W), std::min(1.2, 0.5 * D), H - 0.06);
        if (D >= 1.6) add(HeroPiece::GoodsHanging, std::min(room.doorX0 + 0.6, W - 0.4), D - 0.8, H - 0.06);
        add(HeroPiece::Lantern, 0.55 * W, 0.5 * D, H - 0.04);
        return n;
    }
    // The eatery: the counters 0.55 m off the back wall (the cook's side), clear of the entrance's threshold.
    const double cBack = D - 0.55;
    bool two = W >= 4.9;
    bool counters = cBack - 0.6 >= room.doorClear;
    if (counters) {
        if (two) {
            counters = add(HeroPiece::Counter, 0.5 * W - 1.2, cBack, 0.0);
            two = counters && add(HeroPiece::Counter, 0.5 * W + 1.2, cBack, 0.0);
        } else {
            counters = add(HeroPiece::Counter, 0.5 * W, cBack, 0.0);
        }
    }
    if (counters) {
        // The cook behind the first counter with its back to the lane; the stock pot steaming on the last.
        add(HeroPiece::Figure, std::clamp(1.3, 0.3, W - 0.3), D - 0.28, 0.0, 0.0, 1.0);
        const double potX = two ? std::min(W - 1.4, 0.5 * W + 2.2) : 0.5 * W + 0.6;
        add(HeroPiece::StockPot, potX, cBack - 0.3, counterH);
        add(HeroPiece::Steam, potX, cBack - 0.3, counterH + 0.32);
        add(HeroPiece::Bottles, two ? 0.5 * W - 1.9 : 0.5 * W - 0.7, cBack - 0.3, counterH);
    }
    if (W >= 2.4 && D >= 1.0) add(HeroPiece::Shelf, W - 1.0, D - 0.04, 0.0);
    if (room.doorX0 >= 0.6) add(HeroPiece::Bucket, 0.3, 0.3, 0.0);
    add(HeroPiece::HangGoods, std::min(1.2, 0.3 * W), std::min(0.8, 0.3 * D), H - 0.06);
    add(HeroPiece::Lantern, 0.35 * W, std::min(0.8, 0.4 * D), H - 0.04);
    return n;
}

int heroCornerDressing(const HeroCornerSpec& spec, const HeroCornerLayout& layout, HeroItem* out, int max) {
    int n = 0;
    const double g = spec.ground, floorZ = spec.ground + spec.paveTop;
    const auto add = [&](HeroPiece piece, double x, double y, double z, double fx, double fy, int bay) -> HeroItem* {
        if (n >= max) return nullptr;
        HeroItem& it = out[n++];
        it = HeroItem{};
        it.piece = piece;
        it.at = {x, y, z};
        const double len = std::sqrt(fx * fx + fy * fy);
        it.faceX = len > 0.0 ? fx / len : 0.0;
        it.faceY = len > 0.0 ? fy / len : -1.0;
        it.bay = bay;
        return &it;
    };
    const auto bayOf = [&](double y) { return std::clamp(static_cast<int>(std::floor((y - spec.bayY0) / spec.bayWidth)), 0, kHeroBays - 1); };

    // Hanging signs off the shop face, arms out over the lane.
    const auto blocked = [&](double y) {
        for (std::size_t i = 0; i < layout.lanternCount; ++i) {
            const HeroLantern& l = layout.lanterns[i];
            if (l.large && std::abs(y - l.hook.y) < spec.signClear + 0.5 * spec.largeDiameter * l.scale) return true;
        }
        for (int s = 0; s < kHeroStrings; ++s)
            if (std::abs(y - spec.stringY[s]) < spec.signClear) return true;
        for (int p = 0; p < layout.pairCount; ++p)
            if (std::abs(y - layout.entranceY[p]) < spec.entranceHalf + spec.signClear) return true;
        return false;
    };
    const double signEnd = spec.laneY1 - spec.gateBack - 1.5;
    double last = -1e30;
    int signs = 0;
    for (double c = spec.laneY0 + 1.0; c <= signEnd; c += spec.signPitch) {
        double y = c;
        bool found = false;
        for (int k = 0; k <= 10 && !found; ++k) {
            const double t = c + (k % 2 ? 0.25 : -0.25) * ((k + 1) / 2);
            if (t < spec.laneY0 + 0.5 || t > signEnd || t - last < 2.5 || blocked(t)) continue;
            y = t;
            found = true;
        }
        if (!found) continue;
        const int bay = bayOf(y);
        const bool large = signs % 2 == 0;
        const double up = spec.bayKoshi[bay] ? spec.signUpPent[large ? 0 : 1] : spec.signUpEave[large ? 0 : 1];
        add(large ? HeroPiece::SignLarge : HeroPiece::SignSmall, spec.laneX1, y, g + up, -1.0, 0.0, bay);
        last = y;
        ++signs;
    }

    // Banners from the eave's edge over the dark bays, their faces down the lane.
    for (int b = 0; b < kHeroBays; ++b) {
        if (spec.bayLit[b] || spec.bayKoshi[b]) continue;
        const double y0 = spec.bayY0 + spec.bayWidth * b;
        for (const double f : {0.25, 0.75}) {
            const double y = y0 + f * spec.bayWidth;
            bool nearString = false;
            for (int s = 0; s < kHeroStrings; ++s) nearString = nearString || std::abs(y - spec.stringY[s]) < 1.0;
            if (nearString || y > spec.laneY1 - spec.gateBack - 0.6) continue;
            add(HeroPiece::Banner, spec.laneX1 - spec.eaveOut + spec.bannerIn, y, g + spec.bannerUp, 0.0, -1.0, b);
        }
    }

    // Clutter at the walls of the dark bays, an umbrella stand by the glazed shop's door.
    int firstDark = -1, lastDark = -1;
    for (int b = 0; b < kHeroBays; ++b) {
        if (spec.bayLit[b]) continue;
        if (firstDark < 0) firstDark = b;
        lastDark = b;
    }
    if (firstDark >= 0) {
        const double y0 = spec.bayY0 + spec.bayWidth * firstDark;
        add(HeroPiece::Crate, spec.laneX1 - 0.32, y0 + 2.0, floorZ, -1.0, 0.0, firstDark);
        add(HeroPiece::Crate, spec.laneX1 - 0.32, y0 + 2.04, floorZ + 0.42, -0.99, 0.14, firstDark);
        add(HeroPiece::Crate, spec.laneX1 - 0.32, y0 + 2.62, floorZ, -1.0, -0.08, firstDark);
        add(HeroPiece::Bucket, spec.laneX1 - 0.25, y0 + 3.3, floorZ, -1.0, 0.0, firstDark);
    }
    if (lastDark >= 0 && lastDark != firstDark) {
        const double y0 = spec.bayY0 + spec.bayWidth * lastDark;
        add(HeroPiece::Bucket, spec.laneX1 - 0.25, y0 + 1.1, floorZ, -1.0, 0.0, lastDark);
        add(HeroPiece::Bicycle, spec.laneX1 - 0.4, y0 + 3.2, floorZ, 0.0, -1.0, lastDark);
    }
    for (int p = 0; p < layout.pairCount; ++p) {
        if (spec.bayKoshi[layout.pairBay[p]]) continue;
        add(HeroPiece::UmbrellaStand, spec.laneX1 - 0.25, layout.entranceY[p] - spec.entranceHalf - 0.45, floorZ, -1.0, 0.0, layout.pairBay[p]);
        break;
    }

    // The far end: the gate across the lane, its lanterns under the tie beam, two scooters parked short of it.
    const double gx = 0.5 * (spec.laneX0 + spec.laneX1), gy = spec.laneY1 - spec.gateBack;
    add(HeroPiece::Gate, gx, gy, floorZ, 0.0, -1.0, -1);
    for (int k = 0; k < 4; ++k) {
        if (HeroItem* l = add(HeroPiece::GateLantern, gx + spec.gateHookX[k], gy, g + spec.gateHookUp, 0.0, -1.0, -1)) {
            l->scale = spec.gateLanternScale;
            l->marked = k % 2 == 1;
        }
    }
    add(HeroPiece::Scooter, spec.laneX1 - 1.2, gy - 1.4, floorZ, 0.0, -1.0, -1);
    add(HeroPiece::Scooter, spec.laneX0 + 1.0, gy - 1.2, floorZ, 0.2, -1.0, -1);
    return n;
}

} // namespace ftcore
