// D3D12XboxCreateDevice for the d3d12_xs.dll stand-in (build together with d3d12_xs.cpp: cl /LD /O2 /EHsc d3d12_xs.cpp d3d12_xs_device.cpp /Fe:d3d12_xs.dll).
// Kept in its own file because it needs d3d12.h, whose declarations clash with the exports of d3d12_xs.cpp.
//
// The first graphics call of an Xbox title. Signature from the public GDK documentation: (IGraphicsUnknown* adapter,
// const D3D12XBOX_CREATE_DEVICE_PARAMETERS* params, REFIID riid, void** device). The parameter struct (version, debug flags, command-queue ring and scratch
// sizes, ...) is read leniently: its first 96 bytes are logged (XS_LOG=<file>) and nothing in it is used. The device handed back is a PC device behind the
// Xbox-ABI proxy (xbox_device_overrides.h); riid is not checked. XS_TRACE=1 then logs the first call of every device slot the title uses.
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdarg>
#include "xbox_device_overrides.h"

static void LogLine(const char* fmt, ...) {
    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("XS_LOG", path, MAX_PATH)) return;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a")) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fclose(f);
}

extern "C" __declspec(dllexport) HRESULT WINAPI D3D12XboxCreateDevice(void* adapter, const void* params, const GUID& riid, void** out) {
    (void)adapter; (void)riid;
    if (!out) return E_POINTER;
    *out = nullptr;
    if (params) {
        LogLine("D3D12XboxCreateDevice: params words:");
        for (int i = 0; i < 24; i++) LogLine(" %08x", ((const unsigned*)params)[i]);
        LogLine("\n");
    }
    static HMODULE m = LoadLibraryW(L"d3d12.dll");
    auto f = m ? (PFN_D3D12_CREATE_DEVICE)GetProcAddress(m, "D3D12CreateDevice") : nullptr;
    ID3D12Device* dev = nullptr;
    HRESULT hr = f ? f(nullptr, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), (void**)&dev) : E_NOTIMPL;
    if (FAILED(hr) && f) hr = f(nullptr, D3D_FEATURE_LEVEL_11_1, __uuidof(ID3D12Device), (void**)&dev);
    if (FAILED(hr) && f) hr = f(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&dev);
    LogLine("D3D12XboxCreateDevice: PC device hr 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) return hr;
    *out = xbox_overrides::WrapDevice(dev);
    return S_OK;
}
