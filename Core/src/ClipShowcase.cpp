#include "ftcore/ClipShowcase.h"

#include <cmath>
#include <cstdio>
#include <optional>
#include <utility>

namespace ftcore {

const char* showCueKey(ShowCue c) {
    switch (c) {
    case ShowCue::Tell: return "tell";
    case ShowCue::TellStop: return "tell_stop";
    case ShowCue::Impact: return "impact";
    case ShowCue::Chips: return "chips";
    case ShowCue::GlowOn: return "glow_on";
    case ShowCue::GlowOff: return "glow_off";
    case ShowCue::Crit: return "crit";
    case ShowCue::ChipsHeavy: return "chips_heavy";
    case ShowCue::LightOn: return "light_on";
    case ShowCue::LightOff: return "light_off";
    }
    return "?";
}

namespace {

constexpr double kPi = 3.14159265358979323846;

Vec2 ahead(float yawDeg, double metres) {
    const double r = static_cast<double>(yawDeg) * kPi / 180.0;
    return Vec2{static_cast<float>(std::cos(r) * metres), static_cast<float>(std::sin(r) * metres)};
}

double smootherstep(double u) {
    return u * u * u * (10.0 - 15.0 * u + 6.0 * u * u);
}

struct Builder {
    const BeatClips& clips;
    const ShowSpec& spec;
    ClipShow& out;
    std::string* error;
    double t = 0.0;
    float yaw = 0.f;
    Vec2 pos{};
    bool ok = true;

    void fail(const char* what) {
        if (ok && error) {
            *error = what;
        }
        ok = false;
    }

    double len(std::int16_t c) const { return clips.items[static_cast<std::size_t>(c)].length; }

    // A new segment from the cursor; a segment of the same clip as the one before (not a cut) goes on in its take.
    ShowSegment* add(const char* label, std::int16_t clip, double length, double from, bool loop, bool cut = false) {
        if (!ok) {
            return nullptr;
        }
        if (out.segmentCount >= kMaxShowSegments) {
            fail("the reel has more segments than kMaxShowSegments");
            return nullptr;
        }
        ShowSegment& s = out.segments[out.segmentCount];
        const ShowSegment* before = out.segmentCount > 0 ? &out.segments[out.segmentCount - 1] : nullptr;
        ++out.segmentCount;
        s = ShowSegment{};
        std::snprintf(s.labelText.data(), s.labelText.size(), "%s", label);
        s.clip = clip;
        s.start = t;
        s.length = length;
        s.clipFrom = from;
        s.clipLength = clip >= 0 ? len(clip) : 0.0;
        s.loop = loop;
        s.cut = cut;
        s.yaw = yaw;
        s.pos = pos;
        if (!cut && clip >= 0 && before && before->clip == clip) {
            s.goesOn = true;
            s.clipFrom = before->clipFrom + before->length;
        }
        t += length;
        return &s;
    }

    void cue(double at, ShowCue c, ShowPlace place, std::int16_t attack = -1) {
        if (!ok) {
            return;
        }
        if (out.cueCount >= kMaxShowCues) {
            fail("the reel has more cues than kMaxShowCues");
            return;
        }
        out.cues[out.cueCount++] = ShowCueAt{at, c, place, attack};
    }

    void idle(const char* label, double length) { add(label, spec.idle, length, 0.0, true); }

    void play(const char* label, std::int16_t clip, double extra = 0.0) {
        if (clip >= 0) {
            add(label, clip, len(clip) + extra, 0.0, false);
        }
    }

    void turn(std::int16_t clip, float sign, int steps) {
        const double cycle = len(clip);
        ShowSegment* s = add("turn", clip, cycle * steps, 0.0, true);
        if (s) {
            s->stepDeg = sign * spec.turnDeg;
            s->cycle = cycle;
            s->pivot = spec.turnPivot;
            yaw += sign * spec.turnDeg * static_cast<float>(steps);
        }
    }

    void walk(float speed) {
        const double length = len(spec.walk) * spec.walkCycles;
        ShowSegment* s = add("walk", spec.walk, length, spec.walkFrom, true);
        if (s) {
            s->speed = speed;
            const Vec2 d = ahead(yaw, static_cast<double>(speed) * length);
            pos = Vec2{pos.x + d.x, pos.y + d.y};
        }
    }

    // The boss hidden for a moment; the next take starts square to the front, at the reel's origin, without a fade.
    void hide() {
        add("hidden", kNoClip, spec.hidden, 0.0, false, true);
        yaw = 0.f;
        pos = Vec2{};
    }
};

bool validClip(const BeatClips& clips, std::int16_t c) {
    return c == kNoClip || (c >= 0 && static_cast<std::size_t>(c) < clips.count);
}

// An angle in (-180, 180].
float wrapDeg(float a) {
    float r = std::fmod(a, 360.f);
    if (r <= -180.f) {
        r += 360.f;
    } else if (r > 180.f) {
        r -= 360.f;
    }
    return r;
}

} // namespace

bool buildClipShow(const BeatClips& clips, const ShowSpec& spec, ClipShow& out, std::string* error) {
    out.segmentCount = 0;
    out.cueCount = 0;
    out.length = 0.0;
    const std::int16_t roles[] = {spec.idle, spec.idleTwo, spec.walk, spec.turnL, spec.turnR, spec.stagger, spec.downIn,
                                  spec.down, spec.downOut, spec.defeat, spec.defeatKneel, spec.hitLight, spec.hitHeavy};
    const char* problem = nullptr;
    for (std::int16_t c : roles) {
        if (!validClip(clips, c)) {
            problem = "a clip index is outside the table";
        }
    }
    if (spec.idle == kNoClip) {
        problem = "the idle is missing";
    } else if ((spec.turnL == kNoClip) != (spec.turnR == kNoClip)) {
        problem = "only one of the turns is given";
    } else if (spec.attackCount > kMaxShowAttacks) {
        problem = "more attacks than kMaxShowAttacks";
    } else if (spec.walkCycles < 1 || spec.turnSteps < 1 || spec.turnPivot <= 0.0) {
        problem = "the walk's cycles, the turn's steps and its pivot must be over 0";
    } else if (!std::isfinite(spec.weakFacing)) {
        problem = "the weak point's facing is not a number";
    } else if (!(spec.downGlow >= 0.0) || !(spec.downGlowTwo >= 0.0) || !std::isfinite(spec.downGlow) || !std::isfinite(spec.downGlowTwo)) {
        problem = "a window in the go-down's hold is not a number of seconds, 0 or more";
    } else if (spec.down != kNoClip && validClip(clips, spec.down)
               && std::fmax(spec.downGlow, spec.downGlowTwo) > 2.0 * clips.items[static_cast<std::size_t>(spec.down)].length + 1.0) {
        problem = "a window in the go-down's hold is longer than the hold";
    }
    for (std::size_t i = 0; !problem && i < spec.attackCount; ++i) {
        const ShowAttack& a = spec.attacks[i];
        if (a.clip == kNoClip || !validClip(clips, a.clip)) {
            problem = "an attack's clip is missing or outside the table";
        } else {
            const double length = clips.items[static_cast<std::size_t>(a.clip)].length;
            const double until = a.until > 0.0 ? std::fmin(a.until, length) : length;
            if (a.entry < 0.0 || a.entry >= until) {
                problem = "an attack enters its clip past its end";
            }
        }
    }
    float walkSpeed = spec.walkSpeed;
    if (!problem && spec.walk != kNoClip) {
        walkSpeed = walkSpeed > 0.f ? walkSpeed : static_cast<float>(clips.items[static_cast<std::size_t>(spec.walk)].speed);
        if (walkSpeed <= 0.f) {
            problem = "the walk has no speed";
        }
    }
    if (problem) {
        if (error) {
            *error = problem;
        }
        return false;
    }

    Builder b{clips, spec, out, error};
    const double idleLength = b.len(spec.idle);

    // The turntable: one turn over the idle's first cycle, then still for the second and a little past its seam.
    if (ShowSegment* s = b.add("turntable", spec.idle, idleLength, 0.0, true, true)) {
        s->spin = static_cast<float>(360.0 / idleLength);
    }
    b.idle("idle", idleLength + spec.after);
    if (spec.idleTwo != kNoClip) {
        b.add("idle_p2", spec.idleTwo, 2.0 * b.len(spec.idleTwo) + spec.after, 0.0, true);
    }

    // Every attack as the fight plays it: entered where the fight enters it, left at the move's end.
    for (std::size_t i = 0; i < spec.attackCount; ++i) {
        const ShowAttack& a = spec.attacks[i];
        const double length = b.len(a.clip);
        const double until = a.until > 0.0 ? std::fmin(a.until, length) : length;
        b.idle("idle", spec.before);
        const std::int16_t index = static_cast<std::int16_t>(i);
        ShowSegment* s = b.add("attack", a.clip, until - a.entry, a.entry, false);
        const BeatClip& row = clips.items[static_cast<std::size_t>(a.clip)];
        const double impact = row.impact;
        if (s && impact > 0.0) {
            const double at = s->start + std::fmax(0.0, impact - a.entry);
            s->attack = index;
            b.cue(s->start, ShowCue::Tell, ShowPlace::Strike, index);
            // A marked move's tell light, at its window's clip times (from the part's start when entered past `on`);
            // out with the tell at the impact.
            if (const std::optional<TellLightWindow> w = tellLightWindow(row, a.marked, 0)) {
                const double on = s->start + std::fmax(0.0, w->on - a.entry);
                const double off = s->start + std::fmax(0.0, w->off - a.entry);
                if (on < off) {
                    b.cue(on, ShowCue::LightOn, ShowPlace::Strike, index);
                    b.cue(off, ShowCue::LightOff, ShowPlace::Strike, index);
                }
            }
            b.cue(at, ShowCue::TellStop, ShowPlace::Strike, index);
            b.cue(at, ShowCue::Impact, ShowPlace::Strike, index);
        } else if (s) {
            s->attack = index;
        }
        b.idle("idle", spec.after);
    }

    // The hits (chips where the blow lands), the stagger, the go-down, its hold and the get-up.
    const std::pair<std::int16_t, ShowCue> hits[] = {{spec.hitLight, ShowCue::Chips}, {spec.hitHeavy, ShowCue::ChipsHeavy}};
    for (const auto& [hit, chips] : hits) {
        if (hit != kNoClip) {
            b.cue(b.t, chips, ShowPlace::Surface);
            b.play("hit", hit);
            b.idle("idle", spec.after);
        }
    }
    if (spec.stagger != kNoClip) {
        b.play("stagger", spec.stagger);
        b.idle("idle", spec.after);
    }
    // A window in the hold opens on its first frame, takes a crit at its middle and shuts at its end; a second window
    // (a later phase's) is a second go-down.
    if (spec.down != kNoClip) {
        const double windows[] = {spec.downGlow, spec.downGlowTwo};
        for (std::size_t w = 0; w < 2; ++w) {
            if (w > 0 && !(windows[w] > 0.0)) {
                break;
            }
            b.play("down_in", spec.downIn);
            const ShowSegment* hold = b.add("down", spec.down, 2.0 * b.len(spec.down) + 1.0, 0.0, true);
            if (hold && windows[w] > 0.0) {
                b.cue(hold->start, ShowCue::GlowOn, ShowPlace::WeakPoint);
                b.cue(hold->start + 0.5 * windows[w], ShowCue::Crit, ShowPlace::WeakPoint);
                b.cue(hold->start + windows[w], ShowCue::GlowOff, ShowPlace::WeakPoint);
            }
            b.play("down_out", spec.downOut);
            b.idle("idle", spec.after);
        }
    }

    // The stepping turns and the walk out and back.
    const bool turns = spec.turnL != kNoClip;
    if (turns || spec.walk != kNoClip) {
        if (turns) {
            b.turn(spec.turnR, 1.f, spec.turnSteps);
        }
        if (spec.walk != kNoClip) {
            b.walk(walkSpeed);
        }
        if (turns) {
            b.turn(spec.turnL, -1.f, 2 * spec.turnSteps);
        }
        if (spec.walk != kNoClip) {
            b.walk(walkSpeed);
        }
        if (turns) {
            b.turn(spec.turnR, 1.f, spec.turnSteps);
        }
        b.idle("idle", spec.after);
    }

    // The effects alone: a tell ahead and its dust, light then heavy chips on the body; turned so the weak point faces the
    // front (a half turn for one behind, none for one ahead), its glow, then a crit.
    b.idle("fx_tell", 0.5);
    b.cue(b.t, ShowCue::Tell, ShowPlace::Ahead);
    b.idle("fx_tell", spec.tellFor);
    b.cue(b.t, ShowCue::TellStop, ShowPlace::Ahead);
    b.cue(b.t, ShowCue::Impact, ShowPlace::Ahead);
    b.idle("fx_impact", 2.0);
    b.cue(b.t, ShowCue::Chips, ShowPlace::Surface);
    b.idle("fx_chips", 2.0);
    b.cue(b.t, ShowCue::ChipsHeavy, ShowPlace::Surface);
    b.idle("fx_chips_heavy", 2.0);
    // The turn that brings the weak point to the front: + right; a half turn goes left.
    const float face = wrapDeg(-spec.weakFacing - b.yaw);
    if (turns) {
        const int steps = spec.turnDeg > 0.f ? static_cast<int>(std::lround(std::fabs(face) / spec.turnDeg)) : 0;
        if (steps > 0) {
            const bool right = face > 0.f && face < 180.f;
            b.turn(right ? spec.turnR : spec.turnL, right ? 1.f : -1.f, steps);
        }
        b.idle("fx_glow", 1.0);
    } else if (std::fabs(face) >= 0.5f) {
        b.hide();
        b.yaw = wrapDeg(-spec.weakFacing);
        b.add("fx_glow", spec.idle, 1.0, 0.0, true, true);
    } else {
        b.idle("fx_glow", 1.0);
    }
    b.cue(b.t, ShowCue::GlowOn, ShowPlace::WeakPoint);
    b.idle("fx_glow", spec.glowFor);
    b.cue(b.t, ShowCue::GlowOff, ShowPlace::WeakPoint);
    b.idle("fx_crit", 1.0);
    b.cue(b.t, ShowCue::Crit, ShowPlace::WeakPoint);
    b.idle("fx_crit", 2.5);

    // The deaths, each its own take: standing, and from the hold.
    if (spec.defeat != kNoClip) {
        b.hide();
        b.add("idle", spec.idle, spec.before, 0.0, true, true);
        b.play("death", spec.defeat, spec.hold);
    }
    if (spec.defeatKneel != kNoClip) {
        b.hide();
        bool first = true;
        if (spec.downIn != kNoClip) {
            b.add("down_in", spec.downIn, b.len(spec.downIn), 0.0, false, true);
            first = false;
        }
        if (spec.down != kNoClip) {
            b.add("down", spec.down, 2.0, 0.0, true, first);
            first = false;
        }
        if (first) {
            b.add("idle", spec.idle, spec.before, 0.0, true, true);
        }
        b.play("death", spec.defeatKneel, spec.hold);
    }
    if (!b.ok) {
        out.segmentCount = 0;
        out.cueCount = 0;
        return false;
    }
    out.length = b.t;
    return true;
}

ShowPose clipShowPose(const ClipShow& show, double t) {
    ShowPose p;
    if (show.segmentCount == 0 || t >= show.length) {
        return p;
    }
    t = t < 0.0 ? 0.0 : t;
    std::size_t i = 0;
    while (i + 1 < show.segmentCount && show.segments[i + 1].start <= t) {
        ++i;
    }
    const ShowSegment& s = show.segments[i];
    const double st = t - s.start;
    p.segment = static_cast<int>(i);
    p.clip = s.clip;
    p.loop = s.loop;
    p.segmentTime = st;
    double at = s.clipFrom + st;
    if (s.clipLength > 0.0) {
        at = s.loop ? std::fmod(at, s.clipLength) : (at > s.clipLength ? s.clipLength : at);
    }
    p.clipTime = at < 0.0 ? 0.0 : at;
    double yaw = static_cast<double>(s.yaw) + static_cast<double>(s.spin) * st;
    if (s.stepDeg != 0.f && s.cycle > 0.0) {
        const double whole = std::floor(st / s.cycle);
        const double into = st - whole * s.cycle;
        const double u = s.pivot > 0.0 ? std::fmin(into / s.pivot, 1.0) : 1.0;
        yaw += static_cast<double>(s.stepDeg) * (whole + smootherstep(u));
    }
    p.yaw = static_cast<float>(yaw);
    const Vec2 d = ahead(s.yaw, static_cast<double>(s.speed) * st);
    p.pos = Vec2{s.pos.x + d.x, s.pos.y + d.y};
    return p;
}

void clipShowCues(const ClipShow& show, double from, double to, std::size_t& first, std::size_t& last) {
    first = 0;
    while (first < show.cueCount && show.cues[first].at <= from) {
        ++first;
    }
    last = first;
    while (last < show.cueCount && show.cues[last].at <= to) {
        ++last;
    }
}

} // namespace ftcore
