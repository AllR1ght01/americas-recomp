import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "xbox/platform/xgr/status_json.py"


class StatusJson(unittest.TestCase):
    def run_status(self, table, sources):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            runtime = root / "runtime"
            runtime.mkdir()
            for name, text in sources.items():
                (runtime / name).write_text(text, encoding="utf-8")
            table_path = root / "table.json"
            out_path = root / "out.json"
            table_path.write_text(json.dumps({"interfaces": table}), encoding="utf-8")
            run = subprocess.run(
                [sys.executable, "-I", str(SCRIPT), str(table_path), str(out_path), str(runtime)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(run.returncode, 0, run.stderr)
            written = json.loads(out_path.read_text(encoding="utf-8"))
            return written, run.stdout, str(out_path)

    def assert_summary(self, written, stdout, out_path):
        interfaces = len(written)
        methods = sum(len(rows) for rows in written.values())
        implemented = sum(1 for rows in written.values() for row in rows if row[1])
        self.assertEqual(
            stdout.strip(),
            "%d interfaces, %d methods, %d implemented -> %s" % (interfaces, methods, implemented, out_path),
        )

    def test_both_override_forms_mark_implemented_and_missing_names_stay_false(self):
        written, stdout, out_path = self.run_status(
            [
                {
                    "name": "IXOne",
                    "methods": [
                        {"name": "XAlpha"},
                        {"name": "XMissing"},
                        {"name": "XBeta"},
                    ],
                },
                {"name": "IXTwo", "methods": [{"name": "XGamma"}]},
            ],
            {
                "impl_a.cpp": '{"XAlpha", reinterpret_cast<void*>(&XAlpha_impl)},\n',
                "impl_b.cpp": 'GTX_OV("XBeta", v_Beta),\n{"YSkip", reinterpret_cast<void*>(&no)},\n',
                "notes.txt": '{"XGamma", reinterpret_cast<void*>(&not_cpp)},\n',
            },
        )
        self.assertEqual(
            written,
            {
                "IXOne": [["XAlpha", True], ["XMissing", False], ["XBeta", True]],
                "IXTwo": [["XGamma", False]],
            },
        )
        self.assert_summary(written, stdout, out_path)

    def test_unnamed_methods_are_omitted_even_when_overridden(self):
        written, stdout, out_path = self.run_status(
            [
                {
                    "name": "IXOne",
                    "methods": [
                        {"name": "XKept", "unnamed": False},
                        {"name": "XHidden", "unnamed": True},
                    ],
                }
            ],
            {"impl.cpp": '{"XHidden", reinterpret_cast<void*>(&XHidden_impl)},\nGTX_OV("XKept", v_Kept),\n'},
        )
        self.assertEqual(written, {"IXOne": [["XKept", True]]})
        self.assertNotIn("XHidden", json.dumps(written))
        self.assert_summary(written, stdout, out_path)

    def test_interface_without_named_methods_is_omitted(self):
        written, stdout, out_path = self.run_status(
            [
                {"name": "IXEmpty", "methods": []},
                {"name": "IXPadding", "methods": [{"name": "XPad", "unnamed": True}]},
                {"name": "IXKept", "methods": [{"name": "XOnly"}]},
            ],
            {},
        )
        self.assertEqual(written, {"IXKept": [["XOnly", False]]})
        self.assert_summary(written, stdout, out_path)

    def test_runtime_argument_does_not_read_the_repository_sources(self):
        written, stdout, out_path = self.run_status(
            [{"name": "IXUser", "methods": [{"name": "XUserGetGamertag"}]}],
            {},
        )
        self.assertEqual(written, {"IXUser": [["XUserGetGamertag", False]]})
        self.assert_summary(written, stdout, out_path)


if __name__ == "__main__":
    unittest.main()
