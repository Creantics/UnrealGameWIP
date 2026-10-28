#include "FTCutsceneDirector.h"

#include "Algo/Sort.h"
#include "Camera/CameraComponent.h"
#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "ForgottenThird.h"
#include "FTText.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/WorldSettings.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include "FTExploreCharacter.h"
#include "FTExploreDirector.h"
#include "FTGameInstance.h"
#include "FTPartyFollower.h"
#include "FTPauseSubsystem.h"
#include "FTStepHost.h"

#include "ftcore/StubScreen.h"

#include <string>

namespace
{
constexpr float kCutsceneCmPerMetre = 100.f;
// In place: the height above the lead's feet where the gameplay camera's look point is taken (cm), how
// far from the lead a camera must stand to have a bearing of its own (cm), the turn past which the orbit
// goes round the lead's right rather than the short way (degrees), the camera sweep's radius (cm), and
// how long a traversal is waited out before the stage goes down anyway (real seconds).
constexpr float kChestHeightCm = 145.f;
constexpr float kOrbitMinRadius = 10.f;
constexpr float kOrbitTieDegrees = 160.f;
constexpr float kSweepRadius = 12.f;
constexpr double kTraversalWaitLimit = 5.0;
// In place: how far above or below the lead's feet the ground at a subject mark may lie for a follower
// to walk onto it, and the height above the ground its way is checked at (cm).
constexpr float kWalkGroundCm = 100.f;
constexpr float kWalkWaistCm = 90.f;

const FString kCutsceneNoShot;

ftcore::CutsceneVec3 CutsceneToCore(const FVector& Cm)
{
	return {static_cast<float>(Cm.X) / kCutsceneCmPerMetre, static_cast<float>(Cm.Y) / kCutsceneCmPerMetre, static_cast<float>(Cm.Z) / kCutsceneCmPerMetre};
}

FVector CutsceneToUnreal(const ftcore::CutsceneVec3& M)
{
	return FVector(M.x * kCutsceneCmPerMetre, M.y * kCutsceneCmPerMetre, M.z * kCutsceneCmPerMetre);
}

// The lens of each framing, in millimetres on the cine camera's default filmback.
float CutsceneFocalLength(ftcore::ShotFraming Framing)
{
	switch (Framing)
	{
	case ftcore::ShotFraming::Wide: return 24.f;
	case ftcore::ShotFraming::Medium: return 35.f;
	case ftcore::ShotFraming::Close: return 50.f;
	case ftcore::ShotFraming::OverShoulder: return 40.f;
	case ftcore::ShotFraming::TwoShot: return 30.f;
	case ftcore::ShotFraming::Insert: return 50.f;
	}
	return 35.f;
}

// In place, who stands on subject mark `Id`: the lead on subj_a (0), and on the stage's other subject
// marks, in their order, the followers nearest first (1, 2, ...). -1 when `Id` is no subject mark.
int32 SubjectOrdinal(std::string_view Id)
{
	if (Id == ftcore::kCutsceneLeadMark)
	{
		return 0;
	}
	int32 Ordinal = 0;
	for (uint32 I = 0; I < ftcore::cutsceneStageMarkCount(); ++I)
	{
		const ftcore::CutsceneStageMark& Mark = ftcore::cutsceneStageMark(I);
		if (!Mark.subject || Mark.id == ftcore::kCutsceneLeadMark)
		{
			continue;
		}
		++Ordinal;
		if (Mark.id == Id)
		{
			return Ordinal;
		}
	}
	return -1;
}
} // namespace

AFTCutsceneDirector::AFTCutsceneDirector()
{
	PrimaryActorTick.bCanEverTick = true;
	// After the lead's spring arm has placed the gameplay camera: in place the blends start from it.
	PrimaryActorTick.TickGroup = TG_PostPhysics;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AFTCutsceneDirector::LoadPlan(const FString& Scene, int32 Beat, int32 Step, ftcore::CutscenePlan& Out)
{
	Out.clear();
	const FString Path = FPaths::ProjectDir() / TEXT("Data/Cutscene/cutscenes.json");
	TArray<uint8> Bytes;
	ftcore::CutsceneTable Table;
	std::string Error;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Cutscene: %s not found; playing one wide shot."), *Path);
	}
	else if (!ftcore::importCutsceneTable(FT::BytesAsText(Bytes), Table, &Error))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Cutscene: cutscenes.json refused (%s); playing one wide shot."), UTF8_TO_TCHAR(Error.c_str()));
	}
	else
	{
		const uint32 BeatIndex = static_cast<uint32>(FMath::Max(Beat, 0));
		const uint32 StepIndex = static_cast<uint32>(FMath::Max(Step, 0));
		Out = Table.find(FT::ToUtf8(Scene), BeatIndex, StepIndex);
		return;
	}
	ftcore::CutsceneShot Shot;
	Shot.setSpot("spot_a");
	Shot.setSubject("subj_a");
	Shot.framing = ftcore::ShotFraming::Wide;
	Shot.duration = 3.f;
	Out.add(Shot);
}

void AFTCutsceneDirector::HostSteps(bool bInPlaceHost, bool bRegister)
{
	bInPlace = bInPlaceHost;
	UFTStepHostSubsystem* Hosts = (bRegister && GetWorld()) ? GetWorld()->GetSubsystem<UFTStepHostSubsystem>() : nullptr;
	if (Hosts)
	{
		Hosts->Register(EFTStepHost::Cutscene, FFTStartStepHere::CreateUObject(this, &AFTCutsceneDirector::StartStepHere));
		bRegistered = true;
	}
}

void AFTCutsceneDirector::EndPlay(const EEndPlayReason::Type Reason)
{
	if (bRegistered)
	{
		if (UFTStepHostSubsystem* Hosts = GetWorld() ? GetWorld()->GetSubsystem<UFTStepHostSubsystem>() : nullptr)
		{
			Hosts->Unregister(EFTStepHost::Cutscene, this);
		}
		bRegistered = false;
	}
	// Destroyed mid-cutscene on a map that stays: the lead gets its view, input and party back. (A map
	// that unloads takes them along.)
	if (bInPlace && Reason == EEndPlayReason::Destroyed && (bBeginPending || (bStarted && !bFinished)))
	{
		EndInPlace();
	}
	Super::EndPlay(Reason);
}

void AFTCutsceneDirector::StartStepHere()
{
	bStarted = false;
	bFinished = false;
	bBeginPending = false;
	bControlRestored = false;
	bWasBlendingOut = false;
	bOrbitValid = false;
	PulledShots = 0;
	WalkedCount = 0;
	for (int32 K = 0; K < kWalkSlots; ++K)
	{
		bWalking[K] = false;
		bWalkArrived[K] = false;
	}
	bFast = FParse::Param(FCommandLine::Get(), TEXT("AlphaFast"));
	bBlendTest = FParse::Param(FCommandLine::Get(), TEXT("CutsceneBlendTest"));

	const UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	const FFTStoryCarry* Carry = (GI && GI->StoryCarry.bValid && !GI->StoryCarry.bDone) ? &GI->StoryCarry : nullptr;
	bHasStep = Carry != nullptr;
	bAutoPlayStep = Carry && Carry->bAutoPlay;
	// The HUD's text, built once per step. The title comes from Core, whatever the step's token is (CG:
	// "Cutscene").
	if (Carry)
	{
		LoadPlan(Carry->Scene, Carry->Beat, Carry->Step, StepPlan);
		const FString& Token = Carry->StepToken.IsEmpty() ? Carry->Kind : Carry->StepToken;
		TitleText = FT::ToFString(ftcore::stubTitle(FT::ToUtf8(Token)));
		StepText = FString::Printf(TEXT("Step %d of %d"), Carry->Step + 1, FMath::Max(Carry->StepCount, 1));
	}
	else
	{
		LoadPlan(FString(), -1, 0, StepPlan);
		TitleText = FT::ToFString(ftcore::stubTitle("CG"));
		StepText = TEXT("No story step (preview of the default shots)");
	}
	const uint32 ShotCount = StepPlan.size();
	ShotTexts.Reset(static_cast<int32>(ShotCount));
	for (uint32 I = 0; I < ShotCount; ++I)
	{
		ShotTexts.Add(FString::Printf(TEXT("Shot %u of %u"), I + 1, ShotCount));
	}

	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!bInPlace)
	{
		Play(StepPlan, bAutoPlayStep);
		// Look through the director's camera rather than the pawn.
		if (PC && Camera)
		{
			PC->SetViewTarget(Camera.Get());
		}
		return;
	}
	if (!Carry)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Cutscene: in place without a story step; nothing to play."));
		return;
	}
	AFTExploreCharacter* Body = Cast<AFTExploreCharacter>(UGameplayStatics::GetPlayerPawn(this, 0));
	Lead = Body;
	LeadCamera = Body ? Body->FindComponentByClass<UCameraComponent>() : nullptr;
	LeadBoom = Body ? Body->FindComponentByClass<USpringArmComponent>() : nullptr;
	ExploreDirector = nullptr;
	if (World)
	{
		for (TActorIterator<AFTExploreDirector> It(World); It; ++It)
		{
			ExploreDirector = *It;
			break;
		}
	}
	// The lead stops where it is and the party stands; the stage goes down once a traversal is over (Tick).
	if (Body && PC)
	{
		Body->DisableInput(PC);
	}
	if (AFTExploreDirector* Explore = ExploreDirector.Get())
	{
		Explore->SetPartyHeld(true);
	}
	PendingSince = UFTPauseSubsystem::WallSeconds();
	bBeginPending = true;
}

void AFTCutsceneDirector::EnsureCamera()
{
	if (Camera || !GetWorld())
	{
		return;
	}
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Camera = GetWorld()->SpawnActor<ACineCameraActor>(GetActorLocation(), FRotator::ZeroRotator, Spawn);
}

void AFTCutsceneDirector::Play(const ftcore::CutscenePlan& Plan, bool bAutoPlay)
{
	EnsureCamera();
	if (Camera)
	{
		if (UCineCameraComponent* Cine = Camera->GetCineCameraComponent())
		{
			// Blockouts stay sharp: no depth of field.
			Cine->FocusSettings.FocusMethod = ECameraFocusMethod::Disable;
		}
	}
	bFast = FParse::Param(FCommandLine::Get(), TEXT("AlphaFast"));
	Player.begin(Plan, bAutoPlay);
	ResolveMarkers();
	LastRealTime = UFTPauseSubsystem::WallSeconds();
	AppliedShot = -1;
	bStarted = true;
	ApplyCamera();
}

bool AFTCutsceneDirector::Press()
{
	return Player.press();
}

const FString& AFTCutsceneDirector::GetShotText() const
{
	if (!bStarted)
	{
		return kCutsceneNoShot;
	}
	const int32 Index = static_cast<int32>(Player.currentIndex());
	return ShotTexts.IsValidIndex(Index) ? ShotTexts[Index] : kCutsceneNoShot;
}

bool AFTCutsceneDirector::FindMarker(std::string_view Id, bool bSubject, FVector& Out) const
{
	if (Id.empty() || !GetWorld())
	{
		return false;
	}
	const FName Tag(*FT::ToFString(Id));
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (!It->ActorHasTag(Tag))
		{
			continue;
		}
		if (!bSubject)
		{
			Out = It->GetActorLocation();
			return true;
		}
		FVector Origin, Extent;
		It->GetActorBounds(false, Origin, Extent);
		Out = Extent.IsNearlyZero() ? It->GetActorLocation() : FVector(Origin.X, Origin.Y, Origin.Z - Extent.Z);
		return true;
	}
	return false;
}

void AFTCutsceneDirector::ResolveMarkers()
{
	// Each missing id is logged once.
	TArray<FString, TInlineAllocator<8>> Missing;
	const auto Lookup = [&](std::string_view Id, bool bSubject, FVector& Out)
	{
		if (FindMarker(Id, bSubject, Out))
		{
			return true;
		}
		const FString Name = FT::ToFString(Id);
		if (!Missing.Contains(Name))
		{
			Missing.Add(Name);
			UE_LOG(LogForgottenThird, Warning, TEXT("Cutscene: no actor tagged %s in the map."), *Name);
		}
		return false;
	};

	const ftcore::CutscenePlan& Plan = Player.plan();
	for (uint32 I = 0; I < Plan.size(); ++I)
	{
		const ftcore::CutsceneShot& Shot = Plan.shot(I);
		FVector Subject = FVector::ZeroVector;
		Lookup(Shot.subject(), true, Subject);
		FVector Spot;
		if (!Lookup(Shot.spot(), false, Spot))
		{
			Spot = Subject + FVector(400.f, 0.f, 170.f);
		}
		FVector Subject2 = FVector::ZeroVector;
		bShotHasSubject2[I] = Shot.hasSubject2() && Lookup(Shot.subject2(), true, Subject2);
		ShotSpot[I] = CutsceneToCore(Spot);
		ShotSubject[I] = CutsceneToCore(Subject);
		ShotSubject2[I] = CutsceneToCore(Subject2);
	}
}

void AFTCutsceneDirector::ApplyCamera()
{
	if (!Camera || Player.shotCount() == 0)
	{
		return;
	}
	const uint32 Index = Player.currentIndex();
	const ftcore::CutsceneShot& Shot = Player.currentShot();
	ftcore::CameraPose Pose = ftcore::cutsceneCamera(Shot, Player.shotProgress(), ShotSpot[Index], ShotSubject[Index],
		bShotHasSubject2[Index] ? &ShotSubject2[Index] : nullptr);
	float Focal = CutsceneFocalLength(Shot.framing);
	if (Player.blending() && Index > 0)
	{
		const uint32 Prev = Index - 1;
		const ftcore::CameraPose From = ftcore::cutsceneCamera(Player.previousShot(), 1.f, ShotSpot[Prev], ShotSubject[Prev],
			bShotHasSubject2[Prev] ? &ShotSubject2[Prev] : nullptr);
		const float W = Player.blendWeight();
		Pose = ftcore::lerpPose(From, Pose, W);
		Focal = FMath::Lerp(CutsceneFocalLength(Player.previousShot().framing), Focal, W);
		AppliedShot = -1; // set the lens again once the blend is over
	}
	else if (AppliedShot == static_cast<int32>(Index))
	{
		Focal = -1.f; // already set
	}

	const FVector Position = CutsceneToUnreal(Pose.position);
	const FVector Look = CutsceneToUnreal(Pose.lookAt);
	const FVector Forward = Look - Position;
	Camera->SetActorLocationAndRotation(Position, Forward.IsNearlyZero() ? Camera->GetActorRotation() : Forward.Rotation());
	if (Focal > 0.f)
	{
		if (UCineCameraComponent* Cine = Camera->GetCineCameraComponent())
		{
			Cine->SetCurrentFocalLength(Focal);
		}
		if (!Player.blending())
		{
			AppliedShot = static_cast<int32>(Index);
		}
	}
}

void AFTCutsceneDirector::BeginInPlace()
{
	bBeginPending = false;
	EnsureCamera();
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	AFTExploreCharacter* Body = Lead.Get();
	UCineCameraComponent* Cine = Camera ? Camera->GetCineCameraComponent() : nullptr;
	const UCameraComponent* Live = LeadCamera.Get();
	if (!PC || !Body || !Cine || !Live)
	{
		// Nothing to stage around: the step plays on the Cutscene map instead.
		UE_LOG(LogForgottenThird, Warning, TEXT("Cutscene: in place, no lead or camera to stage around; opening the Cutscene map."));
		UGameplayStatics::SetGlobalTimeDilation(this, 1.f);
		UGameplayStatics::OpenLevel(this, FName(FTStep::ScreenMap(ftcore::GameplayScreen::Cutscene)));
		return;
	}
	const FVector Feet = LeadFeet();
	const FVector2D Facing = Body->GetFacing2D();
	LeadYaw = FMath::RadiansToDegrees(FMath::Atan2(static_cast<float>(Facing.Y), static_cast<float>(Facing.X)));
	const ftcore::StageFrame Frame = ftcore::anchorForLead(CutsceneToCore(Feet), LeadYaw);

	// The cast: the lead, then the followers in view, nearest first. The sweep passes through the whole
	// party and the camera itself.
	CastMembers.Reset();
	CastMembers.Add(Body);
	SweepParams = FCollisionQueryParams(SCENE_QUERY_STAT(FTCutsceneCamera), false, this);
	SweepParams.AddIgnoredActor(Body);
	SweepParams.AddIgnoredActor(Camera.Get());
	if (const AFTExploreDirector* Explore = ExploreDirector.Get())
	{
		for (AFTPartyFollower* Follower : Explore->GetFollowers())
		{
			if (!Follower)
			{
				continue;
			}
			SweepParams.AddIgnoredActor(Follower);
			if (!Follower->IsHidden())
			{
				CastMembers.Add(Follower);
			}
		}
	}
	if (CastMembers.Num() > 2)
	{
		Algo::Sort(MakeArrayView(CastMembers.GetData() + 1, CastMembers.Num() - 1),
			[&Feet](const TObjectPtr<AActor>& A, const TObjectPtr<AActor>& B)
			{
				return FVector::DistSquared(A->GetActorLocation(), Feet) < FVector::DistSquared(B->GetActorLocation(), Feet);
			});
	}

	SavedControl = PC->GetControlRotation();
	// The cine camera sees what the gameplay camera sees: the same aspect handling and post process, and
	// no exposure or focus of its own.
	Cine->SetConstraintAspectRatio(false);
	Cine->FocusSettings.FocusMethod = ECameraFocusMethod::DoNotOverride;
	Cine->ExposureMethod = ECameraExposureMethod::DoNotOverride;
	Cine->PostProcessSettings = Live->PostProcessSettings;
	Cine->SetPostProcessBlendWeight(Live->PostProcessBlendWeight);

	Player.begin(StepPlan, bAutoPlayStep && !bBlendTest, true);
	ResolveStage(Frame);
	BeginWalks(Frame);
	LastRealTime = UFTPauseSubsystem::WallSeconds();
	AppliedShot = -1;
	bStarted = true;
	if (USpringArmComponent* Boom = LeadBoom.Get())
	{
		AddTickPrerequisiteComponent(Boom);
	}
	ApplyInPlace();
	PC->SetViewTarget(Camera.Get());
	LogSwitch(TEXT("view to the cine camera"));
}

void AFTCutsceneDirector::ResolveStage(const ftcore::StageFrame& Frame)
{
	// Each id with a problem is logged once.
	TArray<FString, TInlineAllocator<8>> Logged;
	const auto FirstTime = [&Logged](const FString& Name)
	{
		if (Logged.Contains(Name))
		{
			return false;
		}
		Logged.Add(Name);
		return true;
	};
	const ftcore::CutsceneStageMark* LeadMark = ftcore::findCutsceneStageMark(ftcore::kCutsceneLeadMark);
	const ftcore::CutsceneVec3 LeadAt = LeadMark ? LeadMark->position : ftcore::CutsceneVec3{};
	// The cast member a subject id follows, with where it stands now as the fallback; -1, with the mark's
	// place on the map, when no follower stands on it.
	const auto Subject = [&](std::string_view Id, ftcore::CutsceneVec3& Out) -> int8
	{
		int32 Ordinal = SubjectOrdinal(Id);
		if (Ordinal < 0)
		{
			const FString Name = FT::ToFString(Id);
			if (FirstTime(Name))
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("Cutscene: in place, %s is no subject mark; framing the lead."), *Name);
			}
			Ordinal = 0;
		}
		if (CastFeet(Ordinal, Out))
		{
			return static_cast<int8>(Ordinal);
		}
		const ftcore::CutsceneStageMark* Mark = ftcore::findCutsceneStageMark(Id);
		Out = ftcore::stageToWorld(Frame, Mark ? Mark->position : LeadAt);
		const FString Name = FT::ToFString(Id);
		if (FirstTime(Name))
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Cutscene: in place, no follower stands on %s; the shot frames its mark."), *Name);
		}
		return -1;
	};

	const ftcore::CutscenePlan& Shots = Player.plan();
	for (uint32 I = 0; I < Shots.size(); ++I)
	{
		const ftcore::CutsceneShot& Shot = Shots.shot(I);
		ShotSubjectCast[I] = Subject(Shot.subject(), ShotSubject[I]);
		bShotHasSubject2[I] = Shot.hasSubject2();
		ShotSubject2Cast[I] = bShotHasSubject2[I] ? Subject(Shot.subject2(), ShotSubject2[I]) : static_cast<int8>(-1);
		const ftcore::CutsceneStageMark* Spot = ftcore::findCutsceneStageMark(Shot.spot());
		if (!Spot)
		{
			const FString Name = FT::ToFString(Shot.spot());
			if (FirstTime(Name))
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("Cutscene: in place, no stage spot %s; the camera stands 4 m in front of the lead."), *Name);
			}
		}
		ShotSpot[I] = ftcore::stageToWorld(Frame, Spot ? Spot->position : ftcore::CutsceneVec3{LeadAt.x + 4.f, LeadAt.y, LeadAt.z + 1.7f});
	}
}

void AFTCutsceneDirector::BeginWalks(const ftcore::StageFrame& Frame)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	// The subject marks the shots frame: a follower walks only onto one of those.
	bool bFramed[kWalkSlots] = {};
	const auto Frames = [&bFramed](std::string_view Id)
	{
		const int32 Ordinal = SubjectOrdinal(Id);
		if (Ordinal > 0 && Ordinal < kWalkSlots)
		{
			bFramed[Ordinal] = true;
		}
	};
	const ftcore::CutscenePlan& Shots = Player.plan();
	for (uint32 I = 0; I < Shots.size(); ++I)
	{
		const ftcore::CutsceneShot& Shot = Shots.shot(I);
		Frames(Shot.subject());
		if (Shot.hasSubject2())
		{
			Frames(Shot.subject2());
		}
	}

	const FVector Feet = LeadFeet();
	const ftcore::CutsceneVec3 LeadAt = CutsceneToCore(Feet);
	int32 Ordinal = 0;
	for (uint32 I = 0; I < ftcore::cutsceneStageMarkCount() && Ordinal + 1 < kWalkSlots; ++I)
	{
		const ftcore::CutsceneStageMark& Mark = ftcore::cutsceneStageMark(I);
		if (!Mark.subject || Mark.id == ftcore::kCutsceneLeadMark)
		{
			continue;
		}
		++Ordinal;
		AFTPartyFollower* Follower = (bFramed[Ordinal] && CastMembers.IsValidIndex(Ordinal)) ? Cast<AFTPartyFollower>(CastMembers[Ordinal]) : nullptr;
		if (!Follower)
		{
			continue;
		}
		const FString Name = FT::ToFString(Mark.id);
		// The ground at the mark, near the lead's height: nobody walks off a ledge or into a wall.
		ftcore::CutsceneVec3 To = ftcore::stageToWorld(Frame, Mark.position);
		const FVector ToCm = CutsceneToUnreal(To);
		FHitResult Ground;
		if (!World->LineTraceSingleByChannel(Ground, FVector(ToCm.X, ToCm.Y, Feet.Z + 150.0), FVector(ToCm.X, ToCm.Y, Feet.Z - 300.0),
				ECC_Visibility, SweepParams)
			|| FMath::Abs(Ground.ImpactPoint.Z - Feet.Z) > kWalkGroundCm)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Cutscene: in place, no ground at %s near the lead; follower %d stays where it stands."), *Name, Ordinal);
			continue;
		}
		To.z = static_cast<float>(Ground.ImpactPoint.Z) / kCutsceneCmPerMetre;
		ftcore::MarkWalk& Walk = Walks[Ordinal];
		Walk.begin(CutsceneToCore(Follower->GetActorLocation()), To, LeadAt);
		if (Walk.arrived())
		{
			continue; // already on it
		}
		// Its own way there, leg by leg, at waist height.
		bool bClear = true;
		for (uint32 P = 0; P + 1 < Walk.pointCount() && bClear; ++P)
		{
			const FVector Waist(0.0, 0.0, kWalkWaistCm);
			bClear = !World->LineTraceTestByChannel(CutsceneToUnreal(Walk.point(P)) + Waist, CutsceneToUnreal(Walk.point(P + 1)) + Waist,
				ECC_Visibility, SweepParams);
		}
		if (!bClear)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Cutscene: in place, the way onto %s is blocked; follower %d stays where it stands."), *Name, Ordinal);
			continue;
		}
		bWalking[Ordinal] = true;
		WalkSince[Ordinal] = World->GetTimeSeconds();
		++WalkedCount;
		UE_LOG(LogForgottenThird, Display, TEXT("Cutscene: in place, follower %d walks %.1f m onto %s."), Ordinal, Walk.remaining(), *Name);
	}
}

void AFTCutsceneDirector::TickWalks(float DeltaSeconds)
{
	const FVector Feet = LeadFeet();
	for (int32 K = 1; K < kWalkSlots; ++K)
	{
		AFTPartyFollower* Follower = (bWalking[K] && CastMembers.IsValidIndex(K)) ? Cast<AFTPartyFollower>(CastMembers[K]) : nullptr;
		if (!IsValid(Follower))
		{
			bWalking[K] = false;
			continue;
		}
		ftcore::MarkWalk& Walk = Walks[K];
		Walk.step(DeltaSeconds);
		const FVector At = CutsceneToUnreal(Walk.position());
		if (Walk.arrived() && !bWalkArrived[K])
		{
			// On the mark it turns to face the lead; Place, still called every frame, plays the turn.
			bWalkArrived[K] = true;
			Follower->FaceTo(FMath::RadiansToDegrees(FMath::Atan2(static_cast<float>(Feet.Y - At.Y), static_cast<float>(Feet.X - At.X))));
			const double Took = GetWorld() ? GetWorld()->GetTimeSeconds() - WalkSince[K] : 0.0;
			UE_LOG(LogForgottenThird, Display, TEXT("Cutscene: in place, follower %d on its mark after %.1f s."), K, Took);
		}
		const float Stop = Walk.stopDistance();
		Follower->Place(FVector2D(At.X, At.Y), static_cast<float>(Feet.Z), DeltaSeconds, Walk.wantSpeed() * kCutsceneCmPerMetre,
			Stop < 0.f ? -1.f : Stop * kCutsceneCmPerMetre);
	}
}

FVector AFTCutsceneDirector::LeadFeet() const
{
	const AFTExploreCharacter* Body = Lead.Get();
	if (!Body)
	{
		return GetActorLocation();
	}
	const UCapsuleComponent* Capsule = Body->GetCapsuleComponent();
	return Body->GetActorLocation() - FVector(0.0, 0.0, Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 0.0);
}

bool AFTCutsceneDirector::CastFeet(int32 Index, ftcore::CutsceneVec3& Out) const
{
	if (!CastMembers.IsValidIndex(Index) || !IsValid(CastMembers[Index]))
	{
		return false;
	}
	// A follower's origin is on its feet; the lead's is its capsule's centre.
	Out = CutsceneToCore(Index == 0 ? LeadFeet() : CastMembers[Index]->GetActorLocation());
	return true;
}

ftcore::CameraPose AFTCutsceneDirector::PoseOfShot(uint32 Index, const ftcore::CutsceneShot& Shot, float Progress) const
{
	// The subjects where they stand now; for a mark no one stands on, the point stored at the start.
	ftcore::CutsceneVec3 Subject = ShotSubject[Index];
	CastFeet(ShotSubjectCast[Index], Subject);
	ftcore::CutsceneVec3 Subject2 = ShotSubject2[Index];
	if (bShotHasSubject2[Index])
	{
		CastFeet(ShotSubject2Cast[Index], Subject2);
	}
	return ftcore::cutsceneCamera(Shot, Progress, ShotSpot[Index], Subject, bShotHasSubject2[Index] ? &Subject2 : nullptr);
}

void AFTCutsceneDirector::ShotPose(ftcore::CameraPose& Pose, float& Focal) const
{
	const uint32 Index = Player.currentIndex();
	const ftcore::CutsceneShot& Shot = Player.currentShot();
	Pose = PoseOfShot(Index, Shot, Player.shotProgress());
	Focal = CutsceneFocalLength(Shot.framing);
	if (Player.blending() && Index > 0)
	{
		const float W = Player.blendWeight();
		Pose = ftcore::lerpPose(PoseOfShot(Index - 1, Player.previousShot(), 1.f), Pose, W);
		Focal = FMath::Lerp(CutsceneFocalLength(Player.previousShot().framing), Focal, W);
	}
}

float AFTCutsceneDirector::FovForFocal(float Focal) const
{
	const UCineCameraComponent* Cine = Camera ? Camera->GetCineCameraComponent() : nullptr;
	if (!Cine)
	{
		return HeldFov;
	}
	const float Width = Cine->Filmback.SensorWidth * Cine->LensSettings.SqueezeFactor;
	return FMath::RadiansToDegrees(2.f * FMath::Atan(Width / (2.f * FMath::Max(Focal, 1.f))));
}

void AFTCutsceneDirector::ApplyInPlace()
{
	UCineCameraComponent* Cine = Camera ? Camera->GetCineCameraComponent() : nullptr;
	if (!Cine || Player.shotCount() == 0)
	{
		return;
	}
	// The shots' camera; held where it got to once the blend out has begun.
	if (Player.playing() || Player.blendingIn())
	{
		ftcore::CameraPose Pose;
		float Focal = 0.f;
		ShotPose(Pose, Focal);
		HeldPos = CutsceneToUnreal(Pose.position);
		HeldLook = CutsceneToUnreal(Pose.lookAt);
		HeldFov = FovForFocal(Focal);
	}
	FVector Pos = HeldPos;
	FVector Look = HeldLook;
	float Fov = HeldFov;
	const float P = Player.presence();
	const UCameraComponent* Live = LeadCamera.Get();
	if (P < 1.f && Live && Lead.IsValid())
	{
		// Between the gameplay camera and the shots: orbit round the lead's feet, turning the bearing and
		// easing the distance and height, and swing the look from the gameplay view to the shot's.
		const FVector LivePos = Live->GetComponentLocation();
		const FVector Feet = LeadFeet();
		const FVector2D Pivot(Feet.X, Feet.Y);
		const FVector2D FromXY = FVector2D(LivePos.X, LivePos.Y) - Pivot;
		const FVector2D ToXY = FVector2D(HeldPos.X, HeldPos.Y) - Pivot;
		const float RadiusFrom = static_cast<float>(FromXY.Size());
		const float RadiusTo = static_cast<float>(ToXY.Size());
		float YawFrom = FMath::RadiansToDegrees(FMath::Atan2(static_cast<float>(FromXY.Y), static_cast<float>(FromXY.X)));
		float YawTo = FMath::RadiansToDegrees(FMath::Atan2(static_cast<float>(ToXY.Y), static_cast<float>(ToXY.X)));
		// A camera on the lead has no bearing of its own: it takes the other's.
		if (RadiusFrom < kOrbitMinRadius)
		{
			YawFrom = YawTo;
		}
		if (RadiusTo < kOrbitMinRadius)
		{
			YawTo = YawFrom;
		}
		const float Raw = FMath::UnwindDegrees(YawTo - YawFrom);
		float Delta = Raw;
		if (!bOrbitValid)
		{
			// Near half a turn either way, it goes round the lead's right.
			if (FMath::Abs(Raw) > kOrbitTieDegrees && FMath::Sin(FMath::DegreesToRadians(YawFrom + 0.5f * Raw - LeadYaw)) <= 0.f)
			{
				Delta = Raw - FMath::Sign(Raw) * 360.f;
			}
			bOrbitValid = true;
		}
		else
		{
			// The same way round as the frame before, even past half a turn.
			Delta = Raw + 360.f * FMath::RoundToFloat((OrbitDelta - Raw) / 360.f);
		}
		OrbitDelta = Delta;
		const float Yaw = FMath::DegreesToRadians(YawFrom + P * Delta);
		const float Radius = FMath::Lerp(RadiusFrom, RadiusTo, P);
		Pos = FVector(Pivot.X + Radius * FMath::Cos(Yaw), Pivot.Y + Radius * FMath::Sin(Yaw), FMath::Lerp(LivePos.Z, HeldPos.Z, static_cast<double>(P)));
		const FVector Chest = Feet + FVector(0.0, 0.0, kChestHeightCm);
		const FVector LiveLook = LivePos + Live->GetForwardVector() * FVector::Dist(Chest, LivePos);
		Look = FMath::Lerp(LiveLook, HeldLook, static_cast<double>(P));
		Fov = FMath::Lerp(Live->FieldOfView, HeldFov, P);
	}
	if (P > 0.f && GetWorld())
	{
		// In front of whatever stands between the look point and the camera.
		FHitResult Hit;
		if (GetWorld()->SweepSingleByChannel(Hit, Look, Pos, FQuat::Identity, ECC_Camera, FCollisionShape::MakeSphere(kSweepRadius), SweepParams)
			&& !Hit.bStartPenetrating)
		{
			Pos = Hit.Location;
			PulledShots |= 1u << FMath::Min(Player.currentIndex(), 31u);
		}
	}
	const FVector Forward = Look - Pos;
	Camera->SetActorLocationAndRotation(Pos, Forward.IsNearlyZero() ? Camera->GetActorRotation() : Forward.Rotation());
	Cine->SetFieldOfView(Fov);
}

void AFTCutsceneDirector::LogSwitch(const TCHAR* What) const
{
	const UCameraComponent* Live = LeadCamera.Get();
	const UCineCameraComponent* Cine = Camera ? Camera->GetCineCameraComponent() : nullptr;
	if (!Live || !Cine)
	{
		return;
	}
	const double Cm = FVector::Dist(Cine->GetComponentLocation(), Live->GetComponentLocation());
	const double Dot = FMath::Clamp(FVector::DotProduct(Cine->GetForwardVector(), Live->GetForwardVector()), -1.0, 1.0);
	UE_LOG(LogForgottenThird, Display, TEXT("Cutscene: in place, %s (%.1f cm, %.1f deg, fov %+.1f)."), What, Cm,
		FMath::RadiansToDegrees(FMath::Acos(Dot)), Cine->FieldOfView - Live->FieldOfView);
}

void AFTCutsceneDirector::RestoreControl()
{
	if (bControlRestored)
	{
		return;
	}
	bControlRestored = true;
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		PC->SetControlRotation(SavedControl);
	}
}

void AFTCutsceneDirector::EndInPlace()
{
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	AFTExploreCharacter* Body = Lead.Get();
	if (bStarted && PC)
	{
		RestoreControl();
		LogSwitch(TEXT("view back to the lead"));
		if (Body)
		{
			PC->SetViewTarget(Body);
		}
	}
	if (Body && PC)
	{
		Body->EnableInput(PC);
		Body->ResetHeldInput();
	}
	if (AFTExploreDirector* Explore = ExploreDirector.Get())
	{
		Explore->SetPartyHeld(false);
	}
	if (USpringArmComponent* Boom = LeadBoom.Get())
	{
		RemoveTickPrerequisiteComponent(Boom);
	}
}

void AFTCutsceneDirector::Finish()
{
	if (bFinished)
	{
		return;
	}
	bFinished = true;
	UFTGameInstance* GI = GetGameInstance<UFTGameInstance>();
	const bool bCarry = bHasStep && GI && GI->StoryCarry.bValid;
	if (bCarry)
	{
		FFTStoryCarry& Carry = GI->StoryCarry;
		Carry.bDone = true;
		Carry.bWon = true;
		// Ids and counts only.
		if (bInPlace)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Cutscene: done %s beat %d step %d shots %u skipped %d in place, pulled %d, walked %d"),
				*Carry.Scene, Carry.Beat, Carry.Step, Player.shotCount(), Player.skipped() ? 1 : 0, FMath::CountBits(PulledShots), WalkedCount);
		}
		else
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Cutscene: done %s beat %d step %d shots %u skipped %d"), *Carry.Scene, Carry.Beat,
				Carry.Step, Player.shotCount(), Player.skipped() ? 1 : 0);
		}
	}
	if (bInPlace)
	{
		EndInPlace();
	}
	if (!bCarry)
	{
		return; // a preview: no carry to hand back, stay on the map
	}
	if (FTStep::ContinueHere(this))
	{
		return; // the beat's next step plays on this map
	}
	UGameplayStatics::SetGlobalTimeDilation(this, 1.f);
	const FString& ReturnMap = GI->StoryCarry.ReturnMap;
	if (!ReturnMap.IsEmpty())
	{
		UGameplayStatics::OpenLevel(this, FName(*ReturnMap));
	}
}

void AFTCutsceneDirector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bBeginPending)
	{
		// A vault, climb or roll carrying the lead ends first: the stage goes down where it lands.
		const AFTExploreCharacter* Body = Lead.Get();
		const bool bTraversing = Body && Body->IsTraversing();
		if (bTraversing && UFTPauseSubsystem::WallSeconds() - PendingSince < kTraversalWaitLimit)
		{
			return;
		}
		if (bTraversing)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Cutscene: in place, the lead was still traversing after %.0f s; staging anyway."), kTraversalWaitLimit);
		}
		BeginInPlace();
	}
	if (!bStarted || bFinished)
	{
		return;
	}
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (PC && (Player.playing() || Player.blendingIn())
		&& (PC->WasInputKeyJustPressed(EKeys::SpaceBar) || PC->WasInputKeyJustPressed(EKeys::Enter)
			|| PC->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Bottom)))
	{
		Player.press();
	}
	// Real time without the pause: the autoplay cap and the skip grace must not count the minutes a
	// player spent in the menu (this tick doesn't run while paused, so the gap would land here at once).
	const double Now = UFTPauseSubsystem::WallSeconds();
	const double Real = Now - LastRealTime;
	LastRealTime = Now;
	if (Player.active())
	{
		const AWorldSettings* Settings = GetWorld() ? GetWorld()->GetWorldSettings() : nullptr;
		const double Dilation = Settings ? Settings->GetEffectiveTimeDilation() : 1.0;
		Player.update(static_cast<double>(DeltaSeconds) * ftcore::StubScreen::speedFactor(bFast, Dilation));
		Player.tickReal(Real);
	}
	if (bInPlace && Player.blendingOut() && !bWasBlendingOut)
	{
		// The way back aims at the gameplay camera as it was, and picks its side afresh.
		RestoreControl();
		bOrbitValid = false;
	}
	bWasBlendingOut = Player.blendingOut();
	if (Player.finished())
	{
		// The camera stays where the last frame put it, so the switch logs what is left of the blend (a
		// cut under autoplay).
		Finish();
		return;
	}
	if (bInPlace)
	{
		TickWalks(DeltaSeconds);
		ApplyInPlace();
	}
	else
	{
		ApplyCamera();
	}
}
