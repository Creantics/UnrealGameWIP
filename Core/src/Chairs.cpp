#include "ftcore/Chairs.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

const char* chairKindName(ChairKind k) {
    switch (k) {
    case ChairKind::HealCharges: return "healCharges";
    case ChairKind::LoadGain: return "loadGain";
    case ChairKind::TakeHalf: return "takeHalf";
    default: return "none";
    }
}

const ChairRow* ChairTable::findKey(std::string_view key) const {
    for (const ChairRow& r : rows)
        if (r.key == key) return &r;
    return nullptr;
}

const ChairRow* ChairTable::findPerson(PersonId person) const {
    if (person == kNoPerson) return nullptr;
    for (const ChairRow& r : rows)
        if (r.person == person) return &r;
    return nullptr;
}

namespace {

struct Range {
    double lo;
    double hi;
};

// The ranges of a and b per kind.
bool rangesOf(ChairKind k, Range& a, Range& b) {
    switch (k) {
    case ChairKind::LoadGain: a = {0.0, 0.5}; b = {1.0, 15.0}; return true;
    case ChairKind::HealCharges: a = {0.0, 3.0}; b = {0.0, 0.0}; return true;
    case ChairKind::TakeHalf: a = {0.0, 0.2}; b = {0.0, 0.1}; return true;
    default: return false;
    }
}

bool kindOf(std::string_view s, ChairKind& k) {
    if (s == "loadGain") k = ChairKind::LoadGain;
    else if (s == "healCharges") k = ChairKind::HealCharges;
    else if (s == "takeHalf") k = ChairKind::TakeHalf;
    else return false;
    return true;
}

bool lowerKey(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

} // namespace

bool importChairTable(std::string_view json, ChairTable& out, std::string* error) {
    out = ChairTable{};
    std::vector<std::string> errors;
    const auto finish = [&]() {
        if (errors.empty()) return true;
        if (error) {
            error->clear();
            for (const std::string& e : errors) {
                if (!error->empty()) *error += '\n';
                *error += e;
            }
        }
        return false;
    };

    JsonDocument doc;
    std::string err;
    if (!doc.parse(json, &err)) {
        errors.push_back("chairs file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("chairs file: expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !(v.isInteger() && v.integer() == 1)) errors.push_back("version: expected 1");
    const JsonValue list = root["chairs"];
    if (!list.isArray()) {
        errors.push_back("chairs: expected an array");
        return finish();
    }

    ChairTable table;
    for (std::uint32_t i = 0; i < list.size(); ++i) {
        const JsonValue row = list[i];
        const std::string path = "chairs[" + std::to_string(i) + "]";
        if (!row.isObject()) {
            errors.push_back(path + ": expected an object");
            continue;
        }
        ChairRow r;
        bool ok = true;
        const JsonValue person = row["person"];
        if (!(person.isInteger() && person.integer() >= 1 && person.integer() <= 65535)) {
            errors.push_back(path + ".person: expected a whole number 1..65535");
            ok = false;
        } else {
            r.person = static_cast<PersonId>(person.integer());
        }
        const JsonValue key = row["key"];
        if (!key.isString() || !lowerKey(key.string())) {
            errors.push_back(path + ".key: expected a lower case id (a-z, 0-9, _)");
            ok = false;
        } else {
            r.key = std::string(key.string());
        }
        const JsonValue kind = row["kind"];
        Range ra{}, rb{};
        if (!kind.isString() || !kindOf(kind.string(), r.effect.kind) || !rangesOf(r.effect.kind, ra, rb)) {
            errors.push_back(path + ".kind: expected loadGain, healCharges or takeHalf");
            ok = false;
        } else {
            const JsonValue a = row["a"];
            const double av = a.number(-1.0);
            if (!a.isNumber() || !std::isfinite(av) || av < ra.lo || av > ra.hi) {
                errors.push_back(path + ".a: expected a number in [" + std::to_string(ra.lo) + ", " +
                                 std::to_string(ra.hi) + "]");
                ok = false;
            } else if (r.effect.kind == ChairKind::HealCharges && av != std::floor(av)) {
                errors.push_back(path + ".a: expected whole charges");
                ok = false;
            } else {
                r.effect.a = static_cast<float>(av);
            }
            const JsonValue b = row["b"];
            const double bv = b.isAbsent() ? 0.0 : b.number(-1.0);
            if ((!b.isAbsent() && !b.isNumber()) || !std::isfinite(bv) || bv < rb.lo || bv > rb.hi) {
                errors.push_back(path + ".b: expected a number in [" + std::to_string(rb.lo) + ", " +
                                 std::to_string(rb.hi) + "]");
                ok = false;
            } else {
                r.effect.b = static_cast<float>(bv);
            }
        }
        if (r.person != kNoPerson && table.findPerson(r.person)) {
            errors.push_back(path + ".person: duplicate " + std::to_string(r.person));
            ok = false;
        }
        if (!r.key.empty() && table.findKey(r.key)) {
            errors.push_back(path + ".key: duplicate " + r.key);
            ok = false;
        }
        if (ok) table.rows.push_back(std::move(r));
    }
    if (!errors.empty()) return finish();
    out = std::move(table);
    return true;
}

namespace {

float sumOf(const ArenaChairs& c, ChairKind k, bool useB) {
    float s = 0.f;
    for (std::size_t i = 0; i < c.count && i < c.named.size(); ++i)
        if (c.named[i].kind == k) s += useB ? c.named[i].b : c.named[i].a;
    return s;
}

} // namespace

int ArenaChairs::healCharges() const {
    const float s = sumOf(*this, ChairKind::HealCharges, false);
    return std::clamp(static_cast<int>(std::lround(s)), 0, kChairHealChargesCap);
}
float ArenaChairs::loadGain() const { return std::clamp(sumOf(*this, ChairKind::LoadGain, false), 0.f, kChairLoadGainCap); }
float ArenaChairs::loadGainRadius() const {
    float r = 0.f;
    for (std::size_t i = 0; i < count && i < named.size(); ++i)
        if (named[i].kind == ChairKind::LoadGain) r = std::max(r, named[i].b);
    return r;
}
float ArenaChairs::takeHalfWindow() const {
    return std::clamp(sumOf(*this, ChairKind::TakeHalf, false), 0.f, kChairTakeHalfWindowCap);
}
float ArenaChairs::takeHalfHeal() const {
    return std::clamp(sumOf(*this, ChairKind::TakeHalf, true), 0.f, kChairTakeHalfHealCap);
}

ArenaChairs chairsFor(const PlaceTable& places, const ChairTable& table) {
    ArenaChairs c;
    for (int i = 0; i < places.seats(); ++i) {
        const Place& p = places.place(i);
        if (p.state != PlaceState::Named) continue;
        const ChairRow* row = table.findPerson(p.person);
        if (!row || c.count >= c.named.size()) continue;
        c.named[c.count++] = row->effect;
    }
    return c;
}

} // namespace ftcore
