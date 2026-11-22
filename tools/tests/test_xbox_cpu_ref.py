"""Unit and optional Vulkan execution cross-check tests for the RDNA2 CPU reference interpreter."""
import math
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "xbox"))
os.environ.setdefault("XBOX_RDNA2", "recomp")
import cpu_compare as CC
import cpu_ref as C
import rdna2_recomp as R2
import rdna_asm

VOP3P_OPS = {v: k for k, v in R2.VOP3P.items()}
VOP3_OPS = {v: k for k, v in R2.VOP3.items()}
SOPK_OPS = {v: k for k, v in R2.SOPK.items()}
MUBUF_OPS = {v: k for k, v in R2.MUBUF.items()}
DS_OPS = {v: k for k, v in R2.DS.items()}


def _find_exec():
    env = os.environ.get("XBOX_EXEC")
    if env and Path(env).is_file():
        return env
    for name in ("exec", "exec.exe"):
        candidate = ROOT / "build-xbox" / name
        if candidate.is_file():
            return str(candidate)
    return None


def _enc_vop3p(name, vdst, s0, s1, s2=128, op_sel=0, op_sel_hi=0, neg=0, neg_hi=0, clamp=0):
    op = VOP3P_OPS[name]
    w0 = (0x33 << 26) | ((op & 0x7F) << 16) | ((clamp & 1) << 15) | (((op_sel_hi >> 2) & 1) << 14) | ((op_sel & 7) << 11) | ((neg_hi & 7) << 8) | (vdst & 0xFF)
    w1 = ((neg & 7) << 29) | ((op_sel_hi & 3) << 27) | ((s2 & 0x1FF) << 18) | ((s1 & 0x1FF) << 9) | (s0 & 0x1FF)
    return [w0, w1]


def _enc_vop3(name, vdst, s0, s1=128, s2=128, absm=0, neg=0, clamp=0):
    op = VOP3_OPS[name]
    w0 = (0x35 << 26) | ((op & 0x3FF) << 16) | ((clamp & 1) << 15) | ((absm & 7) << 8) | (vdst & 0xFF)
    w1 = ((neg & 7) << 29) | ((s2 & 0x1FF) << 18) | ((s1 & 0x1FF) << 9) | (s0 & 0x1FF)
    return [w0, w1]


def _enc_sopk(name, sdst, simm16):
    op = SOPK_OPS[name]
    return [0xB0000000 | ((op & 0x1F) << 23) | ((sdst & 0x7F) << 16) | (simm16 & 0xFFFF)]


def _enc_mubuf(name, vdata, vaddr, srsrc, soff=128, offset=0, offen=True, idxen=False):
    op = MUBUF_OPS[name]
    w0 = 0xE0000000 | (((op >> 7) & 1) << 25) | ((op & 0x7F) << 18) | (int(idxen) << 13) | (int(offen) << 12) | (offset & 0xFFF)
    w1 = ((soff & 0xFF) << 24) | ((srsrc // 4) << 16) | ((vdata & 0xFF) << 8) | (vaddr & 0xFF)
    return [w0, w1]


def _enc_ds(name, vdst, addr, d0=0, d1=0, offset=0):
    op = DS_OPS[name]
    w0 = 0xD8000000 | ((op & 0xFF) << 18) | (offset & 0xFFFF)
    w1 = ((vdst & 0xFF) << 24) | ((d1 & 0xFF) << 16) | ((d0 & 0xFF) << 8) | (addr & 0xFF)
    return [w0, w1]


def _run_cpu_and_optional_gpu(code_bytes, lines, check_data_ids, check_image_ids, exec_bin):
    regions, sgpr, images = CC.scenario_memory(lines)
    mem = C.Memory()
    guest = {rid: 0x10000000 + 0x1000000 * k for k, rid in enumerate(sorted(regions))}
    for rid, dd in regions.items():
        mem.add(guest[rid], dd)
    wave = C.Wave(code_bytes, mem, sgpr)
    wave.images = images
    wave.run(100000)

    if exec_bin:
        with tempfile.TemporaryDirectory(prefix="cpuref_gpu_") as d:
            code_path = Path(d) / "code.bin"
            code_path.write_bytes(code_bytes)
            sc_lines = [f"code {code_path}", f"out {d}/out"] + list(lines)
            sc_path = Path(d) / "scenario.txt"
            sc_path.write_text("\n".join(sc_lines) + "\n", encoding="utf-8")
            env = dict(os.environ)
            p = subprocess.run([exec_bin, str(sc_path)], capture_output=True, text=True, timeout=60, env=env)
            if "RAN" not in p.stdout and "supported 8..8" in p.stdout and "XBOX_HOST_SUBGROUP" not in env:
                env["XBOX_HOST_SUBGROUP"] = "8"
                p = subprocess.run([exec_bin, str(sc_path)], capture_output=True, text=True, timeout=60, env=env)
            if "RAN" not in p.stdout:
                raise AssertionError(f"exec failed: {p.stdout[:400]} {p.stderr[:200]}")
            for rid in check_data_ids:
                gpu = (Path(d) / "out" / f"region{rid}.bin").read_bytes()
                cpu = bytes(regions[rid])
                if gpu != cpu:
                    for k in range(0, len(gpu), 4):
                        a, b = struct.unpack_from("<I", gpu, k)[0], struct.unpack_from("<I", cpu, k)[0]
                        if a != b:
                            raise AssertionError(f"region {rid} off {k}: gpu=0x{a:08x} cpu=0x{b:08x}")
            for iid in check_image_ids:
                gpu = (Path(d) / "out" / f"image{iid}.bin").read_bytes()
                cpu = struct.pack("<%dI" % len(images[iid]["data"]), *images[iid]["data"])
                if gpu != cpu:
                    for k in range(0, len(gpu), 4):
                        a, b = struct.unpack_from("<I", gpu, k)[0], struct.unpack_from("<I", cpu, k)[0]
                        if a != b:
                            raise AssertionError(f"image {iid} off {k}: gpu=0x{a:08x} cpu=0x{b:08x}")
    return regions, images


class CpuRefTest(unittest.TestCase):
    def test_vop3p_d16_scalar_and_ds_cpu_and_gpu(self):
        p = rdna_asm.Program()
        p.smem("s_load_dwordx4", 8, 4, 0)
        p.smem("s_load_dwordx4", 12, 4, 16)
        p.words += _enc_sopk("s_waitcnt_vscnt", 125, 0)
        p.sopp("s_waitcnt", 0)

        p.vop2("v_lshlrev_b32", 1, 4, "v0")
        p.words += _enc_mubuf("buffer_load_format_d16_xyzw", 2, 1, 8, offen=True, idxen=False)
        p.sopp("s_waitcnt", 0)

        p.words += _enc_vop3p("v_fma_mix_f32", 4, 256 + 2, 256 + 2, 256 + 3, op_sel=0b010, op_sel_hi=0b111, neg=0b100, neg_hi=0b001)
        p.words += _enc_vop3p("v_pk_add_f16", 5, 256 + 2, 256 + 3, op_sel=0b00, op_sel_hi=0b11)
        p.words += _enc_vop3("v_readlane_b32", 16, 256 + 4, 128 + 3)
        p.words += _enc_sopk("s_mulk_i32", 16, 1)
        p.vop1("v_mov_b32", 6, "s16")

        p.vop2("v_lshlrev_b32", 7, 2, "v0")
        p.words += _enc_ds("ds_write2st64_b32", 0, 7, 4, 5, offset=(1 << 8) | 0)
        p.sopp("s_waitcnt", 0)
        p.sopp("s_barrier")
        p.words += _enc_ds("ds_read2st64_b32", 10, 7, offset=(1 << 8) | 0)
        p.sopp("s_waitcnt", 0)

        p.vop1("v_mov_b32", 12, 10)
        p.words += _enc_ds("ds_write_b32", 0, 7, 12, offset=1024)
        p.sopp("s_waitcnt", 0)
        p.vop1("v_mov_b32", 13, 5)
        p.words += _enc_ds("ds_add_rtn_u32", 14, 7, 13, offset=1024)
        p.sopp("s_waitcnt", 0)

        p.vop1("v_mov_b32", 20, "v10")
        p.vop1("v_mov_b32", 21, "v11")
        p.vop1("v_mov_b32", 22, "v6")
        p.vop1("v_mov_b32", 23, "v14")
        p.mubuf("buffer_store_dwordx4", 20, 1, 12, 128, 0)
        p.words += _enc_mubuf("buffer_store_format_d16_xy", 5, 1, 12, offset=1024, offen=True, idxen=False)
        p.sopp("s_endpgm")

        in_words = bytearray()
        for lane in range(64):
            g = lane & 7
            in_words += struct.pack("<4f", -0.5 * (g % 5 + 1), 1.25, 0.25 * (g % 4 + 1), 2.0)

        with tempfile.TemporaryDirectory(prefix="cpuref_in_") as d:
            in_path = Path(d) / "in.bin"
            in_path.write_bytes(bytes(in_words))
            lines = [
                "wave 64",
                "groups 1 1 1",
                "threads 64 1 1",
                "userdwords 6",
                "lds 4096",
                "lowguest",
                "region 0 256",
                f"region 1 {len(in_words)} init {in_path}",
                "region 2 2048",
                "user32 4 0",
                "ptrlo 0 0 1", "ptrhi 0 4 1 0x100000", "poke 0 8 64", "poke 0 12 0x1004dfac",
                "ptrlo 0 16 2", "ptrhi 0 20 2 0x100000", "poke 0 24 128", "poke 0 28 0x1004dfac",
            ]
            regions, _ = _run_cpu_and_optional_gpu(p.bytes(), lines, [2], [], _find_exec())

        lane3_f = abs(-0.5 * 4) * 1.25 - 0.25 * 4
        for lane in range(64):
            g = lane & 7
            a0 = abs(-0.5 * (g % 5 + 1))
            a1 = 1.25
            a2 = 0.25 * (g % 4 + 1)
            want_mix = a0 * a1 - a2
            got_mix, got_pk, got_rl, got_rtn = struct.unpack_from("<4I", regions[2], lane * 16)
            self.assertAlmostEqual(C.f32(got_mix), want_mix, places=5)
            self.assertAlmostEqual(C.f32(got_rl), lane3_f, places=5)
            self.assertEqual(got_rtn, 10)
            pk_lo = C.f16(got_pk & 0xFFFF)
            pk_hi = C.f16(got_pk >> 16)
            self.assertAlmostEqual(pk_lo, (-0.5 * (g % 5 + 1)) + a2, places=3)
            self.assertAlmostEqual(pk_hi, 1.25 + 2.0, places=3)
            s_x, s_y = struct.unpack_from("<2f", regions[2], 1024 + lane * 16)
            self.assertAlmostEqual(s_x, pk_lo, places=3)
            self.assertAlmostEqual(s_y, pk_hi, places=3)

    def test_v_pack_b32_f16_and_readlane_upper_half_cpu(self):
        p = rdna_asm.Program()
        p.vop1("v_mov_b32", 2, ("lit", C.b16(-1.5) | (C.b16(2.5) << 16)))
        p.vop1("v_mov_b32", 3, ("lit", C.b16(3.0) | (C.b16(-4.0) << 16)))
        op_pack = VOP3_OPS["v_pack_b32_f16"]
        w0 = (0x35 << 26) | ((op_pack & 0x3FF) << 16) | (0b0010 << 11) | (0b01 << 8) | 4
        w1 = (0b10 << 29) | ((256 + 3) << 9) | (256 + 2)
        p.words += [w0, w1]
        p.words += _enc_vop3("v_writelane_b32", 5, 255, 128 + 41) + [0xCAFEBABE]
        p.words += _enc_vop3("v_readlane_b32", 20, 256 + 5, 128 + 41)
        p.sopp("s_endpgm")
        w = C.Wave(p.bytes(), C.Memory(), {})
        w.run()
        self.assertEqual(C.f16(w.v[4][0] & 0xFFFF), 1.5)
        self.assertEqual(C.f16(w.v[4][0] >> 16), 4.0)
        self.assertEqual(w.s[20], 0xCAFEBABE)
        self.assertTrue(math.isnan(C.fma32(0.0, float("inf"), 1.0)))

    def test_gather4_and_3d_images_cpu_and_gpu(self):
        p = rdna_asm.Program()
        p.smem("s_load_dwordx8", 8, 4, 0)
        p.smem("s_load_dwordx8", 16, 4, 32)
        p.smem("s_load_dwordx4", 24, 4, 64)
        p.sopp("s_waitcnt", 0)

        p.vop1("v_mov_b32", 2, ("lit", C.b32(0.35)))
        p.vop1("v_mov_b32", 3, ("lit", C.b32(0.65)))
        p.mimg("image_gather4_lz", 4, 2, 8, dmask=0x2, dim=1, ssamp=0)

        p.vop2("v_and_b32", 10, 3, "v0")
        p.vop2("v_lshrrev_b32", 11, 2, "v0")
        p.vop2("v_and_b32", 11, 3, "v11")
        p.vop2("v_lshrrev_b32", 12, 4, "v0")
        p.mimg("image_load", 16, 10, 16, dmask=0xF, dim=2)
        p.sopp("s_waitcnt", 0)

        p.vop2("v_lshlrev_b32", 1, 5, "v0")
        p.mubuf("buffer_store_dwordx4", 4, 1, 24, 128, 0)
        p.mubuf("buffer_store_dwordx4", 16, 1, 24, 128, 16)
        p.sopp("s_endpgm")

        img2d = bytearray()
        for y in range(4):
            for x in range(4):
                img2d += struct.pack("<4f", float(x), float(10 * y + x + 1), float(y), 1.0)

        img3d = bytearray()
        for z in range(4):
            for y in range(4):
                for x in range(4):
                    img3d += struct.pack("<4f", float(x), float(y), float(z), float(z * 16 + y * 4 + x + 1))

        with tempfile.TemporaryDirectory(prefix="cpuref_img_") as d:
            p2 = Path(d) / "img2d.bin"
            p3 = Path(d) / "img3d.bin"
            p2.write_bytes(bytes(img2d))
            p3.write_bytes(bytes(img3d))
            lines = [
                "wave 64",
                "groups 1 1 1",
                "threads 64 1 1",
                "userdwords 6",
                "lowguest",
                "region 0 256",
                "region 1 2048",
                f"image 1 4 4 init {p2}",
                f"image 2 4 4 dim 3d depth 4 init {p3}",
                "user32 4 0",
                "timg 0 0 1",
                "timg 0 32 2",
                "ptrlo 0 64 1", "ptrhi 0 68 1 0x100000", "poke 0 72 128", "poke 0 76 0x1004dfac",
            ]
            regions, _ = _run_cpu_and_optional_gpu(p.bytes(), lines, [1], [], _find_exec())

        g0 = struct.unpack_from("<4f", regions[1], 0)
        self.assertEqual(g0, (31.0, 32.0, 22.0, 21.0))
        for lane in range(64):
            x, y, z = lane & 3, (lane >> 2) & 3, lane >> 4
            got3 = struct.unpack_from("<4f", regions[1], lane * 32 + 16)
            self.assertEqual(got3, (float(x), float(y), float(z), float(lane + 1)))

    def test_saveexec_variants_and_bit_ops(self):
        p = rdna_asm.Program()
        p.smem("s_load_dwordx4", 8, 4, 0)
        p.sopp("s_waitcnt", 0)
        p.vop1("v_mov_b32", 2, 0)
        p.vopc("v_cmpx_lt_u32", 3, "v0")
        p.sop1("s_mov_b64", 16, rdna_asm.EXEC_LO)
        p.sop1("s_or_saveexec_b64", 18, ("lit", 0x3))
        p.vop1("v_mov_b32", 2, 7)
        p.sop1("s_mov_b64", rdna_asm.EXEC_LO, "s16")
        p.sop1("s_mov_b64", rdna_asm.EXEC_LO, -1)
        p.vop2("v_lshlrev_b32", 1, 2, "v0")
        p.mubuf("buffer_store_dword", 2, 1, 8, 128, 0)
        p.sopp("s_endpgm")

        lines = [
            "wave 64",
            "groups 1 1 1",
            "threads 64 1 1",
            "userdwords 6",
            "lowguest",
            "region 0 256",
            "region 1 256",
            "user32 4 0",
            "ptrlo 0 0 1", "ptrhi 0 4 1 0x100000", "poke 0 8 16", "poke 0 12 0x1004dfac",
        ]
        regions, _ = _run_cpu_and_optional_gpu(p.bytes(), lines, [1], [], _find_exec())
        got = struct.unpack("<64I", regions[1])
        want = tuple(7 if (i < 2 or i > 3) else 0 for i in range(64))
        self.assertEqual(got, want)


if __name__ == "__main__":
    unittest.main()
