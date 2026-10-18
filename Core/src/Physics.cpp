#include "ftcore/Physics.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace ftcore {

// ---- names ---------------------------------------------------------------------------------------------

namespace {

constexpr std::string_view kSurfaceNames[] = {"default", "wood", "stone", "metal", "cloth", "rubber", "flesh"};
static_assert(sizeof(kSurfaceNames) / sizeof(kSurfaceNames[0]) == static_cast<std::size_t>(PhysSurface::Count));
constexpr std::string_view kCombineNames[] = {"default", "average", "min", "multiply", "max"};

bool fail(std::string* error, std::string msg) {
    if (error) *error = std::move(msg);
    return false;
}

std::string str(std::string_view s) { return std::string(s); }

// Typed optional reads: a missing key leaves `out` alone; a wrong type sets `err` ("key: expected number").
bool getNum(JsonValue j, const char* key, float& out, std::string& err) {
    const JsonValue v = j[key];
    if (v.isAbsent()) return true;
    if (!v.isNumber()) return err = std::string(key) + ": expected number", false;
    out = static_cast<float>(v.number());
    return true;
}
bool getInt(JsonValue j, const char* key, int& out, std::string& err) {
    const JsonValue v = j[key];
    if (v.isAbsent()) return true;
    if (!v.isInteger()) return err = std::string(key) + ": expected integer", false;
    const std::int64_t i = v.integer();
    if (i < -1000000 || i > 1000000) return err = std::string(key) + ": out of range", false;
    out = static_cast<int>(i);
    return true;
}
bool getBool(JsonValue j, const char* key, bool& out, std::string& err) {
    const JsonValue v = j[key];
    if (v.isAbsent()) return true;
    if (!v.isBool()) return err = std::string(key) + ": expected boolean", false;
    out = v.boolean();
    return true;
}
bool getStr(JsonValue j, const char* key, std::string& out, std::string& err) {
    const JsonValue v = j[key];
    if (v.isAbsent()) return true;
    if (!v.isString()) return err = std::string(key) + ": expected string", false;
    out = str(v.string());
    return true;
}
// A string or an array of strings.
bool getNames(JsonValue j, const char* key, std::vector<std::string>& out, std::string& err) {
    const JsonValue v = j[key];
    if (v.isAbsent()) return true;
    out.clear();
    if (v.isString()) {
        out.push_back(str(v.string()));
        return true;
    }
    if (!v.isArray()) return err = std::string(key) + ": expected string or array of strings", false;
    for (std::uint32_t i = 0; i < v.size(); ++i) {
        if (!v[i].isString()) return err = std::string(key) + ": expected string or array of strings", false;
        out.push_back(str(v[i].string()));
    }
    return true;
}

bool finite(float v) { return std::isfinite(v); }

bool readLimits(JsonValue j, RagdollJointLimits& l, std::string& err) {
    if (!j.isObject()) return err = "expected object", false;
    std::string type;
    if (!getStr(j, "type", type, err)) return false;
    if (!type.empty()) {
        if (type == "hinge") l.hinge = true;
        else if (type == "swingTwist") l.hinge = false;
        else return err = "type: expected \"hinge\" or \"swingTwist\"", false;
    }
    if (!(getNum(j, "minAngle", l.minAngle, err) && getNum(j, "maxAngle", l.maxAngle, err) &&
          getNum(j, "normalHalfCone", l.normalHalfCone, err) && getNum(j, "planeHalfCone", l.planeHalfCone, err) &&
          getNum(j, "twistMin", l.twistMin, err) && getNum(j, "twistMax", l.twistMax, err)))
        return false;
    if (l.hinge && l.minAngle > l.maxAngle) return err = "minAngle: must not exceed maxAngle", false;
    if (l.twistMin > l.twistMax) return err = "twistMin: must not exceed twistMax", false;
    if (l.normalHalfCone < 0.f || l.planeHalfCone < 0.f || l.normalHalfCone > kPhysPi || l.planeHalfCone > kPhysPi)
        return err = "normalHalfCone/planeHalfCone: expected 0..pi", false;
    if (std::fabs(l.minAngle) > kPhysPi || std::fabs(l.maxAngle) > kPhysPi || std::fabs(l.twistMin) > kPhysPi ||
        std::fabs(l.twistMax) > kPhysPi)
        return err = "angles: expected -pi..pi", false;
    return true;
}

bool parseRoot(std::string_view json, JsonDocument& doc, const char* what, std::string* error) {
    std::string parseError;
    if (!doc.parse(json, &parseError)) return fail(error, parseError);
    if (!doc.root().isObject()) return fail(error, std::string(what) + ": expected object");
    return true;
}

bool parseCombine(JsonValue j, const char* key, PhysCombine& out, std::string& err) {
    std::string s;
    if (!getStr(j, key, s, err)) return false;
    if (s.empty()) return true;
    for (std::size_t i = 0; i < sizeof(kCombineNames) / sizeof(kCombineNames[0]); ++i)
        if (s == kCombineNames[i]) {
            out = static_cast<PhysCombine>(i);
            return true;
        }
    err = std::string(key) + ": expected default, average, min, multiply or max";
    return false;
}

} // namespace

std::string_view physSurfaceName(PhysSurface surface) {
    const auto i = static_cast<std::size_t>(surface);
    return i < static_cast<std::size_t>(PhysSurface::Count) ? kSurfaceNames[i] : kSurfaceNames[0];
}

std::string_view physCombineName(PhysCombine combine) {
    const auto i = static_cast<std::size_t>(combine);
    return i < sizeof(kCombineNames) / sizeof(kCombineNames[0]) ? kCombineNames[i] : kCombineNames[0];
}

// ---- physical materials ---------------------------------------------------------------------------------

int PhysMaterialSet::find(std::string_view name) const {
    for (std::size_t i = 0; i < materials.size(); ++i)
        if (materials[i].name == name) return static_cast<int>(i);
    return -1;
}

bool importPhysMaterials(std::string_view json, PhysMaterialSet& out, std::string* error,
                         std::vector<std::string>* warnings) {
    out = PhysMaterialSet{};
    JsonDocument doc;
    if (!parseRoot(json, doc, "materials file", error)) return false;
    const JsonValue root = doc.root();
    std::vector<std::string> errors;
    for (std::uint32_t k = 0; k < root.size(); ++k) {
        const std::string_view key = root.key(k);
        if (key != "version" && key != "materials" && warnings) warnings->push_back("unknown key: " + str(key));
    }
    if (const JsonValue v = root["version"]; !v.isAbsent() && !(v.isInteger() && v.integer() == 1))
        errors.push_back("version: expected 1");
    const JsonValue list = root["materials"];
    if (!list.isArray() || list.size() == 0) errors.push_back("materials: expected a non-empty array");
    PhysMaterialSet set;
    for (std::uint32_t i = 0; i < list.size(); ++i) {
        const JsonValue m = list[i];
        const std::string path = "materials[" + std::to_string(i) + "]";
        if (!m.isObject()) {
            errors.push_back(path + ": expected object");
            continue;
        }
        static constexpr std::string_view kKeys[] = {"name", "surface", "friction", "staticFriction", "restitution",
                                                     "density", "frictionCombine", "restitutionCombine"};
        for (std::uint32_t k = 0; k < m.size(); ++k) {
            const std::string_view key = m.key(k);
            if (std::find(std::begin(kKeys), std::end(kKeys), key) == std::end(kKeys) && warnings)
                warnings->push_back(path + ": unknown key: " + str(key));
        }
        PhysMaterial p;
        std::string err, surface;
        if (!getStr(m, "name", p.name, err) || !getStr(m, "surface", surface, err) ||
            !getNum(m, "friction", p.friction, err) || !getNum(m, "staticFriction", p.staticFriction, err) ||
            !getNum(m, "restitution", p.restitution, err) || !getNum(m, "density", p.density, err) ||
            !parseCombine(m, "frictionCombine", p.frictionCombine, err) ||
            !parseCombine(m, "restitutionCombine", p.restitutionCombine, err)) {
            errors.push_back(path + "." + err);
            continue;
        }
        bool nameOk = !p.name.empty();
        for (char c : p.name)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) nameOk = false;
        if (!nameOk) errors.push_back(path + ".name: expected lower-case letters, digits and _");
        else if (set.find(p.name) >= 0) errors.push_back(path + ".name: duplicate \"" + p.name + "\"");
        if (!surface.empty()) {
            bool found = false;
            for (std::size_t s = 0; s < static_cast<std::size_t>(PhysSurface::Count); ++s)
                if (surface == kSurfaceNames[s]) {
                    p.surface = static_cast<PhysSurface>(s);
                    found = true;
                }
            if (!found) errors.push_back(path + ".surface: unknown surface \"" + surface + "\"");
        }
        if (!finite(p.friction) || p.friction < 0.f || p.friction > 4.f)
            errors.push_back(path + ".friction: expected a number in 0..4");
        if (m.has("staticFriction") && (!finite(p.staticFriction) || p.staticFriction < 0.f || p.staticFriction > 4.f))
            errors.push_back(path + ".staticFriction: expected a number in 0..4");
        if (!finite(p.restitution) || p.restitution < 0.f || p.restitution > 1.f)
            errors.push_back(path + ".restitution: expected a number in 0..1");
        if (!finite(p.density) || p.density < 1.f || p.density > 25000.f)
            errors.push_back(path + ".density: expected kg/m^3 in 1..25000");
        set.materials.push_back(std::move(p));
    }
    if (!errors.empty()) {
        std::string joined;
        for (const std::string& e : errors) joined += (joined.empty() ? "" : "\n") + e;
        return fail(error, joined);
    }
    out = std::move(set);
    return true;
}

std::string physMaterialAssetName(std::string_view name) {
    std::string out = "PM_";
    out += name;
    if (out.size() > 3 && out[3] >= 'a' && out[3] <= 'z') out[3] = static_cast<char>(out[3] - 'a' + 'A');
    return out;
}

// ---- ragdoll data -----------------------------------------------------------------------------------------

int RagdollMap::find(std::string_view part) const {
    for (std::size_t i = 0; i < parts.size(); ++i)
        if (parts[i].name == part) return static_cast<int>(i);
    return -1;
}

const RagdollJointLimits& RagdollProfile::limitsFor(std::string_view part) const {
    for (const auto& [partName, l] : limits)
        if (partName == part) return l;
    return defaultLimits;
}

bool importRagdollMap(std::string_view json, RagdollMap& out, std::string* error) {
    out = RagdollMap{};
    JsonDocument doc;
    if (!parseRoot(json, doc, "ragdoll map", error)) return false;
    const JsonValue root = doc.root();
    RagdollMap map;
    std::string err;
    if (!getStr(root, "name", map.name, err) || !getNum(root, "totalMass", map.totalMass, err)) return fail(error, err);
    if (!(map.totalMass > 0.f) || !finite(map.totalMass)) return fail(error, "totalMass: must be > 0");
    const JsonValue parts = root["parts"];
    if (!parts.isArray() || parts.size() == 0) return fail(error, "ragdoll map: \"parts\" must be a non-empty array");
    for (std::uint32_t i = 0; i < parts.size(); ++i) {
        const JsonValue pj = parts[i];
        if (!pj.isObject()) return fail(error, "parts[]: expected object");
        RagdollPartDef p;
        if (!getStr(pj, "name", p.name, err) || !getNames(pj, "joints", p.joints, err) ||
            !getStr(pj, "parent", p.parent, err) || !getNames(pj, "tip", p.tip, err) ||
            !getNum(pj, "length", p.length, err) || !getNum(pj, "radius", p.radius, err) ||
            !getNum(pj, "mass", p.massFraction, err) || !getBool(pj, "upper", p.upper, err) ||
            !getBool(pj, "optional", p.optional, err))
            return fail(error, "parts[" + p.name + "]." + err);
        if (p.name.empty() || p.joints.empty())
            return fail(error, "parts[]: every part needs a name and joint candidates");
        if (map.find(p.name) >= 0) return fail(error, "parts[" + p.name + "]: duplicate part name");
        if (!(p.radius > 0.f) || !(p.length > 0.f) || !(p.massFraction > 0.f))
            return fail(error, "parts[" + p.name + "]: radius, length and mass must be > 0");
        if (!p.parent.empty()) {
            if (map.find(p.parent) < 0) return fail(error, "parts[" + p.name + "]: parent must be listed before it");
        } else if (std::any_of(map.parts.begin(), map.parts.end(),
                               [](const RagdollPartDef& q) { return q.parent.empty(); })) {
            return fail(error, "ragdoll map: more than one root part");
        }
        map.parts.push_back(std::move(p));
    }
    if (const JsonValue nc = root["noCollide"]; !nc.isAbsent()) {
        if (!nc.isArray()) return fail(error, "noCollide: expected array of [a, b] pairs");
        for (std::uint32_t i = 0; i < nc.size(); ++i) {
            const JsonValue e = nc[i];
            if (!e.isArray() || e.size() != 2 || !e[0].isString() || !e[1].isString())
                return fail(error, "noCollide: expected array of [a, b] pairs");
            std::string a = str(e[0].string()), b = str(e[1].string());
            if (map.find(a) < 0 || map.find(b) < 0)
                return fail(error, "noCollide: unknown part in [" + a + ", " + b + "]");
            map.noCollide.emplace_back(std::move(a), std::move(b));
        }
    }
    out = std::move(map);
    return true;
}

bool importRagdollProfile(std::string_view json, RagdollProfile& out, std::string* error) {
    out = RagdollProfile{};
    JsonDocument doc;
    if (!parseRoot(json, doc, "ragdoll profile", error)) return false;
    const JsonValue j = doc.root();
    RagdollProfile p;
    std::string err;
    if (!getStr(j, "name", p.name, err) || !getBool(j, "useAuthoredKo", p.useAuthoredKo, err) ||
        !getNum(j, "maxLinearVelocity", p.maxLinearVelocity, err) ||
        !getNum(j, "maxAngularVelocity", p.maxAngularVelocity, err) ||
        !getNum(j, "linearDamping", p.linearDamping, err) || !getNum(j, "angularDamping", p.angularDamping, err) ||
        !getNum(j, "jointFriction", p.jointFriction, err) || !getNum(j, "friction", p.friction, err) ||
        !getNum(j, "restitution", p.restitution, err) || !getInt(j, "positionSteps", p.positionSteps, err) ||
        !getInt(j, "velocitySteps", p.velocitySteps, err) || !getNum(j, "koImpulseScale", p.koImpulseScale, err) ||
        !getNum(j, "koLocalShare", p.koLocalShare, err) || !getNum(j, "koMaxLocalDeltaV", p.koMaxLocalDeltaV, err) ||
        !getNum(j, "inertiaScale", p.inertiaScale, err) || !getNum(j, "settleSpeed", p.settleSpeed, err) ||
        !getNum(j, "settleAngularSpeed", p.settleAngularSpeed, err) || !getNum(j, "settleTime", p.settleTime, err) ||
        !getNum(j, "koHeavyAfter", p.koHeavyAfter, err) || !getNum(j, "koHeavyRate", p.koHeavyRate, err) ||
        !getNum(j, "koHeavyBelow", p.koHeavyBelow, err) || !getNum(j, "hitImpulseScale", p.hitImpulseScale, err) ||
        !getNum(j, "hitHold", p.hitHold, err) || !getNum(j, "hitBlendOut", p.hitBlendOut, err))
        return fail(error, err);
    if (p.maxLinearVelocity <= 0.f || p.maxAngularVelocity <= 0.f || p.positionSteps < 0 || p.positionSteps > 255 ||
        p.velocitySteps < 0 || p.velocitySteps > 255 || p.hitHold < 0.f || p.hitBlendOut < 0.f)
        return fail(error, "ragdoll profile: value out of range");
    if (p.linearDamping < 0.f || p.angularDamping < 0.f || p.jointFriction < 0.f || p.friction < 0.f ||
        p.restitution < 0.f || p.restitution > 1.f || p.inertiaScale <= 0.f || p.koLocalShare < 0.f ||
        p.koLocalShare > 1.f)
        return fail(error, "ragdoll profile: value out of range");
    if (const JsonValue mj = j["motor"]; !mj.isAbsent()) {
        if (!mj.isObject()) return fail(error, "motor: expected object");
        if (!getNum(mj, "frequency", p.motor.frequency, err) || !getNum(mj, "damping", p.motor.damping, err) ||
            !getNum(mj, "maxTorque", p.motor.maxTorque, err))
            return fail(error, "motor." + err);
        if (p.motor.frequency <= 0.f || p.motor.damping < 0.f || p.motor.maxTorque < 0.f)
            return fail(error, "motor: value out of range");
    }
    if (const JsonValue dl = j["defaultLimits"]; !dl.isAbsent())
        if (!readLimits(dl, p.defaultLimits, err)) return fail(error, "defaultLimits." + err);
    if (const JsonValue lj = j["limits"]; !lj.isAbsent()) {
        if (!lj.isObject()) return fail(error, "limits: expected object keyed by part name");
        for (std::uint32_t i = 0; i < lj.size(); ++i) {
            RagdollJointLimits l = p.defaultLimits;
            const std::string key = str(lj.key(i));
            if (!readLimits(lj.value(i), l, err)) return fail(error, "limits." + key + "." + err);
            p.limits.emplace_back(key, l);
        }
    }
    out = std::move(p);
    return true;
}

// ---- binding --------------------------------------------------------------------------------------------

namespace {

int findBone(const std::vector<std::string>& bones, const std::vector<std::string>& candidates) {
    for (const std::string& c : candidates)
        for (std::size_t b = 0; b < bones.size(); ++b)
            if (bones[b] == c) return static_cast<int>(b);
    return -1;
}

void addPair(std::vector<std::pair<int, int>>& pairs, int a, int b) {
    if (a < 0 || b < 0 || a == b) return;
    const std::pair<int, int> p(std::min(a, b), std::max(a, b));
    if (std::find(pairs.begin(), pairs.end(), p) == pairs.end()) pairs.push_back(p);
}

} // namespace

bool bindRagdoll(const RagdollMap& map, const std::vector<std::string>& boneNames, RagdollBinding& out,
                 std::string* error) {
    out = RagdollBinding{};
    RagdollBinding b;
    std::vector<int> partToBound(map.parts.size(), -1);
    for (std::size_t i = 0; i < map.parts.size(); ++i) {
        const RagdollPartDef& def = map.parts[i];
        int parent = -1;
        if (!def.parent.empty()) {
            const int mapParent = map.find(def.parent);
            parent = mapParent >= 0 ? partToBound[static_cast<std::size_t>(mapParent)] : -1;
            if (parent < 0) {
                // The parent was dropped (it was optional and missing): drop the subtree.
                b.dropped.push_back(def.name);
                continue;
            }
        }
        const int bone = findBone(boneNames, def.joints);
        if (bone < 0) {
            if (def.parent.empty()) return fail(error, "ragdoll: the root part \"" + def.name + "\" is missing");
            if (!def.optional) return fail(error, "ragdoll: no joint found for part \"" + def.name + "\"");
            b.dropped.push_back(def.name);
            continue;
        }
        RagdollBoundPart part;
        part.part = static_cast<int>(i);
        part.bone = bone;
        part.tipBone = findBone(boneNames, def.tip);
        part.parent = parent;
        part.mass = def.massFraction * map.totalMass;
        partToBound[i] = static_cast<int>(b.parts.size());
        b.parts.push_back(part);
    }
    for (std::size_t i = 0; i < b.parts.size(); ++i) {
        const int p = b.parts[i].parent;
        addPair(b.noCollide, static_cast<int>(i), p);
        if (p >= 0) addPair(b.noCollide, static_cast<int>(i), b.parts[static_cast<std::size_t>(p)].parent);
    }
    for (const auto& [na, nb] : map.noCollide) {
        const int a = map.find(na), c = map.find(nb);
        if (a < 0 || c < 0) continue;
        addPair(b.noCollide, partToBound[static_cast<std::size_t>(a)], partToBound[static_cast<std::size_t>(c)]);
    }
    out = std::move(b);
    return true;
}

// ---- Chaos limits -----------------------------------------------------------------------------------------

RagdollChaosLimits ragdollChaosLimits(const RagdollJointLimits& l) {
    RagdollChaosLimits c;
    c.hinge = l.hinge;
    if (l.hinge) {
        // newengine clamps the hinge range to include 0 (the rest pose).
        const float lo = std::min(l.minAngle, 0.f), hi = std::max(l.maxAngle, 0.f);
        c.swing1Deg = physRadToDeg(0.5f * (hi - lo));
        c.swing1OffsetDeg = physRadToDeg(0.5f * (hi + lo));
        return c;
    }
    // planeHalfCone bounds the swing about the lateral (normal) axis, normalHalfCone the swing out of that plane:
    // the thigh's forward/back swing is its planeHalfCone (1.0) and its sideways swing the normalHalfCone (0.4).
    c.swing1Deg = physRadToDeg(l.planeHalfCone);
    c.swing2Deg = physRadToDeg(l.normalHalfCone);
    c.twistDeg = physRadToDeg(0.5f * (l.twistMax - l.twistMin));
    c.twistOffsetDeg = physRadToDeg(0.5f * (l.twistMax + l.twistMin));
    return c;
}

// ---- feel maths --------------------------------------------------------------------------------------------

float knockbackSlideDistance(float speed, float friction, float gravity) {
    const float decel = friction * gravity;
    return decel > 0.f ? speed * speed / (2.f * decel) : 0.f;
}

float knockbackSpeedForDistance(float distance, float friction, float gravity) {
    const float decel = friction * gravity;
    return decel > 0.f && distance > 0.f ? std::sqrt(2.f * decel * distance) : 0.f;
}

KnockbackPlan planKnockback(float mass, float vx, float vy, float ix, float iy, float friction, float wallDistance,
                            float skin, float gravity) {
    KnockbackPlan r;
    if (!(mass > 0.f)) return r;
    const float nx = vx + ix / mass, ny = vy + iy / mass;
    const float speed = std::sqrt(nx * nx + ny * ny);
    if (speed <= 1e-6f) return r;
    r.dirX = nx / speed;
    r.dirY = ny / speed;
    r.requestedDistance = knockbackSlideDistance(speed, friction, gravity);
    r.distance = r.requestedDistance;
    float target = speed;
    if (wallDistance >= 0.f && wallDistance < r.requestedDistance + skin) {
        r.wallCut = true;
        r.distance = std::max(wallDistance - skin, 0.f);
        target = knockbackSpeedForDistance(r.distance, friction, gravity);
    }
    r.impulseX = (r.dirX * target - vx) * mass;
    r.impulseY = (r.dirY * target - vy) * mass;
    return r;
}

KoImpulseSplit splitKoImpulse(const RagdollProfile& profile, const float impulse[3], float totalMass, float partMass) {
    KoImpulseSplit s;
    float total[3], local[3];
    const float share = std::clamp(profile.koLocalShare, 0.f, 1.f);
    for (int i = 0; i < 3; ++i) {
        total[i] = impulse[i] * profile.koImpulseScale;
        local[i] = total[i] * share;
    }
    const float maxLocal = std::max(partMass, 0.f) * std::max(profile.koMaxLocalDeltaV, 0.f);
    const float len = std::sqrt(local[0] * local[0] + local[1] * local[1] + local[2] * local[2]);
    if (len > maxLocal && len > 0.f)
        for (float& v : local) v *= maxLocal / len;
    for (int i = 0; i < 3; ++i) {
        s.localImpulse[i] = local[i];
        s.wholeDeltaV[i] = totalMass > 0.f ? (total[i] - local[i]) / totalMass : 0.f;
    }
    return s;
}

float hitReactionWeight(const RagdollProfile& profile, float t) {
    if (t < 0.f) return 0.f;
    if (t <= profile.hitHold) return 1.f;
    if (profile.hitBlendOut <= 0.f) return 0.f;
    const float x = std::clamp((t - profile.hitHold) / profile.hitBlendOut, 0.f, 1.f);
    return 1.f - x * x * (3.f - 2.f * x);
}

float heavyDampingFactor(float rate, float dt) { return rate > 0.f && dt > 0.f ? std::exp(-rate * dt) : 1.f; }

bool SettleTracker::update(float maxLinear, float maxAngular, bool anyAwake, float settleSpeed,
                           float settleAngularSpeed, float settleTime, float dt) {
    if (settleTime <= 0.f) return false;
    const bool quiet = anyAwake && maxLinear <= settleSpeed && maxAngular <= settleAngularSpeed;
    quietFor = quiet ? quietFor + dt : 0.f;
    if (quietFor >= settleTime) {
        quietFor = 0.f;
        return true;
    }
    return false;
}

} // namespace ftcore
