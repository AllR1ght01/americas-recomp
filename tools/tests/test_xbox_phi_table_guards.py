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


def _two_live_arms_nested_dead_arm_shader():
    p = rdna_asm.Program()
    p.smem("s_load_dwordx4", 8, 4, 0)
    p.smem("s_load_dwordx2", 12, 4, 16)
    p.smem("s_load_dwordx4", 24, 4, 32)
    p.smem("s_load_dword", 2, 4, 48)
    p.smem("s_load_dword", 3, 4, 52)
    p.sopp("s_waitcnt", 0)
    p.smem("s_buffer_load_dword", 29, 8, 0)
    p.smem("s_buffer_load_dword", 30, 8, 4)
    p.sopp("s_waitcnt", 0)
    p.sop1("s_mov_b32", 16, rdna_asm.EXEC_LO)
    for r in range(17, 24):
        p.sop1("s_mov_b32", r, 0)
    p.sop1("s_mov_b32", 10, 0)
    p.sopc("s_cmp_eq_u32", "s2", 0)
    skip_first = p.branch_fwd("s_cbranch_scc1")
    p.sop2("s_lshl_b32", 28, "s29", 5)
    p.smem("s_load_dwordx8", 16, 12, 0, soffset="s28")
    p.sop1("s_mov_b32", 10, 1)
    p.patch_to_here(skip_first)
    p.sopc("s_cmp_eq_u32", "s3", 0)
    skip_second = p.branch_fwd("s_cbranch_scc1")
    p.sop2("s_lshl_b32", 28, "s30", 5)
    p.smem("s_load_dwordx8", 16, 12, 0, soffset="s28")
    p.sop1("s_mov_b32", 10, 1)
    p.patch_to_here(skip_second)
    p.sopc("s_cmp_eq_u32", "s10", 0)
    skip_use = p.branch_fwd("s_cbranch_scc1")
    p.sopp("s_waitcnt", 0)
    p.vop1("v_mov_b32", 1, "v0")
    p.vop1("v_mov_b32", 2, 0)
    p.mimg("image_load", 4, 1, 16, dmask=1)
    p.sopp("s_waitcnt", 0)
    p.vop2("v_lshlrev_b32", 5, 2, "v0")
    p.mubuf("buffer_store_dword", 4, 5, 24, 128, 0)
    p.patch_to_here(skip_use)
    p.sopp("s_endpgm")
    return p.bytes()


def _transitive_guard_non_dominating_load_shader():
    p = rdna_asm.Program()
    p.smem("s_load_dwordx4", 8, 4, 0)
    p.smem("s_load_dwordx2", 12, 4, 16)
    p.smem("s_load_dwordx4", 24, 4, 32)
    p.smem("s_load_dword", 2, 4, 48)
    p.smem("s_load_dword", 3, 4, 52)
    p.sopp("s_waitcnt", 0)
    p.smem("s_buffer_load_dword", 29, 8, 0)
    p.sopp("s_waitcnt", 0)
    p.sop1("s_mov_b32", 16, rdna_asm.EXEC_LO)
    for r in range(17, 24):
        p.sop1("s_mov_b32", r, 0)
    p.sop1("s_mov_b32", 10, 0)
    p.sop1("s_mov_b32", 11, 0)
    p.sopc("s_cmp_eq_u32", "s2", 0)
    skip_load = p.branch_fwd("s_cbranch_scc1")
    p.sop2("s_lshl_b32", 28, "s29", 5)
    p.smem("s_load_dwordx8", 16, 12, 0, soffset="s28")
    p.sopc("s_cmp_eq_u32", "s3", 0)
    use_second_flag = p.branch_fwd("s_cbranch_scc1")
    p.sop1("s_mov_b32", 10, 1)
    p.sop1("s_mov_b32", 11, 0)
    after_flags = p.branch_fwd("s_branch")
    p.patch_to_here(use_second_flag)
    p.sop1("s_mov_b32", 10, 0)
    p.sop1("s_mov_b32", 11, 1)
    p.patch_to_here(after_flags)
    p.patch_to_here(skip_load)
    p.sopc("s_cmp_eq_u32", "s10", 0)
    skip_first_use = p.branch_fwd("s_cbranch_scc1")
    p.sopp("s_waitcnt", 0)
    p.vop1("v_mov_b32", 1, "v0")
    p.vop1("v_mov_b32", 2, 0)
    p.mimg("image_load", 4, 1, 16, dmask=1)
    p.sopp("s_waitcnt", 0)
    p.vop2("v_lshlrev_b32", 5, 2, "v0")
    p.mubuf("buffer_store_dword", 4, 5, 24, 128, 0)
    p.sop1("s_mov_b32", 11, 1)
    p.patch_to_here(skip_first_use)
    p.sopc("s_cmp_eq_u32", "s11", 0)
    skip_second_use = p.branch_fwd("s_cbranch_scc1")
    p.sopp("s_waitcnt", 0)
    p.vop1("v_mov_b32", 1, "v0")
    p.vop1("v_mov_b32", 2, 1)
    p.mimg("image_load", 4, 1, 16, dmask=1)
    p.sopp("s_waitcnt", 0)
    p.vop2("v_lshlrev_b32", 5, 2, "v0")
    p.mubuf("buffer_store_dword", 4, 5, 24, 128, 0)
    p.patch_to_here(skip_second_use)
    p.sopp("s_endpgm")
    return p.bytes()


def _flag_structurized_two_live_arms_shader():
    p = rdna_asm.Program()
    p.smem("s_load_dwordx4", 8, 4, 0)
    p.smem("s_load_dwordx2", 12, 4, 16)
    p.smem("s_load_dwordx4", 24, 4, 32)
    p.smem("s_load_dword", 2, 4, 48)
    p.smem("s_load_dword", 3, 4, 52)
    p.sopp("s_waitcnt", 0)
    p.smem("s_buffer_load_dword", 29, 8, 0)
    p.smem("s_buffer_load_dword", 30, 8, 4)
    p.sopp("s_waitcnt", 0)
    p.sop1("s_mov_b32", 16, rdna_asm.EXEC_LO)
    for r in range(17, 24):
        p.sop1("s_mov_b32", r, 0)
    p.sopc("s_cmp_eq_u32", "s2", 0)
    try_second = p.branch_fwd("s_cbranch_scc1")
    p.sop2("s_lshl_b32", 28, "s29", 5)
    p.smem("s_load_dwordx8", 16, 12, 0, soffset="s28")
    to_use = p.branch_fwd("s_branch")
    p.patch_to_here(try_second)
    p.sopc("s_cmp_eq_u32", "s3", 0)
    to_alt = p.branch_fwd("s_cbranch_scc1")
    p.sop2("s_lshl_b32", 28, "s30", 5)
    p.smem("s_load_dwordx8", 16, 12, 0, soffset="s28")
    p.patch_to_here(to_use)
    p.sopp("s_waitcnt", 0)
    p.vop1("v_mov_b32", 1, "v0")
    p.vop1("v_mov_b32", 2, 0)
    p.mimg("image_load", 4, 1, 16, dmask=1)
    p.sopp("s_waitcnt", 0)
    p.vop2("v_lshlrev_b32", 5, 2, "v0")
    p.mubuf("buffer_store_dword", 4, 5, 24, 128, 0)
    to_done = p.branch_fwd("s_branch")
    p.patch_to_here(to_alt)
    p.vop1("v_mov_b32", 4, 7)
    p.vop2("v_lshlrev_b32", 5, 2, "v0")
    p.mubuf("buffer_store_dword", 4, 5, 24, 128, 0)
    p.patch_to_here(to_done)
    p.sopp("s_endpgm")
    return p.bytes()


def _live_garbage_arm_shader():
    p = rdna_asm.Program()
    p.smem("s_load_dwordx4", 8, 4, 0)
    p.smem("s_load_dwordx2", 12, 4, 16)
    p.smem("s_load_dwordx4", 24, 4, 32)
    p.smem("s_load_dword", 2, 4, 48)
    p.sopp("s_waitcnt", 0)
    p.smem("s_buffer_load_dword", 29, 8, 0)
    p.sopp("s_waitcnt", 0)
    p.sop1("s_mov_b32", 16, rdna_asm.EXEC_LO)
    for r in range(17, 24):
        p.sop1("s_mov_b32", r, 0)
    p.sop1("s_mov_b32", 10, 1)
    p.sopc("s_cmp_eq_u32", "s2", 0)
    skip_load = p.branch_fwd("s_cbranch_scc1")
    p.sop2("s_lshl_b32", 28, "s29", 5)
    p.smem("s_load_dwordx8", 16, 12, 0, soffset="s28")
    p.patch_to_here(skip_load)
    p.sopc("s_cmp_eq_u32", "s10", 0)
    skip_use = p.branch_fwd("s_cbranch_scc1")
    p.sopp("s_waitcnt", 0)
    p.vop1("v_mov_b32", 1, "v0")
    p.vop1("v_mov_b32", 2, 0)
    p.mimg("image_load", 4, 1, 16, dmask=1)
    p.sopp("s_waitcnt", 0)
    p.vop2("v_lshlrev_b32", 5, 2, "v0")
    p.mubuf("buffer_store_dword", 4, 5, 24, 128, 0)
    p.patch_to_here(skip_use)
    p.sopp("s_endpgm")
    return p.bytes()


_BIND = (
    "node 0 user 4\n"
    "node 1 child 0 16\n"
    "desc 0 0 buf\n"
    "desc 0 32 buf\n"
    "desc 1 0 img1\n"
    "desc 1 32 img1\n"
)


class PhiTableGuardsRecompileTest(unittest.TestCase):
    def _run_driver(self, code_bytes):
        driver = _find_driver()
        if not driver:
            self.skipTest("driver binary not built (set XBOX_DRIVER or build tools/xbox)")
        with tempfile.TemporaryDirectory(prefix="phi_table_guards_") as tmp:
            code_path = Path(tmp) / "shader.bin"
            bind_path = Path(tmp) / "shader.bind"
            spv_path = Path(tmp) / "out.spv"
            code_path.write_bytes(code_bytes)
            bind_path.write_text(_BIND, encoding="utf-8")
            proc = subprocess.run([driver, str(code_path), "32", str(spv_path), str(bind_path), "1", "cs"], capture_output=True, text=True, timeout=30)
            val_ok = True
            val_err = ""
            if proc.returncode == 0:
                val = _find_spirv_val()
                if val:
                    vp = subprocess.run([val, "--target-env", "vulkan1.3", str(spv_path)], capture_output=True, text=True, timeout=30)
                    val_ok = vp.returncode == 0
                    val_err = vp.stdout + vp.stderr
            return proc.returncode, proc.stdout + proc.stderr, val_ok, val_err

    def test_guarded_two_live_arms_with_nested_dead_arm(self):
        rc, out, val_ok, val_err = self._run_driver(_two_live_arms_nested_dead_arm_shader())
        self.assertEqual(rc, 0, out)
        self.assertTrue(val_ok, val_err)

    def test_guarded_single_live_arm_transitive_non_dominating_load(self):
        rc, out, val_ok, val_err = self._run_driver(_transitive_guard_non_dominating_load_shader())
        self.assertEqual(rc, 0, out)
        self.assertTrue(val_ok, val_err)

    def test_flag_structurized_two_live_arms_with_dead_arm(self):
        rc, out, val_ok, val_err = self._run_driver(_flag_structurized_two_live_arms_shader())
        self.assertEqual(rc, 0, out)
        self.assertTrue(val_ok, val_err)

    def test_live_garbage_arm_rejected(self):
        rc, _, _, _ = self._run_driver(_live_garbage_arm_shader())
        self.assertNotEqual(rc, 0)


if __name__ == "__main__":
    unittest.main()
