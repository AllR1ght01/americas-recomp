"""Compare the GPU result of a recompiled real compute shader with the CPU interpreter (cpu_ref.py).

One wave64 workgroup, scenario from run_real.build. Outcomes per shader:
  MATCH        every output buffer byte equal (or within the float tolerance for shaders using approximate transcendentals)
  MISMATCH     the GPU and the CPU reference disagree (a finding to investigate: reference bug or recompiler bug)
  SKIP         interpreter lacks an instruction, or the shader uses images / unsupported scenario parts
  GPU_FAIL     the harness could not run it
usage: cpu_compare.py <out.json> [--ids a,b] [--max-bytes N] [--workers N] [--steps N] [--gpu-timeout S]
Runs in worker PROCESSES (the interpreter is pure Python, so threads would share one core). --steps caps the CPU run (default
100,000 wave steps; the largest compared shader needs about 10,000) and --gpu-timeout the GPU run (default 30 s); shaders that hit
either are SKIP / GPU_FAIL. Each record has "secs", the wall time it took.
"""
import concurrent.futures as cf, json, os, shutil, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_real as R
import cpu_ref as C

WAVE = 64


def scenario_memory(lines):
    """rebuild the memory image exactly as exec.cpp does from the scenario lines"""
    regions = {}
    for l in lines:
        w = l.split()
        if w[0] == "region":
            size = int(w[2], 0)
            data = bytearray(size)
            if len(w) > 3 and w[3] == "init":
                blob = open(w[4], "rb").read()
                data[:len(blob)] = blob[:size]
            regions[int(w[1])] = data
    guest = {rid: 0x10000000 + 0x1000000 * k for k, rid in enumerate(sorted(regions))}
    sgpr = {}
    images = {}
    for l in lines:
        w = l.split()
        if w[0] == "image":
            iid, wd, ht = int(w[1]), int(w[2]), int(w[3])
            dim_s = w[w.index("dim") + 1] if "dim" in w else "2d"
            depth = int(w[w.index("depth") + 1]) if "depth" in w else 1
            layers = int(w[w.index("layers") + 1]) if "layers" in w else 1
            if dim_s == "2d" and layers > 1:
                dim_s = "2da"
            slices = depth if dim_s == "3d" else layers
            n = wd * ht * 4 * slices
            data = list(struct.unpack('<%dI' % n, open(w[w.index("init") + 1], "rb").read())) if "init" in w else [0] * n
            images[iid] = {"w": wd, "h": ht, "depth": depth, "layers": layers, "dim": dim_s, "data": data, "written": {}}
    def put(rid, off, v):
        struct.pack_into('<I', regions[rid], off, v & 0xffffffff)
    for l in lines:
        w = l.split()
        if w[0] == "ptrlo":
            put(int(w[1]), int(w[2], 0), guest[int(w[3])] + (int(w[4], 0) if len(w) > 4 else 0))
        elif w[0] == "ptrhi":
            put(int(w[1]), int(w[2], 0), ((guest[int(w[3])] >> 32) & 0xffff) | int(w[4], 0))
        elif w[0] == "poke":
            put(int(w[1]), int(w[2], 0), int(w[3], 0))
        elif w[0] == "user32":
            sgpr[int(w[1])] = guest[int(w[2])]
        elif w[0] == "userval":
            sgpr[int(w[1])] = int(w[2], 0)
        elif w[0] == "timg":
            im = images[int(w[3])]
            fake = 0x7000000000 + 0x1000000 * int(w[3])
            o = int(w[2], 0)
            tcode = {"1d": 8, "2d": 9, "3d": 10, "cube": 11, "2da": 13}[im["dim"]]
            dword4 = (im["depth"] - 1) if im["dim"] == "3d" else ((im["layers"] - 1) if im["layers"] > 1 else 0)
            for k, v in enumerate([fake >> 8, ((fake >> 40) & 0xff) | (77 << 20) | (3 << 30), (im["w"] - 1) | ((im["h"] - 1) << 14), 0xfac | (tcode << 28), dword4, 0, 0, 0]):
                put(int(w[1]), o + 4 * k, v)
        elif w[0] in ("user", "ptr8lo", "ptr8hi"):
            raise C.Unsupported("scenario keyword " + w[0])
    mem = C.Memory()
    for rid, d in regions.items():
        mem.add(guest[rid], d)
    return regions, sgpr, images


def close(a, b):
    if a == b:
        return True
    if (a ^ b) & 0x80000000 and not ((a | b) & 0x7fffffff == 0):
        return False
    return abs((a & 0x7fffffff) - (b & 0x7fffffff)) <= 4 and ((a >> 23) & 0xff) not in (0, 0xff)


def one_profile(idx, profile):
    d = tempfile.mkdtemp(prefix=f"cmp{idx}_")
    try:
        return _one_profile_in(idx, profile, d)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def _one_profile_in(idx, profile, d):
    try:
        lines, info = R.build(idx, WAVE, d, WAVE, profile)
    except R.Unsupported as e:
        return {"idx": idx, "outcome": "SKIP", "detail": str(e)}
    lines = [("groups 1 1 1" if l.startswith("groups") else l) for l in lines]
    code = open(os.path.join(R.BINS_DIR, f"{idx}.bin"), "rb").read()
    # CPU side first: cheap, and decides whether the shader is interpretable at all
    try:
        regions, sgpr, images = scenario_memory(lines)
        mem = C.Memory()
        guest = {rid: 0x10000000 + 0x1000000 * k for k, rid in enumerate(sorted(regions))}
        for rid, dd in regions.items():
            mem.add(guest[rid], dd)
        wave = C.Wave(code, mem, sgpr)
        wave.images = images
        wave.run(int(os.environ.get("CPU_COMPARE_STEPS", 100_000)))
    except C.Unsupported as e:
        return {"idx": idx, "outcome": "SKIP", "detail": str(e)[:100]}
    except (KeyError, IndexError, ValueError, struct.error, OverflowError, ZeroDivisionError) as e:
        return {"idx": idx, "outcome": "SKIP", "detail": "interpreter error " + type(e).__name__ + " " + str(e)[:60]}
    sc = os.path.join(d, "scenario.txt")
    open(sc, "w").write("\n".join(lines) + "\n")
    try:
        p = subprocess.run([R.EXEC, sc], capture_output=True, text=True, timeout=float(os.environ.get("CPU_COMPARE_GPU_TIMEOUT", 30)))
    except subprocess.TimeoutExpired:
        return {"idx": idx, "outcome": "GPU_FAIL", "detail": "timeout"}
    out = p.stdout.strip().splitlines()
    if out[-1:] != ["RAN"]:
        return {"idx": idx, "outcome": "GPU_FAIL", "detail": (out[-1] if out else p.stderr[-100:])[:150]}
    bad = []
    changed = False
    for r in info["data"]:
        gpu = open(f"{d}/out/region{r}.bin", "rb").read()
        cpu = bytes(regions[r])
        initial = open(f"{d}/data{r}.bin", "rb").read()
        changed |= cpu != initial
        if gpu == cpu:
            continue
        for k in range(0, len(gpu), 4):
            a, b = struct.unpack_from('<I', gpu, k)[0], struct.unpack_from('<I', cpu, k)[0]
            if a != b and not (wave.approx and close(a, b)):
                bad.append((r, k, a, b))
    for iid in info["images"]:
        gpu = open(f"{d}/out/image{iid}.bin", "rb").read()
        cpu = struct.pack('<%dI' % len(images[iid]["data"]), *images[iid]["data"])
        changed |= cpu != open(f"{d}/img{iid}.bin", "rb").read()
        for k in range(0, len(gpu), 4):
            a, b = struct.unpack_from('<I', gpu, k)[0], struct.unpack_from('<I', cpu, k)[0]
            if a != b and not (wave.approx and close(a, b)):
                bad.append((1000 + iid, k, a, b))
    rec = {"idx": idx, "profile": profile, "outcome": "MISMATCH" if bad else "MATCH", "cpu_changed_memory": changed, "approx": wave.approx, "steps": wave.steps}
    if bad:
        rec["ndiff"] = len(bad)
        rec["first"] = [(r, k, hex(a), hex(b)) for r, k, a, b in bad[:4]]
    return rec


PROFILES = ("float", "int")


def one(idx):
    import time
    t = time.time()
    rec = _one(idx)
    rec["secs"] = round(time.time() - t, 2)
    return rec


def _one(idx):
    """run each input profile; a shader MISMATCHes if any profile does, MATCHes if at least one profile ran and none differ"""
    code = open(os.path.join(R.BINS_DIR, f"{idx}.bin"), "rb").read()
    ins, _ = C.rdna2.sweep(code, 0, len(code))
    has_float = any(("_f32" in i.name or "_f16" in i.name) for i in ins)
    recs = [one_profile(idx, p) for p in (("float",) if has_float else PROFILES)]
    bad = [r for r in recs if r["outcome"] == "MISMATCH"]
    if bad:
        return bad[0]
    ok = [r for r in recs if r["outcome"] == "MATCH"]
    if ok:
        best = dict(ok[0])
        best["cpu_changed_memory"] = any(r["cpu_changed_memory"] for r in ok)
        best["profiles_matched"] = [r["profile"] for r in ok]
        return best
    return recs[0]


def main():
    argv = sys.argv[1:]
    opt = lambda name, default: argv[argv.index(name) + 1] if name in argv else default
    import csv
    max_insns = int(opt("--max-bytes", 800))
    rows = [r for r in csv.DictReader(open(R.MANIFEST)) if r["stage"] == "CS" and int(r.get("bytes") or os.path.getsize(os.path.join(R.BINS_DIR, f"{r['idx']}.bin"))) <= max_insns]
    ids = opt("--ids", "").split(",") if "--ids" in argv else [r["idx"] for r in rows]
    ids = [i for i in ids if os.path.exists(os.path.join(R.BINDS_DIR, f"{i}.bind"))]
    import harness_guard
    harness_guard.require_fresh_binds(R.BINDS_DIR, ids)
    if "--no-canary" not in argv:
        import canary
        if not canary.compare_ok():                                   # known MATCHes must still match before the run
            sys.exit("canary failed: not starting the comparison (--no-canary overrides)")
    os.environ["CPU_COMPARE_STEPS"] = opt("--steps", "100000")          # read by the worker processes
    os.environ["CPU_COMPARE_GPU_TIMEOUT"] = opt("--gpu-timeout", "30")
    res = []
    with cf.ProcessPoolExecutor(int(opt("--workers", max(1, (os.cpu_count() or 4) - 2)))) as ex:
        for n, r in enumerate(ex.map(one, ids, chunksize=1)):
            res.append(r)
            if (n + 1) % 100 == 0:
                print(n + 1, "of", len(ids), flush=True)
    json.dump(res, open(argv[0], "w"), indent=1)
    import collections
    print(collections.Counter(r["outcome"] for r in res))
    m = [r for r in res if r["outcome"] == "MATCH"]
    print("match", len(m), "of which the shader changed memory", sum(1 for r in m if r["cpu_changed_memory"]))
    print("mismatch", [(r["idx"], r["first"][:1]) for r in res if r["outcome"] == "MISMATCH"][:20])
    c = collections.Counter(r["detail"].split(" (")[0][:60] for r in res if r["outcome"] == "SKIP")
    print(c.most_common(12))
    slow = sorted(res, key=lambda r: -r["secs"])[:5]
    print("slowest", [(r["idx"], r["secs"], r["outcome"]) for r in slow])


if __name__ == "__main__":
    main()
