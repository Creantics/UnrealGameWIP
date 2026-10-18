#!/usr/bin/env python3
"""extract_lines.py - parse a Forgotten Third draft folder into a VO line skeleton CSV.

    python3 -I tools/vo_direction/extract_lines.py design/forgotten_third/build/draft56 \
        -o design/forgotten_third/voice/directions/draft56_skeleton.csv [--files 04a] [--summary]

Reads every 04*.md in the folder (or only those whose name starts with a --files prefix) and writes
one row per recordable line: plain dialogue, ROAST/REASSURE barks (one row each), choice-option
lines, lines embedded in hidden VARIANT/ACTING/GAMEPLAY notes, held-silence lines ("DEVIN: ...")
and camp lines (which keep their authored <sub>VO-...</sub> id as line_id).

Lines embedded inside other blocks are rows too:
  * `SPEAKER (how): text` runs written mid-paragraph in `*[direction.hidden · VARIANT/OPTIONAL]*`
    notes and in OPTIONAL bullets (`- **The window.** ... DEVIN (to himself): ...`), one row per run;
  * quotes in a stage direction that the prose attributes to a voice ("the board says: "...",
    "Torrin begins to answer, "..."", "Another: "Marek."", ""It always was," he says"). Written
    text (notes, margins, signs, title cards) is not a row. Speaker = a known name in the clause,
    or the quoted word itself when it is a known name (a roll call), else UNKNOWN; parse_note CHECK.

`variant` comes from, in order: an inline label (`**[VARIANT: ...]**`, `(ROAST)`, `(if ...)`), a
ROAST:/REASSURE:/`If X:` label earlier in the same note (it stays in force for later speakers in
that note), and the enclosing condition block: `**[VARIANT: ...]**`, `**▸ "..."**` branches,
`***CHOICE.** If "X":` / `***CHOICE.** Concede (...):` blockquotes and plain `*If X:*` labels.
A plain label followed directly by text covers that paragraph (up to the next blank line); one
followed by a blank line covers everything up to `*(Returns to the choice.)*`, the next label,
`*When ...:*`, a CHOICE marker, `---` or a header. Nested labels are joined with '; '
(`if ASK MINA; if first pick`). A label saying the answer "continues" covers its paragraph only.

Columns: see vo_common.SKELETON_COLUMNS and design/forgotten_third/voice/line_direction_schema.md.
Rows the parser had to guess about carry a non-empty `parse_note` beginning with "CHECK".
Read-only on the script. Standard library only.
"""
import argparse
import collections
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vo_common import (SKELETON_COLUMNS, fold, inline_directions, spoken, text_hash,  # noqa: E402
                       words_key, write_csv)

SCENE_HDR = re.compile(r'^### (SC-[A-Za-z0-9\-]+) · (.*)$')
CAMP_HDR = re.compile(r'^#{2,3} (CP-[A-Za-z0-9\-]+) · (.*)$')
ACT_HDR = re.compile(r'^# ACT ([IVX]+)\b')
FILE_ACT = re.compile(r'^# .*PART \d+: ACT ([IVX]+)\b')
VO_SUB = re.compile(r'^<sub>(VO-[A-Za-z0-9\-]+)</sub>\s*$')

NOT_SPEAKERS = {'ROAST', 'REASSURE', 'CHOICE', 'VARIANT', 'ACTING', 'OPTIONAL', 'HOLD', 'TIMED',
                'LOOP', 'GAMEPLAY', 'NOTE', 'END', 'CMB', 'ENG', 'PLAY', 'CG', 'CHC', 'SQ', 'OR'}

# Optional leading wrappers: blockquote, bullet, a bold VARIANT label.
PREFIX = re.compile(r'^(?P<bq>>\s?)?(?P<bul>-\s)?(?:\*\*\[VARIANT: (?P<vl>[^\]]*)\]\*\*\s+)?')
SPEAKER = re.compile(
    r"^(?P<who>[A-Z][A-Z0-9.'’\-/É]*(?: [A-Z][A-Z0-9.'’\-/É]*)*(?: (?:and|&) [A-Z][A-Z0-9.'’\-/É]*(?: [A-Z][A-Z0-9.'’\-/É]*)*)?)"
    r"(?: \*?\((?P<how>(?:[^()]|\([^()]*\))*)\)\*?)?: (?P<text>.*)$")
QUOTE = re.compile(r'"([^"]+)"|“([^”]+)”')
GAP_SPEAKER = re.compile(
    r"(?:(?P<label>[A-Za-z_][A-Za-z_ ]{0,30}): )?(?P<who>[A-Z][A-Z'’\-]+(?: [A-Z][A-Z'’\-]+)*)"
    r"(?: \((?P<how>(?:[^()]|\([^()]*\))*)\))?: \*?$")
GAP_LABEL = re.compile(r'(?P<label>ROAST|REASSURE)\s*(?:▸\s*)?:?\s*$')
NOTE_START = re.compile(r'^(?:>\s?)?(?:\*\*\*|\*\*\[|\[|\*)')
BRANCH = re.compile(r'^\*\*▸ "([^"]+)"\*\*')
CHOICE_IF = re.compile(r'^\*\*\*CHOICE\.\*\* If "([^"]+)"')
VARIANT_BLOCK = re.compile(r'^\*\*\[VARIANT: ([^\]]*)\]\*\*\s*(?:\*\(([^)]*)\)\*)?\s*$')
OPTION_HINT = re.compile(r"\((\w+)'s options\)")
BREATH_BARK = re.compile(r'/\s*\((breath[^)]*)\)')
IF_LABEL = re.compile(r'^\*(?P<cond>(?:If|Concede)\b[^*]*?):\*\s*$')          # *If "X":*  *If ASK MINA:*
WHEN_LABEL = re.compile(r'^\*(?:When|Both variants)\b[^*]*:\*\s*$')
CHOICE_CONCEDE = re.compile(r'^\*\*\*CHOICE\.\*\* (Concede[^:]*):')
OPEN_END = re.compile(r'^(?:>\s?)?(?:\[CHOICE|\*\*\[CHOICE|\*\*\*CHOICE|\*\*\*VARIANT|\*\*Pick\b|\*\*Tag\b)')
NOTE_IF = re.compile(r'(?:^|[\s.;:*])(If|Concede)\b([^:"“]{0,80}?):\s*\**\s*$')
SPEECH = re.compile(r"\b(?:says|said|asks|asked|answers|answered|answer|calls|called|whispers|whispered|"
                    r"murmurs|murmured|shouts|shouted|replies|replied|voice|Another)\b")
WRITTEN = re.compile(r"\b(?:writes?|wrote|written|reads|lettered|chalk|margin|title card|sign|caption|"
                     r"graphic|print|notebook|page|note|credits|screen)\b", re.I)
POST_ATTR = re.compile(r"^\s*,?\*?\s*(?:he|she|they|it|[A-Z][a-z]+)\s+(?:says|said|asks|answers|whispers|calls)\b")

LANE_DEFAULT = {'D': 'DEVIN', 'F': 'DEVIN', 'U': 'DUY'}


def act_for_file(path):
    name = os.path.basename(path)
    return {'04a': 'I', '04b': 'II', '04c': 'III', '04d': 'IV', '04e': 'V'}.get(name[:3], '?')


def clean_how(how):
    return re.sub(r'\s+', ' ', (how or '').strip())


def variant_from_how(how):
    h = how or ''
    m = re.search(r'\b(ROAST|REASSURE)\b', h)
    if m:
        return m.group(1)
    m = re.match(r'\s*if\s+(.*)$', h, re.I)
    if m:
        return 'if ' + m.group(1).strip()
    return ''


def cond_text(c):
    """'If "X"' -> 'if "X"'; 'Concede (third miss)' -> 'concede (third miss)'; backticks dropped."""
    c = re.sub(r'\s+', ' ', c.replace('`', '')).strip()
    return c[:1].lower() + c[1:]


def is_speaker(who):
    parts = re.split(r' (?:and|&) ', who)
    for p in parts:
        if p.split(' ')[0] in NOT_SPEAKERS or len(re.sub(r'[^A-Z]', '', p)) < 2:
            return False
    return True


class Extractor:
    def __init__(self, known_names, speakers=()):
        self.known = known_names          # {'Devin': 'DEVIN', ...}
        self.speakers = set(speakers)     # every full speaker label seen at a line start
        self.rows = []
        alt = '|'.join(re.escape(w) for w in sorted(self.speakers, key=len, reverse=True))
        self.inline_label = re.compile(
            r"(?<![A-Za-z'’])(?P<who>%s)(?: \*?\((?P<how>(?:[^()]|\([^()]*\))*)\)\*?)?: " % alt) if alt else None

    # ---- row creation -------------------------------------------------------------------
    def add(self, ctx, lineno, speaker, kind, variant, how, text, note=''):
        dirs = inline_directions(text)
        delivery = ' | '.join([d for d in [clean_how(how)] + dirs if d])
        sp = spoken(text)
        if kind == 'line' and not words_key(sp):
            kind = 'silence'
        row = {
            'vo_id': '', 'file': ctx['file'], 'act': ctx['act'], 'scene': ctx['scene'],
            'scene_title': ctx['title'], 'speaker': speaker, 'kind': kind, 'variant': variant,
            'delivery_original': delivery, 'text': text.strip(), 'spoken_text': sp,
            'text_hash': text_hash(text), 'parse_note': note,
            'source_line': '%s:%d' % (ctx['file'], lineno),
        }
        self.rows.append(row)
        return row

    # ---- notes with embedded quoted lines -------------------------------------------------
    def embedded(self, ctx, lineno, line, is_choice_note, is_variant_note, is_bark_note, variant):
        out = []
        cur = None            # (speaker, how, label)
        last_end = 0
        note_label = ''       # ROAST / REASSURE label still in force for later speakers in this note
        note_cond = ''        # 'if X' from an "If X:" clause earlier in this note
        for m in QUOTE.finditer(line):
            q = m.group(1) or m.group(2)
            gap = line[last_end:m.start()]
            last_end = m.end()
            ic = NOTE_IF.search(gap.rstrip())
            if ic:
                note_cond, note_label = cond_text(ic.group(1) + ic.group(2)), ''
            g = GAP_SPEAKER.search(gap.rstrip(' *').rstrip() + ' ') if gap.strip() else None
            g = GAP_SPEAKER.search(gap.rstrip()) if not g else g
            if g and is_speaker(g.group('who')):
                lab = (g.group('label') or '').strip()
                if lab.upper() in ('ROAST', 'REASSURE'):
                    note_label = lab.upper()
                cur = (g.group('who'), g.group('how') or '', lab or note_label)
            elif gap.strip() in ('/', ''):
                pass                               # "a" / "b": continuation of the same speaker
            else:
                lab = GAP_LABEL.search(gap.rstrip())
                if lab:
                    note_label = lab.group('label')
                cur = None
                if lab and is_choice_note:
                    cur = ('', '', lab.group('label'))
                elif is_bark_note:
                    names = re.findall(r'\b(%s)\b' % '|'.join(self.known), gap) if self.known else []
                    if names:
                        cur = (self.known[names[-1]], '', '')
            if cur is None and is_choice_note:
                out.append(('', 'choice_option', variant, '', q, 'CHECK speaker of choice option'))
                continue
            if cur is None:
                if is_variant_note and not CHOICE_IF.match(line):
                    out.append(('UNKNOWN', 'line', note_cond or variant, '', q,
                                'CHECK quoted line in VARIANT note has no speaker label'))
                continue
            who, how, label = cur
            v = label if label else (variant_from_how(how) or note_cond or variant)
            if label and label.upper() in ('ROAST', 'REASSURE'):
                v = label.upper()
            kind = 'choice_option' if (is_choice_note and not who) else ('bark' if is_bark_note or 'bark' in how else 'embedded')
            out.append((who, kind, v, how, q, 'embedded in note' if who else 'CHECK speaker of choice option'))
        if is_bark_note:
            for b in BREATH_BARK.finditer(line):
                spk = out[-1][0] if out else ''
                out.append((spk, 'nonverbal', variant, b.group(1), '', 'breath-only bark'))
        for who, kind, v, how, q, note in out:
            self.add(ctx, lineno, who, kind, v, how, q, note)
        return bool(out)

    # ---- SPEAKER (how): text runs written mid-paragraph in hidden notes / OPTIONAL bullets -----
    def inline_labels(self, ctx, lineno, line, variant):
        if not self.inline_label:
            return False
        ms = [m for m in self.inline_label.finditer(line) if is_speaker(m.group('who'))]
        if not ms:
            return False
        flag = re.search(r'\bReads\s+`?(F_\w+)', line)
        bullet = re.match(r'^-\s+\*\*(.+?)\.?\*\*', line)
        cond = 'optional: ' + bullet.group(1).strip().lower() if bullet else ''
        start = 0
        for k, m in enumerate(ms):
            prefix = line[start:m.start()]
            c = re.search(r'(?:^|[\s.;:*])(If|Concede)\b([^:]{0,80}):\s*$', prefix)
            if c:
                cond = cond_text(c.group(1) + c.group(2))
                if flag and re.match(r'if [≥≤<>=]', cond):
                    cond = 'if %s %s' % (flag.group(1), cond[3:])
            end = ms[k + 1].start() if k + 1 < len(ms) else len(line)
            seg = line[m.end():end]
            cut = re.search(r'\s(?:Then play\b|If\b[^:]{0,80}:|Reads\s+`?F_)', seg)
            start = m.end() + (cut.start() if cut else len(seg))
            seg = seg[:cut.start()] if cut else seg
            seg = seg.strip()
            if seg.count('*') % 2:
                seg = seg.rstrip('*').strip()
            if not words_key(spoken(seg)):
                continue
            v = variant_from_how(m.group('how')) or cond or variant
            self.add(ctx, lineno, m.group('who'), 'embedded', v, m.group('how') or '', seg,
                     'embedded in note')
        return True

    # ---- quotes a stage direction attributes to a voice ----------------------------------------
    def stage_quotes(self, ctx, lineno, line, variant):
        multi = {k.lower(): k for k in self.speakers if ' ' in k and ' and ' not in k}
        last_end, prev_spoken, added, roll = 0, False, False, False
        for m in QUOTE.finditer(line):
            q = m.group(1) or m.group(2)
            gap = line[last_end:m.start()]
            last_end = m.end()
            clause = re.split(r'[.!?](?:\*)?\s+', gap)[-1]
            after = line[m.end():m.end() + 40]
            cont = prev_spoken and not gap.strip(' *')
            said = (SPEECH.search(clause) and not WRITTEN.search(clause)) or POST_ATTR.match(after)
            prev_spoken = bool(cont or said)
            if not prev_spoken or not words_key(spoken(q)):
                continue
            who, note = '', 'CHECK speaker of line embedded in stage direction'
            names = [(x.start(), self.known[x.group(1)]) for x in
                     re.finditer(r'\b(%s)\b' % '|'.join(map(re.escape, self.known)), clause)] if self.known else []
            names += [(clause.lower().rfind(k), v) for k, v in multi.items() if k in clause.lower()]
            if names:
                who = max(names)[1]
                note = 'embedded in stage direction; speaker from the prose'
            elif roll or re.search(r'\bAnother\b', clause):
                roll = True          # a roll call: each voice says its own name
                bare = words_key(spoken(q)).title()
                if bare in self.known:
                    who, note = self.known[bare], 'embedded in stage direction; speaker says own name'
            self.add(ctx, lineno, who or 'UNKNOWN', 'embedded', variant, '', q, note)
            added = True
        return added

    # ---- per file --------------------------------------------------------------------------
    def run_file(self, path):
        with open(path, encoding='utf-8') as fh:
            lines = fh.read().split('\n')
        fname = os.path.basename(path)
        file_act = act_for_file(path)
        ctx = {'file': fname, 'act': 'Prologue' if fname.startswith('04a') else file_act,
               'scene': '', 'title': ''}
        variant, vkind = '', ''
        ifs = []              # plain *If X:* labels in force: [cond, ends_at_blank, seen_content]
        option_hint = ''
        last_row = None
        chapter_intro = False

        def eff():
            return '; '.join(x for x in [variant] + [c[0] for c in ifs] if x)

        for i, raw in enumerate(lines, 1):
            line = raw.rstrip()
            s = line.strip()
            if not s:
                while ifs and ifs[-1][1] and ifs[-1][2]:
                    ifs.pop()
                continue
            m = VO_SUB.match(s)
            if m:
                if last_row is not None and not last_row['vo_id']:
                    last_row['vo_id'] = m.group(1)
                continue
            last_row = None
            if s.startswith('#') or s == '---':
                ifs = []
            if s.startswith('# '):
                a = ACT_HDR.match(s) or FILE_ACT.match(s)
                if a:
                    ctx['act'] = a.group(1)
                continue
            if s.startswith('## ') or CAMP_HDR.match(s):
                variant = ''
                chapter_intro = s.startswith('## CHAPTER')
                if s.startswith('## EPILOGUE'):
                    ctx['act'] = 'Epilogue'
                elif s.startswith('## PROLOGUE'):
                    ctx['act'] = 'Prologue'
                elif s == '## Camps':
                    ctx['act'] = file_act
                m = CAMP_HDR.match(s)
                if m:
                    ctx['scene'], ctx['title'] = m.group(1), re.sub(r'\s*\(level[^)]*\)\s*$', '', m.group(2).strip()).strip().strip('"')
                    chapter_intro = False
                continue
            m = SCENE_HDR.match(s)
            if m:
                ctx['scene'], ctx['title'] = m.group(1), m.group(2).strip()
                variant = ''
                option_hint = ''
                chapter_intro = False
                continue
            if s.startswith('|') or s.startswith('#') or s == '---' or not ctx['scene']:
                continue
            # ---- plain *If X:* condition labels ----
            m = IF_LABEL.match(s)
            if m:
                cond = cond_text(m.group('cond'))
                nxt = lines[i].strip() if i < len(lines) else ''
                top_level = not nxt
                ends_at_blank = bool(nxt) or 'continues' in cond
                if top_level:
                    ifs = []
                else:
                    while ifs and ifs[-1][1]:
                        ifs.pop()
                ifs.append([cond, ends_at_blank, False])
                continue
            for f in ifs:
                f[2] = True
            if WHEN_LABEL.match(s) or OPEN_END.match(s):
                ifs = []
            if 'Returns to the choice' in s:
                ifs = []            # the line itself is a stage note, never a row
            # ---- variant / branch context markers ----
            m = VARIANT_BLOCK.match(s)
            if m:
                variant, vkind = m.group(1).strip(), 'block'
                if m.group(2):
                    variant += ' (%s)' % m.group(2).strip()
                continue
            m = BRANCH.match(s)
            if m:
                variant, vkind = '▸ ' + m.group(1), 'branch'
                continue
            m = CHOICE_IF.match(s)
            if m:
                variant, vkind = 'if "%s"' % m.group(1), 'choice_if'
                continue
            m = CHOICE_CONCEDE.match(s)
            if m:
                variant, vkind = cond_text(m.group(1)), 'choice_if'
                continue
            if s in ('**Base scene**',) or s.startswith('**[LOOP') or s.startswith('*Both variants'):
                variant = ''
                continue
            # scope of the current variant context
            if variant and vkind == 'block' and (
                    (s.startswith('*') and not s.startswith('**') and not s.startswith('*(')) or
                    s.startswith('[') or s.startswith('***') or s.startswith('> **[') or
                    s.startswith('**[CHOICE') or s.startswith('> **[CHOICE')):
                variant = ''
            if variant and vkind == 'branch' and ('[CHOICE' in s or s.startswith('***CHOICE')):
                variant = ''
            if variant and vkind == 'choice_if' and not s.startswith('>'):
                variant = ''
            m = OPTION_HINT.search(s)
            if m and 'options' in s:
                option_hint = m.group(1).upper()
                continue
            # ---- audit options:  > 1. "Into me."
            m = re.match(r'^(?:>\s?)?\d+\.\s+"([^"]+)"\s*$', s)
            if m:
                spk = option_hint or LANE_DEFAULT.get(lane_of(ctx['scene']), '')
                self.add(ctx, i, spk, 'choice_option', eff(), '', m.group(1),
                         'speaker from options header' if option_hint else 'CHECK speaker of choice option')
                continue
            # ---- spoken line, possibly wrapped ----
            p = PREFIX.match(s)
            body = s[p.end():]
            sm = SPEAKER.match(body)
            if sm and is_speaker(sm.group('who')) and not body.startswith('**'):
                who, how, text = sm.group('who'), sm.group('how') or '', sm.group('text')
                v = p.group('vl') or variant_from_how(how) or eff()
                if p.group('vl'):
                    vv = re.search(r'\b(ROAST|REASSURE)\b', p.group('vl'))
                    v = vv.group(1) if vv else p.group('vl')
                pick = re.match(r'^\*\[pick\]\*\s*(.*)$', text)
                if pick:
                    for q in QUOTE.finditer(pick.group(1)):
                        self.add(ctx, i, who, 'choice_option', v, how, q.group(1) or q.group(2),
                                 'pick option')
                    continue
                kind = 'bark' if ('bark' in how.lower() or p.group('bul') and v in ('ROAST', 'REASSURE')) else 'line'
                last_row = self.add(ctx, i, who, kind, v, how, text)
                continue
            # ---- notes / bracket lines that may carry quoted lines ----
            quoted = '"' in s or '“' in s
            done = False
            if quoted:
                choice_note = bool(re.match(r'^(?:>\s?)?(?:\*\*\[CHOICE|\*\*\*CHOICE \(|\*Choice:\*)', s))
                variant_note = bool(re.match(r'^\*\*\*VARIANT', s))
                bark_note = bool(re.search(r'\bbarks?\b|lane call', s, re.I)) and not choice_note
                labelled = re.search(r"[A-Z]{2,}(?: \((?:[^()]|\([^()]*\))*\))?: \*?[\"“]", s)
                if choice_note or variant_note or (NOTE_START.match(s) and (bark_note or labelled)):
                    done = self.embedded(ctx, i, s, choice_note, variant_note, bark_note, eff()) or True
            hidden = s.startswith('*[direction.hidden') and 'ACTING' not in s[:40]
            if not done and (hidden or s.startswith('- **')):
                done = self.inline_labels(ctx, i, s, eff())
            if (not done and quoted and not chapter_intro and s.startswith('*')
                    and not s.startswith('**') and not s.startswith('*[direction.hidden · ACTING')):
                self.stage_quotes(ctx, i, s, eff())
            continue
        return

    def finish(self):
        # sequence numbers per scene, line ids, choice-option speaker inference
        seqs = collections.Counter()
        for r in self.rows:
            seqs[r['scene']] += 1
            r['seq'] = str(seqs[r['scene']])
            r['line_id'] = r['vo_id'] or '%s#%03d' % (r['scene'], seqs[r['scene']])
        by_scene = collections.defaultdict(list)
        for r in self.rows:
            by_scene[r['scene']].append(r)
        for scene, rows in by_scene.items():
            for k, r in enumerate(rows):
                if r['kind'] != 'choice_option':
                    continue
                key = words_key(r['spoken_text'])
                dup = None
                for r2 in rows[k + 1:k + 40]:
                    if (r2['kind'] != 'choice_option' and key and words_key(r2['spoken_text']) == key
                            and (not r['speaker'] or r['speaker'] == r2['speaker'])):
                        dup = r2
                        break
                if dup:
                    if not r['speaker']:
                        r['speaker'] = dup['speaker']
                    r['parse_note'] = 'option label; also spoken as %s' % dup['line_id']
                elif not r['speaker']:
                    lane = LANE_DEFAULT.get(lane_of(scene), '')
                    r['speaker'] = lane or 'UNKNOWN'
                    r['parse_note'] = 'CHECK speaker of choice option (%s)' % (
                        'lane default' if lane else 'no lane')
        return self.rows


def lane_of(scene):
    m = re.match(r'SC-[A-Z0-9]+-([A-Z]+)', scene)
    if m:
        return m.group(1)[0]
    return ''


def collect_names(paths):
    names = {}
    for path in paths:
        with open(path, encoding='utf-8') as fh:
            raw_lines = fh.read().split('\n')
        for raw in raw_lines:
            s = raw.strip()
            p = PREFIX.match(s)
            m = SPEAKER.match(s[p.end():])
            if m and is_speaker(m.group('who')):
                for who in re.split(r' (?:and|&) ', m.group('who')):
                    if ' ' not in who:
                        names[who.title()] = who
    return names


def collect_speakers(paths):
    """Every full speaker label that opens a dialogue line, e.g. {'DEVIN', 'THE BOARD', ...}."""
    out = set()
    for path in paths:
        with open(path, encoding='utf-8') as fh:
            for raw in fh.read().split('\n'):
                s = raw.strip()
                p = PREFIX.match(s)
                m = SPEAKER.match(s[p.end():])
                if m and is_speaker(m.group('who')) and not s[p.end():].startswith('**'):
                    out.add(m.group('who'))
    return out


def extract(folder, prefixes=None):
    paths = sorted(glob.glob(os.path.join(folder, '04*.md')))
    if prefixes:
        paths = [p for p in paths if any(os.path.basename(p).startswith(x) for x in prefixes)]
    ex = Extractor(collect_names(paths), collect_speakers(paths))
    for p in paths:
        ex.run_file(p)
    return ex.finish()


def summary(rows, out=sys.stdout):
    acts = collections.Counter(r['act'] for r in rows)
    order = ['Prologue', 'I', 'II', 'III', 'IV', 'V', 'Epilogue']
    out.write('lines per act:\n')
    for a in order + sorted(set(acts) - set(order)):
        if acts.get(a):
            out.write('  %-9s %5d\n' % (a, acts[a]))
    files = collections.Counter(r['file'] for r in rows)
    out.write('lines per file:\n')
    for f in sorted(files):
        out.write('  %-34s %5d\n' % (f, files[f]))
    kinds = collections.Counter(r['kind'] for r in rows)
    out.write('lines per kind: %s\n' % ', '.join('%s %d' % kv for kv in kinds.most_common()))
    sp = collections.Counter(r['speaker'] for r in rows)
    out.write('lines per speaker:\n')
    for s, n in sp.most_common():
        out.write('  %-16s %5d\n' % (s, n))
    checks = [r for r in rows if r['parse_note'].startswith('CHECK')]
    out.write('rows needing a speaker check: %d\n' % len(checks))
    for r in checks:
        out.write('  %s %s %s: %s\n' % (r['line_id'], r['source_line'], r['speaker'], r['text'][:60]))
    out.write('total: %d\n' % len(rows))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('folder', help='draft folder with 04*.md')
    ap.add_argument('-o', '--out', help='CSV to write (default: stdout summary only)')
    ap.add_argument('--files', nargs='*', help='only files starting with these prefixes, e.g. 04a')
    ap.add_argument('--summary', action='store_true', help='print counts per act / speaker')
    a = ap.parse_args(argv)
    rows = extract(a.folder, a.files)
    if a.out:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        write_csv(a.out, SKELETON_COLUMNS, rows)
        sys.stderr.write('wrote %d rows to %s\n' % (len(rows), a.out))
    if a.summary or not a.out:
        summary(rows)
    return 0


if __name__ == '__main__':
    sys.exit(main())
