// Enterable rooms (ftcore/Interior.h): the doors table's rooms, and a host box hollowed into a room with a door gap.
// Made-up tables only: the real one is kept outside the repo (opt-in through FT_ROAD_ROOMS). Ids and numbers only.

#include "ftcore/Interior.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string envText(const char* name) {
    std::string value;
#ifdef _MSC_VER
    char* raw = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&raw, &length, name) == 0 && raw != nullptr) value = raw;
    std::free(raw);
#else
    if (const char* raw = std::getenv(name)) value = raw;
#endif
    return value;
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

MassingBox hostBox(double cx, double cy, double sx, double sy, double rot, double z0, double z1) {
    MassingBox b;
    b.id = "HOST";
    b.kind = "slab";
    b.cx = cx;
    b.cy = cy;
    b.sx = sx;
    b.sy = sy;
    b.rot = rot;
    b.z0 = z0;
    b.z1 = z1;
    return b;
}

// A hero corner bay as HeroCorner.h lays it: bay 3 (x 908-916, y 530.95-539.05), the ground 4.18, sunk 0.3, 6.6 m
// tall over the ground; the lattice front's entrance middle 4.125 m south of the bay's north edge; the sill 5 cm up.
InteriorSpec heroBay(RoomKind kind) {
    InteriorSpec s;
    s.host = hostBox(912.0, 535.0, 8.0, 8.1, 0.0, 3.88, 10.78);
    s.host.id = "HC-BAY-3";
    const double y = 539.05 - 4.125;
    s.doorOut = {908.0 - kInteriorDoorOut, y, 4.18};
    s.doorIn = {908.0 + kInteriorDoorIn, y, 4.18};
    s.floorZ = 4.23;
    s.kind = kind;
    return s;
}

// Plan point from the room's frame: u along the front, w into the room, both from the room's middle.
void roomPoint(const InteriorPlan& p, double u, double w, double& x, double& y) {
    x = p.room.x + p.ux * u + p.wx * w;
    y = p.room.y + p.uy * u + p.wy * w;
}

void roomLocal(const InteriorPlan& p, double x, double y, double& u, double& w) {
    u = (x - p.room.x) * p.ux + (y - p.room.y) * p.uy;
    w = (x - p.room.x) * p.wx + (y - p.room.y) * p.wy;
}

bool heldBy(const InteriorPlan& p, InteriorPart part, double x, double y, double z) {
    for (const InteriorBox& b : p.boxes)
        if (b.part == part && interiorBoxHolds(b.box, x, y, z)) return true;
    return false;
}

bool heldByAny(const InteriorPlan& p, double x, double y, double z) {
    for (const InteriorBox& b : p.boxes)
        if (interiorBoxHolds(b.box, x, y, z)) return true;
    return false;
}

MassingFoot segmentFoot(double x0, double y0, double x1, double y1) {
    MassingFoot f;
    f.x = (x0 + x1) * 0.5;
    f.y = (y0 + y1) * 0.5;
    f.hx = std::hypot(x1 - x0, y1 - y0) * 0.5;
    f.rot = std::atan2(y1 - y0, x1 - x0) * 180.0 / kPi;
    return f;
}

// The checks every good plan passes: the walls close the room but for the gap, the doorway is clear, the ceiling
// height, the slots inside the room and off the walk line and each other, the dwell spots and the light.
void checkPlan(const InteriorSpec& s, const InteriorPlan& p) {
    const double fz = s.floorZ, t = s.wall, tf = s.frontWall > 0.0 ? s.frontWall : s.wall;
    CHECK(p.gapU1 - p.gapU0 == doctest::Approx(s.gapWidth));
    CHECK(p.gapU1 - p.gapU0 - 2.0 * kInteriorAgentRadius >= 2.0 * kInteriorNavCell - 1e-9);
    CHECK(p.ceilingTop - fz >= kInteriorCeilingMin);
    // The ring at the walls' mid-thickness, every 0.1 m, at 1.0 m and at 2.5 m over the floor.
    const double ru = p.room.hx + t * 0.5, rw = p.room.hy + t * 0.5, rf = p.room.hy + tf * 0.5;
    int open = 0, shut = 0;
    for (const double z : {fz + 1.0, fz + s.gapHeight + 0.2}) {
        for (int side = 0; side < 4; ++side) {
            const double len = side < 2 ? ru * 2.0 : rw + rf;
            for (double a = 0.0; a <= len + 1e-9; a += 0.1) {
                double u = 0.0, w = 0.0;
                if (side == 0) u = -ru + a, w = -rf; // the front
                else if (side == 1) u = -ru + a, w = rw;
                else if (side == 2) u = -ru, w = -rf + a;
                else u = ru, w = -rf + a;
                double x = 0.0, y = 0.0;
                roomPoint(p, u, w, x, y);
                const bool inGap = side == 0 && u > p.gapU0 - 1e-9 && u < p.gapU1 + 1e-9 && z < fz + s.gapHeight;
                const bool walled = heldBy(p, InteriorPart::Shell, x, y, z);
                if (inGap) {
                    CHECK_MESSAGE(!walled, "the gap is walled at u " << u << " z " << z);
                    ++open;
                } else {
                    CHECK_MESSAGE(walled, "no wall at side " << side << " u " << u << " w " << w << " z " << z);
                    ++shut;
                }
            }
        }
    }
    CHECK(open > 10);
    CHECK(shut > 100);
    // The doorway, through the wall's thickness, from just over the sill to just under the head: nothing in it.
    for (double u = p.gapU0 + 0.01; u < p.gapU1 - 0.005; u += 0.1)
        for (double w = -p.room.hy - tf; w <= -p.room.hy + 1e-9; w += 0.05)
            for (double z = fz + 0.01; z < fz + s.gapHeight - 0.005; z += 0.25) {
                double x = 0.0, y = 0.0;
                roomPoint(p, u, w, x, y);
                CHECK_MESSAGE(!heldByAny(p, x, y, z), "the doorway is blocked at u " << u << " w " << w << " z " << z);
            }
    // The floor and the sill are walkable under the doorway.
    {
        double x = 0.0, y = 0.0;
        roomPoint(p, (p.gapU0 + p.gapU1) * 0.5, -rf, x, y);
        CHECK(heldBy(p, InteriorPart::Walk, x, y, fz - kInteriorFinish - 0.01));
        roomPoint(p, 0.0, 0.0, x, y);
        CHECK(heldBy(p, InteriorPart::Walk, x, y, fz - kInteriorFinish - 0.01));
    }
    // The slots: inside the room, kept off the walk line and apart.
    const MassingFoot walk[2] = {segmentFoot(p.doorOutside.x, p.doorOutside.y, p.doorInside.x, p.doorInside.y),
                                 segmentFoot(p.doorInside.x, p.doorInside.y, p.room.x, p.room.y)};
    REQUIRE(!p.slots.empty());
    for (std::size_t i = 0; i < p.slots.size(); ++i) {
        const InteriorSlot& sl = p.slots[i];
        CAPTURE(interiorSlotText(sl.kind));
        const double c = std::cos(sl.foot.rot * kPi / 180.0), sn = std::sin(sl.foot.rot * kPi / 180.0);
        for (const double su : {-1.0, 1.0})
            for (const double sw : {-1.0, 1.0}) {
                const double x = sl.foot.x + su * sl.foot.hx * c - sw * sl.foot.hy * sn;
                const double y = sl.foot.y + su * sl.foot.hx * sn + sw * sl.foot.hy * c;
                double u = 0.0, w = 0.0;
                roomLocal(p, x, y, u, w);
                CHECK(std::fabs(u) <= p.room.hx + 1e-9);
                CHECK(std::fabs(w) <= p.room.hy + 1e-9);
            }
        CHECK(!massingFeetOverlap(sl.foot, walk[0], kInteriorWalkClear));
        CHECK(!massingFeetOverlap(sl.foot, walk[1], kInteriorWalkClear));
        CHECK(sl.z1 > sl.z0);
        CHECK(sl.z1 <= p.ceilingTop + 1e-9);
        for (std::size_t j = i + 1; j < p.slots.size(); ++j) CHECK(!massingFeetOverlap(sl.foot, p.slots[j].foot, 0.0));
    }
    CHECK(p.dwell.size() >= 2);
    CHECK(p.dwell.size() <= 4);
    for (const MassingVec& d : p.dwell) {
        double u = 0.0, w = 0.0;
        roomLocal(p, d.x, d.y, u, w);
        CHECK(std::fabs(u) < p.room.hx - 0.3);
        CHECK(std::fabs(w) < p.room.hy - 0.3);
        MassingFoot pf;
        pf.x = d.x;
        pf.y = d.y;
        for (const InteriorSlot& sl : p.slots) CHECK(!massingFeetOverlap(pf, sl.foot, 0.3));
    }
    double lu = 0.0, lw = 0.0;
    roomLocal(p, p.light.x, p.light.y, lu, lw);
    CHECK(std::fabs(lu) < p.room.hx);
    CHECK(std::fabs(lw) < p.room.hy);
    CHECK(p.light.z < p.ceilingTop);
    CHECK(p.light.z > fz + s.gapHeight);
}

} // namespace

TEST_CASE("interior: the gap floor is the capsule and a coarse navmesh cell each side") {
    CHECK(kInteriorGapMin == doctest::Approx(1.44));
    CHECK(kInteriorGapMin < 1.6); // the doors table's width
}

TEST_CASE("interiorRoomPlan: a hero bay as each kind of room") {
    for (const RoomKind kind : {RoomKind::Shop, RoomKind::Eatery, RoomKind::Lobby, RoomKind::Flat}) {
        const std::string kindName = roomKindText(kind);
        CAPTURE(kindName);
        const InteriorSpec s = heroBay(kind);
        InteriorPlan p;
        std::string err;
        REQUIRE_MESSAGE(interiorRoomPlan(s, p, &err), err);
        checkPlan(s, p);
        CHECK(p.dropped == 0);
        // The front faces the lane (west): u runs north to south or south to north, w east.
        CHECK(p.wx == doctest::Approx(1.0));
        CHECK(std::fabs(p.wy) < 1e-9);
        // The room fills the bay less its walls: 7.7 m along the front, 7.6 m deep, 3.0 m clear.
        CHECK(p.room.hx * 2.0 == doctest::Approx(7.7));
        CHECK(p.room.hy * 2.0 == doctest::Approx(7.6));
        CHECK(p.ceilingTop - s.floorZ == doctest::Approx(3.0));
        // The crowd points are the door row's.
        CHECK(p.doorOutside.x == doctest::Approx(s.doorOut.x));
        CHECK(p.doorOutside.y == doctest::Approx(s.doorOut.y));
        CHECK(p.doorInside.x == doctest::Approx(s.doorIn.x));
        CHECK(p.doorInside.y == doctest::Approx(s.doorIn.y));
        CHECK(p.doorInside.z == doctest::Approx(s.floorZ));
        for (const InteriorBox& b : p.boxes) CHECK(b.box.id.rfind("HC-BAY-3-IN-", 0) == 0);
    }
}

TEST_CASE("interiorRoomPlan: what each kind is dressed with") {
    auto count = [](const InteriorPlan& p, InteriorSlotKind k) {
        int n = 0;
        for (const InteriorSlot& s : p.slots) n += s.kind == k;
        return n;
    };
    InteriorPlan shop, eatery, lobby;
    REQUIRE(interiorRoomPlan(heroBay(RoomKind::Shop), shop));
    REQUIRE(interiorRoomPlan(heroBay(RoomKind::Eatery), eatery));
    REQUIRE(interiorRoomPlan(heroBay(RoomKind::Lobby), lobby));
    CHECK(count(shop, InteriorSlotKind::Shelf) == 3);
    CHECK(count(shop, InteriorSlotKind::Till) == 1);
    CHECK(count(shop, InteriorSlotKind::Display) == 1);
    CHECK(count(eatery, InteriorSlotKind::Counter) == 1);
    CHECK(count(eatery, InteriorSlotKind::Pass) == 1);
    CHECK(count(eatery, InteriorSlotKind::Stool) >= 6);
    CHECK(count(eatery, InteriorSlotKind::Table) == 2);
    CHECK(count(lobby, InteriorSlotKind::PostBoxes) == 1);
    CHECK(count(lobby, InteriorSlotKind::Stair) == 1);
    CHECK(count(lobby, InteriorSlotKind::InnerDoor) == 1);
    // The stair runs floor to ceiling; the counter's stools face it.
    for (const InteriorSlot& s : lobby.slots)
        if (s.kind == InteriorSlotKind::Stair) CHECK(s.z1 == doctest::Approx(lobby.ceilingTop - kInteriorFinish));
}

TEST_CASE("interiorRoomPlan: an off-centre door on every face of a square and a turned host") {
    for (const double rot : {0.0, 90.0, 33.0, -127.5}) {
        const MassingBox host = hostBox(100.0, 200.0, 10.0, 7.0, rot, 2.0, 9.0);
        const double r = rot * kPi / 180.0;
        const double ax = std::cos(r), ay = std::sin(r);
        const double normals[4][3] = {{ax, ay, 5.0}, {-ax, -ay, 5.0}, {-ay, ax, 3.5}, {ay, -ax, 3.5}}; // nx, ny, half depth
        for (const auto& n : normals) {
            CAPTURE(rot);
            CAPTURE(n[0]);
            CAPTURE(n[1]);
            const double fx = host.cx + n[0] * n[2], fy = host.cy + n[1] * n[2];
            const double ux = -n[1], uy = n[0]; // along the face
            const double off = 1.3;
            InteriorSpec s;
            s.host = host;
            s.doorOut = {fx + n[0] * kInteriorDoorOut + ux * off, fy + n[1] * kInteriorDoorOut + uy * off, 2.0};
            s.doorIn = {fx - n[0] * kInteriorDoorIn + ux * off, fy - n[1] * kInteriorDoorIn + uy * off, 2.0};
            s.floorZ = 2.0;
            s.kind = RoomKind::Shop;
            InteriorPlan p;
            std::string err;
            REQUIRE_MESSAGE(interiorRoomPlan(s, p, &err), err);
            checkPlan(s, p);
            // The gap is where the door is: its middle on the door's line through the face.
            double gx = 0.0, gy = 0.0;
            roomPoint(p, (p.gapU0 + p.gapU1) * 0.5, -p.room.hy - s.wall * 0.5, gx, gy);
            const double mx = fx + ux * off - n[0] * s.wall * 0.5, my = fy + uy * off - n[1] * s.wall * 0.5;
            CHECK(gx == doctest::Approx(mx));
            CHECK(gy == doctest::Approx(my));
            // In the wall 0.3 m past the gap's edge either way: a jamb.
            for (const double edge : {p.gapU0 - 0.3 - kInteriorFinish, p.gapU1 + 0.3 + kInteriorFinish}) {
                double x = 0.0, y = 0.0;
                roomPoint(p, edge, -p.room.hy - s.wall * 0.5, x, y);
                CHECK(heldBy(p, InteriorPart::Shell, x, y, s.floorZ + 1.0));
            }
            CHECK(p.doorInside.x == doctest::Approx(s.doorIn.x));
            CHECK(p.doorInside.y == doctest::Approx(s.doorIn.y));
        }
    }
}

TEST_CASE("interiorRoomPlan: the same input gives the same plan; the seed only breaks a middle door's tie") {
    auto same = [](const InteriorPlan& a, const InteriorPlan& b) {
        if (a.boxes.size() != b.boxes.size() || a.slots.size() != b.slots.size() || a.dwell.size() != b.dwell.size()) return false;
        for (std::size_t i = 0; i < a.boxes.size(); ++i) {
            const MassingBox &x = a.boxes[i].box, &y = b.boxes[i].box;
            if (x.id != y.id || x.cx != y.cx || x.cy != y.cy || x.sx != y.sx || x.sy != y.sy || x.z0 != y.z0 || x.z1 != y.z1) return false;
        }
        for (std::size_t i = 0; i < a.slots.size(); ++i)
            if (a.slots[i].kind != b.slots[i].kind || a.slots[i].foot.x != b.slots[i].foot.x || a.slots[i].foot.y != b.slots[i].foot.y)
                return false;
        return true;
    };
    InteriorSpec s = heroBay(RoomKind::Shop);
    InteriorPlan a, b, c;
    REQUIRE(interiorRoomPlan(s, a));
    REQUIRE(interiorRoomPlan(s, b));
    CHECK(same(a, b));
    s.seed = 1;
    REQUIRE(interiorRoomPlan(s, c));
    CHECK(!same(a, c)); // the hero door is 0.075 m off the middle: a tie, so the seed sides the till
    // A door 1.5 m off the middle: the till goes the far side whatever the seed.
    InteriorSpec o = heroBay(RoomKind::Shop);
    o.doorOut.y -= 1.5;
    o.doorIn.y -= 1.5;
    InteriorPlan d, e;
    REQUIRE(interiorRoomPlan(o, d));
    o.seed = 1;
    REQUIRE(interiorRoomPlan(o, e));
    CHECK(same(d, e));
}

TEST_CASE("interiorRoomPlan: what it refuses") {
    std::string err;
    InteriorPlan p;
    {
        InteriorSpec s = heroBay(RoomKind::Shop);
        s.gapWidth = 1.4;
        CHECK(!interiorRoomPlan(s, p, &err));
        CHECK(err.find("gap") != std::string::npos);
    }
    {
        InteriorSpec s = heroBay(RoomKind::Shop);
        s.doorOut.x = 905.0; // both points out of the front
        s.doorIn.x = 907.0;
        CHECK(!interiorRoomPlan(s, p, &err));
        CHECK(err.find("straddle") != std::string::npos);
    }
    {
        InteriorSpec s = heroBay(RoomKind::Shop);
        s.doorIn.y = s.doorOut.y + 2.0; // 45 degrees across the face
        CHECK(!interiorRoomPlan(s, p, &err));
        CHECK(err.find("30 degrees") != std::string::npos);
    }
    {
        InteriorSpec s = heroBay(RoomKind::Shop);
        s.doorOut.y = s.doorIn.y = 535.0 + 3.3; // the gap's edge 0.15 m off the inner corner
        CHECK(!interiorRoomPlan(s, p, &err));
        CHECK(err.find("corner") != std::string::npos);
    }
    {
        InteriorSpec s = heroBay(RoomKind::Shop);
        s.host.z1 = s.floorZ + 2.6; // 2.45 m clear under the slab
        CHECK(!interiorRoomPlan(s, p, &err));
        CHECK(err.find("clear height") != std::string::npos);
    }
    {
        InteriorSpec s = heroBay(RoomKind::Shop);
        s.floorZ = s.host.z0 - 0.1;
        CHECK(!interiorRoomPlan(s, p, &err));
        CHECK(err.find("foot") != std::string::npos);
    }
    {
        InteriorSpec s = heroBay(RoomKind::Shop);
        s.host.sx = 3.2; // 2.8 m deep inside
        s.host.cx = 909.6;
        CHECK(!interiorRoomPlan(s, p, &err));
        CHECK(err.find("3 m") != std::string::npos);
    }
}

TEST_CASE("heroCornerRooms: the three lit bays, each through its open kit front") {
    const HeroCornerSpec hero;
    HeroRoom rooms[kHeroBays];
    const int n = heroCornerRooms(hero, rooms);
    REQUIRE(n == 3);
    const int bays[3] = {0, 2, 3};
    const RoomKind kinds[3] = {RoomKind::Shop, RoomKind::Eatery, RoomKind::Lobby};
    for (int k = 0; k < n; ++k) {
        const HeroRoom& r = rooms[k];
        CAPTURE(r.bay);
        CHECK(r.bay == bays[k]);
        CHECK(r.spec.kind == kinds[k]);
        REQUIRE(r.doorway != nullptr);
        const KitDoorway& dw = *r.doorway;
        CHECK(std::string(dw.front) == (hero.bayKoshi[r.bay] ? "SHOP_KOSHI_54_OPEN" : "SHOP_GLAZED_81_OPEN"));
        CHECK(r.spec.host.id.rfind("HC-BAY-" + std::to_string(r.bay + 1), 0) == 0);
        CHECK(r.spec.kitFront);
        CHECK(r.spec.frontWall >= dw.deep);
        CHECK(r.spec.floorZ == doctest::Approx(hero.ground + dw.sill));
        InteriorPlan p;
        std::string err;
        REQUIRE_MESSAGE(interiorRoomPlan(r.spec, p, &err), err);
        checkPlan(r.spec, p);
        CHECK(p.dropped == 0);
        // The gap is the open front's clear opening, where the piece puts it: from the bay's north edge, along the lane.
        const double north = hero.bayY0 + hero.bayWidth * (r.bay + 1);
        double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0;
        roomPoint(p, p.gapU0, -p.room.hy, ax, ay);
        roomPoint(p, p.gapU1, -p.room.hy, bx, by);
        CHECK(std::max(ay, by) == doctest::Approx(north - dw.x0));
        CHECK(std::min(ay, by) == doctest::Approx(north - dw.x1));
        CHECK(ax == doctest::Approx(hero.laneX1 + r.spec.frontWall));
        // No reveal lines a kit front's gap; its sill is walked at the floor line, under the front's threshold.
        bool sillAtFloor = false;
        for (const InteriorBox& b : p.boxes) {
            CHECK(b.part != InteriorPart::Reveal);
            if (b.box.id == r.spec.host.id + "-IN-SILL") sillAtFloor = b.box.z1 == doctest::Approx(r.spec.floorZ);
        }
        CHECK(sillAtFloor);
        // The lintel's foot is the front's head: nothing hangs into the clear height.
        for (const InteriorBox& b : p.boxes)
            if (b.box.id == r.spec.host.id + "-IN-LINTEL") CHECK(b.box.z0 == doctest::Approx(r.spec.floorZ + dw.high));
    }
    // The glazed front's opening is lower than the lattice's.
    CHECK(rooms[2].spec.gapHeight == doctest::Approx(2.15));
    CHECK(rooms[0].spec.gapHeight == doctest::Approx(2.25));
}

TEST_CASE("heroCornerDoorRows: the hero doors as table rows, read back") {
    const HeroCornerSpec hero;
    HeroRoom rooms[kHeroBays];
    const int n = heroCornerRooms(hero, rooms);
    const std::string rows = heroCornerDoorRows(hero);
    std::vector<RoadRoom> back;
    std::string err;
    REQUIRE_MESSAGE(importRoadRooms("id,kind,width_m,pts,pavement_m\n" + rows, back, &err), err);
    REQUIRE(static_cast<int>(back.size()) == n);
    for (int k = 0; k < n; ++k) {
        const InteriorSpec& s = rooms[k].spec;
        CHECK(back[k].host == s.host.id);
        CHECK(back[k].kind == s.kind);
        CHECK(back[k].doorWidth == doctest::Approx(s.gapWidth).epsilon(0.005));
        CHECK(back[k].doorOut.x == doctest::Approx(s.doorOut.x).epsilon(1e-4));
        CHECK(back[k].doorOut.y == doctest::Approx(s.doorOut.y).epsilon(1e-4));
        CHECK(back[k].doorIn.x == doctest::Approx(s.doorIn.x).epsilon(1e-4));
        CHECK(back[k].doorIn.z == doctest::Approx(s.floorZ).epsilon(1e-4));
        // The room rectangle lies inside the host box.
        for (const MassingVec& c : back[k].corner) {
            CHECK(std::fabs(c.x - s.host.cx) <= s.host.sx * 0.5);
            CHECK(std::fabs(c.y - s.host.cy) <= s.host.sy * 0.5);
        }
    }
    // A spec with no lit bay has no rows.
    HeroCornerSpec dark;
    for (bool& lit : dark.bayLit) lit = false;
    CHECK(heroCornerDoorRows(dark).empty());
}

TEST_CASE("importRoadRooms: ROOM rows paired with their DOOR rows") {
    const char* csv =
        "# made up\n"
        "id,kind,width_m,pts,pavement_m\n"
        "DOOR-BLD-1,door,1.6,10.00:5.68:4.00;10.00:7.68:4.00,\n"
        "ROOM-BLD-1,interior_shop,0,7:7:4;13:7:4;13:12:4;7:12:4,\n"
        "DOOR-BLD-2,door,1.6,1:1:1;2:2:2,\n"
        "ROOM-BLD-4,interior_lobby,0,1:1:0;2:1:0;2:2:0;1:2:0,\n"
        "DOOR-BLD-4,door,1.8,30:40:2;30:42:2,\n"
        "OTHER,bench,0,1:1:1;2:2:2,\n";
    std::vector<RoadRoom> rooms;
    std::string err;
    REQUIRE_MESSAGE(importRoadRooms(csv, rooms, &err), err);
    REQUIRE(rooms.size() == 2); // BLD-2's door has no room
    CHECK(rooms[0].host == "BLD-1");
    CHECK(rooms[0].kind == RoomKind::Shop);
    CHECK(rooms[0].doorWidth == doctest::Approx(1.6));
    CHECK(rooms[0].doorOut.y == doctest::Approx(5.68));
    CHECK(rooms[0].doorIn.y == doctest::Approx(7.68));
    CHECK(rooms[0].corner[2].x == doctest::Approx(13.0));
    CHECK(rooms[0].corner[2].y == doctest::Approx(12.0));
    CHECK(rooms[1].host == "BLD-4");
    CHECK(rooms[1].kind == RoomKind::Lobby);
    CHECK(rooms[1].doorWidth == doctest::Approx(1.8)); // a DOOR row after its ROOM row still pairs

    // The imported door drives a plan: BLD-1 is an 8 m box whose south face is y 6.
    InteriorSpec s;
    s.host = hostBox(10.0, 10.0, 8.0, 8.0, 0.0, 4.0, 10.0);
    s.doorOut = rooms[0].doorOut;
    s.doorIn = rooms[0].doorIn;
    s.gapWidth = rooms[0].doorWidth;
    s.floorZ = 4.0;
    InteriorPlan p;
    REQUIRE_MESSAGE(interiorRoomPlan(s, p, &err), err);
    checkPlan(s, p);

    rooms.clear();
    CHECK(!importRoadRooms("id,kind,width_m,pts\nROOM-X,interior_shop,0,1:1:1;2:1:1;2:2:1;1:2:1\n", rooms, &err));
    CHECK(err.find("no DOOR") != std::string::npos);
    CHECK(!importRoadRooms("id,kind,width_m,pts\nROOM-X,interior_bank,0,1:1:1;2:1:1;2:2:1;1:2:1\nDOOR-X,door,1.6,1:1:1;2:2:2\n", rooms, &err));
    CHECK(err.find("unknown room kind") != std::string::npos);
    CHECK(!importRoadRooms("id,kind,width_m,pts\nROOM-X,interior_shop,0,1:1:1;2:1:1;2:2:1\nDOOR-X,door,1.6,1:1:1;2:2:2\n", rooms, &err));
    CHECK(!importRoadRooms("id,kind,width_m,pts\nDOOR-X,door,0,1:1:1;2:2:2\n", rooms, &err));
    CHECK(!importRoadRooms("id,kind,width_m,pts\nDOOR-X,door,1.6,1:1;2:2\n", rooms, &err));

    for (const RoomKind k : {RoomKind::Shop, RoomKind::Eatery, RoomKind::Lobby, RoomKind::Flat}) {
        RoomKind back = RoomKind::Flat;
        CHECK(roomKindFromText(roomKindText(k), back));
        CHECK(back == k);
    }
}

TEST_CASE("importRoadRooms: the real doors table (opt-in)") {
    // FT_ROAD_ROOMS: the strip's doors table. Skipped unless set.
    const std::string path = envText("FT_ROAD_ROOMS");
    if (path.empty()) return;
    std::vector<RoadRoom> rooms;
    std::string err;
    REQUIRE_MESSAGE(importRoadRooms(readFile(path), rooms, &err), err);
    REQUIRE(!rooms.empty());
    // A house door (1.0 m) is under the navmesh floor: the build widens its gap to kInteriorGapMin. Counted, not failed.
    int narrow = 0;
    for (const RoadRoom& r : rooms) {
        CAPTURE(r.host);
        CHECK(r.doorWidth > 0.0);
        narrow += r.doorWidth < kInteriorGapMin;
        CHECK(std::hypot(r.doorIn.x - r.doorOut.x, r.doorIn.y - r.doorOut.y) == doctest::Approx(kInteriorDoorOut + kInteriorDoorIn).epsilon(0.01));
    }
    MESSAGE("rooms: " << rooms.size() << ", doors under the gap floor: " << narrow);
}
