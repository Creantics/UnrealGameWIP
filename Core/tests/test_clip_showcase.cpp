#include "ftcore/ClipShowcase.h"

#include "ftcore/BeatChoreo.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
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
    int ahead = 0, glow = 0, crit = 0, chips = 0;
    double glowOn = -1.0, glowOff = -1.0, critAt = -1.0;
    for (std::size_t c = 0; c < show.cueCount; ++c) {
        const ftcore::ShowCueAt& q = show.cues[c];
        if (q.place == ftcore::ShowPlace::Ahead) ++ahead;
        if (q.cue == ftcore::ShowCue::Chips) ++chips;
        if (q.cue == ftcore::ShowCue::GlowOn) glowOn = q.at, ++glow;
        if (q.cue == ftcore::ShowCue::GlowOff) glowOff = q.at, ++glow;
        if (q.cue == ftcore::ShowCue::Crit) critAt = q.at, ++crit;
    }
    CHECK(ahead == 3); // the tell, its stop, the dust
    CHECK(chips == 3); // the two hits and the lone chips
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
    CHECK(show.cueCount == 7); // the tell, its stop, the dust, the chips, the glow on and off, the crit
}
