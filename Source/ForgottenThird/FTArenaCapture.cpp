#include "FTArenaCapture.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "ForgottenThird.h"
#include "FTArenaFighter.h"
#include "FTArenaRunner.h"

#include "ftcore/ArenaFight.h"

#include <cstdio>

namespace
{
// The close view: this far off the enemy's centre, across the enemy-lead line (cm), at this height over its feet (cm).
constexpr double kCloseOff = 135.0;
constexpr double kCloseHeight = 45.0;
// The far view: off the midpoint of the enemy and the lead, at least this far (cm), and this high (cm).
constexpr double kFarOff = 520.0;
constexpr double kFarHeight = 120.0;
// The far view stands back further as the two part: this share of their distance on top of kFarOff.
constexpr double kFarSpread = 0.8;
// How fast the camera follows its aim point (per second): smooth, but never far behind.
constexpr double kFollow = 6.0;
constexpr float kFov = 50.f;

const TCHAR* PartName(ftcore::ArenaPart Part)
{
	switch (Part)
	{
	case ftcore::ArenaPart::Startup:
		return TEXT("startup");
	case ftcore::ArenaPart::Active:
		return TEXT("active");
	case ftcore::ArenaPart::Recovery:
		return TEXT("recovery");
	default:
		return TEXT("-");
	}
}
} // namespace

FFTArenaCapture::~FFTArenaCapture()
{
	End(nullptr);
}

bool FFTArenaCapture::Begin()
{
#if UE_BUILD_SHIPPING
	return false;
#else
	const TCHAR* Cmd = FCommandLine::Get();
	FString Shot;
	if (FParse::Value(Cmd, TEXT("ArenaShot="), Shot) && !Shot.IsEmpty())
	{
		ShotPrefix = Shot;
		FString At;
		if (FParse::Value(Cmd, TEXT("ArenaShotAt="), At, false))
		{
			TArray<FString> Times;
			At.ParseIntoArray(Times, TEXT(","));
			for (const FString& Time : Times)
			{
				const double Seconds = FCString::Atod(*Time);
				if (Seconds >= 0.0)
				{
					ShotAt.Add(Seconds);
				}
			}
			ShotAt.Sort();
		}
		bShotSide = FParse::Param(Cmd, TEXT("ArenaShotSide"));
	}
	FString Rec;
	if (FParse::Value(Cmd, TEXT("ArenaRec="), Rec) && !Rec.IsEmpty())
	{
		RecPrefix = Rec;
		FString View;
		bRecClose = !(FParse::Value(Cmd, TEXT("ArenaRecView="), View) && View.Equals(TEXT("far"), ESearchCase::IgnoreCase));
		FParse::Value(Cmd, TEXT("ArenaRecFrom="), RecFrom);
		FParse::Value(Cmd, TEXT("ArenaRecTo="), RecTo);
	}
	bQuitAfter = FParse::Param(Cmd, TEXT("ArenaRecQuit"));
	bOn = !ShotPrefix.IsEmpty() || !RecPrefix.IsEmpty();
	if (!bOn)
	{
		return false;
	}
	const FString LogFile = (RecPrefix.IsEmpty() ? ShotPrefix : RecPrefix) + TEXT("_frames.csv");
	Log = IFileManager::Get().CreateFileWriter(*LogFile);
	if (Log)
	{
		const char Head[] = "frame,t,brain,part,move,clip,clip_at,rate,ground,vx,vy,dist,lead,hits,muzzle_gap,gap_to,muzzle_fwd,muzzle_up\n";
		Log->Serialize(const_cast<char*>(Head), sizeof(Head) - 1);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: host capture on: views %d (%s), recording %s (%s view, %.2f to %.2f s), log %s."), ShotAt.Num(),
		bShotSide ? TEXT("side") : TEXT("game camera"), RecPrefix.IsEmpty() ? TEXT("off") : *RecPrefix, bRecClose ? TEXT("close") : TEXT("far"),
		RecFrom, RecTo, *LogFile);
	return true;
#endif
}

void FFTArenaCapture::End(APlayerController* PC)
{
	if (Log)
	{
		Log->Close();
		delete Log;
		Log = nullptr;
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: host capture: %d log rows, %d recorded frames, %d of %d views."), Rows, RecFrames, NextShot,
			ShotAt.Num());
	}
	if (PC && Camera.IsValid() && PC->GetViewTarget() == Camera.Get())
	{
		PC->SetViewTarget(PC->GetPawn());
	}
	if (ACameraActor* Cam = Camera.Get())
	{
		Cam->Destroy();
	}
	Camera.Reset();
	bOn = false;
}

bool FFTArenaCapture::AimSide(UWorld& World, APlayerController& PC, const UFTArenaRunner& Runner, bool bClose)
{
	const AFTArenaFighter* Enemy = Runner.GetEnemyFigure(0);
	const AFTArenaFighter* Lead = Runner.GetPlayerFigure();
	if (!Enemy || !Lead)
	{
		return false;
	}
	const FVector E = Enemy->GetActorLocation();
	const FVector P = Lead->GetActorLocation();
	FVector Line = (P - E).GetSafeNormal2D();
	if (Line.IsNearlyZero())
	{
		Line = Enemy->GetActorForwardVector().GetSafeNormal2D();
	}
	FVector Across(-Line.Y, Line.X, 0.0);
	if (Side == 0.f)
	{
		Side = 1.f; // the first frame's left of the line, kept so the camera never jumps across
	}
	Across *= Side;
	const double Feet = E.Z - Enemy->GetSimpleCollisionHalfHeight();
	FVector Look;
	FVector Want;
	if (bClose)
	{
		Look = FVector(E.X, E.Y, Feet + kCloseHeight);
		Want = Look + Across * kCloseOff;
	}
	else
	{
		const FVector Mid = (E + P) * 0.5;
		Look = FVector(Mid.X, Mid.Y, Feet + kFarHeight * 0.6);
		Want = FVector(Mid.X, Mid.Y, Feet + kFarHeight) + Across * (kFarOff + kFarSpread * FVector::Dist2D(E, P));
	}
	ACameraActor* Cam = Camera.Get();
	if (!Cam)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Cam = World.SpawnActor<ACameraActor>(Want, (Look - Want).Rotation(), Params);
		if (!Cam)
		{
			return false;
		}
		Cam->GetCameraComponent()->SetFieldOfView(kFov);
		Camera = Cam;
		CameraAt = Want;
	}
	const double Dt = FMath::Max(0.0, static_cast<double>(World.GetDeltaSeconds()));
	CameraAt = FMath::Lerp(CameraAt, Want, FMath::Min(1.0, Dt * kFollow));
	Cam->SetActorLocationAndRotation(CameraAt, (Look - CameraAt).Rotation());
	if (PC.GetViewTarget() != Cam)
	{
		PC.SetViewTarget(Cam);
	}
	if (!bHudHidden)
	{
		if (AHUD* Hud = PC.GetHUD())
		{
			Hud->bShowHUD = false;
		}
		bHudHidden = true;
	}
	return true;
}

void FFTArenaCapture::Shoot(UWorld& World, const FString& File) const
{
	if (UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport.Get() : nullptr)
	{
		Viewport->Exec(&World, *FString::Printf(TEXT("HighResShot 1600x900 filename=\"%s\""), *File), *GLog);
	}
}

void FFTArenaCapture::FindMuzzle(const USkeletalMeshComponent& Skin)
{
	bMuzzleTried = true;
	const USkeletalMesh* Mesh = Skin.GetSkeletalMeshAsset();
	if (!Mesh)
	{
		return;
	}
	const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
	int32 Snout = Ref.FindBoneIndex(TEXT("jaw_upper"));
	if (Snout == INDEX_NONE)
	{
		Snout = Ref.FindBoneIndex(TEXT("head"));
	}
	const int32 Pelvis = Ref.FindBoneIndex(TEXT("pelvis"));
	if (Snout == INDEX_NONE || Pelvis == INDEX_NONE)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: host capture: no jaw_upper/head or pelvis bone, so no muzzle columns."));
		return;
	}
	const TArray<FTransform>& Pose = Ref.GetRefBonePose();
	auto AtRest = [&Ref, &Pose](int32 Bone)
	{
		FTransform T = FTransform::Identity;
		for (; Bone != INDEX_NONE; Bone = Ref.GetParentIndex(Bone))
		{
			T = T * Pose[Bone];
		}
		return T;
	};
	const FTransform SnoutAt = AtRest(Snout);
	// Forward is the mesh axis the snout lies along from the pelvis; the tip sits at the front face of the rest bounds.
	const FVector Ahead = SnoutAt.GetLocation() - AtRest(Pelvis).GetLocation();
	const FVector Forward = FMath::Abs(Ahead.X) >= FMath::Abs(Ahead.Y) ? FVector(Ahead.X >= 0.0 ? 1.0 : -1.0, 0.0, 0.0)
																	   : FVector(0.0, Ahead.Y >= 0.0 ? 1.0 : -1.0, 0.0);
	const FBoxSphereBounds Bounds = Mesh->GetImportedBounds();
	const double Front = FVector::DotProduct(Bounds.Origin, Forward) + FMath::Abs(FVector::DotProduct(Bounds.BoxExtent, Forward));
	const FVector Tip = SnoutAt.GetLocation() + Forward * (Front - FVector::DotProduct(SnoutAt.GetLocation(), Forward));
	MuzzleBone = Snout;
	MuzzleLocal = SnoutAt.InverseTransformPosition(Tip);
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: host capture: muzzle tip on %s, %.1f cm ahead of it at rest (mesh forward %s)."),
		*Ref.GetBoneName(Snout).ToString(), Front - FVector::DotProduct(SnoutAt.GetLocation(), Forward), *Forward.ToString());
}

void FFTArenaCapture::WriteRow(const UFTArenaRunner& Runner, int32 Frame)
{
	if (!Log)
	{
		return;
	}
#if !UE_BUILD_SHIPPING
	const ftcore::ArenaFight& Fight = Runner.GetFight();
	const ftcore::ArenaOutput& Out = Fight.output();
	const ftcore::ArenaEnemyOut& Enemy = Out.enemies[0];
	const UFTArenaRunner::FEnemyClipNow Clip = Runner.GetEnemyClipNow(0);
	const AFTArenaFighter* EnemyFigure = Runner.GetEnemyFigure(0);
	const AFTArenaFighter* Lead = Runner.GetPlayerFigure();
	const double Dist = EnemyFigure && Lead ? FVector::Dist2D(EnemyFigure->GetActorLocation(), Lead->GetActorLocation()) / 100.0 : -1.0;
	int32 Hits = 0;
	for (const ftcore::ArenaEvent& Event : Out.events)
	{
		Hits += Event.type == ftcore::ArenaEventType::Hit && Event.source == 0 && Event.target == ftcore::kArenaPlayer ? 1 : 0;
	}
	// The clip's object name, narrowed into the row's own buffer (ASCII asset names).
	char ClipName[96] = "-";
	if (Clip.Sequence)
	{
		const FString Name = Clip.Sequence->GetName(); // FName to string: a short-lived copy, dev capture only
		const int32 N = FMath::Min(Name.Len(), static_cast<int32>(sizeof(ClipName)) - 1);
		for (int32 C = 0; C < N; ++C)
		{
			ClipName[C] = static_cast<char>(Name[C] < 128 ? Name[C] : '?');
		}
		ClipName[N] = '\0';
	}
	char PartText[16];
	const TCHAR* Part = PartName(Enemy.part);
	int32 PN = 0;
	for (; Part[PN] && PN < 15; ++PN)
	{
		PartText[PN] = static_cast<char>(Part[PN]);
	}
	PartText[PN] = '\0';
	// The muzzle: the tip's gap to the lead's body (its physics shapes, else its nearest bone), how far ahead of the enemy's
	// root it is along the line to the lead, and its height over the root. -1 gap when there is no skin to measure.
	double MuzzleGap = -1.0;
	double MuzzleFwd = 0.0;
	double MuzzleUp = 0.0;
	char GapTo = '-';
	const USkeletalMeshComponent* PupSkin = EnemyFigure ? EnemyFigure->GetSkin() : nullptr;
	if (PupSkin && !bMuzzleTried)
	{
		FindMuzzle(*PupSkin);
	}
	if (PupSkin && Lead && MuzzleBone != INDEX_NONE && MuzzleBone < PupSkin->GetNumBones())
	{
		const FVector Tip = PupSkin->GetBoneTransform(MuzzleBone).TransformPosition(MuzzleLocal);
		const FVector Root = EnemyFigure->GetActorLocation();
		const FVector Line = (Lead->GetActorLocation() - Root).GetSafeNormal2D();
		MuzzleFwd = FVector::DotProduct(Tip - Root, Line) / 100.0;
		MuzzleUp = (Tip.Z - Root.Z) / 100.0;
		if (const USkeletalMeshComponent* LeadSkin = Lead->GetLeadSkin())
		{
			FClosestPointOnPhysicsAsset Closest;
			if (LeadSkin->GetClosestPointOnPhysicsAsset(Tip, Closest, false))
			{
				MuzzleGap = Closest.Distance / 100.0;
				GapTo = 'p';
			}
			else
			{
				double Best = 1e12;
				for (int32 B = 0; B < LeadSkin->GetNumBones(); ++B)
				{
					Best = FMath::Min(Best, FVector::DistSquared(Tip, LeadSkin->GetBoneTransform(B).GetLocation()));
				}
				MuzzleGap = Best < 1e12 ? FMath::Sqrt(Best) / 100.0 : -1.0;
				GapTo = 'b';
			}
		}
	}
	char Row[384];
	const int Len = std::snprintf(Row, sizeof(Row), "%d,%.4f,%s,%s,%u,%s,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%s,%d,%.3f,%c,%.3f,%.3f\n", Frame,
		static_cast<double>(Fight.time()), ftcore::arenaBrainName(Enemy.brain), PartText, static_cast<unsigned>(Enemy.move), ClipName,
		static_cast<double>(Clip.At), static_cast<double>(Clip.Rate), static_cast<double>(Clip.Ground), static_cast<double>(Enemy.velocity.x),
		static_cast<double>(Enemy.velocity.y), Dist, ftcore::arenaActionName(Out.player.action), Hits, MuzzleGap, GapTo, MuzzleFwd, MuzzleUp);
	if (Len > 0)
	{
		Log->Serialize(Row, FMath::Min(Len, static_cast<int>(sizeof(Row)) - 1));
		++Rows;
	}
#else
	(void)Runner;
	(void)Frame;
#endif
}

void FFTArenaCapture::Tick(UWorld& World, APlayerController& PC, const UFTArenaRunner& Runner)
{
	if (!bOn)
	{
		return;
	}
	const double T = static_cast<double>(Runner.GetFight().time());
	const bool bRecDone = RecPrefix.IsEmpty() || T > RecTo;
	const bool bShotsDone = NextShot >= ShotAt.Num();
	if (bQuitAfter && bRecDone && bShotsDone && T > 0.5)
	{
		// -ArenaRecQuit: every frame and view is in; the run ends here (a frame later, so the last view is written).
		if (bQuitAsked)
		{
			End(&PC);
			FPlatformMisc::RequestExit(false, TEXT("ArenaRecQuit"));
			return;
		}
		bQuitAsked = true;
	}
	const bool bRec = !RecPrefix.IsEmpty() && T >= RecFrom && T <= RecTo;
	if (bRec)
	{
		AimSide(World, PC, Runner, bRecClose);
		Shoot(World, FString::Printf(TEXT("%s_%05d.png"), *RecPrefix, RecFrames));
		WriteRow(Runner, RecFrames);
		++RecFrames;
		return;
	}
	WriteRow(Runner, -1);
	if (NextShot >= ShotAt.Num() || T < ShotAt[NextShot])
	{
		return;
	}
	if (bShotSide && !bShotPending)
	{
		// Aimed this frame, taken the next, so the view is the new camera's.
		bShotPending = AimSide(World, PC, Runner, true);
		if (bShotPending)
		{
			return;
		}
	}
	bShotPending = false;
	Shoot(World, FString::Printf(TEXT("%s_t_%05.2f.png"), *ShotPrefix, ShotAt[NextShot]));
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: host view t_%05.2f at %.3f s."), ShotAt[NextShot], T);
	++NextShot;
}
