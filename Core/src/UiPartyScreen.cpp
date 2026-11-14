#include "ftcore/UiPartyScreen.h"

namespace ftcore {

namespace {

float absOf(float v) { return v < 0.f ? -v : v; }

// One flick from a stick or the mouse: -1 down, +1 up, 0 none. Arms once back under `rest`.
int flick(bool& armed, float v, float out, float rest) {
    if (!armed) {
        if (absOf(v) < rest) armed = true;
        return 0;
    }
    if (absOf(v) > out) {
        armed = false;
        return v > 0.f ? 1 : -1;
    }
    return 0;
}

// The cursor one row up (+1) or down (-1), over the rows it may rest on; past the ends it stays.
UiPartyRow stepped(UiPartyRow at, int up, bool explore) {
    int i = static_cast<int>(at);
    for (int n = 0; n < static_cast<int>(kUiPartyRows); ++n) {
        i -= up; // the rows run top to bottom
        if (i < 0 || i >= static_cast<int>(kUiPartyRows)) return at;
        const UiPartyRow r = static_cast<UiPartyRow>(i);
        if (uiPartyRowSelectable(r, explore)) return r;
    }
    return at;
}

void openScreen(UiPartyScreen& s, bool explore) {
    s.open = true;
    s.explore = explore;
    s.cursor = UiPartyRow::Lead;
    s.opening = true;
    s.stickArmed = false;
    s.mouseArmed = false;
}

void closeScreen(UiPartyScreen& s, UiPartyOut& out, UiPartyRow pick) {
    s.open = false;
    s.opening = false;
    out.closed = true;
    out.pick = pick;
}

} // namespace

bool uiPartyRowSelectable(UiPartyRow r, bool explore) {
    switch (r) {
    case UiPartyRow::Third: return false;
    case UiPartyRow::Partner:
    case UiPartyRow::Lead: return true;
    case UiPartyRow::Follow: return explore;
    }
    return false;
}

UiPartyOut uiPartyScreenStep(UiPartyScreen& s, const UiPartyInput& in, float dt) {
    UiPartyOut out;
    if (!in.live) {
        // Taken away: shut with no pick; a key still down when input returns is no press, and its release no map.
        if (s.open) closeScreen(s, out, UiPartyRow::Lead);
        s.partyWas = s.mapWas = true;
        s.mapHeld = 0.f;
        s.mapUsed = true;
        return out;
    }
    const bool partyPress = in.partyDown && !s.partyWas;
    const bool mapPress = in.mapDown && !s.mapWas;
    const bool mapRelease = !in.mapDown && s.mapWas;
    const bool anyDown = in.partyDown || in.mapDown;
    s.partyWas = in.partyDown;
    s.mapWas = in.mapDown;
    if (mapPress) {
        s.mapHeld = 0.f;
        s.mapUsed = false;
    }

    if (!s.open) {
        if (partyPress || (mapPress && !in.explore)) {
            openScreen(s, in.explore);
            out.opened = true;
            s.mapUsed = true;
        } else if (in.explore && in.mapDown && !s.mapUsed) {
            s.mapHeld += dt > 0.f ? dt : 0.f;
            if (s.mapHeld >= kUiPartyHold) {
                openScreen(s, true);
                out.opened = true;
                s.mapUsed = true;
            }
        }
        if (mapRelease) {
            out.map = in.explore && !s.mapUsed && !out.opened;
            s.mapHeld = 0.f;
            s.mapUsed = true;
        }
        if (!out.opened) return out;
    } else {
        s.mapUsed = true; // the map key while the screen stands is the screen's
    }

    // Open: the cursor moves one row per flick (the stick's first, else the mouse's).
    int up = flick(s.stickArmed, in.stickY, kUiPartyStickOut, kUiPartyStickRest);
    const int mouseUp = flick(s.mouseArmed, in.mouseY, kUiPartyMouseOut, kUiPartyMouseRest);
    if (up == 0) up = mouseUp;
    if (up != 0) s.cursor = stepped(s.cursor, up, s.explore);

    if (out.opened) return out; // the opening tick never confirms
    if (s.opening) {
        if (!anyDown) {
            s.opening = false;
            if (!in.toggle) closeScreen(s, out, s.cursor);
        }
        return out;
    }
    // Toggle: the opening press was let go; the next press confirms.
    if (in.toggle ? (partyPress || mapPress) : !anyDown) closeScreen(s, out, s.cursor);
    return out;
}

bool uiPartyRefused(UiPartyWhy w) {
    switch (w) {
    case UiPartyWhy::Unknown:
    case UiPartyWhy::Ok: return false;
    case UiPartyWhy::Down:
    case UiPartyWhy::NotHere:
    case UiPartyWhy::FightOver:
    case UiPartyWhy::Wait: return true;
    }
    return false;
}

UiPartyWhy uiPartyWhyOf(SwitchRefusal r) {
    switch (r) {
    case SwitchRefusal::None: return UiPartyWhy::Unknown;
    case SwitchRefusal::Down: return UiPartyWhy::Down;
    case SwitchRefusal::Defeated: return UiPartyWhy::FightOver;
    }
    return UiPartyWhy::Unknown;
}

} // namespace ftcore
