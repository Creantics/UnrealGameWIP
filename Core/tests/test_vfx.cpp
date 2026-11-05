#include "ftcore/Vfx.h"
#include "ftcore/ArenaKindCrit.h" // white_glove's seam window, which dealer_seam_open covers

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

const VfxEmitter* findEmitter(const VfxEffect& e, const char* name) {
    for (const VfxEmitter& em : e.emitters)
        if (em.name == name) return &em;
    return nullptr;
}

float peakAlpha(const VfxGradient& g) {
    float peak = 0.f;
    for (const VfxColorStop& s : g.stops) peak = std::max(peak, s.color.a);
    return peak;
}

// Where in a particle's life its colour first reaches 90% of its peak alpha: the ramp-in.
float rampInAt(const VfxGradient& g) {
    const float peak = peakAlpha(g);
    for (const VfxColorStop& s : g.stops)
        if (s.color.a >= 0.9f * peak) return s.t;
    return 1.f;
}

// Where in a particle's life its colour last holds 90% of its peak alpha: the fade starts after it.
float fadeFrom(const VfxGradient& g) {
    const float peak = peakAlpha(g);
    float t = 0.f;
    for (const VfxColorStop& s : g.stops)
        if (s.color.a >= 0.9f * peak) t = s.t;
    return t;
}

float lowest(const VfxCurve& c) {
    float a = 0.f, b = 0.f;
    c.bounds(a, b);
    return a;
}

float highest(const VfxCurve& c) {
    float a = 0.f, b = 0.f;
    c.bounds(a, b);
    return b;
}

// The largest quad edge an emitter's particles reach, in metres.
float peakSize(const VfxEmitter& em) {
    return highest(em.startSize) * (em.sizeOverLife.keys.empty() ? 1.f : highest(em.sizeOverLife));
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
                            {"dealer_deal_tell", 2}, {"dealer_seam_open", 5}, {"dealer_seam_crit", 5}, {"ink_hit", 4},
                            {"ink_hit_floor", 2}, {"dealer_seam_crit_floor", 2}};
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

TEST_CASE("vfx: the Dealer's effects load without warnings and keep their particle budgets") {
    struct Expect {
        const char* file;
        std::size_t emitters;
        std::uint32_t budget; // the sum of the emitters' maxParticles
        bool loops;
    };
    const Expect files[] = {{"dealer_deal_tell", 2, 16, true},
                            {"dealer_seam_open", 5, 28, false},
                            {"dealer_seam_crit", 5, 38, false},
                            {"ink_hit", 4, 39, false},
                            {"ink_hit_floor", 2, 11, false},
                            {"dealer_seam_crit_floor", 2, 13, false}};
    for (const Expect& f : files) {
        CAPTURE(f.file);
        VfxEffect e;
        std::vector<std::string> warnings;
        REQUIRE(loadEffect(f.file, e, &warnings));
        CHECK(e.emitters.size() == f.emitters);
        CHECK_MESSAGE(warnings.empty(), (warnings.empty() ? std::string() : warnings.front()));
        std::uint32_t total = 0;
        for (const VfxEmitter& em : e.emitters) {
            total += em.maxParticles;
            CHECK(em.loop == f.loops);
        }
        CHECK(total <= f.budget);
    }
}

TEST_CASE("vfx: the Dealer's round-8 looks: red that ramps in, a seam open for the whole window, ink that falls") {
    constexpr float kFrame = 1.f / 30.f; // the capture's frame
    // The open seam covers white_glove's whole window: every emitter runs it, the crosshatch lives through it and the
    // glow's pulses overlap to its end.
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Battle/arena_crit.json", text));
    ArenaKindCritTable table;
    REQUIRE(importArenaKindCrits(text, table));
    const ArenaKindCritDef* glove = table.find("white_glove");
    REQUIRE(glove);
    const float opens = static_cast<float>(glove->opens);
    VfxEffect seam;
    REQUIRE(loadEffect("dealer_seam_open", seam));
    for (const VfxEmitter& em : seam.emitters) {
        CAPTURE(em.name);
        CHECK(em.duration == doctest::Approx(opens));
    }
    const VfxEmitter* glow = findEmitter(seam, "glow");
    const VfxEmitter* hatch = findEmitter(seam, "hatch");
    const VfxEmitter* drips = findEmitter(seam, "drips");
    REQUIRE(glow);
    REQUIRE(hatch);
    REQUIRE(drips);
    CHECK(lowest(hatch->lifetime) == doctest::Approx(opens));
    REQUIRE(glow->bursts.size() >= 4); // the pulse
    for (std::size_t i = 1; i < glow->bursts.size(); ++i)
        CHECK(glow->bursts[i].time - glow->bursts[i - 1].time < lowest(glow->lifetime));
    CHECK(glow->bursts.back().time + lowest(glow->lifetime) >= opens);
    // The seam reads at play distance as long crossed lines, each at least 0.7 m (15 px at 60 px of figure), over a
    // glow twice round 8b's: a 1.0-1.2 m soft quad, about 10-12 px.
    CHECK(hatch->renderMode == VfxRenderMode::Stretched);
    CHECK(lowest(hatch->startSize) * hatch->lengthScale >= 0.7f);
    CHECK(peakSize(*glow) >= 1.f);
    CHECK(peakSize(*glow) <= 1.2f);
    // The drips fall in the world, stretched along their fall, wide enough to survive 60 px.
    CHECK(drips->space == VfxSpace::World);
    CHECK(drips->gravity.y <= -9.f);
    CHECK(drips->renderMode == VfxRenderMode::Stretched);
    CHECK(lowest(drips->startSize) >= 0.04f);

    // The tell and the seam ramp in over 3 frames or more and are real red: alpha-blended red, never a big additive
    // disc (additive red on the pale glove clips to pink). The tell is red on the fingers, not a ball: veins of red
    // splash over them and thin red lines flicking off their edge.
    VfxEffect tell;
    REQUIRE(loadEffect("dealer_deal_tell", tell));
    const VfxEmitter* veins = findEmitter(tell, "veins");
    const VfxEmitter* edge = findEmitter(tell, "edge");
    const VfxEmitter* stain = findEmitter(seam, "stain");
    REQUIRE(veins);
    REQUIRE(edge);
    REQUIRE(stain);
    CHECK(veins->sprite == VfxSprite::Splash);
    CHECK(edge->renderMode == VfxRenderMode::Stretched);
    for (const VfxEmitter& em : tell.emitters) {
        CAPTURE(em.name);
        CHECK(em.sprite != VfxSprite::Soft);
    }
    for (const VfxEmitter* em : {veins, glow, hatch, stain}) {
        CAPTURE(em->name);
        REQUIRE_FALSE(em->colorOverLife.stops.empty());
        CHECK(em->colorOverLife.stops.front().t == 0.f);
        CHECK(em->colorOverLife.stops.front().color.a == 0.f);
        CHECK(rampInAt(em->colorOverLife) * lowest(em->lifetime) >= 3.f * kFrame);
    }
    for (const VfxEffect* fx : {&tell, &seam}) {
        for (const VfxEmitter& em : fx->emitters) {
            CAPTURE(fx->name);
            CAPTURE(em.name);
            if (em.blend == VfxBlend::Additive) {
                CHECK(peakSize(em) <= 0.2f);
                continue;
            }
            for (const VfxColorStop& s : em.colorOverLife.stops)
                CHECK(s.color.r >= 4.f * std::max(s.color.g, s.color.b));
        }
    }

    // The plain hit: a wet ink splash about 1 m across (its body reads at about 0.8 of its edge, tendrils beyond) that
    // thins over 8 frames or more; the crit outgrows it and lasts 8 frames or more.
    VfxEffect ink;
    VfxEffect crit;
    REQUIRE(loadEffect("ink_hit", ink));
    REQUIRE(loadEffect("dealer_seam_crit", crit));
    const VfxEmitter* core = findEmitter(ink, "core");
    REQUIRE(core);
    CHECK(lowest(core->startSize) >= 1.1f);
    CHECK(core->sprite == VfxSprite::Splash);
    CHECK(lowest(core->lifetime) * (1.f - fadeFrom(core->colorOverLife)) >= 8.f * kFrame);
    float inkPeak = 0.f;
    float critPeak = 0.f;
    float critLife = 0.f;
    for (const VfxEmitter& em : ink.emitters) inkPeak = std::max(inkPeak, peakSize(em));
    for (const VfxEmitter& em : crit.emitters) {
        critPeak = std::max(critPeak, peakSize(em));
        critLife = std::max(critLife, lowest(em.lifetime));
    }
    CHECK(critPeak > inkPeak);
    CHECK(critLife >= 8.f * kFrame);

    // Debris: whatever flies out falls under full gravity, as spinning wet blots (six silhouettes, one per particle) or
    // drops and flecks stretched along their velocity, never the round soft sprite or the one chip.
    for (const VfxEffect* fx : {&ink, &crit}) {
        bool blots = false;
        for (const VfxEmitter& em : fx->emitters) {
            if (highest(em.speed) < 0.5f) continue; // the cores, the flash, the pop
            CAPTURE(fx->name);
            CAPTURE(em.name);
            CHECK(em.gravity.y <= -9.f);
            CHECK(em.sprite != VfxSprite::Soft);
            CHECK(em.sprite != VfxSprite::Chip);
            if (em.sprite == VfxSprite::Blot) {
                blots = true;
                CHECK(lowest(em.angularVelocity) < 0.f);
                CHECK(highest(em.angularVelocity) > 0.f);
                CHECK(highest(em.startRotation) > 3.f);
            } else {
                CHECK(em.renderMode == VfxRenderMode::Stretched);
            }
            // Untinted ink, never the seam's red: the crit's chunks too.
            for (const VfxColorStop& s : em.colorOverLife.stops) CHECK(s.color.r < 0.05f);
        }
        CHECK(blots);
    }
}

namespace {

// How high (m, up) a particle thrown up at vy m/s is after t s, under gravity gy and drag k: the stateless model the
// plan's lightweight emitters run (p = v0 f + g (t - f) / k, f = (1 - e^-kt) / k).
float dragHeight(float vy, float gy, float k, float t) {
    const float f = k > 0.f ? (1.f - std::exp(-k * t)) / k : t;
    const float h = k > 0.f ? (t - f) / k : 0.5f * t * t;
    return vy * f + gy * h;
}

// Seconds until a particle thrown up at vy m/s is `drop` metres under where it started.
float fallTime(float vy, float gy, float k, float drop) {
    float lo = 0.f;
    float hi = 10.f;
    for (int i = 0; i < 50; ++i) {
        const float mid = 0.5f * (lo + hi);
        if (dragHeight(vy, gy, k, mid) > -drop)
            lo = mid;
        else
            hi = mid;
    }
    return hi;
}

} // namespace

TEST_CASE("vfx: the Dealer's ink lands: debris falls through the floor at full alpha, and marks splat in as it lands") {
    constexpr float kFrame = 1.f / 30.f;
    struct Landing {
        const char* fx;
        const char* floor;
        float height; // metres over the floor the effect plays at
    };
    // ink_hit plays at chest height (the showcase's 1.62 m); the crit at the right hand, about 1.0 m up at its cue.
    const Landing cases[] = {{"ink_hit", "ink_hit_floor", 1.62f}, {"dealer_seam_crit", "dealer_seam_crit_floor", 1.f}};
    for (const Landing& c : cases) {
        CAPTURE(c.fx);
        VfxEffect fx;
        VfxEffect floor;
        REQUIRE(loadEffect(c.fx, fx));
        REQUIRE(loadEffect(c.floor, floor));
        float first = 1e9f;
        float last = 0.f;
        for (const VfxEmitter& em : fx.emitters) {
            if (highest(em.speed) < 0.5f) continue;
            CAPTURE(em.name);
            // Full alpha to the end: debris vanishes under the floor (which hides it), never in mid-air.
            for (const VfxColorStop& s : em.colorOverLife.stops) CHECK(s.color.a >= 0.9f);
            // Even the fastest throw straight up is under the floor before it dies.
            CHECK(lowest(em.lifetime) >= fallTime(highest(em.speed), em.gravity.y, em.drag, c.height));
            // The flattest, slowest throw lands first; the fastest straight up, last.
            const float tilt = em.shape == VfxShape::Cone ? em.coneAngle * kPi / 180.f : 0.5f * kPi;
            first = std::min(first, fallTime(lowest(em.speed) * std::cos(tilt), em.gravity.y, em.drag, c.height));
            last = std::max(last, fallTime(highest(em.speed), em.gravity.y, em.drag, c.height));
        }
        REQUIRE(last > first);
        // The marks: wet silhouettes laid flat on the floor, splatting in over 2 frames or less as the first ink lands
        // and while the rest does, held 0.8 s or more, then drying lighter as they go.
        float firstMark = 1e9f;
        for (const VfxEmitter& em : floor.emitters) {
            CAPTURE(em.name);
            CHECK(em.renderMode == VfxRenderMode::Horizontal);
            CHECK_FALSE(em.depthFade);
            CHECK((em.sprite == VfxSprite::Blot || em.sprite == VfxSprite::Splash));
            CHECK(highest(em.speed) == 0.f);
            CHECK(em.gravity.y == 0.f);
            CHECK(rampInAt(em.colorOverLife) * highest(em.lifetime) <= 2.f * kFrame);
            CHECK(fadeFrom(em.colorOverLife) * lowest(em.lifetime) >= 0.8f);
            REQUIRE(em.colorOverLife.stops.size() >= 2);
            CHECK(em.colorOverLife.stops.back().color.r > 2.f * em.colorOverLife.stops.front().color.r);
            for (const VfxBurst& b : em.bursts) {
                CHECK(b.time >= first - kFrame);
                CHECK(b.time <= last);
                firstMark = std::min(firstMark, b.time);
            }
        }
        CHECK(firstMark <= first + 2.f * kFrame);
    }
}

TEST_CASE("vfx: the Unbending's effects load without warnings and keep their particle budgets") {
    struct Expect {
        const char* file;
        std::size_t emitters;
        std::uint32_t budget; // the sum of the emitters' maxParticles
        bool loops;
    };
    const Expect files[] = {{"stone_tell", 4, 68, true},
                            {"stone_tell_area", 4, 120, true},
                            {"stone_impact", 4, 92, false},
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
            CHECK(em.loop == f.loops);
        }
        CHECK(total <= f.budget);
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

namespace {

// An emitter's screen cover: the particles it makes per cycle (the bursts' counts, or maxParticles when it emits by rate)
// times the square of the largest size they reach (startSize's top times sizeOverLife's peak, metres).
float coverOf(const VfxEmitter& e) {
    float n = 0.f;
    for (const VfxBurst& b : e.bursts) n += static_cast<float>(b.count);
    if (n == 0.f) n = static_cast<float>(e.maxParticles);
    float lo = 0.f, startHi = 0.f, growHi = 0.f;
    e.startSize.bounds(lo, startHi);
    e.sizeOverLife.bounds(lo, growHi);
    const float size = startHi * growHi;
    return n * size * size;
}

// Round 5's numbers by hand: n particles at `size` metres, growing to `grow` times that.
constexpr float cover(float n, float size, float grow = 1.f) { return n * size * grow * size * grow; }

} // namespace

TEST_CASE("vfx: the Unbending's chips and impact keep a screen-cover budget") {
    // The gameplay camera sits behind the lead, so what a hit throws off lands between the camera and the boss. Each
    // effect's cover is summed over its alpha-blended emitters, solid bits (soft sprites: chips, flakes, chunks) and
    // clouds (puffs: dust, slam, plume) apart so a cloud can't hide a chip count. Additive flashes don't count.
    // Round 5 (40 chips at up to 0.36 m, 16 flakes at 0.26 m; chips dust 18 at 1.1 m growing 1.7x; impact dust 32 at
    // 1.4 m growing 2.2x) came to 6.3 solid and 63 cloud for the chips, 460 cloud for the impact; now 1.6, 25 and 260.
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
            (em.sprite == VfxSprite::Puff ? cloud : solid) += coverOf(em);
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
    CHECK(contains(importError(R"J({"name": "x", "emitters": [{"name": "a", "sprite": "star"}]})J"),
                   "ring, chip, blot, splash"));
}

TEST_CASE("vfx: the blot and splash sprites reach the plan") {
    VfxEffect e;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE(importVfxEffect(R"J({"name": "s", "emitters": [
                                  {"name": "bits", "sprite": "blot"},
                                  {"name": "wet", "sprite": "splash", "renderMode": "horizontal"}]})J",
                            e, &err, &warnings));
    CHECK(warnings.empty());
    REQUIRE(e.emitters.size() == 2);
    CHECK(e.emitters[0].sprite == VfxSprite::Blot);
    CHECK(e.emitters[1].sprite == VfxSprite::Splash);
    VfxNiagaraPlan plan;
    planVfxNiagara(e, plan);
    REQUIRE(plan.emitters.size() == 2);
    CHECK(plan.emitters[0].sprite == VfxSprite::Blot);
    CHECK(plan.emitters[1].sprite == VfxSprite::Splash);
    CHECK(plan.emitters[1].facing == VfxNiagaraFacing::Horizontal);
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
