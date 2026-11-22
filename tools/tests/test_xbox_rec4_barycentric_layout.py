"""Regression tests for selective host interpolation and compact barycentric emulation layout (REC-4)."""

import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

OP_CAPABILITY = 17
OP_TYPE_FLOAT = 22
OP_TYPE_VECTOR = 23
OP_TYPE_ARRAY = 28
OP_TYPE_POINTER = 32
OP_VARIABLE = 59
OP_DECORATE = 71

CAP_FRAGMENT_BARYCENTRIC_KHR = 5284
STORAGE_CLASS_INPUT = 1
DECORATION_FLAT = 14
DECORATION_LOCATION = 30
DECORATION_PER_VERTEX_KHR = 5285


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


def _v_interp_p1_f32(vdst, vsrc, attr, chan):
    return 0xC8000000 | (vdst << 18) | (0 << 16) | (attr << 10) | (chan << 8) | vsrc


def _v_interp_p2_f32(vdst, vsrc, attr, chan):
    return 0xC8000000 | (vdst << 18) | (1 << 16) | (attr << 10) | (chan << 8) | vsrc


def _v_interp_mov_f32(vdst, mode, attr, chan):
    return 0xC8000000 | (vdst << 18) | (2 << 16) | (attr << 10) | (chan << 8) | mode


def _v_add_f32(vdst, vsrc0, vsrc1):
    return 0x06000000 | (vdst << 17) | (vsrc1 << 9) | (0x100 + vsrc0)


def _v_add_f32_imm(vdst, imm_encoding, vsrc1):
    return 0x06000000 | (vdst << 17) | (vsrc1 << 9) | imm_encoding


def _build_mixed_ps(host_attrs, mov_attrs, shifted_attrs=()):
    words = []
    acc = 2
    first = True
    for attr in sorted(set(host_attrs) | set(mov_attrs) | set(shifted_attrs)):
        tmp = 3
        if attr in mov_attrs:
            words.append(_v_interp_mov_f32(tmp, 0, attr, 0))
            words.append(_v_interp_mov_f32(4, 1, attr, 0))
            words.append(_v_add_f32(tmp, tmp, 4))
            words.append(_v_interp_mov_f32(4, 2, attr, 0))
            words.append(_v_add_f32(tmp, tmp, 4))
        elif attr in shifted_attrs:
            words.append(_v_add_f32_imm(5, 0xF0, 0))
            words.append(_v_interp_p1_f32(tmp, 5, attr, 0))
            words.append(_v_interp_p2_f32(tmp, 1, attr, 0))
        else:
            words.append(_v_interp_p1_f32(tmp, 0, attr, 0))
            words.append(_v_interp_p2_f32(tmp, 1, attr, 0))
        if first:
            words.append(0x7E000200 | (acc << 17) | (0x100 + tmp))
            first = False
        else:
            words.append(_v_add_f32(acc, acc, tmp))
    words.extend([0xF800080F, (acc << 24) | (acc << 16) | (acc << 8) | acc, 0xBF810000])
    return struct.pack(f"<{len(words)}I", *words)


def _inspect_inputs(words):
    insts = list(_parse_spirv(words))
    caps = {ops[0] for op, ops in insts if op == OP_CAPABILITY and ops}
    types = {}
    for op, ops in insts:
        if op == OP_TYPE_FLOAT:
            types[ops[0]] = ("float", ops[1])
        elif op == OP_TYPE_VECTOR:
            types[ops[0]] = ("vec", ops[1], ops[2])
        elif op == OP_TYPE_ARRAY:
            types[ops[0]] = ("array", ops[1])
        elif op == OP_TYPE_POINTER:
            types[ops[0]] = ("ptr", ops[1], ops[2])
    locations = {}
    flats = set()
    per_vertex_khr = set()
    for op, ops in insts:
        if op == OP_DECORATE and len(ops) >= 2:
            if ops[1] == DECORATION_LOCATION and len(ops) >= 3:
                locations[ops[0]] = ops[2]
            elif ops[1] == DECORATION_FLAT:
                flats.add(ops[0])
            elif ops[1] == DECORATION_PER_VERTEX_KHR:
                per_vertex_khr.add(ops[0])
    inputs = []
    for op, ops in insts:
        if op == OP_VARIABLE and len(ops) >= 3 and ops[2] == STORAGE_CLASS_INPUT and ops[1] in locations:
            ptr = types.get(ops[0])
            pointee = types.get(ptr[2]) if ptr and ptr[0] == "ptr" else None
            kind = pointee[0] if pointee else "unknown"
            inputs.append((locations[ops[1]], kind, ops[1] in flats))
    return caps, per_vertex_khr, sorted(inputs)


class EmulatedBarycentricLayoutTest(unittest.TestCase):
    def _compile(self, code_bytes, nattr):
        driver = _find_driver()
        if not driver:
            self.skipTest("driver binary not built (set XBOX_DRIVER or build tools/xbox)")
        env = os.environ.copy()
        env.pop("XBOX_FRAGMENT_BARY", None)
        with tempfile.TemporaryDirectory(prefix="rec4_") as tmp:
            code_path = Path(tmp) / "ps.bin"
            bind_path = Path(tmp) / "ps.bind"
            spv_path = Path(tmp) / "ps.spv"
            code_path.write_bytes(code_bytes)
            bind_path.write_text("", encoding="utf-8")
            proc = subprocess.run(
                [driver, str(code_path), "32", str(spv_path), str(bind_path), "1", "ps", "2", str(nattr), "0"],
                capture_output=True,
                text=True,
                timeout=30,
                env=env,
            )
            if proc.returncode != 0:
                return proc.returncode, proc.stdout + proc.stderr, None
            raw = spv_path.read_bytes()
            val = _find_spirv_val()
            if val:
                vp = subprocess.run([val, "--target-env", "vulkan1.3", str(spv_path)], capture_output=True, text=True, timeout=30)
                self.assertEqual(vp.returncode, 0, vp.stdout + vp.stderr)
            words = struct.unpack(f"<{len(raw) // 4}I", raw)
            return 0, proc.stdout, words

    def test_selective_host_interpolation_fits_over_ten_attributes(self):
        code = _build_mixed_ps(host_attrs=range(11), mov_attrs=[11])
        rc, out, words = self._compile(code, 12)
        self.assertEqual(rc, 0, out)
        caps, per_vertex_khr, inputs = _inspect_inputs(words)
        self.assertNotIn(CAP_FRAGMENT_BARYCENTRIC_KHR, caps)
        self.assertFalse(per_vertex_khr)
        vecs = [entry for entry in inputs if entry[1] == "vec"]
        arrays = [entry for entry in inputs if entry[1] == "array"]
        self.assertEqual(len(vecs), 11)
        self.assertEqual(len(arrays), 1)
        self.assertTrue(arrays[0][2])

    def test_compact_layout_resolves_fragmented_passed_and_per_vertex_slots(self):
        host_attrs = [0, 2, 5, 6, 8, 10, 11, 13, 14]
        mov_attrs = [1, 3, 4, 7, 9, 12]
        code = _build_mixed_ps(host_attrs=host_attrs, mov_attrs=mov_attrs)
        rc, out, words = self._compile(code, 15)
        self.assertEqual(rc, 0, out)
        caps, per_vertex_khr, inputs = _inspect_inputs(words)
        self.assertNotIn(CAP_FRAGMENT_BARYCENTRIC_KHR, caps)
        self.assertFalse(per_vertex_khr)
        occupied = set()
        vecs = 0
        arrays = 0
        for loc, kind, is_flat in inputs:
            span = 3 if kind == "array" else 1
            if kind == "array":
                arrays += 1
                self.assertTrue(is_flat)
            else:
                vecs += 1
            for slot in range(loc, loc + span):
                self.assertLess(slot, 32)
                self.assertNotIn(slot, occupied)
                occupied.add(slot)
        self.assertEqual(vecs, 9)
        self.assertEqual(arrays, 6)

    def test_nonstandard_barycentric_attribute_stays_per_vertex(self):
        code = _build_mixed_ps(host_attrs=range(10), mov_attrs=[], shifted_attrs=[10])
        rc, out, words = self._compile(code, 11)
        self.assertEqual(rc, 0, out)
        caps, per_vertex_khr, inputs = _inspect_inputs(words)
        self.assertNotIn(CAP_FRAGMENT_BARYCENTRIC_KHR, caps)
        self.assertFalse(per_vertex_khr)
        arrays = [entry for entry in inputs if entry[1] == "array"]
        self.assertEqual(len(arrays), 1)
        self.assertTrue(arrays[0][2])

    def test_truly_over_32_per_vertex_locations_still_rejected(self):
        code = _build_mixed_ps(host_attrs=[], mov_attrs=range(11))
        rc, out, _ = self._compile(code, 11)
        self.assertNotEqual(rc, 0)
        self.assertIn("exceed 32 locations", out)


if __name__ == "__main__":
    unittest.main()
