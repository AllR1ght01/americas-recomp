import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent
SCRIPT = TOOLS / "xbox" / "platform" / "carve_bink.py"
ALIGN = 4096
CHUNK = 1 << 26


def movie(payload_size, version=b"i", fill=0xAB):
    body = bytes([fill]) * payload_size
    return b"KB2" + version + struct.pack("<I", payload_size) + body


class CarveBinkTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name)
        self.out = self.dir / "out"

    def archive(self, placements, total=None):
        end = max((off + len(data) for off, data in placements), default=0)
        buf = bytearray(total if total is not None else end)
        for off, data in placements:
            buf[off:off + len(data)] = data
        path = self.dir / "movies.rpf"
        path.write_bytes(bytes(buf))
        return path

    def run_tool(self, archive, *extra):
        return subprocess.run(
            [sys.executable, "-I", str(SCRIPT), str(archive), str(self.out), *extra],
            capture_output=True, text=True, timeout=120,
        )

    def test_carves_an_aligned_movie_byte_for_byte(self):
        m = movie(100)
        result = self.run_tool(self.archive([(ALIGN, m)]))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.out / "movie0.bk2").read_bytes(), m)
        self.assertEqual(result.stdout.split(), ["movie0.bk2", hex(ALIGN), str(len(m))])

    def test_output_size_is_the_stored_size_plus_eight(self):
        m = movie(1000)
        self.run_tool(self.archive([(0, m)]))
        self.assertEqual(len((self.out / "movie0.bk2").read_bytes()), 1008)

    def test_default_count_is_one(self):
        archive = self.archive([(0, movie(10)), (ALIGN, movie(20))])
        self.run_tool(archive)
        self.assertEqual(sorted(p.name for p in self.out.iterdir()), ["movie0.bk2"])

    def test_count_carves_movies_in_archive_order(self):
        first, second, third = movie(10, fill=1), movie(20, fill=2), movie(30, fill=3)
        archive = self.archive([(0, first), (ALIGN, second), (3 * ALIGN, third)])
        result = self.run_tool(archive, "2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sorted(p.name for p in self.out.iterdir()), ["movie0.bk2", "movie1.bk2"])
        self.assertEqual((self.out / "movie0.bk2").read_bytes(), first)
        self.assertEqual((self.out / "movie1.bk2").read_bytes(), second)

    def test_count_larger_than_the_movies_present_carves_all_of_them(self):
        archive = self.archive([(0, movie(10)), (2 * ALIGN, movie(20))])
        result = self.run_tool(archive, "5")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(list(self.out.iterdir())), 2)

    def test_unaligned_header_is_ignored(self):
        archive = self.archive([(ALIGN + 4, movie(50)), (2 * ALIGN, movie(60, fill=7))])
        self.run_tool(archive)
        self.assertEqual((self.out / "movie0.bk2").read_bytes(), movie(60, fill=7))

    def test_every_version_letter_is_accepted(self):
        for letter in (b"a", b"g", b"i", b"z"):
            with self.subTest(letter=letter):
                m = movie(10, version=letter)
                self.run_tool(self.archive([(0, m)]))
                self.assertEqual((self.out / "movie0.bk2").read_bytes(), m)

    def test_other_signatures_are_not_movies(self):
        archive = self.archive([(0, b"KB2I" + struct.pack("<I", 0)), (ALIGN, b"BIK" + b"i" + struct.pack("<I", 0))])
        result = self.run_tool(archive)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(list(self.out.iterdir()), [])

    def test_archive_without_movies_creates_an_empty_output_directory(self):
        result = self.run_tool(self.archive([], total=3 * ALIGN))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertTrue(self.out.is_dir())
        self.assertEqual(list(self.out.iterdir()), [])

    def test_movie_in_a_later_read_chunk_reports_its_archive_offset(self):
        m = movie(200, fill=9)
        result = self.run_tool(self.archive([(CHUNK + ALIGN, m)]))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.out / "movie0.bk2").read_bytes(), m)
        self.assertEqual(result.stdout.split()[1], hex(CHUNK + ALIGN))

    def test_count_continues_into_the_next_read_chunk(self):
        first, second = movie(30, fill=1), movie(40, fill=2)
        result = self.run_tool(self.archive([(0, first), (CHUNK + ALIGN, second)]), "2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.out / "movie0.bk2").read_bytes(), first)
        self.assertEqual((self.out / "movie1.bk2").read_bytes(), second)

    def test_movie_starting_exactly_at_a_chunk_boundary_is_found_once(self):
        m = movie(64, fill=5)
        result = self.run_tool(self.archive([(CHUNK, m)]), "3")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sorted(p.name for p in self.out.iterdir()), ["movie0.bk2"])
        self.assertEqual((self.out / "movie0.bk2").read_bytes(), m)


if __name__ == "__main__":
    unittest.main()
