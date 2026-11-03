#include "FTStoneBearerHost.h"

#include <cmath>

namespace
{
// A boss blow's shape (ArenaBoss.h's wiring notes): a lane this wide for path impacts and cover moves.
constexpr float kStoneLaneWidth = 1.f;
// The arc of intercept and plain moves (arcCos), and of every move without a template of its own.
constexpr float kStoneArcCos = 0.5f;
// A chained move's startup never falls below this (s): a tile move's active part is longer than the beat to its
// next impact, so that impact lands this late instead.
constexpr float kStoneMinStartup = 0.05f;
// A move's pick range reaches this far past its reach (m), as arena.json's moves do.
constexpr float kStonePickSlack = 0.4f;
// The autopilot: an Intercept is held from the middle of the window to just past the impact; the Load is hoarded
// from this far above the pedestal's hp share; a Release goes this close to the guest's call (s).
constexpr double kStoneHoardShare = 0.2;
constexpr double kStonePressEarly = 0.025;
constexpr double kStoneHoldPast = 0.05;
constexpr double kStoneReleaseSlack = 0.05;
// A guard that blocks a guard-breaking blow loses this much more than the lead has (ArenaAttack has no guard-break
// time of its own: an empty stamina breaks the guard for the kit's guardBreakTime).
constexpr float kStoneGuardBreakExtra = 1.f;

float Dist(ftcore::Vec2 A, ftcore::Vec2 B)
{
	const float X = A.x - B.x;
	const float Y = A.y - B.y;
	return std::sqrt(X * X + Y * Y);
}

ftcore::Vec2 Along(ftcore::Vec2 P, ftcore::Vec2 Dir, float Length)
{
	return ftcore::Vec2{P.x + Dir.x * Length, P.y + Dir.y * Length};
}

// The id of the clip a boss move plays: its own, less a "_twice" or "_halves" ending (the variants play the base
// move's clip).
std::string_view ClipOf(std::string_view Id)
{
	for (const std::string_view Tail : {std::string_view("_twice"), std::string_view("_halves")})
	{
		if (Id.size() > Tail.size() && Id.substr(Id.size() - Tail.size()) == Tail)
		{
			return Id.substr(0, Id.size() - Tail.size());
		}
	}
	return Id;
}

bool LaneMove(const ftcore::ArenaBossMove& Move, std::size_t Impact)
{
	return Move.answer == ftcore::ArenaBossAnswer::Cover
		|| (Move.intercept() && Impact < Move.impactCount && Move.zones[Impact] == ftcore::CalledZoneRule::Path);
}
} // namespace

bool FFTStoneBearerHost::Load(std::string_view Json, std::string* Error, std::vector<std::string>* Warnings)
{
	bLoaded = ftcore::importStoneBearer(Json, Def, Error, Warnings);
	if (bLoaded)
	{
		// The arena moves BuildKind lays out: each boss move's impacts in order.
		std::size_t Next = 0;
		for (std::uint8_t M = 0; M < Def.boss.moveCount; ++M)
		{
			for (std::uint8_t K = 0; K < Def.boss.moves[M].impactCount && Next < ftcore::kMaxArenaMoves; ++K, ++Next)
			{
				MoveOf[Next] = M;
				ImpactOf[Next] = K;
			}
		}
	}
	return bLoaded;
}

bool FFTStoneBearerHost::BuildKind(ftcore::ArenaEnemyKind& Kind, const ftcore::ArenaFighter& Lead, std::string* Error) const
{
	if (!bLoaded)
	{
		return false;
	}
	std::size_t Count = 0;
	for (std::uint8_t M = 0; M < Def.boss.moveCount; ++M)
	{
		Count += Def.boss.moves[M].impactCount;
	}
	if (Count > ftcore::kMaxArenaMoves)
	{
		if (Error)
		{
			*Error = "stone_bearer: " + std::to_string(Count) + " impacts in all, more than kMaxArenaMoves";
		}
		return false;
	}
	ftcore::ArenaEnemyKind Out = Kind;
	Out.hp = Def.boss.hp;
	Out.poise = Def.boss.poise;
	Out.staggerTime = Def.boss.staggerTime;
	Out.openTime = Def.boss.openTime;
	const double Beat = Def.boss.beat();
	std::size_t Next = 0;
	for (std::uint8_t M = 0; M < Def.boss.moveCount; ++M)
	{
		const ftcore::ArenaBossMove& Src = Def.boss.moves[M];
		const std::string_view Clip = ClipOf(Src.id());
		// The arena move of the same clip tunes the pick and the look; else the kind's first move does.
		const ftcore::ArenaEnemyMove* Template = nullptr;
		for (std::uint8_t T = 0; T < Kind.moveCount && !Template; ++T)
		{
			if (Kind.moves[T].clip() == Clip)
			{
				Template = &Kind.moves[T];
			}
		}
		const bool bOwnTemplate = Template != nullptr;
		if (!Template)
		{
			Template = &Kind.moves[0];
		}
		for (std::uint8_t K = 0; K < Src.impactCount; ++K, ++Next)
		{
			ftcore::ArenaEnemyMove& Move = Out.moves[Next];
			Move = *Template;
			ftcore::ArenaAttack& A = Move.attack;
			const double Gap = K == 0 ? Src.impacts[0] : Src.impacts[K] - Src.impacts[K - 1];
			// The first impact lands impacts[0] beats in; each next one the gap later, less the active part before it
			// (a chained move starts when the one before it ends).
			const float Startup = static_cast<float>(Gap * Beat) - (K == 0 ? 0.f : Src.active);
			A.startup = Startup > kStoneMinStartup ? Startup : kStoneMinStartup;
			A.active = Src.active;
			A.recovery = K + 1 == Src.impactCount ? Src.recovery : 0.f;
			// The boss file's reach is from the boss's body; ArenaFight's from its centre.
			A.range = Src.range + Kind.bodyRadius;
			A.damage = static_cast<int>(std::lround(static_cast<double>(Src.damage) * Lead.hp));
			A.width = 0.f;
			if (Src.answer == ftcore::ArenaBossAnswer::Tile)
			{
				A.arcCos = -1.f;
			}
			else if (LaneMove(Src, K))
			{
				A.width = kStoneLaneWidth;
			}
			else if (Src.intercept() || Src.answer == ftcore::ArenaBossAnswer::Any || !bOwnTemplate)
			{
				A.arcCos = kStoneArcCos;
			}
			if (Src.guardBreak > 0.f)
			{
				A.guardDamage = Lead.stamina + kStoneGuardBreakExtra;
			}
			Move.minRange = 0.f;
			Move.maxRange = A.range + kStonePickSlack;
			Move.chain = K + 1 < Src.impactCount ? static_cast<std::int8_t>(Next + 1) : -1;
			Move.weight = K == 0 ? Template->weight : 0.f;
			Move.cooldown = K == 0 ? Template->cooldown : 0.f;
			// The run answers an intercept move (ArenaFight only lands it); a dodge, jump, tile or cover move is not
			// one a guard stops.
			Move.parryable = Template->parryable && Src.answer == ftcore::ArenaBossAnswer::Any;
			Move.unblockable = Src.answer == ftcore::ArenaBossAnswer::Dodge || Src.answer == ftcore::ArenaBossAnswer::Jump
				|| Src.answer == ftcore::ArenaBossAnswer::Tile || Src.answer == ftcore::ArenaBossAnswer::Cover;
			Move.clipText = {};
			for (std::size_t C = 0; C < Clip.size() && C < ftcore::kMaxArenaClipId; ++C)
			{
				Move.clipText[C] = Clip[C];
			}
		}
	}
	Out.moveCount = static_cast<std::uint8_t>(Count);
	Kind = Out;
	return true;
}

void FFTStoneBearerHost::Begin(ftcore::ArenaFight& Fight, std::size_t InSlot, ftcore::Vec2 Centre, float YawDegrees)
{
	if (!bLoaded || InSlot >= Fight.enemyCount())
	{
		End();
		return;
	}
	FightPtr = &Fight;
	Slot = InSlot;
	Plinth = Centre;
	KeepOut = ftcore::arenaKeepOutAt(Centre, YawDegrees, Def.boss.keepOutBearing, Def.boss.keepOutDistance, Def.boss.keepOutRadius);
	LeadHp = Fight.def().fighter.hp;
	UseMove = ftcore::kArenaBossNoMove;
	bUseRefused = false;
	UseDealt = 0;
	RefusedUntil = -1.0e300;
	LastPart = ftcore::ArenaPart::None;
	bCritHold = false;
	bPlinthPending = false;
	bInterceptWas = bGuardWas = false;
	LoadBefore = 0;
	NoteTotal = 0;
	Refusals = 0;
	Intercepts = 0;
	Boss.begin(Def, Fight.time());
	// The boss can't be taken past its pedestal: one big blow stops at the pedestal's share (which starts it), and the
	// floor stays (above 0) until the pedestal is lifted.
	const int Floor = static_cast<int>(std::floor(static_cast<double>(Def.pedestal.at) * Fight.def().enemies[Slot].hp));
	Fight.setEnemyHpFloor(Slot, Floor > 1 ? Floor : 1);
	Fight.setBlowHook(&FFTStoneBearerHost::RuleBlow, this);
}

void FFTStoneBearerHost::End()
{
	if (FightPtr)
	{
		FightPtr->setBlowHook(nullptr, nullptr);
	}
	FightPtr = nullptr;
}

float FFTStoneBearerHost::HpShare() const
{
	const ftcore::ArenaEnemyKind& Kind = FightPtr->def().enemies[Slot];
	return Kind.hp > 0 ? static_cast<float>(FightPtr->enemyHp(Slot)) / static_cast<float>(Kind.hp) : 0.f;
}

ftcore::Vec2 FFTStoneBearerHost::BossFacing() const
{
	return FightPtr->output().enemies[Slot].facing;
}

ftcore::Vec2 FFTStoneBearerHost::LaneEnd(ftcore::Vec2 Pos, ftcore::Vec2 Facing) const
{
	const std::uint8_t Live = Boss.run().liveMove();
	const float Range = Live < Def.boss.moveCount ? Def.boss.moves[Live].range + FightPtr->def().enemies[Slot].bodyRadius : 0.f;
	return Along(Pos, Facing, KeepOut.none() ? Range : KeepOut.clampLane(Pos, Facing, kStoneLaneWidth, Range));
}

ftcore::CalledZone FFTStoneBearerHost::ZoneNow(const ftcore::ArenaBodies& Bodies) const
{
	if (!IsActive())
	{
		return ftcore::CalledZone::Left;
	}
	const ftcore::Vec2 Pos = Bodies.enemies[Slot];
	const ftcore::Vec2 Facing = BossFacing();
	return ftcore::arenaZoneOf(Boss.run().strike().nextZone(), Pos, Facing, LaneEnd(Pos, Facing), kStoneLaneWidth, Bodies.player);
}

bool FFTStoneBearerHost::DrawnStanding() const
{
	if (!IsActive())
	{
		return false;
	}
	return Boss.invulnerable() || (FightPtr->enemyHeld(Slot) && FightPtr->time() < RefusedUntil);
}

void FFTStoneBearerHost::Assist(ftcore::ArenaInput& In, bool& bIntercept, const ftcore::ArenaBodies& Bodies) const
{
	bIntercept = false;
	if (!IsActive())
	{
		return;
	}
	const double T = FightPtr->time();
	const ftcore::ArenaBossRun& Run = Boss.run();
	const ftcore::CalledStrike& Strike = Run.strike();
	const std::uint8_t Live = Run.liveMove();
	if (Live < Def.boss.moveCount && Def.boss.moves[Live].intercept() && !Strike.complete())
	{
		const std::size_t I = Strike.nextIndex();
		const double Press = Strike.idealPress(I);
		const double Impact = Strike.impact(I).at;
		// Hold from just before the middle of the window through the impact; nothing else meanwhile (a guard would be
		// the answer instead, a dodge would move the lead out of the zone).
		bIntercept = T >= Press - kStonePressEarly && T < Impact + kStoneHoldPast;
		if (T >= Impact - Def.boss.intercept.window && T < Impact + kStoneHoldPast)
		{
			In.guard = false;
			In.dodge = false;
		}
		// Into the called zone: a step to the boss's called side, or onto the marked throw's lane.
		const ftcore::Vec2 Pos = Bodies.enemies[Slot];
		const ftcore::Vec2 Facing = BossFacing();
		const ftcore::CalledZone Want = Strike.nextZone();
		if (ZoneNow(Bodies) != Want)
		{
			if (Want == ftcore::CalledZone::Path)
			{
				const ftcore::Vec2 To{Bodies.player.x - Pos.x, Bodies.player.y - Pos.y};
				const float Ahead = To.x * Facing.x + To.y * Facing.y;
				const ftcore::Vec2 Onto = Along(Pos, Facing, Ahead > 0.f ? Ahead : 0.f);
				const ftcore::Vec2 Step{Onto.x - Bodies.player.x, Onto.y - Bodies.player.y};
				const float Length = Dist(Onto, Bodies.player);
				if (Length > 0.f)
				{
					In.move = ftcore::Vec2{Step.x / Length, Step.y / Length};
				}
			}
			else
			{
				const float Sign = Want == ftcore::CalledZone::Right ? 1.f : -1.f;
				In.move = ftcore::Vec2{-Facing.y * Sign, Facing.x * Sign};
			}
		}
	}
	const ftcore::PedestalPhase& Pedestal = Boss.pedestal();
	if (Pedestal.holding())
	{
		// On the plinth: stand still and release on the guest's call (blows do nothing now).
		In.light = In.heavy = false;
		const double Call = Pedestal.nearestCall(Run.guest(), T);
		In.release = FightPtr->load() > ftcore::kArenaReleaseOpenLoad && std::fabs(T - Call) <= kStoneReleaseSlack;
	}
	else if (!Pedestal.lifted() && HpShare() < Pedestal.tuning().at + kStoneHoardShare && !FightPtr->overloaded())
	{
		// Bring Load to the plinth.
		In.release = false;
	}
}

void FFTStoneBearerHost::Buttons(bool bInterceptDown, bool bGuardDown, const ftcore::ArenaBodies& Bodies)
{
	if (!IsActive())
	{
		return;
	}
	const double T = FightPtr->time();
	const ftcore::CalledZone Zone = ZoneNow(Bodies);
	// Releases first, so a switch from one button to the other is a release and a press.
	if (bInterceptWas && !bInterceptDown)
	{
		Absorb(Boss.interceptUp(T));
	}
	if (bGuardWas && !bGuardDown)
	{
		Absorb(Boss.guardUp(T));
	}
	if (bInterceptDown && !bInterceptWas)
	{
		Absorb(Boss.interceptDown(Zone, T));
	}
	if (bGuardDown && !bGuardWas)
	{
		Absorb(Boss.guardDown(Zone, T));
	}
	bInterceptWas = bInterceptDown;
	bGuardWas = bGuardDown;
}

bool FFTStoneBearerHost::AfterTick(const ftcore::ArenaEvents& Events, ftcore::ArenaBodies& Bodies)
{
	if (!IsActive())
	{
		return false;
	}
	ftcore::ArenaFight& Fight = *FightPtr;
	const double T = Fight.time();
	const ftcore::ArenaEnemyOut& Out = Fight.output().enemies[Slot];
	for (const ftcore::ArenaEvent& E : Events)
	{
		switch (E.type)
		{
		case ftcore::ArenaEventType::AttackStarted:
			if (E.source == Slot)
			{
				OnAttackStarted(E.move, Out.brain == ftcore::ArenaBrain::Attack && Out.move == E.move ? Out.stateTime : 0.f, Bodies);
			}
			break;
		case ftcore::ArenaEventType::Blocked:
			// Never Parried: a parried blow changes nothing.
			if (E.source == Slot && E.move < ftcore::kMaxArenaMoves)
			{
				Absorb(Boss.onBlocked(MoveOf[E.move], T));
			}
			break;
		case ftcore::ArenaEventType::Staggered:
			if (E.target == Slot)
			{
				Absorb(Boss.onStagger(T));
			}
			break;
		case ftcore::ArenaEventType::Released:
		{
			ftcore::PedestalRelease Result = ftcore::PedestalRelease::NotHolding;
			Absorb(Boss.onRelease(T, LoadBefore, &Result));
			if (Result == ftcore::PedestalRelease::OffBeat || Result == ftcore::PedestalRelease::LowLoad)
			{
				Note(FFTStoneBearerNote::EKind::ReleaseMissed, ftcore::kArenaBossNoMove, T);
				Notes[static_cast<std::size_t>(NoteTotal - 1) % MaxNotes].Release = Result;
			}
			break;
		}
		default:
			break;
		}
	}
	Absorb(Boss.update(T, HpShare()));

	// A floor blow turning active: its shock ring, when the stacks send one; a tile move: its fragments.
	const ftcore::ArenaPart Part = Out.brain == ftcore::ArenaBrain::Attack ? Out.part : ftcore::ArenaPart::None;
	if (Part == ftcore::ArenaPart::Active && LastPart != ftcore::ArenaPart::Active && Out.move < ftcore::kMaxArenaMoves && !bUseRefused)
	{
		const std::uint8_t M = MoveOf[Out.move];
		if (Boss.shockRing(M))
		{
			OnShockRing(M, T);
		}
		if (M < Def.boss.moveCount && Def.boss.moves[M].answer == ftcore::ArenaBossAnswer::Tile)
		{
			OnTileRain(M, T);
		}
	}
	LastPart = Part;

	if (bPlinthPending)
	{
		bPlinthPending = false;
		Bodies.enemies[Slot] = Plinth;
		return true;
	}
	return false;
}

void FFTStoneBearerHost::OnAttackStarted(std::uint8_t Move, float StateTime, const ftcore::ArenaBodies& Bodies)
{
	if (Move >= ftcore::kMaxArenaMoves || ImpactOf[Move] != 0)
	{
		return; // a chained impact of the use in progress
	}
	ftcore::ArenaFight& Fight = *FightPtr;
	const double Start = static_cast<double>(Fight.time()) - static_cast<double>(StateTime);
	const std::uint8_t BossMove = MoveOf[Move];
	const ftcore::Vec2 Pos = Bodies.enemies[Slot];
	bool bStarted = false;
	Absorb(Boss.startMove(BossMove, Start, ftcore::arenaSideOf(Pos, BossFacing(), Bodies.player), &bStarted));
	UseMove = BossMove;
	UseDealt = 0;
	bUseRefused = !bStarted;
	if (bUseRefused)
	{
		// Not this phase's (or the boss can't move now): cut it, and the boss picks again. A hold already running
		// (the plinth, the down time, the opening) is left as it is.
		++Refusals;
		Note(FFTStoneBearerNote::EKind::Refused, BossMove, Start);
		if (!Fight.enemyHeld(Slot))
		{
			Fight.holdEnemy(Slot, ftcore::ArenaBrain::Down, RefuseHold);
			RefusedUntil = static_cast<double>(Fight.time()) + 2.0 * static_cast<double>(RefuseHold);
		}
	}
}

void FFTStoneBearerHost::Absorb(const ftcore::ArenaBossEvents& Events)
{
	ftcore::ArenaFight& Fight = *FightPtr;
	for (const ftcore::ArenaBossEvent& E : Events)
	{
		Note(FFTStoneBearerNote::EKind::Run, E.move, E.at);
		Notes[static_cast<std::size_t>(NoteTotal - 1) % MaxNotes].Event = E;
		switch (E.type)
		{
		case ftcore::ArenaBossEventType::CritOpened:
		{
			// Kneeling and open to the weak point until `until`, instead of the move's recovery. A crit VFX or marker
			// hook goes here, on the mesh socket WeakPointSocket.
			const double Seconds = E.until - static_cast<double>(Fight.time());
			if (Seconds > 0.0)
			{
				bCritHold = Fight.holdEnemy(Slot, ftcore::ArenaBrain::Open, static_cast<float>(Seconds));
			}
			break;
		}
		case ftcore::ArenaBossEventType::CritClosed:
			if (bCritHold && Fight.enemyHeld(Slot) && Fight.output().enemies[Slot].brain == ftcore::ArenaBrain::Open)
			{
				Fight.releaseEnemy(Slot);
			}
			bCritHold = false;
			break;
		case ftcore::ArenaBossEventType::Invulnerable:
			if (E.value == 1)
			{
				// Onto the plinth: held (drawn standing) until a Release on the call lifts it.
				Fight.holdEnemy(Slot, ftcore::ArenaBrain::Down, INFINITY);
				bCritHold = false;
				bPlinthPending = true;
				// The boss offers no parries while it is held on the plinth, so a lead who comes too low to Release
				// could never lift it: they are topped up to just enough for one Release.
				const int Short = ftcore::kArenaReleaseOpenLoad + 1 - Fight.load();
				if (Short > 0)
				{
					Fight.giveLoad(Short);
				}
			}
			break;
		case ftcore::ArenaBossEventType::Lifted:
		{
			Fight.setEnemyHpFloor(Slot, 0);
			const double Seconds = E.until - static_cast<double>(Fight.time());
			if (Seconds > 0.0)
			{
				Fight.holdEnemy(Slot, ftcore::ArenaBrain::Down, static_cast<float>(Seconds));
			}
			else
			{
				Fight.releaseEnemy(Slot);
			}
			break;
		}
		case ftcore::ArenaBossEventType::Broke:
			OnBroke(E.move, E.at);
			break;
		case ftcore::ArenaBossEventType::Call:
			OnGuestCall(E);
			break;
		case ftcore::ArenaBossEventType::LoadGained:
			OnLoadGained(E);
			break;
		case ftcore::ArenaBossEventType::Answered:
			Intercepts += E.outcome == ftcore::CalledOutcome::Intercepted ? 1 : 0;
			break;
		default:
			break;
		}
	}
}

void FFTStoneBearerHost::Note(FFTStoneBearerNote::EKind Kind, std::uint8_t BossMove, double At)
{
	FFTStoneBearerNote& N = Notes[static_cast<std::size_t>(NoteTotal) % MaxNotes];
	N = FFTStoneBearerNote();
	N.Kind = Kind;
	N.Event.move = BossMove;
	N.Event.at = At;
	++NoteTotal;
}

ftcore::ArenaBlowRuling FFTStoneBearerHost::RuleBlow(void* User, const ftcore::ArenaBlow& Blow)
{
	FFTStoneBearerHost* Host = static_cast<FFTStoneBearerHost*>(User);
	if (!Host || !Host->IsActive())
	{
		return ftcore::ArenaBlowRuling();
	}
	if (Blow.target == Host->Slot && Blow.source == ftcore::kArenaPlayer)
	{
		return Host->RuleLeadBlow(Blow);
	}
	if (Blow.source == Host->Slot && Blow.target == ftcore::kArenaPlayer)
	{
		return Host->RuleBossBlow(Blow);
	}
	return ftcore::ArenaBlowRuling();
}

ftcore::ArenaBlowRuling FFTStoneBearerHost::RuleLeadBlow(const ftcore::ArenaBlow& Blow)
{
	// Locked on (no aim point): the crit judge picks the best gated point.
	ftcore::StoneBearerHit Hit;
	Absorb(Boss.onHit(Blow.from, Blow.at, BossFacing(), static_cast<double>(Blow.time), nullptr, &Hit));
	ftcore::ArenaBlowRuling Ruling;
	Ruling.damage = Hit.damage;
	Ruling.poise = Hit.poise;
	return Ruling;
}

ftcore::ArenaBlowRuling FFTStoneBearerHost::RuleBossBlow(const ftcore::ArenaBlow& Blow)
{
	ftcore::ArenaBlowRuling Ruling;
	if (bUseRefused || Blow.move >= ftcore::kMaxArenaMoves)
	{
		Ruling.veto = true;
		return Ruling;
	}
	const std::uint8_t M = MoveOf[Blow.move];
	const std::uint8_t K = ImpactOf[Blow.move];
	const ftcore::ArenaBossMove& Move = Def.boss.moves[M];
	const ftcore::ArenaFighter& Lead = FightPtr->def().fighter;

	// An intercept move's impact is the run's to answer: bring it up to the impact, and a turned blow doesn't land.
	if (Move.intercept() && Boss.run().liveMove() == M && K < Boss.run().strike().size())
	{
		const double At = Boss.run().strike().impact(K).at;
		Absorb(Boss.update(static_cast<double>(Blow.time) > At ? static_cast<double>(Blow.time) : At, HpShare()));
		if (Boss.run().strike().outcome(K) == ftcore::CalledOutcome::Intercepted)
		{
			Ruling.veto = true;
			return Ruling;
		}
	}

	// The keep-out: the shape cut short of the guest's disc; a lead beyond the cut reach isn't reached.
	if (!KeepOut.none())
	{
		const ftcore::ArenaAttack& A = FightPtr->def().enemies[Slot].moves[Blow.move].attack;
		bool bReached = true;
		if (A.arcCos <= -1.f)
		{
			const ftcore::Vec2 Centre = KeepOut.clampArea(Blow.from, A.range);
			bReached = Dist(Centre, Blow.at) <= A.range + Lead.bodyRadius;
		}
		else
		{
			const float Reach = A.width > 0.f ? KeepOut.clampLane(Blow.from, Blow.facing, A.width, A.range)
				: KeepOut.clampArc(Blow.from, Blow.facing, A.arcCos, A.range);
			bReached = Reach > 0.f && Dist(Blow.from, Blow.at) <= Reach + Lead.bodyRadius;
		}
		if (!bReached)
		{
			Ruling.veto = true;
			return Ruling;
		}
	}

	// The stacks' weight, then the move's cap for the whole use.
	Ruling.damage = Boss.damageScale();
	const int Cap = static_cast<int>(std::lround(static_cast<double>(Move.damageMax) * LeadHp));
	const int Dealt = static_cast<int>(std::lround(static_cast<double>(Blow.damage) * Ruling.damage));
	if (Cap > 0 && UseMove == M && UseDealt + Dealt > Cap)
	{
		const int Left = Cap - UseDealt;
		if (Left <= 0 || Blow.damage <= 0)
		{
			Ruling.veto = true;
			return Ruling;
		}
		Ruling.damage = static_cast<float>(Left) / static_cast<float>(Blow.damage);
		UseDealt = Cap;
	}
	else
	{
		UseDealt += Dealt;
	}
	return Ruling;
}

// ---- hook points: no meshes, sound or pickups yet; each leaves a note the runner logs and the HUD shows ----

void FFTStoneBearerHost::OnBroke(std::uint8_t CutMove, double At)
{
	// The carried piece splits here: spawn its two halves at the boss's hands when they have meshes.
	(void)CutMove;
	(void)At;
}

void FFTStoneBearerHost::OnShockRing(std::uint8_t BossMove, double At)
{
	// A floor blow at 3+ stacks: the ring's VFX and its hit go here (ArenaFight has no second blow per move).
	Note(FFTStoneBearerNote::EKind::ShockRingSent, BossMove, At);
}

void FFTStoneBearerHost::OnGuestCall(const ftcore::ArenaBossEvent& Call)
{
	// The guest's voice line for impact Call.impact of Call.move (a repeating call on the plinth: kArenaBossNoMove).
	(void)Call;
}

void FFTStoneBearerHost::OnLoadGained(const ftcore::ArenaBossEvent& Gained)
{
	// A full intercept's Load (Gained.value), given to the lead; the note keeps what was stored (the gauge caps it).
	const int Stored = FightPtr->giveLoad(Gained.value);
	Note(FFTStoneBearerNote::EKind::LoadGiven, Gained.move, Gained.at);
	Notes[static_cast<std::size_t>(NoteTotal - 1) % MaxNotes].Event.value = Stored;
}

void FFTStoneBearerHost::OnTileRain(std::uint8_t BossMove, double At)
{
	// A tile move's fragments over its area and its clear tile: ArenaFight lands one all-round blow instead.
	Note(FFTStoneBearerNote::EKind::TileRain, BossMove, At);
}
