"""Build xgameruntime.dll with the runtime and every test_*.cpp next to this script with MSVC, run the tests, and exit non-zero if any fails.

usage: python tools/xbox/platform/xgr/run_tests.py <dir with the Xodus *.idl> [--out build-xgr] [--only name ...]

Windows only. Finds Visual Studio with vswhere (as build_msvc.py does); run it from a normal Windows shell.
The IDLs are read to make table.json (idl_table.py) and the stub source (gen_stub.py) in the output directory; neither is committed.
Each test runs in the output directory with table.json next to it; test_save also runs a second time with `reopen`.
In GitHub Actions each FAIL line becomes an error annotation and a summary table is written to the job summary.
"""
import argparse, glob, os, pathlib, subprocess, sys, time

HERE = pathlib.Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
sys.path.insert(0, str(HERE.parents[1]))
from build_msvc import vc_env

DLL_LIBS = ["shell32.lib", "ole32.lib", "user32.lib"]
TEST_LIBS = ["advapi32.lib", "user32.lib", "ole32.lib", "shell32.lib", "runtimeobject.lib"]
TIMEOUT = 300


def run(cmd, env, cwd):
    print(">", " ".join(cmd), flush=True)
    return subprocess.run(cmd, env=env, cwd=cwd)


def annotate(name, line):
    if os.environ.get("GITHUB_ACTIONS") == "true":
        print("::error title=xgr %s::%s" % (name, line.replace("%", "%25").replace("\r", "").replace("\n", " ")), flush=True)


def run_test(exe, args, env, cwd):
    started = time.time()
    try:
        p = subprocess.run([str(exe)] + args, env=env, cwd=cwd, capture_output=True, text=True, errors="replace", timeout=TIMEOUT)
        out, code = p.stdout + p.stderr, p.returncode
    except subprocess.TimeoutExpired as e:
        out = e.stdout if isinstance(e.stdout, str) else ""
        out, code = out + "\nFAIL timed out after %d s" % TIMEOUT, None
    return out, code, time.time() - started


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("idl_dir")
    ap.add_argument("--out", default="build-xgr")
    ap.add_argument("--only", nargs="*", help="test names to run (e.g. test_system); default all")
    a = ap.parse_args()
    if os.name != "nt":
        sys.exit("run_tests.py builds with MSVC and runs Windows executables; run it on Windows.")
    out = pathlib.Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = vc_env()

    py = [sys.executable, "-I"]
    if subprocess.run(py + [str(HERE / "idl_table.py"), a.idl_dir, str(out / "table.json")]).returncode:
        sys.exit("idl_table.py failed")
    if subprocess.run(py + [str(HERE / "gen_stub.py"), str(out / "table.json"), str(out / "gen")]).returncode:
        sys.exit("gen_stub.py failed")

    sources = [str(out / "gen" / "xgr_stub.cpp")] + sorted(glob.glob(str(RUNTIME / "*.cpp"))) + sorted(glob.glob(str(RUNTIME / "gtx" / "*.cpp")))
    dll = ["cl", "/nologo", "/LD", "/O2", "/EHsc", "/std:c++20", "/DXGR_WITH_RUNTIME", "/I", str(RUNTIME), "/Fo:" + str(out) + "\\"] + sources
    dll += ["/Fe:" + str(out / "xgameruntime.dll"), "/link", "/DEF:" + str(out / "gen" / "xgameruntime.def")] + DLL_LIBS
    if run(dll, env, out).returncode:
        sys.exit("building xgameruntime.dll failed")

    tests = sorted(pathlib.Path(p) for p in glob.glob(str(HERE / "test_*.cpp")))
    if a.only:
        tests = [t for t in tests if t.stem in a.only]
    results = []
    for src in tests:
        name = src.stem
        objdir = out / "obj" / name
        objdir.mkdir(parents=True, exist_ok=True)
        cmd = ["cl", "/nologo", "/EHsc", "/std:c++20", "/Fo:" + str(objdir) + "\\", str(src), "/Fe:" + str(out / (name + ".exe")), "/link"] + TEST_LIBS
        if run(cmd, env, out).returncode:
            annotate(name, "build failed")
            results.append((name, "build failed", 0, 0.0))
            continue
        runs = [[]] + ([["reopen"]] if name == "test_save" else [])
        for args in runs:
            label = " ".join([name] + args)
            text, code, seconds = run_test(out / (name + ".exe"), args, env, out)
            print("=" * 20, label, "exit", code, "%.1fs" % seconds)
            print(text, flush=True)
            fails = [l for l in text.splitlines() if l.startswith("FAIL")]
            for l in fails:
                annotate(label, l)
            if code != 0 and not fails:
                annotate(label, "exit code %s" % code)
            results.append((label, "pass" if code == 0 else "FAIL", len(fails), seconds))

    rows = ["| test | result | FAIL lines | seconds |", "| --- | --- | --- | --- |"]
    rows += ["| %s | %s | %d | %.1f |" % r for r in results]
    print("\n".join(rows))
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write("### xgameruntime stand-in tests (MSVC, Windows)\n\n" + "\n".join(rows) + "\n")
    failed = [r[0] for r in results if r[1] != "pass"]
    if failed:
        sys.exit("failed: " + ", ".join(failed))


main()
