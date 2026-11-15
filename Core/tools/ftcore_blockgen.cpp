// ftcore_blockgen: generates a slice of the massing tables from a block plan (ftcore/BlockGen.h) and checks the result
// (ftcore/BlockCheck.h). An offline build step on the private tables; nothing here holds their data.
//
//   ftcore_blockgen --plan P --boxes B --streets S --prop-streets PS --doors D --frontage F --r16 H --samples N
//                   --origin X,Y --spacing M --rect X0,Y0,X1,Y1 --out-dir O --report R [--tag T] [--check-only 1]
//                   [--routes 1 [--towers TW] [--sea X] [--roads RD]] [--frame OX,OY,DEG]
//
// Writes into O: sol_strip_boxes_<T>.csv (B less the slice's swept rows plus the generated rows), sol_strip_streets_<T>.csv
// and sol_strip_prop_streets_<T>.csv (the plan's streets and the mid-block alleys in place of the slice's), sol_strip_doors_<T>.csv,
// sol_strip_frontage_<T>.csv (F as is), sol_<T>_trees.csv, sol_<T>_houses.csv, sol_<T>_towers.csv and sol_<T>_removed.txt.
// Then checks the new box table and writes the check's report to R. --check-only 1 checks B with the streets S as they
// are. Exit 0 on BLOCKCHECK OK, 1 on a failed check, 2 on an error.
// --routes 1 adds the route rules (main streets, lanes, walk, sight): sight targets are the landmark (at three quarters
// of its height; from the generated towers, or TW's landmark rows with --check-only) and, with --sea, the water due east
// of x X; the glass towers block sight. RD: more streets (a road table) for the route rules only. With an out dir it also
// writes sol_<T>_street_flags.csv and sol_<T>_nameplates.csv, and when generating sol_<T>_districts.csv.
// --frame OX,OY,DEG: the plan and --rect are in a local frame turned DEG counter-clockwise about the world point OX,OY
// (a ward on a turned street grid). Every input table stays in world terms; the tool turns them into the frame for the
// generator and the check, and turns what it writes back (boxes, houses and towers get rot DEG). The report's points
// are local. --sea can't be used with a frame. TW's rows join the sight targets when generating too.

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "ftcore/BlockCheck.h"
#include "ftcore/BlockGen.h"
#include "ftcore/MassingDress.h"
#include "ftcore/MassingPlan.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>
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
    std::fprintf(stderr, "ftcore_blockgen: error: %s\n", what.c_str());
    return 2;
}

std::string num(double v, int places = 3) {
    char b[48];
    std::snprintf(b, sizeof b, "%.*f", places, v);
    return b;
}

// The text's lines without their line ends.
std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t p = 0;
    while (p < text.size()) {
        std::size_t q = text.find('\n', p);
        if (q == std::string::npos) q = text.size();
        std::string l = text.substr(p, q - p);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(l);
        p = q + 1;
    }
    return out;
}

bool isCommentLine(std::string_view l) {
    while (!l.empty() && (l.front() == ' ' || l.front() == '\t' || l.front() == '"')) l.remove_prefix(1);
    return !l.empty() && l.front() == '#';
}

bool blank(std::string_view l) {
    for (const char c : l)
        if (c != ' ' && c != '\t' && c != ',') return false;
    return true;
}

std::string firstField(const std::string& l) { return l.substr(0, l.find(',')); }

std::vector<std::string> splitPlain(const std::string& l) {
    std::vector<std::string> f;
    std::size_t p = 0;
    for (;;) {
        const std::size_t q = l.find(',', p);
        f.push_back(l.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return f;
}

bool parsePair(const std::string& s, double out[], int n) {
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

bool loadGround(const std::string& path, double samples, const double origin[2], double spacing, std::vector<std::uint16_t>& grid,
                MassingGround& ground, std::string& err) {
    if (path.empty()) return true;
    std::string text;
    if (!readFile(path, text)) {
        err = "can't read " + path;
        return false;
    }
    const std::size_t n = static_cast<std::size_t>(samples);
    if (n < 2 || text.size() != n * n * 2) {
        err = "the r16 is not samples x samples u16";
        return false;
    }
    grid.resize(n * n);
    for (std::size_t k = 0; k < n * n; ++k)
        grid[k] = static_cast<std::uint16_t>(static_cast<std::uint8_t>(text[2 * k]) | (static_cast<std::uint8_t>(text[2 * k + 1]) << 8));
    ground = MassingGround{grid.data(), n, n, spacing, origin[0], origin[1]};
    return true;
}

std::string streetLine(const BlockPlanStreet& s, double pavement) {
    return s.id + "," + s.kind + "," + num(s.width, 1) + "," + num(s.x0, 1) + ":" + num(s.y0, 1) + ";" + num(s.x1, 1) + ":" +
           num(s.y1, 1) + "," + (pavement > 0.0 ? num(pavement, 2) : std::string(s.pavement > 0.0 ? num(s.pavement, 2) : "0"));
}

std::string boxLine(const std::vector<std::string>& head, const BlockBox& b, double rot) {
    std::string l;
    for (std::size_t i = 0; i < head.size(); ++i) {
        const std::string& h = head[i];
        std::string v;
        if (h == "id") v = b.id;
        else if (h == "row") v = b.row;
        else if (h == "src") v = "INV";
        else if (h == "kind") v = b.kind;
        else if (h == "cx") v = num(b.cx, 4);
        else if (h == "cy") v = num(b.cy, 4);
        else if (h == "sx") v = num(b.sx, 4);
        else if (h == "sy") v = num(b.sy, 4);
        else if (h == "rot") v = num(rot, 4);
        else if (h == "z0") v = num(b.z0, 4);
        else if (h == "z1") v = num(b.z1, 4);
        else if (h == "ground") v = num(b.ground, 4);
        else if (h == "terr_min") v = num(b.terrMin, 4);
        else if (h == "terr_max") v = num(b.terrMax, 4);
        else if (h == "sink_m") v = num(b.sink, 2);
        else if (h == "cover") v = "False";
        else if (h == "note") v = b.note;
        if (i) l += ',';
        l += v;
    }
    return l;
}

MassingStreet toStreet(const BlockPlanStreet& s) {
    MassingStreet m;
    m.id = s.id;
    m.kind = s.kind;
    m.width = s.width;
    m.pavement = s.pavement;
    m.points = {MassingVec{s.x0, s.y0, 0.0}, MassingVec{s.x1, s.y1, 0.0}};
    return m;
}

} // namespace

int main(int argc, char** argv) {
    std::string planPath, boxesPath, streetsPath, propPath, doorsPath, frontPath, r16Path, outDir, reportPath, tag = "r9";
    double samples = 0.0, origin[2] = {0.0, 0.0}, spacing = 1.0, rectV[4] = {0, 0, 0, 0};
    bool checkOnly = false, haveRect = false, routes = false, sea = false;
    std::string towersPath, roadsPath;
    double seaX = 0.0, frameV[3] = {0.0, 0.0, 0.0};
    bool haveFrame = false;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--plan") planPath = v;
        else if (k == "--boxes") boxesPath = v;
        else if (k == "--streets") streetsPath = v;
        else if (k == "--prop-streets") propPath = v;
        else if (k == "--doors") doorsPath = v;
        else if (k == "--frontage") frontPath = v;
        else if (k == "--r16") r16Path = v;
        else if (k == "--samples") samples = std::atof(v.c_str());
        else if (k == "--origin") {
            if (!parsePair(v, origin, 2)) return die("--origin is X,Y");
        } else if (k == "--spacing") spacing = std::atof(v.c_str());
        else if (k == "--rect") {
            if (!parsePair(v, rectV, 4)) return die("--rect is X0,Y0,X1,Y1");
            haveRect = true;
        } else if (k == "--out-dir") outDir = v;
        else if (k == "--report") reportPath = v;
        else if (k == "--tag") tag = v;
        else if (k == "--check-only") checkOnly = v == "1";
        else if (k == "--routes") routes = v == "1";
        else if (k == "--towers") towersPath = v;
        else if (k == "--roads") roadsPath = v;
        else if (k == "--sea") sea = true, seaX = std::atof(v.c_str());
        else if (k == "--frame") {
            if (!parsePair(v, frameV, 3)) return die("--frame is OX,OY,DEG");
            haveFrame = true;
        }
        else return die("unknown switch " + k);
    }
    if (planPath.empty() || boxesPath.empty() || streetsPath.empty() || !haveRect || reportPath.empty())
        return die("needs --plan, --boxes, --streets, --rect and --report");
    if (!checkOnly && (outDir.empty() || propPath.empty() || doorsPath.empty() || frontPath.empty()))
        return die("generating needs --out-dir, --prop-streets, --doors and --frontage");
    if (haveFrame && sea) return die("--sea can't be used with --frame");
    const BlockRect rect{rectV[0], rectV[1], rectV[2], rectV[3]};
    const double frameDeg = haveFrame ? frameV[2] : 0.0;
    const std::string rotCol = haveFrame ? num(frameDeg, 4) : std::string("0"); // houses and towers

    std::string err, text;
    if (!readFile(planPath, text)) return die("can't read " + planPath);
    BlockPlan plan;
    if (!importBlockPlan(text, plan, &err)) return die(planPath + ": " + err);

    std::string boxesText;
    if (!readFile(boxesPath, boxesText)) return die("can't read " + boxesPath);
    std::vector<MassingBox> base;
    if (!importMassingBoxes(boxesText, base, &err)) return die(boxesPath + ": " + err);

    std::vector<std::uint16_t> grid;
    BlockGround ground;
    if (!loadGround(r16Path, samples, origin, spacing, grid, ground.field, err)) return die(err);
    {
        std::vector<MassingBox> zones;
        for (const MassingBox& b : base)
            if (b.source == MassingSource::Zone) zones.push_back(b);
        ground.setZones(zones);
    }
    if (haveFrame) ground.setFrame(frameV[0], frameV[1], frameV[2]);
    // world to the plan's frame and back (identity without --frame)
    auto pointIn = [&](double& x, double& y) {
        double lx = 0.0, ly = 0.0;
        ground.toLocal(x, y, lx, ly);
        x = lx, y = ly;
    };
    auto pointOut = [&](double& x, double& y) {
        double wx = 0.0, wy = 0.0;
        ground.toWorld(x, y, wx, wy);
        x = wx, y = wy;
    };
    auto boxIn = [&](MassingBox& b) {
        pointIn(b.cx, b.cy);
        b.rot -= frameDeg;
    };
    auto streetsIn = [&](std::vector<MassingStreet>& v) {
        for (MassingStreet& s : v)
            for (MassingVec& q : s.points) pointIn(q.x, q.y);
    };
    for (MassingBox& b : base) boxIn(b); // for the sweep (the zones above stay in world terms for the ground)

    std::string streetsText;
    if (!readFile(streetsPath, streetsText)) return die("can't read " + streetsPath);

    std::vector<BlockRect> exempt;
    for (const BlockPlanFeature& f : plan.features)
        if (f.kind == BlockFeatureKind::Park || f.kind == BlockFeatureKind::Forecourt || f.kind == BlockFeatureKind::Plaza ||
            f.kind == BlockFeatureKind::Precinct || f.kind == BlockFeatureKind::Court)
            exempt.push_back(f.r);

    std::string finalBoxes = boxesText, finalStreets = streetsText;
    std::string summary;
    std::vector<BlockTower> towerList;
    std::vector<BlockDistrict> districtList;
    if (!checkOnly) {
        BlockGenResult gen;
        blockGenerate(plan, ground, gen);
        towerList = gen.towers;
        districtList = gen.districts;

        // streets: the base table less the plan's ids and drops, plus the plan's and the alleys
        std::set<std::string> replaced(plan.drops.begin(), plan.drops.end());
        for (const BlockPlanStreet& s : plan.streets)
            if (s.emit) replaced.insert(s.id); // emit 0: a street the plan reads but leaves in the table
        auto streetTable = [&](const std::string& in, bool prop) {
            std::string out;
            bool header = false;
            std::size_t dropped = 0;
            for (const std::string& l : lines(in)) {
                if (isCommentLine(l) || blank(l)) {
                    out += l + "\n";
                    continue;
                }
                if (!header) {
                    header = true;
                    out += "# " + tag + ": the slice's streets replaced by ftcore_blockgen (plan streets and mid-block alleys).\n";
                    out += l + "\n";
                    continue;
                }
                if (replaced.count(firstField(l))) {
                    ++dropped;
                    continue;
                }
                out += l + "\n";
            }
            for (BlockPlanStreet s : gen.streets) {
                pointOut(s.x0, s.y0);
                pointOut(s.x1, s.y1);
                const double pav = prop && s.width <= 6.0 ? std::max(0.0, (s.width - 2.5) * 0.5) : 0.0;
                out += streetLine(s, pav) + "\n";
            }
            (void)dropped;
            return out;
        };
        std::string propText;
        if (!readFile(propPath, propText)) return die("can't read " + propPath);
        finalStreets = streetTable(streetsText, false);
        const std::string finalProp = streetTable(propText, true);
        std::vector<MassingStreet> outStreets;
        if (!importMassingStreets(finalStreets, outStreets, &err)) return die("the new street table: " + err);
        streetsIn(outStreets);

        // boxes: the base rows the sweep keeps, verbatim, then the generated ones
        std::vector<std::string> head;
        std::string out;
        std::vector<std::string> removed;
        std::size_t k = 0;
        bool header = false;
        for (const std::string& l : lines(boxesText)) {
            if (isCommentLine(l) || blank(l)) {
                out += l + "\n";
                continue;
            }
            if (!header) {
                header = true;
                head = splitPlain(l);
                out += "# " + tag + ": the slice x " + num(rect.x0, 0) + ".." + num(rect.x1, 0) + ", y " + num(rect.y0, 0) + ".." +
                       num(rect.y1, 0) + (haveFrame ? " (local frame " + num(frameV[2], 1) + " deg about " + num(frameV[0], 1) + "," + num(frameV[1], 1) + ")" : std::string()) +
                       " swept and regenerated by ftcore_blockgen (rows noted R9; removed ids in sol_" + tag +
                       "_removed.txt).\n";
                out += l + "\n";
                continue;
            }
            if (k >= base.size()) return die("the box table's lines and rows disagree");
            const MassingBox& b = base[k++];
            if (firstField(l) != b.id) return die("the box table's lines and rows disagree at " + b.id);
            if (blockSweepKeep(b, rect, outStreets, gen.boxes)) out += l + "\n";
            else removed.push_back(b.id);
        }
        if (k != base.size()) return die("the box table's lines and rows disagree");
        std::size_t buildings = 0, walls = 0, landings = 0, gates = 0;
        for (const BlockBox& b : gen.boxes) {
            BlockBox w = b;
            pointOut(w.cx, w.cy);
            out += boxLine(head, w, frameDeg) + "\n";
            if (b.building) ++buildings;
            else if (b.kind == "wall") ++walls;
            else if (b.kind == "landing") ++landings;
            else if (b.kind == "gate") ++gates;
        }
        finalBoxes = out;

        // doors: the base rows less those on swept boxes, plus the generated
        std::set<std::string> gone(removed.begin(), removed.end());
        std::string doorsText, doorsOut;
        if (!readFile(doorsPath, doorsText)) return die("can't read " + doorsPath);
        std::size_t doorsDropped = 0;
        header = false;
        for (const std::string& l : lines(doorsText)) {
            if (isCommentLine(l) || blank(l)) {
                doorsOut += l + "\n";
                continue;
            }
            if (!header) {
                header = true;
                doorsOut += l + "\n";
                continue;
            }
            const std::string id = firstField(l);
            const std::size_t dash = id.find('-');
            if (dash != std::string::npos && gone.count(id.substr(dash + 1))) {
                ++doorsDropped;
                continue;
            }
            doorsOut += l + "\n";
        }
        for (BlockDoor d : gen.doors) {
            pointOut(d.ox, d.oy);
            pointOut(d.ix, d.iy);
            const BlockRect lr = d.room;
            double cx[4] = {lr.x0, lr.x0, lr.x1, lr.x1}, cy[4] = {lr.y0, lr.y1, lr.y1, lr.y0};
            for (int c = 0; c < 4; ++c) pointOut(cx[c], cy[c]);
            const std::string z = num(d.z, 2);
            doorsOut += "DOOR-" + d.boxId + ",door,1.6," + num(d.ox, 2) + ":" + num(d.oy, 2) + ":" + z + ";" + num(d.ix, 2) + ":" +
                        num(d.iy, 2) + ":" + z + ",\n";
            doorsOut += "ROOM-" + d.boxId + "," + d.roomKind + ",0," + num(cx[0], 2) + ":" + num(cy[0], 2) + ":" + z + ";" +
                        num(cx[1], 2) + ":" + num(cy[1], 2) + ":" + z + ";" + num(cx[2], 2) + ":" + num(cy[2], 2) + ":" + z + ";" +
                        num(cx[3], 2) + ":" + num(cy[3], 2) + ":" + z + ",\n";
        }

        std::string frontText;
        if (!readFile(frontPath, frontText)) return die("can't read " + frontPath);
        frontText = "# " + tag + ": the frontage rules unchanged (no new facade dressing on the regenerated slice).\n" + frontText;

        const std::string priv = "# PRIVATE. Never committed or uploaded. ftcore_blockgen " + tag + " output.\n";
        std::string trees = priv + "id,kind,x,y,z,height_m,canopy_m,host\n";
        for (BlockTree t : gen.trees) {
            pointOut(t.x, t.y);
            trees += t.id + "," + t.kind + "," + num(t.x, 2) + "," + num(t.y, 2) + "," + num(t.z, 2) + "," + num(t.height, 1) + "," +
                     num(t.canopy, 1) + "," + t.host + "\n";
        }
        std::string houses = priv + "id,box_id,cx,cy,sx,sy,rot,eave_z,pitch_deg,ridge,hidden\n";
        for (BlockHouse h : gen.houses) {
            pointOut(h.cx, h.cy);
            houses += h.id + "," + h.boxId + "," + num(h.cx, 3) + "," + num(h.cy, 3) + "," + num(h.sx, 3) + "," + num(h.sy, 3) + "," +
                      rotCol + "," + num(h.eave, 3) + "," + num(h.pitch, 1) + "," + std::string(1, h.ridge) + "," +
                      (h.hidden ? "1" : "0") + "\n";
        }
        std::string towers = priv + "id,kind,cx,cy,z0,sx,sy,rot,floors,height_m,note\n";
        for (BlockTower t : gen.towers) {
            pointOut(t.cx, t.cy);
            towers += t.id + "," + t.kind + "," + num(t.cx, 2) + "," + num(t.cy, 2) + "," + num(t.z0, 2) + "," + num(t.sx, 1) + "," +
                      num(t.sy, 1) + "," + rotCol + "," + std::to_string(t.floors) + "," + num(t.height, 1) + "," + t.note +
                      "\n";
        }
        std::string rem = "# PRIVATE. " + tag + " slice rows swept by ftcore_blockgen, one id a line.\n";
        for (const std::string& id : removed) rem += id + "\n";

        const std::string o = outDir + "/";
        if (!writeFile(o + "sol_strip_boxes_" + tag + ".csv", finalBoxes) || !writeFile(o + "sol_strip_streets_" + tag + ".csv", finalStreets) ||
            !writeFile(o + "sol_strip_prop_streets_" + tag + ".csv", finalProp) || !writeFile(o + "sol_strip_doors_" + tag + ".csv", doorsOut) ||
            !writeFile(o + "sol_strip_frontage_" + tag + ".csv", frontText) || !writeFile(o + "sol_" + tag + "_trees.csv", trees) ||
            !writeFile(o + "sol_" + tag + "_houses.csv", houses) || !writeFile(o + "sol_" + tag + "_towers.csv", towers) ||
            !writeFile(o + "sol_" + tag + "_removed.txt", rem))
            return die("can't write into " + outDir);

        const BlockGenStats& st = gen.stats;
        summary += "removed " + std::to_string(removed.size()) + " rows; added " + std::to_string(gen.boxes.size()) + " (buildings " +
                   std::to_string(buildings) + ", walls " + std::to_string(walls) + ", paving " + std::to_string(landings) + ", gate " +
                   std::to_string(gates) + ")\n";
        summary += "lots " + std::to_string(st.lots) + " (shop " + std::to_string(st.shopLots) + ", house " + std::to_string(st.houseLots) +
                   ", corner " + std::to_string(st.corners) + "), merged " + std::to_string(st.merged) + " (slabs " + std::to_string(st.slabs) +
                   ", walk-ups " + std::to_string(st.walkups) + "), yards " + std::to_string(st.yards) + ", enterable " +
                   std::to_string(st.enterable) + ", alleys " + std::to_string(st.alleys) + " (" + std::to_string(st.alleysCut) + " ends run on)\n";
        summary += "doors dropped " + std::to_string(doorsDropped) + ", added " + std::to_string(gen.doors.size()) + "; trees " +
                   std::to_string(gen.trees.size()) + ", houses " + std::to_string(gen.houses.size()) + " (hidden " +
                   std::to_string(st.hiddenHouses) + "), towers " + std::to_string(gen.towers.size()) + "\n";
        summary += "dressing: roof-garden trees " + std::to_string(st.roofTrees) + ", market stalls " + std::to_string(st.stalls) +
                   ", gate roofs and paths " + std::to_string(st.marks) + "\n";
        for (const std::string& n : st.notes) summary += "note: " + n + "\n";
    }

    // the check
    std::vector<MassingBox> checked;
    if (!importMassingBoxes(finalBoxes, checked, &err)) return die("the new box table: " + err);
    for (MassingBox& b : checked) boxIn(b);
    BlockCheckInput in;
    in.boxes = &checked;
    if (!importMassingStreets(finalStreets, in.streets, &err)) return die("the street table: " + err);
    streetsIn(in.streets);
    // read-only plan streets the table lacks (a loop road kept in another table)
    for (const BlockPlanStreet& s : plan.streets) {
        if (s.emit) continue;
        bool have = false;
        for (const MassingStreet& m : in.streets) have = have || m.id == s.id;
        if (!have) in.streets.push_back(toStreet(s));
    }
    in.exempt = exempt;
    in.rect = rect;
    in.ground = &ground;
    if (routes) {
        in.routes = true;
        if (!roadsPath.empty()) {
            std::string roadsText;
            std::vector<MassingStreet> roads;
            if (!readFile(roadsPath, roadsText)) return die("can't read " + roadsPath);
            if (!importMassingStreets(roadsText, roads, &err)) return die(roadsPath + ": " + err);
            streetsIn(roads);
            for (MassingStreet& r : roads) {
                bool have = false;
                for (const MassingStreet& m : in.streets) have = have || m.id == r.id;
                if (!have) in.streets.push_back(std::move(r));
            }
        }
        for (const BlockPlanFeature& f : plan.features)
            if (f.kind == BlockFeatureKind::Park || f.kind == BlockFeatureKind::Precinct || f.kind == BlockFeatureKind::Gate ||
                f.kind == BlockFeatureKind::Plaza || f.kind == BlockFeatureKind::Forecourt)
                in.reasons.push_back(f.r);
        if (!towersPath.empty()) {
            std::string tw;
            if (!readFile(towersPath, tw)) return die("can't read " + towersPath);
            std::vector<std::string> head;
            for (const std::string& l : lines(tw)) {
                if (isCommentLine(l) || blank(l)) continue;
                const std::vector<std::string> f = splitPlain(l);
                if (head.empty()) {
                    head = f;
                    continue;
                }
                auto col = [&](const char* name) -> std::string {
                    for (std::size_t i = 0; i < head.size(); ++i)
                        if (head[i] == name) return i < f.size() ? f[i] : std::string();
                    return {};
                };
                BlockTower tr;
                tr.id = col("id");
                tr.kind = col("kind");
                tr.cx = std::atof(col("cx").c_str());
                tr.cy = std::atof(col("cy").c_str());
                pointIn(tr.cx, tr.cy);
                tr.z0 = std::atof(col("z0").c_str());
                tr.sx = std::atof(col("sx").c_str());
                tr.sy = std::atof(col("sy").c_str());
                tr.height = std::atof(col("height_m").c_str());
                tr.rot = std::atof(col("rot").c_str()) - frameDeg;
                towerList.push_back(tr);
            }
        }
        for (const BlockTower& tr : towerList) {
            if (tr.kind == "landmark") {
                in.sights.push_back({tr.id, tr.cx, tr.cy, tr.z0 + 0.75 * tr.height});
            } else {
                MassingBox o;
                o.id = tr.id;
                o.kind = "slab";
                o.cx = tr.cx, o.cy = tr.cy, o.sx = tr.sx, o.sy = tr.sy, o.rot = tr.rot;
                o.z0 = tr.z0, o.z1 = tr.z0 + tr.height;
                in.occluders.push_back(o);
            }
        }
        in.sea = sea;
        in.seaX = seaX;
    }
    BlockCheckReport rep;
    const bool ok = blockCheck(in, rep);
    if (routes && !outDir.empty()) {
        std::vector<BlockStreetFlag> flags;
        std::vector<BlockPlate> plates;
        blockRouteTables(in.streets, checked, rect, ground, flags, plates);
        blockPlacePlates(plan.features, in.streets, ground, plates); // the named places too (park, plaza, precinct)
        const std::string priv = "# PRIVATE. Never committed or uploaded. ftcore_blockgen " + tag + " output.\n";
        std::string ft = priv + "id,kind,role,main,width_m,length_m,straight_m,tree_every_m,lamp_every_m,lamp_kind,signs_per_100m,awning,mouth\n";
        for (const BlockStreetFlag& f : flags)
            ft += f.id + "," + f.kind + "," + f.role + "," + (f.main ? "1" : "0") + "," + num(f.width, 1) + "," + num(f.length, 1) + "," +
                  num(f.straight, 1) + "," + num(f.treeEvery, 0) + "," + num(f.lampEvery, 0) + "," + f.lampKind + "," +
                  num(f.signsPer100, 0) + "," + (f.awning ? "1" : "0") + "," + f.mouth + "\n";
        std::string pt = priv + "kind,id,x,y,z,rot,street_a,street_b,mount,height_m\n";
        std::size_t wallPlates = 0;
        for (BlockPlate q : plates) {
            pointOut(q.x, q.y);
            q.rot += frameDeg;
            pt += std::string(kBlockPlateKind) + "," + q.id + "," + num(q.x, 2) + "," + num(q.y, 2) + "," + num(q.z, 2) + "," +
                  num(q.rot, 1) + "," + q.a + "," + q.b + "," + q.mount + ",3.0\n";
            if (q.mount == "wall") ++wallPlates;
        }
        const std::string o = outDir + "/";
        if (!writeFile(o + "sol_" + tag + "_street_flags.csv", ft) || !writeFile(o + "sol_" + tag + "_nameplates.csv", pt))
            return die("can't write into " + outDir);
        std::size_t mains = 0;
        for (const BlockStreetFlag& f : flags) mains += f.main ? 1 : 0;
        summary += "route tables: " + std::to_string(flags.size()) + " street flags (" + std::to_string(mains) + " main), " +
                   std::to_string(plates.size()) + " name plates (" + std::to_string(wallPlates) + " on walls)\n";
        if (!districtList.empty()) {
            std::string dt = priv + "id,colour,sign_style,paving,lots\n";
            for (const BlockDistrict& d : districtList)
                dt += d.id + "," + d.colour + "," + d.sign + "," + d.paving + "," + std::to_string(d.lots) + "\n";
            if (!writeFile(o + "sol_" + tag + "_districts.csv", dt)) return die("can't write into " + outDir);
            summary += "districts " + std::to_string(districtList.size()) + "\n";
        }
    }
    const std::string frameNote = haveFrame ? " (local frame: " + num(frameV[2], 1) + " deg about " + num(frameV[0], 1) + "," +
                                                  num(frameV[1], 1) + "; points below are local)"
                                            : std::string();
    const std::string report = "# ftcore_blockgen check, slice x " + num(rect.x0, 0) + ".." + num(rect.x1, 0) + ", y " + num(rect.y0, 0) +
                               ".." + num(rect.y1, 0) + frameNote + "\n" + summary + blockCheckText(rep);
    if (!writeFile(reportPath, report)) return die("can't write " + reportPath);
    std::fputs(report.c_str(), stdout);
    return ok ? 0 : 1;
}
