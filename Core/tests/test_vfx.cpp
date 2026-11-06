#include "ftcore/Vfx.h"
#include "ftcore/ArenaKindCrit.h" // white_glove's seam window, which dealer_seam_open covers
#include "ftcore/ClipShowcase.h" // the lone tell's length, which dealer_deal_tell spans

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
                            {"dealer_deal_tell", 5}, {"dealer_seam_open", 4}, {"dealer_seam_crit", 5}, {"ink_hit", 7},
                            {"ink_hit_floor", 3}, {"dealer_seam_crit_floor", 3}};
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
    const Expect files[] = {{"dealer_deal_tell", 5, 21, false},
                            {"dealer_seam_open", 4, 32, false},
                            {"dealer_seam_crit", 5, 38, false},
                            {"ink_hit", 7, 43, false},
                            {"ink_hit_floor", 3, 22, false},
                            {"dealer_seam_crit_floor", 3, 22, false}};
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

namespace {

// The alpha an emitter's colour reaches at x (0..1 of a particle's life).
float alphaAt(const VfxEmitter& em, float x) { return sampleVfxStops(em.colorOverLife.stops, x).a; }

// The shortest rise of alpha between two colour stops, in seconds (the ramp-in and every pulse after it).
float shortestRise(const VfxEmitter& em) {
    float best = 1e9f;
    const std::vector<VfxColorStop>& s = em.colorOverLife.stops;
    for (std::size_t i = 1; i < s.size(); ++i)
        if (s[i].color.a > s[i - 1].color.a + 1e-4f) best = std::min(best, (s[i].t - s[i - 1].t) * lowest(em.lifetime));
    return best;
}

// Where (0..1 of a particle's life) the last fade to nothing starts: walking back from the end, the first stop the
// alpha does not fall from (a hold or a rise before it).
float fadeOutStart(const VfxEmitter& em) {
    const std::vector<VfxColorStop>& s = em.colorOverLife.stops;
    std::size_t i = s.size() - 1;
    while (i > 0 && s[i - 1].color.a > s[i].color.a) --i;
    return s[i].t;
}

// Seconds until a particle thrown straight up at vy m/s stops rising, under gravity gy (negative) and drag k.
float apexTime(float vy, float gy, float k) {
    return k > 0.f ? std::log(1.f + vy * k / -gy) / k : vy / -gy;
}

bool isDebris(const VfxEmitter& em) { return highest(em.speed) >= 0.5f; }

// The main mass of the InkMass sheets, as a fraction of the quad's edge: the d80 (the diameter round the alpha
// centroid that holds 80% of the sprite's alpha), measured on the atlas cells. The sheets' bounding boxes fill 0.87-0.94
// of the quad, but their tendrils and flung drops carry little of the alpha, so the bbox overstates the mass.
constexpr float kCoreMassLo = 0.58f; // the "core" sheets: 0.60 and 0.58
constexpr float kCoreMassHi = 0.60f;
constexpr float kSplashMassMean = 0.49f; // the "splash" sheets: 0.48, 0.55 and 0.43

} // namespace

TEST_CASE("vfx: the Dealer's round-8f looks: a red tell that grows and pulses on the hand, a hot red seam that beats on "
          "the fingertips, a 0.9-1.0 m ink mass, liquid debris thrown clear, a red crit") {
    constexpr float kFrame = 1.f / 30.f; // the capture's frame
    // The open seam covers white_glove's whole window. Every layer is an atlas sprite (no procedural glow): wet red
    // smears splayed from the fingertips with red cut lines, red-hot seam light drawn over them, and dark drops falling
    // in the world. The smears, the cuts and the light come on the 0.32 s beat and overlap to the window's end; the
    // read is 0.3-0.4 m across, every quad under 0.45 m.
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Battle/arena_crit.json", text));
    ArenaKindCritTable table;
    REQUIRE(importArenaKindCrits(text, table));
    const ArenaKindCritDef* glove = table.find("white_glove");
    REQUIRE(glove);
    const float opens = static_cast<float>(glove->opens);
    VfxEffect seam;
    REQUIRE(loadEffect("dealer_seam_open", seam));
    CHECK_FALSE(findEmitter(seam, "glow"));
    const VfxEmitter* wet = findEmitter(seam, "wet");
    const VfxEmitter* cuts = findEmitter(seam, "cuts");
    const VfxEmitter* light = findEmitter(seam, "seam");
    const VfxEmitter* drips = findEmitter(seam, "drips");
    REQUIRE(wet);
    REQUIRE(cuts);
    REQUIRE(light);
    REQUIRE(drips);
    for (const VfxEmitter& em : seam.emitters) {
        CAPTURE(em.name);
        CHECK(em.duration == doctest::Approx(opens));
        CHECK_FALSE(em.atlas.set.empty());
        // Never a ball bigger than the hand: no layer's quad passes 0.45 m.
        CHECK(peakSize(em) <= 0.45f);
    }
    for (const VfxEmitter* em : {wet, cuts, light}) {
        CAPTURE(em->name);
        CHECK(em->space == VfxSpace::Local); // on the fingertips through the swing
        REQUIRE(em->bursts.size() >= 4); // the pulse
        for (std::size_t i = 0; i < em->bursts.size(); ++i)
            CHECK(em->bursts[i].time == doctest::Approx(0.32f * static_cast<float>(i)));
        for (std::size_t i = 1; i < em->bursts.size(); ++i)
            CHECK(em->bursts[i].time - em->bursts[i - 1].time < lowest(em->lifetime));
        CHECK(em->bursts.back().time + lowest(em->lifetime) >= opens);
    }
    CHECK(wet->atlas.set == "RedTell");
    CHECK(wet->atlas.group == "flush");
    CHECK(cuts->atlas.set == "RedTell");
    CHECK(cuts->atlas.group == "line");
    CHECK(light->atlas.set == "SeamLight");
    CHECK(light->atlas.group == "seam");
    CHECK(light->blend == VfxBlend::Additive);
    CHECK(wet < light); // the wet red is drawn first, under the light
    CHECK(cuts < light);
    // The read: the wet smears are 0.35 m or more, several a pulse, fanned round the fingers (not one blob).
    CHECK(lowest(wet->startSize) >= 0.35f);
    CHECK(wet->bursts.front().count >= 3);
    CHECK(highest(wet->startRotation) - lowest(wet->startRotation) >= 1.5f);
    // The light beats: on each pulse it flares to full within 0.12 s, then falls to 0.2 or less before the next pulse,
    // so the 0.32 s beat shows as a rise and a trough. Hot red: brighter than the sprites' own red (HDR).
    {
        const float life = lowest(light->lifetime);
        CHECK(peakAlpha(light->colorOverLife) >= 0.9f);
        CHECK(rampInAt(light->colorOverLife) * life <= 0.12f);
        CHECK(alphaAt(*light, 0.31f / life) <= 0.2f);
        CHECK(highest(light->lifetime) <= 0.45f); // gone soon after the next pulse lands
        for (const VfxColorStop& st : light->colorOverLife.stops) CHECK(st.color.r >= 3.f);
    }
    // The drips fall in the world: dark red drops big enough to show at 60 px, about 14 a second at most.
    CHECK(drips->space == VfxSpace::World);
    CHECK(drips->gravity.y <= -9.f);
    CHECK(drips->atlas.set == "RedTell");
    CHECK(drips->atlas.group == "drop");
    CHECK(lowest(drips->startSize) >= 0.12f);
    CHECK(drips->rate <= 14.f);

    // The tell: wet red on the hand for the whole wind-up (the Deal's 2.3 s startup and 0.2 s active; the lone tell's
    // 2.4 s). A red flush, red threads along the fingers and red beads ride the hand bone (local space: no lag through
    // the whip) and grow with the wind-up, pulsing on each of its four beats; drops fall from the hand in the world on
    // every beat. It is gone soon after the window opens, fading over 0.15 s or more, never cut.
    VfxEffect tell;
    REQUIRE(loadEffect("dealer_deal_tell", tell));
    const float windUp = static_cast<float>(ShowSpec{}.tellFor);
    constexpr float kBeat = 0.577f; // the wind-up's four beats
    const float beats[] = {0.f, kBeat, 2.f * kBeat, 3.f * kBeat};
    for (const VfxEmitter& em : tell.emitters) {
        CAPTURE(em.name);
        CHECK_FALSE(em.loop);
        CHECK(em.rate == 0.f);
        CHECK(em.blend == VfxBlend::Alpha);
        CHECK(em.atlas.set == "RedTell");
        for (const VfxBurst& b : em.bursts) CHECK(b.time + highest(em.lifetime) <= windUp + 0.2f);
        if (em.name == "drops") continue;
        CHECK(em.space == VfxSpace::Local);
        // The fade: it holds to the end of the wind-up, then goes over 0.15 s or more.
        const float fadeAt = fadeOutStart(em);
        CHECK(lowest(em.lifetime) * (1.f - fadeAt) >= 0.15f);
        for (const VfxBurst& b : em.bursts) CHECK(b.time + fadeAt * lowest(em.lifetime) >= windUp - 0.05f);
    }
    const VfxEmitter* flush = findEmitter(tell, "flush");
    const VfxEmitter* threads = findEmitter(tell, "threads");
    const VfxEmitter* beads = findEmitter(tell, "beads");
    const VfxEmitter* drops = findEmitter(tell, "drops");
    REQUIRE(flush);
    REQUIRE(threads);
    REQUIRE(beads);
    REQUIRE(drops);
    CHECK(flush->atlas.group == "flush");
    CHECK(threads->atlas.group == "line");
    CHECK(beads->atlas.group == "drop");
    CHECK(lowest(flush->lifetime) >= windUp);
    // One wash along the fingers, not a ragged clump: three smears at most, all turned within 0.2 rad of the hanging
    // fingers' line, and fewer drops (twelve at most).
    CHECK(flush->maxParticles <= 3);
    CHECK(lowest(flush->startRotation) >= 0.5f * kPi - 0.2f);
    CHECK(highest(flush->startRotation) <= 0.5f * kPi + 0.2f);
    CHECK(beads->maxParticles <= 2);
    CHECK(drops->maxParticles <= 12);
    // Grows: the flush's quad doubles over the wind-up to 0.7 m or more at its end (the red wedge reaches past 0.5 m).
    const float endX = windUp / highest(flush->lifetime);
    CHECK(flush->sizeOverLife.evaluate(endX, 0.5f) >= 1.8f * flush->sizeOverLife.evaluate(0.f, 0.5f));
    CHECK(lowest(flush->startSize) * flush->sizeOverLife.evaluate(endX, 0.5f) >= 0.7f);
    // A pulse on each beat: the flush and the beads brighten and step up in size in the 0.11 s after each beat.
    for (const VfxEmitter* em : {flush, beads}) {
        CAPTURE(em->name);
        const float life = lowest(em->lifetime);
        CHECK(alphaAt(*em, 0.11f / life) >= 0.7f);
        for (int k = 1; k < 4; ++k) {
            CAPTURE(k);
            const float at = beats[k] / life;
            const float after = (beats[k] + 0.11f) / life;
            CHECK(alphaAt(*em, after) >= alphaAt(*em, at) + 0.08f);
            CHECK(em->sizeOverLife.evaluate(after, 0.5f) >= 1.1f * em->sizeOverLife.evaluate(at, 0.5f));
        }
    }
    // Drops on every beat, falling in the world as stretched red drops.
    CHECK(drops->space == VfxSpace::World);
    CHECK(drops->gravity.y <= -9.f);
    CHECK(drops->atlas.group == "drop");
    CHECK(drops->renderMode == VfxRenderMode::Stretched);
    CHECK(drops->lengthScale == 1.f);
    CHECK(drops->speedScale <= 0.05f);
    CHECK(lowest(drops->startSize) >= 0.12f);
    for (int k = 0; k < 4; ++k) {
        CAPTURE(k);
        int n = 0;
        for (const VfxBurst& b : drops->bursts)
            if (b.time >= beats[k] && b.time < beats[k] + 0.35f) n += static_cast<int>(b.count);
        CHECK(n >= k + 1); // more each beat
    }

    // Everything on the hand ramps in, and every pulse rises, over 3 frames or more.
    std::vector<const VfxEmitter*> ramped;
    for (const VfxEmitter& em : seam.emitters) ramped.push_back(&em);
    for (const VfxEmitter& em : tell.emitters) ramped.push_back(&em);
    for (const VfxEmitter* em : ramped) {
        CAPTURE(em->name);
        REQUIRE_FALSE(em->colorOverLife.stops.empty());
        CHECK(em->colorOverLife.stops.front().t == 0.f);
        CHECK(em->colorOverLife.stops.front().color.a == 0.f);
        CHECK(shortestRise(*em) >= 3.f * kFrame);
    }
    // Real red. The sprites are red under a neutral tint; the one additive layer is the seam light, red-leaning and
    // drawn over the wet red, so it neither clips to pink on the pale glove nor greys out on sand.
    for (const VfxEffect* fx : {&tell, &seam}) {
        for (const VfxEmitter& em : fx->emitters) {
            CAPTURE(fx->name);
            CAPTURE(em.name);
            if (em.blend == VfxBlend::Additive) CHECK(&em == light);
            for (const VfxColorStop& s : em.colorOverLife.stops) {
                if (em.blend == VfxBlend::Additive)
                    CHECK(std::max(s.color.g, s.color.b) <= 0.2f * s.color.r);
                else
                    CHECK(std::max(s.color.g, s.color.b) <= s.color.r);
            }
        }
    }

    // The plain hit: one of the two dense "core" sheets, opening in 3 frames or less to a main mass 0.9-1.0 m across
    // (the d80 above, not the bbox), translucent, its heavy mass (alpha 0.5 or more) gone within 6 frames, then thinning
    // as its edges erode, never dropping out in a frame. A lighter, wider wet edge round it (under 0.5 alpha: not part
    // of the mass), and a wider ink haze that takes over before the sheet is gone.
    VfxEffect ink;
    VfxEffect crit;
    REQUIRE(loadEffect("ink_hit", ink));
    REQUIRE(loadEffect("dealer_seam_crit", crit));
    const VfxEmitter* core = findEmitter(ink, "core");
    const VfxEmitter* edge = findEmitter(ink, "edge");
    const VfxEmitter* cloud = findEmitter(ink, "cloud");
    REQUIRE(core);
    REQUIRE(edge);
    REQUIRE(cloud);
    CHECK(core->atlas.set == "InkMass");
    CHECK(core->atlas.group == "core");
    CHECK(core->atlas.erode);
    CHECK(lowest(core->startSize) * kCoreMassLo >= 0.9f); // at full size (1.0), the smallest sheet's mass
    CHECK(peakSize(*core) * kCoreMassHi <= 1.f);           // at the size curve's top, the largest sheet's mass
    CHECK(core->sizeOverLife.evaluate(0.f, 0.5f) >= 0.5f); // opens from half size or more...
    {
        float full = 1.f;
        for (int i = 100; i >= 0; --i)
            if (core->sizeOverLife.evaluate(static_cast<float>(i) / 100.f, 0.5f) >= 0.999f)
                full = static_cast<float>(i) / 100.f;
        CHECK(full * highest(core->lifetime) <= 3.f * kFrame); // ...to full in 3 frames or less
    }
    CHECK(peakAlpha(core->colorOverLife) <= 0.85f);
    float heavy = 0.f;
    for (int i = 0; i <= 100; ++i) {
        const float x = static_cast<float>(i) / 100.f;
        if (alphaAt(*core, x) >= 0.5f) heavy = x * highest(core->lifetime);
    }
    CHECK(heavy <= 6.f * kFrame);
    CHECK(fadeFrom(core->colorOverLife) * lowest(core->lifetime) >= 3.f * kFrame);
    const std::vector<VfxColorStop>& stops = core->colorOverLife.stops;
    for (std::size_t i = 1; i < stops.size(); ++i) {
        CAPTURE(i);
        const float fall = stops[i - 1].color.a - stops[i].color.a;
        const float span = (stops[i].t - stops[i - 1].t) * lowest(core->lifetime);
        CHECK(fall <= span / (4.f * kFrame)); // never faster than full strength to nothing in 4 frames
    }
    CHECK(edge->atlas.set == "InkMass");
    CHECK(edge->atlas.group == "splash");
    CHECK(lowest(edge->startSize) > highest(core->startSize));
    CHECK(peakAlpha(edge->colorOverLife) <= 0.6f * peakAlpha(core->colorOverLife));
    CHECK(peakAlpha(edge->colorOverLife) < 0.5f);
    CHECK(highest(edge->lifetime) < lowest(core->lifetime));
    CHECK(cloud->atlas.set == "InkMass");
    CHECK(cloud->atlas.group == "cloud");
    CHECK(peakAlpha(cloud->colorOverLife) <= 0.6f);
    CHECK(lowest(cloud->startSize) > highest(edge->startSize)); // the haze spreads wider than the mass and the edge
    REQUIRE_FALSE(cloud->bursts.empty());
    CHECK(cloud->bursts.front().time < lowest(core->lifetime) * fadeFrom(core->colorOverLife) + 3.f * kFrame);

    // The crit reads as a crit at 60 px, not as the plain hit: a hot red/white snap and a red ring on the hit frame and
    // for about 0.3 s after, and red-tinted ink in the burst, the chunks and the streaks. No soft flash and no
    // procedural pop: the snap is the one additive layer, an irregular SeamLight sprite drawn over all.
    CHECK_FALSE(findEmitter(crit, "pop"));
    CHECK_FALSE(findEmitter(crit, "flash"));
    int additive = 0;
    for (const VfxEmitter& em : crit.emitters) {
        CAPTURE(em.name);
        CHECK_FALSE(em.atlas.set.empty());
        if (em.blend != VfxBlend::Additive) continue;
        ++additive;
        CHECK(em.atlas.set == "SeamLight");
        CHECK(em.atlas.group == "snap");
        CHECK(&em == &crit.emitters.back());
        // On the hit frame and for about 0.3 s after: every burst inside the first 0.1 s, the last gone by 0.35 s.
        REQUIRE_FALSE(em.bursts.empty());
        CHECK(em.bursts.front().time == 0.f);
        CHECK(em.bursts.back().time <= 0.1f);
        CHECK(em.bursts.back().time + lowest(em.lifetime) >= 0.28f);
        CHECK(em.bursts.back().time + highest(em.lifetime) <= 0.35f);
        // White-hot as it strikes, cooling to red: red leads throughout.
        REQUIRE(em.colorOverLife.stops.size() >= 2);
        const VfxColor& hot = em.colorOverLife.stops.front().color;
        const VfxColor& cool = em.colorOverLife.stops.back().color;
        CHECK(hot.a >= 0.9f);
        CHECK(std::min(hot.g, hot.b) >= 0.5f * hot.r);
        CHECK(std::max(cool.g, cool.b) <= 0.4f * cool.r);
        for (const VfxColorStop& s : em.colorOverLife.stops) CHECK(s.color.r >= std::max(s.color.g, s.color.b));
    }
    CHECK(additive == 1);
    const VfxEmitter* ring = findEmitter(crit, "redring");
    const VfxEmitter* burst = findEmitter(crit, "ink");
    const VfxEmitter* chunks = findEmitter(crit, "chunks");
    REQUIRE(ring);
    REQUIRE(burst);
    REQUIRE(chunks);
    CHECK(burst->atlas.group == "splash");
    CHECK(lowest(burst->startSize) > highest(core->startSize));
    // Bigger than the plain hit's mass: the burst's mean sheet at its peak against the hit's largest.
    CHECK(lowest(burst->startSize) * highest(burst->sizeOverLife) * kSplashMassMean > peakSize(*core) * kCoreMassHi);
    // Red-tinted ink (the sheets are near-black under a grey sheen, so the tint is an HDR red lift).
    for (const VfxColorStop& s : burst->colorOverLife.stops) {
        CHECK(s.color.r >= 2.f);
        CHECK(s.color.r >= 4.f * std::max(s.color.g, s.color.b));
    }
    // The ring: red, on the hit frame and for about 0.3 s, gone before the fastest chunk thrown straight up starts
    // to fall. Only its edge (eroding) and its fade-out are soft.
    CHECK(ring->atlas.set == "InkMass");
    CHECK(ring->atlas.group == "redring");
    CHECK(ring->blend == VfxBlend::Alpha);
    CHECK(ring->atlas.erode);
    CHECK(peakAlpha(ring->colorOverLife) <= 0.75f);
    REQUIRE_FALSE(ring->colorOverLife.stops.empty());
    CHECK(ring->colorOverLife.stops.front().color.a >= 0.5f); // there on the hit frame
    CHECK(lowest(ring->lifetime) >= 0.28f);
    for (const VfxColorStop& s : ring->colorOverLife.stops) {
        CHECK(s.color.r >= 3.f); // hot: the ring sprites' own red, lifted
        CHECK(s.color.r >= 2.f * std::max(s.color.g, s.color.b));
    }
    CHECK(highest(ring->lifetime) <= apexTime(highest(chunks->speed), chunks->gravity.y, chunks->drag));
    float critLife = 0.f;
    for (const VfxEmitter& em : crit.emitters) critLife = std::max(critLife, lowest(em.lifetime));
    CHECK(critLife >= 8.f * kFrame);

    // Debris: liquid ink thrown clear of the body, falling under full gravity, with real variety. The hit throws blots
    // from all eleven blot shapes and drops from all five drop shapes; each emitter's sizes span 2x or more and its
    // speeds 1.8x or more. Drops and streaks laid along their velocity keep the sprite's own shape (lengthScale 1, no
    // speed stretch on top) and outnumber the round blots; round sprites turn slowly both ways from any start (liquid,
    // not tumbling chips). The blots leave at 1.8 m/s or more under light drag, so they clear the body before they fall.
    // The hit's ink is a matte grey-black tint (0.7 or less, no sheen lift); the crit's is red.
    for (const VfxEffect* fx : {&ink, &crit}) {
        bool blots = false;
        bool dropShapes = false;
        std::uint32_t laid = 0;
        std::uint32_t roundN = 0;
        for (const VfxEmitter& em : fx->emitters) {
            if (!isDebris(em)) continue; // the sheets, the cloud, the ring, the snap
            CAPTURE(fx->name);
            CAPTURE(em.name);
            CHECK(em.gravity.y <= -9.f);
            CHECK((em.atlas.set == "InkBlot" || em.atlas.set == "InkStreak"));
            CHECK(highest(em.speed) >= 1.8f * lowest(em.speed));
            blots = blots || em.atlas.group == "blot";
            dropShapes = dropShapes || (em.atlas.set == "InkBlot" && em.atlas.group == "drop");
            if (em.renderMode == VfxRenderMode::Stretched) {
                CHECK(em.lengthScale == 1.f);
                CHECK(em.speedScale <= 0.05f);
                CHECK(lowest(em.startSize) >= (em.atlas.set == "InkStreak" ? 0.4f : 0.1f));
                laid += em.maxParticles;
            } else {
                CHECK(em.atlas.set == "InkBlot");
                CHECK(lowest(em.angularVelocity) <= -3.f);
                CHECK(highest(em.angularVelocity) >= 3.f);
                CHECK(highest(em.angularVelocity) <= 4.f);
                CHECK(highest(em.startRotation) > 3.f);
                CHECK(lowest(em.startSize) >= 0.06f);
                roundN += em.maxParticles;
            }
            if (em.atlas.set == "InkBlot") {
                CHECK(highest(em.startSize) >= 2.f * lowest(em.startSize));
                CHECK(lowest(em.speed) >= 1.8f);
                CHECK(em.drag <= 1.f);
            }
            for (const VfxColorStop& s : em.colorOverLife.stops) {
                if (fx == &ink) {
                    CHECK(s.color.r == s.color.g);
                    CHECK(s.color.g == s.color.b);
                    CHECK(s.color.r <= 0.7f);
                } else {
                    CHECK(s.color.r >= 2.f);
                    CHECK(s.color.r >= 4.f * std::max(s.color.g, s.color.b));
                }
            }
        }
        CHECK(blots);
        CHECK(laid > roundN); // drops and streaks with pinched tails outnumber the round blots
        if (fx == &ink) CHECK(dropShapes);
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
        // and while the rest does, held 0.8 s or more, then drying lighter as they go. A pool about 1.2 m across (wider
        // than the contact shadow) with splats of 0.4 m or more round it, and fine spatter on a ring at its rim.
        float firstMark = 1e9f;
        float pool = 0.f;
        bool rim = false;
        for (const VfxEmitter& em : floor.emitters) {
            CAPTURE(em.name);
            CHECK(em.renderMode == VfxRenderMode::Horizontal);
            CHECK_FALSE(em.depthFade);
            CHECK(em.atlas.set == "InkFloor");
            CHECK_FALSE(em.atlas.erode); // flat marks fade, they do not eat away
            if (highest(em.startSize) <= 0.35f) {
                // small marks only as the rim spatter: on a ring outside the pool's middle, several of them
                rim = true;
                CHECK(em.shape == VfxShape::Ring);
                CHECK(em.innerRadius >= 0.4f);
                CHECK(em.maxParticles >= 6);
            } else {
                CHECK(lowest(em.startSize) * 0.84f >= 0.4f); // most marks' long side is 0.84-0.90 of the quad: 0.4 m reads at 60 px
            }
            pool = std::max(pool, lowest(em.startSize) * 0.84f);
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
        CHECK(pool >= 1.3f); // wide enough to read round the feet at 60 px
        CHECK(rim);
        // The crit's marks carry its red, wet and drying; the plain hit's are dark (1 or less) while wet.
        const bool red = std::string(c.floor) == "dealer_seam_crit_floor";
        for (const VfxEmitter& em : floor.emitters) {
            CAPTURE(em.name);
            for (const VfxColorStop& s : em.colorOverLife.stops) {
                if (red)
                    CHECK(s.color.r >= 3.f * std::max(s.color.g, s.color.b));
                else if (s.color.a > 0.9f)
                    CHECK(s.color.r <= 1.f);
            }
        }
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

// ---- sprite atlases ---------------------------------------------------------------------------------------

namespace {

// The error lines of a failed atlas import, or "" when it succeeded.
std::string atlasError(const char* json) {
    VfxAtlasTable t;
    std::string err;
    if (importVfxAtlases(json, t, &err)) return "";
    CHECK(t.sets.empty());
    return err;
}

bool loadAtlases(VfxAtlasTable& out, std::vector<std::string>* warnings = nullptr) {
    std::string text, err;
    if (!ftcore_test::readProjectData("Assets/vfx_atlases.json", text)) {
        FAIL("cannot read Data/Assets/vfx_atlases.json");
        return false;
    }
    const bool ok = importVfxAtlases(text, out, &err, warnings);
    CHECK_MESSAGE(ok, err);
    return ok;
}

} // namespace

TEST_CASE("vfx: the project's atlas table loads without warnings") {
    VfxAtlasTable t;
    std::vector<std::string> warnings;
    REQUIRE(loadAtlases(t, &warnings));
    CHECK_MESSAGE(warnings.empty(), (warnings.empty() ? std::string() : warnings.front()));
    REQUIRE(t.sets.size() == 6);
    const VfxAtlasDef* mass = findVfxAtlas(t, "InkMass");
    REQUIRE(mass);
    CHECK(mass->gridX == 4);
    CHECK(mass->blend == VfxBlend::Alpha);
    const VfxAtlasGroup* cloud = findVfxAtlasGroup(*mass, "cloud");
    REQUIRE(cloud);
    CHECK(cloud->start == 6);
    CHECK(cloud->count == 6);
    const VfxAtlasDef* light = findVfxAtlas(t, "SeamLight");
    REQUIRE(light);
    CHECK(light->blend == VfxBlend::Additive);
    CHECK_FALSE(findVfxAtlas(t, "inkmass")); // names are case-sensitive, like the asset names
    // Every group of every set has at least two sprites, so no emitter repeats a single one.
    for (const VfxAtlasDef& d : t.sets)
        for (const VfxAtlasGroup& g : d.groups) {
            CAPTURE(d.name);
            CAPTURE(g.name);
            CHECK(g.count >= 2);
        }
}

TEST_CASE("vfx: bad atlas tables are rejected with the path of each problem") {
    CHECK(atlasError(R"J({"atlases": [{"name": "A", "grid": [4, 4], "cell": [64, 64], "blend": "alpha",
                          "groups": {"a": [0, 4], "b": [4, 2]}}]})J") == "");
    CHECK(atlasError(R"J({"atlases": []})J") == "atlases: expected at least one atlas");
    CHECK(atlasError(R"J({"atlases": [{"name": "A", "grid": [4, 4], "cell": [64, 64], "blend": "alpha",
                          "groups": {"a": [0, 4], "b": [3, 2]}}]})J") == "atlases[0].groups.b: overlaps group \"a\"");
    CHECK(atlasError(R"J({"atlases": [{"name": "A", "grid": [4, 2], "cell": [64, 64], "blend": "alpha",
                          "groups": {"a": [6, 3]}}]})J") == "atlases[0].groups.a: cells 6..8 run past the grid's 8");
    CHECK(atlasError(R"J({"atlases": [{"name": "A", "grid": [4, 4], "cell": [64, 64], "blend": "alpha",
                          "groups": {"a": [0, 0]}}]})J") == "atlases[0].groups.a: a group needs at least one cell");
    CHECK(atlasError(R"J({"atlases": [{"name": "A", "grid": [3, 4], "cell": [64, 64], "blend": "alpha",
                          "groups": {"a": [0, 2]}}]})J") ==
          "atlases[0].cell: the atlas (grid x cell) must be a power of two on each side, up to 8192, so it mips");
    CHECK(atlasError(R"J({"atlases": [{"name": "A", "grid": [0, 4], "cell": [64, 64], "blend": "alpha",
                          "groups": {"a": [0, 2]}}]})J") == "atlases[0].grid: expected 1 to 16 cells a side");
    CHECK(atlasError(R"J({"atlases": [{"name": "Ink Mass", "grid": [4, 4], "cell": [64, 64], "blend": "alpha",
                          "groups": {"a": [0, 2]}}]})J") == "atlases[0].name: expected letters, digits and _");
    CHECK(atlasError(R"J({"atlases": [{"name": "A", "grid": [4, 4], "cell": [64, 64], "blend": "glow",
                          "groups": {"a": [0, 2]}}]})J") == "atlases[0].blend: unknown value \"glow\" (alpha, additive)");
    CHECK(atlasError(R"J({"atlases": [{"name": "A", "grid": [4, 4], "cell": [64, 64], "blend": "alpha",
                          "groups": {}}]})J") == "atlases[0].groups: expected at least one group");
    CHECK(atlasError(R"J({"atlases": [{"name": "A", "grid": [4, 4], "cell": [64, 64], "blend": "alpha", "groups": {"a": [0, 2]}},
                                      {"name": "A", "grid": [4, 4], "cell": [64, 64], "blend": "alpha", "groups": {"a": [0, 2]}}]})J") ==
          "atlases[1].name: a second atlas called \"A\"");
    CHECK(contains(atlasError(R"J({"atlases": [{"name": "A", "grid": [4, 4], "cell": [64, 64],
                                   "groups": {"a": [0, 2]}}]})J"), "atlases[0].blend: missing"));

    VfxAtlasTable t;
    std::vector<std::string> warnings;
    REQUIRE(importVfxAtlases(R"J({"atlases": [{"name": "A", "grid": [4, 4], "cell": [64, 64], "blend": "alpha",
                                  "groups": {"a": [0, 2]}, "note": "x"}]})J", t, nullptr, &warnings));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0] == "atlases[0].note: unknown key (ignored)");
}

TEST_CASE("vfx: an emitter's atlas reference parses and resolves to grid numbers") {
    VfxAtlasTable t;
    REQUIRE(loadAtlases(t));

    VfxEffect e;
    std::string err;
    REQUIRE(importVfxEffect(R"J({"name": "x", "emitters": [
        {"name": "core", "bursts": [{"count": 1}], "atlas": {"set": "InkMass", "group": "cloud"}},
        {"name": "streak", "bursts": [{"count": 4}], "atlas": {"set": "InkStreak", "group": "streak", "erode": false}},
        {"name": "glow", "bursts": [{"count": 1}], "blend": "additive", "atlas": {"set": "SeamLight", "group": "seam"}},
        {"name": "plain", "bursts": [{"count": 1}], "sprite": "spark"}]})J",
                            e, &err));
    CHECK(e.emitters[0].atlas.set == "InkMass");
    CHECK(e.emitters[0].atlas.erode);
    CHECK_FALSE(e.emitters[1].atlas.erode);

    VfxNiagaraPlan plan;
    planVfxNiagara(e, t, plan);
    const VfxNiagaraAtlas& core = plan.emitters[0].atlas;
    CHECK(core.resolved);
    CHECK(core.gridX == 4);
    CHECK(core.gridY == 4);
    CHECK(core.cellStart == 6);
    CHECK(core.cellCount == 6);
    CHECK(core.cellAspect == doctest::Approx(1.f));
    const VfxNiagaraAtlas& streak = plan.emitters[1].atlas;
    CHECK(streak.resolved);
    CHECK_FALSE(streak.erode);
    CHECK(streak.gridX == 8);
    CHECK(streak.gridY == 2);
    CHECK(streak.cellCount == 10);
    CHECK(streak.cellAspect == doctest::Approx(0.25f));
    CHECK(plan.emitters[2].atlas.resolved);
    CHECK(plan.emitters[2].atlas.cellAspect == doctest::Approx(0.5f));
    CHECK(plan.emitters[3].atlas.set.empty());
    CHECK_FALSE(plan.emitters[3].atlas.resolved);
    for (const VfxNiagaraEmitter& p : plan.emitters) CHECK_FALSE(anyGap(p, "atlas"));

    // Without the table every reference stays unresolved, with a gap.
    planVfxNiagara(e, plan);
    CHECK_FALSE(plan.emitters[0].atlas.resolved);
    CHECK(anyGap(plan.emitters[0], "no atlas table"));
}

TEST_CASE("vfx: an atlas reference that does not resolve is a gap, never a silent fallback") {
    VfxAtlasTable t;
    REQUIRE(loadAtlases(t));
    VfxEffect e;
    std::string err;
    REQUIRE(importVfxEffect(R"J({"name": "x", "emitters": [
        {"name": "a", "bursts": [{"count": 1}], "atlas": {"set": "InkMud", "group": "splash"}},
        {"name": "b", "bursts": [{"count": 1}], "atlas": {"set": "InkMass", "group": "spray"}},
        {"name": "c", "bursts": [{"count": 1}], "atlas": {"set": "SeamLight", "group": "seam"}},
        {"name": "d", "bursts": [{"count": 1}], "blend": "additive", "atlas": {"set": "InkBlot", "group": "blot"}}]})J",
                            e, &err));
    VfxNiagaraPlan plan;
    planVfxNiagara(e, t, plan);
    for (const VfxNiagaraEmitter& p : plan.emitters) {
        CAPTURE(p.name);
        CHECK_FALSE(p.atlas.resolved);
        CHECK(p.atlas.cellCount == 0);
    }
    CHECK(anyGap(plan.emitters[0], "atlas \"InkMud/splash\": unknown atlas set"));
    CHECK(anyGap(plan.emitters[1], "the set has no such group"));
    CHECK(anyGap(plan.emitters[2], "light sprites (an additive set) on an alpha emitter"));
    CHECK(anyGap(plan.emitters[3], "ink sprites (an alpha set) on an additive emitter"));

    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "atlas": {"set": "InkMass"}}]})J") ==
          "emitters[0].atlas.group: missing");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "atlas": {"set": "", "group": "splash"}}]})J") ==
          "emitters[0].atlas.set: empty");
    CHECK(importError(R"J({"name": "x", "emitters": [{"name": "a", "atlas": "InkMass"}]})J") ==
          "emitters[0].atlas: expected an object");
}

TEST_CASE("vfx: the Dealer's effects draw from the sprite atlases, and every reference resolves") {
    VfxAtlasTable t;
    REQUIRE(loadAtlases(t));
    const char* files[] = {"dealer_deal_tell", "dealer_seam_open", "dealer_seam_crit",
                           "ink_hit",          "ink_hit_floor",    "dealer_seam_crit_floor"};
    for (const char* f : files) {
        CAPTURE(f);
        VfxEffect e;
        REQUIRE(loadEffect(f, e));
        VfxNiagaraPlan plan;
        planVfxNiagara(e, t, plan);
        REQUIRE(plan.emitters.size() == e.emitters.size());
        for (std::size_t i = 0; i < e.emitters.size(); ++i) {
            const VfxEmitter& em = e.emitters[i];
            const VfxNiagaraEmitter& p = plan.emitters[i];
            CAPTURE(em.name);
            CHECK_FALSE(anyGap(p, "atlas"));
            // Every layer is an atlas sprite: no procedural glow, disc or soft primitive anywhere.
            CHECK_FALSE(em.atlas.set.empty());
            CHECK(p.atlas.resolved);
            CHECK(p.atlas.cellCount >= 2); // no emitter repeats one sprite
            if (em.atlas.group == "blot") CHECK(p.atlas.cellCount >= 11); // all eleven blot shapes
            if (em.atlas.set == "InkBlot" && em.atlas.group == "drop") CHECK(p.atlas.cellCount >= 5); // all five drops
        }
    }
}
