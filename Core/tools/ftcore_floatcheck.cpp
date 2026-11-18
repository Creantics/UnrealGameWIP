// ftcore_floatcheck: the float check and fix pass (ftcore/TerrainPocket.h) on a box table and its height field, without
// the engine: the same boxes the massing build checks (the table's visible boxes, plus the hero corner's bays with
// --hero 1, tops as built: massingTopNudges and massingGroundRaises), the floats before (the build's -FloatCheck), the
// fix pass (planFoundations), the retaining plinths after it with --plinths 1 (-Plinths), and the check rerun with the
// span rule on every box. An offline build step on the private tables.
//
//   ftcore_floatcheck --boxes B --r16 H --samples N --origin X,Y --spacing M [--hero 1] [--kit K] [--eave E]
//                     [--plinths 1 --anchor X,Y [--exempt-rect X0,Y0,X1,Y1[;...]]] [--fix 0] [--out CSV] [--report R]
// --fix 0: no fix pass (the plinths and the check after on the boxes as they stand).
//
// --kit K: the kit table (first column the box id); those boxes keep their shape (an under-box, never lowered).
// --eave E: the span rule's maxEave (default 1.5 m; decks keep their own 3.5 m, FloatSpec::kindEaves). Prints the three lists (fixed, spanned, failing) by id and their
// counts, and the floats the fixes made. Exit 0 on FLOATCHECK OK (none failing, none new), 1 otherwise, 2 on an error.

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "ftcore/HeroCorner.h"
#include "ftcore/MassingPlan.h"
#include "ftcore/Roads.h"
#include "ftcore/TerrainPocket.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <vector>

using namespace ftcore;

namespace {

bool readFile(const std::string& path, std::string& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

bool writeFile(const std::string& path, const std::string& s) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
    return true;
}

int die(const std::string& what) {
    std::fprintf(stderr, "ftcore_floatcheck: error: %s\n", what.c_str());
    return 2;
}

std::string num(double v, int places = 2) {
    char b[48];
    std::snprintf(b, sizeof b, "%.*f", places, v);
    return b;
}

bool parseList(const std::string& s, double out[], int n) {
    std::size_t p = 0;
    for (int i = 0; i < n; ++i) {
        const std::size_t q = s.find(',', p);
        const std::string t = s.substr(p, q == std::string::npos ? std::string::npos : q - p);
        char* end = nullptr;
        out[i] = std::strtod(t.c_str(), &end);
        if (end == t.c_str()) return false;
        if (q == std::string::npos) return i == n - 1;
        p = q + 1;
    }
    return true;
}

// The kit table's box ids: each line's first field, past a header and '#' comments.
void kitIds(const std::string& text, std::unordered_set<std::string>& out) {
    bool header = true;
    for (std::size_t p = 0; p < text.size();) {
        std::size_t q = text.find('\n', p);
        if (q == std::string::npos) q = text.size();
        std::string line = text.substr(p, q - p);
        p = q + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line.erase(0, 3);
        const std::size_t lead = line.find_first_not_of(" \t\"");
        if (lead == std::string::npos || line[lead] == '#') continue;
        if (header) {
            header = false;
            continue;
        }
        std::string id = line.substr(0, line.find(','));
        while (!id.empty() && (id.front() == '"' || id.front() == ' ')) id.erase(0, 1);
        while (!id.empty() && (id.back() == '"' || id.back() == ' ')) id.pop_back();
        if (!id.empty()) out.insert(id);
    }
}

std::string idList(const FloatOutcomes& o, const std::vector<MassingBox>& boxes, FloatOutcome which, bool withSpan) {
    std::string s;
    for (const FloatOutcomeRow& r : o.rows) {
        if (r.outcome != which) continue;
        s += (s.empty() ? "" : " ") + boxes[r.box].id;
        if (withSpan) s += "(" + num(r.span, 1) + "/" + num(r.overhang, 1) + ")";
    }
    return s;
}

const char* outcomeName(FloatOutcome o) {
    return o == FloatOutcome::Fixed ? "fixed" : o == FloatOutcome::Spanned ? "spanned" : "failing";
}

} // namespace

int main(int argc, char** argv) {
    std::string boxesPath, r16Path, kitPath, roadsPath, outPath, reportPath, anchorText, exemptText;
    double samples = 0.0, origin[2] = {0.0, 0.0}, spacing = 1.0, eave = 1.5;
    bool hero = false, plinths = false, fix = true;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--boxes") boxesPath = v;
        else if (k == "--r16") r16Path = v;
        else if (k == "--samples") samples = std::atof(v.c_str());
        else if (k == "--origin") {
            if (!parseList(v, origin, 2)) return die("--origin is X,Y");
        } else if (k == "--spacing") spacing = std::atof(v.c_str());
        else if (k == "--hero") hero = v == "1";
        else if (k == "--kit") kitPath = v;
        else if (k == "--eave") eave = std::atof(v.c_str());
        else if (k == "--plinths") plinths = v == "1";
        else if (k == "--fix") fix = v != "0";
        else if (k == "--anchor") anchorText = v;
        else if (k == "--exempt-rect") exemptText = v;
        else if (k == "--roads") roadsPath = v;
        else if (k == "--out") outPath = v;
        else if (k == "--report") reportPath = v;
        else return die("unknown switch " + k);
    }
    if (boxesPath.empty() || r16Path.empty() || samples < 2.0) return die("needs --boxes, --r16 and --samples");

    std::string text, err;
    if (!readFile(boxesPath, text)) return die("can't read " + boxesPath);
    std::vector<MassingBox> boxes;
    if (!importMassingBoxes(text, boxes, &err)) return die(boxesPath + ": " + err);
    const std::size_t tableBoxes = boxes.size();
    if (hero) {
        HeroCornerLayout layout;
        heroCornerLayout(HeroCornerSpec{}, layout);
        for (const MassingBox& bay : layout.bays) boxes.push_back(bay);
    }
    std::string raw;
    if (!readFile(r16Path, raw)) return die("can't read " + r16Path);
    const std::size_t n = static_cast<std::size_t>(samples);
    if (raw.size() != n * n * 2) return die("the r16 is not samples x samples u16");
    std::vector<std::uint16_t> grid(n * n);
    for (std::size_t k = 0; k < n * n; ++k)
        grid[k] = static_cast<std::uint16_t>(static_cast<std::uint8_t>(raw[2 * k]) | (static_cast<std::uint8_t>(raw[2 * k + 1]) << 8));
    const MassingGround ground{grid.data(), n, n, spacing, origin[0], origin[1]};

    // The rows the roads hide, as the build's road prepare does before it lays anything: cleared under a road, then
    // any still standing in a carriageway.
    std::size_t roadHides = 0;
    if (!roadsPath.empty()) {
        std::string roadText;
        if (!readFile(roadsPath, roadText)) return die("can't read " + roadsPath);
        std::vector<RoadBuild> builds;
        if (roadNetTable(roadText)) {
            std::vector<RoadNetStreet> net;
            std::vector<RoadCanal> canals;
            if (!importRoadNet(roadText, net, &err, &canals)) return die(roadsPath + ": " + err);
            builds.resize(1);
            if (!net.empty()) {
                roadNetBuild(net, ground, builds[0]);
                for (const RoadCanal& canal : canals) roadCanalBuild(canal, ground, builds[0]);
            }
        } else {
            std::vector<RoadSpec> roads;
            if (!importRoads(roadText, roads, &err)) return die(roadsPath + ": " + err);
            builds.resize(roads.size());
            for (std::size_t r = 0; r < roads.size(); ++r) roadBuild(roads[r], builds[r]);
        }
        for (const RoadBuild& build : builds) {
            for (const std::size_t i : roadClearedBoxes(build, boxes, 0.0)) {
                boxes[i].kind = "underpass";
                ++roadHides;
            }
            for (const std::size_t i : roadBoxClashes(build, boxes)) {
                boxes[i].kind = "underpass";
                ++roadHides;
            }
        }
    }

    // The boxes as the build lays them: the visible ones, tops nudged apart and up off the ground.
    std::vector<bool> visible(boxes.size()), flat(boxes.size());
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        visible[i] = massingVisible(massingRole(boxes[i]));
        flat[i] = visible[i] && !boxes[i].sloped;
    }
    std::vector<double> nudges, raises;
    massingTopNudges(boxes, flat, nudges);
    massingGroundRaises(grid.data(), n, n, spacing, origin[0], origin[1], boxes, flat, nudges, raises);
    for (std::size_t i = 0; i < boxes.size(); ++i) boxes[i].z1 += nudges[i] + raises[i];

    std::vector<bool> keep(boxes.size(), false);
    if (!kitPath.empty()) {
        std::string kitText;
        if (!readFile(kitPath, kitText)) return die("can't read " + kitPath);
        std::unordered_set<std::string> ids;
        kitIds(kitText, ids);
        for (std::size_t i = 0; i < boxes.size(); ++i) keep[i] = ids.count(boxes[i].id) > 0;
    }

    FloatSpec spec;
    FloatReport before;
    if (!findFloats(ground, boxes, visible, spec, before, &err)) return die(err);
    std::string out = "ftcore_floatcheck: " + boxesPath + "\n" + std::to_string(tableBoxes) + " table boxes" +
                      (hero ? " + " + std::to_string(boxes.size() - tableBoxes) + " hero bays" : std::string()) + "\n";
    out += "roads hide " + std::to_string(roadHides) + " rows\n";
    out += "before: " + std::to_string(before.checked) + " checked, " + std::to_string(before.floating.size()) + " float over " +
           num(spec.visible, 2) + " m, " + std::to_string(before.failing) + " fail, " + std::to_string(before.crouch) + " crouch\n";

    spec.maxEave = eave;
    FoundationPlan plan;
    FoundationStats st;
    if (fix && !planFoundations(ground, boxes, visible, &keep, spec, plan, st, &err)) return die(err);
    out += "fix pass (" + std::to_string(plan.passes) + " passes, eave " + num(eave, 1) + " m): " + std::to_string(st.floating) +
           " floats; " + std::to_string(st.skirts) + " skirts, " + std::to_string(st.podiums) + " podiums, " + std::to_string(st.lowered) +
           " lowered, " + std::to_string(st.underBoxes) + " under-boxes, " + std::to_string(st.snapped) + " snapped, " +
           std::to_string(st.dropped) + " dropped, " + std::to_string(st.piersRaised) + " piers raised, " + std::to_string(st.endPosts) +
           " bridges end-posted, " + std::to_string(st.left) + " left; " + std::to_string(plan.added.size()) + " boxes added\n";
    std::vector<MassingBox> all;
    std::vector<bool> inc;
    applyFoundations(boxes, visible, plan, all, inc);

    if (plinths) {
        PlinthSpec ps;
        if (!exemptText.empty() && !parsePocketRects(exemptText, ps.pocket.exempt)) return die("--exempt-rect is X0,Y0,X1,Y1[;...]");
        double xy[2] = {0.0, 0.0};
        if (!parseList(anchorText, xy, 2)) return die("--plinths 1 needs --anchor X,Y");
        const PocketRect rect = pocketRectOf(ground, all, inc, 10.0);
        std::vector<MassingBox> made;
        PlinthStats pst;
        if (!planPlinths(ground, all, inc, rect, {PocketPoint{xy[0], xy[1]}}, ps, made, pst, &err)) return die(err);
        out += "plinths: " + std::to_string(pst.skirts) + " skirt boxes, " + std::to_string(pst.fills) + " fill boxes, " +
               std::to_string(pst.stairs) + " stair steps; " + std::to_string(pst.pocketsAfter) + " pockets after\n";
        for (MassingBox& b : made) {
            all.push_back(b);
            inc.push_back(true);
        }
    }

    FloatSpec afterSpec = spec;
    afterSpec.spanRule = true;
    afterSpec.chainCheck = true;
    FloatReport after;
    if (!findFloats(ground, all, inc, afterSpec, after, &err)) return die(err);
    FloatOutcomes o;
    classifyFloats(before, plan, after, o);
    out += "after (span rule, chain check, " + std::to_string(after.passes) + " passes): " + std::to_string(o.rows.size()) + " = " +
           std::to_string(o.fixed) + " fixed + " + std::to_string(o.spanned) + " spanned + " + std::to_string(o.failing) +
           " failing; " + std::to_string(o.newFloats) + " new floats\n";
    out += "FIXED: " + idList(o, all, FloatOutcome::Fixed, false) + "\n";
    out += "SPANNED (span/overhang m): " + idList(o, all, FloatOutcome::Spanned, true) + "\n";
    out += "FAILING: " + idList(o, all, FloatOutcome::Failing, false) + "\n";
    std::vector<bool> wasFloat(all.size(), false);
    for (const FloatBox& f : before.floating) wasFloat[f.box] = true;
    for (const FloatBox& f : after.floating)
        if (!wasFloat[f.box]) out += "  new float " + all[f.box].id + " (" + all[f.box].kind + "): " + num(f.gap, 2) + " m\n";

    if (!outPath.empty()) {
        std::vector<const FloatBox*> afterOf(all.size(), nullptr);
        for (const FloatBox& f : after.floating) afterOf[f.box] = &f;
        for (const FloatBox& f : after.spanned) afterOf[f.box] = &f;
        std::vector<const FloatBox*> beforeOf(all.size(), nullptr);
        for (const FloatBox& f : before.floating) beforeOf[f.box] = &f;
        std::string csv = "id,kind,before_gap,before_failing,before_crouch,fix,outcome,after_gap,span_m,overhang_m,supporters,lip_m,x,y\n";
        for (const FloatOutcomeRow& r : o.rows) {
            const MassingBox& b = all[r.box];
            const FloatBox* a = afterOf[r.box];
            const FloatBox* bf = beforeOf[r.box];
            csv += b.id + "," + b.kind + "," + num(r.before, 3) + "," + (bf && bf->failing ? "1" : "0") + "," + (bf && bf->crouch ? "1" : "0") + "," +
                   floatFixName(r.fix) + "," + outcomeName(r.outcome) + "," + num(r.after, 3) + "," + num(r.span, 2) + "," + num(r.overhang, 2) + "," +
                   (a ? std::to_string(a->supporters) : std::string()) + "," + (a ? num(a->top - a->ground, 3) : std::string()) + "," +
                   num(a ? a->x : (bf ? bf->x : 0.0), 2) + "," + num(a ? a->y : (bf ? bf->y : 0.0), 2) + "\n";
        }
        if (!writeFile(outPath, csv)) return die("can't write " + outPath);
    }
    const bool ok = o.failing == 0 && o.newFloats == 0;
    out += ok ? "FLOATCHECK OK\n" : "FLOATCHECK FAIL\n";
    std::fputs(out.c_str(), stdout);
    if (!reportPath.empty() && !writeFile(reportPath, out)) return die("can't write " + reportPath);
    return ok ? 0 : 1;
}
