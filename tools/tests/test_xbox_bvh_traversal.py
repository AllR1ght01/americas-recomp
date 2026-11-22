import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "xbox"))
os.environ.setdefault("XBOX_RDNA2", "recomp")
import bvh_rehydrate_cpu as bvh
import exec_bvh_tests as B

OP_EXT_INST = 12
OP_F_UNORD_LESS_THAN = 185
OP_F_UNORD_GREATER_THAN = 187
GLSL_NMIN = 79
GLSL_NMAX = 80


def _find_driver():
    env = os.environ.get("XBOX_DRIVER")
    if env and Path(env).is_file():
        return env
    for name in ("driver", "driver.exe"):
        candidate = ROOT / "build-xbox" / name
        if candidate.is_file():
            return str(candidate)
    return None


def _find_spirv_val():
    env = os.environ.get("XBOX_SPIRV_VAL")
    if env and Path(env).is_file():
        return env
    return shutil.which("spirv-val")


def _parse_spirv(words):
    cursor = 5
    while cursor < len(words):
        count = words[cursor] >> 16
        op = words[cursor] & 0xFFFF
        yield op, words[cursor + 1 : cursor + count]
        cursor += count


def _nmin(a, b):
    if math.isnan(a):
        return b
    if math.isnan(b):
        return a
    return a if a < b else b


def _nmax(a, b):
    if math.isnan(a):
        return b
    if math.isnan(b):
        return a
    return a if a > b else b


def _box_slab_hit(origin, inv, extent, bounds):
    if math.isnan(extent) or any(math.isnan(c) for c in origin + inv + bounds):
        return False, float("inf")
    enter = None
    leave = None
    for ax in range(3):
        p0 = (bounds[ax] - origin[ax]) * inv[ax]
        p1 = (bounds[ax + 3] - origin[ax]) * inv[ax]
        near, far = (p0, p1) if inv[ax] >= 0.0 else (p1, p0)
        enter = near if enter is None else _nmax(enter, near)
        leave = far if leave is None else _nmin(leave, far)
    if math.isnan(enter) or math.isnan(leave):
        return False, float("inf")
    min_t = _nmax(enter, 0.0)
    max_t = _nmin(leave, extent)
    return (min_t <= max_t), min_t


def _intersect_leaf(buf, nid, origin, direction):
    noff = (nid >> 3) << 6
    ntype = nid & 7
    d = struct.unpack_from("<12f4I", buf, noff)
    if ntype == bvh.NODE_TYPE_TRI0:
        v1, v2, v3 = d[0:3], d[3:6], d[6:9]
    else:
        v1, v2, v3 = d[3:6], d[9:12], d[6:9]
    sub = lambda a, b: (a[0] - b[0], a[1] - b[1], a[2] - b[2])
    dot = lambda a, b: a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
    cross = lambda a, b: (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
    e1, e2, e3 = sub(v2, v1), sub(v3, v1), sub(origin, v1)
    s1, s2 = cross(direction, e2), cross(e3, e1)
    t_num = dot(e2, s2)
    t_denom = dot(s1, e1)
    i_num = dot(e3, s1)
    j_num = dot(direction, s2)
    if t_denom == 0.0 or math.isnan(t_denom):
        return None
    t = t_num / t_denom
    u = i_num / t_denom
    v = j_num / t_denom
    if not (0.0 <= u <= 1.0 and 0.0 <= v <= 1.0 and u + v <= 1.0 and t >= 0.0):
        return None
    tri_id = d[15]
    shift = ntype * 8
    bary = (t_denom - i_num - j_num, i_num, j_num)
    swizzled_i = bary[(tri_id >> shift) & 3]
    swizzled_j = bary[(tri_id >> (shift + 2)) & 3]
    return t, swizzled_i / t_denom, swizzled_j / t_denom, t_denom > 0.0


def _cpu_bvh_trace(buf, root_id, origin, direction, extent):
    inv = B.inv_dir(direction)
    stack = [root_id]
    best = None
    best_t = extent
    while stack:
        nid = stack.pop()
        if nid == B.INVALID:
            continue
        ntype = nid & 7
        noff = (nid >> 3) << 6
        if ntype in (bvh.NODE_TYPE_TRI0, bvh.NODE_TYPE_TRI1):
            hit = _intersect_leaf(buf, nid, origin, direction)
            if hit is not None and hit[0] < best_t:
                best_t = hit[0]
                best = (nid, hit[0], hit[1], hit[2], hit[3])
            continue
        cids = struct.unpack_from("<4I", buf, noff)
        if ntype == bvh.NODE_TYPE_BOX32:
            raw = struct.unpack_from("<24f", buf, noff + 16)
            boxes = [raw[s * 6 : s * 6 + 6] for s in range(4)]
        else:
            halves = struct.unpack_from("<24H", buf, noff + 16)
            boxes = [tuple(struct.unpack("<e", struct.pack("<H", halves[s * 6 + k]))[0] for k in range(6)) for s in range(4)]
        hits = []
        for s in range(4):
            ok, dist = _box_slab_hit(origin, inv, best_t, boxes[s])
            if ok and cids[s] != B.INVALID:
                hits.append((dist, cids[s]))
        hits.sort(key=lambda item: item[0])
        for _, cid in reversed(hits):
            stack.append(cid)
    return best


class BvhTraversalTest(unittest.TestCase):
    def test_cpu_traversal_matches_bruteforce_on_rehydrated_bvh(self):
        bvh_bytes, root_id, scene_tris = B.build_multilevel_xbox_bvh()
        rep = bvh.validate_rehydrated_bvh(bvh_bytes, check_aabb_enclosure=True, check_parent_pointers=True)
        self.assertTrue(rep.valid, rep.errors)
        self.assertEqual(rep.max_depth, 3)
        self.assertGreaterEqual(rep.visited_box32, 3)
        self.assertGreaterEqual(rep.visited_box16, 2)
        for idx, (origin, direction, extent) in enumerate(B.build_scene_rays()):
            got = _cpu_bvh_trace(bvh_bytes, root_id, origin, direction, extent)
            want = B.ref_scene_trace(scene_tris, origin, direction, extent)
            if want is None:
                self.assertIsNone(got, (idx, got))
            else:
                self.assertIsNotNone(got, (idx, want))
                self.assertEqual(got[0], want[0], idx)
                self.assertAlmostEqual(got[1], want[1], places=5, msg=idx)
                self.assertAlmostEqual(got[2], want[2], places=5, msg=idx)
                self.assertAlmostEqual(got[3], want[3], places=5, msg=idx)
                self.assertEqual(got[4], want[4], idx)

    def test_bvh_traversal_shader_compiles_and_emits_nan_guards(self):
        driver = _find_driver()
        if not driver:
            self.skipTest("driver binary not built (set XBOX_DRIVER or build tools/xbox)")
        val = _find_spirv_val()
        bind_text = "node 0 user 4\ndesc 0 0 buf\ndesc 0 16 buf\ndesc 0 32 buf\n"
        for node64 in (False, True):
            for wave in (32, 64):
                with self.subTest(node64=node64, wave=wave):
                    with tempfile.TemporaryDirectory(prefix="bvh_trav_") as tmp:
                        code_path = Path(tmp) / "trav.bin"
                        bind_path = Path(tmp) / "trav.bind"
                        spv_path = Path(tmp) / "trav.spv"
                        code_path.write_bytes(B.assemble_traversal_shader(node64=node64, wave=wave))
                        bind_path.write_text(bind_text, encoding="utf-8")
                        proc = subprocess.run(
                            [driver, str(code_path), str(wave), str(spv_path), str(bind_path)],
                            capture_output=True,
                            text=True,
                            timeout=30,
                        )
                        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                        raw = spv_path.read_bytes()
                        words = struct.unpack("<%dI" % (len(raw) // 4), raw)
                        insts = list(_parse_spirv(words))
                        opcodes = {op for op, _ in insts}
                        self.assertIn(OP_F_UNORD_LESS_THAN, opcodes)
                        self.assertIn(OP_F_UNORD_GREATER_THAN, opcodes)
                        ext_ops = {ops[3] for op, ops in insts if op == OP_EXT_INST and len(ops) >= 4}
                        self.assertIn(GLSL_NMIN, ext_ops)
                        self.assertIn(GLSL_NMAX, ext_ops)
                        if val:
                            vp = subprocess.run(
                                [val, "--target-env", "vulkan1.3", str(spv_path)],
                                capture_output=True,
                                text=True,
                                timeout=30,
                            )
                            self.assertEqual(vp.returncode, 0, vp.stdout + vp.stderr)


if __name__ == "__main__":
    unittest.main()
