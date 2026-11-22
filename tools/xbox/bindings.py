"""Static binding analysis for Xbox compute shaders (straight-line prologue approximation).

For one code blob (<idx>.bin, prologue stripped) find the s_load descriptor/pointer loads from user-SGPR pointer
tables, classify what each loaded range is used as (constant buffer, typed buffer, image, sampler, nested pointer)
and write <idx>.bind for driver.exe:

    node <id> user <sgpr>                 table reached through user SGPR <sgpr> (a 32-bit pointer)
    node <id> child <parent> <byteoffset> table reached through a pointer loaded from parent+offset
    desc <node> <byteoffset> <class>      class in {buf, img, samp}

Approximations (documented, not hidden): linear scan from the entry, so registers overwritten later or loads in
other basic blocks are not tracked precisely; unclassified loads are left zero.
"""
import sys, struct, os, collections, hashlib

# Every .bind starts with "version <VERSION>": a hash of this file, so a runner can refuse bindings written by an older or edited
# generator (harness_guard.py). driver.exe and run_real.py skip lines they do not know.
VERSION = hashlib.sha256(open(os.path.abspath(__file__), 'rb').read().replace(b'\r\n', b'\n')).hexdigest()[:12]   # line-ending neutral
sys.path.insert(0, '.')
sys.path.insert(1, os.environ.get("XBOX_RDNA2_DIR", r'E:/Programming/rpf9shaders/out_disasm'))
try:
    if os.environ.get("XBOX_RDNA2") == "recomp":
        raise ImportError
    import rdna2
except ImportError:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import rdna2_recomp as rdna2

SMEM_LOAD = {'s_load_dword': 1, 's_load_dwordx2': 2, 's_load_dwordx4': 4, 's_load_dwordx8': 8, 's_load_dwordx16': 16}
SMEM_BUF = {'s_buffer_load_dword': 1, 's_buffer_load_dwordx2': 2, 's_buffer_load_dwordx4': 4, 's_buffer_load_dwordx8': 8, 's_buffer_load_dwordx16': 16}


def user_sgprs(stage):
    """How many SGPRs (from s0) can hold user data. Mesh shaders (manifest stage '?', merged NGG): s0-s7 are system/hidden words and
    user data runs to s39, so 40. Everything else: 16 (the harness driver passes 16 words for those stages), except the export halves
    of legacy GS pipelines (export_half), which are merged programs like the mesh shaders."""
    return 40 if stage in ('?', 'MS', 'GS') else 16


def export_half(ins):
    """A VS that ends with s_barrier and s_setpc_b64 into the GS half (run_full.esgs_item): s0-s7 are system words, user data runs
    from s8 to s39, and the harness driver passes it that way."""
    names = [i.name for i in ins if i.name != 's_endpgm']
    return names[-2:] == ['s_barrier', 's_setpc_b64']


def analyse(path, nuser=16):
    data = open(path, 'rb').read()
    ins, _ = rdna2.sweep(data, 0, len(data))
    if export_half(ins):
        nuser = max(nuser, 40)
    tag = {r: ('user', r) for r in range(0, nuser)}
    nodes = {}          # key -> id
    node_lines = []
    loads = []          # dict(node, off, d, n)
    out = set()
    child_of = {}       # (node, off) -> node id

    def use(reg, width, cls):
        t = tag.get(reg)
        if t is None or t[0] != 'data':
            return
        ld = loads[t[1]]
        # the whole width must come from the same load, consecutive dwords
        for j in range(width):
            tj = tag.get(reg + j)
            if tj is None or tj[0] != 'data' or tj[1] != t[1] or tj[2] != t[2] + j:
                return
        out.add((ld['node'], ld['off'] + 4 * t[2], cls))

    def user_node(k):
        key = ('user', k)
        if key not in nodes:
            nodes[key] = len(nodes)
            node_lines.append(f'node {nodes[key]} user {k}')
        return nodes[key]

    def child_node(parent, off):
        key = ('child', parent, off)
        if key not in nodes:
            nodes[key] = len(nodes)
            node_lines.append(f'node {nodes[key]} child {parent} {off}')
        return nodes[key]

    def base_node(reg):
        t = tag.get(reg)
        if t is None:
            return None
        if t[0] == 'user':
            return user_node(t[1])
        if t[0] == 'data':
            ld = loads[t[1]]
            if t[2] == 0 and ld['n'] in (1, 2):
                ld['ptr'] = True
                return child_node(ld['node'], ld['off'])
        return None

    for i in ins:
        w0 = i.words[0]
        w1 = i.words[1] if len(i.words) > 1 else 0
        name = i.name
        fmt = i.fmt.split('+')[0]
        if fmt == 'SMEM':
            if name in SMEM_LOAD:
                n = SMEM_LOAD[name]
                sbase = (w0 & 0x3f) * 2
                sdata = (w0 >> 6) & 0x7f
                off = w1 & 0x1fffff
                node = base_node(sbase)
                if node is not None:
                    loads.append(dict(node=node, off=off, d=sdata, n=n))
                    for k in range(n):
                        tag[sdata + k] = ('data', len(loads) - 1, k)
                else:
                    for k in range(n):
                        tag.pop(sdata + k, None)
            elif name in SMEM_BUF:
                n = SMEM_BUF[name]
                sbase = (w0 & 0x3f) * 2
                sdata = (w0 >> 6) & 0x7f
                use(sbase, 4, 'buf')
                for k in range(n):
                    tag.pop(sdata + k, None)
            continue
        if fmt in ('MUBUF', 'MTBUF'):
            use(((w1 >> 16) & 0x1f) * 4, 4, 'buf')
            continue
        if fmt == 'MIMG':
            srsrc = ((w1 >> 16) & 0x1f) * 4
            ssamp = ((w1 >> 21) & 0x1f) * 4
            if 'bvh' in name:
                continue
            # XBOX-FORK: atomic image ops need a 32-bit uint/sint/float format; mark the class so the driver builds one
            dmask = (w0 >> 8) & 0xf
            atomic64 = 'atomic' in name and ((dmask == 0xf) if 'cmpswap' in name else (dmask == 0x3))  # 64-bit data: 2 dwords (4 for cmpswap)
            use(srsrc, 8, 'img%d%s' % ((w0 >> 3) & 7, 'b' if atomic64 else 'a' if 'atomic' in name else ''))
            if 'sample' in name or 'gather' in name or 'get_lod' in name:
                use(ssamp, 4, 'samp')
            continue
        if fmt == 'SOP1' and name in ('s_mov_b32',):
            dst = (w0 >> 16) & 0x7f
            src = w0 & 0xff
            if src == 128:
                tag[dst] = ('zero',)
            elif src in tag:
                tag[dst] = tag[src]
            else:
                tag.pop(dst, None)
            continue
        if fmt in ('SOP1', 'SOP2', 'SOPK'):
            dst = (w0 >> 16) & 0x7f
            if fmt == 'SOPK':
                dst = (w0 >> 16) & 0x7f
            tag.pop(dst, None)
            if name.endswith('b64'):
                tag.pop(dst + 1, None)

    lines = ['version ' + VERSION] + list(node_lines)
    for node, off, cls in sorted(out):
        lines.append(f'desc {node} {off} {cls}')
    stats = collections.Counter(c for _, _, c in out)
    return lines, stats, len(loads), len(nodes)


def _write(job):
    src, dst, f, nuser = job
    lines, st, nl, nn = analyse(os.path.join(src, f), nuser)
    open(os.path.join(dst, f[:-4] + '.bind'), 'w').write('\n'.join(lines) + '\n')
    return st, nl


if __name__ == '__main__':
    # usage: bindings.py <bins dir> <binds dir>   (all .bin files, in worker processes; stage from the manifest beside the bins dir)
    import concurrent.futures as cf
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    total = collections.Counter()
    nloads = 0
    names = [f for f in os.listdir(src) if f.endswith('.bin')]
    # stages from the manifest next to the bins dir (manifest_full.csv or manifest_all.csv); unknown -> 16 user SGPRs
    stage = {}
    for m in ('manifest_full.csv', 'manifest_all.csv', 'manifest.csv'):
        p = os.path.join(os.path.dirname(os.path.abspath(src)), m)
        if os.path.exists(p):
            import csv
            stage.update({r['idx']: r['stage'] for r in csv.DictReader(open(p)) if r['idx'] not in stage})
    with cf.ProcessPoolExecutor(max(1, (os.cpu_count() or 4) - 2)) as ex:
        for st, nl in ex.map(_write, [(src, dst, f, user_sgprs(stage.get(f[:-4], ''))) for f in names], chunksize=64):
            total.update(st)
            nloads += nl
    print(len(names), 'shaders;', nloads, 'descriptor/pointer loads; classified descriptors:', dict(total), '; bindings.py', VERSION)
