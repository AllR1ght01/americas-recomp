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


def _waterfall_disjoint_branch_sampler_shader():
    p = rdna_asm.Program()
    p.smem("s_load_dword", 2, 4, 0)
    p.smem("s_load_dwordx8", 16, 4, 16)
    p.smem("s_load_dwordx4", 24, 4, 48)
    p.smem("s_load_dwordx4", 8, 4, 64)
    p.sopp("s_waitcnt", 0)
    for r in (10, 11, 12, 13):
        p.vop1("v_mov_b32", r, 0)
    for group, reg in enumerate((10, 11, 12)):
        for k in range(9):
            p.sopc("s_cmp_eq_u32", "s2", group * 9 + k + 1)
            skip = p.branch_fwd("s_cbranch_scc0")
            p.vop1("v_mov_b32", reg, k + 1)
            p.patch_to_here(skip)
    p.sop1("s_mov_b32", 30, rdna_asm.EXEC_LO)
    p.vopc("v_cmp_gt_u32", 16, "v0")
    p.sop1("s_and_saveexec_b32", 31, rdna_asm.VCC_LO)
    for k in range(4):
        p.vop1("v_mov_b32", 10 + k, f"s{8 + k}")
    p.sop2("s_andn2_b32", rdna_asm.EXEC_LO, "s30", rdna_asm.EXEC_LO)
    for group, reg in enumerate((10, 11, 12)):
        for k in range(9):
            p.sopc("s_cmp_eq_u32", "s2", 40 + group * 9 + k)
            skip = p.branch_fwd("s_cbranch_scc0")
            p.vop1("v_mov_b32", reg, 20 + k)
            p.patch_to_here(skip)
    p.sop2("s_and_b32", rdna_asm.EXEC_LO, "s30", rdna_asm.VCC_LO)
    p.vop1("v_mov_b32", 1, ("lit", 0x3F000000))
    p.vop1("v_mov_b32", 2, ("lit", 0x3F000000))
    p.vop2("v_lshlrev_b32", 5, 2, "v0")
    top = p.label()
    for k in range(4):
        p.vop1("v_readfirstlane_b32", 12 + k, f"v{10 + k}")
    p.vopc("v_cmp_eq_u32", "s12", "v10")
    p.sop1("s_and_saveexec_b32", 1, rdna_asm.VCC_LO)
    p.mimg("image_sample_lz", 4, 1, 16, dmask=1, ssamp=12)
    p.sopp("s_waitcnt", 0)
    p.mubuf("buffer_store_dword", 4, 5, 24, 128, 0)
    p.sop2("s_xor_b32", rdna_asm.EXEC_LO, rdna_asm.EXEC_LO, "s1")
    p.branch_to("s_cbranch_execnz", top)
    p.sopp("s_endpgm")
    return p.bytes()


class ActiveMaskPruningRecompileTest(unittest.TestCase):
    def _compile_and_validate(self, code_bytes, bind_text):
        driver = _find_driver()
        if not driver:
            self.skipTest("driver binary not built (set XBOX_DRIVER or build tools/xbox)")
        with tempfile.TemporaryDirectory(prefix="active_mask_") as tmp:
            code_path = Path(tmp) / "shader.bin"
            bind_path = Path(tmp) / "shader.bind"
            spv_path = Path(tmp) / "out.spv"
            code_path.write_bytes(code_bytes)
            bind_path.write_text(bind_text, encoding="utf-8")
            proc = subprocess.run([driver, str(code_path), "32", str(spv_path), str(bind_path), "1", "cs"], capture_output=True, text=True, timeout=30)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            val = _find_spirv_val()
            if val:
                vp = subprocess.run([val, "--target-env", "vulkan1.3", str(spv_path)], capture_output=True, text=True, timeout=30)
                self.assertEqual(vp.returncode, 0, vp.stdout + vp.stderr)

    def test_waterfall_xor_and_disjoint_branch_prunes_dead_vgpr_history(self):
        bind = "node 0 user 4\ndesc 0 16 img1\ndesc 0 48 buf\ndesc 0 64 samp\n"
        self._compile_and_validate(_waterfall_disjoint_branch_sampler_shader(), bind)


if __name__ == "__main__":
    unittest.main()
