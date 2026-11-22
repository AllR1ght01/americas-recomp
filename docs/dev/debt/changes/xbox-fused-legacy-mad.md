# shader

## change [v_mad_legacy_f32 and v_mac_legacy_f32]

- [v_mad_legacy_f32 and v_mac_legacy_f32](../../../core/shader/recompiler/Translation/src/FloatInstructions.cpp) (VOP3 0x140, VOP2 0x06) are fused for the Xbox Series guest, but the Series X GPU is not measured: on gfx1031 (RX 6700 XT, boykopovar/AnyPS5#1281) 0x140 is `v_fma_legacy_f32`, bit-exact with `v_fma_f32` in every IEEE/denorm mode. VOP2 0x06, NaN results and a product below the f32 range with a zero addend have no RDNA2 rows. `XBOX_NO_FUSED_LEGACY_MAD=1` restores the gfx1010 rounding of the product before the add (RX 5700 XT, boykopovar/AnyPS5#1281 and #1290). Both give a +0 product for a zero factor and flush f32 denormal sources and results (gfx1010 also the product) to a zero of the same sign in every mode, an exception to [f32DenormalFlushFor](../../../core/shader/recompiler/Translation/src/VectorInstructions.cpp).
