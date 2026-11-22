# TechnicalDebt

## add Unknown function info

- [XMemAlloc and XMemFree](../../tools/xbox/platform/xmem.cpp) log an ERA interpretation, not a hardware-verified Scarlett layout. Sources: [SlimEra era.h at 912bb112](https://github.com/XWine1/SlimEra/blob/912bb1123e84f32023af6685989ac5d6747d767a/era.h#L115-L149) and [WinDurango kernelx.h at ae747f40](https://github.com/WinDurango/WinDurango/blob/ae747f406395581f1faafc5da8f03d0b701d2705/projects/WinDurango.KernelX/include/WinDurango.KernelX/kernelx.h#L158-L191). Fields occupy bits 0-13 (object type), 14-15 (page size), 16-23 (allocator ID), 24-28 (alignment exponent), 29-32 (memory type), and 33-63 (reserved). The trace prefixes fields with `era_` and preserves unknown encodings numerically. Closes when: console measurements or the Scarlett allocator establish the layout and meaning of its fields.

## add Functional

- [XMemAlloc and XMemFree](../../tools/xbox/platform/xmem.cpp) still ignore attributes when allocating and freeing: the stand-in uses zero-filled `VirtualAlloc` memory and `VirtualFree`. Decoding the trace does not implement allocator routing, GPU visibility, cache policy, physical pages or requested alignments above Windows' allocation granularity. Zero-size requests still allocate one byte. The working-set output and `XMemVirtualQuery` field at +0x30 remain guesses. Synthetic trace tests do not establish console behaviour.
