"""Regression tests for driver.cpp mixed V#/T# heap collision handling (TOOL-16)."""

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


def _mixed_heap_compute_shader():
    p = rdna_asm.Program()
    p.smem("s_load_dwordx4", 8, 4, 0)
    p.smem("s_load_dwordx4", 12, 4, 16)
    p.sopp("s_waitcnt", 0)
    p.smem("s_buffer_load_dword", 16, 8, 0)
    p.smem("s_buffer_load_dword", 17, 8, 8)
    p.sopp("s_waitcnt", 0)
    p.sop2("s_lshl_b32", 16, "s16", 5)
    p.sop2("s_lshl_b32", 17, "s17", 5)
    p.sop1("s_mov_b32", 7, 0)
    p.smem("s_load_dwordx4", 20, 6, 0, soffset="s16")
    p.smem("s_load_dwordx8", 24, 6, 0, soffset="s17")
    p.sopp("s_waitcnt", 0)
    p.vop2("v_lshlrev_b32", 1, 2, "v0")
    p.mubuf("buffer_load_dword", 2, 1, 20, 128, 0)
    p.vop1("v_mov_b32", 3, 0)
    p.mimg("image_load", 4, 3, 24, dmask=1, dim=0)
    p.sopp("s_waitcnt", 0)
    p.vop2("v_add_nc_u32", 2, "v2", "v4")
    p.mubuf("buffer_store_dword", 2, 1, 12, 128, 0)
    p.sopp("s_endpgm")
    return p.bytes()


def _mixed_heap_nonzero_cb_guarded_image_shader():
    p = rdna_asm.Program()
    p.smem("s_load_dwordx4", 8, 4, 0)
    p.smem("s_load_dwordx4", 12, 4, 16)
    p.sopp("s_waitcnt", 0)
    p.smem("s_buffer_load_dword", 16, 8, 0x120)
    p.smem("s_buffer_load_dword", 17, 8, 0x130)
    p.sopp("s_waitcnt", 0)
    p.sop2("s_lshl_b32", 16, "s16", 5)
    p.sop1("s_mov_b32", 7, 0)
    p.smem("s_load_dwordx4", 20, 6, 0, soffset="s16")
    p.sopp("s_waitcnt", 0)
    p.vop2("v_lshlrev_b32", 1, 2, "v0")
    p.mubuf("buffer_load_dword", 2, 1, 20, 128, 0)
    p.sopp("s_waitcnt", 0)
    p.sopc("s_cmp_eq_u32", "s17", 0)
    skip_img = p.branch_fwd("s_cbranch_scc1")
    p.sop2("s_lshl_b32", 17, "s17", 5)
    p.smem("s_load_dwordx8", 24, 6, 0, soffset="s17")
    p.sopp("s_waitcnt", 0)
    p.vop1("v_mov_b32", 3, 0)
    p.mimg("image_load", 4, 3, 24, dmask=1, dim=0)
    p.sopp("s_waitcnt", 0)
    p.vop2("v_add_nc_u32", 2, "v2", "v4")
    p.patch_to_here(skip_img)
    p.mubuf("buffer_store_dword", 2, 1, 12, 128, 0)
    p.sopp("s_endpgm")
    return p.bytes()


class MixedHeapDriverTest(unittest.TestCase):
    def _run_driver(self, code_bytes, bind_text):
        driver = _find_driver()
        if not driver:
            self.skipTest("driver binary not built (set XBOX_DRIVER or build tools/xbox)")
        with tempfile.TemporaryDirectory(prefix="tool16_") as tmp:
            code_path = Path(tmp) / "shader.bin"
            bind_path = Path(tmp) / "shader.bind"
            spv_path = Path(tmp) / "out.spv"
            code_path.write_bytes(code_bytes)
            bind_path.write_text(bind_text, encoding="utf-8")
            proc = subprocess.run(
                [driver, str(code_path), "32", str(spv_path), str(bind_path), "1", "cs"],
                capture_output=True,
                text=True,
                timeout=30,
            )
            if proc.returncode == 0:
                val = _find_spirv_val()
                if val:
                    vp = subprocess.run([val, "--target-env", "vulkan1.3", str(spv_path)], capture_output=True, text=True, timeout=30)
                    self.assertEqual(vp.returncode, 0, vp.stdout + vp.stderr)
            out = (proc.stdout + proc.stderr).split(" RecompileRequest:")[0]
            return proc.returncode, out

    def test_colliding_buf_and_img_heap_compiles_valid_spirv(self):
        bind = (
            "node 0 user 4\n"
            "node 1 user 6\n"
            "desc 0 0 buf\n"
            "desc 0 16 buf\n"
            "desc 1 0 buf\n"
            "desc 1 0 img0\n"
        )
        rc, out = self._run_driver(_mixed_heap_compute_shader(), bind)
        self.assertEqual(rc, 0, out)

    def test_colliding_buf_index_at_nonzero_cb_offset_compiles_valid_spirv(self):
        bind = (
            "node 0 user 4\n"
            "node 1 user 6\n"
            "desc 0 0 buf\n"
            "desc 0 16 buf\n"
            "desc 1 0 buf\n"
            "desc 1 0 img0\n"
        )
        rc, out = self._run_driver(_mixed_heap_nonzero_cb_guarded_image_shader(), bind)
        self.assertEqual(rc, 0, out)

    def test_non_colliding_img_read_as_buffer_still_throws(self):
        bind = (
            "node 0 user 4\n"
            "node 1 user 6\n"
            "desc 0 0 buf\n"
            "desc 0 16 buf\n"
            "desc 1 0 img0\n"
        )
        rc, out = self._run_driver(_mixed_heap_compute_shader(), bind)
        self.assertNotEqual(rc, 0, out)
        self.assertIn("buffer descriptor uses an unsupported type", out)


if __name__ == "__main__":
    unittest.main()
