#include "ftcore/ReadyStance.h"

#include "ftcore/Json.h"

#include <cmath>

namespace ftcore {

namespace {

constexpr const char* kBoneNames[kReadyBoneCount] = {
    "clavicle_l", "upperarm_l", "lowerarm_l", "hand_l", "clavicle_r", "upperarm_r", "lowerarm_r",
    "hand_r",     "thigh_l",    "calf_l",     "foot_l", "thigh_r",    "calf_r",     "foot_r",
};

constexpr std::size_t kReadyIdMax = 31;

char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool sameId(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
    return true;
}

void setTurn(ReadyStance& s, ReadyBone b, float fwd, float out, float twist = 0.f) {
    s.bones[static_cast<std::size_t>(b)] = ReadyTurn{fwd, out, twist};
}

// Both sides the same.
void setPair(ReadyStance& s, ReadyBone left, float fwd, float out, float twist = 0.f) {
    setTurn(s, left, fwd, out, twist);
    setTurn(s, static_cast<ReadyBone>(static_cast<std::uint8_t>(left) + (static_cast<std::uint8_t>(left) < 8 ? 4 : 3)),
            fwd, out, twist);
}

// Knees bent by `bend` degrees, the shins back by twice that and the feet flat again.
void setKnees(ReadyStance& s, float bend) {
    setPair(s, ReadyBone::ThighL, bend, 0.f);
    setPair(s, ReadyBone::CalfL, -2.f * bend, 0.f);
    setPair(s, ReadyBone::FootL, bend, 0.f);
}

} // namespace

const char* readyBoneName(ReadyBone b) {
    const auto i = static_cast<std::size_t>(b);
    return i < kReadyBoneCount ? kBoneNames[i] : "";
}

ReadyStanceSet readyStanceDefaults() {
    ReadyStanceSet set;
    // Tight: hands high by the chin, elbows in, shoulders rolled forward, the trunk over the knees.
    ReadyStance expert;
    expert.lean = 10.f;
    setPair(expert, ReadyBone::ClavicleL, 6.f, 0.f);
    setPair(expert, ReadyBone::UpperArmL, 60.f, -8.f);
    setPair(expert, ReadyBone::LowerArmL, 105.f, 0.f);
    setPair(expert, ReadyBone::HandL, -10.f, 0.f);
    setKnees(expert, 18.f);
    // Bladed: the lead (left) hand up at the chin's height, the other at a low ready by the hip, the chest turned.
    ReadyStance bladed;
    bladed.lean = 6.f;
    bladed.turn = 20.f;
    setTurn(bladed, ReadyBone::UpperArmL, 55.f, -10.f);
    setTurn(bladed, ReadyBone::LowerArmL, 100.f, 0.f);
    setTurn(bladed, ReadyBone::UpperArmR, 15.f, 10.f);
    setTurn(bladed, ReadyBone::LowerArmR, 45.f, 0.f);
    setKnees(bladed, 14.f);
    // Unsure: hands half raised at the chest, the weight back.
    ReadyStance average;
    average.lean = -4.f;
    average.pelvisForward = -3.f;
    setPair(average, ReadyBone::UpperArmL, 35.f, 8.f);
    setPair(average, ReadyBone::LowerArmL, 70.f, 0.f);
    setKnees(average, 10.f);
    set.stances[0] = {"expert", expert};
    set.stances[1] = {"bladed", bladed};
    set.stances[2] = {"average", average};
    set.stanceCount = 3;
    return set;
}

const ReadyStance* readyStanceFor(const ReadyStanceSet& set, std::string_view leadId) {
    if (set.stanceCount == 0) return nullptr;
    for (std::size_t i = 0; i < set.leadCount; ++i)
        if (sameId(set.leads[i].id, leadId) && set.leads[i].stance < set.stanceCount)
            return &set.stances[set.leads[i].stance].stance;
    return &set.stances[0].stance;
}

bool importReadyStances(std::string_view json, ReadyStanceSet& out, std::string* error) {
    JsonDocument doc;
    if (!doc.parse(json, error)) return false;
    const JsonValue root = doc.root();
    const auto fail = [&](std::string_view key, const char* why) {
        if (error) *error = "ready" + (key.empty() ? std::string() : "." + std::string(key)) + ": " + why;
        return false;
    };
    if (!root.isObject()) return fail("", "not an object");
    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view k = root.key(i);
        if (k != "version" && k != "about" && k != "stances" && k != "leads") return fail(k, "not a known key");
    }
    ReadyStanceSet r = out;
    const ReadyStanceSet defaults = readyStanceDefaults();

    const JsonValue stances = root["stances"];
    if (!stances.isAbsent()) {
        if (!stances.isObject()) return fail("stances", "not an object");
        if (stances.size() == 0) return fail("stances", "empty");
        if (stances.size() > kReadyStanceMax) return fail("stances", "too many");
        ReadyStanceSet next = r;
        next.stanceCount = 0;
        for (std::uint32_t i = 0; i < stances.size(); ++i) {
            const std::string_view id = stances.key(i);
            const std::string where = "stances." + std::string(id);
            if (id.empty() || id.size() > kReadyIdMax) return fail(where, "bad id");
            for (std::size_t j = 0; j < next.stanceCount; ++j)
                if (sameId(next.stances[j].id, id)) return fail(where, "named twice");
            const JsonValue sv = stances.value(i);
            if (!sv.isObject()) return fail(where, "not an object");
            ReadyStance s;
            for (std::size_t j = 0; j < defaults.stanceCount; ++j)
                if (sameId(defaults.stances[j].id, id)) s = defaults.stances[j].stance;
            struct Field {
                const char* name;
                float* value;
                double lo, hi;
            };
            const Field fields[] = {
                {"lean", &s.lean, -30.0, 30.0},        {"turn", &s.turn, -45.0, 45.0},
                {"pelvisForward", &s.pelvisForward, -20.0, 20.0},
                {"easeIn", &s.easeIn, 0.02, 3.0},      {"easeOut", &s.easeOut, 0.02, 3.0},
                {"fadeFrom", &s.fadeFrom, 0.0, 10.0},  {"fadeTo", &s.fadeTo, 0.0, 12.0},
            };
            for (std::uint32_t k = 0; k < sv.size(); ++k) {
                const std::string_view key = sv.key(k);
                if (key == "bones") continue;
                bool known = false;
                for (const Field& f : fields) known = known || key == f.name;
                if (!known) return fail(where + "." + std::string(key), "not a known key");
            }
            for (const Field& f : fields) {
                const JsonValue v = sv[f.name];
                if (v.isAbsent()) continue;
                if (!v.isNumber()) return fail(where + "." + f.name, "not a number");
                const double d = v.number();
                if (!(d >= f.lo && d <= f.hi)) return fail(where + "." + f.name, "out of range");
                *f.value = static_cast<float>(d);
            }
            if (!(s.fadeTo > s.fadeFrom)) return fail(where + ".fadeTo", "not above fadeFrom");
            const JsonValue bones = sv["bones"];
            if (!bones.isAbsent()) {
                if (!bones.isObject()) return fail(where + ".bones", "not an object");
                for (std::uint32_t b = 0; b < bones.size(); ++b) {
                    const std::string_view name = bones.key(b);
                    const std::string bw = where + ".bones." + std::string(name);
                    std::size_t bone = kReadyBoneCount;
                    for (std::size_t n = 0; n < kReadyBoneCount; ++n)
                        if (name == kBoneNames[n]) bone = n;
                    if (bone == kReadyBoneCount) return fail(bw, "not a known bone");
                    const JsonValue tv = bones.value(b);
                    if (!tv.isObject()) return fail(bw, "not an object");
                    ReadyTurn t = s.bones[bone];
                    for (std::uint32_t k = 0; k < tv.size(); ++k) {
                        const std::string_view axis = tv.key(k);
                        float* into = axis == "fwd" ? &t.fwd : axis == "out" ? &t.out : axis == "twist" ? &t.twist : nullptr;
                        if (!into) return fail(bw + "." + std::string(axis), "not fwd, out or twist");
                        const JsonValue v = tv.value(k);
                        if (!v.isNumber()) return fail(bw + "." + std::string(axis), "not a number");
                        const double d = v.number();
                        if (!(d >= -150.0 && d <= 150.0)) return fail(bw + "." + std::string(axis), "out of range");
                        *into = static_cast<float>(d);
                    }
                    s.bones[bone] = t;
                }
            }
            next.stances[next.stanceCount++] = {std::string(id), s};
        }
        // Leads kept from before still name their stance by id.
        for (std::size_t i = 0; i < r.leadCount; ++i) {
            const std::string& was = r.stances[r.leads[i].stance].id;
            bool found = false;
            for (std::size_t j = 0; j < next.stanceCount; ++j)
                if (sameId(next.stances[j].id, was)) {
                    next.leads[i].stance = static_cast<std::uint8_t>(j);
                    found = true;
                }
            if (!found && root["leads"].isAbsent()) return fail("stances", "drops a stance a lead uses");
        }
        r = next;
    }

    const JsonValue leads = root["leads"];
    if (!leads.isAbsent()) {
        if (!leads.isObject()) return fail("leads", "not an object");
        if (leads.size() > kReadyLeadMax) return fail("leads", "too many");
        r.leadCount = 0;
        for (std::uint32_t i = 0; i < leads.size(); ++i) {
            const std::string_view id = leads.key(i);
            const std::string where = "leads." + std::string(id);
            if (id.empty() || id.size() > kReadyIdMax) return fail(where, "bad id");
            for (std::size_t j = 0; j < r.leadCount; ++j)
                if (sameId(r.leads[j].id, id)) return fail(where, "named twice");
            const JsonValue v = leads.value(i);
            if (!v.isString()) return fail(where, "not a stance id");
            std::size_t stance = r.stanceCount;
            for (std::size_t j = 0; j < r.stanceCount; ++j)
                if (sameId(r.stances[j].id, v.string())) stance = j;
            if (stance == r.stanceCount) return fail(where, "names no stance");
            std::string lower(id);
            for (char& c : lower) c = lowerAscii(c);
            r.leads[r.leadCount++] = {lower, static_cast<std::uint8_t>(stance)};
        }
    }
    out = r;
    return true;
}

float readyStanceStep(float alpha, bool wanted, float dt, const ReadyStance& stance) {
    if (!(dt > 0.f)) return alpha;
    const float a = wanted ? alpha + dt / stance.easeIn : alpha - dt / stance.easeOut;
    return a < 0.f ? 0.f : a > 1.f ? 1.f : a;
}

float readyStanceShare(float alpha, float groundSpeed, const ReadyStance& stance) {
    const float a = alpha < 0.f ? 0.f : alpha > 1.f ? 1.f : alpha;
    const float ease = a * a * (3.f - 2.f * a);
    const float speed = std::fabs(groundSpeed);
    float fade = 1.f;
    if (speed >= stance.fadeTo)
        fade = 0.f;
    else if (speed > stance.fadeFrom)
        fade = 1.f - (speed - stance.fadeFrom) / (stance.fadeTo - stance.fadeFrom);
    return ease * fade;
}

} // namespace ftcore
