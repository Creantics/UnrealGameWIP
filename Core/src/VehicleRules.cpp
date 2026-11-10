#include "ftcore/VehicleRules.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace ftcore {
namespace {

constexpr float kDeg = 3.14159265358979f / 180.f;
constexpr float kKmh = 100000.f / 3600.f;

int stageOf(float v) {
    if (std::isnan(v)) return 0;
    return static_cast<int>(std::floor(std::clamp(v, 0.f, static_cast<float>(kWearMaxStage))));
}

bool finite(float v) { return std::isfinite(v); }

// Wear that turned out not finite reads as clean.
void heal(WearState& s) {
    for (float& z : s.zone) z = finite(z) ? z : 0.f;
    s.dirt = finite(s.dirt) ? s.dirt : 0.f;
}

} // namespace

// Wear.

WearZone wearZoneAt(float lx, float ly) {
    if (std::abs(lx) >= std::abs(ly)) return lx >= 0.f ? WearZone::Front : WearZone::Rear;
    return ly >= 0.f ? WearZone::Right : WearZone::Left;
}

void addImpact(const WearTuning& t, WearState& s, float speed, float lx, float ly) {
    if (!finite(speed) || !finite(lx) || !finite(ly)) return;
    if (speed <= t.minImpact || t.impactPerStage <= 0.f) return;
    heal(s);
    float& z = s.zone[static_cast<int>(wearZoneAt(lx, ly))];
    z = std::min(z + (speed - t.minImpact) / t.impactPerStage, static_cast<float>(kWearMaxStage));
}

void addDistance(const WearTuning& t, WearState& s, float cm) {
    if (!finite(cm) || cm <= 0.f) return;
    heal(s);
    s.dirt = std::min(s.dirt + cm / 100000.f * t.dirtPerKm, static_cast<float>(kWearMaxStage));
}

int wearStage(const WearState& s, WearZone z) { return stageOf(s.zone[static_cast<int>(z)]); }

int dirtStage(const WearState& s) { return stageOf(s.dirt); }

// Repair.

int garageAt(const RepairTuning& t, float x, float y) {
    for (int i = 0; i < std::min(t.garageCount, kMaxGarages); ++i) {
        const Garage& g = t.garages[i];
        const float dx = x - g.x;
        const float dy = y - g.y;
        if (dx * dx + dy * dy <= g.radius * g.radius) return i;
    }
    return -1;
}

int repairQuote(const RepairTuning& t, const WearState& s) {
    int stages = 0;
    for (const float z : s.zone) stages += stageOf(z);
    return stages * t.pricePerStage;
}

RepairResult repair(const RepairTuning& t, WearState& s, int money) {
    RepairResult r;
    heal(s);
    for (;;) {
        int worst = -1;
        for (int i = 0; i < kWearZones; ++i) {
            if (stageOf(s.zone[i]) > 0 && (worst < 0 || stageOf(s.zone[i]) > stageOf(s.zone[worst]))) worst = i;
        }
        if (worst < 0 || money - r.spent < t.pricePerStage) break;
        s.zone[worst] = static_cast<float>(stageOf(s.zone[worst]) - 1);
        r.spent += t.pricePerStage;
        ++r.stagesFixed;
    }
    bool anyLeft = false;
    for (const float z : s.zone) anyLeft = anyLeft || stageOf(z) > 0;
    if (!anyLeft && r.stagesFixed > 0) {
        for (float& z : s.zone) z = 0.f; // the last scuffs polished out with a full repair
    }
    if (s.dirt > 0.f) {
        s.dirt = 0.f;
        r.washed = true;
    }
    if (r.stagesFixed > 0) r.minutes = t.minutes;
    return r;
}

// Seats.

bool tryEnter(const SeatTuning& t, PartySeats& p, float leadDistance, float vehicleSpeed, bool partyVehicle,
              const float* followerDistance, int followers) {
    if (p.phase != SeatPhase::OnFoot || !partyVehicle) return false;
    if (!finite(leadDistance) || !finite(vehicleSpeed)) return false;
    if (leadDistance > t.enterReach || std::abs(vehicleSpeed) > t.enterMaxSpeed) return false;
    if (followers < 0 || followers > kMaxParty - 1 || (followers > 0 && followerDistance == nullptr)) return false;
    PartySeats n;
    n.phase = SeatPhase::Entering;
    n.count = 1 + followers;
    n.boardAt[0] = t.enterTime;
    float last = t.enterTime;
    for (int i = 0; i < followers; ++i) {
        const float d = finite(followerDistance[i]) ? std::max(followerDistance[i], 0.f) : t.boardFar + 1.f;
        if (d > t.boardFar) {
            n.boardAt[1 + i] = -1.f; // placed in when the doors shut
        } else {
            n.boardAt[1 + i] = d / std::max(t.followerSpeed, 1.f) + t.enterTime;
            last = std::max(last, n.boardAt[1 + i]);
        }
    }
    n.length = std::max(t.enterTime, std::min(last, t.boardWait));
    p = n;
    return true;
}

void tickSeats(const SeatTuning& t, PartySeats& p, float dt, float vehicleSpeed, bool exitPressed, bool driverSideClear,
               bool passengerSideClear) {
    dt = finite(dt) ? std::max(dt, 0.f) : 0.f;
    vehicleSpeed = finite(vehicleSpeed) ? vehicleSpeed : 0.f;
    switch (p.phase) {
    case SeatPhase::OnFoot:
        return;
    case SeatPhase::Entering:
        p.time += dt;
        for (int i = 0; i < p.count; ++i) {
            if (!p.aboard[i] && p.boardAt[i] >= 0.f && p.time >= p.boardAt[i]) p.aboard[i] = true;
        }
        if (p.time >= p.length) {
            for (int i = 0; i < p.count; ++i) p.aboard[i] = true;
            p.phase = SeatPhase::Seated;
            p.time = 0.f;
        }
        return;
    case SeatPhase::Seated:
        p.exitHeld = exitPressed && std::abs(vehicleSpeed) <= t.exitMaxSpeed ? p.exitHeld + dt : 0.f;
        if (p.exitHeld >= t.exitHold) {
            p.exitHeld = 0.f;
            p.side = driverSideClear ? ExitSide::Driver : passengerSideClear ? ExitSide::Passenger : ExitSide::None;
            if (p.side != ExitSide::None) {
                p.phase = SeatPhase::Exiting;
                p.time = 0.f;
                p.length = t.exitTime;
            }
        }
        return;
    case SeatPhase::Exiting:
        p.time += dt;
        if (p.time >= p.length) {
            const ExitSide side = p.side;
            p = PartySeats{};
            p.side = side;
        }
        return;
    }
}

// Pedestrians.

void holdForSeats(const PartySeats& p, VehicleInput& in) {
    if (p.phase != SeatPhase::Entering && p.phase != SeatPhase::Exiting) return;
    in.throttle = 0.f;
    in.brake = 0.f; // a brake at rest would back it up
    in.steer = 0.f;
    in.handbrake = true;
}

float pedestrianCap(const PedestrianTuning& t, float ahead, float side) {
    if (ahead >= kNobodyAhead) return 0.f;
    // Anything else fails safe: a distance that is not a number is someone close, a side that is not one is in the path.
    if (!finite(ahead)) return t.nearCap;
    ahead = std::max(ahead, 0.f);
    const bool inPath = !finite(side) || std::abs(side) < t.halfWidth + t.clearance;
    const float a = std::max(t.brakeDecel, 1.f);
    const float past = std::max(ahead - t.nearDistance - t.brakeMargin, 0.f);
    float cap = std::sqrt(t.nearCap * t.nearCap + 2.f * a * past); // down to nearCap a margin before nearDistance
    if (inPath) {
        // In the path: stop short of them. Never 0, which would read as no limit.
        cap = std::min(cap, std::max(std::sqrt(2.f * a * std::max(ahead - t.stopGap, 0.f)), 1.f));
    }
    return cap;
}

float combinedLimit(float a, float b) {
    a = finite(a) && a > 0.f ? a : 0.f;
    b = finite(b) && b > 0.f ? b : 0.f;
    if (a <= 0.f) return b;
    if (b <= 0.f) return a;
    return std::min(a, b);
}

void applySpeedLimits(const PedestrianTuning& t, VehicleInput& in, float roadLimit, float ahead, float side) {
    const float cap = pedestrianCap(t, ahead, side);
    in.limit = combinedLimit(roadLimit, cap);
    in.limitDecel = cap > 0.f && in.limit >= cap ? t.brakeDecel : 0.f;
    in.clamp = cap > 0.f && !(ahead > t.nearDistance) ? t.nearCap : 0.f; // the hard clamp in the last metres
}

PedOutcome pedestrianReact(const PedestrianTuning& t, float speed, float ahead, float side) {
    PedOutcome o;
    if (!finite(speed) || !finite(ahead) || !finite(side)) return o;
    const float need = t.halfWidth + t.clearance - std::abs(side);
    if (need <= 0.f) return o; // clear of the body
    if (ahead < -t.clearance) return o; // beside the body already: only the front meets anyone
    ahead = std::max(ahead, 0.f);       // at the bumper (its corners included): met now
    // Inside the body's width they are in its path wherever they stand; in the clearance band only within the cone.
    const float out = std::abs(side) - t.halfWidth;
    if (out > 0.f && std::atan2(out, std::max(ahead, 1.f)) > t.coneAngle * kDeg) return o;
    float v = std::abs(speed);
    if (v < 1.f) return o;
    // Braking all the way: down to nearDistance, where the hard clamp holds it to nearCap, then on to them.
    const float a = std::max(t.brakeDecel, 1.f);
    float time = 0.f;
    float d = ahead;
    if (d > t.nearDistance) {
        const float v2 = v * v - 2.f * a * (d - t.nearDistance);
        const float v1 = v2 > 0.f ? std::sqrt(v2) : 0.f;
        time = (v - v1) / a;
        v = v1;
        d = t.nearDistance;
    }
    v = std::min(v, t.nearCap);
    const float left2 = v * v - 2.f * a * d; // speed squared when the front reaches them
    const float meet = left2 > 0.f && v > 0.f ? std::sqrt(left2) : 0.f;
    const float reach = meet > 0.f ? time + (v - meet) / a : 1e9f; // stops short: all the time they need
    const float dodge = t.reaction + need / std::max(t.sidestepSpeed, 1.f);
    if (reach >= dodge) {
        o.react = PedReact::Dodge;
        o.sidestep = side < 0.f ? -need : need;
    } else if (meet < t.stumbleBelow) {
        o.react = PedReact::Stumble;
        o.down = t.stumbleTime;
        o.contact = meet;
    } else {
        o.react = PedReact::KnockDown;
        o.down = t.downTime;
        o.contact = meet;
    }
    return o;
}

// The import.

namespace {

struct Reader {
    std::string* error;
    bool ok = true;

    void fail(std::string_view section, std::string_view name, const char* why) {
        if (!ok) return;
        ok = false;
        if (error) *error = std::string(section) + (name.empty() ? "" : ".") + std::string(name) + ": " + why;
    }
    void number(const JsonValue& obj, std::string_view sec, const char* name, float& v, double lo, double hi,
                double scale = 1.0) {
        if (!ok) return;
        const JsonValue m = obj[name];
        if (m.isAbsent()) return;
        if (!m.isNumber()) return fail(sec, name, "not a number");
        const double d = m.number();
        if (!(d >= lo && d <= hi)) return fail(sec, name, "out of range");
        v = static_cast<float>(d * scale);
    }
    void only(const JsonValue& obj, std::string_view sec, std::initializer_list<std::string_view> known) {
        for (std::uint32_t i = 0; ok && i < obj.size(); ++i) {
            const std::string_view k = obj.key(i);
            bool found = false;
            for (const std::string_view n : known) found = found || n == k;
            if (!found) fail(sec, k, "not a known member");
        }
    }
    JsonValue section(const JsonValue& root, const char* name) {
        const JsonValue s = root[name];
        if (ok && !s.isAbsent() && !s.isObject()) fail(name, "", "not an object");
        return s;
    }
};

} // namespace

bool importVehicleRules(std::string_view json, VehicleRules& out, std::string* error) {
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(json, &parseError)) {
        if (error) *error = parseError;
        return false;
    }
    const JsonValue root = doc.root();
    if (!root.isObject() || root["version"].integer(0) != 1) {
        if (error) *error = "not a version 1 object";
        return false;
    }
    VehicleRules r = out;
    Reader rd{error};
    rd.only(root, "", {"version", "provisional", "note", "wear", "repair", "seats", "pedestrian"});

    const JsonValue w = rd.section(root, "wear");
    rd.only(w, "wear", {"minImpact", "impactPerStage", "dirtPerKm"});
    rd.number(w, "wear", "minImpact", r.wear.minImpact, 0.0, 5000.0);
    rd.number(w, "wear", "impactPerStage", r.wear.impactPerStage, 1.0, 10000.0);
    rd.number(w, "wear", "dirtPerKm", r.wear.dirtPerKm, 0.0, 4.0);

    const JsonValue rp = rd.section(root, "repair");
    rd.only(rp, "repair", {"pricePerStage", "minutes", "garages"});
    float price = static_cast<float>(r.repair.pricePerStage);
    rd.number(rp, "repair", "pricePerStage", price, 0.0, 100000.0);
    r.repair.pricePerStage = static_cast<int>(std::lround(price));
    rd.number(rp, "repair", "minutes", r.repair.minutes, 0.0, 1440.0);
    const JsonValue gs = rp["garages"];
    if (rd.ok && !gs.isAbsent()) {
        if (!gs.isArray()) rd.fail("repair", "garages", "not an array");
        else if (gs.size() > static_cast<std::uint32_t>(kMaxGarages)) rd.fail("repair", "garages", "more than 8");
        else {
            r.repair.garageCount = 0;
            for (std::uint32_t i = 0; rd.ok && i < gs.size(); ++i) {
                const JsonValue g = gs[i];
                if (!g.isObject()) {
                    rd.fail("repair", "garages", "a row is not an object");
                    break;
                }
                Garage row;
                rd.only(g, "repair.garages", {"id", "x", "y", "radius", "where"});
                const std::string_view id = g["id"].string();
                if (rd.ok && (id.empty() || id.size() >= sizeof(row.id))) rd.fail("repair.garages", "id", "empty or too long");
                if (!rd.ok) break;
                std::copy(id.begin(), id.end(), row.id);
                if (rd.ok && (g["x"].isAbsent() || g["y"].isAbsent())) rd.fail("repair.garages", "x", "x and y needed");
                rd.number(g, "repair.garages", "x", row.x, -100000.0, 100000.0, 100.0);
                rd.number(g, "repair.garages", "y", row.y, -100000.0, 100000.0, 100.0);
                rd.number(g, "repair.garages", "radius", row.radius, 1.0, 100.0, 100.0);
                r.repair.garages[r.repair.garageCount++] = row;
            }
        }
    }

    const JsonValue st = rd.section(root, "seats");
    rd.only(st, "seats", {"enterReach", "enterMaxSpeed", "enterTime", "exitTime", "exitHold", "exitMaxSpeed",
                          "followerSpeed", "boardFar", "boardWait"});
    rd.number(st, "seats", "enterReach", r.seats.enterReach, 0.0, 2000.0);
    rd.number(st, "seats", "enterMaxSpeed", r.seats.enterMaxSpeed, 0.0, 1000.0);
    rd.number(st, "seats", "enterTime", r.seats.enterTime, 0.0, 10.0);
    rd.number(st, "seats", "exitTime", r.seats.exitTime, 0.0, 10.0);
    rd.number(st, "seats", "exitHold", r.seats.exitHold, 0.0, 5.0);
    rd.number(st, "seats", "exitMaxSpeed", r.seats.exitMaxSpeed, 0.0, 1000.0);
    rd.number(st, "seats", "followerSpeed", r.seats.followerSpeed, 1.0, 2000.0);
    rd.number(st, "seats", "boardFar", r.seats.boardFar, 0.0, 100000.0);
    rd.number(st, "seats", "boardWait", r.seats.boardWait, 0.0, 30.0);

    const JsonValue pd = rd.section(root, "pedestrian");
    rd.only(pd, "pedestrian", {"nearDistance", "nearCapKmh", "coneAngle", "reaction", "sidestepSpeed", "halfWidth",
                               "clearance", "stumbleBelowKmh", "stumbleTime", "downTime", "brakeDecel", "stopGap", "brakeMargin"});
    rd.number(pd, "pedestrian", "nearDistance", r.pedestrian.nearDistance, 0.0, 10000.0);
    rd.number(pd, "pedestrian", "nearCapKmh", r.pedestrian.nearCap, 1.0, 60.0, kKmh);
    rd.number(pd, "pedestrian", "coneAngle", r.pedestrian.coneAngle, 0.0, 90.0);
    rd.number(pd, "pedestrian", "reaction", r.pedestrian.reaction, 0.0, 5.0);
    rd.number(pd, "pedestrian", "sidestepSpeed", r.pedestrian.sidestepSpeed, 1.0, 2000.0);
    rd.number(pd, "pedestrian", "halfWidth", r.pedestrian.halfWidth, 0.0, 500.0);
    rd.number(pd, "pedestrian", "clearance", r.pedestrian.clearance, 0.0, 500.0);
    rd.number(pd, "pedestrian", "stumbleBelowKmh", r.pedestrian.stumbleBelow, 0.0, 60.0, kKmh);
    rd.number(pd, "pedestrian", "stumbleTime", r.pedestrian.stumbleTime, 0.0, 10.0);
    rd.number(pd, "pedestrian", "downTime", r.pedestrian.downTime, 0.0, 30.0);
    rd.number(pd, "pedestrian", "brakeDecel", r.pedestrian.brakeDecel, 100.0, 5000.0);
    rd.number(pd, "pedestrian", "stopGap", r.pedestrian.stopGap, 0.0, 1000.0);
    rd.number(pd, "pedestrian", "brakeMargin", r.pedestrian.brakeMargin, 0.0, 1000.0);
    if (rd.ok && r.pedestrian.stumbleBelow > r.pedestrian.nearCap) rd.fail("pedestrian", "stumbleBelowKmh", "above nearCapKmh");

    if (!rd.ok) return false;
    out = r;
    return true;
}

} // namespace ftcore
