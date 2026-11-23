// ftcore_lunge: an enemy move's lunge distance against time (arenaLungeDistance), frame by frame, from arena.json.
// The clip build reads it so a root's forward motion is Core's own by construction (a skinned enemy's attack clip, such
// as the Maw-Pup's pounce: po_root(f) is the distance at f / fps).
//
//   ftcore_lunge ARENA_JSON ENEMY_ID MOVE_CLIP [--fps 30] [--out CSV]
//
// Prints (or writes) "frame,t,phase,speed,distance": every frame from 0 through the end of the move (startup + active
// + recovery), phase one of startup, active, recovery; speed the forward m/s over the frame that ends there (0 at
// frame 0). Then a line "# lunge L end E knots N" (N 0: no curve, the even spread). Exit 0, 2 on an error.

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "ftcore/ArenaFight.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

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

int die(const std::string& what) {
    std::fprintf(stderr, "ftcore_lunge: error: %s\n", what.c_str());
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) return die("usage: ftcore_lunge ARENA_JSON ENEMY_ID MOVE_CLIP [--fps 30] [--out CSV]");
    int fps = 30;
    std::string outPath;
    for (int i = 4; i + 1 < argc; i += 2) {
        const std::string_view key = argv[i];
        if (key == "--fps") fps = std::atoi(argv[i + 1]);
        else if (key == "--out") outPath = argv[i + 1];
        else return die("unknown option " + std::string(key));
    }
    if (fps < 1 || fps > 1000) return die("--fps expects 1..1000");
    std::string text;
    if (!readFile(argv[1], text)) return die(std::string("cannot read ") + argv[1]);
    ArenaTable table;
    std::string err;
    if (!importArenaTable(text, table, &err)) return die("arena table: " + err);
    const ArenaEnemyKind* kind = nullptr;
    for (std::size_t e = 0; e < table.enemyIds.size(); ++e)
        if (table.enemyIds[e] == argv[2]) kind = &table.enemies[e];
    if (!kind) return die(std::string("no enemy ") + argv[2]);
    const ArenaEnemyMove* move = nullptr;
    for (std::size_t m = 0; m < kind->moveCount; ++m)
        if (kind->moves[m].clip() == argv[3]) move = &kind->moves[m];
    if (!move) return die(std::string("no move with clip ") + argv[3]);
    const ArenaAttack& a = move->attack;
    const ArenaLungeKnot* pool = kind->lungeKnots.data();

    std::string csv = "frame,t,phase,speed,distance\n";
    const int last = static_cast<int>(std::ceil(a.total() * static_cast<float>(fps) - 1e-4f));
    float before = 0.f;
    char line[160];
    for (int f = 0; f <= last; ++f) {
        const float t = static_cast<float>(f) / static_cast<float>(fps);
        const float d = arenaLungeDistance(a, pool, t);
        const char* phase = t < a.startup ? "startup" : t < a.startup + a.active ? "active" : "recovery";
        const float speed = f == 0 ? 0.f : (d - before) * static_cast<float>(fps);
        std::snprintf(line, sizeof line, "%d,%.6f,%s,%.6f,%.6f\n", f, static_cast<double>(t), phase,
                      static_cast<double>(speed), static_cast<double>(d));
        csv += line;
        before = d;
    }
    std::snprintf(line, sizeof line, "# lunge %.6f end %.6f knots %u\n", static_cast<double>(a.lunge),
                  static_cast<double>(arenaLungeEnd(a, pool)), static_cast<unsigned>(a.lungeKnots));
    csv += line;
    if (outPath.empty()) {
        std::fputs(csv.c_str(), stdout);
        return 0;
    }
    std::FILE* f = std::fopen(outPath.c_str(), "wb");
    if (!f) return die("cannot write " + outPath);
    std::fwrite(csv.data(), 1, csv.size(), f);
    std::fclose(f);
    return 0;
}
