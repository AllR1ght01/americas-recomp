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
import rdna_asm

OP_DECORATE = 71
OP_MEMBER_DECORATE = 72
OP_VARIABLE = 59
OP_ACCESS_CHAIN = 65
DECORATION_BUILTIN = 11
DECORATION_LOCATION = 30
BUILTIN_POSITION = 0
STORAGE_INPUT = 1
STORAGE_OUTPUT = 3
V_MBCNT_LO = 0x365
V_MBCNT_HI = 0x366


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
        yield words[cursor] & 0xFFFF, words[cursor + 1 : cursor + count]
        cursor += count


def _lane_id(p, vdst):
    p.words += [0xD4000000 | (V_MBCNT_LO << 16) | vdst, 193 | (128 << 9)]
    p.words += [0xD4000000 | (V_MBCNT_HI << 16) | vdst, 193 | ((256 + vdst) << 9)]


def _export_half(item_bytes=32, fetch=False):
    p = rdna_asm.Program()
    p.sopp("s_inst_prefetch", 3)
    if fetch:
        p.sop2("s_and_b32", 13, "s13", ("lit", 0xFF))
        p.sop1("s_swappc_b64", 12, "s12")
    _lane_id(p, 1)
    p.vop2("v_mul_u32_u24", 2, item_bytes, "v1")
    p.vop1("v_mov_b32", 3, ("lit", 0x3F800000))
    p.ds("ds_write_b32", 0, 2, data0=3, offset=0)
    p.ds("ds_write_b32", 0, 2, data0=9 if fetch else 3, offset=20)
    p.sopp("s_waitcnt", 0)
    p.sopp("s_barrier")
    p.sop1("s_setpc_b64", 0, "s6")
    p.sopp("s_endpgm")
    return p


class GeometryExportTest(unittest.TestCase):
    def _run(self, program, extra, vin="vin 9 1 0\n"):
        driver = _find_driver()
        if not driver:
            self.skipTest("driver binary not built (set XBOX_DRIVER or build tools/xbox)")
        names = [line.split()[0] for line in program.disasm()]
        self.assertIn("v_mbcnt_lo_u32_b32", names)
        self.assertIn("v_mbcnt_hi_u32_b32", names)
        tmp = tempfile.mkdtemp(prefix="esgs_")
        self.addCleanup(shutil.rmtree, tmp, True)
        code_path = Path(tmp) / "shader.bin"
        spv_path = Path(tmp) / "out.spv"
        vin_path = Path(tmp) / "shader.vin"
        code_path.write_bytes(program.bytes())
        vin_path.write_bytes(vin.encode("utf-8"))
        args = [str(vin_path) if a == "VIN" else a for a in extra]
        cmd = [driver, str(code_path), "32", str(spv_path), "-", "1", "vs"] + args
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        return proc, spv_path

    def _interface(self, spv_path):
        raw = spv_path.read_bytes()
        insts = list(_parse_spirv(struct.unpack("<%dI" % (len(raw) // 4), raw)))
        storage = {ops[1]: ops[2] for op, ops in insts if op == OP_VARIABLE and len(ops) >= 3}
        locations = {ops[0]: ops[2] for op, ops in insts if op == OP_DECORATE and len(ops) >= 3 and ops[1] == DECORATION_LOCATION}
        position_types = {ops[0] for op, ops in insts if op == OP_MEMBER_DECORATE and ops[1:4] == (0, DECORATION_BUILTIN, BUILTIN_POSITION)}
        pointer_types = {ops[0]: ops[2] for op, ops in insts if op == 32 and len(ops) >= 3}
        variable_types = {ops[1]: ops[0] for op, ops in insts if op == OP_VARIABLE}
        position = [v for v, s in storage.items() if s == STORAGE_OUTPUT and pointer_types.get(variable_types[v]) in position_types]
        outputs = {locations[v] for v, s in storage.items() if s == STORAGE_OUTPUT and v in locations}
        inputs = {locations[v] for v, s in storage.items() if s == STORAGE_INPUT and v in locations}
        chains = [ops[2] for op, ops in insts if op == OP_ACCESS_CHAIN and len(ops) >= 3]
        stores = {v: sum(1 for c in chains if c == v) for v in position + [v for v in storage if v in locations and storage[v] == STORAGE_OUTPUT]}
        return position, outputs, inputs, stores

    def _validate(self, spv_path):
        val = _find_spirv_val()
        if val:
            proc = subprocess.run([val, "--target-env", "vulkan1.3", str(spv_path)], capture_output=True, text=True, timeout=30)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_item_writes_become_position_and_parameter_outputs(self):
        proc, spv = self._run(_export_half(), ["-", "8"])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        position, outputs, inputs, stores = self._interface(spv)
        self.assertEqual(len(position), 1)
        self.assertEqual(outputs, {0})
        self.assertEqual(sum(stores.values()), 2)
        self._validate(spv)

    def test_fetch_call_through_masked_user_pair(self):
        proc, spv = self._run(_export_half(fetch=True), ["VIN", "8"])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        position, outputs, inputs, stores = self._interface(spv)
        self.assertEqual(inputs, {0})
        self.assertEqual(outputs, {0})
        self._validate(spv)

    def test_vertex_input_table_whitespace_and_comments(self):
        proc, spv = self._run(_export_half(fetch=True), ["VIN", "8"],
                              vin="# vertex inputs\r\n\r\n  vin\t9  1  0 # attribute zero")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self._interface(spv)[2], {0})
        self._validate(spv)

    def test_malformed_vertex_input_is_rejected_on_its_own_line(self):
        records = ("vin", "vin 9 1", "vin 9 bad 0", "vin 9 1 0junk", "vin 9+1 0", "vin 2147483648 1 0")
        for record in records:
            with self.subTest(record=record):
                vin = f"# vertex inputs\n\n{record}\nvin 9 1 0\n"
                proc, spv = self._run(_export_half(fetch=True), ["VIN", "8"], vin=vin)
                self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
                self.assertIn("THROW malformed vertex input at line 3", proc.stdout)
                self.assertFalse(spv.exists())

    def test_excess_vertex_inputs_report_the_first_over_limit_line(self):
        proc, spv = self._run(_export_half(fetch=True), ["VIN", "8"], vin="vin 9 1 0\n" * 33)
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("THROW too many vertex inputs at line 33", proc.stdout)
        self.assertFalse(spv.exists())

    def test_item_stride_must_match_the_geometry_configuration(self):
        proc, _ = self._run(_export_half(item_bytes=16), ["-", "8"])
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("outside its vertex's ESGS item", proc.stdout)

    def test_without_a_geometry_stage_the_jump_stays_unsupported(self):
        proc, _ = self._run(_export_half(), [])
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("unsupported dynamic s_setpc_b64", proc.stdout)


if __name__ == "__main__":
    unittest.main()
