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
    const UiColor none{0.f, 0.f, 0.f, 0.f};
    const UiColor band{0.f, 0.f, 0.f, kUiSubtitleBand};
    const float bare = uiContrast(ink, none, picture);
    const float banded = uiContrast(ink, band, picture);
    if (ratio) *ratio = bare;
    if (withBand) *withBand = banded;
    return bare >= kUiSubtitleMinContrast ? 0.f : kUiSubtitleBand;
}

} // namespace ftcore
