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


def _flag_structurized_pointer_high_word_shader(with_loop):
    p = rdna_asm.Program()
    p.sop1("s_mov_b32", 6, "s9")
    p.sop1("s_mov_b32", 5, 0)
    p.smem("s_load_dwordx4", 24, 4, 0)
    p.smem("s_load_dwordx4", 28, 4, 32)
    p.sopp("s_waitcnt", 0)
    p.vop2("v_and_b32", 6, 3, "v0")
    p.vop1("v_readfirstlane_b32", 10, "v6")
    p.sop2("s_lshl_b32", 12, "s10", 2)
    p.smem("s_buffer_load_dword", 10, 24, 0, soffset="s12")
    p.smem("s_buffer_load_dword", 11, 24, 4, soffset="s12")
    p.smem("s_buffer_load_dword", 12, 24, 8, soffset="s12")
    p.sopp("s_waitcnt", 0)
    p.sopc("s_cmp_eq_u32", "s10", 0)
    to_end1 = p.branch_fwd("s_cbranch_scc1")
    p.sop1("s_mov_b32", 7, 0)
    if with_loop:
        p.sop1("s_mov_b32", 2, 0)
        p.sopc("s_cmp_eq_u32", "s11", 0)
        to_mid = p.branch_fwd("s_cbranch_scc1")
        loop_top = p.label()
        p.sop2("s_add_u32", 2, "s2", 1)
        p.sopc("s_cmp_eq_u32", "s2", "s12")
        to_end2 = p.branch_fwd("s_cbranch_scc1")
        p.patch_to_here(to_mid)
        p.sop2("s_add_u32", 2, "s2", 2)
        p.sopc("s_cmp_lt_u32", "s2", 8)
        p.branch_to("s_cbranch_scc1", loop_top)
    else:
        p.sopc("s_cmp_eq_u32", "s11", 0)
        to_end2 = p.branch_fwd("s_cbranch_scc1")
    p.sopc("s_cmp_eq_u32", "s12", 0)
    to_end3 = p.branch_fwd("s_cbranch_scc1")
    p.sop1("s_mov_b32", 0, "s11")
    p.sopc("s_cmp_eq_u32", "s12", 2)
    skip_idx = p.branch_fwd("s_cbranch_scc1")
    p.sop1("s_mov_b32", 0, "s10")
    p.patch_to_here(skip_idx)
    p.sop2("s_lshl_b32", 0, "s0", 5)
    p.smem("s_load_dwordx8", 16, 6, 0, soffset="s0")
    p.sopp("s_waitcnt", 0)
    p.vop1("v_mov_b32", 1, "v0")
    p.vop1("v_mov_b32", 2, 0)
    p.mimg("image_load", 4, 1, 16, dmask=1)
    p.sopp("s_waitcnt", 0)
    p.vop2("v_lshlrev_b32", 5, 2, "v0")
    p.mubuf("buffer_store_dword", 4, 5, 28, 128, 0)
    to_done = p.branch_fwd("s_branch")
    p.patch_to_here(to_end3)
    p.vop1("v_mov_b32", 4, 3)
    p.vop2("v_lshlrev_b32", 5, 2, "v0")
    p.mubuf("buffer_store_dword", 4, 5, 28, 128, 0)
    to_done2 = p.branch_fwd("s_branch")
    p.patch_to_here(to_end2)
    p.vop1("v_mov_b32", 4, 2)
    p.vop2("v_lshlrev_b32", 5, 2, "v0")
    p.mubuf("buffer_store_dword", 4, 5, 28, 128, 0)
    p.patch_to_here(to_end1)
    p.patch_to_here(to_done)
    p.patch_to_here(to_done2)
    p.sopp("s_endpgm")
    return p.bytes()


class GotoPhiRefinerRecompileTest(unittest.TestCase):
    def _compile_and_validate(self, code_bytes, bind_text, extra_env=None):
        driver = _find_driver()
        if not driver:
            self.skipTest("driver binary not built (set XBOX_DRIVER or build tools/xbox)")
        env = dict(os.environ)
        if extra_env:
            env.update(extra_env)
        with tempfile.TemporaryDirectory(prefix="goto_phi_") as tmp:
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
                env=env,
            )
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            val = _find_spirv_val()
            if val:
                vp = subprocess.run([val, "--target-env", "vulkan1.3", str(spv_path)], capture_output=True, text=True, timeout=30)
                self.assertEqual(vp.returncode, 0, vp.stdout + vp.stderr)

    def test_irreducible_loop_flag_structurization_refines_pointer_high_word(self):
        bind = "node 0 user 4\nnode 1 user 9\ndesc 0 0 buf\ndesc 0 32 buf\ndesc 1 0 img1\n"
        self._compile_and_validate(_flag_structurized_pointer_high_word_shader(True), bind)

    def test_chained_flag_structurization_refines_pointer_high_word(self):
        bind = "node 0 user 4\nnode 1 user 9\ndesc 0 0 buf\ndesc 0 32 buf\ndesc 1 0 img1\n"
        self._compile_and_validate(
            _flag_structurized_pointer_high_word_shader(False),
            bind,
            extra_env={"APS5_FORCE_STRUCTURIZER_FALLBACK": "1"},
        )


if __name__ == "__main__":
    unittest.main()
