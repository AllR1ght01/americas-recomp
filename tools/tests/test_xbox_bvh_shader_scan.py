import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent
SCRIPT = TOOLS / "xbox" / "platform" / "bvh_shader_scan.py"


class BvhShaderScanTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="bvh_scan_test_")
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name)

    def write_shader(self, index, pdb_name=None, calls=()):
        lines = []
        if pdb_name is not None:
            lines.append(f'source_filename = "{pdb_name}.pdb"')
        lines.append("define void @main() {")
        for call in calls:
            lines.append(f"  call void @xbox.{call}()")
        lines.append("  ret void\n}")
        path = self.dir / f"shader_{index:02d}.ll"
        path.write_text("\n".join(lines), encoding="utf-8")
        return path

    def run_tool(self, *extra_args):
        return subprocess.run(
            [sys.executable, "-I", str(SCRIPT), str(self.dir), *map(str, extra_args)],
            capture_output=True,
            text=True,
            timeout=30,
        )

    def test_classifies_all_four_shader_categories(self):
        self.write_shader(0, pdb_name="StandardKernel", calls=())
        self.write_shader(
            1,
            pdb_name="PointerKernel",
            calls=("global_load_dword", "s_load_dword_with_cache"),
        )
        self.write_shader(
            2, pdb_name="WaveKernel", calls=("Ballot64", "LaneSwizzle")
        )
        self.write_shader(
            3, pdb_name="ArithKernel", calls=("Med3_U32", "Min3_F32")
        )

        hw = self.dir / "hw_result.json"
        hw.write_text(
            json.dumps({"0": "OK", "1": "FAIL", "2": "CRASH", "3": "OK"}),
            encoding="utf-8",
        )

        out = self.dir / "custom_out.json"
        result = self.run_tool(hw, out)
        self.assertEqual(result.returncode, 0, result.stderr)

        self.assertIn("standard DXIL (compiles): 1", result.stdout)
        self.assertIn("needs raw 64-bit address access: 1", result.stdout)
        self.assertIn("wave/lane intrinsics, no pointers: 1", result.stdout)
        self.assertIn("arithmetic intrinsics only: 1", result.stdout)
        self.assertIn(
            "['Ballot64', 'LaneSwizzle', 'Med3_U32', 'Min3_F32']",
            result.stdout,
        )

        data = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(
            data,
            {
                "0": {
                    "name": "StandardKernel",
                    "result_on_amd_pc_compiler": "OK",
                    "xbox_intrinsics": [],
                },
                "1": {
                    "name": "PointerKernel",
                    "result_on_amd_pc_compiler": "FAIL",
                    "xbox_intrinsics": [
                        "global_load_dword",
                        "s_load_dword_with_cache",
                    ],
                },
                "2": {
                    "name": "WaveKernel",
                    "result_on_amd_pc_compiler": "CRASH",
                    "xbox_intrinsics": ["Ballot64", "LaneSwizzle"],
                },
                "3": {
                    "name": "ArithKernel",
                    "result_on_amd_pc_compiler": "OK",
                    "xbox_intrinsics": ["Med3_U32", "Min3_F32"],
                },
            },
        )

    def test_pointer_precedence_over_wave_and_arithmetic(self):
        self.write_shader(
            4,
            pdb_name="MixedPointerWave",
            calls=("Ballot64", "global_atomic_add", "Med3_U32"),
        )
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("needs raw 64-bit address access: 1", result.stdout)

    def test_wave_precedence_over_arithmetic(self):
        self.write_shader(
            5,
            pdb_name="MixedWaveArith",
            calls=("ReadLane", "Med3_U32", "Min3_F32"),
        )
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("wave/lane intrinsics, no pointers: 1", result.stdout)

    def test_defaults_when_optional_arguments_omitted(self):
        self.write_shader(7, pdb_name="Standalone", calls=("MakeUniform",))
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)

        default_out = self.dir / "bvh_shader_scan.json"
        self.assertTrue(default_out.is_file())
        data = json.loads(default_out.read_text(encoding="utf-8"))
        self.assertEqual(
            data,
            {
                "7": {
                    "name": "Standalone",
                    "result_on_amd_pc_compiler": "?",
                    "xbox_intrinsics": ["MakeUniform"],
                },
            },
        )

    def test_missing_pdb_falls_back_to_question_mark(self):
        self.write_shader(4, pdb_name=None, calls=())
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)

        data = json.loads(
            (self.dir / "bvh_shader_scan.json").read_text(encoding="utf-8")
        )
        self.assertEqual(data["4"]["name"], "?")

    def test_ignores_non_matching_file_names(self):
        (self.dir / "shader_1.ll").write_text(
            'source_filename = "one.pdb"\n', encoding="utf-8"
        )
        (self.dir / "shader_001.ll").write_text(
            'source_filename = "three.pdb"\n', encoding="utf-8"
        )
        (self.dir / "shader_05.dxil").write_text("bin", encoding="utf-8")
        (self.dir / "shader_xx.ll").write_text(
            'source_filename = "xx.pdb"\n', encoding="utf-8"
        )
        (self.dir / "other.txt").write_text("text", encoding="utf-8")
        self.write_shader(5, pdb_name="Valid", calls=())

        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)

        data = json.loads(
            (self.dir / "bvh_shader_scan.json").read_text(encoding="utf-8")
        )
        self.assertEqual(list(data.keys()), ["5"])

    def test_category_listing_threshold_twelve(self):
        for i in range(12):
            self.write_shader(i, pdb_name=f"P{i}", calls=("global_load_dword",))
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(str(list(range(12))), result.stdout)

    def test_empty_directory(self):
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(
            (self.dir / "bvh_shader_scan.json").read_text(encoding="utf-8")
        )
        self.assertEqual(data, {})


if __name__ == "__main__":
    unittest.main()
