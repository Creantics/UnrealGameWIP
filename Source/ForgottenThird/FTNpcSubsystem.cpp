#include "FTNpcSubsystem.h"

#include "FTNpcActor.h"
#include "FTText.h"
#include "FTWorldClockSubsystem.h"
#include "ForgottenThird.h"

#include "CollisionQueryParams.h"
#include "Debug/DebugDrawService.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "TimerManager.h"

namespace
{
TAutoConsoleVariable<int32> CVarNpcOverlay(TEXT("ft.NpcOverlay"), 0,
	TEXT("Development overlay of the nearest placed NPC's memory (1 on, 0 off)."));

const TCHAR* const kStanceNames[] = {TEXT("hostile"), TEXT("wary"), TEXT("neutral"), TEXT("warm"), TEXT("loyal")};
const TCHAR* const kExpNames[] = {TEXT("pending"), TEXT("reminding"), TEXT("kept"), TEXT("broken"), TEXT("missed")};

const TCHAR* StanceName(ftcore::NpcStance S)
{
	const int32 I = static_cast<int32>(S);
	return I >= 0 && I < 5 ? kStanceNames[I] : TEXT("?");
}

bool ReadData(const TCHAR* Name, std::string& Out, FString& OutError)
{
	const FString Path = FPaths::ProjectDir() / TEXT("Data") / Name;
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *Path);
		return false;
	}
	Out = FT::ToUtf8(Text);
	return true;
}
} // namespace

UFTNpcSubsystem::UFTNpcSubsystem() = default;
UFTNpcSubsystem::~UFTNpcSubsystem() = default;

void UFTNpcSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	World = MakeUnique<ftcore::NpcMemoryWorld>();
	FString Error;
	if (!Load(Error))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("NPC memory off: %s"), *Error);
	}
#if !UE_BUILD_SHIPPING
	const TCHAR* Cmd = FCommandLine::Get();
	if (FParse::Param(Cmd, TEXT("NpcOverlay")) || FParse::Param(Cmd, TEXT("NpcSliceTest")))
	{
		CVarNpcOverlay.AsVariable()->Set(1, ECVF_SetByCommandline);
	}
	OverlayHandle = UDebugDrawService::Register(TEXT("Game"), FDebugDrawDelegate::CreateUObject(this, &UFTNpcSubsystem::DrawOverlay));
#endif
}

void UFTNpcSubsystem::Deinitialize()
{
#if !UE_BUILD_SHIPPING
	if (OverlayHandle.IsValid())
	{
		UDebugDrawService::Unregister(OverlayHandle);
		OverlayHandle.Reset();
	}
#endif
	bReady = false;
	Facts = nullptr;
	Placed.Reset();
	Super::Deinitialize();
}

UFTNpcSubsystem* UFTNpcSubsystem::Get(const UObject* WorldContext)
{
	const UWorld* W = GEngine ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
	UGameInstance* GI = W ? W->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UFTNpcSubsystem>() : nullptr;
}

bool UFTNpcSubsystem::Load(FString& OutError)
{
	bReady = false;
	static const TCHAR* const kNames[] = {TEXT("Npc/tags.csv"), TEXT("Npc/deeds.csv"), TEXT("Npc/npc_values.csv"),
		TEXT("Npc/ties.csv"), TEXT("Npc/consequences.csv"), TEXT("Npc/expectations.csv"), TEXT("Npc/schedule.csv"),
		TEXT("Npc/memory_tuning.json"), TEXT("time_curves.csv"), TEXT("Npc/npc_lines.csv")};
	std::string Text[10];
	for (int32 I = 0; I < 10; ++I)
	{
		if (!ReadData(kNames[I], Text[I], OutError))
		{
			return false;
		}
	}
	const ftcore::NpcTableSources Src{Text[0], Text[1], Text[2], Text[3], Text[4], Text[5], Text[6], Text[7]};
	std::string Error;
	ftcore::NpcTables NewTables;
	ftcore::NpcGameDeeds NewDeeds;
	ftcore::TimeCurves NewCurves;
	if (!ftcore::importNpcTables(Src, NewTables, &Error) || !ftcore::npcResolveGameDeeds(NewTables, NewDeeds, &Error) ||
		!ftcore::parseTimeCurves(Text[8], NewCurves, &Error))
	{
		OutError = FT::ToFString(Error);
		return false;
	}
	Tables = MoveTemp(NewTables);
	Deeds = NewDeeds;
	Curves = NewCurves;
	LinesCsv = MoveTemp(Text[9]);
	FString LineError;
	if (!LoadLines(LineError))
	{
		// The memory runs without lines; SetFactSource retries (a fact clause needs the story state's reader).
		Lines = ftcore::NpcLineTable();
		UE_LOG(LogForgottenThird, Warning, TEXT("NPC lines not loaded: %s"), *LineError);
	}
	bReady = true;
	ResetMemory();
	for (const TWeakObjectPtr<AFTNpcActor>& P : Placed)
	{
		if (AFTNpcActor* Npc = P.Get())
		{
			Npc->SetNpcIndex(NpcIndexOf(Npc->NpcId));
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("NPC memory: %d NPCs, %d deed types, %d lines."), static_cast<int32>(Tables.npcs.size()),
		static_cast<int32>(Tables.deeds.size()), static_cast<int32>(Lines.lines.size()));
	return true;
}

bool UFTNpcSubsystem::LoadLines(FString& OutError)
{
	std::string Error;
	ftcore::NpcLineTable NewLines;
	if (!ftcore::importNpcLines(LinesCsv, Tables, Facts, NewLines, &Error))
	{
		OutError = FT::ToFString(Error);
		return false;
	}
	if (bReady)
	{
		// The said-line table is indexed by line: carry it over by line id (and everything else as it is).
		const ftcore::NpcMemorySave Kept = ftcore::npcCaptureSave(*World, Tables, Lines);
		Lines = MoveTemp(NewLines);
		ftcore::npcApplySave(*World, Tables, Lines, Kept);
		LastPick.Init(-1, static_cast<int32>(ftcore::kNpcMax));
	}
	else
	{
		Lines = MoveTemp(NewLines);
	}
	return true;
}

void UFTNpcSubsystem::ResetMemory()
{
	if (!bReady)
	{
		return;
	}
	ftcore::npcInit(*World, Tables);
	LastPick.Init(-1, static_cast<int32>(ftcore::kNpcMax));
	LastScore.Init(0, static_cast<int32>(ftcore::kNpcMax));
	LastMinute = MAX_uint32;
}

void UFTNpcSubsystem::SetFactSource(const ftcore::NpcFactSource* InFacts)
{
	Facts = InFacts;
	FString Error;
	if (!LinesCsv.empty() && !LoadLines(Error))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("NPC lines kept as they were: %s"), *Error);
	}
}

void UFTNpcSubsystem::Tick(float DeltaTime)
{
	const UGameInstance* GI = GetGameInstance();
	UWorld* W = GI ? GI->GetWorld() : nullptr;
	const UFTWorldClockSubsystem* Clock = W ? W->GetSubsystem<UFTWorldClockSubsystem>() : nullptr;
	if (!Clock || !Clock->IsRunning())
	{
		return;
	}
	const ftcore::WorldClock& C = Clock->GetClock();
	const uint32 Minute = ftcore::npcMinute(C.day, C.hours);
	if (Minute != LastMinute)
	{
		LastMinute = Minute;
		ftcore::npcOnClock(*World, Tables, C, Curves);
	}
}

TStatId UFTNpcSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFTNpcSubsystem, STATGROUP_Tickables);
}

void UFTNpcSubsystem::SetMemoryTime(uint32 Day, double Hours)
{
	if (!bReady)
	{
		return;
	}
	ftcore::WorldClock C;
	C.day = Day;
	C.hours = Hours;
	ftcore::npcOnClock(*World, Tables, C, Curves);
}

// ---- placed NPCs ----

void UFTNpcSubsystem::RegisterNpc(AFTNpcActor* Npc)
{
	if (!Npc)
	{
		return;
	}
	Npc->SetNpcIndex(bReady ? NpcIndexOf(Npc->NpcId) : -1);
	if (bReady && Npc->GetNpcIndex() < 0)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("NPC %s is not in the NPC tables: it remembers nothing."), *Npc->NpcId);
	}
	Placed.AddUnique(Npc);
}

void UFTNpcSubsystem::UnregisterNpc(AFTNpcActor* Npc)
{
	Placed.RemoveAll([Npc](const TWeakObjectPtr<AFTNpcActor>& P) { return !P.IsValid() || P.Get() == Npc; });
}

int32 UFTNpcSubsystem::NpcIndexOf(const FString& NpcId) const
{
	return Tables.npcIndex(FT::ToUtf8(NpcId));
}

AFTNpcActor* UFTNpcSubsystem::FindNpc(const FString& NpcId) const
{
	for (const TWeakObjectPtr<AFTNpcActor>& P : Placed)
	{
		if (AFTNpcActor* Npc = P.Get(); Npc && Npc->NpcId == NpcId)
		{
			return Npc;
		}
	}
	return nullptr;
}

bool UFTNpcSubsystem::Index(const AFTNpcActor* Npc, uint16& Out) const
{
	if (!bReady || !Npc || Npc->GetNpcIndex() < 0 || Npc->GetNpcIndex() >= World->npcCount)
	{
		return false;
	}
	Out = static_cast<uint16>(Npc->GetNpcIndex());
	return true;
}

uint8 UFTNpcSubsystem::LeadOf(int32 Lead)
{
	return Lead >= 0 && Lead < static_cast<int32>(ftcore::kLeadCount) ? static_cast<uint8>(Lead) : ftcore::kNoLead;
}

uint16 UFTNpcSubsystem::PlaceOf(const FVector& At)
{
	// 20 m cells: a second deed of a kind close by reinforces the first.
	const int32 X = FMath::FloorToInt32(At.X / 2000.0), Y = FMath::FloorToInt32(At.Y / 2000.0);
	const uint32 H = (static_cast<uint32>(X) * 73856093u) ^ (static_cast<uint32>(Y) * 19349663u);
	return static_cast<uint16>(1u + H % 65534u);
}

ftcore::NpcWhere UFTNpcSubsystem::Where(const FVector& At, int32 Lead, const AActor* Exclude, const AActor* Ignore) const
{
	ftcore::NpcWhere Out;
	Out.place = PlaceOf(At);
	Out.district = static_cast<uint16>(FMath::Clamp(District, 0, 65535));
	Out.lead = LeadOf(Lead);
	const UGameInstance* GI = GetGameInstance();
	const UWorld* W = GI ? GI->GetWorld() : nullptr;
	if (!W)
	{
		return Out;
	}
	// Placed NPCs in range and in sight, nearest first.
	TArray<TPair<double, uint16>, TInlineAllocator<ftcore::kNpcMax>> Seen;
	const FVector From = At + FVector(0.0, 0.0, 80.0);
	const double R2 = static_cast<double>(WitnessRadius) * WitnessRadius;
	for (const TWeakObjectPtr<AFTNpcActor>& P : Placed)
	{
		const AFTNpcActor* Npc = P.Get();
		uint16 I = 0;
		if (!Npc || Npc == Exclude || !Index(Npc, I) || Seen.Num() >= static_cast<int32>(ftcore::kNpcMax))
		{
			continue;
		}
		const FVector Eyes = Npc->GetEyes();
		const double D2 = FVector::DistSquared(From, Eyes);
		if (D2 > R2)
		{
			continue;
		}
		FCollisionQueryParams Params(SCENE_QUERY_STAT(FTNpcWitness), false, Npc);
		Params.AddIgnoredActor(Exclude);
		Params.AddIgnoredActor(Ignore);
		if (const APawn* Rider = Ignore ? Cast<APawn>(Ignore->GetOwner()) : nullptr)
		{
			Params.AddIgnoredActor(Rider);
		}
		FHitResult Hit;
		if (W->LineTraceSingleByChannel(Hit, From, Eyes, ECC_Visibility, Params))
		{
			continue; // something in the way
		}
		Seen.Emplace(D2, I);
	}
	Seen.Sort([](const TPair<double, uint16>& A, const TPair<double, uint16>& B) { return A.Key < B.Key; });
	for (int32 K = 0; K < Seen.Num() && K < static_cast<int32>(ftcore::kDeedWitnessMax); ++K)
	{
		Out.witnesses[static_cast<size_t>(K)] = Seen[K].Value;
	}
	return Out;
}

// ---- game events ----

int32 UFTNpcSubsystem::OnVanKnockdown(const FVector& At, AActor* Victim, int32 Lead, AActor* Van)
{
	if (!bReady)
	{
		return 0;
	}
	uint16 V = ftcore::kNoNpc;
	const AFTNpcActor* Npc = Cast<AFTNpcActor>(Victim);
	if (Npc && !Index(Npc, V))
	{
		V = ftcore::kNoNpc;
	}
	const uint32 Seq = ftcore::npcOnVanKnockdown(*World, Tables, Deeds, Where(At, Lead, Npc, Van), V);
	UE_LOG(LogForgottenThird, Display, TEXT("NPC deed: van knock-down (%s), seq %u."), Npc ? *Npc->NpcId : TEXT("crowd"), Seq);
	return static_cast<int32>(FMath::Min<uint32>(Seq, MAX_int32));
}

void UFTNpcSubsystem::OnFightEnd(const FVector& At, int32 Lead, bool bWon, bool bSpared)
{
	if (bReady)
	{
		ftcore::npcOnFightEnd(*World, Tables, Deeds, Where(At, Lead, nullptr), bWon, bSpared);
	}
}

FFTNpcTerms UFTNpcSubsystem::OnShop(AFTNpcActor* Keeper, int32 Lead, EFTNpcShopAct Act)
{
	FFTNpcTerms Out;
	uint16 K = 0;
	if (!Index(Keeper, K))
	{
		return Out;
	}
	const ftcore::NpcConsequence C = ftcore::npcOnShop(*World, Tables, Deeds, Where(Keeper->GetActorLocation(), Lead, Keeper), K,
		static_cast<ftcore::NpcShopAct>(Act));
	Out.PricePct = C.pricePct;
	Out.bShopOpen = C.shopOpen;
	Out.bDoorOpen = C.doorOpen;
	return Out;
}

void UFTNpcSubsystem::OnHelpedPedestrian(const FVector& At, int32 Lead)
{
	if (bReady)
	{
		ftcore::npcOnHelpedPedestrian(*World, Tables, Deeds, Where(At, Lead, nullptr));
	}
}

void UFTNpcSubsystem::OnMeet(AFTNpcActor* Npc, int32 Lead)
{
	uint16 I = 0;
	if (Index(Npc, I))
	{
		ftcore::npcOnMeet(*World, Tables, Deeds, Where(Npc->GetActorLocation(), Lead, Npc), I);
	}
}

void UFTNpcSubsystem::OnDelivery(AFTNpcActor* Recipient, int32 Lead)
{
	uint16 I = 0;
	if (Index(Recipient, I))
	{
		ftcore::npcOnDelivery(*World, Tables, Deeds, Where(Recipient->GetActorLocation(), Lead, Recipient), I);
	}
}

void UFTNpcSubsystem::OnGift(AFTNpcActor* Giver, int32 Lead, bool bAccepted)
{
	uint16 I = 0;
	if (Index(Giver, I))
	{
		ftcore::npcOnGift(*World, Tables, Deeds, Where(Giver->GetActorLocation(), Lead, Giver), I, bAccepted);
	}
}

void UFTNpcSubsystem::OnGate(AFTNpcActor* Owner, int32 Lead, int32 Gate, bool bMended)
{
	uint16 I = 0;
	if (Index(Owner, I))
	{
		ftcore::NpcWhere At = Where(Owner->GetActorLocation(), Lead, Owner);
		At.place = static_cast<uint16>(FMath::Clamp(Gate, 1, 65535));
		ftcore::npcOnGate(*World, Tables, Deeds, At, I, bMended);
	}
}

FString UFTNpcSubsystem::PickLine(AFTNpcActor* Npc, int32 Lead, EFTNpcLineKind Kind)
{
	uint16 I = 0;
	if (!Index(Npc, I))
	{
		return FString();
	}
	const ftcore::NpcLinePick P = ftcore::npcPickLine(*World, Tables, Lines, I, LeadOf(Lead), static_cast<ftcore::NpcLineKind>(Kind), Facts);
	LastPick[I] = P.line;
	LastScore[I] = P.score;
	return P.line >= 0 ? FT::ToFString(Lines.lines[static_cast<size_t>(P.line)].id) : FString();
}

FString UFTNpcSubsystem::GetStance(AFTNpcActor* Npc, int32 Lead) const
{
	uint16 I = 0;
	return Index(Npc, I) ? FString(StanceName(ftcore::npcStance(*World, Tables, I, LeadOf(Lead)))) : FString();
}

FFTNpcTerms UFTNpcSubsystem::GetTerms(AFTNpcActor* Npc, int32 Lead) const
{
	FFTNpcTerms Out;
	uint16 I = 0;
	if (Index(Npc, I))
	{
		const ftcore::NpcConsequence C = ftcore::npcConsequence(*World, Tables, I, LeadOf(Lead));
		Out.PricePct = C.pricePct;
		Out.bShopOpen = C.shopOpen;
		Out.bDoorOpen = C.doorOpen;
	}
	return Out;
}

// ---- save ----

FString UFTNpcSubsystem::CaptureSaveJson() const
{
	return bReady ? FT::ToFString(ftcore::encodeNpcMemorySave(ftcore::npcCaptureSave(*World, Tables, Lines))) : FString();
}

bool UFTNpcSubsystem::ApplySaveJson(const FString& Json, FString& OutError)
{
	if (!bReady)
	{
		OutError = TEXT("NPC memory is not loaded");
		return false;
	}
	ftcore::NpcMemorySave Save;
	std::string Error;
	if (!ftcore::decodeNpcMemorySave(FT::ToUtf8(Json), Save, &Error))
	{
		OutError = FT::ToFString(Error);
		return false;
	}
	const ftcore::NpcMemoryLoadReport Report = ftcore::npcApplySave(*World, Tables, Lines, Save);
	for (const std::string& Id : Report.unknownIds)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("NPC save: %s is no longer in the tables, dropped."), *FT::ToFString(Id));
	}
	LastPick.Init(-1, static_cast<int32>(ftcore::kNpcMax));
	LastScore.Init(0, static_cast<int32>(ftcore::kNpcMax));
	LastMinute = MAX_uint32; // the next tick moves the memory clock to the world's (never back)
	return true;
}

// ---- overlay ----

// Development only: the text lines allocate while the overlay is on (never in a shipping build).
void UFTNpcSubsystem::DrawOverlay(UCanvas* Canvas, APlayerController* PC)
{
#if !UE_BUILD_SHIPPING
	if (!bReady || !Canvas || CVarNpcOverlay.GetValueOnGameThread() == 0 || !GEngine)
	{
		return;
	}
	const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	FVector From = FVector::ZeroVector;
	if (Pawn)
	{
		From = Pawn->GetActorLocation();
	}
	else if (PC && PC->PlayerCameraManager)
	{
		From = PC->PlayerCameraManager->GetCameraLocation();
	}
	const AFTNpcActor* Best = nullptr;
	double BestD = TNumericLimits<double>::Max();
	for (const TWeakObjectPtr<AFTNpcActor>& P : Placed)
	{
		const AFTNpcActor* Npc = P.Get();
		if (Npc && Npc->GetNpcIndex() >= 0 && Npc->GetWorld() == Canvas->GetWorld() && FVector::DistSquared(From, Npc->GetActorLocation()) < BestD)
		{
			BestD = FVector::DistSquared(From, Npc->GetActorLocation());
			Best = Npc;
		}
	}
	uint16 I = 0;
	ftcore::NpcDebugView V;
	if (!Best || !Index(Best, I) || !ftcore::npcDebugView(*World, Tables, I, V))
	{
		return;
	}
	UFont* Font = GEngine->GetSmallFont();
	float Y = 120.f;
	auto Line = [&](const FString& Text) {
		Canvas->SetDrawColor(FColor::White);
		Canvas->DrawText(Font, Text, 24.f, Y);
		Y += 14.f;
	};
	Line(FString::Printf(TEXT("NPC %s  (memory minute %u, %u memories%s)"), *Best->NpcId, World->now, static_cast<uint32>(V.memCount), V.dead ? TEXT(", dead") : TEXT("")));
	Line(FString::Printf(TEXT("axes T %d  F %d  R %d   party %s"), V.axes[0], V.axes[1], V.axes[2], StanceName(V.stance[ftcore::kLeadCount])));
	for (uint32 L = 0; L < ftcore::kLeadCount; ++L)
	{
		Line(FString::Printf(TEXT("  lead %u: %+d %+d %+d  %s"), L, V.offsets[L][0], V.offsets[L][1], V.offsets[L][2], StanceName(V.stance[L])));
	}
	for (uint32 K = 0; K < V.topCount; ++K)
	{
		const ftcore::NpcDebugMemory& M = V.top[K];
		const FString Type = M.type < Tables.deeds.size() ? FT::ToFString(Tables.deeds[M.type].id) : FString(TEXT("?"));
		Line(FString::Printf(TEXT("  %s  S %d  at %u  hops %u  x%u  leads %u%u%u%s"), *Type, M.salience, M.deedMinute, static_cast<uint32>(M.hops), static_cast<uint32>(M.count),
			(M.leads & 1u) ? 1u : 0u, (M.leads & 2u) ? 1u : 0u, (M.leads & 4u) ? 1u : 0u, M.pinned ? TEXT("  pinned") : TEXT("")));
	}
	Line(FString::Printf(TEXT("rumours due %u (next at %u)"), static_cast<uint32>(V.rumoursQueued), V.nextRumourDue));
	for (uint32 K = 0; K < V.expCount; ++K)
	{
		const uint16 E = V.exps[K].exp;
		const int32 S = static_cast<int32>(V.exps[K].state);
		Line(FString::Printf(TEXT("  expects %s: %s"), E < Tables.expectations.size() ? *FT::ToFString(Tables.expectations[E].id) : TEXT("?"),
			S >= 0 && S < 5 ? kExpNames[S] : TEXT("?")));
	}
	const int32 Last = LastPick.IsValidIndex(I) ? LastPick[I] : -1;
	Line(FString::Printf(TEXT("last line: %s (score %d)"), Last >= 0 ? *FT::ToFString(Lines.lines[static_cast<size_t>(Last)].id) : TEXT("-"),
		LastScore.IsValidIndex(I) ? LastScore[I] : 0));
#endif
}

// ---------------------------------------------------------------- the slice placeholders and the scripted day

bool UFTNpcWorldSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* W = Cast<UWorld>(Outer);
	const TCHAR* Cmd = FCommandLine::Get();
	return W && (W->WorldType == EWorldType::Game || W->WorldType == EWorldType::PIE) &&
		(FParse::Param(Cmd, TEXT("NpcSlice")) || FParse::Param(Cmd, TEXT("NpcSliceTest"))) && Super::ShouldCreateSubsystem(Outer);
}

void UFTNpcWorldSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	PlacePlaceholders(InWorld);
	if (FParse::Param(FCommandLine::Get(), TEXT("NpcSliceTest")))
	{
		InWorld.GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &UFTNpcWorldSubsystem::RunSliceTest));
	}
}

void UFTNpcWorldSubsystem::PlacePlaceholders(UWorld& InWorld)
{
	static const TCHAR* const kSlice[] = {TEXT("npc_shop_01"), TEXT("npc_fixer_01"), TEXT("npc_kid_01")};
	// Beside the player start (or the origin), 3 m apart, 4 m ahead; a placed actor of the same id is kept.
	FVector Anchor = FVector::ZeroVector;
	FRotator Facing = FRotator::ZeroRotator;
	for (TActorIterator<APlayerStart> It(&InWorld); It; ++It)
	{
		Anchor = It->GetActorLocation();
		Facing = It->GetActorRotation();
		break;
	}
	const FVector Ahead = Facing.Vector().GetSafeNormal2D();
	const FVector Side(-Ahead.Y, Ahead.X, 0.0);
	for (int32 K = 0; K < 3; ++K)
	{
		bool bThere = false;
		for (TActorIterator<AFTNpcActor> It(&InWorld); It && !bThere; ++It)
		{
			bThere = It->NpcId == kSlice[K];
		}
		if (bThere)
		{
			continue;
		}
		FVector At = Anchor + Ahead * 400.0 + Side * (K - 1) * 300.0;
		FHitResult Hit;
		if (InWorld.LineTraceSingleByChannel(Hit, At + FVector(0, 0, 500), At - FVector(0, 0, 3000), ECC_Visibility))
		{
			At = Hit.ImpactPoint + FVector(0, 0, 92);
		}
		const FTransform T(FRotator(0.0, (Facing.Yaw + 180.0), 0.0), At);
		if (AFTNpcActor* Npc = InWorld.SpawnActorDeferred<AFTNpcActor>(AFTNpcActor::StaticClass(), T, nullptr, nullptr,
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn))
		{
			Npc->NpcId = kSlice[K];
			Npc->FinishSpawning(T);
		}
	}
}

void UFTNpcWorldSubsystem::RunSliceTest()
{
	UWorld* W = GetWorld();
	UFTNpcSubsystem* N = UFTNpcSubsystem::Get(W);
	if (!N || !N->IsReady())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("NPCSLICE no NPC memory loaded"));
		return;
	}
	AFTNpcActor* Shop = N->FindNpc(TEXT("npc_shop_01"));
	AFTNpcActor* Fixer = N->FindNpc(TEXT("npc_fixer_01"));
	AFTNpcActor* Kid = N->FindNpc(TEXT("npc_kid_01"));
	if (!Shop || !Fixer || !Kid)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("NPCSLICE the three placeholders are not placed"));
		return;
	}
	// The doctest's walk: a fresh memory from day 1, and the three out of each other's sight (its deeds have no
	// witnesses but the one dealt with).
	N->HoldClock(true);
	N->ResetMemory();
	const float Radius = N->WitnessRadius;
	N->WitnessRadius = 0.f;
	int32 Pass = 0, Total = 0;
	auto Check = [&](const TCHAR* What, bool bOk) {
		Pass += bOk ? 1 : 0;
		++Total;
		UE_LOG(LogForgottenThird, Display, TEXT("NPCSLICE %s %s"), What, bOk ? TEXT("ok") : TEXT("MISMATCH"));
	};
	const ftcore::NpcMemoryWorld& Mem = *N->GetMemoryWorld();
	const uint16 ShopI = static_cast<uint16>(Shop->GetNpcIndex()), KidI = static_cast<uint16>(Kid->GetNpcIndex());
	auto Expect = [&](const TCHAR* When, AFTNpcActor* Npc, int32 Lead, EFTNpcLineKind Kind, const TCHAR* Want) {
		const FString Got = N->PickLine(Npc, Lead, Kind);
		const bool bOk = Got == Want;
		Pass += bOk ? 1 : 0;
		++Total;
		const FFTNpcTerms Terms = N->GetTerms(Npc, Lead);
		UE_LOG(LogForgottenThird, Display, TEXT("NPCSLICE %s %s lead %d line %s (want %s) stance %s price %d%% %s"), When, *Npc->NpcId,
			Lead, Got.IsEmpty() ? TEXT("-") : *Got, Want, *N->GetStance(Npc, Lead), Terms.PricePct, bOk ? TEXT("ok") : TEXT("MISMATCH"));
	};
	const FVector Street = Shop->GetActorLocation() + FVector(5000.0, 0.0, 0.0); // out of everyone's sight

	N->SetMemoryTime(1, 8.0);
	Expect(TEXT("d1 08:00"), Shop, 0, EFTNpcLineKind::Greet, TEXT("shop_greet_warm"));
	Expect(TEXT("d1 08:00"), Fixer, 0, EFTNpcLineKind::Greet, TEXT("fixer_greet_plain"));
	Expect(TEXT("d1 08:00"), Kid, 0, EFTNpcLineKind::Greet, TEXT("kid_greet_plain"));
	N->SetMemoryTime(1, 9.0);
	N->OnDelivery(Shop, 0);
	Expect(TEXT("d1 09:00"), Shop, 0, EFTNpcLineKind::Greet, TEXT("shop_greet_delivery_self"));
	Expect(TEXT("d1 09:00"), Shop, 1, EFTNpcLineKind::Greet, TEXT("shop_greet_delivery_other"));
	N->SetMemoryTime(1, 10.0);
	N->OnGift(Kid, 2, true);
	Expect(TEXT("d1 10:00"), Kid, 2, EFTNpcLineKind::Greet, TEXT("kid_greet_gift_kept"));
	N->SetMemoryTime(1, 12.0);
	const uint32 Noon = static_cast<uint32>(N->OnVanKnockdown(Street, nullptr, 1));
	N->SetMemoryTime(1, 13.5);
	Check(TEXT("d1 13:30 the noon knock-down reached the shop as hearsay"),
		Noon != 0 && ftcore::npcFindMemory(Mem, ShopI, Noon) != nullptr);
	N->SetMemoryTime(1, 18.0);
	Expect(TEXT("d1 18:00"), Fixer, 0, EFTNpcLineKind::Remind, TEXT("fixer_remind_meet"));
	N->SetMemoryTime(1, 19.5);
	N->OnMeet(Fixer, 0);
	Expect(TEXT("d1 19:30"), Fixer, 0, EFTNpcLineKind::Greet, TEXT("fixer_greet_kept"));
	Check(TEXT("d1 19:30 the kept meeting is thanked once"), N->PickLine(Fixer, 0, EFTNpcLineKind::Greet) != TEXT("fixer_greet_kept"));
	N->SetMemoryTime(1, 22.0);
	N->OnGate(Shop, 1, 9, false);
	Expect(TEXT("d1 22:00"), Shop, 0, EFTNpcLineKind::Greet, TEXT("shop_greet_gate_broken"));

	N->SetMemoryTime(2, 0.0);
	FString Error;
	const bool bLoaded = N->ApplySaveJson(N->CaptureSaveJson(), Error);
	UE_LOG(LogForgottenThird, Display, TEXT("NPCSLICE d2 00:00 saved and loaded: %s %s"), bLoaded ? TEXT("ok") : TEXT("FAILED"), *Error);
	Pass += bLoaded ? 1 : 0;
	++Total;

	N->SetMemoryTime(2, 2.0);
	const uint32 Late = static_cast<uint32>(N->OnVanKnockdown(Street, nullptr, 1));
	N->SetMemoryTime(2, 3.5);
	Check(TEXT("d2 03:30 the night knock-down reached nobody"), Late != 0 && !ftcore::npcFindMemory(Mem, ShopI, Late) &&
		!ftcore::npcFindMemory(Mem, KidI, Late));
	Expect(TEXT("d2 03:30"), Shop, 0, EFTNpcLineKind::Greet, TEXT("shop_greet_gate_broken"));
	Expect(TEXT("d2 03:30"), Kid, 2, EFTNpcLineKind::Greet, TEXT("kid_greet_gift_kept"));
	N->SetMemoryTime(2, 9.0);
	N->OnGate(Shop, 1, 9, true);
	Expect(TEXT("d2 09:00"), Shop, 0, EFTNpcLineKind::Greet, TEXT("shop_greet_gate_mended"));
	Expect(TEXT("d2 09:00"), Kid, 2, EFTNpcLineKind::Greet, TEXT("kid_greet_gift_kept"));
	N->WitnessRadius = Radius;
	N->HoldClock(false);
	UE_LOG(LogForgottenThird, Display, TEXT("NPCSLICE done %d/%d"), Pass, Total);
}
