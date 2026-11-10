#include "ftcore/Crowd.h"

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
        if (error) *error = std::string(section) + (name.empty() ? "" : ".") + std::string(name) + ": " + why;
    }
    void number(const JsonValue& obj, std::string_view section, const char* name, float& v, double lo, double hi) {
        if (!ok) return;
        const JsonValue m = obj[name];
        if (m.isAbsent()) return;
        if (!m.isNumber()) return fail(section, name, "not a number");
        const double d = m.number();
        if (!(d >= lo && d <= hi)) return fail(section, name, "out of range");
        v = static_cast<float>(d);
    }
    void count(const JsonValue& obj, std::string_view section, const char* name, std::uint32_t& v, std::int64_t lo,
               std::int64_t hi) {
        if (!ok) return;
        const JsonValue m = obj[name];
        if (m.isAbsent()) return;
        if (!m.isInteger()) return fail(section, name, "not an integer");
        const std::int64_t d = m.integer();
        if (d < lo || d > hi) return fail(section, name, "out of range");
        v = static_cast<std::uint32_t>(d);
    }
    void only(const JsonValue& obj, std::string_view section, std::initializer_list<std::string_view> known) {
        for (std::uint32_t i = 0; ok && i < obj.size(); ++i) {
            const std::string_view k = obj.key(i);
            bool found = false;
            for (const std::string_view n : known) found = found || n == k;
            if (!found) fail(section, k, "not a known tunable");
        }
    }
    JsonValue section(const JsonValue& root, const char* name) {
        const JsonValue s = root[name];
        if (ok && !s.isAbsent() && !s.isObject()) fail(name, "", "not an object");
        return s;
    }
};

} // namespace

bool importCrowdTuning(std::string_view json, CrowdTuning& out, std::string* error) {
    JsonDocument doc;
    if (!doc.parse(json, error)) return false;
    const JsonValue root = doc.root();
    Reader rd{error};
    if (!root.isObject()) {
        rd.fail("crowd", "", "not an object");
        return false;
    }
    CrowdTuning t = out;
    rd.only(root, "", {"version", "population", "move", "perceive", "events", "react", "flee", "spread", "step", "graph"});

    JsonValue s = rd.section(root, "population");
    rd.only(s, "population", {"maxAgents", "targetAgents", "maxDrawn", "spawnMin", "spawnMax", "despawnDist", "fillMin", "spawnPerStep"});
    rd.count(s, "population", "maxAgents", t.maxAgents, 1, 4096);
    rd.count(s, "population", "targetAgents", t.targetAgents, 0, 4096);
    rd.count(s, "population", "maxDrawn", t.maxDrawn, 0, 4096);
    rd.number(s, "population", "spawnMin", t.spawnMin, 0.0, 2000.0);
    rd.number(s, "population", "spawnMax", t.spawnMax, 1.0, 2000.0);
    rd.number(s, "population", "despawnDist", t.despawnDist, 1.0, 2000.0);
    rd.number(s, "population", "fillMin", t.fillMin, 0.0, 2000.0);
    rd.count(s, "population", "spawnPerStep", t.spawnPerStep, 0, 256);
    if (rd.ok && t.targetAgents > t.maxAgents) rd.fail("population", "targetAgents", "above maxAgents");
    if (rd.ok && !(t.spawnMin < t.spawnMax && t.spawnMax < t.despawnDist)) rd.fail("population", "spawnMax", "needs spawnMin < spawnMax < despawnDist");

    s = rd.section(root, "move");
    rd.only(s, "move", {"walkSpeed", "walkSpeedJitter", "runSpeed", "backSpeed", "turnRate", "laneSpeed", "idleChance", "idleMin",
                        "idleMax", "headingKeep", "headingChange", "avoidRadius", "avoidPlayer", "minGap"});
    rd.number(s, "move", "walkSpeed", t.walkSpeed, 0.1, 5.0);
    rd.number(s, "move", "walkSpeedJitter", t.walkSpeedJitter, 0.0, 2.0);
    rd.number(s, "move", "runSpeed", t.runSpeed, 0.1, 12.0);
    rd.number(s, "move", "backSpeed", t.backSpeed, 0.0, 5.0);
    rd.number(s, "move", "turnRate", t.turnRate, 1.0, 2000.0);
    rd.number(s, "move", "laneSpeed", t.laneSpeed, 0.0, 10.0);
    rd.number(s, "move", "idleChance", t.idleChance, 0.0, 1.0);
    rd.number(s, "move", "idleMin", t.idleMin, 0.0, 120.0);
    rd.number(s, "move", "idleMax", t.idleMax, 0.0, 120.0);
    rd.number(s, "move", "headingKeep", t.headingKeep, 0.0, 1.0);
    rd.number(s, "move", "headingChange", t.headingChange, 0.0, 1.0);
    rd.number(s, "move", "avoidRadius", t.avoidRadius, 0.0, 10.0);
    rd.number(s, "move", "avoidPlayer", t.avoidPlayer, 0.0, 20.0);
    rd.number(s, "move", "minGap", t.minGap, 0.0, 5.0);
    if (rd.ok && t.idleMin > t.idleMax) rd.fail("move", "idleMin", "above idleMax");

    s = rd.section(root, "perceive");
    rd.only(s, "perceive", {"fov", "feelRadius", "reactBase", "reactPerMetre", "reactJitter", "occludedHearing", "occlusionBudget",
                            "reactRadius", "reactCloseMax"});
    rd.number(s, "perceive", "fov", t.fov, 0.0, 360.0);
    rd.number(s, "perceive", "feelRadius", t.feelRadius, 0.0, 50.0);
    rd.number(s, "perceive", "reactBase", t.reactBase, 0.0, 10.0);
    rd.number(s, "perceive", "reactPerMetre", t.reactPerMetre, 0.0, 1.0);
    rd.number(s, "perceive", "reactJitter", t.reactJitter, 0.0, 10.0);
    rd.number(s, "perceive", "occludedHearing", t.occludedHearing, 0.0, 1.0);
    rd.number(s, "perceive", "reactRadius", t.reactRadius, 0.0, 500.0);
    rd.number(s, "perceive", "reactCloseMax", t.reactCloseMax, 0.0, 10.0);
    rd.count(s, "perceive", "occlusionBudget", t.occlusionBudget, 0, 100000);

    s = rd.section(root, "events");
    for (std::uint32_t i = 0; rd.ok && i < s.size(); ++i) {
        const std::string_view name = s.key(i);
        std::size_t kind = static_cast<std::size_t>(CrowdEventKind::Panic);
        for (std::size_t k = 0; k < static_cast<std::size_t>(CrowdEventKind::Panic); ++k)
            if (crowdEventName(static_cast<CrowdEventKind>(k)) == name) kind = k;
        if (kind == static_cast<std::size_t>(CrowdEventKind::Panic)) {
            rd.fail("events", name, "not a known event");
            break;
        }
        const JsonValue e = s.value(i);
        if (!e.isObject()) {
            rd.fail("events", name, "not an object");
            break;
        }
        rd.only(e, "events", {"radius", "intensity", "sightOnly"});
        CrowdEventRule& r = t.events[kind];
        rd.number(e, "events", "radius", r.radius, 0.0, 1000.0);
        rd.number(e, "events", "intensity", r.intensity, 0.0, 10.0);
        const JsonValue so = e["sightOnly"];
        if (!so.isAbsent()) {
            if (!so.isBool()) rd.fail("events", "sightOnly", "not a bool");
            else r.sightOnly = so.boolean();
        }
    }

    s = rd.section(root, "react");
    rd.only(s, "react", {"noticeFear", "backFear", "fleeFear", "fearMax", "fearDecay", "cowerDecay", "calmFear", "noticeTime", "backTime",
                         "calmTime", "recoverTime", "threatMemory", "quietAfter", "quietDecay", "returnChance", "timidScale", "boldScale"});
    rd.number(s, "react", "noticeFear", t.noticeFear, 0.0, 10.0);
    rd.number(s, "react", "backFear", t.backFear, 0.0, 10.0);
    rd.number(s, "react", "fleeFear", t.fleeFear, 0.0, 10.0);
    rd.number(s, "react", "fearMax", t.fearMax, 0.01, 10.0);
    rd.number(s, "react", "fearDecay", t.fearDecay, 0.0, 10.0);
    rd.number(s, "react", "cowerDecay", t.cowerDecay, 0.0, 10.0);
    rd.number(s, "react", "calmFear", t.calmFear, 0.0, 10.0);
    rd.number(s, "react", "noticeTime", t.noticeTime, 0.0, 60.0);
    rd.number(s, "react", "backTime", t.backTime, 0.0, 60.0);
    rd.number(s, "react", "calmTime", t.calmTime, 0.0, 600.0);
    rd.number(s, "react", "recoverTime", t.recoverTime, 0.0, 600.0);
    rd.number(s, "react", "threatMemory", t.threatMemory, 0.0, 3600.0);
    rd.number(s, "react", "quietAfter", t.quietAfter, 0.0, 600.0);
    rd.number(s, "react", "quietDecay", t.quietDecay, 0.0, 10.0);
    rd.number(s, "react", "returnChance", t.returnChance, 0.0, 1.0);
    rd.number(s, "react", "timidScale", t.timidScale, 0.01, 10.0);
    rd.number(s, "react", "boldScale", t.boldScale, 0.01, 10.0);
    if (rd.ok && !(t.noticeFear < t.backFear && t.backFear < t.fleeFear)) rd.fail("react", "backFear", "needs noticeFear < backFear < fleeFear");

    s = rd.section(root, "flee");
    rd.only(s, "flee", {"safeDist", "threatPenaltyRadius", "threatPenalty", "blockRadius", "searchLimit", "plansPerStep", "cowerRetry",
                        "resumeHeading"});
    rd.number(s, "flee", "safeDist", t.safeDist, 1.0, 2000.0);
    rd.number(s, "flee", "resumeHeading", t.resumeHeading, 0.0, 1.0);
    rd.number(s, "flee", "threatPenaltyRadius", t.threatPenaltyRadius, 0.0, 2000.0);
    rd.number(s, "flee", "threatPenalty", t.threatPenalty, 0.0, 1000.0);
    rd.number(s, "flee", "blockRadius", t.blockRadius, 0.0, 500.0);
    rd.count(s, "flee", "searchLimit", t.searchLimit, 1, 10000000);
    rd.count(s, "flee", "plansPerStep", t.plansPerStep, 0, 4096);
    rd.number(s, "flee", "cowerRetry", t.cowerRetry, 0.0, 60.0);

    s = rd.section(root, "spread");
    rd.only(s, "spread", {"radius", "gain", "interval"});
    rd.number(s, "spread", "radius", t.spreadRadius, 0.0, 100.0);
    rd.number(s, "spread", "gain", t.spreadGain, 0.0, 0.99);
    rd.number(s, "spread", "interval", t.spreadInterval, 0.01, 60.0);

    s = rd.section(root, "step");
    rd.only(s, "step", {"rate", "maxSteps"});
    float rate = 1.f / t.step;
    rd.number(s, "step", "rate", rate, 1.0, 240.0);
    t.step = 1.f / rate;
    rd.count(s, "step", "maxSteps", t.maxSteps, 1, 64);

    s = rd.section(root, "graph");
    rd.only(s, "graph", {"snap", "junctionReach", "edgeMargin", "wallInset", "minComponent", "walkKinds"});
    rd.number(s, "graph", "snap", t.graph.snap, 0.01, 50.0);
    rd.number(s, "graph", "junctionReach", t.graph.junctionReach, 0.0, 50.0);
    rd.number(s, "graph", "edgeMargin", t.graph.edgeMargin, 0.0, 50.0);
    rd.number(s, "graph", "wallInset", t.graph.wallInset, 0.0, 50.0);
    rd.count(s, "graph", "minComponent", t.graph.minComponent, 1, 1000000);
    const JsonValue kinds = s["walkKinds"];
    if (rd.ok && !kinds.isAbsent()) {
        if (!kinds.isArray()) rd.fail("graph", "walkKinds", "not an array");
        else {
            t.graph.walkKinds.clear();
            for (std::uint32_t i = 0; rd.ok && i < kinds.size(); ++i) {
                if (!kinds[i].isString()) rd.fail("graph", "walkKinds", "not a list of strings");
                else t.graph.walkKinds.emplace_back(kinds[i].string());
            }
        }
    }

    if (!rd.ok) return false;
    out = t;
    return true;
}

} // namespace ftcore
