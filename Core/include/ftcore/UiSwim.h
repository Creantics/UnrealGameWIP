#pragma once
// The underwater HUD (UI round 18a): no bar and no number for breath. From warningSeconds of breath left the picture's
// edges darken and its clear middle narrows (the Tunnel ring, drawn by the host under every mark); a heartbeat rate
// and beat for the audio lane; a rescue shows only a fade to black and back; the hints the swim flags refuse hide, and
// Crouch and Jump always (their keys read DIVE and RISE in the water); DIVE and RISE stand whenever each applies until
// first used, under the teaching setting (counted on use, so a save that never dived is taught again).
//
// This header does not include Swim.h: the host fills UiSwimIn from its swim step each frame.
//
//   UiSwim s; UiSwimIn in = <from the step>;
//   each frame: if (uiSwimStep(s, firstUse, in, dt)) save uiFirstUseSaved(firstUse);
//   draw the ring at uiSwimEdgeAlpha(s) / uiSwimClearRadius(s), the fade at uiSwimFadeAlpha(s); on s.beat raise the
//   heart event (s.bpm, s.ring as its strength); rebuild the hint line when uiSwimHidden(s, in) changes;
//   DIVE and RISE at uiSwimPromptAlpha(s, firstUse, in, item).
//
// Header-only, plain values, zero allocation.
#include "ftcore/UiFirstUse.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ftcore {

inline constexpr float kUiSwimBreathPart = 0.75f; // the breath curve's share of the ring; the drain takes the rest
inline constexpr float kUiSwimEdgeMax = 0.9f;     // the ring's alpha at its strongest
inline constexpr float kUiSwimNarrow = 0.6f;      // how far the clear middle shrinks (share of the half-diagonal)
inline constexpr float kUiSwimSoft = 0.6f;        // the ring's soft width (share of the half-diagonal)
inline constexpr float kUiSwimSlew = 4.f;         // the ring's most change per second
inline constexpr float kUiSwimFadeOut = 0.35f;    // a rescue: to black
inline constexpr float kUiSwimFadeHold = 0.2f;    // black after the host's reset
inline constexpr float kUiSwimBlackMax = 4.f;     // black at most, reset or not
inline constexpr float kUiSwimFadeIn = 0.6f;      // back to the picture
inline constexpr float kUiSwimHeartRest = 72.f;   // bpm at warningSeconds left
inline constexpr float kUiSwimHeartWarn = 120.f;  // bpm with no breath left
inline constexpr float kUiSwimHeartDrain = 144.f; // bpm as the vitality runs out

// What the host reads from its swim step (Swim.h's SwimStep and tuning) each frame.
struct UiSwimIn {
    bool swimming = false;  // the lead's swim was stepped this frame
    bool under = false;     // Diving, Under or Surfacing
    bool rescued = false;   // Rescued: from the rescue step until the host's reset at the exit
    bool canDive = false;   // a dive press now would dive
    bool canRise = false;   // a rise press now would surface (Diving or Under)
    bool dived = false;     // this step went from the surface to Diving
    bool rose = false;      // this step went from Diving or Under to Surfacing
    bool warning = false;   // breath in use with warningSeconds or less left
    bool draining = false;  // no breath left under water: vitality drains
    bool attack = true;     // the swim flags (all true out of the water)
    bool dodge = true;
    bool guard = true;
    bool weaponsOut = true;
    float breath = 0.f;           // seconds left
    float warningSeconds = 10.f;  // the tuning's
    float vitality = 1.f;         // the share left (0..1)
    ShowBasicControls teach = ShowBasicControls::FirstFights;
};

enum class UiSwimFade : std::uint8_t { Clear, Out, Black, In };

struct UiSwim {
    float ring = 0.f;           // the ring's strength K (0..1), slewed
    float drainFrom = 1.f;      // the vitality when the drain began
    bool wasDraining = false;
    bool wasRescued = false;
    UiSwimFade fade = UiSwimFade::Clear;
    float fadeT = 0.f;          // seconds in this fade phase
    float fadeAfter = 0.f;      // Black: seconds since the reset
    bool heart = false;         // the heartbeat is on
    bool beat = false;          // a beat falls on this frame
    float phase = 0.f;          // the beat's phase (0..1)
    float bpm = 0.f;            // the heart rate; 0 when off
};

// The hints the swim hides, a bit each (uiSwimHintBit). Crouch and Jump hide whenever the lead swims (ruled): their keys
// read DIVE and RISE there.
enum class UiSwimHint : std::uint8_t { Attack, Aim, Dodge, Guard, Crouch, Jump };
using UiSwimHints = std::uint32_t;
constexpr UiSwimHints uiSwimHintBit(UiSwimHint h) { return 1u << static_cast<unsigned>(h); }
inline constexpr UiSwimHints kUiSwimHintAll = 0x3Fu;

// Smoothstep over 0..1, clamped.
inline float uiSwimEase(float x) {
    const float c = std::clamp(x, 0.f, 1.f);
    return c * c * (3.f - 2.f * c);
}
// 0 at warningSeconds of breath left or more, 1 at none (a step at 0 when warningSeconds is 0).
inline float uiSwimBreathCurve(float breath, float warningSeconds) {
    if (!(warningSeconds > 0.f)) return breath > 0.f ? 0.f : 1.f;
    return std::clamp(1.f - breath / warningSeconds, 0.f, 1.f);
}
// 0 when the drain began, 1 at no vitality left.
inline float uiSwimDrainCurve(float vitality, float from) {
    if (!(from > 0.f)) return 1.f;
    return std::clamp(1.f - vitality / from, 0.f, 1.f);
}
// The ring's strength from the two curves.
inline float uiSwimRingTarget(float breathCurve, float drainCurve) {
    return kUiSwimBreathPart * uiSwimEase(breathCurve) + (1.f - kUiSwimBreathPart) * uiSwimEase(drainCurve);
}
// The heartbeat's rate: rest to warn over the breath curve, warn to drain over the drain curve while draining.
inline float uiSwimHeartRate(float breathCurve, float drainCurve, bool draining) {
    if (draining) return kUiSwimHeartWarn + (kUiSwimHeartDrain - kUiSwimHeartWarn) * uiSwimEase(drainCurve);
    return kUiSwimHeartRest + (kUiSwimHeartWarn - kUiSwimHeartRest) * uiSwimEase(breathCurve);
}

// One frame. `firstUse` is the explore HUD's teaching (nothing else ticks it); true when its counts changed (the host
// saves then, never per frame).
inline bool uiSwimStep(UiSwim& s, UiFirstUse& firstUse, const UiSwimIn& in, float dt) {
    const float secs = std::max(dt, 0.f);

    // The rescue: to black, held while rescued and a moment after the reset, then back.
    if (in.rescued && !s.wasRescued) {
        if (s.fade == UiSwimFade::Clear) {
            s.fade = UiSwimFade::Out;
            s.fadeT = 0.f;
        } else if (s.fade == UiSwimFade::In) {
            s.fade = UiSwimFade::Black;
            s.fadeT = 0.f;
            s.fadeAfter = 0.f;
        }
    }
    s.wasRescued = in.rescued;
    switch (s.fade) {
    case UiSwimFade::Clear: break;
    case UiSwimFade::Out:
        s.fadeT += secs;
        if (s.fadeT >= kUiSwimFadeOut) {
            s.fade = UiSwimFade::Black;
            s.fadeT = 0.f;
            s.fadeAfter = 0.f;
        }
        break;
    case UiSwimFade::Black:
        s.fadeT += secs;
        if (!in.rescued) s.fadeAfter += secs;
        if (s.fadeAfter >= kUiSwimFadeHold || s.fadeT >= kUiSwimBlackMax) {
            s.fade = UiSwimFade::In;
            s.fadeT = 0.f;
        }
        break;
    case UiSwimFade::In:
        s.fadeT += secs;
        if (s.fadeT >= kUiSwimFadeIn) {
            s.fade = UiSwimFade::Clear;
            s.fadeT = 0.f;
        }
        break;
    }

    // The ring: held while the picture goes to black, snapped while it is black, slewed otherwise.
    const float u = uiSwimBreathCurve(in.breath, in.warningSeconds);
    if (in.draining && !s.wasDraining) s.drainFrom = in.vitality;
    s.wasDraining = in.draining;
    const float d = in.draining ? uiSwimDrainCurve(in.vitality, s.drainFrom) : 0.f;
    const float target = in.swimming ? uiSwimRingTarget(u, d) : 0.f;
    if (s.fade == UiSwimFade::Black) {
        s.ring = target;
    } else if (s.fade != UiSwimFade::Out) {
        const float most = kUiSwimSlew * secs;
        s.ring = std::clamp(target, s.ring - most, s.ring + most);
    }

    // The heartbeat: from the warning (or the drain) until the surface, an air pocket or the rescue.
    const bool heartOn = in.swimming && !in.rescued && s.fade == UiSwimFade::Clear && (in.warning || in.draining);
    s.beat = false;
    if (!heartOn) {
        s.heart = false;
        s.phase = 0.f;
        s.bpm = 0.f;
    } else {
        s.bpm = uiSwimHeartRate(u, d, in.draining);
        if (!s.heart) {
            s.heart = true;
            s.phase = 0.f;
            s.beat = true; // the first beat falls on the frame it starts
        } else {
            s.phase += secs * s.bpm / 60.f;
            if (s.phase >= 1.f) {
                s.phase -= std::floor(s.phase);
                s.beat = true;
            }
        }
    }

    // DIVE and RISE: shown whenever each applies until first used (FirstFights only). Showing counts nothing; the first
    // use counts (uiFirstUseCountsOnUse), and that is the only change the host saves.
    if (in.teach == ShowBasicControls::FirstFights && in.swimming && s.fade == UiSwimFade::Clear) {
        if (in.canDive) uiFirstUseShow(firstUse, UiFirstUseItem::Dive);
        if (in.canRise) uiFirstUseShow(firstUse, UiFirstUseItem::Rise);
    }
    bool changed = false;
    if (in.dived) changed = uiFirstUseUsed(firstUse, UiFirstUseItem::Dive) || changed;
    if (in.rose) changed = uiFirstUseUsed(firstUse, UiFirstUseItem::Rise) || changed;
    uiFirstUseTick(firstUse, secs);
    return changed;
}

// The ring's alpha at the picture's corners, and the clear middle's radius (a share of the half-diagonal).
inline float uiSwimEdgeAlpha(const UiSwim& s) { return kUiSwimEdgeMax * s.ring; }
inline float uiSwimClearRadius(const UiSwim& s) { return 1.f - kUiSwimNarrow * s.ring; }

// The rescue's black over the picture and the HUD (0 when clear).
inline float uiSwimFadeAlpha(const UiSwim& s) {
    switch (s.fade) {
    case UiSwimFade::Clear: return 0.f;
    case UiSwimFade::Out: return uiSwimEase(s.fadeT / kUiSwimFadeOut);
    case UiSwimFade::Black: return 1.f;
    case UiSwimFade::In: return 1.f - uiSwimEase(s.fadeT / kUiSwimFadeIn);
    }
    return 0.f;
}

// The hints to leave out of the controls line: Crouch and Jump in the water, what the swim flags refuse, and every hint
// while the rescue fades.
inline UiSwimHints uiSwimHidden(const UiSwim& s, const UiSwimIn& in) {
    if (s.fade != UiSwimFade::Clear) return kUiSwimHintAll;
    if (!in.swimming) return 0u;
    UiSwimHints hidden = uiSwimHintBit(UiSwimHint::Crouch) | uiSwimHintBit(UiSwimHint::Jump);
    if (!in.attack) hidden |= uiSwimHintBit(UiSwimHint::Attack);
    if (!in.weaponsOut) hidden |= uiSwimHintBit(UiSwimHint::Aim);
    if (!in.dodge) hidden |= uiSwimHintBit(UiSwimHint::Dodge);
    if (!in.guard) hidden |= uiSwimHintBit(UiSwimHint::Guard);
    return hidden;
}

// DIVE's or RISE's alpha: 0 unless it applies now (in the water, no fade); then by the teaching setting.
inline float uiSwimPromptAlpha(const UiSwim& s, const UiFirstUse& firstUse, const UiSwimIn& in, UiFirstUseItem item) {
    const bool applies = item == UiFirstUseItem::Dive ? in.canDive : item == UiFirstUseItem::Rise ? in.canRise : false;
    if (!applies || !in.swimming || s.fade != UiSwimFade::Clear) return 0.f;
    switch (in.teach) {
    case ShowBasicControls::Always: return 1.f;
    case ShowBasicControls::Never: return 0.f;
    case ShowBasicControls::FirstFights: break;
    }
    return uiFirstUseAlpha(firstUse, item);
}

} // namespace ftcore
