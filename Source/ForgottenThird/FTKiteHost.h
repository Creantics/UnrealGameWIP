#pragma once

// The host of the Kite-Mother's weak point (arena.json's `kite_boss`): one ftcore::CritWindow (Core/include/ftcore/
// ArenaBoss.h) beside the boss's ArenaFight body. Its moves stay arena.json's; this adds only the seam, the smile seam on
// the paper face, which takes critical hits while the boss is open. UFTArenaRunner owns one and steps it after Fight.tick;
// it holds no Unreal type, so a plain C++ harness can run it with the Core sources.
//
// What it does, call by call:
//   - load time: reads Data/Battle/arena_boss/kite_boss_weak.json (importArenaWeakPoints), the points in the boss's figure
//     space (WeakPoint.h: the unit is the figure's height, y up from the mesh's bottom, which `hover` lifts off the ground);
//   - Begin: resets the window and sets the blow hook (ArenaFight has one hook per fight, so the runner never begins this
//     host in a fight the stone_bearer's host is hooked in);
//   - AfterTick: AttackStarted of the kite is its telegraph (OnTelegraph); Parried (the swoop's parry) opens the window
//     for the kind's openTime, Staggered for its staggerTime; the window shuts itself at its end (CritClosed);
//   - the blow hook: a lead's blow on the kite is judged with the gated points, locked on as the stone_bearer's are
//     (CritWindow::judgeLockOn), and a crit scales the blow's damage and poise by the point's multiplier;
//   - every opening, crit and close is kept as a note, and the runner logs each as one line, "Arena: kite ...".
//
// Stubs, each a named hook point that only leaves a note (a later step spawns Niagara systems from them): OnTelegraph,
// OnCritOpened, OnCritLanded, OnHitTaken.
//
// Nothing here allocates after Load.

#include "ftcore/ArenaBoss.h"
#include "ftcore/ArenaFight.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// One of the kite's notes (Kind says which fields are set).
struct FFTKiteNote
{
	enum class EKind : std::uint8_t { Telegraph, CritOpened, CritLanded, CritClosed, HitTaken };

	EKind Kind = EKind::Telegraph;
	double At = 0.0;          // fight time
	double Until = 0.0;       // CritOpened: when the window shuts
	std::uint8_t Move = 0;    // Telegraph: the arena move
	std::uint8_t Point = 0;   // CritLanded: index of the point hit
	float Multiplier = 1.f;   // CritLanded: the point's multiplier
	bool bFirst = false;      // CritLanded: the first crit of its opening
	bool bCrit = false;       // HitTaken: the blow was a crit
};

class FFTKiteHost
{
public:
	static constexpr std::string_view KindId = "kite_boss";
	static constexpr const char* DataPath = "Data/Battle/arena_boss/kite_boss_weak.json";
	static constexpr std::size_t MaxNotes = 32;
	// The mesh socket of its weak point (the json's `seam`; the rig's bone is named the same): any marker, socket lookup or
	// crit VFX hook uses this name.
	static constexpr const char* WeakPointSocket = "seam";

	// Reads the weak-point file. False (with the loader's error) when it is refused.
	bool Load(std::string_view Json, std::string* Error, std::vector<std::string>* Warnings);
	bool IsLoaded() const { return bLoaded; }
	const ftcore::ArenaWeakPoints& GetWeakPoints() const { return Weak; }

	// Starts the host in a fight just begun (or restarted) with the kite in slot Slot. Fight must outlive the host's use of
	// it (the blow hook points here). Sets the blow hook: the caller makes sure nothing else holds it.
	void Begin(ftcore::ArenaFight& Fight, std::size_t Slot);
	// Takes the blow hook back; the host does nothing until the next Begin.
	void End();
	bool IsActive() const { return FightPtr != nullptr; }
	std::size_t GetSlot() const { return Slot; }

	// After the tick: the fight's events and the window's end.
	void AfterTick(const ftcore::ArenaEvents& Events);

	// ---- for the HUD ----
	const ftcore::CritWindow& GetCrit() const { return Crit; }
	// The weak point is open to crits now.
	bool IsOpen() const { return IsActive() && Crit.isOpen(static_cast<double>(FightPtr->time())); }
	int32_t GetNoteTotal() const { return NoteTotal; }
	int32_t GetNoteCount() const { return NoteTotal < static_cast<int32_t>(MaxNotes) ? NoteTotal : static_cast<int32_t>(MaxNotes); }
	// Note I of the GetNoteCount() newest, oldest first; Serial its number (0..GetNoteTotal()-1).
	const FFTKiteNote& GetNoteBySerial(int32_t Serial) const { return Notes[static_cast<std::size_t>(Serial) % MaxNotes]; }
	int32_t GetCrits() const { return Crits; }

private:
	static ftcore::ArenaBlowRuling RuleBlow(void* User, const ftcore::ArenaBlow& Blow);
	ftcore::ArenaBlowRuling RuleLeadBlow(const ftcore::ArenaBlow& Blow);
	// Opens the window for Seconds from At.
	void OpenWindow(double At, float Seconds);
	FFTKiteNote& Note(FFTKiteNote::EKind Kind, double At);
	ftcore::Vec2 BossFacing() const;

	// The named hook points (stubs): see the header comment.
	void OnTelegraph(std::uint8_t Move, double T);
	void OnCritOpened(double At, double Until);
	void OnCritLanded(const ftcore::CritResult& Result, double At);
	void OnHitTaken(bool bCrit, double At);

	ftcore::ArenaWeakPoints Weak;
	ftcore::CritWindow Crit;
	bool bLoaded = false;

	ftcore::ArenaFight* FightPtr = nullptr;
	std::size_t Slot = 0;
	// The window was open at the last step (so its end is noted once).
	bool bWasOpen = false;

	std::array<FFTKiteNote, MaxNotes> Notes{};
	int32_t NoteTotal = 0;
	int32_t Crits = 0;
};
