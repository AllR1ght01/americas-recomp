"""Lower, pin the wave size and assemble every shader_NN.ll of a directory; report which ones validate.
usage: python -I lower_all.py <dir with shader_NN.ll> <out_dir> [wave=64] [dxasm=dxasm.exe]
For each shader writes <out>/sNN.ll (lowered), <out>/wNN.ll (SM 6.6; the wave size is pinned only when wave operations are used and xbox_dxil_lower.needs_wave64 says the shader can tell the wave size) and <out>/repl_NN.dxil (signed; the file bvh_test's BVH_REPLACE_DIR reads).
A validator complaint "Flags must match usage" (declared=X, actual=Y) is repaired by rewriting the declared shader-flags word and retrying."""
import glob, os, re, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from xbox_dxil_lower import needs_wave64
src, out = sys.argv[1], sys.argv[2]
wave = sys.argv[3] if len(sys.argv) > 3 else "64"
dxasm = sys.argv[4] if len(sys.argv) > 4 else os.path.join(out, "dxasm.exe")
os.makedirs(out, exist_ok=True)
res = {}
for f in sorted(glob.glob(os.path.join(src, "shader_*.ll"))):
    n = re.search(r"shader_(\d+)", f).group(1)
    s_ll, w_ll, dxil = (os.path.join(out, p % n) for p in ("s%s.ll", "w%s.ll", "repl_%s.dxil"))
    r = subprocess.run([sys.executable, "-I", os.path.join(here, "xbox_dxil_lower.py"), f, s_ll], capture_output=True, text=True)
    if r.returncode:
        res[n] = "intrinsics left: " + r.stdout.strip().split(": ", 1)[-1]
        continue
    lowered = open(s_ll, encoding="utf-8").read()
    if "@dx.op.wave" in lowered or "@dx.op.createHandleFromHeap" in lowered:
        pin = wave if "@dx.op.wave" in lowered and needs_wave64(open(f, encoding="utf-8", errors="ignore").read()) else "0"
        r = subprocess.run([sys.executable, "-I", os.path.join(here, "dxil_sm66_wave.py"), s_ll, w_ll, pin], capture_output=True, text=True)
        if r.returncode:
            res[n] = "sm66 rewrite failed: " + (r.stderr.strip().splitlines() or ["?"])[-1]
            continue
    else:
        open(w_ll, "w", encoding="utf-8", newline="\n").write(open(s_ll, encoding="utf-8").read())
    for attempt in range(2):
        r = subprocess.run([dxasm, w_ll, dxil], capture_output=True, text=True)
        m = re.search(r"Flags declared=(\d+), actual=(\d+)", r.stdout)
        if m and attempt == 0:
            t = open(w_ll, encoding="utf-8").read()
            t = re.sub(r"(!\d+ = !\{i32 0, i64 )\d+(, i32 4)", r"\g<1>%s\g<2>" % m.group(2), t)
            open(w_ll, "w", encoding="utf-8", newline="\n").write(t)
            continue
        break
    res[n] = "ok" if r.returncode == 0 else "validation: " + " | ".join(l for l in r.stdout.splitlines() if "error" in l)[:300]
ok = sum(1 for v in res.values() if v == "ok")
for n, v in res.items():
    print(n, v)
print("%d of %d assembled and signed" % (ok, len(res)))
