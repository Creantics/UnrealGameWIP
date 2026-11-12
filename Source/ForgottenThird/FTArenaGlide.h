#pragma once

// -ArenaGlide: how far the lead's planted foot slides over the ground against how fast its root moves, by what the lead is
// doing (the action and its part; a light by its chain index). A planted foot that slides while the root travels is the
// glide a player sees. The planted foot is the slower ball bone within 3 cm of the balls' height in the mesh's reference
// pose (in the mesh's space). Sample at the start of a frame, before the figures are placed: the bones then are the pose drawn last frame at
// last frame's placement, and Drawn names what that pose showed. Body speed is the root's drawn displacement over the
// frame, not Core's velocity (the figures are drawn between steps). The hit pause and the frames a pose did not move are
// their own rows. Nothing here allocates per frame.

#include "CoreMinimal.h"
#include "ftcore/ArenaFight.h"

class USkeletalMeshComponent;

struct FFTArenaGlideProbe
{
	enum EBucket : int32
	{
		Idle,
		Move, // steady: the legs' speed changing slower than kGlideAccel and the facing turning slower than kGlideTurn
		MoveFaster, // speeding up
		MoveSlower, // braking
		MoveTurning, // turning, whatever the speed does
		MoveAfterHit, // Idle or Move within 0.6 s of a hitstun's end
		LightStrike0, // a light's startup and active, by chain index (a shot is LightStrike3's row too)
		LightStrike1,
		LightStrike2,
		LightStrike3,
		LightRecovery,
		HeavyStartup,
		HeavyActive,
		HeavyRecovery,
		RiposteRelease,
		Dodge,
		Hitstun,
		GuardHeal,
		HitPause,
		Other,
		NumBuckets
	};

	void Begin(bool bOn);
	bool IsOn() const { return bOn; }
	void Sample(const USkeletalMeshComponent* Skin, float DeltaSeconds);
	// What the pose placed this frame shows (the runner's last output), on its clock.
	// LegSpeed: the ground speed the legs were handed (m/s).
	void Drawn(const ftcore::ArenaPlayerOut& P, double Now, float LegSpeed);
	void Log() const;

private:
	struct FRow
	{
		int32 Frames = 0, Planted = 0, Still = 0;
		int32 Pops = 0; // fresh frames in which a foot moved kPopCm past twice the root's way, or kPopCapCm against it (a pose snap; still counted in the slip)
		int32 PopsAny = 0; // the old rule: fresh frames in which a foot moved over kPopCm
		double Body = 0.0, BodyAlong = 0.0, BodySide = 0.0; // cm/s; along and side of the facing
		double Dt = 0.0, Cycle = 0.0, Yaw = 0.0;
		double Slip = 0.0, SlipAlong = 0.0, SlipSide = 0.0; // planted foot, cm/s
		int32 Plants = 0;
		double Low = 0.0; // the lower ball's height over the floor, cm (summed over fresh frames)
		int32 Enter[3] = {0, 0, 0}; // the locomotion entering a start, a stop, a turn
		double PlantSlip = 0.0, PlantTime = 0.0; // each plant's net slip (start to end, cm) and time, by its first bucket
	};
	// The plant under way: its foot (INDEX_NONE none), where it started, its time and bucket.
	int32 PlantFoot = INDEX_NONE;
	FVector PlantStart = FVector::ZeroVector;
	float PlantTime = 0.f;
	EBucket PlantKey = Other;
	bool bOn = false;
	bool bHas = false;
	FVector LastBall[2] = {FVector::ZeroVector, FVector::ZeroVector};
	FVector LastLocal[2] = {FVector::ZeroVector, FVector::ZeroVector};
	FVector LastRoot = FVector::ZeroVector;
	float Floor = 1e9f; // unset (1e9) when the reference floor could not be read or does not hold: every row INVALID
	bool bFloorRead = false;
	uint8 LastLoco = 255; // the lead locomotion's state last fresh frame
	int32 PopLines = 0;
	float CycleSpeed = 0.f; // the ground speed the locomotion is handed, cm/s
	float LastYaw = 0.f;
	float PendingDt = 0.f; // stale frames' time since the last fresh pose
	EBucket Key = Other;
	ftcore::ArenaAction LastAction = ftcore::ArenaAction::Idle;
	double AfterHitUntil = -1.0;
	// The last Drawn's clock, leg speed (m/s) and facing (radians), for the move's split.
	double LastNow = -1.0;
	float LastLeg = 0.f;
	float LastFacing = 0.f;
	FRow Rows[NumBuckets];
};
