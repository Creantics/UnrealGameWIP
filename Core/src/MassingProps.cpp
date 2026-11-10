#include "ftcore/MassingProps.h"

#include "CsvRead.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_set>

namespace ftcore {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTiny = 1e-9;

using detail::fail;
using detail::isComment;
using detail::parseNumber;
using detail::Record;
using detail::splitCsv;
using detail::trim;
using detail::validId;
using detail::validKind;

bool parseSide(std::string_view s, MassingPropSide& out) {
    s = trim(s);
    if (s.empty() || s == "both") out = MassingPropSide::Both;
    else if (s == "left") out = MassingPropSide::Left;
    else if (s == "right") out = MassingPropSide::Right;
    else if (s == "stagger") out = MassingPropSide::Stagger;
    else if (s == "wall") out = MassingPropSide::Wall;
    else return false;
    return true;
}

// An optional number: empty leaves `out` at its default.
bool optionalNumber(std::string_view s, double& out) {
    if (trim(s).empty()) return true;
    return parseNumber(s, out);
}

// The distance from (x, y) to the segment a-b.
double segmentDistance(double x, double y, const MassingVec& a, const MassingVec& b) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len2 = dx * dx + dy * dy;
    double t = 0.0;
    if (len2 > kTiny) t = std::clamp(((x - a.x) * dx + (y - a.y) * dy) / len2, 0.0, 1.0);
    return std::hypot(x - (a.x + t * dx), y - (a.y + t * dy));
}

// A street's plan bounds, for skipping it cheaply.
struct Bounds {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
};

// Whether (x, y) lies nearer than half its carriageway to the centreline of a street other than `self`.
bool inOtherRoad(const std::vector<MassingStreet>& streets, const std::vector<Bounds>& bounds, std::size_t self, double x,
                 double y) {
    for (std::size_t j = 0; j < streets.size(); ++j) {
        if (j == self) continue;
        const MassingStreet& s = streets[j];
        const double half = 0.5 * massingCarriageway(s);
        const Bounds& b = bounds[j];
        if (x < b.x0 - half || x > b.x1 + half || y < b.y0 - half || y > b.y1 + half) continue;
        const std::size_t n = s.points.size();
        if (n == 1) {
            if (std::hypot(x - s.points[0].x, y - s.points[0].y) < half) return true;
            continue;
        }
        for (std::size_t k = 0; k + 1 < n; ++k)
            if (segmentDistance(x, y, s.points[k], s.points[k + 1]) < half) return true;
    }
    return false;
}

// The lowest and highest corners of a box (massingCorners, no nudge).
void boxSpan(const MassingBox& box, double& bottom, double& lowTop) {
    std::array<MassingVec, 8> c;
    massingCorners(box, 0.0, c);
    bottom = std::min({c[0].z, c[1].z, c[2].z, c[3].z});
    lowTop = std::min({c[4].z, c[5].z, c[6].z, c[7].z});
}

// The first face of a box's footprint a ray from (ox, oy) along the unit (dx, dy) meets, at a distance in [0, maxT]:
// the distance and the face's outward normal. False when it meets none, or starts inside the footprint.
bool rayToBox(const MassingBox& box, double ox, double oy, double dx, double dy, double maxT, double& t, double& nx,
              double& ny) {
    const double a = box.rot * kPi / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    const double px = ox - box.cx, py = oy - box.cy;
    const double o[2] = {px * c + py * s, -px * s + py * c};
    const double d[2] = {dx * c + dy * s, -dx * s + dy * c};
    const double h[2] = {box.sx * 0.5, box.sy * 0.5};
    double enter = -std::numeric_limits<double>::infinity(), leave = std::numeric_limits<double>::infinity();
    int axis = -1;
    for (int k = 0; k < 2; ++k) {
        if (std::fabs(d[k]) < kTiny) {
            if (std::fabs(o[k]) > h[k]) return false;
            continue;
        }
        double t1 = (-h[k] - o[k]) / d[k], t2 = (h[k] - o[k]) / d[k];
        if (t1 > t2) std::swap(t1, t2);
        if (t1 > enter) {
            enter = t1;
            axis = k;
        }
        leave = std::min(leave, t2);
    }
    if (axis < 0 || enter > leave || enter < 0.0 || enter > maxT) return false;
    t = enter;
    // The face the ray enters faces back against it, along the box's own axis.
    const double sign = d[axis] > 0.0 ? -1.0 : 1.0;
    if (axis == 0) {
        nx = sign * c;
        ny = sign * s;
    } else {
        nx = -sign * s;
        ny = sign * c;
    }
    return true;
}

// Whether a box stands as a built face within `reach` of (x, y) at `floor` (massingBuiltFaceWithin), its bottom and
// lowest top corner given.
bool builtFace(const MassingBox& box, double bottom, double lowTop, double x, double y, double floor, double reach) {
    if (box.sloped) return false;
    if (lowTop < floor + kMassingPropWallHeight || bottom >= floor + kMassingPropBody) return false;
    return massingFootprintGap(box, x, y) <= reach;
}

// The distance from (x, y) to a street's polyline (to a lone point, the distance to it).
double polylineDistance(const MassingStreet& s, double x, double y) {
    const std::size_t n = s.points.size();
    if (n == 0) return std::numeric_limits<double>::infinity();
    if (n == 1) return std::hypot(x - s.points[0].x, y - s.points[0].y);
    double d = std::numeric_limits<double>::infinity();
    for (std::size_t k = 0; k + 1 < n; ++k) d = std::min(d, segmentDistance(x, y, s.points[k], s.points[k + 1]));
    return d;
}

// A spot along a street: the centreline point and the street's direction there.
struct Spot {
    double x = 0.0, y = 0.0, dx = 0.0, dy = 0.0;
};

// Every spot `spacing` apart from `phase` along a street, a spot at its very end included.
void streetSpots(const MassingStreet& s, double phase, double spacing, std::vector<Spot>& out) {
    out.clear();
    const std::size_t n = s.points.size();
    std::size_t last = n; // the last segment with length
    for (std::size_t k = 0; k + 1 < n; ++k)
        if (std::hypot(s.points[k + 1].x - s.points[k].x, s.points[k + 1].y - s.points[k].y) > kTiny) last = k;
    if (last == n) return;
    double start = 0.0, next = phase;
    for (std::size_t k = 0; k <= last; ++k) {
        const MassingVec& a = s.points[k];
        const MassingVec& b = s.points[k + 1];
        const double len = std::hypot(b.x - a.x, b.y - a.y);
        if (len <= kTiny) continue;
        const double end = start + len;
        const double dx = (b.x - a.x) / len, dy = (b.y - a.y) / len;
        while (next < end || (k == last && next <= end + 1e-6)) {
            const double t = std::min(next - start, len);
            out.push_back({a.x + dx * t, a.y + dy * t, dx, dy});
            next += spacing;
        }
        start = end;
    }
}

} // namespace

bool importMassingProps(std::string_view csv, std::vector<MassingPropRule>& out, std::string* error) {
    out.clear();
    std::vector<Record> records;
    if (!splitCsv(csv, records, error)) return false;

    enum Col { Id, Asset, Spacing, Phase, Side, Inset, Reach, Clear, Turn, Dz, Cap, Line, FootX, FootY, Keep, Kinds, Count };
    constexpr const char* kNames[Count] = {"id",      "asset",  "spacing_m", "phase_m",  "side",
                                           "inset_m", "reach_m", "clear_m",  "turn",     "dz",
                                           "cap",     "line_m", "foot_x_m",  "foot_y_m", "keep_m",  "kinds"};
    constexpr std::size_t kRequired = 3; // id, asset, spacing_m
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
                if (c < kRequired && col[c] == r.fields.size())
                    return fail(error, r.line, std::string("no column ") + kNames[c]);
            }
            width = r.fields.size();
            haveHeader = true;
            continue;
        }
        if (r.fields.size() != width)
            return fail(error, r.line,
                        std::to_string(r.fields.size()) + " fields where the header has " + std::to_string(width));
        const auto field = [&](Col c) -> std::string_view {
            return col[c] < r.fields.size() ? std::string_view(r.fields[col[c]]) : std::string_view();
        };
        MassingPropRule rule;
        rule.id = std::string(trim(field(Id)));
        if (!validId(rule.id)) return fail(error, r.line, "id must be A-Z, 0-9, '_' or '-'");
        if (!ids.insert(rule.id).second) return fail(error, r.line, "id " + rule.id + " twice");
        rule.asset = std::string(trim(field(Asset)));
        if (rule.asset.empty()) return fail(error, r.line, rule.id + ": asset is empty");
        if (!parseNumber(field(Spacing), rule.spacing) || rule.spacing <= 0.0)
            return fail(error, r.line, rule.id + ": spacing_m must be a number > 0");
        if (!optionalNumber(field(Phase), rule.phase) || rule.phase < 0.0)
            return fail(error, r.line, rule.id + ": phase_m must be a number >= 0");
        if (!parseSide(field(Side), rule.side))
            return fail(error, r.line, rule.id + ": side must be left, right, both, stagger or wall");
        if (!optionalNumber(field(Inset), rule.inset) || rule.inset < 0.0)
            return fail(error, r.line, rule.id + ": inset_m must be a number >= 0");
        if (!optionalNumber(field(Reach), rule.reach) || rule.reach < 0.0)
            return fail(error, r.line, rule.id + ": reach_m must be a number >= 0");
        if (!optionalNumber(field(Clear), rule.clear) || rule.clear < 0.0)
            return fail(error, r.line, rule.id + ": clear_m must be a number >= 0");
        if (!optionalNumber(field(Turn), rule.turn)) return fail(error, r.line, rule.id + ": turn must be a number");
        if (!optionalNumber(field(Dz), rule.dz)) return fail(error, r.line, rule.id + ": dz must be a number");
        double cap = 0.0;
        if (!optionalNumber(field(Cap), cap) || cap < 0.0 || cap != std::floor(cap) || cap > 1e9)
            return fail(error, r.line, rule.id + ": cap must be a whole number >= 0");
        rule.cap = static_cast<std::size_t>(cap);
        if (!optionalNumber(field(Line), rule.line) || rule.line < 0.0)
            return fail(error, r.line, rule.id + ": line_m must be a number >= 0");
        if (!optionalNumber(field(FootX), rule.footX) || rule.footX < 0.0)
            return fail(error, r.line, rule.id + ": foot_x_m must be a number >= 0");
        if (!optionalNumber(field(FootY), rule.footY) || rule.footY < 0.0)
            return fail(error, r.line, rule.id + ": foot_y_m must be a number >= 0");
        if (!optionalNumber(field(Keep), rule.keep) || rule.keep < 0.0)
            return fail(error, r.line, rule.id + ": keep_m must be a number >= 0");
        std::string_view kinds = trim(field(Kinds));
        while (!kinds.empty()) {
            const std::size_t semi = kinds.find(';');
            const std::string_view one = trim(kinds.substr(0, semi));
            if (!validKind(one))
                return fail(error, r.line, rule.id + ": each of kinds must be a-z, A-Z, 0-9, '_' or '-'");
            rule.kinds.emplace_back(one);
            if (semi == std::string_view::npos) break;
            kinds = trim(kinds.substr(semi + 1));
            if (kinds.empty()) return fail(error, r.line, rule.id + ": kinds ends in ';'");
        }
        out.push_back(std::move(rule));
    }
    if (!haveHeader) return fail(error, 1, "no header row");
    return true;
}

bool massingFeetOverlap(const MassingFoot& a, const MassingFoot& b, double gap) {
    const double g = 0.5 * std::max(gap, 0.0);
    const double ah[2] = {std::max(a.hx, 0.0) + g, std::max(a.hy, 0.0) + g};
    const double bh[2] = {std::max(b.hx, 0.0) + g, std::max(b.hy, 0.0) + g};
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double reach = std::hypot(ah[0], ah[1]) + std::hypot(bh[0], bh[1]);
    if (dx * dx + dy * dy >= reach * reach) return false;
    const double ta = a.rot * kPi / 180.0, tb = b.rot * kPi / 180.0;
    const double ua[2][2] = {{std::cos(ta), std::sin(ta)}, {-std::sin(ta), std::cos(ta)}};
    const double ub[2][2] = {{std::cos(tb), std::sin(tb)}, {-std::sin(tb), std::cos(tb)}};
    const double* axes[4] = {ua[0], ua[1], ub[0], ub[1]};
    for (const double* n : axes) {
        const double d = std::fabs(dx * n[0] + dy * n[1]);
        const double pa = ah[0] * std::fabs(ua[0][0] * n[0] + ua[0][1] * n[1]) +
                          ah[1] * std::fabs(ua[1][0] * n[0] + ua[1][1] * n[1]);
        const double pb = bh[0] * std::fabs(ub[0][0] * n[0] + ub[0][1] * n[1]) +
                          bh[1] * std::fabs(ub[1][0] * n[0] + ub[1][1] * n[1]);
        if (d >= pa + pb - kTiny) return false; // a separating axis: apart, or only touching
    }
    return true;
}

MassingFoot massingPropFoot(const MassingProp& prop, const MassingPropRule& rule) {
    MassingFoot foot;
    const double sizeX = std::max(rule.footX, 0.0), sizeY = std::max(rule.footY, 0.0);
    foot.hx = 0.5 * sizeX;
    foot.hy = 0.5 * sizeY;
    foot.rot = prop.face - rule.turn;
    foot.x = prop.x;
    foot.y = prop.y;
    if (prop.wall) {
        const double t = rule.turn * kPi / 180.0;
        const double out = 0.5 * (std::fabs(std::cos(t)) * sizeX + std::fabs(std::sin(t)) * sizeY) + rule.inset;
        const double f = prop.face * kPi / 180.0;
        foot.x += std::cos(f) * out;
        foot.y += std::sin(f) * out;
    }
    return foot;
}

bool massingBuiltFaceWithin(const std::vector<MassingBox>& boxes, const std::vector<bool>& include, double x, double y,
                            double floor, double reach) {
    const std::size_t n = std::min(boxes.size(), include.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (!include[i] || boxes[i].sloped) continue;
        if (massingFootprintGap(boxes[i], x, y) > reach) continue;
        double bottom = 0.0, lowTop = 0.0;
        boxSpan(boxes[i], bottom, lowTop);
        if (builtFace(boxes[i], bottom, lowTop, x, y, floor, reach)) return true;
    }
    return false;
}

double massingPropLineGap(const std::vector<MassingStreet>& streets, const std::vector<MassingBox>& boxes,
                          const std::vector<bool>& include, double x, double y, double floor) {
    double best = std::numeric_limits<double>::infinity();
    for (const MassingStreet& s : streets) {
        if (!(s.pavement > 0.0)) continue;
        best = std::min(best, std::fabs(polylineDistance(s, x, y) - 0.5 * massingCarriageway(s)));
    }
    const std::size_t n = std::min(boxes.size(), include.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (!include[i] || boxes[i].sloped) continue;
        const double gap = massingFootprintGap(boxes[i], x, y);
        if (gap >= best) continue;
        double bottom = 0.0, lowTop = 0.0;
        boxSpan(boxes[i], bottom, lowTop);
        if (builtFace(boxes[i], bottom, lowTop, x, y, floor, gap)) best = gap;
    }
    return best;
}

void massingPlaceProps(const std::vector<MassingStreet>& streets, const std::vector<MassingPropRule>& rules,
                       const std::uint16_t* grid, std::size_t samplesX, std::size_t samplesY, double spacing,
                       double originX, double originY, const std::vector<MassingBox>& boxes,
                       const std::vector<bool>& include, double dryAbove, std::vector<MassingProp>& out,
                       std::vector<MassingPropCounts>& counts, const std::vector<MassingFoot>* taken) {
    out.clear();
    counts.assign(rules.size(), MassingPropCounts{});

    std::vector<Bounds> bounds(streets.size());
    for (std::size_t j = 0; j < streets.size(); ++j) {
        Bounds b{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                 -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
        for (const MassingVec& p : streets[j].points) {
            b.x0 = std::min(b.x0, p.x);
            b.y0 = std::min(b.y0, p.y);
            b.x1 = std::max(b.x1, p.x);
            b.y1 = std::max(b.y1, p.y);
        }
        bounds[j] = b;
    }
    const std::size_t nBoxes = std::min(boxes.size(), include.size());
    std::vector<double> bottoms(nBoxes, 0.0), lowTops(nBoxes, 0.0);
    for (std::size_t i = 0; i < nBoxes; ++i)
        if (include[i]) boxSpan(boxes[i], bottoms[i], lowTops[i]);

    const auto groundAt = [&](double x, double y) {
        return massingGroundAt(grid, samplesX, samplesY, spacing, originX, originY, x, y);
    };
    // The floor at a point near a street whose own floor is `street`: the ground, or a top no higher than a step over
    // the higher of the two.
    const auto floorNear = [&](double x, double y, double street, int& on) {
        const double g = groundAt(x, y);
        return massingFloorAt(boxes, include, x, y, g, std::max(g, street) + kMassingPropMaxStep, &on);
    };
    const auto steepAt = [&](double x, double y, int on) {
        if (on >= 0) return boxes[static_cast<std::size_t>(on)].sloped;
        const double g = groundAt(x, y);
        const double d = kMassingPropFoot;
        return std::fabs(groundAt(x + d, y) - g) > kMassingPropMaxStep ||
               std::fabs(groundAt(x - d, y) - g) > kMassingPropMaxStep ||
               std::fabs(groundAt(x, y + d) - g) > kMassingPropMaxStep ||
               std::fabs(groundAt(x, y - d) - g) > kMassingPropMaxStep;
    };
    // A box `include` marks that stands over `floor` at a body's height, within `clear` of (x, y).
    const auto blockedAt = [&](double x, double y, double floor, double clear) {
        for (std::size_t i = 0; i < nBoxes; ++i) {
            if (!include[i]) continue;
            if (massingFootprintGap(boxes[i], x, y) > clear) continue;
            double bottom = bottoms[i], top = lowTops[i];
            if (boxes[i].sloped) {
                // A ramp or flight under the spot counts by its corners' extremes.
                std::array<MassingVec, 8> c;
                massingCorners(boxes[i], 0.0, c);
                top = std::max({c[4].z, c[5].z, c[6].z, c[7].z});
            }
            if (top > floor + kMassingStandClear && bottom < floor + kMassingPropBody) return true;
        }
        return false;
    };
    // No built face within the rule's line of (x, y) at `floor`.
    const auto openAt = [&](const MassingPropRule& rule, double x, double y, double floor) {
        if (!(rule.line > 0.0)) return false;
        for (std::size_t i = 0; i < nBoxes; ++i)
            if (include[i] && builtFace(boxes[i], bottoms[i], lowTops[i], x, y, floor, rule.line)) return false;
        return true;
    };
    // The feet placed so far by earlier rules, and passed so far by this one (those with area only).
    std::vector<MassingFoot> placedFeet, passedFeet;
    const auto hasArea = [](const MassingPropRule& rule) { return rule.footX > 0.0 && rule.footY > 0.0; };
    const auto crowdedBy = [](const MassingFoot& foot, const std::vector<MassingFoot>& feet, double gap) {
        for (const MassingFoot& other : feet)
            if (other.hx > 0.0 && other.hy > 0.0 && massingFeetOverlap(foot, other, gap)) return true;
        return false;
    };
    const auto crowdedAt = [&](const MassingPropRule& rule, const MassingProp& prop) {
        if (!hasArea(rule)) return false;
        const MassingFoot foot = massingPropFoot(prop, rule);
        const double keep = std::max(kMassingPropGap, rule.keep);
        return (taken && crowdedBy(foot, *taken, kMassingPropGap)) || crowdedBy(foot, placedFeet, keep) ||
               crowdedBy(foot, passedFeet, keep);
    };

    std::vector<Spot> spots;
    std::vector<MassingProp> passed;
    for (std::size_t r = 0; r < rules.size(); ++r) {
        const MassingPropRule& rule = rules[r];
        MassingPropCounts& count = counts[r];
        passed.clear();
        passedFeet.clear();
        const auto pass = [&](const MassingProp& prop) {
            passed.push_back(prop);
            if (hasArea(rule)) passedFeet.push_back(massingPropFoot(prop, rule));
        };
        for (std::size_t j = 0; j < streets.size(); ++j) {
            const MassingStreet& street = streets[j];
            if (!rule.kinds.empty() && std::find(rule.kinds.begin(), rule.kinds.end(), street.kind) == rule.kinds.end())
                continue;
            streetSpots(street, rule.phase, rule.spacing, spots);
            const double half = 0.5 * street.width;
            for (std::size_t k = 0; k < spots.size(); ++k) {
                const Spot& p = spots[k];
                const double gc = groundAt(p.x, p.y);
                const double centre = massingFloorAt(boxes, include, p.x, p.y, gc, gc + kMassingPropDeckReach);
                // Left is a quarter turn counter-clockwise from the street's direction.
                const double lx = -p.dy, ly = p.dx;
                if (rule.side == MassingPropSide::Wall) {
                    ++count.spots;
                    bool found = false;
                    MassingProp prop;
                    for (int sideSign = 1; sideSign >= -1 && !found; sideSign -= 2) {
                        const double nx = lx * sideSign, ny = ly * sideSign;
                        double bestT = std::numeric_limits<double>::infinity(), fx = 0.0, fy = 0.0;
                        std::size_t best = nBoxes;
                        for (std::size_t i = 0; i < nBoxes; ++i) {
                            if (!include[i]) continue;
                            double t = 0.0, ox = 0.0, oy = 0.0;
                            if (!rayToBox(boxes[i], p.x, p.y, nx, ny, half + rule.reach, t, ox, oy)) continue;
                            if (t < bestT) {
                                bestT = t;
                                best = i;
                                fx = ox;
                                fy = oy;
                            }
                        }
                        if (best == nBoxes) continue;
                        const double hx = p.x + nx * bestT, hy = p.y + ny * bestT;
                        const double footX = hx + fx * kMassingPropFoot, footY = hy + fy * kMassingPropFoot;
                        int on = -1;
                        const double floor = floorNear(footX, footY, centre, on);
                        if (lowTops[best] - floor < kMassingPropWallHeight) continue;
                        found = true;
                        prop.rule = r;
                        prop.street = j;
                        prop.x = hx;
                        prop.y = hy;
                        prop.z = floor + rule.dz;
                        prop.face = std::atan2(fy, fx) * 180.0 / kPi;
                        prop.wall = true;
                        if (openAt(rule, hx, hy, floor)) {
                            ++count.open;
                        } else if (inOtherRoad(streets, bounds, j, hx + fx * rule.inset, hy + fy * rule.inset)) {
                            ++count.inRoad;
                        } else if (steepAt(footX, footY, on)) {
                            ++count.steep;
                        } else if (floor <= dryAbove) {
                            ++count.wet;
                        } else if (crowdedAt(rule, prop)) {
                            ++count.crowded;
                        } else {
                            pass(prop);
                        }
                    }
                    if (!found) ++count.noWall;
                    continue;
                }
                // Kerb props: the sides this spot takes, left as +1.
                int sides[2] = {0, 0};
                switch (rule.side) {
                case MassingPropSide::Left: sides[0] = 1; break;
                case MassingPropSide::Right: sides[0] = -1; break;
                case MassingPropSide::Both:
                    sides[0] = 1;
                    sides[1] = -1;
                    break;
                case MassingPropSide::Stagger: sides[0] = (k % 2 == 0) ? 1 : -1; break;
                case MassingPropSide::Wall: break;
                }
                const double offset = std::max(half - rule.inset, 0.0);
                for (const int sideSign : sides) {
                    if (sideSign == 0) continue;
                    ++count.spots;
                    const double nx = lx * sideSign, ny = ly * sideSign;
                    const double x = p.x + nx * offset, y = p.y + ny * offset;
                    int on = -1;
                    const double floor = floorNear(x, y, centre, on);
                    MassingProp prop;
                    prop.rule = r;
                    prop.street = j;
                    prop.x = x;
                    prop.y = y;
                    prop.z = floor + rule.dz;
                    prop.face = std::atan2(-ny, -nx) * 180.0 / kPi;
                    if (blockedAt(x, y, floor, rule.clear)) {
                        ++count.blocked;
                    } else if (openAt(rule, x, y, floor)) {
                        ++count.open;
                    } else if (inOtherRoad(streets, bounds, j, x, y)) {
                        ++count.inRoad;
                    } else if (steepAt(x, y, on)) {
                        ++count.steep;
                    } else if (floor <= dryAbove) {
                        ++count.wet;
                    } else if (crowdedAt(rule, prop)) {
                        ++count.crowded;
                    } else {
                        pass(prop);
                    }
                }
            }
        }
        const std::size_t first = out.size();
        if (rule.cap > 0 && passed.size() > rule.cap) {
            count.capped = passed.size() - rule.cap;
            for (std::size_t k = 0; k < rule.cap; ++k) out.push_back(passed[k * passed.size() / rule.cap]);
            count.placed = rule.cap;
        } else {
            out.insert(out.end(), passed.begin(), passed.end());
            count.placed = passed.size();
        }
        if (hasArea(rule))
            for (std::size_t k = first; k < out.size(); ++k) placedFeet.push_back(massingPropFoot(out[k], rule));
    }
}

bool massingPropPlacement(const MassingProp& prop, double side, const MassingVec& meshMin, const MassingVec& meshMax,
                          double turnDeg, double inset, MassingKitPlace& out) {
    const MassingVec size{meshMax.x - meshMin.x, meshMax.y - meshMin.y, meshMax.z - meshMin.z};
    if (size.x < 0.0 || size.y < 0.0 || size.z < 0.0) return false;
    MassingKitPlace place;
    const double facing = massingEngineYaw(prop.face); // engine yaw the prop faces
    place.yaw = facing + turnDeg;
    MassingVec anchor = massingEnginePoint(side, prop.x, prop.y, prop.z);
    if (prop.wall) {
        // The bounds' depth along the facing (the facing lies at -turnDeg in the mesh's own axes), half of it, set out
        // from the face with the inset.
        const double t = turnDeg * kPi / 180.0;
        const double halfDepth = 0.5 * (std::fabs(std::cos(t)) * size.x + std::fabs(std::sin(t)) * size.y);
        const double f = facing * kPi / 180.0;
        anchor.x += std::cos(f) * (halfDepth + inset);
        anchor.y += std::sin(f) * (halfDepth + inset);
    }
    const double a = place.yaw * kPi / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    const double mx = (meshMin.x + meshMax.x) * 0.5;
    const double my = (meshMin.y + meshMax.y) * 0.5;
    place.at = {anchor.x - (mx * c - my * s), anchor.y - (mx * s + my * c), anchor.z - meshMin.z};
    out = place;
    return true;
}

} // namespace ftcore
