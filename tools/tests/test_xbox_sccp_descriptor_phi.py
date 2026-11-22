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


def _irreducible_loop_srt_descriptor_shader():
    p = rdna_asm.Program()
    p.smem("s_load_dwordx4", 8, 4, 0)
    p.smem("s_load_dwordx4", 24, 4, 32)
    p.sopp("s_waitcnt", 0)
    p.vop2("v_and_b32", 6, 3, "v0")
    p.vop1("v_readfirstlane_b32", 12, "v6")
    p.sop2("s_lshl_b32", 12, "s12", 2)
    p.smem("s_buffer_load_dword", 2, 8, 0, soffset="s12")
    p.smem("s_buffer_load_dword", 3, 8, 4, soffset="s12")
    p.smem("s_buffer_load_dword", 6, 8, 8, soffset="s12")
    p.sopp("s_waitcnt", 0)
    p.sop1("s_mov_b32", 16, rdna_asm.EXEC_LO)
    for r in range(17, 24):
        p.sop1("s_mov_b32", r, 0)
    p.sop1("s_mov_b32", 7, 0)
    p.sopc("s_cmp_eq_u32", "s2", 0)
    to_end1 = p.branch_fwd("s_cbranch_scc1")
    p.smem("s_load_dwordx8", 16, 4, 64)
    p.sopp("s_waitcnt", 0)
    p.sopc("s_cmp_eq_u32", "s3", 0)
    to_mid = p.branch_fwd("s_cbranch_scc1")
    loop_top = p.label()
    p.sop2("s_add_u32", 7, "s7", 1)
    p.sopc("s_cmp_eq_u32", "s7", "s6")
    to_end2 = p.branch_fwd("s_cbranch_scc1")
    p.patch_to_here(to_mid)
    p.sop2("s_add_u32", 7, "s7", 2)
    p.sopc("s_cmp_lt_u32", "s7", 8)
    p.branch_to("s_cbranch_scc1", loop_top)
    p.vop1("v_mov_b32", 1, "v0")
    p.vop1("v_mov_b32", 2, 0)
    p.vop1("v_mov_b32", 4, 1)
    p.mimg("image_atomic_umax", 4, 1, 16, dmask=1)
    p.sopp("s_waitcnt", 0)
    p.vop2("v_lshlrev_b32", 5, 2, "v0")
    p.mubuf("buffer_store_dword", 4, 5, 24, 128, 0)
    to_done1 = p.branch_fwd("s_branch")
    p.patch_to_here(to_end2)
    p.vop1("v_mov_b32", 4, 9)
    p.vop2("v_lshlrev_b32", 5, 2, "v0")
    p.mubuf("buffer_store_dword", 4, 5, 24, 128, 0)
    to_done2 = p.branch_fwd("s_branch")
    p.patch_to_here(to_end1)
    p.vop1("v_mov_b32", 4, 7)
    p.vop2("v_lshlrev_b32", 5, 2, "v0")
    p.mubuf("buffer_store_dword", 4, 5, 24, 128, 0)
    p.patch_to_here(to_done1)
    p.patch_to_here(to_done2)
    p.sopp("s_endpgm")
    return p.bytes()


def _reachable_garbage_descriptor_shader():
    p = rdna_asm.Program()
    p.smem("s_load_dwordx4", 8, 4, 0)
    p.smem("s_load_dwordx4", 24, 4, 32)
    p.sopp("s_waitcnt", 0)
    p.vop2("v_and_b32", 6, 3, "v0")
    p.vop1("v_readfirstlane_b32", 12, "v6")
    p.sop2("s_lshl_b32", 12, "s12", 2)
    p.smem("s_buffer_load_dword", 2, 8, 0, soffset="s12")
    p.sopp("s_waitcnt", 0)
    p.sop1("s_mov_b32", 16, rdna_asm.EXEC_LO)
    for r in range(17, 24):
        p.sop1("s_mov_b32", r, 0)
    p.sopc("s_cmp_eq_u32", "s2", 0)
    skip_load = p.branch_fwd("s_cbranch_scc1")
    p.smem("s_load_dwordx8", 16, 4, 64)
    p.sopp("s_waitcnt", 0)
    p.patch_to_here(skip_load)
    p.vop1("v_mov_b32", 1, "v0")
    p.vop1("v_mov_b32", 2, 0)
    p.vop1("v_mov_b32", 4, 1)
    p.mimg("image_atomic_umax", 4, 1, 16, dmask=1)
    p.sopp("s_waitcnt", 0)
    p.vop2("v_lshlrev_b32", 5, 2, "v0")
    p.mubuf("buffer_store_dword", 4, 5, 24, 128, 0)
    p.sopp("s_endpgm")
    return p.bytes()


_BIND = (
    "node 0 user 4\n"
    "desc 0 0 buf\n"
    "desc 0 32 buf\n"
    "desc 0 64 img1a\n"
)


class SccpDescriptorPhiPruningTest(unittest.TestCase):
    def _run_driver(self, code_bytes):
        driver = _find_driver()
        if not driver:
            self.skipTest("driver binary not built (set XBOX_DRIVER or build tools/xbox)")
        with tempfile.TemporaryDirectory(prefix="sccp_desc_phi_") as tmp:
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

    def test_irreducible_loop_non_dominating_srt_descriptor(self):
        rc, out, val_ok, val_err = self._run_driver(_irreducible_loop_srt_descriptor_shader())
        self.assertEqual(rc, 0, out)
        self.assertTrue(val_ok, val_err)

    def test_reachable_garbage_descriptor_rejected(self):
        rc, _, _, _ = self._run_driver(_reachable_garbage_descriptor_shader())
        self.assertNotEqual(rc, 0)


if __name__ == "__main__":
    unittest.main()
