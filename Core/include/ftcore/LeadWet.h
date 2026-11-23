#pragma once

// The leads' common wet switch: one weather value w (0 dry .. 1 soaked by rain) feeds three things.
//   - Cloth: the garments' soak is the larger of the water's own (WaterWetness, from wading and swimming) and w; when w
//     is the larger, the wet line is lifted over any head (rain soaks the whole body), and the water's stain stays.
//   - Hair: the wet groom (a lead look's WetGroom) is worn from w >= kGroomWetOn until w falls below kGroomWetOff (the
//     gap keeps a value hovering near one edge from swapping the groom every frame).
//   - Skin: a wet-skin amount, smoothstep(kSkinFrom, kSkinTo, w); the skin master lowers its roughness toward
//     kSkinWetRough by that amount times its wet mask (forehead, nose, cheekbones). No drips: nothing here makes any.
// Skin and hair take the lead's surface wetness: the weather's or the lead's own water state's (WaterWetness::wetness,
// from wading and swimming), whichever is wetter, the same rule as the weather's leadSurfaceWetness (ftcore/Weather.h).
// w comes from ft.LeadWet: below 0 (the default, -1) follows the party's rain soak (LeadRain below), a value in 0..1
// forces it (0 holds the leads dry whatever the weather). The night look's damp floor never wets a lead. Plain C++20, header only, no allocation.

#include "ftcore/WaterBodies.h"
#if __has_include("ftcore/Weather.h")
#include "ftcore/RainCover.h"
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
// The lead's own rain soak (ft.LeadWet below 0 reads it). A lead under a roof is not wetted, and does not dry at once:
// - exposure: from an upward collision trace at the top of the lead's head, every kCoverTraceS, whether or not the
//   ground's cover map (-RainCover) runs: 1 with nothing over it, else rainCoverOpen(occluder, head, biasM), the ground
//   cover's own rule and bias. Only hits that the cover map would draw count (opaque, visible, non-skeletal meshes); a
//   drawn mesh with no blocking collision is missed, which the game's dev check lists (LEAD_COVER_SET);
// - the soak moves toward the open-ground wetness times the exposure: up at the rain's wetRisePerS, down at its dryPerS.
//   Exposure 0 only stops the wetting; drying keeps its own rate;
// - on the first step after the first trace that answers (rainSoakSettled) it starts where that rule settles, so a lead
//   loaded into a settled rain is wet in the open and dry under a roof.
inline constexpr float kCoverTraceS = 0.25f;
inline float coverExposure(bool hit, float occluderZ, float headZ, const RainCoverSpec& spec) {
    return hit ? unit(rainCoverOpen(occluderZ, headZ, spec.biasM)) : 1.f;
}
inline float rainSoakSettled(const RainState& rain, float exposure) {
    return unit(unit(rain.wetness) * unit(exposure));
}
inline float rainSoakStep(const RainSpec& spec, float soak, const RainState& rain, float exposure, float dtS) {
    const float s = unit(soak);
    if (!(dtS > 0.f) || dtS > 1e6f) {
        return s;
    }
    const float dt = dtS < kRainMaxStepS ? dtS : kRainMaxStepS;
    const float want = rainSoakSettled(rain, exposure);
    if (want > s) {
        const float up = spec.wetRisePerS > 0.f ? s + dt * spec.wetRisePerS : want;
        return up < want ? up : want;
    }
    const float down = spec.dryPerS > 0.f ? s - dt * spec.dryPerS : s;
    return down > want ? down : want;
}
// The cover trace's clock: true when kCoverTraceS has run out. The overshoot carries into the next period and a
// small slack absorbs float rounding, so 60 fps frames trace every 15 frames, not 16. After a hitch longer than a
// period the clock restarts: one trace, never a burst. NaN, zero or negative steps pass no time and never trace.
inline constexpr float kCoverTraceSlackS = 1e-4f;
inline bool coverTraceDue(float& clock, float dtS) {
    if (!(dtS > 0.f)) {
        return false;
    }
    clock -= dtS;
    if (clock > kCoverTraceSlackS) {
        return false;
    }
    clock += kCoverTraceS;
    if (clock < kCoverTraceSlackS) {
        clock = kCoverTraceS;
    }
    return true;
}

// What one upward trace found: nothing over the head (Open), a drawn occluder at occluderZ (Hit), or no answer (Unknown:
// the trace's ignore cap ran out on undrawn hits, a hit had no component, or there was no world). Unknown never reads
// as open: the last exposure holds, and a lead with no answer yet waits.
enum class CoverTrace : unsigned char { Open, Hit, Unknown };
struct CoverAnswer {
    CoverTrace kind = CoverTrace::Unknown;
    float occluderZ = 0.f; // m, the hit (Hit only)
    float headZ = 0.f;     // m, the top of the head
};

// The party's rain soak: one value on the lead's body, kept across a lead switch (the followers take it as their weather
// value; their own cover is not traced). It owns the exposure, the trace clock and the first-step settle.
struct LeadRain {
    float soak = 0.f;
    float exposure = 1.f;
    float traceClock = 0.f;
    bool known = false; // a trace has answered (Open or Hit)
    bool set = false;   // the soak has settled once, on the first step after that answer
};

// Takes a trace's answer; true when the exposure changed (the first answer counts as a change).
inline bool leadRainCover(LeadRain& s, const CoverAnswer& a, const RainCoverSpec& spec) {
    if (a.kind == CoverTrace::Unknown) {
        return false;
    }
    const float e = coverExposure(a.kind == CoverTrace::Hit, a.occluderZ, a.headZ, spec);
    const bool changed = !s.known || e != s.exposure;
    s.exposure = e;
    s.known = true;
    return changed;
}

// Moves the soak: nothing before the first answer, the settled value on the first step after it, then rainSoakStep.
inline void leadRainSoak(LeadRain& s, const RainSpec& spec, const RainState& rain, float dtS) {
    if (!s.known) {
        return;
    }
    if (!s.set) {
        s.set = true;
        s.soak = rainSoakSettled(rain, s.exposure);
        return;
    }
    s.soak = rainSoakStep(spec, s.soak, rain, s.exposure, dtS);
}

// One step: `trace` (returns a CoverAnswer) is called only when the clock says a trace is due. True when the exposure
// changed. Steps with no time (0, negative, NaN) trace nothing and move nothing.
template <class Trace>
inline bool leadRainStep(LeadRain& s, const RainSpec& spec, const RainCoverSpec& cover, const RainState& rain, float dtS,
                         Trace&& trace) {
    const bool changed = coverTraceDue(s.traceClock, dtS) && leadRainCover(s, trace(), cover);
    leadRainSoak(s, spec, rain, dtS);
    return changed;
}

// The dev log's limiter (LEAD_RAIN lines): an exposure change logs at once, unless a line went out under kLeadRainLogGapS
// ago; then it waits for the gap to end, never dropped, and the line counts the changes it covers (flips). Otherwise a
// line every kLeadRainLogEveryS. Only while `wet` (rain on the ground or a soaked lead): a dry world logs nothing.
inline constexpr float kLeadRainLogGapS = 1.f;
inline constexpr float kLeadRainLogEveryS = 5.f;
struct LeadRainLog {
    float sinceS = kLeadRainLogEveryS; // the first wet step logs
    int held = 0;                      // changes waiting for the next line
    int flips = 0;                     // changes the line just due covers
};
inline bool leadRainLogDue(LeadRainLog& l, bool changed, bool wet, float dtS) {
    l.sinceS += dtS > 0.f && dtS < 1e6f ? dtS : 0.f;
    if (changed && wet) {
        ++l.held;
    }
    const bool due = l.held > 0 ? l.sinceS >= kLeadRainLogGapS : (wet && l.sinceS >= kLeadRainLogEveryS);
    if (!due) {
        return false;
    }
    l.flips = l.held;
    l.held = 0;
    l.sinceS = 0.f;
    return true;
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

// The wet skin shown this frame: at the target at once on the first show after a look that snaps (skinSnapOnLook), else
// eased toward it (ease). The caller clears its snap after this frame.
constexpr float shownSkin(float shown, float target, float dt, bool snap) { return snap ? unit(target) : ease(shown, target, dt); }

// The lead's index brought into a party of `count`: below 0 it reads as 0, past the party it wraps by modulo (the walk
// starts from the lead's place modulo the party, so no follower gets an empty look); `wrapped` says either happened, for
// the caller's warning. A party of none: 0, not wrapped (the caller walks its default names behind member 0).
struct PartyLead {
    int lead = 0;
    bool wrapped = false;
};
constexpr PartyLead leadInParty(int lead, int count) {
    if (count <= 0) {
        return PartyLead{};
    }
    if (lead < 0) {
        return PartyLead{0, true};
    }
    return lead < count ? PartyLead{lead, false} : PartyLead{lead % count, true};
}

// The followers as spawned, in spawn order: each one's place in the walk behind the lead (the slot ft.PartyWet names) and
// the party member it walks as (followerMember). A failed spawn leaves its place out, so a follower's index here and its
// place can differ; its place and its member never part. Fixed size, no allocation.
struct FollowerRoster {
    int count = 0;
    int place[kPartyWetMembers]{};
    int member[kPartyWetMembers]{};
    constexpr void clear() { count = 0; }
    // Adds the follower that spawned at walk place `p` behind `lead` in a party of `members` and returns its member (-1 out
    // of range, as followerMember). False from full(): nothing is added (no party walks more than kPartyWetMembers).
    constexpr bool full() const { return count >= kPartyWetMembers; }
    constexpr int add(int p, int lead, int members) {
        if (full()) {
            return -1;
        }
        place[count] = p;
        member[count] = followerMember(lead, p, members);
        return member[count++];
    }
    constexpr int placeOf(int k) const { return k >= 0 && k < count ? place[k] : -1; }
    constexpr int memberOf(int k) const { return k >= 0 && k < count ? member[k] : -1; }
    // Follower k's weather value: partyWeather by its walk place, not by k.
    constexpr float weatherOf(int k, float leadW, const PartyWetForce& force) const {
        return k >= 0 && k < count ? partyWeather(leadW, place[k], force) : unit(leadW);
    }
};

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
