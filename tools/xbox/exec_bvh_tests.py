"""image_bvh_intersect_ray execution tests: build small BVH node memory, run the recompiled instruction on the GPU, compare with a Python reference
(ray/slab test for box nodes, Moller-Trumbore for triangle nodes)."""
import math, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bvh_rehydrate_cpu as bvh
import rdna_asm
import exec_tests as T

INVALID = 0xffffffff
INF = float('inf')

def node_id(offset, kind):
    return (offset // 64) * 8 + kind

def f2u(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]

def box32_node(children, boxes):
    """children: 4 node ids, boxes: 4 x (min xyz, max xyz) -> 128 bytes (RDNA2 box32 layout)"""
    d = list(children)
    for lo, hi in boxes:
        d += [f2u(v) for v in lo] + [f2u(v) for v in hi]
    d += [0] * (32 - len(d))
    return struct.pack('<32I', *d)

def triangle_node(v0, v1, v2, tri_id):
    d = [f2u(c) for v in (v0, v1, v2) for c in v] + [0] * 6 + [tri_id]
    return struct.pack('<16I', *d)

def inv_dir(direction):
    return tuple((1.0 / c) if c != 0 else INF for c in direction)

def slab(origin, inv, extent, lo, hi, grow=0):
    enter = None
    leave = None
    for ax in range(3):
        t0 = (lo[ax] - origin[ax]) * inv[ax]
        t1 = (hi[ax] - origin[ax]) * inv[ax]
        near, far = (t0, t1) if inv[ax] >= 0 else (t1, t0)
        enter = near if enter is None else max(enter, near)
        leave = far if leave is None else min(leave, far)
    min_t = max(enter, 0.0)
    max_t = min(leave, extent)
    return (min_t <= max_t * (1.0 + grow * 2.0 ** -23)), min_t

def ref_box(children, boxes, origin, direction, extent, sort):
    inv = inv_dir(direction)
    hits = [slab(origin, inv, extent, lo, hi) for lo, hi in boxes]
    ids = [c if h else INVALID for c, (h, _) in zip(children, hits)]
    if not sort:
        return ids
    order = sorted(range(4), key=lambda i: (ids[i] == INVALID, hits[i][1]))
    return [ids[i] for i in order]

def ref_triangle(v0, v1, v2, origin, direction):
    sub = lambda a, b: tuple(x - y for x, y in zip(a, b))
    dot = lambda a, b: sum(x * y for x, y in zip(a, b))
    cross = lambda a, b: (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
    e1, e2, e3 = sub(v1, v0), sub(v2, v0), sub(origin, v0)
    s1, s2 = cross(direction, e2), cross(e3, e1)
    den = dot(s1, e1)
    t, u, v = dot(e2, s2) / den, dot(e3, s1) / den, dot(direction, s2) / den
    return t if (0 <= u <= 1 and 0 <= v <= 1 and u + v <= 1) else None

def half_bits(x):
    return struct.unpack('<H', struct.pack('<e', x))[0]

def box16_node(children, boxes):
    """bounds are fp16, two per dword (low half first): 16 dwords"""
    d = list(children)
    halves = [half_bits(v) for lo, hi in boxes for v in tuple(lo) + tuple(hi)]
    d += [halves[i] | (halves[i + 1] << 16) for i in range(0, len(halves), 2)]
    return struct.pack('<16I', *d)

def run_query(bvh_bytes, node, origin, direction, extent, sort=False, grow=0, node64=False, a16=False, barycentrics=False, inv=None):
    """one ray; returns the four result dwords of lane 0"""
    p = rdna_asm.Program()
    p.smem('s_load_dwordx4', 8, 4, 0)
    p.smem('s_load_dwordx4', 12, 4, 16)
    p.sopp('s_waitcnt', 0)
    p.vop2('v_lshlrev_b32', 1, 4, 'v0')
    inv = inv_dir(direction) if inv is None else inv
    vals = [node] + ([0] if node64 else []) + [f2u(extent)] + [f2u(c) for c in origin]
    if a16:
        h = [half_bits(c) for c in tuple(direction) + tuple(inv)]
        vals += [h[0] | (h[1] << 16), h[2] | (h[3] << 16), h[4] | (h[5] << 16)]
    else:
        vals += [f2u(c) for c in direction] + [f2u(c) for c in inv]
    for i, v in enumerate(vals):
        p.vop1('v_mov_b32', 8 + i, ('lit', v))
    p.mimg('image_bvh64_intersect_ray' if node64 else 'image_bvh_intersect_ray', 4, 8, 12, dmask=0xf, dim=0, a16=a16)
    p.sopp('s_waitcnt', 0)
    p.mubuf('buffer_store_dwordx4', 4, 1, 8, 128, 0)
    p.sopp('s_endpgm')
    pokes = T.vsharp(0, 0, 1, 1024) + [
        "ptr8lo 0 16 2", f"ptr8hi 0 20 2 {(grow << 23) | ((1 << 31) if sort else 0)}",
        "poke 0 24 0xffffffff", f"poke 0 28 {0x3ff | ((1 << 24) if barycentrics else 0) | (0xb << 28)}"]
    r = T.run(p.bytes(), {0: (256, None), 1: (1024, None), 2: (len(bvh_bytes), bvh_bytes)}, pokes)
    return struct.unpack('<4I', r[1][:16])

CHILD_OFFSETS = (128, 192, 256, 320)
CHILDREN = [node_id(o, 0) for o in CHILD_OFFSETS]

def boxes_along_x(order, miss=None):
    out = []
    for slot, x in enumerate(order):
        y = 5.0 if slot == miss else 0.0
        out.append(((x, y, 0.0), (x + 1.0, y + 1.0, 1.0)))
    return out

def test_box32_unsorted():
    boxes = boxes_along_x([0.0, 2.0, 4.0, 6.0], miss=2)
    node = box32_node(CHILDREN, boxes)
    origin, direction = (-1.0, 0.5, 0.5), (1.0, 0.0, 0.0)
    got = run_query(node, node_id(0, 5), origin, direction, 100.0)
    want = ref_box(CHILDREN, boxes, origin, direction, 100.0, False)
    assert list(got) == want, (list(map(hex, got)), list(map(hex, want)))

def test_box32_sorted():
    boxes = boxes_along_x([6.0, 2.0, 4.0, 0.0], miss=1)       # slot order is not distance order, one miss
    node = box32_node(CHILDREN, boxes)
    origin, direction = (-1.0, 0.5, 0.5), (1.0, 0.0, 0.0)
    got = run_query(node, node_id(0, 5), origin, direction, 100.0, sort=True)
    want = ref_box(CHILDREN, boxes, origin, direction, 100.0, True)
    assert list(got) == want, (list(map(hex, got)), list(map(hex, want)))

def test_box32_extent_clips():
    boxes = boxes_along_x([0.0, 2.0, 4.0, 6.0])
    node = box32_node(CHILDREN, boxes)
    origin, direction = (-1.0, 0.5, 0.5), (1.0, 0.0, 0.0)
    got = run_query(node, node_id(0, 5), origin, direction, 3.5)    # boxes at x >= 4 start beyond the extent (t = 5, 7)
    want = ref_box(CHILDREN, boxes, origin, direction, 3.5, False)
    assert list(got) == want, (list(map(hex, got)), list(map(hex, want)))

def test_triangle_hit():
    v0, v1, v2 = (5.0, 0.0, 0.0), (5.0, 2.0, 0.0), (5.0, 0.0, 2.0)
    bvh = box32_node(CHILDREN, boxes_along_x([0, 2, 4, 6])) + b'\0' * 0 + triangle_node(v0, v1, v2, 0x1234)
    pad = 128 - len(bvh) % 128 if False else 0
    origin, direction = (0.0, 0.5, 0.5), (1.0, 0.0, 0.0)
    got = run_query(bvh, node_id(128, 0), origin, direction, 100.0)
    t = got[0:1] and struct.unpack('<ff', struct.pack('<II', got[0], got[1]))
    want = ref_triangle(v0, v1, v2, origin, direction)
    assert want is not None and abs(t[0] / t[1] - want) < 1e-5 and got[2] == 0x1234, (got, t, want)

def test_triangle_miss():
    v0, v1, v2 = (5.0, 0.0, 0.0), (5.0, 2.0, 0.0), (5.0, 0.0, 2.0)
    bvh = box32_node(CHILDREN, boxes_along_x([0, 2, 4, 6])) + triangle_node(v0, v1, v2, 0x1234)
    origin, direction = (0.0, 1.5, 1.5), (1.0, 0.0, 0.0)             # passes outside the triangle
    got = run_query(bvh, node_id(128, 0), origin, direction, 100.0)
    t_num = struct.unpack('<f', struct.pack('<I', got[0]))[0]
    assert ref_triangle(v0, v1, v2, origin, direction) is None and t_num == INF, (got, t_num)

def _box16_case(sort, **kw):
    boxes = boxes_along_x([6.0, 2.0, 4.0, 0.0], miss=1)
    node = box16_node(CHILDREN, boxes)
    origin, direction = (-1.0, 0.5, 0.5), (1.0, 0.0, 0.0)
    got = run_query(node, node_id(0, 4), origin, direction, 100.0, sort=sort, **kw)
    want = ref_box(CHILDREN, boxes, origin, direction, 100.0, sort)
    assert list(got) == want, (list(map(hex, got)), list(map(hex, want)))

def test_box16_unsorted(): _box16_case(False)
def test_box16_sorted(): _box16_case(True)
def test_box32_node64(): 
    boxes = boxes_along_x([6.0, 2.0, 4.0, 0.0], miss=2)
    node = box32_node(CHILDREN, boxes)
    origin, direction = (-1.0, 0.5, 0.5), (1.0, 0.0, 0.0)
    got = run_query(node, node_id(0, 5), origin, direction, 100.0, sort=True, node64=True)
    want = ref_box(CHILDREN, boxes, origin, direction, 100.0, True)
    assert list(got) == want, (list(map(hex, got)), list(map(hex, want)))
def test_box32_a16():
    boxes = boxes_along_x([6.0, 2.0, 4.0, 0.0], miss=3)
    node = box32_node(CHILDREN, boxes)
    origin, direction = (-1.0, 0.5, 0.5), (1.0, 0.0, 0.0)
    got = run_query(node, node_id(0, 5), origin, direction, 100.0, sort=True, a16=True)
    want = ref_box(CHILDREN, boxes, origin, direction, 100.0, True)
    assert list(got) == want, (list(map(hex, got)), list(map(hex, want)))
def test_box16_node64_a16(): _box16_case(True, node64=True, a16=True)
def test_triangle_node64_a16():
    v0, v1, v2 = (5.0, 0.0, 0.0), (5.0, 2.0, 0.0), (5.0, 0.0, 2.0)
    bvh = box32_node(CHILDREN, boxes_along_x([0, 2, 4, 6])) + triangle_node(v0, v1, v2, 0x1234)
    origin, direction = (0.0, 0.5, 0.5), (1.0, 0.0, 0.0)
    got = run_query(bvh, node_id(128, 0), origin, direction, 100.0, node64=True, a16=True)
    t = struct.unpack('<ff', struct.pack('<II', got[0], got[1]))
    assert abs(t[0] / t[1] - 5.0) < 1e-5 and got[2] == 0x1234, (got, t)

def test_box32_low_guest_addresses():
    # the node memory lives at a guest address below 4 GiB; the BDA page table maps it to the real device address
    boxes = boxes_along_x([6.0, 2.0, 4.0, 0.0], miss=1)
    node = box32_node(CHILDREN, boxes)
    origin, direction = (-1.0, 0.5, 0.5), (1.0, 0.0, 0.0)
    p = rdna_asm.Program()
    p.smem('s_load_dwordx4', 8, 4, 0)
    p.smem('s_load_dwordx4', 12, 4, 16)
    p.sopp('s_waitcnt', 0)
    p.vop2('v_lshlrev_b32', 1, 4, 'v0')
    inv = inv_dir(direction)
    vals = [node_id(0, 5), f2u(100.0)] + [f2u(c) for c in origin] + [f2u(c) for c in direction] + [f2u(c) for c in inv]
    for i, v in enumerate(vals):
        p.vop1('v_mov_b32', 8 + i, ('lit', v))
    p.mimg('image_bvh_intersect_ray', 4, 8, 12, dmask=0xf, dim=0)
    p.sopp('s_waitcnt', 0)
    p.mubuf('buffer_store_dwordx4', 4, 1, 8, 128, 0)
    p.sopp('s_endpgm')
    pokes = T.vsharp(0, 0, 1, 1024) + ["ptr8lo 0 16 2", f"ptr8hi 0 20 2 {1 << 31}", "poke 0 24 0xffffffff", f"poke 0 28 {0x3ff | (0xb << 28)}"]
    r = T.run(p.bytes(), {0: (256, None), 1: (1024, None), 2: (len(node), node)}, pokes, extra=["lowguest"])
    got = struct.unpack('<4I', r[1][:16])
    assert list(got) == ref_box(CHILDREN, boxes, origin, direction, 100.0, True), list(map(hex, got))

def test_box32_axis_aligned_ray_on_face():
    boxes = [
        ((0.0, 0.0, 5.0), (1.0, 1.0, 5.0)),
        ((0.0, 2.0, 5.0), (1.0, 3.0, 5.0)),
        ((float('nan'), 0.0, 0.0), (1.0, 1.0, 1.0)),
        ((INF, INF, INF), (-INF, -INF, -INF)),
    ]
    children = [CHILDREN[0], CHILDREN[1], CHILDREN[2], INVALID]
    node = box32_node(children, boxes)
    want = [CHILDREN[0], INVALID, INVALID, INVALID]
    for origin, direction, inv in (
        ((0.0, 0.5, 0.0), (0.0, 0.0, 1.0), (INF, INF, 1.0)),
        ((-1.0, 0.5, 5.0), (1.0, 0.0, 0.0), (1.0, INF, INF)),
        ((0.0, 0.5, 0.0), (0.0, 0.0, 1.0), (-INF, INF, 1.0)),
    ):
        for sort in (False, True):
            got = run_query(node, node_id(0, 5), origin, direction, 100.0, sort=sort, inv=inv)
            assert list(got) == want, (origin, direction, inv, sort, list(map(hex, got)))

def test_box16_axis_aligned_ray_on_flat_box():
    boxes = [
        ((0.0, 0.0, 5.0), (1.0, 1.0, 5.0)),
        ((0.0, 2.0, 5.0), (1.0, 3.0, 5.0)),
        ((float('nan'), 0.0, 0.0), (1.0, 1.0, 1.0)),
        ((INF, INF, INF), (-INF, -INF, -INF)),
    ]
    children = [CHILDREN[0], CHILDREN[1], CHILDREN[2], INVALID]
    node = box16_node(children, boxes)
    want = [CHILDREN[0], INVALID, INVALID, INVALID]
    for origin, direction, inv in (
        ((0.0, 0.5, 0.0), (0.0, 0.0, 1.0), (INF, INF, 1.0)),
        ((-1.0, 0.5, 5.0), (1.0, 0.0, 0.0), (1.0, INF, INF)),
    ):
        for sort in (False, True):
            got = run_query(node, node_id(0, 4), origin, direction, 100.0, sort=sort, inv=inv)
            assert list(got) == want, (origin, direction, inv, sort, list(map(hex, got)))

def test_triangle_nan_and_parallel_miss():
    v0, v1, v2 = (0.0, 0.0, 5.0), (1.0, 0.0, 5.0), (0.0, 1.0, 5.0)
    nan3 = (float('nan'), float('nan'), float('nan'))
    single_leaf = struct.pack(
        '<12f4I',
        *v0, *v1, *v2, *nan3,
        0x01, 0, INVALID, 0x5678,
    )
    degen_leaf = triangle_node(v0, v0, v2, 0x9abc)
    for bary in (False, True):
        got_tri1_nan = run_query(single_leaf, node_id(0, 1), (0.25, 0.25, 0.0), (0.0, 0.0, 1.0), 100.0, barycentrics=bary)
        assert got_tri1_nan[0] == 0x7f800000 and got_tri1_nan[1] == 0x3f800000, ("tri1_nan", bary, list(map(hex, got_tri1_nan)))
        if not bary:
            assert got_tri1_nan[2] == 0x5678 and got_tri1_nan[3] == 0, list(map(hex, got_tri1_nan))
        got_coplanar = run_query(single_leaf, node_id(0, 0), (0.25, 0.25, 5.0), (1.0, 0.0, 0.0), 100.0, barycentrics=bary)
        assert got_coplanar[0] == 0x7f800000 and got_coplanar[1] == 0x3f800000, ("coplanar", bary, list(map(hex, got_coplanar)))
        if not bary:
            assert got_coplanar[2] == 0x5678 and got_coplanar[3] == 0, list(map(hex, got_coplanar))
        got_degen = run_query(degen_leaf, node_id(0, 0), (0.0, 0.25, 0.0), (0.0, 0.0, 1.0), 100.0, barycentrics=bary)
        assert got_degen[0] == 0x7f800000 and got_degen[1] == 0x3f800000, ("degen", bary, list(map(hex, got_degen)))
        if not bary:
            assert got_degen[2] == 0x9abc and got_degen[3] == 0, list(map(hex, got_degen))

def _pack_leaf_chunk(entries):
    p0, p1, p2, p3, p4 = bytearray(), bytearray(), bytearray(), bytearray(), bytearray()
    for geom_idx, flags, prim0, prim1, vorder in entries:
        p0 += bytes((geom_idx & 0xff, (geom_idx >> 8) & 0xff, (geom_idx >> 16) & 0xff))
        p1.append(flags & 0xff)
        p2 += bytes((prim0 & 0xff, (prim0 >> 8) & 0xff, (prim0 >> 16) & 0xff))
        pr1 = 0xffffff if prim1 == INVALID else (prim1 & 0xffffff)
        p3 += bytes((pr1 & 0xff, (pr1 >> 8) & 0xff, (pr1 >> 16) & 0xff))
        p4.append(vorder & 0xff)
    return bytes(p0 + p1 + p2 + p3 + p4)

def _pack_box_group(nodes, is_box32):
    hwords, cids, ovs = [], [], []
    for children, overrides in nodes:
        mask = 0
        ordered = []
        for bit_idx in sorted(overrides.keys()):
            mask |= 1 << bit_idx
            ordered.append(overrides[bit_idx])
        hwords.append((len(children) << 24) | mask)
        cids.extend(children)
        ovs.extend(ordered)
    out = bytearray(struct.pack(f"<{len(hwords)}I", *hwords))
    if cids:
        out += struct.pack(f"<{len(cids)}I", *cids)
    if ovs:
        out += struct.pack(f"<{len(ovs)}{'f' if is_box32 else 'H'}", *ovs)
    return bytes(out)

def _pack_rage9_q8_vb(verts, origin, scale, slot_axis):
    hdr = struct.pack("<3fI3fI", origin[0], origin[1], origin[2], slot_axis, scale[0], scale[1], scale[2], 0)
    body = bytearray()
    for pos in verts:
        slots = (pos[2], pos[1], pos[0]) if slot_axis == 0 else (pos[2], pos[0], pos[1]) if slot_axis == 1 else (pos[0], pos[1], pos[2])
        ix = max(0, min(0xffffff, int(round((slots[0] - origin[0]) / scale[0]))))
        iy = max(0, min(0xffffff, int(round((slots[1] - origin[1]) / scale[1]))))
        iz = max(0, min(0xffff, int(round((slots[2] - origin[2]) / scale[2]))))
        w0 = (iy & 0xff) | ((ix & 0xffffff) << 8)
        w1 = ((iy >> 8) & 0xffff) | ((iz & 0xffff) << 16)
        body += struct.pack("<2I", w0, w1)
    return hdr + bytes(body), 32

def _pack_rage9_meshlet_ib(meshlets):
    m_hdrs, tri_words, remaps = bytearray(), bytearray(), bytearray()
    v_off, t_off = 0, 0
    for m_idx, (local_tris, remap_verts) in enumerate(meshlets):
        mh0 = ((v_off >> 16) & 1) << 14
        mh1 = (v_off & 0xffff) | ((t_off & 0xffff) << 16)
        m_hdrs += struct.pack("<4I", mh0, mh1, 0, 0)
        for r0, r1, r2 in local_tris:
            tri_words += struct.pack("<I", (r0 & 0x7f) | ((r1 & 0x7f) << 7) | ((r2 & 0x7f) << 14) | ((m_idx & 0x7ff) << 21))
        for rv in remap_verts:
            remaps += struct.pack("<H", rv)
        v_off += len(remap_verts)
        t_off += len(local_tris)
    top = struct.pack("<3I", len(meshlets), t_off, v_off)
    return bytes(top + m_hdrs + tri_words + remaps)

def build_multilevel_xbox_bvh():
    """Build and rehydrate a synthetic 3-level Xbox Series BLAS mixing Box32, Box16, single TRI0, paired TRI0+TRI1, and RAGE9 meshlet geometry."""
    g0_verts = [
        (0.0, 0.0, 5.0), (2.0, 0.0, 5.0), (0.0, 2.0, 5.0),
        (10.0, 0.0, 4.0), (12.0, 0.0, 4.0), (10.0, 2.0, 4.0), (12.0, 2.0, 4.0),
        (0.0, 0.0, 2.0), (1.0, 0.0, 2.0), (0.0, 1.0, 2.0),
        (0.0, 2.0, 8.0), (2.0, 0.0, 8.0), (0.0, 0.0, 8.0),
        (20.0, 0.0, 1.0), (22.0, 1.0, 3.0), (20.0, 2.0, 5.0),
        (30.0, 0.0, 1.0), (32.0, 0.0, 1.0), (30.0, 0.0, 3.0),
    ]
    g0_tris = [
        (0, 1, 2),
        (5, 3, 4),
        (5, 4, 6),
        (7, 8, 9),
        (10, 11, 12),
        (13, 14, 15),
        (16, 17, 18),
    ]
    geom0 = bvh.TriangleGeometryDesc(
        vertex_buffer=b"".join(struct.pack("<3f", *v) for v in g0_verts),
        vertex_stride=12,
        vertex_format=bvh.VERTEX_FORMAT_R32G32B32_FLOAT,
        index_buffer=b"".join(struct.pack("<3I", *t) for t in g0_tris),
        index_format=bvh.INDEX_FORMAT_R32_UINT,
    )
    g1_local = [
        (0.0, 0.0, 5.0), (2.0, 0.0, 5.0), (0.0, 2.0, 5.0),
        (4.0, 0.0, 6.0), (6.0, 0.0, 6.0), (4.0, 2.0, 6.0),
    ]
    vb_q8, voff_q8 = _pack_rage9_q8_vb(g1_local, (0.0, 0.0, 0.0), (0.25, 0.25, 0.25), 2)
    ib_m = _pack_rage9_meshlet_ib([([(0, 1, 2), (3, 4, 5)], [0, 1, 2, 3, 4, 5])])
    m3x4 = ((1.0, 0.0, 0.0, 40.0), (0.0, 1.0, 0.0, 1.0), (0.0, 0.0, 1.0, -2.0))
    geom1 = bvh.TriangleGeometryDesc(
        vertex_buffer=vb_q8,
        vertex_stride=8,
        vertex_format=bvh.VERTEX_FORMAT_RAGE9_QUANTIZED8,
        vertex_offset=voff_q8,
        index_buffer=ib_m,
        index_format=bvh.INDEX_FORMAT_RAGE9_MESHLET,
        transform_3x4=m3x4,
    )

    leaf_off = 0x100
    hier_off = 0x300
    l0_b32_0 = hier_off
    l0_b32_1 = hier_off + 128
    l0_b16_0 = hier_off + 256
    l1_b32_1 = hier_off + 320
    l1_b16_0 = hier_off + 448
    root_off = hier_off + 512
    total_bytes = 0x5c0

    leaf_entries = [
        (0, 0x01, 0, INVALID, 0x09),
        (0, 0x81, 1, 2, 0x44),
        (0, 0x01, 3, INVALID, 0x09),
        (0, 0x01, 4, INVALID, 0x09),
        (0, 0x01, 5, INVALID, 0x09),
        (0, 0x01, 6, INVALID, 0x09),
        (1, 0x01, 0, INVALID, 0x09),
        (1, 0x01, 1, INVALID, 0x09),
    ]
    leaf_tri0 = [node_id(leaf_off + i * 64, bvh.NODE_TYPE_TRI0) for i in range(8)]
    leaf1_tri1 = node_id(leaf_off + 64, bvh.NODE_TYPE_TRI1)
    id_l0_b32_0 = node_id(l0_b32_0, bvh.NODE_TYPE_BOX32)
    id_l0_b32_1 = node_id(l0_b32_1, bvh.NODE_TYPE_BOX32)
    id_l0_b16_0 = node_id(l0_b16_0, bvh.NODE_TYPE_BOX16)
    id_l1_b32_1 = node_id(l1_b32_1, bvh.NODE_TYPE_BOX32)
    id_l1_b16_0 = node_id(l1_b16_0, bvh.NODE_TYPE_BOX16)
    root_id = node_id(root_off, bvh.NODE_TYPE_BOX32)

    src = bytearray(total_bytes)
    flags = bvh.BVH_FLAG_BLAS | bvh.BVH_FLAG_COMPRESSED | bvh.BVH_FLAG_UPDATABLE | 0x20006
    src[:64] = struct.pack(
        "<4I6f6I",
        flags, (total_bytes >> 6) - 1, 0x13572468, 8,
        0.0, 0.0, 1.0, 46.0, 3.0, 8.0,
        hier_off, 0, root_id, leaf_off, 0x40, bvh.DEFAULT_TRANSFORM_WORD,
    )
    chunk = _pack_leaf_chunk(leaf_entries)
    src[leaf_off : leaf_off + len(chunk)] = chunk
    g0_b32 = _pack_box_group([
        ([leaf_tri0[0], leaf_tri0[2], leaf_tri0[3]], {}),
        ([leaf_tri0[1], leaf1_tri1, leaf_tri0[4], leaf_tri0[5]], {}),
    ], is_box32=True)
    src[l0_b32_0 : l0_b32_0 + len(g0_b32)] = g0_b32
    g0_b16 = _pack_box_group([([leaf_tri0[6], leaf_tri0[7]], {})], is_box32=False)
    src[l0_b16_0 : l0_b16_0 + len(g0_b16)] = g0_b16
    g1_b32 = _pack_box_group([([id_l0_b16_0], {})], is_box32=True)
    src[l1_b32_1 : l1_b32_1 + len(g1_b32)] = g1_b32
    g1_b16 = _pack_box_group([([id_l0_b32_0, id_l0_b32_1], {})], is_box32=False)
    src[l1_b16_0 : l1_b16_0 + len(g1_b16)] = g1_b16
    g2_b32 = _pack_box_group([([id_l1_b16_0, id_l1_b32_1], {})], is_box32=True)
    src[root_off : root_off + len(g2_b32)] = g2_b32
    tbl = struct.pack("<7I", 256, 320, 448, 512, 640, 640, 3)
    src[total_bytes - len(tbl) : total_bytes] = tbl

    dst = bvh.rehydrate_bvh(bytes(src), geometries=[geom0, geom1])
    rep = bvh.validate_rehydrated_bvh(bytes(dst), check_aabb_enclosure=True, check_parent_pointers=True)
    assert rep.valid, rep.errors

    scene_tris = [
        (leaf_tri0[0], (0.0, 0.0, 5.0), (2.0, 0.0, 5.0), (0.0, 2.0, 5.0)),
        (leaf_tri0[1], (10.0, 2.0, 4.0), (10.0, 0.0, 4.0), (12.0, 0.0, 4.0)),
        (leaf1_tri1, (10.0, 2.0, 4.0), (12.0, 0.0, 4.0), (12.0, 2.0, 4.0)),
        (leaf_tri0[2], (0.0, 0.0, 2.0), (1.0, 0.0, 2.0), (0.0, 1.0, 2.0)),
        (leaf_tri0[3], (0.0, 2.0, 8.0), (2.0, 0.0, 8.0), (0.0, 0.0, 8.0)),
        (leaf_tri0[4], (20.0, 0.0, 1.0), (22.0, 1.0, 3.0), (20.0, 2.0, 5.0)),
        (leaf_tri0[5], (30.0, 0.0, 1.0), (32.0, 0.0, 1.0), (30.0, 0.0, 3.0)),
        (leaf_tri0[6], (40.0, 1.0, 3.0), (42.0, 1.0, 3.0), (40.0, 3.0, 3.0)),
        (leaf_tri0[7], (44.0, 1.0, 4.0), (46.0, 1.0, 4.0), (44.0, 3.0, 4.0)),
    ]
    return bytes(dst), root_id, scene_tris

def ref_scene_trace(scene_tris, origin, direction, extent):
    sub = lambda a, b: (a[0] - b[0], a[1] - b[1], a[2] - b[2])
    dot = lambda a, b: a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
    cross = lambda a, b: (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
    best = None
    best_t = extent
    for nid, v0, v1, v2 in scene_tris:
        e1, e2, e3 = sub(v1, v0), sub(v2, v0), sub(origin, v0)
        s1, s2 = cross(direction, e2), cross(e3, e1)
        den = dot(s1, e1)
        if den == 0.0:
            continue
        t = dot(e2, s2) / den
        u = dot(e3, s1) / den
        v = dot(direction, s2) / den
        if 0.0 <= u <= 1.0 and 0.0 <= v <= 1.0 and u + v <= 1.0 and 0.0 <= t < best_t:
            best_t = t
            best = (nid, t, u, v, den > 0.0)
    return best

def build_scene_rays():
    return [
        ((1.2, 0.4, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((0.4, 1.2, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((0.8, 0.8, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((0.2, 1.5, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((0.25, 0.25, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((0.5, 0.2, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((0.2, 0.5, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((0.0, 0.0, 0.0), (0.125, 0.125, 1.0), 100.0),
        ((10.2, 0.3, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((11.0, 0.4, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((10.4, 1.2, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((10.6, 0.8, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((11.6, 1.4, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((11.8, 1.8, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((11.2, 1.0, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((11.4, 1.6, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((0.3, 1.2, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((1.2, 0.3, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((0.8, 0.8, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((20.4, 0.6, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((19.5, 1.0, 0.0), (1.0, 0.0, 2.0), 100.0),
        ((30.5, 0.0, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((30.4, -2.0, 1.6), (0.0, 1.0, 0.0), 100.0),
        ((40.4, 1.6, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((41.2, 1.2, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((44.4, 1.6, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((45.0, 1.5, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((1.2, 0.4, 0.0), (0.0, 0.0, 1.0), 4.5),
        ((9.0, 0.5, 4.0), (1.0, 0.0, 0.0), 100.0),
        ((-10.0, -10.0, 0.0), (0.0, 0.0, 1.0), 100.0),
        ((0.4, 0.4, 0.0), (0.0, 0.0, -1.0), 100.0),
        ((21.5, 1.8, 0.0), (0.0, 0.0, 1.0), 100.0),
    ]

def assemble_traversal_shader(node64=False, wave=32):
    """Assemble an RDNA2 stack-based multi-level BVH traversal compute kernel."""
    p = rdna_asm.Program()
    mov_exec = 's_mov_b64' if wave == 64 else 's_mov_b32'
    and_saveexec = 's_and_saveexec_b64' if wave == 64 else 's_and_saveexec_b32'
    best_t = 18 if node64 else 17
    orig_r = best_t + 1
    dir_r = best_t + 4
    inv_r = best_t + 7

    p.smem('s_load_dwordx4', 8, 4, 0)
    p.smem('s_load_dwordx4', 12, 4, 16)
    p.smem('s_load_dwordx4', 16, 4, 32)
    p.smem('s_load_dwordx4', 20, 4, 48)
    p.sopp('s_waitcnt', 0)

    p.vop2('v_lshlrev_b32', 1, 6, 'v0')
    p.vop2('v_lshlrev_b32', 2, 5, 'v0')
    p.vop1('v_mov_b32', 3, 0)
    p.vop1('v_mov_b32', 12, ('lit', 0x7f800000))
    p.vop1('v_mov_b32', 13, ('lit', 0x3f800000))
    p.vop1('v_mov_b32', 14, 0)
    p.vop1('v_mov_b32', 15, 0)
    p.vop1('v_mov_b32', 28, -1)
    p.vop1('v_mov_b32', 30, 0)
    p.vop1('v_mov_b32', 31, 0)

    p.mubuf('buffer_load_dwordx3', orig_r, 1, 8, 128, 0)
    p.mubuf('buffer_load_dword', best_t, 1, 8, 128, 12)
    p.mubuf('buffer_load_dwordx3', dir_r, 1, 8, 128, 16)
    p.mubuf('buffer_load_dwordx3', inv_r, 1, 8, 128, 32)
    p.mubuf('buffer_load_dword', 16, 1, 8, 128, 48)
    if node64:
        p.mubuf('buffer_load_dword', 17, 1, 8, 128, 52)
    p.sopp('s_waitcnt', 0)

    p.sop1(mov_exec, 24, rdna_asm.EXEC_LO)
    p.vopc('v_cmpx_lg_u32', -1, 'v16')
    br_exit = p.branch_fwd('s_cbranch_execz')

    loop_top = p.label()
    p.vop2('v_add_nc_u32', 30, 1, 'v30')
    p.mimg('image_bvh64_intersect_ray' if node64 else 'image_bvh_intersect_ray', 4, 16, 20, dmask=0xf, dim=0)
    p.sopp('s_waitcnt', 0)

    p.sop1(mov_exec, 26, rdna_asm.EXEC_LO)
    p.vop2('v_and_b32', 8, 7, 'v16')
    p.vopc('v_cmp_ge_u32', 1, 'v8')
    p.sop1(and_saveexec, 28, rdna_asm.VCC_LO)
    skip_tri = p.branch_fwd('s_cbranch_execz')
    p.vop1('v_rcp_f32', 9, 'v5')
    p.vop2('v_mul_f32', 9, 'v4', 'v9')
    p.vopc('v_cmp_lt_f32', 'v9', f'v{best_t}')
    p.vop2('v_cndmask_b32', best_t, f'v{best_t}', 'v9')
    p.vop2('v_cndmask_b32', 12, 'v12', 'v4')
    p.vop2('v_cndmask_b32', 13, 'v13', 'v5')
    p.vop2('v_cndmask_b32', 14, 'v14', 'v6')
    p.vop2('v_cndmask_b32', 15, 'v15', 'v7')
    p.vop2('v_cndmask_b32', 28, 'v28', 'v16')
    p.patch_to_here(skip_tri)

    p.sop1(mov_exec, rdna_asm.EXEC_LO, 's26')
    p.vopc('v_cmp_le_u32', 4, 'v8')
    p.sop1(and_saveexec, 28, rdna_asm.VCC_LO)
    skip_box = p.branch_fwd('s_cbranch_execz')
    for child_reg in (7, 6, 5, 4):
        p.vopc('v_cmp_lg_u32', -1, f'v{child_reg}')
        p.sop1(and_saveexec, 30, rdna_asm.VCC_LO)
        skip_push = p.branch_fwd('s_cbranch_execz')
        p.vop2('v_lshlrev_b32', 10, 5, 'v0')
        p.vop2('v_add_nc_u32', 10, 'v3', 'v10')
        p.vop2('v_lshlrev_b32', 10, 2, 'v10')
        p.mubuf('buffer_store_dword', child_reg, 10, 16, 128, 0)
        p.vop2('v_add_nc_u32', 3, 1, 'v3')
        p.patch_to_here(skip_push)
        p.sop1(mov_exec, rdna_asm.EXEC_LO, 's30')
    p.patch_to_here(skip_box)

    p.sop1(mov_exec, rdna_asm.EXEC_LO, 's26')
    p.vop1('v_mov_b32', 11, -1)
    p.vopc('v_cmp_eq_u32', 0, 'v3')
    p.vop2('v_cndmask_b32', 16, 'v16', 'v11')
    p.vopc('v_cmp_lg_u32', -1, 'v16')
    p.sop1(and_saveexec, 28, rdna_asm.VCC_LO)
    skip_pop = p.branch_fwd('s_cbranch_execz')
    p.vop2('v_add_nc_u32', 3, -1, 'v3')
    p.vop2('v_lshlrev_b32', 10, 5, 'v0')
    p.vop2('v_add_nc_u32', 10, 'v3', 'v10')
    p.vop2('v_lshlrev_b32', 10, 2, 'v10')
    p.mubuf('buffer_load_dword', 16, 10, 16, 128, 0)
    p.sopp('s_waitcnt', 0)
    p.patch_to_here(skip_pop)
    p.sop1(mov_exec, rdna_asm.EXEC_LO, 's26')
    p.vopc('v_cmpx_lg_u32', -1, 'v16')
    p.branch_to('s_cbranch_execnz', loop_top)

    p.patch_to_here(br_exit)
    p.sop1(mov_exec, rdna_asm.EXEC_LO, 's24')
    p.vop1('v_mov_b32', 29, f'v{best_t}')
    p.mubuf('buffer_store_dwordx4', 12, 2, 12, 128, 0)
    p.mubuf('buffer_store_dwordx4', 28, 2, 12, 128, 16)
    p.sopp('s_endpgm')
    return p.bytes()

def _run_traversal_test(sort=True, node64=False, lowguest=False):
    bvh_bytes, root_id, scene_tris = build_multilevel_xbox_bvh()
    rays = build_scene_rays()
    assert len(rays) == 32
    ray_buf = bytearray()
    for origin, direction, extent in rays:
        inv = inv_dir(direction)
        ray_buf += struct.pack(
            "<16f",
            origin[0], origin[1], origin[2], extent,
            direction[0], direction[1], direction[2], 0.0,
            inv[0], inv[1], inv[2], 0.0,
            struct.unpack("<f", struct.pack("<I", root_id))[0], 0.0, 0.0, 0.0,
        )
    last_node = (len(bvh_bytes) // 64) - 1
    pokes = (
        T.vsharp(0, 0, 1, len(ray_buf))
        + T.vsharp(0, 16, 3, 32 * 32)
        + T.vsharp(0, 32, 4, 32 * 128)
        + [
            "ptr8lo 0 48 2",
            f"ptr8hi 0 52 2 {(1 << 31) if sort else 0}",
            f"poke 0 56 {last_node}",
            f"poke 0 60 {(1 << 24) | (0xb << 28)}",
        ]
    )
    extra = ["lowguest"] if lowguest else []
    r = T.run(
        assemble_traversal_shader(node64=node64, wave=32),
        {0: (256, None), 1: (len(ray_buf), bytes(ray_buf)), 2: (len(bvh_bytes), bvh_bytes), 3: (32 * 32, None), 4: (32 * 128, None)},
        pokes,
        extra=extra,
    )
    for lane, (origin, direction, extent) in enumerate(rays):
        tnum_w, tden_w, inum_w, jnum_w, best_node, best_t_w, steps, _ = struct.unpack_from("<8I", r[3], lane * 32)
        tnum, tden, inum, jnum, best_t = struct.unpack("<5f", struct.pack("<5I", tnum_w, tden_w, inum_w, jnum_w, best_t_w))
        want = ref_scene_trace(scene_tris, origin, direction, extent)
        if want is None:
            assert best_node == INVALID and math.isinf(tnum) and tden == 1.0, (lane, origin, direction, best_node, tnum, tden, steps)
        else:
            want_nid, want_t, want_u, want_v, want_front = want
            assert best_node == want_nid, (lane, hex(best_node), hex(want_nid), origin, direction)
            assert abs(tnum / tden - want_t) < 1e-5, (lane, tnum / tden, want_t)
            assert abs(best_t - want_t) < 1e-5, (lane, best_t, want_t)
            assert abs(inum / tden - want_u) < 1e-5, (lane, inum / tden, want_u)
            assert abs(jnum / tden - want_v) < 1e-5, (lane, jnum / tden, want_v)
            assert (tden > 0.0) == want_front, (lane, tden, want_front)
        if lane in (29, 30):
            assert steps == 1, (lane, steps)
        else:
            assert steps >= 2, (lane, steps)

def test_multilevel_rehydrated_bvh_traversal_sorted():
    _run_traversal_test(sort=True, node64=False, lowguest=False)

def test_multilevel_rehydrated_bvh_traversal_unsorted():
    _run_traversal_test(sort=False, node64=False, lowguest=False)

def test_multilevel_rehydrated_bvh64_traversal_lowguest():
    _run_traversal_test(sort=True, node64=True, lowguest=True)

TESTS = [test_box32_low_guest_addresses, test_box16_unsorted, test_box16_sorted, test_box32_node64, test_box32_a16, test_box16_node64_a16, test_triangle_node64_a16, test_box32_unsorted, test_box32_sorted, test_box32_extent_clips, test_triangle_hit, test_triangle_miss, test_box32_axis_aligned_ray_on_face, test_box16_axis_aligned_ray_on_flat_box, test_triangle_nan_and_parallel_miss, test_multilevel_rehydrated_bvh_traversal_sorted, test_multilevel_rehydrated_bvh_traversal_unsorted, test_multilevel_rehydrated_bvh64_traversal_lowguest]

if __name__ == "__main__":
    bad = 0
    for t in TESTS:
        try:
            t(); print("PASS", t.__name__)
        except Exception as e:
            bad += 1; print("FAIL", t.__name__, str(e)[:600])
    sys.exit(1 if bad else 0)
