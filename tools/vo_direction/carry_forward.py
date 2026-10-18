#!/usr/bin/env python3
"""Carry VO directions from an old draft's directions CSV onto a new draft's skeleton.

    python3 carry_forward.py voice/directions/draft56_04a.csv voice/directions/draft57_skeleton.csv \
        -o voice/directions/draft57_04a.csv [--files 04a] [--threshold 0.85] [--dropped dropped.csv]

Matching, in order (each old row is used at most once):
  0. vo_id      both rows carry the same authored VO id (camp lines)            -> EXACT / REVIEW
  1. scene      same text_hash in the same scene (scene id OR scene title equal)  -> EXACT / REVIEW
  2. global     same text_hash and speaker anywhere, line of 4+ words            -> REVIEW (moved)
  3. fuzzy      same speaker, difflib ratio >= threshold; same scene first, then
                anywhere for lines of 4+ words                                    -> REVIEW (edited)
  otherwise     NEW, direction columns left blank.
EXACT means speaker, punctuation and the script's delivery note are unchanged too; any of those
changing gives REVIEW with the reason in carry_note. REVIEW rows keep the old direction but their
elevenlabs_text no longer matches the script for fuzzy matches: a person must re-cut it.
"""
import argparse
import difflib
import os
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402


def direction_columns(old_cols):
    skel = set(vc.SKELETON_COLUMNS) | set(vc.CARRY_COLUMNS)
    extra = [c for c in old_cols if c not in skel and c not in vc.DIRECTION_COLUMNS]
    return [c for c in vc.DIRECTION_COLUMNS if c in old_cols] + extra


def differences(old, new):
    why = []
    if old['speaker'] != new['speaker']:
        why.append(f'speaker {old["speaker"]}->{new["speaker"]}')
    if vc.surface_key(old['text']) != vc.surface_key(new['text']):
        why.append('punctuation/emphasis changed')
    if (old.get('delivery_original') or '') != (new.get('delivery_original') or ''):
        why.append(f'delivery note "{old.get("delivery_original", "")}" -> "{new.get("delivery_original", "")}"')
    if (old.get('variant') or '') != (new.get('variant') or ''):
        why.append('variant condition changed')
    return why


def carry(old_rows, new_rows, dcols, threshold=0.85, min_global_words=4):
    used = set()
    out = []
    result = {}

    def take(i, j, status, note):
        used.add(j)
        result[i] = (j, status, note)

    old_by_vo = {o['vo_id']: j for j, o in enumerate(old_rows) if o.get('vo_id')}
    # pass 0: authored vo_id
    for i, n in enumerate(new_rows):
        j = old_by_vo.get(n.get('vo_id') or None)
        if j is not None and j not in used:
            o = old_rows[j]
            if o['text_hash'] == n['text_hash']:
                why = differences(o, n)
                take(i, j, 'REVIEW' if why else 'EXACT', '; '.join(why))
            else:
                r = difflib.SequenceMatcher(None, vc.words_key(o['spoken_text']), vc.words_key(n['spoken_text'])).ratio()
                take(i, j, 'REVIEW', f'same vo_id, words changed (ratio {r:.2f}); re-cut elevenlabs_text')

    def same_scene(o, n):
        return o['scene'] == n['scene'] or (o.get('scene_title') and o.get('scene_title') == n.get('scene_title'))

    # pass 1: same scene, exact words. Prefer same speaker and variant, then source order.
    for i, n in enumerate(new_rows):
        if i in result:
            continue
        cands = [j for j, o in enumerate(old_rows)
                 if j not in used and o['text_hash'] == n['text_hash'] and same_scene(o, n)]
        if not cands:
            continue
        cands.sort(key=lambda j: (old_rows[j]['speaker'] != n['speaker'],
                                  (old_rows[j].get('variant') or '') != (n.get('variant') or ''),
                                  int(old_rows[j].get('seq') or 0)))
        j = cands[0]
        o = old_rows[j]
        why = differences(o, n)
        if o['scene'] != n['scene']:
            why.insert(0, f'scene renumbered {o["scene"]}->{n["scene"]}')
        take(i, j, 'REVIEW' if why else 'EXACT', '; '.join(why))

    # pass 2: global exact words, same speaker, long enough not to be a stock reply
    for i, n in enumerate(new_rows):
        if i in result or vc.word_count(n['text']) < min_global_words:
            continue
        cands = [j for j, o in enumerate(old_rows)
                 if j not in used and o['text_hash'] == n['text_hash'] and o['speaker'] == n['speaker']]
        if cands:
            j = cands[0]
            why = [f'moved from {old_rows[j]["scene"]}'] + differences(old_rows[j], n)
            take(i, j, 'REVIEW', '; '.join(why))

    # pass 3: fuzzy, same speaker; same scene first, then anywhere
    for i, n in enumerate(new_rows):
        if i in result:
            continue
        nk = vc.words_key(n['spoken_text'])
        if not nk:
            continue
        best = None
        long_enough = vc.word_count(n['text']) >= min_global_words
        for scene_only in ((True, False) if long_enough else (True,)):
            for j, o in enumerate(old_rows):
                if j in used or o['speaker'] != n['speaker']:
                    continue
                if scene_only and not same_scene(o, n):
                    continue
                r = difflib.SequenceMatcher(None, vc.words_key(o['spoken_text']), nk).ratio()
                if r >= threshold and (best is None or r > best[0]):
                    best = (r, j)
            if best:
                break
        if best:
            r, j = best
            where = '' if same_scene(old_rows[j], n) else f'from {old_rows[j]["scene"]}; '
            take(i, j, 'REVIEW', f'{where}words edited (ratio {r:.2f}); re-cut elevenlabs_text and pauses')

    for i, n in enumerate(new_rows):
        row = dict(n)
        if i in result:
            j, status, note = result[i]
            o = old_rows[j]
            for c in dcols:
                row[c] = o.get(c, '')
            row['carry_status'] = status
            row['carry_from'] = o['line_id']
            row['carry_note'] = note
        else:
            for c in dcols:
                row[c] = ''
            row['carry_status'] = 'NEW'
            row['carry_from'] = ''
            row['carry_note'] = ''
        out.append(row)
    dropped = [o for j, o in enumerate(old_rows) if j not in used]
    return out, dropped


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('old', help='old directions CSV (any number of acts)')
    ap.add_argument('new', help='new skeleton CSV from extract_lines.py')
    ap.add_argument('-o', '--out', required=True)
    ap.add_argument('--files', nargs='*', help='only carry onto new rows whose file starts with these')
    ap.add_argument('--threshold', type=float, default=0.85)
    ap.add_argument('--min-global-words', type=int, default=4)
    ap.add_argument('--dropped', help='write old rows that matched nothing to this CSV')
    a = ap.parse_args(argv)

    old_cols, old_rows = vc.read_csv(a.old)
    _, new_rows = vc.read_csv(a.new)
    if a.files:
        new_rows = [r for r in new_rows if any(r['file'].startswith(f) for f in a.files)]
    dcols = direction_columns(old_cols)
    out, dropped = carry(old_rows, new_rows, dcols, a.threshold, a.min_global_words)
    cols = vc.SKELETON_COLUMNS + dcols + vc.CARRY_COLUMNS
    vc.write_csv(a.out, cols, out)
    if a.dropped:
        vc.write_csv(a.dropped, old_cols, dropped)

    st = Counter(r['carry_status'] for r in out)
    total = len(out) or 1
    print(f'wrote {len(out)} rows to {a.out}')
    for s in ('EXACT', 'REVIEW', 'NEW'):
        print(f'  {s:7} {st[s]:5}  {100 * st[s] / total:5.1f}%')
    print(f'  carried {st["EXACT"] + st["REVIEW"]} of {len(out)} ({100 * (st["EXACT"] + st["REVIEW"]) / total:.1f}%)')
    per = defaultdict(Counter)
    for r in out:
        per[r['scene']][r['carry_status']] += 1
    print('per scene (EXACT/REVIEW/NEW):')
    for sc, c in per.items():
        print(f'  {sc:12} {c["EXACT"]:4} {c["REVIEW"]:4} {c["NEW"]:4}')
    moved = [r for r in out if 'moved from' in r['carry_note'] or 'renumbered' in r['carry_note']]
    if moved:
        print('moved lines:')
        for r in moved:
            print(f'  {r["line_id"]} <- {r["carry_from"]}: {r["carry_note"]}')
    print(f'old rows not carried (cut or rewritten past the threshold): {len(dropped)}')
    for o in dropped[:30]:
        print(f'  {o["line_id"]} {o["speaker"]}: {o["spoken_text"][:60]}')
    if len(dropped) > 30:
        print(f'  ... {len(dropped) - 30} more' + ('' if a.dropped else ' (use --dropped)'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
