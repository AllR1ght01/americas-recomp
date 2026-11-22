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
    flag bits 0-1 are the interpolation: 1 = flat (constant over the primitive, FLAT_SHADE), 2 = interpolated. Checked on
    all 12,101 carved PS (12,091 have a table; the other 10 read 12 or 13 interpolants but have no entry at 0x138): every
    entry has exactly one of the two bits; the 4,295 interpolants the code reads only with v_interp_mov_f32 P0 all have
    bit 0, and no bit-0 interpolant is read any other way; the 49,594 read with v_interp_p1/p2 (32-bit) or the 16-bit
    interpolation instructions all have bit 1. So "read only at P0" in the code and bit 0 in the table are the same mask.
NOT decoded: the trailing sections (offsets in the directory at 0xf0/0x120/0x130 point past the code), the per-shader
register-like words at 0xd4/0xf8/0xfc/0x110/0x12c, flag value 10 (0x0a), vertex/mesh output tables near 0x1d0.
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


def interpolants(blob: bytes):
    """(count, fp16 bitmask by table position) as the recompiler's pixel context wants them."""
    ent = pixel_inputs(blob)
    return len(ent), sum(1 << k for k, e in enumerate(ent) if e[4] & 0x10)


def flat_mask(blob: bytes) -> int:
    """Bitmask by table position of the flat interpolants (flags bits 0-1 == 1), the FLAT_SHADE bits of the pixel context."""
    return sum(1 << k for k, e in enumerate(pixel_inputs(blob)) if (e[4] & 3) == 1)


def directory_ok(blob: bytes) -> bool:
    g = lambda o: struct.unpack_from('<I', blob, o)[0]
    return g(0x120) + g(0x12c) == g(0x130)


if __name__ == '__main__':
    b = open(sys.argv[1], 'rb').read()
    print('directory ok:', directory_ok(b))
    for e in pixel_inputs(b):
        print('semantic %d mask %#x index %d slot %d flags %#x' % e)
    print('interpolants (count, fp16 mask):', interpolants(b))
    print('flat mask: %#x' % flat_mask(b))
