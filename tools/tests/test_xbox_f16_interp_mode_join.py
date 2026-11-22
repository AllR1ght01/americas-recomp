import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "xbox"))
os.environ.setdefault("XBOX_RDNA2", "recomp")
import rdna_asm


def _find_driver():
    env = os.environ.get("XBOX_DRIVER")
    if env and Path(env).is_file():
        return env
    for name in ("driver", "driver.exe"):
        candidate = ROOT / "build-xbox" / name
        if candidate.is_file():
            return str(candidate)
    return None


def _find_spirv_val():
    env = os.environ.get("XBOX_SPIRV_VAL")
    if env and Path(env).is_file():
        return env
    return shutil.which("spirv-val")


def _emit_setreg_mode(p, offset, size, literal):
    simm16 = 1 | (offset << 6) | ((size - 1) << 11)
    p.words += [0xBA800000 | simm16, literal]


def _f16_interp_after_mode_join_shader(mode_offset, mode_size, mode_value):
    p = rdna_asm.Program()
    p.smem("s_load_dword", 2, 4, 0)
    p.sopp("s_waitcnt", 0)
    p.sopc("s_cmp_eq_u32", "s2", 0)
    skip = p.branch_fwd("s_cbranch_scc1")
    _emit_setreg_mode(p, mode_offset, mode_size, mode_value)
    p.patch_to_here(skip)
    p.words += [0xD7420004, (256 << 9) | 0]
    p.words += [0xD75A0005, (260 << 18) | (257 << 9) | 0]
    p.vop1("v_cvt_f32_f16", 6, "v5")
    p.vop1("v_mov_b32", 7, "v6")
    p.vop1("v_mov_b32", 8, "v6")
    p.vop1("v_mov_b32", 9, "v6")
    p.words += [0xF800080F, (9 << 24) | (8 << 16) | (7 << 8) | 6]
    p.sopp("s_endpgm")
    return p.bytes()


class F16InterpModeJoinTest(unittest.TestCase):
    def _run_driver(self, code_bytes, relaxed):
        driver = _find_driver()
        if not driver:
            self.skipTest("driver binary not built (set XBOX_DRIVER or build tools/xbox)")
        env = os.environ.copy()
        env["XBOX_FRAGMENT_BARY"] = "1"
        if relaxed:
            env["XBOX_ALLOW_UNMEASURED_F16_INTERP"] = "1"
        else:
            env.pop("XBOX_ALLOW_UNMEASURED_F16_INTERP", None)
        with tempfile.TemporaryDirectory(prefix="f16_mode_join_") as tmp:
            code_path = Path(tmp) / "shader.bin"
            bind_path = Path(tmp) / "shader.bind"
            spv_path = Path(tmp) / "out.spv"
            code_path.write_bytes(code_bytes)
            bind_path.write_text("node 0 user 4\n", encoding="utf-8")
            proc = subprocess.run(
                [driver, str(code_path), "32", str(spv_path), str(bind_path), "1", "ps", "2", "1", "1"],
                capture_output=True,
                text=True,
                timeout=30,
                env=env,
            )
            val_ok = True
            val_err = ""
            if proc.returncode == 0:
                val = _find_spirv_val()
                if val:
                    vp = subprocess.run([val, "--target-env", "vulkan1.3", str(spv_path)], capture_output=True, text=True, timeout=30)
                    val_ok = vp.returncode == 0
                    val_err = vp.stdout + vp.stderr
            return proc.returncode, proc.stdout + proc.stderr, val_ok, val_err

    def test_rounding_only_join_allowed_when_relaxed_and_refused_when_strict(self):
        code = _f16_interp_after_mode_join_shader(2, 2, 3)
        rc_strict, out_strict, _, _ = self._run_driver(code, relaxed=False)
        self.assertNotEqual(rc_strict, 0, out_strict)
        rc_relaxed, out_relaxed, val_ok, val_err = self._run_driver(code, relaxed=True)
        self.assertEqual(rc_relaxed, 0, out_relaxed)
        self.assertTrue(val_ok, val_err)

    def test_denorm_join_still_refused_when_relaxed(self):
        code = _f16_interp_after_mode_join_shader(4, 2, 3)
        rc_relaxed, _, _, _ = self._run_driver(code, relaxed=True)
        self.assertNotEqual(rc_relaxed, 0)


if __name__ == "__main__":
    unittest.main()
