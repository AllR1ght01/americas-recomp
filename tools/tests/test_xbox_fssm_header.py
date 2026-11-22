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
