#pragma once

// The leads' common wet switch: one weather value w (0 dry .. 1 soaked by rain) feeds three things.
//   - Cloth: the garments' soak is the larger of the water's own (WaterWetness, from wading and swimming) and w; when w
//     is the larger, the wet line is lifted over any head (rain soaks the whole body), and the water's stain stays.
//   - Hair: the wet groom (a lead look's WetGroom) is worn from w >= kGroomWetOn until w falls below kGroomWetOff (the
//     gap keeps a value hovering near one edge from swapping the groom every frame).
//   - Skin: a wet-skin amount, smoothstep(kSkinFrom, kSkinTo, w); the skin master lowers its roughness toward
//     kSkinWetRough by that amount times its wet mask (forehead, nose, cheekbones). No drips: nothing here makes any.
// Skin and hair take the lead's surface wetness: the weather's or the lead's own water state's (WaterWetness::wetness,
// from wading and swimming), whichever is wetter, the same rule as the weather's leadSurfaceWetness (ftcore/Weather.h,
// used through surfaceFromRain below once it is there; nothing calls it yet: the rain hook-up comes after the world
// lane's promotion). w comes from ft.LeadWet: 0 (the default) is no rain, a value in 0..1
// forces it, below 0 follows the rain. The night look's damp floor never wets a lead. Plain C++20, header only, no allocation.

#include "ftcore/WaterBodies.h"
#if __has_include("ftcore/Weather.h")
#include "ftcore/Weather.h"
#define FTCORE_LEADWET_HAS_RAIN 1
#endif

namespace ftcore::leadwet {

inline constexpr float kRainLineM = 2.5f;   // m over the feet: over any lead's head
inline constexpr float kGroomWetOn = 0.5f;
inline constexpr float kGroomWetOff = 0.35f;
inline constexpr float kSkinFrom = 0.15f;
inline constexpr float kSkinTo = 0.6f;
inline constexpr float kSkinWetRough = 0.25f;

// 0..1; NaN reads as 0 (dry).
constexpr float unit(float v) { return v > 0.f ? (v < 1.f ? v : 1.f) : 0.f; }

// The weather value: `forced` when it is 0 or more (ft.LeadWet), else the rain's.
constexpr float weather(float forced, float rain) { return forced >= 0.f ? unit(forced) : unit(rain); }

// The lead's surface wetness for skin and hair: the wetter of the weather and its own water state.
constexpr float surface(float w, const WaterWetness& water) {
    const float a = unit(w);
    const float b = unit(water.wetness);
    return a > b ? a : b;
}
#ifdef FTCORE_LEADWET_HAS_RAIN
// The same through the weather's own rule, with the lead's exposure (1 in the open). Its own name, so it never competes
// with surface(float, ...) in overload resolution.
inline float surfaceFromRain(const RainState& rain, const WaterWetness& water, float exposure = 1.f) {
    return unit(leadSurfaceWetness(rain, water.wetness, exposure));
}
#endif

// The cloth's soak: the water's own, or the weather's when that is wetter (the line then over the head).
constexpr WaterWetness cloth(const WaterWetness& water, float w) {
    const float rain = unit(w);
    if (!(rain > water.wetness)) {
        return water;
    }
    WaterWetness out = water;
    out.wetness = rain;
    out.line = water.line > kRainLineM ? water.line : kRainLineM;
    return out;
}

// Whether the wet groom is worn, given whether it was.
constexpr bool groomWet(float w, bool was) { return was ? unit(w) >= kGroomWetOff : unit(w) >= kGroomWetOn; }

// The wet-skin amount, 0..1.
constexpr float skin(float w) {
    const float t = unit((unit(w) - kSkinFrom) / (kSkinTo - kSkinFrom));
    return t * t * (3.f - 2.f * t);
}

// The shown wet-skin amount eases toward its target over kSkinEaseS (both ways), so a swim's exit or a forced value
// does not snap the face. NaN or negative dt holds it.
inline constexpr float kSkinEaseS = 2.f;
constexpr float ease(float shown, float target, float dt) {
    const float t = unit(target);
    const float s = unit(shown);
    const float step = dt > 0.f ? dt / kSkinEaseS : 0.f;
    if (s < t) {
        return s + step < t ? s + step : t;
    }
    return s - step > t ? s - step : t;
}

// The MetaHuman baked skin (the leads' face and body): its post-bake roughness multiplier goes to kMhRoughWet of its dry
// value, its specular multiplier up to kMhSpecWet of it, by the wet-skin amount. Uniform over the face (no mask).
inline constexpr float kMhRoughWet = 0.5f;
inline constexpr float kMhSpecWet = 1.15f;
constexpr float mhRoughness(float dry, float skinWet) { return dry * (1.f + (kMhRoughWet - 1.f) * unit(skinWet)); }
constexpr float mhSpecular(float dry, float skinWet) { return dry * (1.f + (kMhSpecWet - 1.f) * unit(skinWet)); }

// Hair without a wet groom (a beard, brows, lashes): darker (melanin up by kHairWetMelanin, to 1 at most) and glossier
// (roughness to kHairWetRough of dry), by the same amount.
inline constexpr float kHairWetMelanin = 0.2f;
inline constexpr float kHairWetRough = 0.6f;
constexpr float hairMelanin(float dry, float wet) { return unit(dry + kHairWetMelanin * unit(wet)); }
constexpr float hairRoughness(float dry, float wet) { return dry * (1.f + (kHairWetRough - 1.f) * unit(wet)); }

// The skin master's roughness (mirrors its SkinRoughness node's last step): base lowered toward kSkinWetRough by the
// wet amount times the mask; never raised (a part already smoother stays so).
constexpr float skinRoughness(float base, float skinWet, float mask) {
    const float k = unit(skinWet) * unit(mask);
    const float wet = base + (kSkinWetRough - base) * k;
    return wet < base ? wet : base;
}

// The party followers take the same rules as the lead: their weather value is the lead's w and their own water soak
// starts dry (a default WaterWetness), set later by the party's water step. A dev switch (ft.PartyWet, never in a
// shipped build) forces one follower's weather value, or every follower's, for a wet-beside-dry still and a cost run:
// "<slot>,<wetness>" with slot 0.. a party slot and -1 every follower; empty or unreadable text is off.
struct PartyWetForce {
    bool on = false;
    int slot = 0;          // -1: every follower
    float wetness = 0.f;   // 0..1
};

// Reads "<slot>,<wetness>" (spaces allowed around both). No allocation; anything else is off.
constexpr PartyWetForce parsePartyWet(const char* text) {
    PartyWetForce off{};
    if (text == nullptr) {
        return off;
    }
    const char* p = text;
    auto skip = [&p]() { while (*p == ' ') ++p; };
    skip();
    bool neg = false;
    if (*p == '-') {
        neg = true;
        ++p;
    }
    if (*p < '0' || *p > '9') {
        return off;
    }
    int slot = 0;
    while (*p >= '0' && *p <= '9' && slot < 1000) {
        slot = slot * 10 + (*p++ - '0');
    }
    if (neg && slot != 1) {
        return off; // -1 is the only negative slot
    }
    skip();
    if (*p++ != ',') {
        return off;
    }
    skip();
    float whole = 0.f, frac = 0.f, scale = 1.f;
    bool digits = false;
    while (*p >= '0' && *p <= '9') {
        whole = whole * 10.f + static_cast<float>(*p++ - '0');
        digits = true;
    }
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            scale *= 0.1f;
            frac += scale * static_cast<float>(*p++ - '0');
            digits = true;
        }
    }
    skip();
    if (!digits || *p != '\0') {
        return off;
    }
    return PartyWetForce{true, neg ? -1 : slot, unit(whole + frac)};
}

// A follower's weather value: the forced wetness when the switch names its slot (or every follower), else the lead's.
constexpr float partyWeather(float leadW, int slot, const PartyWetForce& force) {
    return force.on && (force.slot == -1 || force.slot == slot) ? unit(force.wetness) : unit(leadW);
}

// The soak belongs to the member, not to the body the player drives. The lead's lives in the water subsystem and each
// follower's on its actor; when the followers are cleared (a lead switch, a respawn) each one's is stored here by its
// place in the party, the old lead's too, with the game time, and the new lead and the new followers take theirs back.
// A member with no live actor (a failed spawn, one over the follower cap) dries while it is away, in the open, by the
// water's own law (WaterWetness::step out of the water, dry scale 1, as a follower's StepWet), so it never comes back
// as wet as it left however long it was gone. At no time away it gets back exactly what was stored.
inline constexpr int kPartyWetMembers = 8;

// The party member follower i walks as behind `lead`, in a party of `count` (the members after the lead, in order);
// -1 when any argument is out of range (the caller brings the lead into range first).
constexpr int followerMember(int lead, int i, int count) {
    return count > 0 && lead >= 0 && lead < count && i >= 0 && i < count - 1 ? (lead + 1 + i) % count : -1;
}

// Whether a look put on lands the wet skin at its target at once: only a later look (a switch hands the member its own
// soak back); the first look, at the start, eases in from dry like any other change.
constexpr bool skinSnapOnLook(bool lookWornBefore) { return lookWornBefore; }

struct PartyWet {
    WaterWetness member[kPartyWetMembers]{};
    double at[kPartyWetMembers]{};   // game time (s) when it was stored
    bool held[kPartyWetMembers]{};   // stored at least once
    static constexpr bool has(int m) { return m >= 0 && m < kPartyWetMembers; }
    // False, and nothing stored, out of range (the caller says so: that member would come back dry).
    constexpr bool store(int m, const WaterWetness& w, double now) {
        if (!has(m)) {
            return false;
        }
        member[m] = w;
        at[m] = now;
        held[m] = true;
        return true;
    }
    // Whether the member has a soak to give back (a new member, or one out of range, has none: it starts dry, eased).
    constexpr bool stored(int m) const { return has(m) && held[m]; }
    // The stored soak, dried for the time since it was stored; dry when there is none. NaN or earlier `now` holds it.
    WaterWetness of(int m, double now) const {
        if (!stored(m)) {
            return WaterWetness{};
        }
        WaterWetness w = member[m];
        const double away = now - at[m];
        if (away > 0.0) {
            w.step(0.f, 0.f, static_cast<float>(away));
        }
        return w;
    }
};

} // namespace ftcore::leadwet
