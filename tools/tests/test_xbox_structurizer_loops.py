"""Regression tests for Structurizer non-merge loop latches and exit-less loops (REC-18 / REC-9b)."""

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


def _find_tool(env_name, base_name):
    env = os.environ.get(env_name)
    if env and Path(env).is_file():
        return env
    for name in (base_name, base_name + ".exe"):
        candidate = ROOT / "build-xbox" / name
        if candidate.is_file():
            return str(candidate)
    driver = os.environ.get("XBOX_DRIVER")
    if driver:
        for name in (base_name, base_name + ".exe"):
            sibling = Path(driver).resolve().parent / name
            if sibling.is_file():
                return str(sibling)
    return None


def _find_spirv_val():
    env = os.environ.get("XBOX_SPIRV_VAL")
    if env and Path(env).is_file():
        return env
    return shutil.which("spirv-val")


def _multi_pred_non_merge_latch_shader():
    p = rdna_asm.Program()
    p.smem("s_load_dwordx4", 8, 4, 0)
    p.sopp("s_waitcnt", 0)
    p.sop1("s_mov_b32", 12, 0)
    loop_header = p.label()
    p.sopc("s_cmp_eq_u32", "s12", 1)
    to_latch = p.branch_fwd("s_cbranch_scc1")
    p.sop2("s_add_u32", 12, "s12", 1)
    p.sopc("s_cmp_eq_u32", "s12", "s8")
    to_merge = p.branch_fwd("s_cbranch_scc1")
    p.sop2("s_add_u32", 12, "s12", 2)
    p.patch_to_here(to_latch)
    p.sop2("s_add_u32", 12, "s12", 3)
    p.sopc("s_cmp_lt_u32", "s12", 16)
    p.branch_to("s_cbranch_scc1", loop_header)
    p.sop2("s_add_u32", 12, "s12", 4)
    p.patch_to_here(to_merge)
    p.vop1("v_mov_b32", 1, "s12")
    p.vop2("v_lshlrev_b32", 2, 2, "v0")
    p.mubuf("buffer_store_dword", 1, 2, 8, 128, 0)
    p.sopp("s_endpgm")
    return p.bytes()


def _exitless_infinite_loop_shader():
    p = rdna_asm.Program()
    p.smem("s_load_dwordx4", 8, 4, 0)
    p.sopp("s_waitcnt", 0)
    p.sop1("s_mov_b32", 12, 0)
    p.sopc("s_cmp_eq_u32", "s8", 0)
    to_return = p.branch_fwd("s_cbranch_scc1")
    inf_header = p.label()
    p.sop2("s_add_u32", 12, "s12", 1)
    p.branch_to("s_branch", inf_header)
    p.patch_to_here(to_return)
    p.vop1("v_mov_b32", 1, "s12")
    p.vop2("v_lshlrev_b32", 2, 2, "v0")
    p.mubuf("buffer_store_dword", 1, 2, 8, 128, 0)
    p.sopp("s_endpgm")
    return p.bytes()


class StructurizerLoopsTest(unittest.TestCase):
    def _assert_structured_and_valid(self, code_bytes):
        stages = _find_tool("XBOX_STAGES", "stages")
        driver = _find_tool("XBOX_DRIVER", "driver")
        if not stages or not driver:
            self.skipTest("stages/driver binaries not built (set XBOX_DRIVER/XBOX_STAGES or build tools/xbox)")
        bind = "node 0 user 4\ndesc 0 0 buf\n"
        with tempfile.TemporaryDirectory(prefix="struct_loops_") as tmp:
            code_path = Path(tmp) / "shader.bin"
            bind_path = Path(tmp) / "shader.bind"
            spv_path = Path(tmp) / "out.spv"
            code_path.write_bytes(code_bytes)
            bind_path.write_text(bind, encoding="utf-8")

            env = os.environ.copy()
            env["XBOX_STRUCT_PROF"] = "1"
            st = subprocess.run([stages, str(code_path)], capture_output=True, text=True, timeout=30, env=env)
            self.assertEqual(st.returncode, 0, st.stdout + st.stderr)
            self.assertIn("[struct] ok", st.stderr, st.stderr)

            dr = subprocess.run(
                [driver, str(code_path), "32", str(spv_path), str(bind_path), "1", "cs"],
                capture_output=True,
                text=True,
                timeout=30,
            )
            out = (dr.stdout + dr.stderr).split(" RecompileRequest:")[0]
            self.assertEqual(dr.returncode, 0, out)
            val = _find_spirv_val()
            if val:
                vp = subprocess.run([val, "--target-env", "vulkan1.3", str(spv_path)], capture_output=True, text=True, timeout=30)
                self.assertEqual(vp.returncode, 0, vp.stdout + vp.stderr)

    def test_multi_pred_non_merge_latch_compiles_without_flag_fallback(self):
        self._assert_structured_and_valid(_multi_pred_non_merge_latch_shader())

    def test_exitless_infinite_loop_compiles_without_flag_fallback(self):
        self._assert_structured_and_valid(_exitless_infinite_loop_shader())


if __name__ == "__main__":
    unittest.main()
