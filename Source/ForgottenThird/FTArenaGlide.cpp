#include "FTArenaGlide.h"

#include "Components/SkeletalMeshComponent.h"
#include "ForgottenThird.h"

namespace
{
const TCHAR* const kGlideNames[FFTArenaGlideProbe::NumBuckets] = {TEXT("idle"), TEXT("move steady"), TEXT("move speeding up"),
	TEXT("move braking"), TEXT("move turning"), TEXT("move after a hit"),
	TEXT("light 0 strike"), TEXT("light 1 strike"), TEXT("light 2 strike"), TEXT("light 3 strike"), TEXT("light recovery"),
	TEXT("heavy startup"), TEXT("heavy active"), TEXT("heavy recovery"), TEXT("riposte or release"), TEXT("dodge"),
	TEXT("hitstun"), TEXT("guard or heal"), TEXT("hit pause"), TEXT("other")};
constexpr double kGlideAfterHit = 0.6;
constexpr float kGlideAccel = 2.f; // m/s^2: a move whose legs' speed changes faster is speeding up or braking
constexpr float kGlideTurn = 1.5f; // rad/s: a move whose facing turns faster is turning
} // namespace

void FFTArenaGlideProbe::Begin(bool bInOn)
{
	*this = FFTArenaGlideProbe();
	bOn = bInOn;
}

void FFTArenaGlideProbe::Drawn(const ftcore::ArenaPlayerOut& P, double Now, float LegSpeed)
{
	if (!bOn)
	{
		return;
	}
	using ftcore::ArenaAction;
	using ftcore::ArenaPart;
	if (LastAction == ArenaAction::Hitstun && P.action != ArenaAction::Hitstun)
	{
		AfterHitUntil = Now + kGlideAfterHit;
	}
	LastAction = P.action;
	CycleSpeed = 100.f * LegSpeed;
	const float Facing = FMath::Atan2(P.facing.y, P.facing.x);
	const double Since = LastNow >= 0.0 ? Now - LastNow : 0.0;
	const float Accel = Since > 0.0 ? static_cast<float>((LegSpeed - LastLeg) / Since) : 0.f;
	const float Turn = Since > 0.0 ? static_cast<float>(FMath::Abs(FMath::FindDeltaAngleRadians(LastFacing, Facing)) / Since) : 0.f;
	LastNow = Now;
	LastLeg = LegSpeed;
	LastFacing = Facing;
	if (P.stopLeft > 0.f)
	{
		Key = HitPause;
		return;
	}
	switch (P.action)
	{
	case ArenaAction::Idle:
	case ArenaAction::Move:
		if (Now < AfterHitUntil)
		{
			Key = MoveAfterHit;
		}
		else if (P.action == ArenaAction::Idle)
		{
			Key = Idle;
		}
		else
		{
			Key = Turn > kGlideTurn ? MoveTurning : (Accel > kGlideAccel ? MoveFaster : (Accel < -kGlideAccel ? MoveSlower : Move));
		}
		break;
	case ArenaAction::Light:
		if (P.part == ArenaPart::Recovery)
		{
			Key = LightRecovery;
		}
		else
		{
			Key = static_cast<EBucket>(LightStrike0 + FMath::Min<int32>(P.move, 3));
		}
		break;
	case ArenaAction::Heavy:
		Key = P.part == ArenaPart::Startup ? HeavyStartup : (P.part == ArenaPart::Active ? HeavyActive : HeavyRecovery);
		break;
	case ArenaAction::Riposte:
	case ArenaAction::Release:
		Key = RiposteRelease;
		break;
	case ArenaAction::Dodge:
		Key = Dodge;
		break;
	case ArenaAction::Hitstun:
		Key = Hitstun;
		break;
	case ArenaAction::Guard:
	case ArenaAction::Heal:
		Key = GuardHeal;
		break;
	default:
		Key = Other;
		break;
	}
}

void FFTArenaGlideProbe::Sample(const USkeletalMeshComponent* Skin, float DeltaSeconds)
{
	if (!bOn || !Skin || DeltaSeconds <= 0.f)
	{
		return;
	}
	static const FName Balls[2] = {TEXT("ball_l"), TEXT("ball_r")};
	FVector Ball[2];
	for (int32 I = 0; I < 2; ++I)
	{
		Ball[I] = Skin->GetBoneLocation(Balls[I], EBoneSpaces::WorldSpace);
	}
	const FTransform& ToWorld = Skin->GetComponentTransform();
	const FVector Root = ToWorld.GetLocation();
	FVector Local[2];
	for (int32 I = 0; I < 2; ++I)
	{
		Local[I] = ToWorld.InverseTransformPosition(Ball[I]);
	}
	// A pose not evaluated since the last sample (the same feet in the mesh's space while the mesh moved) is stale: the
	// root moved under a frozen pose, which is no glide of the clip's. Counted apart, and left out.
	const bool bStale = bHas && Local[0].Equals(LastLocal[0], 0.01) && Local[1].Equals(LastLocal[1], 0.01);
	if (bHas && bStale)
	{
		// The next fresh pose is measured against the last fresh one, over the time between them.
		++Rows[Key].Still;
		PendingDt += DeltaSeconds;
		return;
	}
	const float Yaw = static_cast<float>(ToWorld.Rotator().Yaw);
	const float Dt = DeltaSeconds + PendingDt;
	PendingDt = 0.f;
	if (bHas)
	{
		FRow& R = Rows[Key];
		const FVector Fwd = Skin->GetOwner() ? Skin->GetOwner()->GetActorForwardVector() : FVector::ForwardVector;
		// Along the facing (signed: + forward) and across it (unsigned: left and right would cancel).
		auto Split = [&Fwd](const FVector& D, double& Along, double& Side)
		{
			Along += D.X * Fwd.X + D.Y * Fwd.Y;
			Side += FMath::Abs(D.Y * Fwd.X - D.X * Fwd.Y);
		};
		const FVector BodyV = (Root - LastRoot) / Dt;
		{
			++R.Frames;
			R.Body += BodyV.Size2D();
			R.Dt += Dt;
			R.Cycle += CycleSpeed;
			R.Yaw += FMath::Abs(FMath::FindDeltaAngleDegrees(LastYaw, Yaw)) / Dt;
			Split(BodyV, R.BodyAlong, R.BodySide);
			int32 Foot = INDEX_NONE;
			double Slowest = 0.0;
			// A foot that moved more than this in one fresh frame snapped: a pose pop, not a slide.
			constexpr double kPopCm = 20.0;
			if (FVector::Dist2D(Ball[0], LastBall[0]) > kPopCm || FVector::Dist2D(Ball[1], LastBall[1]) > kPopCm)
			{
				++R.Pops;
			}
			for (int32 I = 0; I < 2; ++I)
			{
				const float Height = static_cast<float>(Local[I].Z);
				const double Speed = FVector::Dist2D(Ball[I], LastBall[I]) / Dt;
				if (Floor < 1e8f && Height < Floor + 3.f && (Foot == INDEX_NONE || Speed < Slowest))
				{
					Foot = I;
					Slowest = Speed;
				}
			}
			if (Foot != INDEX_NONE)
			{
				++R.Planted;
				R.Slip += Slowest;
				Split((Ball[Foot] - LastBall[Foot]) / Dt, R.SlipAlong, R.SlipSide);
			}
			// A plant ends when the slowest foot on the floor changes: its net slip, start to end, leaves the frame to
			// frame jitter out (as the field's -LocoSlide). Under 0.08 s it is a flip between the feet, not a plant.
			if (Foot != PlantFoot)
			{
				if (PlantFoot != INDEX_NONE && PlantTime >= 0.08f)
				{
					FRow& PR = Rows[PlantKey];
					++PR.Plants;
					PR.PlantSlip += FVector::Dist2D(LastBall[PlantFoot], PlantStart);
					PR.PlantTime += PlantTime;
				}
				PlantFoot = Foot;
				PlantTime = 0.f;
				PlantKey = Key;
				if (Foot != INDEX_NONE)
				{
					PlantStart = LastBall[Foot];
				}
			}
			if (Foot != INDEX_NONE)
			{
				PlantTime += Dt;
			}
		}
	}
	for (int32 I = 0; I < 2; ++I)
	{
		Floor = FMath::Min(Floor, static_cast<float>(Local[I].Z));
		LastBall[I] = Ball[I];
		LastLocal[I] = Local[I];
	}
	LastRoot = Root;
	LastYaw = Yaw;
	bHas = true;
}

void FFTArenaGlideProbe::Log() const
{
	if (!bOn)
	{
		return;
	}
	for (int32 B = 0; B < NumBuckets; ++B)
	{
		const FRow& R = Rows[B];
		if (R.Frames == 0 && R.Still == 0)
		{
			continue;
		}
		const double F = FMath::Max(1, R.Frames);
		const double P = FMath::Max(1, R.Planted);
		UE_LOG(LogForgottenThird, Display,
			TEXT("Arena glide: %s: %d frames (%d stale, %.1f ms apart): root %.0f cm/s (%.0f along the facing, %.0f across), legs told %.0f cm/s, turning %.0f deg/s; planted foot over %d slips %.0f cm/s (%.0f forward, %.0f across); net over %d plants %.0f cm/s; %d pops (a foot over 20 cm in a frame)."),
			kGlideNames[B], R.Frames, R.Still, 1000.0 * R.Dt / F, R.Body / F, R.BodyAlong / F, R.BodySide / F, R.Cycle / F, R.Yaw / F, R.Planted, R.Slip / P, R.SlipAlong / P,
			R.SlipSide / P, R.Plants, R.PlantTime > 0.0 ? R.PlantSlip / R.PlantTime : 0.0, R.Pops);
	}
}
