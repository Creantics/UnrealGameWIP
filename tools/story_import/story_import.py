#!/usr/bin/env python3
"""story_import.py - turn a story design package into the runtime story JSON.

    python3 -I tools/story_import/story_import.py --tables FILE --build-dir DIR [--out-dir DIR] [--check | --allow-problems]

--tables names the story's own tables (a JSON file kept outside the repo): the title, the worlds, the scene
count, the script folder and scene id prefix, the hold ladder and its anchors, the gameplay keywords, and the
hand-authored choices and exchanges. The tool itself holds no story data.

Reads (all under --build-dir):
    <script_dir>/04a..04e_*.md     the script, parsed with scriptparse.py
    <script_dir>/VO_Master_List.csv VO ids
    meta/meta_*.csv                one slate row per scene
    meta/inserts_*.csv             GAMEPLAY / CHOICE / HOLD / OPTIONAL / TIMED / VARIANT rows
    flags.csv, cues_final.csv

Writes (under --out-dir, default assets/story):
    story.json, scenes/<scene id>.json, loc/en.json

The schema is docs/story_format.md. --check rebuilds in memory, validates the spec's rules on the files
that are on disk, and fails if the files differ from a fresh build (stale or hand-edited output).
Output is deterministic: UTF-8, LF, ensure_ascii=False, indent 1, fixed key order, sorted where order is free.
"""
import argparse
import collections
import csv
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)   # python3 -I drops the script dir from sys.path
import scriptparse as sp   # noqa: E402

FORMAT = 1
GAMEPLAY_KINDS = ['CMB', 'ENG', 'PLAY', 'MINI']
BEAT_TYPES = ['line', 'direction', 'choice', 'gameplay', 'cue', 'flag', 'hold', 'end']

TELL_SECONDS = 1.75       # ordinary Tell: a tap and a hold
INPUT_HOLD_SECONDS = 2.0  # [HOLD: ...] input holds that the ladder does not name
# a Tell: a tap and a hold in the same sentence ("taps twice, and holds")
TELL_RX = re.compile(r'\btap(?:s|ping)?\b[^.]{0,80}?\bhold(?:s|ing)?\b', re.I)

# --------------------------------------------------------------------------------------------------
# the story's tables (--tables FILE, JSON), set by load_tables()
#
#   title, worlds              story.json's title; the worlds a scene's slate may name
#   scene_count                scenes the story must have
#   script_dir                 the script's folder under --build-dir
#   scene_id_prefix            the prefix every scene id starts with (beat ids use the rest)
#   hold_ladder                {scene: seconds}: the scene's Tell or input hold
#   ladder_anchor              {scene: regex}: where a ladder hold with no [HOLD] insert goes (default: TELL_RX)
#   gameplay_keywords          [[kind, regex], ...]: a gameplay insert's kind from its text, first match wins
#                              (after the leading kind word; no match: ENG)
#   choices                    {scene: [[label, scripted, [option, ...]], ...]} for the [CHOICE: ...] inserts, in
#                              order; option = {text, effects: [[op, flag, value]], next, selectable, miss};
#                              next 'self' means back to this prompt
#   grammar                    the script's heading grammar and act names (scriptparse.GRAMMAR_KEYS)
#   chapter_labels             {prologue, epilogue, chapter}: a scene's chapter heading; chapter holds one %d
#   mini                       {scene: [[anchor, items], ...]}: the exchange that replaces a MINI insert.
#                              anchor null: right after the insert's hidden MINI direction; else a unique
#                              substring of a body line, the items go just before that line. items:
#                                ['line', speaker, text, delivery|null]  invented line, id PH-<scene>-<nnn>
#                                ['dir', text]                            shown direction
#                                ['hold', seconds, note]
#                                ['flag', op, flag, value|null]           set / clear / require / end
#                                ['label', name]                          a jump target for option `next`
#                                ['choice', label, scripted, [option, ...]]; an option's next may name a label
# --------------------------------------------------------------------------------------------------
TITLE = ''
WORLDS = []
SCENE_COUNT = 0
SCRIPT_DIR = ''
SCENE_ID_PREFIX = ''
HOLD_LADDER = {}
LADDER_ANCHOR = {}
GAMEPLAY_KEYWORDS = []
CHOICES = {}
MINI = {}
CHAPTER_LABELS = {}
MISSING_CHOICE = '[missing choice]'   # the placeholder for a CHOICE insert with no entry in the tables


def _text(v):
    return isinstance(v, str)


def _int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def _num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def _regex(where, v):
    if not _text(v):
        return '%s is not text' % where
    try:
        re.compile(v)
    except re.error as e:
        return '%s is not a valid regex: %s' % (where, e)
    return None


def _check_options(where, opts):
    if not isinstance(opts, list):
        return '%s: the options are not a list' % where
    for i, o in enumerate(opts):
        if not isinstance(o, dict) or not _text(o.get('text')):
            return '%s: option %d is not an object with text' % (where, i)
        if not isinstance(o.get('effects'), list):
            return '%s: option %d effects are not a list' % (where, i)
        for j, e in enumerate(o['effects']):
            if not isinstance(e, list) or len(e) != 3 or not _text(e[0]) or not _text(e[1]):
                return '%s: option %d effect %d is not an [op, flag, value] list' % (where, i, j)
        if not isinstance(o.get('selectable'), bool) or not isinstance(o.get('miss'), bool):
            return '%s: option %d needs selectable and miss (true or false)' % (where, i)
        nx = o.get('next', 0)
        if 'next' not in o or not (nx is None or _text(nx) or _int(nx)):
            return '%s: option %d next is not null, text or a beat index' % (where, i)
    return None


# A MINI item's kind and its fields after the kind: 't' text, 'o' text or null, 'b' true or false, 'O' options,
# 'n' seconds, 'a' anything.
_MINI_ITEMS = {'line': 'tto', 'dir': 't', 'hold': 'no', 'flag': 'tta', 'label': 't', 'choice': 'tbO'}


def _check_mini_item(where, it):
    if not isinstance(it, list) or not it or not _text(it[0]):
        return '%s is not a [kind, ...] list' % where
    shape = _MINI_ITEMS.get(it[0])
    if shape is None:
        return None  # an unknown kind is reported as a problem when the scene is built
    if len(it) != 1 + len(shape):
        return '%s (%s) needs %d fields after the kind' % (where, it[0], len(shape))
    for f, v in zip(shape, it[1:]):
        if f == 'O':
            why = _check_options(where, v)
            if why:
                return why
        elif not {'t': _text(v), 'o': v is None or _text(v), 'b': isinstance(v, bool), 'n': _num(v),
                  'a': True}[f]:
            return '%s (%s) has a field of the wrong type' % (where, it[0])
    return None


def check_tables(t):
    """The tables' shape; returns an error string, or None."""
    if not isinstance(t, dict):
        return 'the tables are not a JSON object'
    need = ('title', 'worlds', 'scene_count', 'script_dir', 'scene_id_prefix', 'hold_ladder', 'ladder_anchor',
            'gameplay_keywords', 'choices', 'mini', 'grammar', 'chapter_labels')
    missing = [k for k in need if k not in t]
    if missing:
        return 'the tables lack: %s' % ', '.join(missing)
    for k in ('title', 'script_dir', 'scene_id_prefix'):
        if not _text(t[k]) or not t[k]:
            return '%s is not non-empty text' % k
    if not isinstance(t['worlds'], list) or not all(_text(w) for w in t['worlds']):
        return 'worlds is not a list of text'
    if not _int(t['scene_count']) or t['scene_count'] < 0:
        return 'scene_count is not a whole number'
    if not isinstance(t['hold_ladder'], dict) or not all(_num(v) for v in t['hold_ladder'].values()):
        return 'hold_ladder is not an object of seconds'
    if not isinstance(t['ladder_anchor'], dict):
        return 'ladder_anchor is not an object'
    for k, v in t['ladder_anchor'].items():
        why = _regex('ladder_anchor[%s]' % k, v)
        if why:
            return why
    if not isinstance(t['gameplay_keywords'], list):
        return 'gameplay_keywords is not a list'
    for i, e in enumerate(t['gameplay_keywords']):
        if not isinstance(e, list) or len(e) != 2 or not _text(e[0]):
            return 'gameplay_keywords[%d] is not a [kind, regex] pair' % i
        why = _regex('gameplay_keywords[%d]' % i, e[1])
        if why:
            return why
    if not isinstance(t['choices'], dict):
        return 'choices is not an object'
    for sid, rows in t['choices'].items():
        if not isinstance(rows, list):
            return 'choices[%s] is not a list' % sid
        for i, r in enumerate(rows):
            if not isinstance(r, list) or len(r) != 3 or not _text(r[0]) or not isinstance(r[1], bool):
                return 'choices[%s][%d] is not a [label, scripted, options] row' % (sid, i)
            why = _check_options('choices[%s][%d]' % (sid, i), r[2])
            if why:
                return why
    if not isinstance(t['mini'], dict):
        return 'mini is not an object'
    for sid, rows in t['mini'].items():
        if not isinstance(rows, list):
            return 'mini[%s] is not a list' % sid
        for i, r in enumerate(rows):
            if (not isinstance(r, list) or len(r) != 2 or not (r[0] is None or _text(r[0]))
                    or not isinstance(r[1], list)):
                return 'mini[%s][%d] is not an [anchor, items] pair' % (sid, i)
            for j, it in enumerate(r[1]):
                why = _check_mini_item('mini[%s][%d] item %d' % (sid, i, j), it)
                if why:
                    return why
    cl = t['chapter_labels']
    if not isinstance(cl, dict) or not all(_text(cl.get(k)) for k in ('prologue', 'epilogue', 'chapter')):
        return 'chapter_labels needs text for prologue, epilogue and chapter'
    try:
        ok = cl['chapter'].count('%d') == 1 and isinstance(cl['chapter'] % 1, str)
    except (TypeError, ValueError):
        ok = False
    if not ok:
        return 'chapter_labels.chapter needs exactly one %d and no other % (write %% for a percent sign)'
    try:
        sp.set_grammar(t['grammar'])
    except ValueError as e:
        return str(e)
    return None


def load_tables(path):
    """Read the story's tables; returns an error string, or None."""
    global TITLE, WORLDS, SCENE_COUNT, SCRIPT_DIR, SCENE_ID_PREFIX, HOLD_LADDER, LADDER_ANCHOR
    global GAMEPLAY_KEYWORDS, CHOICES, MINI, CHAPTER_LABELS
    try:
        with open(path, encoding='utf-8-sig') as fh:
            t = json.load(fh)
    except (OSError, ValueError) as e:
        return 'cannot read the tables: %s' % e
    why = check_tables(t)
    if why:
        return why
    TITLE, WORLDS, SCENE_COUNT = t['title'], t['worlds'], t['scene_count']
    SCRIPT_DIR, SCENE_ID_PREFIX, HOLD_LADDER = t['script_dir'], t['scene_id_prefix'], t['hold_ladder']
    LADDER_ANCHOR = {k: re.compile(v) for k, v in t['ladder_anchor'].items()}
    GAMEPLAY_KEYWORDS = [(k, re.compile(rx)) for k, rx in t['gameplay_keywords']]
    CHOICES, MINI, CHAPTER_LABELS = t['choices'], t['mini'], dict(t['chapter_labels'])
    sp.set_scene_prefix(SCENE_ID_PREFIX)
    return None


# --------------------------------------------------------------------------------------------------
# text helpers
# --------------------------------------------------------------------------------------------------
def fold(t):
    """Fold quotes, ellipsis, dashes and whitespace so script text and CSV text compare equal."""
    t = (t.replace('“', '"').replace('”', '"').replace('‘', "'").replace('’', "'")
          .replace('…', '...'))
    return re.sub(r'\s+', ' ', t).strip()


def norm(t):
    """VO / script comparison form: markdown emphasis stripped, then folded."""
    return fold(t.replace('*', ''))


def norm_nodir(t):
    return norm(sp.INLINE_DIR.sub(' ', t))


def titlecase(s):
    small = {'and', 'of', 'the', 'a', 'in', 'to', 'for', 'at', 'on'}
    out = []
    for i, w in enumerate(s.lower().split(' ')):
        out.append(w if (i and w in small) or not w else w[0].upper() + w[1:])
    return ' '.join(out)


def direction_text(line):
    """Stage-direction line -> display text. A line that is one whole *...* span loses the outer stars."""
    s = line.strip()
    if s.startswith('**') and s.endswith('**') and s.count('**') == 2:
        return s[2:-2].strip()
    if s.startswith('*') and s.endswith('*') and not s.startswith('**') and s.count('*') % 2 == 0:
        inner = s[1:-1]
        if re.sub(r'\*\([^)]*\)\*', '', inner).count('*') % 2 == 0:
            return inner.strip()
    return s


def chapter_id(cid):
    return cid.zfill(2) if cid.isdigit() else cid


def chapter_title(sc):
    cid = sc['chapter_id']
    base = titlecase(sc['chapter_title'])
    if cid == 'EP':
        head = CHAPTER_LABELS['epilogue']
    elif cid.isdigit():
        head = CHAPTER_LABELS['chapter'] % int(cid)
    else:   # the grammar's part_heading ids
        head = CHAPTER_LABELS['prologue']
    note = re.sub(r'^\*|\*$', '', sc['chapter_note']).strip()
    return '%s · %s%s' % (head, base, (' ' + note) if note else '')


def csv_rows(path):
    with open(path, encoding='utf-8', newline='') as fh:
        return list(csv.DictReader(fh))


def csv_glob(pattern):
    rows = []
    for f in sorted(glob.glob(pattern)):
        rows.extend(csv_rows(f))
    return rows


def split_semis(s):
    return [x.strip() for x in s.split(';') if x.strip()]


def dump(obj):
    return json.dumps(obj, ensure_ascii=False, indent=1) + '\n'


TAP_RX = re.compile(r'\btap(?:s|ping)?\b', re.I)
HOVER_RX = re.compile(r'\bhover|\bhesitat', re.I)


def find_tell(sid, body):
    """Body line index after which the scene's Tell hold goes (slate tell_insert=yes scenes only).
    A ladder anchor wins; else the first line (direction or dialogue incl. delivery tag) with a tap and a hold in
    one sentence; else the first tap; else the first hover/hesitation."""
    lines = [(i, ln) for i, ln in enumerate(body) if sp.classify(ln) in ('direction', 'dialogue')]
    if sid in LADDER_ANCHOR:
        rxs = [LADDER_ANCHOR[sid]]
    else:
        rxs = [TELL_RX, TAP_RX, HOVER_RX]
    for rx in rxs:
        for i, ln in lines:
            if rx.search(ln.strip() if sid in LADDER_ANCHOR else direction_text(ln) if sp.classify(ln) == 'direction' else ln):
                return i
    return None


# --------------------------------------------------------------------------------------------------
# gameplay inference
# --------------------------------------------------------------------------------------------------
_HEAD_KIND = {'CMB': 'CMB', 'ENG': 'ENG', 'PLAY': 'PLAY', 'MINI': 'MINI', 'BNT': 'ENG', 'CHC': 'ENG', 'PROMPT': 'ENG',
              'PUZZLE': 'ENG', 'READ': 'ENG', 'EXPLORE': 'PLAY', 'QTE': 'MINI'}


def gameplay_kind(rest):
    m = re.match(r'([A-Za-z]+)\b', rest)
    if m and m.group(1).upper() in _HEAD_KIND and (rest[m.end():m.end() + 1] in (':', ' ', ',', '')):
        return _HEAD_KIND[m.group(1).upper()]
    low = rest[:200].lower()
    for kind, rx in GAMEPLAY_KEYWORDS:
        if rx.search(low):
            return kind
    return 'ENG'


# --------------------------------------------------------------------------------------------------
# build
# --------------------------------------------------------------------------------------------------
class Model:
    pass


def load_inputs(build_dir):
    m = Model()
    script = os.path.join(build_dir, SCRIPT_DIR)
    m.scenes = sp.parse_all(script)
    m.vo = csv_rows(os.path.join(script, 'VO_Master_List.csv'))
    m.meta = {r['scene_id']: r for r in csv_glob(os.path.join(build_dir, 'meta', 'meta_*.csv'))}
    m.inserts = csv_glob(os.path.join(build_dir, 'meta', 'inserts_*.csv'))
    m.flags = csv_rows(os.path.join(build_dir, 'flags.csv'))
    m.cues = csv_rows(os.path.join(build_dir, 'cues_final.csv'))
    return m


def flag_default(s):
    s = s.strip()
    if s == 'false':
        return False
    if s == 'true':
        return True
    if re.fullmatch(r'-?\d+', s):
        return int(s)
    return s


def build(build_dir):
    """-> (files {relpath: text}, stats dict, problems [str])"""
    m = load_inputs(build_dir)
    problems = []
    stats = collections.Counter()
    flag_ids = {f['flag_id'] for f in m.flags}

    scene_ids = [s['id'] for s in m.scenes]
    scene_set = set(scene_ids)
    vo_by_scene = collections.defaultdict(list)
    vo_other = []
    for r in m.vo:
        if r['Scene ID'] in scene_set:
            vo_by_scene[r['Scene ID']].append(r)
        else:
            vo_other.append(r)
    ins_by_scene = collections.defaultdict(list)
    for n, r in enumerate(m.inserts):
        if r['scene_id'] not in scene_set:
            problems.append('insert row %d names unknown scene %s' % (n + 2, r['scene_id']))
        ins_by_scene[r['scene_id']].append(r)
    placed = collections.Counter()   # id(insert row) -> times placed
    for s in m.scenes:
        if s['id'] not in m.meta:
            problems.append('no slate row for ' + s['id'])

    loc = {}
    scene_out = {}
    chapters = collections.OrderedDict()
    unmatched_lines = []
    matched_vo = set()
    insert_kinds = collections.Counter()
    placed_kinds = collections.Counter()

    def add_loc(key, text):
        if key in loc and loc[key] != text:
            problems.append('duplicate loc key with different text: ' + key)
        loc[key] = text
        return key

    for s in m.scenes:
        sid = s['id']
        meta = m.meta.get(sid, {})
        body = s['body']
        beats = []
        cnt = collections.Counter()
        short = sid[len(SCENE_ID_PREFIX):] if sid.startswith(SCENE_ID_PREFIX) else sid

        def key_for(kind):
            cnt[kind] += 1
            return 'story.%s.%s%03d' % (sid, kind, cnt[kind])

        # -- anchors
        ins = []   # dicts: row, kind, at (body line index), absorbed
        for r in ins_by_scene.get(sid, []):
            nt = r['notation'].strip()
            km = re.match(r'^\[([A-Z]+)', nt)
            kind = km.group(1) if km else '?'
            pre = r['before_line_prefix']
            at = None
            if pre == '@END':
                at = len(body)
            else:
                fp = fold(pre)
                for i, ln in enumerate(body):
                    if fold(ln).startswith(fp):
                        at = i
                        break
                if at is None:   # tolerate markdown-emphasis differences
                    np_ = norm(pre)
                    for i, ln in enumerate(body):
                        if norm(ln).startswith(np_):
                            at = i
                            break
            ins.append({'row': r, 'kind': kind, 'at': at, 'note': nt, 'absorbed': False})
            insert_kinds[kind] += 1
            if at is None:
                problems.append('%s: insert anchor not found: %r' % (sid, pre))
        at_map = collections.defaultdict(list)
        for it in ins:
            if it['at'] is not None:
                at_map[it['at']].append(it)
        # in-text note lines absorb the first insert of the same keyword anchored on them
        note_abs = {}
        for i, ln in enumerate(body):
            if sp.classify(ln) == 'note':
                kw = sp.INTEXT_NOTE.match(ln).group(1)
                for it in at_map.get(i, []):
                    if it['kind'] == kw and not it['absorbed']:
                        it['absorbed'] = True
                        note_abs[i] = it
                        break

        # -- VO pool: in file order per (speaker, normalized text)
        pool = collections.defaultdict(list)
        for r in vo_by_scene.get(sid, []):
            pool[(r['Speaker'], norm(r['Line (spoken)']))].append(r)

        def take_vo(who, text):
            for k in ((who, norm_nodir(text)), (who, norm(text))):
                lst = [r for r in pool.get(k, []) if r['VO ID'] not in matched_vo]
                if lst:
                    matched_vo.add(lst[0]['VO ID'])
                    return lst[0]['VO ID']
            return None

        has_hold_insert = any(it['kind'] == 'HOLD' for it in ins)
        ladder = HOLD_LADDER.get(sid)
        ladder_done = [False]
        tell_target = None
        if meta.get('tell_insert', '').strip().lower().startswith('y') and not has_hold_insert:
            tell_target = find_tell(sid, body)
            if tell_target is None:
                problems.append('%s: slate says tell_insert=yes but no Tell line was found' % sid)
        line_no = [0]

        def maybe_tell(i):
            if i == tell_target:
                hold_beat(ladder if ladder else TELL_SECONDS, 'Tell')
                ladder_done[0] = True

        def emit(b):
            beats.append(b)
            return len(beats) - 1

        def hold_beat(seconds, note=None):
            b = {'type': 'hold', 'seconds': seconds}
            if note:
                b['note'] = note
            emit(b)

        ph_no = [0]
        labels = {}
        patches = []   # (option dict, label name)
        mini_done = [False]

        def emit_choice(label, scripted, opts, kind):
            ckey = key_for(kind)
            idx = len(beats)
            options = []
            for k, o in enumerate(opts, 1):
                ob = {'text': o['text'], 'loc': add_loc('%s.o%d' % (ckey, k), o['text']),
                      'effects': [{'op': e[0], 'id': e[1], 'value': e[2]} for e in o['effects']],
                      'next': idx if o['next'] == 'self' else None}
                if isinstance(o['next'], int):
                    ob['next'] = o['next']
                elif o['next'] not in (None, 'self'):
                    patches.append((ob, o['next']))
                if not o['selectable']:
                    ob['selectable'] = False
                if o.get('miss'):
                    ob['miss'] = True
                options.append(ob)
            add_loc(ckey, label)
            emit({'type': 'choice', 'label': label, 'loc': ckey, 'scripted': scripted, 'options': options})
            stats['choice_options'] += len(options)

        def emit_block(items):
            """A MINI exchange (see MINI): invented lines get PH- ids, choices / directions pc / pd keys."""
            for item in items:
                t = item[0]
                if t == 'line':
                    _t, who, text, how = item
                    ph_no[0] += 1
                    lid = 'PH-%s-%03d' % (short, ph_no[0])
                    b = {'type': 'line', 'id': lid, 'speaker': who}
                    if how:
                        b['direction'] = how
                    b['text'] = text
                    b['loc'] = add_loc('story.%s.%s' % (sid, lid), text)
                    emit(b)
                    stats['mini_lines'] += 1
                elif t == 'dir':
                    emit({'type': 'direction', 'text': item[1], 'loc': add_loc(key_for('pd'), item[1])})
                elif t == 'hold':
                    hold_beat(item[1], item[2])
                elif t == 'flag':
                    _t, op, fid, value = item
                    if fid not in flag_ids:
                        problems.append('%s: MINI flag beat names unknown flag %s' % (sid, fid))
                    b = {'type': 'flag', 'op': op, 'id': fid}
                    if value is not None:
                        b['value'] = value
                    emit(b)
                elif t == 'label':
                    if item[1] in labels:
                        problems.append('%s: MINI label used twice: %s' % (sid, item[1]))
                    labels[item[1]] = len(beats)
                elif t == 'choice':
                    _t, label, scripted, opts = item
                    for o in opts:
                        for e in o['effects']:
                            if e[1] not in flag_ids:
                                problems.append('%s: MINI option sets unknown flag %s' % (sid, e[1]))
                    if len([o for o in opts if o['selectable'] and not o.get('miss')]) == 0:
                        problems.append('%s: MINI choice %r has no right pick' % (sid, label))
                    emit_choice(label, scripted, opts, 'pc')
                else:
                    problems.append('%s: unknown MINI item %r' % (sid, t))

        # MINI parts placed before a body line of their own (anchor substring)
        mini_at = collections.defaultdict(list)
        for anchor, items in MINI.get(sid, []):
            if anchor is None:
                continue
            hits = [i for i, ln in enumerate(body) if fold(anchor) in fold(ln)]
            if len(hits) != 1:
                problems.append('%s: MINI anchor %r matches %d body lines (need 1)' % (sid, anchor, len(hits)))
            else:
                mini_at[hits[0]].append(items)

        def emit_insert(it):
            nt = it['note']
            kind = it['kind']
            placed[id(it['row'])] += 1
            placed_kinds[kind] += 1
            if kind == 'GAMEPLAY':
                mm = re.match(r'^\[GAMEPLAY\s*→\s*(.*)\]$', nt, re.S)
                rest = mm.group(1) if mm else nt
                params = {'mode': s['mode']}
                tm = re.search(r'about (\d+)(?:\s*-\s*\d+)? minutes?', rest)
                if tm:
                    params['minutes'] = int(tm.group(1))
                if '[OPTIONAL' in rest:
                    params['optional'] = True
                k = gameplay_kind(rest)
                if k == 'MINI':
                    if sid not in MINI:
                        problems.append('%s: MINI gameplay insert has no entry in MINI' % sid)
                    elif mini_done[0]:
                        problems.append('%s: more than one MINI gameplay insert' % sid)
                    else:   # the notation stays as a hidden note; the exchange replaces the hand-off
                        mini_done[0] = True
                        stats['mini_exchanges'] += 1
                        emit({'type': 'direction', 'text': nt, 'note': 'MINI', 'hidden': True})
                        for anchor, items in MINI[sid]:
                            if anchor is None:
                                emit_block(items)
                        return
                stats['gameplay_' + k] += 1
                key = add_loc(key_for('g'), nt)
                emit({'type': 'gameplay', 'kind': k, 'text': nt, 'loc': key, 'params': params})
            elif kind == 'CHOICE':
                spec = CHOICES.get(sid)
                if spec is None:
                    # the run fails; the placeholder keeps the scene's shape and copies no note text
                    problems.append('%s: CHOICE insert has no entry in the tables (choices)' % sid)
                    spec = [(MISSING_CHOICE, True, [{'text': MISSING_CHOICE, 'effects': [], 'next': None,
                                                     'selectable': True, 'miss': False}])]
                for label, scripted, opts in spec:
                    emit_choice(label, scripted, opts, 'c')
            elif kind == 'HOLD':
                seconds = HOLD_LADDER.get(sid, INPUT_HOLD_SECONDS)
                hold_beat(seconds, nt)
                ladder_done[0] = True
                # "Sets <flag id>" in the HOLD note: the first id the flag table knows counts
                sm = next((g for g in re.findall(r'(?<!No flag )\b[Ss]ets `?([A-Za-z][A-Za-z0-9_]*)', nt)
                           if g in flag_ids), None)
                if sm and 'No flag set' not in nt:
                    emit({'type': 'flag', 'op': 'set', 'id': sm, 'value': True})
            else:   # OPTIONAL / TIMED / VARIANT: designer notation, never shown
                b = {'type': 'direction', 'text': nt, 'note': kind, 'hidden': True}
                emit(b)

        for i, ln in enumerate(body):
            for it in at_map.get(i, []):
                if not it['absorbed']:
                    emit_insert(it)
            for items in mini_at.get(i, []):
                emit_block(items)
            if i in note_abs:
                it = note_abs[i]
                if it['kind'] == 'CHOICE':
                    emit_insert(it)
                else:
                    placed[id(it['row'])] += 1
                    placed_kinds[it['kind']] += 1
                    emit({'type': 'direction', 'text': it['note'], 'note': it['kind'], 'hidden': True,
                          'source': ln.strip()})
                continue
            kind = sp.classify(ln)
            if kind == 'blank':
                continue
            if kind == 'dialogue':
                sm = sp.SPEAKER.match(ln)
                line_no[0] += 1
                who, how, text = sm.group('who'), sm.group('how'), sm.group('text')
                vid = take_vo(who, text)
                lid = vid or 'TX-%s-%03d' % (short, line_no[0])
                if not vid:
                    unmatched_lines.append((sid, who, text))
                lkey = add_loc('story.%s.%s' % (sid, lid), text)
                b = {'type': 'line', 'id': lid, 'speaker': who}
                if how:
                    b['direction'] = how
                b['text'] = text
                b['loc'] = lkey
                emit(b)
                maybe_tell(i)
                continue
            if kind == 'note':   # in-text note that no insert covers
                kw = sp.INTEXT_NOTE.match(ln).group(1)
                if kw == 'HOLD':
                    hold_beat(HOLD_LADDER.get(sid, INPUT_HOLD_SECONDS), ln.strip())
                else:
                    emit({'type': 'direction', 'text': ln.strip(), 'note': kw, 'hidden': True})
                    if kw == 'CHOICE':
                        problems.append('%s: in-text CHOICE note without a CHOICE insert' % sid)
                continue
            # direction (italic) or other (bold card)
            txt = direction_text(ln)
            if sid in MINI and txt.startswith('Gameplay:'):
                # describes the retired mini-game mechanic; the exchange replaces it, keep it as a note
                emit({'type': 'direction', 'text': txt, 'note': 'MINI', 'hidden': True})
            else:
                emit({'type': 'direction', 'text': txt, 'loc': add_loc(key_for('d'), txt)})
            maybe_tell(i)
        for it in at_map.get(len(body), []):
            emit_insert(it)
        emit({'type': 'end'})
        for ob, name in patches:
            if name not in labels:
                problems.append('%s: MINI option jumps to unknown label %s' % (sid, name))
            else:
                ob['next'] = labels[name]
        if sid in MINI and not mini_done[0]:
            problems.append('%s: MINI entry but no MINI gameplay insert to expand' % sid)
        if ladder and not ladder_done[0]:
            problems.append('%s: hold-ladder entry %.1f s has no hold beat' % (sid, ladder))

        stats['scenes'] += 1
        for b in beats:
            stats['beat_' + b['type']] += 1

        # scene object
        cast = []
        for c in split_semis(meta.get('cast_on_stage', '')):
            c = re.sub(r'\s*\([^)]*\)', '', c).strip()
            if c and c not in cast:
                cast.append(c)
        scene = collections.OrderedDict([
            ('id', sid),
            ('title', s['title_plain']),
            ('chapter', chapter_id(s['chapter_id'])),
            ('world', meta.get('world', '')),
            ('location', meta.get('location', '')),
            ('time', meta.get('time_of_day', '')),
            ('mode', s['mode']),
            ('gameplay', s['gameplay']),
            ('cast', cast),
            ('props', split_semis(meta.get('props_in', ''))),
            ('cues', split_semis(meta.get('sound_cues', ''))),
            ('beats', beats),
        ])
        add_loc('story.%s.title' % sid, s['title_plain'])
        scene_out[sid] = scene
        ch = chapters.setdefault(chapter_id(s['chapter_id']),
                                 {'id': chapter_id(s['chapter_id']), 'title': chapter_title(s), 'scenes': []})
        ch['scenes'].append(sid)

    for it_row in m.inserts:
        if placed[id(it_row)] != 1:
            problems.append('insert placed %d times: %s %r' % (placed[id(it_row)], it_row['scene_id'],
                                                                it_row['before_line_prefix'][:50]))
    unmatched_vo = [r for sid in scene_ids for r in vo_by_scene.get(sid, []) if r['VO ID'] not in matched_vo]

    story = collections.OrderedDict([
        ('format', FORMAT),
        ('title', TITLE),
        ('start', scene_ids[0]),
        ('chapters', list(chapters.values())),
        ('flags', [{'id': f['flag_id'], 'default': flag_default(f['default'])}
                   for f in sorted(m.flags, key=lambda f: f['flag_id'])]),
        ('cues', [{'id': c['cue_id'], 'kind': {'MUS': 'music', 'SFX': 'sfx'}.get(c['cue_id'].split('-')[0], 'music')}
                  for c in sorted(m.cues, key=lambda c: c['cue_id'])]),
    ])
    files = {'story.json': dump(story)}
    for sid in scene_ids:
        files['scenes/%s.json' % sid] = dump(scene_out[sid])
    files['loc/en.json'] = dump(collections.OrderedDict(sorted(loc.items())))

    stats['vo_rows_in_script_scenes'] = sum(len(v) for v in vo_by_scene.values())
    stats['vo_matched'] = len(matched_vo)
    stats['vo_unmatched'] = len(unmatched_vo)
    stats['vo_other_rows'] = len(vo_other)
    stats['lines_without_vo'] = len(unmatched_lines)
    stats['inserts_total'] = len(m.inserts)
    stats['inserts_placed'] = sum(1 for r in m.inserts if placed[id(r)] == 1)
    stats['loc_keys'] = len(loc)
    info = {'tell_scenes': {sid for sid in scene_ids
                            if m.meta.get(sid, {}).get('tell_insert', '').strip().lower().startswith('y')},
            'unmatched_vo': unmatched_vo, 'unmatched_lines': unmatched_lines, 'insert_kinds': insert_kinds,
            'vo_other': vo_other, 'flag_ids': flag_ids}
    return files, stats, problems, info


# --------------------------------------------------------------------------------------------------
# validation (the spec's "Rules the importer checks"), run on the files on disk
# --------------------------------------------------------------------------------------------------
def validate(out_dir, fresh_files, vo_ids, info):
    errs = []

    def err(msg):
        errs.append(msg)

    on_disk = {}
    for root, _dirs, names in os.walk(out_dir):
        for n in names:
            p = os.path.join(root, n)
            rel = os.path.relpath(p, out_dir).replace(os.sep, '/')
            if rel in fresh_files or rel.endswith('.json'):
                on_disk[rel] = p
    for rel in sorted(set(fresh_files) | set(on_disk)):
        if rel not in on_disk:
            err('missing file: ' + rel)
            continue
        if rel not in fresh_files:
            err('unexpected file: ' + rel)
            continue
        raw = open(on_disk[rel], 'rb').read()
        try:
            txt = raw.decode('utf-8')
        except UnicodeDecodeError:
            err('not UTF-8: ' + rel)
            continue
        if '\r' in txt:
            err('CR in file (not LF): ' + rel)
        if txt != fresh_files[rel]:
            err('differs from a fresh build (stale or hand-edited): ' + rel)

    def load(rel):
        try:
            with open(os.path.join(out_dir, rel), encoding='utf-8') as fh:
                return json.load(fh)
        except (OSError, ValueError) as e:
            err('cannot read %s: %s' % (rel, e))
            return None

    story = load('story.json')
    en = load('loc/en.json')
    if story is None or en is None:
        return errs
    if story.get('format') != FORMAT:
        err('story.json format != %d' % FORMAT)
    chapters = story.get('chapters', [])
    in_story = [sid for ch in chapters for sid in ch['scenes']]
    if len(in_story) != SCENE_COUNT:
        err('story.json lists %d scenes, expected %d' % (len(in_story), SCENE_COUNT))
    if len(set(in_story)) != len(in_story):
        err('a scene appears in more than one chapter slot')
    if story.get('start') not in in_story:
        err('start scene not in chapters')
    flag_ids = {f['id'] for f in story.get('flags', [])}
    cue_ids = {c['id'] for c in story.get('cues', [])}
    if len(flag_ids) != len(story.get('flags', [])):
        err('duplicate flag ids')
    if len(cue_ids) != len(story.get('cues', [])):
        err('duplicate cue ids')
    scene_files = sorted(r for r in on_disk if r.startswith('scenes/'))
    if len(scene_files) != SCENE_COUNT:
        err('%d scene files, expected %d' % (len(scene_files), SCENE_COUNT))
    ch_of = {sid: ch['id'] for ch in chapters for sid in ch['scenes']}
    used_loc = set()
    line_ids = collections.Counter()
    vo_used = collections.Counter()
    for rel in scene_files:
        sc = load(rel)
        if sc is None:
            continue
        sid = sc.get('id')
        if rel != 'scenes/%s.json' % sid:
            err('%s: file name does not match id %s' % (rel, sid))
        if sid not in ch_of:
            err('%s: scene not in any chapter' % sid)
        elif ch_of[sid] != sc.get('chapter'):
            err('%s: chapter field %r != chapter slot %r' % (sid, sc.get('chapter'), ch_of[sid]))
        if sc.get('world') not in WORLDS:
            err('%s: bad world %r' % (sid, sc.get('world')))
        for k in ('title', 'location', 'time', 'mode'):
            if not sc.get(k):
                err('%s: empty %s' % (sid, k))
        for c in sc.get('cues', []):
            if c not in cue_ids:
                err('%s: unknown scene cue %s' % (sid, c))
        used_loc.add('story.%s.title' % sid)
        if en.get('story.%s.title' % sid) != sc.get('title'):
            err('%s: title loc missing or different' % sid)
        beats = sc.get('beats', [])
        if not beats or beats[-1].get('type') != 'end':
            err('%s: does not finish with an end beat' % sid)
        for n, b in enumerate(beats):
            t = b.get('type')
            where = '%s beat %d' % (sid, n)
            if t not in BEAT_TYPES:
                err('%s: unknown type %r' % (where, t))
                continue
            if t == 'end' and n != len(beats) - 1:
                err('%s: end beat is not last' % where)

            def need_loc(obj, text_field):
                k = obj.get('loc')
                if not k:
                    err('%s: missing loc' % where)
                    return
                used_loc.add(k)
                if k not in en:
                    err('%s: loc key not in en.json: %s' % (where, k))
                elif en[k] != obj.get(text_field):
                    err('%s: loc text differs from %s: %s' % (where, text_field, k))

            if t == 'line':
                for f in ('id', 'speaker', 'text', 'loc'):
                    if not b.get(f):
                        err('%s: line missing %s' % (where, f))
                line_ids[b.get('id')] += 1
                if b.get('id', '').startswith('VO-'):
                    vo_used[b['id']] += 1
                    if b['id'] not in vo_ids:
                        err('%s: id %s is not in the VO list' % (where, b['id']))
                need_loc(b, 'text')
                if b.get('loc') != 'story.%s.%s' % (sid, b.get('id')):
                    err('%s: line loc key is not story.<scene>.<id>' % where)
            elif t == 'direction':
                if not b.get('text'):
                    err('%s: direction without text' % where)
                if b.get('hidden'):
                    if 'loc' in b:
                        err('%s: hidden direction carries a loc key' % where)
                else:
                    need_loc(b, 'text')
            elif t == 'choice':
                opts = b.get('options') or []
                if not opts:
                    err('%s: choice without options' % where)
                if b.get('scripted') and len(opts) != 1:
                    err('%s: scripted choice must have exactly one option' % where)
                need_loc(b, 'label')
                for k, o in enumerate(opts, 1):
                    used_loc.add(o.get('loc'))
                    if en.get(o.get('loc')) != o.get('text'):
                        err('%s option %d: loc missing or different' % (where, k))
                    nx = o.get('next')
                    if nx is not None and not (isinstance(nx, int) and 0 <= nx < len(beats)):
                        err('%s option %d: bad next %r' % (where, k, nx))
                    for e in o.get('effects', []):
                        if e.get('op') not in ('set', 'clear') or e.get('id') not in flag_ids:
                            err('%s option %d: bad effect %r' % (where, k, e))
                    if not isinstance(o.get('miss', False), bool):
                        err('%s option %d: miss is not a boolean' % (where, k))
                if opts and not any(o.get('selectable', True) and not o.get('miss') for o in opts):
                    if any(o.get('miss') for o in opts):
                        err('%s: every option is a miss (nothing moves the story on)' % where)
            elif t == 'gameplay':
                if b.get('kind') not in GAMEPLAY_KINDS:
                    err('%s: bad gameplay kind %r' % (where, b.get('kind')))
                if not isinstance(b.get('params'), dict):
                    err('%s: gameplay params not an object' % where)
                need_loc(b, 'text')
            elif t == 'cue':
                if b.get('id') not in cue_ids or b.get('action') not in ('play', 'stop', 'stinger'):
                    err('%s: bad cue' % where)
            elif t == 'flag':
                if b.get('op') not in ('set', 'clear', 'require', 'end'):
                    err('%s: bad flag beat op' % where)
                elif (b.get('op') != 'end' or 'id' in b) and b.get('id') not in flag_ids:
                    err('%s: flag beat names an unknown flag' % where)
            elif t == 'hold':
                if not isinstance(b.get('seconds'), (int, float)) or b['seconds'] <= 0:
                    err('%s: bad hold seconds' % where)
        holds = [b for b in beats if b.get('type') == 'hold']
        if sid in info['tell_scenes'] and not holds:
            err('%s: slate tell_insert=yes but the scene has no hold beat' % sid)
        if sid not in info['tell_scenes'] and any(b.get('note') == 'Tell' for b in holds):
            err('%s: Tell hold in a scene whose slate says tell_insert=no' % sid)
        if sid in HOLD_LADDER and not any(b.get('type') == 'hold' and b.get('seconds') == HOLD_LADDER[sid]
                                           for b in beats):
            err('%s: ladder hold of %.1f s missing' % (sid, HOLD_LADDER[sid]))
    for k, v in line_ids.items():
        if v > 1:
            err('line id used %d times: %s' % (v, k))
    for k, v in vo_used.items():
        if v != 1:
            err('VO id matched %d times: %s' % (v, k))
    for r in info['unmatched_vo']:
        err('VO row not matched to a script line: %s (%s %s: %s)' % (r['VO ID'], r['Scene ID'], r['Speaker'],
                                                                     r['Line (spoken)'][:60]))
    orphans = sorted(set(en) - used_loc)
    for k in orphans[:20]:
        err('en.json key referenced by no beat: ' + k)
    if len(orphans) > 20:
        err('... and %d more unreferenced loc keys' % (len(orphans) - 20))
    return errs


# --------------------------------------------------------------------------------------------------
def write_files(out_dir, files):
    for rel, text in files.items():
        p = os.path.join(out_dir, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(text)
    # drop scene files that are no longer produced
    sd = os.path.join(out_dir, 'scenes')
    if os.path.isdir(sd):
        for n in os.listdir(sd):
            if n.endswith('.json') and 'scenes/' + n not in files:
                os.remove(os.path.join(sd, n))


def summary(stats, info, out):
    p = lambda *a: print(*a, file=out)
    p('scenes: %d' % stats['scenes'])
    p('beats: ' + ', '.join('%s=%d' % (t, stats['beat_' + t]) for t in BEAT_TYPES))
    p('  gameplay by kind: ' + ', '.join('%s=%d' % (k, stats['gameplay_' + k]) for k in GAMEPLAY_KINDS))
    p('  choice options: %d' % stats['choice_options'])
    p('VO rows in script scenes: %d, matched: %d, unmatched: %d (other VO rows, barks/camp/rest banter: %d)' % (
        stats['vo_rows_in_script_scenes'], stats['vo_matched'], stats['vo_unmatched'], stats['vo_other_rows']))
    p('script lines with no VO row (given TX- ids): %d' % stats['lines_without_vo'])
    for sid, who, text in info['unmatched_lines']:
        p('  %s %s: %s' % (sid, who, text[:80]))
    p('inserts placed: %d of %d (%s)' % (stats['inserts_placed'], stats['inserts_total'],
                                          ', '.join('%s=%d' % kv for kv in sorted(info['insert_kinds'].items()))))
    p('loc keys: %d' % stats['loc_keys'])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--tables', required=True, help="the story's tables (JSON, kept outside the repo)")
    ap.add_argument('--build-dir', required=True, help="the story's design package build folder")
    ap.add_argument('--out-dir', default=os.path.join(REPO, 'assets', 'story'))
    ap.add_argument('--check', action='store_true', help='validate the files in --out-dir instead of writing them')
    ap.add_argument('--allow-problems', action='store_true',
                    help='write the files even when the import found problems (still exits 1); by default an import '
                         'with problems writes nothing, so it cannot overwrite good output')
    a = ap.parse_args(argv)
    why = load_tables(a.tables)
    if why:
        print('story_import: %s' % why, file=sys.stderr)
        return 2
    files, stats, problems, info = build(a.build_dir)
    if a.check:
        vo_ids = {r['VO ID'] for r in csv_rows(os.path.join(a.build_dir, SCRIPT_DIR, 'VO_Master_List.csv'))}
        errs = problems + validate(a.out_dir, files, vo_ids, info)
        summary(stats, info, sys.stdout)
        if errs:
            print('CHECK FAILED: %d problem(s)' % len(errs))
            for e in errs[:60]:
                print('  - ' + e)
            return 1
        print('CHECK OK')
        return 0
    summary(stats, info, sys.stdout)
    for e in problems:
        print('PROBLEM: ' + e, file=sys.stderr)
    if problems and not a.allow_problems:
        print('wrote nothing: %d problem(s); %s is unchanged (--allow-problems writes anyway)' % (
            len(problems), a.out_dir), file=sys.stderr)
        return 1
    write_files(a.out_dir, files)
    print('wrote %d files to %s' % (len(files), a.out_dir))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
