import struct
import unittest
import zlib

from carve_shaders import CHUNK_LIMIT, fssm_from


def xbch(stream, declared_size):
    return struct.pack("<4s5I", b"XBCH", 3, len(stream), 0, declared_size, 0) + stream


class XbchInflateTest(unittest.TestCase):
    def setUp(self):
        self.payload = b"FSSM" + bytes(32)
        self.stream = zlib.compress(self.payload)

    def test_matching_size(self):
        self.assertEqual(fssm_from(xbch(self.stream, len(self.payload))), self.payload)

    def test_size_mismatch_in_either_direction(self):
        for delta in (-1, 1):
            with self.subTest(delta=delta):
                self.assertIsNone(fssm_from(xbch(self.stream, len(self.payload) + delta)))

    def test_truncated_stream(self):
        self.assertIsNone(fssm_from(xbch(self.stream[:-1], len(self.payload))))

    def test_size_above_limit(self):
        self.assertIsNone(fssm_from(xbch(self.stream, CHUNK_LIMIT + 1)))


if __name__ == "__main__":
    unittest.main()
