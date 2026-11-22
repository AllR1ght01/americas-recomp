"""Merge the per-shader results of the whole corpus into docs/xbox/results.json (the input of make_progress_svg.py).

usage: python -I make_full_results.py --manifest <manifest_full.csv> --results <results.json>
       [--graphics <results_final_gfx.json>] --corpus <results_full_corpus.json> [--out <results.json>]

Reads the graphics and corpus result lists (written by run_full.py), plus the stage column of the manifest.
Compute keeps the earlier per-shader statuses (they include the fake-memory variants) and adds the compute
shaders of the corpus run. Pixel: 'strict' if valid without the f16 opt-in, 'relaxed' if only with it, else
'fail'. Geometry shaders have no driver mode and are not in any section. With no --out the results file is
updated in place, as before.
"""
import argparse, collections, csv, json, sys

OK = {"VALID", "VALID_BARY_EXT"}


def read_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def merge(results, manifest, graphics, corpus):
    """Return results.json with its compute/pixel/vertex/mesh sections rebuilt from the three inputs."""
    r = read_json(results)
    man = {str(x["idx"]): x["stage"] for x in csv.DictReader(open(manifest, encoding="utf-8", newline=""))}
    res = {}
    for path in (graphics, corpus):
        for e in read_json(path):
            res[(str(e["idx"]), bool(e.get("relaxed", False)))] = e["status"]
    for e in read_json(corpus):
        idx = str(e["idx"])
        if man.get(idx) == "CS":
            r["compute"][idx] = "valid" if e["status"] in OK else "fail"
    pixel, vertex, mesh = {}, {}, {}
    for idx, stage in man.items():
        strict = res.get((idx, False)) == "VALID"
        relaxed = res.get((idx, False)) in OK or res.get((idx, True)) in OK
        if stage == "PS":
            pixel[idx] = "strict" if strict else ("relaxed" if relaxed else "fail")
        elif stage == "VS":
            vertex[idx] = "strict" if strict else "fail"
        elif stage == "?":
            mesh[idx] = "valid" if strict else "fail"
    r["pixel"], r["vertex"], r["mesh"] = pixel, vertex, mesh
    return r


def main(argv=None):
    ap = argparse.ArgumentParser(description="merge corpus results into results.json")
    ap.add_argument("--manifest", required=True, help="manifest_full.csv with idx and stage columns")
    ap.add_argument("--results", required=True, help="results.json to read (and update unless --out is given)")
    ap.add_argument("--graphics", required=True, help="results_final_gfx.json")
    ap.add_argument("--corpus", required=True, help="results_full_corpus.json")
    ap.add_argument("--out", help="write here instead of updating --results")
    a = ap.parse_args(argv)
    out = merge(a.results, a.manifest, a.graphics, a.corpus)
    path = a.out or a.results
    with open(path, "w", encoding="utf-8") as f:
        json.dump(out, f)
    for k in ("compute", "pixel", "vertex", "mesh"):
        print(k, len(out[k]), dict(collections.Counter(out[k].values())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
