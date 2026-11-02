#include "ftcore/MoveTuning.h"

#include "ftcore/Json.h"

#include <initializer_list>

namespace ftcore {
namespace {

struct Reader {
    std::string* error;
    bool ok = true;

    void fail(std::string_view section, std::string_view name, const char* why) {
        if (!ok) return;
        ok = false;
        if (error) *error = (section.empty() ? std::string() : std::string(section) + ".") + std::string(name) + ": " + why;
    }
    // A number member; absent keeps `v`. Must lie in [lo, hi].
    void number(const JsonValue& obj, std::string_view section, const char* name, float& v, double lo, double hi) {
        if (!ok) return;
        const JsonValue m = obj[name];
        if (m.isAbsent()) return;
        if (!m.isNumber()) return fail(section, name, "not a number");
        const double d = m.number();
        if (!(d >= lo && d <= hi)) return fail(section, name, "out of range");
        v = static_cast<float>(d);
    }
    // Refuses any member of `obj` not in `known`.
    void only(const JsonValue& obj, std::string_view section, std::initializer_list<std::string_view> known) {
        for (std::uint32_t i = 0; ok && i < obj.size(); ++i) {
            const std::string_view k = obj.key(i);
            bool found = false;
            for (const std::string_view n : known) found = found || n == k;
            if (!found) fail(section, k, "not a known tunable");
        }
    }
    // The section `name`: absent reads as an empty object; anything else but an object is refused.
    JsonValue section(const JsonValue& root, const char* name) {
        const JsonValue s = root[name];
        if (ok && !s.isAbsent() && !s.isObject()) fail(name, "", "not an object");
        return s;
    }
};

void readSlide(Reader& rd, const JsonValue& s, SlideTuning& t) {
    constexpr std::string_view n = "slide";
    rd.only(s, n, {"minEntrySpeed", "friction", "gravity", "outSpeed", "maxSpeed", "steerRate", "exitAfter", "speedCost",
                   "footCost", "minEntryRate", "maxEntryRate", "jumpFrom", "jumpCarry", "jumpMaxSpeed"});
    rd.number(s, n, "minEntrySpeed", t.minEntrySpeed, 0.0, 2000.0);
    rd.number(s, n, "friction", t.friction, 0.0, 5000.0);
    rd.number(s, n, "gravity", t.gravity, 0.0, 5000.0);
    rd.number(s, n, "outSpeed", t.outSpeed, 0.0, 2000.0);
    rd.number(s, n, "maxSpeed", t.maxSpeed, 0.0, 3000.0);
    rd.number(s, n, "steerRate", t.steerRate, 0.0, 720.0);
    rd.number(s, n, "exitAfter", t.exitAfter, 0.0, 5.0);
    rd.number(s, n, "speedCost", t.speedCost, 0.0, 1.0);
    rd.number(s, n, "footCost", t.footCost, 0.0, 10.0);
    rd.number(s, n, "minEntryRate", t.minEntryRate, 0.1, 5.0);
    rd.number(s, n, "maxEntryRate", t.maxEntryRate, 0.1, 5.0);
    rd.number(s, n, "jumpFrom", t.jumpFrom, 0.0, 5.0);
    rd.number(s, n, "jumpCarry", t.jumpCarry, 0.0, 2.0);
    rd.number(s, n, "jumpMaxSpeed", t.jumpMaxSpeed, 0.0, 3000.0);
    if (rd.ok && t.minEntryRate > t.maxEntryRate) rd.fail(n, "minEntryRate", "above maxEntryRate");
}

void readTraversal(Reader& rd, const JsonValue& s, TraversalTuning& t) {
    constexpr std::string_view n = "traversal";
    rd.only(s, n, {"standDepth", "heightSlack", "minHeightSlack", "maxDrop", "minLead", "maxPush", "maxPull", "heightCost",
                   "speedCost", "footCost", "exitAfter", "minEntryRate", "maxEntryRate", "rollMinSpeed", "rollMaxRate",
                   "rollTurnOff", "minCatchLead", "maxCatchLead", "maxCatchMiss", "maxCatchStretch", "catchMissCost"});
    rd.number(s, n, "standDepth", t.standDepth, 0.0, 1000.0);
    rd.number(s, n, "heightSlack", t.heightSlack, 0.0, 2.0);
    rd.number(s, n, "minHeightSlack", t.minHeightSlack, 0.0, 500.0);
    rd.number(s, n, "maxDrop", t.maxDrop, 0.0, 2000.0);
    rd.number(s, n, "minLead", t.minLead, 0.0, 2.0);
    rd.number(s, n, "maxPush", t.maxPush, 0.0, 500.0);
    rd.number(s, n, "maxPull", t.maxPull, 0.0, 500.0);
    rd.number(s, n, "heightCost", t.heightCost, 0.0, 1.0);
    rd.number(s, n, "speedCost", t.speedCost, 0.0, 1.0);
    rd.number(s, n, "footCost", t.footCost, 0.0, 10.0);
    rd.number(s, n, "exitAfter", t.exitAfter, 0.0, 5.0);
    rd.number(s, n, "minEntryRate", t.minEntryRate, 0.1, 5.0);
    rd.number(s, n, "maxEntryRate", t.maxEntryRate, 0.1, 5.0);
    rd.number(s, n, "rollMinSpeed", t.rollMinSpeed, 0.0, 2000.0);
    rd.number(s, n, "rollMaxRate", t.rollMaxRate, 0.1, 10.0);
    rd.number(s, n, "rollTurnOff", t.rollTurnOff, 0.0, 180.0);
    rd.number(s, n, "minCatchLead", t.minCatchLead, 0.0, 2.0);
    rd.number(s, n, "maxCatchLead", t.maxCatchLead, 0.0, 2.0);
    rd.number(s, n, "maxCatchMiss", t.maxCatchMiss, 0.0, 500.0);
    rd.number(s, n, "maxCatchStretch", t.maxCatchStretch, 0.0, 500.0);
    rd.number(s, n, "catchMissCost", t.catchMissCost, 0.0, 1.0);
    if (rd.ok && t.minEntryRate > t.maxEntryRate) rd.fail(n, "minEntryRate", "above maxEntryRate");
    if (rd.ok && t.minCatchLead > t.maxCatchLead) rd.fail(n, "minCatchLead", "above maxCatchLead");
}

void readLanding(Reader& rd, const JsonValue& s, LandingTuning& t) {
    constexpr std::string_view n = "landing";
    rd.only(s, n, {"rollMinAir", "rollDrop", "pressWindow"});
    rd.number(s, n, "rollMinAir", t.rollMinAir, 0.0, 5.0);
    rd.number(s, n, "rollDrop", t.rollDrop, 0.0, 10000.0);
    rd.number(s, n, "pressWindow", t.pressWindow, 0.0, 2.0);
}

void readDodge(Reader& rd, const JsonValue& s, DodgeTuning& t) {
    constexpr std::string_view n = "dodge";
    rd.only(s, n, {"length", "invulnStart", "invulnEnd", "cooldown", "staminaCost", "staminaMax", "staminaRegen",
                   "regenDelay", "longFrom", "shortSpeed", "longSpeed", "tiredSpeed", "tiredInvuln", "needsDirection"});
    rd.number(s, n, "length", t.length, 0.05, 5.0);
    rd.number(s, n, "invulnStart", t.invulnStart, 0.0, 5.0);
    rd.number(s, n, "invulnEnd", t.invulnEnd, 0.0, 5.0);
    rd.number(s, n, "cooldown", t.cooldown, 0.0, 10.0);
    rd.number(s, n, "staminaCost", t.staminaCost, 0.0, 1000.0);
    rd.number(s, n, "staminaMax", t.staminaMax, 0.0, 1000.0);
    rd.number(s, n, "staminaRegen", t.staminaRegen, 0.0, 1000.0);
    rd.number(s, n, "regenDelay", t.regenDelay, 0.0, 10.0);
    rd.number(s, n, "longFrom", t.longFrom, 0.0, 2000.0);
    rd.number(s, n, "shortSpeed", t.shortSpeed, 0.0, 2000.0);
    rd.number(s, n, "longSpeed", t.longSpeed, 0.0, 2000.0);
    rd.number(s, n, "tiredSpeed", t.tiredSpeed, 0.0, 2000.0);
    rd.number(s, n, "tiredInvuln", t.tiredInvuln, 0.0, 1.0);
    rd.number(s, n, "needsDirection", t.needsDirection, 0.0, 1.0);
    if (rd.ok && t.invulnStart > t.invulnEnd) rd.fail(n, "invulnStart", "above invulnEnd");
    if (rd.ok && t.staminaCost > t.staminaMax) rd.fail(n, "staminaCost", "above staminaMax");
}

} // namespace

bool importMoveTuning(std::string_view json, MoveTuning& out, std::string* error) {
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(json, &parseError)) {
        if (error) *error = parseError;
        return false;
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        if (error) *error = "the file is not an object";
        return false;
    }
    if (root["version"].integer(0) != 1) {
        if (error) *error = "version is not 1";
        return false;
    }
    MoveTuning t = out;
    Reader rd{error};
    rd.only(root, "", {"version", "slide", "traversal", "landing", "dodge"});
    readSlide(rd, rd.section(root, "slide"), t.slide);
    readTraversal(rd, rd.section(root, "traversal"), t.traversal);
    readLanding(rd, rd.section(root, "landing"), t.landing);
    readDodge(rd, rd.section(root, "dodge"), t.dodge);
    if (!rd.ok) return false;
    out = t;
    return true;
}

} // namespace ftcore
