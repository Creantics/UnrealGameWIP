// NPC memory and disposition: the spec's 28 checks, review regressions and a no-heap check on the hot paths.
// Test data uses mechanical ids only (tag_a, deed_a, npc_a, line_00).
#include "ftcore/NpcMemory.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>

using namespace ftcore;

namespace {

constexpr const char* kTags = R"(tag_id,bit
tag_a,0
tag_b,1
tag_c,2
tag_d,3
tag_e,4
)";

constexpr const char* kDeeds = R"(deed_id,tags,magnitude,polarity,secret,pin,attributable
deed_a,tag_a;tag_b,2,-1,0,0,1
deed_b,tag_c,3,0,0,0,1
deed_c,tag_d,2,1,0,0,1
deed_s,tag_c,2,-1,1,0,1
deed_p,tag_e,3,-1,0,1,1
deed_k,tag_e,2,1,0,0,1
deed_meet,tag_d,1,0,0,0,1
deed_z,,5,0,0,0,0
deed_max,tag_a;tag_b;tag_c,5,-1,0,0,1
deed_h,tag_d,4,0,0,0,0
)";

constexpr const char* kNpcs = R"(npc_id,base_T,base_F,base_R,district_id,weights
npc_a,20,0,0,1,tag_a:-3/2/-1;tag_b:-2/1/-1;tag_c:-1/3/0;tag_d:1/0/2
npc_b,0,0,0,1,tag_a:-2/1/-1;tag_c:-2/2/-1;tag_e:-3/0/-2
npc_c,0,0,0,2,tag_a:-1/1/0
npc_d,0,0,0,2,tag_a:-1/1/0
npc_x,0,0,0,3,tag_a:-6/6/-6;tag_b:-6/6/-6;tag_c:-6/6/-6
npc_y,0,0,0,3,tag_d:5/0/0
npc_r,100,0,0,4,tag_a:-5/0/0
)";

constexpr const char* kTies = R"(from_id,to_id,kind,tie,closeness,delay_min
npc_b,npc_a,npc,60,80,60
npc_c,npc_b,npc,20,65,60
npc_d,npc_c,npc,40,90,60
npc_a,0,lead,30,50,0
)";

constexpr const char* kConsequences = R"(npc_id,stance,price_pct,shop_open,door_open,quest_open,fight_role,greet_anim,greet_dist_cm,van_ride
npc_a,warm,90,1,1,1,aid,2,150,1
npc_a,neutral,100,1,1,1,none,1,200,0
npc_a,wary,125,1,0,0,inform,3,400,0
)";

constexpr const char* kExpectations = R"(exp_id,npc_id,kind,due,window_min,remind_lead_min,kept_by,broken_by,deed_kept,deed_broken,deed_missed
exp_1,npc_b,promise,1:19:00,120,60,deed_meet,deed_b,deed_k,deed_p,deed_p
)";

constexpr const char* kSchedule = R"(event_id,day,hour,foreshadow_lead_min
event_1,2,08:00,120
)";

constexpr const char* kTuning = R"({"kVictim": 150, "decayF": 8})";

constexpr const char* kLines = R"(line_id,npc_id,kind,lead,priority,cooldown_min,once,cond
line_00,npc_a,greet,any,1,0,0,
line_01,npc_a,greet,any,5,0,1,mem:deed=deed_a>=200
line_02,npc_a,greet,any,5,120,0,mem:tag=tag_d>=100
line_03,npc_b,remind,any,3,0,0,exp:exp_1=reminding
line_04,npc_a,talk,any,4,0,0,"stance>=warm;not_since:tag=tag_a,deed=deed_c"
line_05,npc_a,talk,0,2,0,0,since_deed:deed_a>=60;dead:npc_d
line_06,npc_a,bark,any,1,0,0,event:event_1<=120
line_07,npc_y,greet,any,1,0,0,mem:deed=deed_h
)";

// A promise broken by a secret deed.
constexpr const char* kSecretExp = R"(exp_id,npc_id,kind,due,window_min,remind_lead_min,kept_by,broken_by,deed_kept,deed_broken,deed_missed
exp_1,npc_b,promise,1:19:00,120,60,deed_meet,deed_s,deed_k,deed_p,deed_p
)";

struct Fx {
    NpcTables t;
    NpcLineTable lines;
    std::unique_ptr<NpcMemoryWorld> w = std::make_unique<NpcMemoryWorld>();

    explicit Fx(const char* exps = kExpectations) {
        NpcTableSources src{kTags, kDeeds, kNpcs, kTies, kConsequences, exps, kSchedule, kTuning};
        std::string err;
        const bool ok = importNpcTables(src, t, &err);
        INFO(err);
        REQUIRE(ok);
        const bool lok = importNpcLines(kLines, t, lines, &err);
        INFO(err);
        REQUIRE(lok);
        npcInit(*w, t);
    }
    std::uint16_t npc(const char* id) const { return static_cast<std::uint16_t>(t.npcIndex(id)); }
    std::uint16_t deed(const char* id) const { return static_cast<std::uint16_t>(t.deedIndex(id)); }
    int line(const char* id) const { return lines.lineIndex(id); }
    std::int32_t ax(const char* n, NpcAxis a) const { return npcAxis(*w, t, npc(n), a); }
    std::uint32_t record(const char* type, std::initializer_list<const char*> witnesses, const char* victim = nullptr,
                         std::uint8_t lead = kNoLead, std::uint16_t place = 1, std::uint16_t district = 0) {
        NpcDeed d;
        d.type = deed(type);
        d.victim = victim ? npc(victim) : kNoNpc;
        d.lead = lead;
        d.place = place;
        d.district = district;
        std::size_t i = 0;
        for (const char* x : witnesses) d.witnesses[i++] = npc(x);
        return npcRecordDeed(*w, t, d);
    }
    void at(std::uint32_t minute, std::uint16_t crowd = 1000) { npcCheckpoint(*w, t, minute, crowd); }
    int pick(const char* n, NpcLineKind k, std::uint8_t lead = 0) { return npcPickLine(*w, t, lines, npc(n), lead, k).line; }
};

std::string dump(const NpcMemoryWorld& w, const NpcTables& t, const NpcLineTable& lines) {
    const NpcMemorySave s = npcCaptureSave(w, t, lines);
    std::string o = std::to_string(s.now) + "/" + std::to_string(s.nextSeq) + "/" + std::to_string(s.droppedRumours) + "|";
    for (const auto& n : s.npcs) {
        o += n.id + ":";
        for (auto a : n.axes) o += std::to_string(a) + ",";
        o += std::to_string(n.axesTs) + ",";
        for (const auto& l : n.offsets)
            for (auto a : l) o += std::to_string(a) + ",";
        for (auto x : n.offsetsTs) o += std::to_string(x) + ",";
        for (auto x : n.stance) o += std::to_string(x) + ",";
        o += std::to_string(n.dead) + "," + std::to_string(n.deadAt) + "[";
        for (const auto& e : n.mem)
            o += std::to_string(e.seq) + "," + std::to_string(e.deedMinute) + "," + std::to_string(e.ts) + "," + std::to_string(e.learned) +
                 "," + e.type + "," + std::to_string(e.place) + "," + std::to_string(e.s0) + "," + std::to_string(e.hops) + "," +
                 std::to_string(e.count) + "," + std::to_string(e.lead) + "," + std::to_string(e.pinned) + "," + std::to_string(e.leads) + ";";
        o += "]{";
        for (const auto& ag : n.aggregates) o += ag.first + "=" + std::to_string(ag.second.count) + "@" + std::to_string(ag.second.lastMinute) + ";";
        o += "}<";
        for (auto x : n.seen) o += std::to_string(x) + ",";
        for (const auto& r : n.recent) o += r.first + "@" + std::to_string(r.second) + ",";
        o += ">";
    }
    o += "|";
    for (const auto& d : s.deeds) {
        o += std::to_string(d.seq) + "," + std::to_string(d.minute) + "," + d.type + "," + d.victim + "," + std::to_string(d.place) + "," +
             std::to_string(d.district) + "," + std::to_string(d.lead) + "," + std::to_string(d.flags);
        for (const auto& x : d.witnesses) o += "," + x;
        o += ";";
    }
    o += "|";
    for (const auto& r : s.rumours)
        o += std::to_string(r.seq) + "," + std::to_string(r.due) + "," + r.target + "," + std::to_string(r.hops) + "," + std::to_string(r.lead) +
             "," + std::to_string(r.closeness) + ";";
    o += "|";
    for (const auto& e : s.expectations) o += e.first + "=" + std::to_string(e.second) + ";";
    for (const auto& e : s.said) o += e.first + "@" + std::to_string(e.second) + ";";
    return o;
}

}  // namespace

TEST_CASE("npc memory: appraisal_witness_example") {
    Fx f;
    f.at(1000);
    const std::uint32_t seq = f.record("deed_a", {"npc_a"});
    REQUIRE(seq == 1u);
    CHECK(f.ax("npc_a", kAxisT) == 10);  // 20 - 10
    CHECK(f.ax("npc_a", kAxisF) == 6);
    CHECK(f.ax("npc_a", kAxisR) == -4);
    const NpcMemoryEntry* e = npcFindMemory(*f.w, f.npc("npc_a"), seq);
    REQUIRE(e != nullptr);
    CHECK(e->s0 == 500);
    CHECK(e->hops == static_cast<std::uint8_t>(NpcLearn::Witness));
}

TEST_CASE("npc memory: appraisal_zero_weights") {
    Fx f;
    f.at(1000);
    f.record("deed_z", {"npc_a"});
    CHECK(f.ax("npc_a", kAxisT) == 20);
    CHECK(f.ax("npc_a", kAxisF) == 0);
    CHECK(f.ax("npc_a", kAxisR) == 0);
    // Salience still comes from magnitude: 5 * 150 * 100 / 100.
    const NpcMemoryEntry* e = npcRecall(*f.w, f.t, f.npc("npc_a"), f.deed("deed_z"), -1);
    REQUIRE(e != nullptr);
    CHECK(e->s0 == 750);
}

TEST_CASE("npc memory: appraisal_max_clamps") {
    Fx f;
    f.at(1000);
    NpcDeed d;
    d.type = f.deed("deed_max");
    d.victim = f.npc("npc_x");
    const auto dp = npcAppraise(*f.w, f.t, f.npc("npc_x"), d, NpcLearn::Victim);
    CHECK(dp[0] == -90);  // S clamps at -12: 5 * -12 * 150 * 100 / 10000
    CHECK(dp[1] == 90);
    CHECK(dp[2] == -90);
    f.record("deed_max", {}, "npc_x");
    CHECK(f.ax("npc_x", kAxisT) == -90);
    CHECK(f.ax("npc_x", kAxisF) == 90);
    // A second hit saturates instead of overflowing the range.
    f.record("deed_max", {}, "npc_x", kNoLead, 2);
    CHECK(f.ax("npc_x", kAxisT) >= -100);
    CHECK(f.ax("npc_x", kAxisF) <= 100);
}

TEST_CASE("npc memory: saturation_near_cap") {
    Fx f;
    f.at(1000);
    const std::uint16_t y = f.npc("npc_y");
    f.w->npcs[y].axes[kAxisT] = 90;
    f.w->npcs[y].axesTs = 1000;
    NpcDeed d;
    d.type = f.deed("deed_h");
    CHECK(npcAppraise(*f.w, f.t, y, d, NpcLearn::Witness)[0] == 2);  // +20 at T=90 gives +2
    // Toward zero there is no damping.
    f.w->npcs[y].axes[kAxisT] = -90;
    CHECK(npcAppraise(*f.w, f.t, y, d, NpcLearn::Witness)[0] == 20);
    f.w->npcs[y].axes[kAxisT] = 90;
    f.record("deed_h", {"npc_y"});
    CHECK(f.ax("npc_y", kAxisT) == 92);
}

TEST_CASE("npc memory: repeat_damping_floor") {
    Fx f;
    f.at(1000);
    const std::uint16_t r = f.npc("npc_r");
    NpcDeed d;
    d.type = f.deed("deed_a");
    const int expect[] = {-10, -7, -5, -2, -2};  // Rk 100, 75, 50, 25, floor 25
    for (int i = 0; i < 5; ++i) {
        CHECK(npcAppraise(*f.w, f.t, r, d, NpcLearn::Witness)[0] == expect[i]);
        f.record("deed_a", {"npc_r"});
        f.at(1000u + 60u * static_cast<std::uint32_t>(i + 1));
    }
    // Past the 3-day window the damping is gone.
    f.at(1000u + 4u * 1440u);
    CHECK(npcAppraise(*f.w, f.t, r, d, NpcLearn::Witness)[0] == -10);
}

TEST_CASE("npc memory: repeat_floor_survives_other_types") {
    // Many learns of another type in between never push a type's own repeats out: Rk still reaches its floor.
    Fx f;
    f.at(1000);
    const std::uint16_t r = f.npc("npc_r");
    NpcDeed d;
    d.type = f.deed("deed_a");
    for (std::uint32_t i = 0; i < 3; ++i) {
        f.record("deed_a", {"npc_r"});
        f.at(1000u + 10u * (i + 1u));
    }
    for (std::uint32_t i = 0; i < 2u * kRecentMax; ++i) {
        f.record("deed_b", {"npc_r"});
        f.at(1100u + i);
    }
    CHECK(npcAppraise(*f.w, f.t, r, d, NpcLearn::Witness)[0] == -2);  // k 3: floor 25
    // And through a save and load.
    const NpcMemorySave save = npcCaptureSave(*f.w, f.t, f.lines);
    Fx g;
    npcApplySave(*g.w, g.t, g.lines, save);
    CHECK(npcAppraise(*g.w, g.t, r, d, NpcLearn::Witness)[0] == -2);
}

TEST_CASE("npc memory: init_resets_a_used_world") {
    Fx f;
    f.at(1000);
    const std::uint16_t r = f.npc("npc_r");
    NpcDeed d;
    d.type = f.deed("deed_a");
    f.record("deed_a", {"npc_r"});
    f.record("deed_a", {"npc_a"});
    REQUIRE(f.w->nextSeq > 1u);
    npcInit(*f.w, f.t);
    CHECK(f.w->now == 0u);
    CHECK(f.w->nextSeq == 1u);
    CHECK(f.w->rumourCount == 0u);
    CHECK(npcDeedAt(*f.w, 1) == nullptr);
    CHECK(f.w->npcs[r].memCount == 0u);
    CHECK(f.ax("npc_a", kAxisT) == 20);  // its baseline
    CHECK(npcAppraise(*f.w, f.t, r, d, NpcLearn::Witness)[0] == -10);  // no repeats left
}

TEST_CASE("npc memory: load_keeps_a_future_dated_deed_minute") {
    Fx f;
    f.at(1000);
    const std::uint32_t seq = f.record("deed_a", {"npc_a"});
    NpcMemorySave save = npcCaptureSave(*f.w, f.t, f.lines);
    REQUIRE(!save.deeds.empty());
    save.deeds[0].minute = save.now + 500;  // a deed timed ahead (a scheduled event), not damage
    Fx g;
    const NpcMemoryLoadReport rep = npcApplySave(*g.w, g.t, g.lines, save);
    CHECK(rep.clamped == 0);
    REQUIRE(npcDeedAt(*g.w, seq) != nullptr);
    CHECK(npcDeedAt(*g.w, seq)->minute == save.now + 500);
}

TEST_CASE("npc memory: decay_lands_on_baseline") {
    Fx f;
    f.at(1000);
    f.record("deed_a", {"npc_a"});
    CHECK(f.ax("npc_a", kAxisT) == 10);  // t = t_s
    f.at(1000 + 1440);                   // one day: T +2, F -8 (floors at 0), R +1
    CHECK(f.ax("npc_a", kAxisT) == 12);
    CHECK(f.ax("npc_a", kAxisF) == 0);
    CHECK(f.ax("npc_a", kAxisR) == -3);
    f.at(1000 + 100 * 1440);  // a long skip lands exactly on the baseline, never past it
    CHECK(f.ax("npc_a", kAxisT) == 20);
    CHECK(f.ax("npc_a", kAxisR) == 0);
}

TEST_CASE("npc memory: decay_monotonic_clock") {
    Fx f;
    f.at(npcMinute(1, 12.0));
    f.record("deed_a", {"npc_a"});
    f.at(npcMinute(2, 12.0));
    const std::int32_t T = f.ax("npc_a", kAxisT);
    CHECK(T == 12);
    // A cutscene sets the hour back: the clock holds and nothing grows back.
    f.at(npcMinute(2, 6.0));
    npcAdvance(*f.w, npcMinute(1, 0.0));
    CHECK(f.w->now == npcMinute(2, 12.0));
    CHECK(f.ax("npc_a", kAxisT) == T);
}

TEST_CASE("npc memory: salience_decay_example") {
    Fx f;
    f.at(1000);
    const std::uint32_t seq = f.record("deed_a", {"npc_a"});
    const NpcMemoryEntry* e = npcFindMemory(*f.w, f.npc("npc_a"), seq);
    REQUIRE(e != nullptr);
    CHECK(npcSalience(*f.w, f.t, f.npc("npc_a"), *e) == 500);
    f.at(1000 + 100 * 60);
    CHECK(npcSalience(*f.w, f.t, f.npc("npc_a"), *e) == 200);  // still remembered at 100 game hours
    CHECK(npcSalience(*f.w, f.t, f.npc("npc_a"), *e) >= f.t.tuning.recallBar);
    f.at(1000 + 101 * 60);
    CHECK(npcSalience(*f.w, f.t, f.npc("npc_a"), *e) == 197);
    CHECK(npcSalience(*f.w, f.t, f.npc("npc_a"), *e) < f.t.tuning.recallBar);
}

TEST_CASE("npc memory: reinforcement_caps") {
    Fx f;
    f.at(1000);
    for (int i = 0; i < 4; ++i) {
        f.record("deed_max", {}, "npc_x");
        f.at(1000u + 10u * static_cast<std::uint32_t>(i + 1));
    }
    const NpcMemoryEntry* e = npcRecall(*f.w, f.t, f.npc("npc_x"), f.deed("deed_max"), -1);
    REQUIRE(e != nullptr);
    CHECK(e->s0 == 1000);
    CHECK(e->count == 4);
    CHECK(f.w->npcs[f.npc("npc_x")].memCount == 1);
    // The floor: m 1 at K 30 gives 45, raised to 50.
    f.t.tuning.kWitness = 30;
    const std::uint32_t small = f.record("deed_meet", {"npc_c"});
    const NpcMemoryEntry* m = npcFindMemory(*f.w, f.npc("npc_c"), small);
    REQUIRE(m != nullptr);
    CHECK(m->s0 == 50);
}

TEST_CASE("npc memory: pinned_never_fades") {
    Fx f;
    f.at(1000);
    const std::uint32_t seq = f.record("deed_p", {}, "npc_b");
    const NpcMemoryEntry* e = npcFindMemory(*f.w, f.npc("npc_b"), seq);
    REQUIRE(e != nullptr);
    CHECK(e->pinned);
    const std::int32_t s0 = e->s0;
    f.at(1000 + 365 * 1440);
    CHECK(npcSalience(*f.w, f.t, f.npc("npc_b"), *e) == s0);
    // A seventh pin unpins the oldest, which then starts to fade from now.
    for (std::uint16_t p = 2; p <= 7; ++p) {
        f.record("deed_p", {}, "npc_b", kNoLead, p);
        f.at(f.w->now + 1);
    }
    std::uint32_t pinned = 0;
    for (std::uint32_t i = 0; i < f.w->npcs[f.npc("npc_b")].memCount; ++i) pinned += f.w->npcs[f.npc("npc_b")].mem[i].pinned;
    CHECK(pinned == kPinnedMax);
    const NpcMemoryEntry* first = npcFindMemory(*f.w, f.npc("npc_b"), seq);
    REQUIRE(first != nullptr);
    CHECK_FALSE(first->pinned);
}

TEST_CASE("npc memory: eviction_lowest_then_oldest") {
    Fx f;
    f.at(1000);
    const std::uint16_t a = f.npc("npc_a");
    // Sixteen equal memories (750 each) at one minute, then a weaker one replaces the lowest seq.
    for (std::uint16_t p = 1; p <= 16; ++p) f.record("deed_z", {"npc_a"}, nullptr, kNoLead, p);
    CHECK(f.w->npcs[a].memCount == kMemPerNpc);
    f.record("deed_z", {"npc_a"}, nullptr, kNoLead, 17);  // all equal: the oldest (seq 1) goes
    CHECK(npcFindMemory(*f.w, a, 1) == nullptr);
    CHECK(npcFindMemory(*f.w, a, 2) != nullptr);
    // A weak memory is evicted before older strong ones.
    const std::uint32_t weak = f.record("deed_c", {"npc_a"}, nullptr, kNoLead, 18);  // evicts seq 2
    CHECK(npcFindMemory(*f.w, a, weak) != nullptr);
    f.record("deed_z", {"npc_a"}, nullptr, kNoLead, 19);
    CHECK(npcFindMemory(*f.w, a, weak) == nullptr);
    CHECK(npcFindMemory(*f.w, a, 3) != nullptr);
}

TEST_CASE("npc memory: eviction_folds_aggregate") {
    Fx f;
    f.at(1000);
    const std::uint16_t a = f.npc("npc_a");
    f.record("deed_c", {"npc_a"}, nullptr, kNoLead, 100);  // weakest, tag_d
    for (std::uint16_t p = 1; p <= 15; ++p) f.record("deed_z", {"npc_a"}, nullptr, kNoLead, p);
    f.at(1500);
    f.record("deed_z", {"npc_a"}, nullptr, kNoLead, 16);
    const int tagD = f.t.tagIndex("tag_d");
    CHECK(f.w->npcs[a].aggregates[static_cast<std::size_t>(tagD)].count == 1);
    CHECK(f.w->npcs[a].aggregates[static_cast<std::size_t>(tagD)].lastMinute == 1000);
    CHECK(npcRecall(*f.w, f.t, a, f.deed("deed_c"), -1) == nullptr);
}

TEST_CASE("npc memory: stance_warm_hysteresis") {
    CHECK(npcStanceStep(NpcStance::Neutral, 25, 0, 0) == NpcStance::Warm);
    CHECK(npcStanceStep(NpcStance::Warm, 20, 0, 0) == NpcStance::Warm);
    CHECK(npcStanceStep(NpcStance::Warm, 14, 0, 0) == NpcStance::Neutral);
    CHECK(npcStanceStep(NpcStance::Neutral, 20, 0, 0) == NpcStance::Neutral);
    // In the world: npc_a helped twice turns Warm, prices drop.
    Fx f;
    f.at(1000);
    const std::uint16_t a = f.npc("npc_a");
    CHECK(npcStance(*f.w, f.t, a, kNoLead) == NpcStance::Neutral);
    f.record("deed_c", {}, "npc_a");  // victim, benefit: S_T = 1 + 6 = 7, ΔT 21 damped to 16 -> T 36
    CHECK(npcStance(*f.w, f.t, a, kNoLead) == NpcStance::Warm);
    CHECK(npcConsequence(*f.w, f.t, a, kNoLead).pricePct == 90);
}

TEST_CASE("npc memory: stance_loyal_holds") {
    CHECK(npcStanceStep(NpcStance::Warm, 60, 0, 50) == NpcStance::Loyal);
    CHECK(npcStanceStep(NpcStance::Loyal, 55, 0, 45) == NpcStance::Loyal);
    CHECK(npcStanceStep(NpcStance::Loyal, 49, 0, 50) == NpcStance::Warm);
    CHECK(npcStanceStep(NpcStance::Loyal, 55, 0, 39) == NpcStance::Warm);
}

TEST_CASE("npc memory: stance_hostile_cowed_to_wary") {
    CHECK(npcStanceStep(NpcStance::Neutral, -50, 0, 0) == NpcStance::Hostile);
    CHECK(npcStanceStep(NpcStance::Hostile, -45, 65, 0) == NpcStance::Hostile);
    CHECK(npcStanceStep(NpcStance::Hostile, -60, 70, 0) == NpcStance::Wary);
    CHECK(npcStanceStep(NpcStance::Wary, 0, 45, 0) == NpcStance::Wary);
    CHECK(npcStanceStep(NpcStance::Wary, 0, 39, 0) == NpcStance::Neutral);
}

TEST_CASE("npc memory: lead_offset_attributable_only") {
    Fx f;
    f.at(1000);
    const std::uint16_t a = f.npc("npc_a");
    f.record("deed_a", {"npc_a"}, nullptr, 0);
    CHECK(npcOffset(*f.w, f.t, a, 0, kAxisT) == -5);  // trunc(-10 / 2)
    CHECK(npcOffset(*f.w, f.t, a, 0, kAxisF) == 3);
    CHECK(npcOffset(*f.w, f.t, a, 0, kAxisR) == -2);
    CHECK(npcOffset(*f.w, f.t, a, 1, kAxisT) == 0);
    CHECK(npcEffective(*f.w, f.t, a, 0)[0] == 5);
    CHECK(npcEffective(*f.w, f.t, a, 1)[0] == 10);
    // A deed that is not attributable moves the party only.
    f.record("deed_h", {"npc_y"}, nullptr, 1);
    CHECK(npcOffset(*f.w, f.t, f.npc("npc_y"), 1, kAxisT) == 0);
    CHECK(f.ax("npc_y", kAxisT) == 20);
    // Offsets decay at twice the axis rate.
    f.at(1000 + 1440);
    CHECK(npcOffset(*f.w, f.t, a, 0, kAxisT) == -1);
}

TEST_CASE("npc memory: hearsay_two_hops_loss") {
    Fx f;
    f.at(1000);
    const std::uint32_t seq = f.record("deed_a", {"npc_a"}, nullptr, 0);
    CHECK(npcFindMemory(*f.w, f.npc("npc_b"), seq) == nullptr);
    f.at(1059);
    CHECK(npcFindMemory(*f.w, f.npc("npc_b"), seq) == nullptr);  // not before the delay
    f.at(1060);
    const NpcMemoryEntry* b = npcFindMemory(*f.w, f.npc("npc_b"), seq);
    REQUIRE(b != nullptr);
    CHECK(b->hops == static_cast<std::uint8_t>(NpcLearn::Hop1));
    CHECK(b->lead == 0);                   // closeness 80 keeps the lead
    CHECK(f.ax("npc_b", kAxisT) == -2);    // 2 * -2 * 60 * 100 / 10000
    CHECK(npcOffset(*f.w, f.t, f.npc("npc_b"), 0, kAxisT) == -1);
    f.at(1120);
    const NpcMemoryEntry* c = npcFindMemory(*f.w, f.npc("npc_c"), seq);
    REQUIRE(c != nullptr);
    CHECK(c->hops == static_cast<std::uint8_t>(NpcLearn::Hop2));
    CHECK(c->lead == kNoLead);  // closeness 65 drops it
    CHECK(npcOffset(*f.w, f.t, f.npc("npc_c"), 0, kAxisT) == 0);
    f.at(5000);
    CHECK(npcFindMemory(*f.w, f.npc("npc_d"), seq) == nullptr);  // no third hop
    for (std::uint32_t i = 0; i < f.w->rumourCount; ++i) CHECK(f.w->rumours[i].seq != seq);
}

TEST_CASE("npc memory: hearsay_night_crowd_gate") {
    {
        Fx f;
        f.at(1000);
        const std::uint32_t seq = f.record("deed_a", {}, nullptr, kNoLead, 1, 2);  // unwitnessed, district 2
        f.at(1060, 800);  // 2 * 800 = 1600 >= 1500
        CHECK(npcFindMemory(*f.w, f.npc("npc_c"), seq) != nullptr);
        CHECK(npcFindMemory(*f.w, f.npc("npc_d"), seq) != nullptr);
        CHECK(npcFindMemory(*f.w, f.npc("npc_a"), seq) == nullptr);  // other district
    }
    {
        Fx f;
        f.at(1000);
        const std::uint32_t seq = f.record("deed_a", {}, nullptr, kNoLead, 1, 2);
        f.at(1030, 1000);  // before the street delay
        CHECK(npcFindMemory(*f.w, f.npc("npc_c"), seq) == nullptr);
        f.at(1060, 500);  // a thin night crowd: 1000 < 1500, the deed waits
        CHECK(npcFindMemory(*f.w, f.npc("npc_c"), seq) == nullptr);
        f.at(1200, 600);
        CHECK(npcFindMemory(*f.w, f.npc("npc_c"), seq) == nullptr);
        f.at(1500, 1000);  // the day crowd passes it on
        CHECK(npcFindMemory(*f.w, f.npc("npc_c"), seq) != nullptr);
        CHECK(npcFindMemory(*f.w, f.npc("npc_d"), seq) != nullptr);
    }
    {
        Fx f;  // still waiting when its ring slot is reused: lost
        f.at(1000);
        const std::uint32_t seq = f.record("deed_a", {}, nullptr, kNoLead, 1, 2);
        f.at(1060, 500);
        for (std::uint32_t i = 0; i < kDeedRing; ++i) f.record("deed_z", {"npc_x"});
        f.at(1500, 1000);
        CHECK(npcFindMemory(*f.w, f.npc("npc_c"), seq) == nullptr);
    }
}

TEST_CASE("npc memory: secret_no_hearsay_until_reveal") {
    Fx f;
    f.at(1000);
    const std::uint32_t seq = f.record("deed_s", {}, "npc_a");
    CHECK(npcFindMemory(*f.w, f.npc("npc_a"), seq) == nullptr);
    CHECK(f.w->rumourCount == 0);
    f.at(5000);
    CHECK(npcFindMemory(*f.w, f.npc("npc_b"), seq) == nullptr);
    REQUIRE(npcReveal(*f.w, f.t, seq, f.npc("npc_a")));
    const NpcMemoryEntry* e = npcFindMemory(*f.w, f.npc("npc_a"), seq);
    REQUIRE(e != nullptr);
    CHECK(e->s0 == 620);  // K 100, self tie 100: S -7/3/-6 -> Δ -14/6/-12, 300 + 10 * 32
    f.at(5060);
    CHECK(npcFindMemory(*f.w, f.npc("npc_b"), seq) != nullptr);
    // A witness clears the secret.
    const std::uint32_t seen = f.record("deed_s", {"npc_y"}, "npc_a", kNoLead, 9);
    CHECK(npcFindMemory(*f.w, f.npc("npc_a"), seen) != nullptr);
}

TEST_CASE("npc memory: late_learn_uses_learn_time") {
    Fx f;
    f.at(1000);
    const std::uint32_t seq = f.record("deed_a", {"npc_a"});
    f.at(1600);  // npc_b hears it 10 hours late (delivered at the first checkpoint after the delay)
    const NpcMemoryEntry* b = npcFindMemory(*f.w, f.npc("npc_b"), seq);
    REQUIRE(b != nullptr);
    CHECK(b->deedMinute == 1000);
    CHECK(b->learned == 1600);
    CHECK(npcSalience(*f.w, f.t, f.npc("npc_b"), *b) == b->s0);  // salience starts at the learn time
    // since_deed counts from the deed: line_05 needs 60 minutes since deed_a and npc_d dead.
    npcSetDead(*f.w, f.npc("npc_d"));
    CHECK(f.pick("npc_a", NpcLineKind::Talk, 0) == f.line("line_05"));
}

TEST_CASE("npc memory: ring_wrap_seq_mismatch") {
    Fx f;
    f.at(1000);
    const std::uint32_t seq = f.record("deed_a", {"npc_a"});
    REQUIRE(f.w->rumourCount == 1);
    for (std::uint32_t i = 0; i < kDeedRing; ++i) f.record("deed_z", {"npc_x"});
    CHECK(npcDeedAt(*f.w, seq) == nullptr);  // slot reused by seq 257
    CHECK(npcDeedAt(*f.w, seq + kDeedRing) != nullptr);
    f.at(2000);
    CHECK(npcFindMemory(*f.w, f.npc("npc_b"), seq) == nullptr);  // the pruned deed is lost
    CHECK(f.w->rumourCount == 0);
}

TEST_CASE("npc memory: dead_npc_frozen") {
    Fx f;
    f.at(1000);
    const std::uint16_t a = f.npc("npc_a");
    f.record("deed_a", {"npc_a"});
    npcSetDead(*f.w, a);
    f.at(1000 + 10 * 1440);
    CHECK(f.ax("npc_a", kAxisT) == 10);  // frozen at death
    CHECK(f.ax("npc_a", kAxisF) == 6);
    const std::uint32_t later = f.record("deed_a", {"npc_a"}, nullptr, kNoLead, 5);
    CHECK(npcFindMemory(*f.w, a, later) == nullptr);
    const NpcMemoryEntry* kept = npcRecall(*f.w, f.t, a, f.deed("deed_a"), -1);  // the store is kept
    REQUIRE(kept != nullptr);
    CHECK(npcSalience(*f.w, f.t, a, *kept) == kept->s0);  // salience frozen at death too
    // A dead NPC neither receives nor passes on.
    Fx g;
    g.at(1000);
    npcSetDead(*g.w, g.npc("npc_b"));
    const std::uint32_t s2 = g.record("deed_a", {"npc_a"});
    g.at(1060);
    g.at(1200);
    CHECK(npcFindMemory(*g.w, g.npc("npc_b"), s2) == nullptr);
    CHECK(npcFindMemory(*g.w, g.npc("npc_c"), s2) == nullptr);
}

TEST_CASE("npc memory: expectation_kept_broken_missed") {
    const std::uint32_t due = npcMinute(1, 19.0);
    {
        Fx f;
        f.at(due - 90);
        CHECK(f.w->exp[0] == NpcExpState::Pending);
        f.at(due - 60);
        CHECK(f.w->exp[0] == NpcExpState::Reminding);
        CHECK(f.pick("npc_b", NpcLineKind::Remind) == f.line("line_03"));
        f.record("deed_meet", {});
        CHECK(f.w->exp[0] == NpcExpState::Kept);
        CHECK(npcRecall(*f.w, f.t, f.npc("npc_b"), f.deed("deed_k"), -1) != nullptr);
    }
    {
        Fx f;
        f.at(due - 30);
        f.record("deed_b", {});
        CHECK(f.w->exp[0] == NpcExpState::Broken);
        const NpcMemoryEntry* e = npcRecall(*f.w, f.t, f.npc("npc_b"), f.deed("deed_p"), -1);
        REQUIRE(e != nullptr);
        CHECK(e->pinned);
    }
    {
        Fx f;
        f.at(due - 60);
        f.at(due + 120);
        CHECK(f.w->exp[0] == NpcExpState::Reminding);  // the window is still open
        f.at(due + 121);
        CHECK(f.w->exp[0] == NpcExpState::Missed);
        const NpcMemoryEntry* e = npcRecall(*f.w, f.t, f.npc("npc_b"), f.deed("deed_p"), -1);
        REQUIRE(e != nullptr);
        CHECK(e->pinned);
        f.record("deed_meet", {});  // too late to keep
        CHECK(f.w->exp[0] == NpcExpState::Missed);
    }
}

TEST_CASE("npc memory: expectation_sleep_skips_reminder") {
    const std::uint32_t due = npcMinute(1, 19.0);
    Fx f;
    f.at(due - 300);
    CHECK(f.pick("npc_b", NpcLineKind::Remind) < 0);
    f.at(due + 600);  // slept through the evening
    CHECK(f.w->exp[0] == NpcExpState::Missed);
    CHECK(f.pick("npc_b", NpcLineKind::Remind) < 0);
}

TEST_CASE("npc memory: line_pick_priority_salience") {
    Fx f;
    f.at(1000);
    CHECK(f.pick("npc_a", NpcLineKind::Greet) == f.line("line_00"));
    f.record("deed_c", {"npc_a"});  // tag_d memory, S0 350, line_02 qualifies
    f.record("deed_a", {"npc_a"}, nullptr, kNoLead, 2);  // S0 500
    const NpcLinePick p = npcPickLine(*f.w, f.t, f.lines, f.npc("npc_a"), 0, NpcLineKind::Greet);
    CHECK(p.line == f.line("line_01"));
    CHECK(p.score == 5 * 1000 + 500);
    // Redemption: line_04 needs Warm and no deed_c since the last tag_a deed.
    f.w->npcs[f.npc("npc_a")].axes[kAxisT] = 60;
    f.w->npcs[f.npc("npc_a")].axesTs = f.w->now;
    // Same minute: recorded order decides. deed_c came before deed_a, so not redeemed yet.
    CHECK(f.pick("npc_a", NpcLineKind::Talk, 1) == f.line("line_04"));
    f.record("deed_c", {"npc_a"});  // a deed_c after it in the same minute redeems
    CHECK(f.pick("npc_a", NpcLineKind::Talk, 1) < 0);
    // A foreshadow bark only inside the lead time before the event.
    CHECK(f.pick("npc_a", NpcLineKind::Bark) < 0);
    f.at(npcMinute(2, 6.5));
    CHECK(f.pick("npc_a", NpcLineKind::Bark) == f.line("line_06"));
}

TEST_CASE("npc memory: line_once_and_cooldown") {
    Fx f;
    f.at(1000);
    f.record("deed_a", {"npc_a"});
    CHECK(f.pick("npc_a", NpcLineKind::Greet) == f.line("line_01"));
    CHECK(f.pick("npc_a", NpcLineKind::Greet) == f.line("line_00"));  // once: never again
    f.record("deed_c", {"npc_a"}, nullptr, kNoLead, 3);
    CHECK(f.pick("npc_a", NpcLineKind::Greet) == f.line("line_02"));
    f.at(1060);
    CHECK(f.pick("npc_a", NpcLineKind::Greet) == f.line("line_00"));  // cooling down
    f.at(1120);
    CHECK(f.pick("npc_a", NpcLineKind::Greet) == f.line("line_02"));
}

TEST_CASE("npc memory: save_roundtrip_ids") {
    Fx f;
    f.at(npcMinute(1, 17.0));
    f.record("deed_a", {"npc_a"}, nullptr, 0);
    f.record("deed_s", {}, "npc_b");
    f.record("deed_a", {}, nullptr, kNoLead, 4, 2);
    f.at(npcMinute(1, 18.0) + 5);
    f.pick("npc_a", NpcLineKind::Greet);
    const NpcMemorySave save = npcCaptureSave(*f.w, f.t, f.lines);
    Fx g;
    const NpcMemoryLoadReport rep = npcApplySave(*g.w, g.t, g.lines, save);
    CHECK(rep.formatOk);
    CHECK(rep.unknownIds.empty());
    CHECK(rep.clamped == 0);
    CHECK(dump(*g.w, g.t, g.lines) == dump(*f.w, f.t, f.lines));
    // A reload replays exactly.
    for (Fx* x : {&f, &g}) {
        x->at(npcMinute(1, 19.0));
        x->record("deed_meet", {"npc_c"});
        x->at(npcMinute(2, 9.0), 700);
        npcReveal(*x->w, x->t, 2, x->npc("npc_b"));
        x->pick("npc_a", NpcLineKind::Greet);
    }
    CHECK(dump(*g.w, g.t, g.lines) == dump(*f.w, f.t, f.lines));
}

TEST_CASE("npc memory: save_unknown_id_reported") {
    Fx f;
    f.at(1000);
    f.record("deed_a", {"npc_a"});
    f.pick("npc_a", NpcLineKind::Greet);
    NpcMemorySave save = npcCaptureSave(*f.w, f.t, f.lines);
    NpcMemorySaveNpc gone;
    gone.id = "npc_gone";
    save.npcs.push_back(gone);
    save.npcs[0].mem.push_back(NpcMemorySaveEntry{});
    save.npcs[0].mem.back().type = "deed_gone";
    save.said.emplace_back("line_gone", 5u);
    save.expectations.emplace_back("exp_gone", std::uint8_t{0});
    save.npcs[0].axes[0] = 500;
    Fx g;
    const NpcMemoryLoadReport rep = npcApplySave(*g.w, g.t, g.lines, save);
    CHECK(rep.formatOk);
    CHECK(rep.unknownIds.size() == 4);
    CHECK(rep.clamped == 1);
    CHECK(g.ax("npc_a", kAxisT) == 100);
    CHECK(npcRecall(*g.w, g.t, g.npc("npc_a"), g.deed("deed_a"), -1) != nullptr);
    save.format = 99;
    Fx h;
    CHECK_FALSE(npcApplySave(*h.w, h.t, h.lines, save).formatOk);
}

TEST_CASE("npc memory: record, checkpoint and line pick allocate nothing") {
    Fx f;
    f.at(1000);
    REQUIRE(ftcore_test::allocHookInstalled());
    const auto before = ftcore_test::allocStats().count;
    for (std::uint32_t i = 0; i < 300; ++i) {
        f.record(i % 2 ? "deed_a" : "deed_c", {"npc_a", "npc_y"}, i % 3 ? nullptr : "npc_b", static_cast<std::uint8_t>(i % 3),
                 static_cast<std::uint16_t>(i % 20), static_cast<std::uint16_t>(i % 3));
        f.at(1000 + 30 * i, static_cast<std::uint16_t>(400 + i));
        f.pick("npc_a", NpcLineKind::Greet);
        f.pick("npc_b", NpcLineKind::Remind);
    }
    CHECK(ftcore_test::allocStats().count == before);
}

TEST_CASE("npc memory: direct learners before rumours") {
    Fx f;
    f.at(1000);
    const std::uint16_t a = f.npc("npc_a"), b = f.npc("npc_b");
    const std::uint32_t seq = f.record("deed_a", {"npc_b", "npc_a"});
    for (std::uint32_t i = 0; i < f.w->rumourCount; ++i) {
        CHECK(f.w->rumours[i].target != a);
        CHECK(f.w->rumours[i].target != b);
    }
    f.at(2000);
    CHECK(npcFindMemory(*f.w, a, seq)->count == 1);
}

TEST_CASE("npc memory: double_learn_after_reinforcement") {
    Fx f;
    f.at(1000);
    const std::uint16_t b = f.npc("npc_b");
    const std::uint32_t seq = f.record("deed_a", {"npc_a"});  // a rumour to npc_b is queued
    REQUIRE(f.w->rumourCount == 1);
    REQUIRE(npcReveal(*f.w, f.t, seq, b));  // npc_b hears it first another way
    f.record("deed_a", {"npc_b"});          // same type and place: reinforces, the entry now holds the new seq
    const NpcMemoryEntry* e = npcRecall(*f.w, f.t, b, f.deed("deed_a"), -1);
    REQUIRE(e != nullptr);
    REQUIRE(e->seq != seq);
    const std::int32_t T = f.ax("npc_b", kAxisT);
    f.at(1060);  // the old rumour arrives
    CHECK(e->count == 2);
    CHECK(f.ax("npc_b", kAxisT) == T);
    // Eviction does not reopen it either.
    for (std::uint16_t p = 2; p <= 20; ++p) f.record("deed_z", {"npc_b"}, nullptr, kNoLead, p);
    CHECK(npcFindMemory(*f.w, b, seq) == nullptr);
    CHECK(npcReveal(*f.w, f.t, seq, f.npc("npc_x")));  // others can still learn it
    const std::uint32_t before = f.w->npcs[b].memCount;
    NpcRumour r;
    r.seq = seq;
    r.due = f.w->now;
    r.target = b;
    r.hops = 1;
    r.closeness = 80;
    f.w->rumours[f.w->rumourCount++] = r;
    f.at(f.w->now + 1);
    CHECK(npcFindMemory(*f.w, b, seq) == nullptr);
    CHECK(f.w->npcs[b].memCount == before);
}

TEST_CASE("npc memory: store_full_of_pins") {
    Fx f;
    f.at(1000);
    const std::uint16_t b = f.npc("npc_b");
    std::uint32_t pins[6] = {};
    for (std::uint16_t p = 1; p <= 6; ++p) pins[p - 1] = f.record("deed_p", {}, "npc_b", kNoLead, p);
    for (std::uint16_t p = 10; p <= 40; ++p) f.record("deed_z", {}, "npc_b", kNoLead, p);
    CHECK(f.w->npcs[b].memCount == kMemPerNpc);
    for (std::uint32_t seq : pins) CHECK(npcFindMemory(*f.w, b, seq) != nullptr);  // pins are never evicted
    // A forged store with every entry pinned still takes a new memory: the weakest, oldest goes.
    NpcState& st = f.w->npcs[b];
    for (std::uint32_t i = 0; i < st.memCount; ++i) st.mem[i].pinned = true;
    st.mem[3].s0 = 60;
    const std::uint32_t weakSeq = st.mem[3].seq;
    const std::uint32_t fresh = f.record("deed_z", {}, "npc_b", kNoLead, 99);
    CHECK(st.memCount == kMemPerNpc);
    CHECK(npcFindMemory(*f.w, b, fresh) != nullptr);
    CHECK(npcFindMemory(*f.w, b, weakSeq) == nullptr);
}

TEST_CASE("npc memory: kept_at_due_plus_window") {
    const std::uint32_t due = npcMinute(1, 19.0);
    Fx f;
    f.at(due - 60);
    f.at(due + 120);
    REQUIRE(f.w->exp[0] == NpcExpState::Reminding);
    f.record("deed_meet", {});  // the last minute of the window still counts
    CHECK(f.w->exp[0] == NpcExpState::Kept);
    f.at(due + 500);
    CHECK(f.w->exp[0] == NpcExpState::Kept);
}

TEST_CASE("npc memory: repeat_count_window_edge") {
    {
        Fx f;
        f.at(1000);
        const std::uint16_t r = f.npc("npc_r");
        NpcDeed d;
        d.type = f.deed("deed_a");
        f.record("deed_a", {"npc_r"});
        f.at(1000 + 4320 - 1);
        CHECK(npcAppraise(*f.w, f.t, r, d, NpcLearn::Witness)[0] == -7);  // k 1
        f.at(1000 + 4320);
        CHECK(npcAppraise(*f.w, f.t, r, d, NpcLearn::Witness)[0] == -10);  // k 0
    }
    {
        // Only learn events inside the window count, not the lifetime count of a reinforced entry.
        Fx f;
        const std::uint16_t r = f.npc("npc_r");
        NpcDeed d;
        d.type = f.deed("deed_a");
        for (std::uint32_t m : {1000u, 3000u, 5000u}) {
            f.at(m);
            f.record("deed_a", {"npc_r"});
        }
        REQUIRE(npcRecall(*f.w, f.t, r, d.type, -1)->count == 3);
        f.at(1000 + 4320 + 1);
        CHECK(npcAppraise(*f.w, f.t, r, d, NpcLearn::Witness)[0] == -5);  // k 2
    }
}

TEST_CASE("npc memory: expectation_deeds_hearsay_only") {
    const std::uint32_t due = npcMinute(1, 19.0);
    Fx f;
    f.at(due - 30);
    f.record("deed_b", {});
    REQUIRE(f.w->exp[0] == NpcExpState::Broken);
    const NpcMemoryEntry* e = npcRecall(*f.w, f.t, f.npc("npc_b"), f.deed("deed_p"), -1);
    REQUIRE(e != nullptr);
    const NpcDeed* d = npcDeedAt(*f.w, e->seq);
    REQUIRE(d != nullptr);
    CHECK((d->flags & 2u) == 0u);  // never queued for the street
    f.at(due + 60);
    CHECK(npcFindMemory(*f.w, f.npc("npc_a"), e->seq) != nullptr);  // hearsay through the tie
}

TEST_CASE("npc memory: secret_broken_promise_reaches_party_only") {
    const std::uint32_t due = npcMinute(1, 19.0);
    Fx f(kSecretExp);
    f.at(due - 30);
    const std::uint32_t seq = f.record("deed_s", {}, "npc_a");  // secret, unwitnessed
    REQUIRE(f.w->exp[0] == NpcExpState::Broken);
    CHECK(npcFindMemory(*f.w, f.npc("npc_a"), seq) == nullptr);
    const NpcMemoryEntry* e = npcRecall(*f.w, f.t, f.npc("npc_b"), f.deed("deed_p"), -1);
    REQUIRE(e != nullptr);  // the party it was made to knows
    const std::uint32_t broken = e->seq;
    CHECK(f.w->rumourCount == 0);
    f.at(due + 600);
    CHECK(npcFindMemory(*f.w, f.npc("npc_a"), broken) == nullptr);  // and it goes no further
    REQUIRE(npcReveal(*f.w, f.t, broken, f.npc("npc_b")));      // until revealed
    f.at(due + 660);
    CHECK(npcFindMemory(*f.w, f.npc("npc_a"), broken) != nullptr);
    CHECK(npcRecall(*f.w, f.t, f.npc("npc_b"), f.deed("deed_p"), -1)->count == 1);
}

TEST_CASE("npc memory: recall_bar_and_guards") {
    Fx f;
    f.at(1000);
    const std::uint16_t y = f.npc("npc_y");
    f.record("deed_h", {"npc_y"});
    CHECK(npcRemembers(*f.w, f.t, y, f.deed("deed_h"), -1));
    CHECK(f.pick("npc_y", NpcLineKind::Greet) == f.line("line_07"));  // mem:deed=X uses the recall bar
    f.at(1000 + 201 * 60);  // 800 falls below 200 after 200 game hours
    CHECK_FALSE(npcRemembers(*f.w, f.t, y, f.deed("deed_h"), -1));
    CHECK(f.pick("npc_y", NpcLineKind::Greet) < 0);
    CHECK(npcRecall(*f.w, f.t, y, -1, 32) == nullptr);
    CHECK(npcRecall(*f.w, f.t, y, -1, 1000) == nullptr);
    CHECK(npcMinute(1, -5.0) == 1440u);
    CHECK(npcMinute(1, std::nan("")) == 1440u);
    CHECK(npcMinute(0xFFFFFFFFu, 3.0) == 0xFFFFFFFFu);
    CHECK(npcMinute(2, 1.5) == 2u * 1440u + 90u);
}

TEST_CASE("npc memory: time_skips_near_uint32_limit") {
    constexpr std::uint32_t kEnd = 0xFFFFFFFFu;
    Fx f;
    f.at(kEnd - 10);
    CHECK(f.w->exp[0] == NpcExpState::Missed);  // due + window does not wrap
    const std::uint32_t street = f.record("deed_a", {}, nullptr, kNoLead, 1, 2);
    const std::uint32_t seq = f.record("deed_a", {"npc_a"}, nullptr, kNoLead, 2);
    REQUIRE(f.w->rumourCount > 0);
    for (std::uint32_t i = 0; i < f.w->rumourCount; ++i) CHECK(f.w->rumours[i].due == kEnd);  // saturates, never in the past
    f.at(kEnd - 1, 1000);
    CHECK(npcFindMemory(*f.w, f.npc("npc_c"), street) == nullptr);  // the street delay does not wrap
    CHECK(npcFindMemory(*f.w, f.npc("npc_b"), seq) == nullptr);
    f.at(kEnd, 1000);
    CHECK(npcFindMemory(*f.w, f.npc("npc_b"), seq) != nullptr);
    CHECK(f.pick("npc_a", NpcLineKind::Greet) == f.line("line_01"));
    CHECK(f.w->lastSaid[static_cast<std::size_t>(f.line("line_01"))] == kEnd);
    CHECK(f.pick("npc_a", NpcLineKind::Greet) == f.line("line_00"));  // once still holds
    f.at(kEnd);
    CHECK(f.w->now == kEnd);
}

TEST_CASE("npc memory: tuning_clamps_and_table_caps") {
    {
        NpcTables t;
        NpcTableSources src{kTags, kDeeds, kNpcs, kTies, "", "", "",
                            R"({"kVictim": 999, "rkFloor": 0, "recallBar": 5000, "decayF": -3, "stayGap": 1, "spreadThreshold": 9999})"};
        std::string err;
        REQUIRE(importNpcTables(src, t, &err));
        CHECK(t.tuning.kVictim == 200);
        CHECK(t.tuning.rkFloor == 10);
        CHECK(t.tuning.recallBar == 400);
        CHECK(t.tuning.decayPerDay[1] == 0);
        CHECK(t.tuning.stayGap == 5);
        CHECK(t.tuning.spreadThreshold == 250);
        NpcTableSources bad{kTags, kDeeds, kNpcs, kTies, "", "", "", "[1, 2]"};
        CHECK_FALSE(importNpcTables(bad, t, &err));
    }
    auto fails = [](NpcTableSources src) {
        NpcTables t;
        std::string err;
        return !importNpcTables(src, t, &err) && !err.empty();
    };
    std::string deeds = "deed_id,tags,magnitude,polarity,secret,pin,attributable\n";
    for (int i = 0; i <= 128; ++i) deeds += "deed_" + std::to_string(i) + ",,1,0,0,0,0\n";
    CHECK(fails({kTags, deeds, "", "", "", "", "", ""}));
    std::string ties = "from_id,to_id,kind,tie,closeness,delay_min\n";
    for (int i = 0; i <= 512; ++i) ties += "npc_a,npc_b,npc,0,10,60\n";
    CHECK(fails({kTags, kDeeds, kNpcs, ties, "", "", "", ""}));
    std::string events = "event_id,day,hour,foreshadow_lead_min\n";
    for (int i = 0; i <= 64; ++i) events += "event_" + std::to_string(i) + ",1,08:00,0\n";
    CHECK(fails({kTags, kDeeds, kNpcs, kTies, "", "", events, ""}));
    CHECK(fails({kTags, kDeeds, kNpcs, kTies, "", "", "event_id,day,hour,foreshadow_lead_min\nevent_1,1,24:00,0\n", ""}));
    const char* expHead = "exp_id,npc_id,kind,due,window_min,remind_lead_min,kept_by,broken_by,deed_kept,deed_broken,deed_missed\n";
    CHECK(fails({kTags, kDeeds, kNpcs, kTies, "", std::string(expHead) + "e,npc_a,promise,100001:08:00,60,0,-,-,-,-,-\n", "", ""}));
    CHECK(fails({kTags, kDeeds, kNpcs, kTies, "", std::string(expHead) + "e,npc_a,promise,1:25:00,60,0,-,-,-,-,-\n", "", ""}));
    CHECK(fails({kTags, kDeeds, kNpcs, kTies, "", std::string(expHead) + "e,npc_a,promise,999999999999,60,0,-,-,-,-,-\n", "", ""}));
    CHECK_FALSE(fails({kTags, kDeeds, kNpcs, kTies, "", std::string(expHead) + "e,npc_a,promise,100000:23:59,60,0,-,-,-,-,-\n", "", ""}));
}

TEST_CASE("npc memory: malformed_and_truncated_saves") {
    Fx f;
    f.at(1000);
    f.record("deed_a", {"npc_a"});
    f.at(1100);
    {
        // Truncated: only the header survived.
        NpcMemorySave cut;
        cut.now = 1100;
        cut.nextSeq = 0;
        Fx g;
        const NpcMemoryLoadReport rep = npcApplySave(*g.w, g.t, g.lines, cut);
        CHECK(rep.formatOk);
        CHECK(rep.unknownIds.empty());
        CHECK(g.w->nextSeq == 1u);
        CHECK(g.ax("npc_a", kAxisT) == 20);
        CHECK(g.record("deed_a", {"npc_a"}) == 1u);  // and it runs on
    }
    {
        NpcMemorySave save = npcCaptureSave(*f.w, f.t, f.lines);
        REQUIRE(save.npcs[0].id == "npc_a");
        REQUIRE(!save.deeds.empty());
        save.deeds[0].flags = 0xFF;                                         // unknown flag bits
        save.deeds.push_back(save.deeds[0]);
        save.deeds.back().seq = save.nextSeq + 5;                           // a seq from the future
        save.rumours.push_back({1u, 1200u, "npc_c", 9, kNoLead, 100});      // hops out of range
        for (int i = 0; i < 20; ++i) save.npcs[0].mem.push_back(save.npcs[0].mem[0]);  // over the store
        save.npcs[0].recent.emplace_back("deed_gone", 5u);
        save.npcs[0].seen.push_back(200u);                                  // no such deed in the ring
        Fx g;
        const NpcMemoryLoadReport rep = npcApplySave(*g.w, g.t, g.lines, save);
        CHECK(rep.formatOk);
        CHECK(rep.clamped == 4);
        CHECK(rep.unknownIds.size() == 1);
        CHECK(g.w->npcs[g.npc("npc_a")].memCount == kMemPerNpc);
        REQUIRE(npcDeedAt(*g.w, 1) != nullptr);
        CHECK(npcDeedAt(*g.w, 1)->flags <= 3u);
        CHECK(npcDeedAt(*g.w, save.nextSeq + 5) == nullptr);
        CHECK(g.w->rumours[g.w->rumourCount - 1u].hops == 2);
        g.at(2000);
        g.record("deed_a", {"npc_a"}, nullptr, kNoLead, 7);
        g.pick("npc_a", NpcLineKind::Greet);
        // A save with gaps in the ring: recording into a gap must not clear what was learned in slot 0.
        Fx h;
        for (std::uint32_t i = 0; i < kDeedRing - 1u; ++i) h.record("deed_z", {"npc_x"});
        REQUIRE(h.record("deed_a", {"npc_a"}) == kDeedRing);  // lands in slot 0
        NpcMemorySave gappy = npcCaptureSave(*h.w, h.t, h.lines);
        gappy.deeds.erase(gappy.deeds.begin(), gappy.deeds.end() - 1);
        Fx k;
        npcApplySave(*k.w, k.t, k.lines, gappy);
        const std::uint16_t ka = k.npc("npc_a");
        REQUIRE((k.w->npcs[ka].seen[0] & 1u) != 0u);
        k.record("deed_z", {"npc_x"});
        CHECK((k.w->npcs[ka].seen[0] & 1u) != 0u);
    }
}

// ---- The slice: the shipped Data/Npc tables and the game hooks ----
namespace {

std::string readData(const char* name) {
    std::ifstream f(std::string(FTCORE_PROJECT_DATA_DIR) + "/" + name, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Ids only, like the private lines table: the shopkeeper's greets and the fixer's reminder.
constexpr const char* kSliceLines = R"(line_id,npc_id,kind,lead,priority,cooldown_min,once,cond
greet_plain,npc_shop_01,greet,any,1,0,0,
greet_knockdown,npc_shop_01,greet,any,5,240,0,mem:deed=van_knockdown
remind_meet,npc_fixer_01,remind,any,3,0,0,exp:exp_meet_dusk=reminding
greet_delivery_self,npc_shop_01,greet,any,4,0,0,"mem:deed=delivery_made,by=self"
greet_delivery_other,npc_shop_01,greet,any,3,0,0,"mem:deed=delivery_made,by=other"
greet_gate_broken,npc_shop_01,greet,any,7,0,0,"mem:deed=gate_broken;not_since:deed=gate_broken,deed=gate_mended,same_place"
greet_gate_mended,npc_shop_01,greet,any,6,720,0,"mem:deed=gate_mended;since_deed:gate_mended<=1440"
greet_kid_plain,npc_kid_01,greet,any,1,0,0,
greet_gift_kept,npc_kid_01,greet,any,6,0,0,"mem:deed=gift_accepted;not_since:deed=gift_accepted,deed=gift_returned"
greet_gift_back,npc_kid_01,greet,any,6,0,0,"mem:deed=gift_returned;not_since:deed=gift_returned,deed=gift_accepted"
)";

struct Slice {
    std::string files[9];
    NpcTables t;
    NpcLineTable lines;
    NpcGameDeeds g;
    TimeCurves curves;
    std::unique_ptr<NpcMemoryWorld> w = std::make_unique<NpcMemoryWorld>();
    WorldClock clock;

    Slice() {
        const char* names[] = {"Npc/tags.csv",     "Npc/deeds.csv",        "Npc/npc_values.csv",
                               "Npc/ties.csv",     "Npc/consequences.csv", "Npc/expectations.csv",
                               "Npc/schedule.csv", "Npc/memory_tuning.json", "time_curves.csv"};
        for (int i = 0; i < 9; ++i) {
            files[i] = readData(names[i]);
            INFO(names[i]);
            REQUIRE(!files[i].empty());
        }
        NpcTableSources src{files[0], files[1], files[2], files[3], files[4], files[5], files[6], files[7]};
        std::string err;
        const bool ok = importNpcTables(src, t, &err);
        INFO(err);
        REQUIRE(ok);
        REQUIRE(importNpcLines(kSliceLines, t, lines, &err));
        REQUIRE(npcResolveGameDeeds(t, g, &err));
        REQUIRE(parseTimeCurves(files[8], curves, &err));
        npcInit(*w, t);
        setTime(1, 9.0);
    }
    std::uint16_t npc(const char* id) const { return static_cast<std::uint16_t>(t.npcIndex(id)); }
    void setTime(std::uint32_t day, double hours) {
        clock.day = day;
        clock.hours = hours;
        npcOnClock(*w, t, clock, curves);
    }
    NpcWhere where(std::initializer_list<const char*> witnesses, std::uint8_t lead = 0) const {
        NpcWhere a;
        a.place = 7;
        a.district = 1;
        a.lead = lead;
        std::size_t i = 0;
        for (const char* x : witnesses) a.witnesses[i++] = npc(x);
        return a;
    }
    bool remembers(const char* who, std::uint16_t type) const { return npcRemembers(*w, t, npc(who), type, -1); }
    int pick(const char* who, NpcLineKind k, std::uint8_t lead = 0) { return npcPickLine(*w, t, lines, npc(who), lead, k).line; }
    int line(const char* id) const { return lines.lineIndex(id); }
};

}  // namespace

TEST_CASE("npc slice: the shipped tables load, the slice's three NPCs, every hook deed resolves") {
    Slice s;
    // The slice's three, plus the city profiles (test_npc_profiles.cpp).
    CHECK(s.t.npcs.size() >= 3u);
    CHECK(s.t.npcIndex("npc_shop_01") >= 0);
    CHECK(s.t.npcIndex("npc_fixer_01") >= 0);
    CHECK(s.t.npcIndex("npc_kid_01") >= 0);
    CHECK(s.g.vanKnockdown != kNoType);
    CHECK(s.g.meet != kNoType);
    CHECK(s.t.expIndex("exp_meet_dusk") >= 0);
    // Tables without the slice's deeds: resolving names the first missing one and leaves out alone.
    Fx k;
    NpcGameDeeds g;
    std::string err;
    CHECK_FALSE(npcResolveGameDeeds(k.t, g, &err));
    CHECK(err.find("van_knockdown") != std::string::npos);
    CHECK(g.vanKnockdown == kNoType);
}

TEST_CASE("npc slice: remember and feel - the shopkeeper sees a van knock-down; prices rise, warm drops to neutral, "
          "the tied kid hears of it later") {
    Slice s;
    const std::uint16_t shop = s.npc("npc_shop_01");
    CHECK(npcStance(*s.w, s.t, shop, 0) == NpcStance::Warm);
    const NpcConsequence before = npcOnShop(*s.w, s.t, s.g, s.where({}), shop, NpcShopAct::Enter);
    CHECK(before.shopOpen);
    const std::uint32_t seq = npcOnVanKnockdown(*s.w, s.t, s.g, s.where({"npc_shop_01"}), kNoNpc);
    REQUIRE(seq != 0u);
    CHECK(npcDeedAt(*s.w, seq)->type == s.g.vanKnockdown);
    CHECK(s.remembers("npc_shop_01", s.g.vanKnockdown));
    CHECK(npcStance(*s.w, s.t, shop, 0) == NpcStance::Neutral);
    const NpcConsequence after = npcOnShop(*s.w, s.t, s.g, s.where({}), shop, NpcShopAct::Enter);
    CHECK(after.pricePct > before.pricePct);
    CHECK(after.shopOpen);
    // Hearsay: not a witness, so not yet; after the tie's delay it is.
    CHECK(npcFindMemory(*s.w, s.npc("npc_kid_01"), seq) == nullptr);
    s.setTime(1, 10.5);
    CHECK(npcFindMemory(*s.w, s.npc("npc_kid_01"), seq) != nullptr);
}

TEST_CASE("npc slice: recall - the shopkeeper's greet cites the knock-down once, then cools down") {
    Slice s;
    CHECK(s.pick("npc_shop_01", NpcLineKind::Greet) == s.lines.lineIndex("greet_plain"));
    npcOnVanKnockdown(*s.w, s.t, s.g, s.where({"npc_shop_01"}), kNoNpc);
    CHECK(s.pick("npc_shop_01", NpcLineKind::Greet) == s.lines.lineIndex("greet_knockdown"));
    CHECK(s.pick("npc_shop_01", NpcLineKind::Greet) == s.lines.lineIndex("greet_plain"));
    s.setTime(1, 12.0);
    CHECK(s.pick("npc_shop_01", NpcLineKind::Greet) == s.lines.lineIndex("greet_plain"));
    s.setTime(1, 13.5); // past the 240-minute cooldown, still remembered
    CHECK(s.pick("npc_shop_01", NpcLineKind::Greet) == s.lines.lineIndex("greet_knockdown"));
}

TEST_CASE("npc slice: anticipate - the fixer reminds at 18:00; missed once 21:00 has passed leaves a pinned broken promise") {
    Slice s;
    const auto e = static_cast<std::size_t>(s.t.expIndex("exp_meet_dusk"));
    const auto brokenType = static_cast<std::uint16_t>(s.t.deedIndex("promise_broken"));
    s.setTime(1, 17.5);
    CHECK(s.w->exp[e] == NpcExpState::Pending);
    CHECK(s.pick("npc_fixer_01", NpcLineKind::Remind) == -1);
    s.setTime(1, 18.0);
    CHECK(s.w->exp[e] == NpcExpState::Reminding);
    CHECK(s.pick("npc_fixer_01", NpcLineKind::Remind) == s.lines.lineIndex("remind_meet"));
    s.setTime(1, 21.0); // the window's last minute: it could still be kept
    CHECK(s.w->exp[e] == NpcExpState::Reminding);
    s.setTime(1, 21.0 + 1.0 / 60.0);
    CHECK(s.w->exp[e] == NpcExpState::Missed);
    const NpcMemoryEntry* m = npcRecall(*s.w, s.t, s.npc("npc_fixer_01"), brokenType, -1);
    REQUIRE(m != nullptr);
    CHECK(m->pinned);
    s.setTime(5, 12.0); // days on, a pinned memory stays
    CHECK(s.remembers("npc_fixer_01", brokenType));

    // Turning up keeps it instead.
    Slice k;
    k.setTime(1, 19.25);
    CHECK(npcOnMeet(*k.w, k.t, k.g, k.where({}), k.npc("npc_fixer_01")) != 0u);
    CHECK(k.w->exp[e] == NpcExpState::Kept);
    k.setTime(1, 21.5);
    CHECK(k.w->exp[e] == NpcExpState::Kept);
    CHECK_FALSE(k.remembers("npc_fixer_01", brokenType));
}

TEST_CASE("npc slice: day and night - an unseen knock-down spreads through a busy noon street, not at midnight") {
    for (const bool night : {false, true}) {
        Slice s;
        const std::uint32_t day = night ? 2u : 1u;
        s.setTime(day, night ? 0.0 : 12.0);
        const std::uint32_t seq = npcOnVanKnockdown(*s.w, s.t, s.g, s.where({}), kNoNpc);
        REQUIRE(seq != 0u);
        s.setTime(day, night ? 1.5 : 13.5);
        INFO("night " << night);
        CHECK((npcFindMemory(*s.w, s.npc("npc_shop_01"), seq) != nullptr) == !night);
        CHECK((npcFindMemory(*s.w, s.npc("npc_fixer_01"), seq) != nullptr) == !night);
    }
}

TEST_CASE("npc slice: each hook records its own deed; a tied victim learns it as the victim; nothing allocates") {
    Slice s;
    const std::uint16_t kid = s.npc("npc_kid_01");
    const std::uint16_t shop = s.npc("npc_shop_01");
    const std::int32_t respect0 = npcAxis(*s.w, s.t, s.npc("npc_fixer_01"), kAxisR);
    const auto a0 = ftcore_test::allocStats();
    const std::uint32_t tied = npcOnVanKnockdown(*s.w, s.t, s.g, s.where({}), kid);
    const std::uint32_t fight = npcOnFightEnd(*s.w, s.t, s.g, s.where({"npc_fixer_01"}), true, false);
    const std::uint32_t mercy = npcOnFightEnd(*s.w, s.t, s.g, s.where({"npc_fixer_01"}), true, true);
    const std::uint32_t none = npcOnFightEnd(*s.w, s.t, s.g, s.where({}), false, false);
    npcOnShop(*s.w, s.t, s.g, s.where({}), shop, NpcShopAct::Buy);
    const std::uint32_t bought = s.w->nextSeq - 1u;
    npcOnShop(*s.w, s.t, s.g, s.where({}), shop, NpcShopAct::Steal);
    const std::uint32_t stolen = s.w->nextSeq - 1u;
    const std::uint32_t helped = npcOnHelpedPedestrian(*s.w, s.t, s.g, s.where({"npc_kid_01"}));
    const auto a1 = ftcore_test::allocStats();
    CHECK(a1.count == a0.count);
    CHECK(none == 0u);
    CHECK(npcDeedAt(*s.w, tied)->type == s.g.vanKnockdownTied);
    CHECK(npcDeedAt(*s.w, tied)->victim == kid);
    const NpcMemoryEntry* km = npcFindMemory(*s.w, kid, tied);
    REQUIRE(km != nullptr);
    CHECK(km->hops == static_cast<std::uint8_t>(NpcLearn::Victim));
    CHECK(npcDeedAt(*s.w, fight)->type == s.g.fightWon);
    CHECK(npcDeedAt(*s.w, mercy)->type == s.g.enemySpared);
    CHECK(npcDeedAt(*s.w, mercy - 1u)->type == s.g.fightWon);
    CHECK(npcDeedAt(*s.w, bought)->type == s.g.shopBought);
    CHECK(npcDeedAt(*s.w, stolen)->type == s.g.shopTheft);
    CHECK(npcDeedAt(*s.w, stolen)->victim == shop);
    CHECK(npcDeedAt(*s.w, helped)->type == s.g.helpedPedestrian);
    // The fixer respects violence: the wins it saw raise its respect.
    CHECK(npcAxis(*s.w, s.t, s.npc("npc_fixer_01"), kAxisR) > respect0);
}

// ---- Neighbourly deeds: deliveries by one lead or another, gifts kept or handed back, a gate broken then mended ----

TEST_CASE("npc slice: a delivery is remembered with who made it; reinforcement keeps both leads") {
    Slice s;
    const std::uint16_t shop = s.npc("npc_shop_01");
    const std::uint32_t seq = npcOnDelivery(*s.w, s.t, s.g, s.where({}, 0), shop);
    REQUIRE(seq != 0u);
    CHECK(npcDeedAt(*s.w, seq)->victim == shop);
    CHECK(s.pick("npc_shop_01", NpcLineKind::Greet, 0) == s.line("greet_delivery_self"));
    CHECK(s.pick("npc_shop_01", NpcLineKind::Greet, 1) == s.line("greet_delivery_other"));
    CHECK(s.pick("npc_shop_01", NpcLineKind::Greet, 2) == s.line("greet_delivery_other"));
    // The other lead delivers to the same door: one memory, both doers kept.
    s.setTime(1, 11.0);
    REQUIRE(npcOnDelivery(*s.w, s.t, s.g, s.where({}, 1), shop) != 0u);
    const NpcMemoryEntry* m = npcRecall(*s.w, s.t, shop, s.g.delivery, -1);
    REQUIRE(m != nullptr);
    CHECK(m->count == 2u);
    CHECK(m->leads == 0b011u);
    CHECK(m->lead == 1u);
    CHECK(s.pick("npc_shop_01", NpcLineKind::Greet, 0) == s.line("greet_delivery_self"));
    CHECK(s.pick("npc_shop_01", NpcLineKind::Greet, 1) == s.line("greet_delivery_self"));
    CHECK(s.pick("npc_shop_01", NpcLineKind::Greet, 2) == s.line("greet_delivery_other"));
    // Nobody to hand it to: nothing recorded.
    CHECK(npcOnDelivery(*s.w, s.t, s.g, s.where({}, 0), kNoNpc) == 0u);
}

TEST_CASE("npc slice: a gift handed back is asked about at every later meeting; one kept is too, warmer") {
    std::int32_t trust[2] = {0, 0};
    for (const bool accepted : {false, true}) {
        Slice s;
        const std::uint16_t kid = s.npc("npc_kid_01");
        const std::int32_t t0 = npcAxis(*s.w, s.t, kid, kAxisT);
        CHECK(s.pick("npc_kid_01", NpcLineKind::Greet) == s.line("greet_kid_plain"));
        const std::uint32_t seq = npcOnGift(*s.w, s.t, s.g, s.where({}, 1), kid, accepted);
        REQUIRE(seq != 0u);
        const int want = s.line(accepted ? "greet_gift_kept" : "greet_gift_back");
        const NpcMemoryEntry* m = npcFindMemory(*s.w, kid, seq);
        REQUIRE(m != nullptr);
        CHECK(m->pinned);
        trust[accepted ? 1 : 0] = npcAxis(*s.w, s.t, kid, kAxisT) - t0;
        INFO("accepted " << accepted);
        for (int i = 0; i < 3; ++i) CHECK(s.pick("npc_kid_01", NpcLineKind::Greet, static_cast<std::uint8_t>(i)) == want);
        // Days on, after other deeds it saw, the next meeting still asks.
        npcOnFightEnd(*s.w, s.t, s.g, s.where({"npc_kid_01"}), true, false);
        npcOnHelpedPedestrian(*s.w, s.t, s.g, s.where({"npc_kid_01"}));
        s.setTime(12, 18.0);
        CHECK(s.pick("npc_kid_01", NpcLineKind::Greet) == want);
        CHECK(s.pick("npc_kid_01", NpcLineKind::Greet) == want);
    }
    CHECK(trust[0] < 0);
    CHECK(trust[1] > 0);
}

TEST_CASE("npc slice: the shopkeeper hears of a gift handed back, but hearsay of a pinned deed is not pinned") {
    Slice s;
    const std::uint32_t seq = npcOnGift(*s.w, s.t, s.g, s.where({}, 0), s.npc("npc_kid_01"), false);
    REQUIRE(seq != 0u);
    s.setTime(1, 10.5);
    const NpcMemoryEntry* m = npcFindMemory(*s.w, s.npc("npc_shop_01"), seq);
    REQUIRE(m != nullptr);
    CHECK(m->hops == static_cast<std::uint8_t>(NpcLearn::Hop1));
    CHECK_FALSE(m->pinned);
    CHECK(m->leads == 0b001u);  // closeness 80: the doer travels with the story
}

TEST_CASE("npc slice: a broken gate is raised at every meeting until it is mended, then thanked once; breaking it again reopens it") {
    Slice s;
    const std::uint16_t shop = s.npc("npc_shop_01");
    NpcWhere gate = s.where({}, 1);
    gate.place = 9;
    CHECK(npcStance(*s.w, s.t, shop, 1) == NpcStance::Warm);
    const std::uint32_t price0 = static_cast<std::uint32_t>(npcConsequence(*s.w, s.t, shop, 1).pricePct);
    REQUIRE(npcOnGate(*s.w, s.t, s.g, gate, shop, false) != 0u);
    CHECK(npcStance(*s.w, s.t, shop, 1) == NpcStance::Neutral);
    CHECK(static_cast<std::uint32_t>(npcConsequence(*s.w, s.t, shop, 1).pricePct) > price0);
    for (int i = 0; i < 3; ++i) CHECK(s.pick("npc_shop_01", NpcLineKind::Greet, 0) == s.line("greet_gate_broken"));
    s.setTime(7, 9.0);  // pinned: a week on it still comes up
    CHECK(s.pick("npc_shop_01", NpcLineKind::Greet, 1) == s.line("greet_gate_broken"));

    gate.lead = 0;
    REQUIRE(npcOnGate(*s.w, s.t, s.g, gate, shop, true) != 0u);
    CHECK(npcStance(*s.w, s.t, shop, 0) == NpcStance::Warm);
    CHECK(s.pick("npc_shop_01", NpcLineKind::Greet, 0) == s.line("greet_gate_mended"));
    // Thanked; the cooldown passes to the plain greet (the break is no longer raised).
    CHECK(s.pick("npc_shop_01", NpcLineKind::Greet, 0) == s.line("greet_plain"));

    s.setTime(8, 9.0);
    gate.lead = 1;
    REQUIRE(npcOnGate(*s.w, s.t, s.g, gate, shop, false) != 0u);
    const NpcMemoryEntry* m = npcRecall(*s.w, s.t, shop, s.g.gateBroken, -1);
    REQUIRE(m != nullptr);
    CHECK(m->count == 2u);  // the same gate: one memory, reinforced
    CHECK(s.pick("npc_shop_01", NpcLineKind::Greet, 0) == s.line("greet_gate_broken"));
}

TEST_CASE("npc memory: by= clauses parse self, other and a lead; anything else is refused") {
    Fx f;
    NpcLineTable out;
    std::string err;
    const char* head = "line_id,npc_id,kind,lead,priority,cooldown_min,once,cond\n";
    CHECK(importNpcLines(std::string(head) + "l0,npc_a,greet,any,1,0,0,\"mem:deed=deed_a,by=self>=100\"\n"
                         "l1,npc_a,greet,any,1,0,0,\"mem:deed=deed_a,by=other\"\n"
                         "l2,npc_a,greet,any,1,0,0,\"mem:tag=tag_a,by=2\"\n",
                         f.t, out, &err));
    REQUIRE(out.lines.size() == 3u);
    CHECK(out.lines[0].cond[0].by == kBySelf);
    CHECK(out.lines[0].cond[0].value == 100);
    CHECK(out.lines[1].cond[0].by == kByOther);
    CHECK(out.lines[1].cond[0].value == -1);
    CHECK(out.lines[2].cond[0].by == 2);
    for (const char* bad : {"\"mem:deed=deed_a,by=3\"", "\"mem:deed=deed_a,by=x\"", "\"mem:deed=deed_a,by=\"",
                            "\"mem:deed=deed_a,lead=0\"", "\"mem:deed=deed_a,by=-1\""}) {
        INFO(bad);
        CHECK_FALSE(importNpcLines(std::string(head) + "l0,npc_a,greet,any,1,0,0," + bad + "\n", f.t, out, &err));
    }
}

TEST_CASE("npc memory: who did it survives a save; an older save without the doer set rebuilds it from the lead") {
    Fx f;
    f.at(npcMinute(1, 9.0));
    f.record("deed_c", {"npc_a"}, nullptr, 0);
    f.record("deed_c", {"npc_a"}, nullptr, 2);
    const NpcMemoryEntry* m = npcRecall(*f.w, f.t, f.npc("npc_a"), f.deed("deed_c"), -1);
    REQUIRE(m != nullptr);
    CHECK(m->leads == 0b101u);
    NpcMemorySave save = npcCaptureSave(*f.w, f.t, f.lines);
    Fx g;
    npcApplySave(*g.w, g.t, g.lines, save);
    CHECK(dump(*g.w, g.t, g.lines) == dump(*f.w, f.t, f.lines));
    for (auto& n : save.npcs)
        for (auto& e : n.mem) e.leads = 0;
    Fx h;
    npcApplySave(*h.w, h.t, h.lines, save);
    const NpcMemoryEntry* r = npcRecall(*h.w, h.t, h.npc("npc_a"), h.deed("deed_c"), -1);
    REQUIRE(r != nullptr);
    CHECK(r->leads == 0b100u);
}

TEST_CASE("npc slice: the debug view shows axes, stances, the strongest memories, queued rumours and expectations") {
    Slice s;
    const std::uint16_t shop = s.npc("npc_shop_01"), kid = s.npc("npc_kid_01"), fixer = s.npc("npc_fixer_01");
    NpcDebugView v;
    CHECK_FALSE(npcDebugView(*s.w, s.t, kNoNpc, v));
    const std::uint32_t knock = npcOnVanKnockdown(*s.w, s.t, s.g, s.where({"npc_shop_01"}), kNoNpc);
    npcOnDelivery(*s.w, s.t, s.g, s.where({}, 2), shop);
    NpcWhere gate = s.where({}, 1);
    gate.place = 9;
    npcOnGate(*s.w, s.t, s.g, gate, shop, false);
    for (int i = 0; i < 4; ++i) {  // more than five memories: only the strongest five show
        NpcWhere p = s.where({"npc_shop_01"}, 0);
        p.place = static_cast<std::uint16_t>(20 + i);
        npcOnHelpedPedestrian(*s.w, s.t, s.g, p);
    }
    const auto a0 = ftcore_test::allocStats();
    REQUIRE(npcDebugView(*s.w, s.t, shop, v));
    const auto a1 = ftcore_test::allocStats();
    CHECK(a1.count == a0.count);
    CHECK(v.memCount == 7u);
    CHECK(v.topCount == 5u);
    for (std::size_t i = 1; i < v.topCount; ++i) CHECK(v.top[i - 1].salience >= v.top[i].salience);
    // Nothing left out is stronger than the weakest shown.
    std::uint32_t stronger = 0;
    for (std::uint32_t i = 0; i < s.w->npcs[shop].memCount; ++i)
        if (npcSalience(*s.w, s.t, shop, s.w->npcs[shop].mem[i]) > v.top[4].salience) ++stronger;
    CHECK(stronger <= 4u);
    CHECK(v.top[0].type == s.g.gateBroken);  // the strongest: pinned, first hand, magnitude 2
    CHECK(v.top[0].pinned);
    CHECK(v.top[0].leads == 0b010u);
    for (int a = 0; a < 3; ++a) CHECK(v.axes[static_cast<std::size_t>(a)] == npcAxis(*s.w, s.t, shop, static_cast<NpcAxis>(a)));
    CHECK(v.offsets[1][0] == npcOffset(*s.w, s.t, shop, 1, kAxisT));
    CHECK(v.offsets[1][0] < 0);
    CHECK(v.stance[1] == npcStance(*s.w, s.t, shop, 1));
    CHECK(v.stance[kLeadCount] == npcStance(*s.w, s.t, shop, kNoLead));
    CHECK(v.expCount == 0u);
    // The kid has the knock-down coming by hearsay; the fixer has the dusk meeting.
    REQUIRE(npcDebugView(*s.w, s.t, kid, v));
    CHECK(v.rumoursQueued >= 1u);
    CHECK(v.nextRumourDue == npcDeedAt(*s.w, knock)->minute + 60u);
    REQUIRE(npcDebugView(*s.w, s.t, fixer, v));
    REQUIRE(v.expCount == 1u);
    CHECK(v.exps[0].exp == static_cast<std::uint16_t>(s.t.expIndex("exp_meet_dusk")));
    CHECK(v.exps[0].state == NpcExpState::Pending);
}

TEST_CASE("npc slice: a gift handed back then taken after all is asked about as taken; two gates are tracked apart") {
    Slice s;
    const std::uint16_t kid = s.npc("npc_kid_01"), shop = s.npc("npc_shop_01");
    npcOnGift(*s.w, s.t, s.g, s.where({}, 0), kid, false);
    CHECK(s.pick("npc_kid_01", NpcLineKind::Greet) == s.line("greet_gift_back"));
    s.setTime(2, 9.0);
    npcOnGift(*s.w, s.t, s.g, s.where({}, 0), kid, true);
    CHECK(s.pick("npc_kid_01", NpcLineKind::Greet) == s.line("greet_gift_kept"));
    CHECK(s.pick("npc_kid_01", NpcLineKind::Greet) == s.line("greet_gift_kept"));

    NpcWhere front = s.where({}, 1), back = s.where({}, 1);
    front.place = 9;
    back.place = 10;
    npcOnGate(*s.w, s.t, s.g, front, shop, false);
    npcOnGate(*s.w, s.t, s.g, back, shop, false);
    s.setTime(3, 9.0);
    npcOnGate(*s.w, s.t, s.g, front, shop, true);  // one mended, the other still down
    CHECK(s.pick("npc_shop_01", NpcLineKind::Greet) == s.line("greet_gate_broken"));
    s.setTime(3, 10.0);
    npcOnGate(*s.w, s.t, s.g, back, shop, true);
    CHECK(s.pick("npc_shop_01", NpcLineKind::Greet) == s.line("greet_gate_mended"));  // both up again
    s.setTime(5, 9.0);  // the mends are pinned but thanked only within a day
    CHECK(s.pick("npc_shop_01", NpcLineKind::Greet) == s.line("greet_plain"));

    // Taken and handed back in the same minute: the later one recorded is the one asked about.
    Slice k;
    npcOnGift(*k.w, k.t, k.g, k.where({}, 0), kid, true);
    npcOnGift(*k.w, k.t, k.g, k.where({}, 0), kid, false);
    CHECK(k.pick("npc_kid_01", NpcLineKind::Greet) == k.line("greet_gift_back"));
}

TEST_CASE("npc memory: an older deed heard late never rewinds a memory's minute or its latest doer") {
    Fx f;
    const std::uint32_t m0 = npcMinute(1, 9.0);
    f.at(m0);
    f.record("deed_c", {"npc_a"}, nullptr, 0, 5);  // the rumour reaches npc_b at m0 + 60
    f.at(m0 + 30);
    const std::uint32_t seq2 = f.record("deed_c", {"npc_b"}, nullptr, 1, 5);
    f.at(m0 + 61);
    const NpcMemoryEntry* m = npcRecall(*f.w, f.t, f.npc("npc_b"), f.deed("deed_c"), -1);
    REQUIRE(m != nullptr);
    CHECK(m->count == 2u);
    CHECK(m->deedMinute == m0 + 30);
    CHECK(m->seq == seq2);
    CHECK(m->lead == 1u);
    CHECK(m->leads == 0b011u);
}

TEST_CASE("npc memory: not_since takes same_place and nothing else after the second deed") {
    Fx f;
    NpcLineTable out;
    std::string err;
    const char* head = "line_id,npc_id,kind,lead,priority,cooldown_min,once,cond\n";
    REQUIRE(importNpcLines(std::string(head) + "l0,npc_a,greet,any,1,0,0,\"not_since:deed=deed_a,deed=deed_c,same_place\"\n", f.t, out, &err));
    CHECK(out.lines[0].cond[0].value == 1);
    for (const char* bad : {"\"not_since:deed=deed_a,deed=deed_c,place\"", "\"not_since:deed=deed_a,deed=deed_c,\"",
                            "\"not_since:deed=deed_a,deed=deed_c,same_place,x\"", "\"not_since:deed=deed_a,deed=deed_a\""}) {
        INFO(bad);
        CHECK_FALSE(importNpcLines(std::string(head) + "l0,npc_a,greet,any,1,0,0," + bad + "\n", f.t, out, &err));
    }
}

// ---- Pins: a mend releases its break, full pins drop a mend first, saves, by= with since, places, hearsay ----
namespace {

constexpr const char* kGateLines = R"(line_id,npc_id,kind,lead,priority,cooldown_min,once,cond
plain,npc_shop_01,greet,any,1,0,0,
gate_open,npc_shop_01,greet,any,7,0,0,"mem:deed=gate_broken>=0;not_since:deed=gate_broken,deed=gate_mended,same_place"
)";

NpcLineTable linesFor(const Slice& s, const char* csv) {
    NpcLineTable out;
    std::string err;
    const bool ok = importNpcLines(csv, s.t, out, &err);
    INFO(err);
    REQUIRE(ok);
    return out;
}

int pickIn(Slice& s, const NpcLineTable& lines, std::uint8_t lead = 0) {
    return npcPickLine(*s.w, s.t, lines, s.npc("npc_shop_01"), lead, NpcLineKind::Greet).line;
}

std::uint32_t gate(Slice& s, std::uint16_t place, bool mended, std::uint8_t lead = 0) {
    NpcWhere a = s.where({}, lead);
    a.place = place;
    return npcOnGate(*s.w, s.t, s.g, a, s.npc("npc_shop_01"), mended);
}

const NpcMemoryEntry* memAt(const Slice& s, std::uint16_t type, std::uint16_t place) {
    const NpcState& st = s.w->npcs[s.npc("npc_shop_01")];
    for (std::uint32_t i = 0; i < st.memCount; ++i)
        if (st.mem[i].type == type && st.mem[i].place == place) return &st.mem[i];
    return nullptr;
}

bool pinnedAt(const Slice& s, std::uint16_t type, std::uint16_t place) {
    const NpcMemoryEntry* m = memAt(s, type, place);
    return m != nullptr && m->pinned;
}

std::uint32_t shopPins(const Slice& s) {
    const NpcState& st = s.w->npcs[s.npc("npc_shop_01")];
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < st.memCount; ++i) n += st.mem[i].pinned ? 1u : 0u;
    return n;
}

// A gift (at where()'s place 7), a gate left broken at 40, then five gates at 30..34 broken and mended.
void giftOpenGateFiveMended(Slice& s) {
    REQUIRE(npcOnGift(*s.w, s.t, s.g, s.where({}, 0), s.npc("npc_shop_01"), true) != 0u);
    s.setTime(1, 10.0);
    REQUIRE(gate(s, 40, false) != 0u);
    for (int i = 0; i < 5; ++i) {
        const auto p = static_cast<std::uint16_t>(30 + i);
        s.setTime(2, 8.0 + i);
        REQUIRE(gate(s, p, false) != 0u);
        s.setTime(2, 8.5 + i);
        REQUIRE(gate(s, p, true) != 0u);
    }
}

}  // namespace

TEST_CASE("npc slice: pins past the maximum - a mend is let go first; open breaks and a gift stay; with no mend the oldest goes") {
    const std::uint16_t gift = 7;
    {  // A gift, then six gates broken and mended: seven pins wanted, the oldest mend gives way.
        Slice s;
        const NpcLineTable lines = linesFor(s, kGateLines);
        REQUIRE(npcOnGift(*s.w, s.t, s.g, s.where({}, 0), s.npc("npc_shop_01"), true) != 0u);
        for (int i = 0; i < 6; ++i) {
            const auto p = static_cast<std::uint16_t>(30 + i);
            s.setTime(2, 8.0 + i);
            REQUIRE(gate(s, p, false) != 0u);
            s.setTime(2, 8.5 + i);
            REQUIRE(gate(s, p, true) != 0u);
        }
        CHECK(s.w->npcs[s.npc("npc_shop_01")].memCount == 13u);
        CHECK(shopPins(s) == kPinnedMax);
        CHECK(pinnedAt(s, s.g.giftAccepted, gift));
        for (int i = 0; i < 6; ++i) {
            const auto p = static_cast<std::uint16_t>(30 + i);
            INFO("gate " << p);
            REQUIRE(memAt(s, s.g.gateBroken, p) != nullptr);  // released, still held
            CHECK_FALSE(pinnedAt(s, s.g.gateBroken, p));
            REQUIRE(memAt(s, s.g.gateMended, p) != nullptr);
            CHECK(pinnedAt(s, s.g.gateMended, p) == (i > 0));
        }
        CHECK(pickIn(s, lines) == lines.lineIndex("plain"));  // every gate reads mended
    }
    {  // A gift, an open break, then five handled gates: the open break and the gift keep their pins.
        Slice s;
        const NpcLineTable lines = linesFor(s, kGateLines);
        giftOpenGateFiveMended(s);
        CHECK(shopPins(s) == kPinnedMax);
        CHECK(pinnedAt(s, s.g.giftAccepted, gift));
        CHECK(pinnedAt(s, s.g.gateBroken, 40));
        CHECK_FALSE(pinnedAt(s, s.g.gateMended, 30));
        for (std::uint16_t p = 31; p < 35; ++p) CHECK(pinnedAt(s, s.g.gateMended, p));
        CHECK(pickIn(s, lines) == lines.lineIndex("gate_open"));
        // One more pin, a new break: the next oldest mend goes, never the gift or an open break.
        s.setTime(3, 9.0);
        REQUIRE(gate(s, 41, false) != 0u);
        CHECK(shopPins(s) == kPinnedMax);
        CHECK_FALSE(pinnedAt(s, s.g.gateMended, 31));
        CHECK(pinnedAt(s, s.g.giftAccepted, gift));
        CHECK(pinnedAt(s, s.g.gateBroken, 40));
        CHECK(pinnedAt(s, s.g.gateBroken, 41));
    }
    {  // No mend held: a seventh pin unpins the oldest, here the gift.
        Slice s;
        REQUIRE(npcOnGift(*s.w, s.t, s.g, s.where({}, 0), s.npc("npc_shop_01"), true) != 0u);
        for (int i = 0; i < 6; ++i) {
            s.setTime(2, 8.0 + i);
            REQUIRE(gate(s, static_cast<std::uint16_t>(30 + i), false) != 0u);
        }
        CHECK(shopPins(s) == kPinnedMax);
        CHECK_FALSE(pinnedAt(s, s.g.giftAccepted, gift));
        for (std::uint16_t p = 30; p < 36; ++p) CHECK(pinnedAt(s, s.g.gateBroken, p));
    }
}

TEST_CASE("npc slice: a mend releases its break's pin; the gate reads mended while the mend is held; a re-break pins and reopens it") {
    Slice s;
    const NpcLineTable lines = linesFor(s, kGateLines);
    const std::uint16_t shop = s.npc("npc_shop_01");
    REQUIRE(gate(s, 9, false, 1) != 0u);
    CHECK(pinnedAt(s, s.g.gateBroken, 9));
    CHECK(pickIn(s, lines) == lines.lineIndex("gate_open"));

    s.setTime(2, 9.0);
    REQUIRE(gate(s, 9, true, 0) != 0u);
    const NpcMemoryEntry* b = memAt(s, s.g.gateBroken, 9);
    REQUIRE(b != nullptr);
    CHECK_FALSE(b->pinned);
    CHECK(b->ts == s.w->now);  // fades from the mend on
    CHECK(pinnedAt(s, s.g.gateMended, 9));
    CHECK(pickIn(s, lines) == lines.lineIndex("plain"));
    const std::int32_t s0 = npcSalience(*s.w, s.t, shop, *b);

    s.setTime(4, 9.0);
    b = memAt(s, s.g.gateBroken, 9);
    REQUIRE(b != nullptr);
    CHECK(npcSalience(*s.w, s.t, shop, *b) < s0);
    CHECK(pickIn(s, lines) == lines.lineIndex("plain"));

    s.setTime(4, 10.0);  // broken again: one memory, reinforced and pinned again; the gate is open
    REQUIRE(gate(s, 9, false, 2) != 0u);
    b = memAt(s, s.g.gateBroken, 9);
    REQUIRE(b != nullptr);
    CHECK(b->count == 2u);
    CHECK(b->pinned);
    CHECK(pickIn(s, lines) == lines.lineIndex("gate_open"));

    s.setTime(5, 9.0);  // mended again: the reinforced mend releases it again
    REQUIRE(gate(s, 9, true, 0) != 0u);
    REQUIRE(memAt(s, s.g.gateMended, 9) != nullptr);
    CHECK(memAt(s, s.g.gateMended, 9)->count == 2u);
    CHECK(pinnedAt(s, s.g.gateMended, 9));
    CHECK_FALSE(pinnedAt(s, s.g.gateBroken, 9));
    CHECK(pickIn(s, lines) == lines.lineIndex("plain"));

    // A full store drops the released break before the mend that settled it, even when the mend is the weaker.
    NpcState& st = s.w->npcs[shop];
    for (std::uint32_t i = 0; i < st.memCount; ++i) {
        if (st.mem[i].type == s.g.gateBroken) st.mem[i].s0 = 1000;
        if (st.mem[i].type == s.g.gateMended) {
            st.mem[i].pinned = false;
            st.mem[i].s0 = 50;
        }
    }
    for (int i = 0; i <= static_cast<int>(kMemPerNpc); ++i) {
        NpcWhere p = s.where({"npc_shop_01"}, 0);
        p.place = static_cast<std::uint16_t>(100 + i);
        npcOnHelpedPedestrian(*s.w, s.t, s.g, p);
        if (memAt(s, s.g.gateBroken, 9) == nullptr) break;
    }
    CHECK(st.memCount == kMemPerNpc);
    CHECK(memAt(s, s.g.gateBroken, 9) == nullptr);
    CHECK(memAt(s, s.g.gateMended, 9) != nullptr);
    CHECK(pickIn(s, lines) == lines.lineIndex("plain"));
}

TEST_CASE("npc slice: a gate through save and load; a save with too many pins is clamped as play would have left it") {
    {
        Slice s;
        const NpcLineTable lines = linesFor(s, kGateLines);
        REQUIRE(gate(s, 9, false, 1) != 0u);
        s.setTime(2, 9.0);
        REQUIRE(gate(s, 9, true, 0) != 0u);
        const NpcMemorySave save = npcCaptureSave(*s.w, s.t, lines);
        Slice r;
        const NpcMemoryLoadReport rep = npcApplySave(*r.w, r.t, lines, save);
        CHECK(rep.formatOk);
        CHECK(rep.unknownIds.empty());
        CHECK(rep.clamped == 0u);
        CHECK(dump(*r.w, r.t, lines) == dump(*s.w, s.t, lines));
        CHECK_FALSE(pinnedAt(r, r.g.gateBroken, 9));
        CHECK(pinnedAt(r, r.g.gateMended, 9));
        CHECK(pickIn(r, lines) == lines.lineIndex("plain"));
        r.setTime(3, 9.0);
        REQUIRE(gate(r, 9, false, 1) != 0u);
        CHECK(pinnedAt(r, r.g.gateBroken, 9));
        CHECK(pickIn(r, lines) == lines.lineIndex("gate_open"));
    }
    {  // The live state is saved, then the save is edited to pin every memory since it was learned (as a save made
       // before mends released anything would hold it), in reverse order.
        Slice s;
        const NpcLineTable lines = linesFor(s, kGateLines);
        giftOpenGateFiveMended(s);
        NpcMemorySave save = npcCaptureSave(*s.w, s.t, lines);
        std::uint32_t forged = 0;
        for (auto& n : save.npcs) {
            if (n.id != "npc_shop_01") continue;
            for (auto& e : n.mem) {
                if (e.pinned) continue;
                ++forged;
                e.pinned = true;
                e.ts = e.learned;
            }
            std::reverse(n.mem.begin(), n.mem.end());
        }
        REQUIRE(forged == 6u);  // 12 memories, all pinned in the save
        Slice r;
        const NpcMemoryLoadReport rep = npcApplySave(*r.w, r.t, lines, save);
        CHECK(rep.clamped == 6u);  // five breaks released by their mends, then the oldest mend
        CHECK(shopPins(r) == kPinnedMax);
        const NpcState& live = s.w->npcs[s.npc("npc_shop_01")];
        for (std::uint32_t i = 0; i < live.memCount; ++i) {
            const NpcMemoryEntry& e = live.mem[i];
            const NpcMemoryEntry* m = memAt(r, e.type, e.place);
            REQUIRE(m != nullptr);
            INFO("type " << e.type << " place " << e.place);
            CHECK(m->pinned == e.pinned);
            if (!m->pinned) CHECK(m->ts == save.now);
        }
        CHECK(pickIn(r, lines) == lines.lineIndex("gate_open"));  // gate 40 is still open
    }
}

TEST_CASE("npc slice: by=self and by=other combine with since and not_since") {
    Slice s;
    const NpcLineTable lines = linesFor(s, R"(line_id,npc_id,kind,lead,priority,cooldown_min,once,cond
plain,npc_shop_01,greet,any,1,0,0,
open_self,npc_shop_01,greet,any,7,0,0,"mem:deed=gate_broken,by=self>=0;not_since:deed=gate_broken,deed=gate_mended,same_place;since_deed:gate_broken>=60"
open_other,npc_shop_01,greet,any,6,0,0,"mem:deed=gate_broken,by=other>=0;not_since:deed=gate_broken,deed=gate_mended"
mended_self,npc_shop_01,greet,any,5,0,0,"mem:deed=gate_mended,by=self>=0;since_deed:gate_mended<=1440"
)");
    const int plain = lines.lineIndex("plain"), openSelf = lines.lineIndex("open_self"),
              openOther = lines.lineIndex("open_other"), mendedSelf = lines.lineIndex("mended_self");
    const std::uint32_t m0 = s.w->now;
    auto at = [&](std::uint32_t minute) { npcCheckpoint(*s.w, s.t, minute, 0); };
    REQUIRE(gate(s, 9, false, 1) != 0u);
    CHECK(pickIn(s, lines, 1) == plain);  // the one who did it, not yet an hour on
    CHECK(pickIn(s, lines, 0) == openOther);
    CHECK(pickIn(s, lines, 2) == openOther);
    at(m0 + 59);
    CHECK(pickIn(s, lines, 1) == plain);
    at(m0 + 60);
    CHECK(pickIn(s, lines, 1) == openSelf);
    CHECK(pickIn(s, lines, 0) == openOther);
    at(m0 + 120);
    REQUIRE(gate(s, 9, true, 0) != 0u);
    CHECK(pickIn(s, lines, 0) == mendedSelf);
    CHECK(pickIn(s, lines, 1) == plain);  // mended by another lead: no gate line for the one who broke it
    CHECK(pickIn(s, lines, 2) == plain);
    at(m0 + 120 + 1440);
    CHECK(pickIn(s, lines, 0) == mendedSelf);
    at(m0 + 120 + 1441);
    CHECK(pickIn(s, lines, 0) == plain);
}

TEST_CASE("npc slice: a gate deed needs its place; a mend at another place does not mend the break") {
    Slice s;
    const NpcLineTable lines = linesFor(s, kGateLines);
    const NpcLineTable anyPlace = linesFor(s, R"(line_id,npc_id,kind,lead,priority,cooldown_min,once,cond
plain,npc_shop_01,greet,any,1,0,0,
gate_open_any,npc_shop_01,greet,any,7,0,0,"mem:deed=gate_broken>=0;not_since:deed=gate_broken,deed=gate_mended"
)");
    CHECK(gate(s, 0, false) == 0u);  // no place: refused (a mend at 0 would release every break there)
    CHECK(gate(s, 0, true) == 0u);
    REQUIRE(gate(s, 3, false) != 0u);
    CHECK(pinnedAt(s, s.g.gateBroken, 3));
    CHECK(pickIn(s, lines) == lines.lineIndex("gate_open"));

    s.setTime(1, 10.0);
    REQUIRE(gate(s, 9, true) != 0u);  // another gate mended: the break at 3 keeps its pin and stays open
    CHECK(pinnedAt(s, s.g.gateBroken, 3));
    CHECK(pinnedAt(s, s.g.gateMended, 9));
    CHECK(pickIn(s, lines) == lines.lineIndex("gate_open"));
    CHECK(pickIn(s, anyPlace) == anyPlace.lineIndex("plain"));  // without same_place any later mend counts

    s.setTime(1, 11.0);
    REQUIRE(gate(s, 3, true) != 0u);
    CHECK_FALSE(pinnedAt(s, s.g.gateBroken, 3));
    CHECK(pinnedAt(s, s.g.gateMended, 3));
    CHECK(pinnedAt(s, s.g.gateMended, 9));
    CHECK(pickIn(s, lines) == lines.lineIndex("plain"));
}

TEST_CASE("npc slice: a since_deed bound of 1440 minutes holds across midnight to the minute") {
    Slice s;
    const NpcLineTable within = linesFor(s, R"(line_id,npc_id,kind,lead,priority,cooldown_min,once,cond
plain,npc_shop_01,greet,any,1,0,0,
thanks,npc_shop_01,greet,any,5,0,0,"mem:deed=gate_mended>=0;since_deed:gate_mended<=1440"
)");
    const NpcLineTable after = linesFor(s, R"(line_id,npc_id,kind,lead,priority,cooldown_min,once,cond
plain,npc_shop_01,greet,any,1,0,0,
late,npc_shop_01,greet,any,5,0,0,"mem:deed=gate_mended>=0;since_deed:gate_mended>=1440"
)");
    const std::uint32_t d = 2u * 1440u + 23u * 60u + 31u;  // 23:31 on day 2
    auto at = [&](std::uint32_t minute) { npcCheckpoint(*s.w, s.t, minute, 0); };
    at(d);
    REQUIRE(gate(s, 9, true) != 0u);
    CHECK(npcDeedAt(*s.w, s.w->nextSeq - 1u)->minute == d);
    const int thanks = within.lineIndex("thanks"), late = after.lineIndex("late");
    const int plainW = within.lineIndex("plain"), plainA = after.lineIndex("plain");
    CHECK(pickIn(s, within) == thanks);
    CHECK(pickIn(s, after) == plainA);
    at(3u * 1440u + 29u);  // past midnight
    CHECK(pickIn(s, within) == thanks);
    CHECK(pickIn(s, after) == plainA);
    at(d + 1439u);
    CHECK(pickIn(s, within) == thanks);
    CHECK(pickIn(s, after) == plainA);
    at(d + 1440u);  // both bounds hold on the minute
    CHECK(pickIn(s, within) == thanks);
    CHECK(pickIn(s, after) == late);
    at(d + 1441u);
    CHECK(pickIn(s, within) == plainW);
    CHECK(pickIn(s, after) == late);
}

TEST_CASE("npc slice: hearsay with no known doer matches neither by=self nor by=other; the line without by plays") {
    constexpr const char* kKnockLines = R"(line_id,npc_id,kind,lead,priority,cooldown_min,once,cond
plain,npc_shop_01,greet,any,1,0,0,
knock_self,npc_shop_01,greet,any,5,0,0,"mem:deed=van_knockdown,by=self>=0"
knock_other,npc_shop_01,greet,any,4,0,0,"mem:deed=van_knockdown,by=other>=0"
knock_any,npc_shop_01,greet,any,2,0,0,"mem:deed=van_knockdown>=0"
)";
    Slice s;
    const NpcLineTable lines = linesFor(s, kKnockLines);
    s.setTime(1, 12.0);
    const std::uint32_t seq = npcOnVanKnockdown(*s.w, s.t, s.g, s.where({}, 0), kNoNpc);  // no important NPC saw it
    REQUIRE(seq != 0u);
    CHECK(npcDeedAt(*s.w, seq)->lead == 0u);
    s.setTime(1, 13.5);  // street hearsay on a busy afternoon
    const NpcMemoryEntry* m = npcFindMemory(*s.w, s.npc("npc_shop_01"), seq);
    REQUIRE(m != nullptr);
    CHECK(m->hops == static_cast<std::uint8_t>(NpcLearn::Hop1));
    CHECK(m->leads == 0u);
    CHECK(m->lead == kNoLead);
    for (const std::uint8_t lead : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{2}, kNoLead}) {
        INFO("lead " << int(lead));
        CHECK(pickIn(s, lines, lead) == lines.lineIndex("knock_any"));
    }
    // Seen first hand, the doer is known.
    Slice k;
    const NpcLineTable kl = linesFor(k, kKnockLines);
    REQUIRE(npcOnVanKnockdown(*k.w, k.t, k.g, k.where({"npc_shop_01"}, 0), kNoNpc) != 0u);
    CHECK(pickIn(k, kl, 0) == kl.lineIndex("knock_self"));
    CHECK(pickIn(k, kl, 1) == kl.lineIndex("knock_other"));
}

TEST_CASE("npc slice: the debug view of an NPC with a full memory") {
    Slice s;
    const std::uint16_t shop = s.npc("npc_shop_01");
    REQUIRE(npcOnGift(*s.w, s.t, s.g, s.where({}, 0), shop, true) != 0u);
    REQUIRE(gate(s, 9, false, 1) != 0u);
    for (int i = 0; i < 16; ++i) {
        NpcWhere p = s.where({"npc_shop_01"}, 2);
        p.place = static_cast<std::uint16_t>(20 + i);
        npcOnHelpedPedestrian(*s.w, s.t, s.g, p);
    }
    const NpcState& st = s.w->npcs[shop];
    REQUIRE(st.memCount == kMemPerNpc);  // 18 learned, the two weakest folded away
    const int help = s.t.tagIndex("help");
    REQUIRE(help >= 0);
    CHECK(st.aggregates[static_cast<std::size_t>(help)].count == 2u);
    NpcDebugView v;
    const auto a0 = ftcore_test::allocStats();
    REQUIRE(npcDebugView(*s.w, s.t, shop, v));
    const auto a1 = ftcore_test::allocStats();
    CHECK(a1.count == a0.count);
    CHECK(v.memCount == kMemPerNpc);
    CHECK(v.topCount == 5u);
    for (std::size_t i = 1; i < v.topCount; ++i) CHECK(v.top[i - 1].salience >= v.top[i].salience);
    std::uint32_t stronger = 0;
    for (std::uint32_t i = 0; i < st.memCount; ++i)
        if (npcSalience(*s.w, s.t, shop, st.mem[i]) > v.top[4].salience) ++stronger;
    CHECK(stronger <= 4u);
    // The two pinned first-hand memories lead the view; the rest are the help the shopkeeper saw.
    CHECK(v.top[0].pinned);
    CHECK(v.top[1].pinned);
    for (std::size_t i = 2; i < v.topCount; ++i) {
        CHECK_FALSE(v.top[i].pinned);
        CHECK(v.top[i].type == s.g.helpedPedestrian);
        CHECK(v.top[i].leads == 0b100u);
    }
}
