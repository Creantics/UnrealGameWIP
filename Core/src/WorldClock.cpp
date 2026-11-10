#include "ftcore/WorldClock.h"

#include "CsvRead.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ftcore {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

double wrapHours(double h) {
    h = std::fmod(h, 24.0);
    if (h < 0.0) h += 24.0;
    if (h >= 24.0) h = 0.0;  // fmod of a value just under a multiple of 24 can round up
    return h;
}

}  // namespace

void worldClockTick(WorldClock& clock, float dtSeconds) {
    if (clock.paused || !(dtSeconds > 0.0f) || !std::isfinite(dtSeconds)) return;
    const double minutes = std::clamp(static_cast<double>(clock.dayMinutes), static_cast<double>(kWorldClockMinDayMinutes),
                                      static_cast<double>(kWorldClockMaxDayMinutes));
    const double h = clock.hours + static_cast<double>(dtSeconds) * 24.0 / (minutes * 60.0);
    const double days = std::floor(h / 24.0);
    clock.day += static_cast<std::uint32_t>(days);
    clock.hours = wrapHours(h);
}

void worldClockSet(WorldClock& clock, double hours) {
    if (!std::isfinite(hours)) return;
    clock.hours = wrapHours(hours);
}

void worldClockAdvanceTo(WorldClock& clock, double hours) {
    if (!std::isfinite(hours)) return;
    const double target = wrapHours(hours);
    if (target < clock.hours) ++clock.day;
    clock.hours = target;
}

void worldClockPause(WorldClock& clock) { clock.paused = true; }
void worldClockResume(WorldClock& clock) { clock.paused = false; }

void worldClockSetDayMinutes(WorldClock& clock, float minutes) {
    if (!std::isfinite(minutes)) return;
    clock.dayMinutes = std::clamp(minutes, kWorldClockMinDayMinutes, kWorldClockMaxDayMinutes);
}

WorldClock worldClockSanitized(const WorldClock& clock) {
    WorldClock c = clock;
    c.hours = std::isfinite(c.hours) ? wrapHours(c.hours) : 0.0;
    c.dayMinutes = std::isfinite(c.dayMinutes)
                       ? std::clamp(c.dayMinutes, kWorldClockMinDayMinutes, kWorldClockMaxDayMinutes)
                       : kWorldClockDefaultDayMinutes;
    return c;
}

SunDirection sunDirection(const WorldClockSpec& spec, double hours) {
    // Equinox (declination 0): hour angle 15 degrees per hour from noon. At latitude phi the sun at hour angle H sits at
    // up = cos(phi) cos(H), east = -sin(H), north = -sin(phi) cos(H).
    const double h = (wrapHours(hours) - 12.0) * 15.0 * kDeg;
    const double phi = static_cast<double>(spec.latitudeDeg) * kDeg;
    SunDirection d;
    d.east = static_cast<float>(-std::sin(h));
    d.north = static_cast<float>(-std::sin(phi) * std::cos(h));
    d.up = static_cast<float>(std::cos(phi) * std::cos(h));
    return d;
}

float sunElevationDeg(const WorldClockSpec& spec, double hours) {
    const SunDirection d = sunDirection(spec, hours);
    return static_cast<float>(std::asin(std::clamp(static_cast<double>(d.up), -1.0, 1.0)) / kDeg);
}

float nightAmount(const WorldClockSpec& spec, double hours) {
    const float e = sunElevationDeg(spec, hours);
    const float span = spec.twilightHigh - spec.twilightLow;
    if (!(span > 0.0f)) return e < spec.twilightHigh ? 1.0f : 0.0f;
    const float t = std::clamp((spec.twilightHigh - e) / span, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float timeCurveAt(const TimeCurve& curve, double hours) {
    const std::uint32_t n = curve.count;
    if (n == 0) return 0.0f;
    if (n == 1) return curve.values[0];
    const float h = static_cast<float>(wrapHours(hours));
    // The key at or before h, wrapping to the last key when h is before the first.
    std::uint32_t i = n - 1;
    for (std::uint32_t k = 0; k < n; ++k) {
        if (curve.hours[k] <= h) i = k;
        else break;
    }
    const std::uint32_t j = (i + 1) % n;
    float h0 = curve.hours[i], h1 = curve.hours[j], x = h;
    if (h1 <= h0) h1 += 24.0f;  // the span from the last key round midnight to the first
    if (x < h0) x += 24.0f;
    const float t = std::clamp((x - h0) / (h1 - h0), 0.0f, 1.0f);
    return curve.values[i] + (curve.values[j] - curve.values[i]) * t;
}

bool parseTimeCurves(std::string_view csv, TimeCurves& out, std::string* error) {
    using namespace detail;
    std::vector<Record> records;
    if (!splitCsv(csv, records, error)) return false;
    std::vector<float> hours;
    bool header = false, haveCrowd = false, haveDanger = false;
    TimeCurves result;
    for (const Record& r : records) {
        if (isComment(r)) continue;
        if (!header) {
            if (r.fields.empty() || trim(r.fields[0]) != "curve") return fail(error, r.line, "the first row must be \"curve,<hour>,...\"");
            if (r.fields.size() < 2) return fail(error, r.line, "no hours in the header");
            if (r.fields.size() - 1 > kTimeCurveMaxKeys) return fail(error, r.line, "more than 24 hours in the header");
            for (std::size_t k = 1; k < r.fields.size(); ++k) {
                double h = 0.0;
                if (!parseNumber(trim(r.fields[k]), h) || h < 0.0 || h >= 24.0)
                    return fail(error, r.line, "hour \"" + r.fields[k] + "\" is not a number in 0..24");
                if (!hours.empty() && static_cast<float>(h) <= hours.back())
                    return fail(error, r.line, "hours must rise");
                hours.push_back(static_cast<float>(h));
            }
            header = true;
            continue;
        }
        const std::string_view name = r.fields.empty() ? std::string_view{} : trim(r.fields[0]);
        TimeCurve* curve = nullptr;
        if (name == "CrowdDensity") {
            if (haveCrowd) return fail(error, r.line, "CrowdDensity given twice");
            haveCrowd = true;
            curve = &result.crowd;
        } else if (name == "Danger") {
            if (haveDanger) return fail(error, r.line, "Danger given twice");
            haveDanger = true;
            curve = &result.danger;
        } else {
            return fail(error, r.line, "unknown curve \"" + std::string(name) + "\"");
        }
        if (r.fields.size() != hours.size() + 1) return fail(error, r.line, "needs one value per header hour");
        for (std::size_t k = 0; k < hours.size(); ++k) {
            double v = 0.0;
            if (!parseNumber(trim(r.fields[k + 1]), v) || v < 0.0 || v > 1.0)
                return fail(error, r.line, "value \"" + r.fields[k + 1] + "\" is not a number in 0..1");
            curve->hours[k] = hours[k];
            curve->values[k] = static_cast<float>(v);
        }
        curve->count = static_cast<std::uint32_t>(hours.size());
    }
    if (!header) return fail(error, 1, "no header row");
    if (!haveCrowd) return fail(error, records.empty() ? 1 : records.back().line, "CrowdDensity is missing");
    if (!haveDanger) return fail(error, records.empty() ? 1 : records.back().line, "Danger is missing");
    out = result;
    return true;
}

}  // namespace ftcore
