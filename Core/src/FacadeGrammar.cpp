// The facade grammar: one lot of ftcore/BuildingLots.h to kit pieces and plain boxes.

#include "ftcore/BuildingLots.h"

#include "ftcore/BlockGen.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

struct Course {
    const char* id;
    int units;
};

struct Emitter {
    const KitTable& kit;
    const FrontageRun& run;
    const BuildingLot& lot;
    int lotIndex;
    std::vector<KitPlacement>& out;
    std::string* error;
    bool ok = true;

    // A piece whose origin stands x m along the lot, y m in from the run line, at height z, turned yaw.
    void piece(std::string_view id, double x, double y, double z, double yaw = 0.0, double sz = 1.0, double sx = 1.0,
               PlacementRole role = PlacementRole::Piece) {
        const int p = kit.find(id);
        if (p < 0) {
            if (ok && error) *error = "the kit table has no " + std::string(id);
            ok = false;
            return;
        }
        KitPlacement k;
        k.piece = p;
        k.role = role;
        k.lot = lotIndex;
        run.toPlan(lot.s0 + x, y, k.x, k.y);
        k.z = z;
        k.yaw = run.yaw() + yaw;
        k.sx = sx;
        k.sz = sz;
        k.seed = lot.seed ^ (static_cast<std::uint32_t>(out.size() + 1) * 0x9E3779B9u);
        out.push_back(k);
    }
    // A box from (x0, y0, z0) to (x1, y1, z1) in lot terms.
    void box(PlacementRole role, double x0, double y0, double z0, double x1, double y1, double z1) {
        if (x1 - x0 < 0.01 || y1 - y0 < 0.01 || z1 - z0 < 0.01) return;
        KitPlacement k;
        k.role = role;
        k.lot = lotIndex;
        run.toPlan(lot.s0 + 0.5 * (x0 + x1), 0.5 * (y0 + y1), k.x, k.y);
        k.z = z0;
        k.yaw = run.yaw();
        k.sx = x1 - x0;
        k.sy = y1 - y0;
        k.sz = z1 - z0;
        k.seed = lot.seed ^ (static_cast<std::uint32_t>(out.size() + 1) * 0x9E3779B9u);
        out.push_back(k);
    }
};

// Lays a pattern along n grid steps, filling what is left with blank bays: variant 0 blanks after, 1 before,
// 2 half each side. Returns the courses left to right.
std::vector<Course> fill(int n, const std::vector<Course>& pattern, int variant, bool repeat) {
    std::vector<Course> body;
    int used = 0;
    for (std::size_t i = 0; !pattern.empty(); i = (i + 1) % pattern.size()) {
        if (used + pattern[i].units > n) break;
        body.push_back(pattern[i]);
        used += pattern[i].units;
        if (!repeat && i + 1 == pattern.size()) break;
    }
    const int blanks = n - used;
    const int before = variant == 1 ? blanks : variant == 2 ? blanks / 2 : 0;
    std::vector<Course> out;
    for (int k = 0; k < before; ++k) out.push_back({"BAY_BLANK_09", 1});
    out.insert(out.end(), body.begin(), body.end());
    for (int k = before; k < blanks; ++k) out.push_back({"BAY_BLANK_09", 1});
    return out;
}

std::vector<Course> groundCourses(const BuildingLot& lot) {
    const int n = lot.units();
    const int v = lot.groundVariant;
    switch (lot.type) {
    case LotType::Corner: {
        std::vector<Course> c = {{"SHOP_CORNER_63", 7}};
        const std::vector<Course> rest = fill(n - 7, {{"SHOP_GLAZED_36", 4}}, v == 1 ? 1 : 0, false);
        c.insert(c.end(), rest.begin(), rest.end());
        return c;
    }
    case LotType::Pencil: return fill(n, {{"SHOP_GLAZED_36", 4}}, v, false);
    case LotType::WalkUp:
        if (n >= 11 && v == 1) return fill(n, {{"SHOP_GLAZED_36", 4}, {"SHOP_SHUTTER_63", 7}}, 0, false);
        if (n >= 7) return fill(n, {{"SHOP_SHUTTER_63", 7}, {"SHOP_GLAZED_36", 4}}, v, false);
        return fill(n, {{"SHOP_GLAZED_36", 4}}, v, false);
    case LotType::Tiled:
        if (n >= 9 && v != 1) return fill(n, {{"SHOP_GLAZED_81", 9}}, v, false);
        return fill(n, {{"SHOP_GLAZED_36", 4}, {"SHOP_GLAZED_36", 4}}, v, false);
    case LotType::TownHouse: return fill(n, {{"SHOP_KOSHI_54", 6}}, v, false);
    }
    return {};
}

std::vector<Course> upperCourses(const BuildingLot& lot) {
    const int n = lot.units();
    const int v = lot.upperVariant;
    switch (lot.type) {
    case LotType::Corner:
        return v == 1 ? fill(n, {{"BAY_BALC_27", 3}}, 2, true) : fill(n, {{"BAY_WIN_18", 2}, {"BAY_BLANK_09", 1}}, v, true);
    case LotType::Pencil: return fill(n, {{"BAY_PENCIL_45", 5}}, v == 2 ? 1 : v, false);
    case LotType::WalkUp: return fill(n, {{"BAY_CORR_27", 3}}, v, true);
    case LotType::Tiled:
        if (v == 0) return fill(n, {{"BAY_BALC_27", 3}}, 2, true);
        if (v == 1) return fill(n, {{"BAY_WIN_18", 2}, {"BAY_BLANK_09", 1}}, 0, true);
        return fill(n, {{"BAY_BALC_27", 3}, {"BAY_WIN_18", 2}}, 2, true);
    case LotType::TownHouse: return fill(n, {{"BAY_BLANK_09", 1}, {"BAY_WIN_18", 2}}, v == 1 ? 1 : 0, true);
    }
    return {};
}

} // namespace

bool facadePlacements(const KitTable& kit, const FrontageRun& run, const BuildingLot& lot, int lotIndex,
                      std::vector<KitPlacement>& out, std::string* error) {
    Emitter e{kit, run, lot, lotIndex, out, error};
    BlockRng rng(lot.clutterSeed);
    const double W = lot.width();
    const double D = std::max(lot.depth, 3.0);
    const double y0 = lot.setback;      // the facade line
    const double g = lot.groundStorey();
    const double z0 = lot.floorZ;
    const double top = lot.top();
    const int n = lot.units();

    // Plinth: the lot's floor over the falling ground, a little proud of the facade.
    e.box(PlacementRole::Plinth, 0.0, -0.15, lot.footZ - 0.3, W, D, z0);

    // Ground storey.
    double x = 0.0;
    double blankX = -1.0; // a ground blank bay for a vending machine
    for (const Course& c : groundCourses(lot)) {
        const std::string_view id = c.id;
        const double w = c.units * kLotGrid;
        if (id == "BAY_BLANK_09") {
            e.piece(id, x, y0, z0, 0.0, g / kUpperStorey);
            if (blankX < 0.0) blankX = x;
        } else {
            e.piece(id, x, y0, z0);
            const bool shop = id != "SHOP_KOSHI_54";
            const bool awning = lot.signSet >= 2;
            if (shop && !awning) {
                const double fx = id == "SHOP_GLAZED_81" ? 2.25 : id == "SHOP_SHUTTER_63" ? 1.35 : id == "SHOP_CORNER_63" ? 1.2 : 0.0;
                const double fy = id == "SHOP_SHUTTER_63" ? -0.32 : -0.08;
                e.piece("SIGN_FASCIA_36", x + fx, y0 + fy, z0 + 2.95);
            } else if (shop) {
                if (id == "SHOP_GLAZED_36") e.piece("AWNING_27", x + 0.45, y0, z0 + 2.85);
                else if (id == "SHOP_GLAZED_81") {
                    e.piece("AWNING_36", x + 0.3, y0, z0 + 2.85);
                    e.piece("AWNING_36", x + 4.2, y0, z0 + 2.85);
                } else if (id == "SHOP_SHUTTER_63") e.piece("AWNING_36", x + 1.35, y0, z0 + 2.95);
                else e.piece("AWNING_36", x + 1.2, y0, z0 + 2.85);
            }
        }
        x += w;
    }
    if (lot.type == LotType::TownHouse)
        for (int k = 0; k < n; ++k) e.piece("EAVE_PENT_09", k * kLotGrid, y0, z0 + g);

    // Upper storeys.
    const std::vector<Course> upper = upperCourses(lot);
    double bladeX = -1.0;
    for (int f = 1; f < lot.floors; ++f) {
        const double z = z0 + g + kUpperStorey * (f - 1);
        x = 0.0;
        for (const Course& c : upper) {
            const std::string_view id = c.id;
            e.piece(id, x, y0, z);
            if (id == "BAY_WIN_18" && (rng.next() & 3u) == 0u) e.piece("AC_UNIT", x + 1.6, y0 - 0.05, z + 1.2, 0.0, 1.0, 1.0, PlacementRole::Clutter);
            if (id == "BAY_PENCIL_45" && f == 1) {
                e.piece(lot.floors >= 3 ? "SIGN_VERT_L" : "SIGN_VERT_M", x + 4.05, y0 - 0.25, z + 0.46);
            }
            if (id == "BAY_BLANK_09" && f == 1) bladeX = x + 0.45;
            x += c.units * kLotGrid;
        }
    }
    if ((lot.signSet & 1u) && lot.type != LotType::Pencil && lot.floors >= 2 && bladeX >= 0.0)
        e.piece(lot.floors >= 3 ? "SIGN_VERT_M" : "SIGN_VERT_S", bladeX, y0, z0 + g + 0.3);

    // Body: the building's mass behind the facade bays.
    if (lot.roof == RoofKind::Flat) {
        e.box(PlacementRole::Body, 0.02, y0 + 0.30, z0, W - 0.02, D, top + 0.03);
        // Parapets: front, the two sides, the back, a corner piece at the front left.
        for (int k = 0; k < n; ++k) e.piece("PARAPET_09", k * kLotGrid, y0, top);
        e.piece("PARAPET_CORNER", 0.0, y0, top);
        const double side = D - y0 - 0.25;
        const int m = static_cast<int>(std::floor(side / kLotGrid + 1e-6));
        const double rest = side - m * kLotGrid;
        for (int k = 0; k < m; ++k) {
            e.piece("PARAPET_09", 0.0, y0 + 0.25 + (k + 1) * kLotGrid, top, -90.0);
            e.piece("PARAPET_09", W, y0 + 0.25 + k * kLotGrid, top, 90.0);
        }
        if (rest > 0.1) {
            e.piece("PARAPET_09", 0.0, D, top, -90.0, 1.0, rest / kLotGrid);
            e.piece("PARAPET_09", W, D - rest, top, 90.0, 1.0, rest / kLotGrid);
        }
        for (int k = 0; k < n; ++k) e.piece("PARAPET_09", (k + 1) * kLotGrid, D, top, 180.0);
        // Roof kit, kept inside the parapets.
        const std::uint32_t r = rng.next();
        if (W >= 3.6 && D - y0 >= 4.5 && (r & 1u)) e.piece("TANK_M", 0.5, D - 2.2, top + 0.03);
        if (W >= 6.3 && D - y0 >= 5.0 && (r & 2u)) e.piece("PLANT_ROOM_S", W - 3.0, D - 3.1, top + 0.03);
        if (W >= 4.5 && (r & 12u) == 4u) e.piece("ANTENNA", W * 0.5, y0 + 1.6, top + 0.03);
    } else {
        // Tile roof over the front 5.4 m; a lower flat block behind it when the lot runs deeper.
        const double roofD = 5.4;
        e.box(PlacementRole::Body, 0.02, y0 + 0.30, z0, W - 0.02, std::min(D, y0 + roofD), top);
        if (D > y0 + roofD + 0.3) e.box(PlacementRole::Body, 0.02, y0 + roofD, z0, W - 0.02, D, top - 1.0);
        const bool hip = lot.roof == RoofKind::Hip;
        const int k0 = hip ? 3 : 0, k1 = hip ? n - 3 : n;
        for (int k = k0; k < k1; ++k) {
            e.piece("TILE_SLOPE_54_09", k * kLotGrid, y0, top);
            e.piece("TILE_SLOPE_54_09", (k + 1) * kLotGrid, y0 + roofD, top, 180.0);
            e.piece("RIDGE_09", k * kLotGrid, y0, top);
        }
        const char* end = hip ? "HIP_END_54" : "GABLE_END_54";
        e.piece(end, 0.0, y0, top);
        e.piece(end, W, y0 + roofD, top, 180.0);
    }

    // Street clutter on the footway in front (the caller drops it to the ground).
    const std::uint32_t c = lot.clutterSeed;
    if (blankX >= 0.0 && lot.type != LotType::TownHouse && (c & 0x10u)) e.piece("VENDING", blankX - 0.07, -0.95, z0, 0.0, 1.0, 1.0, PlacementRole::Clutter);
    if (c & 0x20u) e.piece("BICYCLE", 0.6 + (W - 1.2) * ((c >> 8) & 0xFFu) / 255.0, -2.2, z0, 90.0 + static_cast<double>((c >> 16) & 15u) - 7.0, 1.0, 1.0, PlacementRole::Clutter);
    if (lot.type == LotType::TownHouse || (c & 0x40u)) e.piece("PLANTERS", W - 1.6, -0.55, z0, 0.0, 1.0, 1.0, PlacementRole::Clutter);
    return e.ok;
}

} // namespace ftcore
