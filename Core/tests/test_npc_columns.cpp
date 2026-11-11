// The optional per-NPC columns: lead_base (a fixed start per lead), stance_lines (an NPC's own stance lines), and the
// cond column of npc_places.csv (schedules that change with a story fact).
#include "ftcore/NpcMemory.h"
#include "ftcore/NpcPlaces.h"
#include <doctest/doctest.h>
#include <algorithm>
#include <memory>
#include <string>

using namespace ftcore;

namespace {

constexpr const char* kTags = "tag_id,bit\nhelp,0\n";
constexpr const char* kDeeds = "deed_id,tags,magnitude,polarity,secret,pin,attributable\nhelped,help,2,1,0,0,1\n";
constexpr const char* kTuning = R"({"stayGap": 10})";

bool load(const std::string& values, NpcTables& t, std::string* err = nullptr) {
    NpcTableSources src{kTags, kDeeds, values, "", "", "", "", kTuning};
    return importNpcTables(src, t, err);
}

struct World {
    NpcTables t;
    std::unique_ptr<NpcMemoryWorld> w = std::make_unique<NpcMemoryWorld>();
    explicit World(const std::string& values) {
        std::string err;
        const bool ok = load(values, t, &err);
        INFO(err);
        REQUIRE(ok);
        npcInit(*w, t);
    }
    std::uint16_t npc(const char* id) const {
        const int i = t.npcIndex(id);
        REQUIRE(i >= 0);
        return static_cast<std::uint16_t>(i);
    }
};

struct StubFacts final : NpcFactSource {
    std::uint8_t values[2] = {0, 0};
    int npcFactIndex(std::string_view id) const override { return id == "f_choice" ? 0 : id == "f_event" ? 1 : -1; }
    bool npcFactValue(int index, std::uint8_t& value) const override {
        if (index < 0 || index > 1 || values[index] == 0) return false;
        value = values[index];
        return true;
    }
};

}  // namespace

TEST_CASE("npc lead_base: a lead's own people start warmer to that lead only") {
    World s("npc_id,base_T,base_F,base_R,district_id,weights,lead_base\n"
            "npc_kin,10,0,10,1,help:1/0/0,0:20/-5/30\n"
            "npc_plain,10,0,10,1,help:1/0/0,\n");
    const std::uint16_t kin = s.npc("npc_kin"), plain = s.npc("npc_plain");
    CHECK(npcEffective(*s.w, s.t, kin, 0) == std::array<std::int32_t, 3>{30, 0, 40});  // F clamps at 0
    CHECK(npcEffective(*s.w, s.t, kin, 1) == std::array<std::int32_t, 3>{10, 0, 10});
    CHECK(npcEffective(*s.w, s.t, kin, kNoLead) == std::array<std::int32_t, 3>{10, 0, 10});  // the party: none
    CHECK(npcStance(*s.w, s.t, kin, 0) == NpcStance::Warm);
    CHECK(npcStance(*s.w, s.t, kin, 1) == NpcStance::Neutral);
    CHECK(npcStance(*s.w, s.t, kin, kNoLead) == NpcStance::Neutral);
    CHECK(npcEffective(*s.w, s.t, plain, 0) == std::array<std::int32_t, 3>{10, 0, 10});
    // Every lead at once, and each one apart.
    // The clamp holds: a start that pushes E past 100 or below 0 (F) stops there.
    World edge("npc_id,base_T,base_F,base_R,district_id,weights,lead_base\n"
               "npc_a,80,10,-90,1,help:1/0/0,2:50/-50/-50\n");
    CHECK(npcEffective(*edge.w, edge.t, 0, 2) == std::array<std::int32_t, 3>{100, 0, -100});
    World all("npc_id,base_T,base_F,base_R,district_id,weights,lead_base\n"
              "npc_a,0,0,0,1,help:1/0/0,0:1/2/3;1:-4/5/-6;2:50/50/50\n");
    REQUIRE(all.t.npcs.size() == 1);
    const NpcDef& d = all.t.npcs[0];
    CHECK(d.leadBase[0] == std::array<std::int8_t, 3>{1, 2, 3});
    CHECK(d.leadBase[1] == std::array<std::int8_t, 3>{-4, 5, -6});
    CHECK(d.leadBase[2] == std::array<std::int8_t, 3>{50, 50, 50});
}

TEST_CASE("npc lead_base and stance_lines: tables without the columns load unchanged") {
    NpcTables t;
    REQUIRE(load("npc_id,base_T,base_F,base_R,district_id,weights\nnpc_a,30,0,10,1,help:1/0/0\n", t));
    const NpcStanceLines shared;
    const NpcDef& d = t.npcs[0];
    for (const auto& l : d.leadBase) CHECK(l == std::array<std::int8_t, 3>{0, 0, 0});
    CHECK(d.lines.hostileT == shared.hostileT);
    CHECK(d.lines.warmT == 25);
    CHECK(d.lines.loyalR == 50);
    // The shared lines are the F4 rule as it was written before the lines became data (constants copied here on
    // purpose, so a changed default fails this test).
    const auto f4 = [](NpcStance cur, int T, int F, int R) {
        const int g = 10;
        NpcStance cand = NpcStance::Neutral;
        if (T <= -50 && F < 60) cand = NpcStance::Hostile;
        else if (F >= 50 || T <= -20) cand = NpcStance::Wary;
        else if (T >= 60 && R >= 50 && F < 30) cand = NpcStance::Loyal;
        else if (T >= 25 && F < 40) cand = NpcStance::Warm;
        bool stay = false;
        switch (cur) {
        case NpcStance::Hostile: stay = T <= -50 + g && F < 60 + g; break;
        case NpcStance::Wary: stay = F >= 50 - g || T <= -20 + g; break;
        case NpcStance::Loyal: stay = T >= 60 - g && R >= 50 - g && F < 30 + g; break;
        case NpcStance::Warm: stay = T >= 25 - g && F < 40 + g; break;
        case NpcStance::Neutral: stay = true; break;
        }
        const int c0 = static_cast<int>(cur), c1 = static_cast<int>(cand);
        return stay && c1 >= std::min(c0, 2) && c1 <= std::max(c0, 2) ? cur : cand;
    };
    int differ = 0;
    for (NpcStance cur : {NpcStance::Neutral, NpcStance::Warm, NpcStance::Loyal, NpcStance::Wary, NpcStance::Hostile})
        for (int T = -100; T <= 100; T += 3)
            for (int F = 0; F <= 100; F += 3)
                for (int R : {0, 40, 50, 60}) {
                    const NpcStance mine = f4(cur, T, F, R);
                    differ += npcStanceStep(cur, T, F, R, d.lines) != mine;
                    differ += npcStanceStep(cur, T, F, R) != mine;
                }
    CHECK(differ == 0);
}

TEST_CASE("npc lead_base: bad cells are refused") {
    const std::string head = "npc_id,base_T,base_F,base_R,district_id,weights,lead_base\nnpc_a,0,0,0,1,help:1/0/0,";
    for (const char* bad : {"3:1/1/1", "-1:1/1/1", "x:1/1/1", "0:1/1", "0:1/1/1/1", "0:51/0/0", "0:0/-51/0", "0:1/1/1;0:2/2/2",
                            "0 1/1/1", "0:a/1/1"}) {
        NpcTables t;
        std::string err;
        INFO(bad);
        CHECK_FALSE(load(head + bad + "\n", t, &err));
        CHECK(err.find("lead_base") != std::string::npos);
    }
}

TEST_CASE("npc stance_lines: an NPC's own lines move its stance") {
    World s("npc_id,base_T,base_F,base_R,district_id,weights,stance_lines\n"
            "npc_easy,20,0,0,1,help:1/0/0,warm_t:-10\n"
            "npc_hard,30,0,0,1,help:1/0/0,warm_t:+15;loyal_t:+5\n"
            "npc_jumpy,0,35,0,1,help:1/0/0,wary_f:-20;warm_f:-20;loyal_f:-10\n"
            "npc_same,20,0,0,1,help:1/0/0,\n");
    CHECK(s.t.npcs[0].lines.warmT == 15);
    CHECK(npcStance(*s.w, s.t, s.npc("npc_easy"), 0) == NpcStance::Warm);     // T 20 >= 15
    CHECK(npcStance(*s.w, s.t, s.npc("npc_same"), 0) == NpcStance::Neutral);  // T 20 < 25
    CHECK(npcStance(*s.w, s.t, s.npc("npc_hard"), 0) == NpcStance::Neutral);  // T 30 < 40
    CHECK(npcStance(*s.w, s.t, s.npc("npc_jumpy"), 0) == NpcStance::Wary);    // F 35 >= 30
    // The stay gap is kept around the shifted lines.
    const NpcStanceLines& easy = s.t.npcs[0].lines;
    CHECK(npcStanceStep(NpcStance::Warm, 6, 0, 0, easy) == NpcStance::Warm);  // 15 - 10 = 5
    CHECK(npcStanceStep(NpcStance::Warm, 4, 0, 0, easy) == NpcStance::Neutral);
}

TEST_CASE("npc stance_lines: bad cells are refused") {
    const std::string head = "npc_id,base_T,base_F,base_R,district_id,weights,stance_lines\nnpc_a,0,0,0,1,help:1/0/0,";
    // Defaults: hostile_t -50 < wary_t -20 < warm_t 25 <= loyal_t 60; loyal_f 30 <= warm_f 40 <= wary_f 50 <= hostile_f 60.
    for (const char* bad : {"warm:5", "warm_t", "warm_t:31", "warm_t:-31", "warm_t:x", "warm_t:1;warm_t:2",
                            "hostile_t:30",            // -20 not below wary_t -20
                            "wary_t:30;warm_t:-20",    // wary_t 10 above warm_t 5
                            "warm_t:30;loyal_t:-6",    // warm_t 55 above loyal_t 54
                            "loyal_f:15",              // 45 above warm_f 40
                            "loyal_f:-30",             // 0: Loyal unreachable
                            "warm_f:15",               // 55 above wary_f 50
                            "hostile_f:-15"}) {        // 45 below wary_f 50
        NpcTables t;
        std::string err;
        INFO(bad);
        CHECK_FALSE(load(head + bad + "\n", t, &err));
        CHECK(err.find("stance_lines") != std::string::npos);
    }
    for (const char* good : {"wary_t:+30", "warm_t:30;loyal_t:-5", "loyal_f:10;warm_f:0", "warm_t:-5;wary_f:0"}) {
        NpcTables t;
        std::string err;
        const bool ok = load(head + good + "\n", t, &err);
        INFO(good << " " << err);
        CHECK(ok);
    }
}

namespace {

constexpr const char* kTwo = "npc_id,base_T,base_F,base_R,district_id,weights\nnpc_a,0,0,0,1,help:1/0/0\nnpc_b,0,0,0,1,help:1/0/0\n";

int where(const NpcPlaceTable& p, const NpcTables& t, const char* npc, double h, const NpcFactSource* f) {
    return npcPlaceAt(p, static_cast<std::uint16_t>(t.npcIndex(npc)), h, f);
}

std::string name(const NpcPlaceTable& p, int i) { return i < 0 ? std::string("-") : p.places[static_cast<std::size_t>(i)]; }

}  // namespace

TEST_CASE("npc places cond: the most specific holding row wins; an unset fact falls back") {
    NpcTables t;
    REQUIRE(load(kTwo, t));
    StubFacts facts;
    NpcPlaceTable p;
    std::string err;
    const bool ok = importNpcPlaces("npc_id,from,to,place_id,cond\n"
                                    "npc_a,08:00,18:00,stall,\n"
                                    "npc_a,12:00,16:00,bakery,fact:f_event=1\n"
                                    "npc_a,12:00,16:00,docks,fact:f_choice=B\n"   // same count, other fact: refused? no, see below
                                    "npc_a,13:00,14:00,roof,fact:f_event=1;fact:f_choice=B\n"
                                    "npc_a,20:00,02:00,-,fact:f_event=2\n"
                                    "npc_a,20:00,02:00,bar,\n"
                                    "npc_b,09:00,10:00,stall,fact:f_choice=A\n",
                                    t, &facts, p, &err);
    // f_event=1 and f_choice=B can both hold at 12:00..16:00 with one clause each: a tie, so the import refuses it.
    CHECK_FALSE(ok);
    CHECK(err.find(":4:") != std::string::npos);
    CHECK(err.find("row 3") != std::string::npos);

    const bool ok2 = importNpcPlaces("npc_id,from,to,place_id,cond\n"
                                     "npc_a,08:00,18:00,stall,\n"
                                     "npc_a,12:00,16:00,bakery,fact:f_event=1\n"
                                     "npc_a,12:00,16:00,docks,fact:f_event=2\n"  // the same fact, another value: never both
                                     "npc_a,13:00,14:00,roof,fact:f_event=1;fact:f_choice=B\n"
                                     "npc_a,20:00,02:00,-,fact:f_event=2\n"
                                     "npc_a,20:00,02:00,bar,\n"
                                     "npc_b,09:00,10:00,stall,fact:f_choice=A\n",
                                     t, &facts, p, &err);
    INFO(err);
    REQUIRE(ok2);
    CHECK(p.places.size() == 5);  // stall, bakery, docks, roof, bar; `-` is not a place
    auto at = [&](const char* n, double h, const NpcFactSource* f = nullptr) { return name(p, where(p, t, n, h, f)); };
    // Nothing set: only the plain rows hold.
    CHECK(at("npc_a", 13.5, &facts) == "stall");
    CHECK(at("npc_a", 22.0, &facts) == "bar");
    CHECK(at("npc_b", 9.5, &facts) == "-");
    facts.values[1] = 1;  // f_event = 1
    CHECK(at("npc_a", 9.0, &facts) == "stall");
    CHECK(at("npc_a", 12.0, &facts) == "bakery");
    CHECK(at("npc_a", 13.5, &facts) == "bakery");  // the roof row also needs f_choice=B
    facts.values[0] = 2;                             // f_choice = B
    CHECK(at("npc_a", 13.5, &facts) == "roof");     // two clauses beat one
    CHECK(at("npc_a", 14.0, &facts) == "bakery");   // 14:00 is past the roof span
    CHECK(at("npc_a", 22.0, &facts) == "bar");
    facts.values[1] = 2;  // f_event = 2
    CHECK(at("npc_a", 12.5, &facts) == "docks");
    CHECK(at("npc_a", 13.5, &facts) == "docks");  // roof no longer holds
    CHECK(at("npc_a", 22.0, &facts) == "-");      // off the map once the fact is set
    CHECK(at("npc_a", 1.5, &facts) == "-");
    CHECK(at("npc_a", 2.0, &facts) == "-");       // no row covers 02:00
    facts.values[1] = 1;
    CHECK(at("npc_a", 1.5, &facts) == "bar");     // past midnight, the clause row no longer holds
    facts.values[1] = 2;
    facts.values[0] = 1;                          // f_choice = A
    CHECK(at("npc_b", 9.5, &facts) == "stall");
    // No source at lookup: clause rows never hold.
    CHECK(at("npc_a", 13.5) == "stall");
    CHECK(at("npc_a", 22.0) == "bar");
    CHECK(at("npc_b", 9.5) == "-");
}

TEST_CASE("npc places cond: refused clauses and ties") {
    NpcTables t;
    REQUIRE(load(kTwo, t));
    StubFacts facts;
    const std::string head = "npc_id,from,to,place_id,cond\n";
    auto refuses = [&](const std::string& rows, const NpcFactSource* f, const char* why) {
        NpcPlaceTable p;
        p.places.push_back("untouched");
        std::string err;
        INFO(rows);
        CHECK_FALSE(importNpcPlaces(head + rows, t, f, p, &err));
        CHECK(err.find(why) != std::string::npos);
        CHECK(p.places.size() == 1);  // out untouched
    };
    refuses("npc_a,08:00,09:00,x,fact:f_event=1\n", nullptr, "fact source");
    refuses("npc_a,08:00,09:00,x,fact:f_nope=1\n", &facts, "unknown fact");
    refuses("npc_a,08:00,09:00,x,fact:f_event=1;fact:f_event=2\n", &facts, "twice");
    refuses("npc_a,08:00,09:00,x,fact:f_event=1;fact:f_choice=A;fact:f_event=1;fact:f_choice=A;fact:f_event=1\n", &facts, "twice");
    refuses("npc_a,08:00,09:00,x,fact:f_event=255\n", &facts, "fact:<id>=<value>");
    refuses("npc_a,08:00,09:00,x,fact:f_event=E\n", &facts, "fact:<id>=<value>");
    refuses("npc_a,08:00,09:00,x,fact:f_event=\n", &facts, "fact:<id>=<value>");
    refuses("npc_a,08:00,09:00,x,fact:=1\n", &facts, "fact:<id>=<value>");
    refuses("npc_a,08:00,09:00,x,mem:deed=helped\n", &facts, "fact:<id>=<value>");
    refuses("npc_a,08:00,09:00,-,\nnpc_a,08:30,09:30,-,\n", &facts, "row 2");  // off-map rows tie too
    // Ties: plain rows that overlap; clause rows with the same clauses; overlap across midnight.
    refuses("npc_a,08:00,10:00,x,\nnpc_a,09:00,11:00,y,\n", &facts, "row 2");
    refuses("npc_a,08:00,10:00,x,fact:f_event=1\nnpc_a,09:59,11:00,y,fact:f_event=1\n", &facts, "row 2");
    refuses("npc_a,22:00,02:00,x,fact:f_event=1\nnpc_a,01:00,03:00,y,fact:f_choice=A\n", &facts, "row 2");
    // Five distinct facts would be needed for a fifth clause; the stub knows two, so check the cap with a wider source.
    struct Wide final : NpcFactSource {
        int npcFactIndex(std::string_view id) const override { return id.size() == 2 && id[0] == 'f' ? id[1] - '0' : -1; }
        bool npcFactValue(int, std::uint8_t&) const override { return false; }
    } wide;
    refuses("npc_a,08:00,09:00,x,fact:f1=1;fact:f2=1;fact:f3=1;fact:f4=1;fact:f5=1\n", &wide, "more than four");
    {
        NpcPlaceTable p;
        CHECK(importNpcPlaces(head + "npc_a,08:00,09:00,x,fact:f1=1;fact:f2=1;fact:f3=1;fact:f4=1\n", t, &wide, p));
        CHECK(p.spans[0].condCount == 4);
    }
    // Not ties: touching spans, other NPCs, a fact told apart, different clause counts.
    NpcPlaceTable p;
    std::string err;
    const bool ok = importNpcPlaces(head + "npc_a,08:00,10:00,x,\n"
                                           "npc_a,10:00,11:00,y,\n"
                                           "npc_b,08:00,10:00,y,\n"
                                           "npc_a,22:00,02:00,x,fact:f_choice=A\n"
                                           "npc_a,01:00,03:00,y,fact:f_choice=B\n"
                                           "npc_a,09:00,09:30,z,fact:f_event=3\n"
                                           "npc_a,02:00,08:00,-,\n",
                                    t, &facts, p, &err);
    INFO(err);
    CHECK(ok);
    // The old overload (no source) still loads plain tables and refuses clauses.
    NpcPlaceTable q;
    CHECK(importNpcPlaces(head + "npc_a,08:00,10:00,x,\n", t, q));
    CHECK_FALSE(importNpcPlaces(head + "npc_a,08:00,10:00,x,fact:f_event=1\n", t, q));
}
