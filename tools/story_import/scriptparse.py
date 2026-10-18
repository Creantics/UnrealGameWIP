#!/usr/bin/env python3
"""scriptparse.py - robust, read-only parser for the locked Draft 54 script (04a..04e).

parse(path) -> list of scene dicts in file order.
reassemble(scene) -> the exact original text of the scene block (heading + header + body + tail).
parse_all(dir) -> scenes from every 04*.md in sorted order.
file_segments(path) -> [('gap', [lines]) | ('scene', scene)] covering the file exactly
                       (used by the self-test to prove nothing is lost).

Model of a file (lines = text.split('\\n'), so a trailing newline is the last, empty element):
  gap lines     preamble, '---' separators, chapter/act headings and their intro lines, END markers
  scene block   '### SC-... · Title' line through the line before the next boundary, where a
                boundary is the next scene heading, any '# ' or '## ' heading, or a '---' line.
Inside a scene block:
  heading       the '### SC-' line
  header_lines  the contiguous '**Label:** ...' lines directly after the heading
  body          every following line, byte for byte (blank lines included), up to the last
                non-blank line that is not an '**END...**' marker
  tail          the trailing blank lines / '**END OF ACT**' markers (kept so reassembly is exact)
"""
import glob
import os
import re
import sys

SCENE_HDR = re.compile(r'^### (SC-[A-Za-z0-9\-]+) · (.*)$')
LABEL_LINE = re.compile(r'^\*\*([A-Za-z][A-Za-z ]*):\*\*(.*)$')
MODE_LINE = re.compile(r'^\*\*Mode:\*\* (.*?) · \*\*Gameplay:\*\*(?: (.*))?$')
CHAPTER_HDR = re.compile(r'^## CHAPTER (\d+) · (.*)$')
PCHAP_HDR = re.compile(r'^## (P\d) · (.*)$')
EPI_HDR = re.compile(r'^## EPILOGUE · (.*)$')
PROLOGUE_HDR = re.compile(r'^## PROLOGUE · (.*)$')
ACT_HDR = re.compile(r'^# (ACT ([IVX]+)) — (.*)$')
FILE_ACT = re.compile(r'^# .*PART \d+: (ACT ([IVX]+)) — (.*)$')
END_MARK = re.compile(r'^\*\*END( OF [A-Z ]+)?\*\*$')
ITALIC_NOTE = re.compile(r'\s*\((?:[^()]|\([^()]*\))*\)\s*$')

# SPEAKER (delivery): text   |   A and B (delivery): text   |   MAREN *(delivery)*: text
SPEAKER = re.compile(
    r"^(?P<who>[A-Z][A-Z0-9 .'’\-/É&]*?(?: and [A-Z][A-Z0-9 .'’\-/É&]*?)?)"
    r"(?: \*?\((?P<how>[^)]*)\)\*?)?: (?P<text>.*)$")
INLINE_DIR = re.compile(r'\*\([^)]*\)\*')
# in-text player-input notes in the locked script, e.g. '***CHOICE (flavor only).** ...'
INTEXT_NOTE = re.compile(r'^\*\*\*(CHOICE|VARIANT|OPTIONAL|HOLD|TIMED)\b')


BLOCKQUOTE = re.compile(r'^>[ 	]?')


def unquote(line):
    """A body line without its markdown blockquote prefix ('> DUY: ...' reads as dialogue)."""
    return BLOCKQUOTE.sub('', line)


def read_lines(path):
    with open(path, encoding='utf-8', newline='') as fh:
        return fh.read().split('\n')


def classify(line):
    """kind of a body line: blank | dialogue | note | direction | other"""
    if not line.strip():
        return 'blank'
    if INTEXT_NOTE.match(line):
        return 'note'
    if SPEAKER.match(line):
        return 'dialogue'
    if line.startswith('*') and not line.startswith('**'):
        return 'direction'
    return 'other'


def is_boundary(line):
    return bool(SCENE_HDR.match(line) or line.startswith('# ') or line.startswith('## ') or line == '---')


def _short_chapter(line):
    """-> (chapter_id, chapter_title, chapter_note) for a '## ' heading, or None"""
    for rx, kind in ((CHAPTER_HDR, 'n'), (PCHAP_HDR, 'p')):
        m = rx.match(line)
        if m:
            cid = m.group(1) if kind == 'n' else m.group(1)
            title = m.group(2)
            break
    else:
        m = EPI_HDR.match(line)
        if not m:
            return None
        cid, title = 'EP', m.group(1)
    note = ''
    mm = re.match(r'^(.*?)\s*(\*\(.*\)\*)\s*$', title)
    if mm:
        title, note = mm.group(1), mm.group(2)
    return cid, title.strip(), note


def _split_speakers(who):
    return [w.strip() for w in re.split(r' and ', who) if w.strip()]


def _build_scene(lines, start, end, ctx, path):
    """lines[start:end] is the scene block (end exclusive)."""
    m = SCENE_HDR.match(lines[start])
    sid, title = m.group(1), m.group(2)
    i = start + 1
    header_lines = []
    while i < end and LABEL_LINE.match(lines[i]):
        header_lines.append(lines[i])
        i += 1
    body_all = lines[i:end]
    # peel the tail: trailing blanks and END markers
    k = len(body_all)
    while k > 0 and (not body_all[k - 1].strip() or END_MARK.match(body_all[k - 1])):
        k -= 1
    body, tail = body_all[:k], body_all[k:]

    fields = {}       # lowercase label -> raw text after the label
    header = []       # [(label, raw_text, raw_line)] in order
    for hl in header_lines:
        if hl.startswith('**Mode:**'):
            mm = MODE_LINE.match(hl)
            if mm:
                fields['mode'] = mm.group(1)
                fields['gameplay'] = mm.group(2) or ''
                header.append(('Mode', mm.group(1), hl))
                header.append(('Gameplay', fields['gameplay'], hl))
                continue
        lm = LABEL_LINE.match(hl)
        label, text = lm.group(1), lm.group(2).strip()
        fields[label.lower()] = text
        header.append((label, text, hl))

    speakers, counts, words, spoken, dirs, notes = [], {}, 0, 0, 0, 0
    nonblank = 0
    for ln in body:
        kind = classify(ln)
        if kind != 'blank':
            nonblank += 1
        if kind == 'dialogue':
            sm = SPEAKER.match(ln)
            spoken += 1
            who = sm.group('who')
            if who not in counts:
                speakers.append(who)
                counts[who] = 0
            counts[who] += 1
            words += len(INLINE_DIR.sub('', sm.group('text')).split())
        elif kind == 'direction':
            dirs += 1
        elif kind == 'note':
            notes += 1
    indiv = []
    for who in speakers:
        for w in _split_speakers(who):
            if w not in indiv:
                indiv.append(w)

    sc = {
        'id': sid,
        'title': title,
        'title_plain': re.sub(r'\s*\*\(.*\)\*\s*$', '', title),
        'file': os.path.basename(path),
        'path': path,
        'chapter_id': ctx['chapter_id'],
        'chapter_title': ctx['chapter_title'],
        'chapter_note': ctx['chapter_note'],
        'chapter_heading': ctx['chapter_heading'],
        'act': ctx['act'],
        'act_title': ctx['act_title'],
        'act_heading': ctx['act_heading'],
        'heading_line': lines[start],
        'heading_lineno': start + 1,          # 1-based line number in the source file
        'body_lineno': i + 1,                 # 1-based line number of body[0]
        'header_lines': header_lines,
        'header': header,
        'mode': fields.get('mode', ''),
        'gameplay': fields.get('gameplay', ''),
        'cinematic': fields.get('cinematic', ''),
        'scene': fields.get('scene', ''),
        'setting': fields.get('setting', ''),
        'other_header': {k: v for k, v in fields.items()
                         if k not in ('mode', 'gameplay', 'cinematic', 'scene', 'setting')},
        'body': body,
        'tail': tail,
        'speakers': speakers,                  # as written, first-appearance order ('DUY and DEVIN' stays whole)
        'speaker_lines': [(w, counts[w]) for w in speakers],
        'speakers_individual': indiv,          # 'A and B' split into A, B
        'n_body_lines': len(body),
        'n_nonblank': nonblank,
        'n_spoken_lines': spoken,
        'n_spoken_words': words,               # words in dialogue text, inline *(directions)* removed
        'n_direction_lines': dirs,
        'n_note_lines': notes,
    }
    return sc


def file_segments(path):
    """Cover the whole file with ('gap', lines) / ('scene', scene) segments, in order."""
    lines = read_lines(path)
    segs, gap = [], []
    act = chap = None
    ctx = {'chapter_id': '', 'chapter_title': '', 'chapter_note': '', 'chapter_heading': '',
           'act': '', 'act_title': '', 'act_heading': ''}
    # the act named in the file's own H1 ('PART 2: ACT II — ...'); Part 1 and 5 are set by later headings
    if lines and lines[0].startswith('# '):
        fm = FILE_ACT.match(lines[0])
        if fm:
            ctx.update(act='Act '+fm.group(2), act_title=fm.group(3), act_heading=lines[0])
        elif 'FINALE' in lines[0]:
            ctx.update(act='Finale', act_title='FINALE', act_heading=lines[0])
    i, n = 0, len(lines)
    while i < n:
        ln = lines[i]
        if SCENE_HDR.match(ln):
            j = i + 1
            while j < n and not is_boundary(lines[j]):
                j += 1
            if gap:
                segs.append(('gap', gap))
                gap = []
            segs.append(('scene', _build_scene(lines, i, j, ctx, path)))
            i = j
            continue
        am = ACT_HDR.match(ln)
        if am:
            ctx.update(act='Act '+am.group(2), act_title=am.group(3), act_heading=ln)
        elif PROLOGUE_HDR.match(ln):
            ctx.update(act='Prologue', act_title=PROLOGUE_HDR.match(ln).group(1), act_heading=ln,
                       chapter_id='', chapter_title='', chapter_note='', chapter_heading='')
        else:
            ch = _short_chapter(ln)
            if ch:
                ctx.update(chapter_id=ch[0], chapter_title=ch[1], chapter_note=ch[2], chapter_heading=ln)
                if ch[0] == 'EP':
                    ctx.update(act='Epilogue', act_title=ch[1], act_heading=ln)
                elif ch[0].isdigit() and int(ch[0]) >= 23:
                    ctx.update(act='Finale', act_title='FINALE')
        gap.append(ln)
        i += 1
    if gap:
        segs.append(('gap', gap))
    return segs


def parse(path):
    return [s for kind, s in file_segments(path) if kind == 'scene']


def default_dir():
    return os.environ.get('SCRIPT_DIR', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'design', 'forgotten_third', 'build', 'draft55'))


def script_files(d=None):
    return sorted(glob.glob(os.path.join(d or default_dir(), '04[a-e]_*.md')))


def parse_all(d=None):
    out = []
    for f in script_files(d):
        out.extend(parse(f))
    return out


def reassemble(scene):
    """Exact original text of the scene block (no trailing newline added)."""
    return '\n'.join([scene['heading_line']] + scene['header_lines'] + scene['body'] + scene['tail'])


def reassemble_file(path):
    parts = []
    for kind, x in file_segments(path):
        parts.extend(x if kind == 'gap' else reassemble(x).split('\n'))
    return '\n'.join(parts)


def self_test(d=None, verbose=True):
    ok = True
    files = script_files(d)
    scenes = []
    for f in files:
        with open(f, encoding='utf-8', newline='') as fh:
            orig = fh.read()
        segs = file_segments(f)
        rebuilt = []
        for kind, x in segs:
            rebuilt.extend(x if kind == 'gap' else reassemble(x).split('\n'))
        same = '\n'.join(rebuilt) == orig
        # scene-by-scene: the block text must appear verbatim in the source, in order
        pos = 0
        for kind, x in segs:
            if kind == 'scene':
                blk = reassemble(x)
                p = orig.find(blk, pos)
                if p < 0:
                    same = False
                    print('  scene block not found verbatim:', x['id'])
                else:
                    pos = p + len(blk)
                scenes.append(x)
        n = sum(1 for k, _ in segs if k == 'scene')
        raw_n = sum(1 for l in orig.split('\n') if l.startswith('### SC-'))
        if verbose:
            print('%-34s scenes=%2d headings=%2d file reassembly byte-identical: %s' % (os.path.basename(f), n, raw_n, same))
        ok &= same and n == raw_n
    ids = [s['id'] for s in scenes]
    dup = sorted({i for i in ids if ids.count(i) > 1})
    if verbose:
        print('total scenes: %d (expected 97); unique ids: %d; duplicates: %s' % (len(ids), len(set(ids)), dup or 'none'))
    ok &= len(ids) == 97 and not dup
    missing = [s['id'] for s in scenes if not (s['mode'] and s['cinematic'] and s['scene'] and s['setting'])]
    if verbose:
        print('scenes missing a Mode/Cinematic/Scene/Setting header field: %s' % (missing or 'none'))
    ok &= not missing
    noact = [s['id'] for s in scenes if not s['act'] or not s['chapter_id']]
    if verbose:
        print('scenes with no act/chapter context: %s' % (noact or 'none'))
    ok &= not noact
    if verbose:
        print('SELF-TEST', 'PASS' if ok else 'FAIL')
    return ok


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    sys.exit(0 if self_test(args[0] if args else None) else 1)
