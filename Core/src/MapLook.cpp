#include "ftcore/MapLook.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace ftcore {

namespace {

constexpr double kFar = 1e20; // "no site" in the distance transform (finite: no inf - inf)
constexpr std::uint32_t kEdgeMarginRows = 4; // the water grid's north and south margin rows the sea may carry over (16 m)

// Felzenszwalb and Huttenlocher's squared distance transform of one line: d[q] = min over p (f[p] + (q - p)^2).
// v and z are scratch (n and n + 1).
void edtLine(const double* f, int n, double* d, int* v, double* z) {
    int k = 0;
    v[0] = 0;
    z[0] = -std::numeric_limits<double>::infinity();
    z[1] = std::numeric_limits<double>::infinity();
    for (int q = 1; q < n; ++q) {
        const double fq = f[q] + static_cast<double>(q) * q;
        double s = (fq - (f[v[k]] + static_cast<double>(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
        while (k > 0 && s <= z[k]) {
            --k;
            s = (fq - (f[v[k]] + static_cast<double>(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = std::numeric_limits<double>::infinity();
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q) ++k;
        const double dq = static_cast<double>(q - v[k]);
        d[q] = dq * dq + f[v[k]];
    }
}

// The squared distance (cells) from each cell of a w x h grid to the nearest cell where site is set.
void edt(const std::vector<std::uint8_t>& site, std::uint32_t w, std::uint32_t h, std::vector<double>& out) {
    const std::size_t n = static_cast<std::size_t>(w) * h;
    out.assign(n, kFar);
    for (std::size_t i = 0; i < n; ++i)
        if (site[i]) out[i] = 0.0;
    const std::size_t side = std::max(w, h);
    std::vector<double> f(side), d(side), z(side + 1);
    std::vector<int> v(side);
    for (std::uint32_t x = 0; x < w; ++x) { // columns
        for (std::uint32_t y = 0; y < h; ++y) f[y] = out[static_cast<std::size_t>(y) * w + x];
        edtLine(f.data(), static_cast<int>(h), d.data(), v.data(), z.data());
        for (std::uint32_t y = 0; y < h; ++y) out[static_cast<std::size_t>(y) * w + x] = d[y];
    }
    for (std::uint32_t y = 0; y < h; ++y) { // rows
        double* row = out.data() + static_cast<std::size_t>(y) * w;
        std::copy(row, row + w, f.begin());
        edtLine(f.data(), static_cast<int>(w), d.data(), v.data(), z.data());
        std::copy(d.begin(), d.begin() + w, row);
    }
}

// The signed distance (m) to the edge of `inside` at each cell centre: + inside, - outside, 0 on the cell edge between
// an inside and an outside cell; then softened once ([1 2 1] each way) so the contours run smooth, not in steps.
void signedField(const std::vector<std::uint8_t>& inside, std::uint32_t w, std::uint32_t h, float cell, std::vector<float>& out) {
    const std::size_t n = static_cast<std::size_t>(w) * h;
    std::vector<std::uint8_t> outside(n);
    for (std::size_t i = 0; i < n; ++i) outside[i] = inside[i] ? 0 : 1;
    std::vector<double> toOut, toIn;
    edt(outside, w, h, toOut);
    edt(inside, w, h, toIn);
    std::vector<float> raw(n);
    const float cap = 1e6f;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = inside[i] ? std::sqrt(toOut[i]) : std::sqrt(toIn[i]);
        const float m = std::min(static_cast<float>((d - 0.5) * cell), cap);
        raw[i] = inside[i] ? m : -m;
    }
    out.assign(n, 0.f);
    std::vector<float> tmp(n);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            const float l = raw[x > 0 ? i - 1 : i], r = raw[x + 1 < w ? i + 1 : i];
            tmp[i] = 0.25f * l + 0.5f * raw[i] + 0.25f * r;
        }
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            const float u = tmp[y > 0 ? i - w : i], dn = tmp[y + 1 < h ? i + w : i];
            out[i] = 0.25f * u + 0.5f * tmp[i] + 0.25f * dn;
        }
}

// Marching squares over a w x h field sampled at cell centres (origin x0, y0; cell m): the contour at `level` as
// segments (inside: value > level; a saddle follows its centre's average).
template <class Emit>
void contour(const std::vector<float>& f, std::uint32_t w, std::uint32_t h, float x0, float y0, float cell, float level, Emit&& emit) {
    if (w < 2 || h < 2) return;
    for (std::uint32_t j = 0; j + 1 < h; ++j) {
        for (std::uint32_t i = 0; i + 1 < w; ++i) {
            const std::size_t ia = static_cast<std::size_t>(j) * w + i;
            const float va = f[ia], vb = f[ia + 1], vc = f[ia + w + 1], vd = f[ia + w];
            const int c = (va > level ? 1 : 0) | (vb > level ? 2 : 0) | (vc > level ? 4 : 0) | (vd > level ? 8 : 0);
            if (c == 0 || c == 15) continue;
            const float px = x0 + (static_cast<float>(i) + 0.5f) * cell, py = y0 + (static_cast<float>(j) + 0.5f) * cell;
            const Vec2 pa{px, py}, pb{px + cell, py}, pc{px + cell, py + cell}, pd{px, py + cell};
            auto cut = [level](Vec2 p, float vp, Vec2 q, float vq) {
                const float t = (vq != vp) ? std::clamp((level - vp) / (vq - vp), 0.f, 1.f) : 0.5f;
                return Vec2{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t};
            };
            const Vec2 e0 = cut(pa, va, pb, vb), e1 = cut(pb, vb, pc, vc), e2 = cut(pc, vc, pd, vd), e3 = cut(pd, vd, pa, va);
            const bool centreIn = 0.25f * (va + vb + vc + vd) > level;
            switch (c) {
            case 1: case 14: emit(e3, e0); break;
            case 2: case 13: emit(e0, e1); break;
            case 3: case 12: emit(e3, e1); break;
            case 4: case 11: emit(e1, e2); break;
            case 6: case 9: emit(e0, e2); break;
            case 7: case 8: emit(e3, e2); break;
            case 5: // a and c inside
                if (centreIn) {
                    emit(e0, e1);
                    emit(e2, e3);
                } else {
                    emit(e3, e0);
                    emit(e1, e2);
                }
                break;
            case 10: // b and d inside
                if (centreIn) {
                    emit(e3, e0);
                    emit(e1, e2);
                } else {
                    emit(e0, e1);
                    emit(e2, e3);
                }
                break;
            default: break;
            }
        }
    }
}

// The 4-connected pieces of `on` cells: a label per cell (-1 off), the count per piece.
void pieces(const std::vector<std::uint8_t>& on, std::uint32_t w, std::uint32_t h, std::vector<std::int32_t>& label,
            std::vector<std::uint32_t>& size) {
    const std::size_t n = static_cast<std::size_t>(w) * h;
    label.assign(n, -1);
    size.clear();
    std::vector<std::uint32_t> stack;
    for (std::size_t s = 0; s < n; ++s) {
        if (!on[s] || label[s] >= 0) continue;
        const std::int32_t id = static_cast<std::int32_t>(size.size());
        std::uint32_t count = 0;
        stack.push_back(static_cast<std::uint32_t>(s));
        label[s] = id;
        while (!stack.empty()) {
            const std::uint32_t c = stack.back();
            stack.pop_back();
            ++count;
            const std::uint32_t x = c % w, y = c / w;
            const std::uint32_t nb[4] = {x > 0 ? c - 1 : c, x + 1 < w ? c + 1 : c, y > 0 ? c - w : c, y + 1 < h ? c + w : c};
            for (std::uint32_t q : nb)
                if (q != c && on[q] && label[q] < 0) {
                    label[q] = id;
                    stack.push_back(q);
                }
        }
        size.push_back(count);
    }
}

void push(MapLook& out, MapStrokeKind kind, Vec2 a, Vec2 b, std::uint8_t level) {
    out.strokes[static_cast<std::size_t>(kind)].push_back(MapStroke{a, b, level});
}

// A line a-b as strokes no longer than kMapStrokeMaxM.
void pushLine(MapLook& out, MapStrokeKind kind, Vec2 a, Vec2 b, std::uint8_t level) {
    const float len = std::hypot(b.x - a.x, b.y - a.y);
    const int parts = std::max(1, static_cast<int>(std::ceil(len / kMapStrokeMaxM)));
    for (int p = 0; p < parts; ++p) {
        const float t0 = static_cast<float>(p) / static_cast<float>(parts), t1 = static_cast<float>(p + 1) / static_cast<float>(parts);
        push(out, kind, Vec2{a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0}, Vec2{a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1}, level);
    }
}

// The part [t0, t1] of segment p-q inside rect r shrunk by `inset` (Liang-Barsky in the rect's own axes); false: none.
bool insideRect(const MapRect& r, Vec2 p, Vec2 q, float inset, float& t0, float& t1) {
    const float c = std::cos(r.rot), s = std::sin(r.rot);
    const float pu = (p.x - r.cx) * c + (p.y - r.cy) * s, pv = -(p.x - r.cx) * s + (p.y - r.cy) * c;
    const float qu = (q.x - r.cx) * c + (q.y - r.cy) * s, qv = -(q.x - r.cx) * s + (q.y - r.cy) * c;
    const float hx = r.hx - inset, hy = r.hy - inset;
    if (hx <= 0.f || hy <= 0.f) return false;
    t0 = 0.f;
    t1 = 1.f;
    const float du = qu - pu, dv = qv - pv;
    const float pp[4] = {-du, du, -dv, dv}, qq[4] = {pu + hx, hx - pu, pv + hy, hy - pv};
    for (int i = 0; i < 4; ++i) {
        if (pp[i] == 0.f) {
            if (qq[i] < 0.f) return false;
            continue;
        }
        const float t = qq[i] / pp[i];
        if (pp[i] < 0.f) {
            if (t > t1) return false;
            t0 = std::max(t0, t);
        } else {
            if (t < t0) return false;
            t1 = std::min(t1, t);
        }
    }
    return t1 > t0;
}

float segDist(Vec2 p, Vec2 a, Vec2 b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len2 = dx * dx + dy * dy;
    const float t = len2 > 0.f ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0.f, 1.f) : 0.f;
    return std::hypot(a.x + dx * t - p.x, a.y + dy * t - p.y);
}

int posMod(long long k, int n) { return static_cast<int>(((k % n) + n) % n); }

// The canals' edges: each water rectangle's long sides (all four when it is nearly square), less the parts inside
// another water rectangle (where two canals meet, the water runs on).
void canalSides(const MiniMapModel& m, MapLook& out) {
    const std::size_t n = m.waterRects.size();
    std::vector<float> bx0(n), by0(n), bx1(n), by1(n);
    for (std::size_t i = 0; i < n; ++i) {
        const MapRect& r = m.waterRects[i];
        const float ex = std::abs(r.hx * std::cos(r.rot)) + std::abs(r.hy * std::sin(r.rot));
        const float ey = std::abs(r.hx * std::sin(r.rot)) + std::abs(r.hy * std::cos(r.rot));
        bx0[i] = r.cx - ex;
        bx1[i] = r.cx + ex;
        by0[i] = r.cy - ey;
        by1[i] = r.cy + ey;
    }
    std::vector<std::pair<float, float>> cut;
    for (std::size_t i = 0; i < n; ++i) {
        const MapRect& r = m.waterRects[i];
        if (!(r.hx > 0.f && r.hy > 0.f)) continue;
        const float c = std::cos(r.rot), s = std::sin(r.rot);
        const Vec2 u{c * r.hx, s * r.hx}, v{-s * r.hy, c * r.hy};
        const Vec2 k[4] = {{r.cx - u.x - v.x, r.cy - u.y - v.y}, {r.cx + u.x - v.x, r.cy + u.y - v.y},
                           {r.cx + u.x + v.x, r.cy + u.y + v.y}, {r.cx - u.x + v.x, r.cy - u.y + v.y}};
        const float lo = std::min(r.hx, r.hy), hi = std::max(r.hx, r.hy);
        const bool all = hi < 1.5f * lo;
        // Sides 0-1 and 2-3 run along the rect's x axis; 1-2 and 3-0 along its y axis.
        const bool alongX = r.hx >= r.hy;
        for (int e = 0; e < 4; ++e) {
            const bool sideAlongX = (e % 2) == 0;
            if (!all && sideAlongX != alongX) continue;
            const Vec2 p = k[e], q = k[(e + 1) % 4];
            cut.clear();
            const float sx0 = std::min(p.x, q.x), sx1 = std::max(p.x, q.x), sy0 = std::min(p.y, q.y), sy1 = std::max(p.y, q.y);
            for (std::size_t o = 0; o < n; ++o) {
                if (o == i || bx1[o] < sx0 || bx0[o] > sx1 || by1[o] < sy0 || by0[o] > sy1) continue;
                float t0 = 0.f, t1 = 0.f;
                if (insideRect(m.waterRects[o], p, q, 0.05f, t0, t1)) cut.emplace_back(t0, t1);
            }
            std::sort(cut.begin(), cut.end());
            float at = 0.f;
            auto emitPart = [&](float a, float b) {
                if (b - a <= 1e-4f) return;
                pushLine(out, MapStrokeKind::Canal, Vec2{p.x + (q.x - p.x) * a, p.y + (q.y - p.y) * a},
                         Vec2{p.x + (q.x - p.x) * b, p.y + (q.y - p.y) * b}, 0);
            };
            for (const auto& span : cut) {
                if (span.first > at) emitPart(at, span.first);
                at = std::max(at, span.second);
            }
            emitPart(at, 1.f);
        }
    }
}

} // namespace

void mapBuildLook(const MiniMapModel& m, const MapNames* names, MapLook& out) {
    out = MapLook{};
    out.seaSide = mapSeaSide(m);
    out.landX0 = m.minX;
    out.landY0 = m.minY;
    out.landX1 = m.maxX;
    out.landY1 = m.maxY;
    const std::uint32_t w = m.waterW, h = m.waterH;
    const std::size_t cells = static_cast<std::size_t>(w) * h;
    const bool grid = m.waterCell > 0.f && w > 0 && h > 0 && m.water.size() >= cells;
    std::vector<float> field;
    if (grid) {
        out.cell = m.waterCell;
        out.x0 = m.minX;
        out.y0 = m.minY;
        out.w = w;
        out.h = h;
        // The sea: the bodies of water that reach the sea-side edge, else the largest.
        std::vector<std::uint8_t> wet(cells);
        for (std::size_t i = 0; i < cells; ++i) wet[i] = m.water[i] ? 1 : 0;
        std::vector<std::int32_t> label;
        std::vector<std::uint32_t> size;
        pieces(wet, w, h, label, size);
        std::vector<std::uint8_t> isSea(size.size(), 0);
        bool any = false;
        if (out.seaSide != 0) {
            const std::uint32_t x = out.seaSide > 0 ? w - 1 : 0;
            for (std::uint32_t y = 0; y < h; ++y) {
                const std::int32_t l = label[static_cast<std::size_t>(y) * w + x];
                if (l >= 0) {
                    isSea[static_cast<std::size_t>(l)] = 1;
                    any = true;
                }
            }
        }
        if (!any && !size.empty())
            isSea[static_cast<std::size_t>(std::max_element(size.begin(), size.end()) - size.begin())] = 1;
        out.sea.assign(cells, 0);
        for (std::size_t i = 0; i < cells; ++i)
            if (label[i] >= 0 && isSea[static_cast<std::size_t>(label[i])]) out.sea[i] = 1;
        // The bake's margin past the height field: up to kEdgeMarginRows rows at the north or south edge, before the first
        // row that holds sea and dry wherever it is sea, are not a shore. They take that row's sea, so the sea runs on to the
        // model's edge (no coast drawn along it) and on past it (mapIsSea). A deeper dry band is a real shore.
        for (int side = 0; side < 2; ++side) {
            const auto rowAt = [&](std::uint32_t k) { return side == 0 ? k : h - 1 - k; };
            std::uint32_t edgeRows = 0;
            for (; edgeRows < h; ++edgeRows) {
                bool seaRow = false;
                const std::size_t r = static_cast<std::size_t>(rowAt(edgeRows)) * w;
                for (std::uint32_t x = 0; x < w && !seaRow; ++x) seaRow = out.sea[r + x] != 0;
                if (seaRow) break;
            }
            if (edgeRows == 0 || edgeRows > kEdgeMarginRows || edgeRows >= h) continue;
            const std::size_t from = static_cast<std::size_t>(rowAt(edgeRows)) * w;
            bool dry = true;
            for (std::uint32_t k = 0; k < edgeRows && dry; ++k)
                for (std::uint32_t x = 0; x < w && dry; ++x)
                    if (out.sea[from + x] && wet[static_cast<std::size_t>(rowAt(k)) * w + x]) dry = false;
            if (!dry) continue;
            for (std::uint32_t k = 0; k < edgeRows; ++k)
                for (std::uint32_t x = 0; x < w; ++x) out.sea[static_cast<std::size_t>(rowAt(k)) * w + x] = out.sea[from + x];
        }
        // Its summed area, and the land's bounds.
        out.seaSum.assign(static_cast<std::size_t>(w + 1) * (h + 1), 0);
        std::uint32_t lx0 = w, ly0 = h, lx1 = 0, ly1 = 0;
        bool land = false;
        for (std::uint32_t y = 0; y < h; ++y) {
            std::uint32_t row = 0;
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::uint8_t s = out.sea[static_cast<std::size_t>(y) * w + x];
                row += s;
                out.seaSum[static_cast<std::size_t>(y + 1) * (w + 1) + x + 1] = out.seaSum[static_cast<std::size_t>(y) * (w + 1) + x + 1] + row;
                if (!s) {
                    land = true;
                    lx0 = std::min(lx0, x);
                    ly0 = std::min(ly0, y);
                    lx1 = std::max(lx1, x);
                    ly1 = std::max(ly1, y);
                }
            }
        }
        if (land) {
            out.landX0 = std::max(m.minX, m.minX + static_cast<float>(lx0) * out.cell);
            out.landY0 = std::max(m.minY, m.minY + static_cast<float>(ly0) * out.cell);
            out.landX1 = std::min(m.maxX, m.minX + static_cast<float>(lx1 + 1) * out.cell);
            out.landY1 = std::min(m.maxY, m.minY + static_cast<float>(ly1 + 1) * out.cell);
        }
        // The coast and the waves: contours of the sea's signed distance.
        bool hasSea = false;
        for (std::uint8_t s : out.sea) hasSea = hasSea || s;
        if (hasSea && land) {
            signedField(out.sea, w, h, out.cell, field);
            const MapStrokeKind kinds[4] = {MapStrokeKind::Coast, MapStrokeKind::Wave12, MapStrokeKind::Wave24, MapStrokeKind::Wave40};
            const float levels[4] = {0.f, kMapWaveM[0], kMapWaveM[1], kMapWaveM[2]};
            for (int k = 0; k < 4; ++k)
                contour(field, w, h, m.minX, m.minY, out.cell, levels[k], [&](Vec2 a, Vec2 b) { push(out, kinds[k], a, b, 0); });
            // Where the sea's name can go: the deepest cell per 8 x 8.
            for (std::uint32_t by = 0; by < h; by += 8)
                for (std::uint32_t bx = 0; bx < w; bx += 8) {
                    float best = -1.f;
                    Vec2 at{};
                    for (std::uint32_t y = by; y < std::min(h, by + 8); ++y)
                        for (std::uint32_t x = bx; x < std::min(w, bx + 8); ++x) {
                            const std::size_t i = static_cast<std::size_t>(y) * w + x;
                            if (!out.sea[i] || field[i] <= best) continue;
                            best = field[i];
                            at = Vec2{m.minX + (static_cast<float>(x) + 0.5f) * out.cell, m.minY + (static_cast<float>(y) + 0.5f) * out.cell};
                        }
                    if (best > 0.f) out.seaSpots.push_back(MapLook::SeaSpot{at, best});
                }
        }
        // The coast past the model's edges (mapIsSea there): the contours stop at the edge cells' middles.
        const auto seaAt = [&](std::uint32_t x, std::uint32_t y) { return out.sea[static_cast<std::size_t>(y) * w + x] != 0; };
        const bool offSea[2] = {out.seaSide < 0, out.seaSide > 0}; // west, east of the model
        const auto colX = [&](std::uint32_t x) {
            return x == 0 ? m.minX : x >= w ? m.maxX : std::min(m.maxX, m.minX + static_cast<float>(x) * out.cell);
        };
        const auto rowY = [&](std::uint32_t y) {
            return y == 0 ? m.minY : y >= h ? m.maxY : std::min(m.maxY, m.minY + static_cast<float>(y) * out.cell);
        };
        const auto edge = [&](Vec2 a, Vec2 b) { out.edgeCoast.push_back(MapStroke{a, b, 0}); };
        // North and south: the edge row runs on; a line out from the row's middle at each change along it.
        for (int side = 0; side < 2; ++side) {
            const std::uint32_t y = side == 0 ? 0 : h - 1;
            const float mid = m.minY + (static_cast<float>(y) + 0.5f) * out.cell;
            const float edgeY = side == 0 ? m.minY : m.maxY, outY = side == 0 ? m.minY - kMapEdgeRunM : m.maxY + kMapEdgeRunM;
            for (std::uint32_t x = 0; x <= w; ++x) {
                const bool left = x == 0 ? offSea[0] : seaAt(x - 1, y), right = x == w ? offSea[1] : seaAt(x, y);
                if (left == right) continue;
                const bool corner = x == 0 || x == w; // the edge column's own line runs to the corner
                edge(Vec2{colX(x), corner ? edgeY : std::clamp(mid, m.minY, m.maxY)}, Vec2{colX(x), outY});
            }
        }
        // West and east: the edge column against what lies off that side, joined to the contours where a run ends inside.
        for (int side = 0; side < 2; ++side) {
            const std::uint32_t col = side == 0 ? 0 : w - 1;
            const float edgeX = side == 0 ? m.minX : m.maxX;
            const float cx = std::clamp(m.minX + (static_cast<float>(col) + 0.5f) * out.cell, m.minX, m.maxX);
            for (std::uint32_t y = 0; y < h;) {
                if (seaAt(col, y) == offSea[side]) {
                    ++y;
                    continue;
                }
                std::uint32_t end = y;
                while (end < h && seaAt(col, end) != offSea[side]) ++end;
                const float a = rowY(y), b = rowY(end);
                edge(Vec2{edgeX, a}, Vec2{edgeX, b});
                if (y > 0) edge(Vec2{cx, a}, Vec2{edgeX, a});
                if (end < h) edge(Vec2{cx, b}, Vec2{edgeX, b});
                y = end;
            }
        }
        // Water the sea never reaches: its edge, as a canal's.
        std::vector<std::uint8_t> basin(cells, 0);
        bool basins = false;
        for (std::size_t i = 0; i < cells; ++i)
            if (wet[i] && !out.sea[i]) basin[i] = 1, basins = true;
        if (basins) {
            signedField(basin, w, h, out.cell, field);
            contour(field, w, h, m.minX, m.minY, out.cell, 0.f, [&](Vec2 a, Vec2 b) { push(out, MapStrokeKind::Canal, a, b, 0); });
        }
    } else if (out.seaSide != 0 && m.maxY > m.minY) {
        // No grid: the model is all ground, the sea off its sea side all the way.
        const float x = out.seaSide > 0 ? m.maxX : m.minX;
        out.edgeCoast.push_back(MapStroke{Vec2{x, m.minY - kMapEdgeRunM}, Vec2{x, m.maxY + kMapEdgeRunM}, 0});
    }
    canalSides(m, out);

    // Blocks: the empty ground between buildings, roads and water.
    const float bc = kMapBlockCellM;
    const std::uint32_t bw = m.maxX > m.minX ? static_cast<std::uint32_t>(std::ceil((m.maxX - m.minX) / bc)) : 0;
    const std::uint32_t bh = m.maxY > m.minY ? static_cast<std::uint32_t>(std::ceil((m.maxY - m.minY) / bc)) : 0;
    if (bw > 2 && bh > 2) {
        const std::size_t bn = static_cast<std::size_t>(bw) * bh;
        std::vector<std::uint8_t> taken(bn, 0);
        auto cellRange = [&](float lo, float hi, float origin, std::uint32_t count, std::uint32_t& a, std::uint32_t& b) {
            const float fa = std::floor((lo - origin) / bc), fb = std::floor((hi - origin) / bc);
            if (fb < 0.f || fa > static_cast<float>(count - 1)) return false;
            a = static_cast<std::uint32_t>(std::max(fa, 0.f));
            b = static_cast<std::uint32_t>(std::min(fb, static_cast<float>(count - 1)));
            return true;
        };
        auto centre = [&](std::uint32_t x, std::uint32_t y) {
            return Vec2{m.minX + (static_cast<float>(x) + 0.5f) * bc, m.minY + (static_cast<float>(y) + 0.5f) * bc};
        };
        auto fillRect = [&](const MapRect& r, float grow) {
            const float c = std::cos(r.rot), s = std::sin(r.rot);
            const float hx = r.hx + grow, hy = r.hy + grow;
            const float ex = std::abs(hx * c) + std::abs(hy * s), ey = std::abs(hx * s) + std::abs(hy * c);
            std::uint32_t xa = 0, xb = 0, ya = 0, yb = 0;
            if (!cellRange(r.cx - ex, r.cx + ex, m.minX, bw, xa, xb) || !cellRange(r.cy - ey, r.cy + ey, m.minY, bh, ya, yb)) return;
            for (std::uint32_t y = ya; y <= yb; ++y)
                for (std::uint32_t x = xa; x <= xb; ++x) {
                    const Vec2 p = centre(x, y);
                    const float u = (p.x - r.cx) * c + (p.y - r.cy) * s, v = -(p.x - r.cx) * s + (p.y - r.cy) * c;
                    if (std::abs(u) <= hx && std::abs(v) <= hy) taken[static_cast<std::size_t>(y) * bw + x] = 1;
                }
        };
        for (const MapRect& b : m.buildings) fillRect(b, 0.5f * bc);
        for (const MapRect& r : m.waterRects) fillRect(r, 0.f);
        for (const MapRoad& road : m.roads) {
            if (road.count == 0 || static_cast<std::size_t>(road.first) + road.count > m.points.size()) continue;
            const float width = road.width > 0.f ? road.width : static_cast<float>(kMapRoadDefaultWidth[static_cast<int>(road.cls)]);
            const float rr = 0.5f * width + 0.75f * bc;
            for (std::uint32_t k = 0; k < road.count; ++k) {
                const Vec2 a = m.points[road.first + k], b = m.points[road.first + std::min(k + 1, road.count - 1)];
                if (k + 1 >= road.count && road.count > 1) break;
                std::uint32_t xa = 0, xb = 0, ya = 0, yb = 0;
                if (!cellRange(std::min(a.x, b.x) - rr, std::max(a.x, b.x) + rr, m.minX, bw, xa, xb) ||
                    !cellRange(std::min(a.y, b.y) - rr, std::max(a.y, b.y) + rr, m.minY, bh, ya, yb))
                    continue;
                for (std::uint32_t y = ya; y <= yb; ++y)
                    for (std::uint32_t x = xa; x <= xb; ++x)
                        if (segDist(centre(x, y), a, b) <= rr) taken[static_cast<std::size_t>(y) * bw + x] = 1;
            }
        }
        if (grid)
            for (std::uint32_t y = 0; y < bh; ++y)
                for (std::uint32_t x = 0; x < bw; ++x) {
                    const Vec2 p = centre(x, y);
                    const long long gx = static_cast<long long>(std::floor((p.x - m.minX) / out.cell));
                    const long long gy = static_cast<long long>(std::floor((p.y - m.minY) / out.cell));
                    if (gx < 0 || gy < 0 || gx >= static_cast<long long>(w) || gy >= static_cast<long long>(h)) continue;
                    if (m.water[static_cast<std::size_t>(gy) * w + static_cast<std::size_t>(gx)]) taken[static_cast<std::size_t>(y) * bw + x] = 1;
                }
        std::vector<std::uint8_t> empty(bn);
        for (std::size_t i = 0; i < bn; ++i) empty[i] = taken[i] ? 0 : 1;
        taken.clear();
        taken.shrink_to_fit();
        std::vector<std::int32_t> label;
        std::vector<std::uint32_t> size;
        pieces(empty, bw, bh, label, size);
        std::vector<std::uint8_t> edge(size.size(), 0);
        for (std::uint32_t x = 0; x < bw; ++x) {
            const std::int32_t t = label[x], b = label[static_cast<std::size_t>(bh - 1) * bw + x];
            if (t >= 0) edge[static_cast<std::size_t>(t)] = 1;
            if (b >= 0) edge[static_cast<std::size_t>(b)] = 1;
        }
        for (std::uint32_t y = 0; y < bh; ++y) {
            const std::int32_t l = label[static_cast<std::size_t>(y) * bw], r = label[static_cast<std::size_t>(y) * bw + bw - 1];
            if (l >= 0) edge[static_cast<std::size_t>(l)] = 1;
            if (r >= 0) edge[static_cast<std::size_t>(r)] = 1;
        }
        // Ground that opens onto the sea (a quay, a shore) is open too, never a block: a cell next to a sea cell.
        if (grid)
            for (std::uint32_t y = 0; y < bh; ++y)
                for (std::uint32_t x = 0; x < bw; ++x) {
                    const std::int32_t l = label[static_cast<std::size_t>(y) * bw + x];
                    if (l < 0 || edge[static_cast<std::size_t>(l)]) continue;
                    const Vec2 p = centre(x, y);
                    for (int d = 0; d < 4; ++d) {
                        const float qx = p.x + (d == 0 ? bc : d == 1 ? -bc : 0.f), qy = p.y + (d == 2 ? bc : d == 3 ? -bc : 0.f);
                        const long long gx = static_cast<long long>(std::floor((qx - m.minX) / out.cell));
                        const long long gy = static_cast<long long>(std::floor((qy - m.minY) / out.cell));
                        if (gx < 0 || gy < 0 || gx >= static_cast<long long>(w) || gy >= static_cast<long long>(h)) continue;
                        if (out.sea[static_cast<std::size_t>(gy) * w + static_cast<std::size_t>(gx)]) {
                            edge[static_cast<std::size_t>(l)] = 1;
                            break;
                        }
                    }
                }
        // 0: not a block (sliver or open ground), 1: yard, 2: park.
        std::vector<std::uint8_t> kind(size.size(), 0);
        const float cellM2 = bc * bc;
        for (std::size_t p = 0; p < size.size(); ++p) {
            const float area = static_cast<float>(size[p]) * cellM2;
            if (area < kMapBlockMinM2) {
                ++out.slivers;
            } else if (edge[p] || area > kMapBlockMaxM2) {
                ++out.open;
            } else {
                kind[p] = 1;
            }
        }
        if (names) {
            for (const MapNames::Spot& spot : names->places) {
                if (!spot.green) continue;
                const long long cx = static_cast<long long>(std::floor((spot.at.x - m.minX) / bc));
                const long long cy = static_cast<long long>(std::floor((spot.at.y - m.minY) / bc));
                // The block it stands in; on a building or a road, the nearest block within a few cells.
                std::int32_t hit = -1;
                for (int r = 0; r <= 4 && hit < 0; ++r)
                    for (long long y = cy - r; y <= cy + r && hit < 0; ++y)
                        for (long long x = cx - r; x <= cx + r && hit < 0; ++x) {
                            if (x < 0 || y < 0 || x >= static_cast<long long>(bw) || y >= static_cast<long long>(bh)) continue;
                            const std::int32_t l = label[static_cast<std::size_t>(y) * bw + static_cast<std::size_t>(x)];
                            if (l >= 0 && kind[static_cast<std::size_t>(l)] != 0) hit = l;
                        }
                if (hit >= 0) kind[static_cast<std::size_t>(hit)] = 2;
            }
        }
        for (std::uint8_t k : kind) {
            if (k == 1) ++out.yards;
            if (k == 2) ++out.parks;
        }
        // Parks: their cells as rectangles (row runs merged down), hatched along x + y = k * step.
        struct Open {
            std::uint32_t x0, x1, y0; // cells [x0, x1), from row y0
        };
        std::vector<Open> open, next;
        auto hatch = [&](const Open& o, std::uint32_t yEnd) {
            const float rx0 = m.minX + static_cast<float>(o.x0) * bc, rx1 = m.minX + static_cast<float>(o.x1) * bc;
            const float ry0 = m.minY + static_cast<float>(o.y0) * bc, ry1 = m.minY + static_cast<float>(yEnd) * bc;
            const long long k0 = static_cast<long long>(std::ceil((rx0 + ry0) / kMapHatchStepM));
            const long long k1 = static_cast<long long>(std::floor((rx1 + ry1) / kMapHatchStepM));
            for (long long k = k0; k <= k1; ++k) {
                const float cc = static_cast<float>(k) * kMapHatchStepM;
                const float xa = std::max(rx0, cc - ry1), xb = std::min(rx1, cc - ry0);
                if (xb - xa <= 1e-3f) continue;
                const std::uint8_t level = posMod(k, 4) == 0 ? 0 : posMod(k, 2) == 0 ? 1 : 2;
                pushLine(out, MapStrokeKind::ParkHatch, Vec2{xa, cc - xa}, Vec2{xb, cc - xb}, level);
            }
        };
        for (std::uint32_t y = 0; y <= bh; ++y) {
            next.clear();
            std::size_t oi = 0;
            std::uint32_t x = 0;
            while (y < bh && x < bw) {
                auto park = [&](std::uint32_t xx) {
                    const std::int32_t l = label[static_cast<std::size_t>(y) * bw + xx];
                    return l >= 0 && kind[static_cast<std::size_t>(l)] == 2;
                };
                if (!park(x)) {
                    ++x;
                    continue;
                }
                const std::uint32_t xa = x;
                while (x < bw && park(x)) ++x;
                // The open rects that end before this run, or don't match it, close.
                while (oi < open.size() && open[oi].x0 < xa) hatch(open[oi++], y);
                if (oi < open.size() && open[oi].x0 == xa && open[oi].x1 == x) {
                    next.push_back(open[oi++]);
                } else {
                    next.push_back(Open{xa, x, y});
                }
            }
            while (oi < open.size()) hatch(open[oi++], y);
            open.swap(next);
        }
        // Yards: a dotted line a cell inside each yard's edge.
        std::vector<float> yard(bn, 0.f);
        for (std::uint32_t y = 1; y + 1 < bh; ++y)
            for (std::uint32_t x = 1; x + 1 < bw; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * bw + x;
                auto isYard = [&](std::size_t q) { return label[q] >= 0 && kind[static_cast<std::size_t>(label[q])] == 1; };
                if (isYard(i) && isYard(i - 1) && isYard(i + 1) && isYard(i - bw) && isYard(i + bw)) yard[i] = 1.f;
            }
        std::vector<float> soft(bn, 0.f);
        for (std::uint32_t y = 1; y + 1 < bh; ++y)
            for (std::uint32_t x = 1; x + 1 < bw; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * bw + x;
                float s = 0.f;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) s += yard[static_cast<std::size_t>(static_cast<long long>(i) + dy * static_cast<long long>(bw) + dx)];
                soft[i] = s / 9.f;
            }
        contour(soft, bw, bh, m.minX, m.minY, bc, 0.5f, [&](Vec2 a, Vec2 b) {
            const bool alongX = std::abs(b.x - a.x) >= std::abs(b.y - a.y);
            const float ua = alongX ? a.x : a.y, ub = alongX ? b.x : b.y;
            const float lo = std::min(ua, ub), hi = std::max(ua, ub);
            const long long k0 = static_cast<long long>(std::ceil(lo / kMapYardDotStepM));
            for (long long k = k0; static_cast<float>(k) * kMapYardDotStepM < hi; ++k) {
                const float u = static_cast<float>(k) * kMapYardDotStepM;
                const float t = hi > lo ? (u - ua) / (ub - ua) : 0.f;
                const Vec2 p{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                push(out, MapStrokeKind::YardDot, p, p, posMod(k, 2) == 0 ? 1 : 2);
            }
        });
    }

    // Every kind's strokes by tile.
    out.tileX0 = m.minX;
    out.tileY0 = m.minY;
    out.tilesW = m.maxX > m.minX ? static_cast<std::uint32_t>(std::ceil((m.maxX - m.minX) / kMapStrokeTileM)) + 1 : 1;
    out.tilesH = m.maxY > m.minY ? static_cast<std::uint32_t>(std::ceil((m.maxY - m.minY) / kMapStrokeTileM)) + 1 : 1;
    const std::size_t tiles = static_cast<std::size_t>(out.tilesW) * out.tilesH;
    std::vector<std::uint32_t> tileOf;
    std::vector<MapStroke> sorted;
    for (int k = 0; k < kMapStrokeKinds; ++k) {
        std::vector<MapStroke>& list = out.strokes[static_cast<std::size_t>(k)];
        std::vector<std::uint32_t>& first = out.tileFirst[static_cast<std::size_t>(k)];
        first.assign(tiles + 1, 0);
        tileOf.resize(list.size());
        for (std::size_t i = 0; i < list.size(); ++i) {
            const float mx = 0.5f * (list[i].a.x + list[i].b.x), my = 0.5f * (list[i].a.y + list[i].b.y);
            const float fx = std::clamp(std::floor((mx - out.tileX0) / kMapStrokeTileM), 0.f, static_cast<float>(out.tilesW - 1));
            const float fy = std::clamp(std::floor((my - out.tileY0) / kMapStrokeTileM), 0.f, static_cast<float>(out.tilesH - 1));
            tileOf[i] = static_cast<std::uint32_t>(fy) * out.tilesW + static_cast<std::uint32_t>(fx);
            ++first[tileOf[i] + 1];
        }
        for (std::size_t t = 0; t < tiles; ++t) first[t + 1] += first[t];
        sorted.resize(list.size());
        std::vector<std::uint32_t> at(first.begin(), first.end() - 1);
        for (std::size_t i = 0; i < list.size(); ++i) sorted[at[tileOf[i]]++] = list[i];
        list.swap(sorted);
        list.shrink_to_fit();
    }
}

bool mapIsSea(const MiniMapModel& m, const MapLook& look, Vec2 world) {
    if (world.x > m.maxX) return look.seaSide > 0;
    if (world.x < m.minX) return look.seaSide < 0;
    if (look.w == 0 || look.h == 0 || !(look.cell > 0.f) || look.sea.size() < static_cast<std::size_t>(look.w) * look.h) return false;
    // North and south of the model the edge rows run on: the sea goes on past the edge as the sea, the land as land.
    const float gx = std::floor((world.x - look.x0) / look.cell);
    const float gy = world.y < m.minY ? 0.f : world.y > m.maxY ? static_cast<float>(look.h - 1) : std::floor((world.y - look.y0) / look.cell);
    if (gx < 0.f || gy < 0.f || gx >= static_cast<float>(look.w) || gy >= static_cast<float>(look.h)) return false;
    return look.sea[static_cast<std::size_t>(gy) * look.w + static_cast<std::size_t>(gx)] != 0;
}

namespace {

// The summed sea area (cells) of grid rect [0, gx] x [0, gy] (fractional cells): bilinear in the summed table, which is
// exact for cells of one value each.
double seaBelow(const MapLook& look, double gx, double gy) {
    gx = std::clamp(gx, 0.0, static_cast<double>(look.w));
    gy = std::clamp(gy, 0.0, static_cast<double>(look.h));
    const std::uint32_t i = std::min(static_cast<std::uint32_t>(gx), look.w - 1), j = std::min(static_cast<std::uint32_t>(gy), look.h - 1);
    const double u = gx - i, v = gy - j;
    const std::size_t row = static_cast<std::size_t>(look.w) + 1;
    const double s00 = look.seaSum[j * row + i], s10 = look.seaSum[j * row + i + 1];
    const double s01 = look.seaSum[(j + 1) * row + i], s11 = look.seaSum[(j + 1) * row + i + 1];
    return s00 * (1 - u) * (1 - v) + s10 * u * (1 - v) + s01 * (1 - u) * v + s11 * u * v;
}

} // namespace

float mapLandShare(const MiniMapModel& m, const MapLook& look, const MapOverlayView& v) {
    const double hw = 0.5 * v.screenW * v.mPerPx, hh = 0.5 * v.screenH * v.mPerPx;
    const double fx0 = v.centre.x - hw, fx1 = v.centre.x + hw, fy0 = v.centre.y - hh, fy1 = v.centre.y + hh;
    const double frame = (fx1 - fx0) * (fy1 - fy0);
    if (!(frame > 0.0)) return 1.f;
    // The model's part of the frame: the ground past its edges is not land (nothing is mapped there).
    const double gx0 = std::max<double>(fx0, m.minX), gx1 = std::min<double>(fx1, m.maxX);
    const double gy0 = std::max<double>(fy0, m.minY), gy1 = std::min<double>(fy1, m.maxY);
    if (!(gx1 > gx0) || !(gy1 > gy0)) return 0.f;
    double land = (gx1 - gx0) * (gy1 - gy0);
    if (look.w > 0 && look.h > 0 && look.seaSum.size() == static_cast<std::size_t>(look.w + 1) * (look.h + 1) && look.cell > 0.f) {
        const double c = look.cell;
        const double ax = (gx0 - look.x0) / c, bx = (gx1 - look.x0) / c;
        const double ay = (gy0 - look.y0) / c, by = (gy1 - look.y0) / c;
        const double cellsIn = seaBelow(look, bx, by) - seaBelow(look, ax, by) - seaBelow(look, bx, ay) + seaBelow(look, ax, ay);
        land -= std::max(cellsIn, 0.0) * c * c;
    }
    return static_cast<float>(std::clamp(land / frame, 0.0, 1.0));
}

MapFrame mapOverlayFrame(const MiniMapModel& m, const MapLook& look, Vec2 focus, MapZoom z, float screenW, float screenH, float uiScale) {
    // A screen of no size (under a pixel, or not a number) is framed as 1920 x 1080, as mapOverlayView does; a scale out of
    // reason as 1; a focus not finite as the model's middle. The frame is always finite.
    if (!(screenW >= 1.f && screenW <= kMapFrameMaxScreenPx)) screenW = 1920.f;
    if (!(screenH >= 1.f && screenH <= kMapFrameMaxScreenPx)) screenH = 1080.f;
    if (!(uiScale >= 0.1f && uiScale <= 10.f)) uiScale = 1.f;
    if (!std::isfinite(focus.x) || !std::isfinite(focus.y)) focus = Vec2{0.5f * (m.minX + m.maxX), 0.5f * (m.minY + m.maxY)};
    MapFrame out;
    MapOverlayView v = mapOverlayView(focus, z, screenW, screenH, uiScale);
    float lo[2], hi[2]; // the centre's room, per axis
    Vec2 c{};
    if (z == MapZoom::Far) {
        const float keep = 1.f - 2.f * kMapFarMargin;
        const float lw = std::max(look.landX1 - look.landX0, 0.f), lh = std::max(look.landY1 - look.landY0, 0.f);
        const float fit = std::max(lw / (v.screenW * keep), lh / (v.screenH * keep));
        const float mid = mapOverlayView(focus, MapZoom::Mid, screenW, screenH, uiScale).mPerPx;
        v.mPerPx = std::max(fit, mid * kMapFarOverMid);
        const float hw = 0.5f * v.screenW * v.mPerPx * keep, hh = 0.5f * v.screenH * v.mPerPx * keep;
        auto room = [](float a0, float a1, float half, float& l, float& h) {
            l = a1 - half;
            h = a0 + half;
            if (l > h) l = h = 0.5f * (a0 + a1);
        };
        room(look.landX0, look.landX1, hw, lo[0], hi[0]);
        room(look.landY0, look.landY1, hh, lo[1], hi[1]);
        c = Vec2{std::clamp(focus.x, lo[0], hi[0]), std::clamp(focus.y, lo[1], hi[1])};
    } else {
        const float hw = 0.5f * v.screenW * v.mPerPx, hh = 0.5f * v.screenH * v.mPerPx;
        lo[0] = focus.x - kMapKeepInner * hw;
        hi[0] = focus.x + kMapKeepInner * hw;
        lo[1] = focus.y - kMapKeepInner * hh;
        hi[1] = focus.y + kMapKeepInner * hh;
        const Vec2 held = mapOverlayClampCentre(m, v);
        c = Vec2{std::clamp(held.x, lo[0], hi[0]), std::clamp(held.y, lo[1], hi[1])};
    }
    v.centre = c;
    out.view = v;
    out.land = mapLandShare(m, look, v);
    if (out.land >= kMapLandShareMin) return out;
    // Away from the sea: straight off its side, else from the sea's middle on screen.
    Vec2 d{static_cast<float>(-look.seaSide), 0.f};
    if (look.seaSide == 0) {
        double sx = 0.0, sy = 0.0;
        int n = 0;
        for (int j = 0; j < 18; ++j)
            for (int i = 0; i < 32; ++i) {
                const Vec2 p = mapOverlayFromScreen(v, Vec2{(static_cast<float>(i) + 0.5f) * v.screenW / 32.f, (static_cast<float>(j) + 0.5f) * v.screenH / 18.f});
                if (!mapIsSea(m, look, p)) continue;
                sx += p.x;
                sy += p.y;
                ++n;
            }
        if (n == 0) { // no sea to move away from: what is short is the ground past the model, and no shift helps
            out.reached = false;
            return out;
        }
        const float ax = c.x - static_cast<float>(sx / n), ay = c.y - static_cast<float>(sy / n);
        const float len = std::hypot(ax, ay);
        if (!(len > 1e-3f)) {
            out.reached = false;
            return out;
        }
        d = Vec2{ax / len, ay / len};
    }
    float most = 1e30f;
    const float dd[2] = {d.x, d.y}, cc[2] = {c.x, c.y};
    for (int a = 0; a < 2; ++a) {
        if (dd[a] > 1e-6f) most = std::min(most, (hi[a] - cc[a]) / dd[a]);
        if (dd[a] < -1e-6f) most = std::min(most, (lo[a] - cc[a]) / dd[a]);
    }
    if (!(most > 0.f) || most >= 1e29f) {
        out.reached = false;
        return out;
    }
    auto at = [&](float s) {
        MapOverlayView t = v;
        t.centre = Vec2{c.x + d.x * s, c.y + d.y * s};
        return t;
    };
    constexpr int kSteps = 40;
    float best = out.land, bestS = 0.f, prev = 0.f;
    for (int i = 1; i <= kSteps; ++i) {
        const float s = most * static_cast<float>(i) / static_cast<float>(kSteps);
        const float share = mapLandShare(m, look, at(s));
        if (share >= kMapLandShareMin) { // the least shift that reaches it, between the last step and this one
            float a = prev, b = s, bShare = share;
            for (int it = 0; it < 14; ++it) {
                const float mid = 0.5f * (a + b);
                const float midShare = mapLandShare(m, look, at(mid));
                if (midShare >= kMapLandShareMin) {
                    b = mid;
                    bShare = midShare;
                } else {
                    a = mid;
                }
            }
            out.view = at(b);
            out.land = bShare; // the very value that passed: a reached frame's land is never under the minimum
            return out;
        }
        if (share > best + 1e-6f) {
            best = share;
            bestS = s;
        }
        prev = s;
    }
    out.view = at(bestS);
    out.land = best;
    out.reached = false;
    return out;
}

Vec2 mapOverlayHold(const MiniMapModel& m, const MapOverlayView& v, Vec2 framed) {
    auto axis = [](float c, float lo, float hi, float half, float f) {
        float a = lo + half, b = hi - half;
        if (hi - lo <= 2.f * half) a = b = 0.5f * (lo + hi);
        a = std::min(a, f);
        b = std::max(b, f);
        return std::clamp(c, a, b);
    };
    return Vec2{axis(v.centre.x, m.minX, m.maxX, 0.5f * v.screenW * v.mPerPx, framed.x),
                axis(v.centre.y, m.minY, m.maxY, 0.5f * v.screenH * v.mPerPx, framed.y)};
}

bool mapSeaLabelAt(const MiniMapModel& m, const MapLook& look, const MapOverlayView& v, float w, float h, Vec2& at) {
    const float hx = 0.5f * w * v.mPerPx, hy = 0.5f * h * v.mPerPx;
    const Vec2 lo = mapOverlayFromScreen(v, Vec2{0.f, 0.f}), hi = mapOverlayFromScreen(v, Vec2{v.screenW, v.screenH});
    float best = -1.f;
    auto tryAt = [&](Vec2 p, float depth) {
        if (depth <= best || p.x - hx < lo.x || p.x + hx > hi.x || p.y - hy < lo.y || p.y + hy > hi.y) return;
        const Vec2 probe[5] = {p, {p.x - hx, p.y - hy}, {p.x + hx, p.y - hy}, {p.x + hx, p.y + hy}, {p.x - hx, p.y + hy}};
        for (const Vec2& q : probe)
            if (!mapIsSea(m, look, q)) return;
        best = depth;
        at = p;
    };
    for (const MapLook::SeaSpot& s : look.seaSpots) tryAt(s.at, s.depth);
    // The open sea off the model, when the frame shows it: the middle of that strip.
    if (look.seaSide != 0) {
        const float x0 = look.seaSide > 0 ? std::max(lo.x, m.maxX) : lo.x, x1 = look.seaSide > 0 ? hi.x : std::min(hi.x, m.minX);
        if (x1 > x0) tryAt(Vec2{0.5f * (x0 + x1), 0.5f * (lo.y + hi.y)}, 0.5f * (x1 - x0));
    }
    return best >= 0.f;
}

MapWaterSeat mapWaterLabelAt(const MiniMapModel& m, const MapNames& n, const MapLook& look, const MapOverlayView& v, MapZoom z,
                             const MapRoadInk& ink, Vec2 at, float w, float h, float grow, MapLabel& out, int picked,
                             std::uint16_t pickedName, float reachM) {
    constexpr float kPi = 3.14159265358979f;
    auto wet = [&](Vec2 p) { return mapIsWater(m, p) || mapIsSea(m, look, p); };
    if (!wet(at)) return MapWaterSeat::Dry;
    if (!(w > 0.f && h > 0.f && v.mPerPx > 0.f && v.screenW > 0.f && v.screenH > 0.f)) return MapWaterSeat::NoSeat;
    // The body: the rectangle that holds `at`, else a disc of reachM round it.
    const MapRect* body = nullptr;
    for (const MapRect& r : m.waterRects) {
        const float c = std::cos(r.rot), s = std::sin(r.rot);
        const float dx = at.x - r.cx, dy = at.y - r.cy;
        if (std::abs(dx * c + dy * s) <= r.hx && std::abs(-dx * s + dy * c) <= r.hy) {
            body = &r;
            break;
        }
    }
    auto inBody = [&](Vec2 p) {
        if (!body) return std::hypot(p.x - at.x, p.y - at.y) <= reachM;
        const float c = std::cos(body->rot), s = std::sin(body->rot);
        const float dx = p.x - body->cx, dy = p.y - body->cy;
        return std::abs(dx * c + dy * s) <= body->hx && std::abs(-dx * s + dy * c) <= body->hy;
    };
    // How far the body reaches from `at` (m), and the angles to try (screen degrees; world and screen share their axes).
    float extent = reachM;
    float angles[2] = {0.f, 0.f};
    int angleCount = 1;
    if (body) {
        const float c = std::abs(std::cos(body->rot)), s = std::abs(std::sin(body->rot));
        const float ex = body->hx * c + body->hy * s, ey = body->hx * s + body->hy * c;
        extent = std::max(std::abs(at.x - body->cx) + ex, std::abs(at.y - body->cy) + ey);
        const float longDeg = (body->hx >= body->hy ? body->rot : body->rot + 0.5f * kPi) * 180.f / kPi;
        const float along = mapUprightDeg(longDeg);
        if (std::abs(along) > 1.f) angles[angleCount++] = along;
    }
    const float step = std::max(3.f * v.mPerPx, extent / 48.f);
    const int rings = static_cast<int>(std::ceil(extent / step));
    float bestD = 1e30f;
    bool found = false;
    MapLabel best;
    auto tryAt = [&](Vec2 p) {
        const float d = std::hypot(p.x - at.x, p.y - at.y);
        if (d >= bestD || !inBody(p)) return;
        const Vec2 s = mapOverlayToScreen(v, p);
        for (int a = 0; a < angleCount; ++a) {
            MapLabel l;
            l.at = s;
            l.angleDeg = angles[a];
            l.w = w;
            l.h = h;
            // The box grown by `grow` whole on the screen.
            Vec2 k[4];
            mapLabelCorners(l, grow, k);
            bool inside = true;
            for (const Vec2& q : k) inside = inside && q.x >= kMapLabelInsetPx && q.y >= kMapLabelInsetPx && q.x <= v.screenW - kMapLabelInsetPx && q.y <= v.screenH - kMapLabelInsetPx;
            if (!inside) continue;
            // The box itself all water: probes no more than 4 px apart along each side, corners included.
            const float r = angles[a] * kPi / 180.f;
            const float cr = std::cos(r), sr = std::sin(r);
            const int nx = std::max(2, static_cast<int>(std::ceil(w / 4.f)) + 1), ny = std::max(2, static_cast<int>(std::ceil(h / 4.f)) + 1);
            bool all = true;
            for (int j = 0; j < ny && all; ++j)
                for (int i = 0; i < nx && all; ++i) {
                    const float u = (static_cast<float>(i) / static_cast<float>(nx - 1) - 0.5f) * w;
                    const float t = (static_cast<float>(j) / static_cast<float>(ny - 1) - 0.5f) * h;
                    all = wet(mapOverlayFromScreen(v, Vec2{s.x + u * cr - t * sr, s.y + u * sr + t * cr}));
                }
            if (!all) continue;
            if (mapLabelOnAnyRoad(m, n, v, z, ink, l, grow, picked, pickedName) >= 0) continue;
            bestD = d;
            best = l;
            found = true;
            return; // level first: a turned seat at the same point is no nearer
        }
    };
    // Square rings out from `at`; a ring whose nearest point is past the best seat found cannot hold a nearer one.
    for (int ring = 0; ring <= rings; ++ring) {
        if (static_cast<float>(ring) * step >= bestD) break;
        auto cellAt = [&](int ix, int iy) { tryAt(Vec2{at.x + static_cast<float>(ix) * step, at.y + static_cast<float>(iy) * step}); };
        if (ring == 0) {
            cellAt(0, 0);
            continue;
        }
        for (int ix = -ring; ix <= ring; ++ix) {
            cellAt(ix, -ring);
            cellAt(ix, ring);
        }
        for (int iy = -ring + 1; iy <= ring - 1; ++iy) {
            cellAt(-ring, iy);
            cellAt(ring, iy);
        }
    }
    if (!found) return MapWaterSeat::NoSeat;
    out = best;
    return MapWaterSeat::Seated;
}

} // namespace ftcore
