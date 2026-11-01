#pragma once

// The beat boss as a choreographed fight scene (docs/BEAT_BOSS_DEMO.md, "Choreography"). BeatBoss
// (BeatBoss.h) stays the judge: it pins every boss move to a bar and a beat and scores the answers.
// BeatChoreo only reads it. From the boss's events it works out what the two fighters play, where the
// camera is and which prompt shows where, all as a function of track time, so the host plays the
// animation clips, cuts the cameras and pops the prompts without any logic of its own.
//
//   ftcore::BeatClips clips;  importBeatClips(text, clips);                       // Data/Anim/beat_clips.json
//   ftcore::BeatChoreoDef def; importBeatChoreo(text, track, clips, def);          // <track>.choreo.json
//   ftcore::BeatChoreo scene;
//   scene.begin(def, clips);                       // with fight.begin(...)
//   every frame:  events = fight.update(clock);  scene.observe(fight, events, clock);
//                 scene.sampleNow(s);              // s.lead, s.boss: clip, time, rate; s.shot; s.prompts
//   after each press / release: scene.observe(fight, fight.press(...), eventTime)
//   after fight.retry(clock):   scene.retry(fight)
//
// The host calls observe() after every call that returns events (update, press, release), and at least
// every 0.1 s of the fight's clock: a prompt is remembered from the frame it first shows, and a prompt
// that was opened and answered between two observe calls is not choreographed (unmatched() counts them).
// It also calls it every frame after the fight is over, so the end poses keep time.
//
// Clips (Data/Anim/beat_clips.json). Each has an id, an asset path, a length in seconds, an impact offset (seconds into
// the clip where the blow or contact lands; 0 for a clip with none) and whether it loops. The catalogue
// holds up to kMaxBeatClips; the animation side may add ids. The choreography refers to clips by id in
// data and by index (BeatClips::find) at run time.
//
// Timing. A boss move's clip is started so its impact frame lands on the move's impact, exactly:
//   start = impact - impactOffset / rate
// The rate aims for the clip's windup to span the tell and is clamped to [kChoreoMinRate, kChoreoMaxRate]
// so the motion stays natural. If the windup still does not fit between the previous action's end and
// the impact, the clip is started partway through (at the previous action's end, rate at the top of the
// clamp) instead of playing it impossibly fast (planChoreoClip). The lead's clips start on the judged
// press and run to their end; a blow lands kChoreoLeadLag after the press (a short windup is skipped),
// and a counter pressed early waits so its contact lands on the boss's impact. A miss plays from the
// impact. Between actions an actor loops its stance, started on a beat, its rate set so one cycle is a
// whole number of beats (it bobs on the beat).
//
// Visual time. Hit-stop and slow motion bend only the visuals: the clip times in a sample are read
// at visual time v(t) = t - lag(t), and the fight's clock and the music are never touched.
// * Hit-stop. A Perfect contact pauses the animation for 50 to 80 ms, then plays it faster until the
//   next beat (never across a bar line or a pinned impact), where lag is 0 again.
// * Slow motion. In a finisher bar the animation dips between the strikes: lag is 0 on every strike's
//   impact and on the bar line, so the finisher still lands on the beat.
// So every pinned impact is on screen at its beat. A sample's clip rates are the scheduled ones
// (0 while a finished clip holds its last frame); `visualRate` is the bend on top (0 in a hit-stop).
//
// Shots (ChoreoShot) are ids the host maps to cameras. A cut happens on a beat or bar boundary, never
// between: at the beat holding a move's tell, to that move's shot. A finisher bar cuts to finisher_orbit
// on its bar line. A stagger, the victory and the defeat cut on the next beat. A silence cuts nothing
// and holds the shot; the actors loop their stance through it, and through the counts.
//
// Prompts. The prompts of the sample are the open moves whose tell has been reached, in impact order:
// each with its move, its answer, the anchor it sits by and its lead-in progress (0 at the tell, 1 at
// the impact). Prompts[0] is the one to answer next.
//
// Retry. retry() (or observe() seeing the fight's retry count rise) clears everything: the choreography
// then begins again at the checkpoint, the same as a fresh start from it.
//
// Data: Data/Battle/beat_boss/<track id>.choreo.json, keyed to the track's moves by index:
//   { "version": 1, "track": "test_track",
//     "stance": {"lead": "lead_stance", "boss": "boss_stance"}, "opening": "wide",
//     "finisher": {"slow": 0.4},
//     "stagger": {"lead": "lead_cross", "boss": "boss_stagger", "shot": "close_boss"},
//     "victory": {...}, "defeat": {...},                     // the same three keys
//     "phases": [ { "id": "phase_a",
//                   "moves": [ {"id": "a_parry", "boss": "boss_hook", "ok": "lead_parry", "then": "lead_counter",
//                               "miss": "lead_hitreact", "hit": "boss_hitreact",
//                               "shot": "over_lead", "anchor": "boss_fist_r"} ],
//                   "finisher": [ ... ] } ] }
// "moves" and "finisher" list the phase's moves in the track's order (the id is checked). `boss` is the
// boss's clip for the move (its impact lands on the move's impact; omit it for none), `ok` the lead's
// clip when the move is answered, `then` the lead's follow-up after a Perfect, `miss` the lead's clip
// when it is not, `hit` the boss's reaction when the lead's blow lands. Ids and numbers only.
//
// Deterministic for the same inputs. Loading allocates; nothing else does (fixed capacity, results by
// value).

#include "ftcore/Api.h"
#include "ftcore/BeatBoss.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

inline constexpr std::size_t kMaxBeatClips = 128;
inline constexpr std::size_t kMaxClipId = 31;   // characters
inline constexpr std::size_t kMaxClipAsset = 127; // characters of an asset path
inline constexpr std::int16_t kNoClip = -1;
inline constexpr double kChoreoMinRate = 0.75;
inline constexpr double kChoreoMaxRate = 1.35;
inline constexpr double kChoreoLeadLag = 0.06;  // a blow lands this long after the press
inline constexpr double kChoreoReactionLag = 0.02; // the boss reacts at least this long after its own impact
inline constexpr std::size_t kChoreoPrompts = 4; // shown at once

// ---- the clip catalogue ----

struct BeatClip {
    std::array<char, kMaxClipId + 1> idText{};
    std::array<char, kMaxClipAsset + 1> assetText{}; // the host's asset path for the clip ("" if none)
    double length = 1.0;
    double impact = 0.0; // seconds into the clip; 0: none
    bool loop = false;

    std::string_view id() const { return std::string_view(idText.data()); }
    std::string_view asset() const { return std::string_view(assetText.data()); }
};

struct BeatClips {
    std::array<BeatClip, kMaxBeatClips> items{};
    std::size_t count = 0;

    // The index of the clip with this id, or -1.
    FTCORE_API int find(std::string_view id) const;
    const BeatClip& operator[](std::size_t i) const { return items[i]; }
    std::string_view id(std::size_t i) const { return items[i].id(); }
    std::string_view asset(std::size_t i) const { return items[i].asset(); }
};

// Data/Anim/beat_clips.json: { "version": 1, "clips": [ {"id": "lead_stance", "asset":
// "/Game/Combat/BeatAnims/Lead/lead_stance", "length_s": 2.0, "impact_s": 0, "loop": true} ] }. Ids
// are 1..31 letters, digits, _ and -; the asset (optional) is a path of 1..127 characters beginning
// with /, which the host loads; the length is over 0 and at most 60 s; the impact is within the clip. Unknown keys are warnings. On failure `out` is empty
// and *error lists each problem with its path, one per line.
FTCORE_API bool importBeatClips(std::string_view json, BeatClips& out, std::string* error = nullptr,
                                std::vector<std::string>* warnings = nullptr);

// ---- shots and anchors ----

// The cameras the host maps (append only). Data: "wide", "over_lead", "over_boss", "close_lead",
// "close_boss", "low_hero", "top_down", "finisher_orbit".
enum class ChoreoShot : std::uint8_t {
    Wide, OverLead, OverBoss, CloseLead, CloseBoss, LowHero, TopDown, FinisherOrbit
};
inline constexpr std::size_t kChoreoShotCount = 8;
FTCORE_API const char* choreoShotKey(ChoreoShot s);
FTCORE_API bool parseChoreoShot(std::string_view key, ChoreoShot& out);

// Which actor and body point a prompt sits by. Data: "boss_head", "boss_fist_r", "boss_fist_l",
// "boss_chest", "lead_head", "lead_chest", "screen_center".
enum class ChoreoAnchor : std::uint8_t {
    BossHead, BossFistR, BossFistL, BossChest, LeadHead, LeadChest, ScreenCenter
};
inline constexpr std::size_t kChoreoAnchorCount = 7;
FTCORE_API const char* choreoAnchorKey(ChoreoAnchor a);
FTCORE_API bool parseChoreoAnchor(std::string_view key, ChoreoAnchor& out);

// ---- the choreography, as data ----

// One track move's choreography. Clips are indices into the catalogue (kNoClip: none).
struct ChoreoMove {
    std::int16_t boss = kNoClip; // the boss's clip: its impact lands on the move's impact
    std::int16_t ok = kNoClip;   // the lead's clip on a Perfect or Good
    std::int16_t then = kNoClip; // the lead's follow-up after a Perfect
    std::int16_t miss = kNoClip; // the lead's clip on a Miss
    std::int16_t hit = kNoClip;  // the boss's reaction when the lead's blow lands
    ChoreoShot shot = ChoreoShot::Wide;
    ChoreoAnchor anchor = ChoreoAnchor::BossChest;
};

// A scripted moment (the stagger, the victory, the defeat).
struct ChoreoBeat {
    std::int16_t lead = kNoClip;
    std::int16_t boss = kNoClip;
    ChoreoShot shot = ChoreoShot::Wide;
    bool cut = false; // the shot is given
};

struct ChoreoPhase {
    std::array<ChoreoMove, kMaxBossMoves> moves{};
    std::uint8_t moveCount = 0;
    std::array<ChoreoMove, kMaxBossFinisherMoves> finisher{};
    std::uint8_t finisherCount = 0;
};

struct BeatChoreoDef {
    std::int16_t stanceLead = kNoClip;
    std::int16_t stanceBoss = kNoClip;
    ChoreoShot opening = ChoreoShot::Wide;
    float finisherSlow = 0.4f; // the animation's speed, at its slowest, in a finisher bar
    ChoreoBeat stagger, victory, defeat;
    std::array<ChoreoPhase, kMaxBossPhases> phases{};
    std::uint8_t phaseCount = 0;
};

struct BeatChoreoTuning {
    double hitStopStrong = 0.080; // a Perfect blow (Attack, Hold, Release, Parry, Paired)
    double hitStopLight = 0.050;  // a Perfect Guard
    double catchUp = 4.0;         // the catch-up after a hit-stop lasts at least this many stops
    double followThrough = 0.20;  // the most of a boss clip's recovery kept before its next move
};

// On failure `out` is empty and *error lists each problem with its path, one per line. Checks the
// choreography against the track (the phases, the moves in order with their ids) and the catalogue.
FTCORE_API bool importBeatChoreo(std::string_view json, const BeatBossTrack& track, const BeatClips& clips,
                                 BeatChoreoDef& out, std::string* error = nullptr,
                                 std::vector<std::string>* warnings = nullptr);

// ---- timing ----

// Where a clip starts so its impact frame lands on `impactTime`. The clip plays from `offset` seconds
// in, at `rate`, from `start`: start + (impact - offset) / rate == impactTime.
struct ChoreoPlan {
    double start = 0.0;
    double offset = 0.0;
    double rate = 1.0;
};
// `desiredStart` is when the windup would like to begin (the tell): the rate is the one that fits the
// clip's windup between it and the impact, clamped to the rate range. `earliest` is when the actor is
// free: if the clip cannot start by then, it starts partway through instead. A clip with no impact
// offset starts at max(desiredStart, earliest).
FTCORE_API ChoreoPlan planChoreoClip(const BeatClip& clip, double impactTime, double desiredStart, double earliest);

// A stance clip's rate so one cycle is a whole number of beats, within the clamp where one exists.
FTCORE_API double choreoStanceRate(const BeatClip& stance, double beatSeconds);

// ---- the sample ----

struct ChoreoPose {
    std::int16_t clip = kNoClip;
    double time = 0.0; // seconds into the clip (looping clips wrap)
    float rate = 1.0f; // 0: a finished clip holding its last frame
};

struct ChoreoPromptView {
    std::uint32_t id = 0;
    std::uint8_t phase = 0;
    std::uint8_t move = 0;       // index into the phase's moves (or finisher)
    bool finisher = false;
    BeatBossAnswer answer = BeatBossAnswer::Attack;
    ChoreoAnchor anchor = ChoreoAnchor::BossChest;
    float progress = 0.0f;       // 0 at the tell, 1 at the impact
};

struct ChoreoSample {
    ChoreoPose lead, boss;
    ChoreoShot shot = ChoreoShot::Wide;
    double sinceCut = 0.0;       // seconds since the shot was cut to
    std::array<ChoreoPromptView, kChoreoPrompts> prompts{};
    std::size_t promptCount = 0;
    bool hitStop = false;        // the animation is paused for a hit
    float slowMo = 1.0f;         // the finisher's slow motion (1: none)
    float visualRate = 1.0f;     // the visuals' bend on top of the clip rates: 0 in a hit-stop
    bool frozen = false;         // the track is stopped (a count or a silence): the actors loop their stance
    bool over = false;           // the fight is won or lost

    const ChoreoPromptView* prompt() const { return promptCount > 0 ? &prompts[0] : nullptr; }
};

struct ChoreoCut {
    double time = 0.0;
    ChoreoShot shot = ChoreoShot::Wide;
};

struct ChoreoBar {
    double start = 0.0;
    double beat = 0.5;  // seconds
    double end = 0.0;
};

class BeatChoreo {
public:
    // Starts a scene: `def` and `clips` are copied. Call it with fight.begin().
    FTCORE_API void begin(const BeatChoreoDef& def, const BeatClips& clips, const BeatChoreoTuning& tuning = {});
    // Back to the checkpoint: call it after fight.retry(). (observe() also sees it.)
    FTCORE_API void retry(const BeatBoss& fight);
    // Feeds the events a call of `fight` returned. `clock` is the fight's clock the call was made at.
    FTCORE_API void observe(const BeatBoss& fight, const BeatBossEvents& events, double clock);
    // What the scene shows at track time `trackTime` (the fight's trackTime()). Const and
    // allocation-free; the same state and time give the same sample.
    FTCORE_API void sample(double trackTime, ChoreoSample& out) const;
    // sample() at the scene's own time: the fight's track time while it runs, and, after the fight is
    // over (when its track stops), the time since the end.
    FTCORE_API void sampleNow(ChoreoSample& out) const;
    FTCORE_API double trackNow() const;

    bool begun() const { return begun_; }
    const BeatClips& clips() const { return clips_; }
    const BeatChoreoDef& def() const { return def_; }
    const BeatChoreoTuning& tuning() const { return tuning_; }
    // Prompts that opened and closed between two observe calls (so were not choreographed).
    std::uint32_t unmatched() const { return unmatched_; }
    // Hit-stops started so far.
    std::uint32_t hitStops() const { return hitStops_; }

    // The cuts scheduled (the newest kMaxCuts, oldest first) and the bars seen: for tools and tests.
    static constexpr std::size_t kMaxCuts = 32;
    static constexpr std::size_t kMaxBars = 8;
    std::size_t cutCount() const { return cutCount_; }
    const ChoreoCut& cut(std::size_t i) const { return cuts_[i]; }
    std::size_t barCount() const { return barCount_; }
    const ChoreoBar& bar(std::size_t i) const { return bars_[i]; }
    // The next beat or bar boundary after t (or at or after it), in track time.
    FTCORE_API double boundary(double t, bool atOrAfter) const;

private:
    static constexpr std::size_t kMaxSegs = 32;
    static constexpr std::size_t kMaxWarps = 16;
    enum class SegKind : std::uint8_t { Attack, Reaction };
    struct Seg {
        std::int16_t clip = kNoClip;
        SegKind kind = SegKind::Reaction;
        double start = 0.0, offset = 0.0, rate = 1.0;
        double impact = 0.0; // Attack: the pinned impact
        double free = 0.0;   // Attack: when the actor is free for its next move
        std::uint32_t prompt = 0;
    };
    struct Actor {
        std::array<Seg, kMaxSegs> segs{};
        std::size_t count = 0;
        double anchor = 0.0; // the stance's beat
    };
    struct PromptRec {
        bool open = false;
        std::uint32_t id = 0;
        std::uint8_t phase = 0, move = 0;
        bool finisher = false;
        BeatBossAnswer answer = BeatBossAnswer::Attack;
        ChoreoAnchor anchor = ChoreoAnchor::BossChest;
        double tell = 0.0, impact = 0.0;
        double press = -1.0;
    };
    enum class WarpKind : std::uint8_t { Stop, Dip };
    struct Warp {
        WarpKind kind = WarpKind::Stop;
        double t0 = 0.0, t1 = 0.0;
        double amount = 0.0; // Stop: the pause; Dip: the deepest lag
    };

    void resetRun(const BeatBoss& fight, double clock);
    void handle(const BeatBoss& fight, const BeatBossEvent& e, double te);
    void barStart(const BeatBoss& fight, int number, double te);
    void judged(const BeatBossEvent& e, double te);
    void bossHit(double te);
    void forceEnd(const ChoreoBeat& beat, double te, bool victory);
    void registerPrompt(const BeatBossPrompt& p, double te);
    PromptRec* findRec(std::uint32_t id);
    const ChoreoMove* moveOf(const PromptRec& r) const;
    const BeatClip* clipOf(std::int16_t clip) const;
    void addSeg(Actor& a, const Seg& s);
    void removeFuture(Actor& a, double t);
    void removePrompt(Actor& a, std::uint32_t prompt, double t);
    void settle(double t);
    double bossFree(double impact) const;
    double segEnd(const Seg& s) const;
    void addCut(double t, ChoreoShot shot);
    void addStop(double t, double seconds);
    void addDip(double t0, double t1);
    double lagAt(double t, double* slope, bool* stop) const;
    double beatAt(double t) const;
    double floorBoundary(double t) const;
    void poseOf(const Actor& a, std::int16_t stance, double v, ChoreoPose& out) const;
    void stancePose(std::int16_t stance, double clockTime, double beat, ChoreoPose& out) const;
    void prune();

    BeatChoreoDef def_{};
    BeatClips clips_{};
    BeatChoreoTuning tuning_{};
    bool begun_ = false;
    bool synced_ = false;
    std::uint32_t unmatched_ = 0;
    std::uint32_t hitStops_ = 0;
    int retries_ = 0;

    // Clocks. The track clock is walked along the events (it runs only while the track does) and
    // set to the fight's own at the end of every call.
    double clock_ = 0.0;
    double track_ = 0.0;
    double cursorClock_ = 0.0, cursorTrack_ = 0.0;
    bool running_ = false;
    bool frozen_ = true;
    double frozenSince_ = -1e30;
    bool ended_ = false;
    double endTrack_ = 0.0, endClock_ = 0.0;
    int phase_ = 0;
    bool newPhaseBar_ = false;
    bool finisherNext_ = false;
    double barBeat_ = 0.5;

    std::array<Actor, 2> actors_{}; // 0 the lead, 1 the boss
    std::array<PromptRec, kMaxBossPrompts> recs_{};
    std::array<ChoreoBar, kMaxBars> bars_{};
    std::size_t barCount_ = 0;
    std::array<ChoreoCut, kMaxCuts> cuts_{};
    std::size_t cutCount_ = 0;
    std::array<Warp, kMaxWarps> warps_{};
    std::size_t warpCount_ = 0;

    // The lead's last Judged answer, for the BossHit that follows it.
    bool pending_ = false;
    std::uint32_t pendingPrompt_ = 0;
    double pendingContact_ = 0.0;
    std::int16_t pendingHit_ = kNoClip;
    double lastLeadEnd_ = 0.0;
    double lastBossContact_ = 0.0;
    double staggerUntil_ = -1e30;
};

} // namespace ftcore
