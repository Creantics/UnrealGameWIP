#include "ftcore/ClipShowcase.h"

#include "ftcore/ArenaFight.h"
#include "ftcore/BeatChoreo.h"
#include "ftcore/StoneBearer.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The -ArenaShowcase reel: the boss's clips one at a time, the effects alone, for the review captures.

namespace {

std::int16_t idOf(const ftcore::BeatClips& clips, const char* id) {
    return static_cast<std::int16_t>(clips.find(id));
}

bool loadClips(ftcore::BeatClips& clips) {
    std::string text, err;
    if (!ftcore_test::readProjectData("Anim/stone_bearer_clips.json", text)) return false;
    return ftcore::importBeatClips(text, clips, &err);
}

ftcore::ShowSpec stoneSpec(const ftcore::BeatClips& clips) {
    ftcore::ShowSpec s;
    s.idle = idOf(clips, "idle");
    s.idleTwo = idOf(clips, "idle_p2");
    s.walk = idOf(clips, "walk");
    s.turnL = idOf(clips, "turn_l");
    s.turnR = idOf(clips, "turn_r");
    s.stagger = idOf(clips, "stagger");
    s.downIn = idOf(clips, "face_down_in");
    s.down = idOf(clips, "face_down");
    s.downOut = idOf(clips, "face_down_out");
    s.defeat = idOf(clips, "defeat");
    s.defeatKneel = idOf(clips, "defeat_kneel");
    s.hitLight = idOf(clips, "hit_light");
    s.hitHeavy = idOf(clips, "hit_heavy");
    s.walkFrom = 0.833;
    const char* attacks[] = {"called_fist", "called_fist_l", "slab_drive", "edge_jab"};
    for (const char* id : attacks) {
        ftcore::ShowAttack& a = s.attacks[s.attackCount++];
        a.clip = idOf(clips, id);
        a.move = static_cast<std::int16_t>(s.attackCount - 1);
        a.left = std::string(id).size() > 2 && std::string(id).substr(std::string(id).size() - 2) == "_l";
        const double impact = clips[static_cast<std::size_t>(a.clip)].impact;
        a.entry = impact > 1.0 ? impact - 1.0 : 0.0; // a 1 s startup
        a.until = a.entry + 1.0 + 0.4 + 1.2;         // startup + active + recovery
    }
    return s;
}

const ftcore::ShowSegment* findSegment(const ftcore::ClipShow& show, const char* label, std::int16_t clip, std::size_t from = 0) {
    for (std::size_t i = from; i < show.segmentCount; ++i)
        if (show.segments[i].label() == label && (clip == ftcore::kNoClip || show.segments[i].clip == clip)) return &show.segments[i];
    return nullptr;
}

} // namespace

TEST_CASE("clip showcase: the reel's segments run end to end and its cues are in time order") {
    ftcore::BeatClips clips;
    REQUIRE(loadClips(clips));
    const ftcore::ShowSpec spec = stoneSpec(clips);
    ftcore::ClipShow show;
    std::string err;
    REQUIRE_MESSAGE(ftcore::buildClipShow(clips, spec, show, &err), err);
    REQUIRE(show.segmentCount > 10);
    double t = 0.0;
    for (std::size_t i = 0; i < show.segmentCount; ++i) {
        const ftcore::ShowSegment& s = show.segments[i];
        CHECK(s.start == doctest::Approx(t));
        CHECK(s.length > 0.0);
        t += s.length;
    }
    CHECK(show.length == doctest::Approx(t));
    for (std::size_t i = 1; i < show.cueCount; ++i) CHECK(show.cues[i].at >= show.cues[i - 1].at);
    // The idle seams show twice on the turntable's take, the second idle's and the hold's.
    const ftcore::ShowSegment& turntable = show.segments[0];
    CHECK(turntable.label() == "turntable");
    CHECK(turntable.cut);
    const double idle = clips[static_cast<std::size_t>(spec.idle)].length;
    CHECK(turntable.length + show.segments[1].length > 2.0 * idle);
    const ftcore::ShowSegment* two = findSegment(show, "idle_p2", spec.idleTwo);
    REQUIRE(two);
    CHECK(two->length > 2.0 * clips[static_cast<std::size_t>(spec.idleTwo)].length);
    const ftcore::ShowSegment* hold = findSegment(show, "down", spec.down);
    REQUIRE(hold);
    CHECK(hold->length > 2.0 * clips[static_cast<std::size_t>(spec.down)].length);
    CHECK(hold->loop);
}

TEST_CASE("clip showcase: the turntable turns once over the idle's first cycle, then the take goes on still") {
    ftcore::BeatClips clips;
    REQUIRE(loadClips(clips));
    const ftcore::ShowSpec spec = stoneSpec(clips);
    ftcore::ClipShow show;
    REQUIRE(ftcore::buildClipShow(clips, spec, show));
    const double idle = clips[static_cast<std::size_t>(spec.idle)].length;
    ftcore::ShowPose p = ftcore::clipShowPose(show, idle * 0.5);
    CHECK(p.segment == 0);
    CHECK(p.yaw == doctest::Approx(180.0).epsilon(1e-4));
    CHECK(p.clipTime == doctest::Approx(idle * 0.5));
    p = ftcore::clipShowPose(show, idle + 1.0);
    CHECK(p.segment == 1);
    CHECK(show.segments[1].goesOn);
    CHECK(p.yaw == doctest::Approx(0.0));
    CHECK(p.clipTime == doctest::Approx(1.0)); // wrapped: the seam is at the idle's length
    CHECK(p.clip == spec.idle);
}

TEST_CASE("clip showcase: each attack enters where the fight does, its tell runs to the impact, the dust lands on it") {
    ftcore::BeatClips clips;
    REQUIRE(loadClips(clips));
    const ftcore::ShowSpec spec = stoneSpec(clips);
    ftcore::ClipShow show;
    REQUIRE(ftcore::buildClipShow(clips, spec, show));
    std::size_t seen = 0;
    for (std::size_t i = 0; i < show.segmentCount; ++i) {
        const ftcore::ShowSegment& s = show.segments[i];
        if (s.label() != "attack") continue;
        REQUIRE(s.attack >= 0);
        const ftcore::ShowAttack& a = spec.attacks[static_cast<std::size_t>(s.attack)];
        CHECK(s.attack == static_cast<std::int16_t>(seen));
        ++seen;
        CHECK(s.clip == a.clip);
        CHECK(s.clipFrom == doctest::Approx(a.entry));
        // The move's end, or the clip's when the move outlasts it (the reel shows the clip, not a held last frame).
        const double clipEnd = clips[static_cast<std::size_t>(a.clip)].length;
        CHECK(s.length == doctest::Approx((a.until < clipEnd ? a.until : clipEnd) - a.entry));
        CHECK_FALSE(s.loop);
        CHECK_FALSE(s.goesOn);
        // Idle on either side.
        CHECK(show.segments[i - 1].clip == spec.idle);
        CHECK(show.segments[i + 1].clip == spec.idle);
        const double impact = clips[static_cast<std::size_t>(a.clip)].impact;
        bool tell = false, stop = false, dust = false;
        for (std::size_t c = 0; c < show.cueCount; ++c) {
            const ftcore::ShowCueAt& q = show.cues[c];
            if (q.attack != s.attack) continue;
            CHECK(q.place == ftcore::ShowPlace::Strike);
            if (q.cue == ftcore::ShowCue::Tell) tell = q.at == doctest::Approx(s.start);
            if (q.cue == ftcore::ShowCue::TellStop) stop = q.at == doctest::Approx(s.start + impact - a.entry);
            if (q.cue == ftcore::ShowCue::Impact) dust = q.at == doctest::Approx(s.start + impact - a.entry);
        }
        CHECK(tell);
        CHECK(stop);
        CHECK(dust);
        // The pose at the impact plays the clip at its impact.
        const ftcore::ShowPose p = ftcore::clipShowPose(show, s.start + impact - a.entry + 1e-6);
        CHECK(p.clip == a.clip);
        CHECK(p.clipTime == doctest::Approx(impact).epsilon(1e-4));
    }
    CHECK(seen == spec.attackCount);
}

TEST_CASE("clip showcase: the stepping turns ease each 30 degree step over the pivot, the walk goes out and back") {
    ftcore::BeatClips clips;
    REQUIRE(loadClips(clips));
    const ftcore::ShowSpec spec = stoneSpec(clips);
    ftcore::ClipShow show;
    REQUIRE(ftcore::buildClipShow(clips, spec, show));
    const ftcore::ShowSegment* right = findSegment(show, "turn", spec.turnR);
    REQUIRE(right);
    const double cycle = clips[static_cast<std::size_t>(spec.turnR)].length;
    CHECK(right->length == doctest::Approx(3 * cycle));
    CHECK(ftcore::clipShowPose(show, right->start + spec.turnPivot * 0.5).yaw == doctest::Approx(15.0).epsilon(1e-4));
    CHECK(ftcore::clipShowPose(show, right->start + spec.turnPivot).yaw == doctest::Approx(30.0).epsilon(1e-4));
    CHECK(ftcore::clipShowPose(show, right->start + cycle * 0.9).yaw == doctest::Approx(30.0).epsilon(1e-4));
    CHECK(ftcore::clipShowPose(show, right->start + cycle + spec.turnPivot).yaw == doctest::Approx(60.0).epsilon(1e-4));
    const ftcore::ShowSegment* out = findSegment(show, "walk", spec.walk);
    REQUIRE(out);
    CHECK(out->yaw == doctest::Approx(90.0));
    CHECK(out->clipFrom == doctest::Approx(0.833));
    const double walk = clips[static_cast<std::size_t>(spec.walk)].length;
    CHECK(out->length == doctest::Approx(2 * walk));
    const ftcore::ShowPose end = ftcore::clipShowPose(show, out->start + out->length - 1e-6);
    CHECK(end.pos.x == doctest::Approx(0.0).epsilon(1e-3));
    CHECK(end.pos.y == doctest::Approx(0.805 * 2 * walk).epsilon(1e-3));
    // The walk ends at the passing pose it began at.
    CHECK(std::fmod(out->clipFrom + out->length, walk) == doctest::Approx(0.833).epsilon(1e-3));
    const ftcore::ShowSegment* back = findSegment(show, "walk", spec.walk, static_cast<std::size_t>(out - show.segments.data()) + 1);
    REQUIRE(back);
    CHECK(back->yaw == doctest::Approx(-90.0));
    const ftcore::ShowPose home = ftcore::clipShowPose(show, back->start + back->length - 1e-6);
    CHECK(std::fabs(home.pos.x) < 1e-3f);
    CHECK(std::fabs(home.pos.y) < 1e-3f);
    const ftcore::ShowSegment& last = show.segments[static_cast<std::size_t>(back - show.segments.data()) + 1];
    CHECK(last.clip == spec.turnR);
    CHECK(last.yaw + last.stepDeg * 3.f == doctest::Approx(0.0));
}

TEST_CASE("clip showcase: the effects alone, then the two deaths each from a hidden cut") {
    ftcore::BeatClips clips;
    REQUIRE(loadClips(clips));
    const ftcore::ShowSpec spec = stoneSpec(clips);
    ftcore::ClipShow show;
    REQUIRE(ftcore::buildClipShow(clips, spec, show));
    int ahead = 0, glow = 0, crit = 0, chips = 0, heavy = 0;
    double glowOn = -1.0, glowOff = -1.0, critAt = -1.0;
    for (std::size_t c = 0; c < show.cueCount; ++c) {
        const ftcore::ShowCueAt& q = show.cues[c];
        if (q.place == ftcore::ShowPlace::Ahead) ++ahead;
        if (q.cue == ftcore::ShowCue::Chips) ++chips;
        if (q.cue == ftcore::ShowCue::ChipsHeavy) ++heavy;
        if (q.cue == ftcore::ShowCue::GlowOn) glowOn = q.at, ++glow;
        if (q.cue == ftcore::ShowCue::GlowOff) glowOff = q.at, ++glow;
        if (q.cue == ftcore::ShowCue::Crit) critAt = q.at, ++crit;
    }
    CHECK(ahead == 3); // the tell, its stop, the dust
    CHECK(chips == 2); // the light hit and the lone light chips
    CHECK(heavy == 2); // the heavy hit and the lone heavy chips
    CHECK(glow == 2);
    CHECK(crit == 1);
    CHECK(glowOff - glowOn == doctest::Approx(spec.glowFor));
    CHECK(critAt > glowOff); // alone, after the glow
    // Turned away for the weak point.
    CHECK(std::fabs(std::fmod(std::fabs(ftcore::clipShowPose(show, glowOn + 0.01).yaw), 360.f) - 180.f) < 1e-3f);
    std::vector<std::int16_t> deaths;
    for (std::size_t i = 0; i < show.segmentCount; ++i) {
        const ftcore::ShowSegment& s = show.segments[i];
        if (s.label() == "hidden") {
            CHECK(s.clip == ftcore::kNoClip);
            REQUIRE(i + 1 < show.segmentCount);
            CHECK(show.segments[i + 1].cut);
            CHECK(show.segments[i + 1].yaw == doctest::Approx(0.0));
            CHECK(ftcore::clipShowPose(show, s.start + 0.01).clip == ftcore::kNoClip);
        }
        if (s.label() == "death") {
            deaths.push_back(s.clip);
            CHECK(s.length == doctest::Approx(clips[static_cast<std::size_t>(s.clip)].length + spec.hold));
            CHECK_FALSE(s.loop);
        }
    }
    REQUIRE(deaths.size() == 2);
    CHECK(deaths[0] == spec.defeat);
    CHECK(deaths[1] == spec.defeatKneel);
    // The kneel death comes from the hold, the hold from the go-down.
    const ftcore::ShowSegment& last = show.segments[show.segmentCount - 1];
    CHECK(show.segments[show.segmentCount - 2].clip == spec.down);
    CHECK(show.segments[show.segmentCount - 3].clip == spec.downIn);
    CHECK(show.segments[show.segmentCount - 3].cut);
    // A death holds its last frame; the reel is over after it.
    CHECK(ftcore::clipShowPose(show, last.start + last.length - 0.01).clipTime
          == doctest::Approx(clips[static_cast<std::size_t>(last.clip)].length));
    CHECK(ftcore::clipShowPose(show, show.length).segment == -1);
}

TEST_CASE("clip showcase: the glow and the crit show the weak point to the front: one ahead is not turned, one behind is") {
    ftcore::BeatClips clips;
    REQUIRE(loadClips(clips));
    ftcore::ShowSpec turning = stoneSpec(clips);
    ftcore::ShowSpec still; // no turn clips: the half turn is a hidden cut
    still.idle = idOf(clips, "idle");
    for (const ftcore::ShowSpec* base : {&turning, &still}) {
        const bool turns = base->turnL != ftcore::kNoClip;
        for (float facing : {0.f, 180.f}) {
            CAPTURE(turns);
            CAPTURE(facing);
            ftcore::ShowSpec spec = *base;
            spec.weakFacing = facing;
            ftcore::ClipShow show;
            std::string err;
            REQUIRE_MESSAGE(ftcore::buildClipShow(clips, spec, show, &err), err);
            double glowOn = -1.0, critAt = -1.0;
            for (std::size_t c = 0; c < show.cueCount; ++c) {
                if (show.cues[c].cue == ftcore::ShowCue::GlowOn) glowOn = show.cues[c].at;
                if (show.cues[c].cue == ftcore::ShowCue::Crit) critAt = show.cues[c].at;
            }
            REQUIRE(glowOn > 0.0);
            REQUIRE(critAt > glowOn);
            // The segments between the last lone chips take (light or heavy) and the first glow take.
            std::size_t glow = show.segmentCount, chips = show.segmentCount;
            for (std::size_t s = 0; s < show.segmentCount && glow == show.segmentCount; ++s) {
                if (show.segments[s].label() == "fx_glow") glow = s;
            }
            REQUIRE(glow < show.segmentCount);
            for (std::size_t s = glow; s-- > 0;) {
                if (show.segments[s].label().substr(0, 8) == "fx_chips") {
                    chips = s;
                    break;
                }
            }
            REQUIRE(chips < glow);
            const std::size_t between = glow - chips - 1;
            const float atGlow = ftcore::clipShowPose(show, glowOn + 0.01).yaw;
            const float atCrit = ftcore::clipShowPose(show, critAt + 0.01).yaw;
            if (facing < 90.f) {
                // Ahead: the glow's take goes on from the chips', square to the front.
                CHECK(between == 0);
                CHECK_FALSE(show.segments[glow].cut);
                CHECK(atGlow == doctest::Approx(0.0));
                CHECK(atCrit == doctest::Approx(0.0));
            } else {
                // Behind: a half turn, stepped round on the turn clip or cut to from a hidden moment.
                REQUIRE(between == 1);
                CHECK(show.segments[chips + 1].label() == (turns ? "turn" : "hidden"));
                CHECK(std::fabs(std::fabs(atGlow) - 180.f) < 1e-3f);
                CHECK(std::fabs(std::fabs(atCrit) - 180.f) < 1e-3f);
            }
        }
    }
    // A facing that is not a number is refused.
    ftcore::ShowSpec bad = still;
    bad.weakFacing = std::nanf("");
    ftcore::ClipShow show;
    std::string err;
    CHECK_FALSE(ftcore::buildClipShow(clips, bad, show, &err));
    CHECK(err.find("facing") != std::string::npos);
}

TEST_CASE("clip showcase: the weak point's window in the go-down's hold, none by default, one per phase when set") {
    ftcore::BeatClips clips;
    REQUIRE(loadClips(clips));
    const ftcore::ShowSpec base = stoneSpec(clips);
    CHECK(base.downGlow == 0.0); // the default reels play the hold without a window
    CHECK(base.downGlowTwo == 0.0);
    const double hold = 2.0 * clips[static_cast<std::size_t>(base.down)].length + 1.0;

    struct Case { double one, two; std::size_t downs; };
    const Case cases[] = {{0.0, 0.0, 1}, {2.0, 0.0, 1}, {2.0, 1.6, 2}};
    for (const Case& k : cases) {
        CAPTURE(k.one);
        CAPTURE(k.two);
        ftcore::ShowSpec spec = base;
        spec.downGlow = k.one;
        spec.downGlowTwo = k.two;
        ftcore::ClipShow show;
        std::string err;
        REQUIRE_MESSAGE(ftcore::buildClipShow(clips, spec, show, &err), err);
        std::vector<const ftcore::ShowSegment*> downs;
        for (std::size_t s = 0; s < show.segmentCount; ++s) {
            if (show.segments[s].label() == "down") downs.push_back(&show.segments[s]);
        }
        // The go-downs before the effects alone; the kneel death's hold is the last segment and has no window.
        const ftcore::ShowSegment* glowTake = findSegment(show, "fx_glow", ftcore::kNoClip);
        REQUIRE(glowTake != nullptr);
        std::size_t before = 0;
        for (const ftcore::ShowSegment* d : downs) before += d->start < glowTake->start ? 1 : 0;
        REQUIRE(before == k.downs);
        int glowOn = 0, glowOff = 0, crit = 0;
        for (std::size_t c = 0; c < show.cueCount; ++c) {
            const ftcore::ShowCueAt& q = show.cues[c];
            if (c > 0) CHECK(q.at >= show.cues[c - 1].at); // still in time order
            glowOn += q.cue == ftcore::ShowCue::GlowOn;
            glowOff += q.cue == ftcore::ShowCue::GlowOff;
            crit += q.cue == ftcore::ShowCue::Crit;
        }
        const int windows = (k.one > 0.0 ? 1 : 0) + (k.two > 0.0 ? 1 : 0);
        CHECK(glowOn == 1 + windows); // the lone glow stays
        CHECK(glowOff == 1 + windows);
        CHECK(crit == 1 + windows);
        const double lengths[] = {k.one, k.two};
        for (int w = 0; w < windows; ++w) {
            const ftcore::ShowSegment& d = *downs[static_cast<std::size_t>(w)];
            CHECK(d.clip == spec.down);
            CHECK(d.length == doctest::Approx(hold));
            double on = -1.0, mid = -1.0, off = -1.0;
            for (std::size_t c = 0; c < show.cueCount; ++c) {
                const ftcore::ShowCueAt& q = show.cues[c];
                if (q.at < d.start - 1e-9 || q.at > d.start + d.length + 1e-9) continue;
                CHECK(q.place == ftcore::ShowPlace::WeakPoint);
                if (q.cue == ftcore::ShowCue::GlowOn) on = q.at;
                if (q.cue == ftcore::ShowCue::Crit) mid = q.at;
                if (q.cue == ftcore::ShowCue::GlowOff) off = q.at;
            }
            CHECK(on == doctest::Approx(d.start)); // opens with the hold
            CHECK(off - on == doctest::Approx(lengths[w]));
            CHECK(mid - on == doctest::Approx(0.5 * lengths[w]));
            // The pose is the hold's own clip all through the window.
            CHECK(ftcore::clipShowPose(show, on + 0.01).clip == spec.down);
            CHECK(ftcore::clipShowPose(show, off - 0.01).clip == spec.down);
        }
    }
    // A window that is not a number, below 0 or longer than the hold is refused.
    const double bad[] = {std::nan(""), -0.5, hold + 0.1};
    for (double v : bad) {
        ftcore::ShowSpec spec = base;
        spec.downGlowTwo = v;
        ftcore::ClipShow show;
        std::string err;
        CHECK_FALSE(ftcore::buildClipShow(clips, spec, show, &err));
        CHECK(err.find("window") != std::string::npos);
    }
}

TEST_CASE("clip showcase: the cues crossed in a frame") {
    ftcore::BeatClips clips;
    REQUIRE(loadClips(clips));
    const ftcore::ShowSpec spec = stoneSpec(clips);
    ftcore::ClipShow show;
    REQUIRE(ftcore::buildClipShow(clips, spec, show));
    REQUIRE(show.cueCount > 3);
    const double at = show.cues[0].at;
    std::size_t first = 0, last = 0;
    ftcore::clipShowCues(show, at - 0.01, at, first, last);
    CHECK(first == 0);
    CHECK(last >= 1);
    ftcore::clipShowCues(show, at, at + 1e-4, first, last);
    CHECK(first == last); // not twice
    // Every cue once over the whole reel, frame by frame.
    std::size_t total = 0;
    for (double t = 0.0; t < show.length + 0.1; t += 1.0 / 30.0) {
        ftcore::clipShowCues(show, t, t + 1.0 / 30.0, first, last);
        total += last - first;
    }
    CHECK(total == show.cueCount);
}

TEST_CASE("clip showcase: a frame's pose and cues allocate nothing") {
    ftcore::BeatClips clips;
    REQUIRE(loadClips(clips));
    const ftcore::ShowSpec spec = stoneSpec(clips);
    static ftcore::ClipShow show;
    REQUIRE(ftcore::buildClipShow(clips, spec, show));
    double sink = 0.0;
    const std::uint64_t before = ftcore_test::allocStats().count;
    for (double t = 0.0; t < show.length; t += 1.0 / 30.0) {
        const ftcore::ShowPose p = ftcore::clipShowPose(show, t);
        std::size_t first = 0, last = 0;
        ftcore::clipShowCues(show, t, t + 1.0 / 30.0, first, last);
        sink += p.clipTime + static_cast<double>(last - first);
    }
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(sink > 0.0);
}

TEST_CASE("clip showcase: refusals") {
    ftcore::BeatClips clips;
    REQUIRE(loadClips(clips));
    ftcore::ClipShow show;
    std::string err;
    ftcore::ShowSpec spec = stoneSpec(clips);
    spec.idle = ftcore::kNoClip;
    CHECK_FALSE(ftcore::buildClipShow(clips, spec, show, &err));
    CHECK(err.find("idle") != std::string::npos);
    CHECK(show.segmentCount == 0);
    spec = stoneSpec(clips);
    spec.turnL = ftcore::kNoClip;
    CHECK_FALSE(ftcore::buildClipShow(clips, spec, show, &err));
    CHECK(err.find("turns") != std::string::npos);
    spec = stoneSpec(clips);
    spec.stagger = static_cast<std::int16_t>(clips.count);
    CHECK_FALSE(ftcore::buildClipShow(clips, spec, show, &err));
    CHECK(err.find("outside") != std::string::npos);
    spec = stoneSpec(clips);
    spec.attacks[0].entry = spec.attacks[0].until + 0.1;
    CHECK_FALSE(ftcore::buildClipShow(clips, spec, show, &err));
    CHECK(err.find("past its end") != std::string::npos);
    // Nothing but the idle: the turntable, the effects alone and nothing else.
    ftcore::ShowSpec bare;
    bare.idle = idOf(clips, "idle");
    REQUIRE_MESSAGE(ftcore::buildClipShow(clips, bare, show, &err), err);
    for (std::size_t i = 0; i < show.segmentCount; ++i)
        CHECK((show.segments[i].clip == bare.idle || show.segments[i].clip == ftcore::kNoClip));
    CHECK(show.cueCount == 8); // the tell, its stop, the dust, the light and the heavy chips, the glow on and off, the crit
}

// ---- the marked move's tell light, and the fight's clip rates ----

namespace {

// The shipped boss laid out as arena moves over the shipped arena kind (the default lead).
struct ShippedBoss {
    ftcore::StoneBearerDef def;
    ftcore::StoneBearerLayout layout;
    ftcore::ArenaEnemyKind kind;
    ftcore::BeatClips clips;
};

void loadShipped(ShippedBoss& s) {
    std::string text, err;
    REQUIRE(ftcore_test::readProjectData("Battle/arena_boss/stone_bearer.json", text));
    REQUIRE_MESSAGE(ftcore::importStoneBearer(text, s.def, &err), err);
    REQUIRE(ftcore_test::readProjectData("Battle/arena.json", text));
    ftcore::ArenaTable table;
    REQUIRE_MESSAGE(ftcore::importArenaTable(text, table, &err), err);
    std::size_t at = table.enemyIds.size();
    for (std::size_t i = 0; i < table.enemyIds.size(); ++i)
        if (table.enemyIds[i] == "stone_bearer") at = i;
    REQUIRE(at < table.enemies.size());
    REQUIRE(!table.fighters.empty());
    s.kind = table.enemies[at];
    REQUIRE_MESSAGE(ftcore::buildStoneBearerKind(s.def, table.fighters[0], s.kind, &s.layout, &err), err);
    REQUIRE(loadClips(s.clips));
}

} // namespace

TEST_CASE("tell light: from the shipped data, every marked move's first impact has the window, on at its time, off at that impact") {
    ShippedBoss s;
    loadShipped(s);
    const double frame = 1.0 / 30.0;
    int lit = 0, later = 0;
    for (std::size_t a = 0; a < s.layout.count; ++a) {
        const ftcore::ArenaBossMove& src = s.def.boss.moves[s.layout.moveOf[a]];
        const std::uint8_t k = s.layout.impactOf[a];
        CAPTURE(src.id());
        CAPTURE(static_cast<int>(k));
        const int c = s.clips.find(ftcore::stoneBearerClipOf(src.id()));
        REQUIRE(c >= 0);
        const ftcore::BeatClip& row = s.clips[static_cast<std::size_t>(c)];
        const std::optional<ftcore::TellLightWindow> w = ftcore::tellLightWindow(row, src.marked, k);
        if (!src.marked) {
            CHECK_FALSE(w.has_value()); // the flag drives it: an unmarked move never lights, whatever its clip has
            continue;
        }
        if (k > 0) {
            // A later impact of the same use enters the clip again: it lights nothing.
            CHECK_FALSE(w.has_value());
            ++later;
            continue;
        }
        // Every marked move's clip carries the field (a variant plays its base move's clip, so it gets the same window).
        REQUIRE(w.has_value());
        CHECK(w->on == doctest::Approx(row.tellLightOn));
        CHECK(w->on > 0.0);
        CHECK(w->off == doctest::Approx(row.impact));
        // Off at the move's first impact: its beats at the file's bpm, and in the fight as its startup ends (entered
        // at rate 1.0, so the clip's impact lands there).
        CHECK(std::fabs(w->off - ftcore::arenaBeats(static_cast<double>(src.impacts[0]), s.def.boss.bpm)) <= frame);
        const double entry = ftcore::stoneBearerClipEntry(s.kind, s.layout, a, row.impact);
        CHECK(std::fabs(entry + static_cast<double>(s.kind.moves[a].attack.startup) - w->off) <= frame);
        ++lit;
    }
    CHECK(lit == 2); // the marked move and its two-impact variant
    CHECK(later == 1);
    // A row with the field is played by a marked move (else it would never light).
    for (std::size_t c = 0; c < s.clips.count; ++c) {
        if (s.clips[c].tellLightOn < 0.0) continue;
        CAPTURE(s.clips.id(c));
        bool byMarked = false;
        for (std::uint8_t m = 0; m < s.def.boss.moveCount; ++m)
            byMarked = byMarked || (s.def.boss.moves[m].marked && ftcore::stoneBearerClipOf(s.def.boss.moves[m].id()) == s.clips.id(c));
        CHECK(byMarked);
    }
}

TEST_CASE("tell light: the reel lights a marked attack at its window's clip times, out with the tell at the impact") {
    ftcore::BeatClips clips;
    REQUIRE(loadClips(clips));
    const std::int16_t thrown = idOf(clips, "marked_throw");
    REQUIRE(thrown >= 0);
    const ftcore::BeatClip& row = clips[static_cast<std::size_t>(thrown)];
    const std::optional<ftcore::TellLightWindow> w = ftcore::tellLightWindow(row, true);
    REQUIRE(w.has_value());
    ftcore::ShowSpec spec = stoneSpec(clips);
    REQUIRE(spec.attackCount + 3 <= ftcore::kMaxShowAttacks);
    // Marked, entered at its start as the fight enters it; the same clip unmarked; marked, entered past `on`.
    const double entries[] = {0.0, 0.0, w->on + 0.2};
    const bool marks[] = {true, false, true};
    const std::size_t first = spec.attackCount;
    for (std::size_t i = 0; i < 3; ++i) {
        ftcore::ShowAttack& a = spec.attacks[spec.attackCount++];
        a.clip = thrown;
        a.move = static_cast<std::int16_t>(10 + i);
        a.entry = entries[i];
        a.until = row.length;
        a.marked = marks[i];
    }
    ftcore::ClipShow show;
    std::string err;
    REQUIRE_MESSAGE(ftcore::buildClipShow(clips, spec, show, &err), err);
    for (std::size_t i = 1; i < show.cueCount; ++i) CHECK(show.cues[i].at >= show.cues[i - 1].at);
    int ons = 0, offs = 0;
    for (std::size_t c = 0; c < show.cueCount; ++c) {
        ons += show.cues[c].cue == ftcore::ShowCue::LightOn ? 1 : 0;
        offs += show.cues[c].cue == ftcore::ShowCue::LightOff ? 1 : 0;
    }
    CHECK(ons == 2); // the two marked attacks only (stoneSpec's own attacks are unmarked)
    CHECK(offs == 2);
    for (std::size_t i = 0; i < 3; ++i) {
        const std::int16_t index = static_cast<std::int16_t>(first + i);
        CAPTURE(i);
        const ftcore::ShowSegment* part = nullptr;
        for (std::size_t g = 0; g < show.segmentCount; ++g)
            if (show.segments[g].attack == index) part = &show.segments[g];
        REQUIRE(part);
        double on = -1.0, off = -1.0, stop = -1.0;
        std::size_t tellAt = show.cueCount, onAt = show.cueCount;
        for (std::size_t c = 0; c < show.cueCount; ++c) {
            const ftcore::ShowCueAt& q = show.cues[c];
            if (q.attack != index) continue;
            if (q.cue == ftcore::ShowCue::Tell && tellAt == show.cueCount) {
                tellAt = c;
            }
            if (q.cue == ftcore::ShowCue::LightOn || q.cue == ftcore::ShowCue::LightOff) {
                CHECK(q.place == ftcore::ShowPlace::Strike);
                (q.cue == ftcore::ShowCue::LightOn ? on : off) = q.at;
                if (q.cue == ftcore::ShowCue::LightOn) {
                    onAt = c;
                }
            }
            if (q.cue == ftcore::ShowCue::TellStop) {
                stop = q.at;
            }
        }
        // The host lights the lane its Tell drew: the Tell comes first, even at the same time (entered past `on`).
        if (marks[i]) {
            CHECK(tellAt < onAt);
        }
        if (!marks[i]) {
            CHECK(on < 0.0);
            CHECK(off < 0.0);
            continue;
        }
        const double entry = entries[i];
        // On at its clip time (from the part's start when entered past it), off at the impact with the tell's stop.
        CHECK(on == doctest::Approx(part->start + std::fmax(0.0, w->on - entry)));
        CHECK(off == doctest::Approx(part->start + w->off - entry));
        CHECK(off == doctest::Approx(stop));
        const ftcore::ShowPose p = ftcore::clipShowPose(show, on + 1e-6);
        CHECK(p.clip == thrown);
        CHECK(p.clipTime == doctest::Approx(std::fmax(w->on, entry)).epsilon(1e-4));
        const ftcore::ShowPose q = ftcore::clipShowPose(show, off + 1e-6);
        CHECK(q.clipTime == doctest::Approx(w->off).epsilon(1e-4));
    }
}

TEST_CASE("fight rate: every shipped arena move's clip plays at rate 1.0, from the shared rate") {
    ShippedBoss s;
    loadShipped(s);
    for (std::size_t a = 0; a < s.layout.count; ++a) {
        const ftcore::ArenaBossMove& src = s.def.boss.moves[s.layout.moveOf[a]];
        CAPTURE(src.id());
        CAPTURE(static_cast<int>(s.layout.impactOf[a]));
        const int c = s.clips.find(ftcore::stoneBearerClipOf(src.id()));
        REQUIRE(c >= 0);
        const ftcore::BeatClip& row = s.clips[static_cast<std::size_t>(c)];
        const double entry = ftcore::stoneBearerClipEntry(s.kind, s.layout, a, row.impact);
        // The runner's clamp (kArenaMinClipRate, kArenaMaxClipRate).
        CHECK(ftcore::beatClipRate(row.impact, entry, static_cast<double>(s.kind.moves[a].attack.startup), 0.25, 4.0) == 1.0);
    }
    // A startup longer than the clip's impact would stretch it from its start: the rate the reel's log reports.
    CHECK(ftcore::beatClipRate(3.158, 3.158 - 4.0, 4.0, 0.25, 4.0) == doctest::Approx(3.158 / 4.0));
}
