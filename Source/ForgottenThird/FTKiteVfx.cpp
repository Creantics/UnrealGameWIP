#include "FTKiteVfx.h"

#include "Components/SkeletalMeshComponent.h"
#include "FTArenaFighter.h"
#include "FTKiteHost.h"
#include "FTText.h"
#include "FTVfxComponent.h"
#include "ForgottenThird.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "NiagaraSystem.h"

#include "ftcore/Json.h"

#include <string_view>

namespace
{
// The json's key for each effect (effects.<key>), and its default system. The order is FFTKiteVfx::EEffect's.
constexpr const char* kEffectKeys[] = {"ring", "open", "crit", "hit", "hitFloor", "impactHeavy", "impactLight", "pegTell", "tailTell",
	"sheetTell", "ropeTell"};
constexpr const TCHAR* kEffectDefaults[] = {TEXT("kite_swoop_tell"), TEXT("kite_seam_open"), TEXT("kite_seam_crit"), TEXT("kite_hit"),
	TEXT("kite_hit_floor"), TEXT("kite_impact_heavy"), TEXT("kite_impact_light"), TEXT("kite_peg_tell"), TEXT("kite_snag_tell"),
	TEXT("kite_sheet_tell"), TEXT("kite_undertow_tell")};
constexpr const char* kMittenKey = "mitten";
constexpr const TCHAR* kMittenDefault = TEXT("kite_mitten_tell");

float ReadFloat(const ftcore::JsonValue& V, const char* Key, float Fallback)
{
	return static_cast<float>(V[Key].number(static_cast<double>(Fallback)));
}

FName ReadName(const ftcore::JsonValue& V)
{
	const std::string_view Text = V.string();
	return Text.empty() ? NAME_None : FName(*FT::ToFString(Text));
}

void ReadLook(const ftcore::JsonValue& V, FName& Effect, FName& Bone, float& SpeedScale, float& ShapeScale)
{
	const FName Name = ReadName(V["effect"]);
	if (!Name.IsNone())
	{
		Effect = Name;
	}
	const FName At = ReadName(V["bone"]);
	if (!At.IsNone())
	{
		Bone = At;
	}
	SpeedScale = ReadFloat(V, "speedScale", SpeedScale);
	ShapeScale = ReadFloat(V, "shapeScale", ShapeScale);
}

// The effect a json key names (effects.<key>), or -1.
int32 EffectOfKey(std::string_view Key)
{
	for (int32 E = 0; E < static_cast<int32>(UE_ARRAY_COUNT(kEffectKeys)); ++E)
	{
		if (Key == kEffectKeys[E])
		{
			return E;
		}
	}
	return -1;
}
} // namespace

void FFTKiteVfx::ReadTuning()
{
	static_assert(UE_ARRAY_COUNT(kEffectKeys) == EffectCount && UE_ARRAY_COUNT(kEffectDefaults) == EffectCount, "one key and default per effect");
	for (int32 E = 0; E < EffectCount; ++E)
	{
		Looks[E] = FLook();
		Looks[E].Effect = FName(kEffectDefaults[E]);
	}
	MittenLook = FLook();
	MittenLook.Effect = FName(kMittenDefault);
	MoveCount = MittenCount = HitBoneCount = 0;
	ShowDownGlowTwo = 0.0;
	TArray<uint8> Bytes;
	const FString Path = FPaths::ProjectDir() / DataPath;
	if (!IFileManager::Get().FileExists(*Path) || !FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: kite fx: couldn't read %s; the effects use their defaults and no move has a tell."), DataPath);
		return;
	}
	ftcore::JsonDocument Doc;
	std::string Error;
	if (!Doc.parse(FT::BytesAsText(Bytes), &Error))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: kite fx: %s refused (%s); the effects use their defaults and no move has a tell."), DataPath,
			*FT::ToFString(Error));
		return;
	}
	const ftcore::JsonValue Root = Doc.root();
	const ftcore::JsonValue Effects = Root["effects"];
	for (int32 E = 0; E < EffectCount; ++E)
	{
		FLook& Look = Looks[E];
		ReadLook(Effects[kEffectKeys[E]], Look.Effect, Look.Bone, Look.SpeedScale, Look.ShapeScale);
	}
	ReadLook(Effects[kMittenKey], MittenLook.Effect, MittenLook.Bone, MittenLook.SpeedScale, MittenLook.ShapeScale);
	const FName Seam = ReadName(Root["socket"]);
	if (!Seam.IsNone())
	{
		Socket = Seam;
	}
	// The glow and the burst ride the seam unless the json puts them elsewhere; the hit picks its bone per blow, and the floor
	// effects are put where they play.
	for (const EEffect E : {Open, Crit})
	{
		if (Looks[E].Bone.IsNone())
		{
			Looks[E].Bone = Socket;
		}
	}
	for (const EEffect E : {Ring, Hit, HitFloor, ImpactHeavy, ImpactLight})
	{
		Looks[E].Bone = NAME_None;
	}
	const FName Lone = ReadName(Root["loneMove"]);
	if (!Lone.IsNone())
	{
		LoneMove = Lone;
	}
	Lift = FMath::Max(ReadFloat(Root, "lift", Lift), 0.f);
	ShowDownGlowTwo = FMath::Max(Root["showcase"]["downGlowTwo"].number(0.0), 0.0);

	const ftcore::JsonValue Mittens = Root["mittenBones"];
	for (uint32 I = 0; I < Mittens.size() && MittenCount < MaxMittens; ++I)
	{
		const FName Bone = ReadName(Mittens[I]);
		if (!Bone.IsNone())
		{
			MittenBones[MittenCount++] = Bone;
		}
	}
	const ftcore::JsonValue HitAt = Root["hitBones"];
	for (uint32 I = 0; I < HitAt.size() && HitBoneCount < MaxHitBones; ++I)
	{
		const FName Bone = ReadName(HitAt[I]);
		if (!Bone.IsNone())
		{
			HitBones[HitBoneCount++] = Bone;
		}
	}
	// moves: { "<clip>": { "ring": true, "tell": "<effect key>", "mittens": [bones], "impact": "<effect key>", "contact": [bones] } }
	const ftcore::JsonValue MoveRows = Root["moves"];
	for (uint32 I = 0; I < MoveRows.size() && MoveCount < MaxMoves; ++I)
	{
		const std::string_view Clip = MoveRows.key(I);
		const ftcore::JsonValue V = MoveRows.value(I);
		if (Clip.empty() || !V.isObject())
		{
			continue;
		}
		if (Clip.size() > static_cast<size_t>(FMoveFx::MaxClip))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: kite fx: move %s's clip id is over %d characters; it is left out."), *FT::ToFString(Clip),
				FMoveFx::MaxClip);
			continue;
		}
		FMoveFx& Row = Moves[MoveCount++];
		Row = FMoveFx();
		FMemory::Memcpy(Row.Clip, Clip.data(), Clip.size());
		Row.ClipLen = static_cast<int32>(Clip.size());
		const FString RowName = FT::ToFString(Clip);
		Row.bRing = V["ring"].boolean(false);
		if (V.has("tell"))
		{
			Row.Tell = EffectOfKey(V["tell"].string());
			if (Row.Tell < PegTell)
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("Arena: kite fx: move %s's tell %s is not a bone tell (pegTell, tailTell, sheetTell, ropeTell); it has none."),
					*RowName, *FT::ToFString(V["tell"].string()));
				Row.Tell = -1;
			}
		}
		const ftcore::JsonValue Hands = V["mittens"];
		for (uint32 H = 0; H < Hands.size(); ++H)
		{
			const FName Bone = ReadName(Hands[H]);
			for (int32 M = 0; M < MittenCount; ++M)
			{
				if (MittenBones[M] == Bone)
				{
					Row.Mittens |= 1u << M;
				}
			}
		}
		if (V.has("impact"))
		{
			Row.Impact = EffectOfKey(V["impact"].string());
			if (Row.Impact != ImpactHeavy && Row.Impact != ImpactLight)
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("Arena: kite fx: move %s's impact %s is not impactHeavy or impactLight; it has none."),
					*RowName, *FT::ToFString(V["impact"].string()));
				Row.Impact = -1;
			}
		}
		const ftcore::JsonValue Contact = V["contact"];
		for (uint32 C = 0; C < Contact.size() && Row.ContactCount < MaxContacts; ++C)
		{
			const FName Bone = ReadName(Contact[C]);
			if (!Bone.IsNone())
			{
				Row.Contact[Row.ContactCount++] = Bone;
			}
		}
	}
	LoneRow = -1;
	const FString LoneText = LoneMove.ToString();
	for (int32 I = 0; I < MoveCount && LoneRow < 0; ++I)
	{
		LoneRow = LoneText.Equals(FT::ToFString(Moves[I].ClipId()), ESearchCase::CaseSensitive) ? I : -1;
	}
	UE_LOG(LogForgottenThird, Log, TEXT("Arena: kite fx: %d moves with tells or impacts, %d mittens, %d hit bones; the lone takes play %s's%s."), MoveCount,
		MittenCount, HitBoneCount, *LoneText, LoneRow < 0 ? TEXT(" (it has no row: none)") : TEXT(""));
}

void FFTKiteVfx::Spawn(AFTArenaFighter* Figure)
{
	if (bSpawned || !Figure)
	{
		return;
	}
	// Read even when off: the showcase's reel takes its later go-down from here, so an OFF and an ON capture match.
	ReadTuning();
	// The OFF half of an OFF/ON capture: the fight runs the same, nothing plays.
	if (FParse::Param(FCommandLine::Get(), TEXT("ArenaNoKiteFx")))
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: kite fx off (-ArenaNoKiteFx)."));
		return;
	}
	bSpawned = true;
	FigureRef = Figure;
	for (int32 E = 0; E < EffectCount; ++E)
	{
		const FLook& Look = Looks[E];
		Fx[E] = Make(Figure, Look.Effect, Look.SpeedScale, Look.ShapeScale, Look.Bone);
		if (UFTVfxComponent* Vfx = Fx[E].Get())
		{
			// What rides a bone keeps its own size as the bone scales (the seam opens in face_down); what plays on the floor
			// is put there each time. All but the seam's keep the world's up, so their drips and scraps fall down and their
			// rings lie flat whatever the bone's turn.
			Vfx->SetUsingAbsoluteScale(true);
			if (E != Open && E != Crit)
			{
				Vfx->SetUsingAbsoluteRotation(true);
			}
			if (Look.Bone.IsNone() && E != Hit)
			{
				Vfx->SetUsingAbsoluteLocation(true);
			}
		}
	}
	for (int32 M = 0; M < MittenCount; ++M)
	{
		MittenFx[M] = Make(Figure, MittenLook.Effect, MittenLook.SpeedScale, MittenLook.ShapeScale, MittenBones[M]);
		if (UFTVfxComponent* Vfx = MittenFx[M].Get())
		{
			Vfx->SetUsingAbsoluteScale(true);
			Vfx->SetUsingAbsoluteRotation(true);
		}
	}
	bTelling = bRingRunning = bWasOpen = false;
	TellRunning = -1;
	MittensRunning = 0;
	TellUntil = RingUntil = ImpactAt = 0.0;
	Pending = nullptr;
	CritsSeen = 0;
}

UFTVfxComponent* FFTKiteVfx::Make(AFTArenaFighter* Figure, FName Effect, float SpeedScale, float ShapeScale, FName Bone)
{
	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *UFTVfxComponent::SystemPath(Effect), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!System)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("Arena: kite fx: no %s; that effect is off (run FTBuildVfx -Only=%s)."),
			*UFTVfxComponent::SystemPath(Effect), *Effect.ToString());
		return nullptr;
	}
	UFTVfxComponent* Vfx = NewObject<UFTVfxComponent>(Figure);
	Vfx->Effect = Effect;
	Vfx->System = System;
	Vfx->SpeedScale = SpeedScale;
	Vfx->ShapeScale = ShapeScale;
	Vfx->bPlayOnBeginPlay = false;
	USkeletalMeshComponent* Skin = Figure->GetSkin();
	if (Skin)
	{
		if (!Bone.IsNone() && !Skin->DoesSocketExist(Bone))
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: kite fx %s: the mesh has no bone or socket %s; it plays at the mesh's origin."),
				*Effect.ToString(), *Bone.ToString());
		}
		Vfx->SetupAttachment(Skin, Bone);
	}
	else
	{
		Vfx->SetupAttachment(Figure->GetRootComponent());
	}
	Vfx->RegisterComponent();
	return Vfx;
}

const FFTKiteVfx::FMoveFx* FFTKiteVfx::FindMove(std::string_view Clip) const
{
	if (Clip.empty())
	{
		return nullptr;
	}
	for (int32 I = 0; I < MoveCount; ++I)
	{
		if (Moves[I].ClipId() == Clip)
		{
			return &Moves[I];
		}
	}
	return nullptr;
}

void FFTKiteVfx::Started(FName Effect, double T)
{
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: kite fx %s at %.2f"), *Effect.ToString(), T);
}

void FFTKiteVfx::Play(EEffect Effect, double T)
{
	UFTVfxComponent* Vfx = Fx[Effect].Get();
	if (!Vfx)
	{
		return;
	}
	Vfx->Play();
	Started(Looks[Effect].Effect, T);
}

void FFTKiteVfx::StopOne(EEffect Effect)
{
	if (UFTVfxComponent* Vfx = Fx[Effect].Get())
	{
		Vfx->Stop();
	}
}

void FFTKiteVfx::CutOne(EEffect Effect)
{
	if (UFTVfxComponent* Vfx = Fx[Effect].Get())
	{
		Vfx->StopNow();
	}
}

void FFTKiteVfx::FadeOne(EEffect Effect, double T)
{
	if (UFTVfxComponent* Vfx = Fx[Effect].Get())
	{
		Vfx->StopWithin(GlowFadeSeconds);
		if (T >= 0.0)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: kite fx %s fades at %.2f"), *Looks[Effect].Effect.ToString(), T);
		}
	}
}

void FFTKiteVfx::StartTells(const FMoveFx& Row, const FVector& Ground, double T)
{
	// A tell still running from the move before ends where this one starts.
	StopTells(T, false);
	const FVector Floor(Ground.X, Ground.Y, Ground.Z + Lift * 100.f);
	if (Row.bRing && Fx[Ring].IsValid())
	{
		Fx[Ring]->SetWorldLocation(Floor);
		Play(Ring, T);
		bRingRunning = true;
	}
	if (Row.Tell >= 0 && Fx[Row.Tell].IsValid())
	{
		// A bone tell rides its bone; one without a bone plays on the floor under the kite.
		if (Looks[Row.Tell].Bone.IsNone())
		{
			Fx[Row.Tell]->SetWorldLocation(Floor);
		}
		Play(static_cast<EEffect>(Row.Tell), T);
		TellRunning = Row.Tell;
	}
	bool bAnyMitten = false;
	for (int32 M = 0; M < MittenCount; ++M)
	{
		if ((Row.Mittens & (1u << M)) == 0)
		{
			continue;
		}
		if (UFTVfxComponent* Vfx = MittenFx[M].Get())
		{
			Vfx->Play();
			MittensRunning |= 1u << M;
			bAnyMitten = true;
		}
	}
	if (bAnyMitten)
	{
		Started(MittenLook.Effect, T);
	}
	bTelling = bRingRunning || TellRunning >= 0 || MittensRunning != 0;
}

void FFTKiteVfx::StopBoneTells(double T, bool bEarly)
{
	if (TellRunning >= 0)
	{
		StopOne(static_cast<EEffect>(TellRunning));
		if (bEarly && T >= 0.0)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: kite fx %s stops at %.2f"), *Looks[TellRunning].Effect.ToString(), T);
		}
		TellRunning = -1;
	}
	if (MittensRunning != 0)
	{
		for (int32 M = 0; M < MittenCount; ++M)
		{
			if ((MittensRunning & (1u << M)) != 0)
			{
				if (UFTVfxComponent* Vfx = MittenFx[M].Get())
				{
					Vfx->Stop();
				}
			}
		}
		if (bEarly && T >= 0.0)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("Arena: kite fx %s stops at %.2f"), *MittenLook.Effect.ToString(), T);
		}
		MittensRunning = 0;
	}
	bTelling = bRingRunning;
}

void FFTKiteVfx::StopRing(double T, bool bEarly)
{
	if (!bRingRunning)
	{
		return;
	}
	// No new particles; the live ones fade out over their own lives (a cut in one frame would pop: RUBRIC S-65 notes).
	StopOne(Ring);
	if (bEarly && T >= 0.0)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: kite fx %s stops at %.2f"), *Looks[Ring].Effect.ToString(), T);
	}
	bRingRunning = false;
	bTelling = TellRunning >= 0 || MittensRunning != 0;
}

void FFTKiteVfx::StopTells(double T, bool bEarly)
{
	StopRing(T, bEarly);
	StopBoneTells(T, bEarly);
}

void FFTKiteVfx::PlayImpact(const FMoveFx& Row, const FVector& Ground, double T)
{
	if (Row.Impact < 0 || !Fx[Row.Impact].IsValid())
	{
		return;
	}
	// Under the lowest of the move's contact bones: the part that came down.
	FVector At(Ground.X, Ground.Y, Ground.Z);
	bool bFound = false;
	double Lowest = 0.0;
	if (const AFTArenaFighter* Figure = FigureRef.Get())
	{
		for (int32 C = 0; C < Row.ContactCount; ++C)
		{
			FVector P;
			if (Figure->GetSkinPoint(Row.Contact[C], P) && (!bFound || P.Z < Lowest))
			{
				bFound = true;
				Lowest = P.Z;
				At.X = P.X;
				At.Y = P.Y;
			}
		}
	}
	Fx[Row.Impact]->SetWorldLocation(FVector(At.X, At.Y, Ground.Z + Lift * 100.f));
	Play(static_cast<EEffect>(Row.Impact), T);
}

void FFTKiteVfx::PlayHit(const FVector& At, double FloorZ, double T)
{
	UFTVfxComponent* Puff = Fx[Hit].Get();
	const AFTArenaFighter* Figure = FigureRef.Get();
	USkeletalMeshComponent* Skin = Figure ? Figure->GetSkin() : nullptr;
	// The hit bone nearest where the blow lands (the tail's end, a sheet's edge, a string's end).
	int32 Best = -1;
	FVector BestAt = At;
	double BestDist = 0.0;
	if (Figure)
	{
		for (int32 B = 0; B < HitBoneCount; ++B)
		{
			FVector P;
			if (!Figure->GetSkinPoint(HitBones[B], P))
			{
				continue;
			}
			const double D = FVector::DistSquared(P, At);
			if (Best < 0 || D < BestDist)
			{
				Best = B;
				BestDist = D;
				BestAt = P;
			}
		}
	}
	if (Puff)
	{
		if (Best >= 0 && Skin)
		{
			Puff->AttachToComponent(Skin, FAttachmentTransformRules::SnapToTargetNotIncludingScale, HitBones[Best]);
		}
		else
		{
			Puff->SetWorldLocation(At);
		}
		Play(Hit, T);
	}
	if (Fx[HitFloor].IsValid())
	{
		Fx[HitFloor]->SetWorldLocation(FVector(BestAt.X, BestAt.Y, FloorZ));
		Play(HitFloor, T);
	}
}

void FFTKiteVfx::Feed(const ftcore::ArenaEvents& Events, const FFTKiteHost& Host, const ftcore::ArenaFight& Fight, const FVector& KiteGround,
	const FVector& LeadChest)
{
	if (!bSpawned || !Host.IsActive())
	{
		return;
	}
	const double T = static_cast<double>(Fight.time());
	const size_t Slot = Host.GetSlot();
	const ftcore::ArenaEnemyKind& Kind = Fight.def().enemies[Slot];
	// The move reached its contact frame: the impact, on the floor under what came down.
	if (Pending && T >= ImpactAt)
	{
		PlayImpact(*Pending, KiteGround, T);
		Pending = nullptr;
	}
	for (const ftcore::ArenaEvent& E : Events)
	{
		if (E.type == ftcore::ArenaEventType::AttackStarted && E.source == Slot && E.move < Kind.moveCount)
		{
			const ftcore::ArenaEnemyMove& Move = Kind.moves[E.move];
			const FMoveFx* Row = FindMove(Move.clip());
			if (!Row)
			{
				continue;
			}
			StartTells(*Row, KiteGround, T);
			TellUntil = T + static_cast<double>(Move.attack.startup);
			RingUntil = T + static_cast<double>(Move.attack.startup + Move.attack.active);
			Pending = Row->Impact >= 0 ? Row : nullptr;
			ImpactAt = TellUntil;
		}
		else if ((E.type == ftcore::ArenaEventType::Hit || E.type == ftcore::ArenaEventType::CounterStagger
					 || E.type == ftcore::ArenaEventType::ReadCounter)
			&& E.source == ftcore::kArenaPlayer && E.target == Slot)
		{
			// A lead blow (its own, or the partner's paired or Read counter). The blow hook counted a crit for it, or it
			// landed plain: the burst at the seam, or the puff on the body where it struck.
			const int32 Crits = Host.GetCrits();
			const bool bCrit = Crits != CritsSeen;
			CritsSeen = Crits;
			if (bCrit)
			{
				Play(Crit, T);
			}
			else
			{
				FVector Toward = LeadChest - KiteGround;
				Toward.Z = 0.0;
				const double Reach = FMath::Min(static_cast<double>(Kind.bodyRadius) * 100.0, Toward.Size());
				const FVector Edge = KiteGround + Toward.GetSafeNormal() * Reach;
				PlayHit(FVector(Edge.X, Edge.Y, LeadChest.Z), KiteGround.Z, T);
			}
		}
		else if (((E.type == ftcore::ArenaEventType::Staggered || E.type == ftcore::ArenaEventType::Downed) && E.target == Slot)
			|| (E.type == ftcore::ArenaEventType::Parried && E.source == Slot))
		{
			// The move is broken off (the parry, a stagger, the kill): its tells stop and fade out, and a contact still to come
			// never comes.
			if (bTelling)
			{
				StopTells(T, true);
			}
			Pending = nullptr;
		}
	}
	if (TellRunning >= 0 || MittensRunning != 0)
	{
		if (T >= TellUntil)
		{
			StopBoneTells(T, false);
		}
	}
	if (bRingRunning && T >= RingUntil)
	{
		StopRing(T, false);
	}
	// The fight is over, or the kite is down: nobody steps the fight again, so the window's own end never reaches the glow
	// (Host.IsOpen is a time, still true then). It ends here, in the step that ended the fight.
	if (Fight.over() || !Fight.enemyAlive(Slot))
	{
		EndOfFight(T);
		return;
	}
	// The window opens on a parry or a stagger and shuts itself: the glow follows it.
	const bool bOpen = Host.IsOpen();
	if (bOpen && !bWasOpen)
	{
		if (bTelling)
		{
			StopTells(T, true);
		}
		Play(Open, T);
	}
	else if (!bOpen && bWasOpen)
	{
		StopOne(Open);
	}
	bWasOpen = bOpen;
}

void FFTKiteVfx::EndOfFight(double T)
{
	if (!bSpawned)
	{
		return;
	}
	// A no-op once done: Feed's last step and the runner's Finish both come here.
	if (bTelling)
	{
		StopTells(T, true);
	}
	Pending = nullptr;
	if (bWasOpen)
	{
		FadeOne(Open, T);
		bWasOpen = false;
	}
}

void FFTKiteVfx::ShowCue(const ftcore::ShowCueAt& Cue, std::string_view Move, const FVector& Ground, const FVector& HitAt)
{
	if (!bSpawned)
	{
		return;
	}
	const double T = Cue.at;
	// The lone takes (a tell ahead and its dust) play the lone move's.
	const FMoveFx* Row = !Move.empty() ? FindMove(Move) : LoneRow >= 0 ? &Moves[LoneRow] : nullptr;
	switch (Cue.cue)
	{
	case ftcore::ShowCue::Tell:
		if (Row)
		{
			StartTells(*Row, Ground, T);
		}
		return;
	case ftcore::ShowCue::TellStop:
		// At an attack's contact the tells end as in a fight; the lone take's stop is the swoop's cut (the parry's): no new
		// particles, the live ones fade out.
		if (bTelling)
		{
			StopTells(T, Cue.place == ftcore::ShowPlace::Ahead);
		}
		return;
	case ftcore::ShowCue::Impact:
		if (Row)
		{
			PlayImpact(*Row, Ground, T);
		}
		return;
	case ftcore::ShowCue::Chips:
	case ftcore::ShowCue::ChipsHeavy: // the kite has one hit, for a light or a heavy blow
		PlayHit(HitAt, Ground.Z, T);
		return;
	case ftcore::ShowCue::GlowOn:
		Play(Open, T);
		bWasOpen = true;
		return;
	case ftcore::ShowCue::GlowOff:
		// Finish's EndOfFight finds nothing left to do.
		if (bWasOpen)
		{
			FadeOne(Open, T);
			bWasOpen = false;
		}
		return;
	case ftcore::ShowCue::Crit:
		Play(Crit, T);
		return;
	}
}

void FFTKiteVfx::Stop()
{
	for (int32 E = 0; E < EffectCount; ++E)
	{
		CutOne(static_cast<EEffect>(E));
	}
	for (int32 M = 0; M < MittenCount; ++M)
	{
		if (UFTVfxComponent* Vfx = MittenFx[M].Get())
		{
			Vfx->StopNow();
		}
	}
	bTelling = bRingRunning = bWasOpen = false;
	TellRunning = -1;
	MittensRunning = 0;
	TellUntil = RingUntil = ImpactAt = 0.0;
	Pending = nullptr;
	// FFTKiteHost::Begin counts crits from 0 again.
	CritsSeen = 0;
}

void FFTKiteVfx::Reset()
{
	Stop();
	for (TWeakObjectPtr<UFTVfxComponent>& Vfx : Fx)
	{
		Vfx.Reset();
	}
	for (TWeakObjectPtr<UFTVfxComponent>& Vfx : MittenFx)
	{
		Vfx.Reset();
	}
	FigureRef.Reset();
	bSpawned = false;
}
