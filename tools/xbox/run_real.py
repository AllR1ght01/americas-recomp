"""Run recompiled real compute shaders on the host GPU (exec.exe) with synthetic inputs and compare configurations.

There is no reference for a real shader's results, so the checks are:
  * status: compiled / harness-unsupported / faulted / timed out / ran
  * determinism: the same build run twice gives byte-identical memory and images
  * equivalence: a build with the structurizer optimisations switched off (XBOX_NO_STABLE_IDS, XBOX_NO_SPLIT_CURSOR) gives the same bytes
  * effect: whether the run changed any buffer or image at all (equality of untouched memory proves nothing)
usage: run_real.py <out.json> [--max-bytes N] [--limit N] [--ids a,b,c] [--workers N] [--wave 32]
"""
import concurrent.futures as cf, csv, json, os, random, struct, subprocess, sys, tempfile, time

ROOT = os.environ.get("XBOX_CORPUS_DIR", r"E:/Programming/rpf9shaders/out_disasm/anyps5/")
BINS_DIR = os.environ.get("XBOX_BINS_DIR", os.path.join(ROOT, "bins_all"))
BINDS_DIR = os.environ.get("XBOX_BINDS_DIR", os.path.join(ROOT, "binds_all"))
MANIFEST = os.environ.get("XBOX_MANIFEST", os.path.join(ROOT, "manifest_all.csv"))
EXEC = os.environ.get("XBOX_EXEC", r"E:/Programming/anyps5_src/build/exec.exe")
USER_DWORDS = 10
NODE_BYTES = 1 << 16
DATA_BYTES = 1 << 20
IMG = 64
IMG3D = 16
ARRAY_LAYERS = 4

def _blob(profile="mixed"):
    """seeded input data: 'mixed' = floats at even dwords and small integers at odd dwords (exercises both kinds of code, but the
    integers are denormals when read as floats); 'float' = normal floats only; 'int' = small integers only"""
    rnd = random.Random(12345)
    words = bytearray()
    for i in range(DATA_BYTES // 4):
        f = lambda: struct.pack('<f', 0.25 + rnd.random() * 3.75)
        n = lambda: struct.pack('<I', rnd.randrange(256))
        words += {"mixed": (f if i % 2 == 0 else n), "float": f, "int": n}[profile]()
    return bytes(words)

BLOBS = {p: _blob(p) for p in ("mixed", "float", "int")}
BLOB = BLOBS["mixed"]

def parse_bind(path):
    nodes, descs = {}, []
    for line in open(path):
        w = line.split()
        if not w:
            continue
        if w[0] == "node":
            nodes[int(w[1])] = ("user", int(w[3])) if w[2] == "user" else ("child", int(w[3]), int(w[4]))
        elif w[0] == "desc":
            descs.append((int(w[1]), int(w[2]), w[3]))
    return nodes, descs

def build(idx, wave, workdir, threads=None, profile="mixed"):
    """returns (scenario lines, info dict) or raises Unsupported"""
    nodes, descs = parse_bind(os.path.join(BINDS_DIR, f"{idx}.bind"))
    rnd = random.Random(int(idx))
    user_dwords = max([s[1] + 1 for s in nodes.values() if s[0] == "user"] or [0])
    threads = threads or workgroup_threads(idx, wave)
    lines = [f"wave {wave}", "groups 2 2 1", f"threads {threads} 1 1", f"code {os.path.join(BINS_DIR, f'{idx}.bin')}", f"out {workdir}/out", f"userdwords {user_dwords}", "lds 4096", "lowguest"]
    info = {"data": [], "images": []}
    for nid in nodes:
        lines.append(f"region {100 + nid} {NODE_BYTES}")
    for kind in nodes.values():
        pass
    for nid, spec in nodes.items():
        if spec[0] == "user":
            lines.append(f"user32 {spec[1]} {100 + nid}")
        else:
            parent, off = spec[1], spec[2]
            lines.append(f"ptrlo {100 + parent} {off} {100 + nid}")
            lines.append(f"ptrhi {100 + parent} {off + 4} {100 + nid} 0")
    data_id, image_id = 200, 1
    for node, off, cls in descs:
        if node not in nodes:
            continue
        base = 100 + node
        if cls == "buf":
            p = os.path.join(workdir, f"data{data_id}.bin")
            open(p, "wb").write(BLOBS[profile])
            lines.append(f"region {data_id} {DATA_BYTES} init {p}")
            lines += [f"ptrlo {base} {off} {data_id}", f"ptrhi {base} {off + 4} {data_id} {16 << 16}",
                      f"poke {base} {off + 8} {DATA_BYTES // 16}", f"poke {base} {off + 12} 0x1004dfac"]
            info["data"].append(data_id)
            data_id += 1
        elif cls in ("img0", "img1", "img2", "img5"):
            ip = os.path.join(workdir, f"img{image_id}.bin")
            irnd = random.Random(1000 + image_id)
            if cls == "img0":
                open(ip, "wb").write(b"".join(struct.pack('<f', 0.25 + irnd.random() * 3.75) for _ in range(IMG * 4)))
                lines += [f"image {image_id} {IMG} 1 dim 1d init {ip}", f"timg {base} {off} {image_id}"]
            elif cls == "img2":
                open(ip, "wb").write(b"".join(struct.pack('<f', 0.25 + irnd.random() * 3.75) for _ in range(IMG3D * IMG3D * IMG3D * 4)))
                lines += [f"image {image_id} {IMG3D} {IMG3D} dim 3d depth {IMG3D} init {ip}", f"timg {base} {off} {image_id}"]
            else:
                layers = ARRAY_LAYERS if cls == "img5" else 1
                open(ip, "wb").write(b"".join(struct.pack('<f', 0.25 + irnd.random() * 3.75) for _ in range(IMG * IMG * 4 * layers)))
                lines += [f"image {image_id} {IMG} {IMG}" + (f" layers {layers}" if layers > 1 else "") + f" init {ip}", f"timg {base} {off} {image_id}"]
            info["images"].append(image_id)
            image_id += 1
        elif cls.startswith("img"):
            raise Unsupported("image class " + cls)
    return lines, info

_RDNA = None
def workgroup_threads(idx, wave):
    """The real numthreads is not recoverable from the container (PSV0 is zeroed, DXIL strings stripped).
    Guess: two waves when the code uses LDS or a barrier, otherwise one wave."""
    global _RDNA
    if _RDNA is None:
        try:
            if os.environ.get("XBOX_RDNA2") == "recomp":
                raise ImportError
            sys.path.insert(0, os.path.join(ROOT, ".."))
            import rdna2
            _RDNA = rdna2
        except ImportError:
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            import rdna2_recomp as rdna2
            _RDNA = rdna2
    d = open(os.path.join(BINS_DIR, f"{idx}.bin"), "rb").read()
    ins, _ = _RDNA.sweep(d, 0, len(d))
    return 2 * wave if any(i.text.startswith(("ds_", "s_barrier")) for i in ins) else wave

class Unsupported(Exception):
    pass

def run_once(idx, wave, env_extra, timeout, tag, threads=None):
    d = tempfile.mkdtemp(prefix=f"real{idx}{tag}_")
    try:
        lines, info = build(idx, wave, d, threads)
    except Unsupported as e:
        return {"status": "UNSUPPORTED", "detail": str(e)}
    sc = os.path.join(d, "scenario.txt")
    open(sc, "w").write("\n".join(lines) + "\n")
    env = dict(os.environ, **env_extra)
    t0 = time.time()
    try:
        p = subprocess.run([EXEC, sc], capture_output=True, text=True, timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        return {"status": "TIMEOUT", "detail": f">{timeout}s"}
    out = p.stdout.strip()
    secs = round(time.time() - t0, 2)
    if out.splitlines()[-1:] == ["RAN"]:
        res = {"status": "RAN", "secs": secs, "dir": d}
        res["blob"] = b"".join(open(f"{d}/out/region{r}.bin", "rb").read() for r in info["data"]) + b"".join(open(f"{d}/out/image{i}.bin", "rb").read() for i in info["images"])
        initial = b"".join(open(f"{d}/data{r}.bin", "rb").read() for r in info["data"]) + b"".join(open(f"{d}/img{i}.bin", "rb").read() for i in info["images"])
        res["changed"] = res["blob"] != initial
        return res
    last = out.splitlines()[-1] if out else (p.stderr.strip()[-200:] or f"exit {p.returncode}")
    if "FAULT" in out:
        return {"status": "FAULT", "detail": [l for l in out.splitlines() if l.startswith("FAULT")][0][:120], "secs": secs}
    return {"status": "COMPILE_FAIL" if "FAIL" in out else "CRASH", "detail": last[:300], "secs": secs}

def one(idx, wave, timeout):
    threads = None
    a = run_once(idx, wave, {}, timeout, "a")
    rec = {"idx": idx, "status": a["status"], "detail": a.get("detail", ""), "secs": a.get("secs")}
    if a["status"] != "RAN":
        return rec
    a2 = run_once(idx, wave, {}, timeout, "a2")
    det = a2["status"] == "RAN" and a2["blob"] == a["blob"]
    if not det:
        # numthreads is not recoverable from the container: a shader that reads workgroup memory nobody wrote is
        # flaky at the wrong size, so try larger workgroups and keep the first deterministic one
        for t in (64, 128, 256, 512):
            if t <= workgroup_threads(idx, wave):
                continue
            c = run_once(idx, wave, {}, timeout, "c", t)
            c2 = run_once(idx, wave, {}, timeout, "c2", t) if c["status"] == "RAN" else None
            if c["status"] == "RAN" and c2["status"] == "RAN" and c["blob"] == c2["blob"]:
                a, det, threads = c, True, t
                break
    b = run_once(idx, wave, {"XBOX_NO_STABLE_IDS": "1", "XBOX_NO_SPLIT_CURSOR": "1"}, timeout, "b", threads)
    rec["changed"] = a["changed"]
    rec["deterministic"] = det
    rec["threads"] = threads or workgroup_threads(idx, wave)
    rec["same_without_structurizer_opts"] = None if b["status"] != "RAN" else (b["blob"] == a["blob"])
    if b["status"] != "RAN":
        rec["b_status"] = b["status"]
    return rec

def main():
    argv = sys.argv[1:]
    out = argv[0]
    opt = lambda name, default: argv[argv.index(name) + 1] if name in argv else default
    max_bytes, limit, workers, wave = int(opt("--max-bytes", 20000)), int(opt("--limit", 100000)), int(opt("--workers", 6)), int(opt("--wave", 32))
    timeout = int(opt("--timeout", 240))
    rows = [r for r in csv.DictReader(open(MANIFEST)) if r["stage"] == "CS" and int(r.get("bytes") or os.path.getsize(os.path.join(BINS_DIR, f"{r['idx']}.bin"))) <= max_bytes]
    ids = opt("--ids", "").split(",") if "--ids" in argv else [r["idx"] for r in rows]
    ids = [i for i in ids if os.path.exists(os.path.join(BINDS_DIR, f"{i}.bind"))][:limit]
    results = []
    with cf.ThreadPoolExecutor(workers) as ex:
        for rec in ex.map(lambda i: one(i, wave, timeout), ids):
            results.append(rec)
            if len(results) % 25 == 0:
                print(len(results), "of", len(ids), flush=True)
    json.dump(results, open(out, "w"))
    import collections
    c = collections.Counter(r["status"] for r in results)
    print(dict(c))
    ran = [r for r in results if r["status"] == "RAN"]
    print("ran", len(ran), "changed memory", sum(1 for r in ran if r.get("changed")), "deterministic", sum(1 for r in ran if r.get("deterministic")),
          "same without structurizer opts", sum(1 for r in ran if r.get("same_without_structurizer_opts")), "differ", [r["idx"] for r in ran if r.get("same_without_structurizer_opts") is False])

if __name__ == "__main__":
    main()
