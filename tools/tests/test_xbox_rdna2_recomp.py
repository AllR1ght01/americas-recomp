import collections
import importlib.util
import struct
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent


def words(*w):
    return struct.pack("<%dI" % len(w), *w) + b"\0" * 16


class Rdna2RecompTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("rdna2_recomp", TOOLS / "xbox" / "rdna2_recomp.py")
        cls.r = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.r)

    def one(self, *w):
        return self.r.decode(words(*w), 0)

    def test_endpgm(self):
        i = self.one(0xBF810000)
        self.assertEqual((i.fmt, i.name, i.size, i.words), ("SOPP", "s_endpgm", 4, [0xBF810000]))

    def test_literal_operand(self):
        i = self.one(0x7E0202FF, 0x12345678)
        self.assertEqual((i.fmt, i.name, i.size), ("VOP1", "v_mov_b32", 8))

    def test_madmk_and_fmamk_carry_a_literal_and_keep_their_names(self):
        for op, name in ((0x20, "v_madmk_f32"), (0x2C, "v_fmamk_f32"), (0x21, "v_madak_f32"), (0x2D, "v_fmaak_f32")):
            i = self.one((op << 25) | (2 << 17) | (1 << 9) | 257, 0x3F800000)
            self.assertEqual((i.fmt, i.name, i.size), ("VOP2", name, 8))

    def test_dpp_word_is_not_a_literal(self):
        i = self.one((0x03 << 25) | (2 << 17) | (1 << 9) | 250, 0xFF)
        self.assertEqual((i.fmt, i.name, i.size), ("VOP2+DPP", "v_add_f32", 8))

    def test_vop3_reads_a_literal_only_from_decoded_sources(self):
        two_sources = 0xD70F0000 | 3
        self.assertEqual(self.one(two_sources, 1 | (2 << 9) | (255 << 18)).size, 8)
        self.assertEqual(self.one(two_sources, 255 | (2 << 9)).size, 12)
        self.assertEqual(self.one(two_sources, 0).name, "v_add_co_u32")

    def test_sopk_names_and_setreg_literal(self):
        self.assertEqual(self.one(0xB0000000 | (0x17 << 23)).name, "s_waitcnt_vscnt")
        self.assertEqual(self.one(0xB0000000 | (0x0F << 23)).name, "s_addk_i32")
        i = self.one(0xB0000000 | (0x15 << 23), 7)
        self.assertEqual((i.name, i.size), ("s_setreg_imm32_b32", 8))

    def test_mimg_nsa_words(self):
        i = self.one(0xF1980001 | (2 << 1), 0, 0, 0)
        self.assertEqual((i.fmt, i.name, i.size), ("MIMG+NSA2", "image_bvh_intersect_ray", 16))

    def test_flat_segments(self):
        for seg, fmt, name in ((0, "FLAT", "flat_load_dword"), (1, "SCRATCH", "scratch_load_dword"), (2, "GLOBAL", "global_load_dword")):
            i = self.one(0xDC300000 | (seg << 14), 0)
            self.assertEqual((i.fmt, i.name, i.size), (fmt, name, 8))

    def test_mubuf_opcode_bit_7(self):
        self.assertEqual(self.one(0xE2000000, 0).name, "buffer_load_format_d16_x")

    def test_sweep_skips_words_of_no_known_family(self):
        blob = struct.pack("<3I", 0xBF800000, 0xD0000000, 0xBF810000)
        insts, bad = self.r.sweep(blob, 0, len(blob))
        self.assertEqual([i.name for i in insts], ["s_nop", "s_endpgm"])
        self.assertEqual(bad, [4])

    def test_decode_incomplete_smem_instruction(self):
        blob = struct.pack("<I", 0xF4000000)
        inst = self.r.decode(blob, 0)

        self.assertEqual((inst.fmt, inst.name), ("UNKNOWN", "unknown"))
        self.assertEqual(inst.words, [0xF4000000])

    def test_names_are_unique_per_table(self):
        for fmt in ("SOP1", "SOP2", "SOPK", "SOPC", "SOPP", "SMEM", "VOP1", "VOP2", "VOPC", "VOP3", "VOP3P", "VINTRP", "DS", "MUBUF",
                    "MTBUF", "FLAT", "MIMG"):
            names = collections.Counter(getattr(self.r, fmt).values())
            self.assertEqual([n for n, c in names.items() if c > 1], [], fmt)


if __name__ == "__main__":
    unittest.main()
