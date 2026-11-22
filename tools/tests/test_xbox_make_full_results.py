import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SCRIPT = ROOT / "tools" / "xbox" / "make_full_results.py"

MANIFEST = """idx,stage
1,CS
2,PS
3,VS
4,?
5,GS
6,PS
7,PS
8,PS
9,PS
10,VS
11,?
12,CS
"""

GRAPHICS = [
    {"idx": "1", "status": "VALID"},
    {"idx": "2", "status": "VALID"},
    {"idx": "3", "status": "VALID"},
    {"idx": "4", "status": "VALID"},
    {"idx": "5", "status": "VALID"},
    {"idx": "6", "status": "VALID_BARY_EXT"},
    {"idx": "7", "status": "THROW"},
    {"idx": "8", "status": "VALID_BARY_EXT"},
    {"idx": "9", "status": "THROW"},
    {"idx": "10", "status": "VALID_BARY_EXT"},
    {"idx": "11", "status": "VALID_BARY_EXT"},
]

CORPUS = [
    {"idx": "1", "status": "VALID"},
    {"idx": "2", "status": "VALID", "relaxed": True},
    {"idx": "7", "status": "VALID", "relaxed": True},
    {"idx": "9", "status": "FAIL"},
    {"idx": "12", "status": "THROW"},
    {"idx": "100", "status": "VALID"},
]

INITIAL = {
    "compute": {"1": "stale", "99": "keep"},
    "pixel": {"old": "strict"},
    "vertex": {"old": "strict"},
    "mesh": {"old": "valid"},
    "platform": {"kept": True},
    "todo": ["kept"],
}


class MakeFullResultsTest(unittest.TestCase):
    def make_inputs(self, tmp):
        d = Path(tmp)
        (d / "manifest.csv").write_text(MANIFEST, encoding="utf-8")
        (d / "graphics.json").write_text(json.dumps(GRAPHICS), encoding="utf-8")
        (d / "corpus.json").write_text(json.dumps(CORPUS), encoding="utf-8")
        (d / "results.json").write_text(json.dumps(INITIAL), encoding="utf-8")
        return d

    def run_script(self, d, out=None):
        cmd = [sys.executable, "-I", str(SCRIPT),
               "--manifest", str(d / "manifest.csv"),
               "--results", str(d / "results.json"),
               "--graphics", str(d / "graphics.json"),
               "--corpus", str(d / "corpus.json")]
        if out:
            cmd += ["--out", str(d / out)]
        p = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        return p.stdout

    def test_stage_rules(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = self.make_inputs(tmp)
            self.run_script(d)
            r = json.loads((d / "results.json").read_text(encoding="utf-8"))
            self.assertEqual(r["compute"], {"1": "valid", "99": "keep", "12": "fail"})
            self.assertEqual(r["pixel"], {"2": "strict", "6": "relaxed", "7": "relaxed", "8": "relaxed", "9": "fail"})
            self.assertEqual(r["vertex"], {"3": "strict", "10": "fail"})
            self.assertEqual(r["mesh"], {"4": "valid", "11": "fail"})
            self.assertEqual(r["platform"], INITIAL["platform"])
            self.assertEqual(r["todo"], INITIAL["todo"])

    def test_out_leaves_input_untouched(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = self.make_inputs(tmp)
            out = self.run_script(d, "merged.json")
            self.assertEqual(json.loads((d / "results.json").read_text(encoding="utf-8")), INITIAL)
            self.assertEqual(json.loads((d / "merged.json").read_text(encoding="utf-8"))["compute"]["1"], "valid")
            for stage in ("compute", "pixel", "vertex", "mesh"):
                self.assertIn(stage + " ", out)

    def test_missing_values_fail_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = self.make_inputs(tmp)
            (d / "graphics.json").write_text("[]", encoding="utf-8")
            (d / "corpus.json").write_text("[]", encoding="utf-8")
            self.run_script(d, "merged.json")
            r = json.loads((d / "merged.json").read_text(encoding="utf-8"))
            self.assertEqual(r["pixel"], {k: "fail" for k in ("2", "6", "7", "8", "9")})
            self.assertEqual(r["vertex"], {"3": "fail", "10": "fail"})
            self.assertEqual(r["mesh"], {"4": "fail", "11": "fail"})
            self.assertEqual(r["compute"], {"1": "stale", "99": "keep"})


if __name__ == "__main__":
    unittest.main()
