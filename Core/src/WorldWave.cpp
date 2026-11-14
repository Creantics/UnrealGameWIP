#include "ftcore/WorldWave.h"

#include <cmath>

namespace ftcore {

namespace {

float waveClamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float waveDistance(const WaveVec3& a, const WaveVec3& b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float dz = b.z - a.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// tau1: the time the front takes to reach fastRadius.
float waveFastPhaseTime(const WorldWaveParams& params) {
    return params.fastSpeed > 0.f ? params.fastRadius / params.fastSpeed : 0.f;
}

} // namespace

const char* worldWaveCueName(WorldWaveCue cue) {
    switch (cue) {
    case WorldWaveCue::Prelude: return "Prelude";
    case WorldWaveCue::FrontPassedListener: return "FrontPassedListener";
    case WorldWaveCue::Settled: return "Settled";
    }
    return "Unknown";
}

float worldWaveRadiusAt(const WorldWaveParams& params, float tau) {
    if (tau <= 0.f) return 0.f;
    const float tau1 = waveFastPhaseTime(params);
    float r = 0.f;
    if (tau <= tau1) {
        r = params.fastSpeed * tau;
    } else {
        const float u = tau - tau1;
        const float k = params.easeTime;
        const float ease = k > 0.f ? (params.fastSpeed - params.slowSpeed) * k * (1.f - std::exp(-u / k)) : 0.f;
        r = params.fastRadius + params.slowSpeed * u + ease;
    }
    return waveClamp(r, 0.f, params.maxRadius);
}

float worldWaveBandWidth(const WorldWaveParams& params, float radius) {
    const float share = params.bandFarRadius > 0.f ? waveClamp(radius / params.bandFarRadius, 0.f, 1.f) : 1.f;
    return params.bandAtOrigin + (params.bandAtFar - params.bandAtOrigin) * share;
}

WorldWaveParams streetWaveParams() {
    WorldWaveParams p;
    p.prelude = 0.3f;
    p.fastSpeed = 40.f;
    p.fastRadius = 20.f;
    p.slowSpeed = 17.5f;
    p.easeTime = 0.5f;
    p.bandAtOrigin = 2.f;
    p.bandAtFar = 5.f;
    p.bandFarRadius = 60.f;
    p.tongueLead = 1.f;
    return p;
}

WorldWaveFrame evaluateWorldWave(const WorldWaveParams& params, double time) {
    WorldWaveFrame frame;
    frame.direction = params.direction;
    const double since = time - params.startTime;
    if (since < 0.0) return frame;
    frame.preludeProgress =
        params.prelude > 0.f ? waveClamp(static_cast<float>(since / static_cast<double>(params.prelude)), 0.f, 1.f) : 1.f;
    const float tau = static_cast<float>(since - static_cast<double>(params.prelude));
    frame.radius = worldWaveRadiusAt(params, tau);
    frame.bandWidth = worldWaveBandWidth(params, frame.radius);
    frame.settled = frame.radius >= params.maxRadius;
    frame.active = !frame.settled;
    return frame;
}

WorldWaveStep updateWorldWave(WorldWaveState& state, const WorldWaveParams& params, double time,
                              const WaveVec3& viewer) {
    WorldWaveStep step;
    step.frame = evaluateWorldWave(params, time);
    if (time < params.startTime) return step;

    const WorldWaveFrame& frame = step.frame;
    const float d = waveDistance(params.origin, viewer);
    if (!state.preludeFired) {
        state.preludeFired = true;
        step.cues |= worldWaveCueBit(WorldWaveCue::Prelude);
    }
    if (!state.frontFired && frame.radius > d) {
        state.frontFired = true;
        step.cues |= worldWaveCueBit(WorldWaveCue::FrontPassedListener);
    }
    if (!state.settledFired && frame.settled) {
        state.settledFired = true;
        step.cues |= worldWaveCueBit(WorldWaveCue::Settled);
    }

    // The inner edge's lead on the viewer; it passes the viewer where this turns from negative to zero. Between the
    // two steps that straddle it the crossing is placed by linear interpolation, exact while the front moves at
    // constant speed and close enough after.
    const float margin = (frame.radius - frame.bandWidth) - d;
    if (!state.crossed && frame.radius > 0.f && margin >= 0.f) {
        state.crossed = true;
        state.crossTime = time;
        if (state.hasLast && state.lastMargin < 0.f && time > state.lastTime) {
            const double share = static_cast<double>(-state.lastMargin) / static_cast<double>(margin - state.lastMargin);
            state.crossTime = state.lastTime + (time - state.lastTime) * share;
        }
    }
    state.hasLast = true;
    state.lastTime = time;
    state.lastMargin = margin;

    if (state.crossed) {
        step.frame.cameraInside =
            params.insideRamp > 0.f
                ? waveClamp(static_cast<float>((time - state.crossTime) / static_cast<double>(params.insideRamp)), 0.f, 1.f)
                : 1.f;
    }
    return step;
}

WaveClass classifyWorldWave(const WorldWaveFrame& frame, const WaveVec3& origin, const WaveVec3& point) {
    const float d = waveDistance(origin, point);
    const float r = frame.radius;
    const float w = frame.bandWidth;
    const bool forward = frame.direction == WaveDirection::Forward;
    WaveClass c;
    if (r <= 0.f || d > r) {
        c.side = forward ? WaveSide::Old : WaveSide::New;
        c.band = 0.f;
    } else if (d < r - w) {
        c.side = forward ? WaveSide::New : WaveSide::Old;
        c.band = 1.f;
    } else {
        c.side = WaveSide::Band;
        c.band = w > 0.f ? waveClamp((r - d) / w, 0.f, 1.f) : 0.f;
    }
    return c;
}

WorldTagState worldTagState(WorldTag tag, const WaveClass& where, WaveDirection direction) {
    WorldTagState s;
    if (tag == WorldTag::Both) return s;
    // The share of the band the second world has taken: the band value runs 0 at the outer edge to 1 at the inner
    // edge, and the second world is inside for Forward, outside for Reverse.
    float newness = 0.f;
    if (where.side == WaveSide::New) {
        newness = 1.f;
    } else if (where.side == WaveSide::Band) {
        newness = direction == WaveDirection::Forward ? where.band : 1.f - where.band;
    }
    newness = waveClamp(newness, 0.f, 1.f);
    if (tag == WorldTag::OldOnly) {
        s.cutValue = newness;
        s.drawn = where.side != WaveSide::New;
        s.collisionOn = where.side != WaveSide::New;
    } else {
        s.cutValue = 1.f - newness;
        s.drawn = where.side != WaveSide::Old;
        s.collisionOn = where.side == WaveSide::New;
    }
    return s;
}

WorldWaveThingStep updateWorldWaveThing(WorldWaveThing& thing, const WorldWaveParams& params,
                                        const WorldWaveFrame& frame, double time, bool pawnInFootprint) {
    // Steps at exact multiples of a frame time land a hair under passTime + delay; this keeps them on the frame due.
    constexpr double kTimeSlack = 1e-6;
    const bool forward = params.direction == WaveDirection::Forward;
    WaveClass outer;
    outer.side = forward ? WaveSide::Old : WaveSide::New;
    outer.band = 0.f;
    WaveClass inner;
    inner.side = forward ? WaveSide::New : WaveSide::Old;
    inner.band = 1.f;
    const WorldTagState before = worldTagState(thing.tag, outer, params.direction);
    const WorldTagState after = worldTagState(thing.tag, inner, params.direction);
    const bool arriving = !before.drawn && after.drawn;
    const bool leaving = before.drawn && !after.drawn;

    const float d = waveDistance(params.origin, thing.pivot);
    const float r = time >= params.startTime ? frame.radius : 0.f;
    const float reach = arriving && r > 0.f ? r + params.tongueLead * frame.bandWidth : r;
    if (!thing.arrived && r > 0.f && reach > d - thing.radius) {
        thing.arrived = true;
        if (arriving && pawnInFootprint) thing.held = true;
    }
    if (!thing.passed && r > 0.f && r > d) {
        thing.passed = true;
        thing.passTime = time;
    }
    if (thing.held && !pawnInFootprint) {
        thing.held = false;
        thing.released = true;
        thing.releaseTime = time;
    }

    WorldWaveThingStep step;
    if (thing.released && !thing.localDone) {
        const float nearSide = d - thing.radius > 0.f ? d - thing.radius : 0.f;
        const float local = nearSide + params.slowSpeed * static_cast<float>(time - thing.releaseTime);
        const float farSide = d + thing.radius + frame.bandWidth;
        // Done once past the far side, or once it has caught the wave's own front (then the two agree).
        if (local >= farSide || local >= r) {
            thing.localDone = true;
        } else {
            step.localFront = true;
            step.localRadius = local;
        }
        if (!thing.localPassed && (local > d || thing.localDone)) {
            thing.localPassed = true;
            thing.localPassTime = time;
        }
    }

    const double delay = static_cast<double>(params.collisionDelay);
    bool want = before.collisionOn;
    if (thing.held) {
        want = false;
    } else if (thing.released) {
        if (thing.localPassed && time + kTimeSlack >= thing.localPassTime + delay) want = after.collisionOn;
    } else if (thing.passed && time + kTimeSlack >= thing.passTime + delay) {
        want = after.collisionOn;
    }
    // Collision never comes on around a pawn: it waits until the footprint is clear. (Not on the first step, which
    // only reports the state the thing starts the wave in.)
    if (want && thing.collisionKnown && !thing.collisionOn && pawnInFootprint) want = false;

    step.collisionChanged = !thing.collisionKnown || want != thing.collisionOn;
    thing.collisionKnown = true;
    thing.collisionOn = want;
    step.collisionOn = want;
    step.held = thing.held;
    if (arriving) {
        step.drawn = thing.arrived && !thing.held;
    } else if (leaving) {
        step.drawn = !(r > 0.f && r - frame.bandWidth > d + thing.radius);
    } else {
        step.drawn = before.drawn || after.drawn;
    }
    return step;
}

} // namespace ftcore
