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
UiPartyRow stepped(UiPartyRow at, int up, bool explore, bool partnerHere) {
    int i = static_cast<int>(at);
    for (int n = 0; n < static_cast<int>(kUiPartyRows); ++n) {
        i -= up; // the rows run top to bottom
        if (i < 0 || i >= static_cast<int>(kUiPartyRows)) return at;
        const UiPartyRow r = static_cast<UiPartyRow>(i);
        if (uiPartyRowSelectable(r, explore, partnerHere)) return r;
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

bool uiPartyRowSelectable(UiPartyRow r, bool explore, bool partnerHere) {
    switch (r) {
    case UiPartyRow::Third: return false;
    case UiPartyRow::Partner: return partnerHere;
    case UiPartyRow::Lead: return true;
    case UiPartyRow::Follow: return explore;
    }
    return false;
}

UiPartyOut uiPartyScreenStep(UiPartyScreen& s, const UiPartyInput& in, float dt) {
    UiPartyOut out;
    s.partnerHere = in.partnerHere;
    // The partner gone from the fight while the cursor rests on its row: back to the lead's, so nothing is picked.
    if (s.open && !uiPartyRowSelectable(s.cursor, s.explore, s.partnerHere)) s.cursor = UiPartyRow::Lead;
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
    if (up != 0) s.cursor = stepped(s.cursor, up, s.explore, s.partnerHere);

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

bool uiPartyProbeInput(float t, UiPartyInput& in) {
    if (!(t >= kUiPartyProbeFrom && t < kUiPartyProbeTo)) return false;
    in.live = true;
    in.explore = false;
    in.toggle = false;
    in.mapDown = false;
    in.mouseY = 0.f;
    in.partyDown = (t >= kUiPartyProbeHold && t < 0.f) || (t >= kUiPartyProbeReopen && t < kUiPartyProbeReopenEnd);
    in.stickY = t >= kUiPartyProbeFlick && t < kUiPartyProbeFlickEnd ? 1.f : 0.f;
    return true;
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

UiPartyWhy uiPartyWhyOf(LeadSwitch r) {
    switch (r) {
    case LeadSwitch::Ok:
    case LeadSwitch::Same: return UiPartyWhy::Ok;
    case LeadSwitch::NotInFight:
    case LeadSwitch::Lone: return UiPartyWhy::NotHere;
    case LeadSwitch::Down: return UiPartyWhy::Down;
    case LeadSwitch::FightOver: return UiPartyWhy::FightOver;
    case LeadSwitch::Cooldown: return UiPartyWhy::Wait;
    }
    return UiPartyWhy::Unknown;
}

UiPartyRowView uiPartyRowView(const UiPartyScreen& s, UiPartyRow r, UiPartyWhy why, float nameW) {
    UiPartyRowView v;
    if (!s.open) return v;
    if (r == UiPartyRow::Third) {
        v.ink = kUiPartyThirdInk;
        return v;
    }
    const bool leadRow = r == UiPartyRow::Partner || r == UiPartyRow::Lead;
    const bool cursor = s.cursor == r && uiPartyRowSelectable(r, s.explore, s.partnerHere);
    v.ink = leadRow && uiPartyRefused(why) ? kUiPartyRefusedInk : cursor ? 1.f : kUiPartyOtherInk;
    v.underlineW = cursor && nameW > 0.f ? nameW : 0.f;
    v.dot = r == UiPartyRow::Lead;
    return v;
}

UiPartyDot uiPartyDot(float nameX, float capMid, float capH) {
    UiPartyDot d;
    const float h = capH > 0.f ? capH : 0.f;
    d.r = 0.5f * kUiPartyDotCap * h;
    d.x = nameX - kUiPartyDotGap * h - d.r;
    d.y = capMid;
    return d;
}

} // namespace ftcore
