#include "ftcore/MassingPlan.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <unordered_set>

namespace ftcore {

namespace {

constexpr double kPi = 3.14159265358979323846;

bool fail(std::string* error, std::size_t line, const std::string& what) {
    if (error) *error = "line " + std::to_string(line) + ": " + what;
    return false;
}

// One CSV record: its fields and the line it starts on.
struct Record {
    std::vector<std::string> fields;
    std::size_t line = 0;
};

// Splits RFC 4180 CSV into records. A quoted field may hold commas, doubled quotes and line breaks.
// Blank lines are skipped. False on a quote left open or text after a closing quote.
bool splitCsv(std::string_view text, std::vector<Record>& out, std::string* error) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);
    std::size_t i = 0, line = 1;
    const std::size_t n = text.size();
    while (i < n) {
        Record rec;
        rec.line = line;
        std::string field;
        bool any = false; // the record has a field or a separator
        for (;;) {
            if (i < n && text[i] == '"') {
                any = true;
                ++i;
                for (;;) {
                    if (i >= n) return fail(error, rec.line, "a quoted field is never closed");
                    const char c = text[i++];
                    if (c == '"') {
                        if (i < n && text[i] == '"') {
                            field.push_back('"');
                            ++i;
                            continue;
                        }
                        break;
                    }
                    if (c == '\n') ++line;
                    field.push_back(c);
                }
                if (i < n && text[i] != ',' && text[i] != '\n' && text[i] != '\r')
                    return fail(error, line, "text after a closing quote");
            } else {
                while (i < n && text[i] != ',' && text[i] != '\n' && text[i] != '\r') {
                    field.push_back(text[i++]);
                    any = true;
                }
            }
            if (i < n && text[i] == ',') {
                any = true;
                rec.fields.push_back(std::move(field));
                field.clear();
                ++i;
                continue;
            }
            rec.fields.push_back(std::move(field));
            break;
        }
        if (i < n && text[i] == '\r') ++i;
        if (i < n && text[i] == '\n') {
            ++i;
            ++line;
        }
        if (any) out.push_back(std::move(rec));
    }
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

bool parseNumber(std::string_view s, double& out) {
    s = trim(s);
    if (s.empty()) return false;
    if (s.front() == '+') s.remove_prefix(1);
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && end == s.data() + s.size() && std::isfinite(out);
}

// "x A to B" with an optional ", T thick".
bool parseSlope(std::string_view s, MassingBox& box) {
    s = trim(s);
    if (s.size() < 2 || s[0] != 'x' || s[1] != ' ') return false;
    s.remove_prefix(2);
    const std::size_t to = s.find(" to ");
    if (to == std::string_view::npos) return false;
    if (!parseNumber(s.substr(0, to), box.slopeFrom)) return false;
    s.remove_prefix(to + 4);
    const std::size_t comma = s.find(',');
    if (!parseNumber(s.substr(0, comma), box.slopeTo)) return false;
    box.slopeThick = -1.0;
    if (comma != std::string_view::npos) {
        std::string_view rest = trim(s.substr(comma + 1));
        constexpr std::string_view kThick = " thick";
        if (rest.size() <= kThick.size() || rest.substr(rest.size() - kThick.size()) != kThick) return false;
        rest.remove_suffix(kThick.size());
        if (!parseNumber(rest, box.slopeThick) || box.slopeThick <= 0.0) return false;
    }
    box.sloped = true;
    return true;
}

bool parseSource(std::string_view s, MassingSource& out) {
    s = trim(s);
    if (s == "BLD") out = MassingSource::Plan;
    else if (s == "INV") out = MassingSource::Invented;
    else if (s == "Z") out = MassingSource::Zone;
    else if (s == "OPEN") out = MassingSource::Open;
    else if (s == "PROXY") out = MassingSource::Proxy;
    else if (s == "WALL") out = MassingSource::Wall;
    else return false;
    return true;
}

bool validId(std::string_view id) {
    if (id.empty()) return false;
    for (const char c : id)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

bool validKind(std::string_view kind) {
    if (kind.empty()) return false;
    for (const char c : kind)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

bool isComment(const Record& r) {
    const std::string_view first = trim(r.fields.front());
    return !first.empty() && first.front() == '#';
}

struct Ring {
    double u, w;
};
constexpr Ring kRing[4] = {{-0.5, -0.5}, {0.5, -0.5}, {0.5, 0.5}, {-0.5, 0.5}};
// Outward in the plan frame (right-handed); massingMesh checks each against the engine's winding.
constexpr std::uint8_t kFaces[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};

struct Point2 {
    double x, y;
};

void footprint(const MassingBox& b, Point2 (&out)[4]) {
    const double a = b.rot * kPi / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    for (int k = 0; k < 4; ++k) {
        const double u = kRing[k].u * b.sx, w = kRing[k].w * b.sy;
        out[k] = {b.cx + u * c - w * s, b.cy + u * s + w * c};
    }
}

void aabbHalf(const MassingBox& b, double& hx, double& hy) {
    const double a = b.rot * kPi / 180.0;
    const double c = std::fabs(std::cos(a)), s = std::fabs(std::sin(a));
    hx = b.sx * 0.5 * c + b.sy * 0.5 * s;
    hy = b.sx * 0.5 * s + b.sy * 0.5 * c;
}

} // namespace

MassingRole massingRole(const MassingBox& box) {
    if (box.source == MassingSource::Zone) return MassingRole::Zone;
    if (box.kind == "interior" || box.kind == "trigger" || box.kind == "underpass") return MassingRole::Hidden;
    if (box.source == MassingSource::Open) return MassingRole::Open;
    if (box.source == MassingSource::Proxy) return MassingRole::Proxy;
    if (box.source == MassingSource::Wall) return MassingRole::Wall;
    return box.cover ? MassingRole::Named : MassingRole::Kit;
}

MassingFinish massingFinish(const MassingBox& box) {
    switch (massingRole(box)) {
    case MassingRole::Kit:
        return box.kind == "wall" ? MassingFinish::Wall : MassingFinish::Plain;
    case MassingRole::Named:
        return (box.kind == "deck" || box.kind == "pier" || box.kind == "bridge" || box.kind == "quay")
                   ? MassingFinish::Deck
                   : MassingFinish::Plain;
    default:
        return MassingFinish::Plain;
    }
}

bool importMassingBoxes(std::string_view csv, std::vector<MassingBox>& out, std::string* error) {
    out.clear();
    std::vector<Record> records;
    if (!splitCsv(csv, records, error)) return false;

    enum Col { Id, Src, Kind, Cx, Cy, Sx, Sy, Rot, Z0, Z1, Cover, Slope, Count };
    constexpr const char* kNames[Count] = {"id", "src", "kind", "cx", "cy", "sx", "sy", "rot", "z0", "z1", "cover", "slope"};
    std::size_t col[Count] = {};
    std::size_t width = 0;
    bool haveHeader = false;
    std::unordered_set<std::string> ids;

    for (const Record& r : records) {
        if (isComment(r)) continue;
        if (!haveHeader) {
            for (std::size_t c = 0; c < Count; ++c) {
                col[c] = r.fields.size();
                for (std::size_t f = 0; f < r.fields.size(); ++f) {
                    if (trim(r.fields[f]) != kNames[c]) continue;
                    if (col[c] != r.fields.size()) return fail(error, r.line, std::string("column ") + kNames[c] + " twice");
                    col[c] = f;
                }
                if (col[c] == r.fields.size()) return fail(error, r.line, std::string("no column ") + kNames[c]);
            }
            width = r.fields.size();
            haveHeader = true;
            continue;
        }
        if (r.fields.size() != width)
            return fail(error, r.line,
                        std::to_string(r.fields.size()) + " fields where the header has " + std::to_string(width));
        MassingBox b;
        b.id = std::string(trim(r.fields[col[Id]]));
        if (!validId(b.id)) return fail(error, r.line, "id must be A-Z, 0-9, '_' or '-'");
        if (!ids.insert(b.id).second) return fail(error, r.line, "id " + b.id + " twice");
        if (!parseSource(r.fields[col[Src]], b.source)) return fail(error, r.line, b.id + ": unknown src");
        b.kind = std::string(trim(r.fields[col[Kind]]));
        if (!validKind(b.kind)) return fail(error, r.line, b.id + ": kind must be a-z, 0-9 or '_'");
        struct Num {
            Col c;
            double* v;
        };
        const Num nums[] = {{Cx, &b.cx}, {Cy, &b.cy}, {Sx, &b.sx}, {Sy, &b.sy}, {Rot, &b.rot}, {Z0, &b.z0}, {Z1, &b.z1}};
        for (const Num& num : nums)
            if (!parseNumber(r.fields[col[num.c]], *num.v))
                return fail(error, r.line, b.id + ": " + kNames[num.c] + " is not a number");
        if (b.sx <= 0.0 || b.sy <= 0.0) return fail(error, r.line, b.id + ": sx and sy must be > 0");
        if (b.z1 < b.z0) return fail(error, r.line, b.id + ": z1 under z0");
        const std::string_view cover = trim(r.fields[col[Cover]]);
        if (cover == "True") b.cover = true;
        else if (cover == "False") b.cover = false;
        else return fail(error, r.line, b.id + ": cover must be True or False");
        const std::string_view slope = trim(r.fields[col[Slope]]);
        if (!slope.empty() && !parseSlope(slope, b))
            return fail(error, r.line, b.id + ": slope must be \"x A to B\" with an optional \", T thick\"");
        out.push_back(std::move(b));
    }
    if (!haveHeader) return fail(error, 1, "no header row");
    return true;
}

void massingCorners(const MassingBox& box, double topNudge, std::array<MassingVec, 8>& out) {
    const double a = box.rot * kPi / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    const bool zone = box.source == MassingSource::Zone;
    const double flatTop = box.z0 + std::max(box.z1 + topNudge - box.z0, kMassingMinHeight);
    for (int top = 0; top < 2; ++top) {
        for (int k = 0; k < 4; ++k) {
            const double u = kRing[k].u * box.sx, w = kRing[k].w * box.sy;
            double z;
            if (box.sloped) {
                const double zt = box.slopeFrom + (box.slopeTo - box.slopeFrom) * (kRing[k].u + 0.5);
                if (top) z = zt + (zone ? kMassingZonePlate : 0.0);
                else if (zone) z = zt - 1.0;
                else if (box.slopeThick > 0.0) z = zt - box.slopeThick;
                else z = box.z0;
            } else {
                z = top ? flatTop : box.z0;
            }
            out[static_cast<std::size_t>(top * 4 + k)] = {box.cx + u * c - w * s, box.cy + u * s + w * c, z};
        }
    }
}

void massingMesh(const MassingBox& box, double side, double topNudge, MassingMesh& out) {
    std::array<MassingVec, 8> plan;
    massingCorners(box, topNudge, plan);
    MassingVec centroid;
    for (std::size_t k = 0; k < 8; ++k) {
        out.v[k] = massingEnginePoint(side, plan[k].x, plan[k].y, plan[k].z);
        centroid.x += out.v[k].x / 8.0;
        centroid.y += out.v[k].y / 8.0;
        centroid.z += out.v[k].z / 8.0;
    }
    for (std::size_t f = 0; f < 6; ++f) {
        std::array<std::uint8_t, 4> q = {kFaces[f][0], kFaces[f][1], kFaces[f][2], kFaces[f][3]};
        // The face's normal by Newell's method (sound for a quad that isn't quite flat) and its
        // direction from the centroid: Unreal's winding wants the right-handed normal pointing in.
        MassingVec n, mid;
        for (std::size_t k = 0; k < 4; ++k) {
            const MassingVec& p = out.v[q[k]];
            const MassingVec& r = out.v[q[(k + 1) % 4]];
            n.x += (p.y - r.y) * (p.z + r.z);
            n.y += (p.z - r.z) * (p.x + r.x);
            n.z += (p.x - r.x) * (p.y + r.y);
            mid.x += p.x / 4.0;
            mid.y += p.y / 4.0;
            mid.z += p.z / 4.0;
        }
        const double outward = n.x * (mid.x - centroid.x) + n.y * (mid.y - centroid.y) + n.z * (mid.z - centroid.z);
        if (outward > 0.0) std::swap(q[1], q[3]);
        out.quads[f] = q;
    }
}

bool massingFootprintsOverlap(const MassingBox& a, const MassingBox& b) {
    double ax, ay, bx, by;
    aabbHalf(a, ax, ay);
    aabbHalf(b, bx, by);
    if (std::fabs(a.cx - b.cx) >= ax + bx || std::fabs(a.cy - b.cy) >= ay + by) return false;
    Point2 pa[4], pb[4];
    footprint(a, pa);
    footprint(b, pb);
    const Point2* polys[2] = {pa, pb};
    for (const Point2* poly : polys) {
        for (int i = 0; i < 4; ++i) {
            const double ex = poly[(i + 1) % 4].x - poly[i].x, ey = poly[(i + 1) % 4].y - poly[i].y;
            const double nx = -ey, ny = ex;
            double minA = 1e300, maxA = -1e300, minB = 1e300, maxB = -1e300;
            for (int k = 0; k < 4; ++k) {
                const double qa = nx * pa[k].x + ny * pa[k].y, qb = nx * pb[k].x + ny * pb[k].y;
                minA = std::min(minA, qa);
                maxA = std::max(maxA, qa);
                minB = std::min(minB, qb);
                maxB = std::max(maxB, qb);
            }
            if (maxA <= minB + 1e-6 || maxB <= minA + 1e-6) return false;
        }
    }
    return true;
}

double massingFootprintGap(const MassingBox& box, double x, double y) {
    const double a = box.rot * kPi / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    const double dx = x - box.cx, dy = y - box.cy;
    // The point in the box's own axes, past each half-size.
    const double u = std::fabs(dx * c + dy * s) - box.sx * 0.5;
    const double w = std::fabs(dy * c - dx * s) - box.sy * 0.5;
    return std::hypot(std::max(u, 0.0), std::max(w, 0.0));
}

void massingTopNudges(const std::vector<MassingBox>& boxes, const std::vector<bool>& include,
                      std::vector<double>& out) {
    const std::size_t n = boxes.size();
    out.assign(n, 0.0);
    std::vector<std::size_t> flat;
    for (std::size_t i = 0; i < n; ++i)
        if (i < include.size() && include[i] && !boxes[i].sloped) flat.push_back(i);
    std::vector<std::vector<std::size_t>> adj(n);
    for (std::size_t p = 0; p < flat.size(); ++p)
        for (std::size_t q = p + 1; q < flat.size(); ++q) {
            const MassingBox& a = boxes[flat[p]];
            const MassingBox& b = boxes[flat[q]];
            if (std::fabs(a.z1 - b.z1) < kMassingSharedTop && massingFootprintsOverlap(a, b)) {
                adj[flat[p]].push_back(flat[q]);
                adj[flat[q]].push_back(flat[p]);
            }
        }
    std::vector<std::size_t> order;
    for (const std::size_t i : flat)
        if (!adj[i].empty()) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return boxes[a].sx * boxes[a].sy < boxes[b].sx * boxes[b].sy;
    });
    std::vector<int> level(n, -1);
    std::vector<bool> used;
    for (const std::size_t i : order) {
        used.assign(adj[i].size() + 1, false);
        for (const std::size_t j : adj[i])
            if (level[j] >= 0 && static_cast<std::size_t>(level[j]) < used.size()) used[static_cast<std::size_t>(level[j])] = true;
        std::size_t lv = 0;
        while (used[lv]) ++lv;
        level[i] = static_cast<int>(lv);
        out[i] = -kMassingNudgeStep * static_cast<double>(lv);
    }
}

std::size_t massingLandscapeSide(std::size_t samplesX, std::size_t samplesY, std::size_t quadsPerComponent) {
    if (quadsPerComponent == 0 || samplesX == 0 || samplesY == 0) return 0;
    const std::size_t need = std::max(samplesX, samplesY);
    const std::size_t components = (need - 1 + quadsPerComponent - 1) / quadsPerComponent;
    return std::max<std::size_t>(components, 1) * quadsPerComponent + 1;
}

bool massingLandscapeHeights(const std::uint16_t* grid, std::size_t samplesX, std::size_t samplesY, std::size_t side,
                             std::uint16_t* out) {
    if (!grid || !out || samplesX == 0 || samplesY == 0 || side < samplesX || side < samplesY) return false;
    for (std::size_t r = 0; r < side; ++r) {
        const std::size_t row = std::min(side - 1 - r, samplesY - 1);
        const std::uint16_t* src = grid + row * samplesX;
        std::uint16_t* dst = out + r * side;
        for (std::size_t c = 0; c < side; ++c) dst[c] = src[std::min(c, samplesX - 1)];
    }
    return true;
}

MassingVec massingLandscapeCorner(double squareSide, double originX, double originY, double spacing, std::size_t side) {
    const double north = originY + static_cast<double>(side > 0 ? side - 1 : 0) * spacing;
    return massingEnginePoint(squareSide, originX, north, 0.0);
}

double massingGroundAt(const std::uint16_t* grid, std::size_t samplesX, std::size_t samplesY, double spacing,
                       double originX, double originY, double x, double y) {
    if (!grid || samplesX == 0 || samplesY == 0 || !(spacing > 0.0) || !std::isfinite(x) || !std::isfinite(y))
        return 0.0;
    const double fx = std::clamp((x - originX) / spacing, 0.0, static_cast<double>(samplesX - 1));
    const double fy = std::clamp((y - originY) / spacing, 0.0, static_cast<double>(samplesY - 1));
    const std::size_t c0 = static_cast<std::size_t>(fx), r0 = static_cast<std::size_t>(fy);
    const std::size_t c1 = std::min(c0 + 1, samplesX - 1), r1 = std::min(r0 + 1, samplesY - 1);
    const double tx = fx - static_cast<double>(c0), ty = fy - static_cast<double>(r0);
    const auto h = [&](std::size_t c, std::size_t r) { return massingHeightFromValue(grid[r * samplesX + c]); };
    const double south = h(c0, r0) + (h(c1, r0) - h(c0, r0)) * tx;
    const double north = h(c0, r1) + (h(c1, r1) - h(c0, r1)) * tx;
    return south + (north - south) * ty;
}

namespace {

// The box's solid over a plan point: false when its footprint doesn't hold the point.
bool solidAt(const MassingBox& b, double x, double y, double& bottom, double& top) {
    if (!(b.sx > 0.0) || !(b.sy > 0.0)) return false;
    const double a = b.rot * kPi / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    const double dx = x - b.cx, dy = y - b.cy;
    const double u = dx * c + dy * s, w = -dx * s + dy * c; // massingCorners' rotation, undone
    if (std::fabs(u) > b.sx * 0.5 || std::fabs(w) > b.sy * 0.5) return false;
    std::array<MassingVec, 8> corners;
    massingCorners(b, 0.0, corners);
    const double t = u / b.sx + 0.5; // the rings run (-u) to (+u) from their first corner to their second
    bottom = corners[0].z + (corners[1].z - corners[0].z) * t;
    top = corners[4].z + (corners[5].z - corners[4].z) * t;
    return true;
}

} // namespace

double massingFloorAt(const std::vector<MassingBox>& boxes, const std::vector<bool>& include, double x, double y,
                      double ground, double ceiling, int* onBox) {
    double floor = ground;
    int on = -1;
    const std::size_t n = std::min(boxes.size(), include.size());
    for (std::size_t i = 0; i < n; ++i) {
        double bottom = 0.0, top = 0.0;
        if (!include[i] || !solidAt(boxes[i], x, y, bottom, top)) continue;
        if (top <= ceiling && top > floor) {
            floor = top;
            on = static_cast<int>(i);
        }
    }
    if (onBox) *onBox = on;
    return floor;
}

int massingBlockingAt(const std::vector<MassingBox>& boxes, const std::vector<bool>& include, double x, double y,
                      double floor, double height) {
    const std::size_t n = std::min(boxes.size(), include.size());
    for (std::size_t i = 0; i < n; ++i) {
        double bottom = 0.0, top = 0.0;
        if (!include[i] || !solidAt(boxes[i], x, y, bottom, top)) continue;
        if (top > floor + kMassingStandClear && bottom < floor + height) return static_cast<int>(i);
    }
    return -1;
}

} // namespace ftcore
