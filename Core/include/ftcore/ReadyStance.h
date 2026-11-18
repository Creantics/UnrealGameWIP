#pragma once

// The ready stance: a lead's fighting stand while a fight is live and it is not acting (a partner waiting or holding,
// the controlled lead locked on and standing), drawn as offsets over the relaxed idle, so the idle's breathing stays.
//
// A stance is a table: a forward lean and a turn of the trunk (degrees), the pelvis moved forward (cm, negative back), and per
// limb bone three turns in degrees about the body's own axes, applied in component space parent first (the bones
// below follow):
//   - fwd: about the body's right axis; + swings a hanging bone's far end forward (an arm raised to the front, a knee
//     brought forward), - back.
//   - out: about the body's forward axis; + swings a hanging bone's far end away from the body's midline (left on
//     the left side, right on the right).
//   - twist: about the up axis; + turns the bone's front toward the midline.
// "out" and "twist" mirror between the sides, so a symmetric stance has the same numbers on both. A knee bend lifts
// the feet: the host lowers the pelvis by what the feet rose, so the soles stay on the floor.
//
// The weight eases in over easeIn seconds when the stance is wanted and out over easeOut (linear in the clock,
// smoothstep in the pose, readyStanceShare), and fades out with the ground speed between fadeFrom and fadeTo m/s, so a
// run keeps its arm swing.
//
// The tables live in Data/Anim/ready_stance.json (importReadyStances): stances by neutral id, and the leads' fighter
// ids mapped onto them. A missing or bad file: readyStanceDefaults().

#include "ftcore/Api.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace ftcore {

// The bones a stance turns, parent first on each chain. The names are the skeleton's.
enum class ReadyBone : std::uint8_t {
    ClavicleL, UpperArmL, LowerArmL, HandL,
    ClavicleR, UpperArmR, LowerArmR, HandR,
    ThighL, CalfL, FootL,
    ThighR, CalfR, FootR,
};
inline constexpr std::size_t kReadyBoneCount = 14;
FTCORE_API const char* readyBoneName(ReadyBone b); // "clavicle_l", "upperarm_l", ... "foot_r". Literals.
// The side of a bone: 0 left, 1 right.
inline int readyBoneSide(ReadyBone b) {
    const auto i = static_cast<std::uint8_t>(b);
    return (i >= 4 && i < 8) || i >= 11 ? 1 : 0;
}

struct ReadyTurn {
    float fwd = 0.f;
    float out = 0.f;
    float twist = 0.f;
};

struct ReadyStance {
    float lean = 0.f;          // degrees the trunk tips forward over the spine (negative back)
    float turn = 0.f;          // degrees the chest turns about up, + bringing the left shoulder forward (bladed)
    float pelvisForward = 0.f; // cm the pelvis moves along the body's forward (negative back: the weight back)
    std::array<ReadyTurn, kReadyBoneCount> bones{};
    float easeIn = 0.25f;  // s from relaxed to ready
    float easeOut = 0.4f;  // s back
    float fadeFrom = 0.6f; // m/s: at or under this the stance is whole
    float fadeTo = 2.0f;   // m/s: at or over this it is gone
};

inline constexpr std::size_t kReadyStanceMax = 6;
inline constexpr std::size_t kReadyLeadMax = 8;

struct ReadyStanceSet {
    struct Named {
        std::string id;
        ReadyStance stance;
    };
    struct Lead {
        std::string id;      // the fighter id (arena.json's "fighters"), lower case
        std::uint8_t stance = 0;
    };
    std::array<Named, kReadyStanceMax> stances{};
    std::size_t stanceCount = 0;
    std::array<Lead, kReadyLeadMax> leads{};
    std::size_t leadCount = 0;
};

// Three stances (an expert's tight guard, a bladed guard with the lead hand up, an unsure half-raised guard with the
// weight back) and no leads mapped: every lead gets the first.
FTCORE_API ReadyStanceSet readyStanceDefaults();
// The stance for a lead's fighter id (case-insensitive); the set's first when it is not mapped. Null only for an empty
// set. Call it when a body takes a lead, not per frame.
FTCORE_API const ReadyStance* readyStanceFor(const ReadyStanceSet& set, std::string_view leadId);
// Reads the file's tables over the defaults. False (out unchanged) with a reason on a bad file: unknown keys or bones,
// a bone turn out of [-150, 150] degrees, a lean out of [-30, 30], a chest turn out of [-45, 45], pelvisForward out of [-20, 20] cm, ease times out of
// [0.02, 3] s, fadeTo not above fadeFrom, a lead naming a stance that isn't there, too many stances or leads.
FTCORE_API bool importReadyStances(std::string_view json, ReadyStanceSet& out, std::string* error = nullptr);

// The eased clock: `alpha` (0..1) toward 1 at 1/easeIn per second while wanted, toward 0 at 1/easeOut otherwise.
FTCORE_API float readyStanceStep(float alpha, bool wanted, float dt, const ReadyStance& stance);
// The pose's share from the clock and the ground speed (m/s): smoothstep of alpha, times the speed fade.
FTCORE_API float readyStanceShare(float alpha, float groundSpeed, const ReadyStance& stance);

} // namespace ftcore
