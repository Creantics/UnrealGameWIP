#include "ftcore/Vfx.h"

#include "ftcore/StoneBearer.h"

#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr float kPi = 3.14159265358979323846f;

bool loadEffect(const std::string& name, VfxEffect& out, std::vector<std::string>* warnings = nullptr) {
    std::string text, err;
    if (!ftcore_test::readProjectData("Vfx/" + name + ".json", text)) {
        FAIL("cannot read Data/Vfx/" << name << ".json");
        return false;
    }
    const bool ok = importVfxEffect(text, out, &err, warnings);
    CHECK_MESSAGE(ok, name << ": " << err);
    return ok;
}

// The single error line of a failed import, or "" when it succeeded.
std::string importError(const char* json) {
    VfxEffect e;
    std::string err;
    if (importVfxEffect(json, e, &err)) return "";
    CHECK(e.emitters.empty());
    return err;
}

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

bool anyGap(const VfxNiagaraEmitter& e, const char* part) {
    for (const std::string& g : e.gaps)
        if (contains(g, part)) return true;
    return false;
}

} // namespace

TEST_CASE("vfx: every Data/Vfx effect loads without warnings") {
    struct Expect {
        const char* file;
        std::size_t emitters;
    };
    const Expect files[] = {{"ember_swirl", 1}, {"falling_leaves", 1}, {"fireworks", 3}, {"flipbook_fire", 1},
                            {"heal", 2},        {"heal_ring", 1},      {"hit_spark", 2}, {"magic_ribbon", 2},
                            {"perfect_burst", 2}, {"smoke", 1},        {"speed_sparks", 1}, {"stretched_sparks", 2}, {"dust_puff", 2},
                            {"dealer_deal_tell", 3}, {"dealer_seam_open", 3}, {"dealer_seam_crit", 3}, {"ink_hit", 3}};
    for (const Expect& f : files) {
        CAPTURE(f.file);
        VfxEffect e;
        std::vector<std::string> warnings;
        REQUIRE(loadEffect(f.file, e, &warnings));
        CHECK(e.name == f.file);
        CHECK(e.emitters.size() == f.emitters);
        CHECK_MESSAGE(warnings.empty(), (warnings.empty() ? std::string() : warnings.front()));
        VfxNiagaraPlan plan;
        planVfxNiagara(e, plan);
        CHECK(plan.emitters.size() == f.emitters);
        for (const VfxNiagaraEmitter& p : plan.emitters) {
            CHECK(p.lifetime.lo > 0.f);
            CHECK(p.boundsMin.x < p.boundsMax.x);
            CHECK(p.boundsMin.z < p.boundsMax.z);
            CHECK_FALSE(p.startColor.empty());
        }
    }
}

TEST_CASE("vfx: the Kite-Mother's effects load without warnings and keep their particle budgets") {
    struct Expect {
        const char* file;
        std::size_t emitters;
        std::uint32_t budget; // the sum of the emitters' maxParticles
        bool loops;
    };
    const Expect files[] = {{"kite_swoop_tell", 3, 230, true},
                            {"kite_seam_open", 3, 54, true},
                            {"kite_seam_crit", 3, 82, false},
                            {"kite_hit", 2, 28, false}};
    for (const Expect& f : files) {
        CAPTURE(f.file);
        VfxEffect e;
        std::vector<std::string> warnings;
        REQUIRE(loadEffect(f.file, e, &warnings));
        CHECK(e.name == f.file);
        CHECK(e.emitters.size() == f.emitters);
        CHECK_MESSAGE(warnings.empty(), (warnings.empty() ? std::string() : warnings.front()));
        std::uint32_t total = 0;
        for (const VfxEmitter& em : e.emitters) {
            total += em.maxParticles;
            CHECK(em.loop == f.loops);
        }
        CHECK(total <= f.budget);
        VfxNiagaraPlan plan;
        planVfxNiagara(e, plan);
        CHECK(plan.emitters.size() == f.emitters);
    }
}

TEST_CASE("vfx: the Unbending's effects load without warnings and keep their particle budgets") {
    struct Expect {
        const char* file;
        std::size_t emitters;
        std::uint32_t budget; // the sum of the emitters' maxParticles
        bool loops;
    };
    // A looping effect may hold a one-shot emitter (weak_glow's opening burst); a one-shot effect has no looping one.
    const Expect files[] = {{"stone_tell", 3, 24, true},
                            {"stone_tell_area", 4, 120, true},
                            {"stone_impact", 5, 92, false},
                            {"stone_chips", 3, 42, false},
                            {"stone_chips_heavy", 4, 70, false},
                            {"crit_burst", 5, 90, false},
                            {"weak_glow", 3, 28, true}};
    for (const Expect& f : files) {
        CAPTURE(f.file);
        VfxEffect e;
        std::vector<std::string> warnings;
        REQUIRE(loadEffect(f.file, e, &warnings));
        CHECK(e.name == f.file);
        CHECK(e.emitters.size() == f.emitters);
        CHECK_MESSAGE(warnings.empty(), (warnings.empty() ? std::string() : warnings.front()));
        std::uint32_t total = 0;
        for (const VfxEmitter& em : e.emitters) {
            total += em.maxParticles;
            if (!f.loops) CHECK_FALSE(em.loop);
        }
        CHECK(total <= f.budget);
        VfxNiagaraPlan plan;
        planVfxNiagara(e, plan);
        CHECK(plan.emitters.size() == f.emitters);
        CHECK(plan.loops == f.loops);
        for (const VfxNiagaraEmitter& p : plan.emitters) {
            CHECK(p.lifetime.lo > 0.f);
            CHECK(p.boundsMin.x < p.boundsMax.x);
            CHECK(p.boundsMin.z < p.boundsMax.z);
            CHECK_FALSE(p.startColor.empty());
        }
    }
}

namespace {

// An emitter's screen cover: the particles it makes per cycle (the bursts' counts, or maxParticles when it emits by rate)
// times the square of the largest size they reach (startSize's top, or startSizeY's when larger, times sizeOverLife's
// peak, metres).
float coverOf(const VfxEmitter& e) {
    float n = 0.f;
    for (const VfxBurst& b : e.bursts) n += static_cast<float>(b.count);
    if (n == 0.f) n = static_cast<float>(e.maxParticles);
    float lo = 0.f, startHi = 0.f, growHi = 0.f, tallHi = 0.f;
    e.startSize.bounds(lo, startHi);
    if (e.hasStartSizeY) e.startSizeY.bounds(lo, tallHi);
    startHi = std::max(startHi, tallHi);
    e.sizeOverLife.bounds(lo, growHi);
    const float size = startHi * growHi;
    return n * size * size;
}

// Round 5's numbers by hand: n particles at `size` metres, growing to `grow` times that.
constexpr float cover(float n, float size, float grow = 1.f) { return n * size * grow * size * grow; }

} // namespace

TEST_CASE("vfx: the Unbending's chips and impact keep a screen-cover budget") {
    // The gameplay camera sits behind the lead, so what a hit throws off lands between the camera and the boss. Each
    // effect's cover is summed over its alpha-blended emitters, solid bits (chips, flakes, chunks) and clouds (puff and
    // dust sprites: dust, slam, plume, settle) apart so a cloud can't hide a chip count. Additive flashes don't count.
    // Round 5 (40 chips at up to 0.36 m, 16 flakes at 0.26 m; chips dust 18 at 1.1 m growing 1.7x; impact dust 32 at
    // 1.4 m growing 2.2x) came to 6.3 solid and 63 cloud for the chips, 460 cloud for the impact; round 7a 1.6, 25 and
    // 260; round 7c 0.41 solid and 5.0 cloud for a light blow's chips, 2.1 and 17 for a heavy blow's, 2.3 and 227 for
    // the impact.
    constexpr float kChipsSolidCap = 2.5f, kChipsCloudCap = 32.f, kImpactSolidCap = 5.f, kImpactCloudCap = 320.f;
    CHECK(cover(40, 0.36f) + cover(16, 0.26f) > kChipsSolidCap);
    CHECK(cover(18, 1.1f, 1.7f) > kChipsCloudCap);
    CHECK(cover(6, 2.2f, 1.25f) + cover(32, 1.4f, 2.2f) + cover(12, 1.6f, 1.9f) > kImpactCloudCap);

    struct Expect {
        const char* file;
        float solidCap, cloudCap;
    };
    // A heavy blow's chips (round 7) are their own effect, within the same caps but well apart from a light blow's.
    const Expect files[] = {{"stone_chips", kChipsSolidCap, kChipsCloudCap},
                            {"stone_chips_heavy", kChipsSolidCap, kChipsCloudCap},
                            {"stone_impact", kImpactSolidCap, kImpactCloudCap}};
    float solids[3] = {}, clouds[3] = {};
    for (std::size_t i = 0; i < 3; ++i) {
        const Expect& f = files[i];
        CAPTURE(f.file);
        VfxEffect e;
        REQUIRE(loadEffect(f.file, e));
        float solid = 0.f, cloud = 0.f;
        for (const VfxEmitter& em : e.emitters) {
            if (em.blend != VfxBlend::Alpha) continue;
            const bool isCloud = em.sprite == VfxSprite::Puff || em.sprite == VfxSprite::Dust;
            (isCloud ? cloud : solid) += coverOf(em);
        }
        CAPTURE(solid);
        CAPTURE(cloud);
        CHECK(solid <= f.solidCap);
        CHECK(cloud <= f.cloudCap);
        solids[i] = solid;
        clouds[i] = cloud;
    }
    CHECK(solids[1] >= 3.f * solids[0]);
    CHECK(clouds[1] >= 3.f * clouds[0]);
}

TEST_CASE("vfx: the Unbending's round-7 looks: a ring tell, angular chips soft against the scene, a short-lived glow") {
    // The tell is a flat ring on the ground (no depth fade: it lies 5 cm over the floor); the chips and the impact's
    // chunks are angular chip sprites and every alpha cloud fades against the scene; no glint disc on a hit; the crit's
    // flash stays under about 0.7 m; the glow's particles all die within 0.27 s, so it fades over 6 to 8 frames at shut.
    for (const char* file : {"stone_tell", "stone_tell_area"}) {
        CAPTURE(file);
        VfxEffect e;
        REQUIRE(loadEffect(file, e));
        bool ring = false;
        for (const VfxEmitter& em : e.emitters) {
            if (em.sprite == VfxSprite::Ring) {
                ring = true;
                CHECK(em.renderMode == VfxRenderMode::Horizontal);
                CHECK_FALSE(em.depthFade);
            }
        }
        CHECK(ring);
    }
    for (const char* file : {"stone_chips", "stone_chips_heavy", "stone_impact"}) {
        CAPTURE(file);
        VfxEffect e;
        REQUIRE(loadEffect(file, e));
        bool chip = false;
        for (const VfxEmitter& em : e.emitters) {
            CAPTURE(em.name);
            CHECK(em.blend == VfxBlend::Alpha);
            CHECK(em.sprite != VfxSprite::Soft);
            CHECK(em.depthFade);
            chip = chip || em.sprite == VfxSprite::Chip;
        }
        CHECK(chip);
    }
    {
        VfxEffect e;
        REQUIRE(loadEffect("crit_burst", e));
        for (const VfxEmitter& em : e.emitters) {
            if (em.name != "flash") continue;
            float lo = 0.f, startHi = 0.f, growHi = 0.f;
            em.startSize.bounds(lo, startHi);
            em.sizeOverLife.bounds(lo, growHi);
            CHECK(startHi * growHi <= 0.7f);
        }
    }
    {
        VfxEffect e;
        REQUIRE(loadEffect("weak_glow", e));
        for (const VfxEmitter& em : e.emitters) {
            CAPTURE(em.name);
            float lo = 0.f, hi = 0.f;
            em.lifetime.bounds(lo, hi);
            CHECK(hi <= 0.27f);
        }
    }
}

namespace {

float loOf(const VfxCurve& c) {
    float lo = 0.f, hi = 0.f;
    c.bounds(lo, hi);
    return lo;
}

float hiOf(const VfxCurve& c) {
    float lo = 0.f, hi = 0.f;
    c.bounds(lo, hi);
    return hi;
}

const VfxEmitter* emitterNamed(const VfxEffect& e, const char* name) {
    const int i = findVfxEmitter(e, name);
    return i < 0 ? nullptr : &e.emitters[static_cast<std::size_t>(i)];
}

// The highest alpha an emitter's colour over life reaches, and the age share where it first does.
float peakAlpha(const VfxEmitter& em, float* at = nullptr) {
    float best = 0.f, when = 0.f;
    for (const VfxColorStop& s : em.colorOverLife.stops) {
        if (s.color.a > best) {
            best = s.color.a;
            when = s.t;
        }
    }
    if (at) *at = when;
    return best;
}

float brightest(const VfxColor& c) { return std::max(c.r, std::max(c.g, c.b)); }

} // namespace

TEST_CASE("vfx: F1 the impact's clouds open thin and soft, over the floor") {
    // Round 7a's dust read as flat beige polygons with hard straight edges for its first ~10 frames: plain round puffs
    // born on the floor at full size and nearly full alpha, faded against the floor over a quarter of their size. Each
    // cloud is now the noisy dust sprite, fades against the scene over at least half its size, is born at half its peak
    // alpha or less and reaches the peak within 0.2 s, starts at 0.75 of its size or less, and every spawn (the chunks'
    // too) sits high enough that less than a fifth of a sprite starts under the floor.
    constexpr float kFloor = -0.05f; // the impact plays 5 cm over the floor (stone_bearer_vfx.json's lift)
    VfxEffect e;
    REQUIRE(loadEffect("stone_impact", e));
    int clouds = 0;
    for (const VfxEmitter& em : e.emitters) {
        CAPTURE(em.name);
        const float startHi = hiOf(em.startSize);
        const float open = em.sizeOverLife.evaluate(0.f, 0.f);
        const float lowest = em.offset.y - (em.shape == VfxShape::Sphere ? em.radius : 0.f);
        CHECK(lowest - kFloor >= 0.3f * startHi * open);
        if (em.sprite == VfxSprite::Chip) continue;
        ++clouds;
        CHECK(em.sprite == VfxSprite::Dust);
        CHECK(em.blend == VfxBlend::Alpha);
        CHECK(em.depthFade);
        CHECK(em.depthFadeDistance >= 0.5f * startHi);
        float peakAt = 0.f;
        const float peak = peakAlpha(em, &peakAt);
        CHECK(em.colorOverLife.evaluate(0.f, 0.5f).a <= 0.5f * peak);
        CHECK(peakAt * hiOf(em.lifetime) <= 0.2f);
        CHECK(open <= 0.75f);
        CHECK(em.colorOverLife.evaluate(1.f, 0.5f).a == 0.f);
    }
    CHECK(clouds == 4);
    VfxNiagaraPlan plan;
    planVfxNiagara(e, plan);
    for (const VfxNiagaraEmitter& p : plan.emitters) {
        CAPTURE(p.name);
        if (p.sprite == VfxSprite::Dust) CHECK(p.fadeDistance >= 40.f); // centimetres, written to the material
    }
}

TEST_CASE("vfx: F3 the stamp's dust lands at the foot, not out along its reach") {
    // Round 7a placed the stamp's tell and dust about 2.5 m ahead (ReachShare of its range plus the body radius): its
    // foot contact sits inside the body radius, so it isn't a usable contact and the arc branch used the file's reach.
    // FTStoneBearerHost::StrikeAt now sends a floor blow with such a contact (not in the air) to the contact itself.
    // This guards the data that branch reads, and that the stamp is the only move it catches.
    std::string text, err;
    REQUIRE(ftcore_test::readProjectData("Battle/arena_boss/stone_bearer.json", text));
    StoneBearerDef def;
    REQUIRE_MESSAGE(importStoneBearer(text, def, &err), err);
    REQUIRE(ftcore_test::readProjectData("Battle/arena.json", text));
    ArenaTable table;
    REQUIRE_MESSAGE(importArenaTable(text, table, &err), err);
    std::size_t at = table.enemyIds.size();
    for (std::size_t i = 0; i < table.enemyIds.size(); ++i)
        if (table.enemyIds[i] == "stone_bearer") at = i;
    REQUIRE(at < table.enemies.size());
    const float bodyRadius = table.enemies[at].bodyRadius;

    const std::uint8_t m = def.boss.find("stamp");
    REQUIRE(m != kArenaBossNoMove);
    const ArenaBossMove& stamp = def.boss.moves[m];
    CHECK(stamp.floor);
    CHECK(stamp.hasContact);
    CHECK_FALSE(stamp.contactAir);
    CHECK_FALSE(stamp.intercept()); // the arc branch, not a called impact to a side
    CHECK_FALSE(stoneBearerUsableContact(stamp, bodyRadius));
    const float foot = stoneBearerContactPlanar(stamp);
    CHECK(foot < bodyRadius);                        // under the body's edge
    CHECK(foot < 0.4f * (stamp.range + bodyRadius)); // well short of where round 7a put it

    int caught = 0;
    for (std::uint8_t k = 0; k < def.boss.moveCount; ++k) {
        const ArenaBossMove& b = def.boss.moves[k];
        if (b.floor && b.hasContact && !b.contactAir && !stoneBearerUsableContact(b, bodyRadius)) ++caught;
    }
    CHECK(caught == 1);
}

TEST_CASE("vfx: F4 chips vary in size, outline and spin, fall and die at the floor") {
    // Round 7a's chips were near-identical hexagons (sizes within 1.3x to 2.3x, spin 4 to 8 rad/s, one shade) that hung
    // in the air about 25 frames. Each chip emitter now spans 2.4x in size or more, draws a quad taller or wider than
    // square (startSizeY apart from startSize) whose outline the material varies per particle, spins at 10 rad/s or more
    // either way from any start angle, takes a random shade between two stone colours, and lives exactly until its
    // slowest piece reaches the floor (Lightweight emitters have no collision).
    for (const char* file : {"stone_chips", "stone_chips_heavy", "stone_impact"}) {
        CAPTURE(file);
        VfxEffect e;
        REQUIRE(loadEffect(file, e));
        int chips = 0;
        for (const VfxEmitter& em : e.emitters) {
            if (em.sprite != VfxSprite::Chip) continue;
            ++chips;
            CAPTURE(em.name);
            CHECK(hiOf(em.startSize) >= 2.4f * loOf(em.startSize));
            CHECK(em.hasStartSizeY);
            CHECK_FALSE(em.startSizeY == em.startSize);
            CHECK(loOf(em.angularVelocity) <= -10.f);
            CHECK(hiOf(em.angularVelocity) >= 10.f);
            CHECK(hiOf(em.startRotation) - loOf(em.startRotation) >= 0.95f * 2.f * kPi);
            CHECK(em.startColor.mode == VfxGradientMode::TwoColors);
            CHECK(brightest(em.startColor.colorHi) >= 1.3f * brightest(em.startColor.colorLo));
            CHECK(em.gravity.y <= -9.f);
            REQUIRE(em.hasFloor);
            // A hit's chips start at the lead's chest (0.6 of a 1.75 m body over the feet) or the kneeling boss's
            // pelvis: a floor this deep puts none of them out in the air over the real floor.
            CHECK(em.floorY <= (std::string(file) == "stone_impact" ? -0.05f : -1.05f));
            CHECK(em.colorOverLife.evaluate(0.9f, 0.5f).a == 1.f); // solid until it lands
        }
        CHECK(chips >= 1);
        VfxNiagaraPlan plan;
        planVfxNiagara(e, plan);
        for (const VfxNiagaraEmitter& p : plan.emitters) {
            if (p.sprite != VfxSprite::Chip) continue;
            CAPTURE(p.name);
            CHECK(p.nonUniformSize);
            CHECK(p.floorTime.lo > 0.f);
            CHECK(p.floorTime.hi > p.floorTime.lo);
            CHECK(p.lifetime.lo >= p.floorTime.hi);       // none dies in the air
            CHECK(p.lifetime.hi <= p.floorTime.hi + 0.05f); // nor lingers past the floor
            CHECK(anyGap(p, "no collision"));
        }
    }
    // The impact's chunks land 0.5 to 0.76 s in; its settle dust puffs up on the ground within that window.
    VfxEffect impact;
    REQUIRE(loadEffect("stone_impact", impact));
    VfxNiagaraPlan plan;
    planVfxNiagara(impact, plan);
    const VfxNiagaraEmitter* chunks = nullptr;
    const VfxNiagaraEmitter* settle = nullptr;
    for (const VfxNiagaraEmitter& p : plan.emitters) {
        if (p.name == "chunks") chunks = &p;
        if (p.name == "settle") settle = &p;
    }
    REQUIRE(chunks);
    REQUIRE(settle);
    CHECK(chunks->floorTime.lo == doctest::Approx(0.504f).epsilon(0.02));
    CHECK(chunks->floorTime.hi == doctest::Approx(0.755f).epsilon(0.02));
    REQUIRE(settle->bursts.size() >= 2);
    for (const VfxNiagaraBurst& b : settle->bursts) {
        CHECK(b.time >= chunks->floorTime.lo);
        CHECK(b.time <= chunks->floorTime.hi);
    }
    CHECK(settle->loopDuration > settle->bursts.back().time);
}

TEST_CASE("vfx: the floor time follows the Lightweight solver") {
    // No drag: 1 + 0 t - t^2 = 0 at t = 1; 5 t - 5 t^2 = 0 at t = 1.
    CHECK(vfxFloorTime(1.f, 0.f, -2.f, 0.f, 0.f) == doctest::Approx(1.f));
    CHECK(vfxFloorTime(0.f, 5.f, -10.f, 0.f, 0.f) == doctest::Approx(1.f));
    CHECK(vfxFloorTime(0.f, 5.f, -10.f, 0.f, -1.f) > 1.f);
    // With drag: the solver's closed form at the returned time sits on the floor, a hundredth of a second earlier over it.
    const float y0 = 0.35f, v = 4.5f, g = -13.f, k = 0.3f, floorY = -0.05f;
    const float t = vfxFloorTime(y0, v, g, k, floorY);
    auto height = [&](float s) { return y0 + (v - g / k) * (1.f - std::exp(-k * s)) / k + (g / k) * s; };
    CHECK(t > 0.7f);
    CHECK(height(t) == doctest::Approx(floorY).epsilon(0.001));
    CHECK(height(t - 0.01f) > floorY);
    CHECK(t < vfxFloorTime(y0, v, g, 0.f, floorY)); // drag cuts the climb more than it slows the fall
    // Never comes down / never rises over the floor.
    CHECK(vfxFloorTime(0.f, 1.f, 0.f, 0.f, -1.f) == -1.f);
    CHECK(vfxFloorTime(0.f, 1.f, 2.f, 0.5f, -1.f) == -1.f);
    CHECK(vfxFloorTime(-2.f, -1.f, -10.f, 0.f, -1.f) == 0.f);
    CHECK(vfxFloorTime(-2.f, 1.f, -10.f, 0.f, -1.f) == 0.f);
}

TEST_CASE("vfx: F5 a heavy blow's puff is stone dust, not a pale glowing ball") {
    // Round 7a's puff was a round soft sprite, 0.33 grey-tan at 0.55 alpha: a pale ball. Now it's the impact dust's colour
    // within 0.05 a channel, alpha-blended dust at 0.35 alpha or less, never lighter than the lightest chip, growing
    // 2x or more as it thins to nothing, and thrown out at 1 m/s or more.
    VfxEffect heavy, impact;
    REQUIRE(loadEffect("stone_chips_heavy", heavy));
    REQUIRE(loadEffect("stone_impact", impact));
    const VfxEmitter* dust = emitterNamed(heavy, "dust");
    const VfxEmitter* ground = emitterNamed(impact, "dust");
    REQUIRE(dust);
    REQUIRE(ground);
    CHECK(dust->blend == VfxBlend::Alpha);
    CHECK(dust->sprite == VfxSprite::Dust);
    CHECK(dust->depthFade);
    CHECK(dust->startColor.isWhite());
    float at = 0.f, groundAt = 0.f;
    const float peak = peakAlpha(*dust, &at);
    peakAlpha(*ground, &groundAt);
    const VfxColor c = dust->colorOverLife.evaluate(at, 0.5f);
    const VfxColor g = ground->colorOverLife.evaluate(groundAt, 0.5f);
    CHECK(std::fabs(c.r - g.r) <= 0.05f);
    CHECK(std::fabs(c.g - g.g) <= 0.05f);
    CHECK(std::fabs(c.b - g.b) <= 0.05f);
    CHECK(peak <= 0.35f);
    float chipHi = 0.f;
    for (const VfxEmitter& em : heavy.emitters) {
        if (em.sprite != VfxSprite::Chip) continue;
        for (const VfxColorStop& s : em.colorOverLife.stops) {
            const VfxColor top = em.startColor.colorHi;
            chipHi = std::max(chipHi, brightest({top.r * s.color.r, top.g * s.color.g, top.b * s.color.b, 1.f}));
        }
    }
    for (const VfxColorStop& s : dust->colorOverLife.stops) CHECK(brightest(s.color) <= chipHi);
    CHECK(dust->sizeOverLife.evaluate(1.f, 0.f) >= 2.f * dust->sizeOverLife.evaluate(0.f, 0.f));
    CHECK(dust->colorOverLife.evaluate(1.f, 0.5f).a == 0.f);
    CHECK(loOf(dust->speed) >= 1.f);
    // A light blow's puff follows the same rules, smaller.
    VfxEffect light;
    REQUIRE(loadEffect("stone_chips", light));
    const VfxEmitter* lightDust = emitterNamed(light, "dust");
    REQUIRE(lightDust);
    CHECK(lightDust->sprite == VfxSprite::Dust);
    CHECK(peakAlpha(*lightDust) <= 0.35f);
    CHECK(hiOf(lightDust->startSize) < hiOf(dust->startSize));
}

TEST_CASE("vfx: F6 the single-target tell is a bold ring from its first frame, with no specks") {
    // Round 7a: a 2.0 m ring with a 0.09 m band, spawned by rate (the first ring a fifth of a second in), and chip grit
    // rising off it from the first frame. Now 3.0 m or more with a band of 0.18 m or more, a ring burst at once and at
    // full strength by 0.1 s, rings overlapping so the mark never drops during the wind-up, and no chip specks. The
    // all-round tell keeps round 7a's look.
    VfxEffect tell;
    REQUIRE(loadEffect("stone_tell", tell));
    const VfxEmitter* ring = emitterNamed(tell, "ring");
    REQUIRE(ring);
    const float size = hiOf(ring->startSize);
    CHECK(size >= 1.5f * 2.0f);
    CHECK(0.5f * size * (ring->ringStroke > 0.f ? ring->ringStroke : 0.09f) >= 0.18f);
    CHECK(ring->renderMode == VfxRenderMode::Horizontal);
    CHECK(ring->loop);
    CHECK(ring->rate == 0.f);
    REQUIRE(ring->bursts.size() >= 2);
    CHECK(ring->bursts.front().time == 0.f);
    const float life = loOf(ring->lifetime);
    CHECK(ring->colorOverLife.evaluate(0.1f / life, 0.5f).a >= 0.9f);
    // The next ring is up (alpha at its peak, 0.1 s in) before the last one starts to fade (after 0.8 of its life).
    for (std::size_t i = 0; i < ring->bursts.size(); ++i) {
        const float next =
            i + 1 < ring->bursts.size() ? ring->bursts[i + 1].time : ring->duration + ring->bursts.front().time;
        CHECK(next - ring->bursts[i].time + 0.1f <= 0.8f * life);
    }
    for (const VfxEmitter& em : tell.emitters) {
        CAPTURE(em.name);
        CHECK(em.sprite != VfxSprite::Chip);
        if (em.sprite == VfxSprite::Ring) {
            CHECK(hiOf(em.startSize) >= size);
        } else {
            CHECK(em.colorOverLife.evaluate(0.f, 0.5f).a == 0.f); // the haze rises in, never pops
        }
    }
    VfxEffect area;
    REQUIRE(loadEffect("stone_tell_area", area));
    const VfxEmitter* areaRing = emitterNamed(area, "ring");
    REQUIRE(areaRing);
    CHECK(hiOf(areaRing->startSize) == doctest::Approx(5.f));
    CHECK(areaRing->ringStroke == 0.f);
}

TEST_CASE("vfx: F7 the crit flash ramps in over three frames under the 0.7 m cap") {
    // Round 7a's flash was born at 0.9 of its size and full alpha: on at once. Now frame 0 is a small dim seed (a third
    // of the peak size or less, alpha 0.4 or less), frames 1 and 2 grow, frame 3 is full; still gold, still under 0.7 m,
    // still ending at alpha 0, with the streak sparks unchanged.
    VfxEffect e;
    REQUIRE(loadEffect("crit_burst", e));
    const VfxEmitter* flash = emitterNamed(e, "flash");
    REQUIRE(flash);
    const float life = hiOf(flash->lifetime);
    CHECK(loOf(flash->lifetime) == life);
    CHECK(life <= 0.3f);
    const float start = hiOf(flash->startSize);
    const float peak = start * hiOf(flash->sizeOverLife);
    CHECK(peak <= 0.7f);
    float sizes[4] = {};
    for (int f = 0; f < 4; ++f) {
        const float x = static_cast<float>(f) / 30.f / life;
        sizes[f] = start * flash->sizeOverLife.evaluate(x, 0.f);
    }
    CHECK(sizes[0] <= 0.35f * peak);
    CHECK(sizes[1] > sizes[0]);
    CHECK(sizes[2] > sizes[1]);
    CHECK(sizes[3] >= 0.95f * peak);
    CHECK(flash->colorOverLife.evaluate(0.f, 0.5f).a <= 0.4f);
    CHECK(flash->colorOverLife.evaluate(1.f, 0.5f).a == 0.f);
    const VfxColor hot = flash->colorOverLife.evaluate(3.f / 30.f / life, 0.5f);
    CHECK(hot.a >= 0.9f);
    CHECK(hot.r > hot.g);
    CHECK(hot.g > hot.b);
    CHECK(flash->blend == VfxBlend::Additive);
    const VfxEmitter* sparks = emitterNamed(e, "sparks");
    REQUIRE(sparks);
    CHECK(sparks->stretch > 0.f);
    REQUIRE_FALSE(sparks->bursts.empty());
    CHECK(sparks->bursts.front().time == 0.f);
}

namespace {

// The weak glow's summed alpha t seconds after it opens: the looping stream's particles (rate x life, born evenly from
// 0) plus the one-shot opening burst's.
float glowSum(const VfxEmitter& stream, const VfxEmitter& open, float t) {
    const float life = hiOf(stream.lifetime);
    const float upTo = std::min(t / life, 1.f);
    constexpr int kSteps = 400;
    float sum = 0.f;
    for (int i = 0; i < kSteps; ++i) {
        const float x = (static_cast<float>(i) + 0.5f) / static_cast<float>(kSteps) * upTo;
        sum += stream.colorOverLife.evaluate(x, 0.5f).a;
    }
    sum *= stream.rate * life * upTo / static_cast<float>(kSteps);
    const float openLife = hiOf(open.lifetime);
    if (t < openLife) {
        float n = 0.f;
        for (const VfxBurst& b : open.bursts) n += static_cast<float>(b.count);
        sum += n * open.colorOverLife.evaluate(t / openLife, 0.5f).a;
    }
    return sum;
}

} // namespace

TEST_CASE("vfx: F8 the weak glow is a crack of light that opens in two frames and shuts clean") {
    // Round 7a's glow was two soft gold discs (a lamp) reaching full strength about 8 frames after opening. Its large
    // sprites are now crack sprites held at one angle (no spin, no jitter); a one-shot burst fills the stream's first
    // frames so the summed light is under 15% at frame 0, 35-80% at frame 1 and within 20% of the steady glow from frame
    // 2 on. At shut the stream stops and its particles (all 0.27 s or under) fade out.
    VfxEffect e;
    REQUIRE(loadEffect("weak_glow", e));
    for (const VfxEmitter& em : e.emitters) {
        CAPTURE(em.name);
        CHECK(hiOf(em.lifetime) <= 0.27f);
        CHECK(em.blend == VfxBlend::Additive);
        if (hiOf(em.startSize) <= 0.1f) continue; // the motes
        CHECK(em.sprite == VfxSprite::Crack);
        CHECK(loOf(em.startRotation) == hiOf(em.startRotation));
        CHECK(em.angularVelocity == VfxCurve::constant(0.f));
        CHECK(em.shape == VfxShape::Point);
        CHECK(em.colorOverLife.evaluate(1.f, 0.5f).a == 0.f);
    }
    const VfxEmitter* core = emitterNamed(e, "core");
    const VfxEmitter* open = emitterNamed(e, "open");
    REQUIRE(core);
    REQUIRE(open);
    CHECK(core->loop);
    CHECK_FALSE(open->loop);
    CHECK(core->rate > 0.f);
    REQUIRE_FALSE(open->bursts.empty());
    CHECK(open->bursts.front().time == 0.f);
    const float steady = glowSum(*core, *open, 1.f);
    CHECK(steady > 0.f);
    CHECK(glowSum(*core, *open, 0.f) <= 0.15f * steady);
    const float f1 = glowSum(*core, *open, 1.f / 30.f);
    CHECK(f1 >= 0.35f * steady);
    CHECK(f1 <= 0.8f * steady);
    for (int f = 2; f <= 8; ++f) {
        CAPTURE(f);
        const float s = glowSum(*core, *open, static_cast<float>(f) / 30.f);
        CHECK(s >= 0.8f * steady);
        CHECK(s <= 1.2f * steady);
    }
}

TEST_CASE("vfx: offset, floor, startSizeY, the depth fade distance and the ring stroke reach the plan") {
    VfxEffect e;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importVfxEffect(R"J({"name": "k", "emitters": [
        {"name": "a", "bursts": [{"count": 4}], "shape": "sphere", "radius": 0.1, "offset": [0, 0.5, 0],
         "speed": [1, 2], "gravity": [0, -10, 0], "floor": -0.2, "startSize": [0.1, 0.3], "startSizeY": [0.05, 0.2],
         "sprite": "chip", "depthFade": true, "depthFadeDistance": 0.4},
        {"name": "b", "sprite": "ring", "renderMode": "horizontal", "ringStroke": 0.2, "softness": 0},
        {"name": "c", "sprite": "dust", "depthFadeDistance": 0.3, "softness": 0},
        {"name": "d", "sprite": "crack", "offset": [0, 1, 0], "ringStroke": 0.1, "softness": 0},
        {"name": "e", "bursts": [{"count": 1}], "floor": -1, "softness": 0}]})J",
                                    e, &err, &warnings),
                    err);
    CHECK(warnings.empty());
    REQUIRE(e.emitters.size() == 5);
    const VfxEmitter& a = e.emitters[0];
    CHECK(a.offset == VfxVec3{0.f, 0.5f, 0.f});
    CHECK(a.hasFloor);
    CHECK(a.floorY == doctest::Approx(-0.2f));
    CHECK(a.hasStartSizeY);
    CHECK(a.depthFadeDistance == doctest::Approx(0.4f));
    CHECK_FALSE(e.emitters[1].hasFloor);
    CHECK_FALSE(e.emitters[1].hasStartSizeY);
    CHECK(e.emitters[2].sprite == VfxSprite::Dust);
    CHECK(e.emitters[3].sprite == VfxSprite::Crack);

    VfxNiagaraPlan plan;
    planVfxNiagara(e, plan);
    REQUIRE(plan.emitters.size() == 5);
    const VfxNiagaraEmitter& pa = plan.emitters[0];
    CHECK(pa.shapeOffset.z == doctest::Approx(50.f));
    CHECK(pa.pointOrigin.z == doctest::Approx(50.f)); // the outward push starts from the moved centre
    CHECK(pa.nonUniformSize);
    CHECK(pa.spriteSize.lo == doctest::Approx(10.f));
    CHECK(pa.spriteSizeY.lo == doctest::Approx(5.f));
    CHECK(pa.spriteSizeY.hi == doctest::Approx(20.f));
    CHECK(pa.fadeDistance == doctest::Approx(40.f));
    // The last particle: 60 cm up at 200 cm/s under -1000 cm/s^2 reaches -20 cm at (200 + sqrt(200000)) / 1000 s; the
    // first: 40 cm up at -200 cm/s at 0.2 s.
    CHECK(pa.floorTime.hi == doctest::Approx((200.f + std::sqrt(200000.f)) / 1000.f).epsilon(0.001));
    CHECK(pa.floorTime.lo == doctest::Approx(0.2f).epsilon(0.001));
    CHECK(pa.lifetime.lo == doctest::Approx(pa.floorTime.hi + 1.f / 30.f));
    CHECK(pa.lifetime.hi == pa.lifetime.lo);
    CHECK(anyGap(pa, "no collision"));
    CHECK(plan.emitters[1].ringStroke == doctest::Approx(0.2f));
    CHECK_FALSE(anyGap(plan.emitters[1], "ringStroke"));
    CHECK(plan.emitters[2].sprite == VfxSprite::Dust);
    CHECK(plan.emitters[2].fadeDistance == 0.f);
    CHECK(anyGap(plan.emitters[2], "depthFadeDistance: ignored"));
    CHECK_FALSE(anyGap(plan.emitters[2], "sprite puff"));
    const VfxNiagaraEmitter& pd = plan.emitters[3];
    CHECK(pd.sprite == VfxSprite::Crack);
    CHECK(pd.shape == VfxNiagaraShape::Sphere); // a point shape carries the offset as a tiny sphere
    CHECK(pd.shapeOffset.z == doctest::Approx(100.f));
    CHECK(pd.ringStroke == 0.f);
    CHECK(anyGap(pd, "ringStroke: ignored"));
    CHECK(anyGap(pd, "decal"));
    CHECK(anyGap(plan.emitters[4], "needs a downward gravity"));
    CHECK(plan.emitters[4].lifetime.lo == doctest::Approx(1.f));

    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "ringStroke": 0.6}]})J") ==
          "emitters[0].ringStroke: expected 0..0.45");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "depthFadeDistance": -1}]})J") ==
          "emitters[0].depthFadeDistance: expected >= 0");
}

TEST_CASE("vfx: fireworks reads bursts, modules and sub-emitters") {
    VfxEffect e;
    REQUIRE(loadEffect("fireworks", e));
    const VfxEmitter& rocket = e.emitters[0];
    CHECK(rocket.name == "rocket");
    REQUIRE(rocket.bursts.size() == 2);
    CHECK(rocket.bursts[1].time == doctest::Approx(0.8f));
    CHECK(rocket.bursts[1].count == 1);
    CHECK(rocket.loop);
    CHECK(rocket.duration == doctest::Approx(1.6f));
    CHECK(rocket.shape == VfxShape::Cone);
    CHECK(rocket.speed.mode == VfxCurveMode::TwoConstants);
    CHECK(rocket.speed.lo == doctest::Approx(5.f));
    CHECK(rocket.speed.hi == doctest::Approx(5.8f));
    CHECK(rocket.gravity == VfxVec3{0.f, -3.f, 0.f});
    CHECK(rocket.startColor.mode == VfxGradientMode::RandomColor);
    CHECK(rocket.startColor.stops.size() == 4);
    CHECK(rocket.blend == VfxBlend::Additive);
    CHECK(rocket.sprite == VfxSprite::Spark);
    CHECK(rocket.trails.enabled);
    CHECK(rocket.trails.maxPoints == 24);
    REQUIRE(rocket.subEmitters.size() == 2);
    CHECK(rocket.subEmitters[0].emitter == "burst");
    CHECK(rocket.subEmitters[0].on == VfxSubEmitterEvent::Death);
    CHECK(rocket.subEmitters[0].countMin == 70);
    CHECK(rocket.subEmitters[0].countMax == 90);
    CHECK(rocket.subEmitters[0].inherit == kVfxInheritColor);
    CHECK(rocket.subEmitters[1].countMin == 1);
    CHECK(rocket.subEmitters[1].countMax == 1);
    CHECK(findVfxEmitter(e, "flash") == 2);
    CHECK(findVfxEmitter(e, "nope") == -1);
}

TEST_CASE("vfx: other module forms read as in newengine") {
    VfxEffect e;
    REQUIRE(loadEffect("ember_swirl", e));
    const VfxEmitter& em = e.emitters[0];
    CHECK(em.forceOverLifetime.enabled);
    CHECK(em.forceOverLifetime.force[0] == VfxCurve::constant(0.f));
    CHECK(em.forceOverLifetime.force[1].mode == VfxCurveMode::Curve);
    CHECK(em.forceOverLifetime.force[1].keys.size() == 2);
    CHECK(em.limitVelocity.enabled);
    CHECK(em.limitVelocity.dampen == doctest::Approx(0.25f));
    CHECK(em.noise.enabled);
    CHECK(em.noise.octaves == 2);
    CHECK(em.noise.quality == VfxNoiseQuality::High);
    CHECK(em.stretch == doctest::Approx(0.04f));

    VfxEffect fire;
    REQUIRE(loadEffect("flipbook_fire", fire));
    CHECK(fire.emitters[0].textureSheet.enabled);
    CHECK(fire.emitters[0].textureSheet.tilesX == 4);
    CHECK(fire.emitters[0].textureSheet.tilesY == 4);
    CHECK(fire.emitters[0].rotationOverLifetime.enabled);

    VfxEffect leaves;
    REQUIRE(loadEffect("falling_leaves", leaves));
    CHECK(leaves.emitters[0].renderMode == VfxRenderMode::Mesh);
    CHECK(leaves.emitters[0].mesh == "leaf");

    VfxEffect sparks;
    REQUIRE(loadEffect("stretched_sparks", sparks));
    CHECK(sparks.emitters[0].renderMode == VfxRenderMode::Stretched);
    CHECK(sparks.emitters[0].sortMode == VfxSortMode::None);
    CHECK(sparks.emitters[1].renderMode == VfxRenderMode::Horizontal);
}

TEST_CASE("vfx: curve and gradient value forms") {
    VfxEffect e;
    std::string err;
    REQUIRE_MESSAGE(importVfxEffect(R"J({"name": "one",
        "lifetime": {"curve": [{"t": 1, "v": 4}, {"t": 0, "v": 2}], "mul": 0.5},
        "startSize": {"min": [{"t": 0, "v": 1}], "max": [{"t": 0, "v": 3}]},
        "speed": [2, 2],
        "sizeOverLife": [],
        "startColor": {"between": [[1, 0, 0, 1], [0, 0, 1, 1]]},
        "colorOverLife": [0.5, 0.5, 0.5, 1]})J",
                                    e, &err),
                    err);
    REQUIRE(e.emitters.size() == 1);
    CHECK(e.name == "one"); // a single-emitter file takes the emitter's name
    const VfxEmitter& m = e.emitters[0];
    CHECK(m.lifetime.mode == VfxCurveMode::Curve);
    CHECK(m.lifetime.keys[0].t == 0.f); // sorted by t
    CHECK(m.lifetime.evaluate(0.5f, 0.f) == doctest::Approx(1.5f));
    CHECK(m.startSize.mode == VfxCurveMode::TwoCurves);
    CHECK(m.startSize.evaluate(0.f, 0.5f) == doctest::Approx(2.f));
    CHECK(m.speed.mode == VfxCurveMode::TwoConstants);
    CHECK(m.sizeOverLife.mode == VfxCurveMode::Curve);
    CHECK(m.sizeOverLife.keys.empty());
    CHECK(m.sizeOverLife.evaluate(0.3f, 0.f) == 1.f);
    CHECK(m.startColor.mode == VfxGradientMode::TwoColors);
    CHECK(m.startColor.evaluate(0.f, 0.5f) == VfxColor{0.5f, 0.f, 0.5f, 1.f});
    CHECK(m.colorOverLife.mode == VfxGradientMode::Color);
    CHECK_FALSE(m.colorOverLife.isWhite());
    float lo = 0.f, hi = 0.f;
    m.lifetime.bounds(lo, hi);
    CHECK(lo == doctest::Approx(1.f));
    CHECK(hi == doctest::Approx(2.f));
}

TEST_CASE("vfx: sampling clamps and handles empty inputs") {
    const std::vector<VfxKey> keys{{0.f, 0.f}, {0.5f, 1.f}, {0.5f, 3.f}, {1.f, 2.f}};
    CHECK(sampleVfxKeys(keys, -1.f) == 0.f);
    CHECK(sampleVfxKeys(keys, 0.25f) == doctest::Approx(0.5f));
    CHECK(sampleVfxKeys(keys, 0.5f) == doctest::Approx(1.f));
    CHECK(sampleVfxKeys(keys, 0.75f) == doctest::Approx(2.5f));
    CHECK(sampleVfxKeys(keys, 2.f) == 2.f);
    CHECK(sampleVfxKeys({}, 0.4f) == 1.f);
    CHECK(sampleVfxStops({}, 0.4f) == VfxColor{});
    CHECK(VfxGradient{}.isWhite());
}

TEST_CASE("vfx: unknown keys are warnings with their path") {
    VfxEffect e;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE(importVfxEffect(R"J({"name": "w", "emitters": [{"name": "a", "colour": [1, 1, 1, 1],
                                  "noise": {"strength": 1, "wobble": 2}}], "extra": 1})J",
                            e, &err, &warnings));
    REQUIRE(warnings.size() == 3);
    CHECK(contains(warnings[0], "emitters[0].noise.wobble"));
    CHECK(contains(warnings[1], "emitters[0].colour"));
    CHECK(contains(warnings[2], "extra"));
}

TEST_CASE("vfx: bad input reports a reason with its path") {
    CHECK(contains(importError("{"), "line"));
    CHECK(importError(R"J({"name": "x", "emitters": []})J") == "emitters: expected at least one emitter");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "renderMode": "mesh"}]})J") ==
          "emitters[0].mesh: renderMode \"mesh\" needs a mesh name");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "renderMode": "quad"}]})J") ==
          "emitters[0].renderMode: unknown value \"quad\" (billboard, stretched, horizontal, vertical, mesh)");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "noise": {"octaves": 9}}]})J") ==
          "emitters[0].noise.octaves: expected 1..8");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "textureSheet": {"tiles": [0, 4]}}]})J") ==
          "emitters[0].textureSheet.tiles: expected [x, y], each 1..64");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "trails": {"ratio": 2}}]})J") ==
          "emitters[0].trails.ratio: expected 0..1");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "trails": {"maxPoints": 1}}]})J") ==
          "emitters[0].trails.maxPoints: expected 2..256");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "trails": {"ribbonCount": 0}}]})J") ==
          "emitters[0].trails.ribbonCount: expected >= 1");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "rate": "fast"}]})J") ==
          "emitters[0].rate: expected a number");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "gravity": [0, 1]}]})J") ==
          "emitters[0].gravity: expected [x, y, z]");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "bursts": [{"time": 0}]}]})J") ==
          "emitters[0].bursts[0].count: missing");

    const char* subs[][2] = {
        {R"J([{"emitter": "b", "count": [5, 2]}])J", "emitters[0].subEmitters[0].count: expected n or [min, max] with min <= max"},
        {R"J([{"emitter": "b", "probability": 1.5}])J", "emitters[0].subEmitters[0].probability: expected 0..1"},
        {R"J([{"emitter": "b", "inherit": ["hue"]}])J",
         "emitters[0].subEmitters[0].inherit[0]: unknown property \"hue\" (color, size, rotation, velocity)"},
        {R"J([{"emitter": "zz"}])J", "emitters[0].subEmitters[0].emitter: unknown emitter \"zz\""},
        {R"J([{"emitter": "a"}])J", "emitters[0].subEmitters[0].emitter: an emitter cannot be its own sub-emitter"},
    };
    for (const auto& s : subs) {
        const std::string json =
            std::string(R"J({"name": "x", "emitters": [{"name": "a", "subEmitters": )J") + s[0] + R"J(}, {"name": "b"}]})J";
        CHECK(importError(json.c_str()) == s[1]);
    }
    const std::string cycle = importError(R"J({"name": "x", "emitters": [
        {"name": "a", "subEmitters": [{"emitter": "b"}]}, {"name": "b", "subEmitters": [{"emitter": "a"}]}]})J");
    CHECK(contains(cycle, "emitters[0].subEmitters[0].emitter: sub-emitter cycle through \"b\""));
    CHECK(contains(cycle, "emitters[1].subEmitters[0].emitter: sub-emitter cycle through \"a\""));

    // Several problems: one line each.
    const std::string many =
        importError(R"J({"name": "x", "emitters": [{"name": "a", "drag": true, "noise": {"octaves": 0}}]})J");
    CHECK(many == "emitters[0].drag: expected a number\nemitters[0].noise.octaves: expected 1..8");
}

TEST_CASE("vfx: ring and chip sprites and the depth fade reach the plan") {
    VfxEffect e;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE(importVfxEffect(R"J({"name": "s", "emitters": [
                                  {"name": "mark", "sprite": "ring", "renderMode": "horizontal"},
                                  {"name": "bits", "sprite": "chip", "depthFade": true},
                                  {"name": "dust", "sprite": "puff"}]})J",
                            e, &err, &warnings));
    CHECK(warnings.empty());
    REQUIRE(e.emitters.size() == 3);
    CHECK(e.emitters[0].sprite == VfxSprite::Ring);
    CHECK(e.emitters[1].sprite == VfxSprite::Chip);
    CHECK(e.emitters[1].depthFade);
    CHECK_FALSE(e.emitters[0].depthFade); // off unless asked for
    VfxNiagaraPlan plan;
    planVfxNiagara(e, plan);
    REQUIRE(plan.emitters.size() == 3);
    CHECK(plan.emitters[0].sprite == VfxSprite::Ring);
    CHECK(plan.emitters[0].facing == VfxNiagaraFacing::Horizontal);
    CHECK_FALSE(plan.emitters[0].depthFade);
    CHECK(plan.emitters[1].sprite == VfxSprite::Chip);
    CHECK(plan.emitters[1].depthFade);
    // softness defaults on, so only the emitter that asks for the depth fade drops the gap.
    CHECK(anyGap(plan.emitters[0], "depth fade"));
    CHECK_FALSE(anyGap(plan.emitters[1], "depth fade"));
    CHECK(contains(importError(R"J({"name": "x", "emitters": [{"name": "a", "sprite": "star"}]})J"), "ring, chip"));
}

TEST_CASE("vfx: coordinates map to Unreal centimetres, Z up") {
    CHECK(vfxToUnreal({1.f, 2.f, 3.f}) == VfxVec3{100.f, 300.f, 200.f});
}

TEST_CASE("vfx: Niagara plan for heal_ring spawns on a ring; the orbit is a gap") {
    VfxEffect e;
    REQUIRE(loadEffect("heal_ring", e));
    VfxNiagaraPlan plan;
    planVfxNiagara(e, plan);
    REQUIRE(plan.emitters.size() == 1);
    const VfxNiagaraEmitter& p = plan.emitters[0];
    CHECK(plan.loops);
    CHECK(p.loop);
    CHECK(p.rate == doctest::Approx(70.f));
    // Lightweight emitters cannot orbit (no RotateAroundPoint in the simulation shader): the ring stays.
    CHECK(anyGap(p, "orbital"));
    CHECK(p.shape == VfxNiagaraShape::Ring);
    CHECK(p.ringRadius == doctest::Approx(100.f));
    CHECK(p.discCoverage == doctest::Approx(0.05f));
    CHECK(p.velocity == VfxNiagaraVelocity::Linear);
    CHECK(p.linearLo == p.linearHi);
    CHECK(p.linearLo.x == 0.f);
    CHECK(p.linearLo.z == doctest::Approx(60.f));
    CHECK(p.additive);
    CHECK(p.spriteSize.lo == doctest::Approx(6.f));
    CHECK(p.spriteSize.hi == doctest::Approx(12.f));
    CHECK(p.sizeOverLife.size() == 3);
    CHECK(p.colorOverLife.size() == 3);
    CHECK(anyGap(p, "radial"));
}

TEST_CASE("vfx: Niagara plan doubles the cone angle and places death bursts") {
    VfxEffect e;
    REQUIRE(loadEffect("fireworks", e));
    VfxNiagaraPlan plan;
    planVfxNiagara(e, plan);
    REQUIRE(plan.emitters.size() == 3);
    const VfxNiagaraEmitter& rocket = plan.emitters[0];
    CHECK(rocket.velocity == VfxNiagaraVelocity::Cone);
    CHECK(rocket.coneAngle == doctest::Approx(24.f)); // full angle
    CHECK(rocket.coneDirection == VfxVec3{0.f, 0.f, 1.f});
    CHECK(rocket.speed.lo == doctest::Approx(500.f));
    CHECK(rocket.gravity == VfxVec3{0.f, 0.f, -300.f});
    CHECK(rocket.startColorRandom);
    CHECK(rocket.startColor.size() == 4);
    CHECK(rocket.shape == VfxNiagaraShape::Ring);
    CHECK(rocket.discCoverage == 1.f);
    CHECK(anyGap(rocket, "trails"));

    const VfxNiagaraEmitter& burst = plan.emitters[1];
    CHECK(burst.rate == 0.f);
    CHECK(burst.loop);
    CHECK(burst.loopDuration == doctest::Approx(1.6f));
    REQUIRE(burst.bursts.size() == 2);
    CHECK(burst.bursts[0].time == doctest::Approx(1.05f));
    CHECK(burst.bursts[1].time == doctest::Approx(0.25f)); // 0.8 + 1.05 wraps in the 1.6 s loop
    CHECK(burst.bursts[0].countLo == 70);
    CHECK(burst.bursts[0].countHi == 90);
    // The rocket dies at about 540 * 1.05 - 150 * 1.05^2 cm up.
    CHECK(burst.shapeOffset.z == doctest::Approx(540.f * 1.05f - 150.f * 1.05f * 1.05f).epsilon(0.01));
    CHECK(burst.pointOrigin.z == doctest::Approx(burst.shapeOffset.z));
    CHECK(burst.velocity == VfxNiagaraVelocity::FromPoint);
    CHECK(anyGap(burst, "inherit"));

    const VfxNiagaraEmitter& flash = plan.emitters[2];
    REQUIRE(flash.bursts.size() == 2);
    CHECK(flash.bursts[0].countLo == 1);
    CHECK(flash.shape == VfxNiagaraShape::Sphere); // carries the offset
    CHECK(flash.shapeOffset.z == doctest::Approx(burst.shapeOffset.z));
}

TEST_CASE("vfx: Niagara plan stretches and lays flat") {
    VfxEffect e;
    REQUIRE(loadEffect("stretched_sparks", e));
    VfxNiagaraPlan plan;
    planVfxNiagara(e, plan);
    const VfxNiagaraEmitter& sparks = plan.emitters[0];
    CHECK(sparks.facing == VfxNiagaraFacing::VelocityAligned);
    CHECK(sparks.scaleBySpeed);
    CHECK(sparks.minScale == VfxVec2{1.f, 2.f});
    CHECK(sparks.maxScale.y > 2.f);
    CHECK(sparks.speedThreshold == doctest::Approx(700.f));
    CHECK(plan.emitters[1].facing == VfxNiagaraFacing::Horizontal);

    VfxEffect embers;
    REQUIRE(loadEffect("ember_swirl", embers));
    planVfxNiagara(embers, plan);
    const VfxNiagaraEmitter& m = plan.emitters[0];
    CHECK(m.facing == VfxNiagaraFacing::VelocityAligned);
    CHECK(m.maxScale.y <= 3.f);
    CHECK(m.curlNoise);
    CHECK(m.noiseFrequency == doctest::Approx(0.4f));
    CHECK(m.noiseStrength == doctest::Approx(120.f / 0.8f));
    CHECK(m.acceleration.z == doctest::Approx(90.f).epsilon(0.01)); // mean of 1.4 -> 0.4 m/s^2
    CHECK(anyGap(m, "limitVelocity"));
    CHECK(anyGap(m, "octaves"));
}

TEST_CASE("vfx: one-shot plans report their duration") {
    VfxEffect e;
    std::string err;
    REQUIRE(importVfxEffect(R"J({"name": "pop", "bursts": [{"count": 10}], "duration": 0.5, "lifetime": [0.2, 0.8]})J", e,
                            &err));
    VfxNiagaraPlan plan;
    planVfxNiagara(e, plan);
    CHECK_FALSE(plan.loops);
    CHECK(plan.duration == doctest::Approx(1.3f));
    CHECK(plan.emitters[0].velocity == VfxNiagaraVelocity::None);
    CHECK(plan.emitters[0].shape == VfxNiagaraShape::None);
}

TEST_CASE("vfx: user parameters are clamped") {
    VfxUserParameters p;
    p.speedScale = -2.f;
    p.shapeScale = std::nanf("");
    p = sanitizeVfxUserParameters(p);
    CHECK(p.speedScale == 0.f);
    CHECK(p.shapeScale == 1.f);
    p.speedScale = 50.f;
    CHECK(sanitizeVfxUserParameters(p).speedScale == 10.f);
    CHECK(sanitizeVfxUserParameters(VfxUserParameters{}).speedScale == 1.f);
}
