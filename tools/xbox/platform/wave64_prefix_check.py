"""Check wave64_emulate.py's prefix and boolean wave operations against a software model, on a dump of wave64_prefix_kernel.ll.

per thread input (u0, 16 bytes): [x as u32, i as i32, the kernel converts i to f32]; per thread output (u1, 48 bytes), 12 dwords:
 [laneCount, isFirstLane, prefix sum(x), prefix min(x) signed] [prefix max(x) unsigned, prefix sum(f), prefix product(x), anyTrue]
 [allTrue, allEqual(x & 1), lane, -]
The prefix is exclusive: lane 0 gets the identity of the operation. anyTrue/allTrue/allEqual are over all 64 lanes.

usage: python -I wave64_prefix_check.py <u0.bin> <u1.bin> [groups]
"""
import struct
import sys

u0 = open(sys.argv[1], "rb").read()
u1 = open(sys.argv[2], "rb").read()
groups = int(sys.argv[3]) if len(sys.argv) > 3 else 64


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def f32_bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


bad, first = {}, {}


def chk(name, group, lane, got, expected):
    if got != expected:
        bad[name] = bad.get(name, 0) + 1
        first.setdefault(name, (group, lane, got, expected))


for g in range(groups):
    base = 16 * g * 64
    x = [struct.unpack_from("<I", u0, base + 16 * L)[0] for L in range(64)]
    f = [f32(float(struct.unpack_from("<i", u0, base + 16 * L + 4)[0])) for L in range(64)]
    signed = [v - (1 << 32) if v & 0x80000000 else v for v in x]
    odd = [v & 1 for v in x]
    for L in range(64):
        total, product, lowest, float_total = 0, 1, 0x7FFFFFFF, None
        highest = 0
        for i in range(L):
            total = (total + signed[i]) & 0xFFFFFFFF
            product = (product * x[i]) & 0xFFFFFFFF
            lowest = min(lowest, signed[i])
            highest = max(highest, x[i])
            float_total = f[i] if float_total is None else f32(float_total + f[i])
        o = struct.unpack_from("<11I", u1, 48 * (g * 64 + L))
        chk("lane_count", g, L, o[0], 64)
        chk("is_first", g, L, o[1], 1 if L == 0 else 0)
        chk("prefix_sum", g, L, o[2], total)
        chk("prefix_min_signed", g, L, o[3], lowest & 0xFFFFFFFF)
        chk("prefix_max_unsigned", g, L, o[4], highest)
        chk("prefix_sum_f32", g, L, o[5], f32_bits(0.0 if float_total is None else float_total))
        chk("prefix_product", g, L, o[6], product)
        chk("any_true", g, L, o[7], 1 if any(odd) else 0)
        chk("all_true", g, L, o[8], 1 if all(odd) else 0)
        chk("all_equal", g, L, o[9], 1 if all(v == odd[0] for v in odd) else 0)
        chk("lane", g, L, o[10], L)
print("%d groups x 64 lanes checked; mismatches: %s" % (groups, bad or "none"))
for k, v in first.items():
    print("first", k, "(group, lane, got, expected)", v)
