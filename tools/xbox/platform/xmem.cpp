// Stand-in for the Xbox GDK xmem.dll, covering every export the package's DLLs import:
//   Bink2Scarlett.dll            XMemAlloc, XMemFree
//   amd_fidelityfx_upscaler_xbox XMemAlloc, XMemFree, XMemGetWorkingSetStatistics
//   XboxBvhBuilder.dll           XMemVirtualQuery
// Signatures were taken from the callers' machine code (dumpbin /disasm), not from headers:
//   XMemAlloc(size, attributes)          rcx = size rounded up to 256, rdx = one global attribute word (0x5e50000 in Bink)
//   XMemFree(pointer, attributes)        same attribute word
//   XMemGetWorkingSetStatistics(kind, out)   kind = 0x20000001 in both call sites, out = a >= 16-byte struct whose qword at +8 the caller
//                                        uses as a size; non-zero return = success (the caller maps 0 to "no data")
//   XMemVirtualQuery(process, address, out)  out is a zeroed 64-byte struct that starts like MEMORY_BASIC_INFORMATION (State at +0x20 is tested
//                                        against MEM_COMMIT) plus a field at +0x30 whose low two bits the caller tests; non-zero = success
#include <windows.h>
#include <cstdio>
static void Log(const char* what, void* p, ULONGLONG a, ULONGLONG b, const ULONGLONG* attrs = nullptr) {
    char path[MAX_PATH];
    DWORD length = GetEnvironmentVariableA("XMEM_LOG", path, sizeof(path));
    if (!length || length >= sizeof(path)) return;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") == 0 && f) {
        fprintf(f, "%s ptr=%p a=0x%llx b=0x%llx", what, p, a, b);
        if (attrs) {
            ULONGLONG value = *attrs;
            fprintf(f, " era_object_type=0x%llx era_page_size=%llu era_allocator_id=0x%llx"
                       " era_alignment_log2=%llu era_memory_type=%llu era_reserved=0x%llx",
                    value & 0x3fff, (value >> 14) & 3, (value >> 16) & 0xff,
                    (value >> 24) & 0x1f, (value >> 29) & 0xf, value >> 33);
        }
        fputc('\n', f);
        fclose(f);
    }
}
extern "C" {
__declspec(dllexport) void* XMemAlloc(SIZE_T size, ULONGLONG attrs) {
    void* p = VirtualAlloc(nullptr, size ? size : 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    Log("alloc", p, size, attrs, &attrs);
    return p;
}
__declspec(dllexport) void XMemFree(void* p, ULONGLONG attrs) {
    Log("free ", p, 0, attrs, &attrs);
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}
__declspec(dllexport) BOOL XMemGetWorkingSetStatistics(DWORD kind, ULONGLONG* out) {
    MEMORYSTATUSEX m{sizeof(m)};
    if (!out || !GlobalMemoryStatusEx(&m)) return FALSE;
    out[0] = m.ullTotalPhys;     // guess: total
    out[1] = m.ullAvailPhys;     // the qword the upscaler reads (+8): treated here as the memory still available
    Log("wsstat", out, kind, out[1]);
    return TRUE;
}
__declspec(dllexport) BOOL XMemVirtualQuery(HANDLE process, const void* address, void* out) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!out || VirtualQueryEx(process ? process : GetCurrentProcess(), address, &mbi, sizeof(mbi)) == 0) return FALSE;
    memset(out, 0, 64);
    memcpy(out, &mbi, sizeof(mbi));                                   // BaseAddress, AllocationBase, protections, RegionSize, State at +0x20
    *reinterpret_cast<DWORD*>(static_cast<char*>(out) + 0x30) = (mbi.State == MEM_COMMIT) ? 3u : 0u;   // guess: bits 0-1 = readable / writable
    Log("vquery", const_cast<void*>(address), mbi.RegionSize, mbi.State);
    return TRUE;
}
}
