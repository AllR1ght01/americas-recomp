"""Image execution tests: formats other than RGBA32F, 1D, 3D and cube images, mip levels, block-compressed images, and samplers built from the guest S#."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rdna_asm
import exec_tests as T

FLOAT = {0.2: 0x3e4ccccd, 0.6: 0x3f19999a, 1.0: 0x3f800000, 0.5: 0x3f000000, 1.25: 0x3fa00000}
DIM_1D, DIM_2D, DIM_3D, DIM_CUBE = 0, 1, 2, 3


def _image(r, image_id, fmt, count):
    return struct.unpack('<%d%s' % (count, fmt), open(os.path.join(r['out'], f"image{image_id}.bin"), 'rb').read())


def _out_vsharp(p, sgpr, off):
    p.smem('s_load_dwordx4', sgpr, 4, off)


def test_store_rgba8_unorm():
    p = rdna_asm.Program()
    p.smem('s_load_dwordx8', 8, 4, 0)
    p.sopp('s_waitcnt', 0)
    p.vop1('v_cvt_f32_u32', 4, 'v0')
    p.vop2('v_mul_f32', 4, ('lit', 0x3d042108), 'v4')
    p.vop1('v_mov_b32', 5, ('lit', FLOAT[0.6]))
    p.vop1('v_mov_b32', 6, ('lit', FLOAT[0.2]))
    p.vop1('v_mov_b32', 7, ('lit', FLOAT[1.0]))
    p.vop1('v_mov_b32', 1, 0)
    p.mimg('image_store', 4, 0, 8)
    p.sopp('s_endpgm')
    r = T.run(p.bytes(), {0: (256, None)}, [], extra=["image 1 32 1 format 56", "timg 0 0 1"])
    got = _image(r, 1, 'B', 128)
    lane_scale = struct.unpack('<f', struct.pack('<I', 0x3d042108))[0]
    for lane in range(32):
        want = (round(lane * lane_scale * 255), 153, 51, 255)
        texel = got[lane * 4:lane * 4 + 4]
        assert all(abs(a - b) <= 1 for a, b in zip(texel, want)), ("lane", lane, texel, want)


def test_store_r32_uint():
    p = rdna_asm.Program()
    p.smem('s_load_dwordx8', 8, 4, 0)
    p.sopp('s_waitcnt', 0)
    p.vop2('v_mul_u32_u24', 4, 3, 'v0')
    p.vop2('v_add_nc_u32', 4, 7, 'v4')
    p.vop1('v_mov_b32', 1, 0)
    p.mimg('image_store', 4, 0, 8, dmask=1)
    p.sopp('s_endpgm')
    r = T.run(p.bytes(), {0: (256, None)}, [], extra=["image 1 32 1 format 20 clear 99 99 99 99", "timg 0 0 1"])
    assert _image(r, 1, 'I', 32) == tuple(lane * 3 + 7 for lane in range(32))


def _load_to_buffer(dim, coords, image_line, image_init):
    p = rdna_asm.Program()
    p.smem('s_load_dwordx8', 8, 4, 0)
    _out_vsharp(p, 16, 32)
    p.sopp('s_waitcnt', 0)
    coords(p)
    p.mimg('image_load', 4, 1, 8, dmask=1, dim=dim)
    p.sopp('s_waitcnt', 0)
    p.vop2('v_lshlrev_b32', 5, 2, 'v0')
    p.mubuf('buffer_store_dword', 4, 5, 16, 128, 0)
    p.sopp('s_endpgm')
    init = os.path.join(T.tempfile.mkdtemp(prefix="img_"), "init.bin")
    open(init, 'wb').write(image_init)
    r = T.run(p.bytes(), {0: (256, None), 2: (128, None)}, T.vsharp(0, 32, 2, 128), extra=[f"{image_line} init {init}", "timg 0 0 1"])
    return struct.unpack('<32I', r[2][:128])


def test_load_3d_r32_uint():
    def coords(p):
        p.vop2('v_and_b32', 1, 3, 'v0')
        p.vop2('v_lshrrev_b32', 2, 2, 'v0')
        p.vop2('v_and_b32', 2, 3, 'v2')
        p.vop2('v_lshrrev_b32', 3, 4, 'v0')
    texels = struct.pack('<32I', *[x + 10 * y + 100 * z for z in range(2) for y in range(4) for x in range(4)])
    got = _load_to_buffer(DIM_3D, coords, "image 1 4 4 format 20 dim 3d depth 2", texels)
    want = tuple((lane & 3) + 10 * ((lane >> 2) & 3) + 100 * (lane >> 4) for lane in range(32))
    assert got == want, (got[:8], want[:8])


def test_load_1d_r32_uint():
    def coords(p):
        p.vop2('v_sub_nc_u32', 1, 31, 'v0')
    texels = struct.pack('<32I', *[5000 + i for i in range(32)])
    got = _load_to_buffer(DIM_1D, coords, "image 1 32 1 format 20 dim 1d", texels)
    assert got == tuple(5000 + 31 - lane for lane in range(32)), got[:8]


def _sample(u, clamp, filt):
    p = rdna_asm.Program()
    p.smem('s_load_dwordx8', 8, 4, 0)
    p.smem('s_load_dwordx4', 16, 4, 32)
    _out_vsharp(p, 20, 48)
    p.sopp('s_waitcnt', 0)
    p.vop1('v_mov_b32', 1, ('lit', FLOAT[u]))
    p.vop1('v_mov_b32', 2, ('lit', FLOAT[0.5]))
    p.mimg('image_sample_lz', 4, 1, 8, dmask=1, ssamp=16)
    p.sopp('s_waitcnt', 0)
    p.vop2('v_lshlrev_b32', 5, 2, 'v0')
    p.mubuf('buffer_store_dword', 4, 5, 20, 128, 0)
    p.sopp('s_endpgm')
    init = os.path.join(T.tempfile.mkdtemp(prefix="img_"), "init.bin")
    open(init, 'wb').write(struct.pack('<2f', 0.0, 1.0))
    ssharp = [clamp | (clamp << 3) | (clamp << 6), 0xfff << 12, (filt << 20) | (filt << 22), 0]
    pokes = [f"poke 0 {32 + 4 * k} {w}" for k, w in enumerate(ssharp)] + T.vsharp(0, 48, 2, 128)
    r = T.run(p.bytes(), {0: (256, None), 2: (128, None)}, pokes, extra=[f"image 1 2 1 format 22 init {init}", "timg 0 0 1"])
    return struct.unpack('<32f', r[2][:128])


def test_resinfo_reports_the_image_extent():
    p = rdna_asm.Program()
    p.smem('s_load_dwordx8', 8, 4, 0)
    _out_vsharp(p, 16, 32)
    p.sopp('s_waitcnt', 0)
    p.vop1('v_mov_b32', 1, 0)
    p.mimg('image_get_resinfo', 4, 1, 8, dmask=3)
    p.sopp('s_waitcnt', 0)
    p.vop2('v_lshlrev_b32', 6, 3, 'v0')
    p.mubuf('buffer_store_dwordx2', 4, 6, 16, 128, 0)
    p.sopp('s_endpgm')
    r = T.run(p.bytes(), {0: (256, None), 2: (256, None)}, T.vsharp(0, 32, 2, 256), extra=["image 1 24 5 format 20", "timg 0 0 1"])
    got = struct.unpack('<64I', r[2][:256])
    assert got == (24, 5) * 32, got[:4]


def test_sampler_from_guest_ssharp():
    WRAP, CLAMP_LAST = 0, 2
    POINT, BILINEAR = 0, 1
    cases = [((1.25, WRAP, POINT), 0.0), ((1.25, CLAMP_LAST, POINT), 1.0), ((0.5, CLAMP_LAST, POINT), 1.0), ((0.5, CLAMP_LAST, BILINEAR), 0.5)]
    for (u, clamp, filt), want in cases:
        got = _sample(u, clamp, filt)
        assert all(abs(g - want) < 1e-3 for g in got), ("u", u, "clamp", clamp, "filter", filt, "got", got[:4], "want", want)


def _cube_coords(p):
    p.vop2('v_and_b32', 1, 3, 'v0')
    p.vop2('v_sub_nc_u32', 2, 3, 'v1')
    p.vop2('v_lshrrev_b32', 3, 2, 'v0')


def _cube_value(lane):
    x, layer = lane & 3, lane >> 2
    return x + 10 * (3 - x) + 100 * layer


def test_load_cube_array_r32_uint():
    texels = struct.pack('<192I', *[x + 10 * y + 100 * layer for layer in range(12) for y in range(4) for x in range(4)])
    got = _load_to_buffer(DIM_CUBE, _cube_coords, "image 1 4 4 format 20 dim cube layers 12", texels)
    want = tuple(_cube_value(lane) for lane in range(32))
    assert got == want, (got[:8], want[:8])


def test_store_cube_array_r32_uint():
    p = rdna_asm.Program()
    p.smem('s_load_dwordx8', 8, 4, 0)
    p.sopp('s_waitcnt', 0)
    _cube_coords(p)
    p.vop2('v_mul_u32_u24', 4, 10, 'v2')
    p.vop2('v_add_nc_u32', 4, 'v1', 'v4')
    p.vop2('v_mul_u32_u24', 5, 100, 'v3')
    p.vop2('v_add_nc_u32', 4, 'v5', 'v4')
    p.mimg('image_store', 4, 1, 8, dmask=1, dim=DIM_CUBE)
    p.sopp('s_endpgm')
    r = T.run(p.bytes(), {0: (256, None)}, [], extra=["image 1 4 4 format 20 dim cube layers 12 clear 7 7 7 7", "timg 0 0 1"])
    got = _image(r, 1, 'I', 192)
    want = [7] * 192
    for lane in range(32):
        x, layer = lane & 3, lane >> 2
        want[layer * 16 + (3 - x) * 4 + x] = _cube_value(lane)
    assert got == tuple(want), [(i, got[i], want[i]) for i in range(192) if got[i] != want[i]][:8]


def _sample_cube(layers):
    p = rdna_asm.Program()
    p.smem('s_load_dwordx8', 8, 4, 0)
    p.smem('s_load_dwordx4', 16, 4, 32)
    _out_vsharp(p, 20, 48)
    _out_vsharp(p, 24, 64)
    p.sopp('s_waitcnt', 0)
    p.vop2('v_lshlrev_b32', 5, 4, 'v0')
    p.mubuf('buffer_load_dwordx3', 1, 5, 24, 128, 0)
    p.sopp('s_waitcnt', 0)
    p.mimg('image_sample_lz', 4, 1, 8, dmask=1, ssamp=16, dim=DIM_CUBE)
    p.sopp('s_waitcnt', 0)
    p.vop2('v_lshlrev_b32', 5, 2, 'v0')
    p.mubuf('buffer_store_dword', 4, 5, 20, 128, 0)
    p.sopp('s_endpgm')
    lanes = [(lane & 1, (lane >> 1) & 1, (lane >> 2) % layers) for lane in range(32)]
    coords = b"".join(struct.pack('<4f', 1.0 + (x + 0.5) / 2, 1.0 + (y + 0.5) / 2, 8 * (layer // 6) + layer % 6, 0.0) for x, y, layer in lanes)
    init = os.path.join(T.tempfile.mkdtemp(prefix="img_"), "init.bin")
    open(init, 'wb').write(struct.pack('<%df' % (layers * 4), *[100.0 * layer + 10 * y + x for layer in range(layers) for y in range(2) for x in range(2)]))
    clamp, point = 2, 0
    ssharp = [clamp | (clamp << 3) | (clamp << 6), 0xfff << 12, (point << 20) | (point << 22), 0]
    pokes = [f"poke 0 {32 + 4 * k} {w}" for k, w in enumerate(ssharp)] + T.vsharp(0, 48, 2, 128) + T.vsharp(0, 64, 3, 512)
    r = T.run(p.bytes(), {0: (256, None), 2: (128, None), 3: (512, coords)}, pokes, extra=[f"image 1 2 2 format 22 dim cube layers {layers} init {init}", "timg 0 0 1"])
    got = struct.unpack('<32f', r[2][:128])
    want = tuple(100.0 * layer + 10 * y + x for x, y, layer in lanes)
    assert got == want, [(lane, got[lane], want[lane]) for lane in range(32) if got[lane] != want[lane]][:8]


def test_sample_cube_face_coordinates():
    _sample_cube(6)


def test_sample_cube_array_layer():
    _sample_cube(12)


MIP_SIZE, MIP_LEVELS = 8, 4


def _mip_lanes():
    lanes = []
    for lane in range(32):
        level = lane & 3
        size = MIP_SIZE >> level
        texel = (lane >> 2) * 3 % (size * size)
        lanes.append((texel % size, texel // size, level))
    return lanes


def _mip_value(x, y, level):
    return 1000 * level + 10 * y + x


def _mip_init(pack):
    data = b"".join(pack(_mip_value(x, y, level)) for level in range(MIP_LEVELS) for y in range(MIP_SIZE >> level) for x in range(MIP_SIZE >> level))
    init = os.path.join(T.tempfile.mkdtemp(prefix="img_"), "init.bin")
    open(init, 'wb').write(data)
    return init


def _mip_read(op, fmt, coords, ssharp=None):
    p = rdna_asm.Program()
    p.smem('s_load_dwordx8', 8, 4, 0)
    if ssharp is not None:
        p.smem('s_load_dwordx4', 16, 4, 32)
    _out_vsharp(p, 20, 48)
    _out_vsharp(p, 24, 64)
    p.sopp('s_waitcnt', 0)
    p.vop2('v_lshlrev_b32', 5, 4, 'v0')
    p.mubuf('buffer_load_dwordx3', 1, 5, 24, 128, 0)
    p.sopp('s_waitcnt', 0)
    p.mimg(op, 4, 1, 8, dmask=1, ssamp=16)
    p.sopp('s_waitcnt', 0)
    p.vop2('v_lshlrev_b32', 5, 2, 'v0')
    p.mubuf('buffer_store_dword', 4, 5, 20, 128, 0)
    p.sopp('s_endpgm')
    pokes = [f"poke 0 {32 + 4 * k} {w}" for k, w in enumerate(ssharp or [])] + T.vsharp(0, 48, 2, 128) + T.vsharp(0, 64, 3, 512)
    init = _mip_init(lambda v: struct.pack('<' + fmt, v))
    image = f"image 1 {MIP_SIZE} {MIP_SIZE} format {20 if fmt == 'I' else 22} mips {MIP_LEVELS} init {init}"
    r = T.run(p.bytes(), {0: (256, None), 2: (128, None), 3: (512, coords)}, pokes, extra=[image, "timg 0 0 1"])
    return struct.unpack('<32' + fmt, r[2][:128])


def test_load_mip_r32_uint():
    lanes = _mip_lanes()
    coords = b"".join(struct.pack('<4I', x, y, level, 0) for x, y, level in lanes)
    got = _mip_read('image_load_mip', 'I', coords)
    want = tuple(_mip_value(*lane) for lane in lanes)
    assert got == want, [(lane, got[lane], want[lane]) for lane in range(32) if got[lane] != want[lane]][:8]


def test_sample_explicit_lod_point_mip():
    lanes = _mip_lanes()
    coords = b"".join(struct.pack('<4f', (x + 0.5) / (MIP_SIZE >> level), (y + 0.5) / (MIP_SIZE >> level), float(level), 0.0) for x, y, level in lanes)
    clamp, point_mip = 2, 1
    ssharp = [clamp | (clamp << 3) | (clamp << 6), 0xfff << 12, point_mip << 26, 0]
    got = _mip_read('image_sample_l', 'f', coords, ssharp)
    want = tuple(float(_mip_value(*lane)) for lane in lanes)
    assert got == want, [(lane, got[lane], want[lane]) for lane in range(32) if got[lane] != want[lane]][:8]


def test_sample_grad_lod_clamp():
    lanes = _mip_lanes()
    coords = b"".join(struct.pack('<4f', (x + 0.5) / (MIP_SIZE >> level), (y + 0.5) / (MIP_SIZE >> level), float(level), 0.0) for x, y, level in lanes)
    clamp, point_mip = 2, 1
    ssharp = [clamp | (clamp << 3) | (clamp << 6), 0xfff << 12, point_mip << 26, 0]
    p = rdna_asm.Program()
    p.smem('s_load_dwordx8', 8, 4, 0)
    p.smem('s_load_dwordx4', 16, 4, 32)
    _out_vsharp(p, 20, 48)
    _out_vsharp(p, 24, 64)
    p.sopp('s_waitcnt', 0)
    p.vop2('v_lshlrev_b32', 8, 4, 'v0')
    p.mubuf('buffer_load_dwordx3', 5, 8, 24, 128, 0)
    for r in (1, 2, 3, 4):
        p.vop1('v_mov_b32', r, 0)
    p.sopp('s_waitcnt', 0)
    p.mimg('image_sample_d_cl', 9, 1, 8, dmask=1, ssamp=16)
    p.sopp('s_waitcnt', 0)
    p.vop2('v_lshlrev_b32', 8, 2, 'v0')
    p.mubuf('buffer_store_dword', 9, 8, 20, 128, 0)
    p.sopp('s_endpgm')
    pokes = [f"poke 0 {32 + 4 * k} {w}" for k, w in enumerate(ssharp)] + T.vsharp(0, 48, 2, 128) + T.vsharp(0, 64, 3, 512)
    init = _mip_init(lambda v: struct.pack('<f', v))
    image = f"image 1 {MIP_SIZE} {MIP_SIZE} format 22 mips {MIP_LEVELS} init {init}"
    try:
        r = T.run(p.bytes(), {0: (256, None), 2: (128, None), 3: (512, coords)}, pokes, extra=[image, "timg 0 0 1"])
    except RuntimeError as e:
        if "needs the device's shaderResourceMinLod" in str(e):
            return "SKIP (device lacks shaderResourceMinLod)"
        raise
    got = struct.unpack('<32f', r[2][:128])
    want = tuple(float(_mip_value(*lane)) for lane in lanes)
    assert got == want, [(lane, got[lane], want[lane]) for lane in range(32) if got[lane] != want[lane]][:8]


BC_COLORS = [((31, 0, 0), (0, 0, 31)), ((0, 63, 0), (31, 63, 31)), ((16, 32, 8), (2, 4, 6)), ((31, 63, 31), (0, 0, 0))]
BC_ALPHAS = [(255, 0), (200, 100), (17, 3), (128, 64)]


def _rgb565(c):
    return c[0] << 11 | c[1] << 5 | c[2]


def _bc_index(x, y):
    return (x + y) & 1


def _bc1_block(c0, c1):
    indices = sum(_bc_index(i & 3, i >> 2) << (2 * i) for i in range(16))
    return struct.pack('<HHI', _rgb565(c0), _rgb565(c1), indices)


def _bc3_alpha_block(a0, a1):
    indices = sum(_bc_index(i & 3, i >> 2) << (3 * i) for i in range(16))
    return bytes([a0, a1]) + indices.to_bytes(6, 'little')


def _bc_expected(bc3):
    want = []
    for lane in range(64):
        x, y = lane & 7, lane >> 3
        block = (y >> 2) * 2 + (x >> 2)
        c = BC_COLORS[block][_bc_index(x & 3, y & 3)]
        a = BC_ALPHAS[block][_bc_index(x & 3, y & 3)] / 255 if bc3 else 1.0
        want.append((c[0] / 31, c[1] / 63, c[2] / 31, a))
    return want


def _load_bc(fmt, data):
    p = rdna_asm.Program()
    p.smem('s_load_dwordx8', 8, 4, 0)
    _out_vsharp(p, 16, 32)
    p.sopp('s_waitcnt', 0)
    p.vop2('v_and_b32', 1, 7, 'v0')
    p.vop2('v_lshrrev_b32', 2, 3, 'v0')
    p.mimg('image_load', 4, 1, 8, dmask=0xf)
    p.sopp('s_waitcnt', 0)
    p.vop2('v_lshlrev_b32', 8, 4, 'v0')
    p.mubuf('buffer_store_dwordx4', 4, 8, 16, 128, 0)
    p.sopp('s_endpgm')
    init = os.path.join(T.tempfile.mkdtemp(prefix="img_"), "init.bin")
    open(init, 'wb').write(data)
    r = T.run(p.bytes(), {0: (256, None), 2: (1024, None)}, T.vsharp(0, 32, 2, 1024), wave=64, extra=[f"image 1 8 8 format {fmt} init {init}", "timg 0 0 1"])
    assert open(os.path.join(r['out'], "image1.bin"), 'rb').read() == data
    return [struct.unpack_from('<4f', r[2], 16 * lane) for lane in range(64)]


def _check_bc(got, want):
    bad = [(lane, got[lane], want[lane]) for lane in range(64) if any(abs(g - w) > 1.0 / 255 for g, w in zip(got[lane], want[lane]))]
    assert not bad, bad[:4]


def test_load_bc1_unorm():
    data = b"".join(_bc1_block(*BC_COLORS[block]) for block in range(4))
    _check_bc(_load_bc(169, data), _bc_expected(False))


def test_load_bc3_unorm():
    data = b"".join(_bc3_alpha_block(*BC_ALPHAS[block]) + _bc1_block(*BC_COLORS[block]) for block in range(4))
    _check_bc(_load_bc(173, data), _bc_expected(True))




def _gather_clamp_expected(lanes, ox=0, oy=0):
    corners = ((0, 1), (1, 1), (1, 0), (0, 0))
    want = []
    for x, y, level in lanes:
        size = MIP_SIZE >> level
        for dx, dy in corners:
            tx = max(0, min(size - 1, x + ox + dx))
            ty = max(0, min(size - 1, y + oy + dy))
            want.append(float(_mip_value(tx, ty, level)))
    return tuple(want)


def _gather_mip_lanes():
    # Group lanes by mip level (8 lanes per level) so subgroup-uniform OpImageQuerySizeLod works on Mesa Lavapipe 22.x too.
    lanes = []
    for lane in range(32):
        level = lane // 8
        size = MIP_SIZE >> level
        texel = ((lane & 7) * 3) % (size * size)
        lanes.append((texel % size, texel // size, level))
    return lanes


def test_gather4_lod_clamp():
    lanes = _gather_mip_lanes()
    coords = b"".join(
        struct.pack('<4f', (x + 1.0) / (MIP_SIZE >> level), (y + 1.0) / (MIP_SIZE >> level), float(level), 0.0)
        for x, y, level in lanes
    )
    clamp, point_mip = 2, 1
    ssharp = [clamp | (clamp << 3) | (clamp << 6), 0xfff << 12, point_mip << 26, 0]
    p = rdna_asm.Program()
    p.smem('s_load_dwordx8', 8, 4, 0)
    p.smem('s_load_dwordx4', 16, 4, 32)
    _out_vsharp(p, 20, 48)
    _out_vsharp(p, 24, 64)
    p.sopp('s_waitcnt', 0)
    p.vop2('v_lshlrev_b32', 8, 4, 'v0')
    p.mubuf('buffer_load_dwordx3', 1, 8, 24, 128, 0)
    p.sopp('s_waitcnt', 0)
    p.mimg('image_gather4_cl', 4, 1, 8, dmask=1, ssamp=16)
    p.sopp('s_waitcnt', 0)
    p.mubuf('buffer_store_dwordx4', 4, 8, 20, 128, 0)
    p.sopp('s_endpgm')
    pokes = [f"poke 0 {32 + 4 * k} {w}" for k, w in enumerate(ssharp)] + T.vsharp(0, 48, 2, 512) + T.vsharp(0, 64, 3, 512)
    init = _mip_init(lambda v: struct.pack('<f', v))
    image = f"image 1 {MIP_SIZE} {MIP_SIZE} format 22 mips {MIP_LEVELS} init {init}"
    r = T.run(p.bytes(), {0: (256, None), 2: (512, None), 3: (512, coords)}, pokes, extra=[image, "timg 0 0 1"])
    got = struct.unpack('<128f', r[2][:512])
    want = _gather_clamp_expected(lanes)
    assert got == want, [(i, got[i], want[i]) for i in range(128) if got[i] != want[i]][:8]


def test_gather4_lod_clamp_offset():
    lanes = _gather_mip_lanes()
    ox, oy = 1, -1
    packed_off = (ox & 0x3f) | ((oy & 0x3f) << 8)
    coords = b"".join(
        struct.pack('<I3f', packed_off, (x + 1.0) / (MIP_SIZE >> level), (y + 1.0) / (MIP_SIZE >> level), float(level))
        for x, y, level in lanes
    )
    clamp, point_mip = 2, 1
    ssharp = [clamp | (clamp << 3) | (clamp << 6), 0xfff << 12, point_mip << 26, 0]
    p = rdna_asm.Program()
    p.smem('s_load_dwordx8', 8, 4, 0)
    p.smem('s_load_dwordx4', 16, 4, 32)
    _out_vsharp(p, 20, 48)
    _out_vsharp(p, 24, 64)
    p.sopp('s_waitcnt', 0)
    p.vop2('v_lshlrev_b32', 9, 4, 'v0')
    p.mubuf('buffer_load_dwordx4', 1, 9, 24, 128, 0)
    p.sopp('s_waitcnt', 0)
    p.mimg('image_gather4_cl_o', 5, 1, 8, dmask=1, ssamp=16)
    p.sopp('s_waitcnt', 0)
    p.mubuf('buffer_store_dwordx4', 5, 9, 20, 128, 0)
    p.sopp('s_endpgm')
    pokes = [f"poke 0 {32 + 4 * k} {w}" for k, w in enumerate(ssharp)] + T.vsharp(0, 48, 2, 512) + T.vsharp(0, 64, 3, 512)
    init = _mip_init(lambda v: struct.pack('<f', v))
    image = f"image 1 {MIP_SIZE} {MIP_SIZE} format 22 mips {MIP_LEVELS} init {init}"
    r = T.run(p.bytes(), {0: (256, None), 2: (512, None), 3: (512, coords)}, pokes, extra=[image, "timg 0 0 1"])
    got = struct.unpack('<128f', r[2][:512])
    want = _gather_clamp_expected(lanes, ox=ox, oy=oy)
    assert got == want, [(i, got[i], want[i]) for i in range(128) if got[i] != want[i]][:8]



def test_gather4_bias_lod_clamp():
    lanes = _gather_mip_lanes()
    # Half the lanes select mip via bias > clamp, half via clamp > bias.
    coords = b"".join(
        struct.pack(
            '<4f',
            float(level) if (i & 1) == 0 else 0.0,
            (x + 1.0) / (MIP_SIZE >> level),
            (y + 1.0) / (MIP_SIZE >> level),
            0.0 if (i & 1) == 0 else float(level),
        )
        for i, (x, y, level) in enumerate(lanes)
    )
    clamp, point_mip = 2, 1
    ssharp = [clamp | (clamp << 3) | (clamp << 6), 0xfff << 12, point_mip << 26, 0]
    p = rdna_asm.Program()
    p.smem('s_load_dwordx8', 8, 4, 0)
    p.smem('s_load_dwordx4', 16, 4, 32)
    _out_vsharp(p, 20, 48)
    _out_vsharp(p, 24, 64)
    p.sopp('s_waitcnt', 0)
    p.vop2('v_lshlrev_b32', 9, 4, 'v0')
    p.mubuf('buffer_load_dwordx4', 1, 9, 24, 128, 0)
    p.sopp('s_waitcnt', 0)
    p.mimg('image_gather4_b_cl', 5, 1, 8, dmask=1, ssamp=16)
    p.sopp('s_waitcnt', 0)
    p.mubuf('buffer_store_dwordx4', 5, 9, 20, 128, 0)
    p.sopp('s_endpgm')
    pokes = [f"poke 0 {32 + 4 * k} {w}" for k, w in enumerate(ssharp)] + T.vsharp(0, 48, 2, 512) + T.vsharp(0, 64, 3, 512)
    init = _mip_init(lambda v: struct.pack('<f', v))
    image = f"image 1 {MIP_SIZE} {MIP_SIZE} format 22 mips {MIP_LEVELS} init {init}"
    r = T.run(p.bytes(), {0: (256, None), 2: (512, None), 3: (512, coords)}, pokes, extra=[image, "timg 0 0 1"])
    got = struct.unpack('<128f', r[2][:512])
    want = _gather_clamp_expected(lanes)
    assert got == want, [(i, got[i], want[i]) for i in range(128) if got[i] != want[i]][:8]

TESTS = [test_resinfo_reports_the_image_extent, test_store_rgba8_unorm, test_store_r32_uint, test_load_3d_r32_uint, test_load_1d_r32_uint, test_sampler_from_guest_ssharp, test_load_cube_array_r32_uint, test_store_cube_array_r32_uint, test_sample_cube_face_coordinates, test_sample_cube_array_layer, test_load_mip_r32_uint, test_sample_explicit_lod_point_mip, test_sample_grad_lod_clamp, test_gather4_lod_clamp, test_gather4_lod_clamp_offset, test_gather4_bias_lod_clamp, test_load_bc1_unorm, test_load_bc3_unorm]

if __name__ == "__main__":
    bad = 0
    for t in TESTS:
        try:
            note = t()
            print(note or "PASS", t.__name__)
        except Exception as e:
            bad += 1; print("FAIL", t.__name__, str(e)[:1500])
    sys.exit(1 if bad else 0)
