#include "ftcore/PoseCorrective.h"

#include "ftcore/Json.h"

#include <cmath>
#include <utility>

namespace ftcore {

namespace {

constexpr float kPi = 3.14159265358979323846f;

char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
    return true;
}

// Letters, digits and '_'; 1..128 characters.
bool assetName(std::string_view s) {
    if (s.empty() || s.size() > 128) return false;
    for (const char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

float dot(const Dir3& a, const Dir3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Dir3 cross(const Dir3& a, const Dir3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Dir3 minus(const Dir3& a, const Dir3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

Dir3 scaled(const Dir3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }

bool lateralAxis(std::string_view s, Dir3& out) {
    float sign = 1.f;
    if (!s.empty() && s[0] == '-') {
        sign = -1.f;
        s.remove_prefix(1);
    }
    if (s == "x") out = {sign, 0.f, 0.f};
    else if (s == "y") out = {0.f, sign, 0.f};
    else if (s == "z") out = {0.f, 0.f, sign};
    else return false;
    return true;
}

} // namespace

float flexionDegrees(const Dir3& ref, const Dir3& cur, const Dir3& lateral) {
    const float ll = std::sqrt(dot(lateral, lateral));
    if (ll < 1e-6f) return 0.f;
    const Dir3 n = scaled(lateral, 1.f / ll);
    const Dir3 a = minus(ref, scaled(n, dot(ref, n)));
    const Dir3 b = minus(cur, scaled(n, dot(cur, n)));
    if (dot(a, a) < 1e-12f || dot(b, b) < 1e-12f) return 0.f;
    const float s = dot(cross(a, b), n);
    const float c = dot(a, b);
    return std::atan2(s, c) * (180.f / kPi);
}

float correctiveWeight(float deg, float startDeg, float endDeg) {
    if (!(endDeg > startDeg)) return deg >= startDeg ? 1.f : 0.f;
    if (deg <= startDeg) return 0.f;
    if (deg >= endDeg) return 1.f;
    const float t = (deg - startDeg) / (endDeg - startDeg);
    return t * t * (3.f - 2.f * t);
}

bool importPoseCorrectives(std::string_view json, PoseCorrectiveTable& out, std::string* error,
                           std::vector<std::string>* warnings) {
    out.entries.clear();
    std::string problems;
    auto fail = [&](const std::string& line) { problems += line + "\n"; };
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(json, &parseError)) {
        if (error) *error = "json: " + parseError;
        return false;
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        if (error) *error = "(root): not an object";
        return false;
    }
    if (!root["version"].isInteger() || root["version"].integer() != 1) fail("version: must be 1");
    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view k = root.key(i);
        if (k != "version" && k != "correctives" && k != "about" && warnings)
            warnings->push_back("unknown key " + std::string(k) + " (ignored)");
    }
    const JsonValue list = root["correctives"];
    if (!list.isArray()) fail("correctives: must be an array");
    std::vector<PoseCorrective> entries;
    for (std::uint32_t i = 0; i < list.size(); ++i) {
        const JsonValue v = list[i];
        const std::string path = "correctives[" + std::to_string(i) + "]";
        if (!v.isObject()) {
            fail(path + ": must be an object");
            continue;
        }
        bool ok = true;
        auto name = [&](const char* key, std::string& dst) {
            const JsonValue f = v[key];
            if (!f.isString() || !assetName(f.string())) {
                fail(path + "." + key + ": must be 1..128 letters, digits and '_'");
                ok = false;
                return;
            }
            dst = std::string(f.string());
        };
        auto degrees = [&](const char* key, float& dst) {
            const JsonValue f = v[key];
            if (!f.isNumber() || !(f.number() >= -180.0 && f.number() <= 180.0)) {
                fail(path + "." + key + ": must be a number within -180..180");
                ok = false;
                return;
            }
            dst = static_cast<float>(f.number());
        };
        PoseCorrective e;
        name("morph", e.morph);
        name("bone", e.bone);
        name("child", e.child);
        const std::string_view side = v["side"].string();
        if (side == "L") e.side = 'L';
        else if (side == "R") e.side = 'R';
        else if (side == "mid") e.side = 'M';
        else {
            fail(path + ".side: must be \"L\", \"R\" or \"mid\"");
            ok = false;
        }
        if (!v["lateral"].isAbsent() && (!v["lateral"].isString() || !lateralAxis(v["lateral"].string(), e.lateral))) {
            fail(path + ".lateral: must be one of x, y, z, -x, -y, -z");
            ok = false;
        }
        degrees("start_deg", e.startDeg);
        degrees("end_deg", e.endDeg);
        for (std::uint32_t k = 0; k < v.size(); ++k) {
            const std::string_view key = v.key(k);
            if (key != "morph" && key != "side" && key != "bone" && key != "child" && key != "lateral" &&
                key != "start_deg" && key != "end_deg" && key != "about" && warnings)
                warnings->push_back(path + ": unknown key " + std::string(key) + " (ignored)");
        }
        if (!ok) continue;
        if (!(e.endDeg > e.startDeg)) {
            fail(path + ": start_deg must be below end_deg");
            continue;
        }
        if (equalsNoCase(e.bone, e.child)) {
            fail(path + ": bone and child are the same bone");
            continue;
        }
        bool twice = false;
        for (const PoseCorrective& p : entries) twice = twice || equalsNoCase(p.morph, e.morph);
        if (twice) {
            fail(path + ".morph: " + e.morph + " listed twice");
            continue;
        }
        entries.push_back(std::move(e));
    }
    if (!problems.empty()) {
        if (error) *error = problems;
        return false;
    }
    out.entries = std::move(entries);
    return true;
}

} // namespace ftcore
