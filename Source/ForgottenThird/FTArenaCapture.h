// Review captures of a hosted fight (the -ArenaHere host in a world map), never in a shipping build. Two switches:
//   -ArenaShot=<path prefix> -ArenaShotAt=<fight seconds, comma list> [-ArenaShotSide]: one view at each time, the game's own
//     camera, or with -ArenaShotSide the side view below (as Demo_Arena's -ArenaShot, for the host).
//   -ArenaRec=<path prefix> [-ArenaRecView=close|far] [-ArenaRecFrom=<s>] [-ArenaRecTo=<s>]: every frame from From to To
//     (fight seconds; default 0 to the fight's end; -ArenaRecQuit ends the run after the last frame and view) as <prefix>_<frame>.png from a side-on camera that tracks enemy slot 0:
//     close sets it about 1 m off the enemy's flank at its body height, far frames the enemy and the lead together from
//     arena distance; q3 is close from the front three-quarter; track is side-on to the enemy's own facing, moving with it,
//     its feet and the ground in view (for foot sliding). Run it with -benchmark -fps=<n> for a fixed step, so the frames play back at native speed.
// Both write <prefix>_frames.csv: one row a frame (fight time, enemy 0's brain, part, move, clip, clip time, rate, ground
// speed, velocity, distance to the lead, the lead's action, and the tick's hit events), which drives the captions, then the
// muzzle: its tip's gap to the lead's body, how far ahead of the enemy's root it is along the line to the lead, and its height.
// The row is formatted into a fixed buffer: no allocation per frame.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

class UFTArenaRunner;
class ACameraActor;
class APlayerController;
class FArchive;
class USkeletalMeshComponent;

class FFTArenaCapture
{
public:
	~FFTArenaCapture();
	// Reads the switches; false when neither is on (then Tick does nothing).
	bool Begin();
	// After the runner's tick, every frame while the fight runs.
	void Tick(UWorld& World, APlayerController& PC, const UFTArenaRunner& Runner);
	// The fight is over: closes the log and gives the view back.
	void End(APlayerController* PC);
	bool IsOn() const { return bOn; }
	// How long the host should hold a finished fight so a recording whose -ArenaRecTo runs past the result (fight time
	// ResultT, then the seconds since the result) gets its settle frames: 0 when not recording or with no -ArenaRecTo.
	double RecTail(double ResultT) const
	{
		return bOn && !RecPrefix.IsEmpty() && RecTo < 1e8 ? FMath::Clamp(RecTo - ResultT, 0.0, 30.0) + 0.2 : 0.0;
	}

private:
	bool AimSide(UWorld& World, APlayerController& PC, const UFTArenaRunner& Runner, bool bClose);
	void WriteRow(const UFTArenaRunner& Runner, int32 Frame);
	void Shoot(UWorld& World, const FString& File) const;
	// Once per run: the tip of the snout, fixed to the upper jaw (jaw_upper, else head) at the front of the mesh's rest bounds.
	void FindMuzzle(const USkeletalMeshComponent& Skin);
	// The closest skinned vertex to At over every visible skeletal mesh on Lead (LOD 0 from the editor's imported model:
	// bone skinning only, no morphs or cloth), its heaviest bone and its mesh; false with no mesh to measure.
	bool ClosestSkinVertex(const AActor& Lead, const FVector& At, FVector& OutPoint, FName& OutBone, FName& OutPart);
	static void CopyName(const FName& Name, char* Out, int32 Size);
	// The front of each of the lead's legs at height Z (world cm, a 4 cm band): over every visible skinned vertex (as
	// ClosestSkinVertex) in the band, the one least far ahead of Origin along Fwd on each side of Fwd (left: +Left).
	// Out: forward then side (cm, from Origin, side + = left), for the left then the right; false when a side has none.
	// One pass over every visible skinned vertex on Lead: for each of Num points, the signed distance (cm) to the closest
	// vertex, negative when the point is behind that vertex's skinned normal (inside the surface). -1e9 with no mesh.
	void SignedSkinDistances(const AActor& Lead, const FVector* Points, int32 Num, double* OutCm);
	bool LegFronts(const AActor& Lead, const FVector& Origin, const FVector& Fwd, double Z, double Out[4]);
	// One pass over every visible skinned vertex on Lead (as ClosestSkinVertex): the least distance (cm) from any of Num
	// points to a vertex whose heaviest bone is a leg bone (name holds thigh, calf, foot or ball), left then right; -1 when
	// a side has none. The tooth tips to the leg surface, the points the reacting-leg pick uses (11-24 14:4x).
	void LegSurfaceDistances(const AActor& Lead, const FVector* Points, int32 Num, double OutCm[2]);

	bool bOn = false;
	FString ShotPrefix;
	TArray<double> ShotAt;
	int32 NextShot = 0;
	bool bShotSide = false;
	bool bShotPending = false; // the side view was aimed this frame; the view is taken the next

	FString RecPrefix;
	bool bRecClose = true;
	bool bRecQ3 = false; // -ArenaRecView=q3: the close view from the front three-quarter (ahead of the enemy, off the line)
	bool bRecTrack = false; // -ArenaRecView=track: side-on to the enemy's own facing, moving with it, its feet and ground in view
	double RecFrom = 0.0;
	double RecTo = 1e9;
	int32 RecFrames = 0;

	bool bMuzzleTried = false;
	int32 MuzzleBone = INDEX_NONE;
	FVector MuzzleLocal = FVector::ZeroVector; // the tip in MuzzleBone's space
	int32 JawBone = INDEX_NONE; // the enemy's "jaw" bone, for jaw_deg
	FQuat JawRest = FQuat::Identity; // its rest rotation in its parent's space
	// The enemy's four paws: per quadrant (front left, front right, hind left, hind right) the lowest bone at rest.
	int32 PawBone[4] = {INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE};
	// Each paw's pad or toe tip: the lowest rest vertex skinned (heaviest) to PawBone or a bone under it, held in that
	// vertex's heaviest bone's space, so the stance is read where the paw meets the ground, not at the bone.
	int32 PadBone[4] = {INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE};
	FVector PadLocal[4];
	TArray<FMatrix44f> RefToLocals; // reused by ClosestSkinVertex

	FArchive* Log = nullptr;
	int32 Rows = 0;
	TWeakObjectPtr<ACameraActor> Camera;
	FVector CameraAt = FVector::ZeroVector;
	float Side = 0.f; // +1 or -1: which flank of the enemy-lead line the camera keeps (picked once)
	bool bHudHidden = false;
	bool bQuitAfter = false; // -ArenaRecQuit: end the run once the recording and the views are done
	bool bQuitAsked = false;
};
