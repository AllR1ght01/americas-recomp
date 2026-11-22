"""Check the 64-bit address lowering of xbox_dxil_lower.py with address_kernel.ll on dispatch_test's heap-indexed mode.

The kernel reaches u0 at 0x123456780000 and u1 at 0x1FFFFF000 (a range that crosses 4 GiB) only through 64-bit addresses. The table
(heap slot 2) lists both, two decoy ranges and padding, sorted by base: a 16-byte header, then 4096 entries of
(base low, base high, size, heap index); unused entries have base 0xFFFFFFFFFFFFFFFF.

  python -I address_check.py table table.bin
  python -I xbox_dxil_lower.py address_kernel.ll k.ll 2 ; python -I dxil_sm66_wave.py k.ll k66.ll 0 ; dxasm k66.ll k.dxil
  set DT_RAW=1, DT_TABLE=table.bin, DT_DUMP=<prefix> ; dispatch_test k.dxil 16
  python -I address_check.py check <prefix>_gpu_u0.bin <prefix>_gpu_u1.bin 1024

Thread t (of N) loads 1, 2, 3 and 4 dwords at 4t, 8t, 12t and 16t of u0 and one group-uniform dword (s_load) at 256 * group, and
stores them with 4-, 3-, 1- and 4-dword stores at 64t of u1. At 1 MiB into u1: 16 counters that every thread adds 1 or subtracts
2 from, and 8 float minimum and 8 float maximum cells over (int(u0[t]) >> 8)."""
import struct, sys

U0, U1, ENTRIES = 0x123456780000, 0x1FFFFF000, 4096


def table(path):
    rows = sorted([(0x1000, 0x1000, 1), (U1, 8 << 20, 1), (0x1000000000, 0x10000, 0), (U0, 4 << 20, 0)])
    rows += [((1 << 64) - 1, 0, 0)] * (ENTRIES - len(rows))
    data = struct.pack("<4I", len(rows), 0, 0, 0) + b"".join(struct.pack("<4I", b & 0xFFFFFFFF, b >> 32, s, h) for b, s, h in rows)
    open(path, "wb").write(data)
    print("wrote", path, len(data), "bytes")


def check(u0_path, u1_path, n):
    u0 = open(u0_path, "rb").read()
    u1 = open(u1_path, "rb").read()
    w = lambda k: struct.unpack_from("<I", u0, 4 * k)[0]
    bad, first = 0, None
    for t in range(n):
        exp = [w(t), w(2 * t), w(2 * t + 1), w(3 * t), w(3 * t + 1), w(3 * t + 2), w(4 * t), w(4 * t + 1), w(4 * t + 2), w(4 * t + 3),
               w((t // 64) * 64), 0xFF7FFFFF]
        got = list(struct.unpack_from("<12I", u1, 64 * t))
        if got != exp:
            bad += 1
            first = first or (t, got, exp)
    base = 1 << 20
    add = struct.unpack_from("<16I", u1, base)
    sub = struct.unpack_from("<16I", u1, base + 64)
    fmin = struct.unpack_from("<8f", u1, base + 128)
    fmax = struct.unpack_from("<8f", u1, base + 160)
    val = [float(struct.unpack("<i", struct.pack("<I", w(t)))[0] >> 8) for t in range(n)]
    emin = [min([0.0] + [val[t] for t in range(n) if t % 8 == k]) for k in range(8)]
    emax = [max([0.0] + [val[t] for t in range(n) if t % 8 == k]) for k in range(8)]
    atom = [("add", list(add), [n // 16] * 16), ("sub", list(sub), [(-2 * (n // 16)) & 0xFFFFFFFF] * 16),
            ("fmin", list(fmin), emin), ("fmax", list(fmax), emax)]
    abad = [(k, g, e) for k, g, e in atom if g != e]
    print("%d threads: %d with wrong loads/stores%s; atomics %s" % (n, bad, "" if first is None else " (first %r)" % (first,),
          "all as expected" if not abad else abad))
    return 1 if bad or abad else 0


if __name__ == "__main__":
    if sys.argv[1] == "table":
        table(sys.argv[2])
    else:
        sys.exit(check(sys.argv[2], sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 1024))
