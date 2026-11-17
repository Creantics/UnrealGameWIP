#include "ftcore/UiHudFit.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

const UiHudSlotRule& uiHudSlotRule(UiHudSlot slot) {
    const std::size_t i = static_cast<std::size_t>(slot);
    return kUiHudSlots[i < kUiHudSlotCount ? i : 0];
}

float uiHudTextPx(UiHudSlot slot, float height, float textScale) {
    // A NaN or a scale under 1 reads as 100%: text never comes out smaller than the slot's own size.
    const float scale = textScale >= 1.f ? std::min(textScale, 2.f) : 1.f;
    const float h = height >= 1.f ? height : 1.f;
    return std::max(uiHudSlotRule(slot).px * scale * h / 1080.f, kUiHudMinPx);
}

namespace {
bool isGap(char16_t c) { return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r'; }
} // namespace

std::size_t uiHudWords(std::u16string_view text, UiHudWord* out, std::size_t cap) {
    if (!out || cap == 0) return 0;
    std::size_t n = 0;
    std::size_t i = 0;
    const std::size_t len = text.size();
    while (i < len) {
        while (i < len && isGap(text[i])) ++i;
        if (i >= len) break;
        const std::size_t b = i;
        while (i < len && !isGap(text[i])) ++i;
        if (n < cap) {
            out[n++] = UiHudWord{static_cast<std::uint32_t>(b), static_cast<std::uint32_t>(i), 0.f};
        } else {
            out[n - 1].end = static_cast<std::uint32_t>(i); // past the cap the last span runs on
        }
    }
    return n;
}

void uiHudFit(const UiHudWord* words, std::size_t n, float spaceW, float room, std::uint8_t lines, UiHudFit& out) {
    out = UiHudFit{};
    if (!words || n == 0) return;
    const std::uint8_t want = lines > 0 ? lines : 1;
    const float gap = std::max(spaceW, 0.f);
    const auto put = [&](std::size_t a, std::size_t b, float w) { // words [a, b)
        const std::size_t k = out.count++;
        out.line[k] = UiSubtitleLine{words[a].begin, words[b - 1].end};
        out.w[k] = w;
        out.widest = std::max(out.widest, w);
        out.over = out.over || w > room;
    };
    float total = 0.f;
    for (std::size_t i = 0; i < n; ++i) total += words[i].w + (i > 0 ? gap : 0.f);

    // No room (zero, negative or NaN): the whole text on one line, over.
    if (!(room > 0.f)) {
        put(0, n, total);
        out.over = true;
        out.shown = 1;
        return;
    }

    // Whole: it fits, or the slot never breaks (a name runs over, whole), or it is one word.
    if (total <= room || want == 1 || n == 1) {
        put(0, n, total);
        out.shown = 1;
        return;
    }

    // Two lines, broken at the gap nearest the middle (the least wide of the two halves' wider), both inside the room.
    std::size_t best = n;
    float bestMax = 0.f, bestL = 0.f, bestR = 0.f;
    float left = 0.f;
    for (std::size_t k = 1; k < n; ++k) {
        left += words[k - 1].w + (k > 1 ? gap : 0.f);
        const float right = total - left - gap;
        const float m = std::max(left, right);
        if (m <= room && (best == n || m < bestMax)) {
            best = k;
            bestMax = m;
            bestL = left;
            bestR = right;
        }
    }
    if (best < n) {
        put(0, best, bestL);
        put(best, n, bestR);
        out.shown = 2;
        return;
    }

    // A window: greedy lines of at most the room (a word wider stands alone, whole); past kUiHudMaxLines the rest joins
    // the last line.
    std::size_t a = 0;
    float w = words[0].w;
    for (std::size_t i = 1; i < n; ++i) {
        if (w + gap + words[i].w <= room || static_cast<std::size_t>(out.count) + 1 >= kUiHudMaxLines) {
            w += gap + words[i].w;
        } else {
            put(a, i, w);
            a = i;
            w = words[i].w;
        }
    }
    put(a, n, w);
    out.shown = static_cast<std::uint8_t>(std::min<std::size_t>(out.count, want));
}

std::size_t uiHudPages(const UiHudFit& fit) {
    if (fit.shown == 0) return 0;
    return (static_cast<std::size_t>(fit.count) + fit.shown - 1) / fit.shown;
}

std::size_t uiHudPageLine(const UiHudFit& fit, float age) {
    const std::size_t pages = uiHudPages(fit);
    if (pages <= 1) return 0;
    const float t = std::isfinite(age) && age > 0.f ? age : 0.f; // NaN, infinities and negatives: the first page
    const float cycle = static_cast<float>(pages) * kUiHudPageTime;
    const std::size_t page = std::min(static_cast<std::size_t>(std::fmod(t, cycle) / kUiHudPageTime), pages - 1);
    return page * fit.shown;
}

} // namespace ftcore
