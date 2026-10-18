#!/usr/bin/env python3
"""Check a VO directions CSV against the schema (and, optionally, against its skeleton).

    python3 validate_directions.py directions/draft56_04a.csv \
        --skeleton directions/draft56_skeleton.csv --files 04a

Errors (exit 1):
  * a skeleton line_id missing, duplicated, or not in the skeleton
  * speaker changed (allowed only where the skeleton says UNKNOWN)
  * a required direction field empty, or a value outside the controlled vocabulary
  * the words of elevenlabs_text (or pauses) differ from the script line (text_hash mismatch);
    silence / nonverbal rows must instead carry tags only
  * elevenlabs_settings unparseable or out of range
Warnings: pace_wpm outside its band, undocumented / experimental tags, more than two tags,
rows still marked REVIEW or NEW by carry_forward.py.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402

REQUIRED = ['context', 'emotion_primary', 'intensity', 'pace', 'pace_wpm', 'volume',
            'distance', 'pitch_contour', 'elevenlabs_settings']
SETTINGS_RE = re.compile(
    r'^stability=(?P<stab>[01](?:\.\d+)?)\s+style=(?P<style>[01](?:\.\d+)?)\s+'
    r'speed=(?P<speed>\d(?:\.\d+)?)(?:\s+similarity=(?P<sim>[01](?:\.\d+)?))?(?P<temp>\s+TEMP)?$')
NUM_RE = re.compile(r'^\d+(?:\.\d+)?$')


def check_row(row, errors, warnings):
    lid = row.get('line_id', '?')

    def err(msg):
        errors.append(f'{lid}: {msg}')

    def warn(msg):
        warnings.append(f'{lid}: {msg}')

    kind = row.get('kind', '')
    silent = kind in vc.SILENT_KINDS
    for col in REQUIRED:
        if not (row.get(col) or '').strip():
            err(f'{col} is empty')
    if not silent and not (row.get('elevenlabs_text') or '').strip():
        err('elevenlabs_text is empty')

    for col in ('emotion_primary', 'emotion_secondary'):
        v = (row.get(col) or '').strip()
        if v and v not in vc.EMOTIONS:
            err(f'{col} "{v}" not in vocabulary')
    v = (row.get('intensity') or '').strip()
    if v and v not in {'1', '2', '3', '4', '5'}:
        err(f'intensity "{v}" not 1-5')
    pace = (row.get('pace') or '').strip()
    if pace and pace not in vc.PACES:
        err(f'pace "{pace}" not in {list(vc.PACES)}')
    wpm = (row.get('pace_wpm') or '').strip()
    if wpm:
        if not wpm.isdigit():
            err(f'pace_wpm "{wpm}" not an integer')
        elif pace in vc.PACES:
            lo, hi, _ = vc.PACES[pace]
            if not lo <= int(wpm) <= hi:
                warn(f'pace_wpm {wpm} outside the {pace} band {lo}-{hi}')
    for col, vocab in (('volume', vc.VOLUMES), ('distance', vc.DISTANCES)):
        v = (row.get(col) or '').strip()
        if v and v not in vocab:
            err(f'{col} "{v}" not in {vocab}')
    v = (row.get('pitch_contour') or '').strip()
    if v:
        bad = [p for p in v.split('>') if p.strip() not in vc.CONTOURS]
        if bad:
            err(f'pitch_contour part(s) {bad} not in vocabulary')
    v = (row.get('take_note') or '').strip()
    if v:
        bad = [p for p in v.split('+') if p.strip() not in vc.TAKE_NOTES]
        if bad:
            err(f'take_note part(s) {bad} not in {vc.TAKE_NOTES}')
    for col in ('hold_before_s', 'hold_after_s'):
        v = (row.get(col) or '').strip()
        if v and not NUM_RE.match(v):
            err(f'{col} "{v}" is not a number of seconds')

    s = (row.get('elevenlabs_settings') or '').strip()
    if s:
        m = SETTINGS_RE.match(s)
        if not m:
            err(f'elevenlabs_settings "{s}" does not parse (stability=0.40 style=0.15 speed=1.00 [similarity=0.80] [TEMP])')
        else:
            if not 0.7 <= float(m['speed']) <= 1.2:
                err(f'speed {m["speed"]} outside 0.7-1.2')
            for k in ('stab', 'style', 'sim'):
                if m[k] is not None and not 0 <= float(m[k]) <= 1:
                    err(f'{k} {m[k]} outside 0-1')

    el = row.get('elevenlabs_text') or ''
    tags = [t[1:-1].strip() for t in vc.TAG_RE.findall(el)]
    expressive = [t for t in tags if t.lower() not in ('short pause', 'long pause')]
    if len(expressive) > 2:
        warn(f'{len(expressive)} audio tags (schema: one by default, two at most; pause tags not counted)')
    for t in tags:
        tl = t.lower()
        if tl in vc.EXPERIMENTAL_TAGS:
            warn(f'experimental tag [{t}]')
        elif tl not in vc.DOCUMENTED_TAGS and len(t.split()) > 4:
            warn(f'long descriptive tag [{t}]')
    if silent:
        if vc.words_key(el):
            err('silence/nonverbal row: elevenlabs_text must be empty or tags only')
    else:
        want = row.get('text_hash', '')
        if vc.text_hash(el) != want:
            err(f'elevenlabs_text words differ from the script line '
                f'("{vc.words_key(el)}" vs "{vc.words_key(vc.spoken(row.get("text", "")))}")')
        p = row.get('pauses') or ''
        if p.strip() and vc.text_hash(p) != want:
            err('pauses column words differ from the script line')

    cs = (row.get('carry_status') or '').strip()
    if cs in ('REVIEW', 'NEW'):
        warn(f'carry_status {cs}: {row.get("carry_note", "")}')


def validate(rows, skeleton=None, files=None):
    errors, warnings = [], []
    seen = {}
    for r in rows:
        seen.setdefault(r.get('line_id'), []).append(r)
        check_row(r, errors, warnings)
    for lid, rs in seen.items():
        if len(rs) > 1:
            errors.append(f'{lid}: appears {len(rs)} times')
    if skeleton is not None:
        sk = [r for r in skeleton if not files or any(r['file'].startswith(f) for f in files)]
        sk_ids = {r['line_id']: r for r in sk}
        for lid in sk_ids:
            if lid not in seen:
                errors.append(f'{lid}: in skeleton but not annotated')
        for lid, rs in seen.items():
            if lid not in sk_ids:
                errors.append(f'{lid}: not in skeleton')
                continue
            s, d = sk_ids[lid], rs[0]
            if d.get('text_hash') != s['text_hash']:
                errors.append(f'{lid}: text_hash differs from skeleton (script changed? re-run carry_forward)')
            if d.get('speaker') != s['speaker'] and s['speaker'] != 'UNKNOWN':
                errors.append(f'{lid}: speaker {d.get("speaker")} != skeleton {s["speaker"]}')
    return errors, warnings


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('directions')
    ap.add_argument('--skeleton', help='skeleton CSV from extract_lines.py; checks coverage')
    ap.add_argument('--files', nargs='*', help='limit the skeleton to these file prefixes, e.g. 04a')
    ap.add_argument('--quiet-warnings', action='store_true')
    a = ap.parse_args(argv)
    _, rows = vc.read_csv(a.directions)
    skel = vc.read_csv(a.skeleton)[1] if a.skeleton else None
    errors, warnings = validate(rows, skel, a.files)
    if not a.quiet_warnings:
        for w in warnings:
            print('WARN ', w)
    for e in errors:
        print('ERROR', e)
    print(f'{len(rows)} rows, {len(errors)} errors, {len(warnings)} warnings')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
