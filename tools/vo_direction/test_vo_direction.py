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

CAZ (on a sack, picking at her sleeve): What was the rope for?

ARLO: …

CAZ: I tied six knots. *(beat)* What was it for?

# ACT I — TEST

### QZ-02-D1 · The Wall

***ACTING.** Patrol barks are Arlo alone, getting shorter: "Mine." / "Got it." / (breath only). Nobody answers.*

### QZ-02-U1 · The Map

DOV: Tin Stairs.

BEX: *[pick]* "East Dock." / "Reed Lane."

DOV *(if "Reed Lane")*: Reed Lane's the Four. *(beat)* I drove the Two.

DOV *(if "East Dock")*: East Dock. *(He nods.)*

### QZ-02-D2 · The Audit

> **Audit** (Arlo's options):
> 1. "Into me. All of it."
> 2. "Nowhere. I block it."

ARLO (low): The bell rings at nine and nobody comes in late.

## Camps

## CP-E1 · "Test Camp"

*Choice:* **[CHOICE: ROAST / REASSURE]** sets `F_MOOD`.

DOV: Line Three went to the sea.
<sub>VO-CP-E1-001</sub>

**[VARIANT: if ROAST]**

BEX: You would have missed it.
<sub>VO-CP-E1-002</sub>

**[VARIANT: if REASSURE]**

BEX: Take me after the final.
<sub>VO-CP-E1-003</sub>
''')

SAMPLE_B = T('''# TEST SCRIPT, PART 2
# ACT II — TEST
### QZ-05-D1 · The Yard

> **[VARIANT: F_MOOD = ROAST]** ARLO (bark): You count like a bell. Keep ringing.
> **[VARIANT: F_MOOD = REASSURE]** ARLO (bark): Good count. Again.

- ARLO (ROAST): You dodge like you are filing a form.
- BEX (REASSURE): Breathe. You are fine.

ARLO and BEX (together): What if we are late?
''')


SAMPLE_C = T('''# TEST SCRIPT, PART 3
# ACT IV — TEST

### QZ-11-HE1 · The Square

EMRYS: Hi.
FAYE: Hello.
ARLO: Go on.
THE EASEL: Begin.

*[direction.hidden · VARIANT]* Concede (third miss): EMRYS (very small): Ask her the one she never answers. Then play "Ask her the last one."

*[direction.hidden · VARIANT]* Concede (second miss): FAYE (hoarse): Mill Lane. Then play "Mill Lane?". If F_ROUNDS = COMPLETE, Lark's first pick plays "Mill Lane?".

*[direction.hidden · VARIANT]* Reads F_TALLY. If ≥ 2: ARLO (low): Two filled. BEX: The chair. If < 2: ARLO (low): One empty.

### QZ-11-Q3 · The Kitchen

*[direction.hidden · OPTIONAL]* The last walk (each plays once, on interaction):
- **The pot.** Arlo scrapes the soup into the bowl. No line.
- **The window.** The drawn dawn. ARLO (to himself): That's nearly a real colour.
- **The beans.** The beans on the table. BEX (from the door, not turning): Contraband. ARLO: It's a bean.

### QZ-12-Q3 · The Easel

*She sets the easel down. And the easel says, sweet and sad:* "Your brush is still wet."

*Emrys asks Gus about the work. Gus tries to answer, "A Tinker's day is mainly—" and goes quiet.*

*In the margin, in Arlo's hand:* "Ask Dov."

*"Not here," he says, and walks on.*

### QZ-13-Q2 · The Roll

*Another: "Hugo." Another: "Ivo." "Kip."*

HUGO: Here.
IVO: Here.

### QZ-14-Q2 · The Supper

***CHOICE (flavor only).** Two lines appear: "I lost a game." / "I'm between houses."*

*If "I lost a game.":*
BEX: I lost a game.
JUNO: Then eat.

*If "I'm between houses.":*
BEX: I'm between houses.
CAZ: See.

JUNO: It's a bed.

*If "No.":*

BEX: No.

GUS: Then go.

*(Returns to the choice.)*

*If "Not sure" (the only answer that continues):*

BEX: Not sure.

GUS: Neither do I.

### QZ-15-Q1 · The Stall

***VARIANT (pair barks).** Reads `F_MOOD`. ROAST: BEX: "Floor's fault." / ARLO: "I'm the hole." REASSURE: BEX: "Next one." / ARLO: "I've got it."*

### QZ-16-Q1 · The Table

***CHOICE.** Concede (third miss of the loop): Bex supplies it.*

> BEX (quietly): It was his.
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
        h = vc.text_hash('I drove the Two.')
        self.assertEqual(h, vc.text_hash('[sighs] I drove... the TWO.'))
        self.assertEqual(h, vc.text_hash('I drove / the Two. //'))
        self.assertEqual(h, vc.text_hash('I drove the *Two.* *(beat)*'))
        self.assertNotEqual(h, vc.text_hash('I drove the Three.'))

    def test_curly_apostrophe(self):
        self.assertEqual(vc.text_hash(T('Reed Lane’s the Four.')), vc.text_hash(T("Reed Lane's the Four.")))


class TestExtract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rows = extract_sample()

    def test_standard_line_and_delivery(self):
        r = find(self.rows, T('CAZ'), 'i tied six knots what was it for')
        self.assertEqual(r['scene'], T('QZ-P0-01'))
        self.assertEqual(r['act'], 'Prologue')
        self.assertIn('beat', r['delivery_original'])
        r = find(self.rows, T('CAZ'), 'what was the rope for')
        self.assertIn('picking at her sleeve', r['delivery_original'])

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
        r = find(self.rows, T('DOV'), T("reed lane's the four i drove the two"))
        self.assertIn(T('Reed Lane'), r['variant'])

    def test_audit_options(self):
        opts = [r for r in self.rows if r['scene'] == T('QZ-02-D2') and r['kind'] == 'choice_option']
        self.assertEqual(len(opts), 2)
        self.assertTrue(all(r['speaker'] == T('ARLO') for r in opts))
        r = find(self.rows, T('ARLO'), 'the bell rings at nine and nobody comes in late')
        self.assertEqual(r['variant'], '')

    def test_camp_vo_ids_and_tone_variants(self):
        r = find(self.rows, T('BEX'), 'you would have missed it')
        self.assertEqual(r['line_id'], 'VO-CP-E1-002')
        self.assertIn('ROAST', r['variant'])
        r = find(self.rows, T('BEX'), 'take me after the final')
        self.assertIn('REASSURE', r['variant'])
        self.assertEqual(find(self.rows, T('DOV'), 'line three went to the sea')['variant'], '')

    def test_barks_each_variant_its_own_row(self):
        barks = [r for r in self.rows if r['scene'] == T('QZ-05-D1') and r['kind'] == 'bark']
        self.assertEqual(len(barks), 4)
        self.assertEqual(sum('ROAST' in r['variant'] for r in barks), 2)
        self.assertEqual(sum('REASSURE' in r['variant'] for r in barks), 2)
        self.assertEqual(barks[0]['act'], 'II')

    def test_joint_speaker(self):
        find(self.rows, T('ARLO and BEX'), 'what if we are late')

    def test_line_ids_unique(self):
        ids = [r['line_id'] for r in self.rows]
        self.assertEqual(len(ids), len(set(ids)))


class TestExtractEmbedded(unittest.TestCase):
    """Bug (a): lines embedded in notes/stage directions; bug (b): blank variants."""
    @classmethod
    def setUpClass(cls):
        cls.rows = extract_sample_c()

    def scene(self, sc):
        return [r for r in self.rows if r['scene'] == sc]

    def test_hidden_variant_concede_lines(self):
        r = find(self.rows, T('EMRYS'), 'ask her the one she never answers')
        self.assertEqual((r['kind'], r['variant']), ('embedded', 'concede (third miss)'))
        self.assertEqual(r['delivery_original'], 'very small')
        r = find(self.rows, T('FAYE'), T('mill lane'))
        self.assertEqual(r['variant'], 'concede (second miss)')
        # "Then play ..." and the trailing If-clause are not part of the line
        self.assertFalse(any('ask her the last one' == vc.words_key(x['spoken_text']) for x in self.rows))

    def test_hidden_variant_reads_flag_conditions(self):
        self.assertEqual(find(self.rows, T('ARLO'), 'two filled')['variant'], T('if F_TALLY ≥ 2'))
        self.assertEqual(find(self.rows, T('BEX'), 'the chair')['variant'], T('if F_TALLY ≥ 2'))
        self.assertEqual(find(self.rows, T('ARLO'), 'one empty')['variant'], T('if F_TALLY < 2'))

    def test_optional_bullets_several_speakers(self):
        rows = self.scene(T('QZ-11-Q3'))
        self.assertEqual([(r['speaker'], vc.words_key(r['spoken_text'])) for r in rows],
                         [(T('ARLO'), "that's nearly a real colour"), (T('BEX'), 'contraband'),
                          (T('ARLO'), T("it's a bean"))])
        self.assertEqual(rows[1]['variant'], T('optional: the beans'))
        self.assertEqual(rows[1]['delivery_original'], 'from the door, not turning')

    def test_stage_direction_quotes(self):
        rows = self.scene(T('QZ-12-Q3'))
        self.assertEqual(find(rows, T('THE EASEL'), T('your brush is still wet'))['kind'], 'embedded')
        find(rows, T('GUS'), T("a tinker's day is mainly"))
        r = find(rows, 'UNKNOWN', 'not here')
        self.assertIn('CHECK speaker', r['parse_note'])
        # written text (a margin note) is not a VO line
        self.assertEqual(len(rows), 3)

    def test_roll_call(self):
        rows = [r for r in self.scene(T('QZ-13-Q2')) if r['kind'] == 'embedded']
        self.assertEqual([r['speaker'] for r in rows], [T('HUGO'), T('IVO'), 'UNKNOWN'])

    def test_if_block_scoping(self):
        v = {vc.words_key(r['spoken_text']) + '/' + r['speaker']: r['variant']
             for r in self.scene(T('QZ-14-Q2')) if r['kind'] == 'line'}
        self.assertEqual(v[T('then eat/JUNO')], 'if "I lost a game."')
        self.assertEqual(v[T('see/CAZ')], 'if "I\'m between houses."')
        self.assertEqual(v[T("it's a bed/JUNO")], '')          # compact block ends at blank line
        self.assertEqual(v[T('then go/GUS')], 'if "No."')      # open block runs to "Returns to the choice"
        self.assertEqual(v[T("not sure/BEX")], T('if "Not sure" (the only answer that continues)'))
        self.assertEqual(v[T('neither do i/GUS')], '')         # the continuing answer rejoins the main path

    def test_tone_variant_note_second_speaker(self):
        rows = self.scene(T('QZ-15-Q1'))
        self.assertEqual([(r['speaker'], r['variant']) for r in rows],
                         [(T('BEX'), 'ROAST'), (T('ARLO'), 'ROAST'), (T('BEX'), 'REASSURE'), (T('ARLO'), 'REASSURE')])

    def test_choice_concede_variant(self):
        r = find(self.rows, T('BEX'), 'it was his')
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
        bad[0]['elevenlabs_text'] = 'What was the ROPE for... today?'   # added a word
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
                 .replace('I drove the Two.', 'I drove the Two, mind.')              # fuzzy edit
                 .replace(T('ARLO (low): The bell'), T('ARLO (very low): The bell'))     # note changed
                 .replace(T('DOV: Tin Stairs.'), T('DOV: Tin Stairs.\n\nDUY: A line nobody wrote before today.')))
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
        self.assertEqual(st[T("reed lane's the four i drove the two mind")]['carry_status'], 'REVIEW')
        self.assertIn('ratio', st[T("reed lane's the four i drove the two mind")]['carry_note'])
        self.assertEqual(st['the bell rings at nine and nobody comes in late']['carry_status'], 'REVIEW')
        self.assertIn('delivery note', st['the bell rings at nine and nobody comes in late']['carry_note'])
        self.assertEqual(st['a line nobody wrote before today']['carry_status'], 'NEW')
        self.assertEqual(st['a line nobody wrote before today']['emotion_primary'], '')
        self.assertEqual(st['good count again']['carry_status'], 'REVIEW')
        self.assertIn('renumbered', st['good count again']['carry_note'])
        self.assertEqual(st['you would have missed it']['carry_status'], 'EXACT')
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
        self.assertEqual(self.b.el_from_pauses('Is anyone / there?'), 'Is anyone… there?')
        self.assertEqual(self.b.el_from_pauses('Burned. / Sorry.'), 'Burned. Sorry.')

    def test_row_caps_only_when_not_soft(self):
        skel = {r['line_id']: r for r in extract_sample()}
        r = [x for x in skel.values() if vc.words_key(x['spoken_text']) == 'you would have missed it'][0]
        f = [r['line_id'], 'ctx', 'teasing', '2', 'normal 140', 'conversational,near', 'falling',
             '=', 'missed', 'sub', 'none', '', '', '', 'n']
        loud = self.b.build_row(r, f, self.bases)
        self.assertIn('MISSED', loud['elevenlabs_text'])
        f[5] = 'soft,near'
        soft = self.b.build_row(r, f, self.bases)
        self.assertNotIn('MISSED', soft['elevenlabs_text'])
        self.assertEqual(soft['elevenlabs_settings'], 'stability=0.50 style=0.10 speed=1.00 similarity=0.75')
        errs, _ = validate_directions.validate([soft])
        self.assertEqual(errs, [])


if __name__ == '__main__':
    unittest.main()
