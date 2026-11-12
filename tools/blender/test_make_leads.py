"""Tests for make_leads.py's argument and names handling, outside Blender (Blender's modules are stubbed)."""
import copy
import json
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

try:
    import numpy  # noqa: F401
    HAVE_NUMPY = True
except ImportError:
    HAVE_NUMPY = False

if HAVE_NUMPY:
    for m in ("bpy", "bmesh", "mathutils", "ne_blender", "rig_common", "make_test_human"):
        sys.modules.setdefault(m, mock.MagicMock())
    import make_leads as ml  # noqa: E402


@unittest.skipUnless(HAVE_NUMPY, "numpy is not installed")
class TestNames(unittest.TestCase):
    def setUp(self):
        self.saved = copy.deepcopy({k: dict(name=c["name"], code=c["code"]) for k, c in ml.CHARS.items()})
        self.dir = tempfile.mkdtemp()

    def tearDown(self):
        for k, m in self.saved.items():
            ml.CHARS[k].update(m)
        shutil.rmtree(self.dir, ignore_errors=True)

    def names(self, obj):
        p = os.path.join(self.dir, 'names.json')
        with open(p, 'w', encoding='utf-8') as fh:
            fh.write(obj if isinstance(obj, str) else json.dumps(obj))
        return p

    def current(self):
        return {k: (c["name"], c["code"]) for k, c in ml.CHARS.items()}

    def test_defaults_are_the_real_leads(self):
        self.assertEqual(self.current(), {"lead_a": ("devin", "dv"), "lead_b": ("duy", "dy"),
                                          "lead_c": ("third", "tr")})
        self.assertEqual(ml.resolve(ml.parse_args([])), ["lead_a", "lead_b", "lead_c"])

    def test_key_for(self):
        self.assertEqual(ml.key_for("duy"), "lead_b")
        self.assertEqual(ml.key_for("lead_c"), "lead_c")
        with self.assertRaises(KeyError):
            ml.key_for("nobody")

    def test_override_some(self):
        self.assertIsNone(ml.load_names(self.names({"leads": {"lead_b": {"name": "pip", "code": "pp"}}})))
        self.assertEqual(self.current()["lead_b"], ("pip", "pp"))
        self.assertEqual(self.current()["lead_a"], ("devin", "dv"))
        self.assertEqual(ml.key_for("pip"), "lead_b")

    def test_refused_files_change_nothing(self):
        before = self.current()
        for bad in ('{bad', [], {"leads": []}, {"leads": {"lead_z": {"name": "x"}}},
                    {"leads": {"lead_a": {"name": "x y"}}}, {"leads": {"lead_a": {"name": "x", "age": 3}}},
                    {"leads": {"lead_a": {"name": "duy"}}}, {"leads": {"lead_a": {"code": "dy"}}},
                    {"leads": {"lead_a": {"name": "lead_b"}}}):
            self.assertIsInstance(ml.load_names(self.names(bad)), str, bad)
            self.assertEqual(self.current(), before, bad)
        self.assertIsInstance(ml.load_names(os.path.join(self.dir, 'missing.json')), str)

    def test_parse_args(self):
        o = ml.parse_args(["--outdir", "out", "--only", "duy,lead_c", "--blend-dir", "b"])
        self.assertEqual((o["outdir"], o["blend_dir"]), ("out", "b"))
        self.assertEqual(ml.resolve(o), ["lead_b", "lead_c"])
        for bad in (["--only"], ["--only", "--outdir", "x"], ["--outdir", "x", "--names"], ["--names", ""],
                    ["--outdir", "a", "--only", "--names", "n"], ["stray"], ["--onyl", "duy"]):
            self.assertIsInstance(ml.parse_args(bad), str, bad)

    def test_unknown_only_and_bad_names_are_errors(self):
        self.assertIn("nobody", ml.resolve(ml.parse_args(["--only", "duy,nobody"])))
        self.assertIsInstance(ml.resolve(ml.parse_args(["--names", os.path.join(self.dir, 'none.json')])), str)

    def test_main_exits_2(self):
        for argv in (["--only"], ["--only", "nobody"]):
            with mock.patch.object(sys, 'argv', ["blender", "--"] + argv), \
                    mock.patch.object(ml, 'build_character') as build, \
                    mock.patch('sys.stderr'):
                with self.assertRaises(SystemExit) as cm:
                    ml.main()
                self.assertEqual(cm.exception.code, 2)
                build.assert_not_called()


if __name__ == '__main__':
    unittest.main()
