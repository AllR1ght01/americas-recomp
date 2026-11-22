import contextlib
import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent

sys.path.insert(0, str(TOOLS / "xbox"))
spec = importlib.util.spec_from_file_location("harness_guard", TOOLS / "xbox" / "harness_guard.py")
guard = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = guard
spec.loader.exec_module(guard)
VERSION = guard.bindings.VERSION


class StaleBindsTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name)

    def write(self, name, text):
        (self.dir / name).write_text(text, encoding="utf-8")

    def test_current_stamp_is_fresh(self):
        self.write("1.bind", f"version {VERSION}\nnode 0\n")
        self.write("2.bind", f"version {VERSION}\n")
        self.assertEqual(guard.stale_binds(str(self.dir)), ([], 2))

    def test_other_stamp_is_reported_with_its_value(self):
        self.write("1.bind", "version 000000000000\n")
        bad, n = guard.stale_binds(str(self.dir))
        self.assertEqual((bad, n), ([("1.bind", "made by bindings.py 000000000000")], 1))

    def test_missing_stamp_is_reported(self):
        self.write("1.bind", "node 0\n")
        self.write("2.bind", "")
        bad, n = guard.stale_binds(str(self.dir))
        self.assertEqual(sorted(bad), [("1.bind", "no version stamp"), ("2.bind", "no version stamp")])
        self.assertEqual(n, 2)

    def test_stamp_word_without_a_value_is_stale(self):
        self.write("1.bind", "version\n")
        bad, _ = guard.stale_binds(str(self.dir))
        self.assertEqual(bad, [("1.bind", "made by bindings.py ?")])

    def test_only_bind_files_are_scanned(self):
        self.write("1.bind", f"version {VERSION}\n")
        self.write("1.bin", "not a binding")
        self.write("notes.txt", "version 000000000000")
        self.assertEqual(guard.stale_binds(str(self.dir)), ([], 1))

    def test_listed_ids_limit_the_scan(self):
        self.write("1.bind", f"version {VERSION}\n")
        self.write("2.bind", "version 000000000000\n")
        self.assertEqual(guard.stale_binds(str(self.dir), ids=[1]), ([], 1))
        bad, n = guard.stale_binds(str(self.dir), ids=[2])
        self.assertEqual((len(bad), n), (1, 1))

    def test_listed_id_without_a_file_is_missing(self):
        self.write("1.bind", f"version {VERSION}\n")
        bad, n = guard.stale_binds(str(self.dir), ids=[1, 7])
        self.assertEqual((bad, n), ([("7.bind", "missing")], 2))

    def test_empty_directory_has_nothing_stale(self):
        self.assertEqual(guard.stale_binds(str(self.dir)), ([], 0))


class RequireFreshBindsTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name)

    def test_fresh_directory_reports_the_count(self):
        (self.dir / "1.bind").write_text(f"version {VERSION}\n", encoding="utf-8")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            guard.require_fresh_binds(str(self.dir))
        self.assertIn("1 bindings, all from bindings.py " + VERSION, out.getvalue())

    def test_stale_directory_exits_with_count_and_fix(self):
        (self.dir / "1.bind").write_text(f"version {VERSION}\n", encoding="utf-8")
        (self.dir / "2.bind").write_text("version 000000000000\n", encoding="utf-8")
        with self.assertRaises(SystemExit) as caught:
            guard.require_fresh_binds(str(self.dir))
        message = str(caught.exception.code)
        self.assertIn("1 of 2 bindings", message)
        self.assertIn("2.bind", message)
        self.assertIn("Regenerate them", message)


if __name__ == "__main__":
    unittest.main()
