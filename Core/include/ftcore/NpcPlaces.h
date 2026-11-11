#pragma once

// Where each remembered NPC stands by the hour (Data/Npc/npc_places.csv): npc_id,from,to,place_id and an optional
// cond column. Times are HH:MM from 00:00 to 24:00 (from below 24:00; to 00:00 means midnight); a span with to < from
// runs past midnight. An hour no matching span covers means the NPC is off the map (home, asleep, away); place_id `-`
// says so outright (to take an NPC off the map once a fact is set), so `-` is never a place id. Place ids are the level's own names for spots
// (the game resolves them to markers); Core only checks them, keeps them, and answers "where now".
//
// Schedules that change with the story: cond holds `fact:<id>=<value>` clauses separated by ';' (value A..D for a
// choice, or a number 0..254), read through the same NpcFactSource as the lines. A row holds while every clause holds
// (an unset fact fails it, so `=0` holds only if the source reports 0 as a set value). At a minute, of the rows that cover it and hold, the one with the most clauses wins (the
// most specific). Import refuses two rows of one NPC that overlap in time, have as many clauses, and could hold at
// once (no fact given two different values between them), so the winner is never a tie.
// Plain C++20. Allocates only in importNpcPlaces (load time); npcPlaceAt does not allocate.

#include "ftcore/Api.h"
#include "ftcore/NpcMemory.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

constexpr std::uint16_t kNpcDayMinutes = 24 * 60;
constexpr std::uint16_t kNpcOffMap = 0xFFFF;  // NpcPlaceSpan::place for place_id `-`
constexpr std::uint8_t kNpcPlaceCondMax = 4;   // clauses per row

struct NpcPlaceCond {
    std::int16_t fact = -1;  // the fact source's index
    std::uint8_t value = 0;
};

struct NpcPlaceSpan {
    std::uint16_t npc = 0;      // index in NpcTables::npcs
    std::uint16_t from = 0;     // minute of the day, 0..1439
    std::uint16_t to = 0;       // minute of the day, 1..1440; to < from wraps past midnight
    std::uint16_t place = 0;    // index in NpcPlaceTable::places, or kNpcOffMap
    std::uint8_t condCount = 0;
    std::array<NpcPlaceCond, kNpcPlaceCondMax> cond{};
};

struct NpcPlaceTable {
    std::vector<std::string> places;  // distinct place ids, in first-seen order (`-` is not one)
    std::vector<NpcPlaceSpan> spans;  // file order
};

// False (out untouched) with *error naming the row on a missing column, an unknown npc, a bad or empty time span,
// an empty place id, a bad clause (an unknown fact, a fact twice in a row, more than four, or any clause with no fact
// source), or two rows of one NPC that could both win the same minute (see above).
FTCORE_API bool importNpcPlaces(std::string_view csv, const NpcTables& t, NpcPlaceTable& out, std::string* error = nullptr);
FTCORE_API bool importNpcPlaces(std::string_view csv, const NpcTables& t, const NpcFactSource* facts, NpcPlaceTable& out,
                                std::string* error = nullptr);
// The place index for the NPC at that hour of the day (any value, wrapped to 0..24), or -1 when off the map. Facts is
// the source the table was imported with (null: rows with clauses never hold).
FTCORE_API int npcPlaceAt(const NpcPlaceTable& p, std::uint16_t npc, double hours, const NpcFactSource* facts = nullptr);

}  // namespace ftcore
