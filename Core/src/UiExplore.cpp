#include "ftcore/UiExplore.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

float safeDt(float dt) { return std::isfinite(dt) && dt > 0.f ? dt : 0.f; }

bool isSpace(char16_t c) { return c == u' ' || c == u'\n' || c == u'\t' || c == u'\r'; }

bool isLowSurrogate(char16_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

} // namespace

// --- Objective --------------------------------------------------------------------------------------------------

void uiObjectiveStep(UiObjectiveFade& f, std::uint32_t serial, float dt) {
    dt = safeDt(dt);
    if (!f.seen || serial != f.serial) {
        f.seen = true;
        f.serial = serial;
        f.age = 0.f;
    } else {
        f.age += dt;
    }
    // Up to full on a change (from wherever it stood), then down to quiet after the hold.
    const float want = f.age < kUiObjectiveHold
        ? 1.f
        : 1.f - (1.f - kUiObjectiveQuiet) * clamp01((f.age - kUiObjectiveHold) / kUiObjectiveEase);
    if (f.alpha < want) {
        f.alpha = std::min(want, f.alpha + dt / kUiObjectiveRise);
    } else {
        f.alpha = want;
    }
}

// --- Notices ----------------------------------------------------------------------------------------------------

std::uint8_t uiNoticePush(UiNoticeQueue& q) {
    if (q.count < kUiNoticeQueue) {
        // The first free slot: one no queued notice holds.
        std::uint8_t slot = 0;
        for (; slot < kUiNoticeQueue; ++slot) {
            bool used = false;
            for (std::uint8_t i = 0; i < q.count; ++i) used = used || q.order[i] == slot;
            if (!used) break;
        }
        if (q.count == 0) q.age = 0.f;
        q.order[q.count++] = slot;
        return slot;
    }
    // Full: the last waiting notice gives its slot to the new one.
    return q.order[kUiNoticeQueue - 1];
}

void uiNoticeStep(UiNoticeQueue& q, float dt) {
    if (q.count == 0) {
        q.age = 0.f;
        return;
    }
    q.age += safeDt(dt);
    if (q.age >= kUiNoticeTime) {
        for (std::uint8_t i = 1; i < q.count; ++i) q.order[i - 1] = q.order[i];
        --q.count;
        q.age = 0.f; // the next starts on the next tick's time, never mid-fade
    }
}

int uiNoticeShown(const UiNoticeQueue& q) { return q.count > 0 ? static_cast<int>(q.order[0]) : -1; }

float uiNoticeAlpha(const UiNoticeQueue& q) {
    if (q.count == 0) return 0.f;
    const float in = clamp01(q.age / kUiNoticeFadeIn);
    const float out = clamp01((kUiNoticeTime - q.age) / kUiNoticeFadeOut);
    return std::min(in, out);
}

// --- Subtitle ---------------------------------------------------------------------------------------------------

void uiSubtitleFit(std::u16string_view text, UiSubtitleFit& out) {
    out.count = 0;
    const auto n = static_cast<std::uint32_t>(text.size());
    std::uint32_t i = 0;
    const auto push = [&out](std::uint32_t b, std::uint32_t e) {
        if (out.count < kUiSubtitleMaxLines) {
            out.line[out.count++] = {b, e};
        } else {
            out.line[kUiSubtitleMaxLines - 1].end = e; // the rest joins the last line
        }
    };
    while (i < n) {
        while (i < n && text[i] != u'\n' && isSpace(text[i])) ++i; // a line never starts with a space
        if (i < n && text[i] == u'\n') {
            ++i; // an empty line from a doubled newline is dropped
            continue;
        }
        if (i >= n) break;
        const std::uint32_t begin = i;
        std::uint32_t lastFit = begin; // the end of the last whole word that fits
        std::size_t chars = 0;
        std::uint32_t j = begin;
        bool forced = false;
        while (j < n) {
            if (text[j] == u'\n') {
                lastFit = j;
                forced = true;
                break;
            }
            if (isSpace(text[j])) {
                lastFit = j;
            }
            if (!isLowSurrogate(text[j])) ++chars;
            if (chars > kUiSubtitleChars) break;
            ++j;
        }
        std::uint32_t end;
        if (forced || j >= n) {
            end = forced ? lastFit : n;
        } else if (lastFit > begin) {
            end = lastFit;
        } else {
            // One word longer than a line: it stands alone, whole.
            end = j;
            while (end < n && !isSpace(text[end])) ++end;
        }
        std::uint32_t trimmed = end;
        while (trimmed > begin && isSpace(text[trimmed - 1])) --trimmed;
        push(begin, trimmed);
        i = end;
        if (forced) ++i;
    }
}

std::size_t uiSubtitlePages(const UiSubtitleFit& fit) {
    return (static_cast<std::size_t>(fit.count) + kUiSubtitleLines - 1) / kUiSubtitleLines;
}

std::size_t uiSubtitlePage(const UiSubtitleFit& fit, std::uint32_t revealed) {
    if (fit.count == 0) return 0;
    // The last line whose start is before the reveal point holds its last revealed character.
    std::size_t lineAt = 0;
    for (std::size_t l = 0; l < fit.count; ++l) {
        if (fit.line[l].begin < revealed) lineAt = l;
    }
    return lineAt / kUiSubtitleLines;
}

float uiSubtitleBand(UiColor ink, UiColor picture, float* ratio, float* withBand) {
    const UiColor black{0.f, 0.f, 0.f, 1.f};
    const UiColor none{0.f, 0.f, 0.f, 0.f};
    const float bare = uiContrast(ink, none, picture);
    const float band = uiLeastBacking(ink, black, picture, kUiSubtitleMinContrast, 0.f, kUiSubtitleBandMax);
    if (ratio) *ratio = bare;
    if (withBand) *withBand = uiContrast(ink, UiColor{0.f, 0.f, 0.f, band}, picture);
    return band;
}

// --- A small line's guard ---------------------------------------------------------------------------------------

namespace {

// The ink over the contour (judged by its mean) over the picture under a black shade.
float guarded(UiColor ink, UiColor pic, float spread, float alpha, float shade) {
    const UiColor under{pic.r * (1.f - shade), pic.g * (1.f - shade), pic.b * (1.f - shade), 1.f};
    return uiContrast(ink, UiColor{0.f, 0.f, 0.f, uiContourMean(spread, alpha)}, under);
}

} // namespace

float uiContourMean(float spread, float alpha) {
    const float s = std::clamp(spread, 0.f, kUiContourWindow);
    const float outer = std::clamp(kUiContourWindow - s, 0.f, 1.f);
    return clamp01(alpha) * (s + outer * kUiContourOuter) / kUiContourWindow;
}

UiLineGuard uiLineGuard(UiColor ink, UiColor picture, float target, float restSpread, float restAlpha) {
    const UiColor pic{picture.r, picture.g, picture.b, 1.f};
    UiLineGuard g;
    g.bare = uiContrast(ink, UiColor{0.f, 0.f, 0.f, 0.f}, pic);
    const float rest = std::clamp(restAlpha, 0.f, kUiContourAlphaMax);
    float spread = std::clamp(std::round(restSpread), 1.f, kUiContourSpreadMax);
    g.spread = spread;
    g.alpha = rest;
    g.ratio = guarded(ink, pic, spread, rest, 0.f);
    if (g.ratio >= target) return g;
    for (; spread <= kUiContourSpreadMax; spread += 1.f) {
        if (guarded(ink, pic, spread, kUiContourAlphaMax, 0.f) < target) continue;
        float lo = rest, hi = kUiContourAlphaMax; // contrast rises with the alpha: bisect the least that reaches the target
        if (guarded(ink, pic, spread, lo, 0.f) >= target) hi = lo;
        for (int i = 0; i < 20 && hi > lo; ++i) {
            const float m = 0.5f * (lo + hi);
            (guarded(ink, pic, spread, m, 0.f) >= target ? hi : lo) = m;
        }
        g.spread = spread;
        g.alpha = hi;
        g.ratio = guarded(ink, pic, spread, hi, 0.f);
        return g;
    }
    // The contour at its cap falls short: the least shade under the line (the cap when even that falls short).
    g.spread = kUiContourSpreadMax;
    g.alpha = kUiContourAlphaMax;
    float lo = 0.f, hi = kUiLineShadeMax;
    if (guarded(ink, pic, g.spread, g.alpha, hi) >= target) {
        for (int i = 0; i < 20; ++i) {
            const float m = 0.5f * (lo + hi);
            (guarded(ink, pic, g.spread, g.alpha, m) >= target ? hi : lo) = m;
        }
    }
    g.shade = hi;
    g.ratio = guarded(ink, pic, g.spread, g.alpha, hi);
    return g;
}

float uiQuietFloor(UiColor ink, UiColor picture, float target, float quiet) {
    const UiColor pic{picture.r, picture.g, picture.b, 1.f};
    const auto at = [&](float a) {
        return guarded(UiColor{ink.r, ink.g, ink.b, ink.a * a}, pic, kUiContourSpreadMax, kUiContourAlphaMax, 0.f);
    };
    float lo = clamp01(quiet), hi = 1.f;
    if (at(lo) >= target) return lo;
    if (at(hi) < target) return hi;
    for (int i = 0; i < 20; ++i) {
        const float m = 0.5f * (lo + hi);
        (at(m) >= target ? hi : lo) = m;
    }
    return hi;
}

namespace {

// A change detector only: Rec. 709 weights on the picture's values as read.
float readLuma(UiColor c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

bool viewFinite(const UiReadView& v) {
    return std::isfinite(v.x0) && std::isfinite(v.y0) && std::isfinite(v.x1) && std::isfinite(v.y1) && std::isfinite(v.px) &&
           std::isfinite(v.py) && std::isfinite(v.pz) && std::isfinite(v.yaw) && std::isfinite(v.pitch);
}

// The turn between two angles in degrees, the short way round.
float turnDeg(float a, float b) {
    const float d = std::fabs(std::remainder(a - b, 360.f));
    return d;
}

} // namespace

void uiReadNote(UiReadMemo& memo, const UiReadView& view, UiColor picture, double now) {
    const float l = readLuma(picture);
    if (!viewFinite(view) || !std::isfinite(l) || !std::isfinite(now)) {
        memo = UiReadMemo{}; // nothing to stand on: the next read is real
        return;
    }
    memo.luma[1] = memo.luma[0];
    memo.luma[0] = l;
    memo.view = view;
    memo.at = now;
    memo.valid = true;
}

bool uiReadSkip(const UiReadMemo& memo, const UiReadView& view, double now) {
    if (!memo.valid || memo.luma[1] < 0.f || !viewFinite(view) || !std::isfinite(now)) return false;
    if (!(now - memo.at < static_cast<double>(kUiReadSkipForceS)) || now < memo.at) return false;
    const UiReadView& m = memo.view;
    if (std::fabs(view.x0 - m.x0) > kUiReadSkipBoxPx || std::fabs(view.y0 - m.y0) > kUiReadSkipBoxPx ||
        std::fabs(view.x1 - m.x1) > kUiReadSkipBoxPx || std::fabs(view.y1 - m.y1) > kUiReadSkipBoxPx) {
        return false;
    }
    const float dx = view.px - m.px, dy = view.py - m.py, dz = view.pz - m.pz;
    if (dx * dx + dy * dy + dz * dz > kUiReadSkipMove * kUiReadSkipMove) return false;
    if (turnDeg(view.yaw, m.yaw) > kUiReadSkipTurnDeg || turnDeg(view.pitch, m.pitch) > kUiReadSkipTurnDeg) return false;
    const float a = memo.luma[0], b = memo.luma[1];
    const float d = std::fabs(a - b);
    return d <= kUiReadSkipLumaAbs || d <= kUiReadSkipLumaShare * std::max(a, b);
}

} // namespace ftcore
