// Enterable rooms (ftcore/Interior.h).

#include "ftcore/Interior.h"
#include "ftcore/Roads.h"

#include "CsvRead.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ftcore {

namespace {

constexpr double kPi = 3.14159265358979323846;

double degOf(double r) { return r * 180.0 / kPi; }

// "x:y:z;x:y:z;...": exactly `want` points.
bool parsePoints(std::string_view pts, MassingVec* out, int want) {
    int n = 0;
    std::size_t p = 0;
    for (;;) {
        const std::size_t q = pts.find(';', p);
        const std::string_view one = pts.substr(p, q == std::string_view::npos ? std::string_view::npos : q - p);
        if (n >= want) return false;
        const std::size_t a = one.find(':');
        const std::size_t b = a == std::string_view::npos ? std::string_view::npos : one.find(':', a + 1);
        if (b == std::string_view::npos) return false;
        if (!detail::parseNumber(one.substr(0, a), out[n].x) || !detail::parseNumber(one.substr(a + 1, b - a - 1), out[n].y) ||
            !detail::parseNumber(one.substr(b + 1), out[n].z))
            return false;
        ++n;
        if (q == std::string_view::npos) break;
        p = q + 1;
    }
    return n == want;
}

// The room's frame: local (u, w) from the front face's middle, u along the front, w into the room.
struct Frame {
    double fx = 0.0, fy = 0.0; // the front face's middle
    double ux = 1.0, uy = 0.0, wx = 0.0, wy = 1.0;
    double rot = 0.0;          // degrees, the u axis
    double px(double u, double w) const { return fx + ux * u + wx * w; }
    double py(double u, double w) const { return fy + uy * u + wy * w; }
};

// A local slot before it goes to the plan frame.
struct LocalSlot {
    InteriorSlotKind kind;
    double u0, u1, w0, w1, height;
    double faceU, faceW; // the way its front looks, local
};

} // namespace

bool roomKindFromText(std::string_view text, RoomKind& out) {
    text = detail::trim(text);
    if (text == "interior_shop") out = RoomKind::Shop;
    else if (text == "interior_eatery") out = RoomKind::Eatery;
    else if (text == "interior_lobby") out = RoomKind::Lobby;
    else if (text == "interior_flat") out = RoomKind::Flat;
    else return false;
    return true;
}

const char* roomKindText(RoomKind kind) {
    switch (kind) {
    case RoomKind::Shop: return "interior_shop";
    case RoomKind::Eatery: return "interior_eatery";
    case RoomKind::Lobby: return "interior_lobby";
    case RoomKind::Flat: return "interior_flat";
    }
    return "interior_shop";
}

const char* interiorSlotText(InteriorSlotKind kind) {
    switch (kind) {
    case InteriorSlotKind::Shelf: return "shelf";
    case InteriorSlotKind::Till: return "till";
    case InteriorSlotKind::Display: return "display";
    case InteriorSlotKind::Counter: return "counter";
    case InteriorSlotKind::Stool: return "stool";
    case InteriorSlotKind::Table: return "table";
    case InteriorSlotKind::Pass: return "pass";
    case InteriorSlotKind::PostBoxes: return "postboxes";
    case InteriorSlotKind::Stair: return "stair";
    case InteriorSlotKind::InnerDoor: return "innerdoor";
    }
    return "shelf";
}

bool importRoadRooms(std::string_view csv, std::vector<RoadRoom>& out, std::string* error) {
    using namespace detail;
    std::vector<Record> recs;
    if (!splitCsv(csv, recs, error)) return false;
    struct Door {
        std::string host;
        double width;
        MassingVec o, i;
    };
    struct Room {
        RoadRoom room;
        std::size_t line;
    };
    std::vector<Door> doors;
    std::vector<Room> rooms;
    std::vector<std::string> head;
    for (const Record& rec : recs) {
        if (isComment(rec)) continue;
        if (head.empty()) {
            for (const std::string& h : rec.fields) head.emplace_back(trim(h));
            continue;
        }
        auto col = [&](const char* name) -> std::string_view {
            for (std::size_t c = 0; c < head.size() && c < rec.fields.size(); ++c)
                if (head[c] == name) return trim(rec.fields[c]);
            return {};
        };
        const std::string_view id = col("id"), kind = col("kind");
        if (id.substr(0, 5) == "DOOR-" && kind == "door") {
            Door d;
            d.host = std::string(id.substr(5));
            if (!parseNumber(col("width_m"), d.width) || d.width <= 0.0) return fail(error, rec.line, "a door needs a width");
            MassingVec p[2];
            if (!parsePoints(col("pts"), p, 2)) return fail(error, rec.line, "a door's pts are \"x:y:z;x:y:z\"");
            d.o = p[0];
            d.i = p[1];
            doors.push_back(d);
        } else if (id.substr(0, 5) == "ROOM-") {
            Room r;
            r.line = rec.line;
            r.room.host = std::string(id.substr(5));
            if (!roomKindFromText(kind, r.room.kind)) return fail(error, rec.line, "unknown room kind \"" + std::string(kind) + "\"");
            if (!parsePoints(col("pts"), r.room.corner, 4)) return fail(error, rec.line, "a room's pts are four \"x:y:z\" corners");
            rooms.push_back(r);
        }
    }
    for (Room& r : rooms) {
        const auto it = std::find_if(doors.begin(), doors.end(), [&](const Door& d) { return d.host == r.room.host; });
        if (it == doors.end()) return fail(error, r.line, "ROOM-" + r.room.host + " has no DOOR row");
        r.room.doorWidth = it->width;
        r.room.doorOut = it->o;
        r.room.doorIn = it->i;
        out.push_back(r.room);
    }
    return true;
}

bool interiorBoxHolds(const MassingBox& box, double x, double y, double z) {
    constexpr double e = 1e-9;
    if (z < box.z0 - e || z > box.z1 + e) return false;
    const double r = box.rot * kPi / 180.0, c = std::cos(r), s = std::sin(r);
    const double dx = x - box.cx, dy = y - box.cy;
    const double u = dx * c + dy * s, w = -dx * s + dy * c;
    return std::fabs(u) <= box.sx * 0.5 + e && std::fabs(w) <= box.sy * 0.5 + e;
}

bool interiorRoomPlan(const InteriorSpec& spec, InteriorPlan& out, std::string* error) {
    out = InteriorPlan{};
    auto bad = [&](const char* what) {
        if (error) *error = what;
        return false;
    };
    const MassingBox& h = spec.host;
    const double hr = h.rot * kPi / 180.0;
    const double ax = std::cos(hr), ay = std::sin(hr), bx = -ay, by = ax;
    double dx = spec.doorIn.x - spec.doorOut.x, dy = spec.doorIn.y - spec.doorOut.y;
    const double dl = std::hypot(dx, dy);
    if (dl < 1e-6) return bad("the door's two points are the same");
    dx /= dl;
    dy /= dl;
    struct Face {
        double nx, ny, half, along;
    };
    const Face faces[4] = {{ax, ay, h.sx * 0.5, h.sy}, {-ax, -ay, h.sx * 0.5, h.sy}, {bx, by, h.sy * 0.5, h.sx}, {-bx, -by, h.sy * 0.5, h.sx}};
    int best = -1;
    double bestDot = std::cos(30.0 * kPi / 180.0);
    for (int k = 0; k < 4; ++k) {
        const double d = -(faces[k].nx * dx + faces[k].ny * dy); // the outward normal against the way in
        if (d > bestDot) best = k, bestDot = d;
    }
    if (best < 0) return bad("the door does not cross a host face within 30 degrees of square");
    const Face& f = faces[best];
    const double po = (spec.doorOut.x - h.cx) * f.nx + (spec.doorOut.y - h.cy) * f.ny - f.half;
    const double pi = (spec.doorIn.x - h.cx) * f.nx + (spec.doorIn.y - h.cy) * f.ny - f.half;
    if (po < -0.1 || pi > 0.1 || po > 1.5 || pi < -3.0) return bad("the door's two points do not straddle the host's front face");

    Frame fr;
    fr.fx = h.cx + f.nx * f.half;
    fr.fy = h.cy + f.ny * f.half;
    fr.wx = -f.nx;
    fr.wy = -f.ny;
    fr.ux = -f.ny; // w is u turned a quarter counter-clockwise: (u, w) right-handed
    fr.uy = f.nx;
    fr.rot = degOf(std::atan2(fr.uy, fr.ux));
    const double mx = (spec.doorOut.x + spec.doorIn.x) * 0.5, my = (spec.doorOut.y + spec.doorIn.y) * 0.5;
    const double gc = (mx - fr.fx) * fr.ux + (my - fr.fy) * fr.uy;

    const double L = f.along, D = 2.0 * f.half, t = spec.wall, tf = spec.frontWall > 0.0 ? spec.frontWall : spec.wall;
    const double iu0 = -L * 0.5 + t, iu1 = L * 0.5 - t, iw0 = tf, iw1 = D - t;
    if (iu1 - iu0 < 3.0 || iw1 - iw0 < 3.0) return bad("the room would be under 3 m across");
    const double gw = spec.gapWidth, gh = spec.gapHeight;
    if (gw < kInteriorGapMin - 1e-9) return bad("the door gap is under kInteriorGapMin");
    const double g0 = gc - gw * 0.5, g1 = gc + gw * 0.5;
    if (g0 < iu0 + 0.1 || g1 > iu1 - 0.1) return bad("the door gap reaches within wall + 0.1 m of a corner");
    if (spec.floorZ < h.z0 - 1e-9) return bad("the floor is under the host's foot");
    const double clear = std::min(spec.ceiling, h.z1 - kInteriorSlabMin - spec.floorZ);
    if (clear < kInteriorCeilingMin - 1e-9) return bad("the room's clear height is under kInteriorCeilingMin");
    if (clear < gh + 0.1 - 1e-9) return bad("the room's clear height leaves no lintel over the door");
    const double fz = spec.floorZ, top = fz + clear, fin = kInteriorFinish;
    out.ceilingTop = top;
    out.ux = fr.ux;
    out.uy = fr.uy;
    out.wx = fr.wx;
    out.wy = fr.wy;
    out.gapU0 = g0;
    out.gapU1 = g1;
    out.room.x = fr.px(0.0, (iw0 + iw1) * 0.5);
    out.room.y = fr.py(0.0, (iw0 + iw1) * 0.5);
    out.room.hx = (iu1 - iu0) * 0.5;
    out.room.hy = (iw1 - iw0) * 0.5;
    out.room.rot = fr.rot;

    auto add = [&](InteriorPart part, InteriorFace face, double u0, double u1, double w0, double w1, double z0, double z1,
                   const char* name) {
        if (u1 - u0 <= 1e-6 || w1 - w0 <= 1e-6 || z1 - z0 <= 1e-6) return;
        InteriorBox b;
        b.part = part;
        b.face = face;
        b.box.id = h.id + "-IN-" + name;
        b.box.kind = "interior";
        b.box.cx = fr.px((u0 + u1) * 0.5, (w0 + w1) * 0.5);
        b.box.cy = fr.py((u0 + u1) * 0.5, (w0 + w1) * 0.5);
        b.box.sx = u1 - u0;
        b.box.sy = w1 - w0;
        b.box.rot = fr.rot;
        b.box.z0 = z0;
        b.box.z1 = z1;
        out.boxes.push_back(b);
    };
    const double hu = L * 0.5, base = std::min(h.z0, fz - 0.1), walkTop = fz - fin;
    // A kit front fills the gap itself: no reveal linings, so the shell stands at the clear opening's edges.
    const double rv = spec.kitFront ? 0.0 : fin;
    const double sg0 = g0 - rv, sg1 = g1 + rv, lintel0 = fz + gh + rv;
    // Shell: the front wall round the gap, the back and sides, the mass over the ceiling.
    add(InteriorPart::Shell, InteriorFace::None, -hu, sg0, 0.0, tf, h.z0, top, "JAMB-L");
    add(InteriorPart::Shell, InteriorFace::None, sg1, hu, 0.0, tf, h.z0, top, "JAMB-R");
    add(InteriorPart::Shell, InteriorFace::None, sg0, sg1, 0.0, tf, lintel0, top, "LINTEL");
    add(InteriorPart::Shell, InteriorFace::None, -hu, hu, D - t, D, h.z0, top, "BACK");
    add(InteriorPart::Shell, InteriorFace::None, -hu, iu0, tf, D - t, h.z0, top, "SIDE-L");
    add(InteriorPart::Shell, InteriorFace::None, iu1, hu, tf, D - t, h.z0, top, "SIDE-R");
    add(InteriorPart::Shell, InteriorFace::None, -hu, hu, 0.0, D, top, h.z1, "TOP");
    // Walk: the floor and the sill.
    add(InteriorPart::Walk, InteriorFace::None, iu0, iu1, iw0, iw1, base, walkTop, "FLOOR");
    // Under a kit front the sill is walked at the floor line (the front's threshold shows it); else a lining tops it.
    add(InteriorPart::Walk, InteriorFace::None, sg0, sg1, 0.0, tf, base, spec.kitFront ? fz : walkTop, "SILL");
    // Linings.
    add(InteriorPart::Finish, InteriorFace::Floor, iu0, iu1, iw0, iw1, walkTop, fz, "FIN-FLOOR");
    add(InteriorPart::Finish, InteriorFace::Ceiling, iu0, iu1, iw0, iw1, top - fin, top, "FIN-CEIL");
    add(InteriorPart::Finish, InteriorFace::Wall, iu0, iu1, iw1 - fin, iw1, fz, top - fin, "FIN-BACK");
    add(InteriorPart::Finish, InteriorFace::Wall, iu0, iu0 + fin, iw0 + fin, iw1 - fin, fz, top - fin, "FIN-SIDE-L");
    add(InteriorPart::Finish, InteriorFace::Wall, iu1 - fin, iu1, iw0 + fin, iw1 - fin, fz, top - fin, "FIN-SIDE-R");
    add(InteriorPart::Finish, InteriorFace::Wall, iu0, sg0, iw0, iw0 + fin, fz, top - fin, "FIN-FRONT-L");
    add(InteriorPart::Finish, InteriorFace::Wall, sg1, iu1, iw0, iw0 + fin, fz, top - fin, "FIN-FRONT-R");
    add(InteriorPart::Finish, InteriorFace::Wall, sg0, sg1, iw0, iw0 + fin, lintel0, top - fin, "FIN-FRONT-HEAD");
    const double sk = kInteriorSkirtingHigh, sp = kInteriorSkirtingProud, skz = fz + sk;
    add(InteriorPart::Skirting, InteriorFace::Skirting, iu0 + fin, iu1 - fin, iw1 - fin - sp, iw1 - fin, fz, skz, "SKIRT-BACK");
    add(InteriorPart::Skirting, InteriorFace::Skirting, iu0 + fin, iu0 + fin + sp, iw0 + fin, iw1 - fin - sp, fz, skz, "SKIRT-SIDE-L");
    add(InteriorPart::Skirting, InteriorFace::Skirting, iu1 - fin - sp, iu1 - fin, iw0 + fin, iw1 - fin - sp, fz, skz, "SKIRT-SIDE-R");
    add(InteriorPart::Skirting, InteriorFace::Skirting, iu0 + fin + sp, sg0, iw0 + fin, iw0 + fin + sp, fz, skz, "SKIRT-FRONT-L");
    add(InteriorPart::Skirting, InteriorFace::Skirting, sg1, iu1 - fin - sp, iw0 + fin, iw0 + fin + sp, fz, skz, "SKIRT-FRONT-R");
    if (!spec.kitFront) {
        add(InteriorPart::Reveal, InteriorFace::Reveal, sg0, g0, 0.0, iw0 + fin, fz, lintel0, "REVEAL-L");
        add(InteriorPart::Reveal, InteriorFace::Reveal, g1, sg1, 0.0, iw0 + fin, fz, lintel0, "REVEAL-R");
        add(InteriorPart::Reveal, InteriorFace::Reveal, g0, g1, 0.0, iw0 + fin, fz + gh, lintel0, "REVEAL-HEAD");
        add(InteriorPart::Reveal, InteriorFace::Floor, g0, g1, 0.0, tf, walkTop, fz, "REVEAL-SILL");
    }

    // Crowd points and the light.
    const double wm = (iw0 + iw1) * 0.5;
    out.doorOutside = {fr.px(gc, -kInteriorDoorOut), fr.py(gc, -kInteriorDoorOut), fz};
    out.doorInside = {fr.px(gc, kInteriorDoorIn), fr.py(gc, kInteriorDoorIn), fz};
    out.light = {fr.px(0.0, wm), fr.py(0.0, wm), top - fin - 0.1};

    // Dressing: local slots by kind, the counter-side things on the side away from the door.
    const double away = gc > 0.3 ? -1.0 : gc < -0.3 ? 1.0 : ((spec.seed & 1u) ? 1.0 : -1.0);
    const double ua0 = iu0 + 0.05, ua1 = iu1 - 0.05, wb0 = iw0 + 0.05, wb1 = iw1 - 0.05;
    const double lu = ua1 - ua0;
    auto sideSpan = [&](double sgn, double depth, double& a, double& b) {
        if (sgn > 0) a = ua1 - depth, b = ua1;
        else a = ua0, b = ua0 + depth;
    };
    std::vector<LocalSlot> ls;
    switch (spec.kind) {
    case RoomKind::Shop: {
        ls.push_back({InteriorSlotKind::Shelf, ua0, ua1, wb1 - 0.45, wb1, 1.8, 0.0, -1.0});
        for (const double sgn : {away, -away}) {
            double a = 0.0, b = 0.0;
            sideSpan(sgn, 0.45, a, b);
            ls.push_back({InteriorSlotKind::Shelf, a, b, wb0 + 1.0, wb1 - 0.5, 1.8, -sgn, 0.0});
        }
        double a = 0.0, b = 0.0;
        sideSpan(away, 1.3, a, b);
        if (away > 0) b -= 0.1;
        else a += 0.1;
        ls.push_back({InteriorSlotKind::Till, a, b, wb0 + 0.25, wb0 + 0.85, 1.0, 0.0, 1.0});
        const double du = away * lu * 0.25;
        ls.push_back({InteriorSlotKind::Display, du - 0.6, du + 0.6, wm + 0.0, wm + 0.8, 0.8, 0.0, -1.0});
        break;
    }
    case RoomKind::Eatery: {
        ls.push_back({InteriorSlotKind::Pass, -1.0, 1.0, wb1 - 0.3, wb1, 2.0, 0.0, -1.0});
        const double c0 = ua0 + 0.8, c1 = ua1 - 0.8;
        ls.push_back({InteriorSlotKind::Counter, c0, c1, wb1 - 1.6, wb1 - 1.0, 1.0, 0.0, -1.0});
        const int n = static_cast<int>(std::floor((c1 - c0) / 0.7));
        const double s0 = (c0 + c1) * 0.5 - (n - 1) * 0.35;
        for (int k = 0; k < n; ++k) {
            const double su = s0 + k * 0.7;
            ls.push_back({InteriorSlotKind::Stool, su - 0.175, su + 0.175, wb1 - 2.125, wb1 - 1.775, 0.75, 0.0, 1.0});
        }
        for (const double sgn : {away, -away}) {
            const double tu = sgn * (lu * 0.5 - 1.1);
            ls.push_back({InteriorSlotKind::Table, tu - 0.4, tu + 0.4, wb0 + 2.4, wb0 + 3.2, 0.75, -sgn, 0.0});
        }
        break;
    }
    case RoomKind::Lobby: {
        double a = 0.0, b = 0.0;
        sideSpan(away, 0.35, a, b);
        ls.push_back({InteriorSlotKind::PostBoxes, a, b, wb0 + 1.2, wb0 + 2.7, 1.5, -away, 0.0});
        sideSpan(away, 1.2, a, b);
        ls.push_back({InteriorSlotKind::Stair, a, b, wb1 - 2.8, wb1, clear - fin, 0.0, -1.0});
        const double inner = away > 0 ? a : b; // the stair's edge toward the room's middle
        const double d0 = inner - away * 0.4 - (away > 0 ? 1.0 : 0.0), d1 = d0 + 1.0;
        ls.push_back({InteriorSlotKind::InnerDoor, d0, d1, wb1 - 0.1, wb1, 2.1, 0.0, -1.0});
        break;
    }
    case RoomKind::Flat: {
        const double tu = away * 1.8; // its near edge 1.2 m off the walk line's end (the clearance test is square-cornered)
        ls.push_back({InteriorSlotKind::Table, tu - 0.6, tu + 0.6, wm + 0.5, wm + 1.3, 0.75, 0.0, -1.0});
        if (away > 0) ls.push_back({InteriorSlotKind::Shelf, 0.0, ua1, wb1 - 0.4, wb1, 1.8, 0.0, -1.0});
        else ls.push_back({InteriorSlotKind::Shelf, ua0, 0.0, wb1 - 0.4, wb1, 1.8, 0.0, -1.0});
        double a = 0.0, b = 0.0;
        sideSpan(away, 0.4, a, b);
        ls.push_back({InteriorSlotKind::Shelf, a, b, wb0 + 1.2, wb1 - 1.0, 1.8, -away, 0.0});
        break;
    }
    }

    // The walk line: the door's outside point, its inside point, the room's middle.
    auto lineFoot = [&](double u0, double w0, double u1, double w1) {
        MassingFoot lf;
        lf.x = fr.px((u0 + u1) * 0.5, (w0 + w1) * 0.5);
        lf.y = fr.py((u0 + u1) * 0.5, (w0 + w1) * 0.5);
        const double ex = fr.px(u1, w1) - fr.px(u0, w0), ey = fr.py(u1, w1) - fr.py(u0, w0);
        lf.hx = std::hypot(ex, ey) * 0.5;
        lf.hy = 0.0;
        lf.rot = degOf(std::atan2(ey, ex));
        return lf;
    };
    const MassingFoot walk[2] = {lineFoot(gc, -kInteriorDoorOut, gc, kInteriorDoorIn), lineFoot(gc, kInteriorDoorIn, 0.0, wm)};
    for (const LocalSlot& l : ls) {
        InteriorSlot s;
        s.kind = l.kind;
        s.foot.x = fr.px((l.u0 + l.u1) * 0.5, (l.w0 + l.w1) * 0.5);
        s.foot.y = fr.py((l.u0 + l.u1) * 0.5, (l.w0 + l.w1) * 0.5);
        s.foot.hx = (l.u1 - l.u0) * 0.5;
        s.foot.hy = (l.w1 - l.w0) * 0.5;
        s.foot.rot = fr.rot;
        s.z0 = fz;
        s.z1 = fz + l.height;
        s.face = degOf(std::atan2(fr.uy * l.faceU + fr.wy * l.faceW, fr.ux * l.faceU + fr.wx * l.faceW));
        if (massingFeetOverlap(s.foot, walk[0], kInteriorWalkClear) || massingFeetOverlap(s.foot, walk[1], kInteriorWalkClear)) {
            ++out.dropped;
            continue;
        }
        // A dwell spot half a metre in front of it, where a walker stands to use it.
        const double half = std::fabs(l.faceU) * (l.u1 - l.u0) * 0.5 + std::fabs(l.faceW) * (l.w1 - l.w0) * 0.5;
        const double su = (l.u0 + l.u1) * 0.5 + l.faceU * (half + 0.5), sw = (l.w0 + l.w1) * 0.5 + l.faceW * (half + 0.5);
        out.slots.push_back(s);
        if (l.kind == InteriorSlotKind::Stool || l.kind == InteriorSlotKind::Pass) continue;
        if (su < ua0 + 0.35 || su > ua1 - 0.35 || sw < wb0 + 0.35 || sw > wb1 - 0.35) continue;
        out.dwell.push_back({fr.px(su, sw), fr.py(su, sw), fz});
    }
    // Every dwell spot keeps 0.3 m off every slot.
    out.dwell.erase(std::remove_if(out.dwell.begin(), out.dwell.end(),
                                   [&](const MassingVec& p) {
                                       MassingFoot pf;
                                       pf.x = p.x;
                                       pf.y = p.y;
                                       for (const InteriorSlot& s : out.slots)
                                           if (massingFeetOverlap(pf, s.foot, 0.3)) return true;
                                       return false;
                                   }),
                    out.dwell.end());
    if (out.dwell.size() > 4) out.dwell.resize(4);
    if (out.dwell.size() < 2) return bad("the room has fewer than two dwell spots");
    return true;
}

int heroCornerRooms(const HeroCornerSpec& hero, HeroRoom out[kHeroBays]) {
    HeroCornerLayout layout;
    heroCornerLayout(hero, layout);
    int n = 0;
    for (int i = 0; i < kHeroBays; ++i) {
        if (!hero.bayLit[i] || !hero.bayRoom[i]) continue;
        const MassingBox& box = layout.bays[i];
        const KitDoorway& dw = hero.bayKoshi[i] ? kKoshiDoorway : kGlazedDoorway;
        // The front faces the lane (west); its piece runs from the bay's north edge (its left end seen from the lane).
        const double face = box.cx - box.sx * 0.5, north = box.cy + box.sy * 0.5;
        const double y = north - (dw.x0 + dw.x1) * 0.5, z = hero.ground + dw.sill;
        HeroRoom& r = out[n++];
        r = HeroRoom{};
        r.bay = i;
        r.doorway = &dw;
        InteriorSpec& s = r.spec;
        s.host = box;
        s.doorOut = {face - kInteriorDoorOut, y, z};
        s.doorIn = {face + kInteriorDoorIn, y, z};
        s.gapWidth = dw.x1 - dw.x0;
        s.gapHeight = dw.high;
        s.floorZ = z;
        s.frontWall = std::max(s.wall, dw.deep);
        s.kitFront = true;
        s.kind = i == layout.eateryBay ? RoomKind::Eatery : hero.bayKoshi[i] ? RoomKind::Shop : RoomKind::Lobby;
        s.seed = static_cast<std::uint32_t>(i);
    }
    return n;
}

int heroCornerCrowd(const HeroCornerSpec& hero, double side, std::vector<crowdv2::CrowdStreet>& streets,
                    std::vector<crowdv2::CrowdDoor>& doors) {
    auto eng = [side](double x, double y) {
        const MassingVec e = massingEnginePoint(side, x, y, 0.0);
        return Vec2{static_cast<float>(e.x), static_cast<float>(e.y)};
    };
    crowdv2::CrowdStreet lane;
    lane.kind = "lane";
    lane.width = static_cast<float>(hero.laneX1 - hero.laneX0);
    const double mid = 0.5 * (hero.laneX0 + hero.laneX1);
    // A point about every 3 m: a lane standing on its own is still a component big enough to spawn on (minComponent).
    // Both start on the coast road's closed south end, so they join it (and the alley its north end), each in one leg
    // over the apron: a loose end joins the nearest street within its half width and a little, its own street's next
    // segment too, so the leg is kept longer than that.
    const double roadS = std::min(hero.padY0, hero.laneY0) - kRoadEndClose, roadN = std::max(hero.padY1, hero.laneY1) + kRoadEndClose;
    const double bayY1 = hero.bayY0 + kHeroBays * hero.bayWidth, bayX1 = hero.laneX1 + hero.bayDepth;
    auto run = [&](crowdv2::CrowdStreet& st, double x, double y0, double y1) {
        const int steps = std::max(1, static_cast<int>(std::ceil((y1 - y0) / 3.0)));
        for (int k = 0; k <= steps; ++k) st.points.push_back(eng(x, y0 + (y1 - y0) * k / steps));
    };
    lane.points.push_back(eng(mid, roadS));
    run(lane, mid, hero.laneY0, hero.laneY1);
    streets.push_back(std::move(lane));
    crowdv2::CrowdStreet alley;
    alley.kind = "alley";
    alley.width = static_cast<float>(hero.padX1 - bayX1);
    if (alley.width > 1.0f) {
        const double ax = 0.5 * (bayX1 + hero.padX1);
        alley.points.push_back(eng(ax, roadS));
        run(alley, ax, hero.bayY0, bayY1);
        alley.points.push_back(eng(ax, roadN));
        streets.push_back(std::move(alley));
    }
    HeroRoom rooms[kHeroBays];
    const int n = heroCornerRooms(hero, rooms);
    int added = 0;
    for (int k = 0; k < n; ++k) {
        InteriorPlan plan;
        if (!interiorRoomPlan(rooms[k].spec, plan)) continue;
        crowdv2::CrowdDoor d;
        d.id = rooms[k].spec.host.id;
        d.at = eng(plan.doorOutside.x, plan.doorOutside.y);
        d.through = eng(plan.doorInside.x, plan.doorInside.y);
        d.inside = eng(plan.room.x, plan.room.y);
        for (const MassingVec& p : plan.dwell) d.dwell.push_back(eng(p.x, p.y));
        doors.push_back(std::move(d));
        ++added;
    }
    return added;
}

std::string heroCornerDoorRows(const HeroCornerSpec& hero) {
    HeroRoom rooms[kHeroBays];
    const int n = heroCornerRooms(hero, rooms);
    std::string text;
    char line[384];
    auto pt = [](char* at, std::size_t room, double x, double y, double z) {
        return std::snprintf(at, room, "%.2f:%.2f:%.2f", x, y, z);
    };
    for (int k = 0; k < n; ++k) {
        const InteriorSpec& s = rooms[k].spec;
        InteriorPlan plan;
        if (!interiorRoomPlan(s, plan)) continue;
        int len = std::snprintf(line, sizeof line, "DOOR-%s,door,%.2f,", s.host.id.c_str(), s.gapWidth);
        len += pt(line + len, sizeof line - len, s.doorOut.x, s.doorOut.y, s.doorOut.z);
        line[len++] = ';';
        len += pt(line + len, sizeof line - len, s.doorIn.x, s.doorIn.y, s.doorIn.z);
        text.append(line, static_cast<std::size_t>(len));
        text += ",\n";
        len = std::snprintf(line, sizeof line, "ROOM-%s,%s,0,", s.host.id.c_str(), roomKindText(s.kind));
        const MassingFoot& f = plan.room;
        const double c = std::cos(f.rot * kPi / 180.0), sn = std::sin(f.rot * kPi / 180.0);
        const double cu[4] = {-1.0, 1.0, 1.0, -1.0}, cw[4] = {-1.0, -1.0, 1.0, 1.0};
        for (int q = 0; q < 4; ++q) {
            const double x = f.x + cu[q] * f.hx * c - cw[q] * f.hy * sn, y = f.y + cu[q] * f.hx * sn + cw[q] * f.hy * c;
            if (q) line[len++] = ';';
            len += pt(line + len, sizeof line - len, x, y, s.floorZ);
        }
        text.append(line, static_cast<std::size_t>(len));
        text += ",\n";
    }
    return text;
}

int interiorDressing(const InteriorPlan& plan, InteriorDressItem* out, int max, int* bare) {
    int n = 0, bareSlots = 0;
    if (bare) *bare = 0;
    const auto push = [&](HeroPiece piece, double x, double y, double z, double fx, double fy, int slot) -> InteriorDressItem* {
        if (n >= max) return nullptr;
        InteriorDressItem& it = out[n++];
        it = InteriorDressItem{};
        it.piece = piece;
        it.at = {x, y, z};
        it.faceX = fx;
        it.faceY = fy;
        it.slot = slot;
        return &it;
    };
    // A piece's size: across its front, its depth, its height, and whether its origin is its back face (else its middle).
    struct Size { double w, d, h; bool back; };
    const double counterTop = 0.95;
    const double hook = plan.ceilingTop - kInteriorFinish - 0.04;
    bool any = false;
    for (std::size_t si = 0; si < plan.slots.size(); ++si) {
        const InteriorSlot& s = plan.slots[si];
        HeroPiece piece = HeroPiece::Crate;
        Size sz{0.0, 0.0, 0.0, false};
        switch (s.kind) {
        case InteriorSlotKind::Shelf: piece = HeroPiece::Shelf; sz = {1.8, 0.4, 1.8, true}; break;
        case InteriorSlotKind::Counter: piece = HeroPiece::Counter; sz = {2.4, 0.6, counterTop, true}; break;
        case InteriorSlotKind::Display: piece = HeroPiece::Crate; sz = {0.5, 0.5, 0.5, false}; break;
        default: break;
        }
        const double fr = s.face * kPi / 180.0;
        const double fx = std::cos(fr), fy = std::sin(fr), ax = -fy, ay = fx;
        const double rr = s.foot.rot * kPi / 180.0;
        const double ex = std::cos(rr), ey = std::sin(rr);
        const double alongHalf = std::fabs(ax * ex + ay * ey) * s.foot.hx + std::fabs(-ax * ey + ay * ex) * s.foot.hy;
        const double deepHalf = std::fabs(fx * ex + fy * ey) * s.foot.hx + std::fabs(-fx * ey + fy * ex) * s.foot.hy;
        int fit = 0;
        if (sz.w > 0.0 && sz.d <= 2.0 * deepHalf + 1e-9)
            fit = static_cast<int>(std::floor((2.0 * alongHalf + kInteriorPieceGap) / (sz.w + kInteriorPieceGap) + 1e-9));
        if (fit < 1) {
            ++bareSlots;
            continue;
        }
        any = true;
        const double total = fit * sz.w + (fit - 1) * kInteriorPieceGap;
        // Each piece's back flush with the slot's back edge, the run centred along it.
        const double bx = s.foot.x - fx * deepHalf, by = s.foot.y - fy * deepHalf;
        for (int k = 0; k < fit; ++k) {
            const double along = -0.5 * total + 0.5 * sz.w + k * (sz.w + kInteriorPieceGap);
            const double px = bx + ax * along, py = by + ay * along;
            const double ox = sz.back ? px : px + fx * 0.5 * sz.d, oy = sz.back ? py : py + fy * 0.5 * sz.d;
            InteriorDressItem* it = push(piece, ox, oy, s.z0, fx, fy, static_cast<int>(si));
            if (!it) break;
            it->block.cx = px + fx * 0.5 * sz.d;
            it->block.cy = py + fy * 0.5 * sz.d;
            it->block.sx = sz.w;
            it->block.sy = sz.d;
            it->block.rot = degOf(std::atan2(ay, ax));
            it->block.z0 = s.z0;
            it->block.z1 = s.z0 + sz.h;
            if (piece != HeroPiece::Counter) continue;
            // The counter's goods half way into its top: bottles on its left slot; the stock pot and its steam on the
            // last counter's right.
            const double gx = px + fx * 0.3, gy = py + fy * 0.3, top = s.z0 + counterTop;
            push(HeroPiece::Bottles, gx + ax * kHeroCounterSlotLeft, gy + ay * kHeroCounterSlotLeft, top, fx, fy, static_cast<int>(si));
            if (k + 1 == fit) {
                const double qx = gx + ax * kHeroCounterSlotRight, qy = gy + ay * kHeroCounterSlotRight;
                push(HeroPiece::StockPot, qx, qy, top, fx, fy, static_cast<int>(si));
                push(HeroPiece::Steam, qx, qy, top + 0.32, fx, fy, static_cast<int>(si));
            }
        }
        // Goods hung over a shelf run, 0.7 m out from its face.
        if (piece == HeroPiece::Shelf)
            push(HeroPiece::HangGoods, s.foot.x + fx * (deepHalf + 0.7), s.foot.y + fy * (deepHalf + 0.7), hook, fx, fy, static_cast<int>(si));
    }
    if (any) push(HeroPiece::Lantern, plan.room.x, plan.room.y, plan.ceilingTop - kInteriorFinish - 0.02, plan.wx, plan.wy, -1);
    if (bare) *bare = bareSlots;
    return n;
}

} // namespace ftcore
