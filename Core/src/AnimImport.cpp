#include "ftcore/AnimImport.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ftcore::animimport {

namespace {

std::string_view Trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

std::vector<std::string_view> Split(std::string_view s, char sep)
{
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            out.push_back(Trim(s.substr(start, i - start)));
            start = i + 1;
        }
    }
    return out;
}

bool ToNumber(std::string_view s, double& out)
{
    if (s.empty()) {
        return false;
    }
    if (s.front() == '+') {
        s.remove_prefix(1);
    }
    const char* end = s.data() + s.size();
    const auto result = std::from_chars(s.data(), end, out);
    return result.ec == std::errc() && result.ptr == end && std::isfinite(out);
}

void Fail(std::string* error, const std::string& why)
{
    if (error) {
        *error = why;
    }
}

std::uint32_t ReadU32(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
        (static_cast<std::uint32_t>(p[3]) << 24);
}

} // namespace

bool ParseSocketSpec(std::string_view text, SocketSpec& out, std::string* error)
{
    const std::vector<std::string_view> f = Split(Trim(text), ',');
    SocketSpec s;
    if (f.size() < 2 || f[0].empty() || f[1].empty()) {
        Fail(error, "a socket needs name,bone,...");
        return false;
    }
    s.name = std::string(f[0]);
    s.bone = std::string(f[1]);
    std::vector<double> n;
    const bool cs = f.size() > 2 && (f[2] == "cs" || f[2] == "CS");
    for (std::size_t i = cs ? 3 : 2; i < f.size(); ++i) {
        double v = 0.0;
        if (!ToNumber(f[i], v)) {
            Fail(error, "socket " + s.name + ": '" + std::string(f[i]) + "' is not a number");
            return false;
        }
        n.push_back(v);
    }
    if (n.size() != 3 && n.size() != 6) {
        Fail(error, "socket " + s.name + ": expected 3 or 6 numbers, got " + std::to_string(n.size()));
        return false;
    }
    s.componentSpace = cs;
    s.x = n[0];
    s.y = n[1];
    s.z = n[2];
    if (n.size() == 6) {
        if (cs) {
            const double len = std::sqrt(n[3] * n[3] + n[4] * n[4] + n[5] * n[5]);
            if (len < 1e-9) {
                Fail(error, "socket " + s.name + ": the axis has zero length");
                return false;
            }
            s.hasAxis = true;
            s.ax = n[3] / len;
            s.ay = n[4] / len;
            s.az = n[5] / len;
        } else {
            s.roll = n[3];
            s.pitch = n[4];
            s.yaw = n[5];
        }
    }
    out = s;
    return true;
}

bool ParseTimes(std::string_view text, std::vector<double>& out, std::string* error)
{
    std::vector<double> times;
    for (std::string_view f : Split(Trim(text), ',')) {
        double v = 0.0;
        if (!ToNumber(f, v) || v < 0.0) {
            Fail(error, "'" + std::string(f) + "' is not a time in seconds at or above 0");
            return false;
        }
        times.push_back(v);
    }
    out = times;
    return true;
}

bool ParseViews(std::string_view text, std::vector<ShotView>& out, std::string* error)
{
    struct Preset {
        const char* name;
        double yaw;
    };
    static constexpr Preset kPresets[] = {
        {"front", 0.0}, {"tq", 40.0}, {"tqr", -40.0}, {"side", 90.0}, {"sider", -90.0}, {"back", 180.0},
    };
    constexpr double kPresetPitch = 6.0;
    std::vector<ShotView> views;
    for (std::string_view f : Split(Trim(text), ',')) {
        if (f.empty()) {
            Fail(error, "an empty view");
            return false;
        }
        const std::vector<std::string_view> parts = Split(f, ':');
        if (parts.size() == 3) {
            ShotView v;
            v.name = std::string(parts[0]);
            if (v.name.empty() || !ToNumber(parts[1], v.yawDeg) || !ToNumber(parts[2], v.pitchDeg)) {
                Fail(error, "view '" + std::string(f) + "' is not name:yaw:pitch");
                return false;
            }
            views.push_back(v);
            continue;
        }
        bool found = false;
        for (const Preset& p : kPresets) {
            if (parts.size() == 1 && f == p.name) {
                views.push_back(ShotView{p.name, p.yaw, kPresetPitch});
                found = true;
                break;
            }
        }
        if (!found) {
            Fail(error, "unknown view '" + std::string(f) + "' (front, tq, tqr, side, sider, back or name:yaw:pitch)");
            return false;
        }
    }
    out = views;
    return true;
}

int FrameAt(double seconds, double fps)
{
    if (!(seconds > 0.0) || !(fps > 0.0)) {
        return 0;
    }
    return static_cast<int>(std::floor(seconds * fps + 0.5));
}

std::string StillName(std::string_view stem, double seconds, std::string_view view)
{
    char t[32];
    std::snprintf(t, sizeof t, "%.2f", seconds);
    return std::string(stem) + "_t" + t + "_" + std::string(view) + ".png";
}

std::vector<std::string> FbxLimbNames(const std::uint8_t* data, std::size_t size)
{
    std::vector<std::string> names;
    if (!data || size < 27) {
        return names;
    }
    static constexpr char kBinaryMagic[] = "Kaydara FBX Binary";
    if (std::memcmp(data, kBinaryMagic, sizeof kBinaryMagic - 1) == 0) {
        // A Model record's properties: L id, S "<name>\0\1Model", S "LimbNode". Strings are never compressed.
        static constexpr char kClass[] = {'\0', '\x01', 'M', 'o', 'd', 'e', 'l'};
        static constexpr char kLimb[] = "LimbNode";
        for (std::size_t i = 0; i + 5 < size; ++i) {
            if (data[i] != 'S') {
                continue;
            }
            const std::uint32_t len = ReadU32(data + i + 1);
            if (len < sizeof kClass || len > 4096 || i + 5 + len + 5 + 8 > size) {
                continue;
            }
            const std::uint8_t* str = data + i + 5;
            if (std::memcmp(str + len - sizeof kClass, kClass, sizeof kClass) != 0) {
                continue;
            }
            const std::uint8_t* next = str + len;
            if (next[0] != 'S' || ReadU32(next + 1) != 8 || std::memcmp(next + 5, kLimb, 8) != 0) {
                continue;
            }
            names.emplace_back(reinterpret_cast<const char*>(str), len - sizeof kClass);
            i += 5 + len;
        }
        return names;
    }
    // ASCII: Model: <id>, "Model::<name>", "LimbNode" {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    static constexpr std::string_view kKey = "\"Model::";
    static constexpr std::string_view kTail = "\", \"LimbNode\"";
    for (std::size_t at = text.find(kKey); at != std::string_view::npos; at = text.find(kKey, at + 1)) {
        const std::size_t start = at + kKey.size();
        const std::size_t end = text.find('"', start);
        if (end == std::string_view::npos) {
            break;
        }
        if (text.compare(end, kTail.size(), kTail) == 0) {
            names.emplace_back(text.substr(start, end - start));
        }
    }
    return names;
}

std::vector<std::string> MissingNames(const std::vector<std::string>& wanted, const std::vector<std::string>& have)
{
    std::vector<std::string> missing;
    for (const std::string& w : wanted) {
        bool found = false;
        for (const std::string& h : have) {
            if (h == w) {
                found = true;
                break;
            }
        }
        if (!found) {
            missing.push_back(w);
        }
    }
    return missing;
}

bool TrackShareOk(int tracks, int fileBones, double share)
{
    if (fileBones <= 0) {
        return true;
    }
    return static_cast<double>(tracks) + 1e-9 >= share * static_cast<double>(fileBones);
}

namespace {

char Lower(char ch)
{
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
}

bool StartsWithNoCase(std::string_view s, std::string_view head)
{
    if (s.size() < head.size()) {
        return false;
    }
    for (std::size_t i = 0; i < head.size(); ++i) {
        if (Lower(s[i]) != Lower(head[i])) {
            return false;
        }
    }
    return true;
}

bool Contains(const std::vector<std::string>& names, const std::string& name)
{
    for (const std::string& n : names) {
        if (n == name) {
            return true;
        }
    }
    return false;
}

constexpr ChainSpec kMixamoChains[] = {
    {"Spine", "Spine", "Spine2"},
    {"Neck", "Neck", "Neck"},
    {"Head", "Head", "Head"},
    {"LeftClavicle", "LeftShoulder", "LeftShoulder"},
    {"RightClavicle", "RightShoulder", "RightShoulder"},
    {"LeftArm", "LeftArm", "LeftHand"},
    {"RightArm", "RightArm", "RightHand"},
    {"LeftLeg", "LeftUpLeg", "LeftFoot"},
    {"RightLeg", "RightUpLeg", "RightFoot"},
    {"LeftFoot", "LeftToeBase", "LeftToeBase"},
    {"RightFoot", "RightToeBase", "RightToeBase"},
    {"LeftThumb", "LeftHandThumb1", "LeftHandThumb3"},
    {"LeftIndex", "LeftHandIndex1", "LeftHandIndex3"},
    {"LeftMiddle", "LeftHandMiddle1", "LeftHandMiddle3"},
    {"LeftRing", "LeftHandRing1", "LeftHandRing3"},
    {"LeftPinky", "LeftHandPinky1", "LeftHandPinky3"},
    {"RightThumb", "RightHandThumb1", "RightHandThumb3"},
    {"RightIndex", "RightHandIndex1", "RightHandIndex3"},
    {"RightMiddle", "RightHandMiddle1", "RightHandMiddle3"},
    {"RightRing", "RightHandRing1", "RightHandRing3"},
    {"RightPinky", "RightHandPinky1", "RightHandPinky3"},
};

struct BoneAlias {
    const char* ue;
    const char* mixamo;
};

constexpr BoneAlias kMixamoBones[] = {
    {"pelvis", "Hips"},
    {"spine_01", "Spine"},
    {"neck_01", "Neck"},
    {"head", "Head"},
    {"clavicle_l", "LeftShoulder"},
    {"clavicle_r", "RightShoulder"},
    {"upperarm_l", "LeftArm"},
    {"upperarm_r", "RightArm"},
    {"lowerarm_l", "LeftForeArm"},
    {"lowerarm_r", "RightForeArm"},
    {"hand_l", "LeftHand"},
    {"hand_r", "RightHand"},
    {"thigh_l", "LeftUpLeg"},
    {"thigh_r", "RightUpLeg"},
    {"calf_l", "LeftLeg"},
    {"calf_r", "RightLeg"},
    {"foot_l", "LeftFoot"},
    {"foot_r", "RightFoot"},
    {"ball_l", "LeftToeBase"},
    {"ball_r", "RightToeBase"},
};

} // namespace

bool MixamoPrefix(const std::vector<std::string>& bones, std::string& prefix)
{
    constexpr std::string_view hips = "Hips";
    for (const std::string& b : bones) {
        if (b.size() <= hips.size() || std::string_view(b).substr(b.size() - hips.size()) != hips) {
            continue;
        }
        const std::string p = b.substr(0, b.size() - hips.size());
        if (StartsWithNoCase(p, "mixamorig") && Contains(bones, p + "Spine") && Contains(bones, p + "LeftUpLeg")) {
            prefix = p;
            return true;
        }
    }
    return false;
}

std::span<const ChainSpec> MixamoChains()
{
    return kMixamoChains;
}

const char* MixamoBoneFor(std::string_view ueBone)
{
    for (const BoneAlias& a : kMixamoBones) {
        if (ueBone == a.ue) {
            return a.mixamo;
        }
    }
    return nullptr;
}

bool OptionalTargetChain(std::string_view chain)
{
    constexpr std::string_view tail = "Metacarpal";
    return chain.size() > tail.size() && chain.substr(chain.size() - tail.size()) == tail;
}

} // namespace ftcore::animimport
