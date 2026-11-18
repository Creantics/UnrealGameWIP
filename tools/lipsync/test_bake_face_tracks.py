"""Tests for the gold set's "voices" table in bake_face_tracks.py (made-up data only)."""
import argparse
import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bake_face_tracks as b  # noqa: E402

SAMPLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gold_sample.json')


class TestVoices(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def gold(self, obj):
        p = os.path.join(self.dir, 'gold.json')
        with open(p, 'w', encoding='utf-8') as fh:
            fh.write(obj if isinstance(obj, str) else json.dumps(obj))
        return p

    def test_sample_voices(self):
        lines = b.load_gold(SAMPLE)
        self.assertEqual([b.voice_for(ln) for ln in lines], [('en-us+f4', 80), ('en-us+m3', 40), ('en-us', 50)])

    def test_table_matches_any_case(self):
        lines = b.load_gold(self.gold({'voices': {'PIPER': ['en-us+f2', 70]},
                                       'lines': [{'speaker': 'Piper'}, {'speaker': 'Unknown Man'}]}))
        self.assertEqual([b.voice_for(ln) for ln in lines], [('en-us+f2', 70), ('en-us+m3', 40)])

    def test_built_in_child_voice(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            lines = b.load_gold(self.gold({'lines': [{'speaker': 'Child'}, {'speaker': 'Arlo'}]}))
        self.assertIn('no "voices" table', err.getvalue())
        self.assertEqual([b.voice_for(ln) for ln in lines], [('en-us+f4', 80), ('en-us', 50)])

    def test_table_overrides_built_in(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            lines = b.load_gold(self.gold({'voices': {'CHILD': ['en-us+f2', 60]}, 'lines': [{'speaker': 'CHILD'}]}))
        self.assertEqual(err.getvalue(), '')
        self.assertEqual(b.voice_for(lines[0]), ('en-us+f2', 60))

    def test_bad_tables(self):
        for bad in ({'voices': [], 'lines': []}, {'voices': {'A': ['x', 120]}, 'lines': []},
                    {'voices': {'A': ['x', True]}, 'lines': []}, {'voices': {'A': ['', 5]}, 'lines': []},
                    {'lines': {}}, {'lines': [1]}, [], '{bad'):
            with self.assertRaises(b.GoldError, msg=str(bad)):
                b.load_gold(self.gold(bad))

    def run_gold(self, out):
        csv_path = os.path.join(self.dir, 'dir.csv')
        with open(csv_path, 'w', encoding='utf-8', newline='') as fh:
            fh.write('line_id,text_hash,kind,spoken_text,speaker,volume\n'
                     'L1,aa11,line,Nets are dry today,PIPER,soft\nL2,bb22,line,Then we sail at noon,ARLO,loud\n')
        a = argparse.Namespace(csv=csv_path, out=out, count=2, require=[])
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as err:
            rc = b.cmd_gold(a)
        return rc, err.getvalue()

    def test_gold_keeps_voices(self):
        out = self.gold({'voices': {'PIPER': ['en-us+f4', 80]}, 'lines': []})
        rc, err = self.run_gold(out)
        self.assertIsNone(rc, err)
        with open(out, encoding='utf-8') as fh:
            d = json.load(fh)
        self.assertEqual(d['voices'], {'PIPER': ['en-us+f4', 80]})
        self.assertEqual(len(d['lines']), 2)

    def test_gold_refuses_bad_old_file(self):
        for bad in ('{bad', json.dumps({'voices': {'A': ['x', 500]}, 'lines': []})):
            out = self.gold(bad)
            rc, err = self.run_gold(out)
            self.assertEqual(rc, 2, err)
            with open(out, encoding='utf-8') as fh:
                self.assertEqual(fh.read(), bad)   # left as it was

    def test_gold_new_file_has_no_voices(self):
        out = os.path.join(self.dir, 'new.json')
        rc, err = self.run_gold(out)
        self.assertIsNone(rc, err)
        with open(out, encoding='utf-8') as fh:
            self.assertNotIn('voices', json.load(fh))


if __name__ == '__main__':
    unittest.main()
