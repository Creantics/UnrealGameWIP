#include "ftcore/SeaAccess.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace ftcore {

namespace {

constexpr double kMaxCells = 50000000.0;

bool inside(const SeaRect& r, double x, double y) { return x >= r.minX && x <= r.maxX && y >= r.minY && y <= r.maxY; }

bool inAny(const std::vector<SeaRect>& rects, double x, double y) {
    for (const SeaRect& r : rects)
        if (inside(r, x, y)) return true;
    return false;
}

} // namespace

bool seaLadders(const std::vector<SeaFace>& faces, const std::vector<SeaRect>& avoid, const SeaLadderSpec& spec,
                std::vector<SeaLadder>& out, std::string* error) {
    if (!(spec.spacing >= 1.0)) {
        if (error) *error = "the ladder spacing is under 1 m";
        return false;
    }
    const std::size_t first = out.size();
    for (const SeaFace& f : faces) {
        const double dx = f.x1 - f.x0, dy = f.y1 - f.y0;
        const double len = std::sqrt(dx * dx + dy * dy);
        if (!(len > 0.0)) {
            if (error) *error = "the face " + f.id + " has no length";
            return false;
        }
        const double ux = dx / len, uy = dy / len;
        const double run = len - 2.0 * spec.endMargin;
        // At most `spacing` apart; a face shorter than that gets one, at its middle.
        const int n = run < spec.spacing ? 1 : static_cast<int>(std::ceil(run / spec.spacing - 1e-9)) + 1;
        int made = 0;
        for (int k = 0; k < n; ++k) {
            const double t = n == 1 ? 0.5 * len : spec.endMargin + run * k / (n - 1);
            const double px = f.x0 + ux * t, py = f.y0 + uy * t;
            SeaLadder l;
            l.footX = px - f.outX * spec.footOut;
            l.footY = py - f.outY * spec.footOut;
            l.topX = px + f.outX * spec.topIn;
            l.topY = py + f.outY * spec.topIn;
            l.topZ = f.topZ;
            l.outX = f.outX;
            l.outY = f.outY;
            if (inAny(avoid, l.footX, l.footY) || inAny(avoid, l.topX, l.topY)) continue;
            bool crowded = false;
            for (std::size_t i = first; i < out.size() && !crowded; ++i) {
                const double ex = out[i].footX - l.footX, ey = out[i].footY - l.footY;
                crowded = ex * ex + ey * ey < spec.minGap * spec.minGap;
            }
            if (crowded) continue;
            // The rail gap, on the rail's line (kMassingRailIn onto the deck) either side of the ladder.
            l.gapX0 = px - ux * spec.gapHalf;
            l.gapY0 = py - uy * spec.gapHalf;
            l.gapX1 = px + ux * spec.gapHalf;
            l.gapY1 = py + uy * spec.gapHalf;
            l.id = f.id + "-L" + std::to_string(++made);
            out.push_back(l);
        }
    }
    return true;
}

bool seaExitReach(const std::vector<SeaRect>& water, const std::vector<SeaRect>& solid, const std::vector<SeaPoint>& exits,
                  double cell, double limit, SeaReach& out, std::string* error) {
    out = SeaReach{};
    if (exits.empty()) {
        if (error) *error = "no ways out";
        return false;
    }
    if (!(cell >= 0.25)) {
        if (error) *error = "the reach cell is under 0.25 m";
        return false;
    }
    for (const SeaRect& w : water) {
        const double nx = std::floor((w.maxX - w.minX) / cell), ny = std::floor((w.maxY - w.minY) / cell);
        if (nx * ny > kMaxCells) {
            if (error) *error = "the water is over 50 million cells";
            return false;
        }
    }
    // Cells already read in an earlier rect aren't read again (rects may overlap).
    for (std::size_t wi = 0; wi < water.size(); ++wi) {
        const SeaRect& w = water[wi];
        for (double y = w.minY + 0.5 * cell; y < w.maxY; y += cell) {
            for (double x = w.minX + 0.5 * cell; x < w.maxX; x += cell) {
                bool seen = false;
                for (std::size_t k = 0; k < wi && !seen; ++k) seen = inside(water[k], x, y);
                if (seen || inAny(solid, x, y)) continue;
                double best = std::numeric_limits<double>::max();
                for (const SeaPoint& e : exits) best = std::min(best, (e.x - x) * (e.x - x) + (e.y - y) * (e.y - y));
                best = std::sqrt(best);
                ++out.cells;
                if (best > limit) ++out.over;
                if (best > out.worst) {
                    out.worst = best;
                    out.worstX = x;
                    out.worstY = y;
                }
            }
        }
    }
    return true;
}

std::vector<MassingBox> seaRailGaps(const std::vector<MassingBox>& furniture, const std::vector<SeaLadder>& ladders, double clear,
                                    double reach, SeaRailCounts& counts) {
    counts = SeaRailCounts{};
    std::vector<MassingBox> out;
    out.reserve(furniture.size() + ladders.size());
    struct Span {
        double a, b;
    };
    std::vector<Span> spans, next;
    for (const MassingBox& box : furniture) {
        if (box.kind == "barrier") {
            const double r = box.rot * 3.14159265358979323846 / 180.0;
            const double ux = std::cos(r), uy = std::sin(r);
            const double half = 0.5 * box.sx;
            spans.assign(1, Span{-half, half});
            bool cut = false;
            for (const SeaLadder& l : ladders) {
                const double ax = l.gapX0 - box.cx, ay = l.gapY0 - box.cy, bx = l.gapX1 - box.cx, by = l.gapY1 - box.cy;
                const double va = -ax * uy + ay * ux, vb = -bx * uy + by * ux;
                if (std::fabs(va) > reach || std::fabs(vb) > reach) continue;
                const double ua = ax * ux + ay * uy, ub = bx * ux + by * uy;
                const double g0 = std::min(ua, ub) - clear, g1 = std::max(ua, ub) + clear;
                if (g1 <= -half || g0 >= half) continue;
                cut = true;
                next.clear();
                for (const Span& s : spans) {
                    if (g1 <= s.a || g0 >= s.b) {
                        next.push_back(s);
                        continue;
                    }
                    if (g0 > s.a) next.push_back(Span{s.a, g0});
                    if (g1 < s.b) next.push_back(Span{g1, s.b});
                }
                spans.swap(next);
            }
            if (!cut) {
                out.push_back(box);
                continue;
            }
            ++counts.cut;
            char tag = 'a';
            for (const Span& s : spans) {
                if (s.b - s.a < 0.3) continue;
                MassingBox piece = box;
                const double mid = 0.5 * (s.a + s.b);
                piece.cx = box.cx + ux * mid;
                piece.cy = box.cy + uy * mid;
                piece.sx = s.b - s.a;
                piece.id = box.id + tag;
                tag = tag == 'z' ? 'z' : static_cast<char>(tag + 1);
                out.push_back(std::move(piece));
            }
            continue;
        }
        if (box.kind == "post") {
            bool inGap = false;
            for (const SeaLadder& l : ladders) {
                const double tx = l.gapX1 - l.gapX0, ty = l.gapY1 - l.gapY0, len = std::sqrt(tx * tx + ty * ty);
                if (!(len > 1e-9)) continue;
                const double px = box.cx - 0.5 * (l.gapX0 + l.gapX1), py = box.cy - 0.5 * (l.gapY0 + l.gapY1);
                const double s = (px * tx + py * ty) / len, w = (-px * ty + py * tx) / len;
                if (std::fabs(s) <= 0.5 * len + clear && std::fabs(w) <= reach + 1.0) {
                    inGap = true;
                    break;
                }
            }
            if (inGap) {
                ++counts.dropped;
                continue;
            }
        }
        out.push_back(box);
    }
    return out;
}

namespace {

std::string trimmed(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

bool number(const std::string& s, double& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end == s.c_str() + s.size() && std::isfinite(v);
}

} // namespace

bool importSeaAccess(const std::string& text, SeaAccessTable& out, std::string* error) {
    out = SeaAccessTable{};
    // A UTF-8 byte order mark (an editor's) is skipped.
    std::size_t pos = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0, lineNo = 0;
    std::vector<std::string> f;
    std::vector<double> v;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        line = trimmed(line);
        if (line.empty()) continue;
        f.clear();
        std::size_t s = 0;
        for (;;) {
            const std::size_t c = line.find(',', s);
            f.push_back(trimmed(line.substr(s, c == std::string::npos ? std::string::npos : c - s)));
            if (c == std::string::npos) break;
            s = c + 1;
        }
        const std::string& kind = f[0];
        const std::size_t first = kind == "face" ? 2 : 1;
        const std::size_t want = kind == "face"                                             ? 9
                                 : (kind == "avoid" || kind == "water" || kind == "solid") ? 5
                                 : kind == "exit"                                           ? 3
                                 : (kind == "bed" || kind == "sea")                         ? 2
                                                                                            : 0;
        auto fail = [&](const std::string& why) {
            if (error) *error = "line " + std::to_string(lineNo) + ": " + why;
            return false;
        };
        if (want == 0) return fail("unknown row kind '" + kind + "'");
        if (f.size() != want) return fail(kind + " wants " + std::to_string(want - 1) + " fields");
        v.clear();
        for (std::size_t i = first; i < f.size(); ++i) {
            double x = 0.0;
            if (!number(f[i], x)) return fail("'" + f[i] + "' is not a number");
            v.push_back(x);
        }
        if (kind == "face") {
            const double len = std::sqrt(v[4] * v[4] + v[5] * v[5]);
            if (!(len > 1e-9)) return fail("the face " + f[1] + " has no outward direction");
            out.faces.push_back(SeaFace{f[1], v[0], v[1], v[2], v[3], v[4] / len, v[5] / len, v[6]});
        } else if (want == 5) {
            if (v[2] < v[0] || v[3] < v[1]) return fail("the rect's max is under its min");
            const SeaRect r{v[0], v[1], v[2], v[3]};
            (kind == "avoid" ? out.avoid : kind == "water" ? out.water : out.solid).push_back(r);
        } else if (kind == "exit") {
            out.exits.push_back(SeaPoint{v[0], v[1]});
        } else if (kind == "bed") {
            out.bedZ = v[0];
        } else {
            out.seaZ = v[0];
        }
    }
    return true;
}

} // namespace ftcore
