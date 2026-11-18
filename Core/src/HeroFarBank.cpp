#include "ftcore/HeroFarBank.h"

#include <cmath>

namespace ftcore {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::uint32_t mix(std::uint32_t x) {
    x += 0x9e3779b9u;
    x ^= x >> 16;
    x *= 0x85ebca6bu;
    x ^= x >> 13;
    x *= 0xc2b2ae35u;
    x ^= x >> 16;
    return x;
}

// [a, b] less the spans in clear, as up to maxOut free stretches, south to north. Allocates nothing.
int freeStretches(double a, double b, const HeroSpan* clear, int clearCount, HeroSpan* out, int maxOut) {
    HeroSpan sorted[kFarClearMax];
    int n = 0;
    for (int i = 0; i < clearCount && i < kFarClearMax; ++i) {
        const HeroSpan s = clear[i];
        int j = n;
        while (j > 0 && sorted[j - 1].y0 > s.y0) {
            sorted[j] = sorted[j - 1];
            --j;
        }
        sorted[j] = s;
        ++n;
    }
    int count = 0;
    double at = a;
    for (int i = 0; i <= n && count < maxOut; ++i) {
        const double end = (i < n) ? sorted[i].y0 : b;
        const double stop = end < b ? end : b;
        if (stop - at > 1e-9) out[count++] = HeroSpan{at, stop};
        if (i < n && sorted[i].y1 > at) at = sorted[i].y1;
        if (at >= b) break;
    }
    return count;
}

bool inSpan(double y, const HeroSpan* spans, int count) {
    for (int i = 0; i < count && i < kFarClearMax; ++i)
        if (y >= spans[i].y0 && y <= spans[i].y1) return true;
    return false;
}

struct Builder {
    HeroFarBankLayout& out;

    void box(HeroPoint c, double along, double depth, double height, double outX, double outY, double tilt,
             FarShape shape, FarMat mat, bool solid) {
        if (out.boxCount >= kFarBoxesMax) {
            out.overflow = true;
            return;
        }
        FarBox& b = out.boxes[out.boxCount++];
        b.centre = c;
        b.along = along;
        b.depth = depth;
        b.height = height;
        b.outX = outX;
        b.outY = outY;
        b.tiltDeg = tilt;
        b.shape = shape;
        b.mat = mat;
        b.solid = solid;
    }
    void piece(FarKit kit, HeroPoint at, bool lit, int front) {
        if (out.pieceCount >= kFarPiecesMax) {
            out.overflow = true;
            return;
        }
        FarPiece& p = out.pieces[out.pieceCount++];
        p.kit = kit;
        p.at = at;
        p.outX = 1.0;
        p.outY = 0.0;
        p.lit = lit;
        p.front = front;
    }
    void lantern(HeroPoint hook, double scale, bool large) {
        if (out.lanternCount >= kFarLanternsMax) {
            out.overflow = true;
            return;
        }
        out.lanterns[out.lanternCount++] = FarLantern{hook, scale, large};
    }
    int lamp(HeroPoint at, float gain) {
        if (out.lampCount >= kFarLampsMax) {
            out.overflow = true;
            return -1;
        }
        out.lamps[out.lampCount] = FarLamp{at, gain};
        return out.lampCount++;
    }
};

void layFronts(const HeroFarBankSpec& spec, Builder& b) {
    HeroSpan stretch[kFarClearMax + 1];
    const int stretches =
        freeStretches(spec.frontY0, spec.frontY1, spec.frontClear, spec.frontClearCount, stretch, kFarClearMax + 1);
    const double widths[3] = {4.5, 5.4, 6.3};
    const double rise = spec.roofRise, run = spec.roofDepth;
    const double alpha = std::atan2(rise, run);
    const double alphaDeg = alpha * 180.0 / kPi;
    const double ca = std::cos(alpha), sa = std::sin(alpha);
    std::uint32_t k = 0;
    int twoRun = 0;
    for (int s = 0; s < stretches; ++s) {
        const double length = stretch[s].y1 - stretch[s].y0;
        double chosen[kFarFrontsMax];
        std::uint32_t hashes[kFarFrontsMax];
        int n = 0;
        double used = 0.0;
        while (length - used >= widths[0] - 1e-9 && n < kFarFrontsMax) {
            const std::uint32_t h = mix(spec.seed ^ (0x51ed27u * (k + 1u)));
            ++k;
            int fit = 0;
            while (fit < 3 && widths[fit] <= length - used + 1e-9) ++fit;
            const double w = widths[h % static_cast<std::uint32_t>(fit)];
            chosen[n] = w;
            hashes[n] = h;
            ++n;
            used += w;
        }
        double y = stretch[s].y0 + 0.5 * (length - used);
        for (int i = 0; i < n; ++i) {
            if (b.out.frontCount >= kFarFrontsMax) {
                b.out.overflow = true;
                return;
            }
            const double w = chosen[i];
            const std::uint32_t h = hashes[i];
            const int index = b.out.frontCount;
            FarFront& f = b.out.fronts[b.out.frontCount++];
            f.y0 = y;
            f.y1 = y + w;
            const double ym = y + 0.5 * w;
            f.ground = heroFarGround(spec, ym);
            double margin = 0.0;
            if (w > 6.0) {
                if (h & 1u) {
                    f.shop = FarKit::ShopShutter63;
                    f.lit = false;
                } else {
                    f.shop = FarKit::ShopKoshi54;
                    f.lit = true;
                    margin = 0.45;
                }
            } else if (w > 5.0) {
                f.shop = FarKit::ShopKoshi54;
                f.lit = (h % 5u) != 0u;
            } else {
                f.shop = FarKit::ShopGlazed36;
                f.lit = (h % 4u) != 0u;
                margin = 0.45;
            }
            f.storeys = ((h >> 4) % 5u) < 3u ? 2 : 1;
            if (f.storeys == 2 && twoRun >= 2) f.storeys = 1;
            for (int m = 0; m < spec.mastCount && m < kFarClearMax; ++m)
                if (spec.mastY[m] > f.y0 - spec.mastClear && spec.mastY[m] < f.y1 + spec.mastClear) f.storeys = 1;
            twoRun = f.storeys == 2 ? twoRun + 1 : 0;
            const double storey1 = f.shop == FarKit::ShopKoshi54 ? 3.0 : 3.6;
            const double floor1 = f.ground + storey1;
            f.eave = floor1 + (f.storeys == 2 ? spec.upperStorey : 0.0);
            f.ridge = f.eave + rise;
            const double x = spec.frontX;

            // The kit: the shop on the ground storey, bays and a pent over it on a second, the lean-to on top.
            b.piece(f.shop, HeroPoint{x, y + margin, f.ground}, f.lit, index);
            const int tiles = static_cast<int>(std::lround(w / 0.9));
            if (f.storeys == 2) {
                const int bays = static_cast<int>(std::floor((w - 0.9) / 1.8 + 1e-9));
                const double bayStart = y + 0.5 * (w - 1.8 * bays);
                for (int j = 0; j < bays; ++j)
                    b.piece(FarKit::BayWin18, HeroPoint{x, bayStart + 1.8 * j, floor1},
                            f.lit && ((h >> (8 + j)) & 1u) == 0u, index);
                for (int j = 0; j < tiles; ++j) b.piece(FarKit::EavePent09, HeroPoint{x, y + 0.9 * j, floor1}, false, index);
            }
            for (int j = 0; j < tiles; ++j) {
                b.piece(FarKit::TileSlope54, HeroPoint{x, y + 0.9 * j, f.eave}, false, index);
                b.piece(FarKit::Ridge09, HeroPoint{x, y + 0.9 * j, f.eave}, false, index);
            }

            // The stucco body behind and under the kit.
            const double foot = f.ground - spec.bodySink;
            const double gDepth = run - spec.groundInset;
            b.box(HeroPoint{x - spec.groundInset - 0.5 * gDepth, ym, 0.5 * (foot + floor1)}, w, gDepth, floor1 - foot,
                  1.0, 0.0, 0.0, FarShape::Cube, FarMat::Stucco, true);
            if (margin > 0.0) {
                const double pd = spec.groundInset + 0.05;
                for (int side = 0; side < 2; ++side) {
                    const double py = side == 0 ? y + 0.5 * margin : y + w - 0.5 * margin;
                    b.box(HeroPoint{x - 0.5 * pd, py, 0.5 * (foot + floor1)}, margin, pd, floor1 - foot, 1.0, 0.0,
                          0.0, FarShape::Cube, FarMat::Stucco, true);
                }
            }
            if (f.storeys == 2) {
                const double inset = 0.24, d = run - inset;
                b.box(HeroPoint{x - inset - 0.5 * d, ym, 0.5 * (floor1 + f.eave)}, w, d, f.eave - floor1, 1.0, 0.0,
                      0.0, FarShape::Cube, FarMat::Stucco, true);
            }
            // The lean-to's fill under its tiles: a slab tilted to the slope (its top 0.08 under it), its back 0.5 m a
            // box up to the slope.
            const double slab = run * ca, thick = run * sa, under = 0.08;
            b.box(HeroPoint{x - 0.5 * slab * ca - (0.5 * thick + under) * sa, ym,
                            f.eave + 0.5 * slab * sa - (0.5 * thick + under) * ca},
                  w, slab, thick, 1.0, 0.0, alphaDeg, FarShape::Cube, FarMat::Stucco, true);
            const double backTop = f.eave + (run - 0.5) * rise / run - 0.03, backFoot = f.eave - 0.3;
            b.box(HeroPoint{x - run + 0.25, ym, 0.5 * (backTop + backFoot)}, w, 0.5, backTop - backFoot, 1.0, 0.0,
                  0.0, FarShape::Cube, FarMat::Stucco, true);

            // Lanterns: a lit front's pair about its entrance with a lamp slot between; a dark one's single small.
            const double hookZ = floor1 - spec.hookUnder, hookX = x + spec.hookOut;
            if (f.lit) {
                b.lantern(HeroPoint{hookX, ym - spec.pairHalf, hookZ}, spec.lanternScale, true);
                b.lantern(HeroPoint{hookX, ym + spec.pairHalf, hookZ}, spec.lanternScale, true);
                b.lamp(HeroPoint{hookX, ym, hookZ - 0.1}, spec.pairGain);
            } else {
                b.lantern(HeroPoint{hookX, ym, hookZ}, spec.lanternScale, false);
            }
            y += w;
        }
    }
}

void layRope(const HeroFarBankSpec& spec, HeroFarBankLayout& out) {
    HeroSpan stretch[kFarClearMax + 1];
    const int stretches =
        freeStretches(spec.ropeY0, spec.ropeY1, spec.ropeClear, spec.ropeClearCount, stretch, kFarClearMax + 1);
    for (int s = 0; s < stretches; ++s) {
        const double a = stretch[s].y0, length = stretch[s].y1 - a;
        auto post = [&](double y) {
            if (out.postCount >= kQuayPostsMax) {
                out.overflow = true;
                return;
            }
            out.posts[out.postCount++] = QuayPost{HeroPoint{spec.ropeX, y, spec.nearGround}, spec.postHeight,
                                                  spec.postDiameter};
        };
        if (length < spec.postPitchMin) {
            post(a + 0.5 * length);
            continue;
        }
        const int spans = static_cast<int>(std::ceil(length / spec.postPitch - 1e-9));
        const double pitch = length / spans;
        for (int i = 0; i <= spans; ++i) post(a + pitch * i);
        for (int i = 0; i < spans; ++i) {
            if (out.ropeCount >= kQuayPostsMax) {
                out.overflow = true;
                break;
            }
            RopeSpan& r = out.ropes[out.ropeCount++];
            for (int p = 0; p < kRopePoints; ++p) {
                const double t = static_cast<double>(p) / (kRopePoints - 1);
                r.pt[p] = HeroPoint{spec.ropeX, a + pitch * (i + t), heroRopeZ(spec, t)};
            }
        }
    }
}

void layBoats(const HeroFarBankSpec& spec, Builder& b) {
    const int count = spec.boatCount < kFarBoatsMax ? spec.boatCount : kFarBoatsMax;
    const double L = spec.boatLength, B = spec.boatBeam;
    const double cx = spec.waterX0 + spec.boatOff + 0.5 * B;
    const double top = spec.waterZ + spec.freeboard, keel = spec.waterZ - spec.draft;
    const double zMid = 0.5 * (top + keel), h = top - keel;
    const double r = 0.70710678118654752;
    for (int i = 0; i < count; ++i) {
        const double cy = spec.boatY[i];
        FarBoat& boat = b.out.boats[b.out.boatCount++];
        boat.centre = HeroPoint{cx, cy, spec.waterZ};
        boat.length = L;
        boat.beam = B;
        boat.alongX = 0.0;
        boat.alongY = 1.0;
        // The hull: a box to the bow's widest, a diamond (a square turned 45 degrees) its pointed bow.
        const double midLen = L - 0.5 * B;
        b.box(HeroPoint{cx, cy - 0.25 * B, zMid}, midLen, B, h, 1.0, 0.0, 0.0, FarShape::Cube, FarMat::Timber, false);
        b.box(HeroPoint{cx, cy + 0.5 * L - 0.5 * B, zMid}, B * r, B * r, h, r, r, 0.0, FarShape::Cube,
              FarMat::Timber, false);
        for (int side = -1; side <= 1; side += 2)
            b.box(HeroPoint{cx + side * (0.5 * B - 0.04), cy - 0.25 * B, top + 0.04}, midLen, 0.1, 0.08, 1.0, 0.0,
                  0.0, FarShape::Cube, FarMat::Dark, false);
        // A barrel canopy aft of the middle, half sunk in the hull.
        b.box(HeroPoint{cx, cy - 0.12 * L, top}, 0.35 * L, 0.8 * B, 0.8 * B, 1.0, 0.0, 0.0, FarShape::CylinderAlong,
              FarMat::Dark, false);
        // The stern pole, an arm over the water at its top, the lantern under the arm's end.
        const double poleY = cy - 0.5 * L + 0.4;
        b.box(HeroPoint{cx, poleY, top + 0.5 * spec.poleHeight - 0.05}, 0.07, 0.07, spec.poleHeight + 0.1, 1.0, 0.0,
              0.0, FarShape::CylinderUp, FarMat::Timber, false);
        const double armTop = top + spec.poleHeight;
        b.box(HeroPoint{cx + 0.22, poleY, armTop - 0.03}, 0.05, 0.5, 0.05, 1.0, 0.0, 0.0, FarShape::Cube,
              FarMat::Dark, false);
        boat.lanternHook = HeroPoint{cx + 0.42, poleY, armTop - 0.06};
        b.lantern(boat.lanternHook, spec.boatLanternScale, false);
        boat.lamp = b.lamp(HeroPoint{boat.lanternHook.x, poleY, boat.lanternHook.z - spec.boatLampUnder},
                           spec.boatLampGain);
    }
}

void layTrees(const HeroFarBankSpec& spec, HeroFarBankLayout& out) {
    std::uint32_t k = 0;
    for (double y = spec.treeY0; y <= spec.treeY1 + 1e-9; y += spec.treePitch) {
        bool crowded = false;
        for (int i = 0; i < spec.oldTreeCount && i < 8; ++i)
            if (std::fabs(y - spec.oldTreeY[i]) < spec.treeClear) crowded = true;
        if (crowded ||inSpan(y, spec.treeMouth, spec.treeMouthCount)) continue;
        if (out.treeCount >= kFarTreesMax) {
            out.overflow = true;
            break;
        }
        const std::uint32_t h = mix(spec.seed * 0x2545f491u + 0x7a3du + k++);
        FarTree& t = out.trees[out.treeCount++];
        t.foot = HeroPoint{spec.treeX, y, spec.treeGround};
        const double u = static_cast<double>(h & 0xffffu) / 65535.0;
        t.height = spec.treeHeight + spec.treeHeightVar * (2.0 * u - 1.0);
        t.yawDeg = static_cast<double>((h >> 16) % 360u);
        t.model = static_cast<int>((h >> 8) % static_cast<std::uint32_t>(kFarTreeModels));
    }
}

} // namespace

const char* farKitName(FarKit kit) {
    switch (kit) {
    case FarKit::ShopKoshi54: return "SHOP_KOSHI_54";
    case FarKit::ShopGlazed36: return "SHOP_GLAZED_36";
    case FarKit::ShopShutter63: return "SHOP_SHUTTER_63";
    case FarKit::BayWin18: return "BAY_WIN_18";
    case FarKit::EavePent09: return "EAVE_PENT_09";
    case FarKit::TileSlope54: return "TILE_SLOPE_54_09";
    case FarKit::Ridge09: return "RIDGE_09";
    case FarKit::Count: break;
    }
    return "";
}

FarKitSize farKitSize(FarKit kit) {
    switch (kit) {
    case FarKit::ShopKoshi54: return FarKitSize{5.4, 0.53, 3.0, 0.0, -0.15, 0.0};
    case FarKit::ShopGlazed36: return FarKitSize{3.6, 0.7, 3.6, 0.0, -0.45, 0.0};
    case FarKit::ShopShutter63: return FarKitSize{6.3, 0.57, 3.6, 0.0, -0.32, 0.0};
    case FarKit::BayWin18: return FarKitSize{1.8, 0.37, 3.0, 0.0, -0.12, 0.0};
    case FarKit::EavePent09: return FarKitSize{0.9, 1.095, 0.683, 0.0, -0.93, -0.50};
    case FarKit::TileSlope54: return FarKitSize{0.9, 3.403, 1.726, 0.0, -0.69, -0.47};
    case FarKit::Ridge09: return FarKitSize{0.9, 0.6, 0.37, 0.0, 2.40, 1.18};
    case FarKit::Count: break;
    }
    return FarKitSize{};
}

double heroFarGround(const HeroFarBankSpec& spec, double y) {
    return spec.farGroundAt + spec.farGroundRise * (y - spec.farGroundFromY);
}

double heroRopeZ(const HeroFarBankSpec& spec, double t) {
    return spec.nearGround + spec.ropeUp - 4.0 * spec.ropeSag * t * (1.0 - t);
}

void heroFarBankLayout(const HeroFarBankSpec& spec, HeroFarBankLayout& out) {
    out.frontCount = out.pieceCount = out.boxCount = out.lanternCount = out.lampCount = 0;
    out.postCount = out.ropeCount = out.boatCount = out.treeCount = 0;
    out.overflow = false;
    Builder b{out};
    layFronts(spec, b);
    layRope(spec, out);
    layBoats(spec, b);
    layTrees(spec, out);
}

} // namespace ftcore
