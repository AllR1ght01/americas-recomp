import os
import shutil
import struct
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


def _nested_pointer_phi_image_shader():
    p = rdna_asm.Program()
    p.smem("s_load_dword", 2, 4, 0)
    p.smem("s_load_dword", 3, 4, 4)
    p.smem("s_load_dwordx4", 24, 4, 32)
    p.smem("s_load_dwordx2", 10, 4, 8)
    p.sopp("s_waitcnt", 0)
    p.sopc("s_cmp_eq_u32", "s2", 0)
    skip_mid = p.branch_fwd("s_cbranch_scc1")
    p.smem("s_load_dwordx2", 10, 4, 16)
    p.patch_to_here(skip_mid)
    p.sopc("s_cmp_eq_u32", "s3", 0)
    skip_hi = p.branch_fwd("s_cbranch_scc1")
    p.smem("s_load_dwordx2", 10, 4, 24)
    p.patch_to_here(skip_hi)
    p.sopp("s_waitcnt", 0)
    p.smem("s_load_dwordx8", 16, 10, 0)
    p.sopp("s_waitcnt", 0)
    p.vop1("v_mov_b32", 1, "v0")
    p.vop1("v_mov_b32", 2, 0)
    p.mimg("image_load", 4, 1, 16, dmask=1)
    p.sopp("s_waitcnt", 0)
    p.vop2("v_lshlrev_b32", 5, 2, "v0")
    p.mubuf("buffer_store_dword", 4, 5, 24, 128, 0)
    p.sopp("s_endpgm")
    return p.bytes()


def _mixed_readfirstlane_and_scalar_sampler_shader():
    p = rdna_asm.Program()
    p.smem("s_load_dword", 2, 4, 0)
    p.smem("s_load_dword", 3, 4, 4)
    p.smem("s_load_dwordx8", 16, 4, 16)
    p.smem("s_load_dwordx4", 24, 4, 48)
    p.smem("s_load_dwordx4", 8, 4, 64)
    p.sopp("s_waitcnt", 0)
    p.sopc("s_cmp_eq_u32", "s2", 0)
    to_a = p.branch_fwd("s_cbranch_scc1")
    p.smem("s_load_dwordx4", 8, 4, 80)
    p.sopp("s_waitcnt", 0)
    p.patch_to_here(to_a)
    p.sopc("s_cmp_eq_u32", "s3", 0)
    to_b = p.branch_fwd("s_cbranch_scc1")
    p.smem("s_load_dwordx4", 8, 4, 96)
    p.sopp("s_waitcnt", 0)
    p.patch_to_here(to_b)
    p.vopc("v_cmpx_gt_u32", 16, "v0")
    for k in range(3):
        p.vop1("v_mov_b32", 10 + k, f"s{8 + k}")
    p.sop1("s_mov_b32", 15, "s11")
    for k in range(3):
        p.vop1("v_readfirstlane_b32", 12 + k, f"v{10 + k}")
    p.vop1("v_mov_b32", 1, ("lit", 0x3FA00000))
    p.vop1("v_mov_b32", 2, ("lit", 0x3F000000))
    p.mimg("image_sample_lz", 4, 1, 16, dmask=1, ssamp=12)
    p.sopp("s_waitcnt", 0)
    p.vop2("v_lshlrev_b32", 5, 2, "v0")
    p.mubuf("buffer_store_dword", 4, 5, 24, 128, 0)
    p.sopp("s_endpgm")
    return p.bytes()


def _readfirstlane_active_mask_pruning_shader():
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
    p.vopc("v_cmpx_gt_u32", 16, "v0")
    for k in range(4):
        p.vop1("v_mov_b32", 10 + k, f"s{8 + k}")
    p.sopc("s_cmp_eq_u32", "s2", 0)
    skip2 = p.branch_fwd("s_cbranch_scc1")
    p.smem("s_load_dwordx4", 8, 4, 80)
    p.sopp("s_waitcnt", 0)
    for k in range(4):
        p.vop1("v_mov_b32", 10 + k, f"s{8 + k}")
    p.patch_to_here(skip2)
    p.vopc("v_cmpx_gt_u32", 8, "v0")
    for k in range(4):
        p.vop1("v_readfirstlane_b32", 12 + k, f"v{10 + k}")
    p.vop1("v_mov_b32", 1, ("lit", 0x3F000000))
    p.vop1("v_mov_b32", 2, ("lit", 0x3F000000))
    p.mimg("image_sample_lz", 4, 1, 16, dmask=1, ssamp=12)
    p.sopp("s_waitcnt", 0)
    p.vop2("v_lshlrev_b32", 5, 2, "v0")
    p.mubuf("buffer_store_dword", 4, 5, 24, 128, 0)
    p.sopp("s_endpgm")
    return p.bytes()


def _split_reloads_shader(joins):
    """ShaderMemory.cpp's splitReloads: each join reloads T# dword 7 from one of two SRT dwords or dwords 0-6 from the same SRT dwords."""
    words = [0xF4080500, 0xFA000020, 0xF4080600, 0xFA000040, 0xF40C0200, 0xFA000000, 0xBF8CC07F]
    for join in range(joins):
        words += [0xBF068014, 0xBF850003, 0xF40003C0, 0xFA000088 if join % 2 == 0 else 0xFA00008C, 0xBF820006, 0xF4080200, 0xFA000060,
                  0xF4040300, 0xFA000070, 0xF4000380, 0xFA000078]
    words += [0xBF8CC07F, 0xF09C0F08, 0x00A20000, 0xBF8C3F70, 0xE0700000, 0x80060000, 0xBF810000]
    return struct.pack("<%dI" % len(words), *words)


SPLIT_RELOADS_BIND = "node 0 user 0\ndesc 0 0 img1\ndesc 0 32 buf\ndesc 0 64 buf\ndesc 0 96 img1\n"


class DescriptorWebRecompileTest(unittest.TestCase):
    def _run_driver(self, code_bytes, bind_text, env=None):
        driver = _find_driver()
        if not driver:
            self.skipTest("driver binary not built (set XBOX_DRIVER or build tools/xbox)")
        tmp = tempfile.mkdtemp(prefix="desc_web_")
        self.addCleanup(shutil.rmtree, tmp, True)
        code_path = Path(tmp) / "shader.bin"
        bind_path = Path(tmp) / "shader.bind"
        spv_path = Path(tmp) / "out.spv"
        code_path.write_bytes(code_bytes)
        bind_path.write_text(bind_text, encoding="utf-8")
        proc = subprocess.run([driver, str(code_path), "32", str(spv_path), str(bind_path), "1", "cs"], capture_output=True, text=True, timeout=30,
                              env=dict(os.environ, **(env or {})))
        return proc, spv_path

    def _compile_and_validate(self, code_bytes, bind_text):
        proc, spv_path = self._run_driver(code_bytes, bind_text)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        val = _find_spirv_val()
        if val:
            vp = subprocess.run([val, "--target-env", "vulkan1.3", str(spv_path)], capture_output=True, text=True, timeout=30)
            self.assertEqual(vp.returncode, 0, vp.stdout + vp.stderr)

    def test_nested_pointer_phi_image_load(self):
        bind = (
            "node 0 user 4\n"
            "node 1 child 0 8\n"
            "node 2 child 0 16\n"
            "node 3 child 0 24\n"
            "desc 0 32 buf\n"
            "desc 1 0 img1\n"
            "desc 2 0 img1\n"
            "desc 3 0 img1\n"
        )
        self._compile_and_validate(_nested_pointer_phi_image_shader(), bind)

    def test_mixed_readfirstlane_and_scalar_sampler_web(self):
        bind = "node 0 user 4\ndesc 0 16 img1\ndesc 0 48 buf\ndesc 0 64 samp\ndesc 0 80 samp\ndesc 0 96 samp\n"
        self._compile_and_validate(_mixed_readfirstlane_and_scalar_sampler_shader(), bind)

    def test_readfirstlane_active_mask_prunes_dead_vgpr_history(self):
        bind = "node 0 user 4\ndesc 0 16 img1\ndesc 0 48 buf\ndesc 0 64 samp\ndesc 0 80 samp\n"
        self._compile_and_validate(_readfirstlane_active_mask_pruning_shader(), bind)

    def test_reloads_of_the_same_srt_dwords_share_web_states(self):
        proc, _ = self._run_driver(_split_reloads_shader(260), SPLIT_RELOADS_BIND, {"XBOX_DUMP_ABI": "1"})
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertTrue(any("kind=4" in line and "count=4 " in line for line in proc.stdout.splitlines()), proc.stdout)

    def test_web_nested_past_the_depth_limit_is_refused(self):
        proc, _ = self._run_driver(_split_reloads_shader(600), SPLIT_RELOADS_BIND)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("GetImageResource dword 0 is not a valid runtime value", proc.stdout)
        self.assertIn("; descriptor web: nested more than 512 joins deep", proc.stdout)


if __name__ == "__main__":
    unittest.main()
