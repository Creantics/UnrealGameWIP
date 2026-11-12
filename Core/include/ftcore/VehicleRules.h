#pragma once

// The party vehicle's rules around the drive (VehicleDrive): cosmetic wear, the repair garages, getting the party in
// and out, and what a pedestrian in its way does. Tunables from Data/Vehicles/vehicle_rules.json (importVehicleRules);
// the numbers there are provisional.
//
// Wear (VehicleWear) is cosmetic only: nothing in VehicleDrive reads it, so handling never changes and the vehicle never
// fails. Each of four zones (front, rear, left, right) gathers wear from impacts above minImpact (one stage per
// impactPerStage of speed into the hit), and the body gathers dirt by distance. Stages run 0 (clean) to 4 (battered);
// the host shows them as material parameters and swapped panels.
//
// Repair (RepairService): garages are data rows. In one, a repair costs pricePerStage per zone stage and advances the
// clock by minutes. Short of money it repairs what it can afford, one stage at a time off the worst zone; the dirt is
// washed off free on any visit. Wear under one stage is polished out only by a repair that fixed something.
//
// Seats (PartySeats): the party's own vehicle only. Entering needs the lead within enterReach of it and it all but
// stopped; the lead takes the driver's seat, the followers walk to theirs (those farther than boardFar are placed in
// when the doors shut), and the drive starts when the lead is seated and the near followers are in, waiting at most
// boardWait for them. Exit is held for exitHold with the vehicle slower than exitMaxSpeed; the party steps out on the
// driver's side, the other side when that is blocked, or stays in when both are. While anyone is getting in or out
// holdForSeats() keeps the vehicle stopped.
//
// Pedestrians: applySpeedLimits() folds the nearest pedestrian ahead into the drive's input each frame. The cap
// (pedestrianCap) is a braking curve: the speed from which brakeDecel brings the vehicle down to nearCap (25 km/h) by
// brakeMargin before nearDistance, so the slow-down starts farther out the faster it goes; inside nearDistance the speed is clamped hard
// at nearCap. A pedestrian in the vehicle's path (within halfWidth + clearance of its centre line) also brings it to a
// stop stopGap short of them. pedestrianReact() is what one does: steps aside when there is time to (reaction plus the
// sidestep clear of the body), else is met at the speed left after braking, never above nearCap: below stumbleBelow a
// stumble, at or above it a fall and downTime on the ground. Never lethal.
//
// Positions and distances in cm (garage rows in metres in the file), speeds in cm/s, time in seconds. Nothing
// allocates after the import.

#include "ftcore/Api.h"
#include "ftcore/VehicleDrive.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace ftcore {

enum class WearZone : std::uint8_t { Front, Rear, Left, Right };
constexpr int kWearZones = 4;
constexpr int kWearMaxStage = 4;

struct WearTuning {
    float minImpact = 150.f;     // impacts slower than this into the hit leave nothing (cm/s)
    float impactPerStage = 400.f; // one stage of wear per this much speed above minImpact (cm/s)
    float dirtPerKm = 0.25f;     // dirt stages per km driven
};

struct WearState {
    float zone[kWearZones] = {0.f, 0.f, 0.f, 0.f}; // 0 to 4; the stage is the whole part
    float dirt = 0.f;                               // 0 to 4
};

struct Garage {
    char id[24] = {};
    float x = 0.f, y = 0.f; // centre of the bay (cm)
    float radius = 800.f;   // inside this the repair is offered (cm)
};

constexpr int kMaxGarages = 8;

struct RepairTuning {
    int pricePerStage = 50;
    float minutes = 30.f;   // the clock advances this much for a repair
    int garageCount = 0;
    Garage garages[kMaxGarages];
};

struct RepairResult {
    int stagesFixed = 0;
    int spent = 0;
    float minutes = 0.f;
    bool washed = false;
};

struct SeatTuning {
    float enterReach = 200.f;    // the lead must be this close to the vehicle (cm)
    float enterMaxSpeed = 50.f;  // the vehicle all but stopped (cm/s)
    float enterTime = 1.2f;      // the lead's door-and-sit (s)
    float exitTime = 0.9f;
    float exitHold = 0.3f;       // the exit input held this long (s)
    float exitMaxSpeed = 100.f;  // only slower than this (cm/s)
    float followerSpeed = 450.f; // followers walk to their doors at this (cm/s)
    float boardFar = 1500.f;     // followers farther than this are placed in at the door shut (cm)
    float boardWait = 3.f;       // the drive waits at most this long for near followers (s)
};

enum class SeatPhase : std::uint8_t { OnFoot, Entering, Seated, Exiting };
enum class ExitSide : std::uint8_t { None, Driver, Passenger };

constexpr int kMaxParty = 3;

struct PartySeats {
    SeatPhase phase = SeatPhase::OnFoot;
    float time = 0.f;            // into the phase (s)
    float length = 0.f;          // how long Entering or Exiting lasts (s)
    int count = 0;               // party members (driver first)
    float boardAt[kMaxParty] = {0.f, 0.f, 0.f}; // when each is in (s into Entering)
    bool aboard[kMaxParty] = {false, false, false};
    float exitHeld = 0.f;
    ExitSide side = ExitSide::None;
    bool canDrive() const { return phase == SeatPhase::Seated; }
};

struct PedestrianTuning {
    float nearDistance = 800.f;  // a pedestrian this close ahead caps the vehicle (cm)
    float nearCap = 694.f;       // at this (cm/s; 25 km/h)
    float coneAngle = 35.f;      // half the cone ahead in which pedestrians react (degrees)
    float reaction = 0.3f;       // before they move (s)
    float sidestepSpeed = 350.f; // (cm/s)
    float halfWidth = 74.f;      // half the vehicle's body (cm)
    float clearance = 50.f;      // a pedestrian this far beside the body is clear (cm)
    float stumbleBelow = 278.f;  // a contact slower than this is a stumble, not a fall (cm/s; 10 km/h)
    float stumbleTime = 0.8f;    // (s)
    float downTime = 2.5f;       // on the ground after a fall, before the get-up (s)
    float brakeDecel = 1100.f;   // the brake for a pedestrian, firmer than the drive's governor (cm/s^2)
    float stopGap = 100.f;       // stops this short of a pedestrian in its path (cm)
    float brakeMargin = 150.f;   // the slow-down to nearCap ends this far before nearDistance (cm)
};

enum class PedReact : std::uint8_t { None, Dodge, Stumble, KnockDown };

struct PedOutcome {
    PedReact react = PedReact::None;
    float sidestep = 0.f; // Dodge: cm to the side, + toward the vehicle's right (+y of its heading)
    float down = 0.f;     // Stumble or KnockDown: seconds before they are back on their feet
    float contact = 0.f;  // Stumble or KnockDown: the speed they are met at (cm/s, at most nearCap)
};

struct VehicleRules {
    WearTuning wear;
    RepairTuning repair;
    SeatTuning seats;
    PedestrianTuning pedestrian;
};

// Wear. (lx, ly) is where the hit was, in the vehicle's frame (+x ahead, +y right); speed is collide()'s return.
FTCORE_API WearZone wearZoneAt(float lx, float ly);
FTCORE_API void addImpact(const WearTuning& t, WearState& s, float speed, float lx, float ly);
FTCORE_API void addDistance(const WearTuning& t, WearState& s, float cm);
FTCORE_API int wearStage(const WearState& s, WearZone z);
FTCORE_API int dirtStage(const WearState& s);

// Lamps. Headlights and tail lamps come on in the night band with a driver in the seat, so a van parked empty at
// night stands dark; brake lamps light under brake input past kBrakeLampInput or, with a driver in the seat, while
// the handbrake holds.
inline constexpr float kBrakeLampInput = 0.1f;
// The night band switches on at kNightOn and off at kNightOff of the clock's night amount, so dusk doesn't flicker.
inline constexpr float kNightOn = 0.55f;
inline constexpr float kNightOff = 0.45f;
struct LampState {
    bool head = false;
    bool tail = false;
    bool brake = false;
};
FTCORE_API bool nightBand(float nightAmount, bool wasNight);
FTCORE_API LampState lampState(bool night, float brake, bool handbrake, bool driverSeated);

// Repair. The garage index at (x, y), or -1; what a full repair costs; the repair itself with `money` to spend.
FTCORE_API int garageAt(const RepairTuning& t, float x, float y);
FTCORE_API int repairQuote(const RepairTuning& t, const WearState& s);
FTCORE_API RepairResult repair(const RepairTuning& t, WearState& s, int money);

// Seats. holdForSeats: while getting in or out, the input becomes a stop (no throttle, brake or steer; handbrake on).
// tryEnter: `leadDistance` from the vehicle, its speed, whether it is the party's own, and each follower's
// distance to it (`followers` of them, at most 2). tick: moves the phases on; `exitPressed` is the exit input held,
// with which side is clear. False when a move isn't allowed (nothing changes).
FTCORE_API bool tryEnter(const SeatTuning& t, PartySeats& p, float leadDistance, float vehicleSpeed, bool partyVehicle,
                         const float* followerDistance, int followers);
FTCORE_API void tickSeats(const SeatTuning& t, PartySeats& p, float dt, float vehicleSpeed, bool exitPressed,
                          bool driverSideClear, bool passengerSideClear);
FTCORE_API void holdForSeats(const PartySeats& p, VehicleInput& in);

// Pedestrians. (ahead, side): the nearest pedestrian from the vehicle's front, in its frame (cm; kNobodyAhead: none).
// Bad numbers fail safe: ahead not a number caps at nearCap, ahead negative is at the bumper, side not a number is in
// the path.
// pedestrianCap: the speed limit they set (cm/s; 0: none). combinedLimit: the lower of two limits, 0 meaning none
// (0 never wipes a real limit). applySpeedLimits: sets in.limit, in.limitDecel and in.clamp from the road's limit and
// the pedestrian. pedestrianReact: what the pedestrian does with the vehicle at `speed`.
inline constexpr float kNobodyAhead = 1e30f;
FTCORE_API float pedestrianCap(const PedestrianTuning& t, float ahead, float side);
FTCORE_API float combinedLimit(float a, float b);
FTCORE_API void applySpeedLimits(const PedestrianTuning& t, VehicleInput& in, float roadLimit, float ahead, float side);
FTCORE_API PedOutcome pedestrianReact(const PedestrianTuning& t, float speed, float ahead, float side);

// Reads {"version": 1, "wear": {...}, "repair": {..., "garages": [{"id", "x", "y", "radius"} in metres]},
// "seats": {...}, "pedestrian": {...}} into `out`. Unknown members are refused; on failure `out` is unchanged.
FTCORE_API bool importVehicleRules(std::string_view json, VehicleRules& out, std::string* error = nullptr);

} // namespace ftcore
