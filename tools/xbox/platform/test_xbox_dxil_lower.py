"""Self-contained offline unit tests for xbox_dxil_lower.py and dxil_sm66_wave.py (no game data or DXC required)."""
import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import xbox_dxil_lower as xdl


SAMPLE_BASE = """\
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-xb2"

%dx.types.Handle = type { i8* }
%dx.types.ResRet.i32 = type { i32, i32, i32, i32, i32 }

define void @main() {
  %h = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 0, i32 0, i1 false)
{body}
  ret void
}

declare %dx.types.Handle @dx.op.createHandle(i32, i8, i32, i32, i1) #0
{decls}
!xbox.compiler.options = !{!0}
!0 = !{i32 1}
"""


class TestXboxDxilLower(unittest.TestCase):
    def test_bundled_kernels_lower_cleanly(self):
        for name in ("oplower_kernel.ll", "wavelower_kernel.ll", "typedload_kernel.ll"):
            path = os.path.join(HERE, name)
            with open(path, encoding="utf-8") as f:
                text = f.read()
            res, n, left = xdl.lower(text)
            self.assertGreater(n, 0, f"{name} should lower at least one intrinsic")
            self.assertEqual(left, set(), f"{name} left unlowered intrinsics: {left}")
            self.assertIn('target triple = "dxil-ms-dx"', res)
            self.assertNotIn("dxil-ms-xb", res)
            self.assertNotIn("!xbox.compiler.options", res)
            self.assertNotRegex(res, r"@xbox\.")

    def test_all_arithmetic_and_bit_intrinsics(self):
        body = "\n".join([
            "  %1 = call float @xbox.Min3_F32(i32 1, float 1.0, float 2.0, float 3.0)",
            "  %2 = call float @xbox.Max3_F32(i32 2, float %1, float 2.0, float 3.0)",
            "  %3 = call float @xbox.Med3_F32(i32 3, float %1, float %2, float 0.5)",
            "  %4 = call i32 @xbox.Min3_U32(i32 4, i32 10, i32 20, i32 30)",
            "  %5 = call i32 @xbox.Max3_U32(i32 5, i32 %4, i32 20, i32 30)",
            "  %6 = call i32 @xbox.Med3_U32(i32 6, i32 %4, i32 %5, i32 15)",
            "  %7 = call i32 @xbox.BFI(i32 7, i32 %6, i32 1, i32 2)",
            "  %8 = call i32 @xbox.V_ALIGNBYTE_B32(i32 8, i32 %7, i32 %6, i32 2)",
            "  %9 = call i32 @xbox.WaveMin_U32(i32 9, i32 %8)",
            "  %10 = call i64 @xbox.MakeUniform.i64(i32 10, i64 42)",
        ])
        decls = "\n".join([
            "declare float @xbox.Min3_F32(i32, float, float, float) #0",
            "declare float @xbox.Max3_F32(i32, float, float, float) #0",
            "declare float @xbox.Med3_F32(i32, float, float, float) #0",
            "declare i32 @xbox.Min3_U32(i32, i32, i32, i32) #0",
            "declare i32 @xbox.Max3_U32(i32, i32, i32, i32) #0",
            "declare i32 @xbox.Med3_U32(i32, i32, i32, i32) #0",
            "declare i32 @xbox.BFI(i32, i32, i32, i32) #0",
            "declare i32 @xbox.V_ALIGNBYTE_B32(i32, i32, i32, i32) #0",
            "declare i32 @xbox.WaveMin_U32(i32, i32) #0",
            "declare i64 @xbox.MakeUniform.i64(i32, i64) #0",
        ])
        res, n, left = xdl.lower(SAMPLE_BASE.replace("{body}", body).replace("{decls}", decls))
        self.assertEqual(n, 10)
        self.assertEqual(left, set())
        self.assertIn("declare float @dx.op.binary.f32(", res)
        self.assertIn("declare i32 @dx.op.binary.i32(", res)
        self.assertIn("declare i32 @dx.op.waveActiveOp.i32(", res)
        self.assertIn("declare i64 @dx.op.waveReadLaneFirst.i64(", res)
        self.assertNotIn("@xbox.", res)

    def test_lane_swizzle_quad_and_bitmask_modes(self):
        body = "\n".join([
            "  %1 = call i32 @xbox.LaneSwizzle.i32(i32 1, i32 7, i32 30)",
            "  %2 = call i32 @xbox.LaneSwizzle.i32(i32 2, i32 %1, i32 32945)",
        ])
        decls = "declare i32 @xbox.LaneSwizzle.i32(i32, i32, i32) #0\n"
        res, n, left = xdl.lower(SAMPLE_BASE.replace("{body}", body).replace("{decls}", decls))
        self.assertEqual(n, 2)
        self.assertEqual(left, set())
        self.assertEqual(res.count("@dx.op.waveReadLaneAt.i32(i32 117,"), 2)
        self.assertIn("and i32 %xl.1, 30", res)
        self.assertIn("lshr i32 177,", res)

    def test_pair_splitting_and_renumbering(self):
        body = "\n".join([
            "  %1 = insertvalue { half, half } undef, half 0xH3C00, 0",
            "  %2 = insertvalue { half, half } %1, half 0xH4000, 1",
            "  %3 = call { half, half } @xbox.V_PK_MIN_F16(i32 1, { half, half } %2, { half, half } { half 0xH3800, half 0xH4200 })",
            "  %4 = extractvalue { half, half } %3, 0",
            "  %5 = extractvalue { half, half } %3, 1",
            "  %6 = fadd half %4, %5",
        ])
        decls = "declare { half, half } @xbox.V_PK_MIN_F16(i32, { half, half }, { half, half }) #0\n"
        res, n, left = xdl.lower(SAMPLE_BASE.replace("{body}", body).replace("{decls}", decls))
        self.assertEqual(n, 1)
        self.assertEqual(left, set())
        self.assertNotIn("insertvalue { half, half }", res)
        self.assertNotIn("extractvalue { half, half }", res)
        self.assertIn("%1 = fadd half %xl.1, %xl.2", res)

    def test_unlowered_intrinsic_reported(self):
        # A made-up name: it must stay unlowered however many real intrinsics lower() learns (global_load_dword_with_cache
        # used to stand here and is now lowered through the address table).
        body = "  %1 = call i32 @xbox.synthetic_never_lowered(i32 1, i64 0)"
        decls = "declare i32 @xbox.synthetic_never_lowered(i32, i64) #0\n"
        res, n, left = xdl.lower(SAMPLE_BASE.replace("{body}", body).replace("{decls}", decls))
        self.assertEqual(n, 0)
        self.assertEqual(left, {"xbox.synthetic_never_lowered"})
        self.assertIn("declare i32 @xbox.synthetic_never_lowered(", res)

    def test_dxil_sm66_wave_pins_wave_size(self):
        with tempfile.TemporaryDirectory(prefix="dxil_test_") as td:
            src = os.path.join(HERE, "wavelower_kernel.ll")
            lowered = os.path.join(td, "lowered.ll")
            sm66 = os.path.join(td, "sm66.ll")
            with open(src, encoding="utf-8") as f:
                res, _, left = xdl.lower(f.read())
            self.assertEqual(left, set())
            with open(lowered, "w", encoding="utf-8") as f:
                f.write(res)
            subprocess.run(
                [sys.executable, os.path.join(HERE, "dxil_sm66_wave.py"), lowered, sm66, "64"],
                check=True,
                capture_output=True,
                text=True,
            )
            with open(sm66, encoding="utf-8") as f:
                out = f.read()
            self.assertIn('!"cs", i32 6, i32 6', out)
            self.assertIn("!9999 = !{i32 64}", out)
            self.assertIn("@dx.op.createHandleFromBinding(i32 217,", out)
            self.assertIn("@dx.op.annotateHandle(i32 216,", out)
            self.assertNotIn("@dx.op.createHandle(", out)


if __name__ == "__main__":
    unittest.main()
