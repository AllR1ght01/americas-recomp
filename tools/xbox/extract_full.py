"""Extract code blobs for EVERY shader (main run + the second run in out/tail_index.csv). Tail shaders get idx 100000+i.
Writes anyps5/bins_full/<idx>.bin and anyps5/manifest_full.csv (idx,name,stage,bytes,run). Existing bins_all files are reused by copy.
For a VS with an input table in its FSSM header it also writes anyps5/vin_full/<idx>.vin (fssm_header.vin_text), which run_full.py
passes to the driver as the fetch-subroutine VGPR layout."""
import sys, struct, csv, os, zlib, collections, shutil
sys.path.insert(0, '.')
import rdna2
sys.path.insert(1, os.path.dirname(os.path.abspath(__file__)))
from fssm_header import vin_text
T = {0: 'PS', 1: 'VS', 2: 'GS', 3: 'HS', 4: 'DS', 5: 'CS', 7: 'MS', 8: 'AS'}
rows = [dict(r, run='main') for r in csv.DictReader(open('../out/shaders_index.csv'))]
rows += [dict(idx=str(100000 + int(r['idx'])), name=r['name'], dxil_file=r['dxil_file'], run='tail') for r in csv.DictReader(open('../out/tail_index.csv'))]
os.makedirs('anyps5/bins_full', exist_ok=True)
os.makedirs('anyps5/vin_full', exist_ok=True)
have = {f[:-4] for f in os.listdir('anyps5/bins_all')}
m = open('anyps5/manifest_full.csv', 'w', newline='')
w = csv.writer(m); w.writerow(['idx', 'name', 'stage', 'bytes', 'run'])
bad = collections.Counter(); cnt = collections.Counter()
for r in rows:
    try:
        d = open('../out/' + r['dxil_file'], 'rb').read()
        st = T.get(struct.unpack_from('<I', d, 0)[0] >> 16, '?')
        i = d.find(b'XBCH'); csz = struct.unpack_from('<I', d, i + 8)[0]
        b = zlib.decompressobj().decompress(d[i + 24:i + 24 + csz])
        off, sz = struct.unpack_from('<QQ', b, 0xc0); e = off + sz
        if not (0 < off < e <= len(b)): bad['range'] += 1; continue
        pro = all((struct.unpack_from('<I', b, off + 4 * k)[0] >> 26) == 0x35 and ((struct.unpack_from('<I', b, off + 4 * k)[0] >> 16) & 0x3ff) == 0x180 for k in (4, 7))
        s0 = off + (40 if pro else 0)
        out = 'anyps5/bins_full/%s.bin' % r['idx']
        if r['idx'] in have and r['run'] == 'main':
            shutil.copyfile('anyps5/bins_all/%s.bin' % r['idx'], out); n = os.path.getsize(out)
        else:
            ins, _ = rdna2.sweep(b, s0, e)
            ends = [x for x in ins if x.name == 's_endpgm']
            e0 = ends[-1].off + 4 if ends else e
            open(out, 'wb').write(b[s0:e0]); n = e0 - s0
        if st == 'VS' and vin_text(b):
            open('anyps5/vin_full/%s.vin' % r['idx'], 'w', newline='\n').write(vin_text(b))
        w.writerow([r['idx'], r['name'], st, n, r['run']]); cnt[(r['run'], st)] += 1
    except Exception as ex:
        bad[type(ex).__name__] += 1
print(sorted(cnt.items())); print('failed', dict(bad), 'total ok', sum(cnt.values()))
