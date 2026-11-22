"""Descriptor chains: descriptors selected through an index or pointer that the shader loads from memory at run time."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rdna_asm
import exec_tests as T

HEAP, FIRST = 2, 10

def _heap_lines(count, size):
    lines = []
    for i in range(count):
        lines += T.vsharp(HEAP, i * 16, FIRST + i, size)
    return lines

def _check_one_written(r, chosen, count, size):
    want = tuple(1000 + i for i in range(32))
    for i in range(count):
        got = struct.unpack('<32I', r[FIRST + i][:128])
        if i == chosen:
            assert got == want, ("index", chosen, "region", FIRST + i, got[:8])
        else:
            assert got == (0,) * 32, ("index", chosen, "region", FIRST + i, "was written", got[:8])

def _store_lane_ids(p, vsharp_sgpr):
    p.vop2('v_lshlrev_b32', 1, 2, 'v0')
    p.vop2('v_add_nc_u32', 2, 1000, 'v0')
    p.mubuf('buffer_store_dword', 2, 1, vsharp_sgpr, 128, 0)
    p.sopp('s_endpgm')

def test_scalar_index_from_buffer():
    """SRT dword 0 = index, dwords 2-3 = heap pointer. V# = heap[index]; the index exists only in memory."""
    count, size = 4, 128
    for chosen in range(count):
        p = rdna_asm.Program()
        p.smem('s_load_dwordx4', 8, 4, 0)
        p.sopp('s_waitcnt', 0)
        p.sop2('s_lshl_b32', 8, 's8', 4)
        p.smem('s_load_dwordx4', 12, 10, 0, soffset='s8')
        p.sopp('s_waitcnt', 0)
        _store_lane_ids(p, 12)
        regions = {0: (256, None), HEAP: (count * 16, None)}
        regions.update({FIRST + i: (size, None) for i in range(count)})
        pokes = [f"poke 0 0 {chosen}", f"ptrlo 0 8 {HEAP}", f"ptrhi 0 12 {HEAP} 0"] + _heap_lines(count, size)
        r = T.run(p.bytes(), regions, pokes)
        _check_one_written(r, chosen, count, size)

def test_waterfall_over_buffer_loaded_index():
    """SRT: V# of the index buffer at 0, heap pointer at 16. Each lane loads its own heap index, and a waterfall loop picks heap[index] for
    the lanes that share it. Lane i uses index perm[i]; region FIRST + k must hold 1000 + i exactly for the lanes with perm[i] == k."""
    count, size, idx = 4, 128, 1
    perm = [(i * 7 + 3) % count for i in range(32)]
    p = rdna_asm.Program()
    p.smem('s_load_dwordx8', 8, 4, 0)
    p.sopp('s_waitcnt', 0)
    p.vop2('v_lshlrev_b32', 1, 2, 'v0')
    p.mubuf('buffer_load_dword', 3, 1, 8, 128, 0)
    p.sopp('s_waitcnt', 0)
    p.vop2('v_add_nc_u32', 2, 1000, 'v0')
    top = p.label()
    p.vop1('v_readfirstlane_b32', 0, 'v3')
    p.vopc('v_cmp_eq_u32', 's0', 'v3')
    p.sop1('s_and_saveexec_b32', 1, rdna_asm.VCC_LO)
    p.sop2('s_lshl_b32', 0, 's0', 4)
    p.smem('s_load_dwordx4', 16, 12, 0, soffset='s0')
    p.sopp('s_waitcnt', 0)
    p.mubuf('buffer_store_dword', 2, 1, 16, 128, 0)
    p.sop2('s_xor_b32', rdna_asm.EXEC_LO, rdna_asm.EXEC_LO, 's1')
    p.branch_to('s_cbranch_execnz', top)
    p.sopp('s_endpgm')
    regions = {0: (256, None), idx: (128, struct.pack('<32I', *perm)), HEAP: (count * 16, None)}
    regions.update({FIRST + i: (size, None) for i in range(count)})
    pokes = T.vsharp(0, 0, idx, 128) + [f"ptrlo 0 16 {HEAP}", f"ptrhi 0 20 {HEAP} 0"] + _heap_lines(count, size)
    r = T.run(p.bytes(), regions, pokes)
    for k in range(count):
        got = struct.unpack('<32I', r[FIRST + k][:128])
        want = tuple(1000 + i if perm[i] == k else 0 for i in range(32))
        assert got == want, ("region", FIRST + k, [(i, got[i], want[i]) for i in range(32) if got[i] != want[i]][:6])

def test_loop_carried_index_from_buffer():
    """SRT: indices[0..3] at 0, heap pointer at 16. for (i = 0; i != 4; ++i) heap[indices[i]] gets 1000 + lane + 100 * i at lane * 4.
    The descriptor changes every iteration, so no single value of the SRT selects it."""
    count, size = 4, 128
    order = [2, 0, 3, 1]
    p = rdna_asm.Program()
    p.smem('s_load_dwordx4', 12, 4, 16)
    p.sopp('s_waitcnt', 0)
    p.vop2('v_lshlrev_b32', 1, 2, 'v0')
    p.vop2('v_add_nc_u32', 2, 1000, 'v0')
    p.sop1('s_mov_b32', 20, 0)
    top = p.label()
    p.sop2('s_lshl_b32', 21, 's20', 2)
    p.smem('s_load_dword', 22, 4, 0, soffset='s21')
    p.sopp('s_waitcnt', 0)
    p.sop2('s_lshl_b32', 22, 's22', 4)
    p.smem('s_load_dwordx4', 16, 12, 0, soffset='s22')
    p.sopp('s_waitcnt', 0)
    p.mubuf('buffer_store_dword', 2, 1, 16, 128, 0)
    p.vop2('v_add_nc_u32', 2, 100, 'v2')
    p.sop2('s_add_u32', 20, 's20', 1)
    p.sopc('s_cmp_eq_u32', 's20', 4)
    p.branch_to('s_cbranch_scc0', top)
    p.sopp('s_endpgm')
    regions = {0: (256, None), HEAP: (count * 16, None)}
    regions.update({FIRST + i: (size, None) for i in range(count)})
    pokes = [f"poke 0 {4 * i} {order[i]}" for i in range(count)] + [f"ptrlo 0 16 {HEAP}", f"ptrhi 0 20 {HEAP} 0"] + _heap_lines(count, size)
    r = T.run(p.bytes(), regions, pokes)
    for i, k in enumerate(order):
        got = struct.unpack('<32I', r[FIRST + k][:128])
        want = tuple(1000 + lane + 100 * i for lane in range(32))
        assert got == want, ("iteration", i, "region", FIRST + k, got[:6], want[:6])

def test_phi_multiple_offset_image_table():
    """SRT: index-buffer V# at 0, image-heap pointer at 16, output V# at 32, selector at 48.
    A branch computes s_off = (selector == 0 ? cb[0] : cb[1]) << 5 into a Phi (no ReadFirstLane),
    loads the T# from heap + s_off, and reads a texel."""
    count = 3
    idx_data = struct.pack('<4I', 1, 2, 0, 0)
    for selector, want_idx in ((0, 1), (1, 2)):
        p = rdna_asm.Program()
        p.smem('s_load_dwordx4', 8, 4, 0)
        p.smem('s_load_dwordx2', 12, 4, 16)
        p.smem('s_load_dwordx4', 24, 4, 32)
        p.smem('s_load_dword', 28, 4, 48)
        p.sopp('s_waitcnt', 0)
        p.vop2('v_and_b32', 6, 1, 'v0')
        p.vop1('v_readfirstlane_b32', 31, 'v6')
        p.sop2('s_lshl_b32', 31, 's31', 2)
        p.smem('s_buffer_load_dword', 29, 8, 0, soffset='s31')
        p.smem('s_buffer_load_dword', 30, 8, 4, soffset='s31')
        p.sopp('s_waitcnt', 0)
        p.sopc('s_cmp_eq_u32', 's28', 0)
        to_a = p.branch_fwd('s_cbranch_scc1')
        p.sop2('s_lshl_b32', 20, 's30', 5)
        to_join = p.branch_fwd('s_branch')
        p.patch_to_here(to_a)
        p.sop2('s_lshl_b32', 20, 's29', 5)
        p.patch_to_here(to_join)
        p.smem('s_load_dwordx8', 16, 12, 0, soffset='s20')
        p.sopp('s_waitcnt', 0)
        p.vop1('v_mov_b32', 1, 'v0')
        p.vop1('v_mov_b32', 2, 0)
        p.mimg('image_load', 4, 1, 16, dmask=1)
        p.sopp('s_waitcnt', 0)
        p.vop2('v_lshlrev_b32', 5, 2, 'v0')
        p.mubuf('buffer_store_dword', 4, 5, 24, 128, 0)
        p.sopp('s_endpgm')
        extra = [f"image {i + 1} 32 1 format 20 clear {100 * (i + 1)} 0 0 0" for i in range(count)]
        extra += [f"timg {HEAP} {i * 32} {i + 1}" for i in range(count)]
        pokes = T.vsharp(0, 0, 1, 16) + [f"ptrlo 0 16 {HEAP}", f"ptrhi 0 20 {HEAP} 0"] + T.vsharp(0, 32, FIRST, 128) + [f"poke 0 48 {selector}"]
        r = T.run(p.bytes(), {0: (256, None), 1: (16, idx_data), HEAP: (count * 32, None), FIRST: (128, None)}, pokes, extra=extra)
        got = struct.unpack('<32I', r[FIRST][:128])
        assert got == (100 * (want_idx + 1),) * 32, (selector, got[:4])

def test_phi_same_heap_image_loads():
    """SRT: index-buffer V# at 0, image-heap pointer at 16, output V# at 32, selector at 48.
    Two branches each execute s_load_dwordx8 from the same heap at different shifted offsets
    and merge the loaded T# dwords in Phis before image_load."""
    count = 3
    idx_data = struct.pack('<4I', 0, 2, 0, 0)
    for selector, want_idx in ((0, 0), (1, 2)):
        p = rdna_asm.Program()
        p.smem('s_load_dwordx4', 8, 4, 0)
        p.smem('s_load_dwordx2', 12, 4, 16)
        p.smem('s_load_dwordx4', 24, 4, 32)
        p.smem('s_load_dword', 28, 4, 48)
        p.sopp('s_waitcnt', 0)
        p.smem('s_buffer_load_dword', 29, 8, 0)
        p.smem('s_buffer_load_dword', 30, 8, 4)
        p.sopp('s_waitcnt', 0)
        p.sopc('s_cmp_eq_u32', 's28', 0)
        to_a = p.branch_fwd('s_cbranch_scc1')
        p.sop2('s_lshl_b32', 20, 's30', 5)
        p.smem('s_load_dwordx8', 16, 12, 0, soffset='s20')
        to_join = p.branch_fwd('s_branch')
        p.patch_to_here(to_a)
        p.sop2('s_lshl_b32', 20, 's29', 5)
        p.smem('s_load_dwordx8', 16, 12, 0, soffset='s20')
        p.patch_to_here(to_join)
        p.sopp('s_waitcnt', 0)
        p.vop1('v_mov_b32', 1, 'v0')
        p.vop1('v_mov_b32', 2, 0)
        p.mimg('image_load', 4, 1, 16, dmask=1)
        p.sopp('s_waitcnt', 0)
        p.vop2('v_lshlrev_b32', 5, 2, 'v0')
        p.mubuf('buffer_store_dword', 4, 5, 24, 128, 0)
        p.sopp('s_endpgm')
        extra = [f"image {i + 1} 32 1 format 20 clear {200 * (i + 1)} 0 0 0" for i in range(count)]
        extra += [f"timg {HEAP} {i * 32} {i + 1}" for i in range(count)]
        pokes = T.vsharp(0, 0, 1, 16) + [f"ptrlo 0 16 {HEAP}", f"ptrhi 0 20 {HEAP} 0"] + T.vsharp(0, 32, FIRST, 128) + [f"poke 0 48 {selector}"]
        r = T.run(p.bytes(), {0: (256, None), 1: (16, idx_data), HEAP: (count * 32, None), FIRST: (128, None)}, pokes, extra=extra)
        got = struct.unpack('<32I', r[FIRST][:128])
        assert got == (200 * (want_idx + 1),) * 32, (selector, got[:4])

def test_table_image_with_sampler_phi():
    """SRT: selector at 0, image-heap pointer at 8, output V# at 16, idx-buffer V# at 32,
    sampler WRAP at 48, sampler CLAMP at 64. The image handle comes from a waterfall loop
    over a Phi index while the sampler handle is a branch Phi over the two SRT samplers."""
    wrap_s = [0, 0xfff << 12, 0, 0]
    clamp_s = [2 | (2 << 3) | (2 << 6), 0xfff << 12, 0, 0]
    init_path = os.path.join(T.tempfile.mkdtemp(prefix="dchain_"), "img.bin")
    open(init_path, "wb").write(struct.pack("<2f", 1.0, 2.0))
    init_path2 = os.path.join(T.tempfile.mkdtemp(prefix="dchain_"), "img2.bin")
    open(init_path2, "wb").write(struct.pack("<2f", 10.0, 20.0))
    perm = [i & 1 for i in range(32)]
    for selector, want_pair in ((0, (1.0, 10.0)), (1, (2.0, 20.0))):
        p = rdna_asm.Program()
        p.smem('s_load_dword', 2, 4, 0)
        p.smem('s_load_dwordx2', 6, 4, 8)
        p.smem('s_load_dwordx4', 24, 4, 16)
        p.smem('s_load_dwordx4', 28, 4, 32)
        p.sopp('s_waitcnt', 0)
        p.sopc('s_cmp_eq_u32', 's2', 0)
        to_wrap = p.branch_fwd('s_cbranch_scc1')
        p.smem('s_load_dwordx4', 12, 4, 64)
        p.vop2('v_lshlrev_b32', 5, 2, 'v0')
        p.mubuf('buffer_load_dword', 3, 5, 28, 128, 0)
        to_join = p.branch_fwd('s_branch')
        p.patch_to_here(to_wrap)
        p.smem('s_load_dwordx4', 12, 4, 48)
        p.vop2('v_lshlrev_b32', 5, 2, 'v0')
        p.mubuf('buffer_load_dword', 3, 5, 28, 128, 0)
        p.patch_to_here(to_join)
        p.sopp('s_waitcnt', 0)
        p.vop1('v_mov_b32', 1, ('lit', 0x3fa00000))
        p.vop1('v_mov_b32', 2, ('lit', 0x3f000000))
        top = p.label()
        p.vop1('v_readfirstlane_b32', 0, 'v3')
        p.vopc('v_cmp_eq_u32', 's0', 'v3')
        p.sop1('s_and_saveexec_b32', 1, rdna_asm.VCC_LO)
        p.sop2('s_lshl_b32', 0, 's0', 5)
        p.smem('s_load_dwordx8', 16, 6, 0, soffset='s0')
        p.sopp('s_waitcnt', 0)
        p.mimg('image_sample_lz', 4, 1, 16, dmask=1, ssamp=12)
        p.sopp('s_waitcnt', 0)
        p.mubuf('buffer_store_dword', 4, 5, 24, 128, 0)
        p.sop2('s_xor_b32', rdna_asm.EXEC_LO, rdna_asm.EXEC_LO, 's1')
        p.branch_to('s_cbranch_execnz', top)
        p.sopp('s_endpgm')
        extra = [
            f"image 1 2 1 format 22 init {init_path}",
            f"image 2 2 1 format 22 init {init_path2}",
            f"timg {HEAP} 0 1",
            f"timg {HEAP} 32 2",
        ]
        pokes = (
            [f"poke 0 0 {selector}", f"ptrlo 0 8 {HEAP}", f"ptrhi 0 12 {HEAP} 0"]
            + T.vsharp(0, 16, FIRST, 128)
            + T.vsharp(0, 32, 1, 128)
            + [f"poke 0 {48 + 4 * k} {w}" for k, w in enumerate(wrap_s)]
            + [f"poke 0 {64 + 4 * k} {w}" for k, w in enumerate(clamp_s)]
        )
        regions = {0: (256, None), 1: (128, struct.pack('<32I', *perm)), HEAP: (64, None), FIRST: (128, None)}
        r = T.run(p.bytes(), regions, pokes, extra=extra)
        got = struct.unpack('<32f', r[FIRST][:128])
        want = tuple(want_pair[i & 1] for i in range(32))
        assert got == want, (selector, got[:6], want[:6])

def test_nested_pointer_phi_image_load():
    """SRT: sel0 at 0, sel1 at 4, heap pointers A/B/C at 8/16/24, output V# at 32.
    Two sequential branches select the heap base pointer through nested Phis before a
    static-offset s_load_dwordx8 and image_load."""
    for sel0, sel1, want_val in ((0, 0, 111), (1, 0, 222), (0, 1, 333)):
        p = rdna_asm.Program()
        p.smem('s_load_dword', 2, 4, 0)
        p.smem('s_load_dword', 3, 4, 4)
        p.smem('s_load_dwordx4', 24, 4, 32)
        p.smem('s_load_dwordx2', 10, 4, 8)
        p.sopp('s_waitcnt', 0)
        p.sopc('s_cmp_eq_u32', 's2', 0)
        skip_mid = p.branch_fwd('s_cbranch_scc1')
        p.smem('s_load_dwordx2', 10, 4, 16)
        p.patch_to_here(skip_mid)
        p.sopc('s_cmp_eq_u32', 's3', 0)
        skip_hi = p.branch_fwd('s_cbranch_scc1')
        p.smem('s_load_dwordx2', 10, 4, 24)
        p.patch_to_here(skip_hi)
        p.sopp('s_waitcnt', 0)
        p.smem('s_load_dwordx8', 16, 10, 0)
        p.sopp('s_waitcnt', 0)
        p.vop1('v_mov_b32', 1, 'v0')
        p.vop1('v_mov_b32', 2, 0)
        p.mimg('image_load', 4, 1, 16, dmask=1)
        p.sopp('s_waitcnt', 0)
        p.vop2('v_lshlrev_b32', 5, 2, 'v0')
        p.mubuf('buffer_store_dword', 4, 5, 24, 128, 0)
        p.sopp('s_endpgm')
        extra = [
            "image 1 32 1 format 20 clear 111 0 0 0",
            "image 2 32 1 format 20 clear 222 0 0 0",
            "image 3 32 1 format 20 clear 333 0 0 0",
            "timg 2 0 1",
            "timg 3 0 2",
            "timg 4 0 3",
        ]
        pokes = [
            f"poke 0 0 {sel0}",
            f"poke 0 4 {sel1}",
            "ptrlo 0 8 2", "ptrhi 0 12 2 0",
            "ptrlo 0 16 3", "ptrhi 0 20 3 0",
            "ptrlo 0 24 4", "ptrhi 0 28 4 0",
        ] + T.vsharp(0, 32, FIRST, 128)
        regions = {0: (256, None), 2: (64, None), 3: (64, None), 4: (64, None), FIRST: (128, None)}
        r = T.run(p.bytes(), regions, pokes, extra=extra)
        got = struct.unpack('<32I', r[FIRST][:128])
        assert got == (want_val,) * 32, (sel0, sel1, got[:4], want_val)

def test_mixed_readfirstlane_and_scalar_sampler_web():
    """SRT: selector at 0, image T# at 16, output V# at 48, WRAP S# at 64, CLAMP S# at 80.
    Dwords 0..2 of the sampler travel through VGPRs and v_readfirstlane_b32 while dword 3 stays a scalar Phi."""
    wrap_s = [0, 0xfff << 12, 0, 0]
    clamp_s = [2 | (2 << 3) | (2 << 6), 0xfff << 12, 0, 0]
    init_path = os.path.join(T.tempfile.mkdtemp(prefix="dweb_"), "img.bin")
    open(init_path, "wb").write(struct.pack("<2f", 3.0, 7.0))
    for selector, want in ((0, 3.0), (1, 7.0)):
        p = rdna_asm.Program()
        p.smem('s_load_dword', 2, 4, 0)
        p.smem('s_load_dwordx8', 16, 4, 16)
        p.smem('s_load_dwordx4', 24, 4, 48)
        p.smem('s_load_dwordx4', 8, 4, 64)
        p.sopp('s_waitcnt', 0)
        p.sopc('s_cmp_eq_u32', 's2', 0)
        to_a = p.branch_fwd('s_cbranch_scc1')
        p.smem('s_load_dwordx4', 8, 4, 80)
        p.sopp('s_waitcnt', 0)
        for k in range(3):
            p.vop1('v_mov_b32', 10 + k, f's{8 + k}')
        to_join = p.branch_fwd('s_branch')
        p.patch_to_here(to_a)
        for k in range(3):
            p.vop1('v_mov_b32', 10 + k, f's{8 + k}')
        p.patch_to_here(to_join)
        for k in range(3):
            p.vop1('v_readfirstlane_b32', 12 + k, f'v{10 + k}')
        p.sop1('s_mov_b32', 15, 's11')
        p.vop1('v_mov_b32', 1, ('lit', 0x3fa00000))
        p.vop1('v_mov_b32', 2, ('lit', 0x3f000000))
        p.mimg('image_sample_lz', 4, 1, 16, dmask=1, ssamp=12)
        p.sopp('s_waitcnt', 0)
        p.vop2('v_lshlrev_b32', 5, 2, 'v0')
        p.mubuf('buffer_store_dword', 4, 5, 24, 128, 0)
        p.sopp('s_endpgm')
        extra = [f"image 1 2 1 format 22 init {init_path}", "timg 0 16 1"]
        pokes = (
            [f"poke 0 0 {selector}"]
            + T.vsharp(0, 48, FIRST, 128)
            + [f"poke 0 {64 + 4 * k} {w}" for k, w in enumerate(wrap_s)]
            + [f"poke 0 {80 + 4 * k} {w}" for k, w in enumerate(clamp_s)]
        )
        r = T.run(p.bytes(), {0: (256, None), FIRST: (128, None)}, pokes, extra=extra)
        got = struct.unpack('<32f', r[FIRST][:128])
        assert got == (want,) * 32, (selector, got[:4], want)

def test_irreducible_loop_non_dominating_srt_descriptor():
    """SRT: control buffer V# at 0, output buffer V# at 32, R32_UINT storage image T# at 64.
    An irreducible loop enters from two paths; the SRT image descriptor load does not dominate
    the image_atomic_umax after the loop, but every CFG path from the Phi to the atomic passes
    through the load."""
    cases = (
        ((0, 0, 99), (7,) * 32, (50,) * 32),
        ((1, 1, 1), (9,) * 32, (50,) * 32),
        ((1, 0, 99), tuple(51 + lane for lane in range(32)), tuple(51 + lane for lane in range(32))),
    )
    for (c0, c1, c2), want_out, want_img in cases:
        p = rdna_asm.Program()
        p.smem('s_load_dwordx4', 8, 4, 0)
        p.smem('s_load_dwordx4', 24, 4, 32)
        p.sopp('s_waitcnt', 0)
        p.vop2('v_and_b32', 6, 0, 'v0')
        p.vop1('v_readfirstlane_b32', 12, 'v6')
        p.sop2('s_lshl_b32', 12, 's12', 2)
        p.smem('s_buffer_load_dword', 2, 8, 0, soffset='s12')
        p.smem('s_buffer_load_dword', 3, 8, 4, soffset='s12')
        p.smem('s_buffer_load_dword', 6, 8, 8, soffset='s12')
        p.sopp('s_waitcnt', 0)
        p.sop1('s_mov_b32', 16, rdna_asm.EXEC_LO)
        for r in range(17, 24):
            p.sop1('s_mov_b32', r, 0)
        p.sop1('s_mov_b32', 7, 0)
        p.sopc('s_cmp_eq_u32', 's2', 0)
        to_end1 = p.branch_fwd('s_cbranch_scc1')
        p.smem('s_load_dwordx8', 16, 4, 64)
        p.sopp('s_waitcnt', 0)
        p.sopc('s_cmp_eq_u32', 's3', 0)
        to_mid = p.branch_fwd('s_cbranch_scc1')
        loop_top = p.label()
        p.sop2('s_add_u32', 7, 's7', 1)
        p.sopc('s_cmp_eq_u32', 's7', 's6')
        to_end2 = p.branch_fwd('s_cbranch_scc1')
        p.patch_to_here(to_mid)
        p.sop2('s_add_u32', 7, 's7', 2)
        p.sopc('s_cmp_lt_u32', 's7', 8)
        p.branch_to('s_cbranch_scc1', loop_top)
        p.vop1('v_mov_b32', 1, 'v0')
        p.vop1('v_mov_b32', 2, 0)
        p.vop2('v_add_nc_u32', 4, 51, 'v0')
        p.mimg('image_atomic_umax', 4, 1, 16, dmask=1)
        p.sopp('s_waitcnt', 0)
        p.vop2('v_lshlrev_b32', 5, 2, 'v0')
        p.mubuf('buffer_store_dword', 4, 5, 24, 128, 0)
        to_done1 = p.branch_fwd('s_branch')
        p.patch_to_here(to_end2)
        p.vop1('v_mov_b32', 4, 9)
        p.vop2('v_lshlrev_b32', 5, 2, 'v0')
        p.mubuf('buffer_store_dword', 4, 5, 24, 128, 0)
        to_done2 = p.branch_fwd('s_branch')
        p.patch_to_here(to_end1)
        p.vop1('v_mov_b32', 4, 7)
        p.vop2('v_lshlrev_b32', 5, 2, 'v0')
        p.mubuf('buffer_store_dword', 4, 5, 24, 128, 0)
        p.patch_to_here(to_done1)
        p.patch_to_here(to_done2)
        p.sopp('s_endpgm')
        ctrl = struct.pack('<4I', c0, c1, c2, 0)
        extra = ["image 1 32 1 format 20 clear 50 0 0 0", "timg 0 64 1"]
        pokes = T.vsharp(0, 0, 1, 16) + T.vsharp(0, 32, FIRST, 128)
        r = T.run(p.bytes(), {0: (256, None), 1: (16, ctrl), FIRST: (128, None)}, pokes, extra=extra)
        got_out = struct.unpack('<32I', r[FIRST][:128])
        got_img = struct.unpack('<32I', open(os.path.join(r['out'], "image1.bin"), 'rb').read())
        assert got_out == want_out, (c0, c1, c2, got_out[:4], want_out[:4])
        assert got_img == want_img, (c0, c1, c2, got_img[:4], want_img[:4])

TESTS = [test_loop_carried_index_from_buffer, test_scalar_index_from_buffer, test_waterfall_over_buffer_loaded_index, test_phi_multiple_offset_image_table, test_phi_same_heap_image_loads, test_table_image_with_sampler_phi, test_nested_pointer_phi_image_load, test_mixed_readfirstlane_and_scalar_sampler_web, test_irreducible_loop_non_dominating_srt_descriptor]

if __name__ == "__main__":
    bad = 0
    for t in TESTS:
        try:
            t(); print("PASS", t.__name__)
        except Exception as e:
            bad += 1; print("FAIL", t.__name__, str(e)[:1500])
    sys.exit(1 if bad else 0)
