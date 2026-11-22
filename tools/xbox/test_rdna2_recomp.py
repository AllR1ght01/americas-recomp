"""Synthetic unit tests for the RDNA2 instruction decoder.

All instruction bytes are constructed in memory; no game data is used.
"""

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rdna2_recomp as rdna


class TestDecode(unittest.TestCase):
    def test_decode_s_endpgm(self):
        code = struct.pack("<I", 0xBF810000)

        inst = rdna.decode(code, 0)

        self.assertEqual(inst.fmt, "SOPP")
        self.assertEqual(inst.name, "s_endpgm")
        self.assertEqual(inst.size, 4)
        self.assertEqual(inst.words, [0xBF810000])

    def test_sweep_consecutive_instructions(self):
        code = struct.pack("<II", 0xBF810000, 0xBF810000)

        insts, bad = rdna.sweep(code, 0, len(code))

        self.assertEqual(len(insts), 2)
        self.assertEqual([inst.off for inst in insts], [0, 4])
        self.assertEqual(bad, [])

    def test_decode_incomplete_instruction(self):
        code = struct.pack("<I", 0xF4000000)

        inst = rdna.decode(code, 0)

        self.assertEqual(inst.fmt, "UNKNOWN")
        self.assertEqual(inst.name, "unknown")

    def test_sweep_mixed_sequence(self):
        code = struct.pack(
            "<III",
            0xBF810000,
            0xBF810000,
            0xF4000000,
        )

        insts, bad = rdna.sweep(code, 0, len(code))

        self.assertEqual(len(insts), 2)
        self.assertEqual(bad, [8])


if __name__ == "__main__":
    unittest.main()
