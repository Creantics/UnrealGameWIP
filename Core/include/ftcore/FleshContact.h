#pragma once

// Flesh contact: a presser (a hand, a strap, a foot; a capsule, or a sphere as a capsule of zero length) pressed into
// receiver capsules (forearm, thigh...). solve() returns at most kMaxContacts contacts, deepest first, in a fixed array.
// The skin material turns each contact into a dent along -normal with a bulge ring around its edge (profile() below; the
// HLSL mirrors it term for term).
//
// "Depth asked" is the dent's centre depth, in cm, that a contact hands the material:
//     depth = min(pen x (1 - firmness), dentFrac x 2r, r)
// with pen = rp + rr - distance between the two axes (how far the presser's surface is inside the receiver's) and r the
// receiver's radius. A firm region (wrist, shin, skull) gives little; a soft one (forearm, thigh, belly) gives most of
// pen, and never more than dentFrac of the limb's thickness, nor more than the radius (a presser deeper than the limb is
// thick still dents by a bounded amount). The patch radius is the circle where the two surfaces cross:
// sqrt(2 x rp x p - p^2), p = min(pen, rp).
// Plain C++20, header only, no allocation.

#include <cmath>
#include <span>

namespace ftcore::flesh {

struct V3 {
    float x = 0.f, y = 0.f, z = 0.f;
};
constexpr V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
constexpr float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(V3 a) { return std::sqrt(dot(a, a)); }

// A capsule: the segment a..b swept by radius r (cm). a == b is a sphere.
struct Capsule {
    V3 a, b;
    float r = 0.f;
};

// A receiver: a body capsule and how firm it is there, 0 (soft) .. 1 (bone under the skin: no dent).
struct Receiver {
    Capsule c;
    float firmness = 0.f;
};

struct Contact {
    V3 point;           // on the receiver's surface, under the presser
    V3 normal;          // unit, out of the receiver toward the presser; the dent goes along -normal
    float radius = 0.f; // patch radius, cm
    float depth = 0.f;  // depth asked, cm (see the top of this file)
    int receiver = -1;  // index into the receivers passed
    int presser = -1;   // index into the pressers passed
};

inline constexpr int kMaxContacts = 4;

struct Contacts {
    Contact c[kMaxContacts];
    int count = 0;   // contacts kept, <= kMaxContacts
    int touching = 0; // pairs that overlapped, kept or not
};

struct Params {
    float dentFrac = 0.15f; // the deepest dent, as a fraction of the receiver's thickness (2r)
};

constexpr float clamp01(float v) { return v > 0.f ? (v < 1.f ? v : 1.f) : 0.f; } // NaN reads as 0

// Closest points between segments p1..q1 and p2..q2 (Ericson, Real-Time Collision Detection 5.1.9): s, t in 0..1.
// Degenerate (zero-length) and parallel segments are handled; never divides by zero.
inline void closestOnSegments(V3 p1, V3 q1, V3 p2, V3 q2, float& s, float& t) {
    constexpr float kEps = 1e-8f;
    const V3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    if (a <= kEps && e <= kEps) {
        s = t = 0.f;
        return;
    }
    if (a <= kEps) {
        s = 0.f;
        t = clamp01(f / e);
        return;
    }
    const float c = dot(d1, r);
    if (e <= kEps) {
        t = 0.f;
        s = clamp01(-c / a);
        return;
    }
    const float b = dot(d1, d2);
    const float denom = a * e - b * b;
    s = denom > kEps * a * e ? clamp01((b * f - c * e) / denom) : 0.f; // parallel: any s, take 0
    t = (b * s + f) / e;
    if (t < 0.f) {
        t = 0.f;
        s = clamp01(-c / a);
    } else if (t > 1.f) {
        t = 1.f;
        s = clamp01((b - c) / a);
    }
}

// Any unit vector at right angles to v (v need not be unit; a zero v gives +Z).
inline V3 perpendicular(V3 v) {
    const float len = length(v);
    if (!(len > 1e-6f)) return {0.f, 0.f, 1.f};
    const V3 u = v * (1.f / len);
    const V3 other = std::fabs(u.x) < 0.9f ? V3{1.f, 0.f, 0.f} : V3{0.f, 1.f, 0.f};
    const V3 p = cross(u, other);
    return p * (1.f / length(p));
}

// One presser against one receiver. False when they do not overlap (or a radius is not positive).
inline bool touch(const Capsule& presser, const Receiver& recv, const Params& prm, Contact& out) {
    const Capsule& rc = recv.c;
    if (!(presser.r > 0.f) || !(rc.r > 0.f)) return false;
    float s = 0.f, t = 0.f; // s on the receiver, t on the presser
    closestOnSegments(rc.a, rc.b, presser.a, presser.b, s, t);
    const V3 q = rc.a + (rc.b - rc.a) * s;
    const V3 p = presser.a + (presser.b - presser.a) * t;
    const V3 d = p - q;
    const float dist = length(d);
    const float pen = presser.r + rc.r - dist;
    if (!(pen > 0.f)) return false;
    V3 n;
    if (dist > 1e-4f) {
        n = d * (1.f / dist);
    } else { // presser axis on the receiver's axis: push out at right angles to the receiver
        const V3 axis = rc.b - rc.a;
        n = perpendicular(length(axis) > 1e-6f ? axis : presser.b - presser.a);
    }
    const float soft = 1.f - clamp01(recv.firmness);
    float depth = pen * soft;
    const float cap = clamp01(prm.dentFrac) * 2.f * rc.r;
    if (depth > cap) depth = cap;
    if (depth > rc.r) depth = rc.r;
    const float pp = pen < presser.r ? pen : presser.r;
    const float r2 = 2.f * presser.r * pp - pp * pp;
    out.point = q + n * rc.r;
    out.normal = n;
    out.radius = r2 > 0.f ? std::sqrt(r2) : 0.f;
    out.depth = depth;
    return true;
}

// Every presser against every receiver; the kMaxContacts deepest kept, deepest first. Ties keep the order found
// (receiver by receiver, presser by presser), so a frame's order does not flicker.
inline Contacts solve(std::span<const Capsule> pressers, std::span<const Receiver> receivers, const Params& prm = {}) {
    Contacts out;
    for (int ri = 0; ri < static_cast<int>(receivers.size()); ++ri) {
        for (int pi = 0; pi < static_cast<int>(pressers.size()); ++pi) {
            Contact c;
            if (!touch(pressers[static_cast<size_t>(pi)], receivers[static_cast<size_t>(ri)], prm, c)) continue;
            c.receiver = ri;
            c.presser = pi;
            ++out.touching;
            int at = out.count; // first place whose depth is less than c's (strictly: ties stay behind)
            while (at > 0 && out.c[at - 1].depth < c.depth) --at;
            if (at >= kMaxContacts) continue;
            const int last = out.count < kMaxContacts ? out.count : kMaxContacts - 1;
            for (int k = last; k > at; --k) out.c[k] = out.c[k - 1];
            out.c[at] = c;
            if (out.count < kMaxContacts) ++out.count;
        }
    }
    return out;
}

// The look of one contact, as the material draws it.
struct Shape {
    float bulge = 1.f;      // bulge ring height multiplier: 0 none, 1 default, 2 double
    float bulgeWidth = 1.f; // ring width, cm, outside the patch radius
};

// The ring's peak, as a fraction of the dent's depth, at bulge 1 (the flesh pushed aside rises a little round the edge).
inline constexpr float kBulgePeak = 0.2f;

// Height of the skin (cm, + out along the normal, - in) at distance s (cm, along the skin) from the contact's centre.
// Dent (1 - u^2)^2 inside the patch radius R, u = s/R; ring 16 v^2 (1 - v)^2 over R..R+W, v = (s - R)/W. Both meet at
// zero height and zero slope at R, and the ring ends flat at R+W. Exactly 0 when depth is 0.
inline float profile(float s, float R, float depth, const Shape& sh) {
    if (!(depth > 0.f) || !(R > 0.f)) return 0.f;
    s = std::fabs(s);
    if (s < R) {
        const float u = s / R, w = 1.f - u * u;
        return -depth * w * w;
    }
    const float W = sh.bulgeWidth;
    if (!(W > 0.f) || !(sh.bulge > 0.f) || s >= R + W) return 0.f;
    const float v = (s - R) / W, m = v * (1.f - v);
    return sh.bulge * kBulgePeak * depth * 16.f * m * m;
}

// d profile / d s: the slope the material's normal fix tilts the normal by. 0 at the centre and at both edges.
inline float slope(float s, float R, float depth, const Shape& sh) {
    if (!(depth > 0.f) || !(R > 0.f)) return 0.f;
    const float sign = s < 0.f ? -1.f : 1.f;
    s = std::fabs(s);
    if (s < R) {
        const float u = s / R, w = 1.f - u * u;
        return sign * (-depth * 2.f * w * (-2.f * u) / R); // d/ds of -depth (1 - u^2)^2
    }
    const float W = sh.bulgeWidth;
    if (!(W > 0.f) || !(sh.bulge > 0.f) || s >= R + W) return 0.f;
    const float v = (s - R) / W, m = v * (1.f - v);
    return sign * (sh.bulge * kBulgePeak * depth * 16.f * 2.f * m * (1.f - 2.f * v) / W);
}

} // namespace ftcore::flesh
