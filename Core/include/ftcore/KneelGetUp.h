#pragma once

// What an arena skin on its knees shows through one Open/Down spell (an Open chained into a Down is one spell): its
// go-down from the spell's start, the hold, and its get-up, timed so that it stands as the spell ends (never early in a
// hold, whose end is not known until a release).
//
//   ftcore::KneelTrack k[slots];                 // one per enemy slot, with that slot's kneelSince and upAt
//   on the spell's start:  ftcore::kneelStart(k, kneelSince, upAt, now);
//   on the spell's end:    ftcore::kneelEnd(k);
//   each frame in it:      ftcore::KneelFrame f = ftcore::kneelShow(k, kneelSince, upAt, now, left, clips);
//                          draw f.clip, at f.time when f.timed
//
// The get-up starts once fewer than upLength seconds of the spell are left. A go-down still playing then is cut at
// progress p = spell / inLength, and the get-up starts from the same pose, (1 - p) of the way into it (the get-up is the
// go-down played back), for any skin. From there it plays out in real time, as the get-up of a tracking skin (the
// stone_bearer) always does once begun: if the down runs on past it (a riposte turns an opening into a full down), it
// kneels again, or stands out the rest when too little is left to go down and up again. An untracked get-up that was not
// cut is placed by the time left (upLength - left), as before.

namespace ftcore {

enum class KneelClip : unsigned char { GoDown, Hold, GetUp, Stand };

struct KneelClips {
    double inLength = 0.0;  // the go-down clip's length (s; 0 when it has none)
    double upLength = 0.0;  // the get-up clip's length (s; 0 when it has none)
    bool hasIn = false;
    bool hasUp = false;
    bool goingDown = false; // the clip last drawn was the go-down
};

struct KneelFrame {
    KneelClip clip = KneelClip::Hold;
    double time = 0.0;       // seconds into the clip, when timed (the go-down and the get-up)
    bool timed = false;
    bool cut = false;        // the go-down was cut this frame: the get-up starts from its pose
    double cutAt = 0.0;      // the go-down's progress at the cut (0..1)
    bool standsOut = false;  // the get-up just ended and it stands out the rest of the down
    bool kneelsAgain = false; // the get-up just ended and it kneels again (the down ran on)
};

constexpr double kKneelNone = -1e9;

struct KneelTrack {
    bool track = false;        // its get-up, once begun, always plays out in real time (else only after a cut)
    double upStart = kKneelNone; // when the get-up under way began (kKneelNone: none)
    bool stood = false;        // up for the rest of the down
};

void kneelStart(KneelTrack& k, double& kneelSince, double& upAt, double now);
void kneelEnd(KneelTrack& k);
// left: seconds of the spell still to run (+infinity in a hold). Updates kneelSince (a kneel-again) and upAt (how far
// into its get-up it is drawn: upLength once up).
KneelFrame kneelShow(KneelTrack& k, double& kneelSince, double& upAt, double now, double left, const KneelClips& c);

} // namespace ftcore
