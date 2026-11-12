"""Tests for story_import's tables (load_tables) and the missing-choice fallback, on a small made-up story.

    python3 -I tools/story_import/test_story_import.py
"""
import contextlib
import copy
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import story_import  # noqa: E402

TABLES = {
    'title': 'Example Story',
    'worlds': ['Harbour'],
    'scene_count': 1,
    'script_dir': 'script',
    'scene_id_prefix': 'XA-',
    'hold_ladder': {},
    'ladder_anchor': {},
    'gameplay_keywords': [['ENG', 'walk']],
    'choices': {},
    'mini': {},
    'grammar': {
        'chapter_heading': r'^## SECTION (\d+) · (.*)$',
        'part_heading': r'^## (O\d) · (.*)$',
        'epilogue_heading': r'^## CODA · (.*)$',
        'prologue_heading': r'^## OPENING · (.*)$',
        'act_heading': r'^# (PART ([IVX]+)) — (.*)$',
        'file_act_heading': r'^# .*BOOK \d+: (PART ([IVX]+)) — (.*)$',
        'finale_marker': 'CLOSE',
        'finale_from_chapter': None,
        'labels': {'act': 'Part %s', 'prologue': 'Opening', 'epilogue': 'Coda', 'finale': 'Close'},
    },
    'chapter_labels': {'prologue': 'Opening', 'epilogue': 'Coda', 'chapter': 'Section %d'},
}

SCRIPT = '''# BOOK 1: PART I — The Quay

## SECTION 1 · The Quay

### XA-01-A1 · Morning Nets

**Mode:** Scene · **Gameplay:** walk

*Gulls over the quay.*

ARLO: Nets are dry.

BEX: Then we sail.

'''

INSERTS = ('"scene_id","before_line_prefix","notation"\n'
           '"XA-01-A1","BEX: Then we sail.","[CHOICE: a made-up note that must not be copied]"\n')
VO = ('VO ID,Scene ID,Scene title,Context,Speaker,Delivery tag,Line (spoken),Inline direction,Words,Est. seconds,'
      'Performance notes\n'
      'VO-01-A1-001,XA-01-A1,Morning Nets,,ARLO,,Nets are dry.,,3,1,\n'
      'VO-01-A1-002,XA-01-A1,Morning Nets,,BEX,,Then we sail.,,3,1,\n')
META = ('"scene_id","day","time_of_day","location","world","cast_on_stage","props_in","props_carry","prereq_flags",'
        '"objective","stakes","mood_dial","color_script","sound_cues","sound_notes","tell_insert","flags_set",'
        '"flags_read","exit_assumes","clue_ids","reclaim","vo_notes","loc_hazards","second_read"\n'
        '"XA-01-A1","1","dawn","Quay","Harbour","ARLO; BEX","","","","sail","","","","","","","","","","","","","",""\n')
FLAGS = 'flag_id,type,description,set_by,read_by,default,notes\n'
CUES = ('cue_id,name,type,palette,short_description,key_or_mode,tempo_bpm,instrumentation,motif_notation,'
        'loop_or_stinger,used_in_scenes,entry_rule,exit_rule,mix_notes\n')


def write(path, text, bom=False):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8-sig' if bom else 'utf-8', newline='\n') as fh:
        fh.write(text)


class TablesBase(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.build = os.path.join(self.dir, 'build')
        write(os.path.join(self.build, 'script', '04a_Script_Example.md'), SCRIPT)
        write(os.path.join(self.build, 'script', 'VO_Master_List.csv'), VO)
        write(os.path.join(self.build, 'meta', 'meta_01.csv'), META)
        write(os.path.join(self.build, 'meta', 'inserts_01.csv'), INSERTS)
        write(os.path.join(self.build, 'flags.csv'), FLAGS)
        write(os.path.join(self.build, 'cues_final.csv'), CUES)
        self.out = os.path.join(self.dir, 'out')

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def run_tool(self, tables, raw=None, bom=False, extra=()):
        path = os.path.join(self.dir, 'tables.json')
        write(path, raw if raw is not None else json.dumps(tables), bom=bom)
        err, out = io.StringIO(), io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
            rc = story_import.main(['--tables', path, '--build-dir', self.build, '--out-dir', self.out] + list(extra))
        return rc, err.getvalue() + out.getvalue()


class TestBadTables(TablesBase):
    """Each wrong shape exits 2 with a plain message (no traceback)."""

    def refused(self, tables=None, raw=None, needle=''):
        rc, text = self.run_tool(tables, raw)
        self.assertEqual(rc, 2, text)
        self.assertIn(needle, text)
        self.assertNotIn('Traceback', text)

    def changed(self, key, value):
        t = copy.deepcopy(TABLES)
        t[key] = value
        return t

    def test_not_json(self):
        self.refused(raw='{bad', needle='cannot read the tables')

    def test_top_level_not_object(self):
        self.refused(raw='[1, 2]', needle='not a JSON object')

    def test_missing_key(self):
        t = copy.deepcopy(TABLES)
        del t['choices']
        self.refused(t, needle='lack: choices')

    def test_scene_count_not_int(self):
        self.refused(self.changed('scene_count', '1'), needle='scene_count')
        self.refused(self.changed('scene_count', True), needle='scene_count')

    def test_ladder_anchor_bad_regex(self):
        self.refused(self.changed('ladder_anchor', {'XA-01-A1': '(unclosed'}), needle='not a valid regex')

    def test_gameplay_keyword_not_pair(self):
        self.refused(self.changed('gameplay_keywords', [['ENG']]), needle='gameplay_keywords[0]')
        self.refused(self.changed('gameplay_keywords', 'ENG'), needle='gameplay_keywords is not a list')
        self.refused(self.changed('gameplay_keywords', [['ENG', '[']]), needle='not a valid regex')

    def test_hold_ladder_wrong_type(self):
        self.refused(self.changed('hold_ladder', [1.5]), needle='hold_ladder')
        self.refused(self.changed('hold_ladder', {'XA-01-A1': 'long'}), needle='hold_ladder')

    def test_choices_wrong_type(self):
        self.refused(self.changed('choices', []), needle='choices is not an object')
        self.refused(self.changed('choices', {'XA-01-A1': [['label', 'yes', []]]}), needle='choices[XA-01-A1][0]')
        self.refused(self.changed('choices', {'XA-01-A1': [['label', True, [{'effects': []}]]]}),
                     needle='option 0')
        self.refused(self.changed('choices', {'XA-01-A1': [['label', True, [{'text': 'Yes', 'effects': [],
                                                                              'next': None}]]]}),
                     needle='selectable and miss')

    def test_mini_wrong_type(self):
        self.refused(self.changed('mini', {'XA-01-A1': [[None]]}), needle='mini[XA-01-A1][0]')
        self.refused(self.changed('mini', {'XA-01-A1': [[None, [['line', 'ARLO']]]]}), needle='fields after the kind')
        self.refused(self.changed('mini', {'XA-01-A1': [[None, [['hold', 'long', None]]]]}), needle='wrong type')
        self.refused(self.changed('mini', {'XA-01-A1': [[None, [['choice', 'Go?', True, [{'text': 'Yes'}]]]]]}),
                     needle='option 0')

    def test_effect_not_triple(self):
        opt = {'text': 'Yes', 'effects': [[5]], 'next': None, 'selectable': True, 'miss': False}
        self.refused(self.changed('choices', {'XA-01-A1': [['label', True, [opt]]]}), needle='effect 0')

    def test_worlds_and_text(self):
        self.refused(self.changed('worlds', 'Harbour'), needle='worlds')
        self.refused(self.changed('scene_id_prefix', ''), needle='scene_id_prefix')

    def test_grammar(self):
        g = copy.deepcopy(TABLES['grammar'])
        g['act_heading'] = '(['
        self.refused(self.changed('grammar', g), needle='grammar.act_heading')
        g = copy.deepcopy(TABLES['grammar'])
        del g['labels']
        self.refused(self.changed('grammar', g), needle='grammar lacks: labels')
        g = copy.deepcopy(TABLES['grammar'])
        g['finale_from_chapter'] = '23'
        self.refused(self.changed('grammar', g), needle='finale_from_chapter')
        g = copy.deepcopy(TABLES['grammar'])
        g['chapter_heading'] = r'^## SECTION (\d+)$'
        self.refused(self.changed('grammar', g), needle='needs at least 2 groups')
        g = copy.deepcopy(TABLES['grammar'])
        g['labels']['act'] = '100% %s'
        self.refused(self.changed('grammar', g), needle='grammar.labels.act')

    def test_chapter_labels(self):
        self.refused(self.changed('chapter_labels', {'prologue': 'A', 'epilogue': 'B', 'chapter': 'C'}),
                     needle='chapter_labels.chapter')
        self.refused(self.changed('chapter_labels', {'prologue': 'A', 'epilogue': 'B', 'chapter': '100% %d'}),
                     needle='chapter_labels.chapter')


class TestGoodTables(TablesBase):
    def test_byte_order_mark_accepted(self):
        t = copy.deepcopy(TABLES)
        t['choices'] = {'XA-01-A1': [['Sail now?', True, [{'text': 'Yes', 'effects': [], 'next': None,
                                                         'selectable': True, 'miss': False}]]]}
        rc, text = self.run_tool(t, bom=True)
        self.assertNotEqual(rc, 2, text)
        self.assertNotIn('cannot read the tables', text)
        self.assertNotIn('no entry in the tables', text)

    def test_failed_import_keeps_good_output(self):
        good = os.path.join(self.out, 'scenes', 'XA-01-A1.json')
        write(good, '{"good": true}')
        rc, text = self.run_tool(TABLES)
        self.assertEqual(rc, 1, text)
        self.assertIn('wrote nothing', text)
        with open(good, encoding='utf-8') as fh:
            self.assertEqual(fh.read(), '{"good": true}')
        self.assertFalse(os.path.exists(os.path.join(self.out, 'loc', 'en.json')))

    def test_missing_choice_fails_with_placeholder(self):
        rc, text = self.run_tool(TABLES, extra=['--allow-problems'])
        self.assertEqual(rc, 1, text)
        self.assertIn('XA-01-A1: CHOICE insert has no entry in the tables (choices)', text)
        with open(os.path.join(self.out, 'scenes', 'XA-01-A1.json'), encoding='utf-8') as fh:
            scene = fh.read()
        with open(os.path.join(self.out, 'loc', 'en.json'), encoding='utf-8') as fh:
            loc = fh.read()
        self.assertIn(story_import.MISSING_CHOICE, loc + scene)
        self.assertNotIn('must not be copied', loc + scene)


if __name__ == '__main__':
    unittest.main()
