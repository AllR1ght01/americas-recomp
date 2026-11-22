"""Write docs/xbox/xgr_status.json: for every xgameruntime interface, which of its methods have a real implementation in runtime/ (the rest log and fail).
usage: python -I status_json.py <table.json from idl_table.py> <out.json> [<runtime directory>]
Implemented = the method name appears as an override entry ({"Name", ...}) in the runtime sources. Names are public GDK API names. The runtime directory defaults to runtime/ next to this script."""
import glob, json, os, re, sys
here = os.path.dirname(os.path.abspath(__file__))
runtime = sys.argv[3] if len(sys.argv) > 3 else os.path.join(here, "runtime")
impl = set()
for f in glob.glob(os.path.join(runtime, "*.cpp")):
    src = open(f, encoding="utf-8").read()
    impl.update(re.findall(r'\{"(X\w+)",\s*reinterpret', src))      # impl.cpp, impl_user.cpp
    impl.update(re.findall(r'GTX_OV\("(X\w+)"', src))               # xtask.cpp
t = json.load(open(sys.argv[1]))["interfaces"]
out = {}
for i in t:
    ms = [[m["name"], m["name"] in impl] for m in i["methods"] if not m.get("unnamed")]
    if ms:
        out[i["name"]] = ms
json.dump(out, open(sys.argv[2], "w"), indent=0)
n = sum(len(v) for v in out.values()); d = sum(1 for v in out.values() for m in v if m[1])
print("%d interfaces, %d methods, %d implemented -> %s" % (len(out), n, d, sys.argv[2]))
