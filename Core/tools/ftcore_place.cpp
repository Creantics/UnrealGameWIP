// ftcore_place: runs the city building placer (ftcore/CityPlacer.h) on a job file and writes the placed
// buildings as CSV and a plain-text summary. An offline build tool; C stdio only.
//
//   ftcore_place <job.json> [--placed <csv>] [--summary <txt>] [--seed <n>]
//
// The job (paths are relative to the job file's folder):
//   {
//     "version": 1,
//     "placer": "city_placer.json",          classes, gaps, seed, variety thresholds
//     "height": "ground.u16",                u16 LE, h = (v - 32768) / 128, row-major from the south
//     "height_meta": "ground.json",          samples_x, samples_y, spacing_m, origin_of_sample_0_0_m
//     "max_roof": "roof.u16",                optional; same grid and coding, 65535 uncapped
//     "no_build": "mask.u8",                 optional; one byte of NoBuild bits a sample
//     "frontages": "frontages.csv",
//     "zones": "zones.csv",
//     "landmarks": "landmarks.csv",          optional; id,x,y
//     "variety": { "zones": ["a"], "bounds": [x0, y0, x1, y1], "enforce": true },
//                                            optional; default all zones, the grid, and the walk holds the
//                                            variety it checks (PlacerVarietyGuard); "enforce": false
//                                            only checks it
//     "placed_out": "placed.csv",            optional; --placed wins
//     "summary_out": "summary.txt"           optional; --summary wins
//   }
// The summary also goes to stdout without its disc lines, and the last line is PLACE OK. Any error
// prints "ftcore_place: error: ..." and exits 1.

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "ftcore/CityPlacer.h"
#include "ftcore/Json.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

int fail(const std::string& what) {
    std::fprintf(stderr, "ftcore_place: error: %s\n", what.c_str());
    return 1;
}

bool readFile(const std::string& path, std::string& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[1 << 16];
    for (;;) {
        const std::size_t n = std::fread(buf, 1, sizeof buf, f);
        if (n > 0) out.append(buf, n);
        if (n < sizeof buf) break;
    }
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok;
}

bool writeFile(const std::string& path, const std::string& text) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

bool isAbsolute(std::string_view p) {
    return (!p.empty() && (p[0] == '/' || p[0] == '\\')) || (p.size() >= 2 && p[1] == ':');
}

std::string folderOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

std::string resolve(const std::string& folder, std::string_view rel) {
    return isAbsolute(rel) ? std::string(rel) : folder + std::string(rel);
}

// A job member naming a file: false with a message when it is required and missing, or not a string.
bool jobPath(ftcore::JsonValue job, const char* key, bool required, const std::string& folder, std::string& out,
             std::string& error) {
    out.clear();
    const ftcore::JsonValue v = job[key];
    if (v.isAbsent() && !required) return true;
    if (!v.isString() || v.string().empty()) {
        error = std::string("job: ") + key + " must name a file";
        return false;
    }
    out = resolve(folder, v.string());
    return true;
}

bool readRaster(const std::string& path, const char* what, std::string& bytes, std::string& error) {
    if (readFile(path, bytes)) return true;
    error = std::string(what) + ": can't read " + path;
    return false;
}

const std::uint8_t* bytesOf(const std::string& s) { return reinterpret_cast<const std::uint8_t*>(s.data()); }

} // namespace

int main(int argc, char** argv) {
    std::string jobFile, placedOut, summaryOut;
    bool seedSet = false;
    unsigned long seedArg = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if ((a == "--placed" || a == "--summary" || a == "--seed") && i + 1 < argc) {
            const std::string v = argv[++i];
            if (a == "--placed") placedOut = v;
            else if (a == "--summary") summaryOut = v;
            else {
                char* end = nullptr;
                seedArg = std::strtoul(v.c_str(), &end, 10);
                if (v.empty() || *end != '\0' || seedArg > 0xfffffffful) return fail("--seed needs a whole number 0..4294967295");
                seedSet = true;
            }
        } else if (jobFile.empty() && !a.empty() && a[0] != '-') {
            jobFile = std::string(a);
        } else {
            std::fprintf(stderr, "usage: ftcore_place <job.json> [--placed <csv>] [--summary <txt>] [--seed <n>]\n");
            return 2;
        }
    }
    if (jobFile.empty()) {
        std::fprintf(stderr, "usage: ftcore_place <job.json> [--placed <csv>] [--summary <txt>] [--seed <n>]\n");
        return 2;
    }

    std::string text, error;
    if (!readFile(jobFile, text)) return fail("can't read the job " + jobFile);
    ftcore::JsonDocument jobDoc;
    if (!jobDoc.parse(text, &error)) return fail("job: " + error);
    const ftcore::JsonValue job = jobDoc.root();
    if (!job.isObject() || !job["version"].isInteger() || job["version"].integer() != 1)
        return fail("job: an object with version 1 is needed");
    for (std::uint32_t i = 0; i < job.size(); ++i) {
        static const char* const known[] = {"version", "placer", "height", "height_meta", "max_roof", "no_build", "frontages",
                                            "zones", "landmarks", "variety", "placed_out", "summary_out"};
        bool ok = false;
        for (const char* k : known) ok = ok || job.key(i) == k;
        if (!ok) return fail("job: unknown member \"" + std::string(job.key(i)) + "\"");
    }
    const std::string folder = folderOf(jobFile);
    std::string placerPath, heightPath, metaPath, roofPath, maskPath, frontagePath, zonePath, landmarkPath, jobPlaced, jobSummary;
    if (!jobPath(job, "placer", true, folder, placerPath, error) || !jobPath(job, "height", true, folder, heightPath, error) ||
        !jobPath(job, "height_meta", true, folder, metaPath, error) || !jobPath(job, "max_roof", false, folder, roofPath, error) ||
        !jobPath(job, "no_build", false, folder, maskPath, error) ||
        !jobPath(job, "frontages", true, folder, frontagePath, error) || !jobPath(job, "zones", true, folder, zonePath, error) ||
        !jobPath(job, "landmarks", false, folder, landmarkPath, error) ||
        !jobPath(job, "placed_out", false, folder, jobPlaced, error) ||
        !jobPath(job, "summary_out", false, folder, jobSummary, error))
        return fail(error);
    if (placedOut.empty()) placedOut = jobPlaced;
    if (summaryOut.empty()) summaryOut = jobSummary;

    ftcore::PlacerConfig config;
    if (!readFile(placerPath, text)) return fail("can't read the placer " + placerPath);
    if (!ftcore::importPlacerConfig(text, config, &error)) return fail("placer: " + error);
    if (seedSet) config.seed = static_cast<std::uint32_t>(seedArg);

    ftcore::PlacerGrid grid;
    if (!readFile(metaPath, text)) return fail("can't read the height meta " + metaPath);
    if (!ftcore::importPlacerGridMeta(text, grid, &error)) return fail("height meta: " + error);
    std::string raster;
    if (!readRaster(heightPath, "height", raster, error)) return fail(error);
    if (!ftcore::placerDecodeHeights(bytesOf(raster), raster.size(), grid, &error)) return fail(error);
    if (!roofPath.empty()) {
        if (!readRaster(roofPath, "max roof", raster, error)) return fail(error);
        if (!ftcore::placerDecodeMaxRoof(bytesOf(raster), raster.size(), grid, &error)) return fail(error);
    }
    if (!maskPath.empty()) {
        if (!readRaster(maskPath, "no-build", raster, error)) return fail(error);
        if (!ftcore::placerDecodeNoBuild(bytesOf(raster), raster.size(), grid, &error)) return fail(error);
    }
    raster.clear();
    raster.shrink_to_fit();

    std::vector<ftcore::PlacerZone> zones;
    if (!readFile(zonePath, text)) return fail("can't read the zones " + zonePath);
    if (!ftcore::importPlacerZones(text, config, zones, &error)) return fail("zones: " + error);
    std::vector<ftcore::PlacerFrontage> frontages;
    if (!readFile(frontagePath, text)) return fail("can't read the frontages " + frontagePath);
    if (!ftcore::importPlacerFrontages(text, frontages, &error)) return fail("frontages: " + error);
    std::vector<ftcore::PlacerPoint> landmarks;
    if (!landmarkPath.empty()) {
        if (!readFile(landmarkPath, text)) return fail("can't read the landmarks " + landmarkPath);
        if (!ftcore::importPlacerPoints(text, landmarks, &error)) return fail("landmarks: " + error);
    }

    // The variety check's zones and bounds.
    std::vector<bool> varietyZone(zones.size(), true);
    bool enforce = true;
    double bx0 = grid.x0, by0 = grid.y0;
    double bx1 = grid.x0 + static_cast<double>(grid.nx - 1) * grid.cell;
    double by1 = grid.y0 + static_cast<double>(grid.ny - 1) * grid.cell;
    const ftcore::JsonValue variety = job["variety"];
    if (!variety.isAbsent()) {
        if (!variety.isObject()) return fail("job: variety must be an object");
        for (std::uint32_t i = 0; i < variety.size(); ++i)
            if (variety.key(i) != "zones" && variety.key(i) != "bounds" && variety.key(i) != "enforce")
                return fail("job: variety: unknown member \"" + std::string(variety.key(i)) + "\"");
        const ftcore::JsonValue vz = variety["zones"];
        if (!vz.isAbsent()) {
            if (!vz.isArray()) return fail("job: variety.zones must be an array of zone ids");
            varietyZone.assign(zones.size(), false);
            for (std::uint32_t i = 0; i < vz.size(); ++i) {
                std::size_t z = 0;
                while (z < zones.size() && zones[z].id != vz[i].string()) ++z;
                if (z == zones.size()) return fail("job: variety.zones: no zone " + std::string(vz[i].string()));
                varietyZone[z] = true;
            }
        }
        const ftcore::JsonValue vb = variety["bounds"];
        if (!vb.isAbsent()) {
            if (!vb.isArray() || vb.size() != 4 || !vb[0].isNumber() || !vb[1].isNumber() || !vb[2].isNumber() || !vb[3].isNumber())
                return fail("job: variety.bounds must be [x0, y0, x1, y1]");
            bx0 = vb[0].number();
            by0 = vb[1].number();
            bx1 = vb[2].number();
            by1 = vb[3].number();
        }
        const ftcore::JsonValue ve = variety["enforce"];
        if (!ve.isAbsent()) {
            if (!ve.isBool()) return fail("job: variety.enforce must be true or false");
            enforce = ve.boolean();
        }
    }

    // The discs' centres, for the guard and the check alike.
    std::vector<ftcore::PlacerPoint> centres;
    ftcore::placerFlatPoints(grid, bx0, by0, bx1, by1, config.variety.spacing, config.variety.maxSlopeDeg, centres);
    ftcore::PlacerVarietyGuard guard;
    if (enforce) {
        guard.centres = centres;
        guard.zones = varietyZone;
    }

    ftcore::PlacerResult result;
    if (!ftcore::placeCity(config, grid, zones, frontages, result, &error, enforce ? &guard : nullptr)) return fail(error);

    std::vector<ftcore::PlacedBuilding> judged;
    judged.reserve(result.placed.size());
    for (const ftcore::PlacedBuilding& b : result.placed)
        if (varietyZone[b.zone]) judged.push_back(b);
    ftcore::VarietyResult varietyResult;
    ftcore::checkVariety(judged, centres, config.variety, varietyResult);
    ftcore::LandmarkReach reach;
    ftcore::checkLandmarkReach(centres, landmarks, config.variety.landmarkReach, reach);

    char buf[512];
    std::string summary = "# ftcore_place summary, format 1\n";
    std::snprintf(buf, sizeof buf, "grid nx=%zu ny=%zu cell=%.3f x0=%.3f y0=%.3f roof_caps=%d no_build=%d\n", grid.nx, grid.ny,
                  grid.cell, grid.x0, grid.y0, grid.maxRoof.empty() ? 0 : 1, grid.noBuild.empty() ? 0 : 1);
    summary += buf;
    std::snprintf(buf, sizeof buf, "inputs zones=%zu frontages=%zu classes=%zu variants=%zu landmarks=%zu seed=%u\n", zones.size(),
                  frontages.size(), config.classes.size(), config.variantCount(), landmarks.size(),
                  static_cast<unsigned>(config.seed));
    summary += buf;
    std::snprintf(buf, sizeof buf, "placed %zu\n", result.placed.size());
    summary += buf;
    summary += ftcore::placerZoneLines(zones, result);
    std::snprintf(buf, sizeof buf, "variety_bounds x0=%.1f y0=%.1f x1=%.1f y1=%.1f judged_buildings=%zu enforce=%d\n", bx0, by0,
                  bx1, by1, judged.size(), enforce ? 1 : 0);
    summary += buf;
    summary += ftcore::placerVarietyLines(config, varietyResult);
    const char* reachResult = landmarks.empty() ? "SKIPPED" : (reach.pass ? "PASS" : "FAIL");
    std::snprintf(buf, sizeof buf, "landmarks points=%u landmarks=%zu reach_m=%.1f uncovered=%u worst_m=%.1f worst_x=%.1f worst_y=%.1f result=%s\n",
                  reach.points, landmarks.size(), config.variety.landmarkReach, reach.uncovered,
                  landmarks.empty() ? 0.0 : reach.worst, reach.worstAt.x, reach.worstAt.y, reachResult);
    summary += buf;

    if (!placedOut.empty() && !writeFile(placedOut, ftcore::placerCsv(config, zones, frontages, result)))
        return fail("can't write " + placedOut);
    if (!summaryOut.empty() && !writeFile(summaryOut, summary + "PLACE OK\n")) return fail("can't write " + summaryOut);

    // stdout: the summary without its disc lines.
    std::size_t at = 0;
    while (at < summary.size()) {
        std::size_t end = summary.find('\n', at);
        if (end == std::string::npos) end = summary.size();
        const std::string_view line(summary.data() + at, end - at);
        if (line.substr(0, 5) != "disc ") std::printf("%.*s\n", static_cast<int>(line.size()), line.data());
        at = end + 1;
    }
    std::printf("PLACE OK\n");
    return 0;
}
