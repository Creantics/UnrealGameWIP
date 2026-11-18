"""Benchmark report: one table row per run and per scene x size from bench.ps1's output folder (stdlib only).

    python -I tools/bench/bench_report.py <out dir> [--hitch-top 5]

For each <scene>_<size>_r<n>/ folder it reads the engine CSV and prints frame, game, render, GPU (median, p95, p99,
max), draw calls, primitives, local VRAM, the streaming pool, frames over 33 and 50 ms, and for each frame over 33 ms
the Exclusive/* game-thread stats that grew most over the run's median (the hitch's likely cause). Writes report.md
next to the runs.
"""
import csv
import math
import re
import statistics
import sys
from pathlib import Path

csv.field_size_limit(10**9)


def pct(values, p):
    """Nearest rank, as ftcore::framePercentile: the ceil(p% x n)-th smallest value (the smallest at p = 0)."""
    if not values:
        return float('nan')
    s = sorted(values)
    rank = math.ceil(min(max(p, 0.0), 100.0) / 100.0 * len(s))
    return s[max(rank, 1) - 1]


def read_csv(path):
    with open(path, newline='', encoding='utf-8', errors='replace') as f:
        rows = list(csv.reader(f))
    if not rows:
        return [], []
    # The engine adds a column when a stat first appears, so with [HasHeaderRowAtEnd] the full header is the row before
    # the metadata row at the end and early rows are short. Rows start with the EVENTS cell; pad short rows.
    head = rows[0]
    end = len(rows)
    if rows[-1] and rows[-1][0] == '[HasHeaderRowAtEnd]':
        head = rows[-2]
        end = len(rows) - 2
    body = []
    for r in rows[1:end]:
        if not r or r[0] == 'EVENTS' or r[0].startswith('['):
            continue
        body.append((r + [''] * len(head))[:len(head)])
    return head, body


def column(head, body, *names):
    for n in names:
        if n in head:
            i = head.index(n)
            out = []
            for r in body:
                try:
                    out.append(float(r[i]))
                except ValueError:
                    pass
            return out
    return []


def run_stats(run_dir, hitch_top):
    files = [p for p in run_dir.glob('*.csv')]
    if not files:
        return None
    head, body = read_csv(files[0])
    if not body or 'FrameTime' not in head:
        return None
    frame = column(head, body, 'FrameTime')
    st = {'frames': len(frame)}
    for key, names in (('frame', ('FrameTime',)), ('game', ('GameThreadTime',)), ('render', ('RenderThreadTime',)),
                       ('rhi', ('RHIThreadTime',)), ('gpu', ('GPUTime', 'GPU/Total'))):
        v = column(head, body, *names)
        st[key] = (statistics.median(v), pct(v, 95), pct(v, 99), max(v)) if v else None
    for key, names in (('draws', ('RHI/DrawCalls',)), ('prims', ('RHI/PrimitivesDrawn',)),
                       ('vram', ('GPUMem/LocalUsedMB',)), ('pool', ('TextureStreaming/StreamingPool',)),
                       ('wanted', ('TextureStreaming/RequiredPool', 'TextureStreaming/WantedMips'))):
        v = column(head, body, *names)
        st[key] = (statistics.median(v), max(v)) if v else None
    st['over33'] = sum(1 for t in frame if t > 33.0)
    st['over50'] = sum(1 for t in frame if t > 50.0)
    # Hitch attribution: the stats whose value on that frame is furthest over their own run median.
    fi = head.index('FrameTime')
    cand = [i for i, h in enumerate(head) if h.startswith(('Exclusive/GameThread/', 'Exclusive/RenderThread/', 'GPU/',
                                                           'PSO/', 'GC/', 'LevelStreaming/', 'TextureStreaming/',
                                                           'Ticks/', 'ChaosPhysics/')) and 'EventWait' not in h]
    med = {}
    for i in cand:
        vals = []
        for r in body:
            try:
                vals.append(float(r[i]))
            except ValueError:
                pass
        med[i] = statistics.median(vals) if vals else 0.0
    hitches = []
    for n, r in enumerate(body):
        try:
            t = float(r[fi])
        except ValueError:
            continue
        if t <= 33.0:
            continue
        lift = []
        for i in cand:
            try:
                d = float(r[i]) - med[i]
            except ValueError:
                continue
            if d > 0.5:
                lift.append((d, head[i]))
        lift.sort(reverse=True)
        hitches.append((n, t, ', '.join(f'{h} +{d:.1f}' for d, h in lift[:hitch_top])))
    st['hitches'] = hitches
    m = run_dir / 'machine.txt'
    st['machine'] = m.read_text(encoding='utf-8-sig').splitlines()[0].strip() if m.exists() else '?'
    st['settings'] = {}
    for f in run_dir.glob('*_summary.txt'):
        for ln in f.read_text(encoding='utf-8', errors='replace').splitlines():
            ln = ln.strip()
            if '=' in ln and (ln.startswith(('r.', 'sg.', 'viewport='))):
                k, v = ln.split('=', 1)
                st['settings'][k] = v.split()[0] if v else v
    return st


def fmt4(v):
    return '-' if v is None else '{:.2f} / {:.2f} / {:.2f} / {:.1f}'.format(*v)


def fmt2(v, scale=1.0, nd=0):
    return '-' if v is None else '{:.{nd}f} / {:.{nd}f}'.format(v[0] * scale, v[1] * scale, nd=nd)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    out = Path(sys.argv[1])
    top = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[2] == '--hitch-top' else 5
    runs = sorted(p for p in out.iterdir() if p.is_dir() and re.match(r'.+_\d+x\d+_r\d+$', p.name))
    lines = ['# Bench report: ' + str(out), '',
             'RESOLUTION: {res}', '',
             'ms columns are median / p95 / p99 / max; counts median / max; prims in millions. A CONTAMINATED run had'
             ' other load on the PC at its start or end (machine.txt) and is not in the per-set table.', '',
             '| run | PC | frames | frame | game | render | RHI | GPU | draws | prims M | VRAM MB | pool MB | >33 | >50 |',
             '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    groups = {}
    res_seen = set()
    hitch_lines = []
    for r in runs:
        st = run_stats(r, top)
        if not st:
            lines.append(f'| {r.name} | | no CSV with a FrameTime column | | | | | | | | | | | |')
            continue
        if st['machine'] != 'CONTAMINATED':
            groups.setdefault(r.name.rsplit('_r', 1)[0], []).append(st)
        sets = st['settings']
        if sets:
            res_seen.add('{} output, AA method {}{}, r.ScreenPercentage {}, Default.Desktop.Mode {}, sg.ResolutionQuality {}'
                         .format(sets.get('viewport', '?'), sets.get('r.AntiAliasingMethod', '?'),
                                 ' (TSR)' if sets.get('r.AntiAliasingMethod') == '4' else '',
                                 sets.get('r.ScreenPercentage', '?'),
                                 sets.get('r.ScreenPercentage.Default.Desktop.Mode', '?'),
                                 sets.get('sg.ResolutionQuality', '?')))
        lines.append('| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} |'.format(
            r.name, st['machine'], st['frames'], fmt4(st['frame']), fmt4(st['game']), fmt4(st['render']), fmt4(st['rhi']),
            fmt4(st['gpu']), fmt2(st['draws']), fmt2(st['prims'], 1e-6, 2), fmt2(st['vram']), fmt2(st['pool']),
            st['over33'], st['over50']))
        for n, t, why in st['hitches'][:40]:
            hitch_lines.append(f'- {r.name} frame {n}: {t:.1f} ms; {why or "no stat over its median by 0.5"}')
    lines += ['', '## Per scene and size (median of the runs\' values; worst run for max and counts)', '',
              '| set | runs | frame p50 | frame p95 | frame p99 | frame max | >33 | >50 |', '|---|---|---|---|---|---|---|---|']
    for k, sts in groups.items():
        f = [s['frame'] for s in sts if s['frame']]
        if not f:
            continue
        lines.append('| {} | {} | {:.2f} | {:.2f} | {:.2f} | {:.1f} | {} | {} |'.format(
            k, len(sts), statistics.median(x[0] for x in f), statistics.median(x[1] for x in f),
            statistics.median(x[2] for x in f), max(x[3] for x in f), max(s['over33'] for s in sts),
            max(s['over50'] for s in sts)))
    lines += ['', '## Frames over 33 ms and the stats that rose most on them', ''] + (hitch_lines or ['- none'])
    base = out / 'baselines.txt'
    lines += ['', '## Block idle baselines (a run is CONTAMINATED when its background CPU is more than the limit over its'
              ' block\'s baseline, or the 3D engine is over its limit)', '']
    lines += ['- ' + x for x in base.read_text(encoding='utf-8-sig').splitlines() if x.strip()] if base.exists() else ['- none']
    lines[2] = ('RESOLUTION: ' + ('; '.join(sorted(res_seen)) or 'no summary files read') +
                '. With sg.ResolutionQuality 0 the engine\'s auto curve renders about 73% at 1080p and 61% at 1440p'
                ' (about 1560x880) before TSR; the GPU-split runs at 100% and 75% give its cost.')
    text = '\n'.join(lines) + '\n'
    (out / 'report.md').write_text(text, encoding='utf-8')
    print(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
