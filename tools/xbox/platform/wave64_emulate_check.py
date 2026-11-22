"""Check wave64_emulate.py against a software model, on a dump of wave64_emulate_kernel.ll run unpinned on a 32-lane device.

per thread input (u0, 16 bytes): [x as u32, i as i32, the kernel converts i to f32]; per thread output (u1, 64 bytes), 16 dwords:
 [sum(x), min(x) signed, max(x) unsigned, product(x)] [or(x), and(x), xor(x), sum(f))]
 [first(f), f[lane ^ 17], first64 low, first64 high] [lane, quadReadLaneAt(x, 2), f[lane ^ 1], x[lane ^ 3]]
first64 is waveReadLaneFirst of the 64-bit value lane 0 of the group builds from its two input dwords.

usage: python -I wave64_emulate_check.py <u0.bin> <u1.bin> [groups]
The fold order is the one the emulation uses (slot 0 to 63), so sum and product of f32 are checked as an ordered
fold of single-precision additions, not as a tree reduction.
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
    total = sum(signed) & 0xFFFFFFFF
    lowest = min(signed) & 0xFFFFFFFF
    highest = max(x)
    product = 1
    for v in x:
        product = (product * v) & 0xFFFFFFFF
    joined = 0
    for v in x:
        joined |= v
    common = 0xFFFFFFFF
    for v in x:
        common &= v
    flipped = 0
    for v in x:
        flipped ^= v
    acc = f[0]
    for i in range(1, 64):
        acc = f32(acc + f[i])
    wide = struct.unpack_from("<I", u0, base)[0] | (struct.unpack_from("<I", u0, base + 4)[0] << 32)
    for L in range(64):
        o = struct.unpack_from("<16I", u1, 64 * (g * 64 + L))
        chk("sum", g, L, o[0], total)
        chk("min_signed", g, L, o[1], lowest)
        chk("max_unsigned", g, L, o[2], highest)
        chk("product", g, L, o[3], product)
        chk("or", g, L, o[4], joined)
        chk("and", g, L, o[5], common)
        chk("xor", g, L, o[6], flipped)
        chk("sum_f32", g, L, o[7], f32_bits(acc))
        chk("first_f32", g, L, o[8], f32_bits(f[0]))
        chk("readlane_f32", g, L, o[9], f32_bits(f[L ^ 17]))
        chk("first_i64_low", g, L, o[10], wide & 0xFFFFFFFF)
        chk("first_i64_high", g, L, o[11], wide >> 32)
        chk("lane", g, L, o[12], L)
        chk("quad_read", g, L, o[13], x[(L & ~3) | 2])
        chk("quad_across_x", g, L, o[14], f32_bits(f[L ^ 1]))
        chk("quad_diagonal", g, L, o[15], x[L ^ 3])
print("%d groups x 64 lanes checked; mismatches: %s" % (groups, bad or "none"))
for k, v in first.items():
    print("first", k, "(group, lane, got, expected)", v)
