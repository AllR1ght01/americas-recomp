# tools/xbox/platform

What the package's DLLs need from the platform, and stand-ins for the Xbox-only parts. Result summary: `docs/xbox/PLATFORM_SURFACE.md`.

* `pe_imports.py <dir> <out.json>` lists the static imports (normal and delay-load) and exports of every PE file under a directory.
  Exports include names and `#<ordinal>` entries for nonzero export-address-table slots, including named aliases and forwarders.
  Empty slots are excluded. The ordinal is the export directory's ordinal base plus the slot index, as specified by the
  [PE export address table](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format#export-address-table).
* `make_platform_results.py <imports.json>` classifies each imported function (standard Windows / shipped in the package / needs a shim) and
  stores it under `"platform"` in `docs/xbox/results.json` for the progress map. Windows only.
* `d3d12_xs_device.cpp` (build with `d3d12_xs.cpp`) adds `D3D12XboxCreateDevice`: a PC device behind the Xbox-ABI proxy with the deviations learned so far (`xbox_device_overrides.h`: descriptor heap handles in RAX, resource creation at slot 76). The parameter struct is only logged. On a device without typed-UAV support for `R9G9B9E5_SHAREDEXP` (NVIDIA), slot 76 creates a UAV resource of that format as `R16G16B16A16_FLOAT` (every RGB9E5 value is exact in f16; writes are not rounded to RGB9E5 and the memory layout differs) and slots 18/19 rewrite RGB9E5 views of it; `XS_NO_FORMAT_FALLBACK=1` keeps the original format. `XS_LOG` and `XS_TRACE=1` show what a title does with it.
* `d3d12_xs_test.cpp` and `wine_d3d12.sh [work dir]` create a device through `D3D12XboxCreateDevice` under Wine, clear a
  4x4 `R32G32B32A32_FLOAT` target to known words, copy it to a readback buffer and compare, and write `report.json`.
  It exits 0 with `skipped: <reason>` when Wine, mingw-w64 or a D3D12 device is missing. Wine's DXGI reports a placeholder
  adapter, so the physical GPU in the report comes from `vulkaninfo`; the stand-in falls back from feature level 12_0 to
  11_1/11_0 because upstream vkd3d offers 11_1.
* `xmem.cpp` is a stand-in `xmem.dll` (XMemAlloc, XMemFree, XMemGetWorkingSetStatistics, XMemVirtualQuery); `d3d12_xs.cpp` forwards the two
  `d3d12_xs.dll` functions the DLLs import to the PC `d3d12.dll`. Build each with `cl /LD /O2 file.cpp /Fe:name.dll`. Argument shapes were read from the callers' machine code;
  the allocation semantics and two output fields remain unverified. `XMEM_LOG=<file>` logs every call.
  Allocation and free records retain the raw `a` and `b` values and append `era_object_type`, `era_page_size`, `era_allocator_id`,
  `era_alignment_log2`, `era_memory_type` and `era_reserved`. These fields interpret `b` using the public ERA bit layout;
  page size and memory type are numeric codes, and alignment is an exponent. Unknown values are printed without rejection.
  This is diagnostic decoding; the allocator still uses `VirtualAlloc` for all attribute words. Sources and limitations are recorded
  in the `XMemAlloc and XMemFree` technical-debt entries.
* `bvh_test.cpp` and `ffx_test.cpp` run the package's BVH builder and FidelityFX DLLs on a PC D3D12 device behind `xbox_device_proxy.h` (what works and where it stops: PLATFORM_SURFACE.md).
* `bink_test.cpp` loads `Bink2Scarlett.dll` next to a stand-in `xmem.dll`, decodes every frame of a video and saves two frames as raw BGRA.
  `carve_bink.py <movies.rpf> <out_dir> [count]` extracts Bink videos from the archive (they are stored raw).
  `wine_smoke.sh <package dir> [work dir]` runs the same test on Linux under Wine: it builds `xmem.dll` and
  `bink_test.exe` with mingw-w64, copies the package's DLLs, carves one video and fails if any frame errors. Needs
  Wine, mingw-w64 and the user's package, so it is not part of the CI tests. The `WINEDLLOVERRIDES` force native
  `xmem`/`d3d12_xs`, so a missing DLL fails loudly instead of falling back; the Bink test loads `xmem` only (a
  one-off `WINEDEBUG=+loaddll` run shows no `d3d12_xs`), and a synthetic D3D12.X caller is the next step for the
  layer itself.

* `xgr/idl_table.py <dir of IDLs> <table.json>` reads the interface definitions of another xgameruntime reimplementation (Xodus, LGPL-2.1; not part of this repo) and
  `xgr/gen_stub.py <table.json> <out_dir>` generates a logging stand-in `xgameruntime.dll` from it (60 interfaces, 1505 vtable slots). With `XGR_LOG=<file>` it records which
  interfaces a game queries and which methods it calls first, and logs interface ids missing from the table (where the console build differs from the PC one).
  Neither the IDLs nor the generated table are committed.

## First run with the game executable
1. `python exe_report.py <exe or directory> --out report --stubs` prints header facts (entry point, subsystem, sections) and classifies every
   static import: standard Windows, shipped in the package, covered by the stand-ins here, or *needs* an implementation. `report/needs.json`
   lists the last group; `report/stubs/` holds generated logging stand-in DLLs for it (`build.bat` from an x64 VS prompt).
2. Run the game with the stubs beside it and `STUB_LOG=<file>`: every call is logged as `dll!name(rcx, rdx, r8, r9) tid=..`
   (`STUB_ABORT=<text>` stops at the first call whose name contains the text). That trace says which functions are called and in which order.
   Check on the known case: a generated `xmem.dll` stub in front of `Bink2Scarlett.dll` logged `xmem!XMemAlloc(0x3c20, 0x5e50000, ..)`,
   the same size and attribute word found by disassembly.
3. Replace stubs with real implementations as behaviour becomes known (`xmem.cpp`, `d3d12_xs.cpp` are the pattern).
Static imports only: calls made through GetProcAddress, COM or vtables do not show up in step 1.

## Shader and BVH side, once the game runs
* Capture: put `xbox_device_proxy.h` in front of the device the game creates; `Slot62` (pipeline creation) in `bvh_test.cpp` shows how to dump every compute shader
  (`BVH_DUMP_DIR`). Slots seen so far are 1, 14, 15, 16, 18, 19, 27, 38, 62, 76; per-slot hit counters (`PrintHits`) show which others the game uses.
* Lower and sign: `dxc -dumpbin` -> `xbox_dxil_lower.py in.ll out.ll` -> `dxasm in.ll out.dxil` (replaces `@xbox.*` intrinsics; handles min/max f16 and med3 so far;
  it reports the intrinsics it could not replace). `dxil_sm66_wave.py` pins the wave size when a shader depends on 64 lanes.
* Try: `bvh_test.exe` with `BVH_REPLACE_DIR=<dir of repl_NN.dxil>` for pipeline creation; `dispatch_test.exe shader.dxil [groups] [seed]` for a dispatch on GPU and WARP.
* `lower_all.py <dir> <out>` runs the whole chain on a directory of disassembled shaders (all 53 builder shaders assemble; the wave size is pinned only where the result can depend on it, `xbox_dxil_lower.needs_wave64`). `oplower_check.py`, `wavelower_check.py` and `typedload_check.py` check lowered operations against software models.
* `wave64_emulate.py <in.ll> <out.ll>` replaces 64-lane wave operations with group-shared memory and a group barrier, so a shader that would
  be pinned to `[WaveSize(64)]` runs unpinned on a 32-lane device; `lower_all.py ... --emulate` tries it first. `wave64_emulate_kernel.ll`
  with `wave64_emulate_check.py`, and `wave64_prefix_kernel.ll` with `wave64_prefix_check.py`, check it against software models
  (64 groups x 64 lanes, GPU and WARP); the commands are under [Synthetic wave64 kernels](#synthetic-wave64-kernels).
* `dxasm --hash-only in.ll out.dxil` assembles without validating and writes only the container hash (`dxil_hash.h`), so a partly lowered shader that still calls `@xbox.*` intrinsics can be given to a PC driver.
* `XS_TRACE=1` makes every proxy log the first call of each forwarded slot (argument registers, caller): one run shows which Xbox-only slots a program uses.
* 64-bit addresses: `xbox_address_table.h` is the device side of the lowering's address table (recorded buffers, reserved heap descriptors, the heap
  indexing root-signature flag); `xbox_overrides::WrapDevice` installs it. `proxy_address_test.exe [kernel.dxil [groups] [warp]]` checks it:
  build `proxy_address_kernel.ll` with `xbox_dxil_lower.py`, `dxil_sm66_wave.py ... 0` and `dxasm`, run with `PT_DUMP=<prefix>`, then
  `address_check.py check <prefix>_gpu_u0.bin <prefix>_gpu_u1.bin <threads>`. Build: `cl /nologo /EHsc /O2 /std:c++17 proxy_address_test.cpp d3d12.lib dxgi.lib`.

No game files are part of this repository: the DLLs, the videos and the decoded frames stay local.

## Synthetic wave64 kernels

Emulation removes the native wave operations and can add 64-bit ones, so the shader flags left in the metadata no longer match what
the shader uses. Assembling the output of `wave64_emulate.py` directly with `dxasm` fails with `Flags must match usage`
(`declared=524304`, `actual=1048592` for `wave64_emulate_kernel.ll`, `actual=16` for `wave64_prefix_kernel.ll`). `lower_all.py`
writes the reported value into the flags word and assembles once more; a second failure is reported, not retried.

From an x64 Visual Studio developer PowerShell at the repository root. `$dxc` is any folder with the x64 `dxcompiler.dll` and
`dxil.dll`, from DXC or the Windows SDK:

```powershell
$dxc = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64'
$src = (Resolve-Path tools/xbox/platform).Path
$work = Join-Path $env:TEMP ('xbox-wave64-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path "$work/input" | Out-Null
Push-Location $work
Copy-Item "$dxc/dxcompiler.dll", "$dxc/dxil.dll" .
cl /nologo /EHsc /O2 /std:c++17 "$src/dxasm.cpp" /Fe:dxasm.exe
cl /nologo /EHsc /O2 /std:c++17 "$src/dispatch_test.cpp" /Fe:dispatch_test.exe d3d12.lib dxgi.lib
Copy-Item "$src/wave64_emulate_kernel.ll" input/shader_01.ll
Copy-Item "$src/wave64_prefix_kernel.ll" input/shader_02.ll
python -I "$src/lower_all.py" input out 64 "$work/dxasm.exe" --emulate
```

It prints `01 emulated; ok`, `02 emulated; ok` and `2 of 2 assembled and signed`. Then run both kernels on the GPU and WARP and
check each readback against its software model:

```powershell
$env:DT_RAW = '1'
$kernels = [ordered]@{ '01' = 'wave64_emulate'; '02' = 'wave64_prefix' }
foreach ($id in $kernels.Keys) {
    $name = $kernels[$id]
    $env:DT_DUMP = $name
    ./dispatch_test.exe "out/repl_$id.dxil" 64 1
    foreach ($adapter in 'gpu', 'warp') {
        python -I "$src/$($name)_check.py" "$($name)_$($adapter)_u0.bin" "$($name)_$($adapter)_u1.bin" 64
    }
}
Remove-Item Env:DT_RAW, Env:DT_DUMP
Pop-Location
```

Each dispatch prints `0 of 2097152 output words differ between the adapters` and each of the four checks prints
`64 groups x 64 lanes checked; mismatches: none`.

## XMem trace test

From an x64 Visual Studio prompt, build the stand-in with `cl /LD /O2 xmem.cpp /Fe:xmem.dll`.
Run `python xmem_test.py --dll <path-to-xmem.dll>` with 64-bit Windows Python. The test uses synthetic calls and temporary log files;
it checks both reported attribute words, field boundaries including bit 32, reserved bits, unknown encodings, allocation/free records,
non-allocation records and disabled logging. No game files or GPU are needed.
