#pragma once

// Which face speaks a voiced story line, the run's tally of voiced lines and their faces, and the
// -StoryShot debug capture's name and count rules. Header-only, no allocation.
//
// A voiced line's speaker is cast as a model (Data/Voice/speakers.json). The line's lip-sync track
// plays on the faces in the map whose model is the speaker's; when there is none, on the map's
// stand-in faces (a face that speaks for any cast speaker, such as the dialogue map's preview).
//
//   switch (ftcore::pickFaceRoute(cast, ownFaces, standIns)) { ... }
//   tally.add(route, played);          // played: a track started on a face with bound shapes
//   tally.format(buffer, sizeof buffer); // "Story: FACES ..." (counts only)

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace ftcore {

enum class FaceRoute : std::uint8_t {
    Uncast,  // the speaker has no model in the cast file
    Own,     // faces of the speaker's model are in the map
    StandIn, // none are; a stand-in face speaks
    NoFace,  // cast, but no face of that model and no stand-in in the map
};

inline FaceRoute pickFaceRoute(bool cast, std::uint32_t ownFaces, std::uint32_t standIns) {
    if (!cast) return FaceRoute::Uncast;
    if (ownFaces > 0) return FaceRoute::Own;
    if (standIns > 0) return FaceRoute::StandIn;
    return FaceRoute::NoFace;
}

struct FaceLineTally {
    std::uint32_t voiced = 0;  // lines whose voice started
    std::uint32_t own = 0, standIn = 0, uncast = 0, noFace = 0;
    std::uint32_t played = 0;  // lines whose track played on at least one face with bound shapes
    std::uint32_t noTrack = 0; // cast lines with a face but no track file
    std::uint32_t unseen = 0;  // a track file, but it played on no face with bound shapes (bad file, no shapes)

    // played: the track started on a face with at least one shape bound to its mesh.
    // track: the line has a track file (only counted for routes with a face).
    void add(FaceRoute route, bool track, bool played_) {
        ++voiced;
        switch (route) {
        case FaceRoute::Uncast: ++uncast; return;
        case FaceRoute::NoFace: ++noFace; return;
        case FaceRoute::Own: ++own; break;
        case FaceRoute::StandIn: ++standIn; break;
        }
        if (!track) ++noTrack;
        else if (played_) ++played;
        else ++unseen;
    }

    // "Story: FACES <p> of <v> voiced lines played on a face (<o> own, <s> stand-in, <u> uncast,
    // <n> no face, <t> no track, <x> unseen)"; played + uncast + no face + no track + unseen = voiced. Returns the length written (cut to fit, always null-terminated).
    std::size_t format(char* buffer, std::size_t capacity) const {
        if (!buffer || capacity == 0) return 0;
        const int n = std::snprintf(buffer, capacity,
                                    "Story: FACES %u of %u voiced lines played on a face (%u own, %u stand-in, %u uncast, %u no face, %u no track, %u unseen)",
                                    static_cast<unsigned>(played), static_cast<unsigned>(voiced), static_cast<unsigned>(own),
                                    static_cast<unsigned>(standIn), static_cast<unsigned>(uncast), static_cast<unsigned>(noFace),
                                    static_cast<unsigned>(noTrack), static_cast<unsigned>(unseen));
        if (n < 0) {
            buffer[0] = '\0';
            return 0;
        }
        return static_cast<std::size_t>(n) < capacity ? static_cast<std::size_t>(n) : capacity - 1;
    }
};

// -StoryShot: a name part (the prefix or a line id) is safe when it is 1..64 characters of letters,
// digits, '-' and '_' only: no path, no "..", no slash, colon or drive. The still is written under the
// game's screenshot folder as <prefix>_<line id>.png.
inline bool storyShotNameOk(std::string_view part) {
    if (part.empty() || part.size() > 64) return false;
    for (const char c : part) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

// -StoryShotMax=N: the most stills a run takes. No value: 4. Clamped to 0..64; 0 takes none and the
// run quits cleanly at once.
inline constexpr std::int32_t kStoryShotDefault = 4;
inline constexpr std::int32_t kStoryShotCap = 64;
inline std::int32_t clampStoryShotMax(std::int32_t requested) {
    return requested < 0 ? 0 : (requested > kStoryShotCap ? kStoryShotCap : requested);
}
// The stills left for this map: the clamped request less those the run took already (never below 0).
inline std::int32_t remainingStoryShots(std::int32_t requested, std::int32_t taken) {
    const std::int32_t max = clampStoryShotMax(requested);
    return taken <= 0 ? max : (taken >= max ? 0 : max - taken);
}

} // namespace ftcore
