import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent
SCRIPT = TOOLS / "xbox" / "platform" / "wine_d3d12.sh"
REQUIRED = ("wine", "wineboot", "x86_64-w64-mingw32-g++", "timeout")


@unittest.skipUnless(
    sys.platform.startswith("linux"), "the Wine smoke test is Linux only"
)
class WineD3D12Test(unittest.TestCase):
    def test_clear_matches_expected_words(self):
        missing = [tool for tool in REQUIRED if shutil.which(tool) is None]
        if missing:
            self.skipTest("missing " + ", ".join(missing))
        with tempfile.TemporaryDirectory() as directory:
            proc = subprocess.run(
                [str(SCRIPT), directory], capture_output=True, text=True, timeout=600
            )
            output = proc.stdout + proc.stderr
            self.assertEqual(proc.returncode, 0, output)
            report = json.loads(
                (Path(directory) / "report.json").read_text(encoding="utf-8")
            )
            self.assertIn(report["result"], ("pass", "skip"), output)
            if report["result"] == "skip":
                self.assertTrue(report["skipped"], output)
                return
            self.assertIn("RESULT: PASS", proc.stdout)
            self.assertTrue(report["device"])
            self.assertTrue(report["adapter"])
            self.assertTrue(report["feature_level"])
            self.assertEqual(report["clear"], report["expected"])


if __name__ == "__main__":
    unittest.main()
