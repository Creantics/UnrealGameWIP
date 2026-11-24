// A capture switch, never in a shipping build: -ArenaMoveTiming=<file> lays a small JSON file of values over the arena
// kinds and their clip tables after Core has loaded them, so a review capture can run a clip's measured timing without
// touching the shipped Data/ (whose startups must stay whole beats). With no flag, nothing changes. One log line per value
// changed. The file:
//   { "kinds": { "<kind id>": { "<kind field>": n, ..., "moves": { "<move clip id>": { "<move field>": n, ... } } } },
//     "clips": { "<clip table path, as the kind's look names it>": { "<clip id>": { "<clip field>": n | "asset" } } } }
// Kind fields: walkSpeed, runSpeed, strafeSpeed, accel, turnRate, preferredRange, runRange, attackDelay. Move fields:
// startup, active, recovery, range, lunge, damage, poiseDamage, weight, cooldown, minRange, maxRange. Clip fields: length_s,
// impact_s, speed_mps, blend_s, asset. Unknown names are logged as warnings and skipped.
#pragma once

#include "CoreMinimal.h"

namespace ftcore
{
struct ArenaTable;
struct BeatClips;
} // namespace ftcore

namespace FTArenaDevTiming
{
// Lays the file's kind values over Table.enemies (by kind id). Call right after importArenaTable.
void ApplyKinds(ftcore::ArenaTable& Table, const TCHAR* Who);
// Lays the file's clip values over Clips, the table read from TablePath (the kind's look.clips()). Call right after
// importBeatClips.
void ApplyClips(ftcore::BeatClips& Clips, const FString& TablePath, int32 Enemy);
} // namespace FTArenaDevTiming
