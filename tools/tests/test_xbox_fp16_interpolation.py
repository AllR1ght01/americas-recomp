"""Check Xbox half-interpolation rounding through the fragment recompiler."""
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
DRIVER = Path(os.environ.get(
    "XBOX_DRIVER", ROOT / "build-xbox" / ("driver.exe" if os.name == "nt" else "driver")
)).resolve()

# v_interp_p1ll_f16 v2, v0, attr0.x
# v_interp_p2_f16 v3, v1, attr0.x, v2
# Export the live result to MRT0, then end the program.
PIXEL = (0xD7420002, 0x00020000, 0xD75A0003, 0x040A0200,
         0xF8000801, 0x00000003, 0xBF810000)


@unittest.skipUnless(DRIVER.is_file(), "build the Xbox driver or set XBOX_DRIVER")
class Fp16InterpolationTest(unittest.TestCase):
    def compile(self, mode):
        with tempfile.TemporaryDirectory(prefix="xbox-fp16-") as temp:
            work = Path(temp)
            (work / "pixel.bin").write_bytes(struct.pack("<7I", *PIXEL))
            env = dict(os.environ, APS5_DUMP_IR="all", XBOX_FRAGMENT_BARY="1")
            env.pop("XBOX_ALLOW_UNMEASURED_F16_INTERP", None)
            return subprocess.run(
                [DRIVER, "pixel.bin", "64", "pixel.spv", "empty.bind",
                 "1", "ps", "2", "1", "1", "0", mode],
                cwd=work, env=env, text=True, capture_output=True, timeout=30,
            )

    def test_p2_uses_fp16_rounding_field(self):
        for mode, flags in (("c0", 0x02), ("c4", 0x0A), ("c8", 0x12), ("cc", 0x1A)):
            with self.subTest(mode=mode):
                result = self.compile(mode)
                self.assertEqual(result.returncode, 0, result.stdout.partition(" RecompileRequest:")[0] + result.stderr)
                for opcode, expected in (("FPInterpolateF32", 0x02), ("FPInterpolateF16", flags)):
                    operands = re.findall(rf"{opcode}[^\n]+, (0x[\da-fA-F]+) \(", result.stderr)
                    self.assertTrue(operands, opcode)
                    self.assertEqual({int(value, 16) for value in operands}, {expected})

    def test_p1_directed_fp32_rounding_stays_unsupported(self):
        result = self.compile("cd")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("FLOAT_MODE 205", result.stdout)


if __name__ == "__main__":
    unittest.main()
