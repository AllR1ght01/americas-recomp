import importlib.util
import struct
import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent

spec = importlib.util.spec_from_file_location("fssm_header", TOOLS / "xbox" / "fssm_header.py")
fssm = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = fssm
spec.loader.exec_module(fssm)


def blob_with_inputs(entries):
    b = bytearray(0x200)
    for k, (semantic, mask, index, slot, flags) in enumerate(entries):
        p = 0x138 + 8 * k
        b[p:p + 8] = bytes([6, semantic, mask, index, 0, 0, slot, flags])
    return b


def blob_with_vertex_inputs(entries):
    b = bytearray(0x200)
    for k, (register, mask, vgpr, top) in enumerate(entries):
        p = 0x138 + 5 * k
        b[p:p + 5] = bytes([6, register, mask, vgpr, top])
    return b


class PixelInputsTest(unittest.TestCase):
    def test_reads_entries_until_tag_changes(self):
        entries = [(1, 0xF, 0, 1, 0x00), (2, 0x3, 1, 2, 0x18), (5, 0x1, 2, 3, 0x08)]
        b = blob_with_inputs(entries)
        b[0x138 + 8 * 3] = 7
        self.assertEqual(fssm.pixel_inputs(bytes(b)), entries)

    def test_fp16_mask_uses_flag_bit_4_by_table_position(self):
        b = blob_with_inputs([(1, 0xF, 0, 9, 0x10), (2, 0xF, 1, 1, 0x08), (3, 0xF, 2, 4, 0x10)])
        self.assertEqual(fssm.interpolants(bytes(b)), (3, 0b101))

    def test_no_table(self):
        self.assertEqual(fssm.interpolants(bytes(0x200)), (0, 0))
        self.assertEqual(fssm.flat_mask(bytes(0x200)), 0)

    def test_flat_mask_takes_flags_0x01_and_0x09_not_0x02_0x1a_0x0a(self):
        b = blob_with_inputs([(1, 0xF, 0, 1, 0x02), (2, 0x1, 1, 2, 0x01), (3, 0xF, 2, 3, 0x1A), (4, 0x3, 3, 4, 0x09), (5, 0xF, 4, 5, 0x0A)])
        self.assertEqual(fssm.flat_mask(bytes(b)), 0b01010)

    def test_table_stops_at_0x1d0(self):
        b = blob_with_inputs([(k, 0xF, k, k, 0) for k in range(24)])
        self.assertEqual(len(fssm.pixel_inputs(bytes(b))), (0x1D0 - 0x138) // 8)

    def test_header_shorter_than_0x1d0(self):
        self.assertEqual(fssm.pixel_inputs(bytes(0x100)), [])
        self.assertEqual(fssm.interpolants(bytes(0x100)), (0, 0))
        self.assertEqual(fssm.flat_mask(bytes(0x100)), 0)
        b = blob_with_inputs([(k, 0xF, k, k, 0) for k in range(24)])
        self.assertEqual(len(fssm.pixel_inputs(bytes(b[:0x150]))), 3)

    def test_both_interpolation_bits_set_is_not_flat(self):
        # bits 0-1 are exclusive on every carved PS (4,295 flat entries all have bit 0,
        # 49,594 interpolated all bit 1, see the fssm_header docstring); a table entry
        # with both set must fall to the interpolated side, not to flat_mask.
        b = blob_with_inputs([(1, 0xF, 0, 1, 0x03), (2, 0xF, 1, 2, 0x0B), (3, 0xF, 2, 3, 0x01)])
        self.assertEqual(fssm.flat_mask(bytes(b)), 0b100)

    def test_flat_and_fp16_bits_combine_into_both_masks(self):
        # flag 0x11 is a 16-bit interpolant the code reads only at P0: it belongs to
        # the fp16 mask of interpolants() AND to flat_mask().
        b = blob_with_inputs([(1, 0xF, 0, 1, 0x11), (2, 0xF, 1, 2, 0x10), (3, 0xF, 2, 3, 0x01)])
        self.assertEqual(fssm.interpolants(bytes(b))[1], 0b011)
        self.assertEqual(fssm.flat_mask(bytes(b)), 0b101)

    def test_masks_over_nine_entries_are_positional(self):
        # nine interpolants: fp16 at positions 1, 4, 8; flat at 0, 4 - position 4 is
        # both (16-bit read only at P0), and no other entry lands in either mask.
        flags = [0x01, 0x10, 0x02, 0x02, 0x11, 0x02, 0x00, 0x08, 0x10]
        b = blob_with_inputs([(k + 1, 0xF, k, k, flags[k]) for k in range(9)])
        n, fp16 = fssm.interpolants(bytes(b))
        self.assertEqual((n, fp16, fssm.flat_mask(bytes(b))), (9, 0b100010010, 0b000010001))


class VertexInputsTest(unittest.TestCase):
    def test_reads_entries_until_tag_changes(self):
        b = blob_with_vertex_inputs([(0, 0x7, 4, 2), (1, 0x3, 8, 1)])
        self.assertEqual(fssm.vertex_inputs(bytes(b)), [(0, 0x7, 4, 3), (1, 0x3, 8, 2)])
        self.assertEqual(fssm.vin_text(bytes(b)), "vin 4 3 0\nvin 8 2 1\n")

    def test_table_stops_at_0x1d0(self):
        b = blob_with_vertex_inputs([(k, 0xF, 4 + 4 * k, 3) for k in range(31)])
        self.assertEqual(len(fssm.vertex_inputs(bytes(b))), (0x1D0 - 0x138) // 5)

    def test_header_shorter_than_0x1d0(self):
        self.assertEqual(fssm.vertex_inputs(bytes(0x100)), [])
        self.assertEqual(fssm.vin_text(bytes(0x100)), "")
        b = blob_with_vertex_inputs([(k, 0xF, 4 + 4 * k, 3) for k in range(31)])
        self.assertEqual(len(fssm.vertex_inputs(bytes(b[:0x150]))), 4)


class VertexInputsExtraTest(unittest.TestCase):
    @staticmethod
    def blob(entries):
        b = bytearray(0x200)
        for k, (reg, mask, vgpr, hi) in enumerate(entries):
            p = 0x138 + 5 * k
            b[p:p + 5] = bytes([6, reg, mask, vgpr, hi])
        return b

    def test_reads_5_byte_entries_until_tag_changes(self):
        b = VertexInputsExtraTest.blob([(0, 0xF, 4, 3), (1, 0x3, 8, 0)])
        self.assertEqual(fssm.vertex_inputs(bytes(b)), [(0, 0xF, 4, 4), (1, 0x3, 8, 1)])

    def test_no_table_is_empty(self):
        self.assertEqual(fssm.vertex_inputs(bytes(0x200)), [])
        self.assertEqual(fssm.vin_text(bytes(0x200)), "")

    def test_vin_text_matches_vertex_inputs(self):
        # driver.cpp consumes vin lines "vin <first vgpr> <vgpr count> <input register>".
        b = VertexInputsExtraTest.blob([(0, 0xF, 4, 3), (1, 0x1, 8, 7)])
        text = fssm.vin_text(bytes(b))
        self.assertEqual(text, "vin 4 4 0\nvin 8 8 1\n")
        self.assertEqual(len(text.splitlines()), len(fssm.vertex_inputs(bytes(b))))


class DirectoryTest(unittest.TestCase):
    def test_offset_plus_size_is_next_offset(self):
        b = bytearray(0x200)
        struct.pack_into("<I", b, 0x120, 0x400)
        struct.pack_into("<I", b, 0x12C, 0x80)
        struct.pack_into("<I", b, 0x130, 0x480)
        self.assertTrue(fssm.directory_ok(bytes(b)))
        struct.pack_into("<I", b, 0x130, 0x484)
        self.assertFalse(fssm.directory_ok(bytes(b)))


if __name__ == "__main__":
    unittest.main()
