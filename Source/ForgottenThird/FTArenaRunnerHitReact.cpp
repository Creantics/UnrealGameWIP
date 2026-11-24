// UFTArenaRunner's lead hit reaction (FTArenaRunner.h, "The leads' hit reaction"): each tick's blows on the leads, read
// by Core (ftcore/ArenaHitReact.h) into at most one reaction per struck body, played on that body's ragdoll
// (AFTArenaFighter::ReactToHit, FTPhysicsComponent::React). A figure without a ragdoll (no look, or no
// PHYS_<body>_Ragdoll asset) reacts to nothing here and keeps the shapes' lean.
//
// The bodies: a reaction names the lead it struck as the tick began (kArenaPlayer or kArenaAlly). On a takeover tick
// (tookOver) TakeOverBodies has already swapped the figures, so the player's blow goes to the ally's figure (the
// fallen lead's body) and the ally's to the player's (the body that now plays).

#include "FTArenaRunner.h"

#include "FTArenaFighter.h"
#include "ForgottenThird.h"

void UFTArenaRunner::ReactLeadBodies(const ftcore::ArenaEvents& Events)
{
	if (Events.hitCount == 0)
	{
		return;
	}
	// Both leads share the humanoid map and the profile, so either figure's setup reads the tick.
	const ftcore::ArenaHitSetup* Setup = PlayerFigure ? PlayerFigure->GetHitSetup() : nullptr;
	if (!Setup && AllyFigure)
	{
		Setup = AllyFigure->GetHitSetup();
	}
	if (!Setup)
	{
		return;
	}
	ftcore::arenaHitReactions(Events, *Setup, HitReactions);
	for (const ftcore::ArenaHitReaction& R : HitReactions)
	{
		const bool bPlayerBody = (R.target == ftcore::kArenaPlayer) != R.tookOver;
		AFTArenaFighter* Figure = bPlayerBody ? PlayerFigure.Get() : AllyFigure.Get();
		if (!Figure || !Figure->HasRagdoll())
		{
			continue; // that body keeps the shapes' lean (its missing ragdoll was warned once at wear time)
		}
		const bool bPlayed = Figure->ReactToHit(R, FVector(R.dir.x, R.dir.y, 0.0));
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.2f: hit reaction on the %s body: tier %d, part %d, %.0f N*s, %d blow(s), stop %d%s."),
			Fight.time(), bPlayerBody ? TEXT("player's") : TEXT("ally's"), static_cast<int32>(R.tier), R.part, R.strength,
			static_cast<int32>(R.blows), R.hitStopFrames, bPlayed ? TEXT("") : TEXT(" (not played: down or no bodies)"));
	}
}

void UFTArenaRunner::RecoverLeadBodies(bool bInstant)
{
	if (PlayerFigure)
	{
		PlayerFigure->RecoverBody(bInstant);
	}
	if (AllyFigure)
	{
		AllyFigure->RecoverBody(bInstant);
	}
}
