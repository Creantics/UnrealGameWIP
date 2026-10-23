# Tuck a simulated garment into a boot (or glove, or any shell fitted to the body): below a height, pull the garment's
# vertices in to a fixed gap off the body, so they sit inside the shell's inner wall; above it, a blend band eases back
# to the simulated shape, so the cloth blouses over the shell's rim. Plain Python (numpy + scipy), no Blender.
#
#   python tuck_garment.py outfit.obj body.obj out.obj --groups trousers_ --top 44 --band 6 --gap 0.9
#
# outfit.obj is an MD export (groups per pattern, one shared vertex list); only vertices used by faces of groups whose
# name starts with one of --groups move. body.obj is the avatar in the same units and axes (y up). --gap must be less
# than the shell's margin minus the garment's thickness. --min keeps vertices already nearer the body than that off it.
# Each group pulls only towards the body on its own side of x = 0 (the side of the group's mean below the rim), so
# cloth hanging between the legs goes to its own leg instead of bridging to the other one. --cut drops the moved
# groups' faces whose vertices all lie below that height: cloth the shell hides only risks poking through it.
import argparse
import numpy as np
from scipy.spatial import cKDTree


def read_vertices(path):
    return np.array([l.split()[1:4] for l in open(path) if l.startswith("v ")], float)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outfit"); ap.add_argument("body"); ap.add_argument("out")
    ap.add_argument("--groups", nargs="+", required=True)
    ap.add_argument("--top", type=float, required=True, help="shell rim height (y)")
    ap.add_argument("--band", type=float, default=6.0, help="blend height above the rim")
    ap.add_argument("--gap", type=float, default=0.9, help="distance off the body below the rim")
    ap.add_argument("--cut", type=float, default=None, help="drop hidden faces below this height")
    ap.add_argument("--min", type=float, default=0.4, help="push vertices nearer than this out to it")
    a = ap.parse_args()

    lines = open(a.outfit).read().split("\n")
    vi = [i for i, l in enumerate(lines) if l.startswith("v ")]
    V = np.array([lines[i].split()[1:4] for i in vi], float)
    groups, cur = {}, None
    for l in lines:
        if l.startswith(("g ", "o ")):
            name = l[2:].strip()
            cur = groups.setdefault(name, set()) if name.startswith(tuple(a.groups)) else None
        elif cur is not None and l.startswith("f "):
            cur.update(int(t.split("/")[0]) - 1 for t in l.split()[1:])
    body = read_vertices(a.body)
    trees = {s: (body[body[:, 0] * s >= 0], cKDTree(body[body[:, 0] * s >= 0])) for s in (1, -1)}
    side = {}
    for g in groups.values():
        low = [i for i in g if V[i, 1] < a.top]
        s = 1 if not low or V[low, 0].mean() >= 0 else -1
        for i in g:
            side.setdefault(i, s)
    idx = np.array(sorted(i for i in side if V[i, 1] < a.top + a.band), int)

    d, near, pts = np.zeros(len(idx)), np.zeros(len(idx), int), np.zeros((len(idx), 3))
    for s, (bv, tree) in trees.items():
        m = np.array([side[i] == s for i in idx])
        if m.any():
            d[m], near[m] = tree.query(V[idx[m]])
            pts[m] = bv[near[m]]
    dirn = (V[idx] - pts) / np.maximum(d, 1e-6)[:, None]
    w = np.clip((a.top + a.band - V[idx, 1]) / a.band, 0.0, 1.0)[:, None]
    target = np.where(d[:, None] > a.gap, a.gap, np.maximum(d, a.min)[:, None])
    V[idx] = pts + dirn * ((1 - w) * d[:, None] + w * target)

    for k, i in enumerate(vi):
        lines[i] = "v %.5f %.5f %.5f" % tuple(V[k])
    if a.cut is not None:
        keep, on, dropped = [], False, 0
        for l in lines:
            if l.startswith(("g ", "o ")):
                on = l[2:].strip().startswith(tuple(a.groups))
            elif on and l.startswith("f ") and all(V[int(t.split("/")[0]) - 1, 1] < a.cut for t in l.split()[1:]):
                dropped += 1
                continue
            keep.append(l)
        lines = keep
        print("tuck_garment: dropped %d faces below %.1f" % (dropped, a.cut))
    open(a.out, "w").write("\n".join(lines))
    d2, _ = cKDTree(body).query(V[idx[w[:, 0] >= 1]])
    print("tuck_garment: moved %d vertices; below the rim distance off the body %.2f..%.2f" %
          (len(idx), d2.min() if len(d2) else 0, d2.max() if len(d2) else 0))


if __name__ == "__main__":
    main()
