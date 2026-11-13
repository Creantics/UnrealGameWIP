#include "FTKiteHost.h"

bool FFTKiteHost::Load(std::string_view Json, std::string* Error, std::vector<std::string>* Warnings)
{
	bLoaded = ftcore::importArenaWeakPoints(Json, Weak, Error, Warnings);
	return bLoaded;
}

void FFTKiteHost::Begin(ftcore::ArenaFight& Fight, std::size_t InSlot)
{
	if (!bLoaded || InSlot >= Fight.enemyCount())
	{
		End();
		return;
	}
	FightPtr = &Fight;
	Slot = InSlot;
	Crit.reset(Weak);
	bWasOpen = false;
	NoteTotal = 0;
	Crits = 0;
	Fight.setBlowHook(&FFTKiteHost::RuleBlow, this);
}

void FFTKiteHost::End()
{
	if (FightPtr)
	{
		FightPtr->setBlowHook(nullptr, nullptr);
	}
	FightPtr = nullptr;
}

ftcore::Vec2 FFTKiteHost::BossFacing() const
{
	return FightPtr->output().enemies[Slot].facing;
}

void FFTKiteHost::AfterTick(const ftcore::ArenaEvents& Events)
{
	if (!IsActive())
	{
		return;
	}
	ftcore::ArenaFight& Fight = *FightPtr;
	const double T = static_cast<double>(Fight.time());
	const ftcore::ArenaEnemyOut& Out = Fight.output().enemies[Slot];
	const ftcore::ArenaEnemyKind& Kind = Fight.def().enemies[Slot];
	for (const ftcore::ArenaEvent& E : Events)
	{
		switch (E.type)
		{
		case ftcore::ArenaEventType::AttackStarted:
			if (E.source == Slot)
			{
				// The move began StateTime ago, if it is still the one running.
				const double Ago = Out.brain == ftcore::ArenaBrain::Attack && Out.move == E.move ? static_cast<double>(Out.stateTime) : 0.0;
				OnTelegraph(E.move, T - Ago);
			}
			break;
		case ftcore::ArenaEventType::Parried:
			// The parry opens the boss: its weak point takes crits for as long as it stays open.
			if (E.source == Slot)
			{
				OpenWindow(T, Kind.openTime);
			}
			break;
		case ftcore::ArenaEventType::Staggered:
			if (E.target == Slot)
			{
				OpenWindow(T, Kind.staggerTime);
			}
			break;
		default:
			break;
		}
	}
	// The window shuts itself at its end (CritWindow::isOpen is a time).
	const bool bOpen = Crit.isOpen(T);
	if (bWasOpen && !bOpen)
	{
		Note(FFTKiteNote::EKind::CritClosed, Crit.closesAt());
	}
	bWasOpen = bOpen;
}

void FFTKiteHost::OpenWindow(double At, float Seconds)
{
	Crit.open(At, static_cast<double>(Seconds));
	bWasOpen = true;
	OnCritOpened(At, Crit.closesAt());
}

FFTKiteNote& FFTKiteHost::Note(FFTKiteNote::EKind Kind, double At)
{
	FFTKiteNote& N = Notes[static_cast<std::size_t>(NoteTotal) % MaxNotes];
	N = FFTKiteNote();
	N.Kind = Kind;
	N.At = At;
	++NoteTotal;
	return N;
}

ftcore::ArenaBlowRuling FFTKiteHost::RuleBlow(void* User, const ftcore::ArenaBlow& Blow)
{
	FFTKiteHost* Host = static_cast<FFTKiteHost*>(User);
	if (!Host || !Host->IsActive() || Blow.target != Host->Slot || Blow.source != ftcore::kArenaPlayer)
	{
		return ftcore::ArenaBlowRuling();
	}
	// A held blow's bonus (HostHit) is not a hit: no crit judged, none noted; only the bars rule it. The Kite-Mother has
	// no invulnerable phase, so today Bars passes everything and the bonus lands whole. A phase that bars blows goes in
	// Bars, which rules the lead's blows and the bonus alike, so the bonus can never slip past it.
	const ftcore::ArenaBlowRuling Barred = Host->Bars(Blow);
	if (Blow.type == ftcore::ArenaEventType::HostHit || Barred.veto)
	{
		return Barred;
	}
	return Host->RuleLeadBlow(Blow);
}

ftcore::ArenaBlowRuling FFTKiteHost::RuleLeadBlow(const ftcore::ArenaBlow& Blow)
{
	// Locked on (no aim point): the judge takes the best point whose gate the lead's side of the boss passes.
	const double T = static_cast<double>(Blow.time);
	const ftcore::CritResult Result = Crit.judgeLockOn(Blow.from, Blow.at, BossFacing(), T);
	ftcore::ArenaBlowRuling Ruling;
	if (Result.crit)
	{
		++Crits;
		Ruling.damage = Result.multiplier;
		Ruling.poise = Result.multiplier;
		OnCritLanded(Result, T);
	}
	OnHitTaken(Result.crit, T);
	return Ruling;
}

// ---- hook points: each leaves a note the runner logs (the effects play in FFTKiteVfx, from the fight's events) ----

void FFTKiteHost::OnTelegraph(std::uint8_t Move, double T)
{
	Note(FFTKiteNote::EKind::Telegraph, T).Move = Move;
}

void FFTKiteHost::OnCritOpened(double At, double Until)
{
	Note(FFTKiteNote::EKind::CritOpened, At).Until = Until;
}

void FFTKiteHost::OnCritLanded(const ftcore::CritResult& Result, double At)
{
	// Result.first flags the first of the opening.
	FFTKiteNote& N = Note(FFTKiteNote::EKind::CritLanded, At);
	N.Point = Result.point;
	N.Multiplier = Result.multiplier;
	N.bFirst = Result.first;
}

void FFTKiteHost::OnHitTaken(bool bCrit, double At)
{
	Note(FFTKiteNote::EKind::HitTaken, At).bCrit = bCrit;
}
