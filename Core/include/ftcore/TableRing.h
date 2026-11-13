#pragma once

// TableRing: the ring of a set table in the arena (TableSet.h is its clock), who sits in it, and the sums that share
// a blow among them. Header only, no allocation.
//
// * The ring: radius metres round the setter's place when the table began, fixed from then on. A body's edge is its
//   distance from the centre less its body radius.
// * Who sits (sit, when the table sets): the setter (the player) always; the ally while it is up and its edge is
//   within the radius; each living enemy within the radius that may sit (enemyMaySit). An enemy turned away only
//   because its armour is on in an attack's startup or active part is tried again every tick while the table is set
//   (still within the radius); one turned away for any other reason, or arriving later, never sits.
// * Who stands (update, every tick while set): a seated body other than the setter whose edge goes past radius +
//   standMargin stands, and never sits again at this table. A seated body that dies or goes down leaves at once.
//   The host calls TableSet::checkBodies(bodies()) after sit and whenever update reports a change.
// * The share (ArenaFight::setTableSeats with seats()): an enemy's blow on a seated lead is split evenly among the
//   bodies at the table (tableEvenSplit). A seated enemy's part is times enemyShare, and times carryShortfall for
//   every carry grade past the table's seats (tableEnemyShare).

#include "ftcore/ArenaFight.h"
#include "ftcore/TableSet.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ftcore {

struct TableRingTuning {
    float radius = 6.f;          // m round the setter's place: 1..30
    float standMargin = 1.f;     // m past the ring a seated body's edge may go before it stands: 0..10
    float carryShortfall = 0.5f; // a seated enemy's share, times this per carry grade past the seats: 0..1
};

inline bool validTableRingTuning(const TableRingTuning& t) {
    return t.radius >= 1.f && t.radius <= 30.f && t.standMargin >= 0.f && t.standMargin <= 10.f &&
           t.carryShortfall >= 0.f && t.carryShortfall <= 1.f;
}

// Edges are compared with this much slack, so a body standing exactly on a line counts as on it.
inline constexpr float kTableEdgeSlack = 1e-4f;

// A body's edge: its distance from the ring's centre less its radius (negative inside a body over the centre).
inline float tableEdge(Vec2 at, float bodyRadius, Vec2 centre) {
    const float dx = at.x - centre.x;
    const float dy = at.y - centre.y;
    return std::sqrt(dx * dx + dy * dy) - bodyRadius;
}
inline bool tableEdgeIn(float edge, float radius) { return edge <= radius + kTableEdgeSlack; }
inline bool tableEdgeOut(float edge, float radius, float margin) { return edge > radius + margin + kTableEdgeSlack; }

// The cooldown after a table clears, longer the smaller the Load cap: cooldown x 100 / cap, at most 300 s
// (cap 100: as given; cap 40: x 2.5; a cap below 1 counts as 1).
inline double tableCooldownForCap(double cooldown, int cap) {
    const int c = std::max(cap, 1);
    return std::min(cooldown * 100.0 / static_cast<double>(c), 300.0);
}

// A seated enemy's multiplier on its even part: enemyShare x shortfall ^ max(0, carry - seats). Carry 0: enemyShare.
inline float tableEnemyShare(float enemyShare, int carry, int seats, float shortfall) {
    float m = std::clamp(enemyShare, 0.f, 4.f);
    const float s = std::clamp(shortfall, 0.f, 1.f);
    for (int k = std::max(0, carry - seats); k > 0 && m > 0.f; --k) {
        m *= s;
        if (m < 1e-6f) m = 0.f;
    }
    return m;
}

// The even split of `amount` hp among `bodies` at the table: each takes amount / bodies (whole hp, rounded down) and
// the body struck takes the rest as well. Fewer than two bodies: the struck one takes it all.
struct TableEvenSplit {
    int each = 0;
    int struck = 0;
};
inline TableEvenSplit tableEvenSplit(int amount, int bodies) {
    TableEvenSplit r;
    if (amount <= 0) return r;
    if (bodies < 2) {
        r.struck = amount;
        return r;
    }
    r.each = amount / bodies;
    r.struck = amount - r.each * (bodies - 1);
    return r;
}

// A seated enemy's part of the even split: round(each x share), halves up.
inline int tableEnemyPart(int each, float share) {
    if (each <= 0 || !(share > 0.f)) return 0;
    return static_cast<int>(std::floor(static_cast<double>(each) * share + 0.5));
}

// Whether enemy i of `fight` may sit now: alive, its kind seatable, and not in an attack's startup or active part
// with armour on. `armourOnly` is set when the armour is the only thing in the way.
inline bool enemyMaySit(const ArenaFight& fight, std::size_t i, bool* armourOnly = nullptr) {
    if (armourOnly) *armourOnly = false;
    if (i >= fight.enemyCount()) return false;
    const ArenaEnemyOut& e = fight.output().enemies[i];
    const ArenaEnemyKind& k = fight.def().enemies[i];
    if (e.brain == ArenaBrain::Dead || !k.seatable) return false;
    const bool armoured = e.brain == ArenaBrain::Attack &&
                          (e.part == ArenaPart::Startup || e.part == ArenaPart::Active) && e.move < k.moveCount &&
                          k.moves[e.move].attack.armor > 0.f;
    if (armoured && armourOnly) *armourOnly = true;
    return !armoured;
}

// The bodies' bits: the player, the ally, then enemy i at bit 2 + i.
inline constexpr std::uint16_t kTablePlayerBit = 1u << 0;
inline constexpr std::uint16_t kTableAllyBit = 1u << 1;
inline constexpr std::uint16_t tableEnemyBit(std::size_t i) { return static_cast<std::uint16_t>(1u << (2 + i)); }

struct TableRingChange {
    int stood = 0;   // bodies that stood this update
    int left = 0;    // bodies that died or went down
    int sat = 0;     // enemies seated on a retry
    bool any() const { return stood + left + sat > 0; }
};

class TableRing {
public:
    TableRing() { reset({}); }

    bool reset(const TableRingTuning& tuning) {
        const bool ok = validTableRingTuning(tuning);
        tuning_ = ok ? tuning : TableRingTuning{};
        clear();
        return ok;
    }

    // The ring's place: the setter's position when the table begins. Clears the seats.
    void place(Vec2 centre) {
        clear();
        centre_ = centre;
        placed_ = true;
    }
    // Clears the seats (the table cleared, guttered or failed) and the place.
    void clear() {
        seated_ = stood_ = waiting_ = 0;
        placed_ = false;
    }

    // The table set: seats the setter and every body in the ring that may sit. Returns bodies().
    int sit(const ArenaFight& fight, const ArenaBodies& at) {
        seated_ = stood_ = waiting_ = 0;
        if (!placed_) return 0;
        if (fight.hp() > 0) seated_ |= kTablePlayerBit;
        if (fight.allyAlive() && tableEdgeIn(tableEdge(at.ally, fight.allyRadius(), centre_), tuning_.radius))
            seated_ |= kTableAllyBit;
        for (std::size_t i = 0; i < fight.enemyCount() && i < kMaxArenaEnemies; ++i) {
            if (!tableEdgeIn(tableEdge(at.enemies[i], fight.def().enemies[i].bodyRadius, centre_), tuning_.radius))
                continue;
            bool armourOnly = false;
            if (enemyMaySit(fight, i, &armourOnly)) seated_ |= tableEnemyBit(i);
            else if (armourOnly) waiting_ |= tableEnemyBit(i);
        }
        return bodies();
    }

    // Every tick while the table is set: armour retries, then stand-ups, then the ones who died or went down.
    TableRingChange update(const ArenaFight& fight, const ArenaBodies& at) {
        TableRingChange c;
        if (!placed_ || seated_ == 0) return c;
        for (std::size_t i = 0; i < fight.enemyCount() && i < kMaxArenaEnemies; ++i) {
            const std::uint16_t bit = tableEnemyBit(i);
            if (!(waiting_ & bit)) continue;
            bool armourOnly = false;
            const bool in = tableEdgeIn(tableEdge(at.enemies[i], fight.def().enemies[i].bodyRadius, centre_), tuning_.radius);
            if (in && enemyMaySit(fight, i, &armourOnly)) {
                waiting_ &= static_cast<std::uint16_t>(~bit);
                seated_ |= bit;
                ++c.sat;
            } else if (!in || !armourOnly) {
                waiting_ &= static_cast<std::uint16_t>(~bit); // gone, or turned away for good
            }
        }
        if ((seated_ & kTablePlayerBit) && fight.hp() <= 0) {
            seated_ &= static_cast<std::uint16_t>(~kTablePlayerBit);
            ++c.left;
        }
        if (seated_ & kTableAllyBit) {
            if (!fight.allyAlive()) {
                seated_ &= static_cast<std::uint16_t>(~kTableAllyBit);
                ++c.left;
            } else if (tableEdgeOut(tableEdge(at.ally, fight.allyRadius(), centre_), tuning_.radius, tuning_.standMargin)) {
                seated_ &= static_cast<std::uint16_t>(~kTableAllyBit);
                stood_ |= kTableAllyBit;
                ++c.stood;
            }
        }
        for (std::size_t i = 0; i < fight.enemyCount() && i < kMaxArenaEnemies; ++i) {
            const std::uint16_t bit = tableEnemyBit(i);
            if (!(seated_ & bit)) continue;
            if (fight.output().enemies[i].brain == ArenaBrain::Dead) {
                seated_ &= static_cast<std::uint16_t>(~bit);
                ++c.left;
            } else if (tableEdgeOut(tableEdge(at.enemies[i], fight.def().enemies[i].bodyRadius, centre_), tuning_.radius,
                                    tuning_.standMargin)) {
                seated_ &= static_cast<std::uint16_t>(~bit);
                stood_ |= bit;
                ++c.stood;
            }
        }
        return c;
    }

    // The bodies a table here would seat now (the setter counted): what an autopilot checks before it begins.
    int wouldSit(const ArenaFight& fight, const ArenaBodies& at, Vec2 centre) const {
        TableRing probe = *this;
        probe.place(centre);
        return probe.sit(fight, at);
    }

    // The fight's share rule for the seats now. `enemyShare` (TableTuning) and `seats` (the table's seats,
    // PlaceTable::tableSeats) set each seated enemy's part with its kind's carry grade.
    ArenaTableSeats seats(const ArenaFight& fight, float enemyShare, int seats) const {
        ArenaTableSeats s;
        s.player = (seated_ & kTablePlayerBit) != 0;
        s.ally = (seated_ & kTableAllyBit) != 0;
        for (std::size_t i = 0; i < fight.enemyCount() && i < kMaxArenaEnemies; ++i) {
            if (!(seated_ & tableEnemyBit(i))) continue;
            s.enemies = static_cast<std::uint8_t>(s.enemies | (1u << i));
            s.enemyShare[i] = tableEnemyShare(enemyShare, fight.def().enemies[i].carry, seats, tuning_.carryShortfall);
        }
        return s;
    }

    int bodies() const {
        int count = 0;
        for (std::uint16_t m = seated_; m; m = static_cast<std::uint16_t>(m & (m - 1))) ++count;
        return count;
    }
    std::uint16_t seated() const { return seated_; }
    std::uint16_t stood() const { return stood_; }
    std::uint16_t waiting() const { return waiting_; }
    bool placed() const { return placed_; }
    Vec2 centre() const { return centre_; }
    const TableRingTuning& tuning() const { return tuning_; }

private:
    TableRingTuning tuning_{};
    Vec2 centre_{};
    bool placed_ = false;
    std::uint16_t seated_ = 0;
    std::uint16_t stood_ = 0;
    std::uint16_t waiting_ = 0;
};

// The player's key for the table: hold Release while the table could begin (TableSet Ready, the Load full, enough
// bodies in reach). The press is then held back from the fight; held holdTime it sets the table (Begin); let go
// sooner, it is passed on as the Release it was (PassRelease, at the let-go). If the table stops being possible during
// the hold, the hold ends and nothing is passed on (Cancelled). A press while the table could not begin is the
// fight's (None).
enum class TableHoldResult : std::uint8_t { None, Holding, Begin, PassRelease, Cancelled }; // append only

class TableHold {
public:
    static constexpr double kDefaultHoldTime = 0.5; // s

    explicit TableHold(double holdTime = kDefaultHoldTime) { reset(holdTime); }

    void reset(double holdTime = kDefaultHoldTime) {
        holdTime_ = std::isfinite(holdTime) ? std::clamp(holdTime, 0.0, 5.0) : kDefaultHoldTime;
        holding_ = false;
        start_ = 0.0;
        share_ = 0.f;
    }

    // One step: `pressed` the Release pressed this step, `down` its key down now, `eligible` the table could begin.
    // Holding, Begin and Cancelled: hold the step's Release back from the fight. PassRelease: send a Release now.
    TableHoldResult step(bool pressed, bool down, bool eligible, double t) {
        if (!holding_) {
            share_ = 0.f;
            if (!(pressed && eligible)) return TableHoldResult::None;
            holding_ = true;
            start_ = t;
        }
        if (!eligible) {
            holding_ = false;
            share_ = 0.f;
            return TableHoldResult::Cancelled;
        }
        const double held = std::isfinite(t) ? std::max(0.0, t - start_) : 0.0;
        if (held + 1e-9 >= holdTime_) {
            holding_ = false;
            share_ = 0.f;
            return TableHoldResult::Begin;
        }
        if (!down) {
            holding_ = false;
            share_ = 0.f;
            return TableHoldResult::PassRelease;
        }
        share_ = holdTime_ > 0.0 ? static_cast<float>(held / holdTime_) : 1.f;
        return TableHoldResult::Holding;
    }

    bool holding() const { return holding_; }
    float share() const { return share_; } // 0..1 toward Begin while holding
    double holdTime() const { return holdTime_; }

private:
    double holdTime_ = kDefaultHoldTime;
    double start_ = 0.0;
    float share_ = 0.f;
    bool holding_ = false;
};

// The table as the HUD and effects read it: plain data, filled by tableView.
struct ArenaTableView {
    std::uint8_t state = 0;    // TableState: Ready, Setting, Set, Cooling
    float timeLeft = 0.f;      // s left of Setting or Set (0 otherwise)
    float duration = 0.f;      // s, the whole of that state (> 0 while Setting or Set)
    float cooldownLeft = 0.f;  // s left of Cooling
    float centreX = 0.f;       // m, the ring's centre (while one is placed)
    float centreY = 0.f;
    float radius = 0.f;        // m (0 while no ring is placed)
    int bodies = 0;            // seated now
    std::uint16_t seated = 0;  // bit 0 the player, bit 1 the ally, bit 2 + i enemy i
    float holdShare = 0.f;     // 0..1, the player's Release hold toward setting it
};

inline ArenaTableView tableView(const TableSet& table, const TableRing& ring, const TableHold& hold, double t) {
    ArenaTableView v;
    const TableState s = table.state();
    v.state = static_cast<std::uint8_t>(s);
    const float left = std::isfinite(t) ? static_cast<float>(std::max(0.0, table.until() - t)) : 0.f;
    if (s == TableState::Setting || s == TableState::Set) {
        v.timeLeft = left;
        v.duration = static_cast<float>(s == TableState::Setting ? table.tuning().setTime : table.tuning().setDuration);
        if (!(v.duration > 0.f)) v.duration = 1e-3f;
    } else if (s == TableState::Cooling) {
        v.cooldownLeft = left;
    }
    if (ring.placed()) {
        v.centreX = ring.centre().x;
        v.centreY = ring.centre().y;
        v.radius = ring.tuning().radius;
    }
    v.bodies = ring.bodies();
    v.seated = ring.seated();
    v.holdShare = hold.share();
    return v;
}

} // namespace ftcore
