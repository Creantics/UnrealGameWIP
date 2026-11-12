#include "FTArenaGlide.h"

#include "AnimationRuntime.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkinnedAsset.h"
#include "FTLocomotionAnimInstance.h"
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
#if UE_BUILD_SHIPPING
	bOn = false; // a measuring probe: never in a shipping build
#else
	bOn = bInOn;
#endif
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
	// The floor is the balls' height in the mesh's reference pose (standing on its ground), read once: the lowest ball
	// seen would sink under the feet of every step after a dodge's or a foot hold's low frame. A failed lookup is an
	// error, not tried again.
	if (!bFloorRead)
	{
		bFloorRead = true;
		const USkinnedAsset* Asset = Skin->GetSkinnedAsset();
		if (!Asset)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("Arena glide: the lead's skin has no mesh: no floor, every row INVALID."));
		}
		else
		{
			const FReferenceSkeleton& Ref = Asset->GetRefSkeleton();
			for (int32 I = 0; I < 2; ++I)
			{
				const int32 Bone = Ref.FindBoneIndex(Balls[I]);
				if (Bone == INDEX_NONE)
				{
					UE_LOG(LogForgottenThird, Error, TEXT("Arena glide: no bone %s on %s: no floor, every row INVALID."), *Balls[I].ToString(), *Asset->GetName());
					RefFloor = 1e9f;
					break;
				}
				RefFloor = FMath::Min(RefFloor, static_cast<float>(FAnimationRuntime::GetComponentSpaceTransformRefPose(Ref, Bone).GetLocation().Z));
			}
			RootRef = Ref.GetNum() > 0 ? static_cast<float>(FAnimationRuntime::GetComponentSpaceTransformRefPose(Ref, 0).GetLocation().Z) : 0.f;
		}
	}
	// Checked against the drawn mesh on every evaluated pose (a fresh one after the first): an uneven scale or a root
	// bone the animation moves off its reference height puts the floor elsewhere, and every row is then INVALID, never
	// a low slip.
	if (bHas && RefFloor < 1e8f && !bFloorBad)
	{
		const FVector Scale = ToWorld.GetScale3D();
		const float RootLive = Skin->GetNumBones() > 0 ? static_cast<float>(Skin->GetBoneTransform(0, FTransform::Identity).GetLocation().Z) : RootRef;
		const bool bEven = FMath::IsNearlyEqual(Scale.X, Scale.Z, 0.01 * FMath::Abs(Scale.X)) && FMath::IsNearlyEqual(Scale.Y, Scale.Z, 0.01 * FMath::Abs(Scale.Y));
		const bool bRoot = FMath::Abs(RootLive - RootRef) <= 2.f;
		if (!bEven || !bRoot)
		{
			bFloorBad = true;
			Floor = 1e9f;
			UE_LOG(LogForgottenThird, Error, TEXT("Arena glide: the reference floor %.1f cm does not hold on %s's evaluated pose (%s: scale %.3f,%.3f,%.3f, root bone %.1f cm, reference %.1f): every row INVALID."),
				RefFloor, *Skin->GetName(), !bEven ? TEXT("its scale is uneven") : TEXT("its root bone is off its reference height"), Scale.X, Scale.Y, Scale.Z, RootLive, RootRef);
		}
		else if (Floor >= 1e8f)
		{
			Floor = RefFloor;
			UE_LOG(LogForgottenThird, Display, TEXT("Arena glide: floor from the reference pose %.1f cm, checked on the evaluated pose: the skin's scale %.3f,%.3f,%.3f; its root bone %.1f cm (reference %.1f)."),
				RefFloor, Scale.X, Scale.Y, Scale.Z, RootLive, RootRef);
		}
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
			// Whatever the root did, a foot that jumps this far against it in one frame snapped.
			constexpr double kPopCapCm = 30.0;
			const UFTLocomotionAnimInstance* Anim = Cast<UFTLocomotionAnimInstance>(Skin->GetAnimInstance());
			const uint8 Loco = Anim ? static_cast<uint8>(Anim->GetLocomotion().state()) : 255;
			if (Loco != LastLoco)
			{
				using ftcore::LocoState;
				const int32 E = Loco == static_cast<uint8>(LocoState::Starting) ? 0
					: (Loco == static_cast<uint8>(LocoState::Stopping) ? 1 : (Loco == static_cast<uint8>(LocoState::Turning) ? 2 : -1));
				if (E >= 0)
				{
					++R.Enter[E];
				}
			}
			const double Jump0 = FVector::Dist2D(Ball[0], LastBall[0]), Jump1 = FVector::Dist2D(Ball[1], LastBall[1]);
			const FVector RootStep = Root - LastRoot;
			const double Against0 = FVector::Dist2D(Ball[0] - LastBall[0], RootStep), Against1 = FVector::Dist2D(Ball[1] - LastBall[1], RootStep);
			// The old rule: any foot over kPopCm in a frame (a run's swinging foot passes it at full pace).
			if (Jump0 > kPopCm || Jump1 > kPopCm)
			{
				++R.PopsAny;
			}
			// A run's swinging foot goes about twice the root's way: what is past that snapped, and so does any jump of
			// over kPopCapCm against the root, at whatever pace.
			const double Swing = 2.0 * RootStep.Size2D();
			if (Jump0 - Swing > kPopCm || Jump1 - Swing > kPopCm || Against0 > kPopCapCm || Against1 > kPopCapCm)
			{
				++R.Pops;
				if (PopLines < 40)
				{
					++PopLines;
					UE_LOG(LogForgottenThird, Display,
						TEXT("Arena glide pop: %s, locomotion state %d (was %d), feet moved %.0f and %.0f cm in %.1f ms, mesh local %.0f,%.0f and %.0f,%.0f (was %.0f,%.0f and %.0f,%.0f), root moved %.0f cm, yaw %.0f deg, legs told %.0f cm/s."),
						kGlideNames[Key], Loco, LastLoco, Jump0, Jump1, 1000.f * Dt, Local[0].X, Local[0].Y, Local[1].X, Local[1].Y, LastLocal[0].X,
						LastLocal[0].Y, LastLocal[1].X, LastLocal[1].Y, FVector::Dist2D(Root, LastRoot), FMath::FindDeltaAngleDegrees(LastYaw, Yaw), CycleSpeed);
				}
			}
			LastLoco = Loco;
			R.Low += FMath::Min(Local[0].Z, Local[1].Z) - (Floor < 1e8f ? Floor : 0.f);
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
	UE_LOG(LogForgottenThird, Display, TEXT("Arena glide: the floor (the balls in the reference pose, in the mesh's space) is at %.1f cm%s."),
		Floor < 1e8f ? Floor : 0.f, Floor < 1e8f ? TEXT("") : TEXT(" (none: every row INVALID)"));
	for (int32 B = 0; B < NumBuckets; ++B)
	{
		const FRow& R = Rows[B];
		if (R.Frames == 0 && R.Still == 0)
		{
			continue;
		}
		const double F = FMath::Max(1, R.Frames);
		const double P = FMath::Max(1, R.Planted);
		// Under 40% of its frames on a planted foot, a row measured too little of what it shows (or the floor is wrong):
		// INVALID, never a low slip.
		const double Share = static_cast<double>(R.Planted) / F;
		const bool bValid = Floor < 1e8f && Share >= 0.4;
		const FString Net = bValid ? FString::Printf(TEXT("net over %d plants %.0f cm/s"), R.Plants, R.PlantTime > 0.0 ? R.PlantSlip / R.PlantTime : 0.0)
			: FString::Printf(TEXT("net over %d plants INVALID"), R.Plants);
		UE_LOG(LogForgottenThird, Display,
			TEXT("Arena glide: %s: %d frames (%d stale, %.1f ms apart): root %.0f cm/s (%.0f along the facing, %.0f across), legs told %.0f cm/s, turning %.0f deg/s; planted foot on %.0f%% of frames, over %d slips %.0f cm/s (%.0f forward, %.0f across); %s; %d pops (old rule: %d); the lower ball %.1f cm over the floor; entered %d starts, %d stops, %d turns."),
			kGlideNames[B], R.Frames, R.Still, 1000.0 * R.Dt / F, R.Body / F, R.BodyAlong / F, R.BodySide / F, R.Cycle / F, R.Yaw / F, 100.0 * Share, R.Planted, R.Slip / P,
			R.SlipAlong / P, R.SlipSide / P, *Net, R.Pops, R.PopsAny, R.Low / F, R.Enter[0], R.Enter[1], R.Enter[2]);
	}
}
