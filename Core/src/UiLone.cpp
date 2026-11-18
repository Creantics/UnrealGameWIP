#include "ftcore/UiLone.h"

#include <algorithm>

namespace ftcore {

namespace {

constexpr std::size_t kFigureCap = sizeof(UiLoneFigure{}.text);

// Appends s (keeping the NUL); returns the new length.
std::size_t put(UiLoneFigure& f, std::size_t n, std::string_view s) {
    for (char c : s) {
        if (n + 1 >= kFigureCap) break;
        f.text[n++] = c;
    }
    f.text[n] = 0;
    return n;
}

std::size_t putUnsigned(UiLoneFigure& f, std::size_t n, std::uint32_t v) {
    char digits[10];
    std::size_t d = 0;
    do {
        digits[d++] = static_cast<char>('0' + v % 10u);
        v /= 10u;
    } while (v != 0u && d < sizeof(digits));
    while (d > 0) {
        const char c[1] = {digits[--d]};
        n = put(f, n, std::string_view(c, 1));
    }
    return n;
}

// v / scale (scale 10 or 100), its decimals with the trailing zeros dropped: 130/100 "1.3", 115/100 "1.15", 200/100
// "2", 145/10 "14.5".
std::size_t putFixed(UiLoneFigure& f, std::size_t n, std::uint32_t v, std::uint32_t scale) {
    n = putUnsigned(f, n, v / scale);
    std::uint32_t frac = v % scale;
    if (frac == 0u) return n;
    n = put(f, n, ".");
    for (std::uint32_t s = scale / 10u; s > 0u && frac > 0u; s /= 10u) {
        const char c[1] = {static_cast<char>('0' + frac / s)};
        n = put(f, n, std::string_view(c, 1));
        frac %= s;
    }
    return n;
}

// A multiplier in hundredths, rounded (0 for anything not a positive finite number; large values held).
std::uint32_t hundredths(float x) {
    if (!(x > 0.f)) return 0u;
    if (!(x < 1.0e6f)) return 100000000u;
    return static_cast<std::uint32_t>(x * 100.f + 0.5f);
}

UiLoneFigure times(std::uint32_t h) {
    UiLoneFigure f;
    std::size_t n = put(f, 0, "\xC3\x97"); // U+00D7, the multiplication sign
    putFixed(f, n, h, 100u);
    return f;
}

UiLoneFigure plain(std::uint32_t v) {
    UiLoneFigure f;
    putUnsigned(f, 0, v);
    return f;
}

// Permille as a percent, one decimal at most, with an optional sign: 145 "+14.5%", 150 "15%".
UiLoneFigure percent(int permille, bool sign) {
    UiLoneFigure f;
    std::size_t n = sign ? put(f, 0, "+") : 0;
    n = putFixed(f, n, static_cast<std::uint32_t>(std::max(permille, 0)), 10u);
    put(f, n, "%");
    return f;
}

template <std::size_t N, typename Row, typename Kind>
void add(std::array<Row, N>& rows, std::uint8_t& count, Kind kind, const UiLoneFigure& figure) {
    if (count >= N) return;
    rows[count].kind = kind;
    rows[count].figure = figure;
    ++count;
}

} // namespace

std::string_view UiLoneFigure::view() const {
    std::size_t n = 0;
    while (n < kFigureCap && text[n] != 0) ++n;
    return std::string_view(text, n);
}

UiLoneCard uiLoneCard(const LoneBargain& b) {
    UiLoneCard c;
    const UiLoneFigure none;
    const std::uint32_t release = hundredths(b.release);
    const std::uint32_t gain = hundredths(b.loadGain);
    const std::uint32_t taken = hundredths(b.damageTaken);
    if (release > 100u) add(c.gain, c.gainCount, UiLoneLine::Release, times(release));
    if (gain > 100u) add(c.gain, c.gainCount, UiLoneLine::Load, times(gain));
    if (b.loadCap > 0) add(c.gain, c.gainCount, UiLoneLine::LoadCap, plain(static_cast<std::uint32_t>(b.loadCap)));
    if (taken > 100u) add(c.give, c.giveCount, UiLoneLine::DamageTaken, times(taken));
    if (b.noCount) add(c.give, c.giveCount, UiLoneLine::NoCount, none);
    if (b.noTable) add(c.give, c.giveCount, UiLoneLine::NoTable, none);
    if (b.noTakeHalf) add(c.give, c.giveCount, UiLoneLine::NoTakeHalf, none);
    if (b.giftReceivedEnds) add(c.give, c.giveCount, UiLoneLine::GiftEnds, none);
    return c;
}

UiLoneRoll uiLoneRoll(const LoneRoll& r) {
    UiLoneRoll out;
    if (!r.shown()) return out;
    const LonePoolEffect& e = r.effect;
    add(out.rows, out.count, UiLoneRollLine::Releases, plain(r.releases));
    add(out.rows, out.count, UiLoneRollLine::Pooled, plain(r.pooled));
    if (e.totalPermille > 0) add(out.rows, out.count, UiLoneRollLine::Extra, percent(e.totalPermille, true));
    if (e.figures > 0) {
        add(out.rows, out.count, UiLoneRollLine::Figures, plain(static_cast<std::uint32_t>(e.figures)));
        add(out.rows, out.count, UiLoneRollLine::FigureShare, percent(e.figurePermille, false));
    }
    if (e.hpPermille > 0) add(out.rows, out.count, UiLoneRollLine::BossHp, percent(e.hpPermille, true));
    return out;
}

void uiLoneStep(UiLone& s, const UiLoneInput& in, float dt) {
    if (!in.fight) {
        s = UiLone{};
        return;
    }
    const float t = dt > 0.f && dt < 1.0e6f ? dt : 0.f;
    if (in.began) {
        s.card = true;
        s.full = in.full;
        s.holding = true;
        s.held = 0.f;
        s.fadeLeft = kUiLoneCardFade;
    } else if (s.card && s.holding) {
        s.held += t;
        if (in.attack || !in.alone || s.held >= kUiLoneCardHold) s.holding = false;
    } else if (s.card) {
        s.fadeLeft -= t;
        if (s.fadeLeft <= 0.f) {
            s.card = false;
            s.fadeLeft = 0.f;
        }
    }
    if (s.ended != UiLoneWord::None) {
        s.endedLeft -= t;
        if (s.endedLeft <= 0.f) {
            s.ended = UiLoneWord::None;
            s.endedLeft = 0.f;
        }
    }
    if (s.alone && !in.alone) {
        s.ended = in.endedBy == LoneEnd::Gift            ? UiLoneWord::EndedGift
                : in.endedBy == LoneEnd::PartnerCalled ? UiLoneWord::EndedPartner
                                                       : UiLoneWord::None;
        s.endedLeft = s.ended != UiLoneWord::None ? kUiLoneEndedShow : 0.f;
    }
    s.alone = in.alone;
}

float uiLoneCardAlpha(const UiLone& s) {
    if (!s.card) return 0.f;
    if (s.holding) return 1.f;
    return std::clamp(s.fadeLeft / kUiLoneCardFade, 0.f, 1.f);
}

UiLoneWord uiLoneWord(const UiLone& s) { return s.alone ? UiLoneWord::Alone : s.ended; }

UiLeadWord uiLeadWord(UiLeadSlot gauge, bool wait, UiLoneWord lone) {
    if (gauge == UiLeadSlot::Release) return UiLeadWord::Release;
    if (wait) return UiLeadWord::Wait;
    if (gauge == UiLeadSlot::Load) return UiLeadWord::Load;
    switch (lone) {
    case UiLoneWord::Alone: return UiLeadWord::Alone;
    case UiLoneWord::EndedGift: return UiLeadWord::EndedGift;
    case UiLoneWord::EndedPartner: return UiLeadWord::EndedPartner;
    case UiLoneWord::None: break;
    }
    return UiLeadWord::None;
}

float uiWaitLineStep(UiWaitLine& s, bool waiting, float left) {
    if (!waiting || !(left > 0.f)) {
        s = UiWaitLine{};
        return 0.f;
    }
    constexpr float kRise = 1e-3f; // a step's rounding is not a new wait
    if (!(s.full > 0.f) || left > s.left + kRise) s.full = left;
    s.left = left;
    return std::clamp(left / s.full, 0.f, 1.f);
}

} // namespace ftcore
