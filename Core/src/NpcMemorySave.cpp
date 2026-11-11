#include "ftcore/NpcMemory.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cstdio>
#include <limits>

// The NPC memory save as JSON text: what npcCaptureSave gives, written field for field. Ids stay strings, so the
// text survives table changes (npcApplySave drops and reports the ids it no longer knows).

namespace ftcore {
namespace {

void quoted(std::string& out, std::string_view s) {
    out += '"';
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
            out += buf;
        } else out += ch;
    }
    out += '"';
}

void num(std::string& out, const char* key, std::int64_t v, bool comma = true) {
    if (comma) out += ',';
    out += '"';
    out += key;
    out += "\":";
    out += std::to_string(v);
}

template <class T> T clampInt(const JsonValue& v, T fallback = T{}) {
    if (!v.isInteger()) return fallback;
    const std::int64_t x = v.integer();
    const std::int64_t lo = static_cast<std::int64_t>(std::numeric_limits<T>::min());
    const std::int64_t hi = static_cast<std::int64_t>(std::numeric_limits<T>::max());
    return static_cast<T>(std::clamp(x, lo, hi));
}

bool fail(std::string* error, const char* why) {
    if (error) *error = why;
    return false;
}

}  // namespace

std::string encodeNpcMemorySave(const NpcMemorySave& s) {
    std::string out = "{";
    num(out, "format", s.format, false);
    num(out, "now", s.now);
    num(out, "nextSeq", s.nextSeq);
    num(out, "droppedRumours", s.droppedRumours);
    out += ",\"npcs\":[";
    for (std::size_t i = 0; i < s.npcs.size(); ++i) {
        const NpcMemorySaveNpc& n = s.npcs[i];
        if (i) out += ',';
        out += "{\"id\":";
        quoted(out, n.id);
        out += ",\"axes\":[" + std::to_string(n.axes[0]) + ',' + std::to_string(n.axes[1]) + ',' +
               std::to_string(n.axes[2]) + ']';
        num(out, "axesTs", n.axesTs);
        out += ",\"offsets\":[";
        for (std::size_t l = 0; l < n.offsets.size(); ++l) {
            if (l) out += ',';
            out += '[' + std::to_string(n.offsets[l][0]) + ',' + std::to_string(n.offsets[l][1]) + ',' +
                   std::to_string(n.offsets[l][2]) + ']';
        }
        out += "],\"offsetsTs\":[";
        for (std::size_t l = 0; l < n.offsetsTs.size(); ++l) out += (l ? "," : "") + std::to_string(n.offsetsTs[l]);
        out += "],\"stance\":[";
        for (std::size_t l = 0; l < n.stance.size(); ++l) out += (l ? "," : "") + std::to_string(n.stance[l]);
        out += "],\"dead\":";
        out += n.dead ? "true" : "false";
        num(out, "deadAt", n.deadAt);
        out += ",\"mem\":[";
        for (std::size_t m = 0; m < n.mem.size(); ++m) {
            const NpcMemorySaveEntry& e = n.mem[m];
            if (m) out += ',';
            out += "{\"type\":";
            quoted(out, e.type);
            num(out, "seq", e.seq);
            num(out, "deedMinute", e.deedMinute);
            num(out, "ts", e.ts);
            num(out, "learned", e.learned);
            num(out, "place", e.place);
            num(out, "s0", e.s0);
            num(out, "hops", e.hops);
            num(out, "count", e.count);
            num(out, "lead", e.lead);
            num(out, "leads", e.leads);
            out += ",\"pinned\":";
            out += e.pinned ? "true" : "false";
            out += '}';
        }
        out += "],\"aggregates\":[";
        for (std::size_t a = 0; a < n.aggregates.size(); ++a) {
            if (a) out += ',';
            out += "{\"tag\":";
            quoted(out, n.aggregates[a].first);
            num(out, "count", n.aggregates[a].second.count);
            num(out, "lastMinute", n.aggregates[a].second.lastMinute);
            out += '}';
        }
        out += "],\"seen\":[";
        for (std::size_t q = 0; q < n.seen.size(); ++q) out += (q ? "," : "") + std::to_string(n.seen[q]);
        out += "],\"recent\":[";
        for (std::size_t r = 0; r < n.recent.size(); ++r) {
            if (r) out += ',';
            out += "{\"type\":";
            quoted(out, n.recent[r].first);
            num(out, "minute", n.recent[r].second);
            out += '}';
        }
        out += "]}";
    }
    out += "],\"deeds\":[";
    for (std::size_t i = 0; i < s.deeds.size(); ++i) {
        const NpcMemorySaveDeed& d = s.deeds[i];
        if (i) out += ',';
        out += "{\"type\":";
        quoted(out, d.type);
        out += ",\"victim\":";
        quoted(out, d.victim);
        num(out, "seq", d.seq);
        num(out, "minute", d.minute);
        num(out, "place", d.place);
        num(out, "district", d.district);
        num(out, "lead", d.lead);
        num(out, "flags", d.flags);
        out += ",\"witnesses\":[";
        for (std::size_t k = 0; k < d.witnesses.size(); ++k) {
            if (k) out += ',';
            quoted(out, d.witnesses[k]);
        }
        out += "]}";
    }
    out += "],\"rumours\":[";
    for (std::size_t i = 0; i < s.rumours.size(); ++i) {
        const NpcMemorySaveRumour& r = s.rumours[i];
        if (i) out += ',';
        out += "{\"target\":";
        quoted(out, r.target);
        num(out, "seq", r.seq);
        num(out, "due", r.due);
        num(out, "hops", r.hops);
        num(out, "lead", r.lead);
        num(out, "closeness", r.closeness);
        out += '}';
    }
    out += "],\"expectations\":[";
    for (std::size_t i = 0; i < s.expectations.size(); ++i) {
        if (i) out += ',';
        out += "{\"id\":";
        quoted(out, s.expectations[i].first);
        num(out, "state", s.expectations[i].second);
        out += '}';
    }
    out += "],\"said\":[";
    for (std::size_t i = 0; i < s.said.size(); ++i) {
        if (i) out += ',';
        out += "{\"line\":";
        quoted(out, s.said[i].first);
        num(out, "at", s.said[i].second);
        out += '}';
    }
    out += "]}";
    return out;
}

bool decodeNpcMemorySave(std::string_view text, NpcMemorySave& out, std::string* error) {
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(text, &parseError)) {
        if (error) *error = "npc save is not valid JSON: " + parseError;
        return false;
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) return fail(error, "npc save: the top level must be an object");
    if (!root["format"].isInteger() || root["format"].integer() < 1 || root["format"].integer() > NpcMemorySave::kFormat)
        return fail(error, "npc save: unsupported or missing format");
    // The npcs list is required (an empty save would wipe every memory on apply); the other lists must be arrays when
    // present (absent counts as empty). A wrong type means a damaged file.
    if (!root["npcs"].isArray()) return fail(error, "npc save: no npcs list");
    for (const char* key : {"npcs", "deeds", "rumours", "expectations", "said"})
        if (!root[key].isAbsent() && !root[key].isArray()) return fail(error, "npc save: a list is not an array");

    NpcMemorySave s;
    s.format = static_cast<int>(root["format"].integer());
    s.now = clampInt<std::uint32_t>(root["now"]);
    s.nextSeq = clampInt<std::uint32_t>(root["nextSeq"], 1);
    s.droppedRumours = clampInt<std::uint32_t>(root["droppedRumours"]);

    const JsonValue npcs = root["npcs"];
    s.npcs.reserve(npcs.size());
    for (std::uint32_t i = 0; i < npcs.size(); ++i) {
        const JsonValue v = npcs[i];
        if (!v["id"].isString()) return fail(error, "npc save: an npc has no id");
        NpcMemorySaveNpc& n = s.npcs.emplace_back();
        n.id = std::string(v["id"].string());
        for (std::uint32_t a = 0; a < 3; ++a) n.axes[a] = clampInt<std::int16_t>(v["axes"][a]);
        n.axesTs = clampInt<std::uint32_t>(v["axesTs"]);
        for (std::uint32_t l = 0; l < n.offsets.size(); ++l) {
            for (std::uint32_t a = 0; a < 3; ++a) n.offsets[l][a] = clampInt<std::int8_t>(v["offsets"][l][a]);
            n.offsetsTs[l] = clampInt<std::uint32_t>(v["offsetsTs"][l]);
        }
        for (std::uint32_t l = 0; l < n.stance.size(); ++l) n.stance[l] = clampInt<std::uint8_t>(v["stance"][l]);
        n.dead = v["dead"].boolean();
        n.deadAt = clampInt<std::uint32_t>(v["deadAt"]);
        const JsonValue mem = v["mem"];
        for (std::uint32_t m = 0; m < mem.size(); ++m) {
            const JsonValue e = mem[m];
            if (!e["type"].isString()) return fail(error, "npc save: a memory has no type");
            NpcMemorySaveEntry& o = n.mem.emplace_back();
            o.type = std::string(e["type"].string());
            o.seq = clampInt<std::uint32_t>(e["seq"]);
            o.deedMinute = clampInt<std::uint32_t>(e["deedMinute"]);
            o.ts = clampInt<std::uint32_t>(e["ts"]);
            o.learned = clampInt<std::uint32_t>(e["learned"]);
            o.place = clampInt<std::uint16_t>(e["place"]);
            o.s0 = clampInt<std::int16_t>(e["s0"]);
            o.hops = clampInt<std::uint8_t>(e["hops"]);
            o.count = clampInt<std::uint8_t>(e["count"]);
            o.lead = clampInt<std::uint8_t>(e["lead"], kNoLead);
            o.leads = clampInt<std::uint8_t>(e["leads"]);  // absent in older text: npcApplySave rebuilds it
            o.pinned = e["pinned"].boolean();
        }
        const JsonValue agg = v["aggregates"];
        for (std::uint32_t a = 0; a < agg.size(); ++a) {
            if (!agg[a]["tag"].isString()) return fail(error, "npc save: an aggregate has no tag");
            NpcTagAggregate g;
            g.count = clampInt<std::uint16_t>(agg[a]["count"]);
            g.lastMinute = clampInt<std::uint32_t>(agg[a]["lastMinute"]);
            n.aggregates.emplace_back(std::string(agg[a]["tag"].string()), g);
        }
        for (std::uint32_t q = 0; q < v["seen"].size(); ++q) n.seen.push_back(clampInt<std::uint32_t>(v["seen"][q]));
        const JsonValue recent = v["recent"];
        for (std::uint32_t r = 0; r < recent.size(); ++r) {
            if (!recent[r]["type"].isString()) return fail(error, "npc save: a recent deed has no type");
            n.recent.emplace_back(std::string(recent[r]["type"].string()), clampInt<std::uint32_t>(recent[r]["minute"]));
        }
    }
    const JsonValue deeds = root["deeds"];
    for (std::uint32_t i = 0; i < deeds.size(); ++i) {
        const JsonValue v = deeds[i];
        if (!v["type"].isString()) return fail(error, "npc save: a deed has no type");
        NpcMemorySaveDeed& d = s.deeds.emplace_back();
        d.type = std::string(v["type"].string());
        d.victim = std::string(v["victim"].string());
        d.seq = clampInt<std::uint32_t>(v["seq"]);
        d.minute = clampInt<std::uint32_t>(v["minute"]);
        d.place = clampInt<std::uint16_t>(v["place"]);
        d.district = clampInt<std::uint16_t>(v["district"]);
        d.lead = clampInt<std::uint8_t>(v["lead"], kNoLead);
        d.flags = clampInt<std::uint8_t>(v["flags"]);
        for (std::uint32_t k = 0; k < v["witnesses"].size(); ++k)
            d.witnesses.emplace_back(v["witnesses"][k].string());
    }
    const JsonValue rumours = root["rumours"];
    for (std::uint32_t i = 0; i < rumours.size(); ++i) {
        const JsonValue v = rumours[i];
        if (!v["target"].isString()) return fail(error, "npc save: a rumour has no target");
        NpcMemorySaveRumour& r = s.rumours.emplace_back();
        r.target = std::string(v["target"].string());
        r.seq = clampInt<std::uint32_t>(v["seq"]);
        r.due = clampInt<std::uint32_t>(v["due"]);
        r.hops = clampInt<std::uint8_t>(v["hops"], 1);
        r.lead = clampInt<std::uint8_t>(v["lead"], kNoLead);
        r.closeness = clampInt<std::uint8_t>(v["closeness"]);
    }
    const JsonValue exps = root["expectations"];
    for (std::uint32_t i = 0; i < exps.size(); ++i) {
        if (!exps[i]["id"].isString()) return fail(error, "npc save: an expectation has no id");
        s.expectations.emplace_back(std::string(exps[i]["id"].string()), clampInt<std::uint8_t>(exps[i]["state"]));
    }
    const JsonValue said = root["said"];
    for (std::uint32_t i = 0; i < said.size(); ++i) {
        if (!said[i]["line"].isString()) return fail(error, "npc save: a said line has no id");
        s.said.emplace_back(std::string(said[i]["line"].string()), clampInt<std::uint32_t>(said[i]["at"]));
    }
    out = std::move(s);
    return true;
}

}  // namespace ftcore
