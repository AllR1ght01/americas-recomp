import argparse
import ctypes
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


class XMemTraceTests(unittest.TestCase):
    fields = (
        "era_object_type", "era_page_size", "era_allocator_id",
        "era_alignment_log2", "era_memory_type", "era_reserved",
    )

    def setUp(self):
        temp = self.enterContext(tempfile.TemporaryDirectory(prefix="xmem trace "))
        self.log = Path(temp) / "calls.log"
        self.enterContext(patch.dict(os.environ, {"XMEM_LOG": str(self.log)}))

    def allocate(self, attrs, size=33):
        pointer = self.library.XMemAlloc(size, attrs)
        self.assertTrue(pointer, "XMemAlloc failed")
        try:
            self.assertEqual(ctypes.string_at(pointer, max(size, 1)), bytes(max(size, 1)))
            ctypes.memset(pointer, 0xA5, max(size, 1))
        finally:
            self.library.XMemFree(pointer, attrs)
        return pointer

    def records(self):
        result = []
        for line in self.log.read_text(encoding="ascii").splitlines():
            operation, *tokens = line.split()
            record = {}
            for token in tokens:
                name, value = token.split("=", 1)
                record[name] = int(value, 16 if name == "ptr" else 0)
            result.append((operation, record))
        return result

    def test_recorded_attribute_words(self):
        cases = (
            (0x05E50000, (0, 0, 0xE5, 5, 0, 0)),
            (0xA8004000, (0, 1, 0, 8, 5, 0)),
        )
        for word, expected in cases:
            with self.subTest(word=hex(word)):
                self.log.unlink(missing_ok=True)
                pointer = self.allocate(word)
                records = self.records()
                self.assertEqual(len(records), 2)
                for (operation, record), (name, size) in zip(records, (("alloc", 33), ("free", 0))):
                    self.assertEqual(operation, name)
                    self.assertEqual(record, {
                        "ptr": pointer, "a": size, "b": word,
                        **dict(zip(self.fields, expected)),
                    })

    def test_field_boundaries_and_unknown_encodings(self):
        cases = (
            (0x0000000000000000, (0, 0, 0, 0, 0, 0)),
            (0x0000000000002000, (0x2000, 0, 0, 0, 0, 0)),
            (0x0000000000004000, (0, 1, 0, 0, 0, 0)),
            (0x0000000000008000, (0, 2, 0, 0, 0, 0)),
            (0x0000000000010000, (0, 0, 1, 0, 0, 0)),
            (0x0000000000800000, (0, 0, 0x80, 0, 0, 0)),
            (0x0000000001000000, (0, 0, 0, 1, 0, 0)),
            (0x0000000010000000, (0, 0, 0, 16, 0, 0)),
            (0x0000000020000000, (0, 0, 0, 0, 1, 0)),
            (0x0000000100000000, (0, 0, 0, 0, 8, 0)),
            (0x0000000200000000, (0, 0, 0, 0, 0, 1)),
            (0x8000000000000000, (0, 0, 0, 0, 0, 0x40000000)),
            (0xFFFFFFFFFFFFFFFF, (0x3FFF, 3, 0xFF, 31, 15, 0x7FFFFFFF)),
        )
        for word, expected in cases:
            with self.subTest(word=hex(word)):
                self.log.unlink(missing_ok=True)
                self.allocate(word)
                for _, record in self.records():
                    self.assertEqual(record["b"], word)
                    self.assertEqual(tuple(record[field] for field in self.fields), expected)

    def test_free_uses_its_own_attribute_argument(self):
        self.library.XMemFree(None, 0xA8004000)
        records = self.records()
        self.assertEqual(len(records), 1)
        operation, record = records[0]
        self.assertEqual(operation, "free")
        self.assertEqual(record["ptr"], 0)
        self.assertEqual(record["b"], 0xA8004000)
        self.assertEqual(tuple(record[field] for field in self.fields), (0, 1, 0, 8, 5, 0))

    def test_zero_size_preserves_raw_size(self):
        self.allocate(0x05E50000, size=0)
        self.assertEqual(self.records()[0][1]["a"], 0)

    def test_non_allocation_log_has_no_attribute_fields(self):
        output = (ctypes.c_uint64 * 2)()
        self.assertTrue(self.library.XMemGetWorkingSetStatistics(0x20000001, output))
        self.assertEqual(self.records(), [("wsstat", {
            "ptr": ctypes.addressof(output), "a": 0x20000001, "b": output[1],
        })])

    def test_logging_disabled(self):
        os.environ.pop("XMEM_LOG")
        self.allocate(0x05E50000)
        self.assertFalse(self.log.exists())

    def test_oversized_log_path(self):
        os.environ["XMEM_LOG"] = str(self.log.parent) + "\\" + "x" * 300
        self.allocate(0x05E50000)
        self.assertFalse(self.log.exists())


def main():
    parser = argparse.ArgumentParser(description="Test the stand-in xmem.dll trace with synthetic calls.")
    parser.add_argument("--dll", type=Path, required=True)
    args = parser.parse_args()
    if os.name != "nt" or ctypes.sizeof(ctypes.c_void_p) != 8:
        parser.error("requires 64-bit Windows Python")
    library = ctypes.WinDLL(str(args.dll.resolve()))
    library.XMemAlloc.argtypes = (ctypes.c_size_t, ctypes.c_uint64)
    library.XMemAlloc.restype = ctypes.c_void_p
    library.XMemFree.argtypes = (ctypes.c_void_p, ctypes.c_uint64)
    library.XMemFree.restype = None
    library.XMemGetWorkingSetStatistics.argtypes = (ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint64))
    library.XMemGetWorkingSetStatistics.restype = ctypes.c_int
    XMemTraceTests.library = library
    unittest.main(argv=[__file__], verbosity=2)


if __name__ == "__main__":
    main()
