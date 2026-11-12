#!/usr/bin/env python3
"""Tests for the VO direction tools on a tiny synthetic draft.  python3 -I test_vo_direction.py

The samples use made-up names and scene ids. A private fixture (JSON: scene_prefix plus ordered
[neutral, private] pairs) can swap in other names: set VO_TEST_FIXTURE to its path. Without it the
made-up samples run and a note says the private run was skipped."""
import io
import json
import re
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vo_common as vc  # noqa: E402
import extract_lines  # noqa: E402
import carry_forward  # noqa: E402
import validate_directions  # noqa: E402


def _load_fixture():
    path = os.environ.get('VO_TEST_FIXTURE', '')
    if not path or not os.path.isfile(path):
        sys.stderr.write('test_vo_direction: no private fixture (VO_TEST_FIXTURE%s); private run skipped, '
                         'made-up samples only\n' % (' not found: ' + path if path else ' unset'))
        return 'QZ-', []
    try:
        with open(path, encoding='utf-8-sig') as fh:
            fx = json.load(fh)
        pairs = [(str(a), str(b)) for a, b in fx['pairs']]
        prefix = str(fx['scene_prefix'])
    except (OSError, ValueError, KeyError, TypeError) as e:
        sys.stderr.write('test_vo_direction: private fixture unusable (%s: %s); private run skipped, '
                         'made-up samples only\n' % (path, e))
        return 'QZ-', []
    sys.stderr.write('test_vo_direction: private fixture in use\n')
    return prefix, pairs


SCENE_PREFIX, PAIRS = _load_fixture()


def T(s):
    """The sample text with the fixture's names swapped in (identity without a fixture)."""
    for a, b in PAIRS:
        s = re.sub(r'(?<![A-Za-z0-9])' + re.escape(a) + r'(?![A-Za-z0-9])', lambda m, b=b: b, s)
    return s

SAMPLE_A = T('''# TEST SCRIPT, PART 1
## PROLOGUE
### QZ-P0-01 · The Kitchen

CAZ (on a crate, tapping a spoon): Where did the ladder go?

ARLO: …

CAZ: I painted three rungs. *(beat)* Where did it go?

# ACT I — TEST

### QZ-02-D1 · The Wall

***ACTING.** Patrol barks are Arlo alone, clipped: "Mine." / "Have it." / (breath only). The yard stays quiet.*

### QZ-02-U1 · The Map

DOV: Tin Stairs.

BEX: *[pick]* "East Dock." / "Reed Lane."

DOV *(if "Reed Lane")*: Reed Lane's the Nine. *(beat)* I swept the Six.

DOV *(if "East Dock")*: East Dock. *(He nods.)*

### QZ-02-D2 · The Audit

> **Audit** (Arlo's options):
> 1. "Left, then left."
> 2. "Right. Then home."

ARLO (low): The kettle sings at noon and the hall fills up.

## Camps

## CAMP-E1 · "Test Camp"

*Choice:* **[CHOICE: TEASE / CHEER]** sets `F_MOOD`.

DOV: Tram Five went up the hill.
<sub>VO-CAMP-E1-001</sub>

**[VARIANT: if TEASE]**

BEX: You would have dropped it.
<sub>VO-CAMP-E1-002</sub>

**[VARIANT: if CHEER]**

BEX: Wake me before the market.
<sub>VO-CAMP-E1-003</sub>
''')

SAMPLE_B = T('''# TEST SCRIPT, PART 2
# ACT II — TEST
### QZ-05-D1 · The Yard

> **[VARIANT: F_MOOD = TEASE]** ARLO (bark): You count like a clock. Keep ticking.
> **[VARIANT: F_MOOD = CHEER]** ARLO (bark): Good count. Once more.

- ARLO (TEASE): You dodge like a door on a hinge.
- BEX (CHEER): Steady. You are fine.

ARLO and BEX (together): What if the boat leaves?
''')


SAMPLE_C = T('''# TEST SCRIPT, PART 3
# ACT IV — TEST

### QZ-11-HE1 · The Square

EMRYS: Hi.
FAYE: Hello.
ARLO: Go on.
THE EASEL: Begin.

*[direction.hidden · VARIANT]* Concede (third miss): EMRYS (very small): Ask him about the blue boat. Then play "Ask about the boat."

*[direction.hidden · VARIANT]* Concede (second miss): FAYE (hoarse): Mill Lane. Then play "Mill Lane?". If F_ROUNDS = COMPLETE, Lark's first pick plays "Mill Lane?".

*[direction.hidden · VARIANT]* Reads F_TALLY. If ≥ 2: ARLO (low): Two lit. BEX: The stool. If < 2: ARLO (low): One dark.

### QZ-11-Q3 · The Kitchen

*[direction.hidden · OPTIONAL]* The last round (each plays once, on interaction):
- **The tap.** Arlo rinses the cup. No line.
- **The window.** Rain on the glass. ARLO (to himself): That's almost a proper storm.
- **The bread.** Bread on the shelf. BEX (from the stairs, not looking): Stale. ARLO: It's a loaf.

### QZ-12-Q3 · The Easel

*She props the easel up. And the easel says, dry and slow:* "Your paint has dried."

*Emrys asks Gus about the shop. Gus begins to answer, "A Tinker's week is mostly—" and stops.*

*On the back, in Arlo's hand:* "See Dov."

*"Not now," he says, and leaves.*

### QZ-13-Q2 · The Roll

*Another: "Hugo." Another: "Ivo." "Kip."*

HUGO: Here.
IVO: Here.

### QZ-14-Q2 · The Supper

***CHOICE (flavor only).** Two lines appear: "I missed the tram." / "I'm off shift."*

*If "I missed the tram.":*
BEX: I missed the tram.
JUNO: Then sit.

*If "I'm off shift.":*
BEX: I'm off shift.
CAZ: See.

JUNO: It's a chair.

*If "No.":*

BEX: No.

GUS: Then leave.

*(Returns to the choice.)*

*If "Not sure" (the one answer that continues):*

BEX: Not sure.

GUS: Neither do I.

### QZ-15-Q1 · The Stall

***VARIANT (pair barks).** Reads `F_MOOD`. TEASE: BEX: "Wet floor." / ARLO: "My fault." CHEER: BEX: "Next round." / ARLO: "I'm ready."*

### QZ-16-Q1 · The Table

***CHOICE.** Concede (third miss of the loop): Bex supplies it.*

> BEX (quietly): It was the dog.
''')


def extract_sample_c():
    d = tempfile.mkdtemp()
    with open(os.path.join(d, '04c_Test.md'), 'w', encoding='utf-8') as fh:
        fh.write(SAMPLE_C)
    return extract_lines.extract(d, scene_prefix=SCENE_PREFIX)


def extract_sample(files=None):
    d = tempfile.mkdtemp()
    for name, body in (('04a_Test.md', SAMPLE_A), ('04b_Test.md', SAMPLE_B)):
        if files is None or name in files:
            with open(os.path.join(d, name), 'w', encoding='utf-8') as fh:
                fh.write(body)
    return extract_lines.extract(d, scene_prefix=SCENE_PREFIX)


def find(rows, speaker, words):
    hits = [r for r in rows if r['speaker'] == speaker and vc.words_key(r['spoken_text']) == words]
    if len(hits) != 1:
        raise AssertionError(f'{speaker} "{words}": {len(hits)} rows')
    return hits[0]


class TestCommon(unittest.TestCase):
    def test_hash_ignores_typography_tags_and_pauses(self):
        h = vc.text_hash('I swept the Six.')
        self.assertEqual(h, vc.text_hash('[sighs] I swept... the SIX.'))
        self.assertEqual(h, vc.text_hash('I swept / the Six. //'))
        self.assertEqual(h, vc.text_hash('I swept the *Six.* *(beat)*'))
        self.assertNotEqual(h, vc.text_hash('I swept the Seven.'))

    def test_curly_apostrophe(self):
        self.assertEqual(vc.text_hash(T('Reed Lane’s the Nine.')), vc.text_hash(T("Reed Lane's the Nine.")))


class TestExtract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rows = extract_sample()

    def test_standard_line_and_delivery(self):
        r = find(self.rows, T('CAZ'), 'i painted three rungs where did it go')
        self.assertEqual(r['scene'], T('QZ-P0-01'))
        self.assertEqual(r['act'], 'Prologue')
        self.assertIn('beat', r['delivery_original'])
        r = find(self.rows, T('CAZ'), 'where did the ladder go')
        self.assertIn('tapping a spoon', r['delivery_original'])

    def test_silence_line(self):
        r = [x for x in self.rows if x['speaker'] == T('ARLO') and x['scene'] == T('QZ-P0-01')][0]
        self.assertEqual(r['kind'], 'silence')

    def test_acting_note_patrol_barks(self):
        rows = [r for r in self.rows if r['scene'] == T('QZ-02-D1')]
        self.assertEqual([r['speaker'] for r in rows], [T('ARLO')] * 3)
        self.assertEqual([r['kind'] for r in rows][-1], 'nonverbal')
        self.assertEqual(rows[0]['act'], 'I')

    def test_pick_options_and_conditional_lines(self):
        opts = [r for r in self.rows if r['scene'] == T('QZ-02-U1') and r['kind'] == 'choice_option']
        self.assertEqual(sorted(vc.words_key(r['spoken_text']) for r in opts), sorted([T('reed lane'), T('east dock')]))
        self.assertTrue(all(r['speaker'] == T('BEX') for r in opts))
        r = find(self.rows, T('DOV'), T("reed lane's the nine i swept the six"))
        self.assertIn(T('Reed Lane'), r['variant'])

    def test_audit_options(self):
        opts = [r for r in self.rows if r['scene'] == T('QZ-02-D2') and r['kind'] == 'choice_option']
        self.assertEqual(len(opts), 2)
        self.assertTrue(all(r['speaker'] == T('ARLO') for r in opts))
        r = find(self.rows, T('ARLO'), 'the kettle sings at noon and the hall fills up')
        self.assertEqual(r['variant'], '')

    def test_camp_vo_ids_and_tone_variants(self):
        r = find(self.rows, T('BEX'), 'you would have dropped it')
        self.assertEqual(r['line_id'], 'VO-CAMP-E1-002')
        self.assertIn('TEASE', r['variant'])
        r = find(self.rows, T('BEX'), 'wake me before the market')
        self.assertIn('CHEER', r['variant'])
        self.assertEqual(find(self.rows, T('DOV'), 'tram five went up the hill')['variant'], '')

    def test_barks_each_variant_its_own_row(self):
        barks = [r for r in self.rows if r['scene'] == T('QZ-05-D1') and r['kind'] == 'bark']
        self.assertEqual(len(barks), 4)
        self.assertEqual(sum('TEASE' in r['variant'] for r in barks), 2)
        self.assertEqual(sum('CHEER' in r['variant'] for r in barks), 2)
        self.assertEqual(barks[0]['act'], 'II')

    def test_joint_speaker(self):
        find(self.rows, T('ARLO and BEX'), 'what if the boat leaves')

    def test_line_ids_unique(self):
        ids = [r['line_id'] for r in self.rows]
        self.assertEqual(len(ids), len(set(ids)))


class TestLanes(unittest.TestCase):
    BODY = '''### %s03-A1 · The Gate

> 1. "Left."
> 2. "Right."
'''

    GRAMMAR_BODY = '''## Camps

## ZZ-E1 · "Camp"

DOV: Tram Five went up the hill.
<sub>VO-ZZ-E1-001</sub>

### %s05-D1 · The Yard

- ARLO (JEER): You dodge like a door on a hinge.
- BEX (PRAISE): Steady. You are fine.
'''

    def tearDown(self):
        extract_lines.LANE_DEFAULT = dict(extract_lines.LANE_GENERIC)
        extract_lines.set_grammar()

    def rows(self):
        d = tempfile.mkdtemp()
        with open(os.path.join(d, '04a_Test.md'), 'w', encoding='utf-8') as fh:
            fh.write(self.BODY % SCENE_PREFIX)
        return [r for r in extract_lines.extract(d, scene_prefix=SCENE_PREFIX) if r['kind'] == 'choice_option']

    def names(self, text):
        p = os.path.join(tempfile.mkdtemp(), 'names.json')
        with open(p, 'w', encoding='utf-8-sig') as fh:
            fh.write(text)
        return p

    def test_generic_lane_by_default(self):
        self.assertEqual([r['speaker'] for r in self.rows()], ['A', 'A'])

    def test_lanes_from_names(self):
        self.assertIsNone(extract_lines.load_names(self.names('{"lanes": {"A": "ARLO"}}')))
        self.assertEqual([r['speaker'] for r in self.rows()], ['ARLO', 'ARLO'])

    def grammar_rows(self):
        d = tempfile.mkdtemp()
        with open(os.path.join(d, '04a_Test.md'), 'w', encoding='utf-8') as fh:
            fh.write(self.GRAMMAR_BODY % SCENE_PREFIX)
        return extract_lines.extract(d, scene_prefix=SCENE_PREFIX)

    def test_grammar_from_names(self):
        # generic grammar: ZZ- is no camp heading and JEER / PRAISE are no bark labels
        rows = self.grammar_rows()
        self.assertNotIn('VO-ZZ-E1-001', [r['line_id'] for r in rows])
        self.assertEqual([r['variant'] for r in rows if r['speaker'] in ('ARLO', 'BEX')][-2:], ['', ''])
        self.assertIsNone(extract_lines.load_names(self.names(
            '{"grammar": {"barks": ["JEER", "PRAISE"], "camp_prefix": "ZZ-"}}')))
        rows = self.grammar_rows()
        self.assertIn('VO-ZZ-E1-001', [r['line_id'] for r in rows])
        barks = [r for r in rows if r['kind'] == 'bark']
        self.assertEqual([(r['speaker'], r['variant']) for r in barks], [('ARLO', 'JEER'), ('BEX', 'PRAISE')])

    def test_bad_grammar(self):
        for text, needle in (('{"grammar": []}', 'grammar'), ('{"grammar": {"barks": []}}', 'barks'),
                             ('{"grammar": {"barks": ["low"]}}', 'barks'),
                             ('{"grammar": {"barks": ["TEASE\\n"]}}', 'barks'),
                             ('{"grammar": {"camp_prefix": "("}}', 'camp_prefix')):
            self.assertIn(needle, extract_lines.load_names(self.names(text)), text)
        # a refused file changes nothing
        self.assertEqual(extract_lines.BARKS, extract_lines.BARKS_GENERIC)
        self.assertIn(extract_lines.re.escape('CAMP-'), extract_lines.CAMP_HDR.pattern)
        self.assertEqual(extract_lines.LANE_DEFAULT, extract_lines.LANE_GENERIC)

    def test_bad_lanes_exit_2(self):
        self.assertIn('lanes', extract_lines.load_names(self.names('{"lanes": ["ARLO"]}')))
        self.assertIn('cannot read', extract_lines.load_names(self.names('{')))
        d = tempfile.mkdtemp()
        self.assertEqual(extract_lines.main([d, '--scene-prefix', SCENE_PREFIX, '--names', self.names('[1]')]), 2)


class TestExtractEmbedded(unittest.TestCase):
    """Bug (a): lines embedded in notes/stage directions; bug (b): blank variants."""
    @classmethod
    def setUpClass(cls):
        cls.rows = extract_sample_c()

    def scene(self, sc):
        return [r for r in self.rows if r['scene'] == sc]

    def test_hidden_variant_concede_lines(self):
        r = find(self.rows, T('EMRYS'), 'ask him about the blue boat')
        self.assertEqual((r['kind'], r['variant']), ('embedded', 'concede (third miss)'))
        self.assertEqual(r['delivery_original'], 'very small')
        r = find(self.rows, T('FAYE'), T('mill lane'))
        self.assertEqual(r['variant'], 'concede (second miss)')
        # "Then play ..." and the trailing If-clause are not part of the line
        self.assertFalse(any('ask about the boat' == vc.words_key(x['spoken_text']) for x in self.rows))

    def test_hidden_variant_reads_flag_conditions(self):
        self.assertEqual(find(self.rows, T('ARLO'), 'two lit')['variant'], T('if F_TALLY ≥ 2'))
        self.assertEqual(find(self.rows, T('BEX'), 'the stool')['variant'], T('if F_TALLY ≥ 2'))
        self.assertEqual(find(self.rows, T('ARLO'), 'one dark')['variant'], T('if F_TALLY < 2'))

    def test_optional_bullets_several_speakers(self):
        rows = self.scene(T('QZ-11-Q3'))
        self.assertEqual([(r['speaker'], vc.words_key(r['spoken_text'])) for r in rows],
                         [(T('ARLO'), "that's almost a proper storm"), (T('BEX'), 'stale'),
                          (T('ARLO'), T("it's a loaf"))])
        self.assertEqual(rows[1]['variant'], T('optional: the bread'))
        self.assertEqual(rows[1]['delivery_original'], 'from the stairs, not looking')

    def test_stage_direction_quotes(self):
        rows = self.scene(T('QZ-12-Q3'))
        self.assertEqual(find(rows, T('THE EASEL'), T('your paint has dried'))['kind'], 'embedded')
        find(rows, T('GUS'), T("a tinker's week is mostly"))
        r = find(rows, 'UNKNOWN', 'not now')
        self.assertIn('CHECK speaker', r['parse_note'])
        # written text (a margin note) is not a VO line
        self.assertEqual(len(rows), 3)

    def test_roll_call(self):
        rows = [r for r in self.scene(T('QZ-13-Q2')) if r['kind'] == 'embedded']
        self.assertEqual([r['speaker'] for r in rows], [T('HUGO'), T('IVO'), 'UNKNOWN'])

    def test_if_block_scoping(self):
        v = {vc.words_key(r['spoken_text']) + '/' + r['speaker']: r['variant']
             for r in self.scene(T('QZ-14-Q2')) if r['kind'] == 'line'}
        self.assertEqual(v[T('then sit/JUNO')], 'if "I missed the tram."')
        self.assertEqual(v[T('see/CAZ')], 'if "I\'m off shift."')
        self.assertEqual(v[T("it's a chair/JUNO")], '')          # compact block ends at blank line
        self.assertEqual(v[T('then leave/GUS')], 'if "No."')      # open block runs to "Returns to the choice"
        self.assertEqual(v[T("not sure/BEX")], T('if "Not sure" (the one answer that continues)'))
        self.assertEqual(v[T('neither do i/GUS')], '')         # the continuing answer rejoins the main path

    def test_tone_variant_note_second_speaker(self):
        rows = self.scene(T('QZ-15-Q1'))
        self.assertEqual([(r['speaker'], r['variant']) for r in rows],
                         [(T('BEX'), 'TEASE'), (T('ARLO'), 'TEASE'), (T('BEX'), 'CHEER'), (T('ARLO'), 'CHEER')])

    def test_choice_concede_variant(self):
        r = find(self.rows, T('BEX'), 'it was the dog')
        self.assertEqual(r['variant'], 'concede (third miss of the loop)')

    def test_line_ids_unique(self):
        ids = [r['line_id'] for r in self.rows]
        self.assertEqual(len(ids), len(set(ids)))


def directed(rows):
    out = []
    for r in rows:
        d = dict(r)
        silent = r['kind'] in vc.SILENT_KINDS
        d.update(context='test', emotion_primary='warm', intensity='2', pace='normal', pace_wpm='140',
                 volume='conversational', distance='near', pitch_contour='falling',
                 elevenlabs_text='[exhales]' if silent else r['spoken_text'],
                 elevenlabs_settings='stability=0.40 style=0.15 speed=1.00')
        out.append(d)
    return out


class TestCarryAndValidate(unittest.TestCase):
    def setUp(self):
        self.old = directed(extract_sample())
        self.dcols = carry_forward.direction_columns(list(self.old[0].keys()))

    def test_validator_accepts_and_rejects(self):
        errs, _ = validate_directions.validate(self.old, extract_sample())
        self.assertEqual(errs, [])
        bad = [dict(r) for r in self.old]
        bad[0]['elevenlabs_text'] = 'Where did the LADDER go... today?'   # added a word
        bad[1]['emotion_primary'] = 'grumpy'
        bad[2]['elevenlabs_settings'] = 'stability=0.4 speed=1.5'
        errs, _ = validate_directions.validate(bad[:-1], extract_sample())
        joined = '\n'.join(errs)
        self.assertIn('words differ', joined)
        self.assertIn('not in vocabulary', joined)
        self.assertIn('does not parse', joined)
        self.assertIn('not annotated', joined)

    def test_carry_exact_review_new(self):
        new_a = (SAMPLE_A
                 .replace('I swept the Six.', 'I swept the Six, mind.')              # fuzzy edit
                 .replace(T('ARLO (low): The kettle'), T('ARLO (very low): The kettle'))     # note changed
                 .replace(T('DOV: Tin Stairs.'), T('DOV: Tin Stairs.\n\nKIP: A line nobody wrote before today.')))
        d = tempfile.mkdtemp()
        with open(os.path.join(d, '04a_Test.md'), 'w', encoding='utf-8') as fh:
            fh.write(new_a)
        with open(os.path.join(d, '04b_Test.md'), 'w', encoding='utf-8') as fh:
            fh.write(SAMPLE_B.replace(T('### QZ-05-D1 · The Yard'), T('### QZ-06-D1 · The Yard')))  # renumbered
        new = extract_lines.extract(d, scene_prefix=SCENE_PREFIX)
        out, dropped = carry_forward.carry(self.old, new, self.dcols)
        st = {vc.words_key(r['spoken_text']): r for r in out}
        self.assertEqual(st[T('tin stairs')]['carry_status'], 'EXACT')
        self.assertEqual(st[T('tin stairs')]['emotion_primary'], 'warm')
        self.assertEqual(st[T("reed lane's the nine i swept the six mind")]['carry_status'], 'REVIEW')
        self.assertIn('ratio', st[T("reed lane's the nine i swept the six mind")]['carry_note'])
        self.assertEqual(st['the kettle sings at noon and the hall fills up']['carry_status'], 'REVIEW')
        self.assertIn('delivery note', st['the kettle sings at noon and the hall fills up']['carry_note'])
        self.assertEqual(st['a line nobody wrote before today']['carry_status'], 'NEW')
        self.assertEqual(st['a line nobody wrote before today']['emotion_primary'], '')
        self.assertEqual(st['good count once more']['carry_status'], 'REVIEW')
        self.assertIn('renumbered', st['good count once more']['carry_note'])
        self.assertEqual(st['you would have dropped it']['carry_status'], 'EXACT')
        self.assertEqual(dropped, [])

    def test_short_line_not_matched_globally(self):
        # "Mine." moved to another scene title must not grab a direction from elsewhere.
        new = [dict(r) for r in extract_sample()]
        for r in new:
            if r['scene'] == T('QZ-02-D1'):
                r['scene'], r['scene_title'] = T('QZ-99-X1'), 'Elsewhere'
        out, _ = carry_forward.carry(self.old, new, self.dcols)
        mine = [r for r in out if vc.words_key(r['spoken_text']) == 'mine'][0]
        self.assertEqual(mine['carry_status'], 'NEW')

    def test_cli_runs(self):
        d = tempfile.mkdtemp()
        old_p, new_p, out_p = (os.path.join(d, n) for n in ('old.csv', 'new.csv', 'out.csv'))
        vc.write_csv(old_p, vc.SKELETON_COLUMNS + vc.DIRECTION_COLUMNS, self.old)
        vc.write_csv(new_p, vc.SKELETON_COLUMNS, extract_sample())
        buf = io.StringIO()
        with redirect_stdout(buf):
            carry_forward.main([old_p, new_p, '-o', out_p])
            rc = validate_directions.main([out_p, '--skeleton', new_p])
        self.assertIn('EXACT', buf.getvalue())
        self.assertEqual(rc, 0, buf.getvalue())


VOICE_SAMPLE = T('''## Principals
### Arlo
- TTS: v4: stability 0.40, similarity 0.80. v3/v2: stability 0.40, similarity 0.80, style 0.15, speed 1.0. Drop stability to 0.30 later.
### Moss
- TTS: v4: stability 0.45, similarity 0.80. v3/v2: stability 0.45, similarity 0.80, style 0.10, speed 0.95.
## Restricted roles: minors (human actors)
### Caz
- TTS: TEMP only. v4: stability 0.45, similarity 0.75.
## Minor and day roles
### Clerk
- TTS: v4: stability 0.55, similarity 0.75.
### False Moss
- TTS: Moss voice; v4 stability 0.85, similarity 0.90.
''')


class TestBuild(unittest.TestCase):
    def setUp(self):
        import build_directions
        self.b = build_directions
        d = tempfile.mkdtemp()
        self.vp = os.path.join(d, 'voice_prompts.md')
        with open(self.vp, 'w', encoding='utf-8') as fh:
            fh.write(VOICE_SAMPLE)
        # the story's voice names (build_directions --names), here the sample's own
        self.b.OVERRIDES, self.b.ALIASES = {}, {T('ARLO AND BEX'): T('ARLO')}
        self.b._REF = {T('moss voice'): T('MOSS')}
        self.bases = self.b.load_bases(self.vp)

    def test_load_names_shapes(self):
        def load(text):
            p = os.path.join(tempfile.mkdtemp(), 'names.json')
            with open(p, 'w', encoding='utf-8-sig') as fh:   # a BOM is fine
                fh.write(text)
            return self.b.load_names(p)
        saved = (self.b.OVERRIDES, self.b.ALIASES, self.b._REF)
        try:
            self.assertIsNone(load('{"overrides": {"MOSS": [0.5, 0.1, 1.0, 0.8, false]}, "aliases": {}, "refs": {}}'))
            self.assertEqual(self.b.OVERRIDES['MOSS'], (0.5, 0.1, 1.0, 0.8, False))
            self.assertIn('JSON object', load('[]'))
            self.assertIn('overrides', load('{"overrides": {"MOSS": [0.5]}}'))
            self.assertIn('overrides', load('{"overrides": []}'))
            self.assertIn('aliases', load('{"aliases": {"A": 1}}'))
            self.assertIn('cannot read', load('{'))
        finally:
            self.b.OVERRIDES, self.b.ALIASES, self.b._REF = saved

    def test_bases(self):
        bf = lambda s: self.b.base_for(s, self.bases)  # noqa: E731
        self.assertEqual(bf(T('ARLO')), (0.40, 0.15, 1.0, 0.80, False))
        self.assertTrue(bf(T('CAZ'))[4])                         # minor -> TEMP
        self.assertFalse(bf('CLERK')[4])                      # day role is not TEMP
        self.assertEqual(bf(T('FALSE MOSS'))[:4], (0.85, 0.10, 0.95, 0.90))  # inherits Moss
        self.assertEqual(bf(T('ARLO and BEX')), bf(T('ARLO')))
        self.assertIs(bf('NOBODY'), self.b.DAY)
        self.bases['UNKNOWN VOICE'] = (0.6, 0.05, 0.9, 0.85, False)
        self.assertIs(bf('UNKNOWN'), self.b.DAY)

    def test_pause_mapping(self):
        self.assertEqual(self.b.el_from_pauses(T('Emrys. // Stay.')), T('Emrys. [short pause] Stay.'))
        self.assertEqual(self.b.el_from_pauses('Is the shop / open?'), 'Is the shop… open?')
        self.assertEqual(self.b.el_from_pauses('Late. / Sorry.'), 'Late. Sorry.')

    def test_row_caps_only_when_not_soft(self):
        skel = {r['line_id']: r for r in extract_sample()}
        r = [x for x in skel.values() if vc.words_key(x['spoken_text']) == 'you would have dropped it'][0]
        f = [r['line_id'], 'ctx', 'teasing', '2', 'normal 140', 'conversational,near', 'falling',
             '=', 'dropped', 'sub', 'none', '', '', '', 'n']
        loud = self.b.build_row(r, f, self.bases)
        self.assertIn('DROPPED', loud['elevenlabs_text'])
        f[5] = 'soft,near'
        soft = self.b.build_row(r, f, self.bases)
        self.assertNotIn('DROPPED', soft['elevenlabs_text'])
        self.assertEqual(soft['elevenlabs_settings'], 'stability=0.50 style=0.10 speed=1.00 similarity=0.75')
        errs, _ = validate_directions.validate([soft])
        self.assertEqual(errs, [])


if __name__ == '__main__':
    unittest.main()
