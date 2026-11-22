# Platform surface of the 16 DLLs in the package root (static imports only)
Tool: tools/xbox/platform/pe_imports.py (normal + delay-load imports; no delay-load imports were present). Per-function classification: the "platform" key of docs/xbox/results.json (make_platform_results.py).

| Category | DLLs | Functions |
|---|---|---|
| Standard Windows (resolves on this PC: every one of the 492 functions found with GetProcAddress) | 61 (55 api-sets + 6 others) | 492 |
| Provided by the package itself (msvcp140, vcruntime140, vcruntime140_1; every imported name is exported by the package copy) | 3 | 113 |
| Xbox-specific, no PC equivalent file | 2 (xmem.dll, d3d12_xs.dll) | 6 distinct |

Xbox-specific imports:
- xmem.dll: XMemAlloc, XMemFree (Bink, upscaler), XMemGetWorkingSetStatistics (upscaler), XMemVirtualQuery (XboxBvhBuilder)
- d3d12_xs.dll: D3D12SerializeVersionedRootSignature (XboxBvhBuilder), D3D12CreateRootSignatureDeserializer (upscaler); both exist with the same names in the PC d3d12.dll

Shims (tools/xbox/platform/xmem.cpp, d3d12_xs.cpp): all six DLLs that import something (XboxBvhBuilder, both FidelityFX, Bink, XCurl, libHttpClient.GDK) load on this PC with them; Bink decodes a real video (tools/xbox/platform/bink_test.cpp; the video was carved with carve_bink.py).
Argument shapes for the three less common xmem calls come from disassembling the callers; the meaning of the attribute bits, of the working-set struct and of the +0x30 field of XMemVirtualQuery is a guess.
Static imports only: this does not include what the DLLs call through COM/vtables (D3D12 device methods) or what the game executable imports.

## XboxBvhBuilder.dll on a PC device (tools/xbox/platform/bvh_test.cpp)
`bvh_test.exe` loads the DLL next to the stand-in `xmem.dll` / `d3d12_xs.dll`, creates a PC D3D12 device and constructs the
`XboxBvhBuilder(ID3D12Device11*, bool)` class through its exported constructor, handing it an Xbox-layout device stand-in
(`xbox_device_proxy.h`, see below). In order:
1. The first root signature carries flags `0x3000000`, bits the PC runtime does not define (its flags are all in the low 16 bits), so the PC
   serializer fails with `E_INVALIDARG`. `d3d12_xs.dll` clears the high byte on a copy of the descriptor (meaning of the bits unknown;
   `XS_KEEP_XBOX_FLAGS=1` forwards it unchanged, `XS_LOG=<file>` logs every serialize call).
2. The constructor then creates its 53 compute pipelines through device vtable slot 62, called as (description, 0, 0, interface id, output).
   On a PC device slot 62 is `CreateStateObject` (three arguments), so called directly it crashes inside the PC runtime. The stand-in
   maps slot 62 to `CreateComputePipelineState(description, iid, output)`; the description is a plain `D3D12_COMPUTE_PIPELINE_STATE_DESC`.
3. **WARP**: all 53 pipelines are created (`S_OK`, shader blobs from 4 KB to 30 KB), the constructor returns, and
   `GetRaytracingAccelerationStructurePrebuildInfo` answers for a 3,000-vertex triangle geometry: result 204,800 bytes, scratch 128,704
   (the PC driver's own answer for the same input: 115,984 / 124,560, so the layouts differ as expected). The destructor runs.
   **AMD Radeon RX 7800 XT**: the driver's shader compiler (`amdxc64.dll`) crashes on most of the builder's shaders. Compiling each of the 53
   blobs alone (`BVH_ONLY=n`): 9 compile, 39 crash the compiler (access violation inside `amdxc64.dll`), 5 fail with an error (one message:
   "Unknown pointer operation encountered during ParseAddress"); after a crash the device is removed, so they have to be run one per process.
   WARP creating the pipelines does not show that they compile: it may defer that.
   **NVIDIA GeForce RTX 5090** (driver 617.42, Windows 11): compiling each blob alone, 52 of 53 are accepted, including the 44 that call
   `@xbox.*` (acceptance may be deferred compilation, as on WARP). Blob 48 (`LeafRehydration`, the only one with `TypedLoad.i32`) ends the
   process with a fail-fast in the user-mode driver (`nvwgf2umx.dll`, `0xc0000409`), which `__try` cannot catch; in one full run it
   returned `E_OUTOFMEMORY` instead and the constructor finished. The full constructor therefore usually dies with no output after
   "constructing"; `BVH_ONLY` runs are the way to test the other blobs on NVIDIA.
   **Cause**: not a random driver bug. The shaders are Xbox DXIL: 44 of the 53 call non-standard functions `@xbox.*` (declared with opaque
   `i32` opcode first arguments, written by the Xbox DXC, `dxcxbox`), and those 44 are exactly the 44 that fail; the 9 that compile have
   none (`ResetCounterValues`, `CopyForBVHUpdate`, `ResetToIndexBuffer`, `CopyCounters`, `RadixReset`, `PT_ConstructBVH4`, `ResetKDOP`,
   `DecompressHeader`, `CopyAndCompactBVH`). The intrinsics are one-to-one wrappers of RDNA instructions: `global_load_dword/x2/x3/x4_with_cache`,
   `global_store_*`, `global_atomic_{add,sub,fmin,fmax}` and `s_load_dword_with_cache` (all with a 64-bit address operand), `Med3/Min3/Max3`
   (float and unsigned), `BFI`, `V_ALIGNBYTE_B32`, `V_CNDMASK_B32`, `V_PK_MIN/MAX_F16`, and lane operations (`ReadLane`, `LaneSwizzle`,
   `Ballot64`, `MBCNT64`, `S_BCNT1_U64`, `GetLaneID`, `WaveMin_U32`, `MakeUniform`, `TypedLoad`). 33 distinct names (the first `i32`
   argument looks like an instruction number, e.g. 356 for the dword global load). `tools/xbox/platform/bvh_shader_scan.py` classifies the
   shaders, `docs/xbox/bvh_builder_shaders.json` has the per-shader result:
   | group | shaders |
   |---|---:|
   | standard DXIL only (compile) | 9 |
   | needs raw 64-bit address access (global load/store/atomic) | 31 |
   | wave/lane intrinsics, no pointers (RadixCount, PrefixSum*, RadixAlloc) | 9 |
   | arithmetic intrinsics only (ConstructBVH4, BuildApexPointMap, Box16/32Rehydration) | 4 |
   So a PC path needs the 13 pointer-free shaders' intrinsics lowered to standard DXIL (all have direct equivalents: wave ops, min/max, bitfield
   insert, funnel shift), and for the 31 others an address-to-resource mapping for the 64-bit addresses, which the Xbox2Windows-style
   translation or our own RDNA-aware path would have to provide. The ISA semantics of these intrinsics are the ones the SPIR-V recompiler
   already implements for the same instructions.
**Lowering the intrinsics (tools/xbox/platform/xbox_dxil_lower.py + dxasm.cpp)**: `xbox_dxil_lower.py` rewrites the disassembled DXIL
(`dxc -dumpbin`) so it calls standard operations instead, and `dxasm.exe` reassembles it with DXC's assembler API and signs it with
`dxil.dll`. Needed besides the replacements: the Xbox target triple `dxil-ms-xb2` and the `xbox.compiler.options` metadata are rejected by the
standard validator; the Xbox validator allows `{ half, half }` aggregates, standard DXIL does not (every pair value is split into two scalars);
and unnamed SSA values and blocks must be renumbered after instructions are removed (number 0 is the entry block).
Done so far: `V_PK_MIN_F16` / `V_PK_MAX_F16` -> `dx.op.binary.f16` on each half, `Med3_F32` -> `max(min(a,b), min(max(a,b),c))`.
That covers the four arithmetic-only shaders, and on the RX 7800 XT (one shader per process, original / lowered):
| shader | original | lowered |
|---|---|---|
| Box16Rehydration | compiler crash | compiles |
| Box32Rehydration | compiler crash | compiles |
| ConstructBVH4 | compiler crash | compiles |
| BuildApexPointMap | error | compiles |
"Compiles" means the driver accepted the pipeline; nothing has been dispatched, so the lowered shaders are not yet checked for correct results.
**First dispatch of a lowered shader (tools/xbox/platform/dispatch_test.cpp)**: Box32Rehydration (lowered) was run with pseudo-random input
(words whose high byte is a count 0..4 and whose low 24 bits are a random mask), on the RX 7800 XT and on WARP, through bounds-checked raw UAVs.
- It runs to completion. On the GPU the output is identical across repeated runs for 1 and 4 groups, and identical whether the wave size is left to the driver or
  pinned to 64 (`dxil_sm66_wave.py`: SM 6.6 `[WaveSize(64)]`; it also converts `createHandle` to `createHandleFromBinding`/`annotateHandle`, which SM 6.6 requires).
  With 64 groups the output differs run to run: random pointers make several threads write the same node, a race in my input, not in the shader.
- WARP gives a different result (6429 vs 6552 non-zero words for 4 groups), and WARP rejects SM 6.6, so it cannot be used to check the pinned version.
- One earlier run hung the GPU (device removed). It followed runs that used root UAVs, which have no bounds checking, so I attribute it to that and not to the shader;
  it did not recur once the views were bounds-checked.
- **The lowered operations themselves are checked.** `tools/xbox/platform/oplower_kernel.ll` is a small Xbox-style kernel that applies `Med3_F32`, `V_PK_MIN_F16` and `V_PK_MAX_F16` to
  random bit patterns; it goes through `xbox_dxil_lower.py` and `dxasm`, runs with `dispatch_test` (`DT_RAW=1 DT_DUMP=<prefix>`), and `oplower_check.py` compares every result with a
  software model (min/max return the other operand when one is NaN; sign of zero and denormals are not distinguished). On 262,144 cases the RX 7800 XT and WARP both match exactly (0 mismatches, NaN and infinity included).
  That is the whole of what the lowering changed in the four arithmetic-only shaders, so what remains unchecked is the shaders' own logic, which no reference exists for.
- This shows the lowered shader is stable, not that it is correct: the original cannot run anywhere, so there is no reference output.
  A CPU model of the rehydration (from the shader text and `BVH_CONTAINER.md`) is the next way to check it.

**More intrinsics lowered; 22 of the 53 builder shaders now compile on the GPU (`lower_all.py`)**: `lower_all.py <dir with shader_NN.ll> <out_dir>` lowers, pins the wave
size to 64 where wave operations are used (`dxil_sm66_wave.py`), assembles, signs and repairs the shader-flags word. Added to the table: lane id, readfirstlane, readlane,
`ds_swizzle` (bitmask and quad modes), ballot, `v_cndmask_b32`, `mbcnt`, `s_bcnt1`, min3/max3/med3 (f32 and u32), `v_bfi`, `v_alignbyte`, `WaveMin_U32`, 64-bit `MakeUniform`.
All 22 (the 13 pointer-free shaders of the earlier count plus the nine wave/lane ones) are accepted as pipelines by the RX 7800 XT.
The lane intrinsics were run (`wavelower_kernel.ll` + `wavelower_check.py`, wave size 64, 64 groups x 64 lanes) and match a software model with 0 mismatches;
the model uses the same formulas as the lowering, so this checks the plumbing. The one argument order that is inferred and not documented is `V_CNDMASK_B32(mask pair, {src0, src1})`:
it is what the Hillis-Steele prefix sums in these shaders (masks `0xAAAAAAAA`/`0xCCCCCCCC`/`0xF0F0F0F0` with swizzle offsets 30/60/120) require.
The other 31 shaders use `global_load/store/atomic` and `s_load` on 64-bit addresses (plus one `TypedLoad`). Those addresses are not derived inside the shader:
they are 64-bit fields read from buffers (for example `rawBufferLoad.i64` at offset 24 of an input record), so they are GPU virtual addresses that the game obtained from the device.
Design for them: the proxy records every resource's (virtual address, size, descriptor) when it is created, the lowered shader looks the address up in that table
(range search in a constant table) and loads through `ResourceDescriptorHeap[i]` (SM 6.6) at `address - base`. It needs the real addresses, so it waits for the game or a captured input;
raw buffers are limited to 4 GiB per view.
The shader side is done (`xbox_dxil_lower.py`, table format in its docstring): with it `lower_all.py` assembles and signs all 53 builder shaders
(22 before; those 22 are byte-identical). `address_kernel.ll` + `address_check.py` exercise every lowered form (1-4 dword loads, `s_load`,
1/3/4 dword stores, `add`/`sub`/`fmin`/`fmax` atomics) through `dispatch_test`'s heap mode (`DT_TABLE=<table file>`) at made-up addresses,
one range crossing 4 GiB, two decoy entries in a 4096-entry table: 0 mismatches over 1024 and 16384 threads and three seeds on the RTX 5090
(driver 617.42) and on WARP. WARP rejects a pinned `WaveSize(32)` (`E_INVALIDARG`), so that kernel runs unpinned (`dxil_sm66_wave.py ... 0`).
In `bvh_test` (RTX 5090, `BVH_REPLACE_DIR`) the 31 lowered shaders are rejected at pipeline creation (`E_INVALIDARG`), as expected: the
game's root signatures lack `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED`.

**The proxy side of the address table (`xbox_address_table.h`, used by `xbox_overrides::WrapDevice`)**:
- Recording: device slots 27 (`CreateCommittedResource`), 29 (`CreatePlacedResource`) and 76 (the Xbox resource creation) record every buffer
  that has `ALLOW_UNORDERED_ACCESS` at its `GetGPUVirtualAddress()` and width, and give it a raw UAV descriptor. Buffers in a default heap get
  that flag added (it does not change a buffer's layout; `XS_NO_BUFFER_UAV=1` turns this off). Upload and readback buffers cannot have a UAV,
  so they are not recorded and an address inside one reads 0. A private-data interface attached to each recorded buffer is released by the
  runtime when the buffer is destroyed, which removes its range and frees its descriptor slot.
- The table: a 64 KiB buffer in a custom heap (write-combined, `L0`, UAV allowed), persistently mapped and rewritten from a sorted list on every
  change, in the format the lowering reads: header (used entries, 4096, reserved slots, 0), then 4096 entries. Entry 0 is address 0 -> the null slot, and
  every range's end gets an entry -> the null slot unless the next range starts there, so an address in a gap, below the first range or past the
  last one reaches a null raw UAV (reads 0, writes are dropped) instead of the neighbouring buffer at a large offset. Ranges inside another range
  are cut at the outer range's end; at one base the newer buffer wins. More than 4096 entries: the rest are dropped (counted).
- How the shader finds it: every shader-visible CBV/SRV/UAV heap created through slot 14 is created 4096 descriptors larger, and the proxy heap's
  `GetDesc`, `GetCPUDescriptorHandleForHeapStart` and `GetGPUDescriptorHandleForHeapStart` (slots 8-10) report the requested size and a start
  4096 descriptors in, so the game never sees the reserved part. Descriptor 0 is the table (the lowering's default `ResourceDescriptorHeap[0]`,
  so the lowering is unchanged), 1 is the null raw UAV and 2-4095 are the recorded buffers. They are written into every live shader-visible heap
  (CPU writes to a shader-visible heap are allowed). No root constant is needed: whichever heap the game binds has the same layout at 0..4095.
- Root signatures: slot 16 (`CreateRootSignature`) deserializes the blob, adds `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED`, and reserializes it at its own
  version (1.0 is converted to 1.1 if the runtime refuses the flag at 1.0); if anything fails the original blob is used.
- Off when the device lacks resource binding tier 3 or shader model 6.6, or with `XS_NO_ADDRESS_TABLE=1`.

Checks (RTX 5090, driver 617.42, and WARP):
- `proxy_address_test.exe` (no arguments: the table builder and a software copy of the shader's binary search; with a kernel: the device side)
  creates heaps, buffers (committed, placed, upload) and a root signature through the proxy, releases one buffer and checks that its slot is reused,
  and that the lowered kernel is rejected with the unpatched signature and accepted with the proxy's. `proxy_address_kernel.ll` is `address_kernel.ll`
  with the two buffer addresses read from a constant buffer (real GPU addresses are not known in advance). With
  `PT_DUMP=<prefix>`, `address_check.py check` compares the result: 0 mismatches at 1,024 and 16,384 threads on both adapters, atomics included.
- `bvh_test` (it now wraps the device with `WrapDevice`; `BVH_PLAIN_PROXY=1` uses the bare proxy), `BVH_REPLACE_DIR` = `lower_all.py` output:
  failures go from 42 to 9 on the RTX 5090. The heap flag was not the only cause: `lower_all.py` pinned the wave size to 64 for every shader that
  indexes the heap, and the RTX 5090 rejects `WaveSize(64)`. It now pins only shaders whose result can depend on the wave size
  (`xbox_dxil_lower.needs_wave64`): the 10 address-using shaders with wave operations (00, 04, 31-36, 42, 44) and 2 pointer-free ones (21, 22) run
  groups of 32 threads, or use only swizzles that stay inside 32 lanes, so they are left unpinned and created. The 9 still rejected need 64-lane waves:
  20 and 28 (ballots, `MakeUniform`), 23-27 (a scan that reads lane 31 into lanes 32-63) and 49, 50 (standard wave operations on 64-thread groups).
  Without the proxy the 42 failures stay.
- Not checked: any real build or rehydrate. The builder's constructor creates no buffers (0 ranges recorded), and the build calls need the
  game's geometry and the matching vertex/index buffers.
- Known limits: the table and the descriptors are rewritten in place, so a buffer created or released while a builder dispatch is in flight can race with it
  (a fresh table per command list would fix it); buffers created through `CreateCommittedResource1/2/3` or `CreatePlacedResource1/2` are not recorded
  (the builder does not call them); descriptor heaps the game passes to a PC command list are still the proxy objects (the `SetDescriptorHeaps` side is not done);
  a raw view covers at most 4 GiB of a buffer.

**`TypedLoad.i32` and LeafRehydration (blob 48) on NVIDIA**: blob 48 is the only `TypedLoad.i32` user, with two calls
`TypedLoad.i32(i32 138, handle u0 = compressedBVH (RWByteAddressBuffer), i32 offset, i32 65)`; only component 0 of the `ResRet.i32` is used.
- Format 65 read as the GFX6-9 MTBUF encoding `dfmt | nfmt << 4` = `DFMT_8 | NFMT_UINT << 4` (LLVM `llvm/lib/Target/AMDGPU/SIDefines.h`, `MTBUFFormat`),
  an 8-bit unsigned load. Inferred, not documented: in the GFX10 unified table (`UfmtGFX10`) 65 is `16_16_16_16_UNORM`, a float format that the
  shader's use does not fit. Checked against the shader: the offsets are byte offsets (`base + 3 * count + i`, right after a packed array of
  24-bit indices that the shader itself reads with `rawBufferLoad.i64` at `offset & ~3`), and the results are used only as bytes
  (`(index24 << 8) | b`; `(b & 0xF) | ((b << 4) & 0xF00)`).
- Lowered (`xbox_dxil_lower.py`) to `rawBufferLoad.i32` at `offset & ~3`, shifted right by `8 * (offset & 3)`, `& 0xFF`. A byte in the last,
  partial dword of a buffer whose size is not a multiple of 4 reads as 0 (the dword is out of bounds); the shader's own unaligned loads do the same.
  `typedload_kernel.ll` + `typedload_check.py` (4096 threads, offsets `t` and `5t + 3` over random bytes): 0 mismatches on the RTX 5090 and on WARP.
- Blob 48 still calls `global_load_dword{,x2,x3,x4}` (64-bit addresses: index and vertex buffers read from the `D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC`
  records), so `dxil.dll` does not validate it. `dxasm --hash-only` assembles it without validation and writes the container hash: the retail
  hash of DXC's `lib/DxilHash/DxilHash.cpp` (`ComputeHashRetail`, MD5 rounds with the bit count in the first dword and `1 | size << 1` in the last
  dword of the final block), reimplemented in `dxil_hash.h`; it reproduces the hash of all 53 original blobs and of `dxil.dll`-signed ones.
  The PC runtime rejects a container with a zeroed hash (`E_INVALIDARG`).
- RTX 5090, driver 617.42, `BVH_ONLY=48`: the original blob, and the same text reassembled unchanged, make the driver's compiler run out of memory
  (private bytes seen at 24 and 51 GB of a 65 GB commit limit; `E_OUTOFMEMORY` after about 90 s, or the fail-fast `0xC0000409`).
  With only the two `TypedLoad.i32` calls lowered (everything else, triple included, as in the original) the pipeline is created in under a second
  (about 100 MB), and also with the whole `xbox_dxil_lower.py` output (`V_ALIGNBYTE_B32` lowered too); replacing the two calls with an aligned dword
  load at offset 0 has the same effect, so the two typed byte loads are what the driver cannot handle.
- Full constructor with only blob 48 replaced: 53 of 53 pipelines, no failures, prebuild 204800/128704, destructor ok (3 runs).
  This is pipeline creation only: the `global_load_*` calls are still Xbox intrinsics, so blob 48 cannot run until the 64-bit address mapping exists.
- Not caused by this: with the 22 `lower_all.py` replacements, the 11 that pin the wave size to 64 (20 to 28, 49, 50) are rejected with
  `E_INVALIDARG` on the RTX 5090, which has no 64-lane waves; the 11 others are accepted.
Not reached: any build or rehydrate call. They take GPU virtual addresses, which the builder's DXIL shaders read by 64-bit address (no PC
D3D12 equivalent), and real inputs would also need the matching vertex/index buffers (BVH_CONTAINER.md).

## FidelityFX upscaler, unmodified, on a PC device (tools/xbox/platform/ffx_test.cpp)
The package's own `amd_fidelityfx_loader_xbox.dll` and `amd_fidelityfx_upscaler_xbox.dll` run as they are (they are ordinary x64 PE files;
nothing is recompiled), next to the stand-in `xmem.dll` / `d3d12_xs.dll`:
* `ffxQuery` (version enumeration, null context): one upscaler, id `0xf5a5ca1e01001001`, name `4.1.1`.
* `ffxCreateContext` for the upscale effect (type `0x10000`, backend extension type `0x01000002` carrying the device), 1280x720 to 1920x1080,
  returns 0 and `ffxDestroyContext` returns 0, on the RX 7800 XT and on WARP, debug layer: 0 errors, 3 warnings (buffers ignore the initial state).
* The create flags must be `0x29`: the descriptor validator (RVA `0xbb10`) fails with 6 unless bits 0x1, 0x8 and 0x20 are set and rejects
  0x2, 0x4, 0x40, 0x100 and 0x200 as unsupported.
* The provider creates 14 resources: 10 default-heap ones through device slot 76 (1920x1080 RGBA8, two RGBA16F, one RGB9E5, an 8-byte
  buffer, 1x1 RG32F, 1x1 R32_UINT, 80x45 R32F, a 21,045,760-byte buffer, a 131,072-byte buffer) and 4 upload buffers through slot 27.
Where the Xbox device differs from the PC one, as found by running it:
| Xbox call | PC | handling |
|---|---|---|
| device slot 76 (resource creation, heap properties / heap flags / description / state / clear value / iid / output) | slot 27 is CreateCommittedResource; slot 76 is something else | forwarded to CreateCommittedResource; the description is 56 bytes plus extension words and its layout field is `0x101` (tiled) or `0x1` (row-major buffer), replaced by driver-chosen layout |
| descriptor heap `GetCPU/GPUDescriptorHandleForHeapStart` (slots 9, 10) | returns through a hidden pointer in RDX | the Xbox build returns the 8-byte handle in RAX; heaps are wrapped so the call is made the PC way |
| slots 1, 15, 18, 19, 27, 38 on the device | same | forwarded unchanged (AddRef, descriptor increment size, SRV, UAV, committed resource for uploads, copyable footprints); 18 and 19 see the RGB9E5 note below |
**NVIDIA**: the fourth slot-76 resource is a 1920x1080 `R9G9B9E5_SHAREDEXP` texture with `ALLOW_UNORDERED_ACCESS`. The RTX 5090 (driver 617.42)
does not report `TYPED_UNORDERED_ACCESS_VIEW` for that format (WARP does), so `CreateCommittedResource` returns `E_INVALIDARG` and the
upscaler throws a C++ exception out of `ffxCreateContext`. `xbox_device_overrides.h` creates such a resource as `R16G16B16A16_FLOAT` when
the device lacks the support (every RGB9E5 value is exact in f16; writes are not rounded to RGB9E5 and the memory layout differs) and
rewrites RGB9E5 views of it in slots 18/19; `XS_NO_FORMAT_FALLBACK=1` turns it off. With it, flags `0x29` create and destroy return 0 on
the RTX 5090; the upscaler builds its views of that resource from the resource's own format (f16), so the view rewrite is not reached
here. Nothing was dispatched, so whether the upscaler's output is unchanged by the substitution is not checked.
The first-chance `0xe06d7363` that `ffx_test` prints during `D3D12CreateDevice` also appears on WARP and is caught inside `D3D12Core`.
This is the same finding the Xbox2Windows project reports (slot 76, layout `0x101`), reached here by running the code instead of translating it.
No dispatch was run: it needs real textures, a command list and the package's shaders.

`xbox_device_proxy.h` is the reusable part: an object whose vtable forwards every slot to the PC object by generated thunks (with per-slot hit
counters, `PrintHits`) and lets selected slots be replaced. It is the starting point for the "D3D12.X runtime" row.

## Compared with the AnyXboxX stand-ins (read only; that repository has no licence, nothing was copied)
- `XMemVirtualQuery`: AnyXboxX assumes a 3-argument `VirtualQuery(address, info, size)`. The call sites in XboxBvhBuilder pass (process, address, out) with a zeroed 64-byte
  struct, which is what `xmem.cpp` here implements; the first call there would be wrong with the other signature.
- `XMemGetWorkingSetStatistics`: they return FALSE (no data). The FidelityFX upscaler reads a size at +8 of the struct and maps a zero return to "no data"; ours returns plausible values (untested whether the upscaler prefers them).
- `XMemAlloc`: they align to 64 bytes or a page and track blocks so that allocation hooks (`XMemSetAllocationHooks`) can coexist; ours uses `VirtualAlloc` (64 KiB granularity, simplest, wasteful for many small blocks). Hooks are not imported by the package's DLLs, so not needed yet.
- `d3d12_xs`: they forward four root-signature functions unchanged. Unchanged forwarding fails for Xbox root signatures (the PC serializer rejects the high flag byte; see above), so ours clears it on a copy.
  Added here: `D3D12SerializeRootSignature` (v1.0 layout, flags at +32; a descriptor with flags 0x3000001 serializes through the shim and fails with E_INVALIDARG on the PC runtime) and `D3D12CreateVersionedRootSignatureDeserializer`.
- They have no D3D12.X device layer (listed as to do), no shader recompiler, and no BVH or upscaler tests; they have a much larger `xgameruntime` (152 of 396 APIs) and a kernel redirect (`kernelx.dll`).

## D3D12XboxCreateDevice (first graphics call of a title)
`d3d12_xs_device.cpp` exports it: `(adapter, params, riid, out)` returns a PC `ID3D12Device` behind the Xbox-ABI proxy, with slot 14 (descriptor heaps, whose start handles come back in RAX) and slot 76
(resource creation with the Xbox texture layout) overridden. Checked with a client that does what the package DLLs do: create the device, create a descriptor heap through slot 14, read the heap start through slot 9/10 (non-zero 8-byte values in RAX).
`wine_d3d12.sh` runs this creation path under Wine (upstream vkd3d offers feature level 11_1, so the stand-in falls back from 12_0), then clears a texture, copies it to a readback buffer and compares the words on the underlying PC device, because the queue and fence slots on the Xbox device are not known.
The signature and the `params` struct are from memory of the public documentation, not from a header: the struct is only logged (24 words), so a wrong layout cannot break creation, but its contents (command-queue ring size, scratch memory, debug flags) are unused.
Not covered: presentation (`PresentX` on the queue, plane parameters), the command-list extensions, queue and fence creation (their slot numbers on the Xbox device are unknown beyond the ones listed above); `XS_TRACE=1` lists what a title calls.

