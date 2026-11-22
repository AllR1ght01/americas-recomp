"""Emulate the 64-lane wave intrinsics of a lowered Xbox shader with the 64 threads of the group.

usage: python -I wave64_emulate.py in.ll out.ll

Input is the `xbox_dxil_lower.py` output, which has the wave intrinsics of the wave-64 shaders the Xbox compiler
emitted. `dxil_sm66_wave.py` handles them by pinning [WaveSize(64)], and a device whose largest compute wave is 32
lanes (NVIDIA, Intel, WARP) rejects that pipeline. This pass instead replaces each intrinsic with an explicit
emulation over the group's 64 threads: the participating lane writes its value to a group-shared slot, a group
barrier follows, and every lane reads the slots it needs. There is one array per type (i32, i64, f32), so operations
of different types never share bytes, and every emulated call begins with a barrier, so a later call of the same type
cannot overwrite slots an earlier one is still reading. The result uses no wave operation, so it runs unpinned on
a 32-lane device and computes the same 64-lane result.

Covered: WaveGetLaneIndex, WaveIsFirstLane, WaveGetLaneCount, WaveReadLaneFirst, WaveReadLaneAt, WaveActiveBallot,
WaveActiveOp (sum, product, min, max), WaveActiveBit (or, and, xor), WavePrefixOp (sum, product, min, max),
WaveAnyTrue, WaveAllTrue, WaveActiveAllEqual (i32), QuadReadLaneAt and QuadOp, for i32, i64 and f32 values and the
64-bit ballot mask. The quad operations address lanes inside the group of four, so they need no reduction. An
intrinsic with no emulation is left in place and reported (exit status 1), so a caller can tell a complete rewrite
from a partial one.

A group barrier must be reached by every thread of the group, so the emulation is only correct where the wave
operations run in uniform control flow. The pass requires every emulated call to sit in the function's entry block
and refuses the module otherwise; that is the shape the harness kernels have, and whether the BVH builder shaders
share it is not established. It also requires a 64-thread group, from the entry metadata: the slots are indexed by
the 64 threads, so any other group size would read lanes that do not exist.

Float min and max ignore NaN (fcmp + select); a NaN input therefore gives a different result from a hardware
operation. Sum and product of f32 are folded per lane in slot order, so they are not bit-identical to a tree
reduction.

Run order: xbox_dxil_lower.py, then this, then dxil_sm66_wave.py with wave size 0.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from xbox_dxil_lower import insert_decls, renumber

BITS = ("i32", "i64", "float")
GLOBAL = {"i32": "__wave64.i32", "i64": "__wave64.i64", "float": "__wave64.f32"}
ALIGN = {"i32": 4, "i64": 8, "float": 4}

DECLS = {
    "@dx.op.barrier(": "declare void @dx.op.barrier(i32, i32) #1",
    "@dx.op.flattenedThreadIdInGroup.i32(": "declare i32 @dx.op.flattenedThreadIdInGroup.i32(i32) #0",
}

CALL_RE = re.compile(r"^(?P<ind>\s*)(?P<dst>%[\w.$]+) = call (?P<rt>[%.\w]+) @dx\.op\.(?P<op>[\w]+?)(?:\.(?P<sfx>[\w]+))?\((?P<args>.*)\)$")
EXTRACT_RE = re.compile(r"^(?P<ind>\s*)(?P<dst>%[\w.$]+) = extractvalue %dx\.types\.fouri32 (?P<src>%[\w.$]+), (?P<idx>[01])$")
LABEL_RE = re.compile(r"^(?:[-.\w$]+:)|(?:;\s*<label>:)|(?:%\d+:)")
FUNC_RE = re.compile(r"^define [^\n]*\{\n", re.M)


def split_args(text):
    out, depth, current = [], 0, ""
    for ch in text:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(current.strip())
            current = ""
        else:
            current += ch
    if current.strip():
        out.append(current.strip())
    return out


def arg_type(text):
    return text.split()[0]


def arg_value(text):
    return text[len(arg_type(text)):].strip()


class Names:
    def __init__(self):
        self.n = 0

    def __call__(self, hint):
        self.n += 1
        return "%%w64.%s.%d" % (hint, self.n)


def slot(ty, index):
    return "getelementptr inbounds [64 x %s], [64 x %s] addrspace(3)* @%s, i32 0, i32 %s" % (
        ty, ty, GLOBAL[ty], index)


def lane_id(names, used):
    used.add("@dx.op.flattenedThreadIdInGroup.i32(")
    name = names("lane")
    return name, ["%s = call i32 @dx.op.flattenedThreadIdInGroup.i32(i32 96)" % name]


def barrier(used):
    used.add("@dx.op.barrier(")
    return ["call void @dx.op.barrier(i32 80, i32 3)"]


def write_lane(ty, value, names, used):
    lane, lines = lane_id(names, used)
    ptr = names("slot")
    lines.append("%s = %s" % (ptr, slot(ty, lane)))
    lines.append("store %s %s, %s addrspace(3)* %s, align %d" % (ty, value, ty, ptr, ALIGN[ty]))
    return lines


def read_lane(ty, index, names, used, dst=None):
    ptr = names("at")
    lines = ["%s = %s" % (ptr, slot(ty, index))]
    out = dst or names("read")
    lines.append("%s = load %s, %s addrspace(3)* %s, align %d" % (out, ty, ty, ptr, ALIGN[ty]))
    return out, lines


def masked(index, names):
    name = names("index")
    return name, ["%s = and i32 %s, 63" % (name, index)]


def read_first(dst, ty, value, names, used):
    lines = barrier(used) + write_lane(ty, value, names, used) + barrier(used)
    return lines + read_lane(ty, "0", names, used, dst)[1]


def read_at(dst, ty, value, index, names, used):
    idx, lines = masked(index, names)
    lines += barrier(used) + write_lane(ty, value, names, used) + barrier(used)
    return lines + read_lane(ty, idx, names, used, dst)[1]


def fold_step(ty, kind, signed, acc, value, names, dst=None):
    if kind in ("add", "mul", "or", "and", "xor"):
        op = kind if ty != "float" else {"add": "fadd", "mul": "fmul"}[kind]
        name = dst or names("acc")
        return name, ["%s = %s %s %s, %s" % (name, op, ty, acc, value)]
    if ty == "float":
        pred = "olt" if kind == "min" else "ogt"
        cmp_name, sel_name = names("cmp"), dst or names("minmax")
        return sel_name, ["%s = fcmp %s float %s, %s" % (cmp_name, pred, acc, value),
                          "%s = select i1 %s, float %s, float %s" % (sel_name, cmp_name, acc, value)]
    pred = {"min": "slt" if signed else "ult", "max": "sgt" if signed else "ugt"}[kind]
    cmp_name, sel_name = names("cmp"), dst or names("minmax")
    return sel_name, ["%s = icmp %s %s %s, %s" % (cmp_name, pred, ty, acc, value),
                      "%s = select i1 %s, %s %s, %s %s" % (sel_name, cmp_name, ty, acc, ty, value)]


def reduce(dst, ty, kind, signed, names, used):
    acc, lines = read_lane(ty, "0", names, used)
    for i in range(1, 64):
        value, tail = read_lane(ty, str(i), names, used)
        lines += tail
        acc, tail = fold_step(ty, kind, signed, acc, value, names, dst if i == 63 else None)
        lines += tail
    return lines


def ballot(dst, pred, names, used):
    bit = names("bit")
    lines = barrier(used) + ["%s = zext i1 %s to i32" % (bit, pred)]
    lines += write_lane("i32", bit, names, used) + barrier(used)
    acc = None
    for i in range(64):
        word, tail = read_lane("i32", str(i), names, used)
        lines += tail
        one, wide, shifted = names("one"), names("wide"), names("shifted")
        lines += ["%s = and i32 %s, 1" % (one, word),
                  "%s = zext i32 %s to i64" % (wide, one),
                  "%s = shl i64 %s, %d" % (shifted, wide, i)]
        if acc is None:
            acc = shifted
        else:
            joined = names("mask")
            lines.append("%s = or i64 %s, %s" % (joined, acc, shifted))
            acc = joined
    low, high_shift, high = names("low"), names("highpart"), names("high")
    lines += ["%s = trunc i64 %s to i32" % (low, acc),
              "%s = lshr i64 %s, 32" % (high_shift, acc),
              "%s = trunc i64 %s to i32" % (high, high_shift)]
    return lines, (dst, low, high)


def identity(ty, kind, signed):
    if ty == "float":
        return {"add": "0.000000e+00", "mul": "1.000000e+00",
                "min": "0x7FF0000000000000", "max": "0xFFF0000000000000"}[kind]
    bits = 64 if ty == "i64" else 32
    if kind == "add":
        return "0"
    if kind == "mul":
        return "1"
    if kind == "min":
        return str((1 << (bits - 1)) - 1) if signed else "-1"
    return str(-(1 << (bits - 1))) if signed else "0"


def prefix(dst, ty, value, kind, signed, names, used):
    lane, lines = lane_id(names, used)
    lines += barrier(used) + write_lane(ty, value, names, used) + barrier(used)
    acc = identity(ty, kind, signed)
    for i in range(64):
        word, tail = read_lane(ty, str(i), names, used)
        lines += tail
        folded, tail = fold_step(ty, kind, signed, acc, word, names)
        lines += tail
        below = names("below")
        chosen = dst if i == 63 else names("prefix")
        lines += ["%s = icmp slt i32 %d, %s" % (below, i, lane),
                  "%s = select i1 %s, %s %s, %s %s" % (chosen, below, ty, folded, ty, acc)]
        acc = chosen
    return lines


def bool_reduce(dst, pred, kind, names, used):
    lines = barrier(used)
    if kind == "equal":
        value = pred
    else:
        value = names("pred")
        lines.append("%s = zext i1 %s to i32" % (value, pred))
    lines += write_lane("i32", value, names, used) + barrier(used)
    reference = None
    if kind == "equal":
        reference, tail = read_lane("i32", "0", names, used)
        lines += tail
    acc = "1"
    for i in range(64):
        word, tail = read_lane("i32", str(i), names, used)
        lines += tail
        test = names("test")
        if kind == "equal":
            lines.append("%s = icmp eq i32 %s, %s" % (test, word, reference))
        else:
            lines.append("%s = icmp ne i32 %s, 0" % (test, word))
        bit = names("bit")
        lines.append("%s = zext i1 %s to i32" % (bit, test))
        joined = names("acc")
        lines.append("%s = %s i32 %s, %s" % (joined, "or" if kind == "any" else "and", acc, bit))
        acc = joined
    lines.append("%s = icmp ne i32 %s, 0" % (dst, acc))
    return lines


def quad_read(dst, ty, value, index, names, used):
    lane, lines = lane_id(names, used)
    base, idx, target = names("quadbase"), names("quadindex"), names("quadlane")
    lines += ["%s = and i32 %s, -4" % (base, lane)]
    masked_index, tail = masked(index, names)
    lines += tail
    lines += ["%s = and i32 %s, 3" % (idx, masked_index),
              "%s = or i32 %s, %s" % (target, base, idx)]
    lines += barrier(used) + write_lane(ty, value, names, used) + barrier(used)
    return lines + read_lane(ty, target, names, used, dst)[1]


def quad_op(dst, ty, value, kind, names, used):
    lane, lines = lane_id(names, used)
    target = names("quadlane")
    lines.append("%s = xor i32 %s, %d" % (target, lane, (1, 2, 3)[kind]))
    lines += barrier(used) + write_lane(ty, value, names, used) + barrier(used)
    return lines + read_lane(ty, target, names, used, dst)[1]


def emulate_call(line, names, used, left):
    m = CALL_RE.match(line)
    if not m:
        return None
    op, dst, args = m.group("op"), m.group("dst"), split_args(m.group("args"))
    if op == "waveGetLaneIndex":
        used.add("@dx.op.flattenedThreadIdInGroup.i32(")
        return ["%s = call i32 @dx.op.flattenedThreadIdInGroup.i32(i32 96)" % dst], None
    if op == "waveReadLaneFirst" and len(args) == 2 and arg_type(args[1]) in BITS:
        return read_first(dst, arg_type(args[1]), arg_value(args[1]), names, used), None
    if op == "waveReadLaneAt" and len(args) == 3 and arg_type(args[1]) in BITS:
        return read_at(dst, arg_type(args[1]), arg_value(args[1]), arg_value(args[2]), names, used), None
    if op == "waveActiveBallot" and len(args) == 2:
        return ballot(dst, arg_value(args[1]), names, used)
    if op in ("waveActiveOp", "waveActiveBit") and len(args) >= 3 and arg_type(args[1]) in BITS:
        ty, kind_value = arg_type(args[1]), int(arg_value(args[2]))
        if op == "waveActiveOp":
            kind = {0: "add", 1: "mul", 2: "min", 3: "max"}.get(kind_value)
            signed = len(args) > 3 and arg_value(args[3]) == "1"
        else:
            kind = {0: "or", 1: "and", 2: "xor"}.get(kind_value)
            signed = False
        if kind:
            lines = barrier(used) + write_lane(ty, arg_value(args[1]), names, used) + barrier(used)
            return lines + reduce(dst, ty, kind, signed, names, used), None
    if op == "wavePrefixOp" and len(args) >= 3 and arg_type(args[1]) in BITS:
        kind = {0: "add", 1: "mul", 2: "min", 3: "max"}.get(int(arg_value(args[2])))
        signed = len(args) > 3 and arg_value(args[3]) == "1"
        if kind:
            return prefix(dst, arg_type(args[1]), arg_value(args[1]), kind, signed, names, used), None
    if op == "waveGetLaneCount" and len(args) == 1:
        return ["%s = add i32 0, 64" % dst], None
    if op == "waveIsFirstLane" and len(args) == 1:
        lane, lines = lane_id(names, used)
        lines.append("%s = icmp eq i32 %s, 0" % (dst, lane))
        return lines, None
    if op in ("waveAnyTrue", "waveAllTrue") and len(args) == 2:
        return bool_reduce(dst, arg_value(args[1]), "any" if op == "waveAnyTrue" else "all", names, used), None
    if op == "waveActiveAllEqual" and len(args) == 2 and arg_type(args[1]) == "i32":
        return bool_reduce(dst, arg_value(args[1]), "equal", names, used), None
    if op == "quadReadLaneAt" and len(args) == 3 and arg_type(args[1]) in BITS:
        return quad_read(dst, arg_type(args[1]), arg_value(args[1]), arg_value(args[2]), names, used), None
    if op == "quadOp" and len(args) == 3 and arg_type(args[1]) in BITS:
        kind = int(arg_value(args[2]))
        if kind in (0, 1, 2):
            return quad_op(dst, arg_type(args[1]), arg_value(args[1]), kind, names, used), None
    if op.startswith("wave") or op.startswith("quad"):
        left.add("@dx.op.%s(" % op)
    return None


def drop_unused_decls(text):
    for name in set(re.findall(r"^declare [^\n]*@([\w.$]+)\(", text, re.M)):
        if re.search(r"call [^@\n]*@" + re.escape(name) + r"\(", text):
            continue
        text = re.sub(r"; Function Attrs:[^\n]*\n(?=declare [^\n]*@" + re.escape(name) + r"\()", "", text)
        text = re.sub(r"^declare [^\n]*@" + re.escape(name) + r"\([^\n]*\n\n?", "", text, flags=re.M)
    return text


def entry_blocks(text):
    spans, pos = [], 0
    for m in FUNC_RE.finditer(text):
        if m.start() < pos:
            continue
        end = text.find("\n}\n", m.end())
        if end < 0:
            continue
        limit, offset = end, 0
        for raw in text[m.end():end].split("\n"):
            if LABEL_RE.match(raw.strip()):
                limit = m.end() + offset
                break
            offset += len(raw) + 1
        spans.append((m.end(), limit))
        pos = end
    return spans


def group_threads(text):
    props = re.search(r"^!\d+ = !\{i32 0, i64 \d+(?:, i32 \d+, ![^,}]+)*?, i32 4, !(\d+)", text, re.M)
    if not props:
        return None
    dims = re.search(r"^!%s = !\{i32 (\d+), i32 (\d+), i32 (\d+)\}" % props.group(1), text, re.M)
    if not dims:
        return None
    return int(dims.group(1)) * int(dims.group(2)) * int(dims.group(3))


def emulate(text):
    threads = group_threads(text)
    if threads is None:
        raise ValueError("the thread group size is not in the entry metadata, so 64 lanes cannot be assumed")
    if threads != 64:
        raise ValueError("the emulation needs a 64-thread group, this one has %d" % threads)
    names, used, left = Names(), set(), set()
    spans = entry_blocks(text)
    structs = {}
    out, count, offset = [], 0, 0
    for line in text.split("\n"):
        start, offset = offset, offset + len(line) + 1
        picked = EXTRACT_RE.match(line)
        if picked and picked.group("src") in structs:
            low, high = structs[picked.group("src")][1:]
            value = low if picked.group("idx") == "0" else high
            out.append("%s%s = add i32 %s, 0" % (picked.group("ind"), picked.group("dst"), value))
            continue
        result = emulate_call(line, names, used, left)
        if result is None:
            out.append(line)
            continue
        new, struct = result
        if not any(a <= start < b for a, b in spans):
            raise ValueError("a group barrier at this call would not be uniform: " + line.strip())
        indent = line[:len(line) - len(line.lstrip())]
        out += [indent + item for item in new]
        if struct:
            structs[struct[0]] = struct
        count += 1
    for name in list(structs):
        uses = [l for l in out if re.search(re.escape(name) + r"(?![\w.$])", l)]
        if uses:
            raise ValueError("the ballot mask %s is used other than by extractvalue: %s" % (name, uses[0].strip()))
    result = "\n".join(out)
    decls = [decl for key, decl in DECLS.items() if key in used and decl.split("(", 1)[0] not in result]
    if decls:
        result = insert_decls(result, decls)
    globals_used = [ty for ty in BITS if ("@" + GLOBAL[ty]) in result]
    if globals_used:
        block = "\n".join("@%s = internal addrspace(3) global [64 x %s] undef, align %d" % (GLOBAL[ty], ty, ALIGN[ty])
                          for ty in globals_used)
        index = result.find("\ntarget triple")
        if index >= 0:
            end = result.find("\n", index + 1) + 1
            result = result[:end] + "\n" + block + "\n" + result[end:]
        else:
            result = block + "\n\n" + result
    if "%dx.types.fouri32 = type" not in result and "%dx.types.fouri32" in result:
        result = result.replace("%dx.types.Handle = type { i8* }\n",
                                "%dx.types.Handle = type { i8* }\n%dx.types.fouri32 = type { i32, i32, i32, i32 }\n", 1)
    result = drop_unused_decls(result)
    return renumber(result), count, left


if __name__ == "__main__":
    text = open(sys.argv[1], encoding="utf-8", errors="ignore").read()
    try:
        res, n, left = emulate(text)
    except ValueError as error:
        sys.exit("wave64_emulate: " + str(error))
    open(sys.argv[2], "w", encoding="utf-8", newline="\n").write(res)
    print("emulated %d call(s); intrinsics left: %s" % (n, sorted(left) or "none"))
    sys.exit(1 if left else 0)
