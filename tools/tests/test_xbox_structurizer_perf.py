import os
import re
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


def _shared_merge_chain_shader():
    p = rdna_asm.Program()
    p.smem("s_load_dwordx4", 8, 4, 0)
    p.sopp("s_waitcnt", 0)
    p.sop1("s_mov_b32", 12, 0)
    exits = []
    for k in range(6):
        p.sopc("s_cmp_eq_u32", "s8", k)
        to_a = p.branch_fwd("s_cbranch_scc1")
        p.sop2("s_add_u32", 12, "s12", k + 1)
        p.sopc("s_cmp_eq_u32", "s9", k)
        exits.append(p.branch_fwd("s_cbranch_scc1"))
        to_join = p.branch_fwd("s_branch")
        p.patch_to_here(to_a)
        p.sop2("s_add_u32", 12, "s12", k + 10)
        p.patch_to_here(to_join)
    loop_top = p.label()
    p.sop2("s_add_u32", 12, "s12", 1)
    p.sopc("s_cmp_lt_u32", "s12", "s10")
    p.branch_to("s_cbranch_scc1", loop_top)
    for ex in exits:
        p.patch_to_here(ex)
    p.vop1("v_mov_b32", 1, "s12")
    p.vop2("v_lshlrev_b32", 2, 2, "v0")
    p.mubuf("buffer_store_dword", 1, 2, 8, 128, 0)
    p.sopp("s_endpgm")
    return p.bytes()


def _irreducible_and_infinite_loop_shader():
    p = rdna_asm.Program()
    p.smem("s_load_dwordx4", 8, 4, 0)
    p.sopp("s_waitcnt", 0)
    p.sopc("s_cmp_eq_u32", "s8", 0)
    to_inf = p.branch_fwd("s_cbranch_scc1")
    p.sopc("s_cmp_eq_u32", "s9", 0)
    to_mid = p.branch_fwd("s_cbranch_scc1")
    top = p.label()
    p.sop2("s_add_u32", 10, "s10", 1)
    p.patch_to_here(to_mid)
    p.sop2("s_add_u32", 10, "s10", 2)
    p.sopc("s_cmp_lt_u32", "s10", 16)
    p.branch_to("s_cbranch_scc1", top)
    p.sopp("s_endpgm")
    p.patch_to_here(to_inf)
    inf_loop = p.label()
    p.sop2("s_add_u32", 11, "s11", 1)
    p.branch_to("s_branch", inf_loop)
    return p.bytes()


class StructurizerTreeIndexEquivalenceTest(unittest.TestCase):
    def _run_stages(self, code_bytes, extra_env=None):
        stages = _find_tool("XBOX_STAGES", "stages")
        if not stages:
            self.skipTest("stages binary not built (set XBOX_STAGES or build tools/xbox)")
        env = os.environ.copy()
        if extra_env:
            env.update(extra_env)
        with tempfile.TemporaryDirectory(prefix="struct_eq_") as tmp:
            code_path = Path(tmp) / "shader.bin"
            code_path.write_bytes(code_bytes)
            proc = subprocess.run([stages, str(code_path)], capture_output=True, text=True, timeout=30, env=env)
            return proc.returncode, proc.stdout, proc.stderr

    def test_shared_merge_chain_hash_matches_fallback(self):
        code = _shared_merge_chain_shader()
        rc_fast, out_fast, err_fast = self._run_stages(code)
        rc_ref, out_ref, err_ref = self._run_stages(code, {"APS5_NO_PERF_DOMINATOR_TREE": "1"})
        self.assertEqual(rc_fast, 0, err_fast)
        self.assertEqual(rc_ref, 0, err_ref)
        m_fast = re.search(r"blocks=(\d+) hash=([0-9a-f]+)", out_fast)
        m_ref = re.search(r"blocks=(\d+) hash=([0-9a-f]+)", out_ref)
        self.assertIsNotNone(m_fast, out_fast)
        self.assertIsNotNone(m_ref, out_ref)
        self.assertEqual(m_fast.groups(), m_ref.groups())

    def test_irreducible_and_infinite_loop_matches_fallback(self):
        code = _irreducible_and_infinite_loop_shader()
        rc_fast, out_fast, err_fast = self._run_stages(code)
        rc_ref, out_ref, err_ref = self._run_stages(code, {"APS5_NO_PERF_DOMINATOR_TREE": "1"})
        self.assertEqual(rc_fast, rc_ref, (err_fast, err_ref))
        if rc_fast == 0:
            m_fast = re.search(r"blocks=(\d+) hash=([0-9a-f]+)", out_fast)
            m_ref = re.search(r"blocks=(\d+) hash=([0-9a-f]+)", out_ref)
            self.assertIsNotNone(m_fast, out_fast)
            self.assertIsNotNone(m_ref, out_ref)
            self.assertEqual(m_fast.groups(), m_ref.groups())


if __name__ == "__main__":
    unittest.main()
