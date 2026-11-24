#include "FTArenaCapture.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#if WITH_EDITORONLY_DATA
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#endif
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
// The close view: this far off a point up to kCloseLead ahead of the enemy, across the enemy-lead line (cm), at this height over its feet (cm).
constexpr double kCloseOff = 135.0;
constexpr double kCloseHeight = 45.0;
constexpr double kCloseLead = 50.0;
// The q3 view: this far from the same point, 45 degrees between across the line and ahead along it (cm), this much higher (cm).
constexpr double kQ3Off = 170.0;
// The track view: this far off the enemy's flank, looking at this height over its feet, from this much higher (cm).
constexpr double kTrackOff = 300.0;
constexpr double kTrackLook = 35.0;
constexpr double kTrackLift = 25.0;
constexpr double kQ3Lift = 25.0;
// The canines sit this far under the muzzle tip (cm; Blender's g1124b measure, 11-24: about 95 mm).
constexpr double kCanineDrop = 9.5;
// The pup's canine tips off G (the muzzle tip: jaw_upper + its rest front face, on the centreline), in the pup's frame:
// forward, left, up (mm). Blender's CANINE_OFFSET_1124 on g1124b, valid for Nip f39-45 only (the shut hold's first frames).
// Order: upper inner L, R; upper outer L, R; lower L, R.
constexpr int32 kProbes = 6;
constexpr double kCanine[kProbes][3] = {{-35.8, 61.2, -96.8}, {-43.1, -61.9, -95.3}, {-64.3, 95.3, -97.1},
	{-66.8, -94.0, -89.9}, {-37.8, 64.0, -100.3}, {-38.1, -62.6, -100.6}};
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
		const bool bView = FParse::Value(Cmd, TEXT("ArenaRecView="), View);
		bRecClose = !(bView && View.Equals(TEXT("far"), ESearchCase::IgnoreCase));
		bRecQ3 = bView && View.Equals(TEXT("q3"), ESearchCase::IgnoreCase);
		bRecTrack = bView && View.Equals(TEXT("track"), ESearchCase::IgnoreCase);
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
		const char Head[] = "frame,t,brain,part,move,clip,clip_at,rate,ground,vx,vy,dist,lead,lead_hp,hits,muzzle_gap,gap_to,muzzle_fwd,muzzle_up,lead_x,lead_y,lead_z,pelvis_x,pelvis_y,pelvis_z,head_z,gap_fwd,gap_side,gap_up,gap_bone,tip_z,mesh_gap,mgap_fwd,mgap_side,mgap_up,mesh_bone,mesh_part,lead_yaw,pup_yaw,tip_side_pup,jaw_deg,canine_z,legl_fwd,legl_side,legr_fwd,legr_side,tooth_ui_l,tooth_ui_r,tooth_uo_l,tooth_uo_r,tooth_lo_l,tooth_lo_r,knee_l,knee_r,midthigh_l,midthigh_r,pup_actor_dz,pup_root_dz,pup_mesh_dz,pup_x,pup_y,pup_wyaw,paw_fl_x,paw_fl_y,paw_fl_z,paw_fr_x,paw_fr_y,paw_fr_z,paw_hl_x,paw_hl_y,paw_hl_z,paw_hr_x,paw_hr_y,paw_hr_z,mix,mix_w,clip2,clip2_at,rate2,w2,slot,mix_speed\n";
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
	FVector Across(-Line.Y, Line.X, 0.0); // with Side +1: the enemy's RIGHT (UE is left-handed, Y is right)
	if (Side == 0.f)
	{
		Side = 1.f; // the first frame's left of the line, kept so the camera never jumps across
	}
	Across *= Side;
	const double Feet = E.Z - Enemy->GetSimpleCollisionHalfHeight();
	FVector Look;
	FVector Want;
	if (bRecTrack && bClose)
	{
		// Side-on to the enemy's own facing (its right flank with Side +1), level with its belly, ground in frame.
		const FVector Fwd = Enemy->GetActorForwardVector().GetSafeNormal2D();
		const FVector Flank = FVector(-Fwd.Y, Fwd.X, 0.0) * Side;
		Look = FVector(E.X, E.Y, Feet + kTrackLook);
		Want = Look + Flank * kTrackOff + FVector(0.0, 0.0, kTrackLift);
	}
	else if (bClose)
	{
		// Half the gap ahead of the enemy toward the lead, at most kCloseLead, so the muzzle and the lead's body share the frame at a bite.
		const FVector Ahead = E + Line * FMath::Min(0.5 * FVector::Dist2D(E, P), kCloseLead);
		Look = FVector(Ahead.X, Ahead.Y, Feet + kCloseHeight);
		Want = bRecQ3 ? Look + (Across + Line).GetSafeNormal() * kQ3Off + FVector(0.0, 0.0, kQ3Lift) : Look + Across * kCloseOff;
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
	JawBone = Ref.FindBoneIndex(TEXT("jaw"));
	if (JawBone != INDEX_NONE && Ref.GetParentIndex(JawBone) != INDEX_NONE)
	{
		JawRest = Ref.GetRefBonePose()[JawBone].GetRotation();
	}
	else
	{
		JawBone = INDEX_NONE;
	}
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
	{
		// Paws: in each quadrant off the pelvis (front/hind along Forward, left/right along Left), the lowest bone at rest.
		const FVector Left(-Forward.Y, Forward.X, 0.0); // the mesh's left (bind: forward -Y, left +X)
		const FVector Hip = AtRest(Pelvis).GetLocation();
		double Low[4] = {1e9, 1e9, 1e9, 1e9};
		for (int32 B = 0; B < Ref.GetNum(); ++B)
		{
			const FVector At = AtRest(B).GetLocation();
			const int32 Q = (FVector::DotProduct(At - Hip, Forward) >= 0.0 ? 0 : 2) + (FVector::DotProduct(At - Hip, Left) >= 0.0 ? 0 : 1);
			if (FMath::Abs(FVector::DotProduct(At - Hip, Left)) > 2.0 && At.Z < Low[Q])
			{
				Low[Q] = At.Z;
				PawBone[Q] = B;
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: host capture: paws FL %s, FR %s, HL %s, HR %s."),
			PawBone[0] != INDEX_NONE ? *Ref.GetBoneName(PawBone[0]).ToString() : TEXT("-"), PawBone[1] != INDEX_NONE ? *Ref.GetBoneName(PawBone[1]).ToString() : TEXT("-"),
			PawBone[2] != INDEX_NONE ? *Ref.GetBoneName(PawBone[2]).ToString() : TEXT("-"), PawBone[3] != INDEX_NONE ? *Ref.GetBoneName(PawBone[3]).ToString() : TEXT("-"));
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: host capture: muzzle tip on %s, %.1f cm ahead of it at rest (mesh forward %s)."),
		*Ref.GetBoneName(Snout).ToString(), Front - FVector::DotProduct(SnoutAt.GetLocation(), Forward), *Forward.ToString());
}

void FFTArenaCapture::SignedSkinDistances(const AActor& Lead, const FVector* Points, int32 Num, double* OutCm)
{
	double Best2[kProbes];
	Num = FMath::Min(Num, kProbes);
	for (int32 K = 0; K < Num; ++K)
	{
		Best2[K] = TNumericLimits<double>::Max();
		OutCm[K] = -1e9;
	}
#if WITH_EDITORONLY_DATA
	TInlineComponentArray<USkeletalMeshComponent*> Skins(&Lead);
	for (USkeletalMeshComponent* Skin : Skins)
	{
		USkeletalMesh* Mesh = Skin ? Skin->GetSkeletalMeshAsset() : nullptr;
		const FSkeletalMeshModel* Model = Mesh ? Mesh->GetImportedModel() : nullptr;
		if (!Model || Model->LODModels.Num() == 0 || !Skin->IsVisible())
		{
			continue;
		}
		Skin->GetCurrentRefToLocalMatrices(RefToLocals, 0);
		const FTransform ToWorld = Skin->GetComponentTransform();
		for (const FSkelMeshSection& Section : Model->LODModels[0].Sections)
		{
			if (Section.bDisabled)
			{
				continue;
			}
			for (const FSoftSkinVertex& V : Section.SoftVertices)
			{
				FVector3f P = FVector3f::ZeroVector;
				FVector3f N = FVector3f::ZeroVector;
				float Sum = 0.f;
				for (int32 I = 0; I < MAX_TOTAL_INFLUENCES; ++I)
				{
					const uint16 W = V.InfluenceWeights[I];
					if (W == 0 || V.InfluenceBones[I] >= Section.BoneMap.Num())
					{
						continue;
					}
					const int32 B = Section.BoneMap[V.InfluenceBones[I]];
					if (RefToLocals.IsValidIndex(B))
					{
						P += RefToLocals[B].TransformPosition(V.Position) * static_cast<float>(W);
						N += RefToLocals[B].TransformVector(FVector3f(V.TangentZ)) * static_cast<float>(W);
						Sum += static_cast<float>(W);
					}
				}
				if (Sum <= 0.f)
				{
					continue;
				}
				const FVector At = ToWorld.TransformPosition(FVector(P / Sum));
				for (int32 K = 0; K < Num; ++K)
				{
					const double D2 = FVector::DistSquared(At, Points[K]);
					if (D2 < Best2[K])
					{
						Best2[K] = D2;
						const FVector Normal = ToWorld.TransformVectorNoScale(FVector(N)).GetSafeNormal();
						OutCm[K] = FMath::Sqrt(D2) * (FVector::DotProduct(Points[K] - At, Normal) < 0.0 ? -1.0 : 1.0);
					}
				}
			}
		}
	}
#endif
}

bool FFTArenaCapture::LegFronts(const AActor& Lead, const FVector& Origin, const FVector& Fwd, double Z, double Out[4])
{
	Out[0] = Out[2] = TNumericLimits<double>::Max();
	Out[1] = Out[3] = 0.0;
#if WITH_EDITORONLY_DATA
	const FVector Left(Fwd.Y, -Fwd.X, 0.0); // UE is left-handed (X forward, Y right): this is Fwd's left
	TInlineComponentArray<USkeletalMeshComponent*> Skins(&Lead);
	for (USkeletalMeshComponent* Skin : Skins)
	{
		USkeletalMesh* Mesh = Skin ? Skin->GetSkeletalMeshAsset() : nullptr;
		const FSkeletalMeshModel* Model = Mesh ? Mesh->GetImportedModel() : nullptr;
		if (!Model || Model->LODModels.Num() == 0 || !Skin->IsVisible())
		{
			continue;
		}
		Skin->GetCurrentRefToLocalMatrices(RefToLocals, 0);
		const FTransform ToWorld = Skin->GetComponentTransform();
		for (const FSkelMeshSection& Section : Model->LODModels[0].Sections)
		{
			if (Section.bDisabled)
			{
				continue;
			}
			for (const FSoftSkinVertex& V : Section.SoftVertices)
			{
				FVector3f P = FVector3f::ZeroVector;
				float Sum = 0.f;
				for (int32 I = 0; I < MAX_TOTAL_INFLUENCES; ++I)
				{
					const uint16 W = V.InfluenceWeights[I];
					if (W == 0 || V.InfluenceBones[I] >= Section.BoneMap.Num())
					{
						continue;
					}
					const int32 B = Section.BoneMap[V.InfluenceBones[I]];
					if (RefToLocals.IsValidIndex(B))
					{
						P += RefToLocals[B].TransformPosition(V.Position) * static_cast<float>(W);
						Sum += static_cast<float>(W);
					}
				}
				if (Sum <= 0.f)
				{
					continue;
				}
				const FVector At = ToWorld.TransformPosition(FVector(P / Sum));
				if (FMath::Abs(At.Z - Z) > 2.0)
				{
					continue;
				}
				const FVector D = At - Origin;
				const double Ahead = FVector::DotProduct(D, Fwd);
				const double Across = FVector::DotProduct(D, Left);
				const int32 K = Across >= 0.0 ? 0 : 2;
				if (Ahead < Out[K])
				{
					Out[K] = Ahead;
					Out[K + 1] = Across;
				}
			}
		}
	}
#endif
	return Out[0] < TNumericLimits<double>::Max() && Out[2] < TNumericLimits<double>::Max();
}

void FFTArenaCapture::CopyName(const FName& Name, char* Out, int32 Size)
{
	const FString Text = Name.ToString();
	int32 N = 0;
	for (; N < Text.Len() && N < Size - 1; ++N)
	{
		const TCHAR C = Text[N];
		Out[N] = (C == ',' || C > 126) ? '_' : static_cast<char>(C);
	}
	Out[N] = 0;
}

bool FFTArenaCapture::ClosestSkinVertex(const AActor& Lead, const FVector& At, FVector& OutPoint, FName& OutBone, FName& OutPart)
{
#if WITH_EDITORONLY_DATA
	TInlineComponentArray<USkeletalMeshComponent*> Skins(&Lead);
	double Best = TNumericLimits<double>::Max();
	for (USkeletalMeshComponent* Skin : Skins)
	{
		USkeletalMesh* Mesh = Skin ? Skin->GetSkeletalMeshAsset() : nullptr;
		const FSkeletalMeshModel* Model = Mesh ? Mesh->GetImportedModel() : nullptr;
		if (!Model || Model->LODModels.Num() == 0 || !Skin->IsVisible())
		{
			continue;
		}
		Skin->GetCurrentRefToLocalMatrices(RefToLocals, 0);
		const FTransform ToWorld = Skin->GetComponentTransform();
		const FVector Local = ToWorld.InverseTransformPosition(At);
		const FVector3f Local3f(Local);
		for (const FSkelMeshSection& Section : Model->LODModels[0].Sections)
		{
			if (Section.bDisabled)
			{
				continue;
			}
			for (const FSoftSkinVertex& V : Section.SoftVertices)
			{
				FVector3f P = FVector3f::ZeroVector;
				float Sum = 0.f;
				int32 Heavy = INDEX_NONE;
				uint16 HeavyW = 0;
				for (int32 I = 0; I < MAX_TOTAL_INFLUENCES; ++I)
				{
					const uint16 W = V.InfluenceWeights[I];
					if (W == 0 || V.InfluenceBones[I] >= Section.BoneMap.Num())
					{
						continue;
					}
					const int32 B = Section.BoneMap[V.InfluenceBones[I]];
					if (!RefToLocals.IsValidIndex(B))
					{
						continue;
					}
					P += RefToLocals[B].TransformPosition(V.Position) * static_cast<float>(W);
					Sum += static_cast<float>(W);
					if (W > HeavyW)
					{
						HeavyW = W;
						Heavy = B;
					}
				}
				if (Sum <= 0.f)
				{
					continue;
				}
				P /= Sum;
				const double D2 = FVector3f::DistSquared(P, Local3f);
				if (D2 < Best)
				{
					Best = D2;
					OutPoint = ToWorld.TransformPosition(FVector(P));
					OutBone = Heavy != INDEX_NONE ? Mesh->GetRefSkeleton().GetBoneName(Heavy) : NAME_None;
					OutPart = Mesh->GetFName();
				}
			}
		}
	}
	return Best < TNumericLimits<double>::Max();
#else
	return false;
#endif
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
	// Hits: the last fight step of the frame only (two 60 Hz steps a 30 fps frame); lead_hp shows every blow.
	int32 Hits = 0;
	for (const ftcore::ArenaEvent& Event : Out.events)
	{
		Hits += Event.type == ftcore::ArenaEventType::Hit && Event.source == 0 && Event.target == ftcore::kArenaPlayer ? 1 : 0;
	}
	// The clip's object name, narrowed into the row's own buffer (ASCII asset names).
	// A gait mix's second layer (FEnemyClipNow): clip2 at clip2_at, rate2 (signed), w2 of the slot; mix_w the first's
	// share, slot the slot's weight over the clip before it, mix_speed the signed m/s the feet are given.
	char Clip2Name[96] = "-";
	if (Clip.Second)
	{
		const FString Name = Clip.Second->GetName();
		const int32 N = FMath::Min(Name.Len(), static_cast<int32>(sizeof(Clip2Name)) - 1);
		for (int32 C = 0; C < N; ++C)
		{
			Clip2Name[C] = static_cast<char>(Name[C] < 128 ? Name[C] : '?');
		}
		Clip2Name[N] = '\0';
	}
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
	// The gap split along the attack line (m): from the tip to the closest point, forward along the pup-to-lead line,
	// sideways (positive to the line's left) and up; and the lead bone that point is on. A shortfall in forward is
	// distance, in sideways or up is aim. tip_z: the tip's height over the lead's ground (actor) point.
	double GapFwd = 0.0, GapSide = 0.0, GapUp = 0.0, TipZ = 0.0;
	char GapBone[24] = "-";
	// The gap to the lead's visible skin (recorded frames only): the closest skinned vertex over every visible skeletal
	// mesh on the lead (body and garments, LOD 0, no morphs or cloth), split the same way; its heaviest bone and its mesh.
	double MeshGapM = -1.0, MGapFwd = 0.0, MGapSide = 0.0, MGapUp = 0.0;
	char MeshBone[24] = "-";
	char MeshPart[40] = "-";
	// Facing (deg, + = turned to its left): the lead's forward against the direction to the pup (0 = facing the pup),
	// the pup's forward against the pup-to-lead line; and the tip's offset off the pup's forward axis (m, + = pup's left).
	double LeadYaw = 0.0, PupYaw = 0.0, TipSidePup = 0.0;
	// jaw_deg: the enemy's jaw bone turned off its rest pose in its parent's space (deg, unsigned; -1 with no jaw bone).
	double JawDeg = -1.0;
	// Leg fronts at canine height (recorded frames only): canine_z is the height (m over the lead's ground) of a band
	// kCanineDrop under the tip; legl_fwd/legl_side and legr_fwd/legr_side are the lead's leg front in that band on the pup's
	// left and right, from the pup's root along its own forward and its left (m). -1 / 0 when not measured.
	double CanineZ = -1.0;
	double LegFront[4] = {-1.0, 0.0, -1.0, 0.0};
	// Canine tips (recorded frames only): signed distance (m, negative = inside) from each kCanine tip to the lead's skinned
	// mesh, in kCanine's order; -9 when not measured. The lead's knee (calf bone) and mid-thigh (thigh-calf midpoint) heights
	// over its ground (m), L then R. The pup's actor over the lead's ground, its root bone and its mesh over its actor (m).
	double ToothCm[kProbes] = {-900.0, -900.0, -900.0, -900.0, -900.0, -900.0};
	double KneeZ[2] = {-1.0, -1.0};
	double MidThighZ[2] = {-1.0, -1.0};
	double PupActorDz = 0.0, PupRootDz = 0.0, PupMeshDz = 0.0;
	// Every frame: the enemy actor's world place (m) and world yaw (deg), and its four paws' world places (m; 0 without bones).
	double PupX = 0.0, PupY = 0.0, PupWYaw = 0.0;
	double Paw[12] = {};
	const USkeletalMeshComponent* PupSkin = EnemyFigure ? EnemyFigure->GetSkin() : nullptr;
	if (PupSkin && !bMuzzleTried)
	{
		FindMuzzle(*PupSkin);
	}
	if (EnemyFigure)
	{
		PupX = EnemyFigure->GetActorLocation().X / 100.0;
		PupY = EnemyFigure->GetActorLocation().Y / 100.0;
		PupWYaw = EnemyFigure->GetActorRotation().Yaw;
	}
	for (int32 K = 0; PupSkin && K < 4; ++K)
	{
		if (PawBone[K] != INDEX_NONE && PawBone[K] < PupSkin->GetNumBones())
		{
			const FVector At = PupSkin->GetBoneTransform(PawBone[K]).GetLocation() / 100.0;
			Paw[K * 3] = At.X;
			Paw[K * 3 + 1] = At.Y;
			Paw[K * 3 + 2] = At.Z;
		}
	}
	if (PupSkin && Lead && MuzzleBone != INDEX_NONE && MuzzleBone < PupSkin->GetNumBones())
	{
		const FVector Tip = PupSkin->GetBoneTransform(MuzzleBone).TransformPosition(MuzzleLocal);
		const FVector Root = EnemyFigure->GetActorLocation();
		const FVector Line = (Lead->GetActorLocation() - Root).GetSafeNormal2D();
		MuzzleFwd = FVector::DotProduct(Tip - Root, Line) / 100.0;
		MuzzleUp = (Tip.Z - Root.Z) / 100.0;
		TipZ = (Tip.Z - Lead->GetActorLocation().Z) / 100.0;
		if (JawBone != INDEX_NONE && JawBone < PupSkin->GetNumBones())
		{
			const int32 JawParent = PupSkin->GetSkeletalMeshAsset()->GetRefSkeleton().GetParentIndex(JawBone);
			const FQuat JawNow = PupSkin->GetBoneTransform(JawBone).GetRelativeTransform(PupSkin->GetBoneTransform(JawParent)).GetRotation();
			JawDeg = FMath::RadiansToDegrees(JawRest.AngularDistance(JawNow));
		}
		{
			const FVector ToPup = -Line;
			const FVector LeadFwd = Lead->GetActorForwardVector().GetSafeNormal2D();
			const FVector PupFwd = EnemyFigure->GetActorForwardVector().GetSafeNormal2D();
			LeadYaw = FMath::RadiansToDegrees(FMath::Atan2(-FVector::CrossProduct(ToPup, LeadFwd).Z, FVector::DotProduct(ToPup, LeadFwd)));
			PupYaw = FMath::RadiansToDegrees(FMath::Atan2(-FVector::CrossProduct(Line, PupFwd).Z, FVector::DotProduct(Line, PupFwd)));
			TipSidePup = FVector::DotProduct(Tip - Root, FVector(PupFwd.Y, -PupFwd.X, 0.0)) / 100.0;
		}
		if (Frame >= 0)
		{
			{
				const FVector PupF = EnemyFigure->GetActorForwardVector().GetSafeNormal2D();
				const FVector PupL(PupF.Y, -PupF.X, 0.0); // the pup's left (UE: Y is right)
				FVector Teeth[kProbes];
				for (int32 K = 0; K < kProbes; ++K)
				{
					Teeth[K] = Tip + (PupF * kCanine[K][0] + PupL * kCanine[K][1] + FVector::UpVector * kCanine[K][2]) / 10.0;
				}
				SignedSkinDistances(*Lead, Teeth, kProbes, ToothCm);
				const double Ground = Lead->GetActorLocation().Z;
				if (const USkeletalMeshComponent* Legs = Lead->GetLeadSkin())
				{
					static const FName LegBones[4] = {TEXT("thigh_l"), TEXT("calf_l"), TEXT("thigh_r"), TEXT("calf_r")};
					for (int32 S = 0; S < 2; ++S)
					{
						if (Legs->GetBoneIndex(LegBones[S * 2]) != INDEX_NONE && Legs->GetBoneIndex(LegBones[S * 2 + 1]) != INDEX_NONE)
						{
							const double Hip = Legs->GetBoneLocation(LegBones[S * 2]).Z;
							const double Knee = Legs->GetBoneLocation(LegBones[S * 2 + 1]).Z;
							KneeZ[S] = (Knee - Ground) / 100.0;
							MidThighZ[S] = ((Hip + Knee) * 0.5 - Ground) / 100.0;
						}
					}
				}
				PupActorDz = (Root.Z - Ground) / 100.0;
				PupRootDz = (PupSkin->GetBoneLocation(PupSkin->GetBoneName(0)).Z - Root.Z) / 100.0;
				PupMeshDz = (PupSkin->GetComponentLocation().Z - Root.Z) / 100.0;
			}
			const double BandZ = Tip.Z - kCanineDrop;
			const FVector PupAxis = EnemyFigure->GetActorForwardVector().GetSafeNormal2D();
			double Fronts[4];
			if (LegFronts(*Lead, Root, PupAxis, BandZ, Fronts))
			{
				CanineZ = (BandZ - Lead->GetActorLocation().Z) / 100.0;
				for (int32 K = 0; K < 4; ++K)
				{
					LegFront[K] = Fronts[K] / 100.0;
				}
			}
		}
		if (Frame >= 0)
		{
			FVector Point;
			FName SkinBone, SkinMesh;
			if (ClosestSkinVertex(*Lead, Tip, Point, SkinBone, SkinMesh))
			{
				const FVector D = Point - Tip;
				const FVector Left(Line.Y, -Line.X, 0.0); // the line's left (UE: Y is right)
				MeshGapM = D.Size() / 100.0;
				MGapFwd = FVector::DotProduct(D, Line) / 100.0;
				MGapSide = FVector::DotProduct(D, Left) / 100.0;
				MGapUp = D.Z / 100.0;
				CopyName(SkinBone, MeshBone, sizeof(MeshBone));
				CopyName(SkinMesh, MeshPart, sizeof(MeshPart));
			}
		}
		if (const USkeletalMeshComponent* LeadSkin = Lead->GetLeadSkin())
		{
			FClosestPointOnPhysicsAsset Closest;
			if (LeadSkin->GetClosestPointOnPhysicsAsset(Tip, Closest, false))
			{
				MuzzleGap = Closest.Distance / 100.0;
				GapTo = 'p';
				const FVector D = Closest.ClosestWorldPosition - Tip;
				const FVector Left(Line.Y, -Line.X, 0.0); // the line's left (UE: Y is right)
				GapFwd = FVector::DotProduct(D, Line) / 100.0;
				GapSide = FVector::DotProduct(D, Left) / 100.0;
				GapUp = D.Z / 100.0;
				CopyName(Closest.BoneName, GapBone, sizeof(GapBone));
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
	// The lead's place and its body: the actor (m, world) and the skin's pelvis and head bones, so a hit reaction's
	// snap, launch and settle read as numbers. Zeros when there is no lead skin.
	static const FName PelvisName(TEXT("pelvis"));
	static const FName HeadName(TEXT("head"));
	FVector LeadAt = FVector::ZeroVector, PelvisAt = FVector::ZeroVector, HeadAt = FVector::ZeroVector;
	if (Lead)
	{
		LeadAt = Lead->GetActorLocation() / 100.0;
		if (const USkeletalMeshComponent* LeadSkin = Lead->GetLeadSkin())
		{
			PelvisAt = LeadSkin->GetBoneIndex(PelvisName) != INDEX_NONE ? LeadSkin->GetBoneLocation(PelvisName) / 100.0 : PelvisAt;
			HeadAt = LeadSkin->GetBoneIndex(HeadName) != INDEX_NONE ? LeadSkin->GetBoneLocation(HeadName) / 100.0 : HeadAt;
		}
	}
	char Row[1900];
	const int Len = std::snprintf(Row, sizeof(Row), "%d,%.4f,%s,%s,%u,%s,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%s,%d,%d,%.3f,%c,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%s,%.3f,%.3f,%.3f,%.3f,%.3f,%s,%s,%.2f,%.2f,%.3f,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.4f,%.4f,%.2f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%.3f,%s,%.4f,%.3f,%.3f,%.3f,%.3f\n", Frame,
		static_cast<double>(Fight.time()) + Runner.GetResultAge(), ftcore::arenaBrainName(Enemy.brain), PartText, static_cast<unsigned>(Enemy.move), ClipName,
		static_cast<double>(Clip.At), static_cast<double>(Clip.Rate), static_cast<double>(Clip.Ground), static_cast<double>(Enemy.velocity.x),
		static_cast<double>(Enemy.velocity.y), Dist, ftcore::arenaActionName(Out.player.action), Fight.hp(), Hits, MuzzleGap, GapTo, MuzzleFwd, MuzzleUp,
		LeadAt.X, LeadAt.Y, LeadAt.Z, PelvisAt.X, PelvisAt.Y, PelvisAt.Z, HeadAt.Z, GapFwd, GapSide, GapUp, GapBone, TipZ,
		MeshGapM, MGapFwd, MGapSide, MGapUp, MeshBone, MeshPart, LeadYaw, PupYaw, TipSidePup, JawDeg,
		CanineZ, LegFront[0], LegFront[1], LegFront[2], LegFront[3],
		ToothCm[0] / 100.0, ToothCm[1] / 100.0, ToothCm[2] / 100.0, ToothCm[3] / 100.0, ToothCm[4] / 100.0, ToothCm[5] / 100.0,
		KneeZ[0], KneeZ[1], MidThighZ[0], MidThighZ[1], PupActorDz, PupRootDz, PupMeshDz, PupX, PupY, PupWYaw,
		Paw[0], Paw[1], Paw[2], Paw[3], Paw[4], Paw[5], Paw[6], Paw[7], Paw[8], Paw[9], Paw[10], Paw[11],
		Clip.bMix ? 1 : 0, static_cast<double>(Clip.Weight), Clip2Name, static_cast<double>(Clip.SecondAt), static_cast<double>(Clip.SecondRate),
		static_cast<double>(Clip.SecondWeight), static_cast<double>(Clip.Slot), static_cast<double>(Clip.Speed));
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
	// Fight time, then the seconds since the result: the fight clock stops at the result, a recording past it does not.
	const double T = static_cast<double>(Runner.GetFight().time()) + Runner.GetResultAge();
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
