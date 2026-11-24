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

namespace
{
// A leg bite's two legs nearer than this to the teeth than each other (cm) is a tie: Core's side is kept.
constexpr float kLegTieCm = 0.2f;
}

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
	for (ftcore::ArenaHitReaction R : HitReactions)
	{
		const bool bPlayerBody = (R.target == ftcore::kArenaPlayer) != R.tookOver;
		AFTArenaFighter* Figure = bPlayerBody ? PlayerFigure.Get() : AllyFigure.Get();
		if (!Figure || !Figure->HasRagdoll())
		{
			continue; // that body keeps the shapes' lean (its missing ragdoll was warned once at wear time)
		}
		// A blow on a leg reacts on the leg the attacker's teeth are at, by world distance (Core's side, by the attacker's
		// place against the body's facing, flips dead ahead).
		FString LegNote;
		const bool bLegPart = R.part >= 0 && (R.part == Setup->thighL || R.part == Setup->thighR || R.part == Setup->shinL || R.part == Setup->shinR);
		AFTArenaFighter* Attacker = bLegPart && R.source < MaxEnemies ? EnemyFigures[R.source].Get() : nullptr;
		if (Attacker)
		{
			FVector Snout;
			ftcore::ArenaLegChain Legs[2];
			bool bBones = Attacker->GetSkinPoint(TEXT("jaw_upper"), Snout) || Attacker->GetSkinPoint(TEXT("head"), Snout);
			const TCHAR* const Chain[2][3] = {{TEXT("thigh_l"), TEXT("calf_l"), TEXT("foot_l")}, {TEXT("thigh_r"), TEXT("calf_r"), TEXT("foot_r")}};
			for (int32 Side = 0; Side < 2 && bBones; ++Side)
			{
				for (int32 J = 0; J < 3 && bBones; ++J)
				{
					FVector At;
					bBones = Figure->GetSkinPoint(Chain[Side][J], At);
					Legs[Side][static_cast<size_t>(J)] = ftcore::ArenaPoint3{static_cast<float>(At.X), static_cast<float>(At.Y), static_cast<float>(At.Z)};
				}
			}
			if (bBones)
			{
				const FVector Fwd = Attacker->GetActorForwardVector();
				const ftcore::ArenaPoint3 From{static_cast<float>(Snout.X), static_cast<float>(Snout.Y), static_cast<float>(Snout.Z)};
				const ftcore::Vec2 Dir{static_cast<float>(Fwd.X), static_cast<float>(Fwd.Y)};
				const ftcore::ArenaLegReach Reach = ftcore::arenaLegReach(From, Dir, Legs[0], Legs[1]);
				const int Side = ftcore::arenaLegAtTeeth(From, Dir, Legs[0], Legs[1], kLegTieCm);
				const int Was = R.part;
				R.part = ftcore::arenaLegPartOnSide(R.part, Side, *Setup);
				LegNote = FString::Printf(TEXT(", leg by teeth: left %.1f cm, right %.1f cm -> %s%s"), Reach.left, Reach.right,
					Side == 0 ? TEXT("left") : Side == 1 ? TEXT("right") : TEXT("tie (Core's side kept)"), R.part != Was ? TEXT(" (moved from Core's side)") : TEXT(""));
			}
		}
		const bool bPlayed = Figure->ReactToHit(R, FVector(R.dir.x, R.dir.y, 0.0));
		// The part by the setup's names (thigh_l etc.), and the push direction in world X/Y with the struck body's yaw, so a
		// review can tell a wrong-side part or a reversed push from drive or idle motion.
		const TCHAR* PartName = R.part == Setup->thighL ? TEXT("thigh_l") : R.part == Setup->thighR ? TEXT("thigh_r")
			: R.part == Setup->shinL ? TEXT("shin_l") : R.part == Setup->shinR ? TEXT("shin_r") : R.part == Setup->pelvis ? TEXT("pelvis")
			: R.part == Setup->spine ? TEXT("spine") : R.part == Setup->chest ? TEXT("chest") : R.part == Setup->head ? TEXT("head") : TEXT("other");
		UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.2f: hit reaction on the %s body: tier %d, part %d (%s), %.0f N*s, %d blow(s), stop %d, push world (%.3f, %.3f), body yaw %.1f%s."),
			Fight.time(), bPlayerBody ? TEXT("player's") : TEXT("ally's"), static_cast<int32>(R.tier), R.part, PartName, R.strength,
			static_cast<int32>(R.blows), R.hitStopFrames, R.dir.x, R.dir.y, Figure->GetActorRotation().Yaw,
			bPlayed ? TEXT("") : TEXT(" (not played: down or no bodies)"));
		if (!LegNote.IsEmpty())
		{
			UE_LOG(LogForgottenThird, Log, TEXT("Arena: %.2f: hit reaction on the %s body%s."), Fight.time(), bPlayerBody ? TEXT("player's") : TEXT("ally's"), *LegNote);
		}
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
