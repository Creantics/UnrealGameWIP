#!/usr/bin/env python3
"""Build a directions CSV from compact pipe-format annotations plus the skeleton.

    python3 build_directions.py --skeleton design/forgotten_third/voice/directions/draft56_skeleton.csv \
        --files 04b --ann my_notes/ann_*.txt \
        -o design/forgotten_third/voice/directions/draft56_04b.csv

This is an authoring aid. The CSV it writes is the source of truth; carry_forward.py and
validate_directions.py work on the CSV only.

One annotation per line, 15 fields separated by ' | ' (blank lines and '#' lines ignored):

  line_id | context | emo1[,emo2] | intensity | pace wpm | volume,distance | contour |
  pauses | emphasis | subtext | breath_nonverbal | hold_before,hold_after,take_note |
  tag | settings_adj | notes

  pauses        the line's words with / and //, or '=' for the spoken text unchanged
  emphasis      comma list; script *italics* are added automatically. A leading '!' means
                CAPS only the words you list (use when an auto-added word also occurs earlier
                in the line and would CAPS the wrong one)
  holds         e.g. '2.0,,one_take'  ',3.0,'  '' (empty)
  tag           '[x]' goes at the start of elevenlabs_text, '>[x]' at the end, '' none
  settings_adj  space-separated: s-0.05 (stability delta), y0.20 (style), v0.90 (speed override)

Derived columns:
  elevenlabs_text  from pauses: '//' -> [short pause]; mid-phrase '/' -> an ellipsis; a '/'
                   after . ? ! — or a quote adds nothing. Emphasis words go in CAPS only when the
                   volume is conversational or louder. Then the tag.
  elevenlabs_settings  the speaker's base from voice_prompts.md (v3/v2 line; v4-only entries get
                   style 0.10 speed 1.0; reuse voices inherit their source), speed x the pace
                   multiplier, then settings_adj. Minors are marked TEMP.
"""
import argparse
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
VOICE_PROMPTS = os.path.join(REPO, 'design', 'forgotten_third', 'voice', 'voice_prompts.md')
DAY = (0.50, 0.10, 1.0, 0.75, False)          # stability, style, speed, similarity, temp
# Values chosen by ear in the Act I pilot where voice_prompts.md gives v4 numbers only.
OVERRIDES = {'UNBENDING': (0.70, 0.05, 0.90, 0.85, False)}
# Speaker names in the script that voice_prompts.md files under another heading.
ALIASES = {'DEVIN AND DUY': 'DEVIN', 'DUY AND DEVIN': 'DUY'}
_REF = {'third core': 'THE THIRD', 'nil voice': 'NIL', 'granite voice': 'GRANITE',
        'pop voice': 'POP', 'maren voice': 'MAREN'}


def _num(pat, s):
    m = re.search(pat + r'\s*([01](?:\.\d+)?)', s)
    return float(m.group(1)) if m else None


def load_bases(path=VOICE_PROMPTS):
    """{HEADING KEY: (stability, style, speed, similarity, temp)} from the '- TTS:' lines."""
    if not os.path.exists(path):
        return {}
    raw, section, head = {}, '', None
    with open(path, encoding='utf-8') as fh:
        for line in fh:
            if line.startswith('## '):
                section, head = line[3:].strip().lower(), None
            elif line.startswith('### '):
                head = line[4:].strip()
            elif head and line.startswith('- TTS:'):
                raw[head] = (line[6:].strip(), 'restricted' in section or 'minors' in section)
                head = None
    bases = {}

    def key_names(h):
        h = re.sub(r'\([^)]*\)', '', h)
        h = re.sub(r"'[^']*'", '', h)
        return [re.sub(r'\s+', ' ', p).strip().upper() for p in h.split('/') if p.strip()]

    def parse(text, temp, depth=0):
        t = text.lower()
        base = DAY
        for ref, who in _REF.items():
            if t.startswith(ref) and depth < 2:
                src = next((v for h, v in raw.items() if who in key_names(h)), None)
                if src:
                    base = parse(src[0], src[1], depth + 1)
        st, sy, sp, sim = base[:4]
        v3 = t.split('v3/v2:', 1)[1] if 'v3/v2:' in t else t
        st = _num(r'stability', v3) if _num(r'stability', v3) is not None else st
        sim = _num(r'similarity', v3) if _num(r'similarity', v3) is not None else sim
        sy = _num(r'style', v3) if _num(r'style', v3) is not None else sy
        m = re.search(r'speed\s*(\d(?:\.\d+)?)', v3)
        sp = float(m.group(1)) if m else sp
        return (st, sy, sp, sim, temp or 'temp only' in t or t.startswith('n/a'))

    for h, (text, temp) in raw.items():
        for k in key_names(h):
            bases[k] = parse(text, temp)
    return bases


def base_for(speaker, bases):
    sp = speaker.upper()
    sp = ALIASES.get(sp, sp)
    if sp == 'UNKNOWN':          # unattributed: never borrow a real voice's settings
        return DAY
    if sp in OVERRIDES:
        return OVERRIDES[sp]
    if sp in bases:
        return bases[sp]
    words = set(sp.split())
    hits = sorted((k for k in bases if words <= set(k.split())), key=len)
    return bases[hits[0]] if hits else DAY


def caps(text, words):
    for w in words:
        w = w.strip()
        if not w:
            continue
        pat = re.compile(r'(?<![\w’\'])' + re.escape(w) + r'(?![\w’\'])', re.I)
        text, n = pat.subn(lambda m: m.group(0).upper(), text, count=1)
        if not n:
            raise ValueError(f'emphasis "{w}" not found in "{text}"')
    return text


def el_from_pauses(p):
    s = ' ' + p + ' '
    s = re.sub(r'\s//\s', ' [short pause] ', s)
    s = re.sub(r',\s/\s', '… ', s)
    s = re.sub(r'([.?!—…"])\s/\s', r'\1 ', s)
    s = re.sub(r'\s/\s', '… ', s)
    return re.sub(r'\s+', ' ', s).strip()


def script_emphasis(text):
    t = re.sub(r'\*\((?:[^()]|\([^()]*\))*\)\*', ' ', text)
    return [m.strip(' .,!?—…') for m in re.findall(r'\*([^*()][^*]*?)\*', t)]


def settings(base, pace, adj):
    st, sy, sp, sim, temp = base
    sp = sp * vc.PACES[pace][2]
    for a in adj.split():
        if a[0] == 's':
            st += float(a[1:])
        elif a[0] == 'y':
            sy = float(a[1:])
        elif a[0] == 'v':
            sp = float(a[1:])
    sp = min(1.2, max(0.7, sp))
    st = min(1.0, max(0.0, st))
    s = f'stability={st:.2f} style={sy:.2f} speed={sp:.2f} similarity={sim:.2f}'
    return s + (' TEMP' if temp else '')


def parse_ann(path):
    out = {}
    with open(path, encoding='utf-8') as fh:
        for n, line in enumerate(fh, 1):
            line = line.rstrip('\n')
            if not line.strip() or line.startswith('#'):
                continue
            f = [x.strip() for x in line.split(' | ')]
            if len(f) != 15:
                raise SystemExit(f'{path}:{n}: {len(f)} fields, want 15: {line[:80]}')
            out[f[0]] = f
    return out


def build_row(r, f, bases, speaker=None):
    (_, ctx, emo, inten, pace, vd, contour, pauses, emph, sub, breath, holds, tag, adj, notes) = f
    e = [x.strip() for x in emo.split(',')]
    pace_band, wpm = pace.rsplit(' ', 1)
    vol, dist = [x.strip() for x in vd.split(',')]
    hb, ha, take = (holds.split(',') + ['', '', ''])[:3] if holds else ('', '', '')
    silent = r['kind'] in vc.SILENT_KINDS
    if pauses == '=':
        pauses = r['spoken_text']
    if silent:
        pauses = ''
    manual_only = emph.startswith('!')
    emph = emph.lstrip('!')
    emph_words = [w.strip() for w in emph.split(',') if w.strip()] if emph else []
    caps_words = list(emph_words)
    for w in script_emphasis(r['text']):
        if w and w.lower() not in [x.lower() for x in emph_words]:
            emph_words.append(w)
            if not manual_only:
                caps_words.append(w)
    el = el_from_pauses(pauses) if pauses else ''
    if el and vol not in ('whisper', 'soft'):
        el = caps(el, caps_words)
    if tag:
        el = (el + ' ' + tag[1:]) if tag.startswith('>') else (tag + ' ' + el)
    d = dict(r)
    if speaker:
        d['speaker'] = speaker
    d.update(context=ctx, emotion_primary=e[0], emotion_secondary=e[1] if len(e) > 1 else '',
             intensity=inten, pace=pace_band, pace_wpm=wpm, volume=vol, distance=dist,
             pitch_contour=contour, pauses=pauses, emphasis=', '.join(emph_words),
             subtext=sub, breath_nonverbal=breath, hold_before_s=hb.strip(), hold_after_s=ha.strip(),
             take_note=take.strip(), elevenlabs_text=el.strip(),
             elevenlabs_settings=settings(base_for(d['speaker'], bases), pace_band, adj), notes=notes)
    return d


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--skeleton', required=True)
    ap.add_argument('--files', nargs='*', help='skeleton file prefixes, e.g. 04b')
    ap.add_argument('--ann', nargs='+', required=True, help='annotation files (globs allowed)')
    ap.add_argument('--speakers', help='optional CSV line_id,speaker for UNKNOWN rows')
    ap.add_argument('--voice-prompts', default=VOICE_PROMPTS)
    ap.add_argument('-o', '--out', required=True)
    a = ap.parse_args(argv)
    bases = load_bases(a.voice_prompts)
    _, skel = vc.read_csv(a.skeleton)
    if a.files:
        skel = [r for r in skel if any(r['file'].startswith(f) for f in a.files)]
    who = {}
    if a.speakers:
        who = {r['line_id']: r['speaker'] for r in vc.read_csv(a.speakers)[1]}
    ann = {}
    for pat in a.ann:
        for p in sorted(glob.glob(pat)) or [pat]:
            for k, v in parse_ann(p).items():
                if k in ann:
                    raise SystemExit(f'duplicate annotation {k} ({p})')
                ann[k] = v
    rows, missing = [], []
    for r in skel:
        f = ann.pop(r['line_id'], None)
        if f is None:
            missing.append(r['line_id'])
            continue
        rows.append(build_row(r, f, bases, who.get(r['line_id']) if r['speaker'] == 'UNKNOWN' else None))
    if ann:
        print('annotations with no skeleton row:', ' '.join(ann))
    fallback = sorted({r['speaker'] for r in rows if base_for(r['speaker'], bases) is DAY})
    if fallback:
        print('day-role default settings for:', ', '.join(fallback))
    print(f'{len(rows)} annotated, {len(missing)} missing')
    if missing:
        print('missing:', ' '.join(missing[:40]) + (' …' if len(missing) > 40 else ''))
    vc.write_csv(a.out, vc.SKELETON_COLUMNS + vc.DIRECTION_COLUMNS, rows)
    return 0


if __name__ == '__main__':
    sys.exit(main())
