#pragma once

// The one place the lead's free swim (ftcore::Swim, metres, Z up, Unreal's axes) meets the engine (centimetres). Both
// frames share their axes (x forward, y right, z up), so the conversion is a scale and nothing turns over. The swim's
// body reference point is the lead's feet raised by the swimmer's draft (ftcore::kWaterSwimDraft): at the surface hold
// the feet hang the draft under the surface, so the point rides on it, and a dive of diveDepth takes the feet that far
// further down.
//
// Swim holds its reference point SwimTuning::bedClearance off the bed and caps it at the surface. The cap is the surface
// hold's own (the feet the draft under the surface). The bed's clearance is not: the point rides the draft above the
// feet, so a clearance read straight from Data/Swim/swim.json would put the feet about 1 m into the floor Swim thinks it
// keeps them off. TuningForRef raises the clearance by the draft, so the data's bedClearance is the FEET's real height
// off the bed (capped at the tuning's own bound, ftcore::kSwimBedClearanceMax: 2 m of point, about 0.7 m of feet).

#include "CoreMinimal.h"

#include "ftcore/Swim.h"
#include "ftcore/WaterBodies.h"

namespace FTSwimUnits
{
inline constexpr double kCmPerMetre = 100.0;

inline float ToMetres(double Cm) { return static_cast<float>(Cm / kCmPerMetre); }
inline double ToCm(float Metres) { return static_cast<double>(Metres) * kCmPerMetre; }

inline ftcore::SwimVec ToSwim(const FVector& Cm) { return {ToMetres(Cm.X), ToMetres(Cm.Y), ToMetres(Cm.Z)}; }
inline FVector ToEngine(const ftcore::SwimVec& M) { return FVector(ToCm(M.x), ToCm(M.y), ToCm(M.z)); }
// A direction or a velocity: the same scale (m/s to cm/s).
inline FVector VelocityToEngine(const ftcore::SwimVec& M) { return ToEngine(M); }
// A unit direction: no scale.
inline ftcore::SwimVec DirectionToSwim(const FVector& Unit)
{
	return {static_cast<float>(Unit.X), static_cast<float>(Unit.Y), static_cast<float>(Unit.Z)};
}
// The plan point of a place for ftcore::WaterBodies (the engine's frame, metres).
inline ftcore::Vec2 ToPlan(const FVector& Cm) { return {ToMetres(Cm.X), ToMetres(Cm.Y)}; }

// The swim's reference point (m) for an actor at ActorCm with a capsule of HalfCm, and back.
inline ftcore::SwimVec RefFromActor(const FVector& ActorCm, double HalfCm)
{
	return ToSwim(ActorCm + FVector(0.0, 0.0, ToCm(ftcore::kWaterSwimDraft) - HalfCm));
}
inline FVector ActorFromRef(const ftcore::SwimVec& Ref, double HalfCm)
{
	return ToEngine(Ref) - FVector(0.0, 0.0, ToCm(ftcore::kWaterSwimDraft) - HalfCm);
}
// The tuning Swim is given: the data's (feet) bed clearance raised to the reference point's (the header's note).
inline ftcore::SwimTuning TuningForRef(ftcore::SwimTuning Tuning)
{
	Tuning.bedClearance = FMath::Min(Tuning.bedClearance + ftcore::kWaterSwimDraft, ftcore::kSwimBedClearanceMax);
	return Tuning;
}

// Whether the step's position may be used to move the body. The core lane's fix round adds SwimStep::positionValid;
// this is the only line that reads it.
inline bool PositionValid(const ftcore::SwimStep& Step) { return Step.positionValid; }
} // namespace FTSwimUnits
