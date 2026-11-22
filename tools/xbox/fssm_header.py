"""Decode the parts of the Xbox FSSM shader blob header that have been verified against the shader code.

Verified (738 pixel shaders, 2,647 compute, 114 VS, 640 MS; see docs/xbox/XBOX_SHADERS.md):
  * section directory: u32 at 0x120 (offset) + u32 at 0x12c (size) == u32 at 0x130 (next offset)
    (holds for all PS/MS and 104/114 VS; compute 2,617/2,647; the exceptions are not understood)
  * pixel shader input table at 0x138: 8-byte entries while byte0 == 6
        byte1 = semantic id, byte2 = component mask, byte3 = running index, byte6 = parameter slot, byte7 = flags
    entry count == number of interpolants read by the code in 719/738 PS (the rest declare one extra, unread input);
    the code's interpolant number (v_interp attr) is the entry's POSITION in the table, not byte6 - 1;
    flag bit 4 (0x10) marks the 16-bit interpolants: consistent with the code in 736/738, bit 3 (0x08) covers all 738
    but also flags a few interpolants the code reads as 32-bit. Feeding the table-derived count and bit-4 mask to the
    recompiler gives identical results to the code-derived ones on all 738 PS.
  * vertex shader input table, also at 0x138: 5-byte entries while byte0 == 6
        byte1 = input register (ISG1 register), byte2 = component mask the code reads (ISG1 read/write mask),
        byte3 = first VGPR, byte4 = highest component read (so byte4 + 1 VGPRs)
    checked on all 2,236 carved VS: the table lists exactly the ISG1 inputs whose read mask is not zero, in order; the 788 with
    entries are the 630 that open with `s_swappc_b64 s[4:5], s[4:5]` (first VGPR 4, 8, 12, ...) and the 158 merged ones
    (first VGPR 9, 13, ...); the 1,448 without entries have no fetch call. In the 630 every VGPR above v3 that the code reads
    before writing lies in the table (626; the other 4 are not settled by the approximate decoder used for the check).
    So the table is the fetch subroutine's output layout: which VGPRs it fills with which input.
NOT decoded: the trailing sections (offsets in the directory at 0xf0/0x120/0x130 point past the code), the per-shader
register-like words at 0xd4/0xf8/0xfc/0x110/0x12c, flag values 1/9/10, vertex/mesh output tables near 0x1d0.
The record tables after the code have code-offset-like first fields that do NOT land on instruction boundaries, so they
are not per-instruction annotations.
"""
import struct, sys


def pixel_inputs(blob: bytes):
    """Return [(semantic, mask, index, slot, flags)] from the pixel input table."""
    out, p = [], 0x138
    while p + 8 <= 0x1d0 and blob[p] == 6:
        out.append((blob[p + 1], blob[p + 2], blob[p + 3], blob[p + 6], blob[p + 7]))
        p += 8
    return out


def vertex_inputs(blob: bytes):
    """Return [(input register, read mask, first vgpr, vgpr count)] from a vertex shader's input table (empty when it has none)."""
    out, p = [], 0x138
    while p + 5 <= 0x1d0 and blob[p] == 6:
        out.append((blob[p + 1], blob[p + 2], blob[p + 3], blob[p + 4] + 1))
        p += 5
    return out


def vin_text(blob: bytes) -> str:
    """The .vin file driver.cpp reads for a vertex shader: one "vin <first vgpr> <vgpr count> <input register>" line per input."""
    return "".join("vin %d %d %d\n" % (vgpr, count, reg) for reg, _, vgpr, count in vertex_inputs(blob))


def interpolants(blob: bytes):
    """(count, fp16 bitmask by table position) as the recompiler's pixel context wants them."""
    ent = pixel_inputs(blob)
    return len(ent), sum(1 << k for k, e in enumerate(ent) if e[4] & 0x10)


def directory_ok(blob: bytes) -> bool:
    g = lambda o: struct.unpack_from('<I', blob, o)[0]
    return g(0x120) + g(0x12c) == g(0x130)


if __name__ == '__main__':
    b = open(sys.argv[1], 'rb').read()
    print('directory ok:', directory_ok(b))
    for e in vertex_inputs(b):
        print('vertex input register %d mask %#x vgpr %d count %d' % e)
    for e in pixel_inputs(b):
        print('semantic %d mask %#x index %d slot %d flags %#x' % e)
    print('interpolants (count, fp16 mask):', interpolants(b))
